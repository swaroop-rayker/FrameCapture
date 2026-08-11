// WASAPI loopback capture and the resampler (SPEC.md §8.1, §8.3, §8.5).
//
// HARDWARE TIER. These need a real render endpoint, so they live in the `gpu`
// label alongside the other tests that need hardware -- the label means "needs
// this machine", not literally "needs a GPU".
//
// Loopback is *not* dependent on anything playing: the endpoint opens and the
// client starts regardless, and silence is the timeline's problem rather than the
// capture path's (SPEC.md §8.2). So these assert that the plumbing works, not
// that audio was audible.

#include "core/audio/loopback_capture.h"
#include "core/audio/resampler.h"
#include "core/logging/logger.h"

#include "temp_dir.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

using fc::audio::LoopbackBuffer;
using fc::audio::LoopbackCapture;
using fc::audio::MixFormat;
using fc::audio::Resampler;

class AudioCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("audiocap");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "audiocaptest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
        dir_.reset();
    }

    std::unique_ptr<fc::test::TempDir> dir_;
};

// ---------------------------------------------------------------------------
// The endpoint
// ---------------------------------------------------------------------------

TEST_F(AudioCaptureTest, OpensTheDefaultRenderEndpointInLoopback) {
    LoopbackCapture capture;
    const auto started = capture.start("", [](const LoopbackBuffer&) {});
    ASSERT_TRUE(started.has_value()) << "loopback start failed: " << fc::error_name(started.error());
    EXPECT_TRUE(capture.running());

    const MixFormat format = capture.format();
    std::cout << "[ MEASURED ] endpoint: " << capture.device_name() << " -- " << format.sample_rate << " Hz, "
              << format.channels << " ch, " << format.bits_per_sample << " bit, " << (format.is_float ? "float" : "int")
              << ", mask 0x" << std::hex << format.channel_mask << std::dec << "\n";

    // Whatever the endpoint reports has to be self-consistent, or the resampler
    // downstream will read the wrong number of bytes per frame.
    EXPECT_GT(format.sample_rate, 0);
    EXPECT_GT(format.channels, 0);
    EXPECT_LE(format.channels, 8) << "more than 7.1 is outside v1's scope (SPEC.md §8.5)";
    EXPECT_GT(format.bytes_per_frame(), 0);

    capture.stop();
    EXPECT_FALSE(capture.running());
}

// SPEC.md §8.1 requires event-driven mode; polling beats against the device
// period and generates drift. If this ever fails, the mode is unavailable on this
// Windows build and §8.1 needs revisiting -- it is not something to silently work
// around.
TEST_F(AudioCaptureTest, EventDrivenModeIsAvailable) {
    LoopbackCapture capture;
    const auto started = capture.start("", [](const LoopbackBuffer&) {});
    ASSERT_TRUE(started.has_value())
        << "event-driven loopback was refused: " << fc::error_name(started.error())
        << " -- SPEC.md §8.1 mandates it, so this is a spec/platform conflict rather than a test failure";
    capture.stop();
}

TEST_F(AudioCaptureTest, StoppingIsIdempotentAndSafeWithoutStarting) {
    LoopbackCapture capture;
    capture.stop(); // never started
    EXPECT_FALSE(capture.running());

    ASSERT_TRUE(capture.start("", [](const LoopbackBuffer&) {}).has_value());
    capture.stop();
    capture.stop();
    EXPECT_FALSE(capture.running());
}

TEST_F(AudioCaptureTest, AnUnknownDeviceIdIsRefusedRatherThanFallingBack) {
    LoopbackCapture capture;
    const auto started = capture.start("{no-such-endpoint}", [](const LoopbackBuffer&) {});
    ASSERT_FALSE(started.has_value()) << "a bogus device id silently opened something else";
    EXPECT_EQ(started.error(), fc::FcError::AUDIO_NO_RENDER_ENDPOINT);
}

// Every delivered buffer must carry a usable QPC timestamp -- SPEC.md §8.3 makes
// it the authoritative one, so a zero here would collapse the timeline onto t0.
//
// Buffers only arrive when something is playing, so this reports rather than
// requires: an unattended run on a silent machine legitimately sees none.
TEST_F(AudioCaptureTest, DeliveredBuffersCarryMonotonicQpcTimestamps) {
    std::atomic<int> count{0};
    std::atomic<std::int64_t> first_qpc{0};
    std::atomic<std::int64_t> last_qpc{0};
    std::atomic<bool> out_of_order{false};
    std::atomic<bool> zero_stamp{false};

    LoopbackCapture capture;
    ASSERT_TRUE(capture
                    .start("",
                           [&](const LoopbackBuffer& buffer) {
                               if (buffer.qpc_ns == 0) {
                                   zero_stamp.store(true);
                               }
                               const std::int64_t previous = last_qpc.exchange(buffer.qpc_ns);
                               if (previous != 0 && buffer.qpc_ns < previous) {
                                   out_of_order.store(true);
                               }
                               if (count.fetch_add(1) == 0) {
                                   first_qpc.store(buffer.qpc_ns);
                               }
                           })
                    .has_value());

    std::this_thread::sleep_for(std::chrono::seconds{2});
    capture.stop();

    std::cout << "[ MEASURED ] loopback: " << count.load() << " buffers in 2 s, " << capture.silent_buffers()
              << " silent, " << capture.discontinuities() << " discontinuities\n";

    if (count.load() == 0) {
        std::cout << "[ MEASURED ]   nothing was playing -- timestamps not exercised. This is a normal state of "
                     "the machine, not a capture failure (SPEC.md §8.2).\n";
        SUCCEED();
        return;
    }

    EXPECT_FALSE(zero_stamp.load()) << "a buffer arrived with no QPC timestamp";
    EXPECT_FALSE(out_of_order.load()) << "QPC timestamps went backwards";
    EXPECT_GT(last_qpc.load(), first_qpc.load());
}

// ---------------------------------------------------------------------------
// The resampler
// ---------------------------------------------------------------------------

TEST_F(AudioCaptureTest, ResamplerConvertsTheEndpointFormatToCanonical) {
    LoopbackCapture capture;
    ASSERT_TRUE(capture.start("", [](const LoopbackBuffer&) {}).has_value());
    const MixFormat endpoint = capture.format();
    capture.stop();

    const auto layout = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Auto, endpoint);
    ASSERT_TRUE(layout.has_value());

    Resampler resampler;
    ASSERT_TRUE(resampler.initialize(endpoint, layout.value()).has_value());
    EXPECT_EQ(resampler.output_channels(), endpoint.channels);

    // 20 ms of silence in the endpoint's own format.
    const std::size_t frames = static_cast<std::size_t>(endpoint.sample_rate) / 50;
    const std::vector<std::uint8_t> input(frames * static_cast<std::size_t>(endpoint.bytes_per_frame()), 0);

    const auto converted = resampler.convert(input.data(), static_cast<std::int64_t>(frames));
    ASSERT_TRUE(converted.has_value()) << fc::error_name(converted.error());

    const AVFrame* frame = converted.value();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->format, AV_SAMPLE_FMT_FLTP) << "output is not planar float";
    EXPECT_EQ(frame->sample_rate, 48000) << "output is not at the canonical rate";
    EXPECT_EQ(frame->ch_layout.nb_channels, endpoint.channels);
    EXPECT_GT(frame->nb_samples, 0);
}

// SPEC.md §8.5: the layout is pinned for the file's lifetime. A 7.1 endpoint
// replaced by stereo mid-recording must still produce 7.1 frames, because
// changing an AAC stream's channel count mid-file is invalid in both containers.
TEST_F(AudioCaptureTest, ThePinnedLayoutSurvivesAnEndpointFormatChange) {
    MixFormat surround;
    surround.sample_rate = 48000;
    surround.channels = 6;
    surround.bits_per_sample = 32;
    surround.is_float = true;
    surround.channel_mask = 0x3F; // 5.1

    const auto layout = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Auto, surround);
    ASSERT_TRUE(layout.has_value());

    Resampler resampler;
    ASSERT_TRUE(resampler.initialize(surround, layout.value()).has_value());
    ASSERT_EQ(resampler.output_channels(), 6);

    // The 5.1 device goes away and a 44.1 kHz stereo one takes its place.
    MixFormat stereo;
    stereo.sample_rate = 44100;
    stereo.channels = 2;
    stereo.bits_per_sample = 16;
    stereo.is_float = false;
    stereo.channel_mask = 0x3;

    ASSERT_TRUE(resampler.reconfigure_input(stereo).has_value());
    EXPECT_EQ(resampler.output_channels(), 6) << "the pinned layout followed the device; the AAC stream is now invalid";

    const std::size_t frames = 441;
    const std::vector<std::uint8_t> input(frames * static_cast<std::size_t>(stereo.bytes_per_frame()), 0);
    const auto converted = resampler.convert(input.data(), static_cast<std::int64_t>(frames));
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(converted.value()->ch_layout.nb_channels, 6);
    EXPECT_EQ(converted.value()->sample_rate, 48000);
}

TEST_F(AudioCaptureTest, AnExplicitLayoutOverridesTheEndpointDownwards) {
    MixFormat surround;
    surround.sample_rate = 48000;
    surround.channels = 8;
    surround.bits_per_sample = 32;
    surround.is_float = true;
    surround.channel_mask = 0x63F; // 7.1

    const auto stereo = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Stereo, surround);
    ASSERT_TRUE(stereo.has_value());
    EXPECT_EQ(stereo.value().nb_channels, 2) << "an explicit stereo setting followed the endpoint instead";

    // The middle of the ladder, so the rule is not "stereo wins" by accident.
    const auto surround51 = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Surround51, surround);
    ASSERT_TRUE(surround51.has_value());
    EXPECT_EQ(surround51.value().nb_channels, 6);

    const auto automatic = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Auto, surround);
    ASSERT_TRUE(automatic.has_value());
    EXPECT_EQ(automatic.value().nb_channels, 8);
}

// SPEC.md §8.5's override is **downward only** (BUG-048).
//
// Reported from real use: a recording made on this rig's stereo endpoint with 7.1
// selected produced a file Windows' own player refused the audio of -- "we can't play
// the audio ... because its encoding settings aren't supported. You can still watch the
// video." The Media Foundation AAC decoder accepts 1, 2 and 6 channels and refuses 8, so
// an eight-channel track is unplayable there whatever is in it.
//
// And there was nothing in it: the endpoint is stereo, so the eight channels were an
// up-mix of two. The pin cost 512 kbps instead of 192 and the audio track's playability,
// to carry exactly the information it started with.
TEST_F(AudioCaptureTest, ALayoutWiderThanTheEndpointFallsBackToTheEndpointsOwn) {
    MixFormat stereo_endpoint;
    stereo_endpoint.sample_rate = 48000;
    stereo_endpoint.channels = 2;
    stereo_endpoint.bits_per_sample = 32;
    stereo_endpoint.is_float = true;
    stereo_endpoint.channel_mask = 0x3; // FL | FR

    // The reported configuration, exactly.
    const auto pinned71 =
        fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Surround71, stereo_endpoint);
    ASSERT_TRUE(pinned71.has_value());
    EXPECT_EQ(pinned71.value().nb_channels, 2)
        << "a 7.1 pin on a stereo endpoint still up-mixes; the file's audio will not play in Windows' own player";
    EXPECT_EQ(pinned71.value().u.mask, AV_CH_LAYOUT_STEREO) << "the fallback did not take the endpoint's own layout";

    // 5.1 is the same mistake one rung down, and it is the one a user is *more* likely
    // to make -- six channels is a common choice and is still an up-mix from two.
    const auto pinned51 =
        fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Surround51, stereo_endpoint);
    ASSERT_TRUE(pinned51.has_value());
    EXPECT_EQ(pinned51.value().nb_channels, 2);

    // Equal is not wider, and must be left alone -- otherwise the rule would quietly
    // become "never honour an explicit setting", which is the opposite of §8.5's pin.
    MixFormat surround_endpoint;
    surround_endpoint.sample_rate = 48000;
    surround_endpoint.channels = 8;
    surround_endpoint.bits_per_sample = 32;
    surround_endpoint.is_float = true;
    surround_endpoint.channel_mask = 0x63F; // 7.1

    const auto honoured =
        fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Surround71, surround_endpoint);
    ASSERT_TRUE(honoured.has_value());
    EXPECT_EQ(honoured.value().nb_channels, 8) << "a 7.1 pin on a 7.1 endpoint was clamped; it is not wider";

    // And the bitrate follows the layout that was actually pinned, not the one asked
    // for. This is the second half of what the up-mix cost and it is easy to miss,
    // because the file plays either way.
    EXPECT_EQ(fc::audio::default_bitrate_kbps(pinned71.value().nb_channels), 192);
    EXPECT_EQ(fc::audio::default_bitrate_kbps(honoured.value().nb_channels), 512);
}

// SPEC.md §8.5's defaults, which the encoder reads when bitrate_kbps is 0.
TEST_F(AudioCaptureTest, DefaultBitratesFollowTheChannelCount) {
    EXPECT_EQ(fc::audio::default_bitrate_kbps(2), 192);
    EXPECT_EQ(fc::audio::default_bitrate_kbps(6), 384);
    EXPECT_EQ(fc::audio::default_bitrate_kbps(8), 512);
}

TEST_F(AudioCaptureTest, AnUnsupportedSampleFormatIsRefused) {
    MixFormat odd;
    odd.sample_rate = 48000;
    odd.channels = 2;
    odd.bits_per_sample = 24; // packed 24-bit: real, and not something swresample takes directly
    odd.is_float = false;
    odd.channel_mask = 0x3;

    const auto layout = fc::audio::resolve_channel_layout(fc::config::ChannelLayoutSetting::Auto, odd);
    ASSERT_TRUE(layout.has_value());

    Resampler resampler;
    const auto initialized = resampler.initialize(odd, layout.value());
    ASSERT_FALSE(initialized.has_value()) << "a 24-bit packed format was accepted; buffers would be misread";
    EXPECT_EQ(initialized.error(), fc::FcError::AUDIO_MIX_FORMAT_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// Endpoint enumeration -- SPEC.md §15.1's `get_devices`, and what §14.1 migrates to
// ---------------------------------------------------------------------------

TEST_F(AudioCaptureTest, RenderEndpointsEnumerateWithExactlyOneDefault) {
    const auto endpoints = fc::audio::enumerate_render_endpoints();
    ASSERT_TRUE(endpoints.has_value()) << fc::error_name(endpoints.error());
    ASSERT_FALSE(endpoints.value().empty());

    int defaults = 0;
    for (const fc::audio::RenderEndpoint& endpoint : endpoints.value()) {
        EXPECT_FALSE(endpoint.id.empty()) << "an endpoint with no id cannot be opened or migrated to";
        EXPECT_GT(endpoint.format.sample_rate, 0);
        EXPECT_GT(endpoint.format.channels, 0);
        defaults += endpoint.is_default ? 1 : 0;
    }
    EXPECT_EQ(defaults, 1) << "exactly one endpoint must be the eRender/eConsole default; "
                              "§14.1's OnDefaultDeviceChanged is about that one";
}

// This is a *measurement that shapes row 12's design*, not a pass/fail gate.
//
// SPEC.md §14.1's hard case is an endpoint whose format differs from the one being
// recorded: "if the new format differs (rate/channels), keep the encoder's output
// format constant and adapt via `libswresample`". Whether this rig can reproduce that
// at all decides where row 12's test has to live -- on hardware if two endpoints
// genuinely differ, or at the `AudioEncodePath` seam on the CPU tier if they do not.
//
// Asserting a difference would make the suite fail on a perfectly ordinary machine,
// so this reports and never fails.
TEST_F(AudioCaptureTest, WhetherThisRigCanReproduceAnEndpointFormatChangeIsRecorded) {
    const auto endpoints = fc::audio::enumerate_render_endpoints();
    ASSERT_TRUE(endpoints.has_value());

    std::cout << "[ MEASURED ] render endpoints: " << endpoints.value().size() << "\n";
    bool rate_differs = false;
    bool channels_differ = false;
    const fc::audio::MixFormat& first = endpoints.value().front().format;
    for (const fc::audio::RenderEndpoint& endpoint : endpoints.value()) {
        std::cout << "[ MEASURED ]   " << (endpoint.is_default ? "* " : "  ") << endpoint.format.sample_rate << " Hz, "
                  << endpoint.format.channels << "ch, mask 0x" << std::hex << endpoint.format.channel_mask << std::dec
                  << " -- " << endpoint.name << "\n";
        rate_differs = rate_differs || endpoint.format.sample_rate != first.sample_rate;
        channels_differ = channels_differ || endpoint.format.channels != first.channels;
    }

    std::cout << "[ MEASURED ]   a live migration on this rig would change rate: " << (rate_differs ? "yes" : "no")
              << ", channels: " << (channels_differ ? "yes" : "no") << "\n";
    if (!rate_differs && !channels_differ) {
        std::cout << "[ MEASURED ]   every endpoint shares one format, so a hardware migration here exercises the "
                     "same-format path only; SPEC.md §14.1's differing-format path needs the CPU-tier seam\n";
    }
    SUCCEED();
}

} // namespace
