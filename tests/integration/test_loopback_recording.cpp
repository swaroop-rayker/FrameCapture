// The real WASAPI path, end to end (SPEC.md §8.1, §8.2, §12).
//
// GPU TIER, and the only test that records with `AudioSource::SystemLoopback`.
//
// Everything else in the audio suite drives `AudioEncodePath` directly, because
// that is the half of the subsystem that can be asserted deterministically. What
// none of it touches is the part below the WASAPI seam: `LoopbackCapture`'s COM
// apartment and MMCSS registration, the sink that carries buffers into the encode
// path, the `on_first_packet` callback that resolves the shared epoch from the
// `audio` thread, and the `silence` watchdog running on a real clock against a
// real endpoint. Those four only exist on hardware.
//
// **The headline assertion is SPEC.md §8.2's.** WASAPI loopback emits nothing at
// all when no application is playing, so a build without a silence generator
// records a file whose audio track is shorter than its video track by however long
// the machine was quiet. An idle machine is quiet the entire time, which makes
// this the *easy* case to run and the hardest one to pass: if the silence
// generator is missing or wired wrong, the audio track here is close to zero
// seconds long.
//
// Timestamps are real, unlike `test_av_sync`'s. They have to be -- WASAPI stamps
// its buffers from QPC and the video has to share that clock -- so this test runs
// in real time and is deliberately short.

#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::pipeline::AudioSource;
using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 60;

/// Long enough that a missing silence path is unmistakable, short enough that a
/// real-time test stays a test.
///
/// **What this does not cover, measured rather than assumed.** On the reference
/// rig the endpoint delivers buffers continuously even with nothing playing --
/// 1200 of them in twelve seconds, all silent -- so `silence_ticks` comes back
/// zero and the *watchdog* branch of SPEC.md §8.2 never runs here. What does run
/// is the timeline's gap filling, which closes the tens of milliseconds the
/// endpoint leaves between buffers. The watchdog's own path, where the endpoint
/// stops producing entirely, is covered on the CPU tier by
/// `AudioPathTest.ASilentStretchIsEncodedRatherThanSkipped` with a three-second
/// gap that no real endpoint on this machine will produce on demand.
constexpr int kDefaultSeconds = 12;

/// `FC_LOOPBACK_SECONDS` overrides it. M4's exit criterion asks for a 30-minute
/// recording, and this is the only test that makes one against a real endpoint in
/// real time; twelve seconds is what belongs in the routine suite.
///
/// Measured at 1800: 108,000 source frames, 107,999 encoded, audio 1800.021 s
/// against video 1799.983 s, and the endpoint's own clock 26 ms ahead of QPC by
/// the end — 14.4 ppm, absorbed entirely by the timeline.
///
/// Note what the audio-versus-video comparison below is and is not. It is a
/// *duration* check: the audio track outlives the video by the length of the stop
/// sequence, and 38 ms over half an hour is that, not a sync error. Sync is
/// `test_av_sync`'s question, measured against a signal whose position is known,
/// and its answer is −187 µs.
[[nodiscard]] int recording_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
    const char* value = std::getenv("FC_LOOPBACK_SECONDS");
    if (value == nullptr) {
        return kDefaultSeconds;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 2) {
        return kDefaultSeconds;
    }
    return static_cast<int>(std::min<long>(parsed, 4 * 60 * 60));
}

class LoopbackRecordingTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("loopback");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "loopbackrec0001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == AdapterClass::Software || !adapter.can_encode()) {
                continue;
            }
            auto created = fc::test::device_for_adapter(adapter.id);
            if (!created.has_value()) {
                continue;
            }
            device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
            encoder_name_ = adapter.encode.encoder_name;
            pool_bind_flags_ = adapter.encode.nv12_pool_bind_flags;
            break;
        }
        ASSERT_NE(device_, nullptr) << "no adapter reported a usable H.264 encoder";
    }

    void TearDown() override {
        device_.reset();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    std::unique_ptr<fc::gpu::D3dDevice> device_;
    std::string encoder_name_;
    std::uint32_t pool_bind_flags_ = 0;
};

TEST_F(LoopbackRecordingTest, ASilentDesktopStillProducesAFullLengthAudioTrack) {
    const int seconds = recording_seconds();
    const std::filesystem::path output = dir_->path() / "loopback.mkv";

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    PipelineSettings settings;
    settings.output = output;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.encoder_name = encoder_name_;
    settings.pool_bind_flags = pool_bind_flags_;
    // The real endpoint, negotiated format, real silence watchdog.
    settings.audio.source = AudioSource::SystemLoopback;

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    ASSERT_TRUE(started.has_value()) << "pipeline start failed: " << fc::error_name(started.error());

    // Paced against the wall clock, because the audio half is. Feeding frames as
    // fast as the encoder takes them would put a minute of video against twelve
    // seconds of audio and prove nothing about either.
    const auto begin = std::chrono::steady_clock::now();
    const auto until = begin + std::chrono::seconds{seconds};
    int submitted = 0;
    while (std::chrono::steady_clock::now() < until) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        ASSERT_TRUE(frame.has_value()) << "the synthetic source stopped at frame " << submitted;

        // Stamped **now**, as WGC and DDA stamp a frame when they acquire it. The
        // synthetic source's own timestamps are `epoch + index/fps` -- content
        // time, which is what makes the deterministic tests deterministic and what
        // makes it the wrong clock here. If the machine cannot generate the
        // pattern at 60 fps, an index-derived timeline falls behind the wall clock
        // while the audio timeline, stamped by WASAPI from QPC, does not, and the
        // two tracks diverge by the shortfall. An unoptimised build manages about
        // 50 fps, which is two seconds of divergence over twelve.
        //
        // With a capture-time stamp the pacer fills the slots the slow source
        // missed with duplicates (SPEC.md §7.2) and the video timeline stays as
        // long as the recording, which is what a real slow source does too.
        fc::capture::CaptureFrame stamped = frame.value();
        stamped.qpc_ns = static_cast<std::uint64_t>(fc::timing::qpc_now_ns());
        ASSERT_TRUE(pipeline.submit(stamped).has_value());
        source.release(frame.value());
        ++submitted;

        const auto next = begin + (std::chrono::nanoseconds{1'000'000'000} * submitted / kFps);
        std::this_thread::sleep_until(next);
    }
    source.stop();

    const fc::pipeline::AudioStats audio = pipeline.audio_stats();
    const fc::pipeline::PipelineStats video = pipeline.stats();
    const auto report = pipeline.stop();
    ASSERT_TRUE(report.has_value()) << "finalization failed: " << fc::error_name(report.error());

    testing::Test::RecordProperty("seconds", seconds);
    testing::Test::RecordProperty("source_frames", static_cast<int>(video.frames_submitted));
    testing::Test::RecordProperty("duplicates", static_cast<int>(video.duplicates_emitted));
    testing::Test::RecordProperty("encoded", static_cast<int>(video.frames_encoded));
    testing::Test::RecordProperty("awaiting_epoch", static_cast<int>(video.frames_awaiting_epoch));
    testing::Test::RecordProperty("queue_dropped", static_cast<int>(video.frames_queue_dropped));
    testing::Test::RecordProperty("paced_out", static_cast<int>(video.frames_paced_out));
    testing::Test::RecordProperty("encode_failures", static_cast<int>(video.encode_failures));
    testing::Test::RecordProperty("convert_failures", static_cast<int>(video.convert_failures));

    EXPECT_TRUE(report.value().valid) << report.value().detail << " | submitted=" << video.frames_submitted
                                      << " encoded=" << video.frames_encoded
                                      << " awaiting_epoch=" << video.frames_awaiting_epoch
                                      << " queue_dropped=" << video.frames_queue_dropped
                                      << " paced_out=" << video.frames_paced_out
                                      << " convert_failures=" << video.convert_failures
                                      << " encode_failures=" << video.encode_failures;

    // The endpoint was opened and produced its own format.
    EXPECT_GT(audio.buffers_offered, 0u) << "the loopback endpoint delivered nothing at all";

    // SPEC.md §8.2. On a quiet machine almost the whole track is manufactured
    // silence; on one that happens to be playing something it is not. Either way
    // the track has to be as long as the recording, which is the assertion that
    // distinguishes a working silence generator from a missing one.
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.stream_count, 2);
    ASSERT_TRUE(media.audio.present);
    ASSERT_FALSE(media.audio.planes.empty());

    const double audio_seconds =
        static_cast<double>(media.audio.planes.front().size()) / std::max(media.audio.sample_rate, 1);
    const double video_seconds = media.video.times.empty() ? 0.0 : media.video.times.back();

    EXPECT_GT(audio_seconds, seconds * 0.9)
        << "the audio track is " << audio_seconds << " s of a " << seconds
        << " s recording -- the silence generator is not filling the gaps (SPEC.md §8.2)";
    // Scaled, not fixed: half a second is a real bound on a twelve-second
    // recording and a meaningless one on a half-hour recording, where the same
    // proportional error would be seventy-five times larger and still pass.
    const double agreement = std::max(0.5, seconds * 0.01);
    EXPECT_NEAR(audio_seconds, video_seconds, agreement)
        << "audio " << audio_seconds << " s against video " << video_seconds << " s over " << seconds << " s";

    // The shared epoch was negotiated from the `audio` thread's callback, not
    // invented by either stream (SPEC.md §7.1).
    EXPECT_GT(audio.first_packet_qpc_ns, 0) << "no first-packet timestamp reached the session";

    // Nothing was lost to backpressure or to a wedged encoder.
    EXPECT_EQ(audio.encode_failures, 0u);
    EXPECT_EQ(audio.hard_resyncs, 0u);

    testing::Test::RecordProperty("audio_seconds", static_cast<int>(audio_seconds * 1000));
    testing::Test::RecordProperty("video_seconds", static_cast<int>(video_seconds * 1000));
    testing::Test::RecordProperty("silence_frames", static_cast<int>(audio.silence_frames_injected));
    testing::Test::RecordProperty("buffers", static_cast<int>(audio.buffers_offered));
    testing::Test::RecordProperty("silence_ticks", static_cast<int>(audio.silence_ticks));
    testing::Test::RecordProperty("device_vs_qpc_us", static_cast<int>(audio.device_clock_delta_ns / 1000));
}

// A recording with audio configured must not start if the endpoint cannot be
// opened -- and must say so rather than quietly producing a video-only file. The
// silent-failure form of this is a user discovering their recording has no sound
// after the fact.
TEST_F(LoopbackRecordingTest, AnUnopenableEndpointFailsTheStartRatherThanDroppingAudio) {
    PipelineSettings settings;
    settings.output = dir_->path() / "bad_endpoint.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.encoder_name = encoder_name_;
    settings.pool_bind_flags = pool_bind_flags_;
    settings.audio.source = AudioSource::SystemLoopback;
    settings.audio.device_id = "{00000000-0000-0000-0000-000000000000}"; // no such endpoint

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    EXPECT_FALSE(started.has_value()) << "a missing endpoint was accepted";
    if (!started.has_value()) {
        EXPECT_EQ(started.error(), fc::FcError::AUDIO_NO_RENDER_ENDPOINT);
    }
}

} // namespace
