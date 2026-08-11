#pragma once

// CFR frame pacing and PTS quantization (SPEC.md §7.2).
//
// This is the "repeated / jerky frames" defect (SPEC.md §20 row 7) in one place.
// The bug is not that duplicate frames exist -- a 12 fps source genuinely has
// nothing new to show 48 times a second. The bug is emitting those duplicates with
// wall-clock-derived timestamps, which makes every player judder. The fix is to
// quantize every frame to an integer index on a fixed grid and derive PTS from the
// index alone, so PTS deltas are *exactly* equal for the whole recording.
//
// Deliberately a pure function over (qpc_ns, t0_ns, fps) with no clock, no
// threading and no libav types, because SPEC.md §20.1 names PTS quantization as a
// unit-test target and a component that reads the clock itself cannot be tested
// deterministically.
//
// M3 scope. The degradation ladder that changes `fps` mid-recording (SPEC.md §13
// rung 3) and the health monitor that drives it are M6; this component is built to
// accept that change -- see `Pacer::retime`.

#include "core/timing/pause_clock.h"

#include <cstdint>
#include <vector>

namespace fc::timing {

/// Video timebase denominator (SPEC.md §7.2). 60000 divides evenly by both 60 and
/// 30, so a frame duration is exactly 1000 or 2000 ticks and no rounding error
/// accumulates over a multi-hour recording.
inline constexpr std::int64_t kVideoTimebaseDen = 60000;

/// What to do with one captured frame.
struct PacingDecision {
    /// Emit nothing: the source produced two frames inside one grid slot, so this
    /// one would collide with an already-emitted PTS. SPEC.md §7.2 "source outran
    /// the cap".
    bool drop = false;

    /// PTS of the frame itself, in `1/kVideoTimebaseDen` units. Meaningless when
    /// `drop` is true.
    std::int64_t pts = 0;

    /// Grid index this frame landed on.
    std::int64_t index = 0;

    /// Signed nanoseconds between this slot's nominal time and the moment the
    /// frame was actually captured. Positive means the content is *older* than the
    /// slot it is being shown in.
    ///
    /// SPEC.md §7.2 keeps the *first* frame that maps to a slot and drops any
    /// later one, so the emitted frame sits somewhere in the slot's leading half
    /// and this is normally positive. A constant offset is imperceptible -- it
    /// shifts the whole timeline equally -- so the number that matters is how much
    /// it *varies*, which is bounded by one source frame interval and therefore
    /// grows as the source rate falls or fluctuates.
    ///
    /// Meaningless when `drop` is true. Not measured for duplicates: a duplicate
    /// carries no fresh content by definition, and is already counted separately.
    std::int64_t staleness_ns = 0;

    /// PTS values for duplicates of the *previous* frame that must be emitted
    /// before this one, filling a gap in the timeline. Empty in the common case.
    ///
    /// These are what keep the CFR timeline contiguous. Without them the file has
    /// fewer frames than its duration implies and players interpolate the gap as a
    /// stall; with them, and only with correct PTS on them, a slow source looks
    /// like smooth low-frame-rate video.
    std::vector<std::int64_t> duplicate_pts;
};

/// Ticks per frame on the CFR grid. `fps` must be positive.
[[nodiscard]] std::int64_t ticks_per_frame(int fps) noexcept;

/// The grid index a capture timestamp quantizes to.
///
/// `round((qpc_ns - t0_ns) * fps / 1e9)`, evaluated in integer arithmetic. Using
/// `double` here would lose exactness after a few hours: at 3 hours `qpc_ns - t0_ns`
/// is ~1.1e13, and a double's 53-bit mantissa cannot represent every integer
/// beyond 9.0e15 -- close enough to make the margin unarguable rather than
/// comfortable. Integers have no such ceiling.
///
/// Timestamps before `t0` clamp to index 0 rather than going negative: a negative
/// PTS is rejected by libavformat and there is no sensible frame before the first.
///
/// **Round-to-nearest is load-bearing, not incidental.** Quantizing a linear ramp
/// by rounding is a low-discrepancy mapping: the error is a sawtooth that resets
/// every period instead of accumulating, so when the source runs faster than the
/// target the frames that survive are spread evenly rather than bunched. That is
/// what stops a 144 Hz source feeding a 60 fps timeline from producing runs of
/// closely-spaced frames followed by a visible gap -- a defect that leaves frame
/// count, duration and PTS spacing all looking correct.
///
/// `test_frame_pacer.cpp` pins this with a spread bound, including a negative
/// control that fails if the check ever stops discriminating. Replacing the
/// rounding with an accumulating step (`index += 1/r` per frame) would break the
/// property while still passing every count-based assertion.
[[nodiscard]] std::int64_t quantize_index(std::int64_t qpc_ns, std::int64_t t0_ns, int fps) noexcept;

/// Known characteristic: when the source rate is an exact integer multiple of the
/// target, every intermediate tick falls precisely on a slot boundary.
///
/// At 120 Hz into 60 fps, tick 1 sits at 1/120 s, which is exactly half a slot.
/// Whether it lands in slot 0 or slot 1 comes down to the last digit of a
/// nanosecond timestamp -- 1/120 s is 8333333.33 ns and is not representable -- so
/// the gaps come out as a repeating 1, 2, 3 rather than a flat 2.
///
/// The effect is bounded to one source tick and is a *content*-timing wobble, not
/// timestamp judder: every slot still receives exactly one frame, every PTS is
/// still exactly on the grid, nothing is dropped or duplicated. It is inherent to
/// taking the first tick that falls in a slot, which is what an online pacer must
/// do -- picking the tick nearest the slot centre would require lookahead, and
/// therefore latency. Real capture jitter is microseconds, orders of magnitude
/// wider than the margin deciding these ties, so on hardware the outcome is
/// arbitrary rather than fragile.
///
/// Pinned by `FramePacer.AnExactMultipleSourceLandsOnSlotBoundariesAndVariesByOneTick`.

/// Stateful CFR pacer. One instance per video stream; not thread-safe, and used
/// only from the `venc` thread (SPEC.md §12).
class Pacer {
public:
    Pacer() = default;

    explicit Pacer(int fps) noexcept : fps_(fps) {}

    /// Establishes `t0`. SPEC.md §7.1: `t0` is the shared epoch for video *and*
    /// audio, so from M4 this is set once by the session, not by the pacer itself.
    void start(std::int64_t t0_ns) noexcept;

    /// Decides what to emit for a frame captured at `qpc_ns`.
    [[nodiscard]] PacingDecision decide(std::int64_t qpc_ns);

    /// Changes the frame rate mid-recording without breaking the timeline
    /// (SPEC.md §13 rung 3 halves the capture rate to 30 fps and keeps the CFR
    /// timeline intact via duplicates).
    ///
    /// The grid changes but the timebase does not, so previously emitted PTS stay
    /// valid and the next frame simply lands on a coarser grid. Unused until M6;
    /// present because the alternative -- rebuilding the pacer and resetting
    /// `last_index_` -- would emit a duplicate PTS and corrupt the stream.
    void retime(int fps) noexcept;

    /// Reserves `frames` grid slots that will never be emitted, so the next frame
    /// lands beyond them (SPEC.md §5.4's migration, §5.4's decode-order constraint).
    ///
    /// This deliberately leaves a **hole** in the CFR timeline, which is the one place
    /// in the engine that is allowed to. The reason is decode order, and it is not
    /// obvious: after the old encoder is flushed its last packet carries `DTS == PTS`,
    /// because a flush drains the whole reorder pipeline. A freshly opened encoder
    /// re-derives DTS from its own first PTS, so its first packet carries
    /// `DTS = PTS - reorder_depth`. Continuing the grid seamlessly therefore hands
    /// libavformat a DTS *below* the one just written and the packet is rejected
    /// outright -- measured as `non monotonically increasing dts ... 483 >= 467`.
    ///
    /// Reserving `reorder_depth` slots is the smallest advance that clears it. At
    /// SPEC.md §9's `max_b_frames = 2` that is two frames, 33 ms at 60 fps, inside a
    /// migration gap §5.4 already budgets 350 ms for. The alternative -- opening the
    /// replacement encoder with `max_b_frames = 0` so no reordering exists -- would
    /// change its SPS and break the byte-identical `extradata` the same-adapter path
    /// depends on.
    ///
    /// Slots after the reservation are filled with duplicates as usual, so the hole is
    /// exactly `frames` wide and never grows.
    void reserve_discontinuity(int frames) noexcept;

    /// Ticks per frame on the current grid, so a caller can convert a reservation
    /// into the PTS it will cost.
    [[nodiscard]] std::int64_t ticks_per_frame_now() const noexcept;

    /// Points the pacer at the recording's shared paused total (SPEC.md §7.5).
    ///
    /// Null -- the default -- means a recording that cannot pause, and `decide` then
    /// behaves exactly as it did before pause existed. That is not a convenience: every
    /// pacer test written before M8 asserts against the unpaused mapping, and a pacer
    /// that silently acquired a second term would make all of them assert something
    /// slightly different from what they say.
    ///
    /// The clock is **borrowed**. It belongs to the session, which outlives the
    /// pipeline, because §7.5 requires video and audio to consult one shared total and
    /// a pacer-owned copy would be exactly the second source of truth that requirement
    /// exists to forbid.
    void attach_pause_clock(const PauseClock* clock) noexcept {
        pause_clock_ = clock;
    }

    /// Frames dropped because they were captured inside a paused span.
    ///
    /// Deliberately **not** folded into `dropped()`: that counter feeds SPEC.md §13's
    /// ladder as "the source outran the cap", and a paused recording that reported its
    /// excised frames there would look like a degrading one. A pause is not a drop.
    [[nodiscard]] std::uint64_t excised() const noexcept {
        return excised_;
    }

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

    [[nodiscard]] int fps() const noexcept {
        return fps_;
    }

    [[nodiscard]] std::int64_t t0_ns() const noexcept {
        return t0_ns_;
    }

    /// Frames emitted, including duplicates. This is the number that must equal
    /// `duration_s * fps` for `test_cfr_exactness` (SPEC.md §20 row 7, M6).
    ///
    /// **Only while the rate is constant.** Use `timeline_seconds` for the file's
    /// length -- see the note there.
    [[nodiscard]] std::uint64_t emitted() const noexcept {
        return emitted_;
    }

    /// Length of the timeline emitted so far, in seconds.
    ///
    /// Derived from the last emitted PTS rather than from `emitted() / fps`, and the
    /// difference is not academic: the moment SPEC.md §13 rung 3 retimes the grid the
    /// two disagree, because the file then holds a stretch at 60 fps followed by a
    /// stretch at 30 and its length divides by neither rate. Computing it the other
    /// way made the SPEC.md §10.4 validation gate *reject* a recording that had
    /// degraded exactly as designed -- a graceful degradation reported as a failed
    /// file, which is worse than the degradation. See BUG-025.
    [[nodiscard]] double timeline_seconds() const noexcept;

    [[nodiscard]] std::uint64_t duplicated() const noexcept {
        return duplicated_;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_;
    }

    /// Grid slots deliberately skipped by `reserve_discontinuity`. Non-zero only after
    /// a migration or a session rebuild, and it is the amount by which
    /// `timeline_seconds()` exceeds what the emitted frame count would suggest.
    [[nodiscard]] std::uint64_t reserved() const noexcept {
        return reserved_;
    }

    /// Largest `|staleness_ns|` seen. Bounded by half a slot by construction, so a
    /// value near that bound means the source is running at or below the target
    /// rate and the pacer is showing whatever it last had.
    [[nodiscard]] std::int64_t worst_staleness_ns() const noexcept {
        return worst_staleness_ns_;
    }

    /// Mean *signed* staleness. Signed rather than absolute deliberately: a steady
    /// offset is a harmless constant shift of the whole timeline and shows up here
    /// as a bias. The gap between this and `worst_staleness_ns` is the wobble --
    /// the part that a viewer could actually notice.
    [[nodiscard]] std::int64_t mean_staleness_ns() const noexcept {
        return staleness_samples_ == 0 ? 0 : staleness_sum_ns_ / static_cast<std::int64_t>(staleness_samples_);
    }

    [[nodiscard]] std::uint64_t staleness_samples() const noexcept {
        return staleness_samples_;
    }

private:
    int fps_ = 60;
    std::int64_t t0_ns_ = 0;
    std::int64_t last_index_ = -1;
    /// Last PTS actually emitted, in timebase units, or -1 before the first frame.
    ///
    /// Kept alongside `last_index_` rather than derived from it, because the two live
    /// in different coordinate systems once `retime` has run: an index means nothing
    /// without the grid it was counted on, while a PTS is absolute and survives every
    /// rate change untouched.
    std::int64_t last_pts_ = -1;
    bool started_ = false;

    std::int64_t worst_staleness_ns_ = 0;
    std::int64_t staleness_sum_ns_ = 0;
    std::uint64_t staleness_samples_ = 0;

    /// Borrowed; see `attach_pause_clock`. Null for a recording that cannot pause.
    const PauseClock* pause_clock_ = nullptr;

    std::uint64_t emitted_ = 0;
    std::uint64_t duplicated_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t reserved_ = 0;
    std::uint64_t excised_ = 0;
};

} // namespace fc::timing
