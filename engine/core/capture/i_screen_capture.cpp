#include "core/capture/i_screen_capture.h"

namespace fc::capture {

std::string_view to_string(Backend backend) noexcept {
    switch (backend) {
    case Backend::Wgc:
        return "wgc";
    case Backend::Dda:
        return "dda";
    }
    return "unknown";
}

} // namespace fc::capture
