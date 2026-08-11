#include "core/util/atomic_write.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"

#include <windows.h>

#include <system_error>

namespace fc {
namespace {

/// RAII for a Win32 handle. CLAUDE.md §4: no raw owning handles.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~FileHandle() {
        reset();
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    void reset() noexcept {
        if (valid()) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

FcError classify_last_error(DWORD error) noexcept {
    switch (error) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return FcError::IO_DISK_FULL;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return FcError::IO_PERMISSION_DENIED;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return FcError::IO_PATH_INVALID;
    case ERROR_INVALID_HANDLE:
        return FcError::IO_FILE_HANDLE_LOST;
    default:
        return FcError::IO_FILE_WRITE_FAILED;
    }
}

void log_win32_failure(std::string_view operation, const std::filesystem::path& path, DWORD error, FcError mapped) {
    FC_LOG_ERROR(Subsystem::Io, "atomic write failed",
                 LogFields{}
                     .add("operation", operation)
                     .add("path", path.string())
                     .add("hr", hresult_message(static_cast<HResult>(HRESULT_FROM_WIN32(error))))
                     .add_error(mapped));
}

} // namespace

std::filesystem::path atomic_temp_path(const std::filesystem::path& path) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    return temp;
}

Result<void> write_file_atomically(const std::filesystem::path& path, std::string_view contents,
                                   AtomicWriteFault fault) {
    if (path.empty()) {
        return FcError::IO_PATH_INVALID;
    }

    const std::filesystem::path temp = atomic_temp_path(path);

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec && !std::filesystem::is_directory(path.parent_path())) {
            return FcError::IO_DIRECTORY_CREATE_FAILED;
        }
    }

    {
        const FileHandle handle{
            CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!handle.valid()) {
            const DWORD error = GetLastError();
            const FcError mapped = classify_last_error(error);
            log_win32_failure("CreateFileW", temp, error, mapped);
            return mapped;
        }

        if (fault == AtomicWriteFault::BeforeWrite) {
            // Leave the empty temp file behind, as a crash here would.
            return FcError::INTERNAL_CANCELLED;
        }

        std::size_t remaining = contents.size();
        const char* cursor = contents.data();
        while (remaining > 0) {
            const auto chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
            DWORD written = 0;
            if (WriteFile(handle.get(), cursor, chunk, &written, nullptr) == 0 || written == 0) {
                const DWORD error = GetLastError();
                const FcError mapped = classify_last_error(error);
                log_win32_failure("WriteFile", temp, error, mapped);
                return mapped;
            }
            cursor += written;
            remaining -= written;
        }

        if (fault == AtomicWriteFault::AfterWrite) {
            return FcError::INTERNAL_CANCELLED;
        }

        // Without this the rename below can become durable before the bytes do,
        // which is how a power cut produces a zero-length config file.
        if (FlushFileBuffers(handle.get()) == 0) {
            const DWORD error = GetLastError();
            const FcError mapped = classify_last_error(error);
            log_win32_failure("FlushFileBuffers", temp, error, mapped);
            return mapped;
        }

        if (fault == AtomicWriteFault::AfterFlush) {
            return FcError::INTERNAL_CANCELLED;
        }
    } // handle closed before the rename -- MoveFileEx cannot replace an open file

    if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD error = GetLastError();
        const FcError mapped = error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION
                                   ? FcError::IO_PERMISSION_DENIED
                                   : FcError::IO_ATOMIC_REPLACE_FAILED;
        log_win32_failure("MoveFileExW", path, error, mapped);

        // Keep the temp file: it holds the content the caller wanted written, and
        // discarding it would turn a recoverable failure into data loss.
        return mapped;
    }

    FC_LOG_DEBUG(Subsystem::Io, "file replaced atomically",
                 LogFields{}.add("path", path.string()).add("bytes", static_cast<std::uint64_t>(contents.size())));
    return ok();
}

} // namespace fc
