// The regression test for BUG-033 (SPEC.md §4.2, CLAUDE.md §5).
//
// GPU TIER.
//
// `test_capture_sustained` and `test_capture_to_nv12` capture a real output, and
// so cannot use `SyntheticSource` -- duplicating an `IDXGIOutput` is their whole
// subject. What they use instead is `ScreenAnimator`, which covers the output with
// a pattern this process owns. That moves the trust from "the desktop happened to
// be busy" to "the fixture works", so the fixture is what needs a test.
//
// Two properties, and the fix depends on both:
//
//   * **Content.** What is captured is the pattern, not the desktop -- otherwise
//     the assertions in those files are still about ambient pixels.
//   * **Delivery.** The pattern alone keeps frames arriving. This is the half that
//     makes an unattended, idle machine safe: WGC composites on change, and if the
//     only thing changing is us, the capture rate is ours to set.
//
// The second is asserted against the *presented* count rather than against a
// constant, because a rate threshold would be a test of the machine. Ambient
// activity can only add frames, so `delivered >= 0.8 x presented` holds on a quiet
// machine and on a busy one alike -- which is exactly the property BUG-033's five
// tests lacked.
//
// **The delivery case presents at 30 Hz, not 60, and the reason is measured.**
// This panel is variable-refresh, 48-144 Hz, and DWM settles at the 48 Hz floor
// when only a small region is updating. Presenting at 60 Hz therefore delivers
// ~48 fps -- a ratio of exactly 0.8, sitting on the threshold, for a reason that
// has nothing to do with the fixture. 30 Hz is below any composition floor this
// project will meet, so the ratio measures what it claims to.
//
// What this pair does *not* rule out: on a machine whose desktop is busy enough to
// deliver 24 fps by itself, a fixture that had stopped painting could still clear
// the delivery bar. The content case is the airtight half -- it fails on a single
// frame of anything else -- and the two are read together.

#include "core/capture/source_resolver.h"
#include "core/capture/wgc/wgc_capture.h"

#include "core/color/nv12_converter.h"
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

#include <chrono>
#include <cstdio>
#include <memory>

namespace {

using fc::capture::CaptureTarget;
using fc::capture::DisplaySource;
using fc::capture::wgc::WgcCapture;

constexpr int kPatternFps = 60;

/// Below any composition floor this project will meet, so the delivery ratio is
/// not silently capped by the panel. See the header comment.
constexpr int kMeasuredFps = 30;

/// Below this the fixture is broken rather than merely slow. See the assertion
/// that uses it.
constexpr double kFixtureFloorHz = 10.0;

constexpr auto kWindow = std::chrono::seconds{3};

class ScreenAnimatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("animator");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "screenanimator1";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        auto primary = fc::capture::primary_display();
        ASSERT_TRUE(primary.has_value()) << "no primary display";
        display_ = primary.value();

        ASSERT_TRUE(topology_.refresh(fc::gpu::DiscoveryOptions{false}).has_value());
    }

    void TearDown() override {
        animator_.stop();
        fc::log::shutdown();
        dir_.reset();
    }

    [[nodiscard]] fc::gpu::AdapterId owning_adapter() const {
        const fc::gpu::AdapterInfo* owner = topology_.topology().owner_of_monitor(display_.monitor);
        return owner != nullptr ? owner->id : fc::gpu::AdapterId{};
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    fc::gpu::GpuTopologyService topology_;
    DisplaySource display_;
    fc::test::ScreenAnimator animator_;
};

// The delivery half. No conversion and no readback in the loop: those cap the rate
// at what the GPU can round-trip, which would measure the readback path rather
// than frame delivery.
TEST_F(ScreenAnimatorTest, AnAnimatedPatternDeliversFramesWithoutHelpFromTheDesktop) {
    fc::test::ScreenAnimator::Settings pattern;
    pattern.monitor = display_.monitor;
    pattern.fps = kMeasuredFps;
    ASSERT_TRUE(animator_.start(pattern).has_value());

    auto device = fc::test::device_for_adapter(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = display_.monitor;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    const std::uint64_t repaints_before = animator_.repaints();
    const auto start = std::chrono::steady_clock::now();
    int delivered = 0;
    while (std::chrono::steady_clock::now() - start < kWindow) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }
        ++delivered;
        capture.release(frame.value());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // Before `stop()`, which joins the capture thread while the animator keeps
    // painting -- sampling after it counts presents the capture was never offered.
    const std::uint64_t presented = animator_.repaints() - repaints_before;
    capture.stop();
    std::printf("[ RUN INFO ] presented %llu (%.1f Hz), delivered %llu (%.1f fps), %llu dropped by the pool\n",
                static_cast<unsigned long long>(presented), static_cast<double>(presented) / seconds,
                static_cast<unsigned long long>(delivered), static_cast<double>(delivered) / seconds,
                static_cast<unsigned long long>(capture.dropped_frames()));

    // Separated so a failure says which half broke: a fixture that never painted
    // and a backend that never delivered look identical in a single ratio.
    //
    // Deliberately an absolute floor rather than a fraction of the requested rate.
    // The gate's job is to catch a fixture that is dead, not to police its pacing
    // -- the delivery assertion below divides by what was actually presented, so a
    // fixture slowed by machine load weakens nothing. A fraction here would instead
    // turn background load into a confusing failure in the one test whose whole
    // subject is not depending on the machine.
    const auto floor = static_cast<std::uint64_t>(kFixtureFloorHz * seconds);
    ASSERT_GT(presented, floor) << "the fixture presented almost nothing; nothing below is about capture";

    EXPECT_GT(static_cast<std::uint64_t>(delivered), (presented * 8) / 10)
        << delivered << " frames delivered against " << presented
        << " presented. Frame delivery is not tracking what this process puts on the output, which is the state "
           "BUG-033 was about -- the tests are reading the desktop again";
}

// The content half. If this fails, the capture is composited from something other
// than the fixture's window and every content assertion in the capture tests is
// about ambient pixels.
TEST_F(ScreenAnimatorTest, EveryDeliveredFrameCarriesThePatternRatherThanTheDesktop) {
    fc::test::ScreenAnimator::Settings pattern;
    pattern.monitor = display_.monitor;
    pattern.fps = kPatternFps;
    ASSERT_TRUE(animator_.start(pattern).has_value());

    auto device = fc::test::device_for_adapter(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = display_.monitor;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    auto first = capture.acquire(std::chrono::milliseconds{2000});
    ASSERT_TRUE(first.has_value());
    D3D11_TEXTURE2D_DESC desc{};
    first.value().texture->GetDesc(&desc);
    const int width = static_cast<int>(desc.Width) - (static_cast<int>(desc.Width) % 2);
    const int height = static_cast<int>(desc.Height) - (static_cast<int>(desc.Height) % 2);
    capture.release(first.value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings settings;
    settings.width = width;
    settings.height = height;
    ASSERT_TRUE(converter.initialize(device.value().device(), settings).has_value());

    int examined = 0;
    int carried = 0;
    fc::test::PatternSignature worst;
    worst.left_mean = 0.0;
    worst.right_mean = 255.0;
    const auto deadline = std::chrono::steady_clock::now() + kWindow;
    while (examined < 20 && std::chrono::steady_clock::now() < deadline) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }
        const auto converted = converter.convert(device.value().context(), frame.value().texture);
        capture.release(frame.value());
        if (!converted.has_value()) {
            ASSERT_EQ(converted.error(), fc::FcError::CAPTURE_RESOLUTION_CHANGED);
            continue;
        }
        auto bytes = fc::color::read_back_nv12(device.value().device(), device.value().context(), converter.output(),
                                               width, height);
        ASSERT_TRUE(bytes.has_value());

        const fc::test::PatternSignature signature = fc::test::pattern_signature(bytes.value(), width, height);
        worst.left_mean = std::max(worst.left_mean, signature.left_mean);
        worst.right_mean = std::min(worst.right_mean, signature.right_mean);
        if (signature.present()) {
            ++carried;
        }
        ++examined;
    }
    capture.stop();

    std::printf("[ RUN INFO ] %d of %d frames carried the pattern; worst left luma %.1f (limit %.0f), worst right "
                "%.1f (limit %.0f)\n",
                carried, examined, worst.left_mean, fc::test::PatternSignature::kDarkMax, worst.right_mean,
                fc::test::PatternSignature::kBrightMin);

    ASSERT_GT(examined, 0) << "no frame arrived to examine";

    // Every one, not a majority. The fixture covers the output for the whole run,
    // so a single frame of something else means the occlusion is not holding.
    EXPECT_EQ(carried, examined) << "the capture is compositing something this test does not control";
}

// The guard that keeps a misdirected fixture from looking like a working one. A
// pattern presented to the wrong output leaves the test reading ambient content
// while every line of it says otherwise -- the original defect, one level up.
TEST_F(ScreenAnimatorTest, PresentingToAnOutputThatDoesNotExistIsRefused) {
    fc::test::ScreenAnimator::Settings pattern;
    pattern.monitor = 0xDEAD0000;
    pattern.fps = kPatternFps;

    const auto started = animator_.start(pattern);
    ASSERT_FALSE(started.has_value()) << "a nonexistent output was accepted";
    EXPECT_EQ(started.error(), fc::FcError::CAPTURE_TARGET_NOT_FOUND);
    EXPECT_FALSE(animator_.running());
}

} // namespace
