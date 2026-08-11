#pragma once

#include "core/error/result.h"
#include "core/gpu/adapter_info.h"
#include "core/gpu/adapter_selector.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace fc::gpu {

struct DiscoveryOptions {
    /// Run the per-adapter encoder probe (SPEC.md §5.1 step 5).
    ///
    /// Costs roughly 100-200 ms per adapter because it opens a real encoder session.
    /// Off when a caller only needs the adapter/output map -- but note that
    /// `AdapterInfo::can_encode()` is then false everywhere, so selection must not
    /// be run on the result.
    bool probe_encoders = true;
};

/// SPEC.md §5: "This subsystem is the project's differentiator."
///
/// Owns adapter discovery, output ownership, and encode capability. It deliberately
/// does **not** own the response to topology *changes*: watching for them and
/// performing a mid-recording migration is SPEC.md §5.4, which lands with M7. What
/// exists here is `refresh()`, which M7 will call from those event handlers.
class GpuTopologyService {
public:
    GpuTopologyService();
    ~GpuTopologyService();

    GpuTopologyService(const GpuTopologyService&) = delete;
    GpuTopologyService& operator=(const GpuTopologyService&) = delete;
    // Movable: the service owns a DXGI factory and a capability cache, both of
    // which relocate fine. Declared rather than left implicit, because the
    // user-declared destructor would otherwise suppress them silently.
    GpuTopologyService(GpuTopologyService&&) noexcept;
    GpuTopologyService& operator=(GpuTopologyService&&) noexcept;

    /// Enumerates adapters, classifies them, maps outputs, and (optionally) probes
    /// encode capability. Safe to call repeatedly.
    [[nodiscard]] Result<void> refresh(const DiscoveryOptions& options = {});

    [[nodiscard]] const Topology& topology() const noexcept;

    /// True when the DXGI factory reports its adapter list is stale, i.e. an adapter
    /// was added or removed. SPEC.md §5.4's first trigger; exposed now so M7 has it.
    [[nodiscard]] bool adapter_set_changed() const;

    /// Resolves the capture adapter for a monitor and runs the §5.2 policy.
    ///
    /// Returns `GPU_OUTPUT_OWNERSHIP_UNRESOLVED` when the monitor maps to no adapter
    /// rather than falling back to a default -- guessing here is the all-black-video
    /// bug (SPEC.md §20 row 1).
    [[nodiscard]] Result<EncoderSelection>
    select_for_monitor(std::uintptr_t monitor, std::optional<double> measured_cross_adapter_ms = {}) const;

    /// Same, for the primary display. Convenience for the common case.
    [[nodiscard]] Result<EncoderSelection>
    select_for_primary_display(std::optional<double> measured_cross_adapter_ms = {}) const;

    /// Measures the cross-adapter transfer cost from the capture adapter to every
    /// other encode-capable adapter, and caches it (SPEC.md §5.3).
    ///
    /// **This is what makes §5.2 rule 2 reachable at all.** Rule 2 requires the cost be
    /// "measured < 2.0 ms/frame", and until this existed nothing supplied a number, so
    /// `measured_cross_adapter_ms` was always `nullopt` and every selection fell
    /// through rule 2 to rule 3 or 4.
    ///
    /// Costs roughly a second per adapter pair, so it is called once per session
    /// alongside §5.1 discovery -- never per frame, and never per selection. After it
    /// has run, `select_for_monitor` uses the cached figure when the caller does not
    /// supply its own.
    ///
    /// Measured on the reference rig: the shared-texture path is **unavailable**
    /// between the 780M and the RTX 4050 (`OpenSharedResource1` fails both ways), so
    /// §5.3's staged system-memory fallback applies at a P99 of 6.8-7.5 ms -- three to
    /// four times the budget. Rule 2 correctly does not fire.
    void measure_transfer_costs(std::uintptr_t monitor);

    /// The cached figure for `adapter`, or `nullopt` when `measure_transfer_costs` has
    /// not run or that pair could not transfer at all.
    [[nodiscard]] std::optional<double> transfer_cost_ms(const AdapterId& adapter) const;

    /// Writes the whole topology to the log at INFO, one event per adapter. Feeds
    /// the mandatory session preamble (SPEC.md §18).
    void log_topology() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::gpu

namespace fc {

struct SessionPreamble;

/// Fills the preamble's adapter enumeration and display topology from a discovered
/// topology (SPEC.md §18 requires both). Lives here rather than in
/// session_preamble.h so that logging does not depend on the GPU layer.
void fill_preamble_from_topology(SessionPreamble& preamble, const gpu::Topology& topology);

} // namespace fc
