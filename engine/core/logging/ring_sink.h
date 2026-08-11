#pragma once

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fc {

/// In-memory ring of the last N formatted log entries (SPEC.md §18: "an in-memory
/// ring of the last 2000 entries", dumped on crash).
///
/// Derives from `base_sink<null_mutex>` and locks internally instead of letting
/// the base class do it, because the crash path needs access to that same lock
/// with `try_lock` semantics -- see `write_to_file_best_effort`.
class RingSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    explicit RingSink(std::size_t capacity);

    /// Entries in chronological order, oldest first.
    [[nodiscard]] std::vector<std::string> snapshot() const;

    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /// Total messages this sink has finished writing since construction.
    ///
    /// Exists so `log::flush()` can implement a real barrier. spdlog's
    /// `async_logger::flush()` is fire-and-forget in every overflow policy, so
    /// "the queue looks empty" is not the same as "the last message was written" --
    /// the worker can be part-way through `sink_it_`. Counting completions is the
    /// only way to know. Incremented last, after the entry is in the buffer.
    [[nodiscard]] std::uint64_t processed() const noexcept {
        return processed_.load(std::memory_order_acquire);
    }

    void clear();

    /// Writes the ring to `path` from a crash context.
    ///
    /// **Best effort, and deliberately willing to race.** It spins briefly on
    /// `try_lock`; if the lock cannot be acquired -- the likely case when the
    /// crash happened *inside* the logger -- it reads the buffer anyway rather
    /// than deadlocking. A torn crash log is worth more than no crash log, and a
    /// hung crash handler produces neither.
    ///
    /// Uses Win32 file APIs and no allocation, so it stays viable when the heap
    /// is the thing that broke.
    bool write_to_file_best_effort(const wchar_t* path) const noexcept;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    mutable std::mutex buffer_mutex_;
    std::atomic<std::uint64_t> processed_{0};
    std::size_t capacity_;
    std::vector<std::string> entries_; ///< Fixed size == capacity_; reused, never reallocated.
    std::size_t next_ = 0;             ///< Index of the next slot to write.
    std::size_t count_ = 0;            ///< Number of valid entries, saturating at capacity_.
};

} // namespace fc
