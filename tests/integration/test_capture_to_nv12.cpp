// Capture -> NV12 -> disk, end to end (SPEC.md §24 M2, §20 row 1).
//
// GPU TIER. Captures the real primary display, because that is what the milestone
// is about; the synthetic source covers deterministic pixel content for the tests
// that need exact values.
//
// Real display, but not ambient content: every case that acquires a frame calls
// `present_pattern` first, which covers the output with a picture this process
// owns and changes at a stated rate (BUG-033). WGC composites only when the screen
// changes, so before that a quiet desktop made these cases fail -- and a busy one
// made them pass without proving anything.

#include "core/capture/nv12_writer.h"
#include "core/capture/wgc/wgc_capture.h"
#include "screen_animator.h"

#include "core/color/nv12_converter.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

namespace {

using fc::capture::CaptureTarget;
using fc::capture::Nv12Writer;
using fc::capture::wgc::WgcCapture;
using Microsoft::WRL::ComPtr;

/// Mean and variance of the luma plane. SPEC.md §20 row 1's black-frame test is
/// "decode every frame, assert mean luma variance > threshold" -- a solid black
/// frame has zero variance whatever its mean.
struct LumaStats {
    double mean = 0.0;
    double variance = 0.0;
};

LumaStats luma_stats(const std::vector<std::uint8_t>& nv12, int width, int height) {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (nv12.size() < count) {
        return {};
    }

    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        sum += nv12[i];
    }
    const double mean = sum / static_cast<double>(count);

    double squared = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double delta = nv12[i] - mean;
        squared += delta * delta;
    }
    return LumaStats{mean, squared / static_cast<double>(count)};
}

class CaptureToNv12Test : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("capture");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "capturetest0001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        ASSERT_TRUE(fc::capture::wgc::is_supported())
            << "Windows.Graphics.Capture unavailable; requires Windows 10 build 19041+";

        ASSERT_TRUE(topology_.refresh(fc::gpu::DiscoveryOptions{false}).has_value());

        primary_ = topology_.topology().primary_display_adapter();
        ASSERT_NE(primary_, nullptr) << "no adapter owns the primary display";
        ASSERT_FALSE(primary_->outputs.empty());
        monitor_ = primary_->outputs.front().monitor;
        width_ = primary_->outputs.front().width();
        height_ = primary_->outputs.front().height();
        // NV12 is 4:2:0, so odd dimensions have no chroma sample to belong to.
        width_ -= width_ % 2;
        height_ -= height_ % 2;
    }

    void TearDown() override {
        animator_.stop();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Takes ownership of what the primary display shows, for the rest of the test
    /// (BUG-033). Call through `ASSERT_NO_FATAL_FAILURE`: if it fails, the case's
    /// input is ambient again and every assertion after it is about the desktop.
    void present_pattern() {
        fc::test::ScreenAnimator::Settings settings;
        settings.monitor = monitor_;
        settings.fps = 60;
        const auto started = animator_.start(settings);
        ASSERT_TRUE(started.has_value()) << "could not present a pattern to the target output: "
                                         << fc::error_name(started.error());
    }

    /// A device on the adapter that owns the primary display -- the M1 result put
    /// to work.
    fc::Result<fc::gpu::D3dDevice> device_on_primary() {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return fc::FcError::GPU_ADAPTER_ENUMERATION_FAILED;
        }
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }
            const fc::gpu::AdapterId id{
                (static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart))};
            if (id == primary_->id) {
                return fc::gpu::create_device_on_adapter(adapter.Get());
            }
        }
        return fc::FcError::GPU_NO_SUITABLE_ADAPTER;
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    fc::gpu::GpuTopologyService topology_;
    const fc::gpu::AdapterInfo* primary_ = nullptr;
    std::uintptr_t monitor_ = 0;
    int width_ = 0;
    int height_ = 0;
    fc::test::ScreenAnimator animator_;
};

TEST_F(CaptureToNv12Test, WgcReportsItselfSupported) {
    EXPECT_TRUE(fc::capture::wgc::is_supported());
}

TEST_F(CaptureToNv12Test, StartingWithNoTargetIsRefused) {
    auto device = device_on_primary();
    ASSERT_TRUE(device.has_value());

    WgcCapture capture;
    const auto started = capture.start(device.value().device(), CaptureTarget{});
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), fc::FcError::CAPTURE_TARGET_NOT_FOUND);
}

TEST_F(CaptureToNv12Test, CapturesTheDisplayOnTheAdapterThatOwnsIt) {
    ASSERT_NO_FATAL_FAILURE(present_pattern());

    auto device = device_on_primary();
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = monitor_;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());
    EXPECT_TRUE(capture.running());

    auto frame = capture.acquire(std::chrono::milliseconds{2000});
    ASSERT_TRUE(frame.has_value()) << "no frame arrived within 2 s";
    EXPECT_NE(frame.value().texture, nullptr);
    EXPECT_EQ(frame.value().adapter, primary_->id) << "frame is not on the adapter we asked for";
    EXPECT_GT(frame.value().content.width(), 0);
    // SPEC.md §4.2: we requested BGRA8. FP16 here means system HDR is on.
    EXPECT_EQ(frame.value().dxgi_format, static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM))
        << "system HDR may be enabled; the tone-map branch is required";

    capture.release(frame.value());
    capture.stop();
    EXPECT_FALSE(capture.running());
}

TEST_F(CaptureToNv12Test, FrameSequenceNumbersAdvanceMonotonically) {
    ASSERT_NO_FATAL_FAILURE(present_pattern());

    auto device = device_on_primary();
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = monitor_;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    std::uint32_t previous = 0;
    int collected = 0;
    for (int attempt = 0; attempt < 60 && collected < 5; ++attempt) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }
        if (collected > 0) {
            EXPECT_GT(frame.value().sequence, previous) << "sequence went backwards";
            EXPECT_GT(frame.value().qpc_ns, 0u);
        }
        previous = frame.value().sequence;
        ++collected;
        capture.release(frame.value());
    }

    capture.stop();
    EXPECT_GE(collected, 2) << "the display produced almost no frames, against " << animator_.repaints()
                            << " repaints presented to it";
}

// The M2 exit criterion in miniature, and SPEC.md §20 row 1's assertion: capture,
// convert, write, and prove the result is not black.
TEST_F(CaptureToNv12Test, CapturedFramesReachDiskAsNv12AndAreNotBlack) {
    ASSERT_NO_FATAL_FAILURE(present_pattern());

    auto device = device_on_primary();
    ASSERT_TRUE(device.has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings settings;
    settings.width = width_;
    settings.height = height_;
    ASSERT_TRUE(converter.initialize(device.value().device(), settings).has_value());

    const std::filesystem::path output = dir_->path() / "capture.nv12";
    Nv12Writer writer;
    ASSERT_TRUE(writer.open(output, width_, height_).has_value());

    CaptureTarget target;
    target.monitor = monitor_;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());

    int written = 0;
    int signed_frames = 0;
    double best_variance = 0.0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};

    while (written < 10 && std::chrono::steady_clock::now() < deadline) {
        auto frame = capture.acquire(std::chrono::milliseconds{500});
        if (!frame.has_value()) {
            continue;
        }

        const auto converted = converter.convert(device.value().context(), frame.value().texture);
        capture.release(frame.value());
        if (!converted.has_value()) {
            // A resolution change mid-test is legitimate; anything else is not.
            ASSERT_EQ(converted.error(), fc::FcError::CAPTURE_RESOLUTION_CHANGED)
                << "conversion failed with an unexpected error";
            continue;
        }

        auto bytes = fc::color::read_back_nv12(device.value().device(), device.value().context(), converter.output(),
                                               width_, height_);
        ASSERT_TRUE(bytes.has_value());

        const LumaStats stats = luma_stats(bytes.value(), width_, height_);
        best_variance = std::max(best_variance, stats.variance);
        if (fc::test::pattern_signature(bytes.value(), width_, height_).present()) {
            ++signed_frames;
        }

        ASSERT_TRUE(writer.write(bytes.value()).has_value());
        ++written;
    }

    capture.stop();
    ASSERT_TRUE(writer.close().has_value());

    ASSERT_GT(written, 0) << "no frames were captured at all";

    // SPEC.md §20 row 1. A solid black frame has zero variance regardless of mean,
    // which is exactly what the adapter/output mismatch produces -- with S_OK from
    // every call along the way.
    EXPECT_GT(best_variance, 1.0) << "every captured frame was uniform; this is the all-black-video signature. "
                                     "Check that the capture device is on the adapter owning the target output.";

    // And what reached disk is the picture this test presented rather than
    // whatever was on screen (BUG-033), so a pass says something about the code.
    EXPECT_GT(signed_frames * 2, written) << signed_frames << " of " << written
                                          << " frames carried the test pattern; the capture is reading something "
                                             "other than what this test put on the output";

    EXPECT_EQ(writer.frames_written(), static_cast<std::uint64_t>(written));
    EXPECT_EQ(writer.bytes_written(), static_cast<std::uint64_t>(written) * Nv12Writer::frame_size(width_, height_));

    // The file must be exactly N whole frames: raw NV12 has no framing of its own,
    // so a partial frame silently corrupts everything after it.
    ASSERT_TRUE(std::filesystem::exists(output));
    EXPECT_EQ(std::filesystem::file_size(output), writer.bytes_written());
    EXPECT_EQ(std::filesystem::file_size(output) % Nv12Writer::frame_size(width_, height_), 0u);
}

TEST_F(CaptureToNv12Test, WriterRejectsAWrongSizedFrame) {
    Nv12Writer writer;
    ASSERT_TRUE(writer.open(dir_->path() / "short.nv12", 64, 64).has_value());

    const std::vector<std::uint8_t> too_short(100, 0);
    const auto result = writer.write(too_short);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::INTERNAL_INVALID_ARGUMENT);
    EXPECT_EQ(writer.frames_written(), 0u);
}

TEST_F(CaptureToNv12Test, StartingTwiceIsRefused) {
    auto device = device_on_primary();
    ASSERT_TRUE(device.has_value());

    CaptureTarget target;
    target.monitor = monitor_;

    WgcCapture capture;
    ASSERT_TRUE(capture.start(device.value().device(), target).has_value());
    const auto second = capture.start(device.value().device(), target);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), fc::FcError::INTERNAL_INVALID_STATE);
    capture.stop();
}

} // namespace
