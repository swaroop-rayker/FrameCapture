// SPEC.md §20 row 17 -- channel layout, end to end.
//
// GPU TIER.
//
// > `test_channel_layout`: encode 5.1 and 7.1, ffprobe assert layout in stream
// > *and* container; per-channel tone identification.
//
// `test_audio_encode.cpp` already asserts signalling sites 1 and 2 -- the
// `AVCodecContext` and the `AudioSpecificConfig` -- directly on the encoder. What
// it cannot assert is that either survived into a real file, and that is the
// whole failure mode: a recording can carry a perfectly correct codec context and
// still play 5.1 as stereo, because what a player reads is the container and the
// extradata inside it (SPEC.md §8.5).
//
// So this records through the whole pipeline, closes the file, reopens it, and
// asserts on what came back:
//
//   * the **container's** `ch_layout`, which is what Matroska's `Channels`
//     element and `ChannelPositions` are written from -- site 3;
//   * the layout the **decoder** derives, which on AAC comes from the
//     `AudioSpecificConfig` in `CodecPrivate` and overrides the container when
//     they disagree -- site 2, observed the way a player observes it;
//   * **which channel is which**, by putting a distinct tone in each one and
//     checking that the channel that comes back at position N is the one that went
//     in at position N. A layout can be labelled correctly and still have its
//     channels permuted, and no metadata assertion sees that.

#include "core/audio/resampler.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::pipeline::AudioSource;
using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 60;
constexpr int kRate = 48000;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kPeriodNs = 20'000'000;
constexpr std::int64_t kFramesPerBuffer = 960;
constexpr int kSeconds = 4;

/// The **LFE** channel is not a full-range channel and cannot be probed like one.
///
/// AAC band-limits it to roughly 120 Hz by design -- that is what a low-frequency
/// effects channel is -- so a mid-band tone put into it is discarded by the encoder
/// and the channel comes back empty. The first version of this test put 1600 Hz in
/// it and read the resulting silence as "channel 3 came back carrying channel 0's
/// tone", because two numbers at the noise floor compare however they like.
///
/// So the probe frequency follows the channel's *purpose*, not its index.
constexpr double kLfeFrequency = 60.0;

/// True when this channel is the band-limited LFE.
[[nodiscard]] bool is_lfe(const AVChannelLayout& layout, int index) {
    return av_channel_layout_channel_from_index(&layout, static_cast<unsigned>(index)) == AV_CHAN_LOW_FREQUENCY;
}

/// Which frequency channel `index` of `layout` carries. Full-range channels are
/// spread widely enough that AAC's joint-stereo and coupling tools cannot smear
/// one into its neighbour.
[[nodiscard]] double frequency_for(const AVChannelLayout& layout, int index) {
    static constexpr std::array kFrequencies{400.0, 700.0, 1100.0, 1900.0, 2600.0, 3300.0, 4100.0, 5000.0};
    if (is_lfe(layout, index)) {
        return kLfeFrequency;
    }
    return kFrequencies[static_cast<std::size_t>(index) % kFrequencies.size()];
}

/// The layout a mask describes, for deciding per-channel frequencies.
[[nodiscard]] AVChannelLayout layout_from(std::uint64_t mask) {
    AVChannelLayout layout{};
    av_channel_layout_from_mask(&layout, mask);
    return layout;
}

/// Interleaved float samples, one distinct tone per channel.
void render_channel_tones(const AVChannelLayout& layout, std::int64_t start_frame, std::int64_t frames,
                          std::vector<float>& out) {
    const int channels = layout.nb_channels;
    out.assign(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels), 0.0F);
    for (int channel = 0; channel < channels; ++channel) {
        const double step = 2.0 * std::numbers::pi * frequency_for(layout, channel) / kRate;
        for (std::int64_t i = 0; i < frames; ++i) {
            out[(static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)) +
                static_cast<std::size_t>(channel)] =
                static_cast<float>(0.4 * std::sin(step * static_cast<double>(start_frame + i)));
        }
    }
}

class ChannelLayoutTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("chlayout");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "chlayouttest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());

        topology_ = std::make_unique<fc::gpu::GpuTopologyService>();
        ASSERT_TRUE(topology_->refresh().has_value());

        for (const AdapterInfo& adapter : topology_->topology().adapters) {
            if (adapter.adapter_class == AdapterClass::Software || !adapter.can_encode()) {
                continue;
            }
            auto created = fc::test::device_for_adapter(adapter.id);
            if (!created.has_value()) {
                continue;
            }
            device_ = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
            encoder_name_ = adapter.encode.encoder_name;
            pool_bind_flags_ = adapter.encode.nv12_pool_bind_flags;
            break;
        }
        ASSERT_NE(device_, nullptr) << "no adapter reported a usable H.264 encoder";
    }

    static void TearDownTestSuite() {
        device_.reset();
        topology_.reset();
        fc::log::shutdown();
        dir_.reset();
    }

    /// Records `kSeconds` with an `channels`-channel endpoint, pinned to
    /// `layout_setting`.
    static std::filesystem::path record(std::uint64_t channel_mask, fc::config::ChannelLayoutSetting layout_setting,
                                        const std::string& name,
                                        fc::config::Container container = fc::config::Container::Mkv) {
        const AVChannelLayout endpoint_layout = layout_from(channel_mask);
        const int channels = endpoint_layout.nb_channels;
        std::filesystem::path output =
            dir_->path() / (name + (container == fc::config::Container::Mp4 ? ".mp4" : ".mkv"));
        const int frames = kSeconds * kFps;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value() ||
            !source.start(device_->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "the synthetic source would not start";
            return {};
        }

        PipelineSettings settings;
        settings.output = output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = container;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;
        settings.audio.source = AudioSource::External;
        settings.audio.channel_layout = layout_setting;
        settings.audio.buffer_period_ns = kPeriodNs;
        settings.audio.external_format.sample_rate = kRate;
        settings.audio.external_format.channels = channels;
        settings.audio.external_format.bits_per_sample = 32;
        settings.audio.external_format.is_float = true;
        settings.audio.external_format.channel_mask = static_cast<std::uint32_t>(channel_mask);

        VideoPipeline pipeline;
        const auto started = pipeline.start(device_->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << channels << " ch: pipeline start failed, " << fc::error_name(started.error());
            return {};
        }

        const std::int64_t epoch = 9'000'000'000LL;
        std::vector<float> scratch;
        std::int64_t next_audio_frame = 0;
        std::int64_t audio_buffers_sent = 0;
        const std::int64_t total_audio_buffers = (static_cast<std::int64_t>(kSeconds) * kNsPerSecond) / kPeriodNs;

        for (int index = 0; index < frames; ++index) {
            auto frame = source.acquire(std::chrono::milliseconds{500});
            if (!frame.has_value()) {
                ADD_FAILURE() << "the synthetic source stopped at frame " << index;
                break;
            }
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns =
                static_cast<std::uint64_t>(epoch + ((static_cast<std::int64_t>(index) * kNsPerSecond) / kFps));

            while (audio_buffers_sent < total_audio_buffers &&
                   epoch + (audio_buffers_sent * kPeriodNs) <= static_cast<std::int64_t>(stamped.qpc_ns)) {
                render_channel_tones(endpoint_layout, next_audio_frame, kFramesPerBuffer, scratch);
                next_audio_frame += kFramesPerBuffer;

                fc::audio::LoopbackBuffer buffer;
                buffer.data = reinterpret_cast<const std::uint8_t*>(scratch.data());
                buffer.frames = kFramesPerBuffer;
                buffer.bytes_per_frame = endpoint_layout.nb_channels * static_cast<int>(sizeof(float));
                buffer.qpc_ns = epoch + (audio_buffers_sent * kPeriodNs);
                pipeline.offer_audio(buffer);
                ++audio_buffers_sent;
            }

            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
        }
        source.stop();

        const auto report = pipeline.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << channels << " ch: finalization failed, " << fc::error_name(report.error());
            return {};
        }
        if (!report.value().valid) {
            ADD_FAILURE() << channels << " ch: the output did not validate, " << report.value().detail;
            return {};
        }
        return output;
    }

    /// The three assertions SPEC.md §20 row 17 names, against a finished file.
    static void assert_layout(const std::filesystem::path& output, std::uint64_t expected_mask) {
        const AVChannelLayout expected = layout_from(expected_mask);
        const int channels = expected.nb_channels;
        fc::test::DecodedMedia media;
        fc::test::decode_media(output, fc::test::DecodeOptions{false, true, false}, media);
        ASSERT_TRUE(media.opened) << media.detail;
        ASSERT_TRUE(media.audio.present) << "the file has no audio stream";
        EXPECT_EQ(media.audio.codec_name, "aac");
        EXPECT_EQ(media.audio.sample_rate, kRate);

        // Site 3: the container. Matroska writes `Channels` from this, and it is
        // what a player reads before it has decoded anything.
        EXPECT_EQ(media.audio.container_layout.nb_channels, channels)
            << "the container reports " << media.audio.container_layout.nb_channels << " channels";
        EXPECT_EQ(media.audio.container_layout.order, AV_CHANNEL_ORDER_NATIVE);
        EXPECT_EQ(media.audio.container_layout.u.mask, expected_mask)
            << "the container's channel mask is 0x" << std::hex << media.audio.container_layout.u.mask;

        // Site 2: the AudioSpecificConfig, observed through the decoder that reads
        // it. Its absence is exactly how a correct container field still plays as
        // stereo.
        EXPECT_TRUE(media.audio.has_extradata) << "the stream carries no AudioSpecificConfig";
        EXPECT_EQ(media.audio.decoded_layout.nb_channels, channels)
            << "the decoder derived " << media.audio.decoded_layout.nb_channels << " channels from the stream";

        // Per-channel tone identification. A permuted layout passes every
        // assertion above and fails only here.
        ASSERT_EQ(static_cast<int>(media.audio.planes.size()), channels);
        for (int channel = 0; channel < channels; ++channel) {
            const auto& plane = media.audio.planes[static_cast<std::size_t>(channel)];
            ASSERT_GT(plane.size(), static_cast<std::size_t>(kRate)) << "channel " << channel << " is too short";

            const double own = fc::test::tone_energy(plane, frequency_for(expected, channel), kRate);

            // A channel that came back empty is not "correct because nothing else
            // is in it either". Checked before the comparison, because two values
            // at the noise floor compare however they like.
            EXPECT_GT(own, 1e-4) << "channel " << channel << " decoded to silence";

            double strongest_other = 0.0;
            int strongest_index = -1;
            for (int other = 0; other < channels; ++other) {
                if (other == channel || is_lfe(expected, other)) {
                    continue; // the LFE tone sits below every other channel's band
                }
                const double energy = fc::test::tone_energy(plane, frequency_for(expected, other), kRate);
                if (energy > strongest_other) {
                    strongest_other = energy;
                    strongest_index = other;
                }
            }
            EXPECT_GT(own, strongest_other * 2.0)
                << "channel " << channel << " came back carrying channel " << strongest_index << "'s tone (" << own
                << " vs " << strongest_other << ")";
        }
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::unique_ptr<fc::gpu::D3dDevice> device_;
    static std::string encoder_name_;
    static std::uint32_t pool_bind_flags_;
};

std::unique_ptr<fc::test::TempDir> ChannelLayoutTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> ChannelLayoutTest::topology_;
std::unique_ptr<fc::gpu::D3dDevice> ChannelLayoutTest::device_;
std::string ChannelLayoutTest::encoder_name_;
std::uint32_t ChannelLayoutTest::pool_bind_flags_ = 0;

TEST_F(ChannelLayoutTest, FivePointOneSurvivesIntoTheFileWithItsChannelsInOrder) {
    // Recorded from an endpoint reporting the *side*-channel 5.1 that Windows also
    // offers, and expected back as the back-channel form -- the only 5.1 AAC can
    // signal without a Program Config Element. See `normalize_for_aac`.
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_5POINT1, fc::config::ChannelLayoutSetting::Surround51, "surround51");
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_5POINT1_BACK);
}

TEST_F(ChannelLayoutTest, SevenPointOneSurvivesIntoTheFileWithItsChannelsInOrder) {
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_7POINT1, fc::config::ChannelLayoutSetting::Surround71, "surround71");
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_7POINT1);
}

TEST_F(ChannelLayoutTest, StereoSurvivesIntoTheFile) {
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_STEREO, fc::config::ChannelLayoutSetting::Stereo, "stereo");
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_STEREO);
}

// The layout is *pinned* (SPEC.md §8.5): an explicit stereo setting must win over
// a 7.1 endpoint, and libswresample down-mixes into it. Changing an AAC stream's
// channel count mid-file is invalid in both containers, so the pin is what makes
// a mid-recording device change survivable at all (§14.1).
TEST_F(ChannelLayoutTest, AnExplicitSettingPinsTheLayoutRatherThanFollowingTheEndpoint) {
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_7POINT1, fc::config::ChannelLayoutSetting::Stereo, "pinned_stereo");
    ASSERT_FALSE(output.empty());

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{false, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_TRUE(media.audio.present);
    EXPECT_EQ(media.audio.container_layout.nb_channels, 2)
        << "the recording followed the endpoint instead of the pinned setting";
    EXPECT_EQ(media.audio.container_layout.u.mask, AV_CH_LAYOUT_STEREO);
    EXPECT_EQ(media.audio.decoded_layout.nb_channels, 2);
}

// `Auto` follows the endpoint, which is the default and the common case.
TEST_F(ChannelLayoutTest, AutoFollowsTheEndpointsOwnMask) {
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_5POINT1_BACK, fc::config::ChannelLayoutSetting::Auto, "auto51");
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_5POINT1_BACK);
}

// ---------------------------------------------------------------------------
// The same three sites, through MP4
// ---------------------------------------------------------------------------
//
// SPEC.md §8.5 requires the layout in the container for **both** containers, and
// MP4 signals it through a completely different mechanism from Matroska: `esds`
// and the `chnl` box rather than `Channels` and `ChannelPositions`. Asserting it
// on MKV alone leaves the MP4 half of the requirement untested, which mattered
// less while MP4 was refused outright (M3) and matters now that M5 made it a real
// output.
//
// It is also where BUG-015 would have bitten again if it had not been fixed: MP4
// and Matroska pick different stream timebases, and the audio timebase is the
// thing that defect got wrong.

TEST_F(ChannelLayoutTest, FivePointOneSurvivesIntoAnMp4) {
    const std::filesystem::path output = record(AV_CH_LAYOUT_5POINT1, fc::config::ChannelLayoutSetting::Surround51,
                                                "surround51_mp4", fc::config::Container::Mp4);
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_5POINT1_BACK);
}

TEST_F(ChannelLayoutTest, SevenPointOneSurvivesIntoAnMp4) {
    const std::filesystem::path output = record(AV_CH_LAYOUT_7POINT1, fc::config::ChannelLayoutSetting::Surround71,
                                                "surround71_mp4", fc::config::Container::Mp4);
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_7POINT1);
}

TEST_F(ChannelLayoutTest, StereoSurvivesIntoAnMp4) {
    const std::filesystem::path output =
        record(AV_CH_LAYOUT_STEREO, fc::config::ChannelLayoutSetting::Stereo, "stereo_mp4", fc::config::Container::Mp4);
    ASSERT_FALSE(output.empty());
    assert_layout(output, AV_CH_LAYOUT_STEREO);
}

} // namespace
