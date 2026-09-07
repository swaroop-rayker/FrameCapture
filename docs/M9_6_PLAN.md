# M9.6 — Overlay, Notifications, Hotkeys, Menus & CI

**Status:** All six phases complete. Phase 5's CI has never run on GitHub and Phase 6's
CI cache timings are therefore unmeasured -- both need a push (M9_6_CHANGES.md §9).
**Sits:** after M9.5 (Tier B, complete), before M10 (installer, updater, acceptance suite).
**Owner sign-off:** all three §0.3 questions are now answered.
**1** — rows 19-22 **do** join SPEC.md §20, and §25's "all 18" becomes "all 22"
(2026-09-06; Phase 6 makes the edit).
**2** — **three separate bindings**, with `start` and `stop` sharing a default so the
familiar toggle survives (answered during Phase 3).
**3** — the recording pill is **on by default**, gated on the exclusion probe (2026-09-06).

This is a mini-milestone in SPEC.md §24's sense: it has an exit criterion, it adds rows to
§20's failure-mode matrix, and no part of it is done until its named test is green
(CLAUDE.md §2.1).

> **What has actually been built, and what it measured, is in
> [`M9_6_CHANGES.md`](M9_6_CHANGES.md).** Its §6 carries the verification status: as of
> 2026-09-06 every gate is green on the rig, including both forms of the GPU tier and the
> engine-backed pytest, which had gone unproven since Phase 0's C++ changes.

---

## 0. What this is, and the two rules it must not break

Five workstreams, three of which put pixels on the user's screen *while the screen is being
recorded*. That single fact is what makes this milestone different from every one before it:

> **Rule A — nothing this milestone draws may appear in the output file.**
> Not as the overlay, and not as the black rectangle that is the naive way of hiding it.
> A frame containing either is a failed recording, and CLAUDE.md §1's prime directive means
> a failed recording is a failed change.

> **Rule B — nothing this milestone draws may block the GUI thread.**
> The pill is the only UI on screen during a fullscreen recording. A pill that does not
> repaint is indistinguishable from a crashed recorder, and the user's response to a crashed
> recorder is to kill it — which is how this milestone loses someone's footage.

Both rules have a mechanism, and both mechanisms are Phase 0. Nothing else starts until they
are verified on the rig.

### 0.1 In scope

| # | Deliverable |
|---|---|
| F1 | Floating recording pill — capture-excluded, stop / pause-resume, elapsed, file size, save progress |
| F2 | Toast notifications — capture-excluded, top-right, stacked, for state changes, warnings, errors, crashes |
| F3 | Settings → Hotkeys: rebindable start / stop / pause-resume, with working conflict reporting |
| A1 | Edit / View / Tools menus — currently three empty `addMenu` calls (`main_window.py:73-75`) |
| A2 | GitHub Actions CI/CD — the repo has no `.github/` at all |

### 0.2 Explicitly out of scope

- **No new capture, encode, mux or audio behaviour.** The engine's media path is not touched
  by F1–F3 except for one additive event (§2.1) and one progress callback threaded through
  the existing finalization path.
- **No release/publish workflow.** See §7.4 — it is blocked on the GPLv2 consequence in
  CLAUDE.md §9 and belongs to M10 with the installer.
- **No light theme, no tray icon, no in-window log viewer.** Adjacent, not asked for, and
  CLAUDE.md §7 bans scaffolding ahead.
- **No 4-hour soak.** That is M10's, per SPEC.md §20.1.

### 0.3 Open questions for the owner — do not guess (CLAUDE.md §9)

1. ~~**§20 gains four rows (19–22).**~~ **Answered 2026-09-06 by the owner: the rows join
   §20**, which becomes twenty-two rows, and §25's Definition of Done becomes "all 22".
   One contract, one renumbering — not a separate table. Phase 6 makes the edit.
2. **Hotkey actions: three, or the existing two?** The request names *start, stop, and
   play/pause* as three separate bindings. The shipped default set (`hotkeys.py:203`) is two
   — a combined `start_stop` toggle and `pause_resume`. §16.5 says "start/stop/pause", which
   reads either way. This plan assumes **three distinct bindings**, with the old defaults
   preserved for muscle memory (§5.2). Confirm.
3. ~~**Is the pill on by default?**~~ **Answered 2026-09-06 by the owner: yes**, conditional
   on the exclusion probe passing (§1.3) — so it can never appear in a recording. `overlay.pill_enabled`
   stays `true`, and View ▸ Show recording pill turns it off per user, persistently.

---

## 1. Phase 0 — Foundations (nothing else starts until this is green)

Three items. Two are the mechanisms behind Rules A and B; the third is the contract change
everything downstream reads. **P0.3 is front-loaded deliberately: it is the only unknown in
this milestone whose answer could change the design of two features.**

### 1.1 P0.1 — The capture-exclusion primitive, and its probe

**Mechanism:** `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` — value `0x11`,
introduced in **Windows 10 2004, build 19041**, which is *exactly* SPEC.md §1's minimum
supported build. There is therefore no OS-version fallback to write: every platform this
project supports has the API. What is not guaranteed is driver/WDDM behaviour on the DDA
path, which is what P0.3 measures.

**The named bug, and its exact cause.** "A pill-shaped or box-shaped black cutout in the
recorded video" is not a mystery — it is `WDA_MONITOR` (`0x01`), the *older* affinity flag,
which tells DWM to render the window to the monitor and paint **black** into every capture
surface. It is one hex digit away from the correct value and it is what a search result from
before 2020 will hand you. Encoded as a banned pattern in `scripts/lint.ps1` (§7.5) so it
cannot be reintroduced quietly.

**Module:** `gui/framecapture_gui/overlay/exclusion.py`. One place, used by the pill and the
toasts alike — two independent call sites is how "the pill is excluded and the toast is not"
becomes a shipped bug.

```python
class ExclusionSupport(StrEnum):
    EXCLUDED      # the affinity was accepted and verified by read-back
    UNSUPPORTED   # the call failed; the window would be captured
    UNKNOWN       # not yet probed

def exclude_from_capture(window: QWindow | int) -> ExclusionSupport: ...
def guard(widget: QWidget) -> None:   # applies now and re-applies on WinIdChange
def probe() -> ExclusionSupport:      # a throwaway 1x1 window, at startup
```

**Five things that must be in the implementation, each of which is a way to ship an overlay
that lands in the file:**

1. **Every top-level HWND, not just the pill's.** Qt gives tooltips, context menus and
   combo-box popups their own native windows. A right-click menu on the pill is a separate
   HWND with default affinity, and it will be recorded. Handled by an application-level
   `QEvent.Show` filter that stamps any window carrying the `fc_overlay` dynamic property,
   plus every transient child of one.
2. **Re-apply on `QEvent.WinIdChange`.** Affinity is a property of an HWND, and Qt destroys
   and recreates native handles on `setWindowFlags`, on reparenting, and on some
   screen-change paths. The affinity does not survive; the window stays on screen. This is
   the failure mode that appears only after the user drags the pill to a second monitor.
3. **Check the return value.** `SetWindowDisplayAffinity` returns `BOOL`; a failure must be
   read back with `GetWindowDisplayAffinity` and reported, never assumed. The Python side
   mirrors CLAUDE.md §4's `FC_HR` discipline: no unchecked Win32 call.
4. **Never stamp the main window.** A user may legitimately want to record FrameCapture's own
   window. Exclusion applies to overlay surfaces only, and the set of overlay surfaces is
   explicit rather than "everything that is frameless".
5. **Order matters:** apply after the native handle exists (`show()` or `winId()`), before
   the first paint. Applying in `__init__` is a silent no-op.

### 1.2 P0.2 — Asynchronous commands (Rule B)

**The defect, located.** `IpcClient.request` blocks the calling thread until the reader
matches a response (`ipc/client.py` module docstring), and `EngineController.stop_recording`
calls it **on the GUI thread** (`engine.py:176`) with SPEC.md §15.1's 30-second timeout. The
engine's own measurements (ACCEPTANCE.md, BUG-046) put a 1.2 GB recording's finalization at
**~3.3 s of remux plus ~1 s of validate**. So on every stop of a long recording the GUI event
loop is dead for multiple seconds:

- the save-progress bar this milestone is adding **cannot paint** — it would be a frozen
  widget, which is worse than not having one;
- `WM_HOTKEY` sits unread in the thread queue, so **hotkeys "stop working"** for the duration
  — one of the three named bugs in this request, and it has nothing to do with hotkey code;
- `pause`/`resume`/`start` block for up to 5 s each on a busy engine, which is the "delay or
  latency after pressing a button" bug in its ordinary form.

> **NARROWED, 2026-09-05.** Only `stop_record` was made asynchronous. `pause_record` and
> `resume_record` set a flag on the pause clock — no I/O, microseconds — and their worst
> case is a press landing during a `start_record`, which SPEC.md §5.4 already budgets at
> 350 ms. Making them async would have cost the ability to report a refusal
> synchronously, which `test_engine_control` asserts on. Trading a proven assertion for
> an unmeasured stall is the wrong way round; revisit if a measurement says otherwise.

**Fix.** Add a non-blocking form to the IPC client and route every *user-initiated* command
through it:

```python
def request_async(self, command, params=None, *, on_done=None, on_error=None) -> RequestHandle
```

One dedicated command thread (not a thread per click — that is an unbounded queue in disguise,
CLAUDE.md §2.5), a bounded backlog of 8, drop-newest with a logged warning on overflow.
Results marshal to the GUI thread through the Qt signal `EngineController` already uses for
events. The heartbeat and the small read-only queries (`get_stats`, `get_sources`,
`get_config`) stay synchronous — they return in microseconds and making them async buys
nothing but ordering bugs.

**And the part that is a UX rule, not a threading one:** every button enters a *pending*
visual state on the click, within one frame, before any IPC happens. The engine's answer
confirms or corrects it. A button that looks unpressed for 40 ms reads as broken no matter how
fast the round trip is.

### 1.3 P0.3 — Prove exclusion on the rig, on both capture paths

> **ANSWERED, 2026-09-05: `WDA_EXCLUDEFROMCAPTURE` works on WGC *and* DDA on this rig.**
> The fallback ladder below is therefore **not needed** and `advanced.capture_backend`
> does not have to be narrowed while an overlay is on screen. Measured, stable over four
> consecutive full-suite runs in one process:
>
> | case | backend | mean_luma | overlay_fraction |
> |---|---|---|---|
> | unstamped (control) | WGC / DDA | 145.8 / 145.8 | 1.0000 |
> | `WDA_MONITOR` | WGC | 0.0 | 0.0000 |
> | `WDA_EXCLUDEFROMCAPTURE` | WGC / DDA | 255.0 / 255.0 | 0.0000 |
>
> 145.8 is exactly the BT.709 luma of the test overlay's orange; 255.0 is the animator's
> white bar coming through intact. Getting here cost BUG-049 — the first version of the
> test was a flaky green that proved nothing. The ladder is kept below as the record of
> what the design would have been.

**This is the gate.** Written and run before any UI exists, because if the answer is "DDA does
not honour it", F1 and F2 need a different design and it is much cheaper to know now.

`tests/integration/test_overlay_exclusion.cpp` — **GPU tier**, label `gpu`.

It uses `ScreenAnimator` (`tests/fixtures/screen_animator.h`), the fixture that already exists
so that real-output capture tests own what is on screen rather than depending on the desktop —
which is how this test stays inside CLAUDE.md §5's "tests use the synthetic source, never the
real desktop" while still capturing a real output. The overlay under test is a plain Win32
window this test creates, not a Qt one: what is being measured is the Win32 behaviour, and
putting PySide6 in the path would make a failure ambiguous.

```
ScreenAnimator covers the output with its known pattern
  -> a 300x64 test window at a known rect, filled magenta (0xFF00FF), WDA_EXCLUDEFROMCAPTURE
  -> capture N frames through WGC, then again through DDA
  -> for the pixels under that rect, assert BOTH:
        (a) no magenta        — the overlay is not in the frame
        (b) the animator's pattern is intact, and mean luma > 0
                              — and it is not a black cutout either
```

**Assertion (b) is the one that matters and the one a naive test omits.** A test that only
checks "the overlay is not visible" passes on `WDA_MONITOR`, which is the exact bug being
guarded against. Both halves, on both paths, or the row is not green.

**Additional cases in the same file:**

- a **negative control**: the same window with affinity `WDA_NONE` *must* show magenta —
  otherwise the test proves the window was never on screen and every other assertion is
  vacuous;
- **`WDA_MONITOR` is asserted to produce the black cutout**, documenting the trap in
  executable form;
- a transient **child window** (a menu-alike) inherits nothing and must be stamped separately
  — asserts the §1.1 item-1 rule;
- **handle recreation**: destroy and recreate the HWND, re-stamp, re-assert.

**If DDA does not honour it** (possible; unmeasured), the fallback ladder, in order:

1. force WGC whenever an overlay is visible and the target is a display (WGC is already the
   preferred backend, §4.2 — this narrows `auto`, it does not remove DDA);
2. place the overlay on a monitor that is not being captured, with a one-time explanation;
3. no overlay: hotkeys and the main window only, with the reason stated in the settings dialog
   rather than a silent grey-out (§20 row 16's principle).

Toasts follow the same ladder, falling back to the existing status-bar path at rung 3.

**Exit criterion for Phase 0:** `test_overlay_exclusion` green on both paths; `stop_record`
issued from the GUI leaves the event loop responsive (asserted by a pytest that pumps and
counts paints during a stop); the new config keys and IPC event documented.

---

## 2. Contract changes (all additive, all in Phase 0)

### 2.1 IPC — one new event

`finalize_progress` (engine → GUI, unsolicited). Additive per SPEC.md §15.1's compatibility
rule: an older GUI ignores it, `Event.from_wire` already returns `None` for unknown names.

```json
{"event": "finalize_progress",
 "phase": "flushing|remuxing|validating|swapping|done",
 "percent": 0,
 "bytes_done": 0, "bytes_total": 0,
 "output": "D:\\...\\FrameCapture_....mp4"}
```

**Emitted from the finalization path, throttled to 10 Hz.** Safe to send while `stop_record`
is in flight: the pipe server handles one *request* at a time (`engine_service.cpp` — "this
thread reads the next request only after this one returns"), but `send_event` is a write and
serialises independently (`pipe_server.h:91`). **Verify this holds under a real 30 s stop
before relying on it** — it is read from the code, not yet measured.

**The percentages are weighted from measured cost, not invented.** BUG-046 gives remux at
**1.97 ms/MB + 8.8 µs/packet** and validate as a **fixed ~650–1000 ms** decode of the tail.
Those have different shapes, and a single linear bar over both is a bar that stalls at 85% for
a second on every recording:

| phase | progress source | MKV | MP4, 1.2 GB |
|---|---|---|---|
| flushing | none — brief | ~0 | ~0 |
| remuxing | `avio_tell(in) / source size`, real | n/a | ~3.3 s |
| validating | fixed estimate, labelled | ~1 s | ~1 s |
| swapping | none | ~5 ms | ~5 ms |

So the bar is **two-segment and phase-labelled**, and MKV — which has no remux — shows
"Validating…" and finishes, rather than a bar that snaps 0→100 and reads as a failure.

**Additionally:** `get_stats` already carries everything else the pill needs — `timeline_ms`,
`bytes_written`, `paused_total_ms`, `output`, `segments` (`engine_service.cpp:330-350`). No new
stats fields. One thing to **verify, not assume**: whether `bytes_written` is per-segment or
cumulative when segmentation is on. The pill must show the session total; if the field is
per-segment, sum GUI-side across `segment_rolled`.

### 2.2 Config — two new sections, one schema bump

Added to `config_schema.cpp`'s table, documented in `docs/CONFIG.md` in the same commit
(CLAUDE.md §10):

```
[hotkeys]   start "Ctrl+Shift+F9" · stop "Ctrl+Shift+F9" · pause_resume "Ctrl+Shift+F10"
            enabled true
[overlay]   pill_enabled true · pill_corner "bottom-right" · pill_monitor ""
            pill_x -1 · pill_y -1        (-1 = unplaced, use the corner)
            toasts_enabled true · toast_corner "top-right" · toast_duration_s 4
            toast_max_visible 4
```

Note `start` and `stop` defaulting to the *same* sequence: that is the existing combined
toggle, expressed as two identical bindings, and it is how current default behaviour survives
the change. Registering one hotkey for two actions is handled explicitly (§5.2), not by
accident.

`schema_version` 1 → 2, with `migrate_1_to_2` per SPEC.md §17: writes the new defaults, backs
up to `config.toml.bak.1`, preserves unknown keys. Trivial, and it still gets a case in
`tests/unit/test_config_migration.cpp` — the migration chain's value is that every link is
exercised, including the boring ones.

### 2.3 New §20 rows (subject to §0.3 question 1)

| # | Symptom | Root cause | Mitigation | Test |
|---|---|---|---|---|
| 19 | Overlay visible in the file, **or** a black rectangle where it was | `WDA_MONITOR` instead of `WDA_EXCLUDEFROMCAPTURE`; affinity lost on HWND recreation; popups/tooltips unstamped | §1.1 — one primitive, stamped on every overlay HWND and re-stamped on `WinIdChange` | `test_overlay_exclusion` (gpu) + `test_overlay_windows` (pytest) |
| 20 | Overlay button unresponsive, or clicking it minimizes a fullscreen game | GUI thread blocked in a synchronous IPC request; overlay takes activation | §1.2 async commands; `WS_EX_NOACTIVATE` + `Qt.WindowDoesNotAcceptFocus` | `test_overlay_responsiveness` (pytest) |
| 21 | A bound hotkey does nothing | Conflict reported once and lost; GUI thread blocked so `WM_HOTKEY` is never pumped; key absent from the VK table | §5.1 — persistent per-binding conflict state; §1.2; complete VK table | `test_hotkey_binding` (pytest) |
| 22 | Save progress never completes, or the pill closes before the file is written | Progress bar driven by a blocked event loop; pill closed on the command's return rather than on `recording_finalized` | §3.4 — the pill closes on the finalized event and validation, never on a timer | `test_finalize_progress` (gpu) |

---

## 3. Phase 1 — The recording pill (F1)  ✅ complete

**Built:** `overlay/pill.py` (the widget), `overlay/surface.py` (flags + no-activate +
exclusion in one call, shared with Phase 2's toasts), `overlay/placement.py` (corner
anchoring, edge snapping, off-screen recovery), `overlay/elapsed.py` (the interpolated
clock), `units.py` (shared formatting, which also fixed `1536.0 MB` in the status panel).

**Found while building:** BUG-050 — `setObjectName` replaces rather than adds, so the
stop button silently lost every style it shared with the pause button. Invisible to all
twenty-odd state assertions; caught by rendering the widget and looking at it.


**Where it lives:** the GUI process, PySide6, `gui/framecapture_gui/overlay/pill.py`. Not the
engine: the engine must survive the GUI's death (§3.1) and owning UI would invert that. Not a
second process: another process is another lifetime to reap.

**Budget:** §16.1 caps GUI CPU at 3% while recording. The pill repaints at **10 Hz maximum**,
only the regions that changed, and the recording dot's 1 Hz pulse (§16.3) is the only
animation. Everything else is static between updates.

### 3.1 Window construction — six properties, each load-bearing

| Property | Why |
|---|---|
| `Qt.Tool \| FramelessWindowHint \| WindowStaysOnTopHint \| WindowDoesNotAcceptFocus` | frameless, above other windows, out of Alt-Tab |
| `WS_EX_NOACTIVATE` (added natively) | **clicking the pill must not activate it.** Activating a window over a fullscreen-exclusive D3D app minimizes that app — a click on "pause" that minimizes the game being recorded is the worst bug available in this feature, and Qt's flags alone do not prevent it |
| `WS_EX_TOOLWINDOW` | no taskbar button |
| `WDA_EXCLUDEFROMCAPTURE` via §1.1 | Rule A |
| `WA_TranslucentBackground` + rounded paint | it is a *pill*; a rectangular window with a rounded drawing inside shows square corners against the desktop |
| per-monitor DPI awareness | the pill is 32 px tall on a 100% monitor and must be 32 px on a 150% one after being dragged there |

### 3.2 Contents and states

```
 ●  00:14:22   ·   412 MB   ·   [ ❚❚ ]  [ ■ ]        recording
 ❚❚ 00:14:22   ·   412 MB   ·   [ ▶ ]  [ ■ ]        paused
    Saving…  ████████████░░░░  73%                    stopping
```

- **Elapsed is `timeline_ms`** — the length the file will actually have, paused time excised
  (§7.5, §16.2). Not wall clock. Paused total is available on hover.
- **Updated by a local 100 ms timer that interpolates** from the last authoritative
  `timeline_ms` plus a monotonic delta, snapping to the authoritative value on each 2 Hz
  `stats`. Rendering only on `stats` gives a clock that visibly ticks twice a second;
  free-running gives a clock that drifts from the file. Both are wrong; interpolate-and-snap
  is not.
- **Size** from `bytes_written`: `MB` with one decimal below 1024 MB, `GB` with two above.
  Formatting lives in one function with a unit test — a size that reads `1024.0 MB` is a small
  thing that looks like a bug.
- **Paused is visually distinct** (§16.5): the dot stops pulsing and holds, the glyph changes,
  the pill's border takes `--warn`. A paused pill that looks like a recording one is how a
  user loses ten minutes.
- **Buttons:** pause/resume toggles on engine state, never on local guesswork — the engine's
  `state_changed` is the authority and the button reflects it. Both are idempotent engine-side
  (§7.5), so a double-click is harmless.
- Palette and radius from `theme/__init__.py`; no hard-coded colours (a colour set in Python
  is one a future light theme misses).

### 3.3 Placement and input

Draggable by its body, snaps to screen edges within 16 px, position persisted per-monitor.
Clamped back on-screen when a monitor is removed — a pill remembered at x=3000 on a display
that no longer exists is a pill the user cannot find, and the stop button with it. Default:
bottom-right of the captured monitor, 24 px inset.

### 3.4 The stop sequence — the part with a named bug

1. Click → button disables and reads "Saving…" **immediately**, same frame.
2. `stop_record` issued via `request_async` (§1.2). The event loop stays live.
3. `finalize_progress` events drive the two-segment bar with phase text.
4. **`recording_finalized` arrives.** Only now:
   - `valid: true` → bar to 100%, "Saved" + filename for **600 ms**, then close. The dwell is
     deliberate: a bar that vanishes at 99% leaves the user unsure the file exists.
   - `valid: false` → the pill **does not close**. It shows the failure and hands off to the
     main window's existing validation dialog. CLAUDE.md §1: a file that did not validate is
     the one thing a user must not discover later.
5. **The pill never closes on a timer, and never on the command's return alone.** Row 22.
6. Engine death mid-finalize → the pill says so and stays until dismissed; §10.4's recovery
   path owns the file.

### 3.5 Tests

| Test | Tier | Asserts |
|---|---|---|
| `test_overlay_exclusion` | gpu | Phase 0's, extended to the real pill widget |
| `test_overlay_responsiveness` | pytest | ≥ 8 paints/s during a mocked 5 s stop; click→visual-state < 1 frame; row 20 |
| `test_finalize_progress` | gpu | monotonic percent, every phase seen, `done` after `recording_finalized`, pill closes only then; row 22 |
| `test_pill_state` | pytest | every `RecordingState` maps to exactly one appearance; paused ≠ recording |
| `test_size_format` | pytest | boundaries at 1023.9 MB / 1024 MB / 1 GB |
| `test_pill_placement` | pytest | monitor removal clamps on-screen; DPI change rescales |

---

## 4. Phase 2 — Toasts (F2)  ✅ complete

**Built:** `overlay/toast_model.py` (severity, dwell, the bounded coalescing queue — pure,
so an error storm is a millisecond), `overlay/toasts.py` (`Toast` + `ToastManager`), and
`placement.stack_positions` alongside the pill's geometry. Reuses `overlay/surface.py`
wholesale, which is what the pill's phase existed to establish.

**Found while building:** BUG-052 — a toast animating toward a superseded slot finished
there and overlapped its neighbour. The pure geometry was correct and had 200 randomised
sequences behind it; the defect was in the layer that *applies* it, and reproduced only
when messages arrive with no event loop in between, which is exactly what a real burst
does.


Same exclusion primitive, same window flags, same non-activating rule. What is new is
**stacking**, which is the named bug and is a geometry-ownership problem.

`gui/framecapture_gui/overlay/toasts.py` — `ToastManager` (owns all geometry) + `Toast` (owns
none). **A toast never positions itself.** Two objects computing positions is exactly how
toasts overlap.

**Rules, each one a bug it prevents:**

- **One anchored column**, top-right of the chosen monitor, 24 px inset, 8 px gaps. Newest at
  top, older pushed down.
- **Max 4 visible**, the rest queued, **queue capped at 32, drop-oldest** — an error storm must
  not allocate unboundedly. The same discipline CLAUDE.md §2.5 requires of the engine's queues;
  a GUI is not exempt.
- **Coalesce identical messages within 2 s** into one toast with a `×N` badge. The burst case is
  real and already documented in `main_window.py:_on_notified` — a failed engine start emits an
  error, the OFFLINE transition prompts a refresh that fails and emits another.
- **Re-layout is animated** at 120 ms ease-out (§16.3) on both insert and removal, so a
  dismissal closes the gap instead of snapping.
- **Dwell by severity:** info 3 s, warning 6 s, **error until dismissed**. An error that
  disappears before it is read is an error that did not happen. Hover pauses the countdown.
- **Never activates, never focuses**, click dismisses, click-through elsewhere.
- **Monitor/DPI change re-anchors** the whole column.

**Sources:** `state_changed` (paused / resumed / stopped / saved), `warning`, `error`,
`degradation_changed`, `gpu_migrated`, `audio_device_migrated`, `segment_rolled`, and engine
death — for which the toast points at `crash_report.json` and the log directory (§18) with a
"Show details" action, non-modal.

**Relationship to what exists:** additive. The status bar keeps its role and the main window's
deliberate no-modals policy is unchanged. When exclusion is unavailable (§1.3 rung 3) toasts
route to the status bar instead of appearing on screen.

**Tests:** `test_toast_stacking` (no two visible toasts overlap, over 200 randomised
arrival/dismiss sequences — the property, not three hand-picked cases); `test_toast_queue`
(cap holds under a 1000-message storm, memory flat); `test_toast_coalesce`;
`test_toast_severity`; and toasts are included in `test_overlay_exclusion`.

---

## 5. Phase 3 — Hotkeys in settings (F3)  ✅ complete

**Owner's answer to §0.3 question 2: three separate bindings.** `start` and `stop` share
a default sequence, so the start/stop toggle FrameCapture shipped with survives untouched
and separating the two keys is a user choice.

**Built:** `hotkeys.py` rewritten around `Action`, `BindingStatus` and an idempotent
`apply` (the old `register` returned a list of conflict strings and was called once);
`settings/hotkey_editor.py` (the capture field and the section); the `Hotkeys` tab.

**Two decisions worth recording, both diverging from what §5.1 proposed:**

- **A refused rebind leaves its action unbound rather than keeping the old key alive.**
  §5.1 asked for a rollback. That was rejected once shared sequences existed: with
  `start` and `stop` on one key, "keep the old binding for the action that failed" means
  keeping a registration whose routing now belongs to the *other* action — so the status
  would claim an accelerator that no longer runs that action. A status that lies is worse
  than an unbound key the user is told about, and the dialog validates as they type, so a
  refusal at apply time is the rare race where another application took the key in
  between. New registrations are still taken before old ones are released, so a rebind
  never passes through a window where neither key works.
- **A shared sequence dispatches by predicate, not by callback list.** Each `Hotkey`
  carries an `applicable` test; the manager runs the first that says yes and stops. Firing
  every callback attached to a shared key would start and stop a recording in one press.

**Found while building:** `setChecked(False)` on an already-unchecked box emits nothing,
so loading a configuration with hotkeys switched off left the fields enabled under a
checkbox that said they were not. Caught by the test for it; the initial state is now
applied directly rather than left to the signal.


### 5.1 Why hotkeys don't work — five causes, all real, all addressed

1. **The GUI thread is blocked.** `WM_HOTKEY` is posted to the thread queue and read by the
   native event filter (`hotkeys.py:_HotkeyFilter`). A thread blocked in a synchronous
   `stop_record` does not pump, so *every* hotkey is dead for the duration. **Fixed in Phase 0,
   not here** — but it is the most likely cause of the reported symptom, and it is worth saying
   that the fix is not in the hotkey code.
2. **Conflicts are reported once and then lost.** `register()` returns the conflict list and
   `main_window.py:120` shows the first one in the status bar for 12 s. After that the user has
   a hotkey that silently does nothing and nowhere to look. → per-binding conflict state,
   persistently visible in the settings section, with the reason.
3. **The VK table is incomplete.** `_VIRTUAL_KEYS` covers F1–F24, A–Z and 0–9 and nothing else.
   `Ctrl+Alt+Home` is refused as "unsupported key". → extend with navigation keys, numpad,
   punctuation, `Pause`, `Insert`/`Delete`; keep the explicit-refusal design, since a
   silently-dropped key registers a *different* hotkey.
4. **Rebinding is not atomic.** Unregister-then-register leaves the user with *nothing* bound if
   the new combination is taken. → register the new binding under a fresh id **first**,
   unregister the old only on success, roll back and report on failure.
5. **Reserved combinations.** `Win`+key is largely the shell's; `Ctrl+Alt+Del` is
   unregisterable. → validated at bind time in the dialog, with the reason, not at startup.

### 5.2 The settings section

An eighth tab, `Hotkeys`, after `Advanced`. Three rows — **Start recording**, **Stop
recording**, **Pause / resume** — each with a capture field, a Clear, a Reset, and a status chip
reading `Bound` / `In use by another application` / `Unsupported key`. Plus a master `Enable
global hotkeys` checkbox.

**The capture widget** grabs the keyboard while arming, so pressing the combination during
capture does not fire the currently-bound global hotkey. It validates live: duplicates within
FrameCapture, modifier-less keys (already refused by `parse_sequence`), and reserved
combinations are all rejected **in the dialog**, before OK — a conflict discovered after
closing the dialog is the current behaviour and it is what this is fixing.

Two actions may share a sequence (start and stop both default to `Ctrl+Shift+F9`). That
registers **one** hotkey dispatching a toggle, decided by current engine state — handled
explicitly in `HotkeyManager`, because two `RegisterHotKey` calls for one combination is the
1409 error and would report a "conflict" against ourselves.

Every fire raises a toast (§4) — "Recording paused" — which is what turns a hotkey from
something you hope worked into something you saw work.

### 5.3 Tests

`test_hotkey_binding` (pytest): parse/format round-trip over the extended VK table; conflict
rollback leaves the *old* binding registered; duplicate detection; reserved rejection;
persistence round-trip through `save_config`/`get_config`; and **dispatch verified by posting a
real `WM_HOTKEY` to the thread queue** — that exercises the filter and the routing without
synthesising keystrokes. One `SendInput` smoke case on the rig covers the last inch.

---

## 6. Phase 4 — Edit / View / Tools (A1)  ✅ complete

Currently `menu.addMenu("&Edit")` ×3 with no actions — three menus that open empty
(`main_window.py:73-75`). Each gets real content; nothing gets a stub.

**Edit** — Settings… (`Ctrl+,`) · Copy output path · Copy diagnostics summary (versions,
adapters, encoder, last error — the thing a user pastes into a bug report). No Undo/Redo: there
is nothing to undo, and a greyed Undo is a lie.

**View** — Show preview · Show recording pill · Show notifications · Panels ▸ (Sources / Audio
Mixer / Controls / Status) · Always on top · Reset layout. Panel visibility persists. No "Light
theme (soon)" item — CLAUDE.md §7.

**Tools** — Open output folder · Open logs folder · **Export diagnostic bundle** (§18 names
this: a zip of logs + config + last minidump; `crash_handler.cpp:211` already places the dump as
a sibling of the log directory) · GPU topology… (renders `get_gpu_topology`, which the GUI
already fetches and never shows) · Log level ▸ trace…critical · Engine ▸ Restart / **Recover**.

**Two spec'd IPC commands the GUI has never called** — `recover` and `set_log_level` (§15.1) —
get their first call sites here. Changing log level from a menu, without a restart, is the
difference between reproducing a bug and asking a user to edit a TOML file.

**Tests:** `test_menu_actions` (pytest) — every action in every menu is enabled-or-explained and
triggers without raising; the parametrised sweep over `menuBar()` is what catches the next empty
menu.

---

## 7. Phase 5 — CI/CD (A2)  ✅ complete (locally verified; not yet run on GitHub)

**There is no `.github/` directory.** The remote is
`https://github.com/swaroop-rayker/FrameCapture`. SPEC.md §20.1 already specifies the shape:
*"GitHub Actions `windows-latest` for build + unit tests; a self-hosted runner on the reference
rig for all GPU/capture/soak tests (hosted runners have no real GPU and will silently pass
meaningless tests)."*

**This phase has no dependency on Phases 0–4 and can run concurrently from day one.**

### 7.1 `ci.yml` — PR and push to `main`, `windows-latest`

`lint` → `build` → `test`, with `concurrency: cancel-in-progress`.

- `scripts/lint.ps1` (clang-format, clang-tidy, the banned-pattern grep, ruff, mypy). The
  clang-tidy compile database is regenerated in CI — CLAUDE.md §3 notes it goes stale on a new
  source file, and a CI that silently tidies a stale database is a gate that passes nothing.
- `cmake --preset windows-msvc-release` + build.
- `ctest --preset windows-msvc-release -LE gpu --output-on-failure` — CPU tier only. **The GPU
  tier must never be scheduled on a hosted runner**; §20.1 is explicit that it would pass
  meaninglessly.
- `pytest gui/tests -v` with `QT_QPA_PLATFORM=offscreen`.
- Artifacts on failure, always: ctest logs, `LastTest.log`, pytest junit XML.

**The one thing that decides whether CI is usable: the vcpkg binary cache.** A cold build of
this manifest compiles FFmpeg with x264 from source. Uncached, every PR is an hours-long job and
CI gets ignored, which is worse than no CI. Use the NuGet/GitHub Packages binary-cache backend
keyed on `vcpkg.json` + baseline, with `actions/cache` on the Python venv. **Measure the cold
and warm times and record both in `docs/TESTING.md`** — an unmeasured cache is a cache nobody
notices has stopped working.

### 7.2 `gpu.yml` — self-hosted, on the reference rig

`runs-on: [self-hosted, windows, framecapture-rig]`. Nightly + `workflow_dispatch`. Full `ctest`
including `-L gpu`. Runner setup documented in `docs/TESTING.md` (§22 already requires "how to
set up the self-hosted GPU runner"). Timeout 90 min; `if: always()` artifact upload; **explicitly
not triggered by pull requests from forks** — a self-hosted runner executing fork code is
arbitrary code execution on the owner's machine.

Note in the workflow, next to the `RealTargetTest` cases, that this tier **plays audible tones
through the speakers** (CLAUDE.md §3) — a runner on someone's daily-driver laptop making noise
at 03:00 is a surprise worth one comment.

### 7.3 `soak.yml` — self-hosted, weekly + manual

The long forms, driven by the environment overrides CLAUDE.md §3 already defines:
`FC_AV_SYNC_SECONDS=1800`, `FC_LOOPBACK_SECONDS=1800`, `FC_SUSTAINED_SECONDS=600`,
`FC_MULTITRACK_SECONDS=1800`. **The 4-hour soak stays M10's** (§20.1) — the workflow carries the
job, disabled, with the reason in a comment.

### 7.4 What is deliberately *not* built

**No release/publish workflow.** CLAUDE.md §9: libx264 is in, `--enable-gpl` is set, and the
distributed binary is therefore **GPLv2** — which obliges source availability for the distributed
work and is incompatible with a proprietary licence. A workflow that publishes binaries before
that is settled would be automating a licensing decision nobody has made. Packaging is M10's; the
licence question is the owner's. CI does add one cheap gate: **fail if `LICENSE` is absent**, so
the question cannot be forgotten.

### 7.5 Two gates worth having

- **Banned-pattern grep**, already implemented at `lint.ps1:178`, now enforced on every PR rather
  than on whoever remembers to run it. **`WDA_MONITOR` joins the list** (§1.1).
- **Docs merge-gate** (§22: "kept current as a merge-gate requirement"): a change touching
  `engine/core/config/` must touch `docs/CONFIG.md`; a change adding an `FcError` must touch
  `docs/ERROR_CODES.md`. A path-pair check, ~20 lines, and it is the difference between §22 being
  a policy and being enforced.

---

## 8. Phase 6 — Acceptance and documentation  ✅ complete

Not a formality; CLAUDE.md §10 makes these graded deliverables.

- `docs/SPEC.md` — §16.2's layout gains the pill and toasts; §16.4 gains the Hotkeys section;
  §16.5's hotkey bullet points at it; §20 gains rows 19–22 (pending §0.3).
- `docs/ACCEPTANCE.md` — a section per row, in the established form: what the clause turned into,
  what was measured, what it does *not* cover.
- `docs/CONFIG.md` — `[hotkeys]` and `[overlay]`, same commit as the schema.
- `docs/IPC_PROTOCOL.md` — `finalize_progress`, with an example exchange.
- `docs/TESTING.md` — the self-hosted runner, and the measured cold/warm CI times.
- `docs/ENGINEERING_LOG.md` — an entry per non-trivial bug found, in §22.1's form.
- `CHANGELOG.md` — under Unreleased, in the user-facing voice the M9.5 entry already uses.

**Report measurements, not adjectives** (CLAUDE.md §6). The numbers this milestone owes: overlay
paint cost (ms/frame) and GUI CPU during recording against §16.1's 3%; click→visual latency;
click→engine-state latency; finalize-progress cadence against the 10 Hz target and the ~3.3 s +
~1 s the remux and validate actually cost.

---

## 9. Exit criterion

> Rows 19–22 green on the reference rig. A recording made with the pill and toasts on screen the
> whole time, decoded frame by frame, contains **neither the overlay nor a black region where it
> was**, on the WGC path and the DDA path. Stop is driven from the pill, the progress bar advances
> while it happens, and the pill closes only after the file is written and validated. All three
> hotkeys are rebindable from the settings dialog, survive a restart, and fire while the engine is
> finalizing. Every Edit / View / Tools action does something. `ci.yml` is green on a PR from a
> clean clone, and `gpu.yml` has run green on the rig at least once.
>
> Both test tiers and `lint.ps1` green, as CLAUDE.md §3 requires before any of this is reported
> done.

---

## 10. Risks, ranked

| Risk | Impact | Handling |
|---|---|---|
| **DDA does not honour `WDA_EXCLUDEFROMCAPTURE`** | F1 and F2 need redesign | Measured in Phase 0, before any UI exists; ladder in §1.3 |
| **Async IPC introduces an ordering bug** | worse than the blocking it replaces | One command thread, bounded backlog, engine state always the authority over local optimism |
| **Overlay pushes GUI CPU past §16.1's 3%** | a spec violation caused by a convenience | 10 Hz cap, region repaints, measured not assumed |
| **CI cold-build time makes CI ignored** | the gate exists and nobody waits for it | Binary cache is a Phase-5 acceptance item with a recorded number, not an optimisation |
| **Fullscreen-exclusive apps cover the pill** | user cannot see or click it | Documented limitation; hotkeys are the affordance, and every hotkey raises a toast |
| **Four new §20 rows change §25's "all 18"** | a documentation contract shifts | §0.3 question 1, answered before Phase 0 ends |

---

## 11. Phase order and dependencies

```
Phase 0  Foundations  ── exclusion primitive · async IPC · finalize_progress · config schema
   │                     GATE: test_overlay_exclusion green on WGC *and* DDA
   ├── Phase 1  Pill        (needs P0.1 exclusion, P0.2 async, P0.2's finalize_progress)
   │      └── Phase 2  Toasts    (needs P0.1; reuses the pill's window scaffolding)
   │             └── Phase 3  Hotkeys   (needs P0.2 for dispatch; toasts for feedback)
   ├── Phase 4  Menus       (needs Phase 1/2 only for the View toggles)
   └── Phase 5  CI/CD       — independent, start immediately, runs in parallel throughout
Phase 6  Acceptance + docs  — continuous, closed out at the end
```

One phase per session, CLAUDE.md §7: read the spec sections, restate the exit criterion, write
the interface, then the test, then the implementation, then run `lint.ps1` and both tiers.
