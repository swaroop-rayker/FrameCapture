#pragma once

// Capture -> convert -> encode -> mux, wired together (SPEC.md §12).
//
// This is what turns M1's topology, M2's capture and colour, and M3's encoder and
// muxer into a recording.
//
// ---------------------------------------------------------------------------
// Deviation from SPEC.md §12, stated deliberately
// ---------------------------------------------------------------------------
// §12's thread table lists `convert` and `venc` as separate threads. This
// implementation runs both on one thread, named `fc-venc`, and runs `mux` on its
// own thread as specified.
//
// The reason is texture lifetime. `Nv12Converter` owns a single NV12 output
// texture and rewrites it on every dispatch. With conversion on its own thread,
// that texture can be overwritten while the encode thread is still copying out of
// it -- a data race on GPU memory that produces intermittently torn frames, which
// is SPEC.md §20 row 5 reintroduced by the very structure meant to make things
// faster. Splitting the two threads correctly requires a ring of output textures
// sized to the queue depth plus a fence per slot, so that a texture cannot be
// recycled until the encoder is provably done reading it.
//
// That ring is the right change when profiling says conversion and encode need to
// overlap. It is not needed to hit 1080p60 -- the conversion dispatch is a few
// hundred microseconds -- and building it now would add an unverifiable
// synchronisation surface to a milestone whose exit criterion does not exercise
// it. The capture thread is still never blocked, which is the §12 rule that
// carries the correctness argument: it pushes into a bounded drop-oldest queue and
// returns.
//
// Flagged rather than silently merged, per CLAUDE.md: SPEC.md wins, so this is a
// noted divergence to revisit, not a reinterpretation.
//
// The audio side of §12's table -- `audio`, `silence`, `aenc` -- is *not* merged.
// Those three live in `AudioPath` and each has the responsibility the table gives
// it, because the audio thread's constraints (MMCSS Pro Audio, a COM apartment it
// created its own interfaces in, no blocking) are not negotiable the way the
// convert/encode split was.

#include "core/capture/capture_frame.h"
#include "core/config/config.h"
#include "core/encode/video_encoder.h"
#include "core/error/result.h"
#include "core/health/health_monitor.h"
#include "core/mux/muxer.h"
#include "core/pipeline/app_audio_tracks.h"
#include "core/pipeline/audio_path.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;

namespace fc::pipeline {

/// Where the recording's audio comes from.
enum class AudioSource {
    /// Video only. The first video frame resolves `t0` on its own -- a video-only
    /// recording must not wait for a packet that is never coming.
    None,

    /// WASAPI loopback on a render endpoint (SPEC.md §8.1). What a real recording
    /// uses.
    SystemLoopback,

    /// The caller supplies buffers through `offer_audio` and drives the silence
    /// watchdog through `tick_audio_silence`.
    ///
    /// This is how the deterministic A/V-sync tests exist at all. SPEC.md §20
    /// row 4 asserts a bounded offset between a tone and a flash at exact frame
    /// boundaries, which requires both streams to be *generated*, on one synthetic
    /// clock, from a known signal -- and CLAUDE.md §5 requires tests to use the
    /// synthetic source rather than whatever the machine happens to be playing.
    /// Nothing else in the engine selects it.
    External,
};

struct PipelineAudioSettings {
    AudioSource source = AudioSource::None;

    /// Empty selects the default render endpoint. `SystemLoopback` only.
    std::string device_id;

    config::ChannelLayoutSetting channel_layout = config::ChannelLayoutSetting::Auto;

    /// 0 selects SPEC.md §8.5's ladder: 192 stereo / 384 5.1 / 512 7.1.
    int bitrate_kbps = 0;

    /// The format the caller's buffers arrive in. `External` only -- for
    /// `SystemLoopback` the endpoint's negotiated mix format wins, because that is
    /// what the buffers actually contain.
    audio::MixFormat external_format;

    /// SPEC.md §8.1's 20 ms buffer, which also sets the silence watchdog's
    /// two-period threshold.
    std::int64_t buffer_period_ns = 20'000'000;

    /// SPEC.md §8.6's Tier B. Default-constructed means off, and it stays off
    /// unless the user ticked the box — the same rule CLAUDE.md hard rule 7 states
    /// for segmentation, for the same reason: a feature that turns itself on is a
    /// feature nobody chose.
    ///
    /// Requires `source` to be something other than `None`. §8.6 makes track 0 the
    /// full system mix "so the file is useful even in a player that exposes only
    /// the first track", so per-application tracks without one are not a
    /// configuration this can honour.
    struct Multitrack {
        bool enabled = false;
        AppTrackSource source = AppTrackSource::ProcessLoopback;
        /// Tracks 1..5. Track 0 is the system mix and is not listed here.
        std::vector<AppTrackConfig> tracks;
        /// See `AppTracksSettings::reattach`. A track follows its executable back when
        /// the application is reopened mid-recording.
        bool reattach = true;
        /// `ProcessLoopback` only; empty selects the real client.
        AppTrackSourceFactory source_factory;
    } multitrack;
};

/// One finished file of a segmented recording (SPEC.md §11).
///
/// Collected so the `<basename>.segments.json` sidecar can record "each segment's global
/// start offset so external tools can concatenate" -- the offset is the recording's own
/// timeline, which each file has had subtracted from its timestamps to restart at zero.
struct SegmentRecord {
    std::filesystem::path path;
    /// Where this segment begins on the whole recording's timeline.
    std::int64_t start_offset_ns = 0;
    /// Where it ends, which is where the next one begins.
    std::int64_t end_offset_ns = 0;
};

struct PipelineSettings {
    std::filesystem::path output;
    config::VideoSettings video;

    /// SPEC.md §11. Default-constructed means disabled, and CLAUDE.md hard rule 7 makes
    /// that non-negotiable: nothing splits unless the user ticked the box.
    config::SegmentationSettings segmentation;

    /// libavcodec encoder name and pool bind flags from the M1 capability probe.
    std::string encoder_name;
    std::uint32_t pool_bind_flags = 0;

    /// When this is anything but `None` the pipeline waits for the first audio
    /// packet before pacing anything, so `t0` is the shared epoch of SPEC.md §7.1
    /// rather than whichever stream happened to produce first.
    PipelineAudioSettings audio;

    /// SPEC.md §20.1 chaos-tier disk-stall injection, passed through to the muxer.
    /// Zero on every production path -- see `mux::MuxerSettings::injected_stall_ns`.
    std::int64_t injected_stall_ns = 0;
    int injected_stall_period = 20;

    /// Called during `stop()`'s finalization with SPEC.md §10.4's stage and how far it
    /// has got (M9.6 §2.1). Null on every path that has nobody to tell -- the tests, and
    /// the recovery path, which finalizes a file whose recording session is long gone.
    ///
    /// Invoked on whichever thread called `stop()`, which is the IPC request thread. It
    /// is never a capture, encode or audio thread: those have been joined by the time
    /// finalization begins, which is what makes a callback here safe at all.
    mux::FinalizeProgressFn on_finalize_progress;
};

/// Counters for the health monitor (M6) and for tests.
struct PipelineStats {
    std::uint64_t frames_submitted = 0;     ///< handed to the pipeline by capture
    std::uint64_t frames_encoded = 0;       ///< reached the encoder, including duplicates
    std::uint64_t frames_paced_out = 0;     ///< dropped by the CFR pacer (source outran the grid)
    std::uint64_t frames_queue_dropped = 0; ///< dropped by queue pressure
    std::uint64_t duplicates_emitted = 0;
    std::uint64_t packets_muxed = 0;
    std::uint64_t bytes_written = 0;

    /// Presentation time of the last video packet handed to the container, in
    /// nanoseconds on the recording's timeline.
    ///
    /// The gap between this and the recording's elapsed time is how far the file
    /// trails the capture. It is what decides how much a `TerminateProcess` costs —
    /// a killed recording decodes to its last complete fragment, not to the instant
    /// it died — and it was unmeasured until BUG-034.
    std::int64_t last_muxed_pts_ns = 0;
    /// Frames discarded because the shared epoch was not resolved yet -- audio
    /// had not produced its first packet. Expected to be small and non-zero at
    /// the very start of a recording with audio; a large number means the audio
    /// device took an unreasonable time to start.
    std::uint64_t frames_awaiting_epoch = 0;

    std::uint64_t convert_failures = 0;
    std::uint64_t encode_failures = 0;

    /// SPEC.md §7.5. Total time excised from the file by pausing, in nanoseconds, and
    /// the completed pause count.
    ///
    /// §7.5 requires this in `get_stats` and in the finalization log by name, because
    /// "why is my 30-minute recording 12 minutes long" must be answerable from the log
    /// rather than from the user's memory of what they pressed.
    std::int64_t paused_total_ns = 0;
    std::uint64_t pauses = 0;

    /// Frames discarded because they were captured inside a paused span. Expected to be
    /// small and non-zero around each pause -- capture is asked to stop, and the frame
    /// already in flight when it was asked lands here. Not a drop; see
    /// `timing::Pacer::excised`.
    std::uint64_t frames_excised = 0;

    /// Items that reached the pause clock describing a moment before the most recent
    /// pause, after that pause had ended (`timing::PauseClock::stragglers`).
    ///
    /// **Zero is the contract, and it is asserted rather than assumed.** The pipeline
    /// quiesces at the pause boundary specifically so this cannot happen; a non-zero
    /// value means the quiesce did not hold and some frames were placed against the
    /// wrong paused total. SPEC.md §20 row 18's test asserts it.
    std::uint64_t pause_stragglers = 0;

    /// Files this recording has written (SPEC.md §11). 1 unless segmentation split it.
    ///
    /// Distinct from `RecordingSession`'s count of the same name, which also increments
    /// for a cross-adapter migration's unplanned break -- this one counts only the splits
    /// the user asked for.
    std::uint64_t segments = 1;

    /// Successful `rebuild_device` calls (SPEC.md §5.4). Non-zero means the recording
    /// survived a device loss in the same file.
    std::uint64_t device_rebuilds = 0;

    /// How far captured content lags the slot it is shown in (SPEC.md §7.2 keeps
    /// the first frame that maps to a slot, not the freshest).
    ///
    /// The mean is a constant offset and harmless -- it shifts the whole timeline
    /// equally. The spread between mean and worst is the part that varies frame to
    /// frame, and it grows with the source's frame interval, so a source running
    /// slowly or erratically shows up here and nowhere else.
    std::int64_t worst_staleness_ns = 0;
    std::int64_t mean_staleness_ns = 0;
};

/// The degradation ladder's current view of the recording (SPEC.md §13).
///
/// Published by the `watchdog` thread and read by anyone -- the caller's record
/// loop, the GUI's `get_health` (SPEC.md §15.1), the tests. A snapshot rather than
/// live references, so a reader never sees half of a transition.
struct PipelineHealth {
    health::Rung rung = health::Rung::Nominal;

    /// Rung changes over the recording. 0 is a recording that never degraded.
    std::uint64_t rung_transitions = 0;

    /// The rate the ladder currently wants, and the rate the pacer is actually on.
    /// They differ only in the window between a rung 3 decision and the next frame
    /// reaching the venc thread, which is where the pacer is allowed to be touched.
    int target_fps = 0;
    int pacer_fps = 0;

    /// Times the pacer was retimed by the ladder. SPEC.md §13's hysteresis is
    /// supposed to keep this in the low single digits; a large number means the
    /// recording oscillated and the recording is worse for it.
    std::uint64_t retimes = 0;

    /// Queue-drop ratio over the monitor's most recent 3 s window.
    double drop_ratio = 0.0;

    /// What rung 6 is reading.
    std::int64_t disk_write_p99_ns = 0;
    std::uint64_t disk_free_bytes = 0;

    /// SPEC.md §20 row 9's detector. The recovery half is M7 -- see
    /// docs/ACCEPTANCE.md row 9 -- so in M6 these are observations, not actions.
    std::uint64_t stall_episodes = 0;
    std::int64_t worst_stall_ns = 0;
    /// How long capture has been silent *right now*, or 0 when it is producing.
    ///
    /// Distinct from `worst_stall_ns`, and the distinction matters to a caller deciding
    /// whether to act: SPEC.md §20 row 9's 3x-frame-interval threshold is a *detection*
    /// threshold, and acting on it directly is wrong for a change-driven backend. WGC
    /// produces a frame when the desktop composites, so a static screen legitimately
    /// goes far longer than 50 ms without one -- measured, an idle 6 s recording trips
    /// the detector. A caller that rebuilds on it thrashes every unattended recording.
    std::int64_t current_stall_ns = 0;

    /// SPEC.md §13 rung 6's second threshold or rung 7: the caller should stop
    /// feeding frames and call `stop()`.
    ///
    /// **Reported rather than acted on, and deliberately so.** `stop()` joins the
    /// watchdog thread, so a watchdog that called it would deadlock on itself. More
    /// to the point, the pipeline does not own capture -- the caller does -- and
    /// "stop capture, finalize the file successfully" (rung 7) is therefore the
    /// caller's move to make. The pipeline's job is to make sure the file finalizes
    /// when it does.
    bool stop_requested = false;

    /// SPEC.md §13 rung 4. Reported only; the §5.4 migration is M7's deliverable.
    bool gpu_migration_requested = false;

    /// False once the recording is on the software encoder (rung 5).
    bool hardware_encoder = true;
};

/// Owns the encode and mux stages and the threads that drive them.
///
/// Lifetime: `start` then any number of `submit` calls then `stop`. `stop` is what
/// finalizes the file, and it is safe to call from a failure path -- CLAUDE.md §1
/// requires a valid output file from any failure short of losing the disk.
class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;
    VideoPipeline(VideoPipeline&&) = delete;
    VideoPipeline& operator=(VideoPipeline&&) = delete;

    /// Opens the encoder and container and starts the threads. `device` must own
    /// the textures that will be submitted.
    [[nodiscard]] Result<void> start(ID3D11Device* device, const PipelineSettings& settings);

    /// Hands one captured frame to the pipeline.
    ///
    /// **Never blocks.** Called from the capture thread, where blocking is
    /// forbidden (SPEC.md §12, CLAUDE.md hard rule 4). Under pressure the frame is
    /// dropped and counted rather than waited on.
    ///
    /// The caller may recycle `frame.texture` only after the pipeline is done with
    /// it; in practice callers hold a reference for the queue's lifetime by keeping
    /// the capture backend's frame checked out until `submit` returns, which is
    /// safe because conversion happens on the venc thread from a texture the
    /// capture backend still owns.
    Result<void> submit(const capture::CaptureFrame& frame);

    /// Reports the first audio packet's timestamp, so `t0` can be the moment
    /// **both** streams are live (SPEC.md §7.1).
    ///
    /// Until this is called -- or `start` is told not to expect audio -- submitted
    /// frames are held rather than paced, because a frame paced against a
    /// provisional epoch would have to be re-timed once the real one arrives, and
    /// re-timing an already-emitted PTS is not possible.
    ///
    /// Called automatically from the `audio` thread for `SystemLoopback`. It also
    /// *publishes* the resolved epoch to the audio path, which is what lets the
    /// audio timeline start at the same instant the pacer does: noting the
    /// timestamp without publishing the result would leave the two streams sharing
    /// an epoch neither of them could see.
    void note_first_audio(std::int64_t qpc_ns);

    /// Hands one buffer to the audio path. `AudioSource::External` only.
    ///
    /// Same contract as the WASAPI sink it stands in for: the buffer's memory is
    /// copied out before this returns, so the caller may reuse it immediately.
    void offer_audio(const audio::LoopbackBuffer& buffer);

    /// Hands one buffer to per-application track `index` (1-based).
    /// `AppTrackSource::External` only; see that enumerator for why it exists.
    void offer_app_audio(int index, const audio::LoopbackBuffer& buffer);

    /// Runs one silence-watchdog check against `now_ns`, on **every** track --
    /// track 0 and each of SPEC.md §8.6's per-application ones. `External` only;
    /// the production paths have threads doing this on a timer.
    ///
    /// Separate from `offer_audio` because a synthetic clock has no thread to run
    /// on: a test that advances time in jumps needs to tell the watchdog when
    /// "now" is, rather than having it read a wall clock the test is not using.
    void tick_audio_silence(std::int64_t now_ns);

    /// Replaces the D3D device, colour converter and video encoder while keeping the
    /// muxer, the pacer, the shared epoch and the audio path (SPEC.md §5.4 steps 5-7).
    ///
    /// **Refuses unless the new encoder's `extradata` is byte-identical to the
    /// original's**, with `GPU_MIGRATION_FAILED`. The container's parameter sets were
    /// fixed by `avformat_write_header` and cannot be rewritten; continuing a file with
    /// different SPS/PPS produces one whose remainder does not decode -- measured at 31
    /// of 60 frames, which is why SPEC.md §5.4 was amended. A refusal is the caller's
    /// cue to close the segment and open the next one instead, and putting the check
    /// here means neither layer has to *remember* that rule.
    ///
    /// Reserves `reorder_depth` grid slots before resuming, so the rebuilt encoder's
    /// first DTS cannot precede the last one written (see `Pacer::reserve_discontinuity`).
    /// The slots after that are duplicate-filled as usual, so the gap costs one
    /// reorder-depth hole and nothing more.
    ///
    /// Called with no `submit` in flight -- the caller pauses capture first. Stops the
    /// `venc` thread, swaps, and restarts it.
    [[nodiscard]] Result<void> rebuild_device(ID3D11Device* device, const std::string& encoder_name,
                                              std::uint32_t pool_bind_flags);

    /// Excises time from here until `resume` (SPEC.md §7.5).
    ///
    /// The muxer, the encoder, the pacer and the audio timeline all stay exactly as
    /// they are: §7.5 forbids a trailer, a new file, a header rewrite or an encoder
    /// rebuild, because the parameter sets were fixed by `avformat_write_header` and
    /// §5.4's amendment established that they cannot change mid-file. What changes is
    /// one shared number, and everything downstream derives from it.
    ///
    /// **Quiesces before publishing the pause.** Frames already queued belong to the
    /// unpaused timeline and must be placed against the paused total as it stood when
    /// they were captured; letting them sit until after the resume would place them
    /// against a larger one. Bounded -- a venc thread that cannot drain within the
    /// deadline is logged and the pause proceeds, because refusing to pause is worse
    /// than a handful of frames landing late, and `PipelineStats::pause_stragglers`
    /// counts exactly how many did.
    ///
    /// **Idempotent** (§7.5): pausing a paused recording succeeds and changes nothing.
    /// Returns `INTERNAL_INVALID_STATE` only when there is no recording at all.
    ///
    /// Called from the owner's thread, never from `venc`, `mux` or `watchdog`.
    [[nodiscard]] Result<void> pause();

    /// `pause`, with the instant supplied rather than read from the clock.
    ///
    /// The same seam `tick_audio_silence` has, for the same reason stated there: SPEC.md
    /// §20 row 18's test generates both streams from one *synthetic* clock, and a pause
    /// that read `qpc_now_ns()` would excise a span of real time from a timeline made of
    /// synthetic time. The excision would be nonsense and the test would measure the
    /// harness.
    ///
    /// Production calls `pause()`. Nothing in the engine calls this.
    [[nodiscard]] Result<void> pause_at(std::int64_t now_ns);

    /// Resumes (SPEC.md §7.5). Idempotent in the same sense.
    ///
    /// Requests a forced IDR on the next frame, because content has jumped and a
    /// P-frame referencing pre-pause pictures is a visible smear at the seam. The
    /// request is published for the `venc` thread rather than applied here -- the
    /// encoder is that thread's, and `apply_ladder_request` established the pattern.
    [[nodiscard]] Result<void> resume();

    /// `resume`, with the instant supplied. See `pause_at`.
    [[nodiscard]] Result<void> resume_at(std::int64_t now_ns);

    /// The file currently being written.
    ///
    /// Not `PipelineSettings::output` when SPEC.md §11 segmentation is on: the first file
    /// of a split recording is `<basename>_part001`, and every caller that reports a path
    /// to a user must report the one that exists. BUG-043 was this mistake in the other
    /// direction, and the lesson generalises -- the layer that opened the file is the one
    /// that knows its name.
    [[nodiscard]] std::filesystem::path current_output() const;

    [[nodiscard]] bool paused() const noexcept;

    /// Total excised time so far, in nanoseconds (SPEC.md §7.5). Also in
    /// `PipelineStats::paused_total_ns`; exposed here so a caller that has just
    /// resumed can log the number without taking the stats lock.
    [[nodiscard]] std::int64_t paused_total_ns() const noexcept;

    /// Drains the queues, flushes the encoder, writes the trailer and validates the
    /// output (SPEC.md §10.4). Idempotent.
    ///
    /// A recording paused when this arrives finalizes normally and yields a valid file
    /// (§7.5's invariant). Nothing special is needed for that: the paused span is
    /// simply absent from the emitted PTS, and `Pacer::timeline_seconds` -- which the
    /// §10.4 gate reads -- derives from emitted PTS rather than from a frame count over
    /// a rate, so it already excludes paused time (BUG-025's fix, paying off twice).
    [[nodiscard]] Result<mux::ValidationReport> stop();

    [[nodiscard]] PipelineStats stats() const;

    /// Counters for the audio half. All zero when `AudioSource::None`.
    ///
    /// **Track 0 only** -- the system mix, which is Tier A's whole output and is
    /// unaffected by whether Tier B is on. Per-application tracks report separately
    /// through `app_track_stats`, deliberately: a single blended figure would make
    /// "did Tier B change Tier A?" unanswerable from the counters, which is half of
    /// SPEC.md §24's M9.5 exit criterion.
    [[nodiscard]] AudioStats audio_stats() const;

    /// SPEC.md §8.6's per-application tracks. Empty when Tier B is off.
    [[nodiscard]] AppTracksStats app_track_stats() const;

    /// The degradation ladder's current view (SPEC.md §13). Meaningful from the
    /// first watchdog tick after `start`; zeroed before it.
    [[nodiscard]] PipelineHealth health() const;

    [[nodiscard]] bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /// Set when a worker missed the SPEC.md §12 join deadline and was detached.
    ///
    /// A detached thread is still reading `impl_`, so the destructor deliberately
    /// leaks it rather than freeing memory out from under a running thread. The
    /// leak is bounded -- one pipeline's worth, once, on a path that has already
    /// logged an error -- and is the lesser fault next to a use-after-free.
    bool abandoned_ = false;
};

} // namespace fc::pipeline
