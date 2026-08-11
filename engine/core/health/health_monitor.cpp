#include "core/health/health_monitor.h"

#include <algorithm>

namespace fc::health {

std::string_view to_string(Rung rung) noexcept {
    switch (rung) {
    case Rung::Nominal:
        return "nominal";
    case Rung::EncoderQueueHigh:
        return "encoder_queue_high";
    case Rung::EncoderQueueCritical:
        return "encoder_queue_critical";
    case Rung::FrameDropsSustained:
        return "frame_drops_sustained";
    case Rung::EncoderDeviceFailing:
        return "encoder_device_failing";
    case Rung::HardwareEncodersUnavailable:
        return "hardware_encoders_unavailable";
    case Rung::DiskPressure:
        return "disk_pressure";
    case Rung::CaptureFailed:
        return "capture_failed";
    }
    return "unknown";
}

void Latch::update(bool triggered, std::int64_t now_ns, std::int64_t hold_ns) noexcept {
    if (triggered) {
        if (!engaged_) {
            engaged_ = true;
            ++engagements_;
        }
        // Any recurrence restarts the clean period from scratch. A trigger that
        // fires once every 29 s never disengages, which is the intent: SPEC.md §13
        // asks for 30 s of *clean* operation, not 30 s since the last look.
        clearing_ = false;
        return;
    }

    if (!engaged_) {
        return;
    }

    if (!clearing_) {
        clearing_ = true;
        clear_since_ns_ = now_ns;
        return;
    }

    if (now_ns - clear_since_ns_ >= hold_ns) {
        engaged_ = false;
        clearing_ = false;
    }
}

Monitor::Monitor(int configured_fps, int configured_cqp) noexcept
    : configured_fps_(configured_fps > 0 ? configured_fps : 60), configured_cqp_(configured_cqp) {}

void Monitor::note_device_failure(std::int64_t now_ns) {
    device_failures_[device_failure_head_] = now_ns;
    device_failure_head_ = (device_failure_head_ + 1) % threshold::kDeviceFailureCount;
    if (device_failure_count_ < threshold::kDeviceFailureCount) {
        ++device_failure_count_;
    }
}

void Monitor::record(const Sample& sample) {
    history_[history_head_] = Observation{sample.now_ns, sample.frames_submitted, sample.frames_queue_dropped};
    history_head_ = (history_head_ + 1) % kHistory;
    if (history_size_ < kHistory) {
        ++history_size_;
    }
}

const Monitor::Observation* Monitor::window_start(std::int64_t now_ns) const noexcept {
    if (history_size_ == 0) {
        return nullptr;
    }

    const std::int64_t cutoff = now_ns - threshold::kWindowNs;

    // Walk oldest to newest and keep the newest observation that is still at or
    // before the cutoff, falling back to the oldest held. The fallback is what
    // makes the first three seconds of a recording behave: the window is short of
    // history, so the ratio is computed over what there is, and
    // `kMinimumFramesForRatio` stops that from being a trigger on its own.
    const std::size_t oldest = (history_head_ + kHistory - history_size_) % kHistory;
    const Observation* chosen = &history_[oldest];
    for (std::size_t i = 0; i < history_size_; ++i) {
        const Observation& observation = history_[(oldest + i) % kHistory];
        if (observation.now_ns > cutoff) {
            break;
        }
        chosen = &observation;
    }
    return chosen;
}

Plan Monitor::evaluate(const Sample& sample) {
    const std::int64_t now = sample.now_ns;

    // ---------------------------------------------------------------------
    // Rungs 1 and 2 -- encoder queue occupancy
    // ---------------------------------------------------------------------
    // A first sample already over the threshold starts its 3 s clock now rather
    // than reading as an infinitely long overrun: a pipeline that happens to start
    // busy must not trip rung 1 on its first evaluation with no evidence behind it.
    if (!has_queue_ok_ || sample.encoder_queue_pressure <= threshold::kQueueHigh) {
        queue_ok_since_ns_ = now;
        has_queue_ok_ = true;
    }
    const bool queue_high_sustained = sample.encoder_queue_pressure > threshold::kQueueHigh &&
                                      (now - queue_ok_since_ns_) >= threshold::kQueueHighHoldNs;
    queue_high_.update(queue_high_sustained, now, threshold::kRecoveryHoldNs);
    queue_critical_.update(sample.encoder_queue_pressure > threshold::kQueueCritical, now, threshold::kRecoveryHoldNs);

    // ---------------------------------------------------------------------
    // Rung 3 -- sustained frame drops over the rolling window
    // ---------------------------------------------------------------------
    const Observation* start = window_start(now);
    drop_ratio_ = 0.0;
    // SPEC.md §7.5 suspends the ratio while paused. Left engaged, the window keeps
    // sliding while submissions stop, and rung 3 halves the capture rate of a recording
    // whose only fault was being paused.
    if (start != nullptr && !sample.paused) {
        // Saturating subtraction: the counters are monotonic in a real recording,
        // but a caller that resets them between recordings must not produce a
        // gigantic unsigned ratio out of a wrapped difference.
        const std::uint64_t submitted =
            sample.frames_submitted > start->frames_submitted ? sample.frames_submitted - start->frames_submitted : 0;
        const std::uint64_t dropped = sample.frames_queue_dropped > start->frames_queue_dropped
                                          ? sample.frames_queue_dropped - start->frames_queue_dropped
                                          : 0;
        if (submitted >= threshold::kMinimumFramesForRatio) {
            drop_ratio_ = static_cast<double>(dropped) / static_cast<double>(submitted);
        }
    }
    frame_drops_.update(drop_ratio_ > threshold::kFrameDropRatio, now, threshold::kRecoveryHoldNs);
    record(sample);

    // ---------------------------------------------------------------------
    // Rung 4 -- device failures, three in 60 s
    // ---------------------------------------------------------------------
    bool device_failing = false;
    if (device_failure_count_ == threshold::kDeviceFailureCount) {
        // The oldest of the three retained: the ring is exactly three deep, so the
        // slot the next write will take is the third-most-recent failure.
        const std::int64_t oldest = device_failures_[device_failure_head_];
        device_failing = (now - oldest) <= threshold::kDeviceFailureWindowNs;
    }
    device_failing_.update(device_failing, now, threshold::kRecoveryHoldNs);

    // ---------------------------------------------------------------------
    // Rung 6 -- disk latency and free space
    // ---------------------------------------------------------------------
    const bool disk_slow = sample.disk_write_p99_ns > threshold::kDiskWriteP99Ns;
    // `UINT64_MAX` is "not measured". A failed free-space query must not read as an
    // empty volume and stop a healthy recording.
    const bool disk_low = sample.disk_free_bytes < threshold::kDiskFreeWarnBytes;
    const bool disk_critical = sample.disk_free_bytes < threshold::kDiskFreeStopBytes;
    disk_pressure_.update(disk_slow || disk_low, now, threshold::kRecoveryHoldNs);

    // ---------------------------------------------------------------------
    // Row 9's stall detector -- no frame in 3 x the frame interval
    // ---------------------------------------------------------------------
    const std::int64_t frame_interval_ns = 1'000'000'000 / configured_fps_;
    const std::int64_t stall_limit_ns = frame_interval_ns * threshold::kStallFrameIntervals;
    std::int64_t stall_ns = 0;
    bool stalled = false;
    // SPEC.md §7.5 suspends this while paused. A paused recording stops producing
    // frames by design, so without the guard the detector fires three frame intervals
    // -- 50 ms at 60 fps -- into every pause, and row 9's M7 recovery answers it by
    // rebuilding the capture session under a user who pressed a hotkey.
    if (!sample.paused && sample.last_frame_ns != 0 && now > sample.last_frame_ns) {
        stall_ns = now - sample.last_frame_ns;
        stalled = stall_ns > stall_limit_ns;
    }
    if (stalled) {
        if (!stalled_) {
            ++stall_episodes_; // counted per episode, not per sample
        }
        worst_stall_ns_ = std::max(worst_stall_ns_, stall_ns);
    }
    stalled_ = stalled;

    // ---------------------------------------------------------------------
    // Compose
    // ---------------------------------------------------------------------
    Plan plan;

    // Each action from its own trigger, never from the composite rung -- see the
    // divergence note in the header.
    plan.preset_step = queue_high_.engaged() ? 1 : 0;
    plan.target_cqp = queue_critical_.engaged() ? configured_cqp_ + 4 : configured_cqp_;
    plan.target_fps = frame_drops_.engaged() ? std::max(1, configured_fps_ / 2) : configured_fps_;
    plan.request_gpu_migration = device_failing_.engaged();
    plan.warn_disk_space = disk_pressure_.engaged();
    plan.capture_stalled = stalled;
    plan.capture_stall_ns = stalled ? stall_ns : 0;

    // Both terminal, and never withdrawn: a recording stopped for want of disk space
    // does not resume because a log file rotated. Tracked apart because they are
    // different rungs -- §13 puts "at < 500 MB free, stop and finalize cleanly" on
    // rung 6, and reserves rung 7 for an unrecoverable *capture* failure. Folding
    // them together would report a full disk as a capture fault, which is the one
    // field of the report a user acts on.
    disk_stop_ = disk_stop_ || disk_critical;
    capture_failed_ = capture_failed_ || sample.capture_failed;
    plan.stop_and_finalize = disk_stop_ || capture_failed_;

    Rung active = Rung::Nominal;
    const auto raise = [&active](bool condition, Rung candidate) {
        if (condition && static_cast<int>(candidate) > static_cast<int>(active)) {
            active = candidate;
        }
    };
    raise(queue_high_.engaged(), Rung::EncoderQueueHigh);
    raise(queue_critical_.engaged(), Rung::EncoderQueueCritical);
    raise(frame_drops_.engaged(), Rung::FrameDropsSustained);
    raise(device_failing_.engaged(), Rung::EncoderDeviceFailing);
    raise(!sample.hardware_encoder, Rung::HardwareEncodersUnavailable);
    raise(disk_pressure_.engaged() || disk_stop_, Rung::DiskPressure);
    raise(capture_failed_, Rung::CaptureFailed);

    plan.changed = active != rung_;
    if (plan.changed) {
        ++transitions_;
        rung_ = active;
    }
    plan.rung = rung_;
    return plan;
}

} // namespace fc::health
