#include "core/util/com_apartment.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <combaseapi.h>

#include <atomic>

namespace fc {
namespace {

std::atomic<bool> g_held{false};

/// Acquired once and never released; see the header. Kept at namespace scope rather
/// than discarded so a debugger and `process_mta_held` can both see the outcome.
CO_MTA_USAGE_COOKIE g_cookie = nullptr;

bool acquire() {
    // `FC_HR_LOG` rather than `FC_HR`: there is nothing to return a `Result` to, and this
    // must not abort the caller. The macro logs the expression, file, line and the
    // decoded HRESULT; the line below adds what it costs, because a crash whose cause was
    // announced an hour earlier is a much cheaper crash than one that was not.
    if (!FC_HR_LOG(CoIncrementMTAUsage(&g_cookie))) {
        // Not fatal: this leaves the process on the behaviour it had before BUG-037 was
        // fixed, where the first recording works and a later one may fault.
        FC_LOG_ERROR(Subsystem::App, "the process MTA could not be pinned; COM state will not survive a recording",
                     LogFields{}.add_error(FcError::INTERNAL_UNKNOWN));
        return false;
    }

    FC_LOG_DEBUG(Subsystem::App, "process MTA pinned for the process lifetime", LogFields{});
    return true;
}

} // namespace

bool hold_process_mta() {
    // Magic static: the initialiser runs exactly once, and every other caller on every
    // other thread blocks until it has. That is the whole of the synchronisation this
    // needs -- there is one cookie and it is acquired before anyone can observe it.
    static const bool kAcquired = acquire();
    g_held.store(kAcquired, std::memory_order_release);
    return kAcquired;
}

bool process_mta_held() noexcept {
    return g_held.load(std::memory_order_acquire);
}

} // namespace fc
