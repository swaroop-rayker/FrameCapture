#include "core/logging/ring_sink.h"

#include <windows.h>

#include <algorithm>
#include <thread>

namespace fc {
namespace {

/// How long the crash path is willing to wait for the buffer lock before giving
/// up and reading anyway. Short: the handler is on a deadline of the user's
/// patience and of whatever else is corrupted.
constexpr int kCrashLockSpinAttempts = 200;

bool write_all(HANDLE handle, const void* data, std::size_t bytes) noexcept {
    const auto* cursor = static_cast<const char*>(data);
    std::size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (WriteFile(handle, cursor, chunk, &written, nullptr) == 0 || written == 0) {
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

RingSink::RingSink(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
    // Sized once. A soak run must not see this vector reallocate.
    entries_.resize(capacity_);
}

void RingSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    {
        const std::lock_guard lock(buffer_mutex_);
        // assign() into the existing string reuses its capacity instead of allocating.
        entries_[next_].assign(formatted.data(), formatted.size());
        next_ = (next_ + 1) % capacity_;
        count_ = std::min(count_ + 1, capacity_);
    }
    // Released after the entry is visible, so an observer that sees this count
    // also sees the entry.
    processed_.fetch_add(1, std::memory_order_release);
}

void RingSink::flush_() {
    // Nothing to flush: the ring is the destination, not a buffer in front of one.
}

std::vector<std::string> RingSink::snapshot() const {
    const std::lock_guard lock(buffer_mutex_);

    std::vector<std::string> out;
    out.reserve(count_);
    // When the ring has wrapped, the oldest surviving entry is the one we are
    // about to overwrite next.
    const std::size_t start = (count_ == capacity_) ? next_ : 0;
    for (std::size_t i = 0; i < count_; ++i) {
        out.push_back(entries_[(start + i) % capacity_]);
    }
    return out;
}

std::size_t RingSink::size() const {
    const std::lock_guard lock(buffer_mutex_);
    return count_;
}

void RingSink::clear() {
    const std::lock_guard lock(buffer_mutex_);
    for (std::string& entry : entries_) {
        entry.clear();
    }
    next_ = 0;
    count_ = 0;
}

bool RingSink::write_to_file_best_effort(const wchar_t* path) const noexcept {
    if (path == nullptr) {
        return false;
    }

    bool locked = false;
    for (int attempt = 0; attempt < kCrashLockSpinAttempts; ++attempt) {
        if (buffer_mutex_.try_lock()) {
            locked = true;
            break;
        }
        std::this_thread::yield();
    }
    // Proceeding without the lock is the documented trade-off; see the header.

    const HANDLE handle =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (locked) {
            buffer_mutex_.unlock();
        }
        return false;
    }

    bool ok = true;
    const std::size_t start = (count_ == capacity_) ? next_ : 0;
    for (std::size_t i = 0; i < count_ && ok; ++i) {
        const std::string& entry = entries_[(start + i) % capacity_];
        if (entry.empty()) {
            continue;
        }
        ok = write_all(handle, entry.data(), entry.size());
        if (ok && entry.back() != '\n') {
            ok = write_all(handle, "\r\n", 2);
        }
    }

    FlushFileBuffers(handle);
    CloseHandle(handle);

    if (locked) {
        buffer_mutex_.unlock();
    }
    return ok;
}

} // namespace fc
