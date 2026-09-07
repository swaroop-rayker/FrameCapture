#include "core/pipeline/video_pipeline.h"

#include "core/build_info.h"
#include "core/color/nv12_converter.h"
#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/mux/recovery.h"
#include "core/mux/segment_planner.h"
#include "core/timing/frame_pacer.h"
#include "core/timing/qpc_clock.h"
#include "core/timing/session_epoch.h"
#include "core/util/atomic_write.h"
#include "core/util/bounded_queue.h"
#include "core/util/thread_utils.h"
#include "core/util/worker.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace fc::pipeline {
namespace {

using Microsoft::WRL::ComPtr;

/// SPEC.md §9: bounded encoder input queue, capacity 8.
constexpr std::size_t kEncodeQueueCapacity = 8;

/// The mux queue can be deeper: the mux thread is allowed to block on disk, and a
/// packet dropped here is data already spent CPU and GPU time producing.
constexpr std::size_t kMuxQueueCapacity = 64;

/// How often the `watchdog` thread samples for the degradation ladder (SPEC.md §12,
/// §13).
///
/// 250 ms gives twelve samples inside §13's 3-second window, which is enough for the
/// "sustained" qualifiers to mean something and infrequent enough that the sampling
/// itself -- one `GetDiskFreeSpaceExW` every eight ticks, one percentile over 512
/// stack-resident integers -- is not a cost worth measuring.
constexpr auto kWatchdogInterval = std::chrono::milliseconds{250};

/// Free space is queried every this-many watchdog ticks rather than every tick.
/// `GetDiskFreeSpaceExW` is a real syscall and free space does not move at 4 Hz;
/// latency, which does, is read every tick.
constexpr int kFreeSpaceEveryNTicks = 8;

/// One captured frame in flight between the capture thread and the venc thread.
struct QueuedFrame {
    /// Index into the pipeline's staging ring, or -1 for none.
    int slot = -1;
    std::int64_t qpc_ns = 0;
};

/// A fixed set of staging textures with a free list.
///
/// The capture backends recycle the textures they hand out -- WGC's frame pool
/// reuses its surfaces and DDA invalidates on `ReleaseFrame` -- so a queued frame
/// cannot simply hold a reference to the capture texture and convert it later.
/// Copying into a slot the pipeline owns decouples the two lifetimes.
///
/// The free list is what makes it correct rather than merely likely to work: a
/// slot is unavailable until the venc thread has finished converting it, so a slot
/// can never be overwritten while it is still queued. When none is free the frame
/// is dropped and counted, which is the deliberate degradation SPEC.md §12 asks
/// for -- not a stall on the capture thread.
class StagingRing {
public:
    [[nodiscard]] Result<void> create(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source, std::size_t count) {
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;

        textures_.resize(count);
        free_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            FC_HR_AS(device->CreateTexture2D(&desc, nullptr, &textures_[i]), FcError::GPU_TEXTURE_CREATE_FAILED);
            free_.push_back(static_cast<int>(i));
        }
        format_ = desc.Format;
        width_ = desc.Width;
        height_ = desc.Height;
        return ok();
    }

    /// -1 when every slot is still in flight.
    [[nodiscard]] int acquire() {
        const std::lock_guard lock(mutex_);
        if (free_.empty()) {
            return -1;
        }
        const int slot = free_.back();
        free_.pop_back();
        return slot;
    }

    void release(int slot) {
        if (slot < 0) {
            return;
        }
        const std::lock_guard lock(mutex_);
        free_.push_back(slot);
    }

    [[nodiscard]] ID3D11Texture2D* at(int slot) const {
        return textures_[static_cast<std::size_t>(slot)].Get();
    }

    [[nodiscard]] bool matches(const D3D11_TEXTURE2D_DESC& desc) const noexcept {
        return !textures_.empty() && desc.Format == format_ && desc.Width == width_ && desc.Height == height_;
    }

    [[nodiscard]] bool created() const noexcept {
        return !textures_.empty();
    }

    /// Releases every slot. Called when the device they belong to is being replaced
    /// (SPEC.md §5.4) -- a texture outliving its device is a use-after-free the driver
    /// reports as a hang rather than as anything legible.
    void reset() {
        const std::lock_guard lock(mutex_);
        textures_.clear();
        free_.clear();
        format_ = DXGI_FORMAT_UNKNOWN;
        width_ = 0;
        height_ = 0;
    }

private:
    std::mutex mutex_;
    std::vector<ComPtr<ID3D11Texture2D>> textures_;
    std::vector<int> free_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace

struct VideoPipeline::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    PipelineSettings settings;
    color::Nv12Converter converter;
    std::unique_ptr<encode::IVideoEncoder> encoder;
    /// A `unique_ptr` rather than a value because SPEC.md §11's rollover needs *two*
    /// muxers alive at once: "the next file's `AVFormatContext` is opened and its header
    /// written **before** the current one is closed", which is what makes a split lose no
    /// packet. `Muxer` is deliberately neither copyable nor movable -- it owns an open
    /// file and a libavformat context -- so the swap is a pointer swap.
    std::unique_ptr<mux::Muxer> muxer = std::make_unique<mux::Muxer>();
    timing::Pacer pacer;
    StagingRing ring;

    // --- SPEC.md §11 segmentation ------------------------------------------
    /// Default-constructed, and a default `SegmentPlanner` never splits -- CLAUDE.md hard
    /// rule 7 holds even if `start` forgets to configure it.
    mux::SegmentPlanner segments;
    /// The file currently open. Not `settings.output` once a split has happened.
    std::filesystem::path current_segment_path;
    /// Timeline position the current file's timestamps count from (§11's restart at 0).
    std::int64_t segment_origin_ns = 0;
    /// Kept for the `<basename>.segments.json` sidecar §11 requires, so external tools
    /// can concatenate. Guarded by `stats_mutex`.
    std::vector<SegmentRecord> completed_segments;
    /// Borrowed for the duration of the recording so a rollover can add the audio streams
    /// to the next file. `avformat_write_header` fixes the stream set, so a segment that
    /// opened without audio would be video-only for the rest of the recording -- and with
    /// SPEC.md §8.6 that generalises: a segment that opened with fewer tracks than the
    /// first would silently drop applications at the split, which §8.6 forbids outright.
    /// Held as the exact specs the first file was opened with, names included.
    std::vector<mux::AudioStreamSpec> audio_streams_for_mux;

    BoundedQueue<QueuedFrame> encode_queue{kEncodeQueueCapacity, QueuePolicy::DropOldest};
    BoundedQueue<encode::EncodedPacket> mux_queue{kMuxQueueCapacity, QueuePolicy::Block};

    std::thread venc_thread;
    std::thread mux_thread;
    std::thread watchdog_thread;

    // Signalled by each worker as it leaves its loop, so shutdown can wait with a
    // deadline. `std::thread::join` has no timed form, and SPEC.md §12 forbids an
    // unbounded one.
    std::promise<void> venc_done;
    std::promise<void> mux_done;
    std::promise<void> watchdog_done;
    std::future<void> venc_finished;
    std::future<void> mux_finished;
    std::future<void> watchdog_finished;

    // ---------------------------------------------------------------------
    // The degradation ladder (SPEC.md §13)
    // ---------------------------------------------------------------------

    health::Monitor monitor;

    // The watchdog waits on this rather than sleeping, so shutdown does not have to
    // wait out a full sampling interval before the thread notices.
    std::mutex watchdog_mutex;
    std::condition_variable watchdog_wake;
    bool watchdog_exit = false;

    /// When capture last produced a frame, for SPEC.md §20 row 9's stall detector.
    /// Written by the capture thread in `submit`, read by the watchdog.
    std::atomic<std::int64_t> last_frame_ns{0};

    /// The frame rate the ladder wants (SPEC.md §13 rung 3), published by the
    /// watchdog and applied by the venc thread.
    ///
    /// It has to cross threads this way round because `timing::Pacer` is not
    /// thread-safe and belongs to the venc thread (SPEC.md §12). A watchdog that
    /// called `retime` directly would be mutating the pacer's `last_index_` while
    /// `decide` was reading it, which corrupts the PTS sequence -- the exact defect
    /// rung 3 exists to avoid causing.
    std::atomic<int> requested_fps{0};
    std::atomic<std::uint64_t> retimes{0};

    /// The quality target the ladder wants (SPEC.md §13 rung 2), published the same way
    /// and for the same reason: `IVideoEncoder` documents itself as driven entirely from
    /// the venc thread and thread-safe in no part, and `try_set_cqp` *mutates* the codec
    /// context. Calling it from the watchdog would race `avcodec_send_frame`.
    std::atomic<int> requested_cqp{0};
    /// Set by the venc thread when the encoder refused a quality change, so the watchdog
    /// can log the gap once without having to ask the encoder anything.
    std::atomic<bool> cqp_refused{false};

    /// Last free-space answer, carried between the ticks that do not query
    /// (`kFreeSpaceEveryNTicks`). Watchdog thread only.
    std::uint64_t last_free_bytes = UINT64_MAX;

    /// The quality target the encoder has actually been told. Venc thread only.
    int applied_cqp = 0;

    /// True when the ladder wanted rung 1's preset step, which nothing in this build
    /// can perform. Latched so the log line is emitted once (BUG-024).
    bool preset_step_reported = false;

    mutable std::mutex health_mutex;
    PipelineHealth health;

    // Exactly one of these is non-null when the recording has audio. Held as two
    // pointers rather than one base class because `External` deliberately has no
    // endpoint, no COM apartment and no `silence` thread -- there is no shared
    // behaviour to abstract over, only a shared destination.
    std::unique_ptr<AudioPath> loopback_audio;
    std::unique_ptr<AudioEncodePath> external_audio;
    /// SPEC.md §8.6's tracks 1..5, or null when Tier B is off.
    ///
    /// **Beside track 0, not around it.** Turning Tier B on constructs this object;
    /// it does not change how the two pointers above behave, what threads they run,
    /// or what they put on the mux queue. That is what makes §24's "Tier A provably
    /// unaffected when Tier B is off" a structural claim rather than a hope --
    /// docs/ACCEPTANCE.md carries the measurement that checks it survived the two
    /// things they *do* share, the mux queue and the muxer.
    std::unique_ptr<AppAudioTracks> app_tracks;
    /// `External` has no `audio` thread to notice its own first buffer, so the
    /// once-only latch lives here instead.
    std::atomic<bool> external_notified{false};

    std::atomic<bool> running{false};

    // Guards the epoch negotiation: the capture thread notes video, the audio
    // thread notes audio, and whichever completes the pair starts the pacer.
    std::mutex epoch_mutex;
    timing::SessionEpoch epoch;

    /// SPEC.md §7.5's paused total. Lives here, next to the epoch, because §7.5 puts it
    /// "alongside `t0` in the session's clock" -- the pacer and the audio timeline are
    /// each handed a pointer to *this* object and neither keeps a total of its own.
    timing::PauseClock pause_clock;

    /// §7.5's forced IDR on resume. Published for the `venc` thread rather than applied
    /// where `resume` runs, because the encoder belongs to that thread (SPEC.md §12) and
    /// touching it here would race `avcodec_send_frame` -- the same reasoning that keeps
    /// rung 2's `try_set_cqp` out of the watchdog.
    std::atomic<bool> idr_requested{false};

    mutable std::mutex stats_mutex;
    PipelineStats stats;

    void venc_loop();
    void mux_loop();
    void watchdog_loop();
    /// SPEC.md §11's rollover. Mux thread only -- it is the one owner of the
    /// `AVFormatContext` (§10.1). Returns false when the next file could not be opened,
    /// in which case the current one is untouched and the recording continues in it.
    [[nodiscard]] bool roll_segment(std::int64_t pts_ns);
    /// One pass of SPEC.md §13's evaluation. Watchdog thread only.
    void evaluate_health(int tick);
    /// Applies whatever the ladder asked of the pacer and the encoder. Venc thread only
    /// -- both of those objects belong to it (SPEC.md §12).
    void apply_ladder_request();
    [[nodiscard]] Result<void> encode_one(const QueuedFrame& queued);
    [[nodiscard]] Result<void> submit_with_backpressure(std::int64_t pts);
    void drain_encoder_packets();

    void note_first_audio(std::int64_t qpc_ns);

    /// The encoder configuration for this recording, with only the adapter-specific
    /// fields varying. Shared by `start` and `rebuild_device` so a migration cannot
    /// drift from the original in a field nobody thought to copy -- which for
    /// `profile`, `max_b_frames` or `full_range` would change the SPS and make the
    /// extradata check below fail for a reason that looks like hardware.
    [[nodiscard]] encode::VideoEncoderSettings encoder_settings_for(const std::string& name,
                                                                    std::uint32_t bind_flags) const {
        encode::VideoEncoderSettings out;
        out.width = settings.video.width;
        out.height = settings.video.height;
        out.fps = settings.video.fps;
        out.codec = settings.video.codec;
        out.rate_control = settings.video.rate_control;
        out.cqp = settings.video.cqp;
        out.bitrate_kbps = settings.video.bitrate_kbps;
        out.gop_seconds = settings.video.gop_seconds;
        out.max_b_frames = settings.video.max_b_frames;
        out.full_range = settings.video.full_range;
        out.encoder_name = name;
        out.pool_bind_flags = bind_flags;
        return out;
    }

    /// The AAC encoder, whichever kind of audio path this recording has. Null for
    /// video-only.
    [[nodiscard]] const encode::AacEncoder* audio_encoder() const noexcept {
        if (loopback_audio) {
            return loopback_audio->encoder();
        }
        if (external_audio) {
            return external_audio->encoder();
        }
        return nullptr;
    }

    /// Starts the pacer and publishes `t0` to the audio path once both streams
    /// have produced something. Returns false while the pair is incomplete.
    ///
    /// Must be called with `epoch_mutex` held. Both streams reach it -- the
    /// capture thread through `submit`, the audio thread through
    /// `note_first_audio` -- and whichever completes the pair does the publishing,
    /// so neither has to be the one that happens to be later.
    [[nodiscard]] bool resolve_epoch_locked();

    void publish_epoch_locked(std::int64_t t0_ns) const;
};

void VideoPipeline::Impl::publish_epoch_locked(std::int64_t t0_ns) const {
    if (loopback_audio) {
        loopback_audio->set_epoch(t0_ns);
    }
    if (external_audio) {
        external_audio->set_epoch(t0_ns);
    }
    // SPEC.md §8.6: "All tracks share one timebase, one epoch (`t0`)". The whole of
    // §20 row 14's alignment guarantee is this one line -- every track is told the
    // same instant, and each pads itself from it rather than from whenever its own
    // first buffer happened to arrive. "Ragged track start times are the classic
    // multi-track desync bug", and the way to not have them is to never give a track
    // an origin of its own.
    if (app_tracks) {
        app_tracks->set_epoch(t0_ns);
    }
}

bool VideoPipeline::Impl::resolve_epoch_locked() {
    if (pacer.started()) {
        return true;
    }
    if (!epoch.resolved()) {
        return false;
    }

    const std::int64_t t0 = epoch.t0_ns();
    pacer.start(t0);
    publish_epoch_locked(t0);

    FC_LOG_INFO(Subsystem::Encode, "media epoch resolved",
                LogFields{}
                    .add("t0_ns", t0)
                    .add("startup_skew_us", epoch.startup_skew_ns() / 1000)
                    .add("expects_audio", epoch.expects_audio()));
    return true;
}

void VideoPipeline::Impl::note_first_audio(std::int64_t qpc_ns) {
    const std::lock_guard lock(epoch_mutex);
    epoch.note_audio(qpc_ns);
    // Not just a note: if video is already waiting, this is the call that resolves
    // the epoch, starts the pacer and hands `t0` to the audio timeline. Recording
    // the timestamp without publishing the result would leave both streams
    // stalled, each holding half of an epoch neither could see.
    static_cast<void>(resolve_epoch_locked());
}

void VideoPipeline::Impl::drain_encoder_packets() {
    for (;;) {
        auto received = encoder->receive();
        if (!received.has_value()) {
            const std::lock_guard lock(stats_mutex);
            ++stats.encode_failures;
            return;
        }
        // `Result::value() &&` is the only overload that yields an rvalue; the
        // lvalue one returns a const reference and EncodedPacket is move-only.
        std::optional<encode::EncodedPacket> packet = std::move(received).value();
        if (!packet.has_value()) {
            return; // encoder wants more input, or the stream is drained
        }
        mux_queue.push(std::move(*packet));
    }
}

Result<void> VideoPipeline::Impl::submit_with_backpressure(std::int64_t pts) {
    // A hardware encoder holds one input surface per frame it has accepted but not
    // yet emitted a packet for, and the pool is deliberately fixed-size. When it is
    // exhausted the answer is to drain -- which is what returns surfaces -- and try
    // again, not to grow the pool.
    //
    // Bounded, because a genuinely wedged encoder must surface as an error rather
    // than as a silent spin on the venc thread.
    constexpr int kMaxAttempts = 64;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const Result<void> submitted = encoder->submit(converter.output(), pts);
        if (submitted.has_value()) {
            drain_encoder_packets();
            return ok();
        }
        if (submitted.error() != FcError::INTERNAL_QUEUE_FULL) {
            return submitted.error();
        }
        drain_encoder_packets();
    }

    FC_LOG_ERROR(Subsystem::Encode, "encoder input pool stayed exhausted after draining",
                 LogFields{}.add("attempts", kMaxAttempts).add_error(FcError::ENCODE_SUBMIT_FAILED));
    return FcError::ENCODE_SUBMIT_FAILED;
}

Result<void> VideoPipeline::Impl::encode_one(const QueuedFrame& queued) {
    // Before `pacer.decide`, on the only thread allowed to touch the pacer or the
    // encoder.
    apply_ladder_request();

    ID3D11Texture2D* source = ring.at(queued.slot);

    const Result<void> converted = converter.convert(context.Get(), source);
    ring.release(queued.slot); // done with the staging slot either way
    if (!converted.has_value()) {
        const std::lock_guard lock(stats_mutex);
        ++stats.convert_failures;
        return converted.error();
    }

    const timing::PacingDecision decision = pacer.decide(queued.qpc_ns);
    if (decision.drop) {
        const std::lock_guard lock(stats_mutex);
        ++stats.frames_paced_out;
        return ok();
    }

    // Duplicates first, then the frame itself, so PTS reaches the encoder in
    // order. Each duplicate re-submits the same NV12 texture with the timestamp of
    // the slot it fills -- SPEC.md §7.2: the duplicate is deliberate and its PTS is
    // the grid's, not the capture clock's.
    for (const std::int64_t duplicate_pts : decision.duplicate_pts) {
        FC_TRY(submit_with_backpressure(duplicate_pts));
    }
    FC_TRY(submit_with_backpressure(decision.pts));

    {
        const std::lock_guard lock(stats_mutex);
        stats.frames_encoded += 1 + decision.duplicate_pts.size();
        stats.duplicates_emitted += decision.duplicate_pts.size();
    }
    return ok();
}

void VideoPipeline::Impl::evaluate_health(int tick) {
    health::Sample sample;
    sample.now_ns = timing::qpc_now_ns();
    sample.encoder_queue_pressure = encode_queue.pressure();
    sample.last_frame_ns = last_frame_ns.load(std::memory_order_acquire);
    sample.disk_write_p99_ns = muxer->write_latency_p99_ns();
    sample.hardware_encoder = encoder ? encoder->is_hardware() : true;
    // SPEC.md §7.5 suspends row 9's stall detector and the frame-drop ratio while
    // paused. Passed as an observation rather than short-circuiting `evaluate` here, so
    // the disk and device rungs keep running -- a volume filling up during a long pause
    // is still worth knowing about, and rung 6's answer to it is unchanged.
    sample.paused = pause_clock.paused();

    {
        const std::lock_guard lock(stats_mutex);
        sample.frames_submitted = stats.frames_submitted;
        sample.frames_queue_dropped = stats.frames_queue_dropped;
    }

    // Free space every eighth tick; on the others carry the previous answer forward
    // rather than reporting "not measured", which would let rung 6 disengage and
    // re-engage at 4 Hz on a genuinely full volume.
    if (tick % kFreeSpaceEveryNTicks == 0) {
        last_free_bytes = muxer->output_volume_free_bytes();
    }
    sample.disk_free_bytes = last_free_bytes;

    const health::Plan plan = monitor.evaluate(sample);

    // Rung 3. Published for the venc thread rather than applied here.
    requested_fps.store(plan.target_fps, std::memory_order_release);

    // Rung 2. Published for the venc thread, not applied here -- `try_set_cqp` mutates
    // the codec context and `IVideoEncoder` is documented as driven from the venc thread
    // and thread-safe in no part, so calling it here would race `avcodec_send_frame`.
    requested_cqp.store(plan.target_cqp, std::memory_order_release);

    if (plan.changed) {
        // One line per transition, never per sample: a recording spending ten
        // minutes at rung 1 must not write 2400 log lines about it.
        const LogFields fields =
            LogFields{}
                .add("rung", static_cast<std::int64_t>(plan.rung))
                .add("trigger", health::to_string(plan.rung))
                .add("queue_pressure", sample.encoder_queue_pressure)
                .add("drop_ratio", monitor.drop_ratio())
                .add("target_fps", plan.target_fps)
                .add("write_p99_us", sample.disk_write_p99_ns / 1000)
                .add("free_mb", static_cast<std::int64_t>(sample.disk_free_bytes / (1024ULL * 1024)))
                .add("stop_requested", plan.stop_and_finalize);
        if (plan.rung == health::Rung::Nominal) {
            FC_LOG_INFO(Subsystem::Health, "degradation recovered", fields);
        } else {
            FC_LOG_WARN(Subsystem::Health, "degradation level changed", fields);
        }
    }

    // BUG-024, reported once: the ladder asked for rung 1's preset step and no
    // encoder in this build can perform one. Saying so is the difference between a
    // known limitation and an unexplained quality cliff at rung 3.
    if (plan.preset_step > 0 && !preset_step_reported) {
        preset_step_reported = true;
        FC_LOG_WARN(Subsystem::Health,
                    "SPEC.md §13 rung 1 asked for an encoder preset step; no encoder in this build can change preset "
                    "mid-recording (BUG-024). Degradation continues at rung 2 and above",
                    LogFields{}.add("encoder", encoder ? encoder->encoder_name() : std::string_view{}));
    }
    if (cqp_refused.load(std::memory_order_acquire)) {
        FC_LOG_TRACE(Subsystem::Health, "rung 2's quality reduction was not applied; the encoder cannot change QP",
                     LogFields{}.add("requested_cqp", plan.target_cqp));
    }

    const std::lock_guard lock(health_mutex);
    health.rung = plan.rung;
    health.rung_transitions = monitor.transitions();
    health.target_fps = plan.target_fps;
    health.pacer_fps = pacer.fps();
    health.retimes = retimes.load(std::memory_order_relaxed);
    health.drop_ratio = monitor.drop_ratio();
    health.disk_write_p99_ns = sample.disk_write_p99_ns;
    health.disk_free_bytes = sample.disk_free_bytes;
    health.stall_episodes = monitor.stall_episodes();
    health.worst_stall_ns = monitor.worst_stall_ns();
    health.current_stall_ns = plan.capture_stall_ns;
    health.stop_requested = plan.stop_and_finalize;
    health.gpu_migration_requested = plan.request_gpu_migration;
    health.hardware_encoder = sample.hardware_encoder;
}

void VideoPipeline::Impl::watchdog_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(watchdog_done);
    set_thread_name("fc-watchdog");
    try {
        int tick = 0;
        for (;;) {
            {
                std::unique_lock lock(watchdog_mutex);
                if (watchdog_wake.wait_for(lock, kWatchdogInterval, [this] { return watchdog_exit; })) {
                    break;
                }
            }
            evaluate_health(tick++);
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        // The ladder going blind must not end the recording. A recording that
        // degrades badly and finishes is worth more than one that stops because the
        // component watching it failed.
        FC_LOG_ERROR(Subsystem::Health, "watchdog thread terminated by an exception; the ladder is no longer evaluated",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

void VideoPipeline::Impl::apply_ladder_request() {
    // Rung 3. `retime` rescales the pacer's last emitted index onto the new grid, so the
    // next PTS is strictly greater than the last one written and the CFR timeline
    // survives the change (SPEC.md §13 rung 3: "keep the CFR timeline intact via
    // duplicates").
    const int requested = requested_fps.load(std::memory_order_acquire);
    if (requested > 0 && requested != pacer.fps()) {
        const int previous = pacer.fps();
        pacer.retime(requested);
        retimes.fetch_add(1, std::memory_order_relaxed);
        FC_LOG_WARN(Subsystem::Health, "capture rate retimed by the degradation ladder",
                    LogFields{}.add("from_fps", previous).add("to_fps", requested));
    }

    // Rung 2. On this thread because the encoder belongs to it (SPEC.md §12); the
    // watchdog only publishes the target. `applied_cqp` starts at the configured value,
    // so the first evaluation at rung 0 is a no-op rather than a redundant option write
    // on every frame.
    const int cqp = requested_cqp.load(std::memory_order_acquire);
    if (encoder && cqp > 0 && cqp != applied_cqp) {
        if (encoder->try_set_cqp(cqp)) {
            applied_cqp = cqp;
            FC_LOG_WARN(Subsystem::Health, "video quality target lowered by the degradation ladder",
                        LogFields{}.add("cqp", cqp));
        } else {
            // Latched so the venc thread stops asking every frame for something this
            // encoder can never do (BUG-024). The watchdog reports it once.
            applied_cqp = cqp;
            cqp_refused.store(true, std::memory_order_release);
        }
    }

    // SPEC.md §7.5's resume. `exchange` rather than load-then-clear so a request that
    // arrives between the two cannot be lost -- a dropped IDR request is a smeared seam
    // that survives until the next GOP boundary, up to §9's two seconds later.
    if (encoder && idr_requested.exchange(false, std::memory_order_acq_rel)) {
        encoder->request_keyframe();
    }
}

void VideoPipeline::Impl::venc_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(venc_done);
    set_thread_name("fc-venc");
    try {
        while (const auto queued = encode_queue.pop()) {
            const Result<void> encoded = encode_one(*queued);
            if (!encoded.has_value()) {
                // A single frame failing must not end the recording: CLAUDE.md §1
                // requires the file to stay valid and playable. Counted, logged,
                // and the loop continues.
                FC_LOG_WARN(Subsystem::Encode, "frame dropped after an encode-stage failure",
                            LogFields{}.add_error(encoded.error()));
                const std::lock_guard lock(stats_mutex);
                ++stats.encode_failures;
            }
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Encode, "venc thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

/// SPEC.md §11's `<basename>.segments.json`.
///
/// > Write a sidecar `<basename>.segments.json` recording each segment's global start
/// > offset so external tools can concatenate.
///
/// Named from the recording's *base* path, not from any segment, so one predictable file
/// describes the set however many parts there turned out to be. Written atomically for
/// the same reason the recovery sidecar is: a half-written index is worse than none,
/// because a tool would read it and believe it.
///
/// Best-effort. A recording whose files are all valid is not a failed recording because
/// an index could not be written, and saying otherwise would trade a real success for a
/// bookkeeping failure (CLAUDE.md §1).
namespace {

void write_segments_sidecar(const std::filesystem::path& base, const std::vector<SegmentRecord>& segments) {
    nlohmann::json document;
    document["version"] = 1;
    document["base"] = base.filename().string();
    document["count"] = segments.size();

    nlohmann::json list = nlohmann::json::array();
    for (const SegmentRecord& record : segments) {
        nlohmann::json entry;
        entry["file"] = record.path.filename().string();
        entry["start_offset_ns"] = record.start_offset_ns;
        entry["duration_ns"] =
            record.end_offset_ns > record.start_offset_ns ? record.end_offset_ns - record.start_offset_ns : 0;
        list.push_back(std::move(entry));
    }
    document["segments"] = std::move(list);

    std::filesystem::path sidecar = base;
    sidecar.replace_extension(".segments.json");
    const std::string text = document.dump(2);
    if (const Result<void> written = write_file_atomically(sidecar, text); !written.has_value()) {
        FC_LOG_WARN(Subsystem::Mux, "the segment index could not be written; the recordings are unaffected",
                    LogFields{}.add("path", sidecar.string()).add_error(written.error()));
        return;
    }
    FC_LOG_INFO(Subsystem::Mux, "segment index written",
                LogFields{}
                    .add("path", sidecar.filename().string())
                    .add("segments", static_cast<std::int64_t>(segments.size())));
}

/// Presentation time of a video packet, in nanoseconds on the recording's timeline.
///
/// Read before the write, because `av_interleaved_write_frame` takes the payload and
/// blanks the packet -- the same trap BUG-034's instrumentation fell into.
std::int64_t packet_pts_ns(const encode::EncodedPacket& packet) noexcept {
    if (!packet.packet || packet.packet->pts == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(packet.packet->pts, AVRational{1, static_cast<int>(timing::kVideoTimebaseDen)},
                        AVRational{1, 1'000'000'000});
}

} // namespace

/// SPEC.md §11's rollover: the next file exists before the current one stops existing.
///
/// > Segment rollover must not drop a single frame. The next file's `AVFormatContext` is
/// > opened and its header written **before** the current one is closed.
///
/// The order below is that sentence, and it is the whole of why this is not simply
/// "finalize, then open". If the open fails -- a full disk, a revoked directory -- the
/// recording still has a muxer, and it is the one that was already working: this returns
/// false, the planner is not advanced, and the packet goes to the current file exactly as
/// if no trigger had fired. A recording that keeps going in one long file is a far better
/// outcome than one that stops because it could not start a second one (CLAUDE.md §1).
bool VideoPipeline::Impl::roll_segment(std::int64_t pts_ns) {
    const int next_index = segments.index() + 1;
    const std::filesystem::path next_path = mux::segment_path(settings.output, next_index);

    mux::MuxerSettings next_settings;
    next_settings.output = next_path;
    next_settings.container = settings.video.container;
    next_settings.full_range = settings.video.full_range;
    next_settings.injected_stall_ns = settings.injected_stall_ns;
    next_settings.injected_stall_period = settings.injected_stall_period;
    // §11: each segment's timestamps restart at 0, so this file counts from the packet
    // that opens it.
    next_settings.timeline_origin_ns = pts_ns;

    auto next = std::make_unique<mux::Muxer>();
    if (const Result<void> opened =
            next->open(next_settings, *encoder, std::span<const mux::AudioStreamSpec>{audio_streams_for_mux});
        !opened.has_value()) {
        FC_LOG_ERROR(Subsystem::Mux, "opening the next segment failed; the recording continues in one file",
                     LogFields{}.add("path", next_path.string()).add_error(opened.error()));
        return false;
    }

    // Only now is the previous file allowed to close. Its trailer is written, its
    // interleaving buffer is flushed, and whatever audio was still held lands in the file
    // it belongs to.
    const std::filesystem::path closed_path = current_segment_path;
    if (const Result<void> finalized = muxer->finalize(); !finalized.has_value()) {
        FC_LOG_ERROR(Subsystem::Mux, "finalizing a segment failed; its file may be short",
                     LogFields{}.add("path", closed_path.string()).add_error(finalized.error()));
    }

    muxer = std::move(next);
    current_segment_path = next_path;
    {
        const std::lock_guard lock(stats_mutex);
        completed_segments.push_back(SegmentRecord{closed_path, segment_origin_ns, pts_ns});
        stats.segments = static_cast<std::uint64_t>(next_index);
    }
    segment_origin_ns = pts_ns;

    FC_LOG_INFO(Subsystem::Mux, "segment rolled over",
                LogFields{}
                    .add("closed", closed_path.filename().string())
                    .add("opened", next_path.filename().string())
                    .add("segment", static_cast<std::int64_t>(next_index))
                    .add("at_ns", pts_ns));
    return true;
}

void VideoPipeline::Impl::mux_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(mux_done);
    set_thread_name("fc-mux");
    try {
        while (auto packet = mux_queue.pop()) {
            // SPEC.md §11's split decision, taken here because this is the one thread
            // that owns the `AVFormatContext` (§10.1) and therefore the only place a
            // file can be closed without racing a write.
            //
            // Video only: a split lands on a video keyframe by definition, and offering
            // audio would let a boundary be chosen where no video packet exists.
            if (!packet->audio && segments.enabled()) {
                const std::int64_t pts_ns = packet_pts_ns(*packet);
                switch (segments.offer(pts_ns, packet->keyframe, muxer->bytes_written())) {
                case mux::SegmentPlanner::Action::Continue:
                    break;
                case mux::SegmentPlanner::Action::RequestKeyframe:
                    // The same request §7.5's resume uses. The venc thread consumes it
                    // before its next `pacer.decide`, so the IDR arrives at the encoder's
                    // next opportunity and this packet -- and the ones until it -- still
                    // belong to the file currently open.
                    idr_requested.store(true, std::memory_order_release);
                    FC_LOG_INFO(
                        Subsystem::Mux, "segment trigger fired; waiting for a keyframe to split on",
                        LogFields{}.add("segment", static_cast<std::int64_t>(segments.index())).add("pts_ns", pts_ns));
                    break;
                case mux::SegmentPlanner::Action::Split:
                    if (roll_segment(pts_ns)) {
                        segments.note_split(pts_ns, muxer->bytes_written());
                    }
                    break;
                }
            }

            const Result<void> written = muxer->write(std::move(*packet));
            if (!written.has_value()) {
                FC_LOG_ERROR(Subsystem::Mux, "packet write failed", LogFields{}.add_error(written.error()));
                continue;
            }
            const std::lock_guard lock(stats_mutex);
            stats.packets_muxed = muxer->packets_written();
            stats.bytes_written = muxer->bytes_written();
            stats.last_muxed_pts_ns = muxer->last_video_pts_ns();
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Mux, "mux thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

VideoPipeline::VideoPipeline() : impl_(std::make_unique<Impl>()) {}

VideoPipeline::~VideoPipeline() {
    if (abandoned_) {
        // A detached worker is still reading through this pointer. Freeing it here
        // would be a use-after-free on a thread we have already given up on, so
        // the allocation is released deliberately and never reclaimed. Logged at
        // the point of abandonment, not here -- the logger may itself be gone by
        // the time a destructor runs during shutdown.
        static_cast<void>(impl_.release()); // NOLINT(bugprone-unused-return-value)
        return;
    }

    if (!impl_ || !impl_->running.load(std::memory_order_acquire)) {
        return;
    }

    // Destroyed while still running. Finalizing here is what keeps CLAUDE.md §1
    // true on the paths that forget to stop -- an early return, a thrown
    // exception, a test that fails an assertion mid-recording. The file is worth
    // more than the tidiness of an empty destructor.
    //
    // `stop` joins threads and writes to disk, so it can throw. An exception
    // leaving a destructor terminates the process, which would turn a recoverable
    // finalization problem into a crash and lose the very file this call exists to
    // save. Caught by type rather than with `catch(...)`, and the handler logs --
    // CLAUDE.md §4 bans both the catch-all outside a thread entry and the empty
    // handler.
    try {
        const auto report = stop();
        if (!report.has_value()) {
            FC_LOG_ERROR(Subsystem::Mux, "pipeline destroyed while running; finalization failed",
                         LogFields{}.add_error(report.error()));
        }
    } catch (const std::exception& e) {
        FC_LOG_ERROR(Subsystem::Mux, "pipeline destructor could not finalize the recording",
                     LogFields{}.add("what", e.what()).add_error(FcError::MUX_FINALIZE_FAILED));
    }
}

Result<void> VideoPipeline::start(ID3D11Device* device, const PipelineSettings& settings) {
    if (device == nullptr || settings.output.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    // `video.pacing = vfr` is a real config key with a real schema entry and real
    // documentation, and until this check existed it silently produced a CFR
    // recording -- the setting was accepted and ignored. SPEC.md §7.3 specifies
    // VFR (timebase 1/1000000, PTS from the exact QPC delta, strictly monotonic)
    // and it is not implemented, so it is refused for the same reason MP4 is:
    // quietly doing something else is worse than saying no.
    if (settings.video.pacing != config::PacingMode::Cfr) {
        FC_LOG_ERROR(Subsystem::Encode, "only CFR pacing is implemented",
                     LogFields{}
                         .add("pacing", config::to_string(settings.video.pacing))
                         .add("hint", "SPEC.md §7.3 VFR is not implemented in this build")
                         .add_error(FcError::INTERNAL_NOT_IMPLEMENTED));
        return FcError::INTERNAL_NOT_IMPLEMENTED;
    }

    impl_->settings = settings;
    impl_->device = device;
    device->GetImmediateContext(&impl_->context);
    if (impl_->context.Get() == nullptr) {
        return FcError::GPU_DEVICE_CREATE_FAILED;
    }

    color::ConverterSettings converter_settings;
    converter_settings.width = settings.video.width;
    converter_settings.height = settings.video.height;
    converter_settings.range = settings.video.full_range ? color::ColorRange::Full : color::ColorRange::Limited;
    FC_TRY(impl_->converter.initialize(device, converter_settings));

    const encode::VideoEncoderSettings encoder_settings =
        impl_->encoder_settings_for(settings.encoder_name, settings.pool_bind_flags);

    FC_TRY_ASSIGN(impl_->encoder, encode::create_video_encoder(encoder_settings));
    FC_TRY(impl_->encoder->open(device, encoder_settings));

    // Before the audio path, not after: `AudioPath::start` spawns the `audio`
    // thread, and that thread can call `note_first_audio` before this function
    // returns. Assigning the epoch afterwards would overwrite the timestamp it
    // recorded, and the recording would then wait forever for an audio packet that
    // had already arrived.
    impl_->pacer = timing::Pacer{settings.video.fps};
    // Re-attached after the assignment above, which replaces the pacer wholesale and
    // would otherwise leave it with no clock on the second recording of a process.
    impl_->pause_clock.reset();
    impl_->pacer.attach_pause_clock(&impl_->pause_clock);
    impl_->epoch = timing::SessionEpoch{settings.audio.source != AudioSource::None};
    impl_->external_notified.store(false, std::memory_order_release);
    impl_->idr_requested.store(false, std::memory_order_release);

    // Fresh per run, like the pacer and the epoch. A monitor carried across two
    // recordings would arrive at the second one already holding the first one's
    // latches, and would report a degraded recording that had not started yet.
    impl_->monitor = health::Monitor{settings.video.fps, settings.video.cqp};
    impl_->last_frame_ns.store(0, std::memory_order_release);
    impl_->requested_fps.store(0, std::memory_order_release);
    impl_->requested_cqp.store(0, std::memory_order_release);
    impl_->cqp_refused.store(false, std::memory_order_release);
    impl_->retimes.store(0, std::memory_order_release);
    impl_->last_free_bytes = UINT64_MAX;
    impl_->applied_cqp = settings.video.cqp;
    impl_->preset_step_reported = false;
    {
        const std::lock_guard lock(impl_->health_mutex);
        impl_->health = PipelineHealth{};
        impl_->health.target_fps = settings.video.fps;
        impl_->health.pacer_fps = settings.video.fps;
    }
    {
        const std::lock_guard lock(impl_->watchdog_mutex);
        impl_->watchdog_exit = false;
    }

    // The audio path opens *before* the muxer, and the order is load-bearing.
    // `avformat_write_header` fixes the stream set, so an audio stream added after
    // it never appears in the file -- and the stream's parameters come from the
    // AAC encoder, whose channel layout comes from the endpoint's mix format,
    // which is not known until the endpoint has been opened.
    const encode::AacEncoder* audio_encoder = nullptr;
    if (settings.audio.source != AudioSource::None) {
        // Both kinds push onto the same mux queue: SPEC.md §10.1 allows exactly
        // one `AVFormatContext` owner, and the packet carries its own destination.
        // Block policy, so an audio packet is never dropped (SPEC.md §12).
        AudioPacketSink sink = [impl = impl_.get()](encode::EncodedPacket packet) {
            static_cast<void>(impl->mux_queue.push(std::move(packet)));
        };

        if (settings.audio.source == AudioSource::SystemLoopback) {
            AudioPathSettings audio_settings;
            audio_settings.device_id = settings.audio.device_id;
            audio_settings.channel_layout = settings.audio.channel_layout;
            audio_settings.bitrate_kbps = settings.audio.bitrate_kbps;

            impl_->loopback_audio = std::make_unique<AudioPath>();
            // Before `start`: the same object the pacer was given two dozen lines above,
            // which is the whole of SPEC.md §7.5's shared-total requirement.
            impl_->loopback_audio->attach_pause_clock(&impl_->pause_clock);
            FC_TRY(impl_->loopback_audio->start(audio_settings, std::move(sink),
                                                [impl = impl_.get()](std::int64_t qpc_ns) {
                                                    // On the `audio` thread. Takes
                                                    // `epoch_mutex` briefly and does
                                                    // nothing else -- the audio thread
                                                    // must not block (SPEC.md §12).
                                                    impl->note_first_audio(qpc_ns);
                                                }));
            audio_encoder = impl_->loopback_audio->encoder();
        } else {
            AudioEncodeSettings audio_settings;
            audio_settings.input = settings.audio.external_format;
            audio_settings.channel_layout = settings.audio.channel_layout;
            audio_settings.bitrate_kbps = settings.audio.bitrate_kbps;
            audio_settings.buffer_period_ns = settings.audio.buffer_period_ns;

            impl_->external_audio = std::make_unique<AudioEncodePath>();
            impl_->external_audio->attach_pause_clock(&impl_->pause_clock);
            FC_TRY(impl_->external_audio->open(audio_settings, std::move(sink)));
            audio_encoder = impl_->external_audio->encoder();
        }

        if (audio_encoder == nullptr) {
            return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
        }
    }

    // ---------------------------------------------------------------------
    // SPEC.md §8.6 Tier B, opened after track 0 and before the muxer
    // ---------------------------------------------------------------------
    // The ordering is the same constraint as Tier A's, one track wider:
    // `avformat_write_header` fixes the stream set, so every per-application
    // encoder has to exist before the container does. It can, because §8.6 supplies
    // the process-loopback format rather than negotiating it -- see
    // `app_audio_tracks.h` for why that is what makes a track for an application
    // nobody has launched yet expressible at all.
    std::vector<mux::AudioStreamSpec> audio_streams;
    if (audio_encoder != nullptr) {
        // §8.6: "Track 0 is **always** the full system mix (Tier A output) so the
        // file is useful even in a player that exposes only the first track."
        //
        // Named only when there is something to distinguish it from. A single-track
        // recording gets an untagged stream exactly as it always has, so Tier B
        // changes no byte of a Tier A file's metadata.
        audio_streams.push_back(
            mux::AudioStreamSpec{audio_encoder, settings.audio.multitrack.enabled ? "System Mix" : ""});
    }

    if (settings.audio.multitrack.enabled) {
        if (audio_encoder == nullptr) {
            FC_LOG_ERROR(Subsystem::Audio,
                         "per-application tracks were requested with no system mix; SPEC.md §8.6 makes track 0 the "
                         "full mix",
                         LogFields{}.add_error(FcError::INTERNAL_INVALID_ARGUMENT));
            return FcError::INTERNAL_INVALID_ARGUMENT;
        }
        // §20 row 16, enforced here as well as at `configure`. This is the layer that
        // knows what container is about to be opened -- `start_record` resolves it
        // from the output path's extension, which can disagree with the setting
        // `configure` validated (BUG-043) -- so a check that only ran at `configure`
        // would be a check a `.mp4` path could walk straight past.
        if (settings.video.container != config::Container::Mkv) {
            FC_LOG_ERROR(Subsystem::Audio, "multi-track audio was requested on a container that cannot carry it",
                         LogFields{}
                             .add("container", std::string{config::to_string(settings.video.container)})
                             .add("hint", "SPEC.md §8.6: Tier B is MKV only")
                             .add_error(FcError::MULTITRACK_REQUIRES_MKV));
            return FcError::MULTITRACK_REQUIRES_MKV;
        }

        AppTracksSettings track_settings;
        track_settings.source = settings.audio.multitrack.source;
        track_settings.tracks = settings.audio.multitrack.tracks;
        track_settings.reattach = settings.audio.multitrack.reattach;
        track_settings.source_factory = settings.audio.multitrack.source_factory;
        track_settings.bitrate_kbps = settings.audio.bitrate_kbps;
        track_settings.buffer_period_ns = settings.audio.buffer_period_ns;

        impl_->app_tracks = std::make_unique<AppAudioTracks>();
        impl_->app_tracks->attach_pause_clock(&impl_->pause_clock);
        FC_TRY(impl_->app_tracks->start(track_settings, [impl = impl_.get()](int track, encode::EncodedPacket packet) {
            // The one thing a per-application packet needs that a Tier A one does
            // not: which stream it belongs to. Everything else -- one queue, one mux
            // thread, one `AVFormatContext` owner (SPEC.md §10.1) -- is unchanged.
            packet.audio_track = track;
            static_cast<void>(impl->mux_queue.push(std::move(packet)));
        }));

        const std::vector<const encode::AacEncoder*> track_encoders = impl_->app_tracks->encoders();
        const std::vector<std::string> track_names = impl_->app_tracks->names();
        for (std::size_t i = 0; i < track_encoders.size(); ++i) {
            if (track_encoders[i] == nullptr) {
                return FcError::ENCODE_AUDIO_ENCODER_OPEN_FAILED;
            }
            audio_streams.push_back(mux::AudioStreamSpec{track_encoders[i], track_names[i]});
        }
    }

    // SPEC.md §11. The planner is configured before the first file is opened, because
    // whether segmentation is on decides what that file is *called*: a split recording's
    // files are all `<basename>_partNNN`, the first one included, so there is no file
    // named after the base and no odd one out in the directory listing.
    impl_->segments = mux::SegmentPlanner{settings.segmentation};
    impl_->audio_streams_for_mux = audio_streams;
    impl_->current_segment_path = impl_->segments.enabled() ? mux::segment_path(settings.output, 1) : settings.output;
    impl_->segment_origin_ns = 0;
    impl_->completed_segments.clear();

    mux::MuxerSettings muxer_settings;
    muxer_settings.output = impl_->current_segment_path;
    muxer_settings.container = settings.video.container;
    muxer_settings.full_range = settings.video.full_range;
    muxer_settings.injected_stall_ns = settings.injected_stall_ns;
    muxer_settings.injected_stall_period = settings.injected_stall_period;
    FC_TRY(impl_->muxer->open(muxer_settings, *impl_->encoder,
                              std::span<const mux::AudioStreamSpec>{impl_->audio_streams_for_mux}));

    // SPEC.md §10.4's sidecar, written now that the stream shape is settled and
    // before a single frame exists. Its presence is what tells the next launch a
    // recording did not finish; writing it later would leave a window in which a
    // crash produces a file nobody knows to look at.
    //
    // A sidecar that cannot be written does not stop the recording. It costs the
    // ability to *repair* a crash, and refusing to record at all because of that
    // trades a certain loss for a possible one.
    mux::RecoveryRecord record;
    record.container = settings.video.container;
    record.output = std::filesystem::absolute(settings.output);
    record.engine_version = std::string{project_version()};
    record.session_id = std::string{log::session_id()};
    record.width = settings.video.width;
    record.height = settings.video.height;
    record.fps = settings.video.fps;
    record.video_codec = config::to_string(settings.video.codec);
    if (audio_encoder != nullptr) {
        if (const AVCodecContext* context = audio_encoder->codec_context(); context != nullptr) {
            record.audio_codec = "aac";
            record.audio_channels = context->ch_layout.nb_channels;
            record.audio_sample_rate = context->sample_rate;
            // So a crash-recovery remux can restore the encoder delay the
            // fragmented file could not record (BUG-021).
            record.audio_initial_padding = context->initial_padding;
        }
    }
    if (const Result<void> written = mux::write_recovery_record(record); !written.has_value()) {
        FC_LOG_WARN(Subsystem::Mux, "recording without a recovery sidecar; a crash will leave a file to repair by hand",
                    LogFields{}.add_error(written.error()));
    }

    impl_->running.store(true, std::memory_order_release);

    // Fresh promises per run. A promise is single-use: reusing one across a
    // second start/stop cycle would throw from `get_future` and, worse, would
    // report the *previous* run's completion instantly.
    impl_->venc_done = std::promise<void>{};
    impl_->mux_done = std::promise<void>{};
    impl_->watchdog_done = std::promise<void>{};
    impl_->venc_finished = impl_->venc_done.get_future();
    impl_->mux_finished = impl_->mux_done.get_future();
    impl_->watchdog_finished = impl_->watchdog_done.get_future();
    impl_->venc_thread = std::thread([impl = impl_.get()] { impl->venc_loop(); });
    impl_->mux_thread = std::thread([impl = impl_.get()] { impl->mux_loop(); });
    impl_->watchdog_thread = std::thread([impl = impl_.get()] { impl->watchdog_loop(); });

    FC_LOG_INFO(Subsystem::Encode, "video pipeline started",
                LogFields{}
                    .add("output", settings.output.string())
                    .add("encoder", settings.encoder_name)
                    .add("hardware_encoder", impl_->encoder->is_hardware())
                    .add("fps", settings.video.fps));
    return ok();
}

Result<void> VideoPipeline::submit(const capture::CaptureFrame& frame) {
    if (!impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    auto* source = static_cast<ID3D11Texture2D*>(frame.texture);
    if (source == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);

    if (!impl_->ring.created()) {
        // Sized one deeper than the queue so a slot is available for the frame
        // being copied while the queue is completely full.
        FC_TRY(impl_->ring.create(impl_->device.Get(), desc, kEncodeQueueCapacity + 1));
    } else if (!impl_->ring.matches(desc)) {
        // SPEC.md §14.3 routes a resolution change to the resize policy; silently
        // scaling here would be the wrong answer.
        return FcError::CAPTURE_RESOLUTION_CHANGED;
    }

    {
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.frames_submitted;
    }

    // SPEC.md §20 row 9's stall detector reads this. Recorded here rather than at the
    // encode stage deliberately: the question row 9 asks is whether *capture* is
    // still producing, and a frame that arrives and is then dropped by queue
    // pressure is not a stall -- it is rung 3's business.
    impl_->last_frame_ns.store(static_cast<std::int64_t>(frame.qpc_ns), std::memory_order_release);

    // SPEC.md §7.1: one epoch, shared by both streams, taken at the moment both
    // are live. Resolving it from whichever stream produced first would offset the
    // other by the difference between the two devices' start-up times -- and since
    // both paths clamp pre-epoch material to zero rather than erroring, that
    // offset would never surface as anything but permanent lip-sync error.
    {
        const std::lock_guard lock(impl_->epoch_mutex);
        impl_->epoch.note_video(static_cast<std::int64_t>(frame.qpc_ns));
        if (!impl_->resolve_epoch_locked()) {
            // Audio has not arrived yet. Dropping rather than pacing against a
            // provisional epoch: a PTS already emitted cannot be re-timed once the
            // real epoch is known.
            const std::lock_guard stats_lock(impl_->stats_mutex);
            ++impl_->stats.frames_awaiting_epoch;
            return ok();
        }
    }

    const int slot = impl_->ring.acquire();
    if (slot < 0) {
        // Every staging slot is still in flight. Dropping here is the deliberate
        // degradation; blocking would violate "never block the capture thread".
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.frames_queue_dropped;
        return ok();
    }

    // Queues a GPU copy. Cheap on the CPU -- it appends a command, it does not
    // wait for the GPU.
    impl_->context->CopyResource(impl_->ring.at(slot), source);

    const auto result = impl_->encode_queue.push(QueuedFrame{slot, static_cast<std::int64_t>(frame.qpc_ns)});
    if (result == BoundedQueue<QueuedFrame>::PushResult::DisplacedOldest) {
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.frames_queue_dropped;
    } else if (result == BoundedQueue<QueuedFrame>::PushResult::Closed) {
        impl_->ring.release(slot);
    }
    return ok();
}

Result<void> VideoPipeline::rebuild_device(ID3D11Device* device, const std::string& encoder_name,
                                           std::uint32_t pool_bind_flags) {
    if (device == nullptr || encoder_name.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (!impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    // The parameter sets the container already committed to. Captured before anything
    // is torn down, because that is what the replacement has to match.
    std::vector<std::uint8_t> original_extradata;
    if (const AVCodecContext* codec = impl_->encoder->codec_context();
        codec != nullptr && codec->extradata != nullptr) {
        original_extradata.assign(codec->extradata, codec->extradata + codec->extradata_size);
    }

    // SPEC.md §5.4 steps 1-2. Stop the venc thread and flush, so every packet the old
    // encoder still owed reaches the muxer *before* the new one writes anything. The
    // finalization deadline applies for BUG-026's reason: this thread can be blocked on
    // a full mux queue and the disk decides how long that takes.
    impl_->encode_queue.close();
    if (!await_worker(impl_->venc_finished, impl_->venc_thread, "fc-venc", Subsystem::Encode,
                      std::chrono::duration_cast<std::chrono::milliseconds>(kFinalizeJoinTimeout))) {
        abandoned_ = true;
        return FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }
    if (const Result<void> flushed = impl_->encoder->flush(); !flushed.has_value()) {
        FC_LOG_ERROR(Subsystem::Encode, "flushing the outgoing encoder failed during a rebuild",
                     LogFields{}.add_error(flushed.error()));
    }
    impl_->drain_encoder_packets();

    // Step 3, in the order that matters: the encoder holds a reference to the device,
    // and the staging ring's textures belong to it. Both go before the device does.
    const int reorder_depth = std::max(0, impl_->settings.video.max_b_frames);
    impl_->encoder.reset();
    impl_->ring.reset();
    impl_->converter = color::Nv12Converter{};
    impl_->context.Reset();
    impl_->device.Reset();

    // Step 5. Everything from here can fail, and a failure leaves the pipeline without
    // an encoder -- so the caller's contract is that a refusal means "close the segment
    // and open the next", never "carry on".
    impl_->device = device;
    device->GetImmediateContext(&impl_->context);
    if (impl_->context.Get() == nullptr) {
        return FcError::GPU_DEVICE_CREATE_FAILED;
    }

    color::ConverterSettings converter_settings;
    converter_settings.width = impl_->settings.video.width;
    converter_settings.height = impl_->settings.video.height;
    converter_settings.range = impl_->settings.video.full_range ? color::ColorRange::Full : color::ColorRange::Limited;
    FC_TRY(impl_->converter.initialize(device, converter_settings));

    const encode::VideoEncoderSettings encoder_settings = impl_->encoder_settings_for(encoder_name, pool_bind_flags);
    FC_TRY_ASSIGN(impl_->encoder, encode::create_video_encoder(encoder_settings));
    FC_TRY(impl_->encoder->open(device, encoder_settings));

    // Step 6's precondition, and the reason this function can refuse. SPEC.md §5.4 was
    // amended on the measurement that two vendors' encoders do not agree on SPS/PPS
    // even with identical settings; the container's copy is already written, and a
    // stream whose parameter sets change mid-file decodes for its first half only.
    std::vector<std::uint8_t> new_extradata;
    if (const AVCodecContext* codec = impl_->encoder->codec_context();
        codec != nullptr && codec->extradata != nullptr) {
        new_extradata.assign(codec->extradata, codec->extradata + codec->extradata_size);
    }
    if (new_extradata != original_extradata) {
        FC_LOG_WARN(Subsystem::Encode,
                    "the rebuilt encoder's parameter sets differ from the container's; this file cannot continue",
                    LogFields{}
                        .add("encoder", encoder_name)
                        .add("original_bytes", static_cast<std::int64_t>(original_extradata.size()))
                        .add("new_bytes", static_cast<std::int64_t>(new_extradata.size()))
                        .add("hint", "SPEC.md §5.4: close the segment and open the next")
                        .add_error(FcError::GPU_MIGRATION_FAILED));
        return FcError::GPU_MIGRATION_FAILED;
    }

    // Step 7's decode-order half. The presentation gap is filled by the pacer's own
    // duplicate logic on the next frame; this is the reservation that keeps the new
    // encoder's first DTS above the last one the muxer accepted.
    impl_->pacer.reserve_discontinuity(reorder_depth);

    if (!impl_->encode_queue.reopen()) {
        // Entries survived the drain. Restarting on top of them would hand the new
        // encoder slots from the dead device's ring.
        FC_LOG_ERROR(Subsystem::Encode, "the encode queue could not be reopened; it did not drain",
                     LogFields{}.add_error(FcError::INTERNAL_INVALID_STATE));
        return FcError::INTERNAL_INVALID_STATE;
    }
    impl_->venc_done = std::promise<void>{};
    impl_->venc_finished = impl_->venc_done.get_future();
    impl_->venc_thread = std::thread([impl = impl_.get()] { impl->venc_loop(); });

    {
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.device_rebuilds;
    }
    FC_LOG_WARN(Subsystem::Gpu, "device stack rebuilt; the recording continues in the same file",
                LogFields{}
                    .add("encoder", encoder_name)
                    .add("reserved_slots", static_cast<std::int64_t>(reorder_depth))
                    .add("extradata_bytes", static_cast<std::int64_t>(new_extradata.size())));
    return ok();
}

Result<mux::ValidationReport> VideoPipeline::stop() {
    if (!impl_->running.exchange(false)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    // The watchdog goes first, before anything it samples starts being torn down. It
    // reads the muxer's latency window, the encode queue's occupancy and the
    // encoder's hardware flag, and every one of those outlives it only because it is
    // stopped here rather than at the end.
    {
        const std::lock_guard lock(impl_->watchdog_mutex);
        impl_->watchdog_exit = true;
    }
    impl_->watchdog_wake.notify_all();
    if (!await_worker(impl_->watchdog_finished, impl_->watchdog_thread, "fc-watchdog", Subsystem::Health)) {
        // A detached watchdog is still reading through `impl_`, so the state has to
        // outlive this object (see `abandoned_`). Not returned as an error, though:
        // the ladder is an observer, and losing the observer must not cost the file.
        // Finalization continues below.
        abandoned_ = true;
    }

    // SPEC.md §10.4 order: stop feeding, flush the encoders, drain the mux queue,
    // write the trailer, flush to disk, validate.
    //
    // Audio first. Its flush pushes the last AAC packets onto the mux queue, and
    // that queue is closed further down -- stopping it after the close would drop
    // the tail of the audio track, silently, on every clean stop.
    if (impl_->loopback_audio || impl_->external_audio) {
        const Result<void> audio_stopped =
            impl_->loopback_audio ? impl_->loopback_audio->stop() : impl_->external_audio->stop();
        if (!audio_stopped.has_value()) {
            if (audio_stopped.error() == FcError::INTERNAL_THREAD_JOIN_TIMEOUT) {
                // A detached audio worker is still reading through `impl_`.
                abandoned_ = true;
                return audio_stopped.error();
            }
            // Anything else is a lost audio tail, not a lost file. The video track
            // and the container are still finalizable, and CLAUDE.md §1 says the
            // recording must survive.
            FC_LOG_ERROR(Subsystem::Audio, "the audio path did not shut down cleanly; finalizing anyway",
                         LogFields{}.add_error(audio_stopped.error()));
        }
    }

    // The per-application tracks, in the same window and for the same reason: each
    // one's flush pushes its last AAC packets onto the mux queue, which is closed
    // below. Track 0 first, deliberately -- SPEC.md §8.6 makes it the track a player
    // showing only one will use, so if a shutdown is going to run out of deadline it
    // should run out on a per-application track rather than on the system mix.
    if (impl_->app_tracks) {
        if (const Result<void> tracks_stopped = impl_->app_tracks->stop(); !tracks_stopped.has_value()) {
            if (tracks_stopped.error() == FcError::INTERNAL_THREAD_JOIN_TIMEOUT) {
                abandoned_ = true;
                return tracks_stopped.error();
            }
            FC_LOG_ERROR(Subsystem::Audio, "the per-application tracks did not shut down cleanly; finalizing anyway",
                         LogFields{}.add_error(tracks_stopped.error()));
        }
    }

    impl_->encode_queue.close();
    // The finalization deadline, not SPEC.md §12's 2 s -- and for the venc thread the
    // reason is indirect enough to be worth stating here too. The mux queue never drops
    // (§12 forbids dropping audio, and an encoded video packet cannot be dropped either
    // without breaking the frames that reference it), so a full mux queue blocks this
    // thread and its exit is gated on the disk. Under BUG-026's 600 ms stalls that is
    // the deadline that actually fired, and the file was left unfinalized.
    if (!await_worker(impl_->venc_finished, impl_->venc_thread, "fc-venc", Subsystem::Encode,
                      std::chrono::duration_cast<std::chrono::milliseconds>(kFinalizeJoinTimeout))) {
        abandoned_ = true;
        return FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }

    if (impl_->encoder) {
        const Result<void> flushed = impl_->encoder->flush();
        if (!flushed.has_value()) {
            FC_LOG_ERROR(Subsystem::Encode, "flushing the encoder failed; finalizing anyway",
                         LogFields{}.add_error(flushed.error()));
        }
        // Delayed frames come out only now. Pushed from this thread because the
        // venc thread has already exited.
        impl_->drain_encoder_packets();
    }

    impl_->mux_queue.close();
    // The finalization deadline, not SPEC.md §12's 2 s. The mux thread has up to 64
    // queued packets to put on the platter here, and on a volume slow enough to engage
    // §13 rung 6 that legitimately takes seconds -- holding it to 2 s meant a slow disk
    // produced an unfinalized file (BUG-026). Still bounded; see `kFinalizeJoinTimeout`.
    if (!await_worker(impl_->mux_finished, impl_->mux_thread, "fc-mux", Subsystem::Mux,
                      std::chrono::duration_cast<std::chrono::milliseconds>(kFinalizeJoinTimeout))) {
        abandoned_ = true;
        return FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }

    const Result<void> finalized = impl_->muxer->finalize();
    if (!finalized.has_value()) {
        return finalized.error();
    }

    {
        const std::lock_guard lock(impl_->stats_mutex);
        impl_->stats.packets_muxed = impl_->muxer->packets_written();
        impl_->stats.bytes_written = impl_->muxer->bytes_written();
        impl_->stats.last_muxed_pts_ns = impl_->muxer->last_video_pts_ns();
    }

    mux::ValidationExpectation expectation;
    // From the pacer's own timeline, not `emitted() / fps`. The two are equal only
    // while the frame rate is constant, and SPEC.md §13 rung 3 exists to change it:
    // after a 60 -> 30 retime the file's length divides by neither rate, and the
    // dividing form made this gate reject a correctly degraded recording (BUG-025).
    //
    // Minus the last segment's origin, because the file being validated is that segment
    // and not the recording (SPEC.md §11). Getting this wrong would have the gate compare
    // a two-minute file against a two-hour recording and reject every segmented
    // recording's last file -- BUG-025's shape a third time, and the reason the
    // subtraction is here rather than left implicit.
    expectation.duration_seconds =
        impl_->pacer.timeline_seconds() - (static_cast<double>(impl_->segment_origin_ns) / 1e9);
    // The gate is told what was recorded, so it can check the file against that
    // rather than only against itself. A missing audio track is a failed recording
    // even when every video frame decodes (BUG-015).
    expectation.audio = impl_->settings.audio.source != AudioSource::None;
    // And how many tracks. SPEC.md §8.6 forbids dropping one from the container, so
    // the count the muxer opened with is the count the file must come back with --
    // a Tier B recording missing a track is not a recording that mostly worked.
    expectation.audio_streams = impl_->muxer->audio_stream_count();
    // SPEC.md §8.5's layout pin, per stream (amended 2026-08-06). Taken from the
    // per-application encoders rather than from a constant, so the gate checks what was
    // actually opened rather than what this file believes §8.6 says.
    if (impl_->app_tracks) {
        for (const encode::AacEncoder* track : impl_->app_tracks->encoders()) {
            if (track == nullptr) {
                continue;
            }
            if (const AVCodecContext* context = track->codec_context(); context != nullptr) {
                expectation.app_track_channels = context->ch_layout.nb_channels;
                break;
            }
        }
    }
    int audio_initial_padding = 0;
    if (const encode::AacEncoder* audio_encoder = impl_->audio_encoder(); audio_encoder != nullptr) {
        if (const AVCodecContext* context = audio_encoder->codec_context(); context != nullptr) {
            expectation.audio_channels = context->ch_layout.nb_channels;
            // Carried to the remux by hand. A fragmented MP4 cannot record it, so
            // this is the only copy that survives the recording (BUG-021).
            audio_initial_padding = context->initial_padding;
        }
    }

    // SPEC.md §10.4's order: trailer, flush, **remux if MP4**, validate. The remux
    // validates the progressive file before swapping it in, so what comes back here
    // already describes the file that ends up on disk (SPEC.md §10.3).
    // The *last* file, which is `settings.output` only when nothing split (SPEC.md §11).
    const std::filesystem::path& final_path = impl_->current_segment_path;
    const bool remuxed = impl_->muxer->needs_remux();
    const mux::FinalizeProgressFn& on_progress = impl_->settings.on_finalize_progress;

    // MKV takes the `validate` branch, which has no remux and therefore no phase
    // boundaries of its own to report. Announced here so the phase is still `Validating`
    // rather than nothing at all for the ~650-1000 ms BUG-046 measured it at -- an MKV
    // stop is not instant, and a GUI told nothing would show an empty bar for a second.
    if (!remuxed && on_progress) {
        on_progress(mux::FinalizeProgress{mux::FinalizePhase::Validating,
                                          mux::finalize_percent(mux::FinalizePhase::Validating, 0.0), 0, 0});
    }
    FC_TRY_ASSIGN(const mux::ValidationReport report,
                  remuxed ? mux::finalize_in_place(final_path, expectation, audio_initial_padding, on_progress)
                          : mux::Muxer::validate(final_path, expectation));
    if (!report.valid) {
        // Returned rather than collapsed into a bare error code. The caller needs
        // to know *why* the file did not validate -- "no decodable frames" and
        // "duration is 8% short" call for completely different responses, and
        // `report.valid` is right there to be checked.
        FC_LOG_ERROR(Subsystem::Mux, "output failed validation",
                     LogFields{}.add("detail", report.detail).add_error(FcError::MUX_VALIDATION_FAILED));
        return report;
    }

    // Only now. The sidecar is the claim that this recording is unfinished, and it
    // is retracted when the file has been proven finished -- not when the trailer
    // was written, and not when the remux returned.
    mux::clear_recovery_record(impl_->settings.output);

    // SPEC.md §11's sidecar, written after the last file has validated so it never
    // describes a recording that turned out to be broken. Only for a recording that
    // actually segmented -- a single-file recording has nothing to concatenate.
    if (impl_->segments.enabled()) {
        std::vector<SegmentRecord> records;
        {
            const std::lock_guard lock(impl_->stats_mutex);
            records = impl_->completed_segments;
        }
        records.push_back(SegmentRecord{final_path, impl_->segment_origin_ns,
                                        static_cast<std::int64_t>(impl_->pacer.timeline_seconds() * 1e9)});
        write_segments_sidecar(impl_->settings.output, records);
    }

    FC_LOG_INFO(Subsystem::Mux, "recording finalized and validated",
                LogFields{}
                    .add("path", final_path.string())
                    .add("segments", static_cast<std::int64_t>(impl_->segments.index()))
                    .add("remuxed", remuxed)
                    .add("duration_s", report.duration_seconds)
                    .add("decoded_frames", report.decoded_frames)
                    .add("audio_codec", report.audio_codec)
                    .add("audio_channels", report.audio_channels)
                    .add("audio_duration_s", report.audio_duration_seconds)
                    .add("format", report.format_name));

    // SPEC.md §7.5: "Total paused duration is reported in `get_stats` and logged at
    // finalization, because 'why is my 30-minute recording 12 minutes long' must be
    // answerable from the log." Emitted only when the recording was actually paused, so
    // it stays a signal rather than a line on every finalization.
    //
    // `stragglers` is here rather than only in the stats because it is the evidence
    // that the pause quiesce held, and a log is what a bug report carries.
    if (impl_->pause_clock.pauses() > 0 || impl_->pause_clock.paused()) {
        FC_LOG_INFO(Subsystem::Encode, "recording included paused time",
                    LogFields{}
                        .add("pauses", static_cast<std::int64_t>(impl_->pause_clock.pauses()))
                        .add("paused_total_ms", impl_->pause_clock.paused_total_ns() / 1'000'000)
                        .add("frames_excised", static_cast<std::int64_t>(impl_->pacer.excised()))
                        .add("stragglers", static_cast<std::int64_t>(impl_->pause_clock.stragglers()))
                        .add("timeline_s", impl_->pacer.timeline_seconds()));
    }

    // The ladder's summary for the whole recording, on every recording. SPEC.md §13
    // makes degradation "automatic, logged and surfaced", and a recording that
    // degraded and recovered leaves no other trace once the transitions have scrolled
    // past.
    const PipelineHealth final_health = health();
    FC_LOG_INFO(Subsystem::Health, "degradation summary",
                LogFields{}
                    .add("final_rung", static_cast<std::int64_t>(final_health.rung))
                    .add("transitions", static_cast<std::int64_t>(final_health.rung_transitions))
                    .add("retimes", static_cast<std::int64_t>(final_health.retimes))
                    .add("worst_drop_ratio", final_health.drop_ratio)
                    .add("write_p99_us", final_health.disk_write_p99_ns / 1000)
                    .add("stall_episodes", static_cast<std::int64_t>(final_health.stall_episodes))
                    .add("worst_stall_us", final_health.worst_stall_ns / 1000)
                    .add("hardware_encoder", final_health.hardware_encoder));
    return report;
}

void VideoPipeline::note_first_audio(std::int64_t qpc_ns) {
    impl_->note_first_audio(qpc_ns);
}

void VideoPipeline::offer_audio(const audio::LoopbackBuffer& buffer) {
    if (!impl_->external_audio) {
        return;
    }
    // The first buffer resolves the audio half of the epoch, exactly as the WASAPI
    // sink does for `SystemLoopback`. Done here rather than in `AudioEncodePath`
    // so the seam behaves identically from both sides.
    if (!impl_->external_notified.exchange(true, std::memory_order_acq_rel)) {
        impl_->note_first_audio(buffer.qpc_ns);
    }
    impl_->external_audio->offer(buffer);
}

void VideoPipeline::offer_app_audio(int index, const audio::LoopbackBuffer& buffer) {
    if (!impl_->app_tracks) {
        return;
    }
    // Deliberately **not** a route to `note_first_audio`. SPEC.md §8.6 makes track 0
    // the system mix and §7.1 makes `t0` the moment both streams are live; letting a
    // per-application track resolve the epoch would make the recording's origin
    // depend on when an application happened to make a sound, and every other track
    // would then be padded from that. Track 0 is always present and always the one
    // that speaks for audio.
    impl_->app_tracks->offer(index, buffer);
}

void VideoPipeline::tick_audio_silence(std::int64_t now_ns) {
    if (impl_->external_audio) {
        if (impl_->external_audio->silence_due_at(now_ns)) {
            impl_->external_audio->request_silence(now_ns);
        }
    }
    // Every per-application track gets the same instant. SPEC.md §8.6 makes silence
    // "the steady state" on these, so this is their primary path and not a fallback:
    // in an ordinary recording most tracks are fed entirely from here.
    if (impl_->app_tracks && impl_->settings.audio.multitrack.source == AppTrackSource::External) {
        impl_->app_tracks->tick(now_ns);
    }
}

PipelineStats VideoPipeline::stats() const {
    const std::lock_guard lock(impl_->stats_mutex);
    PipelineStats copy = impl_->stats;
    // Read straight off the pacer rather than mirrored into `stats` on every
    // frame: these are two loads, and the venc thread's hot path should not pay
    // for a counter nobody reads per frame.
    copy.worst_staleness_ns = impl_->pacer.worst_staleness_ns();
    copy.mean_staleness_ns = impl_->pacer.mean_staleness_ns();
    copy.frames_excised = impl_->pacer.excised();

    // SPEC.md §7.5's reporting requirement. Off the clock rather than mirrored, for
    // the same reason as the two above.
    copy.paused_total_ns = impl_->pause_clock.paused_total_ns();
    copy.pauses = impl_->pause_clock.pauses();
    copy.pause_stragglers = impl_->pause_clock.stragglers();
    return copy;
}

PipelineHealth VideoPipeline::health() const {
    const std::lock_guard lock(impl_->health_mutex);
    return impl_->health;
}

AudioStats VideoPipeline::audio_stats() const {
    if (impl_->loopback_audio) {
        return impl_->loopback_audio->stats();
    }
    if (impl_->external_audio) {
        return impl_->external_audio->stats();
    }
    return AudioStats{};
}

AppTracksStats VideoPipeline::app_track_stats() const {
    if (!impl_->app_tracks) {
        return AppTracksStats{};
    }
    return impl_->app_tracks->stats();
}

bool VideoPipeline::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

std::filesystem::path VideoPipeline::current_output() const {
    // Under the stats lock: the mux thread reassigns this at every rollover.
    const std::lock_guard lock(impl_->stats_mutex);
    return impl_->current_segment_path;
}

bool VideoPipeline::paused() const noexcept {
    return impl_->pause_clock.paused();
}

std::int64_t VideoPipeline::paused_total_ns() const noexcept {
    return impl_->pause_clock.paused_total_ns();
}

Result<void> VideoPipeline::pause() {
    return pause_at(timing::qpc_now_ns());
}

Result<void> VideoPipeline::resume() {
    return resume_at(timing::qpc_now_ns());
}

Result<void> VideoPipeline::pause_at(std::int64_t now_ns) {
    if (!impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (impl_->pause_clock.paused()) {
        return ok(); // SPEC.md §7.5: idempotent, and a no-op that succeeds.
    }

    // The quiesce, and it happens *before* the clock is told, not after. Everything
    // still in the encode queue was captured on the unpaused timeline and has to be
    // placed against the paused total as it stands now. Draining it here makes that
    // true by construction; draining it after publishing the pause would leave a
    // window in which a frame could be mapped against a total that had already grown.
    //
    // Bounded, because a venc thread that cannot drain must not make `pause_record`
    // hang -- the deadline is generous next to a queue that normally empties in
    // single-digit milliseconds, and `PipelineStats::pause_stragglers` reports the
    // exact cost when it is not met rather than leaving it unknown.
    constexpr auto kDrainDeadline = std::chrono::milliseconds{250};
    const auto deadline = std::chrono::steady_clock::now() + kDrainDeadline;
    while (impl_->encode_queue.size() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const std::size_t undrained = impl_->encode_queue.size();
    if (undrained > 0) {
        FC_LOG_WARN(Subsystem::Encode, "pause did not fully quiesce the encode queue within the deadline",
                    LogFields{}
                        .add("queued_frames", static_cast<std::int64_t>(undrained))
                        .add("deadline_ms", static_cast<std::int64_t>(kDrainDeadline.count())));
    }

    if (!impl_->pause_clock.pause(now_ns)) {
        return ok(); // raced another pause; still idempotent
    }

    FC_LOG_INFO(Subsystem::Encode, "recording paused",
                LogFields{}
                    .add("qpc_ns", now_ns)
                    .add("timeline_s", impl_->pacer.timeline_seconds())
                    .add("paused_total_ms", impl_->pause_clock.paused_total_ns() / 1'000'000)
                    .add("drained_frames", static_cast<std::int64_t>(undrained == 0)));
    return ok();
}

Result<void> VideoPipeline::resume_at(std::int64_t now_ns) {
    if (!impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (!impl_->pause_clock.paused()) {
        return ok(); // idempotent
    }

    if (!impl_->pause_clock.resume(now_ns)) {
        return ok();
    }

    // SPEC.md §7.5: "Force an IDR: content has jumped and a P-frame referencing
    // pre-pause content is a visible smear." Published for the venc thread, which
    // consumes it in `apply_ladder_request` before the next `pacer.decide`.
    impl_->idr_requested.store(true, std::memory_order_release);

    FC_LOG_INFO(Subsystem::Encode, "recording resumed",
                LogFields{}
                    .add("qpc_ns", now_ns)
                    .add("timeline_s", impl_->pacer.timeline_seconds())
                    .add("paused_total_ms", impl_->pause_clock.paused_total_ns() / 1'000'000)
                    .add("pauses", static_cast<std::int64_t>(impl_->pause_clock.pauses())));
    return ok();
}

} // namespace fc::pipeline
