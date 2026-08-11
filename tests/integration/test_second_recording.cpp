// A second recording in the same engine process (BUG-037).
//
// GPU TIER.
//
// ---------------------------------------------------------------------------
// What this covers, and why nothing covered it before
// ---------------------------------------------------------------------------
// The engine process is long-lived: SPEC.md §3.1 has the GUI spawn it once and drive it
// over IPC, so `start_record` arrives many times in one process and every recording after
// the first runs against whatever state the previous one left behind. Nothing tested
// that. Every pipeline test in the tree either records once and exits, or injects a
// capture backend through `PipelineSettings::capture_factory` -- and that injection skips
// `capture::create_capture` entirely, which is the exact call that faulted.
//
// The state that leaks between recordings is a COM apartment. The `capture` thread enters
// the MTA at start and leaves at stop; so does the `audio` thread; so does endpoint
// enumeration. When the last of them leaves, the MTA shuts down and its in-proc servers
// unload -- while C++/WinRT's **process-wide** activation-factory cache goes on holding
// pointers into them. The second recording's `GraphicsCaptureSession::IsSupported()` then
// calls through a vtable in an unmapped library. Measured from the field minidump: an
// access violation reading 0x7FFA38D8B0D8, inside `GraphicsCapture.dll`'s range in the
// **unloaded**-module list.
//
// ---------------------------------------------------------------------------
// How the first assertion is stated, and why it is not the crash
// ---------------------------------------------------------------------------
// The defect's own failure mode is an access violation, which no `EXPECT` can catch --
// it takes the test binary with it. So the invariant is asserted one step upstream, on
// the thing that has to be true for the call to be safe: **the library backing the cached
// factory is still loaded.** That fails cleanly on a broken build, and the call that
// would fault follows it, so a build that somehow keeps the module and still faults is
// not quietly passed either.

#include "core/audio/loopback_capture.h"
#include "core/capture/capture_factory.h"
#include "core/capture/wgc/wgc_capture.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/util/com_apartment.h"

#include "adapter_device.h"
#include "screen_animator.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

namespace {

using fc::capture::CaptureTarget;

/// The calling thread's apartment, entered and left around one operation.
///
/// **Load-bearing, and the reason a first attempt at this test passed on a broken
/// build.** C++/WinRT's `get_activation_factory` retries with `CoIncrementMTAUsage`
/// when `RoGetActivationFactory` comes back `CO_E_NOTINITIALIZED` -- so a probe issued
/// from a thread that has *never* entered an apartment silently pins the process MTA as
/// a side effect and the defect cannot occur. The engine's IPC thread is not that
/// thread: by the time `start_record` reaches `create_capture` it has already been in
/// and out of an apartment (endpoint enumeration, device discovery), so the fallback
/// never fires and nothing is pinned. This reproduces that condition rather than the
/// friendlier one a bare test thread would have.
class ScopedMta {
public:
    ScopedMta() : entered_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}

    ~ScopedMta() {
        if (entered_) {
            CoUninitialize();
        }
    }

    ScopedMta(const ScopedMta&) = delete;
    ScopedMta& operator=(const ScopedMta&) = delete;
    ScopedMta(ScopedMta&&) = delete;
    ScopedMta& operator=(ScopedMta&&) = delete;

    [[nodiscard]] bool entered() const noexcept {
        return entered_;
    }

private:
    bool entered_;
};

class SecondRecordingTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("secondrec");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "secondrecord001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh(fc::gpu::DiscoveryOptions{false}).has_value());

        const fc::gpu::AdapterInfo* primary = topology_->topology().primary_display_adapter();
        ASSERT_NE(primary, nullptr) << "no adapter owns the primary display";
        ASSERT_FALSE(primary->outputs.empty());
        monitor_ = primary->outputs.front().monitor;

        // The capture device must be on the adapter that owns the output, or WGC hands
        // back nothing while reporting success (CLAUDE.md §8, SPEC.md §5.1).
        auto created = fc::test::device_for_adapter(primary->id);
        ASSERT_TRUE(created.has_value());
        device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
    }

    void TearDown() override {
        animator_.stop();
        device_.reset();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// One recording's worth of capture-session lifetime: select a backend through the
    /// real factory -- which is what probes WGC -- then start it and stop it, so the
    /// `capture` thread enters its apartment and leaves again exactly as it does around
    /// a real recording.
    void capture_cycle() {
        auto created = fc::capture::create_capture(fc::config::CaptureBackend::Auto, target());
        ASSERT_TRUE(created.has_value()) << "create_capture failed: " << fc::error_name(created.error());

        std::unique_ptr<fc::capture::IScreenCapture> capture = std::move(created).value();
        ASSERT_TRUE(capture->start(device_->device(), target()).has_value());
        // Long enough for the worker to have reached its steady state, so the stop below
        // is a real teardown rather than a race against startup.
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        capture->stop();
        capture.reset();
    }

    [[nodiscard]] CaptureTarget target() const {
        CaptureTarget value;
        value.monitor = monitor_;
        return value;
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    std::unique_ptr<fc::gpu::D3dDevice> device_;
    fc::test::ScreenAnimator animator_;
    std::uintptr_t monitor_ = 0;
};

TEST_F(SecondRecordingTest, TheCaptureBackendCanBeProbedAgainAfterARecordingHasTornItsApartmentDown) {
    // Owns what is on the output for the duration, per CLAUDE.md §5 and BUG-033. Nothing
    // here asserts on pixels, but a capture session that composites nothing at all is a
    // weaker exercise of the start/stop path than one that does.
    fc::test::ScreenAnimator::Settings animation;
    animation.monitor = monitor_;
    animation.fps = 30;
    ASSERT_TRUE(animator_.start(animation).has_value()) << "could not present a pattern to the target output";

    HMODULE before = nullptr;
    {
        // The first recording's probe, from a thread that is in an apartment -- as the
        // IPC thread is. This is the call that populates C++/WinRT's factory cache.
        const ScopedMta apartment;
        ASSERT_TRUE(apartment.entered()) << "could not enter an MTA on the test thread";

        ASSERT_TRUE(fc::capture::probe_availability().wgc_supported)
            << "WGC is unsupported on this rig; the cached-factory path this covers is unreachable";

        before = GetModuleHandleW(L"GraphicsCapture.dll");
        ASSERT_NE(before, nullptr) << "GraphicsCapture.dll is not loaded after a successful WGC probe, so the "
                                      "cached-factory hazard this test is about cannot be observed here";
    }

    // --- the invariant -----------------------------------------------------
    // The apartment has been left. The library holding the vtable of the factory this
    // process still has cached must nonetheless still be mapped.
    //
    // `ASSERT`, not `EXPECT`, and stated before anything else touches WinRT: on a build
    // without the fix the next probe is an access violation, and a test that takes the
    // runner down with it reports "SEH exception with code 0xc0000005" and nothing about
    // why. Failing here returns from the case with the reason written out.
    const HMODULE after_leaving = GetModuleHandleW(L"GraphicsCapture.dll");
    ASSERT_NE(after_leaving, nullptr)
        << "GraphicsCapture.dll was unloaded when the apartment was left, while the process-wide WinRT factory "
           "cache still points into it. The next call through that cache is BUG-037's access violation";
    ASSERT_EQ(after_leaving, before) << "GraphicsCapture.dll was unloaded and reloaded at a different base; the "
                                        "cached factory's vtable pointer is stale either way";
    EXPECT_TRUE(fc::process_mta_held()) << "nothing pinned the process MTA";

    // Through `ASSERT_NO_FATAL_FAILURE`: the helper's `ASSERT`s abort the helper, not
    // this case, and continuing past a capture session that never started would test
    // nothing while looking like it had.
    ASSERT_NO_FATAL_FAILURE(capture_cycle());

    // Endpoint enumeration between the two, because it enters and leaves an apartment on
    // the calling thread and the real engine does it on this seam -- the GUI asks for the
    // device list between recordings.
    static_cast<void>(fc::audio::enumerate_render_endpoints());

    const HMODULE after_recording = GetModuleHandleW(L"GraphicsCapture.dll");
    ASSERT_NE(after_recording, nullptr) << "GraphicsCapture.dll was unloaded when the recording's capture thread "
                                           "left its apartment (BUG-037)";
    ASSERT_EQ(after_recording, before);

    // --- and the call that faulted -----------------------------------------
    // Reached only if the module survived. On a build without the fix this is the access
    // violation itself, which is why the assertions above exist to fail first.
    {
        const ScopedMta apartment;
        EXPECT_TRUE(fc::capture::probe_availability().wgc_supported)
            << "the second probe reported WGC unsupported, which on this rig means the WinRT call threw";
    }

    // A third cycle, because "survives once" and "survives" are different claims and the
    // teardown is what does the damage.
    ASSERT_NO_FATAL_FAILURE(capture_cycle());
    EXPECT_NE(GetModuleHandleW(L"GraphicsCapture.dll"), nullptr);
    {
        const ScopedMta apartment;
        EXPECT_TRUE(fc::capture::probe_availability().wgc_supported);
    }

    std::printf("[BUG-037] GraphicsCapture.dll stayed loaded at %p across 2 capture cycles and 3 probes\n",
                static_cast<void*>(before));
}

} // namespace
