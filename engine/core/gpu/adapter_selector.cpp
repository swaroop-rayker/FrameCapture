#include "core/gpu/adapter_selector.h"

#include "core/logging/logger.h"

#include <algorithm>

namespace fc::gpu {
namespace {

std::string describe(const AdapterInfo& adapter) {
    return adapter.description + " (" + std::string{to_string(adapter.adapter_class)} + ", " + adapter.id.to_string() +
           ")";
}

/// Lowest-index discrete adapter that can encode. Index order is DXGI's, which puts
/// the preferred adapter first; with more than one discrete encoder that is as good
/// a tie-break as any and is at least deterministic.
const AdapterInfo* first_discrete_encoder(const Topology& topology) {
    const auto it = std::ranges::find_if(topology.adapters, [](const AdapterInfo& adapter) {
        return adapter.adapter_class == AdapterClass::Discrete && adapter.can_encode();
    });
    return it != topology.adapters.end() ? &*it : nullptr;
}

const AdapterInfo* first_integrated_encoder(const Topology& topology) {
    const auto it = std::ranges::find_if(topology.adapters, [](const AdapterInfo& adapter) {
        return adapter.adapter_class == AdapterClass::Integrated && adapter.can_encode();
    });
    return it != topology.adapters.end() ? &*it : nullptr;
}

} // namespace

std::string_view to_string(SelectionRule rule) noexcept {
    switch (rule) {
    case SelectionRule::CaptureAdapter:
        return "capture_adapter";
    case SelectionRule::DiscreteWithTransfer:
        return "discrete_with_transfer";
    case SelectionRule::IntegratedFallback:
        return "integrated_fallback";
    case SelectionRule::SoftwareFallback:
        return "software_fallback";
    }
    return "unknown";
}

EncoderSelection select_encoder(const Topology& topology, const SelectionInput& input) {
    // ---- Rule 1: encode where the pixels already live -------------------------
    //
    // The important one. The intuitive-but-wrong instinct is "always encode on the
    // discrete GPU because it is faster". On a MUX-less laptop the panel is wired
    // to the integrated adapter, so frames are already in its memory; shipping a
    // 1080p60 BGRA stream to the discrete adapter costs roughly 500 MB/s over PCIe
    // for zero quality gain.
    if (const AdapterInfo* capture = topology.find(input.capture_adapter);
        capture != nullptr && capture->can_encode()) {
        return EncoderSelection{capture->id, SelectionRule::CaptureAdapter,
                                "encoding on the capture adapter " + describe(*capture) +
                                    "; frames already live there, so no cross-adapter transfer is needed"};
    }

    // ---- Rule 2: a discrete adapter, but only if the transfer is cheap --------
    if (const AdapterInfo* discrete = first_discrete_encoder(topology); discrete != nullptr) {
        if (!input.measured_cross_adapter_ms.has_value()) {
            // Not an error, and not a reason to pick it anyway. Fall through.
            FC_LOG_DEBUG(Subsystem::Gpu, "rule 2 skipped: cross-adapter transfer cost has not been measured",
                         LogFields{}.add("candidate", discrete->description));
        } else if (*input.measured_cross_adapter_ms < kCrossAdapterBudgetMs) {
            return EncoderSelection{discrete->id, SelectionRule::DiscreteWithTransfer,
                                    "encoding on discrete adapter " + describe(*discrete) +
                                        "; measured cross-adapter transfer fits the budget"};
        } else {
            FC_LOG_DEBUG(Subsystem::Gpu, "rule 2 rejected: cross-adapter transfer exceeds the budget",
                         LogFields{}
                             .add("candidate", discrete->description)
                             .add("measured_ms", *input.measured_cross_adapter_ms)
                             .add("budget_ms", kCrossAdapterBudgetMs));
        }
    }

    // ---- Rule 3: the integrated adapter --------------------------------------
    if (const AdapterInfo* integrated = first_integrated_encoder(topology); integrated != nullptr) {
        return EncoderSelection{integrated->id, SelectionRule::IntegratedFallback,
                                "encoding on integrated adapter " + describe(*integrated) +
                                    "; the capture adapter cannot encode"};
    }

    // ---- Rule 4: software ----------------------------------------------------
    //
    // Note for whoever gets here: the software rung needs libx264, which is an open
    // licensing decision (CLAUDE.md §9). Until that is resolved this selection is
    // reachable but not yet serviceable, and the encoder factory will report
    // ENCODE_NO_HARDWARE_ENCODER.
    return EncoderSelection{std::nullopt, SelectionRule::SoftwareFallback,
                            "no adapter offers a working hardware H.264 encoder; falling back to software"};
}

} // namespace fc::gpu
