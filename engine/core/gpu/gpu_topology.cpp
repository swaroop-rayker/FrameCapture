#include "core/gpu/gpu_topology.h"

#include "core/error/hresult.h"
#include "core/gpu/cross_adapter_probe.h"
#include "core/gpu/encoder_probe.h"
#include "core/logging/logger.h"
#include "core/logging/session_preamble.h"

#include <windows.h>
// Must follow windows.h.
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>

namespace fc::gpu {
namespace {

using Microsoft::WRL::ComPtr;

/// SPEC.md §5.1 step 2: integrated when dedicated VRAM is under 512 MB and shared
/// system memory dominates, or when it is an AMD APU family part.
constexpr std::uint64_t kIntegratedVramCeiling = 512ull * 1024 * 1024;

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

AdapterId luid_of(const LUID& luid) {
    return AdapterId{(static_cast<std::int64_t>(luid.HighPart) << 32) |
                     static_cast<std::int64_t>(static_cast<std::uint32_t>(luid.LowPart))};
}

AdapterClass classify(const DXGI_ADAPTER_DESC1& desc) {
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        return AdapterClass::Software;
    }

    // The Microsoft Basic Render Driver does not always set the SOFTWARE flag on
    // every Windows build, so catch it by vendor too.
    if (desc.VendorId == kVendorMicrosoft) {
        return AdapterClass::Software;
    }

    const bool little_dedicated_vram = desc.DedicatedVideoMemory < kIntegratedVramCeiling;
    const bool shared_dominates = desc.SharedSystemMemory > desc.DedicatedVideoMemory;
    if (little_dedicated_vram && shared_dominates) {
        return AdapterClass::Integrated;
    }

    return AdapterClass::Discrete;
}

/// User-mode driver version, used as part of the capability cache key so a driver
/// update forces a re-probe (SPEC.md §5.1 step 5).
std::string driver_version_of(IDXGIAdapter1* adapter) {
    LARGE_INTEGER version{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
        return "unknown";
    }

    std::array<char, 48> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%u.%u.%u.%u", static_cast<unsigned>(HIWORD(version.HighPart)),
                  static_cast<unsigned>(LOWORD(version.HighPart)), static_cast<unsigned>(HIWORD(version.LowPart)),
                  static_cast<unsigned>(LOWORD(version.LowPart)));
    return std::string{buffer.data()};
}

void collect_outputs(IDXGIAdapter1* adapter, AdapterInfo& info) {
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (output == nullptr) {
            break;
        }

        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) {
            continue;
        }

        OutputInfo out;
        out.device_name = narrow(desc.DeviceName);
        out.monitor = reinterpret_cast<std::uintptr_t>(desc.Monitor);
        out.desktop_left = desc.DesktopCoordinates.left;
        out.desktop_top = desc.DesktopCoordinates.top;
        out.desktop_right = desc.DesktopCoordinates.right;
        out.desktop_bottom = desc.DesktopCoordinates.bottom;
        out.attached_to_desktop = desc.AttachedToDesktop != FALSE;
        info.outputs.push_back(std::move(out));
    }
}

/// Capability cache keyed by (LUID, driver version), per SPEC.md §5.1 step 5.
struct CacheKey {
    std::int64_t luid = 0;
    std::string driver_version;

    friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

} // namespace

struct GpuTopologyService::Impl {
    ComPtr<IDXGIFactory1> factory;
    Topology topology;
    std::vector<std::pair<CacheKey, EncoderCapability>> capability_cache;

    /// SPEC.md §5.3's measured transfer cost, keyed by destination adapter. Populated
    /// by `measure_transfer_costs` and consulted by §5.2 rule 2. A pair that cannot
    /// transfer at all is absent rather than present-with-a-large-number, so rule 2
    /// cannot fire on it by arithmetic accident.
    std::vector<std::pair<AdapterId, double>> transfer_costs;

    [[nodiscard]] const EncoderCapability* cached(const CacheKey& key) const {
        for (const auto& [cached_key, capability] : capability_cache) {
            if (cached_key == key) {
                return &capability;
            }
        }
        return nullptr;
    }
};

GpuTopologyService::GpuTopologyService() : impl_(std::make_unique<Impl>()) {}

GpuTopologyService::~GpuTopologyService() = default;

GpuTopologyService::GpuTopologyService(GpuTopologyService&&) noexcept = default;

GpuTopologyService& GpuTopologyService::operator=(GpuTopologyService&&) noexcept = default;

Result<void> GpuTopologyService::refresh(const DiscoveryOptions& options) {
    // A stale factory never reports new adapters, so rebuild it every time rather
    // than caching one. This is the documented behaviour of IsCurrent().
    impl_->factory.Reset();
    FC_HR_AS(CreateDXGIFactory1(IID_PPV_ARGS(&impl_->factory)), FcError::GPU_ADAPTER_ENUMERATION_FAILED);

    Topology discovered;

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (impl_->factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (adapter == nullptr) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        FC_HR_AS(adapter->GetDesc1(&desc), FcError::GPU_ADAPTER_ENUMERATION_FAILED);

        AdapterInfo info;
        info.id = luid_of(desc.AdapterLuid);
        info.description = narrow(desc.Description);
        info.vendor_id = desc.VendorId;
        info.device_id = desc.DeviceId;
        info.dedicated_video_memory = desc.DedicatedVideoMemory;
        info.shared_system_memory = desc.SharedSystemMemory;
        info.adapter_class = classify(desc);
        info.driver_version = driver_version_of(adapter.Get());

        collect_outputs(adapter.Get(), info);

        if (options.probe_encoders && info.adapter_class != AdapterClass::Software) {
            const CacheKey key{info.id.value, info.driver_version};
            if (const EncoderCapability* hit = impl_->cached(key); hit != nullptr) {
                info.encode = *hit;
            } else {
                info.encode = probe_encoder(adapter.Get(), info.vendor_id);
                impl_->capability_cache.emplace_back(key, info.encode);
            }
        }

        discovered.adapters.push_back(std::move(info));
    }

    if (discovered.adapters.empty()) {
        FC_LOG_ERROR(Subsystem::Gpu, "no DXGI adapters found", LogFields{}.add_error(FcError::GPU_NO_SUITABLE_ADAPTER));
        return FcError::GPU_NO_SUITABLE_ADAPTER;
    }

    impl_->topology = std::move(discovered);
    return ok();
}

const Topology& GpuTopologyService::topology() const noexcept {
    return impl_->topology;
}

bool GpuTopologyService::adapter_set_changed() const {
    // SPEC.md §5.4's first trigger. IsCurrent() goes false once the adapter set
    // changes; the factory must then be recreated, which refresh() does.
    return impl_->factory != nullptr && impl_->factory->IsCurrent() == FALSE;
}

void GpuTopologyService::measure_transfer_costs(std::uintptr_t monitor) {
    impl_->transfer_costs.clear();

    const AdapterInfo* capture = impl_->topology.owner_of_monitor(monitor);
    if (capture == nullptr) {
        return;
    }

    for (const AdapterInfo& adapter : impl_->topology.adapters) {
        if (adapter.id == capture->id || adapter.adapter_class == AdapterClass::Software || !adapter.can_encode()) {
            continue;
        }
        const CrossAdapterCost cost = measure_cross_adapter_cost(capture->id, adapter.id);
        if (!cost.supported) {
            // Absent, not zero and not infinity. Rule 2 requires a *measurement*, and a
            // pair that cannot transfer has none -- recording a number here in either
            // direction would let the rule reach a conclusion it has no basis for.
            FC_LOG_INFO(Subsystem::Gpu, "cross-adapter transfer unavailable; §5.2 rule 2 cannot apply to this pair",
                        LogFields{}
                            .add("from", capture->description)
                            .add("to", adapter.description)
                            .add("detail", cost.detail));
            continue;
        }
        impl_->transfer_costs.emplace_back(adapter.id, cost.p99_ms);
        FC_LOG_INFO(Subsystem::Gpu, "cross-adapter transfer cost cached",
                    LogFields{}
                        .add("from", capture->description)
                        .add("to", adapter.description)
                        .add("path", to_string(cost.path))
                        .add("p99_ms", cost.p99_ms)
                        .add("budget_ms", kCrossAdapterBudgetMs)
                        .add("rule2_eligible", cost.p99_ms < kCrossAdapterBudgetMs));
    }
}

std::optional<double> GpuTopologyService::transfer_cost_ms(const AdapterId& adapter) const {
    for (const auto& [id, cost] : impl_->transfer_costs) {
        if (id == adapter) {
            return cost;
        }
    }
    return std::nullopt;
}

Result<EncoderSelection> GpuTopologyService::select_for_monitor(std::uintptr_t monitor,
                                                                std::optional<double> measured_cross_adapter_ms) const {
    const AdapterInfo* owner = impl_->topology.owner_of_monitor(monitor);
    if (owner == nullptr) {
        // Never fall back to adapter 0. SPEC.md §5.1 step 4 exists because DXGI will
        // happily give you a device on the wrong adapter and then return S_OK with
        // black frames forever.
        FC_LOG_ERROR(Subsystem::Gpu, "could not determine which adapter owns the target output",
                     LogFields{}
                         .add("monitor", static_cast<std::uint64_t>(monitor))
                         .add_error(FcError::GPU_OUTPUT_OWNERSHIP_UNRESOLVED));
        return FcError::GPU_OUTPUT_OWNERSHIP_UNRESOLVED;
    }

    // The caller's figure wins when it supplied one; otherwise the cache from
    // `measure_transfer_costs`. Rule 2 needs the *worst* candidate's cost, because
    // `select_encoder` evaluates one number against the budget -- and with several
    // discrete adapters the optimistic one would let the rule fire for a destination
    // that never earned it.
    std::optional<double> cost = measured_cross_adapter_ms;
    if (!cost.has_value()) {
        for (const auto& [id, measured] : impl_->transfer_costs) {
            static_cast<void>(id);
            cost = cost.has_value() ? std::max(*cost, measured) : measured;
        }
    }

    const EncoderSelection selection = select_encoder(impl_->topology, SelectionInput{owner->id, cost});

    FC_LOG_INFO(Subsystem::Gpu, "encoder selected",
                LogFields{}
                    .add("capture_adapter", owner->description)
                    .add("capture_adapter_luid", owner->id.to_string())
                    .add("rule", to_string(selection.rule))
                    .add("encode_adapter", selection.adapter.has_value() ? selection.adapter->to_string() : "software")
                    .add("rationale", selection.rationale));
    return selection;
}

Result<EncoderSelection>
GpuTopologyService::select_for_primary_display(std::optional<double> measured_cross_adapter_ms) const {
    const AdapterInfo* primary = impl_->topology.primary_display_adapter();
    if (primary == nullptr) {
        return FcError::GPU_OUTPUT_OWNERSHIP_UNRESOLVED;
    }
    for (const OutputInfo& output : primary->outputs) {
        if (output.attached_to_desktop && output.desktop_left == 0 && output.desktop_top == 0) {
            return select_for_monitor(output.monitor, measured_cross_adapter_ms);
        }
    }
    return FcError::GPU_OUTPUT_OWNERSHIP_UNRESOLVED;
}

void GpuTopologyService::log_topology() const {
    const Topology& topology = impl_->topology;
    FC_LOG_INFO(Subsystem::Gpu, "adapter enumeration",
                LogFields{}.add("count", static_cast<std::int64_t>(topology.adapters.size())));

    for (std::size_t i = 0; i < topology.adapters.size(); ++i) {
        const AdapterInfo& adapter = topology.adapters[i];
        FC_LOG_INFO(Subsystem::Gpu, "adapter",
                    LogFields{}
                        .add("index", static_cast<std::int64_t>(i))
                        .add("description", adapter.description)
                        .add("luid", adapter.id.to_string())
                        .add("vendor", vendor_name(adapter.vendor_id))
                        .add("device_id", static_cast<std::uint64_t>(adapter.device_id))
                        .add("class", to_string(adapter.adapter_class))
                        .add("driver_version", adapter.driver_version)
                        .add("dedicated_vram_bytes", adapter.dedicated_video_memory)
                        .add("shared_memory_bytes", adapter.shared_system_memory)
                        .add("outputs", static_cast<std::int64_t>(adapter.outputs.size()))
                        .add("drives_display", adapter.drives_display())
                        .add("h264_encode", adapter.encode.h264)
                        .add("encoder", adapter.encode.encoder_name)
                        .add("encoder_detail", adapter.encode.detail));

        for (const OutputInfo& output : adapter.outputs) {
            FC_LOG_INFO(Subsystem::Gpu, "output",
                        LogFields{}
                            .add("adapter", adapter.description)
                            .add("device_name", output.device_name)
                            .add("monitor", static_cast<std::uint64_t>(output.monitor))
                            .add("width", output.width())
                            .add("height", output.height())
                            .add("attached", output.attached_to_desktop));
        }
    }
}

} // namespace fc::gpu

namespace fc {

void fill_preamble_from_topology(SessionPreamble& preamble, const gpu::Topology& topology) {
    preamble.adapters.clear();
    std::string layout;

    for (const gpu::AdapterInfo& adapter : topology.adapters) {
        AdapterSummary summary;
        summary.description = adapter.description;
        summary.driver_version = adapter.driver_version;
        summary.vendor_id = adapter.vendor_id;
        summary.dedicated_video_memory = adapter.dedicated_video_memory;
        summary.owns_target_output = adapter.drives_display();
        preamble.adapters.push_back(std::move(summary));

        for (const gpu::OutputInfo& output : adapter.outputs) {
            if (!layout.empty()) {
                layout += ", ";
            }
            layout += output.device_name + " " + std::to_string(output.width()) + "x" +
                      std::to_string(output.height()) + " on " + adapter.description;
        }
    }

    preamble.display_topology = layout.empty() ? "no attached outputs" : layout;
}

} // namespace fc
