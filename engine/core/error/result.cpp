#include "core/error/result.h"

#include "core/logging/logger.h"

#include <cstdio>
#include <exception>

namespace fc {

void result_misuse(std::string_view what, FcError error) noexcept {
    // Reaching here means a Result was read in the wrong state, which is a
    // programming defect rather than a runtime failure. There is no correct value
    // to return, so this deliberately does not: it records what happened and
    // terminates, which the crash handler converts into a minidump.
    FC_LOG_CRITICAL(Subsystem::Internal, what, LogFields{}.add_error(error));
    log::flush();

    // The logger may not be running yet -- Result is used by code that executes
    // before init() -- so stderr is the backstop. Without this, an early misuse
    // would terminate with no explanation at all.
    std::fprintf(stderr, "FATAL: %.*s (error=%.*s code=%d)\n", static_cast<int>(what.size()), what.data(),
                 static_cast<int>(error_name(error).size()), error_name(error).data(), error_code(error));
    std::fflush(stderr);

    std::terminate();
}

} // namespace fc
