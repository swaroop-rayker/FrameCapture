// AAC encoding and the three-site channel-layout contract (SPEC.md §8.5,
// §20 row 17).
//
// HARDWARE TIER only because it shares the fixture style of the other
// integration tests; nothing here needs a device. The encoder is libavcodec's
// native AAC, and the assertions decode the produced stream back in-process
// exactly as M3's video tests do.
//
// SPEC.md §20 row 17 requires the layout to be asserted "in stream *and*
// container". Both are checked below, and the distinction matters: a decoder
// reads the AudioSpecificConfig first and the container second, so a file can
// carry a correct container field and still play 5.1 as stereo.

#include "core/audio/resampler.h"
#include "core/encode/aac_encoder.h"
#include "core/logging/logger.h"

#include "temp_dir.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace {

using fc::encode::AacEncoder;
using fc::encode::AudioEncoderSettings;
using fc::encode::EncodedPacket;

constexpr int kRate = 48000;

/// Layout for a channel count, using FFmpeg's defaults -- 2, 6 and 8 map to
/// stereo, 5.1 and 7.1.
AVChannelLayout layout_for(int channels) {
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    return layout;
}

/// One AAC frame of per-channel sine tones at distinct frequencies, so a decoded
/// channel can be identified by what it contains (SPEC.md §20 row 17's
/// "per-channel tone identification").
fc::ff::Frame tone_frame(const AVChannelLayout& layout, int samples, std::int64_t pts, std::int64_t phase) {
    fc::ff::Frame frame;
    if (!frame.alloc()) {
        return frame;
    }
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = kRate;
    frame->nb_samples = samples;
    av_channel_layout_copy(&frame->ch_layout, &layout);
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        frame.reset();
        return frame;
    }
    frame->pts = pts;

    for (int channel = 0; channel < layout.nb_channels; ++channel) {
        auto* plane = reinterpret_cast<float*>(frame->data[channel]);
        const double frequency = 440.0 * (channel + 1); // 440, 880, 1320, ...
        for (int i = 0; i < samples; ++i) {
            const double t = static_cast<double>(phase + i) / kRate;
            plane[i] = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * frequency * t));
        }
    }
    return frame;
}

class AudioEncodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("aac");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "aacenctest00001";
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
// Signalling sites 1 and 2 (SPEC.md §8.5)
// ---------------------------------------------------------------------------

TEST_F(AudioEncodeTest, TheEncoderCarriesTheLayoutAndProducesAnAudioSpecificConfig) {
    for (const int channels : {2, 6, 8}) {
        AudioEncoderSettings settings;
        settings.sample_rate = kRate;
        settings.layout = layout_for(channels);

        AacEncoder encoder;
        const auto opened = encoder.open(settings);
        av_channel_layout_uninit(&settings.layout);

        ASSERT_TRUE(opened.has_value()) << channels << " channels: " << fc::error_name(opened.error());

        const AVCodecContext* codec = encoder.codec_context();
        ASSERT_NE(codec, nullptr);

        // Site 1: what the encoder was told to produce.
        EXPECT_EQ(codec->ch_layout.nb_channels, channels);
        EXPECT_EQ(codec->sample_rate, kRate);
        EXPECT_EQ(codec->profile, AV_PROFILE_AAC_LOW) << "not AAC-LC";

        // Site 2: the AudioSpecificConfig. A decoder reads this before it reads
        // anything the container says, so its absence is what makes 5.1 play as
        // stereo even when the container is right.
        EXPECT_NE(codec->extradata, nullptr) << channels << " channels: no AudioSpecificConfig";
        EXPECT_GT(codec->extradata_size, 0);

        // SPEC.md §8.5's default bitrate ladder.
        const int expected_kbps = fc::audio::default_bitrate_kbps(channels);
        EXPECT_EQ(codec->bit_rate, static_cast<std::int64_t>(expected_kbps) * 1000);
    }
}

TEST_F(AudioEncodeTest, AacConsumesFixedFramesAndTheCallerNeedNotAlign) {
    AudioEncoderSettings settings;
    settings.sample_rate = kRate;
    settings.layout = layout_for(2);

    AacEncoder encoder;
    ASSERT_TRUE(encoder.open(settings).has_value());
    const int frame_size = encoder.frame_size();
    EXPECT_EQ(frame_size, 1024) << "AAC-LC frames are 1024 samples";

    int packets = 0;
    // Drains after every submit, exactly as the pipeline does. An encoder that is
    // never drained fills its internal queue and refuses input -- which is
    // backpressure rather than failure, and is reported as INTERNAL_QUEUE_FULL.
    auto drain = [&] {
        for (;;) {
            auto received = encoder.receive();
            if (!received.has_value()) {
                ADD_FAILURE() << "receive failed: " << fc::error_name(received.error());
                return;
            }
            std::optional<EncodedPacket> packet = std::move(received).value();
            if (!packet.has_value()) {
                return;
            }
            EXPECT_TRUE(packet->audio) << "an audio packet was not marked as such";
            EXPECT_TRUE(packet->keyframe) << "every AAC frame is independently decodable";
            ++packets;
        }
    };

    // Deliberately misaligned: 480 samples is a 10 ms WASAPI buffer, which is
    // never a whole number of AAC frames.
    std::int64_t phase = 0;
    for (int i = 0; i < 100; ++i) {
        const fc::ff::Frame frame = tone_frame(settings.layout, 480, phase, phase);
        ASSERT_TRUE(static_cast<bool>(frame));
        const auto consumed = encoder.submit(frame.get());
        ASSERT_TRUE(consumed.has_value()) << "buffer " << i;
        // A caller that drains after every submit never meets backpressure, so the
        // whole buffer goes in on the first call. A short return here would mean
        // the resume protocol (BUG-013) is being exercised where it should not be.
        EXPECT_EQ(consumed.value(), 480) << "buffer " << i << " was only partly consumed";
        drain();
        phase += 480;
    }
    ASSERT_TRUE(encoder.flush().has_value());
    drain();

    // 48000 samples in, 1024 per frame: 46 full frames plus the flushed remainder.
    EXPECT_GE(packets, 46);
    av_channel_layout_uninit(&settings.layout);
}

TEST_F(AudioEncodeTest, TheFinalPartialFrameIsNotDiscardedOnFlush) {
    AudioEncoderSettings settings;
    settings.sample_rate = kRate;
    settings.layout = layout_for(2);

    AacEncoder encoder;
    ASSERT_TRUE(encoder.open(settings).has_value());

    // Half a frame, then stop. Dropping it would truncate the track -- small on
    // its own, but it accumulates with everything else against SPEC.md §20 row 4.
    const fc::ff::Frame frame = tone_frame(settings.layout, 512, 0, 0);
    const auto consumed = encoder.submit(frame.get());
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed.value(), 512);
    EXPECT_EQ(encoder.frames_submitted(), 0u) << "a partial frame should not have been sent yet";

    ASSERT_TRUE(encoder.flush().has_value());
    EXPECT_EQ(encoder.frames_submitted(), 1u) << "the trailing partial frame was discarded";

    av_channel_layout_uninit(&settings.layout);
}

TEST_F(AudioEncodeTest, FlushIsIdempotent) {
    AudioEncoderSettings settings;
    settings.sample_rate = kRate;
    settings.layout = layout_for(2);

    AacEncoder encoder;
    ASSERT_TRUE(encoder.open(settings).has_value());
    EXPECT_TRUE(encoder.flush().has_value());
    // SPEC.md §10.4 requires every finalization stage to be idempotent; a second
    // null frame would otherwise be an API misuse error.
    EXPECT_TRUE(encoder.flush().has_value());
    av_channel_layout_uninit(&settings.layout);
}

TEST_F(AudioEncodeTest, AnEmptyLayoutIsRefused) {
    AudioEncoderSettings settings;
    settings.sample_rate = kRate;
    // layout left zero-initialised

    AacEncoder encoder;
    const auto opened = encoder.open(settings);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error(), fc::FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED);
}

} // namespace
