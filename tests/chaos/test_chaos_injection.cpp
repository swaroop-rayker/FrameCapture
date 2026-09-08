// SPEC.md §20.1's chaos tier.
//
// > **Chaos:** randomized injection of `DEVICE_REMOVED`, `ACCESS_LOST`, audio
// > discontinuity, disk stall, and queue saturation during a 30-minute run; assert the
// > file is always valid.
//
// GPU TIER. The whole point is the real chain -- real capture backend interface, real
// encoder, real muxer, real disk -- because the claim under test is the prime directive
// itself (CLAUDE.md §1): *a failure anywhere in the pipeline must still produce a valid,
// playable output file.* Every other test in this suite injects **one** fault and checks
// **one** recovery. This one injects faults it did not plan, in an order nobody chose,
// and asks the only question that matters afterwards.
//
// ---------------------------------------------------------------------------
// Why randomised, when every fault already has its own deterministic test
// ---------------------------------------------------------------------------
// Because the deterministic tests each establish that the pipeline recovers from a
// fault *starting from a healthy state*. None of them establishes that it recovers from
// a device loss that arrives while the mux queue is already backed up behind a stalled
// disk, or from a second loss arriving during the rebuild from the first. Those are
// states no single-fault test visits, and they are the states a real failing machine
// produces -- a dying drive and a flaky driver are correlated far more often than they
// are independent.
//
// ---------------------------------------------------------------------------
// Reproducibility: the seed is printed, always
// ---------------------------------------------------------------------------
// A randomised test whose failure cannot be replayed is an anecdote. The seed is taken
// from `FC_CHAOS_SEED` when set and from `std::random_device` otherwise, and it is
// printed on **every** run, passing or failing. A failure is re-run with
// `FC_CHAOS_SEED=<the number it printed>` and does the same thing again.
//
// ---------------------------------------------------------------------------
// What is injected here, and what §20.1 names that is not
// ---------------------------------------------------------------------------
// Stated rather than implied, because a chaos tier that silently covers three of five
// named injections while reading as though it covers all five is worse than one that
// says so.
//
//   DEVICE_REMOVED      injected, via `RecordingSession::inject_device_error` --
//                       the seam a real `DXGI_ERROR_DEVICE_REMOVED` arrives at.
//   ACCESS_LOST         injected, same seam, `DXGI_ERROR_ACCESS_LOST`. A distinct
//                       cause with a distinct recovery: §5.4 rebuilds the capture
//                       without necessarily rebuilding the device.
//   disk stall          injected, via `SessionSettings::injected_stall_ns` -- a real
//                       sleep on the real mux thread, the same mechanism row 10 uses.
//                       Configured at start with a period, so stalls recur throughout
//                       the run rather than once.
//   queue saturation    *consequential, not directly driven.* The stall backs the mux
//                       queue up and the encode queue overflows from behind, which is
//                       the same path a failing drive produces -- there is no separate
//                       "saturate the queue" seam because on real hardware there is no
//                       separate cause. Asserted, not assumed: the run fails if the drop
//                       counters never move, because a stall mild enough to drop nothing
//                       leaves this injection named in the header and absent from the
//                       run. That is exactly what the first version of this test did.
//   audio discontinuity **NOT INJECTED.** There is no test seam for it. `AudioTimeline`
//                       injects silence when it detects a gap, but nothing lets a test
//                       *manufacture* the gap -- that would need an injection point in
//                       `LoopbackCapture` or a fake `IAudioClient`, which is engine
//                       work rather than test work. The audio path is live during this
//                       run and its timeline is asserted continuous, so a discontinuity
//                       the faults happen to cause is caught; one that only a
//                       deliberate injection could produce is not. Recorded in
//                       ACCEPTANCE.md rather than left to be discovered.

#include "core/capture/source_resolver.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/recording_session.h"

#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using fc::pipeline::MigrationRecord;
using fc::pipeline::RecordingSession;
using fc::pipeline::SessionSettings;
using fc::pipeline::SessionStats;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// The seams a real driver fault arrives at.
constexpr auto kDeviceRemoved = static_cast<std::int32_t>(0x887A0005); // DXGI_ERROR_DEVICE_REMOVED
constexpr auto kAccessLost = static_cast<std::int32_t>(0x887A0026);    // DXGI_ERROR_ACCESS_LOST

/// One write in this many stalls, for the whole run. A period rather than a one-shot,
/// so the disk is intermittently bad the way a failing one is.
// Sized from `test_slow_disk`, which measured 300 ms on one write in four shedding 183
// of 300 frames on every preset. One in eight is milder -- this run still has to produce
// a watchable file -- but it is well inside the range that demonstrably saturates.
//
// The first version of this test used 120 ms on one write in twenty-four and measured
// **zero** queue drops, which meant §20.1's "queue saturation" was named in the header
// and absent from the run.
constexpr int kStallPeriod = 8;
constexpr std::int64_t kStallNs = 300'000'000; // 300 ms -- still under rung 6's 500 ms threshold

/// How long between fault decisions. Short enough that a 30 s routine run gets several,
/// long enough that a rebuild has finished before the next one is considered.
constexpr auto kFaultInterval = std::chrono::milliseconds{2500};

[[nodiscard]] int seconds_from_env(const char* name, int fallback) {
    // SPEC.md §20.1 names 30 minutes. CLAUDE.md §3's convention is that the routine
    // suite stays quick and the exit-criterion form is one environment variable away.
    //
    // `strtol` with its end pointer checked, not `atoi`, for the same reason the seed
    // below uses `strtoull`: `atoi` cannot report a conversion failure, so a typo in the
    // variable silently becomes the default and the long run nobody notices did not
    // happen. Matches `test_av_sync`'s helper.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup, before any thread starts.
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || parsed < 1) {
        return fallback;
    }
    return static_cast<int>(std::min<long>(parsed, 4L * 60L * 60L));
}

[[nodiscard]] unsigned seed_from_env() {
    // `strtoull`, not `atol`. `std::random_device` produces the full 32-bit unsigned
    // range, `long` is **32 bits** on Windows, and `atol` saturates at LONG_MAX -- so
    // every seed above 2147483647, which is half of them, silently became 2147483647.
    // The failure mode is the worst one available to this feature: the test prints
    // "re-run with FC_CHAOS_SEED=3130200536", you do, and it runs a different schedule
    // while reporting success at reproducing. Found by checking that the seed round-trips
    // rather than assuming it.
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup, before any thread starts.
    if (const char* raw = std::getenv("FC_CHAOS_SEED"); raw != nullptr) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(raw, &end, 10);
        const bool complete = end != nullptr && *end == '\0' && end != raw;
        if (complete && parsed > 0 && parsed <= 0xFFFFFFFFull) {
            return static_cast<unsigned>(parsed);
        }
        std::cout << "[ MEASURED ] FC_CHAOS_SEED=" << raw
                  << " is not a value in [1, 4294967295]; using a random seed instead\n";
    }
    std::random_device device;
    return device();
}

/// One fault the run decided to apply, kept so the failure message can say what
/// happened rather than only that something did.
struct AppliedFault {
    double at_seconds = 0.0;
    const char* what = "";
};

class ChaosTest : public ::testing::Test {
protected:
    /// Where the engine's own log goes, and it is **not** inside the `TempDir`.
    ///
    /// A chaos failure costs thirty minutes to reproduce, so the one thing that must
    /// survive it is the evidence. `TempDir` deletes itself in `TearDownTestSuite`,
    /// which ran after the first failure and took the log with it -- leaving three
    /// competing explanations for `INTERNAL_INVALID_STATE` and no way to choose between
    /// them without another half-hour run.
    static std::filesystem::path log_directory() {
        return std::filesystem::temp_directory_path() / "framecapture-chaos-logs";
    }

    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("chaos");
        fc::log::Config config;
        config.directory = log_directory();
        config.session_id = "chaostest00001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());
    }

    static void TearDownTestSuite() {
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// A session on the synthetic source, with the disk intermittently stalled.
    ///
    /// The synthetic source rather than the real backend, for the reason CLAUDE.md §5
    /// gives: a test that depends on what happens to be on screen is not a test. It is
    /// paced to real time because `RecordingSession`'s capture loop is paced *by* its
    /// backend -- WGC and DDA both block until a frame exists -- so an unpaced source
    /// would have the session running at a rate no real capture produces.
    static SessionSettings settings_for(const char* name) {
        SessionSettings settings;
        settings.output = dir_->path() / (std::string{name} + ".mkv");
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;

        // §5.2's encoder selection keys on which adapter owns the capture target, so a
        // monitor is resolved even though the frames do not come from it.
        if (const auto display = fc::capture::primary_display(); display.has_value()) {
            settings.target.monitor = display.value().monitor;
        }

        settings.injected_stall_ns = kStallNs;
        settings.injected_stall_period = kStallPeriod;

        settings.capture_factory =
            [](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
            fc::test::SyntheticSource::Settings source;
            source.width = kWidth;
            source.height = kHeight;
            source.fps = kFps;
            source.prerendered_frames = 8;
            source.pace_to_real_time = true;

            auto created = std::make_unique<fc::test::SyntheticSource>();
            FC_TRY(created->configure(source));
            FC_TRY(created->start(device, fc::capture::CaptureTarget{}));
            return std::unique_ptr<fc::capture::IScreenCapture>{std::move(created)};
        };
        return settings;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> ChaosTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> ChaosTest::topology_;

// ---------------------------------------------------------------------------

TEST_F(ChaosTest, RandomisedFaultInjectionAlwaysYieldsAValidFile) {
    const int duration_s = seconds_from_env("FC_CHAOS_SECONDS", 30);
    const unsigned seed = seed_from_env();

    // Printed before anything can fail, so a crash still leaves the seed in the log.
    std::cout << "[ MEASURED ] chaos seed " << seed << " (re-run with FC_CHAOS_SEED=" << seed << "), " << duration_s
              << " s, disk stalled " << (kStallNs / 1'000'000) << " ms every " << kStallPeriod << " writes\n"
              << std::flush;

    std::mt19937 rng{seed};
    // Weighted so the run spends most of its time recording rather than rebuilding: a
    // pipeline given no time between faults proves only that it cannot keep up with a
    // fault rate no hardware produces.
    std::discrete_distribution<int> choice{
        55, // nothing this interval
        20, // DEVICE_REMOVED
        20, // ACCESS_LOST
        5,  // both, back to back -- the correlated case single-fault tests never reach
    };

    SessionSettings settings = settings_for("chaos");
    ASSERT_NE(settings.target.monitor, 0u) << "no primary display to resolve an adapter from";

    RecordingSession session;
    const fc::Result<void> started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    std::vector<AppliedFault> applied;
    const auto begun = std::chrono::steady_clock::now();
    const auto deadline = begun + std::chrono::seconds{duration_s};

    // Let the pipeline reach a steady state before the first fault. A device loss during
    // start-up is a different test (§20 row 1) and would make this one's failures
    // ambiguous.
    std::this_thread::sleep_for(std::chrono::seconds{2});

    while (std::chrono::steady_clock::now() < deadline) {
        const auto elapsed = [&] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
        };

        switch (choice(rng)) {
        case 1:
            session.inject_device_error(kDeviceRemoved);
            applied.push_back({elapsed(), "DEVICE_REMOVED"});
            break;
        case 2:
            session.inject_device_error(kAccessLost);
            applied.push_back({elapsed(), "ACCESS_LOST"});
            break;
        case 3:
            // Back to back, deliberately: the second arrives while the first is being
            // recovered from. `DeviceWatcher`'s settle window (BUG-035) is what decides
            // whether that produces one rebuild or two, and either is acceptable here --
            // what is not acceptable is a lost file.
            session.inject_device_error(kDeviceRemoved);
            session.inject_device_error(kAccessLost);
            applied.push_back({elapsed(), "DEVICE_REMOVED + ACCESS_LOST"});
            break;
        default:
            break;
        }

        std::this_thread::sleep_for(kFaultInterval);
    }

    const SessionStats stats = session.stats();
    const std::vector<MigrationRecord> migrations = session.migrations();
    const fc::Result<fc::mux::ValidationReport> stopped = session.stop();

    std::cout << "[ MEASURED ] " << applied.size() << " fault(s) injected over " << duration_s << " s:\n";
    for (const AppliedFault& fault : applied) {
        std::cout << "[ MEASURED ]   t+" << fault.at_seconds << " s  " << fault.what << "\n";
    }
    std::cout << "[ MEASURED ] captured " << stats.frames_captured << ", encoded " << stats.pipeline.frames_encoded
              << ", queue-dropped " << stats.pipeline.frames_queue_dropped << ", rebuilds " << stats.rebuilds
              << ", migrations recorded " << migrations.size() << "\n";

    // Every rebuild, individually. The aggregate counters say a rollover happened
    // somewhere in twenty; only the records say *which* one, whether it opened a new
    // file, and whether it gave up -- which is the difference between reading the engine
    // log and guessing at it.
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const MigrationRecord& record = migrations[i];
        std::cout << "[ MEASURED ]   rebuild " << (i + 1) << ": cause " << fc::pipeline::to_string(record.cause)
                  << ", same_file " << record.same_file << ", failed " << record.failed << ", gap "
                  << (static_cast<double>(record.gap_ns) / 1'000'000.0) << " ms, output "
                  << record.output.filename().string() << "\n";
    }
    std::cout << "[ MEASURED ] engine log: " << log_directory().string() << "\n" << std::flush;

    // --- the prime directive ------------------------------------------------
    //
    // Everything above is setup. This is the assertion SPEC.md §20.1 asks for, and it
    // is deliberately the *only* hard requirement: how the pipeline degraded is row 10's
    // business, and how fast it recovered is row 11's. What this tier adds is that
    // whatever combination of those happened, a file came out.
    ASSERT_TRUE(stopped.has_value()) << "the recording failed rather than degrading: "
                                     << fc::error_name(stopped.error());
    const fc::mux::ValidationReport& report = stopped.value();
    EXPECT_TRUE(report.valid) << report.detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(settings.output, options, media);

    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_EQ(media.video.codec_name, "h264");
    EXPECT_GT(media.video.frame_count, 0) << "the file holds no decodable frames";

    std::cout << "[ MEASURED ] file: valid " << report.valid << ", " << media.video.frame_count << " frames decoded, "
              << report.video_duration_seconds << " s of video\n"
              << std::flush;

    // --- non-vacuity --------------------------------------------------------
    //
    // A chaos run that injected nothing passes every assertion above while proving
    // nothing at all, and with a random schedule that is a real possibility rather than
    // a hypothetical. These are the guards that make a green result mean something.
    ASSERT_FALSE(applied.empty()) << "the schedule injected no faults; seed " << seed
                                  << " produced a run this tier cannot draw a conclusion from";
    EXPECT_GT(stats.rebuilds, 0u) << "faults were injected but the session never rebuilt -- "
                                     "either the injection seam or the watchdog is not working";
    EXPECT_GT(stats.frames_captured, 0) << "no frames were captured at all";
    // §20.1 names queue saturation among the injections this tier must perform. The
    // stall is what produces it, and a stall that produced no drops would mean the tier
    // silently covers four of the five named faults rather than five.
    EXPECT_GT(stats.pipeline.frames_queue_dropped, 0u)
        << "the injected disk stall never saturated a queue, so this run did not exercise "
           "§20.1's queue-saturation injection";
}

} // namespace
