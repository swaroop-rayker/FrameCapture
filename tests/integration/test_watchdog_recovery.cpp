// SPEC.md §20 row 9 -- "Frame freeze / stuck", and the session-level half of row 11.
//
// GPU TIER.
//
// Row 9's mitigation is "watchdog that detects `no frame in 3 × frame_interval` and
// forces a session rebuild", tested by "inject a stalled downstream consumer, assert
// recovery < 500 ms with a contiguous timeline".
//
// The detector has been green on the CPU tier since M6
// (`HealthMonitor.NoFrameForThreeFrameIntervalsIsReportedAsAStall`). What was missing
// was anything that *acted* on it, because a rebuild spans capture and the pipeline and
// no component owned both. `RecordingSession` does, and this is where that shows.
//
// The same file covers the two other things that were reported-but-never-acted-on:
//   * SPEC.md §13 rung 4 -- three encoder failures in 60 s asks for a GPU migration;
//   * SPEC.md §13 rungs 6/7 -- `stop_requested`, which had no consumer at all.
//
// ---------------------------------------------------------------------------
// What "inject a stalled consumer" means here
// ---------------------------------------------------------------------------
// Row 9's own wording is about a *downstream* stall. The session detects it upstream:
// the health monitor watches when capture last produced a frame, and a downstream stall
// that backs the whole pipeline up eventually starves capture too. Stalling the mux
// thread with §20.1's sanctioned disk-stall injection is the honest way to produce that
// from outside -- it is a real stall in a real thread, not a simulated one.

#include "core/capture/i_screen_capture.h"
#include "core/capture/source_resolver.h"
#include "core/gpu/gpu_topology.h"
#include "core/health/health_monitor.h"
#include "core/logging/logger.h"
#include "core/pipeline/recording_session.h"

#include "decoded_media.h"
#include "screen_animator.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using fc::pipeline::MigrationRecord;
using fc::pipeline::RebuildCause;
using fc::pipeline::RecordingSession;
using fc::pipeline::SessionSettings;
using fc::pipeline::SessionStats;

/// A capture backend that goes quiet, and can be asked to do so either **healthily** or
/// **because it died**.
///
/// ---------------------------------------------------------------------------
/// Why this is a fair model and not a seam that deletes the behaviour
/// ---------------------------------------------------------------------------
/// docs/ACCEPTANCE.md records four occasions where a test seam removed the thing under
/// test (BUG-037's injected capture factory, BUG-038's supplied clock, BUG-042's exact
/// timestamp grid, M9's never-entered keyframe wait), so a new stub deserves the same
/// question: what does it take away?
///
/// **Nothing that is under test here.** The subject is `RecordingSession`'s *decision* --
/// given a backend that has stopped producing, does it tear the capture stack down? -- and
/// that decision is a pure function of the signals the backend offers. This supplies those
/// signals exactly as WGC does: real D3D textures from a real `SyntheticSource` while
/// frames flow, and then `CAPTURE_FRAME_TIMEOUT` forever, which is precisely what
/// `WgcCapture::acquire` returns when the compositor has not called `FrameArrived`. The
/// same reasoning `RecordingSession::inject_device_error` is documented with: it does not
/// fake the response, it supplies the input the real thing would.
///
/// **The silence is supplied because it cannot be provoked.** Measured on this rig,
/// `AStaticScreenIsNotMistakenForAWedgedCapture` covers the output with a topmost window
/// painted once and WGC still delivers **301 frames in 15 s**. A machine quiet enough for
/// the field failure -- `worst_stall_us=11113420` -- is not a machine that is also running
/// a test.
class QuietingCapture final : public fc::capture::IScreenCapture {
public:
    struct Settings {
        /// Frames to deliver before going quiet.
        int frames_before_silence = 60;
        /// What `running()` reports once quiet.
        ///
        /// `true` models an idle desktop: the backend is fine, the screen is not changing.
        /// `false` models the worker having died -- which is what both real backends do,
        /// since each sets `running` false as it leaves its loop.
        bool alive_when_quiet = true;
    };

    explicit QuietingCapture(const Settings& settings) : settings_(settings) {}

    [[nodiscard]] fc::Result<void> start(ID3D11Device* device, const fc::capture::CaptureTarget& target) override {
        fc::test::SyntheticSource::Settings source;
        source.width = 1280;
        source.height = 720;
        source.fps = 60;
        source.prerendered_frames = 8;
        source.pace_to_real_time = true;
        FC_TRY(inner_.configure(source));
        FC_TRY(inner_.start(device, target));
        delivered_.store(0, std::memory_order_release);
        return fc::ok();
    }

    void stop() override {
        inner_.stop();
    }

    [[nodiscard]] bool running() const noexcept override {
        if (delivered_.load(std::memory_order_acquire) < settings_.frames_before_silence) {
            return inner_.running();
        }
        return settings_.alive_when_quiet && inner_.running();
    }

    [[nodiscard]] fc::Result<fc::capture::CaptureFrame> acquire(std::chrono::milliseconds timeout) override {
        if (delivered_.load(std::memory_order_acquire) >= settings_.frames_before_silence) {
            // Exactly what WGC returns on an idle desktop: the queue was empty for the
            // whole wait, and that is all it can say.
            std::this_thread::sleep_for(timeout);
            return fc::FcError::CAPTURE_FRAME_TIMEOUT;
        }
        auto frame = inner_.acquire(timeout);
        if (frame.has_value()) {
            delivered_.fetch_add(1, std::memory_order_acq_rel);
        }
        return frame;
    }

    void release(const fc::capture::CaptureFrame& frame) override {
        inner_.release(frame);
    }

    [[nodiscard]] fc::capture::Backend backend() const noexcept override {
        return fc::capture::Backend::Wgc;
    }

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept override {
        return inner_.dropped_frames();
    }

private:
    Settings settings_;
    fc::test::SyntheticSource inner_;
    std::atomic<int> delivered_{0};
};

class WatchdogRecoveryTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("watchdog");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "watchdogrec001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    static void TearDownTestSuite() {
        fc::log::shutdown();
        dir_.reset();
    }

    /// A session fed by the synthetic source (CLAUDE.md §5).
    ///
    /// **Not the real desktop.** The first version of this file captured the primary
    /// display, and that is exactly what §5 forbids: "Tests that depend on what happens
    /// to be on screen are not tests." WGC is change-driven, so on an idle desktop the
    /// recording collected a handful of frames and the §10.4 validation gate's duration
    /// check became a coin flip -- measured, the case passed alone and failed inside a
    /// full tier run.
    ///
    /// The synthetic source is rebuilt on the new device by the same factory, which is
    /// what makes it a fair stand-in: the session tears it down and re-creates it
    /// exactly as it would a WGC session.
    static SessionSettings settings_for(const char* name) {
        SessionSettings settings;
        settings.output = dir_->path() / (std::string{name} + ".mkv");
        settings.video.width = 1920;
        settings.video.height = 1080;
        settings.video.fps = 60;
        settings.video.container = fc::config::Container::Mkv;

        // A monitor is still resolved, because §5.2's selection policy keys on which
        // adapter owns the capture target -- the frames just do not come from it.
        const auto display = fc::capture::primary_display();
        if (display.has_value()) {
            settings.target.monitor = display.value().monitor;
        }

        settings.capture_factory =
            [](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
            fc::test::SyntheticSource::Settings source;
            source.width = 1920;
            source.height = 1080;
            source.fps = 60;
            source.prerendered_frames = 8;
            // Paced, because `RecordingSession`'s capture loop relies on the backend
            // to pace it -- WGC and DDA both block until a frame exists.
            source.pace_to_real_time = true;

            auto created = std::make_unique<fc::test::SyntheticSource>();
            FC_TRY(created->configure(source));
            FC_TRY(created->start(device, fc::capture::CaptureTarget{}));
            return std::unique_ptr<fc::capture::IScreenCapture>{std::move(created)};
        };
        return settings;
    }

    /// A session on the **real** capture backend, aimed at the primary display.
    ///
    /// Deliberately without `capture_factory`, which is the opposite of every other case
    /// in this file and the entire point of the one that uses it: the synthetic source
    /// produces a frame whenever it is asked, so no test that injects it can ever observe
    /// what a *change-driven* backend does when the screen does not change. WGC's whole
    /// contract is that it says nothing when there is nothing to say, and that silence is
    /// what row 9's trigger has to interpret.
    ///
    /// The screen is still owned rather than ambient (CLAUDE.md §5) -- see the caller,
    /// which covers the output with a `ScreenAnimator` at `fps = 0`.
    static SessionSettings real_capture_settings(const char* name, const fc::capture::DisplaySource& display) {
        SessionSettings settings;
        settings.output = dir_->path() / (std::string{name} + ".mkv");
        // The output's own size, evened off: §14.3 pins the encoded resolution, and a
        // mismatch against the source would be a resolution change rather than a capture.
        settings.video.width = display.width - (display.width % 2);
        settings.video.height = display.height - (display.height % 2);
        settings.video.fps = 60;
        settings.video.container = fc::config::Container::Mkv;
        settings.target.monitor = display.monitor;
        return settings;
    }

    /// A session whose backend stops producing after a second of frames.
    static SessionSettings quieting_settings(const char* name, bool alive_when_quiet) {
        SessionSettings settings;
        settings.output = dir_->path() / (std::string{name} + ".mkv");
        settings.video.width = 1280;
        settings.video.height = 720;
        settings.video.fps = 60;
        settings.video.container = fc::config::Container::Mkv;

        if (const auto display = fc::capture::primary_display(); display.has_value()) {
            settings.target.monitor = display.value().monitor;
        }

        settings.capture_factory =
            [alive_when_quiet](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
            QuietingCapture::Settings quiet;
            quiet.frames_before_silence = 60;
            quiet.alive_when_quiet = alive_when_quiet;
            auto created = std::make_unique<QuietingCapture>(quiet);
            FC_TRY(created->start(device, fc::capture::CaptureTarget{}));
            return std::unique_ptr<fc::capture::IScreenCapture>{std::move(created)};
        };
        return settings;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
};

std::unique_ptr<fc::test::TempDir> WatchdogRecoveryTest::dir_;

// ---------------------------------------------------------------------------
// Row 11, at the level that actually drives it
// ---------------------------------------------------------------------------

// The end-to-end shape of SPEC.md §5.4: a device error arrives at the seam a driver
// would deliver it to, and the session performs all eight steps without the caller
// doing anything. This is what `test_gpu_migration` could not cover -- it drives
// `rebuild_device` directly and so proves the mechanism, not the orchestration.
TEST_F(WatchdogRecoveryTest, AnInjectedDeviceLossRebuildsTheSessionWithinTheBudget) {
    SessionSettings settings = settings_for("device_loss");
    ASSERT_NE(settings.target.monitor, 0u) << "no primary display to capture";

    RecordingSession session;
    const fc::Result<void> started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    std::this_thread::sleep_for(std::chrono::seconds{2});

    // The seam a real `DXGI_ERROR_DEVICE_REMOVED` arrives at.
    constexpr auto kDeviceRemoved = static_cast<std::int32_t>(0x887A0005);
    session.inject_device_error(kDeviceRemoved);

    // The session's watchdog polls at 2 Hz, so the rebuild lands within a tick or two.
    for (int i = 0; i < 100 && session.migrations().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const auto report = session.stop();

    ASSERT_FALSE(migrations.empty()) << "the injected device loss never produced a rebuild";
    const MigrationRecord& record = migrations.front();

    std::cout << "[ MEASURED ] injected device loss -> session rebuild:\n"
              << "[ MEASURED ]   cause " << fc::pipeline::to_string(record.cause) << ", gap "
              << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms (budget "
              << (fc::gpu::kMigrationGapBudgetNs / 1'000'000) << " ms), same_file " << record.same_file << "\n"
              << "[ MEASURED ]   captured " << stats.frames_captured << " frames, " << stats.rebuilds << " rebuild(s), "
              << stats.segments << " segment(s)\n"
              << "[ MEASURED ]   phases: teardown " << (static_cast<double>(record.teardown_ns) / 1'000'000.0)
              << " ms, discovery " << (static_cast<double>(record.discovery_ns) / 1'000'000.0) << " ms, device "
              << (static_cast<double>(record.device_ns) / 1'000'000.0) << " ms, pipeline "
              << (static_cast<double>(record.pipeline_ns) / 1'000'000.0) << " ms, capture "
              << (static_cast<double>(record.capture_ns) / 1'000'000.0) << " ms\n";

    EXPECT_EQ(record.cause, RebuildCause::DeviceLost);
    EXPECT_FALSE(record.failed) << "the rebuild failed rather than recovering";

    // One fault, one rebuild. See `TheLadderCanTriggerARebuild` for why this is an
    // equality and not a lower bound.
    EXPECT_EQ(migrations.size(), 1u)
        << "one injected device loss produced " << migrations.size()
        << " rebuilds; the watcher is reporting the rebuild's own DXGI activity as a new change";

    // SPEC.md §5.4's budget, on the whole procedure this time rather than on
    // `rebuild_device` alone.
    EXPECT_LT(record.gap_ns, fc::gpu::kMigrationGapBudgetNs)
        << "the rebuild took " << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms against a "
        << (fc::gpu::kMigrationGapBudgetNs / 1'000'000) << " ms budget. Phases (ms): teardown "
        << (static_cast<double>(record.teardown_ns) / 1'000'000.0) << ", discovery "
        << (static_cast<double>(record.discovery_ns) / 1'000'000.0) << ", device "
        << (static_cast<double>(record.device_ns) / 1'000'000.0) << ", pipeline "
        << (static_cast<double>(record.pipeline_ns) / 1'000'000.0) << ", capture "
        << (static_cast<double>(record.capture_ns) / 1'000'000.0);

    // The prime directive, which is the half a migration must never cost.
    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(record.output, options, media);
    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_GT(media.video.frame_count, 0) << "the file after the rebuild holds no decodable frames";

    for (std::size_t i = 1; i < media.video.pts.size(); ++i) {
        ASSERT_GT(media.video.pts[i], media.video.pts[i - 1]) << "PTS went backwards at frame " << i;
    }
}

// ---------------------------------------------------------------------------
// Row 9's negative control -- and it is the more important of the two
// ---------------------------------------------------------------------------

// Without this, "the stall detector fired" proves nothing: a detector that fires on
// ordinary jitter would rebuild a healthy recording every few seconds and the positive
// case above would still be green.
//
// It matters especially because capture is genuinely intermittent. WGC is
// change-driven: an idle desktop composites rarely or not at all, so a healthy
// recording of a static screen legitimately goes seconds without a frame. If the row 9
// detector treated that as a freeze, every unattended recording would thrash.
TEST_F(WatchdogRecoveryTest, AHealthyRecordingIsNeverRebuilt) {
    SessionSettings settings = settings_for("healthy");
    ASSERT_NE(settings.target.monitor, 0u);

    RecordingSession session;
    const fc::Result<void> started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    std::this_thread::sleep_for(std::chrono::seconds{6});

    const SessionStats stats = session.stats();
    const auto report = session.stop();

    std::cout << "[ MEASURED ] healthy 6 s recording: captured " << stats.frames_captured << " frames, "
              << stats.capture_timeouts << " acquire timeouts, " << stats.rebuilds << " rebuilds, stall episodes "
              << stats.health.stall_episodes << "\n";

    EXPECT_EQ(stats.rebuilds, 0u)
        << "a healthy recording was rebuilt " << stats.rebuilds
        << " time(s). Either the row 9 stall detector fires on ordinary capture jitter, or the device watcher is "
           "reporting a change that is not there -- both would make every unattended recording thrash";
    EXPECT_EQ(stats.segments, 1u);

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
}

// ---------------------------------------------------------------------------
// Row 9's missing negative control: silence is not a fault
// ---------------------------------------------------------------------------

// A static screen must not be mistaken for a wedged capture.
//
// **This is the case `AHealthyRecordingIsNeverRebuilt` cannot make.** That one injects
// `SyntheticSource`, which produces a frame every time it is asked, so the session under
// test never experiences silence at all — and row 9's trigger is entirely about how to
// interpret silence. Measured in the field before this existed: an unattended recording
// logged `worst_stall_us=11113420` with `cause=capture_stalled`, tore down the capture and
// device stack, and cost `gap_ms=243` — repeatedly, for as long as nobody touched the
// machine.
//
// The screen is *owned*, not merely unattended (CLAUDE.md §5, BUG-033): `ScreenAnimator`
// at `fps = 0` covers the output with a topmost window and paints it once. Ambient content
// is occluded, so "nothing is changing" is a fact this test established rather than a
// condition it hoped for. That distinction is not pedantic — a first attempt at
// reproducing this against the live desktop captured **1944 frames in 40 s**, because a
// blinking cursor and a clock are enough to keep WGC delivering, and it reported no defect
// at all.
TEST_F(WatchdogRecoveryTest, AStaticScreenIsNotMistakenForAWedgedCapture) {
    const auto display = fc::capture::primary_display();
    ASSERT_TRUE(display.has_value()) << "no primary display to capture";

    fc::test::ScreenAnimator animator;
    fc::test::ScreenAnimator::Settings animation;
    animation.monitor = display.value().monitor;
    // Paint once and hold. The output is provably static from here on.
    animation.fps = 0;
    ASSERT_TRUE(animator.start(animation).has_value()) << "could not present a static pattern to the output";

    RecordingSession session;
    const fc::Result<void> started = session.start(real_capture_settings("static_screen", display.value()));
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    // Comfortably past the trigger, whatever it is: long enough for several rebuild
    // cycles if the recording is going to thrash, short enough for a routine tier.
    constexpr auto kWatch = std::chrono::seconds{15};
    std::this_thread::sleep_for(kWatch);

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const auto report = session.stop();
    animator.stop();

    std::cout << "[ MEASURED ] static screen for " << kWatch.count() << " s: captured " << stats.frames_captured
              << " frames, " << stats.capture_timeouts << " acquire timeouts, " << stats.rebuilds << " rebuild(s), "
              << stats.health.stall_episodes << " stall episodes, worst stall "
              << (static_cast<double>(stats.health.worst_stall_ns) / 1'000'000.0) << " ms\n";
    for (const MigrationRecord& record : migrations) {
        std::cout << "[ MEASURED ]   rebuild: cause " << fc::pipeline::to_string(record.cause) << ", gap "
                  << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms\n";
    }

    // The detector is *expected* to fire — a static screen really does mean no frames, and
    // SPEC.md §20 row 9 asks for that to be observable. What must not happen is the
    // session acting on it.
    EXPECT_EQ(stats.rebuilds, 0u)
        << "a static screen was rebuilt " << stats.rebuilds
        << " time(s). Silence on a change-driven backend is not evidence of a fault: WGC delivers when the desktop "
           "composites, so an idle machine is indistinguishable from a wedged one by frame arrival alone";
    EXPECT_TRUE(migrations.empty());
    EXPECT_EQ(stats.segments, 1u) << "the recording was broken into pieces by rebuilds nobody asked for";

    // And the prime directive holds regardless: a recording of a screen that never changed
    // is still a valid, playable file of the right length.
    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
}

// The pair that actually discriminates, and neither half means anything without the other.
//
// A backend that has stopped producing looks identical from the frame stream whether the
// screen is static or the capture is wedged. The only difference is whether the backend
// *says* it is still running -- so that is what the session must key on, and these two
// cases differ in exactly that one bit.
//
// **The second case is not optional.** Removing the trigger entirely would make the first
// case pass and would be a strictly worse engine: a genuinely dead capture would record
// nothing for the rest of the session and never say why. So the fix is judged by both, and
// a change that satisfies one at the expense of the other is not a fix.

TEST_F(WatchdogRecoveryTest, ABackendThatIsQuietButHealthyIsLeftAlone) {
    RecordingSession session;
    const fc::Result<void> started = session.start(quieting_settings("quiet_healthy", /*alive_when_quiet=*/true));
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    // Eight seconds of silence, against a trigger that fired at two.
    std::this_thread::sleep_for(std::chrono::seconds{9});

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const auto report = session.stop();

    std::cout << "[ MEASURED ] quiet-but-healthy: captured " << stats.frames_captured << " frames, " << stats.rebuilds
              << " rebuild(s), " << stats.health.stall_episodes << " stall episodes, worst stall "
              << (static_cast<double>(stats.health.worst_stall_ns) / 1'000'000.0) << " ms\n";

    // The detector still reports -- SPEC.md §20 row 9 asks for that, and a user is entitled
    // to know their screen has not changed in eight seconds.
    EXPECT_GT(stats.health.worst_stall_ns, 2'000'000'000LL)
        << "the backend did not actually go quiet, so this case proves nothing";

    EXPECT_EQ(stats.rebuilds, 0u) << "a healthy backend was torn down for saying nothing had changed";
    EXPECT_TRUE(migrations.empty());
    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
}

TEST_F(WatchdogRecoveryTest, ABackendThatHasDiedIsRebuilt) {
    RecordingSession session;
    const fc::Result<void> started = session.start(quieting_settings("quiet_dead", /*alive_when_quiet=*/false));
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    // Long enough for the trigger plus a rebuild, and no longer: once it rebuilds, the
    // fresh backend goes quiet again and it will rebuild repeatedly by design.
    std::this_thread::sleep_for(std::chrono::seconds{6});

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const auto report = session.stop();

    std::cout << "[ MEASURED ] quiet-and-dead: captured " << stats.frames_captured << " frames, " << stats.rebuilds
              << " rebuild(s), worst gap " << (static_cast<double>(stats.worst_rebuild_gap_ns) / 1'000'000.0)
              << " ms\n";
    for (const MigrationRecord& record : migrations) {
        std::cout << "[ MEASURED ]   rebuild: cause " << fc::pipeline::to_string(record.cause) << ", gap "
                  << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms\n";
    }

    ASSERT_FALSE(migrations.empty()) << "a capture backend whose worker had exited was never rebuilt; the recording "
                                        "would produce nothing for the rest of the session and say nothing about it";
    EXPECT_EQ(migrations.front().cause, RebuildCause::CaptureStalled);

    // §20 row 9's budget, on the rebuild that matters.
    EXPECT_LT(static_cast<double>(migrations.front().gap_ns) / 1'000'000.0, 500.0)
        << "recovery took longer than row 9's 500 ms";

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
}

// ---------------------------------------------------------------------------
// SPEC.md §13 rung 4 -- reported since M6, acted on here
// ---------------------------------------------------------------------------

TEST_F(WatchdogRecoveryTest, TheLadderCanTriggerARebuild) {
    SessionSettings settings = settings_for("rung4");
    ASSERT_NE(settings.target.monitor, 0u);

    RecordingSession session;
    const fc::Result<void> started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    std::this_thread::sleep_for(std::chrono::seconds{2});

    // Three `DEVICE_REMOVED` reports inside 60 s is rung 4's trigger. Each one also
    // reaches the device watcher, so the first is enough to provoke a rebuild -- which
    // is the point: the two paths agree about what to do, and the recording is rebuilt
    // once rather than once per path.
    constexpr auto kDeviceRemoved = static_cast<std::int32_t>(0x887A0005);
    for (int i = 0; i < 3; ++i) {
        session.inject_device_error(kDeviceRemoved);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    for (int i = 0; i < 100 && session.migrations().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    std::this_thread::sleep_for(std::chrono::seconds{2});

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const auto report = session.stop();

    std::cout << "[ MEASURED ] rung 4: " << migrations.size() << " rebuild(s), worst gap "
              << (static_cast<double>(stats.worst_rebuild_gap_ns) / 1'000'000.0) << " ms\n";

    // Per rebuild, because the count alone cannot say *why* a second one happened and
    // the two candidate causes are distinguishable here: a trigger that was not latched
    // reports the same cause twice, while the watcher mistaking the rebuild's own DXGI
    // activity for a fresh change reports `TopologyChanged` second. Printed on every
    // run, for BUG-032's reason -- the first occurrence of BUG-035 produced a number and
    // nothing to act on.
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const MigrationRecord& record = migrations[i];
        std::cout << "[ MEASURED ]   rebuild " << (i + 1) << ": cause " << fc::pipeline::to_string(record.cause)
                  << ", gap " << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms, same_file "
                  << record.same_file << ", failed " << record.failed << "\n";
    }

    ASSERT_FALSE(migrations.empty()) << "three device failures produced no rebuild at all";

    // **One burst, one rebuild.** This is the assertion that found the loop: before
    // `DeviceWatcher::acknowledge` existed, a rebuild's own DXGI activity made the next
    // poll report a fresh topology change, so responding to a fault provoked another
    // one. Measured then: three injections produced five rebuilds and a single
    // injection produced four.
    //
    // The bound is 1 rather than "a small number", because the whole point is that a
    // burst is coalesced. A driver that flaps once a second would otherwise rebuild the
    // stack once a second, which costs far more than the faults do.
    EXPECT_EQ(migrations.size(), 1u)
        << "one burst of device failures produced " << migrations.size()
        << " rebuilds; either the trigger is not latched or the watcher is reporting the rebuild's own activity as a "
           "new change, and a flapping driver would thrash the recording";

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
}

} // namespace
