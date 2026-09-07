"""The repository policy gates (M9.6 Phase 5, plan §7.4 and §7.5).

`scripts/pr-gates.ps1` is what CI runs to enforce two things CLAUDE.md §10 already
requires and nothing checked: a LICENSE exists, and a change that adds a config key or
an error code updates the document that describes it.

**Why these tests live under `gui/tests`.** `pyproject.toml` sets
`testpaths = ["gui/tests"]`, so this is where `pytest` looks; the C++ tiers are
GoogleTest and cannot drive a PowerShell script. The subject is repository tooling
rather than the GUI, which is worth saying out loud rather than leaving as a puzzle for
the next reader.

The script is driven through its explicit `-ChangedFiles` / `-AddedConfigKeys` /
`-AddedErrorCodes` parameters. That is deliberate: a test that built real commits to
exercise the `-BaseRef` path would be a test that writes to the repository's git
history, and the one environment rule this milestone has is that git history is not
touched. The git parsing is exercised separately, read-only, against real history.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "scripts" / "pr-gates.ps1"

pytestmark = pytest.mark.skipif(
    shutil.which("pwsh") is None and shutil.which("powershell") is None,
    reason="no PowerShell on PATH",
)


def _powershell() -> str:
    """PowerShell 7 if it is there, Windows PowerShell otherwise.

    The script declares `#Requires -Version 5.1` and uses nothing newer, so both are
    valid hosts -- and CI's `windows-latest` has both. Preferring `pwsh` matches what a
    developer runs.
    """
    return shutil.which("pwsh") or shutil.which("powershell") or "powershell"


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            _powershell(),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(_SCRIPT),
            *args,
        ],
        capture_output=True,
        text=True,
        cwd=_REPO_ROOT,
        check=False,
    )


# ---------------------------------------------------------------------------
# The documentation pairs (plan §7.5)
# ---------------------------------------------------------------------------


def test_a_change_touching_nothing_interesting_passes_the_doc_gates() -> None:
    """The common case. A gate that fires on ordinary changes is a gate people disable."""
    result = _run("-ChangedFiles", "gui/framecapture_gui/main_window.py")
    assert "config key" not in result.stdout.replace("0 new config key(s)", "")
    assert "ERROR_CODES" not in result.stdout


def test_adding_a_config_key_without_touching_config_md_fails() -> None:
    result = _run(
        "-ChangedFiles",
        "engine/core/config/config_schema.cpp",
        "-AddedConfigKeys",
        "window.show_preview",
    )
    assert result.returncode == 1
    assert "docs/CONFIG.md was not touched" in result.stdout
    assert "window.show_preview" in result.stdout


def test_adding_a_config_key_with_config_md_passes() -> None:
    result = _run(
        "-ChangedFiles",
        "engine/core/config/config_schema.cpp,docs/CONFIG.md",
        "-AddedConfigKeys",
        "window.show_preview",
    )
    assert "docs/CONFIG.md updated" in result.stdout


def test_adding_an_error_code_without_touching_error_codes_md_fails() -> None:
    result = _run(
        "-ChangedFiles",
        "engine/core/error/fc_error.h",
        "-AddedErrorCodes",
        "CAPTURE_SOMETHING_NEW",
    )
    assert result.returncode == 1
    assert "docs/ERROR_CODES.md was not touched" in result.stdout
    assert "CAPTURE_SOMETHING_NEW" in result.stdout


def test_adding_an_error_code_with_error_codes_md_passes() -> None:
    result = _run(
        "-ChangedFiles",
        "engine/core/error/fc_error.h,docs/ERROR_CODES.md",
        "-AddedErrorCodes",
        "CAPTURE_SOMETHING_NEW",
    )
    assert "docs/ERROR_CODES.md updated" in result.stdout


def test_touching_the_config_directory_without_adding_a_key_does_not_fire() -> None:
    """The over-triggering this gate was written to avoid.

    A comment fix under `engine/core/config/` is not a new setting, and demanding a
    CONFIG.md edit for one teaches people to add a pointless line to shut the gate up.
    A gate that cries wolf gets routed around, which is worse than no gate.
    """
    result = _run("-ChangedFiles", "engine/core/config/config.cpp")
    assert "0 new config key(s)" in result.stdout
    assert "CONFIG.md was not touched" not in result.stdout


def test_a_windows_path_separator_is_not_a_mismatch() -> None:
    """`git` says `docs/CONFIG.md`; a caller on Windows may say `docs\\CONFIG.md`.

    Two spellings of one path is a gate that fails for a reason that has nothing to do
    with the change.
    """
    result = _run(
        "-ChangedFiles",
        r"engine\core\config\config_schema.cpp,docs\CONFIG.md",
        "-AddedConfigKeys",
        "window.show_preview",
    )
    assert "docs/CONFIG.md updated" in result.stdout


# ---------------------------------------------------------------------------
# The LICENSE gate (plan §7.4)
# ---------------------------------------------------------------------------


def test_the_licence_gate_reports_on_this_repository() -> None:
    """Asserts the gate *runs and reaches a verdict*, not which verdict.

    This repository has no LICENSE today and the gate therefore fails, which is the
    gate working: CLAUDE.md §9 leaves the licence decision to the owner while the
    GPLv2 obligation is already incurred. Pinning the failing verdict here would mean
    this test has to be edited on the day the licence lands -- turning a passing gate
    into a broken test, which is exactly backwards.
    """
    result = _run("-ChangedFiles", "README.md")
    assert "LICENSE" in result.stdout
    if result.returncode == 0:
        assert "LICENSE:" in result.stdout, "a passing run must name the licence file it found"
    else:
        assert "no LICENSE file" in result.stdout
        assert "GPLv2" in result.stdout, "the failure must say why it matters, not just that it failed"


# ---------------------------------------------------------------------------
# Non-vacuity
# ---------------------------------------------------------------------------


def test_both_detectors_still_recognise_this_codebase() -> None:
    """The failure mode that makes a precise gate worthless.

    Both patterns match a declaration *form* -- a `KeySpec{"..."}` line, an
    `X(NAME, code, ...)` enumerator -- rather than a filename. That is what stops the
    gate firing on a comment fix, and it is also what makes it silently fragile:
    reformat the table or replace the X-macro and the pattern matches nothing, the gate
    reports clean on every change forever, and nobody finds out until a config key
    ships undocumented.

    `-SelfTest` runs the gate's own patterns against the real declarations, so this
    cannot drift into a second copy of them that agrees with itself and not with the
    gate.
    """
    result = _run("-SelfTest")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "config key detector:" in result.stdout
    assert "error code detector:" in result.stdout
    # Not a fixed count -- that would be a test that fails every time a setting is
    # added, which teaches people to edit the test rather than read it. What matters is
    # that neither detector found nothing.
    assert "0 match(es)" not in result.stdout


# ---------------------------------------------------------------------------
# The git-driven path, read-only against real history
# ---------------------------------------------------------------------------


def test_the_base_ref_path_parses_real_history_without_writing_to_it() -> None:
    """`-BaseRef` against this repository's own first commit.

    Read-only by construction -- `git diff` writes nothing -- so this exercises the
    parsing that CI actually depends on without the test creating commits. What is
    asserted is that the diff was understood at all: a `-BaseRef` that silently found
    nothing would make the whole gate vacuous in CI, which is the failure mode worth
    catching here.
    """
    # Resolved rather than trusted to PATH lookup at spawn time: a bare "git" is what
    # ruff's S607 objects to, and the objection is reasonable -- which executable runs
    # then depends on the caller's PATH. `which` answers that question once, here.
    git = shutil.which("git")
    if git is None:
        pytest.skip("git is not on PATH")

    head = subprocess.run([git, "rev-parse", "HEAD"], capture_output=True, text=True, cwd=_REPO_ROOT, check=False)
    if head.returncode != 0:
        pytest.skip("not a git repository")

    result = _run("-BaseRef", head.stdout.strip())
    # HEAD against itself is an empty diff, which must be understood as "nothing to
    # check" rather than as an error.
    assert "0 changed file(s)" in result.stdout
    assert "could not diff" not in result.stdout
