"""The Hotkeys settings section (SPEC.md §16.4, §16.5, M9.6 F3).

Three rows -- start, stop, pause/resume -- each with a field that records the next
combination you press, and a chip saying whether it is actually in force.

**The chip is the point.** A conflict used to be reported once, in the status bar, for
twelve seconds at startup, and then lost. After that the user had a hotkey that silently
did nothing and nowhere to look, which is most of what "hotkeys don't work" means in
practice. Here the reason sits next to the binding for as long as it is true.

Validation happens **as the key is pressed**, not on OK. A combination that is taken, or
reserved, or already used by another action in this dialog, says so immediately -- so the
dialog is where a conflict is discovered rather than the next time the user reaches for
the key mid-recording.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QCheckBox,
    QGridLayout,
    QGroupBox,
    QLabel,
    QLineEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from ..hotkeys import (
    ACTION_LABELS,
    DEFAULT_BINDINGS,
    MOD_ALT,
    MOD_CONTROL,
    MOD_SHIFT,
    MOD_WIN,
    Action,
    BindingState,
    BindingStatus,
    HotkeyError,
    format_sequence,
    normalise,
    parse_sequence,
    reserved_reason,
)

#: States that mean the binding is not in force and the user has to do something.
_BAD_STATES = (BindingState.CONFLICT, BindingState.FAILED, BindingState.INVALID)

#: Qt modifier -> the `RegisterHotKey` bit. `Meta` is the Windows key.
_QT_MODIFIERS: tuple[tuple[Qt.KeyboardModifier, int], ...] = (
    (Qt.KeyboardModifier.ControlModifier, MOD_CONTROL),
    (Qt.KeyboardModifier.AltModifier, MOD_ALT),
    (Qt.KeyboardModifier.ShiftModifier, MOD_SHIFT),
    (Qt.KeyboardModifier.MetaModifier, MOD_WIN),
)

#: Keys that are *only* modifiers. Pressing one is the user on their way to a
#: combination, not a combination -- capturing it would record `Ctrl` as a hotkey.
_MODIFIER_KEYS = {
    Qt.Key.Key_Control,
    Qt.Key.Key_Alt,
    Qt.Key.Key_Shift,
    Qt.Key.Key_Meta,
    Qt.Key.Key_AltGr,
    Qt.Key.Key_CapsLock,
    Qt.Key.Key_NumLock,
}


class HotkeyEdit(QLineEdit):
    """Records the next combination pressed into it.

    Read-only to typing: the text is a rendering of a captured combination, never
    something edited character by character. A user typing `Crtl+F9` by hand would
    otherwise store a binding that cannot be parsed and find out later.

    **The keyboard is grabbed while it has focus**, so pressing the combination here does
    not also fire the global hotkey it is currently bound to -- which would pause the
    recording the user is in the middle of setting up.
    """

    captured = Signal(str)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setReadOnly(True)
        self.setPlaceholderText("Click, then press a combination")
        self.setClearButtonEnabled(False)

    def focusInEvent(self, event: object) -> None:  # noqa: N802 -- Qt's spelling
        super().focusInEvent(event)  # type: ignore[arg-type]
        self.grabKeyboard()
        self.setPlaceholderText("Press a combination…")

    def focusOutEvent(self, event: object) -> None:  # noqa: N802 -- Qt's spelling
        self.releaseKeyboard()
        self.setPlaceholderText("Click, then press a combination")
        super().focusOutEvent(event)  # type: ignore[arg-type]

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 -- Qt's spelling
        key = Qt.Key(event.key())

        if key in _MODIFIER_KEYS:
            # Still on the way to a combination. Swallowed so it does not reach the
            # dialog and move focus.
            event.accept()
            return

        if key in (Qt.Key.Key_Escape, Qt.Key.Key_Backspace):
            # Clearing. Escape would otherwise close the dialog, and Backspace is the
            # obvious gesture for "remove this binding". `Delete` is deliberately *not*
            # here: `Ctrl+Shift+Delete` is a reasonable shortcut, and a key that cannot
            # be bound because it is the clear gesture is a key the user cannot use.
            self.setText("")
            self.captured.emit("")
            event.accept()
            return

        sequence = _sequence_for(key, event.modifiers())
        if sequence:
            self.setText(sequence)
            self.captured.emit(sequence)
        event.accept()


def _key_name(key: Qt.Key, modifiers: Qt.KeyboardModifier) -> str:
    """The name `hotkeys.parse_sequence` knows this key by, or empty if it has none.

    **Derived from the key code**, not from a `QKeyEvent`'s text. The first version of
    this asked a freshly constructed `QKeyEvent(type, key, NoModifier)` for its `text()`
    -- and that constructor takes the text as a *parameter*, which was never passed, so
    it returned an empty string every time. The effect was that every key outside the
    lookup table below captured nothing at all: letters, digits, the numpad and all
    punctuation. Only the handful of named keys worked.

    Reading `event.text()` from the *real* event would not have been right either: with
    Ctrl held, a letter's text is the control character (`Ctrl+R` is `\x12`), not `r`.
    The key code is the only thing that says which key was struck regardless of what is
    held down with it.
    """
    if name := _KEY_NAMES_BY_QT.get(key):
        return name

    keypad = bool(modifiers & Qt.KeyboardModifier.KeypadModifier)
    if keypad and (name := _KEYPAD_NAMES.get(key)):
        return name

    code = int(key)
    if Qt.Key.Key_0 <= key <= Qt.Key.Key_9:
        # The numpad digits share Qt key codes with the number row and are told apart
        # only by the modifier -- but they are *different* virtual keys to Windows, so a
        # user who binds the numpad has to get the numpad.
        digit = chr(code)
        return f"num{digit}" if keypad else digit
    if Qt.Key.Key_A <= key <= Qt.Key.Key_Z:
        return chr(code).lower()

    return _PUNCTUATION_NAMES.get(key, "")


def _sequence_for(key: Qt.Key, modifiers: Qt.KeyboardModifier) -> str:
    """Render a Qt key plus modifiers the way `parse_sequence` reads it.

    Goes through `parse_sequence` rather than trusting the rendering: a key Qt can name
    but this build's virtual-key table cannot is refused here, where the user is looking,
    instead of being stored and failing to bind later.
    """
    name = _key_name(key, modifiers)
    if not name:
        return ""

    bits = 0
    for qt_modifier, bit in _QT_MODIFIERS:
        if modifiers & qt_modifier:
            bits |= bit

    parts = [
        label
        for bit, label in ((MOD_WIN, "Win"), (MOD_CONTROL, "Ctrl"), (MOD_ALT, "Alt"), (MOD_SHIFT, "Shift"))
        if bits & bit
    ]
    parts.append(name)
    candidate = "+".join(parts)

    try:
        parsed_modifiers, parsed_key = parse_sequence(candidate)
    except HotkeyError:
        return ""
    return format_sequence(parsed_modifiers, parsed_key)


#: Numpad keys whose Qt code is shared with a non-numpad key. Distinguished by
#: `KeypadModifier`, and mapped to the numpad's own virtual keys.
_KEYPAD_NAMES: dict[Qt.Key, str] = {
    Qt.Key.Key_Plus: "numadd",
    Qt.Key.Key_Minus: "numsubtract",
    Qt.Key.Key_Asterisk: "nummultiply",
    Qt.Key.Key_Slash: "numdivide",
    Qt.Key.Key_Period: "numdecimal",
}

#: Punctuation, by Qt key code. Spellings match `hotkeys._VIRTUAL_KEYS`.
_PUNCTUATION_NAMES: dict[Qt.Key, str] = {
    Qt.Key.Key_Minus: "-",
    Qt.Key.Key_Equal: "=",
    Qt.Key.Key_BracketLeft: "[",
    Qt.Key.Key_BracketRight: "]",
    Qt.Key.Key_Backslash: "\\",
    Qt.Key.Key_Semicolon: ";",
    Qt.Key.Key_Apostrophe: "'",
    Qt.Key.Key_Comma: ",",
    Qt.Key.Key_Period: ".",
    Qt.Key.Key_Slash: "/",
    Qt.Key.Key_QuoteLeft: "`",
}


#: Qt keys whose name this build spells differently from `QKeyEvent.text()`.
_KEY_NAMES_BY_QT: dict[Qt.Key, str] = {
    **{getattr(Qt.Key, f"Key_F{n}"): f"f{n}" for n in range(1, 25)},
    Qt.Key.Key_Space: "space",
    Qt.Key.Key_Tab: "tab",
    Qt.Key.Key_Return: "enter",
    Qt.Key.Key_Enter: "enter",
    Qt.Key.Key_Insert: "insert",
    Qt.Key.Key_Delete: "delete",
    Qt.Key.Key_Home: "home",
    Qt.Key.Key_End: "end",
    Qt.Key.Key_PageUp: "pageup",
    Qt.Key.Key_PageDown: "pagedown",
    Qt.Key.Key_Left: "left",
    Qt.Key.Key_Up: "up",
    Qt.Key.Key_Right: "right",
    Qt.Key.Key_Down: "down",
    Qt.Key.Key_Pause: "pause",
    Qt.Key.Key_Print: "printscreen",
    Qt.Key.Key_ScrollLock: "scrolllock",
}


class HotkeysSection(QWidget):
    """The dialog tab. Owns three rows and the master switch."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("HotkeysSection")

        self._edits: dict[Action, HotkeyEdit] = {}
        self._chips: dict[Action, QLabel] = {}

        layout = QVBoxLayout(self)

        self._enabled = QCheckBox("Enable global hotkeys")
        self._enabled.setToolTip(
            "Global hotkeys work while another application is in front, which is the "
            "whole point of them. Turning them off leaves the in-window controls."
        )
        self._enabled.toggled.connect(self._on_enabled_toggled)
        layout.addWidget(self._enabled)

        group = QGroupBox("Shortcuts")
        grid = QGridLayout(group)
        grid.setColumnStretch(1, 1)

        for row, action in enumerate(Action):
            grid.addWidget(QLabel(ACTION_LABELS[action]), row, 0)

            edit = HotkeyEdit()
            # The signal carries the captured sequence; `_revalidate` re-reads every
            # field anyway and its argument is the manager's statuses, so the value
            # is dropped rather than landing in the wrong parameter.
            edit.captured.connect(lambda _sequence: self._revalidate())
            self._edits[action] = edit
            grid.addWidget(edit, row, 1)

            reset = QPushButton("Reset")
            reset.setToolTip("Back to the default for this action")
            reset.clicked.connect(lambda _=False, a=action: self._reset(a))
            grid.addWidget(reset, row, 2)

            chip = QLabel("")
            chip.setObjectName("HotkeyChip")
            chip.setWordWrap(True)
            self._chips[action] = chip
            grid.addWidget(chip, row, 3)

        layout.addWidget(group)

        note = QLabel(
            "Press Escape or Backspace in a field to remove its shortcut. Every "
            "combination needs at least one modifier — a bare key would be swallowed "
            "system-wide.\n\n"
            "Start and stop may share one combination, which then works as a toggle."
        )
        note.setObjectName("SettingsHint")
        note.setWordWrap(True)
        layout.addWidget(note)

        layout.addStretch(1)

    # -- population ---------------------------------------------------------

    def load(self, config: dict[str, object], statuses: dict[Action, BindingStatus]) -> None:
        """Show the saved bindings, and what actually happened to them."""
        enabled = bool(config.get("hotkeys_enabled", True))
        self._enabled.setChecked(enabled)
        # Applied directly rather than left to the `toggled` signal. `setChecked` emits
        # nothing when the value is unchanged, and the box starts unchecked -- so
        # loading a configuration with hotkeys already off left the fields enabled and
        # editable under a checkbox that said they were not.
        self._on_enabled_toggled(enabled)

        for action in Action:
            saved = str(config.get(f"hotkey_{action.value}", DEFAULT_BINDINGS[action]) or "")
            self._edits[action].setText(normalise(saved) if saved else "")
        self._revalidate(statuses)

    def collect(self) -> dict[str, object]:
        """The settings, in the shape `save_config` takes."""
        return {
            "hotkeys_enabled": self._enabled.isChecked(),
            **{f"hotkey_{action.value}": self._edits[action].text().strip() for action in Action},
        }

    # -- validation ---------------------------------------------------------

    def _on_enabled_toggled(self, enabled: bool) -> None:
        for edit in self._edits.values():
            edit.setEnabled(enabled)
        self._revalidate()

    def _reset(self, action: Action) -> None:
        self._edits[action].setText(DEFAULT_BINDINGS[action])
        self._revalidate()

    def _revalidate(self, statuses: dict[Action, BindingStatus] | None = None) -> None:
        """Re-derive every chip.

        Called on every capture rather than on OK. A conflict the user discovers while
        typing is one they can fix; a conflict they discover the next time they reach for
        the key mid-recording is the reported defect.

        `statuses` is what the manager last reported and is only available when the
        dialog opens -- afterwards the chips describe what *would* happen, which is all
        that can be known before the bindings are applied.
        """
        # Sequences named more than once, so a duplicate is reported on every row that
        # shares it rather than on whichever happened to be checked second.
        counts: dict[str, int] = {}
        for action in Action:
            sequence = self._edits[action].text().strip()
            if sequence:
                counts[normalise(sequence)] = counts.get(normalise(sequence), 0) + 1

        for action in Action:
            self._chips[action].setText(self._chip_text(action, counts, statuses))
            self._chips[action].setProperty("state", self._chip_state(action, counts, statuses))
            self._chips[action].style().unpolish(self._chips[action])
            self._chips[action].style().polish(self._chips[action])

    def _shared_with(self, action: Action, counts: dict[str, int]) -> list[Action]:
        sequence = normalise(self._edits[action].text().strip())
        if not sequence or counts.get(sequence, 0) < 2:
            return []
        return [
            other
            for other in Action
            if other is not action and normalise(self._edits[other].text().strip()) == sequence
        ]

    def _chip_text(self, action: Action, counts: dict[str, int], statuses: dict[Action, BindingStatus] | None) -> str:
        if not self._enabled.isChecked():
            return "Hotkeys are off"

        sequence = self._edits[action].text().strip()
        if not sequence:
            return "No shortcut"

        if (reason := reserved_reason(sequence)) is not None:
            return reason

        try:
            parse_sequence(sequence)
        except HotkeyError as error:
            return str(error)

        # Sharing is allowed and is how the start/stop toggle works, so it is reported
        # as information rather than as a problem.
        if shared := self._shared_with(action, counts):
            names = " and ".join(ACTION_LABELS[other] for other in shared)
            return f"Shared with {names} — works as a toggle"

        status = (statuses or {}).get(action)
        if status is not None and normalise(status.requested) == normalise(sequence) and not status.ok:
            return status.detail

        return "Bound"

    def _chip_state(self, action: Action, counts: dict[str, int], statuses: dict[Action, BindingStatus] | None) -> str:
        """The chip's severity, as a property the stylesheet colours."""
        if not self._enabled.isChecked():
            return "off"

        sequence = self._edits[action].text().strip()
        if not sequence:
            return "off"
        if reserved_reason(sequence) is not None:
            return "bad"
        try:
            parse_sequence(sequence)
        except HotkeyError:
            return "bad"
        if self._shared_with(action, counts):
            return "info"

        status = (statuses or {}).get(action)
        stale = status is None or normalise(status.requested) != normalise(sequence)
        if not stale and status is not None and status.state in _BAD_STATES:
            return "bad"
        return "ok"
