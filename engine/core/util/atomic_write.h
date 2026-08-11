#pragma once

#include "core/error/result.h"

#include <filesystem>
#include <string_view>

namespace fc {

/// Where to abandon an atomic write, for testing crash-safety.
///
/// A real interruption is a process death, which a unit test cannot stage. So the
/// write is instrumented instead: each fault stops at the same point a crash could,
/// leaving the filesystem in exactly the state a crash would leave it. The
/// invariant every fault must satisfy is that the *destination* is either the old
/// content or the new content, never a mixture and never absent.
enum class AtomicWriteFault {
    None,
    /// Temp file created, nothing written to it yet.
    BeforeWrite,
    /// Payload written but not flushed to the device.
    AfterWrite,
    /// Flushed, but the rename never happened.
    AfterFlush,
};

/// Writes `contents` to `path` atomically (SPEC.md §17).
///
/// The sequence is exactly the one the spec prescribes, and the order matters:
///   1. write `<path>.tmp`
///   2. `FlushFileBuffers` -- without this the rename can be durable while the
///      data behind it is not, which yields a zero-length config after a power cut
///   3. `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`
///
/// On any failure the destination is left untouched. The temp file is removed on a
/// clean failure and deliberately left behind on a simulated fault, because that is
/// what a crash would do.
[[nodiscard]] Result<void> write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                                                 AtomicWriteFault fault = AtomicWriteFault::None);

/// The temp path `write_file_atomically` uses. Exposed so tests and recovery code
/// can look for a leftover.
[[nodiscard]] std::filesystem::path atomic_temp_path(const std::filesystem::path& path);

} // namespace fc
