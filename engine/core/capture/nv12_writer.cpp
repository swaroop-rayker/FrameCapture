#include "core/capture/nv12_writer.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"

#include <windows.h>

#include <algorithm>
#include <system_error>

namespace fc::capture {
namespace {

/// Write in at most this much per call. WriteFile takes a DWORD, and a 1 MiB
/// granularity keeps a slow-disk stall observable rather than one huge blocking
/// call.
constexpr DWORD kChunkBytes = 1u << 20;

FcError classify(DWORD error) noexcept {
    switch (error) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return FcError::IO_DISK_FULL;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return FcError::IO_PERMISSION_DENIED;
    case ERROR_INVALID_HANDLE:
        return FcError::IO_FILE_HANDLE_LOST;
    default:
        return FcError::IO_FILE_WRITE_FAILED;
    }
}

} // namespace

struct Nv12Writer::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    std::size_t expected_frame_bytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;

    void reset() noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

Nv12Writer::Nv12Writer() : impl_(std::make_unique<Impl>()) {}

Nv12Writer::~Nv12Writer() {
    if (impl_ != nullptr) {
        impl_->reset();
    }
}

Nv12Writer::Nv12Writer(Nv12Writer&&) noexcept = default;

Nv12Writer& Nv12Writer::operator=(Nv12Writer&&) noexcept = default;

Result<void> Nv12Writer::open(const std::filesystem::path& path, int width, int height) {
    if (width <= 0 || height <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    impl_->reset();

    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec && !std::filesystem::is_directory(path.parent_path())) {
            return FcError::IO_DIRECTORY_CREATE_FAILED;
        }
    }

    // FILE_FLAG_SEQUENTIAL_SCAN tells the cache manager what this access pattern
    // is, which matters when writing hundreds of MB of raw frames.
    impl_->handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        const FcError mapped = classify(error);
        FC_LOG_ERROR(Subsystem::Io, "raw NV12 output could not be opened",
                     LogFields{}
                         .add("path", path.string())
                         .add("hr", hresult_message(static_cast<HResult>(HRESULT_FROM_WIN32(error))))
                         .add_error(mapped));
        return mapped;
    }

    impl_->path = path;
    impl_->expected_frame_bytes = frame_size(width, height);
    impl_->frames = 0;
    impl_->bytes = 0;

    FC_LOG_INFO(Subsystem::Io, "writing raw NV12",
                LogFields{}
                    .add("path", path.string())
                    .add("width", width)
                    .add("height", height)
                    .add("frame_bytes", static_cast<std::uint64_t>(impl_->expected_frame_bytes)));
    return ok();
}

Result<void> Nv12Writer::write(std::span<const std::uint8_t> frame) {
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (frame.size() != impl_->expected_frame_bytes) {
        // A short frame would silently corrupt every frame after it, because the
        // format has no framing of its own.
        FC_LOG_ERROR(Subsystem::Io, "raw NV12 frame is the wrong size",
                     LogFields{}
                         .add("expected", static_cast<std::uint64_t>(impl_->expected_frame_bytes))
                         .add("actual", static_cast<std::uint64_t>(frame.size()))
                         .add_error(FcError::INTERNAL_INVALID_ARGUMENT));
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    std::size_t remaining = frame.size();
    const std::uint8_t* cursor = frame.data();
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, kChunkBytes));
        DWORD written = 0;
        if (WriteFile(impl_->handle, cursor, chunk, &written, nullptr) == 0 || written == 0) {
            const DWORD error = GetLastError();
            const FcError mapped = classify(error);
            FC_LOG_ERROR(Subsystem::Io, "raw NV12 write failed",
                         LogFields{}
                             .add("path", impl_->path.string())
                             .add("hr", hresult_message(static_cast<HResult>(HRESULT_FROM_WIN32(error))))
                             .add_error(mapped));
            return mapped;
        }
        cursor += written;
        remaining -= written;
        impl_->bytes += written;
    }

    ++impl_->frames;
    return ok();
}

Result<void> Nv12Writer::close() {
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        return ok();
    }

    const bool flushed = FlushFileBuffers(impl_->handle) != 0;
    impl_->reset();

    FC_LOG_INFO(Subsystem::Io, "raw NV12 output closed",
                LogFields{}.add("path", impl_->path.string()).add("frames", impl_->frames).add("bytes", impl_->bytes));

    return flushed ? ok() : Result<void>{FcError::IO_FILE_WRITE_FAILED};
}

std::uint64_t Nv12Writer::frames_written() const noexcept {
    return impl_->frames;
}

std::uint64_t Nv12Writer::bytes_written() const noexcept {
    return impl_->bytes;
}

} // namespace fc::capture
