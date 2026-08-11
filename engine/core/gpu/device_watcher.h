#pragma once

// Dynamic GPU change detection (SPEC.md §5.4), the `DeviceWatcher` of SPEC.md §3.
//
// §5.4 opens with "Watch **all** of these; any one may fire during a MUX switch or
// Optimus handoff", and the emphasis is the design. There is no single reliable
// signal that an adapter set has changed underneath a running recording: DXGI may
// report a stale factory, a driver restart surfaces only as a failed D3D call, and
// Windows does not always deliver the window message. So this component collects
// several partial signals and reports the union.
//
// ---------------------------------------------------------------------------
// The split, and why it is where it is
// ---------------------------------------------------------------------------
// Two of §5.4's signals are *classifications* rather than observations:
//
//   * which `FcError` an `HRESULT` from a D3D call means, and whether it obliges a
//     device rebuild;
//   * whether one LUID set differs from another in a way that matters.
//
// Both are pure functions, and both are where the subtle mistakes live -- §5.4 is
// explicit that `DEVICE_HUNG`, `DRIVER_INTERNAL_ERROR` and `ACCESS_LOST` "are
// different bugs" and must be told apart in the log. They are therefore free
// functions, unit-tested on the CPU tier against every HRESULT the API can produce,
// with no DXGI in sight.
//
// `DeviceWatcher` itself owns the DXGI factory and the 2 Hz poll, and can only be
// exercised against real adapters.
//
// ---------------------------------------------------------------------------
// What this component does not do
// ---------------------------------------------------------------------------
// It detects and classifies. It does not migrate: SPEC.md §5.4's eight-step
// procedure spans capture, colour conversion, the encoder and the muxer, and no
// component that owns a DXGI factory should also own that. The watcher reports, and
// the recording's owner acts.
//
// The window-message signals (`WM_DISPLAYCHANGE`, `WM_DEVICECHANGE`,
// `WM_DPICHANGED`) need the engine's hidden message window, which SPEC.md §3.1
// creates for `WM_QUERYENDSESSION` and which does not exist yet. Their absence is
// covered by the 2 Hz poll §5.4 asks for as "a belt-and-braces net for events
// Windows fails to deliver" -- the poll is the net, and until the message window
// exists it is also the primary. Stated here rather than left as a silent gap.

#include "core/error/fc_error.h"
#include "core/error/result.h"
#include "core/gpu/adapter_info.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fc::gpu {

/// What kind of change was observed. Ordered by severity, so `max` over two
/// observations is the one that governs.
enum class DeviceChange {
    /// Nothing has changed.
    None = 0,

    /// The adapter set differs from the cached one -- an adapter appeared or
    /// disappeared, or DXGI reported the factory stale. Re-run SPEC.md §5.1
    /// discovery and §5.2 selection; the current device may still be usable.
    TopologyChanged = 1,

    /// The capture session lost its surface (`DXGI_ERROR_ACCESS_LOST`). The device
    /// is fine; the *session* must be rebuilt. Common on a desktop-mode switch and
    /// routine enough that it must not be reported as a device failure.
    AccessLost = 2,

    /// `DXGI_ERROR_DEVICE_RESET` -- the device was reset, typically because this
    /// application caused a fault. Rebuild the device stack.
    DeviceReset = 3,

    /// `DXGI_ERROR_DEVICE_REMOVED` -- the adapter went away, or its driver
    /// restarted. Rebuild everything and re-select the adapter.
    DeviceRemoved = 4,
};

[[nodiscard]] std::string_view to_string(DeviceChange value) noexcept;

/// One observation, with enough context to log the specific fault SPEC.md §5.4
/// insists on distinguishing.
struct DeviceChangeReport {
    DeviceChange change = DeviceChange::None;

    /// The `FcError` this maps to, for the log and for the caller's `Result`.
    /// `FcError::NONE` when nothing changed.
    FcError error = FcError::NONE;

    /// The raw `HRESULT` that produced this, or 0 when it came from the poll.
    /// Logged verbatim -- §5.4: "log the specific HRESULT".
    std::int32_t hresult = 0;

    /// `GetDeviceRemovedReason()`'s answer, when one was available. Distinct from
    /// `hresult`: a D3D call returns the generic `DXGI_ERROR_DEVICE_REMOVED` and the
    /// *reason* is what says whether this was a hang, a driver internal error, or a
    /// genuine removal. Zero when not queried or not applicable.
    std::int32_t removed_reason = 0;

    /// True when the caller must rebuild the D3D device. False for `AccessLost`,
    /// which only needs the capture session back.
    bool requires_device_rebuild = false;

    /// Adapters present now, when the change came from the poll. Empty otherwise.
    std::vector<AdapterId> adapters_now;
    /// Adapters present at the previous observation.
    std::vector<AdapterId> adapters_before;

    [[nodiscard]] bool changed() const noexcept {
        return change != DeviceChange::None;
    }
};

/// Classifies an `HRESULT` from any D3D or DXGI call.
///
/// `removed_reason` is `ID3D11Device::GetDeviceRemovedReason()`'s answer, or 0 if
/// the caller could not obtain one. It is used only to enrich the report -- the
/// *change kind* comes from `hr`, because a caller that could not query the reason
/// must still get the right classification.
///
/// A pure function over two integers, deliberately. SPEC.md §5.4 requires that
/// `DEVICE_HUNG` and `DRIVER_INTERNAL_ERROR` be distinguishable in the log, and the
/// only way to be sure every one of those paths is right is to test them all
/// without a GPU.
[[nodiscard]] DeviceChangeReport classify_device_error(std::int32_t hr, std::int32_t removed_reason = 0) noexcept;

/// True when `hr` is one of the codes that means the device or session is gone.
///
/// The convenience form for call sites that only need "should I abandon this
/// device?", so they do not each re-derive the set and get it subtly different.
[[nodiscard]] bool is_device_lost(std::int32_t hr) noexcept;

/// True when the two adapter sets differ, ignoring order.
///
/// Order-insensitive because DXGI's enumeration order is not stable across a driver
/// restart -- which is exactly the event this exists to detect, so comparing
/// sequences would report a change every time the same two adapters came back in
/// the other order.
[[nodiscard]] bool adapter_sets_differ(std::span<const AdapterId> before, std::span<const AdapterId> after) noexcept;

/// SPEC.md §5.4's "background poll at 2 Hz".
inline constexpr std::int64_t kPollIntervalNs = 500'000'000;

/// SPEC.md §5.4's budget for the whole migration procedure.
inline constexpr std::int64_t kMigrationGapBudgetNs = 350'000'000;

/// How long after an `acknowledge` a device-error report is treated as describing the
/// device stack that was just replaced (BUG-035).
///
/// **Not a cooldown, and not a guess.** `DXGI_ERROR_DEVICE_REMOVED` is delivered to
/// every thread that touches the dead device, so one fault produces a *burst* of
/// reports spread over however long each of those threads takes to make its next D3D
/// call. `acknowledge` discards the ones that arrived while the rebuild was running;
/// the ones that arrive just after it describe exactly the same dead device, and
/// acting on them rebuilds the replacement for the predecessor's fault. Measured
/// before this existed: one burst of three injected removals produced two rebuilds in
/// **6 of 22** runs, depending on nothing more than whether the rebuild happened to
/// finish before the last report arrived.
///
/// The value is derived from the slowest path by which a thread still holding the old
/// device can deliver a report: the capture thread blocks up to its acquire timeout
/// (100 ms) and the encode thread up to one bounded-queue drain. `kPollIntervalNs`
/// covers both with margin and equals one watchdog tick, so at most one poll is
/// affected.
///
/// **The cost, stated because it is real:** a genuinely *new* fault inside the window
/// is not reported. It is not lost — the threads that hit the device keep reporting,
/// so the next one after the window is acted on, and the topology half of `poll`
/// (`IsCurrent` and the LUID diff) is never suppressed. The trade is up to one poll
/// interval of delay on a second real fault, against rebuilding on a fault that has
/// already been repaired.
inline constexpr std::int64_t kReportSettleNs = kPollIntervalNs;

/// Owns a DXGI factory and answers "has the adapter set changed?".
///
/// Threading: driven from the `watchdog` thread (SPEC.md §12), which is permitted to
/// block. Not thread-safe. `report_device_error` is the exception -- see below.
class DeviceWatcher {
public:
    DeviceWatcher();
    ~DeviceWatcher();

    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;
    DeviceWatcher(DeviceWatcher&&) = delete;
    DeviceWatcher& operator=(DeviceWatcher&&) = delete;

    /// Creates the factory and records the current adapter set as the baseline.
    [[nodiscard]] Result<void> start();

    /// One poll. Returns a report whose `changed()` is false in the common case.
    ///
    /// Checks `IDXGIFactory1::IsCurrent()` first because it is nearly free, then
    /// re-enumerates and diffs the LUID set. **Both**, not either: `IsCurrent` can
    /// return false for changes that do not affect the adapter set at all, and it
    /// has been observed to stay true across a driver restart that renumbered
    /// everything. Reporting the union is what §5.4 asks for.
    [[nodiscard]] DeviceChangeReport poll();

    /// Records a device error observed by another thread on a D3D call.
    ///
    /// Thread-safe, and the only method that is: `DXGI_ERROR_DEVICE_REMOVED` arrives
    /// wherever a D3D call happens -- the capture thread, the venc thread -- and none
    /// of those may block to hand it over. The next `poll` folds it into its report.
    ///
    /// Reports arriving within `kReportSettleNs` of an `acknowledge` are **dropped**:
    /// they come from threads that had not yet noticed the rebuild, and describe the
    /// device that rebuild replaced. See `kReportSettleNs` for why that is the
    /// mechanism and what it costs.
    void report_device_error(std::int32_t hr, std::int32_t removed_reason = 0);

    /// Re-baselines after the caller has finished acting on a change.
    ///
    /// **Without this a single device loss rebuilds the recording forever.** Responding
    /// to §5.4's report means creating a new device, re-running discovery and opening a
    /// new encoder -- and every one of those touches DXGI, which then reports this
    /// watcher's factory stale. The next poll sees `IsCurrent() == false`, calls it a
    /// topology change, and asks for another rebuild. Measured before this existed: one
    /// injected `DEVICE_REMOVED` produced **four** rebuilds, and three produced five.
    ///
    /// The watcher's contract is "changes since you last acknowledged", not "changes
    /// since you started", and this is the acknowledgement. Call it at the end of a
    /// rebuild, once the new device stack is the one that should be considered current.
    void acknowledge();

    /// The adapter set as of the last successful poll.
    [[nodiscard]] std::vector<AdapterId> adapters() const;

    /// Device-error reports dropped as describing an already-replaced device stack.
    /// Surfaced so a suppressed burst is visible rather than silent (BUG-035).
    [[nodiscard]] std::uint64_t settled_reports() const noexcept;

    /// Polls performed and changes reported, for the diagnostics dump.
    [[nodiscard]] std::uint64_t polls() const noexcept;
    [[nodiscard]] std::uint64_t changes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::gpu
