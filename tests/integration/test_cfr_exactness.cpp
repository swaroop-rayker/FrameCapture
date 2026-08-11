// SPEC.md §20 row 7 -- "Repeated / jerky frames", end to end.
//
// GPU TIER.
//
// `test_frame_pacer.cpp` already covers `timing::Pacer` exhaustively as a pure
// function. What it cannot cover is whether the grid the pacer computed is the grid
// that ends up in the file, and that is the whole of row 7: the defect is not a bad
// decision, it is a good decision that something downstream re-times. The
// container's timebase, the encoder's reordering and the muxer's rescale all get a
// vote, and every one of them has been observed to change PTS in some codebase.
//
// SPEC.md §20 row 7's test is `test_cfr_exactness`: "assert `frame_count ==
// duration_s * fps` exactly, and all PTS deltas identical".
//
// ---------------------------------------------------------------------------
// The second half of that sentence is container-dependent, and the spec does not
// say so
// ---------------------------------------------------------------------------
// `matroskaenc` forces a timebase of **1/1000 on every stream** -- it is not
// negotiable and the engine's 1/60000 request is overwritten during
// `avformat_write_header` (this is the same fact BUG-015 turned on). On a
// millisecond grid a 60 fps frame duration of 16.666… ms is not representable, so
// the deltas in an MKV file *cannot* be identical: they come out as 17, 17, 16,
// repeating. That is arithmetic, not a defect, and no amount of correctness in the
// pacer changes it.
//
// So this file asserts the strongest form each container can actually carry:
//
//   * **frame count** -- exact, in both containers. This is the half of row 7 that
//     is unconditional.
//   * **identical deltas** -- asserted exactly where the timebase can represent the
//     frame duration, which is MP4 with the engine's 1/60000.
//   * **bounded, non-accumulating quantisation** -- where it cannot, every PTS must
//     equal the exact rounding of a perfect grid, so error stays under half a tick
//     forever instead of drifting. A pacer with an accumulating step passes a count
//     assertion and fails this one, which is precisely the bug SPEC.md §7.2's
//     round-to-nearest exists to prevent.
//
// The distinction is checked from the file's declared timebase rather than from the
// container enum, so a future FFmpeg that stops forcing 1/1000 tightens this test
// automatically instead of silently keeping the weaker branch.

#include "core/capture/capture_frame.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
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

/// Five seconds of content. Long enough to cross several GOPs (SPEC.md §9 fixes the
/// keyframe interval at 2 s) so B-frame reordering and fragment boundaries are both
/// exercised, and short enough that the case runs in about a second.
constexpr int kFrames = 300;

class CfrExactnessTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("cfr");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "cfrexactness01";
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

    /// How the feeder paces itself, which is the term BUG-027 showed decides whether
    /// a backpressure case exercises anything at all.
    enum class Feed {
        /// Submit only when the encode queue has room. The right choice for the two
        /// exact-count cases: nothing is ever displaced, so rung 0 holds.
        BackPressure,
        /// One frame every 1/kFps of wall clock, regardless of what the pipeline is
        /// doing. The only mode in which a *slow drain* can be outrun by a rate that
        /// is a property of the test rather than of the machine.
        RealTime,
    };

    /// Records `kFrames` frames of the synthetic pattern into `container`.
    ///
    /// `stall_ns` slows the mux thread, which is what actually creates backpressure.
    /// Feeding fast alone does not: measured on this rig the encoder sustains ~370 fps
    /// at 1080p and the feeder cannot outrun it, so a flood with no stall drops nothing
    /// and exercises nothing. Slowing the one thread permitted to block on disk fills
    /// the mux queue, blocks the venc thread pushing into it, and overruns the 8-deep
    /// encode queue from behind -- which is the real chain a slow volume produces.
    static void record(fc::config::Container container, Feed feed, std::int64_t stall_ns,
                       fc::pipeline::PipelineStats& stats_out, std::filesystem::path& path_out,
                       fc::pipeline::PipelineHealth* health_out = nullptr) {
        const AdapterInfo* encoder = encoding_adapter();
        ASSERT_NE(encoder, nullptr) << "no adapter reports an H.264 encoder";

        auto device = fc::test::device_for_adapter(encoder->id);
        ASSERT_TRUE(device.has_value());

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = kFrames;
        // **The feeder has to be able to hold the rate it claims** (BUG-027). Rendering
        // a fresh 1080p BGRA pattern per `acquire` costs more than 16.67 ms on an
        // unoptimised build, so an absolute-deadline sleep returns immediately every
        // time and the feed silently runs at whatever the renderer manages. Measured on
        // Debug in a full tier without this: **12 fps against a 13 packets/s drain** —
        // the feeder was slower than the disk it was supposed to outrun, so nothing was
        // dropped and the case asserted nothing.
        //
        // Safe here because nothing in this file asserts frame *identity*: the barcodes
        // repeat every 8 frames with this set, and these cases assert counts and PTS.
        source_settings.prerendered_frames = 8;

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(device.value().device(), fc::capture::CaptureTarget{}).has_value());

        path_out = dir_->path() / (container == fc::config::Container::Mkv ? "cfr.mkv" : "cfr.mp4");

        fc::pipeline::PipelineSettings settings;
        settings.output = path_out;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = container;
        settings.encoder_name = encoder->encode.encoder_name;
        settings.pool_bind_flags = encoder->encode.nv12_pool_bind_flags;
        settings.injected_stall_ns = stall_ns;
        // One write in four. The period has to make the mux thread decisively slower
        // than the feeder, not merely slower on average: the feeder renders a 1080p
        // BGRA pattern per frame and manages only a few hundred a second, so a gentler
        // stall leaves both sides at about the same rate and the queues never build.
        // Measured with one-in-eight at 40 ms: write P99 46 ms, zero drops.
        settings.injected_stall_period = 4;

        fc::pipeline::VideoPipeline pipeline;
        ASSERT_TRUE(pipeline.start(device.value().device(), settings).has_value());

        int submitted = 0;
        const auto feed_began = std::chrono::steady_clock::now();
        for (;;) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            if (!frame.has_value()) {
                break; // frame_limit reached
            }
            if (feed == Feed::BackPressure) {
                await_capacity(pipeline, submitted);
            } else {
                // An absolute deadline, so a slow submit is caught up rather than
                // compounded -- the feeder's *average* rate is what has to beat the
                // drain, and a relative sleep would let it sag below it silently.
                std::this_thread::sleep_until(feed_began +
                                              (std::chrono::nanoseconds{1'000'000'000} * submitted / kFps));
            }
            // The source's own synthetic timestamps, deliberately. They are an exact
            // 1/60 s apart by construction, which is what makes "the grid in the file
            // is the grid the pacer computed" an assertion about the pipeline rather
            // than about how fast this machine happens to be.
            ASSERT_TRUE(pipeline.submit(frame.value()).has_value());
            ++submitted;
            source.release(frame.value());
        }
        ASSERT_EQ(submitted, kFrames);

        // Read before `stop`: the watchdog is retired first thing inside it, so this
        // is the last chance to see what the ladder made of the recording.
        if (health_out != nullptr) {
            *health_out = pipeline.health();
        }

        const auto report = pipeline.stop();
        source.stop();

        // After `stop`, not before. `stop` closes the encode queue, lets the venc
        // thread drain what is still in it and then flushes the encoder, so counters
        // read beforehand are short by whatever was in flight -- which cost one frame
        // and one confusing failure the first time this was written the other way.
        stats_out = pipeline.stats();

        ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
        ASSERT_TRUE(report.value().valid) << report.value().detail;
    }

    /// Blocks until the pipeline has fewer than the encode queue's depth of frames in
    /// flight, so a submit cannot displace an un-encoded one.
    static void await_capacity(const fc::pipeline::VideoPipeline& pipeline, int submitted) {
        // Deliberately below the queue's capacity of 8: leaving headroom is what makes
        // "nothing was dropped" a property of the feeder rather than a race.
        constexpr std::uint64_t kMaxInFlight = 4;
        for (int spins = 0; spins < 200000; ++spins) {
            const fc::pipeline::PipelineStats stats = pipeline.stats();
            const std::uint64_t settled = stats.frames_encoded + stats.frames_paced_out + stats.frames_queue_dropped +
                                          stats.frames_awaiting_epoch;
            if (static_cast<std::uint64_t>(submitted) - std::min(settled, static_cast<std::uint64_t>(submitted)) <
                kMaxInFlight) {
                return;
            }
            std::this_thread::yield();
        }
        FAIL() << "the pipeline never drained; it is wedged rather than merely slow";
    }

    static constexpr int kFps = 60;

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
};

std::unique_ptr<fc::test::TempDir> CfrExactnessTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> CfrExactnessTest::topology_;

/// Asserts row 7's timing claims against a decoded file.
///
/// Returns the delta set it found, so the caller can print it -- a failure here is
/// far easier to diagnose with the actual distribution in front of you.
void assert_grid_is_exact(const fc::test::DecodedVideo& video, int fps, std::set<std::int64_t>& deltas_out) {
    ASSERT_GE(video.pts.size(), 2u);
    ASSERT_GT(video.time_base.num, 0);
    ASSERT_GT(video.time_base.den, 0);

    for (std::size_t i = 1; i < video.pts.size(); ++i) {
        deltas_out.insert(video.pts[i] - video.pts[i - 1]);
    }

    // Ticks per frame on the file's own grid, as a rational. `representable` is the
    // question of whether row 7's "all PTS deltas identical" is arithmetically
    // possible in this container at this frame rate.
    const auto numerator = static_cast<std::int64_t>(video.time_base.den);
    const std::int64_t denominator = static_cast<std::int64_t>(video.time_base.num) * fps;
    const bool representable = numerator % denominator == 0;

    if (representable) {
        const std::int64_t ticks = numerator / denominator;
        EXPECT_EQ(deltas_out.size(), 1u) << "the timebase can represent 1/" << fps
                                         << " exactly, so every PTS delta must be identical";
        if (deltas_out.size() == 1) {
            EXPECT_EQ(*deltas_out.begin(), ticks);
        }
        return;
    }

    // The container cannot hold a uniform grid. What it must still hold is the exact
    // rounding of one: PTS_i == round(i * den / (num * fps)), measured from the first
    // frame. An accumulating pacer drifts away from this by a tick per frame and is
    // caught within a second; a correct one never leaves it, for any duration.
    const std::int64_t first = video.pts.front();
    std::int64_t worst_error = 0;
    for (std::size_t i = 0; i < video.pts.size(); ++i) {
        const auto index = static_cast<std::int64_t>(i);
        const std::int64_t expected = ((index * numerator) + (denominator / 2)) / denominator;
        const std::int64_t error = (video.pts[i] - first) - expected;
        worst_error = std::max(worst_error, error < 0 ? -error : error);
    }
    // One tick, not a percentage: the bound must not grow with the recording's
    // length, which is the entire difference between quantisation and drift.
    EXPECT_LE(worst_error, 1) << "PTS drifted from the exact quantisation of a uniform grid by " << worst_error
                              << " ticks; that is accumulation, not rounding";

    // And the deltas, while not identical, must take only the two values a rounded
    // uniform grid can produce -- floor and ceil of the true frame duration.
    const std::int64_t floor_ticks = numerator / denominator;
    for (const std::int64_t delta : deltas_out) {
        EXPECT_GE(delta, floor_ticks);
        EXPECT_LE(delta, floor_ticks + 1) << "a delta of " << delta << " is neither floor nor ceil of " << numerator
                                          << "/" << denominator << "; the grid is not uniform";
    }
}

void report(const char* label, const fc::test::DecodedVideo& video, const std::set<std::int64_t>& deltas,
            const fc::pipeline::PipelineStats& stats) {
    std::cout << "[ MEASURED ] " << label << ": " << video.frame_count << " frames, timebase " << video.time_base.num
              << "/" << video.time_base.den << ", distinct PTS deltas {";
    for (const std::int64_t delta : deltas) {
        std::cout << " " << delta;
    }
    std::cout << " }\n"
              << "[ MEASURED ]   submitted " << stats.frames_submitted << ", encoded " << stats.frames_encoded
              << ", duplicates " << stats.duplicates_emitted << ", paced out " << stats.frames_paced_out
              << ", queue-dropped " << stats.frames_queue_dropped << "\n";
}

// ---------------------------------------------------------------------------
// The exit criterion: frame_count == duration_s * fps, exactly
// ---------------------------------------------------------------------------

TEST_F(CfrExactnessTest, TheFileHoldsExactlyOneFramePerGridSlotInMkv) {
    fc::pipeline::PipelineStats stats;
    std::filesystem::path path;
    ASSERT_NO_FATAL_FAILURE(record(fc::config::Container::Mkv, Feed::BackPressure, /*stall_ns=*/0, stats, path));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false; // timing only; averaging 300 1080p frames buys nothing here
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::set<std::int64_t> deltas;
    ASSERT_NO_FATAL_FAILURE(assert_grid_is_exact(media.video, kFps, deltas));
    report("mkv, paced feed", media.video, deltas, stats);

    // Row 7's unconditional half. `kFrames` frames went in on an exact 1/60 s grid,
    // so `kFrames` must come out -- and the duration that implies is
    // `kFrames / kFps`, which is the spec's `duration_s * fps` read backwards.
    EXPECT_EQ(media.video.frame_count, kFrames);
    EXPECT_EQ(static_cast<double>(media.video.frame_count) / kFps, static_cast<double>(kFrames) / kFps);

    // With a paced feeder nothing should have been dropped or duplicated at all. This
    // is what separates "the timeline is contiguous" from "the timeline is
    // contiguous because it was patched" -- both are correct outcomes and only one of
    // them means the pipeline kept up.
    EXPECT_EQ(stats.frames_queue_dropped, 0u);
    EXPECT_EQ(stats.duplicates_emitted, 0u);
    EXPECT_EQ(stats.frames_paced_out, 0u);
    EXPECT_EQ(stats.frames_encoded, static_cast<std::uint64_t>(kFrames));
}

TEST_F(CfrExactnessTest, TheFileHoldsExactlyOneFramePerGridSlotInMp4) {
    fc::pipeline::PipelineStats stats;
    std::filesystem::path path;
    ASSERT_NO_FATAL_FAILURE(record(fc::config::Container::Mp4, Feed::BackPressure, /*stall_ns=*/0, stats, path));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::set<std::int64_t> deltas;
    ASSERT_NO_FATAL_FAILURE(assert_grid_is_exact(media.video, kFps, deltas));
    report("mp4, paced feed", media.video, deltas, stats);

    EXPECT_EQ(media.video.frame_count, kFrames);
    EXPECT_EQ(stats.frames_queue_dropped, 0u);
    EXPECT_EQ(stats.duplicates_emitted, 0u);
}

// ---------------------------------------------------------------------------
// The same claim under queue pressure, which is where row 7's defect lives
// ---------------------------------------------------------------------------

// A dropped frame must cost a frame's *content*, never a frame's *slot*. SPEC.md
// §7.2 fills the gap with a duplicate carrying the grid's timestamp, and that is the
// mechanism that turns a hitch into one repeated frame instead of a visible stall
// followed by everything after it arriving early.
//
// The drops are produced by stalling the mux thread rather than by feeding faster.
// Feeding faster does not work on this rig -- the encoder sustains ~370 fps at 1080p
// and a loop that has to render each synthetic frame cannot beat it -- so a flood on
// its own leaves the queue empty and the case vacuous. A slow disk fills the mux
// queue, blocks the venc thread, and starves the encode queue from behind, which is
// both a real failure mode (SPEC.md §20 row 10) and the only one available here.
TEST_F(CfrExactnessTest, DroppedFramesCostContentButNeverSlots) {
    fc::pipeline::PipelineStats stats;
    fc::pipeline::PipelineHealth health;
    std::filesystem::path path;
    // **300 ms on one write in four, against a feed paced at 60 fps** — a drain of
    // about 13 packets/s under a feeder producing 60. The gap is a property of the
    // two configured rates and of nothing else, which is the whole point (BUG-027).
    //
    // The earlier form flooded and stalled 50 ms in four, draining at ~80 packets/s
    // against "however fast this build's feeder happens to be". On Release that
    // dropped 84–107 of 300; on Debug it could drop nothing and the case went vacuous
    // — it said so rather than passing quietly, but a test that reports it has stopped
    // testing is still a test that has stopped testing. 13 packets/s is below what the
    // feeder achieves on the slowest preset this project builds, so the deficit holds
    // on all three.
    //
    // The stall stays under rung 6's 500 ms threshold, so this case and
    // `SlowDiskTest`'s rung 6 case remain about different rungs.
    ASSERT_NO_FATAL_FAILURE(
        record(fc::config::Container::Mkv, Feed::RealTime, /*stall_ns=*/300'000'000, stats, path, &health));

    fc::test::DecodedMedia media;
    fc::test::DecodeOptions options;
    options.audio = false;
    options.video_luma = false;
    fc::test::decode_media(path, options, media);
    ASSERT_TRUE(media.opened) << media.detail;

    std::set<std::int64_t> deltas;
    for (std::size_t i = 1; i < media.video.pts.size(); ++i) {
        deltas.insert(media.video.pts[i] - media.video.pts[i - 1]);
    }
    report("mkv, 60 fps feed against a 13 packets/s drain", media.video, deltas, stats);
    std::cout << "[ MEASURED ]   ladder: rung " << static_cast<int>(health.rung) << ", write p99 "
              << health.disk_write_p99_ns / 1000 << " us, drop ratio " << health.drop_ratio << ", transitions "
              << health.rung_transitions << ", retimes " << health.retimes << "\n";

    // **Not** `frame_count == kFrames` here, and the reason is the point of the case.
    // Backing the pipeline up hard enough to drop frames also engages SPEC.md §13
    // rung 3, which halves the capture rate -- so the file legitimately holds fewer
    // frames than went in. Asserting a fixed count would make this test fail *because
    // the degradation ladder worked*. Row 7's exact-count claim belongs to the two
    // paced cases above, which run at rung 0 where it is meaningful.
    ASSERT_GT(media.video.frame_count, 0);

    // What must hold under drops, at any rate the ladder chooses: the timeline has no
    // gaps. A dropped frame costs a frame's *content* -- one repeated picture -- and
    // never a frame's *slot*, which is what a viewer would see as a stall followed by
    // everything afterwards arriving early.
    //
    // The bound is one 30 fps frame, plus a tick for the container's millisecond
    // quantisation, because 30 fps is the slowest grid rung 3 can drop to.
    const std::int64_t slowest_frame_ticks = (static_cast<std::int64_t>(media.video.time_base.den) + (kFps / 2)) /
                                             (static_cast<std::int64_t>(media.video.time_base.num) * (kFps / 2));
    for (const std::int64_t delta : deltas) {
        EXPECT_LE(delta, slowest_frame_ticks + 1)
            << "a PTS gap of " << delta << " ticks exceeds one frame at the slowest rate the ladder can select; "
            << "a dropped frame cost a slot instead of only its content";
        EXPECT_GT(delta, 0) << "PTS is not strictly increasing across a mid-recording retime";
    }

    // **The case must have exercised the path it claims to.** Asserted on the *union*
    // of the two ways the pipeline sheds work, for the reason BUG-027 established:
    // which mechanism dominates depends on how quickly rung 3 retimes, and after the
    // retime a 60 fps source has half its frames paced out by design. Counting only
    // queue drops left the older form of this assertion riding a six-frame margin on
    // Debug.
    ASSERT_GT(stats.frames_queue_dropped + stats.frames_paced_out, 0u)
        << "the pipeline absorbed a 13 packets/s drain under a 60 fps feed, so this run did not exercise the "
           "duplicate path at all. The rates are configured, not measured -- if this fires, one of them moved";

    // The mechanism, pinned as an inequality rather than an equality, and the reason
    // is worth stating because the equality is the intuitive claim and it is wrong.
    //
    // At a constant rate every dropped frame leaves exactly one empty slot and costs
    // exactly one duplicate. Once rung 3 has halved the grid that stops holding: two
    // consecutive 60 fps drops can map into a single 30 fps slot that a surviving
    // frame also maps to, and then the drop costs *nothing* -- no gap, no duplicate.
    // Measured on this rig: 107 frames lost to the queue, 38 duplicates, 15 paced
    // out, and a timeline with no gaps.
    //
    // So the ceiling is the assertion. A duplicate count *above* the drop count would
    // mean duplicates were being emitted for slots nothing was missing from, which is
    // the "repeated frames" defect of row 7 arriving by the opposite route.
    EXPECT_LE(stats.duplicates_emitted, stats.frames_queue_dropped)
        << "more duplicates than lost frames means slots were filled that nothing was missing from";
}

} // namespace
