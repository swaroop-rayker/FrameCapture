#include "core/pipeline/recording_session.h"

#include "core/capture/capture_factory.h"
#include "core/gpu/cross_adapter_probe.h"
#include "core/gpu/d3d_device.h"
#include "core/logging/logger.h"
#include "core/mux/segment_planner.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"
#include "core/util/worker.h"

#include <windows.h>
// Must follow windows.h.
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fc::pipeline {
namespace {

using Microsoft::WRL::ComPtr;

/// How often the session's watchdog looks for a reason to rebuild (SPEC.md §5.4's
/// "background poll at 2 Hz").
constexpr auto kWatchInterval = std::chrono::milliseconds{500};

/// How long `acquire` waits before reporting nothing. Short enough that a stop is
/// responsive, long enough that an idle desktop does not spin.
constexpr auto kAcquireTimeout = std::chrono::milliseconds{100};

/// Finds the DXGI adapter object for a LUID. The session re-enumerates on every
/// rebuild rather than caching handles, because the whole premise of §5.4 is that the
/// adapter set can change underneath it.
[[nodiscard]] ComPtr<IDXGIAdapter1> adapter_handle(const gpu::AdapterId& id) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        const auto luid = (static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                          static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart));
        if (luid == id.value) {
            return adapter;
        }
    }
    return nullptr;
}

} // namespace

std::string_view to_string(RebuildCause value) noexcept {
    switch (value) {
    case RebuildCause::DeviceLost:
        return "device_lost";
    case RebuildCause::TopologyChanged:
        return "topology_changed";
    case RebuildCause::LadderRequested:
        return "ladder_requested";
    case RebuildCause::CaptureStalled:
        return "capture_stalled";
    case RebuildCause::AccessLost:
        return "access_lost";
    }
    return "unknown";
}

struct RecordingSession::Impl {
    SessionSettings settings;

    gpu::GpuTopologyService topology;
    gpu::DeviceWatcher watcher;

    std::unique_ptr<gpu::D3dDevice> device;
    std::unique_ptr<capture::IScreenCapture> capture;
    std::unique_ptr<VideoPipeline> pipeline;

    /// SPEC.md §15.2's preview stage. Device-bound, so it is destroyed and rebuilt with
    /// the device (§5.4); the ring it writes into is the caller's and survives.
    ///
    /// **Built and destroyed by the capture thread only**, which is why the pointer needs a
    /// mutex and the capture thread's own copy below does not: `offer` is called from the
    /// same thread that changes it, so the hot path takes no lock at all.
    std::unique_ptr<preview::PreviewWriter> preview;
    mutable std::mutex preview_mutex;
    /// The capture thread's copy of `preview.get()`. Capture thread only.
    preview::PreviewWriter* preview_active = nullptr;
    /// Counters from writers that have been retired -- by a §5.4 rebuild, by a
    /// `stop_preview`, or by the stop -- so `stats()` reports the session's whole preview
    /// history rather than the current writer's slice of it. Guarded by `preview_mutex`.
    preview::PreviewWriterStats retired_preview;

    /// The report from the most recently finalized file, kept so `stop()` can still say
    /// a recording exists after the pipeline that wrote it has been released (BUG-057).
    ///
    /// A segment rollover finalizes and validates the current file and then lets its
    /// pipeline go. Until this existed that report was computed, found good, and dropped
    /// — so if the *next* segment then failed to open, `stop()` had no pipeline to ask
    /// and answered `INTERNAL_INVALID_STATE`. The file was on disk, complete and valid,
    /// and the engine reported it as a failed recording. CLAUDE.md §1 is not only about
    /// writing the file; a file the caller is told does not exist is a file the user has
    /// lost.
    ///
    /// Capture thread writes it, `stop()` reads it after that thread has been joined.
    std::optional<mux::ValidationReport> last_report;

    /// The encoder the current pipeline was opened with. A rebuild must reproduce it
    /// exactly for the same-adapter path to keep one file.
    gpu::AdapterId encode_adapter;
    std::string encoder_name;
    std::uint32_t pool_bind_flags = 0;

    std::filesystem::path current_output;
    int segment_index = 1;

    std::thread capture_thread;
    std::thread watch_thread;
    std::promise<void> capture_done;
    std::promise<void> watch_done;
    std::future<void> capture_finished;
    std::future<void> watch_finished;

    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};

    /// Set by `set_preview_enabled`, consumed by the capture thread. Here with the other
    /// one-byte atomics rather than beside the preview members it belongs to, because
    /// `clang-analyzer-optin.performance.Padding` counts and a `bool` between two
    /// pointer-sized members costs seven bytes every time.
    std::atomic<bool> preview_wanted{false};

    /// SPEC.md §7.5. Read by the capture thread on every frame; written by whichever
    /// thread handles `pause_record`. Separate from `VideoPipeline`'s clock rather than
    /// derived from it because they are answers to different questions -- this one is
    /// "should capture submit", and it is set *before* the pipeline's pause so that no
    /// new frame enters the queue the pipeline is about to drain.
    std::atomic<bool> paused{false};

    /// Set by the watchdog, consumed by the capture thread. The capture thread owns
    /// the backend, so it is the only thread that may tear it down -- SPEC.md §5.4
    /// step 1's "signal PAUSE_CAPTURE" is this flag and nothing more.
    std::atomic<bool> rebuild_pending{false};
    std::atomic<RebuildCause> rebuild_cause{RebuildCause::DeviceLost};

    /// True for the *duration* of a rebuild, distinct from `rebuild_pending`.
    ///
    /// `rebuild_pending` is cleared the moment the capture thread picks the request up,
    /// which leaves the ~240 ms the rebuild actually takes looking idle to the
    /// watchdog. It polls during that window, sees the DXGI churn the rebuild itself is
    /// causing, and asks for another one. Measured: one injected fault produced four
    /// rebuilds even with `DeviceWatcher::acknowledge` in place, because acknowledging
    /// at the *end* cannot help a poll that happened in the middle.
    std::atomic<bool> rebuilding{false};

    /// Test seam (`force_next_adapter`). Zero means "run SPEC.md §5.2's policy".
    std::atomic<std::int64_t> forced_adapter{0};

    std::mutex watch_mutex;
    std::condition_variable watch_wake;
    bool watch_exit = false;

    mutable std::mutex records_mutex;
    std::vector<MigrationRecord> records;

    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> capture_timeouts{0};
    std::atomic<std::int64_t> worst_gap_ns{0};

    /// The capture backend's own liveness, published by the thread that owns it.
    ///
    /// `IScreenCapture::running()` goes false when a backend's worker leaves its loop --
    /// WGC when its session throws or is closed, DDA when `AcquireNextFrame` fails hard --
    /// so this is a fault the backend *names*, as distinct from it merely having nothing to
    /// report. It is published rather than read directly because `capture` is a
    /// `unique_ptr` the capture thread creates and destroys, and the watchdog may not touch
    /// it (SPEC.md §5.4 step 1's ownership rule).
    ///
    /// Starts false and is only believed once `capture_observed` is set, so the window
    /// between `start` and the capture thread's first iteration is not a fault.
    std::atomic<bool> capture_alive{false};
    std::atomic<bool> capture_observed{false};

    /// True when the backend has said it is no longer running. Watchdog thread.
    [[nodiscard]] bool capture_backend_died() const noexcept {
        return capture_observed.load(std::memory_order_acquire) && !capture_alive.load(std::memory_order_acquire);
    }

    bool handled_migration_request = false;

    void capture_loop();
    void watch_loop();

    /// SPEC.md §5.4 steps 1-8. Capture thread only.
    void rebuild(RebuildCause cause);

    /// Picks the encode adapter. Honours `forced_adapter` when a test set one.
    [[nodiscard]] Result<gpu::EncoderSelection> select();

    [[nodiscard]] Result<void> open_pipeline(const std::filesystem::path& output);
    [[nodiscard]] Result<void> open_capture();

    /// Starts the preview stage on the current device, if one was asked for.
    ///
    /// **Best-effort by construction.** A preview that will not start is logged and the
    /// recording proceeds without it -- ERROR_CODES.md says exactly this about 7008 and
    /// 7009 ("degrade the preview, never the recording"), and CLAUDE.md §1 would say it
    /// even if the table did not.
    void open_preview();

    /// Stops the preview stage and folds its counters into `retired_preview`.
    void close_preview();

    void note(MigrationRecord record) {
        const std::lock_guard lock(records_mutex);
        records.push_back(std::move(record));
    }
};

Result<gpu::EncoderSelection> RecordingSession::Impl::select() {
    if (const std::int64_t forced = forced_adapter.load(std::memory_order_acquire); forced != 0) {
        // Test seam. A real `DEVICE_REMOVED` on a rig whose adapter promptly returns
        // re-selects the same adapter every time, which exercises only the same-file
        // path -- row 11 has to cover both outcomes of the amended §5.4.
        for (const gpu::AdapterInfo& adapter : topology.topology().adapters) {
            if (adapter.id.value == forced && adapter.can_encode()) {
                gpu::EncoderSelection selection;
                selection.adapter = adapter.id;
                selection.rule = gpu::SelectionRule::DiscreteWithTransfer;
                selection.rationale = "forced by the caller";
                return selection;
            }
        }
        return FcError::GPU_NO_SUITABLE_ADAPTER;
    }
    return topology.select_for_monitor(settings.target.monitor);
}

Result<void> RecordingSession::Impl::open_pipeline(const std::filesystem::path& output) {
    PipelineSettings pipeline_settings;
    pipeline_settings.output = output;
    pipeline_settings.video = settings.video;
    pipeline_settings.segmentation = settings.segmentation;
    pipeline_settings.audio = settings.audio;
    pipeline_settings.encoder_name = encoder_name;
    pipeline_settings.pool_bind_flags = pool_bind_flags;
    pipeline_settings.injected_stall_ns = settings.injected_stall_ns;
    pipeline_settings.injected_stall_period = settings.injected_stall_period;
    pipeline_settings.on_finalize_progress = settings.on_finalize_progress;

    // Built into a local and installed only once it has started (BUG-057).
    //
    // Assigning the member first and starting it afterwards leaves a *half-constructed
    // pipeline installed as the live one* when the start fails: non-null, so every
    // `if (pipeline)` in this file reads it as working, and unstarted, so
    // `VideoPipeline::stop()` refuses it with `INTERNAL_INVALID_STATE`. That is how a
    // recording that had already been finalized came to be reported as a failure -- the
    // caller was told the session was in a bad state by an object that had never run.
    auto opening = std::make_unique<VideoPipeline>();
    FC_TRY(opening->start(device->device(), pipeline_settings));

    pipeline = std::move(opening);
    // From the pipeline, not from `output`: with SPEC.md §11 segmentation on, the file it
    // actually opened is `<basename>_part001`, and reporting the base would name a file
    // that does not exist (BUG-043's lesson).
    current_output = pipeline->current_output();
    return ok();
}

Result<void> RecordingSession::Impl::open_capture() {
    if (settings.capture_factory) {
        FC_TRY_ASSIGN(capture, settings.capture_factory(device->device()));
    } else {
        FC_TRY_ASSIGN(capture, capture::create_capture(settings.backend, settings.target));
        FC_TRY(capture->start(device->device(), settings.target));
        return ok();
    }
    // An injected backend is handed the device and starts itself, because a test source
    // has its own notion of what "start" needs and no capture target at all.
    return ok();
}

namespace {

/// Adds one writer's counters to a running total. Free function so the fold is written
/// once -- `stats()` and `close_preview` do the same arithmetic and a second copy would
/// eventually count a different set of fields.
void accumulate(preview::PreviewWriterStats& total, const preview::PreviewWriterStats& one) {
    total.offered += one.offered;
    total.rate_limited += one.rate_limited;
    total.dispatched += one.dispatched;
    total.published += one.published;
    total.dropped_no_slot += one.dropped_no_slot;
    total.dropped_stale += one.dropped_stale;
    total.dropped_readback_busy += one.dropped_readback_busy;
    total.failures += one.failures;
    total.total_offer_ns += one.total_offer_ns;
    total.offer_samples += one.offer_samples;
    total.worst_offer_ns = std::max(total.worst_offer_ns, one.worst_offer_ns);
}

} // namespace

void RecordingSession::Impl::open_preview() {
    if (preview_active != nullptr || settings.preview == nullptr || !settings.preview->open() || !device) {
        return;
    }
    auto writer = std::make_unique<preview::PreviewWriter>();
    if (const Result<void> started =
            writer->start(device->device(), device->context(), settings.preview, settings.preview_settings);
        !started.has_value()) {
        FC_LOG_WARN(Subsystem::Ipc, "the preview stage could not start; the recording continues without it",
                    LogFields{}.add_error(started.error()));
        return;
    }
    const std::lock_guard lock(preview_mutex);
    preview = std::move(writer);
    preview_active = preview.get();
}

void RecordingSession::Impl::close_preview() {
    std::unique_ptr<preview::PreviewWriter> dying;
    preview_active = nullptr;
    {
        const std::lock_guard lock(preview_mutex);
        dying = std::move(preview);
    }
    if (!dying) {
        return;
    }
    // Stopped outside the lock. `stop` joins `fc-preview` under SPEC.md §12's bounded
    // deadline, and holding the lock across it would make `get_stats` wait out the same
    // deadline for a thread that has nothing to do with it.
    dying->stop();
    const preview::PreviewWriterStats final_stats = dying->stats();
    const std::lock_guard lock(preview_mutex);
    accumulate(retired_preview, final_stats);
}

void RecordingSession::Impl::rebuild(RebuildCause cause) {
    const std::int64_t gap_started = timing::qpc_now_ns();
    MigrationRecord record;
    record.cause = cause;
    record.from = encode_adapter;

    FC_LOG_WARN(Subsystem::Gpu, "rebuilding the capture and device stack",
                LogFields{}.add("cause", to_string(cause)).add("from", encode_adapter.to_string()));

    // Step 1 and 3: stop feeding, tear the capture session down. The encoder queue
    // drains on its own inside `rebuild_device`.
    //
    // The liveness observation goes with it. `capture->stop()` sets the backend's
    // `running` false by design, and a watchdog that read that as a fault would ask for
    // another rebuild the moment this one finished -- which is BUG-035's shape arriving
    // through a different door. The next capture thread iteration re-establishes it.
    capture_observed.store(false, std::memory_order_release);
    capture_alive.store(false, std::memory_order_release);
    if (capture) {
        capture->stop();
        capture.reset();
    }
    // The preview's shader, staging surfaces and thread belong to the device that is about
    // to be replaced. Torn down here, before it is, for the reason `StagingRing::reset`
    // gives: a texture outliving its device is a use-after-free the driver reports as a
    // hang rather than as anything legible.
    close_preview();
    std::int64_t mark = timing::qpc_now_ns();
    record.teardown_ns = mark - gap_started;

    // Step 4: re-run discovery and selection. Discovery first, because the adapter set
    // is exactly what may have changed.
    if (const Result<void> refreshed = topology.refresh(); !refreshed.has_value()) {
        FC_LOG_ERROR(Subsystem::Gpu, "discovery failed during a rebuild; the recording stops",
                     LogFields{}.add_error(refreshed.error()));
        record.failed = true;
        record.gap_ns = timing::qpc_now_ns() - gap_started;
        note(std::move(record));
        stop_requested.store(true, std::memory_order_release);
        return;
    }

    const Result<gpu::EncoderSelection> selection = select();
    if (!selection.has_value() || !selection.value().adapter.has_value()) {
        FC_LOG_ERROR(
            Subsystem::Gpu, "no encode adapter after a rebuild; the recording stops",
            LogFields{}.add_error(selection.has_value() ? FcError::GPU_NO_SUITABLE_ADAPTER : selection.error()));
        record.failed = true;
        record.gap_ns = timing::qpc_now_ns() - gap_started;
        note(std::move(record));
        stop_requested.store(true, std::memory_order_release);
        return;
    }

    record.discovery_ns = timing::qpc_now_ns() - mark;
    mark = timing::qpc_now_ns();

    const gpu::AdapterId target = *selection.value().adapter;
    const gpu::AdapterInfo* info = nullptr;
    for (const gpu::AdapterInfo& adapter : topology.topology().adapters) {
        if (adapter.id == target) {
            info = &adapter;
            break;
        }
    }
    if (info == nullptr) {
        record.failed = true;
        record.gap_ns = timing::qpc_now_ns() - gap_started;
        note(std::move(record));
        stop_requested.store(true, std::memory_order_release);
        return;
    }
    record.to = target;

    // Step 5: a new device on the selected adapter.
    const ComPtr<IDXGIAdapter1> handle = adapter_handle(target);
    Result<gpu::D3dDevice> created = handle.Get() != nullptr ? gpu::create_device_on_adapter(handle.Get())
                                                             : Result<gpu::D3dDevice>{FcError::GPU_NO_SUITABLE_ADAPTER};
    if (!created.has_value()) {
        FC_LOG_ERROR(Subsystem::Gpu, "device creation failed during a rebuild; the recording stops",
                     LogFields{}.add_error(created.error()));
        record.failed = true;
        record.gap_ns = timing::qpc_now_ns() - gap_started;
        note(std::move(record));
        stop_requested.store(true, std::memory_order_release);
        return;
    }
    record.device_ns = timing::qpc_now_ns() - mark;
    mark = timing::qpc_now_ns();

    device = std::make_unique<gpu::D3dDevice>(std::move(created).value());
    encode_adapter = target;
    encoder_name = info->encode.encoder_name;
    pool_bind_flags = info->encode.nv12_pool_bind_flags;

    // Steps 6 and 7. `rebuild_device` refuses when the parameter sets differ, which is
    // the amended §5.4's cross-adapter case -- so a refusal is not a failure, it is the
    // instruction to close the segment and open the next.
    //
    // A preview-only session (§15.2, no encoder and no file) has nothing to reconcile:
    // there are no parameter sets, no container and no segment, so the rebuild is the new
    // device and the new capture and nothing else. Reported as `same_file` because the
    // alternative -- "the file was split" -- would be a claim about a file that does not
    // exist.
    const Result<void> rebuilt =
        pipeline ? pipeline->rebuild_device(device->device(), encoder_name, pool_bind_flags) : Result<void>{ok()};
    if (rebuilt.has_value()) {
        record.same_file = true;
        record.output = current_output;
    } else {
        FC_LOG_WARN(Subsystem::Mux,
                    "the rebuilt encoder's parameter sets differ; closing this file and opening the next",
                    LogFields{}
                        .add("from", record.from.to_string())
                        .add("to", target.to_string())
                        .add("reason", error_name(rebuilt.error())));

        // The prime directive: the file written so far is finalized and validated
        // before anything else happens. A migration must never cost the recording.
        //
        // **The report is kept** (BUG-057). It used to be examined for failure and
        // otherwise discarded, which left `stop()` with nothing to return if the next
        // segment then failed to open -- a finalized, valid recording reported as an
        // internal error.
        if (const Result<mux::ValidationReport> report = pipeline->stop(); report.has_value()) {
            last_report = report.value();
        } else {
            FC_LOG_ERROR(Subsystem::Mux, "finalizing the previous segment failed",
                         LogFields{}.add_error(report.error()));
        }
        pipeline.reset();

        ++segment_index;
        const std::filesystem::path next = mux::segment_path(settings.output, segment_index);
        if (const Result<void> opened = open_pipeline(next); !opened.has_value()) {
            // The recording stops here, and that is a legitimate outcome -- the adapter
            // this session was encoding on is gone and the replacement will not open.
            // What must not also happen is the *previous* segment being disowned: it was
            // finalized and validated three lines up, and `last_report` is what lets
            // `stop()` hand it to the caller instead of an error code.
            FC_LOG_ERROR(Subsystem::Mux, "opening the next segment failed; the recording stops",
                         LogFields{}.add_error(opened.error()));
            record.failed = true;
            record.gap_ns = timing::qpc_now_ns() - gap_started;
            note(std::move(record));
            stop_requested.store(true, std::memory_order_release);
            return;
        }
        record.same_file = false;
        record.output = next;
    }

    record.pipeline_ns = timing::qpc_now_ns() - mark;
    mark = timing::qpc_now_ns();

    // Before capture, so the first frame after the rebuild has somewhere to preview to.
    // Not counted against §5.4's 350 ms budget separately -- it is inside `capture_ns`,
    // and if it ever grows enough to matter that is the number that will say so.
    if (preview_wanted.load(std::memory_order_acquire)) {
        open_preview();
    }

    // Step 8: capture comes back last, so no frame can arrive before the encoder that
    // has to accept it exists.
    if (const Result<void> opened = open_capture(); !opened.has_value()) {
        FC_LOG_ERROR(Subsystem::Capture, "capture could not be restarted after a rebuild; the recording stops",
                     LogFields{}.add_error(opened.error()));
        record.failed = true;
        record.gap_ns = timing::qpc_now_ns() - gap_started;
        note(std::move(record));
        stop_requested.store(true, std::memory_order_release);
        return;
    }

    record.capture_ns = timing::qpc_now_ns() - mark;

    // The device stack that exists now is the one to consider current. Without this the
    // rebuild's own DXGI activity reads as a fresh topology change on the next poll and
    // the recording rebuilds in a loop -- measured at four rebuilds for one fault.
    watcher.acknowledge();

    record.gap_ns = timing::qpc_now_ns() - gap_started;
    std::int64_t previous = worst_gap_ns.load(std::memory_order_relaxed);
    while (record.gap_ns > previous &&
           !worst_gap_ns.compare_exchange_weak(previous, record.gap_ns, std::memory_order_relaxed)) {
    }

    FC_LOG_WARN(Subsystem::Gpu, "GPU_MIGRATION",
                LogFields{}
                    .add("cause", to_string(cause))
                    .add("from", record.from.to_string())
                    .add("to", record.to.to_string())
                    .add("gap_ms", record.gap_ns / 1'000'000)
                    .add("teardown_ms", record.teardown_ns / 1'000'000)
                    .add("discovery_ms", record.discovery_ns / 1'000'000)
                    .add("device_ms", record.device_ns / 1'000'000)
                    .add("pipeline_ms", record.pipeline_ns / 1'000'000)
                    .add("capture_ms", record.capture_ns / 1'000'000)
                    .add("budget_ms", gpu::kMigrationGapBudgetNs / 1'000'000)
                    .add("same_file", record.same_file)
                    .add("output", record.output.string()));
    note(std::move(record));
}

void RecordingSession::Impl::capture_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(capture_done);
    set_thread_name("fc-capture");
    try {
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (rebuild_pending.exchange(false, std::memory_order_acq_rel)) {
                rebuilding.store(true, std::memory_order_release);
                rebuild(rebuild_cause.load(std::memory_order_acquire));
                // Cleared only after `rebuild` has acknowledged the watcher, so no poll
                // can observe the window in which the rebuild's own DXGI activity is
                // still visible as a change.
                rebuilding.store(false, std::memory_order_release);
                continue;
            }

            // SPEC.md §15.2's preview, armed and disarmed by `set_preview_enabled`. Acted
            // on here rather than where it is requested for the same reason
            // `rebuild_pending` is: this thread owns the capture-side resources, and it is
            // the only one that may build or destroy them.
            if (const bool wanted = preview_wanted.load(std::memory_order_acquire);
                wanted != (preview_active != nullptr)) {
                if (wanted) {
                    open_preview();
                } else {
                    close_preview();
                }
            }

            if (!capture) {
                break;
            }

            // The backend's own verdict on itself, published for the watchdog (SPEC.md §20
            // row 9). Read here rather than there because this thread owns `capture`, and
            // read on *every* iteration rather than only when a frame arrives -- the whole
            // point is that it stays meaningful when frames do not.
            capture_alive.store(capture->running(), std::memory_order_release);
            capture_observed.store(true, std::memory_order_release);

            const Result<capture::CaptureFrame> frame = capture->acquire(kAcquireTimeout);
            if (!frame.has_value()) {
                capture_timeouts.fetch_add(1, std::memory_order_relaxed);
                // A timeout is the *normal* return on an idle desktop and must not be
                // mistaken for device loss (`DeviceWatcher.OrdinaryFailuresAreNotDeviceLoss`).
                // Anything that genuinely is device loss reaches the watcher, whose
                // next poll turns it into a rebuild.
                continue;
            }

            frames_captured.fetch_add(1, std::memory_order_relaxed);

            // SPEC.md §7.5's capture row: "Keep the session open, stop submitting
            // frames." The backend is deliberately still acquiring and releasing --
            // rebuilding the capture session per pause would make a hotkey cost §5.4's
            // 350 ms budget, and would put a DDA duplication at risk of not reopening
            // at all. So the frame is acquired, counted and thrown away.
            //
            // The pacer would excise it anyway (`PauseClock::map` refuses a timestamp
            // inside the span), and this check being here as well is not redundant: it
            // keeps paused frames out of the bounded queue entirely, so a long pause
            // cannot leave the queue full of material that will only be discarded --
            // which would look like queue pressure to SPEC.md §13's ladder.
            if (pipeline && !paused.load(std::memory_order_acquire)) {
                static_cast<void>(pipeline->submit(frame.value()));
            }

            // SPEC.md §15.2, and the ordering is the point: the recording gets the frame
            // first, every time. By the time `offer` runs, `submit` has already copied the
            // texture into the pipeline's own staging ring and queued it, so nothing the
            // preview does -- however slow, however broken -- can arrive before the frame
            // it was supposed to be recording.
            //
            // Offered while **paused**, deliberately. §7.5 keeps the capture session open
            // and only stops submitting; a preview that froze at the pause would tell the
            // user the screen had stopped changing, which is a lie the status panel is
            // explicitly forbidden to tell (§16.5) and the surface should not tell either.
            if (preview_active != nullptr) {
                preview_active->offer(frame.value().texture, static_cast<std::int64_t>(frame.value().qpc_ns));
            }
            capture->release(frame.value());
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Capture, "capture thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

void RecordingSession::Impl::watch_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(watch_done);
    set_thread_name("fc-session-watch");
    try {
        for (;;) {
            {
                std::unique_lock lock(watch_mutex);
                if (watch_wake.wait_for(lock, kWatchInterval, [this] { return watch_exit; })) {
                    break;
                }
            }

            // Requested, in progress, or shutting down. All three must be checked: a
            // rebuild takes ~240 ms during which `rebuild_pending` is already false,
            // and polling in that window is what turned one fault into four rebuilds.
            //
            // The poll is skipped entirely rather than taken and discarded, because
            // `poll` re-baselines the adapter set as a side effect -- consuming a
            // report here would hide the change from the acknowledgement that follows.
            if (rebuild_pending.load(std::memory_order_acquire) || rebuilding.load(std::memory_order_acquire) ||
                stop_requested.load(std::memory_order_acquire)) {
                continue;
            }

            const gpu::DeviceChangeReport change = watcher.poll();
            const PipelineHealth health = pipeline ? pipeline->health() : PipelineHealth{};

            // SPEC.md §13 rungs 6 and 7. The pipeline reports; this is the consumer it
            // has been waiting for since M6.
            if (health.stop_requested) {
                FC_LOG_WARN(Subsystem::Health, "the degradation ladder asked for the recording to stop",
                            LogFields{}.add("rung", static_cast<std::int64_t>(health.rung)));
                stop_requested.store(true, std::memory_order_release);
                continue;
            }

            std::optional<RebuildCause> cause;
            if (change.change == gpu::DeviceChange::DeviceRemoved || change.change == gpu::DeviceChange::DeviceReset) {
                cause = RebuildCause::DeviceLost;
            } else if (change.change == gpu::DeviceChange::AccessLost) {
                cause = RebuildCause::AccessLost;
            } else if (change.change == gpu::DeviceChange::TopologyChanged) {
                cause = RebuildCause::TopologyChanged;
            } else if (health.gpu_migration_requested && !handled_migration_request) {
                // SPEC.md §13 rung 4, acted on at last. Latched so one burst of encoder
                // failures provokes one migration rather than one per tick.
                handled_migration_request = true;
                cause = RebuildCause::LadderRequested;
            } else if (capture_backend_died()) {
                // SPEC.md §20 row 9, keyed on a fault the backend *names* rather than on
                // how long it has been quiet.
                //
                // ---------------------------------------------------------------------
                // Why silence is not the signal, measured twice
                // ---------------------------------------------------------------------
                // Row 9's mitigation is "no frame in 3x frame_interval -> rebuild", and it
                // reads as a timeout because it assumes a *pull* backend, where being
                // asked and having nothing to give is itself information. One of this
                // engine's two backends is not that:
                //
                //   DDA  polls `AcquireNextFrame` and emits a duplicate on every
                //        `WAIT_TIMEOUT` (§4.3), so it never goes quiet at all. Row 9's
                //        trigger can never fire on it.
                //   WGC  is push-only. `FrameArrived` is a compositor callback and the
                //        worker thread does nothing but stay alive to own the WinRT
                //        objects. When the desktop does not change, WGC says **nothing**
                //        -- and "nothing" is exactly what a dead session says too.
                //
                // So on the primary backend, silence carries no information whatever, and
                // any threshold on it is a guess about how bored the user is. The guess
                // was 50 ms, then 2 s; the field measured **11.1 s** of legitimate silence
                // on an idle machine, and each false positive tore down the capture and
                // device stack and cost `gap_ms=243`. There is no fifth attempt that
                // works: an unattended screen can be still for hours.
                //
                // What both backends *do* say is whether their worker is alive -- each
                // sets `running` false as it leaves its loop, for any reason. That is a
                // fault with a name, it cannot be produced by an idle desktop, and it is
                // what this now keys on. `capture_alive` is the capture thread's
                // publication of it, because the backend belongs to that thread.
                //
                // **What this gives up, stated plainly:** a WGC session that stops
                // delivering while still reporting itself alive is no longer detected. No
                // such failure has been observed, and inventing a detector for a fault
                // that cannot be named or reproduced is how the 2 s threshold came to
                // exist. See docs/ACCEPTANCE.md, which records it as an open item rather
                // than as covered.
                //
                // The row 9 *detector* is untouched: `stall_episodes` and `worst_stall_ns`
                // are still computed and still surfaced to the GUI, because "your screen
                // has not changed in eleven seconds" is worth saying. It is just not worth
                // acting on.
                cause = RebuildCause::CaptureStalled;
            }

            if (cause.has_value()) {
                rebuild_cause.store(*cause, std::memory_order_release);
                rebuild_pending.store(true, std::memory_order_release);
            }
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Gpu, "session watchdog terminated by an exception; rebuilds are no longer detected",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

RecordingSession::RecordingSession() : impl_(std::make_unique<Impl>()) {}

RecordingSession::~RecordingSession() {
    if (impl_ && impl_->running.load(std::memory_order_acquire)) {
        // Same reasoning as `VideoPipeline`'s destructor: the file is worth more than
        // a tidy teardown, and `stop` is what finalizes it.
        try {
            static_cast<void>(stop());
        } catch (const std::exception& e) {
            FC_LOG_ERROR(Subsystem::Mux, "session destructor could not finalize the recording",
                         LogFields{}.add("what", e.what()).add_error(FcError::MUX_FINALIZE_FAILED));
        }
    }
}

Result<void> RecordingSession::start(const SessionSettings& settings) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    impl_->settings = settings;
    impl_->segment_index = 1;

    FC_TRY(impl_->topology.refresh());
    FC_TRY(impl_->watcher.start());

    const Result<gpu::EncoderSelection> selection = impl_->select();
    if (!selection.has_value()) {
        return selection.error();
    }
    if (!selection.value().adapter.has_value()) {
        // SPEC.md §5.2 rule 4 -- software. The session does not implement it: rung 5's
        // encoder exists, but choosing it here would need a device-less capture path
        // that does not, and inventing one silently is worse than refusing.
        return FcError::ENCODE_NO_HARDWARE_ENCODER;
    }

    const gpu::AdapterId target = *selection.value().adapter;
    const gpu::AdapterInfo* info = nullptr;
    for (const gpu::AdapterInfo& adapter : impl_->topology.topology().adapters) {
        if (adapter.id == target) {
            info = &adapter;
            break;
        }
    }
    if (info == nullptr) {
        return FcError::GPU_NO_SUITABLE_ADAPTER;
    }

    const ComPtr<IDXGIAdapter1> handle = adapter_handle(target);
    if (handle.Get() == nullptr) {
        return FcError::GPU_NO_SUITABLE_ADAPTER;
    }
    FC_TRY_ASSIGN(gpu::D3dDevice created, gpu::create_device_on_adapter(handle.Get()));
    impl_->device = std::make_unique<gpu::D3dDevice>(std::move(created));

    impl_->encode_adapter = target;
    impl_->encoder_name = info->encode.encoder_name;
    impl_->pool_bind_flags = info->encode.nv12_pool_bind_flags;

    if (!settings.preview_only) {
        FC_TRY(impl_->open_pipeline(settings.output));
    }
    // Before the capture thread exists, so the first frame already has somewhere to
    // preview to; after that the capture thread owns this and `set_preview_enabled` is the
    // only way in.
    impl_->preview_wanted.store(settings.preview_enabled, std::memory_order_release);
    if (settings.preview_enabled) {
        impl_->open_preview();
    }
    FC_TRY(impl_->open_capture());

    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->rebuild_pending.store(false, std::memory_order_release);
    impl_->capture_observed.store(false, std::memory_order_release);
    impl_->capture_alive.store(false, std::memory_order_release);
    impl_->handled_migration_request = false;
    {
        const std::lock_guard lock(impl_->watch_mutex);
        impl_->watch_exit = false;
    }
    impl_->running.store(true, std::memory_order_release);

    impl_->capture_done = std::promise<void>{};
    impl_->watch_done = std::promise<void>{};
    impl_->capture_finished = impl_->capture_done.get_future();
    impl_->watch_finished = impl_->watch_done.get_future();
    impl_->capture_thread = std::thread([impl = impl_.get()] { impl->capture_loop(); });
    impl_->watch_thread = std::thread([impl = impl_.get()] { impl->watch_loop(); });

    FC_LOG_INFO(Subsystem::App, settings.preview_only ? "preview session started" : "recording session started",
                LogFields{}
                    .add("output", settings.output.string())
                    .add("adapter", target.to_string())
                    .add("encoder", impl_->encoder_name)
                    .add("preview", impl_->preview != nullptr)
                    .add("rule", gpu::to_string(selection.value().rule)));
    return ok();
}

Result<mux::ValidationReport> RecordingSession::stop() {
    if (!impl_->running.exchange(false)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->stop_requested.store(true, std::memory_order_release);
    {
        const std::lock_guard lock(impl_->watch_mutex);
        impl_->watch_exit = true;
    }
    impl_->watch_wake.notify_all();
    static_cast<void>(await_worker(impl_->watch_finished, impl_->watch_thread, "fc-session-watch", Subsystem::Gpu));

    // The capture thread may be mid-rebuild, which is legitimately slow -- it opens a
    // device and an encoder. The finalization deadline applies for BUG-026's reason.
    static_cast<void>(await_worker(impl_->capture_finished, impl_->capture_thread, "fc-capture", Subsystem::Capture,
                                   std::chrono::duration_cast<std::chrono::milliseconds>(kFinalizeJoinTimeout)));

    if (impl_->capture) {
        impl_->capture->stop();
        impl_->capture.reset();
    }

    // After the capture thread has been joined, because that thread is the one calling
    // `offer`. Stopping the writer first would race a live caller against a torn-down one.
    impl_->close_preview();

    if (!impl_->pipeline) {
        if (impl_->settings.preview_only) {
            // Nothing failed and there is nothing to validate (SPEC.md §15.2's preview
            // without a recording). Answered as a report rather than an error so a caller
            // cannot mistake "no file was asked for" for "the file went wrong".
            mux::ValidationReport report;
            report.valid = false;
            report.detail = "preview-only session; no file was written";
            FC_LOG_INFO(Subsystem::App, "preview session stopped",
                        LogFields{}
                            .add("frames_captured", static_cast<std::int64_t>(impl_->frames_captured.load()))
                            .add("preview_published", static_cast<std::int64_t>(impl_->retired_preview.published)));
            return report;
        }
        if (impl_->last_report.has_value()) {
            // A file was written, finalized and validated before the pipeline was
            // released -- a segment rollover whose *next* segment could not be opened
            // (BUG-057). The recording ended earlier than the caller asked, which is
            // what `stop_requested` and the failed `MigrationRecord` say; what it did
            // not do is cease to exist.
            FC_LOG_WARN(Subsystem::App, "the session ended before stop was called; reporting the last finalized file",
                        LogFields{}
                            .add("output", impl_->settings.output.string())
                            .add("valid", impl_->last_report->valid)
                            .add("decoded_frames", impl_->last_report->decoded_frames)
                            .add("segments", static_cast<std::int64_t>(impl_->segment_index)));
            return *impl_->last_report;
        }
        return FcError::INTERNAL_INVALID_STATE;
    }
    const Result<mux::ValidationReport> report = impl_->pipeline->stop();

    FC_LOG_INFO(Subsystem::App, "recording session stopped",
                LogFields{}
                    .add("frames_captured", static_cast<std::int64_t>(impl_->frames_captured.load()))
                    .add("rebuilds", static_cast<std::int64_t>(impl_->records.size()))
                    .add("segments", static_cast<std::int64_t>(impl_->segment_index))
                    .add("worst_gap_ms", impl_->worst_gap_ns.load() / 1'000'000));
    return report;
}

std::filesystem::path RecordingSession::current_output() const {
    const std::lock_guard lock(impl_->records_mutex);
    return impl_->current_output;
}

bool RecordingSession::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

bool RecordingSession::paused() const noexcept {
    return impl_->paused.load(std::memory_order_acquire);
}

Result<void> RecordingSession::pause() {
    if (!impl_->running.load(std::memory_order_acquire) || !impl_->pipeline) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (impl_->paused.exchange(true, std::memory_order_acq_rel)) {
        return ok(); // SPEC.md §7.5: idempotent
    }

    // Capture stopped submitting the instant the exchange above landed, so the queue
    // the pipeline is about to drain has a fixed contents rather than a moving one.
    const Result<void> paused = impl_->pipeline->pause();
    if (!paused.has_value()) {
        // Put the session back the way it was rather than leaving capture suppressed
        // by a pause the pipeline does not believe in -- that state records nothing and
        // reports itself as running.
        impl_->paused.store(false, std::memory_order_release);
        return paused.error();
    }
    return ok();
}

Result<void> RecordingSession::resume() {
    if (!impl_->running.load(std::memory_order_acquire) || !impl_->pipeline) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (!impl_->paused.load(std::memory_order_acquire)) {
        return ok(); // idempotent
    }

    // The pipeline first: it closes the paused span and arms the forced IDR. Releasing
    // capture before that would let a frame be submitted while the clock still reports
    // the recording paused, and the pacer would excise it -- a frame lost at the seam,
    // which is exactly where a lost frame is most visible.
    FC_TRY(impl_->pipeline->resume());
    impl_->paused.store(false, std::memory_order_release);
    return ok();
}

SessionStats RecordingSession::stats() const {
    SessionStats out;
    if (impl_->pipeline) {
        out.pipeline = impl_->pipeline->stats();
        out.audio = impl_->pipeline->audio_stats();
        out.app_tracks = impl_->pipeline->app_track_stats();
        out.health = impl_->pipeline->health();
    }
    {
        // Whatever earlier writers left behind plus the live one, so a §5.4 rebuild or a
        // `stop_preview` does not appear to reset the preview's history.
        const std::lock_guard lock(impl_->preview_mutex);
        out.preview = impl_->retired_preview;
        if (impl_->preview) {
            accumulate(out.preview, impl_->preview->stats());
        }
    }
    out.frames_captured = impl_->frames_captured.load(std::memory_order_relaxed);
    out.capture_timeouts = impl_->capture_timeouts.load(std::memory_order_relaxed);
    out.worst_rebuild_gap_ns = impl_->worst_gap_ns.load(std::memory_order_relaxed);
    {
        const std::lock_guard lock(impl_->records_mutex);
        out.rebuilds = impl_->records.size();
    }
    out.segments = static_cast<std::uint64_t>(impl_->segment_index);
    return out;
}

std::vector<MigrationRecord> RecordingSession::migrations() const {
    const std::lock_guard lock(impl_->records_mutex);
    return impl_->records;
}

void RecordingSession::inject_device_error(std::int32_t hr, std::int32_t removed_reason) {
    impl_->watcher.report_device_error(hr, removed_reason);
}

void RecordingSession::set_preview_enabled(bool enabled) noexcept {
    impl_->preview_wanted.store(enabled, std::memory_order_release);
}

void RecordingSession::force_next_adapter(gpu::AdapterId adapter) {
    impl_->forced_adapter.store(adapter.value, std::memory_order_release);
}

} // namespace fc::pipeline
