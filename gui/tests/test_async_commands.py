"""The non-blocking command path (M9.6 Phase 0, §20 row 20).

**What this is a fix for.** `IpcClient.request` blocks its caller until the reader
matches a response, and `EngineController.stop_recording` called it on the GUI thread
with SPEC.md §15.1's 30-second timeout. ACCEPTANCE.md's BUG-046 measured what that
costs on a real file: ~3.3 s of remux plus ~1 s of validate for a 1.2 GB recording. For
those seconds the event loop is dead, and three separately-reported symptoms follow from
that one fact — a save-progress bar that cannot paint, buttons that do not respond, and
``WM_HOTKEY`` sitting unread so the global hotkeys appear broken.

Two halves, and both are needed. The mechanism tests below run anywhere and pin the
queue's contract. The `engine`-marked test at the bottom is the one that actually proves
the exit criterion, because it measures the Qt event loop while a real engine finalizes
a real file — and a mechanism that is correct in isolation but wired up wrong would pass
the first half and fail the second.
"""

from __future__ import annotations

import threading
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QCoreApplication, QTimer

from framecapture_gui.engine import EngineController
from framecapture_gui.ipc.client import IpcClient, IpcError
from framecapture_gui.ipc.protocol import Command, RecordingState

from .screen_activity import screen_activity

# ---------------------------------------------------------------------------
# The queue's contract, without an engine
# ---------------------------------------------------------------------------


class _StubClient(IpcClient):
    """An `IpcClient` whose `request` is slow and never touches a pipe.

    Subclassed rather than mocked because the thing under test is
    `IpcClient.request_async` itself — its threading, its bound, its shutdown. A mock
    of the class would be a test of the mock.
    """

    def __init__(self, delay_s: float = 0.2) -> None:
        super().__init__("stub-session")
        self._delay_s = delay_s
        self.sent: list[Command] = []
        self.fail_with: Exception | None = None
        # `request_async` refuses to queue on a closed channel, and there is no pipe
        # here to open. Set directly: the alternative is a fake pipe, which would put
        # the framing layer inside a test that is not about framing.
        self._running.set()

    def request(self, command: Command, params: dict[str, Any] | None = None) -> dict[str, Any]:
        time.sleep(self._delay_s)
        self.sent.append(command)
        if self.fail_with is not None:
            raise self.fail_with
        return {"id": "1", "cmd": command.value}


@pytest.fixture
def stub() -> Iterator[_StubClient]:
    client = _StubClient()
    try:
        yield client
    finally:
        client.close()


def test_request_async_returns_before_the_request_completes(stub: _StubClient) -> None:
    """The whole point, stated as a measurement rather than as a claim."""
    done = threading.Event()

    def finish(_: dict[str, Any]) -> None:
        done.set()

    began = time.monotonic()
    assert stub.request_async(Command.STOP_RECORD, on_done=finish)
    queued_after = time.monotonic() - began

    # The stub's `request` sleeps 200 ms. Queueing must not wait for it.
    assert queued_after < 0.05, f"request_async blocked for {queued_after * 1000:.0f} ms"
    assert done.wait(timeout=5.0), "the completion callback never fired"
    assert stub.sent == [Command.STOP_RECORD]


def test_the_callback_carries_the_engines_answer(stub: _StubClient) -> None:
    answers: list[dict[str, Any]] = []
    seen = threading.Event()

    def record(body: dict[str, Any]) -> None:
        answers.append(body)
        seen.set()

    stub.request_async(Command.GET_STATS, on_done=record)
    assert seen.wait(timeout=5.0)
    assert answers[-1]["cmd"] == "get_stats"


def test_a_failure_reaches_on_error_and_not_on_done(stub: _StubClient) -> None:
    stub.fail_with = IpcError("the engine went away")
    failures: list[Exception] = []
    seen = threading.Event()

    def record(error: Exception) -> None:
        failures.append(error)
        seen.set()

    def unexpected(_: dict[str, Any]) -> None:
        pytest.fail("on_done fired for a failed request")

    stub.request_async(Command.STOP_RECORD, on_done=unexpected, on_error=record)
    assert seen.wait(timeout=5.0)
    assert isinstance(failures[-1], IpcError)


def test_commands_are_issued_in_the_order_they_were_pressed(stub: _StubClient) -> None:
    """One worker, serialised.

    Not an implementation detail: these come from button presses, and a pause that
    reached the engine after the stop it preceded would be a recording state nobody
    asked for.
    """
    finished = threading.Event()
    stub.request_async(Command.PAUSE_RECORD)
    stub.request_async(Command.RESUME_RECORD)
    stub.request_async(Command.STOP_RECORD, on_done=lambda _: finished.set())

    assert finished.wait(timeout=10.0)
    assert stub.sent == [Command.PAUSE_RECORD, Command.RESUME_RECORD, Command.STOP_RECORD]


def test_the_backlog_is_bounded_and_drops_the_newest(stub: _StubClient) -> None:
    """CLAUDE.md hard rule 5 does not stop at the process boundary.

    A thread per click, or an unbounded queue, turns an engine that has stopped
    answering into an allocation problem. The newest is dropped rather than the oldest
    because the queued commands are the ones the user pressed first.
    """
    from framecapture_gui.ipc.client import _MAX_PENDING_COMMANDS

    accepted = [stub.request_async(Command.GET_STATS) for _ in range(_MAX_PENDING_COMMANDS + 6)]

    assert all(accepted[:_MAX_PENDING_COMMANDS]), "a command was refused while the queue had room"
    assert not accepted[-1], "the queue accepted more than its bound"
    assert accepted.count(False) >= 6 - 1, "the bound did not hold under a burst"


def test_a_raising_callback_does_not_kill_the_command_thread(stub: _StubClient) -> None:
    """One bad callback costs its own notification, not the channel.

    The same contract `_dispatch` already gives event handlers.
    """
    survived = threading.Event()

    def explode(_: dict[str, Any]) -> None:
        raise RuntimeError("boom")

    def survive(_: dict[str, Any]) -> None:
        survived.set()

    stub.request_async(Command.GET_STATS, on_done=explode)
    stub.request_async(Command.GET_HEALTH, on_done=survive)

    assert survived.wait(timeout=10.0), "the command thread died with the callback that raised"


def test_closing_stops_accepting_commands(stub: _StubClient) -> None:
    stub.close()
    assert not stub.request_async(Command.GET_STATS), "a closed channel queued a command"


# ---------------------------------------------------------------------------
# The exit criterion: a real stop, with the event loop measured
# ---------------------------------------------------------------------------


@pytest.mark.engine
@pytest.mark.parametrize("suffix", [".mkv", ".mp4"])
def test_the_event_loop_keeps_running_while_a_recording_is_finalized(
    engine_path: Path, qapp: QCoreApplication, tmp_path: Path, suffix: str
) -> None:
    """M9.6 Phase 0's exit criterion, measured rather than asserted.

    A `QTimer` at 50 ms counts how many times the GUI thread got control while
    `stop_record` was in flight. Under the old synchronous stop this count is **zero**
    by construction — the thread is inside `IpcClient.request` for the whole
    finalization — and every M9.6 feature that renders during a stop is impossible.

    The threshold is deliberately loose. What is being proven is "the loop runs at all",
    not a frame rate: the finalization of a one-second recording is fast, and a strict
    count would be measuring this machine's disk rather than the fix.
    """
    del qapp
    # Both containers, because they take **different finalization paths** and the
    # interesting one would otherwise go untested. MKV finalizes in place, so it never
    # reports a remux; MP4 takes SPEC.md §10.3's progressive remux, which is the only
    # phase with real byte progress behind it and the reason the event exists at all.
    output = tmp_path / f"async_stop{suffix}"
    controller = EngineController(engine_path)
    assert controller.start(), "the engine did not start"

    ticks = 0
    progress: list[dict[str, Any]] = []
    finalized: list[dict[str, Any]] = []

    def tick() -> None:
        nonlocal ticks
        ticks += 1

    timer = QTimer()
    timer.setInterval(50)
    timer.timeout.connect(tick)

    controller.finalize_progress.connect(progress.append)
    controller.recording_finalized.connect(finalized.append)

    try:
        with screen_activity():
            assert controller.start_recording(output, audio=False)
            deadline = time.monotonic() + 10.0
            while controller.state is not RecordingState.RECORDING and time.monotonic() < deadline:
                QCoreApplication.processEvents()
                time.sleep(0.02)
            assert controller.state is RecordingState.RECORDING, "the recording never started"

            time.sleep(1.0)

            timer.start()
            assert controller.stop_recording(), "stop_record was not queued"

            # Pumping *is* the test. A blocked GUI thread could not reach this loop.
            deadline = time.monotonic() + 40.0
            while not finalized and time.monotonic() < deadline:
                QCoreApplication.processEvents()
                time.sleep(0.01)
            timer.stop()

        assert finalized, "the recording never finalized"
        assert finalized[-1]["valid"] is True, finalized[-1].get("detail", "")
        assert output.is_file()
        assert ticks > 0, "the GUI thread never got control while stop_record was in flight"

        # Reported, not just asserted (CLAUDE.md §6). `ticks` is a 50 ms timer, so it is
        # a direct count of how many times the GUI thread got control during a real
        # finalization -- zero by construction under the old synchronous stop. These are
        # the numbers ACCEPTANCE.md quotes for §20 rows 20 and 22.
        duration = float(finalized[-1].get("duration_s", 0.0))
        print(  # noqa: T201 -- the measurement is the point
            f"\n[ MEASURED ] {suffix}: {ticks} event-loop ticks at 50 ms during the stop; "
            f"{len(progress)} finalize_progress events; phases "
            f"{[str(e.get('phase', '')) for e in progress]}; file {duration:.2f} s"
        )

        # The progress events are additive and this build's engine sends them, so their
        # absence here would mean the wiring is broken rather than the protocol old.
        assert progress, "no finalize_progress arrived during the stop"
        phases = [str(entry.get("phase", "")) for entry in progress]
        assert phases[-1] == "done", f"the last phase was {phases[-1]!r}, not 'done'"
        percents = [int(entry.get("percent", -1)) for entry in progress]
        assert percents == sorted(percents), f"progress went backwards: {percents}"
        assert percents[-1] == 100

        # The container-specific half. `finalize_in_place` announces `remuxing` before
        # the remux begins, independently of the 10 Hz throttle, so MP4 reports it even
        # for a recording too short to produce a second progress tick — which is what
        # makes this assertion meaningful on a one-second file.
        if suffix == ".mp4":
            assert "remuxing" in phases, f"an MP4 stop reported no remux phase: {phases}"
        else:
            assert "remuxing" not in phases, f"an MKV stop reported a remux it does not do: {phases}"
        assert "validating" in phases, f"the validation gate was not reported: {phases}"
    finally:
        controller.shutdown()
