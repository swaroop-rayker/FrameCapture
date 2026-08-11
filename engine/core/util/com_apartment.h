#pragma once

// The process's COM apartment lifetime (BUG-037).
//
// ---------------------------------------------------------------------------
// The defect this exists to prevent
// ---------------------------------------------------------------------------
// Every thread in the engine that touches COM enters an apartment and leaves it again:
// the `capture` thread brackets itself with `winrt::init_apartment` /
// `winrt::uninit_apartment` (SPEC.md §4.2 requires MTA on the thread that owns the WinRT
// objects), the `audio` thread with `CoInitializeEx` / `CoUninitialize` (§8.1 requires
// the thread that pumps WASAPI to be the thread that created it), and endpoint
// enumeration with a scoped apartment on whatever thread asked.
//
// Each of those is correct on its own. What none of them owns is the *gap between
// recordings*: stop the recording and every one of them leaves, the process's last MTA
// member is gone, and COM shuts the MTA down and unloads the in-proc servers behind it.
// The engine process outlives that -- it serves `start_record` over IPC and records
// again -- so the next recording runs against a torn-down apartment.
//
// That is survivable for anything that re-creates its interfaces, and it is not
// survivable for C++/WinRT, which caches activation factories in **process-wide
// statics**. A cached `IGraphicsCaptureSessionStatics` does not notice that
// `GraphicsCapture.dll` was unloaded under it; the second recording's
// `GraphicsCaptureSession::IsSupported()` calls straight through a vtable that is no
// longer mapped and the process dies with an access violation before it has logged
// anything. Measured: the faulting vtable at 0x7FFA38D8B0A8 lay inside
// `GraphicsCapture.dll`'s range in the minidump's **unloaded**-module list.
//
// ---------------------------------------------------------------------------
// Why an MTA usage cookie rather than clearing the cache
// ---------------------------------------------------------------------------
// `winrt::clear_factory_cache()` exists and would drop the stale pointers, but it treats
// the symptom on one library's behalf: the cache is process-wide and shared with every
// other WinRT caller, so clearing it at the end of a recording races anything else
// mid-call, and it does nothing for the COM state that is not a WinRT factory. The
// engine's `IAudioClock2` went missing on the second recording of the same session for
// want of the same apartment.
//
// `CoIncrementMTAUsage` is the documented way to say "this process needs an MTA for its
// lifetime" without dedicating a thread to standing in one. With it held, a thread
// leaving its apartment no longer takes the MTA with it, in-proc servers stay loaded
// because the cache still holds references to them, and the per-thread
// initialise/uninitialise brackets keep meaning exactly what they say.

namespace fc {

/// Keeps the process's multi-threaded apartment alive until the process exits.
///
/// Idempotent and thread-safe; call it from anywhere that is about to enter or use a COM
/// apartment, without checking whether someone already has. Deliberately never released:
/// the thing it protects against is the apartment going away *between* recordings, and
/// the only point at which nothing needs it is process exit, where the OS does it.
///
/// Returns false if the platform refused, which is logged. A false here is not fatal --
/// it puts the process back on the pre-fix behaviour rather than stopping it, and the
/// first recording still works.
bool hold_process_mta();

/// Whether the cookie is held. For tests; nothing in the engine branches on it.
[[nodiscard]] bool process_mta_held() noexcept;

} // namespace fc
