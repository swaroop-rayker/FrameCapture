#pragma once

// Demuxing and decoding a finished recording, for the tests that have to prove
// something about *both* streams (SPEC.md §20 rows 4 and 17).
//
// SPEC.md §20 says "ffprobe the output". There is no ffprobe binary in this build
// -- the engine links a `--disable-everything` libavformat with an explicit
// whitelist -- so this demuxes and decodes in-process through the same
// libavformat the engine writes with. That is strictly better than shelling out:
// hermetic, no PATH lookup, and it asserts against the structures rather than
// against scraped text.
//
// Timestamps come back as **seconds in the file's own timebase**, taken from the
// decoded frames rather than from a sample counter. That is the whole point for
// row 4: an AAC stream carries an encoder delay, the container declares it as
// `CodecDelay`, and the decoder applies it. Counting samples from zero would
// silently reintroduce the ~21 ms the container exists to remove, and the test
// would be measuring its own arithmetic.

#include "core/error/result.h"
#include "core/ffmpeg/av_raii.h"

#include "synthetic_source.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fc::test {

struct DecodedVideo {
    bool present = false;
    std::string codec_name;
    int width = 0;
    int height = 0;
    std::int64_t frame_count = 0;

    /// Presentation time of each decoded frame, in seconds.
    std::vector<double> times;

    /// Presentation time of each decoded frame in the **stream's own timebase**,
    /// unconverted, alongside that timebase.
    ///
    /// SPEC.md §20 row 7 asks for "all PTS deltas identical", and that is a claim
    /// about integers that cannot survive a trip through `double` seconds: at 60 fps
    /// a frame is 16.666… ms, so comparing converted values means comparing rounding
    /// error. It also cannot be asserted without knowing the timebase, because
    /// whether a uniform grid *can* have identical deltas depends on it —
    /// matroskaenc forces 1/1000 on every stream, on which 1/60 s is not
    /// representable at all. See `test_cfr_exactness.cpp`.
    std::vector<std::int64_t> pts;
    AVRational time_base{0, 1};
    /// Mean luma below the barcode row, per frame. A flash frame sits near white.
    std::vector<double> mean_luma;
    /// The frame-index barcode, per frame. Empty optional where it did not decode
    /// cleanly, which for a flash frame it still should -- the flash deliberately
    /// leaves the barcode row alone.
    std::vector<std::optional<std::uint32_t>> barcodes;
};

struct DecodedAudioTrack {
    bool present = false;
    std::string codec_name;
    int sample_rate = 0;
    int channels = 0;
    /// Layout as the *container* reports it, before any decoding. Signalling site
    /// 3 of SPEC.md §8.5.
    AVChannelLayout container_layout{};
    /// Layout the decoder derived, which on AAC comes from the
    /// `AudioSpecificConfig` in `CodecPrivate` -- signalling site 2.
    AVChannelLayout decoded_layout{};
    /// True when the container carried an `AudioSpecificConfig` at all.
    bool has_extradata = false;
    /// Encoder delay the container declared, in samples.
    std::int64_t initial_padding = 0;

    /// Presentation time of the first decoded sample, in seconds.
    double first_time = 0.0;
    /// Deinterleaved samples, one vector per channel.
    std::vector<std::vector<float>> planes;

    /// The Matroska `Name` tag, or empty when the track carries none.
    ///
    /// Read back out of the file rather than assumed, because SPEC.md §8.6 calls an
    /// untagged track "a UX failure" and a name that was set on the wrong object,
    /// after `avformat_write_header`, or on a stream the muxer then reordered is
    /// exactly as absent as one that was never set.
    std::string name;

    /// Stream index in the file. Present so a per-track assertion can say which
    /// track failed rather than which position in a vector did.
    int stream_index = -1;

    ~DecodedAudioTrack() {
        av_channel_layout_uninit(&container_layout);
        av_channel_layout_uninit(&decoded_layout);
    }

    DecodedAudioTrack() = default;
    DecodedAudioTrack(const DecodedAudioTrack&) = delete;
    DecodedAudioTrack& operator=(const DecodedAudioTrack&) = delete;
    DecodedAudioTrack(DecodedAudioTrack&&) = delete;
    DecodedAudioTrack& operator=(DecodedAudioTrack&&) = delete;
};

struct DecodeOptions {
    bool video = true;
    bool audio = true;
    /// Skip the per-frame luma work when only timing is wanted. Decoding 60
    /// seconds of 1080p and averaging every frame is minutes of CPU otherwise.
    bool video_luma = true;
};

struct DecodedMedia {
    bool opened = false;
    std::string format_name;
    int stream_count = 0;
    double duration_seconds = 0.0;
    std::string detail;

    DecodedVideo video;

    /// Track 0 — the system mix, which SPEC.md §8.6 makes the first audio stream of
    /// every recording. Every test written before Tier B existed reads this and is
    /// unaffected by there being more.
    DecodedAudioTrack audio;

    /// SPEC.md §8.6's per-application tracks, in file order, **excluding** track 0.
    ///
    /// `unique_ptr` because `DecodedAudioTrack` owns `AVChannelLayout`s and is
    /// deliberately neither copyable nor movable.
    std::vector<std::unique_ptr<DecodedAudioTrack>> extra_audio;

    /// Audio streams the file carries in total, track 0 included.
    [[nodiscard]] std::size_t audio_track_count() const noexcept {
        return (audio.present ? 1U : 0U) + extra_audio.size();
    }

    /// Track `index` (0 = the system mix), or null when the file has no such track.
    [[nodiscard]] const DecodedAudioTrack* audio_track(std::size_t index) const noexcept {
        if (index == 0) {
            return audio.present ? &audio : nullptr;
        }
        return index - 1 < extra_audio.size() ? extra_audio[index - 1].get() : nullptr;
    }
};

/// Decodes `path`. Never throws; `opened` and `detail` carry the outcome.
void decode_media(const std::filesystem::path& path, const DecodeOptions& options, DecodedMedia& out);

/// Beep onsets from a decoded audio plane, as **seconds from the start of the
/// file**, using the track's own first-sample time as the origin.
[[nodiscard]] std::vector<double> onset_times(const DecodedAudioTrack& track, int channel, int envelope_frames,
                                              double threshold, double quiet_seconds);

/// Times of the frames whose mean luma crosses `kFlashLumaThreshold`, in seconds,
/// keeping only the first frame of each run -- a flash lasting one frame can still
/// decode as two if the encoder held it.
[[nodiscard]] std::vector<double> flash_times(const DecodedVideo& video);

/// Energy at `frequency` in `samples`, by the Goertzel algorithm. Used for
/// SPEC.md §20 row 17's per-channel tone identification: each channel carries a
/// distinct frequency, so the loudest bin identifies which channel came back.
[[nodiscard]] double tone_energy(const std::vector<float>& samples, double frequency, int sample_rate);

} // namespace fc::test
