#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fc::gpu {

/// Stable identity for an adapter across a session: the DXGI `LUID`, packed.
///
/// A LUID is only meaningful for the lifetime of the boot, which is exactly the
/// lifetime we need. Adapter *index* is not an identity -- it renumbers when an
/// eGPU is plugged in or a driver restarts, which is precisely when we care.
struct AdapterId {
    std::int64_t value = 0;

    [[nodiscard]] bool valid() const noexcept {
        return value != 0;
    }

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const AdapterId&, const AdapterId&) = default;
};

/// SPEC.md §5.1 step 2.
enum class AdapterClass {
    /// `DXGI_ADAPTER_FLAG_SOFTWARE`. Excluded from encode selection; permitted
    /// only as a last-ditch capture device.
    Software,
    /// Shares system memory with the CPU. On a MUX-less laptop this is usually the
    /// adapter that owns the panel.
    Integrated,
    Discrete,
};

[[nodiscard]] std::string_view to_string(AdapterClass value) noexcept;

/// PCI vendor ids we name. Anything else is reported numerically rather than
/// guessed at.
inline constexpr std::uint32_t kVendorAmd = 0x1002;
inline constexpr std::uint32_t kVendorNvidia = 0x10DE;
inline constexpr std::uint32_t kVendorIntel = 0x8086;
inline constexpr std::uint32_t kVendorMicrosoft = 0x1414;

[[nodiscard]] std::string_view vendor_name(std::uint32_t vendor_id) noexcept;

struct OutputInfo {
    /// e.g. `\\.\DISPLAY1`.
    std::string device_name;
    /// `HMONITOR` as an opaque value, so this header does not need <windows.h>.
    std::uintptr_t monitor = 0;
    std::int32_t desktop_left = 0;
    std::int32_t desktop_top = 0;
    std::int32_t desktop_right = 0;
    std::int32_t desktop_bottom = 0;
    bool attached_to_desktop = false;

    [[nodiscard]] std::int32_t width() const noexcept {
        return desktop_right - desktop_left;
    }

    [[nodiscard]] std::int32_t height() const noexcept {
        return desktop_bottom - desktop_top;
    }
};

/// Result of SPEC.md §5.1 step 5 -- an *actually attempted* encoder session, never
/// a vendor-id heuristic or a driver-version table.
struct EncoderCapability {
    bool h264 = false;
    /// libavcodec encoder that succeeded, e.g. "h264_amf" or "h264_nvenc".
    std::string encoder_name;
    /// Why it failed, when it did. Kept because "no encoder" and "encoder busy" are
    /// very different user-facing situations.
    std::string detail;
    /// D3D11 bind flags the adapter accepted for an NV12 encoder-input pool, with
    /// the planar UAVs verified creatable when `D3D11_BIND_UNORDERED_ACCESS` is
    /// among them. Probed rather than assumed -- see BUG-001 and its follow-up.
    ///
    /// `D3D11_BIND_DECODER` is always present: a texture *array* of a video format
    /// is rejected without it. Test `nv12_pool_is_uav_writable()` to find out
    /// whether the conversion shader can write the pool directly or needs a copy.
    std::uint32_t nv12_pool_bind_flags = 0;

    /// True when the conversion shader can write NV12 straight into a pool slice,
    /// making the capture-to-encode path zero-copy.
    [[nodiscard]] bool nv12_pool_is_uav_writable() const noexcept {
        constexpr std::uint32_t kBindUnorderedAccess = 0x80; // D3D11_BIND_UNORDERED_ACCESS
        return (nv12_pool_bind_flags & kBindUnorderedAccess) != 0;
    }

    bool probed = false;
};

struct AdapterInfo {
    AdapterId id;
    std::string description;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t dedicated_video_memory = 0;
    std::uint64_t shared_system_memory = 0;
    AdapterClass adapter_class = AdapterClass::Software;
    /// User-mode driver version, from `CheckInterfaceSupport`. Part of the cache
    /// key for the capability probe, so a driver update re-probes.
    std::string driver_version;
    std::vector<OutputInfo> outputs;
    EncoderCapability encode;

    /// SPEC.md §5.1 step 3: "An adapter with zero outputs is not driving the
    /// display ... It is still a valid encode target."
    [[nodiscard]] bool drives_display() const noexcept {
        return !outputs.empty();
    }

    [[nodiscard]] bool can_encode() const noexcept {
        return encode.h264 && adapter_class != AdapterClass::Software;
    }
};

/// The adapter set as discovered at one point in time.
struct Topology {
    std::vector<AdapterInfo> adapters;

    [[nodiscard]] const AdapterInfo* find(AdapterId id) const noexcept;

    /// The adapter whose output contains `monitor`. Null when the mapping could not
    /// be resolved -- which must never be silently treated as "adapter 0", because
    /// that is the all-black-video bug (SPEC.md §5.1 step 4, §20 row 1).
    [[nodiscard]] const AdapterInfo* owner_of_monitor(std::uintptr_t monitor) const noexcept;

    /// The adapter owning the primary output, if any.
    [[nodiscard]] const AdapterInfo* primary_display_adapter() const noexcept;

    [[nodiscard]] bool empty() const noexcept {
        return adapters.empty();
    }
};

} // namespace fc::gpu
