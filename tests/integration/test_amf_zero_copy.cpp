// SPEC.md §2.2 item 1 / CLAUDE.md §9 -- the AMF path must not round-trip through
// system memory.
//
// GPU TIER.
//
// > `test_amf_zero_copy` asserts the AMF path never round-trips to system memory.
// > If it round-trips, only the AMF path moves to the direct AMD AMF SDK — NVENC
// > stays on the FFmpeg wrapper.
//
// ---------------------------------------------------------------------------
// What this proves, and what it does not
// ---------------------------------------------------------------------------
// The claim has two halves and they need different evidence.
//
// **Our half — proved here, structurally.** The engine hands libavcodec a D3D11
// texture and never touches system memory doing it: the codec context is opened
// with `pix_fmt = AV_PIX_FMT_D3D11` and a `hw_frames_ctx`, and `submit` moves
// pixels with `CopySubresourceRegion`, GPU to GPU. There is no
// `av_hwframe_transfer_data` and no `Map` anywhere on that path. A regression that
// introduced one would change the codec context's pixel format or fail to attach
// a frames context, and both are asserted below.
//
// **libavcodec's half — measured, not proved.** Whether `h264_amf` internally
// downloads the surface before handing it to AMF is FFmpeg's business and is not
// visible through its public API. What is visible is throughput: a 1080p NV12
// frame is 3.1 MB, so a host round-trip costs that much across PCIe *each way* per
// frame. The measurement below reports sustained encode rate so the number exists;
// it is deliberately not an assertion, because the bandwidth available on this bus
// is high enough that a round-trip would not be obviously slow, and a threshold
// that cannot discriminate is worse than a number a human can read.
//
// So: this test fails if *we* start copying through the host. Deciding whether
// FFmpeg's AMF wrapper does needs a bus-level measurement or a read of amfenc, and
// the open decision in CLAUDE.md §9 stays open until someone does one of those.
// That is stated here rather than left implied, because a green test named
// `zero_copy` invites the assumption that the question is settled.

#include "core/encode/video_encoder.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"

#include "adapter_device.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

// After windows.h and d3d11.h: hwcontext_d3d11va.h names ID3D11Device and friends
// directly and does not include the SDK headers itself.
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFps = 60;

struct Target {
    fc::gpu::AdapterId id;
    std::string description;
    std::string encoder_name;
    std::uint32_t pool_bind_flags = 0;
    std::unique_ptr<fc::gpu::D3dDevice> device;
};

class ZeroCopyTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("zerocopy");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "zerocopytest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == AdapterClass::Software || !adapter.can_encode()) {
                continue;
            }
            auto device = fc::test::device_for_adapter(adapter.id);
            if (!device.has_value()) {
                continue;
            }
            Target target;
            target.id = adapter.id;
            target.description = adapter.description;
            target.encoder_name = adapter.encode.encoder_name;
            target.pool_bind_flags = adapter.encode.nv12_pool_bind_flags;
            target.device = std::make_unique<fc::gpu::D3dDevice>(std::move(device).value());
            targets_.push_back(std::move(target));
        }
        ASSERT_FALSE(targets_.empty()) << "no adapter reported a usable H.264 encoder";
    }

    static void TearDownTestSuite() {
        targets_.clear();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::vector<Target> targets_;
};

std::unique_ptr<fc::test::TempDir> ZeroCopyTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> ZeroCopyTest::topology_;
std::vector<Target> ZeroCopyTest::targets_;

// The structural assertion, on **every** encoder the machine offers -- AMF is what
// CLAUDE.md §9 names, but a regression that started copying through the host would
// be just as wrong on NVENC and there is no reason to check only one.
TEST_F(ZeroCopyTest, EveryHardwareEncoderTakesD3D11SurfacesRatherThanSystemMemory) {
    for (const Target& target : targets_) {
        fc::encode::VideoEncoderSettings settings;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.fps = kFps;
        settings.encoder_name = target.encoder_name;
        settings.pool_bind_flags = target.pool_bind_flags;

        auto created = fc::encode::create_video_encoder(settings);
        ASSERT_TRUE(created.has_value()) << target.description;
        const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(created).value();
        ASSERT_TRUE(encoder->open(target.device->device(), settings).has_value())
            << target.description << " (" << target.encoder_name << ")";

        const AVCodecContext* codec = encoder->codec_context();
        ASSERT_NE(codec, nullptr);

        // The three facts that together mean "libavcodec is being handed GPU
        // surfaces". If any one of them changed, the wrapper would fall back to
        // accepting software frames and every submit would become a download.
        EXPECT_EQ(codec->pix_fmt, AV_PIX_FMT_D3D11)
            << target.description << ": the encoder is configured for software frames";
        EXPECT_EQ(codec->sw_pix_fmt, AV_PIX_FMT_NV12) << target.description;
        ASSERT_NE(codec->hw_frames_ctx, nullptr)
            << target.description << ": no hardware frames context, so frames come from the host";

        // And the pool is on the same device as the textures. A frames context on
        // a *different* adapter is the cross-adapter case of SPEC.md §5.3, which
        // does round-trip -- deliberately -- and must never be reached by accident.
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(codec->hw_frames_ctx->data);
        ASSERT_NE(frames, nullptr);
        EXPECT_EQ(frames->format, AV_PIX_FMT_D3D11) << target.description;
        EXPECT_EQ(frames->sw_format, AV_PIX_FMT_NV12) << target.description;

        const auto* device_ctx = reinterpret_cast<const AVHWDeviceContext*>(frames->device_ref->data);
        ASSERT_NE(device_ctx, nullptr);
        const auto* d3d11 = static_cast<const AVD3D11VADeviceContext*>(device_ctx->hwctx);
        ASSERT_NE(d3d11, nullptr);
        EXPECT_EQ(d3d11->device, target.device->device())
            << target.description
            << ": the encoder's frame pool lives on a different device from the "
               "textures, which forces a cross-adapter copy (SPEC.md §5.3)";
    }
}

// The measurement. Not an assertion about the bus -- see the header -- but the
// number belongs in the record, and a *collapse* in it is worth failing on.
TEST_F(ZeroCopyTest, SustainedEncodeRateIsRecordedForEveryEncoder) {
    for (const Target& target : targets_) {
        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;

        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(target.device->device(), fc::capture::CaptureTarget{}).has_value());

        fc::encode::VideoEncoderSettings settings;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.fps = kFps;
        settings.encoder_name = target.encoder_name;
        settings.pool_bind_flags = target.pool_bind_flags;

        auto created = fc::encode::create_video_encoder(settings);
        ASSERT_TRUE(created.has_value());
        const std::unique_ptr<fc::encode::IVideoEncoder> encoder = std::move(created).value();
        ASSERT_TRUE(encoder->open(target.device->device(), settings).has_value());

        // One frame, submitted repeatedly. The source's own generation cost is CPU
        // work that has nothing to do with the encoder, and including it would
        // measure the test harness (the mistake `test_throughput.cpp` was written
        // to stop making).
        auto frame = source.acquire(std::chrono::milliseconds{500});
        ASSERT_TRUE(frame.has_value());

        const auto begin = std::chrono::steady_clock::now();
        const auto until = begin + std::chrono::seconds{3};
        std::int64_t submitted = 0;
        std::int64_t received = 0;

        while (std::chrono::steady_clock::now() < until) {
            const auto sent =
                encoder->submit(static_cast<ID3D11Texture2D*>(frame.value().texture), submitted * (60000 / kFps));
            if (!sent.has_value()) {
                if (sent.error() != fc::FcError::INTERNAL_QUEUE_FULL) {
                    ADD_FAILURE() << target.description << ": submit failed, " << fc::error_name(sent.error());
                    break;
                }
            } else {
                ++submitted;
            }
            for (;;) {
                auto packet = encoder->receive();
                ASSERT_TRUE(packet.has_value());
                const std::optional<fc::encode::EncodedPacket> value = std::move(packet).value();
                if (!value.has_value()) {
                    break;
                }
                ++received;
            }
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        source.release(frame.value());
        source.stop();

        const double fps = static_cast<double>(submitted) / elapsed;
        // 1080p NV12 is 3.1 MB. At this rate a host round-trip would move
        // `fps * 3.1 MB` in each direction across PCIe, which is the number to
        // compare against the bus if this ever needs settling properly.
        std::cout << "[ MEASURED ] " << target.encoder_name << " on " << target.description << ": " << fps
                  << " fps at 1080p (" << submitted << " submitted, " << received << " packets in " << elapsed
                  << " s), equivalent host round-trip " << (fps * 3.1 * 2) << " MB/s\n";

        testing::Test::RecordProperty(target.encoder_name + "_fps", static_cast<int>(fps));

        // Loose by design. This exists to catch a collapse -- an encoder that
        // silently started synchronising per frame -- not to police performance on
        // a machine that may be doing other things.
        EXPECT_GT(fps, static_cast<double>(kFps)) << target.description << " cannot sustain even real time at 1080p";
        EXPECT_GT(received, 0) << target.description << " produced no packets at all";
    }
}

} // namespace
