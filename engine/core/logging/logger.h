#pragma once

#include "core/error/result.h"
#include "core/logging/log_fields.h"
#include "core/subsystem.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc {

class RingSink;

namespace log {

/// SPEC.md §18 levels. TRACE is per-frame data and is off by default.
enum class Level {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

[[nodiscard]] std::string_view to_string(Level level) noexcept;

/// Parses a level name, case-insensitively. `nullopt` for anything unrecognised.
///
/// One parser, two callers, deliberately: `FC_LOG_LEVEL` and SPEC.md §15.1's
/// `set_log_level` both take a name from outside the process. A second table would drift,
/// and the direction it drifts is always the one with fewer tests -- here that would mean
/// a level the environment accepts and the IPC command rejects, or the reverse.
///
/// Case-insensitive because `to_string` emits `"TRACE"` while every human writes
/// `"trace"`, and a round-trip-only parser would refuse the spelling everyone uses.
[[nodiscard]] std::optional<Level> level_from_string(std::string_view name) noexcept;

struct Config {
    /// Defaults to `%LOCALAPPDATA%\FrameCapture\logs`. Overridable so tests never
    /// write into the real user profile.
    std::filesystem::path directory;
    std::string filename = "framecapture.log";

    /// SPEC.md §18: rotating file sink, 10 MB x 5.
    std::size_t max_file_size = std::size_t{10} * 1024 * 1024;
    std::size_t max_files = 5;

    /// SPEC.md §18: in-memory ring of the last 2000 entries.
    std::size_t ring_capacity = 2000;

    Level level = Level::Info;

    /// MSVC debug-output sink. Defaults to true only in debug builds.
    bool enable_msvc_sink =
#ifdef NDEBUG
        false;
#else
        true;
#endif

    /// Leave empty to generate one. Supplying it makes log assertions in tests
    /// deterministic.
    std::string session_id;

    /// Async queue depth. Bounded on purpose (CLAUDE.md §4: no unbounded queues);
    /// overflow drops the oldest message rather than blocking the producer, since
    /// the producer may be the capture thread.
    std::size_t async_queue_size = 16384;
};

[[nodiscard]] std::filesystem::path default_log_directory();

/// Installs the sinks and starts the async worker. Idempotent-hostile: calling it
/// twice without `shutdown()` returns `INTERNAL_INVALID_STATE`.
[[nodiscard]] Result<void> init(const Config& config = {});

/// Flushes and tears down. Safe to call when uninitialised.
void shutdown();

[[nodiscard]] bool is_initialized() noexcept;

/// Stable per-process id stamped on every line (SPEC.md §18).
[[nodiscard]] std::string_view session_id() noexcept;

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;

/// Cheap atomic comparison. The FC_LOG_* macros call this *before* building any
/// LogFields, which is what keeps a disabled TRACE call site free of allocation.
[[nodiscard]] bool should_log(Level level) noexcept;

void write(Level level, Subsystem subsystem, std::string_view message);
void write(Level level, Subsystem subsystem, std::string_view message, const LogFields& fields);

/// Blocks until every message submitted before this call has been written by
/// every sink, then flushes the sinks' own buffers. Bounded at 2 seconds.
///
/// This does *not* delegate to `spdlog::logger::flush()`, which is fire-and-forget
/// for async loggers in every overflow policy and therefore guarantees nothing.
/// The barrier is built from a submitted/processed counter pair instead.
///
/// For shutdown and tests. Never call it from a capture or audio thread.
void flush();

/// Ring contents, oldest first.
[[nodiscard]] std::vector<std::string> ring_snapshot();
[[nodiscard]] std::size_t ring_size();

/// Non-owning handle to the ring sink, for the crash handler. Null when
/// uninitialised. Not for general use.
[[nodiscard]] RingSink* ring_sink() noexcept;

/// Path of the active rotating log file, for the crash report and the diagnostic
/// bundle. Empty when uninitialised.
[[nodiscard]] std::filesystem::path log_file_path();

} // namespace log
} // namespace fc

// ---------------------------------------------------------------------------
// Call-site macros.
//
// The level check happens before the argument list is evaluated, so a filtered
// call costs one relaxed atomic load and nothing else. That property is why these
// are macros and not functions with default arguments.
//
// CLAUDE.md §4: never log above TRACE on the capture or audio threads.
// ---------------------------------------------------------------------------

#define FC_LOG_AT(level, subsystem, ...)                                                                               \
    do {                                                                                                               \
        if (::fc::log::should_log(level)) {                                                                            \
            ::fc::log::write((level), (subsystem), __VA_ARGS__);                                                       \
        }                                                                                                              \
    } while (false)

#define FC_LOG_TRACE(subsystem, ...) FC_LOG_AT(::fc::log::Level::Trace, subsystem, __VA_ARGS__)
#define FC_LOG_DEBUG(subsystem, ...) FC_LOG_AT(::fc::log::Level::Debug, subsystem, __VA_ARGS__)
#define FC_LOG_INFO(subsystem, ...) FC_LOG_AT(::fc::log::Level::Info, subsystem, __VA_ARGS__)
#define FC_LOG_WARN(subsystem, ...) FC_LOG_AT(::fc::log::Level::Warn, subsystem, __VA_ARGS__)
#define FC_LOG_ERROR(subsystem, ...) FC_LOG_AT(::fc::log::Level::Error, subsystem, __VA_ARGS__)
#define FC_LOG_CRITICAL(subsystem, ...) FC_LOG_AT(::fc::log::Level::Critical, subsystem, __VA_ARGS__)
