// The progress model behind M9.6's save bar (SPEC.md §10.4, §15.1).
//
// CPU TIER. `finalize_percent` is a pure function of a phase and a fraction, so what it
// promises can be asserted without a file, a muxer, or a GPU. What it promises is worth
// asserting because a progress bar that goes backwards, or that reaches 100% before the
// recording is safe, is worse than no progress bar at all -- a user who sees "100%" and
// closes the application during the validation gate loses the file the bar was
// reassuring them about.
//
// The end-to-end behaviour -- that the phases actually arrive in this order during a
// real stop, and that `done` follows `recording_finalized` -- belongs to
// `test_finalize_progress` in the GPU tier. This is the half that can be proven here.

#include "core/mux/muxer.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

using fc::mux::finalize_percent;
using fc::mux::FinalizePhase;

constexpr std::array kPhasesInOrder{FinalizePhase::Flushing, FinalizePhase::Remuxing, FinalizePhase::Validating,
                                    FinalizePhase::Swapping, FinalizePhase::Done};

TEST(FinalizePercent, IsMonotonicAcrossThePhaseSequence) {
    // The property the bar depends on: walking the phases in order, at any fraction
    // within each, never produces a smaller number than the phase before it ended at.
    // Bands that overlapped -- or were listed out of order -- would make the bar jump
    // backwards at a phase boundary, which reads as a failure rather than as progress.
    int previous = -1;
    for (const FinalizePhase phase : kPhasesInOrder) {
        for (const double fraction : {0.0, 0.25, 0.5, 0.75, 1.0}) {
            const int percent = finalize_percent(phase, fraction);
            EXPECT_GE(percent, previous) << "phase " << fc::mux::to_string(phase) << " at " << fraction;
            previous = percent;
        }
    }
}

TEST(FinalizePercent, IsMonotonicWithinAPhase) {
    for (const FinalizePhase phase : kPhasesInOrder) {
        int previous = -1;
        for (int step = 0; step <= 100; ++step) {
            const int percent = finalize_percent(phase, static_cast<double>(step) / 100.0);
            EXPECT_GE(percent, previous) << fc::mux::to_string(phase);
            previous = percent;
        }
    }
}

// The whole reason `Done` is emitted by the caller rather than by `finalize_in_place`:
// 100% is the claim that the recording is saved, and nothing but a passed validation
// gate can make that claim. A bar that reached 100 during `Validating` would be telling
// the user a file is safe while the check that decides that is still running.
TEST(FinalizePercent, OnlyDoneReachesOneHundred) {
    for (const FinalizePhase phase : kPhasesInOrder) {
        const int at_end = finalize_percent(phase, 1.0);
        if (phase == FinalizePhase::Done) {
            EXPECT_EQ(at_end, 100);
        } else {
            EXPECT_LT(at_end, 100) << fc::mux::to_string(phase) << " must not claim the recording is saved";
        }
    }
}

TEST(FinalizePercent, StartsAtZeroAndNeverLeavesTheRange) {
    EXPECT_EQ(finalize_percent(FinalizePhase::Flushing, 0.0), 0);
    for (const FinalizePhase phase : kPhasesInOrder) {
        for (const double fraction : {-1.0, 0.0, 0.5, 1.0, 2.0}) {
            const int percent = finalize_percent(phase, fraction);
            EXPECT_GE(percent, 0) << fc::mux::to_string(phase);
            EXPECT_LE(percent, 100) << fc::mux::to_string(phase);
        }
    }
}

// A fraction outside 0..1 is clamped rather than trusted. `avio_tell` past `avio_size`
// is not hypothetical -- libavformat buffers ahead, so the read position can legitimately
// exceed the size the file reported when it was opened.
TEST(FinalizePercent, ClampsAnOutOfRangeFraction) {
    EXPECT_EQ(finalize_percent(FinalizePhase::Remuxing, 2.0), finalize_percent(FinalizePhase::Remuxing, 1.0));
    EXPECT_EQ(finalize_percent(FinalizePhase::Remuxing, -0.5), finalize_percent(FinalizePhase::Remuxing, 0.0));
}

// The remux is the phase that scales with the file, so it gets the majority of the bar.
// Asserted as a relationship rather than as the literal band, so the numbers can be
// retuned without rewriting the test -- what must not change is which phase dominates.
TEST(FinalizePercent, TheRemuxOwnsMostOfTheBar) {
    const int remux_span =
        finalize_percent(FinalizePhase::Remuxing, 1.0) - finalize_percent(FinalizePhase::Remuxing, 0.0);
    const int validate_span =
        finalize_percent(FinalizePhase::Validating, 1.0) - finalize_percent(FinalizePhase::Validating, 0.0);
    EXPECT_GT(remux_span, validate_span);
    EXPECT_GT(remux_span, 50);
}

// Every phase has a distinct wire spelling, because the GUI renders the phase label and
// two phases sharing a name would make one of them unnameable. `unknown` is reserved for
// a value outside the enum, so no real phase may collide with it.
TEST(FinalizePhaseNames, AreDistinctAndNotUnknown) {
    std::array<std::string, kPhasesInOrder.size()> names{};
    for (std::size_t i = 0; i < kPhasesInOrder.size(); ++i) {
        names[i] = std::string{fc::mux::to_string(kPhasesInOrder[i])};
        EXPECT_NE(names[i], "unknown");
        EXPECT_FALSE(names[i].empty());
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_NE(names[i], names[j]) << "two phases share the spelling " << names[i];
        }
    }
}

} // namespace
