#include "core/ipc/lifecycle.h"

#include "core/logging/log_fields.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace fc::ipc {
namespace {

/// Widen an ASCII name for the Win32 `W` entry points. The names here are all
/// engine-generated ASCII, so this needs none of the machinery a general conversion
/// would.
[[nodiscard]] std::wstring widen(std::string_view text) {
    return std::wstring{text.begin(), text.end()};
}

[[nodiscard]] std::string environment_value(const char* name) {
    std::array<char, 256> buffer{};
    const DWORD length = ::GetEnvironmentVariableA(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::string{buffer.data(), length};
}

/// The part of `ShutdownSignals` the OS's callbacks can reach.
///
/// Split out of `ShutdownSignals::Impl` rather than exposed from it: `SetConsoleCtrlHandler`
/// and `WNDPROC` both take plain function pointers with no user data, so the state has
/// to be reachable through a file-scope pointer -- and a pointer to a class's private
/// `Impl` would mean widening that type's access for the benefit of two callbacks. This
/// holds only what a callback touches, which is also the only state that is written from
/// an OS-owned thread.
struct ShutdownCore {
    std::function<void()> callback;
    std::atomic<bool> fired{false};

    std::mutex reason_mutex;
    std::string reason;

    void fire(std::string_view why) {
        if (fired.exchange(true)) {
            return; // once, whichever signal wins
        }
        {
            const std::lock_guard lock(reason_mutex);
            reason = std::string{why};
        }
        FC_LOG_WARN(Subsystem::App, "shutdown signal received", LogFields{}.add("reason", std::string{why}));
        if (callback) {
            callback();
        }
    }
};

/// Guarded by an atomic rather than a mutex: the handlers run on OS-owned threads under
/// a hard deadline, and blocking one on a lock another thread holds is how a clean
/// shutdown becomes a kill.
std::atomic<ShutdownCore*> g_shutdown_core{nullptr};

BOOL WINAPI console_handler(DWORD type) {
    ShutdownCore* core = g_shutdown_core.load(std::memory_order_acquire);
    if (core == nullptr) {
        return FALSE;
    }

    const char* why = nullptr;
    switch (type) {
    case CTRL_C_EVENT:
        why = "ctrl_c";
        break;
    case CTRL_BREAK_EVENT:
        why = "ctrl_break";
        break;
    case CTRL_CLOSE_EVENT:
        why = "console_close";
        break;
    case CTRL_LOGOFF_EVENT:
        why = "logoff";
        break;
    case CTRL_SHUTDOWN_EVENT:
        why = "shutdown";
        break;
    default:
        return FALSE;
    }

    core->fire(why);
    // TRUE means handled. Windows still terminates the process after its deadline for
    // the close/logoff/shutdown events, which is exactly why `fire` asks for a stop
    // instead of doing the finalizing itself.
    return TRUE;
}

LRESULT CALLBACK session_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ShutdownCore* core = g_shutdown_core.load(std::memory_order_acquire);

    switch (message) {
    case WM_QUERYENDSESSION:
        if (core != nullptr) {
            core->fire("query_end_session");
        }
        // TRUE: the session may end. Blocking it would be the wrong trade -- a user
        // shutting down should not be stopped by a screen recorder, and the finalize
        // has already been asked for.
        return TRUE;
    case WM_ENDSESSION:
        if (core != nullptr && wparam != 0) {
            core->fire("end_session");
        }
        return 0;
    default:
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SingleInstanceGuard
// ---------------------------------------------------------------------------

struct SingleInstanceGuard::Impl {
    HANDLE mutex = nullptr;
};

SingleInstanceGuard::SingleInstanceGuard() : impl_(std::make_unique<Impl>()) {}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (impl_->mutex != nullptr) {
        ::ReleaseMutex(impl_->mutex);
        ::CloseHandle(impl_->mutex);
        impl_->mutex = nullptr;
    }
}

Result<void> SingleInstanceGuard::acquire(std::string_view name) {
    if (impl_->mutex != nullptr) {
        return ok();
    }

    const std::wstring full = L"Local\\" + widen(name);
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, full.c_str());
    if (mutex == nullptr) {
        return FcError::IPC_ENGINE_ALREADY_RUNNING;
    }

    // `CreateMutex` succeeds for an existing mutex and reports it here. The distinction
    // matters: a handle to someone else's mutex is not ownership, and closing it without
    // noticing would leave this process believing it is the only engine.
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        FC_LOG_ERROR(Subsystem::Ipc, "another engine already owns this user session",
                     LogFields{}.add("mutex", std::string{name}).add_error(FcError::IPC_ENGINE_ALREADY_RUNNING));
        return FcError::IPC_ENGINE_ALREADY_RUNNING;
    }

    impl_->mutex = mutex;
    return ok();
}

bool SingleInstanceGuard::held() const noexcept {
    return impl_->mutex != nullptr;
}

// ---------------------------------------------------------------------------
// EngineJob -- the host's side
// ---------------------------------------------------------------------------

struct EngineJob::Impl {
    HANDLE job = nullptr;
    std::string name;
};

EngineJob::EngineJob() : impl_(std::make_unique<Impl>()) {}

EngineJob::~EngineJob() {
    close();
}

Result<void> EngineJob::create(std::string_view name) {
    if (impl_->job != nullptr) {
        return ok();
    }

    impl_->name = name.empty() ? "framecapture-job-" + std::to_string(::GetCurrentProcessId()) : std::string{name};
    const std::wstring full = L"Local\\" + widen(impl_->name);

    HANDLE job = ::CreateJobObjectW(nullptr, full.c_str());
    if (job == nullptr) {
        FC_LOG_ERROR(Subsystem::Ipc, "CreateJobObject failed",
                     LogFields{}
                         .add("gle", static_cast<std::int64_t>(::GetLastError()))
                         .add_error(FcError::IPC_JOB_OBJECT_FAILED));
        return FcError::IPC_JOB_OBJECT_FAILED;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // SPEC.md §3.1's one required limit. Everything in the job dies when the last handle
    // to it closes -- which is what makes an orphaned engine impossible rather than
    // merely unlikely.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
        ::CloseHandle(job);
        return FcError::IPC_JOB_OBJECT_FAILED;
    }

    impl_->job = job;
    return ok();
}

Result<void> EngineJob::assign(void* process_handle) {
    if (impl_->job == nullptr || process_handle == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (::AssignProcessToJobObject(impl_->job, static_cast<HANDLE>(process_handle)) == 0) {
        FC_LOG_ERROR(Subsystem::Ipc, "AssignProcessToJobObject failed",
                     LogFields{}
                         .add("gle", static_cast<std::int64_t>(::GetLastError()))
                         .add_error(FcError::IPC_JOB_OBJECT_FAILED));
        return FcError::IPC_JOB_OBJECT_FAILED;
    }
    return ok();
}

std::string EngineJob::name() const {
    return impl_->name;
}

void EngineJob::close() {
    if (impl_->job != nullptr) {
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
}

// ---------------------------------------------------------------------------
// JobMembership -- the engine's side
// ---------------------------------------------------------------------------

struct JobMembership::Impl {
    HANDLE job = nullptr;
};

JobMembership::JobMembership() : impl_(std::make_unique<Impl>()) {}

JobMembership::~JobMembership() {
    release();
}

Result<void> JobMembership::open_from_environment() {
    const std::string name = environment_value(kJobNameEnvVar);
    if (name.empty()) {
        // Standalone. Not a failure -- see the header. Logged at INFO because "was this
        // engine hosted" is the first question about any orphan report.
        FC_LOG_INFO(Subsystem::Ipc, "engine is running standalone; no host job object", LogFields{});
        return ok();
    }

    const std::wstring full = L"Local\\" + widen(name);
    // `JOB_OBJECT_QUERY` alone is enough: the handle is held for its *existence*, not
    // for anything done through it, and asking for more rights than that is how a
    // reference ends up refused on a hardened system.
    HANDLE job = ::OpenJobObjectW(JOB_OBJECT_QUERY, FALSE, full.c_str());
    if (job == nullptr) {
        FC_LOG_WARN(Subsystem::Ipc,
                    "could not open the host's job object; a host that dies will kill this engine outright",
                    LogFields{}
                        .add("job", name)
                        .add("gle", static_cast<std::int64_t>(::GetLastError()))
                        .add_error(FcError::IPC_JOB_OBJECT_FAILED));
        return FcError::IPC_JOB_OBJECT_FAILED;
    }

    impl_->job = job;
    FC_LOG_INFO(Subsystem::Ipc, "engine joined the host's job object", LogFields{}.add("job", name));
    return ok();
}

bool JobMembership::hosted() const noexcept {
    return impl_->job != nullptr;
}

void JobMembership::release() {
    if (impl_->job != nullptr) {
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ShutdownSignals
// ---------------------------------------------------------------------------

struct ShutdownSignals::Impl {
    ShutdownCore core;

    HWND window = nullptr;
    std::thread pump;
    std::atomic<bool> pump_running{false};
    HANDLE ready = nullptr;

    void pump_loop();
};

void ShutdownSignals::Impl::pump_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-session");
    try {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = session_window_proc;
        cls.hInstance = ::GetModuleHandleW(nullptr);
        cls.lpszClassName = L"FrameCaptureSessionWindow";
        // A duplicate registration from a previous install/uninstall cycle in one
        // process is fine; the class is per-module and the second call reports it.
        static_cast<void>(::RegisterClassExW(&cls));

        // `HWND_MESSAGE` -- a message-only window. It receives `WM_QUERYENDSESSION`
        // without ever appearing on screen, in the taskbar or in Alt+Tab, which a
        // hidden top-level window would eventually do on some shell update.
        window = ::CreateWindowExW(0, cls.lpszClassName, L"FrameCapture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   cls.hInstance, nullptr);
        if (ready != nullptr) {
            ::SetEvent(ready);
        }
        if (window == nullptr) {
            FC_LOG_WARN(Subsystem::App, "session window could not be created; OS shutdown will not be observed",
                        LogFields{}.add("gle", static_cast<std::int64_t>(::GetLastError())));
            clear_thread_name();
            return;
        }

        // The message loop must run on the thread that created the window -- session
        // messages are delivered to that thread's queue and nowhere else.
        MSG message{};
        while (pump_running.load(std::memory_order_acquire) && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        ::DestroyWindow(window);
        window = nullptr;
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::App, "session window thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

ShutdownSignals::ShutdownSignals() : impl_(std::make_unique<Impl>()) {}

ShutdownSignals::~ShutdownSignals() {
    uninstall();
}

Result<void> ShutdownSignals::install(std::function<void()> on_shutdown) {
    impl_->core.callback = std::move(on_shutdown);
    g_shutdown_core.store(&impl_->core, std::memory_order_release);

    if (::SetConsoleCtrlHandler(console_handler, TRUE) == 0) {
        FC_LOG_WARN(Subsystem::App, "console control handler could not be installed",
                    LogFields{}.add("gle", static_cast<std::int64_t>(::GetLastError())));
    }

    impl_->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->pump_running.store(true, std::memory_order_release);
    impl_->pump = std::thread([impl = impl_.get()] { impl->pump_loop(); });

    // Wait for the window to exist before returning. Otherwise a shutdown arriving in
    // the first milliseconds of the engine's life finds no window -- the exact case
    // this exists for, since a crash-on-startup loop is when a user reboots.
    if (impl_->ready != nullptr) {
        static_cast<void>(::WaitForSingleObject(impl_->ready, 2000));
        ::CloseHandle(impl_->ready);
        impl_->ready = nullptr;
    }
    return ok();
}

void ShutdownSignals::uninstall() {
    if (!impl_->pump_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    static_cast<void>(::SetConsoleCtrlHandler(console_handler, FALSE));
    g_shutdown_core.store(nullptr, std::memory_order_release);

    if (impl_->window != nullptr) {
        // `WM_QUIT` into the pump thread's queue, not `DestroyWindow` from here:
        // destroying a window from a thread that does not own it is undefined, and
        // `PostMessage` is the supported way to ask its own thread to stop.
        static_cast<void>(::PostMessageW(impl_->window, WM_QUIT, 0, 0));
    }
    if (impl_->pump.joinable()) {
        impl_->pump.join();
    }
}

std::string ShutdownSignals::reason() const {
    const std::lock_guard lock(impl_->core.reason_mutex);
    return impl_->core.reason;
}

// ---------------------------------------------------------------------------
// GuiWatchdog
// ---------------------------------------------------------------------------

struct GuiWatchdog::Impl {
    std::function<void()> on_lost;
    std::atomic<std::int64_t> last_seen_ns{0};
    std::atomic<bool> running{false};
    std::atomic<bool> fired{false};

    /// Depth rather than a flag: nesting is not expected today, but a counter cannot be
    /// left un-suspended by an inner scope ending before an outer one (BUG-039).
    std::atomic<int> suspensions{0};

    std::mutex wake_mutex;
    std::condition_variable wake;
    bool exit = false;

    std::thread thread;

    void watch_loop();
};

void GuiWatchdog::Impl::watch_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-heartbeat");
    try {
        for (;;) {
            {
                std::unique_lock lock(wake_mutex);
                // A quarter of the interval, so the 5 s timeout is detected with at
                // most 250 ms of extra latency against SPEC.md §20 row 13's 6 s bound.
                if (wake.wait_for(lock, kHeartbeatInterval / 4, [this] { return exit; })) {
                    break;
                }
            }

            // A command is in flight, so the reader thread cannot be calling `notify`
            // whatever the host is doing (BUG-039). Nothing observed during this window
            // says anything about the host, so nothing is concluded from it.
            if (suspensions.load(std::memory_order_acquire) > 0) {
                continue;
            }

            const std::int64_t last = last_seen_ns.load(std::memory_order_acquire);
            if (last == 0) {
                continue; // never armed; see `start`
            }

            const std::int64_t silence = timing::qpc_now_ns() - last;
            if (silence < kHeartbeatTimeout.count() * 1'000'000LL) {
                continue;
            }

            if (fired.exchange(true)) {
                break;
            }
            FC_LOG_ERROR(Subsystem::Ipc, "host heartbeat lost; finalizing the recording and exiting",
                         LogFields{}
                             .add("silence_ms", silence / 1'000'000)
                             .add("timeout_ms", static_cast<std::int64_t>(kHeartbeatTimeout.count()))
                             .add_error(FcError::IPC_HEARTBEAT_TIMEOUT));
            if (on_lost) {
                on_lost();
            }
            break;
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Ipc, "heartbeat thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

GuiWatchdog::GuiWatchdog() : impl_(std::make_unique<Impl>()) {}

GuiWatchdog::~GuiWatchdog() {
    stop();
}

Result<void> GuiWatchdog::start(std::function<void()> on_lost) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    impl_->on_lost = std::move(on_lost);
    impl_->fired.store(false, std::memory_order_release);
    impl_->last_seen_ns.store(0, std::memory_order_release);
    {
        const std::lock_guard lock(impl_->wake_mutex);
        impl_->exit = false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->watch_loop(); });
    return ok();
}

void GuiWatchdog::notify() noexcept {
    impl_->last_seen_ns.store(timing::qpc_now_ns(), std::memory_order_release);
}

GuiWatchdog::Suspension::Suspension(GuiWatchdog& watchdog) noexcept : watchdog_(&watchdog) {
    watchdog_->impl_->suspensions.fetch_add(1, std::memory_order_acq_rel);
}

GuiWatchdog::Suspension::~Suspension() {
    // Re-armed from *now*, not from the last `notify`. The engine was not listening for
    // the duration of the command, so it has no evidence either way and starts a fresh
    // timeout rather than immediately firing on silence it caused itself.
    watchdog_->impl_->last_seen_ns.store(timing::qpc_now_ns(), std::memory_order_release);
    watchdog_->impl_->suspensions.fetch_sub(1, std::memory_order_acq_rel);
}

bool GuiWatchdog::suspended() const noexcept {
    return impl_->suspensions.load(std::memory_order_acquire) > 0;
}

void GuiWatchdog::stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        const std::lock_guard lock(impl_->wake_mutex);
        impl_->exit = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

std::int64_t GuiWatchdog::silence_ns() const noexcept {
    const std::int64_t last = impl_->last_seen_ns.load(std::memory_order_acquire);
    return last == 0 ? 0 : timing::qpc_now_ns() - last;
}

bool GuiWatchdog::fired() const noexcept {
    return impl_->fired.load(std::memory_order_acquire);
}

} // namespace fc::ipc
