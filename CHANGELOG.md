# Changelog

All notable changes to FrameCapture are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Per-application audio tracks (SPEC.md §8.6, "Tier B").** A recording can now carry a
  separate audio track for each of up to five applications, alongside the full system mix.
  Set it up under Settings → Audio: tick "Per-application multi-track" and name the
  programs, comma-separated, as they appear in Task Manager — `chrome.exe, game.exe`.

  Track 1 is **always** the complete system mix, so the file still plays normally in a
  player that only shows one track. Each named application gets its own track after that,
  labelled with its name so a video editor shows you which is which.

  Some things worth knowing, because they are deliberate rather than incidental:

  - **MKV only.** With MP4 selected the option is unavailable and says why, and the engine
    refuses the combination outright rather than writing a file whose extra tracks most
    players would never show you.
  - **An application that is not running yet is fine.** Its track is silent until the
    program starts, and the recording keeps looking for it twice a second.
  - **An application that closes mid-recording is also fine.** Its track continues as
    silence rather than stopping short, so every track is exactly the same length and
    nothing shifts — and if you reopen the application, it goes back on the same track
    with the gap left silent. There is a setting for that if you would rather it did not.
  - **Naming the applications** is a text field with a picker beside it. Pick from what
    is running now, or type the name of something you have not started yet; the picker
    adds to the list rather than replacing it.
  - **Every track is silent whenever its application is not making a sound**, which is most
    of the time. That is the normal case, not a fault.

  Measured over a 30-minute recording: every track the same length to the millisecond, and
  the worst per-track timing difference **−62 µs** against the 20 ms the specification
  allows — the same figure a 20-second recording gives, so it is a fixed offset rather than
  drift.

  It works with everything else the recorder does, and each of those is measured rather
  than assumed:

  - **Pausing** excises the same time from every track — a 3.5 second pause takes 3.5
    seconds off all of them, and the sound stays lined up to within a tenth of a
    millisecond.
  - **Splitting into segments** keeps every track, with its name, in every file.
  - **A graphics driver hiccup** mid-recording does not disturb the tracks: the recording
    rebuilds in 82 ms and each application's audio continues either side of it.

  Recording the system mix on its own is **unchanged**, and that is measured rather than
  assumed: the same recording made with and without per-application tracks produces a
  system-mix track that is identical sample for sample, and a video track with the same
  frame count.

  Verified against five applications playing at once, against an application whose sound
  comes from a child process (which is how browsers work), and against one playing a
  44.1 kHz file through two streams at the same time. Each track carries its own
  application's audio and not its neighbours': measured, **46 times** louder at its own
  sound than at the loudest of the other four.

- **A live preview (SPEC.md §15.2).** The window's preview area now shows what is being
  captured, at 960×540 and 30 fps, letterboxed rather than stretched. It runs **before you
  record**, not only during — so you can frame the shot — and keeps running through a
  recording and after it stops.

  It is built so it cannot cost you a recording. The engine writes preview frames into a
  small shared-memory ring and never waits for the window to read them: a preview that
  falls behind, or a window that is not looking, simply loses preview frames. Measured with
  the preview deliberately jammed — 219 of its frames dropped, publishing 79 where a healthy
  one published 299 — the recording captured, encoded and wrote every one of its 300 frames
  with no drops, no rebuilds, and its wall clock within two milliseconds of the preview-off
  run. The preview's cost to the recording is 25–35 µs per frame on the capture thread, and
  the GPU work is **0.153 ms per frame on the integrated adapter and 0.049 ms on the
  discrete one**, against the 0.2 ms the spec budgets.

  Under system HDR the preview tone-maps through the *same* shader the encoder uses, so
  what you see is the picture going into the file rather than a second interpretation of
  it. If the preview cannot start for any reason, the surface says so plainly instead of
  showing a black rectangle, and the recording is unaffected.

  `start_preview` and `stop_preview` now work rather than answering "not implemented", and
  the engine advertises `preview` in its handshake so a GUI can ask rather than assume.
  `get_stats` reports what the preview is doing — frames published, frames skipped, and
  what it costs the capture thread.

### Changed

- **Choosing a surround layout your device does not have no longer breaks the audio
  (BUG-048).** Setting the channel layout to 5.1 or 7.1 on a stereo device used to record
  that many channels anyway, by spreading the two it had across six or eight. The result
  was a bigger file — 512 kbps instead of 192 — carrying no more sound, and Windows' own
  player refused to play the audio at all: *"We can't play the audio ... because its
  encoding settings aren't supported. You can still watch the video."*

  The setting now works in one direction only. Asking for **fewer** channels than your
  device has still works exactly as before — that is what it is for, and it is how you
  record stereo from a surround device without changing Windows' settings. Asking for
  **more** records what your device actually supplies, and the Settings dialog now says
  so underneath the control the moment you choose it, rather than leaving you to find out
  from a player afterwards.

  If your device really is 5.1 or 7.1, nothing changes.


- **Saving an MP4 no longer writes the recording twice (BUG-046).** Finalizing an MP4
  rearranges it so players can open it instantly, and the way that was done -- FFmpeg's
  `faststart` -- writes the whole file and then rewrites it, so a 1.2 GB recording moved
  4.8 GB through the disk where 2.4 GB would do. The engine now reserves room for the index
  up front and fills it in, which produces exactly the same file in half the writing.

  **Measured honestly: about 10% faster on this machine**, not the large win the change
  might suggest. On a file recently written, the redundant pass is nearly free because the
  operating system still has it in memory; the saving is real disk work only for a file
  large or old enough to have left the cache, which is the case a long recording actually
  is. That case could not be measured here -- no file that fits in RAM can show it.

  For reference, saving now costs about **2 ms per megabyte plus 9 µs per frame**, so a
  1.2 GB recording spends roughly 3.3 s rearranging the file and about a second verifying
  it. Finalization is also now logged per stage, so if a save feels slow the log says which
  part was slow.

### Fixed

- **An unattended recording no longer restarts its own capture every few seconds
  (BUG-045).** On a screen that was not changing — which is most of what an unattended
  recording captures — the engine read the absence of new frames as a sign that capture had
  frozen, and tore down and rebuilt the whole capture and graphics stack to fix a problem
  that was not there. Each needless rebuild cost about a quarter of a second of the
  recording, and on a machine nobody was touching it repeated indefinitely. Measured from a
  user's own recording: 11.1 seconds of ordinary stillness treated as a fault.

  Windows only tells the recorder about frames when something actually changes on screen,
  so "no frames" and "capture is broken" look identical from the outside and no waiting
  period can tell them apart — an idle screen can be still for hours. The engine now asks
  the capture backend whether it is still running rather than guessing from silence.
  Genuine failures are still caught, and caught **sooner** than before: a capture that has
  really stopped is now recovered in 75–86 ms, where the old approach waited two full
  seconds first. Unplugging the monitor being recorded is also now detected directly, which
  it previously was not.

- **Recording with the preview on could write corrupted frames (BUG-044).** Found before
  it could ship, by the test written to prove the preview was harmless. With the preview
  running, a handful of frames per recording came out with the wrong pixels — the right
  number of frames, in the right order, none dropped — because the preview's GPU work and
  the recording's colour conversion were issuing commands to the same graphics device from
  two threads and interleaving their settings. Measured at 11 and 84 corrupted frames per
  300-frame recording; zero after the fix, and zero with the preview off in every run
  before it.

  Worth stating because of how invisible it was: every counter the engine keeps said the
  recording was perfect, and identical to a preview-off run. Only decoding the file and
  reading each frame's own identity out of the picture could see it.

- **Choosing MP4 now produces a file named `.mp4` (BUG-043).** It always produced an MP4
  — the engine honoured the setting and wrote a real MP4 container — but the filename was
  built with a hard-coded `.mkv`, so the recording was correct and most players would
  refuse to open it. The GUI now takes the extension from the configured container, and
  the engine writes the container the filename names, so the extension always describes
  what is actually inside the file.

- **System audio no longer has thousands of micro-gaps punched through it (BUG-042).**
  Recordings made from the real system-audio endpoint carried a persistent roughness — a
  gritty, ring-modulated character rather than dropouts. Windows stamps each audio buffer
  with the moment it was captured, and that stamp carries a few tens of microseconds of
  scheduling noise; the engine treated *any* forward discrepancy as missing audio and
  spliced silence in to cover it, then trimmed the following buffer to make room.
  Measured on a 40-second recording: **2591 separate insertions averaging 85 microseconds
  each, on three buffers in five**. Individually inaudible; at one every 15 ms they are a
  ~65 Hz buzz laid over everything.

  An endpoint cannot lose less than a whole buffer, so anything smaller is now recognised
  as clock noise and the audio is kept continuous. Same recording after the fix: **zero
  silence inserted, zero gaps filled**. A genuine dropout — a whole buffer or more — is
  still filled exactly as before, and a silent desktop still produces a full-length track.

- **Stopping a recording is now quick, instead of taking longer the longer you recorded
  (BUG-040).** Measured: **0.9 s to finish a 120-second recording, against 14.3 s for a
  112-second one before.** The file itself was always finished within milliseconds of the
  stop — the wait was a verification pass that decoded every frame of the recording before
  declaring it good, so the cost grew with the recording and a half-hour capture would have
  spent nearly four minutes there. It now checks that the file opens, that both tracks are
  present and correctly described, that the duration is right, and that the frames at the
  **start and end** decode, which is what the specification asked for in the first place.

- **The engine no longer shuts itself down while saving a long recording (BUG-039).**
  Because of the above, stopping could occupy the engine for longer than the five seconds
  after which each side assumes the other has died — so the GUI reported "the engine went
  away", the engine reported "host heartbeat lost", and it exited **while finalizing the
  recording**. Both processes were healthy and each was waiting on the other. The engine no
  longer counts the time it spends executing a command as evidence that the GUI is gone.
  The recording that provoked this was still written correctly; the fix is what stops it
  being a matter of luck.

- **The engine no longer crashes when you start a second recording without restarting it
  (BUG-037).** The first recording worked, finalized and validated; the second faulted
  during start-up with an access violation and took the recording with it. Every COM
  apartment in the engine was scoped to a recording, so between two of them nothing was
  in the process's multi-threaded apartment, COM shut it down, and Windows unloaded
  `GraphicsCapture.dll` — while the WinRT activation factory cached for the *process*
  went on pointing into it. The next capture-backend probe called through a vtable that
  was no longer mapped.

  The MTA is now held for the process lifetime, so a thread leaving its apartment no
  longer takes the apartment with it. A second symptom went with it: the second recording
  in a session used to open with `IAudioClock2 unavailable`, losing SPEC.md §8.3's
  endpoint cross-check for the rest of that recording.

  Not a regression from pause/resume — the defect had been in the apartment handling
  since WGC was written. M8's GUI is simply the first thing that could start a second
  recording in one process.

- **Pausing no longer corrupts the audio of a recording that uses the real system-audio
  endpoint (BUG-038).** SPEC.md §8.4 measures the encoded audio track against elapsed
  time to detect device-clock drift. The track had paused time excised from it (§7.5) and
  the elapsed reference did not, so a pause was reported as drift *of exactly its own
  duration* — a 2 s pause read as 2 s of drift, fifty times the threshold at which the
  engine stops making inaudible corrections and starts making audible ones. Measured in
  the field: seven hard resyncs in a 15.9 s recording, asking to inject 97,020 correction
  frames.

  The file was the right length and played; only its sound was wrong, which is why
  nothing caught it sooner. Measured after the fix on the same path: **+9 µs of drift
  against a 40 ms threshold, zero resyncs**.

  SPEC.md §8.3's separate endpoint cross-check was checked for the same defect and does
  not have it — both of its terms are wall-clock quantities, and excising paused time
  from one would have introduced the bug rather than fixed it. There is now a test saying
  so.

- **The GUI's end-to-end recording tests no longer report on whatever happened to be on
  screen.** Not a product defect — the engine was correct in every case — but a test
  defect of the class BUG-033 fixed for the C++ tier and never applied here. On an idle
  desktop, Windows' capture API composites nothing after the first frame, so a
  one-second recording finalized with a single frame and was correctly rejected by the
  §10.4 validation gate; the test read that as a product failure. The cases now present
  their own animated window and keep the Qt event loop turning while recording, which
  the bare `time.sleep` they used before did not.

- **Qt's own diagnostics reach the GUI's log** instead of going to stderr unattributed,
  carrying whatever file, line and function Qt supplies. This does not silence anything;
  it is what a warning needs to be actionable. Prompted by an unattributed
  `QFont::setPointSize: Point size <= 0 (-1)` at start-up which could not be reproduced on
  demand — see "Known issues".

### Known issues

- **`QFont::setPointSize: Point size <= 0 (-1)` is occasionally emitted at GUI start-up.**
  Cosmetic; nothing renders wrong. The mechanism is understood and pinned by a test: a
  stylesheet `font-size` in pixels makes `QFont::pointSize()` return −1 by definition, so
  any code reading it back through `setPointSize` warns, and SPEC.md §16.3 specifies "13
  px base, 11 px for the status readout". Which caller does the round trip is **not**
  identified — it did not reproduce across every panel, both dialogs, the tooltip, the
  message box and three DPI scale factors, with a message handler verified to catch the
  warning. Changing the typography to points would resolve it and would depart from
  §16.3, so that is the owner's call rather than a fix made in passing.

### Added

- **Video segmentation (SPEC.md §11, M9) — opt-in, off by default.** A long recording can
  be split into a series of files by elapsed time or by size, whichever limit is reached
  first. Splits happen at a keyframe, so every file plays from its first frame rather than
  opening with a second or two of garbage, and **no frame is lost at a boundary**: the next
  file is opened and its header written before the previous one is closed.

  Each file's timestamps start at zero, so every part is independently playable and
  seekable. A `<basename>.segments.json` index records where each part begins on the whole
  recording's timeline, for tools that want to join them back together. Files are named
  `<basename>_part001.mkv` and up, widening past `_part999` rather than wrapping.

  Measured: 4500 frames split into three segments, all keyframe-aligned, every frame
  present exactly once and in order across the set. With the keyframe alignment removed,
  the same recording loses 91 frames at a single boundary — which is what the alignment is
  for.

- **Pause / resume (SPEC.md §7.5, §20 row 18) — the feature, not just the command
  names.** Pause **excises** wall-clock time: the recording stays one file and the paused
  span is simply absent from it, so the frame before a pause and the frame after it are
  adjacent both in the file and on screen. `pause_record` / `resume_record` are live over
  IPC, idempotent per §7.5, and reported as a distinct `paused` state rather than as
  something a GUI has to infer.

  The shared paused total lives in one object, `timing::PauseClock`, which the video
  pacer and the audio timeline are each handed a pointer to — that is the whole of §7.5's
  requirement, made structural rather than a rule two components have to remember.
  Measured over three uneven pauses totalling 4.6 s in a 7 s recording: the file holds
  **exactly** the 420 unpaused frames, worst A/V offset **−0.188 ms** against a 20 ms
  limit with **−0.188 ms** of growth across the whole recording — identical to the
  unpaused control, so pausing changed the A/V relationship by nothing. 0 repeated
  barcodes, 0 duplicates, 0 stragglers.

  `get_stats` reports `paused_total_ms`, and finalization logs it, because "why is my
  30-minute recording 12 minutes long" has to be answerable from the log.

- **`forced-idr` is now set on every encoder path**, which §7.5's resume needs and which
  was silently absent on two of the three. Measured against the FFmpeg n8.1.2 sources
  this build links: with the option at its default, `pict_type = AV_PICTURE_TYPE_I`
  produces `NV_ENC_PIC_FLAG_FORCEINTRA` on NVENC, `AMF_VIDEO_ENCODER_PICTURE_TYPE_I` on
  AMF and `X264_TYPE_KEYFRAME` on libx264 — a plain I-frame, which does not empty the
  reference buffer, so a later P-frame can still reference pre-pause content. The
  user-visible symptom would have been a smear at every resume seam in a perfectly
  playable file. Note the spelling differs by encoder (`forced_idr` on AMF).

- **`engine/core/ipc/` — the GUI ↔ engine control channel (SPEC.md §15.1).** Named pipe
  at `\\.\pipe\framecapture-{session_guid}`, message mode, ACL restricted to the current
  user's SID via a *protected* DACL. 4-byte little-endian length prefix + UTF-8 JSON,
  64 KB ceiling enforced on both the sending side and — before any allocation — on a
  declared length. All sixteen of §15.1's commands and all nine events. `hello` returns
  an additive capability list, and §15.1's "unknown fields are ignored, never fatal" is
  implemented in the parser and asserted by a test that sends invented keys.

  `start_preview` / `stop_preview` answer `INTERNAL_NOT_IMPLEMENTED` and `preview` is
  absent from the capability list: §15.2's ring is M9's, and a GUI told yes would enable
  a control that does nothing.

- **Process lifecycle (SPEC.md §3.1, §20 row 13).** Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, a suspended spawn assigned to the job before it
  runs a single instruction, a bidirectional heartbeat at §3.1's 1 s / 5 s, a console
  control handler, a `HWND_MESSAGE` window handling `WM_QUERYENDSESSION` / `WM_ENDSESSION`
  so an OS shutdown still writes a trailer, and a `Local\` named mutex enforcing one
  engine per user session. Measured: killing the host with `TerminateProcess` leaves the
  engine **5755 ms** to notice, finalize and exit against row 13's 6000 ms, with the file
  valid at 443 frames and the recovery sidecar cleared.

  §3.1 contains a conflict between the job's kill-on-close and the heartbeat's clean
  finalize when the GUI is *killed*; it is implemented the way §20 row 13's own test text
  implies and flagged for the owner in `docs/ACCEPTANCE.md`.

- **`framecapture-engine --serve`** runs the control plane. Without the flag the binary
  keeps the diagnostic behaviour every script has relied on since M0.

- **`docs/IPC_PROTOCOL.md`** — the SPEC.md §22 deliverable, now that there is a protocol
  to document: schemas, versioning policy, the lifecycle handshake, and the reasoning
  behind the parts that look redundant (the length prefix, the ACL, traffic-as-heartbeat).

- **The PySide6 GUI (SPEC.md §16).** `gui/framecapture_gui/`: §16.2's OBS-inspired
  layout, §16.3's palette as a single themeable `theme_dark.qss`, §16.4's seven-section
  settings dialog, and §16.5's global hotkeys via `RegisterHotKey` with per-binding
  conflict detection. **A recording can be started, paused, resumed and finalized
  entirely from the window** — M8's exit criterion — and the file it produces is
  asserted valid by `pytest gui/tests`.

  Paused is a first-class state throughout, per §16.5: the status headline shows the
  **timeline** elapsed (the length the file will actually have) with total paused time on
  its own line, the recording dot stops pulsing and holds, and the primary button reads
  STOP rather than START. The engine is spawned into a Job Object with
  `KILL_ON_JOB_CLOSE` from `ctypes` — verified by killing the real GUI mid-session and
  finding no surviving engine.

  Honest about its gaps rather than mocking them: no preview surface (§15.2 is M9), no
  audio level meter (per-track levels are not in §15.1's `stats`), and settings that
  apply for the session but are not persisted (see `docs/ACCEPTANCE.md`, open question
  11). Each is stated in the UI, not only in a comment.

- **Settings persist across restarts (SPEC.md §17).** Two commands beyond §15.1's
  sixteen — `get_config` and `save_config` — let the GUI read and write the
  configuration while the **engine remains the file's only writer**. That keeps §17's
  atomic replace, ordered migration chain and unknown-key preservation in the one place
  that already implements and tests them, instead of giving `config.toml` a second writer
  in Python with a second idea of the schema.

  The settings dialog opens showing what is actually in force rather than its own
  defaults, and a failed save leaves the dialog open and says so — closing on failure
  would tell a user their settings were kept when they were not. Verified across a real
  engine restart: written through one process, read back by a second.

  **§15.1's command list should be amended to eighteen** — recorded in
  `docs/ACCEPTANCE.md` as needing the owner's pen.

- **`scripts/lint.ps1` now runs ruff and `mypy --strict`** over `gui/`, which its own
  comment had promised since M0, and `scripts/bootstrap.ps1` creates the venv they need.
  Configuration is in a new root `pyproject.toml`, so the gate is identical whether it is
  run by the script or by an editor.

- **`scripts/run-dev.ps1`**, which SPEC.md §21.2 and CLAUDE.md §3 both listed and which
  did not exist. Launches the engine's control plane with verbose logging; `-Probe` keeps
  the one-shot diagnostic behaviour.

- **`FC_LOG_LEVEL`** overrides the configured log level at startup. Parsed by the same
  function §15.1's `set_log_level` uses, so a level one accepts and the other rejects is
  not a reachable state.

### Planned

- **~~Pause / resume is now a specified feature, scheduled in M8~~ — delivered above
  (SPEC.md §7.5, §20 row 18).** Kept for the record of what was specified before it was
  built. `pause_record` and `resume_record` had been in §15.1's command list since the
  first draft with no semantics attached and no engine code behind them — two command
  names and a hotkey binding, which is how a feature ships half-built. §7.5 now defines
  it: pause **excises** wall-clock time, so the recording stays one file and the paused
  span is simply absent from it.

  The hazard is not the feature, it is what it does to §7.1's shared epoch. Every PTS is
  a function of `qpc - t0`; excising time adds a second term, `paused_total_ns`, and that
  term must be shared by video and audio exactly as `t0` is. Two streams subtracting
  different totals desync permanently and silently, and no existing test would catch it
  because none of them pause. §7.5 puts the total in the session clock, suspends the
  silence generator, forbids duplicate-filling the gap, forces an IDR on resume, and
  suspends the row 9 stall detector so a paused recording does not read as a frozen one.
  The §20 matrix is now **18 rows**; §24 M10 and §25 updated to match.

### Added

- **`pipeline::RecordingSession` (SPEC.md §5.4, §14.1, §20 rows 9, 11, 12)** — the
  orchestration above `VideoPipeline`. Owns the capture, the pipeline and a
  `DeviceWatcher`, and turns a detected device loss into §5.4's eight-step rebuild
  without ending the recording. Reports `MigrationRecord`s so the gap against §5.4's
  350 ms budget is a measured number rather than a claim. Measured: an injected
  `DXGI_ERROR_DEVICE_REMOVED` rebuilds once in **127.3 ms**, 247 frames, one segment;
  reaching the same rebuild through degradation-ladder rung 4 costs **126.3 ms**; and a
  healthy 6 s recording is rebuilt **zero** times.
- **`AudioPath::migrate_to` and `audio::enumerate_render_endpoints` (SPEC.md §14.1,
  §20 row 12)** — a live recording moves to another render endpoint without ending. The
  old endpoint's queued buffers are placed first, the seam is taken after the new
  endpoint has negotiated so the gap cannot be missed, and `AudioEncodePath::migrate_input`
  chains a fresh timeline in the new rate while the **AAC output format stays fixed** —
  changing an AAC stream's rate or channel count mid-file is invalid in both MP4 and MKV.
  A failed reopen is deliberately non-fatal: the silence generator keeps the timeline
  advancing, so the file stays the right length (CLAUDE.md §1). Measured: **15.2 ms**
  against §14.1's 200 ms target.
- **`VideoPipeline::rebuild_device` (SPEC.md §5.4 steps 5–7)** — replaces the D3D device,
  colour converter and video encoder while the muxer, pacer, shared epoch and audio path
  survive. **Refuses unless the rebuilt encoder's `extradata` is byte-identical** to the
  container's, which is what makes the amended §5.4 safe rather than merely written
  down: the pipeline will not continue a file it cannot continue correctly, and the
  caller's fallback is the segment split. Measured: a same-adapter rebuild takes
  **22.2 ms** against §5.4's 350 ms budget and keeps one continuous decodable file.
- **`Pacer::reserve_discontinuity`** — and the decode-order constraint §5.4 never
  mentions. A flushed encoder's last packet carries `DTS == PTS`; a fresh one derives
  `DTS = PTS − reorder_depth`, so continuing the grid seamlessly hands libavformat a DTS
  *below* the one just written and the packet is rejected (`non monotonically increasing
  dts … 483 >= 467`, measured). Reserving `max_b_frames` slots is the smallest advance
  that clears it — 2 frames, 33 ms, inside a gap §5.4 already budgets 350 ms for.
- **`gpu::measure_cross_adapter_cost` (SPEC.md §5.3)** — the measurement §5.2 rule 2 has
  always required and never had. On the reference rig §5.3's preferred shared-texture
  path is **unavailable** (`OpenSharedResource1` fails both ways between the 780M and the
  RTX 4050), so the staged system-memory fallback applies at a P99 of **6.0–7.5 ms**
  against a 2.0 ms budget — rule 2 correctly does not fire. Reported as a percentile, not
  a mean: the median is ~1.0 ms and a mean would have let the rule fire on a path that
  stalls 7 ms once per hundred frames.
- **`DeviceWatcher` (SPEC.md §3, §5.4)** — the detection half of GPU migration. §5.4's
  HRESULT classification is a pure function over two integers, so all of its "these are
  different bugs" cases (`DEVICE_REMOVED` vs `DEVICE_HUNG` vs `DRIVER_INTERNAL_ERROR`
  vs `ACCESS_LOST`) are asserted on the CPU tier — including that `ACCESS_LOST` must
  *not* trigger a device rebuild, and that `DXGI_ERROR_WAIT_TIMEOUT`, the normal return
  from an acquire on an idle desktop, must not read as device loss. Adapter-set diffing
  is order-insensitive, because DXGI renumbers across the very driver restart being
  detected.
- **`audio::enumerate_render_endpoints`** — the `eRender` endpoints, their ids, friendly
  names, default flag and current mix format, read without opening a stream. Needed by
  SPEC.md §15.1's `get_devices` and by §14.1's migration, which has to choose where it
  is moving to. Render only, never `eCapture` (CLAUDE.md §2 rule 6).
- **The graceful degradation ladder and health monitor (M6).** SPEC.md §13's
  `HealthMonitor`, as a pure state machine over sampled observations with no clock of
  its own — a 30-second hysteresis rule verified by sleeping for 30 seconds is a rule
  nobody re-verifies after they change it. All eight rungs are evaluated, logged, and
  surfaced through `VideoPipeline::health()`. Driven by a new `watchdog` thread
  (SPEC.md §12) sampling at 4 Hz.
- **Two deliberate divergences from SPEC.md §13, both documented in
  `health_monitor.h`.** The rung is a *severity report*, not an index into a switch:
  deriving rung 3's action from `rung >= 3` would permanently halve the capture rate of
  any machine sitting at rung 5 with no hardware encoder, for reasons unconnected to
  frame drops. And the hysteresis is applied per condition rather than to the composite
  rung, because on the rung alone the *actions* still flap — a drop ratio hovering at 5%
  would toggle the capture rate twice a second, which is more visible than either steady
  state and is the oscillation §13's recovery rule exists to prevent.
- **Rung 3 acts:** sustained frame drops above 5% retime the pacer to 30 fps, keeping the
  CFR timeline intact via duplicates. Published by the watchdog and applied on the venc
  thread, because `timing::Pacer` belongs to that thread and retiming it from another
  would corrupt the PTS sequence rung 3 exists to protect.
- **Rung 5 exists at last:** the software encode path. libx264 has been linked since the
  owner's decision below, but no submit path existed — every other encoder takes a
  `CopySubresourceRegion` into a hardware pool, and libx264 cannot read a texture. The
  NV12 readback goes through `av_hwframe_transfer_data` so the mapped-texture plane
  arithmetic stays FFmpeg's problem; a mapped D3D11 NV12 chroma plane's offset is not
  reliably `RowPitch * height`, and getting it wrong yields a playable file that is
  green with the picture shifted. Verified by decoding the output and reading the
  frame-index barcode out of every frame — measured 120 frames at 119.7 fps at 1080p,
  0 unreadable barcodes, 0 wrong indices. The readback pool's bind flags are **probed**
  rather than supplied: BUG-001's value describes an encoder-input surface and this path
  has no encoder, and `SHADER_RESOURCE` alone is refused with E_INVALIDARG.
- **`Muxer` measures its own write latency** (P99 over a rolling 512-packet window) and
  the output volume's free space, which are rung 6's two triggers. P99 and not a mean:
  `av_interleaved_write_frame` buffers, so most calls never touch the disk and the tail
  carries the whole signal. Both numbers are now logged on **every** recording's
  finalization — "why did this stutter" is answerable with them and unanswerable
  without.
- **SPEC.md §20 row 6 is green.** `test_sustained_60fps` (gpu): paced 1080p60 against a
  4K compute stressor running the engine's own shader on its own device, with the load
  *level* measured from the Windows `GPU Engine` PDH counters rather than assumed —
  grouped per physical engine the way Task Manager does, because summing every instance
  produces a figure that reads 60% on an idle desktop and is not a percentage of
  anything. Includes an unloaded control, without which a low drop rate says nothing
  about load tolerance.
- **SPEC.md §20 row 7 is green.** `test_cfr_exactness` (gpu): exact frame count and
  PTS-grid assertions on the decoded file in both containers. Row 7's "all PTS deltas
  identical" turns out to be container-dependent in a way the spec does not mention —
  `matroskaenc` forces a 1/1000 timebase on every stream, on which a 60 fps frame
  duration is not representable, so MKV's deltas measure `{16, 17}` and MP4's `{1000}`.
  The test asserts the strongest form each container can carry and decides which from
  the file's declared timebase, so a future FFmpeg that stops forcing 1/1000 tightens it
  automatically. Recorded in `docs/ACCEPTANCE.md`.
- **SPEC.md §20 row 10 is green.** `test_slow_disk` (gpu), using SPEC.md §20.1's
  sanctioned disk-stall injection: a real delay on the real mux thread, so the latency
  rung 6 reads is measured by the same code path a failing volume would move.
- **SPEC.md §20 row 9's detector**, and a scoping decision recorded in
  `docs/ACCEPTANCE.md`: §24 lists row 9 against no milestone, and it is now split with
  detection in M6 and the session-rebuild recovery in M7, where §5.4's machinery is
  built. Row 9 stays **Partial**, not Green.
- **libx264, by the owner's decision of 2026-07-30 — the distribution is now
  GPLv2.** `gpl` and `x264` are in the vcpkg manifest and
  `--enable-gpl --enable-libx264 --enable-encoder=libx264` in the FFmpeg whitelist.
  This unblocks degradation-ladder rung 5 (SPEC.md §13): without a software encoder
  a machine whose hardware encoders all fail simply stops recording. The licence
  consequence is recorded in CLAUDE.md §9 and in the whitelist itself — linking
  libx264 puts FrameCapture under GPLv2 on distribution, which obliges source
  availability and needs SPEC.md §21's packaging work revisited. The *software
  submit path* that uses it is M6; this is the dependency and its verification.
- **`test_codec_availability` (cpu)** — asserts what the FFmpeg build does and does
  not contain. The whitelist has been a policy document with nothing checking that
  the policy survived the build: a mistyped `--enable-encoder=` is a silent no-op.
  **The negative assertions matter more than the positive ones.** SPEC.md §0.2
  makes "no network capability" a hard non-goal and §2.1 implements it by compiling
  none in — a claim load-bearing since M0 and, until now, entirely unverified. It
  passes: the only protocol in the build is `file`. Also pins no streaming muxers,
  no capture devices, and no encodable codec beyond H.264 (SPEC.md §9).
- **`test_amf_zero_copy` (gpu)** — SPEC.md §2.2 item 1 / CLAUDE.md §9. Asserts
  structurally that the engine hands libavcodec D3D11 surfaces and never touches
  system memory: `pix_fmt = AV_PIX_FMT_D3D11`, a frames context on the *same*
  device as the textures, and `CopySubresourceRegion` GPU to GPU. Run on every
  encoder, not just AMF. Measured: **374 fps at 1080p on the 780M, 366 fps on the
  RTX 4050**. It deliberately does **not** claim `h264_amf` avoids an internal
  download — that is not visible through FFmpeg's public API — so CLAUDE.md §9's
  decision stays open, narrowed to that one question.
- **`docs/ACCEPTANCE.md`** — the SPEC.md §20 row → test mapping M10's "all 17 rows
  green" audit needs. Test names in the tree deliberately differ from the spec's
  (`NoFrameIsEverUniform` rather than `test_no_black_frames`, because a failure
  report should say what is wrong, not where to look), and without a mapping the
  spec's requirement becomes unverifiable. A merge gate, like `docs/CONFIG.md`.
- **Rows 4 and 17 now cover MP4 as well as MKV.** Both had been written when MKV
  was the only container. MP4's layout signalling is a different mechanism
  (`chnl`/`esds` rather than `Channels`), and its audio timebase path had never been
  asserted — the exact pairing BUG-015 got wrong. Row 17 passes cleanly in both;
  row 4's steady state through MP4 measures **−187 µs**, identical to MKV.
- `scripts/fetch-asset.cmd` — vcpkg asset fetcher that works around the
  certificate-revocation failure (BUILD.md §4.1) *and* the dead `ftpmirror.gnu.org`
  redirector that blocks x264's toolchain (§4.5). Host rewrites only; vcpkg's
  SHA-512 check is what makes that safe.

- **MP4 output with crash-safe finalization (M5).** SPEC.md §10.3's recipe: the
  recording is written **fragmented** (`frag_keyframe+empty_moov+default_base_moof`)
  so a hard kill leaves a file that plays, and a clean stop losslessly remuxes it
  to progressive with `faststart` — stream copy only, nothing decoded, nothing
  re-encoded. `Muxer::open` no longer refuses MP4.
- The fragmented file is written **at the final output path**, and the remux
  replaces it atomically. Recording to `output.mp4.part` would mean a crash leaves
  a file with an extension nothing opens, when a fragmented MP4 is already playable
  everywhere; this way a hard kill leaves something the user can double-click. The
  progressive form is validated with a decode probe *before* the swap, so a remux
  that goes wrong costs nothing — the fragmented recording is still there.
- **A `.fcrecover` sidecar (SPEC.md §10.4)**, written at recording start and
  removed only once the output has been validated. Its presence is the signal that
  a recording did not finish. It carries container, path, geometry, codecs and a
  wall-clock start time, so a repair pass and a user-facing notice need not probe
  the media to find out what it was meant to be. Written atomically — a half-written
  claim is a claim nobody can evaluate.
- `mux::recover()` — SPEC.md §10.3's "Repair & finalize", and `mux::find_recoverable()`
  for the launch-time scan. MP4 gets the same remux a clean stop performs; Matroska
  is validated and left alone, because a truncated one already plays and rewriting
  it would risk a working recording to gain an index.
- **`test_crash_recovery` (SPEC.md §20 row 3)** and `fc_crash_recorder`, a separate
  process that exists to be killed. The kill has to be real: a test that closed the
  muxer politely would exercise the finalization path, which is precisely the path
  that does not run when a process dies. Measured at row 3's stated 30 seconds:
  **28.004 s decoded of a 30 s recording, 1.995 s lost** — one GOP, which is exactly
  the allowance row 3 gives. The clean-stop half is asserted by reading the MP4 box
  order directly — `moov` ahead of `mdat`, no `moof` left — because "progressive" is
  a statement about byte layout and libavformat will happily open the file either
  way, so asking it would be asking the wrong witness.
- `Muxer::needs_remux()`, and `VideoEncodeTest.Mp4IsAcceptedAndWrittenFragmented`
  replacing M3's `Mp4IsRefusedInM3RatherThanHalfWritten` — the assertion inverts
  with the contract it was guarding.

- **Audio Tier A wired into the pipeline (M4).** The components built separately —
  WASAPI loopback, the silence generator, drift compensation, resampling, AAC and
  the muxer's audio stream — are now one path, with the `audio`, `silence` and
  `aenc` threads SPEC.md §12's table calls for. A recording produced through
  `VideoPipeline` carries both streams against one epoch.
- The path is **split at the WASAPI seam**, deliberately. `AudioEncodePath` is
  everything above it — timeline, drift, resampling, encoding — and touches no
  device, no COM apartment and no D3D, so the arithmetic SPEC.md §20 row 4 turns on
  is asserted on the CPU tier with a synthetic device clock rather than observed on
  hardware. `AudioPath` is the thin part below it: the endpoint and the watchdog
  thread, which can only be exercised against a real device.
- **A/V sync is measurable rather than assumed.** `test_av_sync` (SPEC.md §20
  row 4) records a 1 kHz beep and a white flash frame generated from one synthetic
  clock, decodes both streams back out of the finished MKV, and asserts the offset
  at every second. Measured on the reference rig over 65 marks: **worst offset
  −187 µs** against row 4's 20 ms. The flash leaves the frame-index barcode intact,
  so a flash frame identifies *which* frame it is instead of being inferred from
  its position — without that, a file whose audio and video were both uniformly
  late would pass. `FC_AV_SYNC_SECONDS` sets the duration, which is how the same
  test serves as M4's 30-minute run. It is **not** yet §20.1's four-hour soak form:
  the decoder holds every sample in memory, which is 5.5 GB at four hours, so the
  soak needs a streaming onset detector. That is M10 work and is not pretended to
  exist.
- **`test_loopback_recording` — the real WASAPI path, end to end.** Everything else
  in the audio suite drives `AudioEncodePath` directly, which is what makes it
  deterministic and also what leaves the half below the seam untested:
  `LoopbackCapture`'s COM apartment and MMCSS registration, the sink into the
  encode path, the `on_first_packet` callback that resolves the epoch from the
  `audio` thread, and the `silence` watchdog on a real clock against a real
  endpoint. It records for twelve seconds in real time and asserts the audio track
  is as long as the video — which on an idle machine is SPEC.md §8.2's defect
  stated as directly as it can be, since every sample of that track is
  manufactured silence. It also covers SPEC.md §7.2's duplicate path for free,
  because an unoptimised build cannot generate the test pattern at 60 fps and the
  pacer fills the missed slots — 592 source frames plus 128 duplicates makes 719 of
  a possible 720.

  `FC_LOOPBACK_SECONDS` sets the duration. **M4's 30-minute exit criterion was run
  in this form**, in real time against the real endpoint: 108,000 source frames,
  107,999 encoded, audio 1800.021 s against video 1799.983 s, zero queue drops and
  zero encode failures. The pacer traded 6,349 frames for 6,349 duplicates and
  landed on exactly 60.000 fps over the half hour.

  The number worth keeping is `device_vs_qpc_us` at **25,961 µs — the endpoint's
  crystal is 14.4 ppm fast**, so it gained 26 ms of its own over the recording
  while the encoded track stayed 38 ms from the video track. That is SPEC.md §8.2's
  timeline absorbing a real clock error, measured rather than argued, and it is the
  evidence behind the §8.2-over-§8.4 resolution below: had §8.4's ladder been
  driving corrections off the device clock, it would have spent the recording
  chasing 26 ms that never reached the file.
- `test_channel_layout` (SPEC.md §20 row 17) now records 5.1, 7.1 and stereo end to
  end and reopens the finished file, asserting the layout in the **container** and
  in the layout the **decoder** derives from the `AudioSpecificConfig`, plus
  per-channel tone identification. The encoder-side assertions in
  `test_audio_encode.cpp` could not see whether either survived into a file, and
  surviving into the file is the entire failure mode.
- A **30-minute drift run** against a 50 ppm-fast device clock, on the CPU tier
  (`FC_AUDIO_DRIFT_MINUTES` overrides the duration). Measured: the audio track ends
  **0 µs** from wall clock and the encoded stream **10 µs**, against SPEC.md §20
  row 4's 20 ms. The mirror case — a 200 ppm-slow clock — is covered too, because a
  fast clock is absorbed by trimming and a slow one by silence injection, and only
  one of those two paths runs in either test.
- The 1 kHz tone SPEC.md §20.1 asks of the synthetic source
  (`tests/tools/synthetic_source/synthetic_audio.*`). Defined over absolute QPC
  nanoseconds rather than its own sample counter, so a beep and a flash are the
  same instant by construction whatever the audio device's origin was — a generator
  anchored to the device would put its beeps 40 ms from the flashes on any machine
  whose endpoint opened first, and the test would measure its harness.
- `SessionEpoch` is now *published*, not just recorded. `note_first_audio` resolves
  the epoch, starts the pacer and hands `t0` to the audio timeline; whichever
  stream completes the pair does the publishing, so neither has to be the one that
  happens to arrive later.
- **SPEC.md §8.3's device-position cross-check.** `IAudioClock2::GetDevicePosition`
  was being read and carried on every buffer, and nothing consumed it. It now feeds
  a 1 Hz comparison of the endpoint's own frame counter against QPC, surfaced in
  `AudioStats::device_clock_delta_ns` and summarised when the path finishes. This
  is a *different* quantity from the §8.4 drift number and that is the point: §8.4
  measures the encoded track's length, which §8.2's timeline holds at zero, so on
  its own it cannot distinguish a healthy endpoint from one whose crystal is
  wandering. Logged at DEBUG rather than INFO — 1 Hz over §20.1's four-hour soak is
  14,400 lines, and the value survives in the stats and the closing summary at any
  log level.
- `core/timing/qpc_clock.h` — the single QPC-to-nanoseconds conversion SPEC.md §7.1
  asks for. Every call site now routes through it: the audio path, both capture
  backends and the synthetic test source. They were four copies, one of which was
  wrong in a way no test could see (BUG-019).
- `core/util/worker.h` — the bounded-join shutdown of SPEC.md §12, shared rather
  than copied into each pipeline stage. Its escalation path (detach, log,
  deliberately leak the state the detached thread is still reading) is subtle
  enough that two copies would eventually disagree; see BUG-008.

- Repository skeleton and build toolchain (M0a): CMake 3.25+ project with
  `windows-msvc-debug`, `windows-msvc-relwithdebinfo` and `windows-msvc-release`
  presets, vcpkg manifest mode with a pinned `builtin-baseline`, `.clang-format`
  and `.clang-tidy`, and `scripts/bootstrap.ps1` / `scripts/lint.ps1`.
- `framecapture-engine` reports the version of every linked dependency and exits 0.
- FFmpeg is built from a `--disable-everything` whitelist with `--disable-network`,
  so no network code is compiled into the engine at all. See
  `ports/ffmpeg/framecapture-whitelist.cmake`.
- `docs/BUILD.md` — prerequisites, commands, and every build failure hit so far,
  including the certificate-revocation problem that makes every vcpkg download fail
  with `curl error 35`.
- `docs/ENGINEERING_LOG.md` — bug journal, opened with BUG-001 (D3D11 encoder-input
  frame pool rejected unless `BindFlags` is `D3D11_BIND_DECODER`).
- `scripts/spikes/amf_zerocopy/` — throwaway diagnostic for SPEC.md §2.2 item 1.
  Not part of the engine build graph or the test suite.

- Typed error domain (M0b): `FcError` with 105 stable numeric codes grouped by
  subsystem, `Result<T, FcError>` with `FC_TRY` / `FC_TRY_ASSIGN`, and the
  `FC_HR` / `FC_HR_AS` / `FC_HR_LOG` macros that log the failing expression, file,
  line, HRESULT and its `FormatMessage` text.
- spdlog-based logging (M0b): async with a bounded drop-oldest queue, rotating file
  sink (10 MB × 5), 2000-entry in-memory ring, MSVC sink in debug builds, and the
  structured fields SPEC.md §18 mandates on every line.
- Session preamble logged at startup. Hardware-enumeration fields are marked
  `status=<pending>` with their owning milestone rather than left blank.
- Crash handler: `SetUnhandledExceptionFilter`, `_set_purecall_handler`,
  `set_terminate` and `_set_invalid_parameter_handler`, writing a minidump, a
  ring-buffer dump, and `crash_report.json` carrying the last known engine phase.
- `docs/ERROR_CODES.md` — every code with cause, remediation and log signature.
  `test_fc_error.ErrorCodesDocumentationIsComplete` fails the build if a code is
  undocumented.

- TOML configuration (M0c) at `%LOCALAPPDATA%\FrameCapture\config.toml`: 33 keys
  across `[general]`, `[video]`, `[audio]`, `[segmentation]`, `[advanced]` and
  `[updates]`, with `segmentation.enabled` defaulting to false and `video.codec` to
  `h264`. Schema-driven validation clamps continuous ranges and falls back to the
  default for type and enum errors, warning in both cases and never crashing.
- Unknown config keys are preserved verbatim across load/save cycles, so downgrading
  cannot destroy settings written by a newer build.
- Ordered migration-chain machinery with `config.toml.bak.<version>` backup and
  per-step logging. A missing link in the chain is a hard error, never a skipped
  version.
- `fc::write_file_atomically` — temp file, `FlushFileBuffers`, then
  `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`, with an injectable fault point so
  crash-safety is tested rather than asserted.
- `docs/CONFIG.md` — every key with type, range, default, effect and the schema
  version that introduced it. `test_config.ConfigSchema.IsFullyDocumented` fails the
  build if a key is undocumented.

- BGRA8 → NV12 colour conversion (M2, SPEC.md §6): D3D11 compute shader, BT.709,
  limited range by default with full range as an option, chroma box-filtered over
  the 2×2 luma quad. Compiled to embedded bytecode at build time, so a broken
  shader fails the build rather than a recording. Verified numerically on both
  adapters — including the pure-red test §6 asks for by name.
- Windows.Graphics.Capture backend (SPEC.md §4.2): MTA on a dedicated capture
  thread, `CreateFreeThreaded` frame pool of 3, `FrameArrived` doing nothing but
  enqueueing, bounded drop-oldest queue, `IsBorderRequired` probed by property
  presence rather than OS version.
- `IScreenCapture` / `CaptureFrame` (SPEC.md §4.4) and a raw NV12 file writer, so
  capture and colour can be proven before the encoder exists.
- DXGI Desktop Duplication fallback (SPEC.md §4.3). Refuses to start when the
  device is not on the adapter owning the output, reporting
  `DDA_ADAPTER_AFFINITY` rather than duplicating on the wrong device — the
  configuration that yields an entirely black recording with `S_OK` everywhere.
  `WAIT_TIMEOUT` produces a duplicate frame with an advanced PTS; `ACCESS_LOST`
  rebuilds with 10 ms → 500 ms backoff.
- Source resolver (SPEC.md §4.1): displays by EDID-derived stable id rather than
  index, windows by HWND with process-name + class re-acquisition. Cloaked and
  tool windows are filtered out.
- HDR tone-map branch (SPEC.md §4.2): an FP16 scRGB source is tone-mapped to SDR
  BT.709 rather than reinterpreted. Diffuse white is configurable
  (`sdr_white_nits`, default 203 per ITU-R BT.2408); highlights above it roll off
  through a soft shoulder instead of clipping to flat white; out-of-gamut negatives
  clamp rather than wrap. SDR-range content through this path produces values
  identical to the SDR path, which is asserted. With `hdr_tonemap = false` an FP16
  source is still refused rather than misread.
- Synthetic test-pattern source (SPEC.md §20.1, CLAUDE.md §5): 75% SMPTE bars, a
  moving bar, and a 24-bit frame-index barcode carried in luma so it survives 4:2:0
  subsampling. Implements `IScreenCapture`, so tests no longer depend on what
  happens to be on screen, and a decoded frame can be matched back to the frame
  that produced it — the mechanism §20 row 5 needs.
- **Video encode and MKV mux (M3).** A recording now runs end to end: capture →
  BGRA→NV12 conversion → H.264 → Matroska, producing a playable 1080p60 file on
  both adapters.
- `IVideoEncoder` over `h264_nvenc` / `h264_amf` through libavcodec, with no
  direct vendor SDK calls (SPEC.md §2.2 item 1). Input is NV12 in an
  `AV_PIX_FMT_D3D11` hardware frames context, so frames never touch system memory.
  H.264 High @ 4.2, CQP 20 by default, 2 s GOP with no open-GOP, B-frames 2.
  Non-H.264 codecs are refused rather than silently downgraded (SPEC.md §9).
- Colour is tagged at all three sites SPEC.md §6 requires — encoder context, SPS
  VUI, and the Matroska Colour element — and asserted by decoding the output.
- MKV muxer with the single-writer discipline of SPEC.md §10.1: one thread owns the
  `AVFormatContext`, packets arrive over a bounded queue, and interleaving is left
  to `av_interleaved_write_frame`. 2 s cluster limit, CRC32 off, Cues on finalize.
- Finalization follows SPEC.md §10.4 and **validates before reporting success**:
  the finished file is reopened, demuxed and decoded, and the report carries stream
  count, codec, duration and decoded-frame count. A file that does not decode is
  not a successful recording.
- CFR frame pacing (SPEC.md §7.2) as a pure function over `(qpc_ns, t0_ns, fps)`,
  in integer arithmetic so a four-hour recording quantizes exactly. Duplicates are
  emitted deliberately with grid-derived PTS — the fix for SPEC.md §20 row 7 — and
  a source outrunning the grid is capped rather than allowed to reorder timestamps.
- Pacer **selection-evenness** tests. Frame count, duration and PTS spacing can all
  be correct while the frames that survive downsampling are bunched — the long
  gaps clustered instead of interleaved — which reads as micro-stutter under
  motion. No aggregate assertion can see it, because the clumped and even
  selections have identical totals; only ordering differs. The check bounds the
  spread of the selection's deviation from the ideal ramp, covering 144→60,
  144→30, 165→60, 120→60, 60→60 and the slow-source 48→60 duplicate case, with a
  negative control that fails if the check ever stops discriminating.
- **Audio subsystem (M4, partial).** WASAPI loopback capture, the silence
  generator, drift compensation, resampling to canonical format, AAC-LC encoding
  and an audio stream in the muxer. Verified against the reference rig's endpoint
  (Realtek, 48 kHz, 2 ch, 32-bit float).
- The **silence generator** (SPEC.md §8.2) — the defect SPEC calls the #1 loopback
  sync bug, and a bug of omission. WASAPI emits nothing at all when no application
  is playing, so a build that writes packets as they arrive records 30 minutes of
  video with 22 minutes of audio, with everything after the first silence
  progressively early. The timeline is now authoritative rather than the packet
  stream: any interval no packet covers is filled with silence of exactly the
  missing duration. `AUDCLNT_BUFFERFLAGS_SILENT` is filled rather than skipped, and
  `DATA_DISCONTINUITY` is bridged rather than concatenated.
- **Drift compensation** (SPEC.md §8.4) with the graded ladder: under 5 ms nothing,
  5–40 ms a soft `swr_set_compensation` resync spread over ~10 s, 40 ms+ a hard
  resync. A 50 ppm clock — an ordinary consumer crystal, and 180 ms an hour if left
  alone — is caught at ~100 s while still in the soft band.
- **Channel layout signalled at all three sites** SPEC.md §8.5 requires: the
  `AVCodecContext`, the AAC `AudioSpecificConfig` (via `AV_CODEC_FLAG_GLOBAL_HEADER`),
  and the container. The muxer now checks the layout and the config survived the
  copy rather than assuming, because getting two of three right still plays 5.1 as
  stereo (§20 row 17).
- The audio channel layout is **pinned for the file's lifetime**. If the endpoint
  changes mid-recording — a 7.1 headset unplugged — libswresample remixes into the
  pinned layout, because changing an AAC stream's channel count mid-file is invalid
  in both containers (SPEC.md §8.5, §14.1).
- Throughput measurements (`test_throughput.cpp`, gpu tier). Every capture-rate
  figure quoted before this came from a test that reads every frame back to system
  memory, which measures the harness rather than the engine. Three cases now report
  real numbers: capture+convert with no readback, the distribution of capture
  inter-arrival gaps, and the full encode pipeline driven from the synthetic source.
  Assertions are loose by design — these exist to produce numbers, not to fail on a
  busy machine.
- Variable-source-rate pacer coverage. A game's frame rate wanders — 82, then 97,
  then 110 — and under VRR the capture timestamps wander with it, which none of
  the fixed-rate tests exercised. Covers a sawtooth rate, uniformly random frame
  intervals, a 200 ms stall, crossing below the target rate and back, a
  near-multiple beat frequency, and a simulated 30-minute run. The invariants hold
  throughout: PTS exactly on the grid, strictly increasing, timeline contiguous,
  emitted count equal to duration × fps with no accumulated drift.
- **Content staleness is now measured**, on the pacer and through `PipelineStats`.
  SPEC.md §7.2 keeps the *first* frame that maps to a slot and discards any fresher
  one arriving later in the same slot, so the content shown lags the slot it is
  shown in. The mean lag is a constant offset and harmless; the variation is what a
  viewer could notice, and it was previously invisible at runtime. Measured result:
  **the wobble equals exactly one source frame interval** — 6.9 ms at 144 Hz,
  8.3 ms at 120 Hz, 12.2 ms at 82 Hz.
- `BoundedQueue` with explicit per-queue drop policy (SPEC.md §12): drop-oldest for
  video, block for audio, every drop counted. The encoder input queue is capacity 8
  per SPEC.md §9.
- RAII wrappers for `AVFrame`, `AVPacket`, `AVCodecContext`, `AVBufferRef`,
  `AVDictionary`, and separately for input and output `AVFormatContext` — the two
  have different destructor pairings and confusing them leaks the file handle.
- MP4 is refused with `INTERNAL_NOT_IMPLEMENTED` rather than half-written. Its
  crash-safe fragmented recipe is M5, and a progressive MP4 written now would be
  exactly the unplayable-after-crash file SPEC.md §10.3 exists to prevent.

- Capture backend selection (SPEC.md §4.2, §4.3): `capture::select_backend` is a
  pure policy over probed availability, and `create_capture` builds the chosen
  backend. `advanced.capture_backend` now has a consumer — before this it was a
  documented config key that nothing read. `auto` prefers WGC; an explicit `wgc`
  or `dda` is honoured or fails, never silently falls back, because a forced
  backend exists to diagnose the backend it was forced away from; a window target
  with `dda` is refused rather than substituted with a full-screen capture. Every
  selection carries a rationale string into the log.
- End-to-end pipeline tests driven by the synthetic source rather than the live
  desktop: 30 frames through source → converter → disk, with the decoded barcode
  asserted to be exactly indices 0..29 in order, no gaps and no repeats. This is
  the frame-identity check SPEC.md §20 row 5 calls for, and it now also runs
  identically on both adapters.
- WGC capture verified on an adapter that does *not* own the output — a device on
  the RTX 4050 capturing the 780M-owned display, with frames arriving on the 4050
  and converting to usable NV12. This is the exact configuration DDA refuses in
  `DdaRefusesADeviceOnTheWrongAdapter`, so the pair documents why the project
  carries two backends at all (SPEC.md §5.1).
- GPU topology service (M1): adapter enumeration with LUID identity, Software /
  Integrated / Discrete classification, output enumeration, `HMONITOR` → adapter
  ownership resolution, and per-adapter encode capability established by opening a
  real encoder session rather than by vendor-id heuristic. Results cached by
  (LUID, driver version).
- Encoder selection policy (SPEC.md §5.2) as a pure function, so the MUX-less
  laptop case is exercised on the CPU test tier. Rule 2 requires a *measured*
  cross-adapter transfer cost; an unmeasured cost cannot satisfy it.
- The NV12 encoder-input pool's `BindFlags` is now probed per adapter rather than
  assumed (BUG-001). The probe asks for `DECODER | UNORDERED_ACCESS` first and
  falls back to `DECODER` alone, and it verifies the planar `R8_UNORM` /
  `R8G8_UNORM` views are creatable before reporting `UNORDERED_ACCESS` as usable —
  an accepted texture desc does not imply an accepted view.
  `EncoderCapability::nv12_pool_is_uav_writable()` reports the outcome.
- GPU test tier: `fc_gpu_tests`, labelled `gpu`, so `ctest -L gpu` selects the
  hardware tier and `-LE gpu` selects the tier that runs anywhere. GPU tests fail
  rather than skip when hardware is absent.
- The session preamble's adapter enumeration and display topology are now real data
  instead of the M1 placeholder.
- `docs/GPU_HANDLING.md`.
- libav* diagnostics are routed into the FrameCapture logger. FFmpeg previously
  wrote to stderr, bypassing every sink SPEC.md §18 requires and printing vendor
  chatter ("AMF via D3D11.") into the engine's own output.

### Known issues

- **Five GPU-tier capture tests depend on the real desktop changing, and fail on a quiet
  machine (BUG-033).** `SustainedCaptureTest` (4 cases) and
  `CaptureToNv12Test.FrameSequenceNumbersAdvanceMonotonically` capture the live display,
  and WGC emits a frame only when the composited image changes, so their input is
  whatever happens to be on screen. Demonstrated, not inferred: re-running them with a
  window animating at 60 Hz turns them green and takes
  `CrossAdapterWgcFramesConvertToUsableNv12` from 5689 ms of timeouts to 1167 ms, and a
  full Release GPU tier run that way is 156/156 in 461 s. The precise failure trigger is
  *not* pinned down — Debug and RelWithDebInfo passed on the same quiet desktop minutes
  later, and the failing run was also 65% slower, so load is likely the second
  ingredient. No product defect; these tests need to present their own animated pattern
  to the target output. **Until then, a GPU-tier run on an idle machine is not a
  trustworthy signal for these five.**
- **The audio endpoint migration cannot be tested against a genuinely different
  device on the reference rig.** It has one `eRender` endpoint ("Speakers (Realtek(R)
  Audio)", 48 kHz 2ch). The format change is verified on the CPU tier and the live
  WASAPI handover on the GPU tier, but the two together — a real device change whose
  replacement has a different format — needs a second render endpoint. §20 row 12 stays
  **Partial** for that reason and no other; see docs/ACCEPTANCE.md for what would close
  it.
- **BUG-021 — MP4's audio track starts 21 ms late.** A fragmented MP4 cannot
  declare the AAC encoder's 1024-sample priming: `empty_moov` writes the header
  before any duration exists, so there is nowhere to put an edit list, and a reader
  presents the priming as content. Matroska is unaffected — `CodecDelay` carries the
  number and the decoder skips the samples. Bounded to the head of the track: the
  steady state through MP4 measures −187 µs, the same as MKV. Two fixes were tried
  and rejected (setting `initial_padding` has no effect on movenc; shifting the
  packets negative moves the whole track early instead), and the remaining work is a
  design question about how the finalize declares the delay rather than a remux
  tweak. `AvSyncTest.TheBeepAndTheFlashStayInSyncThroughMp4` pins it between 15 and
  25 ms, so it fails if the defect is fixed *or* if it grows.

### Changed

- **SPEC.md §8.4 ratified.** §8.2 and §8.4 each specified a complete answer to the
  device-clock problem and running both corrected the same error twice. §8.2's
  timeline is now stated as the correction and §8.4's ladder as the measurement,
  with `audio_samples_written` defined as samples handed to the encoder rather than
  samples the device delivered. The 30-minute loopback recording is recorded there
  as the evidence: a 14.4 ppm crystal gained 26 ms while the encoded track stayed
  38 ms from the video track. The zero-crossing alignment of the hard band remains
  a noted deviation.
- **SPEC.md §8.5 corrected** to name `AV_CH_LAYOUT_5POINT1_BACK` and to state the
  normalisation of a side-channel endpoint mask, with the 7.1 interop caveat
  written down (see BUG-017).
- **CLAUDE.md §6 corrected.** It asserted the agent has no GPU and cannot verify the
  hardware tier. This machine *is* the reference rig; the section now says to run
  the GPU tier, and records what that mistaken assumption cost — most of M4 spent
  deferring runnable verification, with BUG-019 sitting undetected behind it.
  §3 gains the environment overrides for the long-running tests and the
  clang-tidy database regeneration step.

### Fixed

- **The GPU tier now keeps the display powered while it runs (BUG-033, second cause).**
  Windows turns the display off after 600 s of no input on this rig, and a three-preset
  tier takes ~25 minutes — so an unattended run crosses the timeout partway through, DWM
  stops compositing, `IDXGIOutput1::DuplicateOutput` starts failing, and every test that
  touches a real output fails from there. Caught in the act: an unattended verification
  run passed Release 159/159 at 17:57, failed **8 capture cases** on RelWithDebInfo at
  18:05 with the fixture reporting *91 frames presented, 1 delivered*, and passed Debug
  159/159 at 18:14 after a keystroke woke the screen. `gpu_test_main` now holds
  `ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED` for the life of the process
  and clears it on exit. This is what explains the two `DuplicateOutput` failures the
  ambient-content fix below could not.
- **Eight GPU-tier capture tests reported the desktop's state as a verdict on the code,
  and now present their own input (BUG-033).** WGC composites only when the screen
  changes, so `SustainedCaptureTest` and `CaptureToNv12Test` — which duplicate a real
  `IDXGIOutput` and therefore cannot use `SyntheticSource` — captured whatever happened
  to be on screen. Five of them failed a Release tier on a quiet machine and passed
  minutes later with a window animating in front of them.

  `tests/fixtures/screen_animator.h` covers the target output with a topmost window and
  repaints it at a stated rate, so the input is the test's: what is captured is a pattern
  this process drew (asserted per frame via `pattern_signature`), and frames arrive
  because this process presented them. `fps = 0` holds one picture, which is how the
  duplicate-frame case now gets a *provably* idle desktop instead of an unattended one.
  Measured: `SustainedCaptureToNv12StaysCorrect` 240 of 240 frames carrying the pattern;
  `DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` 20 frames, 20 duplicates, 0 repaints.
  New `ScreenAnimatorTest` (gpu, 3) tests the fixture itself — and caught a first version
  that left all eight cases green while contributing nothing to frame delivery.
- **`CfrExactnessTest.DroppedFramesCostContentButNeverSlots` can no longer go vacuous.**
  It fed as fast as it could against a 50 ms-per-four-writes stall — a ~80 packets/s
  drain against "however fast this build's feeder is" — so whether it dropped anything
  was build-dependent, the exact race BUG-027 was about. It now feeds at a wall-clock
  60 fps against a 300 ms stall (~13 packets/s), both rates configured, and asserts on
  the union of queue drops and paced-out frames instead of returning early when nothing
  was dropped. It also needed `prerendered_frames = 8` — BUG-027 a third time: setting a
  feed rate is not the same as being able to hold it, and a full Debug tier caught the
  feeder managing **12 fps against the 13 packets/s drain** it was supposed to outrun.
  Measured after: 185 queue-dropped on all three presets (plus 8–11 paced out), where the
  spread used to be 22–182.
- **A killed MP4 recording now loses under half a second instead of two (BUG-034).**
  `frag_keyframe` ties the fragment to the GOP, so a `TerminateProcess` always cost the
  full 2 s SPEC.md §9 fixes it at — leaving row 3's "duration ≥ 28 s of 30 s" with
  **121 ms** of room, and an outcome that was binary rather than marginal: measured, a
  killed file decoded to 28.02 s or 26.02 s and nothing between.

  Two changes. `AVFMT_FLAG_FLUSH_PACKETS` moves the AVIO flush to where libavformat
  actually writes: the existing flush fired on *our* keyframe write, and because
  `av_interleaved_write_frame` queues packets to interleave them, that is usually not the
  call in which movenc closes a fragment and emits the `moof` — so a closed fragment could
  sit in the 256 KB buffer until the next keyframe. And `frag_duration = 500 ms` closes a
  fragment on time as well as on a keyframe, decoupling the loss bound from the GOP.

  Measured: loss **478–495 ms** over five runs, one of them losing nothing at all, against
  1978 ms before; unthrottled write p99 unchanged at **153 µs**. The test now asserts the
  muxer's lag at the kill (**41 ms**) and the content lost beyond it (**454 ms**)
  separately, because asserting only the total is what let this hide behind a plausible
  story about pipeline lag. `Muxer::last_video_pts_ns` and `PipelineStats::last_muxed_pts_ns`
  are new, and are how that lag became a measurement rather than an inference.
  **MKV is unchanged** — §10.2 specifies its 2 s cluster limit.
- **One burst of device failures no longer rebuilds the recording twice (BUG-035).**
  `DeviceWatcher::acknowledge` discarded reports that arrived *during* a rebuild, on the
  grounds that they describe the device stack just replaced — but nothing discarded the
  ones arriving just after, which describe the same dead device for the same reason. A
  burst was therefore coalesced for exactly as long as the rebuild happened to last:
  three `DEVICE_REMOVED` reports 50 ms apart against an ~82 ms rebuild produced **two**
  rebuilds in 6 of 22 runs. `gpu::kReportSettleNs` — one poll interval, derived from the
  slowest path by which a thread holding the old device can still report — replaces the
  coincidence with a rule, and `settled_reports()` counts what it suppresses. Measured:
  **0 failures in 20 runs**, one rebuild each, gaps 86.9–97.1 ms against a 350 ms budget.
  A genuinely new fault inside the window is delayed by up to one poll interval; the
  reasoning for that trade is in `kReportSettleNs`.
- **`WatchdogRecoveryTest.TheLadderCanTriggerARebuild` now prints one line per rebuild** —
  cause, gap, `same_file`, `failed` — because its failure text names two possible
  mechanisms and the rebuild *count* alone cannot tell them apart. It paid for itself
  immediately: both rebuilds of a doubled burst report `device_lost`, which rules out
  BUG-029's watcher-feedback path and points at the trigger's latch instead (BUG-035,
  open — a pre-existing product defect, 4 of 12 idle runs).
- **`MigrationRecord` now reports where a rebuild's time went** — `teardown_ns`,
  `discovery_ns`, `device_ns`, `pipeline_ns`, `capture_ns`, in the migration log line and
  in the test's failure text. Added because a Debug rebuild measured 777.9 ms against
  §5.4's 350 ms budget and a single total could not say why; the breakdown showed the
  product's own phases were identical to Release and the overrun was the test's pattern
  generator rendering eight 1080p frames inside the measured window (BUG-032). The
  generator's output is now memoised, and Debug and Release agree at **102–133 ms**.
- **A heap overflow on an audio endpoint migration that narrowed the format
  (BUG-031).** `AudioEncodePath::offer` sized its copy out of WASAPI's buffer from a
  shared `input_bytes_per_frame` that `apply_migration` rewrote on another thread.
  Migrating 48 kHz stereo → 44.1 kHz mono, a buffer copied at the new 4 bytes/frame
  while still holding the old endpoint's 8 read past the end of its allocation.
  `LoopbackBuffer` and the work item now carry the frame size with the bytes, and the
  shared field is gone — a size that differs per buffer cannot be held in one cell.
  Found as a one-in-332 ctest failure that re-ran green; caught by stressing rather
  than by re-running.
- **Every recording on the NVIDIA adapter was H.264 Main profile, not the High @ 4.2
  SPEC.md §9 requires.** `h264_nvenc`'s *private* `profile` option defaults to Main and
  `nvenc_setup_h264_config` **writes** `AVCodecContext::profile` from it rather than
  reading it — so the engine's `AV_PROFILE_H264_HIGH` was overwritten, and reading the
  field back reported NVENC's choice. AMD was unaffected (`h264_amf` defaults the same
  option to "derive from the context"). Latent since M3, because nothing in the suite
  read `profile_idc`: a decoder decodes Main happily, so the container, colour and
  frame-identity assertions all passed on a non-conforming file. Measured `profile_idc`
  0x4d before, 0x64 after, on both encoders. See BUG-028.
- **`test_slow_disk`'s backpressure was build-dependent, and the fix was to make the
  test's frame generator cheap rather than to keep lowering the disk's drain rate.**
  Three rounds of tuning the drain narrowed the window without closing it; the term that
  would not hold still was the *feeder*, which re-rendered a 1080p pattern and did a
  row-by-row upload for every frame.
  `SyntheticSource::Settings::prerendered_frames` renders a small cycle once and hands
  out cached textures, so Debug and Release now produce matching numbers (183
  queue-dropped either way, against 6 on Debug before). It also lifted row 6's measured
  feed rate from 57.2 fps to 60.02. Tests asserting frame identity leave the option at 0
  and are unaffected. See BUG-027.
- **A slow disk made `stop` abandon the venc thread, leaving an unfinalized file.**
  SPEC.md §12's 2-second bounded join was applied to every thread, but two of them wait
  on the disk at shutdown: `mux` directly, and `venc` transitively — the mux queue never
  drops, because §12 forbids dropping audio and an encoded video packet cannot be dropped
  either without breaking the frames referencing it, so a full mux queue blocks venc by
  design. On a volume stalled 600 ms per write the deadline fired, `stop` returned before
  `av_write_trailer`, and the ladder's gentlest rung therefore delivered the outcome rung
  7 exists to prevent. Both joins now use SPEC.md §15.1's own 30-second finalization
  deadline, reconciling the two sections; the `watchdog` keeps 2 s, being a pure
  observer. Still bounded. **`await_worker` gains an optional timeout**, defaulting to
  §12's 2 s, so the change is visible at each call site rather than hidden in a constant.
  See BUG-026.
- **Rung 3's retime made the §10.4 validation gate reject the recording it had just
  saved.** The expected duration was computed as `pacer.emitted() / configured_fps`,
  an identity that holds only while the frame rate is constant — and rung 3 exists to
  change it. The first recording in which the ladder fired was declared 30% short and
  reported as *failed* while being perfectly playable. `timing::Pacer` now reports
  `timeline_seconds()` from the last PTS it emitted, which is absolute and survives every
  rate change. See BUG-025.
- **Matroska's crash-safety guarantee was false: the clusters never reached the
  disk.** SPEC.md §10.2 promises a truncated file plays up to its last complete
  cluster, and that is a claim about the *format* — nothing was making the bytes
  durable. libavformat's output buffer here is **256 KB** (measured, not the 32 KB
  assumed), which at the bitrate a mostly-static screen produces is about
  twenty-five seconds, so a recording killed before the first flush was **zero bytes
  long**. The default container, green through M3 and M4, because every test until
  now closed its file cleanly. The keyframe flush that makes §10.3's fragments
  durable now runs for both containers. See BUG-020.
- **The synthetic source's QPC-to-nanoseconds conversion overflowed after fifteen
  minutes of uptime**, so on any machine that had been running a while its frame
  timestamps were wrapped nonsense. The wrapped epoch is *consistent*, which is why
  every M2 and M3 timing assertion passed against it — they measure video against
  itself. The first test to put a real WASAPI timestamp on the same timeline
  produced one frame of video against twelve seconds of audio. All four copies of
  the conversion now route through `core/timing/qpc_clock.h`. See BUG-019.
- **The validation gate compared a video-derived expectation against the
  *container's* duration**, which for Matroska is the longest stream — so once
  audio existed and legitimately outlived video by the length of the stop
  sequence, the check fired on healthy recordings. It now measures the video
  track's own length from decoded timestamps.
- **The test decoder reserved exactly the capacity it needed on every frame**, so
  verifying a recording was quadratic in its length: `reserve(size() + n)` before
  each append overrides `push_back`'s geometric growth and reallocates the whole
  accumulated buffer every time. M4's 30-minute run had not finished after 70
  minutes; it now takes single-digit minutes, and 300 s of media verifies in 74 s.
  Invisible at the 65 s the suite routinely runs — 3% of the runtime there, 100% of
  a 70-minute runtime at half an hour. See BUG-018.
- **5.1 was encoded with side channels, which AAC has no standard configuration
  for**, so the encoder fell back to a Program Config Element and the file reopened
  as "six channels, positions unspecified" — SPEC.md §20 row 17's headline defect,
  reached without anything going wrong in the muxer. SPEC.md §8.5 names
  `AV_CH_LAYOUT_5POINT1`; the constant AAC can actually signal is
  `AV_CH_LAYOUT_5POINT1_BACK`. An endpoint reporting the side form is now
  normalised to the back form, which relabels one pair of speakers rather than
  leaving every player to guess. SPEC.md §8.5 has been corrected to name
  `5POINT1_BACK` and to state the normalisation. See BUG-017.
- **Audio buffers dequeued before the shared epoch was published were discarded**,
  losing up to a buffer period at the head of the track depending on how the
  scheduler interleaved two threads. The gate decided from *arrival order* what the
  timeline already decides correctly from *timestamps*; such items are now held and
  replayed once `t0` lands. See BUG-016.
- **The muxer read the audio source timebase back off the stream after
  `avformat_write_header` had already changed it**, making the rescale a no-op that
  reinterpreted rather than converted. AAC PTS arrive as a sample count; matroskaenc
  forces every stream to 1/1000, so one second of audio would have been written at
  48 seconds and the whole track placed 48× too late. Both source timebases are now
  captured at open time. See BUG-015.
- **The validation gate (SPEC.md §10.4) never looked at the audio track.** It found
  the video stream, decoded frames, and compared the container's duration against
  the expectation — so a file whose audio stream was missing, carried the wrong
  channel count, or decoded to nothing was reported as a successful recording.
  `Muxer::validate` now takes a `ValidationExpectation` describing what was
  recorded and decodes both tracks. This matters ahead of M5: `test_crash_recovery`
  leans on the gate, and as it stood that test could have passed on a file with no
  usable audio.
- **Audio captured before the shared epoch was written *at* the epoch**, displacing
  the audio that belonged there. `AudioTimeline` clamped a pre-`t0` packet's start
  index to zero without trimming its leading frames, so on every recording whose
  endpoint opened before the first video frame — the ordinary case, since `t0` is
  the *later* of the two firsts — the first 20 ms of the track was audio from
  before the recording started, and the buffer that actually belonged there was
  discarded as a duplicate. Invisible to every duration assertion, because the
  timeline's length was right and only its content had moved. See BUG-014.
- **`AacEncoder::submit` could not be safely retried after backpressure.** It
  copied samples into its staging frame as it went and then returned an error from
  the middle of the loop, so the only thing a caller could do — resend the same
  frame — would have encoded the already-staged samples twice, lengthening the
  track and desyncing everything after it. `submit` now takes an offset and returns
  what it consumed. `flush` had the same shape and also set its idempotence flag
  before doing the work, so a retry after backpressure would have silently
  discarded the trailing partial frame it exists to preserve. See BUG-013.
- **`Resampler::convert` promised silence for a null input; libswresample reads a
  null input as *flush*.** A request for a silent stretch would have returned an
  empty frame, shortening the audio track by exactly the length of every silence —
  SPEC.md §8.2's headline defect, reintroduced by the component built to prevent
  it. Null inputs are now refused, `convert_silence` feeds real zeros through the
  resampler, and `flush` is named for what it does. See BUG-012.
- `scripts/bootstrap.ps1` exited with the revocation probe's status (35) instead of
  0 after a successful run.
- **The engine was DPI-unaware, so Windows reported a virtualised desktop size.**
  On a 1920×1080 panel at 125% scaling, DXGI said 1536×864 while WGC delivered a
  real 1920×1080 texture — M1's topology reported the wrong output dimensions, and
  a pipeline sized from them rejected every frame. Now declares per-monitor
  DPI awareness V2 before touching DXGI. See BUG-004; `GPU_HANDLING.md`'s measured
  table has been corrected.
- **`crash::write_artifacts` was `noexcept` but constructed `std::filesystem::path`
  objects, which allocate.** A `bad_alloc` there would have terminated the process
  inside the crash handler — exactly when the heap is least trustworthy. The
  crash path now writes into fixed-size buffers (`ArtifactBuffers`) and the
  allocating variant is a separate, non-`noexcept` reporting wrapper. See BUG-003.
- `FC_HR_LOG` used an immediately-invoked lambda, so `__FUNCTION__` logged the
  lambda's `operator()` instead of the calling function, and the capture list's
  comma made the macro unusable inside another macro
  (`EXPECT_TRUE(FC_HR_LOG(...))` failed to preprocess under clang). It now
  delegates to `detail::check_hresult`.
- `session_preamble` cast a `FARPROC` through `void*`, narrowed `0x80000000` to a
  signed `int` implicitly, and did a pointer offset from an unsigned multiplication.
- `scripts/lint.ps1` now accepts an `FC_LINT_OK` / `FC_THREAD_ENTRY` annotation on
  the line *above* an offending line, matching clang-tidy's `NOLINTNEXTLINE`
  convention — an annotation that needs a justification does not fit on the same
  line as the code it excuses.
- `GpuTopologyService` deleted its copy operations but never declared move ones, so
  it was silently immovable; caught by clang-tidy once the gate was running.
- `scripts/lint.ps1` now runs clang-tidy in parallel (~2 min instead of >10 min
  serial) and honours the `FC_THREAD_ENTRY` annotation its own `catch(...)` rule
  advertises.
- `main` now wraps its body in the thread-entry catch-all SPEC.md §19 requires.
- `scripts/lint.ps1` matched banned patterns inside string and character literals,
  so a test payload of `"new content"` tripped the raw-`new` rule. It now strips
  literals and trailing comments before matching, and still catches a genuine
  `new` expression.
- The root `CMakeLists.txt` FFmpeg checks only worked on a *fresh* configure. The
  vcpkg port's `FindFFMPEG` sets its component flags and `FFMPEG_VERSION` as normal
  variables inside a block it skips once `FFMPEG_FOUND` is cached, so a second
  configure failed. Now checks header existence and reads the version from
  FFmpeg's own `ffversion.h`.
- Added `NOMINMAX` / `WIN32_LEAN_AND_MEAN` project-wide; windows.h's `min`/`max`
  macros break `std::min`/`std::max` with a C2589 that never mentions macros.
- **clang-tidy had been silently skipping every file added since M1.** The check
  only covers files present in `compile_commands.json`, and that database was
  stale, so "lint clean" was true of a shrinking fraction of the tree. 37 real
  findings were hiding behind it across the capture, colour and test sources. See
  BUG-005; `scripts/lint.ps1` now fails instead of passing when the database is
  older than the sources it is supposed to describe.
- `DdaCapture`, `WgcCapture` and `SyntheticSource` each declared a destructor
  without copy or move operations. Copying one would have duplicated ownership of
  an `IDXGIOutputDuplication` or detached WinRT objects from the MTA thread
  allowed to touch them; all four operations are now explicitly deleted.
- **The pipeline's shutdown used an unbounded `join()`**, which SPEC.md §12
  forbids. A wedged `venc` or `mux` thread would have hung `stop()` forever, so
  the file would never have been finalized — a direct violation of CLAUDE.md §1.
  There is a concrete path to it: `mux_queue` uses the never-drop `Block` policy,
  so a mux thread stalled on disk blocks the venc thread inside `push`, which
  blocks the join. Workers now signal completion through a `std::promise` and
  shutdown waits with the 2 s deadline; on expiry it logs
  `INTERNAL_THREAD_JOIN_TIMEOUT`, detaches the worker and deliberately leaks the
  pipeline state rather than freeing memory a running thread is still reading.
  For MKV that leaves a file playable to its last complete cluster (SPEC.md §10.2),
  which beats a process that never exits.

  The constant `kJoinTimeout` had been sitting in the file since M3 with a comment
  reading *"Never an unbounded join"*, next to two unbounded joins.
- **`video.pacing = vfr` was accepted and ignored**, silently producing a CFR
  recording. The key is in the schema, validated, round-tripped and documented, but
  nothing read it — the same dead-key situation `advanced.capture_backend` was in
  before M3. SPEC.md §7.3's VFR mode is not implemented, so it is now refused with
  `INTERNAL_NOT_IMPLEMENTED` rather than quietly doing something else.
- The frame-pacer tests generated source timestamps with integer division, making
  every tick systematically *early* by up to a nanosecond. Invisible to a
  tolerance check, but it decided the outcome wherever a tick landed exactly on a
  rounding boundary — which is precisely where the new evenness tests do their
  work. Now rounds, which also models a real source's QPC timestamps more
  faithfully.
- **NVENC produced no codec extradata, so the MKV header could not be written.**
  Matroska stores SPS/PPS in `CodecPrivate` and `avformat_write_header` fails
  without them. Whether an encoder emits extradata by default turns out to be
  vendor-specific — AMF does, NVENC does not — so the recording worked on the 780M
  and failed outright on the RTX 4050. The encoder now opens with
  `AV_CODEC_FLAG_GLOBAL_HEADER`, and the muxer checks for extradata and names the
  cause instead of letting `write_header` return a generic error. See BUG-006.
- **The encoder input pool was sized like a queue, and NVENC exhausted it after 8
  frames.** A hardware encoder holds an input surface for every frame it has
  accepted but not yet emitted a packet for, and NVENC's pipeline is several frames
  deep before it produces anything. 120 submitted frames yielded 8 encoded and 111
  failures. Pool exhaustion is now reported distinctly from a real encode failure,
  and the caller responds to it as backpressure — drain packets, which is what
  releases surfaces, then retry — rather than dropping the frame. See BUG-007.
- **BUG-001 was diagnosed as an AMD quirk and was not one.** `D3D11_BIND_DECODER`
  is required for an NV12 texture *array* on both reference adapters, not just the
  780M; a single NV12 texture requires no particular flag. The original probe only
  tested `ArraySize = 20` with `DECODER` first in its candidate list, so it
  returned the right answer with the wrong explanation, and that explanation had
  propagated into `adapter_info.h` and a test comment as fact. Both corrected.
  This also resolves the M3 blocker: `DECODER | UNORDERED_ACCESS` is accepted on
  both adapters with planar UAVs creatable over a pool slice, so the conversion
  shader can write NV12 straight into an encoder pool and no device-to-device copy
  is needed.
- `scripts/lint.ps1` now finds clang-format and clang-tidy in a pip-installed
  fallback location, so the lint gate does not depend on the optional Visual
  Studio "C++ Clang tools" component being present. See `docs/BUILD.md` §4.4.
