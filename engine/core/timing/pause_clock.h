#pragma once

// The paused total (SPEC.md §7.5).
//
// Pause **excises** time. One file, and the paused span simply is not in it -- frame
// *N* before the pause and frame *N+1* after it are adjacent, in presentation order
// and on screen. That is what separates pause from stop/start, and it is what makes
// it dangerous, because SPEC.md §7.1 makes every PTS a function of `qpc - t0` and
// excising time gives that mapping a second term:
//
//     timeline_ns = qpc_ns - t0_ns - paused_total_ns
//
// `paused_total_ns` is **shared by video and audio exactly as `t0` is**. Two streams
// subtracting different totals desync permanently and invisibly -- the same failure
// §7.1's shared epoch exists to prevent, arriving by a new route, and one no existing
// test would catch because every one of them records without pausing.
//
// So there is exactly one of these per recording, it is the only place the subtraction
// is written, and both `timing::Pacer` and `audio::AudioTimeline` are handed a pointer
// to it rather than keeping a total of their own. §7.5 says so in as many words: it is
// "never maintained independently by either".
//
// ---------------------------------------------------------------------------
// Why `map` can refuse, and why that is not an error
// ---------------------------------------------------------------------------
// A timestamp inside the paused span has no place on the timeline -- that is the
// definition of excised. `map` reports `Excised` for it rather than returning a number,
// because every arithmetic answer would be wrong and the caller's correct move is to
// discard the item. Returning `0`, or the span's start, would place paused content at
// the resume seam, which is the frozen-frame outcome §7.5 explicitly rules out.
//
// ---------------------------------------------------------------------------
// The straggler problem, stated rather than hoped away
// ---------------------------------------------------------------------------
// `paused_total_ns` grows at the *resume* instant, but items are mapped when the venc
// or aenc thread reaches them, not when they were captured. An item captured before the
// pause and mapped after the resume would have the new total subtracted from it and
// land in the wrong slot.
//
// The pipeline prevents this by quiescing at the pause boundary: producers stop, and
// the session drains what is in flight before the pause is published. That makes
// stragglers impossible rather than unlikely -- but "impossible" is a claim about code
// elsewhere, so this class *counts* them (`stragglers()`) instead of trusting it. A
// straggler is mapped with the total as it stood before the most recent pause, which is
// exact for one level of staleness; anything older than that is counted and mapped with
// the best available answer. SPEC.md §20 row 18's test asserts the count is zero, so the
// quiesce is verified rather than assumed.

#include <atomic>
#include <cstdint>

namespace fc::timing {

/// The shared paused total. One per recording; read from the `venc` and `aenc`
/// threads, written only by the thread processing the pause/resume command.
///
/// Lock-free by construction: three atomics and no allocation, because
/// `map` is called once per frame and once per audio buffer and CLAUDE.md hard rule 4
/// forbids a mutex on either path.
class PauseClock {
public:
    enum class State {
        /// The timestamp maps to a place on the timeline.
        Running,
        /// The timestamp falls inside a paused span. The item must be discarded --
        /// there is no correct place for it, by definition.
        Excised,
    };

    struct Mapping {
        State state = State::Running;
        /// `qpc_ns - t0_ns - paused_total`, floored at zero. Meaningless when
        /// `state` is `Excised`.
        std::int64_t timeline_ns = 0;
    };

    PauseClock() = default;
    ~PauseClock() = default;

    PauseClock(const PauseClock&) = delete;
    PauseClock& operator=(const PauseClock&) = delete;
    PauseClock(PauseClock&&) = delete;
    PauseClock& operator=(PauseClock&&) = delete;

    /// Opens a paused span at `qpc_ns`.
    ///
    /// **Idempotent** (SPEC.md §7.5): pausing a paused recording is a no-op that
    /// succeeds. Returns false when nothing changed, so a caller can decide whether to
    /// emit a `state_changed` event without keeping its own copy of the state.
    bool pause(std::int64_t qpc_ns) noexcept {
        if (paused_.load(std::memory_order_acquire)) {
            return false;
        }
        pause_begin_ns_.store(qpc_ns, std::memory_order_relaxed);
        // Release last: it is the flag every reader tests first, so publishing it
        // after the span's start is what makes the start visible to anyone who sees
        // the flag.
        paused_.store(true, std::memory_order_release);
        return true;
    }

    /// Closes the paused span at `qpc_ns` and adds its duration to the total.
    ///
    /// Idempotent in the same sense. A resume timestamp before the pause began would
    /// shorten the total, which cannot happen with a monotonic clock and is clamped
    /// rather than trusted -- QPC is monotonic, but the value reaching here has been
    /// through an IPC hop and a mistake would be permanent and silent.
    bool resume(std::int64_t qpc_ns) noexcept {
        if (!paused_.load(std::memory_order_acquire)) {
            return false;
        }
        const std::int64_t began = pause_begin_ns_.load(std::memory_order_relaxed);
        const std::int64_t duration = qpc_ns > began ? qpc_ns - began : 0;

        last_span_ns_.store(duration, std::memory_order_relaxed);
        last_pause_begin_ns_.store(began, std::memory_order_relaxed);
        total_ns_.fetch_add(duration, std::memory_order_relaxed);
        pauses_.fetch_add(1, std::memory_order_relaxed);
        paused_.store(false, std::memory_order_release);
        return true;
    }

    /// Returns to the state of a recording that has never been paused.
    ///
    /// Exists because one pipeline can record more than once and this object outlives a
    /// single recording -- it is not copyable or movable, deliberately, since the pacer
    /// and the audio timeline hold pointers to it. A total carried into the next
    /// recording would excise time from a file that was never paused.
    ///
    /// Called from the owner's thread with no recording in flight.
    void reset() noexcept {
        paused_.store(false, std::memory_order_relaxed);
        pause_begin_ns_.store(0, std::memory_order_relaxed);
        total_ns_.store(0, std::memory_order_relaxed);
        last_pause_begin_ns_.store(0, std::memory_order_relaxed);
        last_span_ns_.store(0, std::memory_order_relaxed);
        pauses_.store(0, std::memory_order_relaxed);
        stragglers_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool paused() const noexcept {
        return paused_.load(std::memory_order_acquire);
    }

    /// Total excised time, in nanoseconds. Excludes a span still open -- a pause in
    /// progress has no duration yet.
    ///
    /// SPEC.md §7.5 requires this in `get_stats` and in the finalization log, because
    /// "why is my 30-minute recording 12 minutes long" must be answerable from the log.
    [[nodiscard]] std::int64_t paused_total_ns() const noexcept {
        return total_ns_.load(std::memory_order_relaxed);
    }

    /// Completed pause/resume cycles.
    [[nodiscard]] std::uint64_t pauses() const noexcept {
        return pauses_.load(std::memory_order_relaxed);
    }

    /// Items that reached `map` describing a moment before the most recent pause, after
    /// that pause had already ended. Zero when the pipeline quiesced correctly; see the
    /// header note. Non-zero is a defect in the quiesce, not in this class.
    [[nodiscard]] std::uint64_t stragglers() const noexcept {
        return stragglers_.load(std::memory_order_relaxed);
    }

    /// Places a captured **item** on the timeline. The one place SPEC.md §7.5's
    /// arithmetic is written.
    ///
    /// `t0_ns` is passed rather than held because the epoch is negotiated separately
    /// (`timing::SessionEpoch`) and resolves *after* this object exists -- holding a
    /// copy here would be a second source of truth for `t0`, which is the exact defect
    /// this class exists to avoid for `paused_total_ns`.
    [[nodiscard]] Mapping map(std::int64_t qpc_ns, std::int64_t t0_ns) const noexcept {
        return locate(qpc_ns, t0_ns, /*count_stragglers=*/true);
    }

    /// Same arithmetic, for a caller asking **where the timeline is** rather than where
    /// an item belongs.
    ///
    /// The distinction is bookkeeping, not maths, and it exists because `stragglers()`
    /// is an assertion target (SPEC.md §20 row 18 requires zero). A straggler is one
    /// *item* placed against a stale total; if a measurement that happens to consult the
    /// clock in the same instant also incremented the counter, the same event would be
    /// reported twice and a metric that is meant to prove the quiesce held would instead
    /// count how many things asked.
    ///
    /// SPEC.md §8.4's drift ladder is the caller this was added for (BUG-038): its
    /// `elapsed` term is the *timeline's* length, not wall clock, because the quantity
    /// it is compared against -- the encoded track -- already has paused time excised.
    /// `State::Excised` here means the timeline is not advancing at all, and the correct
    /// response is to not measure rather than to measure against a frozen value.
    [[nodiscard]] Mapping observe(std::int64_t qpc_ns, std::int64_t t0_ns) const noexcept {
        return locate(qpc_ns, t0_ns, /*count_stragglers=*/false);
    }

private:
    /// `map` and `observe` differ only in whether they count. Written once so they
    /// cannot drift apart -- two copies of §7.5's subtraction is the defect this whole
    /// class exists to prevent, and it would be no better inside the class than outside.
    [[nodiscard]] Mapping locate(std::int64_t qpc_ns, std::int64_t t0_ns, bool count_stragglers) const noexcept {
        std::int64_t excised = total_ns_.load(std::memory_order_relaxed);

        if (paused_.load(std::memory_order_acquire)) {
            const std::int64_t began = pause_begin_ns_.load(std::memory_order_relaxed);
            if (qpc_ns >= began) {
                return Mapping{State::Excised, 0};
            }
        } else if (qpc_ns < last_pause_begin_ns_.load(std::memory_order_relaxed)) {
            // A straggler: captured before the most recent pause, mapped after it
            // ended. The total it should be measured against is the one that was
            // current when it was captured.
            if (count_stragglers) {
                stragglers_.fetch_add(1, std::memory_order_relaxed);
            }
            excised -= last_span_ns_.load(std::memory_order_relaxed);
        }

        const std::int64_t timeline = qpc_ns - t0_ns - excised;
        return Mapping{State::Running, timeline > 0 ? timeline : 0};
    }

    std::atomic<bool> paused_{false};
    std::atomic<std::int64_t> pause_begin_ns_{0};
    std::atomic<std::int64_t> total_ns_{0};

    /// The most recently completed span, kept so a straggler can be measured against
    /// the total as it stood before it.
    std::atomic<std::int64_t> last_pause_begin_ns_{0};
    std::atomic<std::int64_t> last_span_ns_{0};

    std::atomic<std::uint64_t> pauses_{0};
    mutable std::atomic<std::uint64_t> stragglers_{0};
};

} // namespace fc::timing
