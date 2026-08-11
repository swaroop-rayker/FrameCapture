// SPEC.md §20 row 18 — pause/resume desync or freeze.
//
// GPU TIER.
//
//   > `test_pause_resume`: beep + flash on frame boundaries, pause 3x, assert the
//   > decoded file holds exactly the unpaused frame count, A/V offset < 20 ms after
//   > every resume, no duplicate run at a seam, one continuous file.
//
// ---------------------------------------------------------------------------
// Why this is built on one synthetic clock, like row 4
// ---------------------------------------------------------------------------
// §7.5's failure mode is that video and audio subtract *different* paused totals. That
// desync is permanent, silent, and proportional to the pause -- so the way to see it is
// to make both streams carry the same instant by construction and then measure how far
// apart the file puts them. That is exactly row 4's method, so this test borrows it:
// beeps and flashes are generated from one clock, and everything asserted is recovered
// from the finished file.
//
// The one addition is that the clock now has two coordinate systems. **Wall** time
// advances during a pause; **timeline** time does not. Every fed timestamp is wall
// time; every decoded position is timeline time; and the difference between them is the
// quantity §7.5 defines. Both signals are anchored to the timeline -- the source flashes
// once per 60 *emitted* frames and the tone advances only over *emitted* audio -- so
// beep k and flash k are the same instant in the file no matter what the pauses did.
// Anchoring the tone to wall time instead would put beeps and flashes on different
// grids and this test would measure its own harness rather than the pipeline.
//
// ---------------------------------------------------------------------------
// What "no duplicate run at a seam" is asserted on
// ---------------------------------------------------------------------------
// Not on the pacer's counters, which would be the pipeline grading its own homework.
// The synthetic source stamps every frame with a barcode, so the decoded file carries
// the identity of every frame it holds: a frozen-frame outcome is a *repeated barcode*
// at the seam, and that is what is checked, frame by frame, across the whole file.

#include "core/audio/drift_compensator.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_audio.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using fc::pipeline::AudioSource;
using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kPeriodNs = 20'000'000; // SPEC.md §8.1's 20 ms buffer
constexpr std::int64_t kFramesPerBuffer = (kRate * kPeriodNs) / kNsPerSecond;

/// SPEC.md §20 row 18's tolerance, and row 4's: 20 ms.
constexpr double kToleranceSeconds = 0.020;

/// One pause: begin after this many *emitted* frames, and hold for this long in wall
/// clock. Three of them, per row 18.
struct PauseSpan {
    int after_frame;
    std::int64_t duration_ns;
};

/// Deliberately uneven. Equal pauses would let an implementation that subtracted a
/// *constant* per pause pass, and equal placement would let one that excised a fixed
/// fraction of the recording pass. Neither is what §7.5 asks for.
/// A `std::array`, not a `std::vector`: a vector at namespace scope has a throwing
/// initializer that runs before `main` and outside any handler.
constexpr std::array<PauseSpan, 3> kPauses{{
    {90, 1'500'000'000},  // 1.5 s, mid-second
    {210, 400'000'000},   // 0.4 s, shorter than one beep period
    {330, 2'700'000'000}, // 2.7 s, long enough to trip the stall detector if unsuspended
}};

struct Recorded {
    std::filesystem::path output;
    int emitted_frames = 0;
    std::int64_t expected_paused_ns = 0;
    fc::pipeline::PipelineStats stats;
};

class PauseResumeTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("pauseresume");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "pauseresume0001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        // The first adapter that can actually encode. Which one it is does not matter
        // here -- §7.5 is arithmetic on a shared clock and has no adapter-specific
        // behaviour -- so this takes whatever the rig offers rather than pinning one.
        for (const fc::gpu::AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == fc::gpu::AdapterClass::Software || !adapter.can_encode()) {
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

    static void TearDownTestSuite() {
        device_.reset();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Records `total_frames` emitted frames, pausing where `pauses` says.
    ///
    /// `pauses` empty is the unpaused control, and it runs the identical code path --
    /// which is what makes the two comparable.
    static Recorded record(const char* name, int total_frames, std::span<const PauseSpan> pauses) {
        Recorded out;
        out.output = dir_->path() / name;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(total_frames);
        // One flash per 60 *emitted* frames, i.e. one per second of timeline. The source
        // counts what it hands over, and it hands over nothing while paused.
        source_settings.flash_interval_frames = static_cast<std::uint32_t>(kFps);

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value() ||
            !source.start(device_->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "the synthetic source would not start";
            return out;
        }

        PipelineSettings settings;
        settings.output = out.output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;
        settings.audio.source = AudioSource::External;
        settings.audio.channel_layout = fc::config::ChannelLayoutSetting::Stereo;
        settings.audio.buffer_period_ns = kPeriodNs;
        settings.audio.external_format.sample_rate = kRate;
        settings.audio.external_format.channels = kChannels;
        settings.audio.external_format.bits_per_sample = 32;
        settings.audio.external_format.is_float = true;
        settings.audio.external_format.channel_mask = 0x3;

        VideoPipeline pipeline;
        const auto started = pipeline.start(device_->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << "pipeline start failed: " << fc::error_name(started.error());
            return out;
        }

        const std::int64_t epoch = 8'000'000'000LL;

        fc::test::ToneSettings tone;
        tone.sample_rate = kRate;
        tone.channels = kChannels;
        tone.beep_epoch_ns = epoch;

        std::vector<float> scratch;
        std::int64_t emitted_audio_frames = 0; // timeline-anchored; see the header note
        std::int64_t wall_ns = epoch;          // advances during pauses too
        std::int64_t audio_wall_ns = epoch;    // next audio buffer's wall timestamp
        std::size_t next_pause = 0;

        for (int index = 0; index < total_frames; ++index) {
            auto frame = source.acquire(std::chrono::milliseconds{500});
            if (!frame.has_value()) {
                ADD_FAILURE() << "the synthetic source stopped at frame " << index;
                break;
            }

            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns = static_cast<std::uint64_t>(wall_ns);

            // Audio up to this frame's wall timestamp, interleaved as a real recording
            // would deliver it rather than in blocks.
            while (audio_wall_ns <= wall_ns) {
                fc::test::render_tone(tone, emitted_audio_frames, kFramesPerBuffer, scratch);

                fc::audio::LoopbackBuffer buffer;
                buffer.data = reinterpret_cast<const std::uint8_t*>(scratch.data());
                buffer.frames = kFramesPerBuffer;
                buffer.bytes_per_frame = tone.channels * static_cast<int>(sizeof(float));
                buffer.qpc_ns = audio_wall_ns;
                pipeline.offer_audio(buffer);
                pipeline.tick_audio_silence(audio_wall_ns);

                emitted_audio_frames += kFramesPerBuffer;
                audio_wall_ns += kPeriodNs;
            }

            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
            ++out.emitted_frames;
            wall_ns += kNsPerSecond / kFps;

            // A pause boundary. Wall time jumps forward by the pause duration and
            // nothing is fed -- which is what the capture thread and the audio timeline
            // do for real (`RecordingSession::capture_loop`, `AudioTimeline::accept`).
            if (next_pause < pauses.size() && index + 1 == pauses[next_pause].after_frame) {
                const PauseSpan& span = pauses[next_pause];
                ++next_pause;

                if (!pipeline.pause_at(wall_ns).has_value()) {
                    ADD_FAILURE() << "pause_at failed at frame " << index;
                    break;
                }

                wall_ns += span.duration_ns;
                out.expected_paused_ns += span.duration_ns;

                // The silence watchdog is still ticked across the paused span, at the
                // rate it would really run. This is the assertion-by-construction that
                // §7.5's "stop the silence generator" holds: if it did not, these ticks
                // would lengthen the audio timeline by the pause duration and every
                // later beep would be early by it.
                for (std::int64_t t = audio_wall_ns; t <= wall_ns; t += kPeriodNs) {
                    pipeline.tick_audio_silence(t);
                }
                audio_wall_ns = wall_ns;

                if (!pipeline.resume_at(wall_ns).has_value()) {
                    ADD_FAILURE() << "resume_at failed at frame " << index;
                    break;
                }
            }
        }
        source.stop();

        out.stats = pipeline.stats();
        const auto report = pipeline.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << "finalization failed: " << fc::error_name(report.error());
            return out;
        }
        if (!report.value().valid) {
            ADD_FAILURE() << "the output did not validate: " << report.value().detail;
        }
        return out;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::unique_ptr<fc::gpu::D3dDevice> device_;
    static std::string encoder_name_;
    static std::uint32_t pool_bind_flags_;
};

std::unique_ptr<fc::test::TempDir> PauseResumeTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> PauseResumeTest::topology_;
std::unique_ptr<fc::gpu::D3dDevice> PauseResumeTest::device_;
std::string PauseResumeTest::encoder_name_;
std::uint32_t PauseResumeTest::pool_bind_flags_ = 0;

// ---------------------------------------------------------------------------
// Row 18, all five clauses
// ---------------------------------------------------------------------------

TEST_F(PauseResumeTest, ThreePausesExciseTheirTimeFromBothStreamsAndNothingElse) {
    constexpr int kFrames = 420; // 7 s of timeline
    const Recorded recorded = record("pause_resume.mkv", kFrames, kPauses);
    ASSERT_EQ(recorded.emitted_frames, kFrames) << "the feed did not complete";

    // --- clause 5: one continuous file -------------------------------------
    ASSERT_TRUE(std::filesystem::exists(recorded.output));
    fc::test::DecodedMedia media;
    fc::test::decode_media(recorded.output, fc::test::DecodeOptions{}, media);

    // --- clause 1: exactly the unpaused frame count ------------------------
    // The strongest form of §7.5's contract: the paused span "simply is not in it".
    // A frozen-frame implementation would land here with kFrames + the paused frames.
    EXPECT_EQ(media.video.barcodes.size(), static_cast<std::size_t>(kFrames))
        << "the file holds " << media.video.barcodes.size() << " frames where exactly " << kFrames
        << " were submitted; paused time was not excised";

    EXPECT_EQ(media.video.frame_count, static_cast<std::int64_t>(kFrames));

    // The last frame's presentation time is the timeline's length minus one frame. A
    // pause that was not excised would put it `paused_total` later, which at 4.6 s of
    // pause against a 7 s file is not a subtle difference.
    ASSERT_FALSE(media.video.times.empty());
    const double expected_last = static_cast<double>(kFrames - 1) / kFps;
    EXPECT_NEAR(media.video.times.back(), expected_last, 2.0 / kFps)
        << "the last frame presents at " << media.video.times.back() << " s where the unpaused content ends at "
        << expected_last << " s; paused time is still in the timeline";

    // --- clause 4: no duplicate run at a seam ------------------------------
    // Checked on decoded content, not on the pacer's own counters. Every barcode is the
    // identity of the frame that produced it, so a repeat is a frozen frame and a gap
    // is a lost one.
    int unreadable = 0;
    int repeats = 0;
    std::optional<std::uint32_t> previous;
    for (const std::optional<std::uint32_t>& barcode : media.video.barcodes) {
        if (!barcode.has_value()) {
            ++unreadable;
            continue;
        }
        if (previous.has_value() && *barcode == *previous) {
            ++repeats;
        }
        previous = barcode;
    }
    EXPECT_EQ(repeats, 0) << repeats
                          << " frames repeat the barcode before them -- a frozen run at a seam is exactly "
                             "what SPEC.md §7.5 rules out";
    EXPECT_EQ(unreadable, 0) << unreadable << " frames had an unreadable barcode";
    EXPECT_EQ(recorded.stats.duplicates_emitted, 0U)
        << "the pacer emitted " << recorded.stats.duplicates_emitted
        << " duplicates; §7.5: the pause must not be filled with duplicates";

    // --- clause 3: A/V offset < 20 ms after every resume -------------------
    const std::vector<double> flashes = fc::test::flash_times(media.video);
    const std::vector<double> beeps = fc::test::onset_times(media.audio, 0, kRate / 200, 0.15, 0.1);
    ASSERT_GE(flashes.size(), 5U) << "only " << flashes.size() << " flashes decoded";
    ASSERT_GE(beeps.size(), 5U) << "only " << beeps.size() << " beeps decoded";

    const std::size_t pairs = std::min(flashes.size(), beeps.size());
    double worst = 0.0;
    std::size_t worst_at = 0;
    for (std::size_t i = 0; i < pairs; ++i) {
        const double offset = beeps[i] - flashes[i];
        if (std::abs(offset) > std::abs(worst)) {
            worst = offset;
            worst_at = i;
        }
        EXPECT_LT(std::abs(offset), kToleranceSeconds)
            << "mark " << i << " at " << flashes[i] << " s: audio is " << (offset * 1000.0)
            << " ms from video. A growing offset here is video and audio subtracting different paused totals";
    }

    // Asserted after *every* resume rather than only at the end, because a shared-clock
    // mistake shows as a growing offset that a whole-file average hides (see
    // docs/ACCEPTANCE.md, "Row 18's design").
    const double growth = (beeps[pairs - 1] - flashes[pairs - 1]) - (beeps[0] - flashes[0]);
    EXPECT_LT(std::abs(growth), kToleranceSeconds)
        << "the A/V offset grew by " << (growth * 1000.0)
        << " ms across the recording, which is the signature of the two streams excising different amounts";

    // --- clause 2 (the mechanism): the excision was measured, not assumed --
    EXPECT_EQ(recorded.stats.pauses, kPauses.size());
    EXPECT_EQ(recorded.stats.paused_total_ns, recorded.expected_paused_ns)
        << "the engine excised " << (recorded.stats.paused_total_ns / 1'000'000) << " ms where "
        << (recorded.expected_paused_ns / 1'000'000) << " ms were paused";

    // The quiesce held. Non-zero means frames were placed against the wrong paused
    // total -- see `timing::PauseClock`'s header.
    EXPECT_EQ(recorded.stats.pause_stragglers, 0U)
        << recorded.stats.pause_stragglers << " items were mapped against a stale paused total";

    // The stall detector stayed suspended. The 2.7 s pause is 162 frame intervals, far
    // past row 9's three-interval threshold, so an unsuspended detector would have
    // logged episodes here -- and since M7 would have rebuilt the capture session.
    EXPECT_EQ(recorded.stats.frames_excised, 0U)
        << "frames were captured inside a paused span; the feed did not stop when it was told to";

    std::printf("[row 18] %zu frames, %zu pauses totalling %lld ms; worst A/V offset %+.3f ms at mark %zu "
                "(limit %.0f ms), growth %+.3f ms, %d repeats, %llu duplicates, %llu stragglers\n",
                media.video.barcodes.size(), static_cast<std::size_t>(recorded.stats.pauses),
                static_cast<long long>(recorded.stats.paused_total_ns / 1'000'000), worst * 1000.0, worst_at,
                kToleranceSeconds * 1000.0, growth * 1000.0, repeats,
                static_cast<unsigned long long>(recorded.stats.duplicates_emitted),
                static_cast<unsigned long long>(recorded.stats.pause_stragglers));
}

// The control. Without it, an implementation that excised nothing *and* fed nothing
// during the pause would satisfy the frame count above -- the numbers only mean
// something next to a recording of the same content that was never paused.
TEST_F(PauseResumeTest, TheSameRecordingWithoutPausesHasTheSameShape) {
    constexpr int kFrames = 420;
    const Recorded recorded = record("pause_resume_control.mkv", kFrames, {});
    ASSERT_EQ(recorded.emitted_frames, kFrames);

    fc::test::DecodedMedia media;
    fc::test::decode_media(recorded.output, fc::test::DecodeOptions{}, media);

    EXPECT_EQ(media.video.barcodes.size(), static_cast<std::size_t>(kFrames));
    EXPECT_EQ(recorded.stats.paused_total_ns, 0);
    EXPECT_EQ(recorded.stats.pauses, 0U);
    EXPECT_EQ(recorded.stats.frames_excised, 0U);

    const std::vector<double> flashes = fc::test::flash_times(media.video);
    const std::vector<double> beeps = fc::test::onset_times(media.audio, 0, kRate / 200, 0.15, 0.1);
    const std::size_t pairs = std::min(flashes.size(), beeps.size());
    ASSERT_GE(pairs, 5U);

    double worst = 0.0;
    for (std::size_t i = 0; i < pairs; ++i) {
        worst = std::abs(beeps[i] - flashes[i]) > std::abs(worst) ? beeps[i] - flashes[i] : worst;
    }
    EXPECT_LT(std::abs(worst), kToleranceSeconds);

    std::printf("[row 18 control] %zu frames, no pauses, worst A/V offset %+.3f ms\n", media.video.barcodes.size(),
                worst * 1000.0);
}

// SPEC.md §7.5's idempotence invariant, end to end rather than on the clock alone:
// "pausing a paused recording is a no-op that succeeds, not an error", and a recording
// paused when `stop` arrives "finalizes normally and yields a valid file".
TEST_F(PauseResumeTest, RedundantPausesSucceedAndStoppingWhilePausedStillYieldsAValidFile) {
    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.frame_limit = 180;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    PipelineSettings settings;
    settings.output = dir_->path() / "pause_idempotent.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    settings.encoder_name = encoder_name_;
    settings.pool_bind_flags = pool_bind_flags_;

    VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(device_->device(), settings).has_value());

    const std::int64_t epoch = 8'000'000'000LL;
    std::int64_t wall_ns = epoch;

    for (int index = 0; index < 180; ++index) {
        auto frame = source.acquire(std::chrono::milliseconds{500});
        ASSERT_TRUE(frame.has_value()) << "the source stopped at " << index;
        fc::capture::CaptureFrame stamped = frame.value();
        stamped.qpc_ns = static_cast<std::uint64_t>(wall_ns);
        static_cast<void>(pipeline.submit(stamped));
        source.release(frame.value());
        wall_ns += kNsPerSecond / kFps;

        if (index == 89) {
            EXPECT_TRUE(pipeline.pause_at(wall_ns).has_value());
            EXPECT_TRUE(pipeline.paused());
            // Three more, at later instants. None may move the span's start or fail.
            EXPECT_TRUE(pipeline.pause_at(wall_ns + 100'000'000).has_value());
            EXPECT_TRUE(pipeline.pause_at(wall_ns + 500'000'000).has_value());

            wall_ns += 1'000'000'000;
            EXPECT_TRUE(pipeline.resume_at(wall_ns).has_value());
            // And resuming a running recording is equally a no-op that succeeds.
            EXPECT_TRUE(pipeline.resume_at(wall_ns + 100'000'000).has_value());
            EXPECT_FALSE(pipeline.paused());

            EXPECT_EQ(pipeline.paused_total_ns(), 1'000'000'000)
                << "the redundant pauses moved the span's start or the redundant resumes extended it";
        }
    }

    // Stop while paused: SPEC.md §7.5 requires a normal finalize and a valid file.
    EXPECT_TRUE(pipeline.pause_at(wall_ns).has_value());
    source.stop();

    const auto report = pipeline.stop();
    ASSERT_TRUE(report.has_value()) << "finalizing a paused recording failed: " << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << "a recording stopped while paused did not validate: " << report.value().detail;
    EXPECT_GT(report.value().decoded_frames, 0);

    std::printf("[row 18 idempotence] paused_total %lld ms after 3 pauses and 2 resumes; stopped while paused, "
                "file valid with %lld frames\n",
                static_cast<long long>(pipeline.paused_total_ns() / 1'000'000),
                static_cast<long long>(report.value().decoded_frames));
}

// ---------------------------------------------------------------------------
// Row 18 against a real device clock (BUG-038)
// ---------------------------------------------------------------------------
//
// Everything above drives `AudioSource::External`, where both streams come from one
// supplied clock. That path has **no wall-clock drift loop at all**: SPEC.md §8.4's
// ladder measures the encoded track against `qpc - t0`, and when the test supplies both
// quantities there is nothing for a pause to desynchronise between them. The three
// tests above therefore measured a worst A/V offset of −0.188 ms and were correct about
// what they measured, while the live WASAPI path was injecting 97,020 correction frames
// into the same 16-second recording.
//
// This case closes that. `AudioSource::SystemLoopback` runs against the real endpoint at
// wall-clock speed, `pause()` and `resume()` read QPC themselves rather than being handed
// an instant, and §8.4's ladder runs for real on the `aenc` thread. The headline
// assertion is `hard_resyncs == 0` -- SPEC.md §8.4: "this should essentially never fire;
// if it does, it is a bug report, not a normal event."
//
// **The failure this exists to catch is arithmetic, not statistical.** A drift reference
// that still counts paused time reads short by *exactly* the pause, so the assertion
// that names the defect is not the 40 ms band but the pause duration itself: measured
// pre-fix, `worst_drift_us=2021273` against `paused_total_ms=2021`.
//
// Real time, so it is deliberately short. `FC_LOOPBACK_PAUSE_SECONDS` lengthens each
// leg for a longer run.
class LoopbackPauseTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("loopbackpause");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "loopbackpause001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const fc::gpu::AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == fc::gpu::AdapterClass::Software || !adapter.can_encode()) {
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

    /// Seconds per leg. Two legs, one pause between them.
    [[nodiscard]] static int leg_seconds() {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
        const char* value = std::getenv("FC_LOOPBACK_PAUSE_SECONDS");
        if (value == nullptr) {
            return 4;
        }
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return (end == value || parsed < 2) ? 4 : static_cast<int>(std::min<long>(parsed, 30 * 60));
    }

    /// Feeds real-time-stamped frames until `until`, which is what a real capture
    /// backend does and what makes the audio timeline's clock the endpoint's.
    static void feed_until(VideoPipeline& pipeline, fc::test::SyntheticSource& source,
                           std::chrono::steady_clock::time_point until, int& submitted) {
        while (std::chrono::steady_clock::now() < until) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            ASSERT_TRUE(frame.has_value()) << "the synthetic source stopped at frame " << submitted;
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns = static_cast<std::uint64_t>(fc::timing::qpc_now_ns());
            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
            ++submitted;
            std::this_thread::sleep_for(std::chrono::milliseconds{1000 / kFps});
        }
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    std::unique_ptr<fc::gpu::D3dDevice> device_;
    std::string encoder_name_;
    std::uint32_t pool_bind_flags_ = 0;
};

TEST_F(LoopbackPauseTest, PausingARecordingDoesNotDesyncTheAudioTrackFromTheDeviceClock) {
    const int leg = leg_seconds();
    const auto pause_for = std::chrono::seconds{2};
    const std::filesystem::path output = dir_->path() / "loopback_pause.mkv";

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = 1280;
    source_settings.height = 720;
    source_settings.fps = kFps;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    PipelineSettings settings;
    settings.output = output;
    settings.video.width = 1280;
    settings.video.height = 720;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    settings.encoder_name = encoder_name_;
    settings.pool_bind_flags = pool_bind_flags_;
    // The real endpoint. This is the whole point of the case.
    settings.audio.source = AudioSource::SystemLoopback;

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    ASSERT_TRUE(started.has_value()) << "pipeline start failed: " << fc::error_name(started.error());

    int submitted = 0;
    const auto begin = std::chrono::steady_clock::now();
    feed_until(pipeline, source, begin + std::chrono::seconds{leg}, submitted);

    // `pause()`, not `pause_at()` -- the production entry point, reading the clock
    // itself. Nothing is submitted across the span, exactly as capture stops doing.
    //
    // The span is *measured* rather than assumed to be `pause_for`. `sleep_for` is a
    // lower bound, and on a loaded machine it overshoots by however long the scheduler
    // takes to come back -- so an assertion against the nominal 2 s would be grading the
    // harness's sleep, which is not a property of FrameCapture. What the engine owes is
    // that it excised *the pause that actually happened*, and that is what is checked.
    const auto paused_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(pipeline.pause().has_value());
    EXPECT_TRUE(pipeline.paused());
    std::this_thread::sleep_for(pause_for);
    ASSERT_TRUE(pipeline.resume().has_value());
    const auto resumed_at = std::chrono::steady_clock::now();
    EXPECT_FALSE(pipeline.paused());

    const double measured_pause_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(resumed_at - paused_at).count();

    feed_until(pipeline, source, resumed_at + std::chrono::seconds{leg}, submitted);
    const auto fed_until = std::chrono::steady_clock::now();
    source.stop();

    const double measured_wall_s = std::chrono::duration_cast<std::chrono::duration<double>>(fed_until - begin).count();

    const fc::pipeline::AudioStats audio = pipeline.audio_stats();
    const fc::pipeline::PipelineStats video = pipeline.stats();
    const auto report = pipeline.stop();
    ASSERT_TRUE(report.has_value()) << "finalization failed: " << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    // --- the pause happened, and was the length it actually was ---------------
    // Against the measured span, not the nominal one: the engine's own QPC readings at
    // `pause()` and `resume()` should agree with the test's `steady_clock` readings taken
    // immediately around them to within the cost of the two calls.
    EXPECT_EQ(video.pauses, 1U);
    const std::int64_t paused_ns = video.paused_total_ns;
    EXPECT_NEAR(static_cast<double>(paused_ns) / kNsPerSecond, measured_pause_s, 0.1)
        << "the engine excised " << (paused_ns / 1'000'000) << " ms where " << (measured_pause_s * 1000.0)
        << " ms elapsed between the pause and the resume";
    EXPECT_EQ(video.pause_stragglers, 0U);

    // --- SPEC.md §8.4, on a real device clock ---------------------------------
    // The assertion that names the defect: a drift reference that still counts paused
    // time reads short by exactly the pause, so anything approaching the pause duration
    // is that bug and not device drift. Measured pre-fix: 2,021,273 µs against a
    // 2021 ms pause.
    EXPECT_LT(std::abs(audio.worst_drift_ns), paused_ns / 2)
        << "worst drift was " << (audio.worst_drift_ns / 1000) << " µs against a pause of " << (paused_ns / 1000)
        << " µs -- the drift reference is still counting paused time (SPEC.md §7.5)";

    // And the band it is actually supposed to stay in.
    EXPECT_LT(std::abs(audio.worst_drift_ns), fc::audio::kDriftHardNs)
        << "worst drift " << (audio.worst_drift_ns / 1000) << " µs left SPEC.md §8.4's 40 ms band";
    EXPECT_EQ(audio.hard_resyncs, 0U)
        << audio.hard_resyncs
        << " hard resyncs. SPEC.md §8.4: \"this should essentially never fire; if it does, "
           "it is a bug report, not a normal event\"";

    // --- and the file is the length the pause says it should be ---------------
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_TRUE(media.audio.present);
    ASSERT_FALSE(media.audio.planes.empty());

    const double audio_seconds =
        static_cast<double>(media.audio.planes.front().size()) / std::max(media.audio.sample_rate, 1);
    const double video_seconds = media.video.times.empty() ? 0.0 : media.video.times.back();
    // Wall clock minus the pause, both measured. The two legs are wall-deadline loops, so
    // deriving the expectation from the clock rather than from `2 * leg` keeps the
    // assertion about excision rather than about how punctually the loops exited.
    const double expected = measured_wall_s - measured_pause_s;

    // Excision on the real path: the file holds the two legs and not the pause. An
    // unexcised recording would land `pause_for` longer, which at 2 s against 8 s is
    // not a subtle difference.
    EXPECT_NEAR(audio_seconds, expected, 1.0) << "the audio track is " << audio_seconds << " s where " << expected
                                              << " s were recorded outside the paused span";
    EXPECT_NEAR(audio_seconds, video_seconds, std::max(0.5, expected * 0.02))
        << "audio " << audio_seconds << " s against video " << video_seconds << " s";

    testing::Test::RecordProperty("worst_drift_us", static_cast<int>(audio.worst_drift_ns / 1000));
    testing::Test::RecordProperty("paused_ms", static_cast<int>(paused_ns / 1'000'000));
    std::printf(
        "[row 18 loopback] %d frames over 2x%d s with a %lld ms pause; worst drift %+lld µs "
        "(hard band %lld µs), %llu soft / %llu hard resyncs, audio %.3f s vs video %.3f s, "
        "device_vs_qpc %+lld µs\n",
        submitted, leg, static_cast<long long>(paused_ns / 1'000'000),
        static_cast<long long>(audio.worst_drift_ns / 1000), static_cast<long long>(fc::audio::kDriftHardNs / 1000),
        static_cast<unsigned long long>(audio.soft_resyncs), static_cast<unsigned long long>(audio.hard_resyncs),
        audio_seconds, video_seconds, static_cast<long long>(audio.device_clock_delta_ns / 1000));
}

} // namespace
