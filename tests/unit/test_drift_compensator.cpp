// Audio clock drift compensation (SPEC.md §8.4, §20 row 4).
//
// CPU TIER. The compensator takes (frames, elapsed) as arguments, so every band
// of the ladder is reachable -- including the hard-resync band that SPEC.md says
// should essentially never fire on real hardware and which therefore could never
// be exercised by a live-device test.

#include "core/audio/drift_compensator.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using fc::audio::DriftAction;
using fc::audio::DriftCompensator;
using fc::audio::DriftDecision;
using fc::audio::kDriftCadenceNs;
using fc::audio::kDriftHardNs;
using fc::audio::kDriftIgnoreNs;

constexpr int kRate = 48000;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Frames that exactly fill `ns` at 48 kHz.
std::int64_t frames_for(std::int64_t ns) {
    return (ns / kNsPerSecond * kRate) + ((ns % kNsPerSecond) * kRate / kNsPerSecond);
}

// ---------------------------------------------------------------------------
// The bands
// ---------------------------------------------------------------------------

TEST(DriftCompensator, APerfectClockNeedsNoCorrection) {
    DriftCompensator compensator(kRate);

    for (std::int64_t second = 1; second <= 60; ++second) {
        const std::int64_t elapsed = second * kNsPerSecond;
        const DriftDecision decision = compensator.evaluate(frames_for(elapsed), elapsed);
        EXPECT_EQ(decision.action, DriftAction::None) << "second " << second;
        EXPECT_EQ(decision.drift_ns, 0) << "second " << second;
    }
    EXPECT_EQ(compensator.soft_resyncs(), 0u);
    EXPECT_EQ(compensator.hard_resyncs(), 0u);
}

// Below 5 ms the correction would be chasing measurement noise, so the ladder's
// first rung is deliberately "do nothing".
TEST(DriftCompensator, DriftInsideToleranceIsIgnored) {
    DriftCompensator compensator(kRate);

    for (const std::int64_t drift : {std::int64_t{0}, kDriftIgnoreNs / 2, kDriftIgnoreNs - 1'000'000}) {
        DriftCompensator fresh(kRate);
        const std::int64_t elapsed = 10 * kNsPerSecond;
        const DriftDecision decision = fresh.evaluate(frames_for(elapsed + drift), elapsed);
        EXPECT_EQ(decision.action, DriftAction::None) << "drift " << drift << " ns";
        EXPECT_EQ(decision.compensation_samples, 0);
    }
}

TEST(DriftCompensator, ModerateDriftTriggersASoftResync) {
    DriftCompensator compensator(kRate);
    const std::int64_t elapsed = 10 * kNsPerSecond;

    // 20 ms ahead: squarely inside the soft band.
    const DriftDecision decision = compensator.evaluate(frames_for(elapsed + 20'000'000), elapsed);

    EXPECT_EQ(decision.action, DriftAction::SoftResync);
    EXPECT_NEAR(static_cast<double>(decision.drift_ns), 20'000'000.0, 100'000.0);

    // Audio is ahead, so the correction removes samples.
    EXPECT_LT(decision.compensation_samples, 0);
    EXPECT_NEAR(static_cast<double>(decision.compensation_samples), -960.0, 5.0); // 20 ms at 48 kHz

    // Spread over ~10 s, which is what makes it inaudible.
    EXPECT_EQ(decision.compensation_distance, 10 * kRate);
    EXPECT_EQ(compensator.soft_resyncs(), 1u);
    EXPECT_EQ(compensator.hard_resyncs(), 0u);
}

// Sign matters and is easy to invert. Audio *behind* wall clock needs more
// samples, not fewer.
TEST(DriftCompensator, ASlowClockIsCorrectedInTheOppositeDirection) {
    DriftCompensator compensator(kRate);
    const std::int64_t elapsed = 10 * kNsPerSecond;

    const DriftDecision decision = compensator.evaluate(frames_for(elapsed - 20'000'000), elapsed);

    EXPECT_EQ(decision.action, DriftAction::SoftResync);
    EXPECT_LT(decision.drift_ns, 0) << "audio behind wall clock should read as negative drift";
    EXPECT_GT(decision.compensation_samples, 0) << "a slow clock needs samples added, not removed";
}

// SPEC.md §8.4: "this should essentially never fire; if it does, it is a bug
// report, not a normal event."
TEST(DriftCompensator, LargeDriftTriggersAHardResync) {
    DriftCompensator compensator(kRate);
    const std::int64_t elapsed = 10 * kNsPerSecond;

    const DriftDecision decision = compensator.evaluate(frames_for(elapsed + 100'000'000), elapsed);

    EXPECT_EQ(decision.action, DriftAction::HardResync);
    EXPECT_LT(decision.hard_correction_frames, 0) << "audio ahead should be corrected by dropping";
    EXPECT_NEAR(static_cast<double>(decision.hard_correction_frames), -4800.0, 10.0); // 100 ms
    EXPECT_EQ(decision.compensation_samples, 0) << "a hard resync does not also ask for soft compensation";
    EXPECT_EQ(compensator.hard_resyncs(), 1u);
}

// The boundaries are load-bearing -- they are the difference between an
// inaudible correction and an audible one.
TEST(DriftCompensator, TheBandBoundariesAreExact) {
    const std::int64_t elapsed = 10 * kNsPerSecond;

    auto action_for = [&](std::int64_t drift_ns) {
        DriftCompensator compensator(kRate);
        return compensator.evaluate(frames_for(elapsed) + frames_for(drift_ns), elapsed).action;
    };

    // Just under 5 ms is ignored; at 5 ms the soft band begins.
    EXPECT_EQ(action_for(kDriftIgnoreNs - 1'000'000), DriftAction::None);
    EXPECT_EQ(action_for(kDriftIgnoreNs + 1'000'000), DriftAction::SoftResync);

    // Just under 40 ms stays soft; at 40 ms it goes hard.
    EXPECT_EQ(action_for(kDriftHardNs - 1'000'000), DriftAction::SoftResync);
    EXPECT_EQ(action_for(kDriftHardNs + 1'000'000), DriftAction::HardResync);
}

// ---------------------------------------------------------------------------
// The cadence
// ---------------------------------------------------------------------------

// Faster than 1 Hz and the measurement is dominated by buffer granularity: a
// single 20 ms buffer arriving early reads as 20 ms of drift, which would trip
// the soft band on a perfectly healthy stream.
TEST(DriftCompensator, MeasurementIsDueOncePerSecond) {
    DriftCompensator compensator(kRate);

    EXPECT_FALSE(compensator.due_at(500'000'000));
    EXPECT_TRUE(compensator.due_at(kDriftCadenceNs));

    static_cast<void>(compensator.evaluate(frames_for(kDriftCadenceNs), kDriftCadenceNs));
    EXPECT_FALSE(compensator.due_at(kDriftCadenceNs + 500'000'000));
    EXPECT_TRUE(compensator.due_at(2 * kDriftCadenceNs));
}

// ---------------------------------------------------------------------------
// Realistic hardware
// ---------------------------------------------------------------------------

// 50 ppm is an ordinary consumer crystal. Left uncorrected it is 180 ms an hour,
// nine times SPEC.md §20 row 4's 20 ms tolerance -- so this is the case the
// subsystem exists for, not a pathological one.
TEST(DriftCompensator, AFiftyPpmClockIsCaughtBeforeItBecomesAudible) {
    DriftCompensator compensator(kRate);

    std::int64_t first_correction_second = -1;
    for (std::int64_t second = 1; second <= 600; ++second) {
        const std::int64_t elapsed = second * kNsPerSecond;
        // Device runs 50 ppm fast, and nothing corrects it yet.
        const std::int64_t frames = frames_for(elapsed) + (frames_for(elapsed) * 50 / 1'000'000);
        const DriftDecision decision = compensator.evaluate(frames, elapsed);
        if (decision.action != DriftAction::None && first_correction_second < 0) {
            first_correction_second = second;
        }
    }

    // 50 ppm reaches 5 ms after 100 s.
    ASSERT_GT(first_correction_second, 0) << "a 50 ppm clock was never corrected";
    EXPECT_NEAR(static_cast<double>(first_correction_second), 100.0, 5.0);

    // And it is caught while still soft -- never allowed to reach the hard band.
    EXPECT_GT(compensator.soft_resyncs(), 0u);
}

// ---------------------------------------------------------------------------
// Arithmetic that has to survive a long recording
// ---------------------------------------------------------------------------

// `frames * 1e9` overflows int64 after roughly 53 minutes at 48 kHz, which a
// 4-hour soak passes four times over. The conversion splits seconds from
// remainder for exactly this reason.
TEST(DriftCompensator, DriftStaysExactOverFourHours) {
    DriftCompensator compensator(kRate);

    constexpr std::int64_t kFourHoursNs = 4LL * 3600 * kNsPerSecond;
    const DriftDecision decision = compensator.evaluate(frames_for(kFourHoursNs), kFourHoursNs);

    EXPECT_EQ(decision.action, DriftAction::None);
    EXPECT_LT(std::abs(decision.drift_ns), 1'000'000) << "drift arithmetic lost precision over four hours";
}

TEST(DriftCompensator, WorstDriftIsRetainedForTelemetry) {
    DriftCompensator compensator(kRate);
    const std::int64_t elapsed = 10 * kNsPerSecond;

    static_cast<void>(compensator.evaluate(frames_for(elapsed + 2'000'000), elapsed));
    static_cast<void>(compensator.evaluate(frames_for(elapsed + 30'000'000), 2 * elapsed));
    static_cast<void>(compensator.evaluate(frames_for(elapsed + 1'000'000), 3 * elapsed));

    // The peak survives, so a recording that misbehaved briefly is still
    // reportable after it recovers (SPEC.md §18).
    EXPECT_GT(compensator.worst_drift_ns(), 20'000'000);
    EXPECT_EQ(compensator.evaluations(), 3u);
}

} // namespace
