// SPEC.md §10.3's finalization cost (BUG-046).
//
// GPU TIER.
//
//   > ON CLEAN STOP: losslessly remux fMP4 -> progressive MP4 with faststart
//   >                (stream copy only, no re-encode, **~2 s for a 1 h file**)
//
// That parenthesis is a performance requirement and nothing measured it. Every other MP4
// case in the tree records for a handful of seconds, so finalization was only ever timed on
// files of a few megabytes -- where a cost proportional to file size is invisible. Reported
// from real use: a 10-15 minute recording of about 1.2 GB takes a significant time to save.
//
// ---------------------------------------------------------------------------
// What is asserted here, and what is deliberately not
// ---------------------------------------------------------------------------
// **Not a throughput number.** Three attempts at building a realistically-sized file inside
// a test failed for the same reason: the fixture, not the product, was the bottleneck. The
// synthetic pattern is mostly flat SMPTE bars, so even at lossless it codes to under a
// kilobyte a frame -- reaching 100 MB took **115,200 frames and 701 seconds**, and the file
// it produced had 24x more packets per megabyte than a real recording, so it measured
// per-packet overhead where a user pays per-byte. A benchmark that takes twelve minutes to
// set up and then measures the wrong term is worse than none.
//
// **The structural property instead**, which is exact, cheap, and is the whole of the fix:
// the remux must not write the file and then rewrite it. `faststart` does -- its own option
// text says "Run a second pass to put the index (moov atom) at the beginning of the file",
// and `ff_format_shift_data` re-opens the output for reading and copies the entire mdat
// forward. Reserving the `moov` instead produces the identical layout in one pass.
// `RemuxStats::second_pass` reports which happened, and a regression to `faststart` fails
// this case on any file size at all.
//
// **And a rate would be the wrong assertion even if one were cheap to take**, which is the
// other half of what this file learned. Measured on the same 290 MB recording through both
// code paths: 382-400 MB/s reserved against 344-369 MB/s with `faststart` -- about 10%, not
// the doubling the pass count suggests, because a file written seconds earlier is entirely
// in the OS page cache and its redundant read is memory. The saving is real disk work only
// for a file too large or too old to be cached, which is exactly the case that cannot be
// built inside a test. See BUG-046, which also records the 2.2x figure an earlier and
// invalid comparison produced.

#include "core/config/config.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/mux/muxer.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 60;
constexpr int kFrames = 300;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Whether `moov` appears in the first `bytes` of the file.
///
/// The property `faststart` existed to provide, checked directly. A reader that has to seek
/// to the end of a 1.2 GB file before it can show anything is what §10.3 is avoiding, and a
/// replacement for `faststart` that quietly left the atom at the end would satisfy every
/// other assertion in the tree.
[[nodiscard]] bool moov_is_at_the_front(const std::filesystem::path& path, std::size_t bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::string head(bytes, '\0');
    file.read(head.data(), static_cast<std::streamsize>(bytes));
    head.resize(static_cast<std::size_t>(file.gcount()));
    return head.find("moov") != std::string::npos;
}

class FinalizeThroughputTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("finalize");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "finalize00001";
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

    /// A real MP4, recorded and finalized through the real pipeline.
    static void record(const std::filesystem::path& output) {
        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = kFrames;
        // 0, as `SegmentationTest` uses and for a reason worth stating: a pre-rendered
        // source returns frames instantly, and feeding an unpaced loop from one floods the
        // encode queue so the drop-oldest policy sheds most of the recording. Measured with
        // 8 pre-rendered frames, 300 submitted frames reached the file as **9 packets** and
        // the §10.4 duration gate then failed the recording intermittently. Rendering each
        // frame paces the feed to something the encoder can absorb.
        source_settings.prerendered_frames = 0;

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

        PipelineSettings settings;
        settings.output = output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mp4;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;

        VideoPipeline pipeline;
        ASSERT_TRUE(pipeline.start(device_->device(), settings).has_value());

        const std::int64_t epoch = 8'000'000'000LL;
        for (int i = 0; i < kFrames; ++i) {
            auto frame = source.acquire(std::chrono::milliseconds{1000});
            ASSERT_TRUE(frame.has_value());
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns = static_cast<std::uint64_t>(epoch + (static_cast<std::int64_t>(i) * kNsPerSecond / kFps));
            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
        }
        source.stop();

        const auto report = pipeline.stop();
        ASSERT_TRUE(report.has_value()) << fc::error_name(report.error());
        ASSERT_TRUE(report.value().valid) << report.value().detail;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::unique_ptr<fc::gpu::D3dDevice> device_;
    static std::string encoder_name_;
    static std::uint32_t pool_bind_flags_;
};

std::unique_ptr<fc::test::TempDir> FinalizeThroughputTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> FinalizeThroughputTest::topology_;
std::unique_ptr<fc::gpu::D3dDevice> FinalizeThroughputTest::device_;
std::string FinalizeThroughputTest::encoder_name_;
std::uint32_t FinalizeThroughputTest::pool_bind_flags_ = 0;

// The assertion that would have caught the four-pass remux, on a file of any size.
TEST_F(FinalizeThroughputTest, TheRemuxWritesTheFileOnceRatherThanTwice) {
    const std::filesystem::path recorded = dir_->path() / "recorded.mp4";
    ASSERT_NO_FATAL_FAILURE(record(recorded));

    // Remuxed again, which is a legitimate input -- `remux_to_progressive` takes any MP4 --
    // and is what lets this observe the remux directly rather than through a recording's
    // stop path. The reservation logic is identical either way.
    const std::filesystem::path again = dir_->path() / "again.mp4";
    fc::mux::RemuxStats stats;
    const auto remuxed = fc::mux::remux_to_progressive(recorded, again, 0, &stats);
    ASSERT_TRUE(remuxed.has_value()) << fc::error_name(remuxed.error());

    std::error_code ec;
    const double megabytes = static_cast<double>(std::filesystem::file_size(again, ec)) / (1024.0 * 1024.0);
    const double milliseconds = static_cast<double>(stats.elapsed_ns) / 1'000'000.0;
    std::printf("[finalize] %.1f MB, %llu packets, moov reserved %lld bytes, second pass %s, %.1f ms (%.0f MB/s)\n",
                megabytes, static_cast<unsigned long long>(stats.packets),
                static_cast<long long>(stats.moov_reserved_bytes), stats.second_pass ? "YES" : "no", milliseconds,
                milliseconds > 0.0 ? megabytes / (milliseconds / 1000.0) : 0.0);

    // The fix, stated as the property it is.
    EXPECT_FALSE(stats.second_pass)
        << "the remux wrote the file and then rewrote it. `faststart` does that -- its own option text says \"Run a "
           "second pass\" -- which doubles the I/O of every save. Measured on the same 290 MB file both ways: "
           "382-400 MB/s reserved against 344-369 MB/s with faststart, and more than that on a file too large to "
           "sit in the page cache";
    EXPECT_GT(stats.moov_reserved_bytes, 0) << "no reservation was made, so the second pass was unavoidable";
    EXPECT_GT(stats.packets, 0U);

    // And the reservation scales with the recording rather than with nothing. Whatever is
    // unused stays in the file as `free` padding, so an estimator that reserved wildly too
    // much would trade time for space without saying so.
    //
    // The bound allows the fixed floor plus a tenth: the floor is 256 KB of stream
    // descriptors and parameter sets that any MP4 needs, and on a short recording it is
    // legitimately most of the reservation -- 262,504 bytes of it on this 0.3 MB file, of
    // which only 360 bytes are per-sample. What must not happen is the *scaling* part
    // running away, which is what this catches.
    constexpr double kFixedFloorBytes = 1024.0 * 1024.0;
    EXPECT_LT(static_cast<double>(stats.moov_reserved_bytes), kFixedFloorBytes + (megabytes * 1024.0 * 1024.0 * 0.10))
        << "the moov reservation scales badly with the recording";

    // The property `faststart` was there for, which the replacement has to keep.
    EXPECT_TRUE(moov_is_at_the_front(again, 1024))
        << "the moov atom is not near the front; a reader must seek to the end of the file to open it";
    EXPECT_TRUE(moov_is_at_the_front(recorded, 1024))
        << "a recording finalized by the pipeline has its moov at the end";
}

// Remuxes a file named by `FC_REMUX_INPUT` and reports what it cost.
//
// **A measuring instrument, not an assertion**, and skipped unless pointed at something.
// It exists because the throughput question this file is about cannot be answered on a file
// small enough to build in a test: anything under a few hundred megabytes sits in the OS
// page cache, so the reads are free and the very cost under study is the one that does not
// appear. Measured on the reference rig, a 35 MB file remuxes at 260 MB/s *with* the second
// pass -- which looks fine, and is cache.
//
// Point it at a real recording's fragmented form to get a real number:
//
//     set FC_REMUX_INPUT=D:\big_fragmented.mp4
//     fc_gpu_tests --gtest_filter=FinalizeThroughputTest.MeasureAGivenFile
TEST_F(FinalizeThroughputTest, MeasureAGivenFile) {
    std::array<char, 512> buffer{};
    const DWORD length = ::GetEnvironmentVariableA("FC_REMUX_INPUT", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        GTEST_SKIP() << "set FC_REMUX_INPUT to a fragmented MP4 to measure the remux on it";
    }

    const std::filesystem::path input{buffer.data()};
    ASSERT_TRUE(std::filesystem::exists(input)) << input.string();

    const std::filesystem::path out = dir_->path() / "measured.mp4";
    fc::mux::RemuxStats stats;
    const auto remuxed = fc::mux::remux_to_progressive(input, out, 0, &stats);
    ASSERT_TRUE(remuxed.has_value()) << fc::error_name(remuxed.error());

    std::error_code ec;
    const double megabytes = static_cast<double>(std::filesystem::file_size(input, ec)) / (1024.0 * 1024.0);
    const double seconds = static_cast<double>(stats.elapsed_ns) / 1e9;
    std::printf("[finalize] %s: %.0f MB, %llu packets, moov reserved %lld, second pass %s\n"
                "[finalize]   remux %.2f s = %.0f MB/s\n",
                input.filename().string().c_str(), megabytes, static_cast<unsigned long long>(stats.packets),
                static_cast<long long>(stats.moov_reserved_bytes), stats.second_pass ? "YES" : "no", seconds,
                megabytes / seconds);
}

} // namespace
