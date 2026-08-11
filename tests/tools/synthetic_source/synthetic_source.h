#pragma once

#include "core/capture/i_screen_capture.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fc::test {

/// Deterministic D3D11 test pattern source (SPEC.md §20.1, CLAUDE.md §5).
///
/// Exists so integration tests are hermetic. A test that captures the real desktop
/// asserts things about whatever happened to be on screen, which makes it both
/// flaky and weak -- it cannot check that frame N contains frame N's content,
/// because it has no idea what that should be.
///
/// Each frame carries:
///   * a **frame-index barcode** in the top-left, so a decoded frame can be
///     matched back to the frame that produced it (SPEC.md §20 row 5);
///   * **75% SMPTE colour bars**, giving known values at known positions;
///   * a **moving bar**, so consecutive frames genuinely differ and a stuck
///     pipeline is distinguishable from a static one.
///   * optionally a **white flash**, on frames whose index is a multiple of
///     `flash_interval_frames`. That is SPEC.md §20 row 4's marker: the flash is
///     emitted on the same instant as the 1 kHz beep, so a decoded file carries
///     one event visible in both streams. The barcode row is left untouched, so a
///     flash frame still identifies *which* frame it is rather than only that a
///     flash happened.
///
/// The 1 kHz tone §20.1 also mentions lives in `synthetic_audio.h`.
///
/// Implements `IScreenCapture`, so it drops into any pipeline in place of WGC or
/// DDA.
class SyntheticSource final : public capture::IScreenCapture {
public:
    struct Settings {
        int width = 1920;
        int height = 1080;
        /// Frames per second the generator paces itself at. Frames are produced on
        /// demand rather than on a timer, so this only sets the reported timestamps.
        int fps = 60;
        /// Stop after this many frames, then report timeouts. 0 = unlimited.
        std::uint32_t frame_limit = 0;
        /// Flash white on every Nth frame. 0 = never. Set to `fps` for a flash on
        /// each exact second, which is where the beep is.
        std::uint32_t flash_interval_frames = 0;

        /// Pre-render this many distinct frames once, then cycle them. 0 renders a
        /// fresh frame per `acquire`, which is the default and gives every frame a
        /// unique barcode.
        ///
        /// **This is the difference between a feeder that can hold 60 fps and one
        /// that cannot.** Rendering costs a 1080p CPU pattern fill plus a `Map` and a
        /// row-by-row copy on every call -- around 2 million pixels touched twice --
        /// and on an unoptimised build under load that falls well short of a 16.67 ms
        /// budget. Tests that pace themselves at a real frame rate then silently feed
        /// slower than they claim, which is what made `test_slow_disk`'s backpressure
        /// build-dependent (BUG-027) and what left row 6's ten-minute run feeding at
        /// 57.2 fps rather than 60.0.
        ///
        /// The *pixels* are memoised process-wide, so only the first `start` in a test
        /// binary pays to render them. That matters because `start` sits on SPEC.md §5.4
        /// step 8's path, where a session rebuild restarts capture inside a 350 ms
        /// budget, and eight 1080p fills measured 458-501 ms on an unoptimised build --
        /// four times the budget, from the fixture rather than the product (BUG-032).
        ///
        /// The cost is that barcodes repeat every `prerendered_frames` frames, so any
        /// test asserting frame *identity* -- SPEC.md §20 row 5 -- must leave this at
        /// 0. `CaptureFrame::sequence` and the timestamps stay strictly monotonic
        /// either way; only the picture content cycles.
        std::uint32_t prerendered_frames = 0;

        /// Make `acquire` wait until the frame is due, so the source produces at `fps`
        /// in wall-clock time rather than as fast as it is asked.
        ///
        /// Off by default, because every test that drives the pipeline directly wants
        /// frames on demand and paces itself. It matters for anything driving
        /// `RecordingSession`, whose capture loop calls `acquire` in a tight loop and
        /// relies on the backend to pace it -- WGC and DDA both block until a frame
        /// exists, so a source that returns instantly floods the D3D command queue
        /// until the capture thread blocks inside `CopyResource` and stops noticing
        /// anything else. Measured that way: an injected device loss was never acted on
        /// and the session took 30 s to shut down.
        bool pace_to_real_time = false;
    };

    SyntheticSource();
    ~SyntheticSource() override;

    // Matches the real backends it substitutes for: owns D3D resources, not
    // copyable, not movable.
    SyntheticSource(const SyntheticSource&) = delete;
    SyntheticSource& operator=(const SyntheticSource&) = delete;
    SyntheticSource(SyntheticSource&&) = delete;
    SyntheticSource& operator=(SyntheticSource&&) = delete;

    /// `target` is ignored: the pattern is synthesised, not captured.
    [[nodiscard]] Result<void> start(ID3D11Device* device, const capture::CaptureTarget& target) override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] Result<capture::CaptureFrame> acquire(std::chrono::milliseconds timeout) override;
    void release(const capture::CaptureFrame& frame) override;

    [[nodiscard]] capture::Backend backend() const noexcept override {
        return capture::Backend::Wgc;
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept override {
        return 0;
    }

    [[nodiscard]] Result<void> configure(const Settings& settings);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Pattern generation and verification, usable without a GPU.
// ---------------------------------------------------------------------------

/// Bits in the frame-index barcode. 24 bits covers 4.6 hours at 60 fps before it
/// wraps, which is longer than the 4-hour soak in SPEC.md §20.1.
inline constexpr int kBarcodeBits = 24;
/// Edge length of one barcode cell, in pixels.
inline constexpr int kBarcodeCell = 16;
/// Narrowest source the barcode fits in.
inline constexpr int kMinimumWidth = kBarcodeBits * kBarcodeCell;

/// Renders frame `index` as BGRA8, tightly packed, `width * height * 4` bytes.
///
/// `flash` replaces the bars and the moving bar with white, leaving the barcode
/// alone. Mean luma is what identifies it after decoding, and the gap between a
/// flash frame and a bars frame is far wider than any codec artefact.
[[nodiscard]] std::vector<std::uint8_t> render_bgra(int width, int height, std::uint32_t index, bool flash = false);

/// Reads the barcode out of an NV12 luma plane.
///
/// Works on the *decoded* frame, which is the point: it proves the index survived
/// conversion, encoding, muxing and decoding, rather than proving the generator
/// can count.
///
/// Returns nothing when the barcode cells are not cleanly black or white, which is
/// what a corrupted or torn frame looks like.
[[nodiscard]] std::optional<std::uint32_t> decode_barcode_from_luma(std::span<const std::uint8_t> luma, int width,
                                                                    int height);

/// The 75% SMPTE bar colours, left to right, as BGRA.
struct BarColour {
    std::uint8_t b;
    std::uint8_t g;
    std::uint8_t r;
    const char* name;
};

[[nodiscard]] std::span<const BarColour> smpte_bars() noexcept;

/// Y coordinate at which the bars start, i.e. below the barcode.
[[nodiscard]] constexpr int bars_top() noexcept {
    return kBarcodeCell;
}

/// Mean luma of the region below the barcode, over an NV12 luma plane. A flash
/// frame sits near 235 (limited-range white) and a bars frame near 150, so any
/// threshold between them separates the two without tuning.
[[nodiscard]] double mean_luma_below_barcode(std::span<const std::uint8_t> luma, int width, int height);

/// Luma above which a decoded frame counts as the flash.
inline constexpr double kFlashLumaThreshold = 200.0;

} // namespace fc::test
