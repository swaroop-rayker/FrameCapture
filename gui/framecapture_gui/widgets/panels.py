"""The four panels below the preview (SPEC.md §16.2), and the preview surface itself.

Every panel is passive: it renders what it is given and emits what the user did. None of
them talks to the engine. That is what keeps the threading story in ``EngineController``
and out of here.
"""

from __future__ import annotations

from typing import Any

from PySide6.QtCore import QRect, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QImage, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QFrame,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QProgressBar,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from ..ipc.protocol import RecordingState
from ..preview import PreviewChannel
from ..theme import colour


def _format_hms(milliseconds: int) -> str:
    """``HH:MM:SS``. Negative clamps to zero rather than rendering a minus sign."""
    total = max(0, milliseconds) // 1000
    return f"{total // 3600:02d}:{(total % 3600) // 60:02d}:{total % 60:02d}"


class PreviewSurface(QFrame):
    """The 16:9 preview area (SPEC.md §16.2), showing SPEC.md §15.2's shared-memory ring.

    **Letterboxed, never stretched.** §16.2 asks for "16:9, letterboxed", and the reason is
    the same reason the status panel is not allowed to lie: a preview that fills its widget
    by distorting the picture is telling the user their recording is a shape it is not.

    The paint path is deliberately three calls -- fill the bars, ask the channel for a
    ``QImage`` over shared memory, blit it into the fitted rectangle. No scaling in Python,
    no conversion, no copy (§16.1).
    """

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("PreviewSurface")
        self.setMinimumHeight(280)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        self._channel: PreviewChannel | None = None
        self._image: QImage | None = None
        self._placeholder = "Preview is off"

        layout = QVBoxLayout(self)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._message = QLabel(self._placeholder)
        self._message.setObjectName("SectionHint")
        self._message.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self._message)

        # §15.2 publishes at 30 fps, so asking more often than that only costs repaints of
        # a frame that has not changed -- `PreviewChannel.latest_image` returns None when
        # the sequence has not moved, and this widget then keeps what it drew.
        self._timer = QTimer(self)
        self._timer.setInterval(1000 // 30)
        self._timer.timeout.connect(self._poll)

    def set_channel(self, channel: PreviewChannel | None, *, placeholder: str = "Preview is off") -> None:
        """Attach or detach the shared-memory channel this surface renders."""
        self._channel = channel
        self._placeholder = placeholder
        self._image = None
        if channel is not None and channel.attached:
            self._message.setVisible(False)
            self._timer.start()
        else:
            self._timer.stop()
            self._message.setText(placeholder)
            self._message.setVisible(True)
        self.update()

    @property
    def has_frame(self) -> bool:
        """Whether a published frame has been taken from the ring and is being drawn."""
        return self._image is not None

    def _poll(self) -> None:
        channel = self._channel
        if channel is None or not channel.attached:
            return
        image = channel.latest_image()
        if image is None:
            return
        # Replaced, not accumulated: the image borrows a slot of shared memory, so exactly
        # one is held at a time and it is dropped the moment a newer one arrives.
        #
        # It is held until then rather than only for one paint, which is what lets a static
        # screen keep showing its last frame instead of blanking between publications. The
        # slot stays intact for the two publications §15.2's three buffers buy -- 66 ms at
        # 30 fps, against a repaint that happens within 16 ms of this call -- and a slot the
        # writer laps in that window is reported by `latest_image` and dropped rather than
        # painted torn.
        self._image = image
        # `isHidden`, not `isVisible`: the latter is false for any widget whose window has
        # not been shown, so on a headless run it would report the placeholder as gone
        # while it was still there.
        if not self._message.isHidden():
            self._message.setVisible(False)
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 -- Qt's spelling
        super().paintEvent(event)
        image = self._image
        if image is None or image.isNull():
            return

        painter = QPainter(self)
        # §16.3: no pure black. The letterbox bars are the window background, not #000000,
        # for the halation reason that section gives.
        painter.fillRect(self.rect(), QColor(colour("bg-base")))

        target = self.rect().adjusted(1, 1, -1, -1)
        fitted = image.size().scaled(target.size(), Qt.AspectRatioMode.KeepAspectRatio)
        painter.drawImage(
            QRect(
                target.x() + ((target.width() - fitted.width()) // 2),
                target.y() + ((target.height() - fitted.height()) // 2),
                fitted.width(),
                fitted.height(),
            ),
            image,
        )


class SourcesPanel(QGroupBox):
    """SPEC.md §16.2's source list."""

    refresh_requested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("Sources", parent)
        layout = QVBoxLayout(self)

        self._list = QListWidget()
        self._list.setAlternatingRowColors(False)
        layout.addWidget(self._list)

        buttons = QHBoxLayout()
        self._refresh = QPushButton("Refresh")
        self._refresh.setToolTip("Re-enumerate displays and windows")
        self._refresh.clicked.connect(self.refresh_requested)
        buttons.addWidget(self._refresh)
        buttons.addStretch(1)
        layout.addLayout(buttons)

        self.set_sources(None)

    def set_sources(self, sources: dict[str, Any] | None) -> None:
        """Render ``get_sources``' answer, or an honest blank."""
        self._list.clear()
        if sources is None:
            self._list.addItem("No engine — sources unavailable")
            self._list.setEnabled(False)
            return

        self._list.setEnabled(True)
        displays = sources.get("displays", [])
        for display in displays:
            primary = " (primary)" if display.get("primary") else ""
            name = display.get("name") or display.get("device", "Display")
            self._list.addItem(f"{name} — {display.get('width')}×{display.get('height')}{primary}")

        if not displays:
            self._list.addItem("No displays reported")
        if self._list.count():
            self._list.setCurrentRow(0)

    def selected_monitor(self) -> str:
        """The chosen display's ``stable_id``, or empty for the primary.

        Empty means "let the engine pick", which is what ``start_record`` does with a
        missing ``monitor``. Better than guessing an id the engine may no longer resolve.
        """
        return ""


class AudioMixerPanel(QGroupBox):
    """SPEC.md §16.2's audio mixer.

    The level meter is **not** driven, and says so. Per-track levels are not in §15.1's
    `stats` payload, and adding engine-side metering to feed a bar is M9.5's territory
    (Tier B is where per-track levels become meaningful). A bar that moved to invented
    numbers would violate §16.5's "the status panel never lies" one panel over.
    """

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("Audio Mixer", parent)
        layout = QVBoxLayout(self)

        self._name = QLabel("Desktop Audio")
        layout.addWidget(self._name)

        self._meter = QProgressBar()
        self._meter.setRange(0, 100)
        self._meter.setValue(0)
        self._meter.setTextVisible(False)
        self._meter.setEnabled(False)
        self._meter.setToolTip("Level metering is not implemented in this build")
        layout.addWidget(self._meter)

        self._detail = QLabel("Level metering: not in this build")
        self._detail.setObjectName("SectionHint")
        layout.addWidget(self._detail)

        layout.addStretch(1)

    def set_endpoint(self, name: str) -> None:
        self._name.setText(name or "Desktop Audio")


class ControlsPanel(QGroupBox):
    """Start / stop / pause, and the two buttons beside them (SPEC.md §16.2)."""

    start_requested = Signal()
    stop_requested = Signal()
    pause_requested = Signal()
    resume_requested = Signal()
    settings_requested = Signal()
    logs_requested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("Controls", parent)
        layout = QVBoxLayout(self)

        self._record = QPushButton("START RECORDING")
        self._record.setObjectName("PrimaryAction")
        self._record.clicked.connect(self._on_record_clicked)
        layout.addWidget(self._record)

        # §16.2's `❚❚`. Enabled only while recording, per the same paragraph.
        self._pause = QPushButton("❚❚  Pause")
        self._pause.clicked.connect(self._on_pause_clicked)
        layout.addWidget(self._pause)

        settings = QPushButton("Settings")
        settings.clicked.connect(self.settings_requested)
        layout.addWidget(settings)

        logs = QPushButton("Logs")
        logs.clicked.connect(self.logs_requested)
        layout.addWidget(logs)

        layout.addStretch(1)
        self._state = RecordingState.OFFLINE
        self.apply_state(RecordingState.OFFLINE)

    def _on_record_clicked(self) -> None:
        if self._state in (RecordingState.RECORDING, RecordingState.PAUSED):
            self.stop_requested.emit()
        else:
            self.start_requested.emit()

    def _on_pause_clicked(self) -> None:
        if self._state is RecordingState.PAUSED:
            self.resume_requested.emit()
        else:
            self.pause_requested.emit()

    def apply_state(self, state: RecordingState) -> None:
        """Enable exactly the actions that are valid now.

        SPEC.md §16.1: when the engine is down the controls are greyed rather than
        present-and-failing. A button that can be pressed and cannot work is worse than
        one that is visibly unavailable, because the failure arrives after the user has
        committed to the action.
        """
        self._state = state
        live = state not in (RecordingState.OFFLINE, RecordingState.FAULTED)
        active = state in (RecordingState.RECORDING, RecordingState.PAUSED)
        busy = state in (RecordingState.STARTING, RecordingState.STOPPING)

        self._record.setEnabled(live and not busy)
        self._record.setText("STOP RECORDING" if active else "START RECORDING")
        self._record.setObjectName("StopAction" if active else "PrimaryAction")
        # Qt does not re-evaluate a stylesheet when objectName changes; it has to be
        # asked. Without this the button keeps the previous colour and the most
        # important control on screen lies about what it does.
        self._record.style().unpolish(self._record)
        self._record.style().polish(self._record)

        # §16.2: "The pause control is enabled only while recording."
        self._pause.setEnabled(active and not busy)
        self._pause.setText("▶  Resume" if state is RecordingState.PAUSED else "❚❚  Pause")


class _RecordingDot(QWidget):
    """The status indicator (SPEC.md §16.3, §16.5).

    Pulses at 1 Hz while recording. **Holds steady while paused** -- §16.5: "The
    recording dot stops pulsing and holds. A paused recording that looks like a running
    one is how a user loses ten minutes of footage without noticing."

    §16.3 also says "the recording dot pulses; nothing else moves", so this is the only
    animation in the application.
    """

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setFixedSize(12, 12)
        self._state = RecordingState.OFFLINE
        self._bright = True

        self._timer = QTimer(self)
        # 500 ms half-period is 1 Hz, which is what §16.3 asks for.
        self._timer.setInterval(500)
        self._timer.timeout.connect(self._blink)

    def apply_state(self, state: RecordingState) -> None:
        self._state = state
        if state is RecordingState.RECORDING:
            self._bright = True
            if not self._timer.isActive():
                self._timer.start()
        else:
            self._timer.stop()
            self._bright = True
        self.update()

    def _blink(self) -> None:
        self._bright = not self._bright
        self.update()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 -- Qt's spelling
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if self._state in (RecordingState.RECORDING, RecordingState.PAUSED):
            base = QColor(colour("rec-active"))
        elif self._state in (RecordingState.OFFLINE, RecordingState.FAULTED):
            base = QColor(colour("text-disabled"))
        else:
            base = QColor(colour("text-secondary"))

        if not self._bright:
            base.setAlpha(90)

        painter.setBrush(base)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawEllipse(self.rect().adjusted(1, 1, -1, -1))


class StatusPanel(QGroupBox):
    """SPEC.md §16.2's status readout.

    §16.5: "**The status panel never lies.** If frames are being dropped, it says so in
    ``--warn`` colour with the exact count and percentage." Everything below renders a
    number the engine reported or says it does not have one; nothing is derived
    optimistically, and nothing is blank where a value is unknown.
    """

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("Status", parent)
        layout = QVBoxLayout(self)

        headline = QHBoxLayout()
        self._dot = _RecordingDot()
        headline.addWidget(self._dot)
        self._headline = QLabel("ENGINE OFFLINE")
        self._headline.setObjectName("StatusHeadline")
        headline.addWidget(self._headline)
        headline.addStretch(1)
        layout.addLayout(headline)

        # §7.5, §16.5: while paused this is the *timeline* elapsed -- the length the
        # file will actually have -- with total paused time on its own line beneath.
        self._paused_total = QLabel("")
        self._paused_total.setObjectName("StatusLine")
        layout.addWidget(self._paused_total)

        self._format = QLabel("—")
        self._format.setObjectName("StatusLine")
        layout.addWidget(self._format)

        self._encoder = QLabel("—")
        self._encoder.setObjectName("StatusLine")
        layout.addWidget(self._encoder)

        self._drops = QLabel("Dropped: —")
        self._drops.setObjectName("StatusLine")
        layout.addWidget(self._drops)

        self._output = QLabel("")
        self._output.setObjectName("StatusLine")
        self._output.setWordWrap(True)
        layout.addWidget(self._output)

        layout.addStretch(1)
        self._state = RecordingState.OFFLINE

    def apply_state(self, state: RecordingState) -> None:
        self._state = state
        self._dot.apply_state(state)

        if state is RecordingState.OFFLINE:
            self._headline.setText("ENGINE OFFLINE")
            self._paused_total.setText("")
            self._format.setText("—")
            self._encoder.setText("—")
            self._drops.setText("Dropped: —")
            self._output.setText("")

    def apply_stats(self, stats: dict[str, Any]) -> None:
        """Render one ``stats`` payload."""
        state = RecordingState.from_wire(str(stats.get("state", "idle")))
        self._state = state
        self._dot.apply_state(state)

        timeline_ms = int(stats.get("timeline_ms", 0))
        paused_ms = int(stats.get("paused_total_ms", 0))

        if state is RecordingState.PAUSED:
            # §16.2: "While paused the status line reads `❚❚ PAUSED 00:14:22` -- the
            # **timeline** elapsed, i.e. the length the file will have."
            self._headline.setText(f"❚❚ PAUSED  {_format_hms(timeline_ms)}")
        elif state is RecordingState.RECORDING:
            self._headline.setText(f"REC  {_format_hms(timeline_ms)}")
        elif state is RecordingState.STARTING:
            self._headline.setText("STARTING…")
        elif state is RecordingState.STOPPING:
            self._headline.setText("FINALIZING…")
        elif state is RecordingState.FAULTED:
            self._headline.setText("FAULTED")
        else:
            self._headline.setText("READY")

        # §16.2, §16.5: reported separately and always when non-zero, because "why is my
        # 30-minute recording 12 minutes long" has to be answerable from the window too.
        pauses = int(stats.get("pauses", 0))
        if paused_ms > 0 or pauses > 0:
            self._paused_total.setText(f"Paused: {_format_hms(paused_ms)}  ({pauses}×)")
        else:
            self._paused_total.setText("")

        encoded = int(stats.get("frames_encoded", 0))
        dropped = int(stats.get("frames_dropped", 0))
        self._format.setText(f"{encoded:,} frames encoded")

        written = int(stats.get("bytes_written", 0))
        self._encoder.setText(f"{written / (1024 * 1024):.1f} MB written")

        # The one line §16.5 calls out by name. Exact count *and* percentage, in warn
        # colour the moment it is non-zero -- not above a threshold, because a user
        # deciding whether their recording is usable is entitled to the real number.
        total = encoded + dropped
        if dropped > 0 and total > 0:
            self._drops.setText(f"Dropped: {dropped:,} ({dropped / total * 100:.1f}%)")
            self._drops.setObjectName("StatusWarning")
        else:
            self._drops.setText("Dropped: 0 (0.0%)")
            self._drops.setObjectName("StatusOk" if state is RecordingState.RECORDING else "StatusLine")
        self._drops.style().unpolish(self._drops)
        self._drops.style().polish(self._drops)

        output = str(stats.get("output", ""))
        self._output.setText(output)
