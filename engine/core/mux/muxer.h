#pragma once

// Container muxing (SPEC.md §10).
//
// **Single-writer discipline (SPEC.md §10.1).** Exactly one thread owns the
// `AVFormatContext`. Every packet reaches it through the mux queue; nothing else
// in the engine touches libavformat. This is not a style preference -- libavformat
// contexts are not thread-safe, and a second writer produces a file that is
// corrupt in a way that only shows up on playback.
//
// **MP4 is written fragmented and remuxed on clean stop (SPEC.md §10.3).** A
// progressive MP4 keeps its `moov` atom at the end of the file, so a process that
// dies mid-recording leaves an unplayable brick with zero recoverable content --
// which is the "corrupted file" defect of §20 row 3. Recording fragmented means
// every keyframe closes a self-contained fragment and a hard kill leaves a file
// that plays; the progressive form, which seeks instantly and which some editors
// require, is produced by a lossless stream copy once the recording has ended and
// nothing is at stake.
//
// The fragmented file is written **at the final output path**, not at a temporary
// one, and the remux replaces it atomically. A crash should leave the user a file
// they can double-click, not one with an extension nothing opens.

#include "core/config/config_schema.h"
#include "core/encode/aac_encoder.h"
#include "core/encode/video_encoder.h"
#include "core/error/result.h"
#include "core/ffmpeg/av_raii.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fc::mux {

/// Outcome of the validation gate (SPEC.md §10.4): the output is not declared good
/// until it has been opened, demuxed, and decoded.
struct ValidationReport {
    bool valid = false;
    int stream_count = 0;
    std::string format_name;
    std::string video_codec;
    /// Container duration in seconds, as libavformat reports it. For Matroska this
    /// is the **longest** stream, so with audio present it is not the video
    /// track's length and must not be compared against a video-derived
    /// expectation.
    double duration_seconds = 0.0;
    /// The video track's own length, from the timestamps that actually decoded.
    /// This is what an expectation computed from the pacer is about.
    double video_duration_seconds = 0.0;
    /// Frames that decoded successfully during the probe.
    std::int64_t decoded_frames = 0;
    std::string detail;

    /// Empty when the file has no audio stream. Track 0 -- the system mix, which
    /// SPEC.md §8.6 makes the first audio stream of every recording.
    std::string audio_codec;
    int audio_channels = 0;
    int audio_sample_rate = 0;
    /// Samples per channel that decoded successfully.
    std::int64_t decoded_audio_samples = 0;
    /// `decoded_audio_samples / audio_sample_rate`. Compared against the video
    /// track's own length rather than trusted from the container's metadata.
    double audio_duration_seconds = 0.0;

    /// Audio streams the file actually carries. 1 for an ordinary recording, up to
    /// 6 for SPEC.md §8.6's Tier B.
    int audio_stream_count = 0;

    /// One entry per audio stream, in file order. Entry 0 duplicates the fields
    /// above; the rest are Tier B's per-application tracks.
    ///
    /// Present because §20 row 15's "the file stays valid" is a claim about *every*
    /// track: a per-application track that decodes to nothing, or that is a
    /// different length from the system mix, is the ragged-track defect row 14 is
    /// about arriving through the validation gate that was supposed to catch it.
    struct AudioTrackReport {
        std::string name;
        int channels = 0;
        int sample_rate = 0;
        std::int64_t decoded_samples = 0;
        double duration_seconds = 0.0;
    };

    std::vector<AudioTrackReport> audio_tracks;
};

/// What the caller knows the file should contain, so the gate can check it rather
/// than only checking that the file parses.
///
/// Before this existed the gate found the video stream, decoded frames, and
/// compared the *container's* duration against the expectation. A file whose audio
/// stream was missing, carried the wrong channel count, or decoded to nothing
/// passed every one of those checks, because none of them looked at it (BUG-015).
struct ValidationExpectation {
    /// Expected duration for the 1% tolerance check. 0 skips that assertion.
    double duration_seconds = 0.0;

    /// True when an audio track was written. A file that should have one and does
    /// not is not a successful recording, however well the video decodes.
    bool audio = false;

    /// Expected channel count for **track 0**, the system mix. 0 skips the check.
    ///
    /// SPEC.md §8.5 pins the layout "for the lifetime of the file", and with SPEC.md
    /// §8.6's Tier B that phrase has to be read **per stream** rather than per file —
    /// amended 2026-08-06. The system mix keeps whatever the recording chose, up to
    /// 7.1; the per-application tracks are stereo whatever it chose, because §8.6
    /// supplies their format and there is no surround information in a stereo capture
    /// to preserve. One file, two pinned layouts, neither of which may drift.
    int audio_channels = 0;

    /// Expected channel count for tracks 1..N, the per-application ones. 0 skips it.
    ///
    /// Checked separately from track 0 and not folded into it, because the interesting
    /// failure is precisely that they *differ correctly*: a build that applied the
    /// recording's 5.1 to a per-application track would produce a valid file at three
    /// times the bitrate carrying stereo content in six channels, and one that applied
    /// stereo to the system mix would silently discard the surround the user asked for.
    /// A single expectation could not tell those apart.
    int app_track_channels = 0;

    /// Expected number of audio streams. 0 skips the check.
    ///
    /// Checked rather than inferred because a Tier B recording's whole promise is
    /// the *set* of tracks: a file with five of the six that were opened is not a
    /// partially successful recording, it is one where a stream the header declared
    /// did not survive, and §8.6 forbids dropping a track from the container.
    int audio_streams = 0;
};

/// One audio stream to add at `Muxer::open` (SPEC.md §8.6).
struct AudioStreamSpec {
    /// The opened encoder to copy stream parameters from. Never null.
    const encode::AacEncoder* encoder = nullptr;

    /// The Matroska `Name` tag, written as the stream's `title` metadata.
    ///
    /// §8.6: "Every track gets a human-readable Matroska `Name` tag (`"System Mix"`,
    /// `"chrome.exe"`, `"game.exe"`). Untagged tracks are a UX failure." Empty
    /// leaves the stream untagged, which is what a single-track Tier A recording
    /// gets -- one unnamed audio stream is not ambiguous, and naming it would put a
    /// string in every existing file's metadata for no reader's benefit.
    std::string name;
};

struct MuxerSettings {
    std::filesystem::path output;
    config::Container container = config::Container::Mkv;

    /// Mirrors the encoder's colour configuration onto the container. SPEC.md §6
    /// requires the tags in the bitstream *and* the container: a player that
    /// trusts the container over the SPS VUI otherwise renders the wrong matrix.
    bool full_range = false;

    /// Expected duration, for the validation gate's 1% tolerance check. 0 skips
    /// that particular assertion.
    double expected_duration_seconds = 0.0;

    /// Timeline position, in nanoseconds, that this file's timestamps count from.
    ///
    /// Zero for an ordinary recording, where the file *is* the timeline. Non-zero for
    /// the second and subsequent files of a segmented one: SPEC.md §11 requires that
    /// "timestamps restart at 0 in each segment (each file is independently valid)", and
    /// a segment whose first packet is at 30 minutes would otherwise open with half an
    /// hour of nothing in front of it -- players seek to it, report the wrong duration,
    /// and the file is not independently valid in any useful sense.
    ///
    /// Subtracted from every packet in its *source* timebase, before the rescale to the
    /// stream's, so both tracks shift by the same instant rather than by the same
    /// integer (SPEC.md §7.1's shared epoch, preserved across the seam).
    std::int64_t timeline_origin_ns = 0;

    /// Fault injection: a real stall applied on the `mux` thread inside one write in
    /// every `injected_stall_period` (SPEC.md §20.1's chaos tier names "disk stall"
    /// among the injections the suite must perform).
    ///
    /// **Zero on every production path.** Nothing in the engine sets it; `configure`
    /// has no key for it and never will.
    ///
    /// This does not fake a measurement. The delay is real elapsed time on the one
    /// thread SPEC.md §12 permits to block on disk, so the latency rung 6 reads is
    /// the latency that actually occurred, the encode queue backs up for the same
    /// reason it would on a failing volume, and the ladder sees what it would see.
    ///
    /// Applied periodically rather than to every write because that is the shape a
    /// slow volume actually has -- most writes land in the AVIO buffer and return in
    /// microseconds, and a few pay for the whole flush. A uniform delay would make
    /// P99 equal to the mean and quietly stop testing the percentile at all.
    std::int64_t injected_stall_ns = 0;
    /// One write in this many stalls. Ignored when `injected_stall_ns` is 0.
    int injected_stall_period = 20;
};

/// Sole owner of the output `AVFormatContext`.
///
/// Threading: every method is called from the `mux` thread only (SPEC.md §12).
/// This is the one component permitted to block on disk.
class Muxer {
public:
    Muxer();
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;
    Muxer(Muxer&&) = delete;
    Muxer& operator=(Muxer&&) = delete;

    /// Opens the container and adds the video stream, copying codec parameters
    /// from `encoder`. Writes the header.
    ///
    /// `audio` adds a second stream. It must be supplied here rather than later:
    /// `avformat_write_header` fixes the stream set, and a stream added after it
    /// never appears in the file.
    [[nodiscard]] Result<void> open(const MuxerSettings& settings, const encode::IVideoEncoder& encoder,
                                    const encode::AacEncoder* audio = nullptr);

    /// The same, with SPEC.md §8.6's up to six audio streams.
    ///
    /// `audio[0]` is the system mix, which §8.6 makes track 0 of every Tier B file
    /// "so the file is useful even in a player that exposes only the first track".
    /// The rest are per-application, in the order `EncodedPacket::audio_track`
    /// numbers them.
    ///
    /// **Every stream must be here.** `avformat_write_header` fixes the stream set,
    /// which is why a track for an application that has not started yet still needs
    /// its encoder open before this call -- see `app_audio_tracks.h`.
    [[nodiscard]] Result<void> open(const MuxerSettings& settings, const encode::IVideoEncoder& encoder,
                                    std::span<const AudioStreamSpec> audio);

    /// Writes one packet. Uses `av_interleaved_write_frame`, which owns the
    /// interleaving depth -- SPEC.md §10.1 forbids hand-rolling it.
    [[nodiscard]] Result<void> write(encode::EncodedPacket packet);

    /// SPEC.md §10.4: write the trailer, close the file, flush it to disk. Safe to
    /// call twice; the second call is a no-op rather than a double-free.
    [[nodiscard]] Result<void> finalize();

    /// Opens the finished file and proves it plays. SPEC.md §10.4 forbids
    /// declaring success without this.
    ///
    /// Both tracks are decoded, not just the video one: an audio track that is
    /// absent, mislabelled or undecodable makes the recording a failure even
    /// though every frame of video is intact.
    [[nodiscard]] static Result<ValidationReport> validate(const std::filesystem::path& path,
                                                           const ValidationExpectation& expectation);

    [[nodiscard]] bool open_for_writing() const noexcept;
    /// True when at least one audio stream was added at open time.
    [[nodiscard]] bool has_audio() const noexcept;
    /// Audio streams added at open time. 0, 1, or up to 6 under SPEC.md §8.6.
    [[nodiscard]] int audio_stream_count() const noexcept;

    /// True when the output still needs the §10.3 remux to reach its final form.
    /// MP4 only; MKV finalizes in place.
    [[nodiscard]] bool needs_remux() const noexcept;

    /// Presentation time of the last video packet handed to libavformat, in
    /// nanoseconds on the recording's timeline. 0 before the first one.
    ///
    /// Exists so "how far does the file trail the recording?" is a measurement rather
    /// than an inference. A killed recording decodes to its last *complete* fragment,
    /// so that lag — not the kill instant — is what decides how much SPEC.md §20 row 3
    /// loses, and nothing reported it before BUG-034.
    [[nodiscard]] std::int64_t last_video_pts_ns() const noexcept;

    [[nodiscard]] std::uint64_t packets_written() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

    /// 99th percentile write latency over a rolling window of the most recent
    /// packets, or 0 before there are enough to have one.
    ///
    /// SPEC.md §13 rung 6's trigger. P99 and not a mean: `av_interleaved_write_frame`
    /// buffers, so most calls never touch the disk and the tail carries the whole
    /// signal. Safe to call from the `watchdog` thread while the `mux` thread writes.
    [[nodiscard]] std::int64_t write_latency_p99_ns() const;

    /// Worst single write over the recording's lifetime, for the finalization log
    /// line. The number to compare against when a user reports a stutter.
    [[nodiscard]] std::int64_t worst_write_latency_ns() const;

    /// Free space available to this process on the output's volume, or `UINT64_MAX`
    /// when the query failed.
    ///
    /// `UINT64_MAX` is deliberately not 0: SPEC.md §13 rung 6 stops the recording
    /// below 500 MB, and a failed query reading as an empty volume would stop a
    /// healthy one.
    [[nodiscard]] std::uint64_t output_volume_free_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Rewrites a fragmented MP4 as a progressive one with the `moov` atom at the
/// front (SPEC.md §10.3).
///
/// **Stream copy only.** Nothing is decoded and nothing is re-encoded, so the
/// output is bit-identical in its media payload and the operation costs seconds
/// for an hour of video. A remux that re-encoded would be both slow and lossy, and
/// would make "finalize" a step that can degrade a finished recording.
///
/// `destination` must differ from `source`. Callers that want to replace a file in
/// place use `finalize_in_place`, which does the temp-and-swap.
///
/// `audio_initial_padding` is the AAC encoder's priming, in samples, and it has to
/// be supplied rather than read from the source. A fragmented MP4 cannot carry an
/// edit list -- `empty_moov` writes the header before the durations are known --
/// so the delay is simply absent from the file being read, and copying stream
/// parameters faithfully copies its absence. The progressive form *can* carry it,
/// and without it every MP4 recording begins with 21 ms of priming samples
/// presented as content, which is a 21 ms lip-sync error at the head of the file
/// (BUG-021). Zero means the recording had no audio.
/// What one remux did. Reported because SPEC.md §10.3 makes the remux's *cost* part of the
/// requirement ("~2 s for a 1 h file"), and the thing that decides that cost -- how many
/// times the file was written -- is otherwise invisible from outside.
struct RemuxStats {
    std::uint64_t packets = 0;
    /// Bytes reserved at the head for the `moov`, or 0 when none was.
    std::int64_t moov_reserved_bytes = 0;
    /// True when the file had to be written and then rewritten (`faststart`). False is the
    /// fast path, and a regression to `faststart` is exactly what this exists to catch.
    bool second_pass = false;
    std::int64_t elapsed_ns = 0;
};

[[nodiscard]] Result<void> remux_to_progressive(const std::filesystem::path& source,
                                                const std::filesystem::path& destination, int audio_initial_padding = 0,
                                                RemuxStats* stats = nullptr);

/// Turns the fragmented MP4 at `path` into a progressive one, in place.
///
/// Writes the progressive form beside it, **validates that before touching the
/// original** (SPEC.md §10.3: "verify the output with a decode-probe before
/// deleting the source"), then swaps it in atomically. Any failure leaves `path`
/// exactly as it was -- a playable fragmented recording -- which is the outcome the
/// prime directive requires and the reason this is not done in place.
///
/// The returned report describes the file that ends up at `path`.
[[nodiscard]] Result<ValidationReport> finalize_in_place(const std::filesystem::path& path,
                                                         const ValidationExpectation& expectation,
                                                         int audio_initial_padding = 0);

} // namespace fc::mux
