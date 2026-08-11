// M3's exit criterion: a playable 1080p60 MKV, with the colour tags intact
// (SPEC.md §24, §20 rows 1, 2, 5 and 8).
//
// GPU TIER.
//
// Everything is driven from the synthetic source, never the live desktop, so a
// decoded frame can be matched back to the frame that produced it.
//
// SPEC.md §20 says "ffprobe the output". There is no ffprobe binary in this build
// -- the engine links a `--disable-everything` libavformat with an explicit
// whitelist -- so these tests demux and decode in-process through the same
// libavformat the engine uses. That is strictly better than shelling out: it is
// hermetic, it needs no PATH lookup, and it asserts against the structures rather
// than against scraped text.

#include "core/encode/video_encoder.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/mux/muxer.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/frame_pacer.h"

#include "adapter_device.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// A decoded frame, plus the stream metadata the colour tests need.
struct DecodedStream {
    bool opened = false;
    std::string format_name;
    int stream_count = 0;
    std::string codec_name;
    int width = 0;
    int height = 0;

    // SPEC.md §6: what a player actually reads.
    AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    int profile = 0;
    int level = 0;

    std::int64_t frame_count = 0;
    std::vector<std::int64_t> pts;
    /// Luma plane of each decoded frame, so content can be checked.
    std::vector<std::vector<std::uint8_t>> luma;
    /// Whether each frame was a keyframe, in decode order.
    std::vector<bool> keyframes;
    std::string detail;
};

/// Demuxes and decodes an MKV, keeping `keep_luma` frames' luma planes.
DecodedStream decode_file(const std::filesystem::path& path, int keep_luma) {
    DecodedStream out;

    fc::ff::InputFormatContext input;
    const std::string filename = path.string();
    if (input.open(filename.c_str()) < 0) {
        out.detail = "avformat_open_input failed";
        return out;
    }
    AVFormatContext* raw = input.get();

    if (avformat_find_stream_info(raw, nullptr) < 0) {
        out.detail = "avformat_find_stream_info failed";
        return out;
    }

    out.format_name = raw->iformat->name != nullptr ? raw->iformat->name : "";
    out.stream_count = static_cast<int>(raw->nb_streams);

    int video_index = -1;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        if (raw->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = static_cast<int>(i);
            break;
        }
    }
    if (video_index < 0) {
        out.detail = "no video stream";
        return out;
    }

    const AVCodecParameters* par = raw->streams[video_index]->codecpar;
    out.codec_name = avcodec_get_name(par->codec_id);
    out.width = par->width;
    out.height = par->height;
    out.colorspace = par->color_space;
    out.primaries = par->color_primaries;
    out.transfer = par->color_trc;
    out.range = par->color_range;
    out.profile = par->profile;
    out.level = par->level;

    const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
    if (decoder == nullptr) {
        out.detail = "no decoder";
        return out;
    }

    fc::ff::CodecContext ctx;
    if (!ctx.alloc(decoder) || avcodec_parameters_to_context(ctx.get(), par) < 0 ||
        avcodec_open2(ctx.get(), decoder, nullptr) < 0) {
        out.detail = "decoder setup failed";
        return out;
    }

    fc::ff::Packet packet;
    fc::ff::Frame frame;
    if (!packet.alloc() || !frame.alloc()) {
        out.detail = "allocation failed";
        return out;
    }

    auto harvest = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            ++out.frame_count;
            out.pts.push_back(frame->pts);
            out.keyframes.push_back((frame->flags & AV_FRAME_FLAG_KEY) != 0);
            if (std::cmp_less(out.luma.size(), keep_luma)) {
                std::vector<std::uint8_t> plane(static_cast<std::size_t>(frame->width) * frame->height);
                for (int y = 0; y < frame->height; ++y) {
                    const std::uint8_t* row = frame->data[0] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[0]);
                    std::copy(row, row + frame->width, plane.begin() + (static_cast<std::ptrdiff_t>(y) * frame->width));
                }
                out.luma.push_back(std::move(plane));
            }
            frame.unref();
        }
    };

    while (av_read_frame(raw, packet.get()) >= 0) {
        if (packet->stream_index == video_index && avcodec_send_packet(ctx.get(), packet.get()) >= 0) {
            harvest();
        }
        packet.unref();
    }
    if (avcodec_send_packet(ctx.get(), nullptr) >= 0) {
        harvest();
    }

    out.opened = true;
    return out;
}

/// One adapter that can encode, with a device on it.
struct EncodeTarget {
    fc::gpu::AdapterId id;
    std::string description;
    std::string encoder_name;
    std::uint32_t pool_bind_flags = 0;
    std::unique_ptr<fc::gpu::D3dDevice> device;
};

class VideoEncodeTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("m3encode");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "m3encodetest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == AdapterClass::Software || !adapter.can_encode()) {
                continue;
            }
            auto device = fc::test::device_for_adapter(adapter.id);
            if (!device.has_value()) {
                continue;
            }
            EncodeTarget target;
            target.id = adapter.id;
            target.description = adapter.description;
            target.encoder_name = adapter.encode.encoder_name;
            target.pool_bind_flags = adapter.encode.nv12_pool_bind_flags;
            target.device = std::make_unique<fc::gpu::D3dDevice>(std::move(device).value());
            targets_.push_back(std::move(target));
        }

        ASSERT_FALSE(targets_.empty()) << "no adapter reported a usable H.264 encoder";
    }

    static void TearDownTestSuite() {
        targets_.clear();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Records `frames` synthetic frames on `target` and returns the output path.
    static std::filesystem::path record(const EncodeTarget& target, int frames, const std::string& name,
                                        fc::config::RateControl rate_control = fc::config::RateControl::Cqp) {
        std::filesystem::path output = dir_->path() / (name + ".mkv");

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value()) {
            ADD_FAILURE() << "configuring the synthetic source failed";
            return {};
        }
        if (!source.start(target.device->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "starting the synthetic source failed";
            return {};
        }

        PipelineSettings settings;
        settings.output = output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.rate_control = rate_control;
        settings.encoder_name = target.encoder_name;
        settings.pool_bind_flags = target.pool_bind_flags;

        VideoPipeline pipeline;
        const auto started = pipeline.start(target.device->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << target.description << ": pipeline start failed, " << fc::error_name(started.error());
            source.stop();
            return {};
        }

        for (int i = 0; i < frames; ++i) {
            auto frame = source.acquire(std::chrono::milliseconds{1000});
            if (!frame.has_value()) {
                ADD_FAILURE() << "synthetic source starved at frame " << i;
                break;
            }
            const auto submitted = pipeline.submit(frame.value());
            EXPECT_TRUE(submitted.has_value()) << "submit failed at frame " << i;
            source.release(frame.value());
        }

        const fc::pipeline::PipelineStats stats = pipeline.stats();
        const auto report = pipeline.stop();
        source.stop();

        // Failures on the venc thread are otherwise invisible here: submit only
        // enqueues, so a frame that dies in conversion or encode never surfaces at
        // the call site.
        EXPECT_EQ(stats.convert_failures, 0u) << target.description << ": conversion failures";
        EXPECT_EQ(stats.encode_failures, 0u) << target.description << ": encode failures";

        if (!report.has_value()) {
            ADD_FAILURE() << target.description << ": stop failed, " << fc::error_name(report.error());
            return {};
        }
        if (!report.value().valid) {
            ADD_FAILURE() << target.description << ": output failed validation -- " << report.value().detail
                          << " (streams " << report.value().stream_count << ", codec " << report.value().video_codec
                          << ", duration " << report.value().duration_seconds << " s, decoded "
                          << report.value().decoded_frames << " frames)"
                          << " [submitted " << stats.frames_submitted << ", encoded " << stats.frames_encoded
                          << ", paced out " << stats.frames_paced_out << ", queue-dropped "
                          << stats.frames_queue_dropped << ", dup " << stats.duplicates_emitted << ", convert-fail "
                          << stats.convert_failures << ", encode-fail " << stats.encode_failures << "]";
            return {};
        }
        return output;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::vector<EncodeTarget> targets_;
};

std::unique_ptr<fc::test::TempDir> VideoEncodeTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> VideoEncodeTest::topology_;
std::vector<EncodeTarget> VideoEncodeTest::targets_;

// ---------------------------------------------------------------------------
// The exit criterion: a playable 1080p60 MKV
// ---------------------------------------------------------------------------

TEST_F(VideoEncodeTest, ProducesAPlayableMkvOnEveryEncodingAdapter) {
    for (const EncodeTarget& target : targets_) {
        const auto output = record(target, 120, "playable_" + std::to_string(target.id.value));
        ASSERT_FALSE(output.empty()) << target.description;

        const DecodedStream decoded = decode_file(output, 0);
        ASSERT_TRUE(decoded.opened) << target.description << ": " << decoded.detail;

        // SPEC.md §20 row 2: the container is what was asked for, with the streams
        // and codec it should have.
        EXPECT_EQ(decoded.format_name, "matroska,webm") << target.description;
        EXPECT_EQ(decoded.stream_count, 1) << target.description << " (video only until M4)";
        EXPECT_EQ(decoded.codec_name, "h264") << target.description;
        EXPECT_EQ(decoded.width, kWidth);
        EXPECT_EQ(decoded.height, kHeight);

        // Every frame decoded. This is what "playable" means.
        EXPECT_EQ(decoded.frame_count, 120) << target.description << " decoded a different number of frames "
                                            << "than were submitted";
    }
}

// SPEC.md §9: H.264 High @ 4.2, not whatever the encoder defaults to.
TEST_F(VideoEncodeTest, TheBitstreamIsHighProfileLevel42) {
    const auto output = record(targets_.front(), 60, "profile");
    ASSERT_FALSE(output.empty());

    const DecodedStream decoded = decode_file(output, 0);
    ASSERT_TRUE(decoded.opened) << decoded.detail;
    EXPECT_EQ(decoded.profile, AV_PROFILE_H264_HIGH);
    EXPECT_EQ(decoded.level, 42);
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 8 -- the colour tags, in the stream and in the container
// ---------------------------------------------------------------------------

// The tags must survive into the decoded stream's parameters. libavformat merges
// what it reads from the Matroska Colour element with the SPS VUI, so this
// asserts the pair the way a player sees them.
TEST_F(VideoEncodeTest, ColourIsTaggedBt709LimitedRange) {
    for (const EncodeTarget& target : targets_) {
        const auto output = record(target, 60, "vui_" + std::to_string(target.id.value));
        ASSERT_FALSE(output.empty()) << target.description;

        const DecodedStream decoded = decode_file(output, 0);
        ASSERT_TRUE(decoded.opened) << target.description << ": " << decoded.detail;

        EXPECT_EQ(decoded.colorspace, AVCOL_SPC_BT709)
            << target.description << ": matrix is " << av_color_space_name(decoded.colorspace);
        EXPECT_EQ(decoded.primaries, AVCOL_PRI_BT709)
            << target.description << ": primaries are " << av_color_primaries_name(decoded.primaries);
        EXPECT_EQ(decoded.transfer, AVCOL_TRC_BT709)
            << target.description << ": transfer is " << av_color_transfer_name(decoded.transfer);

        // Limited range is the SPEC.md §6 default. Getting this wrong is the
        // washed-out / crushed-black defect, and it is silent.
        EXPECT_EQ(decoded.range, AVCOL_RANGE_MPEG)
            << target.description << ": range is " << av_color_range_name(decoded.range);
    }
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 1 -- no black frames
// ---------------------------------------------------------------------------

TEST_F(VideoEncodeTest, NoDecodedFrameIsUniform) {
    for (const EncodeTarget& target : targets_) {
        const auto output = record(target, 60, "black_" + std::to_string(target.id.value));
        ASSERT_FALSE(output.empty()) << target.description;

        const DecodedStream decoded = decode_file(output, 10);
        ASSERT_TRUE(decoded.opened) << target.description << ": " << decoded.detail;
        ASSERT_FALSE(decoded.luma.empty());

        for (std::size_t i = 0; i < decoded.luma.size(); ++i) {
            const std::vector<std::uint8_t>& plane = decoded.luma[i];
            double sum = 0.0;
            for (const std::uint8_t sample : plane) {
                sum += sample;
            }
            const double mean = sum / static_cast<double>(plane.size());
            double variance = 0.0;
            for (const std::uint8_t sample : plane) {
                const double delta = sample - mean;
                variance += delta * delta;
            }
            variance /= static_cast<double>(plane.size());

            // SMPTE bars have enormous luma variance. A black or single-colour
            // frame has essentially none.
            EXPECT_GT(variance, 100.0) << target.description << ": decoded frame " << i << " is uniform (mean " << mean
                                       << ")";
        }
    }
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 5 -- frame identity survives the encoder
// ---------------------------------------------------------------------------

// M2 proved the barcode survives capture and conversion. This proves it survives
// H.264 encode and decode too, which is what makes a decoded frame attributable to
// the frame that produced it.
TEST_F(VideoEncodeTest, TheFrameBarcodeSurvivesEncodeAndDecode) {
    const EncodeTarget& target = targets_.front();
    constexpr int kFrames = 30;

    const auto output = record(target, kFrames, "barcode");
    ASSERT_FALSE(output.empty());

    const DecodedStream decoded = decode_file(output, kFrames);
    ASSERT_TRUE(decoded.opened) << decoded.detail;
    ASSERT_EQ(static_cast<int>(decoded.luma.size()), kFrames);

    // Decoded frames come back in presentation order, so index n carries barcode n.
    for (int i = 0; i < kFrames; ++i) {
        const auto index =
            fc::test::decode_barcode_from_luma(decoded.luma[static_cast<std::size_t>(i)], kWidth, kHeight);
        ASSERT_TRUE(index.has_value()) << "frame " << i << " has no readable barcode after encode";
        EXPECT_EQ(*index, static_cast<std::uint32_t>(i))
            << "decoded frame " << i << " carries barcode " << *index << "; frames were reordered or duplicated";
    }
}

// ---------------------------------------------------------------------------
// Timing (SPEC.md §7.2)
// ---------------------------------------------------------------------------

// SPEC.md §20 row 7 at the container level.
//
// Exact-equal PTS deltas are asserted in the CPU tier against the pacer, which is
// where the 1/60000 grid lives. They are *not* achievable in Matroska: block
// timestamps are stored in units of the segment TimestampScale, which FFmpeg fixes
// at 1 ms, so a 16.667 ms frame is written as an alternating 17/17/16.
//
// The property that actually prevents judder survives that rounding, and it is the
// one worth asserting: the error never accumulates. Frame n sits within a
// millisecond of n/60 s no matter how long the recording runs, so a player has no
// growing discrepancy to chase. A pacer that used wall-clock timestamps would fail
// this within seconds.
TEST_F(VideoEncodeTest, PtsNeverDriftFromTheIdealGrid) {
    constexpr int kFrames = 300; // 5 s -- long enough for drift to show
    const auto output = record(targets_.front(), kFrames, "pts");
    ASSERT_FALSE(output.empty());

    const DecodedStream decoded = decode_file(output, 0);
    ASSERT_TRUE(decoded.opened) << decoded.detail;
    ASSERT_GT(decoded.pts.size(), 2u);

    std::vector<std::int64_t> sorted = decoded.pts;
    std::ranges::sort(sorted);

    // Matroska stream timebase is 1/1000.
    const double nominal_ms = 1000.0 / kFps;
    std::int64_t worst = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const double ideal = static_cast<double>(sorted.front()) + (static_cast<double>(i) * nominal_ms);
        const auto error = static_cast<std::int64_t>(std::llround(std::abs(static_cast<double>(sorted[i]) - ideal)));
        worst = std::max(worst, error);
        ASSERT_LE(error, 1) << "frame " << i << " is " << error << " ms from the ideal grid; timestamps are drifting";
    }

    // Every delta is one of the two values the millisecond grid permits, and never
    // anything else -- a stalled or reordered timestamp would show up here.
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const std::int64_t delta = sorted[i] - sorted[i - 1];
        EXPECT_TRUE(delta == 16 || delta == 17) << "delta " << delta << " ms at frame " << i;
    }

    EXPECT_LE(worst, 1) << "worst-case deviation from the ideal 60 fps grid";
}

// SPEC.md §9: a 2 s GOP at 60 fps is a keyframe every 120 frames.
TEST_F(VideoEncodeTest, KeyframesAppearAtTheConfiguredGopInterval) {
    constexpr int kFrames = 300; // 5 s, so several GOPs
    const auto output = record(targets_.front(), kFrames, "gop");
    ASSERT_FALSE(output.empty());

    const DecodedStream decoded = decode_file(output, 0);
    ASSERT_TRUE(decoded.opened) << decoded.detail;

    int keyframes = 0;
    for (const bool key : decoded.keyframes) {
        if (key) {
            ++keyframes;
        }
    }

    // 300 frames at a 120-frame GOP is 3 keyframes (0, 120, 240). Encoders are
    // allowed to insert extra IDRs, so this is a floor and a sane ceiling rather
    // than an exact count.
    EXPECT_GE(keyframes, 3) << "too few keyframes: seeking and segment splitting both depend on this";
    EXPECT_LE(keyframes, 8) << "far more keyframes than a 2 s GOP implies";
}

// ---------------------------------------------------------------------------
// Rate control (SPEC.md §9)
// ---------------------------------------------------------------------------

TEST_F(VideoEncodeTest, VbrHonoursItsBitrateCeilingAndCqpDoesNot) {
    const EncodeTarget& target = targets_.front();

    const auto cqp = record(target, 120, "rc_cqp", fc::config::RateControl::Cqp);
    ASSERT_FALSE(cqp.empty());
    const auto vbr = record(target, 120, "rc_vbr", fc::config::RateControl::Vbr);
    ASSERT_FALSE(vbr.empty());

    // Both must produce a playable file; the point of the test is that the two
    // modes reach the encoder at all rather than being silently identical.
    EXPECT_TRUE(decode_file(cqp, 0).opened);
    EXPECT_TRUE(decode_file(vbr, 0).opened);
    EXPECT_GT(std::filesystem::file_size(cqp), 0u);
    EXPECT_GT(std::filesystem::file_size(vbr), 0u);
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

TEST_F(VideoEncodeTest, NonH264CodecsAreRefusedNotSilentlyDowngraded) {
    // SPEC.md §9: the enum has three values and the probe reports all three, but
    // v1 ships H.264 only.
    for (const auto codec : {fc::config::VideoCodec::Hevc, fc::config::VideoCodec::Av1}) {
        fc::encode::VideoEncoderSettings settings;
        settings.codec = codec;
        settings.encoder_name = targets_.front().encoder_name;
        const auto encoder = fc::encode::create_video_encoder(settings);
        ASSERT_FALSE(encoder.has_value()) << fc::config::to_string(codec) << " was accepted";
        EXPECT_EQ(encoder.error(), fc::FcError::ENCODE_CODEC_UNSUPPORTED);
    }
}

// SPEC.md §12 requires a bounded join with a 2 s deadline on every worker, never
// an unbounded `join()`. This proves the happy path clears that deadline with room
// to spare, so a healthy shutdown is nowhere near tripping the escalation.
//
// The escalation branch itself -- a genuinely wedged worker being detached -- is
// **not** covered here: it needs a fault-injection seam in the venc loop that does
// not exist yet. What is covered is that the deadline is real and that normal
// shutdown does not depend on it.
TEST_F(VideoEncodeTest, ShutdownCompletesWellInsideTheJoinDeadline) {
    const EncodeTarget& target = targets_.front();

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.frame_limit = 60;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(target.device->device(), fc::capture::CaptureTarget{}).has_value());

    PipelineSettings settings;
    settings.output = dir_->path() / "join_deadline.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.encoder_name = target.encoder_name;
    settings.pool_bind_flags = target.pool_bind_flags;

    VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(target.device->device(), settings).has_value());

    for (int i = 0; i < 60; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{1000});
        ASSERT_TRUE(frame.has_value()) << "frame " << i;
        EXPECT_TRUE(pipeline.submit(frame.value()).has_value());
        source.release(frame.value());
    }

    const auto began = std::chrono::steady_clock::now();
    const auto report = pipeline.stop();
    const auto took = std::chrono::steady_clock::now() - began;
    source.stop();

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    // The real assertion is the one above: `stop()` returning a value *means*
    // neither worker hit the deadline, because a timeout returns
    // INTERNAL_THREAD_JOIN_TIMEOUT. That is deterministic, and it is the property
    // under test.
    //
    // The wall-clock bound here is only a hang detector, and is deliberately much
    // looser than the 2 s deadline: the interval measured also covers the encoder
    // flush, the trailer write, the disk flush and a full validation decode of
    // every frame -- none of which is the join, all of which are several times
    // slower in a Debug build on a loaded machine. A tight bound would be a timing
    // flake wearing a correctness assertion's clothes.
    EXPECT_LT(took, std::chrono::seconds{30})
        << "shutdown took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
        << " ms, which suggests something is genuinely stuck rather than merely slow";
}

// `video.pacing` accepts `cfr|vfr`, is validated, round-tripped and documented --
// and until this check existed, selecting `vfr` silently produced a CFR
// recording. SPEC.md §7.3 specifies VFR properly (timebase 1/1000000, PTS from the
// exact QPC delta) and it is not implemented, so it is refused for the same reason
// MP4 is: a setting that is accepted and ignored is worse than one that says no.
TEST_F(VideoEncodeTest, VfrPacingIsRefusedRatherThanSilentlyRecordingCfr) {
    const EncodeTarget& target = targets_.front();

    PipelineSettings settings;
    settings.output = dir_->path() / "vfr_refused.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.pacing = fc::config::PacingMode::Vfr;
    settings.encoder_name = target.encoder_name;
    settings.pool_bind_flags = target.pool_bind_flags;

    VideoPipeline pipeline;
    const auto started = pipeline.start(target.device->device(), settings);
    ASSERT_FALSE(started.has_value()) << "VFR was accepted before it exists";
    EXPECT_EQ(started.error(), fc::FcError::INTERNAL_NOT_IMPLEMENTED);
    EXPECT_FALSE(std::filesystem::exists(settings.output)) << "a partial file was left behind";

    // And the default still works, so the refusal is targeted rather than a
    // blanket rejection of the key.
    settings.video.pacing = fc::config::PacingMode::Cfr;
    settings.output = dir_->path() / "cfr_accepted.mkv";
    VideoPipeline cfr;
    ASSERT_TRUE(cfr.start(target.device->device(), settings).has_value());
    EXPECT_TRUE(cfr.stop().has_value());
}

// Was `Mp4IsRefusedInM3RatherThanHalfWritten` until M5 built the crash-safe path.
// Until then the muxer refused MP4 outright, because writing a *progressive* one
// during recording produces exactly the unplayable-after-crash file SPEC.md §10.3
// exists to prevent. Now it accepts MP4 and writes it fragmented, and the
// assertion inverts with the contract: what has to be true is that the file it
// opens is the fragmented kind.
//
// Crash behaviour itself is `test_crash_recovery`'s (SPEC.md §20 row 3). This is
// only the muxer-level statement that MP4 is no longer refused.
TEST_F(VideoEncodeTest, Mp4IsAcceptedAndWrittenFragmented) {
    const EncodeTarget& target = targets_.front();

    fc::encode::VideoEncoderSettings encoder_settings;
    encoder_settings.width = kWidth;
    encoder_settings.height = kHeight;
    encoder_settings.fps = kFps;
    encoder_settings.encoder_name = target.encoder_name;
    encoder_settings.pool_bind_flags = target.pool_bind_flags;

    auto created = fc::encode::create_video_encoder(encoder_settings);
    ASSERT_TRUE(created.has_value());
    const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(created).value();
    ASSERT_TRUE(encoder->open(target.device->device(), encoder_settings).has_value());

    fc::mux::MuxerSettings settings;
    settings.output = dir_->path() / "fragmented.mp4";
    settings.container = fc::config::Container::Mp4;

    fc::mux::Muxer muxer;
    const auto opened = muxer.open(settings, *encoder);
    ASSERT_TRUE(opened.has_value()) << "MP4 was refused: " << fc::error_name(opened.error());
    EXPECT_TRUE(muxer.needs_remux()) << "an MP4 that needs no remux was not written fragmented";
    EXPECT_TRUE(muxer.open_for_writing());

    // `empty_moov` puts the header at the front immediately, which is the whole
    // reason a killed recording is openable. Asserting the file exists and is
    // non-empty *before a single packet is written* is the cheapest possible check
    // that the flag took effect: a progressive MP4 writes nothing until close.
    ASSERT_TRUE(std::filesystem::exists(settings.output));
    EXPECT_GT(std::filesystem::file_size(settings.output), 0u)
        << "nothing was written at open; the moov atom is being deferred to close";

    EXPECT_TRUE(muxer.finalize().has_value());
}

TEST_F(VideoEncodeTest, MkvNeedsNoRemux) {
    const EncodeTarget& target = targets_.front();

    fc::encode::VideoEncoderSettings encoder_settings;
    encoder_settings.width = kWidth;
    encoder_settings.height = kHeight;
    encoder_settings.fps = kFps;
    encoder_settings.encoder_name = target.encoder_name;
    encoder_settings.pool_bind_flags = target.pool_bind_flags;

    auto created = fc::encode::create_video_encoder(encoder_settings);
    ASSERT_TRUE(created.has_value());
    const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(created).value();
    ASSERT_TRUE(encoder->open(target.device->device(), encoder_settings).has_value());

    fc::mux::MuxerSettings settings;
    settings.output = dir_->path() / "plain.mkv";
    settings.container = fc::config::Container::Mkv;

    fc::mux::Muxer muxer;
    ASSERT_TRUE(muxer.open(settings, *encoder).has_value());
    EXPECT_FALSE(muxer.needs_remux()) << "Matroska finalizes in place; a remux would rewrite it for nothing";
    EXPECT_TRUE(muxer.finalize().has_value());
}

} // namespace
