#include "core/gpu/encoder_probe.h"

#include "core/error/hresult.h"
#include "core/gpu/d3d_device.h"
#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <array>
#include <string>

namespace fc::gpu {
namespace {

using Microsoft::WRL::ComPtr;

/// Encoders to try, in the order SPEC.md §2.2 item 1 fixes them, filtered by vendor
/// so we do not spend 200 ms opening NVENC on an AMD part.
struct EncoderCandidate {
    std::uint32_t vendor_id;
    const char* name;
};

constexpr std::array kCandidates{
    EncoderCandidate{kVendorNvidia, "h264_nvenc"},
    EncoderCandidate{kVendorAmd, "h264_amf"},
    EncoderCandidate{kVendorIntel, "h264_qsv"},
};

struct BindFlagCandidate {
    std::uint32_t flags;
    const char* name;
};

/// Ordered most to least useful, not most to least likely.
///
/// `DECODER` is first-and-mandatory in practice: for a *texture array* of a video
/// format -- which is exactly what an encoder input pool is -- D3D11 rejects every
/// combination that omits it with `E_INVALIDARG`, on both reference adapters. That
/// is the real content of BUG-001, which originally read as an AMD quirk.
///
/// `UNORDERED_ACCESS` is what makes the pool writable by the conversion shader. If
/// the adapter takes it, the converter can write NV12 straight into a pool slice
/// and the encode path is zero-copy; if it does not, M3 has to convert into its own
/// texture and copy. Asking for it first means we find out rather than assume.
constexpr std::array kBindCandidates{
    BindFlagCandidate{D3D11_BIND_DECODER | D3D11_BIND_UNORDERED_ACCESS, "DECODER|UNORDERED_ACCESS"},
    BindFlagCandidate{D3D11_BIND_DECODER, "DECODER"},
    BindFlagCandidate{D3D11_BIND_RENDER_TARGET, "RENDER_TARGET"},
    BindFlagCandidate{D3D11_BIND_SHADER_RESOURCE, "SHADER_RESOURCE"},
    BindFlagCandidate{D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "SHADER_RESOURCE|RENDER_TARGET"},
};

/// The two views `Nv12Converter` writes through. D3D11 has no `PlaneSlice` -- that
/// is a D3D12 concept -- so the plane is selected by the view's format: `R8_UNORM`
/// reaches plane 0 (luma), `R8G8_UNORM` reaches plane 1 (interleaved Cb/Cr).
bool planar_uavs_creatable(ID3D11Device* device, ID3D11Texture2D* texture, unsigned array_size) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (array_size > 1) {
        // A pool slice, which is what an encoder input pool hands out.
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = 1;
    } else {
        desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    }

    desc.Format = DXGI_FORMAT_R8_UNORM;
    ComPtr<ID3D11UnorderedAccessView> luma;
    if (FAILED(device->CreateUnorderedAccessView(texture, &desc, &luma))) {
        return false;
    }

    desc.Format = DXGI_FORMAT_R8G8_UNORM;
    ComPtr<ID3D11UnorderedAccessView> chroma;
    return SUCCEEDED(device->CreateUnorderedAccessView(texture, &desc, &chroma));
}

std::string av_error_text(int err) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(err, buffer.data(), buffer.size());
    return std::string{buffer.data()};
}

/// RAII for the FFmpeg objects this probe creates. CLAUDE.md §4: every FFmpeg
/// object gets a wrapper; none of these may leak on an early return.
struct BufferRef {
    AVBufferRef* ref = nullptr;

    ~BufferRef() {
        if (ref != nullptr) {
            av_buffer_unref(&ref);
        }
    }

    BufferRef() = default;
    BufferRef(const BufferRef&) = delete;
    BufferRef& operator=(const BufferRef&) = delete;
    BufferRef(BufferRef&&) = delete;
    BufferRef& operator=(BufferRef&&) = delete;
};

struct CodecContext {
    AVCodecContext* ctx = nullptr;

    ~CodecContext() {
        if (ctx != nullptr) {
            avcodec_free_context(&ctx);
        }
    }

    CodecContext() = default;
    CodecContext(const CodecContext&) = delete;
    CodecContext& operator=(const CodecContext&) = delete;
    CodecContext(CodecContext&&) = delete;
    CodecContext& operator=(CodecContext&&) = delete;
};

} // namespace

std::uint32_t probe_nv12_pool_bind_flags(void* d3d11_device, int width, int height, unsigned array_size) {
    auto* device = static_cast<ID3D11Device*>(d3d11_device);
    if (device == nullptr) {
        return 0;
    }

    for (const BindFlagCandidate& candidate : kBindCandidates) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = array_size;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = candidate.flags;

        ComPtr<ID3D11Texture2D> probe;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &probe))) {
            continue;
        }

        // Accepting the texture is necessary but not sufficient. If we asked for
        // UNORDERED_ACCESS it was so the conversion shader could write the pool,
        // and that needs the two planar views to exist as well. Reporting the flag
        // without checking would move the failure into M3's hot path.
        if ((candidate.flags & D3D11_BIND_UNORDERED_ACCESS) != 0 &&
            !planar_uavs_creatable(device, probe.Get(), array_size)) {
            FC_LOG_DEBUG(Subsystem::Gpu, "NV12 pool accepted UNORDERED_ACCESS but planar UAVs were rejected",
                         LogFields{}.add("bind_flags", candidate.name));
            continue;
        }

        FC_LOG_DEBUG(Subsystem::Gpu, "NV12 encoder-input pool bind flags resolved",
                     LogFields{}.add("bind_flags", candidate.name));
        return candidate.flags;
    }

    FC_LOG_WARN(
        Subsystem::Gpu, "no NV12 encoder-input bind flags accepted",
        LogFields{}.add("width", width).add("height", height).add("array_size", static_cast<std::int64_t>(array_size)));
    return 0;
}

EncoderCapability probe_encoder(IDXGIAdapter1* adapter, std::uint32_t vendor_id, const ProbeSettings& settings) {
    EncoderCapability capability;
    capability.probed = true;

    if (adapter == nullptr) {
        capability.detail = "no adapter";
        return capability;
    }

    const Result<D3dDevice> device = create_device_on_adapter(adapter);
    if (!device.has_value()) {
        capability.detail = "D3D11 device creation failed on this adapter";
        return capability;
    }
    const D3dDevice& d3d = device.value();

    // The pool bind flags are part of the capability: an encoder that opens but
    // whose input pool cannot be allocated is not usable (BUG-001).
    capability.nv12_pool_bind_flags = probe_nv12_pool_bind_flags(d3d.device(), settings.width, settings.height, 4);
    if (capability.nv12_pool_bind_flags == 0) {
        capability.detail = "no NV12 encoder-input bind flags accepted by this adapter";
        return capability;
    }

    // Wrap the device for FFmpeg. av_hwdevice_ctx_create is deliberately not used --
    // it would pick its own adapter and defeat the entire point of a per-adapter probe.
    BufferRef hw_device;
    hw_device.ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hw_device.ref == nullptr) {
        capability.detail = "av_hwdevice_ctx_alloc failed";
        return capability;
    }
    {
        auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device.ref->data);
        auto* d3d_ctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
        d3d_ctx->device = d3d.device();
        d3d_ctx->device->AddRef(); // FFmpeg releases this on teardown.
        if (const int err = av_hwdevice_ctx_init(hw_device.ref); err < 0) {
            capability.detail = "av_hwdevice_ctx_init failed: " + av_error_text(err);
            return capability;
        }
    }

    BufferRef hw_frames;
    hw_frames.ref = av_hwframe_ctx_alloc(hw_device.ref);
    if (hw_frames.ref == nullptr) {
        capability.detail = "av_hwframe_ctx_alloc failed";
        return capability;
    }
    {
        auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames.ref->data);
        frames_ctx->format = AV_PIX_FMT_D3D11;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = settings.width;
        frames_ctx->height = settings.height;
        frames_ctx->initial_pool_size = 4; // throwaway session; keep it cheap
        static_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx)->BindFlags = capability.nv12_pool_bind_flags;
        if (const int err = av_hwframe_ctx_init(hw_frames.ref); err < 0) {
            capability.detail = "av_hwframe_ctx_init failed: " + av_error_text(err);
            return capability;
        }
    }

    for (const EncoderCandidate& candidate : kCandidates) {
        if (candidate.vendor_id != vendor_id) {
            continue;
        }

        const AVCodec* codec = avcodec_find_encoder_by_name(candidate.name);
        if (codec == nullptr) {
            capability.detail = std::string{candidate.name} + " is not present in this libavcodec build";
            continue;
        }

        CodecContext encoder;
        encoder.ctx = avcodec_alloc_context3(codec);
        if (encoder.ctx == nullptr) {
            capability.detail = "avcodec_alloc_context3 failed";
            continue;
        }

        encoder.ctx->width = settings.width;
        encoder.ctx->height = settings.height;
        encoder.ctx->pix_fmt = AV_PIX_FMT_D3D11;
        encoder.ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
        encoder.ctx->time_base = AVRational{1, 60000}; // SPEC.md §7.2
        encoder.ctx->framerate = AVRational{settings.fps, 1};
        encoder.ctx->max_b_frames = 0;
        encoder.ctx->hw_frames_ctx = av_buffer_ref(hw_frames.ref);
        if (encoder.ctx->hw_frames_ctx == nullptr) {
            capability.detail = "av_buffer_ref failed";
            continue;
        }

        const int err = avcodec_open2(encoder.ctx, codec, nullptr);
        if (err < 0) {
            capability.detail = std::string{candidate.name} + " failed to open: " + av_error_text(err);
            continue;
        }

        // Opened successfully. Record what the encoder actually negotiated -- if it
        // did not take d3d11 surfaces, zero-copy is already lost (SPEC.md §2.2).
        capability.h264 = true;
        capability.encoder_name = candidate.name;
        capability.detail = encoder.ctx->pix_fmt == AV_PIX_FMT_D3D11 ? "opened with d3d11 hardware surfaces"
                                                                     : "opened, but negotiated a software pixel format";
        // The context is destroyed here by CodecContext's destructor: "creating and
        // immediately destroying a throwaway encoder session".
        return capability;
    }

    if (capability.detail.empty()) {
        capability.detail = "no candidate encoder matches vendor 0x" + std::to_string(vendor_id);
    }
    return capability;
}

} // namespace fc::gpu
