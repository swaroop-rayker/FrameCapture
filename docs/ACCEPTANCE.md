# Acceptance Matrix — SPEC.md §20 rows → the tests that cover them

SPEC.md §20 says *"Each row must have a named, automated test. A row without a
green test is an incomplete feature."* and M10's exit criterion is **all 22 rows
green** (eighteen until M9.6 added four; §0.3 question 1, answered by the owner on
2026-09-06). That audit needs something to audit against, because the test names in the
tree do not match the names in the spec — deliberately, in most cases: SPEC.md
names a *subject* (`test_no_black_frames`), and GoogleTest names in this repo are
assertions (`NoFrameIsEverUniform`). A file called `test_no_black_frames.cpp`
containing a test called `Works` would satisfy the spec's letter and none of its
intent.

This file is the mapping. **It is a merge gate in the same sense `docs/CONFIG.md`
is: a row that gains or loses coverage gets updated in the same commit.**

Status values:

- **Green** — covered by a passing automated test, on the tier named.
- **Partial** — something covers part of the row; what is missing is stated.
- **Pending** — no coverage, with the milestone that owns it.

**As of M9.6 (2026-09-07): 21 Green, 1 Partial, 0 Pending.** The one Partial is row 12,
and what it is missing cannot be supplied by writing a test: this rig has a single render
endpoint, so `IMMNotificationClient` firing on a genuine device change needs a second
audio device. Every other row has a named, passing test on the tier stated.

Suite sizes the rows above were measured with: **CPU 434**, **GPU 200** (and 199 + 1
skipped when the GPU tier is run as a single process, which is the stricter form — see
`TESTING.md`), **pytest 312** hardware-free plus **17** engine-backed. Green on the
release preset; the M9.5 figures were green on all three.

---

## The 22 rows

| # | Symptom | Status | Milestone | Covering tests |
|---|---|---|---|---|
| 1 | All-black video | **Green** | M2 | `SyntheticPipelineTest.NoFrameIsEverUniform` (variance on *every* frame, not a sample), `CaptureToNv12Test.*` and `SustainedCaptureTest.*` (both adapters, live display), `VideoEncodeTest` black-frame check after decode. The live-display cases **present their own input** as of 2026-08-02 (BUG-033): they cover the target output and assert per frame that what was captured is the pattern they drew — measured 240 of 240 on the sustained case — so a green run no longer depends on what was on screen |
| 2 | Wrong file type / unplayable | **Green** | M3 | `VideoEncodeTest` container assertions (format name, stream count, codec ids, decoded back in-process); `Muxer::validate`'s gate runs on every recording |
| 3 | Corrupted file after crash | **Green** | M5 | `CrashRecoveryTest.*` — real `TerminateProcess` against `fc_crash_recorder`. **Fixed 2026-08-02 (BUG-034):** a killed MP4 used to lose 1.98 s and occasionally 3.98 s; it now loses **478–495 ms**, measured over five runs, and one run lost nothing at all. Two changes: `AVFMT_FLAG_FLUSH_PACKETS` puts the AVIO flush where libavformat actually writes (BUG-020's flush fired on *our* keyframe write, which the interleaver means is usually not the call that closes a fragment), and `frag_duration=500ms` decouples the loss bound from the 2 s GOP. The case now asserts the two terms separately — mux lag at the kill (**41 ms**) and content lost beyond it (**454 ms**) — each against a 1.0 s bound, where the old single assertion had 121 ms of room. **MKV keeps a 2 s bound:** SPEC.md §10.2 specifies `cluster_time_limit=2000`, so its half of the row still carries the thin margin — see the note below |
| 4 | A/V sync drift | **Green** | M4 | `AvSyncTest.*` (gpu) — beep + flash from one clock, decoded from the finished file, worst offset **−187 µs** over 65 and over 1800 marks, and the same through **MP4**. `AudioPathTest.EveryBeepLandsWhereTheSharedEpochSaysItShould` (cpu) covers the audio half without hardware. See BUG-021 for MP4's head artefact |
| 5 | Glitched / torn frames | **Green** | M2/M3 | `SyntheticPipelineTest.EveryFrameOnDiskIsTheFrameThatProducedIt` (barcode identity, no gaps or repeats), `VideoEncodeTest` frame-identity-after-encode |
| 6 | Low frame rate | **Green** | M6 | `SustainedTest.SixtyFpsIsSustainedUnderHeavyGpuLoadWithFewerThanHalfAPercentDropped` (gpu) — paced 1080p60 against a 4K compute stressor on its own device, with the load level *measured* from the `GPU Engine` PDH counters rather than assumed. Measured at row 6's full ten minutes (`FC_SUSTAINED_SECONDS=600`): **0 of 36,000 frames dropped (0.000%) against the 0.5% limit**, busiest GPU engine **median 99.01%** (min 88.44%, max 100%) over 629 samples, file valid with 36,000 frames and 600.0 s of video, rung 0 throughout, 0 retimes. `TheSameRecordingWithNoGpuLoadDropsNothing` is the unloaded control at 40.75% median. See the feed-rate caveat below. `ThroughputTest.*` still reports the idle ceiling and still asserts loosely, by design |
| 7 | Repeated / jerky frames | **Green** | M6 | `CfrExactnessTest.*` (gpu) — exact frame count and PTS-grid assertions on the decoded file, in **both** containers, plus a case proving a dropped frame costs content and never a slot. `FramePacer.*` (cpu) still covers the pacer exhaustively as a pure function. See the container note below: "all PTS deltas identical" is achievable in MP4 and arithmetically impossible in MKV |
| 8 | Colour tint / washed out | **Green** | M3 | `ColorConversionTest.*` (SMPTE patch accuracy), `VideoEncodeTest` VUI/container colour tags asserted after decode |
| 9 | Frame freeze / stuck | **Green** | M7 | **Fixed 2026-08-02 (BUG-035):** one burst of device failures used to produce **2** rebuilds instead of 1 in **6 of 22** idle runs — the burst was coalesced only for as long as the rebuild happened to last, because nothing discarded reports arriving just *after* `acknowledge`. `gpu::kReportSettleNs` (one poll interval, derived) now drops reports that describe the replaced device stack, counted in `settled_reports()`. Measured after: **0 failures in 20 runs**, exactly 1 rebuild each, gaps 86.9–97.1 ms. Pinned on the CPU tier by `DeviceWatcher.ReportsArrivingJustAfterARebuildDescribeTheDeviceItReplaced` and its negative control. `WatchdogRecoveryTest.*` (gpu), all three against the synthetic source. Measured over 4 runs on **both** Debug and Release, which agree because the cost is driver time: an injected fault rebuilds the session **once** in **102–133 ms** against a 350 ms budget, `TheLadderCanTriggerARebuild` reaches the same outcome through rung 4 at **103–160 ms**, and `AHealthyRecordingIsNeverRebuilt` proves a healthy 6 s recording (361 frames) is rebuilt **zero** times with **zero** stall episodes. Every migration now reports a phase breakdown (teardown/discovery/device/pipeline/capture) — see BUG-032 for why that exists and what it caught. `HealthMonitor.NoFrameForThreeFrameIntervalsIsReportedAsAStall` (cpu) pins the detector. **Row 9's threshold is a detection threshold, not an action one** — see BUG-029. **Amended 2026-08-04 (BUG-045): the *action* no longer keys on silence at all.** A push-only backend says nothing when the desktop is idle and nothing when it is dead, so the rebuild keys on `IScreenCapture::running()` instead — a fault the backend names. `ABackendThatIsQuietButHealthyIsLeftAlone` and `ABackendThatHasDiedIsRebuilt` (gpu) are the discriminating pair: **0 rebuilds through 8 s of healthy silence** (detector still reporting 7977 ms), **75.3–85.6 ms** recovery when the backend actually died, against row 9's 500 ms |
| 10 | Frame drops | **Green** | M6 | `SlowDiskTest.*` (gpu), using SPEC.md §20.1's sanctioned disk-stall injection. Green on **all three presets**; measured on Release — degradation: write P99 **312 ms**, 183 of 300 frames shed, rung 3, one retime, **file valid with 195 decodable frames**. Rung 6: P99 **604 ms** against the 500 ms threshold, rung 6 reported, no stop requested, file valid with 178 frames. Unthrottled control: P99 **0.16 ms**, rung 0, zero shed. `BoundedQueue.*` (cpu) still covers the drop policy and counters. See BUG-026 (the prime-directive violation this row found) and BUG-027 (two build-dependent races in earlier versions of the test) |
| 11 | Mid-recording GPU switch | **Green** | M7 | **Restored 2026-08-02:** the orchestration half rested on `WatchdogRecoveryTest`, which was rebuilding twice for one burst (BUG-035, now fixed — a burst is coalesced by `kReportSettleNs` rather than by however long the rebuild took). Measured after: 0 of 20. `GpuMigrationTest.*` (gpu) covers the pipeline half: a same-adapter rebuild keeps one continuous decodable file (120 submitted → 118 decoded, the 2 reserved slots; **22.2 ms** against §5.4's 350 ms budget), a cross-adapter rebuild is **refused** with `GPU_MIGRATION_FAILED` and the pre-refusal file still finalizes playable, and 5 rebuilds in one recording stay in budget with PTS monotonic. `WatchdogRecoveryTest.AnInjectedDeviceLossRebuildsTheSessionWithinTheBudget` (gpu) covers the orchestration: an injected `DXGI_ERROR_DEVICE_REMOVED` drives all eight §5.4 steps with **one** rebuild in **102–133 ms** across Debug and Release, one segment. Phase breakdown on Release: teardown 0.07 ms, discovery 3.8 ms, device 47.4 ms, pipeline 39.2 ms, capture 11.4 ms. Detection: `DeviceWatcher.*` (cpu, 14) and `GpuTopologyTest.TheDeviceWatcher*` (gpu). §5.4 and this row were **amended 2026-07-30** on `MigrationParameterSetTest` (gpu, 4) |
| 12 | Audio device change | **Partial** | M7 | Both halves covered, on both tiers. **Format change (cpu):** `AudioPathTest.AnEndpointChangeKeepsTheTimelineContinuousAndTheOutputFormatConstant` and `MigratingToAnIdenticalFormatChangesNothingButTheCount` drive §14.1's arithmetic across a synthetic 48 kHz stereo → 44.1 kHz mono change: timeline **2.15 s** for 1.0 s + a 150 ms gap + 1.0 s, silence **exactly 0.15 s** covering the gap, AAC output **unchanged** at 48 kHz stereo. **Live handover (gpu):** `AudioDeviceMigrationTest.ALiveEndpointMigrationKeepsRecordingAndStaysInsideTheGapBudget` stops and reopens a real `LoopbackCapture` mid-recording — **gap 15.2 ms against §14.1's 200 ms target**, timeline 3.059 s over a 3.0 s recording, 145 packets with 70 before the seam, output format constant; `TheSameRecordingWithoutAMigrationHasTheSameShape` is the control at 3.020 s, and `TheSystemsRenderEndpointsEnumerateWithFormatsAndExactlyOneDefault` pins the enumeration. **BUG-031** was found here: `BuffersQueuedAcrossAMigrationAreReadAtTheSizeTheyWereWrittenAt` (cpu) drives 20 alternating 2ch↔1ch migrations across 240 buffers with the `aenc` thread deliberately behind, and is measured under an eight-process stress at **600 runs, 0 crashes** against 8 on the pre-fix build. **Missing, and why it stays Partial:** this rig has **one** render endpoint (measured: "Speakers (Realtek(R) Audio)", 48 kHz 2ch), so the live migration is to the *same* device and `IMMNotificationClient` firing on a genuine device change is **not verifiable here**. Needs a second render device |
| 13 | Orphaned engine process | **Green** | **M8a** (decided 2026-08-02, see below) | `OrphanPreventionTest.*` (gpu, 3). `KillingTheHostLeavesNoEngineAndAFinalizedFile` spawns `fc_gui_host` — which plays SPEC.md §3.1's GUI role and nothing else: Job Object with `KILL_ON_JOB_CLOSE`, suspended spawn, `hello` handshake, 1 s heartbeat — records for 3 s, then `TerminateProcess`es it. Measured on **all three presets** against row 13's **6000 ms** — Release **5209 ms**, RelWithDebInfo **5713 ms**, Debug **5637 ms** — each leaving a file **valid with 439–445 frames / 7.3–7.4 s** and **no sidecar**. The three agree because the cost is dominated by §3.1's 5 s heartbeat timeout, which is wall clock and not build-dependent. `AHostThatShutsDownCleanlyAlsoLeavesNoEngineAndAValidFile` is the control at **0 ms**, 181–182 frames — without it, an engine that discarded the recording on any host death would pass the first case equally well. `ASecondEngineRefusesToStart` pins §3.1's named mutex at `IPC_ENGINE_ALREADY_RUNNING`. **The margin is thin by construction and it is the owner's to look at** — see the note below |
| 14 | Multi-track desync | **Green** | M9.5 | `MultitrackTest.EveryTrackCarriesItsOwnToneWithTheSameDurationAndStaysInSync` (gpu) — four tracks (system mix + three applications), each carrying a distinct tone from one QPC epoch, decoded back out of a real MKV. Measured at the routine 20 s: **60 marks compared across 3 tracks, worst per-track offset −62 µs against row 14's 20 ms**; track lengths within 20 ms of the system mix's 20.010 s; every track loudest at its *own* frequency by more than 4×, which is the assertion a muxer routing every packet to stream 0 fails. §8.6's exit-criterion form is `FC_MULTITRACK_SECONDS=1800` — see the measurements below. On the CPU tier `AppAudioTracksTest.*` (6) covers the per-track timeline, silence and epoch arithmetic without hardware |
| 15 | Multi-track process exit | **Green** | M9.5 | `MultitrackTest.ATargetThatExitsMidRecordingLeavesATrackSilencePaddedToFullDuration` (gpu) — the **real** `ActivateAudioInterfaceAsync` client against `fc_audio_target`, a helper process that renders a 1500 Hz tone for 4 s and then exits, inside a 12 s recording on the real endpoint. Measured: **400 buffers captured, 0 QPC fallbacks**, the track **12.010 s against the system mix's 12.010 s** with 7.986 s of it manufactured silence, `target_exited` true, client torn down. The content check is what makes this row 15 rather than a length check: **1500 Hz energy 0.0433 before the exit and 0.000000 after**. Verified red-before — without the watchdog continuing to tick a detached track the file carries **4.011 s against 12.010 s**. This case found BUG-047. `RealTargetTest.*` (gpu, 4) covers the rest of the real client: six tracks at once, per-track isolation across five simultaneous applications, `INCLUDE_TARGET_PROCESS_TREE` against a child process, and an application at 44.1 kHz on two streams — see "Real applications" below |
| 16 | Multi-track on MP4 | **Green** | M9.5 | Three layers, each asserted separately. `MultitrackTest.MultiTrackOnMp4IsRefusedByTheEngineAndNotOnlyByTheGui` (gpu) refuses at `VideoPipeline::start` with `MULTITRACK_REQUIRES_MKV` and leaves no file behind; `TheSameConfigurationOnMkvIsAccepted` is its positive control. `test_the_engine_refuses_multi_track_audio_on_mp4` (pytest) sends the `configure` command §8.6 names, from a separate process, and asserts code **3021** *and* that nothing was half-applied. `Muxer::open` refuses independently as the last line — with both the pipeline and the muxer checks removed the gpu case goes red, which is how the defence in depth was verified rather than assumed. The GUI half is `test_multi_track_on_mp4_is_explained_inline_rather_than_silently_greyed` (pytest), row 16's "inline reason string" |
| 17 | Wrong channel layout | **Green** | M4 | `ChannelLayoutTest.*` (gpu) — 5.1, 7.1, stereo, pinned and auto, **in both containers**, asserting the container mask, the decoder-derived layout, and per-channel tone identity. `AudioEncodeTest.*` covers signalling sites 1 and 2 on the encoder. **Amended 2026-08-06 (BUG-048):** §8.5's pin overrides the endpoint *downward only* — `AudioCaptureTest.ALayoutWiderThanTheEndpointFallsBackToTheEndpointsOwn` (gpu) drives the reported 7.1-on-stereo case and its three controls. **Read the note below before quoting this row:** every assertion in `ChannelLayoutTest` was true of a file whose audio Windows would not play |
| 18 | Pause/resume desync or freeze | **Green** | **M8a**, amended **2026-08-03** | Two cases with **two different clocks**, because the first alone was not the row. **Synthetic clock** — `PauseResumeTest.*` (gpu, 3): `ThreePausesExciseTheirTimeFromBothStreamsAndNothingElse` runs row 18's shape, three *uneven* pauses (1.5 s, 0.4 s, 2.7 s) totalling **4600 ms**; the file holds **exactly 420 frames**, worst A/V offset **−0.188 ms** against the 20 ms limit, growth **−0.188 ms**, **0 repeated barcodes, 0 duplicates, 0 stragglers**, byte-identical on all three presets. **Real device clock** — `LoopbackPauseTest.PausingARecordingDoesNotDesyncTheAudioTrackFromTheDeviceClock` (gpu): `AudioSource::SystemLoopback`, real time, `pause()`/`resume()` reading QPC themselves. Worst drift **+9 / +8 / +7 µs** on release / relwithdebinfo / debug against §8.4's 40,000 µs hard band, **0 soft and 0 hard resyncs on all three**, over 2×4 s with a ~2005 ms pause. Unlike the synthetic case the figures are not identical across presets — the feed rate is not (382 / 288 / 194 frames) — and that is the point: the quantity being measured is the one a supplied clock cannot produce. Deterministic form on the CPU tier: `AudioPathTest.APausedSpanIsExcisedFromTheDriftReferenceAsWellAsFromTheTrack`. Also `RedundantPausesSucceedAndStoppingWhilePausedStillYieldsAValidFile` (§7.5's idempotence and stop-while-paused), `PauseClock.*` (cpu, 12), and `test_a_second_recording_in_the_same_engine_process_runs` (pytest). **Read "What each of row 18's two clocks proves" below before quoting this row** — the synthetic case was Green on its own for a fortnight while the live path was corrupting audio |
| 19 | Overlay in the recording, or a black rectangle where it was | **Green** | M9.6 | `OverlayExclusionTest.*` (gpu, 6) — a real capture on **both** backends, with the overlay window actually on screen, measured by mean luma and by the fraction of the frame the overlay covers. Stable over four consecutive full-suite runs in one process: unstamped control **145.8** mean luma / **1.0000** overlay fraction on WGC *and* DDA — 145.8 being exactly the BT.709 luma of the test overlay's orange, so the control proves the capture can see it; `WDA_MONITOR` **0.0 / 0.0000**, which is the black-cutout defect measured rather than described; `WDA_EXCLUDEFROMCAPTURE` **255.0 / 0.0000** on both backends, the fixture's white bar coming through intact. **DDA honours the affinity**, which is the finding that made the planned WGC-only fallback ladder unnecessary. `test_overlay_exclusion.py` (pytest, 9) covers the primitive, the read-back verification, re-application on `WinIdChange`, and the popup guard. `WDA_MONITOR` is a banned pattern in `scripts/lint.ps1`, and `SetWindowDisplayAffinity` may appear in exactly one module |
| 20 | Overlay control unresponsive, or clicking it minimizes a fullscreen game | **Green** | M9.6 | Two halves, because a frozen pill has two independent causes. **The loop stays alive** — `test_the_event_loop_keeps_running_while_a_recording_is_finalized` (pytest, engine-backed, both containers): a 50 ms `QTimer` counts how often the GUI thread got control during a **real** `stop_record`. Measured **4 ticks (MKV) and 5 ticks (MP4)**, where the old synchronous stop gives **zero by construction**. **The widget actually repaints** — `test_overlay_responsiveness.py` (pytest, 6): **9.8 paints/s** against row 20's floor of 8 over a mocked save at the engine's 10 Hz cadence, and 50 of 50 progress updates reaching the screen. Paint cost measured inside `paintEvent`: **mean 0.124 ms, worst 0.459 ms** over 127 real paints. **The click is acknowledged locally** — stop **0.120 ms**, pause **0.022 ms**, both against a 16.7 ms frame and both measured with no engine attached, so the number is the acknowledgement and not the round trip. **Activation** — `WindowDoesNotAcceptFocus` and `NoFocus` asserted directly. See the note below for what the 3% budget figure is and is not |
| 21 | A bound hotkey does nothing | **Green** | M9.6 | `test_hotkeys.py` (pytest, 70). Three areas, each of which was a real way for the row to happen. **The virtual-key table** went from F1–F24/A–Z/0–9 to the full set a user reaches for — `Home`, `End`, `Insert`, `Delete`, `Page Up`/`Down`, the arrows, the numpad and punctuation were all *silently refused* before (BUG-053, found from a user report). **Conflict state persists**: a refused binding reports beside that binding for as long as the refusal holds, rather than once in a status bar that clears in twelve seconds. **Dispatch by predicate** on a shared sequence, so `start` and `stop` on one key is a toggle rather than a start-and-immediately-stop. Sequences are normalised to Windows' own modifier order, so `Shift+Ctrl+F9` and `Ctrl+Shift+F9` cannot register as two conflicting bindings for the same key. `WM_HOTKEY` reaching the pump at all is row 20's async path |
| 22 | Save progress never completes, or the control closes before the file is written | **Green** | M9.6 | `FinalizeProgressTest.*` (cpu, 7) covers the event's contract: percent monotonic, only `done` reaching 100, `done` emitted **only after the validation gate passes**, and byte counts present in the remux phase and absent from the phases that have no proportional quantity. The end-to-end half is `test_the_event_loop_keeps_running_while_a_recording_is_finalized` (pytest, engine-backed) against a real engine and a real file: **MKV `['validating', 'done']`, MP4 `['flushing', 'remuxing', 'validating', 'swapping', 'done']`**, percent monotonic to 100, `done` last, and the file valid. The container asymmetry is the point — MKV has no remux, so an MKV reporting one would be a bar inventing work. The pill closes on `recording_finalized` with `valid: true` and on nothing else: `test_overlay_pill.py` asserts it stays up, in warn colour, on `valid: false` |

---

## Row 9's milestone, decided

SPEC.md §24 lists row 9 against no milestone. M6's exit criterion names rows 6, 7 and
10 and nothing else, while M6's *deliverable* is "CFR pacer + degradation ladder +
health monitor" — and row 9's mitigation is a watchdog, which reads like health-monitor
work. That ambiguity was flagged when M6 was scoped. **Resolved 2026-07-30: row 9 is
split, with detection in M6 and recovery in M7.**

The reasoning, so it is not re-litigated:

- **The detector belongs in M6 because the ladder needs it anyway.** SPEC.md §12 gives
  the `watchdog` thread "health metrics", and M6 builds that thread. Adding
  "when did capture last produce a frame" to a component already sampling queue
  occupancy and disk latency costs one field and gives the row its measurement.
- **The recovery does not, because it is M7's machinery.** Row 9's action is "forces a
  session rebuild" — tear down the capture session and start a new one against a
  possibly-changed target. That is the same procedure SPEC.md §5.4 needs for a GPU
  migration, which is M7's deliverable and row 11's test. Building a second, private
  copy of it in M6 to satisfy a row M6's exit criterion does not name is exactly the
  scaffolding CLAUDE.md §7 forbids: it would look finished and would then be replaced.
- **The split is honest about what is proven.** The detector is asserted against a
  synthetic clock, and a live recording reports `stall_episodes` and `worst_stall_ns`.
  What is *not* asserted is the 500 ms recovery budget, because nothing recovers yet.
  Row 9 therefore stays **Partial**, not Green, and the missing half is named.

The alternative — pulling the session-rebuild forward into M6 — was rejected on the
grounds above. If that turns out to be wrong, the cost is that row 9 waits for M7,
which is one milestone.

## Row 13's milestone, decided

**Resolved 2026-08-02: row 13 is in M8, alongside row 18.** Open question 1 below is
answered and kept only for the record of what was asked.

The reasoning, so it is not re-litigated:

- **The two sources were never in conflict about the same thing.** SPEC.md §24's M8 cell
  states an *exit criterion* — "test #18 green". The milestone column in this file states
  a *feature assignment*. SPEC.md §20 has no milestone column of its own (its columns are
  `# | Symptom | Primary root cause(s) | Mitigation | Test`), so what looked like two
  requirements disagreeing is this matrix being more complete than a one-line criterion.
- **Row 13's mitigation is a defining property of what M8 builds, not a feature layered
  on it.** SPEC.md §3.1 states the process-lifecycle contract — Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, bidirectional 1 s/5 s heartbeat, console-control
  handler and `WM_QUERYENDSESSION` window — as the rules of the GUI/engine pair, and M8's
  deliverable *is* that pair. A GUI that spawns an engine it cannot reliably reap has
  implemented half of §3.1, and M8's own exit criterion ("end-to-end recording driven
  entirely from the GUI") reads as met while it is not.
- **No later milestone can absorb it.** M9 is segmentation and preview, M9.5 is Tier B
  audio, M10 is installer/updater/docs/acceptance. Deferring row 13 lands it in M10, where
  §24's own M10 criterion — "clean install → record → uninstall leaves zero orphaned
  processes" — would find it, and the fix would mean reopening an IPC layer that three
  milestones of tests had been built against.
- **The costs are asymmetric.** Including it costs M8 a Job Object, a heartbeat pair and
  `test_orphan_prevention`. Excluding it ships an engine that can outlive its GUI while
  holding a capture session and an open file handle — the exact failure the pairing
  exists to prevent, and one that leaves a user with a locked, unfinalized recording.

**The consequence, and it needs the owner's pen:** SPEC.md §24's M8 exit criterion should
read **"tests #13 and #18 green"**. That is a change to a graded requirement, so it is
recorded here as a recommended amendment rather than made unilaterally. Everything else
about M8's scope is unchanged.

**M8 is now the largest remaining milestone by some margin** — IPC, the full themed GUI,
pause/resume, *and* process lifetime. That was true before this decision; it is worth
stating plainly rather than discovering during the milestone. If M8 has to be split, the
seam is between the transport (IPC + lifetime + a headless driver) and the GUI proper,
not between rows 13 and 18 — both rows sit on the transport.

**M8 was split there, 2026-08-03, and both halves are now done.**

**M8a — the transport.** `engine/core/ipc/` (framing, protocol, pipe server and client,
lifecycle, dispatch), §7.5's pause/resume, `tests/tools/gui_host` as the headless driver,
and **rows 13 and 18 both Green**.

**M8b — the GUI.** `gui/framecapture_gui/`: the Python mirror of the IPC layer, §3.1's
parent role in `ctypes` (named job with `KILL_ON_JOB_CLOSE`, suspended spawn, 1 s
heartbeat), §16.2's layout, §16.3's theme, §16.4's seven-section settings dialog, and
§16.5's global hotkeys. `scripts/lint.ps1` now runs **ruff and mypy `--strict`**, and
`scripts/bootstrap.ps1` creates the venv they need.

**M8's exit criterion is met**: `pytest gui/tests` records, pauses, resumes and finalizes
through `EngineController` — the same object the window uses, over the same pipe — and
asserts the file is valid. Measured: **37 tests, all passing**, of which 6 spawn a real
engine.

What M8 deliberately did **not** build, so it is not looked for:

- ~~**No preview.**~~ **Added 2026-08-04 in M9** — see "M9's preview half" below. As M8
  left it, §15.2's ring was absent and a GUI was told so by three independent routes:
  `start_preview`/`stop_preview` answered `INTERNAL_NOT_IMPLEMENTED`, `preview` was absent
  from `hello`'s capability list, and the surface said so on its face rather than showing a
  black rectangle. All three have flipped rather than been deleted, which is why the
  assertion that recorded the absence is now the assertion that records the presence.
- **No audio level meter.** §16.2's mock-up has one, and per-track levels are not in
  §15.1's `stats` payload. The bar is present, disabled, and labelled — a bar moving to
  invented numbers would break §16.5's "the status panel never lies" one panel over.
- ~~**No settings persistence.**~~ **Added 2026-08-03.** `get_config` / `save_config`
  extend §15.1's command list so the engine — which already implements §17's atomic
  write, ordered migration chain and unknown-key preservation — stays the file's only
  writer while the GUI stays the source of truth for the values. Settings survive a
  restart, verified across two engine processes. See open question 11.

## What SPEC.md §20 row 7 asks for that MKV cannot give

Row 7's test is specified as "assert `frame_count == duration_s * fps` exactly, **and
all PTS deltas identical**". The second half is container-dependent and the spec does
not say so.

`matroskaenc` forces a timebase of 1/1000 on every stream; the engine's 1/60000 request
is overwritten during `avformat_write_header`. On a millisecond grid a 60 fps frame
duration of 16.666… ms is not representable, so an MKV file's PTS deltas **cannot** be
identical — measured, they come out as `{16, 17}`. That is arithmetic, not a defect, and
no amount of correctness in the pacer changes it.

`CfrExactnessTest` therefore asserts the strongest form each container can carry, and
decides which from the file's declared timebase rather than from the container enum — so
a future FFmpeg that stops forcing 1/1000 tightens the test automatically instead of
silently keeping the weaker branch:

| Container | Timebase measured | PTS deltas measured | What is asserted |
|---|---|---|---|
| MP4 | 1/60000 | `{1000}` | Every delta identical, and equal to the exact ticks per frame |
| MKV | 1/1000 | `{16, 17}` | Every PTS equals the exact rounding of a uniform grid, so error stays under one tick and never accumulates; deltas are only ever floor or ceil of the true duration |

The frame-count half of row 7 is unconditional and is asserted exactly in both.

## The capture tests' input is now theirs

Recorded here because it changes how a GPU-tier result should be read, and because the
old reading is written into several rows above.

Until 2026-08-02, every case in `test_capture_sustained` and `test_capture_to_nv12` that
acquired a frame captured whatever happened to be on the desktop. `SyntheticSource` is
not available to them — duplicating a real `IDXGIOutput` is their subject — so
CLAUDE.md §5's rule was kept instead by having the test **own what the output shows**:
`tests/fixtures/screen_animator.h` covers the target monitor with a topmost window and
repaints it at a stated rate. Eight cases use it, not the five that had failed; the other
three had the same defect and had merely not been unlucky yet.

What that buys, and what it does not:

- **Content is owned.** `pattern_signature` reads the black and white strips at the two
  edges of a captured frame, and the cases assert on it per frame. Measured 240 of 240 on
  `SustainedCaptureToNv12StaysCorrect` and 20 of 20 on the fixture's own content case.
- **Delivery is owned.** `ScreenAnimatorTest.AnAnimatedPatternDeliversFramesWithoutHelpFromTheDesktop`
  measures delivered against *presented* rather than against a rate, so it holds on a
  quiet machine and a busy one alike: 91 presented (30.2 Hz), 113 delivered (37.5 fps).
- **Stillness is owned too.** `DdaEmitsDuplicateFramesWhenTheDesktopIsIdle` asks for a
  static pattern, so SPEC.md §4.3's idle desktop is established rather than hoped for:
  20 frames, 20 duplicates, 0 repaints.
- **Not owned: what DWM decides to composite.** Occlusion stops ambient *content* from
  reaching the capture; it does not stop ambient activity from driving composition —
  measured at 20.1 fps with the pattern held static. Ambient can therefore only ever add
  frames, never remove them, which is the direction that makes the assertions safe.
- **Not owned, and not the fixture's to own: whether `DuplicateOutput` opens at all.**
  Two of BUG-033's five original failures were DDA refusing to create a duplication,
  which happens before a frame is requested. Neither has recurred in four full tiers, and
  neither is explained by the fix. See BUG-033's note.

**A related gap left open deliberately.** `ThroughputTest.*` also reads the real desktop
and would give real numbers with the fixture in front of it — it currently prints "no
frames arrived, this measures nothing about throughput" when the desktop is quiet. It is
*not* part of BUG-033, because it asserts nothing rate-dependent and says so in its own
comment; it reports rather than judges. Giving it a controlled input changes what its
published ceiling means and obliges a re-measure on all three presets, so it is named
here rather than folded in.

## Known coverage gaps that are *not* a whole row

These are narrower than a row but real, and they are recorded here because
otherwise the table above reads greener than the tree is.

- **MP4's audio starts 21 ms late (BUG-021).** Closed the "rows 4 and 17 are MKV
  only" gap and it immediately found this: MP4 cannot declare the AAC encoder
  delay while the file is fragmented, so a reader presents the priming samples as
  content. Bounded to the head of the track — the steady state through MP4 measures
  **−187 µs**, identical to MKV — and pinned by a test that fails if it is fixed or
  if it grows. Row 4 is green on the steady state and this artefact is tracked
  separately.
- **`test_amf_zero_copy` proves our half, not FFmpeg's** (SPEC.md §2.2 item 1,
  CLAUDE.md §9). It asserts structurally that the engine hands libavcodec D3D11
  surfaces and never touches system memory — on every encoder, not just AMF. It
  does *not* prove `h264_amf` avoids an internal download, which is not visible
  through FFmpeg's public API. Measured for the record: **374 fps at 1080p on the
  780M, 366 fps on the RTX 4050**, equivalent to ~2.3 GB/s if a round-trip were
  happening. CLAUDE.md §9's decision stays open, now narrowed to that one
  question.
- **SPEC.md §8.3 names `IAudioClock2::GetDevicePosition`; the code reads the same quantity
  from `IAudioCaptureClient::GetBuffer`. Worth the owner's pen (BUG-041).** The two report
  the same thing — the endpoint's own frame counter — but `GetBuffer` supplies it on every
  packet, from an interface that is never optional, whereas `IAudioClock2` is a separate
  `GetService` that some endpoints refuse. The implementation has always used `GetBuffer`;
  `IAudioClock2` was acquired, warned about when missing, and never dereferenced, which is
  how a WARNING came to announce the loss of a feature that was still working. Either §8.3
  should name what is actually read, or the code should be re-pointed at the interface the
  spec names. Not decided here because §8.3 is a measurement contract.
- ~~**A genuinely idle desktop rebuilds the capture stack.**~~ **Fixed 2026-08-04
  (BUG-045).** The trigger keyed on how long capture had been silent — 50 ms originally,
  raised to 2 s — and the field measured **11.1 s of legitimate silence** on a static
  screen, which tore down and rebuilt the capture and device stack and cost `gap_ms=243`,
  repeatedly, for as long as nobody touched the machine.

  **There was no fifth threshold to reach for, because the quantity has no upper bound.**
  The two backends are not the same shape: DDA polls and emits a duplicate on every
  `WAIT_TIMEOUT` (§4.3), so it is never silent at all; WGC is push-only — `FrameArrived` is
  a compositor callback and the worker thread does nothing but stay alive to own the WinRT
  objects — so when the desktop does not change it says **nothing**, and a dead WGC session
  says exactly the same nothing. The trigger was reading a signal that does not exist.

  It now keys on `IScreenCapture::running()`, which both backends drop as their worker
  leaves its loop for any reason: a fault with a name, which an idle desktop cannot
  produce. Measured on a backend that goes quiet after a second of frames, differing only
  in what it then reports about itself — **quiet-but-healthy: 2 rebuilds before, 0 after,
  with the detector still reporting a 7952–7977 ms stall; quiet-because-dead: rebuilt in
  75.3–85.6 ms** against row 9's 500 ms budget, which is *faster* than the old threshold
  could manage. The three presets agree, because the cost is driver time rather than code
  time: worst gap **85.6 / 77.9 / 81.4 ms** on Release / RelWithDebInfo / Debug. Row 9's detector is untouched and still surfaces `stall_episodes` and
  `worst_stall_ns`.

  Two things about the verification are worth carrying:

  **The defect could not be reproduced on this rig, and the attempts are the measurement.**
  Recording the real desktop for 40 s gave **1944 frames**; covering the output with
  `ScreenAnimator` at `fps = 0` — paint once and hold — still gave **301 frames in 15 s**.
  A blinking caret and a clock are enough to keep WGC delivering. So the silence is
  *supplied*, by `QuietingCapture`, and `AStaticScreenIsNotMistakenForAWedgedCapture` is
  kept as a live canary that does not discriminate here and says so in its own comment.

  **Removing the trigger alone would have installed a worse defect.** With silence no
  longer acted on, an unplugged monitor would leave WGC alive and quiet forever — a case
  §14.2 requires. `WgcCapture` now subscribes to `GraphicsCaptureItem::Closed`, the only
  notice WGC gives that the captured thing has gone away, and drops `running` when it
  fires. `ABackendThatHasDiedIsRebuilt` is the control that makes that non-optional.

- **A WGC session that stops delivering while still reporting itself alive is not detected,
  and that is a deliberate gap (2026-08-04).** BUG-045's fix keys on a named fault, so a
  hypothetical failure with no name — the session silently ceasing to call `FrameArrived`
  with `running()` still true and the item not closed — now goes unnoticed where the 2 s
  threshold would eventually have caught it.

  **No such failure has been observed**, in the field or on this rig, and the previous
  attempt to cover it by guessing was the defect. Writing a detector for a fault that
  cannot be named or reproduced is how the 2 s threshold came to exist in the first place.
  Recorded here rather than half-built (CLAUDE.md §7). If it ever *is* observed, the signal
  to reach for is the frame pool's outstanding-buffer count — a starved pool is §20 row 9's
  own stated failure ("failing to call this starves the pool and stops capture") and is
  visible from the engine's own bookkeeping without asking the OS anything.

- **A wedged capture *thread* cannot rebuild itself, and nothing notices (2026-08-04).**
  The rebuild runs on the capture thread, so a thread blocked inside `acquire` can never
  execute the recovery that would fix it. Detecting it is easy — the loop turns at ≥10 Hz
  even when idle, so a tick that stops is unambiguous — but the response cannot be a
  rebuild; it would have to be §13 rung 7's "stop capture, finalize the file successfully,
  report the exact cause". That is a new path with its own failure modes, it is not the
  defect BUG-045 was about, and it needs the owner's view on whether a wedged capture
  should end a recording. Not built.
- **The pytest engine cases were reporting on the desktop, and BUG-033's fix never
  reached them (fixed 2026-08-03).** BUG-033 gave the GPU tier `ScreenAnimator` so its
  capture cases own what they capture. The pytest cases that drive a real recording over
  IPC were left out — and they are the ones with no alternative, because `start_record`
  captures a display and there is no source to inject at that level. Measured both ways
  on this rig: `test_stopping_while_paused_still_yields_a_valid_file` records for one
  second and asserts the file is valid; **with a window animating it passes, and on an
  idle desktop it finalizes `decoded_frames=1, duration_s=0.017` and is correctly
  rejected by the §10.4 gate** — reproducibly, three runs each way. The engine was right
  every time. Two causes, both now fixed: nothing owned the screen, and every case used a
  bare `time.sleep`, which blocks the very event loop a GUI would be running — so even a
  fixture would have been frozen for the whole recording. `gui/tests/screen_activity.py`
  and `_record_for` are the two halves. **This had nothing to do with pause/resume and
  everything to do with the same rule BUG-033 was about**; it surfaced here only because
  this session was the first to run `pytest` against a genuinely idle machine.
- **Every crash report so far has said `engine_phase: "uninitialized"`, and that is a
  defect rather than a reading.** Found while investigating BUG-037, whose report carried
  it while the engine was demonstrably mid-`start_record`. `crash_handler.h` says the
  field "stays `Uninitialized` until M6 wires it up"; M6 did not, and nothing in the tree
  calls `set_engine_phase` except `main.cpp`, with `Stopped` and `Faulted`. SPEC.md §18
  asks the report for the "last known state machine position", so the field is present,
  documented, asserted by `test_crash_artifacts` — and carries no information from a real
  crash. Left as found: it is worth doing and it was not BUG-037. The cost is exactly
  what BUG-037 paid, which is that the one field meant to say *where* the engine was said
  nothing.
- **MKV's half of row 3 still has the thin margin MP4's used to (BUG-034, and it needs
  the owner).** Matroska's recoverability unit is the cluster, and SPEC.md §10.2
  specifies `cluster_time_limit=2000` in as many words — so a killed MKV still loses up to
  2 s against a bar of `30 − 2.0 − 0.1`, which is the same 121 ms of room that made MP4's
  outcome a coin toss between two fragment boundaries. Nothing has failed there, and the
  same treatment would work: a shorter cluster limit, or asserting a bound that admits
  the quantisation. Not changed here, because the number is written into the spec.
  `kMaxTailLossSeconds` (2.0) is MKV's; `kMaxMp4TailLossSeconds` (1.0) is MP4's.
- **`CrashRecoveryTest.TheNextLaunchFindsTheSidecarAndFinishesTheJob` was observed
  failing once in a full GPU tier run, on 2026-07-30.** It passed in isolation
  immediately afterwards (34.1 s) and passed on a full re-run of the same tier
  (150/150), so it is recorded as an intermittent rather than a regression — but it is
  recorded, because row 3 depends on it and a test that fails one run in two is not
  something to notice twice and forget.

  No diagnosis yet. The case kills a real recorder process at a wall-clock deadline and
  then waits on the repair path, so it has real-time elements that a busier machine
  could plausibly perturb; the tier had just gained six new GPU cases including the
  cross-adapter probe, which creates devices on both adapters. If it recurs, the first
  thing to capture is the assertion text — this run's output was filtered before it was
  saved, which is why there is no message here.
- ~~**`CfrExactnessTest.DroppedFramesCostContentButNeverSlots` can go vacuous on a slow
  build.**~~ **Fixed 2026-08-02**, with the configuration BUG-027 arrived at: a feed
  paced at **60 fps in wall clock** against a **300 ms-per-four-writes** stall, a drain
  of ~13 packets/s. Both rates are configured, so the deficit is a property of the test
  rather than of the build — where the old flooded form drained at 80 packets/s against
  "however fast this machine's feeder is" and could drop nothing on Debug.

  The vacuity escape hatch is gone, replaced by an assertion on the **union** of the two
  ways the pipeline sheds work. Feeding at a real-time rate needed a third feed mode
  (`Feed::RealTime`) — back-pressure pacing would simply have blocked behind the stalled
  mux and dropped nothing.

  **And it needed `prerendered_frames = 8`, which is BUG-027 arriving a third time.**
  Setting the rate is not the same as being able to hold it: rendering a fresh 1080p
  BGRA pattern per `acquire` costs more than 16.67 ms on Debug, so the absolute-deadline
  sleep returns immediately and the feed silently runs at whatever the renderer manages.
  Caught in a full Debug tier — **12 fps against a 13 packets/s drain**, so the feeder
  was slower than the disk it was supposed to outrun, 300 of 300 frames encoded, rung 0,
  nothing dropped. Safe to memoise here because nothing in the file asserts frame
  identity. Measured after, and the point is how alike the three now are:

  | Preset | Queue-dropped | Paced out | Rung |
  |---|---|---|---|
  | Release | 185 | 11 | 3 |
  | RelWithDebInfo | 185 | 10 | 3 |
  | Debug | 185 | 8 | 3 |
- **Row 6's original ten-minute run fed at 57.2 fps, not 60.0 — since fixed.** 36,000
  frames had taken 629.3 s of wall clock instead of 600.0, because
  `SyntheticSource::acquire` re-rendered a 1080p BGRA pattern per call and could not
  hold the 16.67 ms budget under 99% GPU load. The *file* was always exactly 60 fps and
  600.0 s (the source stamps frames on its own synthetic grid), and the pipeline
  consumed every frame it was given — but the headline feed rate understated the load
  the row asks for.

  `SyntheticSource::Settings::prerendered_frames` (BUG-027) fixed it: the feeder now
  measures **60.02 fps** on Debug and Release alike. The ten-minute figure quoted in the
  row above predates the fix and is conservative; re-running it can only improve it.
- **SPEC.md §13 rungs 1 and 2 have no implementation on hardware, and cannot have
  one through libavcodec.** The ladder computes both decisions, logs them and is
  tested on them, but neither `h264_nvenc` nor `h264_amf` can change preset or QP
  mid-recording — measured against the FFmpeg n8.1.2 sources this build links, not
  inferred. `libx264` honours a runtime QP change and rung 2 is therefore real on the
  software path only. Rung 1 has no implementation anywhere. The practical consequence
  is that degradation on hardware steps straight from nominal to rung 3's halved
  capture rate with no gentler intermediate, which is a quality regression against the
  spec's intent and is worth the owner's attention. See BUG-024, and
  `SoftwareEncoderTest.TheSoftwareEncoderAcceptsARuntimeQualityChangeAndTheHardwareOnesDoNot`,
  which is written to fail if a future FFmpeg gains the capability.
- **Rung 6's free-space thresholds are covered arithmetically, not on hardware.**
  `HealthMonitor.LowFreeSpaceWarnsAndCriticallyLowFreeSpaceFinalizes` (cpu) pins the
  2 GB and 500 MB behaviour, including that a failed free-space query does not read as
  an empty volume. Reducing the reference rig's free space to 500 MB is not something
  a test may do to the machine it runs on, so the *latency* half of rung 6 is what
  `SlowDiskTest` exercises live.
- **Row 11 contains a spec conflict that needs the owner's decision. Three measurements
  establish it,** all in `MigrationParameterSetTest` (gpu):

  1. **Identical settings do not produce identical parameter sets.** `h264_amf` emits
     28 bytes of SPS/PPS, `h264_nvenc` 53, with every caller-controlled setting equal.
     So §5.4's preferred path — "prefer forcing identical encoder settings" — is not
     available across vendors.
  2. **In-band parameter sets do not rescue the stream.** Encoding half a file on each
     adapter through one muxer, with the second encoder emitting SPS/PPS in band,
     decodes **31 of 60 frames** — the whole first half and one frame of the second.
     The decoder initialises from the container's `CodecPrivate` and a mid-stream SPS
     with the same id but different content does not reliably re-initialise it. §5.4's
     aside that "AVCC → Annex-B-in-`avc1` is invalid" covers more ground than the
     framing question it appears to be about.
  3. **Same-adapter reopens are byte-identical.** A driver restart, a device reset or
     row 9's stall recovery rebuilds on the same adapter and reproduces its parameter
     sets exactly.

  **The conflict:** SPEC.md §20 row 11 requires "a single continuous playable file",
  and §5.4's own fallback says that when extradata changes the muxer should "close the
  segment and start a new one, and log it loudly". Measurement 2 says the fallback is
  the *only* correct outcome for a cross-adapter migration, so the two requirements
  cannot both hold. This is not an implementation choice; it needs §20 row 11 or §5.4
  amended. See "Open questions for the owner" below.

  A fourth finding, smaller but real and **not mentioned in §5.4 at all**: the new
  encoder's DTS must not precede the old one's. §9 sets `max_b_frames = 2`, so DTS lags
  PTS by the reorder depth and a fresh encoder re-derives DTS from its own first PTS.
  Handing over with no PTS gap makes libavformat reject the first packet outright
  (`non monotonically increasing dts ... 483 >= 467`, measured). §5.4's 350 ms budget is
  ~21 frames at 60 fps against a reorder depth of 3, so a real migration clears it
  comfortably — but the constraint is invisible until a migration is fast or the
  B-pyramid is deeper, and it belongs in the procedure rather than in luck.
- **Row 12: the audio timeline will be chained, not rebased, and this rig cannot test
  the trigger.** Two findings:

  `AudioTimeline` holds its endpoint's sample rate as fixed state and counts
  `frames_written_` in that rate's frames, and `accept()` advances the head by the
  *packet's own frame count*. So converting the timeline to a rate-independent unit
  would round once per buffer and accumulate — exactly what the class's header warns
  against — and rebasing `frames_written_` at the migration instant would silently
  change what the accumulated count means. The design is therefore **one timeline per
  endpoint, chained by a boundary offset**: each stays in its own rate, no unit is ever
  mixed, and the only conversion is one per migration. It also matches what the class
  was already built for ("one instance per track... which is why this holds no global
  state") and is the shape Tier B needs in M9.5. Total length will be accumulated in
  *seconds*, for the reason BUG-025 established for the video pacer.

  **Measured on this rig: one render endpoint — 48 kHz, 2 ch, "Speakers (Realtek(R)
  Audio)".** There is nowhere *different* to migrate to, so §14.1's trigger
  (`OnDefaultDeviceChanged` → a genuinely different endpoint) is not verifiable here.

  **What was done about it, rather than settling for CPU-only coverage.** The two halves
  are testable separately and are tested separately. The *format* change is CPU-tier at
  the `AudioEncodePath` seam, driven synthetically at 48 kHz stereo → 44.1 kHz mono. The
  *handover* is GPU-tier and real: `AudioDeviceMigrationTest` migrates a live recording
  to the current default, which runs every line of `AudioPath::migrate_to` — the live
  `LoopbackCapture` stop, the COM apartment and MMCSS re-entry, the re-established sink,
  the format the reopened endpoint reports, and the seam taken from the real clock after
  negotiation. **Measured at 15.2 ms against a 200 ms target**, which is the number
  §14.1 asks for; reopening a *different* endpoint is not systematically dearer than
  reopening this one. What remains uncovered is the two running together on one machine
  — a real device change whose replacement has a different format — and that needs a
  second render endpoint. Called out rather than papered over (CLAUDE.md §6).

  **What would close it**, so the gap has a route out rather than only a caveat: a
  second `eRender` endpoint on the machine at a different rate or channel count — a USB
  or HDMI audio device, a Bluetooth headset, or a virtual audio driver. With one
  present, `AudioDeviceMigrationTest.ALiveEndpointMigration...` already takes a
  `device_id`; pointing it at the non-default endpoint and re-asserting the same three
  invariants is the whole change. **Installing a virtual audio driver on the reference
  rig is a machine-level change and belongs to the owner, not to a test run.**
- **Rung 5 is implemented and verified, and it re-learned BUG-001 the hard way.**
  `SoftwareEncoderTest.EveryFrameSurvivesTheReadbackIntoTheSoftwareEncoder` (gpu)
  decodes the output and reads the frame-index barcode out of **every** frame, because a
  mis-strided or misaligned NV12 readback produces a perfectly playable file that is
  green with the picture shifted — nothing short of checking the content catches it.
  Measured: 120 frames at 119.7 fps through `libx264 superfast` at 1080p with a full GPU
  readback per frame, 0 unreadable barcodes, 0 wrong indices, rung 5 reported and the
  capture rate untouched. The readback pool's bind flags are **probed**, not supplied:
  `SHADER_RESOURCE` is refused with E_INVALIDARG on this rig and the probe resolves to
  `0x280` (`DECODER | UNORDERED_ACCESS`), which is BUG-001's lesson arriving by a second
  route on a path that has no encoder pool to have probed for.
- **§5.2 rule 2 is reachable at last, and on this rig it correctly refuses to fire.**
  Rule 2 requires the cross-adapter transfer cost be "**measured** < 2.0 ms/frame", and
  nothing had ever supplied a measurement — `measured_cross_adapter_ms` was always
  `nullopt`, so every selection fell through rule 2 to rule 3 or 4.
  `gpu::measure_cross_adapter_cost` (§5.3) supplies it, `GpuTopologyService` caches it,
  and `select_for_monitor` consults it.

  Two findings, both measured on the reference rig:

  1. **§5.3's preferred zero-copy path does not exist between these adapters.**
     `OpenSharedResource1` fails in *both* directions between the Radeon 780M and the
     RTX 4050, so §5.3's stated fallback — "staged CPU copy (`D3D11_USAGE_STAGING` +
     `Map`)" — is what the engine would actually use.
  2. **That fallback costs 3–4× the budget.** P99 **6.0–7.5 ms** per 1080p frame
     against a 2.0 ms budget, median ~1.0 ms. Rule 2 correctly does not fire, which is
     §5.2's own thesis confirmed rather than assumed: "shipping them to the 4050 costs
     a full 1080p BGRA round trip … for zero quality gain".

  The median-versus-P99 gap is the reason the probe reports a percentile. A mean of
  ~1.2 ms would have cleared the budget and let rule 2 fire — on a path that stalls for
  7 ms once in every hundred frames, which drops frames at 60 fps. A pair that cannot
  transfer at all is cached as *absent* rather than as a large number, so the rule
  cannot reach a conclusion it has no basis for.
- **Rung 4 is reported but not acted on.** Three device failures in 60 s set
  `PipelineHealth::gpu_migration_requested`, asserted in
  `HealthMonitor.ThreeDeviceFailuresInsideSixtySecondsRequestAGpuMigration` (cpu). The
  §5.4 migration that answers it is M7 and row 11.
- **SPEC.md §20.1's four-hour soak is not runnable yet.** `test_av_sync` takes a
  duration, but its decoder holds every sample in memory — 5.5 GB at four hours
  (BUG-018). The soak form needs a streaming onset detector. M10.

---

## Row 18's design, and why it is riskier than it looks

Recorded here because the row was added to the matrix before any code exists, and the
hazard is not in the feature but in what it does to a component three milestones of
tests already depend on.

Pause **excises** time — one file, and the paused span is simply absent from it. That is
what separates pause from stop/start, and it is what makes it dangerous: SPEC.md §7.1
makes every PTS a function of `qpc - t0`, and excising time means that mapping acquires
a second term:

```
timeline_ns = qpc_ns - t0_ns - paused_total_ns
```

`paused_total_ns` has to be **shared by video and audio exactly as `t0` is**. Two streams
subtracting different totals desync permanently and silently — the same failure §7.1's
shared epoch prevents, arriving by a new route, and it would not be caught by any
existing test because every one of them records without pausing.

Two things already in the tree make this cheaper than it would have been:

- `timing::SessionEpoch` is already the shared-epoch negotiator, so it is the natural
  owner of the paused total. Neither `Pacer` nor `AudioTimeline` should maintain one.
- `Pacer::timeline_seconds()` (BUG-025) already derives the file's length from emitted
  PTS rather than from a frame count over a rate, so it needs no special case for
  paused time. The §10.4 validation gate inherits that for free. That fix was made for
  rung 3's retime and pays off a second time here.

The failure mode to design against is the one BUG-025 was: a control-flow change that
silently invalidates an arithmetic assumption elsewhere. Row 18's test asserts the A/V
offset *after every resume* rather than only at the end, because a single shared-clock
mistake shows as a growing offset that a whole-file check averages away.

### How it was built, and the three things that were not obvious

**The paused total went into a shared object, not into `SessionEpoch`.** The note above
predicted `timing::SessionEpoch` would own it. It does not: `SessionEpoch` is a
single-threaded value that `VideoPipeline` *assigns* per recording, while the paused total
is read from the `venc` and `aenc` threads on every frame and every buffer. Making
`SessionEpoch` atomic would have made it non-assignable and changed a component four
milestones of tests depend on. `timing::PauseClock` is the split: one object, borrowed by
`Pacer` and by `AudioTimeline` as a `const*`, and it is the only place §7.5's subtraction
is written. Attaching it is null-by-default, so every pacer and timeline test written
before M8 still asserts exactly what it says it asserts.

**Order of operations, not just arithmetic.** Subtracting the paused total *after*
quantizing to the CFR grid would manufacture one duplicate per paused frame interval —
the frozen-frame outcome §7.5 rules out, produced by getting the order wrong rather than
the formula. Excising first removes the gap before the index is computed, so §7.5's "the
pause must **not** be filled with duplicates" needs no code of its own. Measured: **0
duplicates emitted** across three pauses.

**A forced I-frame is not a forced IDR, and every encoder in this build gets that wrong
by default.** §7.5 requires an IDR on resume, because a P-frame referencing pre-pause
content is a visible smear at the seam. Setting `pict_type = AV_PICTURE_TYPE_I` is *not*
enough — measured against the FFmpeg n8.1.2 sources this build links: `nvenc.c` picks
`NV_ENC_PIC_FLAG_FORCEINTRA` over `..._FORCEIDR`, `amfenc.c` picks
`AMF_VIDEO_ENCODER_PICTURE_TYPE_I` over `..._IDR`, and `libx264.c` picks
`X264_TYPE_KEYFRAME` over `X264_TYPE_IDR`, in each case unless `forced-idr` was set at
open. A plain I-frame does not empty the reference buffer. The option is now set on all
three paths — note the spelling differs, `forced-idr` on NVENC and libx264 and
`forced_idr` on AMF, and libavcodec leaves an unrecognised key in the dictionary rather
than complaining, which is why `H264Encoder::open` logs whatever is left over. **This
would have shipped as a defect that produced a perfectly playable file**, which is the
class of thing nothing short of looking catches.

### The quiesce, and why `pause_stragglers` exists

`paused_total_ns` grows at the *resume* instant, but items are mapped when the `venc` or
`aenc` thread reaches them. An item captured before a pause and mapped after the resume
would have the new total subtracted from it and land in the wrong slot.

The pipeline prevents this by quiescing: capture stops submitting *first*, then
`VideoPipeline::pause` drains the encode queue with a bounded 250 ms deadline before the
pause is published. That makes stragglers impossible rather than unlikely — but
"impossible" is a claim about code in another file, so `PauseClock` **counts** them
instead of trusting it, corrects one level of staleness exactly, and row 18 asserts the
count is zero. Measured: **0**. A counter that never fires proves nothing, so
`PauseClock.AnItemCapturedBeforeAPauseButMappedAfterItIsCorrectedAndCounted` manufactures
one deliberately and has a negative control beside it.

### What each of row 18's two clocks proves

**Amended 2026-08-03, after the first version of this section turned out to be the
problem.** What it said below was true. What it left out cost a fortnight of Green on a
path that was corrupting audio the whole time — see BUG-038. The correction is not that
the old numbers were wrong; it is that the row's *wording* claimed more than its one test
measured, and nothing said so.

**The synthetic case (`PauseResumeTest`) proves the shared-clock arithmetic.** Both
signals come from one supplied clock and both are anchored to the *timeline* rather than
to wall clock: the source flashes once per 60 emitted frames, the tone advances only over
emitted audio. That is what makes beep *k* and flash *k* the same instant in the file
whatever the pauses did, and it is what lets the result be byte-identical on three
presets. It proves that `Pacer` and `AudioTimeline` subtract the same paused total, that
the file holds exactly the unpaused frames, and that no duplicate is manufactured at a
seam.

**What it cannot prove, structurally.** `AudioSource::External` has **no wall-clock drift
loop at all.** SPEC.md §8.4's ladder measures the encoded track against elapsed time; when
the test supplies both quantities from one clock, there is nothing for a pause to
desynchronise between them and the ladder cannot be wrong. Its −0.188 ms was a correct
measurement of a quantity that was insensitive to the defect. Meanwhile the live WASAPI
path was reading the pause itself as drift and injecting 97,020 correction frames into a
sixteen-second recording. **"Row 18 is Green" was true and misleading at the same time.**

**The loopback case (`LoopbackPauseTest`) proves the measurement against a clock nobody
supplied.** `AudioSource::SystemLoopback`, real time, real endpoint, `pause()` and
`resume()` reading QPC themselves rather than being handed an instant. It is the only
pause case in which §8.4's ladder actually runs, and it states its bound against the
*pause duration* rather than against the 40 ms band — a reference still counting paused
time reads short by exactly the pause, and a band-only assertion would fail without saying
which two seconds it was. Measured: **+9 µs, 0 resyncs** (pre-fix: +2,007,079 µs and 5
hard resyncs).

**Still not covered, stated rather than implied.** A pause driven by a real *hotkey*: the
transport is exercised end to end — `fc_gui_host` and the pytest `engine` suite send real
`pause_record` / `resume_record` over the real pipe — but not with row 18's A/V assertions
attached to it. `pause_at`/`resume_at` remain the seam for the synthetic case, and nothing
in the engine calls them.

**The rule this row is now the standing example of:** when a test injects a seam to become
deterministic, name what the injection removed from the measurement. `AudioSource::External`
removed the drift loop. `PipelineSettings::capture_factory` removed `create_capture`, which
is where BUG-037 crashed. Both were reasonable seams and both hid a live defect behind a
green result.

## M9's segmentation half, and what its exit criterion is worth

**§20 has no row for segmentation or for preview.** M9's exit criterion comes from §24's
table — "keyframe-aligned splits with no frame loss" — so unlike every milestone before it
there is no named test handed down and no pre-agreed set of assertions. The tests below
were derived from §11's own sentences, one clause at a time, and that provenance is worth
recording because nothing else pins them.

**Green, `SegmentationTest.*` (gpu, 3) and `SegmentPlanner.*` / `SegmentPath.*` (cpu, 12).**
Measured: 4500 frames at 720p30 with a one-minute trigger produce **3 segments, every one
opening on a keyframe, 4500 barcodes contiguous, 0 gaps, 0 repeats**.

### The first version of the exit-criterion test could not fail

Worth stating plainly, because it is this session's recurring shape and it very nearly
shipped as a green result that proved nothing.

SPEC.md §9 fixes the keyframe interval at two seconds and §11's duration trigger is a
whole number of minutes. On a perfectly paced stream the trigger therefore lands *exactly*
on a keyframe every time, and the wait for an IDR — the clause the exit criterion is
actually about — is never entered. **Measured: with the keyframe wait deleted outright, the
test still passed**: three segments, all keyframe-aligned, 4500 contiguous barcodes.

The size trigger looked like the fix and was not: the synthetic pattern encodes to about
212 bytes a frame, so 4500 frames is 954 KB and no whole number of megabytes splits it at
all.

What works is a keyframe interval that does not divide the segment. `gop_seconds` is
configurable, and at 7 s a 60 s boundary falls 4 s into a GOP, so the split *must* wait.
`ASplitWhoseTriggerFallsMidGopWaitsForTheKeyframe` runs that, and it discriminates:
with the wait removed it reports `midgop_part002.mkv opens on a packet that is not a
keyframe` and `frames jump from 1799 to 1890` — **91 frames lost at one boundary**, which
is §11's "unplayable garbage" measured rather than quoted.

Both cases are kept. The default-GOP one proves no frame loss on the configuration users
will actually run; the mid-GOP one is the only one that proves alignment.

## M9's preview half, and what it proves

**§20 has no row for the preview either**, and M9's exit criterion names only the splits —
so the assertions below come from §15.2's own four sentences, one clause at a time, exactly
as the segmentation half's came from §11's. Recorded because nothing else pins them.

**Green, `PreviewTest.*` (gpu, 5) and `PreviewRing.*` (cpu, 11), plus `test_preview_reader.py`
(6) and three widget cases in `test_main_window.py`.**

### What each clause of §15.2 turned into

| §15.2 says | asserted by | measured |
|---|---|---|
| "Named shared memory (`CreateFileMapping`) triple-buffered ring … atomic write-index header" | `PreviewRing.*` (cpu) | slot rotation, wrap, publish ordering, lap detection, and a section that does not exist refused rather than created |
| "downscaled preview (default 960×540, 30 fps, BGRA)" | `ARecordingPublishesDownscaledFramesTheGuiCanRead` | **300 captured → 150 published**, i.e. exactly 30 fps off a 60 fps capture; 960×540 read from the header |
| "**never** full-resolution frames" | same, and `TheDefaultGeometryIsTheOneSpecifiedInFifteenTwo` | the whole three-slot ring (6.2 MB) is smaller than one 1080p BGRA frame (8.3 MB) |
| "independently droppable … zero effect on the recording" | `ARecordingIsUnaffectedByAPreviewThatCannotKeepUp` | see the table below |
| "a separate GPU shader dispatch off the same source texture" | `TheDispatchStaysInsideTheBudgetFifteenTwoStates` | dispatched from the capture thread off the live `CaptureFrame::texture`, before it is released |
| "adding < 0.2 ms/frame" | same | **0.153 ms median on the Radeon 780M, 0.049 ms on the RTX 4050** |
| §16.1's "the GUI wraps a `QImage` over the shared-memory buffer and nothing else" | `test_the_gui_attaches_to_the_preview_and_receives_frames` | `Format_RGB32`, `bytesPerLine == stride`, no copy, no numpy anywhere in `gui/` |

### The dispatch cost, and the measurement that was wrong first

| adapter | min | median | p99 | max |
|---|---|---|---|---|
| Radeon 780M (integrated) | 0.139 ms | **0.153 ms** | 0.779 ms | 0.831 ms |
| RTX 4050 (discrete) | 0.048 ms | **0.049 ms** | 0.052 ms | 0.052 ms |

Back to back inside one disjoint block, 200 samples, after 60 warm-up dispatches.

**The first version of this measurement reported a failure that was not there**, and it is
worth stating because the number would otherwise look like it moved for a reason. It
synchronised the GPU after every dispatch — `Begin(disjoint)`, dispatch, `End`, spin on
`GetData` — which drains the pipeline and lets the part drop a power state between samples.
It reported **median 0.237 ms, p99 0.424 ms** and a floor of 0.179 ms.

The obvious suspect was the shader: fxc flattens `cond ? tone_map(x) : x` into evaluating
both sides and selecting, so the SDR path was paying two `exp`, three `log`, three `exp`
and four `div` per tap for a value it discarded. That was **real, and not the cause** —
measured back to back, flattened is 0.171 ms against branched 0.165 ms, about 3%. The
dispatch is memory-bandwidth-bound on an iGPU that reads its source texture across the same
bus as the CPU: 8.3 MB in and 2 MB out per frame at 0.153 ms is ~67 GB/s, which is what a
780M does. The `[branch]` was kept because it is what the code should have said, not because
it fixed anything.

**The 780M's tail is over budget and its median is not**, and the gap between them is the
honest shape of this number on a machine that is also running a desktop. Across runs the
median sat between 0.153 and 0.168 ms while the p99 ranged from 0.210 ms to 0.779 ms — the
long samples are the compositor taking the part, not the shader taking longer. The
assertion is on the median, stated as such in the test. The discrete adapter, which is not
driving a display here, shows no tail at all: 0.048 ms to 0.052 ms across every sample.

### "Zero effect on the recording", measured against a preview that actually failed

Three recordings of the same 300 synthetic frames at 720p30. The third runs `fc-preview`
with a 120 ms injected stall per frame and **no reader attached at all**, so the single
hand-off slot saturates within a few frames and stays saturated.

| | preview off | preview on | preview wedged |
|---|---|---|---|
| frames captured | 300 | 300 | 300 |
| frames encoded | 300 | 300 | 300 |
| queue drops | 0 | 0 | 0 |
| paced out / duplicates | 0 / 0 | 0 / 0 | 0 / 0 |
| frames absent from the file | 0 | 0 | 0 |
| capture stack rebuilds | 0 | 0 | 0 |
| capture wall clock | 9974 ms | 9976 ms | 9976 ms |
| preview frames published | 0 | 299 | **79** |
| preview frames dropped for want of a slot | 0 | 0 | **219** |
| `offer` cost on the capture thread, worst / mean | — | 74 / 29 µs | 41 / 25 µs |

Every recording-side row is identical across the three, to the frame and to two
milliseconds of wall clock. Debug agrees: 9988 / 9995 / 9996 ms, same zeros.

**The control is the point.** `dropped_no_slot > 0` and `published` collapsing from 299 to
79 are asserted *before* the recording is examined, because a version of this case where
the preview kept up would be comparing three healthy runs and reporting success — the same
shape as BUG-038's supplied clock with no drift loop. The wedged preview genuinely fell
over; the recording did not notice.

The capture thread pays 25–35 µs per offered frame across runs and presets, worst case
under 220 µs — the dispatch and one `CopyResource`, recorded and not waited on. Everything
proportional to the picture (the `Map`, the 2 MB `memcpy`, the publish) is on `fc-preview`
at `BELOW_NORMAL`.

**These three runs are 720p30 rather than 1080p60, and the fixture decided that, not the
product.** At 1080p60 with `prerendered_frames = 0` — which unique barcodes require, and
they are what makes BUG-044 visible — the *Debug* build's synthetic source cannot feed the
rate it claims: measured, 300 frames took 13.5 s instead of 5 s, the pacer duplicate-filled
400 slots, and the stall detector rebuilt the capture stack **three times with the preview
switched off**. Comparing three runs for exact equality is only meaningful when the
recording is comfortable in all three, so the render cost came down instead. It is the
third appearance of the warning in `synthetic_source.h`, after BUG-027 and BUG-032, and the
first two are cited there by name.

The decimation case keeps 1080p60, because 60 fps of capture becoming 30 fps of preview is
the thing it exists to show: **300 captured → 150 published, 150 rate-limited**, on every
preset.

### What this cost: BUG-044, and what it says about the numbers above

**The measurement table above was clean before the bug was found, and that is the finding.**
With the preview running, 11 and then 84 frames per recording were written *wrong* — the
right number of frames, in the right order, with the wrong pixels — because
`PreviewScaler::dispatch` and `Nv12Converter::convert` interleaved their compute-stage
bindings on one immediate context. Every counter in the table said the recording was
perfect. Only reading the frame-index barcode back out of the decoded picture saw it.

Fixed by `gpu::ScopedDeviceLock` in both dispatch sites; the full account is BUG-044.

### Divergences from the spec, stated rather than absorbed

Three of the four need the owner's pen and are carried into "Open questions" below as
items 12, 13 and 14; they are listed here too so the divergences are readable in one place.

1. **"one `HANDLE`-passed section" is built as a *named* section opened by name** —
   question 12. §15.2's own first clause says "Named shared memory (`CreateFileMapping`)",
   and the two mechanisms are alternatives. Reasoning in `preview_ring.h`.
2. **SPEC.md §12's thread table gains a row** — question 13. `fc-preview`, `BELOW_NORMAL`,
   blocking allowed. The readback's `memcpy` must not be on the capture thread (CLAUDE.md
   hard rule 4); the dispatch must be, because that is where the source texture is live.
3. **The section carries the default DACL, not §15.1's SID-restricted ACL** — also
   question 12. §15.1 states an ACL requirement for the *control* channel and §15.2 states
   none for this one. `Local\` plus the session GUID gives per-logon-session isolation.
   Recorded rather than invented.
4. **Preview geometry is constants, not configuration.** §16.4 enumerates the settings
   dialog's sections and none is a preview, so a config key would be one no UI could reach
   (CLAUDE.md §7). This one needs no decision — it is what §7 already says.

### What the preview does *not* cover

- **`start_preview` takes no target.** §15.1 gives the command no parameters, so a
  preview-only session previews the primary display. Previewing a chosen monitor before
  recording it would be a §15.1 change — question 14.
- **The preview-only session costs a capture teardown at `start_record`.** There is one
  capture backend (DDA permits one `IDXGIOutputDuplication` per output per process), so the
  preview session is stopped before the recording's starts. That adds to the click latency
  already recorded as an open item below; it is not separately measured.
- **HDR.** The preview shares `tone_map.hlsli` with the encoder specifically so an scRGB
  desktop previews as the picture being recorded, and the branch is exercised by neither
  tier — turning system HDR on is not something a test can do. Same limitation §4.2's
  branch has always had.
- **A second machine.** Everything above is the reference rig. Nothing here depends on a
  GPU pair this machine does not have.

## Finalization cost, measured (BUG-046)

SPEC.md §10.3 states a performance budget in a parenthesis -- "stream copy only, no
re-encode, ~2 s for a 1 h file" -- and until 2026-08-05 nothing measured it. Every MP4 case
in the tree records for a few seconds, where a cost proportional to file size is invisible.
Reported from real use: a 10-15 minute recording of about 1.2 GB takes a significant time
to save.

### Where the time goes

Finalization is now logged per stage (§10.4 already required that; the timings are new).
On a 15 MB recording:

```
size_mb=14.897  remux_ms=105.850  validate_ms=648.870  swap_ms=4.551
```

Two costs with different shapes: **`validate` is a fixed ~650-1000 ms** -- it decodes the
5 s tail, which §10.4 requires and BUG-040 already trimmed to that -- while the **remux
scales with the file**. Solving the remux's two terms from a 15 MB and a 290 MB measurement:

| term | measured |
|---|---|
| per byte | **1.97 ms/MB (509 MB/s)** for one read plus one write |
| per packet | **8.8 µs** |

For the reported recording -- 1.2 GB, ~96,000 packets -- that is 2.4 s of bytes plus 0.8 s
of packets = **3.3 s of remux**, plus validate's fixed second. A user's own file can be
measured with `FinalizeThroughputTest.MeasureAGivenFile` and `FC_REMUX_INPUT`.

### What was fixed, and how much it was worth

`faststart` produces the moov-at-the-front layout by **writing the file and then rewriting
it** -- its own option text says "Run a second pass", and `ff_format_shift_data` re-opens the
output for reading and copies the whole mdat forward. With the remux's own read and write
that is four passes where two are required. Reserving the moov instead (movenc's
`moov_size`) gives the identical layout in one pass.

**Measured on the same 290 MB file through both paths, twice each:**

| | run 1 | run 2 |
|---|---|---|
| moov reserved | 0.76 s / **382 MB/s** | 0.72 s / **400 MB/s** |
| faststart | 0.78 s / 369 MB/s | 0.84 s / 344 MB/s |

**About 10%.** Not the doubling the pass count suggests, and the reason matters: a 290 MB
file written seconds earlier is entirely in the OS page cache, so the redundant read is
memory and the redundant write is overwritten before anything is flushed. The saving is real
disk work only for a file large or old enough to have left the cache -- which a 1.2 GB
recording written over fifteen minutes largely is, and which **cannot be demonstrated on
this rig**: no file that fits in RAM can show it.

An earlier version of this note claimed 145 -> 319 MB/s, a 2.2x win. That was two *different*
recordings at different sizes, and the difference was size and cache rather than the change.
It is recorded in BUG-046 rather than quietly corrected, because the arithmetic was right and
the comparison was not.

### What is not done

- **The per-byte term is the dominant cost and is untouched.** 509 MB/s for a read plus a
  write is roughly 1 GB/s of combined cached I/O; whether a larger AVIO buffer than
  libavformat's default 32 KB would improve it is unmeasured. It would need a custom
  `avio_alloc_context`, so it is a real change rather than an option, and it is not made on
  a hunch.
- **`validate`'s fixed ~1 s** is a decode of the 5 s tail that §10.4 requires by name. It
  dominates on short recordings and is invisible on long ones. Reducing the probe window is
  a change to a stated requirement.
- **Nothing is asynchronous.** `stop_record` still blocks for the whole finalize, which is
  §15.1's contract (30 s budget) and the GUI shows "Finalizing…". Making it return early
  would change that contract -- see the open item about `start_record`'s 956 ms click
  latency, which is the same question from the other end.

## M9.5's Tier B, and what its exit criterion is worth

SPEC.md §24's M9.5 exit criterion is **"Tests #14, #15, #16 green; Tier A provably
unaffected when Tier B is off."** The three rows are in the table above. This section is
about the second clause, which a green suite does not give you for free, and about what
Tier B does *not* cover.

**Green:** `MultitrackTest.*` (gpu, 7) and `RealTargetTest.*` (gpu, 4),
`AppAudioTracksTest.*` and `MultitrackTargets.TheTargetListIsParsedInOnePlace` (cpu, 7),
and five settings-dialog cases plus two engine cases in `gui/tests`.

### What each clause of §8.6 turned into

| §8.6 says | asserted by | measured |
|---|---|---|
| "Max **6** tracks. Track 0 is **always** the full system mix" | `MoreTracksThanTheSpecAllowsAreRefusedRatherThanTruncated` + `FiveTracksIsTheLimitAndIsAccepted` (cpu) | a sixth per-application track is refused with `MULTITRACK_TRACK_LIMIT_EXCEEDED`, not silently dropped |
| "Every track gets a human-readable Matroska `Name` tag ... Untagged tracks are a UX failure" | `EveryTrackCarriesItsOwnTone…` reads the names back out of the decoded file | `System Mix`, `app1.exe`, `app2.exe`, `app3.exe` |
| "All tracks share one timebase, one epoch (`t0`), and identical duration" | same, per track | **worst offset −62 µs over 60 marks** against row 14's 20 ms; lengths within 20 ms of 20.010 s |
| "A track that starts late is **silence-padded from `t0`**, never offset" | `ATrackThatStartsLateIsSilencePaddedFromTheEpochRatherThanOffset` (cpu) | 5 s of quiet then 5 s of tone yields a **10 s** track with **5 s** of manufactured silence, 0 drops |
| "silent far more often than the system mix ... test it as the primary path" | `ATrackThatNeverReceivesABufferIsStillExactlyAsLongAsTheRecording` (cpu) | a track fed **nothing at all** is 10 s of a 10 s recording, all of it silence, with AAC packets actually produced |
| "N tracks = N drift loops, not one shared one" | `EachTrackCarriesItsOwnTimelineAndDriftLoop` (cpu) | two tracks, same length, **0.0 s vs 5.4 s** of silence — the control that a shared timeline would fail |
| "target process exits mid-recording → that track continues as pure silence to the end" | row 15, above | 12.010 s against 12.010 s, 7.986 s silence |
| "poll by executable name at 2 Hz" | `AppAudioTracks::poll_targets`, exercised by row 15's attach | 500 ms interval, shared with the watchdog tick on `fc-atracks` |
| "the format is supplied, not negotiated ... 48 kHz, 32-bit float, stereo" | `process_loopback_format()`, and row 15 activating with it | 400 buffers from a real target, none rejected |
| "It is **initialize-once**" | `AppAudioTracks::detach_locked` tears the client down rather than reconfiguring | `attached` false and `target_exited` true after the exit |
| "MP4 is a hard block, not a soft warning" | row 16, above | three independent refusals; both engine-side ones removed before the test would pass |

### "Tier A provably unaffected", measured against a control

This is the clause that is easy to assert and hard to earn. The argument is structural —
`AppAudioTracks` sits **beside** `AudioPath` rather than around it, so turning Tier B on
constructs a second object and changes no line of track 0's path; there is no
`if (multitrack)` anywhere in `AudioPath`, `AudioEncodePath`, `AudioTimeline`,
`DriftCompensator` or `Resampler`.

**That argument is not evidence, because the two halves do share things:** one mux queue,
one mux thread, one `AVFormatContext`, and a `Block`-policy queue that now has four
producers where it had two. BUG-044 is the reason that matters — a subsystem that could
not affect the recording *by design* corrupted it anyway, through a resource neither
component's design mentioned, with every counter clean.

So the measurement is two recordings of **identical** synthetic input, differing only in
whether three per-application tracks exist, with the system mix's decoded PCM compared
sample for sample:

| | Tier B off | Tier B on |
|---|---|---|
| audio tracks in the file | 1 | 4 |
| system-mix samples decoded | 384,000 | 384,000 |
| **system-mix samples that differ** | — | **0 of 384,000** |
| channels / rate / encoder delay / first sample time | identical | identical |
| video frames decoded | 480 | 480 |

Bit-exact rather than within a tolerance, because the input is deterministic and AAC is
deterministic for identical input — so any difference at all is Tier B having reached
track 0. **Verified red-before**: a one-line change letting the Tier B branch set track
0's bitrate produced **25,405 of 384,000 samples differing**, which is what this case
exists to see.

Three smaller pieces of the same claim, stated because they are cheap and were checked:

- A Tier A file's **audio stream is still untagged**. `Muxer::open`'s single-encoder
  overload passes an empty name, so no existing recording's metadata gains a `title`.
- A Tier A `get_stats` payload gains **no fields**. `audio_tracks` is present only when
  Tier B opened tracks.
- The CPU tier's 406 cases and the GPU tier's pre-existing cases are unchanged in count
  and in outcome.

### The 30-minute form of row 14

§8.6 asks for "per-track sync < 20 ms over 30 min", and the routine suite runs twenty
seconds. `FC_MULTITRACK_SECONDS=1800` is the exit-criterion form.

**It runs on a synthetic clock, not in real time.** Row 14's claim is about where the
timeline puts audio, and every stream in the deterministic case is generated from one
supplied epoch — so 1800 s of *timeline* costs what encoding 108,000 frames costs, not
half an hour of waiting. That is the same trade `test_av_sync` makes at
`FC_AV_SYNC_SECONDS=1800`, and it is available for the same reason.

**Measured at `FC_MULTITRACK_SECONDS=1800`, on Release:**

| | 20 s (routine) | 1800 s (exit criterion) |
|---|---|---|
| marks compared | 60 | **5400** |
| worst per-track offset | −62 µs | **−62 µs** |
| system-mix length | 20.010 s | **1800.000 s** |
| wall clock | 4.3 s | **376 s** |

**The two worst offsets are the same number, and that is the result rather than a
coincidence.** −62 µs is 3 samples at 48 kHz — a fixed quantisation, not an error that
grows — so the long run's value is that it says so: over ninety times as many marks, the
figure did not move. A constant offset and one accumulating at a microsecond a second
look identical over twenty seconds and differ by 1.8 ms over half an hour, and only the
long form can tell them apart. This is the same distinction BUG-016's growth check exists
to make, applied across tracks instead of across streams.

Six minutes rather than thirty, because the clock is synthetic — 1800 s of *timeline*
costs what encoding 108,000 frames costs. Row 15's real-client case cannot make that
trade and is 12 s of real time.

### Real applications, and what the deterministic cases could not have caught

`MultitrackTest.*` feeds each track separately from one synthetic clock, which is what
makes row 14's 20 ms assertion meaningful — and it means **a build that mixed every
application into every track would pass all of it.** Isolation is the property Tier B
exists for and the one that seam cannot test at all.

`RealTargetTest.*` (gpu, 4) drives the real `ActivateAudioInterfaceAsync` client against
real target processes. `fc_audio_target` grew three shapes a real application has and the
first version did not: several concurrent render clients, rendering from a **child**
process, and asking for a sample rate the endpoint does not use.

| what it covers | measured |
|---|---|
| **Six tracks at once** — §8.6's stated maximum, five process-loopback clients plus the system mix | 6 tracks, all within 0.5 s of the mix's **10.026 s** |
| **Per-track isolation**, five applications rendering to one endpoint simultaneously | worst track carries its own tone **46×** louder than the loudest foreign one |
| **`INCLUDE_TARGET_PROCESS_TREE`** — a parent that renders nothing, a child that does | track energy **0.00740** against the system mix's **0.00821** at the child's tone |
| **An application at 44.1 kHz** with `AUTOCONVERTPCM`, and **two concurrent streams** | track comes back at **48 kHz stereo** (§8.6's supplied format) carrying **both** tones |

**Verified red-before, and the break is worth recording because of how loud it was.**
Flipping one enumerator — `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE` instead of
`INCLUDE` — inverts the isolation measurement by about three orders of magnitude: every
track then carries **0.0044–0.053** at other applications' frequencies and **1.1e-5 to
3.6e-5** at its own. The child-process case goes red at the same time and says so in as
many words. A single wrong enumerator is the whole of what stands between Tier B and a
file where every track is the system mix.

### Tier B alongside pause, segmentation and a device loss

Each of these existed before Tier B and each *inherits* rather than gains a mechanism when
it is on. That is an argument; BUG-044 is what happens when an argument of that shape goes
unchecked. All three are now measured.

| | measured | red-before |
|---|---|---|
| **Pause/resume** (§7.5) — three uneven pauses totalling 3.5 s across four tracks | **3498 ms** excised (12.010 s → 8.512 s), every track within 50 ms of the mix, worst per-track offset **−83 µs** | app tracks denied the shared `PauseClock`: track 2 comes back **12.010 s against the mix's 8.512 s**, and the §10.4 gate rejects the file before the test's own assertion runs |
| **Segmentation** (§11) — a rollover reopens a new `AVFormatContext` mid-recording | **3 segments, 4 named tracks in every one**, `System Mix` and `appN.exe` intact across both seams | rollover reopening with only track 0: `..._part002.mkv carries 1 audio tracks` |
| **A §5.4 device loss** with a live process-loopback client attached | **1 rebuild, 82 ms** against §5.4's 350 ms; one file; client still attached; track 12.010 s against the mix's 12.032 s; the target's tone present **before (0.0155) and after (0.0273)** the seam | a rebuild that stopped the app tracks: track **4.075 s against 12.053 s**, `attached` false |

Two of those three breaks were caught by `Muxer::validate`'s per-track length check before
the test's own assertion was reached, which is the answer to why that check is in the gate
rather than only in the test: a ragged track is the defect the gate is in the best position
to see, and §10.4 already forbids declaring a file good without looking.

### The four decisions of 2026-08-06, and what each one is worth

Open questions 15 through 18 were answered together, and three of the four changed code
rather than only wording.

| | decided | measured |
|---|---|---|
| **Silence generators** (q15) | one thread **per track**, not one shared ticker | `asil`N in §12's table. The shared ticker was not merely less literal: `request_silence` blocks under §12's `Block` policy, so a wedged `aenc` would have stalled every other track's timeline behind it |
| **A target that restarts** (q16) | the track **follows its executable back**, `audio.multitrack_reattach` default on | **2 exits, 1 re-attachment** against two real processes; track's own tone **0.0135** while the first played, **0.000000** between, **0.00245** after the replacement |
| **The target list** (q17) | §16.4 describes it; a **picker** appends to the text field, fed by `get_devices`'s new `processes` field | one entry per executable name, reduced engine-side so picking and typing resolve identically; four dialog cases |
| **§8.5's pin** (q18) | **per stream**, not per file | the §10.4 gate checks the system mix and the application tracks against **separate** expectations, because one could not tell "correctly different" from "wrong" |

The re-attachment case is the one worth reading. Both readings of §8.6 keep the track the
full length — the generator never stops — so no duration assertion can distinguish them,
and the only thing that can is the **content in three windows**: loud, silent, loud. The
middle window is what makes the outer two mean anything, and it is what a version of this
test without it would have been missing.

### What Tier B does *not* cover

- **No real *third-party* application.** `fc_audio_target` now has the shapes that matter
  — a process tree, several concurrent clients, a mismatched sample rate — but it is still
  a program written to be recorded. A browser, a game or an application using exclusive
  mode is not exercised, and cannot be without making the suite depend on what is
  installed (CLAUDE.md §5).
- **The QPC fallback path has never executed.** `ProcessLoopbackCapture` stamps a buffer
  from `qpc_now_ns()` when the virtual device supplies no `u64QPCPosition`, because the
  device is not documented to supply one. Measured across every real-client case on this
  rig: **0 fallbacks**. The path exists on evidence about the documentation, not on
  evidence about this machine, and it is untested.
- **No long Tier B soak.** SPEC.md §20.1's four-hour run belongs to M10, and Tier B
  multiplies what it covers: six timelines, six device clocks, eleven extra threads and a
  2 Hz poll running for four hours. Memory and handle flatness with Tier B on is measured
  nowhere.
- **A cross-adapter migration with Tier B on.** The same-adapter rebuild is measured above;
  the cross-adapter path closes the file and opens a new one (§5.4's amendment), which is
  the segmentation case's shape with a different trigger. Untested together.
- **Tier B has never been driven from the GUI end to end.** The dialog's controls, the
  `save_config` round trip and the engine's refusal are each tested; a user ticking the
  box, naming an application and getting a multi-track file is not.

## Row 17 is FFmpeg-to-FFmpeg, and BUG-048 is what that costs

Worth recording once, because the row is Green and stayed Green through a defect that
made a recording's audio unplayable.

`ChannelLayoutTest` asserts four things about a decoded file: the container's channel
mask, the presence of an `AudioSpecificConfig`, the layout the decoder derives from it,
and per-channel tone identity. All four were true of the 7.1 file a user reported as
unplayable. They were true because the loop is closed — **FFmpeg's decoder reading
FFmpeg's encoder** — and the question that decided the outcome was one no participant in
that loop can ask: *will anything else play this?*

Two distinct interop problems live in that gap, and only one of them is now closed:

- **Channel count (BUG-048, fixed).** Windows' Media Foundation AAC decoder accepts 1, 2
  and 6 channels. An eight-channel AAC-LC stream has no supported input type there, so
  Windows' player refuses the audio and offers the video. §8.5's pin now overrides the
  endpoint downward only, so an 8-channel track is only ever produced from an endpoint
  that genuinely has eight channels.
- **Channel *order* on a genuine 7.1 endpoint (open).** §8.5 already records it: FFmpeg
  writes `AV_CH_LAYOUT_7POINT1` as AAC channel configuration 7 and reads it back the same
  way, while a strict decoder reads configuration 7 as 7.1(wide) and places channels 6
  and 7 at `FLC`/`FRC` rather than `SL`/`SR`. That is a *mis-mapping*, not a refusal, and
  it is invisible to this row for exactly the reason above. It has never been observed,
  because no machine here has a 7.1 endpoint to observe it on.

**What would close the gap** is decoding one recording per layout with something that is
not libavcodec — the Media Foundation decoder is present on every target machine and is
reachable from a test. It is not written, and until it is, row 17 means "FFmpeg
round-trips this correctly" and not "this plays".

## Running the suite: two rows are load-sensitive, and they fail loudly rather than flakily

Worth recording once, because it costs a confusing investigation otherwise and the failure
looks exactly like a regression in the product.

**Do not run anything heavy while the GPU tier runs.** Two cases assert *latency budgets*
rather than outcomes, and both measure the machine as much as the code:

| test | budget | idle | with `lint.ps1` (clang-tidy, 16-way) running alongside |
|---|---|---|---|
| `GpuMigrationTest.RepeatedRebuildsStayWithinBudgetAndKeepTheTimelineMonotonic` | 350 ms (§5.4) | **21.8 ms** | **871 ms** — failed |
| `AudioDeviceMigrationTest.ALiveEndpointMigrationKeepsRecordingAndStaysInsideTheGapBudget` | 200 ms (§14.1) | passes | **220 ms** — failed |

Measured 2026-08-04, on the same build, minutes apart. A forty-fold spread on the first is
not jitter — it is a rebuild that opens a device and an encoder, competing for cores with
sixteen compilers.

The budgets are not wrong and should not be relaxed: §5.4 and §14.1 state them, and they
are met with two orders of magnitude to spare on an idle machine. What is worth knowing is
that a red result from either of these is a question about what else was running before it
is a question about the code. CLAUDE.md §6 asks for load-dependent conditions to be named
where a defect depends on one; this is the same rule applied to a *false* one.

## Open questions for the owner

Recorded here because CLAUDE.md §7 says to stop and ask rather than guess at a
media-pipeline requirement, and because each of these changes what gets built.

1. ~~**Does row 13 belong to M8, alongside row 18?**~~ **Answered 2026-08-02 — row 13 is
   in M8.** See "Row 13's milestone, decided" above for the reasoning. One thing is still
   the owner's: SPEC.md §24's M8 exit criterion should be amended to read "tests #13 and
   #18 green", because it currently names only #18.

2. ~~**Sequencing: fix BUG-033 before starting M8.**~~ **Done 2026-08-02.** Eight capture
   cases — the five that failed plus three with the same defect that had not — now
   present their own pattern to the target output and occlude it, so their input is owned
   rather than ambient. See BUG-033 in `docs/ENGINEERING_LOG.md` and "The capture tests'
   input is now theirs" above.

3. **Row 11 versus §5.4: what should a cross-adapter migration produce?** Measured, a
   single continuous file is not achievable across vendors (see row 11's note above).
   Two coherent readings:
   - **Amend row 11.** A migration that stays on one adapter yields one continuous file
     and meets the 350 ms gap; a migration that crosses adapters closes the segment and
     opens a new one, logged loudly, as §5.4 already provides for. Row 11's test then
     asserts continuity for the same-adapter case and a clean, loud split for the other.
   - **Amend §5.4.** Forbid cross-adapter migration mid-recording entirely: on a
     topology change, finish the current file and require the user to start a new
     recording. Simpler and never surprising, but it gives up the feature §5.4 exists
     for on exactly the MUX-less laptop this project targets.

   The first is my recommendation — it keeps the feature and matches what the hardware
   permits — but it is a change to a graded requirement, so it is yours.

4. **SPEC.md §13 rungs 1 and 2 (BUG-024).** Unimplementable through libavcodec on
   hardware; the ladder computes and logs them but nothing can carry them out.
   Degradation steps from nominal straight to rung 3's halved capture rate.

5. **BUG-021**, MP4's 21 ms audio head offset — open since M5, bounded and pinned, and
   **now needing a decision rather than more attempts.** Retried 2026-08-02 with
   `use_editlist=1` and `avoid_negative_ts=disabled` both *confirmed applied* (empty
   unconsumed-option list; the flag read back as 0), plus the audio shifted back by the
   priming. Identical result to the first attempt: mark 0 correct, marks 1–19 at
   −21.52 ms, the whole track slid rather than trimmed. Reverted — one bad mark is
   better than nineteen.

   Three attempts have now produced the same wrong answer, so the remaining candidate is
   `delay_moov` instead of `empty_moov` during recording. That would let the delay be
   declared from the start, and it trades away precisely the crash-safety guarantee
   SPEC.md §10.3 mandates `empty_moov` for. **That trade is yours, not a code change I
   should make.**

6. **The GPLv2 consequence of libx264** for §21's packaging and installer work.

12. **SPEC.md §15.2's preview section: named, or `HANDLE`-passed?** The section's one
    sentence specifies both — "Named shared memory (`CreateFileMapping`) triple-buffered
    ring, one `HANDLE`-passed section" — and they are alternatives, not stages. A named
    section is opened by name and needs no handle duplication; a duplicated handle needs no
    name.

    **Built as named**, for the reasons in `preview_ring.h`: it is the clause that names the
    API, handle duplication would need the engine to hold `PROCESS_DUP_HANDLE` on the GUI
    (inverting §3.1's parent/child ownership), and `mmap(tagname=...)` is the only form
    Python has without a `ctypes` shim around three Win32 calls — which §16.1's "wraps a
    QImage over the shared-memory buffer and nothing else" reads as a budget.

    The two are not equivalent in one respect worth your eye: a named section can be
    *opened by any process running as this user that guesses the name*, where a duplicated
    handle can only be used by the process it was duplicated into. The name carries the
    session GUID and lives in `Local\`, so it is not guessable in practice and not visible
    across logon sessions — but §15.1 states a SID-restricted ACL for the control channel
    and §15.2 states nothing for this one, and "the preview is readable by this user's other
    processes" is a property that should be intended rather than inherited. **§15.2 should
    say which mechanism, and whether the section wants an ACL.**

13. ~~**SPEC.md §12's thread table has no `preview` row, and this build adds one.**~~
    **Answered 2026-08-06 alongside question 15:** §12's table now carries `preview`
    (`BELOW_NORMAL`, blocking allowed) along with Tier B's five rows. The split it
    describes is unchanged — the dispatch stays on the capture thread because that is
    where the source texture is live, and the readback's `memcpy` stays off it because
    CLAUDE.md hard rule 4 exists so per-frame costs do not accumulate there.

    The original question, for the record:

    **SPEC.md §12's thread table has no `preview` row, and this build adds one.**
    `fc-preview`, `BELOW_NORMAL`, blocking allowed. The split is forced: the shader dispatch
    must be on the capture thread because that is where the source texture is live (§15.2's
    "off the same source texture"), and the readback's two-megabyte `memcpy` must not be,
    because CLAUDE.md hard rule 4 exists so that per-frame costs do not accumulate there.
    Measured, the capture thread pays 35–56 µs per offered frame. **§12's table should gain
    the row**, as §15.1's command list should gain `get_config`/`save_config`.

14. **The preview cannot be aimed.** §15.1 gives `start_preview` no parameters, so a
    preview-only session previews the primary display, and previewing a *chosen* monitor
    before recording it is not expressible. §16.2's layout implies you would want to —
    the source list sits directly under the preview surface. Adding a `monitor` param is a
    one-line change to the handler and a change to a specified command signature, so it is
    yours rather than mine (CLAUDE.md §9).

9. **SPEC.md §3.1 states two rules that cannot both hold when the GUI is *killed*, and
   M8a had to pick one.** This is the one decision in M8a I could not make from the spec
   alone, and it is implemented the way §20 row 13's own test text implies.

   The two rules: the job carries `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, **and** on
   heartbeat loss the engine "finalizes the current recording cleanly, then exits. It does
   **not** discard the file." `KILL_ON_JOB_CLOSE` terminates the job's processes when the
   last handle closes; killing the GUI closes its handles; so if the GUI holds the only
   handle the engine dies in the same instant, with no chance to run the second rule.

   **Row 13's test text picks a side:** "kill the GUI, assert the engine exits within
   **6 s** having **finalized** the file." Six seconds is the 5 s heartbeat timeout plus
   one — a number from the heartbeat path, applied to a kill. So the engine now opens its
   own handle to the job, the job outlives the GUI, and the sequence measured is: notice
   at ~5 s, finalize, exit at **5755 ms**, job dissolves.

   **The cost, which is the part that needs your pen:** `KILL_ON_JOB_CLOSE` can no longer
   reap a *frozen* engine, because a process that cannot run its heartbeat thread cannot
   close its handle either. The residual orphan case is "engine wedged **and** GUI gone".
   The other reading — engine holds no handle, the kill is unconditional, and row 13's
   "finalized" is delivered by §10.4's repair path on the fragment-consistent file the
   kill leaves behind — covers that case and gives up the clean finalize in every case.
   Neither covers both.

   My recommendation is what is implemented: it favours CLAUDE.md §1's prime directive in
   the common case, and the residual case is one no in-process mechanism can cover anyway.
   But it is a reading of a graded requirement, so it is recorded here rather than
   settled. **§3.1 should say which.**

10. **Row 13's 6 s budget is 5 s of heartbeat plus ~700 ms of finalize, and the second
    number is not bounded by anything.** Measured on this rig: the engine noticed at the
    5 s timeout and had the file finalized and validated 755 ms later, for 5755 ms against
    6000 ms. That margin is a property of *this* recording — 7.4 s of 1080p60 MKV. A
    longer MP4 recording pays §10.3's fragmented-to-progressive remux on the same path,
    and a stalled disk pays §13 rung 6's latency, either of which could exceed the budget
    with nothing wrong with the mechanism.

    Two coherent answers, and it is your call which: **shorten the heartbeat timeout** so
    more of the 6 s belongs to the finalize (§3.1 fixes it at 5 s, so that is a spec
    change), or **state row 13's budget as "timeout + finalize" rather than a constant**,
    which is what it actually is. Nothing is changed here; the assertion stands at the
    literal 6 s the row specifies.

    **The margin measured across three presets: 287 ms, 363 ms, 791 ms** (Debug,
    RelWithDebInfo, Release) on a 7.3 s MKV. That is the headroom a longer recording, an
    MP4 remux or a slow volume would be eating into.

11. ~~**The GUI cannot persist settings.**~~ **Answered 2026-08-03 by the owner: persist
    them.** Implemented as the recommendation below — `get_config` and `save_config` added
    to §15.1's command list, the engine keeping sole ownership of the file. **§15.1 should
    be amended to list eighteen commands rather than sixteen**; that is a change to a
    graded requirement and is recorded here rather than made silently.

    Verified across a real restart, not within one session:
    `test_settings_survive_an_engine_restart` writes settings through one engine, shuts it
    down, starts a second process, and asserts it reports them back — a `get_config` that
    merely echoed `save_config` would pass the weaker form even if nothing reached disk.
    The original question, for the record:

    **The GUI could not persist settings, and closing that needed a command §15.1 did not
    have.** SPEC.md §17 says configuration "is loaded once by the GUI, validated, and
    pushed to the engine via `configure`. The engine is stateless with respect to disk
    config — a single source of truth eliminates an entire class of 'the GUI says 60 fps
    but the engine recorded 30' bugs."

    The push half works. The *load and save* half does not, because §17 also specifies
    atomic writes, `schema_version`, an ordered migration chain and preserving unknown
    keys — all of which `engine/core/config/` already implements and tests. Reimplementing
    that in Python would put two writers with two ideas of the schema on one file, which
    is the failure §17 is written to prevent, arriving through the door marked "single
    source of truth".

    Three coherent answers, and it is your call:
    - **add `save_config` / `load_config` to §15.1** and let the GUI drive the engine's
      implementation — smallest code, but it extends a specified command list;
    - **let the engine own the file entirely** and have `configure` persist as a side
      effect — no new commands, but it makes the engine stateful with respect to disk,
      which §17 explicitly rules out;
    - **port §17 to Python** — matches §17's letter and duplicates the migration chain.

    My recommendation was the first, and it is what was built.

7. ~~**BUG-034: row 3 lost a whole fragment.**~~ **Fixed 2026-08-02.** Loss is
   478–495 ms where it was 1978 ms. Two things are left for you, neither blocking:
   - **SPEC.md §10.3's prose.** Its `movflags` line is unchanged and every flag it names
     is still set, but "every 2 s keyframe closes a self-contained fragment" is now
     "every 500 ms, or a keyframe, whichever comes first" — `frag_duration` is a separate
     movenc option, added, not substituted. One line.
   - **MKV's cluster limit**, per the note above: §10.2 states `cluster_time_limit=2000`,
     so MKV keeps the 121 ms margin MP4 has just shed.

8. ~~**BUG-035: one burst of device failures rebuilds the session twice.**~~ **Fixed
   2026-08-02.** There was no latch — the burst was coalesced only for as long as the
   rebuild happened to last. `gpu::kReportSettleNs` replaces the coincidence with a stated
   rule. 0 of 20 after, 6 of 22 before. Rows 9 and 11 are Green again. The one judgement
   worth your eye: a genuinely new fault inside the settling window is delayed by up to
   one poll interval, which is written up in BUG-035 and in `kReportSettleNs`.

15. ~~**SPEC.md §12's thread table gains four more rows, and §8.6 is why.**~~
    **Answered 2026-08-06 by the owner: the table gains the rows, and the generators are
    per track.** §12 now carries `preview`, `atracks`, `asil`N, `ploop`N and `aenc`N, with
    a note on what eleven extra threads at six tracks buys.

    **The shared ticker was replaced rather than documented**, which is the part worth
    keeping. The reading was defensible — each track had its own generator *state* — and
    it was wrong for a reason the wording hints at and the argument for sharing missed:
    `request_silence` pushes onto the track's own queue under §12's `Block` policy, so a
    track whose `aenc` had wedged would have blocked every other track's tick behind it,
    and six timelines would have stopped advancing because one application's encoder was
    stuck. `AppAudioTracksTest.EachTrackCarriesItsOwnTimelineAndDriftLoop` covers the
    independence; the coupling itself is structural now and has nothing left to test.

    The original question, for the record:

    **SPEC.md §12's thread table gains four more rows, and §8.6 is why.** Question 13
    recorded one addition (`fc-preview`); Tier B adds `fc-atracks` (one per recording,
    `NORMAL`, blocking allowed — it runs §8.6's 2 Hz poll and every track's §8.2
    watchdog), and per per-application track `fc-ploop-<pid>` (MMCSS "Pro Audio",
    never blocking) and `fc-aencN`. At §8.6's six-track maximum that is 1 + 5 + 5 =
    **eleven** threads a Tier A recording does not have.

    Two things in that need your pen rather than mine:

    - **One watchdog thread serves N tracks.** §8.6 requires a silence *generator* per
      track and there is one — each track's `AudioTimeline` decides its own gap from its
      own last packet and fills its own duration. What is shared is the timer that asks.
      Six threads waking every 10 ms to ask six independent questions is six times the
      wakeups for no additional independence, but §8.6's sentence is "every track
      therefore needs its own `SilenceGenerator`", and whether that means the state or
      the thread is a reading.
    - **`fc-aenc` is now `fc-aenc1`…`fc-aenc5` for the per-application tracks**, and
      Tier A's keeps its old name so nothing that greps a log changes. Names in a
      minidump are what CLAUDE.md §4 asks for; a table that names one of six is not.

    **§12's table should gain the rows**, as §15.1's command list should gain
    `get_config`/`save_config` and §12's should gain `fc-preview`.

16. ~~**§8.6 says a track whose target exits "continues as pure silence to the end of the
    file" — should it?**~~ **Answered 2026-08-06 by the owner: the track follows its
    executable back.** §8.6 amended, `audio.multitrack_reattach` added (default `true`,
    `false` gives the literal reading), and a **pid**-pinned track still latches because
    there is nothing to resolve.

    Measured, against two real processes sharing one executable:
    `RealTargetTest.ATrackReattachesWhenItsApplicationIsClosedAndReopened` records
    **2 exits, 1 re-attachment**, with the track's own tone at **0.0135 while the first
    target played, 0.000000 between them, and 0.00245 after the replacement started**.
    The middle window is what makes the outer two mean anything. Verified red-before with
    `reattach = false`: the track is silent for the rest of the recording and the case
    says so in as many words.

    The original question, for the record:

    **§8.6 says a track whose target exits "continues as pure silence to the end of the
    file", and the implementation reads that literally — should it?** Once a target has
    gone, the 2 Hz poll stops looking for it. A user who closes a game and reopens it
    mid-recording gets one track with four minutes of audio and twenty of silence.

    The literal reading is also the safe one: a relaunched application is a different
    process, and silently re-attaching would put a second application's audio on a track
    the user aimed at the first — with nothing in the file to say where one ended. The
    other reading is "the *track* follows the executable, not the process", which is what
    "poll by executable name at 2 Hz" arguably already implies for the not-yet-started
    case and would be strange to switch off after one exit.

    My recommendation is what is implemented, because the failure mode of the other
    reading is silent and this one's is visible. It is a reading of a graded requirement,
    so it is recorded rather than settled.

17. ~~**Tier B's target list is a new config key, and §16.4 does not describe one.**~~
    **Answered 2026-08-06 by the owner: §16.4 describes it now, and it gained a picker.**
    The text field stays the source of truth — §8.6 supports naming a target that has not
    started yet, which a picker cannot express — and the picker **appends** to it, so a
    game that is not running can be typed and a browser that is can be clicked.

    `get_devices` gained a `processes` field for it: one entry per **executable name**,
    reduced engine-side so that picking `chrome.exe` and typing `chrome.exe` resolve to
    the same target. An additive field on an existing command rather than a new one,
    because §15.1's compatibility rule makes unknown fields free and its command list is a
    graded requirement already extended once. Four settings-dialog cases cover the picker,
    including an engine that reports nothing leaving the field usable.

    The original question, for the record:

    **Tier B's target list is a new config key, and §16.4 does not describe one.**
    §16.4's Audio section is "device, channel layout, bitrate, multi-track when MKV", and
    §8.6 identifies a target by executable name and polls for it — but nothing says where
    that list of names lives. `audio.multitrack_targets` is a comma-separated string,
    documented in `docs/CONFIG.md`, presented as a text field beside the checkbox.

    Two consequences worth your eye. **It is a string rather than a TOML array** because
    §17's schema table has no array type, and adding one for a single key would be a
    schema change carried by one caller. **The GUI offers no picker**, so a user has to
    know that Chrome is `chrome.exe`; `audio::enumerate_processes` exists and would make
    a dropdown a small change, but it is a §16.4 addition rather than an implementation
    detail.

18. ~~**§8.6 fixes per-application tracks at stereo even when the user chose 5.1.**~~
    **Answered 2026-08-06 by the owner: §8.5's pin is per *stream*.** The section now says
    so, and the §10.4 gate checks the two expectations **separately** —
    `ValidationExpectation::audio_channels` for the system mix and `app_track_channels`
    for the rest — because a single expectation could not tell "correctly different" from
    "wrong". A build that applied the recording's 5.1 to an application track would write
    a valid file at three times the bitrate carrying stereo content in six channels;
    one that applied stereo to the system mix would discard the surround the user asked
    for. `AppAudioTracksTest.PerApplicationTracksAreStereoWhateverTheRecordingChose`
    pins the encoder side.

    The original question, for the record:

    **§8.6 fixes per-application tracks at stereo, and this build follows it even when
    the user chose 5.1 or 7.1 for the system mix.** "Supply 48 kHz, 32-bit float, stereo
    and let `libswresample` handle everything downstream" is unambiguous about the
    capture format; what it does not say is what the *encoded* track should be. Pinning
    the per-application tracks to stereo rather than to the recording's chosen layout
    means a 7.1 recording's six tracks are one 7.1 and five stereo — which is correct
    (there is no surround information in a stereo capture to preserve) and is a shape
    §8.5's "channel layout is pinned for the lifetime of the file" does not obviously
    anticipate, since it now means *per stream* rather than *per file*.

M9.6's three questions were put in `M9_6_PLAN.md` §0.3 and **all three are answered**:

- ~~**Do rows 19-22 join §20?**~~ **Answered 2026-09-06: yes.** §20 is twenty-two rows and
  §25's Definition of Done reads "all 22". Both edits are made.
- ~~**Three hotkey bindings or two?**~~ **Answered during Phase 3: three** — `start`, `stop`
  and `pause_resume`, with `start` and `stop` sharing a default so the familiar toggle
  survives.
- ~~**Is the recording pill on by default?**~~ **Answered 2026-09-06: yes**, gated on the
  capture-exclusion probe, so it can never reach a recording.

**One new question, and it is now a CI gate rather than a note.** The repository has no
`LICENSE` file, and `scripts/pr-gates.ps1` fails without one — deliberately (M9.6 plan
§7.4). CLAUDE.md §9 already records the position: libx264 is in, `--enable-gpl` is set, the
distributed binary is therefore **GPLv2**, and that obliges source availability for the
distributed work while being incompatible with a proprietary licence. The obligation exists
whether or not the gate does. What the gate adds is that §21's packaging work cannot start
without the question being answered first, which is what CLAUDE.md §9 asked for when it
said to flag it rather than work around it.

**Two SPEC amendments were made under M9.6 rather than asked about**, because in both cases
the alternative was a spec sentence that contradicted a measurement:

- **§15.1's timeout sentence now names `recover` alongside `stop_record`.** It named only
  `stop_record` because nothing called `recover`; M9.6 Phase 4 gave it a caller, and it runs
  the same lossless remux — ~3.3 s plus ~1 s of validate on a 1.2 GB file (BUG-046). At 5 s
  it would time out on every recording worth recovering.
- **§16.2 and §16.4 gained the overlay, the menus and the Hotkeys section**, which describe
  what was built rather than changing what was required.

## M9.6's four rows, and what they do *not* cover

Rows 19–22 joined §20 on 2026-09-06 by the owner's decision, taking the matrix to
twenty-two and §25's Definition of Done to "all 22". Four notes, in the order they would
mislead someone quoting the rows.

### Row 19 is measured against a capture, not against an API return value

The tempting version of this test asserts that `SetWindowDisplayAffinity` returned
`TRUE`. That would have passed for `WDA_MONITOR` as readily as for
`WDA_EXCLUDEFROMCAPTURE` — both succeed, and one of them paints black into every frame.
The two constants are one hex digit apart (`0x11` and `0x01`), and the wrong one is what
a search result from before 2020 hands you.

So the row is measured on decoded frames: mean luma and the fraction of the frame the
overlay covers, with the overlay genuinely on screen, on both capture backends. The
control matters as much as the assertion — an unstamped overlay reads **145.8** mean luma
and **1.0000** coverage, which is what proves the capture could see it in the first place.
A test that only ever saw the excluded case would pass on a capture that was broken.

**What it does not cover.** One machine, one GPU pair, one Windows build. The affinity is
a DWM behaviour and it is honoured here on both WGC and DDA; a machine whose compositor
is off, or a remote session, has not been tried. `ExclusionSupport.UNSUPPORTED` is the
designed answer to that and it is exercised, but only by forcing it.

### Row 20's 3% figure is not measured, and this is what is measured instead

SPEC.md §16.1 caps GUI CPU at **3% while recording**. That is a process-wide number over
a real recording, and **it is not measured here.** Reporting a 3% pass would need a
sampler running against the GUI process during a genuine capture, and no test does that.

What is measured is the cost of the thing M9.6 added to that budget:

| Quantity | Measured |
|---|---|
| Pill repaint, inside `paintEvent` | mean **0.124 ms**, worst **0.459 ms** over 127 paints |
| Repaint rate during a save | **9.8 paints/s** against row 20's floor of 8 |
| Click → visual acknowledgement | stop **0.120 ms**, pause **0.022 ms** against a 16.7 ms frame |
| Event-loop ticks during a real `stop_record` | **4** (MKV), **5** (MP4); zero under the old synchronous stop |

At the pill's 10 Hz repaint rate, 0.124 ms per paint is **1.24 ms of CPU per second, about
0.12% of one core**. That is an arithmetic consequence of the two measurements above, not
a measurement of the process, and it is quoted here only to say that the overlay is not
plausibly the reason the 3% budget would be missed. **The §16.1 figure itself remains
unmeasured and belongs to M10's acceptance suite**, alongside the idle-CPU figure §25
also asks for.

The event-loop tick counts deserve one caveat: the recordings under test are one second
long, so their finalization is ~200 ms and 4–5 ticks is what that produces. The count
proves the loop *ran*, which is the row. It does not establish a sustained rate over the
30-second finalization §15.1 budgets for — BUG-046 measured that shape at ~3.3 s of remux
plus ~1 s of validate on a 1.2 GB file, and no test drives a file that large.

### Row 21 is a pytest row, and the thing it cannot test is `RegisterHotKey` itself

Every assertion is on this side of the Windows API: that the sequence parses, that the
right virtual key comes out, that a conflict is retained and reported, that a shared
sequence dispatches to the binding whose precondition holds. Whether Windows then
delivers `WM_HOTKEY` is not asserted, because a test that registered real global hotkeys
would fight the developer's own key bindings and would fail differently depending on what
else was running.

**What that leaves uncovered:** the end-to-end path from a physical key press to a
recording starting. It is exercised by hand and by the toast each hotkey raises; it has no
automated case. The row's *named* causes are all covered — the VK table, the persistent
conflict, and the blocked pump — which is why it reads Green rather than Partial.

### Row 22's container asymmetry is the assertion, not an inconsistency

MKV reports `['validating', 'done']`; MP4 reports
`['flushing', 'remuxing', 'validating', 'swapping', 'done']`. That is not one of them
being wrong. §10.3 gives MP4 a fragmented-then-remuxed path and gives MKV none, so an MKV
stop that reported a remux would be a progress bar announcing work the container does not
do — and the test asserts the *absence* for MKV as firmly as the presence for MP4.

The property that matters most is the one that is easiest to get wrong in a way nobody
notices: **`done` is emitted only after the validation gate passes, and the pill closes
only on `recording_finalized` with `valid: true`.** A bar that reached 100% on the stop
command returning would be the recorder claiming success it had not verified, which
CLAUDE.md §1 makes the one unacceptable outcome. `valid: false` leaves the pill on screen
saying so.

**What it does not cover:** a finalization that fails *midway* — the engine dying between
`flushing` and `done`. The pill's designed answer is to stay up and hand off to §10.4's
recovery path, and `test_overlay_pill.py` covers the widget's half, but no test kills an
engine mid-finalize and watches what the GUI does. That is row 3's territory
(`CrashRecoveryTest`) from the engine side; the GUI-side pairing is not written.

## SPEC.md §20.1's soak tier

`tests/soak/test_soak.cpp` (gpu). Of §20.1's three soak clauses it owns the two nothing
else covers — **flat memory and flat handle count** — and leaves drift to `AvSyncTest`,
which already measures it over 1800 marks.

Measured at 90 s on the reference rig: working set **250.4 → 250.9 MB**, grew **0.50 MB**
against a 3.08 MB budget, handles **1586–1587**, 5406 frames captured and encoded, none
dropped, file valid.

**The assertion is on total growth, not on an extrapolated rate**, and the tier's first run
is why. §20.1's "5 MB/hour" is a four-hour budget of 20 MB; at 120 s it is 0.17 MB, below
the noise. A clean run's 0.75 MB working-set wobble became "40 MB/hour" by multiplying by
65 and failed a budget it had not violated. The bound is now
`5 MB/hour × elapsed + 3 MB`, and the fitted rate is still reported because it is the
clearest illustration of why it cannot carry the assertion.

**§20.1's four-hour form has not been run.** `FC_SOAK_SECONDS=14400` is one variable away
and it is M10's exit criterion; what exists today is the mechanism, exercised at 90 s.

---

## SPEC.md §20.1's chaos tier, and the one injection it does not perform

§20.1 asks for *"randomized injection of `DEVICE_REMOVED`, `ACCESS_LOST`, audio
discontinuity, disk stall, and queue saturation during a 30-minute run; assert the file is
always valid."* `tests/chaos/test_chaos_injection.cpp` (gpu) is that tier. It is worth its
own section because it is the only test in the suite whose assertion is the prime directive
itself, and because it covers four of the five named injections rather than five.

### Why it is not redundant with the single-fault rows

Rows 9, 10 and 11 each establish that the pipeline recovers from one fault **starting from
a healthy state**. None of them establishes that it recovers from a device loss arriving
while the mux queue is already backed up behind a stalled disk, or from a second loss
arriving during the rebuild from the first. Those states are unreachable by construction in
a single-fault test, and they are the states a genuinely failing machine produces — a dying
drive and a flaky driver are correlated far more often than they are independent.

The tier deliberately injects `DEVICE_REMOVED` and `ACCESS_LOST` back to back as one of its
outcomes, so the second lands mid-recovery. Whether `DeviceWatcher`'s settle window
(BUG-035) turns that into one rebuild or two is not asserted — either is acceptable. What is
asserted is that a file comes out.

### What is injected, and what is not

| §20.1 names | Here |
|---|---|
| `DEVICE_REMOVED` | **Injected**, via `RecordingSession::inject_device_error` — the seam a real `DXGI_ERROR_DEVICE_REMOVED` arrives at |
| `ACCESS_LOST` | **Injected**, same seam, `DXGI_ERROR_ACCESS_LOST` |
| disk stall | **Injected**, `SessionSettings::injected_stall_ns` — a real sleep on the real mux thread, the mechanism row 10 already uses |
| queue saturation | **Consequential and asserted.** The stall backs the mux queue up and the encode queue overflows from behind, which is the path a failing drive produces. There is no separate seam because on real hardware there is no separate cause |
| audio discontinuity | **NOT INJECTED — see below** |

**Audio discontinuity has no test seam.** `AudioTimeline` injects silence when it *detects*
a gap, but nothing lets a test *manufacture* one. Doing so needs an injection point in
`LoopbackCapture` or a fake `IAudioClient` — engine work, not test work, and not written.
The audio path is live during a chaos run, so a discontinuity the injected faults happen to
cause would be caught; one that only a deliberate injection could produce is not.

Recorded here rather than left to be discovered, because the file header claiming five
injections while performing four is exactly the failure this document exists to prevent.

### Two guards, both of which have fired

A chaos run that injects nothing passes every assertion while proving nothing, and with a
random schedule that is a live possibility rather than a hypothetical. Both guards below
were added after the state they guard against actually occurred:

- **`applied.empty()` fails the run.** Verified: `FC_CHAOS_SECONDS=1` produces a run whose
  schedule draws no faults, and the tier fails with *"the schedule injected no faults; seed
  228835351 produced a run this tier cannot draw a conclusion from"* rather than passing.
- **Zero queue drops fails the run.** The first version stalled 120 ms every 24 writes and
  measured **zero** drops — §20.1's queue saturation named in the header and absent from the
  run. Sized up to 300 ms every 8 writes, taken from `test_slow_disk`'s measured figures.

### Reproducibility, and BUG-056

The seed is printed on every run, passing or failing, and `FC_CHAOS_SEED` replays it. That
property is the entire justification for a randomised test — without it a failure is an
anecdote — and **it was broken when written**: the reader used `std::atol`, `long` is
32 bits on Windows, and every seed above 2147483647 saturated to `LONG_MAX`. Half of all
seeds. The test printed an instruction that did not work.

Fixed with a range-checked `strtoull`, and verified by running the same seed twice and
comparing the schedules rather than by reading the code. See BUG-056; the lesson generalises
past this file.

### Status: green at both durations, and what that does and does not mean

SPEC.md §20.1's 30-minute form passes as of 2026-09-08, at seed `2991276637`:

```
21 rebuilds, captured 7428, queue-dropped 7001
file: valid 1, 7893 frames decoded, 262.133 s of video
```

**Read the last number before quoting this row as healthy.** That is 262 seconds of video
out of a 1800-second recording. The file is valid and the caller is told about it, which is
the prime directive and is all this tier asserts. Roughly 85% of the content was lost to a
capture-thread stall behind a rebuild that could not join its encoder thread — **BUG-058,
open**. A tier that asserts a floor reports the floor being met, not the room above it.

The tier found BUG-057 on its first full-length run: a prime-directive violation in which
the engine reported a valid recording as lost whenever its pipeline could not be stopped.
That is fixed, and this run is its regression coverage.

### Measured

Routine form, 30 s, seed 3402279752:

| | |
|---|---|
| Faults injected | 3 (one of them a correlated `DEVICE_REMOVED + ACCESS_LOST` pair) |
| Rebuilds | 3, all recorded as migrations |
| Frames captured / encoded | 1781 / 897 |
| Queue-dropped | **1514** — the saturation §20.1 names, actually happening |
| File | **valid**, 1016 frames decoded, 31.70 s of video |

That shape is the row-10 behaviour under duress and it is the point: the recording *got
worse* — more than half the frames shed to keep up with a disk stalling 300 ms every eighth
write — and it did not get *lost*.

### What this tier does not cover

- **Audio discontinuity**, above.
- **The four-hour soak.** §20.1 lists them as separate tiers and this is the 30-minute one.
  The soak is still unwritten and remains M10's.
- **A fault during start-up or during finalization.** The schedule leaves two seconds of
  healthy recording before the first fault, deliberately: a device loss during `start` is
  row 1's territory and would make this tier's failures ambiguous. A fault during
  `stop()` is not injected at all, and the GUI-side pairing for it is named as missing in
  row 22's notes above.
- **Any assertion about *how* it degraded.** Rung, gap and drop-ratio bounds belong to rows
  10 and 11, which assert them precisely. This tier asserts only that a valid file exists,
  because that is the one claim that has to survive every combination.

## Why the names differ

Worth stating once, because it looks like drift and is not. SPEC.md names each row
by its *subject*: `test_no_black_frames`, `test_cfr_exactness`. The tree names each
test by the *claim it makes*: `NoFrameIsEverUniform`,
`EveryFrameOnDiskIsTheFrameThatProducedIt`. The second form is what makes a failure
report legible — a red `NoFrameIsEverUniform` says what is wrong; a red
`test_no_black_frames` says only where to look.

The cost of that choice is exactly this file. If the mapping ever falls out of
date, the spec's requirement ("each row has a named test") becomes unverifiable,
which is worse than either naming scheme.
