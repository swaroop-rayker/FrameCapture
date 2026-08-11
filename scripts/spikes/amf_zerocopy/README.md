# Spike: does `h264_amf` round-trip through host memory?

Throwaway diagnostic for **SPEC.md §2.2 item 1**. Not part of `fc_core`, not part of
the test suite, not in the root CMake build graph. Nothing in here should be lifted
into the engine.

> §2.2 item 1, the clause this exists to answer:
> *"`h264_amf`'s FFmpeg wrapper performs a host round-trip on some driver revisions.
> Add `test_amf_zero_copy` asserting no `av_hwframe_transfer_data` fires on the hot
> path, and instrument PCIe transfer time. If measured GPU overhead on the 780M
> exceeds 8%, escalate — only the AMF path drops to direct AMF SDK in v1.1. NVENC
> never leaves the FFmpeg wrapper."*

## What it does

1. Enumerates DXGI adapters and creates a D3D11 device on the **Radeon 780M**
   (match by description substring; `--adapter` overrides).
2. Wraps that device in an `AV_HWDEVICE_TYPE_D3D11VA` context and allocates an
   `AV_PIX_FMT_D3D11` / NV12 frame pool on it. It never calls
   `av_hwdevice_ctx_create`, which would pick its own adapter and invalidate the
   whole experiment.
3. Pre-uploads 64 distinct NV12 patterns (moving bar + drifting chroma) as GPU
   textures, so the encoder has real residual to work on and the timed loop does
   no host→device traffic of its own.
4. Encodes 1000 frames at 1920×1080 through `h264_amf`, after 30 untimed warm-up
   frames.

## Build and run

Requires the main build to have been configured once, so the FFmpeg tree exists.

```bash
cmake -S scripts/spikes/amf_zerocopy -B build/spike-amf && cmake --build build/spike-amf --config Release
```

```bash
build/spike-amf/bin/Release/amf_zerocopy.exe
```

`--frames N`, `--adapter SUBSTRING`, `--list-adapters`.

## How to read each output

### (1) `av_hwframe_transfer_data`

The AMF wrapper lives in `avcodec-*.dll` and would reach `av_hwframe_transfer_data`
in `avutil-*.dll` through its import address table. The spike rewrites that IAT
entry and counts calls. Three distinguishable outcomes:

| Output | Meaning |
| --- | --- |
| `exported by avutil: NO` | **The run is void.** The symbol under test does not exist; a zero count proves nothing. Investigate the FFmpeg build before believing any other line. |
| `imported by libavcodec: no` | Strongest pass. libavcodec has no import entry for the function, so no code path in it — AMF wrapper included — can call it. This is a link-level fact, not a sampling result. |
| `imported: yes`, `invocations on encode path: 0` | Pass. The hook was live and never fired. |
| `invocations on encode path: > 0` | **Guard rail fired.** The wrapper is round-tripping through host memory. Escalate per §2.2. |

Warm-up invocations are counted separately and are not a failure: a one-time
transfer during encoder init is not "on the hot path".

### (2) Per-frame encode time

Only `avcodec_send_frame` plus the `avcodec_receive_packet` drain are timed.
Staging the next source pattern into the pool texture is a device-to-device copy
*we* perform, not the encoder, so counting it would inflate the answer.

Because the encoder is pipelined, a single iteration measures "cost of submitting
one frame and collecting whatever came out", not the latency of one frame through
the encoder. Mean and p50 are the throughput signal; p99 is the hitch signal that
matters for the frame pacer (SPEC.md §7.2).

### (3) Negotiated pixel format

| Output | Meaning |
| --- | --- |
| `negotiated (pix_fmt): d3d11` | The encoder accepted hardware surfaces. Required for zero-copy. |
| anything else | Frames are being converted somewhere. Zero-copy is already lost regardless of what (1) says. |

## What a clean result does *not* rule out

A pass on (1) and (3) means **no FFmpeg-level round-trip**. It does not rule out:

- a copy inside the AMF runtime or the driver, invisible to any FFmpeg API;
- an `ID3D11DeviceContext::CopyResource` issued by the wrapper itself.

Per-frame time is the backstop for both. A host round-trip at 1080p moves ~3.1 MB
each way plus a GPU sync; it does not hide inside a ~1.5 ms budget.

## Recorded observation — 2026-07-27, reference rig

Radeon 780M (integrated) + RTX 4050, driver as installed on that date. FFmpeg 8.1.2,
`x64-windows`, three consecutive runs.

```
exported by avutil            yes
imported by libavcodec        no      <- libavcodec has no IAT entry for it
invocations on encode path    0
negotiated (pix_fmt)          d3d11
negotiated (sw_pix_fmt)       nv12

mean    1.530 - 1.548 ms      p50  ~1.535 ms      p99  ~1.74 ms
throughput  ~650 fps          (~10.8x realtime at 60 fps)
```

**Verdict on the assertion in §2.2: it holds on this driver.** `avcodec-62.dll`
imports `av_hwframe_get_buffer`, `av_hwframe_ctx_init` and `av_hwframe_ctx_alloc`
from avutil, but not `av_hwframe_transfer_data` — confirmed independently with
`dumpbin /imports`. The encoder negotiated `d3d11` surfaces.

## Two things §2.2 needs the owner to pin down

**1. "PCIe transfer time" does not apply to the 780M.** It is an integrated GPU on
the same die, sharing system memory; there is no PCIe hop to instrument. The
meaningful analogue is a host↔device copy through system RAM, which is what the
timing above bounds. The clause appears to have been written with a discrete GPU
in mind and should be reworded.

**2. The 8% threshold has no stated baseline, and the readings disagree.**

| Reading of "GPU overhead > 8%" | Value here | Verdict |
| --- | --- | --- |
| 8% of the 16.67 ms frame budget at 60 fps (= 1.33 ms) | 1.53 ms → **9.2%** | would **exceed** |
| 8% over a direct-AMF-SDK implementation | not measured | needs a second implementation to compare against |

I have deliberately **not** declared the guard rail fired. The first reading is the
one the number happens to fit, but "overhead" naturally means "cost above some
baseline", which is the second reading — and that baseline does not exist yet.
Note also that 1.53 ms is one thread's submission cost at ~10.8× realtime headroom,
and it overlaps with capture; it is not 9.2% of a saturated budget.

**Decision needed before M3 treats the AMF path as done.** If the intent is the
second reading, building the direct-AMF-SDK comparison is a much larger spike and
should be scheduled deliberately.

## Incidental finding, relevant to M2/M3

FFmpeg's D3D11VA frame pool passes `AVD3D11VAFramesContext.BindFlags` to
`CreateTexture2D` unchanged. Left at its default of `0`, allocating an NV12
encoder-input array on the 780M fails:

```
[AVHWFramesContext] Could not create the texture (80070057)   // E_INVALIDARG
```

The spike now probes the adapter and prints the matrix. On this part:

| BindFlags | Result |
| --- | --- |
| `RENDER_TARGET` | rejected, `0x80070057` |
| `SHADER_RESOURCE` | rejected, `0x80070057` |
| `SHADER_RESOURCE \| RENDER_TARGET` | rejected, `0x80070057` |
| `DECODER` | **accepted** |
| `0` (FFmpeg's default) | rejected, `0x80070057` |

Only `D3D11_BIND_DECODER` is accepted, which is counter-intuitive for an *encoder
input* pool. The engine must set this explicitly at M3 and must not assume the
same value holds on the RTX 4050 — re-run the probe per adapter.

Written up as **BUG-001** in [`docs/ENGINEERING_LOG.md`](../../../docs/ENGINEERING_LOG.md),
including the regression test M3 owes (`test_d3d11_encoder_pool_bindflags`).
