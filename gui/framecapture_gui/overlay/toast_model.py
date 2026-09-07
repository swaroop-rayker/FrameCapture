"""What the toast column holds, and for how long (M9.6 F2).

Pure: no Qt widgets, no timers, no clock of its own beyond a caller-supplied `now`. The
two behaviours that actually go wrong under stress — an unbounded backlog and a stream of
identical messages — are decided here, where they can be tested against a thousand
messages in milliseconds rather than by watching a screen.

**Errors arrive in bursts, and that is the normal case rather than the exceptional one.**
`main_window._on_notified` already documents the shape: a failed engine start emits one
error, the OFFLINE transition prompts a refresh that fails and emits another, and so on.
Anything that shows one toast per event would bury the screen in copies of the same
sentence.
"""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field
from enum import StrEnum


class Severity(StrEnum):
    """How long a message stays, and how loudly it is drawn.

    The wire spellings of SPEC.md §15.1's `warning` and `error` events, so a severity
    that arrives from the engine needs no translation table.
    """

    INFO = "info"
    WARNING = "warning"
    ERROR = "error"


#: Identical messages arriving inside this window are counted rather than stacked.
#:
#: Two seconds because that is longer than the burst (a failed start emits its errors
#: within a few hundred milliseconds) and shorter than a user's own repeat — someone who
#: presses pause twice in three seconds meant it twice and should see it twice.
COALESCE_WINDOW_S = 2.0

#: The hard cap on messages held at once, shown and waiting together.
#:
#: CLAUDE.md hard rule 5 is about the engine's queues, and the reason it exists does not
#: stop at the process boundary: an engine emitting an error per frame would otherwise
#: turn a recording problem into a memory problem in the GUI. Thirty-two is far more than
#: a person will read and far less than a burst can allocate.
MAX_HELD = 32


def dwell_seconds(severity: Severity, base_seconds: int) -> float | None:
    """How long a toast of `severity` stays up. `None` means until dismissed.

    `base_seconds` is `overlay.toast_duration_s`, which CONFIG.md defines as the
    *informational* dwell and the number the others scale from.

    **An error has no dwell at all.** A message that disappears before it is read is a
    message that did not happen, and the one class of message a user must not miss is the
    one telling them something went wrong with a recording they cannot make again
    (CLAUDE.md §1). Dismissing it is a click.
    """
    if severity is Severity.ERROR:
        return None
    if severity is Severity.WARNING:
        # Half again as long: a warning is not urgent enough to hold the screen, and is
        # too easy to miss at an informational dwell.
        return base_seconds * 1.5
    return float(base_seconds)


@dataclass
class ToastEntry:
    """One message, plus how many times it has arrived."""

    severity: Severity
    text: str
    detail: str = ""
    #: A label for an optional action, e.g. "Show details". Empty for none.
    action: str = ""
    count: int = 1
    #: Monotonic seconds. Set on arrival and refreshed on every coalesced repeat, so the
    #: dwell measures time since the *last* occurrence rather than the first.
    last_seen: float = field(default_factory=time.monotonic)

    def matches(self, other: ToastEntry) -> bool:
        """Same message, for coalescing purposes.

        Severity and text only. `detail` is deliberately excluded: two failures of the
        same kind differing only in an error code are the same message to a user
        watching the screen, and showing both would be the burst this is preventing.
        """
        return self.severity is other.severity and self.text == other.text


class ToastQueue:
    """The messages currently held, in display order (newest first).

    Not a Qt object and not a widget. `ToastManager` renders whatever this contains;
    this decides what that is.
    """

    def __init__(self, max_visible: int = 4, max_held: int = MAX_HELD) -> None:
        self._entries: deque[ToastEntry] = deque()
        self._max_visible = max(1, max_visible)
        self._max_held = max(self._max_visible, max_held)
        #: Messages dropped because the cap was reached. Reported rather than silent --
        #: a GUI that quietly discarded a warning would be lying by omission.
        self.dropped = 0

    def __len__(self) -> int:
        return len(self._entries)

    @property
    def entries(self) -> list[ToastEntry]:
        """Everything held, newest first."""
        return list(self._entries)

    @property
    def visible(self) -> list[ToastEntry]:
        """The ones with a slot on screen."""
        return list(self._entries)[: self._max_visible]

    def add(self, entry: ToastEntry, *, now: float | None = None) -> ToastEntry:
        """Take one message. Returns the entry that now represents it.

        That return matters: with coalescing, the entry representing a message may be one
        that already existed, and a caller that assumed it got a new one back would
        create a second widget for a toast that is already on screen.
        """
        moment = time.monotonic() if now is None else now
        entry.last_seen = moment

        for existing in self._entries:
            if existing.matches(entry) and moment - existing.last_seen <= COALESCE_WINDOW_S:
                existing.count += 1
                existing.last_seen = moment
                # Moved to the front: a repeat is new information about *when*, and a
                # message that keeps arriving should not sink under quieter ones.
                self._entries.remove(existing)
                self._entries.appendleft(existing)
                return existing

        self._entries.appendleft(entry)
        while len(self._entries) > self._max_held:
            # Drop-**oldest**, unlike `IpcClient`'s command queue, which drops the
            # newest. The two differ because the items differ: a queued command is
            # something the user asked for and still wants, while a queued message from
            # thirty errors ago has been superseded by the twenty-nine after it.
            self._entries.pop()
            self.dropped += 1
        return entry

    def expire(self, base_seconds: int, *, now: float | None = None) -> list[ToastEntry]:
        """Remove and return everything whose dwell has run out.

        Errors never expire here; they leave through `dismiss`.
        """
        moment = time.monotonic() if now is None else now
        expired: list[ToastEntry] = []

        # Only what is on screen ages. A message waiting for a slot has not been seen
        # yet, and expiring it unseen would be the same as dropping it.
        for entry in self.visible:
            dwell = dwell_seconds(entry.severity, base_seconds)
            if dwell is not None and moment - entry.last_seen >= dwell:
                expired.append(entry)

        for entry in expired:
            self._entries.remove(entry)
        return expired

    def dismiss(self, entry: ToastEntry) -> bool:
        """Remove one message. Returns whether it was there."""
        if entry in self._entries:
            self._entries.remove(entry)
            return True
        return False

    def clear(self) -> None:
        self._entries.clear()
