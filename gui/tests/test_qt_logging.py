"""Qt's own diagnostics reach the GUI's log (BUG-C).

Qt writes to stderr by default, which is out of band from every other line the process
emits and carries no attribution. That is why ``QFont::setPointSize: Point size <= 0
(-1)`` was reported as a bare string: the message arrived with nothing to act on, and the
caller doing the round trip could not be identified from it.

These assert the routing, not the font warning. The font warning's *mechanism* is pinned
by ``test_a_pixel_sized_font_has_no_point_size`` below, which is the precondition that
makes the whole class of warning possible and is a direct consequence of SPEC.md §16.3's
"13 px base".
"""

from __future__ import annotations

import logging

import pytest
from PySide6.QtCore import QtMsgType
from PySide6.QtGui import QFont

from framecapture_gui.__main__ import _qt_message_handler


class _Context:
    """Stands in for ``QMessageLogContext``, which cannot be constructed from Python."""

    def __init__(self, file: str = "", line: int = 0, function: str = "") -> None:
        self.file = file
        self.line = line
        self.function = function


@pytest.mark.parametrize(
    ("mode", "expected"),
    [
        (QtMsgType.QtDebugMsg, logging.DEBUG),
        (QtMsgType.QtInfoMsg, logging.INFO),
        (QtMsgType.QtWarningMsg, logging.WARNING),
        (QtMsgType.QtCriticalMsg, logging.ERROR),
        (QtMsgType.QtFatalMsg, logging.CRITICAL),
    ],
)
def test_qt_severities_keep_their_severity(mode: QtMsgType, expected: int, caplog: pytest.LogCaptureFixture) -> None:
    """A Qt warning must not arrive as an info line, or it will be scrolled past."""
    with caplog.at_level(logging.DEBUG, logger="qt"):
        _qt_message_handler(mode, _Context(), "probe")  # type: ignore[arg-type]
    assert len(caplog.records) == 1
    assert caplog.records[0].levelno == expected
    assert "probe" in caplog.records[0].getMessage()


def test_the_source_location_is_carried_when_qt_supplies_one(caplog: pytest.LogCaptureFixture) -> None:
    """The whole point: the next occurrence names the caller instead of just the symptom."""
    with caplog.at_level(logging.WARNING, logger="qt"):
        _qt_message_handler(
            QtMsgType.QtWarningMsg,
            _Context("qfont.cpp", 1234, "QFont::setPointSize"),  # type: ignore[arg-type]
            "QFont::setPointSize: Point size <= 0 (-1), must be greater than 0",
        )
    rendered = caplog.records[0].getMessage()
    assert "qfont.cpp:1234" in rendered
    assert "QFont::setPointSize" in rendered


def test_a_message_with_no_location_still_logs(caplog: pytest.LogCaptureFixture) -> None:
    """Release builds of Qt strip ``__FILE__``; the message must survive that."""
    with caplog.at_level(logging.WARNING, logger="qt"):
        _qt_message_handler(QtMsgType.QtWarningMsg, _Context(), "no location here")  # type: ignore[arg-type]
    assert caplog.records[0].getMessage() == "no location here"


def test_a_pixel_sized_font_has_no_point_size() -> None:
    """BUG-C's mechanism, pinned rather than described.

    SPEC.md §16.3 specifies "13 px base, 11 px for the status readout", so
    ``theme_dark.qss`` sets ``font-size`` in px and every widget's font is pixel-sized.
    ``QFont::pointSize()`` is then -1 by definition, and any code that reads it back and
    feeds it to ``setPointSize`` emits the warning. Anyone proposing to change the
    typography to points to make the warning go away is changing what §16.3 asks for, and
    this is the test that says so out loud.
    """
    font = QFont()
    font.setPixelSize(13)
    assert font.pointSize() == -1
    assert font.pixelSize() == 13
