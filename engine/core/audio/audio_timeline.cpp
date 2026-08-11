#include "core/audio/audio_timeline.h"

#include <algorithm>

namespace fc::audio {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

} // namespace

void AudioTimeline::start(std::int64_t t0_ns) noexcept {
    t0_ns_ = t0_ns;
    last_packet_qpc_ns_ = t0_ns;
    last_packet_timeline_ns_ = t0_ns;
    frames_written_ = 0;
    silence_injected_ = 0;
    discontinuities_ = 0;
    dropped_ = 0;
    excised_ = 0;
    started_ = true;
}

std::optional<std::int64_t> AudioTimeline::timeline_qpc_ns(std::int64_t qpc_ns) const noexcept {
    if (pause_clock_ == nullptr) {
        return qpc_ns;
    }
    const timing::PauseClock::Mapping mapped = pause_clock_->map(qpc_ns, t0_ns_);
    if (mapped.state == timing::PauseClock::State::Excised) {
        return std::nullopt;
    }
    // Returned in the same coordinate system the callers already work in -- an absolute
    // QPC-like value measured from the same origin -- so nothing below this line needs
    // to know whether the recording has ever been paused.
    return t0_ns_ + mapped.timeline_ns;
}

std::int64_t AudioTimeline::signed_frame_at(std::int64_t timeline_ns) const noexcept {
    const std::int64_t elapsed = timeline_ns - t0_ns_;
    const bool negative = elapsed < 0;
    const std::int64_t magnitude = negative ? -elapsed : elapsed;

    // round(elapsed * rate / 1e9) in integers. Split so the intermediate cannot
    // overflow: at 48 kHz `elapsed * rate` passes 2^63 after about 5 hours, which
    // is inside the 4-hour soak's blast radius and well inside a long recording.
    const std::int64_t seconds = magnitude / kNsPerSecond;
    const std::int64_t remainder = magnitude % kNsPerSecond;
    const std::int64_t frames =
        (seconds * sample_rate_) + (((remainder * sample_rate_) + (kNsPerSecond / 2)) / kNsPerSecond);
    return negative ? -frames : frames;
}

std::int64_t AudioTimeline::frame_at(std::int64_t timeline_ns) const noexcept {
    return std::max<std::int64_t>(signed_frame_at(timeline_ns), 0);
}

TimelineSegment AudioTimeline::accept(const CapturedPacket& packet) {
    TimelineSegment segment;
    if (!started_) {
        // No epoch yet. Inventing one from the first audio packet is exactly what
        // SPEC.md §7.1 forbids -- video and audio share one, negotiated by the
        // session -- so the packet is refused rather than silently redefining the
        // timeline's origin.
        segment.dropped = true;
        ++dropped_;
        return segment;
    }

    // SPEC.md §7.5: on pause the timeline stops accepting buffers. A packet captured
    // inside the paused span has no place on the timeline -- writing it would lengthen
    // the timeline by content the file is supposed not to contain, and would do it
    // *only* on the audio side, which is the desync §7.5 exists to prevent.
    //
    // Refused before `last_packet_qpc_ns_` is touched, deliberately: advancing the
    // watchdog's idea of "the last packet" from a buffer that was thrown away would
    // suppress the silence generator for a gap that really does need filling.
    const std::optional<std::int64_t> timeline_ns = timeline_qpc_ns(packet.qpc_ns);
    if (!timeline_ns.has_value()) {
        segment.start_frame = frames_written_;
        segment.dropped = true;
        ++excised_;
        return segment;
    }

    last_packet_qpc_ns_ = std::max(last_packet_qpc_ns_, packet.qpc_ns);
    last_packet_timeline_ns_ = std::max(last_packet_timeline_ns_, *timeline_ns);

    if (packet.flags.discontinuity) {
        // The device dropped samples. The gap is bridged below by the ordinary
        // silence path; what matters here is that it is *counted*, because a
        // recording with discontinuities is one where the timeline was rebuilt
        // rather than observed.
        ++discontinuities_;
    }

    // A packet can genuinely predate the shared epoch: `t0` is the *later* of the
    // two streams' firsts (SPEC.md §7.1), so an audio device that opened before
    // the first video frame delivers buffers that belong before the timeline
    // starts. Those leading frames are trimmed, not clamped -- clamping the whole
    // packet to index 0 writes pre-epoch audio *at* the epoch and displaces the
    // audio that actually belongs there (BUG-014).
    const std::int64_t signed_start = signed_frame_at(*timeline_ns);
    const std::int64_t pre_epoch_frames = signed_start < 0 ? -signed_start : 0;
    // Not const: a sub-buffer gap snaps it to the write head. See the jitter guard below.
    std::int64_t start = signed_start < 0 ? 0 : signed_start;
    const std::int64_t contributed = packet.frames - pre_epoch_frames;

    segment.start_frame = frames_written_;

    if (contributed <= 0) {
        // Entirely before `t0`. Nothing here belongs on the timeline at all.
        segment.dropped = true;
        ++dropped_;
        return segment;
    }

    // -----------------------------------------------------------------------
    // A gap, or a jittery timestamp? (BUG-042)
    // -----------------------------------------------------------------------
    // **An endpoint cannot lose less than one buffer.** WASAPI delivers whole buffers
    // and stamps each with the QPC at which it was captured; that stamp carries a few
    // tens of microseconds of scheduling noise, so a packet routinely maps a handful of
    // frames beyond the write head with nothing actually missing. Filling that is not a
    // repair -- it splices silence into continuous audio, and the packet behind it is
    // then trimmed against the advanced head, so real samples are discarded to make room
    // for the silence.
    //
    // Measured on the reference rig before this guard, over a 40 s loopback recording:
    // **2591 gap fills averaging 4.1 frames -- 85 microseconds -- each**, on roughly
    // three buffers in five. Individually inaudible; at one every 15 ms they are a
    // periodic discontinuity around 65 Hz, which is heard as roughness rather than as
    // dropouts, and is what a user reported as the audio sounding wrong.
    //
    // So the line is drawn where the physics is: below one buffer period the gap cannot
    // be lost audio, and the packet is snapped to the write head instead. The frames are
    // still all written -- nothing is dropped, the track just follows the device's own
    // count across the seam rather than the noise on its clock. What that costs is the
    // endpoint's crystal error accumulating instead of being nulled every buffer, which
    // is precisely the quantity SPEC.md §8.4's ladder exists to measure and correct:
    // measured at 29 ppm here, or 1.15 ms over 40 s, against a 5 ms do-nothing band.
    //
    // A genuine dropout is unaffected: it is at least one whole buffer, so it still
    // fills, and the silent-desktop case fills as it always did.
    const std::int64_t gap = start - frames_written_;
    // One whole buffer, in frames. `signed_frame_at` measures from `t0_`, so a duration
    // is converted by asking where it lands relative to the origin.
    const std::int64_t jitter_tolerance = signed_frame_at(t0_ns_ + buffer_period_ns_);
    if (gap >= jitter_tolerance) {
        // A gap nothing covered. This is the silent-desktop case and the
        // dropped-sample case both: fill exactly the missing duration, so the
        // frames after it land where their timestamps say and not earlier.
        segment.silence_frames = gap;
        silence_injected_ += segment.silence_frames;
        frames_written_ += segment.silence_frames;
        ++gap_fills_;
    } else if (gap != 0 && gap > -jitter_tolerance && pre_epoch_frames == 0) {
        // Noise, in either direction, and the guard is symmetric because the noise is.
        // A timestamp landing *behind* the head is the same jitter with the other sign,
        // and the overlap trim below would silently discard real samples for it exactly
        // as a forward gap silently inserted silence. Measured: 4 frames lost across 400
        // jittery buffers -- inaudible alone, and the identical mistake.
        //
        // The packet's samples are all real and all belong on the track; only *where*
        // they start was uncertain, and the write head is a better answer than a stamp
        // carrying tens of microseconds of scheduling noise.
        start = frames_written_;
        snapped_frames_ += gap < 0 ? -gap : gap;
        ++snaps_;
    }

    // A packet whose span lies entirely behind the write head carries nothing new.
    // Writing it anyway would append audio out of order, which is worse than
    // losing it -- the samples would be heard at the wrong time rather than not at
    // all.
    const std::int64_t end = start + contributed;
    if (end <= frames_written_) {
        segment.dropped = true;
        ++dropped_;
        return segment;
    }

    // Overlap is trimmed from the front rather than the back: the tail is the part
    // that extends the timeline, and the head duplicates ground already written.
    const std::int64_t usable = end - std::max(start, frames_written_);
    segment.audio_frames = usable;
    segment.content_is_silence = packet.flags.silent;

    if (packet.flags.silent) {
        // SPEC.md §8.2: a SILENT buffer has undefined contents and must be filled
        // with zeros, never skipped. It still occupies its full duration.
        silence_injected_ += usable;
    }

    frames_written_ += usable;
    return segment;
}

std::int64_t AudioTimeline::frames_missing_at(std::int64_t now_ns) const noexcept {
    if (!started_) {
        return 0;
    }
    const std::optional<std::int64_t> timeline_ns = timeline_qpc_ns(now_ns);
    if (!timeline_ns.has_value()) {
        // SPEC.md §7.5: "stop the silence generator". A paused recording is missing
        // nothing -- the interval it is not covering is the interval the file is
        // supposed not to contain. Reporting a shortfall here is the specific mistake
        // §7.5 calls out: "injecting silence for the paused span lengthens the timeline
        // by exactly what the pause removed", which un-does the excision on the audio
        // side only and desyncs the file permanently.
        return 0;
    }
    const std::int64_t should_have = frame_at(*timeline_ns);
    return should_have > frames_written_ ? should_have - frames_written_ : 0;
}

bool AudioTimeline::needs_silence_at(std::int64_t now_ns) const noexcept {
    if (!started_) {
        return false;
    }
    const std::optional<std::int64_t> timeline_ns = timeline_qpc_ns(now_ns);
    if (!timeline_ns.has_value()) {
        return false; // paused; see `frames_missing_at`
    }
    // SPEC.md §8.2's trigger. One buffer period of quiet is ordinary scheduling
    // jitter; two means the endpoint has genuinely stopped producing.
    //
    // Measured in timeline time on both sides, so a resume does not look like a gap the
    // length of the pause. The comparison is the same one it always was for a recording
    // that never pauses, where the two coordinate systems coincide.
    return (*timeline_ns - last_packet_timeline_ns_) > (2 * buffer_period_ns_) && frames_missing_at(now_ns) > 0;
}

TimelineSegment AudioTimeline::inject_silence(std::int64_t frames) {
    TimelineSegment segment;
    segment.start_frame = frames_written_;
    if (!started_ || frames <= 0) {
        return segment;
    }

    segment.silence_frames = frames;
    segment.content_is_silence = true;
    silence_injected_ += frames;
    frames_written_ += frames;
    return segment;
}

} // namespace fc::audio
