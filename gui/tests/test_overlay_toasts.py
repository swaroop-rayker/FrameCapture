"""Toast notifications (M9.6 F2, §20 row 19).

Three layers, tested where each of them lives:

* **the queue** (`toast_model`) — pure, so an error storm of a thousand messages is a
  millisecond rather than a minute of watching a screen;
* **the stack geometry** (`placement.stack_positions`) — pure, so "no two toasts
  overlap" is a property over randomised sequences rather than three hand-picked cases;
* **the widgets** — a real `ToastManager` with the application stylesheet applied, as in
  production. Skipped under the offscreen platform plugin, which has no window handles
  to exclude from capture.

The stylesheet matters here for the reason BUG-051 recorded: a fixture without it
reproduces none of the theme's interactions with the widget, and a regression test
written against such a fixture can be green while the defect is on screen.
"""

from __future__ import annotations

import random
import time
from collections.abc import Iterator

import pytest
from PySide6.QtCore import QCoreApplication, QRect, QSize
from PySide6.QtWidgets import QApplication

from framecapture_gui.overlay.exclusion import WDA_EXCLUDEFROMCAPTURE, ExclusionSupport, affinity_of
from framecapture_gui.overlay.placement import DEFAULT_INSET, STACK_GAP, Corner, stack_positions
from framecapture_gui.overlay.toast_model import (
    COALESCE_WINDOW_S,
    MAX_HELD,
    Severity,
    ToastEntry,
    ToastQueue,
    dwell_seconds,
)
from framecapture_gui.overlay.toasts import Toast, ToastManager
from framecapture_gui.theme import load_stylesheet

_SCREEN = QRect(0, 0, 1920, 1080)


def _settle(manager: ToastManager, timeout_s: float = 2.0) -> None:
    """Pump until the column has finished moving.

    The no-overlap property is about the settled layout. Toasts are *meant* to overlap
    briefly during the 120 ms transition -- a new one lands at the anchored corner while
    the one that was there slides out from under it -- so asserting mid-animation would
    be asserting against the animation rather than the layout.
    """
    deadline = time.monotonic() + timeout_s
    while manager.settling and time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.01)
    QCoreApplication.processEvents()
    assert not manager.settling, "the column never stopped moving"


# ---------------------------------------------------------------------------
# Stack geometry -- the reported defect
# ---------------------------------------------------------------------------


def _rects(sizes: list[QSize], corner: Corner) -> list[QRect]:
    return [QRect(p, s) for p, s in zip(stack_positions(_SCREEN, sizes, corner), sizes, strict=True)]


@pytest.mark.parametrize("corner", list(Corner))
def test_stacked_toasts_never_overlap(corner: Corner) -> None:
    """**The reported bug, as a property.**

    Two hundred randomised sequences of differing heights, in every corner. Hand-picked
    cases would have missed the case that actually produces overlap -- toasts of
    *different* heights, which is what a two-line message against a one-line one gives.
    """
    # Seeded: a failure has to be reproducible, and this is a layout property rather
    # than anything cryptographic.
    rng = random.Random(20260905)  # noqa: S311
    for _ in range(200):
        sizes = [QSize(340, rng.randint(40, 120)) for _ in range(rng.randint(2, 6))]
        rects = _rects(sizes, corner)
        for i, first in enumerate(rects):
            for second in rects[i + 1 :]:
                assert not first.intersects(second), f"{corner}: {first} overlaps {second}"


@pytest.mark.parametrize("corner", list(Corner))
def test_the_whole_column_stays_on_screen(corner: Corner) -> None:
    sizes = [QSize(340, 64) for _ in range(4)]
    for rect in _rects(sizes, corner):
        assert _SCREEN.contains(rect), f"{corner}: {rect} is off-screen"


@pytest.mark.parametrize("corner", list(Corner))
def test_the_newest_is_nearest_the_anchored_corner(corner: Corner) -> None:
    """The column grows *away* from its corner.

    So a new toast appears where the user is already looking and pushes the older ones
    aside, rather than arriving at the far end of a column they have to scan.
    """
    sizes = [QSize(340, 64) for _ in range(3)]
    rects = _rects(sizes, corner)

    if corner in (Corner.TOP_LEFT, Corner.TOP_RIGHT):
        assert rects[0].top() < rects[1].top() < rects[2].top()
    else:
        assert rects[0].top() > rects[1].top() > rects[2].top()


@pytest.mark.parametrize("corner", list(Corner))
def test_the_column_is_inset_from_its_corner(corner: Corner) -> None:
    rects = _rects([QSize(340, 64)], corner)
    rect = rects[0]
    if corner in (Corner.TOP_LEFT, Corner.BOTTOM_LEFT):
        assert rect.left() == _SCREEN.left() + DEFAULT_INSET
    else:
        assert rect.right() == _SCREEN.right() - DEFAULT_INSET

    if corner in (Corner.TOP_LEFT, Corner.TOP_RIGHT):
        assert rect.top() == _SCREEN.top() + DEFAULT_INSET
    else:
        assert rect.bottom() == _SCREEN.bottom() - DEFAULT_INSET


def test_differing_heights_are_accumulated_not_assumed() -> None:
    """Offsets come from the actual sizes.

    Assuming a uniform row height is the other way a column overlaps: one two-line
    message and everything below it is wrong by a line.
    """
    sizes = [QSize(340, 40), QSize(340, 100), QSize(340, 40)]
    rects = _rects(sizes, Corner.TOP_RIGHT)
    assert rects[1].top() == rects[0].bottom() + 1 + STACK_GAP
    assert rects[2].top() == rects[1].bottom() + 1 + STACK_GAP


def test_an_empty_column_is_not_an_error() -> None:
    assert stack_positions(_SCREEN, [], Corner.TOP_RIGHT) == []


# ---------------------------------------------------------------------------
# The queue: bursts, bounds, coalescing
# ---------------------------------------------------------------------------


def _entry(text: str = "boom", severity: Severity = Severity.ERROR) -> ToastEntry:
    return ToastEntry(severity=severity, text=text)


def test_an_error_storm_does_not_grow_without_bound() -> None:
    """CLAUDE.md hard rule 5 does not stop at the process boundary.

    An engine emitting an error per frame would otherwise turn a recording problem into
    a memory problem in the GUI.
    """
    queue = ToastQueue(max_visible=4)
    for i in range(1000):
        # Distinct texts, so coalescing cannot be what keeps the number down -- the
        # bound is what is under test here, not the deduplication.
        queue.add(_entry(f"failure {i}"), now=float(i))

    assert len(queue) == MAX_HELD
    assert queue.dropped == 1000 - MAX_HELD


def test_the_storm_keeps_the_newest_messages() -> None:
    """Drop-**oldest**, unlike the command queue, which drops the newest.

    The items differ: a queued command is something the user asked for and still wants;
    a message from thirty errors ago has been superseded by the twenty-nine after it.
    """
    queue = ToastQueue(max_visible=4)
    for i in range(100):
        queue.add(_entry(f"failure {i}"), now=float(i))

    texts = [entry.text for entry in queue.entries]
    assert texts[0] == "failure 99"
    assert "failure 0" not in texts


def test_repeats_inside_the_window_are_counted_not_stacked() -> None:
    """The burst `main_window._on_notified` already documents.

    A failed engine start emits an error, the OFFLINE transition prompts a refresh that
    fails and emits another. Three windows saying the same sentence is noise; one saying
    it with a counter is information.
    """
    queue = ToastQueue()
    first = queue.add(_entry("the engine stopped answering"), now=0.0)
    again = queue.add(_entry("the engine stopped answering"), now=0.4)

    assert again is first, "a repeat created a second entry"
    assert first.count == 2
    assert len(queue) == 1


def test_a_repeat_after_the_window_is_a_new_message() -> None:
    """Someone who presses pause twice in five seconds meant it twice."""
    queue = ToastQueue()
    queue.add(_entry("Recording paused", Severity.INFO), now=0.0)
    queue.add(_entry("Recording paused", Severity.INFO), now=COALESCE_WINDOW_S + 0.1)
    assert len(queue) == 2


def test_coalescing_distinguishes_severity() -> None:
    queue = ToastQueue()
    queue.add(ToastEntry(severity=Severity.WARNING, text="disk is slow"), now=0.0)
    queue.add(ToastEntry(severity=Severity.ERROR, text="disk is slow"), now=0.1)
    assert len(queue) == 2


def test_a_repeat_returns_to_the_front() -> None:
    """A message that keeps arriving should not sink under quieter ones."""
    queue = ToastQueue()
    first = queue.add(_entry("first"), now=0.0)
    queue.add(_entry("second"), now=0.1)
    queue.add(_entry("first"), now=0.2)

    assert queue.entries[0] is first
    assert first.count == 2


# ---------------------------------------------------------------------------
# Dwell
# ---------------------------------------------------------------------------


def test_an_error_stays_until_dismissed() -> None:
    """A message that disappears before it is read is a message that did not happen,
    and this is the class a user must not miss (CLAUDE.md §1)."""
    assert dwell_seconds(Severity.ERROR, 4) is None


def test_a_warning_outlasts_an_informational_message() -> None:
    info = dwell_seconds(Severity.INFO, 4)
    warning = dwell_seconds(Severity.WARNING, 4)
    assert info is not None and warning is not None
    assert warning > info


def test_the_informational_dwell_is_the_configured_number() -> None:
    """CONFIG.md defines `toast_duration_s` as the informational dwell, and the number
    the others scale from. If that stops being literally true the documentation is
    wrong."""
    assert dwell_seconds(Severity.INFO, 9) == 9.0


def test_only_visible_toasts_age() -> None:
    """A message still waiting for a slot has not been seen.

    Expiring it unseen would be the same as dropping it, with the added dishonesty of
    having queued it first.
    """
    queue = ToastQueue(max_visible=1)
    queue.add(_entry("first", Severity.INFO), now=0.0)
    queue.add(_entry("second", Severity.INFO), now=0.0)

    expired = queue.expire(base_seconds=1, now=5.0)
    assert [e.text for e in expired] == ["second"], "the queued message aged while off-screen"
    assert len(queue) == 1


def test_expiry_promotes_the_next_message() -> None:
    queue = ToastQueue(max_visible=1)
    queue.add(_entry("older", Severity.INFO), now=0.0)
    queue.add(_entry("newer", Severity.INFO), now=0.0)

    queue.expire(base_seconds=1, now=5.0)
    assert [e.text for e in queue.visible] == ["older"]


# ---------------------------------------------------------------------------
# The widgets
# ---------------------------------------------------------------------------


@pytest.fixture
def manager(qapp: QCoreApplication) -> Iterator[ToastManager]:
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())
    probe = Toast(_entry("probe"))
    handle = int(probe.winId())
    probe.deleteLater()
    if not handle:
        pytest.skip("no native window handle (offscreen platform plugin)")

    toasts = ToastManager(corner=Corner.TOP_RIGHT, max_visible=4, duration_s=4)
    try:
        yield toasts
    finally:
        toasts.clear()
        toasts.deleteLater()
        QCoreApplication.processEvents()


def test_a_toast_is_excluded_from_capture(manager: ToastManager) -> None:
    """Rule A, for the second overlay surface.

    The whole reason `overlay.surface.prepare` exists: two call sites is how "the pill is
    excluded and the toast is not" ships.
    """
    manager.post(Severity.INFO, "Recording paused")
    QCoreApplication.processEvents()

    toasts = manager.visible_toasts()
    assert toasts, "no toast was shown"
    for toast in toasts:
        assert toast.exclusion is ExclusionSupport.EXCLUDED
        assert affinity_of(int(toast.winId())) == WDA_EXCLUDEFROMCAPTURE


def test_a_toast_does_not_accept_focus(manager: ToastManager) -> None:
    """Same rule as the pill: a notification must not pull a fullscreen game out of
    focus, and appearing unbidden makes that worse than a click would."""
    from PySide6.QtCore import Qt

    manager.post(Severity.INFO, "Recording paused")
    QCoreApplication.processEvents()
    toast = manager.visible_toasts()[0]

    assert bool(toast.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus)
    assert toast.testAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)


def test_no_two_visible_toasts_overlap_on_screen(manager: ToastManager) -> None:
    """The property again, this time against real widgets and their real sizes."""
    manager.post(Severity.INFO, "Recording paused")
    manager.post(Severity.WARNING, "Disk throughput is low")
    manager.post(Severity.ERROR, "Encoder failed", detail="a longer explanation that wraps onto another line")
    _settle(manager)

    rects = [t.geometry() for t in manager.visible_toasts()]
    assert len(rects) == 3
    for i, first in enumerate(rects):
        for second in rects[i + 1 :]:
            assert not first.intersects(second), f"{first} overlaps {second}"


def test_a_burst_with_no_event_processing_still_settles_correctly(manager: ToastManager) -> None:
    """**The bug rendering the column found and every earlier test missed.**

    Posting back-to-back with no event loop in between is the realistic case -- an error
    burst, or a stop that saves and then reports -- and it is the case that breaks. While
    a burst is in flight every toast is momentarily at the anchored corner, so a toast
    already sliding toward a *stale* slot compares equal to its new target, gets skipped,
    and the stale animation then carries it into its neighbour.

    The earlier tests all pumped between posts, which hid it. This one does not, and it
    asserts the settled positions rather than merely the absence of overlap -- an overlap
    check alone would pass on a column that settled one slot down as a whole.
    """
    manager.post(Severity.INFO, "first")
    manager.post(Severity.INFO, "second", detail="with a detail line that makes it taller")
    manager.post(Severity.WARNING, "third")
    manager.post(Severity.ERROR, "fourth", detail="another taller one")
    # A coalesced repeat as the last event, which reorders the column *and* is the case
    # that reproduced the defect: it relayouts while four animations are still running.
    manager.post(Severity.WARNING, "third")

    _settle(manager)

    toasts = manager.visible_toasts()
    sizes = [t.size() for t in toasts]
    area = manager._anchor_area()
    assert area is not None, "no display to anchor against"
    expected = stack_positions(area, sizes, Corner.TOP_RIGHT)

    for toast, position in zip(toasts, expected, strict=True):
        assert toast.pos() == position, (
            f"{toast.entry.text!r} settled at {toast.pos()} instead of {position} -- "
            "an animation from a superseded layout finished last"
        )

    rects = [t.geometry() for t in toasts]
    for i, first in enumerate(rects):
        for second in rects[i + 1 :]:
            assert not first.intersects(second), f"{first} overlaps {second}"


def test_only_max_visible_are_on_screen(manager: ToastManager) -> None:
    for i in range(9):
        manager.post(Severity.WARNING, f"warning {i}")
    _settle(manager)
    assert len(manager.visible_toasts()) == 4


def test_a_repeat_updates_the_existing_toast(manager: ToastManager) -> None:
    manager.post(Severity.ERROR, "the engine stopped answering")
    manager.post(Severity.ERROR, "the engine stopped answering")
    QCoreApplication.processEvents()

    toasts = manager.visible_toasts()
    assert len(toasts) == 1, "a repeat opened a second window"
    assert toasts[0].entry.count == 2


def test_dismissing_removes_it_and_reflows_the_rest(manager: ToastManager) -> None:
    manager.post(Severity.WARNING, "first")
    second = manager.post(Severity.WARNING, "second")
    manager.post(Severity.WARNING, "third")
    QCoreApplication.processEvents()
    assert second is not None

    manager.dismiss(second)
    _settle(manager)

    texts = [t.entry.text for t in manager.visible_toasts()]
    assert "second" not in texts
    assert len(texts) == 2


def test_toasts_disabled_shows_nothing(manager: ToastManager) -> None:
    manager.configure(corner=Corner.TOP_RIGHT, max_visible=4, duration_s=4, enabled=False)
    assert manager.post(Severity.ERROR, "an error") is None
    QCoreApplication.processEvents()
    assert manager.visible_toasts() == []


def test_an_empty_message_is_not_shown(manager: ToastManager) -> None:
    """`EngineController` can emit an empty warning body; `_on_notified` already guards
    it, and the column should not depend on that guard staying there."""
    assert manager.post(Severity.WARNING, "") is None


def test_the_toast_body_has_no_opaque_rectangle_in_it(manager: ToastManager) -> None:
    """BUG-051's defect, checked on the surface it was not found on.

    The universal `QWidget { background-color: @bg-base; }` reaches every child of every
    self-painted overlay. The pill was caught by a photograph; this one is caught here
    instead. Two pixels, no font, no layout: the background must be uniform.
    """
    manager.post(Severity.INFO, "Recording paused")
    QCoreApplication.processEvents()
    toast = manager.visible_toasts()[0]

    image = toast.grab().toImage()
    row = toast.height() // 2
    # Right of the text, inside the body; and the padding on the far right edge.
    inside = image.pixelColor(toast.width() - 30, row)
    body = image.pixelColor(toast.width() - 4, row)
    assert inside == body, f"an opaque rectangle is painted inside the toast: {inside.name()} vs {body.name()}"
