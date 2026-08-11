// Bounded inter-stage transport (SPEC.md §12, CLAUDE.md hard rule 5).
//
// CPU TIER. The properties that matter are the bound itself, the drop policy, and
// that a closed queue still drains -- finalization depends on the last of those.

#include "core/util/bounded_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

using fc::BoundedQueue;
using fc::QueuePolicy;

using IntQueue = BoundedQueue<int>;
using PushResult = IntQueue::PushResult;

// ---------------------------------------------------------------------------
// The bound
// ---------------------------------------------------------------------------

TEST(BoundedQueue, NeverGrowsBeyondItsCapacity) {
    IntQueue queue(4);
    for (int i = 0; i < 1000; ++i) {
        queue.push(i);
    }
    // The entire point: 1000 pushes into a queue of 4 is 4 entries, not an OOM.
    EXPECT_EQ(queue.size(), 4u);
    EXPECT_EQ(queue.capacity(), 4u);
}

TEST(BoundedQueue, FifoOrderIsPreservedWhenItNeverFills) {
    IntQueue queue(8);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(queue.push(i), PushResult::Accepted);
    }
    for (int i = 0; i < 5; ++i) {
        const auto value = queue.try_pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
    EXPECT_FALSE(queue.try_pop().has_value());
}

// The ring has to wrap correctly, which a queue that is only ever half-filled
// never exercises.
TEST(BoundedQueue, SurvivesManyWrapsAroundTheRing) {
    IntQueue queue(3);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(queue.push(i), PushResult::Accepted);
        const auto value = queue.try_pop();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, i);
    }
    EXPECT_EQ(queue.size(), 0u);
}

// ---------------------------------------------------------------------------
// Drop policy -- video
// ---------------------------------------------------------------------------

TEST(BoundedQueue, DropOldestKeepsTheNewestEntries) {
    IntQueue queue(3, QueuePolicy::DropOldest);
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(queue.push(i), PushResult::Accepted);
    }
    // Now full. The next push displaces 0.
    EXPECT_EQ(queue.push(99), PushResult::DisplacedOldest);

    std::vector<int> drained;
    while (const auto value = queue.try_pop()) {
        drained.push_back(*value);
    }
    // Oldest gone, order otherwise intact. A consumer that has fallen behind wants
    // the most recent frames, not the stalest ones.
    EXPECT_EQ(drained, (std::vector<int>{1, 2, 99}));
}

TEST(BoundedQueue, EveryDropIsCounted) {
    IntQueue queue(2, QueuePolicy::DropOldest);
    for (int i = 0; i < 10; ++i) {
        queue.push(i);
    }
    // A queue that silently discards work is indistinguishable from one that never
    // received it; SPEC.md §20 row 10 needs this number.
    EXPECT_EQ(queue.dropped(), 8u);
}

TEST(BoundedQueue, DropOldestNeverBlocksTheProducer) {
    // The property that lets the capture thread push without violating
    // "never block the capture thread" (CLAUDE.md hard rule 4). If this ever
    // blocked, the test would hang rather than fail -- which is why the queue has
    // no consumer at all here.
    IntQueue queue(2, QueuePolicy::DropOldest);
    for (int i = 0; i < 10'000; ++i) {
        ASSERT_NE(queue.push(i), PushResult::Closed);
    }
    SUCCEED();
}

TEST(BoundedQueue, PressureTracksOccupancy) {
    IntQueue queue(4);
    EXPECT_DOUBLE_EQ(queue.pressure(), 0.0);
    queue.push(1);
    queue.push(2);
    // SPEC.md §13 rungs 1 and 2 are thresholds on exactly this value.
    EXPECT_DOUBLE_EQ(queue.pressure(), 0.5);
    queue.push(3);
    queue.push(4);
    EXPECT_DOUBLE_EQ(queue.pressure(), 1.0);
}

// ---------------------------------------------------------------------------
// Close and drain -- what finalization depends on
// ---------------------------------------------------------------------------

// SPEC.md §10.4 stops by draining the mux queue and *then* writing the trailer. If
// close() discarded queued entries, the tail of every recording would be lost.
TEST(BoundedQueue, ClosingStillLetsQueuedEntriesDrain) {
    IntQueue queue(8);
    for (int i = 0; i < 5; ++i) {
        queue.push(i);
    }
    queue.close();

    for (int i = 0; i < 5; ++i) {
        const auto value = queue.pop();
        ASSERT_TRUE(value.has_value()) << "entry " << i << " was lost by close()";
        EXPECT_EQ(*value, i);
    }
    // Only once drained does pop report the end.
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(BoundedQueue, PushingToAClosedQueueIsRejected) {
    IntQueue queue(4);
    queue.close();
    EXPECT_EQ(queue.push(1), PushResult::Closed);
    EXPECT_EQ(queue.size(), 0u);
}

TEST(BoundedQueue, CloseWakesABlockedConsumer) {
    IntQueue queue(4);
    std::atomic<bool> returned{false};

    std::thread consumer([&] {
        const auto value = queue.pop(); // blocks: queue is empty
        returned.store(!value.has_value());
    });

    queue.close();
    consumer.join();
    EXPECT_TRUE(returned.load()) << "pop did not return after close";
}

// ---------------------------------------------------------------------------
// Blocking policy -- audio (SPEC.md §12: audio is never dropped)
// ---------------------------------------------------------------------------

TEST(BoundedQueue, BlockPolicyWaitsForSpaceInsteadOfDropping) {
    BoundedQueue<int> queue(2, QueuePolicy::Block);
    queue.push(1);
    queue.push(2);

    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        queue.push(3); // blocks until the consumer makes room
        pushed.store(true);
    });

    // Make room; the producer completes.
    const auto first = queue.pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1);
    producer.join();

    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(queue.dropped(), 0u) << "the audio policy dropped a sample";
}

// ---------------------------------------------------------------------------
// Move-only payloads -- the real use is unique_ptr-held frames
// ---------------------------------------------------------------------------

TEST(BoundedQueue, CarriesMoveOnlyPayloads) {
    BoundedQueue<std::unique_ptr<int>> queue(4);
    queue.push(std::make_unique<int>(7));

    auto value = queue.pop();
    ASSERT_TRUE(value.has_value());
    ASSERT_NE(*value, nullptr);
    EXPECT_EQ(**value, 7);
}

TEST(BoundedQueue, DisplacedMoveOnlyPayloadsAreReleased) {
    BoundedQueue<std::shared_ptr<int>> queue(2, QueuePolicy::DropOldest);
    auto tracked = std::make_shared<int>(1);
    EXPECT_EQ(tracked.use_count(), 1);

    queue.push(tracked);
    EXPECT_EQ(tracked.use_count(), 2);

    // Fill past capacity so the tracked entry is displaced.
    queue.push(std::make_shared<int>(2));
    queue.push(std::make_shared<int>(3));

    // A displaced entry must actually be destroyed, or the "bounded" queue leaks
    // just as badly as an unbounded one -- it would pin every texture it ever held.
    EXPECT_EQ(tracked.use_count(), 1);
}

TEST(BoundedQueue, PoppedSlotsDoNotRetainTheirPayload) {
    BoundedQueue<std::shared_ptr<int>> queue(4);
    auto tracked = std::make_shared<int>(1);
    queue.push(tracked);
    EXPECT_EQ(tracked.use_count(), 2);

    {
        const auto value = queue.pop();
    }
    EXPECT_EQ(tracked.use_count(), 1) << "the ring slot still holds a reference";
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(BoundedQueue, ProducerAndConsumerAgreeOnTheTotal) {
    IntQueue queue(16, QueuePolicy::Block);
    constexpr int kCount = 20'000;

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            queue.push(i);
        }
        queue.close();
    });

    int received = 0;
    long long sum = 0;
    while (const auto value = queue.pop()) {
        sum += *value;
        ++received;
    }
    producer.join();

    EXPECT_EQ(received, kCount);
    EXPECT_EQ(sum, static_cast<long long>(kCount) * (kCount - 1) / 2);
}

} // namespace
