"""The floating recording pill (M9.6 F1).

A small always-on-top control that stays reachable while the thing being recorded is
fullscreen: elapsed time, file size, pause/resume, stop, and — after stop — a progress
bar for the seconds it takes to write the file.

Three properties this is built around, each of which is a reported defect if it is wrong:

* **It is never in the recording.** Not the pill, and not a black rectangle where the
  pill was. `overlay.surface.prepare` is how, and the pill refuses to show itself if
  that did not take (see `_showable`).
* **It never steals focus.** Clicking pause must not minimize the fullscreen game being
  recorded. Also `surface.prepare` — `WS_EX_NOACTIVATE`.
* **It stays responsive.** Buttons acknowledge a click within a frame, and the event
  loop stays alive through a stop because `EngineController.stop_recording` no longer
  blocks it (M9.6 Phase 0).

**Budget.** SPEC.md §16.1 caps GUI CPU at 3% while recording. This repaints at 10 Hz,
updating two labels; the recording dot's 1 Hz pulse is the only animation, which is also
all §16.3 permits ("the recording dot pulses; nothing else moves").

The pill renders state and emits intent. It never talks to the engine — same rule as
every widget in `widgets/panels.py`, and the reason the threading story stays in one
place.
"""

from __future__ import annotations

import logging
from typing import Any

from PySide6.QtCore import QPoint, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QPushButton,
    QStackedWidget,
    QWidget,
)

from ..ipc.protocol import RecordingState
from ..theme import colour
from ..units import format_hms, format_size
from .elapsed import ElapsedClock
from .exclusion import ExclusionSupport
from .placement import Corner, clamp_to_screens, corner_position, snap_to_edges
from .surface import prepare

_log = logging.getLogger(__name__)

#: Local refresh rate. Fast enough that the seconds digit never visibly stalls, slow
#: enough to stay inside §16.1's 3% budget -- the engine's own `stats` is 2 Hz, and
#: everything between those is interpolation (see `ElapsedClock`).
_TICK_MS = 100

#: How long "Saved" stays up before the pill closes itself.
#:
#: Not cosmetic. A progress bar that vanishes the instant it reaches 100% leaves the user
#: unsure whether it finished or the application gave up, and the recording they are
#: unsure about is one they cannot make again.
_SAVED_DWELL_MS = 600

_CORNER_RADIUS = 18


class RecordingPill(QWidget):
    """The floating control. Renders state, emits intent."""

    pause_requested = Signal()
    resume_requested = Signal()
    stop_requested = Signal()

    #: The user dragged it. Carries the new top-left in virtual-desktop coordinates so
    #: the window above can persist it (`overlay.pill_x` / `overlay.pill_y`).
    moved = Signal(QPoint)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("RecordingPill")

        self._state = RecordingState.IDLE
        self._clock = ElapsedClock()
        self._bytes = 0
        self._drag_origin: QPoint | None = None
        self._closing = False

        self._build()

        # Flags, no-activate, and capture exclusion -- all of it, in one call, so there
        # is no path where a surface gets three of the four (see `surface.prepare`).
        self._exclusion = prepare(self)

        self._tick = QTimer(self)
        self._tick.setInterval(_TICK_MS)
        self._tick.timeout.connect(self._refresh)

        self.apply_state(RecordingState.IDLE)

    # -- construction -------------------------------------------------------

    def _build(self) -> None:
        self.setFixedHeight(44)
        self.setMinimumWidth(300)

        root = QHBoxLayout(self)
        root.setContentsMargins(14, 0, 8, 0)
        root.setSpacing(10)

        self._dot = _PillDot()
        root.addWidget(self._dot)

        # Two pages rather than showing and hiding six widgets: the live readout and the
        # save progress are different layouts, and a stack means neither can leave a
        # stale widget visible behind the other.
        self._pages = QStackedWidget()
        root.addWidget(self._pages, stretch=1)

        self._pages.addWidget(self._build_live_page())
        self._pages.addWidget(self._build_saving_page())

        self._pause = _PillButton("❚❚")
        self._pause.setToolTip("Pause recording")
        self._pause.clicked.connect(self._on_pause_clicked)
        root.addWidget(self._pause)

        self._stop = _PillButton("■")
        # A dynamic property, **not** `setObjectName("PillStop")`. `setObjectName`
        # *replaces* the name, so the stop button stopped matching `#PillButton` and
        # lost its background, border and radius -- it rendered as a flat dark square.
        # Caught by looking at it; no state assertion would have.
        self._stop.setProperty("variant", "stop")
        self._stop.setToolTip("Stop and save")
        self._stop.clicked.connect(self._on_stop_clicked)
        root.addWidget(self._stop)

    def _build_live_page(self) -> QWidget:
        page = QWidget()
        layout = QHBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)

        self._elapsed = QLabel(format_hms(0))
        self._elapsed.setObjectName("PillElapsed")
        # Reserve the width of a two-digit-hour time so the separator and the size
        # beside it do not shuffle sideways every time the seconds digit changes. Qt
        # has no `font-variant-numeric`, so this is how the layout is kept still --
        # measured from the label's own font rather than hard-coded, since the theme
        # picks whichever of three families the system has.
        self._elapsed.setMinimumWidth(self._elapsed.fontMetrics().horizontalAdvance("00:00:00"))
        layout.addWidget(self._elapsed)

        separator = QLabel("·")
        separator.setObjectName("PillSeparator")
        layout.addWidget(separator)

        self._size = QLabel(format_size(0))
        self._size.setObjectName("PillSize")
        layout.addWidget(self._size)

        layout.addStretch(1)
        return page

    def _build_saving_page(self) -> QWidget:
        page = QWidget()
        layout = QHBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        self._phase = QLabel("Saving…")
        self._phase.setObjectName("PillPhase")
        layout.addWidget(self._phase)

        self._progress = QProgressBar()
        self._progress.setObjectName("PillProgress")
        self._progress.setTextVisible(False)
        self._progress.setFixedHeight(6)
        self._progress.setRange(0, 100)
        self._progress.setValue(0)
        layout.addWidget(self._progress, stretch=1)
        return page

    # -- state --------------------------------------------------------------

    @property
    def exclusion(self) -> ExclusionSupport:
        """Whether this window is genuinely uncapturable."""
        return self._exclusion

    def _showable(self) -> bool:
        """Rule A, enforced at the one place that cannot be bypassed.

        A pill that would appear in the recording is not shown, whatever the setting
        says. Refusing here rather than only at the call site means a future caller
        cannot reintroduce the defect by forgetting to check.
        """
        if self._exclusion is ExclusionSupport.EXCLUDED:
            return True
        _log.warning(
            "the recording pill will not be shown: capture exclusion is %s on this system, "
            "and an overlay that would be recorded is never the fallback",
            self._exclusion.value,
        )
        return False

    def apply_state(self, state: RecordingState) -> None:
        """Render `state`. The engine is the authority; nothing here is optimistic."""
        previous = self._state
        self._state = state

        recording = state is RecordingState.RECORDING
        paused = state is RecordingState.PAUSED
        stopping = state is RecordingState.STOPPING

        self._dot.apply_state(state)

        # §16.5 makes paused a first-class state: the glyph changes, the dot holds, and
        # the border takes the warn colour. A paused pill that looks like a recording one
        # is how ten minutes of footage goes missing without anyone noticing.
        self._pause.setText("▶" if paused else "❚❚")
        self._pause.setToolTip("Resume recording" if paused else "Pause recording")
        self._pause.setEnabled(recording or paused)
        self._stop.setEnabled(recording or paused)

        if stopping:
            self._pages.setCurrentIndex(1)
        elif recording or paused:
            self._pages.setCurrentIndex(0)

        self._clock.sync(self._clock.elapsed_ms(), advancing=recording)
        if stopping or state in (RecordingState.IDLE, RecordingState.OFFLINE, RecordingState.FAULTED):
            self._clock.hold()

        if recording or paused:
            self._closing = False
            if not self._tick.isActive():
                self._tick.start()
            if not self.isVisible() and self._showable():
                self.show()
        elif state is RecordingState.STOPPING:
            # Kept on screen: the save progress is the reason this feature exists.
            self._tick.start()
        elif not self._closing:
            # Idle, offline or faulted without having gone through a stop -- for example
            # the engine died. Nothing to report and nothing to wait for.
            self._tick.stop()
            self.hide()

        if previous is not state:
            self.update()

    def apply_stats(self, stats: dict[str, Any]) -> None:
        """Take one 2 Hz `stats` payload as the truth.

        `timeline_ms` rather than wall clock: SPEC.md §7.5 excises paused time from the
        file, and this number is "the length the file will have" (§16.2).
        """
        state = RecordingState.from_wire(str(stats.get("state", "idle")))
        self._clock.sync(int(stats.get("timeline_ms", 0)), advancing=state is RecordingState.RECORDING)
        self._bytes = int(stats.get("bytes_written", 0))
        self._refresh()

    def apply_finalize_progress(self, progress: dict[str, Any]) -> None:
        """Render one `finalize_progress` event (M9.6 §2.1)."""
        self._pages.setCurrentIndex(1)
        phase = str(progress.get("phase", ""))
        percent = int(progress.get("percent", 0))
        total = int(progress.get("bytes_total", 0))

        self._phase.setText(_PHASE_LABELS.get(phase, "Saving…"))

        if total <= 0 and phase not in ("done", "swapping"):
            # No byte progress behind this phase -- validating is a fixed ~1 s decode
            # with no proportional quantity (BUG-046). Shown as a moving indeterminate
            # bar rather than a number that would have to be invented. A bar parked at
            # its last value for a second reads as a hang.
            self._progress.setRange(0, 0)
        else:
            self._progress.setRange(0, 100)
            self._progress.setValue(max(self._progress.value(), percent))

    def apply_finalized(self, report: dict[str, Any]) -> None:
        """The recording ended. Close only if the file is genuinely saved.

        **The pill never closes on a timer and never on the stop command returning.** It
        closes when the engine says the file validated -- because until then there is
        nothing to be reassured about, and a control that disappeared before the outcome
        was known would be the recorder claiming success it had not verified.
        """
        self._pages.setCurrentIndex(1)
        self._progress.setRange(0, 100)

        if not report.get("valid"):
            # Stays up, in warn colour, saying so. CLAUDE.md §1: a file that did not
            # validate is the one thing a user must not discover later. The main window
            # raises the dialog; the pill's job is not to vanish as if nothing happened.
            self._progress.setValue(0)
            self._phase.setText("Save failed")
            self._phase.setProperty("state", "failed")
            self._repolish(self._phase)
            self._tick.stop()
            return

        self._progress.setValue(100)
        self._phase.setText("Saved")
        self._tick.stop()
        self._closing = True
        QTimer.singleShot(_SAVED_DWELL_MS, self._finish_close)

    def _finish_close(self) -> None:
        if not self._closing:
            # A new recording started inside the dwell. Closing now would take the pill
            # away from a recording that is running.
            return
        self._closing = False
        self.hide()
        self._reset()

    def _reset(self) -> None:
        self._clock.reset()
        self._bytes = 0
        self._progress.setRange(0, 100)
        self._progress.setValue(0)
        self._phase.setText("Saving…")
        self._phase.setProperty("state", "")
        self._repolish(self._phase)
        self._pages.setCurrentIndex(0)
        self._elapsed.setText(format_hms(0))
        self._size.setText(format_size(0))

    def _refresh(self) -> None:
        self._elapsed.setText(format_hms(self._clock.elapsed_ms()))
        self._size.setText(format_size(self._bytes))

    @staticmethod
    def _repolish(widget: QWidget) -> None:
        """Qt does not re-evaluate a stylesheet when a property changes; it has to be
        asked. The same call `ControlsPanel` needs for the same reason."""
        widget.style().unpolish(widget)
        widget.style().polish(widget)

    # -- intent -------------------------------------------------------------

    def _on_pause_clicked(self) -> None:
        # The button is disabled immediately so the click is acknowledged within a frame,
        # then re-enabled by whatever state the engine reports. A control that looks
        # unpressed for 40 ms reads as broken however fast the round trip is.
        self._pause.setEnabled(False)
        if self._state is RecordingState.PAUSED:
            self.resume_requested.emit()
        else:
            self.pause_requested.emit()

    def _on_stop_clicked(self) -> None:
        self._stop.setEnabled(False)
        self._pause.setEnabled(False)
        # Switched here rather than waiting for `state_changed`, so the user sees the
        # save begin on their own click rather than a round trip later.
        self._pages.setCurrentIndex(1)
        self._phase.setText("Saving…")
        self._progress.setRange(0, 0)
        self._clock.hold()
        self.stop_requested.emit()

    # -- placement ----------------------------------------------------------

    def place(self, corner: Corner, remembered: QPoint | None) -> None:
        """Put the pill where the user left it, or in `corner` if they never moved it.

        A remembered position on a monitor that is no longer attached is clamped back
        on screen. That is not tidiness: the stop button goes with it, and a recording
        whose only control is off-screen can be ended only by killing the application.
        """
        size = QSize(self.width(), self.height())
        screens = [screen.availableGeometry() for screen in QApplication.screens()]

        if remembered is not None:
            self.move(clamp_to_screens(remembered, size, screens))
            return

        # PySide6's stubs type `primaryScreen()` as non-optional; it returns `None` when
        # there is no display at all, which a headless or mid-topology-change session
        # genuinely hits. The fallback stays and mypy is told the stub is optimistic.
        if not screens:
            # No display at all -- a mid-topology-change moment. Nowhere to place it,
            # and the next screen-change event will bring us back here.
            return

        # PySide6's stubs type `primaryScreen()` as non-optional; it does return `None`
        # when there is no primary display, so the fallback stays.
        primary = QApplication.primaryScreen()
        area = screens[0] if primary is None else primary.availableGeometry()
        self.move(corner_position(area, size, corner))

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 -- Qt's spelling
        if event.button() is Qt.MouseButton.LeftButton:
            self._drag_origin = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802 -- Qt's spelling
        if self._drag_origin is None:
            return
        self.move(event.globalPosition().toPoint() - self._drag_origin)
        event.accept()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802 -- Qt's spelling
        if self._drag_origin is None:
            return
        self._drag_origin = None

        size = QSize(self.width(), self.height())
        screen = self.screen()
        if screen is not None:
            self.move(snap_to_edges(self.pos(), size, screen.availableGeometry()))
        screens = [candidate.availableGeometry() for candidate in QApplication.screens()]
        self.move(clamp_to_screens(self.pos(), size, screens))

        self.moved.emit(self.pos())
        event.accept()

    # -- painting -----------------------------------------------------------

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 -- Qt's spelling
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        body = QColor(colour("bg-surface"))
        # Not fully opaque: the pill sits over whatever is being recorded, and a solid
        # slab hides more of it than it needs to. §16.3 bans glassmorphism, not opacity.
        body.setAlpha(238)
        painter.setBrush(body)

        border = QColor(colour("warn") if self._state is RecordingState.PAUSED else colour("border"))
        pen = painter.pen()
        pen.setColor(border)
        pen.setWidth(1)
        painter.setPen(pen)

        painter.drawRoundedRect(self.rect().adjusted(0, 0, -1, -1), _CORNER_RADIUS, _CORNER_RADIUS)


#: Wire phase name -> what the user reads. `flushing` and `swapping` are both under a
#: tenth of a second (BUG-046 measured the swap at ~5 ms), so naming them separately
#: would be flicker rather than information.
_PHASE_LABELS = {
    "flushing": "Saving…",
    "remuxing": "Saving…",
    "validating": "Checking…",
    "swapping": "Saving…",
    "done": "Saved",
}


class _PillButton(QPushButton):
    """A round, flat control sized for the pill's 44 px height."""

    def __init__(self, glyph: str, parent: QWidget | None = None) -> None:
        super().__init__(glyph, parent)
        self.setObjectName("PillButton")
        self.setFixedSize(28, 28)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)


class _PillDot(QWidget):
    """The recording indicator, on the pill.

    Same rule as `panels._RecordingDot` and for the same reason (§16.5): it pulses while
    recording and **holds** while paused. Separate rather than shared because that one is
    12 px and lives in a panel with a stylesheet; sharing would mean parameterising a
    widget on two axes to save nine lines.
    """

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("PillDot")
        self.setFixedSize(14, 14)
        self._state = RecordingState.IDLE
        self._bright = True

        self._timer = QTimer(self)
        self._timer.setInterval(500)  # 500 ms half-period = the 1 Hz §16.3 asks for
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

        if self._state is RecordingState.PAUSED:
            base = QColor(colour("warn"))
        elif self._state is RecordingState.RECORDING:
            base = QColor(colour("rec-active"))
        else:
            base = QColor(colour("text-secondary"))

        if not self._bright:
            base.setAlpha(90)

        painter.setBrush(base)
        painter.setPen(Qt.PenStyle.NoPen)

        if self._state is RecordingState.PAUSED:
            # Two bars, so paused is distinguishable at a glance and without colour --
            # §16.5 makes it a first-class state, and a colour-only difference is not one
            # for a user who cannot see the difference between amber and red.
            rect = self.rect()
            bar = max(3, rect.width() // 4)
            gap = max(2, rect.width() // 7)
            top, height = rect.top() + 1, rect.height() - 2
            left = rect.center().x() - bar - (gap // 2)
            painter.drawRect(left, top, bar, height)
            painter.drawRect(left + bar + gap, top, bar, height)
            return

        painter.drawEllipse(self.rect().adjusted(1, 1, -1, -1))
