#pragma once

// The shared media epoch, `t0` (SPEC.md §7.1).
//
// > `t0` = QPC at the moment the first video frame *and* first audio packet are
// > both available. All PTS are relative to `t0`. Video and audio share **one**
// > epoch.
//
// Until M4 the video pipeline set `t0` from whichever frame arrived first, which
// is correct for a video-only recording and wrong the moment audio exists: two
// streams timestamped from two different origins are offset by the difference
// between them, permanently and invisibly. A 40 ms head start on the audio device
// is a 40 ms lip-sync error that no amount of correct downstream arithmetic
// recovers, because nothing downstream knows the origins disagreed.
//
// `t0` is therefore the *later* of the two firsts -- the instant both streams are
// genuinely live. Anything captured before it clamps to index 0 rather than going
// negative, which both `Pacer` and `AudioTimeline` already do.
//
// Header-only: it is a few integers and two comparisons, and inlining it keeps
// the capture threads' first-frame path free of a call into another translation
// unit.

#include <cstdint>

namespace fc::timing {

/// Negotiates `t0` between the streams a recording actually has.
///
/// Not thread-safe by itself. The video and audio threads each observe their own
/// first timestamp, so the session owns one of these behind whatever
/// synchronisation it already uses for start-up.
class SessionEpoch {
public:
    SessionEpoch() = default;

    /// `expects_audio` false means video alone resolves the epoch. A video-only
    /// recording must not wait for an audio packet that is never coming.
    explicit SessionEpoch(bool expects_audio) noexcept : expects_audio_(expects_audio) {}

    /// Records the first video frame's timestamp. Later calls are ignored -- only
    /// the first one defines anything.
    void note_video(std::int64_t qpc_ns) noexcept {
        if (!have_video_) {
            video_ns_ = qpc_ns;
            have_video_ = true;
        }
    }

    void note_audio(std::int64_t qpc_ns) noexcept {
        if (!have_audio_) {
            audio_ns_ = qpc_ns;
            have_audio_ = true;
        }
    }

    /// True once every expected stream has produced something.
    [[nodiscard]] bool resolved() const noexcept {
        return have_video_ && (!expects_audio_ || have_audio_);
    }

    /// The shared epoch. Meaningless until `resolved()`.
    ///
    /// The *later* of the two firsts: it is the moment both streams are live, and
    /// picking the earlier one would place the other stream's first sample before
    /// its own origin.
    [[nodiscard]] std::int64_t t0_ns() const noexcept {
        if (!expects_audio_ || !have_audio_) {
            return video_ns_;
        }
        return video_ns_ > audio_ns_ ? video_ns_ : audio_ns_;
    }

    /// How far apart the two streams started, in nanoseconds. Zero for a
    /// video-only recording.
    ///
    /// Worth logging (SPEC.md §18): a large skew means one device took much longer
    /// to spin up than the other, and it is the first thing to look at when a
    /// recording opens with a lip-sync complaint.
    [[nodiscard]] std::int64_t startup_skew_ns() const noexcept {
        if (!have_video_ || !have_audio_) {
            return 0;
        }
        const std::int64_t difference = video_ns_ - audio_ns_;
        return difference < 0 ? -difference : difference;
    }

    [[nodiscard]] bool expects_audio() const noexcept {
        return expects_audio_;
    }

    [[nodiscard]] bool have_video() const noexcept {
        return have_video_;
    }

    [[nodiscard]] bool have_audio() const noexcept {
        return have_audio_;
    }

private:
    bool expects_audio_ = false;
    bool have_video_ = false;
    bool have_audio_ = false;
    std::int64_t video_ns_ = 0;
    std::int64_t audio_ns_ = 0;
};

} // namespace fc::timing
