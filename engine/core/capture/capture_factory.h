#pragma once

#include "core/capture/i_screen_capture.h"
#include "core/config/config_schema.h"
#include "core/error/result.h"

#include <memory>
#include <string>

namespace fc::capture {

/// Facts the backend choice depends on, gathered once so the policy itself stays
/// a pure function.
struct BackendAvailability {
    /// `GraphicsCaptureSession::IsSupported()`. Probed, never inferred from the OS
    /// build (SPEC.md §4.2).
    bool wgc_supported = false;
    /// DDA is present on every supported Windows version, but it is constrained:
    /// display targets only, and the device must own the output.
    bool dda_supported = true;
};

struct BackendSelection {
    Backend backend = Backend::Wgc;
    /// Why, for the log and the GUI's status panel.
    std::string rationale;
};

/// Applies the `advanced.capture_backend` preference against what is available.
///
/// Pure, so every combination is exercised on the CPU tier -- including the ones
/// this machine cannot produce, such as a system without WGC.
///
/// Rules, in order:
///   1. A window target requires WGC. DDA duplicates outputs, not windows, so an
///      explicit `dda` preference with a window target is an error rather than a
///      silent fallback -- the user asked for something that cannot work.
///   2. An explicit preference is honoured, or fails if unavailable. "Force WGC"
///      quietly falling back to DDA would make the setting useless for the
///      diagnosis it exists for.
///   3. `auto` prefers WGC: it is the only backend that captures reliably across
///      hybrid-GPU boundaries, handles fullscreen-exclusive apps, and is tear-free
///      because DWM composites the frames (SPEC.md §4.2, §7.4).
[[nodiscard]] Result<BackendSelection> select_backend(config::CaptureBackend preference, const CaptureTarget& target,
                                                      const BackendAvailability& availability);

/// Probes what is actually available on this machine.
[[nodiscard]] BackendAvailability probe_availability();

/// Selects a backend and constructs it. Does not start it.
[[nodiscard]] Result<std::unique_ptr<IScreenCapture>> create_capture(config::CaptureBackend preference,
                                                                     const CaptureTarget& target);

} // namespace fc::capture
