// SPEC.md §20 row 11 -- "Mid-recording GPU switch breaks recording".
//
// GPU TIER.
//
// Row 11 and §5.4 were amended on 2026-07-30 after `MigrationParameterSetTest`
// measured that a cross-vendor handover cannot stay in one file. The row now has two
// outcomes and both are correct:
//
//   same adapter       parameter sets are byte-identical, so the encoder is rebuilt
//                      underneath the same muxer and the file continues.
//   different adapter  parameter sets differ, so `rebuild_device` **refuses** and the
//                      caller closes the segment and opens the next.
//
// This file covers the pipeline half of that -- the part that decides which outcome
// applies and keeps the timeline correct through it. The session-level orchestration
// (pause capture, re-select, resume, gap accounting) is `RecordingSession`'s, and its
// cases live alongside it.
//
// ---------------------------------------------------------------------------
// Why the device loss is injected rather than provoked
// ---------------------------------------------------------------------------
// SPEC.md §20 row 11 offers "driver restart via `pnputil` or a test-only injected
// `DXGI_ERROR_DEVICE_REMOVED`". The `pnputil` form takes the display down for seconds,
// cannot be aimed at one adapter, and would make the suite unrunnable on a machine
// someone is using -- so the injected form is the one the spec expects here.
//
// Nothing about the *response* is faked. `rebuild_device` runs its production path
// end to end: flush, tear down, re-create on the new device, compare parameter sets,
// reserve the decode-order gap, restart the venc thread. What the test supplies is the
// trigger, which is exactly the seam a driver would supply it at.

#include "core/capture/capture_frame.h"
#include "core/gpu/device_watcher.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

class GpuMigrationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("migration");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "gpumigration01";
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

    static std::vector<const AdapterInfo*> encoding_adapters() {
        std::vector<const AdapterInfo*> found;
        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class != AdapterClass::Software && adapter.can_encode()) {
                found.push_back(&adapter);
            }
        }
        return found;
    }

    /// Feeds `frames` frames, waiting for the pipeline so nothing is dropped and the
    /// decoded frame count is a statement about the migration rather than about load.
    ///
    /// **Timestamps are stamped here, not taken from the source**, and that is the
    /// point of the helper. A rebuild forces a new `SyntheticSource` -- the old one's
    /// textures belong to the dead device -- and each source starts its own synthetic
    /// epoch at `qpc_now`. Since this feeds as fast as it can, 60 frames of *synthetic*
    /// time pass in a fraction of that in wall clock, so a fresh source's frame 0 lands
    /// **behind** the pacer's write head and is dropped as "the source outran the cap".
    /// Measured that way: 76 of 120 frames survived, which says nothing about migration
    /// and everything about two unsynchronised clocks.
    ///
    /// One continuous grid across the whole recording is what the migration is supposed
    /// to preserve, so the test supplies one.
    static void feed(fc::pipeline::VideoPipeline& pipeline, fc::test::SyntheticSource& source, int frames,
                     int& submitted, std::int64_t epoch_ns) {
        for (int i = 0; i < frames; ++i) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            ASSERT_TRUE(frame.has_value());
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns =
                static_cast<std::uint64_t>(epoch_ns + ((static_cast<std::int64_t>(submitted) * 1'000'000'000) / kFps));
            for (int spins = 0; spins < 2'000'000; ++spins) {
                const fc::pipeline::PipelineStats stats = pipeline.stats();
                const std::uint64_t settled =
                    stats.frames_encoded + stats.frames_paced_out + stats.frames_queue_dropped;
                if (static_cast<std::uint64_t>(submitted) - std::min(settled, static_cast<std::uint64_t>(submitted)) <
                    4) {
                    break;
                }
                std::this_thread::yield();
            }
            ASSERT_TRUE(pipeline.submit(stamped).has_value());
            ++submitted;
            source.release(frame.value());
        }
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> GpuMigrationTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> GpuMigrationTest::topology_;

// ---------------------------------------------------------------------------
// The same-adapter case: one continuous file
// ---------------------------------------------------------------------------

TEST_F(GpuMigrationTest, ARebuildOnTheSameAdapterKeepsOneContinuousDecodableFile) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    ASSERT_FALSE(adapters.empty()) << "no adapter reports an H.264 encoder";
    const AdapterInfo& adapter = *adapters.front();

    auto first_device = fc::test::device_for_adapter(adapter.id);
    ASSERT_TRUE(first_device.has_value());

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.prerendered_frames = 8;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(first_device.value().device(), fc::capture::CaptureTarget{}).has_value());

    const std::filesystem::path path = dir_->path() / "same_adapter.mkv";
    fc::pipeline::PipelineSettings settings;
    settings.output = path;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    settings.encoder_name = adapter.encode.encoder_name;
    settings.pool_bind_flags = adapter.encode.nv12_pool_bind_flags;

    fc::pipeline::VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(first_device.value().device(), settings).has_value());

    // One synthetic grid for the whole recording, spanning the rebuild. See `feed`.
    const std::int64_t epoch = fc::timing::qpc_now_ns();
    int submitted = 0;
    ASSERT_NO_FATAL_FAILURE(feed(pipeline, source, 60, submitted, epoch));

    // A *new* D3D device on the same adapter -- which is what a driver restart leaves
    // behind, and what makes this the same-adapter case rather than a no-op.
    source.stop();
    auto second_device = fc::test::device_for_adapter(adapter.id);
    ASSERT_TRUE(second_device.has_value());

    const auto rebuild_started = std::chrono::steady_clock::now();
    const fc::Result<void> rebuilt = pipeline.rebuild_device(
        second_device.value().device(), adapter.encode.encoder_name, adapter.encode.nv12_pool_bind_flags);
    const double rebuild_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rebuild_started).count();
    ASSERT_TRUE(rebuilt.has_value()) << "a same-adapter rebuild was refused: " << fc::error_name(rebuilt.error());

    // The synthetic source has to move to the new device too -- its textures belonged
    // to the old one, exactly as a real capture session's would.
    fc::test::SyntheticSource resumed;
    ASSERT_TRUE(resumed.configure(source_settings).has_value());
    ASSERT_TRUE(resumed.start(second_device.value().device(), fc::capture::CaptureTarget{}).has_value());
    ASSERT_NO_FATAL_FAILURE(feed(pipeline, resumed, 60, submitted, epoch));

    const fc::pipeline::PipelineStats stats_before_stop = pipeline.stats();
    const auto report = pipeline.stop();
    resumed.stop();
    const fc::pipeline::PipelineStats stats = pipeline.stats();

    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::cout << "[ MEASURED ] same-adapter rebuild on " << adapter.encode.encoder_name << ":\n"
              << "[ MEASURED ]   rebuild took " << rebuild_ms << " ms (budget "
              << (fc::gpu::kMigrationGapBudgetNs / 1'000'000) << " ms)\n"
              << "[ MEASURED ]   submitted " << submitted << ", encoded " << stats.frames_encoded << ", rebuilds "
              << stats.device_rebuilds << "\n"
              << "[ MEASURED ]   file: " << media.video.frame_count << " frames decoded, one file\n";
    static_cast<void>(stats_before_stop);

    // --- one continuous file ---------------------------------------------
    EXPECT_EQ(stats.device_rebuilds, 1u);
    EXPECT_GT(media.video.frame_count, 0);

    // Every frame submitted after the rebuild decoded, less the reserved discontinuity.
    // This is the assertion the cross-vendor case fails at 31 of 60 -- it is what
    // "continuous" means.
    //
    // The tolerance is the reserved window and nothing else: `reserve_discontinuity`
    // skips `max_b_frames` slots, and the frames whose natural indices land inside that
    // window are dropped by the pacer as "the source outran the cap". Two frames at
    // SPEC.md §9's `max_b_frames = 2`, 33 ms, against §5.4's 350 ms budget.
    const std::int64_t reserved = settings.video.max_b_frames;
    EXPECT_GE(media.video.frame_count, static_cast<std::int64_t>(submitted) - reserved - 1)
        << "frames went missing across the rebuild beyond the reserved discontinuity; the file is not continuous";

    // --- decode order survived -------------------------------------------
    ASSERT_GE(media.video.pts.size(), 2u);
    for (std::size_t i = 1; i < media.video.pts.size(); ++i) {
        ASSERT_GT(media.video.pts[i], media.video.pts[i - 1])
            << "PTS went backwards at frame " << i << "; the reserved discontinuity was too small";
    }

    // --- the gap is bounded ----------------------------------------------
    // SPEC.md §5.4's budget applies to the whole procedure. Measured here on the
    // pipeline's share of it, which is the part this class owns.
    EXPECT_LT(rebuild_ms, static_cast<double>(fc::gpu::kMigrationGapBudgetNs) / 1'000'000.0)
        << "the rebuild alone exceeded §5.4's total budget";
}

// ---------------------------------------------------------------------------
// The cross-adapter case: refused, so the caller can split the segment
// ---------------------------------------------------------------------------

// The guard that makes the amended §5.4 safe rather than merely written down. Without
// it the pipeline would happily continue a file whose second half does not decode --
// measured at 31 of 60 frames in `MigrationParameterSetTest`.
TEST_F(GpuMigrationTest, ARebuildOntoTheOtherAdapterIsRefusedRatherThanCorruptingTheFile) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    if (adapters.size() < 2) {
        GTEST_SKIP() << "this rig reports one encoding adapter; a cross-vendor refusal needs two";
    }

    auto first_device = fc::test::device_for_adapter(adapters[0]->id);
    ASSERT_TRUE(first_device.has_value());

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.prerendered_frames = 8;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(first_device.value().device(), fc::capture::CaptureTarget{}).has_value());

    const std::filesystem::path path = dir_->path() / "cross_adapter.mkv";
    fc::pipeline::PipelineSettings settings;
    settings.output = path;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    settings.encoder_name = adapters[0]->encode.encoder_name;
    settings.pool_bind_flags = adapters[0]->encode.nv12_pool_bind_flags;

    fc::pipeline::VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(first_device.value().device(), settings).has_value());

    // One synthetic grid for the whole recording, spanning the rebuild. See `feed`.
    const std::int64_t epoch = fc::timing::qpc_now_ns();
    int submitted = 0;
    ASSERT_NO_FATAL_FAILURE(feed(pipeline, source, 60, submitted, epoch));
    source.stop();

    auto other_device = fc::test::device_for_adapter(adapters[1]->id);
    ASSERT_TRUE(other_device.has_value());

    const fc::Result<void> rebuilt = pipeline.rebuild_device(
        other_device.value().device(), adapters[1]->encode.encoder_name, adapters[1]->encode.nv12_pool_bind_flags);

    std::cout << "[ MEASURED ] cross-adapter rebuild " << adapters[0]->encode.encoder_name << " -> "
              << adapters[1]->encode.encoder_name << ": "
              << (rebuilt.has_value() ? "accepted" : fc::error_name(rebuilt.error())) << "\n";

    ASSERT_FALSE(rebuilt.has_value())
        << "the pipeline accepted a rebuild onto an encoder with different parameter sets. It would have written a "
           "file whose remainder does not decode -- the outcome SPEC.md §5.4's amendment exists to prevent. If the "
           "two encoders now agree, `MigrationParameterSetTest` will say so and this expectation should be inverted "
           "deliberately";
    EXPECT_EQ(rebuilt.error(), fc::FcError::GPU_MIGRATION_FAILED);

    // The prime directive still holds through a refused rebuild: whatever was written
    // before it must finalize into a playable file. This is the half that matters most
    // -- a guard that protected the format by losing the recording would be no better
    // than the corruption.
    const auto report = pipeline.stop();
    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_GT(media.video.frame_count, 0) << "the file written before the refused rebuild is not decodable";
    std::cout << "[ MEASURED ]   the pre-refusal file finalized with " << media.video.frame_count
              << " decodable frames\n";
}

// ---------------------------------------------------------------------------
// Repetition -- the case that catches a leak per rebuild
// ---------------------------------------------------------------------------

TEST_F(GpuMigrationTest, RepeatedRebuildsStayWithinBudgetAndKeepTheTimelineMonotonic) {
    const std::vector<const AdapterInfo*> adapters = encoding_adapters();
    ASSERT_FALSE(adapters.empty());
    const AdapterInfo& adapter = *adapters.front();

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;
    source_settings.prerendered_frames = 8;

    auto device = fc::test::device_for_adapter(adapter.id);
    ASSERT_TRUE(device.has_value());

    const std::filesystem::path path = dir_->path() / "repeated.mkv";
    fc::pipeline::PipelineSettings settings;
    settings.output = path;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    settings.encoder_name = adapter.encode.encoder_name;
    settings.pool_bind_flags = adapter.encode.nv12_pool_bind_flags;

    fc::pipeline::VideoPipeline pipeline;
    ASSERT_TRUE(pipeline.start(device.value().device(), settings).has_value());

    // One synthetic grid for the whole recording, spanning every rebuild. See `feed`.
    const std::int64_t epoch = fc::timing::qpc_now_ns();
    constexpr int kRebuilds = 5;
    std::vector<fc::gpu::D3dDevice> devices;
    devices.reserve(kRebuilds + 1);
    devices.push_back(std::move(device).value());

    int submitted = 0;
    double worst_ms = 0.0;

    {
        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(devices.back().device(), fc::capture::CaptureTarget{}).has_value());
        ASSERT_NO_FATAL_FAILURE(feed(pipeline, source, 20, submitted, epoch));
        source.stop();
    }

    for (int i = 0; i < kRebuilds; ++i) {
        auto next = fc::test::device_for_adapter(adapter.id);
        ASSERT_TRUE(next.has_value()) << "device creation failed on rebuild " << i
                                      << "; a previous rebuild leaked something";
        devices.push_back(std::move(next).value());

        const auto started = std::chrono::steady_clock::now();
        const fc::Result<void> rebuilt = pipeline.rebuild_device(devices.back().device(), adapter.encode.encoder_name,
                                                                 adapter.encode.nv12_pool_bind_flags);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        ASSERT_TRUE(rebuilt.has_value()) << "rebuild " << i << " failed: " << fc::error_name(rebuilt.error());
        worst_ms = std::max(worst_ms, elapsed_ms);

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(devices.back().device(), fc::capture::CaptureTarget{}).has_value());
        ASSERT_NO_FATAL_FAILURE(feed(pipeline, source, 20, submitted, epoch));
        source.stop();
    }

    const auto report = pipeline.stop();
    const fc::pipeline::PipelineStats stats = pipeline.stats();
    ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::cout << "[ MEASURED ] " << kRebuilds << " rebuilds in one recording:\n"
              << "[ MEASURED ]   worst rebuild " << worst_ms << " ms, submitted " << submitted << ", decoded "
              << media.video.frame_count << " frames\n";

    EXPECT_EQ(stats.device_rebuilds, static_cast<std::uint64_t>(kRebuilds));
    EXPECT_LT(worst_ms, static_cast<double>(fc::gpu::kMigrationGapBudgetNs) / 1'000'000.0)
        << "a later rebuild was slower than the first; something accumulates per rebuild";

    for (std::size_t i = 1; i < media.video.pts.size(); ++i) {
        ASSERT_GT(media.video.pts[i], media.video.pts[i - 1]) << "PTS went backwards at frame " << i;
    }
}

} // namespace
