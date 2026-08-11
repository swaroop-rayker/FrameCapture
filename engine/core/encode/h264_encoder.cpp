#include "core/encode/video_encoder.h"

#include "core/error/hresult.h"
#include "core/gpu/encoder_probe.h"
#include "core/logging/logger.h"
#include "core/timing/frame_pacer.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <string>

namespace fc::encode {
namespace {

using Microsoft::WRL::ComPtr;

/// H.264 level 4.2, in the `level_idc` units libavcodec expects.
constexpr int kH264Level42 = 42;

/// SPEC.md §9: NVENC `p5` (`preset=slow`, `tune=hq`) is the sweet spot on Ada; `p7`
/// costs latency for negligible gain at 1080p.
constexpr const char* kNvencPreset = "p5";
constexpr const char* kNvencTune = "hq";

/// SPEC.md §9: AMF `quality` preset.
constexpr const char* kAmfQuality = "quality";

/// SPEC.md §13 rung 5: "Fall back to `libx264 superfast`".
constexpr const char* kX264Preset = "superfast";

bool is_nvenc(std::string_view name) noexcept {
    return name.find("nvenc") != std::string_view::npos;
}

bool is_amf(std::string_view name) noexcept {
    return name.find("amf") != std::string_view::npos;
}

bool is_software(std::string_view name) noexcept {
    return name == kSoftwareEncoderName;
}

class H264Encoder final : public IVideoEncoder {
public:
    Result<void> open(ID3D11Device* device, const VideoEncoderSettings& settings) override;
    Result<void> submit(ID3D11Texture2D* nv12, std::int64_t pts) override;
    Result<void> flush() override;
    Result<std::optional<EncodedPacket>> receive() override;

    [[nodiscard]] const AVCodecContext* codec_context() const noexcept override {
        return codec_.get();
    }

    [[nodiscard]] std::string_view encoder_name() const noexcept override {
        return encoder_name_;
    }

    [[nodiscard]] std::uint64_t submitted() const noexcept override {
        return submitted_;
    }

    [[nodiscard]] std::uint64_t received() const noexcept override {
        return received_;
    }

    [[nodiscard]] bool is_hardware() const noexcept override {
        return hardware_;
    }

    [[nodiscard]] bool try_set_cqp(int cqp) override;

    void request_keyframe() noexcept override {
        force_idr_next_ = true;
    }

private:
    [[nodiscard]] Result<void> create_hw_contexts(ID3D11Device* device, const VideoEncoderSettings& settings);
    void apply_rate_control(const VideoEncoderSettings& settings, ff::Dictionary& options) const;

    /// The hardware path: `nv12` is copied into a pool slice on the same adapter and
    /// handed to libavcodec as a D3D11 surface. No system memory, no PCIe.
    [[nodiscard]] Result<void> submit_hardware(ID3D11Texture2D* nv12, std::int64_t pts);

    /// SPEC.md §13 rung 5's path: `nv12` is downloaded to system memory, because
    /// libx264 encodes from there and nowhere else.
    [[nodiscard]] Result<void> submit_software(ID3D11Texture2D* nv12, std::int64_t pts);

    ff::CodecContext codec_;
    ff::BufferRef hw_device_;
    ff::BufferRef hw_frames_;
    ff::Frame frame_;
    ff::Packet packet_;

    /// Software path only: the system-memory NV12 frame the readback lands in, and
    /// the borrowed-texture wrapper it is downloaded from. Both reused across frames
    /// so the fallback path does not allocate two frames per frame.
    ff::Frame sw_frame_;
    ff::Frame hw_wrapper_;

    std::string encoder_name_;
    ComPtr<ID3D11DeviceContext> context_;
    std::uint64_t submitted_ = 0;
    std::uint64_t received_ = 0;
    bool flushed_ = false;
    bool hardware_ = true;

    /// SPEC.md §7.5's resume. Set by `request_keyframe`, consumed by whichever submit
    /// path runs next, and cleared there -- one request codes one IDR.
    bool force_idr_next_ = false;
};

Result<void> H264Encoder::create_hw_contexts(ID3D11Device* device, const VideoEncoderSettings& settings) {
    // Wrap the *existing* device rather than letting FFmpeg create its own. A
    // second device would land on whichever adapter DXGI picks first, which on a
    // MUX-less laptop is routinely not the one holding the pixels (SPEC.md §5.1).
    AVBufferRef* device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (device_ref == nullptr) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }
    hw_device_ = ff::BufferRef{device_ref};

    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
    auto* d3d11_ctx = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

    // FFmpeg takes ownership of this reference and will Release it.
    device->AddRef();
    d3d11_ctx->device = device;

    if (const int err = av_hwdevice_ctx_init(device_ref); err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "av_hwdevice_ctx_init failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_device_.get());
    if (frames_ref == nullptr) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }
    hw_frames_ = ff::BufferRef{frames_ref};

    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = settings.width;
    frames_ctx->height = settings.height;

    // The software path's frames context exists for the *download*, not for submission.
    // `av_hwframe_transfer_data` checks that the frame it is handed carries this exact
    // context, and the context owns the staging texture the download maps -- but nothing
    // ever allocates a frame from the pool, because `submit_software` wraps the
    // converter's own output texture instead.
    //
    // It still needs a pool FFmpeg will accept, and BUG-001 applies here in full: the
    // driver rejects an NV12 texture array whose bind flags it does not like, and which
    // flags it likes is **not portable between adapters** -- on the 780M the conventional
    // `RENDER_TARGET` is refused and only `DECODER` works. `SHADER_RESOURCE`, tried here
    // first, is refused too (E_INVALIDARG), which is the same lesson arriving by a
    // second route.
    //
    // So the flags are probed rather than assumed, using the function BUG-001 produced.
    // A caller on this path has no encoder capability to have probed for it -- there is
    // no encoder input surface at all -- so requiring it to supply a value would be
    // asking it to guess at exactly the thing that is not guessable.
    const bool software_pool = !hardware_;
    frames_ctx->initial_pool_size = software_pool ? 1 : settings.pool_size;

    std::uint32_t bind_flags = settings.pool_bind_flags;
    if (software_pool) {
        bind_flags = gpu::probe_nv12_pool_bind_flags(device, settings.width, settings.height, 1);
        if (bind_flags == 0) {
            FC_LOG_ERROR(Subsystem::Encode, "no NV12 bind flags were accepted for the software encoder's readback pool",
                         LogFields{}.add("encoder", encoder_name_).add_error(FcError::GPU_TEXTURE_CREATE_FAILED));
            return FcError::GPU_TEXTURE_CREATE_FAILED;
        }
    }

    // BUG-001: FFmpeg copies this straight into D3D11_TEXTURE2D_DESC.BindFlags and
    // supplies no default, so 0 means "a texture array with no bind flags" and the
    // driver rejects it.
    static_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx)->BindFlags = bind_flags;

    if (const int err = av_hwframe_ctx_init(frames_ref); err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "av_hwframe_ctx_init failed",
                     LogFields{}
                         .add("error", ff::error_text(err))
                         .add("bind_flags", static_cast<std::int64_t>(settings.pool_bind_flags))
                         .add_error(FcError::ENCODE_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }
    return ok();
}

void H264Encoder::apply_rate_control(const VideoEncoderSettings& settings, ff::Dictionary& options) const {
    const bool nvenc = is_nvenc(encoder_name_);
    const bool amf = is_amf(encoder_name_);
    const bool software = is_software(encoder_name_);

    switch (settings.rate_control) {
    case config::RateControl::Cqp:
        // SPEC.md §9: `-rc constqp -qp 20` on NVENC; a quality preset with a
        // bitrate ceiling on AMF, which has no true constant-QP mode.
        if (nvenc) {
            options.set("rc", "constqp");
            options.set("qp", static_cast<std::int64_t>(settings.cqp));
        } else if (amf) {
            options.set("rc", "cqp");
            options.set("qp_i", static_cast<std::int64_t>(settings.cqp));
            options.set("qp_p", static_cast<std::int64_t>(settings.cqp));
            options.set("qp_b", static_cast<std::int64_t>(settings.cqp));
        } else if (software) {
            // x264's own constant-QP mode. `qp` and not `crf`: CQP is what SPEC.md
            // §9 specifies and what rung 2 adjusts, and it is the one x264 honours
            // through `x264_encoder_reconfig` mid-stream.
            options.set("qp", static_cast<std::int64_t>(settings.cqp));
        }
        break;

    case config::RateControl::Vbr:
        codec_->bit_rate = static_cast<std::int64_t>(settings.bitrate_kbps) * 1000;
        // A ceiling well above the target; without one, "VBR" degenerates into
        // something close to CBR on both encoders.
        codec_->rc_max_rate = codec_->bit_rate * 2;
        codec_->rc_buffer_size = static_cast<int>(codec_->bit_rate);
        if (nvenc) {
            options.set("rc", "vbr");
        } else if (amf) {
            options.set("rc", "vbr_peak");
        }
        // x264 needs no `rc` option: setting `bit_rate` with no `qp` or `crf`
        // selects ABR, which is its VBR.
        break;

    case config::RateControl::Lossless:
        if (nvenc) {
            options.set("tune", "lossless");
        } else if (amf) {
            // AMF has no lossless mode; QP 0 is as close as it gets, and saying so
            // in the log is better than pretending the setting took effect.
            options.set("rc", "cqp");
            options.set("qp_i", static_cast<std::int64_t>(0));
            options.set("qp_p", static_cast<std::int64_t>(0));
            FC_LOG_WARN(Subsystem::Encode, "AMF has no true lossless mode; using QP 0",
                        LogFields{}.add("encoder", encoder_name_));
        } else if (software) {
            options.set("qp", static_cast<std::int64_t>(0));
        }
        break;
    }
}

Result<void> H264Encoder::open(ID3D11Device* device, const VideoEncoderSettings& settings) {
    if (device == nullptr || settings.encoder_name.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (settings.codec != config::VideoCodec::H264) {
        // The enum has three values and the probe reports all three, but v1 ships
        // H.264 only (SPEC.md §9). Refusing beats silently downgrading.
        FC_LOG_ERROR(
            Subsystem::Encode, "only H.264 is supported in v1",
            LogFields{}.add("codec", config::to_string(settings.codec)).add_error(FcError::ENCODE_CODEC_UNSUPPORTED));
        return FcError::ENCODE_CODEC_UNSUPPORTED;
    }
    if (settings.width <= 0 || settings.height <= 0 || settings.fps <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    encoder_name_ = settings.encoder_name;
    hardware_ = !is_software(encoder_name_);

    const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name_.c_str());
    if (codec == nullptr) {
        FC_LOG_ERROR(Subsystem::Encode, "encoder not present in this libavcodec build",
                     LogFields{}.add("encoder", encoder_name_).add_error(FcError::ENCODE_NO_HARDWARE_ENCODER));
        return FcError::ENCODE_NO_HARDWARE_ENCODER;
    }

    // The D3D11 contexts are created on **both** paths. The hardware path needs the
    // frame pool to submit into; the software path needs the same frames context to
    // download *out* of, because `av_hwframe_transfer_data` is what owns the NV12
    // plane arithmetic for a mapped D3D11 texture and reimplementing it here would
    // be a second copy of a layout rule that is already subtle (the UV plane's
    // offset is not simply `RowPitch * height` on every driver).
    FC_TRY(create_hw_contexts(device, settings));

    if (!codec_.alloc(codec)) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    codec_->width = settings.width;
    codec_->height = settings.height;
    // The software encoder takes system-memory NV12 directly; there is no hardware
    // surface format for it to negotiate.
    codec_->pix_fmt = hardware_ ? AV_PIX_FMT_D3D11 : AV_PIX_FMT_NV12;
    codec_->sw_pix_fmt = AV_PIX_FMT_NV12;

    // SPEC.md §7.2: one timebase for the whole video stream, chosen so a frame
    // duration is an exact integer.
    codec_->time_base = AVRational{1, static_cast<int>(timing::kVideoTimebaseDen)};
    codec_->framerate = AVRational{settings.fps, 1};

    // SPEC.md §9: High @ 4.2.
    codec_->profile = AV_PROFILE_H264_HIGH;
    codec_->level = kH264Level42;

    // 2 s, expressed in frames. Not 250, not "auto".
    codec_->gop_size = settings.gop_seconds * settings.fps;
    codec_->max_b_frames = settings.max_b_frames;

    // SPEC.md §6: these four must reach the SPS VUI, which is the only place a
    // player looks. Setting them on the context is what puts them there.
    codec_->colorspace = AVCOL_SPC_BT709;
    codec_->color_primaries = AVCOL_PRI_BT709;
    codec_->color_trc = AVCOL_TRC_BT709;
    codec_->color_range = settings.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    codec_->chroma_sample_location = AVCHROMA_LOC_LEFT;

    if (settings.global_header) {
        codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (hardware_) {
        // Only the hardware path advertises a frame pool to libavcodec. Setting this
        // on the software path would make libx264 reject the context: its pix_fmt is
        // NV12, and a hw_frames_ctx on a software encoder is a contradiction.
        codec_->hw_frames_ctx = hw_frames_.new_ref();
        if (codec_->hw_frames_ctx == nullptr) {
            return FcError::ENCODE_ENCODER_OPEN_FAILED;
        }
    }

    ff::Dictionary options;
    apply_rate_control(settings, options);

    if (is_nvenc(encoder_name_)) {
        options.set("preset", kNvencPreset);
        options.set("tune", kNvencTune);
        // SPEC.md §9's High profile, set as a **private option** because that is the
        // only thing `h264_nvenc` reads.
        //
        // BUG-028: `codec_->profile = AV_PROFILE_H264_HIGH` above has no effect on
        // NVENC. Its private `profile` option defaults to `NV_ENC_H264_PROFILE_MAIN`,
        // and `nvenc_setup_h264_config` *writes* `avctx->profile` from that option
        // rather than reading it -- the opposite direction from every other encoder
        // setting here. Every recording on the NVIDIA adapter was therefore Main
        // profile from M3 until this line existed, measured as `profile_idc` 0x4d in
        // the SPS against AMF's 0x64.
        options.set("profile", "high");
        // SPEC.md §9: no open-GOP. An open GOP makes the first frames after a
        // segment boundary reference pictures that are in the previous file.
        options.set("strict_gop", static_cast<std::int64_t>(1));
        // SPEC.md §7.5's resume needs `request_keyframe` to produce a genuine IDR. See
        // the note on `IVideoEncoder::request_keyframe`: with this at its default of 0,
        // `pict_type = I` yields `NV_ENC_PIC_FLAG_FORCEINTRA` -- an I-frame that does
        // not empty the reference buffer, so a later P-frame can still reference
        // pre-pause content.
        options.set("forced-idr", static_cast<std::int64_t>(1));
    } else if (is_amf(encoder_name_)) {
        options.set("quality", kAmfQuality);
        options.set("header_insertion_mode", "gop");
        // The same requirement, in AMF's spelling -- `forced_idr`, underscored, where
        // NVENC and libx264 both hyphenate. Setting the wrong one is not an error:
        // libavcodec leaves an unrecognised key in the dictionary, which is why `open`
        // logs whatever is left rather than discarding it.
        //
        // With this set, `amfenc.c` also asks AMF to insert SPS/PPS in band ahead of
        // the forced IDR. That is safe here and is not the hazard `MigrationParameterSetTest`
        // measured: those parameter sets come from the *same* encoder instance and are
        // byte-identical to the container's, where the migration case had two encoders
        // emitting different ones.
        options.set("forced_idr", static_cast<std::int64_t>(1));
    } else if (is_software(encoder_name_)) {
        options.set("preset", kX264Preset);
        // SPEC.md §9's closed GOP, in x264's spelling. Without it the frames after a
        // segment boundary reference pictures in the previous file (SPEC.md §11).
        options.set("x264-params", "open-gop=0:scenecut=0");
        options.set("forced-idr", static_cast<std::int64_t>(1));
    }

    if (const int err = avcodec_open2(codec_.get(), codec, options.address()); err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_open2 failed",
                     LogFields{}
                         .add("encoder", encoder_name_)
                         .add("error", ff::error_text(err))
                         .add_error(FcError::ENCODE_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    // Anything left in the dictionary was not recognised. A mistyped private
    // option otherwise vanishes without a word and the setting silently does
    // nothing.
    if (!options.empty()) {
        FC_LOG_WARN(Subsystem::Encode, "encoder ignored some options",
                    LogFields{}.add("encoder", encoder_name_).add("ignored", options.remaining()));
    }

    if (!frame_.alloc() || !packet_.alloc()) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    // Allocated here rather than lazily in `submit`, so the software path's hot loop
    // has no allocation branch and a machine out of memory finds out at open time.
    if (!hardware_ && (!sw_frame_.alloc() || !hw_wrapper_.alloc())) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    device->GetImmediateContext(&context_);
    if (context_.Get() == nullptr) {
        return FcError::ENCODE_ENCODER_OPEN_FAILED;
    }

    FC_LOG_INFO(Subsystem::Encode, "video encoder opened",
                LogFields{}
                    .add("encoder", encoder_name_)
                    // SPEC.md §13 rung 5. Worth a field of its own rather than
                    // leaving it to be inferred from the name: "why is this
                    // recording slow" is answered by this line.
                    .add("hardware", hardware_)
                    .add("width", settings.width)
                    .add("height", settings.height)
                    .add("fps", settings.fps)
                    .add("rate_control", config::to_string(settings.rate_control))
                    .add("gop_frames", codec_->gop_size)
                    .add("max_b_frames", codec_->max_b_frames)
                    .add("color_range", settings.full_range ? "full" : "limited"));
    return ok();
}

Result<void> H264Encoder::submit(ID3D11Texture2D* nv12, std::int64_t pts) {
    if (!codec_ || nv12 == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    return hardware_ ? submit_hardware(nv12, pts) : submit_software(nv12, pts);
}

Result<void> H264Encoder::submit_software(ID3D11Texture2D* nv12, std::int64_t pts) {
    // Wrap the caller's texture as a borrowed D3D11 frame. Borrowed deliberately:
    // there is no `av_buffer_ref` on it and it is never unreffed, because the
    // pipeline owns the texture and recycles it after `submit` returns. What makes
    // that safe is that the download below is synchronous -- by the time this
    // function returns, the pixels are in system memory.
    //
    // `d3d11va_transfer_data` requires only that the frame carry *this* frames
    // context; it does not require the texture to have come from the pool. That is
    // what lets the converter's own output texture be downloaded without a pointless
    // GPU-to-GPU copy into a pool slice first.
    hw_wrapper_.unref();
    hw_wrapper_->format = AV_PIX_FMT_D3D11;
    hw_wrapper_->width = codec_->width;
    hw_wrapper_->height = codec_->height;
    hw_wrapper_->data[0] = reinterpret_cast<std::uint8_t*>(nv12);
    hw_wrapper_->data[1] = nullptr; // subresource index 0
    hw_wrapper_->hw_frames_ctx = hw_frames_.new_ref();
    if (hw_wrapper_->hw_frames_ctx == nullptr) {
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    // Reused across frames, so it has to be made writable first: libx264 may still
    // hold a reference to the previous frame's buffer, and overwriting it would
    // corrupt a frame that has not been encoded yet. `av_frame_make_writable` copies
    // only when that is actually the case, so a steady state with no outstanding
    // reference reuses the same allocation.
    if (sw_frame_->buf[0] != nullptr) {
        if (const int err = av_frame_make_writable(sw_frame_.get()); err < 0) {
            FC_LOG_ERROR(Subsystem::Encode, "could not make the readback frame writable",
                         LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
            return FcError::ENCODE_SUBMIT_FAILED;
        }
    } else {
        sw_frame_->format = AV_PIX_FMT_NV12;
        sw_frame_->width = codec_->width;
        sw_frame_->height = codec_->height;
    }

    // The GPU-to-CPU readback, and the reason rung 5 is the bottom of the ladder:
    // this is a full pipeline sync plus ~3 MB across the bus per frame at 1080p. It
    // is what libx264 requires, and a machine on this path has no hardware encoder
    // to be slower than.
    if (const int err = av_hwframe_transfer_data(sw_frame_.get(), hw_wrapper_.get(), 0); err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "downloading the NV12 surface for software encode failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    // `transfer_data` copies pixels and nothing else -- colour properties are the
    // encoder's business and are not carried on the frame. The PTS is, and it is the
    // pacer's quantized value (SPEC.md §7.2), not a clock reading.
    sw_frame_->pts = pts;

    // Assigned unconditionally rather than only when a keyframe was asked for: this
    // frame is reused across submissions and is never unreffed, so a `pict_type` left
    // behind from a previous request would force an IDR on every subsequent frame --
    // which is a silent bitrate multiplier, not a visible failure.
    sw_frame_->pict_type = force_idr_next_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    force_idr_next_ = false;

    const int err = avcodec_send_frame(codec_.get(), sw_frame_.get());
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_send_frame failed on the software path",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    ++submitted_;
    return ok();
}

Result<void> H264Encoder::submit_hardware(ID3D11Texture2D* nv12, std::int64_t pts) {
    // A fresh pool slice per frame. The pool is bounded (`pool_size`), so this
    // blocks only if every slice is still referenced by the encoder -- which is
    // backpressure, not a leak.
    frame_.unref();
    if (const int err = av_hwframe_get_buffer(hw_frames_.get(), frame_.get(), 0); err < 0) {
        // The pool is fixed-size, so exhaustion means the encoder is still holding
        // every surface. That is backpressure, not a fault: the caller drains
        // packets -- which is what releases surfaces -- and submits again. Reported
        // distinctly so the caller can tell it apart from a real encode failure.
        FC_LOG_TRACE(Subsystem::Encode, "encoder input pool exhausted", LogFields{}.add("error", ff::error_text(err)));
        return FcError::INTERNAL_QUEUE_FULL;
    }

    // The pool hands out (texture array, slice index) rather than a standalone
    // texture, so the copy names a subresource rather than the whole resource.
    auto* destination = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
    const auto slice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame_->data[1]));
    if (destination == nullptr) {
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    // GPU-local copy into the encoder's pool. This stays on the adapter that owns
    // both textures -- no `av_hwframe_transfer_data`, no system memory, no PCIe
    // (SPEC.md §9). It is not free, and eliminating it by converting straight into
    // the pool slice is now possible (BUG-001 follow-up proved the pool accepts
    // UNORDERED_ACCESS); that is a measured optimisation, not a correctness fix.
    context_->CopySubresourceRegion(destination, slice, 0, 0, 0, nv12, 0, nullptr);

    frame_->pts = pts;

    // `frame_.unref()` above resets this to `AV_PICTURE_TYPE_NONE` every submission, so
    // the hardware path needs no counterpart to the software path's explicit clear --
    // but it is still assigned rather than left implicit, because the two paths reading
    // differently is how one of them silently stops honouring a resume.
    frame_->pict_type = force_idr_next_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    force_idr_next_ = false;

    const int err = avcodec_send_frame(codec_.get(), frame_.get());
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_send_frame failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    ++submitted_;
    return ok();
}

bool H264Encoder::try_set_cqp(int cqp) {
    if (!codec_ || codec_->priv_data == nullptr) {
        return false;
    }

    // Software only, and this is a measurement rather than a preference. Reading the
    // FFmpeg n8.1.2 sources this build links:
    //
    //   libx264   `reconfig_encoder` runs before every frame and compares
    //             `params.rc.i_qp_constant` against the private `qp` option, calling
    //             `x264_encoder_reconfig` when they differ. Setting the option takes
    //             effect on the next frame.
    //   h264_nvenc `reconfig_encoder` reconfigures `averageBitRate`, `maxBitRate` and
    //             `vbvBufferSize` only, and the whole block is gated on
    //             `ctx->rc != NV_ENC_PARAMS_RC_CONSTQP`. QP is written once into
    //             `encode_config.rcParams.constQP` at init. SPEC.md §9 makes CQP the
    //             default, so even the bitrate path is unreachable as configured.
    //   h264_amf  every property is assigned in `amf_encode_init`; nothing is
    //             revisited per frame.
    //
    // Reopening the encoder is not an alternative worth taking here: it would emit
    // fresh SPS/PPS, and the container's `avcC`/`CodecPrivate` was fixed by
    // `avformat_write_header` (SPEC.md §10.1). See BUG-024.
    if (hardware_) {
        return false;
    }

    if (const int err = av_opt_set_int(codec_->priv_data, "qp", cqp, 0); err < 0) {
        FC_LOG_WARN(Subsystem::Encode, "the software encoder refused a quality change",
                    LogFields{}.add("encoder", encoder_name_).add("qp", cqp).add("error", ff::error_text(err)));
        return false;
    }
    return true;
}

Result<void> H264Encoder::flush() {
    if (!codec_) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (flushed_) {
        // Idempotent: SPEC.md §10.4 requires every finalization stage to be, and a
        // second null frame would be an API misuse error from libavcodec.
        return ok();
    }
    flushed_ = true;

    // A null frame is the end-of-stream signal. Everything the encoder was holding
    // for reorder comes out of `receive` after this.
    if (const int err = avcodec_send_frame(codec_.get(), nullptr); err < 0 && err != AVERROR_EOF) {
        FC_LOG_ERROR(Subsystem::Encode, "flushing the encoder failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
        return FcError::ENCODE_SUBMIT_FAILED;
    }
    return ok();
}

Result<std::optional<EncodedPacket>> H264Encoder::receive() {
    if (!codec_) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    packet_.unref();
    const int err = avcodec_receive_packet(codec_.get(), packet_.get());
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        return std::optional<EncodedPacket>{};
    }
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_receive_packet failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_RECEIVE_FAILED));
        return FcError::ENCODE_RECEIVE_FAILED;
    }

    EncodedPacket out;
    if (!out.packet.alloc()) {
        return FcError::ENCODE_RECEIVE_FAILED;
    }
    // Moves the payload rather than copying it; `packet_` is left blank and ready
    // for the next call.
    av_packet_move_ref(out.packet.get(), packet_.get());
    out.keyframe = (out.packet->flags & AV_PKT_FLAG_KEY) != 0;

    ++received_;
    return std::optional<EncodedPacket>{std::move(out)};
}

} // namespace

Result<std::unique_ptr<IVideoEncoder>> create_video_encoder(const VideoEncoderSettings& settings) {
    if (settings.codec != config::VideoCodec::H264) {
        return FcError::ENCODE_CODEC_UNSUPPORTED;
    }
    return std::unique_ptr<IVideoEncoder>{std::make_unique<H264Encoder>()};
}

} // namespace fc::encode
