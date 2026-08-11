#pragma once

// The GUI/engine process contract (SPEC.md §3.1, §20 row 13).
//
//   * GUI spawns the engine inside a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
//   * Bidirectional heartbeat, 1 s interval, 5 s timeout.
//   * Console control handler + a hidden `WM_QUERYENDSESSION` window.
//   * Exactly one engine instance per user session, via a named mutex.
//
// ===========================================================================
// A conflict inside §3.1, and how this implements it. **This needs the owner.**
// ===========================================================================
// §3.1 states two rules that cannot both hold when the GUI is *killed* rather than
// closed:
//
//   1. "GUI spawns the engine as a child process inside a Windows Job Object with
//      `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`."
//   2. "GUI heartbeat lost -> engine **finalizes the current recording cleanly**, then
//      exits. It does **not** discard the file."
//
// `KILL_ON_JOB_CLOSE` terminates every process in the job when the *last handle* to the
// job closes. Terminating a killed GUI closes its handles, so if the GUI holds the only
// handle the engine is killed by the kernel in the same instant -- with no notification,
// no unwinding, and no opportunity to run rule 2's clean finalize. The two rules would
// then be in a race the kernel always wins.
//
// SPEC.md §20 row 13's test text is what settles which reading was intended:
//
//   > `test_orphan_prevention`: kill the GUI, assert the engine exits within **6 s**
//   > having **finalized** the file.
//
// Six seconds is 5 s of heartbeat timeout plus one. A number chosen for the heartbeat
// path, applied to the case where the GUI is killed, only makes sense if the engine is
// expected to *survive* the GUI's death long enough to use it. So this implementation
// has the engine **open its own handle to the job**: the job then outlives the GUI, the
// heartbeat notices within 5 s, the recording finalizes, the engine exits, and closing
// the engine's handle -- the last one -- dissolves the job.
//
// **What that costs, stated plainly because it is the reason this needs the owner's
// pen.** With the engine holding a handle, `KILL_ON_JOB_CLOSE` can no longer reap an
// engine that is *frozen*: a process that cannot run its heartbeat thread also cannot
// close its handle. The residual orphan case is therefore "engine wedged **and** GUI
// gone", where the pure-job reading would have reaped it unconditionally at the cost of
// never finalizing cleanly. Neither reading covers both.
//
// The trade taken here is deliberate and is the one the spec's own test asks for: a
// clean, playable file in the common case (CLAUDE.md §1's prime directive), against a
// residual wedged-engine case that no in-process mechanism can cover anyway. It is
// mitigated rather than ignored -- the heartbeat watchdog is a dedicated thread doing
// nothing but a timed wait, so the only thing that stops it is the whole process being
// frozen, and `GuiWatchdog` bounds its own finalize with a deadline after which it
// closes the job handle and lets the kernel finish the job.
//
// The alternative -- engine holds no handle, job kill is unconditional, and row 13's
// "finalized" is delivered by §10.4's repair path on the fragment-consistent file the
// kill leaves behind -- is coherent and weaker on the prime directive. See
// docs/ACCEPTANCE.md's open questions.

#include "core/error/result.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fc::ipc {

/// SPEC.md §3.1's heartbeat interval and timeout.
inline constexpr std::chrono::milliseconds kHeartbeatInterval{1000};
inline constexpr std::chrono::milliseconds kHeartbeatTimeout{5000};

/// The environment variable the host uses to hand the engine its job's name.
///
/// An environment variable rather than a command-line argument because it must not
/// appear in a process listing next to the output path -- and because the engine's
/// command line is a stable surface a user may script against, while this is an
/// implementation detail of the pairing.
inline constexpr const char* kJobNameEnvVar = "FC_ENGINE_JOB";

/// The environment variable carrying the control pipe's session id.
inline constexpr const char* kSessionEnvVar = "FC_ENGINE_SESSION";

// ---------------------------------------------------------------------------
// One engine per user session (SPEC.md §3.1)
// ---------------------------------------------------------------------------

/// Holds the named mutex that makes a second engine refuse to start.
///
/// `Local\` scope, not `Global\`: §3.1 says "per user session", and a `Global\` mutex
/// would make one user's engine block another user's on a shared machine -- which is a
/// worse failure than the one it prevents.
class SingleInstanceGuard {
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard(SingleInstanceGuard&&) = delete;
    SingleInstanceGuard& operator=(SingleInstanceGuard&&) = delete;

    /// Acquires the mutex. `IPC_ENGINE_ALREADY_RUNNING` when another engine holds it.
    [[nodiscard]] Result<void> acquire(std::string_view name = "framecapture-engine");

    [[nodiscard]] bool held() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// The Job Object, from both ends
// ---------------------------------------------------------------------------

/// The **host** side: creates the job and puts spawned children in it.
///
/// This is the GUI's role in §3.1. It lives in `fc_core` so the M8a headless driver and
/// SPEC.md §20 row 13's test drive the same code the Python GUI will reimplement over
/// `ctypes`, rather than a stand-in for it.
class EngineJob {
public:
    EngineJob();
    ~EngineJob();

    EngineJob(const EngineJob&) = delete;
    EngineJob& operator=(const EngineJob&) = delete;
    EngineJob(EngineJob&&) = delete;
    EngineJob& operator=(EngineJob&&) = delete;

    /// Creates a named job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
    ///
    /// Named so the child can open it -- see the header note. Empty generates a name.
    [[nodiscard]] Result<void> create(std::string_view name = {});

    /// Puts an already-created process into the job.
    ///
    /// Takes a raw `HANDLE` as `void*` so this header stays free of `<windows.h>`, which
    /// every translation unit including it would otherwise inherit.
    [[nodiscard]] Result<void> assign(void* process_handle);

    [[nodiscard]] std::string name() const;

    /// Closes the job handle. If this was the last handle, every process in the job is
    /// terminated by the kernel -- which is the whole point.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The **engine** side: opens a handle to the job it was placed in.
///
/// Reads `FC_ENGINE_JOB`. Absent -- an engine started by hand, or by a test -- is not an
/// error: the engine is then simply not managed by a host, which is a supported way to
/// run it and is how every GPU-tier test that is not row 13 uses it.
///
/// Returns whether a handle is held, so the caller can log the difference between
/// "hosted" and "standalone" rather than guessing.
class JobMembership {
public:
    JobMembership();
    ~JobMembership();

    JobMembership(const JobMembership&) = delete;
    JobMembership& operator=(const JobMembership&) = delete;
    JobMembership(JobMembership&&) = delete;
    JobMembership& operator=(JobMembership&&) = delete;

    [[nodiscard]] Result<void> open_from_environment();

    [[nodiscard]] bool hosted() const noexcept;

    /// Releases the job handle. When the host is already gone this is what lets
    /// `KILL_ON_JOB_CLOSE` fire -- so it is the engine's own last resort against
    /// outliving its host, and is called on every exit path.
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Shutdown signals (SPEC.md §3.1)
// ---------------------------------------------------------------------------

/// Installs a console control handler and a hidden message window handling
/// `WM_QUERYENDSESSION` / `WM_ENDSESSION`, "so OS shutdown triggers a clean trailer
/// write" (§3.1).
///
/// Both are needed and they cover different events. The console handler catches
/// Ctrl+C, Ctrl+Break and `CTRL_CLOSE_EVENT`; only a window receives the session-end
/// messages, and a console process without one is terminated at log-off with whatever
/// was on disk. For a recording that is the difference between a finalized file and one
/// the §10.4 repair path has to reconstruct.
///
/// The callback runs on the OS's thread with a **hard deadline** -- Windows gives a
/// console handler about 5 s and `WM_ENDSESSION` rather less -- so it must ask for a
/// stop and return, never finalize inline.
class ShutdownSignals {
public:
    ShutdownSignals();
    ~ShutdownSignals();

    ShutdownSignals(const ShutdownSignals&) = delete;
    ShutdownSignals& operator=(const ShutdownSignals&) = delete;
    ShutdownSignals(ShutdownSignals&&) = delete;
    ShutdownSignals& operator=(ShutdownSignals&&) = delete;

    /// `on_shutdown` is invoked once, from the OS's thread. It must return promptly.
    [[nodiscard]] Result<void> install(std::function<void()> on_shutdown);

    void uninstall();

    /// Which signal fired, for the log. Empty until one does.
    [[nodiscard]] std::string reason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// The heartbeat (SPEC.md §3.1)
// ---------------------------------------------------------------------------

/// Watches for the host going quiet and calls back once when it does.
///
/// **What counts as a heartbeat, and why it is not a new command.** §15.1 fixes the
/// command list at sixteen and `heartbeat` is not among them, so inventing one would
/// extend a specified protocol surface. It is not needed: §15.1 already has the engine
/// emitting `stats` at 2 Hz -- the engine's half of §3.1's heartbeat, at four times the
/// required rate -- and a GUI that displays those stats is polling `get_stats` anyway.
/// So **any** inbound message is the host's heartbeat, and a GUI need only make sure it
/// sends one within the interval.
///
/// The consequence is worth stating: liveness is proven by traffic rather than by a
/// dedicated ping, so a GUI that goes silent because it is busy looks the same as one
/// that has died. That is what the 5 s timeout is for -- it is five times the interval,
/// not one.
class GuiWatchdog {
public:
    GuiWatchdog();
    ~GuiWatchdog();

    GuiWatchdog(const GuiWatchdog&) = delete;
    GuiWatchdog& operator=(const GuiWatchdog&) = delete;
    GuiWatchdog(GuiWatchdog&&) = delete;
    GuiWatchdog& operator=(GuiWatchdog&&) = delete;

    /// Starts watching. `on_lost` fires **once**, on the watchdog thread, when nothing
    /// has arrived for `kHeartbeatTimeout`.
    ///
    /// Not armed until `notify` has been called at least once: an engine that has never
    /// had a client must not decide its client is late.
    [[nodiscard]] Result<void> start(std::function<void()> on_lost);

    /// Records a sign of life. Called for every inbound message; one relaxed store.
    void notify() noexcept;

    /// Stops counting silence for as long as the returned guard lives (BUG-039).
    ///
    /// **The engine is single-threaded on the control pipe**: one thread reads a request,
    /// dispatches it, and only then reads the next. So while a command is executing,
    /// nothing can call `notify` -- not because the host has gone quiet, but because the
    /// engine is not listening. Measured in the field: a `stop_record` on a 112-second
    /// recording spent 14.3 s finalizing, the watchdog saw 5169 ms of "silence" at the
    /// 5000 ms mark, and the engine logged `host heartbeat lost` and exited **while
    /// finalizing the recording that host had just asked it for**.
    ///
    /// A request in flight is itself proof the host was alive when it was sent, and the
    /// host is blocked on the reply, so silence during a command carries no information
    /// about the host at all. On release the clock is re-armed from *now* rather than
    /// from the last `notify`: the engine genuinely does not know whether the host
    /// survived the intervening seconds, so it starts a fresh timeout rather than
    /// crediting or condemning it.
    ///
    /// SPEC.md §20 row 13's 6 s budget is measured from the host's death during a
    /// *recording*, where no command is in flight, so this does not extend it. What it
    /// does extend is the pathological case of the host dying during a long command, to
    /// that command's duration plus the timeout -- which is the correct answer, because
    /// killing the engine mid-finalize is what CLAUDE.md §1 exists to prevent.
    class [[nodiscard]] Suspension {
    public:
        explicit Suspension(GuiWatchdog& watchdog) noexcept;
        ~Suspension();

        Suspension(const Suspension&) = delete;
        Suspension& operator=(const Suspension&) = delete;
        Suspension(Suspension&&) = delete;
        Suspension& operator=(Suspension&&) = delete;

    private:
        GuiWatchdog* watchdog_;
    };

    /// Whether a `Suspension` is currently held. For tests.
    [[nodiscard]] bool suspended() const noexcept;

    void stop();

    /// Nanoseconds since the last `notify`, or 0 before the first.
    [[nodiscard]] std::int64_t silence_ns() const noexcept;

    [[nodiscard]] bool fired() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::ipc
