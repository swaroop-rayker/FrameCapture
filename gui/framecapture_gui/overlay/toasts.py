"""Toast notifications (M9.6 F2).

Short messages in a column at a screen corner: a recording paused, a file saved, a
warning from the engine, an error that needs reading. Same two hard properties as the
pill — **never in the recording**, and **never takes focus** — both by going through
`overlay.surface.prepare`, which is the one place either can be got wrong.

**The reported defect is stacking**, and the fix is an ownership rule rather than an
algorithm: `ToastManager` computes every position and every toast is told where to be.
`Toast` has no idea where it is. Two objects deciding position independently is precisely
how two of them end up in the same place, and no amount of care at either site fixes a
design where both are entitled to an opinion.

Everything about *what* is held and *for how long* is in `toast_model`, which is pure and
gets tested against a thousand messages without a screen. This file is the rendering.

**Relationship to the status bar.** Additive. `main_window` keeps showing warnings and
errors there, and deliberately still opens no modals for them. When capture exclusion is
unavailable the toasts do not appear at all and the status bar is the whole story — an
overlay that would land in the recording is never the fallback.
"""

from __future__ import annotations

import logging
import time

from PySide6.QtCore import (
    QEasingCurve,
    QEvent,
    QPoint,
    QPropertyAnimation,
    QRect,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import QColor, QEnterEvent, QMouseEvent, QPainter, QPaintEvent
from PySide6.QtWidgets import (
    QApplication,
    QGraphicsOpacityEffect,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from ..theme import colour
from .exclusion import ExclusionSupport
from .placement import STACK_GAP, Corner, stack_positions
from .surface import prepare
from .toast_model import Severity, ToastEntry, ToastQueue

_log = logging.getLogger(__name__)

#: SPEC.md §16.3: "All state changes animate at 120 ms ease-out."
#:
#: §16.3 also says "the recording dot pulses; nothing else moves", which is about idle
#: motion -- a column that jumped between layouts would be a state change rendered
#: without the transition the same paragraph asks for.
_ANIMATION_MS = 120

#: How often expiry is checked. Coarse on purpose: a toast is not a stopwatch, and a
#: 4-second dwell does not need 60 Hz.
_TICK_MS = 250

_TOAST_WIDTH = 340
_CORNER_RADIUS = 10


class Toast(QWidget):
    """One message. Renders what it is given; knows nothing about where it is."""

    dismissed = Signal(object)
    action_invoked = Signal(object)

    def __init__(self, entry: ToastEntry, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("Toast")
        self.entry = entry
        #: True while the pointer is over it. `ToastManager` reads this and does not age
        #: a toast the user is currently reading.
        self.hovered = False

        self._build(entry)
        self.setProperty("severity", entry.severity.value)

        self._exclusion = prepare(self)
        self._fade = QGraphicsOpacityEffect(self)
        self.setGraphicsEffect(self._fade)

    @property
    def exclusion(self) -> ExclusionSupport:
        return self._exclusion

    def _build(self, entry: ToastEntry) -> None:
        self.setFixedWidth(_TOAST_WIDTH)

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 10, 12, 10)
        root.setSpacing(6)

        headline = QHBoxLayout()
        headline.setSpacing(8)

        self._text = QLabel(entry.text)
        self._text.setObjectName("ToastText")
        self._text.setWordWrap(True)
        headline.addWidget(self._text, stretch=1)

        #: The repeat counter. Hidden at a count of one, where it would be noise.
        self._count = QLabel("")
        self._count.setObjectName("ToastCount")
        headline.addWidget(self._count, alignment=Qt.AlignmentFlag.AlignTop)
        root.addLayout(headline)

        if entry.detail:
            detail = QLabel(entry.detail)
            detail.setObjectName("ToastDetail")
            detail.setWordWrap(True)
            root.addWidget(detail)

        if entry.action:
            row = QHBoxLayout()
            row.addStretch(1)
            button = QPushButton(entry.action)
            button.setObjectName("ToastAction")
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.clicked.connect(self._on_action)
            row.addWidget(button)
            root.addLayout(row)

        self.refresh()
        self.adjustSize()

    def refresh(self) -> None:
        """Re-render after the entry changed — which, for a coalesced repeat, it has."""
        self._text.setText(self.entry.text)
        self._count.setText(f"×{self.entry.count}" if self.entry.count > 1 else "")
        self._count.setVisible(self.entry.count > 1)
        self.adjustSize()

    # -- input --------------------------------------------------------------

    def _on_action(self) -> None:
        self.action_invoked.emit(self.entry)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802 -- Qt's spelling
        """Click anywhere to dismiss.

        The whole surface, not a close button: an error stays until dismissed, so
        dismissing has to be easy enough that the toast is not in the way — and a 12 px
        target in the corner of a window that must not take focus is not that.
        """
        if event.button() is Qt.MouseButton.LeftButton:
            self.dismissed.emit(self.entry)
            event.accept()

    def enterEvent(self, event: QEnterEvent) -> None:  # noqa: N802 -- Qt's spelling
        # Pausing the countdown under the pointer: a message the user is reading must not
        # vanish mid-sentence.
        self.hovered = True
        super().enterEvent(event)

    def leaveEvent(self, event: QEvent) -> None:  # noqa: N802 -- Qt's spelling
        self.hovered = False
        super().leaveEvent(event)

    # -- painting -----------------------------------------------------------

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802 -- Qt's spelling
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        body = QColor(colour("bg-surface"))
        body.setAlpha(242)
        painter.setBrush(body)

        pen = painter.pen()
        pen.setColor(QColor(_ACCENT_FOR[self.entry.severity]()))
        pen.setWidth(1)
        painter.setPen(pen)
        painter.drawRoundedRect(self.rect().adjusted(0, 0, -1, -1), _CORNER_RADIUS, _CORNER_RADIUS)

        # A severity stripe down the leading edge, so the kind of message is legible
        # without reading it and without depending on telling amber from red.
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(_ACCENT_FOR[self.entry.severity]()))
        painter.drawRoundedRect(1, 1, 4, self.height() - 2, 2, 2)


#: Severity -> the palette entry that marks it. Looked up lazily so the palette stays the
#: single source of truth in `theme/__init__.py` rather than being copied at import time.
_ACCENT_FOR = {
    Severity.INFO: lambda: colour("accent"),
    Severity.WARNING: lambda: colour("warn"),
    Severity.ERROR: lambda: colour("rec-active"),
}


class ToastManager(QWidget):
    """Owns the column: what is shown, where each one goes, and when it leaves.

    A `QWidget` only so it can parent the toasts' lifetimes and own timers; it is never
    shown itself.
    """

    #: A toast's action button was pressed. Carries the entry so the window above can
    #: decide what "Show details" means for that message.
    action_invoked = Signal(object)

    def __init__(
        self,
        parent: QWidget | None = None,
        *,
        corner: Corner = Corner.TOP_RIGHT,
        max_visible: int = 4,
        duration_s: int = 4,
    ) -> None:
        super().__init__(parent)
        self.setVisible(False)

        self._corner = corner
        self._duration_s = duration_s
        self._queue = ToastQueue(max_visible=max_visible)
        self._widgets: dict[int, Toast] = {}
        #: One in-flight animation per toast, keyed by `id(toast)`. **Not a list.** Two
        #: animations driving the same `pos` property fight, and the loser wins
        #: whichever way the last frame lands -- so a burst of toasts arriving faster
        #: than 120 ms could leave one parked at a stale position for good.
        self._animations: dict[int, QPropertyAnimation] = {}
        self._enabled = True
        #: Set on the first toast built; `UNKNOWN` until then. Nothing is shown unless
        #: this is `EXCLUDED`.
        self._exclusion = ExclusionSupport.UNKNOWN

        self._tick = QTimer(self)
        self._tick.setInterval(_TICK_MS)
        self._tick.timeout.connect(self._expire)

        # Re-anchor when the display topology changes: a column measured against a screen
        # that has gone is a column off the side of the desktop.
        app = QApplication.instance()
        if isinstance(app, QApplication):
            app.screenAdded.connect(self._relayout)
            app.screenRemoved.connect(self._relayout)
            app.primaryScreenChanged.connect(self._relayout)

    # -- configuration ------------------------------------------------------

    def configure(self, *, corner: Corner, max_visible: int, duration_s: int, enabled: bool) -> None:
        """Apply `overlay.*` settings. Safe to call while toasts are on screen."""
        self._corner = corner
        self._duration_s = duration_s
        self._enabled = enabled
        self._queue = ToastQueue(max_visible=max_visible)
        if not enabled:
            self.clear()
        else:
            self._relayout()

    @property
    def exclusion(self) -> ExclusionSupport:
        return self._exclusion

    @property
    def dropped(self) -> int:
        """Messages discarded because the cap was reached."""
        return self._queue.dropped

    def visible_toasts(self) -> list[Toast]:
        """The toasts currently on screen, newest first."""
        return [self._widgets[id(entry)] for entry in self._queue.visible if id(entry) in self._widgets]

    # -- posting ------------------------------------------------------------

    def post(
        self,
        severity: Severity,
        text: str,
        *,
        detail: str = "",
        action: str = "",
    ) -> ToastEntry | None:
        """Show a message. Returns the entry representing it, or `None` if suppressed.

        `None` means the column is off — disabled in settings, or capture exclusion is
        unavailable and showing anything would put it in the recording. The caller has
        already put the message in the status bar either way; this is additive.
        """
        if not self._enabled or not text:
            return None

        entry = self._queue.add(ToastEntry(severity=severity, text=text, detail=detail, action=action))
        existing = self._widgets.get(id(entry))
        if existing is not None:
            # A coalesced repeat: the count changed, nothing else did.
            existing.refresh()
            self._relayout()
            return entry

        self._sync_widgets()
        if not self._tick.isActive():
            self._tick.start()
        return entry

    def dismiss(self, entry: ToastEntry) -> None:
        self._queue.dismiss(entry)
        self._sync_widgets()

    def clear(self) -> None:
        self._queue.clear()
        # Before the widgets go: an animation outliving its target is a write to a
        # deleted object.
        for animation in self._animations.values():
            animation.stop()
        self._animations.clear()
        self._sync_widgets()
        self._tick.stop()

    # -- internals ----------------------------------------------------------

    def _expire(self) -> None:
        # A toast under the pointer does not age -- the user is reading it. Held by
        # refreshing its timestamp, so the dwell restarts when they move away rather
        # than expiring the instant they do.
        for toast in self.visible_toasts():
            if toast.hovered:
                toast.entry.last_seen = time.monotonic()

        if self._queue.expire(self._duration_s):
            self._sync_widgets()
        if not self._queue.entries:
            self._tick.stop()

    def _sync_widgets(self) -> None:
        """Make the widgets match the queue: build what is new, drop what has gone."""
        wanted = {id(entry): entry for entry in self._queue.visible}

        for key in list(self._widgets):
            if key not in wanted:
                toast = self._widgets.pop(key)
                # Its animation first: one still running against a deleted widget is a
                # write to freed memory on the next frame.
                animation = self._animations.pop(id(toast), None)
                if animation is not None:
                    animation.stop()
                toast.hide()
                toast.deleteLater()

        for key, entry in wanted.items():
            if key in self._widgets:
                continue
            toast = Toast(entry)
            self._exclusion = toast.exclusion
            if toast.exclusion is not ExclusionSupport.EXCLUDED:
                # Rule A. One line, once: an overlay that cannot be hidden from the
                # capture is not shown at all, and the status bar carries the message.
                _log.warning("toasts suppressed: capture exclusion is %s on this system", toast.exclusion.value)
                toast.deleteLater()
                self._enabled = False
                self.clear()
                return
            toast.dismissed.connect(self.dismiss)
            toast.action_invoked.connect(self.action_invoked)
            self._widgets[key] = toast

        self._relayout()

    def _relayout(self) -> None:
        """Recompute every position and move each toast to it.

        The single authority on where anything is. Called on insert, on removal, on a
        coalesced repeat (the count can change a toast's height), and on a screen change.
        """
        toasts = self.visible_toasts()
        if not toasts:
            return

        area = self._anchor_area()
        if area is None:
            # No display -- a topology change in progress. The next screen event brings
            # us back here.
            return

        # **Everything in flight is superseded, unconditionally.** A relayout is the new
        # truth about where things go, and an animation started against the previous
        # layout is aiming at a slot that no longer exists.
        #
        # This is not tidiness -- it is the bug. The guard below compares a toast's
        # *current* position against its new target, and during a burst every toast is
        # momentarily at the anchored corner. A toast already sliding toward a stale slot
        # therefore looked "already in the right place", was skipped, and the stale
        # animation then carried it into its neighbour. Which is the reported
        # overlapping-notifications defect, arriving through the animation rather than
        # through the geometry.
        for animation in self._animations.values():
            animation.stop()
        self._animations.clear()

        sizes = [toast.size() for toast in toasts]
        for toast, position in zip(toasts, stack_positions(area, sizes, self._corner, gap=STACK_GAP), strict=True):
            if not toast.isVisible():
                # Placed before the first show, so it never appears at 0,0 and slides
                # into position -- which reads as a glitch rather than as an animation.
                toast.move(position)
                toast.show()
                continue
            if toast.pos() != position:
                self._animate_to(toast, position)

    def _animate_to(self, toast: Toast, position: QPoint) -> None:
        """Slide one toast to `position`. Only ever called from `_relayout`, which has
        already stopped everything that was in flight."""
        key = id(toast)
        animation = QPropertyAnimation(toast, b"pos", self)
        animation.setDuration(_ANIMATION_MS)
        animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        animation.setStartValue(toast.pos())
        animation.setEndValue(position)
        # Kept referenced until it finishes: a `QPropertyAnimation` that goes out of
        # scope is collected mid-flight and the toast stops wherever it happened to be.
        self._animations[key] = animation
        animation.finished.connect(lambda: self._animations.pop(key, None))
        animation.start()

    @property
    def settling(self) -> bool:
        """True while any toast is still sliding to its place.

        Exposed for the tests: the no-overlap property is about the *settled* column,
        and a column measured 10 ms into a 120 ms transition is measuring the animation.
        """
        return bool(self._animations)

    def _anchor_area(self) -> QRect | None:
        """The screen rectangle the column is measured against, or `None` if there is
        no display — which happens mid-topology-change."""
        app = QApplication.instance()
        if not isinstance(app, QApplication):
            return None
        screens = app.screens()
        if not screens:
            return None
        # PySide6's stubs type `primaryScreen()` as non-optional; it does return `None`
        # when there is no primary display. Same shape as `RecordingPill.place` for the
        # same reason -- the check stays, expressed so mypy can see it is reachable.
        primary = app.primaryScreen()
        return screens[0].availableGeometry() if primary is None else primary.availableGeometry()
