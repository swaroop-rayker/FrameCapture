"""The dark theme (SPEC.md §16.3).

§16.3 asks for "a single themeable file (``theme_dark.qss``) with the palette as
variables so a light theme is a drop-in later". Qt stylesheets have no variables, so the
palette lives here and is substituted into the QSS at load time. That keeps the promise
the spec is actually making -- one place to change a colour -- rather than the literal
mechanism it names, which Qt does not offer.

A light theme is then exactly this dict with different values and the same QSS.
"""

from __future__ import annotations

from pathlib import Path

#: SPEC.md §16.3's palette, verbatim.
#:
#: "**No pure black, no pure white** -- pure `#000000` backgrounds cause halation on OLED
#: and pure white text at high contrast is fatiguing over long sessions."
PALETTE: dict[str, str] = {
    "bg-base": "#16181C",
    "bg-surface": "#1E2126",
    "bg-elevated": "#272B32",
    "border": "#343941",
    "text-primary": "#E4E6EB",
    "text-secondary": "#9BA1AC",
    "text-disabled": "#5C636E",
    "accent": "#4C8DFF",
    "accent-hover": "#6BA0FF",
    "rec-active": "#E5484D",
    "warn": "#F5A524",
    "ok": "#35C489",
}

_QSS_PATH = Path(__file__).with_name("theme_dark.qss")


def load_stylesheet(palette: dict[str, str] | None = None) -> str:
    """The QSS with ``@token`` replaced by the palette's values.

    Longest key first, so ``@accent-hover`` is not eaten by ``@accent``. Getting that
    ordering wrong yields ``#4C8DFF-hover``, which Qt silently ignores -- leaving one
    hover state unstyled and no error anywhere.
    """
    colours = palette or PALETTE
    text = _QSS_PATH.read_text(encoding="utf-8")
    for name in sorted(colours, key=len, reverse=True):
        text = text.replace(f"@{name}", colours[name])
    return text


def colour(name: str) -> str:
    """One palette entry, for the few places that need a colour in Python.

    Kept to a minimum: a colour set in code is a colour the QSS cannot override, and
    therefore one a future light theme would miss.
    """
    return PALETTE[name]
