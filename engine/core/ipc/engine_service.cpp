#include "core/ipc/engine_service.h"

#include "core/audio/loopback_capture.h"
#include "core/audio/process_loopback.h"
#include "core/capture/source_resolver.h"
#include "core/config/config_schema.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/log_fields.h"
#include "core/logging/logger.h"
#include "core/mux/recovery.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace fc::ipc {
namespace {

/// SPEC.md §15.1's `stats` event rate.
constexpr std::chrono::milliseconds kStatsInterval{500};

[[nodiscard]] std::string environment_value(const char* name) {
    std::array<char, 256> buffer{};
    const DWORD length = ::GetEnvironmentVariableA(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return (length == 0 || length >= buffer.size()) ? std::string{} : std::string{buffer.data(), length};
}

/// Reads an optional field without making its absence an error.
///
/// The shape of SPEC.md §15.1's compatibility rule at the field level: a GUI omitting a
/// key gets the engine's configured default rather than a rejection.
template <typename T>
[[nodiscard]] T field_or(const nlohmann::json& object, const char* key, T fallback) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return fallback;
    }
    try {
        return found->get<T>();
    } catch (const nlohmann::json::exception&) {
        // A field of the wrong *type* is a caller mistake, not a version difference,
        // and it is reported as the default rather than as a failure for the same
        // reason: one bad field must not make the whole command unusable.
        return fallback;
    }
}

} // namespace

struct EngineService::Impl {
    EngineServiceSettings settings;

    PipeServer pipe;
    SingleInstanceGuard instance;
    JobMembership job;
    ShutdownSignals signals;
    GuiWatchdog watchdog;

    config::Config config;
    gpu::GpuTopologyService topology;

    std::unique_ptr<pipeline::RecordingSession> session;
    std::mutex session_mutex;
    std::filesystem::path current_output;

    // --- SPEC.md §15.2's preview ------------------------------------------------
    /// The section, owned here so it outlives every session. See the header note.
    preview::PreviewRing preview_ring;
    /// Runs only while the preview is armed and nothing is recording.
    std::unique_ptr<pipeline::RecordingSession> preview_session;

    // The small members are together deliberately rather than beside the things they
    // describe: `clang-analyzer-optin.performance.Padding` counts, and a `bool` dropped
    // between two pointers costs seven bytes of padding each time.
    RecordingState state = RecordingState::Idle;
    /// True between `start_preview` and `stop_preview`. The ring being open is not the
    /// same question: the ring stays open across a `start_record`, and it is this flag
    /// that decides whether a preview-only session comes back after a `stop_record`.
    bool preview_armed = false;

    std::thread stats_thread;
    std::atomic<bool> stats_running{false};

    mutable std::mutex exit_mutex;
    std::condition_variable exit_wake;
    bool exit_requested = false;
    std::string exit_reason;
    std::atomic<bool> finalized_on_exit{false};

    /// Wakes `run`. Reachable from the console handler, the window proc, the heartbeat
    /// thread and the `fc-ipc` thread, so it does the least it can: take a short
    /// mutex, set two fields, notify. Nothing that can block for long, because two of
    /// those callers are on OS-owned threads under a deadline.
    void request_shutdown_internal(std::string_view reason) {
        {
            const std::lock_guard lock(exit_mutex);
            if (exit_requested) {
                return; // first reason wins; it is the one that actually happened
            }
            exit_requested = true;
            exit_reason = std::string{reason};
        }
        exit_wake.notify_all();
    }

    [[nodiscard]] std::string handle(const Request& request);
    void stats_loop();
    void publish_state(RecordingState next);

    /// Finalizes whatever is recording, on any exit path. Idempotent.
    void finalize_for_exit();

    [[nodiscard]] std::string handle_start_record(const Request& request);
    [[nodiscard]] std::string handle_stop_record(const Request& request);
    [[nodiscard]] std::string handle_start_preview(const Request& request);
    [[nodiscard]] std::string handle_stop_preview(const Request& request);
    [[nodiscard]] nlohmann::json stats_payload();
    [[nodiscard]] nlohmann::json health_payload();

    /// What the GUI needs to attach to the ring (SPEC.md §15.2). Shared by `start_preview`
    /// and `get_stats`, so a GUI that missed the response can still find the section.
    [[nodiscard]] nlohmann::json preview_payload() const;

    /// Starts a preview-only session, if the preview is armed and nothing is recording.
    /// Call with `session_mutex` held. Best-effort: a preview that will not start is
    /// logged and the engine stays idle rather than faulted.
    void start_preview_session_locked();

    /// Stops the preview-only session if there is one. Call with `session_mutex` held.
    void stop_preview_session_locked();

    /// Fills in the preview fields every session shares.
    void apply_preview_to_locked(pipeline::SessionSettings& session_settings);

    /// Everything the GUI can set, as the engine currently holds it.
    ///
    /// Shared by `configure`, `get_config` and `save_config` so the three cannot report
    /// different pictures of the same state -- a `configure` that echoed one shape and a
    /// `get_config` that returned another is how a settings dialog ends up showing a
    /// value the engine is not using.
    [[nodiscard]] nlohmann::json config_payload() const;

    /// Merges a partial settings object into `target`. Absent keys keep their value.
    ///
    /// Takes the target by reference rather than writing `config` directly, so a
    /// caller can build a **candidate** and refuse it whole. SPEC.md §20 row 16's
    /// block does exactly that: a settings dialog that asked for multi-track on MP4
    /// has to be told no with nothing applied, not left with half its request in
    /// force and one key rejected.
    static void apply_config_params(config::Config& target, const nlohmann::json& params);

    /// Merges SPEC.md §8.6's Tier B keys into `target`.
    ///
    /// Separate and shared, because `configure` and `save_config` both accept them
    /// and this milestone is not the place to let a third copy of "what does this
    /// key mean" appear -- BUG-043's shape, and the reason `extension_for` exists.
    static void apply_multitrack_params(config::Config& target, const nlohmann::json& params);

    /// SPEC.md §20 row 16, in one place.
    ///
    /// > **MP4 is a hard block, not a soft warning.** If the container is MP4, Tier B
    /// > is unavailable in the GUI with an inline reason string, and the engine
    /// > rejects a `configure` command that requests both with
    /// > `FcError::MULTITRACK_REQUIRES_MKV` (3021).
    ///
    /// "Enforced engine-side, not just in the GUI" is the row's own wording, so this
    /// runs whether or not a GUI is involved -- and it runs again in
    /// `VideoPipeline::start`, because `start_record` resolves the container from the
    /// output path rather than from this setting (BUG-043).
    [[nodiscard]] static bool multitrack_conflicts(const config::Config& candidate) noexcept;
};

bool EngineService::Impl::multitrack_conflicts(const config::Config& candidate) noexcept {
    return candidate.audio.multitrack_enabled && candidate.video.container != config::Container::Mkv;
}

void EngineService::Impl::apply_multitrack_params(config::Config& target, const nlohmann::json& params) {
    target.audio.multitrack_enabled = field_or<bool>(params, "multitrack_enabled", target.audio.multitrack_enabled);
    target.audio.multitrack_targets =
        field_or<std::string>(params, "multitrack_targets", target.audio.multitrack_targets);
    target.audio.multitrack_reattach = field_or<bool>(params, "multitrack_reattach", target.audio.multitrack_reattach);
    target.audio.max_tracks = field_or<int>(params, "max_tracks", target.audio.max_tracks);
}

nlohmann::json EngineService::Impl::config_payload() const {
    nlohmann::json body = nlohmann::json::object();
    body["output_directory"] = config.general.output_directory.string();
    body["filename_template"] = config.general.filename_template;
    body["language"] = config.general.language;

    body["fps"] = config.video.fps;
    body["cqp"] = config.video.cqp;
    body["bitrate_kbps"] = config.video.bitrate_kbps;
    body["container"] = std::string{config::to_string(config.video.container)};
    body["encoder"] = std::string{config::to_string(config.video.encoder)};

    body["audio_device"] = config.audio.device_id;
    body["channel_layout"] = std::string{config::to_string(config.audio.channel_layout)};
    body["audio_bitrate_kbps"] = config.audio.bitrate_kbps;

    // SPEC.md §8.6. Reported alongside the container the GUI needs in order to
    // present the option correctly -- §20 row 16 asks for "an inline reason string",
    // not a silent grey-out, and a GUI cannot write that sentence without knowing
    // both halves of it.
    body["multitrack_enabled"] = config.audio.multitrack_enabled;
    body["multitrack_targets"] = config.audio.multitrack_targets;
    body["multitrack_reattach"] = config.audio.multitrack_reattach;
    body["max_tracks"] = config.audio.max_tracks;
    // Whether Tier B can be offered at all on this machine (§8.6: "probe at runtime
    // anyway, and degrade to Tier A with a GUI notice if activation fails"). Reported
    // rather than assumed from the Windows build, for the reason
    // `process_loopback_available` records.
    body["multitrack_available"] = audio::process_loopback_available();

    // CLAUDE.md hard rule 7: reported so a GUI can show the truth, and defaulted false
    // by `config::Config` rather than by anything here.
    body["segmentation_enabled"] = config.segmentation.enabled;
    body["segment_minutes"] = config.segmentation.duration_minutes;

    body["capture_backend"] = std::string{config::to_string(config.advanced.capture_backend)};
    body["capture_cursor"] = config.advanced.capture_cursor;
    body["hdr_tonemap"] = config.advanced.hdr_tonemap;
    body["log_level"] = std::string{config::to_string(config.advanced.log_level)};
    body["gpu_override"] = config.advanced.gpu_override;

    body["check_updates"] = config.updates.check_enabled;
    body["schema_version"] = config.schema_version;
    return body;
}

void EngineService::Impl::apply_config_params(config::Config& target, const nlohmann::json& params) {
    // Field by field, absent-keeps-current. SPEC.md §15.1's compatibility rule at the
    // field level: a GUI that omits a key -- because it is older, or because the user
    // only touched one tab -- must not have every other key reset to a default.
    target.general.output_directory = std::filesystem::path{
        field_or<std::string>(params, "output_directory", target.general.output_directory.string())};
    target.general.filename_template =
        field_or<std::string>(params, "filename_template", target.general.filename_template);
    target.general.language = field_or<std::string>(params, "language", target.general.language);

    target.video.fps = field_or<int>(params, "fps", target.video.fps);
    target.video.cqp = field_or<int>(params, "cqp", target.video.cqp);
    target.video.bitrate_kbps = field_or<int>(params, "bitrate_kbps", target.video.bitrate_kbps);

    target.audio.device_id = field_or<std::string>(params, "audio_device", target.audio.device_id);
    target.audio.bitrate_kbps = field_or<int>(params, "audio_bitrate_kbps", target.audio.bitrate_kbps);

    target.segmentation.enabled = field_or<bool>(params, "segmentation_enabled", target.segmentation.enabled);
    target.segmentation.duration_minutes =
        field_or<int>(params, "segment_minutes", target.segmentation.duration_minutes);

    target.advanced.capture_cursor = field_or<bool>(params, "capture_cursor", target.advanced.capture_cursor);
    target.advanced.hdr_tonemap = field_or<bool>(params, "hdr_tonemap", target.advanced.hdr_tonemap);
    target.advanced.gpu_override = field_or<std::string>(params, "gpu_override", target.advanced.gpu_override);
    target.updates.check_enabled = field_or<bool>(params, "check_updates", target.updates.check_enabled);

    // The enum-valued keys go through the schema's own parsers, so an unrecognised
    // spelling keeps the current value rather than silently selecting the first
    // enumerator. `configure` used to hand-roll the container comparison, which meant
    // any typo other than "mp4" quietly meant MKV.
    if (const auto text = field_or<std::string>(params, "container", {}); !text.empty()) {
        if (const auto parsed = config::container_from_string(text); parsed.has_value()) {
            target.video.container = *parsed;
        }
    }
    if (const auto text = field_or<std::string>(params, "encoder", {}); !text.empty()) {
        if (const auto parsed = config::encoder_from_string(text); parsed.has_value()) {
            target.video.encoder = *parsed;
        }
    }
    if (const auto text = field_or<std::string>(params, "channel_layout", {}); !text.empty()) {
        if (const auto parsed = config::channel_layout_from_string(text); parsed.has_value()) {
            target.audio.channel_layout = *parsed;
        }
    }
    if (const auto text = field_or<std::string>(params, "capture_backend", {}); !text.empty()) {
        if (const auto parsed = config::capture_backend_from_string(text); parsed.has_value()) {
            target.advanced.capture_backend = *parsed;
        }
    }
    if (const auto text = field_or<std::string>(params, "log_level", {}); !text.empty()) {
        if (const auto parsed = config::log_level_from_string(text); parsed.has_value()) {
            target.advanced.log_level = *parsed;
        }
    }
}

void EngineService::Impl::publish_state(RecordingState next) {
    if (state == next) {
        return;
    }
    state = next;
    nlohmann::json body = nlohmann::json::object();
    body["state"] = std::string{to_string(next)};
    // SPEC.md §15.1: paused is a `state_changed` value, not an event of its own,
    // "because a paused recording that looks like a running one loses footage silently".
    pipe.send_event(Event::StateChanged, body);
}

nlohmann::json EngineService::Impl::stats_payload() {
    nlohmann::json body = nlohmann::json::object();
    body["state"] = std::string{to_string(state)};

    const std::lock_guard lock(session_mutex);

    // Always, even with nothing recording: a GUI that reconnected mid-session finds the
    // section here rather than having to re-issue `start_preview` and hope.
    body["preview"] = preview_payload();

    // Whichever session is live. A preview-only one has no pipeline counters, so the
    // recording fields below stay absent rather than reading as a recording of zero frames.
    const pipeline::RecordingSession* live = session ? session.get() : preview_session.get();
    if (live == nullptr) {
        return body;
    }
    const pipeline::SessionStats stats = live->stats();
    body["preview_offered"] = stats.preview.offered;
    body["preview_published"] = stats.preview.published;
    body["preview_rate_limited"] = stats.preview.rate_limited;
    body["preview_dropped"] =
        stats.preview.dropped_no_slot + stats.preview.dropped_stale + stats.preview.dropped_readback_busy;
    body["preview_offer_worst_us"] = stats.preview.worst_offer_ns / 1000;
    body["preview_offer_mean_us"] = stats.preview.mean_offer_ns() / 1000;
    if (!session) {
        return body;
    }
    body["frames_captured"] = stats.frames_captured;
    body["frames_encoded"] = stats.pipeline.frames_encoded;
    body["frames_dropped"] = stats.pipeline.frames_queue_dropped;
    body["frames_paced_out"] = stats.pipeline.frames_paced_out;
    body["duplicates"] = stats.pipeline.duplicates_emitted;
    body["bytes_written"] = stats.pipeline.bytes_written;
    body["packets_muxed"] = stats.pipeline.packets_muxed;
    body["timeline_ms"] = stats.pipeline.last_muxed_pts_ns / 1'000'000;
    body["segments"] = stats.segments;
    body["rebuilds"] = stats.rebuilds;

    // SPEC.md §15.1 names this field: "`get_stats` reports `paused_total_ms` alongside
    // the timeline elapsed."
    body["paused_total_ms"] = stats.pipeline.paused_total_ns / 1'000'000;
    body["pauses"] = stats.pipeline.pauses;
    body["frames_excised"] = stats.pipeline.frames_excised;
    body["pause_stragglers"] = stats.pipeline.pause_stragglers;
    body["output"] = current_output.string();

    // SPEC.md §8.6's tracks, absent entirely when Tier B is off -- so a Tier A
    // recording's `stats` payload is byte-for-byte what it was before this milestone.
    if (!stats.app_tracks.tracks.empty()) {
        nlohmann::json tracks = nlohmann::json::array();
        for (const pipeline::AppTrackStats& track : stats.app_tracks.tracks) {
            nlohmann::json entry = nlohmann::json::object();
            entry["index"] = track.index;
            entry["name"] = track.name;
            entry["executable"] = track.executable;
            entry["pid"] = track.pid;
            entry["attached"] = track.attached;
            // §20 row 15's outcome, so a GUI can say "the application closed" rather
            // than leaving a silent track looking broken.
            entry["target_exited"] = track.target_exited;
            entry["timeline_ms"] = static_cast<std::int64_t>(track.audio.timeline_seconds * 1000.0);
            entry["silence_ms"] = static_cast<std::int64_t>(track.audio.silence_seconds * 1000.0);
            entry["buffers"] = track.source_buffers;
            tracks.push_back(std::move(entry));
        }
        body["audio_tracks"] = std::move(tracks);
    }
    return body;
}

nlohmann::json EngineService::Impl::health_payload() {
    nlohmann::json body = nlohmann::json::object();
    const std::lock_guard lock(session_mutex);
    if (!session) {
        body["rung"] = 0;
        return body;
    }

    const pipeline::PipelineHealth health = session->stats().health;
    body["rung"] = static_cast<int>(health.rung);
    body["rung_transitions"] = health.rung_transitions;
    body["target_fps"] = health.target_fps;
    body["pacer_fps"] = health.pacer_fps;
    body["retimes"] = health.retimes;
    body["drop_ratio"] = health.drop_ratio;
    body["disk_write_p99_ms"] = health.disk_write_p99_ns / 1'000'000;
    body["disk_free_mb"] = static_cast<std::uint64_t>(health.disk_free_bytes / (1024ULL * 1024));
    body["stall_episodes"] = health.stall_episodes;
    body["worst_stall_ms"] = health.worst_stall_ns / 1'000'000;
    body["stop_requested"] = health.stop_requested;
    body["hardware_encoder"] = health.hardware_encoder;
    return body;
}

nlohmann::json EngineService::Impl::preview_payload() const {
    nlohmann::json body = nlohmann::json::object();
    body["armed"] = preview_armed;
    if (!preview_ring.open()) {
        return body;
    }
    const preview::PreviewGeometry& geometry = preview_ring.geometry();
    // Everything a reader needs to lay out the section without a second copy of the
    // constants. BUG-043's lesson generalises: the layer that created the thing is the one
    // that gets to describe it.
    body["section"] = preview_ring.name();
    body["width"] = geometry.width;
    body["height"] = geometry.height;
    body["stride"] = static_cast<std::uint64_t>(geometry.stride());
    body["slots"] = geometry.slots;
    body["slot_bytes"] = static_cast<std::uint64_t>(geometry.slot_bytes());
    body["header_bytes"] = static_cast<std::uint64_t>(preview::kPreviewHeaderBytes);
    body["bytes"] = static_cast<std::uint64_t>(geometry.total_bytes());
    body["fps"] = geometry.fps;
    body["format"] = "bgra8";
    return body;
}

void EngineService::Impl::apply_preview_to_locked(pipeline::SessionSettings& session_settings) {
    preview::PreviewRing* const ring = &preview_ring;
    // The pointer is always handed over once the section exists, even when the preview is
    // *disarmed*: §15.1 lets `start_preview` arrive after `start_record`, and a session
    // that was never given the ring could not honour it. `preview_enabled` is what decides
    // whether anything is dispatched, and it defaults to false.
    session_settings.preview = ring;
    if (!preview_armed || !preview_ring.open()) {
        return;
    }
    session_settings.preview = ring;
    session_settings.preview_enabled = true;
    session_settings.preview_settings.fps = preview_ring.geometry().fps;
    session_settings.preview_settings.tone_map_hdr = config.advanced.hdr_tonemap;
    session_settings.preview_settings.injected_publish_stall_ns = settings.preview_publish_stall_ns;
}

void EngineService::Impl::start_preview_session_locked() {
    if (!preview_armed || preview_session || (session && session->running())) {
        return;
    }

    pipeline::SessionSettings session_settings;
    session_settings.preview_only = true;
    session_settings.video = config.video;
    session_settings.backend = config.advanced.capture_backend;
    session_settings.encoder_override = config.advanced.gpu_override;
    session_settings.capture_factory = settings.capture_factory;
    session_settings.target.capture_cursor = config.advanced.capture_cursor;
    apply_preview_to_locked(session_settings);

    // The primary display, because `start_preview` names no monitor: §15.1's command list
    // gives it no parameters at all. A GUI that wants to preview a different source selects
    // it and starts recording; making the preview target configurable would be a §15.1
    // change, and CLAUDE.md §9 says not to guess at those.
    if (const Result<capture::DisplaySource> display = capture::primary_display(); display.has_value()) {
        session_settings.target.monitor = display.value().monitor;
    }

    auto starting = std::make_unique<pipeline::RecordingSession>();
    if (const Result<void> started = starting->start(session_settings); !started.has_value()) {
        FC_LOG_WARN(Subsystem::Ipc, "the preview session could not start; the ring stays armed but empty",
                    LogFields{}.add_error(started.error()));
        return;
    }
    preview_session = std::move(starting);
}

void EngineService::Impl::stop_preview_session_locked() {
    if (!preview_session) {
        return;
    }
    // The report is deliberately discarded: a preview-only session answers "no file was
    // written", which is not an outcome anyone has to act on.
    static_cast<void>(preview_session->stop());
    preview_session.reset();
}

std::string EngineService::Impl::handle_start_preview(const Request& request) {
    const std::lock_guard lock(session_mutex);

    if (!preview_ring.open()) {
        const preview::PreviewGeometry geometry;
        if (const Result<void> created =
                preview_ring.create(preview::preview_section_name(pipe.session_id()), geometry);
            !created.has_value()) {
            // ERROR_CODES.md, 7008/7009: "Preview is optional -- recording must continue
            // without it." So this fails the *command* and changes nothing else.
            return make_error_response(request.id, created.error());
        }
    }
    preview_armed = true;

    // Idempotent, like §7.5's pause: asking for a preview that is already running succeeds
    // and returns the same section. A GUI that reconnects must not have to know whether it
    // is the first to ask.
    if (session && session->running()) {
        session->set_preview_enabled(true);
    } else {
        start_preview_session_locked();
    }

    const nlohmann::json result = preview_payload();
    return make_response(request.id, result);
}

std::string EngineService::Impl::handle_stop_preview(const Request& request) {
    const std::lock_guard lock(session_mutex);
    preview_armed = false;
    stop_preview_session_locked();

    if (session && session->running()) {
        // The recording keeps going and stops dispatching. The section is deliberately
        // *not* closed here: a live `PreviewWriter` may be mid-publish on `fc-preview`, and
        // unmapping the pages underneath it would turn a stopped preview into a crashed
        // engine. It is closed at `stop_record` instead, and until then a disarmed preview
        // costs one atomic load per captured frame.
        session->set_preview_enabled(false);
    } else {
        preview_ring.close();
    }

    nlohmann::json result = nlohmann::json::object();
    result["armed"] = false;
    return make_response(request.id, result);
}

std::string EngineService::Impl::handle_start_record(const Request& request) {
    const std::lock_guard lock(session_mutex);
    if (session && session->running()) {
        return make_error_response(request.id, FcError::INTERNAL_INVALID_STATE, "a recording is already in progress");
    }

    pipeline::SessionSettings session_settings;
    session_settings.video = config.video;
    // SPEC.md §11, opt-in. CLAUDE.md hard rule 7 lives in `config::SegmentationSettings`'s
    // own default rather than here, so a path that forgets this line still does not split.
    session_settings.segmentation = config.segmentation;
    session_settings.backend = config.advanced.capture_backend;
    session_settings.encoder_override = config.advanced.gpu_override;
    session_settings.capture_factory = settings.capture_factory;
    apply_preview_to_locked(session_settings);

    const auto output = field_or<std::string>(request.params, "output", {});
    if (output.empty()) {
        return make_error_response(request.id, FcError::IO_PATH_INVALID, "start_record requires an output path");
    }
    // The extension names the container that will actually be written (BUG-043).
    //
    // The engine holds the container *setting* and the caller supplies the *path*, so
    // the two could disagree -- and did: a GUI hard-coding `.mkv` while the configuration
    // said `mp4` produced a real MP4 file called `.mkv`, which the log recorded plainly
    // as `container opened path="...mkv" format=mp4` and nothing treated as a problem.
    // The recording was correct and unopenable in anything that trusts extensions.
    //
    // **The path wins, not the setting**, and that direction is deliberate. A request
    // that names its own container is self-describing: it means the same thing whatever
    // this engine's `config.toml` happens to say, which is what makes a recording
    // reproducible across machines and a test independent of the developer running it.
    // Resolving the other way was tried first and made every existing test's outcome
    // depend on the saved container -- three pytest cases and two GPU cases started
    // failing on a machine whose config said `mp4`, which is the same class of defect
    // arriving from the other side.
    //
    // The setting still decides when the path says nothing, and the GUI derives the name
    // from the setting, so a user's choice reaches the file exactly as before.
    session_settings.output = std::filesystem::path{output};
    if (const auto named = config::container_from_extension(session_settings.output.extension().string());
        named.has_value()) {
        if (*named != session_settings.video.container) {
            FC_LOG_INFO(Subsystem::Ipc, "the output path names a different container than the configured one",
                        LogFields{}
                            .add("path", session_settings.output.filename().string())
                            .add("configured", std::string{config::to_string(session_settings.video.container)})
                            .add("writing", std::string{config::to_string(*named)}));
        }
        session_settings.video.container = *named;
    } else {
        // No usable extension: the setting decides, and the name is made to match it so
        // the file still says what it is.
        const std::filesystem::path requested = session_settings.output;
        session_settings.output.replace_extension(config::extension_for(session_settings.video.container));
        FC_LOG_WARN(Subsystem::Ipc, "the output path named no known container; using the configured one",
                    LogFields{}
                        .add("requested", requested.filename().string())
                        .add("container", std::string{config::to_string(session_settings.video.container)})
                        .add("writing", session_settings.output.filename().string()));
    }

    // The target: an explicit monitor id, or the primary display. Resolved here rather
    // than carried in config, because a display that existed when the config was
    // written may not exist now -- and `stable_id` is exactly the identity that
    // survives that (see `source_resolver.h`).
    const auto monitor_id = field_or<std::string>(request.params, "monitor", {});
    const Result<capture::DisplaySource> display =
        monitor_id.empty() ? capture::primary_display() : capture::resolve_display(monitor_id);
    if (!display.has_value()) {
        return make_error_response(request.id, display.error());
    }
    session_settings.target.monitor = display.value().monitor;
    session_settings.target.capture_cursor = config.advanced.capture_cursor;

    const bool want_audio = field_or<bool>(request.params, "audio", true);
    if (want_audio) {
        session_settings.audio.source = pipeline::AudioSource::SystemLoopback;
        session_settings.audio.device_id = config.audio.device_id;
        session_settings.audio.channel_layout = config.audio.channel_layout;
        session_settings.audio.bitrate_kbps = config.audio.bitrate_kbps;

        // SPEC.md §8.6's Tier B. Opt-in twice over: the setting has to be on *and*
        // name at least one target, because a multi-track recording with no
        // applications listed is a Tier A recording with an extra checkbox ticked.
        const std::vector<std::string> targets = config::parse_multitrack_targets(config.audio.multitrack_targets);
        if (config.audio.multitrack_enabled && !targets.empty()) {
            // §20 row 16, at the layer that knows which container is actually about to
            // be opened. The setting was already checked at `configure`, and the path's
            // extension can still disagree with it (BUG-043) -- so this is the check
            // that matters for a `start_record` naming a `.mp4`.
            if (session_settings.video.container != config::Container::Mkv) {
                FC_LOG_WARN(Subsystem::Audio, "multi-track audio is configured but this recording writes MP4",
                            LogFields{}
                                .add("path", session_settings.output.filename().string())
                                .add_error(FcError::MULTITRACK_REQUIRES_MKV));
                return make_error_response(request.id, FcError::MULTITRACK_REQUIRES_MKV,
                                           "multi-track audio requires the Matroska container (SPEC.md §8.6)");
            }
            // §8.6: "probe at runtime anyway, and degrade to Tier A with a GUI notice
            // if activation fails." Degrade, not refuse -- a machine that cannot
            // intercept per-application audio can still record the system mix, and
            // ending the recording over it would trade a complete file for none.
            if (!audio::process_loopback_available()) {
                FC_LOG_WARN(Subsystem::Audio, "process loopback is unavailable; recording Tier A only",
                            LogFields{}.add_error(FcError::PROCESS_LOOPBACK_UNSUPPORTED));
                pipe.send_event(
                    Event::Warning,
                    nlohmann::json{{"error", std::string{error_name(FcError::PROCESS_LOOPBACK_UNSUPPORTED)}},
                                   {"code", error_code(FcError::PROCESS_LOOPBACK_UNSUPPORTED)},
                                   {"message", "Per-application audio is unavailable on this system; "
                                               "recording the system mix only."}});
            } else {
                // §8.6's limit counts track 0, so the per-application budget is one
                // fewer. `max_tracks` is the user's own further limit and the schema
                // already clamps it to 1..6.
                const auto budget = static_cast<std::size_t>(
                    std::max(0, std::min(config.audio.max_tracks, pipeline::kMaxAudioTracks) - 1));
                session_settings.audio.multitrack.enabled = true;
                session_settings.audio.multitrack.source = pipeline::AppTrackSource::ProcessLoopback;
                session_settings.audio.multitrack.reattach = config.audio.multitrack_reattach;
                for (const std::string& target : targets) {
                    if (session_settings.audio.multitrack.tracks.size() >= budget) {
                        FC_LOG_WARN(Subsystem::Audio, "more multi-track targets are configured than the limit allows",
                                    LogFields{}
                                        .add("requested", static_cast<std::int64_t>(targets.size()))
                                        .add("limit", static_cast<std::int64_t>(budget))
                                        .add("dropped", target)
                                        .add_error(FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED));
                        break;
                    }
                    pipeline::AppTrackConfig track;
                    // §8.6: "Every track gets a human-readable Matroska `Name` tag
                    // (`"System Mix"`, `"chrome.exe"`, `"game.exe"`)." The executable
                    // *is* that name, which is why it is not a second setting to keep
                    // in step with the first.
                    track.name = target;
                    track.executable = target;
                    session_settings.audio.multitrack.tracks.push_back(std::move(track));
                }
            }
        }
    }

    // The preview-only session holds the capture backend, and there is one of those. Torn
    // down *before* the recording opens rather than after it fails to: DDA permits one
    // `IDXGIOutputDuplication` per output per process, so overlapping the two would make a
    // recording fail for a reason no user could act on.
    stop_preview_session_locked();

    session = std::make_unique<pipeline::RecordingSession>();
    const Result<void> started = session->start(session_settings);
    if (!started.has_value()) {
        session.reset();
        publish_state(RecordingState::Faulted);
        // The recording did not start, so the preview goes back to running on its own --
        // a failed `start_record` must not also cost the user their preview.
        start_preview_session_locked();
        return make_error_response(request.id, started.error());
    }

    // The file the session actually opened, which is `<basename>_part001` when SPEC.md
    // §11 segmentation is on. Reporting the requested path instead would name a file that
    // does not exist -- BUG-043, from the other direction.
    current_output = session->current_output();
    publish_state(RecordingState::Recording);

    nlohmann::json result = nlohmann::json::object();
    result["output"] = current_output.string();
    return make_response(request.id, result);
}

std::string EngineService::Impl::handle_stop_record(const Request& request) {
    std::unique_ptr<pipeline::RecordingSession> stopping;
    {
        const std::lock_guard lock(session_mutex);
        if (!session) {
            return make_error_response(request.id, FcError::INTERNAL_INVALID_STATE, "no recording is in progress");
        }
        publish_state(RecordingState::Stopping);
        stopping = std::move(session);
    }

    // Stopped with the mutex released: SPEC.md §15.1 gives `stop_record` 30 s because
    // finalization is legitimately slow, and holding the session lock across it would
    // make `get_stats` block for the same 30 s -- which is precisely when a GUI most
    // wants to say "finalizing".
    const Result<mux::ValidationReport> report = stopping->stop();
    stopping.reset();

    {
        const std::lock_guard lock(session_mutex);
        publish_state(RecordingState::Idle);
        if (preview_armed) {
            // The preview goes back to running on its own, so the surface keeps showing the
            // screen after the user stops recording.
            start_preview_session_locked();
        } else {
            // Safe only now: the recording's `PreviewWriter` was destroyed with the session
            // above, so nothing is holding the section any more. A `stop_preview` that
            // arrived mid-recording deferred this to exactly here.
            preview_ring.close();
        }
    }

    if (!report.has_value()) {
        return make_error_response(request.id, report.error());
    }

    nlohmann::json result = nlohmann::json::object();
    result["output"] = current_output.string();
    result["valid"] = report.value().valid;
    result["duration_s"] = report.value().duration_seconds;
    result["decoded_frames"] = report.value().decoded_frames;
    result["format"] = report.value().format_name;

    pipe.send_event(Event::RecordingFinalized, result);
    return make_response(request.id, result);
}

std::string EngineService::Impl::handle(const Request& request) {
    // Every inbound message is the host's heartbeat (see `GuiWatchdog`). Recorded
    // before dispatch, so a slow command does not make the host look dead.
    watchdog.notify();

    // Recording it before dispatch is necessary and was not sufficient (BUG-039). This
    // thread reads the *next* request only after this one returns, so a command that
    // outlasts the 5 s timeout starves the watchdog no matter when the timestamp was
    // taken -- and the watchdog then blames the host for the engine's own silence.
    // Measured: a 14.3 s `stop_record` made the engine exit mid-finalize.
    const GuiWatchdog::Suspension busy{watchdog};

    switch (request.command) {
    case Command::Hello: {
        nlohmann::json result = nlohmann::json::object();
        result["proto"] = std::string{kProtocolVersion};
        result["engine"] = std::string{"framecapture-engine"};
        result["capabilities"] = engine_capabilities();
        result["session"] = pipe.session_id();
        return make_response(request.id, result);
    }

    case Command::GetSources: {
        nlohmann::json result = nlohmann::json::object();
        nlohmann::json displays = nlohmann::json::array();
        if (const Result<std::vector<capture::DisplaySource>> found = capture::enumerate_displays();
            found.has_value()) {
            for (const capture::DisplaySource& source : found.value()) {
                nlohmann::json entry = nlohmann::json::object();
                entry["stable_id"] = source.stable_id;
                entry["name"] = source.friendly_name;
                entry["device"] = source.device_name;
                entry["width"] = source.width;
                entry["height"] = source.height;
                entry["primary"] = source.primary;
                displays.push_back(entry);
            }
        }
        result["displays"] = displays;

        nlohmann::json windows = nlohmann::json::array();
        if (const Result<std::vector<capture::WindowSource>> found = capture::enumerate_windows(); found.has_value()) {
            for (const capture::WindowSource& source : found.value()) {
                nlohmann::json entry = nlohmann::json::object();
                entry["hwnd"] = source.hwnd;
                entry["title"] = source.title;
                entry["process"] = source.process_name;
                windows.push_back(entry);
            }
        }
        result["windows"] = windows;
        return make_response(request.id, result);
    }

    case Command::GetDevices: {
        const Result<std::vector<audio::RenderEndpoint>> endpoints = audio::enumerate_render_endpoints();
        if (!endpoints.has_value()) {
            return make_error_response(request.id, endpoints.error());
        }
        nlohmann::json list = nlohmann::json::array();
        for (const audio::RenderEndpoint& endpoint : endpoints.value()) {
            nlohmann::json entry = nlohmann::json::object();
            entry["id"] = endpoint.id;
            entry["name"] = endpoint.name;
            entry["sample_rate"] = endpoint.format.sample_rate;
            entry["channels"] = endpoint.format.channels;
            entry["default"] = endpoint.is_default;
            list.push_back(entry);
        }
        nlohmann::json result = nlohmann::json::object();
        result["render_endpoints"] = list;

        // SPEC.md §8.6's per-application targets, so the settings dialog can offer a
        // picker rather than asking a user to know that Chrome is `chrome.exe`.
        //
        // **An additive field on an existing command, not a new one.** §15.1's command
        // list is a graded requirement and has already been extended once
        // (`get_config`/`save_config`, open question 11); its compatibility rule makes
        // unknown *fields* free — "unknown fields are ignored, never fatal" — so a GUI
        // that does not know about this sees the same response it always did.
        //
        // Best-effort: a snapshot that fails leaves the field absent rather than failing
        // the command, because the endpoint list is what `get_devices` is actually for
        // and a picker is a convenience over a text field that still works.
        if (const Result<std::vector<audio::ProcessEntry>> processes = audio::enumerate_distinct_executables();
            processes.has_value()) {
            nlohmann::json running = nlohmann::json::array();
            for (const audio::ProcessEntry& entry : processes.value()) {
                nlohmann::json item = nlohmann::json::object();
                item["pid"] = entry.pid;
                item["executable"] = entry.executable;
                running.push_back(std::move(item));
            }
            result["processes"] = std::move(running);
        }
        return make_response(request.id, result);
    }

    case Command::GetGpuTopology: {
        if (const Result<void> refreshed = topology.refresh(); !refreshed.has_value()) {
            return make_error_response(request.id, refreshed.error());
        }
        nlohmann::json adapters = nlohmann::json::array();
        for (const gpu::AdapterInfo& adapter : topology.topology().adapters) {
            nlohmann::json entry = nlohmann::json::object();
            entry["luid"] = adapter.id.value;
            entry["description"] = adapter.description;
            entry["owns_output"] = !adapter.outputs.empty();
            adapters.push_back(entry);
        }
        nlohmann::json result = nlohmann::json::object();
        result["adapters"] = adapters;
        return make_response(request.id, result);
    }

    case Command::GetConfig: {
        // The whole of what the GUI can set, so a settings dialog opens showing what is
        // actually in force rather than its own defaults. `config_path` is included
        // because "where did that setting go" is otherwise unanswerable from the GUI.
        nlohmann::json result = config_payload();
        result["config_path"] = config::default_config_path().string();
        return make_response(request.id, result);
    }

    case Command::SaveConfig: {
        // Merge first, exactly as `configure` does -- the two must not diverge, or a
        // setting would apply for the session and persist as something else.
        //
        // Onto a *copy*, because §20 row 16's block can refuse the result: a settings
        // dialog that asked for multi-track on MP4 must be told no with nothing
        // applied, rather than left with half its request in force.
        {
            config::Config candidate = config;
            apply_config_params(candidate, request.params);
            apply_multitrack_params(candidate, request.params);
            if (multitrack_conflicts(candidate)) {
                FC_LOG_WARN(Subsystem::Config, "multi-track audio was requested with MP4 selected; refused",
                            LogFields{}.add_error(FcError::MULTITRACK_REQUIRES_MKV));
                return make_error_response(request.id, FcError::MULTITRACK_REQUIRES_MKV,
                                           "multi-track audio requires the Matroska container (SPEC.md §8.6)");
            }
            config = std::move(candidate);
        }

        // Then persist. `config::save` is §17's implementation: atomic write via
        // `config.toml.tmp` + `MoveFileEx`, and it re-serialises the *retained document*
        // so keys this build does not recognise survive (§17: "Never silently discard
        // unknown keys -- preserve them so downgrading doesn't destroy the user's
        // settings").
        if (const Result<void> saved = config::save(config); !saved.has_value()) {
            FC_LOG_ERROR(Subsystem::Config, "saving the configuration failed",
                         LogFields{}.add("path", config::default_config_path().string()).add_error(saved.error()));
            return make_error_response(request.id, saved.error());
        }

        FC_LOG_INFO(Subsystem::Config, "configuration saved",
                    LogFields{}.add("path", config::default_config_path().string()));

        nlohmann::json result = config_payload();
        result["config_path"] = config::default_config_path().string();
        result["saved"] = true;
        return make_response(request.id, result);
    }

    case Command::Configure: {
        // Merged field by field rather than replaced wholesale: §15.1's compatibility
        // rule means a GUI may send a subset, and replacing would silently reset every
        // key it did not mention.
        //
        // Onto a candidate, for the reason `apply_config_params` records: SPEC.md §20
        // row 16's block refuses the *combination*, so it has to be able to refuse the
        // whole request rather than leave the container applied and the track setting
        // rejected.
        config::Config candidate = config;
        candidate.video.fps = field_or<int>(request.params, "fps", candidate.video.fps);
        candidate.video.cqp = field_or<int>(request.params, "cqp", candidate.video.cqp);
        candidate.video.bitrate_kbps = field_or<int>(request.params, "bitrate_kbps", candidate.video.bitrate_kbps);
        candidate.advanced.capture_cursor =
            field_or<bool>(request.params, "capture_cursor", candidate.advanced.capture_cursor);
        candidate.audio.device_id = field_or<std::string>(request.params, "audio_device", candidate.audio.device_id);
        candidate.advanced.gpu_override =
            field_or<std::string>(request.params, "encoder", candidate.advanced.gpu_override);

        if (const auto container = field_or<std::string>(request.params, "container", {}); !container.empty()) {
            candidate.video.container = container == "mp4" ? config::Container::Mp4 : config::Container::Mkv;
        }
        apply_multitrack_params(candidate, request.params);

        // SPEC.md §8.6, verbatim: "the engine rejects a `configure` command that
        // requests both with `FcError::MULTITRACK_REQUIRES_MKV` (3021)". §20 row 16
        // makes the point of that explicit -- multi-track in MP4 is invisible in most
        // players, so a soft warning is not enough and this refuses rather than
        // degrades.
        if (multitrack_conflicts(candidate)) {
            FC_LOG_WARN(Subsystem::Config, "multi-track audio was requested with MP4 selected; refused",
                        LogFields{}
                            .add("container", std::string{config::to_string(candidate.video.container)})
                            .add_error(FcError::MULTITRACK_REQUIRES_MKV));
            return make_error_response(request.id, FcError::MULTITRACK_REQUIRES_MKV,
                                       "multi-track audio requires the Matroska container (SPEC.md §8.6)");
        }
        config = std::move(candidate);

        nlohmann::json result = nlohmann::json::object();
        result["fps"] = config.video.fps;
        result["cqp"] = config.video.cqp;
        result["container"] = std::string{config::to_string(config.video.container)};
        result["multitrack_enabled"] = config.audio.multitrack_enabled;
        return make_response(request.id, result);
    }

    case Command::StartPreview:
        return handle_start_preview(request);

    case Command::StopPreview:
        return handle_stop_preview(request);

    case Command::StartRecord:
        return handle_start_record(request);

    case Command::StopRecord:
        return handle_stop_record(request);

    case Command::PauseRecord: {
        const std::lock_guard lock(session_mutex);
        if (!session) {
            return make_error_response(request.id, FcError::INTERNAL_INVALID_STATE, "no recording is in progress");
        }
        if (const Result<void> paused = session->pause(); !paused.has_value()) {
            return make_error_response(request.id, paused.error());
        }
        publish_state(RecordingState::Paused);
        nlohmann::json result = nlohmann::json::object();
        result["paused"] = true;
        result["paused_total_ms"] = session->stats().pipeline.paused_total_ns / 1'000'000;
        return make_response(request.id, result);
    }

    case Command::ResumeRecord: {
        const std::lock_guard lock(session_mutex);
        if (!session) {
            return make_error_response(request.id, FcError::INTERNAL_INVALID_STATE, "no recording is in progress");
        }
        if (const Result<void> resumed = session->resume(); !resumed.has_value()) {
            return make_error_response(request.id, resumed.error());
        }
        publish_state(RecordingState::Recording);
        nlohmann::json result = nlohmann::json::object();
        result["paused"] = false;
        result["paused_total_ms"] = session->stats().pipeline.paused_total_ns / 1'000'000;
        return make_response(request.id, result);
    }

    case Command::GetStats:
        return make_response(request.id, stats_payload());

    case Command::GetHealth:
        return make_response(request.id, health_payload());

    case Command::SetLogLevel: {
        const auto level = field_or<std::string>(request.params, "level", "info");
        // One parser, shared with `FC_LOG_LEVEL`, so a level the environment accepts and
        // this command rejects is not a state the two can reach.
        const std::optional<log::Level> parsed = log::level_from_string(level);
        if (!parsed.has_value()) {
            return make_error_response(request.id, FcError::INTERNAL_INVALID_ARGUMENT, "unknown log level");
        }
        log::set_level(*parsed);

        nlohmann::json result = nlohmann::json::object();
        result["level"] = std::string{log::to_string(*parsed)};
        return make_response(request.id, result);
    }

    case Command::Recover: {
        const auto sidecar = field_or<std::string>(request.params, "sidecar", {});
        if (sidecar.empty()) {
            return make_error_response(request.id, FcError::IO_PATH_INVALID, "recover requires a sidecar path");
        }
        const Result<mux::RecoveryOutcome> outcome = mux::recover(std::filesystem::path{sidecar});
        if (!outcome.has_value()) {
            return make_error_response(request.id, outcome.error());
        }
        nlohmann::json result = nlohmann::json::object();
        result["repaired"] = outcome.value().repaired;
        result["valid"] = outcome.value().valid;
        result["output"] = outcome.value().output.string();
        result["detail"] = outcome.value().detail;
        return make_response(request.id, result);
    }

    case Command::Shutdown: {
        // Answered *before* shutting down: the response has to reach the GUI while the
        // pipe is still up, and requesting the stop first would race the teardown.
        nlohmann::json result = nlohmann::json::object();
        result["shutting_down"] = true;
        const std::string response = make_response(request.id, result);
        request_shutdown_internal("command");
        return response;
    }
    }

    return make_error_response(request.id, FcError::IPC_UNKNOWN_COMMAND);
}

void EngineService::Impl::stats_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-stats");
    try {
        while (stats_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kStatsInterval);
            if (!stats_running.load(std::memory_order_acquire) || !pipe.client_connected()) {
                continue;
            }
            pipe.send_event(Event::Stats, stats_payload());
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Ipc, "stats thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

void EngineService::Impl::finalize_for_exit() {
    std::unique_ptr<pipeline::RecordingSession> stopping;
    {
        const std::lock_guard lock(session_mutex);
        // The preview goes first and unconditionally. It has no file to lose, and leaving
        // it capturing while the recording finalizes would keep a device alive that the
        // exit path is about to release.
        preview_armed = false;
        stop_preview_session_locked();
        if (!session) {
            preview_ring.close();
            return;
        }
        stopping = std::move(session);
    }

    // SPEC.md §3.1: "engine **finalizes the current recording cleanly**, then exits. It
    // does **not** discard the file." This is the whole of SPEC.md §20 row 13's second
    // assertion, and it runs on every exit path -- heartbeat loss, `shutdown`, and OS
    // session end -- because a user does not care which one ended their recording.
    const std::int64_t began_ns = timing::qpc_now_ns();
    const Result<mux::ValidationReport> report = stopping->stop();
    const std::int64_t elapsed_ms = (timing::qpc_now_ns() - began_ns) / 1'000'000;

    finalized_on_exit.store(true, std::memory_order_release);
    {
        // After the session is gone, so no writer is still holding the pages.
        const std::lock_guard lock(session_mutex);
        preview_ring.close();
    }
    if (report.has_value()) {
        FC_LOG_INFO(Subsystem::App, "recording finalized on the exit path",
                    LogFields{}
                        .add("path", current_output.string())
                        .add("valid", report.value().valid)
                        .add("duration_s", report.value().duration_seconds)
                        .add("finalize_ms", elapsed_ms));
    } else {
        FC_LOG_ERROR(Subsystem::App, "finalizing on the exit path failed",
                     LogFields{}.add("path", current_output.string()).add_error(report.error()));
    }
}

EngineService::EngineService() : impl_(std::make_unique<Impl>()) {}

EngineService::~EngineService() {
    // `stop` joins threads, finalizes a recording and writes to disk, so it can throw.
    // An exception leaving a destructor terminates the process, which would turn a
    // recoverable finalization problem into a crash and lose the very file the call
    // exists to save -- the same reasoning, and the same shape, as
    // `VideoPipeline::~VideoPipeline`.
    //
    // Caught by type rather than with `catch(...)`, and the handler logs: CLAUDE.md §4
    // bans both the catch-all outside a thread entry and the empty handler.
    try {
        stop();
    } catch (const std::exception& e) {
        FC_LOG_ERROR(Subsystem::Ipc, "engine service destructor could not shut down cleanly",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
}

Result<void> EngineService::start(const EngineServiceSettings& settings) {
    impl_->settings = settings;

    if (settings.enforce_single_instance) {
        FC_TRY(impl_->instance.acquire());
    }

    // A missing job is not fatal -- see `JobMembership`. The result is deliberately
    // discarded rather than propagated: an engine started by hand has no host, and
    // refusing to run without one would make the binary undebuggable.
    static_cast<void>(impl_->job.open_from_environment());

    // A configuration that cannot be read is not fatal: the defaults in
    // `config::Config` are a working recorder, and refusing to start because a TOML
    // file is malformed would leave a user with no way to record *and* no GUI to fix
    // the file from. Logged, not propagated.
    if (const Result<config::LoadOutcome> loaded = config::load(); loaded.has_value()) {
        impl_->config = loaded.value().config;
        for (const config::Warning& warning : loaded.value().warnings) {
            FC_LOG_WARN(Subsystem::Config, "configuration warning",
                        LogFields{}.add("key", warning.key).add("detail", warning.detail));
        }
    } else {
        FC_LOG_WARN(Subsystem::Config, "configuration could not be loaded; using defaults",
                    LogFields{}.add_error(loaded.error()));
    }

    PipeServerSettings pipe_settings;
    pipe_settings.session_id = settings.session_id.empty() ? environment_value(kSessionEnvVar) : settings.session_id;
    FC_TRY(impl_->pipe.start(pipe_settings,
                             [impl = impl_.get()](const Request& request) { return impl->handle(request); }));

    if (settings.enable_shutdown_signals) {
        FC_TRY(impl_->signals.install([impl = impl_.get()] {
            // On an OS-owned thread under a hard deadline: ask, do not finalize here.
            impl->request_shutdown_internal("os_shutdown");
        }));
    }

    if (settings.enable_heartbeat) {
        FC_TRY(impl_->watchdog.start([impl = impl_.get()] { impl->request_shutdown_internal("heartbeat_lost"); }));
    }

    impl_->stats_running.store(true, std::memory_order_release);
    impl_->stats_thread = std::thread([impl = impl_.get()] { impl->stats_loop(); });

    FC_LOG_INFO(Subsystem::Ipc, "engine service started",
                LogFields{}
                    .add("pipe", impl_->pipe.pipe_path())
                    .add("hosted", impl_->job.hosted())
                    .add("heartbeat", settings.enable_heartbeat));
    return ok();
}

void EngineService::run() {
    std::unique_lock lock(impl_->exit_mutex);
    impl_->exit_wake.wait(lock, [this] { return impl_->exit_requested; });
    const std::string reason = impl_->exit_reason;
    lock.unlock();

    FC_LOG_INFO(Subsystem::App, "engine exiting", LogFields{}.add("reason", reason));
    impl_->finalize_for_exit();
}

void EngineService::request_shutdown(std::string_view reason) {
    impl_->request_shutdown_internal(reason);
}

void EngineService::stop() {
    impl_->stats_running.store(false, std::memory_order_release);
    if (impl_->stats_thread.joinable()) {
        impl_->stats_thread.join();
    }

    impl_->watchdog.stop();
    impl_->signals.uninstall();
    impl_->pipe.stop();
    impl_->finalize_for_exit();

    // Last, and deliberately so. Releasing the job handle is what lets
    // `KILL_ON_JOB_CLOSE` reap this process if the host is already gone, so it must not
    // happen while there is still a recording to finalize.
    impl_->job.release();
}

std::string EngineService::session_id() const {
    return impl_->pipe.session_id();
}

std::string EngineService::pipe_path() const {
    return impl_->pipe.pipe_path();
}

std::string EngineService::exit_reason() const {
    const std::lock_guard lock(impl_->exit_mutex);
    return impl_->exit_reason;
}

bool EngineService::finalized_on_exit() const noexcept {
    return impl_->finalized_on_exit.load(std::memory_order_acquire);
}

} // namespace fc::ipc
