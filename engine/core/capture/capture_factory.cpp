#include "core/capture/capture_factory.h"

#include "core/capture/dda/dda_capture.h"
#include "core/capture/wgc/wgc_capture.h"
#include "core/logging/logger.h"

namespace fc::capture {

Result<BackendSelection> select_backend(config::CaptureBackend preference, const CaptureTarget& target,
                                        const BackendAvailability& availability) {
    if (!target.is_display() && !target.is_window()) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }

    // --- Rule 1: a window can only be captured by WGC -------------------------
    if (target.is_window()) {
        if (preference == config::CaptureBackend::Dda) {
            // Not a silent fallback: the user asked for a combination that cannot
            // exist, and should be told so.
            FC_LOG_ERROR(
                Subsystem::Capture, "DDA cannot capture a window",
                LogFields{}.add("hint", "window capture requires WGC").add_error(FcError::INTERNAL_NOT_IMPLEMENTED));
            return FcError::INTERNAL_NOT_IMPLEMENTED;
        }
        if (!availability.wgc_supported) {
            return FcError::WGC_UNSUPPORTED;
        }
        return BackendSelection{Backend::Wgc, "window capture requires WGC"};
    }

    // --- Rule 2: an explicit preference is honoured or it fails ---------------
    if (preference == config::CaptureBackend::Wgc) {
        if (!availability.wgc_supported) {
            FC_LOG_ERROR(Subsystem::Capture, "WGC was requested but is unavailable",
                         LogFields{}.add_error(FcError::WGC_UNSUPPORTED));
            return FcError::WGC_UNSUPPORTED;
        }
        return BackendSelection{Backend::Wgc, "WGC requested explicitly"};
    }
    if (preference == config::CaptureBackend::Dda) {
        if (!availability.dda_supported) {
            return FcError::DDA_UNSUPPORTED;
        }
        return BackendSelection{Backend::Dda, "DDA requested explicitly"};
    }

    // --- Rule 3: auto ---------------------------------------------------------
    if (availability.wgc_supported) {
        return BackendSelection{Backend::Wgc, "auto: WGC captures across hybrid-GPU boundaries and is tear-free"};
    }
    if (availability.dda_supported) {
        return BackendSelection{Backend::Dda, "auto: WGC unavailable on this build, falling back to DDA"};
    }

    FC_LOG_ERROR(Subsystem::Capture, "no capture backend is available",
                 LogFields{}.add_error(FcError::CAPTURE_BACKEND_UNAVAILABLE));
    return FcError::CAPTURE_BACKEND_UNAVAILABLE;
}

BackendAvailability probe_availability() {
    BackendAvailability availability;
    availability.wgc_supported = wgc::is_supported();
    // DDA ships with every supported Windows version. Whether a *given* output can
    // be duplicated is decided by DdaCapture::start, which is where the adapter
    // affinity constraint lives.
    availability.dda_supported = true;
    return availability;
}

Result<std::unique_ptr<IScreenCapture>> create_capture(config::CaptureBackend preference, const CaptureTarget& target) {
    const auto selection = select_backend(preference, target, probe_availability());
    if (!selection.has_value()) {
        return selection.error();
    }

    FC_LOG_INFO(Subsystem::Capture, "capture backend selected",
                LogFields{}
                    .add("backend", to_string(selection.value().backend))
                    .add("preference", config::to_string(preference))
                    .add("target", target.is_window() ? "window" : "display")
                    .add("rationale", selection.value().rationale));

    switch (selection.value().backend) {
    case Backend::Wgc:
        return std::unique_ptr<IScreenCapture>{std::make_unique<wgc::WgcCapture>()};
    case Backend::Dda:
        return std::unique_ptr<IScreenCapture>{std::make_unique<dda::DdaCapture>()};
    }
    return FcError::CAPTURE_BACKEND_UNAVAILABLE;
}

} // namespace fc::capture
