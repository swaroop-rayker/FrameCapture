#pragma once

// WASAPI loopback capture (SPEC.md §8.1, §8.3).
//
// Captures what the system is playing, from the default `eRender` / `eConsole`
// endpoint. No microphone, ever -- CLAUDE.md §2 rule 6 makes input capture a hard
// non-goal, and this class only ever opens a *render* endpoint in loopback mode.

#include "core/audio/audio_timeline.h"
#include "core/error/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fc::audio {

/// The endpoint's negotiated format, as WASAPI reports it.
struct MixFormat {
    int sample_rate = 48000;
    int channels = 2;
    /// Bits per sample of the *device* format, before conversion to canonical.
    int bits_per_sample = 32;
    /// True when the endpoint hands over IEEE float rather than PCM integer.
    bool is_float = true;
    /// `dwChannelMask` from `WAVEFORMATEXTENSIBLE`, or 0 when the endpoint used a
    /// plain `WAVEFORMATEX`. Needed to map to an `AVChannelLayout` without
    /// guessing (SPEC.md §8.5).
    std::uint32_t channel_mask = 0;

    [[nodiscard]] int bytes_per_frame() const noexcept {
        return channels * (bits_per_sample / 8);
    }
};

/// One buffer as WASAPI handed it over, with the timeline metadata already
/// extracted.
struct LoopbackBuffer {
    /// Interleaved device-format samples. Empty when `flags.silent` -- the caller
    /// fills zeros rather than reading undefined memory (SPEC.md §8.2).
    const std::uint8_t* data = nullptr;
    std::int64_t frames = 0;
    PacketFlags flags;

    /// Bytes per frame in `data`, from the format this capture stream negotiated.
    ///
    /// **Travels with the buffer rather than being looked up.** A consumer that read
    /// the size off shared state would be reading a value another thread rewrites on
    /// an endpoint migration (SPEC.md §14.1), and the two disagree in exactly the
    /// window where `data` is still the old endpoint's -- which reads past the end of
    /// it. Carried here, the size is a property of the bytes, not of whatever the
    /// pipeline currently believes about its input. See BUG-031.
    int bytes_per_frame = 0;

    /// `u64QPCPosition`, converted to nanoseconds. Authoritative per SPEC.md §8.3.
    std::int64_t qpc_ns = 0;

    /// `IAudioClock2::GetDevicePosition`, for the 1 Hz drift cross-check.
    std::int64_t device_position_frames = 0;
};

/// One `eRender` endpoint, as the system reports it.
struct RenderEndpoint {
    /// The WASAPI endpoint id. This is what `LoopbackCapture::start` and
    /// `AudioPathSettings::device_id` take.
    std::string id;
    /// `PKEY_Device_FriendlyName`, for the GUI and the log.
    std::string name;
    /// True for the current `eRender`/`eConsole` default -- the endpoint an empty
    /// `device_id` selects, and the one SPEC.md §14.1's `OnDefaultDeviceChanged`
    /// fires about.
    bool is_default = false;
    /// The endpoint's *current* mix format. Read without opening a capture stream,
    /// so enumerating is cheap and does not disturb anything already recording.
    ///
    /// Present because SPEC.md §14.1 turns on whether the new endpoint's format
    /// differs from the old one: "if the new format differs (rate/channels), keep
    /// the encoder's output format constant and adapt via `libswresample`". Knowing
    /// that before opening is what lets a migration decide whether it needs a new
    /// resampler at all.
    MixFormat format;
};

/// Every active `eRender` endpoint on the system.
///
/// Render only, never `eCapture`: CLAUDE.md §2 rule 6 makes microphone capture a
/// hard non-goal, and an enumeration that returned input devices would be the first
/// step toward accidentally offering one.
///
/// Needed by SPEC.md §15.1's `get_devices` command and by §14.1's migration, which
/// has to pick the endpoint it is moving to.
[[nodiscard]] Result<std::vector<RenderEndpoint>> enumerate_render_endpoints();

/// Called on the audio thread for every captured buffer.
///
/// **Must not block.** SPEC.md §12 marks the audio thread as never-blocking, and
/// it runs at MMCSS `Pro Audio` / `AVRT_PRIORITY_CRITICAL` -- stalling it starves
/// the endpoint and produces exactly the discontinuities the timeline then has to
/// paper over.
using LoopbackSink = std::function<void(const LoopbackBuffer&)>;

/// Event-driven WASAPI loopback on a dedicated thread.
///
/// SPEC.md §8.1 requires event-driven mode (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`)
/// with a 20 ms buffer, because polling is a drift generator: the poll interval
/// beats against the device period and the error accumulates.
///
/// Note that event mode does **not** rescue the silence problem. The event fires
/// when the endpoint has data; when nothing is playing there is no data and no
/// event, which is precisely why `AudioTimeline`'s watchdog exists (SPEC.md §8.2).
class LoopbackCapture {
public:
    LoopbackCapture();
    ~LoopbackCapture();

    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;
    LoopbackCapture(LoopbackCapture&&) = delete;
    LoopbackCapture& operator=(LoopbackCapture&&) = delete;

    /// Opens the endpoint and starts the capture thread.
    ///
    /// `device_id` empty selects the default render endpoint. `sink` is invoked
    /// on the audio thread for every buffer.
    [[nodiscard]] Result<void> start(const std::string& device_id, LoopbackSink sink);

    /// Stops the thread and releases the endpoint. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept;

    /// The negotiated endpoint format. Only meaningful after a successful
    /// `start`.
    [[nodiscard]] MixFormat format() const noexcept;

    /// Buffers delivered, and buffers WASAPI flagged.
    [[nodiscard]] std::uint64_t buffers_captured() const noexcept;
    [[nodiscard]] std::uint64_t silent_buffers() const noexcept;
    [[nodiscard]] std::uint64_t discontinuities() const noexcept;

    /// Human-readable endpoint name, for the session preamble (SPEC.md §18).
    [[nodiscard]] std::string device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::audio
