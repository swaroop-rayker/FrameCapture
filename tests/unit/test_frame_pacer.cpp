// CFR pacing and PTS quantization (SPEC.md §7.2, §20 row 7, §20.1).
//
// CPU TIER. The pacer takes timestamps as arguments rather than reading a clock,
// so every case here -- a source running slow, fast, or backwards -- is exact and
// reproducible. That is the whole reason it is shaped that way.

#include "core/timing/frame_pacer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <vector>

namespace {

using fc::timing::kVideoTimebaseDen;
using fc::timing::Pacer;
using fc::timing::PacingDecision;
using fc::timing::quantize_index;
using fc::timing::ticks_per_frame;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Nanosecond timestamp of frame `n` from a perfectly-paced `fps` source.
///
/// Rounds rather than truncates. Most rates do not divide a nanosecond evenly --
/// 1e9/144 is 6944444.44 -- so integer division would make every generated tick
/// systematically *early*, by up to a nanosecond and always in the same
/// direction. That bias is invisible to a tolerance check but decides the outcome
/// wherever a tick lands exactly on a rounding boundary, which is precisely where
/// the selection-evenness tests below do their work. A real source's QPC
/// timestamps are the nearest nanosecond, not the floor, so rounding is also the
/// more faithful model.
std::int64_t on_grid(std::int64_t index, int fps) {
    return ((index * kNsPerSecond) + (fps / 2)) / fps;
}

// ---------------------------------------------------------------------------
// The grid itself
// ---------------------------------------------------------------------------

TEST(FramePacer, TimebaseDividesEvenlyAtBothSupportedRates) {
    // SPEC.md §7.2 picks 1/60000 precisely so this holds. If either of these ever
    // has a remainder, PTS deltas stop being identical and the recording judders
    // by a fraction of a frame that accumulates over hours.
    EXPECT_EQ(kVideoTimebaseDen % 60, 0);
    EXPECT_EQ(kVideoTimebaseDen % 30, 0);
    EXPECT_EQ(ticks_per_frame(60), 1000);
    EXPECT_EQ(ticks_per_frame(30), 2000);
}

TEST(FramePacer, QuantizationRoundsToTheNearestGridSlot) {
    constexpr int kFps = 60;
    const std::int64_t slot = kNsPerSecond / kFps; // 16'666'666 ns

    EXPECT_EQ(quantize_index(0, 0, kFps), 0);
    EXPECT_EQ(quantize_index(slot, 0, kFps), 1);

    // Just under and just over the midpoint between slots 0 and 1.
    EXPECT_EQ(quantize_index((slot / 2) - 1, 0, kFps), 0);
    EXPECT_EQ(quantize_index((slot / 2) + (slot / 100), 0, kFps), 1);
}

TEST(FramePacer, QuantizationIsRelativeToT0) {
    constexpr int kFps = 60;
    constexpr std::int64_t kT0 = 987'654'321'000;
    EXPECT_EQ(quantize_index(kT0, kT0, kFps), 0);
    EXPECT_EQ(quantize_index(kT0 + on_grid(42, kFps), kT0, kFps), 42);
}

// A frame stamped before t0 is possible when video and audio negotiate a shared
// epoch (SPEC.md §7.1). A negative PTS is rejected by libavformat, so it clamps.
TEST(FramePacer, TimestampsBeforeT0ClampToZeroRatherThanGoingNegative) {
    EXPECT_EQ(quantize_index(500, 1'000'000'000, 60), 0);
}

// The reason quantize_index is integer arithmetic rather than double. At 4 hours
// the elapsed nanosecond count is ~1.44e13; every index must still be exact.
TEST(FramePacer, QuantizationStaysExactOverAFourHourRecording) {
    constexpr int kFps = 60;
    constexpr std::int64_t kFourHours = 4LL * 3600 * static_cast<std::int64_t>(kFps); // frames

    for (const std::int64_t index : {kFourHours - 1, kFourHours / 2, kFourHours}) {
        EXPECT_EQ(quantize_index(on_grid(index, kFps), 0, kFps), index) << "index " << index;
    }
}

// ---------------------------------------------------------------------------
// The nominal case
// ---------------------------------------------------------------------------

TEST(FramePacer, APerfectlyPacedSourceProducesIdenticalPtsDeltas) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> pts;
    for (std::int64_t i = 0; i < 600; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i, kFps));
        ASSERT_FALSE(decision.drop) << "frame " << i;
        EXPECT_TRUE(decision.duplicate_pts.empty()) << "frame " << i;
        pts.push_back(decision.pts);
    }

    ASSERT_EQ(pts.size(), 600u);
    EXPECT_EQ(pts.front(), 0);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_EQ(pts[i] - pts[i - 1], ticks_per_frame(kFps)) << "delta " << i << " is not one frame";
    }

    EXPECT_EQ(pacer.emitted(), 600u);
    EXPECT_EQ(pacer.duplicated(), 0u);
    EXPECT_EQ(pacer.dropped(), 0u);
}

// SPEC.md §20 row 7's exactness property, at the pacer level: N seconds of a
// well-behaved source yields exactly N*fps frames.
TEST(FramePacer, FrameCountEqualsDurationTimesFps) {
    constexpr int kFps = 60;
    constexpr int kSeconds = 10;
    Pacer pacer(kFps);
    pacer.start(0);

    std::uint64_t total = 0;
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(kSeconds) * kFps; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i, kFps));
        if (!decision.drop) {
            total += 1 + decision.duplicate_pts.size();
        }
    }
    EXPECT_EQ(total, static_cast<std::uint64_t>(kSeconds) * kFps);
}

// ---------------------------------------------------------------------------
// A slow source -- the duplicate path
// ---------------------------------------------------------------------------

TEST(FramePacer, ASlowSourceFillsTheGapWithCorrectlyTimedDuplicates) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    ASSERT_FALSE(pacer.decide(on_grid(0, kFps)).drop);

    // Next frame arrives four slots later: a 12 fps-ish source.
    const PacingDecision decision = pacer.decide(on_grid(4, kFps));
    ASSERT_FALSE(decision.drop);

    // Three duplicates fill slots 1, 2, 3 -- and their PTS are the slot PTS, not
    // the capture time. This is the entire fix for SPEC.md §20 row 7.
    ASSERT_EQ(decision.duplicate_pts.size(), 3u);
    EXPECT_EQ(decision.duplicate_pts[0], 1 * ticks_per_frame(kFps));
    EXPECT_EQ(decision.duplicate_pts[1], 2 * ticks_per_frame(kFps));
    EXPECT_EQ(decision.duplicate_pts[2], 3 * ticks_per_frame(kFps));
    EXPECT_EQ(decision.pts, 4 * ticks_per_frame(kFps));
    EXPECT_EQ(pacer.duplicated(), 3u);
}

TEST(FramePacer, TheTimelineHasNoHolesWhenTheSourceIsSlow) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    // 15 fps source: one frame every four slots, for two seconds.
    std::vector<std::int64_t> pts;
    for (std::int64_t i = 0; i < 30; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i * 4, kFps));
        ASSERT_FALSE(decision.drop);
        for (const std::int64_t duplicate : decision.duplicate_pts) {
            pts.push_back(duplicate);
        }
        pts.push_back(decision.pts);
    }

    // Contiguous, strictly increasing, one frame apart. A player sees 60 fps.
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_EQ(pts[i] - pts[i - 1], ticks_per_frame(kFps)) << "hole at " << i;
    }
    EXPECT_EQ(pts.size(), (30u * 4) - 3);
}

// ---------------------------------------------------------------------------
// A fast source -- the drop path
// ---------------------------------------------------------------------------

TEST(FramePacer, TwoFramesInOneSlotDropsTheSecond) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    ASSERT_FALSE(pacer.decide(on_grid(0, kFps)).drop);

    // A second frame barely later still quantizes to slot 0.
    const PacingDecision decision = pacer.decide(1000);
    EXPECT_TRUE(decision.drop);
    EXPECT_EQ(pacer.dropped(), 1u);
    EXPECT_EQ(pacer.emitted(), 1u);
}

// SPEC.md §7.2: "never emit more than fps frames per second of wall clock, even if
// the source produces 240."
TEST(FramePacer, A240HzSourceIsCappedAtTheConfiguredRate) {
    constexpr int kFps = 60;
    constexpr int kSourceHz = 240;
    constexpr int kSeconds = 10;
    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> pts;
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(kSourceHz) * kSeconds; ++i) {
        const PacingDecision decision = pacer.decide((i * kNsPerSecond) / kSourceHz);
        // A source running *faster* than the grid never leaves a gap, so it must
        // never manufacture a duplicate.
        EXPECT_TRUE(decision.duplicate_pts.empty());
        if (!decision.drop) {
            pts.push_back(decision.pts);
        }
    }

    // Input spans [0, 10) s and the final sample lands at 2399/240 s, which rounds
    // onto slot 600 -- so the emitted set is slots 0..600 inclusive, 601 of them.
    // The endpoint is why this is not a round 600.
    EXPECT_EQ(pacer.emitted(), 601u);
    EXPECT_EQ(pacer.dropped(), (static_cast<std::uint64_t>(kSourceHz) * kSeconds) - 601u);
    EXPECT_EQ(pacer.emitted() + pacer.dropped(), static_cast<std::uint64_t>(kSourceHz) * kSeconds);

    // The cap itself: three quarters of the input was discarded, and what survived
    // is one frame per grid slot with no repeats.
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_EQ(pts[i] - pts[i - 1], ticks_per_frame(kFps));
    }
}

// A non-monotonic capture timestamp must not produce a non-monotonic PTS.
// SPEC.md §7.3: one non-monotonic PTS makes libavformat reject the packet and can
// corrupt the stts table.
TEST(FramePacer, ABackwardTimestampIsDroppedNotEmittedOutOfOrder) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    ASSERT_FALSE(pacer.decide(on_grid(10, kFps)).drop);
    EXPECT_TRUE(pacer.decide(on_grid(3, kFps)).drop);
    EXPECT_TRUE(pacer.decide(on_grid(9, kFps)).drop);
    ASSERT_FALSE(pacer.decide(on_grid(11, kFps)).drop);
}

TEST(FramePacer, EveryEmittedPtsIsStrictlyIncreasingUnderAJitterySource) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    // Deterministic pseudo-jitter around the ideal grid, some of it backwards.
    std::int64_t previous = -1;
    std::uint64_t seed = 12345;
    for (std::int64_t i = 0; i < 2000; ++i) {
        seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto jitter = static_cast<std::int64_t>(seed >> 50) - 4096; // +/- ~4 us
        const PacingDecision decision = pacer.decide(on_grid(i, kFps) + jitter);
        if (decision.drop) {
            continue;
        }
        for (const std::int64_t duplicate : decision.duplicate_pts) {
            EXPECT_GT(duplicate, previous);
            previous = duplicate;
        }
        EXPECT_GT(decision.pts, previous);
        previous = decision.pts;
    }
}

// ---------------------------------------------------------------------------
// Rate change (SPEC.md §13 rung 3, consumed from M6)
// ---------------------------------------------------------------------------

TEST(FramePacer, HalvingTheRateKeepsPtsMonotonicAcrossTheChange) {
    constexpr int kFast = 60;
    Pacer pacer(kFast);
    pacer.start(0);

    std::int64_t last = -1;
    for (std::int64_t i = 0; i < 60; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i, kFast));
        ASSERT_FALSE(decision.drop);
        last = decision.pts;
    }

    pacer.retime(30);

    // Continue in real time from where we were; the grid is coarser now.
    for (std::int64_t i = 1; i <= 30; ++i) {
        const std::int64_t qpc = on_grid(60, kFast) + ((i * kNsPerSecond) / 30);
        const PacingDecision decision = pacer.decide(qpc);
        ASSERT_FALSE(decision.drop) << "frame " << i << " after retime";
        for (const std::int64_t duplicate : decision.duplicate_pts) {
            EXPECT_GT(duplicate, last);
            last = duplicate;
        }
        EXPECT_GT(decision.pts, last) << "PTS went backwards across the rate change";
        last = decision.pts;
    }

    // The timebase is unchanged, so a 30 fps frame is two ticks-per-frame wide.
    EXPECT_EQ(ticks_per_frame(pacer.fps()), 2000);
}

// BUG-025. The SPEC.md §10.4 validation gate needs the recording's length, and it
// used to compute it as `emitted() / configured_fps`. That is correct only while the
// rate is constant, and SPEC.md §13 rung 3 exists to change it -- so the first time
// the ladder halved the capture rate, the gate declared a perfectly good file 30%
// short and the recording was reported as *failed*. A graceful degradation surfacing
// as a corrupt output is the prime directive inverted.
//
// The regression test is the arithmetic itself: one second at 60 fps followed by one
// second at 30 fps emits 91 frames over a timeline that is neither 91/60 s nor
// 91/30 s long. Any single division is wrong, and the old code had to pick one.
TEST(FramePacer, TheTimelineLengthSurvivesARateChangeThatFrameCountsCannotDescribe) {
    constexpr int kFast = 60;
    constexpr int kSlow = 30;
    Pacer pacer(kFast);
    pacer.start(0);

    EXPECT_DOUBLE_EQ(pacer.timeline_seconds(), 0.0) << "a pacer that has emitted nothing has no timeline";

    // One second at 60 fps: slots 0..59, PTS 0..59000.
    for (std::int64_t i = 0; i < kFast; ++i) {
        ASSERT_FALSE(pacer.decide(on_grid(i, kFast)).drop);
    }
    // 60 slots of 1/60 s, the last occupied to its end.
    EXPECT_DOUBLE_EQ(pacer.timeline_seconds(), 1.0);
    EXPECT_EQ(pacer.emitted(), 60u);

    pacer.retime(kSlow);

    // A further second, now at 30 fps. The last frame lands on slot 60, PTS 120000.
    for (std::int64_t i = 1; i <= kSlow; ++i) {
        const std::int64_t qpc = on_grid(kFast, kFast) + ((i * kNsPerSecond) / kSlow);
        ASSERT_FALSE(pacer.decide(qpc).drop) << "frame " << i << " after retime";
    }

    // 120000 ticks plus the final 30 fps slot's 2000, over a 60000 timebase. Written
    // as the tick arithmetic rather than as 2.0333… so there is no float fuzz to
    // tune, and because the ticks are what the file actually carries.
    EXPECT_DOUBLE_EQ(pacer.timeline_seconds(), 122000.0 / 60000.0);

    // 91, not 90, and both surprises are correct:
    //
    //   * the retime leaves the last 60 fps PTS (59000) *between* 30 fps slots 29 and
    //     30, so slot 30 is still free and legitimately earns a duplicate -- without
    //     it there would be a 3000-tick gap on a grid whose frames are 2000 wide;
    //   * the final frame occupies a 1/30 s slot, so the timeline runs 1/30 s past
    //     the two seconds of wall time that produced it.
    EXPECT_EQ(pacer.emitted(), 91u);

    // The point of the accessor, stated so nobody reinstates the division: neither
    // rate divides the frame count into the timeline's length.
    EXPECT_NE(static_cast<double>(pacer.emitted()) / kFast, pacer.timeline_seconds());
    EXPECT_NE(static_cast<double>(pacer.emitted()) / kSlow, pacer.timeline_seconds());
    // And how badly wrong the old form was: 1.52 s claimed against 2.03 s of file,
    // which is a 25% shortfall against a validation gate whose tolerance is 1%.
    EXPECT_LT(static_cast<double>(pacer.emitted()) / kFast, pacer.timeline_seconds() * 0.8);
}

// ---------------------------------------------------------------------------
// Discontinuity reservation (SPEC.md §5.4's migration, consumed from M7)
// ---------------------------------------------------------------------------

TEST(FramePacer, AReservedDiscontinuityIsSkippedByBothTheFrameAndTheDuplicateFill) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    for (std::int64_t i = 0; i < 10; ++i) {
        ASSERT_FALSE(pacer.decide(on_grid(i, kFps)).drop);
    }
    const std::int64_t last_pts = 9 * ticks_per_frame(kFps);

    // Two slots reserved -- SPEC.md §9's `max_b_frames`, the smallest advance that
    // clears a fresh encoder's reorder delay.
    pacer.reserve_discontinuity(2);
    EXPECT_EQ(pacer.reserved(), 2u);

    // The next frame arrives where wall clock says: slot 20.
    const PacingDecision decision = pacer.decide(on_grid(20, kFps));
    ASSERT_FALSE(decision.drop);
    EXPECT_EQ(decision.index, 20);

    // Every PTS it emits -- duplicates first, then the frame -- must clear the reserved
    // window. Slots 10 and 11 are the hole and must appear nowhere.
    const std::int64_t first_allowed = (9 + 2 + 1) * ticks_per_frame(kFps);
    for (const std::int64_t duplicate : decision.duplicate_pts) {
        EXPECT_GE(duplicate, first_allowed)
            << "a duplicate landed inside the reserved window; the new encoder's DTS would precede the old one's";
        EXPECT_GT(duplicate, last_pts);
    }
    EXPECT_GT(decision.pts, last_pts);

    // The hole is exactly two slots wide and does not grow: slots 12..19 are filled.
    EXPECT_EQ(decision.duplicate_pts.size(), 8u);
}

// The property the reservation exists for, stated as the arithmetic rather than as a
// libavformat error. A fresh encoder's first DTS is `PTS - reorder_depth`; the old
// encoder's last DTS equals its last PTS, because flushing drains the reorder pipeline.
TEST(FramePacer, TheReservationIsExactlyLargeEnoughForTheReorderDelay) {
    constexpr int kFps = 60;
    constexpr int kReorderDepth = 2; // SPEC.md §9's max_b_frames
    const std::int64_t ticks = ticks_per_frame(kFps);

    for (const int reserved : {0, 1, 2, 3}) {
        Pacer pacer(kFps);
        pacer.start(0);
        for (std::int64_t i = 0; i < 10; ++i) {
            ASSERT_FALSE(pacer.decide(on_grid(i, kFps)).drop);
        }
        const std::int64_t old_last_dts = 9 * ticks; // == last PTS after a flush

        pacer.reserve_discontinuity(reserved);
        const PacingDecision decision = pacer.decide(on_grid(30, kFps));
        ASSERT_FALSE(decision.drop);

        const std::int64_t first_new_pts =
            decision.duplicate_pts.empty() ? decision.pts : decision.duplicate_pts.front();
        const std::int64_t first_new_dts = first_new_pts - (kReorderDepth * ticks);

        if (reserved >= kReorderDepth) {
            EXPECT_GT(first_new_dts, old_last_dts)
                << "reserving " << reserved << " slots left the first DTS at or below the last one written";
        } else {
            // The negative control: too small a reservation is exactly the rejected
            // case, and it has to still be reproducible or the bound above is folklore.
            EXPECT_LE(first_new_dts, old_last_dts)
                << "reserving " << reserved << " slots unexpectedly cleared the reorder delay; the bound is wrong";
        }
    }
}

TEST(FramePacer, ReservingNothingOrReservingBeforeStartDoesNothing) {
    Pacer pacer(60);
    pacer.reserve_discontinuity(4); // before start
    EXPECT_EQ(pacer.reserved(), 0u);

    pacer.start(0);
    ASSERT_FALSE(pacer.decide(on_grid(0, 60)).drop);
    pacer.reserve_discontinuity(0);
    pacer.reserve_discontinuity(-3);
    EXPECT_EQ(pacer.reserved(), 0u);
    EXPECT_EQ(pacer.decide(on_grid(1, 60)).index, 1) << "a no-op reservation moved the grid";
}

TEST(FramePacer, TheTimelineLengthIsExactAcrossManyRateChanges) {
    Pacer pacer(60);
    pacer.start(0);

    // Alternate rates every half second for five seconds. Each retime re-derives the
    // grid index from the recorded PTS, so any truncation error would compound here
    // and nowhere else.
    std::int64_t qpc = 0;
    for (int block = 0; block < 10; ++block) {
        const int fps = (block % 2 == 0) ? 60 : 30;
        pacer.retime(fps);
        for (int i = 0; i < fps / 2; ++i) {
            qpc += kNsPerSecond / fps;
            ASSERT_FALSE(pacer.decide(qpc).drop) << "block " << block << " frame " << i;
        }
    }

    // Five seconds of wall time, and the timeline must agree to within one frame at
    // the final rate -- not to within a percentage, because the error must not grow
    // with the number of changes.
    EXPECT_NEAR(pacer.timeline_seconds(), 5.0, 1.0 / 30.0);
}

// ---------------------------------------------------------------------------
// Selection evenness
//
// When the source rate is not an integer multiple of the target, some output
// slots must take more source ticks than others. 144 -> 60 reduces to 12:5, so
// over every 12 source ticks the pacer keeps 5 frames with gaps that sum to 12 --
// necessarily a mix of 2s and 3s, since 12/5 = 2.4.
//
// *Which* mix is the whole question. `2,2,3,2,3` spreads the long gaps out;
// `2,2,2,3,3` clumps them into a single visible hitch. Both have five gaps, both
// sum to 12, both give an identical frame count and an identical average -- so no
// aggregate assertion can tell them apart. Ordering is the entire property.
//
// What separates them is *discrepancy*: how far the running total strays from the
// ideal. The even sequence peaks at 0.8 ticks of deviation; the clumped one
// reaches 1.2 by its third gap. Bounding it below 1 is therefore exactly the
// statement "the long gaps never bunch up", and it generalises to any ratio
// without hardcoding 144:60.
//
// This is the low-discrepancy property that makes Bresenham draw a straight line
// rather than a staircase. `quantize_index` is round-to-nearest over a linear
// ramp, which cannot accumulate error in one direction -- so it should hold for
// free. These tests exist to make that guarantee explicit rather than emergent,
// because "for free" survives exactly until someone swaps the rounding for
// truncation.
// ---------------------------------------------------------------------------

/// Deviations must all fall inside a window one tick wide.
///
/// The ideal selection keeps source tick `ceil(r(k - 0.5))` for output slot `k`,
/// and `ceil(x) - x` always lies in `[0, 1)` -- so in exact arithmetic the spread
/// is strictly below 1. Integer-nanosecond timestamps can push a tick that sits
/// exactly on a boundary to the far edge, making the measured value land on 1.0
/// rather than just under it, hence the epsilon.
constexpr double kEvenSpreadBound = 1.0 + 1e-9;

/// Source tick indices the pacer kept, in order.
std::vector<std::int64_t> selection_ticks(int source_hz, int target_fps, int source_ticks) {
    Pacer pacer(target_fps);
    pacer.start(0);

    std::vector<std::int64_t> kept;
    for (std::int64_t i = 0; i < source_ticks; ++i) {
        if (!pacer.decide(on_grid(i, source_hz)).drop) {
            kept.push_back(i);
        }
    }
    return kept;
}

/// Gaps between consecutive kept ticks, skipping the startup pair.
///
/// The first kept tick is tick 0 by construction -- `t0` is defined *by* it -- so
/// it is not a choice the selection rule made, and the gap that follows it is
/// measured from an anchor rather than from a selected position. Including either
/// reports the initial condition as though it were a scheduling decision.
std::vector<std::int64_t> steady_state_gaps(const std::vector<std::int64_t>& kept) {
    std::vector<std::int64_t> gaps;
    for (std::size_t k = 2; k < kept.size(); ++k) {
        gaps.push_back(kept[k] - kept[k - 1]);
    }
    return gaps;
}

/// Width of the band the selection stays inside, in source ticks.
///
/// `max(d) - min(d)` over `d_k = kept[k] - k*ratio`. Spread rather than
/// `max|d|`, because the whole sequence carries a constant offset -- slot 0's
/// ideal tick is negative and gets clamped to 0 -- and an absolute measure would
/// report that fixed offset as though the selection were drifting. Spread is
/// invariant to it and responds only to the ordering, which is the property under
/// test.
///
/// `k = 0` is excluded for the same reason `steady_state_gaps` skips it.
double deviation_spread(const std::vector<std::int64_t>& kept, double ratio) {
    double lowest = 0.0;
    double highest = 0.0;
    bool first = true;
    for (std::size_t k = 1; k < kept.size(); ++k) {
        const double deviation = static_cast<double>(kept[k]) - (static_cast<double>(k) * ratio);
        if (first) {
            lowest = highest = deviation;
            first = false;
        } else {
            lowest = std::min(lowest, deviation);
            highest = std::max(highest, deviation);
        }
    }
    return highest - lowest;
}

/// Every gap is one of the values the ratio permits -- never anything else.
void expect_gaps_within(const std::vector<std::int64_t>& gaps, std::int64_t low, std::int64_t high) {
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        EXPECT_TRUE(gaps[i] >= low && gaps[i] <= high)
            << "gap " << i << " is " << gaps[i] << ", expected " << low << " to " << high;
    }
}

// The reference rig's panel. 144 -> 60 is the awkward ratio: 2.4 source ticks per
// output slot, so neither a clean 2 nor a clean 3.
TEST(FramePacer, A144HzSourceIsSampledEvenlyAt60Fps) {
    constexpr double kRatio = 144.0 / 60.0;

    const auto kept = selection_ticks(144, 60, 1440); // 10 s
    ASSERT_GT(kept.size(), 100u);

    // Only the two lengths the ratio permits. A 1 or a 4 would mean the pacer had
    // skipped or doubled a slot.
    expect_gaps_within(steady_state_gaps(kept), 2, 3);

    EXPECT_LE(deviation_spread(kept, kRatio), kEvenSpreadBound)
        << "the long gaps are bunching; selection is uneven even though the frame count is right";
}

TEST(FramePacer, A144HzSourceIsSampledEvenlyAt30Fps) {
    constexpr double kRatio = 144.0 / 30.0; // 4.8

    const auto kept = selection_ticks(144, 30, 1440);
    ASSERT_GT(kept.size(), 50u);

    expect_gaps_within(steady_state_gaps(kept), 4, 5);
    EXPECT_LE(deviation_spread(kept, kRatio), kEvenSpreadBound);
}

// 165 Hz is the other common high-refresh panel, and 165:60 reduces to 11:4 --
// a different period from 144's 12:5, so it exercises the property rather than
// one lucky sequence.
TEST(FramePacer, A165HzSourceIsSampledEvenlyAt60Fps) {
    constexpr double kRatio = 165.0 / 60.0; // 2.75

    const auto kept = selection_ticks(165, 60, 1650);
    ASSERT_GT(kept.size(), 100u);

    expect_gaps_within(steady_state_gaps(kept), 2, 3);
    EXPECT_LE(deviation_spread(kept, kRatio), kEvenSpreadBound);
}

// An exact *multiple* is the awkward case, which is the opposite of the intuition
// that led to this test.
//
// At 120 -> 60 every second source tick falls exactly on the boundary between two
// output slots: tick 1 sits at 1/120 s, which is precisely 0.5 slots. Whether it
// lands in slot 0 or slot 1 is decided by the last significant digit of a
// nanosecond timestamp -- 1/120 s is 8333333.33 ns and is not representable -- so
// the selection is stable but not uniform. Gaps come out as a repeating 1, 2, 3
// rather than a flat 2.
//
// The consequence is bounded and small: the frame chosen for a slot may be one
// source tick (8.3 ms here) earlier than the slot's centre. Every slot still gets
// exactly one frame, every PTS is still exactly on the grid, and no frame is
// dropped or duplicated -- so this is a content-timing wobble, not the timestamp
// judder of SPEC.md §20 row 7.
//
// It is inherent to picking the *first* tick that falls in a slot, which is what
// an online pacer has to do; choosing the tick nearest the slot centre instead
// would need lookahead and therefore latency. Real capture jitter is microseconds,
// far wider than the sub-nanosecond margin that decides these ties, so on real
// hardware the outcome is arbitrary rather than fragile -- but still bounded to
// one tick, which is what this test pins.
TEST(FramePacer, AnExactMultipleSourceLandsOnSlotBoundariesAndVariesByOneTick) {
    constexpr double kRatio = 2.0;

    const auto kept = selection_ticks(120, 60, 1200);
    ASSERT_GT(kept.size(), 100u);

    // One tick either side of the nominal 2, never further.
    expect_gaps_within(steady_state_gaps(kept), 1, 3);

    // Bounded is the point: the wobble must not accumulate into drift.
    EXPECT_LE(deviation_spread(kept, kRatio), kEvenSpreadBound)
        << "boundary ambiguity is accumulating rather than staying within one tick";

    // And the rate is still exactly right despite the uneven spacing.
    const auto span = static_cast<double>(kept.back() - kept.front());
    EXPECT_NEAR(span / static_cast<double>(kept.size() - 1), kRatio, 0.01);
}

TEST(FramePacer, AMatchedSourceKeepsEveryFrameAndDuplicatesNothing) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    for (std::int64_t i = 0; i < 600; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i, kFps));
        ASSERT_FALSE(decision.drop) << "frame " << i;
        ASSERT_TRUE(decision.duplicate_pts.empty()) << "frame " << i;
    }
    EXPECT_EQ(pacer.dropped(), 0u);
    EXPECT_EQ(pacer.duplicated(), 0u);
}

// The dual case, and the one that matters for a 48 fps source on a 60 fps
// timeline: the pacer duplicates instead of dropping, and the duplicates must be
// spread out rather than arriving two-at-a-time.
TEST(FramePacer, ASlowSourceSpreadsItsDuplicatesEvenly) {
    constexpr int kSourceHz = 48;
    constexpr int kFps = 60;
    constexpr double kRatio = 60.0 / 48.0; // 1.25 output slots per source frame

    Pacer pacer(kFps);
    pacer.start(0);

    // Output slot each source frame landed on. Gaps of 1 mean no duplicate was
    // needed; 2 means one was inserted to keep the timeline contiguous.
    std::vector<std::int64_t> indices;
    for (std::int64_t i = 0; i < 480; ++i) { // 10 s
        const PacingDecision decision = pacer.decide(on_grid(i, kSourceHz));
        ASSERT_FALSE(decision.drop) << "a source slower than the target should never drop";
        indices.push_back(decision.index);
    }

    ASSERT_GT(indices.size(), 100u);
    expect_gaps_within(steady_state_gaps(indices), 1, 2);
    EXPECT_LE(deviation_spread(indices, kRatio), kEvenSpreadBound)
        << "duplicates are arriving in bursts rather than spread across the timeline";

    // One duplicate per four source frames, as 60/48 implies.
    EXPECT_NEAR(static_cast<double>(pacer.duplicated()), 480.0 / 4.0, 2.0);
}

// The assertion has to be able to fail, or it is decoration. BUG-005 was exactly
// a gate that could not fail; this proves `worst_discrepancy` catches the clumped
// arrangement that no aggregate check would.
TEST(FramePacer, TheEvennessCheckRejectsAClumpedSelection) {
    constexpr double kRatio = 2.4;

    auto ticks_from = [](const std::vector<std::int64_t>& gaps) {
        std::vector<std::int64_t> ticks{0};
        for (const std::int64_t gap : gaps) {
            ticks.push_back(ticks.back() + gap);
        }
        return ticks;
    };

    // Both are five gaps summing to 12 -- identical frame count, identical
    // average, identical total duration, identical multiset of gap lengths. Only
    // the ordering differs, so nothing but an order-sensitive measure can separate
    // them.
    const std::vector<std::int64_t> even{2, 2, 3, 2, 3};
    const std::vector<std::int64_t> clumped{2, 2, 2, 3, 3};

    ASSERT_EQ(std::accumulate(even.begin(), even.end(), std::int64_t{0}),
              std::accumulate(clumped.begin(), clumped.end(), std::int64_t{0}));

    EXPECT_LE(deviation_spread(ticks_from(even), kRatio), kEvenSpreadBound);
    EXPECT_GT(deviation_spread(ticks_from(clumped), kRatio), kEvenSpreadBound)
        << "the check cannot distinguish a clumped selection from an even one, so it proves nothing";
}

// Real capture timestamps are not a metronome: WGC delivers on content change and
// the OS adds scheduling jitter. The bound has to degrade gracefully rather than
// collapse, or it only ever describes an idealised source.
TEST(FramePacer, EvennessDegradesGracefullyUnderAJitterySource) {
    constexpr int kSourceHz = 144;
    constexpr int kFps = 60;
    constexpr double kRatio = 144.0 / 60.0;

    // +/- ~1 ms, which is well beyond what a healthy capture path shows and is
    // a sixth of a 144 Hz frame interval.
    constexpr std::int64_t kJitterNs = 1'000'000;

    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> kept;
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;

    for (std::int64_t i = 0; i < 1440; ++i) {
        seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
        const auto jitter = (static_cast<std::int64_t>(seed >> 33) % (2 * kJitterNs)) - kJitterNs;

        if (!pacer.decide(on_grid(i, kSourceHz) + jitter).drop) {
            kept.push_back(i);
        }
    }

    ASSERT_GT(kept.size(), 100u);

    // Jitter can carry a tick across a slot boundary, so a gap of 1 or 4 becomes
    // reachable -- but the selection must not wander further than one tick either
    // side of the 2/3 it would otherwise produce.
    expect_gaps_within(steady_state_gaps(kept), 1, 4);

    // The band widens under jitter but must stay a band. A pacer that let error
    // accumulate would show a spread that grows with the length of the run.
    const double spread = deviation_spread(kept, kRatio);
    EXPECT_LT(spread, 2.0) << "jitter is accumulating rather than averaging out";

    // Half the run should not be measurably worse than the whole: that is what
    // distinguishes a bounded wobble from drift.
    const auto midpoint = static_cast<std::ptrdiff_t>(kept.size() / 2);
    const std::vector<std::int64_t> first_half(kept.begin(), kept.begin() + midpoint);
    EXPECT_LT(deviation_spread(first_half, kRatio), spread + 0.5)
        << "the deviation band is widening as the run goes on";
}

// ---------------------------------------------------------------------------
// Variable source rate
//
// The realistic case, and the one none of the tests above cover: a game whose
// frame rate wanders -- 82, then 97, then 110 -- rather than sitting on a clean
// multiple of the target. Under VRR the panel refreshes at the game's rate, so
// the capture timestamps wander with it.
//
// The evenness metric used above compares against `k * ratio` and is meaningless
// when the ratio is not constant. What generalises is *staleness*: how far the
// content shown in a slot lags that slot's nominal time. It is defined for any
// input pattern, it is what actually degrades when the source misbehaves, and it
// is computable from what the test feeds in and gets back -- so this stays a
// pure-function test with no clock and no GPU.
//
// The invariants that must survive regardless of how erratic the source is:
// PTS exactly on the grid, strictly increasing, timeline contiguous, and the
// emitted count equal to duration x fps. Those are guaranteed by construction --
// `quantize_index` is absolute from `t0` and never accumulates -- and these tests
// exist to keep it that way.
// ---------------------------------------------------------------------------

/// Half a slot: the bound `|staleness_ns|` cannot exceed by construction, since a
/// timestamp further out than that quantizes to a different slot.
std::int64_t half_slot_ns(int fps) {
    return kNsPerSecond / (2 * static_cast<std::int64_t>(fps));
}

/// Runs a scripted timestamp sequence through the pacer and checks the invariants
/// that hold for *any* input. Returns the emitted PTS in order.
std::vector<std::int64_t> run_and_check_invariants(Pacer& pacer, const std::vector<std::int64_t>& timestamps, int fps) {
    std::vector<std::int64_t> pts;
    std::int64_t previous = -1;

    for (const std::int64_t timestamp : timestamps) {
        const PacingDecision decision = pacer.decide(timestamp);
        if (decision.drop) {
            continue;
        }

        for (const std::int64_t duplicate : decision.duplicate_pts) {
            EXPECT_GT(duplicate, previous) << "duplicate PTS went backwards";
            EXPECT_EQ(duplicate % ticks_per_frame(fps), 0) << "duplicate PTS is off the grid";
            previous = duplicate;
            pts.push_back(duplicate);
        }

        EXPECT_GT(decision.pts, previous) << "PTS went backwards";
        EXPECT_EQ(decision.pts % ticks_per_frame(fps), 0) << "PTS is off the grid";
        EXPECT_LE(std::abs(decision.staleness_ns), half_slot_ns(fps))
            << "staleness exceeded half a slot, which should be impossible";
        previous = decision.pts;
        pts.push_back(decision.pts);
    }

    // Contiguity: emitted PTS form an unbroken run of slots. A hole here is a
    // stall for the player to interpolate through.
    for (std::size_t i = 1; i < pts.size(); ++i) {
        EXPECT_EQ(pts[i] - pts[i - 1], ticks_per_frame(fps)) << "hole in the timeline at " << i;
    }
    return pts;
}

/// Deterministic small-state PRNG, so a failure reproduces exactly.
struct Rng {
    std::uint64_t state;

    std::uint64_t next() {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return state >> 33;
    }

    /// Uniform in [low, high].
    std::int64_t range(std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(high - low + 1));
    }
};

// A rate that sweeps back and forth across the range a game actually occupies.
TEST(FramePacer, ASawtoothSourceRateKeepsTheTimelineIntact) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    // 82 -> 110 -> 82 fps, repeatedly, for ~10 s.
    std::vector<std::int64_t> timestamps;
    std::int64_t now = 0;
    bool rising = true;
    int rate = 82;
    while (now < 10 * kNsPerSecond) {
        timestamps.push_back(now);
        now += kNsPerSecond / rate;
        rate += rising ? 2 : -2;
        if (rate >= 110) {
            rising = false;
        } else if (rate <= 82) {
            rising = true;
        }
    }

    const auto pts = run_and_check_invariants(pacer, timestamps, kFps);
    ASSERT_GT(pts.size(), 500u);

    // The source never dips below 60, so nothing should ever need duplicating.
    EXPECT_EQ(pacer.duplicated(), 0u) << "duplicates appeared even though the source outran the target throughout";
    EXPECT_GT(pacer.dropped(), 0u) << "a source faster than the target must be dropping something";
}

// No periodic structure for a bug to hide behind.
TEST(FramePacer, ARandomlyJitteringSourceKeepsTheTimelineIntact) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    // Intervals uniform in 8-14 ms, i.e. roughly 71-125 fps.
    Rng rng{0xC0FFEE123456789ULL};
    std::vector<std::int64_t> timestamps;
    std::int64_t now = 0;
    while (now < 10 * kNsPerSecond) {
        timestamps.push_back(now);
        now += rng.range(8'000'000, 14'000'000);
    }

    const auto pts = run_and_check_invariants(pacer, timestamps, kFps);
    ASSERT_GT(pts.size(), 500u);
    EXPECT_EQ(pacer.duplicated(), 0u);
}

// A hitch: the game stalls for 200 ms. The timeline must not.
TEST(FramePacer, AStallIsFilledWithExactlyTheRightNumberOfDuplicates) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> timestamps;
    timestamps.reserve(120);
    for (std::int64_t i = 0; i < 60; ++i) { // 1 s at 60 fps
        timestamps.push_back(on_grid(i, kFps));
    }
    // Nothing for 200 ms -- 12 slots -- then the source resumes on the grid.
    const std::int64_t resume = on_grid(60, kFps) + (200LL * 1'000'000);
    for (std::int64_t i = 0; i < 60; ++i) {
        timestamps.push_back(resume + on_grid(i, kFps));
    }

    const auto pts = run_and_check_invariants(pacer, timestamps, kFps);

    // Contiguity is checked inside; here the point is that the gap was filled
    // rather than skipped, so the file's duration still matches wall clock.
    EXPECT_EQ(pacer.duplicated(), 12u) << "a 200 ms stall at 60 fps is 12 slots";
    EXPECT_EQ(pts.size(), pacer.emitted());
}

// A game dropping below the target and recovering: the pacer has to switch from
// dropping to duplicating and back without a discontinuity.
TEST(FramePacer, CrossingBelowTheTargetRateAndBackIsSeamless) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> timestamps;
    std::int64_t now = 0;
    // 100 fps for 2 s, 40 fps for 2 s, 100 fps for 2 s.
    for (const int rate : {100, 40, 100}) {
        const std::int64_t until = now + (2 * kNsPerSecond);
        while (now < until) {
            timestamps.push_back(now);
            now += kNsPerSecond / rate;
        }
    }

    const auto pts = run_and_check_invariants(pacer, timestamps, kFps);
    ASSERT_GT(pts.size(), 300u);

    // Both regimes were exercised -- otherwise the test proves nothing about the
    // transition between them.
    EXPECT_GT(pacer.dropped(), 0u) << "the fast sections should have dropped frames";
    EXPECT_GT(pacer.duplicated(), 0u) << "the 40 fps section should have duplicated frames";
}

// The awkward near-multiple. 121 fps into 60 is a ratio of 2.0167, so the extra
// slot arrives roughly once a second -- a periodic hitch rather than a random one,
// and periodic artifacts are the ones an eye locks onto.
//
// This characterises the effect rather than forbidding it: it is inherent to
// resampling any near-multiple rate onto a fixed grid, and the only way to avoid
// it is not to resample.
TEST(FramePacer, ANearMultipleRateProducesABoundedPeriodicExtraSlot) {
    constexpr int kFps = 60;
    constexpr int kSourceHz = 121;
    Pacer pacer(kFps);
    pacer.start(0);

    std::vector<std::int64_t> timestamps;
    timestamps.reserve(static_cast<std::size_t>(kSourceHz) * 10);
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(kSourceHz) * 10; ++i) {
        timestamps.push_back(on_grid(i, kSourceHz));
    }

    const auto pts = run_and_check_invariants(pacer, timestamps, kFps);
    ASSERT_GT(pts.size(), 500u);

    // Still exactly one frame per slot -- the beat changes *which* source frame is
    // chosen, never how many are emitted.
    EXPECT_EQ(pacer.duplicated(), 0u);
    EXPECT_NEAR(static_cast<double>(pts.size()), 600.0, 2.0);
}

// The property the whole design rests on: quantization is absolute from t0, so
// error cannot accumulate no matter how long the run or how erratic the source.
TEST(FramePacer, AVariableSourceDoesNotDriftOverAThirtyMinuteRun) {
    constexpr int kFps = 60;
    constexpr std::int64_t kDurationNs = 30LL * 60 * kNsPerSecond;
    Pacer pacer(kFps);
    pacer.start(0);

    Rng rng{0x1234567890ABCDEFULL};
    std::int64_t now = 0;
    std::int64_t last_pts = -1;

    while (now < kDurationNs) {
        const PacingDecision decision = pacer.decide(now);
        if (!decision.drop) {
            for (const std::int64_t duplicate : decision.duplicate_pts) {
                ASSERT_GT(duplicate, last_pts);
                last_pts = duplicate;
            }
            ASSERT_GT(decision.pts, last_pts);
            last_pts = decision.pts;
        }
        now += rng.range(8'000'000, 14'000'000); // 71-125 fps
    }

    // 30 minutes at 60 fps is 108000 slots. Being more than a frame or two out
    // after this many would mean the quantizer was accumulating rather than
    // recomputing from t0 each time.
    const auto expected = static_cast<std::uint64_t>((kDurationNs / kNsPerSecond) * kFps);
    EXPECT_NEAR(static_cast<double>(pacer.emitted()), static_cast<double>(expected), 2.0);

    // The last PTS must land within one frame of the run's wall-clock length. The
    // final source timestamp sits just under the 30-minute mark and can round up
    // into the next slot, so this is a tolerance rather than an equality.
    const std::int64_t expected_final_pts = (kDurationNs / kNsPerSecond) * kVideoTimebaseDen;
    EXPECT_LE(std::abs(last_pts - expected_final_pts), ticks_per_frame(kFps))
        << "the timeline drifted from wall clock: last PTS " << last_pts << " vs " << expected_final_pts;
}

// ---------------------------------------------------------------------------
// Staleness -- what SPEC.md §7.2's "first frame in the slot wins" actually costs
// ---------------------------------------------------------------------------

// The bound is structural: a timestamp further than half a slot from the slot's
// nominal time quantizes into a different slot, so it cannot be reported here.
TEST(FramePacer, StalenessNeverExceedsHalfASlot) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    Rng rng{0xFEEDFACEULL};
    std::int64_t now = 0;
    while (now < 5 * kNsPerSecond) {
        const PacingDecision decision = pacer.decide(now);
        if (!decision.drop) {
            EXPECT_LE(std::abs(decision.staleness_ns), half_slot_ns(kFps));
        }
        now += rng.range(4'000'000, 30'000'000); // 33-250 fps, deliberately wild
    }
    EXPECT_LE(pacer.worst_staleness_ns(), half_slot_ns(kFps));
    EXPECT_GT(pacer.staleness_samples(), 0u);
}

// A source exactly on the grid has nothing to be stale about.
TEST(FramePacer, AMatchedSourceHasNoStaleness) {
    constexpr int kFps = 60;
    Pacer pacer(kFps);
    pacer.start(0);

    for (std::int64_t i = 0; i < 600; ++i) {
        const PacingDecision decision = pacer.decide(on_grid(i, kFps));
        ASSERT_FALSE(decision.drop);
        EXPECT_LE(std::abs(decision.staleness_ns), 100) << "frame " << i; // sub-microsecond
    }
    EXPECT_LE(pacer.worst_staleness_ns(), 100);
}

// The number Part C needs: how much does the *variation* in staleness grow as the
// source rate falls? The wobble is what a viewer could notice; the mean is a
// constant offset and is not.
//
// Documented as a measurement rather than a threshold. Turning it into a hard
// bound would be inventing an acceptance criterion SPEC.md does not state.
TEST(FramePacer, StalenessWobbleTracksTheSourceFrameInterval) {
    constexpr int kFps = 60;

    for (const int source_hz : {240, 144, 120, 100, 82}) {
        Pacer pacer(kFps);
        pacer.start(0);

        std::int64_t lowest = std::numeric_limits<std::int64_t>::max();
        std::int64_t highest = std::numeric_limits<std::int64_t>::min();
        bool skipped_first = false;

        for (std::int64_t i = 0; i < static_cast<std::int64_t>(source_hz) * 5; ++i) {
            const PacingDecision decision = pacer.decide(on_grid(i, source_hz));
            if (decision.drop) {
                continue;
            }
            // The first emitted frame *defines* t0, so its staleness is zero by
            // construction rather than by selection. Including it reports the
            // initial condition as though it were part of the wobble -- the same
            // trap the evenness metric above has to sidestep.
            if (!skipped_first) {
                skipped_first = true;
                continue;
            }
            lowest = std::min(lowest, decision.staleness_ns);
            highest = std::max(highest, decision.staleness_ns);
        }

        const std::int64_t wobble = highest - lowest;
        // Integer division, so this is up to a nanosecond short of the true period
        // -- hence the slack below rather than an exact comparison.
        const std::int64_t source_interval = kNsPerSecond / source_hz;

        // Taking the first frame in each slot means the chosen frame lands
        // somewhere within one source interval after the slot's lower boundary, so
        // that interval is exactly the wobble's ceiling -- and the measurements
        // sit right on it, not comfortably under.
        //
        // This is the closed-form cost of SPEC.md §7.2's first-wins rule: the
        // content-timing wobble equals one source frame interval, which is why a
        // slower or fluctuating source looks worse even when every frame count and
        // every timestamp is exactly correct.
        EXPECT_LE(wobble, source_interval + 1)
            << source_hz << " Hz: wobble " << wobble << " ns exceeds one source interval " << source_interval;

        std::cout << "[ RUN INFO ] " << source_hz << " Hz -> " << kFps << " fps: staleness wobble " << wobble / 1000
                  << " us, mean " << pacer.mean_staleness_ns() / 1000 << " us, worst "
                  << pacer.worst_staleness_ns() / 1000 << " us\n";
    }
}

// ---------------------------------------------------------------------------
// Misuse
// ---------------------------------------------------------------------------

TEST(FramePacer, DecidingBeforeStartDropsRatherThanInventingAnEpoch) {
    Pacer pacer(60);
    EXPECT_FALSE(pacer.started());
    const PacingDecision decision = pacer.decide(1'000'000);
    EXPECT_TRUE(decision.drop);
}

} // namespace
