# IPC Protocol — GUI ↔ Engine

Implements SPEC.md §15.1 (control channel), §15.2 (preview channel) and §3.1 (process
lifecycle).

Implementation: `engine/core/ipc/`. The C++ client in `pipe_client.h` is not the shipped
GUI — it exists so the protocol is testable without a Python interpreter, and so M8a's
headless driver (`tests/tools/gui_host`) drives the real transport rather than a mock.

---

## 1. Transport

| Property | Value |
|---|---|
| Kind | Named pipe, `PIPE_TYPE_MESSAGE \| PIPE_READMODE_MESSAGE` |
| Name | `\\.\pipe\framecapture-{session_guid}` |
| Instances | 1 (`FILE_FLAG_FIRST_PIPE_INSTANCE`) |
| ACL | `D:P(A;;GA;;;<current-user-SID>)` — protected DACL, current user only |
| Framing | 4-byte little-endian length prefix + UTF-8 JSON body |
| Max message | 65536 bytes (body; the prefix is on top) |

The session id reaches the GUI through the `FC_ENGINE_SESSION` environment variable when
the GUI spawns the engine, and is echoed in `hello`'s response.

**Why a length prefix when the pipe already preserves message boundaries.** Two reasons,
both only visible under failure. `ReadFile` on a message-mode pipe returns
`ERROR_MORE_DATA` when the buffer is smaller than the message and leaves the remainder
queued — without a declared length the reader cannot size its buffer or know when it is
done. And a peer that writes in byte mode by mistake produces a stream that silently
concatenates messages, which the prefix turns into a detectable error rather than a JSON
parse failure ten messages later.

**Why the ACL is not optional.** A null DACL makes the pipe reachable by every process on
the machine. The commands behind it start recordings and choose output paths, so an
unrestricted pipe turns a screen recorder into a way for any local process to record
another user's screen to a file of its choosing. `D:P` — *protected* — additionally stops
inheritable ACEs from widening it.

---

## 2. Handshake

```jsonc
// GUI → engine
{"cmd":"hello","id":"1","proto":"1.0","client":"gui/1.0.0"}

// engine → GUI
{"id":"1","ok":true,"proto":"1.0","engine":"framecapture-engine","version":"0.1.0",
 "session":"a1b2c3…","capabilities":["pause_resume","heartbeat","segments","preview",
 "multitrack","gpu_migration","audio_device_migration","degradation_ladder",
 "crash_recovery"]}
```

Capabilities are the additive feature flags §15.1 requires, and the list is honest: a
flag is a promise the feature works, not that code for it exists.

**`version` — added in M9.6.** The engine binary's own version, from the CMake
`project()` declaration, which is *not* the GUI's `__version__`: the two are separate
artefacts with separate version numbers and they already disagree. It is what Help ▸
About and Tools' diagnostics summary report, because the first question any report about
this application needs answered is which build produced the file. Additive, so a GUI
reading an engine that predates it sees an absent field and renders "unknown" rather
than failing the handshake.

`multitrack` says the **engine** implements SPEC.md §8.6's Tier B. It deliberately does
not say this **machine** can perform per-application capture — that is a runtime probe
(§8.6: "probe at runtime anyway"), it can be false on a system whose audio service refuses
the virtual device, and it is reported per answer as `multitrack_available` in
`get_config` rather than baked into a handshake the GUI performs once at start-up.

`get_config` also carries `multitrack_targets` and `multitrack_reattach`; `get_devices`
carries the `processes` list the target picker is built from. All three are additive
fields on existing commands rather than new commands, because §15.1's compatibility rule
makes unknown *fields* free and its command list is a graded requirement that has already
been extended once.

---

## 3. Versioning and backward compatibility

SPEC.md §15.1's rule, and where each clause is enforced:

| Rule | Enforced in |
|---|---|
| Unknown fields are ignored, never fatal | `parse_request` reads the fields it knows and never enumerates the rest. `IpcProtocol.UnknownFieldsAreIgnoredRatherThanFatal` sends a request full of invented keys and asserts it succeeds. |
| New features are additive capability flags | `engine_capabilities()`; never inferred from the version. |
| Major bumps only on a breaking change | `PipeClient::handshake` compares **majors only**. `1.7` against a `1.0` client is accepted; `2.0` is `IPC_PROTOCOL_VERSION_MISMATCH` (7006). |
| Support the previous major for one release cycle | Nothing to do at 1.x — there is no previous major. |

A missing `id` is echoed as empty rather than refused: rejecting it would leave the peer
with a rejection it cannot correlate either.

---

## 4. Commands

All sixteen of §15.1, complete. Every request carries `id`; every response echoes it.

| Command | Params | Result | Notes |
|---|---|---|---|
| `hello` | `proto`, `client` | `proto`, `engine`, `version`, `session`, `capabilities` | `version` is the engine binary's own, added in M9.6. |
| `get_sources` | — | `displays[]`, `windows[]` | Displays carry `stable_id`, which is the identity that survives a reboot or a cable swap. |
| `get_devices` | — | `render_endpoints[]`, `processes[]` | `eRender` only. Never `eCapture` — microphone capture is a hard non-goal (§0.2). `processes[]` is one `{pid, executable}` per **executable name**, for the settings dialog's Tier B target picker; it is additive and best-effort, so a snapshot that fails leaves the field absent rather than failing the command. |
| `get_gpu_topology` | — | `adapters[]` | Re-runs §5.1 discovery. |
| `configure` | any subset | the merged values | **Merged, not replaced**: a GUI may send a subset, and replacing would reset every key it did not mention. Merged onto a *candidate* and applied only if it is valid, so a refusal leaves **nothing** applied — see `MULTITRACK_REQUIRES_MKV` below. |
| `start_preview` | — | `armed`, `section`, `width`, `height`, `stride`, `slot_bytes`, `slots`, `header_bytes`, `bytes`, `fps`, `format` | §15.2. Idempotent. Arms the ring and starts a preview-only capture session if nothing is recording. |
| `stop_preview` | — | `armed` | Idempotent, and safe with a recording in flight — see the note below. |
| `start_record` | `output` (required), `monitor`, `audio` | `output` | |
| `stop_record` | — | `output`, `valid`, `duration_s`, `decoded_frames`, `format` | Also emits `recording_finalized`. |
| `pause_record` | — | `paused`, `paused_total_ms` | §7.5. Idempotent. |
| `resume_record` | — | `paused`, `paused_total_ms` | §7.5. Idempotent. Forces an IDR. |
| `get_stats` | — | see below | |
| `get_health` | — | ladder state (§13) | |
| `set_log_level` | `level` | `level` | Matched against the logger's own spellings. Changes the **running process** and persists nothing -- `advanced.log_level` is the durable setting. First called by the GUI in M9.6 (Tools ▸ Log level). |
| `recover` | `sidecar` | `repaired`, `valid`, `output`, `detail` | §10.4's repair path, and **30 s, not 5** -- see Timeouts. The *scan* for sidecars is the GUI's (§10.4 says so); this command repairs one of them. First called by the GUI in M9.6. |
| `shutdown` | — | `shutting_down` | Answered **before** the teardown starts, so the response reaches the GUI while the pipe is still up. |
| `get_config` ⁺ | — | the full settings object + `config_path` | Includes `multitrack_available`: whether **this machine** can capture per-application audio (§8.6's runtime probe), which is a different question from the `multitrack` capability flag. |
| `save_config` ⁺ | any subset | the merged settings | Merges **and** writes `config.toml`. |

### `configure` and `save_config` can be refused, and a refusal applies nothing

SPEC.md §8.6 and §20 row 16 make multi-track audio on MP4 **a hard block, not a soft
warning**, "enforced engine-side, not just in the GUI":

```json
// GUI → engine
{"cmd":"configure","id":"9","container":"mp4","multitrack_enabled":true}

// engine → GUI
{"id":"9","ok":false,"error":"MULTITRACK_REQUIRES_MKV","code":3021,
 "message":"multi-track audio requires the Matroska container",
 "detail":"multi-track audio requires the Matroska container (SPEC.md §8.6)"}
```

The engine merges the request onto a **candidate** and commits it only if the candidate
is valid, so a refused `configure` leaves the engine exactly as it was — the container is
not applied and the track setting is not rejected in isolation. Half-applying would leave
a caller holding a state it never asked for, which is worse than either outcome the
refusal chose between.

`start_record` performs the same check independently, because the container it will write
comes from the **output path's extension** and can disagree with the setting `configure`
validated (BUG-043).

⁺ **Not in SPEC.md §15.1's sixteen; added 2026-08-03.** §17 makes the GUI the source of
truth for configuration and the engine "stateless with respect to disk config", which
left nothing able to *persist* a setting — `configure` is in-memory, and the GUI cannot
write the file without reimplementing §17's atomic replace, migration chain and
unknown-key preservation. These two keep the engine as the file's only writer. §15.1
needs amending to list them; see `docs/ACCEPTANCE.md` open question 11.

### Timeouts

5 s for everything except `stop_record`, which gets 30 s "since finalization is
legitimately slow" (§15.1) — on MP4 that includes the fragmented-to-progressive remux of
the whole file (§10.3).

`stop_record` is handled with the session lock released, so `get_stats` does not block for
those 30 s — which is precisely when a GUI most wants to say "finalizing".

**`recover` also gets 30 s, from M9.6 — and §15.1's sentence needs the owner's pen to
say so.** §15.1 names only `stop_record` because `recover` had no caller when it was
written; M9.6 Phase 4 gave it one (Tools ▸ Engine ▸ Recover unfinished recordings). The
command runs `mux::recover`, which on MP4 is *the same lossless remux a clean stop
performs* — BUG-046 measured ~3.3 s of remux plus ~1 s of validate for a 1.2 GB file. A
5 s budget would therefore time out on every recording large enough to be worth
recovering, which is all of them. Implemented as 30 s in `ipc/protocol.py`'s
`timeout_for`; flagged here rather than silently diverging.

The GUI issues it **asynchronously**, on the same command thread `stop_record` uses and
for the same reason: a multi-second blocking call on the GUI thread is the freeze M9.6
Rule B exists to prevent. Recoveries are issued one at a time — the pipe server handles
one request at a time and the client's backlog is bounded at 8 with a drop-newest policy,
so firing a directory's worth at once would silently drop the ninth.

### `get_stats`

Includes `paused_total_ms` alongside the timeline elapsed, as §15.1 requires, plus
`pauses`, `frames_excised` and `pause_stragglers`. The last is the evidence that §7.5's
pause quiesce held; it is zero on a correct recording and SPEC.md §20 row 18 asserts it.

Also carries a `preview` object — the same body `start_preview` returns — plus
`preview_published`, `preview_rate_limited`, `preview_dropped` and
`preview_offer_worst_us` / `preview_offer_mean_us`. Present **whether or not anything is
recording**, so a GUI that reconnected mid-session finds the section here rather than
having to re-issue `start_preview` and hope.

With SPEC.md §8.6's Tier B on, an `audio_tracks` array describes the per-application
tracks — `index`, `name`, `executable`, `pid`, `attached`, `target_exited`, `timeline_ms`,
`silence_ms`, `buffers`:

```jsonc
{"index":1,"name":"chrome.exe","executable":"chrome.exe","pid":12044,
 "attached":true,"target_exited":false,
 "timeline_ms":11986,"silence_ms":7986,"buffers":400}
```

`target_exited` is what lets a GUI say "the application closed" rather than leaving a
silent track looking broken (§20 row 15), and `attached` false with `target_exited` false
is the third case — a target that has not started yet, which §8.6's 2 Hz poll is still
looking for.

**The array is absent entirely when Tier B is off**, so a Tier A recording's payload is
byte-for-byte what it was before M9.5. Track 0 is not in it: the system mix is Tier A's
output and is reported by the fields that always existed.

---

## 4a. The preview channel (SPEC.md §15.2)

The frames themselves never touch this pipe — §2.1 is explicit that 1080p frames must not
— so the control channel's whole role is to name the section and describe its layout. The
full layout, the ring protocol and the reasons behind both are in
`engine/core/preview/preview_ring.h`; what follows is only what crosses the wire.

```json
{"id":"7","ok":true,"armed":true,
 "section":"Local\\framecapture-preview-a1b2c3…","width":960,"height":540,
 "stride":3840,"slot_bytes":2073600,"slots":3,"header_bytes":128,
 "bytes":6220928,"fps":30,"format":"bgra8"}
```

A reader maps `bytes` at `section`, checks the magic and version in the header, and
computes slot *n* at `header_bytes + n * slot_bytes`. **It reads the geometry back out of
the header rather than trusting the fields above**: they agree, and the check is what makes
that a fact rather than an assumption when a GUI meets an engine from a different build.

**The two commands arm and disarm; they do not start and stop a capture.** §15.1 makes
`start_preview` and `start_record` independent, so all four combinations are reachable and
the engine means something sensible by each:

| | not recording | recording |
|---|---|---|
| **armed** | a preview-only session runs — capture, downscale and ring, with no encoder, muxer or file | the recording's own session carries the preview off the same source texture |
| **not armed** | nothing is dispatched anywhere | |

Consequences worth knowing:

* `start_record` stops a preview-only session before it starts the recording, because there
  is one capture backend and DDA permits one `IDXGIOutputDuplication` per output per
  process. `stop_record` starts one again if the preview is still armed.
* **The section outlives both.** It is owned by the engine service, not by a session:
  recreating it per recording would invalidate every `QImage` the GUI holds, twice per
  recording, for no reason the GUI could distinguish from a crash.
* A `stop_preview` that arrives *during* a recording disarms the dispatch but leaves the
  section mapped until the recording stops. A live writer may be mid-publish on
  `fc-preview`, and unmapping underneath it would turn a stopped preview into a crashed
  engine.
* `IPC_SHM_CREATE_FAILED` (7008) and `IPC_SHM_MAP_FAILED` (7009) fail the *command* and
  nothing else. ERROR_CODES.md states the rule these follow: preview is optional, and the
  recording must continue without it.

---

## 5. Events (engine → GUI, unsolicited)

`state_changed`, `stats` (2 Hz), `warning`, `error`, `gpu_migrated`,
`audio_device_migrated`, `degradation_changed`, `segment_rolled`, `recording_finalized`,
`finalize_progress`.

An event carries `event` and never `id` — it answers nothing.

### `finalize_progress` — added in M9.6

SPEC.md §15.1 gives `stop_record` a 30-second timeout "since finalization is legitimately
slow", and ACCEPTANCE.md's BUG-046 measured how slow: **~3.3 s of remux plus ~1 s of
validate for a 1.2 GB recording**. This is what happens during those seconds.

```json
{"event": "finalize_progress",
 "phase": "flushing|remuxing|validating|swapping|done",
 "percent": 47,
 "bytes_done": 601380864, "bytes_total": 1288490188,
 "output": "D:\\Videos\\FrameCapture\\FrameCapture_2026-09-05_14-22-01.mp4"}
```

| Field | Meaning |
| --- | --- |
| `phase` | Which stage of §10.4's finalization is running. |
| `percent` | 0–100 across the whole finalization. **Monotonic** — render it directly. |
| `bytes_done` / `bytes_total` | The remux's read position and the source's size. **Both `0` in every other phase**, which is the signal that the phase has no byte progress and should render as indeterminate rather than as a bar that has stopped. |
| `output` | The file being finalized, so a GUI that connected mid-stop has a name to show. |

**Emitted while `stop_record` is still in flight.** The engine's request thread handles
one request at a time, so no *command* is answered during finalization — but
`PipeServer::send_event` is a write and serialises independently of the read loop, so
these arrive before the response they precede. A GUI whose event loop is blocked waiting
on `stop_record` will not see them until it is over, which is why the GUI issues that
command asynchronously.

**Only `done` reaches 100, and only for a file that passed the validation gate.** A bar at
100% is the claim that the recording is saved; a file that failed validation is not saved,
and travels as `recording_finalized` with `"valid": false` instead. A GUI must not treat
the absence of `done` as success.

**MKV has no `remuxing` phase.** Only MP4 needs §10.3's progressive remux, so an MKV stop
goes `flushing` → `validating` → `done`. That is a container with no second pass, not a
progress bar that broke.

Additive under §3's rule: an older GUI does not recognise the name and ignores it, losing
the progress detail and nothing else — `recording_finalized` still ends the wait.

**Paused is a `state_changed` value, not an event of its own.** §15.1 is explicit, and so
is the reason: "a paused recording that looks like a running one loses footage silently".
States are `idle`, `starting`, `recording`, `paused`, `stopping`, `faulted`.

Events are **not buffered** for a disconnected GUI. That is deliberate: events are state
notifications, and queueing them for a client that may never return is an unbounded queue
by another name (CLAUDE.md hard rule 5). A reconnecting GUI calls `get_stats` and
`get_health`, which is what those exist for.

---

## 6. Process lifecycle (SPEC.md §3.1)

```
GUI                                    engine
 │  CreateJobObject(KILL_ON_JOB_CLOSE)
 │  CreateProcess(CREATE_SUSPENDED) ──────►  (suspended)
 │  AssignProcessToJobObject
 │  ResumeThread ─────────────────────────►  OpenJobObject(FC_ENGINE_JOB)
 │                                           CreateNamedPipe(FC_ENGINE_SESSION)
 │  connect + hello ◄──────────────────────► capabilities
 │  …any message every ≤1 s (heartbeat)
```

The engine is spawned **suspended** and resumed only after it is in the job. A process
created running can spawn children of its own in the window before
`AssignProcessToJobObject` lands, and those children would be outside the job.

### The heartbeat is traffic, not a ping

§15.1 fixes the command list at sixteen and `heartbeat` is not among them, so inventing
one would extend a specified surface. It is not needed: §15.1 already has the engine
emitting `stats` at 2 Hz — the engine's half, at four times the required rate — and a GUI
displaying those stats is polling anyway. So **any inbound message is the host's
heartbeat**, and a GUI need only ensure it sends one within the interval.

The consequence, stated because it is a real one: liveness is proven by traffic, so a GUI
that goes silent because it is *busy* looks the same as one that has died. That is what
the 5 s timeout is for — it is five times the interval, not one.

### ⚠ A conflict inside §3.1 that needs the owner

§3.1 states two rules that cannot both hold when the GUI is **killed** rather than closed:

1. the job carries `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`;
2. on heartbeat loss the engine "finalizes the current recording cleanly, then exits. It
   does **not** discard the file."

`KILL_ON_JOB_CLOSE` terminates the job's processes when the last handle closes. Killing
the GUI closes its handles, so if the GUI holds the only handle the engine dies in the
same instant — no notification, no unwinding, no chance to run rule 2.

**SPEC.md §20 row 13's own test text settles which reading was intended:** "kill the GUI,
assert the engine exits within **6 s** having **finalized** the file." Six seconds is the
5 s heartbeat timeout plus one. A number drawn from the heartbeat path, applied to a kill,
only makes sense if the engine is expected to survive the GUI long enough to use it.

So the engine **opens its own handle to the job**. The job outlives the GUI, the heartbeat
notices within 5 s, the recording finalizes, the engine exits, and closing its handle —
the last one — dissolves the job.

**What that costs.** `KILL_ON_JOB_CLOSE` can no longer reap an engine that is *frozen*: a
process that cannot run its heartbeat thread cannot close its handle either. The residual
orphan case is "engine wedged **and** GUI gone", where the pure-job reading would have
reaped it unconditionally at the cost of never finalizing cleanly. Neither reading covers
both.

The trade taken is the one the spec's own test asks for: a playable file in the common
case (CLAUDE.md §1), against a residual case no in-process mechanism can cover. Mitigated
by the watchdog being a dedicated thread doing nothing but a timed wait. **The alternative
reading is coherent and this is recorded in docs/ACCEPTANCE.md's open questions.**

### Other §3.1 mechanisms

| Mechanism | Implementation | Covers |
|---|---|---|
| Console control handler | `SetConsoleCtrlHandler` | Ctrl+C, Ctrl+Break, console close, logoff, shutdown |
| Hidden message window | `HWND_MESSAGE` window, `WM_QUERYENDSESSION` / `WM_ENDSESSION` | OS shutdown and log-off. A console process **without** a window is terminated at log-off with whatever was on disk. |
| Single instance | `Local\framecapture-engine` mutex | `IPC_ENGINE_ALREADY_RUNNING` (7012). `Local\`, not `Global\` — §3.1 says "per user session", and a global mutex would make one user's engine block another's. |

Both shutdown handlers run on OS-owned threads under a hard deadline (~5 s for a console
handler, less for `WM_ENDSESSION`), so the callback **asks for a stop and returns**. It
never finalizes inline.

---

## 7. Errors

A failed response carries the numeric `FcError` code and its enumerator spelling:

```jsonc
{"id":"9","ok":false,"error":"MULTITRACK_REQUIRES_MKV","code":3021,
 "message":"multi-track audio requires the Matroska container","detail":"…"}
```

Prose is for humans. SPEC.md §19 makes the **numeric codes** the stable external
contract, so a GUI matches on `code` — one matching on English breaks when the English
improves. See `docs/ERROR_CODES.md`.

A malformed *frame* drops the connection: on a stream the reader cannot know where the
next message starts. A malformed *message* does not — the stream is still aligned, so the
engine answers `IPC_MESSAGE_MALFORMED` (7004) and keeps serving.

---

## 8. Coverage

| Area | Test | Tier |
|---|---|---|
| Framing, limits, partial reads, reader compaction | `IpcFraming.*` | cpu |
| Command set, events, states, compat rule, timeouts, capabilities | `IpcProtocol.*` | cpu |
| Job object, heartbeat, orphan prevention, single instance | `OrphanPreventionTest.*` | gpu |
| Pause/resume over the real transport | `fc_gui_host` + `PauseResumeTest.*` | gpu |
