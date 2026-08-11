# FrameCapture — Engineering Build Prompt (v1.0)

> **Role for the implementing agent:** You are a senior systems engineer building a production-grade, OBS-class screen recorder. You optimize for *correctness under adversarial hardware conditions* first, throughput second, and feature surface last. Every failure mode listed in §20 is a shipped-bug-class defect, not an edge case. No stubs. No `TODO: handle later`. No silent `catch(...) {}`.

---

## 0. Mission & Non-Goals

### 0.1 Mission
Build **FrameCapture**: a desktop screen recorder that captures **1080p SDR @ 60 fps (default, hard-capped) or 30 fps**, muxes to **`.mp4`** or **`.mkv`**, with **multi-channel AAC system audio**, and runs flawlessly on **hybrid-GPU laptops** where the active adapter changes at runtime.

Architecture: a **C++ core engine process** (all capture/encode/mux) + a **separate lightweight Python GUI process** (control plane only). The two communicate over a versioned IPC contract. Killing one must never corrupt the other's state or the output file.

### 0.2 Hard Non-Goals (do not implement, do not scaffold, do not leave hooks that imply future support)
| Excluded | Enforcement |
|---|---|
| Microphone / any input-endpoint capture | Engine must not link or call `eCapture` dataflow WASAPI paths. |
| Webcam / virtual camera / any video input device | No Media Foundation source reader for video devices. |
| Streaming (RTMP/SRT/WHIP/HLS) | No network egress from the engine process, ever. Engine binds no sockets except the localhost IPC endpoint. |
| Anti-cheat-triggering graphics hooking (`d3d11.dll` injection à la OBS game capture) | v1 uses OS-sanctioned capture APIs only. |
| Video segmentation **as a default** | Segmentation code path is dormant unless the user explicitly enables it in the GUI. Default config value is `enabled = false`. |

### 0.3 Non-Negotiable Quality Bars
- Zero black frames, zero color tint, zero duplicate-frame stutter in the acceptance suite (§20).
- A/V drift **< 20 ms** absolute over a **4-hour** continuous recording.
- Encoded frame count == expected frame count ± 0 in CFR mode.
- A hard-killed engine (`TerminateProcess`) leaves a **playable, seekable** file.
- Capture overhead **< 6%** of one CPU core and **< 4%** GPU at 1080p60 on the reference rig.

---

## 1. Target Platform & Reference Hardware

- **Primary target:** Windows 10 2004 (build 19041) and later; Windows 11 fully supported.
- **Reference rig (must be the CI/validation baseline):** AMD Ryzen 7 7840HS + **Radeon 780M iGPU** + **NVIDIA RTX 4050 Laptop dGPU**.
- Assume **MUX-less / Advanced Optimus** topology. Do **not** assume the display output is owned by the dGPU. Do **not** assume it is owned by the iGPU either — **detect it, every time, and re-detect on change.**
- Architect the platform layer behind interfaces (`IScreenCapture`, `IAudioCapture`, `IVideoEncoder`) so a future Linux (PipeWire + VAAPI/NVENC) backend is a new implementation, not a rewrite. **Do not implement Linux in v1.**

---

## 2. Technology Stack — Decisions & Rationale

> Items marked **[APPROVAL REQUIRED]** must be confirmed by the project owner before implementation begins.

### 2.1 Locked-in decisions

| Layer | Choice | Why this and not the alternative |
|---|---|---|
| Core engine language | **C++20** (MSVC 19.3x, `/std:c++20`) | Coroutines for the async capture loop, `std::span`, `<atomic>` wait/notify, designated initializers. C++23 modules are still fragile in MSVC + vcpkg; skip. |
| Primary capture API | **Windows.Graphics.Capture (WGC)** via C++/WinRT | Only API that reliably captures across hybrid-GPU boundaries, handles fullscreen-exclusive apps on modern builds, supports per-window and per-monitor, and survives display topology changes better than DDA. |
| Fallback capture API | **DXGI Desktop Duplication (DDA)** | Needed for Win10 builds < 19041 and as a degradation rung when WGC session creation fails. Its adapter-affinity constraint is exactly why it is the *fallback*, not the primary. |
| GPU pixel conversion | **D3D11 Compute Shader (BGRA → NV12)** | `libswscale` on CPU at 1080p60 burns ~1.5 cores and is the #1 cause of dropped frames in naive recorders. GPU CS is ~0.3 ms/frame and keeps the texture on-device for zero-copy encode. |
| Muxing / container | **libavformat (FFmpeg 8.1.x, LGPLv2.1 build)** | Battle-tested MP4 + Matroska writers, correct edit lists, correct MKV cues, correct color VUI tagging. Hand-rolling an MP4 muxer to "reduce dependencies" is how you ship corrupted files. Pin the exact patch version in `vcpkg.json`; build with `--disable-everything` + explicit whitelist, including **`--disable-network --disable-protocols`** so the "no network egress from the engine" requirement (§0.2) is enforced at compile time, not by code review. |
| CPU fallback encoder | **libx264** (via libavcodec) | 7840HS handles 1080p60 `veryfast`/`superfast` comfortably. Mandatory final rung of the degradation ladder. |
| AAC encoder | **FFmpeg native `aac` (LC profile)** | Quality at ≥160 kbps is transparent for system audio. **Explicitly rejecting `libfdk_aac`**: its license is GPL-incompatible and forces `--enable-nonfree`, making the binary legally non-redistributable. Not worth 2% quality. Media Foundation AAC is an optional runtime alternative, not the default. |
| Audio capture | **WASAPI loopback** (`eRender` + `AUDCLNT_STREAMFLAGS_LOOPBACK`), event-driven | Only sanctioned zero-driver system-audio path. Optionally **Process Loopback** (`VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, Win10 20H1+) for per-application tracks. |
| Sample rate conversion / drift compensation | **libswresample** with `swr_set_compensation()` | Needed for soft-resync; see §8.4. |
| Logging | **spdlog** (async, rotating file sink + in-memory ring sink) | Lock-free async queue means logging never stalls the capture thread. Ring sink is dumped on crash. |
| Config format | **TOML** via **toml++** | Human-editable, supports comments, unambiguous typing. JSON has no comments; YAML's spec is a footgun. Schema is versioned with a migration chain (§17). |
| C++ unit tests | **GoogleTest + GoogleMock** | Death tests, parameterized fixtures, mature MSVC support. |
| Python GUI framework | **PySide6 (Qt 6.6+)** | **LGPL** — no license contamination for a closed or permissive distribution, unlike PyQt6's GPL/commercial dual license. Native-feeling, QSS-themeable to an OBS-like dark palette, mature docking/layout for the OBS UX clone. |
| Build system | **CMake ≥ 3.25** + **vcpkg manifest mode** | Reproducible, lockfile-pinned (`vcpkg.json` + builtin-baseline), best-in-class MSVC/Windows story, has an FFmpeg port with granular feature flags so you build *only* the codecs you ship. |
| IPC transport | **Windows Named Pipe** (control) + **shared memory ring** (preview frames) | See §15. Never push 1080p frames through a pipe. |
| Control message encoding | **Length-prefixed JSON** (`nlohmann::json` C++ side, stdlib `json` Python side) | Human-debuggable, trivially versionable via additive fields = free backward compatibility. Protobuf/gRPC adds a codegen toolchain for a link that carries <1 KB/s. |
| Crash handling | `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` (+ optional Crashpad in v2) | Minidump + last-2000-log-lines ring dump on fault. |
| Installer | **Inno Setup** (or WiX v4 if MSI/GPO deployment is ever needed) | Clean per-user or per-machine install, real uninstall, upgrade-in-place. |
| Python packaging | **PyInstaller** (`--onedir`) | `--onedir` starts faster and is debuggable, unlike `--onefile`'s temp-extract. Nuitka is a v2 optimization, not a v1 risk. |

### 2.2 Ratified decisions (owner-approved — implement exactly as stated)

1. **Encoder integration: unified FFmpeg / libavcodec wrapper.** ✅ APPROVED
   `h264_nvenc` + `h264_amf` + `libx264` behind one `IVideoEncoder`, fed by an `AV_HWDEVICE_TYPE_D3D11VA` hardware frames context (`AV_PIX_FMT_D3D11`). One encode path, one packet type, one muxer interface — encoder swap on GPU migration is a factory call, not a rewrite. Accepted cost: ~1–3 ms latency vs. raw SDK.
   **Required guard rail:** `h264_amf`'s FFmpeg wrapper performs a host round-trip on some driver revisions. Add `test_amf_zero_copy` asserting no `av_hwframe_transfer_data` fires on the hot path, and instrument PCIe transfer time. If measured GPU overhead on the 780M exceeds 8%, escalate — *only* the AMF path drops to direct AMF SDK in v1.1. NVENC never leaves the FFmpeg wrapper.

2. **Platform scope: Windows-only, behind cross-platform interfaces.** ✅ LOCKED
   `IScreenCapture` / `IAudioCapture` / `IVideoEncoder` / `IMuxer` are platform-agnostic; every implementation in v1 is Win32/WinRT. No Linux code, no `#ifdef __linux__` stubs, no PipeWire scaffolding.

3. **Audio: multi-channel baseline + multi-track as an MKV-only advanced tier.** ✅ APPROVED — both
   - **Tier A (always on, guaranteed):** capture the render endpoint's native mix format up to **7.1**, encode as one AAC-LC stream with an explicitly signalled channel layout. Works in MP4 and MKV. This tier must be independently shippable and independently tested.
   - **Tier B (opt-in, MKV only):** up to **6** discrete audio tracks via **Process Loopback**. See §8.6 for the full contract. The GUI hard-disables Tier B whenever MP4 is the selected container, with an inline explanation — not a silent grey-out.

4. **Codec: H.264 (AVC) High @ L4.2 only in v1.** ✅ APPROVED
   Codec is a config enum (`h264 | hevc | av1`) from day one with a full encoder-capability probe per codec per adapter, but **only `h264` is selectable in the GUI and only `h264` is validated in v1**. HEVC and AV1 (both your 4050 and 780M encode AV1) slot in at v1.1 with zero refactoring once the H.264 acceptance suite is green.

---

## 3. Process Topology & Architecture

```
┌──────────────────────────────┐        ┌───────────────────────────────────────────┐
│  framecapture-gui.exe        │        │  framecapture-engine.exe                  │
│  (Python 3.11 / PySide6)     │        │  (C++20, native)                          │
│                              │        │                                           │
│  • Settings & scene UI       │◄──────►│  ┌─────────────┐  ┌──────────────┐        │
│  • Preview surface           │ Named  │  │ CaptureSvc  │─►│ ColorConvert │        │
│  • Recording controls        │ Pipe   │  │ (WGC/DDA)   │  │ (D3D11 CS)   │        │
│  • Stats/health readout      │ JSON   │  └─────────────┘  └──────┬───────┘        │
│  • Log viewer                │  RPC   │  ┌─────────────┐         ▼                │
│                              │        │  │ AudioSvc    │  ┌──────────────┐        │
│  Owns: config file,          │◄──────►│  │ (WASAPI LB) │  │ VideoEncoder │        │
│  process lifecycle,          │ SHM    │  └──────┬──────┘  └──────┬───────┘        │
│  update checks               │ ring   │         ▼                ▼                │
│                              │(preview│  ┌──────────────┐ ┌──────────────┐        │
│  Zero frame data in Python.  │ only)  │  │ AudioEncoder │ │  Muxer       │        │
└──────────────────────────────┘        │  └──────┬───────┘ │ (libavformat)│        │
                                        │         └────────►└──────────────┘        │
                                        │  ┌──────────────────────────────────┐     │
                                        │  │ ClockSvc • DeviceWatcher •       │     │
                                        │  │ HealthMonitor • Logger • Config  │     │
                                        │  └──────────────────────────────────┘     │
                                        └───────────────────────────────────────────┘
```

### 3.1 Process lifecycle rules
- GUI spawns the engine as a **child process inside a Windows Job Object** with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. An orphaned engine holding a capture session and an open file handle is unacceptable.
- **Bidirectional heartbeat**, 1 s interval, 5 s timeout.
  - GUI heartbeat lost → engine **finalizes the current recording cleanly**, then exits. It does **not** discard the file.
  - Engine heartbeat lost → GUI shows `ENGINE_UNRESPONSIVE`, offers "Recover last recording" (which invokes the repair path in §10.4).
- Engine registers a **console control handler** + a hidden message window handling `WM_QUERYENDSESSION` / `WM_ENDSESSION` so OS shutdown triggers a clean trailer write.
- Exactly **one engine instance per user session**, enforced by a named mutex.

---

## 4. Capture Subsystem

### 4.1 Source types (v1)
- **Display capture** — a specific monitor, by stable `DISPLAYCONFIG` path ID (not by index; indices reshuffle on hotplug).
- **Window capture** — a specific `HWND`, with process-name + window-class re-acquisition fallback if the HWND dies.

### 4.2 WGC path (primary)
- Initialize C++/WinRT in **MTA** on a dedicated capture thread. Do not touch the WinRT capture objects from any other thread.
- `Direct3D11CaptureFramePool.CreateFreeThreaded()` — **mandatory**. The non-free-threaded variant requires a `DispatcherQueue` and is a common source of "no frames ever arrive."
- Frame pool size: **3** buffers. Larger pools mask backpressure and increase latency; smaller pools drop frames on GPU hiccups.
- Subscribe to `FrameArrived`; in the handler do **only**: acquire frame → `QueryInterface` the `IDirect3DDxgiInterfaceAccess` → get `ID3D11Texture2D` → enqueue a ref-counted handle → return. **Never encode, convert, or log-to-disk inside `FrameArrived`.**
- Set `IsCursorCaptureEnabled` from config. On Win11 (build 22000+), set `IsBorderRequired = false` when available; probe via `ApiInformation::IsPropertyPresent` rather than OS-version checks.
- **HDR/WCG guard:** request `DirectXPixelFormat::B8G8R8A8UIntNormalized`. If the system returns `R16G16B16A16Float` (Windows HDR or Auto-HDR is on for the target), the compute shader **must** branch into an scRGB → BT.709 SDR tone-map path. Silently reinterpreting FP16 as BGRA8 is the classic "black or neon-green output" bug. Additionally, surface a GUI warning: *"System HDR is enabled; output will be tone-mapped to SDR."*

### 4.3 DDA path (fallback)
- **Critical constraint:** the `ID3D11Device` used for `IDXGIOutput1::DuplicateOutput` **must be created on the exact `IDXGIAdapter` that owns that `IDXGIOutput`.** Violating this returns `DXGI_ERROR_UNSUPPORTED` — and on hybrid laptops, it is the single most common cause of the "recording is entirely black" defect. Enumerate adapter → enumerate its outputs → match `DXGI_OUTPUT_DESC.Monitor` against the target `HMONITOR` → create the device on *that* adapter.
- Handle `DXGI_ERROR_WAIT_TIMEOUT` as *"no screen change"* → emit a **duplicate frame with a correctly advanced PTS**, not a stall and not a gap.
- Handle `DXGI_ERROR_ACCESS_LOST` → tear down and re-duplicate with exponential backoff (10 ms → 500 ms cap). This fires on UAC prompts, secure-desktop transitions, resolution changes, and **MUX switches**.

### 4.4 Capture invariants
- The capture thread runs at `THREAD_PRIORITY_ABOVE_NORMAL` and is registered with MMCSS (`AvSetMmThreadCharacteristics("Capture")`).
- Every captured texture carries an immutable `CaptureFrame { ComPtr<ID3D11Texture2D> tex; uint64_t qpc_ns; uint32_t seq; LUID adapter_luid; DXGI_FORMAT fmt; RECT content_rect; }`.
- Content-rect changes (window resize, resolution change) are **not** silently rescaled. They trigger the resize policy in §14.3.

---

## 5. GPU Detection & Encoder Selection

This subsystem is the project's differentiator. Build it as `GpuTopologyService`.

### 5.1 Discovery (on startup and on every topology-change event)
1. `CreateDXGIFactory1` → enumerate all `IDXGIAdapter1`. Record `LUID`, `VendorId`, `DeviceId`, `DedicatedVideoMemory`, `Flags`, and `DXGI_ADAPTER_FLAG3_*` where available.
2. Classify each adapter: **Integrated** (`DedicatedVideoMemory` < 512 MB and shared memory dominant, or `VendorId==0x1002` with an APU device family), **Discrete**, **Software** (`DXGI_ADAPTER_FLAG_SOFTWARE` → excluded from encode selection, permitted only as a last-ditch capture device).
3. For each adapter, enumerate `IDXGIOutput`s. **An adapter with zero outputs is not driving the display** — on MUX-less Optimus laptops this is typically the RTX 4050 in "render-only" mode. It is still a **valid encode target**.
4. Determine the **display-owning adapter** for the capture target: map target `HMONITOR` → `IDXGIOutput` → parent `IDXGIAdapter` → `LUID`.
5. Probe encode capability per adapter by **actually creating and immediately destroying a throwaway encoder session** at 1920×1080@60. Do not trust vendor ID heuristics or driver version tables — probe. Cache the result keyed by `(LUID, driver_version)` and invalidate on driver change.

### 5.2 Encoder selection policy (deterministic, in order)
```
1. If capture_adapter has a working HW encoder  → use it.  [zero-copy, no PCIe transfer]
2. Else if a discrete adapter has a working HW encoder AND
   cross-adapter transfer cost is measured < 2.0 ms/frame → use it. [see 5.3]
3. Else if the integrated adapter has a working HW encoder → use it.
4. Else → libx264 CPU. [preset from degradation ladder, §13]
```
**Rule 1 is the important one.** The intuitive but wrong instinct is "always encode on the RTX 4050 because it's the fast GPU." On a MUX-less laptop the display is wired to the 780M, so frames already live in 780M memory; shipping them to the 4050 costs a full 1080p BGRA round trip (≈8.3 MB × 60 fps ≈ 500 MB/s) over PCIe for **zero quality gain**. Encode where the pixels already are.

### 5.3 Cross-adapter transfer (only when Rule 2 fires)
- Preferred: `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED` on the source adapter, opened on the destination via `ID3D11Device1::OpenSharedResource1`, with a `IDXGIKeyedMutex` for synchronization.
- Cross-adapter shared resources have hard constraints: **no mips, no MSAA, `D3D11_USAGE_DEFAULT`, single-subresource, restricted formats.** Validate all of them at creation and fall back rather than crash.
- Fallback: staged CPU copy (`D3D11_USAGE_STAGING` + `Map`) with a **dedicated transfer thread** and triple-buffered staging textures so the capture thread never blocks on `Map`.
- Instrument this path with a per-frame timer. If P99 transfer time exceeds 2.0 ms, log a `PERF_DEGRADE` event and re-run selection.

### 5.4 Dynamic GPU change detection (runtime, mid-recording)
Watch **all** of these; any one may fire during a MUX switch or Optimus handoff:
- `IDXGIFactory1::IsCurrent() == false` → adapter set changed → re-enumerate, re-run §5.1.
- `WM_DISPLAYCHANGE`, `WM_DEVICECHANGE`, `WM_DPICHANGED` on the engine's hidden message window.
- `DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` from any D3D call → call `GetDeviceRemovedReason()`, **log the specific HRESULT** (`DXGI_ERROR_DEVICE_HUNG` vs `DRIVER_INTERNAL_ERROR` vs `ACCESS_LOST` are different bugs), rebuild the device stack.
- `DXGI_ERROR_ACCESS_LOST` from the duplication or capture session.
- A background poll at 2 Hz comparing the current adapter LUID set against the cached set, as a belt-and-braces net for events Windows fails to deliver.

**Mid-recording GPU migration procedure (must not drop the output file):**
```
1. Signal PAUSE_CAPTURE. Encoder queue continues draining.
2. Flush the video encoder (send NULL frame, collect trailing packets).
3. Tear down capture session + color-convert pipeline + D3D device.
4. Re-run GpuTopologyService discovery and encoder selection.
5. Rebuild D3D device, capture session, convert pipeline, encoder.
6. Force an IDR keyframe. Reuse the SAME muxer, SAME stream index, SAME timebase.
7. Emit duplicate frames covering the gap so the CFR timeline stays contiguous.
8. Resume. Log a GPU_MIGRATION event with before/after LUIDs and total gap in ms.
```
Target: **< 350 ms** total gap.

**Amended 2026-07-30 by the owner, after measurement.** This section previously required that "the output file remains a single continuous, seekable stream", and offered in-band SPS/PPS as a fallback when `extradata` changed. `MigrationParameterSetTest` (gpu) established on the reference rig that neither is available across vendors:

| Question | Measured |
|---|---|
| Do identical settings produce identical parameter sets? | **No.** `h264_amf` 28 bytes of SPS/PPS, `h264_nvenc` 53 |
| Do in-band parameter sets rescue one stream? | **No.** 31 of 60 frames decode; the decoder is initialised from the container's fixed `CodecPrivate`/`avcC` and a same-id SPS with different content does not reliably re-initialise it |
| Does a same-adapter reopen reproduce them? | **Yes**, byte for byte |

So the outcome now depends on where the migration lands, and both outcomes are correct:

- **Same adapter** — a driver restart, a `DEVICE_RESET`, or §20 row 9's stall recovery. Parameter sets are identical, so the recording continues in **one continuous, seekable file**. This is the common case and the one the 350 ms budget was written for.
- **Different adapter** — a genuine topology change. Parameter sets differ and the container's are already fixed, so the muxer **closes the current file and opens the next**, keyed by §11's segment naming, logs `GPU_MIGRATION` at `WARN` with both LUIDs and the reason, and surfaces it to the GUI. Both files must be independently playable and the boundary must be keyframe-aligned. Reusing the stream here is *not* permitted: it produces a file whose second half does not decode, which is worse than a visible split.

**Decode-order constraint (added with the same amendment).** The rebuilt encoder's first DTS must not precede the last DTS already written. §9 sets `max_b_frames = 2`, so DTS lags PTS by the reorder depth and a fresh encoder re-derives DTS from its own first PTS; a zero-gap handover is rejected by libavformat outright (`non monotonically increasing dts`, measured). The migration must therefore advance the PTS grid by at least the reorder depth before the new encoder's first frame. The 350 ms budget is ~21 frames at 60 fps against a reorder depth of 3, so this is satisfied comfortably in practice — but it must be enforced rather than left to the budget.

---

## 6. Color Pipeline (the tint/washed-out killer)

Color bugs are silent and ship. Lock every stage.

| Stage | Requirement |
|---|---|
| Source | WGC/DDA yields **BGRA8, full range, sRGB-ish, straight alpha**. Never bind an `_SRGB`-typed SRV over it — that applies a second gamma decode and produces washed-out output. Use `DXGI_FORMAT_B8G8R8A8_UNORM`, not `_UNORM_SRGB`. |
| Conversion | Compute shader BGRA → **NV12**, matrix **BT.709**, range **limited (16–235 luma / 16–240 chroma)** by default; full-range is a config option. Chroma downsample must use a **box/average filter over the 2×2 luma quad**, not point sampling — point sampling produces visible chroma fringing on text. |
| Channel order | Assert at compile time and in a unit test that B/G/R are read in the correct order. A single unit test rendering pure `#FF0000` and asserting decoded output is red (not blue) catches the entire class of channel-swap bugs. |
| Encoder | Set `AVCodecContext`: `colorspace = AVCOL_SPC_BT709`, `color_primaries = AVCOL_PRI_BT709`, `color_trc = AVCOL_TRC_BT709`, `color_range = AVCOL_RANGE_MPEG`. |
| Bitstream | These **must** land in the H.264 SPS VUI. Verify with `ffprobe -show_streams` in an automated test. |
| Container | MP4: `colr` box (`nclx`, 1/1/1). MKV: `Colour` element with matching primaries/transfer/matrix and `Range`. Assert both in the acceptance suite. |
| HDR source | If the source surface is FP16 scRGB, run the tone-map branch (§4.2). Never reinterpret bits. |

---

## 7. Frame Pacing & Timestamping (the stutter/jerk killer)

### 7.1 Master clock
- **`QueryPerformanceCounter` is the single source of truth** for the entire engine. Convert to nanoseconds once, in `ClockSvc`. Never use `GetTickCount`, `std::chrono::system_clock`, or `time()` for media timing.
- `t0` = QPC at the moment the first video frame *and* first audio packet are both available. All PTS are relative to `t0`. Video and audio share **one** epoch.
- Never call `timeBeginPeriod`. It is a global system side effect and modern Windows already grants 1 ms granularity to MMCSS threads.

### 7.2 CFR frame pacer (default mode)
- Video timebase: **`1/60000`** (or `1/30000` at 30 fps) — clean integer frame durations of 1000, no rounding drift over hours.
- Algorithm:
  ```
  expected_index = round((frame.qpc_ns - t0_ns) * fps / 1e9)
  if expected_index <= last_emitted_index:   drop  (source outran the cap)
  if expected_index >  last_emitted_index+1: emit (expected_index - last_emitted_index - 1)
                                             duplicates of the last frame, then emit this one
  pts = expected_index * (timebase_den / fps)
  ```
- **Duplicates are emitted deliberately with correct PTS.** This is what makes a 12 fps source look like smooth-but-low-fps video instead of a jerky, timestamp-scrambled mess. It is also why the "repeated jerky frames" bug happens when you *don't* do it: naive implementations emit the duplicate with the *wrong* PTS, and the player judders.
- Hard cap: never emit more than `fps` frames per second of wall clock, even if the source produces 240.

### 7.3 VFR mode (opt-in)
- Timebase `1/1000000` (µs). PTS = exact QPC delta. **Enforce strictly monotonic PTS** — a single non-monotonic PTS makes `libavformat` reject the packet and can corrupt the MP4 `stts` table.
- MP4 + VFR is a known-fragile combination. When the user selects VFR, the GUI must recommend MKV.

### 7.4 Tearing
- Tearing is a *composition* artifact, not an encoding one. WGC frames are composited by DWM and are inherently tear-free. If tearing appears in output, the root cause is almost always DDA capturing a fullscreen-exclusive swapchain mid-flip. **Mitigation:** prefer WGC; when DDA is active on a fullscreen-exclusive target, log `TEARING_RISK` and offer the user a one-click switch to WGC.

### 7.5 Pause / resume

`pause_record` and `resume_record` have been in §15.1's command list since the first draft with no semantics attached. This section supplies them. **Added 2026-07-30; scheduled in M8 (§24).**

**The user-visible contract:** pause excises time. One file, and the paused span simply is not in it — frame *N* before the pause and frame *N+1* after it are adjacent, both in presentation order and on screen. That is what distinguishes pause from stop/start, and anything else (a frozen frame held for the pause duration, or a new file per pause) is a different feature wearing the name.

**The consequence, which is the whole of the difficulty.** §7.1 makes `t0` the single shared epoch and every PTS a function of `qpc - t0`. Excising time breaks that mapping: after a 10-second pause, a frame captured 30 s of wall clock after `t0` belongs at 20 s in the file. So the timeline's origin is no longer a constant, and:

```
timeline_ns = qpc_ns - t0_ns - paused_total_ns
```

`paused_total_ns` accumulates across every pause and is **shared by video and audio, exactly as `t0` is.** Two streams subtracting different paused totals desync permanently and invisibly — the same failure §7.1's shared epoch exists to prevent, arriving by a new route. It belongs alongside `t0` in the session's clock, consulted by `timing::Pacer` and `audio::AudioTimeline` alike, and it is **never** maintained independently by either.

**Required behaviour per subsystem:**

| Subsystem | On pause | On resume |
|---|---|---|
| Capture | Keep the session open, stop submitting frames. Rebuilding the capture session per pause would make a hotkey cost §5.4's 350 ms budget. | Resume submitting. |
| CFR pacer (§7.2) | Stop advancing. The pause must **not** be filled with duplicates — that is the frozen-frame outcome, not pause. | Force an IDR: content has jumped and a P-frame referencing pre-pause content is a visible smear. |
| Audio timeline (§8.2) | Stop accepting buffers, **and stop the silence generator**. Injecting silence for the paused span lengthens the timeline by exactly what the pause removed. | Resume. Reconcile against device position as §8.2 already requires after a gap. |
| Muxer (§10) | Stays open. No trailer, no finalize, no new file. | No header rewrite — same stream, same timebase, same parameter sets. |
| Encoder | Stays open. Do **not** rebuild: the parameter sets are what §5.4's amendment showed cannot be changed mid-file. | The forced IDR is a `forced-idr` request, not a reopen. |
| Health monitor (§13) | Suspend the row 9 stall detector and the frame-drop ratio. A paused recording has no frames by design and must not read as a stall. | Resume evaluation. |
| Segmentation (§11) | A pause does **not** close a segment. Duration-based splits measure timeline time, not wall clock. | — |

**Invariants:**
- Pause and resume are idempotent: pausing a paused recording is a no-op that succeeds, not an error.
- A recording paused when `stop_record` arrives finalizes normally and yields a valid file. A crash while paused is no different from any other crash (§10.3).
- `Pacer::timeline_seconds()` and the §10.4 validation gate need no special case: both derive from emitted PTS, which already excludes paused time.
- Total paused duration is reported in `get_stats` and logged at finalization, because "why is my 30-minute recording 12 minutes long" must be answerable from the log.

**Test:** §20 row 18.

---

## 8. Audio Subsystem

### 8.1 Capture
- WASAPI **loopback** on the default `eRender` / `eConsole` endpoint (user-selectable device).
- **Event-driven mode** (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`) with a 20 ms buffer. Polling mode is a drift generator.
- Register the audio thread with MMCSS as **`"Pro Audio"`** and set `AvSetMmThreadPriority(AVRT_PRIORITY_CRITICAL)`.
- Negotiate the endpoint's native `WAVEFORMATEXTENSIBLE` (typically 48 kHz, 32-bit float, 2ch — but handle 5.1/7.1 and 44.1/96 kHz). Convert to the internal canonical format: **48 kHz, planar float, native channel layout**.

### 8.2 The silence problem (the #1 loopback sync bug)
WASAPI loopback **emits no packets when no application is playing audio.** A naive implementation therefore records a 30-minute video with 22 minutes of audio, and everything after the first silence is desynced.

**Required behavior:** an independent `SilenceGenerator` timer thread monitors the last packet's QPC timestamp. If `now - last_packet_qpc > 2 * buffer_period`, it injects silence buffers of exactly the missing duration into the audio timeline. On resume, reconcile against the real device position and adjust the injected tail.

Also handle `AUDCLNT_BUFFERFLAGS_SILENT` (fill zeros, do not skip) and `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` (log it, insert silence to bridge the gap, **never** just concatenate — concatenation is exactly how drift accumulates).

### 8.3 Timestamping
- Use the `u64QPCPosition` returned by `IAudioCaptureClient::GetBuffer` as the authoritative packet timestamp. Do **not** derive audio PTS from a running sample counter alone — that assumes the device clock is exactly 48000.000 Hz, which it is not.
- Cross-check against `IAudioClock2::GetDevicePosition` and log the delta at 1 Hz as a drift telemetry signal.

### 8.4 Drift compensation
- Compute `drift = (audio_samples_written / sample_rate) - (qpc_elapsed_seconds)` on a 1 s cadence.
- `|drift| < 5 ms` → do nothing.
- `5 ms ≤ |drift| < 40 ms` → **soft resync** via `swr_set_compensation()`, correcting over ~10 s. This is inaudible.
- `|drift| ≥ 40 ms` → **hard resync**: insert or drop samples at a zero crossing, log `AUDIO_HARD_RESYNC` with the magnitude. This should essentially never fire; if it does, it is a bug report, not a normal event.

**Ratified M4: §8.2 is the correction, §8.4 is the measurement.** §8.2 and §8.4 each
describe a complete answer to the device-clock problem, and running both means
correcting the same error twice. The resolution is:

- **§8.2's timeline is authoritative and does the correcting.** Every packet lands
  at the frame index its QPC timestamp maps to, gaps are filled with silence of
  exactly the missing duration, and a packet overlapping ground already written is
  trimmed. A wrong device clock is absorbed there.
- **§8.4's ladder measures the result and does not act on the device.**
  `audio_samples_written` above means **samples handed to the encoder** — the
  encoded track's length — not samples the device delivered. On a healthy recording
  that sits near zero and the ladder correctly stays in band 0; the bands exist to
  notice §8.2 failing, and `swr_set_compensation` acts on the same quantity the
  measurement reads, so a correction converges instead of being re-requested every
  second.
- **Measuring the device's own clock instead is a different quantity** that grows
  without bound by design and must not drive corrections. It is §8.3's telemetry
  signal and is reported separately.

Measured on the reference rig, 30-minute real-time loopback recording: the
endpoint's crystal ran **14.4 ppm fast and gained 26 ms**, while the encoded track
stayed **38 ms** from the video track — which is the stop sequence's tail, not
drift. Driving §8.4's corrections from the device clock would have had it chasing
26 ms that never reached the file, and crossing the hard-resync threshold on
healthy hardware.

**Known deviation:** the hard band's insert-or-drop happens at a WASAPI buffer
boundary, not at a zero crossing. Aligning to a zero crossing needs a lookahead
window the timeline does not keep. Revisit if the band is ever seen to fire.

### 8.5 Encoding & Tier A (multi-channel, always on)
- AAC-LC, 48 kHz, default **192 kbps stereo** / **384 kbps 5.1** / **512 kbps 7.1**. Configurable.
- Channel layout must be explicitly signalled (`AV_CH_LAYOUT_STEREO` / `5POINT1_BACK` / `7POINT1`) in the `AVCodecContext`, in the AAC `AudioSpecificConfig`, **and** in the container (MP4 `chnl`/`esds`, MKV `Channels` + `ChannelPositions`). All three are asserted in `test_channel_layout`.
- **`5POINT1_BACK`, not `5POINT1` — corrected M4 (BUG-017).** AAC's standard channel
  configurations are a fixed list, and configuration 6 is `FL FR FC LFE BL BR`,
  with the surround pair at the **back**. FFmpeg's `AV_CH_LAYOUT_5POINT1` puts it at
  the **sides**, which is not on that list: the encoder falls back to a Program
  Config Element and the file reopens with `AV_CHANNEL_ORDER_UNSPEC` and a zero
  mask — six channels and no record of which is which, which is §20 row 17's defect
  reached without the muxer doing anything wrong. Windows offers both forms
  (`KSAUDIO_SPEAKER_5POINT1` is the back one, `KSAUDIO_SPEAKER_5POINT1_SURROUND` the
  side one), so a side-channel mask arriving from an endpoint is **normalised** to
  the back form. Relabelling one pair of speakers costs nothing a listener can hear;
  a file that labels nothing costs the whole feature.
- 7.1 is reachable only on an endpoint that actually supplies eight channels (see the clamp above), and is left as `AV_CH_LAYOUT_7POINT1`. FFmpeg encodes it as configuration 7 with a
  documented non-compliant interpretation and reads it back the same way, and the
  container mask survives intact. **Interop caveat:** a strict decoder reads
  configuration 7 as 7.1(wide) and would place channels 6 and 7 at `FLC`/`FRC`
  rather than `SL`/`SR`. Windows produces no layout that AAC can signal exactly, so
  this is a choice between FFmpeg-consistent and PCE-coded; revisit if a real player
  is found to mis-map it.
- **The pin overrides the endpoint downward only — amended 2026-08-06 (BUG-048).** An explicit layout may ask for *fewer* channels than the endpoint supplies and `libswresample` down-mixes into it; that is what the setting is for, and it is how a user records stereo from a 7.1 endpoint without touching Windows' settings. It may **not** ask for more: a request wider than the endpoint falls back to the endpoint's own layout, logged at WARN with both counts. Up-mixing cannot add information, and it is not harmless — a 7.1 pin on the stereo endpoint most machines have produced an eight-channel AAC track at 512 kbps instead of 192 whose audio **Windows' own player refuses**, because the Media Foundation AAC decoder accepts 1, 2 and 6 channels and not 8. The GUI states the consequence inline when the choice exceeds the device (§16.4); the engine enforces it regardless, so a headless caller cannot walk past it.
- Channel count is **pinned for the lifetime of the stream**. If the endpoint's mix format changes mid-recording (7.1 → stereo on a device switch), `libswresample` up/down-mixes into the pinned layout. Changing an AAC stream's channel count mid-file is invalid in both containers — see §14.1.
- **"Per stream", not "per file" — amended 2026-08-06.** This read "for the lifetime of the file" until Tier B, when a file could hold only one audio stream and the two phrasings meant the same thing. §8.6 supplies the per-application format at stereo whatever the recording's own layout is, so a 5.1 recording with Tier B on writes a 5.1 system mix beside stereo application tracks: one file, two pinned layouts, neither of which may drift. Up-mixing an application track to the recording's layout would triple its bitrate to carry information a stereo capture does not contain. The validation gate (§10.4) checks both expectations separately, because a single one could not tell "correctly different" from "wrong".
- Tier A must be shippable and green **independently of Tier B**. Milestone M4 gates on Tier A alone.

### 8.6 Tier B — per-application multi-track (opt-in, MKV only)

**Mechanism:** `ActivateAudioInterfaceAsync` with `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` (`AUDIOCLIENT_ACTIVATION_PARAMS` + `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`), targeting a PID with `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`. Requires Windows 10 build 19041+ — which is already our floor (§1), so no additional version gating is needed, but **probe at runtime anyway** and degrade to Tier A with a GUI notice if activation fails.

**Non-obvious constraints — all of these are shipped-bug sources:**
- Process Loopback does **not** negotiate a mix format. You must supply the `WAVEFORMATEX` yourself. Supply **48 kHz, 32-bit float, stereo** and let `libswresample` handle everything downstream. Requesting the endpoint's native format here silently fails on some configurations.
- It is **initialize-once**: the target PID cannot be changed on a live client. A process exiting means tearing down that track's client, not reconfiguring it.
- Each track carries its **own independent device clock**. Every track therefore needs its own `SilenceGenerator` (§8.2) and its own drift compensator (§8.4), all reconciled against the single QPC master clock (§7.1). N tracks = N drift loops, not one shared one. This is the real engineering cost of Tier B and the reason it is opt-in.
- A per-app track is silent far more often than the system mix. Silence injection is not an edge case here — it is the steady state. Test it as the primary path.

**Track policy:**
- Max **6** tracks. Track 0 is **always** the full system mix (Tier A output) so the file is useful even in a player that exposes only the first track. Tracks 1–5 are per-application.
- Every track gets a human-readable Matroska `Name` tag (`"System Mix"`, `"chrome.exe"`, `"game.exe"`). Untagged tracks are a UX failure.
- All tracks share one timebase, one epoch (`t0`), and identical duration. A track that starts late is **silence-padded from `t0`**, never offset. Ragged track start times are the classic multi-track desync bug.
- **Process lifecycle:** target process exits mid-recording → that track continues as silence. Never truncate a track; never drop it from the container. A target process that hasn't started yet → its track is silence until the PID resolves (poll by executable name at 2 Hz).
- **A target that is closed and reopened is picked back up — decided 2026-08-06.** This section previously read "continues as *pure* silence to the end of the file", which left open whether the track kept looking. It keeps looking, and it follows the **executable**, which is already the identity this section uses for a target that has not started yet. A browser that crashes and is reopened lands back on its own track with the gap silence-filled. The track's length is identical under either reading — its generator never stops — so what this decides is only whether a user who restarts an application gets the rest of its audio or silence. `audio.multitrack_reattach` (default `true`) exposes the literal reading for anyone who wants it. A track pinned to a **PID** rather than a name never re-attaches: there is nothing to resolve, and a different process wearing that number later is a different application.
- **MP4 is a hard block, not a soft warning.** If the container is MP4, Tier B is unavailable in the GUI with an inline reason string, and the engine rejects a `configure` command that requests both with `FcError::MULTITRACK_REQUIRES_MKV` (3021).

**Tests:** `test_multitrack_alignment` (all tracks identical duration, per-track 1 kHz tones at distinct frequencies, assert per-track sync < 20 ms over 30 min); `test_multitrack_process_exit` (kill a target mid-record, assert the track is silence-padded to full duration and the file stays valid); `test_multitrack_mp4_rejected` (assert the config is refused with the correct error code).

---

## 9. Video Encoding

- **Codec scope (v1): H.264 / AVC High Profile @ Level 4.2 only.** The `VideoCodec` config enum defines `h264 | hevc | av1` and the capability probe (§5.1) tests all three per adapter and reports results, but the GUI exposes only `h264` and only `h264` is in the acceptance matrix. Any code path that would branch on codec must exist and be exercised by a unit test with a fake encoder, so that enabling HEVC/AV1 in v1.1 is a config change plus a validation pass — not a refactor.
- **Encoder abstraction:** one `IVideoEncoder` over `h264_nvenc` / `h264_amf` / `libx264` via libavcodec (§2.2 item 1). No direct vendor SDK calls anywhere in `fc_core`. `test_amf_zero_copy` asserts the AMF path never round-trips to system memory.
- **Rate control:** CBR is wrong for local recording (wastes bits on static screens). Default to **CQP/CQ 20** for NVENC (`-rc constqp -qp 20`) and **quality preset with a bitrate ceiling** for AMF. Expose `Quality (CQP) | Bitrate (VBR) | Lossless` in the GUI.
- **Preset:** NVENC `p5` (`preset=slow`, `tune=hq`) is the sweet spot on Ada; `p7` costs latency for negligible gain at 1080p. AMF `quality` preset.
- **GOP:** keyframe interval **2 s** (120 frames @ 60 fps). Not 250, not "auto" — 2 s gives fast seeking and clean segment boundaries. B-frames: 2 (NVENC), 0 if any latency issue appears. **No open-GOP** — open-GOP breaks segment splitting.
- **Profile/Level:** H.264 High @ 4.2.
- Encoder input is **NV12 in a `AV_PIX_FMT_D3D11` hardware frames context** — zero copy. Verify with an assertion that no `av_hwframe_transfer_data` to system memory occurs on the hot path.
- **Bounded encoder input queue** (capacity 8 frames). When full, apply the degradation ladder (§13); do **not** block the capture thread and do **not** grow the queue unbounded (unbounded queues turn a transient hitch into an OOM).

---

## 10. Muxing & Container Strategy (the corruption killer)

### 10.1 Single-writer discipline
Exactly **one** mux thread owns the `AVFormatContext`. It consumes from a bounded, **DTS-ordered** priority queue fed by both encoders. Use `av_interleaved_write_frame` and let libavformat handle interleaving depth; do not hand-roll it.

### 10.2 MKV (recommended default)
Matroska's cluster structure means a truncated file is still playable up to the last complete cluster. Set `cluster_time_limit=2000`, `cluster_size_limit` sane, `write_crc32=0` (saves CPU). Write `Cues` on finalize.

### 10.3 MP4 (crash-safe recipe)
Progressive MP4 keeps the `moov` atom at the end; a crash mid-recording yields an **unplayable file with zero recoverable content.** This is the "corrupted file" defect in the requirements. The fix:

```
DURING RECORDING:  write fragmented MP4
                   movflags = frag_keyframe + empty_moov + default_base_moof
                   → every 2 s keyframe closes a self-contained fragment
                   → a hard kill leaves a fully playable file

ON CLEAN STOP:     losslessly remux fMP4 → progressive MP4 with faststart
                   (stream copy only, no re-encode, ~2 s for a 1 h file)
                   verify the output with a decode-probe before deleting the source

ON CRASH RECOVERY: the fMP4 is already valid — offer "Repair & finalize"
                   in the GUI, which runs the same remux.
```

### 10.4 Finalization & recovery
- `stop()` sequence: stop capture → flush video encoder → flush audio encoder → drain mux queue → `av_write_trailer` → `fclose` → `fsync` equivalent (`FlushFileBuffers`) → remux if MP4 → validate → notify GUI.
- Every stage of finalization is idempotent and separately logged. If the process dies mid-finalize, a `.fcrecover` sidecar (written at recording start, containing container type, codec params, and expected output path) lets the GUI's recovery path complete the job on next launch.
- **Validate before declaring success:** open the output with libavformat, confirm stream count, duration within 1% of expected, and that the first and last frames decode.

---

## 11. Video Segmentation (opt-in only)

- **Default `enabled = false`.** Never split unless the user explicitly ticks it in the GUI.
- Triggers: by duration (minutes) or by size (MB). Both may be armed; whichever fires first wins.
- Splits must be **keyframe-aligned**: request an IDR from the encoder, wait for it, close the current file at the packet *before* it, open the next file starting *at* it. Splitting mid-GOP produces a segment whose first seconds are unplayable garbage.
- Timestamps restart at 0 in each segment (each file is independently valid). Write a sidecar `<basename>.segments.json` recording each segment's global start offset so external tools can concatenate.
- Naming: `<basename>_part001.mkv`, zero-padded to 3, auto-widening past 999.
- Segment rollover must not drop a single frame. The next file's `AVFormatContext` is opened and its header written **before** the current one is closed.

---

## 12. Threading Model & Queues

| Thread | Priority | Responsibility | Blocking allowed? |
|---|---|---|---|
| `capture` | MMCSS `Capture`, ABOVE_NORMAL | WGC callback drain / DDA acquire loop | **No.** Never blocks on downstream. |
| `convert` | NORMAL | D3D11 CS dispatch, BGRA→NV12 | No |
| `venc` | NORMAL | Video encode submit + packet drain | No |
| `audio` | MMCSS `Pro Audio`, `AVRT_PRIORITY_CRITICAL` | WASAPI event loop | **No.** |
| `silence` | NORMAL | Silence injection watchdog (track 0) | Yes (timed wait) |
| `aenc` | NORMAL | AAC encode (track 0) | No |
| `preview` | BELOW_NORMAL | §15.2 readback + publish to the shared-memory ring | Yes |
| `atracks` | NORMAL | §8.6 Tier B: 2 Hz target poll, attach/detach of process-loopback clients | Yes (activation, teardown) |
| `asil`N | NORMAL | §8.6 Tier B: silence injection watchdog, **one per per-application track** | Yes (timed wait) |
| `ploop`N | MMCSS `Pro Audio`, `AVRT_PRIORITY_CRITICAL` | §8.6 Tier B: one process-loopback capture loop per attached target | **No.** |
| `aenc`N | NORMAL | §8.6 Tier B: AAC encode, one per per-application track | No |
| `mux` | BELOW_NORMAL | Sole `AVFormatContext` owner, disk I/O | Yes (disk) |
| `xfer` | NORMAL | Cross-adapter staging (only if §5.3 active) | No |
| `watchdog` | LOW | Device/topology polling, health metrics, heartbeat | Yes |
| `ipc` | LOW | Named-pipe RPC | Yes |

**Rules:**
- Inter-stage transport is **bounded lock-free SPSC ring buffers**. Bounded is not a limitation — it is the mechanism that makes backpressure observable and degradation deliberate.
- Explicit, documented **drop policy per queue** (video: drop oldest; audio: **never drop** — audio drops are audible and desync the stream, so on audio queue pressure, degrade video instead).
- No mutex is ever held across a D3D call or a disk write.
- All threads are named via `SetThreadDescription` so they are legible in a debugger and in a minidump.
- Every thread has an ownership-documented shutdown path with a bounded join timeout (2 s), after which the shutdown is escalated and logged — never an unbounded `join()`.

**Tier B's thread cost (added 2026-08-06).** The five rows above marked §8.6 are the
"real engineering cost of Tier B" §8.6 refers to. At its six-track maximum a recording
carries **eleven threads a Tier A recording does not have**: one `atracks`, and five each
of `asil`, `ploop` and `aenc`. Two consequences are stated rather than left to be
rediscovered:

- **`asil` is per track and not one shared ticker**, because `request_silence` pushes
  onto the track's own bounded queue under this section's `Block` policy for audio. A
  track whose `aenc` has wedged stops accepting ticks once that queue fills, and on a
  shared ticker that blocks *every other track's* tick behind it — six timelines stop
  advancing because one application's encoder is stuck, which is §8.2's defect reached
  through a coupling §8.2 does not mention. §8.6's "every track needs its own
  `SilenceGenerator`" is read literally for that reason, not only for that wording.
- **`atracks` is separate from `asil` for the same reason.** A poll pass blocks: an
  activation has a five-second timeout and a teardown joins a capture thread. Generators
  waiting behind it would stop advancing their tracks for exactly that long.

---

## 13. Graceful Degradation Ladder

`HealthMonitor` evaluates a rolling 3-second window. Degradation steps are **automatic, logged, and surfaced in the GUI status bar**. Recovery is hysteretic: only step back up after 30 s of clean operation, to prevent oscillation.

| Rung | Trigger | Action |
|---|---|---|
| 0 | Nominal | — |
| 1 | Encoder queue > 60% for 3 s | Lower encoder preset one step (`p5` → `p4`) |
| 2 | Encoder queue > 80% | Drop video quality target (CQP 20 → 24) |
| 3 | Sustained frame drops > 5% | Halve capture rate to 30 fps, keep the CFR timeline intact via duplicates |
| 4 | HW encoder init failure or `DEVICE_REMOVED` ×3 in 60 s | Migrate to the alternate GPU (§5.4) |
| 5 | All HW encoders unavailable | Fall back to `libx264 superfast` |
| 6 | Disk write latency P99 > 500 ms or free space < 2 GB | Warn user; at < 500 MB free, stop and finalize cleanly |
| 7 | Unrecoverable capture failure | Stop capture, **finalize the file successfully**, report the exact cause |

**Rung 7 is the point of the whole ladder: the recording never dies with an unusable file.**

---

## 14. Dynamic Source Change Handling

### 14.1 Audio device change mid-recording
- Implement `IMMNotificationClient` (`OnDefaultDeviceChanged`, `OnDeviceStateChanged`, `OnDeviceRemoved`).
- Procedure: detect → inject silence covering the transition → open the new endpoint → if the new format differs (rate/channels), **keep the encoder's output format constant** and adapt via `libswresample` (changing an AAC stream's sample rate or channel count mid-file is invalid in both MP4 and MKV) → resume → log `AUDIO_DEVICE_MIGRATION`. Target gap: **< 200 ms**, fully silence-filled so the timeline never shortens.

### 14.2 Capture target disappears
- Window closed → attempt re-acquisition by `(process name, window class, title regex)` for a configurable grace period (default 5 s), emitting duplicate/black frames meanwhile. On timeout, degrade to full-display capture (config-gated) or stop cleanly.
- Monitor unplugged → migrate to the primary display or stop cleanly. Never crash on a `nullptr` output.

### 14.3 Resolution / refresh-rate change mid-recording
- Output resolution is **pinned at 1920×1080 for the entire file.** Changing the encoded resolution mid-stream is invalid in MP4 and hostile in MKV.
- A source resolution change triggers a scaler reconfiguration (GPU shader), letterboxing/pillarboxing to preserve aspect ratio, filling bars with pure black (`Y=16` in limited range, not `Y=0`).
- Refresh-rate change does not alter the encoded frame rate — the CFR pacer absorbs it.

---

## 15. IPC Contract (GUI ↔ Engine)

### 15.1 Control channel
- **Named pipe:** `\\.\pipe\framecapture-{session_guid}`, message mode, ACL restricted to the current user SID.
- **Framing:** 4-byte little-endian length prefix + UTF-8 JSON body. Max message 64 KB.
- **Handshake:** GUI sends `{"cmd":"hello","proto":"1.0","client":"gui/1.0.0"}`; engine replies with its proto version + a `capabilities` array. **Backward compatibility rule: unknown fields are ignored, never fatal; new features are additive capability flags; the major proto version bumps only on a breaking change, and the engine must support the previous major for one release cycle.**
- **Commands:** `hello`, `get_sources`, `get_devices`, `get_gpu_topology`, `configure`, `start_preview`, `stop_preview`, `start_record`, `stop_record`, `pause_record`, `resume_record`, `get_stats`, `get_health`, `set_log_level`, `recover`, `shutdown`.
- `pause_record` / `resume_record` semantics are **§7.5**, not obvious from the names: paused wall-clock time is excised from the file, and both are idempotent (pausing a paused recording succeeds). `get_stats` reports `paused_total_ms` alongside the timeline elapsed.
- **Events (engine → GUI, unsolicited):** `state_changed`, `stats` (2 Hz), `warning`, `error`, `gpu_migrated`, `audio_device_migrated`, `degradation_changed`, `segment_rolled`, `recording_finalized`. Paused is a `state_changed` value, not an event of its own — the GUI must render it as a distinct state (§16.5), because a paused recording that looks like a running one loses footage silently.
- Every command carries an `id`; every response echoes it. Requests time out at 5 s (except `stop_record`, 30 s, since finalization is legitimately slow).

### 15.2 Preview channel
- Named **shared memory** (`CreateFileMapping`) triple-buffered ring, one `HANDLE`-passed section, with an atomic write-index header.
- Engine writes a **downscaled preview** (default 960×540, 30 fps, BGRA) — **never** full-resolution frames. The preview path must be independently droppable: if the GUI is slow, preview frames are skipped with **zero effect on the recording**.
- Preview generation is a separate GPU shader dispatch off the same source texture, adding < 0.2 ms/frame.

---

## 16. Python GUI Specification

### 16.1 Constraints
- **Zero media processing in Python.** No numpy frame math, no PIL, no codec calls. Python renders a `QImage` wrapped over the shared-memory preview buffer and nothing more.
- GUI process idle CPU **< 1%**; recording-active CPU **< 3%**.
- The GUI must be fully functional and honest when the engine is down: greyed controls + a clear `ENGINE OFFLINE` state, plus a restart action.

### 16.2 Layout (OBS-inspired)
```
┌────────────────────────────────────────────────────────────────┐
│ File   Edit   View   Tools   Help                    ─  □  ×   │
├────────────────────────────────────────────────────────────────┤
│                                                                │
│                    [ PREVIEW SURFACE ]                         │
│                   16:9, letterboxed, GPU-blitted               │
│                                                                │
├──────────────┬──────────────┬──────────────┬──────────────────┤
│  Sources     │ Audio Mixer  │  Controls    │   Status         │
│ ┌──────────┐ │ ┌──────────┐ │ ┌──────────┐ │ ● REC 00:14:22   │
│ │Display 1 │ │ │Desktop   │ │ │  START   │ │ 1920×1080 @60    │
│ │Window: …│ │ │▬▬▬▬▬▬░░░ │ │ │ RECORDING│ │ NVENC H.264 CQ20 │
│ └──────────┘ │ │  -6.2 dB │ │ └──────────┘ │ Dropped: 0 (0.0%)│
│  + − ⚙       │ └──────────┘ │ [   ❚❚    ]  │ Disk: 8.4 MB/s   │
│              │  🔊 ⚙        │ [ Settings ] │ GPU: RTX 4050    │
│              │              │ [   Logs   ] │                  │
└──────────────┴──────────────┴──────────────┴──────────────────┘
```
The pause control (`❚❚`) is enabled only while recording. While paused the status line
reads `❚❚ PAUSED 00:14:22` — the **timeline** elapsed, i.e. the length the file will
have — with `Paused: 00:02:07` beneath it (§7.5, §16.5).

### 16.3 Theme
Dark, modern, low-eye-strain. **No pure black, no pure white** — pure `#000000` backgrounds cause halation on OLED and pure white text at high contrast is fatiguing over long sessions.

```
--bg-base         #16181C   window background
--bg-surface      #1E2126   panels, group boxes
--bg-elevated     #272B32   inputs, list items, hover targets
--border          #343941
--text-primary    #E4E6EB   (not #FFFFFF)
--text-secondary  #9BA1AC
--text-disabled   #5C636E
--accent          #4C8DFF   primary actions, focus rings
--accent-hover    #6BA0FF
--rec-active      #E5484D   recording indicator (pulsing, 1 Hz)
--warn            #F5A524
--ok              #35C489
```
- Typography: **Inter** or **Segoe UI Variable**, 13 px base, 11 px for the status readout.
- 4 px corner radius, 1 px borders, no drop shadows, no gradients, no glassmorphism.
- Focus rings on every interactive element (keyboard navigability is not optional).
- All state changes animate at 120 ms `ease-out`. The recording dot pulses; nothing else moves.
- Ship a QSS stylesheet as a single themeable file (`theme_dark.qss`) with the palette as variables so a light theme is a drop-in later.

### 16.4 Settings dialog sections
`General` (output dir, filename template, language) · `Video` (resolution locked at 1080p, fps 60/30, container mp4/mkv, encoder auto/NVENC/AMF/x264, rate control, quality) · `Audio` (device, channel layout, bitrate, multi-track when MKV — see below) · `Recording` (segmentation — **default off**, with an explicit warning that it produces multiple files) · `Advanced` (capture backend auto/WGC/DDA, cursor, HDR tone-map, log level, GPU override) · `Updates` · `About`.

**The `Audio` section's multi-track controls — added 2026-08-06.** This section named "multi-track when MKV" and said nothing about how a user names the applications, which §8.6 requires. Three controls:

- a **checkbox**, disabled with an inline reason when the container is MP4 (§20 row 16 requires the reason, not a silent grey-out) or when §8.6's runtime probe says the machine cannot do it;
- a **text field** of comma-separated executable names, which is the source of truth — §8.6 explicitly supports naming a target that has not started yet, and a picker alone could not express that;
- a **picker** of the applications running now, which *appends* to that field. It lists one entry per executable name, resolved the same way §8.6's poll resolves it, so choosing `chrome.exe` and typing `chrome.exe` mean the same thing. Populated from `get_devices`, which gained a `processes` field for it — additive, so an older GUI is unaffected (§15.1).

Plus a **reconnect** checkbox for §8.6's re-attach policy, default on.

### 16.5 Accessibility & UX invariants
- Global hotkeys for start/stop/pause, rebindable, registered via `RegisterHotKey`, with conflict detection.
- **Paused is a first-class state, not a variant of recording (§7.5).** The status panel shows `❚❚ PAUSED` with the *timeline* elapsed — the length the file will actually have — and reports total paused time separately. The recording dot stops pulsing and holds. A paused recording that looks like a running one is how a user loses ten minutes of footage without noticing.
- Every destructive or surprising action (enabling segmentation, changing output directory mid-session) requires explicit confirmation.
- The status panel never lies. If frames are being dropped, it says so in `--warn` colour with the exact count and percentage.

---

## 17. Configuration System

- Location: `%LOCALAPPDATA%\FrameCapture\config.toml`. Never in Program Files.
- Every file carries `schema_version`. On load: if `schema_version < current`, run the **ordered migration chain** (`migrate_1_to_2`, `migrate_2_to_3`, …), back up the original to `config.toml.bak.<version>`, and log every transformation. **Never silently discard unknown keys** — preserve them so downgrading doesn't destroy the user's settings.
- Validation is **schema-driven with explicit ranges**. An invalid value falls back to the default and raises a GUI warning; it never crashes and never silently produces a bad recording.
- Config is loaded once by the GUI, validated, and pushed to the engine via `configure`. The engine is stateless with respect to disk config — a single source of truth eliminates an entire class of "the GUI says 60 fps but the engine recorded 30" bugs.
- Atomic writes: write to `config.toml.tmp`, `FlushFileBuffers`, then `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`.

---

## 18. Logging, Telemetry & Diagnostics

- Async spdlog. Sinks: rotating file (10 MB × 5) at `%LOCALAPPDATA%\FrameCapture\logs\`, an in-memory ring of the last 2000 entries, and (debug builds) MSVC output.
- Levels: `TRACE`(per-frame, off by default) `DEBUG` `INFO` `WARN` `ERROR` `CRITICAL`.
- **Structured fields on every line:** ISO-8601 UTC timestamp, level, thread name, subsystem, `session_id`, message, and a key-value map.
- Mandatory session preamble logged at every start: OS build, CPU, full adapter enumeration with driver versions, display topology, audio endpoint format, selected capture backend, selected encoder + settings, config hash.
- **Never log per-frame at `INFO`.** Frame-level data goes to a 2 Hz aggregated `stats` event: `{captured, encoded, dropped_capture, dropped_encode, duplicated, avg_encode_ms, p99_encode_ms, queue_depths, av_drift_ms, disk_mbps, bitrate_kbps}`.
- On crash: minidump + ring-buffer flush + a `crash_report.json` with the last known state machine position. `SetUnhandledExceptionFilter` **plus** `_set_purecall_handler`, `set_terminate`, and `_set_invalid_parameter_handler` — the default CRT handlers swallow failures silently.
- **Telemetry is local-only. No network transmission, ever.** A "Export diagnostic bundle" button zips logs + config + last minidump for manual sharing.

---

## 19. Error Handling Policy

- **Typed error domain:** `enum class FcError` with stable numeric codes, grouped by subsystem (1xxx capture, 2xxx gpu, 3xxx audio, 4xxx encode, 5xxx mux, 6xxx io, 7xxx ipc, 9xxx internal). Every code is documented in `docs/ERROR_CODES.md` with cause and remediation.
- **`Result<T, FcError>` (expected-style) for recoverable failures; exceptions only for genuinely exceptional, unrecoverable conditions.** Exceptions never cross the IPC or thread boundary — each thread's entry point wraps its body in a catch-all that logs, classifies, and transitions the state machine.
- **Every `HRESULT` is checked.** A `FC_HR(expr)` macro logs the failing expression, file, line, HRESULT value, and the human-readable `FormatMessage` string. No bare `hr = foo();` with an unchecked result.
- **Banned patterns** (enforce with a clang-tidy config and CI grep): empty catch blocks, catching `...` outside a thread entry point, `assert` as production error handling, `exit()` outside `main`, raw `new`/`delete`, raw owning pointers (use `ComPtr`, `unique_ptr`, and RAII wrappers for every FFmpeg object: `AVFormatContext`, `AVCodecContext`, `AVFrame`, `AVPacket`, `SwrContext`).
- **The prime directive of error handling in this project:** *a failure anywhere in the pipeline must still result in a valid, playable output file.* Only a disk-full or file-handle-lost condition may violate this, and both must be detected pre-emptively.

---

## 20. Failure-Mode Matrix → Root Cause → Mitigation → Test

Each row must have a named, automated test. A row without a green test is an incomplete feature.

| # | Symptom | Primary root cause(s) | Mitigation | Test |
|---|---|---|---|---|
| 1 | **All-black video** | DDA device created on the wrong adapter; DRM-protected surface; HDR FP16 misinterpreted as BGRA8; capture session never started | §5.1 adapter-output matching; §4.2 format probe + tone-map; explicit protected-content detection with a user-facing message | `test_no_black_frames`: decode every frame, assert mean luma variance > threshold; run on both adapters and with HDR forced on |
| 2 | **Wrong file type / unplayable** | Extension/muxer mismatch; `av_write_trailer` never called | Muxer chosen from container enum, never from the filename string; §10.4 validation gate before success is reported | `test_container_integrity`: ffprobe the output, assert `format_name`, stream count, codec IDs |
| 3 | **Corrupted file after crash** | Progressive MP4 `moov` never written | §10.3 fragmented-MP4-then-remux | `test_crash_recovery`: `TerminateProcess` at t=30 s, assert the file decodes and duration ≥ 28 s |
| 4 | **A/V sync drift** | No silence injection during loopback gaps; audio PTS from sample counter; device clock ≠ nominal | §8.2, §8.3, §8.4 | `test_av_sync_4h`: 1 kHz beep on exact frame boundaries + white flash frame; decode and assert `\|offset\| < 20 ms` at every 60 s mark |
| 5 | **Glitched / torn frames** | DDA on a fullscreen-exclusive swapchain; texture read before GPU fence signal | Prefer WGC; keyed-mutex / fence sync on every shared texture; never `Map` without waiting | `test_frame_integrity`: burn a frame-index barcode into a synthetic source, decode, assert monotonic index with zero corruption |
| 6 | **Low frame rate** | CPU-side `swscale`; unbounded queue growth; encoder preset too slow; cross-adapter copy on the hot path | GPU compute conversion (§6); bounded queues; degradation ladder (§13); encode-where-pixels-live (§5.2) | `test_sustained_60fps`: 10 min under 90% GPU load from a synthetic stressor, assert dropped < 0.5% |
| 7 | **Repeated / jerky frames** | Duplicate frames emitted with wrong PTS; wall-clock PTS jitter | §7.2 deterministic CFR pacer with quantized indices | `test_cfr_exactness`: assert `frame_count == duration_s * fps` exactly, and all PTS deltas identical |
| 8 | **Color tint / washed out** | BGRA↔RGBA swap; `_SRGB` view double-gamma; missing/incorrect VUI; wrong matrix or range | §6, every stage locked and asserted | `test_color_accuracy`: SMPTE bars source, decode, assert each patch within ΔE00 < 2.0; `test_vui_tags`: ffprobe assert BT.709 + limited range in stream *and* container |
| 9 | **Frame freeze / stuck** | Blocking call on the capture thread; deadlock on a shared texture mutex; WGC pool exhausted | No blocking on capture thread (§12); watchdog that detects `no frame in 3 × frame_interval` and forces a session rebuild | `test_watchdog_recovery`: inject a stalled downstream consumer, assert recovery < 500 ms with a contiguous timeline |
| 10 | **Frame drops** | Queue overflow; disk too slow; encoder starved | Bounded queues + §13 ladder + disk-throughput monitor | Covered by #6, plus `test_slow_disk`: throttled I/O, assert degradation instead of failure |
| 11 | **Mid-recording GPU switch breaks recording** | `DEVICE_REMOVED` / `ACCESS_LOST` unhandled | §5.4 migration procedure | `test_gpu_migration`: force adapter change (driver restart via `pnputil` or a test-only injected `DXGI_ERROR_DEVICE_REMOVED`). **Same-adapter rebuild:** assert a single continuous playable file, gap < 350 ms. **Cross-adapter migration:** assert a clean, loudly-logged segment boundary and that both segments are playable, gap < 350 ms. See §5.4's amendment of 2026-07-30 |
| 12 | **Audio device change breaks audio** | No `IMMNotificationClient` | §14.1 | `test_audio_device_migration`: switch default endpoint mid-record, assert no timeline shortening and no desync |
| 13 | **Orphaned engine process** | GUI killed, engine survives | Job Object kill-on-close + heartbeat (§3.1) | `test_orphan_prevention`: kill the GUI, assert engine exits within 6 s having finalized the file |
| 14 | **Multi-track: tracks desynced or ragged lengths** | Per-track clocks reconciled independently; late-starting track offset instead of silence-padded | §8.6 — one QPC epoch, per-track silence generator, pad from `t0` | `test_multitrack_alignment`: distinct tone per track, assert identical durations and per-track sync < 20 ms over 30 min |
| 15 | **Multi-track: target app exits → track truncated or file invalid** | Client torn down without padding the timeline | §8.6 — track continues as silence to end of file | `test_multitrack_process_exit` |
| 16 | **Multi-track silently enabled on MP4 → tracks invisible in most players** | Soft warning instead of a hard block | `FcError::MULTITRACK_REQUIRES_MKV` (3021), enforced engine-side, not just in the GUI | `test_multitrack_mp4_rejected` |
| 17 | **Wrong channel layout (5.1 plays as stereo / channels swapped)** | Layout signalled in the codec context but not in `AudioSpecificConfig` or the container | §8.5 — all three signalling sites asserted | `test_channel_layout`: encode 5.1 and 7.1, ffprobe assert layout in stream *and* container; per-channel tone identification |
| 18 | **Pause/resume desyncs audio, freezes video, or shortens the file** | Paused wall-clock time excised from one stream's timeline but not the other's; the silence generator filling the paused span; duplicates emitted across the pause | §7.5 — one shared `paused_total_ns` alongside `t0`, consulted by the pacer and the audio timeline both; silence generator suspended; forced IDR on resume | `test_pause_resume`: record with a 1 kHz beep and a flash on frame boundaries, pause 3×, assert the decoded file contains exactly the unpaused frame count, `\|A/V offset\| < 20 ms` at every mark **after** each resume, no duplicate run at a pause seam, and one continuous file |

### 20.1 Additional test tiers
- **Unit (GoogleTest):** every pure function — clock math, PTS quantization, drift calculation, config migration, segment naming, error mapping. Target **> 85% line coverage on non-UI C++**.
- **Integration:** synthetic D3D11 test source (deterministic frame-index barcode + SMPTE bars + a 1 kHz tone) so tests are hermetic and don't depend on what's on screen.
- **Chaos:** randomized injection of `DEVICE_REMOVED`, `ACCESS_LOST`, audio discontinuity, disk stall, and queue saturation during a 30-minute run; assert the file is always valid.
- **Soak:** 4-hour continuous recording; assert zero drift beyond 20 ms, flat memory (no growth > 5 MB/hour), no handle leaks (`GetProcessHandleCount` flat).
- **Python:** pytest for GUI logic + IPC contract tests against a mock engine, and a golden-file test for the JSON protocol schema.
- **CI:** GitHub Actions `windows-latest` for build + unit tests; a **self-hosted runner on the reference rig** for all GPU/capture/soak tests (hosted runners have no real GPU and will silently pass meaningless tests).

---

## 21. Build, Packaging, Install, Update

### 21.1 CMake
- Top-level `CMakeLists.txt`, `cmake_minimum_required(VERSION 3.25)`, presets in `CMakePresets.json` (`windows-msvc-debug`, `windows-msvc-release`, `windows-msvc-relwithdebinfo`).
- vcpkg manifest mode (`vcpkg.json` with a pinned `builtin-baseline`) — the build must be **byte-reproducible from a clean clone**.
- Targets: `fc_core` (static lib, all logic), `framecapture-engine` (thin exe), `fc_tests` (GoogleTest). All logic lives in `fc_core` so it is testable without spawning a process.
- Warnings: `/W4 /permissive- /WX` on `fc_core`. No exceptions to `/WX`.
- Release: `/O2 /GL /Gy /DNDEBUG` + `/LTCG`, **`/DEBUG:FULL` with separate PDBs shipped to a symbol store** (you cannot debug a user crash dump without them).
- `clang-tidy` + `clang-format` (LLVM base, 4-space, 120 col) enforced in CI.

### 21.2 Commands (must all work verbatim from a clean clone)
```powershell
# Bootstrap
git clone --recursive <repo> && cd FrameCapture
.\scripts\bootstrap.ps1              # vcpkg + Python venv + pre-commit hooks

# Configure & build (C++ engine)
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release --parallel

# Tests
ctest --preset windows-msvc-release --output-on-failure
ctest --preset windows-msvc-release -L gpu        # reference-rig only
pytest gui/tests -v

# Run (dev)
.\scripts\run-dev.ps1                # launches engine + GUI with verbose logging

# Package
.\scripts\package.ps1 -Version 1.0.0 # PyInstaller + Inno Setup -> dist/FrameCapture-1.0.0-setup.exe

# Quality gates
.\scripts\lint.ps1                   # clang-format --dry-run --Werror, clang-tidy, ruff, mypy
```

### 21.3 Install / uninstall / update
- **Install:** per-user by default (`%LOCALAPPDATA%\Programs\FrameCapture`) — no admin prompt, no UAC friction. Per-machine offered as an option. Registers Start Menu shortcut, optional desktop shortcut, and an `ARP` (Add/Remove Programs) entry with a working `UninstallString`, publisher, version, and icon.
- **Clean uninstall:** removes binaries, shortcuts, registry keys, and the Job Object registration. **Prompts before deleting** `%LOCALAPPDATA%\FrameCapture\` (config, logs) and **never touches recorded video files.** Leaving orphaned config is bad; deleting a user's recordings is unforgivable.
- **Update:** GUI checks a static JSON manifest over HTTPS (opt-in, default on, one-click disable), verifies an **Ed25519 signature** on the manifest, downloads the delta or full installer, verifies SHA-256, and runs the installer with `/SILENT /CLOSEAPPLICATIONS`. Config migration (§17) runs on first launch of the new version. **Never auto-update while a recording is active.**
- Every install/uninstall/update step is logged to `%LOCALAPPDATA%\FrameCapture\logs\install.log`.

---

## 22. Documentation Deliverables

All in `docs/`, all in Markdown, all kept current as a merge-gate requirement.

| File | Contents |
|---|---|
| `README.md` | What it is, screenshots, quick start, system requirements, 5-line install |
| `ARCHITECTURE.md` | Process topology, subsystem responsibilities, data flow diagrams, threading model, state machines |
| `BUILD.md` | Prerequisites (VS 2022 17.8+, Windows SDK 10.0.22621+, CMake 3.25+, Python 3.11+), every command, every common build failure and its fix |
| `CONFIG.md` | Every config key: type, range, default, effect, schema version introduced |
| `IPC_PROTOCOL.md` | Full message schemas, versioning policy, example exchanges, backward-compat rules |
| `ERROR_CODES.md` | Every `FcError`: code, meaning, likely cause, user remediation, log signature |
| `GPU_HANDLING.md` | Deep dive on hybrid-GPU detection, adapter/output ownership, cross-adapter transfer, migration procedure, and the specific behavior of MUX-less vs Advanced Optimus |
| `TESTING.md` | How to run each tier, how to set up the self-hosted GPU runner, how to interpret the acceptance report |
| `PERFORMANCE.md` | Benchmarks on the reference rig, profiling methodology, per-stage timing budgets |
| **`ENGINEERING_LOG.md`** | **Mandatory bug journal** — see below |
| `CONTRIBUTING.md` | Code style, PR checklist, commit convention (Conventional Commits) |
| `CHANGELOG.md` | Keep a Changelog format, semver |

### 22.1 `ENGINEERING_LOG.md` format (required for every non-trivial bug)
```markdown
## [BUG-023] Black frames when capturing a fullscreen game on the dGPU

**Severity:** Critical   **Found:** 2026-03-14   **Fixed:** 2026-03-16   **Commit:** a3f9c21

### Symptom
Recording a fullscreen D3D12 title produced a file of the correct duration and
bitrate in which every frame was solid black. Audio was correct. No errors logged.

### Investigation
1. Confirmed the encoder was receiving frames (frame counter incremented).
2. Dumped the raw NV12 staging texture — already black at the convert stage.
3. Dumped the source BGRA texture — black at capture.
4. Compared `DXGI_OUTPUT_DESC.Monitor` against the D3D device's adapter LUID.

### Root cause
The D3D11 device was created on adapter index 0 (RTX 4050) while the target
`IDXGIOutput` belonged to adapter index 1 (Radeon 780M), which owns the internal
panel on this MUX-less chassis. `DuplicateOutput` returned S_OK but produced no
valid content — the failure was silent, not an HRESULT error.

### Fix
`GpuTopologyService::ResolveAdapterForMonitor()` now maps HMONITOR → IDXGIOutput →
parent IDXGIAdapter and creates the capture device on that exact adapter. Added a
post-init validation frame check that fails loudly if the first 3 frames are
uniformly black.

### Regression test
`test_capture_adapter_affinity` (gpu tier) — forces capture on each adapter in turn
and asserts non-trivial luma variance.

### Lessons
DXGI can return S_OK and hand you nothing. Never treat a successful HRESULT as
proof of correct output — validate the pixels.
```

---

## 23. Repository Layout

```
FrameCapture/
├── CMakeLists.txt · CMakePresets.json · vcpkg.json · .clang-format · .clang-tidy
├── engine/
│   ├── core/          # fc_core static lib
│   │   ├── clock/     ipc/     config/    logging/    error/
│   │   ├── capture/   # IScreenCapture, wgc/, dda/, source_resolver
│   │   ├── gpu/       # topology, adapter_selector, cross_adapter, d3d_device
│   │   ├── color/     # compute shaders (.hlsl), converter, tonemap
│   │   ├── audio/     # wasapi_loopback, silence_gen, resampler, drift
│   │   ├── encode/    # video_encoder, audio_encoder, encoder_factory
│   │   ├── mux/       # muxer, mp4_finalizer, segmenter, recovery
│   │   ├── pipeline/  # frame_pacer, queues, health_monitor, state_machine
│   │   └── util/      # ring_buffer, raii wrappers, thread_utils
│   └── app/           # main.cpp — thin
├── gui/
│   ├── framecapture_gui/  # main_window, preview, settings/, widgets/, ipc_client,
│   │                      # shm_reader, theme/theme_dark.qss, resources/
│   └── tests/
├── tests/
│   ├── unit/  integration/  chaos/  soak/  fixtures/
│   └── tools/synthetic_source/   # deterministic D3D11 test pattern generator
├── scripts/       bootstrap · run-dev · package · lint · sign
├── installer/     framecapture.iss
└── docs/          (§22)
```

---

## 24. Milestones

| M | Deliverable | Exit criterion |
|---|---|---|
| **M0** | Skeleton: CMake + vcpkg + CI + logging + config + error domain | `cmake --build` and `ctest` green on a clean clone |
| **M1** | GPU topology service | Correctly identifies both adapters, display ownership, and encode capability on the reference rig; `test_gpu_topology` green |
| **M2** | Capture → raw NV12 to disk | 60 s of visually correct NV12 on both adapters; no black frames |
| **M3** | Video encode + MKV mux | Playable 1080p60 MKV; color tests green |
| **M4** | Audio **Tier A** (multi-channel up to 7.1) + AAC + A/V mux | 30 min recording, drift < 20 ms, silence gaps handled, tests #4 and #17 green |
| **M5** | MP4 + crash-safe finalization | `test_crash_recovery` green |
| **M6** | CFR pacer + degradation ladder + health monitor | Tests #6, #7, #10 green |
| **M7** | Dynamic GPU + audio device migration | Tests #11, #12 green |
| **M8** | IPC + Python GUI (full, themed) + **pause/resume (§7.5)** | End-to-end recording driven entirely from the GUI, **including pause and resume**; test #18 green |
| **M9** | Segmentation (opt-in) + preview | Keyframe-aligned splits with no frame loss |
| **M9.5** | Audio **Tier B** (per-app multi-track, MKV only) | Tests #14, #15, #16 green; Tier A provably unaffected when Tier B is off |
| **M10** | Installer, updater, docs, full acceptance suite | **All 18 rows of §20 green; 4-hour soak clean; ENGINEERING_LOG.md complete** |

---

## 25. Definition of Done

FrameCapture v1.0 ships only when **all** of the following hold on the reference rig:

- [ ] Every row in §20 (all 18) has a green automated test.
- [ ] Audio Tier A green with stereo, 5.1, and 7.1 endpoints; layout correct in stream *and* container.
- [ ] Audio Tier B green on MKV with 6 tracks; hard-rejected on MP4 with error 3021; Tier A byte-identical whether Tier B is on or off.
- [ ] `test_amf_zero_copy` green — no host round-trip on the AMF encode path.
- [ ] HEVC/AV1 code paths exist, are unit-tested against a fake encoder, and are not GUI-exposed.
- [ ] 4-hour soak: zero dropped frames beyond 0.1%, A/V drift < 20 ms, flat memory and handle count.
- [ ] `TerminateProcess` at any point during recording yields a playable, seekable file.
- [ ] Forced mid-recording GPU migration yields one continuous file with a gap < 350 ms.
- [ ] Mid-recording default-audio-device change yields no desync and no timeline shortening.
- [ ] Pause/resume (§7.5) excises paused time from **both** streams equally: one continuous file, A/V offset < 20 ms after every resume, no frozen-frame run at any seam.
- [ ] Color: SMPTE bars round-trip within ΔE00 < 2.0; BT.709 + limited range tagged in both bitstream and container.
- [ ] Capture overhead < 6% of one CPU core, < 4% GPU.
- [ ] GUI idle CPU < 1%; zero media data crosses the IPC boundary except downscaled preview.
- [ ] Clean install → record → uninstall leaves zero orphaned processes, services, or registry keys, and zero deleted user recordings.
- [ ] `/W4 /WX` clean, clang-tidy clean, ruff + mypy clean.
- [ ] All §22 docs complete, including at least the real bug entries in `ENGINEERING_LOG.md`.
- [ ] No microphone code, no camera code, no network egress from the engine — verified by a static audit and by monitoring the engine process with a network sniffer during a full recording session.
```
