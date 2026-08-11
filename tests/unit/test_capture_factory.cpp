// Backend selection policy (SPEC.md §4.2, §4.3, §16.4 `advanced.capture_backend`).
//
// CPU TIER. The policy is a pure function over probed facts, so every combination
// is reachable here -- including the ones this machine cannot produce, such as a
// Windows build without WGC.

#include "core/capture/capture_factory.h"

#include "core/config/config.h"

#include <gtest/gtest.h>

namespace {

using fc::capture::Backend;
using fc::capture::BackendAvailability;
using fc::capture::CaptureTarget;
using fc::config::CaptureBackend;

CaptureTarget display_target() {
    CaptureTarget target;
    target.monitor = 0x1234;
    return target;
}

CaptureTarget window_target() {
    CaptureTarget target;
    target.window = 0x5678;
    return target;
}

constexpr BackendAvailability kBoth{true, true};
constexpr BackendAvailability kDdaOnly{false, true};
constexpr BackendAvailability kWgcOnly{true, false};
constexpr BackendAvailability kNeither{false, false};

// ---------------------------------------------------------------------------
// auto
// ---------------------------------------------------------------------------

// SPEC.md §4.2 makes WGC the primary: it is the only backend that captures
// reliably across hybrid-GPU boundaries, and it is tear-free because DWM
// composites the frames.
TEST(CaptureFactory, AutoPrefersWgcWhenBothAreAvailable) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Auto, display_target(), kBoth);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection.value().backend, Backend::Wgc);
    EXPECT_FALSE(selection.value().rationale.empty());
}

TEST(CaptureFactory, AutoFallsBackToDdaWhenWgcIsUnavailable) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Auto, display_target(), kDdaOnly);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection.value().backend, Backend::Dda);
}

TEST(CaptureFactory, AutoFailsWhenNothingIsAvailable) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Auto, display_target(), kNeither);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::CAPTURE_BACKEND_UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// Explicit preferences are honoured, or they fail
// ---------------------------------------------------------------------------

TEST(CaptureFactory, ExplicitWgcIsHonoured) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Wgc, display_target(), kBoth);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection.value().backend, Backend::Wgc);
}

TEST(CaptureFactory, ExplicitDdaIsHonouredEvenThoughWgcIsAvailable) {
    // The setting exists for diagnosis. Overriding it would make it useless.
    const auto selection = fc::capture::select_backend(CaptureBackend::Dda, display_target(), kBoth);
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection.value().backend, Backend::Dda);
}

// "Force WGC" quietly falling back to DDA would hide exactly the situation the
// user set it to investigate.
TEST(CaptureFactory, ExplicitWgcFailsRatherThanFallingBack) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Wgc, display_target(), kDdaOnly);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::WGC_UNSUPPORTED);
}

TEST(CaptureFactory, ExplicitDdaFailsRatherThanFallingBack) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Dda, display_target(), kWgcOnly);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::DDA_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// Window targets
// ---------------------------------------------------------------------------

TEST(CaptureFactory, AWindowTargetAlwaysUsesWgc) {
    for (const CaptureBackend preference : {CaptureBackend::Auto, CaptureBackend::Wgc}) {
        const auto selection = fc::capture::select_backend(preference, window_target(), kBoth);
        ASSERT_TRUE(selection.has_value());
        EXPECT_EQ(selection.value().backend, Backend::Wgc);
    }
}

// DDA duplicates outputs, not windows. Silently substituting a full-screen capture
// for the window the user asked for would be worse than refusing.
TEST(CaptureFactory, AWindowTargetWithExplicitDdaIsRefused) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Dda, window_target(), kBoth);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::INTERNAL_NOT_IMPLEMENTED);
}

TEST(CaptureFactory, AWindowTargetWithoutWgcIsRefused) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Auto, window_target(), kDdaOnly);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::WGC_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// Bad input
// ---------------------------------------------------------------------------

TEST(CaptureFactory, AnEmptyTargetIsRefused) {
    const auto selection = fc::capture::select_backend(CaptureBackend::Auto, CaptureTarget{}, kBoth);
    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), fc::FcError::CAPTURE_TARGET_NOT_FOUND);
}

// ---------------------------------------------------------------------------
// The config key actually drives the policy
// ---------------------------------------------------------------------------

// `advanced.capture_backend` existed in config and in CONFIG.md before anything
// consumed it. This closes that loop: a value parsed from a real TOML document
// reaches the selection policy and changes the answer.
TEST(CaptureFactory, TheConfigKeyReachesTheSelectionPolicy) {
    const auto forced_dda = fc::config::load_from_string("[advanced]\ncapture_backend = \"dda\"\n");
    ASSERT_TRUE(forced_dda.has_value());
    ASSERT_EQ(forced_dda.value().config.advanced.capture_backend, CaptureBackend::Dda);

    const auto dda_selection =
        fc::capture::select_backend(forced_dda.value().config.advanced.capture_backend, display_target(), kBoth);
    ASSERT_TRUE(dda_selection.has_value());
    EXPECT_EQ(dda_selection.value().backend, Backend::Dda);

    // And the default -- `auto` -- lands on WGC.
    const auto defaults = fc::config::load_from_string("");
    ASSERT_TRUE(defaults.has_value());
    ASSERT_EQ(defaults.value().config.advanced.capture_backend, CaptureBackend::Auto);

    const auto auto_selection =
        fc::capture::select_backend(defaults.value().config.advanced.capture_backend, display_target(), kBoth);
    ASSERT_TRUE(auto_selection.has_value());
    EXPECT_EQ(auto_selection.value().backend, Backend::Wgc);
}

TEST(CaptureFactory, EverySelectionExplainsItself) {
    // The rationale reaches the log and the GUI status panel; an empty one makes
    // "why is it using that backend" unanswerable.
    for (const auto& availability : {kBoth, kDdaOnly}) {
        const auto selection = fc::capture::select_backend(CaptureBackend::Auto, display_target(), availability);
        ASSERT_TRUE(selection.has_value());
        EXPECT_FALSE(selection.value().rationale.empty());
    }
}

} // namespace
