#pragma once

// The audio half of a recording: WASAPI -> timeline -> drift -> resample -> AAC
// (SPEC.md §8, §12).
//
// ---------------------------------------------------------------------------
// Why this is two classes
// ---------------------------------------------------------------------------
// `AudioEncodePath` is everything *above* the WASAPI seam. It takes buffers that
// look like WASAPI buffers and produces encoded AAC packets, and it touches no
// device, no COM apartment and no D3D. That is deliberate: CLAUDE.md §6 says the
// hardware tier cannot be verified here, so the seam is placed exactly where
// verification stops. Timeline placement, silence filling, drift correction and
// the sample-accurate PTS that SPEC.md §20 row 4 turns on are all on the testable
// side of it, driven by `test_audio_path.cpp` on the CPU tier with a synthetic
// device clock.
//
// `AudioPath` is the thin part below the seam: a `LoopbackCapture` whose sink
// feeds `AudioEncodePath::offer`, plus the `silence` watchdog thread. It can only
// be exercised against a real endpoint.
//
// ---------------------------------------------------------------------------
// Thread ownership (SPEC.md §12)
// ---------------------------------------------------------------------------
//   `audio`    LoopbackCapture's own thread, MMCSS "Pro Audio". Copies the
//              endpoint buffer into a pooled slot and enqueues it. Nothing else --
//              no timeline arithmetic, no libswresample, no encoder.
//   `silence`  Watches the last offered timestamp and enqueues a tick when the
//              endpoint has been quiet for more than two buffer periods
//              (SPEC.md §8.2). Timed wait, so blocking is allowed.
//   `aenc`     Drains the queue and owns `AudioTimeline`, `DriftCompensator`,
//              `Resampler` and `AacEncoder` outright. Single-threaded ownership is
//              what makes those four components -- none of which is thread-safe --
//              correct without a lock on the audio thread's path.
//
// The queue between `audio` and `aenc` is bounded with the **Block** policy
// (SPEC.md §12: "audio: never drop"). Blocking on the audio thread is otherwise
// forbidden, and the tension is real: it is resolved in SPEC's own direction,
// because a dropped audio buffer is an audible discontinuity and a permanent
// desync, while a full queue means `aenc` is wedged and the recording has a
// larger problem than a few milliseconds of priority inversion. Every wait is
// counted in `AudioStats::queue_waits` so the pressure is visible rather than
// inferred.

#include "core/audio/audio_timeline.h"
#include "core/audio/loopback_capture.h"
#include "core/config/config_schema.h"
#include "core/encode/aac_encoder.h"
#include "core/encode/video_encoder.h"
#include "core/error/result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fc::pipeline {

/// Where an encoded audio packet goes. Invoked on the `aenc` thread; in the
/// pipeline it pushes onto the shared mux queue.
using AudioPacketSink = std::function<void(encode::EncodedPacket)>;

struct AudioEncodeSettings {
    /// The endpoint's negotiated format. The timeline and the resampler's input
    /// side both work in *this* rate -- packet frame counts arrive in it, and
    /// converting them to 48 kHz per packet would round once per buffer and
    /// accumulate.
    audio::MixFormat input;

    /// Layout to pin for the file's lifetime (SPEC.md §8.5).
    config::ChannelLayoutSetting channel_layout = config::ChannelLayoutSetting::Auto;

    /// 0 selects SPEC.md §8.5's ladder: 192 stereo / 384 5.1 / 512 7.1.
    int bitrate_kbps = 0;

    /// SPEC.md §8.1's 20 ms buffer. Sets the silence watchdog's threshold, which
    /// is two of these.
    std::int64_t buffer_period_ns = 20'000'000;

    /// What to call this path's `aenc` thread (SPEC.md §12: every thread is named).
    ///
    /// Configurable because Tier B runs up to six of these at once (§8.6), and six
    /// threads all called `fc-aenc` is a minidump you cannot read -- which is the
    /// exact reason CLAUDE.md §4 requires the naming in the first place. Tier A
    /// keeps the name it has always had, so nothing that greps a log changes.
    std::string thread_label = "fc-aenc";
};

struct AudioStats {
    std::uint64_t buffers_offered = 0;

    /// Buffers lost *waiting* for the shared epoch, rather than refused by it.
    ///
    /// Ordinarily zero. Buffers that arrive before `t0` is published are held and
    /// replayed once it lands, and the ones that genuinely predate it are then
    /// refused by the timeline and counted in `timeline_drops` (BUG-016). This
    /// counts only the two cases where holding was not enough: the stash
    /// overflowed because video took more than a second to produce its first
    /// frame, or the recording stopped before an epoch was ever resolved.
    std::uint64_t buffers_before_epoch = 0;
    std::uint64_t silence_ticks = 0;

    /// Timeline length in endpoint-rate frames, silence included. This is what
    /// makes the audio track exactly as long as the video.
    std::int64_t frames_written = 0;
    std::int64_t silence_frames_injected = 0;
    std::int64_t discontinuities = 0;
    /// Packets whose span lay entirely behind the write head (SPEC.md §8.2).
    std::int64_t timeline_drops = 0;

    /// Endpoint migrations performed (SPEC.md §14.1). Each one starts a fresh timeline
    /// chained onto the previous, so `frames_written` covers the *current* endpoint's
    /// stretch only -- `timeline_seconds` is the figure that spans them all.
    std::uint64_t input_migrations = 0;
    /// Total length of every timeline this recording has had, in seconds.
    ///
    /// Accumulated in seconds rather than frames for the reason BUG-025 established for
    /// the video pacer: once the unit can change mid-recording, a frame count divided by
    /// one rate is wrong and there is no single rate to divide by.
    double timeline_seconds = 0.0;

    /// Silence across *every* timeline, in seconds.
    ///
    /// `silence_frames_injected` above counts only the current endpoint's stretch,
    /// because it comes straight off the live `AudioTimeline` and a migration retires
    /// that object. The silence that covered an endpoint gap is therefore banked in the
    /// timeline being retired and vanishes from the frame counter the moment the new one
    /// starts -- measured as 0 frames on a migration that had just filled 150 ms.
    /// Anything asking "was the gap filled?" wants this one.
    double silence_seconds = 0.0;

    std::uint64_t packets_encoded = 0;
    std::uint64_t encode_failures = 0;
    /// Times the `audio` thread had to wait for queue space. Non-zero means `aenc`
    /// fell behind; audio is still never dropped.
    std::uint64_t queue_waits = 0;

    std::int64_t last_drift_ns = 0;
    std::int64_t worst_drift_ns = 0;
    std::uint64_t soft_resyncs = 0;
    /// Non-zero here is a bug report, per SPEC.md §8.4.
    std::uint64_t hard_resyncs = 0;

    /// SPEC.md §8.3's cross-check: how far `IAudioClock2::GetDevicePosition` has
    /// diverged from QPC since the first buffer, in nanoseconds. **Positive means
    /// the endpoint's clock is running fast.**
    ///
    /// This is a *different* quantity from `last_drift_ns`, and the difference is
    /// the point. `last_drift_ns` measures the encoded track against wall clock --
    /// the thing that would desync playback, and the thing SPEC.md §8.2's timeline
    /// holds at zero. This measures the endpoint's crystal against QPC, which
    /// nothing corrects and which grows for the length of the recording. A healthy
    /// machine shows a few tens of microseconds per minute here and zero there;
    /// both being non-zero means the timeline stopped absorbing.
    ///
    /// Zero when the endpoint offered no `IAudioClock2`, which is logged once at
    /// start-up rather than inferred from a zero here.
    std::int64_t device_clock_delta_ns = 0;
    /// Frames the endpoint's own clock reports having rendered since the first
    /// buffer. Zero when the cross-check is unavailable.
    std::int64_t device_position_frames = 0;

    /// QPC of the first buffer offered, or 0 if none has been. What the session
    /// epoch negotiates against (SPEC.md §7.1).
    std::int64_t first_packet_qpc_ns = 0;
};

/// Everything above the WASAPI seam. See the header comment.
class AudioEncodePath {
public:
    AudioEncodePath();
    ~AudioEncodePath();

    AudioEncodePath(const AudioEncodePath&) = delete;
    AudioEncodePath& operator=(const AudioEncodePath&) = delete;
    AudioEncodePath(AudioEncodePath&&) = delete;
    AudioEncodePath& operator=(AudioEncodePath&&) = delete;

    /// Pins the layout, opens the resampler and the AAC encoder, and starts the
    /// `aenc` thread. Packets are produced only after `set_epoch`.
    [[nodiscard]] Result<void> open(const AudioEncodeSettings& settings, AudioPacketSink sink);

    /// The opened encoder, for `Muxer::open` to copy stream parameters from. Null
    /// before `open` succeeds.
    [[nodiscard]] const encode::AacEncoder* encoder() const noexcept;

    /// Publishes the shared epoch (SPEC.md §7.1). Everything offered before this
    /// is discarded: `t0` is the moment *both* streams are live, so a buffer that
    /// predates it has no place on the timeline. Later calls are ignored.
    void set_epoch(std::int64_t t0_ns) noexcept;

    /// Points this path's timeline at the recording's shared paused total (SPEC.md
    /// §7.5). The **same object** the video pacer is given -- that is the requirement.
    ///
    /// Must be called before `open`. Retained so that §14.1's migration, which builds a
    /// fresh `AudioTimeline` in the new endpoint's rate, can re-attach it: a migrated
    /// timeline with no clock would stop excising paused time from the audio half
    /// alone, which is §7.5's desync arriving through §14.1's door.
    void attach_pause_clock(const timing::PauseClock* clock) noexcept;

    [[nodiscard]] bool epoch_set() const noexcept;

    /// Hands over one endpoint buffer. Called from the `audio` thread; also the
    /// entry point the CPU-tier tests drive directly.
    void offer(const audio::LoopbackBuffer& buffer);

    /// SPEC.md §8.2's watchdog trigger: has the endpoint been quiet for more than
    /// two buffer periods? Reads one atomic, so the `silence` thread never touches
    /// the timeline the `aenc` thread owns.
    [[nodiscard]] bool silence_due_at(std::int64_t now_ns) const noexcept;

    /// Asks the `aenc` thread to bring the timeline up to `now_ns` with silence.
    /// The frame count is computed there, against the timeline, rather than here.
    void request_silence(std::int64_t now_ns);

    /// Switches to a new endpoint format mid-recording (SPEC.md §14.1).
    ///
    /// The encoder's **output** format never changes -- §14.1 is explicit that
    /// "changing an AAC stream's sample rate or channel count mid-file is invalid in
    /// both MP4 and MKV" -- so the AAC encoder, the pinned layout and the mux stream
    /// all survive untouched. What changes is the resampler's input side, and the
    /// timeline.
    ///
    /// **The timeline is chained, not rebased**, and that is the design decision this
    /// method exists to implement. `AudioTimeline` counts in the *endpoint's* frames and
    /// `accept` advances its head by the packet's own frame count, so a rate-independent
    /// unit would round once per buffer and accumulate -- the very thing the class's
    /// header warns against. Rebasing `frames_written_` at the seam would instead
    /// silently change what the accumulated count means: 480,000 frames is 10.000 s at
    /// 48 kHz and 10.884 s at 44.1 kHz, so the timeline would jump by nearly a second
    /// without a sample being written.
    ///
    /// So a fresh timeline starts at `at_qpc_ns`, in the new endpoint's rate, and the
    /// old one's contribution is already committed to the encoder. Each stays in its own
    /// units and no unit is ever mixed; the only conversion is one boundary offset per
    /// migration. It is also the shape Tier B (§8.6) needs, which is why `AudioTimeline`
    /// holds no global state in the first place.
    ///
    /// Silence covers `[last written .. at_qpc_ns)` so the timeline never shortens --
    /// §14.1's "fully silence-filled" and its < 200 ms gap target.
    ///
    /// Queued like a buffer rather than applied here, so it lands on the `aenc` thread
    /// in order with the buffers around it. Applying it from the caller's thread would
    /// race the four components `aenc` owns exclusively.
    void migrate_input(const audio::MixFormat& format, std::int64_t at_qpc_ns);

    /// Drains the queue, flushes the encoder into the sink and joins `aenc`.
    /// Idempotent.
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] AudioStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AudioPathSettings {
    /// Empty selects the default render endpoint.
    std::string device_id;
    config::ChannelLayoutSetting channel_layout = config::ChannelLayoutSetting::Auto;
    int bitrate_kbps = 0;
};

/// `LoopbackCapture` + the `silence` thread + an `AudioEncodePath`.
class AudioPath {
public:
    AudioPath();
    ~AudioPath();

    AudioPath(const AudioPath&) = delete;
    AudioPath& operator=(const AudioPath&) = delete;
    AudioPath(AudioPath&&) = delete;
    AudioPath& operator=(AudioPath&&) = delete;

    /// Opens the endpoint, pins the layout from its mix format, and starts the
    /// `audio`, `silence` and `aenc` threads.
    ///
    /// `on_first_packet` is invoked once, from the `audio` thread, with the QPC of
    /// the first buffer the endpoint produced -- the session's cue to resolve
    /// `t0` (SPEC.md §7.1). It must not block.
    [[nodiscard]] Result<void> start(const AudioPathSettings& settings, AudioPacketSink sink,
                                     std::function<void(std::int64_t)> on_first_packet);

    [[nodiscard]] const encode::AacEncoder* encoder() const noexcept;

    void set_epoch(std::int64_t t0_ns) noexcept;

    /// See `AudioEncodePath::attach_pause_clock`. Must be called before `start`.
    void attach_pause_clock(const timing::PauseClock* clock) noexcept;

    /// Moves the recording to a different render endpoint (SPEC.md §14.1, §20 row 12).
    ///
    /// Closes the current `LoopbackCapture`, opens `device_id`, and hands the new
    /// endpoint's negotiated format to `AudioEncodePath::migrate_input` -- which
    /// silence-fills the gap, chains a fresh timeline in the new rate, and leaves the
    /// AAC encoder's output format untouched. §14.1's target for the whole operation is
    /// **< 200 ms**, fully silence-filled so the timeline never shortens.
    ///
    /// Empty `device_id` selects the current default, which is what
    /// `OnDefaultDeviceChanged` calls for.
    ///
    /// **The endpoint-swap half of this is unverified on the reference rig**, which has
    /// exactly one render endpoint (measured -- see docs/ACCEPTANCE.md row 12). What is
    /// verified, on the CPU tier, is everything above the WASAPI seam: the timeline
    /// chaining, the silence fill and the constant output format. Testing the swap needs
    /// a second device.
    [[nodiscard]] Result<void> migrate_to(const std::string& device_id);

    /// Stops capture and the watchdog, then flushes the encode path. Idempotent.
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] AudioStats stats() const;

    /// The endpoint format actually negotiated, and its human-readable name, for
    /// the session preamble (SPEC.md §18).
    [[nodiscard]] audio::MixFormat format() const noexcept;
    [[nodiscard]] std::string device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::pipeline
