"""The main window (SPEC.md §16.2, §16.5).

Layout is §16.2's, top to bottom: menu bar, preview surface, then four panels in a row --
Sources, Audio Mixer, Controls, Status.

This class owns the ``EngineController`` and nothing else owns anything. Every widget
below it is passive; every engine interaction goes through one object. That is what makes
"is the GUI honest about what the engine is doing" a question with one place to look.
"""

from __future__ import annotations

import logging
import platform
from collections.abc import Callable
from datetime import datetime
from pathlib import Path
from typing import Any

from PySide6.QtCore import QPoint, Qt, QTimer, QUrl, Slot
from PySide6.QtGui import QAction, QActionGroup, QCloseEvent, QDesktopServices, QGuiApplication
from PySide6.QtWidgets import (
    QFileDialog,
    QHBoxLayout,
    QMainWindow,
    QMenu,
    QMenuBar,
    QMessageBox,
    QVBoxLayout,
    QWidget,
)

from . import __version__, diagnostics, recovery
from .engine import EngineController
from .hotkeys import ACTION_LABELS, DEFAULT_BINDINGS, Action, Hotkey, HotkeyManager
from .ipc.protocol import RecordingState
from .overlay.exclusion import ExclusionSupport
from .overlay.pill import RecordingPill
from .overlay.placement import Corner
from .overlay.toast_model import Severity, ToastEntry
from .overlay.toasts import ToastManager
from .units import format_size
from .widgets.panels import AudioMixerPanel, ControlsPanel, PreviewSurface, SourcesPanel, StatusPanel
from .widgets.topology_dialog import GpuTopologyDialog

#: SPEC.md §18's levels, in severity order, as `set_log_level` spells them.
#:
#: `off` is deliberately absent. It is a level the engine accepts, and a menu item that
#: silently stops a user's machine producing any diagnostics at all is one they will find
#: again only by reading this source -- the plan's "trace…critical" says the same thing.
LOG_LEVELS = ("trace", "debug", "info", "warn", "error", "critical")

#: The View menu's persisted toggles: config key -> menu label.
#:
#: The key is the one `get_config`/`save_config` use, so what the menu shows and what the
#: engine stores cannot drift into two names for one setting.
_VIEW_TOGGLES = {
    "show_preview": "Show &preview",
    "pill_enabled": "Show recording &pill",
    "toasts_enabled": "Show &notifications",
    "show_sources": "&Sources",
    "show_audio_mixer": "&Audio Mixer",
    "show_controls": "&Controls",
    "show_status": "S&tatus",
    "always_on_top": "Always on &top",
}

#: What `_reset_layout` restores. The overlay keys are not in it: the pill and the toasts
#: are not layout, and a user who turned the notifications off did not ask for them back
#: because they wanted their panels rearranged.
_LAYOUT_DEFAULTS = {
    "show_preview": True,
    "show_sources": True,
    "show_audio_mixer": True,
    "show_controls": True,
    "show_status": True,
    "always_on_top": False,
}

_log = logging.getLogger(__name__)


class MainWindow(QMainWindow):
    def __init__(self, engine_path: Path, output_directory: Path, *, output_directory_pinned: bool = False) -> None:
        super().__init__()
        self.setWindowTitle("FrameCapture")
        self.resize(1100, 720)

        self._output_directory = output_directory
        #: True when the user passed `--output-dir`, which outranks the saved value.
        self._output_directory_pinned = output_directory_pinned
        self._engine = EngineController(engine_path, parent=self)

        # Overlay placement, replaced by `_adopt_overlay_config` once the engine can be
        # asked. Defaults here so the window is constructible with no engine (§16.1).
        self._pill_enabled = True
        self._pill_corner = Corner.BOTTOM_RIGHT
        self._pill_position: QPoint | None = None

        #: What the engine said last. A toast names a *transition* -- "resumed" is only
        #: meaningful against having been paused -- and `state_changed` carries only the
        #: destination.
        self._previous_state = RecordingState.OFFLINE

        #: SPEC.md §16.5's bindings, replaced by `_adopt_hotkey_config` once the engine
        #: can be asked. Defaults here so the window binds something with no engine.
        self._hotkeys_enabled = True
        self._hotkey_sequences: dict[Action, str] = dict(DEFAULT_BINDINGS)

        #: The last error the engine reported, for the diagnostics summary. A bug report
        #: written after the fact is written from memory unless something kept it.
        self._last_error = ""

        #: Toast column settings, so the View menu's "Show notifications" can flip the
        #: enabled flag without inventing values for the other three.
        self._toast_corner = Corner.TOP_RIGHT
        self._toast_max_visible = 4
        self._toast_duration_s = 4

        #: How each View toggle takes effect, keyed the same way the config is.
        self._view_appliers: dict[str, Callable[[bool], None]] = {}

        #: Every menu this window owns, top-level and sub. Held rather than discarded --
        #: see `_add_menu`.
        self._menus: list[QMenu] = []

        #: SPEC.md §10.4's recovery, worked through one file at a time.
        self._recovery_queue: list[recovery.Unfinished] = []
        self._recovery_results: list[tuple[recovery.Unfinished, dict[str, Any]]] = []

        # Layout and overlay before the menu: the View menu's toggles act on the panels,
        # the pill and the toast column, and an action whose target does not exist yet is
        # an action that has to be wired up somewhere else later.
        self._build_layout()
        self._build_overlay()
        self._build_menu()
        self._connect_engine()
        self._install_hotkeys()

        # SPEC.md §16.1: the window must be usable before -- and without -- an engine.
        # It is shown in the OFFLINE state and the engine is started a beat later, so a
        # slow or failed start is something the user watches happen rather than a blank
        # screen that eventually becomes a window.
        QTimer.singleShot(0, self._start_engine)

    # -- construction -------------------------------------------------------

    def _build_menu(self) -> None:
        """SPEC.md §16.2's menu bar: File, Edit, View, Tools, Help.

        **Every action does something.** Edit, View and Tools used to be three
        ``addMenu`` calls with no actions -- menus that open empty, which is worse than
        no menu at all because the user goes looking twice. Nothing here is a
        placeholder, nothing is greyed out to stand for a feature that does not exist,
        and where an action would have had to be a stub it was left out instead
        (CLAUDE.md §7).
        """
        bar = self.menuBar()

        file_menu = self._add_menu(bar, "&File")
        quit_action = QAction("&Quit", self)
        quit_action.setShortcut("Ctrl+Q")
        quit_action.triggered.connect(self.close)
        file_menu.addAction(quit_action)

        self._build_edit_menu(self._add_menu(bar, "&Edit"))
        self._build_view_menu(self._add_menu(bar, "&View"))
        self._build_tools_menu(self._add_menu(bar, "&Tools"))

        help_menu = self._add_menu(bar, "&Help")
        about = QAction("&About FrameCapture", self)
        about.triggered.connect(self._show_about)
        help_menu.addAction(about)

    def _add_menu(self, parent: QMenuBar | QMenu, title: str) -> QMenu:
        """Create a menu and **keep a Python reference to it** (BUG-054).

        `QMenuBar.addMenu(str)` hands back a menu that PySide treats as owned by Python
        despite Qt parenting it. With nothing on this side holding it, the menu survives
        only as long as some incidental wrapper does -- and `QAction.menu()` creating and
        discarding one is enough to take the C++ object with it. The failure is total and
        silent: the menu title stays on the bar and opens empty, which is
        indistinguishable from the defect this whole phase existed to fix.
        """
        menu = parent.addMenu(title)
        self._menus.append(menu)
        return menu

    # -- Edit ---------------------------------------------------------------

    def _build_edit_menu(self, edit: QMenu) -> None:
        """Settings, and the two things a user copies out of this application.

        No Undo and no Redo. There is nothing in a recorder to undo, and a greyed-out
        Undo at the top of an Edit menu is a claim that there is.
        """
        settings = QAction("&Settings…", self)
        settings.setShortcut("Ctrl+,")
        settings.triggered.connect(self._show_settings)
        edit.addAction(settings)
        edit.addSeparator()

        #: Its label changes with what there is to copy -- see `_refresh_edit_menu`.
        self._copy_path_action = QAction("Copy &output folder path", self)
        self._copy_path_action.triggered.connect(self._copy_output_path)
        edit.addAction(self._copy_path_action)

        copy_diagnostics = QAction("Copy &diagnostics summary", self)
        copy_diagnostics.triggered.connect(self._copy_diagnostics)
        edit.addAction(copy_diagnostics)

        edit.aboutToShow.connect(self._refresh_edit_menu)

    @Slot()
    def _refresh_edit_menu(self) -> None:
        """Say which path the copy action will copy, rather than disabling it.

        Two shapes were available: one action that greys out until a recording exists,
        or one that always works and states what it will do. The second, because the
        output *folder* is always a real answer to "where do my recordings go", and an
        action that greys out gives the user nothing at the moment they want to know.
        """
        latest = self._engine.last_output
        self._copy_path_action.setText(
            f"Copy path of &{latest.name}" if latest is not None else "Copy &output folder path"
        )

    def _set_clipboard(self, text: str) -> None:
        """The one place this application writes the clipboard.

        A seam rather than two call sites, because the Windows clipboard is a *machine-wide*
        resource behind a lock: `setText` can fail while another process holds it, and it
        fails without raising. One place to write it is one place to change if that ever
        needs handling -- and one place for a test to observe without the two copy actions
        contending for a real system resource between them (BUG-055).
        """
        QGuiApplication.clipboard().setText(text)

    @Slot()
    def _copy_output_path(self) -> None:
        latest = self._engine.last_output
        path = str(latest) if latest is not None else str(self._output_directory)
        self._set_clipboard(path)
        self.statusBar().showMessage(f"Copied {path}", 6000)

    @Slot()
    def _copy_diagnostics(self) -> None:
        """SPEC.md §18's diagnostics, as text, on the clipboard.

        Works with the engine offline, which is the state it is most often wanted in:
        `format_summary` renders every engine-supplied section as "unavailable" rather
        than refusing to produce anything.
        """
        self._set_clipboard(self._diagnostics_summary())
        self.statusBar().showMessage("Diagnostics summary copied to the clipboard", 6000)
        self._toasts.post(Severity.INFO, "Diagnostics summary copied", detail="Paste it into a bug report.")

    def _diagnostics_summary(self) -> str:
        """Gather what the engine will say, and render it. Never raises."""
        return diagnostics.format_summary(
            gui_version=__version__,
            engine_version=self._engine.engine_version or "unknown",
            protocol=self._engine.protocol or "unknown",
            state=self._engine.state.value,
            capabilities=self._engine.capabilities,
            platform=f"{platform.platform()} · Python {platform.python_version()}",
            config=self._engine.get_config(),
            topology=self._engine.get_gpu_topology(),
            health=self._engine.get_health(),
            last_error=self._last_error,
        )

    # -- View ---------------------------------------------------------------

    def _build_view_menu(self, view: QMenu) -> None:
        """What is on screen, and it persists (SPEC.md §17 owns the file).

        Every toggle writes its key through `save_config`, so a layout survives a
        restart. A layout that resets every launch is one a user stops adjusting.
        """
        #: config key -> the menu item, so `_apply_window_config` can set them all from
        #: what the engine reports without each one needing its own attribute.
        self._view_actions: dict[str, QAction] = {}

        view.addAction(self._view_toggle("show_preview", self._apply_show_preview))
        view.addAction(self._view_toggle("pill_enabled", self._apply_pill_enabled))
        view.addAction(self._view_toggle("toasts_enabled", self._apply_toasts_enabled))
        view.addSeparator()

        panels = self._add_menu(view, "&Panels")
        panels.addAction(self._view_toggle("show_sources", self._sources.setVisible))
        panels.addAction(self._view_toggle("show_audio_mixer", self._mixer.setVisible))
        panels.addAction(self._view_toggle("show_controls", self._controls.setVisible))
        panels.addAction(self._view_toggle("show_status", self._status.setVisible))
        view.addSeparator()

        view.addAction(self._view_toggle("always_on_top", self._apply_always_on_top))
        reset = QAction("&Reset layout", self)
        reset.triggered.connect(self._reset_layout)
        view.addAction(reset)

    def _view_toggle(self, key: str, apply: Callable[[bool], None]) -> QAction:
        """One checkable action that applies its effect *and* persists it."""
        action = QAction(_VIEW_TOGGLES[key], self)
        action.setCheckable(True)
        action.setChecked(_LAYOUT_DEFAULTS.get(key, True))
        action.toggled.connect(lambda checked, k=key: self._on_view_toggled(k, checked))
        self._view_actions[key] = action
        self._view_appliers[key] = apply
        return action

    def _on_view_toggled(self, key: str, checked: bool) -> None:
        self._view_appliers[key](checked)
        # The engine owns the file (§17). A save with no engine is refused and reported
        # by `EngineController`; the toggle still took effect for this session, which is
        # the honest outcome -- the window works with the engine down (§16.1).
        self._engine.save_config({key: checked})

    def _apply_show_preview(self, shown: bool) -> None:
        self._preview.setVisible(shown)

    def _apply_pill_enabled(self, enabled: bool) -> None:
        """The same switch `_adopt_overlay_config` sets, driven from the menu.

        Turning it on mid-recording *places* the pill rather than just showing it: it is
        placed on the way into a recording, and showing it from here without placing it
        would put it where it last was -- possibly on a monitor that no longer exists.
        """
        self._pill_enabled = enabled
        if not enabled:
            self._pill.hide()
        elif self._engine.state in (RecordingState.RECORDING, RecordingState.PAUSED):
            self._pill.place(self._pill_corner, self._pill_position)
            self._pill.apply_state(self._engine.state)

    def _apply_toasts_enabled(self, enabled: bool) -> None:
        self._toasts.configure(
            corner=self._toast_corner,
            max_visible=self._toast_max_visible,
            duration_s=self._toast_duration_s,
            enabled=enabled,
        )

    def _apply_always_on_top(self, on_top: bool) -> None:
        """Set the flag, then show the window again.

        Changing a window flag destroys and recreates the native window, and Qt hides it
        when it does. Without the reshow the application disappears from the screen with
        no way back. `isVisible` is checked first so that adopting the setting during
        construction -- before the window has ever been shown -- does not show it early.
        """
        was_visible = self.isVisible()
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, on_top)
        if was_visible:
            self.show()

    @Slot()
    def _reset_layout(self) -> None:
        """Everything in `_LAYOUT_DEFAULTS` back to §16.2's window, in one save."""
        for key, value in _LAYOUT_DEFAULTS.items():
            self._set_view_toggle(key, value)
        self._engine.save_config(dict(_LAYOUT_DEFAULTS))
        self.statusBar().showMessage("Layout reset", 4000)

    def _set_view_toggle(self, key: str, checked: bool) -> None:
        """Set a toggle and apply it **without** persisting it.

        Signals are blocked deliberately: `toggled` is what persists, and letting it
        fire here would write back the value that was just read out of the file -- once
        per key, on every launch, for settings nobody touched.
        """
        action = self._view_actions.get(key)
        if action is None:
            return
        was_blocked = action.blockSignals(True)
        action.setChecked(checked)
        action.blockSignals(was_blocked)
        self._view_appliers[key](checked)

    def _apply_window_config(self, config: dict[str, Any]) -> None:
        """Take the whole View menu's state from the engine's config."""
        for key in _VIEW_TOGGLES:
            self._set_view_toggle(key, bool(config.get(key, _LAYOUT_DEFAULTS.get(key, True))))

    # -- Tools --------------------------------------------------------------

    def _build_tools_menu(self, tools: QMenu) -> None:
        """The folders, the bundle, the topology, the log level, and the engine.

        Two of these -- Log level and Engine ▸ Recover -- are the first callers this
        project has had for SPEC.md §15.1's `set_log_level` and `recover`. Both commands
        have been implemented and tested for milestones and reachable from nothing.
        """
        output = QAction("Open &output folder", self)
        output.triggered.connect(self._open_output_folder)
        tools.addAction(output)

        logs = QAction("Open &logs folder", self)
        logs.triggered.connect(self._show_logs)
        tools.addAction(logs)

        tools.addSeparator()
        bundle = QAction("&Export diagnostic bundle…", self)
        bundle.triggered.connect(self._export_bundle)
        tools.addAction(bundle)

        topology = QAction("&GPU topology…", self)
        topology.triggered.connect(self._show_topology)
        tools.addAction(topology)

        tools.addSeparator()
        level_menu = self._add_menu(tools, "Log &level")
        self._log_level_group = QActionGroup(self)
        self._log_level_group.setExclusive(True)
        self._log_level_actions: dict[str, QAction] = {}
        for level in LOG_LEVELS:
            action = QAction(level.upper(), self)
            action.setCheckable(True)
            action.triggered.connect(lambda _checked=False, value=level: self._set_log_level(value))
            self._log_level_group.addAction(action)
            level_menu.addAction(action)
            self._log_level_actions[level] = action
        level_menu.aboutToShow.connect(self._refresh_log_level_menu)

        tools.addSeparator()
        engine_menu = self._add_menu(tools, "&Engine")
        self._restart_action = QAction("&Restart engine", self)
        self._restart_action.triggered.connect(self._restart_engine)
        engine_menu.addAction(self._restart_action)

        self._recover_action = QAction("Reco&ver unfinished recordings…", self)
        self._recover_action.triggered.connect(self._recover_unfinished)
        engine_menu.addAction(self._recover_action)

    @Slot()
    def _open_output_folder(self) -> None:
        self._output_directory.mkdir(parents=True, exist_ok=True)
        self._open_folder(self._output_directory)

    def _open_folder(self, folder: Path) -> None:
        """Open a folder in Explorer, or say plainly that it could not be opened."""
        if not folder.is_dir():
            QMessageBox.information(
                self,
                "FrameCapture",
                f"There is nothing there yet:\n\n{folder}\n\nIt is created when it is first written to.",
            )
            return
        if not QDesktopServices.openUrl(QUrl.fromLocalFile(str(folder))):
            QMessageBox.information(self, "FrameCapture", f"The folder could not be opened:\n\n{folder}")

    @Slot()
    def _export_bundle(self) -> None:
        """SPEC.md §18's diagnostic bundle: logs + config + the last minidump.

        The user picks the destination. §18 says "for manual sharing" and, one line
        above, "Telemetry is local-only. No network transmission, ever" -- so this
        writes a file and stops. Nothing here sends anything anywhere.
        """
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        suggested = self._output_directory / f"framecapture-diagnostics_{stamp}.zip"
        chosen, _selected_filter = QFileDialog.getSaveFileName(
            self, "Export diagnostic bundle", str(suggested), "Zip archives (*.zip)"
        )
        if not chosen:
            return

        config = self._engine.get_config() or {}
        config_path = str(config.get("config_path", ""))
        try:
            report = diagnostics.write_bundle(
                Path(chosen),
                summary=self._diagnostics_summary(),
                log_directory=diagnostics.default_log_directory(),
                config_path=Path(config_path) if config_path else None,
                crash_directory=diagnostics.default_crash_directory(),
            )
        except OSError as error:
            _log.exception("the diagnostic bundle could not be written")
            QMessageBox.warning(self, "FrameCapture", f"The bundle could not be written:\n\n{error}")
            return

        # What went in, and what did not. A bundle quietly missing the logs is one the
        # recipient discovers is useless only after asking for it.
        detail = f"{len(report.members)} files, {format_size(report.size_bytes)}"
        self.statusBar().showMessage(f"Diagnostic bundle written — {detail}", 8000)
        if report.missing:
            detail += "\n\nNot included: " + ", ".join(report.missing)
        QMessageBox.information(self, "FrameCapture", f"Diagnostic bundle written to:\n\n{chosen}\n\n{detail}")

    @Slot()
    def _show_topology(self) -> None:
        """SPEC.md §5.1's adapters, which the GUI has never shown."""
        GpuTopologyDialog(self._engine.get_gpu_topology(), self).exec()

    @Slot()
    def _refresh_log_level_menu(self) -> None:
        """Tick the level in force, which is not necessarily the configured one."""
        level = self._engine.log_level
        if not level:
            config = self._engine.get_config() or {}
            level = str(config.get("log_level", "info")).lower()
        action = self._log_level_actions.get(level)
        if action is not None:
            action.setChecked(True)

    def _set_log_level(self, level: str) -> None:
        """SPEC.md §15.1's `set_log_level` -- this run of the engine only.

        Said out loud in the status bar, because a change that does not persist and does
        not say so is one a user is surprised by twice: once when the logs go quiet after
        a restart, and once when they cannot find where they set it.
        """
        if self._engine.set_log_level(level):
            self.statusBar().showMessage(
                f"Engine log level set to {level.upper()} for this session — Settings ▸ Advanced is where it persists",
                8000,
            )
            return
        # `EngineController` has already reported why. Re-tick whatever is actually in
        # force, so the menu does not claim a level that was refused.
        self._refresh_log_level_menu()

    # -- recovery (SPEC.md §10.4) -------------------------------------------

    @Slot()
    def _recover_unfinished(self) -> None:
        """Finish recordings a crash left unfinished (SPEC.md §10.4).

        The scan is the GUI's -- §10.4 says the sidecar "lets the GUI's recovery path
        complete the job on next launch" -- and the repair is entirely the engine's.
        This side never opens a media file.

        Serialised deliberately: each `recover` is a full remux, the pipe server handles
        one request at a time, and the async backlog is bounded at 8 with a drop-newest
        policy. Queueing every sidecar at once would have a user's ninth unfinished
        recording dropped by a queue doing exactly what CLAUDE.md hard rule 5 asks of it.
        """
        if self._recovery_queue:
            QMessageBox.information(self, "FrameCapture", "A recovery is already running.")
            return

        unfinished = recovery.find_unfinished(self._output_directory)
        if not unfinished:
            QMessageBox.information(
                self,
                "FrameCapture",
                f"No unfinished recordings were found in:\n\n{self._output_directory}\n\n"
                "A recording has a .fcrecover file beside it only until it has been finalized.",
            )
            return

        listing = "\n".join(f"  - {entry.label()}" for entry in unfinished)
        if not self._confirm(
            "Recover unfinished recordings?",
            f"{len(unfinished)} unfinished recording(s) were found:\n\n{listing}\n\n"
            "Each will be repaired and validated. This can take a few seconds per file.",
        ):
            return

        self._recovery_queue = list(unfinished)
        self._recovery_results = []
        self.statusBar().showMessage(f"Recovering {len(self._recovery_queue)} recording(s)…")
        self._recover_next()

    def _recover_next(self) -> None:
        if not self._recovery_queue:
            self._report_recovery()
            return
        if not self._engine.recover(self._recovery_queue[0].sidecar):
            # Could not even be queued -- no engine, or the backlog is full. Reported by
            # `EngineController`; the queue is abandoned rather than spun on.
            self._recovery_queue.clear()
            self._report_recovery()

    @Slot(dict)
    def _on_recovery_finished(self, outcome: dict[str, Any]) -> None:
        if self._recovery_queue:
            self._recovery_results.append((self._recovery_queue.pop(0), outcome))
        self._recover_next()

    def _report_recovery(self) -> None:
        """One dialog at the end, not one per file."""
        if not self._recovery_results:
            self.statusBar().clearMessage()
            return

        recovered = [entry for entry, outcome in self._recovery_results if outcome.get("valid")]
        failed = [(entry, outcome) for entry, outcome in self._recovery_results if not outcome.get("valid")]
        self._recovery_results = []

        message = f"{len(recovered)} recording(s) recovered." if recovered else "No recordings could be recovered."
        if recovered:
            message += "\n\n" + "\n".join(f"  - {entry.output.name}" for entry in recovered)
        if failed:
            message += "\n\nCould not be recovered:\n" + "\n".join(
                f"  - {entry.output.name}: {outcome.get('detail') or 'no detail reported'}" for entry, outcome in failed
            )

        self.statusBar().showMessage(f"{len(recovered)} recording(s) recovered", 10000)
        self._toasts.post(
            Severity.INFO if recovered and not failed else Severity.WARNING,
            f"{len(recovered)} recording(s) recovered",
        )
        QMessageBox.information(self, "FrameCapture", message)

    def _build_layout(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)

        root = QVBoxLayout(central)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(10)

        self._preview = PreviewSurface()
        root.addWidget(self._preview, stretch=1)

        panels = QHBoxLayout()
        panels.setSpacing(10)

        self._sources = SourcesPanel()
        self._sources.refresh_requested.connect(self._refresh_sources)
        panels.addWidget(self._sources, stretch=2)

        self._mixer = AudioMixerPanel()
        panels.addWidget(self._mixer, stretch=2)

        self._controls = ControlsPanel()
        self._controls.start_requested.connect(self._start_recording)
        self._controls.stop_requested.connect(self._stop_recording)
        self._controls.pause_requested.connect(self._pause_recording)
        self._controls.resume_requested.connect(self._resume_recording)
        self._controls.settings_requested.connect(self._show_settings)
        self._controls.logs_requested.connect(self._show_logs)
        panels.addWidget(self._controls, stretch=2)

        self._status = StatusPanel()
        panels.addWidget(self._status, stretch=3)

        root.addLayout(panels)
        self.statusBar().showMessage("Starting engine…")

    def _build_overlay(self) -> None:
        """The floating recording pill (M9.6 F1) and the toast column (F2).

        Constructed once and shown per recording rather than created on demand: making
        the window is what applies the capture exclusion, and doing that at the moment
        recording starts would put a window creation and a display-affinity call on the
        path of the button press the user is waiting on.

        **Not parented to this window.** A child would be minimized, hidden and restored
        with it, and the whole point is that it stays on screen while the user is in a
        fullscreen game with FrameCapture's own window buried.
        """
        self._toasts = ToastManager(self)
        self._toasts.action_invoked.connect(self._on_toast_action)

        self._pill = RecordingPill()
        self._pill.pause_requested.connect(self._pause_recording)
        self._pill.resume_requested.connect(self._resume_recording)
        self._pill.stop_requested.connect(self._stop_recording)
        self._pill.moved.connect(self._on_pill_moved)

        if self._pill.exclusion is not ExclusionSupport.EXCLUDED:
            # Rule A's fallback, and it is not "show it anyway". The pill declines to
            # show itself (see `RecordingPill._showable`); this says so once, where the
            # user will see it, rather than leaving them wondering where the pill is.
            _log.warning("capture exclusion unavailable (%s); the recording pill is disabled", self._pill.exclusion)
            self.statusBar().showMessage(
                "The floating controls are unavailable on this system — they could not be hidden from the recording.",
                15000,
            )

    @Slot(QPoint)
    def _on_pill_moved(self, position: QPoint) -> None:
        """Persist where the user put it (SPEC.md §17: the engine owns the file)."""
        self._pill_position = position
        self._engine.save_config({"pill_x": position.x(), "pill_y": position.y()})

    def _install_hotkeys(self) -> None:
        """SPEC.md §16.5's global hotkeys (M9.6 F3).

        A conflict is reported in the status bar rather than a dialog: the application
        works without the accelerator, and interrupting a launch over a keyboard
        shortcut is out of proportion to the problem. It is *also* kept in the settings
        dialog, which is the part that was missing -- a twelve-second status message is
        gone long before the user next reaches for the key.
        """
        self._hotkeys = HotkeyManager(self)
        self._apply_hotkeys()

    def _hotkey_bindings(self) -> list[Hotkey]:
        """The three bindings, each with the state in which it can act.

        The predicates are what make a shared sequence a toggle. With `start` and `stop`
        on one combination there is one registration holding both, and `HotkeyManager`
        runs the first whose predicate says yes -- so the key starts a recording when
        there is none and stops one when there is, without the hotkey layer knowing what
        a recording is.
        """
        active = (RecordingState.RECORDING, RecordingState.PAUSED)
        return [
            Hotkey(
                Action.START,
                self._hotkey_sequences.get(Action.START, DEFAULT_BINDINGS[Action.START]),
                self._start_recording,
                applicable=lambda: self._engine.state is RecordingState.IDLE,
            ),
            Hotkey(
                Action.STOP,
                self._hotkey_sequences.get(Action.STOP, DEFAULT_BINDINGS[Action.STOP]),
                self._stop_recording,
                applicable=lambda: self._engine.state in active,
            ),
            Hotkey(
                Action.PAUSE_RESUME,
                self._hotkey_sequences.get(Action.PAUSE_RESUME, DEFAULT_BINDINGS[Action.PAUSE_RESUME]),
                self._toggle_pause,
                applicable=lambda: self._engine.state in active,
            ),
        ]

    def _apply_hotkeys(self) -> None:
        """(Re)bind, and report anything that did not take.

        Called at startup and again after the settings dialog saves, so a rebind takes
        effect without a restart -- which is what "rebindable" has to mean.
        """
        if not self._hotkeys_enabled:
            self._hotkeys.unregister_all()
            return

        statuses = self._hotkeys.apply(self._hotkey_bindings())
        unbound = [status for status in statuses.values() if not status.ok and status.requested]
        if not unbound:
            return

        first = unbound[0]
        message = f"{ACTION_LABELS[first.action]} ({first.requested}): {first.detail}"
        self.statusBar().showMessage(f"Hotkey unavailable — {message}", 12000)
        # And a toast, because the status bar's copy is gone in twelve seconds and this
        # is a setting the user has to open a dialog to fix.
        self._toasts.post(
            Severity.WARNING,
            "A keyboard shortcut is unavailable",
            detail=message,
        )

    @Slot()
    def _toggle_pause(self) -> None:
        if self._engine.state is RecordingState.PAUSED:
            self._resume_recording()
        elif self._engine.state is RecordingState.RECORDING:
            self._pause_recording()

    def _connect_engine(self) -> None:
        self._engine.state_changed.connect(self._on_state_changed)
        self._engine.stats_updated.connect(self._on_stats)
        self._engine.notified.connect(self._on_notified)
        self._engine.recording_finalized.connect(self._on_finalized)
        self._engine.finalize_progress.connect(self._on_finalize_progress)
        self._engine.recovery_finished.connect(self._on_recovery_finished)

    # -- engine lifecycle ---------------------------------------------------

    @Slot()
    def _start_engine(self) -> None:
        if self._engine.start():
            self.statusBar().showMessage("Engine ready", 4000)
            self._adopt_saved_config()
            self._refresh_sources()
            self._start_preview()
        else:
            # `notified` has already told the user why; the state is already OFFLINE.
            #
            # The surface is told explicitly rather than through `_on_state_changed`: the
            # state was *already* OFFLINE, so no transition is emitted and the placeholder
            # would keep whatever it said at construction. That is how the largest thing on
            # screen ends up reading "Preview is off" next to a failed engine start.
            self._preview.set_channel(None, placeholder="Preview unavailable — the engine is offline")
            self.statusBar().showMessage("Engine offline")

    def _start_preview(self) -> None:
        """Attach SPEC.md §15.2's preview, or say plainly that there isn't one.

        Started as soon as the engine is up rather than when recording begins: §16.2 puts
        the preview surface above the controls, and a preview you can only see once you are
        already recording is a preview you cannot frame a shot with.

        A failure is not reported through ``notified``. The surface itself is the honest
        signal -- it says what it is showing, or that it is showing nothing -- and an error
        toast for a feature the user did not ask for would be noise (§16.1's "honest", not
        "loud").
        """
        channel = self._engine.start_preview()
        if channel is None:
            self._preview.set_channel(None, placeholder="Preview unavailable — the engine reported no channel")
            return
        self._preview.set_channel(channel)

    @Slot()
    def _restart_engine(self) -> None:
        """SPEC.md §16.1's "plus a restart action"."""
        recording = self._engine.state in (RecordingState.RECORDING, RecordingState.PAUSED)
        if recording and not self._confirm(
            "Restart the engine?",
            "A recording is in progress. Restarting will stop and finalize it.",
        ):
            return
        # The surface lets go of the mapping before the engine that owns it goes away.
        self._preview.set_channel(None, placeholder="Restarting…")
        self._engine.shutdown()
        self._start_engine()

    def _adopt_saved_config(self) -> None:
        """Take the persisted output directory once the engine can tell us what it is.

        Only when the user did not pass ``--output-dir``: an explicit flag is a stronger
        statement than a saved preference, and silently overriding it would make the flag
        look broken.

        This is what makes persistence *visible*. A saved container or CQP is invisible
        until you record; a saved output directory is the setting a user notices
        immediately, and getting it from the file rather than the built-in default is the
        difference between "it remembered" and "it did not".
        """
        config = self._engine.get_config()
        if not config:
            return

        self._adopt_overlay_config(config)
        # Ticked from launch rather than only once the submenu has been opened:
        # `aboutToShow` keeps it fresh, but a menu whose first opening is also the first
        # time it decides what to tick shows nothing selected for that first frame.
        self._refresh_log_level_menu()

        if self._output_directory_pinned:
            return
        saved = str(config.get("output_directory", ""))
        if saved:
            self._output_directory = Path(saved)
            _log.info("output directory from config: %s", self._output_directory)

    def _adopt_overlay_config(self, config: dict[str, Any]) -> None:
        """Take the pill's placement and enablement from the engine's config (M9.6).

        `pill_x`/`pill_y` are -1 until the user has dragged it, which is why "unplaced"
        is a distinct value rather than 0,0 -- the top-left of the primary monitor is a
        real position somebody might have chosen.
        """
        self._pill_enabled = bool(config.get("pill_enabled", True))
        self._pill_corner = Corner.from_wire(str(config.get("pill_corner", "")), Corner.BOTTOM_RIGHT)

        x = int(config.get("pill_x", -1))
        y = int(config.get("pill_y", -1))
        self._pill_position = QPoint(x, y) if x >= 0 or y >= 0 else None

        self._toast_corner = Corner.from_wire(str(config.get("toast_corner", "")), Corner.TOP_RIGHT)
        self._toast_max_visible = int(config.get("toast_max_visible", 4))
        self._toast_duration_s = int(config.get("toast_duration_s", 4))
        self._toasts.configure(
            corner=self._toast_corner,
            max_visible=self._toast_max_visible,
            duration_s=self._toast_duration_s,
            enabled=bool(config.get("toasts_enabled", True)),
        )

        # The View menu, from the same file. After the toast settings above, because
        # `_apply_toasts_enabled` reconfigures the column from the three fields just set
        # -- doing it first would apply last launch's corner and count.
        self._apply_window_config(config)

        self._hotkeys_enabled = bool(config.get("hotkeys_enabled", True))
        self._hotkey_sequences = {
            action: str(config.get(f"hotkey_{action.value}", DEFAULT_BINDINGS[action]) or "") for action in Action
        }
        self._apply_hotkeys()

    @Slot()
    def _refresh_sources(self) -> None:
        self._sources.set_sources(self._engine.get_sources())

    # -- recording ----------------------------------------------------------

    def _output_path(self) -> Path:
        """A fresh path per recording, from SPEC.md §17's filename template shape.

        Timestamped rather than incremented: a counter has to be persisted, and a
        counter that resets silently overwrites a recording.

        The extension comes from the engine's configured container, not from here
        (BUG-043). It used to be the literal string ``.mkv``, so choosing MP4 in the
        settings dialog produced an MP4 file named ``.mkv`` -- the engine muxed what it
        was configured to mux and the GUI named it what it always had. Asking rather than
        assuming is the point: the engine owns the container setting, so the engine is
        what decides the extension.
        """
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        return self._output_directory / f"FrameCapture_{stamp}{self._container_suffix()}"

    def _container_suffix(self) -> str:
        """``.mp4`` or ``.mkv``, from the engine.

        Falls back to ``.mkv`` -- the engine's own default container -- when the engine
        cannot be asked, which is the offline case SPEC.md §16.1 requires the window to
        survive. The engine corrects the extension at ``start_record`` regardless, so a
        wrong guess here costs a misleading status line and not a misnamed file.
        """
        config = self._engine.get_config() or {}
        return ".mp4" if str(config.get("container", "mkv")).lower() == "mp4" else ".mkv"

    @Slot()
    def _start_recording(self) -> None:
        self._output_directory.mkdir(parents=True, exist_ok=True)
        output = self._output_path()
        if self._engine.start_recording(output, monitor=self._sources.selected_monitor()):
            # The engine returns the path it actually opened, which is authoritative --
            # it corrects the extension if this one disagreed with the container.
            actual = self._engine.last_output or output
            self.statusBar().showMessage(f"Recording to {actual.name}")

    @Slot()
    def _stop_recording(self) -> None:
        self.statusBar().showMessage("Finalizing…")
        self._engine.stop_recording()

    @Slot()
    def _pause_recording(self) -> None:
        self._engine.pause_recording()

    @Slot()
    def _resume_recording(self) -> None:
        self._engine.resume_recording()

    # -- engine signals -----------------------------------------------------

    @Slot(RecordingState)
    def _on_state_changed(self, state: RecordingState) -> None:
        previous, self._previous_state = self._previous_state, state
        self._controls.apply_state(state)
        self._status.apply_state(state)

        if self._pill_enabled:
            # Placed before it is shown, and re-placed on every transition into a
            # recording, so a monitor unplugged between two recordings cannot leave it
            # off-screen (`placement.clamp_to_screens`).
            if state is RecordingState.RECORDING and not self._pill.isVisible():
                self._pill.place(self._pill_corner, self._pill_position)
            self._pill.apply_state(state)
        elif self._pill.isVisible():
            self._pill.hide()

        self._toast_for_state(previous, state)

        if state is RecordingState.OFFLINE:
            self._sources.set_sources(None)
            # The section belongs to the engine, and the engine is gone.
            self._preview.set_channel(None, placeholder="Preview unavailable — the engine is offline")
            self.statusBar().showMessage("Engine offline — use File ▸ Restart engine")

    def _toast_for_state(self, previous: RecordingState, state: RecordingState) -> None:
        """Confirm the transitions a user acts on, and only those.

        Not every `state_changed` is worth a message. `STARTING` and `STOPPING` are
        already visible in the controls and the status panel, and a toast for each would
        turn a normal recording into four notifications. What earns one is a transition
        the user asked for and cannot otherwise confirm from a fullscreen game -- which
        is the whole reason this feature exists.

        Saving is deliberately absent here: it is reported by `_on_finalized`, which
        knows the filename and whether the file passed validation.
        """
        if previous is state:
            return

        if state is RecordingState.PAUSED:
            self._toasts.post(Severity.INFO, "Recording paused")
        elif state is RecordingState.RECORDING and previous is RecordingState.PAUSED:
            self._toasts.post(Severity.INFO, "Recording resumed")
        elif state is RecordingState.RECORDING:
            # Started. Worth saying because a hotkey pressed from a fullscreen game is
            # otherwise confirmed only by the pill appearing, and "did that work?" is
            # the question a global shortcut most needs answered.
            self._toasts.post(Severity.INFO, "Recording started")
        elif state is RecordingState.OFFLINE and previous in (
            RecordingState.RECORDING,
            RecordingState.PAUSED,
            RecordingState.STOPPING,
        ):
            # The engine died with a recording open. SPEC.md §18 writes a crash report
            # beside the logs; §10.4's recovery path owns the file itself.
            self._toasts.post(
                Severity.ERROR,
                "The recording engine stopped unexpectedly",
                detail="Your recording may have been recovered. The logs say what happened.",
                action="Show logs",
            )

    @Slot(object)
    def _on_toast_action(self, entry: ToastEntry) -> None:
        """A toast's action button. One action so far, and it opens the log folder."""
        if entry.action == "Show logs":
            self._show_logs()

    @Slot(dict)
    def _on_stats(self, stats: dict[str, Any]) -> None:
        self._status.apply_stats(stats)
        if self._pill_enabled and self._pill.isVisible():
            self._pill.apply_stats(stats)

    @Slot(dict)
    def _on_finalize_progress(self, progress: dict[str, Any]) -> None:
        """SPEC.md §10.4's stages, while they happen (M9.6 §2.1).

        Only the pill renders these. The status panel already says `FINALIZING…` and a
        second progress bar in the main window would be two things to keep in step for
        no gain -- the pill is the one on screen when the user is in a fullscreen game.
        """
        if self._pill_enabled and self._pill.isVisible():
            self._pill.apply_finalize_progress(progress)

    @Slot(str, str)
    def _on_notified(self, severity: str, text: str) -> None:
        """Surface an engine warning or error **without blocking**.

        Deliberately not a modal. Three reasons, and the first is a bug this had:

        * a modal opened from a signal that can arrive on any thread's behalf blocks the
          event loop, so the next engine event -- including the one saying the problem
          resolved -- cannot be delivered until a human clicks;
        * errors arrive in bursts. A failed start emits one, the state change to OFFLINE
          prompts a refresh that fails and emits another, and a modal per error stacks
          dialogs the user has to dismiss one at a time;
        * the honest signal is already on screen. ``ENGINE OFFLINE`` in the status panel
          and greyed controls (§16.1) say more than a dialog does, and they stay said.

        A modal is reserved for the two things that genuinely need an answer: closing
        mid-recording, and a file that failed validation.
        """
        if not text:
            return
        if severity == "error":
            _log.error("engine: %s", text)
            # Kept for the diagnostics summary. A user writes the bug report after the
            # error has scrolled out of the status bar, and "it said something about the
            # encoder" is not a defect report.
            self._last_error = text
            self.statusBar().showMessage(f"Error: {text}", 15000)
        else:
            _log.warning("engine: %s", text)
            self.statusBar().showMessage(text, 8000)

        # And a toast (M9.6 F2). Additive: the status bar keeps saying it, because the
        # toasts are absent entirely on a system where they could not be hidden from
        # the recording. Bursts are handled by `ToastQueue`'s coalescing rather than
        # here -- the second and third copies of one error become a counter, not three
        # windows.
        self._toasts.post(
            Severity.ERROR if severity == "error" else Severity.WARNING,
            text,
        )

    @Slot(dict)
    def _on_finalized(self, report: dict[str, Any]) -> None:
        output = str(report.get("output", ""))
        name = Path(output).name if output else "the recording"

        # Before the dialog below, which is modal and would otherwise leave the pill
        # showing a half-finished progress bar behind it until the user clicked.
        if self._pill_enabled and self._pill.isVisible():
            self._pill.apply_finalized(report)

        if report.get("valid"):
            duration = float(report.get("duration_s", 0.0))
            self.statusBar().showMessage(f"Saved {name} — {duration:.1f}s", 10000)
            self._toasts.post(Severity.INFO, f"Saved {name}", detail=f"{duration:.1f} seconds")
            return

        detail = str(report.get("detail", "")) or "no detail reported"
        self._toasts.post(Severity.ERROR, f"{name} did not pass validation", detail=detail)
        # Never silent, and still a modal despite the toast. A toast can be missed, and
        # this is the one message a user must not discover later (§16.5, CLAUDE.md §1) --
        # one of exactly two things this window interrupts for.
        QMessageBox.warning(self, "FrameCapture", f"{name} did not pass validation:\n\n{detail}")

    # -- dialogs ------------------------------------------------------------

    def _confirm(self, title: str, text: str) -> bool:
        """SPEC.md §16.5: "Every destructive or surprising action requires explicit
        confirmation.\""""
        answer = QMessageBox.question(
            self,
            title,
            text,
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        return answer == QMessageBox.StandardButton.Yes

    @Slot()
    def _show_settings(self) -> None:
        from .settings.dialog import SettingsDialog

        dialog = SettingsDialog(
            self._engine,
            self._output_directory,
            self,
            hotkey_statuses=self._hotkeys.statuses(),
        )
        if dialog.exec():
            self._output_directory = dialog.output_directory()
            # Re-read everything the dialog can have changed, then rebind. Without this a
            # new shortcut would not work until the next launch, which is not what
            # "rebindable" means (§16.5).
            self._adopt_saved_config()

    @Slot()
    def _show_logs(self) -> None:
        """Open SPEC.md §18's log folder.

        It used to show a dialog naming the path, which left the user to copy it into
        Explorer themselves -- the button said "Logs" and produced a sentence. Opening
        the folder is what both callers (the Logs button and Tools ▸ Open logs folder)
        were always asking for. `_open_folder` still falls back to naming the path when
        it cannot be opened, so nothing is lost on a machine where it fails.

        Still no in-window log viewer: tailing a file the engine rotates is a feature,
        not a button (CLAUDE.md §7).
        """
        self._open_folder(diagnostics.default_log_directory())

    @Slot()
    def _show_about(self) -> None:
        # Both versions, because they are two artefacts with two version numbers and
        # "which build?" is the first question any report about this application needs
        # answered. The engine's comes from the handshake (§15.1).
        engine = self._engine.engine_version or "not running"
        QMessageBox.about(
            self,
            "About FrameCapture",
            f"FrameCapture {__version__}\nEngine {engine}\n\nEngine capabilities: "
            + (", ".join(self._engine.capabilities) or "engine offline"),
        )

    # -- shutdown -----------------------------------------------------------

    def closeEvent(self, event: QCloseEvent) -> None:  # noqa: N802 -- Qt's spelling
        """Never lose a recording to a window close.

        SPEC.md §16.5 makes a surprising action require confirmation, and closing the
        window mid-recording is the most surprising one available. Confirmed, then
        finalized through the engine's own clean path rather than by dropping the job
        handle -- which is the difference between a saved file and one §10.4 has to
        repair.
        """
        if self._engine.state in (RecordingState.RECORDING, RecordingState.PAUSED):
            if not self._confirm(
                "Stop the recording?",
                "A recording is in progress. Closing will stop and finalize it.",
            ):
                event.ignore()
                return
            # The blocking form, deliberately. The window is closing and the engine is
            # about to be shut down, so returning before the file is finalized would
            # tear down the process that was finalizing it -- the one outcome the prime
            # directive does not permit. There is no responsiveness left to protect
            # here, which is exactly when blocking is the right call.
            self._engine.stop_recording_and_wait()

        self._hotkeys.unregister_all()
        # Same reason as the pill below: each toast is its own top-level window and
        # would outlive this one, keeping the process alive with nothing to reach it.
        self._toasts.clear()
        # The pill is a top-level window of its own, so it does not close with this one.
        # Left open, it would outlive the window and keep the process alive with no way
        # to reach it.
        self._pill.close()
        # Ahead of the shutdown, so the mapping is released while the section it belongs to
        # is still the engine's to close.
        self._preview.set_channel(None)
        self._engine.shutdown()
        event.accept()
