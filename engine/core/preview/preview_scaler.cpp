#include "core/preview/preview_scaler.h"

#include "core/error/hresult.h"
#include "core/gpu/device_lock.h"
#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

// Generated at build time by fxc from downscale_preview.hlsl.
#include "downscale_preview.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fc::preview {
namespace {

using Microsoft::WRL::ComPtr;

/// Must match [numthreads(8, 8, 1)] in the shader.
constexpr UINT kThreadGroupSize = 8;

/// Ceiling on the taps per axis.
///
/// Each bilinear tap covers two source pixels, so four taps is a 8x8 box -- enough for a
/// 4K source scaled to 960x540 to be filtered rather than aliased. Beyond that the cost
/// grows quadratically for a difference nobody can see in a 960-pixel-wide preview, and
/// §15.2's budget is 0.2 ms.
constexpr int kMaxTaps = 4;

/// scRGB defines 1.0 as the 80-nit SDR reference. Same constant as the encoder's.
constexpr double kScrgbReferenceNits = 80.0;

/// Must match cbuffer PreviewParams in downscale_preview.hlsl.
struct alignas(16) PreviewParams {
    std::uint32_t destination_width = 0;
    std::uint32_t destination_height = 0;
    float source_per_dest_x = 1.0f;
    float source_per_dest_y = 1.0f;
    float inverse_source_x = 1.0f;
    float inverse_source_y = 1.0f;
    std::uint32_t taps_x = 1;
    std::uint32_t taps_y = 1;
    std::uint32_t source_is_scrgb = 0;
    float sdr_white_scale = 1.0f;
    float padding[2] = {0.0f, 0.0f};
};

/// Bilinear taps for one axis. One tap per two source pixels of the box, clamped.
[[nodiscard]] std::uint32_t taps_for(UINT source, int destination) noexcept {
    if (destination <= 0) {
        return 1;
    }
    const double ratio = static_cast<double>(source) / static_cast<double>(destination);
    const int taps = static_cast<int>(std::lround(ratio / 2.0));
    return static_cast<std::uint32_t>(std::clamp(taps, 1, kMaxTaps));
}

} // namespace

struct PreviewScaler::Impl {
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> params;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> output_uav;
    PreviewScalerSettings settings;

    /// Mirror of the constant buffer, so the per-frame path only touches it when the
    /// source's size or format has actually changed.
    PreviewParams params_value;
    UINT source_width = 0;
    UINT source_height = 0;
    bool scrgb_active = false;

    /// The device's critical section. The other holder is `Nv12Converter`, on the venc
    /// thread; both must take it or neither is protected. See `core/gpu/device_lock.h`.
    ComPtr<ID3D11Multithread> multithread;
};

PreviewScaler::PreviewScaler() : impl_(std::make_unique<Impl>()) {}

PreviewScaler::~PreviewScaler() = default;

PreviewScaler::PreviewScaler(PreviewScaler&&) noexcept = default;

PreviewScaler& PreviewScaler::operator=(PreviewScaler&&) noexcept = default;

Result<void> PreviewScaler::initialize(ID3D11Device* device, const PreviewScalerSettings& settings) {
    if (device == nullptr || settings.width <= 0 || settings.height <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    impl_->settings = settings;
    impl_->source_width = 0;
    impl_->source_height = 0;
    impl_->scrgb_active = false;

    {
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        impl_->multithread.Attach(gpu::acquire_multithread(context.Get()));
    }

    FC_HR_AS(
        device->CreateComputeShader(g_downscale_preview_cs, sizeof(g_downscale_preview_cs), nullptr, &impl_->shader),
        FcError::GPU_SHADER_COMPILE_FAILED);

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(PreviewParams);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    PreviewParams params;
    params.destination_width = static_cast<std::uint32_t>(settings.width);
    params.destination_height = static_cast<std::uint32_t>(settings.height);
    params.sdr_white_scale = static_cast<float>(settings.sdr_white_nits / kScrgbReferenceNits);
    impl_->params_value = params;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = &params;
    FC_HR_AS(device->CreateBuffer(&buffer_desc, &initial, &impl_->params), FcError::GPU_TEXTURE_CREATE_FAILED);

    // CLAMP rather than WRAP: the last tap of the last destination pixel can land a
    // half-texel past the edge, and wrapping there would fold the left edge of the desktop
    // into its right one -- a one-pixel seam that looks like a capture bug.
    // Every field named, including the two enums that have no zero enumerator
    // (`D3D11_TEXTURE_ADDRESS_MODE`, `D3D11_COMPARISON_FUNC`). A `{}` here would
    // value-initialise those to 0, which is not a value either enum defines --
    // `bugprone-invalid-enum-default-initialization` is right to refuse it.
    const D3D11_SAMPLER_DESC sampler_desc{
        .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .MipLODBias = 0.0f,
        .MaxAnisotropy = 1,
        // Never compared -- this is not a comparison sampler -- but it must name a real
        // enumerator rather than 0.
        .ComparisonFunc = D3D11_COMPARISON_NEVER,
        .BorderColor = {0.0f, 0.0f, 0.0f, 0.0f},
        .MinLOD = 0.0f,
        .MaxLOD = D3D11_FLOAT32_MAX,
    };
    FC_HR_AS(device->CreateSamplerState(&sampler_desc, &impl_->sampler), FcError::GPU_TEXTURE_CREATE_FAILED);

    // R8G8B8A8_UNORM, holding BGRA byte order. B8G8R8A8_UNORM is not in D3D11's
    // guaranteed typed-UAV-store set, so the swizzle happens in the shader's store; see
    // the shader's header note.
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture_desc.Width = static_cast<UINT>(settings.width);
    texture_desc.Height = static_cast<UINT>(settings.height);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    FC_HR_AS(device->CreateTexture2D(&texture_desc, nullptr, &impl_->output), FcError::GPU_TEXTURE_CREATE_FAILED);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    FC_HR_AS(device->CreateUnorderedAccessView(impl_->output.Get(), &uav_desc, &impl_->output_uav),
             FcError::GPU_TEXTURE_CREATE_FAILED);

    FC_LOG_INFO(Subsystem::Color, "preview scaler ready",
                LogFields{}.add("width", settings.width).add("height", settings.height));
    return ok();
}

Result<void> PreviewScaler::dispatch(ID3D11DeviceContext* context, ID3D11Texture2D* source) {
    if (context == nullptr || source == nullptr || impl_->shader == nullptr) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);

    // SPEC.md §4.2: FP16 means system HDR is on. Tone-map or refuse -- never reinterpret.
    const bool source_is_scrgb = source_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (source_is_scrgb && !impl_->settings.tone_map_hdr) {
        return FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED;
    }
    if (!source_is_scrgb && source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return FcError::CAPTURE_SOURCE_FORMAT_UNSUPPORTED;
    }
    if (source_desc.Width == 0 || source_desc.Height == 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    if (source_desc.Width != impl_->source_width || source_desc.Height != impl_->source_height ||
        source_is_scrgb != impl_->scrgb_active) {
        // The source's geometry, restamped only when it changes. A resolution change
        // (SPEC.md §14.3) is the case this exists for -- and unlike the encoder's, the
        // preview may simply follow it, because nothing downstream of the ring has
        // committed to a source size.
        impl_->source_width = source_desc.Width;
        impl_->source_height = source_desc.Height;
        impl_->scrgb_active = source_is_scrgb;

        impl_->params_value.source_per_dest_x =
            static_cast<float>(source_desc.Width) / static_cast<float>(impl_->settings.width);
        impl_->params_value.source_per_dest_y =
            static_cast<float>(source_desc.Height) / static_cast<float>(impl_->settings.height);
        impl_->params_value.inverse_source_x = 1.0f / static_cast<float>(source_desc.Width);
        impl_->params_value.inverse_source_y = 1.0f / static_cast<float>(source_desc.Height);
        impl_->params_value.taps_x = taps_for(source_desc.Width, impl_->settings.width);
        impl_->params_value.taps_y = taps_for(source_desc.Height, impl_->settings.height);
        impl_->params_value.source_is_scrgb = source_is_scrgb ? 1U : 0U;
        context->UpdateSubresource(impl_->params.Get(), 0, nullptr, &impl_->params_value, 0, 0);

        FC_LOG_INFO(Subsystem::Color, "preview scaler retargeted",
                    LogFields{}
                        .add("source_width", static_cast<std::int64_t>(source_desc.Width))
                        .add("source_height", static_cast<std::int64_t>(source_desc.Height))
                        .add("taps_x", static_cast<std::int64_t>(impl_->params_value.taps_x))
                        .add("taps_y", static_cast<std::int64_t>(impl_->params_value.taps_y))
                        .add("scrgb", source_is_scrgb));
    }

    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    // _UNORM, never _UNORM_SRGB (SPEC.md §6). The SRGB view would apply a second gamma
    // decode, and a washed-out preview of a correct recording is worse than no preview --
    // it sends the user looking for a colour bug that is not there.
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = source_is_scrgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> source_srv;
    FC_HR_AS(device->CreateShaderResourceView(source, &srv_desc, &source_srv), FcError::GPU_TEXTURE_CREATE_FAILED);

    ID3D11ShaderResourceView* const srvs[] = {source_srv.Get()};
    ID3D11UnorderedAccessView* const uavs[] = {impl_->output_uav.Get()};
    ID3D11Buffer* const buffers[] = {impl_->params.Get()};
    ID3D11SamplerState* const samplers[] = {impl_->sampler.Get()};

    // Held across the whole bind-and-go sequence, for the reason `Nv12Converter::convert`
    // states and `core/gpu/device_lock.h` measures: this dispatch runs on the capture
    // thread and that one on `venc`, against one immediate context whose compute-stage
    // bindings they share.
    //
    // This is the one place the preview can hold something the recording wants, so it is
    // worth being precise about what it costs: the section is held for the six calls
    // below and no longer -- no map, no readback, no allocation, no wait. Everything with
    // a duration proportional to the picture happens on `fc-preview`, outside it.
    const gpu::ScopedDeviceLock lock{impl_->multithread.Get()};

    context->CSSetShader(impl_->shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, buffers);
    context->CSSetShaderResources(0, 1, srvs);
    context->CSSetSamplers(0, 1, samplers);
    context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    context->Dispatch((static_cast<UINT>(impl_->settings.width) + kThreadGroupSize - 1) / kThreadGroupSize,
                      (static_cast<UINT>(impl_->settings.height) + kThreadGroupSize - 1) / kThreadGroupSize, 1);

    // Unbound so the capture backend can recycle the source texture without it still being
    // referenced by the compute stage, and so the destination can be copied from.
    ID3D11ShaderResourceView* const no_srvs[] = {nullptr};
    ID3D11UnorderedAccessView* const no_uavs[] = {nullptr};
    context->CSSetShaderResources(0, 1, no_srvs);
    context->CSSetUnorderedAccessViews(0, 1, no_uavs, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    return ok();
}

ID3D11Texture2D* PreviewScaler::output() const noexcept {
    return impl_->output.Get();
}

int PreviewScaler::width() const noexcept {
    return impl_->settings.width;
}

int PreviewScaler::height() const noexcept {
    return impl_->settings.height;
}

} // namespace fc::preview
