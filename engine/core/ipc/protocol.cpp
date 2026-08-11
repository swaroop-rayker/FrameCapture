#include "core/ipc/protocol.h"

#include <array>

namespace fc::ipc {
namespace {

struct CommandName {
    Command command;
    std::string_view name;
};

// One table, both directions. Two independent switch statements drift, and the
// direction that drifts is always the one with fewer tests.
constexpr std::array<CommandName, 18> kCommandNames{{
    {Command::Hello, "hello"},
    {Command::GetSources, "get_sources"},
    {Command::GetDevices, "get_devices"},
    {Command::GetGpuTopology, "get_gpu_topology"},
    {Command::Configure, "configure"},
    {Command::StartPreview, "start_preview"},
    {Command::StopPreview, "stop_preview"},
    {Command::StartRecord, "start_record"},
    {Command::StopRecord, "stop_record"},
    {Command::PauseRecord, "pause_record"},
    {Command::ResumeRecord, "resume_record"},
    {Command::GetStats, "get_stats"},
    {Command::GetHealth, "get_health"},
    {Command::SetLogLevel, "set_log_level"},
    {Command::Recover, "recover"},
    {Command::Shutdown, "shutdown"},
    {Command::GetConfig, "get_config"},
    {Command::SaveConfig, "save_config"},
}};

} // namespace

std::string_view to_string(Command command) noexcept {
    for (const CommandName& entry : kCommandNames) {
        if (entry.command == command) {
            return entry.name;
        }
    }
    return "unknown";
}

Result<Command> command_from_string(std::string_view name) noexcept {
    for (const CommandName& entry : kCommandNames) {
        if (entry.name == name) {
            return entry.command;
        }
    }
    return FcError::IPC_UNKNOWN_COMMAND;
}

std::string_view to_string(Event event) noexcept {
    switch (event) {
    case Event::StateChanged:
        return "state_changed";
    case Event::Stats:
        return "stats";
    case Event::Warning:
        return "warning";
    case Event::Error:
        return "error";
    case Event::GpuMigrated:
        return "gpu_migrated";
    case Event::AudioDeviceMigrated:
        return "audio_device_migrated";
    case Event::DegradationChanged:
        return "degradation_changed";
    case Event::SegmentRolled:
        return "segment_rolled";
    case Event::RecordingFinalized:
        return "recording_finalized";
    }
    return "unknown";
}

std::string_view to_string(RecordingState state) noexcept {
    switch (state) {
    case RecordingState::Idle:
        return "idle";
    case RecordingState::Starting:
        return "starting";
    case RecordingState::Recording:
        return "recording";
    case RecordingState::Paused:
        return "paused";
    case RecordingState::Stopping:
        return "stopping";
    case RecordingState::Faulted:
        return "faulted";
    }
    return "unknown";
}

Result<Request> parse_request(std::string_view body) {
    // Non-throwing parse. libnlohmann's default is to throw, and an exception here
    // would cross the pipe-reader thread's boundary on nothing worse than a peer
    // sending junk -- which SPEC.md §19 classifies as recoverable.
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return FcError::IPC_MESSAGE_MALFORMED;
    }

    const auto cmd = parsed.find("cmd");
    if (cmd == parsed.end() || !cmd->is_string()) {
        return FcError::IPC_MESSAGE_MALFORMED;
    }

    Request request;
    FC_TRY_ASSIGN(request.command, command_from_string(cmd->get<std::string>()));

    // A missing or non-string `id` is echoed as empty rather than refused. §15.1 makes
    // the id the peer's correlation token, and rejecting the message would leave it
    // with nothing to correlate the rejection to either.
    if (const auto id = parsed.find("id"); id != parsed.end() && id->is_string()) {
        request.id = id->get<std::string>();
    }

    // Everything else, kept whole. Nothing enumerates it, which is what makes §15.1's
    // "unknown fields are ignored, never fatal" true rather than aspirational.
    request.params = parsed;
    return request;
}

std::string make_response(std::string_view id, const nlohmann::json& result) {
    nlohmann::json message = result.is_object() ? result : nlohmann::json::object();
    message["id"] = std::string{id};
    message["ok"] = true;
    return message.dump();
}

std::string make_error_response(std::string_view id, FcError error, std::string_view detail) {
    nlohmann::json message = nlohmann::json::object();
    message["id"] = std::string{id};
    message["ok"] = false;
    message["error"] = std::string{error_name(error)};
    message["code"] = error_code(error);
    message["message"] = std::string{error_message(error)};
    if (!detail.empty()) {
        message["detail"] = std::string{detail};
    }
    return message.dump();
}

std::string make_event(Event event, const nlohmann::json& body) {
    nlohmann::json message = body.is_object() ? body : nlohmann::json::object();
    message["event"] = std::string{to_string(event)};
    return message.dump();
}

std::vector<std::string> engine_capabilities() {
    // Additive, and honest: a flag here is a promise that the feature works, not that
    // the code for it exists.
    //
    // `multitrack` says the engine implements SPEC.md §8.6's Tier B -- the tracks, the
    // muxer's N-stream path, the naming and the MP4 refusal. It deliberately does
    // **not** claim that per-application interception is available on *this machine*:
    // that is a runtime probe (§8.6: "probe at runtime anyway"), it can be false on a
    // system whose audio service refuses the virtual device, and it is reported per
    // answer as `multitrack_available` in `get_config` rather than baked into a
    // handshake the GUI performs once at start-up.
    return {
        "pause_resume", // SPEC.md §7.5
        "heartbeat",    // SPEC.md §3.1's bidirectional 1 s / 5 s
        "segments",     // SPEC.md §11, opt-in and off by default
        "preview",      // SPEC.md §15.2's shared-memory ring
        "multitrack",   // SPEC.md §8.6's Tier B, opt-in and MKV only
        "gpu_migration",  "audio_device_migration", "degradation_ladder",
        "crash_recovery", // SPEC.md §10.4's `recover`
    };
}

std::int64_t request_timeout_ms(Command command) noexcept {
    // SPEC.md §15.1: "Requests time out at 5 s (except `stop_record`, 30 s, since
    // finalization is legitimately slow)." Finalization on MP4 includes the
    // fragmented-to-progressive remux of the whole file (§10.3), which is bounded by
    // disk throughput and not by anything the engine controls.
    return command == Command::StopRecord ? 30'000 : 5'000;
}

} // namespace fc::ipc
