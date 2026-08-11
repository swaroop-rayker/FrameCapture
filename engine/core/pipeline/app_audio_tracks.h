#pragma once

// SPEC.md §8.6 Tier B — the per-application audio tracks.
//
// ---------------------------------------------------------------------------
// What this owns, and what it deliberately does not
// ---------------------------------------------------------------------------
// **Track 0 is not here.** §8.6 makes track 0 "always the full system mix (Tier A
// output) so the file is useful even in a player that exposes only the first
// track", and Tier A's system mix is `AudioPath` — capture, silence watchdog and
// `AudioEncodePath`, exactly as M4 shipped it. This class owns tracks **1 to 5**
// and nothing else.
//
// That split is the whole of how §24's "Tier A provably unaffected when Tier B is
// off" is made structural rather than promised. Turning Tier B on does not put
// track 0 on a different code path, wrap it in an abstraction, or give it a new
// neighbour to share a lock with: it constructs a second object beside it. There
// is no `if (multitrack)` anywhere in `AudioPath`, `AudioEncodePath`,
// `AudioTimeline`, `DriftCompensator` or `Resampler`, and the measurement in
// docs/ACCEPTANCE.md is what checks that the claim survived contact with the
// muxer and the mux queue, which the two tracks *do* share.
//
// ---------------------------------------------------------------------------
// N tracks = N drift loops (§8.6), and why that costs nothing new
// ---------------------------------------------------------------------------
// > Each track carries its **own independent device clock**. Every track therefore
// > needs its own `SilenceGenerator` (§8.2) and its own drift compensator (§8.4),
// > all reconciled against the single QPC master clock (§7.1). N tracks = N drift
// > loops, not one shared one.
//
// `AudioEncodePath` already *is* one of those loops: it owns an `AudioTimeline`, a
// `DriftCompensator`, a `Resampler` and an `AacEncoder`, holds no global state, and
// takes its epoch from outside. So a track is one of those plus a source, and the
// reconciliation §8.6 asks for is the single `set_epoch(t0)` every track receives —
// the same `t0` the video pacer and track 0 were given (§7.1).
//
// ---------------------------------------------------------------------------
// Silence is the primary path, not the edge case
// ---------------------------------------------------------------------------
// > A per-app track is silent far more often than the system mix. Silence injection
// > is not an edge case here — it is the steady state. Test it as the primary path.
//
// Three separate reasons a track produces nothing, all of which look identical from
// below and all of which resolve to the same answer — the §8.2 watchdog advances the
// timeline:
//   * the target is playing nothing (the ordinary case, most of a recording);
//   * the target has not started yet (§8.6: poll by executable name at 2 Hz);
//   * the target has exited (§20 row 15: "continues as pure silence to the end").
//
// ---------------------------------------------------------------------------
// Why every track's encoder must exist before the first one has a source
// ---------------------------------------------------------------------------
// `avformat_write_header` fixes the stream set (SPEC.md §10.1), so every track's
// `AacEncoder` has to be open before the muxer is. For Tier A that is why the audio
// path opens before the muxer at all — the stream's parameters come from the
// endpoint's mix format, which is not known until the endpoint is open.
//
// For Tier B there *is* no endpoint to ask: §8.6 makes the process-loopback format
// **supplied** rather than negotiated. That is what makes a track for a process
// which has not started yet expressible at all — its encoder is opened from a
// constant, and the file can carry a `game.exe` track that stays silent because the
// game was never launched. A negotiated format would have made "poll for the PID at
// 2 Hz" impossible to honour, because the stream set would depend on the answer.

#include "core/audio/process_loopback.h"
#include "core/config/config_schema.h"
#include "core/encode/aac_encoder.h"
#include "core/error/result.h"
#include "core/pipeline/audio_path.h"
#include "core/timing/pause_clock.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fc::pipeline {

/// SPEC.md §8.6: "Max **6** tracks. Track 0 is **always** the full system mix."
inline constexpr int kMaxAudioTracks = 6;
/// So five per-application tracks, at most.
inline constexpr int kMaxAppAudioTracks = kMaxAudioTracks - 1;

/// Where one per-application track's buffers come from.
///
/// Mirrors `AudioSource` deliberately — the vocabulary is established and the
/// reasons are the same one milestone later.
enum class AppTrackSource {
    /// `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`. What every real recording uses.
    ProcessLoopback,

    /// The caller supplies buffers through `offer` and drives the silence watchdog
    /// through `tick`.
    ///
    /// SPEC.md §20 row 14 asserts "per-track sync < 20 ms over 30 min" between
    /// tracks carrying distinct tones. That is a claim about where the timeline
    /// *puts* audio, and it only means anything if every track's signal is
    /// generated from one clock — the same argument `AudioSource::External` records
    /// for row 4, and the same reason CLAUDE.md §5 forbids testing against whatever
    /// the machine happens to be playing. Routing it through a real render →
    /// process-loopback round trip instead would measure Windows' end-to-end audio
    /// latency, which is neither bounded by this project nor what row 14 is about.
    ///
    /// The real client is exercised by row 15, against a real target process.
    /// Nothing in the engine selects this.
    External,
};

/// One per-application track (SPEC.md §8.6's tracks 1 through 5).
struct AppTrackConfig {
    /// The Matroska `Name` tag. §8.6: "Every track gets a human-readable Matroska
    /// `Name` tag (`"System Mix"`, `"chrome.exe"`, `"game.exe"`). Untagged tracks
    /// are a UX failure." Empty falls back to `executable`, and if that is empty
    /// too, to `Track N` — a name that is unhelpful is still better than none.
    std::string name;

    /// The image name to follow, e.g. `chrome.exe`. Resolved at start and, while
    /// it stays unresolved, re-polled at 2 Hz (§8.6).
    std::string executable;

    /// A pre-resolved target. 0 means resolve from `executable`.
    ///
    /// Present because the GUI's picker lists running processes and hands back the
    /// one the user clicked: re-resolving that by name would risk selecting a
    /// different instance of the same executable.
    std::uint32_t pid = 0;

    /// `External` only: the format the caller's buffers arrive in.
    audio::MixFormat external_format = audio::process_loopback_format();
};

/// Builds the source for one track. Empty selects `ProcessLoopbackCapture`, which
/// is what production does.
///
/// The same seam `SessionSettings::capture_factory` is, for the same reason: it
/// supplies the input without removing any decision above it.
using AppTrackSourceFactory =
    std::function<Result<std::unique_ptr<audio::IProcessAudioSource>>(const AppTrackConfig&, std::uint32_t pid)>;

struct AppTracksSettings {
    AppTrackSource source = AppTrackSource::ProcessLoopback;

    /// Tracks 1..N. More than `kMaxAppAudioTracks` is refused with
    /// `MULTITRACK_TRACK_LIMIT_EXCEEDED` rather than silently truncated.
    std::vector<AppTrackConfig> tracks;

    /// 0 selects SPEC.md §8.5's ladder, which for these tracks means 192 kbps —
    /// they are stereo by construction (§8.6 supplies the format).
    int bitrate_kbps = 0;

    /// SPEC.md §8.1's 20 ms buffer, which also sets each track's watchdog threshold.
    std::int64_t buffer_period_ns = 20'000'000;

    /// Whether a track whose target exits keeps looking for it to come back.
    ///
    /// SPEC.md §8.6 says a track whose target exits "continues as pure silence to the
    /// end of the file", and leaves open whether it keeps *looking*. **Decided
    /// 2026-08-06 by the owner: it looks.** A track follows its **executable**, which is
    /// already the identity §8.6 uses for a target that has not started yet — so a
    /// browser that crashes and is reopened lands back on its own track with the gap
    /// silence-filled, rather than leaving the rest of the recording silent for a reason
    /// the user cannot see.
    ///
    /// The track's *length* is identical either way; the generator never stops. False
    /// gives §8.6's literal reading.
    ///
    /// A track pinned to a **pid** with no executable name never re-attaches, whatever
    /// this says: there is nothing to resolve.
    bool reattach = true;

    /// `ProcessLoopback` only. Empty selects the real client.
    AppTrackSourceFactory source_factory;
};

/// What one track did, for `get_stats`, the log and rows 14 and 15.
struct AppTrackStats {
    /// Stream index in the file. 1-based: track 0 is the system mix.
    int index = 0;
    std::string name;
    std::string executable;
    std::uint32_t pid = 0;

    /// True while a source is attached and delivering.
    bool attached = false;
    /// True once a source has ever attached. A track that is false here for a whole
    /// recording is one whose target never ran — legitimate, and worth being able to
    /// tell apart from one that ran and was silent.
    bool ever_attached = false;
    /// True while this track's target is gone. Cleared if it comes back and
    /// `AppTracksSettings::reattach` is on.
    bool target_exited = false;
    /// How many times the target has gone away, and how many times a replacement has
    /// been picked up. A quiet stretch in the middle of a track is explicable from
    /// these two and mysterious without them.
    std::uint64_t exits = 0;
    std::uint64_t reattachments = 0;
    /// Set when the process-loopback client refused to start
    /// (`MULTITRACK_TRACK_INIT_FAILED`). The track still exists in the file and is
    /// still silence-padded to full duration — §8.6 forbids dropping a track from
    /// the container, and a track that vanished would change the stream set the
    /// header already fixed.
    bool init_failed = false;

    /// Buffers the source delivered, and the ones it stamped from the engine clock
    /// because the virtual device supplied no QPC position.
    std::uint64_t source_buffers = 0;
    std::uint64_t qpc_fallbacks = 0;

    /// This track's own timeline, silence and drift figures. The per-track half of
    /// §8.6's "N tracks = N drift loops".
    AudioStats audio;
};

struct AppTracksStats {
    std::vector<AppTrackStats> tracks;
    /// Targets that resolved from an executable name during the recording.
    std::uint64_t targets_resolved = 0;
    /// Targets that exited during it (§20 row 15).
    std::uint64_t targets_lost = 0;
    /// Passes of the 2 Hz poll (§8.6).
    std::uint64_t polls = 0;
};

/// Owns tracks 1..N of a Tier B recording.
///
/// Threading (SPEC.md §12's table, amended 2026-08-06 to carry these):
///   `fc-atracks`   one for the whole set. §8.6's 2 Hz target poll and nothing else:
///                  resolving an executable to a pid, noticing one has gone, and
///                  attaching or detaching a client. Blocking is allowed and expected
///                  — an activation waits on the audio service and a teardown joins a
///                  capture thread.
///   `fc-asilN`     one **per track**: its §8.2 SilenceGenerator, waking every half
///                  buffer period. Blocking allowed, as Tier A's `fc-silence` is.
///   `fc-ploop-PID` one per attached process-loopback client, its own capture thread,
///                  MMCSS "Pro Audio" (SPEC.md §8.1). Never blocks.
///   `fc-aencN`     one per track, inside its `AudioEncodePath`.
///
/// At §8.6's six-track maximum that is eleven threads a Tier A recording does not
/// have, which is what §8.6 means by "the real engineering cost of Tier B and the
/// reason it is opt-in".
///
/// **Why the generators are per track and not one shared ticker.** §8.2 describes the
/// generator as "an independent `SilenceGenerator` timer thread" and §8.6 says every
/// track needs its own, so per-track is the literal reading — and it is the correct
/// one for a reason worth stating, because a shared ticker looks strictly cheaper.
///
/// `request_silence` pushes onto the track's own bounded queue under §12's `Block`
/// policy. A track whose `aenc` thread has wedged therefore stops accepting ticks once
/// that queue fills — and on a shared ticker it blocks *every other track's* tick
/// behind it. Six timelines would stop advancing because one application's encoder was
/// stuck, which is §8.2's defect (a track shorter than the recording) arriving through
/// a coupling §8.2 never mentions. The poll thread has the same shape and is separate
/// for the same reason: a five-second activation timeout must not hold six generators.
///
/// The cost is five threads waking every 10 ms to do one atomic comparison. That is
/// what independence costs here and it is cheap.
class AppAudioTracks {
public:
    AppAudioTracks();
    ~AppAudioTracks();

    AppAudioTracks(const AppAudioTracks&) = delete;
    AppAudioTracks& operator=(const AppAudioTracks&) = delete;
    AppAudioTracks(AppAudioTracks&&) = delete;
    AppAudioTracks& operator=(AppAudioTracks&&) = delete;

    /// Opens every track's encoder and starts the sources that can start.
    ///
    /// `sink` is invoked with the track's **1-based stream index** and the packet,
    /// from that track's own `aenc` thread.
    ///
    /// A track whose target has not started, or whose client refused, is **not** a
    /// failure: its encoder is open, its stream will exist, and it carries silence
    /// until something changes (§8.6). `start` fails only for a configuration that
    /// cannot produce a valid file at all — too many tracks, or an encoder that
    /// would not open.
    [[nodiscard]] Result<void> start(const AppTracksSettings& settings,
                                     std::function<void(int, encode::EncodedPacket)> sink);

    /// Points every track at the recording's shared paused total (SPEC.md §7.5).
    /// Must be called before `start`.
    void attach_pause_clock(const timing::PauseClock* clock) noexcept;

    /// Publishes the shared epoch (SPEC.md §7.1) to every track.
    ///
    /// This is where §8.6's "silence-padded from `t0`, never offset" is delivered:
    /// `AudioEncodePath::set_epoch` also moves each track's watchdog origin up to
    /// `t0`, so a track that produces its first buffer ten minutes in has ten
    /// minutes of silence in front of it rather than a ten-minute head start.
    void set_epoch(std::int64_t t0_ns) noexcept;

    /// Every track's AAC encoder, in stream order, for `Muxer::open`. All non-null
    /// after a successful `start`.
    [[nodiscard]] std::vector<const encode::AacEncoder*> encoders() const;

    /// Every track's Matroska `Name`, in stream order (§8.6).
    [[nodiscard]] std::vector<std::string> names() const;

    [[nodiscard]] int track_count() const noexcept;

    /// `External` only: hands one buffer to track `index` (1-based).
    void offer(int index, const audio::LoopbackBuffer& buffer);

    /// `External` only: runs one watchdog pass over every track against `now_ns`.
    ///
    /// Separate from `offer` for the reason `VideoPipeline::tick_audio_silence`
    /// gives: a synthetic clock has no thread to run on.
    void tick(std::int64_t now_ns);

    /// Stops every source and flushes every encoder. Idempotent.
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] AppTracksStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::pipeline
