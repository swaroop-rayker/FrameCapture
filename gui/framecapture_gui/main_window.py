"""The main window (SPEC.md §16.2, §16.5).

Layout is §16.2's, top to bottom: menu bar, preview surface, then four panels in a row --
Sources, Audio Mixer, Controls, Status.

This class owns the ``EngineController`` and nothing else owns anything. Every widget
below it is passive; every engine interaction goes through one object. That is what makes
"is the GUI honest about what the engine is doing" a question with one place to look.
"""

from __future__ import annotations

import logging
from datetime import datetime
from pathlib import Path
from typing import Any

from PySide6.QtCore import QTimer, Slot
from PySide6.QtGui import QAction, QCloseEvent
from PySide6.QtWidgets import (
    QHBoxLayout,
    QMainWindow,
    QMessageBox,
    QVBoxLayout,
    QWidget,
)

from .engine import EngineController
from .hotkeys import DEFAULT_BINDINGS, Hotkey, HotkeyManager
from .ipc.protocol import RecordingState
from .widgets.panels import AudioMixerPanel, ControlsPanel, PreviewSurface, SourcesPanel, StatusPanel

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

        self._build_menu()
        self._build_layout()
        self._connect_engine()
        self._install_hotkeys()

        # SPEC.md §16.1: the window must be usable before -- and without -- an engine.
        # It is shown in the OFFLINE state and the engine is started a beat later, so a
        # slow or failed start is something the user watches happen rather than a blank
        # screen that eventually becomes a window.
        QTimer.singleShot(0, self._start_engine)

    # -- construction -------------------------------------------------------

    def _build_menu(self) -> None:
        menu = self.menuBar()

        file_menu = menu.addMenu("&File")
        self._restart_action = QAction("&Restart engine", self)
        self._restart_action.triggered.connect(self._restart_engine)
        file_menu.addAction(self._restart_action)
        file_menu.addSeparator()
        quit_action = QAction("&Quit", self)
        quit_action.setShortcut("Ctrl+Q")
        quit_action.triggered.connect(self.close)
        file_menu.addAction(quit_action)

        menu.addMenu("&Edit")
        menu.addMenu("&View")
        menu.addMenu("&Tools")

        help_menu = menu.addMenu("&Help")
        about = QAction("&About FrameCapture", self)
        about.triggered.connect(self._show_about)
        help_menu.addAction(about)

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

    def _install_hotkeys(self) -> None:
        """SPEC.md §16.5's global hotkeys, and what happens when one is taken.

        A conflict is reported in the status bar rather than a dialog: the application
        works without the accelerator, and interrupting a launch over a keyboard
        shortcut is out of proportion to the problem.
        """
        self._hotkeys = HotkeyManager(self)
        conflicts = self._hotkeys.register(
            [
                Hotkey("Start/stop recording", DEFAULT_BINDINGS["start_stop"], self._toggle_recording),
                Hotkey("Pause/resume", DEFAULT_BINDINGS["pause_resume"], self._toggle_pause),
            ]
        )
        if conflicts:
            self.statusBar().showMessage(f"Hotkey unavailable — {conflicts[0]}", 12000)

    @Slot()
    def _toggle_recording(self) -> None:
        if self._engine.state in (RecordingState.RECORDING, RecordingState.PAUSED):
            self._stop_recording()
        elif self._engine.state is RecordingState.IDLE:
            self._start_recording()

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
        if self._output_directory_pinned:
            return
        config = self._engine.get_config()
        if not config:
            return
        saved = str(config.get("output_directory", ""))
        if saved:
            self._output_directory = Path(saved)
            _log.info("output directory from config: %s", self._output_directory)

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
        self._controls.apply_state(state)
        self._status.apply_state(state)

        if state is RecordingState.OFFLINE:
            self._sources.set_sources(None)
            # The section belongs to the engine, and the engine is gone.
            self._preview.set_channel(None, placeholder="Preview unavailable — the engine is offline")
            self.statusBar().showMessage("Engine offline — use File ▸ Restart engine")

    @Slot(dict)
    def _on_stats(self, stats: dict[str, Any]) -> None:
        self._status.apply_stats(stats)

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
            self.statusBar().showMessage(f"Error: {text}", 15000)
        else:
            _log.warning("engine: %s", text)
            self.statusBar().showMessage(text, 8000)

    @Slot(dict)
    def _on_finalized(self, report: dict[str, Any]) -> None:
        output = str(report.get("output", ""))
        name = Path(output).name if output else "the recording"
        if report.get("valid"):
            duration = float(report.get("duration_s", 0.0))
            self.statusBar().showMessage(f"Saved {name} — {duration:.1f}s", 10000)
        else:
            # Never silent. A file that did not validate is the one thing a user must
            # not discover later (§16.5, CLAUDE.md §1).
            QMessageBox.warning(
                self,
                "FrameCapture",
                f"{name} did not pass validation:\n\n{report.get('detail', 'no detail reported')}",
            )

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

        dialog = SettingsDialog(self._engine, self._output_directory, self)
        if dialog.exec():
            self._output_directory = dialog.output_directory()

    @Slot()
    def _show_logs(self) -> None:
        # SPEC.md §18 puts the engine's logs under %LOCALAPPDATA%. Pointing at the
        # directory is honest and costs nothing; an in-window log viewer that tailed a
        # file the engine rotates is a feature, not a button.
        import os

        log_dir = Path(os.environ.get("LOCALAPPDATA", "")) / "FrameCapture" / "logs"
        QMessageBox.information(self, "Logs", f"Engine logs are written to:\n\n{log_dir}")

    @Slot()
    def _show_about(self) -> None:
        from . import __version__

        QMessageBox.about(
            self,
            "About FrameCapture",
            f"FrameCapture {__version__}\n\nEngine capabilities: "
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
            self._engine.stop_recording()

        self._hotkeys.unregister_all()
        # Ahead of the shutdown, so the mapping is released while the section it belongs to
        # is still the engine's to close.
        self._preview.set_channel(None)
        self._engine.shutdown()
        event.accept()
