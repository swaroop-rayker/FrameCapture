// Synthetic test-pattern source (SPEC.md §20.1, CLAUDE.md §5).
//
// GPU TIER for the parts that render on a device; the pattern maths is verified
// without one.
//
// The point of this file is the round trip: a frame index encoded into the pattern,
// pushed through the real colour converter, and read back out of the NV12 luma
// plane. That is the mechanism SPEC.md §20 row 5 needs to assert monotonic,
// uncorrupted frames -- and it removes the dependency on whatever is on screen.

#include "synthetic_source.h"

#include "core/color/nv12_converter.h"
#include "core/gpu/adapter_info.h"
#include "core/gpu/d3d_device.h"
#include "core/logging/logger.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>
#include <set>
#include <span>
#include <vector>

namespace {

using fc::test::SyntheticSource;
using Microsoft::WRL::ComPtr;

constexpr int kWidth = 640;
constexpr int kHeight = 360;

// ---------------------------------------------------------------------------
// Pattern maths -- no GPU needed
// ---------------------------------------------------------------------------

TEST(SyntheticPattern, RendersTheRequestedSize) {
    const auto pixels = fc::test::render_bgra(kWidth, kHeight, 0);
    EXPECT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);
}

TEST(SyntheticPattern, IsDeterministic) {
    // Same index must give byte-identical output, or nothing downstream can assert
    // anything about content.
    EXPECT_EQ(fc::test::render_bgra(kWidth, kHeight, 42), fc::test::render_bgra(kWidth, kHeight, 42));
}

TEST(SyntheticPattern, ConsecutiveFramesDiffer) {
    // Without motion, a pipeline stuck on frame 0 is indistinguishable from a
    // working one.
    EXPECT_NE(fc::test::render_bgra(kWidth, kHeight, 7), fc::test::render_bgra(kWidth, kHeight, 8));
}

TEST(SyntheticPattern, BarcodeCellsAreOnlyBlackOrWhite) {
    // The barcode has to survive 4:2:0 subsampling, so it must be carried by luma
    // alone -- pure black or pure white, never a colour.
    const auto pixels = fc::test::render_bgra(kWidth, kHeight, 0xA5A5A5u);
    for (int bit = 0; bit < fc::test::kBarcodeBits; ++bit) {
        const int x = (bit * fc::test::kBarcodeCell) + (fc::test::kBarcodeCell / 2);
        const std::size_t offset = static_cast<std::size_t>(x) * 4;
        const std::uint8_t b = pixels[offset];
        const std::uint8_t g = pixels[offset + 1];
        const std::uint8_t r = pixels[offset + 2];
        EXPECT_EQ(b, g) << "barcode cell " << bit << " is not neutral";
        EXPECT_EQ(g, r) << "barcode cell " << bit << " is not neutral";
        EXPECT_TRUE(b == 0 || b == 255) << "barcode cell " << bit << " is neither black nor white";
    }
}

TEST(SyntheticPattern, RejectsASourceTooNarrowForTheBarcode) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = fc::test::kMinimumWidth - 2;
    settings.height = 240;
    const auto configured = source.configure(settings);
    ASSERT_FALSE(configured.has_value());
    EXPECT_EQ(configured.error(), fc::FcError::INTERNAL_INVALID_ARGUMENT);
}

TEST(SyntheticPattern, ProvidesSevenSmpteBars) {
    ASSERT_EQ(fc::test::smpte_bars().size(), 7u);
    EXPECT_STREQ(fc::test::smpte_bars().front().name, "white");
    EXPECT_STREQ(fc::test::smpte_bars().back().name, "blue");
}

// ---------------------------------------------------------------------------
// Round trip through the real converter
// ---------------------------------------------------------------------------

class SyntheticSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("synthetic");
        fc::log::Config config;
        config.directory = dir_->path();
        config.session_id = "synthetic000001";
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

TEST_F(SyntheticSourceTest, ProducesFramesWithMonotonicSequenceNumbers) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    std::uint32_t previous = 0;
    std::uint64_t previous_pts = 0;
    for (int i = 0; i < 10; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{100});
        ASSERT_TRUE(frame.has_value());
        if (i > 0) {
            EXPECT_EQ(frame.value().sequence, previous + 1);
            EXPECT_GT(frame.value().qpc_ns, previous_pts);
        }
        previous = frame.value().sequence;
        previous_pts = frame.value().qpc_ns;
        source.release(frame.value());
    }
    source.stop();
}

TEST_F(SyntheticSourceTest, HonoursItsFrameLimit) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_limit = 3;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    for (int i = 0; i < 3; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{100});
        ASSERT_TRUE(frame.has_value()) << "frame " << i;
        source.release(frame.value());
    }
    const auto past_end = source.acquire(std::chrono::milliseconds{50});
    ASSERT_FALSE(past_end.has_value());
    EXPECT_EQ(past_end.error(), fc::FcError::CAPTURE_FRAME_TIMEOUT);
    source.stop();
}

// The headline capability: the frame index survives the whole colour pipeline.
//
// This is what lets a test assert "frame N decoded to frame N's content" instead of
// merely "some bytes arrived", and it is the mechanism SPEC.md §20 row 5 needs.
TEST_F(SyntheticSourceTest, FrameIndexSurvivesConversionToNv12) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings convert_settings;
    convert_settings.width = kWidth;
    convert_settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(device_->device(), convert_settings).has_value());

    std::set<std::uint32_t> decoded;
    for (int i = 0; i < 12; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{100});
        ASSERT_TRUE(frame.has_value());
        const std::uint32_t expected = frame.value().sequence;

        ASSERT_TRUE(converter.convert(device_->context(), frame.value().texture).has_value());
        source.release(frame.value());

        auto nv12 =
            fc::color::read_back_nv12(device_->device(), device_->context(), converter.output(), kWidth, kHeight);
        ASSERT_TRUE(nv12.has_value());

        const auto index = fc::test::decode_barcode_from_luma(
            std::span<const std::uint8_t>{nv12.value()}.first(static_cast<std::size_t>(kWidth) * kHeight), kWidth,
            kHeight);
        ASSERT_TRUE(index.has_value()) << "barcode did not survive conversion for frame " << expected;
        EXPECT_EQ(*index, expected) << "decoded frame index does not match the frame that produced it";
        EXPECT_TRUE(decoded.insert(*index).second) << "frame index " << *index << " appeared twice";
    }

    EXPECT_EQ(decoded.size(), 12u);
    source.stop();
}

// SMPTE bars land at known positions with known values, so a geometry or colour
// regression is caught by value rather than by eye.
TEST_F(SyntheticSourceTest, SmpteBarsConvertToTheExpectedLumaOrder) {
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings convert_settings;
    convert_settings.width = kWidth;
    convert_settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(device_->device(), convert_settings).has_value());

    auto frame = source.acquire(std::chrono::milliseconds{100});
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(converter.convert(device_->context(), frame.value().texture).has_value());
    source.release(frame.value());

    auto nv12 = fc::color::read_back_nv12(device_->device(), device_->context(), converter.output(), kWidth, kHeight);
    ASSERT_TRUE(nv12.has_value());

    const int bar_width = kWidth / static_cast<int>(fc::test::smpte_bars().size());
    // Sample well below the barcode and away from the moving bar's start.
    const int sample_y = kHeight - 8;

    std::vector<std::uint8_t> bar_luma;
    for (std::size_t bar = 0; bar < fc::test::smpte_bars().size(); ++bar) {
        const int x = (static_cast<int>(bar) * bar_width) + (bar_width / 2);
        bar_luma.push_back(nv12.value()[(static_cast<std::size_t>(sample_y) * kWidth) + static_cast<std::size_t>(x)]);
    }

    // BT.709 luma weights are R 0.2126, G 0.7152, B 0.0722, so the bars descend in
    // brightness: white > yellow > cyan > green > magenta > red > blue.
    for (std::size_t i = 1; i < bar_luma.size(); ++i) {
        EXPECT_LT(bar_luma[i], bar_luma[i - 1])
            << "bar " << fc::test::smpte_bars()[i].name << " is not darker than " << fc::test::smpte_bars()[i - 1].name
            << "; the bar order or the luma weights are wrong";
    }

    // Nothing may fall outside limited range.
    for (const std::uint8_t luma : bar_luma) {
        EXPECT_GE(luma, 16);
        EXPECT_LE(luma, 235);
    }
}

TEST_F(SyntheticSourceTest, EveryFrameIsNonUniform) {
    // The synthetic source must defeat the black-frame check by construction, so a
    // pipeline test failing that check means the pipeline is at fault, not the input.
    SyntheticSource source;
    SyntheticSource::Settings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    ASSERT_TRUE(source.configure(settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    fc::color::Nv12Converter converter;
    fc::color::ConverterSettings convert_settings;
    convert_settings.width = kWidth;
    convert_settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(device_->device(), convert_settings).has_value());

    for (int i = 0; i < 5; ++i) {
        auto frame = source.acquire(std::chrono::milliseconds{100});
        ASSERT_TRUE(frame.has_value());
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

        EXPECT_GT(variance, 100.0) << "synthetic frame " << i << " was nearly uniform";
    }
    source.stop();
}

} // namespace
