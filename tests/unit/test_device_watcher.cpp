// SPEC.md §5.4's device-loss classification, without a GPU.
//
// CPU TIER. §5.4 says of the device-removed path: "call `GetDeviceRemovedReason()`,
// **log the specific HRESULT** (`DXGI_ERROR_DEVICE_HUNG` vs `DRIVER_INTERNAL_ERROR`
// vs `ACCESS_LOST` are different bugs)". That sentence is the whole reason this file
// exists: the difference between those codes decides whether the right response is
// to rebuild the device, rebuild only the capture session, or go looking for a bug
// in our own command stream — and a mapping that quietly collapses them produces a
// recording that migrates when it should have recovered, or worse, the reverse.
//
// Every one of these paths is reachable only when a driver actually falls over,
// which is not something the GPU tier can arrange on demand either. So the
// classification is a pure function over two integers and every branch is asserted
// here, on the tier that runs anywhere.
//
// The HRESULT values are written as literals rather than taken from `<dxgi.h>` on
// purpose. Importing the header would make this test agree with the engine by
// construction even if both were wrong; the literals are from the DXGI
// documentation, so a typo in the engine's constant fails here.

#include "core/gpu/device_watcher.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using fc::FcError;
using fc::gpu::AdapterId;
using fc::gpu::DeviceChange;
using fc::gpu::DeviceChangeReport;

// DXGI error codes, from the documented values.
constexpr auto kDeviceRemoved = static_cast<std::int32_t>(0x887A0005);
constexpr std::int32_t kDeviceReset = static_cast<std::int32_t>(0x887A0007);
constexpr std::int32_t kDeviceHung = static_cast<std::int32_t>(0x887A0006);
constexpr std::int32_t kDriverInternalError = static_cast<std::int32_t>(0x887A0020);
constexpr std::int32_t kAccessLost = static_cast<std::int32_t>(0x887A0026);
constexpr std::int32_t kInvalidCall = static_cast<std::int32_t>(0x887A0001);
constexpr std::int32_t kWaitTimeout = static_cast<std::int32_t>(0x887A0027);
constexpr std::int32_t kOk = 0;

// ---------------------------------------------------------------------------
// The three that §5.4 insists are different bugs
// ---------------------------------------------------------------------------

TEST(DeviceWatcher, DeviceRemovedAndDeviceHungAndAccessLostAreThreeDifferentAnswers) {
    // A removed adapter: the hardware or its driver is gone. Rebuild everything and
    // re-select, because the adapter we were using may not exist any more.
    const DeviceChangeReport removed = fc::gpu::classify_device_error(kDeviceRemoved);
    EXPECT_EQ(removed.change, DeviceChange::DeviceRemoved);
    EXPECT_EQ(removed.error, FcError::GPU_DEVICE_REMOVED);
    EXPECT_TRUE(removed.requires_device_rebuild);

    // A hung device: the GPU stopped responding, usually because of a bad command
    // stream. Still a device rebuild, but a *different* error code, because this one
    // points at us and the other points at the driver.
    const DeviceChangeReport hung = fc::gpu::classify_device_error(kDeviceRemoved, kDeviceHung);
    EXPECT_EQ(hung.change, DeviceChange::DeviceRemoved);
    EXPECT_EQ(hung.error, FcError::GPU_DEVICE_HUNG)
        << "a hung device reported as a plain removal loses the one clue that says the fault is ours";
    EXPECT_TRUE(hung.requires_device_rebuild);
    EXPECT_EQ(hung.removed_reason, kDeviceHung);

    // Access lost: the *session* lost its surface. The device is fine, and rebuilding
    // it would be an expensive answer to a cheap problem -- this happens on an
    // ordinary desktop-mode switch.
    const DeviceChangeReport lost = fc::gpu::classify_device_error(kAccessLost);
    EXPECT_EQ(lost.change, DeviceChange::AccessLost);
    EXPECT_EQ(lost.error, FcError::CAPTURE_TARGET_GONE);
    EXPECT_FALSE(lost.requires_device_rebuild)
        << "access loss must not trigger a device rebuild; it is a session-level fault";

    // All three are distinct, which is the property §5.4 actually asks for.
    EXPECT_NE(removed.error, hung.error);
    EXPECT_NE(removed.change, lost.change);
}

TEST(DeviceWatcher, ADriverInternalErrorIsReportedAsItsOwnFault) {
    const DeviceChangeReport report = fc::gpu::classify_device_error(kDeviceRemoved, kDriverInternalError);
    EXPECT_EQ(report.change, DeviceChange::DeviceRemoved);
    EXPECT_TRUE(report.requires_device_rebuild);
    EXPECT_EQ(report.removed_reason, kDriverInternalError);
    // Distinguished from a hang: same response, different diagnosis, and the log line
    // is the only place that distinction survives to reach a bug report.
    EXPECT_NE(report.error, FcError::GPU_DEVICE_HUNG);
}

TEST(DeviceWatcher, DeviceResetIsSeparateFromDeviceRemoved) {
    const DeviceChangeReport report = fc::gpu::classify_device_error(kDeviceReset);
    EXPECT_EQ(report.change, DeviceChange::DeviceReset);
    EXPECT_EQ(report.error, FcError::GPU_DEVICE_RESET);
    EXPECT_TRUE(report.requires_device_rebuild);
}

// ---------------------------------------------------------------------------
// The negative space -- what must *not* be treated as device loss
// ---------------------------------------------------------------------------

TEST(DeviceWatcher, SuccessIsNotAChange) {
    const DeviceChangeReport report = fc::gpu::classify_device_error(kOk);
    EXPECT_FALSE(report.changed());
    EXPECT_EQ(report.change, DeviceChange::None);
    EXPECT_EQ(report.error, FcError::NONE);
    EXPECT_FALSE(report.requires_device_rebuild);
}

TEST(DeviceWatcher, OrdinaryFailuresAreNotDeviceLoss) {
    // The distinction that matters operationally: a bug in our own call, or a frame
    // that simply was not ready, must not tear down and rebuild the entire device
    // stack. `DXGI_ERROR_WAIT_TIMEOUT` in particular is the *normal* return from a
    // duplication acquire on an idle desktop -- treating it as device loss would
    // migrate the GPU every time nothing was happening on screen.
    for (const std::int32_t hr : {kInvalidCall, kWaitTimeout}) {
        const DeviceChangeReport report = fc::gpu::classify_device_error(hr);
        EXPECT_FALSE(report.changed()) << "HRESULT 0x" << std::hex << hr << " was classified as a device change";
        EXPECT_FALSE(fc::gpu::is_device_lost(hr));
    }
}

TEST(DeviceWatcher, IsDeviceLostAgreesWithTheClassifier) {
    // Two entry points, one set. They drifted apart in every codebase that has had
    // both, so the agreement is asserted rather than assumed.
    for (const std::int32_t hr :
         {kDeviceRemoved, kDeviceReset, kAccessLost, kInvalidCall, kWaitTimeout, kOk, kDeviceHung}) {
        const DeviceChangeReport report = fc::gpu::classify_device_error(hr);
        EXPECT_EQ(fc::gpu::is_device_lost(hr), report.changed()) << "disagreement on HRESULT 0x" << std::hex << hr;
    }
}

// ---------------------------------------------------------------------------
// Severity ordering
// ---------------------------------------------------------------------------

TEST(DeviceWatcher, TheChangeKindsAreOrderedBySeverity) {
    // The enum's numeric order is load-bearing: the watcher folds a reported error
    // and a poll result together by taking the more severe, and "more severe" is
    // `>`. If someone reorders the enumerators for tidiness, an access-loss would
    // start outranking a device removal.
    EXPECT_LT(static_cast<int>(DeviceChange::None), static_cast<int>(DeviceChange::TopologyChanged));
    EXPECT_LT(static_cast<int>(DeviceChange::TopologyChanged), static_cast<int>(DeviceChange::AccessLost));
    EXPECT_LT(static_cast<int>(DeviceChange::AccessLost), static_cast<int>(DeviceChange::DeviceReset));
    EXPECT_LT(static_cast<int>(DeviceChange::DeviceReset), static_cast<int>(DeviceChange::DeviceRemoved));
}

TEST(DeviceWatcher, ChangeNamesAreStableAndGreppable) {
    // SPEC.md §18: these reach the log, and §5.4 requires a `GPU_MIGRATION` event
    // that names what provoked it.
    EXPECT_EQ(fc::gpu::to_string(DeviceChange::None), "none");
    EXPECT_EQ(fc::gpu::to_string(DeviceChange::TopologyChanged), "topology_changed");
    EXPECT_EQ(fc::gpu::to_string(DeviceChange::AccessLost), "access_lost");
    EXPECT_EQ(fc::gpu::to_string(DeviceChange::DeviceReset), "device_reset");
    EXPECT_EQ(fc::gpu::to_string(DeviceChange::DeviceRemoved), "device_removed");
}

// ---------------------------------------------------------------------------
// Adapter-set diffing
// ---------------------------------------------------------------------------

TEST(DeviceWatcher, AnIdenticalAdapterSetIsNotAChange) {
    const std::array<AdapterId, 2> before{AdapterId{0x1111}, AdapterId{0x2222}};
    const std::array<AdapterId, 2> after{AdapterId{0x1111}, AdapterId{0x2222}};
    EXPECT_FALSE(fc::gpu::adapter_sets_differ(before, after));
}

TEST(DeviceWatcher, ReorderingIsNotAChange) {
    // The case this property exists for. DXGI's enumeration order is not stable
    // across a driver restart, which is precisely the event being detected -- so a
    // sequence comparison would report a change every time the same two adapters came
    // back in the other order, and every recording on a hybrid laptop would migrate
    // for no reason.
    const std::array<AdapterId, 2> before{AdapterId{0x1111}, AdapterId{0x2222}};
    const std::array<AdapterId, 2> after{AdapterId{0x2222}, AdapterId{0x1111}};
    EXPECT_FALSE(fc::gpu::adapter_sets_differ(before, after));
}

TEST(DeviceWatcher, AnAdapterAppearingOrDisappearingIsAChange) {
    const std::array<AdapterId, 2> both{AdapterId{0x1111}, AdapterId{0x2222}};
    const std::array<AdapterId, 1> one{AdapterId{0x1111}};

    EXPECT_TRUE(fc::gpu::adapter_sets_differ(both, one)) << "an adapter disappeared and the diff missed it";
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(one, both)) << "an adapter appeared and the diff missed it";

    // A same-sized set with a different member -- the case a count comparison misses,
    // and the one a driver restart that renumbers a LUID actually produces.
    const std::array<AdapterId, 2> swapped{AdapterId{0x1111}, AdapterId{0x3333}};
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(both, swapped))
        << "the sets are the same size and different; a count-based diff would pass this";
}

TEST(DeviceWatcher, AnEmptySetIsHandledWithoutSpecialCasing) {
    const std::vector<AdapterId> none;
    const std::array<AdapterId, 1> one{AdapterId{0x1111}};
    EXPECT_FALSE(fc::gpu::adapter_sets_differ(none, none));
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(none, one));
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(one, none));
}

TEST(DeviceWatcher, DuplicateIdsDoNotMaskAMissingAdapter) {
    // Defensive: DXGI should never report the same LUID twice, but a set-difference
    // written as "every element of A is in B" would call {1,1} and {1,2} equal in one
    // direction. Asserted so the implementation cannot take that shortcut.
    const std::array<AdapterId, 2> duplicated{AdapterId{0x1111}, AdapterId{0x1111}};
    const std::array<AdapterId, 2> distinct{AdapterId{0x1111}, AdapterId{0x2222}};
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(duplicated, distinct));
    EXPECT_TRUE(fc::gpu::adapter_sets_differ(distinct, duplicated));
}

// ---------------------------------------------------------------------------
// The budget SPEC.md §5.4 sets
// ---------------------------------------------------------------------------

TEST(DeviceWatcher, TheSpecifiedBudgetsAreWhatTheSpecSays) {
    // Pinned so a future edit to either constant has to be deliberate. Row 11's
    // assertion is written against `kMigrationGapBudgetNs`, so silently raising it
    // would silently weaken the row.
    EXPECT_EQ(fc::gpu::kMigrationGapBudgetNs, 350'000'000) << "SPEC.md §5.4: total gap < 350 ms";
    EXPECT_EQ(fc::gpu::kPollIntervalNs, 500'000'000) << "SPEC.md §5.4: background poll at 2 Hz";

    // Derived from the poll interval rather than chosen. If the two ever diverge it
    // should be because someone decided they should, not because one was edited.
    EXPECT_EQ(fc::gpu::kReportSettleNs, fc::gpu::kPollIntervalNs)
        << "BUG-035: the settling window covers one watchdog tick";
}

// ---------------------------------------------------------------------------
// BUG-035: coalescing a burst of device errors
// ---------------------------------------------------------------------------

// One fault, many reporters. `DXGI_ERROR_DEVICE_REMOVED` reaches every thread that
// touches the dead device, so the reports arrive spread over however long each of
// them takes to make its next D3D call. Before the settling window, whether the burst
// became one rebuild or two depended on whether the rebuild finished before the last
// report landed — measured at 2 rebuilds in 6 of 22 runs.
//
// This is the mechanism on the CPU tier, where the timing is the test's to set rather
// than the driver's.
TEST(DeviceWatcher, ReportsArrivingJustAfterARebuildDescribeTheDeviceItReplaced) {
    fc::gpu::DeviceWatcher watcher;
    ASSERT_TRUE(watcher.start().has_value());

    // The fault, and the response to it.
    watcher.report_device_error(kDeviceRemoved);
    EXPECT_EQ(watcher.poll().change, fc::gpu::DeviceChange::DeviceRemoved);
    watcher.acknowledge();

    // The stragglers: same fault, same dead device, noticed late.
    watcher.report_device_error(kDeviceRemoved);
    watcher.report_device_error(kDeviceRemoved);

    EXPECT_EQ(watcher.settled_reports(), 2u) << "late reports about the replaced device were not recognised as such";

    // `poll` re-baselines DXGI as a side effect, so this asserts only that no *device*
    // change is reported. A topology change can legitimately appear here: creating and
    // discarding factories is exactly what makes `IsCurrent` false, which is BUG-029's
    // territory and is handled by `acknowledge` re-baselining above.
    const fc::gpu::DeviceChangeReport after = watcher.poll();
    EXPECT_NE(after.change, fc::gpu::DeviceChange::DeviceRemoved)
        << "a second rebuild would be provoked for a fault that has already been repaired";
}

// The other half, and the one that keeps the window from being a way to lose faults:
// a report that is not inside a settling window is acted on exactly as before.
TEST(DeviceWatcher, AReportOutsideASettlingWindowIsActedOn) {
    fc::gpu::DeviceWatcher watcher;
    ASSERT_TRUE(watcher.start().has_value());

    // No acknowledge has ever been called, so nothing is settling.
    watcher.report_device_error(kDeviceRemoved);
    EXPECT_EQ(watcher.settled_reports(), 0u);
    EXPECT_EQ(watcher.poll().change, fc::gpu::DeviceChange::DeviceRemoved);
}

} // namespace
