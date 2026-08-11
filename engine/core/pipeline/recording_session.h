#pragma once

// A recording: capture, the pipeline, and the machinery that keeps them alive
// (SPEC.md §5.4, §14.2, §20 rows 9 and 11).
//
// This header was written before its implementation, because SPEC.md §5.4's migration
// procedure spans four subsystems and getting the ownership wrong is expensive to undo.
//
// ---------------------------------------------------------------------------
// Why this component has to exist
// ---------------------------------------------------------------------------
// Today the caller owns the capture backend and pushes frames into `VideoPipeline`.
// That works until something has to be rebuilt mid-recording, because SPEC.md §5.4's
// procedure is:
//
//     1. Signal PAUSE_CAPTURE. Encoder queue continues draining.
//     2. Flush the video encoder.
//     3. Tear down capture session + colour-convert pipeline + D3D device.
//     4. Re-run discovery and encoder selection.
//     5. Rebuild device, capture session, convert pipeline, encoder.
//     6. Force an IDR. Reuse the SAME muxer, stream index and timebase.
//     7. Emit duplicates covering the gap so the CFR timeline stays contiguous.
//     8. Resume. Log GPU_MIGRATION with before/after LUIDs and the gap.
//
// Steps 1 and 3 are the caller's territory; 2, 5, 6 and 7 are the pipeline's; and 6
// requires the muxer to outlive the encoder that was writing to it. No component
// currently spans that, which is why rows 9 and 11 have been waiting.
//
// Three other things have been waiting on the same seam and land here:
//   * `PipelineHealth::stop_requested` (SPEC.md §13 rungs 6 and 7) is reported by the
//     watchdog and consumed by nobody, because the pipeline cannot stop itself --
//     `stop` joins the watchdog thread.
//   * `PipelineHealth::gpu_migration_requested` (rung 4) is likewise reported only.
//   * SPEC.md §20 row 9's stall recovery is a same-adapter rebuild, i.e. the easy
//     case of the same procedure.
//
// ---------------------------------------------------------------------------
// What the amendment of 2026-07-30 settled, and what it means here
// ---------------------------------------------------------------------------
// SPEC.md §5.4 originally required one continuous file across any migration.
// `MigrationParameterSetTest` measured that this is impossible across vendors --
// `h264_amf` emits 28 bytes of SPS/PPS and `h264_nvenc` 53, in-band parameter sets do
// not re-initialise the decoder (31 of 60 frames decoded), and the container's `avcC`
// is fixed at `avformat_write_header`. The owner amended §5.4 accordingly, and the
// consequence for this class is a pleasant one:
//
//   **Same adapter**       the device died but came back -- a driver restart, a
//                          `DEVICE_RESET`, or row 9's stall. Parameter sets are
//                          byte-identical (asserted), so the encoder is rebuilt
//                          underneath the *same* muxer and the file continues.
//   **Different adapter**  a real topology change. Parameter sets differ, so the
//                          current file is finalized and the next one opened. That is
//                          `stop()` followed by `start()` on a fresh output path --
//                          no surgery on anything.
//
// So exactly one intrusive capability is needed, and only for the same-adapter case:
// `VideoPipeline::rebuild_device`. See its note below.
//
// ---------------------------------------------------------------------------
// The constraint SPEC.md §5.4 does not mention
// ---------------------------------------------------------------------------
// The rebuilt encoder's first DTS must not precede the last DTS already written.
// SPEC.md §9 sets `max_b_frames = 2`, so DTS lags PTS by the reorder depth and a fresh
// encoder re-derives DTS from its own first PTS. A zero-gap handover is rejected by
// libavformat outright (`non monotonically increasing dts ... 483 >= 467`, measured).
// The 350 ms budget is ~21 frames at 60 fps against a reorder depth of 3, so this is
// satisfied comfortably in practice -- but `MigrationPlan::minimum_pts_advance_frames`
// makes it explicit rather than leaving it to the budget.

#include "core/capture/i_screen_capture.h"
#include "core/config/config.h"
#include "core/error/result.h"
#include "core/gpu/device_watcher.h"
#include "core/gpu/gpu_topology.h"
#include "core/mux/muxer.h"
#include "core/pipeline/video_pipeline.h"
#include "core/preview/preview_ring.h"
#include "core/preview/preview_writer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fc::pipeline {

/// Why a rebuild happened. Logged with every `GPU_MIGRATION` event, and the field a
/// user's bug report is triaged from.
enum class RebuildCause {
    /// `DXGI_ERROR_DEVICE_REMOVED` or `DEVICE_RESET` from a D3D call.
    DeviceLost,
    /// The adapter set changed under the recording (SPEC.md §5.4's poll).
    TopologyChanged,
    /// SPEC.md §13 rung 4: three encoder failures in 60 s.
    LadderRequested,
    /// SPEC.md §20 row 9: no frame for 3x the frame interval.
    CaptureStalled,
    /// `DXGI_ERROR_ACCESS_LOST` -- the session, not the device.
    AccessLost,
};

[[nodiscard]] std::string_view to_string(RebuildCause value) noexcept;

/// What one rebuild did, for the log, the GUI event and row 11's assertions.
struct MigrationRecord {
    RebuildCause cause = RebuildCause::DeviceLost;
    gpu::AdapterId from;
    gpu::AdapterId to;
    /// Wall-clock nanoseconds from pausing capture to the first frame accepted after
    /// resuming. SPEC.md §5.4 budgets 350 ms (`gpu::kMigrationGapBudgetNs`).
    std::int64_t gap_ns = 0;
    /// True when the file continued; false when a new one was opened because the
    /// parameter sets could not match (the amended §5.4).
    bool same_file = false;
    /// The output actually being written after this rebuild. Differs from the
    /// previous one only when `same_file` is false.
    std::filesystem::path output;
    /// Set when the rebuild failed and the recording stopped instead. The file is
    /// still finalized -- CLAUDE.md §1 -- but no frames follow.
    bool failed = false;

    /// Where `gap_ns` went, in nanoseconds. Present so a gap over budget is a
    /// diagnosis rather than a number: the phases have very different characters --
    /// `discovery_ns` and `device_ns` are driver time, `pipeline_ns` is the encoder
    /// open, `capture_ns` is the backend's own start latency -- and which one grew
    /// says what to do about it. They sum to slightly less than `gap_ns`; the
    /// remainder is the teardown and the bookkeeping between them.
    std::int64_t teardown_ns = 0;
    std::int64_t discovery_ns = 0;
    std::int64_t device_ns = 0;
    std::int64_t pipeline_ns = 0;
    std::int64_t capture_ns = 0;
};

struct SessionSettings {
    /// Where the first file goes. A cross-adapter migration derives subsequent names
    /// from this using SPEC.md §11's segment naming.
    std::filesystem::path output;

    capture::CaptureTarget target;
    config::CaptureBackend backend = config::CaptureBackend::Auto;
    config::VideoSettings video;
    /// SPEC.md §11, passed through to the pipeline. Disabled by default (CLAUDE.md hard
    /// rule 7); the session does not interpret it, it only carries it.
    config::SegmentationSettings segmentation;
    PipelineAudioSettings audio;

    /// Empty selects SPEC.md §5.2's policy. Set to pin an adapter, which is what the
    /// row 11 test uses to force a migration to a *known* destination.
    std::string encoder_override;

    /// SPEC.md §20.1's chaos-tier disk stall, passed through. Zero in production.
    std::int64_t injected_stall_ns = 0;
    int injected_stall_period = 20;

    /// Builds the capture backend. Empty selects `capture::create_capture`, which is
    /// what production does and what every real recording uses.
    ///
    /// **The seam exists because CLAUDE.md §5 forbids testing against the real
    /// desktop**: "Tests that depend on what happens to be on screen are not tests."
    /// A session test without this captures the live display, and on an idle desktop
    /// WGC composites nothing — so the recording ends up with a handful of frames and
    /// the §10.4 validation gate's duration check becomes a coin flip. Measured that
    /// way, `test_watchdog_recovery` passed alone and failed inside a full tier run.
    ///
    /// Called on the capture thread, once at `start` and once per rebuild, with the
    /// device the backend must create its resources on.
    std::function<Result<std::unique_ptr<capture::IScreenCapture>>(ID3D11Device*)> capture_factory;

    /// SPEC.md §15.2's preview ring, or null for no preview.
    ///
    /// **Borrowed, and it outlives the session on purpose.** The ring is the GUI's mapping;
    /// a section recreated per recording would leave every `QImage` in the GUI pointing at
    /// freed pages every time the user pressed stop. So `EngineService` owns it for as long
    /// as the preview is armed and hands sessions a pointer, and the session owns only the
    /// device-bound half -- the shader, the staging surfaces and the `fc-preview` thread,
    /// all of which a §5.4 rebuild legitimately destroys.
    preview::PreviewRing* preview = nullptr;

    /// Whether the preview stage should be running.
    ///
    /// Separate from `preview` being non-null so a session started while the GUI was not
    /// previewing can still be told to start one later (§15.1 makes `start_preview` and
    /// `start_record` independent commands, so that ordering is reachable). Default false,
    /// so a caller that forgets this line dispatches nothing.
    bool preview_enabled = false;

    preview::PreviewWriterSettings preview_settings;

    /// Run capture and the preview without opening an encoder, a muxer or a file.
    ///
    /// SPEC.md §15.1 lists `start_preview` and `start_record` as separate commands, and
    /// §16.2 puts a preview surface above the controls rather than inside them -- so a
    /// preview that only existed during a recording would be a preview you can only see
    /// once it is too late to frame the shot. This is that case: everything in the class
    /// works as usual except that `pipeline` is never created, and every use of it is
    /// already null-checked because a migration can legitimately leave it absent.
    ///
    /// `stop()` on such a session returns a report saying no file was written, rather than
    /// an error: nothing failed, there was simply nothing to validate.
    bool preview_only = false;
};

struct SessionStats {
    PipelineStats pipeline;
    /// Track 0 — the system mix. Tier A's output, whether or not Tier B is on.
    AudioStats audio;
    /// SPEC.md §8.6's per-application tracks. Empty unless Tier B is on.
    AppTracksStats app_tracks;
    PipelineHealth health;

    std::uint64_t frames_captured = 0;
    /// Frames the capture backend refused to hand over -- a timeout on an idle
    /// desktop is normal and is *not* counted as a fault.
    std::uint64_t capture_timeouts = 0;

    std::uint64_t rebuilds = 0;
    std::int64_t worst_rebuild_gap_ns = 0;
    /// Files written. More than one means a cross-adapter migration happened.
    std::uint64_t segments = 1;

    /// SPEC.md §15.2's preview. All zero when no preview is attached.
    preview::PreviewWriterStats preview;
};

/// Owns a recording end to end and keeps it alive across device and topology change.
///
/// Threading: `start` and `stop` are called from the owner's thread. Everything else
/// happens on the `capture` thread this class creates (SPEC.md §12) and on the
/// `watchdog` thread inside `VideoPipeline`. Accessors are snapshot-returning and
/// safe from any thread.
///
/// Lifetime: `start`, then the session runs itself until `stop`. Unlike
/// `VideoPipeline` it is *not* fed from outside -- it pulls from the capture backend,
/// which is what lets it pause and rebuild that backend without the caller noticing.
class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;
    RecordingSession(RecordingSession&&) = delete;
    RecordingSession& operator=(RecordingSession&&) = delete;

    /// Runs SPEC.md §5.1 discovery and §5.2 selection, opens the device, capture
    /// backend and pipeline, and starts the capture thread.
    [[nodiscard]] Result<void> start(const SessionSettings& settings);

    /// Stops capture, finalizes, validates. Idempotent. Returns the report for the
    /// **last** file written; earlier segments are in `migrations()`.
    ///
    /// A recording paused when this arrives finalizes normally and yields a valid file
    /// (SPEC.md §7.5's invariant).
    [[nodiscard]] Result<mux::ValidationReport> stop();

    /// Excises wall-clock time from the recording until `resume` (SPEC.md §7.5).
    ///
    /// Stops the capture thread submitting *first*, then pauses the pipeline -- the
    /// order matters, because the pipeline's pause drains the encode queue and a
    /// capture thread still pushing into it would never let that finish.
    ///
    /// **Idempotent.** Pausing a paused recording succeeds and changes nothing;
    /// `INTERNAL_INVALID_STATE` means there is no recording, which is a different
    /// answer and the only failure this has.
    [[nodiscard]] Result<void> pause();

    /// Resumes (SPEC.md §7.5). Idempotent in the same sense. Forces an IDR on the next
    /// frame so the seam carries no reference to pre-pause content.
    [[nodiscard]] Result<void> resume();

    [[nodiscard]] bool running() const noexcept;

    /// Starts or stops SPEC.md §15.2's preview stage mid-session.
    ///
    /// The change is *requested*, not performed: the preview writer's shader, staging
    /// surfaces and thread belong to the capture thread, and that thread is the only one
    /// allowed to build or tear them down -- the same ownership rule `rebuild_pending`
    /// follows and for the same reason. So this returns as soon as the request is posted,
    /// and the next captured frame acts on it.
    ///
    /// Does nothing when no ring was supplied at `start`, or when the ring is not open.
    void set_preview_enabled(bool enabled) noexcept;

    /// The file currently being written.
    ///
    /// Not the path handed to `start` when SPEC.md §11 segmentation is on -- the first
    /// file of a split recording is `<basename>_part001` -- nor after a cross-adapter
    /// migration has broken the recording into pieces. Callers that report a filename to
    /// a user must use this one.
    [[nodiscard]] std::filesystem::path current_output() const;

    /// True between a successful `pause` and the `resume` that closes it.
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] SessionStats stats() const;

    /// Every rebuild this session performed, in order.
    [[nodiscard]] std::vector<MigrationRecord> migrations() const;

    /// Test seam: injects a device error as though a D3D call had returned it.
    ///
    /// SPEC.md §20 row 11 offers "driver restart via `pnputil` or a test-only injected
    /// `DXGI_ERROR_DEVICE_REMOVED`", and this is the second. A `pnputil` restart takes
    /// the display down for seconds, cannot be aimed at one adapter, and would make the
    /// suite unrunnable on a developer's machine -- so the injected form is the one the
    /// spec expects to be used, and the real one is exercised by hand.
    ///
    /// **This does not fake the response.** It supplies the same HRESULT the driver
    /// would, at the same seam `DeviceWatcher::report_device_error` receives it from,
    /// and every subsequent step -- teardown, re-selection, rebuild, IDR, duplicate
    /// fill, resume -- is the production path.
    void inject_device_error(std::int32_t hr, std::int32_t removed_reason = 0);

    /// Test seam: forces the next rebuild to select `adapter`, so a migration can be
    /// aimed at a *different* adapter deliberately.
    ///
    /// Needed because a real `DEVICE_REMOVED` on a rig whose adapter promptly returns
    /// re-selects the same adapter, which exercises only the same-file path. Row 11
    /// has to cover both outcomes of the amended §5.4.
    void force_next_adapter(gpu::AdapterId adapter);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// The one capability `VideoPipeline` has to grow
// ---------------------------------------------------------------------------
//
// For the same-adapter case the muxer, the pacer, the shared epoch and the audio path
// must survive while the D3D device, the converter and the video encoder are replaced
// underneath them. All four are private to `VideoPipeline::Impl`, and they have to
// stay that way -- SPEC.md §10.1's single-writer discipline is exactly the property
// that would be lost by handing the muxer out.
//
// So the operation belongs on `VideoPipeline`, with this shape:
//
//     /// Replaces the D3D device, colour converter and video encoder while keeping
//     /// the muxer, the pacer, the shared epoch and the audio path (SPEC.md §5.4
//     /// steps 5-7).
//     ///
//     /// Refuses unless the new encoder's `extradata` is byte-identical to the
//     /// original's: the container's parameter sets were fixed by
//     /// `avformat_write_header` and a mismatch produces a file whose remainder does
//     /// not decode. A refusal is the caller's cue to close the segment instead.
//     ///
//     /// Emits duplicates covering `gap_ns` so the CFR timeline stays contiguous, and
//     /// advances the PTS grid by at least the encoder's reorder depth so the new
//     /// encoder's first DTS cannot precede the last one written.
//     [[nodiscard]] Result<void> rebuild_device(ID3D11Device* device,
//                                               const std::string& encoder_name,
//                                               std::uint32_t pool_bind_flags,
//                                               std::int64_t gap_ns);
//
// The extradata check is what makes the amended §5.4 safe rather than merely stated:
// the pipeline refuses to continue a file it cannot continue correctly, and the
// session's fallback is the segment split. Neither layer has to *remember* the rule.
//
// ---------------------------------------------------------------------------
// Test plan
// ---------------------------------------------------------------------------
// Named per the repo's convention -- the assertion, not the subject -- and mapped to
// SPEC.md §20 in docs/ACCEPTANCE.md.
//
// `test_gpu_migration.cpp` (gpu), row 11:
//
//   ADeviceLossOnTheSameAdapterKeepsOneContinuousFile
//       Inject `DXGI_ERROR_DEVICE_REMOVED` mid-recording, let re-selection land on the
//       same adapter. Assert: one output file; decoded frame count equals the CFR grid
//       for the whole duration with no gap beyond the migration's duplicates; PTS
//       strictly increasing across the seam; `MigrationRecord::same_file` true;
//       `gap_ns < kMigrationGapBudgetNs`.
//
//   AMigrationToTheOtherAdapterClosesTheSegmentCleanly
//       `force_next_adapter` to the other adapter, then inject. Assert: two files; both
//       independently decodable; the second starts on a keyframe; the boundary is
//       logged at WARN with both LUIDs; `same_file` false; the two durations sum to the
//       recording's length within one frame.
//
//   TheGapNeverExceedsTheBudgetAcrossRepeatedMigrations
//       Five injected losses in one recording. Assert every `gap_ns` under budget and
//       the timeline still contiguous -- the case that catches a rebuild leaking a
//       device, a pool or a thread per iteration.
//
//   ARebuiltEncoderNeverEmitsADtsBeforeTheLastOneWritten
//       Inject with the PTS-advance deliberately set to zero. Assert `rebuild_device`
//       refuses rather than producing the libavformat error. The negative control for
//       the constraint §5.4 omits.
//
// `test_watchdog_recovery.cpp` (gpu), row 9:
//
//   ~~AStalledCaptureIsRebuiltWithinTheRecoveryBudget~~
//       **Never written, and it could not have been written as stated** (BUG-045).
//       "Stall the capture backend past 3x the frame interval" is not a thing a test can
//       arrange on the primary backend: WGC is push-only, so the only way to make it stop
//       delivering is to stop changing the screen -- which is what an idle desktop does,
//       and is not a stall. The trigger it would have covered was firing on exactly that.
//
//       Replaced by the pair below, which differ in one bit: what the backend reports
//       about itself once it goes quiet.
//
//   ABackendThatIsQuietButHealthyIsLeftAlone
//       A backend that stops producing while `running()` stays true is an idle desktop.
//       Assert `rebuilds == 0` -- and assert the silence really happened, or the case is
//       measuring nothing.
//
//   ABackendThatHasDiedIsRebuilt
//       The same backend reporting `running() == false`. Assert a rebuild fires with
//       `cause == CaptureStalled` and recovery under row 9's 500 ms. **Not optional:**
//       without it, deleting the trigger outright would satisfy the case above.
//
//   AHealthyRecordingIsNeverRebuilt
//       The negative control for ordinary jitter. A clean recording must show
//       `rebuilds == 0`.
//
//   AStaticScreenIsNotMistakenForAWedgedCapture
//       The live end-to-end form, against real WGC behind a `ScreenAnimator` at `fps = 0`.
//       Does *not* discriminate on the reference rig -- measured, WGC delivers 301 frames
//       in 15 s behind a provably static window -- and is kept as a canary for quieter
//       machines.
//
// `test_health_monitor.cpp` (cpu), rung 4, already present:
//   `ThreeDeviceFailuresInsideSixtySecondsRequestAGpuMigration` covers the decision;
//   the session wiring adds `TheLadderCanTriggerARebuild` to the gpu tier.

} // namespace fc::pipeline
