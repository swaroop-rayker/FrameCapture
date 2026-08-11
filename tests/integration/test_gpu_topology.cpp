// test_gpu_topology -- the named exit criterion for M1 (SPEC.md §24).
//
// GPU TIER. Every test here needs real adapters and a real display, and is
// registered with the `gpu` ctest label. On a machine with no GPU these do not
// silently pass -- they fail, because a green test that proves nothing is worse
// than a red one (CLAUDE.md §6).

#include "core/gpu/gpu_topology.h"

#include "core/gpu/adapter_info.h"
#include "core/gpu/adapter_selector.h"
#include "core/gpu/cross_adapter_probe.h"
#include "core/gpu/device_watcher.h"
#include "core/logging/logger.h"
#include "core/logging/session_preamble.h"
#include "adapter_device.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::gpu::GpuTopologyService;
using fc::gpu::SelectionRule;
using fc::gpu::Topology;

/// Discovery runs once for the whole suite: the encoder probe opens a real encoder
/// session per adapter, which costs a few hundred milliseconds each.
class GpuTopologyTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("gputopo");

        fc::log::Config config;
        config.directory = dir_->path();
        config.level = fc::log::Level::Debug;
        config.session_id = "gputopotest0001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        service_ = std::make_unique<GpuTopologyService>();
        const auto discovered = service_->refresh();
        ASSERT_TRUE(discovered.has_value()) << "discovery failed; this test requires a real GPU and is labelled `gpu`";
        service_->log_topology();
    }

    static void TearDownTestSuite() {
        service_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    static const Topology& topology() {
        return service_->topology();
    }

    static std::unique_ptr<GpuTopologyService> service_;
    static std::unique_ptr<fc::test::TempDir> dir_;
};

std::unique_ptr<GpuTopologyService> GpuTopologyTest::service_;
std::unique_ptr<fc::test::TempDir> GpuTopologyTest::dir_;

// ---------------------------------------------------------------------------
// "Correctly identifies both adapters" -- SPEC.md §24
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, FindsAtLeastOneHardwareAdapter) {
    ASSERT_FALSE(topology().empty());

    const auto hardware = std::ranges::count_if(topology().adapters, [](const AdapterInfo& adapter) {
        return adapter.adapter_class != AdapterClass::Software;
    });
    EXPECT_GT(hardware, 0) << "no hardware adapter found; WARP alone cannot drive this project";
}

TEST_F(GpuTopologyTest, EveryAdapterHasAnIdentityAndADescription) {
    for (const AdapterInfo& adapter : topology().adapters) {
        EXPECT_TRUE(adapter.id.valid()) << adapter.description;
        EXPECT_FALSE(adapter.description.empty());
        EXPECT_NE(adapter.driver_version, "") << adapter.description;
    }
}

TEST_F(GpuTopologyTest, AdapterLuidsAreUnique) {
    // LUID is the identity the whole subsystem keys on; a duplicate would make
    // owner_of_monitor and the capability cache both wrong.
    for (std::size_t i = 0; i < topology().adapters.size(); ++i) {
        for (std::size_t j = i + 1; j < topology().adapters.size(); ++j) {
            EXPECT_NE(topology().adapters[i].id, topology().adapters[j].id);
        }
    }
}

TEST_F(GpuTopologyTest, ClassificationIsConsistentWithReportedMemory) {
    for (const AdapterInfo& adapter : topology().adapters) {
        if (adapter.adapter_class == AdapterClass::Integrated) {
            EXPECT_LT(adapter.dedicated_video_memory, 512ull * 1024 * 1024) << adapter.description;
            EXPECT_GT(adapter.shared_system_memory, adapter.dedicated_video_memory) << adapter.description;
        }
        if (adapter.adapter_class == AdapterClass::Discrete) {
            EXPECT_GT(adapter.dedicated_video_memory, 0u) << adapter.description;
        }
    }
}

// ---------------------------------------------------------------------------
// "display ownership" -- SPEC.md §24, §5.1 steps 3 and 4
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, SomeAdapterOwnsThePrimaryDisplay) {
    const AdapterInfo* primary = topology().primary_display_adapter();
    ASSERT_NE(primary, nullptr) << "no adapter reports an output at the desktop origin";
    EXPECT_TRUE(primary->drives_display());
}

TEST_F(GpuTopologyTest, EveryAttachedOutputResolvesBackToItsOwningAdapter) {
    int attached = 0;
    for (const AdapterInfo& adapter : topology().adapters) {
        for (const fc::gpu::OutputInfo& output : adapter.outputs) {
            if (!output.attached_to_desktop) {
                continue;
            }
            ++attached;
            const AdapterInfo* owner = topology().owner_of_monitor(output.monitor);
            ASSERT_NE(owner, nullptr) << output.device_name;
            EXPECT_EQ(owner->id, adapter.id) << "monitor " << output.device_name << " resolved to the wrong adapter";
            EXPECT_GT(output.width(), 0);
            EXPECT_GT(output.height(), 0);
        }
    }
    EXPECT_GT(attached, 0) << "no attached outputs; capture has nothing to target";
}

// SPEC.md §5.1 step 3: an adapter with zero outputs is normal on Optimus and must
// remain a valid encode target rather than being filtered out.
TEST_F(GpuTopologyTest, RenderOnlyAdaptersAreKeptAsEncodeTargets) {
    for (const AdapterInfo& adapter : topology().adapters) {
        if (adapter.adapter_class == AdapterClass::Software || adapter.drives_display()) {
            continue;
        }
        SUCCEED() << "render-only adapter retained: " << adapter.description << " (can_encode=" << adapter.can_encode()
                  << ")";
    }
}

TEST_F(GpuTopologyTest, AnUnknownMonitorIsRefusedRatherThanGuessed) {
    // The failure that produces an all-black recording. It must be an error, not a
    // fallback to adapter 0.
    const auto selection = service_->select_for_monitor(0xDEADBEEF);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::GPU_OUTPUT_OWNERSHIP_UNRESOLVED);
}

// ---------------------------------------------------------------------------
// "encode capability" -- SPEC.md §24, §5.1 step 5
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, EveryHardwareAdapterWasActuallyProbed) {
    for (const AdapterInfo& adapter : topology().adapters) {
        if (adapter.adapter_class == AdapterClass::Software) {
            continue;
        }
        // The spec forbids inferring this from a vendor id table, so the flag must
        // be set by a real probe having run.
        EXPECT_TRUE(adapter.encode.probed) << adapter.description;
        EXPECT_FALSE(adapter.encode.detail.empty())
            << adapter.description << " reported no explanation for its probe result";
    }
}

TEST_F(GpuTopologyTest, AtLeastOneAdapterCanEncodeH264) {
    const auto encoders =
        std::ranges::count_if(topology().adapters, [](const AdapterInfo& adapter) { return adapter.can_encode(); });
    EXPECT_GT(encoders, 0) << "no hardware H.264 encoder found on any adapter; "
                              "the software rung needs libx264, which is an open licensing decision";
}

TEST_F(GpuTopologyTest, AProbedEncoderNamesItselfAndItsPoolBindFlags) {
    for (const AdapterInfo& adapter : topology().adapters) {
        if (!adapter.can_encode()) {
            continue;
        }
        EXPECT_FALSE(adapter.encode.encoder_name.empty()) << adapter.description;
        // BUG-001: the accepted value is probed, never assumed.
        EXPECT_NE(adapter.encode.nv12_pool_bind_flags, 0u)
            << adapter.description << " reported an encoder but no usable input pool";

        // A texture array of a video format is rejected without DECODER, on every
        // adapter tested. If this ever fires, the pool allocation strategy in the
        // encode path has to change with it.
        EXPECT_NE(adapter.encode.nv12_pool_bind_flags & D3D11_BIND_DECODER, 0u)
            << adapter.description << " accepted an encoder-input pool without D3D11_BIND_DECODER";
    }
}

// The finding that unblocked M3. If an encoder-input pool can also carry
// UNORDERED_ACCESS, the conversion shader writes NV12 straight into a pool slice
// and capture-to-encode is zero-copy; otherwise M3 must convert into its own
// texture and copy into the pool, costing a full frame copy per frame.
//
// Both reference adapters accept it. This is EXPECT rather than ASSERT because a
// different GPU may not, and that is a supported-but-slower configuration, not a
// failure -- the message is what matters when it fires.
TEST_F(GpuTopologyTest, AnEncoderInputPoolCanAlsoBeWrittenByTheConversionShader) {
    for (const AdapterInfo& adapter : topology().adapters) {
        if (!adapter.can_encode()) {
            continue;
        }
        EXPECT_TRUE(adapter.encode.nv12_pool_is_uav_writable())
            << adapter.description
            << " rejected UNORDERED_ACCESS on its NV12 encoder-input pool, so the encode path on this "
               "adapter needs a device-to-device copy between conversion and encode";
    }
}

// Guards the claim the probe order rests on: without DECODER, an NV12 texture
// array does not allocate at all. At ArraySize == 1 the same flags are fine, which
// is why a single-texture probe would have drawn the wrong conclusion.
TEST_F(GpuTopologyTest, VideoFormatTextureArraysRequireTheDecoderBindFlag) {
    for (const AdapterInfo& adapter : topology().adapters) {
        // The Microsoft Basic Render Driver has no video engine and no encoder; the
        // NV12 pool constraint is a hardware-adapter claim.
        if (adapter.adapter_class == AdapterClass::Software) {
            continue;
        }

        const auto device = fc::test::device_for_adapter(adapter.id);
        ASSERT_TRUE(device.has_value()) << adapter.description;

        auto allocate = [&](UINT array_size, UINT bind_flags) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 1920;
            desc.Height = 1080;
            desc.MipLevels = 1;
            desc.ArraySize = array_size;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = bind_flags;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            return SUCCEEDED(device.value().device()->CreateTexture2D(&desc, nullptr, &texture));
        };

        EXPECT_TRUE(allocate(1, D3D11_BIND_UNORDERED_ACCESS))
            << adapter.description << ": a single NV12 texture with UNORDERED_ACCESS alone should allocate";
        EXPECT_FALSE(allocate(4, D3D11_BIND_UNORDERED_ACCESS))
            << adapter.description << ": an NV12 texture array without DECODER should be rejected";
        EXPECT_TRUE(allocate(4, D3D11_BIND_DECODER | D3D11_BIND_UNORDERED_ACCESS))
            << adapter.description << ": an NV12 texture array with DECODER|UNORDERED_ACCESS should allocate";
    }
}

TEST_F(GpuTopologyTest, ProbeResultsAreCachedAcrossRefreshes) {
    // A second refresh must not re-open encoder sessions; the cache is keyed by
    // (LUID, driver version) per SPEC.md §5.1 step 5.
    const auto before = topology().adapters;

    const auto again = service_->refresh();
    ASSERT_TRUE(again.has_value());

    ASSERT_EQ(topology().adapters.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(topology().adapters[i].id, before[i].id);
        EXPECT_EQ(topology().adapters[i].encode.h264, before[i].encode.h264);
        EXPECT_EQ(topology().adapters[i].encode.encoder_name, before[i].encode.encoder_name);
    }
}

// ---------------------------------------------------------------------------
// Selection on real hardware -- SPEC.md §5.2
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, SelectionForThePrimaryDisplaySucceeds) {
    const auto selection = service_->select_for_primary_display();
    ASSERT_TRUE(selection.has_value());
    EXPECT_FALSE(selection.value().rationale.empty());
}

// The behaviour this whole subsystem exists for: if the display-owning adapter can
// encode, that is where we encode -- regardless of whether a faster adapter exists.
TEST_F(GpuTopologyTest, WhenTheDisplayOwningAdapterCanEncodeItIsChosen) {
    const AdapterInfo* primary = topology().primary_display_adapter();
    ASSERT_NE(primary, nullptr);

    if (!primary->can_encode()) {
        GTEST_SKIP() << "primary display adapter cannot encode; rule 1 is not applicable on this machine";
    }

    const auto selection = service_->select_for_primary_display();
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection.value().rule, SelectionRule::CaptureAdapter);
    ASSERT_TRUE(selection.value().adapter.has_value());
    EXPECT_EQ(*selection.value().adapter, primary->id)
        << "encoding was moved off the adapter that already holds the frames";
}

TEST_F(GpuTopologyTest, SelectionIsStableAcrossRepeatedCalls) {
    const auto first = service_->select_for_primary_display();
    ASSERT_TRUE(first.has_value());
    for (int i = 0; i < 5; ++i) {
        const auto again = service_->select_for_primary_display();
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(again.value().adapter, first.value().adapter);
        EXPECT_EQ(again.value().rule, first.value().rule);
    }
}

// ---------------------------------------------------------------------------
// Session preamble integration -- SPEC.md §18
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, PreambleAdapterFieldsAreFilledFromTheTopology) {
    fc::SessionPreamble preamble = fc::collect_session_preamble();
    ASSERT_TRUE(preamble.adapters.empty()) << "collect_session_preamble must not invent adapters";

    fc::fill_preamble_from_topology(preamble, topology());

    EXPECT_EQ(preamble.adapters.size(), topology().adapters.size());
    EXPECT_FALSE(preamble.display_topology.empty());
    for (const fc::AdapterSummary& summary : preamble.adapters) {
        EXPECT_FALSE(summary.description.empty());
        EXPECT_FALSE(summary.driver_version.empty());
    }
}

// ---------------------------------------------------------------------------
// Change detection primitive -- SPEC.md §5.4's first trigger (M7 consumes it)
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, AdapterSetIsCurrentImmediatelyAfterDiscovery) {
    ASSERT_TRUE(service_->refresh().has_value());
    EXPECT_FALSE(service_->adapter_set_changed())
        << "the factory reported a stale adapter set immediately after being created";
}

// ---------------------------------------------------------------------------
// Cross-adapter transfer cost -- SPEC.md §5.3, and the number §5.2 rule 2 needs
//
// Rule 2 says "cross-adapter transfer cost is **measured** < 2.0 ms/frame". Until this
// existed nothing supplied a measurement, so `measured_cross_adapter_ms` was always
// `nullopt` and rule 2 could never fire -- every selection fell through it. This is a
// *measurement*, so it reports rather than asserting a threshold: whether the number
// clears the budget is a property of the hardware, not of the engine.
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, CrossAdapterTransferCostIsMeasuredInBothDirections) {
    std::vector<const fc::gpu::AdapterInfo*> adapters;
    for (const fc::gpu::AdapterInfo& adapter : topology().adapters) {
        if (adapter.adapter_class != fc::gpu::AdapterClass::Software) {
            adapters.push_back(&adapter);
        }
    }
    if (adapters.size() < 2) {
        GTEST_SKIP() << "this rig has one hardware adapter; there is nothing to transfer between";
    }

    for (int direction = 0; direction < 2; ++direction) {
        const fc::gpu::AdapterInfo& from = *adapters[direction == 0 ? 0 : 1];
        const fc::gpu::AdapterInfo& to = *adapters[direction == 0 ? 1 : 0];

        const fc::gpu::CrossAdapterCost cost = fc::gpu::measure_cross_adapter_cost(from.id, to.id);

        std::cout << "[ MEASURED ] cross-adapter " << from.description << " -> " << to.description << ": ";
        if (!cost.supported) {
            std::cout << "unsupported (" << cost.detail << ")\n";
            // Not a failure. SPEC.md §5.3 requires the engine to "validate all of them
            // at creation and fall back rather than crash", and an adapter pair that
            // cannot share is exactly the case that fallback exists for.
            continue;
        }
        std::cout << "p99 " << cost.p99_ms << " ms, median " << cost.median_ms << " ms, worst " << cost.worst_ms
                  << " ms over " << cost.iterations << " frames (budget " << fc::gpu::kCrossAdapterBudgetMs << " ms)\n"
                  << "[ MEASURED ]   §5.2 rule 2 would " << (cost.p99_ms < fc::gpu::kCrossAdapterBudgetMs ? "" : "NOT ")
                  << "fire on this pair\n";

        EXPECT_GT(cost.p99_ms, 0.0) << "a transfer that measures zero was not waited on; the timer is around the "
                                       "command submission rather than the copy";
        EXPECT_GE(cost.worst_ms, cost.median_ms);
        EXPECT_EQ(cost.iterations, 32);
    }
}

// The wiring, not the number. A measurement that never reaches `select_encoder` leaves
// rule 2 exactly as unreachable as it was before -- so this asserts that the cached
// figure is consulted, and that the *outcome* it produces matches what the figure
// implies.
TEST_F(GpuTopologyTest, TheMeasuredTransferCostReachesTheSelectionPolicy) {
    const AdapterInfo* primary = topology().primary_display_adapter();
    ASSERT_NE(primary, nullptr);
    std::uintptr_t monitor = 0;
    for (const fc::gpu::OutputInfo& output : primary->outputs) {
        if (output.attached_to_desktop) {
            monitor = output.monitor;
            break;
        }
    }
    ASSERT_NE(monitor, 0u);

    // Before measuring, nothing is cached and rule 2 cannot fire -- the state every
    // selection was in until this milestone.
    GpuTopologyService fresh;
    ASSERT_TRUE(fresh.refresh().has_value());
    for (const AdapterInfo& adapter : fresh.topology().adapters) {
        EXPECT_FALSE(fresh.transfer_cost_ms(adapter.id).has_value())
            << "a cost was cached before anything measured one";
    }

    fresh.measure_transfer_costs(monitor);

    const auto selection = fresh.select_for_monitor(monitor);
    ASSERT_TRUE(selection.has_value()) << fc::error_name(selection.error());

    // Whatever the hardware says, the rule the policy picked has to be consistent with
    // the number it was given. That is the property; the number itself is hardware.
    std::optional<double> worst;
    for (const AdapterInfo& adapter : fresh.topology().adapters) {
        if (const auto cost = fresh.transfer_cost_ms(adapter.id); cost.has_value()) {
            worst = worst.has_value() ? std::max(*worst, *cost) : *cost;
        }
    }

    std::cout << "[ MEASURED ] selection with measured transfer cost: rule "
              << fc::gpu::to_string(selection.value().rule) << ", worst measured "
              << (worst.has_value() ? std::to_string(*worst) : std::string{"none"}) << " ms\n";

    if (selection.value().rule == SelectionRule::DiscreteWithTransfer) {
        ASSERT_TRUE(worst.has_value()) << "rule 2 fired with no measurement at all";
        EXPECT_LT(*worst, fc::gpu::kCrossAdapterBudgetMs)
            << "rule 2 fired on a pair whose measured cost exceeds the budget";
    }
    // The converse is not asserted: rule 1 legitimately pre-empts rule 2 whenever the
    // capture adapter can encode, which on this rig it can.
}

TEST_F(GpuTopologyTest, AnAdapterCannotTransferToItself) {
    const fc::gpu::AdapterInfo* adapter = nullptr;
    for (const fc::gpu::AdapterInfo& candidate : topology().adapters) {
        if (candidate.adapter_class != fc::gpu::AdapterClass::Software) {
            adapter = &candidate;
            break;
        }
    }
    ASSERT_NE(adapter, nullptr);

    // Refused rather than measured as ~0 ms, because a caller that asked this question
    // has already made a mistake -- rule 1 covers the same-adapter case and never needs
    // a transfer cost. Returning a fast number would let rule 2 fire on a pair that
    // does not exist.
    const fc::gpu::CrossAdapterCost cost = fc::gpu::measure_cross_adapter_cost(adapter->id, adapter->id);
    EXPECT_FALSE(cost.supported);
    EXPECT_FALSE(cost.detail.empty());
}

// ---------------------------------------------------------------------------
// DeviceWatcher against real adapters -- SPEC.md §5.4, §3's `DeviceWatcher`
//
// The classification half is covered exhaustively on the CPU tier
// (`test_device_watcher.cpp`), because a driver falling over is not something the
// GPU tier can arrange on demand either. What needs real DXGI is the other half:
// that enumeration finds the adapters, that a quiet machine reports quiet, and that
// an error reported from another thread survives to the next poll.
// ---------------------------------------------------------------------------

TEST_F(GpuTopologyTest, TheDeviceWatcherFindsTheSameAdaptersDiscoveryDid) {
    fc::gpu::DeviceWatcher watcher;
    ASSERT_TRUE(watcher.start().has_value());

    const std::vector<fc::gpu::AdapterId> seen = watcher.adapters();
    EXPECT_EQ(seen.size(), topology().adapters.size())
        << "the watcher and the topology service disagree about how many adapters exist";

    for (const fc::gpu::AdapterInfo& adapter : topology().adapters) {
        EXPECT_NE(std::ranges::find(seen, adapter.id), seen.end())
            << "the watcher did not see adapter " << adapter.id.to_string();
    }
}

TEST_F(GpuTopologyTest, AQuietMachineReportsNoDeviceChange) {
    fc::gpu::DeviceWatcher watcher;
    ASSERT_TRUE(watcher.start().has_value());

    // Several polls, because a watcher that recreated its factory incorrectly would
    // report a change on every poll after the first -- the failure mode that would
    // make a healthy recording migrate the GPU twice a second.
    for (int i = 0; i < 5; ++i) {
        const fc::gpu::DeviceChangeReport report = watcher.poll();
        EXPECT_FALSE(report.changed()) << "poll " << i << " reported " << fc::gpu::to_string(report.change)
                                       << " on an idle machine";
    }
    EXPECT_EQ(watcher.polls(), 5u);
    EXPECT_EQ(watcher.changes(), 0u);
}

TEST_F(GpuTopologyTest, AnErrorReportedFromAnotherThreadSurfacesOnTheNextPoll) {
    fc::gpu::DeviceWatcher watcher;
    ASSERT_TRUE(watcher.start().has_value());

    // This is how a real `DXGI_ERROR_DEVICE_REMOVED` reaches the watcher: from the
    // capture or venc thread, which may not block, into the `watchdog` thread's next
    // poll. Injected here rather than provoked, because provoking it means restarting
    // a driver under a running test.
    constexpr auto kDeviceRemoved = static_cast<std::int32_t>(0x887A0005);
    constexpr auto kDeviceHung = static_cast<std::int32_t>(0x887A0006);
    watcher.report_device_error(kDeviceRemoved, kDeviceHung);

    const fc::gpu::DeviceChangeReport report = watcher.poll();
    ASSERT_TRUE(report.changed());
    EXPECT_EQ(report.change, fc::gpu::DeviceChange::DeviceRemoved);
    EXPECT_EQ(report.error, fc::FcError::GPU_DEVICE_HUNG);
    EXPECT_TRUE(report.requires_device_rebuild);

    // Consumed, not sticky. A report that persisted would make the caller migrate
    // repeatedly for one event.
    EXPECT_FALSE(watcher.poll().changed()) << "the reported error was not cleared by the poll that consumed it";
}

} // namespace
