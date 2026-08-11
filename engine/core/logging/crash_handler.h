#pragma once

#include "core/error/result.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace fc::crash {

/// Which of the four handlers fired. SPEC.md §18 requires all four, because "the
/// default CRT handlers swallow failures silently" -- a pure-virtual call or a bad
/// CRT argument would otherwise kill the process with no artifact at all.
enum class Kind {
    UnhandledException, ///< SetUnhandledExceptionFilter
    PureVirtualCall,    ///< _set_purecall_handler
    Terminate,          ///< std::set_terminate
    InvalidParameter,   ///< _set_invalid_parameter_handler
};

[[nodiscard]] std::string_view to_string(Kind kind) noexcept;

/// The crash report's "last known state machine position" (SPEC.md §18).
///
/// This is the *crash-report projection* of the pipeline state machine, not the
/// state machine itself -- that lands with M6 in `pipeline/state_machine`. It
/// lives here because the crash handler must be able to read it without taking a
/// lock or allocating, so it is a single atomic that the pipeline pushes into.
/// Until M6 wires it up it stays `Uninitialized`.
enum class EnginePhase {
    Uninitialized,
    Starting,
    Configuring,
    Idle,
    Recording,
    Stopping,
    Finalizing,
    Stopped,
    Faulted,
};

[[nodiscard]] std::string_view to_string(EnginePhase phase) noexcept;

struct Config {
    /// Defaults to `%LOCALAPPDATA%\FrameCapture\crashes`. Overridable so tests do
    /// not scatter minidumps through the user profile.
    std::filesystem::path directory;

    /// Writing a minidump is the expensive part; tests that only care about the
    /// ring dump and the JSON report turn it off.
    bool write_minidump = true;
};

/// Installs all four handlers. Paths are resolved and pre-widened here so that
/// the crash path itself performs no path construction.
[[nodiscard]] Result<void> install(const Config& config = {});

/// Restores the previous handlers.
void uninstall();

[[nodiscard]] bool is_installed() noexcept;

/// Records the current phase. Lock-free and allocation-free; safe from any thread.
void set_engine_phase(EnginePhase phase) noexcept;
[[nodiscard]] EnginePhase engine_phase() noexcept;

/// Fixed-capacity output for the crash path.
///
/// Deliberately not `std::filesystem::path`: constructing one allocates, and the
/// crash path must not touch the heap, which is a plausible casualty of whatever
/// caused the crash. Making the buffers fixed-size is what lets
/// `write_artifacts_into` be genuinely `noexcept` rather than nominally so.
struct ArtifactBuffers {
    static constexpr std::size_t kCapacity = 1024;

    std::array<wchar_t, kCapacity> minidump{};
    std::array<wchar_t, kCapacity> ring_log{};
    std::array<wchar_t, kCapacity> report_json{};
    bool minidump_written = false;
    bool ring_log_written = false;
    bool report_written = false;
};

/// The crash-path entry point: writes all three artifacts into `out`.
///
/// Allocation-free and truly `noexcept` -- this is what the four handlers call.
void write_artifacts_into(Kind kind, void* exception_pointers, ArtifactBuffers& out) noexcept;

struct ArtifactPaths {
    std::filesystem::path minidump;
    std::filesystem::path ring_log;
    std::filesystem::path report_json;
    bool minidump_written = false;
    bool ring_log_written = false;
    bool report_written = false;
};

/// Reporting wrapper over `write_artifacts_into`, for tests and tooling.
///
/// **Allocates** (it builds `std::filesystem::path` objects), so it is not
/// `noexcept` and must never be called from a crash context. It exists because it
/// is the seam that makes crash handling testable: `test_crash_artifacts` calls it
/// with `exception_pointers == nullptr` and asserts the ring buffer reached disk,
/// which is the part of SPEC.md §18 verifiable without faulting the test runner.
///
/// The one thing it cannot cover is the OS deciding to invoke our filter; an
/// end-to-end faulting-subprocess test belongs with `test_crash_recovery`
/// (SPEC.md §20 row 3) at M5.
[[nodiscard]] ArtifactPaths write_artifacts(Kind kind, void* exception_pointers);

} // namespace fc::crash
