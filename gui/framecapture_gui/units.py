"""Rendering numbers the engine reports (SPEC.md §16.5).

Small, pure, and shared. §16.5's "the status panel never lies" is mostly about *which*
numbers are shown, but it has a quieter half: a number rendered badly reads as a bug in
the recorder. `1536.0 MB` is not wrong, and every user who sees it thinks something is
broken.

Nothing here imports Qt, so all of it is testable without a display.
"""

from __future__ import annotations

_KIB = 1024
_MIB = _KIB * 1024
_GIB = _MIB * 1024


def format_hms(milliseconds: int) -> str:
    """``HH:MM:SS``. Negative clamps to zero rather than rendering a minus sign.

    Hours are not wrapped at 24: a recording that ran for 30 hours should say `30`, not
    `06`, and this project's soak tier makes that a real case rather than a hypothetical.
    """
    total = max(0, milliseconds) // 1000
    return f"{total // 3600:02d}:{(total % 3600) // 60:02d}:{total % 60:02d}"


def format_size(size_bytes: int) -> str:
    """A file size a user can read at a glance.

    Three ranges, and the boundaries are where they are for a reason:

    * below 1 MiB — whole kilobytes. Tenths of a kilobyte are noise at that scale.
    * below 1 GiB — one decimal of megabytes. This is where a recording spends its first
      minute, and 1 MB of resolution is enough to see the file growing.
    * at and above 1 GiB — two decimals of gigabytes, because one decimal changes only
      every ~100 MB and the number then looks stuck on a recording that is very much
      still running.

    **The bug this exists to prevent** is the one it replaced: a bare
    ``f"{n / (1024 * 1024):.1f} MB"``, which renders a 1.5 GB recording as `1536.0 MB`.
    Not incorrect, and it reads as a counter that has broken.

    Negative clamps to zero. A size cannot be negative, and rendering `-1 KB` would
    mean the caller had a bug this function should not be hiding.
    """
    size = max(0, size_bytes)
    if size < _MIB:
        return f"{size // _KIB} KB"
    if size < _GIB:
        return f"{size / _MIB:.1f} MB"
    return f"{size / _GIB:.2f} GB"
