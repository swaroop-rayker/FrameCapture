# CLAUDE.md — FrameCapture

**What this is:** an OBS-class Windows screen recorder. C++20 engine process + separate Python/PySide6 GUI process. 1080p SDR @ 60/30 fps, MP4/MKV, multi-channel AAC, built to survive hybrid-GPU laptops where the active adapter changes at runtime.

**The full specification is `docs/SPEC.md`. Read the relevant section before writing code — do not infer requirements.** This file is *how we work*; SPEC.md is *what we build*. When they conflict, SPEC.md wins and you flag the conflict.

---

## 1. Prime directive

> **A failure anywhere in the pipeline must still produce a valid, playable output file.**

Only disk-full or file-handle-loss may violate this, and both must be detected pre-emptively. If a change you're making could result in an unplayable file under any failure mode, stop and say so before proceeding.

---

## 2. Hard rules — violating any of these is a failed change

1. **Never mark a milestone or task complete without its named test passing.** "It compiles" and "it looks right" are not completion. SPEC.md §20 maps every known failure mode to a named test; that mapping is the contract.
2. **Never write `catch(...)` outside a thread entry point. Never write an empty catch block.** See §19 of the spec for the error policy.
3. **Never leave an `HRESULT` unchecked.** Use the `FC_HR(expr)` macro. A bare `hr = foo();` with no check is a review-blocking defect.
4. **Never block the capture thread or the audio thread.** No mutex held across a D3D call, no disk I/O, no allocation on the hot path, no logging above `TRACE`.
5. **Never use unbounded queues.** Bounded + explicit drop policy is the mechanism that makes degradation deliberate. An unbounded queue turns a 200 ms hitch into an OOM.
6. **Never add microphone, webcam, virtual camera, or any network capability.** These are hard non-goals (§0.2). The engine binds no sockets except the local IPC endpoint. If a task seems to require one of these, stop and ask.
7. **Never enable segmentation by default.** Default is `enabled = false`. Always.
8. **Never claim a GPU-dependent behavior works without having run it.** You can — the GPU tier runs on this machine. Run it. See §6.

---

## 3. Commands

```powershell
# Bootstrap (once)
.\scripts\bootstrap.ps1

# Build
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release --parallel

# Test
# SPEC.md §21.2's unfiltered `ctest --preset ... --output-on-failure` runs both
# tiers and is the contract. These two are the same thing split, for iterating.
ctest --preset windows-msvc-release -LE gpu --output-on-failure   # CPU tier
ctest --preset windows-msvc-release -L  gpu --output-on-failure   # GPU tier — runs here too (§6)
pytest gui/tests -v

# Lint (run before every commit)
.\scripts\lint.ps1          # clang-format --dry-run --Werror, clang-tidy, ruff, mypy
.\scripts\lint.ps1 -Fix     # same, applying the clang-format fixes

# Dev run
.\scripts\run-dev.ps1
```

Run `lint.ps1` and **both** test tiers before you report a task done. Not after
being asked.

**Long-running tests take an environment override**, so the routine suite stays
quick and the exit-criterion form is one variable away:

| Variable | Test | Default | Exit-criterion value |
|---|---|---|---|
| `FC_AV_SYNC_SECONDS` | `test_av_sync` | 65 | `1800` (M4's 30 min) |
| `FC_LOOPBACK_SECONDS` | `test_loopback_recording` | 12 | `1800`, real time |
| `FC_AUDIO_DRIFT_MINUTES` | `AudioPathTest` drift run | 30 | — |
| `FC_SUSTAINED_SECONDS` | `test_sustained_60fps` | 20 | `600` (§20 row 6's 10 min), real time |
| `FC_MULTITRACK_SECONDS` | `test_multitrack` row 14 | 20 | `1800` (§8.6's 30 min), synthetic clock — 376 s wall |

**Several GPU cases render audible tones through the speakers.** `fc_audio_target` is a
real render client — SPEC.md §8.6's per-application capture is keyed on a process id, so
there is no way to exercise it without a process actually playing something. Expect noise
during `RealTargetTest.*` and row 15; it is the test working.

**After any change to `AudioTimeline`, run `FC_LOOPBACK_SECONDS=300` before you call it
done.** BUG-042's first attempt passed lint, the whole CPU tier, the whole GPU tier and a
40-second real-endpoint recording — and produced **76 seconds of audio in a 300-second
one**. Five minutes is the shortest run that failed; the routine suite's twelve seconds
cannot see it.

**If the clang-tidy database goes stale** after adding a source file, regenerate it
before lint will pass:

```powershell
cmake -B build/tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON --preset windows-msvc-debug
```

---

## 4. Code standards

**C++**
- C++20, MSVC. `/W4 /permissive- /WX` on `fc_core`. **No warning suppressions without a comment justifying it.**
- RAII for everything. `ComPtr` for COM, `unique_ptr` for owned heap, purpose-built wrappers for every FFmpeg object (`AVFormatContext`, `AVCodecContext`, `AVFrame`, `AVPacket`, `SwrContext`). **Zero raw `new`/`delete`. Zero raw owning pointers.**
- `Result<T, FcError>` for recoverable failures. Exceptions only for genuinely unrecoverable conditions, and they never cross a thread or IPC boundary.
- `clang-format` (LLVM base, 4-space, 120 col). Never hand-format.
- Every thread named via `SetThreadDescription`. A minidump with unnamed threads is a minidump you can't read.

**Banned patterns** (clang-tidy + CI grep enforce these — don't make CI catch you):
`catch(...)` outside thread entry · empty catch · `assert` as production error handling · `exit()` outside `main` · raw `new`/`delete` · unchecked `HRESULT` · `GetTickCount` / `system_clock` / `time()` for media timing · `timeBeginPeriod` · unbounded queues · per-frame logging at `INFO` or above

**Python**
- 3.11+, PySide6, `ruff` + `mypy --strict`. Type hints on everything.
- **Zero media processing in Python.** No numpy frame math, no PIL, no codec calls. The GUI wraps a `QImage` over the shared-memory preview buffer and nothing else. If you find yourself importing numpy in `gui/`, stop.

---

## 5. Where things go

```
engine/core/     fc_core static lib — ALL logic lives here, so it's testable
                 without spawning a process
engine/app/      main.cpp only. Thin. If you're adding logic here, it belongs in core/
gui/             PySide6. Control plane only.
tests/unit/      GoogleTest, pure functions, no hardware
tests/integration/  uses tests/tools/synthetic_source (deterministic D3D11
                    test pattern: frame-index barcode + SMPTE bars + 1 kHz tone)
tests/chaos/     fault injection
tests/soak/      long-run
docs/            SPEC.md + the deliverables in §22
```

**Tests use the synthetic source, never the real desktop.** Tests that depend on what happens to be on screen are not tests.

---

## 6. The verification boundary — read this twice

**You are running on the reference rig. Run the GPU tier yourself.**

```powershell
ctest --preset windows-msvc-release -L gpu --output-on-failure
```

The Radeon 780M and the RTX 4050 are both present, and so is a real WASAPI
endpoint. Everything in SPEC.md §4 (capture), §5 (GPU topology/migration), §6
(color pipeline) and §20 rows 1, 5, 6, 8, 11 is yours to verify, not to defer.

> This section previously said "you have no GPU, you cannot verify anything in the
> hardware tier." That was wrong, and it was expensive: through most of M4 the
> hardware tier was described as needing the owner while it was runnable the whole
> time, and BUG-019 — a QPC overflow that made the test source's clock nonsense on
> any machine up more than fifteen minutes — sat undetected until the tier was
> finally run. **A claimed limitation is worth one command to check.**

**What still cannot be verified here, and what to do about it:**

- **Anything needing a second machine or a different GPU pair.** Say so and stop.
- **Anything uptime-, thermal- or load-dependent.** BUG-019 only reproduced because
  the machine had been up thirteen hours. Name the condition in the test's comment
  when a defect depends on one — a freshly booted CI runner will not show it.
- **Long soaks.** SPEC.md §20.1's four-hour run is real time and belongs to M10, not
  to a routine suite. Where a test takes a duration, give it an environment
  override and record the measured numbers rather than running the long form every
  time.

**Report measurements, not adjectives.** "Green" is worth less than "worst offset
−187 µs against a 20 ms tolerance, 1800 marks". A number is what the next person
compares against when it changes.

Do not simulate hardware behavior to make a test pass, and do not weaken an
assertion because it failed on hardware — find out why first. A green test that
proves nothing is worse than a red one.

---

## 7. Working a milestone

Milestones are defined in SPEC.md §24. One milestone per session. For each:

1. Read the relevant spec sections. Restate the exit criterion before writing code.
2. Write the interface first, then the test, then the implementation.
3. Run lint and **both** test tiers.
4. Report: what's done, what's tested, **what's unverified and why**, what you'd do next.

**Don't scaffold ahead.** Do not create empty files for future milestones. Do not stub out HEVC/AV1 encoders, Linux backends, or streaming. Half-built structure is worse than no structure — it looks finished and isn't.

**If a spec section is ambiguous or looks wrong, stop and ask.** Guessing at a media-pipeline requirement produces bugs that only surface in a 4-hour soak test.

---

## 8. Project-specific gotchas

These are the traps. If you're touching one of these areas, re-read the cited section first.

| Working on… | Remember |
|---|---|
| **Any D3D device creation** | On a MUX-less laptop the display is wired to the **iGPU**, not the dGPU. The D3D device must be created on the adapter that owns the target `IDXGIOutput`, or DDA returns black with `S_OK`. **DXGI can succeed and hand you nothing.** §5.1 |
| **Encoder selection** | Encode where the pixels already live. Shipping frames to the "faster" RTX 4050 costs ~500 MB/s over PCIe for zero quality gain. §5.2 |
| **Any PTS math** | QPC is the only clock. Timebase `1/60000`. Duplicate frames are emitted **deliberately with quantized indices** — that's what prevents judder, and getting it wrong is the "repeated jerky frames" bug. §7.2 |
| **WASAPI loopback** | It emits **nothing** when no audio is playing. Without the silence generator you get 22 minutes of audio in a 30-minute file and everything after the first silence is desynced. §8.2 |
| **Multi-track audio** | N tracks = N independent device clocks = N drift loops, all reconciled to one QPC epoch. Tracks are silence-padded from `t0`, never offset. §8.6 |
| **MP4 output** | Write fragmented MP4 during recording, losslessly remux to progressive on clean stop. Progressive-during-recording means a crash yields an unplayable brick. §10.3 |
| **Color conversion** | Use `DXGI_FORMAT_B8G8R8A8_UNORM`, **not** `_UNORM_SRGB` — the SRGB view applies a second gamma decode and washes the output out. Color must be tagged in the encoder, the SPS VUI, **and** the container. §6 |
| **Anything touching the source format** | If WGC hands you `R16G16B16A16Float`, system HDR is on. Reinterpreting those bits as BGRA8 is the black/neon-green bug. Branch to tone-map. §4.2 |
| **Segment splitting** | Must be keyframe-aligned. Splitting mid-GOP makes the next file's opening seconds unplayable garbage. §11 |

---

## 9. Open decisions — do not guess

- ~~**libx264 is GPLv2.**~~ **Decided 2026-07-30 by the owner: libx264 is in.**
  `--enable-gpl` and `--enable-libx264` are set in
  `ports/ffmpeg/framecapture-whitelist.cmake`, and degradation-ladder rung 5
  (§13) is unblocked.

  **The consequence, stated once so it is not rediscovered:** the distributed
  binary is **GPLv2**. libx264 is GPLv2, `--enable-gpl` makes the whole FFmpeg
  build GPL, and linking that into FrameCapture puts FrameCapture under GPLv2 on
  distribution. That obliges source availability for the distributed work, and it
  is incompatible with shipping under a proprietary licence. Anything in §21's
  packaging and installer work that assumed otherwise needs revisiting — flag it
  rather than working around it.

  The alternative, had it gone the other way, was rung 5 simply not existing: a
  machine whose hardware encoders all fail would stop recording rather than fall
  back. That is the trade that was made.

  **Implemented in M6.** `libx264 superfast` with an NV12 readback through
  `av_hwframe_transfer_data`, verified by `SoftwareEncoderTest` (gpu) — which decodes
  the output and reads the frame-index barcode out of every frame, because a
  mis-strided readback produces a playable file that is green with the picture
  shifted, and nothing short of checking the content catches it.
- **SPEC.md §13 rungs 1 and 2 cannot be implemented through libavcodec, and §13 needs
  the owner's decision on how it should read.** Measured against the FFmpeg n8.1.2
  sources this build links, not inferred: `h264_nvenc` reconfigures only bitrate
  fields and only when rate control is *not* CONSTQP — and §9 makes CQP the default —
  while `h264_amf` sets every property once at init. Neither can change preset or QP
  mid-recording. `libx264` can change QP, so rung 2 is real on the software path only;
  rung 1 is available nowhere.

  **The consequence:** on hardware, degradation steps from nominal straight to rung 3's
  halved capture rate, with neither of §13's two soft rungs available to absorb a
  transient invisibly. The ladder still *computes* both decisions and logs the gap, so
  nothing is silent — see BUG-024 and `docs/ACCEPTANCE.md`. Reopening the encoder is not
  a workaround: it emits fresh SPS/PPS and the container's parameter sets were fixed by
  `avformat_write_header` (§10.1).
- **`h264_amf` zero-copy** is unverified. `test_amf_zero_copy` must pass on the reference rig before the AMF path is considered done. If it round-trips through system memory, only the AMF path moves to the direct AMD AMF SDK — NVENC stays on the FFmpeg wrapper. §2.2

---

## 10. Documentation is not optional

- **`docs/ENGINEERING_LOG.md` gets an entry for every non-trivial bug**, in the format shown in SPEC.md §22.1: symptom → investigation steps → root cause → fix → regression test → lesson. This is a graded deliverable, not a nicety.
- Update `CHANGELOG.md` (Keep a Changelog, semver) with every user-visible change.
- If you add a config key, it goes in `docs/CONFIG.md` in the same commit. If you add an `FcError`, it goes in `docs/ERROR_CODES.md` in the same commit.
- Commits follow Conventional Commits.
