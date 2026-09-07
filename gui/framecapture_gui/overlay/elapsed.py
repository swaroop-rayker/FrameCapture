"""The pill's clock (SPEC.md §7.5, §16.2).

**The number shown is `timeline_ms` — the length the file will actually have.** Not wall
clock. §7.5 excises paused time from the timeline, so a recording paused for ten minutes
shows the shorter figure, and that is the point: "why is my 30-minute recording 12
minutes long" must be answerable, and the honest answer is on screen the whole time.

**Why interpolate at all.** `stats` arrives at 2 Hz (§15.1). A clock redrawn only when it
arrives ticks twice a second in visible half-second jumps — which looks broken next to
every other timer a user has seen. A clock that free-runs from its own start instant
looks smooth and drifts away from the file, which is worse: it is the status panel
lying, quietly, by a growing margin.

So: run locally between updates, and **snap to the engine's figure every time one
arrives**. The engine is always the authority; the interpolation only fills the gaps
between its statements.

No Qt here. The widget owns a timer and asks this what to display.
"""

from __future__ import annotations

import time


class ElapsedClock:
    """Timeline elapsed, interpolated between the engine's 2 Hz updates."""

    def __init__(self) -> None:
        self._authoritative_ms = 0
        self._synced_at = time.monotonic()
        self._advancing = False

    def sync(self, timeline_ms: int, *, advancing: bool) -> None:
        """Take the engine's figure as the truth, from this instant.

        `advancing` is whether the timeline is currently moving — true while recording,
        false while paused, stopping, or idle. A paused clock **holds**: §7.5 excises the
        paused span from the file, so a pill that kept counting through a pause would be
        promising footage the recording does not contain.
        """
        self._authoritative_ms = max(0, timeline_ms)
        self._synced_at = time.monotonic()
        self._advancing = advancing

    def elapsed_ms(self) -> int:
        """What to display now."""
        if not self._advancing:
            return self._authoritative_ms
        drift = (time.monotonic() - self._synced_at) * 1000.0
        return self._authoritative_ms + int(max(0.0, drift))

    def hold(self) -> None:
        """Freeze at the current displayed value.

        Used when the engine stops reporting — a stop is in progress, or the engine has
        gone — so the last number stays put instead of counting up forever against a
        recording that has ended.
        """
        self._authoritative_ms = self.elapsed_ms()
        self._synced_at = time.monotonic()
        self._advancing = False

    def reset(self) -> None:
        self._authoritative_ms = 0
        self._synced_at = time.monotonic()
        self._advancing = False
