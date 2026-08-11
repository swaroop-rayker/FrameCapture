#include "core/pipeline/audio_path.h"

#include "core/audio/drift_compensator.h"
#include "core/audio/resampler.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"
#include "core/util/bounded_queue.h"
#include "core/util/thread_utils.h"
#include "core/util/worker.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace fc::pipeline {
namespace {

/// 64 endpoint buffers -- about 1.3 s at SPEC.md §8.1's 20 ms period. Deep enough
/// that an ordinary scheduling hiccup on the `aenc` thread never reaches the audio
/// thread, shallow enough that a genuinely wedged encoder is visible in
/// `queue_waits` within a couple of seconds rather than after a gigabyte.
constexpr std::size_t kAudioQueueCapacity = 64;

/// Pooled input buffers kept alive between uses. One per queue slot plus a margin
/// for the buffer being filled while the queue is full.
constexpr std::size_t kPooledBuffers = kAudioQueueCapacity + 4;

/// Bound on the drain-and-retry loop around a full encoder queue. A genuinely
/// wedged encoder has to surface as an error rather than as a silent spin.
constexpr int kMaxEncoderStalls = 64;

/// Sentinel for "the session has not resolved `t0` yet". QPC is monotonic from
/// boot, so a real timestamp is never negative.
constexpr std::int64_t kNoEpoch = -1;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// Frames at `rate` as nanoseconds, in integers.
[[nodiscard]] std::int64_t frames_to_ns(std::int64_t frames, int rate) noexcept {
    if (rate <= 0) {
        return 0;
    }
    const std::int64_t seconds = frames / rate;
    const std::int64_t remainder = frames % rate;
    return (seconds * kNsPerSecond) + ((remainder * kNsPerSecond) / rate);
}

/// One unit of work on its way from the `audio` or `silence` thread to `aenc`.
struct AudioWorkItem {
    enum class Kind {
        /// A buffer the endpoint produced.
        Buffer,
        /// The watchdog noticed the endpoint has gone quiet (SPEC.md §8.2). Carries
        /// only a timestamp: how much silence that implies is a question about the
        /// timeline, and the timeline belongs to `aenc`.
        SilenceTick,
        /// The endpoint changed (SPEC.md §14.1). Carries the new format and the instant
        /// the new timeline begins. Queued rather than applied directly so it lands in
        /// order with the buffers around it -- the four components `aenc` owns are not
        /// thread-safe, and that is only safe because nothing else touches them.
        MigrateInput,
    };

    Kind kind = Kind::Buffer;
    std::int64_t qpc_ns = 0;
    std::int64_t frames = 0;
    /// `MigrateInput` only: the new endpoint's negotiated format.
    audio::MixFormat new_format;
    /// `IAudioClock2::GetDevicePosition`, for SPEC.md §8.3's cross-check. Zero when
    /// the endpoint offered no clock interface.
    std::int64_t device_position_frames = 0;
    audio::PacketFlags flags;
    /// Interleaved endpoint-format samples, copied out of WASAPI's buffer because
    /// it is released the moment the sink returns. Empty for a silent buffer or a
    /// tick.
    std::vector<std::uint8_t> bytes;
    /// Bytes per frame in `bytes`, taken from the buffer that produced them.
    ///
    /// The item is self-describing on purpose (BUG-031). `offer` runs on the audio
    /// thread and `apply_migration` on `aenc`, so any shared notion of "the current
    /// input format" is read and written by different threads and is wrong for
    /// precisely the items queued across a migration.
    int bytes_per_frame = 0;
};

/// Recycles the byte buffers so the steady state allocates nothing.
///
/// A `std::vector` that is resized within its existing capacity does not
/// reallocate, so after the first few buffers the audio thread's copy is a
/// `memcpy` into memory it already owns -- which is what CLAUDE.md hard rule 4
/// asks for.
class BytePool {
public:
    [[nodiscard]] std::vector<std::uint8_t> acquire(std::size_t bytes) {
        std::vector<std::uint8_t> buffer;
        {
            const std::lock_guard lock(mutex_);
            if (!free_.empty()) {
                buffer = std::move(free_.back());
                free_.pop_back();
            }
        }
        buffer.resize(bytes);
        return buffer;
    }

    void release(std::vector<std::uint8_t>&& buffer) {
        if (buffer.capacity() == 0) {
            return;
        }
        const std::lock_guard lock(mutex_);
        if (free_.size() < kPooledBuffers) {
            free_.push_back(std::move(buffer));
        }
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> free_;
};

} // namespace

// ---------------------------------------------------------------------------
// AudioEncodePath
// ---------------------------------------------------------------------------

struct AudioEncodePath::Impl {
    Impl() = default;

    ~Impl() {
        av_channel_layout_uninit(&pinned);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    AudioEncodeSettings settings;
    AudioPacketSink sink;

    // Owned exclusively by the `aenc` thread once the thread is running. None of
    // these four is thread-safe, and that is fine precisely because nothing else
    // touches them.
    audio::AudioTimeline timeline;
    audio::DriftCompensator drift;
    audio::Resampler resampler;
    encode::AacEncoder encoder;

    AVChannelLayout pinned{};
    int input_rate = 48000;

    /// The recording's shared paused total (SPEC.md §7.5), or null when the recording
    /// cannot pause. Held here as well as inside `timeline` because §14.1's migration
    /// builds a *fresh* timeline, and a new timeline with no clock attached would
    /// silently stop excising paused time from the audio half of the file -- which is
    /// §7.5's exact failure mode, reachable through a code path that has nothing to do
    /// with pause.
    const timing::PauseClock* pause_clock = nullptr;

    /// Samples handed to the AAC encoder, in canonical 48 kHz frames. This is the
    /// audio track's length, and SPEC.md §8.4's drift is measured on it.
    std::int64_t canonical_written = 0;

    // SPEC.md §8.3's device-position cross-check. Anchored on the first buffer
    // that carried a position rather than on `t0`, because the endpoint's counter
    // has its own origin and only its *rate* is being compared.
    std::int64_t device_anchor_frames = -1;
    std::int64_t device_anchor_qpc_ns = 0;
    std::int64_t last_device_log_ns = 0;

    /// SPEC.md §14.1. Every timeline this recording has retired, in seconds -- the
    /// current one's `frames_written` covers only the stretch since the last migration.
    double timeline_seconds_total = 0.0;
    /// Silence from retired timelines, banked in seconds for the same unit reason.
    double silence_seconds_total = 0.0;
    std::atomic<std::uint64_t> migrations{0};

    void apply_migration(const AudioWorkItem& item);

    BoundedQueue<AudioWorkItem> queue{kAudioQueueCapacity, QueuePolicy::Block};
    BytePool pool;
    /// Latches the "no frame size" report so it is logged once rather than per buffer.
    /// See `offer`.
    std::atomic_flag unsized_buffer_reported;

    std::thread aenc_thread;
    std::promise<void> aenc_done;
    std::future<void> aenc_finished;

    std::atomic<bool> opened{false};
    std::atomic<bool> stopped{false};
    std::atomic<std::int64_t> t0_ns{kNoEpoch};
    std::atomic<std::int64_t> last_offer_qpc_ns{0};
    std::atomic<std::int64_t> first_packet_qpc_ns{0};
    std::atomic<std::uint64_t> buffers_offered{0};
    std::atomic<std::uint64_t> buffers_before_epoch{0};
    std::atomic<std::uint64_t> silence_ticks{0};
    std::atomic<std::uint64_t> queue_waits{0};

    mutable std::mutex stats_mutex;
    AudioStats stats;

    /// Work items that arrived before `t0` was published. See `process`.
    std::vector<AudioWorkItem> held;

    void aenc_loop();
    void process(AudioWorkItem& item);
    /// Puts one item on the timeline. Only reachable once the epoch is known.
    void place(AudioWorkItem& item);
    [[nodiscard]] Result<void> emit_audio(const std::uint8_t* data, std::int64_t frames);
    [[nodiscard]] Result<void> emit_silence(std::int64_t frames);
    [[nodiscard]] Result<void> feed(const AVFrame* frame);
    void drain_packets();
    void evaluate_drift(std::int64_t now_ns);
    void cross_check_device_clock(std::int64_t qpc_ns, std::int64_t device_position_frames);
    void publish_stats();
    [[nodiscard]] Result<void> finish();
};

void AudioEncodePath::Impl::drain_packets() {
    for (;;) {
        auto received = encoder.receive();
        if (!received.has_value()) {
            const std::lock_guard lock(stats_mutex);
            ++stats.encode_failures;
            return;
        }
        // `Result::value() &&` is the only overload that yields an rvalue;
        // EncodedPacket is move-only.
        std::optional<encode::EncodedPacket> packet = std::move(received).value();
        if (!packet.has_value()) {
            return; // the encoder wants more input, or the stream is drained
        }
        {
            const std::lock_guard lock(stats_mutex);
            ++stats.packets_encoded;
        }
        if (sink) {
            sink(std::move(*packet));
        }
    }
}

Result<void> AudioEncodePath::Impl::feed(const AVFrame* frame) {
    if (frame == nullptr || frame->nb_samples <= 0) {
        return ok();
    }

    canonical_written += frame->nb_samples;

    int offset = 0;
    int stalls = 0;
    while (offset < frame->nb_samples) {
        FC_TRY_ASSIGN(const int consumed, encoder.submit(frame, offset));
        offset += consumed;

        // Draining is what releases the encoder's output queue, which is the only
        // thing a short return is waiting on.
        drain_packets();

        if (consumed == 0) {
            if (++stalls >= kMaxEncoderStalls) {
                FC_LOG_ERROR(Subsystem::Encode, "the AAC encoder stayed blocked after draining",
                             LogFields{}.add("attempts", kMaxEncoderStalls).add_error(FcError::ENCODE_SUBMIT_FAILED));
                return FcError::ENCODE_SUBMIT_FAILED;
            }
        } else {
            stalls = 0;
        }
    }
    return ok();
}

Result<void> AudioEncodePath::Impl::emit_audio(const std::uint8_t* data, std::int64_t frames) {
    if (data == nullptr || frames <= 0) {
        return ok();
    }
    FC_TRY_ASSIGN(const AVFrame* converted, resampler.convert(data, frames));
    return feed(converted);
}

Result<void> AudioEncodePath::Impl::emit_silence(std::int64_t frames) {
    // Chunked because a silent desktop can ask for minutes at once and the zero
    // buffer behind `convert_silence` is preallocated at a fixed size.
    std::int64_t remaining = frames;
    while (remaining > 0) {
        const std::int64_t chunk = std::min(remaining, audio::kSilenceChunkFrames);
        FC_TRY_ASSIGN(const AVFrame* converted, resampler.convert_silence(chunk));
        FC_TRY(feed(converted));
        remaining -= chunk;
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Drift (SPEC.md §8.4)
// ---------------------------------------------------------------------------
//
// > Compute `drift = (audio_samples_written / sample_rate) - (qpc_elapsed_seconds)`
// > on a 1 s cadence.
//
// `audio_samples_written` is **samples handed to the encoder**, not samples the
// device delivered. SPEC.md §8.4 states this since M4's ratification -- §8.2's
// timeline is the correction and this ladder is the measurement -- and the
// distinction is what makes the loop converge.
// The two differ because §8.2's timeline sits between them: it places every
// packet at the frame index its QPC timestamp maps to, fills gaps with silence of
// exactly the missing duration, and trims a packet that overlaps ground already
// written. A device clock 50 ppm fast is absorbed there as roughly two discarded
// samples a second, and never reaches the encoder as accumulated length.
//
// So on a healthy recording this measurement sits near zero and the ladder never
// leaves band 0 -- which is the correct outcome, not a dead loop. It is measuring
// the invariant §8.2 exists to hold: **the audio track is as long as the wall
// clock says it should be.** If that ever stops being true, this is what notices,
// and `swr_set_compensation` acts on the same quantity it measures, so a
// correction shows up in the next measurement and the loop settles instead of
// asking for the same correction forever.
//
// Measuring the *device* instead would be a different quantity -- the endpoint's
// crystal error -- which grows without bound by design and would drive the ladder
// into its hard band on perfectly healthy hardware, correcting an error the
// timeline has already absorbed.
void AudioEncodePath::Impl::evaluate_drift(std::int64_t now_ns) {
    const std::int64_t t0 = t0_ns.load(std::memory_order_acquire);
    if (t0 == kNoEpoch) {
        return;
    }

    // **Timeline elapsed, not wall clock** (BUG-038). Both sides of this subtraction have
    // to be measured on the same clock, and `canonical_written` is the encoded track,
    // which SPEC.md §7.5 has already had paused time excised from. Passing raw
    // `qpc - t0` against it reports the pause itself as drift -- exactly, to the
    // microsecond -- and a 2 s pause lands 2 s outside a 40 ms band, so the ladder
    // hard-resyncs every second for the rest of the recording and injects tens of
    // thousands of correction frames into a file that was correct.
    //
    // A paused span is not measured at all rather than measured against a frozen
    // timeline: neither term is advancing, so there is nothing a correction could
    // converge on, and `due_at`'s cadence would fire on the first buffer after the
    // resume regardless.
    std::int64_t elapsed = now_ns - t0;
    if (pause_clock != nullptr) {
        const timing::PauseClock::Mapping mapped = pause_clock->observe(now_ns, t0);
        if (mapped.state == timing::PauseClock::State::Excised) {
            return;
        }
        elapsed = mapped.timeline_ns;
    }

    if (elapsed <= 0 || !drift.due_at(elapsed)) {
        return;
    }

    const audio::DriftDecision decision = drift.evaluate(canonical_written, elapsed);

    switch (decision.action) {
    case audio::DriftAction::None:
        break;

    case audio::DriftAction::SoftResync:
        // Both arguments are in output samples, which is what the compensator was
        // constructed to produce -- see `open`.
        if (const Result<void> applied =
                resampler.set_compensation(decision.compensation_samples, decision.compensation_distance);
            !applied.has_value()) {
            FC_LOG_WARN(Subsystem::Audio, "soft resync was refused; drift will be re-measured next second",
                        LogFields{}.add("drift_us", decision.drift_ns / 1000).add_error(applied.error()));
        } else {
            FC_LOG_DEBUG(Subsystem::Audio, "soft resync applied",
                         LogFields{}
                             .add("drift_us", decision.drift_ns / 1000)
                             .add("samples", decision.compensation_samples)
                             .add("distance", decision.compensation_distance));
        }
        break;

    case audio::DriftAction::HardResync:
        // SPEC.md §8.4: "this should essentially never fire; if it does, it is a
        // bug report, not a normal event."
        //
        // The insert-or-drop it calls for is already the timeline's behaviour --
        // `accept` fills a gap with silence of exactly the missing duration and
        // trims an overlapping packet, both against the QPC timestamp. Reaching
        // this band means that mechanism did not hold, so what is useful here is
        // the report rather than a second, weaker correction on top.
        //
        // Deviation flagged rather than reinterpreted (CLAUDE.md §7): SPEC.md §8.4
        // specifies the insert or drop at a *zero crossing*, and the timeline does
        // it at a WASAPI buffer boundary. Aligning to a zero crossing needs a
        // lookahead window the timeline does not keep.
        FC_LOG_ERROR(Subsystem::Audio, "the audio track's length left the soft-correctable band",
                     LogFields{}
                         .add("event", "AUDIO_HARD_RESYNC")
                         .add("drift_us", decision.drift_ns / 1000)
                         .add("correction_frames", decision.hard_correction_frames)
                         .add("elapsed_s", elapsed / kNsPerSecond));
        break;
    }
}

// SPEC.md §8.3's cross-check.
//
// > Use the `u64QPCPosition` returned by `IAudioCaptureClient::GetBuffer` as the
// > authoritative packet timestamp. [...] Cross-check against
// > `IAudioClock2::GetDevicePosition` and log the delta at 1 Hz as a drift
// > telemetry signal.
//
// Two independent answers to "how much time has passed": QPC, which the whole
// engine is timed against (SPEC.md §7.1), and the endpoint's own frame counter.
// Nothing here corrects anything -- the timeline already places audio by QPC, and
// this is the measurement that says whether the two clocks agree about how fast
// time is going. A machine whose endpoint runs 50 ppm fast shows a few tens of
// microseconds per minute and nothing else changes.
//
// Anchored on the first buffer that carried a position, not on `t0`: the
// endpoint's counter has its own origin and only its rate is being compared.
//
// **This one takes raw QPC across a pause, and that is correct** -- checked while
// fixing BUG-038, because it looks like it should have the same defect and does not.
// Both sides here are wall-clock rate measurements of things that keep running while
// the recording is paused: the endpoint does not stop clocking frames because the user
// pressed pause, and neither does QPC. Excising paused time from `elapsed_ns` alone
// would subtract from one side of a comparison whose other side still contains it, and
// manufacture a delta of exactly the pause duration -- which is BUG-038's failure, in
// mirror image. Measured across a 2 s pause on the reference rig: `device_vs_qpc_us`
// stayed inside ±550 µs throughout, while §8.4's ladder read −2,021,263 µs.
//
// The distinction is what each quantity is compared against. §8.4 measures the
// *encoded track*, which paused time never reaches, so its reference is the timeline.
// §8.3 measures the *device's crystal*, which paused time does reach, so its reference
// is wall clock. `AudioPathTest.TheDeviceCrossCheckIsUnaffectedByAPause` pins it.
//
// Logged at DEBUG rather than INFO. SPEC.md §18 calls it telemetry and asks for
// 1 Hz, which over the four-hour soak of §20.1 is 14,400 lines; the value is
// carried in `AudioStats` at every log level and summarised once at INFO when the
// path finishes, so nothing is lost by keeping the per-second line out of a
// default-level log.
void AudioEncodePath::Impl::cross_check_device_clock(std::int64_t qpc_ns, std::int64_t device_position_frames) {
    if (device_position_frames <= 0) {
        // The endpoint offered no `IAudioClock2`. `LoopbackCapture` logs that once
        // at start-up; there is nothing to say about it per buffer.
        return;
    }

    if (device_anchor_frames < 0) {
        device_anchor_frames = device_position_frames;
        device_anchor_qpc_ns = qpc_ns;
        last_device_log_ns = qpc_ns;
        return;
    }

    const std::int64_t device_frames = device_position_frames - device_anchor_frames;
    const std::int64_t elapsed_ns = qpc_ns - device_anchor_qpc_ns;
    if (device_frames < 0 || elapsed_ns <= 0) {
        // The counter went backwards, which means the endpoint was reset under us.
        // Re-anchor rather than reporting a nonsense delta.
        device_anchor_frames = device_position_frames;
        device_anchor_qpc_ns = qpc_ns;
        return;
    }

    const std::int64_t delta_ns = frames_to_ns(device_frames, input_rate) - elapsed_ns;
    {
        const std::lock_guard lock(stats_mutex);
        stats.device_clock_delta_ns = delta_ns;
        stats.device_position_frames = device_frames;
    }

    if (qpc_ns - last_device_log_ns < audio::kDriftCadenceNs) {
        return;
    }
    last_device_log_ns = qpc_ns;

    FC_LOG_DEBUG(Subsystem::Audio, "endpoint clock cross-check",
                 LogFields{}
                     .add("device_vs_qpc_us", delta_ns / 1000)
                     .add("device_frames", device_frames)
                     .add("elapsed_s", elapsed_ns / kNsPerSecond)
                     .add("track_vs_qpc_us", drift.last_drift_ns() / 1000));
}

void AudioEncodePath::Impl::publish_stats() {
    const std::lock_guard lock(stats_mutex);
    stats.frames_written = timeline.frames_written();
    stats.silence_frames_injected = timeline.silence_frames_injected();
    stats.discontinuities = timeline.discontinuities();
    stats.timeline_drops = timeline.dropped_packets();
    stats.last_drift_ns = drift.last_drift_ns();
    stats.worst_drift_ns = drift.worst_drift_ns();
    stats.soft_resyncs = drift.soft_resyncs();
    stats.hard_resyncs = drift.hard_resyncs();
    stats.input_migrations = migrations.load(std::memory_order_relaxed);
    // Retired timelines plus the one running. Summed in seconds because the running
    // one's unit is the *current* endpoint's rate and the retired ones' may not be
    // (SPEC.md §14.1) -- adding the frame counts would add two different units.
    const int rate = timeline.sample_rate() > 0 ? timeline.sample_rate() : 48000;
    stats.timeline_seconds =
        timeline_seconds_total + (static_cast<double>(timeline.frames_written()) / static_cast<double>(rate));
    stats.silence_seconds =
        silence_seconds_total + (static_cast<double>(timeline.silence_frames_injected()) / static_cast<double>(rate));
}

// Buffers that arrive before the session has published `t0` are **held**, not
// discarded (BUG-016).
//
// The epoch is resolved by whichever stream produces second, so between the first
// audio buffer and the first video frame there is a window in which `aenc` is
// running and `t0` is not yet known. Dropping what arrives in it looks harmless --
// `t0` is the *later* of the two firsts (SPEC.md §7.1), so surely anything earlier
// predates it? -- and is not: the window is bounded by wall time, but which
// buffers fall inside it is decided by thread scheduling. A buffer stamped at or
// after `t0` can still be dequeued before `t0` is published, and dropping it
// removes audio that belongs on the timeline.
//
// So the decision is deferred to the one component that can make it on evidence:
// `AudioTimeline::accept` already drops a packet entirely before `t0` and trims
// one that straddles it, both from the timestamp rather than from arrival order
// (BUG-014). Holding costs a bounded stash and makes the outcome independent of
// when the scheduler happened to run this thread.
void AudioEncodePath::Impl::process(AudioWorkItem& item) {
    const std::int64_t t0 = t0_ns.load(std::memory_order_acquire);
    if (t0 == kNoEpoch) {
        if (held.size() >= kAudioQueueCapacity) {
            // Bounded, like every other queue here (CLAUDE.md hard rule 5). The
            // oldest goes, because it is the one most likely to predate `t0`
            // anyway. Reaching this at all means the video side took more than a
            // second to produce its first frame.
            pool.release(std::move(held.front().bytes));
            held.erase(held.begin());
            buffers_before_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        held.push_back(std::move(item));
        return;
    }

    if (!timeline.started()) {
        timeline.start(t0);

        // Replay in arrival order, before the item that resolved the epoch. The
        // timeline decides what survives.
        std::vector<AudioWorkItem> replay;
        replay.swap(held);
        for (AudioWorkItem& stashed : replay) {
            place(stashed);
            pool.release(std::move(stashed.bytes));
        }
    }

    place(item);
}

void AudioEncodePath::Impl::apply_migration(const AudioWorkItem& item) {
    // Silence-fill up to the seam first, so the timeline never shortens across the
    // handover -- SPEC.md §14.1's "fully silence-filled". This is still the *old*
    // endpoint's rate, which is why it happens before anything is swapped.
    if (const std::int64_t missing = timeline.frames_missing_at(item.qpc_ns); missing > 0) {
        static_cast<void>(timeline.inject_silence(missing));
        static_cast<void>(emit_silence(missing));
    }

    // Bank the outgoing timeline's contribution in seconds before its unit stops
    // meaning anything. A frame count cannot be carried across a rate change, which is
    // the same lesson BUG-025 taught the video pacer.
    const int old_rate = timeline.sample_rate() > 0 ? timeline.sample_rate() : 48000;
    timeline_seconds_total += static_cast<double>(timeline.frames_written()) / static_cast<double>(old_rate);
    silence_seconds_total += static_cast<double>(timeline.silence_frames_injected()) / static_cast<double>(old_rate);

    // The resampler is the only thing that changes shape, and it already knows how:
    // `reconfigure_input` rebuilds the input side and leaves the output layout alone.
    // That is exactly §14.1's "keep the encoder's output format constant and adapt via
    // `libswresample`", so the AAC encoder's output -- and therefore the mux stream's --
    // is identical either side of the seam and the file stays valid.
    //
    // Rebuilding *in place* rather than swapping a fresh object also matters: the
    // resampler is deliberately non-movable because it owns an `SwrContext` and a
    // pre-allocated output frame, and replacing it wholesale would throw away the
    // buffer capacity the steady state depends on.
    if (const Result<void> reconfigured = resampler.reconfigure_input(item.new_format); !reconfigured.has_value()) {
        FC_LOG_ERROR(Subsystem::Audio, "the new endpoint's format is unusable; keeping the previous one",
                     LogFields{}
                         .add("sample_rate", item.new_format.sample_rate)
                         .add("channels", item.new_format.channels)
                         .add_error(reconfigured.error()));
        return;
    }

    // A fresh timeline in the new endpoint's rate, beginning at the seam. Chained, not
    // rebased: the two never share a unit and nothing is converted per packet.
    timeline = audio::AudioTimeline{item.new_format.sample_rate, settings.buffer_period_ns};
    timeline.attach_pause_clock(pause_clock);
    timeline.start(item.qpc_ns);

    settings.input = item.new_format;
    input_rate = item.new_format.sample_rate;

    // The drift loop and the device-clock cross-check both anchor on an endpoint that
    // no longer exists. Left un-reset they would report the *previous* device's error
    // against the new one's clock for the rest of the recording.
    drift = audio::DriftCompensator{};
    device_anchor_frames = -1;
    device_anchor_qpc_ns = 0;

    migrations.fetch_add(1, std::memory_order_relaxed);
    FC_LOG_WARN(Subsystem::Audio, "AUDIO_DEVICE_MIGRATION",
                LogFields{}
                    .add("at_qpc_ns", item.qpc_ns)
                    .add("sample_rate", item.new_format.sample_rate)
                    .add("channels", item.new_format.channels)
                    .add("previous_rate", old_rate)
                    .add("timeline_s", timeline_seconds_total));
}

void AudioEncodePath::Impl::place(AudioWorkItem& item) {
    Result<void> emitted = ok();

    if (item.kind == AudioWorkItem::Kind::MigrateInput) {
        apply_migration(item);
        return;
    }

    if (item.kind == AudioWorkItem::Kind::SilenceTick) {
        const std::int64_t missing = timeline.frames_missing_at(item.qpc_ns);
        if (missing > 0) {
            static_cast<void>(timeline.inject_silence(missing));
            emitted = emit_silence(missing);
        }
        // A tick carries no duration of its own -- the timeline was just brought up
        // to exactly this instant, so this instant is where the comparison belongs.
        evaluate_drift(item.qpc_ns);
    } else {
        cross_check_device_clock(item.qpc_ns, item.device_position_frames);

        const audio::CapturedPacket packet{item.qpc_ns, item.frames, item.flags};
        const audio::TimelineSegment segment = timeline.accept(packet);

        if (segment.silence_frames > 0) {
            emitted = emit_silence(segment.silence_frames);
        }
        if (emitted.has_value() && segment.audio_frames > 0) {
            if (segment.content_is_silence || item.bytes.empty()) {
                // A SILENT buffer's contents are undefined, so it is *filled*
                // rather than skipped -- skipping shortens the timeline by its
                // duration (SPEC.md §8.2).
                emitted = emit_silence(segment.audio_frames);
            } else {
                // `accept` trims an overlap from the front, so the usable audio is
                // the tail of the buffer. Trimming the back instead would write
                // samples the timeline has already passed.
                const std::int64_t trimmed = item.frames - segment.audio_frames;
                const auto offset = static_cast<std::size_t>(trimmed) * static_cast<std::size_t>(item.bytes_per_frame);
                emitted = emit_audio(item.bytes.data() + offset, segment.audio_frames);
            }
        }

        // Measured at the packet's *end*: the timeline has advanced by the
        // packet's duration, so comparing the track's length against the packet's
        // start timestamp would report one buffer period of drift on every
        // healthy stream.
        evaluate_drift(item.qpc_ns + frames_to_ns(item.frames, input_rate));
    }

    if (!emitted.has_value()) {
        // One failed buffer must not end the recording (CLAUDE.md §1). The
        // timeline has already advanced, so the track keeps its length and the
        // loss shows up as a gap rather than as a desync of everything after it.
        FC_LOG_WARN(Subsystem::Audio, "an audio buffer was lost after an encode-stage failure",
                    LogFields{}.add_error(emitted.error()));
        const std::lock_guard lock(stats_mutex);
        ++stats.encode_failures;
    }

    publish_stats();
}

void AudioEncodePath::Impl::aenc_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(aenc_done);
    set_thread_name(settings.thread_label.empty() ? std::string{"fc-aenc"} : settings.thread_label);
    try {
        while (auto item = queue.pop()) {
            process(*item);
            pool.release(std::move(item->bytes));
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "aenc thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

/// End of stream, run from the caller's thread after `aenc` has exited.
Result<void> AudioEncodePath::Impl::finish() {
    if (!opened.load(std::memory_order_acquire)) {
        return ok();
    }

    // A recording stopped before the epoch ever resolved -- no video frame ever
    // arrived -- leaves the stash full. There is no timeline to put it on, so it
    // is counted and released rather than encoded against an epoch that never
    // existed.
    if (!held.empty()) {
        buffers_before_epoch.fetch_add(held.size(), std::memory_order_relaxed);
        for (AudioWorkItem& stashed : held) {
            pool.release(std::move(stashed.bytes));
        }
        held.clear();
    }

    // libswresample holds a filter delay whenever the endpoint rate differs from
    // 48 kHz. Those samples are audio; dropping them truncates the track.
    if (const Result<const AVFrame*> tail = resampler.flush(); tail.has_value()) {
        if (const Result<void> fed = feed(tail.value()); !fed.has_value()) {
            FC_LOG_WARN(Subsystem::Audio, "the resampler tail was lost", LogFields{}.add_error(fed.error()));
        }
    }

    for (int attempt = 0; attempt < kMaxEncoderStalls; ++attempt) {
        const Result<void> flushed = encoder.flush();
        if (flushed.has_value()) {
            break;
        }
        if (flushed.error() != FcError::INTERNAL_QUEUE_FULL) {
            FC_LOG_ERROR(Subsystem::Audio, "flushing the AAC encoder failed; finalizing anyway",
                         LogFields{}.add_error(flushed.error()));
            break;
        }
        drain_packets();
    }

    // Delayed packets only come out now.
    drain_packets();
    publish_stats();

    FC_LOG_INFO(Subsystem::Audio, "audio path finished",
                LogFields{}
                    .add("frames_written", timeline.frames_written())
                    .add("silence_frames", timeline.silence_frames_injected())
                    // The three numbers that separate "a quiet desktop" from "audio with
                    // holes in it". A large `silence_frames` over one `gap_fills` is a
                    // silent stretch; the same total over dozens is a stutter, and a
                    // non-zero `timeline_drops` beside it means real audio was discarded
                    // to make room for the silence rather than merely absent.
                    .add("gap_fills", timeline.gap_fills())
                    .add("jitter_snaps", timeline.snaps())
                    .add("timeline_drops", timeline.dropped_packets())
                    .add("discontinuities", timeline.discontinuities())
                    .add("worst_drift_us", drift.worst_drift_ns() / 1000)
                    // SPEC.md §8.3's cross-check, summarised once at INFO so the
                    // number survives a default-level log.
                    .add("device_vs_qpc_us", stats.device_clock_delta_ns / 1000)
                    .add("soft_resyncs", static_cast<std::int64_t>(drift.soft_resyncs()))
                    .add("hard_resyncs", static_cast<std::int64_t>(drift.hard_resyncs())));
    return ok();
}

AudioEncodePath::AudioEncodePath() : impl_(std::make_unique<Impl>()) {}

AudioEncodePath::~AudioEncodePath() {
    if (impl_ && impl_->opened.load(std::memory_order_acquire) && !impl_->stopped.load(std::memory_order_acquire)) {
        // Destroyed without stop -- a caller bug, but leaving the thread running
        // against freed state is worse than the tidy alternative.
        //
        // `stop` joins threads, flushes libav and logs, so it can allocate and
        // therefore throw. An exception leaving a destructor terminates the
        // process, which would turn a recoverable shutdown problem into a crash.
        // Caught by type rather than with `catch(...)`, and the handler logs --
        // CLAUDE.md §4 bans both the catch-all outside a thread entry and the
        // empty handler.
        try {
            const Result<void> stopped = stop();
            if (!stopped.has_value()) {
                FC_LOG_ERROR(Subsystem::Audio, "audio encode path destroyed while running; shutdown failed",
                             LogFields{}.add_error(stopped.error()));
            }
        } catch (const std::exception& e) {
            FC_LOG_ERROR(Subsystem::Audio, "audio encode path destructor could not shut down cleanly",
                         LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
        }
    }
}

Result<void> AudioEncodePath::open(const AudioEncodeSettings& settings, AudioPacketSink sink) {
    if (impl_->opened.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (settings.input.sample_rate <= 0 || settings.input.channels <= 0 || settings.buffer_period_ns <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    impl_->settings = settings;
    impl_->sink = std::move(sink);

    FC_TRY_ASSIGN(const AVChannelLayout resolved,
                  audio::resolve_channel_layout(settings.channel_layout, settings.input));
    // Ownership transfer, not a copy: `resolved` is a local that is deliberately
    // never uninitialised, so the layout has exactly one owner and no double free.
    av_channel_layout_uninit(&impl_->pinned);
    impl_->pinned = resolved;

    FC_TRY(impl_->resampler.initialize(settings.input, impl_->pinned));

    encode::AudioEncoderSettings encoder_settings;
    encoder_settings.sample_rate = audio::kCanonicalSampleRate;
    encoder_settings.layout = impl_->pinned; // copied by `open`; ownership stays here
    encoder_settings.bitrate_kbps = settings.bitrate_kbps;
    encoder_settings.global_header = true;
    FC_TRY(impl_->encoder.open(encoder_settings));

    impl_->input_rate = settings.input.sample_rate;
    impl_->timeline = audio::AudioTimeline{settings.input.sample_rate, settings.buffer_period_ns};
    impl_->timeline.attach_pause_clock(impl_->pause_clock);
    // Canonical rate, not the endpoint's: the drift being measured is the encoded
    // track's length against wall clock, and the track is 48 kHz whatever the
    // endpoint runs at. It is also the unit `swr_set_compensation` takes, so the
    // decision needs no rescaling on the way back out.
    impl_->drift = audio::DriftCompensator{audio::kCanonicalSampleRate};
    impl_->canonical_written = 0;
    impl_->device_anchor_frames = -1;
    impl_->device_anchor_qpc_ns = 0;
    impl_->last_device_log_ns = 0;

    impl_->aenc_done = std::promise<void>{};
    impl_->aenc_finished = impl_->aenc_done.get_future();
    impl_->opened.store(true, std::memory_order_release);
    impl_->aenc_thread = std::thread([impl = impl_.get()] { impl->aenc_loop(); });

    FC_LOG_INFO(Subsystem::Audio, "audio encode path opened",
                LogFields{}
                    .add("endpoint_rate", settings.input.sample_rate)
                    .add("endpoint_channels", settings.input.channels)
                    .add("pinned_channels", impl_->pinned.nb_channels)
                    .add("buffer_period_us", settings.buffer_period_ns / 1000));
    return ok();
}

const encode::AacEncoder* AudioEncodePath::encoder() const noexcept {
    return impl_->opened.load(std::memory_order_acquire) ? &impl_->encoder : nullptr;
}

void AudioEncodePath::attach_pause_clock(const timing::PauseClock* clock) noexcept {
    // Recorded on the Impl as well as handed to the timeline, so §14.1's migration can
    // re-attach it to the fresh timeline it builds. Called before `open`, from the
    // owner's thread, so it does not race the `aenc` thread that owns the timeline
    // afterwards.
    impl_->pause_clock = clock;
    impl_->timeline.attach_pause_clock(clock);
}

void AudioEncodePath::set_epoch(std::int64_t t0_ns) noexcept {
    std::int64_t expected = kNoEpoch;
    if (!impl_->t0_ns.compare_exchange_strong(expected, t0_ns, std::memory_order_acq_rel)) {
        return; // already resolved; the epoch is set once and never moves
    }

    // The watchdog measures quiet from here, so a stream that produces nothing at
    // all still starts receiving ticks two buffer periods after `t0` rather than
    // two periods after whatever pre-epoch buffer happened to arrive last.
    std::int64_t last = impl_->last_offer_qpc_ns.load(std::memory_order_acquire);
    while (last < t0_ns && !impl_->last_offer_qpc_ns.compare_exchange_weak(last, t0_ns, std::memory_order_acq_rel)) {
        // `last` is reloaded by the failed exchange.
    }
}

bool AudioEncodePath::epoch_set() const noexcept {
    return impl_->t0_ns.load(std::memory_order_acquire) != kNoEpoch;
}

void AudioEncodePath::offer(const audio::LoopbackBuffer& buffer) {
    if (!impl_->opened.load(std::memory_order_acquire) || impl_->stopped.load(std::memory_order_acquire)) {
        return;
    }
    if (buffer.frames <= 0) {
        return;
    }

    std::int64_t unset = 0;
    impl_->first_packet_qpc_ns.compare_exchange_strong(unset, buffer.qpc_ns, std::memory_order_acq_rel);
    impl_->last_offer_qpc_ns.store(buffer.qpc_ns, std::memory_order_release);

    AudioWorkItem item;
    item.kind = AudioWorkItem::Kind::Buffer;
    item.qpc_ns = buffer.qpc_ns;
    item.frames = buffer.frames;
    item.device_position_frames = buffer.device_position_frames;
    item.flags = buffer.flags;

    item.bytes_per_frame = buffer.bytes_per_frame;

    if (buffer.data != nullptr && buffer.bytes_per_frame > 0) {
        const auto bytes = static_cast<std::size_t>(buffer.frames) * static_cast<std::size_t>(buffer.bytes_per_frame);
        item.bytes = impl_->pool.acquire(bytes);
        std::memcpy(item.bytes.data(), buffer.data, bytes);
    } else if (buffer.data != nullptr) {
        // A buffer that does not say what its bytes are cannot be copied safely, and
        // guessing is what BUG-031 was. Treated as silent: the timeline still advances
        // by the buffer's duration, so the track keeps its length (CLAUDE.md §1).
        //
        // Logged once, not per buffer. This runs on the audio thread, where CLAUDE.md
        // rule 4 forbids logging above TRACE -- and a condition that holds for one
        // buffer holds for every buffer that endpoint produces, so the unguarded form
        // would be a 50 Hz log flood on the one thread that must never block.
        if (!impl_->unsized_buffer_reported.test_and_set(std::memory_order_relaxed)) {
            FC_LOG_ERROR(Subsystem::Audio, "an audio buffer arrived without a frame size and was filled with silence",
                         LogFields{}.add("frames", buffer.frames).add_error(FcError::AUDIO_MIX_FORMAT_UNSUPPORTED));
        }
        item.flags.silent = true;
    }

    // Approximate by construction -- the queue can drain between the check and the
    // push. It exists to make sustained pressure visible, and sustained pressure
    // does not race away.
    if (impl_->queue.size() >= impl_->queue.capacity()) {
        impl_->queue_waits.fetch_add(1, std::memory_order_relaxed);
    }

    // Block policy: audio is never dropped (SPEC.md §12). See the header comment
    // for why blocking here is the lesser fault.
    static_cast<void>(impl_->queue.push(std::move(item)));
    impl_->buffers_offered.fetch_add(1, std::memory_order_relaxed);
}

bool AudioEncodePath::silence_due_at(std::int64_t now_ns) const noexcept {
    if (impl_->t0_ns.load(std::memory_order_acquire) == kNoEpoch) {
        return false;
    }
    if (impl_->stopped.load(std::memory_order_acquire)) {
        return false;
    }
    // SPEC.md §8.2's threshold. One buffer period of quiet is ordinary jitter; two
    // means the endpoint has genuinely stopped producing.
    return (now_ns - impl_->last_offer_qpc_ns.load(std::memory_order_acquire)) > (2 * impl_->settings.buffer_period_ns);
}

void AudioEncodePath::request_silence(std::int64_t now_ns) {
    if (!impl_->opened.load(std::memory_order_acquire) || impl_->stopped.load(std::memory_order_acquire)) {
        return;
    }
    AudioWorkItem item;
    item.kind = AudioWorkItem::Kind::SilenceTick;
    item.qpc_ns = now_ns;
    static_cast<void>(impl_->queue.push(std::move(item)));
    impl_->silence_ticks.fetch_add(1, std::memory_order_relaxed);
}

void AudioEncodePath::migrate_input(const audio::MixFormat& format, std::int64_t at_qpc_ns) {
    if (!impl_->opened.load(std::memory_order_acquire) || impl_->stopped.load(std::memory_order_acquire)) {
        return;
    }
    AudioWorkItem item;
    item.kind = AudioWorkItem::Kind::MigrateInput;
    item.qpc_ns = at_qpc_ns;
    item.new_format = format;
    static_cast<void>(impl_->queue.push(std::move(item)));
}

Result<void> AudioEncodePath::stop() {
    if (!impl_->opened.load(std::memory_order_acquire)) {
        return ok();
    }
    if (impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return ok(); // idempotent, per SPEC.md §10.4
    }

    impl_->queue.close();
    if (!await_worker(impl_->aenc_finished, impl_->aenc_thread, impl_->settings.thread_label.c_str(),
                      Subsystem::Audio)) {
        // The worker is detached and still reading `impl_`. The caller keeps this
        // object alive deliberately rather than freeing state out from under it.
        return FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }

    // Safe from this thread now: `aenc` has exited, so nothing else owns the
    // resampler or the encoder.
    return impl_->finish();
}

AudioStats AudioEncodePath::stats() const {
    AudioStats copy;
    {
        const std::lock_guard lock(impl_->stats_mutex);
        copy = impl_->stats;
    }
    // Read straight off the atomics the producer threads own, so neither the
    // `audio` nor the `silence` thread ever contends for the stats lock.
    copy.buffers_offered = impl_->buffers_offered.load(std::memory_order_relaxed);
    copy.buffers_before_epoch = impl_->buffers_before_epoch.load(std::memory_order_relaxed);
    copy.silence_ticks = impl_->silence_ticks.load(std::memory_order_relaxed);
    copy.queue_waits = impl_->queue_waits.load(std::memory_order_relaxed);
    copy.first_packet_qpc_ns = impl_->first_packet_qpc_ns.load(std::memory_order_relaxed);
    return copy;
}

// ---------------------------------------------------------------------------
// AudioPath
// ---------------------------------------------------------------------------

struct AudioPath::Impl {
    audio::LoopbackCapture capture;
    AudioEncodePath encode;

    std::function<void(std::int64_t)> on_first_packet;
    std::atomic<bool> notified{false};

    std::thread silence_thread;
    std::promise<void> silence_done;
    std::future<void> silence_finished;
    std::mutex silence_mutex;
    std::condition_variable silence_cv;
    bool silence_stop = false;
    std::chrono::nanoseconds tick_interval{10'000'000};

    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    void silence_loop();
};

void AudioPath::Impl::silence_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(silence_done);
    set_thread_name("fc-silence");
    try {
        for (;;) {
            {
                std::unique_lock lock(silence_mutex);
                if (silence_cv.wait_for(lock, tick_interval, [this] { return silence_stop; })) {
                    break;
                }
            }
            // Outside the lock: `request_silence` may block on the audio queue,
            // and the `silence` thread is the one thread SPEC.md §12 permits to
            // wait -- but not while holding a lock `stop` needs.
            const std::int64_t now = timing::qpc_now_ns();
            if (encode.silence_due_at(now)) {
                encode.request_silence(now);
            }
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "silence watchdog terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::AUDIO_SILENCE_GENERATOR_STALLED));
    }
    clear_thread_name();
}

AudioPath::AudioPath() : impl_(std::make_unique<Impl>()) {}

AudioPath::~AudioPath() {
    if (impl_ && impl_->started.load(std::memory_order_acquire) && !impl_->stopped.load(std::memory_order_acquire)) {
        // See `AudioEncodePath::~AudioEncodePath` for why this is caught by type.
        try {
            const Result<void> stopped = stop();
            if (!stopped.has_value()) {
                FC_LOG_ERROR(Subsystem::Audio, "audio path destroyed while running; shutdown failed",
                             LogFields{}.add_error(stopped.error()));
            }
        } catch (const std::exception& e) {
            FC_LOG_ERROR(Subsystem::Audio, "audio path destructor could not shut down cleanly",
                         LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
        }
    }
}

Result<void> AudioPath::start(const AudioPathSettings& settings, AudioPacketSink sink,
                              std::function<void(std::int64_t)> on_first_packet) {
    if (impl_->started.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    impl_->on_first_packet = std::move(on_first_packet);

    // Capture starts before the encode path is open, because the endpoint's mix
    // format is what decides the pinned layout and the format is only known once
    // the endpoint is negotiated. Buffers that arrive in that window are dropped
    // by `offer`, which is harmless: they predate `t0` in every case, since `t0`
    // cannot resolve until a video frame has also arrived, and the timeline fills
    // the interval with silence of exactly the right length.
    FC_TRY(impl_->capture.start(settings.device_id, [impl = impl_.get()](const audio::LoopbackBuffer& buffer) {
        if (!impl->notified.exchange(true, std::memory_order_acq_rel) && impl->on_first_packet) {
            impl->on_first_packet(buffer.qpc_ns);
        }
        impl->encode.offer(buffer);
    }));

    AudioEncodeSettings encode_settings;
    encode_settings.input = impl_->capture.format();
    encode_settings.channel_layout = settings.channel_layout;
    encode_settings.bitrate_kbps = settings.bitrate_kbps;

    if (const Result<void> opened = impl_->encode.open(encode_settings, std::move(sink)); !opened.has_value()) {
        impl_->capture.stop();
        return opened.error();
    }

    // Half a buffer period: the watchdog must notice a two-period gap promptly,
    // and polling at exactly the period would beat against it.
    impl_->tick_interval =
        std::chrono::nanoseconds{std::max<std::int64_t>(encode_settings.buffer_period_ns / 2, 1'000'000)};
    impl_->silence_done = std::promise<void>{};
    impl_->silence_finished = impl_->silence_done.get_future();
    impl_->started.store(true, std::memory_order_release);
    impl_->silence_thread = std::thread([impl = impl_.get()] { impl->silence_loop(); });

    FC_LOG_INFO(Subsystem::Audio, "audio path started",
                LogFields{}
                    .add("device", impl_->capture.device_name())
                    .add("rate", encode_settings.input.sample_rate)
                    .add("channels", encode_settings.input.channels)
                    .add("float", encode_settings.input.is_float));
    return ok();
}

const encode::AacEncoder* AudioPath::encoder() const noexcept {
    return impl_->encode.encoder();
}

void AudioPath::set_epoch(std::int64_t t0_ns) noexcept {
    impl_->encode.set_epoch(t0_ns);
}

void AudioPath::attach_pause_clock(const timing::PauseClock* clock) noexcept {
    impl_->encode.attach_pause_clock(clock);
}

Result<void> AudioPath::migrate_to(const std::string& device_id) {
    if (!impl_->started.load(std::memory_order_acquire) || impl_->stopped.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    const std::int64_t began = timing::qpc_now_ns();

    // The old endpoint first. Its buffers are already queued ahead of the migration
    // item, so nothing in flight is lost -- the `aenc` thread will place them, then see
    // the migration, then place the new endpoint's.
    impl_->capture.stop();

    // The sink is re-established rather than reused, because `LoopbackCapture::start`
    // takes ownership of one and the previous call's has gone with the stopped capture.
    const Result<void> opened = impl_->capture.start(
        device_id, [impl = impl_.get()](const audio::LoopbackBuffer& b) { impl->encode.offer(b); });
    if (!opened.has_value()) {
        FC_LOG_ERROR(Subsystem::Audio, "the replacement endpoint would not open; the recording continues in silence",
                     LogFields{}.add("device_id", device_id).add_error(opened.error()));
        // Deliberately not fatal. The silence watchdog keeps the timeline advancing, so
        // the recording stays the right length and the video is unaffected -- which is
        // a far better outcome than ending it, and is what CLAUDE.md §1 asks for.
        return opened.error();
    }

    // The seam is *now*, after the new endpoint has negotiated. Taking it before would
    // leave a hole the silence fill could not see, because the timeline would already
    // have been chained past it.
    const std::int64_t seam = timing::qpc_now_ns();
    impl_->encode.migrate_input(impl_->capture.format(), seam);

    FC_LOG_WARN(Subsystem::Audio, "audio endpoint migrated",
                LogFields{}
                    .add("device", impl_->capture.device_name())
                    .add("sample_rate", impl_->capture.format().sample_rate)
                    .add("channels", impl_->capture.format().channels)
                    .add("gap_ms", (seam - began) / 1'000'000)
                    .add("target_ms", 200));
    return ok();
}

Result<void> AudioPath::stop() {
    if (!impl_->started.load(std::memory_order_acquire)) {
        return ok();
    }
    if (impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return ok();
    }

    // Order matters: no more buffers, then no more ticks, then drain and flush.
    // Stopping the watchdog first would let a late tick queue behind a closed
    // queue and be silently dropped, which is harmless but makes the counters lie.
    impl_->capture.stop();

    {
        const std::lock_guard lock(impl_->silence_mutex);
        impl_->silence_stop = true;
    }
    impl_->silence_cv.notify_all();
    if (!await_worker(impl_->silence_finished, impl_->silence_thread, "fc-silence", Subsystem::Audio)) {
        return FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }

    return impl_->encode.stop();
}

AudioStats AudioPath::stats() const {
    return impl_->encode.stats();
}

audio::MixFormat AudioPath::format() const noexcept {
    return impl_->capture.format();
}

std::string AudioPath::device_name() const {
    return impl_->capture.device_name();
}

} // namespace fc::pipeline
