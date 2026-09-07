"""Rendering numbers (SPEC.md §16.5, M9.6 §3.2).

Pure functions, no Qt. Worth their own file because both the status panel and the pill
render the same figures, and a size that reads wrong looks like a broken recorder rather
than a formatting choice.
"""

from __future__ import annotations

import pytest

from framecapture_gui.units import format_hms, format_size

_KIB = 1024
_MIB = _KIB * 1024
_GIB = _MIB * 1024


# ---------------------------------------------------------------------------
# format_size
# ---------------------------------------------------------------------------


def test_the_bug_this_replaced_is_gone() -> None:
    """A 1.5 GB recording used to render as `1536.0 MB`.

    Not incorrect, and every user who saw it thought the counter had broken. This is the
    regression that named the function.
    """
    assert format_size(int(1.5 * _GIB)) == "1.50 GB"
    assert "MB" not in format_size(int(1.5 * _GIB))


@pytest.mark.parametrize(
    ("size", "expected"),
    [
        (0, "0 KB"),
        (512, "0 KB"),
        (_KIB, "1 KB"),
        (_MIB - 1, "1023 KB"),
        (_MIB, "1.0 MB"),
        (_GIB - 1, "1024.0 MB"),
        (_GIB, "1.00 GB"),
        (2 * _GIB + (_GIB // 2), "2.50 GB"),
    ],
)
def test_the_unit_boundaries(size: int, expected: str) -> None:
    assert format_size(size) == expected


def test_the_range_boundaries_are_where_the_docstring_says() -> None:
    """One below a boundary and one at it, so an off-by-one in the comparison shows."""
    assert format_size(_MIB - 1).endswith("KB")
    assert format_size(_MIB).endswith("MB")
    assert format_size(_GIB - 1).endswith("MB")
    assert format_size(_GIB).endswith("GB")


def test_a_negative_size_clamps_rather_than_rendering_a_minus() -> None:
    """A size cannot be negative. Rendering `-1 KB` would hide a caller's bug."""
    assert format_size(-1) == "0 KB"
    assert format_size(-_GIB) == "0 KB"


def test_the_size_is_monotonic_in_its_input() -> None:
    """More bytes never reads as less.

    A property rather than a table: the three ranges use different divisors, and a
    boundary written the wrong way round would make the number jump *down* as a
    recording grows past 1 GB.
    """
    samples = [0, _KIB, 100 * _KIB, _MIB, 500 * _MIB, _GIB, 3 * _GIB, 64 * _GIB]
    values = [_as_bytes(format_size(size)) for size in samples]
    assert values == sorted(values), f"format_size is not monotonic: {list(zip(samples, values, strict=True))}"


def _as_bytes(rendered: str) -> float:
    number, unit = rendered.split(" ")
    return float(number) * {"KB": _KIB, "MB": _MIB, "GB": _GIB}[unit]


# ---------------------------------------------------------------------------
# format_hms
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("milliseconds", "expected"),
    [
        (0, "00:00:00"),
        (999, "00:00:00"),
        (1000, "00:00:01"),
        (59_000, "00:00:59"),
        (60_000, "00:01:00"),
        (3_600_000, "01:00:00"),
        (14 * 60_000 + 22_000, "00:14:22"),
    ],
)
def test_hms_rendering(milliseconds: int, expected: str) -> None:
    assert format_hms(milliseconds) == expected


def test_hours_are_not_wrapped_at_a_day() -> None:
    """A 30-hour soak should say 30, not 06.

    SPEC.md §20.1's soak tier makes this a real case rather than a hypothetical, and a
    wrapped hour on a long recording is the status panel lying.
    """
    assert format_hms(30 * 3_600_000) == "30:00:00"


def test_a_negative_elapsed_clamps() -> None:
    assert format_hms(-5000) == "00:00:00"
