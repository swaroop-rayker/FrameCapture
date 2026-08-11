#include "core/ipc/pipe_server.h"

#include "core/error/hresult.h"
#include "core/ipc/framing.h"
#include "core/logging/log_fields.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"

#include <windows.h>

#include <objbase.h> // CoCreateGuid for §15.1's {session_guid}
#include <sddl.h>

#include <array>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fc::ipc {
namespace {

/// One message at a time, so the buffer is sized for §15.1's ceiling plus the prefix.
/// A message-mode read that does not fit returns `ERROR_MORE_DATA` and leaves the rest
/// queued; sizing the buffer to the protocol's own limit means that only happens for a
/// peer that is already violating it.
constexpr DWORD kPipeBufferBytes = static_cast<DWORD>(kMaxMessageBytes + kLengthPrefixBytes);

/// How long a blocking wait sleeps before re-checking the stop flag. The waits are
/// event-driven, so this bounds only the pathological case where an event is missed.
constexpr DWORD kWaitSliceMs = 250;

/// A `HANDLE` that closes itself. `INVALID_HANDLE_VALUE` and `nullptr` both mean "no
/// handle" in the Win32 API depending on which call produced it, and both are handled
/// here so no call site has to remember which it got.
class ScopedHandle {
public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        HANDLE released = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return released;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

/// A security descriptor granting the current user and nobody else (SPEC.md §15.1).
///
/// Built from SDDL rather than by hand-assembling an ACL: `D:P` makes the DACL
/// *protected*, which stops inheritable ACEs from the parent object adding access the
/// engine did not grant. Hand-built ACLs routinely omit that and end up more permissive
/// than intended, silently.
class UserOnlySecurity {
public:
    [[nodiscard]] Result<void> build() {
        ScopedHandle token;
        HANDLE raw = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) == 0) {
            return FcError::IPC_PIPE_CREATE_FAILED;
        }
        token.reset(raw);

        DWORD needed = 0;
        static_cast<void>(::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed));
        if (needed == 0) {
            return FcError::IPC_PIPE_CREATE_FAILED;
        }

        std::vector<std::uint8_t> buffer(needed);
        if (::GetTokenInformation(token.get(), TokenUser, buffer.data(), needed, &needed) == 0) {
            return FcError::IPC_PIPE_CREATE_FAILED;
        }

        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        LPWSTR sid_text = nullptr;
        if (::ConvertSidToStringSidW(user->User.Sid, &sid_text) == 0) {
            return FcError::IPC_PIPE_CREATE_FAILED;
        }
        const std::wstring sid{sid_text};
        ::LocalFree(sid_text);

        // Protected DACL, one ACE: generic-all to this user. No SYSTEM ACE and no
        // Administrators ACE -- an administrator can take ownership regardless, and
        // naming them here would only widen what a compromised service can reach.
        const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                   nullptr) == 0) {
            return FcError::IPC_PIPE_CREATE_FAILED;
        }
        descriptor_ = descriptor;

        attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
        return ok();
    }

    ~UserOnlySecurity() {
        if (descriptor_ != nullptr) {
            ::LocalFree(descriptor_);
        }
    }

    UserOnlySecurity() = default;
    UserOnlySecurity(const UserOnlySecurity&) = delete;
    UserOnlySecurity& operator=(const UserOnlySecurity&) = delete;
    UserOnlySecurity(UserOnlySecurity&&) = delete;
    UserOnlySecurity& operator=(UserOnlySecurity&&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept {
        return &attributes_;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

} // namespace

std::string pipe_path_for(std::string_view session_id) {
    return R"(\\.\pipe\framecapture-)" + std::string{session_id};
}

std::string new_session_id() {
    GUID guid{};
    if (::CoCreateGuid(&guid) != S_OK) {
        // A GUID that cannot be created is not worth failing a recording over, and a
        // process id plus a clock reading is unique enough for a name that only has to
        // be unique among the sessions alive right now.
        //
        // QPC rather than `GetTickCount64`, which the banned-pattern gate rejects
        // (SPEC.md §7.1). The gate is about *media* timing and this is a name, so it is
        // arguably out of scope -- but a grep-based gate cannot make that distinction,
        // and QPC costs nothing here.
        return std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(timing::qpc_now_ns());
    }

    std::array<char, 40> text{};
    const int written = std::snprintf(text.data(), text.size(), "%08lx%04hx%04hx%02x%02x%02x%02x%02x%02x%02x%02x",
                                      guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                                      guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return written > 0 ? std::string{text.data(), static_cast<std::size_t>(written)} : std::string{"fallback"};
}

struct PipeServer::Impl {
    std::string session_id;
    std::string path;
    RequestHandler handler;

    ScopedHandle pipe;
    ScopedHandle stop_event;
    ScopedHandle read_event;
    ScopedHandle write_event;

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> rejected{0};

    /// Serialises writes. The reader thread writes responses and any thread may write
    /// events, and two overlapped writes on one handle with one event would interleave
    /// their completions.
    std::mutex write_mutex;

    void serve_loop();
    /// Blocks until a client connects, the stop event fires, or the pipe fails.
    [[nodiscard]] bool await_client() const;
    /// Reads and dispatches until the client disconnects or stop is signalled.
    void pump_client();
    [[nodiscard]] bool write_frame(const std::string& body);
    void disconnect();
};

bool PipeServer::Impl::await_client() const {
    OVERLAPPED overlapped{};
    overlapped.hEvent = read_event.get();
    ::ResetEvent(read_event.get());

    if (::ConnectNamedPipe(pipe.get(), &overlapped) == 0) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            // The client connected between `CreateNamedPipe` and here. Not a failure
            // and not a race to fix: it is the documented outcome, and treating it as
            // an error would drop a perfectly good connection.
            return true;
        }
        if (error != ERROR_IO_PENDING) {
            FC_LOG_ERROR(Subsystem::Ipc, "ConnectNamedPipe failed",
                         LogFields{}.add("gle", static_cast<std::int64_t>(error)));
            return false;
        }
    }

    const std::array<HANDLE, 2> waits{stop_event.get(), read_event.get()};
    for (;;) {
        const DWORD signalled =
            ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, kWaitSliceMs);
        if (signalled == WAIT_OBJECT_0) {
            ::CancelIoEx(pipe.get(), &overlapped);
            return false;
        }
        if (signalled == WAIT_OBJECT_0 + 1) {
            DWORD transferred = 0;
            return ::GetOverlappedResult(pipe.get(), &overlapped, &transferred, FALSE) != 0;
        }
        if (signalled != WAIT_TIMEOUT) {
            return false;
        }
    }
}

bool PipeServer::Impl::write_frame(const std::string& body) {
    const Result<std::vector<std::uint8_t>> framed = encode_frame(body);
    if (!framed.has_value()) {
        FC_LOG_ERROR(Subsystem::Ipc, "outgoing message exceeded the protocol limit",
                     LogFields{}.add("bytes", static_cast<std::int64_t>(body.size())).add_error(framed.error()));
        return false;
    }

    const std::lock_guard lock(write_mutex);
    if (!connected.load(std::memory_order_acquire)) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = write_event.get();
    ::ResetEvent(write_event.get());

    DWORD written = 0;
    if (::WriteFile(pipe.get(), framed.value().data(), static_cast<DWORD>(framed.value().size()), &written,
                    &overlapped) == 0) {
        if (::GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (::GetOverlappedResult(pipe.get(), &overlapped, &written, TRUE) == 0) {
            return false;
        }
    }
    return written == framed.value().size();
}

void PipeServer::Impl::pump_client() {
    FrameReader reader;
    std::vector<std::uint8_t> chunk(kPipeBufferBytes);

    while (running.load(std::memory_order_acquire)) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = read_event.get();
        ::ResetEvent(read_event.get());

        DWORD read = 0;
        if (::ReadFile(pipe.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &read, &overlapped) == 0) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                return;
            }
            if (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA) {
                FC_LOG_WARN(Subsystem::Ipc, "control pipe read failed",
                            LogFields{}.add("gle", static_cast<std::int64_t>(error)));
                return;
            }
            if (error == ERROR_IO_PENDING) {
                const std::array<HANDLE, 2> waits{stop_event.get(), read_event.get()};
                const DWORD signalled =
                    ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
                if (signalled == WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe.get(), &overlapped);
                    return;
                }
                if (::GetOverlappedResult(pipe.get(), &overlapped, &read, FALSE) == 0) {
                    return;
                }
            }
        }

        if (read == 0) {
            continue;
        }

        reader.append(std::span<const std::uint8_t>{chunk.data(), read});

        for (;;) {
            const Result<std::optional<std::string>> body = reader.next();
            if (!body.has_value()) {
                // A framing error is not recoverable on a stream: the reader cannot
                // know where the next message starts. Drop the connection rather than
                // keep decoding rubbish, and say why.
                rejected.fetch_add(1, std::memory_order_relaxed);
                FC_LOG_WARN(Subsystem::Ipc, "control frame rejected; dropping the connection",
                            LogFields{}.add_error(body.error()));
                return;
            }
            if (!body.value().has_value()) {
                break; // need more bytes
            }

            const Result<Request> request = parse_request(*body.value());
            if (!request.has_value()) {
                // A malformed *message*, unlike a malformed frame, is recoverable: the
                // stream is still aligned. Answer with the error and keep serving.
                rejected.fetch_add(1, std::memory_order_relaxed);
                static_cast<void>(write_frame(make_error_response({}, request.error())));
                continue;
            }

            requests.fetch_add(1, std::memory_order_relaxed);
            const std::string response = handler
                                             ? handler(request.value())
                                             : make_error_response(request.value().id, FcError::INTERNAL_INVALID_STATE);
            if (!write_frame(response)) {
                return;
            }
        }
    }
}

void PipeServer::Impl::disconnect() {
    connected.store(false, std::memory_order_release);
    if (pipe.valid()) {
        static_cast<void>(::FlushFileBuffers(pipe.get()));
        static_cast<void>(::DisconnectNamedPipe(pipe.get()));
    }
}

void PipeServer::Impl::serve_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-ipc");
    try {
        while (running.load(std::memory_order_acquire)) {
            if (!await_client()) {
                break;
            }
            connected.store(true, std::memory_order_release);
            FC_LOG_INFO(Subsystem::Ipc, "control client connected", LogFields{}.add("pipe", path));

            pump_client();

            disconnect();
            FC_LOG_INFO(Subsystem::Ipc, "control client disconnected",
                        LogFields{}.add("requests", static_cast<std::int64_t>(requests.load())));
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Ipc, "ipc thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

PipeServer::PipeServer() : impl_(std::make_unique<Impl>()) {}

PipeServer::~PipeServer() {
    stop();
}

Result<void> PipeServer::start(const PipeServerSettings& settings, RequestHandler handler) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->session_id = settings.session_id.empty() ? new_session_id() : settings.session_id;
    impl_->path = pipe_path_for(impl_->session_id);
    impl_->handler = std::move(handler);

    UserOnlySecurity security;
    FC_TRY(security.build());

    const std::wstring wide_path(impl_->path.begin(), impl_->path.end());
    impl_->pipe.reset(::CreateNamedPipeW(wide_path.c_str(),
                                         PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                         1, // SPEC.md §3.1: one GUI per engine
                                         kPipeBufferBytes, kPipeBufferBytes, 0, security.attributes()));
    if (!impl_->pipe.valid()) {
        const DWORD error = ::GetLastError();
        FC_LOG_ERROR(Subsystem::Ipc, "CreateNamedPipe failed",
                     LogFields{}
                         .add("pipe", impl_->path)
                         .add("gle", static_cast<std::int64_t>(error))
                         .add_error(FcError::IPC_PIPE_CREATE_FAILED));
        return FcError::IPC_PIPE_CREATE_FAILED;
    }

    // Manual-reset events: an auto-reset event consumed by `WaitForMultipleObjects`
    // would let one of two waiters miss a stop signal, and the stop event has to be
    // observable by every waiter that is up.
    impl_->stop_event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    impl_->read_event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    impl_->write_event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl_->stop_event.valid() || !impl_->read_event.valid() || !impl_->write_event.valid()) {
        impl_->pipe.reset();
        return FcError::IPC_PIPE_CREATE_FAILED;
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->serve_loop(); });

    FC_LOG_INFO(Subsystem::Ipc, "control channel open",
                LogFields{}.add("pipe", impl_->path).add("proto", std::string{kProtocolVersion}));
    return ok();
}

void PipeServer::stop() {
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (impl_->stop_event.valid()) {
        ::SetEvent(impl_->stop_event.get());
    }
    if (impl_->pipe.valid()) {
        // Cancels whatever the serve thread is blocked on. Without it a thread parked
        // in `ConnectNamedPipe` waits for a client that is never coming, and `stop`
        // becomes an unbounded join -- BUG-008's defect, in a new component.
        static_cast<void>(::CancelIoEx(impl_->pipe.get(), nullptr));
    }

    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }

    impl_->disconnect();
    impl_->pipe.reset();
    impl_->stop_event.reset();
    impl_->read_event.reset();
    impl_->write_event.reset();
}

void PipeServer::send_event(Event event, const nlohmann::json& body) {
    if (!impl_->connected.load(std::memory_order_acquire)) {
        return;
    }
    static_cast<void>(impl_->write_frame(make_event(event, body)));
}

bool PipeServer::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

bool PipeServer::client_connected() const noexcept {
    return impl_->connected.load(std::memory_order_acquire);
}

std::string PipeServer::pipe_path() const {
    return impl_->path;
}

std::string PipeServer::session_id() const {
    return impl_->session_id;
}

std::uint64_t PipeServer::requests_handled() const noexcept {
    return impl_->requests.load(std::memory_order_relaxed);
}

std::uint64_t PipeServer::frames_rejected() const noexcept {
    return impl_->rejected.load(std::memory_order_relaxed);
}

} // namespace fc::ipc
