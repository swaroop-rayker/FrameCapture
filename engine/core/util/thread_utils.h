#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fc {

/// Names the calling thread (SPEC.md §12: "All threads are named via
/// SetThreadDescription so they are legible in a debugger and in a minidump").
///
/// Does two things:
///   1. Calls `SetThreadDescription`, so the name shows up in a debugger and in
///      a minidump.
///   2. Registers the name in a process-wide id -> name table, so the *async*
///      logger can resolve it.
///
/// The registry exists because spdlog formats on its worker thread, not on the
/// thread that emitted the line. A `thread_local` name would make every log line
/// claim to come from the log worker.
void set_thread_name(std::string_view name);

/// The calling thread's registered name, or "unnamed".
[[nodiscard]] std::string current_thread_name();

/// Looks up a name by the thread id spdlog recorded on a message. Returns
/// "unnamed" for threads that never called `set_thread_name`.
[[nodiscard]] std::string thread_name_for(std::size_t thread_id);

/// Forgets the calling thread's registration. Call from a thread's exit path so
/// the table does not grow without bound in a long soak run.
void clear_thread_name();

} // namespace fc
