"""The floating recording pill (M9.6 F1, §20 rows 19-22).

Split by what needs a window. `ElapsedClock` and the placement geometry are pure and are
tested directly; the widget's behaviour is tested against a real `RecordingPill`, which
needs a native window and so skips under the offscreen platform plugin.

What is asserted here is **state**, not pixels. A screenshot comparison would pin the
theme to one font-rendering stack and break on every machine that is not the one it was
recorded on -- the same reasoning `test_main_window` gives.
"""

from __future__ import annotations

import time
from collections.abc import Iterator

import pytest
from PySide6.QtCore import QCoreApplication, QPoint, QRect, QSize, Qt
from PySide6.QtWidgets import QApplication

from framecapture_gui.ipc.protocol import RecordingState
from framecapture_gui.overlay.elapsed import ElapsedClock
from framecapture_gui.overlay.exclusion import WDA_EXCLUDEFROMCAPTURE, ExclusionSupport, affinity_of
from framecapture_gui.overlay.pill import RecordingPill
from framecapture_gui.overlay.placement import (
    DEFAULT_INSET,
    Corner,
    clamp_to_screens,
    corner_position,
    snap_to_edges,
)
from framecapture_gui.theme import load_stylesheet

# ---------------------------------------------------------------------------
# The clock (SPEC.md §7.5, §16.2)
# ---------------------------------------------------------------------------


def test_the_clock_shows_the_engines_figure_immediately() -> None:
    clock = ElapsedClock()
    clock.sync(14 * 60_000 + 22_000, advancing=False)
    assert clock.elapsed_ms() == 862_000


def test_the_clock_advances_between_updates() -> None:
    """The reason it interpolates: `stats` is 2 Hz, and a clock redrawn only on those
    ticks visibly jumps half a second at a time."""
    clock = ElapsedClock()
    clock.sync(10_000, advancing=True)
    time.sleep(0.15)
    assert clock.elapsed_ms() > 10_000


def test_the_clock_holds_while_paused() -> None:
    """SPEC.md §7.5 excises paused time from the file.

    A pill that kept counting through a pause would be promising footage the recording
    does not contain -- §16.5's silent-footage-loss, told by a clock instead of a colour.
    """
    clock = ElapsedClock()
    clock.sync(10_000, advancing=False)
    time.sleep(0.15)
    assert clock.elapsed_ms() == 10_000


def test_the_engines_figure_always_wins() -> None:
    """Interpolation fills the gaps; it never overrides.

    If the local clock has run ahead -- a slow frame, a stalled timer -- the next `stats`
    pulls it back. Otherwise the displayed length drifts away from the file's real one,
    which is exactly the lie the interpolation exists to avoid.
    """
    clock = ElapsedClock()
    clock.sync(60_000, advancing=True)
    time.sleep(0.2)
    assert clock.elapsed_ms() > 60_000

    clock.sync(60_100, advancing=True)
    assert clock.elapsed_ms() < 60_200


def test_hold_freezes_at_what_was_displayed() -> None:
    clock = ElapsedClock()
    clock.sync(30_000, advancing=True)
    time.sleep(0.12)
    frozen = clock.elapsed_ms()
    clock.hold()
    time.sleep(0.12)
    assert clock.elapsed_ms() == frozen


# ---------------------------------------------------------------------------
# Placement (M9.6 §3.3)
# ---------------------------------------------------------------------------

_SCREEN = QRect(0, 0, 1920, 1080)
_PILL = QSize(320, 44)


@pytest.mark.parametrize(
    ("corner", "expected"),
    [
        (Corner.TOP_LEFT, QPoint(DEFAULT_INSET, DEFAULT_INSET)),
        (Corner.TOP_RIGHT, QPoint(1920 - 320 - DEFAULT_INSET, DEFAULT_INSET)),
        (Corner.BOTTOM_LEFT, QPoint(DEFAULT_INSET, 1080 - 44 - DEFAULT_INSET)),
        (Corner.BOTTOM_RIGHT, QPoint(1920 - 320 - DEFAULT_INSET, 1080 - 44 - DEFAULT_INSET)),
    ],
)
def test_each_corner_anchors_where_it_says(corner: Corner, expected: QPoint) -> None:
    assert corner_position(_SCREEN, _PILL, corner) == expected


def test_a_position_already_on_screen_is_left_alone() -> None:
    """Including one deliberately hanging off the edge.

    The rule is reachability, not tidiness: a user who parked it half off the right edge
    should find it where they put it.
    """
    parked = QPoint(1800, 500)
    assert clamp_to_screens(parked, _PILL, [_SCREEN]) == parked


def test_a_position_on_a_monitor_that_is_gone_comes_back() -> None:
    """**The failure this exists for.**

    A pill remembered at x=3000 on a second monitor that has been unplugged is a pill the
    user cannot see -- and the stop button goes with it. The recording keeps running with
    no reachable control, and the only way out is killing the application.
    """
    remembered = QPoint(3000, 400)
    clamped = clamp_to_screens(remembered, _PILL, [_SCREEN])

    assert clamped != remembered
    assert _SCREEN.contains(QRect(clamped, _PILL)), f"{clamped} is still off-screen"


def test_it_returns_to_the_nearest_screen_not_the_first() -> None:
    """With the pill remembered on a monitor that has just gone, the *adjacent* monitor
    is where the user will look for it."""
    left = QRect(0, 0, 1920, 1080)
    right = QRect(1920, 0, 1920, 1080)
    # Off the far right of the right-hand screen.
    clamped = clamp_to_screens(QPoint(4200, 500), _PILL, [left, right])
    assert right.contains(QRect(clamped, _PILL))


def test_no_screens_at_all_leaves_the_position_alone() -> None:
    """Happens during a display topology change. Inventing a coordinate would be worse
    than waiting for the next screen-change event."""
    position = QPoint(100, 100)
    assert clamp_to_screens(position, _PILL, []) == position


def test_a_drag_near_an_edge_snaps_flush() -> None:
    snapped = snap_to_edges(QPoint(6, 500), _PILL, _SCREEN)
    assert snapped.x() == 0

    snapped = snap_to_edges(QPoint(1920 - 320 - 5, 500), _PILL, _SCREEN)
    assert snapped.x() == 1920 - 320


def test_a_drag_toward_a_corner_snaps_on_both_axes() -> None:
    """Per axis independently, so a corner drag does not have to pick one edge."""
    snapped = snap_to_edges(QPoint(4, 1080 - 44 - 4), _PILL, _SCREEN)
    assert snapped == QPoint(0, 1080 - 44)


def test_a_drag_in_open_space_does_not_snap() -> None:
    loose = QPoint(800, 500)
    assert snap_to_edges(loose, _PILL, _SCREEN) == loose


# ---------------------------------------------------------------------------
# The widget
# ---------------------------------------------------------------------------


@pytest.fixture
def pill(qapp: QCoreApplication) -> Iterator[RecordingPill]:
    """A pill **with the application stylesheet applied**, as in production.

    Applying it is not decoration. `theme_dark.qss` opens with a universal
    `QWidget { background-color: @bg-base; }`, and that rule is the entire cause of the
    opaque-rectangle defect below. A fixture without the stylesheet reproduces none of
    the theme's interactions with this widget, and the regression test for that defect
    passed with the fix reverted until this line existed.
    """
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())
    widget = RecordingPill()
    if not int(widget.winId()):
        widget.deleteLater()
        pytest.skip("no native window handle (offscreen platform plugin)")
    try:
        yield widget
    finally:
        widget.close()
        widget.deleteLater()


def test_the_pill_is_excluded_from_capture(pill: RecordingPill) -> None:
    """Rule A, at the widget. The frame-level proof is `test_overlay_exclusion.cpp`."""
    assert pill.exclusion is ExclusionSupport.EXCLUDED
    assert affinity_of(int(pill.winId())) == WDA_EXCLUDEFROMCAPTURE


def test_the_pill_does_not_accept_focus(pill: RecordingPill) -> None:
    """Clicking pause must not activate the pill.

    Activating any window over a fullscreen-exclusive Direct3D application minimizes it —
    so a click that paused the recording *and* minimized the game would be the worst
    outcome this feature can produce.
    """
    assert bool(pill.windowFlags() & Qt.WindowType.WindowDoesNotAcceptFocus)
    assert pill.testAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)


@pytest.mark.parametrize(
    ("state", "visible"),
    [
        (RecordingState.RECORDING, True),
        (RecordingState.PAUSED, True),
        (RecordingState.IDLE, False),
        (RecordingState.OFFLINE, False),
        (RecordingState.FAULTED, False),
    ],
)
def test_the_pill_is_on_screen_exactly_while_there_is_a_recording(
    pill: RecordingPill, state: RecordingState, visible: bool
) -> None:
    pill.apply_state(state)
    QCoreApplication.processEvents()
    assert pill.isVisible() is visible


def test_the_buttons_keep_their_shared_styling(pill: RecordingPill) -> None:
    """A variant is a property, never a second `objectName`.

    `setObjectName` *replaces* the name. Setting `"PillStop"` on the stop button made it
    stop matching `#PillButton`, so it lost its background, border and radius and
    rendered as a flat dark square with no red. Every state assertion still passed —
    the defect was only visible by looking at it, which is why this asserts the
    mechanism rather than the appearance.
    """
    assert pill._stop.objectName() == "PillButton"
    assert pill._stop.property("variant") == "stop"
    assert pill._pause.objectName() == "PillButton"


def test_the_failed_phase_keeps_its_label_styling(pill: RecordingPill) -> None:
    """Same trap, same fix: a state property rather than a replacement objectName."""
    pill.apply_state(RecordingState.STOPPING)
    pill.apply_finalized({"valid": False, "detail": "short", "output": "r.mkv"})

    assert pill._phase.objectName() == "PillPhase"
    assert pill._phase.property("state") == "failed"


def test_the_pill_body_has_no_opaque_rectangle_in_it(pill: RecordingPill) -> None:
    """A reported visual defect: a dark square block across the middle of the pill.

    `theme_dark.qss` opens with a universal `QWidget { background-color: @bg-base; }`.
    That applies to the pill's `QStackedWidget` and its pages, and `bg-base` is darker
    than the `bg-surface` the pill paints itself — so the stack filled a hard-edged
    rectangle over the middle of a rounded, translucent pill.

    **No state assertion can see this**, which is how it reached a screenshot from a real
    session. So this samples two pixels: one inside the stack's area and one in the gap
    between the stack and the pause button, which is bare pill body. They must match.

    Deliberately *not* a screenshot comparison — nothing here depends on fonts, layout
    metrics or a theme. It asserts one property: the pill's background is uniform.
    """
    pill.apply_state(RecordingState.RECORDING)
    pill.resize(360, pill.height())
    QCoreApplication.processEvents()

    image = pill.grab().toImage()
    stack = pill._pages.geometry()
    row = stack.center().y()

    # Inside the stack, near its right edge: the labels are left-aligned behind a
    # stretch, so this is empty background and never a glyph.
    inside = image.pixelColor(stack.right() - 6, row)
    # The pill's own painted body, in the left margin before the recording dot.
    body = image.pixelColor(pill._dot.geometry().left() - 4, row)

    # Verified to distinguish the two states: with the fix, both read #1e2126; without
    # it, `inside` reads #16181c -- the universal rule's `bg-base`.
    assert inside == body, (
        f"an opaque rectangle is painted inside the pill: {inside.name()} inside the stack vs {body.name()} on the body"
    )


def test_paused_looks_different_from_recording(pill: RecordingPill) -> None:
    """SPEC.md §16.5 makes paused a first-class state.

    "A paused recording that looks like a running one is how a user loses ten minutes of
    footage without noticing." Asserted on the control's glyph, which is the difference a
    user reads -- and which does not depend on being able to tell amber from red.
    """
    pill.apply_state(RecordingState.RECORDING)
    recording_glyph = pill._pause.text()

    pill.apply_state(RecordingState.PAUSED)
    paused_glyph = pill._pause.text()

    assert recording_glyph != paused_glyph
    assert paused_glyph == "▶"


def test_the_clock_shows_the_timeline_not_the_wall_clock(pill: RecordingPill) -> None:
    """`timeline_ms`, which is the length the file will have (§16.2, §7.5)."""
    pill.apply_state(RecordingState.RECORDING)
    pill.apply_stats({"state": "recording", "timeline_ms": 862_000, "bytes_written": 412 * 1024 * 1024})

    assert pill._elapsed.text() == "00:14:22"
    assert pill._size.text() == "412.0 MB"


def test_stopping_acknowledges_the_click_before_the_engine_answers(pill: RecordingPill) -> None:
    """The "latency after pressing a button" defect, at the widget.

    The engine has not been told anything yet when this assertion runs -- the point is
    that the control has already changed, so the click is visibly received rather than
    apparently ignored for a round trip.
    """
    pill.apply_state(RecordingState.RECORDING)
    received: list[bool] = []
    pill.stop_requested.connect(lambda: received.append(True))

    pill._on_stop_clicked()

    assert received == [True]
    assert not pill._stop.isEnabled(), "the stop button did not acknowledge the click"
    assert pill._pages.currentIndex() == 1, "the save page was not shown on the click"


def test_progress_is_rendered_and_never_goes_backwards(pill: RecordingPill) -> None:
    """A bar that jumps backwards reads as a failure.

    The engine's `percent` is monotonic by construction (`finalize_percent`), but the
    *second* remux attempt legitimately restarts at 0 when a reserved moov did not fit.
    The pill holds the high-water mark so that shows as a pause, not a reversal.
    """
    pill.apply_state(RecordingState.STOPPING)
    pill.apply_finalize_progress({"phase": "remuxing", "percent": 40, "bytes_total": 1000, "bytes_done": 400})
    assert pill._progress.value() == 40

    pill.apply_finalize_progress({"phase": "remuxing", "percent": 12, "bytes_total": 1000, "bytes_done": 120})
    assert pill._progress.value() == 40, "the bar went backwards"


def test_a_phase_with_no_byte_progress_is_indeterminate_rather_than_stalled(pill: RecordingPill) -> None:
    """`validating` is a fixed ~1 s decode with no proportional quantity (BUG-046).

    A determinate bar parked at its last value for a second reads as a hang, so the phase
    is shown as motion instead of as an invented number. Qt spells indeterminate as a
    zero range.
    """
    pill.apply_state(RecordingState.STOPPING)
    pill.apply_finalize_progress({"phase": "validating", "percent": 80, "bytes_total": 0, "bytes_done": 0})

    assert pill._progress.minimum() == 0 and pill._progress.maximum() == 0
    assert pill._phase.text() == "Checking…"


def test_the_pill_closes_only_after_the_file_validated(pill: RecordingPill) -> None:
    """§20 row 22.

    Not on a timer, and not when `stop_record` returned. Until the engine says the file
    validated there is nothing to be reassured about, and a control that vanished before
    the outcome was known would be claiming a success it had not verified.
    """
    pill.apply_state(RecordingState.RECORDING)
    pill.apply_state(RecordingState.STOPPING)
    QCoreApplication.processEvents()
    assert pill.isVisible(), "the pill left while the file was still being written"

    pill.apply_finalized({"valid": True, "output": "r.mkv"})
    assert pill._progress.value() == 100
    assert pill.isVisible(), "the pill vanished the instant the bar filled"

    # The dwell, then gone.
    deadline = time.monotonic() + 3.0
    while pill.isVisible() and time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.02)
    assert not pill.isVisible(), "the pill never closed after a successful save"


def test_a_file_that_failed_validation_keeps_the_pill_up(pill: RecordingPill) -> None:
    """CLAUDE.md §1: a file that did not validate is the one thing a user must not
    discover later. The pill does not tidy itself away as if nothing happened."""
    pill.apply_state(RecordingState.RECORDING)
    pill.apply_state(RecordingState.STOPPING)
    pill.apply_finalized({"valid": False, "detail": "duration 8% short", "output": "r.mkv"})

    for _ in range(20):
        QCoreApplication.processEvents()
        time.sleep(0.05)

    assert pill.isVisible(), "the pill closed on a recording that failed validation"
    assert pill._phase.text() == "Save failed"


def test_a_new_recording_during_the_saved_dwell_keeps_the_pill(pill: RecordingPill) -> None:
    """The dwell must not take the pill away from a recording that has already started.

    Reachable in practice: stop, then start again within 600 ms -- which a hotkey makes
    easy.
    """
    pill.apply_state(RecordingState.RECORDING)
    pill.apply_state(RecordingState.STOPPING)
    pill.apply_finalized({"valid": True, "output": "r.mkv"})
    pill.apply_state(RecordingState.RECORDING)

    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.02)

    assert pill.isVisible(), "the previous recording's dwell closed the pill on a live one"
