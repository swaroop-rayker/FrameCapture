"""Every menu action does something (M9.6 Phase 4, SPEC.md §16.2).

The named test for this phase, and the sweep at the top of it is the point: `Edit`,
`View` and `Tools` were three `addMenu` calls with no actions, and the way that stayed
true through nine milestones is that no test ever asked what was in them. A
parametrised walk of `menuBar()` is what catches the next empty menu, so it walks the
menu bar rather than a list of actions this file maintains -- a hand-kept list would
have been just as empty as the menus.

**The fixture applies `load_stylesheet()`** (M9_6_CHANGES.md §5, lesson 2). Without it
the theme's interactions are absent and a styling regression test passes with its fix
reverted.

Every modal is redirected, not because modals are hard to test but because a modal in a
headless run blocks forever. What each one was *asked* is then assertable, which is how
the "did it actually do the thing" half of these tests works.
"""

from __future__ import annotations

import json
import zipfile
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QApplication, QMenu

from framecapture_gui import diagnostics, recovery
from framecapture_gui.ipc.protocol import Command, RecordingState, timeout_for
from framecapture_gui.main_window import LOG_LEVELS, MainWindow
from framecapture_gui.theme import load_stylesheet

# ---------------------------------------------------------------------------
# A window with a stub engine
# ---------------------------------------------------------------------------

#: What a healthy engine answers `get_config` with, trimmed to the keys the menus read.
_CONFIG: dict[str, Any] = {
    "output_directory": "D:/recordings",
    "container": "mkv",
    "encoder": "auto",
    "fps": 60,
    "cqp": 20,
    "channel_layout": "stereo",
    "audio_bitrate_kbps": 160,
    "capture_backend": "auto",
    "capture_cursor": True,
    "hdr_tonemap": True,
    "log_level": "info",
    "schema_version": 2,
    "config_path": "C:/Users/someone/AppData/Local/FrameCapture/config.toml",
    "pill_enabled": True,
    "toasts_enabled": True,
    "show_preview": True,
    "show_sources": True,
    "show_audio_mixer": True,
    "show_controls": True,
    "show_status": True,
    "always_on_top": False,
}

_TOPOLOGY: dict[str, Any] = {
    "adapters": [
        {"luid": 111, "description": "AMD Radeon 780M Graphics", "owns_output": True},
        {"luid": 222, "description": "NVIDIA GeForce RTX 4050 Laptop GPU", "owns_output": False},
    ]
}


class _RecordingEngine:
    """Records what the menus asked the engine to do.

    A stub rather than a real `EngineController` because every one of these assertions is
    about the *GUI's* half of the conversation -- which command, with which parameters --
    and running a real engine to check that a menu item sends `set_log_level` would make
    a menu test depend on a GPU.
    """

    def __init__(self) -> None:
        self.saved: list[dict[str, Any]] = []
        self.log_levels: list[str] = []
        self.recovered: list[Path] = []
        self.restarts = 0
        self.topology_calls = 0
        self.log_level = ""
        self.last_output: Path | None = None
        self.state = RecordingState.IDLE
        self.capabilities = ["preview"]
        self.engine_version = "0.1.0"
        self.protocol = "1.0"
        #: Set false to test the offline paths, which is where the diagnostics summary
        #: matters most.
        self.online = True

    # -- the calls the menus make ------------------------------------------
    def get_config(self) -> dict[str, Any] | None:
        return dict(_CONFIG) if self.online else None

    def get_gpu_topology(self) -> dict[str, Any] | None:
        self.topology_calls += 1
        return dict(_TOPOLOGY) if self.online else None

    def get_health(self) -> dict[str, Any] | None:
        return {"rung": 0} if self.online else None

    def save_config(self, settings: dict[str, Any]) -> bool:
        self.saved.append(dict(settings))
        return self.online

    def set_log_level(self, level: str) -> bool:
        if not self.online:
            return False
        self.log_levels.append(level)
        self.log_level = level
        return True

    def recover(self, sidecar: Path) -> bool:
        self.recovered.append(sidecar)
        return self.online

    # -- everything the window touches but these tests do not --------------
    def shutdown(self) -> None:
        pass

    def start(self) -> bool:
        self.restarts += 1
        return False

    def get_sources(self) -> dict[str, Any] | None:
        return None

    def start_preview(self) -> None:
        return None


@pytest.fixture
def window(qtbot: Any, tmp_path: Path, qapp: QCoreApplication) -> Iterator[MainWindow]:
    """A real `MainWindow` with its engine replaced, and the theme applied.

    The stylesheet is not decoration here. M9_6_CHANGES.md §5 lesson 2: a fixture
    without it reproduces none of the theme's interactions with these widgets, and
    BUG-051's regression test passed with the fix reverted until the fixture was fixed.
    """
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())

    win = MainWindow(tmp_path / "no-engine.exe", tmp_path / "recordings")
    qtbot.addWidget(win)
    # The window defers its engine start by a zero-timer; letting it run would spawn a
    # process. Replacing the controller first is what keeps this test hardware-free.
    win._engine = _RecordingEngine()  # type: ignore[assignment]
    yield win
    win._toasts.clear()
    win._pill.close()


@pytest.fixture
def copied(window: MainWindow, monkeypatch: pytest.MonkeyPatch) -> list[str]:
    """What the window put on the clipboard, without touching the real one.

    BUG-055: the Windows clipboard is a machine-wide resource behind a lock. Reading it
    back in a test makes the test depend on every other process on the machine, and the
    four tests that did so failed together in one full-suite run and passed in the next.
    A test that fails for a reason outside the code it tests is worse than no test.

    What is worth asserting is the *text* the window produced; that `QClipboard.setText`
    then works is Qt's contract, not this project's.
    """
    captured: list[str] = []
    monkeypatch.setattr(window, "_set_clipboard", captured.append)
    return captured


def _menus(win: MainWindow) -> list[QMenu]:
    """Every menu the window owns, from its own registry -- **not** `QAction.menu()`.

    BUG-054: `QAction.menu()` hands back a wrapper PySide treats as owning the menu, and
    discarding it deletes the C++ object even while the window still holds a reference to
    it. `[a.menu() for a in bar.actions() if a.menu() is not None]` -- the obvious way to
    write this -- destroys every menu in the bar and its submenus with it. So the
    traversal goes through `MainWindow._menus`, which is what `_add_menu` maintains.

    The cross-check against `menuBar()` is what stops that being a way of hiding a menu
    from this file: a menu added with a bare `addMenu` appears on the bar and not in the
    registry, and this fails.
    """
    registry = {menu.title(): menu for menu in win._menus}
    on_the_bar = [action.text() for action in win.menuBar().actions() if not action.isSeparator()]
    unregistered = [title for title in on_the_bar if title not in registry]
    assert not unregistered, f"menus on the bar but not registered with _add_menu (BUG-054): {unregistered}"
    return [registry[title] for title in on_the_bar]


def _all_actions(win: MainWindow) -> list[tuple[str, QAction]]:
    """Every action in every menu, as `(menu title, action)`.

    Submenu openers are skipped: `QMenu.menuAction()` names them without going through
    `QAction.menu()`, and their contents arrive anyway because `_menus` holds the
    submenus too.
    """
    openers = [menu.menuAction() for menu in win._menus]
    found: list[tuple[str, QAction]] = []
    for menu in win._menus:
        for action in menu.actions():
            if action.isSeparator() or any(action is opener for opener in openers):
                continue
            found.append((menu.title(), action))
    return found


# ---------------------------------------------------------------------------
# The sweep -- the test that catches the next empty menu
# ---------------------------------------------------------------------------


def test_the_menu_bar_has_the_five_menus_the_spec_draws(window: MainWindow) -> None:
    """§16.2's menu bar, verbatim: File, Edit, View, Tools, Help."""
    titles = [action.text().replace("&", "") for action in window.menuBar().actions()]
    assert titles == ["File", "Edit", "View", "Tools", "Help"]


def test_every_menu_is_registered_with_the_window(window: MainWindow) -> None:
    """BUG-054, as an assertion rather than a comment.

    A menu created with a bare `bar.addMenu(...)` has no owner on this side, and the
    symptom is not a crash: it is a menu that opens empty, which is exactly the defect
    Phase 4 existed to remove. `_menus` raises with the offending titles.
    """
    assert len(_menus(window)) == 5
    # The submenus are registered too, and they are what `_all_actions` walks.
    titles = {menu.title() for menu in window._menus}
    assert {"&Panels", "Log &level", "&Engine"} <= titles


def test_no_menu_opens_empty(window: MainWindow) -> None:
    """The defect this phase existed to fix, asserted so it cannot come back."""
    for menu in _menus(window):
        assert menu.actions(), f"{menu.title()} opens empty"


def test_every_action_is_enabled_and_none_is_a_placeholder(window: MainWindow) -> None:
    """CLAUDE.md §7: nothing is stubbed, so nothing is greyed out to stand for a stub.

    Also catches the "(soon)" / "(not implemented)" label, which is the other way a
    placeholder gets shipped.
    """
    actions = _all_actions(window)
    # Guard against the sweep walking nothing, which would make this vacuous -- exactly
    # what happens if `_add_menu` stops registering (BUG-054).
    assert len(actions) >= 18
    for title, action in actions:
        assert action.isEnabled(), f"{title} - {action.text()} is disabled"
        lowered = action.text().lower()
        for banned in ("soon", "todo", "not implemented", "unavailable", "n/a"):
            assert banned not in lowered, f"{action.text()} reads as a placeholder"


def test_every_action_triggers_without_raising(window: MainWindow, monkeypatch: pytest.MonkeyPatch) -> None:
    """The plan's `test_menu_actions`: every action, triggered, no exception.

    Modals are redirected rather than skipped. Skipping them would leave the actions
    that matter most -- the bundle, the recovery, the topology -- untriggered, which is
    the same hole this test was written to close.
    """
    _silence_modals(monkeypatch, window)
    monkeypatch.setattr(window, "close", lambda: None)

    triggered = 0
    for _title, action in _all_actions(window):
        action.trigger()
        triggered += 1
    # A guard against the sweep silently walking nothing, which would make every
    # assertion above vacuous.
    assert triggered >= 18, f"only {triggered} actions were reachable"


def _silence_modals(monkeypatch: pytest.MonkeyPatch, window: MainWindow) -> None:
    """Redirect everything that would block a headless run."""
    from PySide6.QtWidgets import QFileDialog, QMessageBox

    monkeypatch.setattr(QMessageBox, "information", staticmethod(lambda *a, **k: QMessageBox.StandardButton.Ok))
    monkeypatch.setattr(QMessageBox, "warning", staticmethod(lambda *a, **k: QMessageBox.StandardButton.Ok))
    monkeypatch.setattr(QMessageBox, "about", staticmethod(lambda *a, **k: None))
    monkeypatch.setattr(QMessageBox, "question", staticmethod(lambda *a, **k: QMessageBox.StandardButton.No))
    monkeypatch.setattr(QFileDialog, "getSaveFileName", staticmethod(lambda *a, **k: ("", "")))
    monkeypatch.setattr("framecapture_gui.main_window.QDesktopServices.openUrl", staticmethod(lambda _url: True))
    monkeypatch.setattr("framecapture_gui.widgets.topology_dialog.GpuTopologyDialog.exec", lambda self: 0)
    monkeypatch.setattr(window, "_show_settings", lambda: None)


# ---------------------------------------------------------------------------
# Edit
# ---------------------------------------------------------------------------


def test_settings_carries_the_shortcut_the_plan_names(window: MainWindow) -> None:
    settings = next(action for title, action in _all_actions(window) if "Settings" in action.text())
    assert settings.shortcut().toString() == "Ctrl+,"


def test_copy_output_path_names_the_folder_when_nothing_has_been_recorded(
    window: MainWindow, copied: list[str]
) -> None:
    """An action that always does something beats one that greys out."""
    window._refresh_edit_menu()
    assert "output folder" in window._copy_path_action.text()

    window._copy_output_path()
    assert copied == [str(window._output_directory)]


def test_copy_output_path_switches_to_the_recording_once_there_is_one(window: MainWindow, copied: list[str]) -> None:
    """The label states what will be copied, so the action never lies about it."""
    recording = Path("D:/recordings/FrameCapture_2026-09-06.mkv")
    # `setattr` rather than assignment: on the real `EngineController` `last_output` is a
    # read-only property, and mypy checks this against that type rather than the stub's.
    setattr(window._engine, "last_output", recording)  # noqa: B010
    window._refresh_edit_menu()
    assert "FrameCapture_2026-09-06.mkv" in window._copy_path_action.text()

    window._copy_output_path()
    # `str(Path(...))`, not the literal: what reaches the clipboard is the platform's
    # spelling, and a user pasting it into Explorer needs backslashes.
    assert copied == [str(recording)]


def test_the_diagnostics_summary_reaches_the_clipboard_with_the_facts_in_it(
    window: MainWindow, copied: list[str]
) -> None:
    window._last_error = "the encoder rejected the frame size"
    window._copy_diagnostics()

    assert len(copied) == 1
    text = copied[0]
    assert "engine 0.1.0" in text
    assert "AMD Radeon 780M Graphics" in text
    assert "owns the display output" in text
    assert "the encoder rejected the frame size" in text
    # No line may begin with a bare colon: an indented continuation went through the
    # label formatter and rendered as "  : stalls 0 …". Found by reading the output.
    for line in text.splitlines():
        assert not line.strip().startswith(":"), f"malformed line: {line!r}"


def test_the_diagnostics_summary_still_renders_with_the_engine_down(window: MainWindow, copied: list[str]) -> None:
    """The state it is most wanted in. A summary that needs a live engine is absent."""
    window._engine.online = False  # type: ignore[attr-defined]
    window._copy_diagnostics()

    assert len(copied) == 1
    text = copied[0]
    assert "FrameCapture diagnostics" in text
    assert "unavailable" in text


# ---------------------------------------------------------------------------
# View
# ---------------------------------------------------------------------------

_PANEL_TOGGLES = [
    ("show_sources", "_sources"),
    ("show_audio_mixer", "_mixer"),
    ("show_controls", "_controls"),
    ("show_status", "_status"),
    ("show_preview", "_preview"),
]


@pytest.mark.parametrize(("key", "attribute"), _PANEL_TOGGLES)
def test_a_view_toggle_hides_its_widget_and_persists_the_choice(window: MainWindow, key: str, attribute: str) -> None:
    """Both halves. Hiding without persisting is a layout that resets every launch."""
    window.show()
    widget = getattr(window, attribute)
    assert widget.isVisible()

    window._view_actions[key].setChecked(False)
    assert not widget.isVisible()
    assert {key: False} in window._engine.saved  # type: ignore[attr-defined]

    window._view_actions[key].setChecked(True)
    assert widget.isVisible()
    assert {key: True} in window._engine.saved  # type: ignore[attr-defined]


def test_adopting_the_config_applies_the_toggles_without_writing_them_back(window: MainWindow) -> None:
    """The write-back loop this would otherwise have.

    `toggled` is what persists, so setting a toggle from the file has to block signals.
    Without that, every launch writes back every key it just read -- and the regression
    is invisible unless the saves are counted.
    """
    window.show()
    saved_before = len(window._engine.saved)  # type: ignore[attr-defined]

    window._apply_window_config({**_CONFIG, "show_status": False, "always_on_top": True})

    assert not window._status.isVisible()
    assert window._view_actions["always_on_top"].isChecked()
    assert len(window._engine.saved) == saved_before, "adopting the config wrote it back"  # type: ignore[attr-defined]


def test_always_on_top_sets_the_flag_and_leaves_the_window_visible(window: MainWindow) -> None:
    """The reshow. Changing a window flag hides the window; forgetting to show it again
    makes the application vanish with no way back."""
    from PySide6.QtCore import Qt

    window.show()
    window._view_actions["always_on_top"].setChecked(True)

    assert bool(window.windowFlags() & Qt.WindowType.WindowStaysOnTopHint)
    assert window.isVisible()

    window._view_actions["always_on_top"].setChecked(False)
    assert not bool(window.windowFlags() & Qt.WindowType.WindowStaysOnTopHint)
    assert window.isVisible()


def test_reset_layout_restores_every_panel_in_one_save(window: MainWindow) -> None:
    window.show()
    for key in ("show_sources", "show_status", "show_preview"):
        window._view_actions[key].setChecked(False)
    window._view_actions["always_on_top"].setChecked(True)

    window._engine.saved.clear()  # type: ignore[attr-defined]
    window._reset_layout()

    assert window._sources.isVisible()
    assert window._status.isVisible()
    assert window._preview.isVisible()
    assert not bool(window._view_actions["always_on_top"].isChecked())
    # One save, not six: six round trips to rewrite one file is six chances to be
    # interrupted halfway through a layout.
    assert len(window._engine.saved) == 1  # type: ignore[attr-defined]


def test_reset_layout_leaves_the_notifications_alone(window: MainWindow) -> None:
    """Resetting the *layout* is not consent to turn the toasts back on."""
    window._view_actions["toasts_enabled"].setChecked(False)
    window._reset_layout()
    assert not window._view_actions["toasts_enabled"].isChecked()


def test_turning_the_pill_off_hides_it(window: MainWindow) -> None:
    window._view_actions["pill_enabled"].setChecked(False)
    assert not window._pill_enabled
    assert not window._pill.isVisible()


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------


def test_the_log_level_menu_offers_the_levels_the_engine_parses(window: MainWindow) -> None:
    """§18's levels. `off` is deliberately not among them."""
    assert list(window._log_level_actions) == list(LOG_LEVELS)
    assert "off" not in window._log_level_actions


def test_choosing_a_log_level_sends_it_and_says_it_is_for_this_session(window: MainWindow) -> None:
    window._log_level_actions["debug"].trigger()

    assert window._engine.log_levels == ["debug"]  # type: ignore[attr-defined]
    message = window.statusBar().currentMessage()
    assert "DEBUG" in message
    # The part that stops it being a surprise twice over.
    assert "session" in message.lower()


def test_the_log_level_is_ticked_from_launch_not_from_the_first_opening(window: MainWindow) -> None:
    """Found by rendering the menu and looking at it: nothing was selected until
    `aboutToShow` had fired once, so the first opening showed six unticked levels."""
    window._adopt_saved_config()
    assert window._log_level_actions["info"].isChecked()


def test_the_log_level_menu_ticks_what_is_in_force(window: MainWindow) -> None:
    """Not what is configured. `set_log_level` changes the process and persists nothing,
    so a menu showing the file's value would be wrong the moment it is used."""
    window._refresh_log_level_menu()
    assert window._log_level_actions["info"].isChecked(), "the configured level should show first"

    window._log_level_actions["trace"].trigger()
    window._refresh_log_level_menu()
    assert window._log_level_actions["trace"].isChecked()


def test_a_refused_log_level_does_not_leave_the_menu_claiming_it(window: MainWindow) -> None:
    window._engine.online = False  # type: ignore[attr-defined]
    window._log_level_actions["critical"].trigger()
    # Falls back to the configured level -- and with the engine down there is no config
    # either, so the default. What matters is that CRITICAL is not left ticked.
    assert not window._log_level_actions["critical"].isChecked()


def test_the_topology_dialog_shows_which_adapter_owns_the_output(qtbot: Any) -> None:
    """SPEC.md §5.1's one diagnostic fact, rendered."""
    from framecapture_gui.widgets.topology_dialog import GpuTopologyDialog

    dialog = GpuTopologyDialog(_TOPOLOGY)
    qtbot.addWidget(dialog)

    assert dialog._table.rowCount() == 2

    def cell(row: int, column: int) -> str:
        item = dialog._table.item(row, column)
        assert item is not None, f"no item at {row},{column}"
        return item.text()

    assert cell(0, 0) == "AMD Radeon 780M Graphics"
    assert cell(0, 2) == "owns it"
    assert cell(1, 2) == "no"


def test_the_topology_dialog_says_so_when_the_engine_did_not_answer(qtbot: Any) -> None:
    from framecapture_gui.widgets.topology_dialog import GpuTopologyDialog

    dialog = GpuTopologyDialog(None)
    qtbot.addWidget(dialog)

    assert dialog._table.rowCount() == 0
    assert not dialog._table.isVisible()
    assert "did not answer" in dialog._summary.text()


def test_no_adapter_owning_an_output_is_called_out_as_the_black_frame_condition(qtbot: Any) -> None:
    """§5.1's failure, seen from the GUI. Worth naming rather than showing a bare table."""
    from framecapture_gui.widgets.topology_dialog import GpuTopologyDialog

    dialog = GpuTopologyDialog({"adapters": [{"luid": 1, "description": "Some GPU", "owns_output": False}]})
    qtbot.addWidget(dialog)
    assert "black frames" in dialog._summary.text()


# ---------------------------------------------------------------------------
# Recovery (SPEC.md §10.4)
# ---------------------------------------------------------------------------


def _sidecar(directory: Path, name: str, started: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    recording = directory / name
    recording.write_bytes(b"not really a recording")
    sidecar = directory / f"{name}{recovery.RECOVERY_SUFFIX}"
    # Written the way `write_recovery_record` writes it: JSON, with the output as a
    # generic path. `json.dumps` rather than a literal so a Windows path cannot smuggle
    # an unescaped backslash into the document and make the test's own fixture invalid.
    sidecar.write_text(
        json.dumps({"schema": 1, "container": "mkv", "output": recording.as_posix(), "started_utc": started}),
        encoding="utf-8",
    )
    return sidecar


def test_recovery_says_so_plainly_when_there_is_nothing_to_recover(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The common case, and it must not look like a failure."""
    shown: list[str] = []
    from PySide6.QtWidgets import QMessageBox

    def _record(_parent: object, _title: str, text: str) -> QMessageBox.StandardButton:
        shown.append(text)
        return QMessageBox.StandardButton.Ok

    monkeypatch.setattr(QMessageBox, "information", staticmethod(_record))
    window._output_directory.mkdir(parents=True, exist_ok=True)
    window._recover_unfinished()

    assert shown and "No unfinished recordings" in shown[0]
    assert window._engine.recovered == []  # type: ignore[attr-defined]


def test_recovery_asks_before_touching_anything(window: MainWindow, monkeypatch: pytest.MonkeyPatch) -> None:
    """§16.5: a surprising action requires explicit confirmation. Declining does nothing."""
    _sidecar(window._output_directory, "one.mkv", "2026-09-06T10:00:00Z")
    monkeypatch.setattr(window, "_confirm", lambda *_: False)

    window._recover_unfinished()
    assert window._engine.recovered == []  # type: ignore[attr-defined]


def test_recovery_works_through_the_files_one_at_a_time(window: MainWindow, monkeypatch: pytest.MonkeyPatch) -> None:
    """Serialised, not queued in a batch.

    Each `recover` is a full remux and the async backlog is bounded at 8 with a
    drop-newest policy (CLAUDE.md hard rule 5) -- so firing them all at once would drop
    a user's ninth unfinished recording silently.
    """
    first = _sidecar(window._output_directory, "one.mkv", "2026-09-06T10:00:00Z")
    second = _sidecar(window._output_directory, "two.mkv", "2026-09-06T11:00:00Z")
    monkeypatch.setattr(window, "_confirm", lambda *_: True)
    _silence_modals(monkeypatch, window)

    window._recover_unfinished()
    assert window._engine.recovered == [first], "both were queued at once"  # type: ignore[attr-defined]

    window._on_recovery_finished({"valid": True, "repaired": True, "output": "one.mkv"})
    assert window._engine.recovered == [first, second]  # type: ignore[attr-defined]

    window._on_recovery_finished({"valid": True, "repaired": False, "output": "two.mkv"})
    assert "2 recording(s) recovered" in window.statusBar().currentMessage()


def test_a_recording_that_cannot_be_repaired_is_named_rather_than_swallowed(
    window: MainWindow, monkeypatch: pytest.MonkeyPatch
) -> None:
    _sidecar(window._output_directory, "broken.mkv", "2026-09-06T10:00:00Z")
    monkeypatch.setattr(window, "_confirm", lambda *_: True)

    reported: list[str] = []
    from PySide6.QtWidgets import QMessageBox

    def _record(_parent: object, _title: str, text: str) -> QMessageBox.StandardButton:
        reported.append(text)
        return QMessageBox.StandardButton.Ok

    monkeypatch.setattr(QMessageBox, "information", staticmethod(_record))

    window._recover_unfinished()
    window._on_recovery_finished({"valid": False, "detail": "the file was truncated mid-cluster"})

    assert reported
    assert "broken.mkv" in reported[-1]
    assert "truncated mid-cluster" in reported[-1]


# ---------------------------------------------------------------------------
# The two commands that had never been called (SPEC.md §15.1)
# ---------------------------------------------------------------------------


def test_recover_gets_the_finalization_timeout_not_the_default_one(window: MainWindow) -> None:
    """A 5 s budget would time out on every recording large enough to be worth
    recovering -- `recover` runs the same remux `stop_record` does (BUG-046: ~3.3 s of
    remux plus ~1 s of validate for 1.2 GB)."""
    assert timeout_for(Command.RECOVER) == timeout_for(Command.STOP_RECORD)
    assert timeout_for(Command.RECOVER) == 30.0
    assert timeout_for(Command.GET_STATS) == 5.0


# ---------------------------------------------------------------------------
# The diagnostic bundle (SPEC.md §18)
# ---------------------------------------------------------------------------


def test_the_export_action_writes_a_bundle_the_user_chose_the_name_of(
    window: MainWindow, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """End to end through the menu, because the sweep's stubbed file dialog cancels.

    Also the one place the "nothing is uploaded" property is worth asserting on: SPEC.md
    §18 says "Telemetry is local-only. No network transmission, ever", and what makes
    that true here is that the whole feature is `zipfile.write` to a path the user typed.
    """
    from PySide6.QtWidgets import QFileDialog, QMessageBox

    logs = tmp_path / "logs"
    logs.mkdir()
    (logs / "framecapture.log").write_text("a log line", encoding="utf-8")
    monkeypatch.setattr(diagnostics, "default_log_directory", lambda: logs)
    monkeypatch.setattr(diagnostics, "default_crash_directory", lambda: tmp_path / "no-crashes")

    destination = tmp_path / "bundle.zip"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", staticmethod(lambda *a, **k: (str(destination), "")))
    shown: list[str] = []

    def _record(_parent: object, _title: str, text: str) -> QMessageBox.StandardButton:
        shown.append(text)
        return QMessageBox.StandardButton.Ok

    monkeypatch.setattr(QMessageBox, "information", staticmethod(_record))

    window._export_bundle()

    assert destination.is_file()
    with zipfile.ZipFile(destination) as bundle:
        names = bundle.namelist()
        summary = bundle.read(diagnostics.SUMMARY_MEMBER).decode()
    assert "logs/framecapture.log" in names
    # The summary travels with the bundle: a pile of logs with no statement of what
    # machine produced them is most of the way to useless.
    assert "FrameCapture diagnostics" in summary
    assert "AMD Radeon 780M Graphics" in summary
    # And the dialog says what could not be included.
    assert shown and "crash dump" in shown[-1]


def test_the_bundle_carries_logs_config_and_the_last_crash(tmp_path: Path) -> None:
    logs = tmp_path / "logs"
    logs.mkdir()
    (logs / "framecapture.log").write_text("a log line", encoding="utf-8")
    (logs / "framecapture.1.log").write_text("an older log line", encoding="utf-8")

    config = tmp_path / "config.toml"
    config.write_text("[video]\nfps = 60\n", encoding="utf-8")

    crashes = tmp_path / "crashes"
    crashes.mkdir()
    (crashes / "crash_abc_20260803.dmp").write_bytes(b"MDMP")
    (crashes / "crash_report_abc_20260803.json").write_text("{}", encoding="utf-8")

    report = diagnostics.write_bundle(
        tmp_path / "bundle.zip",
        summary="a summary",
        log_directory=logs,
        config_path=config,
        crash_directory=crashes,
    )

    with zipfile.ZipFile(report.path) as bundle:
        names = set(bundle.namelist())
        assert bundle.read(diagnostics.SUMMARY_MEMBER).decode() == "a summary"
    assert "logs/framecapture.log" in names
    assert "logs/framecapture.1.log" in names
    assert "config/config.toml" in names
    # The report as well as the dump: a minidump without the state-machine position
    # beside it is missing what it was for.
    assert "crashes/crash_abc_20260803.dmp" in names
    assert "crashes/crash_report_abc_20260803.json" in names
    assert not report.missing


def test_only_the_newest_crash_goes_in(tmp_path: Path) -> None:
    """§18 says "last minidump". Every dump a machine ever produced is a different,
    much larger, thing to ask a user to upload."""
    import os
    import time

    crashes = tmp_path / "crashes"
    crashes.mkdir()
    old = crashes / "crash_old_20260101.dmp"
    old.write_bytes(b"MDMP")
    new = crashes / "crash_new_20260901.dmp"
    new.write_bytes(b"MDMP")
    # Explicit times rather than write order: the selection is by mtime, so the test has
    # to set mtimes or it is asserting on filesystem timestamp resolution.
    os.utime(old, (time.time() - 10_000, time.time() - 10_000))

    files = diagnostics.latest_crash_files(crashes)
    assert [p.name for p in files] == ["crash_new_20260901.dmp"]


def test_a_bundle_from_a_clean_machine_says_what_it_lacks(tmp_path: Path) -> None:
    """A quietly smaller zip is one the recipient finds out is useless after asking."""
    report = diagnostics.write_bundle(
        tmp_path / "bundle.zip",
        summary="a summary",
        log_directory=tmp_path / "no-logs",
        config_path=tmp_path / "no-config.toml",
        crash_directory=tmp_path / "no-crashes",
    )
    assert report.members == [diagnostics.SUMMARY_MEMBER]
    assert len(report.missing) == 3
    assert any("crash dump" in item for item in report.missing)


# ---------------------------------------------------------------------------
# The recovery scan (SPEC.md §10.4)
# ---------------------------------------------------------------------------


def test_the_scan_finds_sidecars_and_reads_their_records(tmp_path: Path) -> None:
    _sidecar(tmp_path, "second.mkv", "2026-09-06T11:00:00Z")
    _sidecar(tmp_path, "first.mkv", "2026-09-06T09:00:00Z")
    (tmp_path / "finished.mkv").write_bytes(b"a finished recording has no sidecar")

    found = recovery.find_unfinished(tmp_path)
    assert [entry.output.name for entry in found] == ["first.mkv", "second.mkv"]
    assert found[0].started_utc == "2026-09-06T09:00:00Z"
    assert "first.mkv" in found[0].label()


def test_an_unreadable_sidecar_is_still_offered_for_recovery(tmp_path: Path) -> None:
    """Dropping it would be the prime directive's one unacceptable outcome: an
    unfinished recording the GUI cannot describe is still an unfinished recording."""
    (tmp_path / "mystery.mkv").write_bytes(b"data")
    (tmp_path / f"mystery.mkv{recovery.RECOVERY_SUFFIX}").write_text("{ not json", encoding="utf-8")

    found = recovery.find_unfinished(tmp_path)
    assert len(found) == 1
    assert found[0].output.name == "mystery.mkv"


def test_a_sidecar_whose_recording_is_gone_says_so(tmp_path: Path) -> None:
    sidecar = tmp_path / f"deleted.mkv{recovery.RECOVERY_SUFFIX}"
    sidecar.write_text('{"output": "D:/gone/deleted.mkv", "started_utc": "2026-09-06T09:00:00Z"}', encoding="utf-8")

    entry = recovery.read_sidecar(sidecar)
    assert not entry.exists
    assert "missing" in entry.label()


def test_scanning_a_directory_that_does_not_exist_is_not_an_error(tmp_path: Path) -> None:
    assert recovery.find_unfinished(tmp_path / "never-created") == []
