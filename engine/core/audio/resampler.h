#pragma once

// Conversion to the engine's canonical audio format (SPEC.md §8.1, §8.5).
//
// Endpoints report whatever they like -- 44.1 or 96 kHz, 16-bit PCM or 32-bit
// float, stereo through 7.1. Everything downstream works in one format instead:
// **48 kHz, planar float, in a channel layout pinned for the file's lifetime.**
//
// The pinning is the part that matters and is not obvious. If the endpoint's mix
// format changes mid-recording -- a 7.1 headset unplugged, leaving stereo
// speakers -- the *stream* cannot follow it: changing an AAC stream's channel
// count mid-file is invalid in both MP4 and MKV, and produces a file that players
// either reject or decode as noise from the switch onwards (SPEC.md §8.5, §14.1).
// So the layout is fixed when recording starts and libswresample up- or
// down-mixes whatever arrives into it.

#include "core/audio/loopback_capture.h"
#include "core/config/config_schema.h"
#include "core/error/result.h"
#include "core/ffmpeg/av_raii.h"

#include <cstdint>
#include <vector>

namespace fc::audio {

/// The canonical format everything downstream of capture speaks.
inline constexpr int kCanonicalSampleRate = 48000;
inline constexpr AVSampleFormat kCanonicalSampleFormat = AV_SAMPLE_FMT_FLTP;

/// Largest silent stretch `Resampler::convert_silence` produces in one call --
/// 100 ms at 48 kHz. The zero buffer behind it is allocated once at `initialize`,
/// so the silence path allocates nothing per call no matter how long the gap.
inline constexpr std::int64_t kSilenceChunkFrames = 4800;

/// Resolves the layout to pin for this recording (SPEC.md §8.5).
///
/// `Auto` follows the endpoint. An explicit setting overrides it **downward only**:
/// it may ask for fewer channels than the endpoint has, and it may not ask for more.
///
/// ---------------------------------------------------------------------------
/// Why the override is one-directional (BUG-048)
/// ---------------------------------------------------------------------------
/// Down is the case the override exists for: it is how a user records stereo from a
/// 7.1 endpoint without touching Windows' settings, and `libswresample` down-mixes
/// into it losing nothing a stereo listener could hear.
///
/// Up has no such argument, and it is actively harmful. Choosing 7.1 on the stereo
/// endpoint most machines have produced an eight-channel AAC track carrying two
/// channels' worth of information at 512 kbps instead of 192 — and **unplayable in
/// Windows' own player**, whose Media Foundation AAC decoder accepts 1, 2 and 6
/// channels and refuses 8. The user's video played and their audio did not.
/// Up-mixing cannot add information; all it can do is cost interoperability and
/// bitrate.
///
/// So a request wider than the endpoint falls back to the endpoint's own layout and
/// is logged at WARN with both counts. Nothing fails: SPEC.md §8.5's pin still holds
/// for the file's lifetime, it is simply pinned to a layout that exists.
///
/// **The clamp is decided once, at `open`.** §14.1 forbids changing an AAC stream's
/// channel count mid-file, so an endpoint that later migrates to a wider one does not
/// widen the recording — which is why this is not re-evaluated on migration.
[[nodiscard]] Result<AVChannelLayout> resolve_channel_layout(config::ChannelLayoutSetting setting,
                                                             const MixFormat& endpoint);

/// Default AAC bitrate for a layout, per SPEC.md §8.5: 192 kbps stereo,
/// 384 for 5.1, 512 for 7.1.
[[nodiscard]] int default_bitrate_kbps(int channels) noexcept;

/// Converts endpoint buffers to canonical format.
///
/// Not thread-safe; lives on the `audio` thread with the capture sink.
class Resampler {
public:
    Resampler();
    ~Resampler();

    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&) = delete;
    Resampler& operator=(Resampler&&) = delete;

    /// Configures conversion from `input` to the canonical format in
    /// `pinned_layout`.
    [[nodiscard]] Result<void> initialize(const MixFormat& input, const AVChannelLayout& pinned_layout);

    /// Reconfigures for a new endpoint format, keeping the pinned layout.
    ///
    /// Called when the device changes mid-recording (SPEC.md §14.1). The output
    /// side is deliberately untouched -- that is the whole point of pinning.
    [[nodiscard]] Result<void> reconfigure_input(const MixFormat& input);

    /// Converts one buffer of interleaved endpoint-format samples.
    ///
    /// `input` must not be null: `swr_convert` treats a null input as *flush*, not
    /// as silence, and passing one to produce a silent stretch yields whatever
    /// happened to be buffered instead of the requested duration (BUG-012). Use
    /// `convert_silence` for silence.
    ///
    /// Returns a frame owned by the resampler and valid until the next call.
    [[nodiscard]] Result<const AVFrame*> convert(const std::uint8_t* input, std::int64_t frames);

    /// Produces exactly `frames` endpoint-rate frames of silence, converted to
    /// canonical format.
    ///
    /// Silence goes through libswresample rather than around it. Bypassing it
    /// would leave the filter delay and the rate conversion's fractional
    /// accumulator untouched across the gap, so the samples after a silent stretch
    /// would land a fraction of a sample away from where the ones before it did --
    /// which is drift introduced by the very mechanism that exists to close a gap.
    ///
    /// `frames` must not exceed `kSilenceChunkFrames`; longer stretches are the
    /// caller's to chunk, because the zero buffer is preallocated and a silent
    /// desktop can otherwise ask for minutes of it in one call.
    [[nodiscard]] Result<const AVFrame*> convert_silence(std::int64_t frames);

    /// Drains samples libswresample is holding, with no new input. Only meaningful
    /// at end of stream: a rate conversion has a filter delay, and the tail is
    /// audio that would otherwise be truncated.
    [[nodiscard]] Result<const AVFrame*> flush();

    /// Applies SPEC.md §8.4's soft resync. `samples` is added to (or removed
    /// from) the output over `distance` output samples.
    [[nodiscard]] Result<void> set_compensation(int samples, int distance);

    /// Output frames buffered inside libswresample. Non-zero when the input and
    /// output rates differ, since resampling has a filter delay.
    [[nodiscard]] std::int64_t queued_output_frames() const noexcept;

    [[nodiscard]] const AVChannelLayout& output_layout() const noexcept {
        return output_layout_;
    }

    [[nodiscard]] int output_channels() const noexcept {
        return output_layout_.nb_channels;
    }

    [[nodiscard]] bool initialized() const noexcept {
        return static_cast<bool>(context_);
    }

private:
    [[nodiscard]] Result<void> build(const MixFormat& input);
    [[nodiscard]] Result<void> ensure_output_capacity(int frames);
    [[nodiscard]] Result<const AVFrame*> run(const std::uint8_t* input, std::int64_t frames);

    ff::SwrContextRef context_;
    ff::Frame output_;
    AVChannelLayout output_layout_{};
    AVChannelLayout input_layout_{};
    AVSampleFormat input_format_ = AV_SAMPLE_FMT_FLT;
    int input_rate_ = 48000;
    int output_capacity_ = 0;

    /// Preallocated zeros in the *input* format, so the silence path costs a
    /// memcpy-free `swr_convert` and no allocation (SPEC.md §12: no allocation on
    /// the hot path). Resized by `build`, since the endpoint format sets its width.
    std::vector<std::uint8_t> silence_;
};

} // namespace fc::audio
