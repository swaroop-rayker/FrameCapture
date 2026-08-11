#pragma once

#include "core/error/result.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace fc::color {

/// SPEC.md §6: BT.709, limited range (16-235 luma / 16-240 chroma) by default.
/// Full range is a config option (`video.full_range`), and most players assume
/// limited, so changing it is a good way to get output that looks wrong elsewhere.
enum class ColorRange {
    Limited,
    Full,
};

struct ConverterSettings {
    int width = 1920;
    int height = 1080;
    ColorRange range = ColorRange::Limited;

    /// Accept an FP16 scRGB source and tone-map it to SDR BT.709 (SPEC.md §4.2).
    ///
    /// When false, an FP16 source is refused with
    /// `CAPTURE_SOURCE_FORMAT_UNSUPPORTED` rather than reinterpreted -- which is
    /// the correct failure, because reinterpreting those bits as BGRA8 produces the
    /// black or neon-green output §4.2 describes. Mirrors `advanced.hdr_tonemap`.
    bool tone_map_hdr = true;

    /// The display luminance that should read as diffuse white, in nits.
    ///
    /// scRGB defines 1.0 as the 80-nit SDR reference, but Windows composites the
    /// HDR desktop with SDR content considerably brighter. 203 nits is ITU-R
    /// BT.2408's reference white and is the sane default; the user-visible effect
    /// of getting it wrong is a recording that looks washed out or too dark.
    double sdr_white_nits = 203.0;
};

/// GPU BGRA8 -> NV12 conversion via a compute shader.
///
/// SPEC.md §2.1 rejects libswscale for this: on CPU at 1080p60 it burns ~1.5 cores
/// and is the single biggest cause of dropped frames in naive recorders. The
/// compute path is ~0.3 ms/frame and leaves the result on the device, which is what
/// makes zero-copy encode possible.
///
/// Not thread-safe. One instance per pipeline, used from the convert thread only
/// (SPEC.md §12).
class Nv12Converter {
public:
    Nv12Converter();
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;
    Nv12Converter(Nv12Converter&&) noexcept;
    Nv12Converter& operator=(Nv12Converter&&) noexcept;

    /// Compiles nothing at runtime -- the bytecode is embedded -- but does create
    /// the shader, the constant buffer, and an owned NV12 destination texture.
    [[nodiscard]] Result<void> initialize(ID3D11Device* device, const ConverterSettings& settings);

    /// Converts `source` (BGRA8) into the owned NV12 texture.
    ///
    /// `source` must be `DXGI_FORMAT_B8G8R8A8_UNORM` and match the configured size.
    /// The SRV is created per call because the source texture changes every frame;
    /// it is a cheap object and caching it by pointer would be a use-after-free
    /// waiting for a frame pool to recycle.
    [[nodiscard]] Result<void> convert(ID3D11DeviceContext* context, ID3D11Texture2D* source);

    /// The NV12 result. Owned by the converter; valid until the next `initialize`.
    [[nodiscard]] ID3D11Texture2D* output() const noexcept;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Reads an NV12 texture back to host memory in NV12 layout: the full luma plane
/// followed by the interleaved chroma plane, both tightly packed.
///
/// Staging readback stalls the GPU, so this is for tests, the raw-NV12 writer, and
/// diagnostics -- never the encode path, which keeps frames on the device.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_back_nv12(ID3D11Device* device, ID3D11DeviceContext* context,
                                                               ID3D11Texture2D* nv12, int width, int height);

} // namespace fc::color
