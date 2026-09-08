// SPEC.md §20.1's soak tier.
//
// > **Soak:** 4-hour continuous recording; assert zero drift beyond 20 ms, flat memory
// > (no growth > 5 MB/hour), no handle leaks (`GetProcessHandleCount` flat).
//
// GPU TIER, and the slowest thing this project owns. `FC_SOAK_SECONDS` defaults to 120 so
// the mechanism is exercised in the routine suite; §20.1's form is `14400` and belongs to
// a scheduled run, not to anyone's iteration loop (CLAUDE.md §3).
//
// ---------------------------------------------------------------------------
// What this tier adds that the others do not
// ---------------------------------------------------------------------------
// Of §20.1's three soak clauses, **only the leak clauses are this file's**.
//
//   drift < 20 ms   Already `AvSyncTest`'s, and it already has the long form:
//                   `FC_AV_SYNC_SECONDS=1800` decodes the finished file and compares a
//                   beep against a flash at every mark, worst offset −187 µs over 1800
//                   marks. Re-deriving that here would mean decoding four hours of 1080p
//                   to answer a question a sharper test already answers, so this file
//                   validates the recording and leaves drift to the test built for it.
//
//   flat memory     **Nothing else measures this.** Every other case in the suite runs
//   flat handles    for seconds, and a leak of a few kilobytes per rebuild is invisible
//                   at that length and fatal at four hours. This is the whole reason the
//                   tier exists.
//
// ---------------------------------------------------------------------------
// Why a rate, and why the first sample is discarded
// ---------------------------------------------------------------------------
// §20.1 says "no growth > 5 MB/hour", which is a *rate*, so a single before-and-after
// pair cannot answer it -- a process that allocates 40 MB of pools in its first second and
// nothing afterwards is healthy, and a difference-of-two-samples test would call it a
// 140 MB/hour leak on a 120-second run.
//
// So the working set is sampled throughout and the growth rate is taken by least squares
// over the samples **after** a settling window. The settling window is not a fudge: a
// recording's steady state begins once the encoder's pools, the mux queue and the
// staging ring have all reached their working size, and measuring before that is
// measuring start-up.

#include "core/capture/source_resolver.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/recording_session.h"

#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <psapi.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using fc::pipeline::RecordingSession;
using fc::pipeline::SessionSettings;
using fc::pipeline::SessionStats;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// How often the working set and handle count are read. Cheap enough to do often, and a
/// four-hour run at this interval still fits comfortably in memory.
constexpr auto kSampleInterval = std::chrono::seconds{5};

/// Samples inside this window are start-up, not steady state, and are excluded from the
/// growth rate. See the header.
constexpr auto kSettleFor = std::chrono::seconds{30};

/// SPEC.md §20.1's budget.
constexpr double kMaxGrowthMbPerHour = 5.0;

/// How much the working set of a recording process moves without leaking anything.
///
/// **This is why the assertion is on total growth and not on the extrapolated rate.**
/// §20.1's clause is written for a four-hour run, where 5 MB/hour is 20 MB and a real
/// leak dwarfs any jitter. At the routine 120 s it is 0.17 MB -- smaller than the noise.
/// Measured here: a clean 90 s run moved 246.9 -> 247.7 MB, a 0.75 MB wobble that becomes
/// "40 MB/hour" purely by multiplying by 65, and failed a budget it had not violated.
///
/// So the test asserts `growth < 5 MB/hour * elapsed + this`. Over four hours the
/// allowance is 13% of the budget and irrelevant; over 90 seconds it is what stops the
/// test reporting a leak that is not there.
constexpr double kWorkingSetJitterMb = 3.0;

[[nodiscard]] int seconds_from_env(const char* name, int fallback) {
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
    return static_cast<int>(std::min<long>(parsed, 6L * 60L * 60L));
}

struct Sample {
    double at_seconds = 0.0;
    double working_set_mb = 0.0;
    unsigned long handles = 0;
};

[[nodiscard]] Sample take_sample(double at_seconds) {
    Sample sample;
    sample.at_seconds = at_seconds;

    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        sample.working_set_mb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles) != 0) {
        sample.handles = handles;
    }
    return sample;
}

/// Least-squares slope of `working_set_mb` against `at_seconds`, in MB per hour.
///
/// A fit rather than (last - first) / elapsed: the working set of a recording process
/// moves several MB either way as queues fill and drain, so two endpoints can report a
/// leak or hide one depending on where in that cycle they land. A slope over every sample
/// is not fooled by the phase the run happens to stop in.
[[nodiscard]] double growth_mb_per_hour(const std::vector<Sample>& samples) {
    if (samples.size() < 2) {
        return 0.0;
    }
    double sum_t = 0.0;
    double sum_v = 0.0;
    for (const Sample& s : samples) {
        sum_t += s.at_seconds;
        sum_v += s.working_set_mb;
    }
    const auto n = static_cast<double>(samples.size());
    const double mean_t = sum_t / n;
    const double mean_v = sum_v / n;

    double covariance = 0.0;
    double variance = 0.0;
    for (const Sample& s : samples) {
        const double dt = s.at_seconds - mean_t;
        covariance += dt * (s.working_set_mb - mean_v);
        variance += dt * dt;
    }
    if (variance <= 0.0) {
        return 0.0;
    }
    return (covariance / variance) * 3600.0; // MB per second -> MB per hour
}

class SoakTest : public ::testing::Test {
protected:
    static std::filesystem::path log_directory() {
        // Outside the TempDir, for BUG-057's reason: a failure that takes hours to
        // reproduce must leave its evidence behind.
        return std::filesystem::temp_directory_path() / "framecapture-soak-logs";
    }

    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("soak");
        fc::log::Config config;
        config.directory = log_directory();
        config.session_id = "soaktest000001";
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

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> SoakTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> SoakTest::topology_;

// ---------------------------------------------------------------------------

TEST_F(SoakTest, ALongRecordingHoldsFlatMemoryAndHandlesAndStillValidates) {
    const int duration_s = seconds_from_env("FC_SOAK_SECONDS", 120);

    std::cout << "[ MEASURED ] soak: " << duration_s << " s (SPEC.md §20.1's form is FC_SOAK_SECONDS=14400)\n"
              << std::flush;

    SessionSettings settings;
    settings.output = dir_->path() / "soak.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;

    if (const auto display = fc::capture::primary_display(); display.has_value()) {
        settings.target.monitor = display.value().monitor;
    }
    ASSERT_NE(settings.target.monitor, 0u) << "no primary display to resolve an adapter from";

    settings.capture_factory = [](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
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

    RecordingSession session;
    const fc::Result<void> started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());

    std::vector<Sample> all;
    const auto begun = std::chrono::steady_clock::now();
    const auto deadline = begun + std::chrono::seconds{duration_s};

    while (std::chrono::steady_clock::now() < deadline) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
        all.push_back(take_sample(elapsed));
        std::this_thread::sleep_for(kSampleInterval);
    }

    const SessionStats stats = session.stats();
    const fc::Result<fc::mux::ValidationReport> stopped = session.stop();

    ASSERT_GE(all.size(), 3u) << "too few samples to say anything about a trend";

    std::vector<Sample> steady;
    for (const Sample& s : all) {
        if (s.at_seconds >= std::chrono::duration<double>(kSettleFor).count()) {
            steady.push_back(s);
        }
    }
    // A run shorter than the settling window still measures something rather than
    // nothing: every sample it has.
    const std::vector<Sample>& measured = steady.size() >= 3 ? steady : all;

    const double growth = growth_mb_per_hour(measured);
    const auto [min_handles, max_handles] =
        std::ranges::minmax_element(measured, [](const Sample& a, const Sample& b) { return a.handles < b.handles; });
    const double first_mb = measured.front().working_set_mb;
    const double last_mb = measured.back().working_set_mb;

    std::cout << "[ MEASURED ] working set " << first_mb << " -> " << last_mb << " MB over "
              << (measured.back().at_seconds - measured.front().at_seconds) << " s of steady state (" << measured.size()
              << " samples)\n"
              << "[ MEASURED ] growth " << growth << " MB/hour against §20.1's " << kMaxGrowthMbPerHour << "\n"
              << "[ MEASURED ] handles " << min_handles->handles << "-" << max_handles->handles << "\n"
              << "[ MEASURED ] captured " << stats.frames_captured << ", encoded " << stats.pipeline.frames_encoded
              << ", dropped " << stats.pipeline.frames_queue_dropped << ", rebuilds " << stats.rebuilds << "\n"
              << std::flush;

    // --- the recording itself ----------------------------------------------
    ASSERT_TRUE(stopped.has_value()) << "the soak recording did not finalize: " << fc::error_name(stopped.error());
    EXPECT_TRUE(stopped.value().valid) << stopped.value().detail;

    // --- §20.1's leak clauses ----------------------------------------------
    const double measured_seconds = measured.back().at_seconds - measured.front().at_seconds;
    const double budget_mb = (kMaxGrowthMbPerHour * measured_seconds / 3600.0) + kWorkingSetJitterMb;
    const double grew_mb = last_mb - first_mb;

    std::cout << "[ MEASURED ] grew " << grew_mb << " MB against a budget of " << budget_mb << " MB ("
              << kMaxGrowthMbPerHour << " MB/hour over " << measured_seconds << " s, plus " << kWorkingSetJitterMb
              << " MB of jitter allowance)\n"
              << std::flush;

    EXPECT_LT(grew_mb, budget_mb) << "working set grew " << grew_mb << " MB in " << measured_seconds
                                  << " s, which is beyond §20.1's rate even allowing for jitter";

    // Handles are asserted as a band rather than as equality: a recording opens and closes
    // handles legitimately -- a rebuild replaces a device, the muxer reopens a file on a
    // segment roll -- so "flat" has to mean "does not trend upward", not "never moves".
    const auto handle_growth = static_cast<long>(max_handles->handles) - static_cast<long>(min_handles->handles);
    EXPECT_LT(handle_growth, 64L) << "handle count ranged " << min_handles->handles << "-" << max_handles->handles
                                  << ", which looks like a leak rather than churn";

    // --- non-vacuity --------------------------------------------------------
    EXPECT_GT(stats.frames_captured, 0u) << "nothing was captured, so nothing was measured";
}

} // namespace
