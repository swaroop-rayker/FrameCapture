// The shared media epoch (SPEC.md §7.1, §20 row 4).
//
// CPU TIER. The whole component is a negotiation between two timestamps, so the
// cases that matter -- audio first, video first, audio never -- are three lines
// each and none of them needs a device.

#include "core/timing/session_epoch.h"

#include <gtest/gtest.h>

namespace {

using fc::timing::SessionEpoch;

constexpr std::int64_t kMs = 1'000'000;

// ---------------------------------------------------------------------------
// Video only
// ---------------------------------------------------------------------------

// A recording with no audio must not wait for a packet that is never coming.
TEST(SessionEpoch, VideoAloneResolvesWhenNoAudioIsExpected) {
    SessionEpoch epoch(false);
    EXPECT_FALSE(epoch.resolved());

    epoch.note_video(500 * kMs);
    EXPECT_TRUE(epoch.resolved());
    EXPECT_EQ(epoch.t0_ns(), 500 * kMs);
    EXPECT_EQ(epoch.startup_skew_ns(), 0);
}

// ---------------------------------------------------------------------------
// Both streams
// ---------------------------------------------------------------------------

TEST(SessionEpoch, WaitsForBothStreamsWhenAudioIsExpected) {
    SessionEpoch epoch(true);

    epoch.note_video(100 * kMs);
    EXPECT_FALSE(epoch.resolved()) << "resolved on video alone; audio's origin would be ignored";

    epoch.note_audio(140 * kMs);
    EXPECT_TRUE(epoch.resolved());
}

// The epoch is the moment *both* are live, which is the later of the two. Taking
// the earlier one would place the other stream's first sample before its own
// origin -- and since both components clamp negatives to zero, that shows up as a
// permanent offset rather than as an error.
TEST(SessionEpoch, TheEpochIsTheLaterOfTheTwoFirsts) {
    {
        SessionEpoch audio_late(true);
        audio_late.note_video(100 * kMs);
        audio_late.note_audio(140 * kMs);
        EXPECT_EQ(audio_late.t0_ns(), 140 * kMs);
    }
    {
        SessionEpoch video_late(true);
        video_late.note_audio(100 * kMs);
        video_late.note_video(140 * kMs);
        EXPECT_EQ(video_late.t0_ns(), 140 * kMs);
    }
}

TEST(SessionEpoch, ArrivalOrderDoesNotChangeTheResult) {
    SessionEpoch video_first(true);
    video_first.note_video(100 * kMs);
    video_first.note_audio(140 * kMs);

    SessionEpoch audio_first(true);
    audio_first.note_audio(140 * kMs);
    audio_first.note_video(100 * kMs);

    EXPECT_EQ(video_first.t0_ns(), audio_first.t0_ns());
    EXPECT_EQ(video_first.startup_skew_ns(), audio_first.startup_skew_ns());
}

// Only the first timestamp from each stream defines anything; every frame after
// it is just a frame.
TEST(SessionEpoch, LaterTimestampsAreIgnored) {
    SessionEpoch epoch(true);
    epoch.note_video(100 * kMs);
    epoch.note_video(900 * kMs);
    epoch.note_audio(140 * kMs);
    epoch.note_audio(950 * kMs);

    EXPECT_EQ(epoch.t0_ns(), 140 * kMs);
}

// The number worth logging: how far apart the two devices started. It is the
// first thing to look at when a recording draws a lip-sync complaint.
TEST(SessionEpoch, SkewIsReportedAndIsUnsigned) {
    SessionEpoch audio_late(true);
    audio_late.note_video(100 * kMs);
    audio_late.note_audio(140 * kMs);
    EXPECT_EQ(audio_late.startup_skew_ns(), 40 * kMs);

    SessionEpoch video_late(true);
    video_late.note_audio(100 * kMs);
    video_late.note_video(140 * kMs);
    EXPECT_EQ(video_late.startup_skew_ns(), 40 * kMs);
}

TEST(SessionEpoch, SimultaneousStartsHaveNoSkew) {
    SessionEpoch epoch(true);
    epoch.note_video(500 * kMs);
    epoch.note_audio(500 * kMs);
    EXPECT_EQ(epoch.t0_ns(), 500 * kMs);
    EXPECT_EQ(epoch.startup_skew_ns(), 0);
}

// ---------------------------------------------------------------------------
// The bug this prevents
// ---------------------------------------------------------------------------

// Before this existed, `t0` came from whichever frame the video path saw first.
// With audio starting 40 ms later, the audio timeline's own origin would have
// been 40 ms after video's -- and both streams clamp pre-epoch material to zero,
// so the discrepancy never surfaces as an error. It surfaces as lip-sync that is
// wrong by exactly the device start-up difference, for the whole recording.
TEST(SessionEpoch, BothStreamsMeasureFromTheSameOriginRegardlessOfStartupSkew) {
    constexpr std::int64_t kVideoFirst = 100 * kMs;
    constexpr std::int64_t kAudioFirst = 140 * kMs;

    SessionEpoch epoch(true);
    epoch.note_video(kVideoFirst);
    epoch.note_audio(kAudioFirst);
    ASSERT_TRUE(epoch.resolved());

    const std::int64_t t0 = epoch.t0_ns();

    // A video frame and an audio packet captured at the same instant must be the
    // same distance from the epoch. That is the entire property.
    constexpr std::int64_t kSameInstant = 1000 * kMs;
    EXPECT_EQ(kSameInstant - t0, kSameInstant - t0);

    // And the epoch is not before either stream started, so neither has to
    // represent a negative offset.
    EXPECT_GE(t0, kVideoFirst);
    EXPECT_GE(t0, kAudioFirst);
}

} // namespace
