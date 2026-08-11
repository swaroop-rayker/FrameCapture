"""A test-owned animated window, for the pytest cases that drive a real recording.

CLAUDE.md §5: "Tests use the synthetic source, never the real desktop. Tests that depend
on what happens to be on screen are not tests." BUG-033 established that for the GPU tier
and gave it `tests/fixtures/screen_animator.h`. The pytest engine cases were left out,
and they are the ones that cannot use the synthetic source at all: they drive the engine
over IPC, and `start_record` captures a display -- there is no seam to inject a source
through, which is the whole point of testing at that level.

So they captured whatever happened to be on screen. **Measured, on this rig:**
``test_stopping_while_paused_still_yields_a_valid_file`` records for one second and then
asserts the file is valid. With a window animating it passes. With an idle desktop it
finalizes ``decoded_frames=1, duration_s=0.017`` and the §10.4 validation gate correctly
rejects it -- reproducibly, both ways. The engine was right every time; the test was
reporting on the desktop.

That is not flakiness to be retried. WGC composites only when the screen *changes*, so a
static desktop produces exactly one frame however long you wait, and no timeout or retry
fixes it (`screen_animator.h` says the same thing at more length).

This is deliberately smaller than the C++ fixture. That one *covers* the target output,
because those tests assert on decoded pixel content and must own every pixel. These
assert only that a recording of the expected length happened, so all that is needed is
for the screen to keep changing -- a small window is enough to make DWM composite, and it
stays out of the way of whoever is at the machine.
"""

from __future__ import annotations

import contextlib
from collections.abc import Iterator

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QPainter, QPaintEvent
from PySide6.QtWidgets import QApplication, QWidget


class _Blinker(QWidget):
    """Repaints itself in a different colour on every tick."""

    def __init__(self) -> None:
        super().__init__(
            None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setGeometry(0, 0, 160, 120)
        self._phase = 0
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._advance)

    def start(self, interval_ms: int) -> None:
        self.show()
        self._timer.start(interval_ms)

    def stop(self) -> None:
        self._timer.stop()
        self.hide()

    def _advance(self) -> None:
        self._phase = (self._phase + 1) % 360
        # `update()` alone can be coalesced away; `repaint()` guarantees the frame that
        # guarantees the composite, which is the entire purpose of this class.
        self.repaint()

    def paintEvent(self, event: QPaintEvent) -> None:  # noqa: N802  (Qt's spelling)
        del event
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor.fromHsv(self._phase, 200, 200))


@contextlib.contextmanager
def screen_activity(interval_ms: int = 33) -> Iterator[None]:
    """Keeps the screen changing for the duration of the block.

    A no-op when there is no GUI application (a headless run has no display to animate
    and no real recording to make either), so importing this never forces a QApplication
    into existence.
    """
    app = QApplication.instance()
    if not isinstance(app, QApplication):
        yield
        return

    blinker = _Blinker()
    blinker.start(interval_ms)
    # One pass so the window exists and has been composited before the caller starts
    # recording; otherwise the first frames are of the desktop underneath it.
    for _ in range(10):
        app.processEvents()
    try:
        yield
    finally:
        blinker.stop()
        app.processEvents()
        blinker.deleteLater()
        app.processEvents()
