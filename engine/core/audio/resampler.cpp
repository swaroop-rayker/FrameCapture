#include "core/audio/resampler.h"

#include "core/logging/logger.h"

#include <algorithm>

namespace fc::audio {
namespace {

/// Maps a WASAPI `dwChannelMask` to the equivalent FFmpeg layout.
///
/// The two use the same bit values for the standard speaker positions -- both
/// derive from the WAVE_FORMAT_EXTENSIBLE speaker constants -- so the mask can be
/// handed over directly rather than translated position by position. A mask of 0
/// means the endpoint used a plain WAVEFORMATEX and told us nothing, so the
/// channel count is all there is to go on.
/// AAC's standard channel configurations put 5.1's surround pair at the **back**
/// (`AV_CH_LAYOUT_5POINT1_BACK`, configuration 6). FFmpeg's `AV_CH_LAYOUT_5POINT1`
/// puts it at the sides, which is not a standard configuration -- the encoder falls
/// back to a Program Config Element, and a PCE-coded stream reopens with its
/// channel order *unspecified*: six channels and no idea which is which.
///
/// That is SPEC.md §20 row 17's defect exactly ("5.1 plays as stereo / channels
/// swapped"), so a side-channel 5.1 is normalised to the back-channel form the
/// codec can actually signal. Windows reports both -- `KSAUDIO_SPEAKER_5POINT1` is
/// the back form and `KSAUDIO_SPEAKER_5POINT1_SURROUND` the side one -- so this is
/// reachable from an ordinary endpoint, not a contrived one.
///
/// The relabelling is the lesser cost. On a 5.1 system the surround pair is one
/// pair of speakers whichever name the file gives it, and the alternative is a
/// file that names nothing.
void normalize_for_aac(AVChannelLayout* layout) {
    if (layout->order == AV_CHANNEL_ORDER_NATIVE && layout->u.mask == AV_CH_LAYOUT_5POINT1) {
        av_channel_layout_uninit(layout);
        av_channel_layout_from_mask(layout, AV_CH_LAYOUT_5POINT1_BACK);
    }
}

/// Channels an explicit setting asks for, or 0 for `Auto`.
///
/// A small table rather than resolving the layout and measuring it, because the
/// comparison happens *before* the layout is built -- and because a table makes the
/// answer readable next to §8.5's ladder of default bitrates.
[[nodiscard]] int channels_for(config::ChannelLayoutSetting setting) noexcept {
    switch (setting) {
    case config::ChannelLayoutSetting::Auto:
        return 0; // follows the endpoint, so there is nothing to be wider than
    case config::ChannelLayoutSetting::Stereo:
        return 2;
    case config::ChannelLayoutSetting::Surround51:
        return 6;
    case config::ChannelLayoutSetting::Surround71:
        return 8;
    }
    return 0;
}

void layout_from_mask(AVChannelLayout* layout, std::uint32_t mask, int channels) {
    if (mask != 0) {
        av_channel_layout_from_mask(layout, mask);
        if (layout->nb_channels == channels) {
            normalize_for_aac(layout);
            return;
        }
        // The mask and the count disagree. Trusting the count is the safer of the
        // two: it is what the buffers actually contain, and a wrong layout with
        // the right count merely mislabels speakers, where a wrong count
        // misreads memory.
        av_channel_layout_uninit(layout);
    }
    av_channel_layout_default(layout, channels);
}

AVSampleFormat sample_format_of(const MixFormat& format) noexcept {
    if (format.is_float) {
        return format.bits_per_sample == 64 ? AV_SAMPLE_FMT_DBL : AV_SAMPLE_FMT_FLT;
    }
    switch (format.bits_per_sample) {
    case 16:
        return AV_SAMPLE_FMT_S16;
    case 32:
        return AV_SAMPLE_FMT_S32;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

} // namespace

Result<AVChannelLayout> resolve_channel_layout(config::ChannelLayoutSetting setting, const MixFormat& endpoint) {
    AVChannelLayout layout{};

    // SPEC.md §8.5's override is **downward only** (BUG-048). An explicit layout wider
    // than the endpoint would up-mix, which cannot add information and costs both
    // bitrate and interoperability -- Windows' own AAC decoder refuses eight channels
    // outright, so a 7.1 pin on a stereo endpoint produced a file whose audio would not
    // play. See `resolve_channel_layout`'s header note.
    //
    // Decided here rather than in the GUI because the engine is what knows the
    // *negotiated* endpoint format: a settings dialog knows what the user picked and
    // what the default device reported at the time, and the device can change between
    // the two. The GUI warns; this is what enforces.
    const int requested = channels_for(setting);
    if (requested > 0 && endpoint.channels > 0 && requested > endpoint.channels) {
        FC_LOG_WARN(Subsystem::Audio,
                    "the requested channel layout is wider than the endpoint; recording the endpoint's layout instead",
                    LogFields{}
                        .add("requested_channels", requested)
                        .add("endpoint_channels", endpoint.channels)
                        .add("setting", std::string{config::to_string(setting)})
                        .add("hint", "up-mixing adds no information and Windows' AAC decoder refuses 8 channels"));
        setting = config::ChannelLayoutSetting::Auto;
    }

    switch (setting) {
    case config::ChannelLayoutSetting::Auto:
        layout_from_mask(&layout, endpoint.channel_mask, endpoint.channels);
        break;
    case config::ChannelLayoutSetting::Stereo:
        av_channel_layout_default(&layout, 2);
        break;
    case config::ChannelLayoutSetting::Surround51:
        // `_BACK`, which is what SPEC.md §8.5 now names. See `normalize_for_aac`:
        // the side-channel form is not one of AAC's standard configurations and
        // reopens with unspecified channel positions.
        av_channel_layout_from_mask(&layout, AV_CH_LAYOUT_5POINT1_BACK);
        break;
    case config::ChannelLayoutSetting::Surround71:
        av_channel_layout_from_mask(&layout, AV_CH_LAYOUT_7POINT1);
        break;
    }

    if (layout.nb_channels <= 0 || layout.nb_channels > 8) {
        av_channel_layout_uninit(&layout);
        FC_LOG_ERROR(
            Subsystem::Audio, "unsupported channel layout",
            LogFields{}.add("channels", endpoint.channels).add_error(FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED));
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }
    return layout;
}

int default_bitrate_kbps(int channels) noexcept {
    // SPEC.md §8.5.
    if (channels >= 8) {
        return 512;
    }
    if (channels >= 6) {
        return 384;
    }
    return 192;
}

Resampler::Resampler() = default;

Resampler::~Resampler() {
    av_channel_layout_uninit(&output_layout_);
    av_channel_layout_uninit(&input_layout_);
}

Result<void> Resampler::build(const MixFormat& input) {
    const AVSampleFormat input_format = sample_format_of(input);
    if (input_format == AV_SAMPLE_FMT_NONE) {
        FC_LOG_ERROR(Subsystem::Audio, "endpoint sample format is not supported",
                     LogFields{}
                         .add("bits", input.bits_per_sample)
                         .add("float", input.is_float)
                         .add_error(FcError::AUDIO_MIX_FORMAT_UNSUPPORTED));
        return FcError::AUDIO_MIX_FORMAT_UNSUPPORTED;
    }

    av_channel_layout_uninit(&input_layout_);
    layout_from_mask(&input_layout_, input.channel_mask, input.channels);

    input_format_ = input_format;
    input_rate_ = input.sample_rate;

    // Interleaved: every format the endpoint can hand over is packed, so one plane
    // holds `frames * channels * bytes_per_sample`.
    const auto bytes_per_frame =
        static_cast<std::size_t>(av_get_bytes_per_sample(input_format_)) * static_cast<std::size_t>(input.channels);
    silence_.assign(static_cast<std::size_t>(kSilenceChunkFrames) * bytes_per_frame, std::uint8_t{0});

    if (const int err = context_.alloc(&output_layout_, kCanonicalSampleFormat, kCanonicalSampleRate, &input_layout_,
                                       input_format_, input_rate_);
        err < 0) {
        FC_LOG_ERROR(Subsystem::Audio, "swr_alloc_set_opts2 failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::AUDIO_RESAMPLER_INIT_FAILED));
        return FcError::AUDIO_RESAMPLER_INIT_FAILED;
    }

    if (const int err = swr_init(context_.get()); err < 0) {
        FC_LOG_ERROR(Subsystem::Audio, "swr_init failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::AUDIO_RESAMPLER_INIT_FAILED));
        return FcError::AUDIO_RESAMPLER_INIT_FAILED;
    }

    FC_LOG_INFO(Subsystem::Audio, "resampler configured",
                LogFields{}
                    .add("in_rate", input_rate_)
                    .add("in_channels", input_layout_.nb_channels)
                    .add("in_format", av_get_sample_fmt_name(input_format_))
                    .add("out_rate", kCanonicalSampleRate)
                    .add("out_channels", output_layout_.nb_channels)
                    .add("out_format", av_get_sample_fmt_name(kCanonicalSampleFormat)));
    return ok();
}

Result<void> Resampler::initialize(const MixFormat& input, const AVChannelLayout& pinned_layout) {
    av_channel_layout_uninit(&output_layout_);
    if (const int err = av_channel_layout_copy(&output_layout_, &pinned_layout); err < 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }

    if (!output_.alloc()) {
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }
    output_capacity_ = 0;

    return build(input);
}

Result<void> Resampler::reconfigure_input(const MixFormat& input) {
    if (output_layout_.nb_channels == 0) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    FC_LOG_WARN(Subsystem::Audio, "endpoint format changed mid-recording; remixing into the pinned layout",
                LogFields{}
                    .add("in_rate", input.sample_rate)
                    .add("in_channels", input.channels)
                    .add("pinned_channels", output_layout_.nb_channels));

    // Only the input side is rebuilt. The output layout is pinned for the file's
    // lifetime because an AAC stream cannot change channel count mid-file.
    return build(input);
}

Result<void> Resampler::ensure_output_capacity(int frames) {
    if (frames <= output_capacity_ && output_->data[0] != nullptr) {
        return ok();
    }

    output_.unref();
    output_->format = kCanonicalSampleFormat;
    output_->sample_rate = kCanonicalSampleRate;
    output_->nb_samples = frames;
    if (const int err = av_channel_layout_copy(&output_->ch_layout, &output_layout_); err < 0) {
        return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
    }

    if (const int err = av_frame_get_buffer(output_.get(), 0); err < 0) {
        FC_LOG_ERROR(Subsystem::Audio, "could not allocate the resampler output frame",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::INTERNAL_OUT_OF_MEMORY));
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }
    output_capacity_ = frames;
    return ok();
}

Result<const AVFrame*> Resampler::run(const std::uint8_t* input, std::int64_t frames) {
    if (!context_) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (frames < 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    // Output can exceed input when upsampling, and libswresample may also flush
    // samples it had buffered from the previous call.
    const std::int64_t delay = swr_get_delay(context_.get(), input_rate_);
    const auto capacity =
        static_cast<int>(av_rescale_rnd(delay + frames, kCanonicalSampleRate, input_rate_, AV_ROUND_UP));
    FC_TRY(ensure_output_capacity(std::max(capacity, 1)));

    // Not const*const[]: swr_convert takes const uint8_t**, and a const array
    // of pointers will not convert to it.
    // NOLINTNEXTLINE(misc-const-correctness)
    const std::uint8_t* planes[1] = {input};
    const int produced = swr_convert(context_.get(), output_->data, output_capacity_,
                                     input != nullptr ? planes : nullptr, static_cast<int>(frames));
    if (produced < 0) {
        FC_LOG_ERROR(
            Subsystem::Audio, "swr_convert failed",
            LogFields{}.add("error", ff::error_text(produced)).add_error(FcError::AUDIO_RESAMPLER_INIT_FAILED));
        return FcError::AUDIO_RESAMPLER_INIT_FAILED;
    }

    output_->nb_samples = produced;
    return static_cast<const AVFrame*>(output_.get());
}

Result<const AVFrame*> Resampler::convert(const std::uint8_t* input, std::int64_t frames) {
    if (input == nullptr) {
        // Reached only by a caller that has confused "silent buffer" with "no
        // buffer". Refused rather than forwarded, because `swr_convert` would
        // read it as a flush and quietly return a short frame -- which shortens
        // the audio track by the length of the stretch and is SPEC.md §20 row 4
        // arriving through the code that exists to prevent it (BUG-012).
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    return run(input, frames);
}

Result<const AVFrame*> Resampler::convert_silence(std::int64_t frames) {
    if (frames < 0 || frames > kSilenceChunkFrames) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (silence_.empty()) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    return run(silence_.data(), frames);
}

Result<const AVFrame*> Resampler::flush() {
    return run(nullptr, 0);
}

Result<void> Resampler::set_compensation(int samples, int distance) {
    if (!context_) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (distance <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    if (const int err = swr_set_compensation(context_.get(), samples, distance); err < 0) {
        FC_LOG_WARN(Subsystem::Audio, "swr_set_compensation was refused; drift will be left to accumulate",
                    LogFields{}.add("error", ff::error_text(err)).add("samples", samples).add("distance", distance));
        return FcError::AUDIO_DRIFT_UNRECOVERABLE;
    }
    return ok();
}

std::int64_t Resampler::queued_output_frames() const noexcept {
    return context_ ? swr_get_delay(context_.get(), kCanonicalSampleRate) : 0;
}

} // namespace fc::audio
