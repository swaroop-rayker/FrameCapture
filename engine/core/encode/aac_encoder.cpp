#include "core/encode/aac_encoder.h"

#include "core/audio/resampler.h"
#include "core/logging/logger.h"

extern "C" {
#include <libavutil/opt.h>
}

#include <algorithm>

namespace fc::encode {

struct AacEncoder::Impl {
    ff::CodecContext codec;
    ff::Frame staging;  ///< accumulates input until a full AAC frame is available
    ff::Packet scratch; ///< reused across receive(), so the hot path allocates nothing

    AVChannelLayout layout{};
    int frame_size = 1024;
    std::int64_t staged = 0;   ///< samples currently held in `staging`
    std::int64_t next_pts = 0; ///< PTS, in samples, of the frame being accumulated
    std::uint64_t submitted = 0;
    std::uint64_t received = 0;
    bool flushed = false;

    ~Impl() {
        av_channel_layout_uninit(&layout);
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Result<void> send_staged();
};

/// Hands the accumulated frame to the encoder and resets the buffer.
Result<void> AacEncoder::Impl::send_staged() {
    if (staged == 0) {
        return ok();
    }

    staging->nb_samples = static_cast<int>(staged);
    staging->pts = next_pts;

    const int err = avcodec_send_frame(codec.get(), staging.get());
    if (err == AVERROR(EAGAIN)) {
        // The encoder is holding packets nobody has collected. Backpressure, not
        // failure -- the caller drains and submits again. Reported distinctly for
        // the same reason the video pool's exhaustion is (BUG-007): a "wait"
        // misreported as a "fail" becomes discarded audio, and audio drops are
        // audible (SPEC.md §12).
        return FcError::INTERNAL_QUEUE_FULL;
    }
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_send_frame failed on the audio encoder",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_SUBMIT_FAILED));
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    next_pts += staged;
    staged = 0;
    ++submitted;
    return ok();
}

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}

AacEncoder::~AacEncoder() = default;

Result<void> AacEncoder::open(const AudioEncoderSettings& settings) {
    if (settings.layout.nb_channels <= 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        FC_LOG_ERROR(Subsystem::Encode, "the AAC encoder is not present in this libavcodec build",
                     LogFields{}.add_error(FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
    }

    if (!impl_->codec.alloc(codec)) {
        return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
    }

    if (const int err = av_channel_layout_copy(&impl_->layout, &settings.layout); err < 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }

    // Signalling site 1 of 3: what the encoder is told to produce.
    if (const int err = av_channel_layout_copy(&impl_->codec->ch_layout, &impl_->layout); err < 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }

    impl_->codec->sample_fmt = audio::kCanonicalSampleFormat;
    impl_->codec->sample_rate = settings.sample_rate;
    impl_->codec->time_base = AVRational{1, settings.sample_rate};

    const int bitrate =
        settings.bitrate_kbps > 0 ? settings.bitrate_kbps : audio::default_bitrate_kbps(impl_->layout.nb_channels);
    impl_->codec->bit_rate = static_cast<std::int64_t>(bitrate) * 1000;
    impl_->codec->profile = AV_PROFILE_AAC_LOW; // AAC-LC, per SPEC.md §8.5

    // Signalling site 2 of 3: without this the AudioSpecificConfig never becomes
    // extradata, and a decoder falls back to whatever the container claims.
    if (settings.global_header) {
        impl_->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (const int err = avcodec_open2(impl_->codec.get(), codec, nullptr); err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_open2 failed on the AAC encoder",
                     LogFields{}
                         .add("error", ff::error_text(err))
                         .add("channels", impl_->layout.nb_channels)
                         .add_error(FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
    }

    if (impl_->codec->extradata == nullptr || impl_->codec->extradata_size == 0) {
        FC_LOG_ERROR(Subsystem::Encode, "the AAC encoder produced no AudioSpecificConfig",
                     LogFields{}
                         .add("hint", "open with AV_CODEC_FLAG_GLOBAL_HEADER")
                         .add_error(FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED));
        return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
    }

    impl_->frame_size = impl_->codec->frame_size > 0 ? impl_->codec->frame_size : 1024;

    if (!impl_->staging.alloc() || !impl_->scratch.alloc()) {
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }
    impl_->staging->format = audio::kCanonicalSampleFormat;
    impl_->staging->sample_rate = settings.sample_rate;
    impl_->staging->nb_samples = impl_->frame_size;
    if (av_channel_layout_copy(&impl_->staging->ch_layout, &impl_->layout) < 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }
    if (const int err = av_frame_get_buffer(impl_->staging.get(), 0); err < 0) {
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }

    FC_LOG_INFO(Subsystem::Encode, "AAC encoder opened",
                LogFields{}
                    .add("sample_rate", settings.sample_rate)
                    .add("channels", impl_->layout.nb_channels)
                    .add("bitrate_kbps", bitrate)
                    .add("frame_size", impl_->frame_size)
                    .add("extradata_bytes", impl_->codec->extradata_size));
    return ok();
}

Result<int> AacEncoder::submit(const AVFrame* frame, int offset) {
    if (!impl_->codec) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (frame == nullptr || frame->nb_samples <= 0) {
        return 0;
    }
    if (offset < 0 || offset > frame->nb_samples) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    const int channels = impl_->layout.nb_channels;
    const std::int64_t available = static_cast<std::int64_t>(frame->nb_samples) - offset;
    std::int64_t consumed = 0;

    // AAC wants exactly `frame_size` samples at a time; the resampler produces
    // whatever the endpoint's buffer period yields. Accumulate rather than
    // pushing the alignment problem onto every caller.
    while (consumed < available) {
        const std::int64_t room = impl_->frame_size - impl_->staged;
        const std::int64_t take = std::min(room, available - consumed);

        for (int channel = 0; channel < channels; ++channel) {
            const auto* source = reinterpret_cast<const float*>(frame->data[channel]);
            auto* destination = reinterpret_cast<float*>(impl_->staging->data[channel]);
            std::copy_n(source + offset + consumed, take, destination + impl_->staged);
        }

        impl_->staged += take;
        consumed += take;

        if (impl_->staged == impl_->frame_size) {
            const Result<void> sent = impl_->send_staged();
            if (!sent.has_value()) {
                if (sent.error() == FcError::INTERNAL_QUEUE_FULL) {
                    // Backpressure. The samples copied so far are safely staged;
                    // reporting them as consumed is what lets the caller resume
                    // after draining instead of resending them (BUG-013).
                    return static_cast<int>(consumed);
                }
                return sent.error();
            }
        }
    }
    return static_cast<int>(consumed);
}

Result<void> AacEncoder::flush() {
    if (!impl_->codec) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (impl_->flushed) {
        return ok();
    }

    // A partial frame at end of stream is still audio. Dropping it truncates the
    // track by up to 21 ms, which is inside SPEC.md §20 row 4's tolerance on its
    // own but accumulates with everything else.
    //
    // The flushed flag is set only after this succeeds. Setting it first would
    // make the idempotence guard swallow the retry after a QUEUE_FULL and discard
    // the very samples this call exists to preserve.
    FC_TRY(impl_->send_staged());

    const int err = avcodec_send_frame(impl_->codec.get(), nullptr);
    if (err == AVERROR(EAGAIN)) {
        // The end-of-stream marker is input like any other, and libavcodec refuses
        // input while packets are waiting to be collected. Backpressure again, not
        // failure -- and `flushed` stays false so the retry after draining is the
        // one that actually ends the stream.
        return FcError::INTERNAL_QUEUE_FULL;
    }
    if (err < 0 && err != AVERROR_EOF) {
        return FcError::ENCODE_SUBMIT_FAILED;
    }

    impl_->flushed = true;
    return ok();
}

Result<std::optional<EncodedPacket>> AacEncoder::receive() {
    if (!impl_->codec) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->scratch.unref();
    const int err = avcodec_receive_packet(impl_->codec.get(), impl_->scratch.get());
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
        return std::optional<EncodedPacket>{};
    }
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Encode, "avcodec_receive_packet failed on the audio encoder",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::ENCODE_RECEIVE_FAILED));
        return FcError::ENCODE_RECEIVE_FAILED;
    }

    EncodedPacket out;
    if (!out.packet.alloc()) {
        return FcError::ENCODE_RECEIVE_FAILED;
    }
    av_packet_move_ref(out.packet.get(), impl_->scratch.get());
    // Every AAC frame is independently decodable, so every packet is a sync
    // point. The muxer uses this for cue placement.
    out.keyframe = true;
    out.audio = true;

    ++impl_->received;
    return std::optional<EncodedPacket>{std::move(out)};
}

const AVCodecContext* AacEncoder::codec_context() const noexcept {
    return impl_->codec.get();
}

int AacEncoder::frame_size() const noexcept {
    return impl_->frame_size;
}

std::uint64_t AacEncoder::frames_submitted() const noexcept {
    return impl_->submitted;
}

std::uint64_t AacEncoder::packets_received() const noexcept {
    return impl_->received;
}

} // namespace fc::encode
