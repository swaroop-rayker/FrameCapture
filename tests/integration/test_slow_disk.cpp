// SPEC.md §20 row 10 -- "Frame drops": `test_slow_disk`, "throttled I/O, assert
// degradation instead of failure".
//
// GPU TIER.
//
// `BoundedQueue.*` on the CPU tier covers the drop policy and its counters. What it
// cannot cover is the sentence row 10 actually makes, which is about the *whole*
// pipeline: when the disk cannot keep up, the recording must get worse and still
// finish. Those are two claims and both need the real chain.
//
//   degrade  the encode queue overflows, the drops are counted, and SPEC.md §13's
//            ladder engages and says so.
//   not fail `stop()` returns a report, the report says valid, and the file decodes.
//
// The second is the prime directive (CLAUDE.md §1). It is also the one a
// queue-level test can never make, because the failure it guards against is a
// pipeline that decides a slow disk is an error.
//
// ---------------------------------------------------------------------------
// How the disk is throttled
// ---------------------------------------------------------------------------
// `MuxerSettings::injected_stall_ns` sleeps inside `Muxer::write`, on the `mux`
// thread -- the one thread SPEC.md §12 permits to block on disk. SPEC.md §20.1's
// chaos tier names "disk stall" among the injections the suite must perform, so this
// is the sanctioned mechanism rather than a convenience.
//
// It is worth being precise about what is real here, because CLAUDE.md §6 forbids
// simulating behaviour to make a test pass. Nothing is simulated: the delay is real
// elapsed time on the real mux thread, the latency the health monitor reads is
// measured by the same code path a real volume would move, the mux queue fills for
// the same reason, and the encode queue overflows from behind exactly as it would on
// a failing drive. The only thing the injection replaces is the *cause* of the
// slowness. Filling a volume to make it slow is not something a unit of the
// acceptance suite can do to the machine it runs on.
//
// What this cannot cover is stated rather than papered over: rung 6's free-space
// thresholds (2 GB warn, 500 MB stop) are not exercised, because reducing the free
// space on the reference rig to 500 MB is not something a test may do. Those
// thresholds are covered arithmetically on the CPU tier in `test_health_monitor.cpp`.

#include "core/capture/capture_frame.h"
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

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::health::Rung;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

struct Outcome {
    fc::pipeline::PipelineStats stats;
    fc::pipeline::PipelineHealth health;
    fc::mux::ValidationReport report;
    std::filesystem::path path;
    /// The error `stop()` returned, if any. Row 10's whole point is that this stays
    /// empty however slow the disk gets.
    std::optional<fc::FcError> stop_error;
};

class SlowDiskTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("slowdisk");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "slowdisktest01";
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

    static const AdapterInfo* encoding_adapter() {
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software && adapter.can_encode()) {
                return &adapter;
            }
        }
        return nullptr;
    }

    /// Records `frames` frames onto a volume stalled by `stall_ns` on one write in
    /// `period`.
    ///
    /// `paced` feeds at wall-clock 60 fps rather than as fast as the loop manages. That
    /// is what a real recording does, and it matters for more than realism: the
    /// watchdog samples at 4 Hz and the muxer needs 32 writes before it will report a
    /// P99 at all, so a feed loop that finishes in a second leaves the ladder with
    /// nothing to have noticed. Flooding measured a P99 of 0 for exactly that reason.
    static void record(const char* name, int frames, std::int64_t stall_ns, int period, bool paced, Outcome& out) {
        const AdapterInfo* encoder = encoding_adapter();
        ASSERT_NE(encoder, nullptr) << "no adapter reports an H.264 encoder";

        auto device = fc::test::device_for_adapter(encoder->id);
        ASSERT_TRUE(device.has_value());

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);
        // BUG-027's real fix. This case's whole precondition is that the mux thread
        // drains slower than the feeder feeds, and until the frames were pre-rendered
        // the feeder's rate was the term that moved: rendering a 1080p pattern per
        // acquire cannot hold 60 fps on an unoptimised build under load, so the
        // deficit vanished on Debug and the case failed having measured a perfectly
        // good write latency. Nothing here asserts frame identity, so a repeating
        // barcode costs nothing.
        source_settings.prerendered_frames = 8;

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(device.value().device(), fc::capture::CaptureTarget{}).has_value());

        out.path = dir_->path() / (std::string{name} + ".mkv");

        fc::pipeline::PipelineSettings settings;
        settings.output = out.path;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;
        settings.encoder_name = encoder->encode.encoder_name;
        settings.pool_bind_flags = encoder->encode.nv12_pool_bind_flags;
        settings.injected_stall_ns = stall_ns;
        settings.injected_stall_period = period;

        fc::pipeline::VideoPipeline pipeline;
        ASSERT_TRUE(pipeline.start(device.value().device(), settings).has_value());

        const auto feed_started = std::chrono::steady_clock::now();
        int submitted = 0;
        for (;;) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            if (!frame.has_value()) {
                break;
            }
            if (paced) {
                // Wall-clock 60 fps. Sleeping *past* the deadline rather than skipping
                // it: the point is to occupy real time so the watchdog gets to sample,
                // and a feeder that catches up after a stall would defeat that.
                const auto due = feed_started + (std::chrono::microseconds{1'000'000 / kFps} * submitted);
                std::this_thread::sleep_until(due);
            }
            // Never asserted to succeed-with-a-frame: `submit` returning `ok()` having
            // dropped the frame *is* the degradation under test. What must not happen
            // is an error, and that is asserted.
            const fc::Result<void> submission = pipeline.submit(frame.value());
            ASSERT_TRUE(submission.has_value())
                << "submit failed rather than degrading: " << fc::error_name(submission.error());
            ++submitted;
            source.release(frame.value());
        }
        ASSERT_EQ(submitted, frames);

        // Before `stop`: the watchdog is retired inside it, so this is the last look
        // at what the ladder concluded.
        out.health = pipeline.health();

        const auto report = pipeline.stop();
        source.stop();
        out.stats = pipeline.stats();

        if (report.has_value()) {
            out.report = report.value();
        } else {
            out.stop_error = report.error();
        }
    }

    static void report_measurements(const char* label, const Outcome& out, const fc::test::DecodedMedia& media) {
        std::cout << "[ MEASURED ] " << label << ":\n"
                  << "[ MEASURED ]   submitted " << out.stats.frames_submitted << ", encoded "
                  << out.stats.frames_encoded << ", queue-dropped " << out.stats.frames_queue_dropped << ", duplicates "
                  << out.stats.duplicates_emitted << ", paced out " << out.stats.frames_paced_out << "\n"
                  << "[ MEASURED ]   write p99 " << out.health.disk_write_p99_ns / 1000 << " us, rung "
                  << static_cast<int>(out.health.rung) << " (" << fc::health::to_string(out.health.rung)
                  << "), transitions " << out.health.rung_transitions << ", retimes " << out.health.retimes
                  << ", drop ratio " << out.health.drop_ratio << "\n"
                  << "[ MEASURED ]   file: valid " << out.report.valid << ", " << media.video.frame_count
                  << " frames decoded, video " << out.report.video_duration_seconds << " s, "
                  << out.report.decoded_frames << " decoded in the gate\n";
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> SlowDiskTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> SlowDiskTest::topology_;

// ---------------------------------------------------------------------------
// Row 10's claim: degradation, not failure
// ---------------------------------------------------------------------------

TEST_F(SlowDiskTest, AThrottledDiskDegradesTheRecordingInsteadOfFailingIt) {
    Outcome out;
    // 300 ms on one write in four: the mux drains at **13 packets/s**.
    //
    // That number is chosen, not tuned, and BUG-027 records two earlier attempts that
    // were tuned and failed. What has to hold is `drain rate < feed rate`, and the
    // trouble is that the *feed* rate is not a constant either — the synthetic source
    // renders a 1080p BGRA pattern on the CPU per frame, so the feeder manages several
    // hundred fps on Release and far less on Debug. Two build-dependent speeds, and the
    // test asserted on which one won:
    //
    //   50 ms / 4 writes  = 80 packets/s  -- passed on Release, failed on RelWithDebInfo
    //   100 ms / 2 writes = 20 packets/s  -- passed on both, failed on Debug
    //   300 ms / 4 writes = 13 packets/s  -- the rate the rung 6 case has always used,
    //                                        and that case has never failed on any preset
    //
    // 13 packets/s is below what the feeder achieves on the slowest configuration the
    // project builds, which is what makes the deficit a property of the configuration
    // rather than a race. The stall stays **under** rung 6's 500 ms threshold so this case
    // and the next remain about different rungs — asserted below, not just intended.
    ASSERT_NO_FATAL_FAILURE(record("throttled", 300, 300'000'000, 4, /*paced=*/true, out));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(out.path, options, media);

    report_measurements("300 ms stall, one write in four, paced 60 fps feed", out, media);

    // --- not fail ---------------------------------------------------------
    ASSERT_FALSE(out.stop_error.has_value())
        << "the recording failed rather than degrading: " << fc::error_name(*out.stop_error);
    EXPECT_TRUE(out.report.valid) << out.report.detail;
    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_GT(media.video.frame_count, 0) << "the file holds no decodable frames";
    EXPECT_EQ(media.video.codec_name, "h264");

    // --- degrade ----------------------------------------------------------
    // The injection has to have actually bitten, or everything above is vacuous. This
    // is the guard that stops the case passing on a machine where the stall silently
    // did nothing -- which is exactly how it behaved the first time it was written
    // with a gentler period.
    EXPECT_GT(out.health.disk_write_p99_ns, 10'000'000)
        << "write latency never rose; the throttle did not take effect and this case proved nothing";

    // And it must stay *below* rung 6's threshold, so this case and the next one remain
    // about different rungs. A stall that crept past 500 ms would make both cases test
    // rung 6 and leave the queue and frame-drop rungs uncovered.
    EXPECT_LT(out.health.disk_write_p99_ns, fc::health::threshold::kDiskWriteP99Ns)
        << "this case is meant to degrade without engaging rung 6; the stall is too large";

    // Content was shed. Asserted as the *union* of the two ways the pipeline sheds it
    // rather than on queue drops alone, and the reason is a measurement: on Debug this
    // configuration loses 6 frames to the queue and 34 to the pacer, on RelWithDebInfo
    // 160 and 11. Both are the pipeline deliberately shedding work under pressure, and
    // which mechanism dominates depends on how quickly rung 3 retimes -- after the retime
    // a 60 fps source has half its frames paced out *by design*. Asserting on the queue
    // count alone would leave the case riding a 6-frame margin on the slowest build.
    const std::uint64_t shed = out.stats.frames_queue_dropped + out.stats.frames_paced_out;
    EXPECT_GT(shed, 0u) << "nothing was shed, so the pipeline never came under the pressure row 10 is about";
    EXPECT_LT(out.stats.frames_encoded, out.stats.frames_submitted)
        << "every submitted frame was encoded, so the disk never actually held the pipeline up";

    // SPEC.md §13: "Degradation steps are automatic, logged, and surfaced". The ladder
    // must have noticed. Which rung depends on how the pressure lands -- sustained
    // drops are rung 3, the queue itself is 1 and 2 -- so the assertion is that it
    // moved at all, plus the specific action below.
    EXPECT_GT(out.health.rung_transitions, 0u) << "the disk was demonstrably slow and the ladder stayed at rung 0";
    EXPECT_NE(out.health.rung, Rung::Nominal);

    // The rung reached must be one the pressure explains. Rung 3 is the expected landing
    // -- sustained frame drops -- and rungs 1 and 2 are queue occupancy, so any of the
    // three is a coherent answer to a slow disk. What would not be is rung 5 or 7, which
    // would mean the ladder had blamed the encoder or capture for a disk problem.
    EXPECT_NE(out.health.rung, Rung::HardwareEncodersUnavailable);
    EXPECT_NE(out.health.rung, Rung::CaptureFailed);
    EXPECT_TRUE(out.health.hardware_encoder) << "a slow disk must not be mistaken for an encoder problem";
}

// SPEC.md §13 rung 6's first trigger, on the half of it a test may actually cause:
// "disk write latency P99 > 500 ms". The free-space half is covered arithmetically on
// the CPU tier -- see this file's header.
TEST_F(SlowDiskTest, ADiskPastRungSixsLatencyThresholdIsReportedAsDiskPressure) {
    Outcome out;
    // 600 ms is past the 500 ms threshold, on one write in eight so well over 1% of
    // writes stall and the P99 sees it. Fed at wall-clock 60 fps over five seconds,
    // which is what gives the muxer its 32 writes and the watchdog its 20 samples --
    // flooding finished the feed in a second and measured a P99 of 0.
    ASSERT_NO_FATAL_FAILURE(record("rung6", 300, 600'000'000, 8, /*paced=*/true, out));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(out.path, options, media);

    report_measurements("600 ms stall, one write in eight, paced 60 fps feed", out, media);

    // The threshold, measured. Asserted against the engine's own constant rather than
    // a retyped 500 ms.
    EXPECT_GT(out.health.disk_write_p99_ns, fc::health::threshold::kDiskWriteP99Ns)
        << "the injection did not push P99 past rung 6's threshold, so the rung below is not being tested";

    // Rung 6 outranks the queue and frame-drop rungs, so with the latency this high it
    // is the rung that must be reported.
    EXPECT_EQ(out.health.rung, Rung::DiskPressure);
    EXPECT_TRUE(out.health.hardware_encoder) << "a slow disk must not be mistaken for an encoder problem";

    // Rung 6's *first* threshold warns and keeps recording. Only the free-space one
    // stops, and this volume has plenty.
    EXPECT_FALSE(out.health.stop_requested)
        << "a slow disk asked the recording to stop; SPEC.md §13 reserves that for < 500 MB free";

    // And still, the prime directive.
    ASSERT_FALSE(out.stop_error.has_value())
        << "the recording failed rather than degrading: " << fc::error_name(*out.stop_error);
    EXPECT_TRUE(out.report.valid) << out.report.detail;
    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_GT(media.video.frame_count, 0);
}

// The negative control for both cases above. A recording on the same volume with no
// injection must sit at rung 0 -- otherwise "the ladder engaged" proves nothing about
// the disk, only that the ladder engages.
TEST_F(SlowDiskTest, TheSameRecordingOnAnUnthrottledDiskStaysAtRungZero) {
    Outcome out;
    // Paced, exactly like the two cases it controls for. It differs from them in the
    // throttle and in nothing else, which is what makes it a control.
    //
    // It used to flood, and that was only ever harmless by accident: the feeder was too
    // slow to outrun the encoder. Once the frames were pre-rendered the flood became
    // real and the control started dropping 291 of 300 frames -- correct engine
    // behaviour under a drop-oldest queue, and a useless baseline for "an unthrottled
    // recording degrades not at all".
    ASSERT_NO_FATAL_FAILURE(record("nominal", 300, 0, 4, /*paced=*/true, out));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(out.path, options, media);

    report_measurements("no injection", out, media);

    ASSERT_FALSE(out.stop_error.has_value());
    EXPECT_TRUE(out.report.valid) << out.report.detail;
    EXPECT_EQ(out.health.rung, Rung::Nominal)
        << "an unthrottled recording degraded; the ladder's thresholds are too tight to distinguish a slow disk from a "
           "healthy one";
    EXPECT_EQ(out.health.rung_transitions, 0u);
    EXPECT_EQ(out.health.retimes, 0u);
    EXPECT_EQ(out.stats.frames_queue_dropped, 0u);
    EXPECT_LE(out.health.disk_write_p99_ns, fc::health::threshold::kDiskWriteP99Ns);
}

} // namespace
