#include "core/subsystem.h"

namespace fc {

std::string_view to_string(Subsystem subsystem) noexcept {
    switch (subsystem) {
    case Subsystem::None:
        return "none";
    case Subsystem::Capture:
        return "capture";
    case Subsystem::Gpu:
        return "gpu";
    case Subsystem::Audio:
        return "audio";
    case Subsystem::Encode:
        return "encode";
    case Subsystem::Mux:
        return "mux";
    case Subsystem::Io:
        return "io";
    case Subsystem::Ipc:
        return "ipc";
    case Subsystem::Internal:
        return "internal";
    case Subsystem::Clock:
        return "clock";
    case Subsystem::Color:
        return "color";
    case Subsystem::Config:
        return "config";
    case Subsystem::Pipeline:
        return "pipeline";
    case Subsystem::App:
        return "app";
    case Subsystem::Health:
        return "health";
    }
    return "unknown";
}

} // namespace fc
