#pragma once

// The graceful degradation ladder (SPEC.md §13).
//
// Rung 7 is the point of the whole ladder: the recording never dies with an
// unusable file. Everything below it exists so that rung 7 is reached rarely and
// deliberately rather than by collapse.
//
// Deliberately a pure state machine over (Sample, now_ns) with no clock, no
// threading, no queues and no libav types. SPEC.md §20.1 names the health metrics
// as a unit-test target, and a component that reads the clock itself cannot be
// tested deterministically -- a 30 s hysteresis rule verified by sleeping for 30 s
// is a rule nobody re-verifies after they change it. The caller (the `watchdog`
// thread, SPEC.md §12) samples the world and passes it in.
//
// ---------------------------------------------------------------------------
// Two divergences from SPEC.md §13, stated deliberately
// ---------------------------------------------------------------------------
//
// **1. The rung is a severity report, not an index into a switch.**
//
// §13's table reads as one ladder, which invites deriving each action from
// `rung >= N`. That is wrong here, and the bug it produces is not subtle. Rung 5
// ("all HW encoders unavailable") is a persistent *fact* about the machine, not a
// pressure level: a laptop with no working hardware encoder sits at rung 5 for the
// whole recording. Deriving rung 3's action from `rung >= 3` would then halve that
// machine's capture rate to 30 fps permanently, for no reason connected to frame
// drops. So every action is derived from its own trigger, and `Plan::rung` is the
// highest active severity -- what the log line and the GUI status bar show
// (SPEC.md §13, §15.1's `degradation_changed`).
//
// **2. Hysteresis is per condition, not on the composite rung.**
//
// §13 puts the 30 s recovery rule on the ladder. Applied to the composite rung
// alone, the *actions* still flap: a drop ratio hovering either side of 5% toggles
// the capture rate between 60 and 30 fps every window, and halving the frame rate
// twice a second is far more visible than either steady state. The rule is
// therefore applied to each condition -- see `Latch` -- which is what actually
// delivers "prevent oscillation", the reason §13 gives for having the rule.
//
// ---------------------------------------------------------------------------
// What this component decides versus what it can carry out
// ---------------------------------------------------------------------------
// The monitor decides; the pipeline applies; the encoder reports what it managed.
// That seam matters because §13 rungs 1 and 2 name actions the encoders in this
// build cannot perform mid-recording -- see `Plan::preset_step` and
// `Plan::target_cqp`, and BUG-024 in docs/ENGINEERING_LOG.md for the measurement.
// The decision is still computed, logged and tested, so the day an encoder gains
// the capability nothing here changes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fc::health {

/// SPEC.md §13's rung numbers, named by their *trigger* rather than their action.
///
/// The numeric value is the spec's rung number so a log line or status string can
/// print it directly, and so `max` over two active conditions is the more severe
/// one without a lookup table.
enum class Rung : int {
    /// Nothing is wrong.
    Nominal = 0,
    /// Encoder queue above 60% for 3 s.
    EncoderQueueHigh = 1,
    /// Encoder queue above 80%.
    EncoderQueueCritical = 2,
    /// Sustained frame drops above 5%.
    FrameDropsSustained = 3,
    /// Hardware encoder init failure or `DEVICE_REMOVED` three times in 60 s.
    EncoderDeviceFailing = 4,
    /// No hardware encoder available; the recording is on the software fallback.
    HardwareEncodersUnavailable = 5,
    /// Disk write latency P99 above 500 ms, or free space below 2 GB.
    DiskPressure = 6,
    /// Capture has failed unrecoverably. Finalize and report.
    CaptureFailed = 7,
};

[[nodiscard]] std::string_view to_string(Rung rung) noexcept;

/// SPEC.md §13's thresholds, in one place so the test asserts against the same
/// constants the engine runs on rather than a copy that can drift.
namespace threshold {

/// Rung 1: "Encoder queue > 60% for 3 s".
inline constexpr double kQueueHigh = 0.60;
inline constexpr std::int64_t kQueueHighHoldNs = 3'000'000'000;

/// Rung 2: "Encoder queue > 80%". No duration qualifier in the spec, so this fires
/// on the first sample that sees it -- at 80% occupancy the next hitch drops
/// frames, and waiting 3 s to react means dropping them.
inline constexpr double kQueueCritical = 0.80;

/// Rung 3: "Sustained frame drops > 5%", measured over the rolling window.
inline constexpr double kFrameDropRatio = 0.05;

/// SPEC.md §13: "evaluates a rolling 3-second window".
inline constexpr std::int64_t kWindowNs = 3'000'000'000;

/// Rung 4: "DEVICE_REMOVED ×3 in 60 s".
inline constexpr std::size_t kDeviceFailureCount = 3;
inline constexpr std::int64_t kDeviceFailureWindowNs = 60'000'000'000;

/// Rung 6: "Disk write latency P99 > 500 ms or free space < 2 GB ... at < 500 MB
/// free, stop and finalize cleanly".
inline constexpr std::int64_t kDiskWriteP99Ns = 500'000'000;
inline constexpr std::uint64_t kDiskFreeWarnBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t kDiskFreeStopBytes = 500ULL * 1024 * 1024;

/// SPEC.md §13: "only step back up after 30 s of clean operation".
inline constexpr std::int64_t kRecoveryHoldNs = 30'000'000'000;

/// SPEC.md §20 row 9's detector: "no frame in 3 × frame_interval".
inline constexpr int kStallFrameIntervals = 3;

/// Below this many frames in the window the drop *ratio* is noise -- one dropped
/// frame out of four is 25% and means nothing. Rung 3 says "sustained", and a
/// handful of frames is not a sustained anything.
inline constexpr std::uint64_t kMinimumFramesForRatio = 30;

} // namespace threshold

/// One observation of the world, taken by the `watchdog` thread (SPEC.md §12).
///
/// Counters are **cumulative**, not per-window deltas. A caller that reports deltas
/// has to know the monitor's window length, and a single missed sample silently
/// loses events; cumulative totals differenced by the monitor cannot.
struct Sample {
    /// QPC nanoseconds. The same clock as `last_frame_ns` -- SPEC.md §7.1 allows
    /// exactly one clock, and comparing two here would produce a stall detector
    /// that fires on clock skew.
    std::int64_t now_ns = 0;

    /// Encoder input queue occupancy in [0, 1], from `BoundedQueue::pressure()`.
    double encoder_queue_pressure = 0.0;

    /// Frames handed to the pipeline by capture, cumulative.
    std::uint64_t frames_submitted = 0;

    /// Frames lost to **queue pressure**, cumulative.
    ///
    /// Frames the CFR pacer discarded because the source outran the grid are
    /// deliberately *not* counted here. A 144 Hz source feeding a 60 fps timeline
    /// discards 58% of its frames by design (SPEC.md §7.2); folding that into the
    /// drop ratio would hold rung 3 permanently engaged on entirely healthy
    /// hardware and halve the capture rate of the machines least in need of it.
    std::uint64_t frames_queue_dropped = 0;

    /// QPC timestamp of the most recent frame to arrive from capture; 0 before the
    /// first one. Row 9's stall detector compares this against `now_ns`.
    std::int64_t last_frame_ns = 0;

    /// P99 write latency over the mux thread's own window, or 0 before it has
    /// written enough packets to have one (`mux::Muxer::write_latency_p99_ns`).
    std::int64_t disk_write_p99_ns = 0;

    /// Free space on the output volume. `UINT64_MAX` means "not measured", which is
    /// not the same as "plenty" -- a failed query must not read as 0 bytes free and
    /// stop the recording.
    std::uint64_t disk_free_bytes = UINT64_MAX;

    /// False once the recording has fallen to the software encoder (rung 5).
    bool hardware_encoder = true;

    /// Set when capture has failed in a way it cannot recover from (rung 7).
    bool capture_failed = false;

    /// True while the recording is paused (SPEC.md §7.5).
    ///
    /// §7.5: "Suspend the row 9 stall detector and the frame-drop ratio. A paused
    /// recording has no frames by design and must not read as a stall."
    ///
    /// Both suspensions are real defects if omitted, and they are different defects.
    /// The stall detector would fire within three frame intervals of every pause and
    /// -- since M7 -- row 9's recovery would rebuild the capture session underneath a
    /// user who pressed pause, at §5.4's 350 ms budget, repeatedly. The drop ratio
    /// would see submissions stop while the window kept sliding and would engage
    /// rung 3, so a recording resumed after a long pause would come back at 30 fps for
    /// no reason a user could discover.
    bool paused = false;
};

/// What the pipeline should do about it.
///
/// Absolute targets rather than deltas: a caller that applies "one preset step
/// down" repeatedly walks off the end of the ladder, and one that has to remember
/// what it already applied will eventually disagree with the monitor about the
/// current state.
struct Plan {
    /// Highest active severity, for the log line and the GUI status bar.
    Rung rung = Rung::Nominal;

    /// True on the evaluation that changed `rung`. What SPEC.md §15.1's
    /// `degradation_changed` event and the state-change log line hang off, so that
    /// a recording spending ten minutes at rung 1 produces one line and not
    /// thousands.
    bool changed = false;

    /// SPEC.md §13 rung 3: "Halve capture rate to 30 fps, keep the CFR timeline
    /// intact via duplicates". Equal to the configured rate when rung 3 is clear.
    int target_fps = 60;

    /// SPEC.md §13 rung 2: "Drop video quality target (CQP 20 -> 24)".
    ///
    /// **Not applicable on `h264_nvenc` or `h264_amf` in this build** -- neither
    /// exposes a runtime QP change through libavcodec, which SPEC.md §2.2 item 1
    /// requires we go through. Computed, logged and tested regardless; the encoder
    /// reports whether it could honour it. See BUG-024.
    int target_cqp = 20;

    /// SPEC.md §13 rung 1: "Lower encoder preset one step (p5 -> p4)". 0 is the
    /// configured preset, 1 is one step gentler.
    ///
    /// **Not applicable on any encoder in this build** for the same reason as
    /// `target_cqp`: a preset is baked in at `avcodec_open2` and no encoder
    /// reconfigures it per frame. See BUG-024.
    int preset_step = 0;

    /// SPEC.md §13 rung 6's first threshold: warn, keep recording.
    bool warn_disk_space = false;

    /// SPEC.md §13 rung 6's second threshold and rung 7: stop feeding the pipeline
    /// and finalize.
    ///
    /// **This is not an abort.** The file is completed, validated and reported --
    /// CLAUDE.md §1's prime directive is the whole reason the ladder has a rung 7
    /// rather than a crash. Terminal: never withdrawn once set, because a recording
    /// stopped for want of disk space does not resume when a log file rotates.
    bool stop_and_finalize = false;

    /// SPEC.md §13 rung 4: migrate to the alternate GPU (§5.4).
    ///
    /// Reported but not acted on in M6 -- the migration procedure is M7's
    /// deliverable and `test_gpu_migration` is row 11. Surfacing the decision now
    /// is what lets M7 wire an action to an already-tested trigger.
    bool request_gpu_migration = false;

    /// SPEC.md §20 row 9's detector: no frame for 3 x the frame interval.
    ///
    /// Reported, counted and logged in M6. The recovery half -- forcing a capture
    /// session rebuild -- is M7, which is where the session-restart machinery
    /// §5.4 needs is built. See docs/ACCEPTANCE.md row 9.
    bool capture_stalled = false;

    /// How long capture has been silent, when `capture_stalled`. 0 otherwise.
    std::int64_t capture_stall_ns = 0;
};

/// A condition that engages the moment its trigger fires and disengages only after
/// SPEC.md §13's 30 s of clean operation.
///
/// Split out and named because it is the whole of the hysteresis rule, and because
/// per-condition latching is the divergence from §13 explained in this file's
/// header. Exposed rather than hidden in the .cpp so the unit test can pin the
/// rule once instead of inferring it six times through the monitor.
class Latch {
public:
    Latch() = default;

    /// Folds one observation in. `hold_ns` is how long the trigger must stay clear
    /// before this disengages.
    void update(bool triggered, std::int64_t now_ns, std::int64_t hold_ns) noexcept;

    [[nodiscard]] bool engaged() const noexcept {
        return engaged_;
    }

    /// Engagements over the latch's lifetime. The number that says whether a
    /// recording degraded once and recovered or oscillated forty times.
    [[nodiscard]] std::uint64_t engagements() const noexcept {
        return engagements_;
    }

private:
    bool engaged_ = false;
    bool clearing_ = false;
    std::int64_t clear_since_ns_ = 0;
    std::uint64_t engagements_ = 0;
};

/// Evaluates SPEC.md §13's ladder.
///
/// Threading: not thread-safe. Driven from the `watchdog` thread only (SPEC.md
/// §12), which is the thread permitted to block and allowed to allocate.
class Monitor {
public:
    Monitor() = default;

    /// `configured_fps` and `configured_cqp` are the recording's settings, so a
    /// plan can name an absolute target. `configured_fps` also sets the frame
    /// interval the row 9 stall detector measures against.
    Monitor(int configured_fps, int configured_cqp) noexcept;

    /// Records a hardware-encoder init failure or a `DXGI_ERROR_DEVICE_REMOVED`.
    ///
    /// An event rather than a level, so it is reported as it happens rather than
    /// sampled -- three failures inside one 250 ms watchdog interval is exactly the
    /// case rung 4 is about, and a level-based counter would see one.
    void note_device_failure(std::int64_t now_ns);

    /// Folds one observation in and returns the resulting plan.
    [[nodiscard]] Plan evaluate(const Sample& sample);

    [[nodiscard]] Rung rung() const noexcept {
        return rung_;
    }

    /// Rung changes over the recording. Flat is healthy; large means the recording
    /// spent its time oscillating and the hysteresis wants looking at.
    [[nodiscard]] std::uint64_t transitions() const noexcept {
        return transitions_;
    }

    /// Distinct stall episodes seen (SPEC.md §20 row 9). Counted on the transition
    /// into a stall, so a 4 s silence is one episode and not sixteen samples.
    [[nodiscard]] std::uint64_t stall_episodes() const noexcept {
        return stall_episodes_;
    }

    /// Longest single stall, in nanoseconds. The number M7's recovery work has to
    /// beat with its 500 ms budget.
    [[nodiscard]] std::int64_t worst_stall_ns() const noexcept {
        return worst_stall_ns_;
    }

    /// Frame-drop ratio over the most recent window, for the log line and the
    /// stats event. Negative-free: 0 when the window holds too few frames to
    /// have a meaningful ratio.
    [[nodiscard]] double drop_ratio() const noexcept {
        return drop_ratio_;
    }

    /// The latches, for `test_health_monitor` and for the diagnostics dump. Named
    /// individually rather than kept in an array indexed by rung, because two of
    /// §13's rungs have no latch at all and an array would need a hole.
    [[nodiscard]] const Latch& queue_high_latch() const noexcept {
        return queue_high_;
    }

    [[nodiscard]] const Latch& queue_critical_latch() const noexcept {
        return queue_critical_;
    }

    [[nodiscard]] const Latch& frame_drop_latch() const noexcept {
        return frame_drops_;
    }

    [[nodiscard]] const Latch& device_failure_latch() const noexcept {
        return device_failing_;
    }

    [[nodiscard]] const Latch& disk_pressure_latch() const noexcept {
        return disk_pressure_;
    }

private:
    /// Retained samples for the rolling window. At the watchdog's 250 ms cadence
    /// this is 16 s of history against a 3 s window -- and it is a fixed
    /// allocation, because CLAUDE.md hard rule 5 applies to every buffer in the
    /// engine and not only to the ones on the media path.
    static constexpr std::size_t kHistory = 64;

    struct Observation {
        std::int64_t now_ns = 0;
        std::uint64_t frames_submitted = 0;
        std::uint64_t frames_queue_dropped = 0;
    };

    /// Oldest retained observation at or before `now_ns - kWindowNs`, or the oldest
    /// held at all when the history does not reach back that far.
    [[nodiscard]] const Observation* window_start(std::int64_t now_ns) const noexcept;

    void record(const Sample& sample);

    int configured_fps_ = 60;
    int configured_cqp_ = 20;

    std::array<Observation, kHistory> history_{};
    std::size_t history_head_ = 0;
    std::size_t history_size_ = 0;

    /// Timestamps of the last `kDeviceFailureCount` failures. Three slots, because
    /// rung 4's rule only ever asks about the third-most-recent one.
    std::array<std::int64_t, threshold::kDeviceFailureCount> device_failures_{};
    std::size_t device_failure_head_ = 0;
    std::size_t device_failure_count_ = 0;

    /// Last observation at or below the rung 1 threshold, for the "for 3 s"
    /// qualifier. `has_queue_ok_` distinguishes "never been clean" from "clean at
    /// time 0".
    std::int64_t queue_ok_since_ns_ = 0;
    bool has_queue_ok_ = false;

    Latch queue_high_;
    Latch queue_critical_;
    Latch frame_drops_;
    Latch device_failing_;
    Latch disk_pressure_;

    Rung rung_ = Rung::Nominal;
    std::uint64_t transitions_ = 0;
    double drop_ratio_ = 0.0;

    bool stalled_ = false;
    std::uint64_t stall_episodes_ = 0;
    std::int64_t worst_stall_ns_ = 0;

    /// Both terminal once set, and tracked apart so a full disk reports as rung 6
    /// and not as a capture fault (`Plan::stop_and_finalize`).
    bool disk_stop_ = false;
    bool capture_failed_ = false;
};

} // namespace fc::health
