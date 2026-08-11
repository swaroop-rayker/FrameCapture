#pragma once

// SPEC.md §15.2's "separate GPU shader dispatch off the same source texture".
//
// Deliberately just the dispatch. It owns a shader, a constant buffer and one destination
// texture, and it knows nothing about shared memory, threads or readback -- so the
// measurement §15.2 actually specifies ("< 0.2 ms/frame") is a measurement of *this*, and
// `PreviewDispatchCostTest` can wrap timestamp queries around one call rather than around
// a pipeline stage that also copies and publishes.

#include "core/error/result.h"

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace fc::preview {

struct PreviewScalerSettings {
    /// Destination size. SPEC.md §15.2's 960x540 by default, and **never** the source's --
    /// "never full-resolution frames" is the sentence this class exists to obey.
    int width = 960;
    int height = 540;

    /// Accept an FP16 scRGB source and tone-map it (SPEC.md §4.2), mirroring
    /// `color::ConverterSettings::tone_map_hdr`. When false an FP16 source is refused with
    /// `CAPTURE_SOURCE_FORMAT_UNSUPPORTED` rather than reinterpreted.
    bool tone_map_hdr = true;

    /// The display luminance that reads as diffuse white, in nits. Same default and same
    /// meaning as the encoder's, because the two share the curve.
    double sdr_white_nits = 203.0;
};

/// BGRA8 (or FP16 scRGB) -> downscaled BGRA8, on the GPU.
///
/// Not thread-safe. One instance per capture session, driven from the thread that holds
/// the source texture (SPEC.md §12).
class PreviewScaler {
public:
    PreviewScaler();
    ~PreviewScaler();

    PreviewScaler(const PreviewScaler&) = delete;
    PreviewScaler& operator=(const PreviewScaler&) = delete;
    PreviewScaler(PreviewScaler&&) noexcept;
    PreviewScaler& operator=(PreviewScaler&&) noexcept;

    [[nodiscard]] Result<void> initialize(ID3D11Device* device, const PreviewScalerSettings& settings);

    /// Downscales `source` into the owned destination texture.
    ///
    /// `source` may be any size; the filter re-derives its tap count when the size changes,
    /// which is the case SPEC.md §14.3's resolution change produces. It may **not** be a
    /// format other than `B8G8R8A8_UNORM` or `R16G16B16A16_FLOAT` -- the same two the
    /// encoder accepts, refused the same way for the same reason (§4.2).
    [[nodiscard]] Result<void> dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* source);

    /// The result: `DXGI_FORMAT_R8G8B8A8_UNORM` holding **BGRA byte order**. See the
    /// shader's header note for why the format and the byte order disagree on purpose.
    [[nodiscard]] ID3D11Texture2D* output() const noexcept;

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::preview
