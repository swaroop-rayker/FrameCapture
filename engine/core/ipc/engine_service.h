#pragma once

// The engine's control plane (SPEC.md §15.1, §3.1).
//
// Owns the pipe, the recording session, the heartbeat and the shutdown signals, and
// turns each of §15.1's sixteen commands into a call on something that already exists.
// There is deliberately no *new* recording logic here: a command handler that could
// decide anything about a recording would be a second place recordings are controlled
// from, and the whole point of `RecordingSession` is that there is one.
//
// ---------------------------------------------------------------------------
// The preview's state machine (SPEC.md §15.2), which is the one thing here that is
// not a straight delegation
// ---------------------------------------------------------------------------
// §15.1 makes `start_preview` and `start_record` independent commands, so all four
// combinations of (preview armed, recording running) are reachable and the engine has to
// mean something sensible by each. Capture, however, is owned by `RecordingSession` and
// there is exactly one of it -- two capture sessions on one output is not something DDA
// permits at all (one `IDXGIOutputDuplication` per output per process).
//
// So the preview is **armed** rather than *started*, and whichever session exists carries
// it:
//
//   armed, not recording   a preview-only session runs: capture, the downscale dispatch
//                          and the ring, with no encoder, no muxer and no file.
//   armed, recording       the recording's own session carries the preview off the same
//                          source texture, which is what §15.2 specifies.
//   not armed              no preview stage exists anywhere and nothing is dispatched.
//
// `start_record` therefore stops a preview-only session before starting the recording,
// and `stop_record` starts one again if the preview is still armed. **The ring itself is
// owned here and outlives both**, because it is the GUI's mapping: recreating the section
// per recording would invalidate every `QImage` the GUI holds, twice per recording, for
// no reason the GUI could distinguish from a crash.

#include "core/config/config.h"
#include "core/error/result.h"
#include "core/ipc/lifecycle.h"
#include "core/ipc/pipe_server.h"
#include "core/pipeline/recording_session.h"
#include "core/preview/preview_ring.h"

#include <atomic>
#include <memory>
#include <string>

namespace fc::ipc {

struct EngineServiceSettings {
    /// Session id for the control pipe. Empty reads `FC_ENGINE_SESSION`, and generates
    /// one if that is absent too -- so a hand-started engine still has a pipe, and a
    /// hosted one uses the name its host already knows.
    std::string session_id;

    /// Enforce SPEC.md §3.1's one-engine-per-user-session mutex.
    ///
    /// Off for tests, which run several engines at once by design. Production is on,
    /// and `framecapture-engine` sets it.
    bool enforce_single_instance = true;

    /// Watch for the host going quiet (SPEC.md §3.1's 5 s timeout) and, when it does,
    /// finalize and exit.
    bool enable_heartbeat = true;

    /// Install the console handler and the `WM_QUERYENDSESSION` window (§3.1).
    bool enable_shutdown_signals = true;

    /// Test seam: builds the capture backend. Empty is production's real capture.
    /// Passed straight through to `pipeline::SessionSettings::capture_factory`, and it
    /// exists for the reason stated there -- CLAUDE.md §5 forbids testing against
    /// whatever happens to be on the desktop.
    std::function<Result<std::unique_ptr<capture::IScreenCapture>>(ID3D11Device*)> capture_factory;

    /// SPEC.md §20.1 chaos-tier injection for §15.2's preview, passed through to every
    /// session this service starts. Zero on every production path -- see
    /// `preview::PreviewWriterSettings::injected_publish_stall_ns`.
    std::int64_t preview_publish_stall_ns = 0;
};

/// The engine, as the GUI sees it.
class EngineService {
public:
    EngineService();
    ~EngineService();

    EngineService(const EngineService&) = delete;
    EngineService& operator=(const EngineService&) = delete;
    EngineService(EngineService&&) = delete;
    EngineService& operator=(EngineService&&) = delete;

    /// Acquires the single-instance mutex, joins the host's job, opens the pipe and
    /// arms the watchdogs.
    [[nodiscard]] Result<void> start(const EngineServiceSettings& settings);

    /// Blocks until `shutdown` is commanded, the host's heartbeat is lost, or the OS
    /// asks the process to end.
    ///
    /// **Any of those three finalizes an in-flight recording before returning** --
    /// CLAUDE.md §1, and SPEC.md §3.1's "it does **not** discard the file".
    void run();

    /// Asks `run` to return. Safe from any thread and from an OS callback.
    void request_shutdown(std::string_view reason);

    void stop();

    [[nodiscard]] std::string session_id() const;
    [[nodiscard]] std::string pipe_path() const;

    /// Why `run` returned: "command", "heartbeat_lost", "os_shutdown", or empty while
    /// still running. Row 13's test asserts on this, because "the engine exited" and
    /// "the engine exited *for the right reason*" are different claims.
    [[nodiscard]] std::string exit_reason() const;

    /// True once a recording has been finalized by the shutdown path rather than by an
    /// explicit `stop_record`. Row 13's other assertion.
    [[nodiscard]] bool finalized_on_exit() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::ipc
