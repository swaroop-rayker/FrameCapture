# GPU Handling

Hybrid-GPU detection, adapter/output ownership, encoder selection, and the specific
behaviour of MUX-less versus Advanced Optimus laptops (SPEC.md §5, §22).

> **Scope.** This covers what M1 implements: discovery (§5.1) and the selection
> policy (§5.2). Cross-adapter transfer (§5.3) and mid-recording migration (§5.4)
> are described here only where they affect the current design; they land with
> M2/M3 and M7 respectively and are marked as such.

---

## 1. Why this subsystem exists

On a MUX-less laptop, **the display is wired to the integrated GPU**, not the
discrete one. The discrete GPU renders and hands finished frames to the iGPU for
scanout. Two consequences drive everything below:

1. **A capture device created on the wrong adapter produces black frames, silently.**
   `DuplicateOutput` returns `S_OK`. Every subsequent call succeeds. The recording is
   the right length, the right bitrate, and solid black. There is no error code
   anywhere in the sequence (SPEC.md §20 row 1).
2. **Encoding on the "faster" GPU is usually the wrong choice.** Frames already live
   in iGPU memory. Shipping 1080p60 BGRA to the dGPU costs roughly
   8.3 MB × 60 = **500 MB/s over PCIe** for zero quality gain.

Both mistakes are intuitive. That is why the policy is written down and tested
rather than left to judgement at the call site.

---

## 2. Discovery

`GpuTopologyService::refresh()` implements SPEC.md §5.1:

| Step | What happens |
| --- | --- |
| 1 | `CreateDXGIFactory1` → `EnumAdapters1`. Records LUID, vendor/device id, dedicated VRAM, shared memory, flags. |
| 2 | Classifies each adapter as Software, Integrated, or Discrete. |
| 3 | Enumerates each adapter's `IDXGIOutput`s. |
| 4 | Maps `HMONITOR` → `IDXGIOutput` → parent adapter → LUID. |
| 5 | Probes encode capability by opening a real encoder session. |

The factory is **rebuilt on every refresh**, never cached. A stale
`IDXGIFactory1` keeps reporting the old adapter set forever — which is exactly what
you must not do when handling a topology change.

### Identity is the LUID, never the index

Adapter *index* renumbers when an eGPU is attached or a driver restarts, i.e.
precisely when it matters. Everything keys on the LUID.

### Classification

```
Software    DXGI_ADAPTER_FLAG_SOFTWARE, or VendorId == 0x1414 (Microsoft)
Integrated  DedicatedVideoMemory < 512 MB  AND  SharedSystemMemory > DedicatedVideoMemory
Discrete    everything else
```

The Microsoft Basic Render Driver is caught by vendor id as well as by flag, because
the flag is not set on every Windows build.

Software adapters are **excluded from encode selection** and permitted only as a
last-ditch capture device.

### An adapter with zero outputs is normal

On Optimus the discrete GPU typically reports **no outputs at all**. It is not
broken, not disabled, and **still a valid encode target**. Filtering out
output-less adapters would remove the dGPU from consideration entirely.

### Ownership resolution returns null rather than guessing

`Topology::owner_of_monitor()` returns `nullptr` when the monitor maps to no
adapter, and `select_for_monitor()` turns that into
`GPU_OUTPUT_OWNERSHIP_UNRESOLVED` (2003).

**It must never fall back to adapter 0.** That fallback is the single most direct
route to an all-black recording, and it fails in the one configuration this project
exists to support.

### Capability is probed, never inferred

SPEC.md §5.1 step 5 is emphatic, and the reason is that a vendor id tells you which
encoder *exists*, not whether it will *open*. These all look identical to a
heuristic and different to a probe:

- another application holding the encoder's session limit;
- a driver advertising NVENC on a card whose sessions are exhausted;
- a laptop dGPU currently powered down;
- an adapter that opens the encoder but rejects the NV12 input pool (see §4).

The probe creates a D3D11 device on the adapter, wraps it in an
`AV_HWDEVICE_TYPE_D3D11VA` context, allocates a small NV12 frame pool, opens
`h264_nvenc` / `h264_amf` / `h264_qsv` by vendor, and immediately tears it all down.
It costs roughly 100–200 ms per adapter, so results are cached by
**(LUID, driver version)** — a driver update invalidates the entry and forces a
re-probe.

The probe never fails as an *operation*. An adapter that cannot encode is a fact,
not an error; the result carries `probed = true` either way, with `detail`
explaining a negative.

---

## 3. Encoder selection

`select_encoder()` is a **pure function** over already-discovered facts. That is
deliberate: it means the MUX-less case is exercised on the CPU test tier with
synthetic topologies, not only on hardware.

```
Rule 1  capture adapter can encode              → use it            [zero-copy]
Rule 2  a discrete adapter can encode AND
        measured transfer < 2.0 ms/frame        → use it            [§5.3]
Rule 3  the integrated adapter can encode       → use it
Rule 4  otherwise                               → software
```

### Rule 2 requires a *measured* cost

`SelectionInput::measured_cross_adapter_ms` is `std::optional<double>`, and
`std::nullopt` — "not measured yet" — **cannot satisfy rule 2**. The spec says the
cost must be measured; assuming a value would be exactly the "just use the fast GPU"
mistake the rule exists to prevent.

Until the transfer path exists (§5.3, M2/M3) this is always `nullopt`, so rule 2
never fires today. That is the correct behaviour, not a gap.

### Rule 4 is reachable but not yet serviceable

The software rung needs libx264, which is an open licensing decision — linking it
makes the distribution GPLv2 (CLAUDE.md §9). Selection will report
`software_fallback`; the encoder factory will then report
`ENCODE_NO_HARDWARE_ENCODER` (4001).

---

## 4. The NV12 encoder-input pool trap

FFmpeg passes `AVD3D11VAFramesContext.BindFlags` straight to `CreateTexture2D` and
supplies **no default**. Left at `0`, allocation fails with `E_INVALIDARG` and a
message that names no field.

**`D3D11_BIND_DECODER` is required, and the reason is `ArraySize`, not the vendor.**
A frame pool is a texture *array*, and D3D11 rejects an array of a video format
without `DECODER` on both reference adapters — the conventional `RENDER_TARGET`
fails, which is counter-intuitive for an *encoder input* pool. A single NV12
texture, by contrast, accepts all of these; measuring at `ArraySize = 1` gives the
wrong answer.

| `BindFlags` | `ArraySize = 1` | `ArraySize = 4` |
| --- | --- | --- |
| `UNORDERED_ACCESS` | accepted | rejected, `0x80070057` |
| `RENDER_TARGET` | accepted | rejected, `0x80070057` |
| `SHADER_RESOURCE` | accepted | rejected, `0x80070057` |
| `DECODER` | accepted | **accepted** |
| `DECODER \| UNORDERED_ACCESS` | accepted | **accepted** |

Measured 2026-07-28 on both the Radeon 780M and the RTX 4050; identical results.

**`DECODER | UNORDERED_ACCESS` is accepted, which makes the encode path
zero-copy.** The conversion shader writes NV12 straight into an encoder pool slice,
so no device-to-device copy sits between conversion and encode. Accepting the
texture is not enough on its own — the probe also creates the `R8_UNORM` and
`R8G8_UNORM` planar UAVs before reporting the flag usable, because a valid texture
desc does not guarantee a valid view. (D3D11 picks the plane by view *format*;
`PlaneSlice` is D3D12-only.)

The value is still **probed per adapter** and stored in
`EncoderCapability::nv12_pool_bind_flags`; call `nv12_pool_is_uav_writable()` to
find out whether this adapter got the zero-copy path or needs the copy. Do not
hard-code either. Full write-up: BUG-001 and its follow-up in
`ENGINEERING_LOG.md`.

---

## 5. Measured on the reference rig

AMD Ryzen 7 7840HS + Radeon 780M + RTX 4050, 2026-07-27, as reported by
`GpuTopologyService::log_topology()`:

| | Radeon 780M | RTX 4050 Laptop | Basic Render Driver |
| --- | --- | --- | --- |
| LUID | `0x…117FE` | `0x…4D8CB12` | `0x…132B1` |
| Class | **integrated** | **discrete** | **software** |
| Driver | 32.0.11026.1 | 32.0.16.1074 | 10.0.26100.8875 |
| Dedicated VRAM | 437 MB | 6207 MB | 0 |
| Shared memory | 8155 MB | 8155 MB | 8155 MB |
| Outputs | **1** (`\\.\DISPLAY1`, 1920x1080) | **0** | 0 |
| Drives display | **yes** | no | no |
| H.264 encode | **h264_amf** | **h264_nvenc** | — |
| Negotiated | d3d11 hardware surfaces | d3d11 hardware surfaces | — |

> **Corrected 2026-07-27.** An earlier revision of this table recorded the output
> as 1536x864. That was the *virtualised* size Windows reports to a DPI-unaware
> process at 125% scaling, not the physical mode. See BUG-004; the engine now
> declares per-monitor DPI awareness before it queries DXGI.

**Selection: rule 1 → Radeon 780M.**

This is the textbook MUX-less result. The 4050 has 14× the VRAM and a faster
encoder, reports zero outputs, and is correctly **not** chosen — because the panel
is on the 780M and that is where the frames already are.

Both adapters classify correctly by the memory heuristic: the 780M's 437 MB is under
the 512 MB ceiling *and* its shared memory dominates; the 4050 fails the ceiling test
outright.

### Throughput, measured 2026-07-28

From `test_throughput.cpp`, 5-second windows on an otherwise-idle desktop:

| Measurement | Result |
| --- | --- |
| Capture + convert, **no readback** | **48.2 fps** |
| Capture inter-arrival, median | **20.875 ms** (47.9 fps) |
| Inter-arrival distribution | 93% of samples in the 20–21 ms buckets |
| Encode pipeline, synthetic source | **150.9 fps**, 0 queue drops |

**The capture rate is not a throughput limit.** Removing the per-frame readback —
previously assumed to be the bottleneck — changed nothing: 48.2 fps with it gone
versus 48.0 fps with it in place. The limiter is upstream of us.

**It is the display cadence.** 3 vblanks at 144 Hz is 20.833 ms; the measured
median is 20.875 ms, and three quarters of all samples land in a single 1 ms
bucket. A throughput-limited source produces a spread, not a spike. The most
likely mechanism is the panel idling at its VRR floor — 48 Hz is the usual
bottom of a 144 Hz variable-refresh range — with DWM compositing at that rate and
WGC delivering one frame per composite. WGC is change-driven, so an idle desktop
cannot produce more.

The practical consequence: **capture-rate figures measured on an idle desktop say
nothing about the engine.** Anything claiming a sustained capture rate needs a
source that actually changes every vblank, which is what SPEC.md §20 row 6's
stressor is for (M6).

**The encode pipeline has headroom.** 150.9 fps submitted with **zero** queue
drops — 2.5× the 60 fps target and just over a 144 Hz source. Note this is a floor
rather than a ceiling: the synthetic source's own frame generation limits the loop,
so the pipeline was never pushed to saturation.

That number settles the open question about the encode queue's drop-oldest policy
competing with the pacer's deterministic drop. The queue never filled, so the
competition does not arise in practice. Eliminating the wasted work — 60% of
conversions at 144 Hz are on frames the pacer then discards — remains worthwhile
as an optimisation, but it is not a correctness or quality risk.

---

## 6. Reacting to change (§5.4 — M7)

Not implemented. `refresh()` is safe to call repeatedly and
`adapter_set_changed()` exposes `IDXGIFactory1::IsCurrent() == false`, which is the
first of §5.4's triggers, so M7 has the primitives it needs.

Still to come at M7: the hidden message window for `WM_DISPLAYCHANGE` /
`WM_DEVICECHANGE` / `WM_DPICHANGED`, `DXGI_ERROR_DEVICE_REMOVED` handling with
`GetDeviceRemovedReason()`, the 2 Hz belt-and-braces LUID poll, and the eight-step
migration procedure with its < 350 ms gap target.

---

## 7. Diagnosing a black recording

In order of likelihood:

1. **Adapter/output mismatch.** Grep the log for `subsystem=gpu` and compare
   `capture_adapter_luid` against the adapter whose `outputs` list contains your
   monitor. They must match. `code=2003` means resolution failed outright.
2. **Protected content** — `CAPTURE_TARGET_PROTECTED` (1005). Not recoverable; the
   OS refuses to hand over the pixels.
3. **HDR misread** — `CAPTURE_SOURCE_FORMAT_UNSUPPORTED` (1006). If the source is
   `R16G16B16A16Float`, system HDR is on and the tone-map branch should have run.
   Reinterpreting those bits as BGRA8 gives black or neon green.

The session preamble records the full adapter enumeration with driver versions on
every start, so a user's log alone is enough to check step 1.

---

## 8. Testing

| Test | Tier | Covers |
| --- | --- | --- |
| `test_adapter_selector` | **cpu** | The §5.2 ladder against synthetic topologies, including the reference rig's shape, an empty topology, a vanished capture adapter, budget boundaries, and software-adapter exclusion. |
| `test_gpu_topology` | **gpu** | Real discovery: adapter identity, LUID uniqueness, classification vs reported memory, output↔adapter round trip, unknown-monitor refusal, probe execution, cache reuse across refreshes, and selection on live hardware. |
| `test_capture_sustained` | **gpu** | DDA on the owning adapter, and — the important one — DDA **refused** on a device belonging to a different adapter, with `DDA_ADAPTER_AFFINITY`. That is the exact configuration described in §1 above, asserted rather than described. |

### The DPI trap

Every dimension in this document is a **physical** pixel count. A DPI-unaware
process is told a virtualised size instead: on a 1920×1080 panel at 125% scaling,
`DesktopCoordinates` reads 1536×864 while `Windows.Graphics.Capture` still delivers
a real 1920×1080 texture.

`fc::set_process_dpi_awareness()` runs before any DXGI call in both the engine and
the GPU test binary. `SustainedCaptureTest.ReportsPhysicalPixelsNotDpiScaledOnes`
compares the resolver's output against `EnumDisplaySettings` and is the standing
guard. See BUG-004.

```bash
ctest --preset windows-msvc-release -LE gpu
```

```bash
ctest --preset windows-msvc-release -L gpu
```

GPU-tier tests **fail** rather than skip when hardware is absent. A GPU test that
passes on a machine with no GPU proves nothing (CLAUDE.md §6).
