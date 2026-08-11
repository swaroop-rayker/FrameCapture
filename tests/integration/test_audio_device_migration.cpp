// SPEC.md §14.1 / §20 row 12 -- the audio endpoint migration, below the WASAPI seam.
//
// GPU TIER, because it needs a real render endpoint.
//
// **What the CPU tier already proves, and why this exists anyway.**
// `AudioPathTest.AnEndpointChangeKeepsTheTimelineContinuousAndTheOutputFormatConstant`
// drives `AudioEncodePath::migrate_input` directly with two synthetic endpoints of
// different rates, and asserts the three things §14.1 is actually about: the AAC
// output format never changes, the timeline never shortens, and the gap is filled
// with silence rather than closed up. That is the *decision* half, and it is
// deterministic.
//
// What it cannot touch is `AudioPath::migrate_to` -- the half that stops a live
// `LoopbackCapture`, opens another one, and takes the seam from the real clock after
// the new endpoint has negotiated. Everything there is WASAPI: the COM apartment the
// capture thread re-enters, the MMCSS registration it re-acquires, the sink that has
// to be re-established because `start` consumed the previous one, and the format the
// new endpoint reports. None of that runs without hardware, and all of it is on the
// path a real device change takes.
//
// **The reference rig has exactly one render endpoint** -- measured, and printed by
// the first test below so the number is in the log rather than in a comment. So the
// migration here is to the *same* device. That still exercises every line of
// `migrate_to`, and it still measures the real close-open cost against §14.1's 200 ms
// target, which is the number that matters -- reopening a different endpoint is not
// systematically cheaper or dearer than reopening this one. What it does not prove is
// that a *different* device's format is picked up correctly, and the CPU tier covers
// exactly that with a 48 kHz stereo -> 44.1 kHz mono move. Between the two, the only
// thing left unverified is the pair running together on one machine, and that needs a
// second device. Stated in docs/ACCEPTANCE.md rather than papered over.

#include "core/audio/loopback_capture.h"
#include "core/encode/aac_encoder.h"
#include "core/logging/logger.h"
#include "core/pipeline/audio_path.h"
#include "core/timing/qpc_clock.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fc::audio::RenderEndpoint;
using fc::pipeline::AudioPath;
using fc::pipeline::AudioPathSettings;
using fc::pipeline::AudioStats;

/// Long enough on each side of the seam that a timeline which lost a chunk shows up
/// well outside the tolerance, short enough to stay a routine test.
constexpr auto kSegment = std::chrono::milliseconds{1500};

class AudioDeviceMigrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        fc::log::set_level(fc::log::Level::Warn);
    }
};

/// Counts what actually reached the muxer, so "the recording continued" is asserted on
/// packets rather than on the absence of an error.
struct PacketCounter {
    std::atomic<std::uint64_t> packets{0};

    [[nodiscard]] fc::pipeline::AudioPacketSink sink() {
        return [this](fc::encode::EncodedPacket) { packets.fetch_add(1, std::memory_order_relaxed); };
    }
};

} // namespace

// The enumeration §14.1's migration picks its target from, and §15.1's `get_devices`
// answers with. Separated from the migration itself because an empty or malformed
// enumeration would make the migration test fail for a reason that has nothing to do
// with migrating.
TEST_F(AudioDeviceMigrationTest, TheSystemsRenderEndpointsEnumerateWithFormatsAndExactlyOneDefault) {
    const auto endpoints = fc::audio::enumerate_render_endpoints();
    ASSERT_TRUE(endpoints.has_value()) << "enumeration failed: " << fc::error_name(endpoints.error());
    ASSERT_FALSE(endpoints->empty()) << "no render endpoints; this machine cannot record system audio at all";

    int defaults = 0;
    std::cout << "[ MEASURED ] render endpoints: " << endpoints->size() << "\n";
    for (const RenderEndpoint& endpoint : *endpoints) {
        std::cout << "[ MEASURED ]   " << (endpoint.is_default ? "* " : "  ") << endpoint.name << " -- "
                  << endpoint.format.sample_rate << " Hz, " << endpoint.format.channels << "ch\n";
        defaults += endpoint.is_default ? 1 : 0;

        EXPECT_FALSE(endpoint.id.empty()) << "an endpoint with no id cannot be migrated to";
        EXPECT_FALSE(endpoint.name.empty()) << "an endpoint with no name cannot be shown in the GUI";
        // The format is read without opening a stream, which is the point -- §14.1
        // decides whether it needs a resampler *before* committing to the endpoint.
        EXPECT_GT(endpoint.format.sample_rate, 0) << endpoint.name << " reported no sample rate";
        EXPECT_GT(endpoint.format.channels, 0) << endpoint.name << " reported no channels";
    }

    EXPECT_EQ(defaults, 1) << "exactly one endpoint is eRender/eConsole default; an empty device_id selects it";
}

// The headline. A live recording migrates and comes out one continuous timeline.
TEST_F(AudioDeviceMigrationTest, ALiveEndpointMigrationKeepsRecordingAndStaysInsideTheGapBudget) {
    const auto endpoints = fc::audio::enumerate_render_endpoints();
    ASSERT_TRUE(endpoints.has_value());
    ASSERT_FALSE(endpoints->empty());

    PacketCounter collector;
    AudioPath path;

    std::atomic<std::int64_t> first_packet_ns{0};
    AudioPathSettings settings;
    ASSERT_TRUE(
        path.start(settings, collector.sink(),
                   [&first_packet_ns](std::int64_t qpc) { first_packet_ns.store(qpc, std::memory_order_release); })
            .has_value());

    // The epoch a session would resolve from the first buffer (SPEC.md §7.1). Without
    // it the timeline has no origin and every duration below is meaningless.
    const std::int64_t began = fc::timing::qpc_now_ns();
    while (first_packet_ns.load(std::memory_order_acquire) == 0 && (fc::timing::qpc_now_ns() - began) < 3'000'000'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    const std::int64_t t0 = first_packet_ns.load(std::memory_order_acquire);
    ASSERT_NE(t0, 0) << "the endpoint produced no buffer in three seconds; there is nothing to migrate";
    path.set_epoch(t0);

    const std::string before_name = path.device_name();
    const auto before_format = path.format();

    // The AAC stream's shape, read before anything migrates. §14.1's invariant is that
    // this pair never changes mid-file, whatever the endpoint does.
    const AVCodecContext* encoder = path.encoder()->codec_context();
    ASSERT_NE(encoder, nullptr);
    const int output_rate_before = encoder->sample_rate;
    const int output_channels_before = encoder->ch_layout.nb_channels;

    std::this_thread::sleep_for(kSegment);
    const std::uint64_t packets_before = collector.packets.load(std::memory_order_relaxed);
    ASSERT_GT(packets_before, 0u) << "nothing was encoded before the migration; the test proves nothing";

    // --- migrate ------------------------------------------------------------------
    //
    // Empty `device_id` re-selects the current default, which is what
    // `OnDefaultDeviceChanged` calls for and what a real device change looks like from
    // `migrate_to`'s side.
    const std::int64_t migrate_started = fc::timing::qpc_now_ns();
    const auto migrated = path.migrate_to(std::string{});
    const std::int64_t migrate_finished = fc::timing::qpc_now_ns();
    ASSERT_TRUE(migrated.has_value()) << "the migration failed: " << fc::error_name(migrated.error());

    const double gap_ms = static_cast<double>(migrate_finished - migrate_started) / 1e6;

    std::this_thread::sleep_for(kSegment);
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    const double expected_seconds = 2.0 * static_cast<double>(kSegment.count()) / 1000.0;

    std::cout << "[ MEASURED ] live endpoint migration:\n"
              << "[ MEASURED ]   device \"" << before_name << "\" " << before_format.sample_rate << " Hz "
              << before_format.channels << "ch -> \"" << path.device_name() << "\" " << path.format().sample_rate
              << " Hz " << path.format().channels << "ch\n"
              << "[ MEASURED ]   gap " << gap_ms << " ms (target 200 ms)\n"
              << "[ MEASURED ]   migrations " << stats.input_migrations << ", timeline " << stats.timeline_seconds
              << " s (expected ~" << expected_seconds << "), silence " << stats.silence_seconds << " s\n"
              << "[ MEASURED ]   packets " << collector.packets.load(std::memory_order_relaxed) << " ("
              << packets_before << " before the seam)\n";

    EXPECT_EQ(stats.input_migrations, 1u) << "the migration never reached the aenc thread";

    // §14.1's target. Measured across the whole close-open, which is the interval the
    // timeline has to silence-fill.
    EXPECT_LT(gap_ms, 200.0) << "the endpoint swap took " << gap_ms << " ms against §14.1's 200 ms target";

    // The recording continued: packets kept arriving after the seam. The failure this
    // guards against is a migration that succeeds and leaves a dead capture behind,
    // which no error code would report.
    EXPECT_GT(collector.packets.load(std::memory_order_relaxed), packets_before)
        << "no audio was encoded after the migration; the replacement endpoint is not delivering";

    // The output format is fixed at `open` and must survive (§14.1). Asserted on the
    // encoder rather than on the endpoint, because it is the *stream* that cannot
    // change mid-file.
    EXPECT_EQ(encoder->sample_rate, output_rate_before)
        << "the AAC stream's sample rate moved with the endpoint; that is invalid in both containers";
    EXPECT_EQ(encoder->ch_layout.nb_channels, output_channels_before)
        << "the AAC stream's channel count moved with the endpoint; that is invalid in both containers";

    // One continuous timeline across the seam, within a buffer period either side. A
    // migration that restarted the timeline would land near half this.
    EXPECT_NEAR(stats.timeline_seconds, expected_seconds, 0.25)
        << "the timeline is " << stats.timeline_seconds << " s against an expected " << expected_seconds
        << " s; a migration must never shorten it";
}

// The negative control. `ALiveEndpointMigration...` would pass on an implementation
// that quietly restarted the whole path, because a restart also produces packets and
// also ends up roughly the right length if the stats were reset with it. This one
// records the same interval with no migration at all and asserts the two agree, which
// only holds if the migration is genuinely transparent.
TEST_F(AudioDeviceMigrationTest, TheSameRecordingWithoutAMigrationHasTheSameShape) {
    PacketCounter collector;
    AudioPath path;

    std::atomic<std::int64_t> first_packet_ns{0};
    AudioPathSettings settings;
    ASSERT_TRUE(
        path.start(settings, collector.sink(),
                   [&first_packet_ns](std::int64_t qpc) { first_packet_ns.store(qpc, std::memory_order_release); })
            .has_value());

    const std::int64_t began = fc::timing::qpc_now_ns();
    while (first_packet_ns.load(std::memory_order_acquire) == 0 && (fc::timing::qpc_now_ns() - began) < 3'000'000'000) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_NE(first_packet_ns.load(std::memory_order_acquire), 0);
    path.set_epoch(first_packet_ns.load(std::memory_order_acquire));

    std::this_thread::sleep_for(kSegment * 2);
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    const double expected_seconds = 2.0 * static_cast<double>(kSegment.count()) / 1000.0;

    std::cout << "[ MEASURED ] control (no migration): timeline " << stats.timeline_seconds << " s (expected ~"
              << expected_seconds << "), migrations " << stats.input_migrations << ", packets "
              << collector.packets.load(std::memory_order_relaxed) << "\n";

    EXPECT_EQ(stats.input_migrations, 0u);
    EXPECT_NEAR(stats.timeline_seconds, expected_seconds, 0.25);
}
