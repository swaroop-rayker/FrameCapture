#include "core/ipc/pipe_client.h"

#include "core/ipc/framing.h"
#include "core/ipc/pipe_server.h"
#include "core/logging/log_fields.h"
#include "core/logging/logger.h"
#include "core/util/thread_utils.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace fc::ipc {
namespace {

constexpr DWORD kReadBufferBytes = static_cast<DWORD>(kMaxMessageBytes + kLengthPrefixBytes);

/// Parses the engine's `"proto"` string far enough to compare majors.
///
/// Deliberately lenient about everything after the first dot: SPEC.md §15.1 makes the
/// minor version additive, so a client that failed to parse `1.0-rc2` and refused the
/// connection would be enforcing a rule the spec does not have.
[[nodiscard]] int major_of(std::string_view proto) noexcept {
    int major = 0;
    for (const char c : proto) {
        if (c == '.') {
            break;
        }
        if (c < '0' || c > '9') {
            return -1;
        }
        major = (major * 10) + (c - '0');
    }
    return major;
}

} // namespace

struct PipeClient::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE stop_event = nullptr;
    HANDLE read_event = nullptr;
    HANDLE write_event = nullptr;

    std::thread reader;
    std::atomic<bool> connected{false};
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> next_id{1};

    EventHandler on_event;

    std::mutex write_mutex;

    /// Outstanding requests, keyed by the id echoed in the response.
    std::mutex pending_mutex;
    std::condition_variable pending_cv;
    std::map<std::string, nlohmann::json> answers;

    void read_loop();
    [[nodiscard]] bool write_frame(const std::string& body);
    void close_handles();
};

void PipeClient::Impl::close_handles() {
    if (pipe != INVALID_HANDLE_VALUE) {
        ::CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
    for (HANDLE* handle : {&stop_event, &read_event, &write_event}) {
        if (*handle != nullptr) {
            ::CloseHandle(*handle);
            *handle = nullptr;
        }
    }
}

bool PipeClient::Impl::write_frame(const std::string& body) {
    const Result<std::vector<std::uint8_t>> framed = encode_frame(body);
    if (!framed.has_value()) {
        return false;
    }

    const std::lock_guard lock(write_mutex);
    if (!connected.load(std::memory_order_acquire)) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = write_event;
    ::ResetEvent(write_event);

    DWORD written = 0;
    if (::WriteFile(pipe, framed.value().data(), static_cast<DWORD>(framed.value().size()), &written, &overlapped) ==
        0) {
        if (::GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (::GetOverlappedResult(pipe, &overlapped, &written, TRUE) == 0) {
            return false;
        }
    }
    return written == framed.value().size();
}

void PipeClient::Impl::read_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-ipc-client");
    try {
        FrameReader reader_state;
        std::vector<std::uint8_t> chunk(kReadBufferBytes);

        while (connected.load(std::memory_order_acquire)) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = read_event;
            ::ResetEvent(read_event);

            DWORD read = 0;
            if (::ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, &overlapped) == 0) {
                const DWORD error = ::GetLastError();
                if (error != ERROR_IO_PENDING) {
                    break;
                }
                const std::array<HANDLE, 2> waits{stop_event, read_event};
                const DWORD signalled =
                    ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
                if (signalled == WAIT_OBJECT_0) {
                    ::CancelIoEx(pipe, &overlapped);
                    break;
                }
                if (::GetOverlappedResult(pipe, &overlapped, &read, FALSE) == 0) {
                    break;
                }
            }
            if (read == 0) {
                continue;
            }

            reader_state.append(std::span<const std::uint8_t>{chunk.data(), read});
            for (;;) {
                const Result<std::optional<std::string>> body = reader_state.next();
                if (!body.has_value() || !body.value().has_value()) {
                    break;
                }

                const nlohmann::json message = nlohmann::json::parse(*body.value(), nullptr, false);
                if (message.is_discarded() || !message.is_object()) {
                    continue;
                }

                if (const auto event = message.find("event"); event != message.end() && event->is_string()) {
                    events.fetch_add(1, std::memory_order_relaxed);
                    if (on_event) {
                        // Events are dispatched by name rather than re-parsed into the
                        // enum: an engine one minor version ahead may emit an event this
                        // build has no enumerator for, and §15.1's compatibility rule
                        // says that must not be fatal.
                        for (const Event known :
                             {Event::StateChanged, Event::Stats, Event::Warning, Event::Error, Event::GpuMigrated,
                              Event::AudioDeviceMigrated, Event::DegradationChanged, Event::SegmentRolled,
                              Event::RecordingFinalized}) {
                            if (to_string(known) == event->get<std::string>()) {
                                on_event(known, message);
                                break;
                            }
                        }
                    }
                    continue;
                }

                if (const auto id = message.find("id"); id != message.end() && id->is_string()) {
                    {
                        const std::lock_guard lock(pending_mutex);
                        answers[id->get<std::string>()] = message;
                    }
                    pending_cv.notify_all();
                }
            }
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Ipc, "ipc client thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }

    connected.store(false, std::memory_order_release);
    // Wake anything waiting on a response that is now never coming, rather than letting
    // it sit out the full 5 s for an answer the broken pipe already ruled out.
    pending_cv.notify_all();
    clear_thread_name();
}

PipeClient::PipeClient() : impl_(std::make_unique<Impl>()) {}

PipeClient::~PipeClient() {
    disconnect();
}

Result<void> PipeClient::connect(const PipeClientSettings& settings, EventHandler on_event) {
    if (impl_->connected.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    const std::string path = pipe_path_for(settings.session_id);
    const std::wstring wide_path(path.begin(), path.end());

    // Retry until the deadline. The engine may still be creating the pipe -- see the
    // note on `connect_timeout` -- and `WaitNamedPipe` alone does not cover the window
    // before the pipe exists at all, which is precisely the window a freshly spawned
    // engine is in.
    const auto deadline = std::chrono::steady_clock::now() + settings.connect_timeout;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = ::CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            FC_LOG_ERROR(Subsystem::Ipc, "could not connect to the control pipe",
                         LogFields{}
                             .add("pipe", path)
                             .add("gle", static_cast<std::int64_t>(::GetLastError()))
                             .add_error(FcError::IPC_PIPE_CONNECT_FAILED));
            return FcError::IPC_PIPE_CONNECT_FAILED;
        }
        ::Sleep(20);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) == 0) {
        ::CloseHandle(pipe);
        return FcError::IPC_PIPE_CONNECT_FAILED;
    }

    impl_->pipe = pipe;
    impl_->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->read_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->write_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->stop_event == nullptr || impl_->read_event == nullptr || impl_->write_event == nullptr) {
        impl_->close_handles();
        return FcError::IPC_PIPE_CONNECT_FAILED;
    }

    impl_->on_event = std::move(on_event);
    impl_->connected.store(true, std::memory_order_release);
    impl_->reader = std::thread([impl = impl_.get()] { impl->read_loop(); });
    return ok();
}

Result<nlohmann::json> PipeClient::request(Command command, const nlohmann::json& params) {
    if (!impl_->connected.load(std::memory_order_acquire)) {
        return FcError::IPC_PIPE_BROKEN;
    }

    const std::string id = std::to_string(impl_->next_id.fetch_add(1, std::memory_order_relaxed));

    nlohmann::json message = params.is_object() ? params : nlohmann::json::object();
    message["cmd"] = std::string{to_string(command)};
    message["id"] = id;

    if (!impl_->write_frame(message.dump())) {
        return FcError::IPC_PIPE_BROKEN;
    }

    const auto timeout = std::chrono::milliseconds{request_timeout_ms(command)};
    std::unique_lock lock(impl_->pending_mutex);
    const bool answered = impl_->pending_cv.wait_for(lock, timeout, [&] {
        return impl_->answers.contains(id) || !impl_->connected.load(std::memory_order_acquire);
    });

    const auto found = impl_->answers.find(id);
    if (found == impl_->answers.end()) {
        // The entry is erased on every path below, so a timed-out request cannot leave
        // a row behind -- a map that only ever grows is the unbounded queue of
        // CLAUDE.md hard rule 5 wearing a different hat.
        return answered ? FcError::IPC_PIPE_BROKEN : FcError::INTERNAL_TIMEOUT;
    }

    const nlohmann::json answer = found->second;
    impl_->answers.erase(found);
    lock.unlock();

    if (const auto ok_field = answer.find("ok");
        ok_field != answer.end() && ok_field->is_boolean() && !ok_field->get<bool>()) {
        const auto code = answer.find("code");
        return code != answer.end() && code->is_number_integer() ? error_from_code(code->get<int>())
                                                                 : FcError::INTERNAL_UNKNOWN;
    }
    return answer;
}

Result<nlohmann::json> PipeClient::handshake(std::string_view client_name) {
    nlohmann::json params = nlohmann::json::object();
    params["proto"] = std::string{kProtocolVersion};
    params["client"] = std::string{client_name};

    FC_TRY_ASSIGN(const nlohmann::json reply, request(Command::Hello, params));

    const auto proto = reply.find("proto");
    if (proto == reply.end() || !proto->is_string()) {
        return FcError::IPC_MESSAGE_MALFORMED;
    }
    const int engine_major = major_of(proto->get<std::string>());
    if (engine_major != kProtocolMajor) {
        FC_LOG_ERROR(Subsystem::Ipc, "engine speaks an incompatible protocol major",
                     LogFields{}
                         .add("engine_proto", proto->get<std::string>())
                         .add("client_proto", std::string{kProtocolVersion})
                         .add_error(FcError::IPC_PROTOCOL_VERSION_MISMATCH));
        return FcError::IPC_PROTOCOL_VERSION_MISMATCH;
    }
    return reply;
}

void PipeClient::disconnect() {
    if (!impl_->connected.exchange(false, std::memory_order_acq_rel)) {
        impl_->close_handles();
        return;
    }
    if (impl_->stop_event != nullptr) {
        ::SetEvent(impl_->stop_event);
    }
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        static_cast<void>(::CancelIoEx(impl_->pipe, nullptr));
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    impl_->close_handles();
}

bool PipeClient::connected() const noexcept {
    return impl_->connected.load(std::memory_order_acquire);
}

std::uint64_t PipeClient::events_received() const noexcept {
    return impl_->events.load(std::memory_order_relaxed);
}

} // namespace fc::ipc
