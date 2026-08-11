#pragma once

// Crash recovery for an unfinalised recording (SPEC.md §10.3, §10.4).
//
// ---------------------------------------------------------------------------
// What can be lost, and what cannot
// ---------------------------------------------------------------------------
// A recording dies in one of two places, and they need different answers.
//
// **Killed mid-recording.** SPEC.md §10.3's fragmented MP4 means the file on disk
// is already valid and playable up to its last complete fragment; Matroska's
// clusters give the same property (§10.2). Nothing is lost and nothing has to be
// repaired for the file to open. What the user does *not* have is a progressive
// MP4 with the `moov` atom at the front, which is what makes seeking instant and
// what some editors insist on.
//
// **Killed mid-finalize.** The remux was underway. Depending on when the process
// died there may be a half-written temp file next to the output. The output itself
// is untouched, because the remux never writes over it in place -- it writes a
// temp and atomically replaces (SPEC.md §17's sequence, reused).
//
// Either way the recording survives. Recovery is about *upgrading* a survivable
// file to a finished one, never about rescuing data, and that distinction is why
// none of this is on the prime directive's critical path.
//
// ---------------------------------------------------------------------------
// The sidecar
// ---------------------------------------------------------------------------
// > a `.fcrecover` sidecar (written at recording start, containing container type,
// > codec params, and expected output path) lets the GUI's recovery path complete
// > the job on next launch.
//
// Its presence is the signal: written when recording starts, removed when
// finalization succeeds. A sidecar still on disk means a recording did not finish,
// and the file beside it is the one to repair. It carries what a recovery pass and
// a user-facing notice need — container, path, geometry, codecs — so neither has
// to probe the media to find out what it was supposed to be.

#include "core/config/config_schema.h"
#include "core/error/result.h"
#include "core/mux/muxer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fc::mux {

/// The extension appended to the output path. `capture.mp4` →
/// `capture.mp4.fcrecover`, so the sidecar sorts next to its recording and cannot
/// collide with a second recording's.
inline constexpr const char* kRecoverySuffix = ".fcrecover";

/// What was being recorded, as of the moment recording started.
struct RecoveryRecord {
    config::Container container = config::Container::Mkv;
    /// Where the recording is being written. Absolute.
    std::filesystem::path output;

    /// Engine version and log session id, so a recovered file can be traced back
    /// to the run that produced it.
    std::string engine_version;
    std::string session_id;

    /// Wall-clock start, ISO-8601 UTC, for a human-readable notice.
    ///
    /// Wall clock deliberately, and this is the one place it is correct: CLAUDE.md
    /// bans `system_clock` for *media timing*, where QPC is the only clock, and
    /// "the recording you lost was the one from 14:32" is not media timing.
    std::string started_utc;

    int width = 0;
    int height = 0;
    int fps = 0;
    std::string video_codec;

    /// Empty when the recording had no audio track.
    std::string audio_codec;
    int audio_channels = 0;
    int audio_sample_rate = 0;

    /// The AAC encoder's priming, in samples. Recorded because a fragmented MP4
    /// cannot carry it and the repair has to put it back, or the recovered file
    /// begins with 21 ms of priming presented as audio (BUG-021).
    int audio_initial_padding = 0;
};

/// `<output><kRecoverySuffix>`.
[[nodiscard]] std::filesystem::path sidecar_path_for(const std::filesystem::path& output);

/// Writes the sidecar atomically, so a crash during the write cannot leave a
/// half-parsed record that recovery would then act on.
[[nodiscard]] Result<void> write_recovery_record(const RecoveryRecord& record);

/// Reads one back. Fails with `MUX_RECOVERY_FAILED` on a record this build cannot
/// make sense of, rather than guessing at a partially recognised one.
[[nodiscard]] Result<RecoveryRecord> read_recovery_record(const std::filesystem::path& sidecar);

/// Removes the sidecar. Called once finalization has succeeded and validated.
///
/// `noexcept` and silent on failure: by the time this runs the recording is
/// finished and validated, and a leftover sidecar costs one spurious "unfinished
/// recording" prompt — which is a far better outcome than failing a completed
/// recording over a file deletion.
void clear_recovery_record(const std::filesystem::path& output) noexcept;

/// Every sidecar in `directory`, for the launch-time scan (SPEC.md §10.4).
///
/// Not recursive: recordings land in the configured output directory, and walking
/// a user's whole disk looking for them is not this function's business.
[[nodiscard]] std::vector<std::filesystem::path> find_recoverable(const std::filesystem::path& directory);

/// What a recovery attempt did.
struct RecoveryOutcome {
    /// True when the file was rewritten.
    ///
    /// An MP4 is always rewritten, even one that turns out to have been finalized
    /// already -- which happens when the process died in the window between a
    /// successful finalize and the sidecar's removal. Telling those apart means
    /// parsing the box structure for a `moof`, and the remux is idempotent and
    /// cheap, so the end state is guaranteed rather than inferred.
    bool repaired = false;

    /// Whether the recording is usable now, repaired or not. This is the field a
    /// caller should branch on.
    bool valid = false;

    std::filesystem::path output;
    ValidationReport report;
    std::string detail;
};

/// Completes an unfinished recording described by `sidecar` (SPEC.md §10.3's
/// "Repair & finalize").
///
/// For MP4 this is the same lossless remux a clean stop performs. For MKV the file
/// is validated and left alone: a truncated Matroska is playable to its last
/// complete cluster and rewriting it would risk a working recording to add an
/// index. The sidecar is removed only when the recording ends up valid.
[[nodiscard]] Result<RecoveryOutcome> recover(const std::filesystem::path& sidecar);

} // namespace fc::mux
