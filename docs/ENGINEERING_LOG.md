# Engineering Log

Mandatory bug journal (SPEC.md §22.1, CLAUDE.md §10). Every non-trivial bug gets an
entry — symptom, investigation, root cause, fix, regression test, lesson. This is a
graded deliverable, not a nicety.

**Conventions**

- IDs are permanent and never reused, allocated in order of discovery. Gaps are fine.
- An entry is written when the bug is *understood*, not when it is closed. An entry
  whose **Fixed** field is not a date is an open item and its named regression test
  does not exist yet.
- Build-environment failures (toolchain, proxy, SDK) belong in `BUILD.md`, not here.
  This log is for defects in FrameCapture and in how it drives its dependencies.

---

## [BUG-057] A failed segment rollover makes the engine disown a file it had already finalized

**Severity:** Critical (prime-directive violation — a valid recording is reported to the user as lost)   **Found:** 2026-09-08   **Fixed:** _OPEN — two contributing defects fixed, firing cause not yet identified_   **Commit:** ffb17b7+

### Symptom

The chaos tier's first run at SPEC.md §20.1's specified 30-minute duration failed on its
only hard assertion — the prime directive itself:

```
[ MEASURED ] 322 fault(s) injected over 1800 s
[ MEASURED ] captured 5689, encoded 6364, queue-dropped 5278, rebuilds 20, migrations recorded 20
test_chaos_injection.cpp(327): error: Value of: stopped.has_value()
  Actual: false
Expected: true
the recording failed rather than degrading: INTERNAL_INVALID_STATE
```

`RecordingSession::stop()` returned an error instead of a `ValidationReport`. To every
caller — including the GUI, which renders it as "Save failed" on the recording pill — the
recording was lost.

Seed `2991276637`, reproducible with `FC_CHAOS_SEED=2991276637 FC_CHAOS_SECONDS=1800`.

### Investigation

`INTERNAL_INVALID_STATE` has five sites in `recording_session.cpp`. `stop()`'s entry guard
(`running.exchange(false)`) was ruled out first: `running` is set true in exactly one place
and false in exactly one place, so a second `stop()` was the only way to reach it, and the
test calls it once.

That left `stop()`'s *second* guard, at line 859:

```cpp
if (!impl_->pipeline) {
    if (impl_->settings.preview_only) { /* ...a report, not an error... */ }
    return FcError::INTERNAL_INVALID_STATE;
}
```

So the session reached `stop()` with **no pipeline**. One path leaves it that way — the
migration-triggered segment rollover, when the rebuilt encoder's parameter sets differ:

```cpp
if (const Result<mux::ValidationReport> report = pipeline->stop(); !report.has_value()) { /* log */ }
pipeline.reset();                                   // (2) pipeline is now null

++segment_index;
if (const Result<void> opened = open_pipeline(next); !opened.has_value()) {
    FC_LOG_ERROR(..., "opening the next segment failed; the recording stops", ...);
    stop_requested.store(true, std::memory_order_release);
    return;                                         // (3) returns with pipeline still null
}
```

Under 20 rebuilds against a disk stalling 300 ms every eighth write, `open_pipeline` failed
once. From that moment the session had no pipeline, and `stop()` could only report an
internal error.

### What is proven

Established by reading the code and confirmed by a second full-length run. These are
facts, not the hypothesis below:

1. **`pipeline == nullptr` at `stop()` has exactly one source.** `pipeline.reset()` appears
   once in `recording_session.cpp` — in the migration-triggered segment rollover. So the
   session reached `stop()` having rolled over and failed to open the next segment.
2. **`VideoPipeline` never clears its own `running` flag.** Only its `stop()` does, so the
   pipeline had not stopped itself before the rollover asked it to.
3. **The routine 30 s form passes and the 1800 s form fails**, reproducibly, at seed
   `2991276637`. The defect needs ~20 rebuilds; 30 seconds reaches three.

### What was fixed, and why it was not enough

Two real defects were found and corrected on the way. Neither is the one that fires.

**`open_pipeline` installed a pipeline before starting it:**

```cpp
pipeline = std::make_unique<VideoPipeline>();   // installed as the live pipeline
FC_TRY(pipeline->start(...));                   // ...then started
```

A failed start left a half-constructed pipeline installed as the live one — non-null, so
every `if (pipeline)` in the file reads it as working, and unstarted, so
`VideoPipeline::stop()` refuses it with `INTERNAL_INVALID_STATE`. Fixed: built into a local
and installed only on success.

**The rollover's `ValidationReport` was discarded.** The rollover finalizes and validates
the current file, and that report was examined for failure and otherwise dropped. `Impl`
now retains it in `last_report`, and `stop()` returns it when the pipeline is gone but a
file was written.

**The re-run at the same seed failed identically.** Same error, same 20 rebuilds, same
counters. So neither defect above is the live cause.

### The remaining hypothesis — NOT yet evidenced

By elimination: `last_report` is only set when the rollover's `pipeline->stop()` *succeeds*.
If that finalize returns an error — plausible with the device lost and the disk stalling
300 ms every eighth write — the report is never retained, and `stop()` falls through to the
same `INTERNAL_INVALID_STATE`.

**This is a hypothesis and it is recorded as one.** Two earlier accounts of this bug read as
confident and were wrong; the difference between the two above and this one is that this one
has not been checked against evidence.

### Why it was not checked

The engine's own log was deleted before it could be read. The chaos fixture logged into its
`TempDir`, and `TearDownTestSuite` removes that — so the run that failed took its own
explanation with it, twice.

Fixed for next time, and this is the durable lesson of the entry so far:

- the chaos log now goes to `%TEMP%\framecapture-chaos-logs` and **survives the test**;
- every `MigrationRecord` is printed — cause, `same_file`, `failed`, gap, output — so the
  aggregate "20 rebuilds" becomes "which one rolled over, and what happened to it".

A failure that costs thirty minutes to reproduce must leave its evidence behind. That should
have been true before the first run, not after the second.

### Fix

**Open.** Two contributing defects fixed (above); the firing cause is not yet identified.
The next step is one instrumented run at seed `2991276637`, which will produce the finalize
error and the open error rather than another hypothesis.

The contract question from the original entry stands and is the owner's: **what should
`stop()` report after a rollover?** It reports on the current pipeline only. With segments
the honest answer is the last segment's report plus the existence of earlier ones — and the
migration path writes no `.segments.json` sidecar, unlike planned segmentation (§11).

### An unexplained observation, recorded rather than pursued

After the first failure the test process **printed its verdict and never exited**. It sat
for half an hour; `Get-Process` listed it, `Stop-Process` reported no such process, and it
held `fc_gpu_tests.exe` locked so the next link failed. Exited but unreapable — a thread
stuck in a kernel call, which after 20 GPU device rebuilds under load points at the driver
rather than at this code.

Not investigated. It matters operationally: a chaos run that hangs on exit will wedge a CI
runner, and `gpu.yml` has a 90-minute timeout that would not catch it quickly.

### Regression test

`ChaosTest.RandomisedFaultInjectionAlwaysYieldsAValidFile` at
`FC_CHAOS_SECONDS=1800`, seed `2991276637`. It is the test that found it, it fails on the
defect today, and it is the test that must go green.

**The routine 30-second form passes**, which is exactly why this needed the spec's stated
duration to surface: 20 rebuilds is where it lives, and a 30-second run reaches three.

### Lessons

1. **The chaos tier justified itself on its first full-length run.** Every fault here has a
   deterministic single-fault test and every one of those is green. What none of them
   reaches is the twentieth rebuild, and that is where the pipeline stops being able to
   report.
2. **Run the duration the spec states, not the duration that is convenient.** The routine
   form is for iterating. It passed while a critical defect sat behind it.
3. **A successful result that is discarded is a failure waiting to be reported.** The
   `ValidationReport` from the finalized segment existed, was correct, and was thrown away
   two lines before the code path that needed it.

---

## [BUG-056] The chaos tier's seed silently truncated, so half of all failures were unreproducible

**Severity:** Major (in a test whose entire value rests on the property it broke)   **Found:** 2026-09-08   **Fixed:** 2026-09-08   **Commit:** _uncommitted_

### Symptom

`ChaosTest` prints its seed on every run so a failure can be replayed:

```
[ MEASURED ] chaos seed 3130200536 (re-run with FC_CHAOS_SEED=3130200536), 30 s, ...
```

Following that instruction did not replay it:

```
$env:FC_CHAOS_SEED="3130200536"
[ MEASURED ] chaos seed 2147483647 (re-run with FC_CHAOS_SEED=2147483647), 30 s, ...
```

A different seed, a different fault schedule, and nothing anywhere saying the request had
not been honoured.

### Investigation

`2147483647` is `LONG_MAX`, which named the cause immediately. The seed reader was:

```cpp
if (const long parsed = std::atol(raw); parsed > 0) {
    return static_cast<unsigned>(parsed);
}
```

`long` is **32 bits** on Windows — `LLP64`, unlike the LP64 model where this code would
have been fine. `std::random_device` produces the full 32-bit *unsigned* range, so any
seed above 2147483647 exceeds `LONG_MAX`, and `atol` saturates rather than failing.

That is **half of the seed space**. A failing chaos run had a coin-flip chance of printing
a seed that could not reproduce it.

### Root cause

Two defects, one line:

1. **`long` cannot hold the value being parsed.** `std::random_device` is
   `unsigned int` — 32 bits, max 4294967295. `long` on Windows tops out at 2147483647.
2. **`atol` cannot report a failure.** It returns 0 on garbage and saturates on overflow,
   and both are indistinguishable from a valid parse. clang-tidy's
   `bugprone-unchecked-string-to-number-conversion` says exactly this, and it did flag the
   sibling `atoi` in the same file — but only once the compile database was regenerated
   after the new source file was added, which is CLAUDE.md §3's stale-database note
   earning its keep for the second time (BUG-005 was the first).

### Fix

`std::strtoull`, with the end pointer checked and the value range-checked against
`0xFFFFFFFF`:

```cpp
char* end = nullptr;
const unsigned long long parsed = std::strtoull(raw, &end, 10);
const bool complete = end != nullptr && *end == '\0' && end != raw;
if (complete && parsed > 0 && parsed <= 0xFFFFFFFFull) {
    return static_cast<unsigned>(parsed);
}
std::cout << "[ MEASURED ] FC_CHAOS_SEED=" << raw
          << " is not a value in [1, 4294967295]; using a random seed instead\n";
```

A rejected seed now **says so** rather than quietly substituting one. The sibling
`seconds_from_env` was moved from `atoi` to a checked `strtol` in the same change, for the
same reason: a typo in `FC_CHAOS_SECONDS` silently became the 30-second default, so the
30-minute run somebody thought they had asked for never happened.

### Regression test

Verified by measurement rather than by a new test case: two consecutive runs with
`FC_CHAOS_SEED=3130200536` now produce an identical fault schedule —
`DEVICE_REMOVED + ACCESS_LOST` at t+7.01, `DEVICE_REMOVED` at t+12.02, `ACCESS_LOST` at
t+14.53, and two more correlated pairs at t+24.54 and t+27.05 — and that schedule matches
the one the original random run at that seed produced. Before the fix the same command ran
seed 2147483647.

There is no automated case, and that is a stated gap rather than an oversight: asserting it
means running the tier twice, which is 80 seconds of GPU time to check a property that a
one-line reader either has or does not. The check belongs in the review of any change to
`seed_from_env`, and the comment there says so.

### Lessons

1. **A reproducibility feature has to be reproduced, not read.** The code looked right, the
   output looked right, and the instruction it printed was wrong. Nothing short of
   following the instruction would have found it — the same shape as M9.6's lesson about
   rendering the widget and looking at it.
2. **`long` is 32 bits on Windows.** `atol`, `strtol` and `%ld` are all narrower here than
   the habits formed on Linux expect, and a value from `std::random_device`,
   `GetTickCount64` or a file size will exceed it. This is the second time in this project
   a width assumption produced nonsense from a clock-adjacent value (BUG-019 was a QPC
   overflow).
3. **A chaos test's non-vacuity guards are worth as much as its assertions.** The same
   session found the tier reporting green while injecting nothing detectable — 120 ms of
   stall every 24 writes produced **zero** queue drops, so §20.1's "queue saturation" was
   named in the file header and absent from the run. Both were caught by asking what the
   numbers actually said rather than what the test reported.

---

## [BUG-055] Four menu tests failed together, then passed, because they read the real Windows clipboard

**Severity:** Minor in effect, major in kind (an intermittently red suite is a suite nobody trusts)   **Found:** 2026-09-06   **Fixed:** 2026-09-06   **Commit:** _uncommitted_

### Symptom

A full `pytest gui/tests -m "not engine"` run that had passed 281/281 twice reported:

```
4 failed, 277 passed, 17 deselected in 32.03s
FAILED test_menu_actions.py::test_copy_output_path_names_the_folder_when_nothing_has_been_recorded
FAILED test_menu_actions.py::test_copy_output_path_switches_to_the_recording_once_there_is_one
FAILED test_menu_actions.py::test_the_diagnostics_summary_reaches_the_clipboard_with_the_facts_in_it
FAILED test_menu_actions.py::test_the_diagnostics_summary_still_renders_with_the_engine_down
```

Running `test_menu_actions.py` on its own immediately afterwards: 41 passed. Running the
full suite again: 281 passed.

### Investigation

The four are exactly — and only — the tests that did this:

```python
window._copy_output_path()
assert QApplication.clipboard().text() == str(window._output_directory)
```

Nothing else in the suite touches the clipboard, and no other test failed. Nothing was
running concurrently: the pytest run and the `lint.ps1` run in that shell invocation were
sequential.

### Root cause

The Windows clipboard is a **machine-wide resource behind a lock**. `OpenClipboard` fails
while another process holds it, and Qt's `QClipboard::setText` does not raise when that
happens — the write is simply lost and the subsequent read returns whatever was there
before. Any process on the machine can take that lock at any moment; a browser, the shell,
a password manager, the desktop app this session runs in.

So the four assertions were reading a global mutable resource that no part of the test
owns. They pass when nothing else wants the clipboard for a few microseconds, and fail
when something does. **The failure has nothing to do with the code under test**, which is
what makes it worse than no test: the natural response to an intermittent red is to re-run
until it is green, and that habit is how a real intermittent defect gets ignored later.

### Fix

A seam, and one place rather than two:

```python
def _set_clipboard(self, text: str) -> None:
    """The one place this application writes the clipboard."""
    QGuiApplication.clipboard().setText(text)
```

Both copy actions go through it. A `copied` fixture replaces it with a list's `append`, so
the tests assert on **the text the window produced** and never touch the system clipboard.

That is the right boundary independently of the flake. What this project can get wrong is
*which text* it copies — the wrong path, a summary missing the adapter that owns the
display output, a malformed line. That `QClipboard.setText` then works is Qt's contract.

Verified by three consecutive full-suite runs: 281/281 each time.

### Lessons

1. **A test must not assert on a resource it does not own.** The system clipboard, the
   real desktop, the user's config file, a fixed TCP port — each makes a test's result
   depend on the machine rather than the change. CLAUDE.md §5 already says tests use the
   synthetic source and never the real desktop; the clipboard is the same rule in a place
   the rule had not been stated.
2. **Put the shared resource behind one method.** The seam that makes the test
   deterministic is the same seam that would make the failure handleable if it ever needs
   handling. Two call sites would have needed two patches and would have given the
   application two places to get the failure wrong.
3. **An intermittent green is a result to investigate, not to accept.** This one cost ten
   minutes because the four failures shared an obvious property. The reason to spend those
   ten minutes now is that the next intermittent failure will not be obvious, and a suite
   with a known flake in it teaches everyone to re-run rather than read.

---

## [BUG-054] Asking a menu for its contents deleted the menu

**Severity:** Major (a menu bar whose menus open empty, which is the exact defect Phase 4 existed to remove)   **Found:** 2026-09-06   **Fixed:** 2026-09-06   **Commit:** _uncommitted_

### Symptom

The first run of `test_menu_actions` — the named test for M9.6 Phase 4, which walks
`menuBar()` and asserts that no menu opens empty — failed on five cases with the same
error, and not an assertion:

```
RuntimeError: libshiboken: Internal C++ object (PySide6.QtWidgets.QMenu) already deleted.
```

The menus had been rendered to PNG moments earlier and were plainly there, fully
populated. Constructing the window in a plain script and printing each menu's action
count also worked:

```
'&File' 1   '&Edit' 4   '&View' 8   '&Tools' 9   '&Help' 1
```

So the menus existed, and asking a test about them did not.

### Investigation

The difference between the script that worked and the test that did not was narrowed by
elimination, one property at a time. Not the stylesheet, not `qtbot`, not the fixture's
replacement of the `EngineController`, and not a `gc.collect()`. What was left was the
shape of the helper:

```python
def _menus(win):
    return [action.menu() for action in win.menuBar().actions() if action.menu() is not None]
```

`action.menu()` appears **twice** per action — once in the condition, once in the
expression. The script called it once and kept the result. Measured directly:

```
double-call validity: [False, False, False, False, False]
single-call validity: [False, False, False, False, True]
held validity:        [False, False, False, False, False, False, False, False]
```

The third line is the important one. `MainWindow` had by then been changed to keep every
menu in `self._menus`, so a live Python reference existed to all eight — and they died
anyway. All eight: the three submenus went with their parents, so a single discarded
wrapper for the View menu took `Panels` with it.

### Root cause

PySide hands back a `QMenu` from `QMenuBar.addMenu(str)` and from `QAction.menu()` with
**Python ownership**, despite Qt having parented the menu to the bar. When a wrapper for
one of those is discarded, shiboken deletes the underlying C++ object. Holding another
reference does not save it.

So the menu bar's menus were alive only for as long as nothing asked about them. Nothing
in the shipped GUI asks — which is why this had been latent since the menu bar was first
written, and why it surfaced the moment a test enumerated it.

**The failure mode is what makes this worth an entry.** There is no crash in the
application: the title stays on the bar and the menu opens *empty*. That is
indistinguishable from the defect this whole phase existed to remove, and it would have
been reported as "the Tools menu is empty again" long after the change that caused it.

### Fix

Two parts, and only the second actually prevents deletion.

1. **`MainWindow._add_menu`** creates every menu — top-level and sub — and appends it to
   `self._menus`. This does not stop a discarded wrapper deleting the menu, but it gives
   the window a single, named owner and, crucially, a registry that can be traversed
   without going through `QAction.menu()` at all.
2. **Nothing traverses menus via `QAction.menu()`.** The test's `_menus` reads
   `MainWindow._menus` and cross-checks the titles against `menuBar()`, so a menu created
   with a bare `addMenu` — the way the bug gets reintroduced — appears on the bar, not in
   the registry, and fails loudly with its title named. `_all_actions` skips submenu
   openers by identity against `QMenu.menuAction()`, which is owned by the menu and safe.

### Regression test

`gui/tests/test_menu_actions.py::test_every_menu_is_registered_with_the_window`, plus
`test_no_menu_opens_empty` and `test_every_action_is_enabled_and_none_is_a_placeholder`,
which fail through the same registry check.

Verified by reverting: with the `self._menus.append(menu)` line removed,
`test_every_menu_is_registered_with_the_window`, `test_no_menu_opens_empty`,
`test_every_action_triggers_without_raising` and
`test_settings_carries_the_shortcut_the_plan_names` all fail.

That revert also exposed a second hole and closed it. With the registry empty,
`_all_actions` returned nothing, so `test_every_action_is_enabled_and_none_is_a_placeholder`
passed **vacuously** — a loop over an empty list asserts nothing. Both sweeps now assert a
floor on how many actions they walked, which is the difference between a test that checks
every action and a test that checks that there are no actions.

### Lessons

1. **A Qt object handed back by a getter is not automatically safe to discard.** The
   idiomatic Python filter — call the accessor in the condition and again in the
   expression — is the exact shape that destroys it. Where an accessor returns a
   long-lived Qt object, call it once and keep what it returned.
2. **Own what you create.** The window created five menus and kept none of them. That was
   survivable only by accident, and "survivable by accident" is what turns into a defect
   the first time anything else touches it.
3. **A failure that is a `RuntimeError` from the bindings is still a product bug.** The
   temptation was to call it a test artefact and write the traversal differently. The
   measurement — that the *held* references died too — is what showed it was ownership
   and not the test.
4. **Reverting the fix found the second bug.** Lesson 3 of M9.6's Phase 0–3 lessons says
   verify a regression test by reverting its fix; here that check did not just confirm the
   test, it revealed that a neighbouring test had been passing on an empty collection.

---

## [BUG-053] The hotkey capture field ignored every key except the fifteen in its lookup table

**Severity:** Major (the settings section could not be used to change a shortcut)   **Found:** 2026-09-05   **Fixed:** 2026-09-05   **Commit:** _uncommitted_

### Symptom

Reported from real use: "the hotkey is not registering when trying to enter/change to a
new hotkey combination". Clicking a field and pressing a combination did nothing -- the
field stayed as it was, and no shortcut was recorded.

### Investigation

Reproduced immediately by sending synthetic key events at a `HotkeyEdit` and printing
what came out:

```
expected               via keyPressEvent
Ctrl+Alt+R             ''                 <-- BROKEN
Ctrl+Shift+F9          'Ctrl+Shift+F9'
Ctrl+Alt+Home          'Ctrl+Alt+Home'
Ctrl+Alt+5             ''                 <-- BROKEN
Ctrl+Alt+Num5          ''                 <-- BROKEN
Ctrl+Alt+-             ''                 <-- BROKEN
Ctrl+Alt+[             ''                 <-- BROKEN
Ctrl+Shift+Delete      ''                 <-- BROKEN
```

The keys that worked were exactly the contents of `_KEY_NAMES_BY_QT` -- the function keys
and a handful of named navigation keys. **Everything else -- every letter, every digit,
the whole numpad, all punctuation -- captured nothing**, which for a user reaching for
`Ctrl+Alt+R` is the entire feature not working.

### Root cause

The fallback for a key not in the lookup table:

```python
name = _KEY_NAMES_BY_QT.get(key)
if name is None:
    text = QKeyEvent(QKeyEvent.Type.KeyPress, key, Qt.KeyboardModifier.NoModifier).text()
    name = text.strip().lower()
if not name:
    return ""
```

`QKeyEvent`'s constructor takes the event's text as a **parameter**; it does not derive it
from the key code. None was passed, so `text()` returned `""` every time. The fallback
could never have produced a name for anything -- it was dead code that looked like
working code, and it silently swallowed every key the table did not already list.

Reading `event.text()` from the *real* event would not have worked either: with Ctrl held,
a letter's text is the control character (`Ctrl+R` is `\x12`), not `r`.

### Fix

Derive the name from the key **code**, which is the only thing that identifies the key
struck regardless of what is held with it:

- `Key_A`-`Key_Z` and `Key_0`-`Key_9` from `chr(int(key))`;
- numpad digits from the same codes plus `KeypadModifier`, mapped to `num0`-`num9` --
  they are different virtual keys to Windows, so a user who binds the numpad must get
  the numpad;
- numpad operators and punctuation from explicit tables keyed by Qt code.

`Delete` also became bindable in the same change. It had been grouped with `Escape` and
`Backspace` as a "clear the field" gesture, which made `Ctrl+Shift+Delete` unreachable;
`Escape` and `Backspace` are enough for clearing.

### Regression test

`test_every_kind_of_key_can_be_captured`, parametrised over a letter, a function key, a
navigation key, a digit, punctuation, a bracket, `Delete`, an arrow and `Space`; plus
`test_the_numpad_captures_as_the_numpad` and `test_a_captured_combination_is_announced`.
Verified by reverting the fix: six of them fail, and pass with it.

### Lessons

- **The tests covered the section's `load` and `collect` and never pressed a key at it.**
  Both of those set the field's text directly, so the entire capture path -- the reason
  the widget exists -- had no coverage at all. A round-trip test through the data layer
  can look like thorough coverage of a widget whose job is input.
- **A fallback that cannot work is worse than no fallback.** Had the code simply returned
  `""` for an unlisted key, the fifteen-key limit would have been obvious on the first
  read. Wrapping it in a plausible-looking `QKeyEvent(...).text()` made it look handled.
- Constructing a Qt event to interrogate it is a smell: `QKeyEvent` is a *carrier* of
  what happened, not a decoder of key codes, and asking a synthetic one what a key means
  is asking a question it was never given the answer to.
- This is the fourth defect this milestone found by exercising the thing rather than by
  reading it, and the second where the missing coverage was the *interaction* layer
  (BUG-052 was the animation applying the layout; this is the widget receiving input).

---

## [BUG-052] A toast animating toward a superseded slot finished there, overlapping its neighbour

**Severity:** Major (the reported overlapping-notifications defect; purely visual)   **Found:** 2026-09-05   **Fixed:** 2026-09-05   **Commit:** _uncommitted_

### Symptom

With five notifications posted back to back, the column settled wrong:

```
Recording quality reduced (x2)   y= 156   <- should be 24
The recording engine stopped     y=  88   height 124  -> occupies 88..211
Saved FrameCapture_....mkv       y= 220
Recording paused                 y= 306
```

The first toast sat *inside* the second. Three of the four were exactly right, which is
what made it look like a geometry bug in the one that was wrong.

### Investigation

`stack_positions` is pure and had 200 randomised sequences of differing heights behind it
across all four corners, all green. Recomputing the wanted positions from the live widget
sizes at the moment of the failure agreed with the pure function -- the layout it had
been *asked for* was correct. The toast simply was not where the layout said.

The difference between a run that reproduced it and one that did not turned out to be
whether the event loop ran between posts. Every widget test pumped `processEvents()`
between messages, and none of them reproduced it. Posting five messages back to back --
which is what a real error burst does, and what the screenshot script happened to do --
reproduced it every time.

Tracing positions through the burst showed why:

```
-- immediately after five back-to-back posts: settling=True
   Recording quality reduced  pos=24   want=24    <- skipped: already "in place"
   Engine stopped             pos=24   want=88
-- after settling
   Recording quality reduced  pos=138  want=24    <- carried off by a stale animation
```

### Root cause

`_relayout` decided whether to move a toast with:

```python
if toast.pos() != position:
    self._animate_to(toast, position)
```

That compares a toast's **current** position against its new target. During a burst every
toast is momentarily still at the anchored corner, because none of the 120 ms transitions
has advanced. A toast that was already sliding toward a *now-stale* slot therefore
compared equal to its new target, was skipped -- and the animation from the previous
layout kept running and delivered it to the old slot.

The guard was asking "is it there yet?" when the question is "is it *going* there?".

### Fix

A relayout supersedes everything in flight, unconditionally:

```python
for animation in self._animations.values():
    animation.stop()
self._animations.clear()
```

before the per-toast comparison. A relayout is by definition the new truth about where
things go; an animation started against the previous layout is aiming at a slot that no
longer exists. Stopping mid-slide is correct rather than jarring -- the toast simply
animates on from wherever it had reached.

Two related hazards were removed in the same pass: animations are now keyed per toast
rather than held in a list (two `QPropertyAnimation`s driving one `pos` fight, and the
winner is whichever finishes last), and a toast's animation is stopped when the toast is
removed, which had been a write to a deleted widget on the next frame.

### Regression test

`test_a_burst_with_no_event_processing_still_settles_correctly` -- five posts with no
event loop in between, ending on a coalesced repeat so the relayout happens while four
animations are running. It asserts the **settled positions**, not merely the absence of
overlap: a column that settled one slot down as a whole would pass an overlap check.

Verified by reverting the fix: it fails with
`QPoint(1172, 92) == QPoint(1172, 24)`, and passes with it.

### Lessons

- **A guard against "already correct" has to account for what is in motion.** Any
  animated layout has two positions per item -- where it is and where it is going -- and
  comparing against the wrong one is invisible until two updates arrive inside one
  transition.
- **Pumping the event loop between steps hid the bug in every test that had one.** The
  realistic case is a burst with no pump: an error storm, or a stop that saves and then
  reports. A widget test that processes events between every action is testing an
  interaction pattern users do not have.
- The pure geometry was correct and thoroughly tested throughout. Property tests over
  `stack_positions` could never have found this, because the defect was in the layer that
  *applies* the geometry. Splitting pure logic out is worth doing and is not sufficient
  on its own.
- Found, again, by rendering the thing and looking at it -- the third visual defect this
  milestone (BUG-050, BUG-051, this one) that no state assertion could see.

---

## [BUG-051] The theme's universal `QWidget` background painted a hard-edged dark rectangle inside the rounded pill

**Severity:** Minor (purely visual; no effect on a recording)   **Found:** 2026-09-05   **Fixed:** 2026-09-05   **Commit:** _uncommitted_

### Symptom

Reported from real use, with a photograph of the screen. The recording pill showed a
**square dark block** across its middle — behind the elapsed time and the file size —
inside an otherwise rounded, translucent body. It looked like a rendering fault.

### Investigation

`theme_dark.qss` opens with a universal rule:

```css
QWidget {
    background-color: @bg-base;   /* #16181C */
    ...
}
```

The pill paints its own rounded body in `paintEvent` with `bg-surface` (`#1E2126`) at
alpha 238. Its children are ordinary widgets, so the universal rule fills them with
`bg-base` — which is *darker* than the body and has square corners.

Sampling a row of the grabbed widget shows it exactly:

```
x=  6  #1e2126 a=238     <- pill body
x= 48  #16181c a=255     <- the block starts
x=270  #16181c a=255
x=348  #1e2126 a=238     <- pill body again
```

The appended pill styles had made only `#RecordingPill QLabel` transparent. The
`QStackedWidget` holding the two pages, and the pages themselves, were left to the
universal rule.

### Root cause

A universal `QWidget` background rule reaches every child of every widget in the
application, including containers a custom-painted widget expects to be invisible. A
widget that paints its own background has to opt its containers *out*; there is nothing
in Qt that makes a child transparent by default once such a rule exists.

### Fix

```css
#RecordingPill QLabel,
#RecordingPill QStackedWidget,
#RecordingPill QStackedWidget > QWidget,
#PillDot {
    background: transparent;
}
```

`_PillDot` gained an object name so it could be named here; it was showing the same
`bg-base` fill, invisibly, because 14×14 px of `#16181C` on `#1E2126` is not something
the eye picks out.

### Regression test

`test_the_pill_body_has_no_opaque_rectangle_in_it` grabs the widget and compares two
pixels: one inside the stack's area, one on the pill's own painted body. **Not a
screenshot comparison** — it depends on no font, no metric and no layout, only on the
property that the background is uniform.

**The first version of this test was worthless and passed with the fix reverted.**
pytest's `qapp` fixture never applies the application stylesheet, so the universal rule
that *causes* the defect was not present in the test environment. The fixture now applies
`load_stylesheet()`, and the test was then verified to fail without the fix and pass with
it — in that order.

Fixing the fixture also surfaced `QtWarningMsg: Unknown property font-variant-numeric`:
the pill's anti-jitter measure was a CSS property Qt does not implement, silently ignored.
Replaced with a minimum width measured from the label's own font metrics, which is what
actually keeps the layout still as the seconds digit changes.

### Lessons

- **A universal `QWidget` rule in a themed application is a background applied to things
  you did not think of as backgrounds.** Any widget that paints itself has to name its
  containers and opt them out.
- **A GUI test fixture that does not apply the application's stylesheet is not testing
  the application.** Every styling interaction — which is most of what a theme *is* —
  is absent from it, and a regression test written against that fixture can be green
  while the defect is on screen.
- Verify a regression test by reverting the fix. This one was written, passed, and proved
  nothing; two minutes of checking turned it into a real test. It is the same lesson as
  BUG-049 in a different medium.
- Both of this milestone's visual defects (BUG-050, BUG-051) were invisible to state
  assertions and found by rendering the widget and looking at it. That step belongs in
  the routine for any new widget, not just when something is suspected.

---

## [BUG-050] `setObjectName` on the pill's stop button silently removed every style it shared with the others

**Severity:** Minor (a control that looked broken; no effect on a recording)   **Found:** 2026-09-05   **Fixed:** 2026-09-05   **Commit:** _uncommitted_

### Symptom

The recording pill's stop button rendered as a flat dark square: no rounded background,
no border, and not the `--rec-active` red it was supposed to be. The pause button beside
it was correct.

**Every test passed.** Twenty-odd assertions on the pill covered its states, its clock,
its progress bar, its close behaviour, and its capture exclusion, and not one of them
could see this.

### Investigation

Found by rendering the widget in each of its seven states and looking at the images,
which is the only reason it was found at all before release.

`_PillButton.__init__` sets `setObjectName("PillButton")`, and the stylesheet gives
`#PillButton` its background, border, radius and hover. The stop button then had
`setObjectName("PillStop")` applied to add the red.

`setObjectName` **replaces** the name; it does not add one. So the stop button stopped
matching `#PillButton` entirely and matched only `#PillStop`, which declared a colour and
nothing else.

### Root cause

Qt object names are singular. Treating one as a class list -- which is what "add
`PillStop` for the variant" assumes -- silently drops every rule attached to the name it
overwrote. Nothing warns: the selector that no longer matches simply stops applying.

The same pattern was in the phase label (`PillPhase` -> `PillPhaseFailed`) and happened to
look right only because that rule re-declared all three properties it needed.

### Fix

A dynamic property for the variant, which is Qt's mechanism for exactly this:

```python
self._stop.setProperty("variant", "stop")     # not setObjectName("PillStop")
```

```css
#PillButton[variant="stop"] { color: @rec-active; }
```

The object name stays `PillButton`, so the shared rules keep applying and the variant adds
to them. Same change for the phase label's `state` property. `_repolish` was already
being called and is still required -- Qt does not re-evaluate a stylesheet when a property
changes any more than when a name does.

### Regression test

`test_the_buttons_keep_their_shared_styling` and
`test_the_failed_phase_keeps_its_label_styling` assert the *mechanism* -- that
`objectName()` is still the shared one and the variant lives in a property. They
deliberately do not assert appearance: a pixel comparison would pin the theme to one
font-rendering stack, which `test_main_window` already declines to do for the same reason.

### Lessons

- **A widget's styling is not observable from its state**, and this project's GUI tests
  are all state tests by design. The gap is real and the only thing that closes it is
  rendering the thing and looking at it. Worth doing once per new widget rather than
  never.
- `setObjectName` is not `classList.add`. When a widget needs a variant of a shared style,
  the variant belongs in a property.
- The two occurrences differed only in whether the replacement rule happened to re-declare
  everything it had displaced. One looked fine and was equally wrong -- which is why the
  fix went to both rather than only to the one that showed.

---

## [BUG-049] The overlay-exclusion test's DDA case passed under ctest and failed when the suite was run in one process

**Severity:** Major (a green test that proved nothing, gating M9.6's whole overlay design)   **Found:** 2026-09-05   **Fixed:** 2026-09-05   **Commit:** _uncommitted_

### Symptom

`test_overlay_exclusion` is M9.6 Phase 0's gate: it asks whether
`SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` keeps a window out of a real
capture on both backends, because a "no" for DDA would change the design of two features.

Run through ctest, all six cases passed. Run as one process to collect the measured
numbers, the DDA exclusion case **failed**:

```
[ measured ] DDA mean_luma=0.0 overlay_fraction=0.0000
[  FAILED  ] OverlayExclusionTest.ExcludeFromCaptureLeavesNoOverlayAndNoCutoutDda
```

Read at face value that says DDA blackens the region — the black-cutout defect the whole
milestone exists to avoid.

### Investigation

The DDA **control** case is what gave it away. That case places an unstamped overlay and
asserts it *is* captured, and it reported:

```
[ measured ] DDA mean_luma=0.0 overlay_fraction=1.0000
```

Those two numbers cannot both come from one frame. RGB(255,128,0) has a BT.709 luma of
145.8, so `overlay_fraction = 1.0` and `mean_luma = 0.0` are mutually exclusive
descriptions of the same pixels. They came from *different* frames: the test aggregated
a worst case across four frames, taking the lowest luma and the highest overlay fraction,
so a single all-black frame set the luma while a good frame set the fraction.

Where an all-black frame comes from is in `DdaCapture::Impl::emit`. SPEC.md §4.3 requires
`DXGI_ERROR_WAIT_TIMEOUT` to yield "a duplicate frame with a correctly advanced PTS", and
`emit` implements that by re-emitting `latest` — the copy target. Before the first real
`AcquireNextFrame` has copied anything into it, that texture is a freshly created,
zero-filled one. Those frames are black everywhere, including under the overlay's rect.

WGC never showed this because it composites on change and does not have a
duplicate-on-timeout path to emit an unpopulated buffer from.

The flakiness followed directly: `gtest_discover_tests` runs each case in its own
process, and whether an unpopulated duplicate landed inside the first four frames depended
on timing that differed between a fresh process and the fifth test in a shared one.

### Root cause

**The test asserted on frames it had not established came from the fixture.**

`ScreenAnimator` exists precisely because of BUG-033 — capture tests that reported on
whatever happened to be on screen — and `pattern_signature` is documented in its header
as "the check that the frames under assertion really came from the fixture". The new test
used the animator but never applied that check, so DDA's startup behaviour was measured as
if it were a property of display affinity.

### Fix

A `shows_pattern` gate before any frame is inspected. It samples two bar centres in the
top third, outside the overlay's rect — bar 0 must be black and bar 6 must be yellow — so
a zero-filled duplicate is rejected rather than measured. Frames are drawn until four have
passed the gate, with a bounded attempt count so a backend that never delivers a usable
frame ends the loop instead of spinning. The count of rejected frames is printed alongside
the measurements, and the DDA control now reports `rejected=1` — the unpopulated frame,
correctly discarded.

Two smaller fixture defects were fixed in the same pass:

- the overlay window's colour was **magenta**, which is one of the eight bars
  `ScreenAnimator` paints. An overlay coloured like the pattern it sits on cannot be
  distinguished from it. Changed to orange, which is in neither the bar palette nor the
  two greys of the moving block.
- `OverlayWindow::destroy` deleted the brush that its registered window class still
  referenced. A window class outlives its windows and cannot be re-registered, so the
  second `create()` in a test kept the first registration and painted with a
  `DeleteObject`'d handle. The window came up unpainted and the recreation case measured
  an overlay fraction of 0 — reporting "the affinity survived recreation", the exact
  opposite of what had happened. The class now has no background brush and the window is
  filled explicitly in `repaint`.

### Regression test

`OverlayExclusionTest.*` itself, with the gate in place. The measured result, stable over
four consecutive runs of the full suite in one process:

| case | backend | mean_luma | overlay_fraction |
|---|---|---|---|
| unstamped (control) | WGC | 145.8 | 1.0000 |
| unstamped (control) | DDA | 145.8 | 1.0000 |
| `WDA_MONITOR` | WGC | 0.0 | 0.0000 |
| `WDA_EXCLUDEFROMCAPTURE` | WGC | 255.0 | 0.0000 |
| `WDA_EXCLUDEFROMCAPTURE` | DDA | 255.0 | 0.0000 |

145.8 is exactly the BT.709 luma of RGB(255,128,0), and 255.0 is the animator's white
bar coming through intact. **DDA honours `WDA_EXCLUDEFROMCAPTURE` on this rig**, so M9.6
§1.3's fallback ladder is not needed.

### Lessons

- **A worst-case aggregation across frames needs every frame to be valid.** Taking the
  minimum of one statistic and the maximum of another across a set of frames produces a
  pair of numbers that may describe no frame at all — which is what made the
  contradiction visible, and would have made a subtler version invisible.
- **A control case earns its place by failing informatively.** The exclusion case's
  failure was ambiguous between "DDA does not honour the affinity" and "the measurement
  is wrong". The control's impossible number settled it in one line.
- **`ScreenAnimator` is only half the fixture; `pattern_signature` is the other half.**
  BUG-033's lesson was "own what is on screen". The completion of it is "and check that
  what you captured is what you own" — a new capture test that skips the check
  reintroduces the original bug wearing a new backend.
- Running a GPU suite **in one process** is a different test from running it through
  ctest, and it found this. Worth doing before believing a per-process green.

---

## [BUG-048] A 7.1 pin on a stereo endpoint up-mixed to eight channels, and Windows' own player would not play the audio

**Severity:** Major (a recording whose audio is unplayable in the default Windows player, with nothing to say why)   **Found:** 2026-08-06   **Fixed:** 2026-08-06   **Commit:** _uncommitted_

### Symptom

Reported from real use, with a screenshot. A recording made with `channel_layout = 7.1`,
the system default device, MKV, and Tier B off opened in Windows' player as:

> We can't play the audio for FrameCapture_2026-08-06_11-55-39 because its encoding
> settings aren't supported. **You can still watch the video.**

Video decoded. Audio refused. The file opened.

### Investigation

1. **The file was eliminated first, because three separate things said it was fine.**
   `ChannelLayoutTest` covers 7.1 in MKV and is green — container mask,
   `AudioSpecificConfig`, decoder-derived layout, and per-channel tone identity, all
   asserted against a decoded file. `Muxer::validate` runs on every recording and had
   passed this one, which means libavformat opened it and both tracks decoded. And the
   video in the same MKV played. So whatever was wrong was decoder-side and specific to
   the audio codec.
2. **The Media Foundation AAC decoder takes 1, 2 and 6 channels.** Not 8. An
   eight-channel AAC-LC stream has no supported input type there, so Windows' player
   refuses the stream and says exactly what it said.
3. **And the eight channels held two channels' worth of information.** This rig has one
   render endpoint — "Speakers (Realtek(R) Audio)", 48 kHz, **2 ch**, measured and
   recorded in docs/ACCEPTANCE.md row 12. The recording had up-mixed 2 → 8.

### Root cause

`resolve_channel_layout` used the endpoint only in its `Auto` branch. An explicit setting
was applied unconditionally:

```cpp
case config::ChannelLayoutSetting::Surround71:
    av_channel_layout_from_mask(&layout, AV_CH_LAYOUT_7POINT1);
```

which is SPEC.md §8.5's "pinned for the lifetime of the file", implemented literally and
in both directions. `resampler.h` justified the override with the *downward* case — "how
a user records stereo from a 7.1 endpoint without touching Windows' settings" — and that
argument is correct and does not generalise. Upward it buys nothing: an up-mix cannot add
information, and this one cost 512 kbps instead of 192 (§8.5's ladder keys on the pinned
channel count) and the audio track's playability in the default player.

**The GUI offered the choice unconditionally too**, so nothing anywhere connected "7.1"
to "your device has two channels".

### Fix

The override is **downward only**. A request wider than the endpoint falls back to the
endpoint's own layout and is logged at WARN with both counts. Equal is not wider and is
honoured; `Auto` is unaffected; the downmix case is untouched.

Decided at `open` and never re-evaluated, because §14.1 forbids changing an AAC stream's
channel count mid-file — so an endpoint that later migrates to a wider one does not widen
the recording.

The GUI says so before the recording rather than leaving it to a player afterwards: an
inline note under the layout selector when the choice exceeds the default endpoint, in
the shape §20 row 16 already asks for on the MP4 block. It **explains**; the engine
**enforces**, which is the same split row 16 draws and for the same reason — a headless
caller must not be able to walk past it.

### Measured

| | before | after |
|---|---|---|
| pinned layout, 7.1 on a 2 ch endpoint | 8 channels | **2 channels** |
| AAC bitrate for that recording | 512 kbps | **192 kbps** |
| audio in Windows' player | refused | plays |
| 7.1 on a genuine 8 ch endpoint | 8 channels | **8 channels**, unchanged |
| explicit stereo on an 8 ch endpoint | 2 channels | **2 channels**, unchanged |

### Regression test

`AudioCaptureTest.ALayoutWiderThanTheEndpointFallsBackToTheEndpointsOwn` (gpu) drives the
reported configuration exactly — `Surround71` against a 2 ch endpoint — and asserts the
result is stereo with the endpoint's own mask. It carries three controls in the same case,
because the interesting failure is a rule that is *too* aggressive: 5.1 on stereo clamps
(the same mistake one rung down, and the likelier one), 7.1 on a 7.1 endpoint does **not**
(equal is not wider), and the default bitrate follows the layout actually pinned rather
than the one asked for. Without those three, "never honour an explicit setting" would pass.

`AnExplicitLayoutOverridesTheEndpointDownwards` — the renamed original — keeps the
downmix under test, which is the half of §8.5's pin that still works exactly as before.

Four settings-dialog cases in `gui/tests/test_main_window.py` cover the notice, including
the two that stop it becoming noise: a layout the device *can* supply says nothing, and an
engine that reports no endpoints stays silent rather than guessing.

### Lessons

**A green test proved the file was correct and the file was unusable.** Every assertion in
`ChannelLayoutTest` was true: the container mask, the `AudioSpecificConfig`, the decoded
layout and the per-channel tones were all exactly right. They were verified with FFmpeg's
decoder against FFmpeg's encoder, and the question that mattered — will anything else play
this — is one a closed loop cannot ask. SPEC.md §8.5 had already written down the adjacent
version of this ("revisit if a real player is found to mis-map it") and the note was one
step short: it anticipated mis-mapping and not refusal.

**"Pinned" was implemented as a symmetric rule because the word is symmetric.** The
justification in the header was asymmetric — it described the downmix — and the code did
not distinguish them. When a comment argues for one direction of a two-directional
behaviour, the other direction has not been considered.

**The setting was reachable and its consequence was not visible anywhere.** Not in the
GUI, not in the log at the point of choice, not in the file's own metadata. The user's
only feedback was a player error naming neither the cause nor the setting. A configuration
that can produce an unusable output should be refused, clamped, or annotated at the moment
it is chosen — and this now clamps *and* annotates, because the clamp alone would have
silently ignored what they asked for.

---

## [BUG-047] Process loopback reported itself unavailable on a machine where it works, because the completion handler was not agile

**Severity:** Major (SPEC.md §8.6's Tier B unavailable everywhere, and silently)   **Found:** 2026-08-05   **Fixed:** 2026-08-05   **Commit:** _uncommitted_

### Symptom

The first run of `MultitrackTest.ATargetThatExitsMidRecordingLeavesATrackSilencePaddedToFullDuration`
on the reference rig:

```
process loopback is unavailable on this system; SPEC.md §8.6 requires Windows 10 19041+,
which §1 already makes the floor
```

Windows 11 26200. SPEC.md §1 puts the floor at 19041 and §8.6 says so explicitly, so
either the spec was wrong about the floor or the probe was wrong about the machine.

**The three cases that do not touch the real client passed** -- rows 14 and 16 and the
Tier A control, all of which use `AppTrackSource::External`. So every deterministic
assertion about placement, naming, duration and the MP4 block was green while the
feature could not start on any machine at all.

### Investigation

1. **The probe returned a boolean, and a boolean cannot be investigated.**
   `process_loopback_available()` answered "no" and logged the HRESULT, but the test's
   log directory is a `TempDir` that is deleted at teardown, so the one useful fact was
   written and then thrown away. The first change was to expose
   `process_loopback_probe_result()` and put the HRESULT in the failure message. That
   is the whole reason step 2 took one run rather than several.
2. **`0x8000000E` -- `E_ILLEGAL_METHOD_CALL`**, returned *synchronously* by
   `ActivateAudioInterfaceAsync` itself rather than through the completion handler. So
   the call was refused before any audio-service work happened, which rules out policy,
   the target process, and the activation parameters.
3. That leaves the arguments. The device string, the IID and the `PROPVARIANT` blob were
   all as `audioclientactivationparams.h` describes them. The remaining argument is the
   handler -- and `ActivateAudioInterfaceAsync` queries it for `IAgileObject` before it
   does anything else.

### Root cause

`ActivationHandler` implemented `IActivateAudioInterfaceCompletionHandler` and returned
`E_NOINTERFACE` for everything else, including `IAgileObject`. The API requires the
handler to be agile, because it calls back from a thread whose apartment it does not
promise anything about, and it enforces that requirement up front.

The header comment written alongside it said, in as many words, "Not `IAgileObject`: the
activation is issued from an MTA thread and the audio service calls back on one, so there
is no apartment to marshal across." That reasoning is sound and irrelevant: the API does
not check whether marshalling would be *needed*, it checks whether the object claims to
be agile.

### Fix

`ActivationHandler` inherits `IAgileObject` and answers its IID from `QueryInterface`.
It is a marker interface with no methods, so that is the whole of implementing it.

### Measured, before and after

| | before | after |
|---|---|---|
| `ActivateAudioInterfaceAsync` | `0x8000000E` | `S_OK` |
| buffers from a real target over 12 s | -- | **400** |
| buffers with no QPC position | -- | **0** |
| track length against the system mix | -- | 12.010 s against 12.010 s |
| 1500 Hz energy, before the target exited / after | -- | **0.0433 / 0.000000** |

The QPC figure is worth keeping: the fallback path in `drain_available` exists because
the virtual device is not documented to populate `u64QPCPosition`, and on this rig it
populates it on every one of 400 buffers. The fallback has therefore **never executed
here**, which is a limitation of the evidence and not a claim about other machines.

### Regression test

`MultitrackTest.ATargetThatExitsMidRecordingLeavesATrackSilencePaddedToFullDuration`
(gpu), which is §20 row 15 and is what found this. It **fails** rather than skipping when
the probe says no, per CLAUDE.md §6 -- a GPU-tier case that passes on a machine without
the hardware proves nothing -- and it now prints the HRESULT, so the next occurrence is a
diagnosis rather than a boolean.

`test_the_engine_reports_whether_this_machine_can_do_per_application_audio` (pytest)
asserts the same thing through `get_config`, from a separate process, so a regression
that only affected the shipped engine would still be caught.

### Lessons

**A capability probe that returns a boolean throws away the only useful thing it
learned.** "Unavailable" is not actionable; `E_ILLEGAL_METHOD_CALL`, `AUDCLNT_E_*` and
`ERROR_TIMEOUT` mean three completely different things with three different answers. The
probe now caches and exposes the HRESULT, and the log line carries it, because the cost
of that is four lines and the cost of not having it was a debugging session that started
from "the spec must be wrong about the Windows floor".

**A correct-looking comment can encode a wrong premise, and it is more convincing than
no comment.** The `IAgileObject` note reasoned about whether marshalling was *necessary*.
The API asks a different question, and a comment explaining why a requirement does not
apply is exactly as expensive as one explaining why it does -- BUG-014's "documented as
deliberate" lesson arriving from a different direction.

**Every test that could pass without the feature working did.** Rows 14 and 16 and the
Tier A control are all real assertions about real behaviour, and all three are green on a
build where Tier B cannot start. That is not a flaw in them -- they are deliberately on
the deterministic seam -- but it is the reason row 15 uses the real client against a real
process, and the reason it asserts *tone energy* rather than only track length. Without
that one case, this defect ships.

---

## [BUG-046] The MP4 remux wrote the recording twice, and the measurement that found it was wrong three times before it was right

**Severity:** Minor (a correct file, saved more slowly than it needs to be)   **Found:** 2026-08-05   **Fixed:** 2026-08-05   **Commit:** _uncommitted_

### Symptom

Reported from real use: a 10-15 minute recording of about **1.2 GB** takes a significant
time to save.

### Investigation

**Nothing had ever measured this.** SPEC.md §10.3 states a budget in a parenthesis --
"stream copy only, no re-encode, ~2 s for a 1 h file" -- and every MP4 case in the tree
records for a handful of seconds, where a cost proportional to file size is invisible.

1. **The finalize path was instrumented first**, because §10.4 already requires every stage
   to be "separately logged" and a single end-to-end number cannot say which stage spent it.
   On a 15 MB recording:

   ```
   size_mb=14.897  remux_ms=105.850  validate_ms=648.870  swap_ms=4.551
   ```

   Two different costs, and which dominates depends on size: `validate` is a **fixed**
   ~650 ms (it decodes the 5 s tail, per §10.4), while the remux scales.

2. **`faststart` was found by reading libavformat rather than by guessing.** Its own option
   text is explicit -- "Run a second pass to put the index (moov atom) at the beginning of
   the file" -- and `ff_format_shift_data` re-opens the output *for reading* and copies the
   entire mdat forward by the moov's size. Combined with the read and write the remux
   already performs, that is **four passes over the recording where two are required**.

3. **movenc offers the alternative.** `moov_size` (its `reserved_moov_size`) skips that many
   bytes at the head, writes the mdat after it, and at trailer time seeks back, writes the
   moov into the reservation and pads the remainder with a `free` atom. Same layout, one
   pass.

### Root cause

`kProgressiveMp4Flags = "faststart"`, chosen because §10.3 names it, without noticing that
it achieves the layout by writing the file twice.

### Fix

Reserve the `moov` instead, sized from the source's own sample count. `estimate_moov_size`
counts `nb_frames` where the demuxer reports it and derives it from duration and frame rate
where it does not, then pads at 40 bytes a sample against the 16-20 the tables actually need.
`faststart` remains as a fallback for a source that will not say how long it is -- correct,
and two passes slower.

### Measured, and the three wrong measurements first

**This is the part worth keeping.** The fix is right; three attempts to measure it were not.

| attempt | what it reported | why it was wrong |
|---|---|---|
| encode a large file with the synthetic source | 100 MB took **115,200 frames and 701 s** | the flat SMPTE pattern codes to <1 KB/frame, so the file had 24x more packets per MB than a real recording -- it measured per-packet cost where a user pays per-byte |
| loop a short recording's packets | no faster | same tiny packets, ~2 M of them for 400 MB |
| compare two *different* real recordings | "145 MB/s before, 319 MB/s after" -- a 2.2x win | **different files at different sizes.** The difference was size and page cache, not the fix. Publishing this would have been a false claim |

The measurement that discriminates is the **same file, both ways**. A 290 MB fragmented
recording, remuxed twice per build:

| | run 1 | run 2 |
|---|---|---|
| moov reserved | 0.76 s / **382 MB/s** | 0.72 s / **400 MB/s** |
| faststart | 0.78 s / 369 MB/s | 0.84 s / 344 MB/s |

**About 10%, not 2x.** The second pass is nearly free here because a 290 MB file written
seconds earlier is entirely in the OS page cache: its extra read is memory, and its extra
write is overwritten in cache before `FlushFileBuffers` forces anything. The saving is real
I/O only when the file is *cold*, which a 1.2 GB recording written over fifteen minutes
largely is -- and that case could not be reproduced on this rig, because no file that fits
in RAM can demonstrate it.

Solving the two-term cost from a 15 MB and a 290 MB measurement gives the model worth
carrying: **1.97 ms per MB (509 MB/s) plus 8.8 µs per packet.** For the reported recording --
1.2 GB, ~96,000 packets -- that is 2.4 s of bytes plus 0.8 s of packets = **3.3 s of remux**,
plus validate's fixed ~1 s.

### Regression test

`FinalizeThroughputTest.TheRemuxWritesTheFileOnceRatherThanTwice` (gpu). It asserts the
*structural* property rather than a rate, because a rate on a file small enough to build in
a test is a measurement of the page cache: `RemuxStats::second_pass` must be false, a
reservation must have been made and must scale sanely, and the `moov` must be in the first
kilobyte -- the property `faststart` existed to provide. Verified red-before by forcing the
estimator to 0, where FFmpeg itself logs "Starting second pass".

`MeasureAGivenFile` is the instrument, skipped unless `FC_REMUX_INPUT` names a file. It is
how the numbers above were taken and how a user's own recording can be measured.

### Lessons

**A benchmark whose fixture is the bottleneck measures the fixture.** Two of the three bad
attempts failed that way, and `synthetic_source.h` already warns about exactly it -- the
same warning BUG-027 and BUG-032 record. A test that takes twelve minutes to set up is also
one nobody runs.

**Comparing two different files is not an A/B.** The 2.2x figure was arithmetic on real
measurements and was still wrong, because size and cache moved between them. The only honest
form was the same file through both code paths, and it turned a claimed 120% win into a
measured 10% one.

**The page cache hides exactly the cost this fix removes**, so the measurement that is
easiest to take is the one that shows the least. Worth remembering for any I/O change: if
the test file fits in RAM, the test is not measuring the disk.

---

## [BUG-045] An idle desktop was rebuilt as though it were a wedged capture, because on a push-only backend silence is not a signal

**Severity:** Major (a healthy recording tears down its capture and device stack, repeatedly, for as long as nobody touches the machine)   **Found:** 2026-08-04   **Fixed:** 2026-08-04   **Commit:** _uncommitted_

### Symptom

From a user's own 112-second recording:

```
worst_stall_us=11113420  cause=capture_stalled  gap_ms=243
```

11.1 seconds of silence on a static screen, read as a fault. The session tore down the
capture backend, the D3D device, the colour converter and the encoder, rebuilt all of it,
and lost 243 ms of content. Nothing was wrong. On an unattended machine it repeats
indefinitely.

### Investigation

1. **The threshold had already been raised once and was still wrong.** The trigger started
   at row 9's stated 3× frame interval (50 ms), and a previous session raised it to 2 s
   with the comment "longer than any rebuild (~240 ms measured) and longer than any
   plausible idle-desktop gap". The first clause is still true. The second was out by 5×.
2. **The reproduction failed twice, and both failures were informative.** Recording the
   real desktop for 40 s produced **1944 frames** — a blinking caret and a clock are enough
   to keep WGC delivering. Covering the output with `ScreenAnimator` at `fps = 0` (paint
   once and hold) still produced **301 frames in 15 s**. A machine quiet enough to show the
   defect is not a machine that is also running a test.
3. **The two backends are not the same shape, and only one of them can go quiet.**

   | | delivery | what "no frame" means |
   |---|---|---|
   | DDA | polls `AcquireNextFrame`, emits a duplicate on every `WAIT_TIMEOUT` (§4.3) | never happens — DDA is never silent |
   | WGC | `FrameArrived` callback; the worker thread only sleeps to own the WinRT objects | **nothing at all** |

   So on the primary backend the trigger was reading a signal that does not exist. WGC says
   nothing when the desktop is idle, and a dead WGC session says exactly the same nothing.
4. **There is no fifth threshold.** An unattended screen can be still for hours. Any number
   is a guess about how bored the user is, and the cost of guessing low is a needless
   teardown of the exact D3D/WinRT stack BUG-037 established is fragile between recordings.
5. **What the backends *do* say.** Both set `running` false as their worker leaves its loop,
   for any reason. That is a fault with a name, it cannot be produced by an idle desktop,
   and nothing was reading it — `RecordingSession` never called `capture->running()`.

### Root cause

Row 9's mitigation ("no frame in 3 × frame_interval → rebuild") is written as a timeout,
which presumes a **pull** backend where being asked and having nothing to give is itself
information. WGC is push-only. The trigger therefore inferred a fault from the absence of a
signal that is absent by design.

### Fix

The rebuild now keys on the backend's own verdict rather than on how long it has been
quiet. `RecordingSession` publishes `capture->running()` from the capture thread — which
owns the backend, so the watchdog may not read it directly — and rebuilds when it goes
false, guarded by `capture_observed` so the window before the first iteration is not a
fault, and cleared across a rebuild so `capture->stop()`'s own `running = false` cannot
provoke the next one (BUG-035's shape, through a different door).

Row 9's **detector** is untouched: `stall_episodes` and `worst_stall_ns` are still computed
and still surfaced, because "your screen has not changed in eleven seconds" is worth saying.
It is just not worth acting on.

**A second change was required to make the first one safe.** With silence no longer a
trigger, an unplugged monitor would have left WGC reporting itself alive and quiet forever
— trading a false positive for a false negative on a case §14.2 requires. `WgcCapture` now
subscribes to `GraphicsCaptureItem::Closed`, which is the only notice WGC gives that the
thing being captured has gone away, and drops `running` when it fires.

### Measured

| | before | after |
|---|---|---|
| backend quiet for 8 s, healthy | **2 rebuilds** | **0 rebuilds** |
| worst stall reported in that run | 2596 ms | 7977 ms (detector still reports) |
| backend quiet because it died | 1 rebuild, 78.7 ms | **4 rebuilds, 75.3–85.6 ms** |
| injected device loss (control) | 1 rebuild, ~90 ms | 1 rebuild, 89.7 ms |
| healthy 6 s recording (control) | 0 rebuilds | 0 rebuilds |

Recovery from a genuinely dead backend got **faster**: it now fires on the next 500 ms
watchdog poll instead of waiting out a 2 s threshold, against row 9's 500 ms budget.

### Regression test

`WatchdogRecoveryTest.ABackendThatIsQuietButHealthyIsLeftAlone` and
`WatchdogRecoveryTest.ABackendThatHasDiedIsRebuilt` (gpu) — a pair over one `QuietingCapture`
backend that differs in a single bit: what `running()` reports once it stops producing.
Verified red-before on the first (2 rebuilds) and green-after; the second passed both before
and after, which is the point of having it.

`WatchdogRecoveryTest.AStaticScreenIsNotMistakenForAWedgedCapture` (gpu) is the live
end-to-end case against real WGC behind a static owned screen. **It does not discriminate on
this rig** — WGC delivers 301 frames in 15 s regardless — and its comment says so. It is kept
as a canary for quieter machines, where it would have caught the original defect.

### Lessons

**A threshold on something you do not control is a guess, however carefully it is chosen.**
This one was tuned twice, from 50 ms to 2 s, with a measurement and a justification each
time, and both were wrong because the quantity being thresholded — how often the world
changes the screen — has no upper bound. The fix was not a better number but a different
signal: `running()` is a threshold on something the engine *does* control, namely whether
its own worker is alive.

**Two backends behind one interface can differ in whether a value means anything.** DDA and
WGC both "return no frame", and the same code read both as the same event. It is worth
asking, of any interface with more than one implementation, whether the *absence* of a
result carries the same meaning in each.

**Removing a false positive can install a false negative, and the second is worse.** Killing
the trigger alone would have made the failing test pass and left an unplugged monitor
undetected forever. The `Closed` subscription is not a bonus; it is what made the removal
safe. `ABackendThatHasDiedIsRebuilt` exists to make that non-optional.

---

## [BUG-044] The preview's compute dispatch and the colour converter's interleaved on one D3D11 context, and the recording was written wrong while every counter said it was perfect

**Severity:** Critical (corrupt frames in the output file, undetectable from the engine's own telemetry)   **Found:** 2026-08-04   **Fixed:** 2026-08-04   **Commit:** _uncommitted_

### Symptom

Found by the test written for something else. `ARecordingIsUnaffectedByAPreviewThatCannotKeepUp`
records the same 300 synthetic frames three times — preview off, preview on, preview
deliberately wedged — and decodes each file to check that every frame's index barcode
follows its predecessor. Every recording-side counter was identical and clean across the
three:

```
             captured   300 / 300 / 300
             encoded    300 / 300 / 300
             qdropped   0 / 0 / 0
             paced_out  0 / 0 / 0
             duplicates 0 / 0 / 0
```

and the barcodes were not:

```
             absent     0 / 11 / 375        (run 1)
             absent     0 / 84 / 16744327   (run 2)
```

Nothing had been dropped, merged, or paced out. Preview off was perfect. Preview on was
not.

### Investigation

1. **The first hypothesis was wrong and worth recording.** `absent` counts barcode jumps,
   so the obvious reading was lost frames — two source frames landing in one CFR grid slot,
   §7.2 correctly keeping one. `PipelineStats::frames_paced_out` and `duplicates_emitted`
   were added to the report to confirm it. **Both were 0.** The pacer had merged nothing.
   No frame was missing.
2. That leaves misreads. A 24-bit barcode cannot decode to 16 744 327 unless the *pixels*
   are wrong: the run-2 figure is one frame whose top-left corner decoded to nearly the
   maximum value, i.e. garbage in the barcode cells.
3. So the corruption is in the picture, and it appears only with the preview running. What
   the preview added is a second compute dispatch:

   | | thread | sequence |
   |---|---|---|
   | `Nv12Converter::convert` | `venc` | CSSetShader → CSSetConstantBuffers → CSSetShaderResources → CSSetUnorderedAccessViews → Dispatch → unbind |
   | `PreviewScaler::dispatch` | `capture` | the same six calls, with its own shader, SRV and UAV |

4. **A D3D11 device created without `D3D11_CREATE_DEVICE_SINGLETHREADED` protects each
   *call* on the immediate context, not a sequence of them.** Compute-stage bindings are
   device-wide state, and slot 0 means slot 0 to both dispatches. Two threads running that
   sequence interleave, and one thread's `Dispatch` executes with the other's shader, SRV
   or UAV bound.
5. The wedged run corrupted *more* than the healthy one while dispatching roughly a quarter
   as often (39 dispatches against 150), which looks contradictory and is not: with
   `fc-preview` asleep 120 ms per frame, the capture thread's dispatches land in a far
   wider spread of positions relative to the venc thread's, so a rarer event with much
   less correlation comes out ahead.

### Root cause

Until M9 the engine had exactly one compute stage and the question could not arise.
SPEC.md §15.2's preview is the second, on a different thread, against the same device — and
nothing serialised the two sequences.

**M9 did not introduce the fragility, it introduced the second dispatch.** The converter's
bind-and-go sequence was always unsynchronised; it was simply alone.

### Fix

`gpu::ScopedDeviceLock` — RAII over `ID3D11Multithread::Enter`/`Leave`, D3D11's own
mechanism for exactly this — held across the bind-and-go sequence in **both**
`Nv12Converter::convert` and `PreviewScaler::dispatch`. The `ID3D11Multithread` is queried
once at `initialize` and cached, so the per-frame path pays an uncontended lock and not a
`QueryInterface`.

Both sites, because one is useless: the lock only serialises threads that take it.

The preview holds it for six calls and nothing else — no map, no readback, no allocation,
no wait. Everything with a cost proportional to the picture happens on `fc-preview`,
outside the section.

Rejected alternatives: a **deferred context** per preview frame (correct, but
`FinishCommandList` allocates, and the recording path it would sit on is the capture
thread — CLAUDE.md hard rule 4); **dispatching the preview from the venc thread** (removes
the interleave but not the sharing, and a preview-only session has no venc thread at all).

### Measured, before and after

Same three-configuration run, barcodes read out of the decoded files:

| | preview off | preview on | preview wedged |
|---|---|---|---|
| before, run 1 | 0 | 11 | 375 |
| before, run 2 | 0 | 84 | 16 744 327 |
| **after, run 1** | **0** | **0** | **0** |
| **after, run 2** | **0** | **0** | **0** |

Recording counters were identical before and after, in every configuration, which is the
part worth keeping in mind.

### Regression test

`PreviewTest.ARecordingIsUnaffectedByAPreviewThatCannotKeepUp` (gpu) — the barcode check,
now asserted rather than only reported, in all three configurations.
`PreviewTest.ThePreviewCanBeArmedAndDisarmedWhileTheRecordingRuns` (gpu) covers the same
thing across two mid-recording transitions.

### Lessons

**A subsystem that cannot affect the recording *by design* still shares a device with it.**
§15.2's "zero effect on the recording" is a statement about queues, drops and threads, and
every one of those was honoured: the preview never blocked, never back-pressured, never
took a frame away. It corrupted the output anyway, through a resource neither component's
design mentions.

**Every counter the pipeline keeps was clean.** `frames_encoded`, `frames_queue_dropped`,
`frames_paced_out`, `duplicates_emitted`, `convert_failures`, `encode_failures` — all
correct, all identical to the control. A defect that writes the wrong bytes into the right
number of frames is invisible to a pipeline that counts frames. The only thing that caught
it was reading the frame's own identity back out of the decoded picture, which is the
lesson `SoftwareEncoderTest` already records for the NV12 readback (CLAUDE.md §9) arriving
from a completely different direction. **Content checks are not a luxury on top of counter
checks; they are the only ones that can see this class of bug at all.**

**And the first explanation was plausible, cheap, and wrong.** "Barcode gaps on a
real-time-paced source" reads exactly like CFR grid jitter, and the fix for that would have
been to relax the assertion — which would have shipped the corruption with a comment
explaining why it was fine. Two counters, added specifically to test that story, refuted
it. Adding the measurement that could disprove the comfortable hypothesis is what turned
this from a weakened test into a bug.

---

## [BUG-043] Choosing MP4 produced an MP4 file named `.mkv`

**Severity:** Major (the file is correct and most players will not open it)   **Found:** 2026-08-04   **Fixed:** 2026-08-04   **Commit:** _uncommitted_

### Symptom

Reported from real use: "when I chose .mp4 as the file type, the video was recorded in
.mkv filetype instead."

### Investigation

1. `config.toml` held `container = 'mp4'`, so the setting had been saved correctly and
   the settings dialog was not at fault.
2. The engine's own log said the whole thing in one line:

   ```
   container opened path="D:\Screen recs\FrameCapture_2026-08-04_11-16-09.mkv" format=mp4
   ```

   **`format=mp4` with a `.mkv` filename.** The engine had honoured the setting and muxed
   MP4. The recording was not "in MKV" at all — it was an MP4 wearing the wrong extension,
   which is worse than the report suggested, because the bytes are right and the name lies.
3. `gui/framecapture_gui/main_window.py` built the name as
   `f"FrameCapture_{stamp}.mkv"` — a literal.
4. `start_record` carries `output` and nothing about the container, and the engine takes
   the container from its own configuration. So the two halves of the filename came from
   two places that never compared notes.

### Root cause

Two components each held half of the answer: the GUI decided the *name*, the engine
decided the *format*, and nothing reconciled them. SPEC.md §17 names this exact failure
class — "a single source of truth eliminates an entire class of 'the GUI says 60 fps but
the engine recorded 30' bugs" — and the filename extension had quietly become a second
source of truth for the container.

### The first fix was the wrong way round, and the tests said so

The obvious repair — the engine holds the container setting, so let it rewrite the
caller's extension to match — was written, verified end to end in both directions, and
was wrong.

It failed **three pytest cases and two GPU cases on all three presets**, deterministically,
with `gui_test.mkv` not existing. Those tests ask for a `.mkv` path; this machine's
`config.toml` said `mp4`; so the engine renamed their output. The change had made every
recording test's outcome depend on the developer's saved settings — the same class of
defect as the original, arriving from the other side, and precisely the ambient-state
dependence CLAUDE.md §5 exists to forbid.

The failure was the design telling on itself, not a set of stale assertions to update.

### Fix

**The path decides.** A `start_record` whose output ends in `.mp4` writes MP4, whatever
this engine's configuration says. That makes a recording request self-describing: it means
the same thing on every machine, which is what makes it reproducible and what makes a test
independent of who is running it. The configured container still decides when the path
names nothing usable, and the name is then made to match it.

**The GUI asks the engine for the container** rather than hard-coding an extension, so a
user's choice still reaches the file: setting `mp4` → GUI names `.mp4` → engine writes MP4.
Fixing only this would have left the next client free to repeat the mistake; fixing only
the engine, as above, broke reproducibility. Both halves are needed and they pull in
opposite directions, which is why getting the *direction* right mattered more than the
change itself.

`config::extension_for` and `container_from_extension` are a new pair, kept separate from
`to_string`/`container_from_string` because they answer different questions that merely
agree today: one names the setting, the other names the file. They diverge the moment a
container gains an alias — `.m4v` for MP4 — and a caller reusing the setting's name as an
extension would then be wrong with nothing changing at its own call site.

### Regression test

`test_the_file_written_is_the_container_the_path_names` (pytest, `engine`), parameterised
over both extensions and **configuring the engine to the opposite container each time**,
so a build that let the setting win fails rather than coincidentally passing. It asserts
the file exists where it was asked for, that the engine echoed that same path, and — the
point of the whole entry — that the *bytes* are the container the name claims: `ftyp` at
offset 4 for MP4, the EBML magic `1A 45 DF A3` for Matroska.

Verified red before the fix, and the failure message is the user's bug verbatim:
`not a Matroska file: b'\x00\x00\x00 ftypisom'`.

`ConfigSchema.EveryContainerRoundTripsThroughItsExtension`,
`ExtensionMatchingIgnoresCase` and `AnExtensionWeDoNotWriteNamesNoContainer` (cpu) pin the
two helpers, including the `nullopt` cases that are what make the fallback reachable
rather than a guess.

### Lessons

- **The log had already said it, in one line, for every affected recording.**
  `path="....mkv" format=mp4` is self-contradictory on its face, and nothing — no test, no
  assertion, no reader — was looking. A line that contains both halves of an invariant is
  worth an assertion somewhere.
- A configuration value with two consumers needs one of them to be authoritative, and the
  other to ask. Here both decided, and the one that decided the *name* was the one the
  user could see.
- **Which one is authoritative is a real decision, and the first answer was wrong.**
  Making the engine's setting win looked natural — it is the layer that opens the muxer —
  and it turned a per-request property into ambient process state. The test suite caught
  it in one run because five recording tests suddenly depended on a value in
  `%LOCALAPPDATA%`. A fix that makes previously-deterministic tests depend on machine
  configuration is telling you something about the design, not about the tests.
- The report was "recorded as MKV". The truth was "MP4 with the wrong name". Those need
  different fixes, and only the log distinguished them — worth remembering before acting
  on a symptom's description.

---

## [BUG-042] Timestamp jitter was read as missing audio, and 2591 micro-silences were spliced into a 40-second recording

**Severity:** Critical (audible on every recording made from the real endpoint)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

### Symptom

Reported from real use: "the audio is not 100% clean, has that 60s or 70s style sound
effect in the audio", with sync explicitly ruled out by the reporter — "audio is perfectly
synced, just the audio quality is the issue".

### Investigation

The recording was decoded and the chain eliminated end to end, because the metrics all
looked healthy — `worst_drift_us=10`, `soft_resyncs=0`, `hard_resyncs=0`, one
discontinuity.

1. **Format, spectrum, clipping, channels: clean.** 48 kHz stereo; natural spectral
   rolloff with no band-limiting; 4 samples over full scale out of 5 M; inter-channel
   phase 15.7° mean with no fixed all-pass structure, so no phaser or widening effect.
2. **Bitrate: 194.2 kbps against 192 configured.** Correct.
3. **The AAC encoder was exonerated by experiment, not by inspection.** A steady reference
   — pink noise plus fixed 10/12.5/15 kHz tones, no modulation of its own — was encoded
   through the engine's exact settings and decoded back. Tone stability went from cv 0.011
   to 0.012 and level from 0 to −0.0 dB. The codec does not warble.
4. That left the timeline. The engine's own summary said
   `silent_buffers=0 discontinuities=1` — WASAPI reported no gaps — beside
   `silence_frames=12549`, **261 ms of manufactured silence** the endpoint had not asked
   for.
5. The first silence scan of the decoded audio missed it, because AAC smears injected
   zeros into a dip rather than leaving them exact. Scanning for *level collapses* instead
   found 60 dips totalling 760 ms, mean 12.7 ms.
6. **`gap_fills` and `timeline_drops` did not exist as diagnostics.** They were added, and
   a 40 s loopback recording then said it outright:

   ```
   frames_written=1925812 silence_frames=10562 gap_fills=2591 timeline_drops=4
   ```

   **2591 separate fills averaging 4.1 frames — 85 microseconds — each**, on roughly three
   buffers in five.

### Root cause

`AudioTimeline::accept` filled *any* forward discrepancy between a packet's mapped start
and the write head:

```cpp
if (start > frames_written_) {          // no tolerance of any kind
    segment.silence_frames = start - frames_written_;
```

`start` comes from the packet's QPC stamp, and WASAPI stamps each buffer with the QPC at
which it was captured — a value carrying tens of microseconds of scheduling noise. **An
endpoint cannot lose 85 µs of audio; it delivers whole buffers.** So the discrepancy was
never missing audio, and filling it spliced silence into continuous sound. Worse, the next
packet then mapped behind the advanced head and was trimmed against it, so real samples
were discarded to make room for the silence.

Individually these are inaudible. At one every 15 ms they are a periodic discontinuity
around 65 Hz, heard as roughness and grit rather than as dropouts — which is why every
"is the track the right length" assertion in the tree passed. The track *was* the right
length. It was full of holes.

The §8.2 silence *watchdog* had a `2 × buffer_period` threshold all along
(`needs_silence_tick`). The gap fill in `accept` had none. That asymmetry was the bug.

### Fix

A jitter guard at one buffer period — the smallest thing an endpoint can actually lose —
below which the packet is snapped to the write head instead of having silence put in front
of it. **Symmetric**, because the noise is: a stamp landing *behind* the head is the same
jitter with the other sign, and the overlap trim discarded real samples for it exactly as
a forward gap inserted silence. Measured: 4 frames lost across 400 jittery buffers, the
identical mistake in the inaudible direction.

Nothing is dropped and nothing is manufactured — the track carries every frame the
endpoint delivered. What the guard gives up is nulling the endpoint's clock error on every
buffer; it now accumulates as an offset between track and wall clock, which is exactly the
quantity SPEC.md §8.4's ladder exists to measure and correct in its soft band.

### Measured, before and after, same 40 s loopback recording

| | before | after |
|---|---|---|
| `silence_frames` | 10562 (220 ms) | **0** |
| `gap_fills` | 2591 | **0** |
| `jitter_snaps` | — | 3902 |
| `worst_drift_us` | 10 | 742 |
| soft / hard resyncs | 0 / 0 | **0 / 0** |

The drift rising from 10 µs to 742 µs is the predicted cost, and it is the fix working
rather than a regression: 742 µs over 40 s is 18.5 ppm of endpoint clock error, sitting
inside §8.4's 5 ms do-nothing band with the ladder untouched.

**The old 10 µs was not health.** It was the clock error being nulled against the audio
every 15 ms, which is what the holes were. The honest figure is what the endpoint's
crystal actually does, and §8.4 exists to carry it.

Over five minutes, where that error has room to accumulate and the ladder has to act:

```
frames_written=14412000 (300.25 s)  silence_frames=0  gap_fills=0  jitter_snaps=30024
worst_drift_us=14291  soft_resyncs=18  hard_resyncs=0
```

18 soft resyncs, no hard ones, worst 14.3 ms against the 40 ms band — `swr_set_compensation`
absorbing the endpoint's drift smoothly and inaudibly, which is precisely the division of
labour §8.4 ratified in M4: the timeline places, the ladder corrects.

### Regression test

`AudioTimeline.TimestampJitterSmallerThanABufferIsNotTreatedAsMissingAudio` (cpu) feeds
400 buffers with deterministic sub-buffer jitter and asserts no silence is injected, no
packet dropped, and every delivered frame present — with `snaps() > 0` as the control, so
it cannot pass by the jitter never reaching the guard.

`AudioTimeline.AGapOfAWholeBufferOrMoreIsStillFilled` is the other half, and the reason
the threshold is a whole buffer rather than a number chosen to make the symptom go away:
a genuine lost buffer must still be filled, or this fix would be BUG-016 again with audio
quietly shortening whenever the device really dropped something.

`AudioTimeline.SnappingLosesNoFramesAndManufacturesNone` runs a drifting endpoint for a
simulated minute and holds the track to exactly the frames delivered.

### The symmetry was not tidiness, and a long test proved it

The first version of the guard handled only the forward direction — fill nothing under a
buffer, leave the backward case alone. It passed every CPU-tier test, and it passed a 40 s
loopback recording with the headline numbers above.

`LoopbackRecordingTest` at `FC_LOOPBACK_SECONDS=300` then failed catastrophically:
**76 seconds of audio in a 300-second recording**, audio and video 224 s apart. Snapping
the head forward without also handling packets that land behind it left those packets to
the overlap path, where one entirely behind the head is *dropped* — and as the head was
snapped forward again and again, more and more of them were. Forward-only did not merely
leave half the defect in place; it converted an inaudible one into the loss of three
quarters of the audio.

The symmetric guard passes the same run. The lesson is about the test rather than the
code: a five-minute recording is the shortest one on which this failed, the routine suite
runs twelve seconds, and the 40 s check that preceded it looked perfect.

### Lessons

- **Every existing audio test fed timestamps on an exact grid.** The harness supplied a
  clock with no noise on it, so the one property that mattered — what happens when the
  stamp is slightly wrong — was the one property never exercised. This is the third defect
  in this session with that shape, after BUG-038 (a supplied clock hid the drift loop) and
  BUG-037 (an injected capture factory hid the real one). A seam that makes a subsystem
  testable deletes the behaviour it replaces, and that behaviour needs a case of its own.
- **The metrics were all green and all true.** Track length correct, drift 10 µs, zero
  resyncs, one discontinuity. The 10 µs drift was not evidence of health — it was the
  *symptom*, because it was being held there by nulling the clock error against the audio
  every 15 ms. A number that looks too good is worth the same suspicion as one that looks
  wrong.
- **The diagnosis needed a counter that did not exist.** `silence_frames` is a total, and
  a total cannot tell one clean gap from sixty holes. `gap_fills` took four lines and
  turned an inconclusive investigation into a one-line answer; it is now logged for every
  recording.
- Ruling things out by experiment beats ruling them out by reading. The AAC encoder was
  the obvious suspect and would have stayed a suspect for a long time; twenty seconds of
  steady tones settled it.

---

## [BUG-041] A warning announced the loss of a feature that was still working

**Severity:** Minor (misleading diagnostics; no functional defect)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

### Symptom

`IAudioClock2 unavailable; device-position cross-check is disabled`, at WARNING, on a
user's recording — 18 times across one log. And then, one second later and every second
after it:

```
endpoint clock cross-check device_vs_qpc_us=-171 device_frames=48000 elapsed_s=1
```

The cross-check the warning said was disabled, running, with real device positions.

### Investigation

1. `ComPtr<IAudioClock2> clock` is declared, acquired at `open_endpoint`, warned about
   when the acquisition fails — and **never dereferenced anywhere**.
2. `LoopbackBuffer::device_position_frames` is filled from the `device_position`
   out-parameter of `IAudioCaptureClient::GetBuffer`, which `drain_available` already
   calls for every packet. That is what reaches `cross_check_device_clock`.
3. So the cross-check never depended on `IAudioClock2` at all, and no endpoint can turn it
   off by not offering that interface.

### Root cause

A message written from the design's intent rather than from the code's behaviour. SPEC.md
§8.3 names `IAudioClock2::GetDevicePosition`, the implementation took the same quantity
from a source it was already reading, and the warning was never revisited to match.

### Fix

Reworded to state what is true, and demoted to DEBUG — the interface's absence is worth
recording and is not worth a warning, since nothing degrades. The acquisition is kept
because §8.3 names it; whether §8.3 should be re-pointed at `GetBuffer` is noted for the
owner in `docs/ACCEPTANCE.md`.

### Lessons

- **This one did damage before it was found.** BUG-037's write-up cited this warning as
  corroborating evidence that a COM apartment teardown had broken a second subsystem. It
  was adjacent to a real defect and pointed the same way, so it was believed without being
  checked. That entry now carries the retraction. A false signal beside a true one is the
  cheapest way to believe something wrong.
- A log line is an assertion about the program, and nothing type-checks it. This one had
  been wrong since it was written, in a subsystem with tests that pass, because no test
  asserts on log text and no reader had both lines in view at once.

---

## [BUG-040] The validation gate decoded the whole recording, so stopping cost 14 s and scaled with length

**Severity:** Major (user-visible hang; the direct cause of BUG-039)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

### Symptom

Reported from real use: "after I clicked stop it took quite a while to save the recording
(should be instantaneous or quick at least, like OBS) and also just before it saved
successfully the GUI hanged and displayed 'not responding'".

From the engine's log, on a 111.6-second recording:

```
13:53:22.899  container finalized   packets=11925 bytes=32213514 write_p99_us=84
13:53:37.166  recording finalized and validated  duration_s=111.613 decoded_frames=6693
```

**14.27 seconds** between the container being written and the recording being declared
finished. The muxing itself was done at 13:53:22.899 — the file was complete and on disk.

### Investigation

1. `decoded_frames=6693` is every frame in the file, so the gate was decoding all of them.
2. `Muxer::validate` reads to EOF with `av_read_frame` and feeds every video packet and
   every audio packet to a decoder — 6693 frames and 5.36 M samples.
3. So its cost is proportional to the recording's length, at roughly **12.8 % of real
   time** on this rig. A half-hour recording would spend nearly four minutes here.
4. SPEC.md §10.4 does not ask for that:

   > **Validate before declaring success:** open the output with libavformat, confirm
   > stream count, duration within 1% of expected, and that the **first and last frames
   > decode**.

   First and last. The full decode was an implementation that had drifted past its spec,
   and nothing measured the cost because every test records for seconds.

### Root cause

`validate` proved a stronger property than §10.4 asks for, at a price that scales with
the thing being recorded. It ran synchronously inside `stop_record` on the IPC thread, so
the price was paid with the control channel held.

### Fix

Head and tail probes. Read from the start until one video frame (and one audio frame, when
audio is expected) has decoded; then `av_seek_frame` backwards to `duration - 5 s` and
decode to EOF. Five seconds is wider than SPEC.md §9's 2 s keyframe interval, so the seek
lands on a keyframe and the closing pictures decode with their references present.

Every check survives: stream count, decodable content at both ends, channel count against
what was recorded, the `AudioSpecificConfig`, duration within 1%, and the two tracks
against each other. Two derived quantities had to change how they are computed, and both
got *better* rather than merely cheaper:

- The video track's length used to add one frame estimated as `span / (decoded_frames-1)`
  — an average. It is now the measured interval between the last two decoded frames, which
  is the right one once SPEC.md §13 rung 3 can change the frame rate mid-recording. That is
  BUG-025's lesson applied a third time.
- The audio track's length used to be a sum of every decoded sample. It is now the track's
  span, first PTS to the end of the last frame, which is the quantity actually being
  compared against the video track.

**Any seek failure falls back to reading the rest of the file.** Correctness before speed:
a refused seek gives the old behaviour, not a weaker check.

### What this no longer catches, stated rather than left implicit

A file that decodes at both ends and is corrupt only in the middle. That is a disk that
lied about a write during one continuous muxer run, and §10.4's answer to it is the
`.fcrecover` sidecar and the recovery path — not a full decode on every stop. The
`write_p99_us` and `worst_write_us` figures the muxer already reports are the signal that
the disk misbehaved.

### Regression test

Measured end to end through the real IPC path, on a 120-second recording with audio:
**`stop_record` returned in 0.945 s**, against 14.27 s for a 112-second recording before —
and the file still validated (`valid=True duration_s=120.366`). The engine stayed up.

`CrashRecoveryTest.*` (gpu, 4) is the case that matters most and passed unchanged: it
validates deliberately truncated MP4 and MKV files, which is exactly where a head-and-tail
probe could have gone wrong. `SlowDiskTest.*` and `VideoEncodeMkvTest.*` cover the gate's
ordinary path.

### Lessons

- **A test that only records for seconds cannot notice a cost that scales with minutes.**
  Every test in the tree records for 2–20 s, where the gate cost 0.1–1 s and looked free.
  The first 112-second recording anyone made was on a user's machine.
- An implementation that proves *more* than its specification is not automatically safer.
  Here the extra proof cost 12.8 % of the recording's length, held the control channel
  while it did, and directly caused a worse failure than the one it was guarding against.
- The spec was right and specific — "first and last frames" — and the code had quietly
  become something else. Re-reading §10.4 was faster than any amount of profiling.

---

## [BUG-039] The engine decided its host was dead while it was busy doing what that host asked

**Severity:** Critical (the engine terminates itself during finalization)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

### Symptom

The GUI reported its engine had vanished:

```
IpcError: get_stats timed out after 5.0s
OSError: [Errno 22] Invalid argument          (control pipe read ended)
IpcError: the engine went away while waiting for get_stats
```

The engine's own log, at the same instant, blamed the GUI:

```
13:53:22.899  container finalized ...
13:53:28.039  ERROR  host heartbeat lost; finalizing the recording and exiting
                     silence_ms=5169 timeout_ms=5000 error=IPC_HEARTBEAT_TIMEOUT
13:53:28.039  engine exiting reason=heartbeat_lost
13:53:37.166  recording finalized and validated  duration_s=111.613
```

Both processes concluded the other had died. Both were alive. **No crash dump was
written, because nothing crashed** — the engine chose to exit.

### Investigation

1. The timestamps bracket it exactly: the watchdog fired at 13:53:28.039, inside the
   window from 13:53:22.899 (container finalized) to 13:53:37.166 (validation complete).
   The engine was inside `stop_record` for all of it (BUG-040).
2. `EngineService::Impl::handle` calls `watchdog.notify()` before dispatch, with a comment
   saying it is done there "so a slow command does not make the host look dead". Necessary,
   and not sufficient: the timestamp is fresh at the *start* of the command and the problem
   is the 14 s after it.
3. `PipeServer::Impl::pump_client` reads one request, dispatches it, and only then reads
   the next — one thread, serially. So for the whole of a long command nothing can call
   `notify`, whatever the host is doing.
4. The GUI's side is the mirror image: its heartbeat is a `get_stats` request with a 5 s
   timeout, and the engine could not answer it for the same reason.

### Root cause

The watchdog measured **"requests dispatched"** and called it **"host liveness"**. Those
are the same quantity only while the engine is keeping up. When the engine itself is the
reason no request arrived, the measurement inverts: the engine reads its own silence as
the host's death.

### Fix

`GuiWatchdog::Suspension`, a scoped guard taken around request dispatch. While it is held
the watchdog counts no silence, because a request in flight is proof the host was alive
when it sent it and the host is blocked on the reply — silence during a command carries no
information about the host at all.

On release the clock is re-armed **from now**, not from the last `notify`. The engine
genuinely does not know whether the host survived the intervening seconds, so it starts a
fresh timeout rather than crediting or condemning it. Re-arming from the last `notify`
would have found the whole command's duration already waiting and fired on the next poll,
which is the original bug with an extra step.

A depth counter rather than a flag: nesting is not expected today, and a counter cannot be
left un-suspended by an inner scope ending before an outer one.

### What it costs

SPEC.md §20 row 13's 6 s budget is measured from the host dying *during a recording*, when
no command is in flight, so it is unaffected — `OrphanPreventionTest` passes unchanged. The
case that is extended is a host dying during a long command, to that command's duration
plus the timeout. That is the right trade: killing the engine mid-finalize is what
CLAUDE.md §1's prime directive exists to prevent, and with BUG-040 fixed the longest
command is now under a second.

### Regression test

`GuiWatchdog.*` (cpu, 4), verified red before the fix and green after — two of the four
fail without it, and the other two are the controls that stop it being vacuous:

- `AHostIsNotDeclaredLostWhileTheEngineIsBusyWithItsOwnCommand` — the bug.
- `TheTimeoutRestartsWhenTheCommandFinishesRatherThanFiringImmediately` — the re-arm.
- `ASilentHostIsReportedLost` — the positive control. Without it the two above would pass
  on a watchdog that had simply been switched off.
- `AHostThatDiesDuringACommandIsStillReportedOnceItFinishes` — suspension defers detection
  and must not cancel it.

### Lessons

- **A watchdog that shares a thread with the work it watches is watching the wrong thing.**
  The signal it read was not "the host is alive" but "this process is keeping up", and
  those diverge exactly when a watchdog matters.
- When two components each report that the other has failed, neither has: look for the one
  resource they share. Here it was the single dispatch thread.
- The comment at the `notify()` call already named the hazard — "so a slow command does not
  make the host look dead" — and the mitigation it described only covered commands shorter
  than the timeout. A comment that states an intent is worth checking against the case that
  intent was written for.

---

## [BUG-038] A pause was reported as audio drift, exactly, and the ladder corrupted the track trying to correct it

**Severity:** Critical (corrupts the audio of any paused recording with a real endpoint)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

### Symptom

A 15.9 s recording from the real GUI with a single 2 s pause. The file validated, played,
and was the right length. Its audio was corrupt.

```
recording included paused time   pauses=1 paused_total_ms=2021 ... timeline_s=15.850
AUDIO_HARD_RESYNC                drift_us=-2021263 correction_frames=97020
audio path finished              worst_drift_us=2021273 hard_resyncs=7
```

Seven hard resyncs in fifteen seconds, asking to inject 97,020 correction frames — into a
track 760,715 frames long. SPEC.md §8.4 says of this band: "this should essentially never
fire; if it does, it is a bug report, not a normal event."

### Investigation

1. `paused_total_ms=2021` and `drift_us=-2021263` are the same number. Not approximately —
   to within the microsecond the two are printed at. A drift measurement that equals the
   pause duration is not measuring drift.
2. `AudioEncodePath::Impl::evaluate_drift` computes `elapsed = now_ns - t0` and hands it to
   `DriftCompensator::evaluate(frames_written, elapsed_ns)`, whose contract read
   "`elapsed_ns` is QPC time since `t0`" (`drift_compensator.h:81`).
3. The other argument, `canonical_written`, is the encoded track's length. SPEC.md §7.5 has
   the track excise paused time; §8.2's timeline refuses every packet inside a paused span.
   So one argument had paused time removed and the other did not.
4. The sign confirms it. Negative drift means the track is *short* against the reference,
   and it was short by exactly what the pause removed from it.
5. The ladder never converged because it never could: `swr_set_compensation` acts on the
   track, and the discrepancy was in the reference. Each second it re-measured the same
   2 s and asked again.

### Root cause

M8 wired `timing::PauseClock` into `timing::Pacer` and `audio::AudioTimeline` — the two
places that *place* items on the timeline. It did not wire it into the one place that
*measures* the timeline. §7.5 gave `timeline_ns = qpc_ns - t0_ns - paused_total_ns` a
second term, and one consumer of the old two-term form was left behind, holding a
correctly-worded comment that had silently become false.

### Fix

`evaluate_drift` takes its elapsed term from `timing::PauseClock`, and a paused span is not
measured at all rather than measured against a frozen timeline — neither term advances
during one, so there is nothing a correction could converge on.

`PauseClock` grew `observe()` beside `map()`: identical arithmetic, written once in a
shared private `locate()`, differing only in that `observe` does not increment the
straggler counter. That distinction is not cosmetic. `stragglers()` is an assertion target
— SPEC.md §20 row 18 requires zero — and a measurement that consulted the clock in the
same instant as a late item would have inflated a number meant to prove the quiesce held.

`drift_compensator.h`'s contract now names the clock instead of describing it, and says
why: **both arguments must be measured on the same clock.**

**SPEC.md §8.3's device cross-check was checked in the same pass and is correct as it
stands.** It looks like it has the same defect and does not: both of its terms are
wall-clock quantities — the endpoint's own frame counter and QPC — and neither stops
during a pause, so excising paused time from one would manufacture a delta of exactly the
pause duration, which is this bug in mirror image. The asymmetry is the point: §8.4
measures the encoded track, which paused time never reaches; §8.3 measures the device's
crystal, which it does. Measured across the field's 2 s pause: `device_vs_qpc_us` stayed
inside ±550 µs throughout while §8.4's ladder read −2,021,263 µs.

### Regression test

`AudioPathTest.APausedSpanIsExcisedFromTheDriftReferenceAsWellAsFromTheTrack` (cpu) is the
deterministic form: three seconds, a two-second pause during which the endpoint keeps
delivering as WASAPI really does, three seconds more. Measured with the fix disabled,
**2,000,000 µs of drift against a 2,000,000 µs pause and 5 hard resyncs**; with it,
0 resyncs inside §8.4's 5 ms do-nothing band. It runs in 75 ms.

`LoopbackPauseTest.PausingARecordingDoesNotDesyncTheAudioTrackFromTheDeviceClock` (gpu) is
the same thing against the real endpoint at wall-clock speed, calling `pause()` and
`resume()` rather than `pause_at()`. Measured before: **worst drift +2,007,079 µs, 5 hard
resyncs**. After: **+9 µs against a 40,000 µs band, 0 soft and 0 hard resyncs**, audio
8.043 s against video 8.017 s.

Both state their bound against *the pause duration* rather than against §8.4's 40 ms band.
A 2 s pause is fifty times the band, so a band-only assertion would fail without saying
which two seconds it was.

`AudioPathTest.TheDeviceCrossCheckIsUnaffectedByAPause` (cpu) pins §8.3's correctness, and
passes both before and after — a negative control for the paragraph above.
`PauseClock.ObserveIsMapWithoutTheStragglerBookkeeping` (cpu) holds the two entry points to
the same arithmetic.

### Lessons

- **A test can exercise the seam it was written for and still leave the production path
  untested.** SPEC.md §20 row 18's test drives `AudioSource::External`, where both streams
  come from one supplied clock. That path has no wall-clock drift loop *at all*: §8.4's
  ladder compares the track against `qpc - t0`, and when the test supplies both, a pause
  cannot desynchronise them. Row 18 measured a worst A/V offset of −0.188 ms and was
  correct about what it measured. **"Row 18 is Green" was true and misleading at the same
  time.** The row's claim was narrower than its wording, and nothing in the wording said
  so. `docs/ACCEPTANCE.md` now states what each of the two cases proves, separately.
- The injectable seam that makes a subsystem testable is the same seam that hides the
  production path behind it. Every `AudioSource::External` test and every
  `capture_factory` test buys determinism by not running the thing that ships — see
  BUG-037, which is the identical shape on the video side of the same session.
- This is BUG-025's shape again: a control-flow change silently invalidating an arithmetic
  assumption elsewhere. There the derived quantity was `emitted() / fps`; here it was
  `qpc - t0`. Both were correct for milestones and became wrong the moment a feature
  designed to change the underlying term was switched on. The tell is the same in both
  cases — a comment stating the assumption, still accurate-sounding, no longer true.
- When two logged numbers are equal to the microsecond, they are the same quantity.
  `paused_total_ms=2021` next to `drift_us=-2021263` located this before any code was
  read.

---

## [BUG-037] The second recording in one engine process faulted on a WinRT factory whose library had been unloaded

**Severity:** Critical (crashes the engine; the recording in flight is lost)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

Reported as **BUG-A** in the session that found it; BUG-038 is the same session's **BUG-B**.

### Symptom

Start the GUI, record once with a pause, stop, let it finalize, save settings, start a
second recording. The engine dies.

```
exception_code    0xC0000005
exception_address 0x00007FF6FE9C413E
thread_id         28544
engine_phase      "uninitialized"
```

The ring log's last lines are a clean open — encoder, audio path, muxer, recovery sidecar,
"video pipeline started" — and then nothing. The first recording had finalized and
validated correctly.

### Investigation

No debugger on the rig, so the minidump was parsed directly and symbolised through
DbgHelp against the matching PDB.

1. **The faulting frame.** `0x7FF6FE9C413E` resolves to
   `fc::capture::wgc::is_supported+0x4E`, `wgc_capture.cpp:81` — the
   `GraphicsCaptureSession::IsSupported()` call. The stack below it reads
   `create_capture` → `open_capture` → `RecordingSession::start` →
   `handle_start_record` → `PipeServer::pump_client`, so this is the IPC thread
   servicing the second `start_record`.
2. **The instruction.** The bytes at the fault are `48 8b 01` (`mov rax,[rcx]`),
   `48 8d 54 24 50`, then at `0x413E` exactly: `ff 50 30` — `call [rax+0x30]`. `rax` was
   `0x7FFA38D8B0A8`, the exception's second parameter was `0x7FFA38D8B0D8`, and
   `0xA8 + 0x30 = 0xD8`. So the interface pointer was readable, its vtable was not, and
   slot 6 of an `IInspectable`-derived vtable is the first non-inherited method:
   `IsSupported`.
3. **Where that address lived.** No *loaded* module covers `0x7FFA38D8B0A8`. The dump's
   **unloaded**-module list does: `GraphicsCapture.dll`, base `0x7FFA38D60000`, size
   `0x41000`. It had been loaded and then unloaded.
4. **What else went with it.** `WinTypes.dll`, `rometadata.dll`, `dcomp.dll` and
   `OneCoreUAPCommonProxyStub.dll` unloaded in the same group. That is not a library
   deciding it was idle; that is the WinRT/COM stack being shut down.
5. **Who shut it down.** Three places bracket themselves with an apartment and all three
   leave at stop: the `capture` thread (`winrt::init_apartment` /
   `uninit_apartment`, SPEC.md §4.2), the `audio` thread (`CoInitializeEx` /
   `CoUninitialize`, §8.1), and endpoint enumeration's `ScopedApartment`. Each is correct
   on its own. Between two recordings, none of them is in the MTA, so COM tears it down.
6. **Why that reaches a *cached* pointer.** C++/WinRT caches activation factories in
   process-wide statics and never revalidates them —
   `winrt::impl::factory_cache_entry_v<GraphicsCaptureSession, IGraphicsCaptureSessionStatics>`
   is on the stack two frames from the fault.

**A retracted claim, left visible rather than edited out.** This entry originally offered a
corroborating detail: that the second recording opened with `IAudioClock2 unavailable;
device-position cross-check is disabled` where the first had cross-checked normally, and
called it the same apartment teardown surfacing in a second subsystem. **That was wrong.**
A later session with this fix in place logged the same warning on the *first* recording of
the process, and the cross-check then ran normally every second regardless — because
`IAudioClock2` is acquired and never dereferenced, and §8.3's device position actually
comes from `IAudioCaptureClient::GetBuffer`'s out-parameter. The warning is unrelated to
apartments, and it is also simply untrue; see BUG-041.

The dump alone carries this entry. The coincidence looked like evidence because it was
adjacent to a real defect, which is the cheapest way to believe something false.

### Root cause

The engine process is long-lived (SPEC.md §3.1) and every COM apartment in it is
recording-scoped. Nothing owned the gap between recordings. C++/WinRT's factory cache is
process-scoped, so it outlives the apartment that made its contents valid, and the second
recording calls through a vtable in an unmapped library.

**M8 did not introduce this.** M8 made it *reachable*: M8a built the transport and M8b the
GUI, and together they are the first way to start a second recording in one process. The
defect was latent in the apartment handling from the moment WGC was written.

### Fix

`fc::hold_process_mta()` — `CoIncrementMTAUsage` once, idempotent, never released — called
from the four places that enter or use an apartment: `wgc::is_supported`, the WGC worker
thread, the audio capture thread, and `ScopedApartment`. With the MTA held for the
process, a thread leaving no longer takes it with it, the in-proc servers stay loaded
because the cache still references them, and every existing per-thread bracket keeps
meaning exactly what it says.

`winrt::clear_factory_cache()` was the alternative and was rejected: it treats the symptom
on one library's behalf, races anything mid-call on a process-wide cache, and does nothing
for the COM state that is not a WinRT factory — the missing `IAudioClock2` above.

### Regression test

`SecondRecordingTest.TheCaptureBackendCanBeProbedAgainAfterARecordingHasTornItsApartmentDown`
(gpu). Verified red before the fix and green after, and getting the red required one
non-obvious thing:

**A first version of this test passed on the broken build.** C++/WinRT's
`get_activation_factory` retries with `CoIncrementMTAUsage` when `RoGetActivationFactory`
returns `CO_E_NOTINITIALIZED` — so a probe from a thread that has *never* entered an
apartment pins the process MTA as a side effect, and the defect cannot occur. A bare test
thread is that thread; the engine's IPC thread is not, having already been in and out of
an apartment for device enumeration. The test now enters and leaves an MTA around the
first probe, which is what the engine does.

Pre-fix, run in that shape, the case failed as
`SEH exception with code 0xc0000005 thrown in the test body` — the field crash, in the
test binary. The assertion was then moved one step upstream onto the invariant that has to
hold for the call to be safe (`GetModuleHandleW(L"GraphicsCapture.dll")` still non-null),
because an access violation takes the runner down and reports nothing about why. It now
fails cleanly with the reason written out, and the call that used to fault runs behind it.

`test_a_second_recording_in_the_same_engine_process_runs` (pytest, `engine`) is the
reported sequence at the seam it was reported from: record with audio, pause, resume,
stop, `save_config`, record again. Audio is on deliberately — the audio thread's apartment
is one of the three that leave at stop.

### Lessons

- **A process that outlives the work it does needs someone to own the gaps.** Every
  apartment here was correctly scoped to a recording, and correctness of each scope said
  nothing about the state between them. The engine had recorded once per process for seven
  milestones, so "between recordings" had never existed.
- **An injected test seam hides exactly the code it replaces.** Every pipeline test either
  records once and exits, or supplies `PipelineSettings::capture_factory` — and that
  injection skips `capture::create_capture` entirely, which is the call that faulted. The
  same session's BUG-038 is the identical shape on the audio side. A seam that makes a
  subsystem testable should be paired with one case that does not use it.
- **`engine_phase` read "uninitialized" and that is a defect of its own, not a clue.**
  `crash_handler.h` says the field "stays `Uninitialized`" until M6 wires it up. M6 did not:
  nothing in the tree calls `set_engine_phase` except `main.cpp`, with `Stopped` and
  `Faulted`. Every crash report so far has carried a field that says nothing. Left as
  found and recorded here rather than fixed in passing — it is worth doing, and it is not
  this bug.
- **A claimed limitation is worth one command to check** (CLAUDE.md §6), and so is a
  missing tool. "There is no debugger on this machine" was true and did not mean the dump
  was unreadable: the module list, the exception record, the unloaded-module list and the
  faulting bytes are all plain structures, and DbgHelp will symbolise an address against a
  PDB without a debugger anywhere near it.

---

## [BUG-036] Row 13's budget was compared in two different units, so a passing engine failed and a failing one would have passed

**Severity:** Major (test correctness; no product defect)   **Found:** 2026-08-03   **Fixed:** 2026-08-03   **Commit:** _uncommitted_

**Symptom.** The first run of `OrphanPreventionTest.KillingTheHostLeavesNoEngineAndAFinalizedFile`
failed with:

```
Expected: (exit_after.count()) <= (kExitBudget.count()), actual: 5716 vs 6
the engine outlived its host by more than SPEC.md §20 row 13's 6 s
```

and, two lines below, printed its own measurement:

```
[row 13] engine exited 5716 ms after the host was killed (budget 6 ms);
         file valid with 444 frames, 7.400 s
```

The engine had done exactly what row 13 requires — noticed the dead host, finalized a
valid 444-frame file and exited, all in 5716 ms of a 6000 ms budget.

**Investigation.** Nothing to investigate in the engine: the failure message contains the
refutation. `kExitBudget` was declared `constexpr auto kExitBudget = 6s`, and
`std::chrono::seconds::count()` is 6. `exit_after` is `std::chrono::milliseconds`, so
`count()` is 5716. The comparison was `5716 <= 6`.

The *wait* was correct throughout — `wait_for_exit(engine_pid, kExitBudget)` takes
`std::chrono::milliseconds`, so the implicit conversion made it 6000 ms and the engine was
genuinely given its full budget. Only the assertion and the printed budget read the raw
count.

**Root cause.** `std::chrono`'s implicit conversions are duration-safe at the *type*
boundary and absent at the `.count()` boundary. Mixing the two in one file gives one
call site the conversion and denies it to another, with no diagnostic, because both
`count()` calls return `long long`.

**Why this one matters more than an ordinary slip.** The numbers happened to fall so that
the bug produced a *false negative* — a loud failure on correct behaviour, noticed
immediately. Reverse the operands' magnitudes and the same defect is a **false positive**:
a budget of `6000` compared against a measurement in seconds passes for any engine that
exits within 6000 seconds, which is every engine including one that never reaps at all.
Row 13's entire claim is a bound on a duration, so a unit error in the bound is a test
that reports on nothing. That is CLAUDE.md §6's "a green test that proves nothing is worse
than a red one", reached by arithmetic rather than by a weakened assertion.

**Fix.** The constant is declared in the unit it is compared in, and says why:

```cpp
/// **In milliseconds deliberately.** Written as `6s`, every comparison against a
/// millisecond measurement silently compares 5716 against 6 [...]
constexpr std::chrono::milliseconds kExitBudget = 6000ms;
```

Both call sites are unchanged and both are now correct — the wait because
`milliseconds` converts to `milliseconds`, the assertion because the counts share a unit.

**Regression test.** None added, and deliberately: the defect is *in* a test, and a test
that checked another test's units would be the same class of thing one level up. What
replaces it is that the case now prints its budget alongside its measurement in the same
unit on every run, so the two are readable against each other in any log — which is how
this was caught in the first place.

**Lesson.** A `.count()` in an assertion is a unit cast with no compiler check. Compare
`std::chrono` durations as durations, or declare the constant in the unit the measurement
arrives in. And a test that prints its own bound next to its own measurement is
self-checking in a way a bare assertion is not: here the printed line said "budget 6 ms"
and made a two-minute diagnosis out of what could have been a hunt through the engine's
shutdown path.

---

## [BUG-035] One burst of device failures rebuilds the session twice, one run in three

**Severity:** Major (product defect; SPEC.md §5.4, §13 rung 4)   **Found:** 2026-08-02   **Fixed:** 2026-08-02   **Commit:** _uncommitted_

**Symptom.** `WatchdogRecoveryTest.TheLadderCanTriggerARebuild` fails intermittently:

```
[ MEASURED ] rung 4: 2 rebuild(s), worst gap 84.6942 ms
one burst of device failures produced 2 rebuilds; either the trigger is not latched or
the watcher is reporting the rebuild's own activity as a new change, and a flapping
driver would thrash the recording
```

Both rebuilds succeed and both are well inside SPEC.md §5.4's 350 ms budget. Only the
count is wrong — and the count is the assertion, because coalescing a burst is the whole
point of the latch.

**Investigation.**

1. Surfaced in a full Release GPU tier run under background load, which made it look
   load-dependent. It is not: run alone on an **idle** machine it failed **4 of 12**
   times, and again **2 of 10** after the diagnostics below were added — **6 of 22, 27%**.
   A quarter of runs is not a flake, and the tier had only ever run this case once.

   Across six full GPU tiers this session it failed **2** times, both on Release
   (once under load, once not) and never on RelWithDebInfo or Debug. That is consistent
   with a 27% rate and six samples rather than with a build-specific defect; nothing
   suggests the optimiser is involved.
2. **Not caused by anything in this session.** `fc_core` is untouched — the session added
   files under `tests/` only — and `test_watchdog_recovery` drives `SyntheticSource`,
   which is also untouched. The defect is pre-existing and was exposed by running the
   case repeatedly rather than once.
3. The failure message names two candidate mechanisms and could not distinguish them,
   which is BUG-032's lesson arriving again: a count with no breakdown is not a
   diagnosis. `MigrationRecord` already carries `cause`, so the case now prints one line
   per rebuild.
4. **That settled it in one run.** Both rebuilds report `cause device_lost`:

   ```
   rebuild 1: cause device_lost, gap 83.2909 ms, same_file 1, failed 0
   rebuild 2: cause device_lost, gap 84.6942 ms, same_file 1, failed 0
   ```

   BUG-029's mechanism — the device watcher reporting the rebuild's own DXGI activity as
   a fresh topology change — would have made the second rebuild `topology_changed`. It
   does not. **The device-lost trigger itself is not latched across the burst.**

**Root cause. There was no latch — only a coincidence that usually held.**

`acknowledge()` discards reports that arrived *while* the rebuild was running, on the
stated grounds that they describe the device stack just replaced. Nothing discarded the
ones arriving a moment *later*, and they describe the same dead device for the same
reason. So whether a burst became one rebuild or two came down to arithmetic nobody had
written down:

- the case injects three `DEVICE_REMOVED` reports **50 ms** apart, so the burst spans
  ~100 ms;
- a rebuild takes **~82 ms** and ends with `acknowledge()`;
- if the rebuild finishes before the last report lands, that report survives, the next
  poll picks it up, and the recording is rebuilt a second time.

Which is to say: **the burst was coalesced for exactly as long as the rebuild happened to
last.** Faster hardware, a shorter burst, a slower one — any of them flips the outcome.
That is why it reproduced at 27%, and why both rebuilds report `device_lost` rather than
the `topology_changed` that BUG-029's mechanism would have produced.

This is not an artefact of the test's injection. `DXGI_ERROR_DEVICE_REMOVED` is delivered
to *every* thread that touches the dead device, each on its own schedule — so a real
device loss produces exactly this shape of burst, spread over a window the engine does
not control.

**Fix.** `gpu::kReportSettleNs`. After an `acknowledge`, `report_device_error` drops
reports for one settling window, because they describe the replaced stack. The value is
`kPollIntervalNs`, derived rather than tuned: it covers the slowest path by which a thread
still holding the old device can deliver a report — the capture thread blocks up to its
100 ms acquire timeout, the encode thread up to one bounded-queue drain — and equals one
watchdog tick, so at most one poll is affected. Suppressed reports are counted in
`settled_reports()` rather than vanishing.

**The cost, stated because it is real.** A genuinely new fault inside the window is not
reported. It is not lost: threads that hit the device keep reporting, so the next one
after the window is acted on, and the topology half of `poll` — `IsCurrent` and the LUID
diff — is never suppressed. The trade is up to one poll interval of delay on a second
real fault, against rebuilding on a fault that has already been repaired.

**Regression test.** Two on the CPU tier, where the timing is the test's to set rather
than the driver's.
`DeviceWatcher.ReportsArrivingJustAfterARebuildDescribeTheDeviceItReplaced` drives the
exact sequence and asserts both that the stragglers are counted and that no second
`DeviceRemoved` is reported; `AReportOutsideASettlingWindowIsActedOn` is the negative
control that keeps the window from becoming a way to lose faults.
`TheSpecifiedBudgetsAreWhatTheSpecSays` pins `kReportSettleNs == kPollIntervalNs` so the
derivation cannot be edited away by accident.

Measured on the GPU case that found it: **0 failures in 20 consecutive runs**, against
6 of 22 before — exactly 1 rebuild every time, gaps 86.9–97.1 ms against a 350 ms budget.
Then **159/159 on all three presets**, twice: the case had failed 2 of the 6 tiers run
before the fix and 0 of the 6 run after it.

**Lesson.** A coalescing guard whose window is "however long the work took" is not a
guard, it is a race with a comfortable name. `acknowledge()` read as though it closed the
door on a burst, and it did — for a duration that was never chosen, never written down and
never asserted. The tell was in the entry BUG-029 left behind: it fixed *reports generated
by the rebuild* and said so precisely, which should have raised the question of what
happens to reports generated by the original fault but delivered late. It took a 27%
failure to ask it.

**Lesson.** A test that runs once per tier hides a one-in-three defect for as long as
nobody runs it twice. Three consecutive green tiers said this case passed three times,
which is exactly the evidence BUG-033's lesson warns against reading as "the code is
right" — and it was the same suite, a day apart, that produced both.

---

## [BUG-034] A killed MP4 recording lost two GOPs instead of one, and load is not why

**Severity:** Major (row 3's guarantee)   **Found:** 2026-08-02   **Fixed:** 2026-08-02   **Commit:** _uncommitted_

**Symptom.** `CrashRecoveryTest.AKilledMp4RecordingIsPlayableWhereItStopped` failed in a
full Release GPU tier:

```
the killed recording decodes to 26.021333333333335 s of a 30 s recording;
SPEC.md §20 row 3 allows losing one GOP, which is 2 s
Expected: (decoded_seconds) >= (minimum_decoded(seconds)), actual: 26.0213 vs 27.9
```

It passed on RelWithDebInfo minutes later in the same batch, and has been green on every
tier before this one.

**What the numbers say.**

- **26.021 s is not a near miss — it is one whole fragment short.** `frag_keyframe` closes
  a fragment when the *next* keyframe arrives and SPEC.md §9 fixes the GOP at 2 s, so a
  decoded length lands on a 2 s grid. Measured values are **28.02** or **26.02**, sharing
  a fractional part and differing by exactly 1.98 s. There is nothing in between, so this
  case is effectively binary: the bar at 27.9 s sits **121 ms below** the good outcome and
  **1.88 s above** the bad one. It cannot degrade gracefully and it cannot half-fail.
- Run duration looks like a tell and is not. The failing Release case took 57.6 s against
  ~32 s standalone — but the *passing* RelWithDebInfo case took **58.7 s**. Both builds
  pay ~26 s of extra cost when this runs as the first crash test in a tier, and only one
  of them failed.

**Investigation. Six deliberate attempts under load, zero reproductions.**

| Condition | decoded_ms | lost_ms | Verdict | Wall |
|---|---|---|---|---|
| Idle, standalone, ×3 | **28021** | 1978 | pass | 31.7–32.5 s |
| 8 of 16 cores busy, ×3 | **28004** | 1995 | pass | 32.2–32.6 s |
| 16 of 16 cores busy + disk churn, ×3 | **28004** | 1995 | pass | 37.7–40.9 s |
| In-tier, Release | **26021** | 3978 | **fail** | 57.6 s |
| In-tier, RelWithDebInfo | — | — | pass | 58.7 s |
| In-tier, Debug | — | — | pass | 66.5 s |
| In-tier, Release, under 8-core load + disk churn | — | — | pass | 32.8 s |

The load was real — wall time rose from 32 s to 41 s under saturation — and the decoded
length did not move by so much as a frame. Nine standalone runs produced two values
17 ms apart and never crossed a fragment boundary.

**The one thing that does correlate is start-up cost, and it was nearly missed.** This
case takes **58–66 s** when it is the first crash test to run against a cold file cache,
and **32 s** when the cache is warm — the difference is ~26 s of process start-up, before
the recording begins. Every failure so far has been in a cold run (1 of 3); no warm run
has ever failed (0 of 10, three of them under saturation). The loaded tier above is warm
*because* the nine standalone runs preceded it, which is why it is not the load result it
looks like.

That is a correlation across four cold observations, not a mechanism — the start-up cost
is spent before `t0`, 30 seconds before the kill, and there is no obvious path from it to
a fragment missing at the end.

**So the obvious hypothesis is wrong, and is recorded as wrong rather than deleted.** The
first reading was "the muxer is behind real time when `TerminateProcess` lands, so the
fragment the 28 s keyframe would have closed was never written". That predicts
load-sensitivity, and the table above is what testing the prediction produced. Whatever
loses the fragment, it is not CPU or disk contention of the kind that can be applied from
outside the process.

What is still unexplained, and is where the next session should start: the only
reproduction so far is *in tier context*, where this case is the first crash test and pays
~26 s of extra start-up. That cost is present in both the failing and the passing tier
run, so it is not the cause by itself — but it is the one condition that separates the
runs that can fail from the nine that could not.

**Not** caused by this session's work. `test_crash_recovery` drives `fc_crash_recorder`,
which uses `SyntheticSource` and never touches the desktop, and runs in its own process
with no `ScreenAnimator` anywhere near it.

**Then the process was made to say what it knew when it died, and that ended the
guessing.** `Muxer::last_video_pts_ns` records the presentation time of the last video
packet handed to libavformat; `fc_crash_recorder` writes it next to the wall clock into a
progress file it rewrites and closes every 200 ms, so it survives `TerminateProcess` with
nothing to flush. Driving the recorder directly and killing it at 30 s:

| Run | elapsed at kill | last muxed PTS | **mux lag** |
|---|---|---|---|
| 1 | 29.927 s | 29.883 s | **43.5 ms** |
| 2 | 29.939 s | 29.900 s | **38.8 ms** |
| 3 | 30.025 s | 29.983 s | **41.9 ms** |

**The muxer was 39–44 ms behind, and the file decoded 1.98 s short.** Two seconds of
content had been handed to libavformat and was not on the disk. So the loss is not in the
pipeline at all — it is between "written" and "durable", which is the one place nobody had
looked because BUG-020 had already put a flush there.

**Root cause.** `av_interleaved_write_frame` **queues** a packet until it can interleave
it against the other stream. So the call in which we hand over a keyframe is very often
not the call in which movenc receives it, closes the previous fragment and writes the
`moof`. BUG-020's flush is triggered by *our* keyframe write, which means it fires at a
moment when the fragment has usually not been emitted yet — and when movenc does emit it,
a packet or two later, no flush follows. Those bytes then sit in the 256 KB AVIO buffer
until the *next* keyframe's write flushes them, one whole fragment later.

That gives exactly the observed shape: normally the file is one fragment short (the
fragment still open, which §10.3 promises), and sometimes two (the open one plus a closed
one still sitting in the buffer). It is binary because a fragment is the unit, and it is
insensitive to CPU and disk load because it depends on interleaving order rather than on
throughput.

**Stated honestly: the second fragment's fate was never caught in the act.** The failure
is rare and did not reproduce in 15 further attempts. What is *measured* is that the
muxer was 39–44 ms behind while 1.98 s was missing, which rules out the pipeline and
leaves buffering; the AVIO buffer is the only store big enough, and the interleaver
explains why the existing flush misses it. The fix is chosen so that being wrong about
the last step still fixes the row — see below.

**Fix, in two parts.**

1. **`AVFMT_FLAG_FLUSH_PACKETS`.** libavformat flushes after every packet *it* writes,
   which is the same call in which a fragment is closed. The flush is now where the
   writing is, instead of where we guessed it would be. Measured cost: unthrottled write
   p99 **153 µs**, against 160 µs recorded before the change — nothing.
2. **`frag_duration = 500 ms`**, so a fragment closes on time as well as on a keyframe.
   This is the part that does not depend on the diagnosis being right: it cuts the
   *whole* residual — whatever is still open when the process dies — to a quarter, and
   decouples it from the GOP length that made the outcome quantised in 2 s steps.

**Measured after, five runs:** loss **478–495 ms** where it was 1978 ms, and one run at
**−21 ms** (nothing lost at all). The split the new instrumentation reports: mux lag
0.041 s, open fragment 0.454 s. Row 3's margin goes from **121 ms to about 1.6 s**.

Consistent across all three presets, in-tier:

| Preset | Decoded of 30 s | Mux lag | Open fragment |
|---|---|---|---|
| Release | 29.521 s | 43 ms | 436 ms |
| RelWithDebInfo | 29.505 s | 47 ms | 448 ms |
| Debug | 29.505 s | 57 ms | 438 ms |

**A fragility the fix introduced, and closed.** With the loss down to half a second, the
decoded length now lands within ~100 ms of the *upper* bound — measured at 30.005 s
against a 30.1 s limit — because the recording's timeline starts slightly before the
ready marker the parent counts its 30 s from. Leaving that would have recreated exactly
the margin this entry is about, on the other side. The upper assertion now compares
against the elapsed time the recorder itself reports, so the tolerance is measured rather
than assumed.

**Regression test.** `AKilledMp4RecordingIsPlayableWhereItStopped` now asserts the two
terms **separately** — the muxer's lag at the kill, and the content lost beyond it — each
against `kMaxMp4TailLossSeconds` (1.0 s, twice the fragment duration). Asserting only the
total is what let a defect in the second term hide behind a plausible story about the
first for two occurrences. A future failure now says which one moved, and the numbers are
printed on every run.

**Spec notes, both needing the owner's pen rather than mine.**

- SPEC.md §10.3's `movflags` line is **unchanged** — `frag_duration` is a separate movenc
  option, not a movflag, and every flag the recipe names is still set. But the recipe's
  prose says "every 2 s keyframe closes a self-contained fragment", and that is now "every
  500 ms, or a keyframe, whichever comes first". Worth one line in §10.3.
- **MKV was deliberately left alone.** Its recoverability unit is the cluster and §10.2
  specifies `cluster_time_limit=2000` in as many words, so its kill still costs up to 2 s
  and its half of row 3 keeps the thin margin this entry is about. The same treatment
  would work; changing a number the spec states is not a test's call. Flagged in
  `docs/ACCEPTANCE.md`.

**Lesson.** The load hypothesis was good enough to write down and cheap enough to test,
and testing it cost nine runs and twenty minutes. Writing it down *without* testing would
have left a confident paragraph here for the next reader to build on. The negative table
above was worth more than the hypothesis, and it was worth it precisely for being
negative.

**Second, and the one that generalises:** a flush placed on the caller's schedule proves
nothing about a library that buffers on its own. BUG-020 correctly identified that the
AVIO buffer had to be flushed, and put the flush at the point *we* knew was a fragment
boundary — which is not the point at which libavformat writes one, because an interleaver
sits in between. The fix was not more flushing but flushing *where the writing happens*,
and the way to see the difference was to measure what the process knew at the moment it
died rather than to reason about where the bytes ought to be.

---

## [BUG-033] Five GPU-tier capture tests pass only when something happens to be moving on screen

**Severity:** Major (suite reliability; no product defect)   **Found:** 2026-08-01   **Fixed:** 2026-08-02   **Commit:** _uncommitted_

**Symptom.** A Release GPU tier reported 5 failures out of 156 where the same binary had
reported 156/156 twice earlier the same day:

- `SustainedCaptureTest.DdaCapturesOnTheAdapterThatOwnsTheOutput` — `DDA_UNSUPPORTED`
- `SustainedCaptureTest.CrossAdapterWgcFramesConvertToUsableNv12` — 0 frames converted
- `SustainedCaptureTest.DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` — start failed
- `SustainedCaptureTest.SustainedCaptureToNv12StaysCorrect` — 0 frames converted
- `CaptureToNv12Test.FrameSequenceNumbersAdvanceMonotonically` — 1 frame, 2 wanted

Debug and RelWithDebInfo, run immediately afterwards from the same tree, were 156/156.

**Investigation.**

1. Ruled out the session's own changes first, by inclusion rather than by argument:
   `test_capture_sustained.cpp` includes only the capture backends, the NV12 converter,
   the topology service and the logger, uses `SyntheticSource` **nowhere**, and none of
   those files were edited. There was no path from the session's work to these tests.
2. The failure shape was the tell. In `CrossAdapterWgcFramesConvertToUsableNv12` the
   *first* acquire succeeds — WGC hands over the initial frame — and then a five-second
   loop of 500 ms acquires yields nothing at all. Not a slow frame, no frame.
3. Corroborating measurement from a test that *passed*:
   `FrameSequenceNumbersAdvanceMonotonically` took **29.8 s** to collect 2 frames.
   And `DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` passes — it is the one test that
   *expects* a still desktop.
4. Confirmed by experiment rather than left as a plausible story. The two stubborn
   failures were re-run with a window being moved and recoloured at 60 Hz in front of
   them. Both passed, and `CrossAdapterWgcFramesConvertToUsableNv12` went from **5689 ms
   of timeouts to 1167 ms** — frames arriving immediately.

**Root cause.** These tests capture the **real desktop**, and Windows Graphics Capture
emits a frame only when the composited image changes. Their input is whatever happens to
be on screen.

**What is proven and what is not, stated separately.** Proven: guaranteeing screen
changes makes them pass, quickly and repeatably. Not proven: the exact condition that
makes them fail. The same batch that failed on Release passed 156/156 on Debug and
RelWithDebInfo minutes later on the same quiet desktop — so a static screen alone is not
sufficient to fail them. The failing Release run also took **759 s against 461 s** for the
same tier afterwards, which points at machine load as the second ingredient: under load,
WGC's already-sparse frame delivery slips past the tests' 500 ms acquire timeouts.
Recorded as a hypothesis, not a finding, because it was not tested.

Either way the defect is the same and does not depend on which trigger it is: the tests
read an input they do not control.

**And two of the five were not explained by it at all** —
`DdaCapturesOnTheAdapterThatOwnsTheOutput` failed with `DDA_UNSUPPORTED` and
`DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` failed in `start`; both are
`IDXGIOutput1::DuplicateOutput` refusing to open a duplication, which happens before any
frame is asked for and cannot depend on what is on screen. That gap was recorded here as
open, and it turned out to be the more important half.

---

### The second cause, found 2026-08-02: **Windows had turned the display off**

The ambient-content diagnosis was right about what it covered and did not cover
everything. The rest is power management, and it explains the residue exactly.

**How it surfaced.** A three-preset verification run, unattended:

| Preset | Wall clock | Result |
|---|---|---|
| Release | 17:57 → 18:04 | 159/159 |
| RelWithDebInfo | 18:04 → 18:14 | **151/159 — every one of the eight is a capture case** |
| Debug | 18:14 → | 159/159 |

The eight were the two `ScreenAnimatorTest` cases and all six capture cases. The
fixture's own numbers are the tell: **91 frames presented, 1 delivered.** The animator
painted perfectly and nothing was composited — which is not a content problem, and not
something occlusion or animation can fix.

**Measured:** this rig's AC display timeout is **600 s** (`powercfg`, `SUB_VIDEO
VIDEOIDLE`, `0x258`). A three-preset tier takes ~25 minutes. So an unattended run crosses
the timeout partway through, and from that point every test touching a real output fails.

**Why Debug then passed, which is what confirms it rather than muddies it:** a keystroke
arrived at ~18:13 — a human typing into the session, which resets the idle timer and
wakes the display. Debug's capture cases ran a minute later, with the display on, and
passed. Nothing about the *code* differed between the two presets.

**This accounts for every observation the first diagnosis had to leave open:**

- the two `DuplicateOutput` start failures — a powered-off output cannot be duplicated,
  and no amount of screen content changes that;
- why deliberate screen activity "fixed" it — moving and recolouring a window means
  someone is at the keyboard, which resets the display idle timer as a side effect;
- why it correlated with slow, heavily-loaded runs — those are the *long* ones, and
  length is what crosses a timeout;
- and the observation this entry recorded as "not proven": that the same binary passed
  156/156 twice earlier the same day and failed once. The failing run was the unattended
  one.

**Fix.** `gpu_test_main.cpp` holds `ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
ES_SYSTEM_REQUIRED` for the life of the process and clears it on the way out. It sits
next to the DPI-awareness call for the same reason that one is there: it has to be true
before any test body runs, and a fixture's `SetUp` is already too late for whatever ran
first. It is process-wide rather than inside `ScreenAnimator` because *every* GPU test
needs a live display, including the ones that do not present a pattern.

**Verified under the condition that produced the failure**, which is the only way this
one can be verified — the trigger is elapsed time without input, so the test is to stop
touching the machine and watch the clock:

| | Wall clock | Result |
|---|---|---|
| Build + CPU tier ×3 | 18:25 → 18:46 | 335/335 each |
| GPU Release | 18:46:08 → 18:53:01 | **159/159** |
| GPU RelWithDebInfo | 18:53:01 → 19:00:41 | **159/159** |
| GPU Debug | 19:00:41 → 19:11:07 | **159/159** |

Last keyboard input was **18:13**, so the display idle timer expired at ~18:23 — *before*
the first GPU test ran, and it stayed expired for the whole 29-minute unattended span.
The previous run, identical but for this change, failed 8 capture cases in the same
window. The animator's own numbers came back healthy throughout: 91 presented / 131
delivered, 240 of 240 frames carrying the pattern.

This is CLAUDE.md §5 — *"Tests use the synthetic source, never the real desktop. Tests
that depend on what happens to be on screen are not tests"* — in tests that predate this
session. BUG-030 was the same rule broken in `test_watchdog_recovery`, found a day
earlier and fixed there; this is the rest of the family.

**Fix.** The obvious workarounds are both wrong and were rejected: retrying makes a
static screen fail slower, and lengthening the deadline makes it fail later. These tests
also cannot move to `SyntheticSource` — their whole subject is WGC and DDA against a real
output, which a synthetic source bypasses.

`tests/fixtures/screen_animator.h` — **the test owns what is on the output**. A
`WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` popup covering the target monitor,
painted with eight colour bars and a marker block that steps 17 px and alternates between
two greys every tick. It does two separate jobs, and the distinction turned out to matter
more than expected:

- **It covers**, so ambient content cannot reach the capture. Assertions about pixels are
  then assertions about pixels the test drew.
- **It changes**, at a stated rate, so frame delivery is the test's property rather than
  the machine's.

`fps == 0` is the deliberate opposite: paint once and hold. That is what SPEC.md §4.3's
duplicate-frame case wants, and it makes "the desktop is idle" something the test
established rather than something it hoped for.

Two supporting pieces. `ScreenAnimator::start` fails with `CAPTURE_TARGET_NOT_FOUND`
unless `MonitorFromWindow` agrees the window landed on the monitor that was asked for — a
fixture pointed at the wrong output leaves a test reading ambient content while every
line of it claims otherwise, which is the original defect one level up. And
`pattern_signature` reads mean luma from the leftmost and rightmost sixteenth of a
captured NV12 frame's top third, where the bars are pure black and pure white; the
capture tests assert on it, so a frame that is *not* the fixture's is detectable rather
than merely unlikely.

**Two things the fix got wrong first. Both were caught by measurement, and the first
would otherwise have shipped as a placebo.**

1. **Blitting straight to the window DC does not change the screen.** The first version
   drew the dirty region onto the window's own DC. That updates the DWM redirection
   surface and never marks the window dirty, so DWM has no reason to composite:
   **60.1 Hz presented against 0.6 fps delivered**. The capture tests passed anyway —
   because the *occlusion* half worked, and ambient activity underneath was recompositing
   a window whose content happened to be ours. Every frame carried the pattern; none of
   them arrived because of us. The fix is the ordinary GDI path: `InvalidateRect` +
   `UpdateWindow`, blitting inside `BeginPaint`/`EndPaint`, because validating an update
   region is what tells DWM there is new content. After: **30.2 Hz presented, 37.5 fps
   delivered.**
2. **Occlusion controls content, not delivery.** Run with the pattern *static*, the
   capture still received **101 frames in 5.0 s (20.1 fps), 101 of 101 carrying the
   pattern, 0 repaints presented**. Activity in windows underneath still drives DWM to
   composite; the image it composites is just ours. So "100% of frames carried the
   pattern" does **not** mean "ambient contributed nothing" — and that reading was one
   inference away from being written down here as the verification.

**A measurement worth keeping, because it explains a number that recurs.** The ~48 fps
that keeps appearing in these tests is neither a coincidence nor a ceiling of ours: this
panel is variable-refresh, **48–144 Hz**, and DWM settles at the 48 Hz floor when only a
small region is updating. Presenting at 60 Hz therefore delivers ~48 fps — a ratio of
exactly 0.8, which is precisely where the delivery assertion's threshold had been put.
That case now presents at **30 Hz**, below any composition floor this project will meet,
so the ratio measures the fixture rather than the panel.

**Regression test.** `ScreenAnimatorTest` (gpu), three cases, which exist because the
capture tests' trust moved from "the desktop was busy" to "the fixture works":

| Case | Asserts | Measured |
|---|---|---|
| `AnAnimatedPatternDeliversFramesWithoutHelpFromTheDesktop` | `delivered >= 0.8 × presented`, at 30 Hz | 91 presented (30.2 Hz), 113 delivered (37.5 fps), 0 pool drops |
| `EveryDeliveredFrameCarriesThePatternRatherThanTheDesktop` | **every** frame, not a majority | 20 of 20; worst left luma 16.0 against a limit of 48, worst right 235.0 against 170 |
| `PresentingToAnOutputThatDoesNotExistIsRefused` | `CAPTURE_TARGET_NOT_FOUND`, rather than a silent landing elsewhere | pass |

Stated plainly, because it is the limit of what the pair proves: on a machine whose
desktop delivers 24 fps by itself, a fixture that had stopped painting could still clear
the delivery bar. The content case is the airtight half — it fails on a single frame of
anything else — and the two are read together.

In the cases themselves: `SustainedCaptureToNv12StaysCorrect` measures **240 frames in
5.0 s, 240 of 240 carrying the pattern**, and
`DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` measures **20 frames, 20 of them
duplicates, 0 repaints presented** — an output that is still because this process is
holding it still. The fixture went to all eight cases in the two files that acquire a
frame, not only the five that had failed; the other three had the same defect and had
merely not been unlucky yet.

**Tier results.** Six full GPU tiers, all 159 tests (156 + the three new ones), and all
eleven affected cases green in every one:

| Run | Result | Time |
|---|---|---|
| Release | 158/159 — BUG-034 | 416 s |
| RelWithDebInfo | **159/159** | 482 s |
| Debug | **159/159** | 670 s |
| Release, under 8-core load + disk churn | 158/159 — BUG-035 | 425 s |
| Release, confirmation | 158/159 — BUG-035 | 386 s |
| RelWithDebInfo / Debug, confirmation | **159/159** / **159/159** | 449 s / 635 s |

CPU tier 333/333 on all three presets; lint clean at 97 files. The two failures are
BUG-034 and BUG-035, both unrelated to this work and both filed.

**What could not be verified, and why.** The exit criterion asked for a pass on an idle
machine with no console output visible **and** under background load. The load half was
run and is in the table. The idle half could not be *arranged* — this machine's desktop
is never quiet while a session is driving it, measured at 0.6 to 20.1 fps of ambient
composition at different moments. What replaces it is that the tests no longer care:
delivery is asserted against what the fixture *presented* rather than against a rate, and
ambient composition can only add frames. That is an argument plus a measurement rather
than the run that was asked for, and it is stated as such.

**Lesson.** "It passed the last two runs" is evidence about the runs, not about the
tests. Three consecutive green tiers here meant only that someone had been watching them
scroll. A test whose input is the ambient environment reports the environment, and it
reports it as a verdict on the code.

Note also which way the diagnosis had to run: the *cheap* conclusion was "the machine was
busy, re-run it", and a re-run did turn three of the five green — which would have looked
like confirmation. The two that stayed red are what forced the real answer.

**And the lesson from the second cause: a diagnosis that explains most of the evidence is
the most dangerous kind.** "The tests read ambient content" was true, was demonstrable,
and fixed six of the eight symptoms — so it felt finished. The two it could not explain
were written down as open, which is the only reason the second cause was ever found:
the residue was on the page, and when eight tests failed at 18:05 and passed at 18:14
with identical binaries, there was something to attach it to. **Had the entry rounded
those two off as "did not recur", the tier would have kept failing at random and the
fixture would have taken the blame.**

Second: the failure had been in front of us three times wearing different clothes — a
quiet machine, a loaded machine, a cold cache — and each of those is a proxy for *time
since someone last touched the keyboard*. When a defect correlates with three unrelated
conditions, look for the thing they are all downstream of.

**The lesson from the first fix is separate, and sharper: a fixture needs its own test, and the
tests it serves are not that test.** The broken first version made all eight capture cases
pass, with 100% of frames carrying the pattern, while contributing essentially nothing to
frame delivery. None of the eight could tell the difference, because they assert on what
arrives and not on why it arrived. A case comparing *delivered* against *presented* found
it on the first run. When a fix works by making a test's input controlled, the thing that
then needs proving is the control — and proving it means measuring the fixture's effect,
not observing that the tests went green.

---

## [BUG-032] A rebuild looked four times over budget on Debug, and every millisecond of the overrun belonged to the test fixture

**Severity:** Minor (test correctness; no product defect)   **Found:** 2026-08-01   **Fixed:** 2026-08-01   **Commit:** _uncommitted_

**Symptom.** `WatchdogRecoveryTest.AnInjectedDeviceLossRebuildsTheSessionWithinTheBudget`
failed on `windows-msvc-debug` only: rebuild gap **777.9 ms** against SPEC.md §5.4's
350 ms budget, while Release measured 127.3 ms and RelWithDebInfo passed. The rebuild
itself worked — one rebuild, same file, 254 frames, file valid. Only the timing missed.

**Investigation.**

1. Reproducible, not a flake: five Debug runs gave 550, 580, 581, 585, 606 ms; five
   Release runs gave 156–160 ms. A consistent ~3.7× is not scheduler noise.
2. "Debug is slower" is a hypothesis, not a diagnosis, and CLAUDE.md §6 forbids
   relaxing an assertion that failed on hardware before the cause is known. `gap_ns`
   was a single number covering eight §5.4 steps, so it could not answer the question.
   `MigrationRecord` gained `teardown_ns`, `discovery_ns`, `device_ns`, `pipeline_ns`
   and `capture_ns`, reported in the migration log line and in the test's failure text.
3. The breakdown settled it immediately (ms):

   | phase | Debug | Release |
   |---|---|---|
   | teardown | 0.14–0.22 | 0.04–0.08 |
   | discovery | 3.5–5.0 | 3.7–4.1 |
   | device | 51.6–59.4 | 44.0–58.4 |
   | pipeline | 43.7–57.4 | 39.5–48.8 |
   | **capture** | **458–501** | **61–64** |

   Discovery, device creation and the pipeline rebuild are **the same in both builds**,
   as driver-dominated work should be. The entire difference is the capture restart.

**Root cause.** `SyntheticSource::start` pre-rendered eight 1080p BGRA patterns on the
CPU — about 16 million pixel writes — inside step 8's capture restart. That is fixture
cost, not product cost: WGC and DDA render nothing in `start`. The test was measuring
its own test pattern generator against a product budget, and an unoptimised build made
the generator the dominant term.

**Fix.** `PatternCache` memoises `render_bgra` process-wide — the pattern is a pure
function of (width, height, index, flash), so caching is sound — bounded to 16 entries
because they are 8 MB apiece at 1080p. The first `start` in a test binary pays the
render; every rebuild after it pays a texture create and a copy.

An intermediate attempt filled the slots **lazily**, on first `acquire`, which fixed the
budget and broke something else: the paints then landed inside the capture loop just
after a rebuild and read as a fresh stall, turning one fault into **two rebuilds** on
2 of 3 Release runs. Recorded because it is the same trap as BUG-029 — recovery work
tripping the detector that triggered it — and because "move the cost somewhere else" is
the obvious fix and the wrong one. The cost had to *stop existing per rebuild*, not
move.

**Regression test.** `WatchdogRecoveryTest.*`, with the phase breakdown printed on every
run so a future overrun is attributable on sight. Measured after the fix, 4 runs each:
Debug gap **107–133 ms**, Release **102–130 ms** — the two builds now agree, which is
the real evidence that what remains is driver time. Capture phase **9–18 ms** in both,
down from 458–501. `AHealthyRecordingIsNeverRebuilt` also improved to **0** stall
episodes on Debug (was 1), because the pre-render was stalling the first frames too.

**Lesson.** A budget assertion measures whatever sits inside the window, and a test
fixture inside that window is measured as if it were the product. Before relaxing a
timing assertion, break the interval down — the instrumentation cost twenty minutes and
turned "Debug is just slow, widen the budget" into "the fixture was 80% of the number,
and the product's phases were identical across builds". Widening the budget would have
hidden a real 350 ms of headroom the product actually has.

Corollary: when a number differs between an optimised and an unoptimised build, the
phases that *do not* differ are the informative ones. Discovery, device and pipeline
matching to within noise was what proved the product was innocent.

---

## [BUG-031] An audio buffer's frame size was read off shared state a migration rewrote, so a format change read past the end of a buffer

**Severity:** Critical   **Found:** 2026-08-01   **Fixed:** 2026-08-01   **Commit:** _uncommitted_

**Symptom.** `AudioPathTest.AnEndpointChangeKeepsTheTimelineContinuousAndTheOutputFormatConstant`
failed once in a 332-test Release CPU tier and passed on the immediate re-run. Run
alone it passed 6/6 with byte-identical measurements.

**Investigation.**

1. The ctest re-run was green, so there was no assertion text to read. An intermittent
   with no captured failure is not a result — the test was stressed instead: eight
   processes, `--gtest_repeat=25`.
2. It was **not** an assertion failure. `SEH exception with code 0xc0000005` — an access
   violation, in a test whose arithmetic is deterministic. That reclassified it from a
   flaky test to a defect in shipped code.
3. Stressing the whole `AudioPathTest` suite the same way produced 8 crashes, **all** in
   that one test. `MigratingToAnIdenticalFormatChangesNothingButTheCount` — same code
   path, same threads, same queue — never crashed. The only difference between them is
   that one migrates to a *different* format.
4. `AudioEncodePath::offer` runs on the audio thread and sized its copy out of WASAPI's
   buffer as `frames * impl_->input_bytes_per_frame`. `apply_migration` runs on the
   `aenc` thread and *writes* `input_bytes_per_frame`. A plain `int`, no
   synchronization.

**Root cause.** Two threads sharing one mutable notion of "the current input format",
for a quantity that is not a property of the pipeline at all — it is a property of the
bytes in a particular buffer.

The window is narrow and the failure is asymmetric. Migrating 48 kHz stereo float
(8 bytes/frame) → 44.1 kHz mono float (4), if `offer` reads the *new* size while the
buffer it is copying is still the *old* endpoint's, it allocates half what it needs and
`memcpy`s the full amount — a heap overflow. The reverse ordering over-allocates and
merely produces garbage audio. That is why only the narrowing test crashed, and why the
same-format test could not: with 8 → 8 there is nothing to disagree about.

**Fix.** The shared variable was **deleted**, not synchronized. `LoopbackBuffer` now
carries `bytes_per_frame`, filled by the capture from the format that stream negotiated,
and `AudioWorkItem` carries it onward through the queue. `offer` sizes the copy from the
buffer it was handed; `place` computes its trim offset from the item it is placing.
Nothing reads a format it did not receive alongside the bytes, so there is no window to
race in. A buffer arriving with no frame size is logged and filled as silence — the
timeline still advances, so the file keeps its length (CLAUDE.md §1).

**Regression test.**
`AudioPathTest.BuffersQueuedAcrossAMigrationAreReadAtTheSizeTheyWereWrittenAt` (cpu):
20 migrations alternating 2ch↔1ch across 240 buffers, delivered with no pause so the
`aenc` thread is behind the whole time and the window is open on every seam. Timeline
4.8 s against 4.8 s expected, 20 applied. Measured against the fix under the same
eight-process stress: **600 runs, 0 crashes**, where the old build crashed 8 times.

**Lesson.** An intermittent failure with no captured output is not a flake until it has
been shown to be one. Re-running until green would have shipped a heap overflow: the
re-run was green, the test is deterministic on its face, and every instinct said
"contention, move on". Stressing it cost fifteen minutes and changed the verdict from
"flaky test" to "critical defect".

Second: a value that two threads must agree on, describing data that flows between them,
belongs **on the data**. `input_bytes_per_frame` looked like configuration and was
actually a per-buffer property, and no amount of atomics or mutexes would have fixed it
— the correct value differs per item, so any single shared cell is wrong for some item
by construction. When a race appears around a "current settings" field, ask whether the
field should exist at all.

---

## [BUG-030] The row 9 test captured the real desktop, and the synthetic source that replaced it wedged the capture thread

**Severity:** Major   **Found:** 2026-08-01   **Fixed:** 2026-08-01   **Commit:** _uncommitted_

**Symptom.** `WatchdogRecoveryTest.AnInjectedDeviceLossRebuildsTheSessionWithinTheBudget`
passed in isolation and failed in the full RelWithDebInfo GPU tier, on §10.4's duration
check. Re-running it alone passed again.

**Investigation.**

1. The failure was a duration shortfall, not a rebuild failure — the migration itself was
   in budget. So the recording had collected fewer frames than the wall-clock elapsed.
2. The test built its capture with `fc::capture::primary_display()`. It was recording
   **the real desktop**, which CLAUDE.md §5 forbids for exactly this reason: WGC
   composites nothing when nothing on screen changes, so the frame count depends on
   whether another test's console output happened to repaint a window. In isolation the
   test's own output kept the desktop busy; behind twenty other tests it did not.
3. Replacing it with `SyntheticSource` — through a new `SessionSettings::capture_factory`
   seam, since `RecordingSession` opened its own capture — produced a *different* failure:
   45.7 s runtime and **zero** rebuilds, where the budget is 350 ms.
4. `RecordingSession`'s capture loop calls `acquire` in a tight loop and checks
   `rebuild_pending` between calls. WGC and DDA both **block** inside `acquire` until a
   frame exists, which is what paces that loop. `SyntheticSource` returns immediately, so
   the loop became an unbounded producer, filled the D3D command queue, and blocked inside
   `CopyResource` — where it stopped checking `rebuild_pending`. The injected device loss
   was never acted on, and `stop()` then sat out the full 30 s finalize deadline.

**Root cause.** Two, stacked. The test asserted on content it did not control, and the
fake it was replaced with did not reproduce the one property of the real backends the
code under test depends on: that `acquire` paces the caller.

**Fix.** `SessionSettings::capture_factory`, so a test can inject its capture without
`RecordingSession` reaching for the display. And `SyntheticSource::Settings::pace_to_real_time`,
off by default — every test that drives the pipeline directly wants frames on demand and
paces itself — which makes `acquire` wait until the frame is due against a wall-clock
origin taken at `start`.

**Regression test.** The three `WatchdogRecoveryTest` cases, now hermetic. Measured:
injected device loss rebuilds once in **127.3 ms** against a 350 ms budget, 247 frames,
one segment; rung 4 reaches the same outcome at **126.3 ms**; and the negative control
records 6 s and 360 frames with **zero** rebuilds.

**Lesson.** A fake has to reproduce the *properties the caller relies on*, not just the
interface. `SyntheticSource` was a correct `IScreenCapture` by every signature and still
broke the first caller that depended on blocking semantics — a dependency that was never
written down anywhere, because with the real backends it had never needed to be. When a
fake replaces something that blocks, ask what the blocking was doing for the caller.

Second, smaller: the original defect was a rule violation, not an oversight. CLAUDE.md §5
says tests use the synthetic source and never the real desktop, and the cost of ignoring
it was a test that passed for eight days while proving nothing about a fault it claimed
to cover.

---

## [BUG-029] The recovery from a device loss tripped the detector that triggered it, so one fault rebuilt the recording four times

**Severity:** Major   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

The first end-to-end test of `RecordingSession` injected **one**
`DXGI_ERROR_DEVICE_REMOVED` and got four rebuilds:

```
injected device loss -> session rebuild:
  cause device_lost, gap 236.091 ms (budget 350 ms), same_file 1
  captured 156 frames, 4 rebuild(s), 1 segment(s)
```

Three injections produced five. Each rebuild individually succeeded and stayed inside
§5.4's budget, so nothing looked broken except the count.

### Investigation

This took three attempts, and the first two were both real bugs hiding a third.

1. **The watcher reported its own answer back to itself.** Responding to §5.4 means
   creating a device, re-running discovery — which probes an encoder session per
   adapter — and opening a new encoder. All of that touches DXGI, and afterwards
   `IDXGIFactory1::IsCurrent()` on the watcher's factory reads false. The next poll
   called that a topology change and asked for another rebuild.
   **Fix:** `DeviceWatcher::acknowledge()` — re-baseline the factory and adapter set,
   and discard reports that arrived during the rebuild, because they describe the
   device stack that has just been replaced. The watcher's contract is "changes since
   you last acknowledged", not "changes since you started".
2. **Acknowledging at the end could not help a poll in the middle.** Still four
   rebuilds. `rebuild_pending` is cleared the moment the capture thread picks the
   request up, so the ~240 ms the rebuild actually takes looked idle to the watchdog,
   which polled straight into the churn.
   **Fix:** a separate `rebuilding` flag covering the whole operation, cleared only
   after the acknowledgement.
3. **The third attempt changed the reported cause, which is what gave it away.** The
   rebuilds kept coming but now as `capture_stalled`. Capture is deliberately stopped
   for those ~240 ms — and SPEC.md §20 row 9's detector fires at **3 × the frame
   interval**, 50 ms at 60 fps. So the recovery was itself a stall by the detector's own
   definition, and each rebuild triggered the next.

   The same run exposed a second, independent instance: the negative control
   (`AHealthyRecordingIsNeverRebuilt`) started failing with one rebuild and one stall
   episode on a *healthy* idle recording. WGC is change-driven — it produces a frame
   when the desktop composites — so a static screen legitimately goes far longer than
   50 ms without one.

### Root cause

Row 9's threshold is a **detection** threshold being used as an **action** threshold.
At 3 × the frame interval it correctly notices that no frame has arrived; it cannot
distinguish "capture is wedged" from "nothing has changed on screen" or from "we
stopped capture on purpose two hundred milliseconds ago", because from frame arrival
alone those are identical.

### Fix

The report stays at row 9's threshold; the *action* waits for a silence neither an idle
desktop nor a rebuild can explain. `PipelineHealth::current_stall_ns` exposes the live
duration alongside the episode count, and `RecordingSession` rebuilds only past
`kStallRebuildThresholdNs` (2 s) — longer than any measured rebuild (~240 ms), longer
than any plausible idle gap, still a fifth of the time a user takes to notice a freeze.

### Regression test

`WatchdogRecoveryTest` (gpu), all three cases. After the fix:

| Case | Rebuilds | Gap |
|---|---|---|
| One injected `DEVICE_REMOVED` | **1** | 242 ms |
| Three injected in a burst (rung 4) | **1** | 270 ms |
| Healthy 6 s idle recording | **0** (3 stall episodes *detected*) | — |

The counts are asserted as equalities, not upper bounds. The healthy case is the one
that matters most: it detects three stalls and acts on none, which is precisely the
distinction the fix draws.

### Lessons

- **A recovery action must not satisfy its own trigger.** Anything that stops the thing
  a watchdog watches has to suppress or outlast that watchdog, and the cheapest way to
  find out is to make the test assert *how many times* recovery ran rather than that it
  ran at all. Three separate bugs here were invisible to "did it recover?".
- **Detection thresholds and action thresholds are different numbers.** SPEC.md §20 row
  9 states one; using it for both is what made a healthy idle recording rebuild itself.
- **The negative control did the work.** `AHealthyRecordingIsNeverRebuilt` was written
  as a formality and caught a defect the positive case passed straight through.

---

## [BUG-028] Every recording on the NVIDIA adapter was Main profile, not the High @ 4.2 SPEC.md §9 requires

**Severity:** Major   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

None visible. Every file played, every test passed, and had M7 not needed to compare
parameter sets across adapters this would still be true.

Building the cross-adapter comparison row 11 needs (`test_migration_parameter_sets`)
printed both encoders' `extradata` as hex, and the two SPS did not start the same way:

```
h264_amf    28 bytes  000000016764042a...
                              ^^ profile_idc 0x64 = 100 = High
h264_nvenc  52 bytes  00000001674d402a...
                              ^^ profile_idc 0x4d =  77 = Main
```

Both were opened with `codec_->profile = AV_PROFILE_H264_HIGH`.

### Investigation

1. Level was right on both — `0x2a` = 42 — so the context's fields were not being
   ignored wholesale. Only the profile disagreed.
2. `libavcodec/nvenc_h264.c` line 60 declares a **private** `profile` option whose
   default is `NV_ENC_H264_PROFILE_MAIN`.
3. `libavcodec/nvenc.c` line 1357 is the part that makes this silent rather than
   merely wrong: `nvenc_setup_h264_config` switches on `ctx->profile` — the private
   option — and **writes** `avctx->profile` from it. The data flows out of the encoder
   into the context, not in. Setting `AVCodecContext::profile` is therefore not
   ignored so much as overwritten, and reading it back afterwards reports the value
   NVENC chose, which is why nothing downstream noticed.
4. `libavcodec/amfenc_h264.c` line 42 declares the same option with a default of `-1`,
   which means "derive from `avctx->profile`". Hence AMF was correct all along, and
   the defect was invisible on the adapter most of the suite happens to select.

### Root cause

An encoder-private option with a non-neutral default, on an encoder that treats its
private option as authoritative and the generic field as an output. `h264_nvenc` is
configured through private options for preset, tune, rate control and GOP behaviour
already; profile was the one setting assumed to work through the generic field.

The reason it survived from M3: nothing in the suite read `profile_idc`. The container
tests assert format name, stream count and codec ids; the colour tests assert the VUI;
the frame-identity tests decode pictures. A decoder happily decodes Main, so every
assertion passed on a file that did not meet §9.

### Fix

Set the private option explicitly, alongside the other NVENC private options:

```cpp
options.set("profile", "high");
```

Measured after: `profile_idc` 100 on both encoders.

### Regression test

`MigrationParameterSetTest.EveryEncoderProducesTheHighProfileSpecNineRequires` (gpu).
It parses `profile_idc` out of the SPS in `extradata` and asserts it is 100, **per
encoder** rather than by comparing the two — so a single-adapter rig still catches it,
and so the assertion means "meets §9" rather than "the two agree".

### Lessons

- **A generic field and a private option that mean the same thing are a bug waiting
  to happen, and the direction of data flow is the thing to check.** NVENC writes
  `avctx->profile`; reading it back to verify would have confirmed the wrong answer.
- **Assert the requirement, not a proxy for it.** §9 says "High @ 4.2" and the suite
  checked codec id, colour tags and decodability — three things that are all true of a
  Main-profile file. A requirement nothing reads is a requirement nothing enforces.
- **A test built for one purpose found a defect in another.** The parameter-set
  comparison exists for row 11's migration design; printing both blobs as hex, rather
  than only comparing them, is what made the profile visible at a glance. Cheap
  diagnostics in a test pay for themselves.

---

## [BUG-027] `test_slow_disk`'s backpressure came from a race between two build-dependent speeds, so it passed on one preset and failed on another

**Severity:** Minor (test-only)   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

`SlowDiskTest.AThrottledDiskDegradesTheRecordingInsteadOfFailingIt` passed on
`windows-msvc-release` (124 frames dropped, rung 3) and failed on
`windows-msvc-relwithdebinfo` — having measured a perfectly healthy throttle:

```
write p99 56880 us, rung 0 (nominal), transitions 0, retimes 0, drop ratio 0
Expected: (out.stats.frames_queue_dropped) > (0u), actual: 0 vs 0
```

The disk was demonstrably slow. Nothing was dropped.

### Investigation

1. The injection was working — a 56.9 ms P99 against an unthrottled baseline of 0.3 ms.
   So the failure was downstream of the stall, in whether the *queues* built.
2. The configuration was 50 ms on one write in four, i.e. the mux thread draining at
   **80 packets/s**, against a flooded feeder. Whether a backlog formed therefore
   depended entirely on whether the feeder could beat 80 fps.
3. It could on Release (the encoder sustains ~370 fps and `SyntheticSource::acquire`
   renders and uploads a 1080p BGRA pattern per call) and could not on RelWithDebInfo,
   where that per-frame render is slower. Two speeds, both build-dependent, and the test
   asserted on which one won.
4. The rung 6 case in the same file never failed, and the reason is instructive: it feeds
   at a **paced** 60 fps against a 600 ms-per-eight-packet drain, so its deficit is
   arithmetic rather than a race.

### Root cause

The case's backpressure was *tuned* until it reproduced on the machine in front of me,
not *derived* from rates that hold on any machine. CLAUDE.md §6 names this exact failure
mode — "a throughput threshold that fails on a busy machine is a test people learn to
ignore" — and the first version of this case was one.

### Fix

It took two attempts, and the second failure is the more instructive one.

**Attempt 1: paced 60 fps feed, 100 ms on one write in two** — a drain of 20 packets/s
against a nominal 60 fps feed. Green on Release and RelWithDebInfo, **failed on Debug**.
The feed rate is not a constant either: `SyntheticSource::acquire` renders a 1080p BGRA
pattern in unoptimised code, and on Debug the loop could not hold 60 fps, so the
absolute-deadline `sleep_until` returned immediately every time and the feeder ran at
roughly the drain rate. Measured: write P99 112,884 µs, 300 of 300 frames encoded, zero
shed. The same defect one level down — a rate ordering that was still a race, just
between a different pair of speeds.

**Attempt 2: 300 ms on one write in four** — a drain of **13 packets/s**, which is the
rate the rung 6 case had used from the start and the reason that case never failed on any
preset. 13 packets/s is below what the feeder achieves on the slowest configuration the
project builds, so the deficit is a property of the configuration rather than of the
machine. The stall stays under rung 6's 500 ms so the two cases remain about different
rungs, and that is now asserted rather than merely intended.

The assertions were also made structural rather than marginal. `frames_queue_dropped > 0`
alone left the case riding a 6-frame margin on Debug; it now asserts on the **union** of
the two ways the pipeline sheds work, because which mechanism dominates depends on how
quickly rung 3 retimes — after the retime a 60 fps source has half its frames paced out
*by design*.

### Regression test

The case itself, green on **all three presets**:

| Preset | Write P99 | Queue-dropped | Paced out | Rung | File |
|---|---|---|---|---|---|
| Release | 312 ms | 183 | 9 | 3 | valid, 195 frames |
| RelWithDebInfo | 307 ms | 180 | 11 | 3 | valid, 195 frames |
| Debug | 306 ms | 6 | 45 | 3 | valid, 253 frames |

The Debug row is why the union assertion exists: 6 queue drops would have been a coin
flip, 51 shed frames is not.

### The third attempt failed too, and that was the useful one

The 300 ms / one-in-four configuration passed on Release and RelWithDebInfo, and passed
on Debug in isolation, but failed twice on Debug inside a *full* tier run where the
machine is busier and the feeder slower still.

Three attempts narrowing the window without closing it was itself the finding: **the
variable being tuned was the wrong one.** Every configuration had tried to push the mux
thread's drain rate below the feeder's, while the feeder's rate was the term that would
not hold still — `SyntheticSource::acquire` rendered a 1080p BGRA pattern on the CPU and
did a row-by-row `Map` copy for *every frame*, roughly two million pixels touched twice,
which on an unoptimised build under load falls far short of a 16.67 ms budget.

### The actual fix

`SyntheticSource::Settings::prerendered_frames` renders a small cycle of frames once at
`start` and hands out cached textures thereafter, so `acquire` becomes an `AddRef` and a
timestamp. The barcode then repeats every N frames, so tests that assert frame
*identity* (§20 row 5) leave it at 0; `CaptureFrame::sequence` and the timestamps stay
strictly monotonic either way.

Measured on **Debug**, the configuration that had been failing:

| | Before pre-render | After |
|---|---|---|
| `test_slow_disk` degradation case | 6 queue-dropped, 45 paced out | **183 queue-dropped**, 9 paced out |
| Same case on Release | 183 queue-dropped | 183 queue-dropped |
| `test_sustained_60fps` feed rate | 57.2 fps | **60.02 fps** |

Debug and Release now produce the same numbers, which is the property that was missing.
The unthrottled control had to be paced as well: it had been flooding, which was only
ever harmless because the feeder was too slow to outrun the encoder, and once the frames
were cached the flood became real and dropped 291 of 300 frames — correct engine
behaviour, useless baseline.

### Regression test

The case itself, green on **all three presets** with matching numbers. Row 6's
`test_sustained_60fps` also benefits: its ten-minute run had fed at 57.2 fps and taken
629 s to deliver 600 s of frames, understating the load the row asks for.

### Lessons

- **Derive the load, do not tune it.** The question to ask of a backpressure test is
  "what two rates am I relying on, and is their ordering guaranteed on the slowest
  configuration we build?" — not "does it go red when I make the number bigger." Two
  attempts here failed that question before the third passed it — and the third still
  failed under full-tier load, because the question was being asked of the wrong rate.
- **After the second failed tuning attempt, change the variable.** The third attempt
  reasoned carefully about drain rates and still lost, because the feeder was the term
  that moved. A test whose precondition is an inequality between two measured rates
  should make one of them a constant, not chase the gap.
- **Run the other build configurations.** This was only visible because the GPU tier was
  run on RelWithDebInfo and Debug as well as Release; a single-preset run would have
  shipped a test that silently stopped testing whenever the harness got slower.
- **Assert on the invariant, not on the symptom that happened to be biggest.** Queue
  drops and paced-out frames are two faces of the same deliberate shedding, and picking
  one made the test fragile in a way that had nothing to do with what it was checking.
- A test that measures the right thing (P99 56.9 ms) and asserts on a consequence that
  does not follow from it is worse than one that fails outright, because the measurement
  makes it look sound.

---

## [BUG-026] A slow disk made `stop` abandon the venc thread, so SPEC.md §13's gentlest rung produced an unfinalized file

**Severity:** Critical   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

`test_slow_disk`'s rung 6 case — a volume stalled 600 ms per write, which is the
threshold's own scale — recorded 300 frames and then failed finalization outright:

```
SlowDiskTest.ADiskPastRungSixsLatencyThresholdIsReportedAsDiskPressure
the recording failed rather than degrading: INTERNAL_THREAD_JOIN_TIMEOUT
file: valid 0, 0 frames decoded, video 0 s
```

The ladder had done its job perfectly — write P99 609 ms, rung 6 `disk_pressure`
reported, 182 frames deliberately dropped — and the output was a zero-frame file.

### Investigation

1. Assumed the `mux` thread, since it is the one holding the disk, and raised its join
   deadline. No change: still `INTERNAL_THREAD_JOIN_TIMEOUT`, still 7 s total, where a
   30 s deadline would have shown as a much longer run.
2. The deadline that fired was the **venc** thread's, one step earlier in `stop`. That
   was the surprise, because SPEC.md §12 lists `venc` as a thread that never blocks.
3. It does block, and by design. The mux queue is `Block` policy because §12 forbids
   ever dropping audio — and an encoded *video* packet cannot be dropped either, since
   discarding a P-frame breaks every frame that references it. So a full mux queue
   blocks the venc thread, and on a slow volume it stays blocked. §12's "on audio queue
   pressure, degrade video instead" is satisfied upstream, at the drop-oldest *encode*
   queue, which is exactly where those 182 frames went.
4. The consequence: `await_worker` detached venc after 2 s and `stop` returned early —
   before `encoder->flush()`, before the mux queue drained, before `av_write_trailer`.

### Root cause

SPEC.md §12 mandates a 2 s bounded join for every thread. That is correct for a thread
forbidden to block, and wrong for the two threads whose *shutdown* legitimately waits
on the disk: `mux` directly, and `venc` transitively through the never-drop mux queue.

SPEC.md §15.1 already says finalization is different — "Requests time out at 5 s (except
`stop_record`, **30 s**, since finalization is legitimately slow)" — so the two sections
were in tension and the code had implemented only one of them.

### Fix

`await_worker` takes a timeout, defaulting to §12's 2 s. `kFinalizeJoinTimeout` is
§15.1's 30 s, used for the `venc` and `mux` joins in `VideoPipeline::stop`. The
`watchdog` keeps 2 s: it is a pure observer waiting on a 250 ms condition variable, and
one that has not noticed a shutdown flag in 2 s really is wedged.

Still bounded — a thread that has not finished in 30 s escalates and logs exactly as
before, and for MKV the file remains playable to its last complete cluster (§10.2). The
change is which number, not whether there is one.

### Regression test

`SlowDiskTest.ADiskPastRungSixsLatencyThresholdIsReportedAsDiskPressure` (gpu). After
the fix: write P99 609,798 µs against rung 6's 500,000 µs threshold, rung 6 reported,
182 of 300 frames dropped, **file valid with 178 decodable frames**. It asserts both
that the rung fired and that `stop` returned a report rather than an error.

### Lessons

- **A join deadline is a policy about one thread's obligations, not a global constant.**
  Applying the strictest thread's number to every thread cost the file in the one
  scenario the number was supposed to protect.
- **The ladder working correctly is what exposed this.** Rungs 1 through 6 all fired as
  designed and the outcome was still the failure rung 7 exists to prevent. Degradation
  logic needs a test that checks the *file*, not only the rung.
- Two sections of a spec disagreeing is a finding to reconcile in code and write down,
  not a detail to pick a side on silently.

---

## [BUG-025] Rung 3's retime made the validation gate reject the recording it had just saved

**Severity:** Major   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

The first recording in which SPEC.md §13 rung 3 actually fired failed its §10.4
validation gate:

```
the video track is 4.990652 s against an expected 3.516667 s, a difference of more than 1%
```

The file was fine. 4.99 s of continuous, gapless, correctly-timed video, and the gate
called it 30% short.

### Investigation

1. `3.516667 × 60 = 211` exactly, so the expectation came from
   `pacer.emitted() == 211` divided by the configured 60 fps.
2. The pacer had been retimed to 30 fps partway through by the ladder. Its 211 emitted
   frames were therefore a stretch on the 60 fps grid followed by a stretch on the 30 fps
   grid, spanning a timeline of 4.99 s.
3. `211 / 60 = 3.52`. `211 / 30 = 7.03`. Neither is 4.99 — **no single rate divides the
   frame count into the timeline's length once the rate has changed.**

### Root cause

`VideoPipeline::stop` computed the expected duration as
`pacer.emitted() / settings.video.fps`. That identity holds only while the frame rate is
constant, and SPEC.md §13 rung 3 exists specifically to change it. The gate was being
handed an expectation that its own degradation ladder had invalidated.

### Fix

`timing::Pacer` records the last PTS it actually emitted and exposes
`timeline_seconds()`, computed from that plus one frame at the current rate. PTS is
absolute and survives every rate change untouched, which a frame count cannot.
`stop` uses it.

`retime` now rescales `last_index_` from the recorded PTS rather than re-deriving it as
`last_index_ * ticks_per_frame(fps_)`. The two are equal, but only one stays equal after
a *second* retime, and re-deriving a value already stored exactly is how truncation
error gets a foothold.

### Regression test

`FramePacer.TheTimelineLengthSurvivesARateChangeThatFrameCountsCannotDescribe` (cpu):
one second at 60 fps then one at 30 emits 91 frames over a 122000/60000 s timeline, and
the test asserts that *neither* rate divides the count into the length — so the old form
cannot be reinstated silently. `TheTimelineLengthIsExactAcrossManyRateChanges` alternates
rates ten times and holds the error under one frame, which is where truncation would
compound. End to end, `CfrExactnessTest.DroppedFramesCostContentButNeverSlots` (gpu) is
the case that found it.

### Lessons

- **A derived quantity outlives the assumption it was derived under.**
  `emitted() / fps` was correct for three milestones and became wrong the moment a
  feature designed to change `fps` was switched on. The clue was there in the field name
  the divisor came from: `settings.video.fps` is the *configured* rate, and nothing
  guaranteed the pacer was still on it.
- **A validation gate can be the thing that fails.** The prime directive says a failure
  anywhere must still produce a valid file; this was a *valid file* reported as a
  failure, which is the same promise broken from the other end.
- Two counters that agree today because a third value is constant should be one
  counter, or should carry a note saying which one is authoritative.

---

## [BUG-024] SPEC.md §13 rungs 1 and 2 name actions no hardware encoder in this build can perform

**Severity:** Major   **Found:** 2026-07-30   **Fixed:** _open — see Status_   **Commit:** _uncommitted_

### Status

**Not fixable in the engine.** This is a spec-versus-reality finding, not a defect in
FrameCapture, and it needs the owner's decision on how §13 should read. The ladder
computes both decisions, logs them, and is tested on them; what does not exist is a way
to carry them out. Written up because it is understood, per the log convention, and
because the consequence is a real quality regression against §13's intent.

### Symptom

Not a runtime failure — an absence. While wiring the ladder into `VideoPipeline` there
was no libavcodec call that would perform either of §13's first two actions:

| Rung | §13's action |
|---|---|
| 1 | Lower encoder preset one step (`p5` → `p4`) |
| 2 | Drop video quality target (CQP 20 → 24) |

### Investigation

Read the FFmpeg n8.1.2 sources this build links, rather than guessing from the public
API:

1. **`libavcodec/nvenc.c`** has `reconfig_encoder()`, called before every frame. It
   reconfigures `averageBitRate`, `maxBitRate` and `vbvBufferSize` — and the whole block
   is gated on `ctx->rc != NV_ENC_PARAMS_RC_CONSTQP`. QP is written once into
   `encode_config.rcParams.constQP` at init and never revisited. Preset lives in
   `init_encode_params` and is not touched. SPEC.md §9 makes CQP the default, so even
   the bitrate path is unreachable as the engine configures it.
2. **`libavcodec/amfenc.c`** assigns every property in `amf_encode_init`. There is no
   per-frame property write at all.
3. **`libavcodec/libx264.c`** has `reconfig_encoder()` comparing
   `params.rc.i_qp_constant` against the private `qp` option and calling
   `x264_encoder_reconfig` when they differ. A runtime QP change **works** here.
4. Neither encoder advertises `AV_CODEC_CAP_PARAM_CHANGE`.

### Root cause

§13's rungs 1 and 2 were specified against what encoders can do in principle. Through
libavcodec — which SPEC.md §2.2 item 1 requires the engine go through, forbidding direct
vendor SDK calls in `fc_core` — a preset is baked in at `avcodec_open2` and QP is
reconfigurable on libx264 only.

Reopening the encoder mid-recording is not an available workaround: it emits fresh
SPS/PPS, and the container's `avcC`/`CodecPrivate` was fixed by
`avformat_write_header` (§10.1). A parameter-set change the container never declares is
how a file plays correctly for its first GOP and then falls apart.

### What was built instead

The decision and the action are separated, so the gap is visible rather than silent:

- `health::Monitor` computes `Plan::preset_step` and `Plan::target_cqp` on §13's
  triggers, and is unit-tested on both.
- `IVideoEncoder::try_set_cqp` returns whether the encoder could honour it. `libx264`
  returns true; the hardware encoders return false and say why in the header.
- The pipeline logs rung 1's unavailability once per recording, and rung 2's at `TRACE`.

**The consequence, stated plainly for the owner:** on hardware, degradation now steps
from nominal straight to rung 3 — halving the capture rate to 30 fps — with no gentler
intermediate. §13's two soft rungs were the ones that would have absorbed a transient
without a visible change, and they are not available. That is worse than the spec
intends and it is a deliberate, documented gap rather than a silent one.

### Regression test

`SoftwareEncoderTest.TheSoftwareEncoderAcceptsARuntimeQualityChangeAndTheHardwareOnesDoNot`
(gpu). It asserts libx264 accepts a runtime QP change and that every hardware encoder
this rig reports refuses one — deliberately written so that a future FFmpeg gaining the
capability makes it **fail**, forcing the expectation to be inverted on purpose rather
than quietly relaxed. `HealthMonitor.*` covers the decisions regardless.

### Lessons

- **Read the dependency's source before designing against its capabilities.** Ten
  minutes in `nvenc.c` replaced a plausible assumption with a fact, and the fact changed
  the design.
- **Separating "what should happen" from "what could happen" is what makes an
  unimplementable requirement visible.** Had the ladder skipped rungs 1 and 2, the gap
  would have been invisible in the code and absent from the log.
- A spec written against hardware capability needs re-checking against the abstraction
  layer the spec itself mandates.

---

## [BUG-021] MP4 cannot declare the AAC encoder delay, so every MP4 recording's audio starts 21 ms late

**Severity:** Major   **Found:** 2026-07-30   **Fixed:** _open — see Status_   **Commit:** _uncommitted_

### Status

**Not fixed.** The symptom is bounded, measured and pinned by a test; two attempts
at a fix are recorded below with why each was rejected. This entry is written
because the bug is *understood*, which is when the log convention says to write
one — not when it is closed.

### Symptom

Extending SPEC.md §20 row 4 to cover MP4 (previously MKV only) failed on exactly
one mark of twelve:

```
AvSyncTest.TheBeepAndTheFlashStayInSyncThroughMp4
mark 0 in MP4: audio is 21.145833333333332 ms from video
```

Marks 1 through 11 were within 200 µs. 1024 / 48000 = 21.33 ms is AAC's priming.

### Investigation

1. Compared the two containers on the same recording, which made the cause
   immediate:

   | | MKV | MP4 |
   |---|---|---|
   | `initial_padding` read back from the file | **1024** | **0** |
   | first beep decodes at | 0 µs | **21,145 µs** |
   | first flash decodes at | 0 µs | 0 µs |

2. Matroska carries the encoder delay in `CodecDelay`, written by matroskaenc from
   `codecpar->initial_padding`; the demuxer applies it and the decoder discards the
   priming, so content begins at zero. MP4's equivalent is an **edit list**
   (`elst`), and a fragmented MP4 cannot write one: `empty_moov` emits the header
   before any duration is known, which is the whole point of it (SPEC.md §10.3).
   So the delay is absent from the file, and `remux_to_progressive`'s
   `avcodec_parameters_copy` faithfully copied its absence.
3. The defect is confined to the head. Priming occupies the first 1024 samples of
   the track, so only a beep at the epoch itself is displaced; every later one is
   correctly timed. That is why the steady-state offset through MP4 measures
   −187 µs — identical to MKV — once mark 0 is set aside.

### Attempted fixes, and why they were rejected

- **Set `codecpar->initial_padding` on the remux output.** No effect. movenc does
  not read that field; it derives the edit list from *packet timestamps*. Kept
  anyway, because it is the correct metadata and costs nothing.
- **Shift the audio packets back by the padding**, reproducing the negative first
  PTS a transcode produces, with `avoid_negative_ts` disabled so libavformat would
  not normalise it away. This *did* fix mark 0 — and moved every other mark to
  **−21.5 ms**. Without a reader that trims on the negative start, shifting the
  timestamps just slides the whole track early. Trading one bad mark for twelve is
  worse, so it was reverted.

- **Retried 2026-08-02, with the obvious omission ruled out.** The natural reading
  of the attempt above is that it never wrote an edit list because `use_editlist`
  defaults to "auto" and auto turns it *off* for fragmented output. So it was tried
  again with `use_editlist=1` set explicitly, `avoid_negative_ts` set to
  `AVFMT_AVOID_NEG_TS_DISABLED` on the output context, `initial_padding` carried
  across, and the audio shifted back by the priming rescaled into the source's
  timebase.

  **Both settings were confirmed to have taken effect** — unconsumed options after
  `avformat_write_header` came back empty, so movenc accepted `use_editlist`, and
  `avoid_negative_ts` read back as 0 (disabled). The result was **identical to the
  first attempt**: mark 0 at 0 ms, marks 1–19 at −21.52 ms, a uniform slide of the
  whole track with no trim anywhere. Reverted again.

  The write side was then read rather than assumed. `mov_write_edts_tag` in the
  FFmpeg n8.1.2 sources this build links *does* contain the right logic for a
  negative start — `start_ct = -FFMIN(start_dts, 0)`, which is exactly the media
  time that would trim the priming. So the failure is not "movenc cannot express
  this". Something between writing that box and the decoded samples is not applying
  it, and the next attempt should start by dumping the `elst` of the produced file
  rather than by trying a third variation on the shift.

**What is now known:** the fix is not a remux tweak, and three attempts at making it
one have produced the same wrong answer. The remaining candidate is `delay_moov`
instead of `empty_moov` during recording, which would let the delay be declared from
the start — and which trades away exactly the crash-safety guarantee SPEC.md §10.3
mandates `empty_moov` for. **That is a spec trade, not an implementation choice, and
it is the owner's.**

### Regression test

`AvSyncTest.TheBeepAndTheFlashStayInSyncThroughMp4` (gpu). It asserts the steady
state — marks 1..N within row 4's 20 ms, measured at **−187 µs** — and **pins the
head artefact between 15 and 25 ms**, so the test fails if the defect is ever fixed
(telling whoever fixed it to delete the exclusion) and equally if it grows past one
AAC frame. A widened tolerance would have hidden it; an excluded mark with no
assertion would have forgotten it.

### Lessons

1. **A container is not a detail the layer above can ignore.** Everything upstream
   of the muxer was identical between the two runs. The difference is entirely in
   what each container is *able to say*, and MP4 cannot say this thing in the mode
   §10.3 requires it to be written in.
2. **Extending coverage to a second configuration found a real defect immediately.**
   Row 4 and row 17 had covered MKV only because MKV was the only container when
   they were written. MP4 became real one milestone ago and nothing re-checked
   them; the gap was worth closing on the first attempt.
3. **Three "fixes" produced byte-identical results before one of them ran.** Every
   attempt was gated on a parameter that was being computed and then not passed,
   because an edit to the call site silently failed to apply after clang-format
   reflowed it. Identical output across substantively different changes is not a
   stubborn bug — it is evidence the code under test never executed, and it should
   have been the first hypothesis rather than the fourth.

---

## [BUG-020] Matroska's crash-safety guarantee was false: the clusters never reached the disk

**Severity:** Critical   **Found:** 2026-07-30   **Fixed:** 2026-07-30   **Commit:** _uncommitted_

### Symptom

`test_crash_recovery`, first run. The MP4 cases passed. The Matroska case did not
open at all:

```
CrashRecoveryTest.AKilledMkvRecordingIsPlayableWithoutRepair
[matroska,webm] EBML header parsing failed
the killed Matroska does not open: avformat_open_input failed
```

SPEC.md §10.2 says a truncated Matroska "is still playable up to the last complete
cluster". This one was not playable at all.

### Investigation

1. Ran the recorder by hand against an MKV, killed it after ten seconds, and looked
   at the file rather than at the error. **Zero bytes.** Not a damaged header — no
   header, no clusters, nothing. The engine's own log next to it was also zero
   bytes, which ruled out "the recording never started" and pointed at buffering
   rather than at Matroska.
2. That reframed the question from "is the file valid" to "when does anything reach
   the disk at all", so the next run watched the file size instead of guessing:

   | elapsed | file size |
   |---|---|
   | 10 s | 0 bytes |
   | 20 s | 0 bytes |
   | 30 s | 262,144 bytes |
   | 45 s | 524,288 bytes |

   Exact multiples of 256 KB. libavformat's output buffer is 256 KB here, not the
   32 KB assumed — and at the bitrate a mostly-static screen produces, 256 KB is
   about twenty-five seconds of recording. Every kill before the first flush leaves
   an empty file.
3. So §10.2's guarantee, and §10.3's, are claims about *structure*: clusters and
   fragments bound what a crash costs **given that the bytes are on disk**. Neither
   spec section says anything about getting them there, and nothing in the
   implementation did either.
4. MP4 passed only because the MP4 path had already grown an `avio_flush` on
   keyframes while this milestone's fragmented recipe was being written. That flush
   was added for MP4 alone, on the reasoning that §10.3 was the section demanding
   it. The reasoning was too narrow by exactly one container.

### Root cause

A durability guarantee stated in terms of file structure and implemented as file
structure, with no step that makes the structure durable. The cluster boundary
exists in the byte stream; the byte stream was in userspace.

Worth being precise about how close this came to shipping: the MKV path is the
**default container** (SPEC.md §10.2 calls it recommended), it had been through M3
and M4 with a full green suite, and every one of those tests closed the file
normally. Nothing that stops cleanly can see this. It took the first test that
killed a process, and that test only exists because §20 row 3 names it.

### Fix

The `avio_flush` on the keyframe that opens each fragment or cluster now runs for
**both** containers rather than for MP4 alone. That bounds the loss to one GOP,
which SPEC.md §9 fixes at two seconds, which is exactly the allowance §20 row 3
gives ("duration ≥ 28 s" of a 30 s recording). Once every two seconds costs nothing
against the mux thread's budget, and the mux thread is the one SPEC.md §12 permits
to block on disk.

The measured buffer size is now written down in the comment beside the flush, since
"32 KB" was assumed for hours and was wrong by 8×.

### Regression test

`CrashRecoveryTest.AKilledMkvRecordingIsPlayableWithoutRepair` and
`CrashRecoveryTest.AKilledMp4RecordingIsPlayableWhereItStopped` (gpu tier). Both
kill a real `fc_crash_recorder` process with `TerminateProcess` and decode what is
left. At SPEC.md row 3's stated 30 seconds: **28.004 s decoded, 1.995 s lost —
one GOP**, against an allowance of exactly one GOP.

### Lessons

1. **A durability property needs a durability step.** "Truncated files are playable
   because the format is chunked" is a statement about the format. Whether the
   chunks are on the disk is a statement about the program, and the two were
   conflated in the spec and then in the code.
2. **Clean-stop tests cannot see this class of bug, and every test was a clean-stop
   test.** M3 and M4 had a fully green suite over a default container that produced
   nothing at all under a kill. The only test that could see it is one that kills a
   process, which is why row 3 specifies the mechanism (`TerminateProcess`) and not
   just the outcome.
3. **Scoping a fix to the section that motivated it.** The flush was written while
   reading §10.3, so it was applied to §10.3's container. §10.2 makes the identical
   promise about the identical mechanism one section earlier. When a fix is a
   *mechanism*, the question is which promises depend on it — not which section was
   open at the time.
4. **Measure the buffer, do not assume it.** Two hypotheses were argued from an
   assumed 32 KB buffer and neither fit the evidence. Watching the file size for
   forty-five seconds answered it in one run and gave a number worth writing down.

---

## [BUG-019] The synthetic source converted QPC to nanoseconds by a multiply that overflows after fifteen minutes of uptime

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

`test_loopback_recording` — the first test to record with a real WASAPI endpoint
rather than through the synthetic `External` seam — produced a file with twelve
seconds of audio and **one frame** of video:

```
submitted=720 encoded=1 awaiting_epoch=1 queue_dropped=0 paced_out=718
```

720 frames went in. 718 were discarded by the pacer as "the source outran the
grid", which is what it does when several frames quantize to a grid slot already
emitted.

### Investigation

1. 718 frames spanning twelve seconds cannot all belong to one 1/60 s slot unless
   they all quantize to the *same* index — and `Pacer::quantize_index` clamps any
   timestamp before `t0` to index 0. So every video frame was, as far as the pacer
   was concerned, before the epoch.
2. `awaiting_epoch=1` says the epoch resolved on the second frame, so the audio
   callback had fired almost immediately. The epoch is `max(video_first,
   audio_first)`, so `t0` had been taken from the audio side and was more than
   twelve seconds ahead of every video timestamp.
3. Both sides claim to be QPC in nanoseconds. `LoopbackCapture` converts WASAPI's
   `u64QPCPosition`, documented in 100 ns units, by multiplying by 100. The
   synthetic source did this:

   ```cpp
   impl_->epoch_ns = (counter.QuadPart * 1'000'000'000LL) / frequency.QuadPart;
   ```

4. Measured on the rig: `QueryPerformanceFrequency` is 10 MHz and the counter was
   4.7e11. `counter * 1'000'000'000` is 4.7e20, against a signed 64-bit ceiling of
   9.22e18 — an overflow by a factor of fifty. The threshold is a counter of
   9.22e9, which at 10 MHz is **about fifteen minutes of uptime**. The machine had
   been up thirteen hours.
5. The engine's own capture backends were already correct: both `wgc_capture.cpp`
   and `dda_capture.cpp` split the counter into whole seconds and a remainder
   before scaling, with a comment saying why. There were four copies of this
   conversion and one of them — the one in test code — was wrong.

### Root cause

The naive conversion, in the one copy that had no reviewer looking for the
overflow. What made it survive is the shape of its failure: the wrapped value is
*consistent*. Every frame gets `wrapped_epoch + index * 16.67 ms`, so any test
measuring video against itself — frame counts, PTS spacing, CFR exactness,
duration, barcode identity — sees a perfectly well-formed timeline and passes.
M2's and M3's whole suites are that shape.

It only breaks when a second, independently derived clock has to agree with it,
and M4 is the first milestone that has one.

### Fix

`synthetic_source.cpp` now calls `timing::qpc_now_ns()`. The two engine copies were
folded onto the same helper at the same time — not because they were wrong, but
because four copies of a conversion with one silent-failure mode is the condition
that produced this, and consolidating removes it rather than fixing an instance.

`qpc_clock.h`'s own header comment was also corrected: it claimed the overflow
threshold was "about 90 seconds at a 10 MHz timer", which is out by a factor of
ten. Wrong for the right reason is still wrong in a comment whose entire job is to
stop someone reintroducing the bug.

### Regression test

`LoopbackRecordingTest.ASilentDesktopStillProducesAFullLengthAudioTrack` (gpu
tier), which found it. It is the only test that puts a real WASAPI timestamp and a
synthetic-source timestamp on the same timeline, and that is exactly the
configuration the defect needs. `encoded` went from 1 to 703 of 720 submitted.

No unit test is possible: the overflow depends on the machine's uptime, so the
same code passes on a freshly booted machine and fails on one that has been up
half an hour. Naming that here is the substitute — a CI runner that reboots per
job would never have shown this.

### Lessons

1. **A consistent wrong clock is invisible to every self-relative measurement.**
   Every M2 and M3 timing assertion is video against video, and all of them pass
   against a garbage epoch. The first assertion that could see it is the first one
   that crosses two clocks.
2. **Four copies of a conversion is three too many, and the wrong one will be the
   one nobody reviews.** The engine's copies had the overflow comment; the test
   tooling's copy was written later, by someone reading the same API and not the
   same comment. `qpc_clock.h` had already been introduced this milestone and the
   existing copies were deliberately left alone as "correct, low value to touch" —
   which was the wrong call for the reason above.
3. **"Uptime-dependent" is a category of bug worth naming.** It cannot be
   reproduced on demand, it cannot be unit-tested, and it gets *more* likely the
   longer a machine has been doing real work — so a developer's machine shows it
   and a fresh CI runner does not.

---

## [BUG-018] The test decoder reserved exactly the capacity it needed on every frame, making a long recording's verification quadratic

**Severity:** Minor (test tooling)   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

`test_av_sync` run at `FC_AV_SYNC_SECONDS=1800` — M4's 30-minute exit criterion —
had not finished after **70 minutes**. CPU time tracked wall time almost exactly,
so it was computing rather than deadlocked, and the working set was climbing
steadily.

The same test at 65 seconds finished in 22 s.

### Investigation

1. The encode phase was over: the output file had stopped growing at 27 MB and the
   process had moved on to `decode_media`. So the cost was in reading the file
   back, not in producing it.
2. 108,000 frames of 720p in 70 minutes is 39 ms per frame. H.264 decode of a 720p
   frame is one to two milliseconds, and the per-frame work on top of it — a mean
   over 921,600 luma samples and a 24-cell barcode read — is a fraction of that.
   The arithmetic did not come close to explaining the time, which meant the cost
   was not per frame at all.
3. Working set at 909 MB against an expected 691 MB of decoded audio was the tell:
   the audio side, not the video side.
4. `collect_audio_frame`:

   ```cpp
   plane.reserve(plane.size() + static_cast<std::size_t>(frame->nb_samples));
   ```

   `reserve` with *exactly* the new size, on every 1024-sample AAC frame. That is
   a request for precisely the capacity needed right now, which overrides the
   geometric growth `push_back` would otherwise apply — so every frame reallocated
   the whole accumulated vector and copied it. 84,375 frames per channel with a
   final length of 86.4M floats works out to roughly **14 TB copied per channel**.
5. The reason it was invisible at 65 s is the same arithmetic: 3,046 frames gives
   ~19 GB of copying, a couple of seconds hidden inside a 22-second test. The
   defect scales as the square of the recording length, so the case that would show
   it is exactly the case nobody had run.

### Root cause

`reserve` used as though it were a growth hint. It is not: it is an exact demand,
and calling it before each append converts amortized O(1) into O(n) per append.
The line reads as an optimisation and is the opposite of one.

### Fix

Removed. `push_back`'s own geometric growth is correct here and needs no help.

The per-frame luma buffer was fixed alongside it, for the same class of reason
rather than the same bug: `collect_video_frame` allocated and zeroed a fresh
~1 MB `std::vector` per frame, which over a half-hour recording is 100 GB of
allocation for a buffer whose size never changes. It is now a scratch buffer owned
by `decode_media` and reused.

### Regression test

None, and this is a deliberate gap worth naming. The defect is a *performance*
property of test tooling, and a threshold assertion on decode time would be
flaky on a loaded machine and would not have caught it at any duration the suite
routinely runs. What replaces it is the measurement: 300 seconds of media now
verifies in 74 s where the trend line predicted ~500 s, and the 30-minute run
completes in single-digit minutes. Those numbers are recorded here so the next
person who sees the decode slow down has something to compare against.

### Lessons

1. **`reserve` inside an append loop is almost always a defect.** It looks like
   care and it removes the growth strategy that makes appending cheap. The correct
   uses are once, up front, with the final size — or not at all.
2. **A cost that scales quadratically is invisible until it is catastrophic.** At
   65 seconds it was 3% of the runtime. At 30 minutes it was 100% of a 70-minute
   runtime. There is no duration in between at which it looks like a mild problem
   worth investigating.
3. **"The same test with a bigger number" is not automatically a scalability
   claim.** The changelog said `FC_AV_SYNC_SECONDS` let this test serve as SPEC.md
   §20.1's four-hour soak form. It could not have: even fixed, the decoder holds
   every sample in memory, which is 5.5 GB at four hours. That claim has been
   corrected, and the streaming onset detector the soak form needs is noted as M10
   work rather than assumed to exist.

---

## [BUG-017] 5.1 was encoded with side channels, which AAC cannot signal, so the file reopened with its speaker positions unspecified

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

The first run of `test_channel_layout` on the reference rig, on the 5.1 cases only
— stereo and 7.1 passed:

```
ChannelLayoutTest.FivePointOneSurvivesIntoTheFileWithItsChannelsInOrder
  media.audio.container_layout.order
    Which is: 0        (AV_CHANNEL_ORDER_UNSPEC)
  AV_CHANNEL_ORDER_NATIVE
    Which is: 1
  media.audio.container_layout.u.mask
    Which is: 0
    Which is: 1551     (0x60F)
```

Six channels came back. Which six was not recorded anywhere in the file.

### Investigation

1. libavcodec had said so on the way in, and the line was sitting in the test
   output above the failure:

   ```
   [aac] Using a PCE to encode channel layout "5.1(side)"
   ```

2. AAC's standard channel configurations (ISO/IEC 14496-3 Table 1.19) are a fixed
   list, and configuration 6 — "5.1" — is `FL FR FC LFE BL BR`, with the surround
   pair at the **back**. FFmpeg's `AV_CH_LAYOUT_5POINT1` is `FL FR FC LFE SL SR`,
   with it at the **sides**: 0x60F, not 0x3F. That is not on the list.
3. When the requested layout is not a standard configuration the encoder falls
   back to a Program Config Element — a legal way to describe an arbitrary layout
   inside the bitstream. What it is not is a *channel configuration index*, and the
   downstream chain reads the index: `avformat_find_stream_info` reopened the file
   with `AV_CHANNEL_ORDER_UNSPEC` and a zero mask, and Matroska carried no
   `ChannelPositions`.
4. So the file said "six channels, positions unknown", and a player is left to
   guess. That is SPEC.md §20 row 17's headline — "5.1 plays as stereo / channels
   swapped" — reached without anything having gone wrong in the muxer at all.
5. SPEC.md §8.5 names the constant: *"Channel layout must be explicitly signalled
   (`AV_CH_LAYOUT_STEREO` / `5POINT1` / `7POINT1`)"*. `resolve_channel_layout`
   implemented that literally. The spec's constant is the one AAC cannot signal.
6. This is reachable from an ordinary machine, not a contrived one. Windows offers
   both forms — `KSAUDIO_SPEAKER_5POINT1` is the back one and
   `KSAUDIO_SPEAKER_5POINT1_SURROUND` the side one — so an endpoint configured as
   "5.1 Surround" would have produced this on the `Auto` path too.

### Root cause

Taking a channel layout constant from the spec without checking it against the
codec's own list of what it can describe. The two disagree, the encoder papers
over the disagreement with a PCE, and the loss shows up two stages downstream in
the container.

### Fix

`ChannelLayoutSetting::Surround51` now resolves to `AV_CH_LAYOUT_5POINT1_BACK`, and
`normalize_for_aac` rewrites a side-channel 5.1 arriving from an endpoint's mask
into the same form.

The relabelling is deliberate and is the lesser cost. On a 5.1 system the surround
pair is one pair of speakers whichever name the file gives it, so naming them
`BL`/`BR` costs nothing a listener can hear; the alternative is a file that names
nothing and leaves every player to guess. 7.1 is left alone — FFmpeg encodes
`AV_CH_LAYOUT_7POINT1` as configuration 7 with a documented non-compliant
interpretation, its own decoder reads it back the same way, and the container mask
survives intact. That is an interop caveat with other decoders rather than a
defect here, and it is noted rather than worked around.

**Ratified 2026-07-29.** SPEC.md §8.5 now reads `5POINT1_BACK` and states the
normalisation, with the PCE reasoning and the 7.1 interop caveat recorded there.

### Regression test

`ChannelLayoutTest.FivePointOneSurvivesIntoTheFileWithItsChannelsInOrder` and
`ChannelLayoutTest.AutoFollowsTheEndpointsOwnMask` (gpu tier). The first records
from an endpoint reporting the *side* form and asserts the *back* form comes back,
so it covers the normalisation and not just the constant. Both assert the
container's mask, the layout the decoder derives from the `AudioSpecificConfig`,
and per-channel tone identification. Green on the reference rig.

### Lessons

1. **A codec's list of describable layouts is shorter than the format's.** AAC has
   twelve standard configurations and everything else costs a PCE. Choosing a
   layout constant is choosing whether the file can be labelled, and that is not
   visible at the call site — it is visible three stages later, in a field nobody
   was looking at.
2. **The encoder said so and nobody was listening.** `Using a PCE to encode channel
   layout "5.1(side)"` is libavcodec telling you precisely what it did and why.
   The line was in the output of the failing run before the failure, and it was
   also in the output of every passing run of `test_audio_encode.cpp` before that —
   which asserted sites 1 and 2 and had no way to notice.
3. **Asserting the layout on the encoder is not asserting it on the file.** This is
   the whole reason row 17 says "in stream *and* container". The encoder-side test
   was green throughout.

---

## [BUG-016] Audio buffers dequeued before the epoch was published were discarded, losing the head of the track non-deterministically

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

`test_av_sync` on the reference rig. Every one of the 65 per-second marks was
inside SPEC.md §20 row 4's 20 ms — and the growth check, which exists to separate
a constant bias from an accumulating one, failed:

```
AvSyncTest.TheBeepAndTheFlashStayWithinTwentyMillisecondsOfEachOther
the offset grew by -20.000000000002672 ms over 65 marks: sync is accumulating
```

−20.000 ms. Exactly one buffer period, to the nanosecond, and all of it between
mark 0 and mark 1.

### Investigation

1. An exact buffer period is not drift. Drift accumulates in fractions; this was
   one whole buffer, once. The remaining 64 marks agreed with each other, so the
   defect was at the head of the recording, not spread through it.
2. The test starts both streams at the same instant, so `t0` is that instant and
   the first audio buffer sits exactly *on* it. The timeline should keep it whole.
3. It never reached the timeline. `AudioEncodePath::process` opened with:

   ```cpp
   if (t0 == kNoEpoch) { ++buffers_before_epoch; return; }
   ```

   and the epoch is published by whichever stream produces *second*. In the test's
   ordering the first audio buffer is offered, then the first video frame is
   submitted, and only then does the epoch resolve — so whether that buffer was
   still in the queue when `t0` landed came down to how the scheduler interleaved
   the `aenc` thread with the caller.
4. When `aenc` won the race, buffer 0 was dropped, the timeline started at buffer 1
   (20 ms later), and `accept` correctly filled the resulting head gap with 20 ms
   of silence — swallowing the first 20 ms of beep 0. The beep's detected onset
   moved 20 ms late, and only that beep's.
5. The gate's reasoning had looked sound: `t0` is the *later* of the two firsts
   (SPEC.md §7.1), so surely anything dequeued before it is earlier than it? No.
   The window is bounded in wall time; which buffers fall inside it is decided by
   thread scheduling. A buffer stamped at or after `t0` can still be dequeued
   before `t0` is *published*.

### Root cause

Deciding whether audio belonged on the timeline from **when it was dequeued**
rather than from **what its timestamp said**. Those coincide only if the consumer
never runs during the epoch-negotiation window, which nothing guarantees and
nothing was checking.

The gate was also redundant. `AudioTimeline::accept` already makes this decision
correctly, on the timestamp, since BUG-014: a packet entirely before `t0` is
dropped and one straddling it is trimmed. The gate existed to protect a timeline
that had not been started yet, and it answered a question the timeline was better
placed to answer.

### Fix

Items that arrive before `t0` is published are **held**, in a bounded stash, and
replayed in arrival order the moment the epoch resolves — before the item that
resolved it. The timeline then decides what survives, on evidence.

The stash is capped at the audio queue's own capacity (64 buffers, ~1.3 s) and
drops its oldest entry on overflow, because CLAUDE.md hard rule 5 applies to it as
much as to a queue; reaching that cap means the video side took more than a second
to produce a frame. A recording stopped before the epoch ever resolves releases
the stash and counts it, rather than encoding it against an epoch that never
existed.

### Regression test

`AvSyncTest.TheBeepAndTheFlashStayWithinTwentyMillisecondsOfEachOther` (gpu tier),
which is what found it: worst offset across 65 marks went from a 20 ms outlier on
mark 0 to **−187 µs**, and the growth check passes. On the CPU tier,
`AudioPathTest.NoAudioFromBeforeTheEpochReachesTheTimeline` asserts the invariant
that survives the fix — that no pre-`t0` audio lands on the timeline — and
deliberately does *not* assert which mechanism refused it, since that is the racy
part and the outcome is not.

### Lessons

1. **Arrival order is not evidence about time.** Every timestamp in this engine is
   authoritative and every arrival order is a scheduling accident; the gate mixed
   the two. The tell is that the failure magnitude was exactly one buffer — a
   quantum of the transport, not of the signal.
2. **Two components deciding the same thing means one of them is wrong.** After
   BUG-014 the timeline could already answer this correctly. The gate was left in
   place because it had been written first, and it answered from worse
   information.
3. **The per-mark check would have shipped this.** All 65 marks passed their 20 ms
   assertion; only the growth check failed, and only because it compares the first
   mark against the last rather than each against a bound. A tolerance wide enough
   to be honest about codec smear is wide enough to hide a whole buffer, so the
   test needs an assertion whose scale is set by the *shape* of the error and not
   by the tolerance.

---

## [BUG-015] The muxer read the audio source timebase back off the stream after `avformat_write_header` had changed it

**Severity:** Critical   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

None observed. Found by reading `Muxer::write` while wiring the audio path's
packets into the mux queue — the code had no audio caller before M4, so nothing
had ever exercised the branch.

### Investigation

1. `Muxer::write` chose the source timebase like this:

   ```cpp
   const AVRational source_timebase =
       is_audio ? stream->time_base : AVRational{1, kVideoTimebaseDen};
   av_packet_rescale_ts(packet, source_timebase, stream->time_base);
   ```

   Video's source is a literal. Audio's is read back off the stream — and the
   destination is the same expression, so for audio the rescale is a no-op by
   construction.
2. A no-op would be harmless if the packet were already in the stream's units. AAC
   packets are not: `AacEncoder` assigns PTS as a **sample count from `t0`**, which
   is its codec context's timebase of `1/48000`. `Muxer::open` does set the audio
   stream's `time_base` to `1/sample_rate` before writing the header, which is why
   the line looks right.
3. `avformat_write_header` is entitled to change a stream's timebase, and
   matroskaenc does not merely reserve the right — `mkv_init` calls
   `avpriv_set_pts_info(st, 64, 1, 1000)` on **every** stream, because Matroska
   block timestamps are stored in units of the segment's TimestampScale and
   FFmpeg fixes that at 1 ms. So by the time any packet is written,
   `audio_stream->time_base` is `1/1000`, not `1/48000`.
4. The consequence: a packet whose PTS is 48000 (one second, in samples) is written
   as 48000 in a 1/1000 timebase — **48 seconds**. Audio would be placed 48× later
   than it belongs, growing without bound. Not a drift; a multiplication.
5. The comment above the line asserted the opposite of what the code did — "the
   encoder's timebase and the stream's are both 1/60000 here, so this is a no-op
   today" — and had been written for the video path, where the literal source makes
   the rescale correct whatever the muxer does to the stream. Extending the line to
   audio reused the shape without reusing the property that made it safe.

### Root cause

Reading a source timebase back from the object that is about to redefine it. The
video path avoided this by construction, because a literal cannot be changed out
from under it; the audio path was written to look symmetric and was not.

The reason this rates Critical despite never having shipped: it is silent and
total. The file muxes, the trailer writes, and every packet is accounted for.

**Correction to an earlier draft of this entry**, which claimed the validation gate
could not see it. It can, indirectly: `ValidationReport::duration_seconds` comes
from `AVFormatContext::duration`, which for Matroska is the segment Duration —
the longest stream, audio included. A track written 48× long makes the container
duration 48× long, and the gate's existing 1% expected-duration check fails. So
the gate would have caught this, by accident, through a check aimed at something
else and only when the caller supplies an expected duration. That is worth
distinguishing from catching it deliberately, and see the Fix below.

### Fix

Both source timebases are now recorded in `Muxer::Impl` at open time, before
`avformat_write_header` runs, and `write` rescales from those:

```cpp
AVRational video_source_timebase{1, kVideoTimebaseDen};
AVRational audio_source_timebase{1, 48000};  // set from the encoder's context
```

The video literal moved into the same field for symmetry, so neither branch reads
a mutable value for a fixed quantity.

The validation gate was widened at the same time, because "caught by accident
through the duration check" is not a property worth relying on. `Muxer::validate`
now takes a `ValidationExpectation` describing what was recorded and, when audio
was written, asserts that the file has an audio stream, that its channel count is
the one that was pinned, that it carries an `AudioSpecificConfig`, that it decodes
to a non-zero number of samples, and that its decoded length agrees with the
file's duration to within a second. That last bound is deliberately loose: this
gate answers "is the recording usable", not "is it in sync" — the 20 ms question
is `test_av_sync`'s, against a signal whose position is known — and a
crash-truncated file legitimately ends its two tracks a fragment apart.

### Regression test

`test_av_sync` (gpu tier, SPEC.md §20 row 4) is the assertion that would have
caught it: a 48× error puts the first beep 48 seconds from its flash. `test_channel_layout`
also covers it incidentally, since a four-second recording would report a
three-minute audio track. **Both are GPU tier and unverified here** — the muxer
needs a hardware video encoder to open at all, so this fix is reasoned from
matroskaenc's source rather than measured. The CPU tier's
`AudioPathTest.EveryBeepLandsWhereTheSharedEpochSaysItShould` covers everything up
to the muxer and cannot see this.

### Lessons

1. **Never read a source unit from the object that owns the destination unit.**
   The two are the same field at different times, and the code cannot tell which
   time it is looking at. Capture the source where it is authoritative.
2. **A comment that says "this is a no-op today" is a request to check tomorrow.**
   It was accurate when written and about a different branch. The line it justified
   grew a second caller with different properties, and the comment came along and
   vouched for it.
3. **A gate that catches a defect by accident has not caught it.** The
   expected-duration check would have failed on this, because the container's
   duration happens to include the audio track — but it fires only when the caller
   supplies an expectation, it names the wrong cause, and nothing about the check
   was designed to look at audio. An audio track that was *missing*, mislabelled,
   or undecodable would have passed cleanly. The gate now checks the audio track
   because it was asked to, not because a neighbouring assertion happened to
   overlap it.
4. **Asserting this entry's own claim was wrong is the point of the entry.** The
   first draft said the gate could not see the defect at all. That was a guess
   about `AVFormatContext::duration` that took thirty seconds to check and was not
   checked, in a document whose value is that its conclusions can be trusted later.

---

## [BUG-014] Audio captured before the shared epoch was written *at* the epoch, displacing the audio that belonged there

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

The first end-to-end run of the audio path's A/V-sync assertion reported every beep
as correct except the first, which was 20 ms late:

```
AudioPathTest.EveryBeepLandsWhereTheSharedEpochSaysItShould
the offset drifted by -20 ms across the recording; it should be a constant
```

Beeps 1 through 64 landed within 200 µs of where the shared epoch said they should.
Beep 0 — the one at the epoch itself — was a full buffer period late, and nothing
else was.

### Investigation

1. The test starts the endpoint 37 ms before `t0`, which is the ordinary case:
   `t0` is the *later* of the two streams' firsts (SPEC.md §7.1), so an audio
   device that opens before the first video frame delivers buffers that predate the
   timeline. Only the first beep was affected, which pointed at start-up rather
   than at any accumulating quantity.
2. Tracing the first four buffers through `AudioTimeline::accept` by hand, with
   `t0` at the epoch and buffers every 20 ms from 37 ms before it:

   | buffer | qpc | `frame_at` | outcome |
   |---|---|---|---|
   | 0 | t0 − 37 ms | 0 (clamped) | writes all 960 frames at position 0 |
   | 1 | t0 − 17 ms | 0 (clamped) | `end` ≤ write head → dropped |
   | 2 | t0 + 3 ms  | 144 | overlap trimmed, last 144 frames written |

   Buffer 0's content is audio from 37 ms *before* the epoch, and it was being
   written as the first 20 ms *after* it. Buffer 1 — which actually contained the
   audio belonging there — was discarded as a duplicate, because buffer 0 had
   already covered that ground.
3. `frame_at` clamps a pre-epoch timestamp to index 0. The clamp is right for the
   question "where should the write head be" and wrong for the question "where does
   this packet start", and one function was answering both. `SessionEpoch`'s own
   header describes the clamp as deliberate, which is what made it look correct on
   review: the clamp *is* deliberate, but clamping a packet's start without also
   trimming its leading frames moves the packet rather than cropping it.

### Root cause

A packet straddling or preceding `t0` had its start index clamped to zero while
its frame count was left whole, so pre-epoch samples were written at post-epoch
positions. The timeline's *length* stayed correct — which is why every duration
assertion in `test_audio_timeline.cpp` passed — and only its *content* was
displaced, by up to one buffer period, once per recording.

The reason this is worth an entry rather than a shrug: it is invisible to every
aggregate check. Duration is right, sample count is right, no gap is reported, and
the misplacement lasts 20 ms at the head of the file. It was only caught because
the test asserted against a signal whose position was known independently.

### Fix

`AudioTimeline` grew `signed_frame_at`, which returns the real (possibly negative)
index, and `frame_at` became `max(signed_frame_at, 0)` for the write-head question.
`accept` now computes `pre_epoch_frames` from the negative part and trims them off
the front of the packet:

- a packet entirely before `t0` contributes nothing and is dropped;
- a packet straddling `t0` contributes only its tail, starting at index 0.

Both trims the timeline applies — pre-epoch frames and frames overlapping ground
already written — come off the *front*, so a caller writes the packet's last
`audio_frames` frames. That was already true of the overlap trim and is now stated
in `TimelineSegment`'s documentation rather than left to be rediscovered.

### Regression test

`AudioTimeline.APacketEntirelyBeforeT0IsDroppedRatherThanClampedToZero` and
`AudioTimeline.APacketStraddlingT0ContributesOnlyThePartAfterIt` (cpu tier) pin
both halves directly. `AudioPathTest.EveryBeepLandsWhereTheSharedEpochSaysItShould`
is what found it and now passes with a worst-case offset of 187 µs across 65
seconds. The previous test —
`AudioTimeline.APacketStampedBeforeT0ClampsRatherThanGoingNegative` — asserted the
buggy behaviour and was replaced; it had encoded "does not go negative" as "clamps
the whole packet", which are not the same requirement.

### Lessons

1. **A clamp is not a trim.** Both keep an index non-negative and they differ in
   what happens to the data behind it. The function was named for the invariant it
   preserved (`frame_at` never returns negative) rather than for the question it
   answered, and the name is what made the second caller look correct.
2. **Length assertions cannot see displacement.** Every existing timeline test
   checked frames written against wall clock, and all of them passed throughout. A
   defect that moves content without changing its quantity needs a signal whose
   position is known independently of the mechanism under test — which is exactly
   what SPEC.md §20.1's synthetic 1 kHz tone is for, and why the tone generator was
   worth building before the tests that use it.
3. **The "documented as deliberate" note nearly closed the investigation early.**
   `SessionEpoch`'s header says pre-epoch material clamps to index 0 and that both
   the pacer and the timeline already do it. That is accurate, and it made the
   clamp look reviewed rather than accidental. A comment recording *that* a
   decision was made is not evidence that it was made for the case in front of you.

---

## [BUG-013] `AacEncoder::submit` could not be safely retried after backpressure, so a resumed submit would have encoded the same samples twice

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

None observed. Found by reading `AacEncoder::submit` while writing the `aenc`
thread's drain loop against it — the same way BUG-008 was found, and with the same
implication: the code path that would have exposed it is the one a real recording
takes under load, and no test reached it.

### Investigation

1. BUG-011 established that `avcodec_send_frame` returning `EAGAIN` is
   backpressure, and made `submit` report it as `INTERNAL_QUEUE_FULL` so the caller
   can drain and try again.
2. What "try again" means was never specified. `submit` accumulates the caller's
   samples into a staging frame as it walks the input:

   ```cpp
   while (consumed < frame->nb_samples) {
       ...copy `take` samples into staging...
       staged += take; consumed += take;
       if (staged == frame_size) { FC_TRY(send_staged()); }
   }
   ```

   On `INTERNAL_QUEUE_FULL` the `FC_TRY` returns from the middle of that loop.
   `consumed` samples have already been copied into staging and are gone from the
   caller's point of view — but the caller has been told the call failed.
3. The only thing a caller can do with a failed `submit` is drain and call it again
   with the same frame, which re-copies from sample 0. Those first `consumed`
   samples are then encoded twice: the audio track grows, and everything after the
   duplication is late by the duplicated duration. That is SPEC.md §20 row 4's
   failure arriving through the code BUG-011 added to prevent audio loss.
4. In the shipped pipeline it is unreachable today — the `aenc` thread drains after
   every submit and endpoint buffers are 960 samples, under one AAC frame, so at
   most one `send_staged` happens per call and a fully drained encoder always
   accepts it. "Unreachable given the current caller" is not a property of the
   function, and the function is the thing being reviewed.

### Fix

`submit` now takes an `offset` and returns the number of samples it consumed:

```cpp
[[nodiscard]] Result<int> submit(const AVFrame* frame, int offset = 0);
```

A short return — fewer than `nb_samples - offset` — means the output queue is full.
The caller drains with `receive` and calls again with `offset` advanced by the
returned count, so the staged samples are never resent. The protocol is stated on
the declaration rather than inferred.

`flush` had the same shape and was fixed alongside it: it set `flushed = true`
*before* sending the trailing partial frame, so a retry after a `QUEUE_FULL` would
hit the idempotence guard and silently discard the very samples the flush existed
to preserve. The flag now moves only after the send succeeds. `flush` also treats
`EAGAIN` from the end-of-stream `avcodec_send_frame(nullptr)` as backpressure — the
marker is input like any other, and libavcodec refuses input while packets wait.

### Regression test

`AudioPathTest.ASubmitThatHitsBackpressureResumesRatherThanRepeating` (cpu tier)
submits 4096 samples — exactly four AAC frames — without draining until the encoder
refuses, then resumes from the returned offset. It asserts the encoder ends up
having sent exactly four frames. A resume-from-zero implementation sends more, and
the count is what discriminates. `AudioPathTest.ASilentStretchIsEncodedRatherThanSkipped`
covers the flush path by asserting the decoded track's length.

### Lessons

1. **A partial-failure return has to say how partial.** `Result<void>` can express
   "worked" and "did not work" but not "worked as far as here", and a function that
   mutates state as it goes needs the third. Returning the error alone was
   well-formed, checked by the caller, and still unsafe.
2. **This is BUG-007 and BUG-011's third act.** Both were "backpressure reported as
   failure". This one is "backpressure reported correctly but not *recoverably*",
   which is what is left after you fix the first mistake without asking what the
   caller does next. The encoder interfaces now document the drain-and-resume
   contract; the previous two entries recorded the classification and stopped there.
3. **An idempotence guard set before the work is not idempotent.** `flush` marking
   itself done and then failing is a guard that converts a retryable error into
   silent data loss. Set the flag after the work, or the flag is a lie about it.

---

## [BUG-012] `Resampler::convert` documented a null input as "produce silence", where libswresample reads it as "flush"

**Severity:** Major   **Found:** 2026-07-29   **Fixed:** 2026-07-29   **Commit:** _uncommitted_

### Symptom

None observed — the function had no caller until the `aenc` thread needed one. The
defect is in the contract, and the first caller to trust it would have been the
silence generator.

### Investigation

1. `Resampler::convert`'s declaration read:

   > `input` may be null when `frames` is a silent stretch, in which case silence
   > of that length is produced in canonical format.

   The `aenc` thread's silence path was written against exactly that sentence,
   because it is the natural way to emit `TimelineSegment::silence_frames`.
2. The implementation forwards a null `input` straight to `swr_convert` as a null
   input pointer. libswresample documents that form as the **flush** call: it
   ignores `in_count` entirely and returns whatever it had buffered, which for a
   48 kHz-in/48 kHz-out configuration — no resampling, no filter delay — is zero
   samples.
3. So a request for two seconds of silence would have returned an empty frame, and
   the audio track would have been short by exactly the length of every silent
   stretch. That is SPEC.md §8.2's 30-minutes-of-video-with-22-minutes-of-audio
   defect, reintroduced by the component built to prevent it, and it would have
   been invisible in any test that only checked `AudioTimeline`'s counters — the
   timeline would have reported the silence as written, because from its point of
   view it was.

### Root cause

A documented contract that the implementation never had. The header described the
behaviour the caller would want; the body passed the argument through to a
third-party function whose meaning for that argument is different and whose failure
mode is a short return rather than an error.

### Fix

Split into three explicit operations over one shared `run`:

- `convert(input, frames)` refuses a null input with `INTERNAL_INVALID_ARGUMENT`
  rather than forwarding it;
- `convert_silence(frames)` feeds a preallocated zero buffer in the *endpoint's*
  format through `swr_convert`, so silence goes through libswresample rather than
  around it — bypassing it would leave the filter delay and the rate conversion's
  fractional accumulator untouched across the gap, and the samples after a silent
  stretch would land a fraction of a sample away from where the ones before it did;
- `flush()` is the null-input call, named for what it does.

The zero buffer is allocated once at `initialize` and capped at
`kSilenceChunkFrames` (100 ms), so a silent desktop asking for minutes of silence
costs no allocation on the `aenc` thread and the caller chunks.

### Regression test

`AudioPathTest.TheResamplerProducesRealSilenceAndRefusesANullInput` (cpu tier)
asserts the refusal, asserts `convert_silence(480)` returns exactly 480 samples,
and checks every sample of every channel is zero. `AudioPathTest.ASilentStretchIsEncodedRatherThanSkipped`
is the end-to-end form: a three-second gap in a seven-second recording must decode
back as seven seconds of audio.

### Lessons

1. **A contract with no caller has not been tested, whatever the header says.** The
   sentence had survived review and a milestone. Nothing had ever passed null.
2. **Wrapping a third-party function does not reinterpret its arguments.** The
   wrapper's documentation described the wrapper's intent and the body was a
   pass-through, so the two disagreed silently. Where a wrapper's contract differs
   from the wrapped call's, the difference has to be *implemented*, not written
   down.
3. **The failure mode was a short return, not an error.** `swr_convert` would have
   succeeded and returned zero. Every `has_value()` check on the path would have
   passed. Functions whose under-delivery is legal need their output *counted* by
   the caller, which is why the regression test asserts `nb_samples` rather than
   just success.

---

## [BUG-011] AAC's `avcodec_send_frame` returning EAGAIN was treated as failure, which would have discarded audio

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

The first AAC encoder test that submitted more than a handful of buffers without
draining between them failed:

```
AudioEncodeTest.AacConsumesFixedFramesAndTheCallerNeedNotAlign
Value of: encoder.submit(frame.get()).has_value()
  Actual: false
```

100 buffers of 480 samples each (10 ms WASAPI buffers) were submitted in a loop
with no `receive()` calls between them. The submit loop failed partway through.

### Investigation

1. `AacEncoder::submit` accumulates samples into a staging frame and calls
   `avcodec_send_frame` once 1024 samples (one AAC frame) have arrived. The
   accumulation logic was not in question — the failure was in the send itself.
2. `avcodec_send_frame`'s return value is documented as `AVERROR(EAGAIN)` when the
   encoder's internal output queue is full and must be drained with
   `avcodec_receive_packet` before it will accept more input. The test submitted
   ~11 full AAC frames' worth of audio (100 × 480 = 48000 samples ÷ 1024 ≈ 46
   frames, in fact — more than enough to fill the queue) before ever calling
   `receive()`.
3. This is the same shape of defect as BUG-007 in the video encoder: a hardware or
   software encoder with internal buffering signals "not right now" and the
   surrounding code treated it identically to "this failed."

### Root cause

`send_staged()` returned `FcError::ENCODE_SUBMIT_FAILED` for any negative return
from `avcodec_send_frame`, without checking whether the negative value was
`AVERROR(EAGAIN)` specifically. EAGAIN is backpressure — the encoder is holding
packets nobody has collected — not a fault. Reporting it as a fault gives the
caller nothing to act on except "stop encoding," which for audio means silently
truncating the track. Audio drops are audible and are explicitly forbidden by
SPEC.md §12's queue-policy rules (audio is never dropped; video degrades instead).

### Fix

`send_staged()` now checks for `AVERROR(EAGAIN)` before the generic failure branch
and returns `FcError::INTERNAL_QUEUE_FULL` — the same code BUG-007's fix
introduced for the video encoder's pool exhaustion. The test was also corrected:
it now drains with `receive()` after every `submit()` call, which is what the real
pipeline does and what the encoder actually requires.

### Regression test

`AudioEncodeTest.AacConsumesFixedFramesAndTheCallerNeedNotAlign` (gpu tier) submits
100 misaligned buffers with a drain after each submit and asserts at least 46
packets are produced with none lost. The undrained version of the test is what
originally caught the bug and is not preserved separately — the fix changed both
the encoder's error classification and the test's usage pattern, and only the
corrected pattern is one the real pipeline would ever produce.

### Lessons

1. **The same defect shape recurred within one milestone.** BUG-007 (M3, the video
   encoder's pool exhaustion) and this bug are the identical mistake — treating an
   encoder's "drain before you can send more" signal as failure rather than
   backpressure — in two different encoders written days apart. Neither
   `IVideoEncoder` nor `AacEncoder`'s interface documentation called out EAGAIN
   handling as a required behavior before this; it should have been written down
   after BUG-007 rather than rediscovered.
2. **A test that never drains is not a realistic caller.** The bug was caught
   because the test happened to submit enough data to fill the queue in one loop.
   A test that drained after every submission — the only pattern a real pipeline
   would use — would never have exercised the EAGAIN path at all, and the defect
   would have shipped invisible until a real recording produced enough audio to
   trigger it.

---

## [BUG-010] `functiondiscoverykeys_devpkey.h` fails to parse under clang, silently invalidating every clang-tidy finding in the file

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

Linting `loopback_capture.cpp` after it built and ran correctly under MSVC
produced a wall of clang-tidy errors inside a Windows SDK header:

```
functiondiscoverykeys_devpkey.h(47,20): error: use of undeclared identifier 'PKEY_NAME'
functiondiscoverykeys_devpkey.h(53,1): error: a type specifier is required for all declarations
...
error: too many errors emitted, stopping now [clang-diagnostic-error]
```

followed by dozens of clang-tidy findings against `loopback_capture.cpp` itself —
`misc-const-correctness` on nearly every local variable, `cppcoreguidelines-init-variables`
on already-initialized `HRESULT`s, a special-member-functions complaint on `Impl`.

### Investigation

1. The SDK header error was the real signal, and it was almost missed: the
   temptation was to start fixing the 15+ findings in `loopback_capture.cpp`
   directly, since several of them (the special-member-functions one, in
   particular) looked plausible on their own.
2. `clang-diagnostic-error: too many errors emitted, stopping now` means clang
   abandoned parsing partway through the translation unit. Everything reported
   *after* that point — which was all of `loopback_capture.cpp`, since the failing
   header is included near the top — comes from an incomplete or recovered AST and
   cannot be trusted. The const-correctness and init-variable findings on
   perfectly ordinary `UINT32`/`HRESULT` locals were not real; they were
   clang-tidy doing its best with a parse tree that never finished.
3. The actual defect: `functiondiscoverykeys_devpkey.h` calls the
   `DEFINE_PROPERTYKEY` macro without including the header (`propkeydef.h`) that
   defines it. MSVC's standard include order happens to bring in `propkeydef.h`
   first through an unrelated chain, so this has never been a problem building
   with `cl.exe`. clang-cl, with a different include resolution order, hits
   `functiondiscoverykeys_devpkey.h` before anything has defined the macro, and the
   macro invocations become malformed declarations.
4. First fix attempt: `#include <propkeydef.h>` immediately before
   `functiondiscoverykeys_devpkey.h`. This fixed clang and broke MSVC —
   `propkeydef.h` `#undef`s `DEFINE_PROPERTYKEY` at the bottom of the header on
   the assumption that whatever needed it has already used it, so by the time
   `functiondiscoverykeys_devpkey.h`'s later property keys are declared, the macro
   is gone again under *either* compiler's normal header order once both are
   explicitly included.

### Root cause

A Windows SDK header with an implicit, unstated include-order dependency, invisible
under MSVC because of an accidental transitive include, and fatal under clang
because that accident doesn't hold. No amount of code review of
`loopback_capture.cpp` would have found this — the bug is entirely in a header the
project does not own, and its symptom (a page of unrelated-looking lint findings)
actively points away from the real cause.

### Fix

Stopped including `functiondiscoverykeys_devpkey.h` at all. The only symbol needed
from it was `PKEY_Device_FriendlyName`, a well-known, permanently stable GUID/PID
pair. It is now defined locally as a single `const PROPERTYKEY` with a comment
explaining why, rather than pulled in through a header that only works correctly
under one of the project's two compilers.

### Regression test

None directly — this is a build/tooling defect, not an engine behavior, and
belongs to the class CLAUDE.md's ENGINEERING_LOG convention excludes (though it
sits close to the line, since it did affect the correctness signal `lint.ps1`
reports for real code). The existing `AudioCaptureTest` suite continues to build
and pass under both MSVC and the clang-tidy pass; a clean `lint.ps1` run against
`loopback_capture.cpp` is the practical regression check.

### Lessons

1. **`clang-diagnostic-error: too many errors emitted, stopping now` invalidates
   everything after it, not just the errors named alongside it.** Every finding
   downstream of that line in the same file is suspect until the parse is fixed,
   even findings that look individually plausible. Fixing them individually would
   have wasted effort and left the real defect — and the header dependency it
   exposes — untouched.
2. **A fix verified against only one compiler is not verified.** The
   `propkeydef.h` pre-include fixed clang and was not re-checked against MSVC
   before being considered done; it broke the build that had been working. Every
   Windows-header fix in this project needs both compilers checked, specifically
   *because* clang-tidy and cl.exe read the SDK headers through different paths.
3. **When a third-party header has an undocumented ordering requirement, depending
   on it at all is fragile even when it currently works.** Replacing the include
   with a local definition of the one symbol needed removed the dependency
   entirely rather than finding the "correct" include order — the safer fix when
   the header's own internal contract is unstated and enforced only by which
   compiler happens to be reading it.

---

## [BUG-009] WASAPI interfaces were created on the caller's thread and pumped from another, so `start()` failed with a generic COM error

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

Every `LoopbackCapture` test failed identically, including the simplest one that
only opens the default endpoint:

```
AudioCaptureTest.OpensTheDefaultRenderEndpointInLoopback
Value of: started.has_value()
loopback start failed: AUDIO_INIT_FAILED
```

`AUDIO_INIT_FAILED` is the code `CoCreateInstance` failure maps to — the very
first WASAPI call in `start()`, before any endpoint or format negotiation.

### Investigation

1. `CoCreateInstance(__uuidof(MMDeviceEnumerator), ...)` was failing on its own,
   with no endpoint or device involved yet. That narrowed it immediately to a COM
   initialization problem rather than anything WASAPI-specific.
2. `start()` ran entirely on the caller's thread — the gtest worker thread in every
   failing test — and only *afterward* spawned `capture_loop()` on a separate
   `std::thread` to pump buffer-ready events. The gtest thread had never called
   `CoInitializeEx`, so it was not in any COM apartment, and `CoCreateInstance`
   requires one.
3. This was directly contrary to a comment already sitting in the same file,
   written when `capture_loop()` was first drafted, correctly noting that WASAPI
   interfaces are apartment-sensitive and must be pumped from the thread that
   created them. The comment was correct; the code that shipped alongside it did
   the opposite — created on one thread, pumped from another.

### Root cause

Splitting "open the endpoint" (on the caller's thread, synchronously, so `start()`
can return a `Result` immediately) from "pump it" (on the capture thread, because
that is where blocking waits belong) is a natural-looking decomposition that is
wrong for COM: creation and use of an STA-sensitive or thread-affine COM object
must happen on the same thread, and the object is unusable — or, in this case,
never even successfully created — from a thread with no COM apartment at all.

### Fix

`open_endpoint()` (the `CoCreateInstance` through `IAudioClient::Start` sequence)
moved entirely onto the capture thread, called from `capture_loop()` itself before
it enters its wait loop. `start()` now spawns the thread first, then blocks on a
`std::promise<Result<void>>` that `capture_loop()` fulfills once it has either
succeeded in opening the endpoint or failed and is about to exit. This makes
`start()`'s synchronous, `Result`-returning contract compatible with COM's
same-thread requirement: the caller still gets an immediate answer, but the
actual COM work happens on the thread that will go on to use those interfaces.

If opening fails, `capture_loop()` returns before entering its wait loop, and
`start()` joins the now-exited thread before returning the error, so a failed
`start()` leaves nothing running.

### Regression test

`AudioCaptureTest.OpensTheDefaultRenderEndpointInLoopback` and the rest of the
`AudioCaptureTest` suite (gpu tier) exercise this path on every run — they were
the tests that caught the bug in the first place, since the fixture calls
`start()` from the test thread with no COM initialization, which is exactly the
caller shape that used to fail. `AudioCaptureTest.StoppingIsIdempotentAndSafeWithoutStarting`
additionally covers a `start()` that never completes threading correctly on the
failure side.

### Lessons

1. **A comment describing an invariant does not enforce it.** The apartment
   -affinity comment was accurate and present in the file before the bug was
   written; it documented the rule the very code below it broke. Comments record
   intent; they do not check it.
2. **Splitting "acquire" from "use" across a thread boundary is a common shape
   that COM (and several other Windows APIs — GDI, some DirectX interfaces under
   certain creation flags) specifically forbids.** The decomposition that looks
   natural in ordinary C++ — do the fallible setup synchronously, hand the result
   to a background worker — needs to be checked against the API's threading
   contract before assuming it is safe, not after.
3. **A synchronous `Result`-returning `start()` and a same-thread COM requirement
   are not in conflict**, they just need the promise/future handshake to bridge
   them. The fix did not weaken the caller-facing contract to work around the COM
   constraint; it kept the contract and moved the work.

---

## [BUG-008] Shutdown used an unbounded join, so a wedged worker would have hung finalization forever

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

None observed. Found by reading the code while deciding whether M3 was safe to
build on, not by a failing test — which is the whole problem with it.

### Investigation

1. `VideoPipeline::stop()` closed each queue and then called `thread.join()` with
   no deadline, twice.
2. SPEC.md §12 is explicit: *"Every thread has an ownership-documented shutdown
   path with a bounded join timeout (2 s), after which the shutdown is escalated
   and logged — never an unbounded `join()`."*
3. The file already contained a constant for it:

   ```cpp
   /// SPEC.md §12: bounded join timeout, after which shutdown escalates and is
   /// logged. Never an unbounded join.
   constexpr auto kJoinTimeout = std::chrono::seconds{2};
   ```

   Declared, documented, and never referenced. The comment asserted the behaviour;
   the code two hundred lines below did the opposite.
4. Looked for a path that could actually wedge, rather than assuming it was
   theoretical. There is one, and it is not exotic: `mux_queue` uses
   `QueuePolicy::Block` because SPEC.md §12 forbids dropping audio, and video
   packets are expensive enough to be worth waiting for. So a mux thread stalled on
   a slow or disconnected disk blocks the venc thread inside `push`, which blocks
   `encode_queue.close()`'s join, which hangs `stop()`. Every step is ordinary
   behaviour; the deadlock is emergent.

### Root cause

`std::thread::join` has no timed form, so honouring the deadline needs a separate
completion signal. That is a few lines of work, and skipping it left an
unbounded wait on the one path that must never be unbounded: the path that
finalizes the file.

A recording that hangs on stop is worse than one that fails on stop. CLAUDE.md §1
permits only disk-full and file-handle-loss to prevent a valid output file; a
wedged worker thread is neither.

### Fix

- Each worker fulfils a `std::promise<void>` as it leaves its loop, via a
  `ScopedSignal` guard so an exception still releases the waiter.
- `await_worker` waits `kJoinTimeout` on the future. Ready → `join()`, which then
  returns immediately. Expired → log `INTERNAL_THREAD_JOIN_TIMEOUT` and `detach()`.
- On expiry the pipeline sets `abandoned_`, and the destructor **deliberately
  leaks** `impl_` rather than freeing state a detached thread is still reading.
  A bounded one-off leak on an already-failing path beats a use-after-free.

The escalation deliberately does not attempt to finalize. For MKV an unfinalized
file is playable to its last complete cluster (SPEC.md §10.2) — which is why MKV
is the default container, and why abandoning is survivable.

### Regression test

`VideoEncodeTest.ShutdownCompletesWellInsideTheJoinDeadline` (gpu tier) — records
60 frames and asserts `stop()` returns in under 4 s, so a healthy shutdown is
provably nowhere near the deadline.

**The escalation branch is not covered.** Exercising it needs a fault-injection
seam in the venc loop that does not exist. Stated rather than glossed: the happy
path is tested, the failure path is reasoned about.

### Lessons

1. **A comment describing behaviour is not the behaviour.** The constant and its
   doc comment made the file *read* as though the deadline were implemented, which
   is worse than an obvious omission — it defeats review by looking finished.
2. **An unused constant is a claim nobody checked.** `kJoinTimeout` was dead for
   an entire milestone and neither the compiler nor clang-tidy objected. Where a
   constant encodes a requirement, something has to reference it or the
   requirement is not implemented.
3. **Deadlocks assemble from correct parts.** Block-on-full is right for the mux
   queue, waiting for a worker is right at shutdown, and disk stalls happen. The
   hang exists only in the composition, which is exactly what a per-component
   review does not see.

---

## [BUG-007] The encoder input pool was sized like a queue, and NVENC exhausted it after eight frames

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

With the header problem of BUG-006 fixed, the RTX 4050 wrote a valid MKV that was
2 % of the expected length:

```
output failed validation -- duration 0.134 s differs from expected 2.000 s
[submitted 120, encoded 8, paced out 0, queue-dropped 0, dup 0,
 convert-fail 0, encode-fail 111]
```

The 780M was unaffected.

### Investigation

1. The counters made it a two-line diagnosis, which is the only reason this entry
   is short. 120 frames went in, 8 came out, 111 failed — and 8 was exactly
   `VideoEncoderSettings::pool_size`. One frame succeeded per pool slot and then
   every subsequent frame failed.

   Those counters did not exist when the run started. `submit` only enqueues, so a
   frame that dies on the venc thread never surfaces at the call site; the first
   version of the test could see only that the file was short. Surfacing the
   pipeline stats in the failure message turned "the file is wrong" into "the pool
   is exhausted" without a debugger.

2. `av_hwframe_get_buffer` was the failing call. The pool is fixed-size, and a
   hardware encoder holds one input surface for every frame it has *accepted* but
   not yet emitted a packet for.

3. NVENC's pipeline is several frames deep before it emits anything — it returns
   `EAGAIN` from `avcodec_receive_packet` while it fills. So the first 8 frames
   were accepted and held, no packets came back, no surfaces were returned, and
   frame 9 onwards had nowhere to go. AMF's shallower pipeline emitted packets
   early enough that its surfaces recycled before the pool ran dry.

### Root cause

Two different things were both called a queue depth and given the same number.

SPEC.md §9 specifies a **bounded encoder input queue, capacity 8** — that is how
many *captured frames* may wait to be submitted. The **hardware frame pool** is a
different quantity: how many frames the encoder may hold at once. They are only
equal if the encoder returns each surface before the next frame arrives, which is
true of no hardware encoder.

The deeper mistake was treating `av_hwframe_get_buffer` failing as an error at all.
It is the pool's backpressure signal. Reporting it as `ENCODE_SUBMIT_FAILED` made
the pipeline log a warning, drop the frame, and continue — turning a "wait a
moment" into 111 discarded frames.

### Fix

- Pool size raised to 32 and documented as distinct from the §9 queue capacity,
  with the reason stated at the field.
- `av_hwframe_get_buffer` failing now returns `INTERNAL_QUEUE_FULL`, which the
  caller can tell apart from a real failure.
- `VideoPipeline::submit_with_backpressure` responds to it correctly: drain
  packets — which is what returns surfaces — and retry. Bounded at 64 attempts, so
  a genuinely wedged encoder surfaces as an error rather than spinning the venc
  thread forever.

The pool stays fixed-size. Growing it on demand would be an unbounded queue wearing
a different hat (CLAUDE.md hard rule 5).

### Regression test

`VideoEncodeTest.ProducesAPlayableMkvOnEveryEncodingAdapter` (gpu tier) submits
120 frames on **every** encoding adapter and asserts 120 decode back out. It also
asserts `encode_failures == 0` and `convert_failures == 0`, so a silent partial
failure fails the test rather than shortening the file.

### Lessons

1. **Two bounded resources that happen to share a number are still two resources.**
   The §9 queue capacity of 8 is about frames waiting to go in; the pool is about
   frames the encoder is holding. Naming the pool field after the spec's queue
   capacity is what made 8 look like the obvious value.
2. **Backpressure reported as failure becomes data loss.** The distinction between
   "not now" and "not ever" has to survive the return type, or the caller's only
   available response is to discard.
3. **Counters on the wrong side of a thread boundary are invisible.** A pipeline
   whose stages run on their own threads needs its failure counters surfaced at the
   assertion site; otherwise every internal fault looks like "the output is wrong"
   and gets debugged from the wrong end.

---

## [BUG-006] NVENC emits no codec extradata, so the MKV header could not be written on one of the two adapters

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

The first end-to-end M3 run produced a playable MKV on the Radeon 780M and failed
on the RTX 4050 before writing a single frame:

```
NVIDIA GeForce RTX 4050 Laptop GPU: pipeline start failed, MUX_WRITE_HEADER_FAILED
```

Same code, same settings, same synthetic source. Only the adapter differed.

### Investigation

1. The encoder opened successfully on both. The failure was in
   `avformat_write_header`, after the stream had been added and its parameters
   copied from the codec context.
2. Matroska stores H.264 parameter sets in the `CodecPrivate` element, filled from
   `AVCodecParameters::extradata`. The muxer had no check on it, so an empty
   extradata reached `write_header` and came back as a generic failure that named
   nothing.
3. `avcodec_open2` populates `extradata` only when the encoder is asked for a
   global header. Nothing in the code asked. AMF supplies parameter sets anyway;
   NVENC emits them in-band only, so its `extradata` stayed null.

### Root cause

`AV_CODEC_FLAG_GLOBAL_HEADER` was never set. It is required by both containers v1
targets — Matroska's `CodecPrivate` and MP4's `avcC` — and whether an encoder
supplies extradata without being asked is vendor-specific.

Relying on the default therefore produced code that worked on whichever adapter
happened to be tested first. On a hybrid-GPU laptop that is a coin flip, and the
coin came up AMD.

### Fix

- `VideoEncoderSettings::global_header` (default true), applied as
  `AV_CODEC_FLAG_GLOBAL_HEADER` before `avcodec_open2`.
- The muxer now checks `extradata` after copying parameters and fails with a
  message naming the cause and the remedy, rather than deferring to
  `write_header`'s generic error several lines later.

### Regression test

`VideoEncodeTest.ProducesAPlayableMkvOnEveryEncodingAdapter` (gpu tier) — records
and decodes on **every** adapter that reports an encoder, not just the first one
found. That loop is what makes this class of bug visible at all.

### Lessons

1. **"Works on this GPU" is not a result on a hybrid-GPU machine.** Every
   hardware-facing test in this project iterates adapters for exactly this reason;
   this bug is what happens in the gap before one does.
2. **A container requirement is not an encoder default.** libavcodec will happily
   open an encoder that cannot be muxed into the container you are about to use.
   The two are configured independently and nothing checks the pair.
3. **Validate at the boundary that knows the requirement.** The muxer knows it
   needs parameter sets; it is the right place to say so. Letting the failure fall
   through to `avformat_write_header` cost the entire investigation above, because
   that function's error names no field.

---

## [BUG-005] A stale compile database made the lint gate report clean on a shrinking fraction of the tree

**Severity:** Major   **Found:** 2026-07-28   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

### Symptom

`scripts/lint.ps1` had been reporting `clang-tidy: clean` at the end of every
milestone since M1. Re-running it after a routine reconfigure produced 37 findings
across nine files — none of them newly written, and several dating back to M2.

Nothing about the source had changed between the clean run and the failing one.

### Investigation

1. First hypothesis was toolchain drift: a newer clang-tidy with new checks. Several
   of the findings were from checks added relatively recently
   (`modernize-use-integer-sign-comparison`, `misc-use-internal-linkage`,
   `bugprone-invalid-enum-default-initialization`), which fit the theory neatly.

   It was wrong. The clang-tidy that produced the clean runs was still on disk in a
   previous session's scratch directory; `--version` reported **22.1.8** for both.
   Same binary, same checks.

2. The next difference was the compile database. `lint.ps1` picks the first
   `compile_commands.json` under `build/`, and the one it had been using was dated
   *before M2 started*.

3. `lint.ps1` deliberately restricts clang-tidy to files that appear in the
   database — passing a file that is absent produces a spurious "not found in
   compilation database" error, so filtering is correct. The filter is an
   intersection, and it is silent:

   ```powershell
   $targets = $sources | Where-Object { $dbFiles -contains $_.FullName }
   ```

   A file missing from the database is not analysed and not reported. The count in
   the success message (`clean (N files)`) was the only evidence, and N shrinking
   relative to the tree is not something a human notices.

4. Counting confirmed it: the database listed 46 entries against a tree that had
   grown well past that. Every `.cpp` added since M1 — the whole of `capture/`,
   `color/`, and their tests — had never once been analysed.

### Root cause

Two correct behaviours combining into a wrong one.

- A compile database is a *build artefact*. It is regenerated by `cmake`, and
  nothing regenerates it when a source file is added — the file lands in
  `CMakeLists.txt`, the normal build picks it up through the Visual Studio
  generator, and the separate Ninja tree used only for tidy is never reconfigured.
- The gate's file filter fails **open**. Absent from the database means not
  checked, and not checked was indistinguishable from checked and clean.

The result is a quality gate whose coverage silently decays. It is at its most
misleading exactly when it matters most: right after new code is written, which is
the only time a file can be missing from a stale database.

### Fix

Three parts.

1. **Fail closed.** `lint.ps1` now diffs the tree's `.cpp` files against the
   database and *fails*, naming every file that would have been skipped and
   printing the reconfigure command. A gate that cannot see the whole tree now
   says so instead of passing.
2. The 37 findings were fixed on their merits — see the Fixed section of
   `CHANGELOG.md`. Two suppressions were added with written justifications
   (`performance-no-int-to-ptr` project-wide, for Win32 `LPARAM` context pointers
   and the deliberate opaque-handle representation; `clang-analyzer-core.CallAndMessage`
   scoped to `capture/wgc/` alone, for two false positives inside cppwinrt that
   cannot be annotated at the source because analyzer diagnostics are reported at
   the sink and bypass every header filter).
3. `ExcludeHeaderFilterRegex` now excludes `build/**`, so fxc's generated shader
   bytecode header — which sits under a path containing `engine` and therefore
   matched the include filter — stops being linted as if it were hand-written.

### Regression test

Verified in both directions by removing three entries from the database and
re-running: the gate failed and named exactly those three files
(`capture_factory.cpp`, `test_pipeline_synthetic.cpp`, `test_capture_factory.cpp`).
Restoring the database returned it to `clean (49 files)`.

Not a ctest case: the subject is the lint script, which is not in the build graph.

### Lessons

- **A quality gate must fail closed.** "Nothing to check" and "checked, nothing
  found" have to be distinguishable, or coverage decays without a signal. This is
  the same class of defect as DDA returning black frames with `S_OK` (§5.1) — the
  mechanism reports success while doing nothing.
- **Suspect the input before the tool.** The version-drift theory was plausible and
  cost time; the compile database's timestamp would have settled it in one command.
- Derived artefacts that gate correctness need a freshness check, not a
  regeneration convention. Conventions are not enforced and this one had been
  quietly broken for two milestones.

---

## [BUG-004] A DPI-unaware process is told a virtualised desktop size, so every captured frame was rejected

**Severity:** Major   **Found:** 2026-07-27   **Fixed:** 2026-07-27   **Commit:** _uncommitted_

### Symptom

The first end-to-end capture test captured zero frames in ten seconds. Frames were
arriving — the other tests in the same fixture acquired textures, checked their
adapter, and watched sequence numbers advance — but every single one failed
conversion, so nothing reached disk.

### Investigation

1. Noted that the failure was not "no frames". `Nv12Converter::convert` was
   returning `CAPTURE_RESOLUTION_CHANGED` for every frame, and the test's `continue`
   turned that into a silent zero.
2. The converter was sized from `DXGI_OUTPUT_DESC.DesktopCoordinates`, which M1's
   topology service reported as **1536x864**.
3. `1536 x 1.25 = 1920`, `864 x 1.25 = 1080`. That is a 125% display scale.
4. Confirmed with a two-line probe: a process that calls
   `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` is told **1920x1080**; the
   same query from a DPI-unaware process returns **1536x864**.

### Root cause

Neither the engine nor the test binaries declared DPI awareness. Windows
therefore virtualised what it reported to them: DXGI described the desktop in
scaled logical pixels, while `Windows.Graphics.Capture` — which is not subject to
that virtualisation — handed over a real 1920x1080 texture. The pipeline was sized
from one and fed by the other.

The blast radius was wider than the test:

- **M1's topology service reported the wrong output dimensions** on any scaled
  display, and `GPU_HANDLING.md` recorded 1536x864 as a measured fact.
- Any consumer sizing itself from that rectangle would reject frames, or worse
  rescale silently, which SPEC.md §4.4 forbids.
- The recorded resolution would have depended on the user's scaling setting — a
  laptop at 150% would have produced a 1280x720 recording of a 1080p panel.

### Fix

`fc::set_process_dpi_awareness()` (`util/dpi_awareness.h`), calling
`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`,
resolved dynamically with a `SetProcessDPIAware` fallback. Called as the first
statement of `main()`, before anything touches DXGI.

The GPU test binary needed its own `main` for the same reason: a fixture's `SetUp`
runs after earlier tests in the binary have already queried DXGI, so
`gpu_test_main.cpp` sets awareness before `RUN_ALL_TESTS`.

The shipping executable should additionally carry a manifest declaring this, since
a manifest applies before any code runs. Noted for the M10 packaging work.

### Regression test

`CaptureToNv12Test.CapturedFramesReachDiskAsNv12AndAreNotBlack` fails with "no
frames were captured at all" without the fix, on any display that is not at 100%
scaling. `GpuTopologyTest.EveryAttachedOutputResolvesBackToItsOwningAdapter` now
observes true physical dimensions.

A dedicated assertion that the reported output size equals the physical mode would
be stronger, and is worth adding when the source resolver lands — it would catch
this without needing a capture session at all.

### Lessons

1. **A screen recorder must declare DPI awareness before it asks any question about
   the screen.** Every size Windows reports to a DPI-unaware process is a polite
   fiction, and nothing in the API surface hints at it.
2. **The converter's refusal was the design working.** It was sized for one
   resolution and handed another, and it returned `CAPTURE_RESOLUTION_CHANGED`
   instead of rescaling — exactly what §4.4 demands. A more "forgiving" converter
   would have silently produced a stretched recording and nobody would have looked
   for a DPI bug.
3. **A test that swallows an expected-looking error can hide the real one.** The
   `continue` on `CAPTURE_RESOLUTION_CHANGED` was there for a legitimate case (a
   genuine mid-test resolution change) and it converted a hard failure into a
   confusing zero. The assertion that saved it was the unrelated
   `ASSERT_GT(written, 0)`.

---

## [BUG-003] `write_artifacts` was `noexcept` but allocated, so the crash handler could terminate inside itself

**Severity:** Major   **Found:** 2026-07-27   **Fixed:** 2026-07-27   **Commit:** _uncommitted_

### Symptom

No observed failure. Found by the first clang-tidy run over the tree, once a
compile database existed:

```
crash_handler.cpp:326: an exception may be thrown in function 'write_artifacts'
                       which should not throw exceptions [bugprone-exception-escape]
```

### Investigation

1. Read the flagged lines. `write_artifacts` is declared `noexcept` and ends by
   populating its return value:

   ```cpp
   result.minidump    = std::filesystem::path{dump_path.data()};
   result.ring_log    = std::filesystem::path{ring_path.data()};
   result.report_json = std::filesystem::path{report_path.data()};
   ```

2. `std::filesystem::path` owns a string. Each construction allocates and can throw
   `std::bad_alloc`. In a `noexcept` function that is an immediate `std::terminate`.
3. Checked the rest of the function: the path building, the ring dump, the minidump
   and the JSON report were all already allocation-free by design, using fixed
   `std::array<wchar_t>` buffers and Win32 calls. The allocation was confined to
   the last three lines -- the *return value*, not the work.

### Root cause

`ArtifactPaths` was designed for the convenience of the test that reads it, and the
same type was then used as the crash path's output. Its `std::filesystem::path`
members made allocation unavoidable at the exact moment the file's own header says
allocation is forbidden:

> "The crash path must not construct a std::filesystem::path, format a std::string,
> or take the logger's lock: any of those can fail or deadlock in exactly the
> situation this code exists to survive."

The comment was right and the code contradicted it. Worse than a plain
`noexcept` violation: a crash caused by heap corruption or exhaustion is precisely
when these constructions fail, so the failure mode was "the crash handler dies
while handling the crash, producing no artifacts at all".

### Fix

Split the return type by audience.

- `ArtifactBuffers` holds three fixed `std::array<wchar_t, 1024>` buffers. No
  allocation, so `write_artifacts_into(Kind, void*, ArtifactBuffers&) noexcept` is
  genuinely `noexcept` rather than nominally so. All four handlers call it.
- `ArtifactPaths write_artifacts(Kind, void*)` is retained as a reporting wrapper
  for tests and tooling. It allocates, and is therefore **not** `noexcept`, with a
  header comment saying it must never be called from a crash context.

`kMaxPath` in the .cpp is now defined as `ArtifactBuffers::kCapacity` so the two
sizes cannot drift.

### Regression test

The existing `CrashFixture` suite covers behaviour and continues to pass unchanged
against the wrapper. The `noexcept`-correctness itself is enforced by clang-tidy's
`bugprone-exception-escape`, which is now part of `scripts/lint.ps1` and runs in
about two minutes. That is the real regression guard: a future edit that puts an
allocation back on the crash path fails the lint gate.

### Lessons

1. **A `noexcept` specifier is a claim, not a mechanism.** Nothing in the compiler
   checks that the body can honour it; it just converts a throw into a terminate.
   The only enforcement here was a static analyser nobody had run yet.
2. **Return types leak requirements.** The allocation was not in the logic, it was
   in the convenience of the type being returned. Designing the output for the test
   that reads it, then reusing that type on the constrained path, is how the
   constraint got violated.
3. **A comment describing an invariant is not the invariant.** This file documented
   the no-allocation rule in three places and still broke it. Making the buffers
   fixed-size moved the rule from prose into the type system.

---

## [BUG-002] `log::flush()` did not flush, and shutdown could drop the tail of the log

**Severity:** Major   **Found:** 2026-07-27   **Fixed:** 2026-07-27   **Commit:** _uncommitted_

### Symptom

Ten of the new M0b logging tests failed non-deterministically. `ring_snapshot()`
returned an empty vector immediately after `FC_LOG_INFO(...)` followed by
`fc::log::flush()`. Some tests in the same fixture passed — the ones that happened
to log from a worker thread and then `join()` it, which incidentally gave the async
worker time to catch up.

A ctest run also hung once in `LoggerFixture.InitCreatesTheDirectoryAndFile` at
~0% CPU; it did not reproduce after the fix.

### Investigation

1. Assumed a test bug first. One *was* present and was fixed separately:
   `spdlog::details::log_msg` stores a `string_view` over the payload, so
   `log_msg{..., "entry" + std::to_string(i)}` leaves a dangling view once the
   temporary dies. That produced the corrupted `\0ntry12` in one failure — but it
   did not explain the *empty* snapshots.
2. Noted the pattern: passing tests logged from a thread that was then joined;
   failing tests logged and read on the same thread. That is a race, not a logic
   error.
3. Read spdlog's own source rather than trusting the API's name:

   ```cpp
   void SPDLOG_INLINE thread_pool::post_flush(async_logger_ptr &&worker_ptr,
                                              async_overflow_policy overflow_policy) {
       post_async_msg_(async_msg(std::move(worker_ptr), async_msg_type::flush), overflow_policy);
   }
   ```

   No promise, no future, no wait — **in any overflow policy.**

### Root cause

`spdlog::async_logger::flush()` only *enqueues* a flush request. Our
`fc::log::flush()` looped over the per-subsystem loggers calling it and returned
immediately, while documenting itself as "blocks until queued messages reach their
sinks". It did not block at all.

Two consequences, the second much worse than the failing tests:

- Reading the ring straight after logging was a race.
- `shutdown()` called the same non-blocking flush and then destroyed the loggers,
  the thread pool and the sinks. The tail of the log — the part you actually need
  after a failure — could be discarded, and the worker could still be inside
  `sink_it_` on sinks that were being freed underneath it. That is the likely
  explanation for the one-off hang.

An initial fix idea — a second logger with `async_overflow_policy::block` used only
as a barrier — was discarded once the source above showed the policy is not
consulted for flushes.

### Fix

A real barrier built from counters rather than from spdlog's flush:

- `RingSink` counts completed writes in an atomic, incremented *after* the entry is
  in the buffer (release ordering).
- The logger counts submissions.
- `flush()` waits until `ring->processed() + pool->overrun_counter() >= submitted`,
  bounded at 2 s, then flushes the sinks directly from the calling thread. Dropped
  messages are counted via the overrun counter, or the wait would always time out
  under backpressure.
- The ring sink is now registered **last**, so "the ring wrote message N" implies
  every earlier sink did too.
- The thread pool moved from a function-local static into the logger's state, and
  `shutdown()` now tears down in an explicit order: drain, loggers, pool (its
  destructor joins the worker), then sinks.

The producer path keeps `overrun_oldest`, so nothing here can block the capture
thread (CLAUDE.md §4).

### Regression test

`LoggerFixture.SessionIdIsStampedOnEveryLine`,
`EveryLineCarriesTheMandatoryStructuredFields`, `RingHonoursItsConfiguredCapacity`
and `PreambleMarksUnimplementedFieldsAsPending` all log and then immediately read
the ring; they fail deterministically without the barrier.
`CrashFixture.RingBufferIsFlushedToDiskOnCrash` covers the same property through
the crash path.

### Lessons

1. **A function named `flush` is not evidence that anything was flushed.** The one
   thing that settled this was reading `thread_pool-inl.h`. For an asynchronous
   sink, "did this API wait?" has to be checked, not assumed.
2. **A test that passes for an accidental reason is worse than one that fails.**
   `ThreadNameIsTheProducerNotTheLogWorker` passed only because `join()` bought the
   worker time. Had every logging test been written that way, the shutdown data
   loss would have shipped.
3. Losing the end of the log is the worst possible thing for a logger to do,
   because that is the part that describes the failure.

---

## [BUG-001] D3D11 encoder-input frame pool rejected unless `BindFlags` includes `D3D11_BIND_DECODER`

**Severity:** Major   **Found:** 2026-07-27   **Fixed:** 2026-07-28   **Commit:** _uncommitted_

> The original diagnosis — that `DECODER` was an AMD-specific requirement — was
> wrong, and the follow-up section at the end of this entry corrects it. The
> sections between here and there are left as they were written.

### Symptom

Allocating an `AV_PIX_FMT_D3D11` / NV12 frame pool (1920×1080, `initial_pool_size`
20) on the Radeon 780M failed at `av_hwframe_ctx_init`:

```
[AVHWFramesContext] Could not create the texture (80070057)
FATAL: av_hwframe_ctx_init failed: Unknown error occurred (-1313558101)
```

The D3D11 device itself was created successfully on the intended adapter, and the
`AV_HWDEVICE_TYPE_D3D11VA` device context initialised without complaint. The error
named no field and no parameter. Encountered in
`scripts/spikes/amf_zerocopy`, which was measuring something else entirely.

### Investigation

1. Confirmed adapter selection was correct — DXGI enumeration printed
   `AMD Radeon(TM) 780M` and the device was created against that `IDXGIAdapter1`.
2. Decoded the status: `0x80070057` is `HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER)`,
   i.e. `E_INVALIDARG` out of `CreateTexture2D`, raised inside FFmpeg's
   `d3d11va_frames_init`. `-1313558101` is the same value wrapped as an AVERROR.
3. Set `AVD3D11VAFramesContext.BindFlags = D3D11_BIND_RENDER_TARGET` — the
   conventional choice for an encoder input surface, and what the NVENC samples use.
   **Still `E_INVALIDARG`.** This is the point where guessing stopped being productive.
4. Replaced the guess with a probe: call `CreateTexture2D` directly for each
   candidate flag combination against an `ArraySize = 20`, `DXGI_FORMAT_NV12`,
   `D3D11_USAGE_DEFAULT` array, and print the matrix.
5. Result on this part:

   | `BindFlags` | Result |
   | --- | --- |
   | `RENDER_TARGET` | rejected, `0x80070057` |
   | `SHADER_RESOURCE` | rejected, `0x80070057` |
   | `SHADER_RESOURCE \| RENDER_TARGET` | rejected, `0x80070057` |
   | `DECODER` | **accepted** |
   | `0` (the value FFmpeg passes when the field is untouched) | rejected, `0x80070057` |

### Root cause

FFmpeg's D3D11VA frames pool copies `AVD3D11VAFramesContext.BindFlags` straight into
`D3D11_TEXTURE2D_DESC.BindFlags` and supplies no default of its own. Leaving the
field at its zero-initialised value is therefore not "let FFmpeg decide" — it is
"request a texture array with no bind flags", which the AMD driver rejects.

On the 780M the only accepted value for an NV12 texture array is
`D3D11_BIND_DECODER`, including when the array is an *encoder input* pool. That is
counter-intuitive, and it is the opposite of what the conventional `RENDER_TARGET`
choice predicts.

### Fix

Spike: `probe_pool_bind_flags()` in `scripts/spikes/amf_zerocopy/main.cpp` queries
the device for an accepted combination at runtime and feeds the result to
`AVD3D11VAFramesContext.BindFlags`. The probe matrix is printed on every run, so a
future driver revision that changes the answer is visible rather than silent.

**Engine fix is open.** When M3 builds the real encoder input pool it must set this
field deliberately, and it must resolve the value **per adapter** — this result is
from the 780M only and has not been checked on the RTX 4050. Hard-coding
`D3D11_BIND_DECODER` as a constant would be repeating the mistake in the other
direction.

_(Closed 2026-07-28 — see the follow-up below.)_

### Regression test

`test_d3d11_encoder_pool_bindflags` (gpu tier) — **not yet written.** Must assert
that an NV12 encoder-input pool initialises on *every* adapter that advertises an
H.264 encoder, and must exercise each adapter separately rather than assuming one
value generalises. Until this test exists and passes, the M3 encoder path cannot be
called done (CLAUDE.md hard rule 1).

_(Written 2026-07-28 as three tests — see the follow-up below.)_

### Lessons

1. **A zero-initialised field in an FFmpeg hardware context is a request, not a
   default.** `AVHWFramesContext` and its `hwctx` have caller-owned fields that
   FFmpeg passes through untouched. "I did not set it" and "FFmpeg will pick
   something sensible" are different things, and only the first one is true.
2. **The most-documented flag for a use case is not necessarily the one the driver
   accepts.** Two plausible guesses failed before the probe found the answer in one
   run. When an API takes device-dependent capability flags, ask the device.
3. **`E_INVALIDARG` from `CreateTexture2D` names nothing.** A generic invalid-argument
   status on a struct with a dozen fields is a time sink. The probe is now permanent
   diagnostic infrastructure precisely because the error message will never improve.

### Follow-up — 2026-07-28: it was never an AMD quirk, and M3 is not blocked

Re-probed both adapters before starting M3, across the full cross product of bind
flags and `ArraySize ∈ {1, 4}`. Two results, both of which change what this entry
concluded.

**1. `DECODER` is required by `ArraySize > 1`, not by AMD.** The RTX 4050 behaves
identically to the 780M:

| `BindFlags` | `ArraySize = 1` | `ArraySize = 4` |
| --- | --- | --- |
| `UNORDERED_ACCESS` | accepted | rejected, `0x80070057` |
| `RENDER_TARGET` | accepted | rejected, `0x80070057` |
| `SHADER_RESOURCE` | accepted | rejected, `0x80070057` |
| `DECODER` | accepted | **accepted** |
| `DECODER \| UNORDERED_ACCESS` | accepted | **accepted** |

The original probe only ever tested `ArraySize = 20`, and `DECODER` was first in its
candidate list, so it won and the result read as adapter-specific. It is not: a
texture *array* of a video format requires `D3D11_BIND_DECODER` on both vendors,
while a single texture of the same format requires nothing in particular. The
"per adapter, never portable" warning this entry left in `adapter_info.h` was
therefore wrong, and has been corrected.

The per-adapter probe stays regardless — the right answer for the wrong reason is
still worth keeping, and lesson 2 above is unaffected.

**2. `DECODER | UNORDERED_ACCESS` is accepted, with planar UAVs creatable over a
pool slice.** This was the open M3 design question: the encoder input pool wanted
`DECODER` while `Nv12Converter` writes through `UNORDERED_ACCESS`, and the two
looked mutually exclusive. They are not, on either adapter. The conversion shader
can write NV12 straight into an encoder pool slice, so capture-to-encode is
zero-copy and the device-to-device copy that looked mandatory is not needed.

Note that accepting the texture is only half the question. `CreateTexture2D`
succeeding with `UNORDERED_ACCESS` does not guarantee the two planar views exist,
so the probe now creates the `R8_UNORM` and `R8G8_UNORM` UAVs as well and only then
reports the flag as usable. (D3D11 selects the plane by view *format*; `PlaneSlice`
is D3D12-only.)

**Engine fix is now closed.** `probe_nv12_pool_bind_flags` asks for
`DECODER | UNORDERED_ACCESS` first and falls back to `DECODER` alone, so hardware
that refuses the combination degrades to a copy rather than failing.
`EncoderCapability::nv12_pool_is_uav_writable()` reports which happened.

### Regression test — closed

Three tests, all `gpu` tier, replacing the unwritten `test_d3d11_encoder_pool_bindflags`:

- `GpuTopologyTest.AProbedEncoderNamesItselfAndItsPoolBindFlags` — every adapter
  advertising an encoder resolves non-zero pool flags, and they include `DECODER`.
- `GpuTopologyTest.AnEncoderInputPoolCanAlsoBeWrittenByTheConversionShader` — the
  zero-copy precondition. `EXPECT`, not `ASSERT`: an adapter that refuses is a
  slower supported configuration, not a failure, and the message says so.
- `GpuTopologyTest.VideoFormatTextureArraysRequireTheDecoderBindFlag` — pins the
  `ArraySize` finding, including the `ArraySize = 1` case that would have misled a
  single-texture probe.

### Lesson added

4. **A probe that stops at the first success measures the candidate order, not the
   hardware.** Ordering `DECODER` first and returning immediately produced a true
   answer with a false explanation, and that explanation then propagated into a
   header comment and a test comment as fact. When a probe exists to explain
   hardware behaviour, sweep the matrix — the cost is milliseconds and the
   alternative is a plausible wrong model that survives two milestones.
