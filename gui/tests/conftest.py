"""Shared fixtures for the GUI tests.

The engine binary is found rather than configured: a path in a config file is a path
that goes stale, and the tests that need a real engine should skip loudly when there
isn't one rather than fail with a confusing error about a missing file.
"""

from __future__ import annotations

import os
from collections.abc import Iterator
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]

#: Searched in this order. Release first because that is what a developer most likely
#: built last and what CI builds.
_PRESETS = ("windows-msvc-release", "windows-msvc-relwithdebinfo", "windows-msvc-debug")


def find_engine() -> Path | None:
    """The most recently built ``framecapture-engine.exe``, or ``None``.

    Newest wins rather than first: running the GUI tests against a stale Release build
    while iterating on Debug would report on code that is no longer there.
    """
    override = os.environ.get("FC_ENGINE_EXE")
    if override:
        candidate = Path(override)
        return candidate if candidate.is_file() else None

    found = [
        path
        for preset in _PRESETS
        if (path := _REPO_ROOT / "build" / preset / "bin" / "framecapture-engine.exe").is_file()
    ]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


@pytest.fixture(scope="session")
def engine_path() -> Path:
    """Path to a built engine, skipping the test if there is none.

    Skipped rather than failed: the Python half of this repo is testable on a machine
    that has never run CMake, and most of these tests do not need an engine at all.
    """
    engine = find_engine()
    if engine is None:
        pytest.skip(
            "no framecapture-engine.exe found under build/*/bin -- "
            "run `cmake --build --preset windows-msvc-release --parallel`, "
            "or set FC_ENGINE_EXE"
        )
    return engine


@pytest.fixture
def output_path(tmp_path: Path) -> Iterator[Path]:
    """A recording destination inside pytest's per-test temporary directory."""
    yield tmp_path / "gui_test.mkv"
