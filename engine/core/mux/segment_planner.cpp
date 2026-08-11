#include "core/mux/segment_planner.h"

#include <string>

namespace fc::mux {
namespace {

constexpr std::int64_t kNsPerMinute = 60LL * 1'000'000'000LL;
constexpr std::uint64_t kBytesPerMb = 1024ULL * 1024ULL;

} // namespace

std::filesystem::path segment_path(const std::filesystem::path& base, int index) {
    std::string suffix = std::to_string(index > 0 ? index : 1);
    // Zero-padded to three, and left alone once it is wider -- §11's "auto-widening past
    // 999". Truncating to three would make `_part1000` collide with `_part000`, which is
    // a recording overwriting itself after sixteen hours at the default trigger.
    while (suffix.size() < 3) {
        suffix.insert(suffix.begin(), '0');
    }

    std::filesystem::path out = base;
    out.replace_filename(base.stem().string() + "_part" + suffix + base.extension().string());
    return out;
}

SegmentPlanner::Action SegmentPlanner::offer(std::int64_t pts_ns, bool keyframe, std::uint64_t total_bytes) noexcept {
    if (!enabled()) {
        return Action::Continue;
    }

    // A segment is measured from its own first packet, not from the recording's epoch.
    // Latching here rather than in `note_split` covers the first segment too, whose start
    // no split ever announced.
    if (!have_start_) {
        have_start_ = true;
        segment_start_ns_ = pts_ns;
        segment_start_bytes_ = total_bytes;
    }

    if (awaiting_keyframe_) {
        // §11: the split lands *at* the keyframe, and everything before it belongs to the
        // file being closed. Until one arrives the recording keeps writing where it was.
        return keyframe ? Action::Split : Action::Continue;
    }

    const std::int64_t elapsed = pts_ns - segment_start_ns_;
    const std::uint64_t written = total_bytes >= segment_start_bytes_ ? total_bytes - segment_start_bytes_ : 0;

    // Whichever fires first wins, which falls out of testing them independently -- there
    // is no ordering between them because either one arming the request is enough.
    const bool duration_due =
        armed_by_duration() && elapsed >= (static_cast<std::int64_t>(settings_.duration_minutes) * kNsPerMinute);
    const bool size_due = armed_by_size() && written >= (static_cast<std::uint64_t>(settings_.size_mb) * kBytesPerMb);

    if (!duration_due && !size_due) {
        return Action::Continue;
    }

    // The trigger has fired. If this packet already *is* a keyframe the wait is over
    // before it began and the split happens here -- asking the encoder for an IDR it has
    // just produced would cost a whole extra GOP for nothing.
    if (keyframe) {
        return Action::Split;
    }

    awaiting_keyframe_ = true;
    return Action::RequestKeyframe;
}

void SegmentPlanner::note_split(std::int64_t pts_ns, std::uint64_t total_bytes) noexcept {
    ++index_;
    awaiting_keyframe_ = false;
    have_start_ = true;
    segment_start_ns_ = pts_ns;
    segment_start_bytes_ = total_bytes;
}

} // namespace fc::mux
