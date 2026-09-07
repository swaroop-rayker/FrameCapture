#pragma once

// The control protocol's vocabulary (SPEC.md §15.1).
//
// Commands, events, the handshake and the compatibility rule, expressed as types so
// that a misspelling is a compile error rather than a message the peer silently
// ignores. Free of any transport, for the reason `framing.h` gives.
//
// ---------------------------------------------------------------------------
// SPEC.md §15.1's compatibility rule, and where it is enforced
// ---------------------------------------------------------------------------
// > Unknown fields are ignored, never fatal; new features are additive capability
// > flags; the major proto version bumps only on a breaking change, and the engine
// > must support the previous major for one release cycle.
//
// "Unknown fields are ignored" is a property of the *parser*, and it is easy to get
// backwards: a strict schema check is the natural thing to write and would make every
// future GUI field a fatal error against an older engine. So `parse_request` reads the
// fields it knows and never enumerates the ones it does not, and there is a test that
// sends a request full of invented keys and asserts it succeeds.
//
// The version check is deliberately on the **major** only. `1.7` talking to `1.0` is
// required to work; `2.0` talking to `1.0` is the breaking change the rule is about.

#include "core/error/fc_error.h"
#include "core/error/result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fc::ipc {

/// This engine's protocol version. Major.minor, per §15.1's `"proto":"1.0"`.
inline constexpr std::string_view kProtocolVersion = "1.0";
inline constexpr int kProtocolMajor = 1;

/// SPEC.md §15.1's command set, verbatim and complete.
///
/// An enum rather than string comparison at the dispatch site, so that adding a command
/// to §15.1 without handling it is a `switch` warning under `/W4 /WX` instead of a
/// runtime `IPC_UNKNOWN_COMMAND` nobody notices until a user hits it.
enum class Command {
    Hello,
    GetSources,
    GetDevices,
    GetGpuTopology,
    Configure,
    StartPreview,
    StopPreview,
    StartRecord,
    StopRecord,
    PauseRecord,
    ResumeRecord,
    GetStats,
    GetHealth,
    SetLogLevel,
    Recover,
    Shutdown,

    // ---- added 2026-08-03, beyond SPEC.md §15.1's sixteen -------------------
    //
    // §17 makes configuration "loaded once by the GUI, validated, and pushed to the
    // engine via `configure`", and the engine "stateless with respect to disk config".
    // That leaves nothing able to *persist* a setting: `configure` is in-memory, and the
    // GUI cannot write the file without reimplementing §17's atomic write, ordered
    // migration chain and unknown-key preservation -- which would put two writers with
    // two ideas of the schema on one file, the exact failure §17 exists to prevent.
    //
    // So the engine keeps ownership of `config.toml` and these two expose it. The
    // engine stays stateless *during a recording* -- `configure` is still what a
    // recording reads -- and becomes the only writer of the file, which is the property
    // that actually matters.
    //
    // **This extends a specified command list and needs the owner's pen on §15.1.**
    // Recorded in docs/ACCEPTANCE.md's open questions and in docs/IPC_PROTOCOL.md.
    GetConfig,
    SaveConfig,
};

/// The wire spelling. Total over the enum.
[[nodiscard]] std::string_view to_string(Command command) noexcept;

/// Wire spelling to enum. `IPC_UNKNOWN_COMMAND` for anything not in §15.1.
[[nodiscard]] Result<Command> command_from_string(std::string_view name) noexcept;

/// SPEC.md §15.1's unsolicited engine -> GUI events.
enum class Event {
    StateChanged,
    Stats,
    Warning,
    Error,
    GpuMigrated,
    AudioDeviceMigrated,
    DegradationChanged,
    SegmentRolled,
    RecordingFinalized,

    /// SPEC.md §10.4's finalization, reported while it runs (M9.6 §2.1).
    ///
    /// Additive under §15.1's compatibility rule -- an older GUI does not recognise the
    /// name and ignores it, which is exactly the behaviour that rule requires. Emitted
    /// **while `stop_record` is still in flight**: the request thread is inside
    /// `RecordingSession::stop`, and `PipeServer::send_event` is a write that serialises
    /// independently of the read loop, so the events reach the peer before the response
    /// they precede.
    FinalizeProgress,
};

[[nodiscard]] std::string_view to_string(Event event) noexcept;

/// Recording states, as carried by `state_changed`.
///
/// SPEC.md §15.1: "Paused is a `state_changed` value, not an event of its own -- the GUI
/// must render it as a distinct state (§16.5), because a paused recording that looks
/// like a running one loses footage silently."
enum class RecordingState {
    Idle,
    Starting,
    Recording,
    Paused,
    Stopping,
    Faulted,
};

[[nodiscard]] std::string_view to_string(RecordingState state) noexcept;

/// One parsed command from the peer.
struct Request {
    /// §15.1: "Every command carries an `id`; every response echoes it." Kept as a
    /// string rather than an integer because it is the GUI's correlation token and the
    /// engine has no business constraining its shape.
    std::string id;
    Command command = Command::Hello;
    /// Everything else the message carried. Read by name; never enumerated, so an
    /// unknown field costs nothing (see the header note).
    nlohmann::json params;
};

/// Parses one JSON body into a `Request`.
///
/// Fails with `IPC_MESSAGE_MALFORMED` for anything that is not an object with a string
/// `cmd`, and with `IPC_UNKNOWN_COMMAND` when `cmd` is not one of §15.1's. A missing
/// `id` is **not** a failure -- it is echoed back as empty, because refusing the message
/// would leave the peer with no response to correlate to anything at all.
[[nodiscard]] Result<Request> parse_request(std::string_view body);

/// A successful response, `id` echoed. `result` is merged in at the top level.
[[nodiscard]] std::string make_response(std::string_view id, const nlohmann::json& result);

/// A failed response.
///
/// Carries the numeric `FcError` code and its enumerator spelling, not a prose message:
/// SPEC.md §19 makes the numeric codes "a stable external contract", and a GUI matching
/// on English text is a GUI that breaks when the text improves.
[[nodiscard]] std::string make_error_response(std::string_view id, FcError error, std::string_view detail = {});

/// An unsolicited event. `id` is absent by construction -- an event answers nothing.
[[nodiscard]] std::string make_event(Event event, const nlohmann::json& body);

/// What this engine can do beyond the base protocol, returned by `hello`.
///
/// §15.1 makes new features "additive capability flags", so a GUI must ask rather than
/// infer from the version. The list is the honest one: things absent from it are absent
/// from the engine, including features a later milestone will add.
[[nodiscard]] std::vector<std::string> engine_capabilities();

/// Both sides' timeout budget, per §15.1: 5 s for everything except `stop_record`,
/// which gets 30 s "since finalization is legitimately slow".
[[nodiscard]] std::int64_t request_timeout_ms(Command command) noexcept;

} // namespace fc::ipc
