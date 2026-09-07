"""The CI workflows, as assertions (M9.6 Phase 5, SPEC.md §20.1).

A workflow file has no compiler and no test suite of its own: it is checked by being
run, and a mistake in one is discovered on the day it matters. Three of the properties
these files must hold are worth more than that.

**The one that matters most.** SPEC.md §20.1: *"a self-hosted runner on the reference rig
for all GPU/capture/soak tests (hosted runners have no real GPU and will silently pass
meaningless tests)."* A hosted runner that ran the GPU tier would not fail — it would
report green, and every hardware guarantee this project makes would rest on a suite that
proved nothing. That is the failure this file exists to make impossible.

**The second** is the mirror of it: a self-hosted runner triggered by a pull request is
arbitrary code execution on the owner's machine, and a fork PR is code from a stranger.

**The third** is dull and catches the common slip: a run step that forgets `shell: pwsh`
gets GitHub's default shell, and a job with no timeout can hang until the six-hour
platform limit.

Like `test_pr_gates.py`, this is repository tooling rather than GUI code; it lives here
because `pyproject.toml` sets `testpaths = ["gui/tests"]`.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parents[2]
_WORKFLOWS = _REPO_ROOT / ".github" / "workflows"

#: Every job in these runs on hardware the project owns.
_SELF_HOSTED = ("gpu.yml", "soak.yml")


def _load(name: str) -> dict[Any, Any]:
    """The parsed workflow.

    `dict[Any, Any]`, not `dict[str, Any]`, and that is not laziness -- see `_triggers`
    for why one of the keys is genuinely not a string.
    """
    document = yaml.safe_load((_WORKFLOWS / name).read_text(encoding="utf-8"))
    assert isinstance(document, dict), f"{name} is not a mapping"
    return document


def _triggers(document: dict[Any, Any]) -> dict[str, Any]:
    """The `on:` block.

    YAML 1.1 resolves a bare `on` to the boolean `True` -- the "Norway problem", after
    the same rule that turns the country code `NO` into `false`. GitHub reads these files
    as YAML 1.2, where it stays the string `"on"`, so the workflow is correct and it is
    this parser that has to accommodate both spellings. Changing the file to suit PyYAML
    would be a test dictating the format of the thing it tests.
    """
    found = document.get("on", document.get(True))
    assert isinstance(found, dict), "no on: block"
    return found


def _workflow_files() -> list[Path]:
    files = sorted(_WORKFLOWS.glob("*.yml"))
    assert files, "no workflows found"
    return files


def _commands(step: dict[str, Any]) -> str:
    """A run step's actual commands, with comment lines removed.

    Comments in a run block are prose about the commands, and prose quotes the thing it
    is warning against -- "`-LE gpu`, never `-L gpu`" is a comment that would trip a
    naive search for `-L gpu` and report the exact violation it exists to prevent. A
    check that fires on its own documentation gets deleted.
    """
    run = step.get("run", "")
    return "\n".join(line for line in run.splitlines() if not line.strip().startswith("#"))


def _jobs(name: str) -> dict[str, Any]:
    jobs = _load(name).get("jobs", {})
    assert isinstance(jobs, dict) and jobs, f"{name} has no jobs"
    return jobs


# ---------------------------------------------------------------------------
# The rule: no hosted runner ever runs the hardware tier
# ---------------------------------------------------------------------------


def _is_hosted(runs_on: Any) -> bool:
    """A hosted runner is a plain label; the rig is a list including `self-hosted`."""
    if isinstance(runs_on, list):
        return "self-hosted" not in runs_on
    return "self-hosted" not in str(runs_on)


def test_ci_runs_on_hosted_runners_only() -> None:
    """If this ever changes, the assertion below stops meaning what it says."""
    for name, job in _jobs("ci.yml").items():
        assert _is_hosted(job["runs-on"]), f"ci.yml job {name} is self-hosted"


def test_ci_never_runs_the_gpu_tier() -> None:
    """SPEC.md §20.1. A hosted runner has no real GPU, so this would pass meaninglessly.

    Both spellings are checked: `ctest -L gpu`, and the GPU test binary invoked directly.
    """
    for name, job in _jobs("ci.yml").items():
        for step in job.get("steps", []):
            commands = _commands(step)
            assert "-L gpu" not in commands, f"ci.yml {name}/{step.get('name')} selects the gpu label"
            assert "fc_gpu_tests" not in commands, f"ci.yml {name}/{step.get('name')} runs the GPU binary"


def test_ci_excludes_the_gpu_label_explicitly() -> None:
    """Not merely absent: the CPU tier is selected by `-LE gpu`.

    A bare `ctest` would run everything, including the hardware tier, which is the same
    violation arrived at by omission rather than by intent.
    """
    ctest_steps = [
        _commands(step)
        for job in _jobs("ci.yml").values()
        for step in job.get("steps", [])
        if "ctest" in _commands(step)
    ]
    assert ctest_steps, "ci.yml runs no tests at all"
    for commands in ctest_steps:
        assert "-LE gpu" in commands, f"ctest without -LE gpu: {commands.strip()}"


def test_ci_does_not_run_the_engine_backed_python_cases() -> None:
    """The same rule applied to pytest.

    The `engine` marker spawns a real engine and records the screen. On a hosted runner
    there is no GPU and no composited desktop, and the recording comes back as a single
    frame -- the `decoded_frames=1, duration_s=0.017` signature M9.6 hit twice against a
    locked workstation.
    """
    pytest_steps = [
        _commands(step)
        for job in _jobs("ci.yml").values()
        for step in job.get("steps", [])
        if "pytest" in _commands(step)
    ]
    assert pytest_steps, "ci.yml runs no Python tests"
    for commands in pytest_steps:
        assert "not engine" in commands, f"pytest without a marker filter: {commands.strip()}"


# ---------------------------------------------------------------------------
# The rule: a pull request cannot reach the rig
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("name", _SELF_HOSTED)
def test_a_self_hosted_workflow_is_not_triggerable_by_a_pull_request(name: str) -> None:
    """Arbitrary code execution on the owner's machine, otherwise.

    Asserted on the *trigger* rather than on an `if:` guard, because a condition is a
    thing somebody can weaken in a hurry and a missing trigger is not.
    """
    assert "pull_request" not in _triggers(_load(name))
    assert "pull_request_target" not in _triggers(_load(name))


@pytest.mark.parametrize("name", _SELF_HOSTED)
def test_the_rig_runs_one_job_at_a_time(name: str) -> None:
    """Two recordings competing for the encoder is the CPU-starvation signature that
    invalidated three of M9.6's hardware runs (`submitted 60, encoded 0`).

    `cancel-in-progress: false` as well: killing a run mid-recording leaves a capture
    device open, and the next run inherits it.
    """
    concurrency = _load(name)["concurrency"]
    assert concurrency["group"] == "framecapture-rig"
    assert concurrency["cancel-in-progress"] is False


@pytest.mark.parametrize("name", _SELF_HOSTED)
def test_a_self_hosted_workflow_says_it_makes_noise(name: str) -> None:
    """CLAUDE.md §3 warns that these cases play audible tones through the speakers.

    A scheduled job that makes noise at 03:00 on somebody's daily driver is a surprise
    worth one comment, and the comment is only useful if it is actually there.
    """
    text = (_WORKFLOWS / name).read_text(encoding="utf-8").lower()
    assert "audible" in text or "tones" in text


# ---------------------------------------------------------------------------
# The dull ones that catch the common slip
# ---------------------------------------------------------------------------


def test_every_workflow_parses() -> None:
    for path in _workflow_files():
        assert yaml.safe_load(path.read_text(encoding="utf-8")), f"{path.name} is empty"


def test_every_job_has_a_timeout() -> None:
    """Without one a hung job runs to GitHub's six-hour limit, holding the rig."""
    for path in _workflow_files():
        for name, job in _load(path.name).get("jobs", {}).items():
            assert job.get("timeout-minutes"), f"{path.name}/{name} has no timeout-minutes"


def test_every_run_step_declares_powershell() -> None:
    """The scripts are PowerShell and the paths are Windows paths."""
    for path in _workflow_files():
        for name, job in _load(path.name).get("jobs", {}).items():
            for step in job.get("steps", []):
                if "run" in step:
                    assert step.get("shell") == "pwsh", f"{path.name}/{name}/{step.get('name')} has no shell: pwsh"


def test_every_step_does_something() -> None:
    for path in _workflow_files():
        for name, job in _load(path.name).get("jobs", {}).items():
            steps = job.get("steps", [])
            assert steps, f"{path.name}/{name} has no steps"
            for step in steps:
                assert "run" in step or "uses" in step, f"{path.name}/{name}/{step.get('name')} is empty"


def test_ci_fetches_enough_history_for_the_merge_base() -> None:
    """`pr-gates -BaseRef` diffs against the merge base, which a shallow clone lacks.

    Without `fetch-depth: 0` the documentation gate reports "could not diff" on every
    pull request -- a gate that fails for its own reasons teaches people to ignore it.
    """
    gates = _jobs("ci.yml")["gates"]
    checkout = next(step for step in gates["steps"] if str(step.get("uses", "")).startswith("actions/checkout"))
    assert checkout["with"]["fetch-depth"] == 0
