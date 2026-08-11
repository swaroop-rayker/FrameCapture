#pragma once

// Bounded worker shutdown (SPEC.md §12).
//
// > Every thread has an ownership-documented shutdown path with a bounded join
// > timeout (2 s), after which the shutdown is escalated and logged -- never an
// > unbounded `join()`.
//
// `std::thread::join` has no timed form, so the pattern is a promise the worker
// fulfils as it leaves its loop plus a `wait_for` on the matching future. Shared
// here rather than copied per pipeline stage: the escalation path (detach, log,
// deliberately leak the state the detached thread is still reading) is subtle
// enough that two copies would eventually disagree. See BUG-008.

#include "core/error/fc_error.h"
#include "core/logging/logger.h"
#include "core/subsystem.h"

#include <chrono>
#include <future>
#include <thread>

namespace fc {

/// SPEC.md §12's join deadline, for every thread that is forbidden to block.
inline constexpr auto kWorkerJoinTimeout = std::chrono::seconds{2};

/// The deadline for the threads whose shutdown waits on the disk (SPEC.md §15.1).
///
/// SPEC.md §12 gives every thread a 2 s bounded join, and for a thread that is
/// genuinely forbidden to block that is generous. Two of the pipeline's threads are
/// not in that category *at shutdown*, and BUG-026 is what that costs:
///
///   `mux`  §12's own exception -- the one thread permitted to block on disk. At `stop`
///          it has up to a queue's worth of packets to put on the platter before
///          `av_write_trailer` can run.
///   `venc` transitively. The mux queue is `Block` policy because SPEC.md §12 forbids
///          ever dropping audio, and an encoded video packet cannot be dropped either
///          -- discarding a P-frame after the fact breaks every frame that references
///          it. So a full mux queue blocks the venc thread, by design, and the venc
///          thread's exit is therefore gated on the same disk. Degradation happens
///          upstream instead, at the drop-oldest *encode* queue, which is where §12's
///          "on audio queue pressure, degrade video instead" actually lands.
///
/// Measured on a volume stalled 600 ms per write -- rung 6's own scale -- the 2 s
/// deadline fired on the venc thread, `stop` returned `INTERNAL_THREAD_JOIN_TIMEOUT`,
/// the encoder was never flushed and the trailer was never written. A *slow disk* thus
/// produced an unfinalized file: CLAUDE.md §1 inverted, with the ladder's gentlest rung
/// delivering the outcome rung 7 exists to prevent.
///
/// 30 s is SPEC.md §15.1's own number for exactly this operation -- "Requests time out
/// at 5 s (except `stop_record`, 30 s, since finalization is legitimately slow)" -- so
/// the two sections are reconciled here rather than left in tension. It is still
/// bounded: a thread that has not finished in 30 s escalates and logs exactly as
/// before, and for MKV the file remains playable to its last complete cluster
/// (SPEC.md §10.2).
///
/// The `watchdog` thread keeps the 2 s deadline. It is a pure observer, it waits on a
/// 250 ms condition variable, and one that has not noticed a shutdown flag in 2 s
/// really is wedged.
inline constexpr auto kFinalizeJoinTimeout = std::chrono::seconds{30};

/// Fulfils a promise however its scope is left, so a worker that exits by
/// exception still releases whoever is waiting on it. Without this, the one case
/// where the deadline matters most -- a thread dying unexpectedly -- is the case
/// where the waiter would sit out the full timeout for no reason.
class ScopedSignal {
public:
    explicit ScopedSignal(std::promise<void>& promise) noexcept : promise_(&promise) {}

    ~ScopedSignal() {
        // `set_value` throws `future_error` on a promise with no shared state or
        // one already satisfied. Neither is reachable here -- each run installs a
        // freshly constructed promise and exactly one ScopedSignal fulfils it --
        // but an exception escaping a destructor terminates the process, so the
        // one case that would is caught by type and reported. Not `catch(...)`,
        // and not an empty handler (CLAUDE.md §4).
        try {
            promise_->set_value();
        } catch (const std::future_error& e) {
            FC_LOG_ERROR(Subsystem::Internal, "worker completion signal failed; shutdown will wait out its deadline",
                         LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_INVARIANT_VIOLATED));
        }
    }

    ScopedSignal(const ScopedSignal&) = delete;
    ScopedSignal& operator=(const ScopedSignal&) = delete;
    ScopedSignal(ScopedSignal&&) = delete;
    ScopedSignal& operator=(ScopedSignal&&) = delete;

private:
    std::promise<void>* promise_;
};

/// Waits `kWorkerJoinTimeout` for a worker to leave its loop, then joins it.
///
/// Returns false when the deadline passed, having logged the escalation. The
/// thread is *detached* in that case: it is still running and still referencing
/// the owner's state, so joining would hang exactly as before and destroying that
/// state would be a use-after-free. The caller keeps the state alive deliberately
/// instead.
///
/// Hanging here would be the worse failure. For MKV an unfinalized file is still
/// playable up to its last complete cluster (SPEC.md §10.2), so abandoning a
/// wedged worker leaves the user with a recording; blocking forever leaves them
/// with a process that never exits and a file nobody closed.
///
/// `timeout` defaults to SPEC.md §12's 2 s. Pass `kFinalizeJoinTimeout` for the `mux`
/// thread's drain, which is legitimately slow -- see that constant and BUG-026.
[[nodiscard]] inline bool await_worker(
    std::future<void>& finished, std::thread& thread, const char* name, Subsystem subsystem,
    std::chrono::milliseconds timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kWorkerJoinTimeout)) {
    if (!thread.joinable()) {
        return true;
    }

    if (finished.valid() && finished.wait_for(timeout) == std::future_status::ready) {
        thread.join(); // signalled, so this returns immediately
        return true;
    }

    FC_LOG_ERROR(subsystem, "worker thread did not stop within the join timeout; abandoning it",
                 LogFields{}
                     .add("thread", name)
                     .add("timeout_ms", static_cast<std::int64_t>(timeout.count()))
                     .add_error(FcError::INTERNAL_THREAD_JOIN_TIMEOUT));
    thread.detach();
    return false;
}

} // namespace fc
