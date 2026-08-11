// SPEC.md §11 — video segmentation, and M9's exit criterion.
//
// GPU TIER.
//
//   > **M9** | Segmentation (opt-in) + preview | Keyframe-aligned splits with no frame
//   > loss
//
// §20 has no row for segmentation, so there is no named test handed down and no
// pre-agreed set of assertions. The clauses below are §11's own sentences, each turned
// into something the finished files can be asked.
//
// ---------------------------------------------------------------------------
// Why this runs on a synthetic clock rather than in real time
// ---------------------------------------------------------------------------
// The duration trigger's smallest configurable value is one minute (`config_schema.cpp`),
// and the size trigger's is 100 MB. Waiting for either in wall-clock time would make this
// a multi-minute test of the encoder's throughput.
//
// It does not have to be. §11's duration trigger measures *timeline* time -- §7.5's table
// says so in as many words -- so feeding the pipeline frames stamped on a synthetic clock
// reaches the one-minute mark as fast as the encoder can accept them, and every assertion
// below is about the files rather than about how long they took to write. That is the
// same method row 18 uses, for the same reason.
//
// ---------------------------------------------------------------------------
// What "no frame loss" is asserted on
// ---------------------------------------------------------------------------
// Not on a frame count, which would be satisfied by a recording that lost a frame at the
// seam and duplicated one elsewhere. The synthetic source stamps every frame with its
// index as a barcode, so the files carry the identity of every frame they hold: the
// barcodes of all segments concatenated must be the submitted sequence, in order, with
// nothing missing and nothing repeated. A frame lost at a rollover is a gap in that
// sequence and nothing else looks like one.

#include "core/config/config.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/mux/segment_planner.h"
#include "core/pipeline/video_pipeline.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

extern "C" {
#include <libavformat/avformat.h>
}

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 30;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// One minute per segment -- the smallest the configuration allows, so the test exercises
/// a value a user can actually choose.
constexpr int kSegmentMinutes = 1;
/// Two and a half minutes of timeline: two splits, three files, and a last segment that
/// is deliberately *not* a whole one.
constexpr int kFrames = kSegmentMinutes * 60 * kFps * 5 / 2;

/// Whether a file's first video packet is a keyframe, without decoding it.
///
/// The direct form of §11's "open the next file starting *at* it". Decoding proves the
/// file plays; only the packet flag proves the boundary was chosen where §11 says.
[[nodiscard]] std::optional<bool> first_video_packet_is_keyframe(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    const std::string name = path.string();
    if (avformat_open_input(&raw, name.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    std::optional<bool> answer;
    if (avformat_find_stream_info(raw, nullptr) >= 0) {
        int video_index = -1;
        for (unsigned i = 0; i < raw->nb_streams; ++i) {
            if (raw->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_index = static_cast<int>(i);
                break;
            }
        }
        AVPacket* packet = av_packet_alloc();
        if (video_index >= 0 && packet != nullptr) {
            while (av_read_frame(raw, packet) >= 0) {
                if (packet->stream_index == video_index) {
                    answer = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                    av_packet_unref(packet);
                    break;
                }
                av_packet_unref(packet);
            }
        }
        av_packet_free(&packet);
    }
    avformat_close_input(&raw);
    return answer;
}

class SegmentationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("segmentation");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "segmentation001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const fc::gpu::AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == fc::gpu::AdapterClass::Software || !adapter.can_encode()) {
                continue;
            }
            auto created = fc::test::device_for_adapter(adapter.id);
            if (!created.has_value()) {
                continue;
            }
            device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
            encoder_name_ = adapter.encode.encoder_name;
            pool_bind_flags_ = adapter.encode.nv12_pool_bind_flags;
            break;
        }
        ASSERT_NE(device_, nullptr) << "no adapter reported a usable H.264 encoder";
    }

    static void TearDownTestSuite() {
        device_.reset();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    struct Recorded {
        std::filesystem::path base;
        int submitted = 0;
        fc::pipeline::PipelineStats stats;
        bool valid = false;
    };

    /// Records `frames` on a synthetic clock with `segmentation` applied, at SPEC.md §9's
    /// default two-second keyframe interval.
    static Recorded record(const char* name, int frames, const fc::config::SegmentationSettings& segmentation) {
        return record_with_gop(name, frames, segmentation, 2);
    }

    /// As above, with the keyframe interval chosen rather than defaulted -- which is what
    /// lets a split be made to fall mid-GOP. See the mid-GOP case below.
    static Recorded record_with_gop(const char* name, int frames, const fc::config::SegmentationSettings& segmentation,
                                    int gop_seconds) {
        Recorded out;
        out.base = dir_->path() / name;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value() ||
            !source.start(device_->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "the synthetic source would not start";
            return out;
        }

        PipelineSettings settings;
        settings.output = out.base;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;
        settings.segmentation = segmentation;
        settings.video.gop_seconds = gop_seconds;

        VideoPipeline pipeline;
        const auto started = pipeline.start(device_->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << "pipeline start failed: " << fc::error_name(started.error());
            return out;
        }

        const std::int64_t epoch = 8'000'000'000LL;
        for (int index = 0; index < frames; ++index) {
            auto frame = source.acquire(std::chrono::milliseconds{1000});
            if (!frame.has_value()) {
                ADD_FAILURE() << "the synthetic source stopped at frame " << index;
                break;
            }
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns =
                static_cast<std::uint64_t>(epoch + (static_cast<std::int64_t>(index) * kNsPerSecond / kFps));
            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
            ++out.submitted;
        }
        source.stop();

        out.stats = pipeline.stats();
        const auto report = pipeline.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << "finalization failed: " << fc::error_name(report.error());
            return out;
        }
        out.valid = report.value().valid;
        if (!out.valid) {
            ADD_FAILURE() << "the last segment did not validate: " << report.value().detail;
        }
        return out;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::unique_ptr<fc::gpu::D3dDevice> device_;
    static std::string encoder_name_;
    static std::uint32_t pool_bind_flags_;
};

std::unique_ptr<fc::test::TempDir> SegmentationTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> SegmentationTest::topology_;
std::unique_ptr<fc::gpu::D3dDevice> SegmentationTest::device_;
std::string SegmentationTest::encoder_name_;
std::uint32_t SegmentationTest::pool_bind_flags_ = 0;

// ---------------------------------------------------------------------------
// CLAUDE.md hard rule 7, first, because it is the one that must never regress
// ---------------------------------------------------------------------------

TEST_F(SegmentationTest, SegmentationOffProducesExactlyOneFileWithTheNameItWasGiven) {
    const Recorded recorded = record("plain.mkv", 300, fc::config::SegmentationSettings{});
    ASSERT_EQ(recorded.submitted, 300);

    EXPECT_TRUE(std::filesystem::exists(recorded.base)) << "the recording was not written under the name it was given";
    EXPECT_EQ(recorded.stats.segments, 1U);
    EXPECT_FALSE(std::filesystem::exists(fc::mux::segment_path(recorded.base, 1)))
        << "a `_part001` file exists for an unsegmented recording";

    std::filesystem::path sidecar = recorded.base;
    sidecar.replace_extension(".segments.json");
    EXPECT_FALSE(std::filesystem::exists(sidecar)) << "an unsegmented recording wrote a segment index";
}

// ---------------------------------------------------------------------------
// M9's exit criterion
// ---------------------------------------------------------------------------

TEST_F(SegmentationTest, SplitsAreKeyframeAlignedAndLoseNoFrames) {
    fc::config::SegmentationSettings segmentation;
    segmentation.enabled = true;
    segmentation.split_by_duration = true;
    segmentation.duration_minutes = kSegmentMinutes;
    segmentation.split_by_size = false;

    const Recorded recorded = record("split.mkv", kFrames, segmentation);
    ASSERT_EQ(recorded.submitted, kFrames) << "the feed did not complete";

    // --- the files exist, are named per §11, and there is no odd one out ------
    ASSERT_GE(recorded.stats.segments, 2U) << "two and a half minutes at one minute per segment did not split";
    EXPECT_FALSE(std::filesystem::exists(recorded.base))
        << "a file named after the base exists alongside the parts; §11 names every file `_partNNN`";

    std::vector<std::filesystem::path> parts;
    for (std::uint64_t i = 1; i <= recorded.stats.segments; ++i) {
        const std::filesystem::path part = fc::mux::segment_path(recorded.base, static_cast<int>(i));
        ASSERT_TRUE(std::filesystem::exists(part)) << "missing segment " << part.filename().string();
        EXPECT_GT(std::filesystem::file_size(part), 0U) << part.filename().string() << " is empty";
        parts.push_back(part);
    }

    // --- every segment opens on a keyframe -----------------------------------
    // The exit criterion's first half, asserted on the packet flag rather than inferred
    // from the file playing: a segment that opens mid-GOP can still decode *something*.
    for (const std::filesystem::path& part : parts) {
        const std::optional<bool> keyframe = first_video_packet_is_keyframe(part);
        ASSERT_TRUE(keyframe.has_value()) << "could not read the first video packet of " << part.filename().string();
        EXPECT_TRUE(*keyframe) << part.filename().string()
                               << " opens on a packet that is not a keyframe -- §11's split was not aligned";
    }

    // --- every segment is independently valid, and restarts at zero ----------
    std::vector<std::uint32_t> all_barcodes;
    int unreadable = 0;
    for (const std::filesystem::path& part : parts) {
        fc::test::DecodedMedia media;
        fc::test::decode_media(part, fc::test::DecodeOptions{}, media);
        ASSERT_TRUE(media.opened) << part.filename().string() << ": " << media.detail;
        ASSERT_GT(media.video.frame_count, 0) << part.filename().string() << " decoded no frames";

        // > Timestamps restart at 0 in each segment (each file is independently valid).
        ASSERT_FALSE(media.video.times.empty());
        EXPECT_LT(media.video.times.front(), 2.0 / kFps)
            << part.filename().string() << " starts at " << media.video.times.front()
            << " s; §11 requires each segment's timestamps to restart at 0";

        for (const std::optional<std::uint32_t>& barcode : media.video.barcodes) {
            if (barcode.has_value()) {
                all_barcodes.push_back(*barcode);
            } else {
                ++unreadable;
            }
        }
    }
    EXPECT_EQ(unreadable, 0) << unreadable << " frames across the segments had an unreadable barcode";

    // --- no frame loss -------------------------------------------------------
    // The exit criterion's second half. Every frame submitted appears exactly once,
    // across the whole set, in order -- a frame dropped at a rollover is a gap here and
    // a frame written twice is a repeat, and neither has anywhere else to hide.
    ASSERT_FALSE(all_barcodes.empty());
    EXPECT_EQ(all_barcodes.size(), static_cast<std::size_t>(kFrames))
        << "the segments hold " << all_barcodes.size() << " frames of " << kFrames << " submitted";

    int gaps = 0;
    int repeats = 0;
    for (std::size_t i = 1; i < all_barcodes.size(); ++i) {
        const std::uint32_t previous = all_barcodes[i - 1];
        const std::uint32_t current = all_barcodes[i];
        if (current == previous) {
            ++repeats;
        } else if (current != previous + 1) {
            ++gaps;
            ADD_FAILURE() << "frames jump from " << previous << " to " << current << " at position " << i
                          << " -- a frame was lost at a segment boundary";
        }
    }
    EXPECT_EQ(gaps, 0);
    EXPECT_EQ(repeats, 0) << repeats << " frames repeat across the segment set";

    // --- §11's sidecar -------------------------------------------------------
    std::filesystem::path sidecar = recorded.base;
    sidecar.replace_extension(".segments.json");
    ASSERT_TRUE(std::filesystem::exists(sidecar)) << "§11's segment index was not written";
    const std::ifstream stream(sidecar);
    std::stringstream text;
    text << stream.rdbuf();
    const std::string body = text.str();
    EXPECT_NE(body.find("start_offset_ns"), std::string::npos) << "the index records no start offsets";
    for (const std::filesystem::path& part : parts) {
        EXPECT_NE(body.find(part.filename().string()), std::string::npos)
            << part.filename().string() << " is missing from the segment index";
    }

    std::printf("[M9] %d frames -> %llu segments, all keyframe-aligned, %zu barcodes contiguous, %d gaps, "
                "%d repeats\n",
                kFrames, static_cast<unsigned long long>(recorded.stats.segments), all_barcodes.size(), gaps, repeats);
}

// The case above cannot fail on alignment, and finding that out is worth recording.
//
// SPEC.md §9's default keyframe interval is two seconds, and §11's duration trigger is a
// whole number of minutes -- so on a perfectly paced stream the trigger *always* lands
// exactly on a keyframe and the wait for an IDR is never entered. Measured: with the wait
// deleted outright, the case above still passed with three segments, all keyframe-aligned
// and 4500 contiguous barcodes. It proves no frame loss and nothing whatever about
// alignment.
//
// The size trigger looked like the answer and is not: the synthetic pattern encodes to
// about 212 bytes a frame, so 4500 frames is 954 KB and no whole number of megabytes can
// split it at all.
//
// What does work is a keyframe interval that does not divide the segment: `gop_seconds`
// is configurable (SPEC.md §9), and at 7 s a 60 s segment boundary falls 4 s into a GOP.
// The split then *must* wait, and every frame between the trigger and the IDR must still
// reach the file being closed.
TEST_F(SegmentationTest, ASplitWhoseTriggerFallsMidGopWaitsForTheKeyframe) {
    constexpr int kGopSeconds = 7;
    static_assert(60 % kGopSeconds != 0, "the trigger would land on a GOP boundary and prove nothing");

    fc::config::SegmentationSettings segmentation;
    segmentation.enabled = true;
    segmentation.split_by_duration = true;
    segmentation.duration_minutes = kSegmentMinutes;
    segmentation.split_by_size = false;

    const Recorded recorded = record_with_gop("midgop.mkv", kFrames, segmentation, kGopSeconds);
    ASSERT_EQ(recorded.submitted, kFrames);
    ASSERT_GE(recorded.stats.segments, 2U) << "the recording did not split";

    std::vector<std::uint32_t> all_barcodes;
    int off_grid = 0;
    for (std::uint64_t i = 1; i <= recorded.stats.segments; ++i) {
        const std::filesystem::path part = fc::mux::segment_path(recorded.base, static_cast<int>(i));
        ASSERT_TRUE(std::filesystem::exists(part)) << "missing " << part.filename().string();

        const std::optional<bool> keyframe = first_video_packet_is_keyframe(part);
        ASSERT_TRUE(keyframe.has_value()) << "could not read the first packet of " << part.filename().string();
        EXPECT_TRUE(*keyframe) << part.filename().string() << " opens on a packet that is not a keyframe";

        fc::test::DecodedMedia media;
        fc::test::decode_media(part, fc::test::DecodeOptions{}, media);
        ASSERT_TRUE(media.opened) << part.filename().string() << ": " << media.detail;
        for (const std::optional<std::uint32_t>& barcode : media.video.barcodes) {
            if (barcode.has_value()) {
                all_barcodes.push_back(*barcode);
            }
        }

        // The boundary must not be one the two-second grid could have produced by luck --
        // otherwise this case has the same blind spot as the one above.
        if (i > 1 && !media.video.barcodes.empty() && media.video.barcodes.front().has_value()) {
            const std::uint32_t first = *media.video.barcodes.front();
            if ((first % static_cast<std::uint32_t>(kSegmentMinutes * 60 * kFps)) != 0) {
                ++off_grid;
            }
        }
    }

    EXPECT_GT(off_grid, 0) << "every split landed exactly on the duration trigger, so the wait for a keyframe was "
                              "never entered and this case proves no more than the one above";

    // And the waiting cost nothing: every frame is still present, in order.
    EXPECT_EQ(all_barcodes.size(), static_cast<std::size_t>(kFrames));
    for (std::size_t i = 1; i < all_barcodes.size(); ++i) {
        ASSERT_EQ(all_barcodes[i], all_barcodes[i - 1] + 1)
            << "frames jump from " << all_barcodes[i - 1] << " to " << all_barcodes[i] << " at position " << i;
    }

    std::printf("[M9 mid-GOP] %d frames, %d s GOP -> %llu segments, %d boundaries past their trigger, "
                "%zu barcodes contiguous\n",
                kFrames, kGopSeconds, static_cast<unsigned long long>(recorded.stats.segments), off_grid,
                all_barcodes.size());
}

} // namespace
