#include "core/audio/drift_compensator.h"

#include <algorithm>
#include <cstdlib>

namespace fc::audio {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Nanoseconds a frame count represents at `rate`, in integers.
std::int64_t frames_to_ns(std::int64_t frames, int rate) noexcept {
    if (rate <= 0) {
        return 0;
    }
    // Split so the intermediate cannot overflow: `frames * 1e9` passes 2^63 after
    // about 53 minutes of audio at 48 kHz, which a 4-hour soak sails past.
    const std::int64_t whole_seconds = frames / rate;
    const std::int64_t remainder = frames % rate;
    return (whole_seconds * kNsPerSecond) + ((remainder * kNsPerSecond) / rate);
}

std::int64_t ns_to_frames(std::int64_t ns, int rate) noexcept {
    if (rate <= 0) {
        return 0;
    }
    const std::int64_t whole_seconds = ns / kNsPerSecond;
    const std::int64_t remainder = ns % kNsPerSecond;
    return (whole_seconds * rate) + ((remainder * rate) / kNsPerSecond);
}

} // namespace

const char* to_string(DriftAction action) noexcept {
    switch (action) {
    case DriftAction::None:
        return "none";
    case DriftAction::SoftResync:
        return "soft_resync";
    case DriftAction::HardResync:
        return "hard_resync";
    }
    return "unknown";
}

bool DriftCompensator::due_at(std::int64_t elapsed_ns) const noexcept {
    if (last_evaluated_ns_ < 0) {
        return elapsed_ns >= kDriftCadenceNs;
    }
    return (elapsed_ns - last_evaluated_ns_) >= kDriftCadenceNs;
}

DriftDecision DriftCompensator::evaluate(std::int64_t frames_written, std::int64_t elapsed_ns) {
    DriftDecision decision;

    last_evaluated_ns_ = elapsed_ns;
    ++evaluations_;

    // SPEC.md §8.4: drift = (samples / rate) - elapsed. Positive means the audio
    // timeline is longer than wall clock, i.e. the device clock runs fast.
    decision.drift_ns = frames_to_ns(frames_written, sample_rate_) - elapsed_ns;
    last_drift_ns_ = decision.drift_ns;

    const std::int64_t magnitude = std::abs(decision.drift_ns);
    worst_drift_ns_ = std::max(worst_drift_ns_, magnitude);

    if (magnitude < kDriftIgnoreNs) {
        return decision;
    }

    if (magnitude >= kDriftHardNs) {
        decision.action = DriftAction::HardResync;
        // Audio ahead is corrected by dropping frames, so the sign inverts.
        decision.hard_correction_frames = -ns_to_frames(decision.drift_ns, sample_rate_);
        ++hard_resyncs_;
        return decision;
    }

    decision.action = DriftAction::SoftResync;

    // `swr_set_compensation(delta, distance)`: emit `delta` extra samples spread
    // across `distance` output samples. Audio running ahead needs *fewer*
    // samples, hence the negation.
    const std::int64_t correction = -ns_to_frames(decision.drift_ns, sample_rate_);
    const std::int64_t distance = ns_to_frames(kSoftCorrectionWindowNs, sample_rate_);

    // Both are `int` in the libswresample API. The clamp cannot bite at these
    // magnitudes -- 40 ms at 48 kHz is 1920 frames, and the 10 s window is
    // 480000 -- but relying on that silently would make a future rate or window
    // change an overflow rather than a compile error.
    decision.compensation_samples = static_cast<int>(std::clamp<std::int64_t>(correction, INT32_MIN, INT32_MAX));
    decision.compensation_distance = static_cast<int>(std::clamp<std::int64_t>(distance, 1, INT32_MAX));
    ++soft_resyncs_;
    return decision;
}

} // namespace fc::audio
