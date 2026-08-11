// SPEC.md §20 row 6 -- "Low frame rate": `test_sustained_60fps`, "10 min under 90%
// GPU load from a synthetic stressor, assert dropped < 0.5%".
//
// GPU TIER.
//
// `ThroughputTest` already reports what the pipeline can do on an idle machine, and
// asserts loosely on purpose. Row 6 asks a different question: a recorder is used
// while the GPU is busy, because the thing worth recording is usually the thing
// loading the GPU. A pipeline that sustains 370 fps idle and drops frames the moment
// a game is running has not met row 6.
//
// ---------------------------------------------------------------------------
// How the load is produced
// ---------------------------------------------------------------------------
// `GpuStressor` runs the engine's own BGRA->NV12 compute shader over a 4K surface in
// a tight loop, on **its own D3D11 device on the same adapter**. Its own device
// deliberately: sharing the pipeline's device would serialise the stressor behind the
// pipeline's command queue and measure queue ordering rather than contention for the
// GPU.
//
// Reusing the shipped shader rather than writing a synthetic one keeps this test free
// of a runtime HLSL compile, and the work is genuinely heavy -- a compute dispatch
// over 8.3 million pixels per iteration.
//
// ---------------------------------------------------------------------------
// How the load is measured, and what that does and does not establish
// ---------------------------------------------------------------------------
// Row 6 names a load *level*, so the level is measured rather than asserted: the
// Windows `GPU Engine` PDH counters are read across all processes, which is the same
// source Task Manager's GPU column uses. When those counters are unavailable the case
// says so and reports the stressor's achieved dispatch rate instead.
//
// A number that is not claimed: this exercises contention on **one** adapter pair.
// SPEC.md row 6 lists "cross-adapter copy on the hot path" among its root causes, and
// that path is §5.3's, which is M7's work and not exercised here.

#include "core/capture/capture_frame.h"
#include "core/color/nv12_converter.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/health/health_monitor.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"
#include "core/util/thread_utils.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using Microsoft::WRL::ComPtr;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// Row 6's threshold.
constexpr double kMaxDropRatio = 0.005;

/// SPEC.md §20 row 6 asks for ten minutes. That is a real ten minutes and belongs to
/// an acceptance run, not to every `ctest` invocation, so it takes an override like
/// the other long cases (CLAUDE.md §3).
///
///     $env:FC_SUSTAINED_SECONDS = 600   # row 6's exit-criterion value
[[nodiscard]] int sustained_seconds() {
    std::size_t size = 0;
    char value[16] = {};
    if (getenv_s(&size, value, sizeof(value), "FC_SUSTAINED_SECONDS") == 0 && size > 1) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        // `strtol` rather than `atoi`, and the end pointer is checked: a typo in the
        // variable would otherwise silently run the 20 s form and the log would claim
        // the exit criterion had been met.
        if (end != value && parsed > 0 && parsed <= 86400) {
            return static_cast<int>(parsed);
        }
        std::cout << "[ MEASURED ] FC_SUSTAINED_SECONDS=\"" << value
                  << "\" is not a usable duration; using the default\n";
    }
    return 20;
}

// ---------------------------------------------------------------------------
// GPU load measurement
// ---------------------------------------------------------------------------

/// Total GPU engine utilisation across every process, from the same PDH counters Task
/// Manager reads.
///
/// Returns `std::nullopt` when the counter set is unavailable -- it depends on the
/// WDDM version and can be absent -- rather than reporting 0, because "not measured"
/// and "idle" must not look alike in the output.
class GpuUtilisation {
public:
    ~GpuUtilisation() {
        if (query_ != nullptr) {
            PdhCloseQuery(query_);
        }
    }

    GpuUtilisation() = default;
    GpuUtilisation(const GpuUtilisation&) = delete;
    GpuUtilisation& operator=(const GpuUtilisation&) = delete;
    GpuUtilisation(GpuUtilisation&&) = delete;
    GpuUtilisation& operator=(GpuUtilisation&&) = delete;

    /// Opens the query. False when the counters are not present on this machine.
    [[nodiscard]] bool open() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            return false;
        }
        // Every engine instance of every process. The instance names encode pid, LUID
        // and engine type; summing them is what produces a machine-wide figure.
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return false;
        }
        // A rate counter needs two collections to produce a value; this is the first.
        return PdhCollectQueryData(query_) == ERROR_SUCCESS;
    }

    /// Busiest **physical engine**, in percent.
    ///
    /// The counter's instances are per process *and* per engine
    /// (`pid_1234_luid_..._phys_0_eng_0_engtype_3D`), so a machine-wide figure needs
    /// two steps: sum the processes sharing an engine, then take the busiest engine.
    /// That is what Task Manager's GPU column reports and therefore what row 6's "90%
    /// GPU load" can be read against.
    ///
    /// Summing every instance instead -- the obvious first thing to write -- produces a
    /// number that exceeds 100% on any machine with more than one engine doing
    /// anything, and measured 60% on a completely idle desktop. It is not wrong, it is
    /// just not a percentage of anything.
    [[nodiscard]] std::optional<double> sample() {
        if (query_ == nullptr || PdhCollectQueryData(query_) != ERROR_SUCCESS) {
            return std::nullopt;
        }

        DWORD size = 0;
        DWORD count = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, nullptr);
        if (status != PDH_MORE_DATA || size == 0) {
            return std::nullopt;
        }

        buffer_.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, items);
        if (status != ERROR_SUCCESS) {
            return std::nullopt;
        }

        // Key on everything from `_phys_` onward -- the physical engine and its type,
        // with the pid and LUID prefix dropped -- so the processes sharing an engine
        // add together.
        std::vector<std::pair<std::wstring, double>> engines;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS || items[i].szName == nullptr) {
                continue;
            }
            const std::wstring name{items[i].szName};
            const std::size_t phys = name.find(L"_phys_");
            const std::wstring key = phys == std::wstring::npos ? name : name.substr(phys);

            const auto existing =
                std::ranges::find_if(engines, [&key](const auto& entry) { return entry.first == key; });
            if (existing == engines.end()) {
                engines.emplace_back(key, items[i].FmtValue.doubleValue);
            } else {
                existing->second += items[i].FmtValue.doubleValue;
            }
        }
        if (engines.empty()) {
            return std::nullopt;
        }

        const auto busiest =
            std::ranges::max_element(engines, [](const auto& a, const auto& b) { return a.second < b.second; });
        // Clamped: the per-process figures are sampled independently and can sum a
        // little past 100 on a fully saturated engine, which would otherwise read as a
        // measurement error rather than as "saturated".
        return std::min(busiest->second, 100.0);
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    std::vector<std::byte> buffer_;
};

// ---------------------------------------------------------------------------
// The stressor
// ---------------------------------------------------------------------------

/// Occupies the GPU with real compute work on its own device.
class GpuStressor {
public:
    ~GpuStressor() {
        stop();
    }

    GpuStressor() = default;
    GpuStressor(const GpuStressor&) = delete;
    GpuStressor& operator=(const GpuStressor&) = delete;
    GpuStressor(GpuStressor&&) = delete;
    GpuStressor& operator=(GpuStressor&&) = delete;

    /// `dispatches_per_flush` sets how much work is queued before the GPU is allowed
    /// to be waited on. Larger values load the GPU harder and make the stressor less
    /// responsive to `stop`.
    [[nodiscard]] fc::Result<void> start(const fc::gpu::AdapterId& adapter, int edge, int dispatches_per_flush) {
        auto device = fc::test::device_for_adapter(adapter);
        if (!device.has_value()) {
            return device.error();
        }
        device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(device).value());

        // 4K, so one dispatch is 8.3 million pixels of real compute.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(edge);
        desc.Height = static_cast<UINT>(edge * 9 / 16);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // SPEC.md §6 / CLAUDE.md §8: `_UNORM`, never `_UNORM_SRGB`. Irrelevant to the
        // load but the converter refuses anything else, and rightly.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->device()->CreateTexture2D(&desc, nullptr, &source_))) {
            return fc::FcError::GPU_TEXTURE_CREATE_FAILED;
        }

        fc::color::ConverterSettings settings;
        settings.width = static_cast<int>(desc.Width);
        settings.height = static_cast<int>(desc.Height);
        if (const fc::Result<void> initialized = converter_.initialize(device_->device(), settings);
            !initialized.has_value()) {
            return initialized.error();
        }

        device_->device()->GetImmediateContext(&context_);
        if (context_.Get() == nullptr) {
            return fc::FcError::GPU_DEVICE_CREATE_FAILED;
        }

        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this, dispatches_per_flush] { loop(dispatches_per_flush); });
        return fc::ok();
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint64_t dispatches() const noexcept {
        return dispatches_.load(std::memory_order_relaxed);
    }

private:
    void loop(int dispatches_per_flush) {
        fc::set_thread_name("fc-test-stressor");
        while (running_.load(std::memory_order_acquire)) {
            for (int i = 0; i < dispatches_per_flush; ++i) {
                if (converter_.convert(context_.Get(), source_.Get()).has_value()) {
                    dispatches_.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Force the queued work through, so the loop does not run ahead of the GPU
            // building an unbounded command list -- which would put the pressure on
            // memory rather than on the GPU.
            context_->Flush();
        }
        fc::clear_thread_name();
    }

    std::unique_ptr<fc::gpu::D3dDevice> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> source_;
    fc::color::Nv12Converter converter_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dispatches_{0};
};

// ---------------------------------------------------------------------------

struct RunResult {
    fc::pipeline::PipelineStats stats;
    fc::pipeline::PipelineHealth health;
    fc::mux::ValidationReport report;
    std::optional<fc::FcError> stop_error;
    std::filesystem::path path;
    int submitted = 0;
    double elapsed_seconds = 0.0;
    /// GPU utilisation samples, in percent, or empty when PDH had nothing.
    std::vector<double> utilisation;
};

class SustainedTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("sustained");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "sustained00001";
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

    static const AdapterInfo* encoding_adapter() {
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software && adapter.can_encode()) {
                return &adapter;
            }
        }
        return nullptr;
    }

    /// Records for `seconds`, feeding at wall-clock 60 fps.
    ///
    /// Paced, not flooded, and that is the whole design of the case. Row 6 is about a
    /// 60 fps capture keeping up; a feeder that pushes as fast as it can would drop
    /// frames on an idle machine and prove nothing about load.
    static void record(const char* name, int seconds, GpuStressor* stressor, RunResult& out) {
        const AdapterInfo* encoder = encoding_adapter();
        ASSERT_NE(encoder, nullptr) << "no adapter reports an H.264 encoder";

        auto device = fc::test::device_for_adapter(encoder->id);
        ASSERT_TRUE(device.has_value());

        const int frames = seconds * kFps;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);
        // So the feeder can actually hold the 60 fps it paces itself at. Rendering a
        // 1080p pattern per acquire could not: row 6's ten-minute run fed at 57.2 fps
        // and took 629 s to deliver 600 s of frames, which understated the load the row
        // asks for. Frame identity is not asserted here, so a repeating barcode is free.
        source_settings.prerendered_frames = 8;

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(device.value().device(), fc::capture::CaptureTarget{}).has_value());

        out.path = dir_->path() / (std::string{name} + ".mkv");

        fc::pipeline::PipelineSettings settings;
        settings.output = out.path;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;
        settings.encoder_name = encoder->encode.encoder_name;
        settings.pool_bind_flags = encoder->encode.nv12_pool_bind_flags;

        GpuUtilisation utilisation;
        const bool have_pdh = utilisation.open();

        fc::pipeline::VideoPipeline pipeline;
        ASSERT_TRUE(pipeline.start(device.value().device(), settings).has_value());

        const auto started = std::chrono::steady_clock::now();
        auto next_sample = started + std::chrono::seconds{1};

        for (;;) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            if (!frame.has_value()) {
                break;
            }
            const auto due = started + (std::chrono::microseconds{1'000'000 / kFps} * out.submitted);
            std::this_thread::sleep_until(due);

            const fc::Result<void> submission = pipeline.submit(frame.value());
            ASSERT_TRUE(submission.has_value())
                << "submit failed rather than degrading: " << fc::error_name(submission.error());
            ++out.submitted;
            source.release(frame.value());

            if (have_pdh && std::chrono::steady_clock::now() >= next_sample) {
                next_sample += std::chrono::seconds{1};
                if (const std::optional<double> value = utilisation.sample(); value.has_value()) {
                    out.utilisation.push_back(*value);
                }
            }
        }

        out.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        out.health = pipeline.health();

        const auto report = pipeline.stop();
        source.stop();
        out.stats = pipeline.stats();

        if (report.has_value()) {
            out.report = report.value();
        } else {
            out.stop_error = report.error();
        }

        static_cast<void>(stressor);
    }

    static double drop_ratio(const RunResult& out) {
        return out.submitted == 0
                   ? 0.0
                   : static_cast<double>(out.stats.frames_queue_dropped) / static_cast<double>(out.submitted);
    }

    static void report_measurements(const char* label, const RunResult& out, const fc::test::DecodedMedia& media,
                                    std::uint64_t dispatches) {
        std::cout << "[ MEASURED ] " << label << ", " << out.elapsed_seconds << " s:\n"
                  << "[ MEASURED ]   submitted " << out.submitted << " at "
                  << static_cast<double>(out.submitted) / out.elapsed_seconds << " fps, encoded "
                  << out.stats.frames_encoded << ", queue-dropped " << out.stats.frames_queue_dropped << " ("
                  << (drop_ratio(out) * 100.0) << "% against row 6's 0.5% limit)\n"
                  << "[ MEASURED ]   duplicates " << out.stats.duplicates_emitted << ", paced out "
                  << out.stats.frames_paced_out << ", worst staleness " << out.stats.worst_staleness_ns / 1000
                  << " us, mean " << out.stats.mean_staleness_ns / 1000 << " us\n"
                  << "[ MEASURED ]   rung " << static_cast<int>(out.health.rung) << " ("
                  << fc::health::to_string(out.health.rung) << "), transitions " << out.health.rung_transitions
                  << ", retimes " << out.health.retimes << ", write p99 " << out.health.disk_write_p99_ns / 1000
                  << " us\n"
                  << "[ MEASURED ]   file: valid " << out.report.valid << ", " << media.video.frame_count
                  << " frames decoded, video " << out.report.video_duration_seconds << " s\n";

        if (dispatches > 0) {
            std::cout << "[ MEASURED ]   stressor: " << dispatches << " 4K compute dispatches ("
                      << static_cast<double>(dispatches) / out.elapsed_seconds << "/s)\n";
        }

        if (out.utilisation.empty()) {
            std::cout << "[ MEASURED ]   GPU utilisation: the `GPU Engine` PDH counters returned nothing on this "
                         "machine, so the load *level* row 6 names is unmeasured here\n";
            return;
        }
        std::vector<double> sorted = out.utilisation;
        std::ranges::sort(sorted);
        const double median = sorted[sorted.size() / 2];
        std::cout << "[ MEASURED ]   busiest GPU engine, all processes: median " << median << "%, min "
                  << sorted.front() << "%, max " << sorted.back() << "% over " << sorted.size() << " samples\n";
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> SustainedTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> SustainedTest::topology_;

// ---------------------------------------------------------------------------
// Row 6's exit criterion
// ---------------------------------------------------------------------------

TEST_F(SustainedTest, SixtyFpsIsSustainedUnderHeavyGpuLoadWithFewerThanHalfAPercentDropped) {
    const AdapterInfo* encoder = encoding_adapter();
    ASSERT_NE(encoder, nullptr);

    GpuStressor stressor;
    // 3840-wide 16:9 at eight dispatches per flush. Calibrated on the reference rig to
    // load the GPU heavily while leaving the recorder able to run -- which is the point:
    // a stressor that starves the encoder entirely tests nothing about a recorder, it
    // tests what happens when the GPU is unavailable.
    ASSERT_TRUE(stressor.start(encoder->id, 3840, 8).has_value()) << "the GPU stressor would not start";

    // Let the load establish before the recording starts, so the first seconds are not
    // measured against a ramp.
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    RunResult out;
    ASSERT_NO_FATAL_FAILURE(record("loaded", sustained_seconds(), &stressor, out));
    const std::uint64_t dispatches = stressor.dispatches();
    stressor.stop();

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(out.path, options, media);

    report_measurements("1080p60 under GPU load", out, media, dispatches);

    // The stressor has to have done something, or "under load" is a claim about
    // nothing. Asserted rather than assumed, because a converter that failed to
    // initialise would leave this case looking like a pass.
    EXPECT_GT(dispatches, 0u) << "the stressor never dispatched; this run was not under load";

    // Row 6's *precondition*, enforced and not merely printed. Where PDH gives a
    // figure, the load has to actually be at the level the row names -- a drop rate
    // under 0.5% on an idle GPU is not evidence of anything.
    if (!out.utilisation.empty()) {
        std::vector<double> sorted = out.utilisation;
        std::ranges::sort(sorted);
        const double median = sorted[sorted.size() / 2];
        EXPECT_GE(median, 90.0) << "the busiest GPU engine sat at " << median
                                << "%, below row 6's 90%; the stressor needs recalibrating for this machine and the "
                                   "drop figure above understates the load the row asks for";
    }

    // Row 6's assertion.
    EXPECT_LT(drop_ratio(out), kMaxDropRatio) << "dropped " << out.stats.frames_queue_dropped << " of " << out.submitted
                                              << " frames under load, above row 6's 0.5% limit";

    // And the file that came out of it. A recording that meets the drop budget by
    // producing an unplayable file has not met row 6.
    ASSERT_FALSE(out.stop_error.has_value()) << fc::error_name(*out.stop_error);
    EXPECT_TRUE(out.report.valid) << out.report.detail;
    ASSERT_TRUE(media.opened) << media.detail;

    // A paced 60 fps feed for N seconds should put N*60 frames in the file. The
    // tolerance is one frame: the feed loop stops on the source's frame limit, and
    // whether the final frame's slot closes before `stop` is a matter of microseconds.
    EXPECT_NEAR(static_cast<double>(media.video.frame_count), static_cast<double>(out.submitted), 1.0);

    // Row 6 is "low frame rate", so the rate itself is asserted and not only the drop
    // count -- a pipeline that stayed under the drop budget by falling to rung 3 and
    // halving the capture rate has failed row 6 while passing its headline number.
    EXPECT_EQ(out.health.retimes, 0u) << "the ladder halved the capture rate; 60 fps was not sustained";
    EXPECT_NE(out.health.rung, fc::health::Rung::FrameDropsSustained);
}

// The unloaded control. Without it, a drop rate under 0.5% says nothing about load
// tolerance -- it might simply be what this machine does, load or no load.
TEST_F(SustainedTest, TheSameRecordingWithNoGpuLoadDropsNothing) {
    RunResult out;
    ASSERT_NO_FATAL_FAILURE(record("unloaded", sustained_seconds(), nullptr, out));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(out.path, options, media);

    report_measurements("1080p60 with no added load", out, media, 0);

    EXPECT_EQ(out.stats.frames_queue_dropped, 0u);
    EXPECT_EQ(out.health.rung, fc::health::Rung::Nominal);
    ASSERT_FALSE(out.stop_error.has_value()) << fc::error_name(*out.stop_error);
    EXPECT_TRUE(out.report.valid) << out.report.detail;
    EXPECT_NEAR(static_cast<double>(media.video.frame_count), static_cast<double>(out.submitted), 1.0);
}

} // namespace
