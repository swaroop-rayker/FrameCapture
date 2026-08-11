#pragma once

// A test-owned pattern presented to a real output (BUG-033).
//
// CLAUDE.md §5 says tests use the synthetic source and never the real desktop,
// and for most of the tree `SyntheticSource` is how that rule is kept. It cannot
// be here: the subject of the DDA and WGC cases *is* duplication of a real
// `IDXGIOutput`, which a synthetic `IScreenCapture` bypasses by construction.
// Those tests therefore captured whatever happened to be on screen, and reported
// its state as a verdict on the code -- five of them failed a Release GPU tier on
// a quiet machine and passed minutes later with a window animating in front of
// them.
//
// The way to keep the rule with a real backend is for the test to *own what is on
// the output*. This fixture does two things, and both matter:
//
//   1. It **covers** the target output with a topmost window, so ambient content
//      is occluded and cannot reach the capture. What the backend sees is what
//      this class drew, not what the desktop happened to be doing.
//   2. It **changes** that window at a stated rate, so frame delivery is a
//      property of the test rather than of the machine. WGC composites -- and DDA
//      reports a present -- only when the screen changes; a static desktop
//      produces nothing, however long you wait, which is why neither retrying nor
//      lengthening a deadline fixes anything.
//
// `fps == 0` is the deliberate opposite and is just as much a controlled input:
// paint once and hold. That makes "the desktop is idle" a fact the test
// established rather than a condition it hoped for, which is what SPEC.md §4.3's
// duplicate-frame case needs.
//
// GPU tier only -- it puts a window on the user's screen for the duration of the
// test.

#include "core/error/result.h"

#include <cstdint>
#include <memory>
#include <span>

namespace fc::test {

/// Mean luma of the two strips `ScreenAnimator` paints at the extremes of the
/// range, read off a captured NV12 luma plane.
///
/// This is the check that the frames under assertion really came from the
/// fixture. Without it a test still *passes* on ambient content if the animator
/// silently landed on the wrong output -- which is the failure mode this whole
/// class exists to remove, arriving one level up.
struct PatternSignature {
    double left_mean = 0.0;
    double right_mean = 0.0;

    /// BT.709 limited range puts black at Y=16 and white at Y=235; full range at
    /// 0 and 255. These clear both encodings by a wide margin, so the check does
    /// not depend on which the converter is configured for.
    static constexpr double kDarkMax = 48.0;
    static constexpr double kBrightMin = 170.0;

    [[nodiscard]] bool present() const noexcept {
        return left_mean < kDarkMax && right_mean > kBrightMin;
    }
};

/// Reads the signature strips out of an NV12 luma plane of `width` x `height`.
///
/// Both strips are taken from the top third of the frame, which the moving block
/// never enters, so a frame's verdict does not depend on where the block is.
[[nodiscard]] PatternSignature pattern_signature(std::span<const std::uint8_t> luma, int width, int height);

class ScreenAnimator {
public:
    struct Settings {
        /// `HMONITOR` of the output to cover, as the capture tests carry it.
        std::uintptr_t monitor = 0;

        /// Rate at which the picture changes, in Hz.
        ///
        /// **0 means paint once and hold**, which is how a test asks for a
        /// provably static output rather than merely an unattended one.
        int fps = 60;
    };

    ScreenAnimator();
    ~ScreenAnimator();

    // Owns a window and GDI objects with thread affinity.
    ScreenAnimator(const ScreenAnimator&) = delete;
    ScreenAnimator& operator=(const ScreenAnimator&) = delete;
    ScreenAnimator(ScreenAnimator&&) = delete;
    ScreenAnimator& operator=(ScreenAnimator&&) = delete;

    /// Covers `settings.monitor` and starts painting.
    ///
    /// Returns only once the window exists, has been verified to be *on the
    /// monitor that was asked for*, and has been composited at least once --
    /// otherwise the first frames a test captures are of the desktop underneath.
    ///
    /// `CAPTURE_TARGET_NOT_FOUND` if the monitor is gone or the window landed
    /// somewhere else; `CAPTURE_INIT_FAILED` for a window or GDI failure.
    [[nodiscard]] Result<void> start(const Settings& settings);

    void stop();

    [[nodiscard]] bool running() const noexcept;

    /// Repaints issued since `start`. A test that reports a capture rate should
    /// report this next to it: the two together say whether a shortfall was in
    /// the backend or in the thing feeding it.
    [[nodiscard]] std::uint64_t repaints() const noexcept;

    /// Size of the covered output, in physical pixels.
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::test
