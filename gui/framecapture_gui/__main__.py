"""Entry point: ``python -m framecapture_gui``.

Finds the engine, applies the theme, shows the window. Deliberately thin -- the same
rule ``engine/app/main.cpp`` follows, for the same reason: logic here is logic no test
can reach without starting a GUI.
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path

from PySide6.QtCore import QMessageLogContext, QtMsgType, qInstallMessageHandler
from PySide6.QtWidgets import QApplication

from .main_window import MainWindow
from .theme import load_stylesheet

_REPO_ROOT = Path(__file__).resolve().parents[2]
_PRESETS = ("windows-msvc-release", "windows-msvc-relwithdebinfo", "windows-msvc-debug")


def find_engine() -> Path | None:
    """Locate ``framecapture-engine.exe``.

    Three places, in order: ``FC_ENGINE_EXE``; next to this package, which is where the
    installer puts it (§21.3); then the build tree, newest first, which is where a
    developer's is. Returning ``None`` is not fatal -- SPEC.md §16.1 requires the window
    to run and be honest without an engine.
    """
    override = os.environ.get("FC_ENGINE_EXE")
    if override and Path(override).is_file():
        return Path(override)

    installed = Path(sys.argv[0]).resolve().parent / "framecapture-engine.exe"
    if installed.is_file():
        return installed

    built = [
        path
        for preset in _PRESETS
        if (path := _REPO_ROOT / "build" / preset / "bin" / "framecapture-engine.exe").is_file()
    ]
    return max(built, key=lambda p: p.stat().st_mtime) if built else None


#: The engine's log-level vocabulary (`fc::log::Level`), mapped to Python's.
#:
#: **One vocabulary across both processes, because there is one setting.** The engine
#: spells them `trace/debug/info/warn/error/critical/off`; Python's `logging` has no
#: TRACE and spells the fourth one `WARNING`. Accepting only Python's meant
#: `run-dev.ps1 -Gui` — whose `-LogLevel` defaults to `trace` and is passed to the engine
#: through `FC_LOG_LEVEL` — died on its own default with an argparse error.
#:
#: `trace` maps to DEBUG for the GUI's own logging: Python cannot go finer, and the level
#: the *engine* uses comes from `FC_LOG_LEVEL` rather than from here, so nothing is lost
#: by flattening it on this side.
_LOG_LEVELS: dict[str, int] = {
    "trace": logging.DEBUG,
    "debug": logging.DEBUG,
    "info": logging.INFO,
    "warn": logging.WARNING,
    "warning": logging.WARNING,
    "error": logging.ERROR,
    "critical": logging.CRITICAL,
    "off": logging.CRITICAL + 10,
}


#: Qt's severities, mapped onto Python's.
_QT_LEVELS: dict[QtMsgType, int] = {
    QtMsgType.QtDebugMsg: logging.DEBUG,
    QtMsgType.QtInfoMsg: logging.INFO,
    QtMsgType.QtWarningMsg: logging.WARNING,
    QtMsgType.QtCriticalMsg: logging.ERROR,
    QtMsgType.QtFatalMsg: logging.CRITICAL,
}


def _qt_message_handler(mode: QtMsgType, context: QMessageLogContext, message: str) -> None:
    """Routes Qt's own diagnostics into the GUI's log, with whatever context Qt carried.

    Qt writes to stderr by default, so a warning from inside the toolkit arrives
    unattributed, out of band from every other line the process emits, and is gone the
    moment the console is closed. That is how BUG-C -- ``QFont::setPointSize: Point size
    <= 0 (-1)`` -- was reported as a bare string with nothing to act on: the *mechanism*
    is known (a stylesheet ``font-size`` in px makes ``QFont::pointSize()`` return -1, so
    anything that reads it back through ``setPointSize`` warns; SPEC.md §16.3 asks for
    "13 px base", so the precondition is the design), but not which caller does the
    round trip, and it could not be reproduced on demand.

    This does not silence it. It makes the next occurrence carry Qt's file, line and
    function, which is what naming the caller needs.
    """
    where = ""
    if context.file:
        where = f" [{context.file}:{context.line} {context.function or '?'}]"
    logging.getLogger("qt").log(_QT_LEVELS.get(mode, logging.WARNING), "%s%s", message, where)


def default_output_directory() -> Path:
    return Path.home() / "Videos" / "FrameCapture"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="framecapture-gui", description="FrameCapture")
    parser.add_argument("--engine", type=Path, default=None, help="path to framecapture-engine.exe")
    parser.add_argument("--output-dir", type=Path, default=None, help="where recordings go")
    parser.add_argument(
        "--log-level",
        default="info",
        # `type=str.lower` so `--log-level TRACE` works too. A level is a level whichever
        # case the caller happens to type it in.
        type=str.lower,
        choices=sorted(_LOG_LEVELS),
        help="matches the engine's levels; passed to the engine via FC_LOG_LEVEL",
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=_LOG_LEVELS[args.log_level],
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
    )
    # Before QApplication: Qt emits during construction, and those are exactly the
    # start-up messages that were being lost.
    qInstallMessageHandler(_qt_message_handler)

    engine = args.engine or find_engine()
    if engine is None:
        # Not fatal, and not silent. The window opens in its OFFLINE state, which is
        # exactly the state §16.1 requires it to be usable in.
        logging.getLogger(__name__).error(
            "no framecapture-engine.exe found; the window will open offline. Build it, or pass --engine."
        )
        engine = Path("framecapture-engine.exe")

    app = QApplication(sys.argv)
    app.setApplicationName("FrameCapture")
    app.setStyleSheet(load_stylesheet())

    # The saved configuration wins over the built-in default but not over an explicit
    # flag, so the window is told which of the two it was given.
    window = MainWindow(
        engine,
        args.output_dir or default_output_directory(),
        output_directory_pinned=args.output_dir is not None,
    )
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
