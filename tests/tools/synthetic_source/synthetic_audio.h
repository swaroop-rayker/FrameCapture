#pragma once

// The 1 kHz tone half of the deterministic test source (SPEC.md §20.1, §20 row 4).
//
// SPEC.md §20 row 4 asks for "1 kHz beep on exact frame boundaries + white flash
// frame; decode and assert |offset| < 20 ms at every 60 s mark". That test only
// means anything if both events are generated from **one** clock, so this
// generator is defined over absolute QPC nanoseconds rather than over its own
// sample counter: a beep starts when wall time crosses a multiple of
// `beep_period_ns` from `beep_epoch_ns`, whatever the audio device's own origin
// is and whatever it did with the buffer before.
//
// That is also what makes the epoch negotiation of SPEC.md §7.1 observable. If
// the audio device opens 40 ms before the first video frame, a generator anchored
// to the *device* would put its beeps 40 ms away from the flashes and the test
// would measure its own harness. Anchored to the shared epoch, a beep and a flash
// are the same instant by construction, and any offset the file comes back with
// is the pipeline's.
//
// No Windows headers, no libav, no D3D. The CPU tier uses it exactly as the GPU
// tier does.

#include <cstdint>
#include <span>
#include <vector>

namespace fc::test {

struct ToneSettings {
    int sample_rate = 48000;
    int channels = 2;

    /// SPEC.md §20.1's 1 kHz tone.
    double frequency = 1000.0;
    double amplitude = 0.5;

    /// The instant beep 0 starts. Everything is measured from here.
    std::int64_t beep_epoch_ns = 0;

    /// One beep per second, which is an exact frame boundary at 60 and at 30 fps.
    std::int64_t beep_period_ns = 1'000'000'000;

    /// How long each beep sounds. Long enough that AAC's 1024-sample MDCT window
    /// cannot smear the whole burst into the noise floor, short enough that
    /// consecutive beeps stay clearly separated.
    std::int64_t beep_length_ns = 60'000'000;
};

/// Frames per beep period, and per beep.
[[nodiscard]] std::int64_t frames_per_period(const ToneSettings& settings) noexcept;
[[nodiscard]] std::int64_t frames_per_beep(const ToneSettings& settings) noexcept;

/// The absolute frame index, counted from `beep_epoch_ns`, that `qpc_ns` falls on.
/// Negative before the epoch, which is legitimate -- an audio device can open
/// first (SPEC.md §7.1).
[[nodiscard]] std::int64_t frame_index_at(const ToneSettings& settings, std::int64_t qpc_ns) noexcept;

/// Renders `frames` interleaved float samples beginning at absolute frame index
/// `start_index`. Every channel carries the same tone, so a decoded file can be
/// checked on any one of them.
///
/// Deterministic and stateless: the same index range always renders the same
/// samples, so a test can re-render a stretch to compare against what came back.
void render_tone(const ToneSettings& settings, std::int64_t start_index, std::int64_t frames, std::vector<float>& out);

/// Beep onsets, as frame indices into `mono`.
///
/// A rising edge is where a short-window envelope first crosses `threshold` after
/// having been below half of it for at least `quiet_frames`. The hysteresis is
/// what stops AAC's pre-echo -- the MDCT spreads a hard transient backwards over
/// most of a window -- from registering as an onset of its own a few milliseconds
/// early.
[[nodiscard]] std::vector<std::int64_t> detect_onsets(std::span<const float> mono, int envelope_frames,
                                                      double threshold, std::int64_t quiet_frames);

} // namespace fc::test
