#include "core/timing/frame_pacer.h"

#include <algorithm>

namespace fc::timing {
namespace {

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

} // namespace

std::int64_t ticks_per_frame(int fps) noexcept {
    if (fps <= 0) {
        return kVideoTimebaseDen;
    }
    return kVideoTimebaseDen / fps;
}

std::int64_t quantize_index(std::int64_t qpc_ns, std::int64_t t0_ns, int fps) noexcept {
    if (fps <= 0) {
        return 0;
    }

    const std::int64_t elapsed = qpc_ns - t0_ns;
    if (elapsed <= 0) {
        // Before the epoch. Clamping rather than returning a negative index: a
        // negative PTS is rejected by libavformat, and there is no frame earlier
        // than the first one.
        return 0;
    }

    // round(elapsed * fps / 1e9) in integers. Splitting the division keeps the
    // intermediate below 2^63 for any plausible recording length: at 24 hours
    // `elapsed` is ~8.6e13, and `seconds * fps` cannot overflow before the
    // recording outlives the hardware. Doing it as `elapsed * fps` directly would
    // overflow after roughly 4 hours at 60 fps.
    const std::int64_t seconds = elapsed / kNsPerSecond;
    const std::int64_t remainder = elapsed % kNsPerSecond;
    const std::int64_t whole = seconds * fps;

    // round(remainder * fps / 1e9), still integral.
    const std::int64_t scaled = (remainder * fps) + (kNsPerSecond / 2);
    return whole + (scaled / kNsPerSecond);
}

void Pacer::start(std::int64_t t0_ns) noexcept {
    t0_ns_ = t0_ns;
    last_index_ = -1;
    last_pts_ = -1;
    started_ = true;
}

double Pacer::timeline_seconds() const noexcept {
    if (!started_ || last_pts_ < 0) {
        return 0.0;
    }
    // The last frame occupies its whole slot, so the timeline runs to the end of it
    // rather than to its start. Without the trailing frame duration a one-frame file
    // measures zero seconds.
    return static_cast<double>(last_pts_ + ticks_per_frame(fps_)) / static_cast<double>(kVideoTimebaseDen);
}

void Pacer::retime(int fps) noexcept {
    if (fps <= 0) {
        return;
    }

    // The grid changes; the timeline must not. `last_index_` counts slots on the
    // *old* grid, so it is rescaled onto the new one -- otherwise the next frame
    // would be compared against an index from a different coordinate system and
    // could emit a PTS at or below one already written.
    //
    // Rescaled from the recorded PTS rather than from `last_index_ *
    // ticks_per_frame(fps_)`: they are equal, but only one of them stays equal after
    // a second retime, and re-deriving a value that is already stored exactly is how
    // truncation error gets a foothold.
    if (started_ && last_pts_ >= 0) {
        last_index_ = last_pts_ / ticks_per_frame(fps);
    }
    fps_ = fps;
}

std::int64_t Pacer::ticks_per_frame_now() const noexcept {
    return ticks_per_frame(fps_);
}

void Pacer::reserve_discontinuity(int frames) noexcept {
    if (!started_ || frames <= 0) {
        return;
    }
    // Advancing `last_index_` is what makes the slots unreachable: `decide` only ever
    // emits above it, and its duplicate fill starts at `last_index_ + 1`. So the
    // reserved slots are skipped by both paths without a special case in either.
    last_index_ += frames;
    last_pts_ = last_index_ * ticks_per_frame(fps_);
    reserved_ += static_cast<std::uint64_t>(frames);
}

PacingDecision Pacer::decide(std::int64_t qpc_ns) {
    PacingDecision decision;

    if (!started_) {
        // No epoch yet. Inventing one here would put t0 at whichever frame
        // happened to arrive first, which is exactly what SPEC.md §7.1 forbids --
        // video and audio share one epoch, negotiated by the session.
        decision.drop = true;
        ++dropped_;
        return decision;
    }

    // SPEC.md §7.5. Everything below this point works in *timeline* time rather than
    // capture time, and the two differ by the paused total. Expressed as a
    // pause-adjusted timestamp rather than by giving every line below a new origin, so
    // that a recording which never pauses runs the identical arithmetic it always did.
    std::int64_t timeline_qpc_ns = qpc_ns;
    if (pause_clock_ != nullptr) {
        const PauseClock::Mapping mapped = pause_clock_->map(qpc_ns, t0_ns_);
        if (mapped.state == PauseClock::State::Excised) {
            // Captured inside a paused span. There is no slot for it -- that is what
            // excision means -- and it is not a drop: see `excised()`.
            decision.drop = true;
            ++excised_;
            return decision;
        }
        timeline_qpc_ns = t0_ns_ + mapped.timeline_ns;
    }

    const std::int64_t index = quantize_index(timeline_qpc_ns, t0_ns_, fps_);

    // §7.5's "the pause must **not** be filled with duplicates" needs no code of its
    // own, and it is worth saying why rather than leaving it to be rediscovered. The
    // duplicate fill below triggers on `index > last_index_ + 1`, i.e. on a gap in the
    // grid. Excising the paused span removes the gap before the index is computed, so
    // the first frame after a resume lands on the slot immediately after the last one
    // before the pause and there is nothing to fill. Subtracting the paused total
    // *after* quantizing would instead manufacture one duplicate per paused frame
    // interval -- the frozen-frame outcome, produced by getting the order wrong.
    if (index <= last_index_) {
        // The source outran the cap, or its timestamp went backwards. Either way
        // this frame has no slot of its own, and emitting it would repeat or
        // reverse a PTS.
        decision.drop = true;
        ++dropped_;
        return decision;
    }

    const std::int64_t ticks = ticks_per_frame(fps_);

    // Fill any slots between the last emitted frame and this one. Skipped entirely
    // for the first frame, whose `last_index_` is -1 and which therefore has no
    // predecessor to duplicate.
    if (last_index_ >= 0 && index > last_index_ + 1) {
        const std::int64_t gap = index - last_index_ - 1;
        decision.duplicate_pts.reserve(static_cast<std::size_t>(gap));
        for (std::int64_t slot = last_index_ + 1; slot < index; ++slot) {
            decision.duplicate_pts.push_back(slot * ticks);
        }
        // The duplicates are emitted before the frame itself, so the timeline already
        // reaches them even if the frame's own submission were to fail.
        last_pts_ = (index - 1) * ticks;
        duplicated_ += static_cast<std::uint64_t>(gap);
        emitted_ += static_cast<std::uint64_t>(gap);
    }

    decision.index = index;
    decision.pts = index * ticks;

    // How far the content lags the slot it is being shown in. The slot's nominal
    // time is `index / fps`, evaluated in integers to stay exact over long runs.
    const std::int64_t slot_nominal_ns = (index * kNsPerSecond) / fps_;
    decision.staleness_ns = slot_nominal_ns - (timeline_qpc_ns - t0_ns_);

    const std::int64_t magnitude = decision.staleness_ns < 0 ? -decision.staleness_ns : decision.staleness_ns;
    worst_staleness_ns_ = std::max(worst_staleness_ns_, magnitude);
    staleness_sum_ns_ += decision.staleness_ns;
    ++staleness_samples_;

    last_index_ = index;
    last_pts_ = decision.pts;
    ++emitted_;
    return decision;
}

} // namespace fc::timing
