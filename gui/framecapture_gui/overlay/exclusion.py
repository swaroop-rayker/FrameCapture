r"""Keeping the overlay out of the recording (M9.6 Rule A).

The pill and the toasts float over whatever is being recorded. Nothing they draw may
reach the file — and the failure that matters is not "the overlay is visible", it is the
*other* one: a black rectangle where the overlay was, in a recording the user cannot
redo.

**The mechanism.** ``SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)``. DWM then
composites the capture surface as though the window were not there — the desktop behind
it shows through, no hole, no smear. It arrived in **Windows 10 2004, build 19041**,
which is exactly SPEC.md §1's minimum supported build, so there is no version fallback
to write here: every platform this project supports has the call.

**The trap, stated because it is one hex digit away.** ``WDA_MONITOR`` (``0x01``) is the
*older* flag. It also hides the window from captures — by painting **black** into them.
It is what a search result from before 2020 hands you, and it is precisely the
"pill-shaped black cutout in the recorded video" defect. It appears in this file once,
as a named constant, so that `scripts/lint.ps1` can ban it everywhere else.

**Why a module rather than three lines in the pill.** Because the toasts need it too,
and two call sites is how "the pill is excluded and the toast is not" ships. One place
decides what an overlay window is, and one place applies it.

What this module does *not* do is decide whether an overlay should exist. That is
`probe`'s result plus the user's setting, and it is the caller's to act on — but the
rule the caller must honour is absolute: **if exclusion is unavailable, do not show the
overlay over a captured display.** An overlay that would land in the file is never the
fallback.
"""

from __future__ import annotations

import ctypes
import logging
from ctypes import wintypes
from enum import StrEnum

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtWidgets import QApplication, QWidget

_log = logging.getLogger(__name__)

_user32 = ctypes.WinDLL("user32", use_last_error=True)

_user32.SetWindowDisplayAffinity.argtypes = [wintypes.HWND, wintypes.DWORD]
_user32.SetWindowDisplayAffinity.restype = wintypes.BOOL
_user32.GetWindowDisplayAffinity.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
_user32.GetWindowDisplayAffinity.restype = wintypes.BOOL

#: Captured normally. The default, and what a window that has never been stamped has.
WDA_NONE = 0x00000000

#: **Never use this.** Hides the window from captures by painting black into them —
#: the black-cutout defect this module exists to prevent. Defined so the test that
#: proves it produces a cutout has a name for it, and banned everywhere else.
WDA_MONITOR = 0x00000001

#: What we want: the window is composited out of capture surfaces entirely.
#: Windows 10 build 19041+.
WDA_EXCLUDEFROMCAPTURE = 0x00000011

#: Marks a widget as an overlay surface. Read by the application-level filter so a
#: menu or tooltip opened from an overlay is stamped too — a Qt popup is a separate
#: top-level HWND with its own default affinity, and an unstamped one is recorded.
OVERLAY_PROPERTY = "fc_overlay"


class ExclusionSupport(StrEnum):
    """Whether a window is genuinely excluded from capture.

    Three values rather than a bool because "not yet asked" is a real state and must
    not be mistaken for "asked and refused". A caller that treated `UNKNOWN` as
    `EXCLUDED` would show an overlay it had never verified.
    """

    EXCLUDED = "excluded"
    UNSUPPORTED = "unsupported"
    UNKNOWN = "unknown"


def exclude_from_capture(handle: int) -> ExclusionSupport:
    """Stamp one HWND, and **verify it took**.

    The read-back is not defensive padding. ``SetWindowDisplayAffinity`` returns a
    ``BOOL`` that this project's C++ half would never leave unchecked (CLAUDE.md §4),
    and the same discipline applies on this side for a much sharper reason: the cost of
    believing a failed call is a recording with the overlay burned into it. Asking the
    window what its affinity actually is costs one syscall per overlay per show.
    """
    if not handle:
        return ExclusionSupport.UNKNOWN

    hwnd = wintypes.HWND(handle)
    if not _user32.SetWindowDisplayAffinity(hwnd, wintypes.DWORD(WDA_EXCLUDEFROMCAPTURE)):
        error = ctypes.get_last_error()
        _log.warning("SetWindowDisplayAffinity refused for hwnd %#x: Windows error %d", handle, error)
        return ExclusionSupport.UNSUPPORTED

    applied = wintypes.DWORD(0)
    if not _user32.GetWindowDisplayAffinity(hwnd, ctypes.byref(applied)):
        # The set call succeeded and the read did not. Reported as unsupported rather
        # than assumed good: an affinity that cannot be confirmed is one this module
        # has no business promising.
        _log.warning("GetWindowDisplayAffinity failed for hwnd %#x: Windows error %d", handle, ctypes.get_last_error())
        return ExclusionSupport.UNSUPPORTED

    if applied.value != WDA_EXCLUDEFROMCAPTURE:
        _log.warning(
            "display affinity for hwnd %#x read back as %#x, not WDA_EXCLUDEFROMCAPTURE", handle, applied.value
        )
        return ExclusionSupport.UNSUPPORTED

    return ExclusionSupport.EXCLUDED


def affinity_of(handle: int) -> int | None:
    """The window's current affinity, or ``None`` if it cannot be read.

    Exists for the tests and for the diagnostic bundle. Nothing in the overlay path
    needs it: `exclude_from_capture` already reads back.
    """
    if not handle:
        return None
    value = wintypes.DWORD(0)
    if not _user32.GetWindowDisplayAffinity(wintypes.HWND(handle), ctypes.byref(value)):
        return None
    return int(value.value)


class _AffinityGuard(QObject):
    """Re-applies the affinity whenever Qt gives the widget a new native handle.

    **This is the failure that only shows up later.** Display affinity is a property of
    an HWND, not of a ``QWidget``. Qt destroys and recreates the native handle on
    ``setWindowFlags``, on reparenting, and on some screen-change paths — so a pill that
    was correctly excluded at startup silently loses its affinity the first time the
    user drags it to a second monitor, and every frame after that has a pill in it.

    ``Show`` is watched as well as ``WinIdChange`` because a widget hidden and shown
    again may have been recreated in between without the change event reaching here.
    Re-stamping an already-stamped window is a no-op syscall, so the cheap thing to do
    is the safe one.
    """

    def __init__(self, widget: QWidget) -> None:
        super().__init__(widget)
        self._widget = widget
        self.support = ExclusionSupport.UNKNOWN

    def apply(self) -> ExclusionSupport:
        # `winId()` creates the native handle if there is not one yet, which is what
        # makes stamping before the first `show()` work at all. Calling it on a widget
        # that will never be shown is harmless.
        self.support = exclude_from_capture(int(self._widget.winId()))
        return self.support

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 -- Qt's spelling
        if watched is self._widget and event.type() in (QEvent.Type.WinIdChange, QEvent.Type.Show):
            self.apply()
        return False


def guard(widget: QWidget) -> ExclusionSupport:
    """Mark `widget` as an overlay surface, stamp it, and keep it stamped.

    Idempotent: guarding a widget twice reuses the guard already installed rather than
    stacking two event filters that would each re-stamp on every show.

    Returns what the first stamp achieved. A caller that gets `UNSUPPORTED` must not
    show the widget over a captured display — see the module docstring.
    """
    installed = _existing_guard(widget)
    if installed is not None:
        return installed.support

    widget.setProperty(OVERLAY_PROPERTY, True)
    created = _AffinityGuard(widget)
    widget.installEventFilter(created)
    return created.apply()


def _existing_guard(widget: QWidget) -> _AffinityGuard | None:
    """The guard already installed on `widget`, if any.

    Searched among direct children only. `findChild` recurses by default, and a
    recursive search would find the guard belonging to a *child* widget and conclude
    this one was already handled.
    """
    for child in widget.children():
        if isinstance(child, _AffinityGuard):
            return child
    return None


class _PopupGuard(QObject):
    """Stamps transient windows that an overlay opens.

    **The one that gets missed.** A tooltip, a context menu and a combo-box popup are
    each a separate top-level HWND with its own default affinity — Qt does not inherit
    the parent's, because Windows does not. So a pill that is correctly excluded still
    puts its own right-click menu into the recording, and nobody notices until they
    watch the file back.

    Anything whose ownership chain reaches a guarded widget is stamped on show. That is
    a wider net than "popups" on purpose: the question is not what kind of window it is,
    it is whether an overlay caused it to exist.
    """

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802 -- Qt's spelling
        # This filter sees **every** `Show` in the application, so it is an entry point
        # in the same sense a thread's is, and it gets the same treatment (CLAUDE.md §4,
        # SPEC.md §19): a failure inside it is logged and contained. It was written
        # without this first, and a single `AttributeError` in the ownership walk then
        # propagated out of `QWidget.show()` for unrelated windows -- one bug in the
        # overlay breaking parts of the GUI that have nothing to do with the overlay.
        try:
            if event.type() != QEvent.Type.Show or not isinstance(watched, QWidget):
                return False
            if not watched.isWindow() or watched.property(OVERLAY_PROPERTY):
                # Not a top-level window, or already guarded in its own right -- in
                # which case its own `_AffinityGuard` has it covered and stamping again
                # would just be a second syscall.
                return False
            if _owned_by_overlay(watched):
                exclude_from_capture(int(watched.winId()))
        except Exception:
            _log.exception("the overlay popup guard raised; event delivery continues")
        return False


def _owned_by_overlay(widget: QWidget) -> bool:
    """True when `widget` exists because an overlay window opened it.

    Walks both links Qt uses, because they are not the same link: `parentWidget` is the
    ownership chain, which is what a `QMenu` created with a parent carries, while
    `transientParent` is the window-manager relationship, which is what a tooltip has
    instead. Following only one leaves a whole class of popup unstamped.
    """
    seen: set[int] = set()
    node: QWidget | None = widget
    while node is not None and id(node) not in seen:
        seen.add(id(node))
        if node.property(OVERLAY_PROPERTY):
            return True
        node = node.parentWidget() or _transient_owner(node)
    return False


def _transient_owner(widget: QWidget) -> QWidget | None:
    """The widget whose window is `widget`'s transient parent, if it is one of ours.

    Qt gives no direct `QWindow` → `QWidget` mapping, so the application's top-level
    widgets are searched. The list is short — a handful of windows — and this runs only
    for a shown top-level widget with no parent widget, which is rare.

    Windows are compared by their `QWindow`, never by forcing `winId()` on the
    candidates: `winId()` *creates* a native handle, so a comparison written that way
    would materialise a real window for every unshown top-level widget in the
    application as a side effect of asking a question.
    """
    # PySide6's stubs type `windowHandle()` and `transientParent()` as non-optional.
    # Both genuinely return `None` at runtime -- a widget that has never been shown has
    # no window, and most windows have no transient parent -- so the checks stay and
    # mypy is told the stub is optimistic rather than the checks being deleted to
    # satisfy it. Deleting them would crash this filter on the first tooltip.
    handle = widget.windowHandle()
    if handle is None:
        return None  # type: ignore[unreachable]
    transient = handle.transientParent()
    if transient is None:
        return None  # type: ignore[unreachable]

    # `topLevelWidgets` is static, so there is no application instance to fetch and
    # no "is there an app yet" branch to get wrong.
    for candidate in QApplication.topLevelWidgets():
        if candidate.windowHandle() == transient:
            return candidate
    return None


_popup_guard = _PopupGuard()


def install_popup_guard(app: QObject) -> None:
    """Install the transient-window guard on the application. Idempotent.

    Application-wide rather than per-overlay because the windows it catches are created
    by Qt, not by us — there is no constructor of ours to hook.
    """
    app.installEventFilter(_popup_guard)


def probe() -> ExclusionSupport:
    """Ask this machine whether capture exclusion works, using a throwaway window.

    Called once at startup, before any overlay is shown, because the answer decides
    whether there is an overlay at all. A 1x1 offscreen tool window: never mapped
    anywhere a user could see it, and destroyed immediately.

    A `UNKNOWN` result means Qt could not give us a native handle — which happens under
    the offscreen platform plugin — and is not the same as a refusal. The caller
    treats it the same way for safety, but the log line says which happened.
    """
    window = QWidget(None, Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint)
    try:
        window.resize(1, 1)
        # Positioned off any plausible desktop rather than hidden: a window that has
        # never been shown may have no native handle to stamp on some platforms, and
        # the probe has to test the real path.
        window.move(-32000, -32000)
        window.show()
        handle = int(window.winId())
        if not handle:
            _log.info("capture-exclusion probe: no native handle (offscreen platform?)")
            return ExclusionSupport.UNKNOWN
        result = exclude_from_capture(handle)
        _log.info("capture-exclusion probe: %s", result.value)
        return result
    finally:
        window.hide()
        window.deleteLater()
