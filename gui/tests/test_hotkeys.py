"""Global hotkeys (SPEC.md §16.5, M9.6 F3, §20 row 21).

"Hotkeys don't work" has five causes and only three of them live in `hotkeys.py`. The
biggest -- a GUI thread blocked in a synchronous IPC request, so `WM_HOTKEY` is never
pumped -- was fixed in Phase 0 and is covered by `test_async_commands`. What is here is
the rest: parsing, the virtual-key table, conflict reporting that survives longer than a
status message, atomic rebinding, and the dispatch itself.

**Dispatch is tested by posting a real `WM_HOTKEY` to the thread queue.** That exercises
the actual native event filter and the actual routing, without synthesising keystrokes
into whatever window happens to be focused -- which on a developer's machine is a way to
type into their editor.
"""

from __future__ import annotations

import ctypes
from collections.abc import Iterator
from ctypes import wintypes

import pytest
from PySide6.QtCore import QCoreApplication, QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication

from framecapture_gui.hotkeys import (
    DEFAULT_BINDINGS,
    MOD_ALT,
    MOD_CONTROL,
    MOD_NOREPEAT,
    MOD_SHIFT,
    Action,
    BindingState,
    BindingStatus,
    Hotkey,
    HotkeyError,
    HotkeyManager,
    format_sequence,
    normalise,
    parse_sequence,
    reserved_reason,
)
from framecapture_gui.settings.hotkey_editor import HotkeyEdit, HotkeysSection
from framecapture_gui.theme import load_stylesheet

_user32 = ctypes.WinDLL("user32", use_last_error=True)
_WM_HOTKEY = 0x0312


# ---------------------------------------------------------------------------
# Parsing and the virtual-key table
# ---------------------------------------------------------------------------


def test_a_sequence_parses_to_its_modifiers_and_key() -> None:
    modifiers, key = parse_sequence("Ctrl+Shift+F9")
    assert modifiers == MOD_NOREPEAT | MOD_CONTROL | MOD_SHIFT
    assert key == 0x78  # VK_F9


def test_norepeat_is_always_set() -> None:
    """Without it, holding the combination auto-repeats.

    For "stop recording" that means a stop, then a start, then a stop.
    """
    modifiers, _ = parse_sequence("Alt+R")
    assert modifiers & MOD_NOREPEAT


def test_a_bare_key_is_refused() -> None:
    """A global hotkey with no modifier swallows that key system-wide."""
    with pytest.raises(HotkeyError, match="no modifier"):
        parse_sequence("F9")


def test_an_unknown_modifier_is_refused_rather_than_dropped() -> None:
    """Dropping the part it did not understand would register a *different* hotkey."""
    with pytest.raises(HotkeyError, match="unknown modifier"):
        parse_sequence("Hyper+F9")


@pytest.mark.parametrize(
    "sequence",
    [
        "Ctrl+Alt+Home",
        "Ctrl+Shift+End",
        "Ctrl+Alt+Insert",
        "Ctrl+Alt+Delete",
        "Ctrl+Shift+PageUp",
        "Ctrl+Alt+Left",
        "Ctrl+Alt+Num5",
        "Ctrl+Alt+NumAdd",
        "Ctrl+Shift+Space",
        "Ctrl+Alt+-",
        "Ctrl+Alt+[",
        "Ctrl+Alt+`",
        "Ctrl+Shift+F24",
        "Ctrl+Alt+Pause",
    ],
)
def test_the_keys_the_old_table_could_not_name(sequence: str) -> None:
    """**A reported cause of "the hotkey does nothing".**

    The table held F1-F24, A-Z and 0-9 and nothing else, so every one of these was
    refused as an unsupported key and the binding silently did not exist.
    """
    modifiers, key = parse_sequence(sequence)
    assert key != 0
    assert modifiers & ~MOD_NOREPEAT, "the modifier was lost"


def test_the_numpad_is_not_the_number_row() -> None:
    """A user who binds the numpad should get the numpad."""
    _, numpad = parse_sequence("Ctrl+Num5")
    _, row = parse_sequence("Ctrl+5")
    assert numpad != row


def test_format_is_the_inverse_of_parse() -> None:
    """Round-trip for sequences already in canonical order.

    Canonical is Windows' own -- `Win`, `Ctrl`, `Alt`, `Shift` -- so a binding reads the
    way the shortcuts a user already knows are written.
    """
    for sequence in ("Ctrl+Shift+F9", "Alt+Home", "Ctrl+Alt+Num0", "Win+Shift+Up"):
        modifiers, key = parse_sequence(sequence)
        assert format_sequence(modifiers, key) == sequence


def test_a_sequence_out_of_order_is_rendered_canonically() -> None:
    """Whatever order it was written in, one combination has one spelling."""
    assert normalise("Shift+Win+Up") == "Win+Shift+Up"
    assert normalise("Alt+Ctrl+Home") == "Ctrl+Alt+Home"


def test_modifier_order_is_canonical() -> None:
    """`Shift+Ctrl+A` and `Ctrl+Shift+A` are one combination.

    Two spellings of one binding would defeat the duplicate check in the settings dialog
    *and* make the manager register a shared sequence twice -- conflicting with itself.
    """
    assert normalise("Shift+Ctrl+A") == normalise("Ctrl+Shift+A") == "Ctrl+Shift+A"


def test_normalise_leaves_an_unparseable_sequence_alone() -> None:
    """So a bad stored value survives to be shown to the user rather than mangled."""
    assert normalise("Crtl+F9") == "Crtl+F9"


def test_reserved_combinations_are_named_before_windows_refuses_them() -> None:
    assert reserved_reason("Ctrl+Alt+Delete")
    assert reserved_reason("Ctrl+Shift+Escape")
    assert reserved_reason("Ctrl+Shift+F9") is None


def test_a_reserved_combination_is_recognised_in_any_spelling() -> None:
    assert reserved_reason("Alt+Ctrl+Delete") == reserved_reason("Ctrl+Alt+Delete")


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------


@pytest.fixture
def manager(qapp: QCoreApplication) -> Iterator[HotkeyManager]:
    del qapp
    hotkeys = HotkeyManager()
    try:
        yield hotkeys
    finally:
        hotkeys.unregister_all()


def _binding(action: Action, sequence: str, calls: list[Action], *, applicable: bool = True) -> Hotkey:
    return Hotkey(action, sequence, lambda: calls.append(action), applicable=lambda: applicable)


#: Combinations unlikely to be taken by anything else on a test machine.
_FREE_A = "Ctrl+Alt+Shift+F13"
_FREE_B = "Ctrl+Alt+Shift+F14"
_FREE_C = "Ctrl+Alt+Shift+F15"


def test_bindings_register_and_report_themselves(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    statuses = manager.apply(
        [
            _binding(Action.START, _FREE_A, calls),
            _binding(Action.STOP, _FREE_B, calls),
            _binding(Action.PAUSE_RESUME, _FREE_C, calls),
        ]
    )
    assert all(status.ok for status in statuses.values()), statuses
    assert statuses[Action.START].effective == _FREE_A


def test_statuses_persist_rather_than_being_returned_once(manager: HotkeyManager) -> None:
    """**The reported defect.**

    A conflict was reported in the status bar for twelve seconds at startup and then
    lost, leaving a hotkey that silently did nothing and nowhere to look. The settings
    dialog reads this.
    """
    calls: list[Action] = []
    manager.apply([_binding(Action.START, _FREE_A, calls)])
    assert manager.statuses()[Action.START].ok


def test_an_empty_sequence_is_unset_rather_than_an_error(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    statuses = manager.apply([_binding(Action.START, "", calls)])
    assert statuses[Action.START].state is BindingState.UNSET


def test_an_unparseable_sequence_is_reported_as_invalid(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    statuses = manager.apply([_binding(Action.START, "Crtl+F9", calls)])
    assert statuses[Action.START].state is BindingState.INVALID
    assert not statuses[Action.START].effective


def test_a_reserved_sequence_is_refused_without_asking_windows(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    statuses = manager.apply([_binding(Action.START, "Ctrl+Alt+Delete", calls)])
    assert statuses[Action.START].state is BindingState.FAILED
    assert "reserve" in statuses[Action.START].detail


def test_reapplying_the_same_bindings_is_not_a_self_conflict(manager: HotkeyManager) -> None:
    """Re-registering a sequence we already hold *is* the 1409 conflict.

    Reported against ourselves, it would look exactly like another application taking
    the key -- so an idempotent apply is not an optimisation, it is correctness.
    """
    calls: list[Action] = []
    first = manager.apply([_binding(Action.START, _FREE_A, calls)])
    second = manager.apply([_binding(Action.START, _FREE_A, calls)])
    assert first[Action.START].ok
    assert second[Action.START].ok, second


def test_a_conflict_is_reported_per_binding_and_does_not_take_the_others_down(
    manager: HotkeyManager,
) -> None:
    """ "Two of three registering is a usable state."

    A second manager holds one of the keys, standing in for another application.
    """
    calls: list[Action] = []
    rival = HotkeyManager()
    try:
        rival.apply([_binding(Action.START, _FREE_B, calls)])

        statuses = manager.apply(
            [
                _binding(Action.START, _FREE_A, calls),
                _binding(Action.STOP, _FREE_B, calls),
                _binding(Action.PAUSE_RESUME, _FREE_C, calls),
            ]
        )
    finally:
        rival.unregister_all()

    assert statuses[Action.STOP].state is BindingState.CONFLICT
    assert "another application" in statuses[Action.STOP].detail
    assert statuses[Action.START].ok, "one conflict unbound an unrelated action"
    assert statuses[Action.PAUSE_RESUME].ok


def test_rebinding_releases_the_old_key(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    manager.apply([_binding(Action.START, _FREE_A, calls)])
    manager.apply([_binding(Action.START, _FREE_B, calls)])

    # The old one is free again, which a second manager can prove by taking it.
    rival = HotkeyManager()
    try:
        statuses = rival.apply([_binding(Action.STOP, _FREE_A, calls)])
        assert statuses[Action.STOP].ok, "the superseded binding was never released"
    finally:
        rival.unregister_all()


def test_unregister_all_releases_everything(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    manager.apply([_binding(Action.START, _FREE_A, calls)])
    manager.unregister_all()
    assert manager.statuses() == {}

    rival = HotkeyManager()
    try:
        assert rival.apply([_binding(Action.STOP, _FREE_A, calls)])[Action.STOP].ok
    finally:
        rival.unregister_all()


# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------


def _press(manager: HotkeyManager, sequence: str) -> None:
    """Deliver a `WM_HOTKEY` for whichever id holds `sequence`.

    Posted to the thread queue and pumped, so the real `_HotkeyFilter` inside Qt's own
    message loop is what routes it -- the same path a real keypress takes. Synthesising
    keystrokes with `SendInput` would instead type into whatever window has focus.
    """
    registration = manager._registered[normalise(sequence)]
    _user32.PostThreadMessageW(
        wintypes.DWORD(ctypes.windll.kernel32.GetCurrentThreadId()),
        wintypes.UINT(_WM_HOTKEY),
        wintypes.WPARAM(registration.hotkey_id),
        wintypes.LPARAM(0),
    )
    QCoreApplication.processEvents()


def test_a_press_reaches_the_bound_callback(manager: HotkeyManager) -> None:
    calls: list[Action] = []
    manager.apply([_binding(Action.PAUSE_RESUME, _FREE_C, calls)])

    _press(manager, _FREE_C)
    assert calls == [Action.PAUSE_RESUME]


def test_a_shared_sequence_runs_one_action_not_both(manager: HotkeyManager) -> None:
    """**The whole reason a shared key works.**

    `start` and `stop` on one combination share a single registration. Firing both
    callbacks would start and stop a recording in the same keypress; the first whose
    predicate says yes is the one that runs.
    """
    calls: list[Action] = []
    manager.apply(
        [
            _binding(Action.START, _FREE_A, calls, applicable=False),
            _binding(Action.STOP, _FREE_A, calls, applicable=True),
        ]
    )

    _press(manager, _FREE_A)
    assert calls == [Action.STOP], "a shared key ran more than one action"


def test_a_shared_sequence_registers_once(manager: HotkeyManager) -> None:
    """Registering it twice would be the 1409 conflict, reported against ourselves."""
    calls: list[Action] = []
    statuses = manager.apply(
        [
            _binding(Action.START, _FREE_A, calls),
            _binding(Action.STOP, _FREE_A, calls),
        ]
    )
    assert statuses[Action.START].ok and statuses[Action.STOP].ok
    assert len(manager._registered) == 1


def test_a_press_with_no_applicable_action_does_nothing(manager: HotkeyManager) -> None:
    """Pressing the toggle while a recording is still starting has nothing to do."""
    calls: list[Action] = []
    manager.apply(
        [
            _binding(Action.START, _FREE_A, calls, applicable=False),
            _binding(Action.STOP, _FREE_A, calls, applicable=False),
        ]
    )
    _press(manager, _FREE_A)
    assert calls == []


def test_a_raising_handler_does_not_break_the_filter(manager: HotkeyManager) -> None:
    """One bad handler costs its own press, not every hotkey afterwards."""
    calls: list[Action] = []

    def explode() -> None:
        raise RuntimeError("boom")

    manager.apply(
        [
            Hotkey(Action.START, _FREE_A, explode),
            _binding(Action.PAUSE_RESUME, _FREE_C, calls),
        ]
    )

    _press(manager, _FREE_A)
    _press(manager, _FREE_C)
    assert calls == [Action.PAUSE_RESUME]


# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------


def test_the_defaults_cover_every_action() -> None:
    assert set(DEFAULT_BINDINGS) == set(Action)


def test_start_and_stop_share_a_default() -> None:
    """Which is what preserves the start/stop toggle FrameCapture shipped with."""
    assert DEFAULT_BINDINGS[Action.START] == DEFAULT_BINDINGS[Action.STOP]
    assert DEFAULT_BINDINGS[Action.PAUSE_RESUME] != DEFAULT_BINDINGS[Action.START]


def test_every_default_parses() -> None:
    """A default that cannot be bound would ship with the feature broken."""
    for sequence in DEFAULT_BINDINGS.values():
        parse_sequence(sequence)
        assert reserved_reason(sequence) is None


def test_alt_is_a_usable_modifier() -> None:
    modifiers, _ = parse_sequence("Alt+F9")
    assert modifiers & MOD_ALT


# ---------------------------------------------------------------------------
# The settings section
# ---------------------------------------------------------------------------


@pytest.fixture
def section(qapp: QCoreApplication) -> Iterator[HotkeysSection]:
    if isinstance(qapp, QApplication):
        qapp.setStyleSheet(load_stylesheet())
    widget = HotkeysSection()
    try:
        yield widget
    finally:
        widget.deleteLater()


def test_the_section_round_trips_the_config(section: HotkeysSection) -> None:
    section.load(
        {
            "hotkeys_enabled": True,
            "hotkey_start": "Ctrl+Alt+R",
            "hotkey_stop": "Ctrl+Alt+S",
            "hotkey_pause_resume": "Ctrl+Alt+P",
        },
        {},
    )
    assert section.collect() == {
        "hotkeys_enabled": True,
        "hotkey_start": "Ctrl+Alt+R",
        "hotkey_stop": "Ctrl+Alt+S",
        "hotkey_pause_resume": "Ctrl+Alt+P",
    }


def test_a_saved_binding_is_shown_canonically(section: HotkeysSection) -> None:
    """So the same combination written two ways does not look like two bindings."""
    section.load({"hotkeys_enabled": True, "hotkey_start": "shift+ctrl+f9"}, {})
    assert section.collect()["hotkey_start"] == "Ctrl+Shift+F9"


def test_missing_keys_fall_back_to_the_defaults(section: HotkeysSection) -> None:
    section.load({}, {})
    collected = section.collect()
    for action in Action:
        assert collected[f"hotkey_{action.value}"] == DEFAULT_BINDINGS[action]


def test_a_shared_sequence_is_reported_as_a_toggle_not_a_clash(section: HotkeysSection) -> None:
    """Sharing is how start/stop works, so it is information rather than a problem."""
    section.load(
        {"hotkeys_enabled": True, "hotkey_start": "Ctrl+Shift+F9", "hotkey_stop": "Ctrl+Shift+F9"},
        {},
    )
    assert "toggle" in section._chips[Action.START].text()
    assert section._chips[Action.START].property("state") == "info"


def test_sharing_is_recognised_across_spellings(section: HotkeysSection) -> None:
    """`Shift+Ctrl+F9` and `Ctrl+Shift+F9` are one key, and both rows must say so."""
    section.load(
        {"hotkeys_enabled": True, "hotkey_start": "Ctrl+Shift+F9", "hotkey_stop": "shift+ctrl+f9"},
        {},
    )
    assert "toggle" in section._chips[Action.STOP].text()


def test_a_reserved_combination_is_explained_in_the_dialog(section: HotkeysSection) -> None:
    """Discovered while typing, not the next time the user reaches for the key."""
    section.load({"hotkeys_enabled": True, "hotkey_start": "Ctrl+Alt+Delete"}, {})
    assert "reserve" in section._chips[Action.START].text()
    assert section._chips[Action.START].property("state") == "bad"


def test_an_unparseable_binding_says_why(section: HotkeysSection) -> None:
    section.load({"hotkeys_enabled": True, "hotkey_start": "Crtl+F9"}, {})
    assert section._chips[Action.START].property("state") == "bad"


def test_an_empty_binding_reads_as_no_shortcut(section: HotkeysSection) -> None:
    section.load({"hotkeys_enabled": True, "hotkey_start": ""}, {})
    assert section._chips[Action.START].text() == "No shortcut"


def test_a_reported_conflict_is_shown_beside_its_binding(section: HotkeysSection) -> None:
    """**The reported defect.** The reason has to survive longer than a status message."""
    statuses = {
        Action.START: BindingStatus(
            Action.START, "Ctrl+Alt+R", "", BindingState.CONFLICT, "already in use by another application"
        )
    }
    section.load({"hotkeys_enabled": True, "hotkey_start": "Ctrl+Alt+R"}, statuses)

    assert section._chips[Action.START].text() == "already in use by another application"
    assert section._chips[Action.START].property("state") == "bad"


def test_a_stale_conflict_is_not_shown_against_a_new_binding(section: HotkeysSection) -> None:
    """The status describes the sequence it was reported for.

    Showing it against a different one would tell the user their new key is taken when
    nothing has tried to bind it yet.
    """
    statuses = {
        Action.START: BindingStatus(
            Action.START, "Ctrl+Alt+R", "", BindingState.CONFLICT, "already in use by another application"
        )
    }
    section.load({"hotkeys_enabled": True, "hotkey_start": "Ctrl+Alt+Q"}, statuses)
    assert section._chips[Action.START].text() == "Bound"


def test_turning_hotkeys_off_disables_the_fields(section: HotkeysSection) -> None:
    section.load({"hotkeys_enabled": False}, {})
    assert not section._edits[Action.START].isEnabled()
    assert section._chips[Action.START].text() == "Hotkeys are off"


# ---------------------------------------------------------------------------
# Capturing a combination
#
# The gap that let the capture field ship broken: the section's tests drove `load` and
# `collect`, which set the text directly, and nothing pressed a key at it. Only the keys
# in the widget's own lookup table worked -- every letter, digit, numpad key and piece of
# punctuation captured nothing at all, so a user trying to *change* a shortcut found that
# pressing keys did nothing.
# ---------------------------------------------------------------------------


_CTRL = Qt.KeyboardModifier.ControlModifier
_ALT = Qt.KeyboardModifier.AltModifier
_SHIFT = Qt.KeyboardModifier.ShiftModifier
_KEYPAD = Qt.KeyboardModifier.KeypadModifier


@pytest.fixture
def edit(qapp: QCoreApplication) -> Iterator[HotkeyEdit]:
    del qapp
    widget = HotkeyEdit()
    try:
        yield widget
    finally:
        widget.deleteLater()


def _capture(edit: HotkeyEdit, key: Qt.Key, modifiers: Qt.KeyboardModifier) -> str:
    edit.keyPressEvent(QKeyEvent(QEvent.Type.KeyPress, key, modifiers))
    return edit.text()


@pytest.mark.parametrize(
    ("key", "modifiers", "expected"),
    [
        # A letter: the case a user hits first, and the one that was broken.
        (Qt.Key.Key_R, _CTRL | _ALT, "Ctrl+Alt+R"),
        (Qt.Key.Key_F9, _CTRL | _SHIFT, "Ctrl+Shift+F9"),
        (Qt.Key.Key_Home, _CTRL | _ALT, "Ctrl+Alt+Home"),
        (Qt.Key.Key_5, _CTRL | _ALT, "Ctrl+Alt+5"),
        (Qt.Key.Key_Minus, _CTRL | _ALT, "Ctrl+Alt+-"),
        (Qt.Key.Key_BracketLeft, _CTRL | _ALT, "Ctrl+Alt+["),
        (Qt.Key.Key_Delete, _CTRL | _SHIFT, "Ctrl+Shift+Delete"),
        (Qt.Key.Key_Up, _CTRL | _ALT, "Ctrl+Alt+Up"),
        (Qt.Key.Key_Space, _CTRL | _SHIFT, "Ctrl+Shift+Space"),
    ],
)
def test_every_kind_of_key_can_be_captured(
    edit: HotkeyEdit, key: Qt.Key, modifiers: Qt.KeyboardModifier, expected: str
) -> None:
    assert _capture(edit, key, modifiers) == expected


def test_the_numpad_captures_as_the_numpad(edit: HotkeyEdit) -> None:
    """Qt gives the numpad digits the same key codes as the number row.

    They are *different* virtual keys to Windows, so a user who binds the numpad has to
    get the numpad -- the keypad modifier is the only thing that tells them apart.
    """
    assert _capture(edit, Qt.Key.Key_5, _CTRL | _ALT | _KEYPAD) == "Ctrl+Alt+Num5"
    assert _capture(edit, Qt.Key.Key_5, _CTRL | _ALT) == "Ctrl+Alt+5"


def test_a_captured_combination_is_announced(edit: HotkeyEdit) -> None:
    """The signal is what revalidates the chips, so a capture nobody hears about is a
    capture whose conflict is never reported."""
    seen: list[str] = []
    edit.captured.connect(seen.append)
    _capture(edit, Qt.Key.Key_R, _CTRL | _ALT)
    assert seen == ["Ctrl+Alt+R"]


def test_a_modifier_on_its_own_is_not_a_combination(edit: HotkeyEdit) -> None:
    """The user is on their way to a combination. Capturing `Ctrl` would record it."""
    edit.setText("Ctrl+Alt+R")
    assert _capture(edit, Qt.Key.Key_Control, _CTRL) == "Ctrl+Alt+R"
    assert _capture(edit, Qt.Key.Key_Shift, _SHIFT) == "Ctrl+Alt+R"


def test_a_key_with_no_modifier_is_refused(edit: HotkeyEdit) -> None:
    """A bare key would be swallowed system-wide, in every application."""
    edit.setText("Ctrl+Alt+R")
    assert _capture(edit, Qt.Key.Key_R, Qt.KeyboardModifier.NoModifier) == "Ctrl+Alt+R"


def test_escape_and_backspace_clear_the_binding(edit: HotkeyEdit) -> None:
    for key in (Qt.Key.Key_Escape, Qt.Key.Key_Backspace):
        edit.setText("Ctrl+Alt+R")
        assert _capture(edit, key, Qt.KeyboardModifier.NoModifier) == ""


def test_clearing_is_announced_too(edit: HotkeyEdit) -> None:
    seen: list[str] = []
    edit.setText("Ctrl+Alt+R")
    edit.captured.connect(seen.append)
    _capture(edit, Qt.Key.Key_Escape, Qt.KeyboardModifier.NoModifier)
    assert seen == [""]


def test_a_captured_combination_is_one_the_manager_can_bind(edit: HotkeyEdit) -> None:
    """The capture field and `RegisterHotKey` have to agree on spelling.

    A field that recorded something `parse_sequence` cannot read would store a binding
    that silently never works -- which is the defect this whole phase is about.
    """
    for key, modifiers in (
        (Qt.Key.Key_R, _CTRL | _ALT),
        (Qt.Key.Key_Num_Lock if hasattr(Qt.Key, "Key_Num_Lock") else Qt.Key.Key_F9, _CTRL | _SHIFT),
        (Qt.Key.Key_Comma, _CTRL | _SHIFT),
        (Qt.Key.Key_5, _CTRL | _ALT | _KEYPAD),
    ):
        sequence = _capture(edit, key, modifiers)
        if not sequence:
            continue
        parse_sequence(sequence)
        assert normalise(sequence) == sequence, f"{sequence!r} is not canonical"
