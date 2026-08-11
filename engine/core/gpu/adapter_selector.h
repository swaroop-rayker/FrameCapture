#pragma once

#include "core/gpu/adapter_info.h"

#include <optional>
#include <string>

namespace fc::gpu {

/// Which clause of SPEC.md §5.2 produced the answer. Logged and surfaced, because
/// "why is it encoding on the slow GPU" is a question users and maintainers both
/// ask, and the answer is always one of these four.
enum class SelectionRule {
    /// Rule 1: the capture adapter can encode. Zero-copy, no PCIe transfer.
    CaptureAdapter,
    /// Rule 2: a discrete adapter can encode and measured transfer cost fits the
    /// budget.
    DiscreteWithTransfer,
    /// Rule 3: the integrated adapter can encode.
    IntegratedFallback,
    /// Rule 4: no hardware encoder anywhere.
    SoftwareFallback,
};

[[nodiscard]] std::string_view to_string(SelectionRule rule) noexcept;

/// SPEC.md §5.2 rule 2: cross-adapter transfer is only worth it below this.
inline constexpr double kCrossAdapterBudgetMs = 2.0;

struct SelectionInput {
    /// The adapter that owns the capture target's output. Frames already live in
    /// its memory.
    AdapterId capture_adapter;

    /// Measured cross-adapter transfer cost, in milliseconds per frame.
    ///
    /// `std::nullopt` means "not measured yet", which is the state until the
    /// transfer path exists (SPEC.md §5.3, M2/M3). Rule 2 requires a *measured*
    /// cost below the budget, so an unmeasured cost cannot satisfy it -- the spec
    /// says "cross-adapter transfer cost is measured < 2.0 ms/frame", and assuming
    /// a value would be exactly the "always use the fast GPU" mistake §5.2 exists
    /// to prevent.
    std::optional<double> measured_cross_adapter_ms;
};

struct EncoderSelection {
    /// The adapter to encode on. `std::nullopt` means software encoding, which has
    /// no adapter.
    std::optional<AdapterId> adapter;
    SelectionRule rule = SelectionRule::SoftwareFallback;
    /// Human-readable justification, for the log and the GUI status panel.
    std::string rationale;
};

/// Applies SPEC.md §5.2's ladder, in order, and reports which rung fired.
///
/// Deliberately a pure function over already-discovered facts: it needs no GPU, so
/// every topology that matters -- including the MUX-less laptop this project exists
/// to handle -- is exercised on the CPU test tier.
[[nodiscard]] EncoderSelection select_encoder(const Topology& topology, const SelectionInput& input);

} // namespace fc::gpu
