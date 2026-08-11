#pragma once

// The engine's end of the control channel (SPEC.md §15.1).
//
//     \\.\pipe\framecapture-{session_guid}, message mode,
//     ACL restricted to the current user SID.
//
// One client at a time, because SPEC.md §3.1 allows exactly one engine per user session
// and therefore exactly one GUI driving it. A second connection is accepted only after
// the first has gone; it is not refused at the pipe, because a GUI that crashed and
// restarted must be able to reconnect without the engine being restarted too.
//
// ---------------------------------------------------------------------------
// Why the ACL is not optional, and what it is actually stopping
// ---------------------------------------------------------------------------
// A named pipe with a null DACL is reachable by every process on the machine, including
// other users' sessions on a shared box. The commands behind it can start a recording,
// choose an output path and read the GPU topology -- so an unrestricted pipe turns a
// screen recorder into a way for any local process to record another user's screen to a
// file of its choosing. §15.1's one clause about the ACL is the whole of the engine's
// access control, which is why it is built explicitly here rather than left to the
// default.
//
// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------
// One `fc-ipc` thread owns the pipe handle and does all reading. Handlers run on it, so
// a handler that blocks stops the channel -- which is why `stop_record`'s finalization,
// the one legitimately slow command, has §15.1's separate 30 s budget rather than a
// thread of its own. `send_event` may be called from any thread and serialises against
// the reader's writes with a mutex; it never blocks on a handler.

#include "core/error/result.h"
#include "core/ipc/protocol.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace fc::ipc {

/// The pipe name for a session id. `\\.\pipe\framecapture-{id}`.
///
/// Exposed because the client needs to compute the same name from the same id, and two
/// independent format strings is how a rename breaks one side only.
[[nodiscard]] std::string pipe_path_for(std::string_view session_id);

/// A fresh session id, as SPEC.md §15.1's `{session_guid}`.
[[nodiscard]] std::string new_session_id();

struct PipeServerSettings {
    /// Session id the pipe is named for. Empty generates one.
    std::string session_id;
};

/// Handles one request and returns the JSON body to send back.
///
/// Returning a string rather than a `Result` deliberately: every outcome, including a
/// refusal, is a response the peer is owed, and `make_error_response` is how a failure
/// becomes one. A handler that could "fail to answer" would leave the GUI waiting out
/// its 5 s timeout for something the engine already knew.
using RequestHandler = std::function<std::string(const Request&)>;

class PipeServer {
public:
    PipeServer();
    ~PipeServer();

    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    PipeServer(PipeServer&&) = delete;
    PipeServer& operator=(PipeServer&&) = delete;

    /// Creates the pipe and starts the `fc-ipc` thread.
    ///
    /// The pipe exists the moment this returns successfully, so a GUI that spawned the
    /// engine can connect without polling for the name to appear -- and, more to the
    /// point, without a sleep that is right on one machine and wrong on another.
    [[nodiscard]] Result<void> start(const PipeServerSettings& settings, RequestHandler handler);

    /// Disconnects the client, closes the pipe and joins the thread. Idempotent.
    void stop();

    /// Sends an unsolicited event (SPEC.md §15.1). Safe from any thread.
    ///
    /// Silently does nothing when no client is connected. That is the honest behaviour:
    /// events are state notifications, not a queue, and buffering them for a GUI that
    /// may never return is an unbounded queue by another name (CLAUDE.md hard rule 5).
    /// A reconnecting GUI asks `get_stats` and `get_health`, which is why those exist.
    void send_event(Event event, const nlohmann::json& body);

    [[nodiscard]] bool running() const noexcept;

    /// True while a peer is connected.
    [[nodiscard]] bool client_connected() const noexcept;

    /// The pipe's full path, for logging and for handing to a spawned GUI.
    [[nodiscard]] std::string pipe_path() const;

    [[nodiscard]] std::string session_id() const;

    /// Requests handled since `start`, and frames rejected as malformed or oversized.
    /// Both are asserted by the transport tests; the second is the one that says a peer
    /// is speaking the wrong dialect rather than merely being quiet.
    [[nodiscard]] std::uint64_t requests_handled() const noexcept;
    [[nodiscard]] std::uint64_t frames_rejected() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::ipc
