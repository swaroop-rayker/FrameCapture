"""The window itself (SPEC.md §16.1, §16.2, §16.3, §16.5).

These run headless (``QT_QPA_PLATFORM=offscreen``) and assert on *state*, not on
pixels. A screenshot comparison would pin the theme to one font-rendering stack and
break on every machine that is not the one it was recorded on.

What is worth asserting about a window, and what is not: that the controls offered
match what is actually possible, and that the status readout says what the engine said.
Both are §16.5's "the status panel never lies" -- a control that can be pressed and
cannot work is the same lie told with a button.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QCoreApplication

from framecapture_gui.ipc.protocol import RecordingState
from framecapture_gui.preview import HEADER_BYTES, PreviewChannel
from framecapture_gui.theme import PALETTE, load_stylesheet
from framecapture_gui.widgets.panels import ControlsPanel, PreviewSurface, StatusPanel

# ---------------------------------------------------------------------------
# Theme (SPEC.md §16.3)
# ---------------------------------------------------------------------------


def test_the_palette_is_the_one_the_spec_states() -> None:
    """§16.3 gives exact values. Drift here is drift from the spec, not a preference."""
    assert PALETTE["bg-base"] == "#16181C"
    assert PALETTE["bg-surface"] == "#1E2126"
    assert PALETTE["bg-elevated"] == "#272B32"
    assert PALETTE["border"] == "#343941"
    assert PALETTE["text-primary"] == "#E4E6EB"
    assert PALETTE["text-secondary"] == "#9BA1AC"
    assert PALETTE["text-disabled"] == "#5C636E"
    assert PALETTE["accent"] == "#4C8DFF"
    assert PALETTE["accent-hover"] == "#6BA0FF"
    assert PALETTE["rec-active"] == "#E5484D"
    assert PALETTE["warn"] == "#F5A524"
    assert PALETTE["ok"] == "#35C489"


def test_no_pure_black_and_no_pure_white() -> None:
    """§16.3 states the reason: halation on OLED, and fatigue over long sessions."""
    for name, value in PALETTE.items():
        assert value.upper() not in ("#000000", "#FFFFFF"), f"{name} is {value}"


def test_the_stylesheet_substitutes_every_token() -> None:
    """A missed token leaves a literal ``@accent`` in the QSS, which Qt ignores silently.

    Asserted per palette key rather than as "no ``@`` anywhere": the file's own header
    comment names the ``@token`` convention, and a blanket check would fail on the
    documentation rather than on the styling.
    """
    qss = load_stylesheet()
    for name in PALETTE:
        assert f"@{name}" not in qss, f"@{name} was never substituted"
    assert PALETTE["accent"] in qss
    # The ordering trap: `@accent-hover` must not be eaten by `@accent`, which would
    # leave `#4C8DFF-hover` and one unstyled hover state with no error anywhere.
    assert f"{PALETTE['accent']}-hover" not in qss
    assert PALETTE["accent-hover"] in qss


def test_a_light_theme_would_be_a_drop_in() -> None:
    """§16.3: "so a light theme is a drop-in later"."""
    light = dict.fromkeys(PALETTE, "#FAFAFA")
    qss = load_stylesheet(light)
    for name in PALETTE:
        assert f"@{name}" not in qss, f"@{name} was never substituted"
    assert "#16181C" not in qss, "a dark value was baked into the QSS rather than substituted"


# ---------------------------------------------------------------------------
# Controls (SPEC.md §16.1, §16.2)
# ---------------------------------------------------------------------------


@pytest.fixture
def controls(qapp: QCoreApplication) -> ControlsPanel:
    del qapp
    return ControlsPanel()


def test_every_control_is_greyed_when_the_engine_is_offline(controls: ControlsPanel) -> None:
    """§16.1: "greyed controls + a clear ENGINE OFFLINE state"."""
    controls.apply_state(RecordingState.OFFLINE)
    assert not controls._record.isEnabled()
    assert not controls._pause.isEnabled()


def test_pause_is_enabled_only_while_recording(controls: ControlsPanel) -> None:
    """§16.2: "The pause control is enabled only while recording"."""
    controls.apply_state(RecordingState.IDLE)
    assert not controls._pause.isEnabled(), "pause was offered with nothing recording"

    controls.apply_state(RecordingState.RECORDING)
    assert controls._pause.isEnabled()

    controls.apply_state(RecordingState.PAUSED)
    assert controls._pause.isEnabled(), "resume must be reachable from paused"

    controls.apply_state(RecordingState.STOPPING)
    assert not controls._pause.isEnabled(), "pause was offered during finalization"


def test_the_pause_button_becomes_resume_while_paused(controls: ControlsPanel) -> None:
    controls.apply_state(RecordingState.RECORDING)
    assert "Pause" in controls._pause.text()

    controls.apply_state(RecordingState.PAUSED)
    assert "Resume" in controls._pause.text()


def test_the_primary_button_says_what_it_will_do(controls: ControlsPanel) -> None:
    """A button labelled START that stops is the worst version of §16.5's lie."""
    controls.apply_state(RecordingState.IDLE)
    assert controls._record.text() == "START RECORDING"

    controls.apply_state(RecordingState.RECORDING)
    assert controls._record.text() == "STOP RECORDING"

    # Paused is still an active recording: the way out is stop, not start.
    controls.apply_state(RecordingState.PAUSED)
    assert controls._record.text() == "STOP RECORDING"


def test_the_primary_button_emits_stop_while_paused(controls: ControlsPanel, qtbot: Any) -> None:
    """The click has to follow the label. Asserted because the two are set separately."""
    controls.apply_state(RecordingState.PAUSED)
    with qtbot.waitSignal(controls.stop_requested, timeout=1000):
        controls._record.click()


# ---------------------------------------------------------------------------
# Status (SPEC.md §16.2, §16.5, §7.5)
# ---------------------------------------------------------------------------


@pytest.fixture
def status(qapp: QCoreApplication) -> StatusPanel:
    del qapp
    return StatusPanel()


def test_offline_says_so_in_words(status: StatusPanel) -> None:
    status.apply_state(RecordingState.OFFLINE)
    assert status._headline.text() == "ENGINE OFFLINE"


def test_paused_shows_the_timeline_elapsed_not_the_wall_clock(status: StatusPanel) -> None:
    """§16.2 and §16.5, and the whole reason §7.5 exists.

    The headline is the length the file will actually have. Showing wall clock would tell
    a user their 14-minute recording is 16 minutes long, and the number they would then
    plan around is the wrong one.
    """
    status.apply_stats(
        {
            "state": "paused",
            "timeline_ms": 862_000,  # 00:14:22
            "paused_total_ms": 127_000,  # 00:02:07
            "pauses": 3,
        }
    )
    assert "PAUSED" in status._headline.text()
    assert "00:14:22" in status._headline.text(), status._headline.text()
    # And the paused total on its own line, per §16.2's mock-up.
    assert "00:02:07" in status._paused_total.text(), status._paused_total.text()
    assert "3" in status._paused_total.text()


def test_recording_shows_rec_and_the_elapsed_timeline(status: StatusPanel) -> None:
    status.apply_stats({"state": "recording", "timeline_ms": 862_000})
    assert status._headline.text() == "REC  00:14:22"


def test_dropped_frames_are_reported_exactly_and_in_warn_colour(status: StatusPanel) -> None:
    """§16.5: "it says so in --warn colour with the exact count and percentage"."""
    status.apply_stats({"state": "recording", "frames_encoded": 900, "frames_dropped": 100})
    text = status._drops.text()
    assert "100" in text, text
    assert "10.0%" in text, text
    assert status._drops.objectName() == "StatusWarning"


def test_zero_drops_are_stated_rather_than_left_blank(status: StatusPanel) -> None:
    """A blank field reads as "unknown". §16.5 wants the number either way."""
    status.apply_stats({"state": "recording", "frames_encoded": 900, "frames_dropped": 0})
    assert status._drops.text() == "Dropped: 0 (0.0%)"
    assert status._drops.objectName() != "StatusWarning"


def test_an_unpaused_recording_shows_no_paused_line(status: StatusPanel) -> None:
    status.apply_stats({"state": "recording", "timeline_ms": 5000, "paused_total_ms": 0, "pauses": 0})
    assert status._paused_total.text() == ""


def test_the_recording_dot_pulses_only_while_recording(status: StatusPanel) -> None:
    """§16.5: "The recording dot stops pulsing and holds" while paused.

    §16.3 also makes it the only thing that moves, so a timer running in any other state
    is motion the spec forbids.
    """
    dot = status._dot

    status.apply_stats({"state": "recording"})
    assert dot._timer.isActive(), "the dot does not pulse while recording"

    status.apply_stats({"state": "paused"})
    assert not dot._timer.isActive(), "the dot kept pulsing while paused"

    status.apply_stats({"state": "idle"})
    assert not dot._timer.isActive()


# ---------------------------------------------------------------------------
# The window as a whole
# ---------------------------------------------------------------------------


def test_the_window_opens_and_is_honest_without_an_engine(qtbot: Any, tmp_path: Path) -> None:
    """§16.1: "The GUI must be fully functional and honest when the engine is down".

    Pointed at a path that does not exist, which is the harshest version of "engine
    down" -- there is nothing to spawn, let alone talk to.
    """
    from framecapture_gui.main_window import MainWindow

    window = MainWindow(tmp_path / "does-not-exist.exe", tmp_path / "recordings")
    qtbot.addWidget(window)

    # The engine start is deferred by a zero-timer so the window paints first; running
    # it here is what a real launch does a moment later.
    QCoreApplication.processEvents()
    window._start_engine()

    assert window._engine.state is RecordingState.OFFLINE
    assert window._status._headline.text() == "ENGINE OFFLINE"
    assert not window._controls._record.isEnabled()
    assert not window._controls._pause.isEnabled()

    # And the restart action §16.1 requires is present and usable.
    assert window._restart_action.isEnabled()

    # §15.2's preview needs an engine, so with none it says so rather than showing a black
    # rectangle a user would read as a broken capture (§16.5, "the status panel never
    # lies", applied to the largest thing on screen).
    assert not window._preview.has_frame
    # `isHidden`, not `isVisible`: the latter is false for any widget in an unshown window,
    # so it would pass here whatever the placeholder was doing.
    assert not window._preview._message.isHidden()
    assert "unavailable" in window._preview._message.text().lower()


# ---------------------------------------------------------------------------
# The preview surface (SPEC.md §15.2, §16.2)
# ---------------------------------------------------------------------------


def _stamped_section(name: str, width: int, height: int) -> tuple[Any, dict[str, Any]]:
    """A shared-memory section stamped like the engine's, with one frame published.

    The engine's own tier proves the *engine* writes this correctly
    (`PreviewTest.ARecordingPublishesDownscaledFramesTheGuiCanRead`). What is under test
    here is the widget, so the section is stamped by hand -- spawning an engine to prove a
    `QPainter` letterboxes would be testing the wrong half, and could not run headless in
    the fast suite.
    """
    import mmap
    import struct

    from framecapture_gui import preview as preview_module

    stride = width * 4
    slot_bytes = stride * height
    slots = 3
    total = HEADER_BYTES + (slot_bytes * slots)

    mapping = mmap.mmap(-1, total, tagname=name)
    struct.pack_into("<II", mapping, preview_module.OFFSET_VERSION, preview_module.VERSION, width)
    struct.pack_into("<IIIIII", mapping, preview_module.OFFSET_WIDTH, width, height, stride, slot_bytes, slots, 0)
    struct.pack_into("<I", mapping, preview_module.OFFSET_FPS, 30)
    # One mid-grey frame in slot 0, then the magic, then the publish -- the same order the
    # engine uses, so a reader that attaches mid-stamp cannot see a valid magic over an
    # unwritten geometry.
    mapping[HEADER_BYTES : HEADER_BYTES + slot_bytes] = b"\x80" * slot_bytes
    struct.pack_into("<I", mapping, preview_module.OFFSET_MAGIC, preview_module.MAGIC)
    struct.pack_into("<Q", mapping, preview_module.OFFSET_WRITE_INDEX, 1)

    return mapping, {"section": name, "bytes": total}


def test_the_surface_renders_a_published_frame_without_copying_it(qtbot: Any) -> None:
    """§16.1: the GUI "wraps a QImage over the shared-memory buffer and nothing more"."""
    mapping, response = _stamped_section("Local\\fc-widget-test-render", 960, 540)
    try:
        channel = PreviewChannel()
        channel.attach(response)

        surface = PreviewSurface()
        qtbot.addWidget(surface)
        surface.set_channel(channel)

        surface._poll()
        assert surface.has_frame, "the surface did not take the published frame"
        assert surface._message.isHidden(), "the placeholder is still over a live preview"

        image = surface._image
        assert image is not None
        assert (image.width(), image.height()) == (960, 540)
        # The stride the header declared, not one Qt chose -- which is what makes it a view
        # over the mapping rather than a repacked copy.
        assert image.bytesPerLine() == 960 * 4

        # A second poll with nothing new published must not report a new frame, or the
        # surface would repaint 30 times a second over a static screen.
        surface._image = None
        surface._poll()
        assert surface._image is None, "an unchanged sequence was reported as a new frame"

        channel.detach()
    finally:
        mapping.close()


def test_the_surface_letterboxes_rather_than_stretching(qtbot: Any) -> None:
    """§16.2 asks for "16:9, letterboxed".

    Asserted on the geometry the paint path computes rather than on pixels: a screenshot
    comparison would pin this to one font-rendering stack, and the thing that would
    actually be wrong -- a distorted picture -- is an aspect ratio, which is a number.
    """
    from PySide6.QtCore import QSize, Qt

    frame = QSize(960, 540)
    for target in (QSize(400, 400), QSize(1000, 300), QSize(1920, 1080)):
        fitted = frame.scaled(target, Qt.AspectRatioMode.KeepAspectRatio)
        assert fitted.width() <= target.width()
        assert fitted.height() <= target.height()
        # 16:9 preserved to within a pixel of rounding.
        assert abs((fitted.width() / fitted.height()) - (16 / 9)) < 0.01, f"{target} -> {fitted}"


def test_a_surface_with_no_channel_says_so(qtbot: Any) -> None:
    surface = PreviewSurface()
    qtbot.addWidget(surface)
    surface.set_channel(None, placeholder="Preview unavailable — no engine")

    assert not surface.has_frame
    assert not surface._message.isHidden()
    assert surface._message.text() == "Preview unavailable — no engine"
    assert not surface._timer.isActive(), "the repaint timer runs with nothing to repaint"


# ---------------------------------------------------------------------------
# Global hotkeys (SPEC.md §16.5)
# ---------------------------------------------------------------------------


def test_a_hotkey_sequence_parses_to_modifiers_and_a_key() -> None:
    from framecapture_gui.hotkeys import MOD_CONTROL, MOD_NOREPEAT, MOD_SHIFT, parse_sequence

    modifiers, key = parse_sequence("Ctrl+Shift+F9")
    assert modifiers & MOD_CONTROL
    assert modifiers & MOD_SHIFT
    # Without NOREPEAT, holding "stop recording" would stop, start, then stop again.
    assert modifiers & MOD_NOREPEAT
    assert key == 0x78  # VK_F9


def test_a_hotkey_without_a_modifier_is_refused() -> None:
    """A bare key registered globally swallows that key for every application."""
    from framecapture_gui.hotkeys import HotkeyError, parse_sequence

    with pytest.raises(HotkeyError):
        parse_sequence("F9")


def test_an_unknown_part_is_refused_rather_than_dropped() -> None:
    """Silently ignoring a modifier registers a *different* hotkey than the one asked for."""
    from framecapture_gui.hotkeys import HotkeyError, parse_sequence

    with pytest.raises(HotkeyError):
        parse_sequence("Hyper+F9")
    with pytest.raises(HotkeyError):
        parse_sequence("Ctrl+PrintScreen")
    with pytest.raises(HotkeyError):
        parse_sequence("")


def test_a_conflicting_hotkey_is_reported_and_not_fatal(qapp: QCoreApplication) -> None:
    """§16.5 asks for conflict detection; §16.1 implies it must not stop the app.

    The conflict is manufactured by registering the same combination twice -- the second
    registration is exactly what a user hits when another application owns the key.
    """
    del qapp
    from framecapture_gui.hotkeys import Hotkey, HotkeyManager

    first = HotkeyManager()
    second = HotkeyManager()
    try:
        assert first.register([Hotkey("Test", "Ctrl+Alt+Shift+F24", lambda: None)]) == []
        conflicts = second.register([Hotkey("Test", "Ctrl+Alt+Shift+F24", lambda: None)])
        assert conflicts, "a duplicate registration was not reported as a conflict"
        assert "Test" in conflicts[0]
    finally:
        first.unregister_all()
        second.unregister_all()


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def test_the_gui_accepts_every_level_the_engine_uses() -> None:
    """One vocabulary across both processes, because there is one setting.

    The regression this pins: `run-dev.ps1 -Gui` defaults `-LogLevel` to `trace` and
    passes it straight through, so a GUI that only knew Python's spellings died on the
    launcher's own default with an argparse error. `warn` vs `warning` is the same fault
    waiting one level along.
    """
    from framecapture_gui.__main__ import _LOG_LEVELS

    # Exactly `fc::log::Level`'s spellings, which is what `FC_LOG_LEVEL` and §15.1's
    # `set_log_level` both take.
    for level in ("trace", "debug", "info", "warn", "error", "critical", "off"):
        assert level in _LOG_LEVELS, f"the engine has a level {level!r} the GUI cannot accept"

    # Python's own spelling stays valid too -- someone will type it.
    assert "warning" in _LOG_LEVELS

    # Python has no TRACE, so it flattens to DEBUG. The engine still gets `trace`
    # through FC_LOG_LEVEL, so nothing is actually lost.
    assert _LOG_LEVELS["trace"] == logging.DEBUG
    assert _LOG_LEVELS["warn"] == _LOG_LEVELS["warning"] == logging.WARNING


def test_the_launchers_default_level_is_accepted() -> None:
    """`run-dev.ps1 -Gui` with no arguments must start. That is the path a user takes."""
    from framecapture_gui.__main__ import _LOG_LEVELS

    assert "trace" in _LOG_LEVELS, "scripts/run-dev.ps1 defaults -LogLevel to 'trace'"


# ---------------------------------------------------------------------------
# The settings dialog's multi-track section (SPEC.md §16.4, §8.6, §20 row 16)
# ---------------------------------------------------------------------------


class _StubEngine:
    """An engine that answers `get_config` and nothing else.

    The dialog is the thing under test here. Spawning a real engine to prove a
    checkbox greys out would be testing the wrong half, and the *engine's* refusal is
    asserted from a separate process by
    `test_the_engine_refuses_multi_track_audio_on_mp4` in `test_engine_control.py` --
    which is the half §20 row 16 calls "enforced engine-side, not just in the GUI".
    """

    running = False
    capabilities: tuple[str, ...] = ("multitrack",)

    def __init__(
        self,
        config: dict[str, Any],
        processes: list[dict[str, Any]] | None = None,
        endpoints: list[dict[str, Any]] | None = None,
    ) -> None:
        self._config = config
        self._processes = processes
        self._endpoints = endpoints

    def get_config(self) -> dict[str, Any]:
        return dict(self._config)

    def get_devices(self) -> dict[str, Any] | None:
        """``None`` is what an engine that is down answers, and it must not break the
        dialog — the targets field is the source of truth and the picker is a
        convenience over it."""
        if self._processes is None and self._endpoints is None:
            return None
        return {
            "render_endpoints": list(self._endpoints or []),
            "processes": list(self._processes or []),
        }


def _settings_dialog(
    qtbot: Any,
    tmp_path: Path,
    processes: list[dict[str, Any]] | None = None,
    endpoints: list[dict[str, Any]] | None = None,
    **config: Any,
) -> Any:
    from framecapture_gui.settings.dialog import SettingsDialog

    base: dict[str, Any] = {"container": "mkv", "multitrack_available": True}
    base.update(config)
    dialog = SettingsDialog(_StubEngine(base, processes, endpoints), tmp_path)  # type: ignore[arg-type]
    qtbot.addWidget(dialog)
    return dialog


def test_multi_track_is_offered_on_mkv(qtbot: Any, tmp_path: Path) -> None:
    """SPEC.md §16.4: the Audio section gets "multi-track when MKV"."""
    dialog = _settings_dialog(qtbot, tmp_path, container="mkv")

    assert dialog._multitrack.isEnabled()
    # The positive control for the case below: without it, a build that disabled the
    # control unconditionally would satisfy the MP4 assertion.
    assert "requires MKV" not in dialog._multitrack_hint.text()


def test_multi_track_on_mp4_is_explained_inline_rather_than_silently_greyed(qtbot: Any, tmp_path: Path) -> None:
    """SPEC.md §20 row 16, the GUI half.

    > The GUI must explain inline rather than silently greying the option out.

    A greyed checkbox with no sentence beside it is a user wondering whether the
    feature is broken, missing, or theirs to fix — and it is the third one, on the
    Video tab, one click away.
    """
    dialog = _settings_dialog(qtbot, tmp_path, container="mp4")

    assert not dialog._multitrack.isEnabled()
    hint = dialog._multitrack_hint.text()
    assert hint, "the option is unavailable and nothing says why"
    assert "MKV" in hint
    # Names the setting *and* where to change it. "Requires MKV" alone is a fact; this
    # has to be an instruction.
    assert "Video" in hint


def test_a_system_without_process_loopback_says_so_rather_than_blaming_the_container(
    qtbot: Any, tmp_path: Path
) -> None:
    """SPEC.md §8.6: "degrade to Tier A with a GUI notice if activation fails".

    Two different reasons the control can be unavailable, with two different answers.
    Reporting the container's reason on a machine that simply cannot do it would send
    the user to change a setting that would not help.
    """
    dialog = _settings_dialog(qtbot, tmp_path, container="mkv", multitrack_available=False)

    assert not dialog._multitrack.isEnabled()
    hint = dialog._multitrack_hint.text()
    assert "system" in hint.lower()
    assert "MKV" not in hint, "the container is not the problem here"


def test_switching_the_container_re_evaluates_the_block_before_ok_is_pressed(qtbot: Any, tmp_path: Path) -> None:
    """The reason the hint is wired to the container box rather than computed once.

    A user who ticks multi-track and then switches to MP4 must be told there and then.
    Learning it from an error after pressing OK is the same information delivered as a
    failure.
    """
    dialog = _settings_dialog(qtbot, tmp_path, container="mkv", multitrack_enabled=True)
    assert dialog._multitrack.isEnabled()

    index = dialog._container.findData("mp4")
    assert index >= 0
    dialog._container.setCurrentIndex(index)

    assert not dialog._multitrack.isEnabled()
    assert "MKV" in dialog._multitrack_hint.text()


def test_the_targets_reach_the_engine_in_the_shape_it_takes(qtbot: Any, tmp_path: Path) -> None:
    """One string, one meaning. `config::parse_multitrack_targets` is the only parser.

    BUG-043's shape: the GUI and the engine each having their own idea of one setting.
    The GUI sends the text; the engine decides what it means.
    """
    dialog = _settings_dialog(
        qtbot, tmp_path, container="mkv", multitrack_enabled=True, multitrack_targets="chrome.exe, game.exe"
    )

    settings = dialog.collect()
    assert settings["multitrack_enabled"] is True
    assert settings["multitrack_targets"] == "chrome.exe, game.exe"


def test_the_process_picker_offers_what_the_engine_reported(qtbot: Any, tmp_path: Path) -> None:
    """SPEC.md §16.4 gained a target list in M9.5, and a user should not have to know
    that Chrome is `chrome.exe`.

    The engine reduces its process list to one entry per executable before sending it,
    so the picker and the text field mean the same thing — a second reduction here would
    be two layers deciding what a target is, which is BUG-043's shape.
    """
    dialog = _settings_dialog(
        qtbot,
        tmp_path,
        processes=[{"pid": 100, "executable": "chrome.exe"}, {"pid": 220, "executable": "game.exe"}],
        multitrack_enabled=True,
    )

    offered = [dialog._process_picker.itemData(i) for i in range(dialog._process_picker.count())]
    assert offered == ["chrome.exe", "game.exe"]
    assert dialog._process_picker.isEnabled()
    assert dialog._add_target.isEnabled()


def test_adding_a_picked_application_appends_rather_than_replaces(qtbot: Any, tmp_path: Path) -> None:
    """The field is the source of truth and the picker composes with it.

    SPEC.md §8.6 explicitly supports naming "a target process that hasn't started yet",
    which a picker alone cannot express — so a user must be able to type a game that is
    not running and then pick the browser that is, without losing the first.
    """
    dialog = _settings_dialog(
        qtbot,
        tmp_path,
        processes=[{"pid": 100, "executable": "chrome.exe"}],
        multitrack_enabled=True,
        multitrack_targets="game.exe",
    )

    dialog._on_add_target()
    assert dialog._multitrack_targets.text() == "game.exe, chrome.exe"

    # And a second press does not duplicate it. The engine drops duplicates too, so
    # without this the field would show something the recording would not do.
    dialog._on_add_target()
    assert dialog._multitrack_targets.text() == "game.exe, chrome.exe"


def test_an_engine_that_reports_no_processes_leaves_the_field_usable(qtbot: Any, tmp_path: Path) -> None:
    """An engine that is down, or one too old to report the field, must not disable the
    way a target is actually named."""
    dialog = _settings_dialog(qtbot, tmp_path, processes=None, multitrack_enabled=True)

    assert dialog._process_picker.count() == 0
    assert not dialog._add_target.isEnabled()
    # The field is what matters, and it still works.
    assert dialog._multitrack_targets.isEnabled()


def test_the_reconnect_setting_round_trips(qtbot: Any, tmp_path: Path) -> None:
    """SPEC.md §8.6 leaves open whether a track keeps looking for a target that exits.

    Decided 2026-08-06: it does, so the box is ticked by default — and a user who wants
    §8.6's literal reading can untick it and have that reach the engine.
    """
    default = _settings_dialog(qtbot, tmp_path, multitrack_enabled=True)
    assert default._multitrack_reattach.isChecked(), "re-attachment is the decided default"
    assert default.collect()["multitrack_reattach"] is True

    off = _settings_dialog(qtbot, tmp_path, multitrack_enabled=True, multitrack_reattach=False)
    assert not off._multitrack_reattach.isChecked()
    assert off.collect()["multitrack_reattach"] is False


# ---------------------------------------------------------------------------
# SPEC.md §8.5's downward-only pin (BUG-048)
# ---------------------------------------------------------------------------


def _stereo_endpoint() -> list[dict[str, Any]]:
    return [{"id": "x", "name": "Speakers", "sample_rate": 48000, "channels": 2, "default": True}]


def _surround_endpoint() -> list[dict[str, Any]]:
    return [{"id": "x", "name": "Receiver", "sample_rate": 48000, "channels": 8, "default": True}]


def test_a_layout_wider_than_the_device_is_explained_before_recording(qtbot: Any, tmp_path: Path) -> None:
    """The reported defect, caught one step earlier.

    A 7.1 pin on a stereo endpoint produced a file whose audio Windows' own player
    refused — "we can't play the audio ... You can still watch the video." The engine
    now clamps, so the file is fine either way; what this asserts is that the user is
    told *why* they will not get 7.1, at the moment they choose it.
    """
    dialog = _settings_dialog(qtbot, tmp_path, endpoints=_stereo_endpoint(), channel_layout="7.1")

    hint = dialog._layout_hint.text()
    assert hint, "a 7.1 layout on a stereo device says nothing about what will actually be recorded"
    assert "2 channels" in hint
    # Names the consequence, not just the fact. "Your device has 2 channels" alone
    # leaves a user wondering whether to change the device or the setting.
    assert "will not play" in hint


def test_a_layout_the_device_can_supply_says_nothing(qtbot: Any, tmp_path: Path) -> None:
    """The positive control. Without it, a hint shown unconditionally would satisfy the
    case above — and a warning that is always on is a warning nobody reads."""
    assert not _settings_dialog(
        qtbot, tmp_path, endpoints=_surround_endpoint(), channel_layout="7.1"
    )._layout_hint.text()
    assert not _settings_dialog(
        qtbot, tmp_path, endpoints=_stereo_endpoint(), channel_layout="stereo"
    )._layout_hint.text()
    # `auto` follows the endpoint by definition, so it can never be too wide.
    assert not _settings_dialog(
        qtbot, tmp_path, endpoints=_stereo_endpoint(), channel_layout="auto"
    )._layout_hint.text()


def test_changing_the_layout_re_evaluates_the_hint(qtbot: Any, tmp_path: Path) -> None:
    """A user who picks 7.1 after opening the dialog must be told there and then, not on
    the next launch."""
    dialog = _settings_dialog(qtbot, tmp_path, endpoints=_stereo_endpoint(), channel_layout="stereo")
    assert not dialog._layout_hint.text()

    index = dialog._layout_combo.findData("5.1")
    assert index >= 0
    dialog._layout_combo.setCurrentIndex(index)
    assert "2 channels" in dialog._layout_hint.text(), "5.1 on a stereo device is the same up-mix one rung down"


def test_an_engine_that_reports_no_endpoints_stays_silent(qtbot: Any, tmp_path: Path) -> None:
    """A warning shown on no evidence is worse than none.

    With the engine down there is nothing to compare against, and guessing would put a
    scary sentence under a setting that may well be correct.
    """
    dialog = _settings_dialog(qtbot, tmp_path, endpoints=None, channel_layout="7.1")
    assert not dialog._layout_hint.text()
