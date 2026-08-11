#include "core/gpu/adapter_selector.h"

#include "core/gpu/adapter_info.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterId;
using fc::gpu::AdapterInfo;
using fc::gpu::EncoderSelection;
using fc::gpu::SelectionInput;
using fc::gpu::SelectionRule;
using fc::gpu::Topology;

constexpr AdapterId kIgpu{0x1111};
constexpr AdapterId kDgpu{0x2222};
constexpr AdapterId kWarp{0x3333};

AdapterInfo make_adapter(AdapterId id, std::string description, AdapterClass adapter_class, bool can_encode,
                         bool has_output) {
    AdapterInfo adapter;
    adapter.id = id;
    adapter.description = std::move(description);
    adapter.adapter_class = adapter_class;
    adapter.encode.h264 = can_encode;
    adapter.encode.probed = true;
    adapter.encode.encoder_name = can_encode ? "h264_test" : "";
    if (has_output) {
        fc::gpu::OutputInfo output;
        output.device_name = R"(\\.\DISPLAY1)";
        output.monitor = static_cast<std::uintptr_t>(id.value);
        output.attached_to_desktop = true;
        adapter.outputs.push_back(output);
    }
    return adapter;
}

/// The reference rig: AMD Radeon 780M driving the panel, RTX 4050 in render-only
/// mode with no outputs. Both can encode. SPEC.md §1.
Topology reference_rig() {
    Topology topology;
    topology.adapters.push_back(make_adapter(kIgpu, "AMD Radeon(TM) 780M", AdapterClass::Integrated, true, true));
    topology.adapters.push_back(
        make_adapter(kDgpu, "NVIDIA GeForce RTX 4050 Laptop GPU", AdapterClass::Discrete, true, false));
    return topology;
}

// ---------------------------------------------------------------------------
// Rule 1 -- the one that matters
// ---------------------------------------------------------------------------

// This is the whole point of SPEC.md §5.2. If this test ever goes green on the
// dGPU, the engine is shipping ~500 MB/s of 1080p60 BGRA across PCIe for no gain.
TEST(AdapterSelector, MuxlessLaptopEncodesOnTheDisplayOwningIgpuNotTheFasterDgpu) {
    const Topology topology = reference_rig();
    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, std::nullopt});

    ASSERT_TRUE(selection.adapter.has_value());
    EXPECT_EQ(*selection.adapter, kIgpu) << "must encode where the pixels already are";
    EXPECT_EQ(selection.rule, SelectionRule::CaptureAdapter);
    EXPECT_NE(selection.rationale.find("780M"), std::string::npos) << selection.rationale;
}

// The mirror image: an external monitor on the dGPU means the dGPU is the capture
// adapter, and rule 1 should then pick it for the same reason.
TEST(AdapterSelector, WhenTheDiscreteAdapterDrivesTheDisplayItIsAlsoRule1) {
    Topology topology = reference_rig();
    topology.adapters[1].outputs = topology.adapters[0].outputs;
    topology.adapters[0].outputs.clear();

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kDgpu, std::nullopt});

    ASSERT_TRUE(selection.adapter.has_value());
    EXPECT_EQ(*selection.adapter, kDgpu);
    EXPECT_EQ(selection.rule, SelectionRule::CaptureAdapter);
}

TEST(AdapterSelector, Rule1BeatsRule2EvenWhenTransferWouldBeCheap) {
    const Topology topology = reference_rig();
    // Even with a measured, well-inside-budget transfer cost, staying put wins.
    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, 0.1});

    EXPECT_EQ(selection.rule, SelectionRule::CaptureAdapter);
    EXPECT_EQ(*selection.adapter, kIgpu);
}

// ---------------------------------------------------------------------------
// Rule 2 -- discrete, but only on measured evidence
// ---------------------------------------------------------------------------

TEST(AdapterSelector, DiscreteIsChosenWhenTheCaptureAdapterCannotEncodeAndTransferFitsTheBudget) {
    Topology topology = reference_rig();
    topology.adapters[0].encode.h264 = false;

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, 1.5});

    ASSERT_TRUE(selection.adapter.has_value());
    EXPECT_EQ(*selection.adapter, kDgpu);
    EXPECT_EQ(selection.rule, SelectionRule::DiscreteWithTransfer);
}

// SPEC.md §5.2 says the cost must be *measured*. An unmeasured cost is not a
// licence to assume it is cheap -- that assumption is the bug the rule prevents.
TEST(AdapterSelector, DiscreteIsNotChosenOnAnUnmeasuredTransferCost) {
    Topology topology = reference_rig();
    topology.adapters[0].encode.h264 = false;

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, std::nullopt});

    EXPECT_NE(selection.rule, SelectionRule::DiscreteWithTransfer);
    EXPECT_EQ(selection.rule, SelectionRule::SoftwareFallback)
        << "with no integrated encoder either, the only honest answer is software";
}

TEST(AdapterSelector, DiscreteIsRejectedWhenTransferExceedsTheBudget) {
    Topology topology = reference_rig();
    topology.adapters[0].encode.h264 = false;

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, 2.5});

    EXPECT_NE(selection.rule, SelectionRule::DiscreteWithTransfer);
}

TEST(AdapterSelector, TheBudgetBoundaryIsExclusive) {
    Topology topology = reference_rig();
    topology.adapters[0].encode.h264 = false;

    // "< 2.0 ms" -- exactly at the budget does not qualify.
    EXPECT_NE(fc::gpu::select_encoder(topology, SelectionInput{kIgpu, fc::gpu::kCrossAdapterBudgetMs}).rule,
              SelectionRule::DiscreteWithTransfer);
    EXPECT_EQ(fc::gpu::select_encoder(topology, SelectionInput{kIgpu, fc::gpu::kCrossAdapterBudgetMs - 0.001}).rule,
              SelectionRule::DiscreteWithTransfer);
}

// ---------------------------------------------------------------------------
// Rule 3 -- integrated fallback
// ---------------------------------------------------------------------------

TEST(AdapterSelector, FallsBackToIntegratedWhenTheCaptureAdapterIsSoftware) {
    Topology topology;
    topology.adapters.push_back(
        make_adapter(kWarp, "Microsoft Basic Render Driver", AdapterClass::Software, false, true));
    topology.adapters.push_back(make_adapter(kIgpu, "AMD Radeon(TM) 780M", AdapterClass::Integrated, true, false));

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kWarp, std::nullopt});

    ASSERT_TRUE(selection.adapter.has_value());
    EXPECT_EQ(*selection.adapter, kIgpu);
    EXPECT_EQ(selection.rule, SelectionRule::IntegratedFallback);
}

TEST(AdapterSelector, IntegratedIsUsedWhenDiscreteTransferIsTooExpensive) {
    Topology topology;
    topology.adapters.push_back(make_adapter(kWarp, "WARP", AdapterClass::Software, false, true));
    topology.adapters.push_back(make_adapter(kIgpu, "iGPU", AdapterClass::Integrated, true, false));
    topology.adapters.push_back(make_adapter(kDgpu, "dGPU", AdapterClass::Discrete, true, false));

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kWarp, 9.0});

    EXPECT_EQ(selection.rule, SelectionRule::IntegratedFallback);
    EXPECT_EQ(*selection.adapter, kIgpu);
}

// ---------------------------------------------------------------------------
// Rule 4 -- software
// ---------------------------------------------------------------------------

TEST(AdapterSelector, FallsBackToSoftwareWhenNothingCanEncode) {
    Topology topology;
    topology.adapters.push_back(make_adapter(kIgpu, "iGPU", AdapterClass::Integrated, false, true));
    topology.adapters.push_back(make_adapter(kDgpu, "dGPU", AdapterClass::Discrete, false, false));

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kIgpu, 0.5});

    EXPECT_FALSE(selection.adapter.has_value());
    EXPECT_EQ(selection.rule, SelectionRule::SoftwareFallback);
}

TEST(AdapterSelector, EmptyTopologyIsSoftware) {
    const EncoderSelection selection = fc::gpu::select_encoder(Topology{}, SelectionInput{kIgpu, std::nullopt});
    EXPECT_FALSE(selection.adapter.has_value());
    EXPECT_EQ(selection.rule, SelectionRule::SoftwareFallback);
}

TEST(AdapterSelector, AnUnknownCaptureAdapterDoesNotCrashAndFallsThrough) {
    const Topology topology = reference_rig();
    // A LUID that is not in the topology -- e.g. the adapter vanished between
    // discovery and selection.
    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{AdapterId{0xDEAD}, 1.0});

    EXPECT_NE(selection.rule, SelectionRule::CaptureAdapter);
    ASSERT_TRUE(selection.adapter.has_value());
    EXPECT_EQ(*selection.adapter, kDgpu) << "rule 2 applies once rule 1 cannot";
}

// ---------------------------------------------------------------------------
// Software adapters are never an encode target
// ---------------------------------------------------------------------------

// SPEC.md §5.1 step 2: software adapters are "excluded from encode selection,
// permitted only as a last-ditch capture device".
TEST(AdapterSelector, ASoftwareAdapterIsNeverSelectedEvenIfItClaimsAnEncoder) {
    Topology topology;
    // A WARP device that somehow reports an encoder must still be refused.
    topology.adapters.push_back(make_adapter(kWarp, "WARP", AdapterClass::Software, true, true));

    const EncoderSelection selection = fc::gpu::select_encoder(topology, SelectionInput{kWarp, 0.1});

    EXPECT_FALSE(selection.adapter.has_value());
    EXPECT_EQ(selection.rule, SelectionRule::SoftwareFallback);
}

// ---------------------------------------------------------------------------
// Determinism -- SPEC.md §5.2 calls the policy "deterministic"
// ---------------------------------------------------------------------------

TEST(AdapterSelector, SelectionIsDeterministicAcrossRepeatedCalls) {
    const Topology topology = reference_rig();
    const SelectionInput input{kIgpu, 1.0};

    const EncoderSelection first = fc::gpu::select_encoder(topology, input);
    for (int i = 0; i < 20; ++i) {
        const EncoderSelection again = fc::gpu::select_encoder(topology, input);
        EXPECT_EQ(again.adapter, first.adapter);
        EXPECT_EQ(again.rule, first.rule);
        EXPECT_EQ(again.rationale, first.rationale);
    }
}

TEST(AdapterSelector, EveryRuleHasADistinctLabelAndANonEmptyRationale) {
    EXPECT_EQ(fc::gpu::to_string(SelectionRule::CaptureAdapter), "capture_adapter");
    EXPECT_EQ(fc::gpu::to_string(SelectionRule::DiscreteWithTransfer), "discrete_with_transfer");
    EXPECT_EQ(fc::gpu::to_string(SelectionRule::IntegratedFallback), "integrated_fallback");
    EXPECT_EQ(fc::gpu::to_string(SelectionRule::SoftwareFallback), "software_fallback");

    EXPECT_FALSE(fc::gpu::select_encoder(reference_rig(), SelectionInput{kIgpu, std::nullopt}).rationale.empty());
    EXPECT_FALSE(fc::gpu::select_encoder(Topology{}, SelectionInput{kIgpu, std::nullopt}).rationale.empty());
}

// ---------------------------------------------------------------------------
// Topology helpers
// ---------------------------------------------------------------------------

TEST(TopologyTest, ResolvesTheMonitorOwner) {
    const Topology topology = reference_rig();
    const AdapterInfo* owner = topology.owner_of_monitor(static_cast<std::uintptr_t>(kIgpu.value));
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->id, kIgpu);
}

// The single most consequential null in the codebase: falling back to "adapter 0"
// here produces a recording of solid black frames with S_OK everywhere
// (SPEC.md §5.1 step 4, §20 row 1).
TEST(TopologyTest, AnUnknownMonitorResolvesToNullRatherThanAGuess) {
    const Topology topology = reference_rig();
    EXPECT_EQ(topology.owner_of_monitor(0xBADF00D), nullptr);
    EXPECT_EQ(topology.owner_of_monitor(0), nullptr);
}

TEST(TopologyTest, AdapterWithNoOutputsIsStillPresentAndUsable) {
    const Topology topology = reference_rig();
    const AdapterInfo* dgpu = topology.find(kDgpu);
    ASSERT_NE(dgpu, nullptr);
    // SPEC.md §5.1 step 3: render-only is normal on Optimus, not a fault.
    EXPECT_FALSE(dgpu->drives_display());
    EXPECT_TRUE(dgpu->can_encode());
}

TEST(TopologyTest, FindReturnsNullForAnAbsentAdapter) {
    EXPECT_EQ(reference_rig().find(AdapterId{0x9999}), nullptr);
}

} // namespace
