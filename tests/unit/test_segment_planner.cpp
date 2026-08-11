// SPEC.md §11's split policy.
//
// CPU TIER. `SegmentPlanner` owns no muxer, no file and no clock, so §11's arithmetic is
// asserted against a supplied packet stream rather than observed on hardware. The
// mechanism half -- opening the next context before closing the current one -- is the
// GPU tier's `SegmentationTest`.

#include "core/mux/segment_planner.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using fc::mux::SegmentPlanner;
using Action = fc::mux::SegmentPlanner::Action;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kNsPerMinute = 60 * kNsPerSecond;
constexpr std::uint64_t kBytesPerMb = 1024ULL * 1024ULL;

fc::config::SegmentationSettings by_duration(int minutes) {
    fc::config::SegmentationSettings settings;
    settings.enabled = true;
    settings.split_by_duration = true;
    settings.duration_minutes = minutes;
    settings.split_by_size = false;
    return settings;
}

fc::config::SegmentationSettings by_size(int megabytes) {
    fc::config::SegmentationSettings settings;
    settings.enabled = true;
    settings.split_by_duration = false;
    settings.split_by_size = true;
    settings.size_mb = megabytes;
    return settings;
}

/// Drives a planner through a packet stream and records where the splits landed.
///
/// Models the caller's contract faithfully -- `note_split` only after a `Split`, and the
/// keyframe cadence of a real encoder that honours a keyframe request on the next GOP
/// boundary -- so a planner that only works when driven leniently fails here.
struct DriveResult {
    std::vector<std::int64_t> split_pts;
    std::vector<int> split_indices;
    int keyframes_requested = 0;
    int packets = 0;
};

/// `gop` packets per keyframe; `extra_gops_to_honour` models an encoder that does not
/// produce the requested IDR immediately.
DriveResult drive(SegmentPlanner& planner, int packet_count, std::int64_t step_ns, std::uint64_t bytes_per_packet,
                  int gop = 120) {
    DriveResult run;
    std::uint64_t total = 0;
    for (int i = 0; i < packet_count; ++i) {
        const std::int64_t pts = static_cast<std::int64_t>(i) * step_ns;
        const bool keyframe = (i % gop) == 0;
        total += bytes_per_packet;
        ++run.packets;

        const Action action = planner.offer(pts, keyframe, total);
        if (action == Action::RequestKeyframe) {
            ++run.keyframes_requested;
        } else if (action == Action::Split) {
            EXPECT_TRUE(keyframe) << "a split landed on a packet that is not a keyframe, at pts " << pts;
            run.split_pts.push_back(pts);
            run.split_indices.push_back(planner.index() + 1);
            planner.note_split(pts, total);
        }
    }
    return run;
}

// ---------------------------------------------------------------------------
// CLAUDE.md hard rule 7
// ---------------------------------------------------------------------------

TEST(SegmentPlanner, ADefaultPlannerNeverSplits) {
    SegmentPlanner planner;
    EXPECT_FALSE(planner.enabled());

    const DriveResult run = drive(planner, 100'000, kNsPerSecond, 10 * kBytesPerMb);
    EXPECT_TRUE(run.split_pts.empty()) << "a default planner split " << run.split_pts.size() << " times";
    EXPECT_EQ(planner.index(), 1);
}

TEST(SegmentPlanner, SegmentationOffOverridesArmedTriggers) {
    // The triggers are configured and would fire; `enabled` is the master switch, and
    // CLAUDE.md hard rule 7 makes it false the thing that must always win.
    fc::config::SegmentationSettings settings = by_duration(1);
    settings.split_by_size = true;
    settings.size_mb = 1;
    settings.enabled = false;

    SegmentPlanner planner{settings};
    EXPECT_FALSE(planner.enabled());
    EXPECT_TRUE(drive(planner, 10'000, kNsPerSecond, kBytesPerMb).split_pts.empty());
}

TEST(SegmentPlanner, AnEnabledPlannerWithNoArmedTriggerNeverSplits) {
    // Both triggers off is a coherent configuration and must not mean "split always".
    fc::config::SegmentationSettings settings;
    settings.enabled = true;
    settings.split_by_duration = false;
    settings.split_by_size = false;

    SegmentPlanner planner{settings};
    EXPECT_FALSE(planner.enabled());
    EXPECT_TRUE(drive(planner, 10'000, kNsPerSecond, kBytesPerMb).split_pts.empty());
}

// ---------------------------------------------------------------------------
// Keyframe alignment -- the clause the exit criterion names
// ---------------------------------------------------------------------------

// > Splits must be **keyframe-aligned**: request an IDR from the encoder, wait for it,
// > close the current file at the packet *before* it, open the next file starting *at*
// > it.
//
// `drive` asserts the alignment on every split it sees. This case makes the trigger fire
// deep inside a GOP so that honouring it *requires* waiting: a planner that split on the
// triggering packet would be caught here and nowhere else.
TEST(SegmentPlanner, ATriggerFiringMidGopWaitsForTheNextKeyframe) {
    constexpr int kGop = 120;
    // 60 fps, so a two-second GOP. One minute is 3600 packets: 30 whole GOPs, so the
    // trigger fires exactly on a keyframe unless it is nudged. Half a GOP of offset puts
    // it in the middle of one.
    SegmentPlanner planner{by_duration(1)};

    DriveResult run;
    std::uint64_t total = 0;
    const std::int64_t step = kNsPerSecond / 60;
    const std::int64_t offset = 60 * step; // half a GOP
    for (int i = 0; i < 8000; ++i) {
        const std::int64_t pts = offset + (static_cast<std::int64_t>(i) * step);
        const bool keyframe = (i % kGop) == 0;
        total += 1000;
        const Action action = planner.offer(pts, keyframe, total);
        if (action == Action::Split) {
            ASSERT_TRUE(keyframe) << "split at pts " << pts << " is not a keyframe";
            run.split_pts.push_back(pts);
            planner.note_split(pts, total);
        } else if (action == Action::RequestKeyframe) {
            ++run.keyframes_requested;
        }
    }

    ASSERT_FALSE(run.split_pts.empty()) << "a one-minute trigger never fired across 8000 packets";
    EXPECT_GT(run.keyframes_requested, 0) << "the split happened without ever asking for a keyframe";

    // Measured from the segment's own first packet, not from zero. §11's trigger is "each
    // file is N minutes", so a stream whose first packet is at `offset` reaches a minute
    // of *segment* time at `offset + 60 s` -- comparing against absolute time instead
    // would call a correct planner late by exactly the offset.
    const std::int64_t due_at = offset + kNsPerMinute;
    EXPECT_GE(run.split_pts.front(), due_at) << "the split happened before the segment was a minute old";
    EXPECT_LT(run.split_pts.front() - due_at, kGop * step) << "the split waited longer than one GOP past its trigger";
}

// One request per split, however long the encoder takes to answer. Asking again every
// packet would flood a request the encoder is already working on, and -- the real hazard
// -- could split twice at adjacent keyframes and emit a file a few frames long.
TEST(SegmentPlanner, ATriggerAsksForAKeyframeOnceAndNotOncePerPacket) {
    SegmentPlanner planner{by_duration(1)};

    int requests = 0;
    int splits = 0;
    std::uint64_t total = 0;
    const std::int64_t step = kNsPerSecond / 60;
    // A deliberately distant keyframe: 600 packets of waiting after the trigger.
    for (int i = 0; i < 7200; ++i) {
        total += 1000;
        const Action action = planner.offer(static_cast<std::int64_t>(i) * step, (i % 600) == 0, total);
        if (action == Action::RequestKeyframe) {
            ++requests;
        } else if (action == Action::Split) {
            ++splits;
            planner.note_split(static_cast<std::int64_t>(i) * step, total);
        }
    }

    EXPECT_EQ(splits, 1) << "a 7200-packet run at 60 fps is two minutes and should split once";
    EXPECT_EQ(requests, 1) << "the keyframe was requested " << requests << " times for one split";
}

// ---------------------------------------------------------------------------
// The triggers themselves
// ---------------------------------------------------------------------------

TEST(SegmentPlanner, TheDurationTriggerMeasuresEachSegmentRatherThanTheRecording) {
    // A 1-minute trigger over 5 minutes is four splits and five files -- not one split at
    // the first minute and then silence, which is what measuring from the epoch gives.
    SegmentPlanner planner{by_duration(1)};
    const std::int64_t step = kNsPerSecond / 60;
    const DriveResult run = drive(planner, 5 * 60 * 60, step, 1000, /*gop=*/120);

    EXPECT_EQ(run.split_pts.size(), 4U) << "five minutes at one minute per segment is four splits";
    EXPECT_EQ(planner.index(), 5);

    // Each split roughly a minute after the last, within a GOP either way.
    for (std::size_t i = 1; i < run.split_pts.size(); ++i) {
        const std::int64_t gap = run.split_pts[i] - run.split_pts[i - 1];
        EXPECT_GE(gap, kNsPerMinute);
        EXPECT_LT(gap - kNsPerMinute, 120 * step) << "segment " << i << " ran a GOP too long";
    }
}

TEST(SegmentPlanner, TheSizeTriggerMeasuresEachSegmentRatherThanTheRecording) {
    // 1 MB segments, 64 KB per packet: 16 packets a segment. The GOP is 8, so the split
    // lands on the keyframe at or after the threshold.
    SegmentPlanner planner{by_size(1)};
    const DriveResult run = drive(planner, 200, kNsPerSecond / 60, std::uint64_t{64} * 1024, /*gop=*/8);

    ASSERT_GE(run.split_pts.size(), 5U) << "200 packets of 64 KB is 12.5 MB and should split repeatedly";
    EXPECT_EQ(planner.splits(), static_cast<int>(run.split_pts.size()));
}

// > Both may be armed; whichever fires first wins.
TEST(SegmentPlanner, WhicheverTriggerFiresFirstWins) {
    fc::config::SegmentationSettings settings;
    settings.enabled = true;
    settings.split_by_duration = true;
    settings.duration_minutes = 60; // far away
    settings.split_by_size = true;
    settings.size_mb = 1; // near

    SegmentPlanner planner{settings};
    const DriveResult run = drive(planner, 200, kNsPerSecond / 60, std::uint64_t{64} * 1024, /*gop=*/8);
    EXPECT_GE(run.split_pts.size(), 5U) << "the size trigger was armed and never won";

    // And the mirror: a near duration trigger beats a distant size one.
    fc::config::SegmentationSettings other;
    other.enabled = true;
    other.split_by_duration = true;
    other.duration_minutes = 1;
    other.split_by_size = true;
    other.size_mb = 100'000; // unreachable

    SegmentPlanner second{other};
    const DriveResult by_time = drive(second, 5 * 60 * 60, kNsPerSecond / 60, 1000, /*gop=*/120);
    EXPECT_EQ(by_time.split_pts.size(), 4U) << "the duration trigger was armed and never won";
}

// ---------------------------------------------------------------------------
// The caller can fail
// ---------------------------------------------------------------------------

// `note_split` is separate from `offer` because opening the next file is I/O and can
// refuse. A planner that advanced on `offer` would then be measuring the next segment
// from a boundary that never happened, and the recording would carry a permanent
// off-by-one in its segment length.
TEST(SegmentPlanner, ASplitThatTheCallerCouldNotPerformDoesNotAdvanceTheSegment) {
    SegmentPlanner planner{by_size(1)};

    std::uint64_t total = 0;
    int offered_splits = 0;
    for (int i = 0; i < 40; ++i) {
        total += std::uint64_t{64} * 1024;
        if (planner.offer(static_cast<std::int64_t>(i) * kNsPerSecond, (i % 8) == 0, total) == Action::Split) {
            ++offered_splits; // and deliberately no `note_split` -- the open failed
        }
    }

    EXPECT_GT(offered_splits, 0) << "no split was offered, so this proves nothing";
    EXPECT_EQ(planner.index(), 1) << "the planner advanced without the caller confirming";
    EXPECT_EQ(planner.splits(), 0);
}

// ---------------------------------------------------------------------------
// Naming (SPEC.md §11)
// ---------------------------------------------------------------------------

TEST(SegmentPath, NamesArePartPrefixedZeroPaddedAndKeepTheExtension) {
    const std::filesystem::path base = "D:/recordings/capture.mkv";

    EXPECT_EQ(fc::mux::segment_path(base, 1).filename().string(), "capture_part001.mkv");
    EXPECT_EQ(fc::mux::segment_path(base, 2).filename().string(), "capture_part002.mkv");
    EXPECT_EQ(fc::mux::segment_path(base, 42).filename().string(), "capture_part042.mkv");

    // The directory is preserved, not just the name.
    EXPECT_EQ(fc::mux::segment_path(base, 1).parent_path(), base.parent_path());

    // And the container's extension survives, because §11's split is per-file and each
    // file is the same container as the recording.
    EXPECT_EQ(fc::mux::segment_path("D:/x/capture.mp4", 3).filename().string(), "capture_part003.mp4");
}

// > zero-padded to 3, auto-widening past 999
//
// Widening rather than wrapping: a recording long enough to produce a thousand segments
// must not start overwriting the ones it already wrote.
TEST(SegmentPath, NumberingWidensPastNineHundredNinetyNineInsteadOfWrapping) {
    const std::filesystem::path base = "capture.mkv";

    EXPECT_EQ(fc::mux::segment_path(base, 999).filename().string(), "capture_part999.mkv");
    EXPECT_EQ(fc::mux::segment_path(base, 1000).filename().string(), "capture_part1000.mkv");
    EXPECT_EQ(fc::mux::segment_path(base, 12345).filename().string(), "capture_part12345.mkv");

    // The property that matters is that no two indices share a name.
    std::vector<std::string> names;
    for (int i = 1; i <= 1100; ++i) {
        names.push_back(fc::mux::segment_path(base, i).filename().string());
    }
    std::ranges::sort(names);
    EXPECT_EQ(std::ranges::adjacent_find(names), names.end()) << "two segments share a filename";
}

// A base that is already a segment name must not accumulate suffixes. The session names
// the *next* file from the original base, and a bug there yields `x_part001_part002.mkv`.
TEST(SegmentPath, NamingIsIdempotentInTheSenseThatItAlwaysDerivesFromTheBase) {
    const std::filesystem::path base = "capture.mkv";
    const std::filesystem::path first = fc::mux::segment_path(base, 1);
    EXPECT_EQ(fc::mux::segment_path(base, 2).filename().string(), "capture_part002.mkv");
    EXPECT_NE(fc::mux::segment_path(base, 2), fc::mux::segment_path(first, 2))
        << "deriving from a segment name rather than the base would double the suffix";
}

} // namespace
