// Does an H.264 stream survive being handed to a different encoder mid-file?
//
// GPU TIER.
//
// SPEC.md §5.4 step 6 says the migration procedure must "Reuse the SAME muxer, SAME
// stream index, SAME timebase", and then adds the caveat that decides whether that is
// possible at all:
//
// > If the encoder's `extradata` (SPS/PPS) changes across the migration, the muxer
// > must handle it — for MP4 this means either matching the original parameter sets
// > or falling back to in-band SPS/PPS (`AVCC` → Annex-B-in-`avc1` is invalid, so
// > prefer forcing identical encoder settings; if impossible, close the segment and
// > start a new one, and log it loudly).
//
// "Prefer forcing identical encoder settings" is a hope, not a design, until someone
// checks whether identical settings actually produce identical parameter sets across
// two vendors' encoders. The container's `avcC` / `CodecPrivate` is written by
// `avformat_write_header` before the first frame and cannot be rewritten, so if the
// answer is no, §5.4's fallback is not optional — it is the only path, and row 11's
// implementation has to be built around it.
//
// This file answers that question on the reference rig and pins the answer, so the
// design decision is recorded as a measurement rather than an assumption. It asserts
// nothing about *which* answer is correct — it asserts that whatever we found is
// still true, so that a driver update that changes it fails here rather than in a
// user's migrated recording.

#include "core/capture/capture_frame.h"
#include "core/color/nv12_converter.h"
#include "core/encode/video_encoder.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/mux/muxer.h"
#include "core/timing/frame_pacer.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

class MigrationParameterSetTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("paramsets");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "paramsets00001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());
    }

    static void TearDownTestSuite() {
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Opens an encoder on `adapter` with the engine's standard settings and returns
    /// its `extradata` — the SPS/PPS the container would record.
    static std::vector<std::uint8_t> extradata_for(const AdapterInfo& adapter) {
        auto device = fc::test::device_for_adapter(adapter.id);
        if (!device.has_value()) {
            return {};
        }

        fc::encode::VideoEncoderSettings settings;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.fps = kFps;
        settings.encoder_name = adapter.encode.encoder_name;
        settings.pool_bind_flags = adapter.encode.nv12_pool_bind_flags;
        // Identical in every respect that the caller controls. This is exactly what
        // SPEC.md §5.4's "forcing identical encoder settings" would amount to.

        auto created = fc::encode::create_video_encoder(settings);
        if (!created.has_value()) {
            return {};
        }
        const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(created).value();
        if (!encoder->open(device.value().device(), settings).has_value()) {
            return {};
        }

        const AVCodecContext* codec = encoder->codec_context();
        if (codec == nullptr || codec->extradata == nullptr || codec->extradata_size <= 0) {
            return {};
        }
        return std::vector<std::uint8_t>{codec->extradata,
                                         codec->extradata + static_cast<std::size_t>(codec->extradata_size)};
    }

    static std::vector<const AdapterInfo*> encoding_adapters() {
        std::vector<const AdapterInfo*> found;
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software && adapter.can_encode()) {
                found.push_back(&adapter);
            }
        }
        return found;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> MigrationParameterSetTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> MigrationParameterSetTest::topology_;

/// `profile_idc` out of an `avcC` extradata blob.
///
/// The layout FFmpeg produces here is Annex-B (`00 00 00 01` start code) followed by
/// the SPS NAL, whose first byte is the NAL header and whose second byte is
/// `profile_idc`. Returns 0 when the blob is not shaped that way, so a format change
/// reads as a failure rather than as a wrong-but-plausible profile.
[[nodiscard]] unsigned profile_idc_of(std::span<const std::uint8_t> extradata) {
    for (std::size_t i = 0; i + 5 < extradata.size(); ++i) {
        const bool start_code =
            extradata[i] == 0x00 && extradata[i + 1] == 0x00 && extradata[i + 2] == 0x00 && extradata[i + 3] == 0x01;
        if (!start_code) {
            continue;
        }
        // NAL unit type 7 is the SPS; `profile_idc` is the byte after the header.
        if ((extradata[i + 4] & 0x1Fu) == 7u) {
            return extradata[i + 5];
        }
    }
    return 0;
}

/// SPEC.md §9: "H.264 High @ 4.2".
constexpr unsigned kProfileIdcHigh = 100; // 0x64

// ---------------------------------------------------------------------------

// BUG-028. SPEC.md §9 fixes the profile at High, and until this assertion existed
// nothing checked it: the container tests assert the codec id, the colour tests
// assert the VUI, and neither reads `profile_idc`. `h264_nvenc` was silently
// producing Main, because its private `profile` option defaults to Main and
// overwrites `AVCodecContext::profile` rather than reading it.
//
// Asserted per encoder rather than by comparing the two, so a rig with one adapter
// still catches it.
TEST_F(MigrationParameterSetTest, EveryEncoderProducesTheHighProfileSpecNineRequires) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    ASSERT_FALSE(adapters.empty()) << "no adapter reports an H.264 encoder";

    for (const AdapterInfo* adapter : adapters) {
        const std::vector<std::uint8_t> extradata = extradata_for(*adapter);
        ASSERT_FALSE(extradata.empty()) << adapter->encode.encoder_name << " produced no extradata";

        const unsigned profile = profile_idc_of(extradata);
        std::cout << "[ MEASURED ] " << adapter->encode.encoder_name << " profile_idc: " << profile << " (0x"
                  << std::hex << profile << std::dec << ")\n";

        EXPECT_EQ(profile, kProfileIdcHigh)
            << adapter->encode.encoder_name << " encodes profile_idc " << profile
            << " where SPEC.md §9 requires High (100). Main profile lacks the 8x8 transform and costs quality at the "
               "same bitrate, and nothing else in the suite reads profile_idc";
    }
}

// The measurement SPEC.md §5.4's caveat turns on.
TEST_F(MigrationParameterSetTest, ReopeningTheSameEncoderYieldsIdenticalParameterSets) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    ASSERT_FALSE(adapters.empty()) << "no adapter reports an H.264 encoder";

    // The easy half first, and it is not a formality: if an encoder did not even
    // reproduce its own parameter sets across two opens with identical settings, then
    // *every* migration would need the fallback, including one that stays on the same
    // adapter — which is what row 9's session rebuild does.
    for (const AdapterInfo* adapter : adapters) {
        const std::vector<std::uint8_t> first = extradata_for(*adapter);
        const std::vector<std::uint8_t> second = extradata_for(*adapter);
        ASSERT_FALSE(first.empty()) << adapter->encode.encoder_name << " produced no extradata";

        std::cout << "[ MEASURED ] " << adapter->encode.encoder_name << " (" << adapter->id.to_string()
                  << ") extradata: " << first.size() << " bytes, " << to_hex(first) << "\n";

        EXPECT_EQ(first, second) << adapter->encode.encoder_name
                                 << " produced different parameter sets on two opens with identical settings; even a "
                                    "same-adapter session rebuild would need SPEC.md §5.4's fallback";
    }
}

// The hard half, and the one that decides row 11's design.
TEST_F(MigrationParameterSetTest, ParameterSetsAcrossTwoVendorsEncodersAreRecordedAsAFinding) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    if (adapters.size() < 2) {
        GTEST_SKIP() << "this rig reports one encoding adapter; a cross-vendor comparison needs two";
    }

    const std::vector<std::uint8_t> a = extradata_for(*adapters[0]);
    const std::vector<std::uint8_t> b = extradata_for(*adapters[1]);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());

    const bool identical = a == b;
    std::cout << "[ MEASURED ] cross-adapter parameter sets with identical settings:\n"
              << "[ MEASURED ]   " << adapters[0]->encode.encoder_name << ": " << a.size() << " bytes, " << to_hex(a)
              << "\n"
              << "[ MEASURED ]   " << adapters[1]->encode.encoder_name << ": " << b.size() << " bytes, " << to_hex(b)
              << "\n"
              << "[ MEASURED ]   identical: " << (identical ? "yes" : "NO") << "\n";

    // **This expectation encodes the finding, not a requirement.** SPEC.md §5.4 hoped
    // identical settings would produce identical parameter sets; on this rig they do
    // not, because two vendors' encoders make different legal choices about VUI
    // fields, reference frame counts and entropy coding.
    //
    // The consequence, which row 11 is built around: the container's `avcC` /
    // `CodecPrivate` is fixed by `avformat_write_header` and a cross-adapter migration
    // cannot match it. §5.4's fallback is therefore the only path, not the exception.
    //
    // If a future driver makes these agree, this fails and the design can be
    // simplified deliberately rather than by accident.
    EXPECT_FALSE(identical) << "the two encoders now agree on parameter sets. SPEC.md §5.4's preferred path (force "
                               "identical settings and reuse the stream) has become available; revisit row 11's "
                               "design rather than leaving the fallback in place unnecessarily";
}

// ---------------------------------------------------------------------------
// Can one file survive the handover at all?
// ---------------------------------------------------------------------------

// The experiment row 11's design turns on, run in miniature before building the
// eight-step procedure around it.
//
// SPEC.md §5.4 wants the migration to "Reuse the SAME muxer, SAME stream index, SAME
// timebase" and row 11 wants "a single continuous playable file". The measurement
// above says the two encoders' parameter sets differ, and the container's
// `avcC`/`CodecPrivate` is fixed at `avformat_write_header` — so the only way both can
// be true is if the *second* encoder emits its parameter sets **in band**, ahead of
// each IDR, and the decoder picks them up.
//
// §5.4 warns that "AVCC → Annex-B-in-`avc1` is invalid", which is about the packet
// *framing* (start codes versus length prefixes), not about whether SPS/PPS NAL units
// may appear in the stream. In-band parameter sets in length-prefixed form are legal.
// Whether they actually work through this muxer and this decoder is a question for the
// machine, not for reasoning, so: encode half a file on one adapter, half on the other,
// through one muxer, and decode the result.
TEST_F(MigrationParameterSetTest, AStreamHandedFromOneAdapterToTheOtherStillDecodesEndToEnd) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    if (adapters.size() < 2) {
        GTEST_SKIP() << "this rig reports one encoding adapter; a cross-vendor handover needs two";
    }

    constexpr int kFramesPerHalf = 30;
    const std::filesystem::path path = dir_->path() / "handover.mkv";

    // Both halves' frames come from a source on the *first* adapter, converted there.
    // A real migration re-creates the capture and convert stages on the new adapter
    // too; what is under test here is only whether the muxer and the decoder tolerate
    // the parameter-set change, so the pixel path is held constant deliberately.
    auto device_a = fc::test::device_for_adapter(adapters[0]->id);
    ASSERT_TRUE(device_a.has_value());
    auto device_b = fc::test::device_for_adapter(adapters[1]->id);
    ASSERT_TRUE(device_b.has_value());

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;

    fc::color::ConverterSettings converter_settings;
    converter_settings.width = kWidth;
    converter_settings.height = kHeight;

    std::size_t decoded = 0;
    {
        fc::encode::VideoEncoderSettings settings_a;
        settings_a.width = kWidth;
        settings_a.height = kHeight;
        settings_a.fps = kFps;
        settings_a.encoder_name = adapters[0]->encode.encoder_name;
        settings_a.pool_bind_flags = adapters[0]->encode.nv12_pool_bind_flags;

        auto created_a = fc::encode::create_video_encoder(settings_a);
        ASSERT_TRUE(created_a.has_value());
        const std::unique_ptr<fc::encode::IVideoEncoder> encoder_a = std::move(created_a).value();
        ASSERT_TRUE(encoder_a->open(device_a.value().device(), settings_a).has_value());

        fc::mux::MuxerSettings muxer_settings;
        muxer_settings.output = path;
        muxer_settings.container = fc::config::Container::Mkv;

        fc::mux::Muxer muxer;
        ASSERT_TRUE(muxer.open(muxer_settings, *encoder_a).has_value());

        // --- first half, adapter A -------------------------------------------
        std::int64_t next_pts = 0;
        const auto pump = [&muxer](fc::encode::IVideoEncoder& encoder) {
            for (;;) {
                auto received = encoder.receive();
                ASSERT_TRUE(received.has_value());
                std::optional<fc::encode::EncodedPacket> packet = std::move(received).value();
                if (!packet.has_value()) {
                    return;
                }
                ASSERT_TRUE(muxer.write(std::move(*packet)).has_value());
            }
        };

        {
            fc::test::SyntheticSource source;
            ASSERT_TRUE(source.configure(source_settings).has_value());
            ASSERT_TRUE(source.start(device_a.value().device(), fc::capture::CaptureTarget{}).has_value());

            fc::color::Nv12Converter converter;
            ASSERT_TRUE(converter.initialize(device_a.value().device(), converter_settings).has_value());
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device_a.value().device()->GetImmediateContext(&context);

            for (int i = 0; i < kFramesPerHalf; ++i) {
                auto frame = source.acquire(std::chrono::milliseconds{200});
                ASSERT_TRUE(frame.has_value());
                ASSERT_TRUE(
                    converter.convert(context.Get(), static_cast<ID3D11Texture2D*>(frame.value().texture)).has_value());
                ASSERT_TRUE(encoder_a->submit(converter.output(), next_pts).has_value());
                next_pts += fc::timing::ticks_per_frame(kFps);
                ASSERT_NO_FATAL_FAILURE(pump(*encoder_a));
                source.release(frame.value());
            }
            ASSERT_TRUE(encoder_a->flush().has_value());
            ASSERT_NO_FATAL_FAILURE(pump(*encoder_a));
            source.stop();
        }

        // --- the handover ----------------------------------------------------
        //
        // **The gap is load-bearing, and this is a constraint SPEC.md §5.4 does not
        // mention.** §5.4 step 7 talks about the *presentation* timeline staying
        // contiguous; it says nothing about decode order. But SPEC.md §9 sets
        // `max_b_frames = 2`, so DTS lags PTS by the reorder depth, and a fresh
        // encoder re-derives its DTS from its own first PTS. Handing over with no gap
        // therefore makes the new encoder's first DTS land *below* the old encoder's
        // last, and libavformat rejects the packet outright:
        //
        //     Application provided invalid, non monotonically increasing dts
        //     to muxer in stream 0: 483 >= 467
        //
        // Measured, by writing this experiment with a zero gap first. The migration
        // must therefore advance PTS by at least the reorder depth before the new
        // encoder's first frame — which a real migration does anyway, since §5.4
        // budgets up to 350 ms and that is ~21 frames at 60 fps against a reorder
        // depth of 3. The constraint is only invisible because the budget dwarfs it;
        // it would surface immediately on a fast migration or a deeper B-pyramid.
        constexpr int kMigrationGapFrames = 21; // §5.4's 350 ms budget at 60 fps
        next_pts += kMigrationGapFrames * fc::timing::ticks_per_frame(kFps);

        // `global_header = false` is the other half: without
        // `AV_CODEC_FLAG_GLOBAL_HEADER` the encoder writes SPS/PPS into the stream
        // ahead of each IDR instead of only into `extradata`, so the decoder meets
        // the new parameter sets where the old ones no longer apply.
        fc::encode::VideoEncoderSettings settings_b;
        settings_b.width = kWidth;
        settings_b.height = kHeight;
        settings_b.fps = kFps;
        settings_b.encoder_name = adapters[1]->encode.encoder_name;
        settings_b.pool_bind_flags = adapters[1]->encode.nv12_pool_bind_flags;
        settings_b.global_header = false;

        auto created_b = fc::encode::create_video_encoder(settings_b);
        ASSERT_TRUE(created_b.has_value());
        const std::unique_ptr<fc::encode::IVideoEncoder> encoder_b = std::move(created_b).value();
        ASSERT_TRUE(encoder_b->open(device_b.value().device(), settings_b).has_value());

        {
            fc::test::SyntheticSource source;
            ASSERT_TRUE(source.configure(source_settings).has_value());
            ASSERT_TRUE(source.start(device_b.value().device(), fc::capture::CaptureTarget{}).has_value());

            fc::color::Nv12Converter converter;
            ASSERT_TRUE(converter.initialize(device_b.value().device(), converter_settings).has_value());
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device_b.value().device()->GetImmediateContext(&context);

            for (int i = 0; i < kFramesPerHalf; ++i) {
                auto frame = source.acquire(std::chrono::milliseconds{200});
                ASSERT_TRUE(frame.has_value());
                ASSERT_TRUE(
                    converter.convert(context.Get(), static_cast<ID3D11Texture2D*>(frame.value().texture)).has_value());
                ASSERT_TRUE(encoder_b->submit(converter.output(), next_pts).has_value());
                next_pts += fc::timing::ticks_per_frame(kFps);
                ASSERT_NO_FATAL_FAILURE(pump(*encoder_b));
                source.release(frame.value());
            }
            ASSERT_TRUE(encoder_b->flush().has_value());
            ASSERT_NO_FATAL_FAILURE(pump(*encoder_b));
            source.stop();
        }

        ASSERT_TRUE(muxer.finalize().has_value());
    }

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;
    decoded = static_cast<std::size_t>(media.video.frame_count);

    std::cout << "[ MEASURED ] cross-adapter handover through one muxer: " << decoded << " of " << (2 * kFramesPerHalf)
              << " frames decoded (" << adapters[0]->encode.encoder_name << " -> " << adapters[1]->encode.encoder_name
              << ", in-band parameter sets on the second half)\n"
              << "[ MEASURED ]   container duration " << media.duration_seconds << " s, timebase "
              << media.video.time_base.num << "/" << media.video.time_base.den << "\n";

    // **This records the measured answer, and the answer is no.**
    //
    // Measured on the reference rig: 31 of 60 frames -- the whole first half plus a
    // single frame of the second. In-band parameter sets do not rescue the stream. The
    // decoder is initialised from the container's `CodecPrivate`, which holds the
    // *first* encoder's SPS, and a mid-stream SPS carrying the same id but different
    // content does not reliably re-initialise it.
    //
    // So SPEC.md §5.4's own fallback -- "close the segment and start a new one, and log
    // it loudly" -- is not the exception for a cross-vendor migration. It is the only
    // correct outcome, and §5.4's warning that "AVCC → Annex-B-in-`avc1` is invalid"
    // covers more ground than the framing question it appears to be about.
    //
    // That collides with SPEC.md §20 row 11's "single continuous playable file", and
    // the collision is real rather than an implementation choice. It is recorded in
    // docs/ACCEPTANCE.md and needs the owner's decision, because the two readings lead
    // to different products:
    //
    //   * a *same-adapter* rebuild -- a driver restart, a device reset, row 9's stall
    //     recovery -- reproduces its parameter sets byte for byte (asserted above), so
    //     one continuous file is achievable and row 11's assertion holds exactly;
    //   * a *cross-adapter* migration cannot be one file, by this measurement.
    //
    // The expectation is written as the finding so that a future FFmpeg or driver that
    // changes it fails here and forces the design to be revisited deliberately.
    EXPECT_LT(decoded, static_cast<std::size_t>(2 * kFramesPerHalf))
        << "the stream now survives a cross-vendor handover in one file (" << decoded << " of " << (2 * kFramesPerHalf)
        << " frames). SPEC.md §5.4's segment fallback may no longer be necessary and row 11 could meet its 'single "
           "continuous playable file' assertion across adapters -- revisit the design rather than relaxing this";
    EXPECT_GE(decoded, static_cast<std::size_t>(kFramesPerHalf))
        << "even the first half stopped decoding; the handover damaged already-written frames, which is worse than "
           "the segment split it was trying to avoid";
}

} // namespace
