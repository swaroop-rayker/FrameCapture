#include "core/color/nv12_converter.h"

#include "core/error/hresult.h"
#include "core/gpu/device_lock.h"
#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

// Generated at build time by fxc from bgra_to_nv12.hlsl.
#include "bgra_to_nv12.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace fc::color {
namespace {

using Microsoft::WRL::ComPtr;

/// Must match [numthreads(8, 8, 1)] in the shader.
constexpr UINT kThreadGroupSize = 8;

/// Must match cbuffer ConversionParams in bgra_to_nv12.hlsl.
struct alignas(16) ConversionParams {
    std::uint32_t luma_width = 0;
    std::uint32_t luma_height = 0;
    std::uint32_t full_range = 0;
    std::uint32_t source_is_scrgb = 0;
    float sdr_white_scale = 1.0f;
    float padding[3] = {0.0f, 0.0f, 0.0f};
};

/// scRGB defines 1.0 as the 80-nit SDR reference.
constexpr double kScrgbReferenceNits = 80.0;

} // namespace

struct Nv12Converter::Impl {
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> luma_uav;
    ComPtr<ID3D11UnorderedAccessView> chroma_uav;
    ConverterSettings settings;
    /// Mirror of the constant buffer, so a mid-session HDR switch only has to
    /// change one field rather than rebuild the struct from settings.
    ConversionParams params_value;
    bool scrgb_active = false;

    /// The device's critical section, so this dispatch's bind-and-go sequence cannot
    /// interleave with SPEC.md §15.2's preview dispatch on the capture thread. Cached
    /// rather than queried per frame. See `core/gpu/device_lock.h` -- the corruption it
    /// prevents is invisible to every counter in the pipeline.
    ComPtr<ID3D11Multithread> multithread;
};

Nv12Converter::Nv12Converter() : impl_(std::make_unique<Impl>()) {}

Nv12Converter::~Nv12Converter() = default;

Nv12Converter::Nv12Converter(Nv12Converter&&) noexcept = default;

Nv12Converter& Nv12Converter::operator=(Nv12Converter&&) noexcept = default;

Result<void> Nv12Converter::initialize(ID3D11Device* device, const ConverterSettings& settings) {
    if (device == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    // NV12 is 4:2:0: a chroma sample covers a 2x2 luma quad, so both dimensions
    // must be even or the last row/column has no chroma to belong to.
    if (settings.width <= 0 || settings.height <= 0 || (settings.width % 2) != 0 || (settings.height % 2) != 0) {
        FC_LOG_ERROR(Subsystem::Color, "NV12 conversion requires even, positive dimensions",
                     LogFields{}
                         .add("width", settings.width)
                         .add("height", settings.height)
                         .add_error(FcError::INTERNAL_INVALID_ARGUMENT));
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    impl_->settings = settings;

    {
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        impl_->multithread.Attach(gpu::acquire_multithread(context.Get()));
    }

    FC_HR_AS(device->CreateComputeShader(g_bgra_to_nv12_cs, sizeof(g_bgra_to_nv12_cs), nullptr, &impl_->shader),
             FcError::GPU_SHADER_COMPILE_FAILED);

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(ConversionParams);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    ConversionParams params;
    params.luma_width = static_cast<std::uint32_t>(settings.width);
    params.luma_height = static_cast<std::uint32_t>(settings.height);
    params.full_range = settings.range == ColorRange::Full ? 1u : 0u;
    params.source_is_scrgb = 0u; // set per frame, from the source format
    params.sdr_white_scale = static_cast<float>(settings.sdr_white_nits / kScrgbReferenceNits);
    impl_->params_value = params;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = &params;
    FC_HR_AS(device->CreateBuffer(&buffer_desc, &initial, &impl_->params), FcError::GPU_TEXTURE_CREATE_FAILED);

    // One NV12 texture, written through two planar UAVs. Both reference adapters
    // accept R8_UNORM over plane 0 and R8G8_UNORM over plane 1; the alternative --
    // two separate plane textures -- would need a copy into an NV12 surface later.
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = static_cast<UINT>(settings.width);
    texture_desc.Height = static_cast<UINT>(settings.height);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_NV12;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    FC_HR_AS(device->CreateTexture2D(&texture_desc, nullptr, &impl_->output), FcError::GPU_TEXTURE_CREATE_FAILED);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    uav_desc.Format = DXGI_FORMAT_R8_UNORM; // plane 0, luma
    FC_HR_AS(device->CreateUnorderedAccessView(impl_->output.Get(), &uav_desc, &impl_->luma_uav),
             FcError::GPU_TEXTURE_CREATE_FAILED);

    uav_desc.Format = DXGI_FORMAT_R8G8_UNORM; // plane 1, interleaved Cb/Cr
    FC_HR_AS(device->CreateUnorderedAccessView(impl_->output.Get(), &uav_desc, &impl_->chroma_uav),
             FcError::GPU_TEXTURE_CREATE_FAILED);

    FC_LOG_INFO(Subsystem::Color, "NV12 converter ready",
                LogFields{}
                    .add("width", settings.width)
                    .add("height", settings.height)
                    .add("matrix", "bt709")
                    .add("range", settings.range == ColorRange::Full ? "full" : "limited"));
    return ok();
}

Result<void> Nv12Converter::convert(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (context == nullptr || source == nullptr || impl_->shader == nullptr) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);

    // SPEC.md §4.2 / §6: an FP16 source means system HDR is on. Take the tone-map
    // branch, or refuse -- but never reinterpret those bits as BGRA8, which is the
    // black or neon-green output bug.
    const bool source_is_scrgb = source_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (source_is_scrgb && !impl_->settings.tone_map_hdr) {
        FC_LOG_ERROR(Subsystem::Color, "source is FP16 scRGB but HDR tone-mapping is disabled",
                     LogFields{}
                         .add("dxgi_format", static_cast<std::int64_t>(source_desc.Format))
                         .add("hint", "enable advanced.hdr_tonemap, or turn off system HDR for the target")
                         .add_error(FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED));
        return FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED;
    }
    if (source_is_scrgb != impl_->scrgb_active) {
        // The desktop switched HDR mode mid-session. Restamp the constant buffer
        // and say so once, rather than once per frame.
        impl_->scrgb_active = source_is_scrgb;
        impl_->params_value.source_is_scrgb = source_is_scrgb ? 1u : 0u;
        context->UpdateSubresource(impl_->params.Get(), 0, nullptr, &impl_->params_value, 0, 0);

        FC_LOG_INFO(Subsystem::Color,
                    source_is_scrgb ? "source is FP16 scRGB; tone-mapping to SDR BT.709" : "source is BGRA8 SDR",
                    LogFields{}.add("sdr_white_nits", impl_->settings.sdr_white_nits));
    }
    if (!source_is_scrgb && source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        FC_LOG_ERROR(Subsystem::Color, "unsupported source surface format",
                     LogFields{}
                         .add("dxgi_format", static_cast<std::int64_t>(source_desc.Format))
                         .add_error(FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED));
        return FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED;
    }
    if (std::cmp_not_equal(source_desc.Width, impl_->settings.width) ||
        std::cmp_not_equal(source_desc.Height, impl_->settings.height)) {
        // Not silently rescaled -- SPEC.md §4.4 routes size changes to the §14.3
        // resize policy instead.
        return FcError::CAPTURE_RESOLUTION_CHANGED;
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    // Deliberately _UNORM, never _UNORM_SRGB: an SRGB view applies a second gamma
    // decode and washes the output out (SPEC.md §6).
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = source_is_scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> source_srv;
    FC_HR_AS(device->CreateShaderResourceView(source, &srv_desc, &source_srv), FcError::GPU_TEXTURE_CREATE_FAILED);

    ID3D11ShaderResourceView* const srvs[] = {source_srv.Get()};
    ID3D11UnorderedAccessView* const uavs[] = {impl_->luma_uav.Get(), impl_->chroma_uav.Get()};
    ID3D11Buffer* const buffers[] = {impl_->params.Get()};

    // Compute-stage bindings are device-wide state and this is a sequence, not a call. The
    // preview's dispatch (SPEC.md §15.2) runs the same sequence from the capture thread,
    // and without this the two interleave and one `Dispatch` executes with the other's
    // shader and UAVs bound. See `core/gpu/device_lock.h` for the measurement.
    const gpu::ScopedDeviceLock lock{impl_->multithread.Get()};

    context->CSSetShader(impl_->shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, buffers);
    context->CSSetShaderResources(0, 1, srvs);
    context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    // One thread per chroma sample, i.e. per 2x2 luma quad.
    const UINT chroma_width = static_cast<UINT>(impl_->settings.width) / 2;
    const UINT chroma_height = static_cast<UINT>(impl_->settings.height) / 2;
    context->Dispatch((chroma_width + kThreadGroupSize - 1) / kThreadGroupSize,
                      (chroma_height + kThreadGroupSize - 1) / kThreadGroupSize, 1);

    // Unbind so the source texture is not still referenced when its owner recycles
    // it, and so the UAVs can be read as SRVs downstream.
    ID3D11ShaderResourceView* const no_srvs[] = {nullptr};
    ID3D11UnorderedAccessView* const no_uavs[] = {nullptr, nullptr};
    context->CSSetShaderResources(0, 1, no_srvs);
    context->CSSetUnorderedAccessViews(0, 2, no_uavs, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);

    return ok();
}

ID3D11Texture2D* Nv12Converter::output() const noexcept {
    return impl_->output.Get();
}

int Nv12Converter::width() const noexcept {
    return impl_->settings.width;
}

int Nv12Converter::height() const noexcept {
    return impl_->settings.height;
}

Result<std::vector<std::uint8_t>> read_back_nv12(ID3D11Device* device, ID3D11DeviceContext* context,
                                                 ID3D11Texture2D* nv12, int width, int height) {
    if (device == nullptr || context == nullptr || nv12 == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    FC_HR_AS(device->CreateTexture2D(&desc, nullptr, &staging), FcError::GPU_TEXTURE_CREATE_FAILED);

    context->CopyResource(staging.Get(), nv12);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    FC_HR_AS(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), FcError::GPU_TEXTURE_CREATE_FAILED);

    const auto luma_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> out(luma_bytes + (luma_bytes / 2));

    const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
        std::copy_n(base + (static_cast<std::size_t>(y) * mapped.RowPitch), static_cast<std::size_t>(width),
                    out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)));
    }

    // The chroma plane follows the luma plane at RowPitch * Height, sharing the
    // same pitch. This is the documented D3D11 layout for NV12 staging surfaces.
    const std::uint8_t* chroma_base = base + (static_cast<std::size_t>(mapped.RowPitch) * desc.Height);
    for (int y = 0; y < height / 2; ++y) {
        std::copy_n(chroma_base + (static_cast<std::size_t>(y) * mapped.RowPitch), static_cast<std::size_t>(width),
                    out.data() + luma_bytes + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)));
    }

    context->Unmap(staging.Get(), 0);
    return out;
}

} // namespace fc::color
