// The gate for M9.6's overlay: does capture exclusion actually work here? (§20 row 19)
//
// GPU TIER.
//
// M9.6 puts a floating pill and a stack of toasts on screen *while the screen is being
// recorded*. Two ways that can go wrong, and the second is the one that costs a user
// their recording:
//
//   1. the overlay appears in the file;
//   2. a **black rectangle** appears in the file where the overlay was.
//
// (2) is not hypothetical and it is not exotic. `WDA_MONITOR` (0x01) is the older
// display-affinity flag, it also hides a window from captures, and the way it does so is
// by painting black into every capture surface. It is one hex digit from
// `WDA_EXCLUDEFROMCAPTURE` (0x11) and it is what a search result from before 2020 hands
// you. So a test that only asserted "the overlay is not visible" would pass on the
// defect it was written to prevent. **Both halves are asserted here, on both capture
// backends.**
//
// **Why this is written before any overlay UI exists.** If DDA does not honour the
// affinity on this hardware, the pill and the toasts need a different design -- and
// finding that out after they are built is the expensive order. This file is the
// question, asked first.
//
// **Owning what is on screen.** CLAUDE.md §5 forbids testing against the real desktop,
// and `SyntheticSource` cannot help here: the subject is what a real capture backend
// does with a real composited desktop, which a synthetic `IScreenCapture` bypasses by
// construction. `ScreenAnimator` is the fixture for exactly this -- it covers the output
// with a pattern the test owns, so "the pattern is intact under the overlay's rect" is a
// statement about the code and not about what happened to be on screen.
//
// The overlay under test is a plain Win32 window rather than the Qt one. What is being
// measured is a Win32 behaviour, and putting PySide6 in the path would make a failure
// ambiguous between the API and the binding. The Qt-side half -- that the affinity is
// applied, survives handle recreation, and reaches popups -- is
// `gui/tests/test_overlay_exclusion.py`.

#include "core/capture/dda/dda_capture.h"
#include "core/capture/source_resolver.h"
#include "core/capture/wgc/wgc_capture.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"

#include "adapter_device.h"
#include "screen_animator.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using fc::capture::CaptureTarget;
using fc::capture::DisplaySource;
using fc::capture::dda::DdaCapture;
using fc::capture::wgc::WgcCapture;
using Microsoft::WRL::ComPtr;

// --- the affinity constants, spelled out here rather than trusted from a header ------
//
// `winuser.h` defines all three. They are restated because this file's entire subject is
// telling them apart, and a test that imported the name it is asserting about would not
// notice a header that defined it wrongly.

constexpr DWORD kWdaNone = 0x00000000;
constexpr DWORD kWdaMonitor = 0x00000001;            // the black-cutout trap
constexpr DWORD kWdaExcludeFromCapture = 0x00000011; // what the overlay must use

/// The overlay's fill colour.
///
/// **Not magenta**, which the first draft of this file used: `ScreenAnimator` paints the
/// eight corners of the RGB cube as vertical bars, and one of them *is* magenta. An
/// overlay coloured like the pattern it sits on cannot be distinguished from it, and the
/// test would have been unable to fail. Orange is in neither the bar palette nor the
/// two greys of the moving block.
constexpr COLORREF kOverlayColour = RGB(255, 128, 0);
constexpr int kOverlayR = 255;
constexpr int kOverlayG = 128;
constexpr int kOverlayB = 0;

/// How close a pixel must be to `kOverlayColour` to count as the overlay. Wide, because
/// the capture path is BGRA in and BGRA out but DWM may still have composited the window
/// with fractional scaling at the edges; narrow enough that no bar colour or grey can
/// reach it.
constexpr int kColourTolerance = 24;

/// Mean luma below this is "black" for the purposes of the cutout assertion. The white
/// bar the overlay sits on reads ~255, so there is no ambiguity to tune around.
constexpr double kCutoutMaxLuma = 24.0;

/// Mean luma above this means the animator's white bar came through intact.
constexpr double kIntactMinLuma = 170.0;

constexpr auto kAcquireTimeout = std::chrono::milliseconds{1500};

/// Frames to look at per case. More than one because DWM may need a composition pass to
/// react to the affinity change, and the assertion is about the steady state.
constexpr int kFramesToInspect = 4;

// ---------------------------------------------------------------------------
// A test-owned overlay window
// ---------------------------------------------------------------------------

/// A solid-colour topmost window, placed at an exact screen rect.
///
/// Deliberately as close to what the real pill will be as the properties under test
/// require: topmost, no activation, tool window, popup. Everything else the pill will
/// have -- rounded corners, translucency, controls -- is irrelevant to whether DWM keeps
/// it out of a capture surface.
class OverlayWindow {
public:
    OverlayWindow() = default;

    ~OverlayWindow() {
        destroy();
    }

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;
    OverlayWindow(OverlayWindow&&) = delete;
    OverlayWindow& operator=(OverlayWindow&&) = delete;

    [[nodiscard]] bool create(const RECT& screen_rect) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        // **No class background brush, deliberately.** The first version of this fixture
        // gave the class a solid brush and deleted it in `destroy()` -- but a window
        // class outlives the windows made from it and cannot be re-registered, so the
        // second `create()` in a test kept the *first* registration and painted with a
        // brush that had already been `DeleteObject`'d. The window came up unpainted,
        // the recreation case measured an overlay fraction of 0, and the failure looked
        // like "the affinity survived recreation" -- the exact opposite of what had
        // happened. Painting explicitly in `repaint` has no such lifetime to get wrong.
        cls.hbrBackground = nullptr;
        cls.lpszClassName = L"FrameCaptureOverlayExclusionTest";
        // A duplicate registration across test cases is expected and is not a failure;
        // the class outlives any one window.
        if (RegisterClassExW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
                                  L"FrameCapture overlay under test", WS_POPUP, screen_rect.left, screen_rect.top,
                                  screen_rect.right - screen_rect.left, screen_rect.bottom - screen_rect.top, nullptr,
                                  nullptr, cls.hInstance, nullptr);
        return window_ != nullptr;
    }

    /// Applies a display affinity, and **reports whether Windows accepted it**.
    ///
    /// The return value is the point. A test that set the affinity and assumed it took
    /// would, on a machine that refused, assert against a window that was never excluded
    /// and report a pass for the wrong reason.
    [[nodiscard]] bool set_affinity(DWORD affinity) const {
        return SetWindowDisplayAffinity(window_, affinity) != FALSE;
    }

    [[nodiscard]] DWORD affinity() const {
        DWORD value = kWdaNone;
        return GetWindowDisplayAffinity(window_, &value) != FALSE ? value : 0xFFFFFFFF;
    }

    /// Shows the window above the animator's own topmost window and paints it.
    void show_on_top() const {
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        repaint();
    }

    /// Drains the message queue, then fills the window with `kOverlayColour`.
    ///
    /// Both halves are needed, for different reasons. The capture loop below blocks in
    /// `acquire`, so nothing pumps this window's messages unless the test does it —
    /// and the fill is explicit rather than left to `WM_ERASEBKGND` so that painting
    /// does not depend on a window class registration this fixture cannot re-do.
    ///
    /// The brush is created and destroyed per call. That is a few microseconds a handful
    /// of times per test, and it buys a fixture with no GDI object outliving the scope
    /// that made it.
    void repaint() const {
        if (window_ == nullptr) {
            return;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        const HDC dc = GetDC(window_);
        if (dc == nullptr) {
            return;
        }
        RECT client{};
        if (GetClientRect(window_, &client) != 0) {
            if (const HBRUSH brush = CreateSolidBrush(kOverlayColour); brush != nullptr) {
                FillRect(dc, &client, brush);
                DeleteObject(brush);
            }
        }
        ReleaseDC(window_, dc);
    }

    void destroy() {
        if (window_ != nullptr) {
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

private:
    HWND window_ = nullptr;
};

// ---------------------------------------------------------------------------
// Reading pixels back
// ---------------------------------------------------------------------------

/// One captured frame's pixels, in the texture's own BGRA layout.
struct Readback {
    std::vector<std::uint8_t> pixels; ///< BGRA, `row_pitch` bytes per row
    UINT row_pitch = 0;
    UINT width = 0;
    UINT height = 0;
};

/// Copies a captured texture into system memory.
///
/// A staging copy rather than anything cleverer: this runs a handful of times per test
/// and correctness is the only property that matters here.
[[nodiscard]] bool read_back(ID3D11Device* device, ID3D11Texture2D* texture, Readback& out) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    // Anything but BGRA8 means the system is in HDR and the frame is FP16 (SPEC.md
    // §4.2). That is a different pipeline with its own tone-map branch, and this test
    // has nothing to say about it -- the caller skips rather than reinterpreting the
    // bits, which is the very bug §4.2 warns about.
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return false;
    }

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> copy;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &copy))) {
        return false;
    }

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(copy.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    out.row_pitch = mapped.RowPitch;
    out.width = desc.Width;
    out.height = desc.Height;
    out.pixels.resize(static_cast<std::size_t>(mapped.RowPitch) * desc.Height);
    std::memcpy(out.pixels.data(), mapped.pData, out.pixels.size());
    context->Unmap(copy.Get(), 0);
    return true;
}

/// What a rectangle of a captured frame contains.
struct RegionStats {
    double mean_luma = 0.0;
    double overlay_fraction = 0.0; ///< pixels within tolerance of `kOverlayColour`
    std::uint64_t samples = 0;
};

/// Mean BT.709 luma of a small box centred on (`cx`, `cy`).
[[nodiscard]] double luma_at(const Readback& frame, int cx, int cy) {
    constexpr int kHalf = 4;
    double total = 0.0;
    int samples = 0;
    for (int y = std::max(0, cy - kHalf); y < std::min(static_cast<int>(frame.height), cy + kHalf); ++y) {
        const std::uint8_t* row = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.row_pitch);
        for (int x = std::max(0, cx - kHalf); x < std::min(static_cast<int>(frame.width), cx + kHalf); ++x) {
            const std::uint8_t* pixel = row + (static_cast<std::size_t>(x) * 4);
            total += (0.2126 * pixel[2]) + (0.7152 * pixel[1]) + (0.0722 * pixel[0]);
            ++samples;
        }
    }
    return samples > 0 ? total / samples : 0.0;
}

/// True when this frame really is a capture of `ScreenAnimator`'s pattern.
///
/// **Not optional, and this test was wrong without it.** `DdaCapture::emit` re-emits its
/// copy target on `DXGI_ERROR_WAIT_TIMEOUT` (SPEC.md §4.3's duplicate-frame rule), and
/// before the first real copy has landed that target is a freshly created texture full
/// of zeros. Those frames are black everywhere — including under the overlay's rect — so
/// a test that inspected them measured "black region" and concluded *black cutout*, on a
/// machine where exclusion was working perfectly.
///
/// The first version of this file had that bug and it was **flaky, not merely wrong**:
/// under ctest each case runs in its own process and the timing differed, so the DDA
/// exclusion case passed there and failed when the suite was run in one process. A green
/// run was proving nothing, which is exactly what CLAUDE.md §6 warns about.
///
/// Sampled at two bar centres in the top third, both outside the overlay's rect: bar 0
/// is black and bar 6 is yellow. A zeroed frame fails the second. The moving block never
/// enters the top third, so neither sample depends on where it is.
[[nodiscard]] bool shows_pattern(const Readback& frame) {
    const int width = static_cast<int>(frame.width);
    const int height = static_cast<int>(frame.height);
    if (width <= 0 || height <= 0) {
        return false;
    }
    const int bar = std::max(1, width / 8);
    const int y = height / 12;
    const double black_bar = luma_at(frame, bar / 2, y);
    const double yellow_bar = luma_at(frame, (bar * 6) + (bar / 2), y);
    return black_bar < kCutoutMaxLuma && yellow_bar > kIntactMinLuma;
}

[[nodiscard]] RegionStats inspect(const Readback& frame, const RECT& region) {
    RegionStats stats;
    const int left = std::max(0, static_cast<int>(region.left));
    const int top = std::max(0, static_cast<int>(region.top));
    const int right = std::min(static_cast<int>(frame.width), static_cast<int>(region.right));
    const int bottom = std::min(static_cast<int>(frame.height), static_cast<int>(region.bottom));

    double luma_total = 0.0;
    std::uint64_t overlay_hits = 0;
    for (int y = top; y < bottom; ++y) {
        const std::uint8_t* row = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.row_pitch);
        for (int x = left; x < right; ++x) {
            const std::uint8_t* pixel = row + (static_cast<std::size_t>(x) * 4);
            const int b = pixel[0];
            const int g = pixel[1];
            const int r = pixel[2];
            // BT.709 luma, which is what SPEC.md §6 makes the pipeline's colour space.
            luma_total += (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
            if (std::abs(r - kOverlayR) <= kColourTolerance && std::abs(g - kOverlayG) <= kColourTolerance &&
                std::abs(b - kOverlayB) <= kColourTolerance) {
                ++overlay_hits;
            }
            ++stats.samples;
        }
    }

    if (stats.samples > 0) {
        stats.mean_luma = luma_total / static_cast<double>(stats.samples);
        stats.overlay_fraction = static_cast<double>(overlay_hits) / static_cast<double>(stats.samples);
    }
    return stats;
}

// ---------------------------------------------------------------------------

enum class Backend { Wgc, Dda };

[[nodiscard]] const char* name_of(Backend backend) {
    return backend == Backend::Wgc ? "WGC" : "DDA";
}

class OverlayExclusionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("overlay");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "overlayexcl1";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        auto primary = fc::capture::primary_display();
        ASSERT_TRUE(primary.has_value()) << "no primary display";
        display_ = primary.value();

        ASSERT_TRUE(topology_.refresh(fc::gpu::DiscoveryOptions{false}).has_value());

        MONITORINFO info{};
        info.cbSize = sizeof(info);
        ASSERT_NE(GetMonitorInfoW(reinterpret_cast<HMONITOR>(display_.monitor), &info), 0);
        monitor_ = info.rcMonitor;

        fc::test::ScreenAnimator::Settings pattern;
        pattern.monitor = display_.monitor;
        pattern.fps = 60;
        ASSERT_TRUE(animator_.start(pattern).has_value()) << "the pattern fixture did not start";

        // The overlay sits inside the animator's **rightmost bar, which is white**, in
        // the top third, which the moving block never enters. That choice is what makes
        // all three outcomes separable at a glance:
        //
        //   excluded    -> white  (mean luma ~255)
        //   WDA_MONITOR -> black  (mean luma ~0)
        //   not stamped -> orange (overlay_fraction ~1)
        //
        // Placing it over the black bar instead would make "excluded" and "cut out"
        // the same measurement, and the test could not tell the defect from the fix.
        const int width = animator_.width();
        const int height = animator_.height();
        const int bar = std::max(1, width / 8);
        const int inset = std::max(8, bar / 8);
        region_.left = width - bar + inset;
        region_.right = width - inset;
        region_.top = height / 12;
        region_.bottom = region_.top + std::max(32, height / 24);
    }

    void TearDown() override {
        overlay_.destroy();
        animator_.stop();
        fc::log::shutdown();
        dir_.reset();
    }

    [[nodiscard]] fc::gpu::AdapterId owning_adapter() const {
        const fc::gpu::AdapterInfo* owner = topology_.topology().owner_of_monitor(display_.monitor);
        return owner != nullptr ? owner->id : fc::gpu::AdapterId{};
    }

    /// Places the overlay over `region_` and applies `affinity`.
    ///
    /// Returns false when Windows refused the affinity, which the caller reports rather
    /// than asserting on -- `WDA_EXCLUDEFROMCAPTURE` being refused is the finding this
    /// whole file exists to surface, and it deserves a message, not a bare failure.
    [[nodiscard]] bool place_overlay(DWORD affinity) {
        RECT screen = region_;
        OffsetRect(&screen, monitor_.left, monitor_.top);
        if (!overlay_.create(screen)) {
            return false;
        }
        if (!overlay_.set_affinity(affinity)) {
            return false;
        }
        overlay_.show_on_top();
        // A composition pass, so the capture below sees the steady state rather than
        // the frame in which the window appeared.
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
        overlay_.repaint();
        return overlay_.affinity() == affinity;
    }

    /// Captures through `backend` and returns the worst-case view of `region_`.
    ///
    /// "Worst case" per statistic: the **highest** overlay fraction seen and the
    /// **lowest** mean luma. A single clean frame among several dirty ones must not be
    /// able to carry the assertion, which averaging across frames would allow.
    [[nodiscard]] ::testing::AssertionResult sample(Backend backend, RegionStats& worst) {
        auto device = fc::test::device_for_adapter(owning_adapter());
        if (!device.has_value()) {
            return ::testing::AssertionFailure() << "no device on the display-owning adapter";
        }

        CaptureTarget target;
        target.monitor = display_.monitor;

        WgcCapture wgc;
        DdaCapture dda;
        fc::capture::IScreenCapture* capture =
            backend == Backend::Wgc ? static_cast<fc::capture::IScreenCapture*>(&wgc) : &dda;

        if (const auto started = capture->start(device.value().device(), target); !started.has_value()) {
            return ::testing::AssertionFailure()
                   << name_of(backend) << " did not start: " << fc::error_name(started.error());
        }

        worst = RegionStats{};
        int inspected = 0;
        int rejected = 0;
        // Bounded rather than "until we have enough": a backend that never delivers a
        // usable frame must end the loop, not spin in it.
        for (int attempt = 0; attempt < kFramesToInspect * 8 && inspected < kFramesToInspect; ++attempt) {
            overlay_.repaint();
            auto frame = capture->acquire(kAcquireTimeout);
            if (!frame.has_value()) {
                continue;
            }
            Readback pixels;
            const bool ok = read_back(device.value().device(), frame.value().texture, pixels);
            capture->release(frame.value());
            if (!ok) {
                capture->stop();
                return ::testing::AssertionFailure()
                       << name_of(backend)
                       << " produced a frame this test cannot read. If the format is FP16, system HDR is on and "
                          "SPEC.md §4.2's tone-map path applies -- turn HDR off to run this case.";
            }

            // The gate. See `shows_pattern`: an unpopulated DDA duplicate is black
            // everywhere, and asserting on one measures the fixture's startup rather
            // than the affinity.
            if (!shows_pattern(pixels)) {
                ++rejected;
                continue;
            }

            const RegionStats stats = inspect(pixels, region_);
            if (inspected == 0) {
                worst = stats;
            } else {
                worst.mean_luma = std::min(worst.mean_luma, stats.mean_luma);
                worst.overlay_fraction = std::max(worst.overlay_fraction, stats.overlay_fraction);
                worst.samples = stats.samples;
            }
            ++inspected;
        }
        capture->stop();

        if (inspected == 0) {
            return ::testing::AssertionFailure()
                   << name_of(backend) << " delivered no frame showing the test pattern (" << rejected
                   << " rejected). Either the fixture is not on the captured output, or the backend is not "
                      "delivering.";
        }

        // Printed on success as well as failure. CLAUDE.md §6: "a number is what the
        // next person compares against when it changes" -- and the interesting question
        // about this test is never "did it pass" but "how far from the threshold was
        // it", which a green run otherwise throws away.
        std::printf("[ measured ] %s region=%ldx%ld mean_luma=%.1f overlay_fraction=%.4f samples=%llu "
                    "frames=%d rejected=%d\n",
                    name_of(backend), region_.right - region_.left, region_.bottom - region_.top, worst.mean_luma,
                    worst.overlay_fraction, static_cast<unsigned long long>(worst.samples), inspected, rejected);
        std::fflush(stdout);
        return ::testing::AssertionSuccess();
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    fc::gpu::GpuTopologyService topology_;
    DisplaySource display_;
    fc::test::ScreenAnimator animator_;
    OverlayWindow overlay_;
    RECT monitor_{};
    RECT region_{};
};

// ---------------------------------------------------------------------------
// The negative control
//
// Without this every other assertion in the file is vacuous: an overlay window that
// never made it onto the screen is "not in the capture" too, and would pass the
// exclusion cases for entirely the wrong reason.
// ---------------------------------------------------------------------------

TEST_F(OverlayExclusionTest, AnUnstampedOverlayIsCapturedWgc) {
    ASSERT_TRUE(place_overlay(kWdaNone)) << "could not place the control overlay";

    RegionStats stats;
    ASSERT_TRUE(sample(Backend::Wgc, stats));
    EXPECT_GT(stats.overlay_fraction, 0.9)
        << "the control overlay was not captured, so every other case in this file proves nothing. "
        << "overlay_fraction=" << stats.overlay_fraction << " mean_luma=" << stats.mean_luma;
}

TEST_F(OverlayExclusionTest, AnUnstampedOverlayIsCapturedDda) {
    ASSERT_TRUE(place_overlay(kWdaNone)) << "could not place the control overlay";

    RegionStats stats;
    ASSERT_TRUE(sample(Backend::Dda, stats));
    EXPECT_GT(stats.overlay_fraction, 0.9)
        << "overlay_fraction=" << stats.overlay_fraction << " mean_luma=" << stats.mean_luma;
}

// ---------------------------------------------------------------------------
// The trap, in executable form
//
// `WDA_MONITOR` hides the window and leaves a black hole. Asserted rather than merely
// described, so the difference between the two constants is a fact this suite checks
// rather than a claim in a comment.
// ---------------------------------------------------------------------------

TEST_F(OverlayExclusionTest, WdaMonitorProducesTheBlackCutoutDefect) {
    if (!place_overlay(kWdaMonitor)) {
        GTEST_SKIP() << "WDA_MONITOR was refused; the trap cannot be demonstrated here";
    }

    RegionStats stats;
    ASSERT_TRUE(sample(Backend::Wgc, stats));
    EXPECT_LT(stats.overlay_fraction, 0.01) << "WDA_MONITOR should hide the window's colour";
    EXPECT_LT(stats.mean_luma, kCutoutMaxLuma)
        << "WDA_MONITOR is expected to blacken the region. If this no longer holds, the constant's behaviour has "
           "changed and the guidance in overlay/exclusion.py needs revisiting. mean_luma="
        << stats.mean_luma;
}

// ---------------------------------------------------------------------------
// The requirement
// ---------------------------------------------------------------------------

TEST_F(OverlayExclusionTest, ExcludeFromCaptureLeavesNoOverlayAndNoCutoutWgc) {
    ASSERT_TRUE(place_overlay(kWdaExcludeFromCapture))
        << "WDA_EXCLUDEFROMCAPTURE was refused on this machine. SPEC.md §1's floor is Windows 10 build 19041, which "
           "is the build that introduced it, so this is a finding and not an environment problem.";

    RegionStats stats;
    ASSERT_TRUE(sample(Backend::Wgc, stats));

    // Half one: the overlay is not in the frame.
    EXPECT_LT(stats.overlay_fraction, 0.01)
        << "the overlay was captured despite WDA_EXCLUDEFROMCAPTURE; overlay_fraction=" << stats.overlay_fraction;

    // Half two, and the one a naive test omits: nor is a black rectangle. The pattern
    // behind the overlay is the animator's white bar, so it must come through white.
    EXPECT_GT(stats.mean_luma, kIntactMinLuma)
        << "the region under the overlay came back dark, which is the black-cutout defect rather than exclusion. "
           "mean_luma="
        << stats.mean_luma;
}

TEST_F(OverlayExclusionTest, ExcludeFromCaptureLeavesNoOverlayAndNoCutoutDda) {
    ASSERT_TRUE(place_overlay(kWdaExcludeFromCapture)) << "WDA_EXCLUDEFROMCAPTURE was refused on this machine";

    RegionStats stats;
    ASSERT_TRUE(sample(Backend::Dda, stats));

    // **This is the case M9.6's design hangs on.** WGC honouring the affinity is well
    // documented; DDA duplicates DWM's composed desktop and whether the exclusion
    // reaches that path was unverified before this test. A failure here does not mean
    // the overlay is impossible -- it means `advanced.capture_backend` must be narrowed
    // to WGC whenever an overlay is on screen (M9.6 §1.3's ladder).
    EXPECT_LT(stats.overlay_fraction, 0.01)
        << "DDA captured the overlay despite WDA_EXCLUDEFROMCAPTURE. M9.6 §1.3's fallback ladder applies: force WGC "
           "while an overlay is visible. overlay_fraction="
        << stats.overlay_fraction;
    EXPECT_GT(stats.mean_luma, kIntactMinLuma)
        << "DDA returned a black region where the overlay was. mean_luma=" << stats.mean_luma;
}

// ---------------------------------------------------------------------------
// The failure that only shows up later
// ---------------------------------------------------------------------------

TEST_F(OverlayExclusionTest, AffinityIsLostWhenTheWindowIsRecreatedAndMustBeReapplied) {
    ASSERT_TRUE(place_overlay(kWdaExcludeFromCapture));

    // Qt destroys and recreates native handles on flag changes and reparenting, and the
    // affinity does not come with them. Simulated here by destroying the window and
    // making a new one at the same place: a fresh HWND is capturable by default, which
    // is precisely why `_AffinityGuard` re-stamps on `WinIdChange` rather than stamping
    // once at construction.
    overlay_.destroy();
    RECT screen = region_;
    OffsetRect(&screen, monitor_.left, monitor_.top);
    ASSERT_TRUE(overlay_.create(screen));
    EXPECT_EQ(overlay_.affinity(), kWdaNone) << "a newly created window was somehow already excluded";

    overlay_.show_on_top();
    std::this_thread::sleep_for(std::chrono::milliseconds{250});

    RegionStats before;
    ASSERT_TRUE(sample(Backend::Wgc, before));
    EXPECT_GT(before.overlay_fraction, 0.9) << "the recreated window should be capturable, and was not";

    ASSERT_TRUE(overlay_.set_affinity(kWdaExcludeFromCapture));
    std::this_thread::sleep_for(std::chrono::milliseconds{250});

    RegionStats after;
    ASSERT_TRUE(sample(Backend::Wgc, after));
    EXPECT_LT(after.overlay_fraction, 0.01) << "re-stamping the recreated handle did not exclude it again";
    EXPECT_GT(after.mean_luma, kIntactMinLuma);
}

} // namespace
