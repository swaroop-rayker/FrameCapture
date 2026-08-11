#include "core/logging/crash_handler.h"

#include "core/build_info.h"
#include "core/logging/logger.h"
#include "core/logging/ring_sink.h"

#include <windows.h>
// dbghelp.h must follow windows.h.
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <system_error>

namespace fc::crash {
namespace {

/// Tied to the public buffer size so the two cannot drift apart.
constexpr std::size_t kMaxPath = ArtifactBuffers::kCapacity;

/// Everything the crash path needs, resolved at install() time.
///
/// The crash path must not construct a std::filesystem::path, format a
/// std::string, or take the logger's lock: any of those can fail or deadlock in
/// exactly the situation this code exists to survive. So the directory is
/// pre-widened into a fixed buffer here, and only a short filename is appended
/// later, into another fixed buffer.
struct Installed {
    std::atomic<bool> installed{false};
    std::atomic<bool> handling{false};
    std::atomic<EnginePhase> phase{EnginePhase::Uninitialized};
    bool write_minidump = true;

    std::array<wchar_t, kMaxPath> directory{};
    std::array<char, 32> session_id{};

    LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
    _purecall_handler previous_purecall = nullptr;
    _invalid_parameter_handler previous_invalid_parameter = nullptr;
    std::terminate_handler previous_terminate = nullptr;
};

Installed& installed() {
    static Installed instance;
    return instance;
}

/// Fills `out` with `<directory>\<stem>_<session>_<timestamp><extension>`.
/// No allocation.
bool build_artifact_path(std::array<wchar_t, kMaxPath>& out, const wchar_t* stem, const wchar_t* extension,
                         const SYSTEMTIME& now) noexcept {
    const Installed& state = installed();
    const int written = _snwprintf_s(out.data(), out.size(), _TRUNCATE, L"%s\\%s_%hs_%04u%02u%02u_%02u%02u%02u%s",
                                     state.directory.data(), stem, state.session_id.data(), now.wYear, now.wMonth,
                                     now.wDay, now.wHour, now.wMinute, now.wSecond, extension);
    return written > 0;
}

bool write_buffer_to_file(const wchar_t* path, const char* data, std::size_t bytes) noexcept {
    const HANDLE handle =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(handle, data, static_cast<DWORD>(bytes), &written, nullptr) != 0 && written == bytes;
    FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok;
}

bool write_minidump_file(const wchar_t* path, EXCEPTION_POINTERS* pointers) noexcept {
    const HANDLE handle =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = pointers;
    info.ClientPointers = FALSE;

    // Data segments and thread info make globals and thread names readable in the
    // dump; handle data catches file-handle-loss (SPEC.md §1). Deliberately not
    // MiniDumpWithFullMemory -- a 1080p pipeline's dump would be gigabytes.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): MINIDUMP_TYPE is
    // a flags enum; the OR of several flags is intentionally not a named enumerator.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                 MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle, type,
                                      pointers != nullptr ? &info : nullptr, nullptr, nullptr);
    FlushFileBuffers(handle);
    CloseHandle(handle);
    return ok != FALSE;
}

/// Hand-formatted JSON. nlohmann::json would allocate repeatedly, and the heap is
/// a plausible casualty of whatever brought us here.
bool write_report(const wchar_t* path, Kind kind, EXCEPTION_POINTERS* pointers, const SYSTEMTIME& now,
                  const wchar_t* minidump_path, const wchar_t* ring_path) noexcept {
    const Installed& state = installed();

    unsigned long exception_code = 0;
    unsigned long long exception_address = 0;
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        exception_code = pointers->ExceptionRecord->ExceptionCode;
        exception_address = reinterpret_cast<unsigned long long>(pointers->ExceptionRecord->ExceptionAddress);
    }

    // Narrow the two paths for the JSON body without allocating.
    std::array<char, kMaxPath> minidump_narrow{};
    std::array<char, kMaxPath> ring_narrow{};
    WideCharToMultiByte(CP_UTF8, 0, minidump_path, -1, minidump_narrow.data(), static_cast<int>(minidump_narrow.size()),
                        nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, ring_path, -1, ring_narrow.data(), static_cast<int>(ring_narrow.size()), nullptr,
                        nullptr);

    // JSON requires escaped backslashes in string values.
    const auto escape = [](std::array<char, kMaxPath>& buffer) noexcept {
        std::array<char, kMaxPath * 2> escaped{};
        std::size_t out = 0;
        for (std::size_t i = 0; buffer[i] != '\0' && out + 2 < escaped.size(); ++i) {
            if (buffer[i] == '\\') {
                escaped[out++] = '\\';
            }
            escaped[out++] = buffer[i];
        }
        std::memcpy(buffer.data(), escaped.data(), buffer.size() - 1);
        buffer[buffer.size() - 1] = '\0';
    };
    escape(minidump_narrow);
    escape(ring_narrow);

    const std::string_view version = project_version();

    std::array<char, 4096> body{};
    const int written = std::snprintf(
        body.data(), body.size(),
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"session_id\": \"%s\",\n"
        "  \"timestamp_utc\": \"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\",\n"
        "  \"app_version\": \"%.*s\",\n"
        "  \"kind\": \"%.*s\",\n"
        "  \"engine_phase\": \"%.*s\",\n"
        "  \"exception_code\": \"0x%08lX\",\n"
        "  \"exception_address\": \"0x%016llX\",\n"
        "  \"thread_id\": %lu,\n"
        "  \"process_id\": %lu,\n"
        "  \"minidump\": \"%s\",\n"
        "  \"ring_log\": \"%s\"\n"
        "}\n",
        state.session_id.data(), now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, static_cast<int>(version.size()), version.data(), static_cast<int>(to_string(kind).size()),
        to_string(kind).data(), static_cast<int>(to_string(state.phase.load(std::memory_order_relaxed)).size()),
        to_string(state.phase.load(std::memory_order_relaxed)).data(), exception_code, exception_address,
        GetCurrentThreadId(), GetCurrentProcessId(), minidump_narrow.data(), ring_narrow.data());

    if (written <= 0) {
        return false;
    }
    return write_buffer_to_file(path, body.data(), static_cast<std::size_t>(written));
}

/// Common tail for the three handlers that must not return.
[[noreturn]] void finish_and_die(Kind kind, EXCEPTION_POINTERS* pointers) noexcept {
    ArtifactBuffers buffers;
    write_artifacts_into(kind, pointers, buffers);
    // TerminateProcess rather than exit()/abort(): no further CRT teardown runs,
    // so no destructor can re-enter a handler on an already-broken heap.
    // (CLAUDE.md §4 bans exit() outside main; this is the sanctioned alternative.)
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
    for (;;) {
        // Unreachable; satisfies [[noreturn]] without UB if Terminate were to fail.
    }
}

LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* pointers) {
    ArtifactBuffers buffers;
    write_artifacts_into(Kind::UnhandledException, pointers, buffers);
    // Handle it ourselves: we have already written the artifacts, and letting WER
    // also run would produce a second, redundant dump.
    return EXCEPTION_EXECUTE_HANDLER;
}

void on_pure_call() {
    finish_and_die(Kind::PureVirtualCall, nullptr);
}

void on_terminate() {
    finish_and_die(Kind::Terminate, nullptr);
}

void on_invalid_parameter(const wchar_t* /*expression*/, const wchar_t* /*function*/, const wchar_t* /*file*/,
                          unsigned /*line*/, uintptr_t /*reserved*/) {
    // The CRT only supplies these arguments in a debug build; in release they are
    // all null, so there is nothing worth recording from them.
    finish_and_die(Kind::InvalidParameter, nullptr);
}

std::filesystem::path default_crash_directory() {
    // Sibling of the log directory, so the diagnostic bundle in SPEC.md §18 is one
    // parent away from both.
    return log::default_log_directory().parent_path() / "crashes";
}

} // namespace

std::string_view to_string(Kind kind) noexcept {
    switch (kind) {
    case Kind::UnhandledException:
        return "unhandled_exception";
    case Kind::PureVirtualCall:
        return "pure_virtual_call";
    case Kind::Terminate:
        return "terminate";
    case Kind::InvalidParameter:
        return "invalid_parameter";
    }
    return "unknown";
}

std::string_view to_string(EnginePhase phase) noexcept {
    switch (phase) {
    case EnginePhase::Uninitialized:
        return "uninitialized";
    case EnginePhase::Starting:
        return "starting";
    case EnginePhase::Configuring:
        return "configuring";
    case EnginePhase::Idle:
        return "idle";
    case EnginePhase::Recording:
        return "recording";
    case EnginePhase::Stopping:
        return "stopping";
    case EnginePhase::Finalizing:
        return "finalizing";
    case EnginePhase::Stopped:
        return "stopped";
    case EnginePhase::Faulted:
        return "faulted";
    }
    return "unknown";
}

Result<void> install(const Config& config) {
    Installed& state = installed();
    if (state.installed.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    const std::filesystem::path directory = config.directory.empty() ? default_crash_directory() : config.directory;

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return FcError::IO_DIRECTORY_CREATE_FAILED;
    }

    const std::wstring wide = directory.wstring();
    if (wide.size() + 128 >= kMaxPath) {
        return FcError::IO_PATH_INVALID;
    }
    std::wmemcpy(state.directory.data(), wide.c_str(), wide.size() + 1);

    const std::string_view session = log::session_id();
    const std::size_t copied = std::min(session.size(), state.session_id.size() - 1);
    std::memcpy(state.session_id.data(), session.data(), copied);
    state.session_id[copied] = '\0';
    if (copied == 0) {
        // A crash report that cannot be correlated with a log file is much less
        // useful, so make the gap obvious rather than emitting an empty field.
        std::snprintf(state.session_id.data(), state.session_id.size(), "no-session");
    }

    state.write_minidump = config.write_minidump;

    // A fresh install starts from a known phase. Inheriting whatever was left over
    // would put a stale "last known state machine position" in the crash report,
    // which is worse than reporting nothing.
    state.phase.store(EnginePhase::Uninitialized, std::memory_order_relaxed);

    state.previous_filter = SetUnhandledExceptionFilter(&on_unhandled_exception);
    state.previous_purecall = _set_purecall_handler(&on_pure_call);
    state.previous_invalid_parameter = _set_invalid_parameter_handler(&on_invalid_parameter);
    state.previous_terminate = std::set_terminate(&on_terminate);
    state.installed.store(true, std::memory_order_release);

    FC_LOG_INFO(Subsystem::App, "crash handler installed",
                LogFields{}
                    .add("directory", directory.string())
                    .add("minidump", config.write_minidump)
                    .add("handlers", "unhandled_exception,purecall,terminate,invalid_parameter"));
    return ok();
}

void uninstall() {
    Installed& state = installed();
    if (!state.installed.load(std::memory_order_acquire)) {
        return;
    }

    SetUnhandledExceptionFilter(state.previous_filter);
    _set_purecall_handler(state.previous_purecall);
    _set_invalid_parameter_handler(state.previous_invalid_parameter);
    std::set_terminate(state.previous_terminate);

    state.installed.store(false, std::memory_order_release);
    state.handling.store(false, std::memory_order_relaxed);
}

bool is_installed() noexcept {
    return installed().installed.load(std::memory_order_acquire);
}

void set_engine_phase(EnginePhase phase) noexcept {
    installed().phase.store(phase, std::memory_order_relaxed);
}

EnginePhase engine_phase() noexcept {
    return installed().phase.load(std::memory_order_relaxed);
}

void write_artifacts_into(Kind kind, void* exception_pointers, ArtifactBuffers& out) noexcept {
    Installed& state = installed();

    // Reentrancy guard: a fault inside this function must not loop forever.
    bool expected = false;
    if (!state.handling.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    SYSTEMTIME now{};
    GetSystemTime(&now);

    build_artifact_path(out.minidump, L"crash", L".dmp", now);
    build_artifact_path(out.ring_log, L"crash", L".log", now);
    build_artifact_path(out.report_json, L"crash_report", L".json", now);

    // Ring buffer first: it is the cheapest artifact and the one most likely to
    // explain the crash, so it should exist even if the minidump attempt dies.
    if (const RingSink* ring = log::ring_sink(); ring != nullptr) {
        out.ring_log_written = ring->write_to_file_best_effort(out.ring_log.data());
    }

    if (state.write_minidump) {
        out.minidump_written =
            write_minidump_file(out.minidump.data(), static_cast<EXCEPTION_POINTERS*>(exception_pointers));
    }

    out.report_written =
        write_report(out.report_json.data(), kind, static_cast<EXCEPTION_POINTERS*>(exception_pointers), now,
                     out.minidump.data(), out.ring_log.data());

    state.handling.store(false, std::memory_order_release);
}

ArtifactPaths write_artifacts(Kind kind, void* exception_pointers) {
    ArtifactBuffers buffers;
    write_artifacts_into(kind, exception_pointers, buffers);

    // These constructions allocate, which is exactly why they are here and not in
    // the noexcept core above.
    ArtifactPaths result;
    result.minidump = std::filesystem::path{buffers.minidump.data()};
    result.ring_log = std::filesystem::path{buffers.ring_log.data()};
    result.report_json = std::filesystem::path{buffers.report_json.data()};
    result.minidump_written = buffers.minidump_written;
    result.ring_log_written = buffers.ring_log_written;
    result.report_written = buffers.report_written;
    return result;
}

} // namespace fc::crash
