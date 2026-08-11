#pragma once

#include "core/capture/capture_frame.h"
#include "core/error/result.h"

#include <chrono>
#include <cstdint>
#include <string>

struct ID3D11Device;

namespace fc::capture {

/// Which backend produced a session. SPEC.md §4.2 / §4.3.
enum class Backend {
    /// Windows.Graphics.Capture. Primary: the only API that reliably captures
    /// across hybrid-GPU boundaries and is tear-free because DWM composites it.
    Wgc,
    /// DXGI Desktop Duplication. Fallback, and constrained: the device must be on
    /// the adapter that owns the output.
    Dda,
};

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;

/// What to capture. SPEC.md §4.1.
struct CaptureTarget {
    /// `HMONITOR` for display capture, as an opaque value.
    std::uintptr_t monitor = 0;
    /// `HWND` for window capture. Exactly one of these is set.
    std::uintptr_t window = 0;

    /// SPEC.md §16.4 / §4.2: drawn from config, not hard-coded.
    bool capture_cursor = true;

    [[nodiscard]] bool is_display() const noexcept {
        return monitor != 0 && window == 0;
    }

    [[nodiscard]] bool is_window() const noexcept {
        return window != 0;
    }
};

/// Platform-agnostic capture interface (SPEC.md §2.2 item 2).
///
/// Every v1 implementation is Win32/WinRT. The interface exists so a future Linux
/// backend is a new implementation rather than a rewrite -- but there is
/// deliberately no Linux code, no `#ifdef __linux__`, and no PipeWire scaffolding.
class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    IScreenCapture(const IScreenCapture&) = delete;
    IScreenCapture& operator=(const IScreenCapture&) = delete;
    IScreenCapture(IScreenCapture&&) = delete;
    IScreenCapture& operator=(IScreenCapture&&) = delete;

    /// Starts producing frames.
    ///
    /// `device` must already be on the adapter that owns the target's output. For
    /// DDA that is a hard requirement of the API; for WGC it avoids a
    /// cross-adapter copy on every frame.
    [[nodiscard]] virtual Result<void> start(ID3D11Device* device, const CaptureTarget& target) = 0;

    virtual void stop() = 0;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// Takes the next frame, waiting up to `timeout`.
    ///
    /// Returns `CAPTURE_FRAME_TIMEOUT` when nothing arrived in time. The caller
    /// owns the returned frame's texture reference until it calls `release`.
    [[nodiscard]] virtual Result<CaptureFrame> acquire(std::chrono::milliseconds timeout) = 0;

    /// Returns a frame's texture to the pool. Failing to call this starves the
    /// pool and stops capture (SPEC.md §20 row 9).
    virtual void release(const CaptureFrame& frame) = 0;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;

    /// Frames the backend dropped because the consumer was too slow. Surfaced in
    /// the 2 Hz stats event rather than logged per frame (SPEC.md §18).
    [[nodiscard]] virtual std::uint64_t dropped_frames() const noexcept = 0;

protected:
    IScreenCapture() = default;
};

} // namespace fc::capture
