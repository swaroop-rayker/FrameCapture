#pragma once

// Video segmentation (SPEC.md §11).
//
// ---------------------------------------------------------------------------
// What this decides, and what it deliberately does not
// ---------------------------------------------------------------------------
// §11 has two halves. One is policy -- *when* should the recording split, given a
// duration trigger, a size trigger, and the rule that whichever fires first wins. The
// other is mechanism -- open the next `AVFormatContext`, write its header, close the
// old one, restart timestamps. This class is the first half and none of the second: it
// owns no muxer, no file, no clock and no thread, which is what lets §11's arithmetic be
// asserted on the CPU tier against a supplied packet stream rather than observed on
// hardware.
//
// ---------------------------------------------------------------------------
// Why it is a state machine and not a predicate
// ---------------------------------------------------------------------------
// The obvious shape is `bool should_split(elapsed, bytes)`. It is wrong, and §11 says
// why in one line:
//
//   > Splits must be **keyframe-aligned**: request an IDR from the encoder, wait for it,
//   > close the current file at the packet *before* it, open the next file starting *at*
//   > it. Splitting mid-GOP produces a segment whose first seconds are unplayable
//   > garbage.
//
// So a trigger firing is not a split. It is a *request*, and the split happens later, at
// whichever packet turns out to be the IDR that answers it. Between the two the
// recording keeps writing to the current file, and nothing about the trigger may fire
// again -- a second request while one is outstanding would ask for an IDR the encoder is
// already producing and, worse, could split twice at adjacent keyframes and emit a file
// of a few frames.
//
// Modelling that as a predicate pushes the waiting into the caller, where it would live
// beside a muxer and a thread and stop being testable. Here it is three states and a
// unit test.
//
// ---------------------------------------------------------------------------
// The trigger measures the segment, not the recording
// ---------------------------------------------------------------------------
// A 30-minute duration trigger means each file is 30 minutes, not that the recording
// stops at 30. Both counters are therefore *relative to the last split*, and the planner
// keeps the marks itself rather than making every caller remember to subtract -- the
// caller passes cumulative values, which is what a muxer naturally reports.

#include "core/config/config.h"

#include <cstdint>
#include <filesystem>

namespace fc::mux {

/// Names one file of a segmented recording (SPEC.md §11).
///
/// `<basename>_part001.mkv`, zero-padded to three digits and widening past 999 rather
/// than wrapping -- a recording long enough to produce a thousand segments must not
/// start overwriting its first ones, and `_part1000` sorting after `_part999` is worth
/// more than a fixed width.
///
/// `index` is one-based, and **the first file is named too**: §11 gives the naming as
/// `<basename>_part001.mkv` without an exception for the first, and a set of files where
/// one is called `recording.mkv` and the rest `recording_partNNN.mkv` sorts wrongly and
/// reads as a mistake.
[[nodiscard]] std::filesystem::path segment_path(const std::filesystem::path& base, int index);

/// SPEC.md §11's split policy.
class SegmentPlanner {
public:
    /// What the caller should do with the packet it just offered.
    enum class Action {
        /// Write it to the current file and carry on.
        Continue,
        /// A trigger has fired. Ask the encoder for an IDR, then keep writing this and
        /// following packets to the current file until one arrives.
        RequestKeyframe,
        /// This packet is the keyframe the split was waiting for. Open the next file and
        /// write its header, close the current one, and write this packet as the first
        /// of the new file -- in that order, because §11 requires the next context to
        /// exist before the current one is closed so no packet has nowhere to go.
        Split,
    };

    SegmentPlanner() = default;

    explicit SegmentPlanner(config::SegmentationSettings settings) noexcept : settings_(settings) {}

    /// CLAUDE.md hard rule 7: a default-constructed planner never splits, and a planner
    /// built from a default `SegmentationSettings` never splits either.
    [[nodiscard]] bool enabled() const noexcept {
        return settings_.enabled && (armed_by_duration() || armed_by_size());
    }

    /// Offered every **video** packet, in write order, before it is written.
    ///
    /// `pts_ns` is the packet's presentation time on the recording's timeline and
    /// `total_bytes` the muxer's cumulative byte count; both are cumulative because that
    /// is what the muxer reports, and the marks that make them per-segment are kept here.
    ///
    /// Audio packets are not offered. A split lands on a video keyframe by definition,
    /// and letting audio drive the decision would mean a boundary chosen where no video
    /// packet exists.
    [[nodiscard]] Action offer(std::int64_t pts_ns, bool keyframe, std::uint64_t total_bytes) noexcept;

    /// Confirms the split the caller was told to make, and starts the next segment's
    /// measurement from this packet.
    ///
    /// Separate from `offer` because the caller can fail: opening the next file is I/O
    /// and may refuse, and a planner that had already advanced its marks would then be
    /// measuring the new segment from a boundary that never happened.
    void note_split(std::int64_t pts_ns, std::uint64_t total_bytes) noexcept;

    /// One-based index of the file currently being written. 1 until the first split.
    [[nodiscard]] int index() const noexcept {
        return index_;
    }

    /// Completed splits. `index() - 1`, named so a caller reporting statistics does not
    /// have to know that.
    [[nodiscard]] int splits() const noexcept {
        return index_ - 1;
    }

    /// True between a trigger firing and the keyframe that answers it.
    [[nodiscard]] bool awaiting_keyframe() const noexcept {
        return awaiting_keyframe_;
    }

    /// Timeline position at which the current segment began. The sidecar's
    /// `start_offset_ns` for this segment (§11).
    [[nodiscard]] std::int64_t segment_start_ns() const noexcept {
        return segment_start_ns_;
    }

private:
    [[nodiscard]] bool armed_by_duration() const noexcept {
        return settings_.split_by_duration && settings_.duration_minutes > 0;
    }

    [[nodiscard]] bool armed_by_size() const noexcept {
        return settings_.split_by_size && settings_.size_mb > 0;
    }

    config::SegmentationSettings settings_{};

    int index_ = 1;
    bool awaiting_keyframe_ = false;
    /// Set when the first packet of a segment is seen, so a segment's duration is
    /// measured from its own first packet rather than from the recording's epoch.
    bool have_start_ = false;
    std::int64_t segment_start_ns_ = 0;
    std::uint64_t segment_start_bytes_ = 0;
};

} // namespace fc::mux
