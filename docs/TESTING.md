# Testing

SPEC.md §22's deliverable: how to run each tier, how to set up the self-hosted GPU
runner, and how to read what comes out.

The one idea behind all of it is SPEC.md §20.1:

> GitHub Actions `windows-latest` for build + unit tests; a self-hosted runner on the
> reference rig for all GPU/capture/soak tests (**hosted runners have no real GPU and
> will silently pass meaningless tests**).

A hosted runner does not *fail* the hardware tier. It passes it, having proved nothing —
which is why the split is a rule rather than an optimisation, and why
`gui/tests/test_workflows.py` asserts it rather than trusting anyone to remember.

---

## 1. The tiers

| Tier | Command | Where |
|---|---|---|
| CPU | `ctest --preset windows-msvc-release -LE gpu --output-on-failure` | anywhere |
| GPU | `ctest --preset windows-msvc-release -L gpu --output-on-failure` | the rig |
| GPU, one process | `build\windows-msvc-release\bin\fc_gpu_tests.exe` | the rig |
| Python, hardware-free | `pytest gui/tests -m "not engine"` | anywhere |
| Python, engine-backed | `pytest gui/tests -m engine` | the rig |
| Chaos | `fc_gpu_tests.exe --gtest_filter=ChaosTest.*` | the rig |
| Soak | `fc_gpu_tests.exe --gtest_filter=SoakTest.*` | the rig |
| Everything | `ctest --preset windows-msvc-release --output-on-failure` | the rig |

SPEC.md §21.2 makes the **unfiltered** `ctest` run the contract. The two split forms in
CLAUDE.md §3 are the same thing divided, for iterating.

### The chaos tier (SPEC.md §20.1)

Randomised fault injection against the real chain, asserting the one thing that must hold
however the faults land: **a valid, playable file comes out** (CLAUDE.md §1).

Every other test in the suite injects one fault from a healthy state. This one injects
several, in an order nobody chose, while the disk is already stalling — including
`DEVICE_REMOVED` and `ACCESS_LOST` back to back, so the second arrives during the recovery
from the first. Those are states no single-fault test visits and the ones a genuinely
failing machine produces.

**The seed is printed on every run, passing or failing**, and `FC_CHAOS_SEED` replays it:

```powershell
$env:FC_CHAOS_SEED = "3130200536"   # the number the failing run printed
$env:FC_CHAOS_SECONDS = "1800"      # §20.1's 30-minute form
.\build\windows-msvc-release\bin\fc_gpu_tests.exe --gtest_filter=ChaosTest.*
```

A randomised test whose failure cannot be replayed is an anecdote, so **check that the
replay works after any change to `seed_from_env`** — BUG-056 was that reader silently
truncating every seed above 2147483647, which is half of them, while printing an
instruction that did not work.

**Both durations pass as of 2026-09-08.** The 30-minute form found BUG-057 — the engine
reporting a valid recording as lost whenever its pipeline could not be stopped — and now
covers the fix. Note what a green run means: the tier asserts the prime directive and
nothing else, and the passing run produced **262 s of video from a 1800 s recording**.
BUG-058 is open on the stall behind that, so do not read a green chaos run as a healthy
engine.

The run also fails if it injected nothing, or if the disk stall caused no queue drops.
Both guards exist because both states have happened: a schedule that drew no faults, and a
stall mild enough that §20.1's "queue saturation" was named in the header and absent from
the run.

### Run the GPU tier in both forms

Not redundant. **BUG-049 was a flaky green**: it passed when ctest gave each test its own
process and failed when the whole suite shared one, because DDA re-emits an unpopulated
copy target on timeout and the test asserted on those frames. Per-process isolation hid a
real defect, so the single-process run is the stricter of the two and the one that has
caught something.

---

## 2. Long-form runs

CLAUDE.md §3 gives every long test an environment override, so the routine suite stays
quick and the exit-criterion form is one variable away.

| Variable | Test | Routine default | Exit-criterion value |
|---|---|---|---|
| `FC_AV_SYNC_SECONDS` | `test_av_sync` | 65 | `1800` (M4's 30 min) |
| `FC_LOOPBACK_SECONDS` | `test_loopback_recording` | 12 | `1800`, real time |
| `FC_AUDIO_DRIFT_MINUTES` | `AudioPathTest` drift run | 30 | — |
| `FC_SUSTAINED_SECONDS` | `test_sustained_60fps` | 20 | `600` (§20 row 6), real time |
| `FC_MULTITRACK_SECONDS` | `test_multitrack` row 14 | 20 | `1800` synthetic — 376 s wall |
| `FC_CHAOS_SECONDS` | `ChaosTest` | 30 | `1800` (§20.1's 30 min), real time |
| `FC_SOAK_SECONDS` | `SoakTest` | 120 | `14400` (§20.1's 4 h), real time |

`soak.yml` sets these weekly. **SPEC.md §20.1's four-hour soak form is not scheduled**:
`FC_SOAK_SECONDS` defaults to 120 there too, because four hours inside a weekly job that
already runs several long forms would push it past its timeout. Run the exit-criterion form
deliberately:

```powershell
$env:FC_SOAK_SECONDS = "14400"
.\build\windows-msvc-release\bin\fc_gpu_tests.exe --gtest_filter=SoakTest.*
```

### What the soak tier measures, and what it leaves to others

§20.1 names three soak clauses. **Only the leak clauses are the soak tier's.** Drift is
`AvSyncTest`'s and already has its own long form (`FC_AV_SYNC_SECONDS=1800`, worst offset
−187 µs over 1800 marks); re-deriving it would mean decoding four hours of 1080p to answer
a question a sharper test already answers.

**The growth assertion is on total megabytes, not on an extrapolated rate**, and that is
deliberate. §20.1's "5 MB/hour" is written for a four-hour run, where it is a 20 MB budget
that a real leak dwarfs. At the routine 120 s it is 0.17 MB — smaller than the noise. A
clean 90-second run measured a **0.75 MB** working-set wobble, which becomes "40 MB/hour"
purely by multiplying by 65, and failed a budget it had not violated. The test now asserts
`growth < 5 MB/hour × elapsed + 3 MB of jitter allowance`; over four hours the allowance is
13% of the budget and irrelevant.

Measured on the reference rig at 90 s: working set **250.4 → 250.9 MB**, grew **0.50 MB**
against a 3.08 MB budget, handles **1586–1587**, 5406 frames captured and encoded, none
dropped.

**After any change to `AudioTimeline`, run `FC_LOOPBACK_SECONDS=300` before calling it
done.** BUG-042's first attempt passed lint, both tiers and a 40-second real-endpoint
recording, and produced **76 seconds of audio in a 300-second recording**. Five minutes is
the shortest run that failed it.

---

## 3. Two ways to invalidate a hardware run

Both cost real time during M9.6 — three runs discarded, none of them because of the code.

**Concurrency.** The encoder needs the CPU. A GPU run started alongside `lint.ps1`, whose
clang-tidy pass runs 16-way, produced seven failures all reading `submitted 60, encoded 0,
queue-dropped 52` — an encoder starved, not a bug. Run the hardware tier with nothing else
running, including the Python suite and including lint.

**A locked workstation.** A locked session composites the secure desktop, so WGC captures
a static screen. The signature is `decoded_frames=1, duration_s=0.017`. Keep the machine
unlocked and awake for the whole run.

**Expect noise.** `RealTargetTest.*` and §20 row 15 play audible tones through the
speakers, and several cases put a full-screen test pattern on the display.
`fc_audio_target` is a real WASAPI render client — §8.6's per-application capture is keyed
on a process id, so there is no way to exercise it without a process actually playing
something. The noise is the test working.

---

## 4. The self-hosted runner

`gpu.yml` and `soak.yml` target `[self-hosted, windows, framecapture-rig]`.

### 4.1 Prerequisites on the rig

- Visual Studio 2022 Build Tools with the C++ workload, and Windows SDK 10.0.22621+.
- Python 3.11+ on `PATH` (the `py` launcher is what `bootstrap.ps1` uses).
- Git.
- Both adapters present and drivers installed, a real display attached, and a working
  audio render endpoint. Every one of those is something a test asserts against.

### 4.2 Registering it

1. Repository → **Settings** → **Actions** → **Runners** → **New self-hosted runner**,
   Windows x64. GitHub shows a download and a `config.cmd` line carrying a registration
   token; the token is short-lived, so do this in one sitting.
2. When `config.cmd` asks for labels, add **`framecapture-rig`**. `self-hosted` and
   `windows` are added automatically, and the workflows require all three.
3. Start it with **`run.cmd`**, from a logged-in interactive session.

### 4.3 Do not install it as a Windows service

This is the one setup detail that will waste a day if it is got wrong.

A service runs in **session 0**, which has no desktop. WGC and DDA capture a desktop, so
every capture test would run against nothing — and the failure looks exactly like the
locked-workstation signature above: a file that decodes to one frame, or black frames with
`S_OK`. It does not look like a configuration mistake.

So: `run.cmd` in an interactive session on an unlocked machine. Practically that means
disabling the lock screen and sleep on the rig, and accepting that it is a machine
dedicated to this rather than someone's daily driver — the nightly run makes noise at
03:00 UTC.

### 4.4 What the runner may not do

`gpu.yml` and `soak.yml` have **no `pull_request` trigger**, deliberately. A self-hosted
runner executing pull-request code is arbitrary code execution on the owner's machine, and
a fork PR is code from a stranger. There is no `if:` guard to weaken because the trigger
does not exist. `test_workflows.py` asserts this.

Both also share `concurrency: framecapture-rig` with `cancel-in-progress: false`: one job
at a time, and never kill one mid-recording — a cancelled recording leaves a capture device
open and the next run inherits it.

---

## 5. CI

| Workflow | Trigger | Runner | What |
|---|---|---|---|
| `ci.yml` | PR, push to `main` | `windows-latest` | gates, lint, build, CPU tier, hardware-free pytest |
| `gpu.yml` | nightly 03:00 UTC, manual | the rig | both tiers, both GPU forms, all of pytest |
| `soak.yml` | Sunday 04:00 UTC, manual | the rig | the long forms |

### 5.1 `lint.ps1 -Strict`

CI passes `-Strict`, and it matters more than it looks. By default `lint.ps1` *warns* and
continues when clang-tidy is absent, when there is no compile database, or when the GUI
venv is missing — deliberately, so a C++-only contributor gets the passes that apply to
them instead of a red run they learn to ignore.

In CI that leniency is a lint job that reports clean while checking nothing, because there
a missing tool means the workflow forgot to install it. `-Strict` turns every skip into a
failure. Measured on the reference rig: with `build/tidy/compile_commands.json` removed,
`-Strict` exits 1 naming the skip, and the default run exits 0 with a warning.

### 5.2 The repository gates

`scripts/pr-gates.ps1`, run by `ci.yml`'s `gates` job:

- **LICENSE must exist.** CLAUDE.md §9 records that libx264 is in, `--enable-gpl` is set,
  and the distributed binary is therefore GPLv2. **This gate fails today**, and that is
  the gate working rather than a bug in it: the licence decision is the owner's, and the
  obligation is already incurred. It is not asking for a file to be invented to make it
  green.
- **Documentation pairs.** A change that adds a config key must touch `docs/CONFIG.md`; one
  that adds an `FcError` must touch `docs/ERROR_CODES.md` (CLAUDE.md §10, SPEC.md §22).

Both documentation checks read the **diff**, not the file list: they fire on an added
`KeySpec{...}` line or an added `X(NAME, code, ...)` enumerator. Triggering on "a file
under `engine/core/config/` changed" would fire on a comment fix and teach everyone to add
a meaningless CONFIG.md edit to silence it, and a gate that cries wolf gets routed around.

Because those patterns match a declaration *form*, they can go stale silently — reformat
the table and the gate reports clean forever. `pr-gates.ps1 -SelfTest` runs the gate's own
patterns against the real declarations and fails if either matches nothing. Measured:
**54 config keys, 107 error codes.**

### 5.3 The vcpkg cache — the thing that decides whether CI is usable

A cold build compiles FFmpeg with x264 from source. Uncached, every PR is an hours-long
job, and CI that slow is CI nobody waits for — which is worse than no CI.

`ci.yml` sets `VCPKG_BINARY_SOURCES` to a workspace directory and caches it with
`actions/cache`, keyed on `vcpkg.json` (the dependency set *and* the baseline) plus
`ports/**` (the FFmpeg feature whitelist). A change to either produces different binaries,
so both belong in the key; without `ports/**`, editing the whitelist would silently restore
the previous build's FFmpeg.

> **Cold and warm build times: not yet measured.**
>
> The plan asks for both to be recorded here, and an unmeasured cache is a cache nobody
> notices has stopped working. Neither number exists yet because no workflow run has
> happened — the workflows are written and locally verified but have never been pushed.
>
> Record them here after the first two runs on `main`: the first is cold, the second is
> warm. If the warm number ever approaches the cold one, the key has stopped matching.

If the cache outgrows the 10 GB per-repository limit, the upgrade is vcpkg's
NuGet/GitHub Packages binary-cache backend. It needs a token; the files backend does not,
which is why it is the starting point.

---

## 6. Reading the results

**Report measurements, not adjectives** (CLAUDE.md §6). "Green" is worth less than "worst
offset −187 µs against a 20 ms tolerance, 1800 marks" — a number is what the next person
compares against when it changes. Several GPU cases print `[ MEASURED ]` lines for exactly
this; they are the useful part of the log.

`docs/ACCEPTANCE.md` maps SPEC.md §20's failure-mode rows to what was actually measured,
and is where a row's evidence lives once its test is green.

The most recent full run on the reference rig, for comparison:

| Gate | Result | Time |
|---|---|---|
| CPU tier | 434/434 | 80.7 s |
| GPU tier (ctest) | 199/199 | 780.2 s |
| GPU tier (one process) | 198 passed, 1 skipped, 0 failed | 619.3 s |
| pytest, hardware-free | 306/306 | 33.1 s |
| pytest, engine-backed | 17/17 | 94.9 s |

The single skip is `FinalizeThroughputTest.MeasureAGivenFile`, a measurement tool that
needs a file named on the command line and skips itself without one. It is not a test that
failed to run.
