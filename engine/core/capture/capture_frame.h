#pragma once

#include "core/gpu/adapter_info.h"

#include <cstdint>

struct ID3D11Texture2D;

namespace fc::capture {

/// A rectangle in the source's coordinate space.
struct ContentRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    [[nodiscard]] std::int32_t width() const noexcept {
        return right - left;
    }

    [[nodiscard]] std::int32_t height() const noexcept {
        return bottom - top;
    }

    friend bool operator==(const ContentRect&, const ContentRect&) = default;
};

/// SPEC.md §4.4: "Every captured texture carries an immutable CaptureFrame".
///
/// Immutable by convention: nothing downstream may modify a frame in place. The
/// texture is reference-counted and owned by the capture backend's pool; the
/// consumer must release its reference promptly or it starves the pool.
struct CaptureFrame {
    /// Non-owning. The backend hands out an AddRef'd pointer via `acquire()`, and
    /// `release()` gives it back.
    ID3D11Texture2D* texture = nullptr;

    /// QPC, converted to nanoseconds once (SPEC.md §7.1: QPC is the only clock).
    std::uint64_t qpc_ns = 0;

    /// Monotonic per session, so a gap is detectable without comparing timestamps.
    std::uint32_t sequence = 0;

    /// Which adapter's memory this texture lives in. Carried so a downstream stage
    /// can assert it is not about to use a texture from the wrong device.
    gpu::AdapterId adapter;

    /// DXGI format as reported by the source. FP16 here means system HDR is on and
    /// the tone-map branch is required (SPEC.md §4.2).
    std::uint32_t dxgi_format = 0;

    /// The live content area. A change is not silently rescaled -- it triggers the
    /// resize policy in SPEC.md §14.3.
    ContentRect content;

    /// True when the backend synthesised this frame because the source produced
    /// nothing (DDA's WAIT_TIMEOUT means "no screen change"). The pacer still needs
    /// it, with a correctly advanced PTS rather than a gap (SPEC.md §4.3).
    bool duplicated = false;

    [[nodiscard]] bool valid() const noexcept {
        return texture != nullptr;
    }
};

} // namespace fc::capture
