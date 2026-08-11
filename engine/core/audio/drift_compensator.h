#pragma once

// Audio clock drift compensation (SPEC.md §8.4).
//
// The audio device's clock is not exactly 48000.000 Hz and never was. Over a
// long recording a nominal-rate assumption accumulates: at 50 ppm -- an ordinary
// consumer part -- an hour of audio ends up 180 ms away from wall clock, which is
// nine times SPEC.md §20 row 4's tolerance.
//
// The correction is graded, because the cure is more audible than the disease
// until the disease gets large:
//
//   |drift| < 5 ms    nothing. Below audibility and below the measurement's own
//                     noise; correcting here would chase jitter.
//   5-40 ms           soft resync. `swr_set_compensation` stretches or squeezes
//                     over ~10 s, which is inaudible.
//   >= 40 ms          hard resync. Insert or drop at a zero crossing and log it.
//                     SPEC.md §8.4: "this should essentially never fire; if it
//                     does, it is a bug report, not a normal event."
//
// Pure decision function over (frames written, elapsed time). No clock, no
// resampler, no device -- so every band including the one that should never fire
// is reachable in a unit test. SPEC.md §20.1 names drift calculation as a
// unit-test target for exactly this reason.

#include <cstdint>

namespace fc::audio {

/// Where a measured drift falls in SPEC.md §8.4's ladder.
enum class DriftAction {
    /// Within tolerance. Do nothing.
    None,
    /// Correct gradually via `swr_set_compensation`.
    SoftResync,
    /// Beyond what resampling can hide. Insert or drop at a zero crossing.
    HardResync,
};

[[nodiscard]] const char* to_string(DriftAction action) noexcept;

struct DriftDecision {
    DriftAction action = DriftAction::None;

    /// Signed drift. **Positive means audio is ahead** -- more samples written
    /// than wall clock accounts for, i.e. the device clock is running fast.
    std::int64_t drift_ns = 0;

    /// Samples to add (positive) or remove (negative), for
    /// `swr_set_compensation`'s first argument. Zero unless `action` is
    /// `SoftResync`.
    ///
    /// Sign is the opposite of `drift_ns`: audio running ahead is corrected by
    /// producing *fewer* samples.
    int compensation_samples = 0;

    /// Output samples over which to spread the correction --
    /// `swr_set_compensation`'s second argument. Roughly 10 s worth.
    int compensation_distance = 0;

    /// Frames to insert (positive) or drop (negative) at a zero crossing. Zero
    /// unless `action` is `HardResync`.
    std::int64_t hard_correction_frames = 0;
};

/// Evaluates drift on the 1 s cadence SPEC.md §8.4 specifies.
///
/// One per track: Tier B (M9.5) has N tracks with N independent device clocks and
/// therefore N of these, all measured against the one QPC master (SPEC.md §8.6).
class DriftCompensator {
public:
    DriftCompensator() = default;

    explicit DriftCompensator(int sample_rate) noexcept : sample_rate_(sample_rate) {}

    /// Measures drift and returns what to do about it.
    ///
    /// `frames_written` is the audio timeline's length in frames -- silence
    /// included, since injected silence occupies real time and a timeline that
    /// counted only device audio would report the silence itself as drift.
    ///
    /// `elapsed_ns` is **the timeline's elapsed time**: QPC since `t0` with every
    /// paused span excised (SPEC.md §7.5), which is `timing::PauseClock::observe`.
    /// This used to read "QPC time since `t0`", and the two were the same number
    /// until §7.5 gave the timeline a second term -- see BUG-038. The rule the
    /// wording exists to state is that **both arguments must be measured on the
    /// same clock**: `frames_written` counts what reached the encoder, and paused
    /// time never does, so an `elapsed_ns` that still contains it reports the
    /// pause as drift, to the microsecond, and drives the ladder straight into a
    /// band SPEC.md §8.4 says should essentially never fire.
    [[nodiscard]] DriftDecision evaluate(std::int64_t frames_written, std::int64_t elapsed_ns);

    /// Whether enough time has passed to measure again. SPEC.md §8.4 puts this at
    /// 1 s: shorter and the measurement is dominated by buffer granularity, since
    /// a single 20 ms buffer arriving early reads as 20 ms of drift.
    [[nodiscard]] bool due_at(std::int64_t elapsed_ns) const noexcept;

    [[nodiscard]] std::int64_t last_drift_ns() const noexcept {
        return last_drift_ns_;
    }

    [[nodiscard]] std::int64_t worst_drift_ns() const noexcept {
        return worst_drift_ns_;
    }

    [[nodiscard]] std::uint64_t soft_resyncs() const noexcept {
        return soft_resyncs_;
    }

    /// Non-zero here is a bug report, per SPEC.md §8.4.
    [[nodiscard]] std::uint64_t hard_resyncs() const noexcept {
        return hard_resyncs_;
    }

    [[nodiscard]] std::uint64_t evaluations() const noexcept {
        return evaluations_;
    }

private:
    int sample_rate_ = 48000;
    std::int64_t last_evaluated_ns_ = -1;
    std::int64_t last_drift_ns_ = 0;
    std::int64_t worst_drift_ns_ = 0;
    std::uint64_t soft_resyncs_ = 0;
    std::uint64_t hard_resyncs_ = 0;
    std::uint64_t evaluations_ = 0;
};

/// SPEC.md §8.4's bands, exposed so tests assert against the spec's numbers
/// rather than against a copy of them.
inline constexpr std::int64_t kDriftIgnoreNs = 5'000'000;               ///< below this, do nothing
inline constexpr std::int64_t kDriftHardNs = 40'000'000;                ///< at or above this, hard resync
inline constexpr std::int64_t kDriftCadenceNs = 1'000'000'000;          ///< measure once a second
inline constexpr std::int64_t kSoftCorrectionWindowNs = 10'000'000'000; ///< spread soft correction over ~10 s

} // namespace fc::audio
