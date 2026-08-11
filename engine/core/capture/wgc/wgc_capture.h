#pragma once

#include "core/capture/i_screen_capture.h"

#include <memory>

namespace fc::capture::wgc {

/// True when Windows.Graphics.Capture is present and usable.
///
/// Probes `GraphicsCaptureSession::IsSupported()` rather than checking the OS
/// build number, per SPEC.md §4.2's guidance on capability probing.
[[nodiscard]] bool is_supported();

/// Windows.Graphics.Capture backend -- the primary path (SPEC.md §4.2).
///
/// Chosen over DDA because it is the only API that reliably captures across
/// hybrid-GPU boundaries, handles fullscreen-exclusive apps, supports per-window
/// capture, and is inherently tear-free (DWM composites the frames).
///
/// Threading: WinRT is initialised in MTA on the internal capture thread, and the
/// WinRT objects are touched from that thread only. The frame pool is created with
/// `CreateFreeThreaded` -- the non-free-threaded variant needs a `DispatcherQueue`
/// and is a common source of "no frames ever arrive".
class WgcCapture final : public IScreenCapture {
public:
    WgcCapture();
    ~WgcCapture() override;

    // Owns a running MTA thread and the WinRT objects bound to it. Copying or
    // moving either would detach them from the thread that is allowed to touch
    // them. Hold it by unique_ptr if you need to relocate ownership.
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;
    WgcCapture(WgcCapture&&) = delete;
    WgcCapture& operator=(WgcCapture&&) = delete;

    [[nodiscard]] Result<void> start(ID3D11Device* device, const CaptureTarget& target) override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] Result<CaptureFrame> acquire(std::chrono::milliseconds timeout) override;
    void release(const CaptureFrame& frame) override;

    [[nodiscard]] Backend backend() const noexcept override {
        return Backend::Wgc;
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::capture::wgc
