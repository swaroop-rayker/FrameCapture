// SPEC.md §13 rung 5 -- "All HW encoders unavailable -> fall back to `libx264
// superfast`".
//
// GPU TIER, despite being about a *software* encoder. The frames still arrive as D3D11
// NV12 textures from the capture and colour stages, and the whole of what rung 5 adds
// is getting them out of GPU memory correctly. A test without a GPU could not exercise
// the part that is new.
//
// ---------------------------------------------------------------------------
// Why the barcode assertion is the important one
// ---------------------------------------------------------------------------
// `CodecAvailability.TheSoftwareFallbackEncoderIsPresent` already proves libx264 is in
// the build and accepts NV12. What was missing until M6 is the submit path: every
// other encoder in this engine takes a `CopySubresourceRegion` into a hardware pool and
// never touches system memory, and libx264 cannot read a texture at all.
//
// The risk in that readback is not "does it run" -- it is plane layout. A mapped D3D11
// NV12 texture is one allocation holding the luma plane followed by the interleaved
// chroma plane, and the chroma plane's offset is *not* reliably `RowPitch * height`;
// drivers may align it further. Get it wrong and the encoder still runs, still produces
// a playable file, and the file is green or magenta with the picture shifted. Nothing
// short of decoding the content catches it.
//
// So this decodes the output and reads the frame-index barcode out of every frame. The
// barcode is drawn in pure black and white cells in the luma plane, so it survives
// encoding but not a misaligned plane or a mis-strided copy -- a frame whose cells are
// not cleanly black or white decodes as `nullopt` rather than as a wrong number.
//
// The engine hands the download to `av_hwframe_transfer_data` precisely so the plane
// arithmetic stays FFmpeg's problem. This test is what makes that delegation checkable.

#include "core/capture/capture_frame.h"
#include "core/encode/video_encoder.h"
#include "core/gpu/encoder_probe.h"
#include "core/gpu/gpu_topology.h"
#include "core/health/health_monitor.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// Two seconds. libx264 `superfast` at 1080p plus a full GPU readback per frame is far
/// slower than the hardware path, and rung 5 is the bottom of the ladder -- the point
/// is that it works, not that it is quick.
constexpr int kFrames = 120;

class SoftwareEncoderTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("swenc");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "softwareenc001";
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

    /// Any hardware adapter. The *encoder* is software, but the textures have to live
    /// somewhere and the readback has to come off a real device.
    static const AdapterInfo* any_adapter() {
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software) {
                return &adapter;
            }
        }
        return nullptr;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> SoftwareEncoderTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> SoftwareEncoderTest::topology_;

// ---------------------------------------------------------------------------

TEST_F(SoftwareEncoderTest, EveryFrameSurvivesTheReadbackIntoTheSoftwareEncoder) {
    const AdapterInfo* adapter = any_adapter();
    ASSERT_NE(adapter, nullptr) << "no hardware adapter to read frames back from";

    auto device = fc::test::device_for_adapter(adapter->id);
    ASSERT_TRUE(device.has_value());

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.frame_limit = kFrames;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device.value().device(), fc::capture::CaptureTarget{}).has_value());

    const std::filesystem::path path = dir_->path() / "software.mkv";

    fc::pipeline::PipelineSettings settings;
    settings.output = path;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    // The whole point. Not an adapter's probed encoder -- the name rung 5 selects when
    // every adapter's probe came back negative.
    settings.encoder_name = fc::encode::kSoftwareEncoderName;
    // No pool bind flags: there is no hardware input pool on this path. Leaving this 0
    // would be a defect on the hardware path (BUG-001) and is correct here.
    settings.pool_bind_flags = 0;

    fc::pipeline::VideoPipeline pipeline;
    const fc::Result<void> started = pipeline.start(device.value().device(), settings);
    ASSERT_TRUE(started.has_value()) << "the software encoder would not open: " << fc::error_name(started.error());

    const auto begun = std::chrono::steady_clock::now();
    int submitted = 0;
    for (;;) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            break;
        }
        // Backpressure-aware, so nothing is dropped and the barcode sequence below is
        // contiguous. libx264 is slow enough that flooding would drop most of it.
        for (int spins = 0; spins < 2'000'000; ++spins) {
            const fc::pipeline::PipelineStats stats = pipeline.stats();
            const std::uint64_t settled = stats.frames_encoded + stats.frames_paced_out + stats.frames_queue_dropped;
            if (static_cast<std::uint64_t>(submitted) - std::min(settled, static_cast<std::uint64_t>(submitted)) < 4) {
                break;
            }
            std::this_thread::yield();
        }
        ASSERT_TRUE(pipeline.submit(frame.value()).has_value());
        ++submitted;
        source.release(frame.value());
    }
    ASSERT_EQ(submitted, kFrames);

    const fc::pipeline::PipelineHealth health = pipeline.health();
    const auto report = pipeline.stop();
    source.stop();
    const fc::pipeline::PipelineStats stats = pipeline.stats();
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = true; // the barcodes are the point
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::cout << "[ MEASURED ] libx264 superfast, 1080p, with GPU readback:\n"
              << "[ MEASURED ]   " << submitted << " frames in " << elapsed << " s ("
              << static_cast<double>(submitted) / elapsed << " fps), encoded " << stats.frames_encoded
              << ", queue-dropped " << stats.frames_queue_dropped << "\n"
              << "[ MEASURED ]   file: " << media.video.frame_count << " frames, " << media.video.width << "x"
              << media.video.height << ", " << media.video.codec_name << ", rung " << static_cast<int>(health.rung)
              << " (" << fc::health::to_string(health.rung) << ")\n";

    // --- the timeline -----------------------------------------------------
    EXPECT_EQ(media.video.frame_count, kFrames);
    EXPECT_EQ(stats.frames_queue_dropped, 0u);
    EXPECT_EQ(media.video.width, kWidth);
    EXPECT_EQ(media.video.height, kHeight);

    // --- the content, which is what proves the readback ------------------
    ASSERT_EQ(media.video.barcodes.size(), static_cast<std::size_t>(kFrames));
    std::size_t unreadable = 0;
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < media.video.barcodes.size(); ++i) {
        if (!media.video.barcodes[i].has_value()) {
            ++unreadable;
            continue;
        }
        if (*media.video.barcodes[i] != static_cast<std::uint32_t>(i)) {
            ++wrong;
        }
    }
    // Zero tolerance on both. A misaligned chroma plane offset does not corrupt a few
    // frames, it corrupts all of them; and a barcode that reads as the wrong index
    // means the luma plane was strided wrongly, which is the same defect wearing a
    // different symptom.
    EXPECT_EQ(unreadable, 0u) << unreadable
                              << " frames had a barcode that was neither black nor white; the luma plane came back "
                                 "misaligned or mis-strided from the readback";
    EXPECT_EQ(wrong, 0u) << wrong << " frames carried the wrong index after the readback";

    // --- rung 5's reporting ----------------------------------------------
    EXPECT_FALSE(health.hardware_encoder) << "the software path must report itself as software";
    EXPECT_EQ(health.rung, fc::health::Rung::HardwareEncodersUnavailable);
    // Rung 5 is a fact about the machine, not a pressure level: it must not drag the
    // capture rate down with it. The unit test pins this arithmetically; this is the
    // same claim on a real recording.
    EXPECT_EQ(health.retimes, 0u);
    EXPECT_EQ(health.target_fps, kFps);
}

// SPEC.md §13 rung 2 on the one encoder in this build that can perform it. The
// hardware encoders' inability is the measured finding of BUG-024, and it is asserted
// here rather than only written down -- a future FFmpeg that gains the capability
// should make this test fail and be updated deliberately.
TEST_F(SoftwareEncoderTest, TheSoftwareEncoderAcceptsARuntimeQualityChangeAndTheHardwareOnesDoNot) {
    const AdapterInfo* adapter = any_adapter();
    ASSERT_NE(adapter, nullptr);

    auto device = fc::test::device_for_adapter(adapter->id);
    ASSERT_TRUE(device.has_value());

    // Recorded because BUG-001's value is adapter-specific and this is the second place
    // that depends on it. The software path probes for itself rather than being handed an
    // encoder-pool value it has no encoder to have probed for; measured on this rig it
    // resolves to 0x280 (`DECODER | UNORDERED_ACCESS`), and `SHADER_RESOURCE` alone is
    // refused with E_INVALIDARG.
    std::cout << "[ MEASURED ]   probed NV12 readback-pool bind flags: 0x" << std::hex
              << fc::gpu::probe_nv12_pool_bind_flags(device.value().device(), kWidth, kHeight, 1) << std::dec << "\n";

    fc::encode::VideoEncoderSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.fps = kFps;
    settings.encoder_name = fc::encode::kSoftwareEncoderName;
    settings.pool_bind_flags = 0;

    auto software = fc::encode::create_video_encoder(settings);
    ASSERT_TRUE(software.has_value());
    const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(software).value();
    ASSERT_TRUE(encoder->open(device.value().device(), settings).has_value());

    EXPECT_FALSE(encoder->is_hardware());
    EXPECT_TRUE(encoder->try_set_cqp(settings.cqp + 4)) << "libx264 honours a runtime `qp` change through "
                                                           "x264_encoder_reconfig; if this fails, rung 2 has no "
                                                           "implementation on any encoder at all";

    // And the hardware side of the finding, on whichever encoder this rig reports.
    for (const AdapterInfo& candidate : topology_->topology().adapters) {
        if (candidate.adapter_class == AdapterClass::Software || !candidate.can_encode()) {
            continue;
        }
        auto hw_device = fc::test::device_for_adapter(candidate.id);
        ASSERT_TRUE(hw_device.has_value());

        fc::encode::VideoEncoderSettings hw_settings = settings;
        hw_settings.encoder_name = candidate.encode.encoder_name;
        hw_settings.pool_bind_flags = candidate.encode.nv12_pool_bind_flags;

        auto created = fc::encode::create_video_encoder(hw_settings);
        ASSERT_TRUE(created.has_value());
        const std::unique_ptr<fc::encode::IVideoEncoder> hw = std::move(created).value();
        ASSERT_TRUE(hw->open(hw_device.value().device(), hw_settings).has_value());

        EXPECT_TRUE(hw->is_hardware());
        EXPECT_FALSE(hw->try_set_cqp(hw_settings.cqp + 4))
            << hw_settings.encoder_name
            << " reported that it applied a runtime QP change. Measured against FFmpeg n8.1.2 that is not possible "
               "(BUG-024) -- if it has become possible, SPEC.md §13 rung 2 can now be implemented on hardware and "
               "this expectation should be inverted deliberately rather than relaxed";
        std::cout << "[ MEASURED ]   " << hw_settings.encoder_name << ": runtime QP change unavailable, as expected\n";
    }
}

} // namespace
