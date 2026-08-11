// Shared entry point for the GPU tier.
//
// DPI awareness must be established before any DXGI or capture call, and a gtest
// fixture's SetUp is already too late for tests that ran earlier in the binary.
// Doing it in main is the only place that is reliably first.
//
// The same is true of the display's power state, for a reason that cost two
// diagnoses to find — see below.

#include "core/util/dpi_awareness.h"

#include <windows.h>

#include <gtest/gtest.h>

namespace {

/// Keeps the display powered for as long as this process runs (BUG-033).
///
/// **This tier cannot capture a display that Windows has turned off.** DWM stops
/// compositing, so WGC delivers nothing, and `IDXGIOutput1::DuplicateOutput` fails
/// outright — which surfaces as `DDA_UNSUPPORTED` or a start failure rather than as
/// anything mentioning power. Measured on this rig: the AC display timeout is
/// **600 s**, and a three-preset tier run takes about 25 minutes, so an unattended
/// run reliably crosses it partway through and everything that touches a real output
/// fails from that point on.
///
/// That is the whole of BUG-033. It explains the symptoms that the "ambient content"
/// diagnosis could not: two of the original five failures were `DuplicateOutput`
/// refusing to open, which no amount of screen *content* affects. It explains why
/// deliberate screen activity fixed it — moving a window means someone was at the
/// keyboard, which resets the idle timer. It explains why it correlated with slow,
/// heavily-loaded runs: those are the long ones. And it explains the observation the
/// entry recorded as "not proven", that the same binary passed twice earlier the same
/// day and failed once: the failing run was the unattended one.
///
/// `ES_CONTINUOUS` makes the request stick until it is cleared rather than applying
/// once; the display and system flags are what the tier actually needs. Cleared on the
/// way out so the machine's normal power behaviour resumes — a test binary that leaves
/// a laptop unable to sleep is a worse bug than the one it was fixing.
class DisplayPowerRequest {
public:
    DisplayPowerRequest()
        : held_(SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) != 0) {}

    ~DisplayPowerRequest() {
        if (held_) {
            SetThreadExecutionState(ES_CONTINUOUS);
        }
    }

    DisplayPowerRequest(const DisplayPowerRequest&) = delete;
    DisplayPowerRequest& operator=(const DisplayPowerRequest&) = delete;
    DisplayPowerRequest(DisplayPowerRequest&&) = delete;
    DisplayPowerRequest& operator=(DisplayPowerRequest&&) = delete;

private:
    bool held_ = false;
};

} // namespace

int main(int argc, char** argv) {
    fc::set_process_dpi_awareness();
    const DisplayPowerRequest display_stays_on;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
