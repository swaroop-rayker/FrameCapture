"""Where an overlay sits, and how it stays reachable (M9.6 §3.3).

Pure geometry over `QRect`s, so every case that matters — a monitor unplugged, a
resolution change, a remembered position from a display that no longer exists — is
testable without owning the hardware that produces it.

**The failure this is written against:** a pill remembered at x=3000 on a second monitor
that is no longer attached is a pill the user cannot see, *and the stop button with it*.
The recording keeps running and the only control for it is off-screen. Clamping is not
tidiness; it is the difference between a recoverable state and one where the only way to
stop recording is to kill the application.
"""

from __future__ import annotations

from enum import StrEnum

from PySide6.QtCore import QPoint, QRect, QSize

#: Distance from the screen edge for a corner-anchored overlay, in logical pixels.
DEFAULT_INSET = 24

#: How close to an edge a dragged overlay has to come before it snaps flush to it.
SNAP_DISTANCE = 16

#: Vertical space between stacked overlays (the toast column).
STACK_GAP = 8


class Corner(StrEnum):
    """Matches `config::OverlayCorner`'s spellings exactly.

    The same strings on both sides of the IPC boundary, so there is no mapping table to
    drift -- the same discipline `ipc.protocol`'s enums use.
    """

    TOP_LEFT = "top-left"
    TOP_RIGHT = "top-right"
    BOTTOM_LEFT = "bottom-left"
    BOTTOM_RIGHT = "bottom-right"

    @classmethod
    def from_wire(cls, name: str, fallback: Corner) -> Corner:
        try:
            return cls(name)
        except ValueError:
            return fallback


def corner_position(area: QRect, size: QSize, corner: Corner, inset: int = DEFAULT_INSET) -> QPoint:
    """Top-left point that places `size` in `corner` of `area`.

    `area` should be a screen's *available* geometry rather than its full geometry, so a
    bottom-anchored overlay sits above the taskbar instead of under it.
    """
    left = area.left() + inset
    top = area.top() + inset
    right = area.right() - size.width() - inset + 1
    bottom = area.bottom() - size.height() - inset + 1

    if corner is Corner.TOP_LEFT:
        return QPoint(left, top)
    if corner is Corner.TOP_RIGHT:
        return QPoint(right, top)
    if corner is Corner.BOTTOM_LEFT:
        return QPoint(left, bottom)
    return QPoint(right, bottom)


def clamp_to_screens(position: QPoint, size: QSize, screens: list[QRect]) -> QPoint:
    """Move `position` back onto a screen if it is not on one.

    **Left alone when it already overlaps a screen**, even partially. A user who
    deliberately parked the pill half off the edge should find it where they put it; the
    rule is about reachability, not tidiness.

    When it overlaps nothing, it is moved fully onto whichever screen its centre is
    nearest. Nearest rather than primary, because with the pill remembered on a monitor
    that has just been unplugged, the *adjacent* monitor is where the user will look.

    With no screens at all — which happens during a display topology change — the
    position is returned untouched. There is nowhere to clamp to, and inventing a
    coordinate would be worse than leaving it for the next screen-change event.
    """
    if not screens:
        return position

    proposed = QRect(position, size)
    if any(screen.intersects(proposed) for screen in screens):
        return position

    centre = proposed.center()
    nearest = min(screens, key=lambda screen: _distance_squared(centre, screen.center()))
    return _push_inside(proposed, nearest).topLeft()


def snap_to_edges(position: QPoint, size: QSize, area: QRect, distance: int = SNAP_DISTANCE) -> QPoint:
    """Pull a dragged overlay flush to an edge it came close to.

    Per axis independently, so a drag toward a corner snaps to both edges rather than
    picking one.
    """
    x, y = position.x(), position.y()

    if abs(x - area.left()) <= distance:
        x = area.left()
    elif abs((x + size.width()) - (area.right() + 1)) <= distance:
        x = area.right() + 1 - size.width()

    if abs(y - area.top()) <= distance:
        y = area.top()
    elif abs((y + size.height()) - (area.bottom() + 1)) <= distance:
        y = area.bottom() + 1 - size.height()

    return QPoint(x, y)


def stack_positions(
    area: QRect,
    sizes: list[QSize],
    corner: Corner,
    gap: int = STACK_GAP,
    inset: int = DEFAULT_INSET,
) -> list[QPoint]:
    """Where each item in a corner-anchored column goes, newest first.

    **This is the whole of the reported stacking defect**, in one pure function. Toasts
    overlapping, or drifting out of their column, is a geometry bug — so the geometry is
    computed in one place that owns nothing, and every widget is told where to be. A
    toast that positioned itself would be a second authority on the same question, which
    is exactly how two of them end up in the same place.

    `sizes` is in display order: index 0 is the newest and sits at the anchored corner.
    The column grows **away** from that corner — downward from a top corner, upward from
    a bottom one — so the newest item is always the one nearest the edge the user is
    watching, and older ones move aside rather than the new one appearing at the far end.

    Heights may differ (a two-line message is taller), so each offset is accumulated from
    the actual sizes rather than from an assumed row height. Assuming a uniform height is
    the other way this overlaps.
    """
    positions: list[QPoint] = []
    grows_down = corner in (Corner.TOP_LEFT, Corner.TOP_RIGHT)
    offset = 0

    for size in sizes:
        if corner in (Corner.TOP_LEFT, Corner.BOTTOM_LEFT):
            x = area.left() + inset
        else:
            x = area.right() - size.width() - inset + 1

        y = area.top() + inset + offset if grows_down else area.bottom() - size.height() - inset + 1 - offset

        positions.append(QPoint(x, y))
        offset += size.height() + gap

    return positions


def _push_inside(rect: QRect, area: QRect) -> QRect:
    """`rect` moved -- never resized -- to sit inside `area`.

    An overlay larger than the screen is pinned to the top-left rather than centred: the
    controls are on the left of the pill, so if something has to be cut off it must be
    the right-hand end.
    """
    moved = QRect(rect)
    if moved.width() <= area.width():
        moved.moveLeft(min(max(moved.left(), area.left()), area.right() + 1 - moved.width()))
    else:
        moved.moveLeft(area.left())

    if moved.height() <= area.height():
        moved.moveTop(min(max(moved.top(), area.top()), area.bottom() + 1 - moved.height()))
    else:
        moved.moveTop(area.top())
    return moved


def _distance_squared(a: QPoint, b: QPoint) -> int:
    dx = a.x() - b.x()
    dy = a.y() - b.y()
    return (dx * dx) + (dy * dy)
