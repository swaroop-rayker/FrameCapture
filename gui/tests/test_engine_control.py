"""The GUI driving a real engine, end to end (SPEC.md §24 M8, §3.1, §15.1, §7.5).

M8's exit criterion is "end-to-end recording driven entirely from the GUI, **including
pause and resume**". This is that criterion with the widgets taken out: the same
``EngineController`` the window uses, spawning the same engine into the same Job Object,
over the same pipe — the only thing absent is the button that calls it.

Widgets are absent deliberately rather than for convenience. A test that clicks a
``QPushButton`` proves the button is connected; it does not prove the recording is
valid, and it cannot run headless without a display server. What matters for the exit
criterion is that a *file* comes out, so that is what is asserted.

Marked ``engine`` because these spawn a real process and record for several seconds:

    pytest gui/tests -v -m "not engine"   # fast, no engine needed
"""

from __future__ import annotations

import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QCoreApplication

from framecapture_gui.engine import EngineController
from framecapture_gui.ipc.protocol import Command, RecordingState

from .screen_activity import screen_activity

pytestmark = pytest.mark.engine


@pytest.fixture
def controller(engine_path: Path, qapp: QCoreApplication) -> Iterator[EngineController]:
    """A started controller, always shut down.

    ``qapp`` is pytest-qt's application fixture, and it is not optional here -- see
    ``_pump``.
    """
    del qapp
    control = EngineController(engine_path)
    assert control.start(), "the engine did not start"
    try:
        yield control
    finally:
        control.shutdown()


def _state_of(controller: EngineController) -> RecordingState:
    """Read the controller's state through a function call.

    Not merely a shortcut. mypy narrows ``controller.state`` at an ``assert`` and keeps
    that narrowing across later statements -- which is unsound for a property backed by
    state another thread mutates, and makes a later ``assert ... is RECORDING`` look like
    an impossible comparison. Going through a call gives each read its own type.
    """
    return controller.state


def _pump() -> None:
    """Deliver whatever the reader thread has queued for the GUI thread.

    **This is not test scaffolding; it is the mechanism under test.** ``IpcClient``
    invokes its event callback on the reader thread, and ``EngineController`` emits a Qt
    signal from there. A signal crossing a thread boundary is delivered through the
    *receiving* thread's event loop -- which is exactly the property that makes it safe
    for a slot to touch a widget, and exactly why nothing above ``EngineController``
    needs a lock.

    A real application has that loop running inside ``app.exec()``. A test does not, so
    it pumps by hand. Written without this, the first version of this file saw only the
    transitions the *GUI thread* made and none of the ones the engine reported --
    measured, `[STARTING, STOPPING]` for a recording that demonstrably ran, paused,
    resumed and finalized.
    """
    app = QCoreApplication.instance()
    if app is not None:
        app.processEvents()


def _wait_for(predicate: Callable[[], bool], timeout_s: float = 10.0) -> bool:
    """Poll until ``predicate()`` is true, pumping the event loop. Returns whether it was."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        _pump()
        if predicate():
            return True
        time.sleep(0.05)
    _pump()
    return predicate()


def _record_for(seconds: float) -> None:
    """Let a recording run, keeping the event loop turning while it does.

    Not `time.sleep`. A real application is inside `app.exec()` for the whole recording,
    so a test that blocks its own event loop is not simulating one -- and concretely, it
    stops `screen_activity`'s timer, which is what keeps the captured display changing
    (see `screen_activity.py`). Every one of these cases used a bare sleep, and every one
    of them was therefore recording a frozen screen.
    """
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        _pump()
        time.sleep(0.01)


def test_the_gui_starts_the_engine_and_completes_the_handshake(controller: EngineController) -> None:
    assert controller.running
    assert _state_of(controller) is RecordingState.IDLE

    # SPEC.md §15.1's capability list. `pause_resume` is the one M8 turns on, and the
    # GUI asks rather than assuming — which is the whole point of the flags.
    assert controller.supports("pause_resume"), f"engine advertised {controller.capabilities}"
    assert controller.supports("heartbeat")

    # M9's other half. The GUI asks rather than assuming here too, and this assertion is
    # the flipped form of the one that recorded its absence -- kept rather than deleted,
    # because "the engine stopped claiming preview" is exactly as much of a regression as
    # "it claimed it before it worked".
    assert controller.supports("preview"), f"SPEC.md §15.2's preview ring: {controller.capabilities}"

    # M9.5's, flipped the same way and for the same reason.
    assert controller.supports("multitrack"), f"SPEC.md §8.6's Tier B: {controller.capabilities}"

    # And the honest absences that remain: SPEC.md §0.2 makes streaming and webcam
    # capture hard non-goals, so a capability list that grew one would be the first sign
    # something had been scaffolded in.
    assert not controller.supports("streaming"), "SPEC.md §0.2 forbids it"
    assert not controller.supports("webcam"), "SPEC.md §0.2 forbids it"


def test_a_recording_runs_pauses_resumes_and_finalizes_entirely_from_the_gui(
    controller: EngineController, output_path: Path
) -> None:
    """M8's exit criterion, in one test."""
    # The recording captures a real display, so the test owns what is on it
    # (CLAUDE.md §5). Without this the case reports on the desktop: measured, an
    # idle screen yields `decoded_frames=1, duration_s=0.017` and a correctly
    # rejected file. See `screen_activity.py`.
    with screen_activity():
        states: list[RecordingState] = []
        controller.state_changed.connect(states.append)

        finalized: list[dict[str, Any]] = []
        controller.recording_finalized.connect(finalized.append)

        # --- record ---------------------------------------------------------
        assert controller.start_recording(output_path, audio=False), "start_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING), (
            f"never reached RECORDING: {states}"
        )
        _record_for(1.5)

        # --- pause ----------------------------------------------------------
        assert controller.pause_recording(), "pause_record was refused"
        _pump()
        assert _state_of(controller) is RecordingState.PAUSED

        stats_while_paused = controller.get_stats()
        assert stats_while_paused is not None
        assert stats_while_paused["state"] == "paused"

        paused_for_s = 1.5
        _record_for(paused_for_s)

        # --- resume ---------------------------------------------------------
        assert controller.resume_recording(), "resume_record was refused"
        _pump()
        assert _state_of(controller) is RecordingState.RECORDING
        _record_for(1.5)

        # SPEC.md §15.1 requires `paused_total_ms` in `get_stats`, and §16.5 requires the
        # GUI to report paused time *separately* from the timeline. Both need this field to
        # be real, so it is asserted rather than merely read.
        stats = controller.get_stats()
        assert stats is not None
        paused_total_ms = int(stats["paused_total_ms"])
        assert paused_total_ms >= int(paused_for_s * 1000 * 0.8), f"paused_total_ms={paused_total_ms}"
        assert stats["pauses"] == 1

        # The quiesce held. Non-zero means frames were placed against a stale paused total
        # — see `timing::PauseClock`. SPEC.md §20 row 18 asserts the same thing on hardware.
        assert stats["pause_stragglers"] == 0

        # --- stop -----------------------------------------------------------
        assert controller.stop_recording(), "stop_record was refused"
        _pump()
        assert _state_of(controller) is RecordingState.IDLE

        assert finalized, "no recording_finalized was delivered"
        report = finalized[-1]
        assert report["valid"] is True, f"the engine reported an invalid file: {report}"
        assert int(report["decoded_frames"]) > 0

        # The file the user would open.
        assert output_path.is_file()
        assert output_path.stat().st_size > 0

        # SPEC.md §16.5: paused is a distinct state the GUI passed through, not a variant of
        # recording. If the transitions never included it, the window had nothing to render
        # differently and a user could lose footage without noticing.
        assert RecordingState.PAUSED in states, f"never observed PAUSED: {states}"


def test_pause_and_resume_are_idempotent(controller: EngineController, output_path: Path) -> None:
    """SPEC.md §7.5: "pausing a paused recording is a no-op that succeeds, not an error"."""
    # The recording captures a real display, so the test owns what is on it
    # (CLAUDE.md §5). Without this the case reports on the desktop: measured, an
    # idle screen yields `decoded_frames=1, duration_s=0.017` and a correctly
    # rejected file. See `screen_activity.py`.
    with screen_activity():
        assert controller.start_recording(output_path, audio=False)
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING)
        time.sleep(0.5)

        assert controller.pause_recording()
        assert controller.pause_recording(), "a redundant pause was refused"
        assert controller.pause_recording(), "a redundant pause was refused"
        _pump()
        assert _state_of(controller) is RecordingState.PAUSED
        time.sleep(0.5)

        assert controller.resume_recording()
        assert controller.resume_recording(), "a redundant resume was refused"
        _pump()
        assert _state_of(controller) is RecordingState.RECORDING

        assert controller.stop_recording()
        assert output_path.is_file()


def test_stopping_while_paused_still_yields_a_valid_file(controller: EngineController, output_path: Path) -> None:
    """SPEC.md §7.5's invariant, and CLAUDE.md §1's prime directive through the GUI."""
    # The recording captures a real display, so the test owns what is on it
    # (CLAUDE.md §5). Without this the case reports on the desktop: measured, an
    # idle screen yields `decoded_frames=1, duration_s=0.017` and a correctly
    # rejected file. See `screen_activity.py`.
    with screen_activity():
        finalized: list[dict[str, Any]] = []
        controller.recording_finalized.connect(finalized.append)

        assert controller.start_recording(output_path, audio=False)
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING)
        _record_for(1.0)

        assert controller.pause_recording()
        assert controller.stop_recording(), "stopping a paused recording was refused"
        _pump()

        assert finalized and finalized[-1]["valid"] is True
        assert output_path.is_file()


def test_a_refused_command_leaves_the_gui_honest_rather_than_optimistic(
    controller: EngineController,
) -> None:
    """Pausing when nothing is recording must not leave the window claiming PAUSED.

    SPEC.md §16.5: "The status panel never lies." A controller that set its state
    optimistically before the engine answered would show a paused recording that does
    not exist.
    """
    problems: list[tuple[str, str]] = []
    controller.notified.connect(lambda severity, text: problems.append((severity, text)))

    assert not controller.pause_recording(), "pausing with no recording was accepted"
    _pump()
    assert _state_of(controller) is RecordingState.IDLE, f"state is {_state_of(controller)}"
    assert problems, "the refusal was not reported to the user"
    assert problems[-1][0] == "error"


def test_the_engine_is_reaped_when_the_controller_shuts_down(engine_path: Path, qapp: QCoreApplication) -> None:
    """SPEC.md §3.1, from the GUI's side.

    The engine-side half of this is SPEC.md §20 row 13 on the GPU tier, which kills the
    host outright. This is the ordinary path: the GUI closes, and nothing is left behind.
    """
    del qapp
    control = EngineController(engine_path)
    assert control.start()

    process = control._process
    assert process is not None
    pid = process.pid
    assert pid > 0

    control.shutdown()

    assert not control.running
    assert _state_of(control) is RecordingState.OFFLINE
    assert not process.alive(), f"engine {pid} outlived the GUI that spawned it"


def test_settings_survive_an_engine_restart(engine_path: Path, qapp: QCoreApplication, tmp_path: Path) -> None:
    """SPEC.md §17, and the whole point of `save_config`.

    Asserted across a **real restart** rather than by reading back within one session:
    a `get_config` that returned what `save_config` was handed would pass even if the
    file was never written. The second engine is a fresh process that has only the file
    to go on.
    """
    del qapp

    # The engine writes to the real per-user config path (§17: "%LOCALAPPDATA%
    # \FrameCapture\config.toml. Never in Program Files"), so this restores whatever was
    # there rather than leaving the developer's settings changed by a test run.
    first = EngineController(engine_path)
    assert first.start()
    try:
        original = first.get_config()
        assert original is not None, "the engine did not report its configuration"
        config_path = Path(str(original["config_path"]))
        previous = config_path.read_bytes() if config_path.is_file() else None

        chosen_directory = str(tmp_path / "persisted")
        assert first.save_config(
            {
                "output_directory": chosen_directory,
                "cqp": 27,
                "fps": 30,
                "container": "mp4",
                "capture_cursor": False,
            }
        ), "save_config was refused"

        # It reached the disk, not just the engine's memory.
        assert config_path.is_file(), f"no config written at {config_path}"
    finally:
        first.shutdown()

    second = EngineController(engine_path)
    assert second.start(), "the second engine did not start"
    try:
        restored = second.get_config()
        assert restored is not None

        assert restored["cqp"] == 27, restored
        assert restored["fps"] == 30, restored
        assert restored["container"] == "mp4", restored
        assert restored["capture_cursor"] is False, restored
        assert Path(str(restored["output_directory"])) == Path(chosen_directory), restored

        # §17: "Never silently discard unknown keys." The schema version travels with
        # the file, so a round trip that lost it would mean the migration chain has
        # nothing to key off next time.
        assert restored["schema_version"] == original["schema_version"]
    finally:
        second.shutdown()
        # Put the developer's own configuration back.
        if previous is not None:
            config_path.write_bytes(previous)
        elif config_path.is_file():
            config_path.unlink()

        # And make sure the restore took, rather than leaving a test's settings behind.
        restore_check = EngineController(engine_path)
        if restore_check.start():
            after = restore_check.get_config()
            restore_check.shutdown()
            if after is not None and previous is not None:
                assert after["cqp"] == original["cqp"], "the developer's config was not restored"


def test_a_second_recording_in_the_same_engine_process_runs(controller: EngineController, tmp_path: Path) -> None:
    """BUG-037, at the seam it was reported from.

    Every other case in this file records once and then tears the engine down, so
    nothing exercised what a real session does constantly: the engine process is
    long-lived (SPEC.md §3.1) and ``start_record`` arrives again against whatever the
    previous recording left behind. What it left behind was a torn-down COM apartment,
    and the second recording's WGC probe called through C++/WinRT's process-wide factory
    cache into a library that had been unloaded -- an access violation, with the crash
    report's ``engine_phase`` still reading "uninitialized".

    Audio is **on**, unlike the cases above. The audio thread's apartment is one of the
    three that leave at stop, so a version of this with ``audio=False`` is a weaker
    exercise of exactly the thing that broke.

    The sequence is the reported one, including the ``save_config`` between the two: an
    engine that dies here dies without answering, so the assertion that actually catches
    the regression is that the second ``start_recording`` is answered at all.
    """
    # The recording captures a real display, so the test owns what is on it
    # (CLAUDE.md §5). Without this the case reports on the desktop: measured, an
    # idle screen yields `decoded_frames=1, duration_s=0.017` and a correctly
    # rejected file. See `screen_activity.py`.
    with screen_activity():
        first = tmp_path / "first.mkv"
        second = tmp_path / "second.mkv"

        assert controller.start_recording(first, audio=True), "the first start_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING)
        _record_for(1.0)

        assert controller.pause_recording(), "pause_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.PAUSED)
        _record_for(1.0)
        assert controller.resume_recording(), "resume_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING)
        _record_for(1.0)

        assert controller.stop_recording(), "stop_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.IDLE)
        assert first.is_file(), "the first recording produced no file"

        # As the reported session did, between the two.
        config = controller.get_config()
        assert config is not None
        assert controller.save_config(config), "save_config was refused"

        # The call that used to fault. `running` is checked first so a dead engine is
        # reported as a dead engine rather than as a refused command.
        assert controller.running, "the engine died between the two recordings (BUG-037)"
        assert controller.start_recording(second, audio=True), "the second start_record was refused"
        assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING), (
            "the second recording never reached RECORDING; the engine faulted on the WGC probe (BUG-037)"
        )
        _record_for(1.0)

        # Read while it is still recording: `get_stats` reports the *live* recording, so
        # after the stop below there is nothing to ask about.
        #
        # SPEC.md §7.5: the paused total belongs to a recording, not to the process. The
        # first recording paused once; a clock carried across would excise that time from a
        # file that was never paused, which is the same class of leak as BUG-037 on the other
        # side of the same seam.
        stats = controller.get_stats()
        assert stats is not None
        assert stats["pauses"] == 0, f"the second recording inherited {stats['pauses']} pause(s)"
        assert stats["paused_total_ms"] == 0

        assert controller.stop_recording()
        assert _wait_for(lambda: _state_of(controller) is RecordingState.IDLE)
        assert second.is_file(), "the second recording produced no file"


@pytest.mark.parametrize("suffix", [".mp4", ".mkv"])
def test_the_file_written_is_the_container_the_path_names(
    controller: EngineController, tmp_path: Path, suffix: str
) -> None:
    """BUG-043: choosing MP4 produced a real MP4 file called ``.mkv``.

    The engine held the container *setting* and the caller supplied the *path*, and
    nothing reconciled them. The log said it plainly and nothing acted on it::

        container opened path="...FrameCapture_2026-08-04_11-16-09.mkv" format=mp4

    The recording was correct and unopenable in anything that trusts extensions.

    **The path decides**, which is what makes this assertable at all: the request names
    its own container, so it means the same thing whatever this machine's ``config.toml``
    says. Resolving it the other way -- engine setting wins, path gets rewritten -- was
    tried first and made every recording test's outcome depend on the developer's saved
    container, which is the same defect arriving from the other side.

    The container setting is deliberately set to the *opposite* of the requested extension
    here, so a build that let the setting win would fail rather than coincidentally pass.
    """
    opposite = "mkv" if suffix == ".mp4" else "mp4"
    original = controller.get_config()
    assert original is not None

    try:
        assert controller.configure({"container": opposite}), "configure was refused"

        asked = tmp_path / f"recording{suffix}"

        # The recording captures a real display, so the test owns what is on it
        # (CLAUDE.md §5). See `screen_activity.py`.
        with screen_activity():
            assert controller.start_recording(asked, audio=False), "start_record was refused"
            assert _wait_for(lambda: _state_of(controller) is RecordingState.RECORDING)
            _record_for(1.0)
            assert controller.stop_recording()
            assert _wait_for(lambda: _state_of(controller) is RecordingState.IDLE)

        # Written where it was asked for, under the name it was asked for.
        assert asked.is_file(), f"on disk: {[p.name for p in tmp_path.iterdir()]}"
        assert asked.stat().st_size > 0

        # And the engine echoes that same path, so a caller can report the truth.
        echoed = controller.last_output
        assert echoed is not None and echoed.name == asked.name

        # The bytes are the container the name claims -- the whole point of BUG-043.
        header = asked.read_bytes()[:12]
        if suffix == ".mp4":
            assert header[4:8] == b"ftyp", f"not an MP4: {header!r}"
        else:
            assert header[:4] == bytes([0x1A, 0x45, 0xDF, 0xA3]), f"not a Matroska file: {header!r}"
    finally:
        controller.configure({"container": str(original["container"])})


def test_the_engine_refuses_multi_track_audio_on_mp4(controller: EngineController) -> None:
    """SPEC.md §20 row 16, from a separate process.

    > **MP4 is a hard block, not a soft warning.** ... the engine rejects a `configure`
    > command that requests both with `FcError::MULTITRACK_REQUIRES_MKV` (3021).

    The row's own wording is "enforced engine-side, not just in the GUI", and the way
    to hold that to account is to ask the engine directly rather than to ask the
    dialog. This is the same command a GUI sends, over the same pipe, to a real
    engine — with no dialog anywhere in the picture.

    Matched on the **numeric code**, which SPEC.md §19 makes the stable external
    contract. Matching on the prose is what breaks when the prose improves.
    """
    from framecapture_gui.ipc.client import EngineError

    client = controller._client
    assert client is not None

    # MKV first, and it is accepted. The negative control: without it, an engine that
    # refused every `configure` would satisfy the assertion below.
    accepted = client.request(Command.CONFIGURE, {"container": "mkv", "multitrack_enabled": True})
    assert accepted["multitrack_enabled"] is True

    # Then the combination the row is about.
    with pytest.raises(EngineError) as refusal:
        client.request(Command.CONFIGURE, {"container": "mp4", "multitrack_enabled": True})
    assert refusal.value.code == 3021, f"expected MULTITRACK_REQUIRES_MKV, got {refusal.value}"

    # And the refusal left **nothing** applied. A `configure` that had taken the
    # container and rejected the track setting would leave the engine holding a state
    # the caller never asked for -- which is worse than either outcome it chose
    # between.
    after = controller.get_config()
    assert after is not None
    assert after["container"] == "mkv", "the refused container was applied anyway"
    assert after["multitrack_enabled"] is True

    # Tidy up: leave the engine where the rest of the suite expects it.
    client.request(Command.CONFIGURE, {"multitrack_enabled": False})


def test_the_engine_reports_whether_this_machine_can_do_per_application_audio(
    controller: EngineController,
) -> None:
    """SPEC.md §8.6: "probe at runtime anyway, and degrade to Tier A with a GUI notice".

    The capability flag says the *engine* implements Tier B; this says whether the
    *machine* can perform it. They are different questions and the GUI needs both --
    one decides whether to show the control at all, the other what to say beside it.
    """
    config = controller.get_config()
    assert config is not None
    assert "multitrack_available" in config, "the engine did not report the §8.6 runtime probe"
    assert isinstance(config["multitrack_available"], bool)
    # The reference rig runs Windows 11, well past §8.6's 19041 floor.
    assert config["multitrack_available"] is True, (
        "process loopback probed unavailable on the reference rig; SPEC.md §1 makes "
        "Windows 10 19041 the floor and §8.6 requires no more than that"
    )
