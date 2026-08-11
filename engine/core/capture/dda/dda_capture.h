#pragma once

#include "core/capture/i_screen_capture.h"

#include <memory>

namespace fc::capture::dda {

/// DXGI Desktop Duplication backend -- the *fallback* (SPEC.md §4.3).
///
/// It is the fallback rather than the primary because of one constraint:
///
/// > The `ID3D11Device` used for `IDXGIOutput1::DuplicateOutput` **must be created
/// > on the exact `IDXGIAdapter` that owns that `IDXGIOutput`.**
///
/// Violating it returns `DXGI_ERROR_UNSUPPORTED`, and on hybrid laptops that is the
/// single most common cause of an entirely black recording. This class refuses to
/// start rather than duplicating on the wrong device, and reports
/// `DDA_ADAPTER_AFFINITY` so the cause is named rather than guessed at.
///
/// DDA also cannot capture a single window (display only) and can tear on
/// fullscreen-exclusive targets, which is why WGC is preferred when available.
class DdaCapture final : public IScreenCapture {
public:
    DdaCapture();
    ~DdaCapture() override;

    // A capture backend owns an IDXGIOutputDuplication and the device it was
    // created against. Neither can be meaningfully duplicated, and moving one
    // out from under a running acquire loop would be worse. Hold it by
    // unique_ptr if you need to relocate ownership.
    DdaCapture(const DdaCapture&) = delete;
    DdaCapture& operator=(const DdaCapture&) = delete;
    DdaCapture(DdaCapture&&) = delete;
    DdaCapture& operator=(DdaCapture&&) = delete;

    /// `device` must be on the adapter owning `target.monitor`. Window targets are
    /// rejected: DDA duplicates outputs, not windows.
    [[nodiscard]] Result<void> start(ID3D11Device* device, const CaptureTarget& target) override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] Result<CaptureFrame> acquire(std::chrono::milliseconds timeout) override;
    void release(const CaptureFrame& frame) override;

    [[nodiscard]] Backend backend() const noexcept override {
        return Backend::Dda;
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept override;

    /// Frames emitted because the desktop did not change (SPEC.md §4.3:
    /// `DXGI_ERROR_WAIT_TIMEOUT` means "no screen change", and must produce a
    /// duplicate with an advanced PTS rather than a stall or a gap).
    [[nodiscard]] std::uint64_t duplicate_frames() const noexcept;

    /// Times the duplication session was rebuilt after `DXGI_ERROR_ACCESS_LOST`.
    /// Fires on UAC prompts, secure-desktop transitions, resolution changes and MUX
    /// switches, so a non-zero count is normal rather than alarming.
    [[nodiscard]] std::uint64_t access_lost_recoveries() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::capture::dda
