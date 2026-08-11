#pragma once

// Measuring what a cross-adapter frame transfer actually costs (SPEC.md §5.3).
//
// SPEC.md §5.2 rule 2 reads:
//
//     Else if a discrete adapter has a working HW encoder AND
//     cross-adapter transfer cost is measured < 2.0 ms/frame → use it.
//
// "measured" is the operative word, and `adapter_selector.h` has always been honest
// about it: `SelectionInput::measured_cross_adapter_ms` is a `std::optional<double>`
// documented as "not measured yet ... assuming a value would be exactly the 'always
// use the fast GPU' mistake §5.2 exists to prevent". Nothing has ever supplied one,
// so **rule 2 has never been reachable** -- every selection has fallen through it to
// rule 3 or rule 4. This is what supplies the number.
//
// ---------------------------------------------------------------------------
// What is measured, and why it is the honest figure
// ---------------------------------------------------------------------------
// §5.3's preferred path: a texture on the source adapter created with
// `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | ..._SHARED`, opened on the destination via
// `ID3D11Device1::OpenSharedResource1`, synchronised with an `IDXGIKeyedMutex`.
//
// The timer spans acquire -> copy -> release **plus a flush and a wait for the copy to
// land**, because a `CopyResource` that has only been *queued* costs nothing and tells
// you nothing. Without the wait this reports sub-microsecond times on any adapter pair
// and rule 2 fires everywhere -- which is the failure mode §5.2's emphasis exists to
// prevent, arriving through the measurement rather than through an assumption.
//
// The result is a P99 over many iterations rather than a mean. A transfer that is
// usually fast and occasionally 8 ms still drops frames at 60 fps; the mean hides that
// and the tail does not.

#include "core/error/result.h"
#include "core/gpu/adapter_info.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fc::gpu {

/// Which of SPEC.md §5.3's two paths the measurement used.
enum class TransferPath {
    /// Nothing worked. Rule 2 must not fire regardless of timing.
    None,
    /// §5.3's preferred path: shared NT handle, `OpenSharedResource1`, keyed mutex.
    /// Zero-copy -- the destination reads the source's memory directly.
    SharedTexture,
    /// §5.3's stated fallback: "staged CPU copy (`D3D11_USAGE_STAGING` + `Map`)".
    /// A full round trip through system memory, and priced accordingly.
    StagedSystemMemory,
};

[[nodiscard]] std::string_view to_string(TransferPath value) noexcept;

/// What a cross-adapter transfer costs between one pair of adapters.
struct CrossAdapterCost {
    /// True when *some* path worked. False means the pair cannot transfer at all --
    /// §5.3's "validate all of them at creation and fall back rather than crash" --
    /// and rule 2 must not fire regardless of timing.
    bool supported = false;

    /// The path the timings below describe. Reported because the two differ by an
    /// order of magnitude and "2.0 ms" means nothing without knowing which was used.
    TransferPath path = TransferPath::None;

    /// Milliseconds per 1080p frame. `p99` is what SPEC.md §5.2 rule 2 compares
    /// against `kCrossAdapterBudgetMs`; the others are for the log.
    double p99_ms = 0.0;
    double median_ms = 0.0;
    double worst_ms = 0.0;

    int iterations = 0;
    /// Why `supported` is false, for the log. Empty on success.
    std::string detail;
};

/// Measures the transfer cost from `source` to `destination` at `width` x `height`.
///
/// Costs roughly `iterations` frame copies -- about 40 ms at the default -- and is run
/// once per session during SPEC.md §5.1 discovery, not per frame. Never fails as an
/// operation: an adapter pair that cannot share is a fact, and it comes back with
/// `supported = false` rather than an error.
[[nodiscard]] CrossAdapterCost measure_cross_adapter_cost(const AdapterId& source, const AdapterId& destination,
                                                          int width = 1920, int height = 1080, int iterations = 32);

} // namespace fc::gpu
