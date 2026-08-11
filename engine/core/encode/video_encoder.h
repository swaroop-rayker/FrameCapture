#pragma once

// Video encoding (SPEC.md §9).
//
// One interface over the hardware encoders, all reached through libavcodec --
// SPEC.md §2.2 item 1 forbids direct vendor SDK calls anywhere in fc_core, so
// `h264_nvenc` and `h264_amf` differ here only by name and by which private
// options are set.
//
// v1 encodes H.264 High @ 4.2 and nothing else (SPEC.md §9). The codec enum has
// three values and the capability probe reports all three, but every other value
// is refused here rather than silently downgraded.
//
// libx264 is deliberately absent: it is GPLv2 and that licensing decision is still
// open (CLAUDE.md §9). Its absence is what makes degradation-ladder rung 5
// (SPEC.md §13) unavailable, and `ENCODE_NO_HARDWARE_ENCODER` is the honest answer
// on a machine with no hardware encoder rather than a silent software fallback.

#include "core/config/config_schema.h"
#include "core/error/result.h"
#include "core/ffmpeg/av_raii.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace fc::encode {

/// SPEC.md §13 rung 5's encoder. The one name in the engine that is not supplied by
/// the per-adapter capability probe, because it is the answer for a machine where
/// every probe came back negative (`gpu::SelectionRule::SoftwareFallback`).
inline constexpr const char* kSoftwareEncoderName = "libx264";

/// Everything the encoder needs that is not derivable from the device.
struct VideoEncoderSettings {
    int width = 1920;
    int height = 1080;
    int fps = 60;

    config::VideoCodec codec = config::VideoCodec::H264;
    config::RateControl rate_control = config::RateControl::Cqp;

    /// SPEC.md §9: CQP 20 is the default. CBR is wrong for local recording -- it
    /// spends bits on a static screen that a quality-targeted mode simply does not
    /// emit.
    int cqp = 20;
    int bitrate_kbps = 20000;

    /// Keyframe interval in seconds. SPEC.md §9 fixes this at 2 -- not 250 frames,
    /// not "auto" -- because it sets both seek granularity and the segment
    /// boundaries M9 splits on.
    int gop_seconds = 2;
    int max_b_frames = 2;

    /// False = limited range (16-235), the SPEC.md §6 default.
    bool full_range = false;

    /// libavcodec encoder name from the capability probe, e.g. "h264_nvenc".
    std::string encoder_name;

    /// D3D11 bind flags for the encoder input pool, resolved per adapter by
    /// `gpu::probe_nv12_pool_bind_flags` (BUG-001). Passing 0 is not "let FFmpeg
    /// decide" -- FFmpeg copies the field through untouched and the driver rejects
    /// a texture array with no bind flags.
    std::uint32_t pool_bind_flags = 0;

    /// Emit SPS/PPS as codec extradata rather than only in-band.
    ///
    /// Both containers v1 targets want this: Matroska stores the parameter sets in
    /// `CodecPrivate` and MP4 in `avcC`, and `avformat_write_header` fails outright
    /// without them. Whether an encoder supplies extradata by default is
    /// vendor-specific -- AMF happens to, NVENC does not -- so relying on the
    /// default means the file writes on one adapter and refuses on the other.
    bool global_header = true;

    /// Textures in the encoder's hardware input pool.
    ///
    /// Distinct from the SPEC.md §9 encoder *queue* capacity of 8. That bounds how
    /// many frames wait to be submitted; this bounds how many the encoder can hold
    /// at once, and hardware encoders hold a surface for every frame they have
    /// accepted but not yet emitted a packet for. NVENC's pipeline is several
    /// frames deep before it produces anything, so a pool of 8 is exhausted after
    /// 8 submissions and every subsequent frame fails to acquire one.
    ///
    /// The pool is fixed-size by design -- `av_hwframe_get_buffer` failing is
    /// backpressure, which the caller handles by draining packets and retrying.
    /// Growing it on demand would be an unbounded queue wearing a different hat
    /// (CLAUDE.md hard rule 5).
    int pool_size = 32;
};

/// One encoded packet on its way to the muxer.
///
/// Owns its `AVPacket`, so the encode thread hands ownership across the queue and
/// the mux thread frees it. The alternative -- a borrowed packet with a promise
/// about lifetime -- is how a single-writer muxer ends up reading freed memory.
struct EncodedPacket {
    ff::Packet packet;
    /// True for an IDR. The muxer needs this for MKV cues, and M9 needs it because
    /// a segment boundary that is not keyframe-aligned makes the next file's
    /// opening seconds unplayable (SPEC.md §11).
    bool keyframe = false;

    /// Which stream this belongs to. Both encoders feed one queue and one mux
    /// thread -- SPEC.md §10.1 allows exactly one `AVFormatContext` owner -- so
    /// the packet has to carry its own destination.
    bool audio = false;

    /// Which audio track, when `audio` is true. Ignored otherwise.
    ///
    /// 0 is SPEC.md §8.6's system mix, which is every recording's only audio track
    /// unless Tier B is on -- so the default is the Tier A answer and nothing on
    /// that path sets this field. 1..5 are the per-application tracks.
    ///
    /// An index rather than a stream pointer: SPEC.md §11's rollover opens a *new*
    /// `AVFormatContext` mid-recording, and a packet already in the mux queue when
    /// that happens must land on the equivalent stream of the new file rather than
    /// on a pointer into the old one.
    int audio_track = 0;
};

/// The encoder contract.
///
/// Threading: created and driven entirely from the `venc` thread (SPEC.md §12).
/// Nothing here is thread-safe and nothing here blocks on disk.
class IVideoEncoder {
public:
    IVideoEncoder() = default;
    virtual ~IVideoEncoder() = default;

    IVideoEncoder(const IVideoEncoder&) = delete;
    IVideoEncoder& operator=(const IVideoEncoder&) = delete;
    IVideoEncoder(IVideoEncoder&&) = delete;
    IVideoEncoder& operator=(IVideoEncoder&&) = delete;

    /// Opens the encoder on `device`. The device must be the one that owns the
    /// textures passed to `submit` -- encode where the pixels already live
    /// (SPEC.md §5.2).
    [[nodiscard]] virtual Result<void> open(ID3D11Device* device, const VideoEncoderSettings& settings) = 0;

    /// Submits one NV12 frame with an already-quantized PTS in `1/60000` units
    /// (SPEC.md §7.2 -- the encoder does not do timing, it is handed a decision).
    ///
    /// `nv12` stays owned by the caller and may be recycled as soon as this
    /// returns.
    [[nodiscard]] virtual Result<void> submit(ID3D11Texture2D* nv12, std::int64_t pts) = 0;

    /// Codes the **next** submitted frame as an IDR (SPEC.md §7.5's resume).
    ///
    /// A `forced-idr` request, not a reopen: reopening would emit fresh SPS/PPS and the
    /// container's parameter sets were fixed by `avformat_write_header` (§10.1), which
    /// is the constraint §5.4's amendment established and BUG-028's neighbourhood.
    ///
    /// **Why an IDR rather than an I-frame, and why the distinction is not pedantic.**
    /// Pause excises time, so the content either side of a resume seam is unrelated. A
    /// P-frame after the seam that still references pre-pause pictures produces a
    /// visible smear -- §7.5 says so by name. Only an IDR empties the reference buffer;
    /// a plain I-frame does not, and every encoder in this build produces exactly that
    /// unless `forced-idr` was set at open. Measured in the FFmpeg n8.1.2 sources this
    /// build links: `nvenc.c` picks `NV_ENC_PIC_FLAG_FORCEINTRA` over
    /// `NV_ENC_PIC_FLAG_FORCEIDR` when `forced_idr` is 0, `amfenc.c` picks
    /// `AMF_VIDEO_ENCODER_PICTURE_TYPE_I` over `..._IDR`, and `libx264.c` picks
    /// `X264_TYPE_KEYFRAME` over `X264_TYPE_IDR`. So the option is set at open on all
    /// three paths, and this call is the per-frame half of the same requirement.
    ///
    /// Idempotent, and cleared once a frame has been submitted. Venc thread only.
    virtual void request_keyframe() noexcept = 0;

    /// Signals end of stream. After this, drain with `receive` until it yields
    /// `nullopt`; delayed frames (B-frames) only come out at this point.
    [[nodiscard]] virtual Result<void> flush() = 0;

    /// Next available packet, or `nullopt` when the encoder needs more input --
    /// or, after `flush`, when the stream is fully drained.
    [[nodiscard]] virtual Result<std::optional<EncodedPacket>> receive() = 0;

    /// The opened codec context, for the muxer to copy stream parameters from.
    /// Null before `open` succeeds.
    [[nodiscard]] virtual const AVCodecContext* codec_context() const noexcept = 0;

    /// Name of the libavcodec encoder actually in use.
    [[nodiscard]] virtual std::string_view encoder_name() const noexcept = 0;

    /// False when this is the software fallback (SPEC.md §13 rung 5). Drives the
    /// health monitor's rung-5 reporting, and it is read from the encoder rather
    /// than inferred from the name at the call site so there is one place that
    /// decides.
    [[nodiscard]] virtual bool is_hardware() const noexcept = 0;

    /// Applies SPEC.md §13 rung 2's quality reduction mid-recording.
    ///
    /// Returns false when the encoder cannot change quality without being reopened,
    /// **which in this build is every hardware encoder**. Measured against the
    /// FFmpeg n8.1.2 sources we link: `h264_nvenc` reconfigures only
    /// `averageBitRate`/`maxBitRate`/`vbvBufferSize` and only when rate control is
    /// not CONSTQP -- and SPEC.md §9 makes CQP the default -- while `h264_amf` sets
    /// every property once at init and never revisits it. `libx264` does honour a
    /// runtime `qp` change. See BUG-024 in docs/ENGINEERING_LOG.md.
    ///
    /// The seam exists rather than the ladder silently skipping rung 2, because the
    /// difference between "the ladder acted" and "the ladder wanted to act and could
    /// not" is the difference between a quality drop and an unexplained frame drop.
    [[nodiscard]] virtual bool try_set_cqp(int cqp) = 0;

    /// Frames submitted and packets produced. `submitted - received` at end of
    /// stream must be zero, which is what proves nothing was silently swallowed.
    [[nodiscard]] virtual std::uint64_t submitted() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t received() const noexcept = 0;
};

/// Builds an H.264 encoder. Fails with `ENCODE_CODEC_UNSUPPORTED` for anything
/// other than `VideoCodec::H264` in v1.
[[nodiscard]] Result<std::unique_ptr<IVideoEncoder>> create_video_encoder(const VideoEncoderSettings& settings);

} // namespace fc::encode
