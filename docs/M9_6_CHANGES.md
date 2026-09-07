# M9.6 — what changed, Phases 0–6

A record of the work, for review and for whoever picks this up next. The **plan** is
`M9_6_PLAN.md`; the **user-facing** account is `CHANGELOG.md`; the **defects** are
`ENGINEERING_LOG.md` BUG-049 … BUG-055. This is the engineering summary that ties them
together and states, plainly, what has and has not been proven.

**Nothing here is committed.** 36 files modified, 29 added, on `main`.

| | |
|---|---|
| Phases complete | **all six** — 0 (foundations), 1 (pill), 2 (toasts), 3 (hotkeys), 4 (menus), 5 (CI/CD), 6 (acceptance + docs) |
| Phases remaining | none |
| Production code added | ~3,400 lines across 14 new modules |
| Tests added | 254 pytest cases + 34 C++ cases |
| Defects found and fixed | 7 (BUG-049 … BUG-055), plus three in Phase 5's own tooling (§10) |
| Verified | lint clean (`-Strict`) · CPU 434/434 · GPU 199/199 (ctest) and 198+1 skipped (single process) · pytest 306/306 + 17 engine-backed — §6 |
| Open questions | **none** — §0.3's three are all answered (§8) |
| **Not verified** | **Phase 5's workflows have never run on GitHub** — nothing is committed or pushed. See §9 |

---

## 1. The two rules everything else hangs off

M9.6 puts pixels on screen *while the screen is being recorded*, which no earlier
milestone did. Two properties had to hold before any of it was worth building:

**Rule A — nothing drawn may reach the output file.** Not the overlay, and not the black
rectangle that is the naive way of hiding it. Answered in Phase 0 by measurement rather
than assumption; see §2.

**Rule B — nothing drawn may block the GUI thread.** The pill is the only control on
screen during a fullscreen recording, and one that stops repainting is indistinguishable
from a crashed recorder. Answered by making `stop_record` asynchronous.

---

## 2. The gate: capture exclusion, measured

`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` — value `0x11`, introduced in
Windows 10 build 19041, which is *exactly* SPEC.md §1's minimum supported build, so there
is no OS-version fallback to write.

The near-miss that produces the reported "black cutout" is `WDA_MONITOR` (`0x01`): it also
hides a window from captures, by painting **black** into them. One hex digit apart.

`tests/integration/test_overlay_exclusion.cpp` (GPU tier, 6 cases) settles it against a
real capture on both backends. Stable over four consecutive full-suite runs in one
process:

| case | backend | mean_luma | overlay_fraction |
|---|---|---|---|
| unstamped (control) | WGC / DDA | 145.8 / 145.8 | 1.0000 |
| `WDA_MONITOR` | WGC | 0.0 | 0.0000 |
| `WDA_EXCLUDEFROMCAPTURE` | WGC / DDA | 255.0 / 255.0 | 0.0000 |

145.8 is exactly the BT.709 luma of the test overlay's orange; 255.0 is the fixture's
white bar coming through intact. **DDA honours the affinity**, so the plan's §1.3 fallback
ladder (force WGC while an overlay is visible) is **not needed** and
`advanced.capture_backend` does not have to be narrowed.

`WDA_MONITOR` is now a banned pattern in `scripts/lint.ps1`, alongside a rule that
`SetWindowDisplayAffinity` may appear in exactly one module.

---

## 3. Contract changes

Four, all additive. Two landed in Phase 0, two in Phase 4.

### 3.1 IPC — `finalize_progress` (engine → GUI)

```json
{"event": "finalize_progress",
 "phase": "flushing|remuxing|validating|swapping|done",
 "percent": 47, "bytes_done": 601380864, "bytes_total": 1288490188,
 "output": "…\\FrameCapture_….mp4"}
```

Exists because SPEC.md §15.1 gives `stop_record` 30 seconds and BUG-046 measured why:
**~3.3 s of remux plus ~1 s of validate for a 1.2 GB recording.** Documented in
`IPC_PROTOCOL.md` §5.

Three properties worth knowing:

- **`percent` is monotonic and only `done` reaches 100**, and `done` is emitted by the
  service only after the validation gate passes. A bar at 100% is the claim that the
  recording is saved; a file that failed validation is not saved.
- **`bytes_done`/`bytes_total` are 0 in every phase but the remux**, which is the signal
  that a phase has no byte progress behind it. `validating` is a fixed ~1 s decode with no
  proportional quantity, and inventing one would be a progress bar lying about its
  progress. The GUI renders those phases as indeterminate.
- **MKV has no `remuxing` phase.** That is a container with no second pass, not a bar
  that broke.

Emitted while `stop_record` is still in flight — the pipe server handles one *request* at
a time, but `send_event` is a write and serialises independently.

### 3.2 Config — schema v1 → v2

New `[hotkeys]`, `[overlay]` and — from Phase 4 — `[window]` sections (documented in
`CONFIG.md`, with the migration history table). `[window]` joined v2 rather than starting a
v3 because v2 is unreleased; it is six booleans behind the View menu's panel toggles and
`always_on_top`. `migrate_1_to_2` is the chain's **first real link**; the machinery had
been implemented and tested against synthetic steps since M0 and never exercised.

The step deliberately **writes nothing** — both sections are new, every key has a default,
and an absent key already loads as its default. What it contributes is the version stamp
and the backup to `config.toml.bak.1` that §17 promises before a file is rewritten. It
exists as a step rather than the chain simply accepting a v1 document because
`run_migrations` refuses a version it has no step for, deliberately.

One pre-existing test had to change: `MigrationBackup.AnOutOfRangeVersionIsClamped…`
asserted "clamped ⇒ not migrated", which held only while 1 was both the clamp floor *and*
the current version. That coincidence ended at v2; the assertion was testing it rather
than the behaviour in the test's own title.

### 3.3 IPC — `version` on the handshake (Phase 4)

`hello`'s response gained `"version"`: the engine binary's own version, from the CMake
`project()` declaration. It is what Help ▸ About and the diagnostics summary report.

It is not cosmetic. The GUI's `__version__` is `1.0.0` and the engine's CMake version is
`0.1.0` — two artefacts with two version numbers that **already disagree**, and a bug
report that names the wrong one is a bug report against the wrong build. Additive under
§15.1, so a GUI reading an older engine sees an absent field and renders "unknown".

### 3.4 IPC — `recover` gets `stop_record`'s timeout (Phase 4), and §15.1 needs a word

SPEC.md §15.1 says requests time out at 5 s "except `stop_record`, 30 s, since
finalization is legitimately slow". `recover` runs `mux::recover`, which for MP4 is *the
same lossless remux a clean stop performs* — BUG-046 measured ~3.3 s of remux plus ~1 s of
validate for a 1.2 GB file. At 5 s the command would time out on every recording large
enough to be worth recovering.

§15.1 names only `stop_record` because `recover` had no caller when it was written. This
build gives `recover` 30 s in `ipc/protocol.py`'s `timeout_for`, and **the spec sentence
needs the owner's pen to match** — flagged here and in `IPC_PROTOCOL.md` rather than
silently diverging. It is a Phase 6 edit alongside the §20 rows.

---

---

## 4. What was built

### 4.1 New modules

| Module | Purpose |
|---|---|
| `overlay/exclusion.py` | The `WDA_EXCLUDEFROMCAPTURE` primitive, read-back verification, re-application on handle recreation, and a guard that stamps popups an overlay opens |
| `overlay/surface.py` | One call that makes a widget an overlay: flags, `WS_EX_NOACTIVATE`, translucency, exclusion. Shared by pill and toasts so there is no path where a surface gets three of the four |
| `overlay/placement.py` | Corner anchoring, edge snapping, off-screen recovery, and the toast column's stack geometry. Pure |
| `overlay/elapsed.py` | The interpolated timeline clock. Pure |
| `overlay/pill.py` | The floating recording control |
| `overlay/toast_model.py` | Severity, dwell, the bounded coalescing queue. Pure |
| `overlay/toasts.py` | `Toast` + `ToastManager` |
| `settings/hotkey_editor.py` | The capture field and the Hotkeys settings section |
| `units.py` | `format_hms` / `format_size`, shared by the pill and the status panel |
| `diagnostics.py` | Phase 4. The pasteable summary and §18's zip bundle. Pure — no Qt, so every branch including "the engine is not answering" is testable headless |
| `recovery.py` | Phase 4. The `.fcrecover` scan §10.4 assigns to the GUI. Pure |
| `widgets/topology_dialog.py` | Phase 4. `get_gpu_topology` on screen, with `owns_output` stated in words |
| `scripts/pr-gates.ps1` | Phase 5. LICENSE and the documentation pairs, with a `-SelfTest` that proves the detectors are not vacuous |
| `.github/workflows/*.yml` | Phase 5. `ci.yml`, `gpu.yml`, `soak.yml` |
| `docs/TESTING.md` | Phase 5. SPEC.md §22's deliverable |

The pure/widget split is deliberate and paid for itself: an error storm of a thousand
messages is a millisecond, and "no two toasts overlap" is a property over 200 randomised
sequences rather than three hand-picked cases.

### 4.2 Changed behaviour in existing code

- **`stop_record` no longer blocks the GUI thread.** `IpcClient.request_async` adds one
  worker thread with a bounded backlog (8, drop-newest, logged). `EngineController`
  keeps a blocking `stop_recording_and_wait` for the two callers that genuinely need it —
  closing the window mid-recording, and restarting the engine — where there is no
  responsiveness left to protect.
- **`hotkeys.py` was rewritten** around `Action`, `BindingStatus` and an idempotent
  `apply`. The virtual-key table went from F1–F24/A–Z/0–9 to the full set a user would
  reach for; conflicts are now kept rather than returned once.
- **`StatusPanel`** now renders sizes through `format_size` — it read `1536.0 MB` for a
  1.5 GB recording, which is correct and reads as a broken counter.
- **`scripts/lint.ps1`** gained the overlay rules block (and a PowerShell bug of its own,
  fixed: `Get-Content` returns a bare string for a one-line file, which has no `.Count`).

### 4.3 Tests added

| File | Cases |
|---|---|
| `gui/tests/test_hotkeys.py` | 70 |
| `gui/tests/test_overlay_toasts.py` | 39 |
| `gui/tests/test_overlay_pill.py` | 34 |
| `gui/tests/test_units.py` | 21 |
| `gui/tests/test_overlay_exclusion.py` | 9 |
| `gui/tests/test_async_commands.py` | 9 |
| `tests/integration/test_overlay_exclusion.cpp` | 6 (gpu) |
| `tests/unit/test_finalize_progress.cpp` | 7 (cpu) |
| `gui/tests/test_menu_actions.py` | 41 (Phase 4) |
| plus `test_config.cpp` / `test_config_migration.cpp` additions | 21 (cpu) |
| plus `test_engine_control.py`'s handshake case, extended for `version` | (engine) |
| `gui/tests/test_pr_gates.py` | 10 (Phase 5) |
| `gui/tests/test_workflows.py` | 15 (Phase 5) |
| `gui/tests/test_overlay_responsiveness.py` | 6 (Phase 6, §20 row 20's named test) |

### 4.4 Phase 4 — the menus

Three menus that opened empty now have twenty-two actions between them, and the two SPEC
§15.1 commands the GUI had never called have their first call sites.

| Menu | Contents |
|---|---|
| **Edit** | Settings (`Ctrl+,`) · Copy output path · Copy diagnostics summary |
| **View** | Show preview / recording pill / notifications · Panels ▸ four · Always on top · Reset layout |
| **Tools** | Open output folder · Open logs folder · Export diagnostic bundle · GPU topology · Log level ▸ six · Engine ▸ Restart / Recover |

No Undo, no Redo, no "Light theme (soon)", nothing greyed out standing in for something
unfinished. `test_menu_actions` asserts that: every action enabled, no label containing
"soon"/"todo"/"not implemented", and every one of them triggered without raising.

**Six decisions worth recording.**

1. **`recover` needs a sidecar path, so the scan had to live somewhere.** §10.4 says the
   sidecar "lets **the GUI's** recovery path complete the job on next launch", and
   `fc::mux::find_recoverable` — the engine's own scan — is called by nothing but tests.
   So `recovery.py` globs `*.fcrecover` in the output directory and reads each record for
   its label only. The repair is entirely the engine's; this side never opens a media
   file, which is what keeps §16.1's "zero media processing in Python" true of a feature
   whose subject is a media file. An unreadable sidecar is still offered, labelled by
   filename: an unfinished recording the GUI cannot describe is still an unfinished
   recording.
2. **Recoveries are issued one at a time.** Each is a full remux, the pipe server handles
   one request at a time, and the async backlog is bounded at 8 with a drop-newest policy.
   Firing a directory's worth at once would have a user's ninth unfinished recording
   dropped by a queue doing exactly what CLAUDE.md hard rule 5 asks of it.
3. **`set_log_level` is session-only, and the menu says so.** `advanced.log_level` in
   `config.toml` is the durable setting; this is the knob you turn to reproduce a defect in
   the next thirty seconds. Persisting it would leave a user recording at TRACE for weeks
   because they once chased a bug. The menu ticks the level **in force**, not the
   configured one, because after one use those are different.
4. **`off` is not offered** even though the engine parses it. A menu item that silently
   stops a machine producing any diagnostics is one the user finds again only by reading
   the source.
5. **Copy output path changes its own label** rather than greying out. With no recording
   yet it reads "Copy output folder path"; after one it names the file. The alternative —
   an action disabled until you have recorded — gives the user nothing at the moment they
   are trying to find out where recordings go.
6. **Restart engine moved from File to Tools ▸ Engine**, next to Recover, per the plan.
   §16.1 requires a restart action, not a location. File keeps Quit.

**The diagnostic bundle** (§18) is a zip of the logs, `config.toml`, the newest minidump
*and its siblings*, and the summary itself. The siblings matter: `crash_handler.cpp` writes
`crash_<key>.dmp`, `crash_<key>.log` and `crash_report_<key>.json`, and a minidump without
the state-machine position beside it is missing what it was for. What could not be included
is reported in the dialog — a quietly smaller zip is one the recipient discovers is useless
after asking for it. Nothing is uploaded: §18's "Telemetry is local-only. No network
transmission, ever" is the whole design, and the bundle is a file the user chooses a
location for.

**The GPU topology dialog** exists because §5.1's hybrid-GPU trap — the display is wired to
the iGPU on a MUX-less laptop, so a capture device on the dGPU returns black with `S_OK` —
is diagnosed by exactly one fact, `owns_output`, and until now a user could not see it
without reading a log file. It is stated in words ("owns it" / "no"), and a topology where
*nothing* owns an output says outright that this is the condition that produces black
frames.

### 4.5 Phase 5 — CI/CD

Three workflows, two enforcement scripts, and one change to `lint.ps1` that is the
difference between a lint job and a lint job that means something.

| File | What |
|---|---|
| `.github/workflows/ci.yml` | PR and push to `main`, `windows-latest`. Gates, lint, build, CPU tier, hardware-free pytest |
| `.github/workflows/gpu.yml` | Nightly and manual, on the rig. Both tiers, both GPU forms, all of pytest |
| `.github/workflows/soak.yml` | Weekly and manual, on the rig. The long forms via CLAUDE.md §3's environment overrides |
| `scripts/pr-gates.ps1` | LICENSE presence and the documentation pairs (plan §7.4, §7.5) |
| `scripts/lint.ps1 -Strict` | Every "skipped" becomes a failure |
| `docs/TESTING.md` | SPEC.md §22's deliverable: the tiers, the runner setup, how to read the results |

**`-Strict` is the load-bearing change.** `lint.ps1` warns and continues when clang-tidy is
absent, when there is no compile database, or when the GUI venv is missing — deliberately,
so a C++-only contributor gets the passes that apply to them rather than a red run they
learn to ignore. In CI that same leniency is a lint job reporting clean while checking
nothing, because there a missing tool means the *workflow* forgot to install it. Without
`-Strict`, `ci.yml`'s lint step would have been decoration.

Measured, by hiding `build/tidy/compile_commands.json`: `-Strict` exits 1 naming the skip;
the default run exits 0 with a warning. Both behaviours, one command apart.

**The gates fire on the diff, not on the file list.** "Any file under
`engine/core/config/` changed" would fire on a comment fix and teach everyone to add a
meaningless `CONFIG.md` edit to silence it — a gate that cries wolf gets routed around,
which leaves it worse than absent. So they detect an *added* `KeySpec{...}` line or an
*added* `X(NAME, code, ...)` enumerator.

That precision is bought with fragility: the patterns match a declaration *form*, so
reformatting the table would make the gate match nothing and report clean forever.
`pr-gates.ps1 -SelfTest` runs the gate's own patterns against the real declarations and
fails if either finds zero. Measured: **54 config keys, 107 error codes**. Run against
M9.6's own working-tree diff, the detector finds all 20 config keys this milestone added
— so the gate would have fired on this very change set had `CONFIG.md` not been updated.

**The LICENSE gate fails today, and that is the gate working.** CLAUDE.md §9 records that
libx264 is in, `--enable-gpl` is set, and the distributed binary is therefore GPLv2 — an
obligation already incurred by a repository with no licence file. The gate does not ask
for a file to be invented to make it green; the licence decision is the owner's, and
§7.4's whole purpose is that the question cannot be quietly forgotten. See §9.

**Two rules asserted rather than trusted.** `gui/tests/test_workflows.py` parses the
workflow YAML and fails if `ci.yml` ever selects the `gpu` label, runs `fc_gpu_tests`, or
runs pytest without `not engine` — SPEC.md §20.1's "hosted runners have no real GPU and
will silently pass meaningless tests", which does not fail loudly but passes emptily. It
also fails if `gpu.yml` or `soak.yml` ever gains a `pull_request` trigger, which would be
arbitrary code execution from a fork on the owner's machine.

**One deviation from the plan, stated.** §7.3 asked for the four-hour soak to be carried
in `soak.yml` as a disabled job with the reason in a comment. It is carried as the comment
alone: a job with `if: false` renders in the Actions UI as a perpetually skipped job that
looks like something somebody broke, which is exactly the "looks finished and isn't" that
CLAUDE.md §7 bans. The information is identical; the false signal is not.

**Two deviations from §7.1's literal text**, both following the plan's own principle:

1. **`pytest` on the hosted runner excludes the `engine` marker.** §7.1 says
   `pytest gui/tests -v`. Those cases spawn a real engine and record the screen; a hosted
   runner has no GPU and no composited desktop, so they would produce the
   `decoded_frames=1, duration_s=0.017` signature — SPEC.md §20.1's meaningless pass,
   arrived at through Python instead of ctest. They run in `gpu.yml`.
2. **Lint runs after the configure, not first.** §7.1 orders it `lint` → `build` → `test`.
   clang-tidy cannot run without a compile database, and the database is produced by a
   configure. Lint still runs before the *build*, so a formatting failure does not wait on
   a forty-minute compile.

### 4.6 Phase 6 — acceptance and the spec amendments

SPEC.md now carries M9.6: **§20 has twenty-two rows** and §25's Definition of Done reads
"all 22"; §15.1's timeout sentence names `recover` alongside `stop_record`; §16.2 draws the
pill, the toasts and a populated menu bar; §16.4 gains a **Hotkeys** section; §16.5's hotkey
bullet points at it. `ACCEPTANCE.md` has a row apiece and a prose section saying what each
one does *not* cover.

**Row 20's named test did not exist.** `test_overlay_responsiveness` was named in the plan
and never written — the row's *substance* was covered by the engine-backed async case, but
the two measurements the milestone owed (paint cost, click latency) had never been taken.
Writing it is what made Phase 6 more than documentation.

**And the first version of it measured the wrong thing.** Paint cost came out at
**0.002 ms**, which is not a widget repaint — an event filter runs *before* the widget
handles the event, so what was being timed was the filter returning. Re-measured from
inside `paintEvent` via a subclass: **mean 0.124 ms, worst 0.459 ms** over 127 real paints.
A number that is wrong is worse than no number, and this one was wrong by sixty-fold in the
flattering direction.

The measurements the milestone owed, and their honest status:

| Owed | Status |
|---|---|
| Overlay paint cost | **0.124 ms mean, 0.459 ms worst** |
| Repaint rate while saving | **9.8 paints/s** against row 20's floor of 8 |
| Click → visual latency | stop **0.120 ms**, pause **0.022 ms**, against a 16.7 ms frame |
| Click → engine-state latency | **Not measured.** It is a round trip to a process, and no test times it |
| Finalize-progress cadence | Phases and monotonicity measured on a real engine; **the 10 Hz rate is not**, because a 1 s recording finalizes in ~200 ms |
| GUI CPU vs §16.1's 3% | **Not measured.** Needs a sampler over a real recording; M10's. The derived figure — 10 Hz × 0.124 ms ≈ **0.12% of one core** — says the overlay is not plausibly why the budget would be missed, and is arithmetic, not a measurement of the process |

---

---

## 5. Defects found, and the pattern in them

Seven, all caught before release. `ENGINEERING_LOG.md` has the full entries; what matters
here is that **five of the seven were invisible to state assertions**, that one was latent
in code nine milestones old, and that the last was in the tests themselves.

| | What it was | How it was found |
|---|---|---|
| BUG-049 | The exclusion test's DDA case passed under ctest and failed in one process — DDA re-emits an unpopulated copy target on timeout, and the test asserted on those frames | Running the suite in a single process |
| BUG-050 | `setObjectName` **replaces**, so a variant name silently dropped every rule attached to the name it overwrote; the stop button lost its background, border and radius | Rendering the widget and looking |
| BUG-051 | The theme's universal `QWidget { background-color }` painted a hard-edged dark rectangle inside the rounded pill | **A photograph from the user** |
| BUG-052 | An animation from a superseded layout finished last and carried a toast into its neighbour — the reported "improper stacking" | Rendering the column |
| BUG-053 | The hotkey capture field ignored every key but fifteen: the fallback asked a freshly constructed `QKeyEvent` for `text()`, which is a constructor *parameter* and was never passed | **A user report**, then a probe |
| BUG-054 | The window owned none of its menus; PySide gives Python ownership of a `QMenu` returned by `QAction.menu()`, so the idiomatic `[a.menu() for a in bar.actions() if a.menu()]` **destroys every menu in the bar**. Latent since the menu bar was written — the symptom is a menu that opens empty | The first test ever to enumerate the menu bar |
| BUG-055 | Four menu tests read the **real Windows clipboard** — a machine-wide resource behind a lock that any process can hold, and whose failed write does not raise. They failed together in one full-suite run and passed in the next | A full-suite run that disagreed with the one before it |

Three lessons that generalise. Phase 4 applied all three, and each earned its place again:

1. **Render the widget and look at it.** State assertions cannot see styling, layout or
   input handling. This is a step, not a contingency. Phase 4 rendered all eight menus and
   the topology dialog to PNG, which is how it found that no log level was ticked until the
   submenu had been opened once — the first opening showed six unticked levels.
2. **A GUI test fixture must apply `load_stylesheet()`.** Without it the theme's
   interactions are absent, and BUG-051's regression test passed with the fix reverted
   until the fixture was corrected. `test_menu_actions`'s fixture applies it.
3. **Verify a regression test by reverting the fix.** Done for all three of Phase 4's, and
   it paid a second time: with BUG-054's fix reverted, `_all_actions` returned nothing and
   `test_every_action_is_enabled_and_none_is_a_placeholder` passed **vacuously**. Both
   sweeps now assert a floor on how many actions they walked. A loop over an empty list is a
   test that asserts nothing and reports success.

A fourth, from BUG-054, worth adding to the list:

4. **Own what you create, and never discard a Qt object a getter handed you.** The window
   created five menus and kept none of them; that was survivable only by accident. The
   idiomatic Python filter — call the accessor in the condition and again in the expression
   — is the exact shape that destroys the thing it is asking about.
5. **A test must not assert on a resource it does not own** (BUG-055). CLAUDE.md §5
   already says tests use the synthetic source and never the real desktop; the system
   clipboard is the same rule somewhere the rule had not been written down. An
   intermittent green is a result to investigate, not to re-run — a suite with a known
   flake in it teaches everyone to re-run rather than read.

---

## 6. Verification status

**The verification debt recorded here through Phases 0-3 is cleared.** Every gate below
was run on the reference rig on 2026-09-06, on an unlocked workstation with nothing else
running -- which is what the three earlier attempts lacked.

| Gate | Status | Measured |
|---|---|---|
| `lint.ps1 -Strict` (clang-format, clang-tidy, banned patterns, overlay rules, ruff, mypy) | **clean** | 128 C++ files, 46 Python files |
| CPU tier (`ctest -LE gpu`) | **434/434** | 82.0 s |
| GPU tier (`ctest -L gpu`) | **199/199** | 780.2 s |
| GPU tier, **single process** (`fc_gpu_tests.exe`) | **198 passed, 1 skipped, 0 failed** | 619.3 s |
| pytest, hardware-free (`-m "not engine"`) | **312/312** | 47.4 s |
| pytest, engine-backed (`-m engine`) | **17/17** | 94.9 s |

Two notes on the numbers, because both were wrong in this document before:

- **The engine-backed pytest is 17 cases, not 4.** §6 said four; the `engine` marker
  selects seventeen. Nothing had run them to find out.
- **The single skip is deliberate.** `FinalizeThroughputTest.MeasureAGivenFile` is a
  measurement tool that needs a file named on the command line, and it skips itself when
  given none. It is not a test that failed to run.

**Both GPU forms were run because they are not the same test.** BUG-049 was a flaky green
that passed per-process under ctest and failed when the suite ran in one process, so the
in-process form is the stricter of the two and is what proves the exclusion cases are
stable across a whole run.

**What was run against which tree, stated exactly.** The ctest form ran on the Phase 0-3
tree, before Phase 4's C++ changes; the in-process form ran on the finished Phase 4 tree.
Phase 4's C++ delta is six boolean config keys and one JSON field on the `hello` response
-- no media-path code -- and all of it is covered by the CPU tier, which was re-run after
the change. The stricter GPU form is the one that ran last.

**If you re-run these**, keep the workstation unlocked and run nothing else concurrently.
The three attempts this milestone lost were lost to the environment, not the code: one to
a concurrent pytest run, one to a concurrent `lint.ps1` whose clang-tidy runs 16-way and
starved the encoder (`submitted 60, encoded 0, queue-dropped 52`), and two to a **locked
session**, which composites the secure desktop so WGC sees a static screen
(`decoded_frames=1, duration_s=0.017`). Expect ~12 minutes per GPU form, **audible tones
through the speakers** -- `fc_audio_target` is a real render client and §8.6's
per-application capture cannot be exercised silently -- and a full-screen test pattern
appearing periodically.

---

## 7. Decisions taken, including three that diverge from the plan

1. **Only `stop_record` was made asynchronous**, not every user command. It is the only
   one with a measured multi-second cost. `pause`/`resume` set a flag on the pause clock;
   their worst case is a press landing during a `start_record`, which §5.4 already budgets
   at 350 ms. Making them async would have cost the ability to report a refusal
   synchronously, which `test_engine_control` asserts on.
2. **A refused hotkey rebind leaves its action unbound** rather than keeping the old key
   alive, as §5.1 proposed. Once sequences can be shared, "keep the old binding for the
   action that failed" means keeping a registration whose routing belongs to the *other*
   action — the status would claim an accelerator that no longer runs it. New
   registrations are still taken before old ones are released, so a rebind never passes
   through a window where neither key works.
3. **A shared sequence dispatches by predicate.** Each binding carries a test of whether
   it can act now; the first that says yes runs, and stops. Firing every callback on a
   shared key would start and stop a recording in one press.
4. **Modifier order is Windows' own** — `Win`, `Ctrl`, `Alt`, `Shift` — and sequences are
   normalised, so `Shift+Ctrl+F9` and `Ctrl+Shift+F9` are one binding. Without that the
   duplicate check misses them and the manager registers a shared key twice, conflicting
   with itself.
5. **Hotkeys are three separate bindings** (§0.3 Q2, answered by the owner), with `start`
   and `stop` sharing a default so the familiar toggle survives untouched.

**Phase 4's, all recorded in full in §4.4:**

6. **The `.fcrecover` scan is the GUI's**, and the repair is entirely the engine's. §10.4
   says so in as many words, and `fc::mux::find_recoverable` is called by nothing but
   tests.
7. **Recoveries run one at a time**, because the async backlog is bounded at 8 with a
   drop-newest policy and a directory's worth issued at once would lose the ninth.
8. **`set_log_level` does not persist**, and the menu says so in the status bar.
9. **Copy output path changes its label** rather than greying out, so it always does
   something real.
10. **Restart engine moved to Tools ▸ Engine**, per the plan; §16.1 requires the action,
    not a location.

---

## 8. Still open

**Nothing is waiting on the owner.** `M9_6_PLAN.md` §0.3's three questions are all
answered:

- **Q1 — answered 2026-09-06:** rows 19–22 **join SPEC.md §20**, which becomes
  twenty-two rows, and §25's Definition of Done becomes "all 22". One contract, one
  renumbering, not a separate table.
- **Q2 — answered during Phase 3:** three separate hotkey bindings, `start` and `stop`
  sharing a default so the familiar toggle survives.
- **Q3 — answered 2026-09-06:** the recording pill is **on by default**, gated on the
  exclusion probe, so it can never appear in a recording. `overlay.pill_enabled` stays
  `true`, and View ▸ Show recording pill turns it off per user, persistently.

**Two spec amendments now owed, both Phase 6's** (they are edits to SPEC.md, which this
milestone does not touch unilaterally):

1. **§20 grows to twenty-two rows** and §25's "all 18" becomes "all 22" — Q1's answer.
2. **§15.1's timeout sentence** must name `recover` alongside `stop_record`. See §3.4:
   the 5 s budget would time out on every recording large enough to be worth recovering,
   and this build already gives it 30 s.

**Deferred deliberately:** `ACCEPTANCE.md` sections for the new §20 rows are Phase 6's,
along with the §16.2/§16.4/§16.5 amendments describing the menus, the overlay and the
hotkeys section.

**One decision the owner now cannot avoid, because Phase 5 made it a gate:** the licence.
`ci.yml` fails on its first run for want of a `LICENSE` file, which is what plan §7.4
asked for. See §9.

**A repository hazard, pre-existing and not introduced here:** the git index stores LF, the
worktree was checked out CRLF, and effective `core.autocrlf` is `false`. Untouched files
look clean only via git's stat cache, so **any file that gets edited shows a whole-file
diff** unless it is normalised to LF. Every file in this change set has been. Worth
deciding repo-wide at some point; it is not a M9.6 problem.

---

## 9. Phase 5's exit criterion, and why it is not met here

The milestone's exit criterion (plan §9) says of this phase:

> `ci.yml` is green on a PR from a clean clone, and `gpu.yml` has run green on the rig at
> least once.

**Neither half can be reached from where this work was done, and both halves need the
owner.** Stating it rather than letting a green local suite imply otherwise:

| Half | Blocked on |
|---|---|
| `ci.yml` green on a PR | A commit, a push and a pull request. Nothing in M9.6 is committed — 36 files modified and 29 added, all in the working tree, on `main` |
| `gpu.yml` green on the rig | A **registered self-hosted runner**. Registration needs a short-lived token from the repository's Settings → Actions → Runners, which is an owner action |

### What was verified instead

Every command the workflows run was run on this machine, in the order the workflow runs
them, and the numbers are §6's. That covers the part most likely to be wrong — a step that
does not work — and leaves the part that needs GitHub.

What is *proven* about the workflow files themselves:

- they parse, every job has a timeout, every `run` step declares `shell: pwsh`, and no
  step is empty (`test_workflows.py`);
- `ci.yml` cannot select the `gpu` label, cannot invoke `fc_gpu_tests`, and cannot run
  pytest without `not engine`;
- `gpu.yml` and `soak.yml` cannot be triggered by a pull request, and share a
  single-concurrency group that never cancels a run in flight.

Those guards were verified the way M9.6 verifies everything: by breaking the thing they
guard. With `ci.yml` edited to `ctest -L gpu`, both GPU-tier assertions fail; restored,
all fifteen pass.

What is **not** proven, and cannot be from here:

- that `actions/checkout@v4`, `actions/setup-python@v5`, `actions/cache@v4` and
  `actions/upload-artifact@v4` resolve and behave as expected on `windows-latest`;
- that the vcpkg binary cache key produces a hit on the second run — the whole basis of
  CI being fast enough to matter (plan §7.1);
- **the cold and warm build times**, which §7.1 explicitly asks to be recorded in
  `TESTING.md`. The place for them is written and marked unmeasured. An unmeasured cache
  is a cache nobody notices has stopped working.

### To close it out

1. Decide the licence (see below), or accept that CI's `gates` job is red on its first run.
2. Commit and push M9.6, open a PR, and let `ci.yml` run. Record the cold time.
3. Merge, let the push build run, and record the warm time in `TESTING.md` §5.3.
4. Register the runner on the rig with the `framecapture-rig` label — **`run.cmd` in an
   interactive session, not as a Windows service.** `TESTING.md` §4.3 explains why: a
   service runs in session 0, which has no desktop, so every capture test would run
   against nothing and fail in a way that looks like a code defect.
5. Trigger `gpu.yml` by hand once, and check it against §6's numbers.

### The licence, which is now a gate rather than a note

`ci.yml`'s `gates` job **will fail on its first run**, on exactly one thing: there is no
`LICENSE` file. That is the gate doing its job (plan §7.4), not a defect in it.

CLAUDE.md §9 already records the position: libx264 is in, `--enable-gpl` is set, the
distributed binary is GPLv2, and that obliges source availability for the distributed work
and is incompatible with shipping under a proprietary licence. The obligation exists today,
with or without the gate. What the gate adds is that it cannot be reached by accident.

No `LICENSE` was written here, deliberately. Choosing one is the owner's decision and
CLAUDE.md §9 says not to guess at those. If the answer is GPLv2, adding the standard text
turns the gate green; if it is anything else, the conversation that needs having is the one
§9 flagged and §21's packaging work depends on.

---

## 10. Three defects in Phase 5's own tooling

None reached `ENGINEERING_LOG.md` — they are gates and tests rather than shipped
behaviour — but the pattern is worth recording, because all three were found the same way
and none would have been found by the tooling being green.

1. **`-ChangedFiles a,b` bound as the single string `"a,b"`.** PowerShell's `-File`
   invocation does not split a comma-separated argument into an array, and GitHub hands
   file lists over newline-separated. The gate would have seen one path where there were
   forty and reported clean. Found by the first test written against it.
2. **An empty array failed `Mandatory` binding**, so the *common* case — a change adding
   no keys and no error codes — crashed the gate rather than passing it. Found by the same
   test run.
3. **`Select-Matches` returned `$null` on no matches**, because PowerShell unrolls a
   returned empty array, and `$null.Count` throws under `Set-StrictMode -Latest`. The
   consequence is the sharp one: the "detector matches nothing" branch — the entire point
   of `-SelfTest` — could never report, it could only crash. Found by *reverting the
   pattern to check the self-test could fail*, which is M9.6's lesson 3 applied to a piece
   of test infrastructure and repaid immediately.

The lesson that generalises: **verification tooling needs the same treatment as the code
it verifies.** All three of these lived in something whose whole purpose is to catch other
people's mistakes, and all three would have made it silently permissive.
