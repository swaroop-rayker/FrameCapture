"""SPEC.md §20 row 20's named test: the overlay stays responsive while saving.

Row 20's symptom is *"overlay button unresponsive, or clicking it minimizes a fullscreen
game"*, and its root causes are a GUI thread blocked inside a synchronous IPC request and
an overlay window that takes activation. The mechanisms are M9.6 §1.2's async commands and
`WS_EX_NOACTIVATE`; this file is where the mechanisms are checked against the symptom.

**What is here and what is next door.** The async command path itself — that
`stop_record` no longer holds the GUI thread — is `test_async_commands.py`, and its
engine-backed case measures the event loop running during a real finalization. That
proves the *cause* was removed. This file proves the *effect* the user experiences: the
pill repaints while a save is in flight, and a click on it is acknowledged within a
frame. A blocked loop and a widget that never asked to repaint produce the same frozen
pill, so both halves are worth their own assertions.

The stop here is mocked rather than real, deliberately. What is being measured is the
widget's repaint behaviour given a stream of `finalize_progress` events, and driving that
from a real 1.2 GB recording would make a UI test depend on a disk. The real cadence is
measured against a real engine in `test_async_commands.py`.

**Measurements, not adjectives** (CLAUDE.md §6). The paint-cost and click-latency numbers
this file prints are what `ACCEPTANCE.md` records for row 20; the assertions are loose
floors around them so the test fails on a regression rather than on a busy machine.
"""

from __future__ import annotations

import time
from collections.abc import Iterator
from typing import Any

import pytest
from PySide6.QtCore import QCoreApplication, QElapsedTimer, QEvent, QObject
from PySide6.QtWidgets import QApplication

from framecapture_gui.ipc.protocol import RecordingState
from framecapture_gui.overlay.pill import RecordingPill
from framecapture_gui.theme import load_stylesheet

#: SPEC.md §16.2's finalize-progress cadence. The engine emits at this rate, so it is the
#: rate the pill is fed at here.
_PROGRESS_HZ = 10.0

#: How long the mocked save runs. The plan says five seconds; two is used because the
#: property under test is a *rate* and two seconds at 10 Hz is twenty updates, which
#: establishes it just as well while keeping the suite quick. The duration is a constant
#: rather than a literal so the long form is one edit away.
_SAVE_SECONDS = 2.0

#: §20 row 20's floor: at least eight paints per second during a save. Below this the bar
#: visibly steps rather than moves, which is what "unresponsive" looks like before it
#: looks frozen.
_MIN_PAINTS_PER_SECOND = 8.0

#: One frame at 60 Hz. A click acknowledged more slowly than this is a control that looks
#: unpressed, however fast the round trip behind it turns out to be.
_ONE_FRAME_MS = 1000.0 / 60.0


class _PaintCounter(QObject):
    """Counts paint events on a widget.

    An event filter rather than a `paintEvent` override, because counting does not need
    to intercept the drawing and the subject should stay the real `RecordingPill`.

    **It counts and does not time.** The first version of this class also timed each
    event, and the numbers it produced — 0.002 ms for a repaint — were nonsense: an event
    filter runs *before* the widget handles the event, so what was timed was the filter
    returning, not the pill drawing. Paint cost is measured by `_TimedPill` below, which
    is the only place that can see the real thing.
    """

    def __init__(self) -> None:
        super().__init__()
        self.paints = 0

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 -- Qt's spelling
        if event.type() == QEvent.Type.Paint:
            self.paints += 1
        return super().eventFilter(watched, event)


class _TimedPill(RecordingPill):
    """A pill that records how long each of its own repaints takes.

    Overriding `paintEvent` and timing `super().paintEvent(event)` measures the real
    widget's drawing, because the drawing *is* the superclass call. Nothing else is
    overridden, so this is a `RecordingPill` in every other respect.
    """

    def __init__(self) -> None:
        super().__init__()
        self.durations_ms: list[float] = []

    def paintEvent(self, event: Any) -> None:  # noqa: N802 -- Qt's spelling
        elapsed = QElapsedTimer()
        elapsed.start()
        super().paintEvent(event)
        self.durations_ms.append(elapsed.nsecsElapsed() / 1e6)

    @property
    def mean_ms(self) -> float:
        return sum(self.durations_ms) / len(self.durations_ms) if self.durations_ms else 0.0

    @property
    def worst_ms(self) -> float:
        return max(self.durations_ms) if self.durations_ms else 0.0


@pytest.fixture
def pill(qtbot: Any, qapp: QCoreApplication) -> Iterator[RecordingPill]:
    """A real pill with the theme applied (M9_6_CHANGES.md §5, lesson 2)."""
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())
    widget = RecordingPill()
    if not int(widget.winId()):
        widget.deleteLater()
        pytest.skip("no native window handle (offscreen platform plugin)")
    qtbot.addWidget(widget)
    widget.show()
    QCoreApplication.processEvents()
    try:
        yield widget
    finally:
        widget.close()


def _progress(percent: int, phase: str = "remuxing") -> dict[str, Any]:
    """One `finalize_progress` body, in the shape §2.1 defines."""
    return {
        "phase": phase,
        "percent": percent,
        "bytes_done": percent * 12_884_901,
        "bytes_total": 1_288_490_188,
        "output": "capture.mp4",
    }


# ---------------------------------------------------------------------------
# Row 20: the pill repaints while a save is in flight
# ---------------------------------------------------------------------------


def test_every_progress_update_reaches_the_screen(pill: RecordingPill, qtbot: Any) -> None:
    """Deterministic, and the one that must never flake.

    Fifty updates through a running event loop. Qt coalesces repaint requests, so an
    exact one-to-one is not the property — what matters is that the widget is not
    *stuck*, which a blocked loop or a missing `update()` would both produce as a paint
    count near zero.
    """
    counter = _PaintCounter()
    pill.installEventFilter(counter)
    pill.apply_state(RecordingState.STOPPING)
    QCoreApplication.processEvents()

    updates = 50
    counter.paints = 0
    for step in range(updates):
        pill.apply_finalize_progress(_progress(step * 2))
        QCoreApplication.processEvents()

    assert counter.paints >= updates * 0.8, f"{counter.paints} paints for {updates} updates"


def test_the_pill_repaints_at_least_eight_times_a_second_while_saving(
    pill: RecordingPill, qtbot: Any, record_property: Any
) -> None:
    """§20 row 20's floor, measured against a mocked save at the engine's real cadence.

    The assertion is a floor rather than a target: what is being proven is that the loop
    is alive and the widget is asking to be drawn. A machine under load paints fewer
    times, and a strict count would be measuring the machine.
    """
    counter = _PaintCounter()
    pill.installEventFilter(counter)
    pill.apply_state(RecordingState.STOPPING)
    QCoreApplication.processEvents()

    counter.paints = 0
    interval = 1.0 / _PROGRESS_HZ
    started = time.monotonic()
    percent = 0
    while time.monotonic() - started < _SAVE_SECONDS:
        percent = min(99, percent + 1)
        pill.apply_finalize_progress(_progress(percent))
        deadline = time.monotonic() + interval
        while time.monotonic() < deadline:
            QCoreApplication.processEvents()
            time.sleep(0.002)

    elapsed = time.monotonic() - started
    rate = counter.paints / elapsed

    # Recorded so ACCEPTANCE.md quotes a measurement rather than a claim.
    record_property("paints_per_second", round(rate, 1))
    print(  # noqa: T201 -- the measurement is the point of this test
        f"\n[ MEASURED ] {counter.paints} paints in {elapsed:.2f} s = {rate:.1f} paints/s"
    )

    assert rate >= _MIN_PAINTS_PER_SECOND, f"{rate:.1f} paints/s"


def test_a_paint_costs_far_less_than_a_frame(qtbot: Any, qapp: QCoreApplication, record_property: Any) -> None:
    """The §16.1 budget question, at the level this test can answer it.

    §16.1 caps GUI CPU at 3% while recording, which is a process-wide number this test
    cannot produce — that one needs a real recording and a sampler, and `ACCEPTANCE.md`
    says so rather than pretending otherwise. What this can produce is the cost of the
    thing M9.6 added: one pill repaint. At 10 Hz a paint costing a whole 60 Hz frame
    would make the overlay the reason the budget was missed.

    Uses `_TimedPill` rather than the shared fixture, because only a subclass can time
    the superclass's drawing.
    """
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())
    widget = _TimedPill()
    if not int(widget.winId()):
        widget.deleteLater()
        pytest.skip("no native window handle (offscreen platform plugin)")
    qtbot.addWidget(widget)
    widget.show()
    QCoreApplication.processEvents()

    try:
        widget.apply_state(RecordingState.RECORDING)
        widget.durations_ms.clear()
        for step in range(100):
            widget.apply_stats({"timeline_ms": step * 100, "bytes_written": step * 1_000_000})
            widget.repaint()  # synchronous, so every iteration produces one real paint
            QCoreApplication.processEvents()

        assert widget.durations_ms, "the pill never painted"
        record_property("paint_mean_ms", round(widget.mean_ms, 3))
        record_property("paint_worst_ms", round(widget.worst_ms, 3))
        print(  # noqa: T201 -- the measurement is the point of this test
            f"\n[ MEASURED ] {len(widget.durations_ms)} real paints while recording: "
            f"mean {widget.mean_ms:.3f} ms, worst {widget.worst_ms:.3f} ms"
        )
        assert widget.mean_ms < _ONE_FRAME_MS, f"mean paint {widget.mean_ms:.3f} ms"
    finally:
        widget.close()


# ---------------------------------------------------------------------------
# Row 20: a click is acknowledged within a frame
# ---------------------------------------------------------------------------


def test_the_stop_button_acknowledges_the_click_before_the_engine_answers(
    pill: RecordingPill, record_property: Any
) -> None:
    """The half of row 20 that is not about the event loop at all.

    Even with a perfectly responsive loop, a button that waits for the engine before
    changing appearance looks broken for the length of the round trip. The pill switches
    to its saving page on the click itself, so the acknowledgement cannot be slower than
    the round trip — it does not depend on it.

    Measured with no engine attached: nothing answers the signal, so what is timed is
    purely the local acknowledgement.
    """
    pill.apply_state(RecordingState.RECORDING)
    QCoreApplication.processEvents()
    assert pill._pages.currentIndex() == 0, "the pill did not start on its live page"

    emitted: list[bool] = []
    pill.stop_requested.connect(lambda: emitted.append(True))

    elapsed = QElapsedTimer()
    elapsed.start()
    pill._on_stop_clicked()
    latency_ms = elapsed.nsecsElapsed() / 1e6

    record_property("click_to_visual_ms", round(latency_ms, 3))
    print(f"\n[ MEASURED ] click to visual state: {latency_ms:.3f} ms")  # noqa: T201

    assert emitted == [True], "the click did not reach the engine"
    assert pill._pages.currentIndex() == 1, "the pill did not switch to saving on the click"
    assert not pill._stop.isEnabled(), "the stop button stayed enabled after being pressed"
    assert latency_ms < _ONE_FRAME_MS, f"{latency_ms:.3f} ms to acknowledge a click"


def test_the_pause_button_acknowledges_the_click_before_the_engine_answers(
    pill: RecordingPill, record_property: Any
) -> None:
    """The same property for pause, which is the one pressed mid-recording."""
    pill.apply_state(RecordingState.RECORDING)
    QCoreApplication.processEvents()

    emitted: list[str] = []
    pill.pause_requested.connect(lambda: emitted.append("pause"))

    elapsed = QElapsedTimer()
    elapsed.start()
    pill._on_pause_clicked()
    latency_ms = elapsed.nsecsElapsed() / 1e6

    record_property("click_to_visual_ms", round(latency_ms, 3))
    print(f"\n[ MEASURED ] pause click to visual state: {latency_ms:.3f} ms")  # noqa: T201

    assert emitted == ["pause"]
    assert not pill._pause.isEnabled(), "the pause button stayed enabled after being pressed"
    assert latency_ms < _ONE_FRAME_MS, f"{latency_ms:.3f} ms to acknowledge a click"


# ---------------------------------------------------------------------------
# Row 20's other root cause: the overlay must not take activation
# ---------------------------------------------------------------------------


def test_the_pill_does_not_accept_focus(pill: RecordingPill) -> None:
    """Row 20's second clause: "…or clicking it minimizes a fullscreen game".

    A normal window clicked over an exclusive-fullscreen game takes activation, and
    Windows minimizes the game. `WS_EX_NOACTIVATE` plus
    `Qt.WindowDoesNotAcceptFocus` is what stops that, and it is invisible in every other
    test in this suite because nothing else has a fullscreen game to lose.
    """
    from PySide6.QtCore import Qt

    assert pill.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus
    assert pill.focusPolicy() == Qt.FocusPolicy.NoFocus
