#pragma once

// The GUI's end of the control channel (SPEC.md §15.1).
//
// This lives in `fc_core` rather than only in Python, and that is a deliberate choice
// worth stating: the shipped GUI is PySide6 and will speak this protocol over its own
// implementation, but SPEC.md §20.1 makes the IPC contract a test target and CLAUDE.md
// §5 puts every testable line in `fc_core`. A C++ client is what lets the transport,
// the handshake, the timeouts and SPEC.md §20 row 13's orphan case be exercised without
// a Python interpreter in the loop -- and it is what the M8a headless driver is built
// from, so the thing under test is the real protocol and not a mock of it.
//
// It is not dead weight when the Python GUI arrives. The two implementations are a
// cross-check: a change that breaks one and not the other is a change that broke the
// protocol rather than the code.

#include "core/error/result.h"
#include "core/ipc/protocol.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace fc::ipc {

/// Called for every unsolicited event (SPEC.md §15.1). Runs on the client's reader
/// thread; must not block, and must not call back into `request`.
using EventHandler = std::function<void(Event, const nlohmann::json&)>;

struct PipeClientSettings {
    /// The engine's session id. The pipe path is derived with `pipe_path_for`, never
    /// spelled out again.
    std::string session_id;

    /// How long `connect` waits for the pipe to appear.
    ///
    /// Non-zero because a GUI that spawns the engine races its `CreateNamedPipe`. The
    /// alternative -- a fixed sleep before connecting -- is right on one machine and
    /// wrong on the next, and is how a start-up race becomes a bug report about a
    /// "slow" laptop.
    std::chrono::milliseconds connect_timeout{5000};
};

class PipeClient {
public:
    PipeClient();
    ~PipeClient();

    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    PipeClient(PipeClient&&) = delete;
    PipeClient& operator=(PipeClient&&) = delete;

    /// Connects and starts the reader thread. Does **not** send `hello` -- that is
    /// `handshake`, so a caller can observe a connection that has not yet agreed on a
    /// protocol version.
    [[nodiscard]] Result<void> connect(const PipeClientSettings& settings, EventHandler on_event = {});

    /// SPEC.md §15.1's handshake. Sends `hello` and checks the engine's major version
    /// against this build's.
    ///
    /// Fails with `IPC_PROTOCOL_VERSION_MISMATCH` on a major mismatch and *not* on a
    /// minor one -- §15.1's compatibility rule makes minor versions additive, so
    /// refusing `1.7` from a `1.0` client would break the rule it is meant to implement.
    [[nodiscard]] Result<nlohmann::json> handshake(std::string_view client_name = "fc_core/1.0.0");

    /// Sends one command and waits for the response carrying the same `id`.
    ///
    /// The timeout is §15.1's: 5 s, or 30 s for `stop_record`. A timeout returns
    /// `INTERNAL_TIMEOUT` and leaves the connection usable, because a slow answer is
    /// not a broken pipe and tearing the channel down would turn one late response into
    /// a lost session.
    [[nodiscard]] Result<nlohmann::json> request(Command command, const nlohmann::json& params = {});

    void disconnect();

    [[nodiscard]] bool connected() const noexcept;

    /// Events received since `connect`. The transport tests assert on this; a GUI would
    /// not need it.
    [[nodiscard]] std::uint64_t events_received() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::ipc
