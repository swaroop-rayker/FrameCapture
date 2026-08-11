#include "core/gpu/device_watcher.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"

#include <windows.h>
// Must follow windows.h.
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace fc::gpu {
namespace {

using Microsoft::WRL::ComPtr;

/// DXGI's device-loss codes, named locally rather than pulled from the SDK headers.
///
/// `DXGI_ERROR_*` are `HRESULT` constants that expand to `_HRESULT_TYPEDEF_` and are
/// awkward to compare against a plain `std::int32_t` without a cast at every site.
/// Naming them once here keeps `classify_device_error` a function over integers,
/// which is what makes it testable without a GPU.
constexpr std::int32_t kDxgiDeviceRemoved = static_cast<std::int32_t>(0x887A0005);
constexpr std::int32_t kDxgiDeviceHung = static_cast<std::int32_t>(0x887A0006);
constexpr std::int32_t kDxgiDeviceReset = static_cast<std::int32_t>(0x887A0007);
constexpr std::int32_t kDxgiDriverInternalError = static_cast<std::int32_t>(0x887A0020);
constexpr std::int32_t kDxgiAccessLost = static_cast<std::int32_t>(0x887A0026);

} // namespace

std::string_view to_string(DeviceChange value) noexcept {
    switch (value) {
    case DeviceChange::None:
        return "none";
    case DeviceChange::TopologyChanged:
        return "topology_changed";
    case DeviceChange::AccessLost:
        return "access_lost";
    case DeviceChange::DeviceReset:
        return "device_reset";
    case DeviceChange::DeviceRemoved:
        return "device_removed";
    }
    return "unknown";
}

bool is_device_lost(std::int32_t hr) noexcept {
    return hr == kDxgiDeviceRemoved || hr == kDxgiDeviceReset || hr == kDxgiAccessLost;
}

DeviceChangeReport classify_device_error(std::int32_t hr, std::int32_t removed_reason) noexcept {
    DeviceChangeReport report;
    report.hresult = hr;
    report.removed_reason = removed_reason;

    if (hr == kDxgiAccessLost) {
        // The device is healthy; the *session* lost its surface. Rebuilding the
        // device here would be an expensive answer to a routine desktop-mode switch,
        // and it is the misclassification most likely to be made, because the code
        // arrives from the same calls as a genuine removal.
        report.change = DeviceChange::AccessLost;
        report.error = FcError::CAPTURE_TARGET_GONE;
        report.requires_device_rebuild = false;
        return report;
    }

    if (hr == kDxgiDeviceReset) {
        report.change = DeviceChange::DeviceReset;
        report.error = FcError::GPU_DEVICE_RESET;
        report.requires_device_rebuild = true;
        return report;
    }

    if (hr == kDxgiDeviceRemoved) {
        report.change = DeviceChange::DeviceRemoved;
        report.requires_device_rebuild = true;
        // The *response* is the same for every removal reason; the *diagnosis* is
        // not, and SPEC.md §5.4 requires the difference survive into the log. A hang
        // points at our command stream, a driver internal error points at the driver,
        // and a bare removal points at the hardware or a restart.
        switch (removed_reason) {
        case kDxgiDeviceHung:
            report.error = FcError::GPU_DEVICE_HUNG;
            break;
        case kDxgiDeviceReset:
            report.error = FcError::GPU_DEVICE_RESET;
            break;
        default:
            report.error = FcError::GPU_DEVICE_REMOVED;
            break;
        }
        return report;
    }

    // Everything else -- including `DXGI_ERROR_WAIT_TIMEOUT`, which is the *normal*
    // return from an acquire on an idle desktop. Classifying that as device loss
    // would migrate the GPU every time nothing was happening on screen.
    return report;
}

bool adapter_sets_differ(std::span<const AdapterId> before, std::span<const AdapterId> after) noexcept {
    if (before.size() != after.size()) {
        return true;
    }

    // Multiplicity-counting rather than a nested "is every element of A in B" scan,
    // and rather than sorted copies.
    //
    // The membership scan is the tempting form and it is wrong in one direction: with
    // a duplicate on one side it calls {1,1} and {1,2} equal, which would hide an
    // adapter disappearing at exactly the moment DXGI reported a stale enumeration.
    // Counting each value on both sides has no such blind spot.
    //
    // Sorted copies would also be correct, but they allocate, and this function is
    // `noexcept` so that a call site holding a lock or inside a `catch` can use it
    // without a second thought. O(n^2) is free here: a machine with more than four
    // adapters does not exist in this project's scope.
    for (const AdapterId& probe : before) {
        std::size_t in_before = 0;
        std::size_t in_after = 0;
        for (const AdapterId& id : before) {
            in_before += static_cast<std::size_t>(id == probe);
        }
        for (const AdapterId& id : after) {
            in_after += static_cast<std::size_t>(id == probe);
        }
        if (in_before != in_after) {
            return true;
        }
    }
    // Equal sizes plus every `before` value appearing equally often in `after` leaves
    // no room for an unmatched `after` value, so one direction suffices.
    return false;
}

struct DeviceWatcher::Impl {
    ComPtr<IDXGIFactory1> factory;
    std::vector<AdapterId> adapters;

    /// Errors reported from other threads, folded into the next poll. Guarded
    /// because `report_device_error` is called from the capture and venc threads,
    /// neither of which may block -- the lock is held for two stores.
    mutable std::mutex reported_mutex;
    DeviceChangeReport reported;

    std::uint64_t polls = 0;
    std::uint64_t changes = 0;

    /// QPC nanoseconds until which a device-error report is taken to describe the
    /// replaced stack. 0 when not settling. Guarded by `reported_mutex`, which
    /// `acknowledge` and `report_device_error` both take (BUG-035).
    std::int64_t settle_until_ns = 0;
    std::uint64_t settled = 0;

    /// Enumerates the current adapter LUIDs. Software adapters included: SPEC.md
    /// §5.1 excludes them from *encode selection*, not from the topology, and one
    /// appearing or vanishing is still a topology change worth re-running discovery
    /// for.
    [[nodiscard]] std::vector<AdapterId> enumerate() const {
        std::vector<AdapterId> found;
        if (factory.Get() == nullptr) {
            return found;
        }
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }
            AdapterId id;
            // The LUID's two halves packed into one integer, matching `AdapterInfo`.
            id.value = (static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                       static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart));
            found.push_back(id);
        }
        return found;
    }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}

DeviceWatcher::~DeviceWatcher() = default;

Result<void> DeviceWatcher::start() {
    FC_HR_AS(CreateDXGIFactory1(IID_PPV_ARGS(&impl_->factory)), FcError::GPU_ADAPTER_ENUMERATION_FAILED);
    impl_->adapters = impl_->enumerate();
    if (impl_->adapters.empty()) {
        return FcError::GPU_ADAPTER_ENUMERATION_FAILED;
    }

    FC_LOG_INFO(Subsystem::Gpu, "device watcher started",
                LogFields{}.add("adapters", static_cast<std::int64_t>(impl_->adapters.size())));
    return ok();
}

void DeviceWatcher::report_device_error(std::int32_t hr, std::int32_t removed_reason) {
    DeviceChangeReport report = classify_device_error(hr, removed_reason);
    if (!report.changed()) {
        return;
    }

    const std::lock_guard lock(impl_->reported_mutex);

    // Still settling after a rebuild: this describes the device that rebuild replaced,
    // reported by a thread that had not noticed yet (BUG-035). Counted, not silent.
    if (impl_->settle_until_ns != 0) {
        if (timing::qpc_now_ns() < impl_->settle_until_ns) {
            ++impl_->settled;
            return;
        }
        impl_->settle_until_ns = 0;
    }

    // The more severe of what is already pending and what just arrived. A device
    // removal must not be overwritten by an access loss that happened to be noticed
    // afterwards -- the removal is what the response has to be built for.
    if (static_cast<int>(report.change) > static_cast<int>(impl_->reported.change)) {
        impl_->reported = std::move(report);
    }
}

DeviceChangeReport DeviceWatcher::poll() {
    ++impl_->polls;

    DeviceChangeReport report;
    {
        const std::lock_guard lock(impl_->reported_mutex);
        report = impl_->reported;
        impl_->reported = DeviceChangeReport{};
    }

    if (impl_->factory.Get() != nullptr) {
        // `IsCurrent` and the LUID diff are both consulted, never either/or.
        // `IsCurrent` returning false does not always mean the adapter set moved, and
        // it has been observed to stay true across a driver restart that renumbered
        // everything -- so one is not a substitute for the other, and SPEC.md §5.4
        // asks for the union.
        const bool stale = impl_->factory->IsCurrent() == FALSE;
        std::vector<AdapterId> now = impl_->enumerate();
        const bool set_changed = adapter_sets_differ(impl_->adapters, now);

        if (stale || set_changed) {
            if (static_cast<int>(DeviceChange::TopologyChanged) > static_cast<int>(report.change)) {
                report.change = DeviceChange::TopologyChanged;
                report.error = FcError::NONE; // not a failure; discovery simply has to re-run
            }
            report.adapters_before = impl_->adapters;
            report.adapters_now = now;

            if (set_changed) {
                impl_->adapters = std::move(now);
            }
            // A stale factory must be recreated or `IsCurrent` stays false forever
            // and every subsequent poll reports a change that is not there.
            if (stale) {
                ComPtr<IDXGIFactory1> refreshed;
                if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&refreshed)))) {
                    impl_->factory = std::move(refreshed);
                }
            }
        }
    }

    if (report.changed()) {
        ++impl_->changes;
        FC_LOG_WARN(Subsystem::Gpu, "device change observed",
                    LogFields{}
                        .add("change", to_string(report.change))
                        .add("hresult", static_cast<std::int64_t>(report.hresult))
                        .add("removed_reason", static_cast<std::int64_t>(report.removed_reason))
                        .add("requires_device_rebuild", report.requires_device_rebuild)
                        .add("adapters_before", static_cast<std::int64_t>(report.adapters_before.size()))
                        .add("adapters_now", static_cast<std::int64_t>(report.adapters_now.size())));
    }
    return report;
}

void DeviceWatcher::acknowledge() {
    // A fresh factory, because the old one's `IsCurrent` is already false and would stay
    // false forever -- that is the whole failure this exists to break.
    ComPtr<IDXGIFactory1> refreshed;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&refreshed)))) {
        impl_->factory = std::move(refreshed);
    }
    impl_->adapters = impl_->enumerate();

    // Anything another thread reported *while* the rebuild was running is discarded
    // too. It described the device stack that has just been replaced, so acting on it
    // would rebuild the replacement for the predecessor's fault.
    //
    // And the same is true of reports that arrive in the moments *after* this call, from
    // threads that had not yet made the D3D call that would tell them the device is
    // gone. Whether a burst lands before or after the rebuild finishes is a matter of
    // how long the rebuild took, which is not a property the response should depend on
    // (BUG-035). `kReportSettleNs` covers the difference.
    const std::lock_guard lock(impl_->reported_mutex);
    impl_->reported = DeviceChangeReport{};
    impl_->settle_until_ns = timing::qpc_now_ns() + kReportSettleNs;
}

std::vector<AdapterId> DeviceWatcher::adapters() const {
    return impl_->adapters;
}

std::uint64_t DeviceWatcher::settled_reports() const noexcept {
    const std::lock_guard lock(impl_->reported_mutex);
    return impl_->settled;
}

std::uint64_t DeviceWatcher::polls() const noexcept {
    return impl_->polls;
}

std::uint64_t DeviceWatcher::changes() const noexcept {
    return impl_->changes;
}

} // namespace fc::gpu
