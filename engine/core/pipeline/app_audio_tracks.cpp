#include "core/pipeline/app_audio_tracks.h"

#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"
#include "core/util/worker.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace fc::pipeline {
namespace {

/// SPEC.md §8.6: "poll by executable name at 2 Hz".
constexpr std::int64_t kTargetPollIntervalNs = 500'000'000;

/// The watchdog tick, as a fraction of the buffer period. Half, for the reason
/// `AudioPath` records: the §8.2 threshold is two periods, and polling at exactly
/// one period beats against it.
constexpr std::int64_t kMinimumTickNs = 1'000'000;

/// One track: its encoder path, its source, its own silence generator, and what we
/// know about its target.
struct AppTrack {
    AppTrack() = default;
    ~AppTrack() = default;

    AppTrack(const AppTrack&) = delete;
    AppTrack& operator=(const AppTrack&) = delete;
    AppTrack(AppTrack&&) = delete;
    AppTrack& operator=(AppTrack&&) = delete;

    /// 1-based stream index. Track 0 is the system mix and lives in `AudioPath`.
    int index = 0;
    AppTrackConfig config;

    /// One of §8.6's N drift loops: its own timeline, silence generator, drift
    /// compensator, resampler and AAC encoder.
    std::unique_ptr<AudioEncodePath> encode;

    /// Null until a target resolves, and null again once one exits — the client
    /// is initialize-once (§8.6), so a target change is a teardown and not a
    /// reconfiguration.
    std::unique_ptr<audio::IProcessAudioSource> source;

    /// Held for as long as the track follows the target, so the id cannot be
    /// reused underneath it. See `audio::ProcessWatch`.
    audio::ProcessWatch watch;

    bool ever_attached = false;
    /// True while the track's target is gone. Cleared again if it comes back and
    /// re-attachment is enabled — see `AppTracksSettings::reattach`.
    bool target_exited = false;
    bool init_failed = false;
    /// Latched when there is nothing left to look for: either the target went and
    /// re-attachment is off (§8.6 read literally), or the track was pinned to a pid
    /// with no executable name to resolve.
    bool stop_polling = false;

    /// How many times this track's target has gone away, and how many times a
    /// replacement has been picked up. Reported so a silent stretch in the middle of a
    /// track is explicable rather than mysterious.
    std::uint64_t exits = 0;
    std::uint64_t reattachments = 0;

    /// Carried across the source's teardown so the figures survive the target
    /// exiting — the object that counted them is destroyed at that point.
    std::uint64_t retired_buffers = 0;
    std::uint64_t retired_qpc_fallbacks = 0;

    // -----------------------------------------------------------------------
    // This track's SilenceGenerator (SPEC.md §8.2, §8.6)
    // -----------------------------------------------------------------------
    // §8.2 describes the generator as "an independent `SilenceGenerator` timer thread",
    // and §8.6 says "every track therefore needs its own". One thread per track is
    // therefore the literal reading — and it is also the correct one, which is why this
    // is not merely pedantry.
    //
    // **A shared ticker couples the tracks through the one thing that must never couple
    // them.** `request_silence` pushes onto the track's own bounded queue under SPEC.md
    // §12's `Block` policy, so a track whose `aenc` thread has wedged stops accepting
    // ticks once its queue fills — and on a shared thread that blocks *every other
    // track's* tick behind it. Six timelines would then stop advancing because one
    // application's encoder was stuck, which is §8.2's defect (a track shorter than the
    // recording) reached through a mechanism §8.2 never mentions.
    //
    // The cost is five extra threads at §8.6's maximum, each waking every 10 ms and
    // doing one atomic comparison. That is what independence costs and it is cheap.
    std::thread silence_thread;
    std::promise<void> silence_done;
    std::future<void> silence_finished;
    std::mutex silence_mutex;
    std::condition_variable silence_cv;
    bool silence_stop = false;

    void silence_loop(std::chrono::nanoseconds interval);
};

void AppTrack::silence_loop(std::chrono::nanoseconds interval) {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(silence_done);
    set_thread_name("fc-asil" + std::to_string(index));
    try {
        for (;;) {
            {
                std::unique_lock lock(silence_mutex);
                if (silence_cv.wait_for(lock, interval, [this] { return silence_stop; })) {
                    break;
                }
            }
            // Outside the lock, for the reason `AudioPath::silence_loop` records:
            // `request_silence` may block on this track's queue, and blocking while
            // holding a lock `stop` needs is how a shutdown wedges.
            const std::int64_t now = timing::qpc_now_ns();
            if (encode->silence_due_at(now)) {
                encode->request_silence(now);
            }
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "a per-application track's silence generator terminated by an exception",
                     LogFields{}
                         .add("track", static_cast<std::int64_t>(index))
                         .add("what", e.what())
                         .add_error(FcError::AUDIO_SILENCE_GENERATOR_STALLED));
    }
    clear_thread_name();
}

} // namespace

struct AppAudioTracks::Impl {
    Impl() = default;
    ~Impl() = default;

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    AppTracksSettings settings;
    std::function<void(int, encode::EncodedPacket)> sink;
    const timing::PauseClock* pause_clock = nullptr;

    /// `unique_ptr` because `AppTrack` owns threads and non-movable components, and
    /// because the `fc-atracks` thread holds pointers into these across a vector
    /// that must never reallocate under it.
    std::vector<std::unique_ptr<AppTrack>> tracks;

    std::thread poll_thread;
    std::promise<void> poll_done;
    std::future<void> poll_finished;
    std::mutex poll_mutex;
    std::condition_variable poll_cv;
    bool poll_stop = false;

    /// Guards `AppTrack::source`, `watch` and the flags around them: the
    /// `fc-atracks` thread writes them and `stats()` reads them from the IPC thread.
    /// Never held across an encoder call.
    mutable std::mutex track_mutex;

    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> targets_resolved{0};
    std::atomic<std::uint64_t> targets_lost{0};
    std::atomic<std::uint64_t> polls{0};

    void poll_loop();
    /// One pass of §8.6's target poll. `fc-atracks` thread only.
    void poll_targets();

    // ---------------------------------------------------------------------
    // Attach and detach, and why neither holds `track_mutex` while it works
    // ---------------------------------------------------------------------
    // Both of these block: an activation waits on the audio service (up to
    // `kActivationTimeoutMs`), and a teardown joins a capture thread. `stats()` is
    // called from the IPC thread to answer `get_stats` at 2 Hz, and §15.1 gives that
    // command **5 s** — so holding the lock across either of these makes a rare slow
    // activation into a timed-out command, and a wedged one into a GUI that stops
    // updating. It is the same rule as CLAUDE.md hard rule 4, one thread along: the
    // threads here are both allowed to block, and neither is allowed to make the other
    // wait while it does.
    //
    // What makes that safe is that there is exactly **one writer, ever**. These fields
    // are written by `start` (before the poll thread exists), by the `fc-atracks`
    // thread, and by `stop` (after it has been joined) — never by two at once. So the
    // lock is not protecting the *work*, only the moment the result becomes visible to
    // a reader, and it is held for the assignments and nothing else.

    /// Attaches a source to `track` at `pid`. `fc-atracks` thread only, or from
    /// `start` before that thread exists.
    void attach(AppTrack& track, std::uint32_t pid);
    /// Tears the source down and latches the track as silent to the end (§20 row 15).
    void detach(AppTrack& track, bool exited);
};

void AppAudioTracks::Impl::attach(AppTrack& track, std::uint32_t pid) {
    if (pid == 0) {
        return;
    }

    // The handle first. Opening it *after* starting the client would leave a window
    // in which the target exits, its id is reused, and the watch then follows a
    // process this track was never aimed at.
    audio::ProcessWatch watch{pid};
    if (!watch.valid() || !watch.alive()) {
        // Resolved a moment ago and gone already, or not visible to this token.
        // Neither is an error: the poll comes round again.
        return;
    }

    Result<std::unique_ptr<audio::IProcessAudioSource>> created =
        settings.source_factory
            ? settings.source_factory(track.config, pid)
            : Result<std::unique_ptr<audio::IProcessAudioSource>>{std::make_unique<audio::ProcessLoopbackCapture>(pid)};
    if (!created.has_value()) {
        {
            const std::lock_guard lock(track_mutex);
            track.init_failed = true;
        }
        FC_LOG_WARN(Subsystem::Audio, "a per-application track's source could not be built; it stays silent",
                    LogFields{}
                        .add("track", static_cast<std::int64_t>(track.index))
                        .add("pid", static_cast<std::int64_t>(pid))
                        .add_error(created.error()));
        return;
    }

    std::unique_ptr<audio::IProcessAudioSource> source = std::move(created).value();
    AudioEncodePath* const encode = track.encode.get();
    if (const Result<void> began = source->start([encode](const audio::LoopbackBuffer& buffer) {
            // On the source's own capture thread. `offer` copies the bytes and
            // enqueues; nothing here blocks (SPEC.md §12).
            encode->offer(buffer);
        });
        !began.has_value()) {
        {
            const std::lock_guard lock(track_mutex);
            track.init_failed = true;
        }
        // Not fatal, and deliberately so. §8.6 forbids dropping a track from the
        // container, the header has fixed the stream set anyway, and a track that
        // exists and is silent is a far better outcome than a recording that stops
        // because one application could not be intercepted (CLAUDE.md §1).
        FC_LOG_WARN(Subsystem::Audio, "a per-application track failed to start; it continues as silence",
                    LogFields{}
                        .add("track", static_cast<std::int64_t>(track.index))
                        .add("name", track.config.name)
                        .add("pid", static_cast<std::int64_t>(pid))
                        .add_error(FcError::MULTITRACK_TRACK_INIT_FAILED));
        return;
    }

    // The activation is done; this is the only part a reader can see, and it is the
    // only part the lock covers.
    const bool returning = track.target_exited;
    {
        const std::lock_guard lock(track_mutex);
        track.source = std::move(source);
        track.watch = std::move(watch);
        track.ever_attached = true;
        track.init_failed = false;
        // The track is live again. The gap the target was away for is already on the
        // timeline as silence — the generator never stopped — so nothing is shifted by
        // this and `reattachments` is what explains the quiet stretch.
        track.target_exited = false;
        if (returning) {
            ++track.reattachments;
        }
    }
    targets_resolved.fetch_add(1, std::memory_order_relaxed);

    FC_LOG_INFO(Subsystem::Audio,
                returning ? "a per-application track's target came back and was re-attached"
                          : "a per-application track attached to its target",
                LogFields{}
                    .add("track", static_cast<std::int64_t>(track.index))
                    .add("name", track.config.name)
                    .add("executable", track.config.executable)
                    .add("pid", static_cast<std::int64_t>(pid))
                    .add("reattachments", static_cast<std::int64_t>(track.reattachments)));
}

void AppAudioTracks::Impl::detach(AppTrack& track, bool exited) {
    // Taken out of the track under the lock, and *stopped* outside it: `stop()` joins
    // the source's capture thread, which is exactly the wait a reader must not inherit.
    // The counters are banked here rather than after, because the object that holds
    // them is about to be destroyed.
    std::unique_ptr<audio::IProcessAudioSource> retiring;
    const std::uint32_t pid = track.watch.pid();
    {
        const std::lock_guard lock(track_mutex);
        if (track.source) {
            track.retired_buffers += track.source->buffers_captured();
            track.retired_qpc_fallbacks += track.source->qpc_fallbacks();
            retiring = std::move(track.source);
        }
        track.watch.reset();
        if (exited) {
            track.target_exited = true;
            ++track.exits;
            // Whether the poll keeps looking. See the note below the log line.
            track.stop_polling = !settings.reattach || track.config.executable.empty();
        }
    }
    if (retiring) {
        retiring->stop();
        retiring.reset();
    }

    if (!exited) {
        return;
    }

    // SPEC.md §8.6: "target process exits mid-recording → that track continues as
    // pure silence to the end of the file. Never truncate a track; never drop it from
    // the container."
    //
    // **The track's *length* is never in question** — the §8.2 generator keeps its
    // timeline advancing either way, so it ends exactly as long as every other one.
    // What §8.6 leaves open is whether the track keeps *looking*, and the two readings
    // differ only for a user whose application restarts mid-recording:
    //
    //   `reattach = true`   the track follows the **executable**, which is the identity
    //                       §8.6 already uses for a target that has not started yet
    //                       ("poll by executable name at 2 Hz"). A browser that crashes
    //                       and is reopened lands back on its own track, with the gap
    //                       silence-filled. This is the default, decided by the owner on
    //                       2026-08-06.
    //   `reattach = false`  §8.6 read to the letter: pure silence to the end.
    //
    // A track pinned to a **pid** with no executable name latches either way, because
    // there is nothing to resolve: pids are not reused while `ProcessWatch` holds a
    // handle, and a different process with the same number would be a different
    // application.
    targets_lost.fetch_add(1, std::memory_order_relaxed);

    FC_LOG_WARN(Subsystem::Audio,
                track.stop_polling ? "a per-application track's target exited; the track continues as silence"
                                   : "a per-application track's target exited; the track waits for it to return",
                LogFields{}
                    .add("track", static_cast<std::int64_t>(track.index))
                    .add("name", track.config.name)
                    .add("pid", static_cast<std::int64_t>(pid))
                    .add("exits", static_cast<std::int64_t>(track.exits))
                    .add("reattach", !track.stop_polling)
                    .add_error(FcError::MULTITRACK_TARGET_PROCESS_GONE));
}

void AppAudioTracks::Impl::poll_targets() {
    polls.fetch_add(1, std::memory_order_relaxed);

    // Read without the lock: this thread is the only writer of everything it reads
    // here, so its own writes are the only ones it can miss, and it cannot miss those.
    // `attach` and `detach` take the lock themselves, for the assignments alone.
    for (const std::unique_ptr<AppTrack>& held : tracks) {
        AppTrack& track = *held;
        if (track.stop_polling) {
            continue;
        }

        if (track.source) {
            // Attached. The only question is whether the target is still there.
            if (!track.watch.alive()) {
                detach(track, true);
            }
            continue;
        }

        // Unattached: §8.6's "a target process that hasn't started yet → its track
        // is silence until the PID resolves (poll by executable name at 2 Hz)".
        if (track.config.executable.empty()) {
            // Pinned to a PID that is gone, with no name to look for. Nothing to do
            // but stay silent; latched so the poll stops asking.
            track.stop_polling = true;
            continue;
        }
        if (const std::uint32_t pid = audio::find_process_by_executable(track.config.executable); pid != 0) {
            attach(track, pid);
        }
    }
}

/// SPEC.md §8.6's target poll, and nothing else.
///
/// The silence generators used to share this thread, and they do not any more: a poll
/// pass can block for seconds — an activation has a five-second timeout and a teardown
/// joins a capture thread — and a generator that waited behind it would stop advancing
/// its track for exactly that long. They are separate jobs on separate clocks and now
/// on separate threads. See `AppTrack::silence_loop`.
void AppAudioTracks::Impl::poll_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(poll_done);
    set_thread_name("fc-atracks");
    try {
        const auto interval = std::chrono::nanoseconds{kTargetPollIntervalNs};
        for (;;) {
            {
                std::unique_lock lock(poll_mutex);
                if (poll_cv.wait_for(lock, interval, [this] { return poll_stop; })) {
                    break;
                }
            }
            poll_targets();
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "the per-application target poll terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::MULTITRACK_TRACK_INIT_FAILED));
    }
    clear_thread_name();
}

AppAudioTracks::AppAudioTracks() : impl_(std::make_unique<Impl>()) {}

AppAudioTracks::~AppAudioTracks() {
    if (impl_ && impl_->started.load(std::memory_order_acquire) && !impl_->stopped.load(std::memory_order_acquire)) {
        // See `AudioPath::~AudioPath` for why this is caught by type rather than
        // with a catch-all (CLAUDE.md §4).
        try {
            const Result<void> result = stop();
            if (!result.has_value()) {
                FC_LOG_ERROR(Subsystem::Audio, "per-application tracks destroyed while running; shutdown failed",
                             LogFields{}.add_error(result.error()));
            }
        } catch (const std::exception& e) {
            FC_LOG_ERROR(Subsystem::Audio, "per-application tracks destructor could not shut down cleanly",
                         LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
        }
    }
}

void AppAudioTracks::attach_pause_clock(const timing::PauseClock* clock) noexcept {
    impl_->pause_clock = clock;
}

Result<void> AppAudioTracks::start(const AppTracksSettings& settings,
                                   std::function<void(int, encode::EncodedPacket)> sink) {
    if (impl_->started.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (settings.tracks.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (settings.tracks.size() > static_cast<std::size_t>(kMaxAppAudioTracks)) {
        // Refused, not truncated. §8.6 states the limit as a resource bound, and a
        // recording that silently dropped the sixth application the user asked for
        // would be a file that is missing a track nobody was told about.
        FC_LOG_ERROR(Subsystem::Audio, "more per-application tracks were requested than SPEC.md §8.6 allows",
                     LogFields{}
                         .add("requested", static_cast<std::int64_t>(settings.tracks.size() + 1))
                         .add("limit", kMaxAudioTracks)
                         .add_error(FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED));
        return FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED;
    }

    impl_->settings = settings;
    impl_->sink = std::move(sink);

    // Reserved before anything points into it: the `fc-atracks` thread holds
    // `AppTrack*` across polls, and a vector that reallocated would leave it
    // reading freed memory. `unique_ptr` elements make that structurally
    // impossible, and the reserve keeps it cheap.
    impl_->tracks.reserve(settings.tracks.size());

    int index = 0;
    for (const AppTrackConfig& config : settings.tracks) {
        ++index;
        auto track = std::make_unique<AppTrack>();
        track->index = index;
        track->config = config;
        if (track->config.name.empty()) {
            track->config.name = !config.executable.empty() ? config.executable : ("Track " + std::to_string(index));
        }

        AudioEncodeSettings encode_settings;
        // §8.6 supplies the format rather than negotiating it, which is what lets
        // this encoder open before — and independently of — any target existing.
        encode_settings.input =
            settings.source == AppTrackSource::External ? config.external_format : audio::process_loopback_format();
        // `Auto`, and not the recording's channel-layout setting. §8.6 fixes the
        // per-application format at stereo; up-mixing that to the 5.1 a user chose
        // for the system mix would triple the track's bitrate and add no
        // information, because there is none in the source to add.
        encode_settings.channel_layout = config::ChannelLayoutSetting::Auto;
        encode_settings.bitrate_kbps = settings.bitrate_kbps;
        encode_settings.buffer_period_ns = settings.buffer_period_ns;
        encode_settings.thread_label = "fc-aenc" + std::to_string(index);

        track->encode = std::make_unique<AudioEncodePath>();
        track->encode->attach_pause_clock(impl_->pause_clock);

        const int stream_index = index;
        auto* const impl = impl_.get();
        if (const Result<void> opened = track->encode->open(encode_settings,
                                                            [impl, stream_index](encode::EncodedPacket packet) {
                                                                if (impl->sink) {
                                                                    impl->sink(stream_index, std::move(packet));
                                                                }
                                                            });
            !opened.has_value()) {
            // Fatal, unlike a source that will not start: without this encoder there
            // is no stream to add, and `avformat_write_header` has not run yet, so
            // failing here still leaves the caller free to record Tier A instead.
            FC_LOG_ERROR(Subsystem::Audio, "a per-application track's encoder would not open",
                         LogFields{}.add("track", static_cast<std::int64_t>(index)).add_error(opened.error()));
            return opened.error();
        }

        impl_->tracks.push_back(std::move(track));
    }

    // Every track's SilenceGenerator, before any source exists.
    //
    // Before, deliberately: §8.6 makes silence the steady state, and a track whose
    // target never starts is fed *entirely* from here. A generator that only began once
    // a client attached would leave exactly the tracks §8.6 is most concerned about with
    // no timeline at all.
    //
    // `External` drives its ticks from `tick`, on the caller's clock, so it gets no
    // thread — the same reason `AudioSource::External` has no `silence` thread.
    const auto silence_interval = std::chrono::nanoseconds{std::max(settings.buffer_period_ns / 2, kMinimumTickNs)};
    if (settings.source == AppTrackSource::ProcessLoopback) {
        for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
            held->silence_stop = false;
            held->silence_done = std::promise<void>{};
            held->silence_finished = held->silence_done.get_future();
            AppTrack* const track = held.get();
            held->silence_thread = std::thread([track, silence_interval] { track->silence_loop(silence_interval); });
        }
    }

    // Sources second, and only for the production path. Attaching before every
    // encoder exists would let a buffer arrive for a track whose queue is not there.
    if (settings.source == AppTrackSource::ProcessLoopback) {
        // No outer lock: `attach` takes it for the assignments, and the poll thread
        // that would be the other writer does not exist yet.
        for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
            const std::uint32_t pid =
                held->config.pid != 0 ? held->config.pid : audio::find_process_by_executable(held->config.executable);
            if (pid == 0) {
                // §8.6's "hasn't started yet". The poll will find it.
                FC_LOG_INFO(Subsystem::Audio, "a per-application track has no target yet; it starts as silence",
                            LogFields{}
                                .add("track", static_cast<std::int64_t>(held->index))
                                .add("executable", held->config.executable));
                continue;
            }
            impl_->attach(*held, pid);
        }
    }

    impl_->poll_stop = false;
    impl_->poll_done = std::promise<void>{};
    impl_->poll_finished = impl_->poll_done.get_future();
    impl_->started.store(true, std::memory_order_release);

    if (settings.source == AppTrackSource::ProcessLoopback) {
        impl_->poll_thread = std::thread([impl = impl_.get()] { impl->poll_loop(); });
    }

    FC_LOG_INFO(Subsystem::Audio, "per-application audio tracks opened",
                LogFields{}
                    .add("tracks", static_cast<std::int64_t>(impl_->tracks.size()))
                    .add("source", settings.source == AppTrackSource::External ? "external" : "process_loopback")
                    .add("reattach", settings.reattach)
                    .add("limit", kMaxAudioTracks));
    return ok();
}

void AppAudioTracks::set_epoch(std::int64_t t0_ns) noexcept {
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        held->encode->set_epoch(t0_ns);
    }
}

std::vector<const encode::AacEncoder*> AppAudioTracks::encoders() const {
    std::vector<const encode::AacEncoder*> out;
    out.reserve(impl_->tracks.size());
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        out.push_back(held->encode->encoder());
    }
    return out;
}

std::vector<std::string> AppAudioTracks::names() const {
    std::vector<std::string> out;
    out.reserve(impl_->tracks.size());
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        out.push_back(held->config.name);
    }
    return out;
}

int AppAudioTracks::track_count() const noexcept {
    return static_cast<int>(impl_->tracks.size());
}

void AppAudioTracks::offer(int index, const audio::LoopbackBuffer& buffer) {
    if (index < 1 || std::cmp_greater(index, impl_->tracks.size())) {
        return;
    }
    impl_->tracks[static_cast<std::size_t>(index - 1)]->encode->offer(buffer);
}

void AppAudioTracks::tick(std::int64_t now_ns) {
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        if (held->encode->silence_due_at(now_ns)) {
            held->encode->request_silence(now_ns);
        }
    }
}

Result<void> AppAudioTracks::stop() {
    if (!impl_->started.load(std::memory_order_acquire)) {
        return ok();
    }
    if (impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return ok();
    }

    // The poll thread first, so nothing attaches a source while sources are being
    // torn down.
    {
        const std::lock_guard lock(impl_->poll_mutex);
        impl_->poll_stop = true;
    }
    impl_->poll_cv.notify_all();
    Result<void> outcome = ok();
    if (!await_worker(impl_->poll_finished, impl_->poll_thread, "fc-atracks", Subsystem::Audio)) {
        outcome = FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
    }

    // Then every track's SilenceGenerator, before its source goes: a tick that ran
    // after the source was released would be harmless, but one that ran after the
    // encoder was flushed would push onto a closed queue and be counted as a lost
    // buffer that never existed.
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        {
            const std::lock_guard lock(held->silence_mutex);
            held->silence_stop = true;
        }
        held->silence_cv.notify_all();
        const std::string label = "fc-asil" + std::to_string(held->index);
        if (!await_worker(held->silence_finished, held->silence_thread, label.c_str(), Subsystem::Audio)) {
            outcome = FcError::INTERNAL_THREAD_JOIN_TIMEOUT;
        }
    }

    // No outer lock, for the reason `attach` records: `detach` joins a capture thread,
    // and the poll thread that would be the other writer has already been joined above.
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        impl_->detach(*held, false);
    }

    // Then flush. Each path drains its own queue, flushes its own encoder and joins
    // its own `aenc` thread; they are independent by construction, so a track that
    // will not shut down does not hold the others.
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        if (const Result<void> stopped = held->encode->stop(); !stopped.has_value()) {
            FC_LOG_ERROR(Subsystem::Audio, "a per-application track did not shut down cleanly; finalizing anyway",
                         LogFields{}
                             .add("track", static_cast<std::int64_t>(held->index))
                             .add("name", held->config.name)
                             .add_error(stopped.error()));
            if (outcome.has_value()) {
                outcome = stopped.error();
            }
        }
    }

    const AppTracksStats final_stats = stats();
    for (const AppTrackStats& track : final_stats.tracks) {
        // One line per track, at INFO, once. The three numbers that tell a silent
        // track apart from a broken one: how long it is, how much of that was
        // manufactured, and whether anything was ever attached to it.
        FC_LOG_INFO(Subsystem::Audio, "per-application track finished",
                    LogFields{}
                        .add("track", static_cast<std::int64_t>(track.index))
                        .add("name", track.name)
                        .add("timeline_s", track.audio.timeline_seconds)
                        .add("silence_s", track.audio.silence_seconds)
                        .add("buffers", static_cast<std::int64_t>(track.source_buffers))
                        .add("attached", track.ever_attached)
                        .add("target_exited", track.target_exited)
                        .add("exits", static_cast<std::int64_t>(track.exits))
                        .add("reattachments", static_cast<std::int64_t>(track.reattachments))
                        .add("qpc_fallbacks", static_cast<std::int64_t>(track.qpc_fallbacks))
                        .add("worst_drift_us", track.audio.worst_drift_ns / 1000));
    }
    return outcome;
}

AppTracksStats AppAudioTracks::stats() const {
    AppTracksStats out;
    out.targets_resolved = impl_->targets_resolved.load(std::memory_order_relaxed);
    out.targets_lost = impl_->targets_lost.load(std::memory_order_relaxed);
    out.polls = impl_->polls.load(std::memory_order_relaxed);
    out.tracks.reserve(impl_->tracks.size());

    const std::lock_guard lock(impl_->track_mutex);
    for (const std::unique_ptr<AppTrack>& held : impl_->tracks) {
        AppTrackStats track;
        track.index = held->index;
        track.name = held->config.name;
        track.executable = held->config.executable;
        track.pid = held->watch.pid();
        track.attached = static_cast<bool>(held->source);
        track.ever_attached = held->ever_attached;
        track.target_exited = held->target_exited;
        track.exits = held->exits;
        track.reattachments = held->reattachments;
        track.init_failed = held->init_failed;
        track.source_buffers = held->retired_buffers + (held->source ? held->source->buffers_captured() : 0);
        track.qpc_fallbacks = held->retired_qpc_fallbacks + (held->source ? held->source->qpc_fallbacks() : 0);
        track.audio = held->encode->stats();
        out.tracks.push_back(std::move(track));
    }
    return out;
}

} // namespace fc::pipeline
