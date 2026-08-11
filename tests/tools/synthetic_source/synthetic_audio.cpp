#include "synthetic_audio.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fc::test {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Frames at `rate` in `ns`, in integer arithmetic. Splitting off whole seconds
/// keeps the intermediate inside 64 bits for a multi-hour run, the same reason
/// `AudioTimeline` and the frame pacer do it.
[[nodiscard]] std::int64_t ns_to_frames(std::int64_t ns, int rate) noexcept {
    const bool negative = ns < 0;
    const std::int64_t magnitude = negative ? -ns : ns;
    const std::int64_t seconds = magnitude / kNsPerSecond;
    const std::int64_t remainder = magnitude % kNsPerSecond;
    const std::int64_t frames = (seconds * rate) + (((remainder * rate) + (kNsPerSecond / 2)) / kNsPerSecond);
    return negative ? -frames : frames;
}

} // namespace

std::int64_t frames_per_period(const ToneSettings& settings) noexcept {
    return ns_to_frames(settings.beep_period_ns, settings.sample_rate);
}

std::int64_t frames_per_beep(const ToneSettings& settings) noexcept {
    return ns_to_frames(settings.beep_length_ns, settings.sample_rate);
}

std::int64_t frame_index_at(const ToneSettings& settings, std::int64_t qpc_ns) noexcept {
    return ns_to_frames(qpc_ns - settings.beep_epoch_ns, settings.sample_rate);
}

void render_tone(const ToneSettings& settings, std::int64_t start_index, std::int64_t frames, std::vector<float>& out) {
    const auto samples = static_cast<std::size_t>(std::max<std::int64_t>(frames, 0)) *
                         static_cast<std::size_t>(std::max(settings.channels, 0));
    out.assign(samples, 0.0F);
    if (frames <= 0 || settings.channels <= 0 || settings.sample_rate <= 0) {
        return;
    }

    const std::int64_t period = std::max<std::int64_t>(frames_per_period(settings), 1);
    const std::int64_t length = std::max<std::int64_t>(frames_per_beep(settings), 1);
    const double step = 2.0 * std::numbers::pi * settings.frequency / settings.sample_rate;

    for (std::int64_t i = 0; i < frames; ++i) {
        const std::int64_t index = start_index + i;
        if (index < 0) {
            continue; // before beep 0; silence
        }

        // Phase is measured from the beep's own start, so every burst begins at
        // zero and the first zero crossing is the onset rather than an arbitrary
        // point in a free-running oscillator.
        const std::int64_t into_period = index % period;
        if (into_period >= length) {
            continue;
        }

        const auto value = static_cast<float>(settings.amplitude * std::sin(step * static_cast<double>(into_period)));
        for (int channel = 0; channel < settings.channels; ++channel) {
            out[(static_cast<std::size_t>(i) * static_cast<std::size_t>(settings.channels)) +
                static_cast<std::size_t>(channel)] = value;
        }
    }
}

std::vector<std::int64_t> detect_onsets(std::span<const float> mono, int envelope_frames, double threshold,
                                        std::int64_t quiet_frames) {
    std::vector<std::int64_t> onsets;
    if (mono.empty() || envelope_frames <= 0) {
        return onsets;
    }

    const auto window = static_cast<std::size_t>(envelope_frames);
    const double release = threshold * 0.5;

    // Rectified moving sum, so the envelope costs one add and one subtract per
    // sample rather than a window scan. Over a 4-hour decode that is the
    // difference between a test and a coffee break.
    double running = 0.0;
    bool armed = true;
    std::int64_t quiet_run = 0;

    for (std::size_t i = 0; i < mono.size(); ++i) {
        running += std::abs(static_cast<double>(mono[i]));
        if (i >= window) {
            running -= std::abs(static_cast<double>(mono[i - window]));
        }
        const double envelope = running / static_cast<double>(std::min<std::size_t>(i + 1, window));

        if (armed && envelope >= threshold) {
            // The envelope trails the signal by half a window, because a sample
            // only enters the sum once it has arrived. Correcting for it here
            // keeps the reported onset at the burst's start rather than half a
            // window late, which at a 5 ms window is a quarter of the tolerance
            // SPEC.md §20 row 4 allows.
            const auto corrected = static_cast<std::int64_t>(i) - (envelope_frames / 2);
            onsets.push_back(std::max<std::int64_t>(corrected, 0));
            armed = false;
            quiet_run = 0;
        } else if (!armed) {
            if (envelope < release) {
                ++quiet_run;
                if (quiet_run >= quiet_frames) {
                    armed = true;
                }
            } else {
                quiet_run = 0;
            }
        }
    }
    return onsets;
}

} // namespace fc::test
