// Throughput measurements (SPEC.md §20 row 6's precondition).
//
// GPU TIER.
//
// These are *measurements*, not pass/fail gates. Every number the project has
// quoted for capture rate so far came from `SustainedCaptureToNv12StaysCorrect`,
// which reads every frame back to system memory to check for uniformity -- a full
// CPU/GPU sync per frame that the real pipeline never performs. That figure says
// what the test harness costs, not what the engine can do.
//
// Three things are unknown and each is answered by one case below:
//
//   1. What does capture + convert sustain with the readback removed?
//   2. Is the capture rate limited by our throughput, or quantised to the
//      display's vblank cadence? (48.02 fps on a 144 Hz panel is suspiciously
//      close to 144/3.)
//   3. What does the full M3 encode pipeline sustain, with no display coupling
//      at all?
//
// Assertions are deliberately loose -- a throughput threshold that fails on a
// busy machine is a test people learn to ignore. The numbers are printed; the
// assertions only catch a collapse.

#include "core/capture/capture_factory.h"
#include "core/capture/source_resolver.h"
#include "core/color/nv12_converter.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"

#include "adapter_device.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

/// Short by default so these can live in the normal GPU tier; `FC_RUN_SOAK=1`
/// lengthens them when a steadier number is wanted.
std::chrono::seconds measurement_window() {
    std::size_t size = 0;
    char value[8] = {};
    const bool soak = getenv_s(&size, value, sizeof(value), "FC_RUN_SOAK") == 0 && size > 0 && value[0] == '1';
    return soak ? std::chrono::seconds{30} : std::chrono::seconds{5};
}

std::int64_t qpc_now_ns() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const auto seconds = counter.QuadPart / frequency.QuadPart;
    const auto remainder = counter.QuadPart % frequency.QuadPart;
    return (seconds * 1'000'000'000) + ((remainder * 1'000'000'000) / frequency.QuadPart);
}

class ThroughputTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("throughput");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "throughputtest1";
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

    /// The adapter that owns the primary display -- the one capture must use.
    static const AdapterInfo* display_adapter(std::uint64_t monitor) {
        return topology_->topology().owner_of_monitor(monitor);
    }

    /// First adapter that reports an H.264 encoder.
    static const AdapterInfo* encoding_adapter() {
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software && adapter.can_encode()) {
                return &adapter;
            }
        }
        return nullptr;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> ThroughputTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> ThroughputTest::topology_;

// ---------------------------------------------------------------------------
// 1. Capture + convert, with no readback
// ---------------------------------------------------------------------------

TEST_F(ThroughputTest, CaptureAndConvertCeilingWithoutReadback) {
    const auto display = fc::capture::primary_display();
    ASSERT_TRUE(display.has_value());

    const AdapterInfo* owner = display_adapter(display.value().monitor);
    ASSERT_NE(owner, nullptr);

    auto device = fc::test::device_for_adapter(owner->id);
    ASSERT_TRUE(device.has_value());

    fc::capture::CaptureTarget target;
    target.monitor = display.value().monitor;

    auto capture = fc::capture::create_capture(fc::config::CaptureBackend::Auto, target);
    ASSERT_TRUE(capture.has_value());
    const std::unique_ptr<fc::capture::IScreenCapture> backend = std::move(capture).value();
    ASSERT_TRUE(backend->start(device.value().device(), target).has_value());

    fc::color::ConverterSettings settings;
    settings.width = display.value().width;
    settings.height = display.value().height;

    fc::color::Nv12Converter converter;
    ASSERT_TRUE(converter.initialize(device.value().device(), settings).has_value());

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device.value().device()->GetImmediateContext(&context);
    ASSERT_NE(context.Get(), nullptr);

    const auto window = measurement_window();
    const auto started = std::chrono::steady_clock::now();
    std::int64_t frames = 0;
    std::int64_t converted = 0;

    while (std::chrono::steady_clock::now() - started < window) {
        auto frame = backend->acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            continue;
        }
        ++frames;
        // The real pipeline's per-frame GPU work, and nothing else. No staging
        // copy, no Map, no disk.
        if (converter.convert(context.Get(), static_cast<ID3D11Texture2D*>(frame.value().texture)).has_value()) {
            ++converted;
        }
        backend->release(frame.value());
    }

    backend->stop();

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "[ MEASURED ] capture+convert, no readback: " << static_cast<double>(converted) / elapsed << " fps ("
              << converted << " frames in " << elapsed << " s, " << frames - converted << " conversion failures)\n";

    // WGC is change-driven: it produces a frame when the desktop composites, and
    // an idle desktop composites rarely or not at all. During an unattended test
    // run nothing is moving, so a low count here is a statement about the
    // environment, not about the engine -- and asserting a rate would make this a
    // test that fails depending on whether someone happened to wiggle the mouse.
    //
    // What *is* the engine's business: every frame that did arrive converted.
    EXPECT_EQ(frames, converted) << "some frames failed conversion";
    if (frames == 0) {
        std::cout << "[ MEASURED ]   no frames arrived -- desktop was idle for the whole window; "
                     "this measures nothing about throughput\n";
    }
}

// ---------------------------------------------------------------------------
// 2. Where do capture timestamps actually land?
// ---------------------------------------------------------------------------

// If the inter-arrival gaps cluster tightly on a multiple of the panel's vblank
// interval, the capture rate is quantised to the display rather than limited by
// our throughput -- which is a completely different problem with a completely
// different fix. Scattered gaps mean the opposite.
TEST_F(ThroughputTest, CaptureInterArrivalDistribution) {
    const auto display = fc::capture::primary_display();
    ASSERT_TRUE(display.has_value());

    const AdapterInfo* owner = display_adapter(display.value().monitor);
    ASSERT_NE(owner, nullptr);

    auto device = fc::test::device_for_adapter(owner->id);
    ASSERT_TRUE(device.has_value());

    fc::capture::CaptureTarget target;
    target.monitor = display.value().monitor;

    auto capture = fc::capture::create_capture(fc::config::CaptureBackend::Auto, target);
    ASSERT_TRUE(capture.has_value());
    const std::unique_ptr<fc::capture::IScreenCapture> backend = std::move(capture).value();
    ASSERT_TRUE(backend->start(device.value().device(), target).has_value());

    const auto window = measurement_window();
    const auto started = std::chrono::steady_clock::now();

    std::vector<std::int64_t> gaps_us;
    std::int64_t previous_qpc = 0;

    while (std::chrono::steady_clock::now() - started < window) {
        auto frame = backend->acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            continue;
        }
        const auto qpc = static_cast<std::int64_t>(frame.value().qpc_ns);
        if (previous_qpc != 0) {
            gaps_us.push_back((qpc - previous_qpc) / 1000);
        }
        previous_qpc = qpc;
        backend->release(frame.value());
    }
    backend->stop();

    // Same environmental caveat as above: an idle desktop yields few or no gaps.
    // Reported rather than asserted, because "the desktop did not change" is a
    // legitimate state of the machine, not a defect in the capture path.
    if (gaps_us.size() < 20) {
        std::cout << "[ MEASURED ] capture inter-arrival: only " << gaps_us.size()
                  << " samples -- desktop was too idle to characterise. Re-run with something animating "
                     "on screen for a meaningful distribution.\n";
        SUCCEED();
        return;
    }

    // Bucket to the nearest millisecond; a vblank-quantised source collapses into
    // one or two buckets, a throughput-limited one spreads out.
    std::map<std::int64_t, int> histogram;
    for (const std::int64_t gap : gaps_us) {
        ++histogram[(gap + 500) / 1000];
    }

    std::vector<std::int64_t> sorted = gaps_us;
    std::ranges::sort(sorted);
    const std::int64_t median = sorted[sorted.size() / 2];

    std::cout << "[ MEASURED ] capture inter-arrival: " << gaps_us.size() << " samples, median " << median << " us ("
              << (median > 0 ? 1'000'000.0 / static_cast<double>(median) : 0.0) << " fps)\n";
    for (const auto& [milliseconds, count] : histogram) {
        const double share = 100.0 * count / static_cast<double>(gaps_us.size());
        if (share >= 1.0) {
            std::cout << "[ MEASURED ]   " << milliseconds << " ms: " << count << " (" << share << "%)\n";
        }
    }

    // The dominant bucket's share is the tell: high means quantised, low means
    // spread. Reported rather than asserted, because both are legitimate states.
    const auto dominant =
        std::ranges::max_element(histogram, [](const auto& a, const auto& b) { return a.second < b.second; });
    std::cout << "[ MEASURED ]   dominant bucket " << dominant->first << " ms holds "
              << (100.0 * dominant->second / static_cast<double>(gaps_us.size())) << "% of samples\n";

    EXPECT_GT(median, 0);
}

// ---------------------------------------------------------------------------
// 3. The full encode pipeline, with no display coupling
// ---------------------------------------------------------------------------

// The number that decides whether queue pressure is a real concern on a 144 Hz
// panel or a theoretical one. The synthetic source produces on demand, so this
// measures convert + encode + mux and nothing else.
TEST_F(ThroughputTest, EncodePipelineThroughput) {
    const AdapterInfo* encoder = encoding_adapter();
    ASSERT_NE(encoder, nullptr) << "no adapter reports an H.264 encoder";

    auto device = fc::test::device_for_adapter(encoder->id);
    ASSERT_TRUE(device.has_value());

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device.value().device(), fc::capture::CaptureTarget{}).has_value());

    fc::pipeline::PipelineSettings settings;
    settings.output = dir_->path() / "throughput.mkv";
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.encoder_name = encoder->encode.encoder_name;
    settings.pool_bind_flags = encoder->encode.nv12_pool_bind_flags;

    fc::pipeline::VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(device.value().device(), settings).has_value());

    const auto window = measurement_window();
    const auto started = std::chrono::steady_clock::now();
    const std::int64_t t0 = qpc_now_ns();
    std::int64_t submitted = 0;

    while (std::chrono::steady_clock::now() - started < window) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            break;
        }
        // Stamp with wall-clock QPC rather than the source's synthetic clock, so
        // the pacer sees the rate the pipeline is actually achieving. Otherwise
        // the pacer would quantise against a 60 fps fiction and the measurement
        // would describe the synthetic timeline instead of the hardware.
        fc::capture::CaptureFrame stamped = frame.value();
        stamped.qpc_ns = static_cast<std::uint64_t>(qpc_now_ns());
        EXPECT_TRUE(pipeline.submit(stamped).has_value());
        ++submitted;
        source.release(frame.value());
    }

    const double elapsed_capture = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const fc::pipeline::PipelineStats stats = pipeline.stats();
    const auto report = pipeline.stop();
    source.stop();

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    const double submit_rate = static_cast<double>(submitted) / elapsed_capture;
    const auto wall_ns = static_cast<double>(qpc_now_ns() - t0);

    std::cout << "[ MEASURED ] encode pipeline: " << submit_rate << " fps submitted (" << submitted << " frames in "
              << elapsed_capture << " s)\n"
              << "[ MEASURED ]   encoded " << stats.frames_encoded << ", paced out " << stats.frames_paced_out
              << ", queue-dropped " << stats.frames_queue_dropped << ", duplicates " << stats.duplicates_emitted << "\n"
              << "[ MEASURED ]   muxed " << stats.packets_muxed << " packets, "
              << stats.bytes_written / (1024ULL * 1024) << " MiB, output duration " << report.value().duration_seconds
              << " s over " << wall_ns / 1e9 << " s wall\n"
              << "[ MEASURED ]   headroom vs 60 fps: " << submit_rate / 60.0 << "x, vs 144 fps: " << submit_rate / 144.0
              << "x\n";

    // The concern this measurement exists to settle: if the pipeline keeps up,
    // the encode queue never fills and its drop-oldest policy never competes with
    // the pacer's deterministic one.
    std::cout << "[ MEASURED ]   queue-dropped share: "
              << (submitted > 0
                      ? 100.0 * static_cast<double>(stats.frames_queue_dropped) / static_cast<double>(submitted)
                      : 0.0)
              << "%\n";

    EXPECT_GT(submitted, 0);
    EXPECT_EQ(stats.convert_failures, 0u);
    EXPECT_EQ(stats.encode_failures, 0u);
}

} // namespace
