#pragma once

// The audio timeline and its silence generator (SPEC.md §8.2).
//
// **This is the #1 loopback sync bug, and it is a bug of omission.**
//
// WASAPI loopback emits no packets at all when no application is playing audio.
// Nothing fails, nothing errors -- the capture client simply has nothing to hand
// over. An implementation that writes packets as they arrive therefore records a
// 30-minute video containing 22 minutes of audio, and every sample after the
// first silent stretch is early by the length of that stretch. The desync is
// cumulative, so it looks fine for the first minute and unwatchable by the
// twentieth.
//
// The fix is to treat the timeline as the authority rather than the packet
// stream: audio occupies a continuous span from `t0`, and any interval no packet
// covers is filled with silence of exactly the missing duration.
//
// Shaped as a pure function over (timestamp, frame count) for the same reason the
// frame pacer is: SPEC.md §20.1 lists drift calculation as a unit-test target,
// and a component that reads its own clock cannot be tested for what it does
// across a two-second gap.

#include "core/error/result.h"
#include "core/timing/pause_clock.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace fc::audio {

/// Flags WASAPI reports alongside a captured buffer (SPEC.md §8.2). Mirrored here
/// rather than including <audioclient.h>, so this header stays testable and free
/// of Windows headers.
struct PacketFlags {
    /// `AUDCLNT_BUFFERFLAGS_SILENT`. The buffer's contents are undefined and must
    /// be treated as zeros -- **filled**, never skipped. Skipping shortens the
    /// timeline by the buffer's duration, which is the same defect as the gap this
    /// class exists to close, arriving by a different route.
    bool silent = false;

    /// `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`. The device dropped samples. The
    /// gap is bridged with silence and logged; concatenating the two sides is
    /// exactly how drift accumulates (SPEC.md §8.2).
    bool discontinuity = false;
};

/// One captured buffer, as WASAPI describes it.
struct CapturedPacket {
    /// `u64QPCPosition` from `IAudioCaptureClient::GetBuffer`, in nanoseconds.
    ///
    /// SPEC.md §8.3: this is authoritative. A running sample counter is not, since
    /// it assumes the device clock is exactly 48000.000 Hz and no device is.
    std::int64_t qpc_ns = 0;

    /// Frames (samples per channel) in the buffer.
    std::int64_t frames = 0;

    PacketFlags flags;
};

/// What the caller should write, in order, for one captured packet.
struct TimelineSegment {
    /// Frame index from `t0` at which this segment starts. Contiguous by
    /// construction: each segment begins exactly where the last one ended.
    std::int64_t start_frame = 0;

    /// Frames of silence to emit before the packet's own audio. Zero in the
    /// common case.
    std::int64_t silence_frames = 0;

    /// Frames of real audio to emit. Zero when the packet was dropped as a
    /// duplicate, or when it carried `silent` (its content becomes silence).
    ///
    /// **These are the packet's *last* `audio_frames` frames.** Both trims the
    /// timeline applies -- frames that predate `t0`, and frames that overlap
    /// ground already written -- come off the front, because the tail is the part
    /// that extends the timeline. A caller writes
    /// `packet.data + (packet.frames - audio_frames)`.
    std::int64_t audio_frames = 0;

    /// True when this packet's own content is silence -- either WASAPI flagged it
    /// or the generator manufactured it. Kept distinct from `silence_frames` so a
    /// caller can tell "the device sent me a silent buffer" from "nothing arrived
    /// and I invented one".
    bool content_is_silence = false;

    /// True when the packet arrived entirely inside ground already covered, and
    /// carries nothing new. Its audio is discarded rather than written past the
    /// end of the timeline, which would corrupt ordering.
    bool dropped = false;
};

/// Tracks a single audio stream's position on the shared timeline.
///
/// One instance per track. Tier A has exactly one; Tier B (M9.5) needs one per
/// track, because each carries its own device clock (SPEC.md §8.6) -- which is
/// why this holds no global state.
///
/// Not thread-safe. Driven from the `audio` thread, with the silence watchdog
/// asking it questions from the `silence` thread via `frames_missing_at`.
class AudioTimeline {
public:
    AudioTimeline() = default;

    AudioTimeline(int sample_rate, std::int64_t buffer_period_ns) noexcept
        : sample_rate_(sample_rate), buffer_period_ns_(buffer_period_ns) {}

    /// Establishes the epoch. SPEC.md §7.1: `t0` is shared with video and is the
    /// moment both first became available, so the session sets it -- never the
    /// audio path on its own.
    void start(std::int64_t t0_ns) noexcept;

    /// Places one captured packet on the timeline, inserting silence for whatever
    /// interval preceded it and nothing covered.
    [[nodiscard]] TimelineSegment accept(const CapturedPacket& packet);

    /// Frames of silence needed to bring the timeline up to `now_ns`.
    ///
    /// The silence watchdog (SPEC.md §8.2) polls this: when
    /// `now - last_packet_qpc > 2 * buffer_period` it injects exactly this many
    /// frames. Returns 0 while the stream is keeping up, so a healthy stream costs
    /// nothing but the comparison.
    [[nodiscard]] std::int64_t frames_missing_at(std::int64_t now_ns) const noexcept;

    /// True when the stream has been quiet longer than the watchdog tolerates.
    /// Two buffer periods, per SPEC.md §8.2 -- one is normal jitter.
    [[nodiscard]] bool needs_silence_at(std::int64_t now_ns) const noexcept;

    /// Emits `frames` of silence, advancing the timeline. Used by the watchdog
    /// when no packet has arrived; `accept` handles gaps it can see for itself.
    [[nodiscard]] TimelineSegment inject_silence(std::int64_t frames);

    /// Points the timeline at the recording's shared paused total (SPEC.md §7.5).
    ///
    /// The **same object** the video pacer is given. That is the whole requirement:
    /// §7.5's failure mode is two streams subtracting two different totals, which is
    /// silent, permanent, and invisible to every test that does not pause. Sharing one
    /// object makes the two totals the same by construction rather than by agreement.
    ///
    /// Null -- the default -- is a recording that cannot pause, and every mapping below
    /// then behaves exactly as it did before pause existed.
    ///
    /// Borrowed; owned by the session, which outlives this.
    void attach_pause_clock(const timing::PauseClock* clock) noexcept {
        pause_clock_ = clock;
    }

    /// Packets refused because they were captured inside a paused span.
    ///
    /// Separate from `dropped_packets()` for the same reason the pacer separates its
    /// excised frames from its drops: a drop is a fault worth investigating and an
    /// excision is the feature working.
    [[nodiscard]] std::int64_t excised_packets() const noexcept {
        return excised_;
    }

    /// Total frames placed on the timeline, silence included. This is what makes
    /// the audio track exactly as long as the video: `frames_written / rate` is
    /// the stream's duration, and it advances whether or not anything is playing.
    [[nodiscard]] std::int64_t frames_written() const noexcept {
        return frames_written_;
    }

    /// Frames of the above that were silence the generator manufactured. A
    /// recording of a silent desktop is legitimately almost all of this; a
    /// recording with continuous audio should be near zero, and anything in
    /// between is worth surfacing (SPEC.md §18).
    [[nodiscard]] std::int64_t silence_frames_injected() const noexcept {
        return silence_injected_;
    }

    /// How many *separate* times `accept` found a gap and filled it.
    ///
    /// `silence_frames_injected()` is a total, and a total cannot tell a silent desktop
    /// from a stuttering one: 261 ms of silence is one clean gap at the start of a
    /// recording, or sixty ten-millisecond holes punched through continuous audio, and
    /// only the second is audible. Counted separately because that distinction is the
    /// whole diagnosis when a user reports the sound is wrong.
    [[nodiscard]] std::int64_t gap_fills() const noexcept {
        return gap_fills_;
    }

    /// Packets whose timestamp landed less than one buffer period beyond the write head
    /// and were snapped to it rather than having silence spliced in front of them
    /// (BUG-042). Expected to be *large* on a real endpoint — it is the ordinary jitter
    /// of a QPC-stamped capture clock, and every one of these used to be a hole in the
    /// audio. `snapped_frames()` is what the endpoint's clock error would have been
    /// nulled by; SPEC.md §8.4's ladder absorbs it instead.
    [[nodiscard]] std::int64_t snaps() const noexcept {
        return snaps_;
    }

    [[nodiscard]] std::int64_t snapped_frames() const noexcept {
        return snapped_frames_;
    }

    [[nodiscard]] std::int64_t discontinuities() const noexcept {
        return discontinuities_;
    }

    [[nodiscard]] std::int64_t dropped_packets() const noexcept {
        return dropped_;
    }

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

    [[nodiscard]] int sample_rate() const noexcept {
        return sample_rate_;
    }

    /// QPC of the most recent packet, or `t0` if none has arrived.
    [[nodiscard]] std::int64_t last_packet_qpc_ns() const noexcept {
        return last_packet_qpc_ns_;
    }

private:
    /// `qpc_ns` with paused time removed (SPEC.md §7.5), or `nullopt` when it falls
    /// inside a paused span and therefore has no place on the timeline at all.
    ///
    /// Every timestamp entering this class goes through here, which is what makes "the
    /// pause is excised from audio too" a property of the type rather than a rule each
    /// method has to remember.
    [[nodiscard]] std::optional<std::int64_t> timeline_qpc_ns(std::int64_t qpc_ns) const noexcept;

    /// Frame index a timestamp maps to, relative to `t0`, **negative before it**.
    /// Integer arithmetic for the same reason the video pacer uses it: over a
    /// four-hour recording the nanosecond count exceeds what a double represents
    /// exactly.
    ///
    /// Takes an *already pause-adjusted* timestamp -- see `timeline_qpc_ns`. Passing a
    /// raw QPC reading here on a paused recording would place the packet by wall clock
    /// while the video pacer placed its frames by timeline, which is precisely the
    /// desync §7.5 warns about.
    [[nodiscard]] std::int64_t signed_frame_at(std::int64_t timeline_ns) const noexcept;

    /// The same, floored at zero. For questions about where the write head should
    /// be, which is never before the epoch.
    [[nodiscard]] std::int64_t frame_at(std::int64_t timeline_ns) const noexcept;

    int sample_rate_ = 48000;
    std::int64_t buffer_period_ns_ = 20'000'000; // 20 ms, per SPEC.md §8.1
    std::int64_t t0_ns_ = 0;
    bool started_ = false;

    std::int64_t frames_written_ = 0;
    std::int64_t silence_injected_ = 0;
    std::int64_t gap_fills_ = 0;
    std::int64_t snaps_ = 0;
    std::int64_t snapped_frames_ = 0;
    std::int64_t discontinuities_ = 0;
    std::int64_t dropped_ = 0;
    std::int64_t excised_ = 0;
    std::int64_t last_packet_qpc_ns_ = 0;

    /// The most recent packet's position in *timeline* time, recorded when it was
    /// accepted rather than re-derived later.
    ///
    /// The silence watchdog's "has the endpoint gone quiet" question has to be asked in
    /// timeline time, or a 10 s pause reads as 10 s of missing audio the instant the
    /// recording resumes. Re-mapping `last_packet_qpc_ns_` through the pause clock
    /// would answer it correctly too -- and would count the packet as a straggler every
    /// time the watchdog ticked, which would bury the one signal that tells us the
    /// pipeline's quiesce is working.
    std::int64_t last_packet_timeline_ns_ = 0;

    /// Borrowed; see `attach_pause_clock`. Null for a recording that cannot pause.
    const timing::PauseClock* pause_clock_ = nullptr;
};

} // namespace fc::audio
