#include "decoded_media.h"

#include "synthetic_audio.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

namespace fc::test {
namespace {

int find_stream(AVFormatContext* format, AVMediaType type) {
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (format->streams[i]->codecpar->codec_type == type) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void collect_video_frame(const AVFrame* frame, const AVRational& timebase, DecodedVideo& video,
                         std::vector<std::uint8_t>& plane) {
    ++video.frame_count;
    video.times.push_back(static_cast<double>(frame->pts) * av_q2d(timebase));
    video.pts.push_back(frame->pts);
    video.time_base = timebase;

    if (video.width <= 0 || video.height <= 0) {
        return;
    }

    // The luma plane is padded to `linesize[0]`; the helpers want it tight. The
    // scratch buffer is the caller's and is reused: a fresh megabyte per frame is
    // 100 GB of allocation over a half-hour recording, for a buffer whose size
    // never changes.
    plane.resize(static_cast<std::size_t>(frame->width) * static_cast<std::size_t>(frame->height));
    for (int y = 0; y < frame->height; ++y) {
        const std::uint8_t* row = frame->data[0] + (static_cast<std::ptrdiff_t>(y) * frame->linesize[0]);
        std::copy(row, row + frame->width, plane.begin() + (static_cast<std::ptrdiff_t>(y) * frame->width));
    }

    const std::span<const std::uint8_t> view{plane};
    video.mean_luma.push_back(mean_luma_below_barcode(view, frame->width, frame->height));
    video.barcodes.push_back(decode_barcode_from_luma(view, frame->width, frame->height));
}

void collect_audio_frame(const AVFrame* frame, const AVRational& timebase, DecodedAudioTrack& audio) {
    if (audio.planes.empty()) {
        audio.planes.resize(static_cast<std::size_t>(frame->ch_layout.nb_channels));
        // The decoder has already applied the container's `CodecDelay`, so this is
        // where the first *audible* sample sits in the file's own timeline.
        audio.first_time = static_cast<double>(frame->pts) * av_q2d(timebase);
        if (frame->pts == AV_NOPTS_VALUE) {
            audio.first_time = 0.0;
        }
    }

    const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)) != 0;
    const int channels = frame->ch_layout.nb_channels;
    for (int channel = 0; channel < channels && std::cmp_less(channel, audio.planes.size()); ++channel) {
        const auto* samples = reinterpret_cast<const float*>(planar ? frame->data[channel] : frame->data[0]);
        const int stride = planar ? 1 : channels;
        const int offset = planar ? 0 : channel;
        auto& plane = audio.planes[static_cast<std::size_t>(channel)];

        // No `reserve` here, deliberately. Reserving exactly `size() + nb_samples`
        // on every 1024-sample AAC frame asks for the precise capacity needed right
        // now, which defeats the geometric growth `push_back` would otherwise use:
        // every frame reallocates and copies the whole accumulated vector, making
        // the decode quadratic in the recording's length. It cost 70 minutes on a
        // 30-minute file and a few invisible seconds on a 65-second one.
        for (int i = 0; i < frame->nb_samples; ++i) {
            plane.push_back(samples[static_cast<std::size_t>((i * stride) + offset)]);
        }
    }
}

} // namespace

void decode_media(const std::filesystem::path& path, const DecodeOptions& options, DecodedMedia& out) {
    ff::InputFormatContext input;
    const std::string filename = path.string();
    if (input.open(filename.c_str()) < 0) {
        out.detail = "avformat_open_input failed";
        return;
    }
    AVFormatContext* format = input.get();

    if (avformat_find_stream_info(format, nullptr) < 0) {
        out.detail = "avformat_find_stream_info failed";
        return;
    }

    out.format_name = format->iformat->name != nullptr ? format->iformat->name : "";
    out.stream_count = static_cast<int>(format->nb_streams);
    out.duration_seconds =
        format->duration != AV_NOPTS_VALUE ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;

    const int video_index = options.video ? find_stream(format, AVMEDIA_TYPE_VIDEO) : -1;

    // Every audio stream, in file order. SPEC.md §8.6 puts the system mix first, so
    // index 0 is `out.audio` and the rest are the per-application tracks.
    std::vector<int> audio_indices;
    if (options.audio) {
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_indices.push_back(static_cast<int>(i));
            }
        }
    }
    ff::CodecContext video_ctx;
    if (video_index >= 0) {
        const AVCodecParameters* par = format->streams[video_index]->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
        if (decoder == nullptr || !video_ctx.alloc(decoder) ||
            avcodec_parameters_to_context(video_ctx.get(), par) < 0 ||
            avcodec_open2(video_ctx.get(), decoder, nullptr) < 0) {
            out.detail = "video decoder setup failed";
            return;
        }
        out.video.present = true;
        out.video.codec_name = avcodec_get_name(par->codec_id);
        out.video.width = options.video_luma ? par->width : 0;
        out.video.height = options.video_luma ? par->height : 0;
    }

    // One decoder per audio stream, and one `DecodedAudioTrack` behind it. Track 0
    // stays in `out.audio` so every test written before Tier B keeps working
    // unchanged.
    std::vector<ff::CodecContext> audio_contexts;
    std::vector<DecodedAudioTrack*> audio_targets;
    audio_contexts.reserve(audio_indices.size());
    audio_targets.reserve(audio_indices.size());
    for (std::size_t slot = 0; slot < audio_indices.size(); ++slot) {
        const int index = audio_indices[slot];
        const AVCodecParameters* par = format->streams[index]->codecpar;
        ff::CodecContext context;
        const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
        if (decoder == nullptr || !context.alloc(decoder) || avcodec_parameters_to_context(context.get(), par) < 0 ||
            avcodec_open2(context.get(), decoder, nullptr) < 0) {
            out.detail = "audio decoder setup failed for stream " + std::to_string(index);
            return;
        }

        DecodedAudioTrack* track = nullptr;
        if (slot == 0) {
            track = &out.audio;
        } else {
            out.extra_audio.push_back(std::make_unique<DecodedAudioTrack>());
            track = out.extra_audio.back().get();
        }

        track->present = true;
        track->stream_index = index;
        track->codec_name = avcodec_get_name(par->codec_id);
        track->sample_rate = par->sample_rate;
        track->channels = par->ch_layout.nb_channels;
        track->has_extradata = par->extradata != nullptr && par->extradata_size > 0;
        track->initial_padding = par->initial_padding;
        if (const AVDictionaryEntry* title = av_dict_get(format->streams[index]->metadata, "title", nullptr, 0);
            title != nullptr && title->value != nullptr) {
            track->name = title->value;
        }
        static_cast<void>(av_channel_layout_copy(&track->container_layout, &par->ch_layout));
        static_cast<void>(av_channel_layout_copy(&track->decoded_layout, &context->ch_layout));

        audio_contexts.push_back(std::move(context));
        audio_targets.push_back(track);
    }

    ff::Packet packet;
    ff::Frame frame;
    if (!packet.alloc() || !frame.alloc()) {
        out.detail = "allocation failed";
        return;
    }

    std::vector<std::uint8_t> luma_scratch;

    const auto harvest_video = [&] {
        while (avcodec_receive_frame(video_ctx.get(), frame.get()) >= 0) {
            collect_video_frame(frame.get(), format->streams[video_index]->time_base, out.video, luma_scratch);
            frame.unref();
        }
    };
    const auto harvest_audio = [&](std::size_t slot) {
        while (avcodec_receive_frame(audio_contexts[slot].get(), frame.get()) >= 0) {
            collect_audio_frame(frame.get(), format->streams[audio_indices[slot]]->time_base, *audio_targets[slot]);
            frame.unref();
        }
    };
    /// Which decoder a packet belongs to, or `npos`. Linear over at most six entries
    /// (SPEC.md §8.6's cap).
    const auto audio_slot = [&](int index) -> std::size_t {
        for (std::size_t slot = 0; slot < audio_indices.size(); ++slot) {
            if (audio_indices[slot] == index) {
                return slot;
            }
        }
        return audio_indices.size();
    };

    while (av_read_frame(format, packet.get()) >= 0) {
        if (packet->stream_index == video_index && video_ctx) {
            if (avcodec_send_packet(video_ctx.get(), packet.get()) >= 0) {
                harvest_video();
            }
        } else if (const std::size_t slot = audio_slot(packet->stream_index); slot < audio_indices.size()) {
            if (avcodec_send_packet(audio_contexts[slot].get(), packet.get()) >= 0) {
                harvest_audio(slot);
            }
        }
        packet.unref();
    }

    // Drain every reorder buffer, or the last frames go uncounted.
    if (video_ctx && avcodec_send_packet(video_ctx.get(), nullptr) >= 0) {
        harvest_video();
    }
    for (std::size_t slot = 0; slot < audio_contexts.size(); ++slot) {
        if (avcodec_send_packet(audio_contexts[slot].get(), nullptr) >= 0) {
            harvest_audio(slot);
        }
    }

    // The decoder derives the layout from the AudioSpecificConfig on the first
    // frame, so it is only final once something has actually decoded.
    for (std::size_t slot = 0; slot < audio_contexts.size(); ++slot) {
        av_channel_layout_uninit(&audio_targets[slot]->decoded_layout);
        static_cast<void>(
            av_channel_layout_copy(&audio_targets[slot]->decoded_layout, &audio_contexts[slot]->ch_layout));
    }

    out.opened = true;
    out.detail = "ok";
}

std::vector<double> onset_times(const DecodedAudioTrack& track, int channel, int envelope_frames, double threshold,
                                double quiet_seconds) {
    std::vector<double> times;
    if (!track.present || std::cmp_greater_equal(channel, track.planes.size()) || track.sample_rate <= 0) {
        return times;
    }

    const auto quiet_frames = static_cast<std::int64_t>(quiet_seconds * track.sample_rate);
    const std::vector<std::int64_t> onsets =
        detect_onsets(track.planes[static_cast<std::size_t>(channel)], envelope_frames, threshold, quiet_frames);

    times.reserve(onsets.size());
    for (const std::int64_t onset : onsets) {
        times.push_back(track.first_time + (static_cast<double>(onset) / track.sample_rate));
    }
    return times;
}

std::vector<double> flash_times(const DecodedVideo& video) {
    std::vector<double> times;
    bool in_flash = false;
    for (std::size_t i = 0; i < video.mean_luma.size() && i < video.times.size(); ++i) {
        const bool bright = video.mean_luma[i] >= kFlashLumaThreshold;
        if (bright && !in_flash) {
            times.push_back(video.times[i]);
        }
        in_flash = bright;
    }
    return times;
}

double tone_energy(const std::vector<float>& samples, double frequency, int sample_rate) {
    if (samples.empty() || sample_rate <= 0) {
        return 0.0;
    }
    // Goertzel: one bin of a DFT for two multiplies per sample, which matters when
    // eight channels of a multi-second recording are each probed at eight
    // frequencies.
    const double omega = 2.0 * std::numbers::pi * frequency / sample_rate;
    const double coefficient = 2.0 * std::cos(omega);
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float sample : samples) {
        const double s0 = sample + (coefficient * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = (s1 * s1) + (s2 * s2) - (coefficient * s1 * s2);
    return std::sqrt(std::max(power, 0.0)) / static_cast<double>(samples.size());
}

} // namespace fc::test
