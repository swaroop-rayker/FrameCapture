// Capture -> convert -> disk, driven by the synthetic source (CLAUDE.md §5).
//
// GPU TIER.
//
// These are the pipeline-correctness tests. They use the deterministic pattern
// rather than the desktop, so every assertion is exact: frame N must decode to
// frame N's barcode, the sequence must have no gaps, and the bar geometry must be
// where it was drawn. The desktop-facing tests in test_capture_to_nv12.cpp and
// test_capture_sustained.cpp remain, but they now only prove the *backends* work
// against a real display -- content correctness is asserted here.

#include "synthetic_source.h"

#include "core/capture/capture_factory.h"
#include "core/capture/nv12_writer.h"
#include "core/color/nv12_converter.h"
#include "core/gpu/adapter_info.h"
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

#include <memory>
#include <span>
#include <vector>

namespace {

using fc::capture::Nv12Writer;
using fc::test::SyntheticSource;
using Microsoft::WRL::ComPtr;

constexpr int kWidth = 640;
constexpr int kHeight = 360;

class SyntheticPipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("pipeline");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "pipeline0000001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        ComPtr<IDXGIFactory1> factory;
        ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                desc.VendorId == fc::gpu::kVendorMicrosoft) {
                continue;
            }
            auto created = fc::gpu::create_device_on_adapter(adapter.Get());
            if (created.has_value()) {
                device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
                break;
            }
        }
        ASSERT_NE(device_, nullptr) << "no hardware adapter; this test is labelled `gpu`";
    }

    void TearDown() override {
        device_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    std::unique_ptr<fc::test::TempDir> dir_;
    std::unique_ptr<fc::gpu::D3dDevice> device_;
};

// The assertion the desktop-facing tests could never make: every frame written to
// disk carries the index of the frame that produced it, in order, with no gaps and
// no repeats.
TEST_F(SyntheticPipelineTest, EveryFrameOnDiskIsTheFrameThatProducedIt) {
    constexpr int kFrames = 30;

    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_limit = kFrames;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings convert_settings;
    convert_settings.width = kWidth;
    convert_settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(device_->device(), convert_settings).has_value());

    const std::filesystem::path output = dir_->path() / "synthetic.nv12";
    Nv12Writer writer;
    ASSERT_TRUE(writer.open(output, kWidth, kHeight).has_value());

    std::vector<std::uint32_t> decoded_indices;
    for (int i = 0; i < kFrames; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        ASSERT_TRUE(frame.has_value()) << "frame " << i;

        ASSERT_TRUE(converter.convert(device_->context(), frame.value().texture).has_value());
        source.release(frame.value());

        auto nv12 =
            fc::color::read_back_nv12(device_->device(), device_->context(), converter.output(), kWidth, kHeight);
        ASSERT_TRUE(nv12.has_value());

        const auto index = fc::test::decode_barcode_from_luma(
            std::span<const std::uint8_t>{nv12.value()}.first(static_cast<std::size_t>(kWidth) * kHeight), kWidth,
            kHeight);
        ASSERT_TRUE(index.has_value()) << "frame " << i << " has an unreadable barcode -- torn or corrupted";
        decoded_indices.push_back(*index);

        ASSERT_TRUE(writer.write(nv12.value()).has_value());
    }
    source.stop();
    ASSERT_TRUE(writer.close().has_value());

    // Exactly 0..kFrames-1, in order. No gaps (dropped frames), no repeats (a
    // stalled pipeline re-emitting), no reordering.
    ASSERT_EQ(decoded_indices.size(), static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_EQ(decoded_indices[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i))
            << "frame " << i << " decoded to the wrong index";
    }

    EXPECT_EQ(writer.frames_written(), static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(std::filesystem::file_size(output),
              static_cast<std::uint64_t>(kFrames) * Nv12Writer::frame_size(kWidth, kHeight));
}

// SPEC.md §20 row 1, asserted exactly rather than statistically: the pattern is
// non-uniform by construction, so *any* uniform frame is a pipeline fault.
TEST_F(SyntheticPipelineTest, NoFrameIsEverUniform) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_limit = 20;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings convert_settings;
    convert_settings.width = kWidth;
    convert_settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(device_->device(), convert_settings).has_value());

    int checked = 0;
    while (true) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            break;
        }
        ASSERT_TRUE(converter.convert(device_->context(), frame.value().texture).has_value());
        source.release(frame.value());

        auto nv12 =
            fc::color::read_back_nv12(device_->device(), device_->context(), converter.output(), kWidth, kHeight);
        ASSERT_TRUE(nv12.has_value());

        const std::size_t count = static_cast<std::size_t>(kWidth) * kHeight;
        double sum = 0.0;
        for (std::size_t p = 0; p < count; ++p) {
            sum += nv12.value()[p];
        }
        const double mean = sum / static_cast<double>(count);
        double variance = 0.0;
        for (std::size_t p = 0; p < count; ++p) {
            const double delta = nv12.value()[p] - mean;
            variance += delta * delta;
        }
        variance /= static_cast<double>(count);

        // Not "most frames" -- every frame. That is only assertable because the
        // input is known.
        EXPECT_GT(variance, 100.0) << "frame " << checked << " was uniform";
        ++checked;
    }
    source.stop();
    EXPECT_EQ(checked, 20);
}

// The pattern is identical on every adapter, so the pipeline result must be too.
// A per-vendor difference here would be a real portability bug rather than a
// difference in what happened to be on each screen.
TEST_F(SyntheticPipelineTest, EveryAdapterProducesIdenticalOutputForTheSameFrame) {
    ComPtr<IDXGIFactory1> factory;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));

    std::vector<std::vector<std::uint8_t>> per_adapter;
    std::vector<std::string> names;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
            desc.VendorId == fc::gpu::kVendorMicrosoft) {
            continue;
        }
        auto created = fc::gpu::create_device_on_adapter(adapter.Get());
        if (!created.has_value()) {
            continue;
        }
        const fc::gpu::D3dDevice& device = created.value();

        SyntheticSource source;
        SyntheticSource::Settings settings;
        settings.width = kWidth;
        settings.height = kHeight;
        ASSERT_TRUE(source.configure(settings).has_value());
        ASSERT_TRUE(source.start(device.device(), fc::capture::CaptureTarget{}).has_value());

        fc::color::Nv12Converter converter;
        fc::color::ConverterSettings convert_settings;
        convert_settings.width = kWidth;
        convert_settings.height = kHeight;
        ASSERT_TRUE(converter.initialize(device.device(), convert_settings).has_value());

        auto frame = source.acquire(std::chrono::milliseconds{200});
        ASSERT_TRUE(frame.has_value());
        ASSERT_TRUE(converter.convert(device.context(), frame.value().texture).has_value());
        source.release(frame.value());

        auto nv12 = fc::color::read_back_nv12(device.device(), device.context(), converter.output(), kWidth, kHeight);
        ASSERT_TRUE(nv12.has_value());
        per_adapter.push_back(std::move(nv12).value());

        const int length = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        std::string name(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
        if (length > 1) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), length, nullptr, nullptr);
        }
        names.push_back(std::move(name));
        source.stop();
    }

    ASSERT_FALSE(per_adapter.empty());
    if (per_adapter.size() < 2) {
        GTEST_SKIP() << "only one hardware adapter present";
    }

    for (std::size_t a = 1; a < per_adapter.size(); ++a) {
        ASSERT_EQ(per_adapter[a].size(), per_adapter[0].size());
        std::size_t differing = 0;
        int worst = 0;
        for (std::size_t p = 0; p < per_adapter[0].size(); ++p) {
            const int delta = std::abs(static_cast<int>(per_adapter[a][p]) - static_cast<int>(per_adapter[0][p]));
            if (delta != 0) {
                ++differing;
                worst = std::max(worst, delta);
            }
        }
        // Vendors may round the final UNORM store differently by one code point;
        // anything beyond that is a genuine divergence in the conversion.
        EXPECT_LE(worst, 1) << names[a] << " differs from " << names[0] << " by " << worst << " code points";
        EXPECT_LT(differing, per_adapter[0].size() / 4)
            << names[a] << " differs from " << names[0] << " across " << differing << " samples";
    }
}

// ---------------------------------------------------------------------------
// The factory, against real hardware
// ---------------------------------------------------------------------------

TEST_F(SyntheticPipelineTest, AutoSelectionYieldsAWorkingBackendForTheDisplay) {
    fc::gpu::GpuTopologyService topology;
    ASSERT_TRUE(topology.refresh(fc::gpu::DiscoveryOptions{false}).has_value());
    const fc::gpu::AdapterInfo* primary = topology.topology().primary_display_adapter();
    ASSERT_NE(primary, nullptr);
    ASSERT_FALSE(primary->outputs.empty());

    fc::capture::CaptureTarget target;
    target.monitor = primary->outputs.front().monitor;

    auto capture = fc::capture::create_capture(fc::config::CaptureBackend::Auto, target);
    ASSERT_TRUE(capture.has_value());
    // On any supported Windows build, auto must land on WGC.
    EXPECT_EQ(capture.value()->backend(), fc::capture::Backend::Wgc);
}

TEST_F(SyntheticPipelineTest, ForcingDdaYieldsTheDdaBackend) {
    fc::capture::CaptureTarget target;
    target.monitor = 0x1;

    auto capture = fc::capture::create_capture(fc::config::CaptureBackend::Dda, target);
    ASSERT_TRUE(capture.has_value());
    EXPECT_EQ(capture.value()->backend(), fc::capture::Backend::Dda);
}

TEST_F(SyntheticPipelineTest, AWindowTargetWithForcedDdaIsRefusedByTheFactory) {
    fc::capture::CaptureTarget target;
    target.window = 0x1;

    const auto capture = fc::capture::create_capture(fc::config::CaptureBackend::Dda, target);
    ASSERT_FALSE(capture.has_value());
    EXPECT_EQ(capture.error(), fc::FcError::INTERNAL_NOT_IMPLEMENTED);
}

} // namespace
