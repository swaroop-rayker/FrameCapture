#pragma once

// Makes a *sequence* of immediate-context calls atomic (SPEC.md §12).
//
// ---------------------------------------------------------------------------
// The defect this exists to prevent, measured
// ---------------------------------------------------------------------------
// A D3D11 device created without `D3D11_CREATE_DEVICE_SINGLETHREADED` -- which is every
// device this engine creates -- protects each *call* on the immediate context with an
// internal critical section. It does **not** protect a sequence of them, and a compute
// dispatch is unavoidably a sequence:
//
//     CSSetShader -> CSSetConstantBuffers -> CSSetShaderResources -> CSSetUnorderedAccessViews
//       -> Dispatch -> unbind
//
// Compute-stage bindings are device-wide state. Two threads running that sequence against
// one context interleave, so one thread's `Dispatch` can execute with the other's shader,
// SRV or UAV bound -- and slot 0 means slot 0 to both of them.
//
// Until M9 the engine had exactly one compute stage (`Nv12Converter`, on the `venc`
// thread) and the question never arose. SPEC.md §15.2's preview adds a second, on the
// `capture` thread, off the same device. The result was measured before it was reasoned
// about: a recording with the preview running decoded with **11 and 84 corrupted frames**
// across two runs where the same recording with the preview off had none -- while
// `frames_encoded`, `frames_queue_dropped`, `frames_paced_out` and `duplicates_emitted`
// were all *identical and clean*. Nothing in the pipeline's own counters could see it,
// because nothing was dropped: the frames were written, and they were written wrong. Only
// reading the frame-index barcode out of the decoded picture caught it, which is the same
// lesson `SoftwareEncoderTest` records for the NV12 readback (CLAUDE.md §9).
//
// `ID3D11Multithread::Enter`/`Leave` is D3D11's own answer to this and is what both
// dispatch sites now take. It is the device's existing critical section, so it costs an
// uncontended lock in the common case and is re-entrant on one thread.
//
// **Both sites must take it or neither is protected**, which is why this is a shared
// header rather than a few lines inside the preview.

#include <memory>

struct ID3D11DeviceContext;
struct ID3D11Multithread;

namespace fc::gpu {

/// Holds the device's critical section for a scope.
///
/// Constructed from a cached `ID3D11Multithread`, so the per-frame path pays a lock and
/// not a `QueryInterface`. A null holder is a no-op -- a device that does not offer
/// `ID3D11Multithread` is one that was created single-threaded, where there is nothing to
/// serialise against.
class ScopedDeviceLock {
public:
    explicit ScopedDeviceLock(ID3D11Multithread* multithread) noexcept;
    ~ScopedDeviceLock();

    ScopedDeviceLock(const ScopedDeviceLock&) = delete;
    ScopedDeviceLock& operator=(const ScopedDeviceLock&) = delete;
    ScopedDeviceLock(ScopedDeviceLock&&) = delete;
    ScopedDeviceLock& operator=(ScopedDeviceLock&&) = delete;

private:
    ID3D11Multithread* multithread_ = nullptr;
};

/// The `ID3D11Multithread` for `context`, or null.
///
/// Queried once and cached by the caller. Also *enables* multithread protection, which is
/// belt and braces -- `create_device_on_adapter` already turns it on -- but a device
/// arriving from anywhere else (a test, a future interop path) must not silently lose the
/// guarantee this header is about.
///
/// The returned pointer is AddRef'd; the caller owns the reference. Declared as a raw
/// pointer out-parameter shape rather than a `ComPtr` so this header does not pull `wrl`
/// into everything that includes it.
[[nodiscard]] ID3D11Multithread* acquire_multithread(ID3D11DeviceContext* context) noexcept;

} // namespace fc::gpu
