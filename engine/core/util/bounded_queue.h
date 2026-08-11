#pragma once

// Bounded inter-stage transport (SPEC.md §12, CLAUDE.md hard rule 5).
//
// Bounded is not a limitation, it is the mechanism. An unbounded queue converts a
// 200 ms hitch into unbounded memory growth and then an OOM; a bounded one makes
// backpressure observable at the moment it happens and forces the drop policy to
// be a decision someone wrote down rather than an emergent property.
//
// Two policies, because the two media types have opposite requirements
// (SPEC.md §12):
//
//   DropOldest  video. A dropped frame is a visual glitch of one frame. Dropping
//               the *oldest* keeps the queue's contents closest to the present,
//               which is what matters when the consumer has fallen behind.
//   Block       audio. Audio drops are audible and desync the stream, so audio is
//               never dropped -- pressure on the audio path degrades video
//               instead. Used from M4.
//
// Every drop is counted. A queue that silently discards work is indistinguishable
// from one that never received it, and §20 row 10 needs the number.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace fc {

enum class QueuePolicy {
    /// Overwrite the oldest entry when full. Video.
    DropOldest,
    /// Wait for space. Audio -- never drops.
    Block,
};

/// Bounded multi-producer/multi-consumer queue with an explicit drop policy.
///
/// SPEC.md §12 specifies lock-free SPSC ring buffers for the inter-stage
/// transport. This is a mutex-based bounded queue with the same capacity and drop
/// semantics: it is the *bound* and the *policy* that carry the correctness
/// argument, and both are identical here. The lock is held only to move a pointer
/// into or out of a slot -- never across a D3D call or a disk write, which is the
/// rule that actually matters (SPEC.md §12, CLAUDE.md hard rule 4).
///
/// Sized for pointer-like payloads. `T` must be movable.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity, QueuePolicy policy = QueuePolicy::DropOldest)
        : slots_(capacity), capacity_(capacity), policy_(policy) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;
    ~BoundedQueue() = default;

    /// Result of a push, so the caller can react to pressure rather than guess.
    enum class PushResult {
        Accepted,
        /// Accepted, but the oldest entry was discarded to make room.
        DisplacedOldest,
        /// Rejected: the queue is closed.
        Closed,
    };

    /// Enqueues `value`.
    ///
    /// Under `DropOldest` this never blocks and never fails except on a closed
    /// queue -- which is what lets the capture and encode threads call it without
    /// violating "never block the capture thread".
    PushResult push(T value) {
        std::unique_lock lock(mutex_);
        if (closed_) {
            return PushResult::Closed;
        }

        if (size_ == capacity_) {
            if (policy_ == QueuePolicy::Block) {
                not_full_.wait(lock, [this] { return size_ < capacity_ || closed_; });
                if (closed_) {
                    return PushResult::Closed;
                }
            } else {
                // Discard the oldest to make room. The slot is overwritten below.
                head_ = (head_ + 1) % capacity_;
                --size_;
                ++dropped_;
                emplace_at_tail(std::move(value));
                lock.unlock();
                not_empty_.notify_one();
                return PushResult::DisplacedOldest;
            }
        }

        emplace_at_tail(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return PushResult::Accepted;
    }

    /// Waits for an entry. `std::nullopt` means the queue was closed and drained --
    /// the consumer's signal to finish, not an error.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return size_ > 0 || closed_; });
        if (size_ == 0) {
            return std::nullopt;
        }
        return take_locked(lock);
    }

    /// Non-blocking pop. `std::nullopt` when empty, whether or not it is closed.
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock lock(mutex_);
        if (size_ == 0) {
            return std::nullopt;
        }
        return take_locked(lock);
    }

    /// Wakes every waiter and stops accepting new entries. Entries already queued
    /// remain poppable, so a consumer drains before it sees `nullopt` -- the
    /// property finalization depends on (SPEC.md §10.4: drain, then write trailer).
    void close() {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /// Reopens a closed queue so a stage can be restarted (SPEC.md §5.4's migration
    /// stops the `venc` thread, rebuilds the encoder underneath it, and starts it
    /// again on the same queue).
    ///
    /// Refuses unless the queue is drained. Reopening with entries still in it would
    /// hand the restarted consumer frames belonging to the *previous* device, whose
    /// textures have been released -- a use-after-free wearing a queue's clothing.
    [[nodiscard]] bool reopen() {
        const std::lock_guard lock(mutex_);
        if (size_ != 0) {
            return false;
        }
        closed_ = false;
        return true;
    }

    [[nodiscard]] bool closed() const {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return size_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /// Total entries discarded by the drop policy over the queue's lifetime.
    [[nodiscard]] std::uint64_t dropped() const {
        const std::lock_guard lock(mutex_);
        return dropped_;
    }

    /// Occupancy in [0, 1]. The degradation ladder's trigger input (SPEC.md §13
    /// rungs 1 and 2 are thresholds on this), consumed from M6.
    [[nodiscard]] double pressure() const {
        const std::lock_guard lock(mutex_);
        return capacity_ == 0 ? 0.0 : static_cast<double>(size_) / static_cast<double>(capacity_);
    }

private:
    void emplace_at_tail(T&& value) {
        slots_[tail_] = std::move(value);
        tail_ = (tail_ + 1) % capacity_;
        ++size_;
    }

    T take_locked(std::unique_lock<std::mutex>& lock) {
        T value = std::move(slots_[head_]);
        slots_[head_] = T{};
        head_ = (head_ + 1) % capacity_;
        --size_;
        lock.unlock();
        not_full_.notify_one();
        return value;
    }

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;

    std::vector<T> slots_;
    std::size_t capacity_;
    QueuePolicy policy_;

    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

} // namespace fc
