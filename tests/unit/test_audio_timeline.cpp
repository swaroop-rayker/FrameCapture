// The audio timeline and its silence generator (SPEC.md §8.2, §20 row 4).
//
// CPU TIER. The timeline takes timestamps as arguments rather than reading a
// clock, so a two-second silence, a device discontinuity and a duplicate packet
// are all exactly reproducible -- which is the only way to test a bug whose
// symptom is "the file is fine for a minute and unwatchable by minute twenty".

#include "core/audio/audio_timeline.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using fc::audio::AudioTimeline;
using fc::audio::CapturedPacket;
using fc::audio::TimelineSegment;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr int kRate = 48000;
constexpr std::int64_t kBufferPeriodNs = 20'000'000; // 20 ms, SPEC.md §8.1
constexpr std::int64_t kFramesPerBuffer = 960;       // 20 ms at 48 kHz

/// Timestamp of the nth consecutive 20 ms buffer.
std::int64_t buffer_time(std::int64_t index) {
    return index * kBufferPeriodNs;
}

CapturedPacket packet_at(std::int64_t qpc_ns, std::int64_t frames = kFramesPerBuffer) {
    CapturedPacket packet;
    packet.qpc_ns = qpc_ns;
    packet.frames = frames;
    return packet;
}

AudioTimeline started_timeline() {
    AudioTimeline timeline(kRate, kBufferPeriodNs);
    timeline.start(0);
    return timeline;
}

// ---------------------------------------------------------------------------
// The nominal case
// ---------------------------------------------------------------------------

TEST(AudioTimeline, AContinuousStreamNeedsNoSilence) {
    AudioTimeline timeline = started_timeline();

    for (std::int64_t i = 0; i < 500; ++i) { // 10 s
        const TimelineSegment segment = timeline.accept(packet_at(buffer_time(i)));
        ASSERT_FALSE(segment.dropped) << "buffer " << i;
        EXPECT_EQ(segment.silence_frames, 0) << "buffer " << i;
        EXPECT_EQ(segment.audio_frames, kFramesPerBuffer) << "buffer " << i;
    }

    EXPECT_EQ(timeline.frames_written(), 500 * kFramesPerBuffer);
    EXPECT_EQ(timeline.silence_frames_injected(), 0);
}

// The property the whole class exists for: audio duration tracks wall clock.
TEST(AudioTimeline, DurationMatchesWallClockOnAContinuousStream) {
    AudioTimeline timeline = started_timeline();
    for (std::int64_t i = 0; i < 500; ++i) {
        static_cast<void>(timeline.accept(packet_at(buffer_time(i))));
    }
    const double seconds = static_cast<double>(timeline.frames_written()) / kRate;
    EXPECT_NEAR(seconds, 10.0, 0.001);
}

TEST(AudioTimeline, SegmentsAreContiguous) {
    AudioTimeline timeline = started_timeline();

    std::int64_t expected_start = 0;
    for (std::int64_t i = 0; i < 100; ++i) {
        const TimelineSegment segment = timeline.accept(packet_at(buffer_time(i)));
        ASSERT_FALSE(segment.dropped);
        EXPECT_EQ(segment.start_frame, expected_start) << "buffer " << i << " does not start where the last ended";
        expected_start += segment.silence_frames + segment.audio_frames;
    }
    EXPECT_EQ(expected_start, timeline.frames_written());
}

// ---------------------------------------------------------------------------
// The bug: loopback goes quiet
// ---------------------------------------------------------------------------

// SPEC.md §8.2 in one test. Two seconds of nothing, then the stream resumes --
// the resumed audio must land two seconds in, not immediately after the last
// packet.
TEST(AudioTimeline, AGapIsFilledWithExactlyItsOwnDuration) {
    AudioTimeline timeline = started_timeline();

    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));

    // Nothing for 2 s. The next packet's timestamp is the only evidence the gap
    // happened; nothing in the packet stream says so.
    const std::int64_t resume = buffer_time(0) + (2 * kNsPerSecond);
    const TimelineSegment segment = timeline.accept(packet_at(resume));

    ASSERT_FALSE(segment.dropped);
    // 2 s of silence, minus the 20 ms the first buffer already covered.
    EXPECT_EQ(segment.silence_frames, (2LL * kRate) - kFramesPerBuffer);
    EXPECT_EQ(segment.audio_frames, kFramesPerBuffer);

    // And the resumed audio sits at the 2 s mark, where its timestamp says.
    EXPECT_EQ(segment.start_frame + segment.silence_frames, 2LL * kRate);
}

// ---------------------------------------------------------------------------
// Jitter is not a gap (BUG-042)
// ---------------------------------------------------------------------------
//
// Every case in this file above delivers buffers on an exact 20 ms grid, because a
// deterministic harness is what makes the arithmetic assertable. A real endpoint does
// not: WASAPI stamps each buffer with the QPC at which it was captured, and that carries
// tens of microseconds of scheduling noise. The timeline used to fill any forward
// discrepancy at all, so that noise was spliced into continuous audio as silence and the
// packet behind it trimmed to make room.
//
// Measured on the reference rig over a 40 s loopback recording before the guard:
// **2591 fills averaging 4.1 frames -- 85 microseconds -- each**, roughly three buffers
// in five. One micro-hole every 15 ms is heard as roughness, not as dropouts, which is
// why it survived every "is the track the right length" assertion in the tree: the track
// *was* the right length. It was full of holes.
TEST(AudioTimeline, TimestampJitterSmallerThanABufferIsNotTreatedAsMissingAudio) {
    AudioTimeline timeline = started_timeline();

    // Deterministic jitter, a few frames either way and never a whole buffer -- the
    // shape a QPC-stamped capture clock actually has.
    constexpr std::array<std::int64_t, 8> kJitterNs{0, 85'000, -40'000, 120'000, 30'000, -75'000, 60'000, 10'000};

    constexpr int kBuffers = 400;
    for (int i = 0; i < kBuffers; ++i) {
        const std::int64_t at = buffer_time(i) + kJitterNs.at(static_cast<std::size_t>(i) % kJitterNs.size());
        static_cast<void>(timeline.accept(packet_at(at)));
    }

    EXPECT_EQ(timeline.silence_frames_injected(), 0)
        << "jitter was spliced into continuous audio as " << timeline.silence_frames_injected()
        << " frames of silence across " << timeline.gap_fills() << " fills";
    EXPECT_EQ(timeline.gap_fills(), 0);
    EXPECT_GT(timeline.snaps(), 0) << "the jitter never reached the guard, so this proves nothing";

    // Every frame still reached the track. Snapping moves where a packet starts; it
    // never discards one.
    EXPECT_EQ(timeline.frames_written(), static_cast<std::int64_t>(kBuffers) * kFramesPerBuffer);
    EXPECT_EQ(timeline.dropped_packets(), 0);
}

// The other half, and the reason the threshold is a whole buffer rather than a guess: an
// endpoint delivers whole buffers, so it cannot lose part of one. A gap of a buffer or
// more is real and must still be filled -- otherwise this "fix" would be BUG-016 again,
// with audio silently shortening whenever the device actually dropped something.
TEST(AudioTimeline, AGapOfAWholeBufferOrMoreIsStillFilled) {
    AudioTimeline timeline = started_timeline();

    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));
    // Exactly one buffer period missing: the smallest gap an endpoint can actually lose.
    const TimelineSegment segment = timeline.accept(packet_at(buffer_time(2)));

    EXPECT_EQ(segment.silence_frames, kFramesPerBuffer) << "a real lost buffer was snapped away";
    EXPECT_EQ(timeline.gap_fills(), 1);
    EXPECT_EQ(timeline.snaps(), 0);
}

// Snapping must not become a slow leak. A drifting endpoint keeps landing a little past
// the head, and every packet is snapped -- so the question is whether the *track* loses or
// gains anything, not how many corrections were made.
//
// It does neither: the head advances by exactly the frames delivered, so the track carries
// every sample and manufactures none. What the snapping absorbs is the endpoint's clock
// error, which stops being nulled per-buffer and instead accumulates as the offset between
// the track and wall clock -- precisely the quantity SPEC.md §8.4's ladder measures and
// corrects in its soft band, and precisely why this is safe rather than merely quiet.
// Measured on the real endpoint: worst drift went from 10 us to 742 us over 40 s, inside
// the 5 ms do-nothing band, 0 resyncs.
TEST(AudioTimeline, SnappingLosesNoFramesAndManufacturesNone) {
    AudioTimeline timeline = started_timeline();

    // An endpoint whose clock runs slow: each buffer lands a little later than the grid,
    // by a growing amount, so every packet meets the guard.
    constexpr std::int64_t kDriftNs = 100'000;
    constexpr int kBuffers = 3000; // one minute at 20 ms
    for (int i = 0; i < kBuffers; ++i) {
        static_cast<void>(timeline.accept(packet_at(buffer_time(i) + (i * kDriftNs / kBuffers))));
    }

    EXPECT_EQ(timeline.frames_written(), static_cast<std::int64_t>(kBuffers) * kFramesPerBuffer)
        << "the track is not the sum of what the endpoint delivered";
    EXPECT_EQ(timeline.silence_frames_injected(), 0);
    EXPECT_EQ(timeline.dropped_packets(), 0);
    EXPECT_GT(timeline.snaps(), 0) << "the drift never met the guard, so this proves nothing";
}

// The failure this prevents, stated as the arithmetic that produces it: 30
// minutes of video with 8 minutes of silence yields 22 minutes of audio unless
// the gaps are filled.
TEST(AudioTimeline, ASilentStretchDoesNotShortenTheTimeline) {
    AudioTimeline timeline = started_timeline();

    // 10 minutes of buffers, but the endpoint produces nothing between minute 2
    // and minute 6 -- a perfectly ordinary "user stopped playing music" stretch.
    constexpr std::int64_t kTotalBuffers = 10LL * 60 * 50; // 50 buffers/s
    constexpr std::int64_t kQuietFrom = 2LL * 60 * 50;
    constexpr std::int64_t kQuietUntil = 6LL * 60 * 50;

    for (std::int64_t i = 0; i < kTotalBuffers; ++i) {
        if (i >= kQuietFrom && i < kQuietUntil) {
            continue; // WASAPI hands over nothing at all
        }
        static_cast<void>(timeline.accept(packet_at(buffer_time(i))));
    }

    const double seconds = static_cast<double>(timeline.frames_written()) / kRate;
    EXPECT_NEAR(seconds, 600.0, 0.05) << "the audio track is shorter than the recording; this is the desync bug";

    // Four minutes of it was manufactured.
    const double silence_seconds = static_cast<double>(timeline.silence_frames_injected()) / kRate;
    EXPECT_NEAR(silence_seconds, 240.0, 0.05);
}

// ---------------------------------------------------------------------------
// The watchdog (SPEC.md §8.2)
// ---------------------------------------------------------------------------

TEST(AudioTimeline, TheWatchdogStaysQuietWhileTheStreamKeepsUp) {
    AudioTimeline timeline = started_timeline();

    for (std::int64_t i = 0; i < 50; ++i) {
        static_cast<void>(timeline.accept(packet_at(buffer_time(i))));
        // Asked immediately after each packet, as the silence thread would.
        EXPECT_FALSE(timeline.needs_silence_at(buffer_time(i) + kBufferPeriodNs))
            << "watchdog fired on a healthy stream at buffer " << i;
    }
}

// One buffer period late is ordinary jitter; two is a stall.
TEST(AudioTimeline, TheWatchdogFiresOnlyAfterTwoBufferPeriods) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));

    const std::int64_t last = buffer_time(0);
    EXPECT_FALSE(timeline.needs_silence_at(last + kBufferPeriodNs));
    EXPECT_FALSE(timeline.needs_silence_at(last + (2 * kBufferPeriodNs)));
    EXPECT_TRUE(timeline.needs_silence_at(last + (2 * kBufferPeriodNs) + 1));
}

TEST(AudioTimeline, TheWatchdogAsksForExactlyTheMissingDuration) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));

    const std::int64_t now = buffer_time(0) + (500 * kBufferPeriodNs); // 10 s later
    const std::int64_t missing = timeline.frames_missing_at(now);
    EXPECT_EQ(missing, (10LL * kRate) - kFramesPerBuffer);

    const TimelineSegment segment = timeline.inject_silence(missing);
    EXPECT_EQ(segment.silence_frames, missing);
    EXPECT_TRUE(segment.content_is_silence);
    EXPECT_EQ(timeline.frames_missing_at(now), 0) << "the timeline is still short after filling it";
}

// Injection and arrival must agree: a packet landing right after the watchdog
// filled up to `now` must not double-count.
TEST(AudioTimeline, WatchdogInjectionAndAPacketArrivalDoNotOverlap) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));

    const std::int64_t now = buffer_time(100);
    static_cast<void>(timeline.inject_silence(timeline.frames_missing_at(now)));
    const std::int64_t after_injection = timeline.frames_written();

    // A packet arrives covering ground the watchdog just filled.
    const TimelineSegment segment = timeline.accept(packet_at(buffer_time(99)));
    EXPECT_TRUE(segment.dropped) << "audio was appended behind the write head";
    EXPECT_EQ(timeline.frames_written(), after_injection);
}

TEST(AudioTimeline, APartiallyOverlappingPacketContributesOnlyItsNewTail) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));
    static_cast<void>(timeline.inject_silence(kFramesPerBuffer)); // now at 2 buffers

    // A packet starting one buffer back but running two buffers long: only its
    // second half is new.
    const TimelineSegment segment = timeline.accept(packet_at(buffer_time(1), 2 * kFramesPerBuffer));
    ASSERT_FALSE(segment.dropped);
    EXPECT_EQ(segment.audio_frames, kFramesPerBuffer) << "the overlapping head was written twice";
    EXPECT_EQ(timeline.frames_written(), 3 * kFramesPerBuffer);
}

// ---------------------------------------------------------------------------
// WASAPI's flags (SPEC.md §8.2)
// ---------------------------------------------------------------------------

// A SILENT buffer has undefined contents. Filling it with zeros keeps the
// timeline honest; *skipping* it shortens the track by its duration, which is the
// same defect as an unfilled gap arriving by a different route.
TEST(AudioTimeline, ASilentFlaggedBufferOccupiesItsFullDuration) {
    AudioTimeline timeline = started_timeline();

    CapturedPacket packet = packet_at(buffer_time(0));
    packet.flags.silent = true;

    const TimelineSegment segment = timeline.accept(packet);
    ASSERT_FALSE(segment.dropped);
    EXPECT_EQ(segment.audio_frames, kFramesPerBuffer) << "a SILENT buffer was skipped rather than filled";
    EXPECT_TRUE(segment.content_is_silence);
    EXPECT_EQ(timeline.frames_written(), kFramesPerBuffer);
}

TEST(AudioTimeline, AStreamOfSilentBuffersStillTracksWallClock) {
    AudioTimeline timeline = started_timeline();

    for (std::int64_t i = 0; i < 250; ++i) { // 5 s, all flagged silent
        CapturedPacket packet = packet_at(buffer_time(i));
        packet.flags.silent = true;
        static_cast<void>(timeline.accept(packet));
    }

    EXPECT_NEAR(static_cast<double>(timeline.frames_written()) / kRate, 5.0, 0.001);
    EXPECT_EQ(timeline.silence_frames_injected(), timeline.frames_written());
}

// A discontinuity is bridged with silence and counted -- never concatenated,
// which is precisely how drift accumulates (SPEC.md §8.2).
TEST(AudioTimeline, ADiscontinuityIsBridgedAndCounted) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));

    CapturedPacket packet = packet_at(buffer_time(50)); // 1 s later
    packet.flags.discontinuity = true;

    const TimelineSegment segment = timeline.accept(packet);
    ASSERT_FALSE(segment.dropped);
    EXPECT_EQ(segment.silence_frames, kRate - kFramesPerBuffer) << "the dropped samples were concatenated over";
    EXPECT_EQ(timeline.discontinuities(), 1);
}

// ---------------------------------------------------------------------------
// Long-run exactness -- what SPEC.md §20 row 4 needs
// ---------------------------------------------------------------------------

// row 4 allows |offset| < 20 ms at every 60 s mark across four hours. Frame
// positions are computed from t0 rather than accumulated, so error cannot build.
TEST(AudioTimeline, NoDriftOverFourHoursOfContinuousAudio) {
    AudioTimeline timeline = started_timeline();

    constexpr std::int64_t kBuffers = 4LL * 3600 * 50; // 4 h at 50 buffers/s
    for (std::int64_t i = 0; i < kBuffers; ++i) {
        static_cast<void>(timeline.accept(packet_at(buffer_time(i))));
    }

    const double seconds = static_cast<double>(timeline.frames_written()) / kRate;
    EXPECT_NEAR(seconds, 4.0 * 3600.0, 0.001) << "audio duration drifted from wall clock";
}

// The same, with the stream cutting in and out throughout -- the realistic case,
// and the one where a naive implementation's error compounds.
TEST(AudioTimeline, NoDriftAcrossManyAlternatingSilenceAndAudioStretches) {
    AudioTimeline timeline = started_timeline();

    constexpr std::int64_t kBuffers = 3600LL * 50; // 1 h
    std::int64_t supplied = 0;
    for (std::int64_t i = 0; i < kBuffers; ++i) {
        // 10 s on, 10 s off, repeatedly.
        const bool playing = ((i / 500) % 2) == 0;
        if (!playing) {
            continue;
        }
        static_cast<void>(timeline.accept(packet_at(buffer_time(i))));
        ++supplied;
    }

    const double seconds = static_cast<double>(timeline.frames_written()) / kRate;
    // The last stretch is silent, so the timeline ends at the final *packet*, not
    // at the hour mark; the watchdog covers the tail in production.
    EXPECT_GT(seconds, 3500.0);
    EXPECT_LT(seconds, 3600.0);

    // Half the content was manufactured, and the audio that did arrive is intact.
    const double silence_seconds = static_cast<double>(timeline.silence_frames_injected()) / kRate;
    EXPECT_NEAR(silence_seconds, seconds - (static_cast<double>(supplied * kFramesPerBuffer) / kRate), 0.05);
}

// ---------------------------------------------------------------------------
// Misuse
// ---------------------------------------------------------------------------

// SPEC.md §7.1: t0 is shared with video and negotiated by the session. An audio
// packet must not be able to define it.
TEST(AudioTimeline, PacketsBeforeStartAreRefusedRatherThanDefiningTheEpoch) {
    AudioTimeline timeline(kRate, kBufferPeriodNs);
    EXPECT_FALSE(timeline.started());

    const TimelineSegment segment = timeline.accept(packet_at(buffer_time(0)));
    EXPECT_TRUE(segment.dropped);
    EXPECT_EQ(timeline.frames_written(), 0);
}

// BUG-014. Audio can genuinely predate a shared epoch pinned by the first video
// frame, because `t0` is the *later* of the two firsts (SPEC.md §7.1). A packet
// entirely before it belongs nowhere on the timeline -- clamping it to index 0
// writes pre-epoch audio *at* the epoch and displaces the audio that actually
// belongs there.
TEST(AudioTimeline, APacketEntirelyBeforeT0IsDroppedRatherThanClampedToZero) {
    AudioTimeline timeline(kRate, kBufferPeriodNs);
    timeline.start(kNsPerSecond);

    const TimelineSegment segment = timeline.accept(packet_at(0));
    EXPECT_TRUE(segment.dropped);
    EXPECT_EQ(segment.audio_frames, 0);
    EXPECT_EQ(timeline.frames_written(), 0);
}

// A packet *straddling* `t0` contributes only the part that follows it, trimmed
// from the front. Getting this wrong by one buffer is the 20 ms of misplaced
// audio at the head of every recording whose endpoint opened first.
TEST(AudioTimeline, APacketStraddlingT0ContributesOnlyThePartAfterIt) {
    constexpr std::int64_t kT0 = kNsPerSecond;
    AudioTimeline timeline(kRate, kBufferPeriodNs);
    timeline.start(kT0);

    // Starts 5 ms before t0 and runs for a 20 ms buffer, so 15 ms of it is inside.
    CapturedPacket packet;
    packet.qpc_ns = kT0 - 5'000'000;
    packet.frames = kFramesPerBuffer;

    const TimelineSegment segment = timeline.accept(packet);
    ASSERT_FALSE(segment.dropped);
    EXPECT_EQ(segment.start_frame, 0);
    EXPECT_EQ(segment.silence_frames, 0);
    EXPECT_EQ(segment.audio_frames, (15 * kRate) / 1000) << "the pre-epoch head was not trimmed";
    EXPECT_EQ(timeline.frames_written(), (15 * kRate) / 1000);

    // And the next buffer lands exactly where its own timestamp says, with no gap.
    CapturedPacket next;
    next.qpc_ns = kT0 + 15'000'000;
    next.frames = kFramesPerBuffer;
    const TimelineSegment following = timeline.accept(next);
    EXPECT_EQ(following.silence_frames, 0);
    EXPECT_EQ(following.audio_frames, kFramesPerBuffer);
}

TEST(AudioTimeline, InjectingZeroOrNegativeSilenceIsANoOp) {
    AudioTimeline timeline = started_timeline();
    static_cast<void>(timeline.accept(packet_at(buffer_time(0))));
    const std::int64_t before = timeline.frames_written();

    EXPECT_EQ(timeline.inject_silence(0).silence_frames, 0);
    EXPECT_EQ(timeline.inject_silence(-100).silence_frames, 0);
    EXPECT_EQ(timeline.frames_written(), before);
}

} // namespace
