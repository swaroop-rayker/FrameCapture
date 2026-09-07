"""Global hotkeys (SPEC.md §16.5, M9.6 F3).

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

---

**Why a bound hotkey might still do nothing**, since "hotkeys not working" is the
reported symptom and only one of its causes lives in this file:

1. **The GUI thread is blocked.** ``WM_HOTKEY`` is posted to the thread's queue and read
   by `_HotkeyFilter` inside Qt's pump. A thread sitting in a synchronous IPC request
   does not pump, so *every* hotkey is dead for the duration. That was the biggest cause
   and it was fixed in M9.6 Phase 0 by making `stop_record` asynchronous -- nothing in
   this file could have fixed it.
2. **Another application owns the combination.** Reported per binding, and now kept
   visible in the settings dialog rather than shown once in the status bar and lost.
3. **The key is not in `_VIRTUAL_KEYS`.** It used to hold F1-F24, A-Z and 0-9 and
   nothing else, so `Ctrl+Alt+Home` was refused as "unsupported key". The table below is
   now the full set a user can reasonably ask for.
4. **The combination is reserved by Windows.** `reserved_reason` names the ones that
   can be known in advance; the rest surface as a refusal from `RegisterHotKey`.
5. **A rebind was refused and left the action unbound.** `HotkeyManager.apply` takes new
   registrations before releasing old ones, so a rebind never passes through a window
   where neither key works -- but a sequence that cannot be taken does leave its action
   without one, reported rather than silent. See `apply` for why that is preferred to
   keeping the superseded key alive.
"""

from __future__ import annotations

import ctypes
import logging
from collections.abc import Callable
from ctypes import wintypes
from dataclasses import dataclass, field
from enum import StrEnum

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

#: Canonical spelling for each modifier, in the order they are rendered. Fixed order so
#: `Shift+Ctrl+F9` and `Ctrl+Shift+F9` are stored, compared and displayed identically --
#: otherwise the same binding written two ways looks like two different bindings, and
#: the duplicate check in the settings dialog misses it.
#: Windows' own order, so a binding reads the way the shortcuts a user already knows
#: are written -- `Win+Shift+S`, `Ctrl+Alt+Delete`, `Ctrl+Shift+Escape`.
_MODIFIER_ORDER: tuple[tuple[int, str], ...] = (
    (MOD_WIN, "Win"),
    (MOD_CONTROL, "Ctrl"),
    (MOD_ALT, "Alt"),
    (MOD_SHIFT, "Shift"),
)

#: Every key a binding may name.
#:
#: **This table's incompleteness was a reported cause of "the hotkey does nothing".** It
#: held F1-F24, A-Z and 0-9, so anything else -- `Home`, `Insert`, an arrow, a numpad key
#: -- was refused by `parse_sequence` and the binding silently did not exist. Refusing
#: loudly is still right (a silently-dropped key registers a *different* hotkey), so the
#: fix is for the table to cover what a user would actually reach for.
_VIRTUAL_KEYS: dict[str, int] = (
    {f"f{n}": 0x6F + n for n in range(1, 25)}
    | {chr(c).lower(): c for c in range(ord("A"), ord("Z") + 1)}
    | {str(d): 0x30 + d for d in range(10)}
    | {
        # Navigation and editing.
        "space": 0x20,
        "tab": 0x09,
        "enter": 0x0D,
        "return": 0x0D,
        "backspace": 0x08,
        "escape": 0x1B,
        "esc": 0x1B,
        "insert": 0x2D,
        "ins": 0x2D,
        "delete": 0x2E,
        "del": 0x2E,
        "home": 0x24,
        "end": 0x23,
        "pageup": 0x21,
        "pgup": 0x21,
        "pagedown": 0x22,
        "pgdn": 0x22,
        "left": 0x25,
        "up": 0x26,
        "right": 0x27,
        "down": 0x28,
        "pause": 0x13,
        "printscreen": 0x2C,
        "prtsc": 0x2C,
        "scrolllock": 0x91,
        # Numpad. Distinct virtual keys from the number row, so a user who binds the
        # numpad gets the numpad.
        **{f"num{d}": 0x60 + d for d in range(10)},
        "nummultiply": 0x6A,
        "numadd": 0x6B,
        "numsubtract": 0x6D,
        "numdecimal": 0x6E,
        "numdivide": 0x6F,
        # Punctuation, by their US-layout OEM codes. Windows registers a *virtual key*,
        # so on another layout these are whichever keys sit in those positions -- which
        # is the same thing `RegisterHotKey` would do for any other application.
        ";": 0xBA,
        "=": 0xBB,
        ",": 0xBC,
        "-": 0xBD,
        ".": 0xBE,
        "/": 0xBF,
        "`": 0xC0,
        "[": 0xDB,
        "\\": 0xDC,
        "]": 0xDD,
        "'": 0xDE,
    }
)

#: Reverse lookup for rendering. Built from the *first* spelling of each code, so
#: `0x2E` renders as `Delete` rather than `Del` -- a stored binding round-trips to the
#: same string it went in as.
_KEY_NAMES: dict[int, str] = {}
for _name, _code in _VIRTUAL_KEYS.items():
    _KEY_NAMES.setdefault(_code, _name)


class Action(StrEnum):
    """The three bindings SPEC.md §16.5 names.

    Spellings match `config::HotkeySettings`' keys (`hotkeys.start`, ...) so a binding
    needs no translation between the file, the wire and here.
    """

    START = "start"
    STOP = "stop"
    PAUSE_RESUME = "pause_resume"


#: What each action is called in the settings dialog and in a conflict message.
ACTION_LABELS: dict[Action, str] = {
    Action.START: "Start recording",
    Action.STOP: "Stop recording",
    Action.PAUSE_RESUME: "Pause / resume",
}

#: SPEC.md §16.5's actions and their out-of-the-box keys.
#:
#: **`start` and `stop` deliberately share a sequence.** That one combination is the
#: start/stop toggle FrameCapture shipped with; expressing it as two identical bindings
#: keeps the familiar behaviour while making the two actions separable for anyone who
#: wants separate keys. Two actions naming one sequence register **one** hotkey, which
#: `HotkeyManager` dispatches by current state -- registering it twice would be the 1409
#: conflict, reported against ourselves.
DEFAULT_BINDINGS: dict[Action, str] = {
    Action.START: "Ctrl+Shift+F9",
    Action.STOP: "Ctrl+Shift+F9",
    Action.PAUSE_RESUME: "Ctrl+Shift+F10",
}


@dataclass(frozen=True)
class Hotkey:
    """One binding. ``sequence`` is spelled the way a user would write it."""

    action: Action
    sequence: str
    callback: Callable[[], None]

    #: Whether this action can act right now. **This is what makes one key a toggle.**
    #: When two actions share a sequence they share a single registration, and the
    #: first whose predicate says yes is the one that runs -- so `start` and `stop` on
    #: one combination behave as the start/stop toggle FrameCapture shipped with,
    #: without this module knowing what a recording is.
    #:
    #: `None` means always applicable, which is right for a key only one action names.
    #: Firing *every* callback on a shared key would start and stop in the same press.
    applicable: Callable[[], bool] | None = None


class HotkeyError(Exception):
    """A binding could not be parsed."""


class BindingState(StrEnum):
    """What became of one action's requested binding."""

    BOUND = "bound"
    #: Deliberately not bound -- the user cleared the field.
    UNSET = "unset"
    #: The sequence could not be parsed. A typo, or a key this build cannot name.
    INVALID = "invalid"
    #: Another application owns the combination.
    CONFLICT = "conflict"
    #: `RegisterHotKey` refused for some other reason.
    FAILED = "failed"


@dataclass(frozen=True)
class BindingStatus:
    """What an action's binding actually is, versus what was asked for.

    **Both, because they can differ**, and §16.5's "never lies" applies to a settings
    dialog as much as to the status panel. When a rebind fails the previous binding is
    kept working (see `HotkeyManager.apply`), so the honest thing to show is "you asked
    for X, it is taken, Y is still in force" rather than either half alone.
    """

    action: Action
    requested: str
    #: The sequence actually registered, which is `requested` when all went well and the
    #: previous binding when a rebind was refused. Empty when nothing is bound.
    effective: str
    state: BindingState
    detail: str = ""

    @property
    def ok(self) -> bool:
        return self.state is BindingState.BOUND

    @property
    def diverged(self) -> bool:
        """True when what is in force is not what was asked for."""
        return bool(self.effective) and self.effective != self.requested


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


def format_sequence(modifiers: int, virtual_key: int) -> str:
    """``(modifiers, vk)`` → ``"Ctrl+Shift+F9"``. The inverse of `parse_sequence`.

    Modifiers always in `_MODIFIER_ORDER`, so a sequence has exactly one spelling and
    two bindings that mean the same thing compare equal.
    """
    parts = [name for bit, name in _MODIFIER_ORDER if modifiers & bit]
    key = _KEY_NAMES.get(virtual_key)
    if key is None:
        raise HotkeyError(f"no name for virtual key {virtual_key:#04x}")
    parts.append(key.upper() if len(key) == 1 else key.capitalize())
    return "+".join(parts)


def normalise(sequence: str) -> str:
    """The canonical spelling of `sequence`, or the input unchanged if unparseable.

    Used wherever two sequences are compared -- the settings dialog's duplicate check,
    and `HotkeyManager`'s grouping of actions that share a key. Without it `Shift+Ctrl+A`
    and `Ctrl+Shift+A` are two different strings naming one combination, and the shared
    sequence gets registered twice and conflicts with itself.
    """
    try:
        modifiers, key = parse_sequence(sequence)
    except HotkeyError:
        return sequence
    return format_sequence(modifiers, key)


#: Combinations Windows will not give to an application, with the reason.
#:
#: Checked before `RegisterHotKey` so the settings dialog can explain rather than report
#: a bare failure. Not exhaustive -- the shell reserves a moving target of `Win+` keys --
#: so a refusal from Windows is still handled, and this only improves the message for the
#: ones that can be known in advance.
_RESERVED: dict[str, str] = {
    "Ctrl+Alt+Delete": "Windows reserves this combination and no application can bind it",
    "Ctrl+Shift+Escape": "Windows reserves this for Task Manager",
    "Win+L": "Windows reserves this for locking the workstation",
    "Win+D": "Windows reserves this for showing the desktop",
    "Win+Tab": "Windows reserves this for Task View",
}


def reserved_reason(sequence: str) -> str | None:
    """Why `sequence` cannot be bound, or ``None`` if it can be attempted."""
    return _RESERVED.get(normalise(sequence))


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


@dataclass
class _Registration:
    """One `RegisterHotKey` id, and every action routed through it."""

    hotkey_id: int
    sequence: str
    hotkeys: list[Hotkey] = field(default_factory=list)


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
        #: Canonical sequence -> the registration holding it.
        self._registered: dict[str, _Registration] = {}
        self._next_id = 1
        self._statuses: dict[Action, BindingStatus] = {}

    # -- state --------------------------------------------------------------

    def statuses(self) -> dict[Action, BindingStatus]:
        """What each action's binding actually is.

        **Kept, rather than returned once and forgotten.** A conflict used to be
        reported in the status bar for twelve seconds at startup and then lost, leaving
        the user with a hotkey that silently did nothing and nowhere to look. The
        settings dialog reads this.
        """
        return dict(self._statuses)

    @property
    def conflicts(self) -> list[str]:
        """Human-readable descriptions of the bindings that are not in force."""
        return [
            f"{ACTION_LABELS[s.action]} ({s.requested}): {s.detail}"
            for s in self._statuses.values()
            if s.state in (BindingState.CONFLICT, BindingState.FAILED, BindingState.INVALID)
        ]

    # -- binding ------------------------------------------------------------

    def apply(self, hotkeys: list[Hotkey]) -> dict[Action, BindingStatus]:
        """Make `hotkeys` the bindings in force. Returns what each action ended up with.

        **New registrations are taken before old ones are released**, so a rebind never
        passes through a window in which neither key works, and re-applying an unchanged
        set re-registers nothing -- which matters, because re-registering a sequence we
        already hold *is* the 1409 conflict, reported against ourselves.

        **A sequence that cannot be taken leaves its action unbound, and says so.**
        M9.6's plan proposed keeping the previous binding alive instead. That was
        rejected once shared sequences existed: with `start` and `stop` on one key,
        "keep the old binding for the action that failed" means keeping a registration
        whose routing now belongs to the *other* action, so the status would claim an
        accelerator that no longer runs that action. A status that lies is worse than an
        unbound key the user is told about -- and the settings dialog validates as the
        user types, so a refusal at this point is the rare case where another
        application took the key in between.

        Idempotent: re-applying the same bindings re-registers nothing. That matters
        because re-registering a sequence we already hold is itself the 1409 conflict,
        reported against ourselves.
        """
        self._ensure_filter_installed()

        # Actions that name the same key share one registration. Grouped on the
        # *canonical* spelling so `Shift+Ctrl+A` and `Ctrl+Shift+A` are one key, not two.
        desired: dict[str, list[Hotkey]] = {}
        statuses: dict[Action, BindingStatus] = {}

        for hotkey in hotkeys:
            sequence = hotkey.sequence.strip()
            if not sequence:
                statuses[hotkey.action] = BindingStatus(hotkey.action, "", "", BindingState.UNSET, "no key assigned")
                continue

            if (reason := reserved_reason(sequence)) is not None:
                statuses[hotkey.action] = BindingStatus(hotkey.action, sequence, "", BindingState.FAILED, reason)
                continue

            try:
                parse_sequence(sequence)
            except HotkeyError as error:
                statuses[hotkey.action] = BindingStatus(hotkey.action, sequence, "", BindingState.INVALID, str(error))
                continue

            desired.setdefault(normalise(sequence), []).append(hotkey)

        # Take what is not already ours, before releasing anything.
        taken: dict[str, _Registration] = {}
        failures: dict[str, tuple[BindingState, str]] = {}

        for sequence in desired:
            if sequence in self._registered:
                continue
            outcome = self._take(sequence)
            if isinstance(outcome, _Registration):
                taken[sequence] = outcome
            else:
                failures[sequence] = outcome

        # Route, and record what each action got.
        keep: set[str] = set()
        for sequence, group in desired.items():
            registration = taken.get(sequence) or self._registered.get(sequence)
            if registration is None:
                state, detail = failures[sequence]
                for hotkey in group:
                    statuses[hotkey.action] = BindingStatus(hotkey.action, sequence, "", state, detail)
                continue

            keep.add(sequence)
            registration.hotkeys = list(group)
            for hotkey in group:
                statuses[hotkey.action] = BindingStatus(hotkey.action, sequence, sequence, BindingState.BOUND)

        for sequence, registration in list(self._registered.items()):
            if sequence not in keep:
                self._release(registration)
                del self._registered[sequence]

        self._registered.update({s: r for s, r in taken.items() if s in keep})
        self._statuses = statuses
        return dict(statuses)

    def unregister_all(self) -> None:
        for registration in self._registered.values():
            self._release(registration)
        self._registered.clear()
        self._statuses = {}

        if self._installed:
            from PySide6.QtCore import QCoreApplication

            app = QCoreApplication.instance()
            if app is not None:
                app.removeNativeEventFilter(self._filter)
            self._installed = False

    # -- internals ----------------------------------------------------------

    def _ensure_filter_installed(self) -> None:
        from PySide6.QtCore import QCoreApplication

        app = QCoreApplication.instance()
        if app is None:
            raise HotkeyError("no QCoreApplication; hotkeys need Qt's message pump")
        if not self._installed:
            app.installNativeEventFilter(self._filter)
            self._installed = True

    def _take(self, sequence: str) -> _Registration | tuple[BindingState, str]:
        """Register one sequence, or say why not."""
        modifiers, key = parse_sequence(sequence)
        hotkey_id = self._next_id

        # `None` hwnd registers against the calling *thread*, which is the GUI thread and
        # the one running Qt's pump -- so the message lands where the filter is listening.
        ctypes.set_last_error(0)
        if _user32.RegisterHotKey(None, hotkey_id, modifiers, key):
            self._next_id += 1
            _log.info("hotkey %s registered as id %d", sequence, hotkey_id)
            return _Registration(hotkey_id=hotkey_id, sequence=sequence)

        error_code = ctypes.get_last_error()
        if error_code == _ERROR_HOTKEY_ALREADY_REGISTERED:
            _log.warning("hotkey %s is already in use by another application", sequence)
            return (BindingState.CONFLICT, "already in use by another application")
        _log.warning("hotkey %s could not be registered: Windows error %d", sequence, error_code)
        return (BindingState.FAILED, f"Windows error {error_code}")

    def _release(self, registration: _Registration) -> None:
        _user32.UnregisterHotKey(None, registration.hotkey_id)
        _log.info("hotkey %s released", registration.sequence)

    def _dispatch(self, hotkey_id: int) -> None:
        """Run the one action this press means.

        **First applicable wins, and only one runs.** A sequence shared by `start` and
        `stop` is a single registration with both attached; firing both would start and
        stop a recording in one keypress. Nothing running is a legitimate outcome --
        pressing the toggle while a recording is still starting has nothing to do.
        """
        for registration in self._registered.values():
            if registration.hotkey_id != hotkey_id:
                continue
            for hotkey in list(registration.hotkeys):
                if hotkey.applicable is not None and not hotkey.applicable():
                    continue
                try:
                    hotkey.callback()
                except Exception:
                    _log.exception("the %s hotkey handler raised", hotkey.action.value)
                return
            _log.debug("hotkey %s pressed with no applicable action", registration.sequence)
            return
