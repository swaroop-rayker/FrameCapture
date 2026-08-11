"""The GUI's end of the control channel (SPEC.md §15.1).

Threading, because it is the part that is easy to get wrong:

* one reader thread owns the pipe's read side and does nothing else;
* ``request`` is called from the GUI thread and blocks on an ``Event`` until the reader
  matches a response by ``id``;
* unsolicited events are handed to a callback **on the reader thread**, so whatever the
  GUI does with them has to marshal to the GUI thread itself. ``EngineController`` does
  that with a Qt signal; nothing here knows about Qt.

Shutdown deliberately does **not** close the pipe out from under a blocked read. Closing
a handle another thread is reading is undefined on Windows -- the handle can be recycled
between the close and the read returning. Instead the *peer* is asked to go away
(``shutdown``, or the process exits and the job reaps it), the read then returns zero
bytes, and the reader exits on its own. The thread is a daemon so a peer that never
answers cannot keep the GUI alive.
"""

from __future__ import annotations

import json
import logging
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from .framing import FrameReader, FramingError, encode_message
from .protocol import PROTOCOL_MAJOR, PROTOCOL_VERSION, Command, Event, pipe_path_for, timeout_for

_log = logging.getLogger(__name__)

#: Called with (event, body) on the reader thread.
EventCallback = Callable[[Event, dict[str, Any]], None]


class IpcError(Exception):
    """Transport or protocol failure."""


class EngineError(IpcError):
    """The engine answered, and the answer was a refusal.

    Carries the numeric code, which SPEC.md §19 makes the stable external contract --
    matching on ``name`` or on the prose is what breaks when the prose improves.
    """

    def __init__(self, code: int, name: str, message: str, detail: str = "") -> None:
        super().__init__(f"{name} ({code}): {message}" + (f" -- {detail}" if detail else ""))
        self.code = code
        self.name = name
        self.detail = detail


class IpcClient:
    """A connected control channel. Not reusable after ``close``."""

    def __init__(self, session_id: str, on_event: EventCallback | None = None) -> None:
        self._session_id = session_id
        self._on_event = on_event
        self._pipe: Any = None
        self._reader: threading.Thread | None = None
        self._running = threading.Event()

        self._next_id = 0
        self._id_lock = threading.Lock()
        self._write_lock = threading.Lock()

        self._pending: dict[str, dict[str, Any]] = {}
        self._pending_lock = threading.Lock()
        self._answered = threading.Condition(self._pending_lock)

        self.events_received = 0

    # -- connection ---------------------------------------------------------

    def connect(self, timeout_s: float = 15.0) -> None:
        """Open the pipe and start the reader.

        Retries until the deadline: a GUI that spawned the engine races its
        ``CreateNamedPipe``, and the alternative -- a fixed sleep -- is right on one
        machine and wrong on the next.
        """
        path = pipe_path_for(self._session_id)
        deadline = time.monotonic() + timeout_s
        last: OSError | None = None

        while True:
            try:
                # Unbuffered: buffering a request means the engine sees it whenever the
                # buffer happens to flush, which turns a 5 s timeout into a coin toss.
                self._pipe = Path(path).open("r+b", buffering=0)  # noqa: SIM115 -- closed in close()
                break
            except OSError as error:
                last = error
                if time.monotonic() >= deadline:
                    raise IpcError(f"could not connect to {path} within {timeout_s}s: {error}") from last
                time.sleep(0.02)

        self._running.set()
        self._reader = threading.Thread(target=self._read_loop, name="fc-ipc-client", daemon=True)
        self._reader.start()

    def close(self) -> None:
        """Stop reading and release the pipe.

        The reader is joined *before* the handle is closed -- see the module note. A
        reader that will not exit within the grace period is left to the daemon-thread
        reaping at process exit rather than having its handle pulled.
        """
        if not self._running.is_set():
            return
        self._running.clear()

        if self._reader is not None and self._reader.is_alive():
            self._reader.join(timeout=2.0)
            if self._reader.is_alive():
                _log.warning("ipc reader did not exit within 2s; leaving it to process exit")
                # Deliberately return without closing: see the module note.
                return

        with self._write_lock:
            if self._pipe is not None:
                try:
                    self._pipe.close()
                except OSError:
                    _log.debug("closing the control pipe failed", exc_info=True)
                self._pipe = None

        # Anything still waiting is waiting for an answer that is not coming.
        with self._answered:
            self._answered.notify_all()

    @property
    def connected(self) -> bool:
        return self._running.is_set()

    # -- requests -----------------------------------------------------------

    def request(self, command: Command, params: dict[str, Any] | None = None) -> dict[str, Any]:
        """Send one command and wait for the response carrying the same ``id``.

        Raises ``EngineError`` when the engine refused, ``IpcError`` on transport failure
        or timeout. A timeout leaves the connection usable: a slow answer is not a broken
        pipe, and tearing the channel down would turn one late response into a lost
        session.
        """
        if not self._running.is_set():
            raise IpcError("not connected")

        with self._id_lock:
            self._next_id += 1
            request_id = str(self._next_id)

        message: dict[str, Any] = dict(params or {})
        message["cmd"] = command.value
        message["id"] = request_id

        frame = encode_message(message)
        with self._write_lock:
            if self._pipe is None:
                raise IpcError("not connected")
            try:
                self._pipe.write(frame)
            except OSError as error:
                raise IpcError(f"writing {command.value} failed: {error}") from error

        deadline = time.monotonic() + timeout_for(command)
        with self._answered:
            while request_id not in self._pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not self._running.is_set():
                    break
                self._answered.wait(remaining)
            answer = self._pending.pop(request_id, None)

        if answer is None:
            if not self._running.is_set():
                raise IpcError(f"the engine went away while waiting for {command.value}")
            raise IpcError(f"{command.value} timed out after {timeout_for(command)}s")

        if answer.get("ok") is False:
            raise EngineError(
                code=int(answer.get("code", 0)),
                name=str(answer.get("error", "UNKNOWN")),
                message=str(answer.get("message", "")),
                detail=str(answer.get("detail", "")),
            )
        return answer

    def handshake(self, client_name: str = "gui/1.0.0") -> dict[str, Any]:
        """SPEC.md §15.1's handshake.

        Checks the **major** only. §15.1 makes minor versions additive, so refusing a
        ``1.7`` engine from a ``1.0`` client would break the rule the check implements.
        """
        reply = self.request(Command.HELLO, {"proto": PROTOCOL_VERSION, "client": client_name})

        proto = str(reply.get("proto", ""))
        major_text = proto.split(".", 1)[0]
        if not major_text.isdigit() or int(major_text) != PROTOCOL_MAJOR:
            raise IpcError(f"engine speaks protocol {proto!r}; this build speaks {PROTOCOL_VERSION}")
        return reply

    # -- reader -------------------------------------------------------------

    def _read_loop(self) -> None:
        reader = FrameReader()
        try:
            while self._running.is_set():
                pipe = self._pipe
                if pipe is None:
                    break
                chunk = pipe.read(4096)
                if not chunk:
                    break  # the peer closed; the documented way this thread ends

                reader.feed(chunk)
                while True:
                    body = reader.next_frame()
                    if body is None:
                        break
                    self._dispatch(body)
        except FramingError:
            # Unrecoverable on a stream: the reader cannot know where the next message
            # starts. Drop the connection rather than keep decoding rubbish.
            _log.exception("control frame rejected; dropping the connection")
        except (OSError, ValueError):
            if self._running.is_set():
                _log.debug("control pipe read ended", exc_info=True)
        finally:
            self._running.clear()
            with self._answered:
                self._answered.notify_all()

    def _dispatch(self, body: str) -> None:
        try:
            message = json.loads(body)
        except json.JSONDecodeError:
            _log.warning("ignoring an unparseable control message")
            return
        if not isinstance(message, dict):
            return

        if "event" in message:
            self.events_received += 1
            event = Event.from_wire(str(message["event"]))
            # `None` is an event a newer engine emits and this build cannot name.
            # SPEC.md §15.1's compatibility rule says that must not be fatal.
            if event is not None and self._on_event is not None:
                try:
                    self._on_event(event, message)
                except Exception:
                    _log.exception("an event handler raised; the control channel continues")
            return

        request_id = message.get("id")
        if isinstance(request_id, str):
            with self._answered:
                self._pending[request_id] = message
                self._answered.notify_all()
