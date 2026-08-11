// The paused total (SPEC.md §7.5, §20 row 18).
//
// CPU TIER. The component is three atomics and one subtraction, and every property
// SPEC.md §7.5 states about it -- idempotence, excision, the shared total -- is
// expressible against a synthetic clock. That is deliberate: row 18's failure mode is a
// *silent* one, and a pure component is the only place it can be pinned exactly rather
// than measured with a tolerance.

#include "core/timing/pause_clock.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace {

using fc::timing::PauseClock;
using State = fc::timing::PauseClock::State;

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSecond = 1'000'000'000;

// ---------------------------------------------------------------------------
// The mapping itself
// ---------------------------------------------------------------------------

TEST(PauseClock, AnUnpausedRecordingIsTheSpecSevenPointOneMappingUnchanged) {
    const PauseClock clock;
    constexpr std::int64_t kT0 = (500 * kMs);

    EXPECT_EQ(clock.paused_total_ns(), 0);
    EXPECT_FALSE(clock.paused());

    const PauseClock::Mapping mapped = clock.map(kT0 + (3 * kSecond), kT0);
    EXPECT_EQ(mapped.state, State::Running);
    EXPECT_EQ(mapped.timeline_ns, (3 * kSecond)) << "an unpaused recording must be exactly qpc - t0";
}

// The whole of SPEC.md §7.5 in one assertion: a frame captured 30 s of wall clock
// after `t0`, with a 10 s pause behind it, belongs at 20 s in the file.
TEST(PauseClock, TimeInsideAPausedSpanIsExcisedFromEverythingAfterIt) {
    PauseClock clock;
    constexpr std::int64_t kT0 = 0;

    ASSERT_TRUE(clock.pause(kT0 + (10 * kSecond)));
    ASSERT_TRUE(clock.resume(kT0 + (20 * kSecond)));

    EXPECT_EQ(clock.paused_total_ns(), (10 * kSecond));

    const PauseClock::Mapping mapped = clock.map(kT0 + (30 * kSecond), kT0);
    EXPECT_EQ(mapped.state, State::Running);
    EXPECT_EQ(mapped.timeline_ns, (20 * kSecond))
        << "SPEC.md §7.5: 30 s of wall clock minus a 10 s pause is 20 s of file";
}

// Excised means excised. An item timestamped inside the span has no correct place on
// the timeline, and answering with a number -- zero, or the seam -- would place paused
// content at the resume, which is the frozen-frame outcome §7.5 rules out by name.
TEST(PauseClock, ATimestampInsideAPausedSpanIsRefusedRatherThanPlaced) {
    PauseClock clock;
    ASSERT_TRUE(clock.pause(10 * kSecond));

    EXPECT_EQ(clock.map((10 * kSecond), 0).state, State::Excised) << "the pause instant itself is inside the span";
    EXPECT_EQ(clock.map((15 * kSecond), 0).state, State::Excised);

    // Material captured *before* the pause is still perfectly placeable while the
    // pause is open -- that is what lets the pipeline drain what is in flight.
    const PauseClock::Mapping before = clock.map((9 * kSecond), 0);
    EXPECT_EQ(before.state, State::Running);
    EXPECT_EQ(before.timeline_ns, (9 * kSecond));
}

TEST(PauseClock, PausedTimeAccumulatesAcrossEveryPause) {
    PauseClock clock;

    ASSERT_TRUE(clock.pause(10 * kSecond));
    ASSERT_TRUE(clock.resume(13 * kSecond));
    ASSERT_TRUE(clock.pause(20 * kSecond));
    ASSERT_TRUE(clock.resume(27 * kSecond));
    ASSERT_TRUE(clock.pause(40 * kSecond));
    ASSERT_TRUE(clock.resume(41500 * kMs));

    EXPECT_EQ(clock.pauses(), 3U);
    EXPECT_EQ(clock.paused_total_ns(), (3 * kSecond) + (7 * kSecond) + (1500 * kMs));

    // 60 s of wall clock, 11.5 s of it excised.
    EXPECT_EQ(clock.map((60 * kSecond), 0).timeline_ns, (48500 * kMs));
}

// A pause still open has no duration yet. Reporting one would make `get_stats` show a
// growing total for a recording that has not resumed, and the §10.4 gate reads the same
// number.
TEST(PauseClock, AnOpenPauseContributesNothingUntilItCloses) {
    PauseClock clock;
    ASSERT_TRUE(clock.pause(10 * kSecond));

    EXPECT_TRUE(clock.paused());
    EXPECT_EQ(clock.paused_total_ns(), 0);
    EXPECT_EQ(clock.pauses(), 0U);

    ASSERT_TRUE(clock.resume(14 * kSecond));
    EXPECT_EQ(clock.paused_total_ns(), (4 * kSecond));
    EXPECT_EQ(clock.pauses(), 1U);
}

// ---------------------------------------------------------------------------
// SPEC.md §7.5's idempotence invariant
// ---------------------------------------------------------------------------

// "Pause and resume are idempotent: pausing a paused recording is a no-op that
// succeeds, not an error." The return value distinguishes "nothing changed" from
// "failed", which is what lets the IPC layer answer `ok` while suppressing a duplicate
// `state_changed` event.
TEST(PauseClock, PausingAPausedRecordingChangesNothingAndSaysSo) {
    PauseClock clock;

    EXPECT_TRUE(clock.pause(10 * kSecond));
    EXPECT_FALSE(clock.pause(12 * kSecond)) << "the second pause must not move the span's start";
    EXPECT_FALSE(clock.pause(90 * kSecond));

    ASSERT_TRUE(clock.resume(20 * kSecond));
    EXPECT_EQ(clock.paused_total_ns(), (10 * kSecond)) << "the span began at the first pause, not the last";
}

TEST(PauseClock, ResumingARunningRecordingChangesNothingAndSaysSo) {
    PauseClock clock;

    EXPECT_FALSE(clock.resume(10 * kSecond));
    EXPECT_EQ(clock.paused_total_ns(), 0);
    EXPECT_EQ(clock.pauses(), 0U);

    ASSERT_TRUE(clock.pause(10 * kSecond));
    ASSERT_TRUE(clock.resume(20 * kSecond));
    EXPECT_FALSE(clock.resume(30 * kSecond)) << "a second resume must not extend the span";
    EXPECT_EQ(clock.paused_total_ns(), (10 * kSecond));
}

// QPC is monotonic, so this cannot happen from the clock. It can happen from a
// transposed pair of timestamps crossing the IPC boundary, and the consequence would be
// a *negative* contribution to the total -- which lengthens the timeline for everything
// after it and desyncs the two streams by exactly that amount, permanently.
TEST(PauseClock, AResumeBeforeItsOwnPauseContributesZeroRatherThanNegativeTime) {
    PauseClock clock;
    ASSERT_TRUE(clock.pause(20 * kSecond));
    ASSERT_TRUE(clock.resume(15 * kSecond));

    EXPECT_EQ(clock.paused_total_ns(), 0);
    EXPECT_FALSE(clock.paused());
    EXPECT_EQ(clock.map((30 * kSecond), 0).timeline_ns, (30 * kSecond));
}

// ---------------------------------------------------------------------------
// The property row 18 exists to protect
// ---------------------------------------------------------------------------

// Row 18's real failure is not a wrong number, it is *two* wrong numbers that disagree.
// This is the assertion that a video frame and an audio packet describing the same
// instant land at the same place on the timeline no matter how many pauses precede them
// -- which is the entire reason §7.5 puts the total in one shared object.
TEST(PauseClock, VideoAndAudioMappingTheSameInstantAlwaysAgree) {
    PauseClock clock;
    constexpr std::int64_t kT0 = (250 * kMs);

    struct Span {
        std::int64_t begin;
        std::int64_t end;
    };

    const std::vector<Span> spans{
        {(2 * kSecond), (5 * kSecond)}, {(9 * kSecond), (9500 * kMs)}, {(30 * kSecond), (95 * kSecond)}};

    std::int64_t expected_excised = 0;
    for (const Span& span : spans) {
        ASSERT_TRUE(clock.pause(kT0 + span.begin));
        ASSERT_TRUE(clock.resume(kT0 + span.end));
        expected_excised += span.end - span.begin;

        // Both streams ask the *same* object the same question. There is no second
        // total to disagree with, which is the structural half of the guarantee.
        const std::int64_t instant = kT0 + span.end + (400 * kMs);
        const PauseClock::Mapping video = clock.map(instant, kT0);
        const PauseClock::Mapping audio = clock.map(instant, kT0);

        ASSERT_EQ(video.state, State::Running);
        EXPECT_EQ(video.timeline_ns, audio.timeline_ns);
        EXPECT_EQ(video.timeline_ns, span.end + (400 * kMs) - expected_excised);
    }

    EXPECT_EQ(clock.paused_total_ns(), expected_excised);
    EXPECT_EQ(clock.stragglers(), 0U);
}

// ---------------------------------------------------------------------------
// Stragglers
// ---------------------------------------------------------------------------

// The pipeline quiesces at the pause boundary so this cannot arise. The counter exists
// because "cannot arise" is a claim about code in another file, and row 18 asserts the
// count rather than trusting the claim. Here the straggler is manufactured deliberately,
// to prove the counter discriminates -- a counter that never fires is not evidence.
TEST(PauseClock, AnItemCapturedBeforeAPauseButMappedAfterItIsCorrectedAndCounted) {
    PauseClock clock;
    ASSERT_TRUE(clock.pause(10 * kSecond));
    ASSERT_TRUE(clock.resume(18 * kSecond));

    EXPECT_EQ(clock.stragglers(), 0U);

    // Captured at 9 s -- before the pause -- but mapped now. Its correct place is 9 s,
    // not 1 s, because the 8 s that were excised had not happened when it was captured.
    const PauseClock::Mapping late = clock.map((9 * kSecond), 0);
    EXPECT_EQ(late.state, State::Running);
    EXPECT_EQ(late.timeline_ns, (9 * kSecond));
    EXPECT_EQ(clock.stragglers(), 1U);

    // The negative control: an item captured after the resume is not a straggler and
    // must not be corrected.
    const PauseClock::Mapping ordinary = clock.map((20 * kSecond), 0);
    EXPECT_EQ(ordinary.timeline_ns, (12 * kSecond));
    EXPECT_EQ(clock.stragglers(), 1U) << "an ordinary item was counted as a straggler";
}

// `observe` is `map` without the bookkeeping, for a caller asking where the timeline is
// rather than where an item belongs (BUG-038: SPEC.md §8.4's drift ladder). The arithmetic
// must be identical in every state -- two copies of §7.5's subtraction is the defect this
// class exists to prevent -- and only the counter may differ.
TEST(PauseClock, ObserveIsMapWithoutTheStragglerBookkeeping) {
    PauseClock clock;
    ASSERT_TRUE(clock.pause(10 * kSecond));

    // Inside an open span both refuse, and for the same reason.
    EXPECT_EQ(clock.observe((12 * kSecond), 0).state, State::Excised);
    EXPECT_EQ(clock.map((12 * kSecond), 0).state, State::Excised);

    ASSERT_TRUE(clock.resume(18 * kSecond));
    EXPECT_EQ(clock.stragglers(), 0U);

    // The ordinary case: same answer, byte for byte.
    for (const std::int64_t at : {(20 * kSecond), (25 * kSecond), (100 * kSecond)}) {
        const PauseClock::Mapping observed = clock.observe(at, 0);
        const PauseClock::Mapping mapped = clock.map(at, 0);
        EXPECT_EQ(observed.state, mapped.state);
        EXPECT_EQ(observed.timeline_ns, mapped.timeline_ns) << "at " << at;
    }
    EXPECT_EQ(clock.stragglers(), 0U);

    // The straggler case: same answer, and only `map` counts. A measurement that
    // consulted the clock in the same instant as a late item must not be able to inflate
    // a number SPEC.md §20 row 18 asserts is zero.
    const PauseClock::Mapping observed_late = clock.observe((9 * kSecond), 0);
    EXPECT_EQ(observed_late.timeline_ns, (9 * kSecond)) << "the stale-total correction was not applied";
    EXPECT_EQ(clock.stragglers(), 0U) << "`observe` counted a straggler";

    const PauseClock::Mapping mapped_late = clock.map((9 * kSecond), 0);
    EXPECT_EQ(mapped_late.timeline_ns, observed_late.timeline_ns);
    EXPECT_EQ(clock.stragglers(), 1U) << "`map` stopped counting";
}

// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------

// `map` runs on the venc and aenc threads while pause/resume runs on the thread
// handling the IPC command. Nothing here may tear: a reader must never observe a total
// that includes half of a span. The assertion is that every observation is one of the
// finitely many *legal* values, not merely that the program did not crash.
//
// **The barrier is the load-bearing part, not the span count.** Written without it,
// 200 spans of pure atomic arithmetic completed before either reader thread had been
// scheduled, and the case passed having observed nothing at all -- measured, 0
// observations. The `EXPECT_GT` below is what caught that, and it stays as the guard
// against the same thing recurring on a faster machine or a busier one.
TEST(PauseClock, ConcurrentReadersNeverObserveAHalfAppliedPause) {
    PauseClock clock;
    constexpr int kSpans = 20000;
    constexpr std::int64_t kSpanNs = (1 * kMs);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> illegal{0};
    std::atomic<std::uint64_t> before_writer{0};
    std::atomic<std::uint64_t> observations{0};

    auto reader = [&] {
        while (!stop.load(std::memory_order_acquire)) {
            const std::int64_t total = clock.paused_total_ns();
            // Every legal total is an exact multiple of the span length, because every
            // span is the same length. A torn read is not.
            if (total % kSpanNs != 0 || total < 0 || total > kSpans * kSpanNs) {
                illegal.fetch_add(1, std::memory_order_relaxed);
            }
            observations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread video(reader);
    std::thread audio(reader);

    // Both readers must be *running* before the writer starts, or the writer finishes
    // first and this case measures nothing. Two observations is not proof that both
    // threads are up, so wait for enough that neither can plausibly be the only one.
    while (observations.load(std::memory_order_relaxed) < 1000) {
        std::this_thread::yield();
    }
    before_writer.store(observations.load(std::memory_order_relaxed), std::memory_order_relaxed);

    for (int i = 0; i < kSpans; ++i) {
        const std::int64_t begin = static_cast<std::int64_t>(i) * (10 * kMs);
        ASSERT_TRUE(clock.pause(begin));
        ASSERT_TRUE(clock.resume(begin + kSpanNs));
    }

    stop.store(true, std::memory_order_release);
    video.join();
    audio.join();

    EXPECT_EQ(illegal.load(), 0U);
    EXPECT_GT(observations.load(), before_writer.load())
        << "the readers stopped before the writer ran, so this proved nothing";
    EXPECT_EQ(clock.paused_total_ns(), kSpans * kSpanNs);
    EXPECT_EQ(clock.pauses(), static_cast<std::uint64_t>(kSpans));
}

} // namespace
