// DDA backend, source resolution, and the M2 sustained run (SPEC.md §4.1, §4.3, §24).
//
// GPU TIER.
//
// The 60-second run is opt-in: it is a soak, not a unit test, and adding a minute
// to every `ctest` would make people stop running it. Enable with
// `FC_RUN_SOAK=1`; otherwise it runs a short version that still exercises the same
// code path.
//
// **Every case here that acquires a frame owns what is on the output first**
// (`present_pattern`, BUG-033). These backends duplicate a real `IDXGIOutput`, so
// they cannot move to `SyntheticSource` -- but capturing whatever happened to be
// on screen meant the tests reported the desktop's state as a verdict on the code,
// and five of them failed a Release tier on a quiet machine for that reason.
// `ScreenAnimator` covers the target output and changes it at a stated rate, so
// the input is the test's.

#include "core/capture/dda/dda_capture.h"
#include "core/capture/nv12_writer.h"
#include "core/capture/source_resolver.h"
#include "core/capture/wgc/wgc_capture.h"
#include "adapter_device.h"
#include "screen_animator.h"

#include "core/color/nv12_converter.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using fc::capture::CaptureTarget;
using fc::capture::DisplaySource;
using fc::capture::Nv12Writer;
using fc::capture::dda::DdaCapture;
using fc::capture::wgc::WgcCapture;

bool soak_enabled() {
    std::size_t size = 0;
    char value[8] = {};
    return getenv_s(&size, value, sizeof(value), "FC_RUN_SOAK") == 0 && size > 0 && value[0] == '1';
}

double luma_variance(const std::vector<std::uint8_t>& nv12, int width, int height) {
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (nv12.size() < count) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sum += nv12[i];
    }
    const double mean = sum / static_cast<double>(count);
    double squared = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double delta = nv12[i] - mean;
        squared += delta * delta;
    }
    return squared / static_cast<double>(count);
}

class SustainedCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("sustained");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "sustained000001";
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

    /// Takes ownership of what the target output shows, for the rest of the test
    /// (BUG-033).
    ///
    /// `fps == 0` holds a single picture, which is how a case asks for a
    /// *provably* idle desktop rather than an unattended one.
    ///
    /// Call through `ASSERT_NO_FATAL_FAILURE` -- a failure here means the test's
    /// input is ambient again, and every assertion after it is about the desktop.
    void present_pattern(int fps) {
        fc::test::ScreenAnimator::Settings settings;
        settings.monitor = display_.monitor;
        settings.fps = fps;
        const auto started = animator_.start(settings);
        ASSERT_TRUE(started.has_value()) << "could not present a pattern to the target output: "
                                         << fc::error_name(started.error());
    }

    /// Device on the adapter that owns `id`, or on a deliberately different one.
    static fc::Result<fc::gpu::D3dDevice> device_for(fc::gpu::AdapterId id) {
        return fc::test::device_for_adapter(id);
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

// ---------------------------------------------------------------------------
// Source resolution -- SPEC.md §4.1
// ---------------------------------------------------------------------------

TEST_F(SustainedCaptureTest, EnumeratesDisplaysWithStableIdentifiers) {
    auto displays = fc::capture::enumerate_displays();
    ASSERT_TRUE(displays.has_value());
    ASSERT_FALSE(displays.value().empty());

    for (const DisplaySource& display : displays.value()) {
        EXPECT_FALSE(display.stable_id.empty()) << display.device_name;
        EXPECT_NE(display.monitor, 0u);
        EXPECT_GT(display.width, 0);
        EXPECT_GT(display.height, 0);
    }

    // Stable ids must be unique, or resolution is ambiguous.
    for (std::size_t i = 0; i < displays.value().size(); ++i) {
        for (std::size_t j = i + 1; j < displays.value().size(); ++j) {
            EXPECT_NE(displays.value()[i].stable_id, displays.value()[j].stable_id);
        }
    }
}

// The point of §4.1's "not by index": resolution must survive enumeration order.
TEST_F(SustainedCaptureTest, ADisplayResolvesBackFromItsStableId) {
    auto resolved = fc::capture::resolve_display(display_.stable_id);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved.value().monitor, display_.monitor);
    EXPECT_EQ(resolved.value().device_name, display_.device_name);
}

TEST_F(SustainedCaptureTest, AnAbsentDisplayIsReportedNotGuessed) {
    const auto resolved = fc::capture::resolve_display(R"(\\?\DISPLAY#NOPE0000#never)");
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(resolved.error(), fc::FcError::CAPTURE_TARGET_NOT_FOUND);
}

TEST_F(SustainedCaptureTest, ReportsPhysicalPixelsNotDpiScaledOnes) {
    // BUG-004: a DPI-unaware process is told a virtualised size. The GPU test
    // binary sets awareness in main, so these must agree with the display mode.
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const std::wstring device(display_.device_name.begin(), display_.device_name.end());
    ASSERT_NE(EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode), 0);

    EXPECT_EQ(display_.width, static_cast<std::int32_t>(mode.dmPelsWidth))
        << "reported width is DPI-scaled rather than physical";
    EXPECT_EQ(display_.height, static_cast<std::int32_t>(mode.dmPelsHeight));
}

TEST_F(SustainedCaptureTest, EnumeratesWindowsWithProcessAndClass) {
    auto windows = fc::capture::enumerate_windows();
    ASSERT_TRUE(windows.has_value());

    for (const fc::capture::WindowSource& window : windows.value()) {
        EXPECT_NE(window.hwnd, 0u);
        EXPECT_FALSE(window.title.empty());
        EXPECT_NE(window.process_id, GetCurrentProcessId()) << "our own window was offered as a source";
    }
}

TEST_F(SustainedCaptureTest, ADeadWindowHandleFailsReacquisitionCleanly) {
    fc::capture::WindowSource ghost;
    ghost.hwnd = 0xDEAD0000;
    ghost.class_name = "FrameCaptureNoSuchClass";
    ghost.process_name = "no_such_process.exe";

    const auto reacquired = fc::capture::reacquire_window(ghost);
    ASSERT_FALSE(reacquired.has_value());
    EXPECT_EQ(reacquired.error(), fc::FcError::CAPTURE_TARGET_GONE);
}

// ---------------------------------------------------------------------------
// DDA -- SPEC.md §4.3
// ---------------------------------------------------------------------------

TEST_F(SustainedCaptureTest, DdaCapturesOnTheAdapterThatOwnsTheOutput) {
    ASSERT_NO_FATAL_FAILURE(present_pattern(60));

    auto device = device_for(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = display_.monitor;

    DdaCapture capture;
    const auto started = capture.start(device.value().device(), target);
    ASSERT_TRUE(started.has_value()) << "DDA failed on the owning adapter, error " << fc::error_name(started.error());

    auto frame = capture.acquire(std::chrono::milliseconds{2000});
    ASSERT_TRUE(frame.has_value());
    EXPECT_NE(frame.value().texture, nullptr);
    EXPECT_EQ(frame.value().adapter, owning_adapter());
    capture.release(frame.value());
    capture.stop();
}

// The headline bug of this whole project, asserted directly: duplicating an output
// from a device on the wrong adapter must be refused, not attempted.
TEST_F(SustainedCaptureTest, DdaRefusesADeviceOnTheWrongAdapter) {
    const fc::gpu::AdapterId owner = owning_adapter();
    ASSERT_TRUE(owner.valid());

    const fc::gpu::AdapterInfo* other = nullptr;
    for (const fc::gpu::AdapterInfo& adapter : topology_.topology().adapters) {
        if (adapter.id != owner && adapter.adapter_class != fc::gpu::AdapterClass::Software) {
            other = &adapter;
            break;
        }
    }
    if (other == nullptr) {
        GTEST_SKIP() << "only one hardware adapter; the affinity constraint cannot be exercised";
    }

    auto device = device_for(other->id);
    ASSERT_TRUE(device.has_value()) << "could not create a device on " << other->description;

    CaptureTarget target;
    target.monitor = display_.monitor;

    DdaCapture capture;
    const auto started = capture.start(device.value().device(), target);

    ASSERT_FALSE(started.has_value()) << "DDA accepted a device on " << other->description
                                      << ", which does not own the target output. This is the configuration that "
                                         "produces an entirely black recording with S_OK from every call.";
    EXPECT_EQ(started.error(), fc::FcError::DDA_ADAPTER_AFFINITY);
    EXPECT_FALSE(capture.running());
}

// ---------------------------------------------------------------------------
// WGC across the hybrid-GPU boundary -- SPEC.md §4.2
// ---------------------------------------------------------------------------

// The property that makes WGC the primary backend: unlike DDA, it captures an
// output owned by a *different* adapter. On a MUX-less laptop that is what lets the
// engine encode on the discrete GPU while the panel hangs off the integrated one.
//
// This is the exact configuration DDA refuses two tests above, so the pair together
// document why there are two backends at all.
TEST_F(SustainedCaptureTest, WgcCapturesAnOutputOwnedByADifferentAdapter) {
    ASSERT_NO_FATAL_FAILURE(present_pattern(60));

    const fc::gpu::AdapterId owner = owning_adapter();
    ASSERT_TRUE(owner.valid());

    const fc::gpu::AdapterInfo* other = nullptr;
    for (const fc::gpu::AdapterInfo& adapter : topology_.topology().adapters) {
        if (adapter.id != owner && adapter.adapter_class != fc::gpu::AdapterClass::Software) {
            other = &adapter;
            break;
        }
    }
    if (other == nullptr) {
        GTEST_SKIP() << "only one hardware adapter; there is no cross-adapter case to exercise";
    }

    auto device = device_for(other->id);
    ASSERT_TRUE(device.has_value()) << "could not create a device on " << other->description;

    CaptureTarget target;
    target.monitor = display_.monitor;

    WgcCapture capture;
    const auto started = capture.start(device.value().device(), target);
    ASSERT_TRUE(started.has_value()) << "WGC failed on " << other->description
                                     << " for an output owned by another adapter, error "
                                     << fc::error_name(started.error());

    auto frame = capture.acquire(std::chrono::milliseconds{2000});
    ASSERT_TRUE(frame.has_value()) << "no frame arrived on the non-owning adapter";
    EXPECT_NE(frame.value().texture, nullptr);

    // The frame must live on the adapter we asked for, not the one that owns the
    // display -- otherwise the texture is unusable by that device downstream.
    EXPECT_EQ(frame.value().adapter, other->id) << "frame is not on the capturing device's adapter";
    EXPECT_EQ(frame.value().dxgi_format, static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_GT(frame.value().content.width(), 0);

    capture.release(frame.value());
    capture.stop();
}

// And the frames are usable: convertible on the same device, and not black.
TEST_F(SustainedCaptureTest, CrossAdapterWgcFramesConvertToUsableNv12) {
    ASSERT_NO_FATAL_FAILURE(present_pattern(60));

    const fc::gpu::AdapterId owner = owning_adapter();
    const fc::gpu::AdapterInfo* other = nullptr;
    for (const fc::gpu::AdapterInfo& adapter : topology_.topology().adapters) {
        if (adapter.id != owner && adapter.adapter_class != fc::gpu::AdapterClass::Software) {
            other = &adapter;
            break;
        }
    }
    if (other == nullptr) {
        GTEST_SKIP() << "only one hardware adapter";
    }

    auto device = device_for(other->id);
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

    int converted = 0;
    int signed_frames = 0;
    double best_variance = 0.0;
    fc::test::PatternSignature last_signature;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (converted < 5 && std::chrono::steady_clock::now() < deadline) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }
        const auto result = converter.convert(device.value().context(), frame.value().texture);
        capture.release(frame.value());
        if (!result.has_value()) {
            ASSERT_EQ(result.error(), fc::FcError::CAPTURE_RESOLUTION_CHANGED);
            continue;
        }
        auto bytes = fc::color::read_back_nv12(device.value().device(), device.value().context(), converter.output(),
                                               width, height);
        ASSERT_TRUE(bytes.has_value());
        best_variance = std::max(best_variance, luma_variance(bytes.value(), width, height));
        last_signature = fc::test::pattern_signature(bytes.value(), width, height);
        if (last_signature.present()) {
            ++signed_frames;
        }
        ++converted;
    }
    capture.stop();

    ASSERT_GT(converted, 0) << "no cross-adapter frame survived conversion";
    EXPECT_GT(best_variance, 1.0) << "every cross-adapter frame was uniform";

    // What makes this a test of the code rather than of the desktop: the frames
    // carry the pattern this process put on the output. Without it the case passes
    // on ambient content whenever the animator has silently landed elsewhere.
    EXPECT_GT(signed_frames * 2, converted)
        << signed_frames << " of " << converted << " frames carried the test pattern (last: left luma "
        << last_signature.left_mean << ", right " << last_signature.right_mean << "); the capture is reading "
        << "something other than what this test presented";
}

TEST_F(SustainedCaptureTest, DdaRejectsWindowTargets) {
    auto device = device_for(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.window = 0x1234;

    DdaCapture capture;
    const auto started = capture.start(device.value().device(), target);
    ASSERT_FALSE(started.has_value());
    // DDA duplicates outputs; window capture is WGC's job.
    EXPECT_EQ(started.error(), fc::FcError::INTERNAL_NOT_IMPLEMENTED);
}

// SPEC.md §4.3: WAIT_TIMEOUT means "no screen change" and must yield a duplicate
// frame with an advanced PTS -- never a stall, never a gap. An idle desktop is
// mostly timeouts, so this is the steady state rather than an edge case.
//
// This is the one case whose controlled input is *stillness*, so it asks for a
// static pattern rather than an animated one: the output is covered by a picture
// this process painted once and holds, so nothing the desktop does can change what
// DDA duplicates.
//
// **What that does and does not buy, measured rather than assumed** (BUG-033).
// Occlusion controls the *content* completely -- 101 of 101 captured frames
// carried this pattern with the animator static. It does *not* stop DWM
// compositing: activity in windows underneath still drove presents at ~20 Hz on a
// busy machine, of an image identical to the one before. So the duplicate-frame
// assertion still needs gaps between presents, and what makes it safe is the
// margin: DDA's acquire timeout is 16 ms, so it takes sustained composition above
// ~62 Hz to starve it of timeouts, against the ~20 Hz measured here and 0 Hz on an
// idle machine. The split is printed so a future regression is visible rather than
// inferred.
TEST_F(SustainedCaptureTest, DdaEmitsDuplicateFramesWhenTheDesktopIsIdle) {
    ASSERT_NO_FATAL_FAILURE(present_pattern(0));

    auto device = device_for(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = display_.monitor;

    DdaCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    int frames = 0;
    std::uint64_t previous_pts = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (frames < 20 && std::chrono::steady_clock::now() < deadline) {
        auto frame = capture.acquire(std::chrono::milliseconds{300});
        if (!frame.has_value()) {
            continue;
        }
        // The timeline must advance whether or not the screen changed.
        EXPECT_GT(frame.value().qpc_ns, previous_pts) << "timestamp did not advance on a duplicate frame";
        previous_pts = frame.value().qpc_ns;
        ++frames;
        capture.release(frame.value());
    }
    capture.stop();

    std::printf("[ RUN INFO ] %d frames collected, %llu of them duplicates, %llu repaints presented\n", frames,
                static_cast<unsigned long long>(capture.duplicate_frames()),
                static_cast<unsigned long long>(animator_.repaints()));

    EXPECT_EQ(animator_.repaints(), 0u) << "the fixture was asked to hold still and did not";
    EXPECT_GT(frames, 0) << "DDA produced nothing at all on an idle desktop";
    EXPECT_GT(capture.duplicate_frames(), 0u)
        << "an idle desktop produced no duplicate frames; WAIT_TIMEOUT is being treated as a gap";
}

// ---------------------------------------------------------------------------
// The M2 sustained run -- SPEC.md §24
// ---------------------------------------------------------------------------

TEST_F(SustainedCaptureTest, SustainedCaptureToNv12StaysCorrect) {
    const auto duration = soak_enabled() ? std::chrono::seconds{60} : std::chrono::seconds{5};

    ASSERT_NO_FATAL_FAILURE(present_pattern(60));

    auto device = device_for(owning_adapter());
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = display_.monitor;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    // Size from the first real frame, never from a reported rectangle (BUG-004).
    auto first = capture.acquire(std::chrono::milliseconds{2000});
    ASSERT_TRUE(first.has_value()) << "no frame arrived";
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

    const std::filesystem::path output = dir_->path() / "sustained.nv12";
    Nv12Writer writer;
    ASSERT_TRUE(writer.open(output, width, height).has_value());

    std::uint64_t converted = 0;
    std::uint64_t uniform_frames = 0;
    std::uint64_t signed_frames = 0;
    std::uint64_t resolution_changes = 0;
    // Sampled here rather than taken whole, so the reported presentation rate
    // covers the same window as the capture rate. The animator has already been
    // running through `start`, the converter's initialise and the first acquire.
    const std::uint64_t repaints_before = animator_.repaints();
    const auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < duration) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }

        const auto result = converter.convert(device.value().context(), frame.value().texture);
        capture.release(frame.value());
        if (!result.has_value()) {
            ASSERT_EQ(result.error(), fc::FcError::CAPTURE_RESOLUTION_CHANGED)
                << "conversion failed unexpectedly: " << fc::error_name(result.error());
            ++resolution_changes;
            continue;
        }

        auto bytes = fc::color::read_back_nv12(device.value().device(), device.value().context(), converter.output(),
                                               width, height);
        ASSERT_TRUE(bytes.has_value());

        // SPEC.md §20 row 1, applied to every frame rather than to a sample.
        if (luma_variance(bytes.value(), width, height) <= 1.0) {
            ++uniform_frames;
        }
        if (fc::test::pattern_signature(bytes.value(), width, height).present()) {
            ++signed_frames;
        }

        ASSERT_TRUE(writer.write(bytes.value()).has_value());
        ++converted;
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Read here, not after `stop()`: joining the capture thread and closing the
    // writer take a few hundred milliseconds, during which the animator keeps
    // painting. Sampling later inflates the presentation rate against a capture
    // rate measured over the shorter window.
    const std::uint64_t presented = animator_.repaints() - repaints_before;
    capture.stop();
    ASSERT_TRUE(writer.close().has_value());

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double fps = static_cast<double>(converted) / seconds;

    // The animator's repaint count belongs next to the capture rate: together they
    // say whether a shortfall was in the backend or in what was feeding it.
    std::printf("[ RUN INFO ] %dx%d, %.1f s, %llu frames (%.1f fps), %llu uniform, %llu with the test pattern, "
                "%llu resolution changes, %llu dropped by the pool, %llu repaints presented (%.1f Hz)\n",
                width, height, seconds, static_cast<unsigned long long>(converted), fps,
                static_cast<unsigned long long>(uniform_frames), static_cast<unsigned long long>(signed_frames),
                static_cast<unsigned long long>(resolution_changes),
                static_cast<unsigned long long>(capture.dropped_frames()), static_cast<unsigned long long>(presented),
                static_cast<double>(presented) / seconds);

    ASSERT_GT(converted, 0u) << "no frames survived the pipeline";

    // Not every frame need be interesting -- a genuinely static desktop can produce
    // a uniform frame -- but they must not all be.
    EXPECT_LT(uniform_frames, converted) << "every frame was uniform; this is the all-black-video signature";

    // And the frames are the ones this test presented, not the desktop's
    // (BUG-033).
    EXPECT_GT(signed_frames * 2, converted) << signed_frames << " of " << converted
                                            << " frames carried the test pattern; the capture is reading something "
                                               "other than what this test put on the output";

    // The file must be exactly N whole frames. Raw NV12 has no framing, so a short
    // write corrupts everything after it.
    const std::size_t frame_bytes = Nv12Writer::frame_size(width, height);
    EXPECT_EQ(std::filesystem::file_size(output), converted * frame_bytes);
    EXPECT_EQ(writer.frames_written(), converted);

    if (soak_enabled()) {
        // Readback stalls the GPU on every frame, so this loop is far slower than
        // the real pipeline will be; the bar here is "sustained", not "60 fps".
        EXPECT_GT(fps, 5.0) << "sustained throughput collapsed";
    }
}

} // namespace
