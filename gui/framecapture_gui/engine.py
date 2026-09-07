"""The engine, as the rest of the GUI sees it.

One object owning the whole of SPEC.md §3.1's parent role -- job object, spawn,
handshake, heartbeat -- and turning §15.1's channel into Qt signals. Everything above
this is widgets; nothing above this touches a pipe, a thread or a handle.

**Why the signals matter.** ``IpcClient`` invokes its event callback on the reader
thread. Touching a widget from there is undefined in Qt. Emitting a signal from a
``QObject`` is not: a signal crossing a thread boundary is delivered through the
receiving thread's event loop by default, so every slot connected here runs on the GUI
thread. That one property is the reason this class exists rather than the main window
holding an ``IpcClient`` directly.
"""

from __future__ import annotations

import logging
from collections.abc import Callable
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Signal

from .ipc.client import EngineError, IpcClient, IpcError
from .ipc.lifecycle import EngineProcess, Heartbeat, JobObjectError
from .ipc.protocol import Command, Event, RecordingState
from .preview import PreviewChannel, PreviewUnavailableError

_log = logging.getLogger(__name__)


class EngineController(QObject):
    """Owns the engine process and its control channel."""

    #: The recording state changed. Always the authority for what the UI shows --
    #: including ``OFFLINE``, which the engine cannot report about itself.
    state_changed = Signal(RecordingState)

    #: A ``stats`` event (2 Hz) or the answer to ``get_stats``.
    stats_updated = Signal(dict)

    #: Something the user should see. ``(severity, text)`` where severity is
    #: "warning" or "error", matching §15.1's two event names.
    notified = Signal(str, str)

    #: A recording finished and validated. Carries the ``stop_record`` result.
    recording_finalized = Signal(dict)

    #: SPEC.md §10.4's finalization, while it happens (M9.6 §2.1). Carries the
    #: ``finalize_progress`` event body: phase, percent, bytes.
    #:
    #: Reaches the GUI thread only because `stop_recording` no longer blocks it. That is
    #: the whole point of the async command path -- a queued signal is delivered through
    #: the receiving thread's event loop, and an event loop stuck inside a 30-second
    #: request delivers nothing until it is over.
    finalize_progress = Signal(dict)

    #: One `recover` attempt finished (SPEC.md §10.4, M9.6 Phase 4). Carries the
    #: engine's outcome plus the `sidecar` it was asked about, and is emitted whether
    #: the attempt succeeded or failed -- a caller working through several unfinished
    #: recordings has to hear back about every one of them, or it stops on the first
    #: that could not be repaired.
    recovery_finished = Signal(dict)

    def __init__(self, engine_path: Path, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._engine_path = engine_path
        self._process: EngineProcess | None = None
        self._client: IpcClient | None = None
        self._heartbeat: Heartbeat | None = None
        self._state = RecordingState.OFFLINE
        self._capabilities: list[str] = []
        self._engine_version = ""
        self._protocol = ""
        #: The level the engine is logging at *now*, which is not necessarily the one in
        #: `config.toml`: `set_log_level` changes the running process and persists
        #: nothing. Tracked here so the menu can show what is in force rather than what
        #: was configured.
        self._log_level = ""
        self._last_response: dict[str, Any] = {}
        self._last_output: Path | None = None
        self._preview: PreviewChannel | None = None

    # -- lifecycle ----------------------------------------------------------

    def start(self) -> bool:
        """Spawn the engine and connect. Returns False having reported why.

        Failure is reported through ``notified`` and by staying ``OFFLINE`` rather than
        by raising: SPEC.md §16.1 requires the GUI to be "fully functional and honest
        when the engine is down", which means a failed start is a state the window
        renders, not an exception that closes it.
        """
        if self._client is not None and self._client.connected:
            return True

        try:
            self._process = EngineProcess(self._engine_path)
            self._process.start()
        except (JobObjectError, OSError) as error:
            _log.exception("could not start the engine")
            self._set_state(RecordingState.OFFLINE)
            self.notified.emit("error", f"Could not start the engine: {error}")
            return False

        try:
            self._client = IpcClient(self._process.session_id, on_event=self._on_event)
            self._client.connect()
            reply = self._client.handshake()
            self._capabilities = [str(c) for c in reply.get("capabilities", [])]
            # Absent on an engine older than M9.6, which is exactly the compatibility
            # case §15.1's additive rule exists for -- an empty string renders as
            # "unknown" in the diagnostics summary rather than taking the handshake down.
            self._engine_version = str(reply.get("version", ""))
            self._protocol = str(reply.get("proto", ""))
        except IpcError as error:
            _log.exception("handshake failed")
            self.shutdown()
            self.notified.emit("error", f"The engine did not answer: {error}")
            return False

        self._heartbeat = Heartbeat(self._beat)
        self._heartbeat.start()

        self._set_state(RecordingState.IDLE)
        _log.info("engine %s ready, capabilities=%s", self._process.pid, self._capabilities)
        return True

    def shutdown(self) -> None:
        """Stop the engine and release everything, in the order that keeps the file.

        ``shutdown`` first so an in-flight recording is finalized by the engine's own
        clean path; the job handle closes last, because closing it while the engine is
        still finalizing is what SPEC.md §3.1's whole design exists to avoid.
        """
        if self._heartbeat is not None:
            self._heartbeat.stop()
            self._heartbeat = None

        # Before the engine is told to go: the section is the engine's, and a mapping still
        # open here outlives the process that owns it.
        if self._preview is not None:
            self._preview.detach()
            self._preview = None

        if self._client is not None:
            if self._client.connected:
                try:
                    self._client.request(Command.SHUTDOWN)
                except IpcError:
                    _log.debug("the engine did not acknowledge shutdown", exc_info=True)
            self._client.close()
            self._client = None

        if self._process is not None:
            # Generous next to a finalize: SPEC.md §15.1 budgets 30 s for `stop_record`
            # because an MP4 remux is bounded by the disk, and this is the same work.
            if self._process.wait(timeout_s=30.0) is None:
                _log.warning("the engine did not exit within 30s; the job object will reap it")
            self._process.close()
            self._process = None

        self._set_state(RecordingState.OFFLINE)

    @property
    def running(self) -> bool:
        return self._client is not None and self._client.connected

    @property
    def state(self) -> RecordingState:
        return self._state

    @property
    def capabilities(self) -> list[str]:
        return list(self._capabilities)

    @property
    def engine_version(self) -> str:
        """The engine binary's version, from the handshake. Empty if it did not say.

        Not the GUI's `__version__`: the two are separate artefacts with separate
        version numbers, and a bug report needs the one that did the recording.
        """
        return self._engine_version

    @property
    def protocol(self) -> str:
        """The protocol version the engine announced (§15.1)."""
        return self._protocol

    @property
    def log_level(self) -> str:
        """The level the engine is logging at now, or empty if it has not been asked."""
        return self._log_level

    def supports(self, capability: str) -> bool:
        """Whether the engine advertised ``capability`` in its handshake.

        Asked rather than assumed: §15.1 makes features additive capability flags
        precisely so a GUI can be newer than its engine without guessing.
        """
        return capability in self._capabilities

    # -- commands -----------------------------------------------------------

    def start_recording(self, output: Path, *, audio: bool = True, monitor: str = "") -> bool:
        params: dict[str, Any] = {"output": str(output), "audio": audio}
        if monitor:
            params["monitor"] = monitor
        started = self._command(Command.START_RECORD, params, busy_state=RecordingState.STARTING)
        if started:
            # The engine echoes the path it actually opened, which may differ from the one
            # asked for: it corrects the extension to match the container it is going to
            # write (BUG-043). Recorded so the window reports the real filename.
            echoed = str(self._last_response.get("output", ""))
            self._last_output = Path(echoed) if echoed else output
        return started

    @property
    def last_output(self) -> Path | None:
        """The path the engine opened for the current or most recent recording."""
        return self._last_output

    def stop_recording(self) -> bool:
        """Ask the engine to stop, **without blocking the GUI thread**.

        Returns whether the request was *queued*, not whether the file was written --
        which is the honest return for a call that no longer waits for it. The outcome
        arrives as `recording_finalized`; the progress in between as `finalize_progress`.

        This used to block the GUI thread for up to SPEC.md §15.1's 30 seconds. See
        `IpcClient.request_async` for the three separate symptoms that caused. Nothing
        about the engine's side changed -- what changed is that the event loop stays
        alive to hear the answer, which is what makes a save-progress bar possible at
        all (M9.6 §1.2).
        """
        self._set_state(RecordingState.STOPPING)
        return self._command_async(Command.STOP_RECORD, {}, on_success=self._on_stop_finished)

    def _on_stop_finished(self, result: dict[str, Any]) -> None:
        """The `stop_record` response. Runs on the command thread; only emits."""
        self._set_state(RecordingState.IDLE)
        self.recording_finalized.emit(result)

    def stop_recording_and_wait(self) -> bool:
        """Stop, and **block until the file is on disk**.

        The blocking form still exists because two callers genuinely need it, and both
        are cases where there is nothing left to keep responsive:

        * closing the window mid-recording — the process is about to exit, and returning
          before the file is finalized is the prime directive's one unacceptable
          outcome;
        * restarting the engine — the engine that would finalize the file is the one
          being torn down.

        Everything a user drives from a control uses `stop_recording` instead. Blocking
        is correct here and wrong there, which is why these are two methods rather than
        one with a flag: the choice is about who is waiting, and that is not something a
        caller should be able to get wrong by omitting an argument.
        """
        client = self._connected_client()
        if client is None:
            return False
        self._set_state(RecordingState.STOPPING)
        try:
            result = client.request(Command.STOP_RECORD)
        except EngineError as error:
            self.notified.emit("error", f"Stopping failed: {error}")
            self._refresh_state()
            return False
        except IpcError as error:
            self.notified.emit("error", f"The engine stopped answering: {error}")
            self._set_state(RecordingState.OFFLINE)
            return False

        self._on_stop_finished(result)
        return True

    def pause_recording(self) -> bool:
        """SPEC.md §7.5's pause.

        **Still synchronous, and that is a measured decision rather than an oversight.**
        M9.6's plan proposed routing every user-initiated command through the async path.
        Only `stop_record` has a measured multi-second cost -- BUG-046 puts finalization
        at ~3.3 s of remux plus ~1 s of validate for a 1.2 GB file. `pause_record` sets a
        flag on the pause clock: no I/O, no device work, and it returns in microseconds.
        Its 5 s timeout is a ceiling nothing approaches.

        The worst case that does exist is a pause pressed while the engine's request
        thread is inside a `start_record` -- opening a device and an encoder, which
        SPEC.md §5.4 budgets at 350 ms. That is a sub-second stall, not the seconds-long
        freeze the async path was built for, and it is bounded by a budget the engine
        already has to meet.

        What making it async *would* cost is real: the return value could then only mean
        "queued", not "the engine accepted it", and `test_engine_control` asserts on the
        refusal (pausing with nothing recording). Trading a proven assertion for an
        unmeasured stall is the wrong way round. Revisit if Phase 1 measures otherwise.
        """
        return self._command(Command.PAUSE_RECORD, {}, success_state=RecordingState.PAUSED)

    def resume_recording(self) -> bool:
        return self._command(Command.RESUME_RECORD, {}, success_state=RecordingState.RECORDING)

    def start_preview(self) -> PreviewChannel | None:
        """Arm SPEC.md §15.2's preview and attach to its ring.

        Returns the attached channel, or ``None`` having reported why. Failure is never
        fatal and never noisy: ERROR_CODES.md says of the two shared-memory errors
        "preview is optional -- recording must continue without it", and the window's
        answer to a ``None`` here is the placeholder it already had.

        The capability is checked first rather than the command being tried and refused,
        because §15.1 makes features additive flags precisely so a newer GUI can ask an
        older engine without guessing.
        """
        if not self.supports("preview"):
            _log.info("the engine does not advertise a preview channel")
            return None
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            response = client.request(Command.START_PREVIEW)
        except IpcError as error:
            _log.warning("start_preview failed: %s", error)
            return None

        channel = PreviewChannel()
        try:
            channel.attach(response or {})
        except PreviewUnavailableError as error:
            # The engine says it is previewing and this process cannot see it. Worth a log
            # line and nothing more -- the recording is unaffected either way.
            _log.warning("the preview section could not be attached: %s", error)
            return None
        self._preview = channel
        return channel

    def stop_preview(self) -> None:
        """Detach, then disarm. Idempotent, and safe with no engine.

        **Detach first.** The engine closes the section when the last session lets go of
        it, and a mapping still open here would be a view over pages nobody owns.
        """
        if self._preview is not None:
            self._preview.detach()
            self._preview = None
        client = self._client
        if client is None or not client.connected:
            return
        try:
            client.request(Command.STOP_PREVIEW)
        except IpcError:
            _log.debug("stop_preview was not acknowledged", exc_info=True)

    @property
    def preview(self) -> PreviewChannel | None:
        """The attached preview channel, or ``None``."""
        return self._preview

    def get_stats(self) -> dict[str, Any] | None:
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_STATS)
        except IpcError:
            return None

    def get_sources(self) -> dict[str, Any] | None:
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_SOURCES)
        except IpcError:
            _log.debug("get_sources failed", exc_info=True)
            return None

    def configure(self, settings: dict[str, Any]) -> bool:
        """Apply settings for this session only. Not persisted -- see `save_config`."""
        return self._command(Command.CONFIGURE, settings)

    def get_devices(self) -> dict[str, Any] | None:
        """The engine's audio endpoints, and the processes a Tier B track can follow.

        ``None`` when there is no engine to ask, for the same reason `get_config`
        returns ``None``: a settings dialog with no engine shows its own defaults rather
        than refusing to open.
        """
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_DEVICES)
        except IpcError:
            _log.debug("get_devices failed", exc_info=True)
            return None

    def get_gpu_topology(self) -> dict[str, Any] | None:
        """SPEC.md §5.1's adapters, and which one owns the display output.

        A query, so synchronous: it refreshes the topology through DXGI, which is a
        microsecond-scale enumeration, and the dialog that asked for it cannot draw
        anything until the answer is in.
        """
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_GPU_TOPOLOGY)
        except IpcError:
            _log.debug("get_gpu_topology failed", exc_info=True)
            return None

    def get_health(self) -> dict[str, Any] | None:
        """SPEC.md §13's degradation rung and the counters behind it."""
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_HEALTH)
        except IpcError:
            _log.debug("get_health failed", exc_info=True)
            return None

    def set_log_level(self, level: str) -> bool:
        """SPEC.md §15.1's `set_log_level`, for **this run of the engine only**.

        Deliberately not persisted. `advanced.log_level` in `config.toml` is the setting;
        this is the knob you turn to reproduce a bug in the next thirty seconds, and
        writing it to disk would leave a user recording at TRACE for weeks because they
        once chased a defect. The settings dialog is where the durable choice lives.

        The engine echoes the level it applied, which is what gets recorded here -- so
        the menu shows what is in force rather than what was asked for.
        """
        if not self._command(Command.SET_LOG_LEVEL, {"level": level}):
            return False
        self._log_level = str(self._last_response.get("level", level)).lower()
        return True

    def recover(self, sidecar: Path) -> bool:
        """Finish an unfinished recording (SPEC.md §10.4). Reports on `recovery_finished`.

        **Asynchronous, for the same reason `stop_record` is.** For MP4 this runs the
        identical lossless remux a clean stop performs, which BUG-046 measured at ~3.3 s
        of remux plus ~1 s of validate for a 1.2 GB file. Blocking the GUI thread for
        that long is the freeze M9.6 Rule B exists to prevent, and it would do it while
        a progress-free dialog sat on screen.

        Returns whether the request was *queued*. The outcome -- including a refusal --
        arrives on `recovery_finished`.
        """
        path = str(sidecar)

        def done(result: dict[str, Any]) -> None:
            self.recovery_finished.emit({"sidecar": path, **result})

        def failed(error: Exception) -> None:
            self.recovery_finished.emit(
                {"sidecar": path, "output": "", "repaired": False, "valid": False, "detail": str(error)}
            )

        return self._command_async(Command.RECOVER, {"sidecar": path}, on_success=done, on_failure=failed)

    def get_config(self) -> dict[str, Any] | None:
        """The engine's current configuration, as loaded from `config.toml` (§17).

        ``None`` when there is no engine to ask. The caller then shows its own defaults
        rather than a blank dialog -- which is what the window does, and why this
        returns ``None`` instead of raising.
        """
        client = self._client
        if client is None or not client.connected:
            return None
        try:
            return client.request(Command.GET_CONFIG)
        except IpcError:
            _log.debug("get_config failed", exc_info=True)
            return None

    def save_config(self, settings: dict[str, Any]) -> bool:
        """Apply **and persist** settings (§17).

        The engine writes the file: it owns the atomic replace, the ordered migration
        chain and the preservation of keys this build does not recognise. Doing it from
        here would put a second writer with a second idea of the schema on one file.
        """
        return self._command(Command.SAVE_CONFIG, settings)

    # -- internals ----------------------------------------------------------

    def _command(
        self,
        command: Command,
        params: dict[str, Any],
        *,
        busy_state: RecordingState | None = None,
        success_state: RecordingState | None = None,
    ) -> bool:
        client = self._connected_client()
        if client is None:
            return False

        if busy_state is not None:
            self._set_state(busy_state)
        try:
            self._last_response = client.request(command, params) or {}
        except EngineError as error:
            # The engine refused and is still there. Report it and re-read the state
            # rather than guessing -- a refused `start_record` may mean a recording is
            # already running, and inventing "idle" here would offer the wrong button.
            self.notified.emit("error", str(error))
            self._refresh_state()
            return False
        except IpcError as error:
            self.notified.emit("error", f"The engine stopped answering: {error}")
            self._set_state(RecordingState.OFFLINE)
            return False

        if success_state is not None:
            self._set_state(success_state)
        return True

    def _command_async(
        self,
        command: Command,
        params: dict[str, Any],
        *,
        on_success: Callable[[dict[str, Any]], None] | None = None,
        on_failure: Callable[[Exception], None] | None = None,
    ) -> bool:
        """Issue a user-initiated command without waiting for it.

        The counterpart to `_command`, and the two are split by *who is waiting*, not by
        what the command is. A query the GUI needs the answer to before it can draw
        anything -- `get_stats`, `get_config` -- returns in microseconds and stays
        synchronous, where making it async would add a round trip through the event loop
        and an ordering hazard for nothing. A command the user pressed a button for is
        async, because the one thing it must not do is stop the button from repainting.

        Failure is reported exactly as `_command` reports it, on the same two signals,
        so a caller does not have to care which path a command took.
        """
        client = self._connected_client()
        if client is None:
            return False

        def done(result: dict[str, Any]) -> None:
            self._last_response = result
            if on_success is not None:
                on_success(result)

        def failed(error: Exception) -> None:
            if isinstance(error, EngineError):
                # The engine refused and is still there. Re-read the state rather than
                # guessing -- exactly as `_command` does, and for the same reason: a
                # refused `pause_record` leaves the recording running, and inventing
                # "paused" here would put §16.5's silent-footage-loss on screen.
                #
                # `on_failure` *replaces* the generic report, and only for the refusal
                # branch. A `recover` the engine declines is not an engine error the user
                # needs a toast about; it is this file's result, and the caller working
                # through a list of them is the only thing that can say so usefully. The
                # state refresh still happens either way.
                if on_failure is None:
                    self.notified.emit("error", str(error))
                else:
                    on_failure(error)
                self._refresh_state()
            else:
                self.notified.emit("error", f"The engine stopped answering: {error}")
                self._set_state(RecordingState.OFFLINE)
                if on_failure is not None:
                    on_failure(error)

        if not client.request_async(command, params, on_done=done, on_error=failed):
            self.notified.emit("error", f"The engine is busy; {command.value} was not sent.")
            return False
        return True

    def _connected_client(self) -> IpcClient | None:
        """The live client, or ``None`` having already reported why not.

        Returns the client rather than a bool so the caller has a narrowed, non-optional
        reference -- the alternative is an `assert` for the type checker's benefit, and
        an assertion that carries program logic is exactly what CLAUDE.md §4 bans on the
        C++ side for the same reason.
        """
        client = self._client
        if client is not None and client.connected:
            return client
        self.notified.emit("error", "The engine is not running.")
        self._set_state(RecordingState.OFFLINE)
        return None

    def _refresh_state(self) -> None:
        stats = self.get_stats()
        if stats is None:
            self._set_state(RecordingState.OFFLINE)
            return
        self._set_state(RecordingState.from_wire(str(stats.get("state", "idle"))))

    def _set_state(self, state: RecordingState) -> None:
        if state == self._state:
            return
        self._state = state
        self.state_changed.emit(state)

    def _beat(self) -> None:
        """One heartbeat. Runs on the heartbeat thread."""
        if self._client is None or not self._client.connected:
            return
        self._client.request(Command.GET_STATS)

    def _on_event(self, event: Event, body: dict[str, Any]) -> None:
        """Runs on the **reader thread**. Only emits; never touches a widget."""
        if event is Event.STATE_CHANGED:
            self._set_state(RecordingState.from_wire(str(body.get("state", "idle"))))
        elif event is Event.STATS:
            self.stats_updated.emit(body)
        elif event is Event.WARNING:
            self.notified.emit("warning", str(body.get("message", "")))
        elif event is Event.ERROR:
            self.notified.emit("error", str(body.get("message", "")))
        elif event is Event.RECORDING_FINALIZED:
            self.recording_finalized.emit(body)
        elif event is Event.FINALIZE_PROGRESS:
            self.finalize_progress.emit(body)
        elif event is Event.DEGRADATION_CHANGED:
            self.notified.emit("warning", f"Recording quality reduced (rung {body.get('rung', '?')})")
        # Anything else is an event this build has no use for. Ignored, not fatal.
