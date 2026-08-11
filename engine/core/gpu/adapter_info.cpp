#include "core/gpu/adapter_info.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace fc::gpu {

std::string AdapterId::to_string() const {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "0x%016llX", static_cast<unsigned long long>(value));
    return std::string{buffer.data()};
}

std::string_view to_string(AdapterClass value) noexcept {
    switch (value) {
    case AdapterClass::Software:
        return "software";
    case AdapterClass::Integrated:
        return "integrated";
    case AdapterClass::Discrete:
        return "discrete";
    }
    return "unknown";
}

std::string_view vendor_name(std::uint32_t vendor_id) noexcept {
    switch (vendor_id) {
    case kVendorAmd:
        return "AMD";
    case kVendorNvidia:
        return "NVIDIA";
    case kVendorIntel:
        return "Intel";
    case kVendorMicrosoft:
        return "Microsoft";
    default:
        return "unknown";
    }
}

const AdapterInfo* Topology::find(AdapterId id) const noexcept {
    const auto it = std::ranges::find(adapters, id, &AdapterInfo::id);
    return it != adapters.end() ? &*it : nullptr;
}

const AdapterInfo* Topology::owner_of_monitor(std::uintptr_t monitor) const noexcept {
    if (monitor == 0) {
        return nullptr;
    }
    for (const AdapterInfo& adapter : adapters) {
        for (const OutputInfo& output : adapter.outputs) {
            if (output.monitor == monitor) {
                return &adapter;
            }
        }
    }
    // Deliberately null rather than a fallback. Guessing here is how a capture
    // device ends up on the wrong adapter and DDA returns black with S_OK.
    return nullptr;
}

const AdapterInfo* Topology::primary_display_adapter() const noexcept {
    for (const AdapterInfo& adapter : adapters) {
        for (const OutputInfo& output : adapter.outputs) {
            // The primary output is the one whose desktop rectangle starts at the
            // origin; Windows guarantees exactly one such output.
            if (output.attached_to_desktop && output.desktop_left == 0 && output.desktop_top == 0) {
                return &adapter;
            }
        }
    }
    return nullptr;
}

} // namespace fc::gpu
