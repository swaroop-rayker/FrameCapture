"""Global hotkeys (SPEC.md §16.5).

    Global hotkeys for start/stop/pause, rebindable, registered via ``RegisterHotKey``,
    with conflict detection.

``RegisterHotKey`` rather than a keyboard hook, because §16.5 says so and because it is
the right mechanism: a hook sees every keystroke on the system, which is a keylogger's
capability and something a screen recorder should not ask for. ``RegisterHotKey``
receives exactly the combinations it registered and nothing else.

**Conflict detection is free and is the reason this reports failures individually.**
``RegisterHotKey`` fails with ``ERROR_HOTKEY_ALREADY_REGISTERED`` when another process
owns the combination, so a conflict is a return value rather than something to probe
for. Each binding is reported on its own: two of three registering is a usable state,
and telling the user which one is missing is the difference between a fixable complaint
and "hotkeys don't work".
"""

from __future__ import annotations

import ctypes
import logging
from collections.abc import Callable
from ctypes import wintypes
from dataclasses import dataclass

from PySide6.QtCore import QAbstractNativeEventFilter, QByteArray, QObject

_log = logging.getLogger(__name__)

_user32 = ctypes.WinDLL("user32", use_last_error=True)

_WM_HOTKEY = 0x0312
_ERROR_HOTKEY_ALREADY_REGISTERED = 1409

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
#: Without this, holding the combination repeats at the keyboard's auto-repeat rate --
#: which for "stop recording" means a stop, then a start, then a stop.
MOD_NOREPEAT = 0x4000

_MODIFIER_NAMES = {"ctrl": MOD_CONTROL, "control": MOD_CONTROL, "alt": MOD_ALT, "shift": MOD_SHIFT, "win": MOD_WIN}

#: The virtual-key codes §16.5's defaults need. Not a full table -- an incomplete map
#: that silently returns 0 would register a hotkey for the wrong key.
_VIRTUAL_KEYS = (
    {f"f{n}": 0x6F + n for n in range(1, 25)}
    | {chr(c).lower(): c for c in range(ord("A"), ord("Z") + 1)}
    | {str(d): 0x30 + d for d in range(10)}
)


@dataclass(frozen=True)
class Hotkey:
    """One binding. ``sequence`` is spelled the way a user would write it."""

    action: str
    sequence: str
    callback: Callable[[], None]


class HotkeyError(Exception):
    """A binding could not be registered."""


def parse_sequence(sequence: str) -> tuple[int, int]:
    """``"Ctrl+Shift+F9"`` → ``(modifiers, virtual_key)``.

    Raises ``HotkeyError`` for anything unrecognised rather than dropping the part it
    did not understand -- a silently-ignored modifier registers a *different* hotkey,
    which is worse than refusing.
    """
    parts = [part.strip().lower() for part in sequence.split("+") if part.strip()]
    if not parts:
        raise HotkeyError(f"empty hotkey sequence: {sequence!r}")

    modifiers = MOD_NOREPEAT
    for part in parts[:-1]:
        if part not in _MODIFIER_NAMES:
            raise HotkeyError(f"unknown modifier {part!r} in {sequence!r}")
        modifiers |= _MODIFIER_NAMES[part]

    key = parts[-1]
    if key not in _VIRTUAL_KEYS:
        raise HotkeyError(f"unsupported key {key!r} in {sequence!r}")

    # A bare key with no modifier would swallow that key system-wide. Refused.
    if modifiers == MOD_NOREPEAT:
        raise HotkeyError(f"{sequence!r} has no modifier; a global hotkey must have at least one")

    return modifiers, _VIRTUAL_KEYS[key]


class _HotkeyFilter(QAbstractNativeEventFilter):
    """Turns ``WM_HOTKEY`` into a callback on the GUI thread.

    A native event filter runs inside Qt's own message pump, so the callback is already
    on the GUI thread -- no marshalling, unlike the IPC reader.
    """

    def __init__(self, dispatch: Callable[[int], None]) -> None:
        super().__init__()
        self._dispatch = dispatch

    def nativeEventFilter(  # noqa: N802 -- Qt's spelling
        self,
        eventType: QByteArray | bytes | bytearray | memoryview,  # noqa: N803 -- Qt's parameter name
        message: int,
    ) -> tuple[bool, int]:
        # `bytes(...)` on a `memoryview[int]` is well defined; mypy's stub for the
        # overload set is narrower than the runtime accepts, hence the cast.
        if bytes(eventType) == b"windows_generic_MSG":  # type: ignore[arg-type]
            msg = ctypes.cast(int(message), ctypes.POINTER(wintypes.MSG)).contents
            if msg.message == _WM_HOTKEY:
                self._dispatch(int(msg.wParam))
                # False: the message is still delivered onward. Claiming it would be
                # lying to Qt about a message Qt has no handler for anyway.
        return False, 0


class HotkeyManager(QObject):
    """Registers §16.5's global hotkeys and routes them.

    Not fatal on failure. A conflict means one accelerator is unavailable, and a
    recorder that refused to start over it would be trading a whole application for a
    keyboard shortcut.
    """

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._filter = _HotkeyFilter(self._dispatch)
        self._installed = False
        self._callbacks: dict[int, Callable[[], None]] = {}
        self._next_id = 1
        self.conflicts: list[str] = []

    def register(self, hotkeys: list[Hotkey]) -> list[str]:
        """Register each binding. Returns the descriptions that could not be bound."""
        from PySide6.QtCore import QCoreApplication

        app = QCoreApplication.instance()
        if app is None:
            raise HotkeyError("no QCoreApplication; hotkeys need Qt's message pump")

        if not self._installed:
            app.installNativeEventFilter(self._filter)
            self._installed = True

        self.conflicts = []
        for hotkey in hotkeys:
            try:
                modifiers, key = parse_sequence(hotkey.sequence)
            except HotkeyError as error:
                _log.warning("%s", error)
                self.conflicts.append(f"{hotkey.action} ({hotkey.sequence}): {error}")
                continue

            hotkey_id = self._next_id
            # `None` hwnd registers against the calling *thread*, which is the GUI
            # thread and the one running Qt's pump -- so the message lands where the
            # filter is listening.
            if _user32.RegisterHotKey(None, hotkey_id, modifiers, key):
                self._callbacks[hotkey_id] = hotkey.callback
                self._next_id += 1
                _log.info("hotkey %s bound to %s", hotkey.action, hotkey.sequence)
                continue

            error_code = ctypes.get_last_error()
            if error_code == _ERROR_HOTKEY_ALREADY_REGISTERED:
                detail = "already in use by another application"
            else:
                detail = f"Windows error {error_code}"
            _log.warning("hotkey %s (%s) not bound: %s", hotkey.action, hotkey.sequence, detail)
            self.conflicts.append(f"{hotkey.action} ({hotkey.sequence}): {detail}")

        return self.conflicts

    def unregister_all(self) -> None:
        for hotkey_id in list(self._callbacks):
            _user32.UnregisterHotKey(None, hotkey_id)
        self._callbacks.clear()

        if self._installed:
            from PySide6.QtCore import QCoreApplication

            app = QCoreApplication.instance()
            if app is not None:
                app.removeNativeEventFilter(self._filter)
            self._installed = False

    def _dispatch(self, hotkey_id: int) -> None:
        callback = self._callbacks.get(hotkey_id)
        if callback is None:
            return
        try:
            callback()
        except Exception:
            _log.exception("a hotkey handler raised")


#: SPEC.md §16.5's three actions. Rebindable means these are defaults, not constants --
#: they are what the settings dialog would edit once §17 persistence exists.
DEFAULT_BINDINGS: dict[str, str] = {
    "start_stop": "Ctrl+Shift+F9",
    "pause_resume": "Ctrl+Shift+F10",
}
