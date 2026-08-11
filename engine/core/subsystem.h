#pragma once

#include <string_view>

namespace fc {

/// Shared vocabulary for the `subsystem` field that SPEC.md §18 requires on every
/// log line, and for the subsystem grouping of `FcError` codes (SPEC.md §19).
///
/// The eight subsystems that own an error-code group appear first, numbered to
/// match the thousands digit of their codes, so `subsystem_of()` is arithmetic
/// rather than a table. The remainder have no error group of their own and log
/// under a subsystem that does.
enum class Subsystem {
    None = 0,
    Capture = 1,  ///< 1xxx
    Gpu = 2,      ///< 2xxx
    Audio = 3,    ///< 3xxx
    Encode = 4,   ///< 4xxx
    Mux = 5,      ///< 5xxx
    Io = 6,       ///< 6xxx
    Ipc = 7,      ///< 7xxx
    Internal = 9, ///< 9xxx

    // No dedicated error-code group; these are log-only origins.
    Clock = 20,
    Color = 21,
    Config = 22,
    Pipeline = 23,
    App = 24,
    Health = 25, ///< The SPEC.md §13 degradation ladder and the watchdog's metrics.
};

/// Lowercase, stable, machine-greppable. Never localise these -- log scrapers and
/// the acceptance suite match on them.
[[nodiscard]] std::string_view to_string(Subsystem subsystem) noexcept;

} // namespace fc
