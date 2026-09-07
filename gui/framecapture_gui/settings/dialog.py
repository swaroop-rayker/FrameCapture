"""The settings dialog (SPEC.md §16.4).

Seven sections, in §16.4's order: General, Video, Audio, Recording, Advanced, Updates,
About.

**Settings persist across restarts.** The dialog loads the engine's current
configuration when it opens and writes it back with ``save_config`` on OK, so what a
user sets is what they get next launch.

**The engine writes the file, not this.** SPEC.md §17 requires an atomic replace, an
ordered migration chain and the preservation of keys this build does not recognise --
all of which ``engine/core/config/`` already implements and tests. Writing
``config.toml`` from Python would put two writers with two ideas of the schema on one
file, which is the failure §17 exists to prevent. So the GUI is still the single source
of truth for what the *settings are*, and the engine is the single writer of where they
live. ``get_config`` / ``save_config`` are the seam, and they are additions to §15.1's
command list that need the owner's pen.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, ClassVar

from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import __version__
from ..engine import EngineController
from ..hotkeys import Action, BindingStatus
from .hotkey_editor import HotkeysSection


def _select_data(combo: QComboBox, value: object) -> None:
    """Select the entry whose data equals ``value``; leave the box alone if none does.

    Left alone rather than reset to index 0: a config naming a value this build has no
    entry for is a *newer* config, and silently selecting the first option would both
    misreport it and overwrite it on the next save.
    """
    if value is None:
        return
    index = combo.findData(value)
    if index < 0 and isinstance(value, str):
        index = combo.findData(value.lower())
    if index >= 0:
        combo.setCurrentIndex(index)


class SettingsDialog(QDialog):
    def __init__(
        self,
        engine: EngineController,
        output_directory: Path,
        parent: QWidget | None = None,
        *,
        hotkey_statuses: dict[Action, BindingStatus] | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("Settings")
        self.setMinimumWidth(520)

        self._engine = engine
        self._output_directory = output_directory
        #: What the hotkey manager last reported. Empty when the dialog is opened
        #: without one, in which case the chips describe what *would* happen.
        self._hotkey_statuses = hotkey_statuses or {}

        # Read before the widgets are built, so each control is constructed showing what
        # is actually in force. Populating afterwards would flash the defaults first and,
        # worse, fire the `toggled` handlers -- including segmentation's confirmation
        # prompt, which would appear every time the dialog opened.
        self._current: dict[str, Any] = engine.get_config() or {}
        # One `get_devices` for the whole dialog: the audio tab needs the default
        # endpoint's channel count for §8.5's hint and the running processes for the
        # Tier B picker, and asking twice would be two snapshots of a machine that
        # changes between them.
        self._devices: dict[str, Any] = engine.get_devices() or {}
        if self._current.get("output_directory"):
            self._output_directory = Path(str(self._current["output_directory"]))

        layout = QVBoxLayout(self)
        self._tabs = QTabWidget()
        layout.addWidget(self._tabs)

        self._tabs.addTab(self._general_tab(), "General")
        self._tabs.addTab(self._video_tab(), "Video")
        self._tabs.addTab(self._audio_tab(), "Audio")
        self._tabs.addTab(self._recording_tab(), "Recording")
        self._tabs.addTab(self._advanced_tab(), "Advanced")
        self._tabs.addTab(self._hotkeys_tab(), "Hotkeys")
        self._tabs.addTab(self._updates_tab(), "Updates")
        self._tabs.addTab(self._about_tab(), "About")

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    # -- sections -----------------------------------------------------------

    #: Channels each layout asks for; ``auto`` follows the endpoint so it asks for none.
    #: Mirrors `channels_for` in `resampler.cpp` — the engine enforces the rule, this
    #: explains it, and both need the same table.
    _LAYOUT_CHANNELS: ClassVar[dict[str, int]] = {"auto": 0, "stereo": 2, "5.1": 6, "7.1": 8}

    def _default_endpoint_channels(self) -> int:
        """Channels the default render endpoint reports, or 0 when nothing is known.

        Read from the same `get_devices` snapshot the target picker uses. 0 means the
        engine is down or too old to say, and the hint then stays silent rather than
        guessing — a warning shown on no evidence is worse than none.
        """
        devices = self._devices or {}
        endpoints = devices.get("render_endpoints") or []
        for entry in endpoints:
            if entry.get("default"):
                return int(entry.get("channels", 0) or 0)
        # No endpoint flagged default: fall back to the first, which is what the engine
        # would open. A machine with endpoints but no default is unusual rather than
        # impossible — `enumerate_render_endpoints` says so too.
        if endpoints:
            return int(endpoints[0].get("channels", 0) or 0)
        return 0

    def _refresh_layout_hint(self) -> None:
        """Say so when the chosen layout is wider than the device can supply.

        SPEC.md §8.5's pin overrides the endpoint **downward only** (BUG-048): asking
        for more channels than the device has cannot add information, and it produced a
        file whose audio Windows' own player refused — its AAC decoder takes 1, 2 and 6
        channels and not 8.

        The engine clamps regardless; this is the explanation, not the enforcement. The
        same split SPEC.md §20 row 16 asks for on the MP4 block.
        """
        chosen = self._LAYOUT_CHANNELS.get(str(self._layout_combo.currentData()), 0)
        available = self._default_endpoint_channels()

        if chosen == 0 or available == 0 or chosen <= available:
            self._layout_hint.setText("")
            return

        self._layout_hint.setText(
            f"Your audio device has {available} channels, so this recording will use them. "
            f"Asking for more cannot add anything that is not already there, and an "
            f"8-channel track will not play in Windows' own player."
        )

    def _populate_process_picker(self) -> None:
        """Fill the picker from the engine's snapshot of running processes.

        One entry per **executable name**, which the engine has already reduced from
        the raw process list — a machine with forty ``chrome.exe`` processes offers one,
        and picking it means exactly what typing the name means. Doing that reduction
        here instead would be a second place deciding what a target *is* (BUG-043's
        shape).

        An engine that is down, or one too old to report the field, leaves the picker
        empty and disabled. The text field still works, which is the point of it being
        the source of truth.
        """
        processes = (self._devices or {}).get("processes") or []
        for entry in processes:
            name = str(entry.get("executable", "")).strip()
            if name:
                self._process_picker.addItem(name, name)

    def _on_add_target(self) -> None:
        """Append the picked application, without disturbing what is already typed.

        Appending rather than replacing is what lets the picker and the field compose:
        a user can name a game that is not running yet and then pick the browser that
        is. Duplicates are dropped here as well as in the engine, so the field shows
        what the recording will actually do.
        """
        chosen = str(self._process_picker.currentData() or "").strip()
        if not chosen:
            return
        existing = [part.strip() for part in self._multitrack_targets.text().split(",") if part.strip()]
        if any(part.lower() == chosen.lower() for part in existing):
            return
        existing.append(chosen)
        self._multitrack_targets.setText(", ".join(existing))

    def _general_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)

        row = QHBoxLayout()
        self._output_edit = QLineEdit(str(self._output_directory))
        row.addWidget(self._output_edit)
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse_output)
        row.addWidget(browse)
        container = QWidget()
        container.setLayout(row)
        form.addRow("Output directory", container)

        self._filename_template = QLineEdit(
            str(self._current.get("filename_template", "FrameCapture_%Y-%m-%d_%H-%M-%S"))
        )
        form.addRow("Filename template", self._filename_template)

        self._language = QComboBox()
        self._language.addItem("English", "en")
        form.addRow("Language", self._language)
        return page

    def _video_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)

        # §16.4: "resolution locked at 1080p". Shown rather than hidden, so a user
        # looking for it finds the answer instead of concluding it is missing.
        resolution = QLabel("1920 × 1080 (locked in v1)")
        form.addRow("Resolution", resolution)

        self._fps = QComboBox()
        self._fps.addItem("60", 60)
        self._fps.addItem("30", 30)
        _select_data(self._fps, self._current.get("fps"))
        form.addRow("Frame rate", self._fps)

        self._container = QComboBox()
        self._container.addItem("MKV (recommended)", "mkv")
        self._container.addItem("MP4", "mp4")
        _select_data(self._container, self._current.get("container"))
        form.addRow("Container", self._container)

        self._encoder = QComboBox()
        self._encoder.addItem("Automatic", "")
        self._encoder.addItem("NVENC (NVIDIA)", "h264_nvenc")
        self._encoder.addItem("AMF (AMD)", "h264_amf")
        self._encoder.addItem("x264 (software)", "libx264")
        form.addRow("Encoder", self._encoder)

        self._cqp = QSpinBox()
        self._cqp.setRange(0, 51)
        self._cqp.setValue(int(self._current.get("cqp", 20)))
        self._cqp.setToolTip("Constant quality. Lower is better quality and a larger file.")
        form.addRow("Quality (CQP)", self._cqp)
        return page

    def _audio_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)

        self._audio_device = QComboBox()
        self._audio_device.addItem("System default", "")
        form.addRow("Device", self._audio_device)

        self._layout_combo = QComboBox()
        for label, value in (("Automatic", "auto"), ("Stereo", "stereo"), ("5.1", "5.1"), ("7.1", "7.1")):
            self._layout_combo.addItem(label, value)
        _select_data(self._layout_combo, self._current.get("channel_layout"))
        form.addRow("Channel layout", self._layout_combo)

        # SPEC.md §8.5's override is downward only (BUG-048), and the engine enforces
        # that whatever this says. What this does is tell the user *before* they record
        # rather than leaving them to find it in a player afterwards — which is how the
        # defect was reported: an unplayable audio track and no indication why.
        self._layout_hint = QLabel()
        self._layout_hint.setObjectName("SectionHint")
        self._layout_hint.setWordWrap(True)
        form.addRow("", self._layout_hint)
        self._layout_combo.currentIndexChanged.connect(self._refresh_layout_hint)
        self._refresh_layout_hint()

        self._audio_bitrate = QSpinBox()
        self._audio_bitrate.setRange(0, 512)
        self._audio_bitrate.setValue(int(self._current.get("audio_bitrate_kbps", 0)))
        self._audio_bitrate.setSpecialValueText("Automatic")
        self._audio_bitrate.setSuffix(" kbps")
        form.addRow("Bitrate", self._audio_bitrate)

        # §16.4: "multi-track when MKV" — SPEC.md §8.6's Tier B, landed in M9.5.
        self._multitrack = QCheckBox("Per-application multi-track")
        self._multitrack.setChecked(bool(self._current.get("multitrack_enabled", False)))
        form.addRow("", self._multitrack)

        # A text field **and** a picker, not one or the other.
        #
        # The field is the source of truth, because SPEC.md §8.6 explicitly supports
        # naming "a target process that hasn't started yet" — a picker alone could not
        # express that, and it is the case a user setting up before launching a game is
        # in. The picker exists because a user should not have to know that Chrome is
        # `chrome.exe`, and it *appends* rather than replaces so the two compose.
        targets_row = QHBoxLayout()
        self._multitrack_targets = QLineEdit(str(self._current.get("multitrack_targets", "")))
        self._multitrack_targets.setPlaceholderText("chrome.exe, game.exe")
        self._multitrack_targets.setToolTip(
            "Executable names, comma-separated. Up to five, alongside the system mix.\n"
            "An application that is not running yet gets a silent track until it starts."
        )
        targets_row.addWidget(self._multitrack_targets)

        self._process_picker = QComboBox()
        self._process_picker.setToolTip("Applications running now")
        self._process_picker.setMinimumWidth(160)
        targets_row.addWidget(self._process_picker)

        self._add_target = QPushButton("Add")
        self._add_target.setToolTip("Append the selected application to the list")
        self._add_target.clicked.connect(self._on_add_target)
        targets_row.addWidget(self._add_target)

        targets_container = QWidget()
        targets_container.setLayout(targets_row)
        form.addRow("Applications", targets_container)
        self._populate_process_picker()

        # SPEC.md §8.6 leaves open whether a track keeps looking for a target that
        # exits. Decided 2026-08-06: it does, and this is where a user turns that off.
        self._multitrack_reattach = QCheckBox("Reconnect if an application is closed and reopened")
        self._multitrack_reattach.setChecked(bool(self._current.get("multitrack_reattach", True)))
        self._multitrack_reattach.setToolTip(
            "The track stays the full length of the recording either way.\n"
            "Off, an application that closes leaves its track silent for the rest of the recording."
        )
        form.addRow("", self._multitrack_reattach)

        # §20 row 16: "The GUI must explain inline rather than silently greying the
        # option out." Two separate reasons the control can be unavailable, and they
        # are *different* problems with different answers — one is a setting the user
        # can change on the Video tab, the other is the machine. A single greyed
        # checkbox would tell them neither.
        self._multitrack_hint = QLabel()
        self._multitrack_hint.setObjectName("SectionHint")
        self._multitrack_hint.setWordWrap(True)
        form.addRow("", self._multitrack_hint)

        # Re-evaluated whenever the container changes, because the block turns on it and
        # a user who switches to MP4 with multi-track ticked must be told before they
        # press OK rather than by an error afterwards.
        self._container.currentIndexChanged.connect(self._refresh_multitrack_state)
        self._multitrack.toggled.connect(self._refresh_multitrack_state)
        self._refresh_multitrack_state()
        return page

    def _refresh_multitrack_state(self) -> None:
        """Apply SPEC.md §20 row 16's inline reason, and §8.6's runtime probe.

        The engine refuses multi-track on MP4 with ``MULTITRACK_REQUIRES_MKV`` (3021)
        whether or not a GUI is involved, so this is the *explanation*, not the
        enforcement — which is exactly the split row 16 asks for: "enforced engine-side,
        not just in the GUI".
        """
        mkv = str(self._container.currentData()) == "mkv"
        # §8.6: "probe at runtime anyway, and degrade to Tier A with a GUI notice if
        # activation fails." Absent from an older engine's payload, which reads as
        # available — the honest default for a field a peer did not send (§15.1's
        # compatibility rule).
        available = bool(self._current.get("multitrack_available", True))

        self._multitrack.setEnabled(mkv and available)
        usable = mkv and available and self._multitrack.isChecked()
        self._multitrack_targets.setEnabled(usable)
        self._process_picker.setEnabled(usable and self._process_picker.count() > 0)
        self._add_target.setEnabled(usable and self._process_picker.count() > 0)
        self._multitrack_reattach.setEnabled(usable)

        if not mkv:
            self._multitrack_hint.setText(
                "Multi-track audio requires MKV. Most players show only the first track "
                "of a multi-track MP4, so the engine refuses the combination rather than "
                "writing a file whose extra tracks are invisible. Change the container on "
                "the Video tab to enable this."
            )
        elif not available:
            self._multitrack_hint.setText(
                "This system does not offer per-application audio capture, so only the "
                "system mix can be recorded. Everything else is unaffected."
            )
        elif self._multitrack.isChecked():
            self._multitrack_hint.setText(
                "Track 1 is always the full system mix. Each application named above gets "
                "its own track, silent whenever it is not playing anything."
            )
        else:
            self._multitrack_hint.setText("")

    def _recording_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        # CLAUDE.md hard rule 7 and SPEC.md §11: default false, always.
        self._segmentation = QCheckBox("Split the recording into segments")
        # CLAUDE.md hard rule 7 is about the *default*, not about refusing to remember a
        # user's explicit choice. The default in `config::Config` is false; this reflects
        # what is saved. Connected after the initial state is set, so restoring a saved
        # `true` does not re-prompt.
        self._segmentation.setChecked(bool(self._current.get("segmentation_enabled", False)))
        self._segmentation.toggled.connect(self._on_segmentation_toggled)
        layout.addWidget(self._segmentation)

        warning = QLabel(
            "Segmentation produces multiple files rather than one. "
            "Each is independently playable, but they are not joined for you."
        )
        warning.setObjectName("SectionHint")
        warning.setWordWrap(True)
        layout.addWidget(warning)

        form = QFormLayout()
        self._segment_minutes = QSpinBox()
        self._segment_minutes.setRange(1, 600)
        self._segment_minutes.setValue(int(self._current.get("segment_minutes", 30)))
        self._segment_minutes.setSuffix(" min")
        self._segment_minutes.setEnabled(self._segmentation.isChecked())
        form.addRow("Split every", self._segment_minutes)
        layout.addLayout(form)

        layout.addStretch(1)
        return page

    def _advanced_tab(self) -> QWidget:
        page = QWidget()
        form = QFormLayout(page)

        self._backend = QComboBox()
        for label, value in (
            ("Automatic", "auto"),
            ("Windows Graphics Capture", "wgc"),
            ("Desktop Duplication", "dda"),
        ):
            self._backend.addItem(label, value)
        _select_data(self._backend, self._current.get("capture_backend"))
        form.addRow("Capture backend", self._backend)

        self._cursor = QCheckBox("Capture the mouse cursor")
        self._cursor.setChecked(bool(self._current.get("capture_cursor", True)))
        form.addRow("", self._cursor)

        self._hdr = QCheckBox("Tone-map HDR sources")
        self._hdr.setChecked(bool(self._current.get("hdr_tonemap", True)))
        form.addRow("", self._hdr)

        self._log_level = QComboBox()
        for level in ("trace", "debug", "info", "warn", "error"):
            self._log_level.addItem(level.upper(), level)
        self._log_level.setCurrentText(str(self._current.get("log_level", "info")).upper())
        form.addRow("Log level", self._log_level)

        self._gpu_override = QLineEdit()
        self._gpu_override.setText(str(self._current.get("gpu_override", "")))
        self._gpu_override.setPlaceholderText("Automatic (SPEC.md §5.2 selection)")
        form.addRow("GPU override", self._gpu_override)
        return page

    def _hotkeys_tab(self) -> QWidget:
        """SPEC.md §16.4's sections, plus the one §16.5 asks for (M9.6 F3).

        Given the manager's last statuses so a binding that failed says why, right
        next to itself, for as long as it is true. That is the whole difference
        between a fixable complaint and "hotkeys don't work".
        """
        self._hotkeys = HotkeysSection()
        self._hotkeys.load(self._current, self._hotkey_statuses)
        return self._hotkeys

    def _updates_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        self._check_updates = QCheckBox("Check for updates automatically")
        self._check_updates.setChecked(bool(self._current.get("check_updates", True)))
        self._check_updates.setEnabled(False)
        layout.addWidget(self._check_updates)
        hint = QLabel("The updater ships with the installer (M10). This setting has no effect yet.")
        hint.setObjectName("SectionHint")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        layout.addStretch(1)
        return page

    def _about_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.addWidget(QLabel(f"FrameCapture {__version__}"))

        capabilities = ", ".join(self._engine.capabilities) or "engine offline"
        detail = QLabel(f"Engine capabilities: {capabilities}")
        detail.setObjectName("SectionHint")
        detail.setWordWrap(True)
        layout.addWidget(detail)

        licence = QLabel("Includes FFmpeg with libx264. The distributed binary is licensed under the GPL v2.")
        licence.setObjectName("SectionHint")
        licence.setWordWrap(True)
        layout.addWidget(licence)

        layout.addStretch(1)
        return page

    # -- behaviour ----------------------------------------------------------

    def _browse_output(self) -> None:
        chosen = QFileDialog.getExistingDirectory(self, "Output directory", self._output_edit.text())
        if chosen:
            self._output_edit.setText(chosen)

    def _on_segmentation_toggled(self, enabled: bool) -> None:
        """SPEC.md §16.5: enabling segmentation is a named "surprising action"."""
        self._segment_minutes.setEnabled(enabled)
        if not enabled:
            return

        answer = QMessageBox.question(
            self,
            "Enable segmentation?",
            "Segmentation writes multiple files instead of one, split on keyframe "
            "boundaries.\n\nRecordings will not be a single file. Continue?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            self._segmentation.setChecked(False)

    def output_directory(self) -> Path:
        return Path(self._output_edit.text())

    def collect(self) -> dict[str, Any]:
        """Everything the dialog can set, in the shape `save_config` takes.

        One method, used for both the apply and the persist, so a setting cannot be
        applied for the session and saved as something different.
        """
        return {
            "output_directory": str(self.output_directory()),
            "filename_template": self._filename_template.text(),
            "language": str(self._language.currentData() or "en"),
            "fps": int(self._fps.currentData()),
            "cqp": self._cqp.value(),
            "container": str(self._container.currentData()),
            "encoder": str(self._encoder.currentData() or ""),
            "audio_device": str(self._audio_device.currentData() or ""),
            "channel_layout": str(self._layout_combo.currentData() or "auto"),
            "audio_bitrate_kbps": self._audio_bitrate.value(),
            # SPEC.md §8.6. Sent as the user left them; the engine refuses the MP4
            # combination on its own (§20 row 16) rather than trusting this to have
            # been prevented here.
            "multitrack_enabled": self._multitrack.isChecked(),
            "multitrack_targets": self._multitrack_targets.text().strip(),
            "multitrack_reattach": self._multitrack_reattach.isChecked(),
            "segmentation_enabled": self._segmentation.isChecked(),
            "segment_minutes": self._segment_minutes.value(),
            "capture_backend": str(self._backend.currentData() or "auto"),
            "capture_cursor": self._cursor.isChecked(),
            "hdr_tonemap": self._hdr.isChecked(),
            "log_level": str(self._log_level.currentData() or "info"),
            "gpu_override": self._gpu_override.text().strip(),
            "check_updates": self._check_updates.isChecked(),
            # SPEC.md §16.5's bindings. Sent as the user left them; whether each one
            # can actually be taken is Windows' answer, and the window re-applies
            # them after the save and reports what happened.
            **self._hotkeys.collect(),
        }

    def _on_accept(self) -> None:
        """Apply **and persist**, then close.

        `save_config` does both: the engine merges the settings into the configuration a
        recording reads, then writes `config.toml`. One command rather than `configure`
        followed by a save, because two commands is a window in which the applied and the
        saved state can differ -- and the one that differs is the one nobody looks at.

        A failed save is reported and the dialog **stays open**. Closing on a failure
        would tell the user their settings were kept when they were not, which is the
        settings-dialog version of §16.5's "the status panel never lies".
        """
        self._output_directory = self.output_directory()
        settings = self.collect()

        if not self._engine.running:
            # Nothing to persist through. Accept so the output directory still applies
            # for this session, and say why it will not survive a restart.
            QMessageBox.information(
                self,
                "Settings not saved",
                "The engine is offline, so these settings apply to this session only.\n\n"
                "Start the engine and reopen Settings to save them.",
            )
            self.accept()
            return

        if not self._engine.save_config(settings):
            # `save_config` already reported the reason through `notified`.
            QMessageBox.warning(
                self,
                "Settings not saved",
                "The engine could not write the configuration file. Your changes have "
                "not been kept — see the status bar for the reason.",
            )
            return

        self.accept()
