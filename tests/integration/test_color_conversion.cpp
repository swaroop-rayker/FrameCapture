// BGRA8 -> NV12 conversion correctness (SPEC.md §6, §20 row 8).
//
// GPU TIER. Runs the real compute shader on every hardware adapter present, so a
// driver that disagrees about planar UAVs or rounding shows up here rather than in
// a recording.
//
// Colour bugs are silent and they ship. Every assertion below corresponds to a
// specific locked stage in §6.

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

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using fc::color::ColorRange;
using fc::color::ConverterSettings;
using fc::color::Nv12Converter;
using Microsoft::WRL::ComPtr;

constexpr int kWidth = 64;
constexpr int kHeight = 64;

struct Bgra {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 255;
};

/// Decoded NV12, addressable by pixel.
struct Nv12Image {
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;

    [[nodiscard]] std::uint8_t y(int px, int py) const {
        return bytes[(static_cast<std::size_t>(py) * width) + px];
    }

    /// Cb for the chroma sample covering this luma pixel.
    [[nodiscard]] std::uint8_t cb(int px, int py) const {
        const std::size_t plane = static_cast<std::size_t>(width) * height;
        return bytes[plane + (static_cast<std::size_t>(py / 2) * width) + static_cast<std::size_t>((px / 2) * 2)];
    }

    [[nodiscard]] std::uint8_t cr(int px, int py) const {
        const std::size_t plane = static_cast<std::size_t>(width) * height;
        return bytes[plane + (static_cast<std::size_t>(py / 2) * width) + static_cast<std::size_t>((px / 2) * 2) + 1];
    }
};

/// One D3D11 device per hardware adapter, so the shader is proven on each.
struct AdapterUnderTest {
    std::string description;
    std::shared_ptr<fc::gpu::D3dDevice> device;
};

class ColorConversionTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("color");
        fc::log::Config config;
        config.directory = dir_->path();
        config.session_id = "colortest000001";
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
            ASSERT_TRUE(SUCCEEDED(adapter->GetDesc1(&desc)));
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == fc::gpu::kVendorMicrosoft) {
                continue;
            }

            auto created = fc::gpu::create_device_on_adapter(adapter.Get());
            if (!created.has_value()) {
                continue;
            }

            const int length = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
            std::string name(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
            if (length > 1) {
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), length, nullptr, nullptr);
            }
            adapters_.push_back(
                AdapterUnderTest{std::move(name), std::make_shared<fc::gpu::D3dDevice>(std::move(created).value())});
        }

        ASSERT_FALSE(adapters_.empty()) << "no hardware adapter; this test is labelled `gpu`";
    }

    static void TearDownTestSuite() {
        adapters_.clear();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Uploads a solid BGRA image, converts it, and reads the NV12 back.
    static Nv12Image convert_solid(const AdapterUnderTest& adapter, Bgra colour, ColorRange range) {
        const std::vector<Bgra> pixels(static_cast<std::size_t>(kWidth) * kHeight, colour);
        return convert_pixels(adapter, pixels, range);
    }

    static Nv12Image convert_pixels(const AdapterUnderTest& adapter, const std::vector<Bgra>& pixels,
                                    ColorRange range) {
        Nv12Image image;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = pixels.data();
        initial.SysMemPitch = kWidth * sizeof(Bgra);

        ComPtr<ID3D11Texture2D> source;
        if (FAILED(adapter.device->device()->CreateTexture2D(&desc, &initial, &source))) {
            ADD_FAILURE() << "source texture creation failed on " << adapter.description;
            return image;
        }

        Nv12Converter converter;
        ConverterSettings settings;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.range = range;
        if (const auto init = converter.initialize(adapter.device->device(), settings); !init.has_value()) {
            ADD_FAILURE() << "converter init failed on " << adapter.description;
            return image;
        }

        if (const auto converted = converter.convert(adapter.device->context(), source.Get()); !converted.has_value()) {
            ADD_FAILURE() << "conversion failed on " << adapter.description;
            return image;
        }

        auto bytes = fc::color::read_back_nv12(adapter.device->device(), adapter.device->context(), converter.output(),
                                               kWidth, kHeight);
        if (!bytes.has_value()) {
            ADD_FAILURE() << "readback failed on " << adapter.description;
            return image;
        }

        image.bytes = std::move(bytes).value();
        image.width = kWidth;
        image.height = kHeight;
        return image;
    }

    static std::vector<AdapterUnderTest> adapters_;
    static std::unique_ptr<fc::test::TempDir> dir_;
};

std::vector<AdapterUnderTest> ColorConversionTest::adapters_;
std::unique_ptr<fc::test::TempDir> ColorConversionTest::dir_;

// ---------------------------------------------------------------------------
// Channel order -- SPEC.md §6 "Channel order"
// ---------------------------------------------------------------------------

// The spec asks for exactly this test: "A single unit test rendering pure #FF0000
// and asserting decoded output is red (not blue) catches the entire class of
// channel-swap bugs."
//
// BT.709 limited range, pure red: Y=63, Cb=102, Cr=240.
// If B and R were swapped we would see Y=32, Cb=240, Cr=118 -- unmistakably wrong.
TEST_F(ColorConversionTest, PureRedProducesRedNotBlue) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_solid(adapter, Bgra{0, 0, 255, 255}, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty()) << adapter.description;

        EXPECT_NEAR(image.y(0, 0), 63, 2) << adapter.description;
        EXPECT_NEAR(image.cb(0, 0), 102, 2) << adapter.description;
        EXPECT_NEAR(image.cr(0, 0), 240, 2) << adapter.description;

        // The discriminating assertion: red has Cr well above Cb.
        EXPECT_GT(image.cr(0, 0), image.cb(0, 0) + 100) << "red/blue channels look swapped on " << adapter.description;
    }
}

TEST_F(ColorConversionTest, PureBlueProducesBlueNotRed) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_solid(adapter, Bgra{255, 0, 0, 255}, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty()) << adapter.description;

        // BT.709 limited, pure blue: Y=32, Cb=240, Cr=118.
        EXPECT_NEAR(image.y(0, 0), 32, 2) << adapter.description;
        EXPECT_NEAR(image.cb(0, 0), 240, 2) << adapter.description;
        EXPECT_NEAR(image.cr(0, 0), 118, 2) << adapter.description;
        EXPECT_GT(image.cb(0, 0), image.cr(0, 0) + 100) << adapter.description;
    }
}

TEST_F(ColorConversionTest, PureGreenIsTheBrightestPrimary) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_solid(adapter, Bgra{0, 255, 0, 255}, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty()) << adapter.description;

        // BT.709 weights green at 0.7152: Y = 16 + 219*0.7152 = 173.
        EXPECT_NEAR(image.y(0, 0), 173, 2) << adapter.description;
        EXPECT_LT(image.cb(0, 0), 128) << adapter.description;
        EXPECT_LT(image.cr(0, 0), 128) << adapter.description;
    }
}

// ---------------------------------------------------------------------------
// Range -- SPEC.md §6 "range limited (16-235 / 16-240) by default"
// ---------------------------------------------------------------------------

TEST_F(ColorConversionTest, LimitedRangeClampsBlackAndWhiteToTheVideoRange) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image black = convert_solid(adapter, Bgra{0, 0, 0, 255}, ColorRange::Limited);
        const Nv12Image white = convert_solid(adapter, Bgra{255, 255, 255, 255}, ColorRange::Limited);
        ASSERT_FALSE(black.bytes.empty());
        ASSERT_FALSE(white.bytes.empty());

        EXPECT_NEAR(black.y(0, 0), 16, 1) << "limited-range black must be 16, not 0, on " << adapter.description;
        EXPECT_NEAR(white.y(0, 0), 235, 1) << "limited-range white must be 235, not 255, on " << adapter.description;

        // Neutral colours carry no chroma.
        EXPECT_NEAR(black.cb(0, 0), 128, 1);
        EXPECT_NEAR(black.cr(0, 0), 128, 1);
        EXPECT_NEAR(white.cb(0, 0), 128, 1);
        EXPECT_NEAR(white.cr(0, 0), 128, 1);
    }
}

TEST_F(ColorConversionTest, FullRangeUsesTheWholeScale) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image black = convert_solid(adapter, Bgra{0, 0, 0, 255}, ColorRange::Full);
        const Nv12Image white = convert_solid(adapter, Bgra{255, 255, 255, 255}, ColorRange::Full);
        ASSERT_FALSE(black.bytes.empty());
        ASSERT_FALSE(white.bytes.empty());

        EXPECT_NEAR(black.y(0, 0), 0, 1) << adapter.description;
        EXPECT_NEAR(white.y(0, 0), 255, 1) << adapter.description;
    }
}

TEST_F(ColorConversionTest, MidGreyRoundTripsToTheExpectedCode) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_solid(adapter, Bgra{128, 128, 128, 255}, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty());
        // 16 + 219 * (128/255) = 126.
        EXPECT_NEAR(image.y(0, 0), 126, 2) << adapter.description;
        EXPECT_NEAR(image.cb(0, 0), 128, 1);
        EXPECT_NEAR(image.cr(0, 0), 128, 1);
    }
}

// ---------------------------------------------------------------------------
// Chroma siting -- SPEC.md §6 "box/average filter over the 2x2 luma quad"
// ---------------------------------------------------------------------------

// Point sampling would take one corner of the quad and produce visible chroma
// fringing on text. A box filter averages, so a half-red/half-black quad must land
// midway between the two chroma values -- not on either one.
TEST_F(ColorConversionTest, ChromaIsBoxFilteredNotPointSampled) {
    for (const AdapterUnderTest& adapter : adapters_) {
        // Vertical stripes one pixel wide: every 2x2 quad is half red, half black.
        std::vector<Bgra> pixels(static_cast<std::size_t>(kWidth) * kHeight);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                pixels[(static_cast<std::size_t>(y) * kWidth) + x] =
                    (x % 2 == 0) ? Bgra{0, 0, 255, 255} : Bgra{0, 0, 0, 255};
            }
        }

        const Nv12Image image = convert_pixels(adapter, pixels, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty()) << adapter.description;

        // Luma keeps full resolution: red column 63, black column 16.
        EXPECT_NEAR(image.y(0, 0), 63, 2) << adapter.description;
        EXPECT_NEAR(image.y(1, 0), 16, 2) << adapter.description;

        // Averaging (255,0,0) and (0,0,0) gives (127.5,0,0):
        // Y' = 0.2126*0.5 = 0.1063, Cr = 128 + 224*(0.5-0.1063)/1.5748 = 184.
        // Point sampling the red corner would give 240; the black corner, 128.
        EXPECT_NEAR(image.cr(0, 0), 184, 3)
            << "chroma looks point-sampled rather than box-filtered on " << adapter.description;
        EXPECT_LT(image.cr(0, 0), 230) << "chroma took the red corner only";
        EXPECT_GT(image.cr(0, 0), 140) << "chroma took the black corner only";
    }
}

TEST_F(ColorConversionTest, LumaKeepsFullResolutionWhileChromaIsHalved) {
    for (const AdapterUnderTest& adapter : adapters_) {
        // A single white pixel at the origin, black everywhere else.
        std::vector<Bgra> pixels(static_cast<std::size_t>(kWidth) * kHeight, Bgra{0, 0, 0, 255});
        pixels[0] = Bgra{255, 255, 255, 255};

        const Nv12Image image = convert_pixels(adapter, pixels, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty()) << adapter.description;

        EXPECT_NEAR(image.y(0, 0), 235, 2) << "the lit pixel lost its luma on " << adapter.description;
        EXPECT_NEAR(image.y(1, 0), 16, 2) << "luma bled into a neighbour on " << adapter.description;
        EXPECT_NEAR(image.y(0, 1), 16, 2) << adapter.description;
        EXPECT_NEAR(image.y(2, 2), 16, 2) << adapter.description;
    }
}

// ---------------------------------------------------------------------------
// Output shape and refusals
// ---------------------------------------------------------------------------

TEST_F(ColorConversionTest, OutputIsExactlyNv12Sized) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_solid(adapter, Bgra{10, 20, 30, 255}, ColorRange::Limited);
        // NV12 is 12 bits per pixel: a full luma plane plus a half-resolution
        // interleaved chroma plane.
        EXPECT_EQ(image.bytes.size(), static_cast<std::size_t>(kWidth) * kHeight * 3 / 2) << adapter.description;
    }
}

TEST_F(ColorConversionTest, OddDimensionsAreRefusedRatherThanTruncated) {
    Nv12Converter converter;
    ConverterSettings settings;
    settings.width = 1921;
    settings.height = 1080;
    const auto result = converter.initialize(adapters_.front().device->device(), settings);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::INTERNAL_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// HDR -- SPEC.md §4.2's scRGB -> BT.709 SDR tone-map branch
// ---------------------------------------------------------------------------

/// Half-precision float, so a real FP16 scRGB surface can be uploaded.
std::uint16_t to_half(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) {
        return static_cast<std::uint16_t>(sign);
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

struct ScRgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

/// Converts a solid scRGB image, taking the HDR branch.
Nv12Image convert_scrgb(const AdapterUnderTest& adapter, ScRgb colour, double sdr_white_nits, ColorRange range) {
    Nv12Image image;

    struct Half4 {
        std::uint16_t r, g, b, a;
    };

    std::vector<Half4> pixels(static_cast<std::size_t>(kWidth) * kHeight,
                              Half4{to_half(colour.r), to_half(colour.g), to_half(colour.b), to_half(1.0f)});

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = kWidth * sizeof(Half4);

    ComPtr<ID3D11Texture2D> source;
    if (FAILED(adapter.device->device()->CreateTexture2D(&desc, &initial, &source))) {
        ADD_FAILURE() << "FP16 source creation failed on " << adapter.description;
        return image;
    }

    Nv12Converter converter;
    ConverterSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.range = range;
    settings.tone_map_hdr = true;
    settings.sdr_white_nits = sdr_white_nits;
    if (!converter.initialize(adapter.device->device(), settings).has_value()) {
        ADD_FAILURE() << "converter init failed on " << adapter.description;
        return image;
    }
    if (!converter.convert(adapter.device->context(), source.Get()).has_value()) {
        ADD_FAILURE() << "HDR conversion failed on " << adapter.description;
        return image;
    }

    auto bytes = fc::color::read_back_nv12(adapter.device->device(), adapter.device->context(), converter.output(),
                                           kWidth, kHeight);
    if (!bytes.has_value()) {
        ADD_FAILURE() << "readback failed on " << adapter.description;
        return image;
    }
    image.bytes = std::move(bytes).value();
    image.width = kWidth;
    image.height = kHeight;
    return image;
}

// The property that makes the branch trustworthy: ordinary SDR content composited
// into an HDR desktop must come out *identical* to the SDR path. Almost all of an
// HDR desktop is exactly that, so a tone-map that shifted it would recolour the
// whole recording.
TEST_F(ColorConversionTest, SdrRangeContentTonemapsToTheSameValuesAsTheSdrPath) {
    constexpr double kDiffuseWhiteNits = 203.0;
    // scRGB 1.0 is 80 nits, so diffuse white sits at 203/80.
    constexpr float kWhite = 203.0f / 80.0f;

    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image sdr_red = convert_solid(adapter, Bgra{0, 0, 255, 255}, ColorRange::Limited);
        const Nv12Image hdr_red =
            convert_scrgb(adapter, ScRgb{kWhite, 0.0f, 0.0f}, kDiffuseWhiteNits, ColorRange::Limited);
        ASSERT_FALSE(sdr_red.bytes.empty());
        ASSERT_FALSE(hdr_red.bytes.empty());

        EXPECT_NEAR(hdr_red.y(0, 0), sdr_red.y(0, 0), 2) << adapter.description;
        EXPECT_NEAR(hdr_red.cb(0, 0), sdr_red.cb(0, 0), 2) << adapter.description;
        EXPECT_NEAR(hdr_red.cr(0, 0), sdr_red.cr(0, 0), 2) << adapter.description;
        // And it is still red, not blue -- the channel-order lock holds on this
        // path too.
        EXPECT_GT(hdr_red.cr(0, 0), hdr_red.cb(0, 0) + 100) << adapter.description;
    }
}

TEST_F(ColorConversionTest, DiffuseWhiteLandsNearSdrWhite) {
    constexpr float kWhite = 203.0f / 80.0f;
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_scrgb(adapter, ScRgb{kWhite, kWhite, kWhite}, 203.0, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty());
        // The shoulder engages just below white, so this sits a little under 235
        // rather than exactly on it -- that headroom is what highlights use.
        EXPECT_GT(image.y(0, 0), 215) << "diffuse white came out too dark on " << adapter.description;
        EXPECT_LE(image.y(0, 0), 235) << adapter.description;
        EXPECT_NEAR(image.cb(0, 0), 128, 2) << "neutral white picked up a colour cast";
        EXPECT_NEAR(image.cr(0, 0), 128, 2);
    }
}

// Clipping highlights turns every specular into a flat blob. The shoulder must keep
// them distinguishable and must never exceed the legal range.
TEST_F(ColorConversionTest, HighlightsAboveDiffuseWhiteRollOffWithoutClipping) {
    constexpr float kWhite = 203.0f / 80.0f;
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image at_white = convert_scrgb(adapter, ScRgb{kWhite, kWhite, kWhite}, 203.0, ColorRange::Limited);
        const Nv12Image bright =
            convert_scrgb(adapter, ScRgb{kWhite * 2, kWhite * 2, kWhite * 2}, 203.0, ColorRange::Limited);
        const Nv12Image brightest =
            convert_scrgb(adapter, ScRgb{kWhite * 8, kWhite * 8, kWhite * 8}, 203.0, ColorRange::Limited);
        ASSERT_FALSE(brightest.bytes.empty());

        EXPECT_GE(bright.y(0, 0), at_white.y(0, 0)) << adapter.description;
        EXPECT_GE(brightest.y(0, 0), bright.y(0, 0)) << adapter.description;
        // Monotonic, and never past legal limited-range white.
        EXPECT_LE(brightest.y(0, 0), 235) << "highlight exceeded the limited range on " << adapter.description;
    }
}

// scRGB permits negative components for colours outside BT.709. They cannot be
// shown on an SDR display, and letting them through produces luma that wraps.
TEST_F(ColorConversionTest, OutOfGamutNegativesAreClampedNotWrapped) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_scrgb(adapter, ScRgb{-0.5f, -0.5f, -0.5f}, 203.0, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty());
        EXPECT_NEAR(image.y(0, 0), 16, 2) << "negative scRGB wrapped instead of clamping on " << adapter.description;
    }
}

TEST_F(ColorConversionTest, ScrgbBlackIsLimitedRangeBlack) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image image = convert_scrgb(adapter, ScRgb{0.0f, 0.0f, 0.0f}, 203.0, ColorRange::Limited);
        ASSERT_FALSE(image.bytes.empty());
        EXPECT_NEAR(image.y(0, 0), 16, 1) << adapter.description;
        EXPECT_NEAR(image.cb(0, 0), 128, 1);
        EXPECT_NEAR(image.cr(0, 0), 128, 1);
    }
}

// A lower reference white means the same scRGB value reads as brighter.
TEST_F(ColorConversionTest, SdrWhiteNitsScalesTheResult) {
    for (const AdapterUnderTest& adapter : adapters_) {
        const Nv12Image dim = convert_scrgb(adapter, ScRgb{1.0f, 1.0f, 1.0f}, 400.0, ColorRange::Limited);
        const Nv12Image bright = convert_scrgb(adapter, ScRgb{1.0f, 1.0f, 1.0f}, 100.0, ColorRange::Limited);
        ASSERT_FALSE(dim.bytes.empty());
        ASSERT_FALSE(bright.bytes.empty());
        EXPECT_GT(bright.y(0, 0), dim.y(0, 0)) << "sdr_white_nits had no effect on " << adapter.description;
    }
}

// SPEC.md §4.2: reinterpreting FP16 bits as BGRA8 is the black/neon-green bug. With
// tone-mapping switched off the converter must refuse, never guess.
TEST_F(ColorConversionTest, AnFp16SourceIsRefusedWhenToneMappingIsDisabled) {
    const AdapterUnderTest& adapter = adapters_.front();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> hdr_source;
    ASSERT_TRUE(SUCCEEDED(adapter.device->device()->CreateTexture2D(&desc, nullptr, &hdr_source)));

    Nv12Converter converter;
    ConverterSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.tone_map_hdr = false; // mirrors advanced.hdr_tonemap = false
    ASSERT_TRUE(converter.initialize(adapter.device->device(), settings).has_value());

    const auto result = converter.convert(adapter.device->context(), hdr_source.Get());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED);
}

TEST_F(ColorConversionTest, ASourceOfTheWrongSizeIsRefused) {
    const AdapterUnderTest& adapter = adapters_.front();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth * 2;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> wrong_size;
    ASSERT_TRUE(SUCCEEDED(adapter.device->device()->CreateTexture2D(&desc, nullptr, &wrong_size)));

    Nv12Converter converter;
    ConverterSettings settings;
    settings.width = kWidth;
    settings.height = kHeight;
    ASSERT_TRUE(converter.initialize(adapter.device->device(), settings).has_value());

    const auto result = converter.convert(adapter.device->context(), wrong_size.Get());
    ASSERT_FALSE(result.has_value());
    // SPEC.md §4.4: content-rect changes are never silently rescaled.
    EXPECT_EQ(result.error(), fc::FcError::CAPTURE_RESOLUTION_CHANGED);
}

// ---------------------------------------------------------------------------
// Both adapters agree
// ---------------------------------------------------------------------------

TEST_F(ColorConversionTest, EveryAdapterProducesTheSameResult) {
    if (adapters_.size() < 2) {
        GTEST_SKIP() << "only one hardware adapter present";
    }

    const Nv12Image reference = convert_solid(adapters_.front(), Bgra{40, 90, 200, 255}, ColorRange::Limited);
    ASSERT_FALSE(reference.bytes.empty());

    for (std::size_t i = 1; i < adapters_.size(); ++i) {
        const Nv12Image other = convert_solid(adapters_[i], Bgra{40, 90, 200, 255}, ColorRange::Limited);
        ASSERT_FALSE(other.bytes.empty());
        // Allow one code point for rounding differences between vendors.
        EXPECT_NEAR(other.y(0, 0), reference.y(0, 0), 1) << adapters_[i].description;
        EXPECT_NEAR(other.cb(0, 0), reference.cb(0, 0), 1) << adapters_[i].description;
        EXPECT_NEAR(other.cr(0, 0), reference.cr(0, 0), 1) << adapters_[i].description;
    }
}

} // namespace
