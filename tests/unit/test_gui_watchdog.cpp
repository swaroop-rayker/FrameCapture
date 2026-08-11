// The engine's host-liveness watchdog (SPEC.md §3.1, §20 row 13; BUG-039).
//
// CPU TIER. `GuiWatchdog` is a clock and two atomics -- no pipe, no process -- which is
// what makes the case below assertable rather than a five-second wait.
//
// ---------------------------------------------------------------------------
// What BUG-039 was, and why it belongs in a unit test
// ---------------------------------------------------------------------------
// The engine reads one request, dispatches it, and only then reads the next: one thread,
// serially. So while a command is executing, nothing calls `notify` -- not because the
// host stopped talking, but because the engine stopped listening. The watchdog counted
// that as the host's silence.
//
// Measured in the field on a 112-second recording: `stop_record` spent 14.3 s finalizing,
// the watchdog saw 5169 ms against its 5000 ms timeout, and the engine logged
// `host heartbeat lost; finalizing the recording and exiting` **while finalizing the
// recording that host had just asked for**. The host was alive throughout and blocked on
// the reply. Both sides then blamed the other: the GUI's own heartbeat timed out at the
// same moment and reported "the engine went away".
//
// `notify()` before dispatch -- which the code already did, with a comment saying it was
// so "a slow command does not make the host look dead" -- cannot fix this. The timestamp
// is fresh at the *start* of the command; the problem is the 14 s after it.

#include "core/ipc/lifecycle.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using fc::ipc::GuiWatchdog;

/// Long enough to pass the timeout and the watchdog's quarter-interval poll, short
/// enough to keep the suite quick. The timeout is 5 s (SPEC.md §3.1), so this cannot be
/// shortened without a seam into the constants -- and the case is about *whether* it
/// fires, which needs the real one.
constexpr auto kPastTheTimeout = fc::ipc::kHeartbeatTimeout + std::chrono::milliseconds{1500};

TEST(GuiWatchdog, ASilentHostIsReportedLost) {
    // The positive control. Without it the suspension case below proves only that
    // nothing ever fires, which would pass on a watchdog that was simply broken.
    std::atomic<int> lost{0};
    GuiWatchdog watchdog;
    ASSERT_TRUE(watchdog.start([&lost] { lost.fetch_add(1); }).has_value());

    watchdog.notify(); // arm it; it does not watch a host it has never heard from
    std::this_thread::sleep_for(kPastTheTimeout);

    EXPECT_EQ(lost.load(), 1) << "a host that went quiet for " << kPastTheTimeout.count()
                              << " ms was not reported lost";
    watchdog.stop();
}

TEST(GuiWatchdog, AHostIsNotDeclaredLostWhileTheEngineIsBusyWithItsOwnCommand) {
    // BUG-039, stated as the thing that actually happened: a command that outlasts the
    // timeout, with a host that is alive and waiting for the reply.
    std::atomic<int> lost{0};
    GuiWatchdog watchdog;
    ASSERT_TRUE(watchdog.start([&lost] { lost.fetch_add(1); }).has_value());

    watchdog.notify(); // the request arrives, and is the host's proof of life
    {
        const GuiWatchdog::Suspension busy{watchdog};
        EXPECT_TRUE(watchdog.suspended());
        std::this_thread::sleep_for(kPastTheTimeout);
        EXPECT_EQ(lost.load(), 0) << "the engine decided its host was dead while executing that host's command";
    }
    EXPECT_FALSE(watchdog.suspended());

    watchdog.stop();
    EXPECT_EQ(lost.load(), 0);
}

TEST(GuiWatchdog, TheTimeoutRestartsWhenTheCommandFinishesRatherThanFiringImmediately) {
    // The subtle half. On release the watchdog must not fire on the silence it caused
    // itself -- if it re-armed from the *last* `notify` it would find the whole command's
    // duration waiting for it and fire on the next poll, which is the original bug with
    // an extra step.
    std::atomic<int> lost{0};
    GuiWatchdog watchdog;
    ASSERT_TRUE(watchdog.start([&lost] { lost.fetch_add(1); }).has_value());

    watchdog.notify();
    {
        const GuiWatchdog::Suspension busy{watchdog};
        std::this_thread::sleep_for(kPastTheTimeout);
    }

    // Immediately after release the host has not been heard from for longer than the
    // timeout in wall-clock terms, and must still not be called lost.
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    EXPECT_EQ(lost.load(), 0) << "the watchdog re-armed from before the command instead of from its end";
    EXPECT_LT(watchdog.silence_ns(), fc::ipc::kHeartbeatTimeout.count() * 1'000'000LL);

    watchdog.stop();
}

TEST(GuiWatchdog, AHostThatDiesDuringACommandIsStillReportedOnceItFinishes) {
    // Suspension defers detection; it must not cancel it. SPEC.md §3.1 requires the
    // engine to exit when its host is gone, and a host that died mid-command is still
    // gone -- it just cannot be noticed until the engine is listening again.
    std::atomic<int> lost{0};
    GuiWatchdog watchdog;
    ASSERT_TRUE(watchdog.start([&lost] { lost.fetch_add(1); }).has_value());

    watchdog.notify();
    {
        const GuiWatchdog::Suspension busy{watchdog};
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
    }
    // Nothing arrives after the command completes, because the host is gone.
    std::this_thread::sleep_for(kPastTheTimeout);

    EXPECT_EQ(lost.load(), 1) << "a host that died during a command was never reported";
    watchdog.stop();
}

} // namespace
