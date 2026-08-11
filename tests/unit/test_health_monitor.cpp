// The degradation ladder (SPEC.md §13), asserted against a synthetic clock.
//
// CPU TIER. The monitor reads no clock and touches no hardware, which is the whole
// reason it is shaped the way it is: SPEC.md §13's rules are a 3-second window and
// a 30-second hysteresis, and a test that verified those by sleeping would take a
// minute per case and would be rewritten into a sleep-free one by the first person
// who had to run it twice.
//
// Every threshold here is referenced from `fc::health::threshold` rather than
// retyped. A copy of 0.60 in this file would keep passing after someone changed the
// engine's copy to 0.70, which is the failure mode an acceptance test exists to
// prevent.

#include "core/health/health_monitor.h"

#include <gtest/gtest.h>

namespace {

using fc::health::Latch;
using fc::health::Monitor;
using fc::health::Plan;
using fc::health::Rung;
using fc::health::Sample;
namespace threshold = fc::health::threshold;

constexpr int kFps = 60;
constexpr int kCqp = 20;

/// The watchdog's sampling cadence (SPEC.md §12). Every duration below is expressed
/// in these, so a change to the cadence does not silently change what is asserted.
constexpr std::int64_t kTickNs = 250'000'000;

/// Drives a monitor over a synthetic timeline, keeping the cumulative counters that
/// `Sample` requires so a case can say "40 frames, 4 of them dropped" rather than
/// doing the bookkeeping itself.
class Driver {
public:
    Driver() : monitor_(kFps, kCqp) {}

    /// Advances by one tick, submitting `frames` of which `dropped` were lost to
    /// queue pressure, and returns the resulting plan.
    Plan tick(double pressure = 0.0, std::uint64_t frames = 15, std::uint64_t dropped = 0) {
        now_ += kTickNs;
        submitted_ += frames;
        queue_dropped_ += dropped;
        last_frame_ns_ = now_;

        Sample sample;
        sample.now_ns = now_;
        sample.encoder_queue_pressure = pressure;
        sample.frames_submitted = submitted_;
        sample.frames_queue_dropped = queue_dropped_;
        sample.last_frame_ns = last_frame_ns_;
        sample.disk_write_p99_ns = disk_p99_ns_;
        sample.disk_free_bytes = disk_free_bytes_;
        sample.hardware_encoder = hardware_encoder_;
        sample.capture_failed = capture_failed_;
        return monitor_.evaluate(sample);
    }

    /// Ticks for `duration_ns`, holding the arguments steady. Returns the last plan.
    Plan hold(std::int64_t duration_ns, double pressure = 0.0, std::uint64_t frames = 15, std::uint64_t dropped = 0) {
        Plan plan;
        const std::int64_t until = now_ + duration_ns;
        while (now_ < until) {
            plan = tick(pressure, frames, dropped);
        }
        return plan;
    }

    /// Advances time without capture producing a frame -- what a stall looks like
    /// from the watchdog's seat.
    Plan stall(std::int64_t duration_ns) {
        now_ += duration_ns;
        Sample sample;
        sample.now_ns = now_;
        sample.frames_submitted = submitted_;
        sample.frames_queue_dropped = queue_dropped_;
        sample.last_frame_ns = last_frame_ns_; // deliberately not advanced
        sample.disk_free_bytes = disk_free_bytes_;
        sample.hardware_encoder = hardware_encoder_;
        return monitor_.evaluate(sample);
    }

    void set_disk_p99(std::int64_t value) noexcept {
        disk_p99_ns_ = value;
    }

    void set_disk_free(std::uint64_t value) noexcept {
        disk_free_bytes_ = value;
    }

    void set_hardware_encoder(bool value) noexcept {
        hardware_encoder_ = value;
    }

    void fail_capture() noexcept {
        capture_failed_ = true;
    }

    [[nodiscard]] Monitor& monitor() noexcept {
        return monitor_;
    }

    [[nodiscard]] std::int64_t now() const noexcept {
        return now_;
    }

private:
    Monitor monitor_;
    std::int64_t now_ = 1'000'000'000; // deliberately not 0: nothing may assume an epoch
    std::uint64_t submitted_ = 0;
    std::uint64_t queue_dropped_ = 0;
    std::int64_t last_frame_ns_ = 0;
    std::int64_t disk_p99_ns_ = 0;
    std::uint64_t disk_free_bytes_ = UINT64_MAX;
    bool hardware_encoder_ = true;
    bool capture_failed_ = false;
};

// ---------------------------------------------------------------------------
// Latch -- the hysteresis rule, pinned once
// ---------------------------------------------------------------------------

TEST(HealthLatch, EngagesImmediatelyAndReleasesOnlyAfterTheFullHoldOfCleanTime) {
    Latch latch;
    constexpr std::int64_t kHold = threshold::kRecoveryHoldNs;
    constexpr std::int64_t kTrigger = 1'000'000'000;

    latch.update(false, 0, kHold);
    EXPECT_FALSE(latch.engaged());

    latch.update(true, kTrigger, kHold);
    EXPECT_TRUE(latch.engaged()) << "degradation must be immediate; only recovery is delayed";
    EXPECT_EQ(latch.engagements(), 1u);

    // The hold runs from the **first clean observation**, not from the trigger.
    // That is what "30 s of clean operation" says, and the difference is
    // load-bearing: measuring from the trigger would let a condition that stayed
    // active for 29 s release one second after it finally cleared.
    constexpr std::int64_t kFirstClean = kTrigger + 5'000'000'000;
    latch.update(false, kFirstClean, kHold);
    EXPECT_TRUE(latch.engaged());

    // One nanosecond short. The boundary is the point of the rule, so it is
    // asserted rather than approached.
    latch.update(false, kFirstClean + kHold - 1, kHold);
    EXPECT_TRUE(latch.engaged());

    latch.update(false, kFirstClean + kHold, kHold);
    EXPECT_FALSE(latch.engaged());
    EXPECT_EQ(latch.engagements(), 1u);
}

TEST(HealthLatch, ARecurrenceInsideTheHoldRestartsTheCleanPeriodFromScratch) {
    Latch latch;
    constexpr std::int64_t kHold = threshold::kRecoveryHoldNs;

    latch.update(true, 0, kHold);
    // Clean for all but a moment of the hold, then one recurrence.
    latch.update(false, kHold - 1, kHold);
    latch.update(true, kHold, kHold);
    ASSERT_TRUE(latch.engaged());

    // Still one engagement, because it never released. A recurrence inside the hold
    // is the same episode continuing, and counting it twice would make
    // `engagements()` -- the number that says whether a recording oscillated --
    // report noise as oscillation.
    EXPECT_EQ(latch.engagements(), 1u);

    // The clean period restarts from the next clean observation, so the clock being
    // well past the original hold is not enough. Without the restart, a trigger
    // firing once every 29 s would look like a recovered recording.
    constexpr std::int64_t kCleanAgain = kHold + 1;
    latch.update(false, kCleanAgain, kHold);
    latch.update(false, kCleanAgain + kHold - 1, kHold);
    EXPECT_TRUE(latch.engaged());
    latch.update(false, kCleanAgain + kHold, kHold);
    EXPECT_FALSE(latch.engaged());

    // A second, genuinely separate engagement does count.
    latch.update(true, kCleanAgain + kHold + 1, kHold);
    EXPECT_EQ(latch.engagements(), 2u);
}

// ---------------------------------------------------------------------------
// Rung 1 -- "Encoder queue > 60% for 3 s"
// ---------------------------------------------------------------------------

TEST(HealthMonitor, TheQueueMustStayAboveSixtyPercentForThreeSecondsBeforeRungOneEngages) {
    Driver driver;
    constexpr double kOver = threshold::kQueueHigh + 0.05;

    // Just short of the 3 s qualifier.
    Plan plan = driver.hold(threshold::kQueueHighHoldNs - kTickNs, kOver);
    EXPECT_EQ(plan.rung, Rung::Nominal) << "rung 1 fired before its 3 s of evidence";
    EXPECT_EQ(plan.preset_step, 0);

    plan = driver.hold(2 * kTickNs, kOver);
    EXPECT_EQ(plan.rung, Rung::EncoderQueueHigh);
    EXPECT_EQ(plan.preset_step, 1);
}

TEST(HealthMonitor, ABriefQueueSpikeNeverReachesRungOne) {
    Driver driver;
    // Two seconds over, then clean. Short of the qualifier, so nothing engages --
    // this is the case that separates "busy" from "failing".
    Plan plan = driver.hold(2'000'000'000, threshold::kQueueHigh + 0.1);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    plan = driver.hold(2'000'000'000, 0.1);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(driver.monitor().transitions(), 0u);
}

TEST(HealthMonitor, RungOneHoldsForThirtySecondsOfCleanOperationThenReleases) {
    Driver driver;
    Plan plan = driver.hold(threshold::kQueueHighHoldNs + kTickNs, threshold::kQueueHigh + 0.05);
    ASSERT_EQ(plan.rung, Rung::EncoderQueueHigh);

    // Clean, but not yet for long enough.
    plan = driver.hold(threshold::kRecoveryHoldNs - (2 * kTickNs), 0.05);
    EXPECT_EQ(plan.rung, Rung::EncoderQueueHigh) << "recovered before the 30 s hysteresis elapsed";

    plan = driver.hold(3 * kTickNs, 0.05);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(plan.preset_step, 0);
    // Engaged once, released once.
    EXPECT_EQ(driver.monitor().transitions(), 2u);
}

// ---------------------------------------------------------------------------
// Rung 2 -- "Encoder queue > 80%"
// ---------------------------------------------------------------------------

TEST(HealthMonitor, RungTwoFiresOnTheFirstSampleAboveEightyPercentWithNoDurationQualifier) {
    Driver driver;
    const Plan plan = driver.tick(threshold::kQueueCritical + 0.05);
    EXPECT_EQ(plan.rung, Rung::EncoderQueueCritical);
    EXPECT_EQ(plan.target_cqp, kCqp + 4) << "SPEC.md §13 rung 2: CQP 20 -> 24";
}

TEST(HealthMonitor, RungTwoOutranksRungOneWhenBothAreEngaged) {
    Driver driver;
    // Sustained over 80% engages both conditions; the reported rung is the worse.
    const Plan plan = driver.hold(threshold::kQueueHighHoldNs + kTickNs, 0.9);
    EXPECT_EQ(plan.rung, Rung::EncoderQueueCritical);
    EXPECT_EQ(plan.preset_step, 1) << "rung 1's action still applies -- the rungs are not exclusive";
    EXPECT_EQ(plan.target_cqp, kCqp + 4);
}

// ---------------------------------------------------------------------------
// Rung 3 -- "Sustained frame drops > 5%"
// ---------------------------------------------------------------------------

TEST(HealthMonitor, RungThreeHalvesTheCaptureRateWhenDropsExceedFivePercent) {
    Driver driver;
    // 15 frames per tick, 2 dropped -- 13%, comfortably over.
    const Plan plan = driver.hold(threshold::kWindowNs + kTickNs, 0.1, 15, 2);
    EXPECT_EQ(plan.rung, Rung::FrameDropsSustained);
    EXPECT_EQ(plan.target_fps, 30) << "SPEC.md §13 rung 3: halve the capture rate";
    EXPECT_GT(driver.monitor().drop_ratio(), threshold::kFrameDropRatio);
}

TEST(HealthMonitor, ADropRateJustUnderTheThresholdDoesNotHalveTheCaptureRate) {
    Driver driver;
    // 100 frames per tick, 4 dropped -- 4%, under the 5% line. The negative control
    // that stops the previous case from passing on any drop at all.
    const Plan plan = driver.hold(threshold::kWindowNs + (4 * kTickNs), 0.1, 100, 4);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(plan.target_fps, kFps);
    EXPECT_LT(driver.monitor().drop_ratio(), threshold::kFrameDropRatio);
}

TEST(HealthMonitor, AHandfulOfFramesCannotProduceADropRatioAtAll) {
    Driver driver;
    // One frame per tick, every one dropped. That is a 100% drop rate over four
    // frames, and it means nothing -- rung 3 says *sustained*. Without the minimum
    // the first four frames of any recording could halve its frame rate.
    const Plan plan = driver.hold(4 * kTickNs, 0.1, 1, 1);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(plan.target_fps, kFps);
    EXPECT_DOUBLE_EQ(driver.monitor().drop_ratio(), 0.0);
}

TEST(HealthMonitor, FramesThePacerDiscardsAreNotFrameDrops) {
    Driver driver;
    // A 144 Hz source feeding a 60 fps timeline discards most of its frames by
    // design (SPEC.md §7.2). `Sample` carries only queue drops, so a recording that
    // paces out 58% of its input sits at rung 0 -- this pins the contract that
    // makes that true, because the alternative permanently halves the capture rate
    // of the healthiest machines.
    const Plan plan = driver.hold(threshold::kWindowNs + (4 * kTickNs), 0.1, 36, 0);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(plan.target_fps, kFps);
}

// ---------------------------------------------------------------------------
// Rung 4 -- "DEVICE_REMOVED x3 in 60 s"
// ---------------------------------------------------------------------------

TEST(HealthMonitor, TwoDeviceFailuresAreNotEnoughForRungFour) {
    Driver driver;
    driver.monitor().note_device_failure(driver.now());
    driver.monitor().note_device_failure(driver.now());
    const Plan plan = driver.tick();
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_FALSE(plan.request_gpu_migration);
}

TEST(HealthMonitor, ThreeDeviceFailuresInsideSixtySecondsRequestAGpuMigration) {
    Driver driver;
    for (int i = 0; i < 3; ++i) {
        driver.monitor().note_device_failure(driver.now());
        driver.tick();
    }
    const Plan plan = driver.tick();
    EXPECT_EQ(plan.rung, Rung::EncoderDeviceFailing);
    EXPECT_TRUE(plan.request_gpu_migration);
}

TEST(HealthMonitor, ThreeDeviceFailuresSpreadBeyondSixtySecondsDoNotRequestAMigration) {
    Driver driver;
    // Two failures, then more than 60 s, then a third. Three failures in the
    // recording, never three inside the window -- a driver that hiccups once an
    // hour must not trigger a GPU migration.
    driver.monitor().note_device_failure(driver.now());
    driver.tick();
    driver.monitor().note_device_failure(driver.now());
    driver.hold(threshold::kDeviceFailureWindowNs + (2 * kTickNs));
    driver.monitor().note_device_failure(driver.now());
    const Plan plan = driver.tick();
    EXPECT_FALSE(plan.request_gpu_migration);
    EXPECT_EQ(plan.rung, Rung::Nominal);
}

// ---------------------------------------------------------------------------
// Rung 5 -- the software fallback is a state, not a pressure level
// ---------------------------------------------------------------------------

TEST(HealthMonitor, TheSoftwareEncoderReportsRungFiveWithoutHalvingTheCaptureRate) {
    Driver driver;
    driver.set_hardware_encoder(false);
    const Plan plan = driver.hold(threshold::kWindowNs + (4 * kTickNs), 0.1);

    EXPECT_EQ(plan.rung, Rung::HardwareEncodersUnavailable);
    // The trap this exists to catch: deriving rung 3's action from `rung >= 3`
    // would halve the capture rate of every machine with no hardware encoder, for
    // the whole recording, with no frame drops anywhere in sight.
    EXPECT_EQ(plan.target_fps, kFps) << "rung 5 is a fact about the machine, not a pressure level";
    EXPECT_EQ(plan.target_cqp, kCqp);
    EXPECT_EQ(plan.preset_step, 0);
    EXPECT_FALSE(plan.stop_and_finalize);
}

// ---------------------------------------------------------------------------
// Rung 6 -- disk latency and free space
// ---------------------------------------------------------------------------

TEST(HealthMonitor, SlowDiskWarnsAndKeepsRecording) {
    Driver driver;
    driver.set_disk_p99(threshold::kDiskWriteP99Ns + 1);
    const Plan plan = driver.tick();
    EXPECT_EQ(plan.rung, Rung::DiskPressure);
    EXPECT_TRUE(plan.warn_disk_space);
    EXPECT_FALSE(plan.stop_and_finalize) << "a slow disk degrades the recording; it does not end it";
}

TEST(HealthMonitor, LowFreeSpaceWarnsAndCriticallyLowFreeSpaceFinalizes) {
    Driver driver;
    driver.set_disk_free(threshold::kDiskFreeWarnBytes - 1);
    Plan plan = driver.tick();
    EXPECT_EQ(plan.rung, Rung::DiskPressure);
    EXPECT_TRUE(plan.warn_disk_space);
    EXPECT_FALSE(plan.stop_and_finalize);

    driver.set_disk_free(threshold::kDiskFreeStopBytes - 1);
    plan = driver.tick();
    EXPECT_TRUE(plan.stop_and_finalize);
    EXPECT_EQ(plan.rung, Rung::DiskPressure) << "a full disk is rung 6, not a capture failure";
}

TEST(HealthMonitor, AnUnmeasuredFreeSpaceQueryIsNotAnEmptyVolume) {
    Driver driver;
    driver.set_disk_free(UINT64_MAX);
    const Plan plan = driver.hold(4 * kTickNs);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_FALSE(plan.stop_and_finalize) << "a failed free-space query must not stop a healthy recording";
}

TEST(HealthMonitor, TheDecisionToFinalizeIsNeverWithdrawn) {
    Driver driver;
    driver.set_disk_free(threshold::kDiskFreeStopBytes - 1);
    ASSERT_TRUE(driver.tick().stop_and_finalize);

    // Space comes back -- a log rotated, a temp file was cleaned up. The recording
    // does not un-stop: by now the pipeline is finalizing, and a plan that flipped
    // back would ask it to resume writing to a container whose trailer is written.
    driver.set_disk_free(UINT64_MAX);
    const Plan plan = driver.hold(threshold::kRecoveryHoldNs + (4 * kTickNs));
    EXPECT_TRUE(plan.stop_and_finalize);
}

// ---------------------------------------------------------------------------
// Rung 7 -- unrecoverable capture failure
// ---------------------------------------------------------------------------

TEST(HealthMonitor, AnUnrecoverableCaptureFailureIsRungSevenAndFinalizes) {
    Driver driver;
    driver.hold(4 * kTickNs);
    driver.fail_capture();
    const Plan plan = driver.tick();
    EXPECT_EQ(plan.rung, Rung::CaptureFailed);
    EXPECT_TRUE(plan.stop_and_finalize) << "rung 7 finalizes the file; it does not abandon it";
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 9's detector (the recovery half is M7 -- docs/ACCEPTANCE.md)
// ---------------------------------------------------------------------------

TEST(HealthMonitor, NoFrameForThreeFrameIntervalsIsReportedAsAStall) {
    Driver driver;
    driver.hold(4 * kTickNs);

    const std::int64_t interval_ns = 1'000'000'000 / kFps;

    // Two intervals of silence is not a stall: at 60 fps that is 33 ms, which a
    // single scheduling hiccup produces on a healthy machine.
    Plan plan = driver.stall(2 * interval_ns);
    EXPECT_FALSE(plan.capture_stalled);

    // The stall clock measures from the last frame, so this sample is 2 + 2 = 4
    // intervals out and over the line.
    plan = driver.stall(2 * interval_ns);
    EXPECT_TRUE(plan.capture_stalled);
    EXPECT_GT(plan.capture_stall_ns, interval_ns * threshold::kStallFrameIntervals);
    EXPECT_EQ(driver.monitor().stall_episodes(), 1u);
}

TEST(HealthMonitor, AContinuingStallIsOneEpisodeAndNotOnePerSample) {
    Driver driver;
    driver.hold(4 * kTickNs);
    for (int i = 0; i < 8; ++i) {
        driver.stall(500'000'000);
    }
    EXPECT_EQ(driver.monitor().stall_episodes(), 1u)
        << "counting per sample would report a four-second freeze as eight faults";
    EXPECT_GE(driver.monitor().worst_stall_ns(), 4'000'000'000);
}

TEST(HealthMonitor, ARecordingThatHasNotStartedYetIsNotStalled) {
    Driver driver;
    // `last_frame_ns == 0` -- capture has produced nothing. Before the first frame
    // there is no interval to be late against, and reporting a stall here would
    // make every recording begin with one.
    Sample sample;
    sample.now_ns = 5'000'000'000;
    sample.disk_free_bytes = UINT64_MAX;
    const Plan plan = driver.monitor().evaluate(sample);
    EXPECT_FALSE(plan.capture_stalled);
    EXPECT_EQ(driver.monitor().stall_episodes(), 0u);
}

// ---------------------------------------------------------------------------
// Composition and reporting
// ---------------------------------------------------------------------------

TEST(HealthMonitor, ChangedIsTrueOnlyOnTheEvaluationThatMovedTheRung) {
    Driver driver;
    Plan plan = driver.tick(0.9);
    ASSERT_EQ(plan.rung, Rung::EncoderQueueCritical);
    EXPECT_TRUE(plan.changed);

    // Ten more seconds at the same rung must produce no further transitions. This
    // is what keeps a degraded recording from emitting a log line and a GUI event
    // four times a second for its whole duration (CLAUDE.md hard rule 4's spirit:
    // no per-frame logging above TRACE).
    plan = driver.hold(10'000'000'000, 0.9);
    EXPECT_FALSE(plan.changed);
    EXPECT_EQ(driver.monitor().transitions(), 1u);
}

TEST(HealthMonitor, ANominalRecordingStaysAtRungZeroAndPlansNothing) {
    Driver driver;
    const Plan plan = driver.hold(60'000'000'000, 0.2, 15, 0);
    EXPECT_EQ(plan.rung, Rung::Nominal);
    EXPECT_EQ(plan.target_fps, kFps);
    EXPECT_EQ(plan.target_cqp, kCqp);
    EXPECT_EQ(plan.preset_step, 0);
    EXPECT_FALSE(plan.warn_disk_space);
    EXPECT_FALSE(plan.stop_and_finalize);
    EXPECT_FALSE(plan.request_gpu_migration);
    EXPECT_FALSE(plan.capture_stalled);
    EXPECT_EQ(driver.monitor().transitions(), 0u);
}

TEST(HealthMonitor, TheWorstActiveConditionIsTheReportedRung) {
    Driver driver;
    // Disk pressure (6) alongside sustained queue pressure (1 and 2). The reported
    // rung is the worst, and every action still applies independently.
    driver.set_disk_p99(threshold::kDiskWriteP99Ns * 2);
    const Plan plan = driver.hold(threshold::kQueueHighHoldNs + (4 * kTickNs), 0.9, 15, 2);

    EXPECT_EQ(plan.rung, Rung::DiskPressure);
    EXPECT_EQ(plan.preset_step, 1);
    EXPECT_EQ(plan.target_cqp, kCqp + 4);
    EXPECT_EQ(plan.target_fps, 30);
    EXPECT_TRUE(plan.warn_disk_space);
}

TEST(HealthMonitor, RungNamesAreStableAndGreppable) {
    // SPEC.md §18 and §15.1: these reach the log and the GUI status bar, and the
    // acceptance suite matches on them.
    EXPECT_EQ(fc::health::to_string(Rung::Nominal), "nominal");
    EXPECT_EQ(fc::health::to_string(Rung::EncoderQueueHigh), "encoder_queue_high");
    EXPECT_EQ(fc::health::to_string(Rung::FrameDropsSustained), "frame_drops_sustained");
    EXPECT_EQ(fc::health::to_string(Rung::HardwareEncodersUnavailable), "hardware_encoders_unavailable");
    EXPECT_EQ(fc::health::to_string(Rung::DiskPressure), "disk_pressure");
    EXPECT_EQ(fc::health::to_string(Rung::CaptureFailed), "capture_failed");
}

} // namespace
