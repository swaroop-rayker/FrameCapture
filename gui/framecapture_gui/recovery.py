"""Finding unfinished recordings (SPEC.md §10.4).

> a `.fcrecover` sidecar (written at recording start, containing container type, codec
> params, and expected output path) lets **the GUI's recovery path** complete the job on
> next launch.

The engine has `fc::mux::find_recoverable`, and nothing in the engine calls it: the scan
is the GUI's by §10.4's own wording, and the engine's `recover` command takes the sidecar
path the scan produced. This module is that scan.

**It reads the sidecar only to label it.** The repair is entirely the engine's -- this
side never opens the recording, never parses a container, and never decides whether a
file is valid, which is what keeps SPEC.md §16.1's "zero media processing in Python"
true of a feature whose subject is a media file. If the JSON is unreadable, the entry
survives with the filename as its label: an unfinished recording the GUI cannot describe
is still an unfinished recording, and dropping it would be the one outcome the prime
directive does not allow.

Nothing here imports Qt.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path

_log = logging.getLogger(__name__)

#: `fc::mux::kRecoverySuffix`. Appended to the recording's full name, so `capture.mp4`
#: has `capture.mp4.fcrecover` beside it.
RECOVERY_SUFFIX = ".fcrecover"


@dataclass(frozen=True)
class Unfinished:
    """One sidecar, and as much of its record as could be read."""

    sidecar: Path
    #: The recording the sidecar describes. Falls back to the sidecar path with the
    #: suffix removed, which is what the engine's naming guarantees anyway.
    output: Path
    started_utc: str = ""
    container: str = ""

    @property
    def exists(self) -> bool:
        """Whether the recording named by the sidecar is still on disk.

        A sidecar whose recording has been deleted is a leftover, not a job. Reported
        rather than filtered here so the caller can say which it is.
        """
        return self.output.is_file()

    def label(self) -> str:
        """One line a user can choose by."""
        when = f" — started {self.started_utc}" if self.started_utc else ""
        missing = "" if self.exists else " (the recording is missing)"
        return f"{self.output.name}{when}{missing}"


def read_sidecar(sidecar: Path) -> Unfinished:
    """Parse one sidecar. Never raises -- an unreadable record still yields an entry."""
    fallback = Path(str(sidecar)[: -len(RECOVERY_SUFFIX)]) if str(sidecar).endswith(RECOVERY_SUFFIX) else sidecar
    try:
        document = json.loads(sidecar.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        _log.warning("the recovery sidecar %s could not be read: %s", sidecar, error)
        return Unfinished(sidecar=sidecar, output=fallback)

    if not isinstance(document, dict):
        return Unfinished(sidecar=sidecar, output=fallback)

    output = str(document.get("output", ""))
    return Unfinished(
        sidecar=sidecar,
        output=Path(output) if output else fallback,
        started_utc=str(document.get("started_utc", "")),
        container=str(document.get("container", "")),
    )


def find_unfinished(directory: Path) -> list[Unfinished]:
    """Every unfinished recording in `directory`, oldest first.

    Not recursive, matching `fc::mux::find_recoverable`: recordings land in the
    configured output directory, and walking a user's disk looking for them is not this
    function's business.

    Oldest first because that is the order a user would work through them, and because
    a stable order makes the confirmation dialog's list reproducible.
    """
    if not directory.is_dir():
        return []
    try:
        sidecars = sorted(p for p in directory.iterdir() if p.is_file() and p.name.endswith(RECOVERY_SUFFIX))
    except OSError as error:
        _log.warning("the output directory %s could not be scanned: %s", directory, error)
        return []
    entries = [read_sidecar(path) for path in sidecars]
    # By the recorded start where there is one, by name where there is not. `started_utc`
    # is ISO-8601 UTC, so lexicographic order is chronological order.
    return sorted(entries, key=lambda entry: (entry.started_utc or "", entry.sidecar.name))
