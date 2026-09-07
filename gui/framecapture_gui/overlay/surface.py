"""Turning a widget into an overlay surface (M9.6 §3.1).

One function, `prepare`, because there are four properties an overlay window must have
and getting three of them right is not a partial success:

1. **frameless, topmost, out of Alt-Tab** — it is a control, not a window;
2. **never activated** — see below, this is the sharp one;
3. **excluded from capture** — `exclusion.guard`, Rule A;
4. **translucent background** — a pill drawn inside a rectangular window shows square
   corners against the desktop.

**Why "never activated" is the one that matters.** Clicking a normal window activates it.
Activating *any* window over a fullscreen-exclusive Direct3D application makes that
application minimize — so a user pressing "pause" on the pill while recording a
fullscreen game would minimize the game. That is the worst bug available in this
feature: the click did what it said *and* destroyed what was being recorded.

Qt's `WindowDoesNotAcceptFocus` and `WA_ShowWithoutActivating` cover showing and keyboard
focus. Neither stops a *mouse click* from activating the window — that needs
`WS_EX_NOACTIVATE` on the native window, which is why this module reaches for Win32 at
all.
"""

from __future__ import annotations

import ctypes
import logging
from ctypes import wintypes

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget

from .exclusion import ExclusionSupport, guard

_log = logging.getLogger(__name__)

_user32 = ctypes.WinDLL("user32", use_last_error=True)

# `GetWindowLongPtrW` / `SetWindowLongPtrW` do not exist as separate exports on 32-bit
# Windows -- the headers alias them to the non-Ptr forms. Resolved by name with a
# fallback so this does not depend on the interpreter's bitness.
_get_window_long = getattr(_user32, "GetWindowLongPtrW", None) or _user32.GetWindowLongW
_set_window_long = getattr(_user32, "SetWindowLongPtrW", None) or _user32.SetWindowLongW
_get_window_long.argtypes = [wintypes.HWND, ctypes.c_int]
_get_window_long.restype = ctypes.c_ssize_t
_set_window_long.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
_set_window_long.restype = ctypes.c_ssize_t

_GWL_EXSTYLE = -20

#: The window cannot be activated by a click. See the module docstring -- this is what
#: keeps a fullscreen game from minimizing when the user presses pause.
_WS_EX_NOACTIVATE = 0x08000000

#: No taskbar button and no Alt-Tab entry.
_WS_EX_TOOLWINDOW = 0x00000080


def apply_no_activate(handle: int) -> bool:
    """Add `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` to a native window.

    Read-modify-write rather than a bare set: Qt has already put its own extended styles
    on this window, and replacing them wholesale would break the frameless and layered
    behaviour it set up.
    """
    if not handle:
        return False
    hwnd = wintypes.HWND(handle)
    current = _get_window_long(hwnd, _GWL_EXSTYLE)
    if current == 0 and ctypes.get_last_error() != 0:
        _log.warning("GetWindowLongPtr failed for hwnd %#x: Windows error %d", handle, ctypes.get_last_error())
        return False

    wanted = current | _WS_EX_NOACTIVATE | _WS_EX_TOOLWINDOW
    if wanted == current:
        return True

    ctypes.set_last_error(0)
    if _set_window_long(hwnd, _GWL_EXSTYLE, wanted) == 0 and ctypes.get_last_error() != 0:
        _log.warning("SetWindowLongPtr failed for hwnd %#x: Windows error %d", handle, ctypes.get_last_error())
        return False
    return True


def prepare(widget: QWidget) -> ExclusionSupport:
    """Make `widget` an overlay surface. Returns whether it is genuinely uncapturable.

    **A caller that gets anything but `EXCLUDED` must not show the widget over a captured
    display.** Showing it anyway puts it in the user's recording, which is the one
    outcome this whole module exists to prevent. `prepare` does not enforce that itself
    because it does not know what is being captured — but nothing above it is entitled to
    ignore the return value.
    """
    widget.setWindowFlags(
        Qt.WindowType.Tool
        | Qt.WindowType.FramelessWindowHint
        | Qt.WindowType.WindowStaysOnTopHint
        | Qt.WindowType.WindowDoesNotAcceptFocus
    )
    widget.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
    # Showing must not steal focus from whatever is being recorded. The Qt half; the
    # click half is `WS_EX_NOACTIVATE` below.
    widget.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)

    # `winId()` materialises the native window, which both of the next two calls need.
    # Ordering matters: `setWindowFlags` above can destroy and recreate the handle, so
    # the native styling has to come after it, not before.
    handle = int(widget.winId())
    apply_no_activate(handle)

    # Last, and re-applied on every later handle recreation by the guard it installs.
    return guard(widget)
