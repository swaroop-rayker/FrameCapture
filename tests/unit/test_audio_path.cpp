// The audio path, end to end, above the WASAPI seam (SPEC.md §8, §20 row 4).
//
// CPU TIER. Everything here runs without a GPU, without an audio endpoint and
// without a COM apartment, because `AudioEncodePath` is deliberately the half of
// the audio subsystem that needs none of those (see `audio_path.h`). The device
// is a function: buffers with chosen frame counts and chosen QPC timestamps,
// which is what makes a 30-minute clock-drift run and a 65-second A/V-sync check
// something that can be asserted rather than observed.
//
// **What this does and does not cover for SPEC.md §20 row 4.** Row 4 asks for a
// beep and a flash decoded out of one muxed file. That needs a video encoder and
// therefore hardware, and it lives in `test_av_sync.cpp` on the GPU tier. What is
// asserted here is the audio half of the same measurement: that a beep generated
// at an exact second from the shared epoch comes back out of the AAC stream at
// that same instant, to within a small fraction of row 4's 20 ms. If this passes
// and row 4 fails, the fault is in the video half or the container -- which is
// most of the value of separating them.

#include "core/audio/drift_compensator.h"
#include "core/audio/resampler.h"
#include "core/encode/aac_encoder.h"
#include "core/logging/logger.h"
#include "core/pipeline/audio_path.h"
#include "core/timing/pause_clock.h"

#include "synthetic_audio.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

namespace {

using fc::audio::LoopbackBuffer;
using fc::audio::MixFormat;
using fc::pipeline::AudioEncodePath;
using fc::pipeline::AudioEncodeSettings;
using fc::pipeline::AudioStats;
using fc::test::ToneSettings;

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr std::int64_t kPeriodNs = 20'000'000; // SPEC.md §8.1
constexpr std::int64_t kFramesPerBuffer = 960; // 20 ms at 48 kHz
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// An arbitrary but realistic QPC origin -- large enough that a bug treating a
/// timestamp as an offset from zero produces an obviously wrong answer rather
/// than a plausible one.
constexpr std::int64_t kT0 = 4'321'000'000'000LL;

MixFormat stereo_float_48k() {
    MixFormat format;
    format.sample_rate = kRate;
    format.channels = kChannels;
    format.bits_per_sample = 32;
    format.is_float = true;
    format.channel_mask = 0x3; // FL | FR
    return format;
}

/// Collects what the `aenc` thread produces. The sink runs on that thread, so the
/// vector is guarded even though the test only reads it after `stop`.
class PacketCollector {
public:
    [[nodiscard]] fc::pipeline::AudioPacketSink sink() {
        return [this](fc::encode::EncodedPacket packet) {
            const std::lock_guard lock(mutex_);
            packets_.push_back(std::move(packet));
        };
    }

    [[nodiscard]] std::vector<fc::encode::EncodedPacket>& packets() {
        return packets_;
    }

private:
    std::mutex mutex_;
    std::vector<fc::encode::EncodedPacket> packets_;
};

struct DecodedAudio {
    /// Channel 0, in decode order. One value per frame.
    std::vector<float> mono;
    /// Samples libavcodec's AAC encoder pushes ahead of the signal. Present in
    /// this raw-packet decode because there is no container here to carry
    /// `CodecDelay`; the muxed path is the GPU tier's problem.
    int initial_padding = 0;
};

/// Decodes AAC packets back to PCM in-process, exactly as the muxer's validation
/// gate decodes video (SPEC.md §10.4). Nothing is asserted about a stream that
/// was never decoded.
DecodedAudio decode_packets(const AVCodecContext* encoder, std::vector<fc::encode::EncodedPacket>& packets) {
    DecodedAudio decoded;
    if (encoder == nullptr) {
        return decoded;
    }
    decoded.initial_padding = encoder->initial_padding;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        return decoded;
    }

    fc::ff::CodecContext context;
    if (!context.alloc(codec)) {
        return decoded;
    }
    context->sample_rate = encoder->sample_rate;
    if (av_channel_layout_copy(&context->ch_layout, &encoder->ch_layout) < 0) {
        return decoded;
    }

    // The AudioSpecificConfig. Without it the decoder guesses, which is the same
    // failure SPEC.md §20 row 17 is about, arriving on the read side.
    if (encoder->extradata_size > 0) {
        context->extradata = static_cast<std::uint8_t*>(
            av_mallocz(static_cast<std::size_t>(encoder->extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (context->extradata == nullptr) {
            return decoded;
        }
        std::memcpy(context->extradata, encoder->extradata, static_cast<std::size_t>(encoder->extradata_size));
        context->extradata_size = encoder->extradata_size;
    }

    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        return decoded;
    }

    fc::ff::Frame frame;
    if (!frame.alloc()) {
        return decoded;
    }

    const auto harvest = [&] {
        while (avcodec_receive_frame(context.get(), frame.get()) >= 0) {
            const int stride = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format)) != 0
                                   ? 1
                                   : frame->ch_layout.nb_channels;
            const auto* samples = reinterpret_cast<const float*>(frame->data[0]);
            for (int i = 0; i < frame->nb_samples; ++i) {
                decoded.mono.push_back(samples[static_cast<std::size_t>(i) * static_cast<std::size_t>(stride)]);
            }
            frame.unref();
        }
    };

    for (auto& packet : packets) {
        if (avcodec_send_packet(context.get(), packet.packet.get()) >= 0) {
            harvest();
        }
    }
    if (avcodec_send_packet(context.get(), nullptr) >= 0) {
        harvest();
    }
    return decoded;
}

/// Feeds one buffer of tone into the path, as WASAPI would deliver it.
class Endpoint {
public:
    Endpoint(AudioEncodePath& path, const ToneSettings& tone) : path_(&path), tone_(tone) {}

    /// One buffer whose first sample was captured at `qpc_ns`.
    void deliver(std::int64_t qpc_ns, std::int64_t frames) {
        fc::test::render_tone(tone_, next_index_, frames, scratch_);
        next_index_ += frames;

        LoopbackBuffer buffer;
        buffer.data = reinterpret_cast<const std::uint8_t*>(scratch_.data());
        buffer.frames = frames;
        // The generator writes interleaved 32-bit float, so this is the endpoint's
        // own frame size -- and it is what makes a buffer produced before a migration
        // still readable after one (BUG-031).
        buffer.bytes_per_frame = tone_.channels * static_cast<int>(sizeof(float));
        buffer.qpc_ns = qpc_ns;
        buffer.device_position_frames = device_position_;
        device_position_ += static_cast<std::int64_t>(std::llround(static_cast<double>(frames) * device_rate_scale_));
        path_->offer(buffer);
    }

    /// Makes the endpoint's own frame counter run fast or slow relative to the
    /// timestamps it reports, which is what SPEC.md §8.3's cross-check exists to
    /// notice. 1.0 is a clock that agrees with QPC.
    void set_device_rate_scale(double scale) {
        device_rate_scale_ = scale;
    }

    /// Withholds the counter, as an endpoint with no `IAudioClock2` does.
    void disable_device_clock() {
        device_position_ = 0;
        device_rate_scale_ = 0.0;
    }

    /// A buffer WASAPI flagged as silent: no pointer, but a real duration
    /// (SPEC.md §8.2).
    void deliver_silent(std::int64_t qpc_ns, std::int64_t frames) {
        next_index_ += frames;
        LoopbackBuffer buffer;
        buffer.data = nullptr;
        buffer.frames = frames;
        buffer.bytes_per_frame = tone_.channels * static_cast<int>(sizeof(float));
        buffer.qpc_ns = qpc_ns;
        buffer.flags.silent = true;
        path_->offer(buffer);
    }

    /// Aligns the generator's phase to a wall-clock instant, for the gaps in which
    /// the endpoint produced nothing at all.
    void resync_to(std::int64_t qpc_ns) {
        next_index_ = fc::test::frame_index_at(tone_, qpc_ns);
    }

private:
    AudioEncodePath* path_;
    ToneSettings tone_;
    std::int64_t next_index_ = 0;
    /// Starts well away from zero: the endpoint's counter has its own origin, and a
    /// cross-check that assumed it started at the epoch would read every recording
    /// as catastrophically drifted.
    std::int64_t device_position_ = 7'000'000;
    double device_rate_scale_ = 1.0;
    std::vector<float> scratch_;
};

class AudioPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("audiopath");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "audiopath000001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
        dir_.reset();
    }

    static AudioEncodeSettings settings() {
        AudioEncodeSettings value;
        value.input = stereo_float_48k();
        value.buffer_period_ns = kPeriodNs;
        return value;
    }

    std::unique_ptr<fc::test::TempDir> dir_;
};

// ---------------------------------------------------------------------------
// SPEC.md §8.2 -- the silence problem
// ---------------------------------------------------------------------------

// The defect SPEC.md calls the #1 loopback sync bug, asserted end to end rather
// than at the timeline's own interface: a recording whose endpoint went quiet for
// three seconds must still contain three seconds of *encoded* audio for that
// stretch, because the AAC stream's length is what the container reports.
TEST_F(AudioPathTest, ASilentStretchIsEncodedRatherThanSkipped) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // 2 s of audio, 3 s of nothing at all, 2 s more. The middle stretch produces
    // no buffers whatsoever -- that is what WASAPI loopback does, and it is the
    // whole problem.
    for (int k = 0; k < 100; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }
    for (std::int64_t now = kT0 + (2 * kNsPerSecond); now < kT0 + (5 * kNsPerSecond); now += kPeriodNs / 2) {
        if (path.silence_due_at(now)) {
            path.request_silence(now);
        }
    }
    endpoint.resync_to(kT0 + (5 * kNsPerSecond));
    for (int k = 250; k < 350; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }

    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    constexpr std::int64_t kExpectedFrames = std::int64_t{7} * kRate;
    EXPECT_NEAR(static_cast<double>(stats.frames_written), static_cast<double>(kExpectedFrames), kFramesPerBuffer)
        << "the timeline is " << (stats.frames_written - kExpectedFrames) << " frames away from wall clock";
    EXPECT_NEAR(static_cast<double>(stats.silence_frames_injected), 3.0 * kRate, kFramesPerBuffer);
    EXPECT_GT(stats.silence_ticks, 0u) << "the watchdog never fired during a three-second gap";

    // The encoded stream, not just the bookkeeping. 7 s at 1024 samples per AAC
    // frame is 329 packets; the flushed remainder makes it 329 or 330.
    EXPECT_GE(stats.packets_encoded, 328u);
    const DecodedAudio decoded = decode_packets(path.encoder()->codec_context(), collector.packets());
    EXPECT_NEAR(static_cast<double>(decoded.mono.size()) - decoded.initial_padding,
                static_cast<double>(kExpectedFrames), 2048.0)
        << "the decoded track is not seven seconds long";
}

// `AUDCLNT_BUFFERFLAGS_SILENT` means "the bytes are undefined", not "there is
// nothing here". Skipping such a buffer shortens the timeline by its duration --
// the same defect as the gap above, arriving by a different route (SPEC.md §8.2).
TEST_F(AudioPathTest, SilentFlaggedBuffersOccupyTheirFullDuration) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    for (int k = 0; k < 150; ++k) {
        const std::int64_t qpc = kT0 + (k * kPeriodNs);
        if (k >= 50 && k < 100) {
            endpoint.deliver_silent(qpc, kFramesPerBuffer);
        } else {
            endpoint.deliver(qpc, kFramesPerBuffer);
        }
    }
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    EXPECT_NEAR(static_cast<double>(stats.frames_written), 3.0 * kRate, kFramesPerBuffer);
    // The silent stretch is one second of it, and none of it came from the
    // watchdog -- the buffers arrived, they were simply flagged.
    EXPECT_NEAR(static_cast<double>(stats.silence_frames_injected), 1.0 * kRate, kFramesPerBuffer);
    EXPECT_EQ(stats.silence_ticks, 0u);
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 4 -- the audio half
// ---------------------------------------------------------------------------

// A beep generated at an exact second from the shared epoch must come back out of
// the AAC stream at that same instant. This is the measurement row 4 makes
// against video; here it is made against the epoch itself, which is the common
// reference both streams are supposed to share (SPEC.md §7.1).
// ---------------------------------------------------------------------------
// SPEC.md §20 row 12 -- audio device change mid-recording (§14.1)
//
// CPU TIER, and deliberately so. The reference rig has **one** render endpoint
// ("Speakers (Realtek(R) Audio)", 48 kHz stereo -- measured by
// `AudioCaptureTest.WhetherThisRigCanReproduceAnEndpointFormatChangeIsRecorded`), so
// there is nowhere to migrate *to* and no second format to migrate *into*. §14.1's hard
// case -- "if the new format differs (rate/channels), keep the encoder's output format
// constant and adapt via `libswresample`" -- is therefore not reproducible on hardware
// here at all.
//
// What *is* reproducible is everything above the WASAPI seam, which is where the
// arithmetic lives: the timeline chaining, the silence fill across the gap, and the
// invariant that the encoder's output never changes. Driving `AudioEncodePath` directly
// exercises all of it against a synthetic format change. The endpoint handover itself
// -- `IMMNotificationClient` firing, a second device opening -- is not covered, and
// that gap is recorded in docs/ACCEPTANCE.md rather than papered over.
// ---------------------------------------------------------------------------

namespace {

MixFormat mono_float_44k() {
    MixFormat format;
    format.sample_rate = 44100; // a genuinely different rate, which is the hard case
    format.channels = 1;
    format.bits_per_sample = 32;
    format.is_float = true;
    format.channel_mask = 0x4; // FC
    return format;
}

} // namespace

TEST_F(AudioPathTest, AnEndpointChangeKeepsTheTimelineContinuousAndTheOutputFormatConstant) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    // The AAC stream's shape, captured before anything migrates. §14.1: "changing an
    // AAC stream's sample rate or channel count mid-file is invalid in both MP4 and
    // MKV", so this is the invariant the whole feature is built around.
    const AVCodecContext* encoder = path.encoder()->codec_context();
    ASSERT_NE(encoder, nullptr);
    const int output_rate_before = encoder->sample_rate;
    const int output_channels_before = encoder->ch_layout.nb_channels;

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint first(path, tone);

    constexpr int kBuffersBefore = 50; // 1 s at 20 ms
    for (int k = 0; k < kBuffersBefore; ++k) {
        first.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }

    // The endpoint goes away and a different one arrives 150 ms later -- inside §14.1's
    // 200 ms target. The gap is what the silence fill has to cover.
    constexpr std::int64_t kGapNs = 150'000'000;
    const std::int64_t seam = kT0 + (kBuffersBefore * kPeriodNs) + kGapNs;

    const MixFormat replacement = mono_float_44k();
    path.migrate_input(replacement, seam);

    // The new endpoint: 44.1 kHz mono, 20 ms buffers -- 882 frames, not 960.
    ToneSettings second_tone;
    second_tone.sample_rate = replacement.sample_rate;
    second_tone.channels = replacement.channels;
    second_tone.beep_epoch_ns = seam;
    Endpoint second(path, second_tone);

    constexpr std::int64_t kFramesPerBuffer44k = 882;
    constexpr int kBuffersAfter = 50; // another 1 s
    for (int k = 0; k < kBuffersAfter; ++k) {
        second.deliver(seam + (k * kPeriodNs), kFramesPerBuffer44k);
    }

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    std::cout << "[ MEASURED ] endpoint migration 48 kHz stereo -> 44.1 kHz mono:\n"
              << "[ MEASURED ]   migrations " << stats.input_migrations << ", timeline " << stats.timeline_seconds
              << " s, silence " << stats.silence_seconds << " s\n"
              << "[ MEASURED ]   encoder output " << encoder->sample_rate << " Hz, " << encoder->ch_layout.nb_channels
              << "ch\n";

    EXPECT_EQ(stats.input_migrations, 1u) << "the migration never reached the aenc thread";

    // --- the invariant §14.1 exists to protect --------------------------------
    EXPECT_EQ(encoder->sample_rate, output_rate_before)
        << "the AAC stream's sample rate changed mid-file, which is invalid in both containers";
    EXPECT_EQ(encoder->ch_layout.nb_channels, output_channels_before)
        << "the AAC stream's channel count changed mid-file, which is invalid in both containers";

    // --- the timeline never shortens ------------------------------------------
    // 1 s on the first endpoint + 150 ms of silence covering the gap + 1 s on the
    // second. The tolerance is one buffer period, because the last buffer's own
    // duration is not counted until the next one places it.
    constexpr double kExpectedSeconds = 1.0 + 0.15 + 1.0;
    EXPECT_NEAR(stats.timeline_seconds, kExpectedSeconds, 0.03)
        << "the timeline is " << stats.timeline_seconds << " s against an expected " << kExpectedSeconds
        << " s; a migration must never shorten it (SPEC.md §14.1)";

    // The gap was filled rather than skipped. Skipping it is the failure mode §14.1
    // names, and it is silent -- the file simply ends up shorter than the recording.
    //
    // Asserted on `silence_seconds`, not `silence_frames_injected`: the latter comes off
    // the *live* timeline, and the silence that covered this gap was written by the
    // timeline being retired. Measured that way it reads 0 on a migration that had just
    // filled 150 ms, which is the reporting trap the seconds accumulator exists to
    // avoid.
    EXPECT_NEAR(stats.silence_seconds, kGapNs / 1e9, 0.03)
        << "silence of " << stats.silence_seconds << " s covered a " << (kGapNs / 1e9)
        << " s endpoint gap; the timeline was closed up instead of filled";
}

// BUG-031's regression test.
//
// `offer` runs on the audio thread and `apply_migration` on `aenc`. When the size of
// a buffer's frames was read off shared state that the migration rewrote, the two
// threads disagreed for exactly the buffers queued across the seam: `offer` sized a
// copy at the *new* endpoint's 4 bytes per frame while `aenc` still read it at the
// old endpoint's 8, running off the end of the allocation. It crashed roughly once in
// forty runs under load, and never once run alone -- which is why the assertion below
// is not the interesting part of this test. The interesting part is that it delivers
// buffers from both endpoints without waiting for the migration to be applied, so the
// window is wide open every time.
//
// A regression would show up here as an access violation, not as a failed
// expectation. Run under load if you want to see it fail on the old code:
//   fc_tests --gtest_filter=*NarrowingFormat* --gtest_repeat=50, eight at once.
TEST_F(AudioPathTest, BuffersQueuedAcrossAMigrationAreReadAtTheSizeTheyWereWrittenAt) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings wide;
    wide.sample_rate = kRate;
    wide.channels = kChannels; // 2ch float -- 8 bytes per frame
    wide.beep_epoch_ns = kT0;

    const MixFormat narrow_format = mono_float_44k(); // 1ch float -- 4 bytes per frame
    ToneSettings narrow;
    narrow.sample_rate = narrow_format.sample_rate;
    narrow.channels = narrow_format.channels;

    // Ten migrations, each with buffers pressed in on both sides of the seam and no
    // pause anywhere. The `aenc` thread is behind the whole time, so the wide buffers
    // are still queued when the narrow ones are being offered -- which is the state
    // the old code read the wrong size in.
    constexpr int kRounds = 10;
    constexpr int kBuffersPerSide = 12;
    constexpr std::int64_t kFramesPerBuffer44k = 882;

    std::int64_t now = kT0;
    for (int round = 0; round < kRounds; ++round) {
        wide.beep_epoch_ns = now;
        Endpoint wide_endpoint(path, wide);
        for (int k = 0; k < kBuffersPerSide; ++k) {
            wide_endpoint.deliver(now, kFramesPerBuffer);
            now += kPeriodNs;
        }

        path.migrate_input(narrow_format, now);

        narrow.beep_epoch_ns = now;
        Endpoint narrow_endpoint(path, narrow);
        for (int k = 0; k < kBuffersPerSide; ++k) {
            narrow_endpoint.deliver(now, kFramesPerBuffer44k);
            now += kPeriodNs;
        }

        path.migrate_input(stereo_float_48k(), now);
    }

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    const double expected_seconds =
        static_cast<double>(kRounds * kBuffersPerSide * 2) * static_cast<double>(kPeriodNs) / 1e9;
    std::cout << "[ MEASURED ] " << (kRounds * 2) << " migrations across " << (kRounds * kBuffersPerSide * 2)
              << " buffers: timeline " << stats.timeline_seconds << " s (expected ~" << expected_seconds
              << "), applied " << stats.input_migrations << "\n";

    EXPECT_EQ(stats.input_migrations, static_cast<std::uint64_t>(kRounds * 2));
    EXPECT_NEAR(stats.timeline_seconds, expected_seconds, 0.05)
        << "the timeline did not survive repeated migrations under queue pressure";
}

// The negative control. Without it, "the timeline survived a migration" could be true
// of an implementation that ignored the migration entirely -- the rates only differ by
// 8%, so a chained timeline and a broken one are not far apart over one second.
TEST_F(AudioPathTest, MigratingToAnIdenticalFormatChangesNothingButTheCount) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    constexpr int kBuffers = 50;
    for (int k = 0; k < kBuffers / 2; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }

    // Same format, no gap: a default-endpoint change to a device that happens to match.
    const std::int64_t seam = kT0 + ((kBuffers / 2) * kPeriodNs);
    path.migrate_input(stereo_float_48k(), seam);

    ToneSettings second_tone = tone;
    second_tone.beep_epoch_ns = seam;
    Endpoint second(path, second_tone);
    for (int k = 0; k < kBuffers / 2; ++k) {
        second.deliver(seam + (k * kPeriodNs), kFramesPerBuffer);
    }

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    EXPECT_EQ(stats.input_migrations, 1u);
    // One second of audio, unbroken. A migration that lost or duplicated the seam would
    // show here as a timeline short or long by the half it mishandled.
    EXPECT_NEAR(stats.timeline_seconds, 1.0, 0.03)
        << "a same-format migration disturbed the timeline; the chaining is not transparent";
}

TEST_F(AudioPathTest, EveryBeepLandsWhereTheSharedEpochSaysItShould) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // The endpoint opened 37 ms before the first video frame did, which is the
    // ordinary case: `t0` is the later of the two, so the first buffers predate
    // the epoch and clamp to index 0 (SPEC.md §7.1). The beeps are all at one
    // second or later, so what this proves is that a ragged start does not shift
    // the rest of the timeline.
    constexpr std::int64_t kEarlyStart = kT0 - 37'000'000;
    constexpr int kSeconds = 65;
    const int buffers = static_cast<int>((kSeconds * kNsPerSecond) / kPeriodNs);

    endpoint.resync_to(kEarlyStart);
    for (int k = 0; k < buffers; ++k) {
        endpoint.deliver(kEarlyStart + (k * kPeriodNs), kFramesPerBuffer);
    }
    ASSERT_TRUE(path.stop().has_value());

    const AVCodecContext* encoder = path.encoder()->codec_context();
    ASSERT_NE(encoder, nullptr);
    const DecodedAudio decoded = decode_packets(encoder, collector.packets());
    ASSERT_GT(decoded.mono.size(), static_cast<std::size_t>(60 * kRate));

    // 5 ms envelope: long enough to ride over the tone's own zero crossings, short
    // enough that its own lag is a fraction of the tolerance being measured.
    const std::vector<std::int64_t> onsets = fc::test::detect_onsets(decoded.mono, kRate / 200, 0.15, kRate / 10);
    ASSERT_GE(onsets.size(), static_cast<std::size_t>(kSeconds - 2)) << "found only " << onsets.size() << " beeps";

    // The raw-packet decode carries the encoder's priming samples, because there
    // is no container here to declare them. Subtracting it is not a fudge: it is
    // the number the muxed file carries as CodecDelay, and the GPU tier's row-4
    // test is what proves the container path applies it.
    EXPECT_GT(decoded.initial_padding, 0) << "no encoder delay declared; a muxed file would be early by it";

    std::int64_t worst_us = 0;
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        const std::int64_t frame = onsets[i] - decoded.initial_padding;
        // Beep 0 is at the epoch itself, which is timeline frame 0.
        const auto expected = static_cast<std::int64_t>(i) * kRate;
        const std::int64_t offset_us = ((frame - expected) * 1'000'000) / kRate;
        worst_us = std::max(worst_us, std::abs(offset_us));

        EXPECT_LT(std::abs(offset_us), 20'000)
            << "beep at " << i << " s decoded " << (static_cast<double>(offset_us) / 1000.0)
            << " ms away from the epoch";
    }

    // The number that matters over a long recording is whether the offset *grows*.
    // A constant bias shifts the whole track equally and is what `initial_padding`
    // exists to declare; an accumulating one is SPEC.md §20 row 4's actual failure.
    const std::int64_t first = onsets.front() - decoded.initial_padding;
    const std::int64_t last =
        onsets.back() - decoded.initial_padding - ((static_cast<std::int64_t>(onsets.size()) - 1) * kRate);
    const std::int64_t growth_us = ((last - first) * 1'000'000) / kRate;
    EXPECT_LT(std::abs(growth_us), 2'000) << "the offset drifted by " << (static_cast<double>(growth_us) / 1000.0)
                                          << " ms across the recording; it should be a constant";

    testing::Test::RecordProperty("worst_offset_us", static_cast<int>(worst_us));
}

// ---------------------------------------------------------------------------
// SPEC.md §8.4 / M4's exit criterion -- a long run against a drifting clock
// ---------------------------------------------------------------------------

// M4 exits on "30 min recording, drift < 20 ms". This is that run with the device
// clock deliberately wrong: 50 ppm fast, which is an ordinary consumer crystal and
// 180 ms an hour if nothing absorbs it.
//
// What must hold after half an hour is SPEC.md §8.4's own quantity --
// `audio_samples_written / rate - qpc_elapsed` -- inside SPEC.md §20 row 4's
// 20 ms. The mechanism that holds it is §8.2's QPC-authoritative timeline, which
// absorbs the clock error as roughly two discarded samples a second; the drift
// ladder measures the result and, on a healthy run, correctly does nothing. A
// ladder that fires here would mean the timeline had stopped working.
//
// Duration is `FC_AUDIO_DRIFT_MINUTES`, defaulting to 30. It costs roughly half a
// second of encoding per simulated minute.
TEST_F(AudioPathTest, ThirtyMinutesOfAFiftyPpmClockStaysInsideTheTolerance) {
    int minutes = 30;
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
    if (const char* value = std::getenv("FC_AUDIO_DRIFT_MINUTES")) {
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && parsed > 0) {
            minutes = static_cast<int>(std::min<long>(parsed, 4 * 60));
        }
    }

    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // A clock 50 ppm fast delivers its 960-frame buffers slightly *sooner* than
    // nominal. Modelled on the arrival time rather than on the frame count,
    // because that is what a fast crystal actually does: the buffer size is fixed
    // by the client, the rate at which it fills is not.
    constexpr double kPpm = 50.0;
    const std::int64_t buffers = (static_cast<std::int64_t>(minutes) * 60 * kNsPerSecond) / kPeriodNs;
    for (std::int64_t k = 0; k < buffers; ++k) {
        const auto qpc = kT0 + static_cast<std::int64_t>(static_cast<double>(k * kPeriodNs) / (1.0 + (kPpm / 1e6)));
        endpoint.deliver(qpc, kFramesPerBuffer);
    }
    const std::int64_t end_qpc =
        kT0 + static_cast<std::int64_t>(static_cast<double>(buffers * kPeriodNs) / (1.0 + (kPpm / 1e6)));

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    // M4's exit criterion, on the timeline.
    const std::int64_t elapsed_frames = ((end_qpc - kT0) * kRate) / kNsPerSecond;
    const std::int64_t offset_frames = stats.frames_written - elapsed_frames;
    const std::int64_t offset_us = (offset_frames * 1'000'000) / kRate;
    EXPECT_LT(std::abs(offset_us), 20'000) << "after " << minutes << " minutes the audio track is "
                                           << (static_cast<double>(offset_us) / 1000.0) << " ms away from wall clock";

    // And on the encoded stream, which is what the container's duration comes
    // from. `worst_drift_ns` is the ladder's own measurement of exactly this.
    EXPECT_LT(std::abs(stats.worst_drift_ns), 20'000'000)
        << "the encoded track drifted " << (static_cast<double>(stats.worst_drift_ns) / 1e6) << " ms from wall clock";
    EXPECT_EQ(stats.hard_resyncs, 0u) << "SPEC.md §8.4: a hard resync is a bug report, not a normal event";
    EXPECT_EQ(stats.encode_failures, 0u);

    // The clock error did not vanish -- it was absorbed. At 50 ppm the timeline
    // discards roughly 2.4 frames a second, so over the run it must have trimmed
    // rather than silently accumulated. Silence injection would mean the opposite
    // sign of error and is a different bug.
    EXPECT_EQ(stats.silence_frames_injected, 0) << "a clock running *fast* should never have needed silence inserted";

    testing::Test::RecordProperty("minutes", minutes);
    testing::Test::RecordProperty("timeline_offset_us", static_cast<int>(offset_us));
    testing::Test::RecordProperty("encoded_drift_us", static_cast<int>(stats.worst_drift_ns / 1000));
    testing::Test::RecordProperty("soft_resyncs", static_cast<int>(stats.soft_resyncs));
}

// The mirror case. A clock running *slow* delivers fewer frames than wall clock
// accounts for, so the timeline closes the shortfall with silence rather than
// letting the track fall behind -- and the total still tracks the wall clock.
TEST_F(AudioPathTest, ASlowClockIsClosedWithSilenceRatherThanLettingTheTrackFallBehind) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // 200 ppm slow, four times the fast case's magnitude, so the effect is
    // unambiguous over a couple of minutes.
    constexpr double kPpm = -200.0;
    constexpr int kMinutes = 2;
    const std::int64_t buffers = (static_cast<std::int64_t>(kMinutes) * 60 * kNsPerSecond) / kPeriodNs;
    for (std::int64_t k = 0; k < buffers; ++k) {
        const auto qpc = kT0 + static_cast<std::int64_t>(static_cast<double>(k * kPeriodNs) / (1.0 + (kPpm / 1e6)));
        endpoint.deliver(qpc, kFramesPerBuffer);
    }
    const std::int64_t end_qpc =
        kT0 + static_cast<std::int64_t>(static_cast<double>(buffers * kPeriodNs) / (1.0 + (kPpm / 1e6)));

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    const std::int64_t elapsed_frames = ((end_qpc - kT0) * kRate) / kNsPerSecond;
    const std::int64_t offset_us = ((stats.frames_written - elapsed_frames) * 1'000'000) / kRate;
    EXPECT_LT(std::abs(offset_us), 20'000)
        << "the track is " << (static_cast<double>(offset_us) / 1000.0) << " ms from wall clock";
    EXPECT_GT(stats.silence_frames_injected, 0) << "a slow clock left a shortfall that nothing closed";
    EXPECT_EQ(stats.hard_resyncs, 0u);
}

// ---------------------------------------------------------------------------
// SPEC.md §8.3 -- the device-position cross-check
// ---------------------------------------------------------------------------

// > Cross-check against `IAudioClock2::GetDevicePosition` and log the delta at
// > 1 Hz as a drift telemetry signal.
//
// Two independent answers to how much time has passed. This measures a quantity
// nothing corrects, which is what makes it useful: `last_drift_ns` says whether
// the *track* is the right length and is held at zero by SPEC.md §8.2's timeline,
// so on its own it cannot tell a healthy endpoint from one whose crystal is
// wandering. This can.
TEST_F(AudioPathTest, TheEndpointsOwnClockIsCrossCheckedAgainstQpc) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // The counter advances 1000 ppm faster than the timestamps say it should --
    // gross by any real standard, so the sign and the magnitude are both
    // unambiguous over a short run.
    constexpr double kPpm = 1000.0;
    endpoint.set_device_rate_scale(1.0 + (kPpm / 1e6));

    constexpr int kSeconds = 10;
    const std::int64_t buffers = (static_cast<std::int64_t>(kSeconds) * kNsPerSecond) / kPeriodNs;
    for (std::int64_t k = 0; k < buffers; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();

    // 1000 ppm over ~10 s is 10 ms, and positive means the endpoint is ahead.
    const double expected_ns = kPpm / 1e6 * kSeconds * static_cast<double>(kNsPerSecond);
    EXPECT_GT(stats.device_clock_delta_ns, 0) << "a fast endpoint clock should read positive";
    EXPECT_NEAR(static_cast<double>(stats.device_clock_delta_ns), expected_ns, expected_ns * 0.25)
        << "measured " << (static_cast<double>(stats.device_clock_delta_ns) / 1e6) << " ms";
    EXPECT_GT(stats.device_position_frames, 0);

    // And the two measurements stay distinct: the *track* is still the right
    // length, because the timeline places audio by QPC regardless of what the
    // endpoint's counter says.
    EXPECT_LT(std::abs(stats.worst_drift_ns), 20'000'000)
        << "the endpoint's clock error leaked into the encoded track's length";
}

// ---------------------------------------------------------------------------
// SPEC.md §7.5 against §8.4 -- paused time and the drift reference (BUG-038)
// ---------------------------------------------------------------------------
//
// §7.5 excises paused time from the audio *track*. §8.4 measures that track against
// elapsed time. If the two use different clocks the ladder reports the pause itself as
// drift, exactly, and every band boundary is crossed by however long the user paused.
//
// This is the deterministic form of the defect. The GPU tier's
// `LoopbackPauseTest.PausingARecordingDoesNotDesyncTheAudioTrackFromTheDeviceClock`
// found it on a real endpoint; this reproduces it on a supplied clock in a hundred
// milliseconds, which is what makes it a regression test rather than a re-observation.
//
// Note what the endpoint does across the paused span: it keeps delivering. WASAPI does
// not stop clocking frames because the user pressed pause, and that is the whole
// difficulty -- the device keeps producing, the timeline refuses what it produces, and
// only one of those two facts is visible to a measurement that reads wall clock.
TEST_F(AudioPathTest, APausedSpanIsExcisedFromTheDriftReferenceAsWellAsFromTheTrack) {
    PacketCollector collector;
    AudioEncodePath path;

    fc::timing::PauseClock pause_clock;
    path.attach_pause_clock(&pause_clock);

    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    constexpr std::int64_t kLegNs = 3 * kNsPerSecond;
    constexpr std::int64_t kPauseNs = 2 * kNsPerSecond;
    const auto buffers_over = [](std::int64_t span) { return span / kPeriodNs; };

    std::int64_t qpc = kT0;
    for (std::int64_t k = 0; k < buffers_over(kLegNs); ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }

    ASSERT_TRUE(pause_clock.pause(qpc));
    for (std::int64_t k = 0; k < buffers_over(kPauseNs); ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }
    ASSERT_TRUE(pause_clock.resume(qpc));
    ASSERT_EQ(pause_clock.paused_total_ns(), kPauseNs);

    // The generator's phase follows the timeline, not wall clock, so the tone is
    // continuous across the seam exactly as row 18's source is.
    endpoint.resync_to(kT0 + kLegNs);
    for (std::int64_t k = 0; k < buffers_over(kLegNs); ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    // The track holds the two legs and not the pause. This half was already correct
    // before the fix -- the file was the right length -- which is what made the defect
    // survive: nothing about the output looked wrong.
    const std::int64_t expected_frames = (2 * kLegNs * kRate) / kNsPerSecond;
    EXPECT_NEAR(static_cast<double>(stats.frames_written), static_cast<double>(expected_frames), kFramesPerBuffer * 2.0)
        << "the track is " << stats.frames_written << " frames where " << expected_frames << " were unpaused";

    // And this half was not. A reference still counting paused time reads short by
    // exactly `kPauseNs`, so the bound is stated against the pause rather than against
    // §8.4's band: 2 s of pause is fifty times the 40 ms band, and a test that only
    // checked the band would not say *which* 2 s it was.
    EXPECT_LT(std::abs(stats.worst_drift_ns), kPauseNs / 10)
        << "worst drift " << (stats.worst_drift_ns / 1000) << " µs against a " << (kPauseNs / 1000)
        << " µs pause -- the drift reference is measuring wall clock, not the timeline";
    EXPECT_LT(std::abs(stats.worst_drift_ns), fc::audio::kDriftIgnoreNs)
        << "a paused recording on a perfect clock left SPEC.md §8.4's do-nothing band";
    EXPECT_EQ(stats.hard_resyncs, 0u);
    EXPECT_EQ(stats.soft_resyncs, 0u);

    // The measurement did not simply stop happening. A fix that skipped drift
    // evaluation for the rest of the recording would pass everything above.
    EXPECT_EQ(pause_clock.stragglers(), 0u) << "the drift measurement counted itself as a straggler; `observe` is the "
                                               "non-counting entry point for exactly this reason";

    testing::Test::RecordProperty("worst_drift_us", static_cast<int>(stats.worst_drift_ns / 1000));
}

// The mirror, and the reason it is here: §8.3's cross-check looks like it has the same
// defect and does not. Both of its terms are wall-clock quantities -- the endpoint's own
// frame counter and QPC -- and neither stops during a pause, so excising paused time
// from one of them would manufacture a delta of exactly the pause duration. The
// asymmetry with §8.4 is not an inconsistency: §8.4 measures the encoded track, which
// paused time never reaches.
TEST_F(AudioPathTest, TheDeviceCrossCheckIsUnaffectedByAPause) {
    PacketCollector collector;
    AudioEncodePath path;

    fc::timing::PauseClock pause_clock;
    path.attach_pause_clock(&pause_clock);

    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);
    // A clock that agrees with QPC exactly, so anything the cross-check reports here is
    // the pause leaking in rather than crystal error.
    endpoint.set_device_rate_scale(1.0);

    constexpr std::int64_t kLegNs = 3 * kNsPerSecond;
    constexpr std::int64_t kPauseNs = 2 * kNsPerSecond;

    std::int64_t qpc = kT0;
    for (std::int64_t k = 0; k < kLegNs / kPeriodNs; ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }
    ASSERT_TRUE(pause_clock.pause(qpc));
    for (std::int64_t k = 0; k < kPauseNs / kPeriodNs; ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }
    ASSERT_TRUE(pause_clock.resume(qpc));
    endpoint.resync_to(kT0 + kLegNs);
    for (std::int64_t k = 0; k < kLegNs / kPeriodNs; ++k, qpc += kPeriodNs) {
        endpoint.deliver(qpc, kFramesPerBuffer);
    }

    ASSERT_TRUE(path.stop().has_value());
    const AudioStats stats = path.stats();

    // A pause of 2 s would show up here as 2 s if either term had been excised.
    EXPECT_LT(std::abs(stats.device_clock_delta_ns), 1'000'000)
        << "the endpoint cross-check read " << (stats.device_clock_delta_ns / 1000)
        << " µs across a pause on a clock that agrees with QPC; one of its two wall-clock "
           "terms has had paused time taken out of it";
    EXPECT_GT(stats.device_position_frames, 0) << "the cross-check did not run at all";
}

TEST_F(AudioPathTest, AnEndpointWithNoClockInterfaceReportsNoCrossCheckRatherThanZeroDrift) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);
    endpoint.disable_device_clock();

    for (int k = 0; k < 100; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }
    ASSERT_TRUE(path.stop().has_value());

    // Zero here means "not measured", and `LoopbackCapture` is what logs the
    // reason. What must not happen is the absent counter being read as a position
    // of zero and reported as seconds of drift.
    const AudioStats stats = path.stats();
    EXPECT_EQ(stats.device_clock_delta_ns, 0);
    EXPECT_EQ(stats.device_position_frames, 0);
    EXPECT_NEAR(static_cast<double>(stats.frames_written), 2.0 * kRate, kFramesPerBuffer);
}

// ---------------------------------------------------------------------------
// SPEC.md §7.1 -- the shared epoch
// ---------------------------------------------------------------------------

// Nothing from before the shared epoch reaches the timeline. Inventing an epoch
// from the first audio packet is what §7.1 forbids, and the visible consequence
// of getting it wrong is a permanent lip-sync offset that no downstream
// arithmetic can recover.
//
// Note what is *not* asserted: how many buffers the epoch gate caught. `set_epoch`
// lands on one thread while `aenc` drains on another, so a buffer offered before
// it can legitimately be processed after it -- and either way it must not extend
// the timeline, because the timeline drops it on its timestamp (BUG-014). The
// invariant is the outcome, and the outcome is deterministic even though the
// route to it is not.
TEST_F(AudioPathTest, NoAudioFromBeforeTheEpochReachesTheTimeline) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    EXPECT_FALSE(path.epoch_set());
    for (int k = 0; k < 25; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }

    // The watchdog must stay silent too: there is no timeline to keep up with yet.
    EXPECT_FALSE(path.silence_due_at(kT0 + kNsPerSecond));

    path.set_epoch(kT0 + (25 * kPeriodNs));
    EXPECT_TRUE(path.epoch_set());
    for (int k = 25; k < 75; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    EXPECT_EQ(stats.buffers_offered, 75u);

    // Exactly one second: the 50 buffers at or after the epoch, and none of the 25
    // before it. Half a second of misplaced audio here is the lip-sync bug.
    EXPECT_NEAR(static_cast<double>(stats.frames_written), 1.0 * kRate, kFramesPerBuffer);
    EXPECT_EQ(stats.silence_frames_injected, 0) << "the epoch was padded rather than aligned to";

    // Each pre-epoch buffer was accounted for exactly once, by one of the two
    // mechanisms that can refuse it.
    EXPECT_EQ(stats.buffers_before_epoch + static_cast<std::uint64_t>(stats.timeline_drops), 25u);
}

// ---------------------------------------------------------------------------
// SPEC.md §12 -- audio is never dropped
// ---------------------------------------------------------------------------

TEST_F(AudioPathTest, EveryOfferedBufferReachesTheEncoder) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.beep_epoch_ns = kT0;
    Endpoint endpoint(path, tone);

    // Far more buffers than the queue holds, offered as fast as the loop can
    // manage. The Block policy means the producer waits rather than discarding;
    // what must not happen is a shorter timeline than the timestamps imply.
    constexpr int kBuffers = 1500; // 30 s
    for (int k = 0; k < kBuffers; ++k) {
        endpoint.deliver(kT0 + (k * kPeriodNs), kFramesPerBuffer);
    }
    ASSERT_TRUE(path.stop().has_value());

    const AudioStats stats = path.stats();
    EXPECT_EQ(stats.buffers_offered, static_cast<std::uint64_t>(kBuffers));
    EXPECT_EQ(stats.timeline_drops, 0);
    EXPECT_EQ(stats.silence_frames_injected, 0) << "a queue that kept up should have needed no silence";
    EXPECT_NEAR(static_cast<double>(stats.frames_written), 30.0 * kRate, kFramesPerBuffer);
}

TEST_F(AudioPathTest, StoppingTwiceIsSafeAndStoppingWithoutBuffersStillProducesNoPackets) {
    PacketCollector collector;
    AudioEncodePath path;
    ASSERT_TRUE(path.open(settings(), collector.sink()).has_value());
    path.set_epoch(kT0);

    EXPECT_TRUE(path.stop().has_value());
    EXPECT_TRUE(path.stop().has_value()); // SPEC.md §10.4: idempotent
    EXPECT_EQ(path.stats().frames_written, 0);
}

// ---------------------------------------------------------------------------
// BUG-012 -- silence through the resampler
// ---------------------------------------------------------------------------

// `swr_convert` reads a null input as *flush*, not as silence. The old
// `Resampler::convert` contract promised silence for a null pointer and would
// have returned whatever happened to be buffered instead -- a short frame where a
// full silent stretch was asked for, which shortens the audio track by exactly
// the gap the silence generator exists to close.
TEST_F(AudioPathTest, TheResamplerProducesRealSilenceAndRefusesANullInput) {
    fc::audio::Resampler resampler;
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, kChannels);
    ASSERT_TRUE(resampler.initialize(stereo_float_48k(), layout).has_value());

    const auto refused = resampler.convert(nullptr, 480);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), fc::FcError::INTERNAL_INVALID_ARGUMENT);

    const auto silent = resampler.convert_silence(480);
    ASSERT_TRUE(silent.has_value());
    ASSERT_EQ(silent.value()->nb_samples, 480) << "silence came back short";
    for (int channel = 0; channel < layout.nb_channels; ++channel) {
        const auto* plane = reinterpret_cast<const float*>(silent.value()->data[channel]);
        for (int i = 0; i < 480; ++i) {
            ASSERT_EQ(plane[i], 0.0F) << "channel " << channel << " sample " << i << " is not silent";
        }
    }

    // A stretch longer than one chunk is the caller's to split, and saying so is
    // better than silently truncating it.
    EXPECT_FALSE(resampler.convert_silence(fc::audio::kSilenceChunkFrames + 1).has_value());

    av_channel_layout_uninit(&layout);
}

// ---------------------------------------------------------------------------
// BUG-013 -- resuming a submit that hit backpressure
// ---------------------------------------------------------------------------

// `AacEncoder::submit` copies samples into its staging frame as it goes. When
// libavcodec refuses more input partway through, the samples already copied are
// staged and must not be sent again: restarting the same frame from zero encodes
// them twice, which lengthens the track and desyncs everything after it.
TEST_F(AudioPathTest, ASubmitThatHitsBackpressureResumesRatherThanRepeating) {
    fc::encode::AudioEncoderSettings encoder_settings;
    encoder_settings.sample_rate = kRate;
    av_channel_layout_default(&encoder_settings.layout, kChannels);

    fc::encode::AacEncoder encoder;
    ASSERT_TRUE(encoder.open(encoder_settings).has_value());

    fc::ff::Frame frame;
    ASSERT_TRUE(frame.alloc());
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = kRate;
    frame->nb_samples = 4096;
    ASSERT_GE(av_channel_layout_copy(&frame->ch_layout, &encoder_settings.layout), 0);
    ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);
    for (int channel = 0; channel < kChannels; ++channel) {
        auto* plane = reinterpret_cast<float*>(frame->data[channel]);
        for (int i = 0; i < frame->nb_samples; ++i) {
            plane[i] = 0.25F * std::sin(2.0F * std::numbers::pi_v<float> * 1000.0F * static_cast<float>(i) / kRate);
        }
    }

    // Deliberately never drained until the loop finishes, which is what fills the
    // encoder's output queue and makes a short return reachable at all.
    int offset = 0;
    int short_returns = 0;
    while (offset < frame->nb_samples) {
        const auto consumed = encoder.submit(frame.get(), offset);
        ASSERT_TRUE(consumed.has_value()) << fc::error_name(consumed.error());
        if (consumed.value() < frame->nb_samples - offset) {
            ++short_returns;
            // Draining is the only thing that unblocks it.
            for (;;) {
                auto received = encoder.receive();
                ASSERT_TRUE(received.has_value());
                const std::optional<fc::encode::EncodedPacket> packet = std::move(received).value();
                if (!packet.has_value()) {
                    break;
                }
            }
        }
        offset += consumed.value();
        ASSERT_LE(offset, frame->nb_samples);
    }

    EXPECT_EQ(offset, frame->nb_samples) << "the resume protocol lost or duplicated samples";

    // Draining before the flush, because the end-of-stream marker is input too and
    // libavcodec refuses input while packets are waiting.
    for (;;) {
        auto received = encoder.receive();
        ASSERT_TRUE(received.has_value());
        const std::optional<fc::encode::EncodedPacket> packet = std::move(received).value();
        if (!packet.has_value()) {
            break;
        }
    }

    // 4096 samples is exactly four AAC frames, so the encoder must have taken
    // four and staged nothing.
    ASSERT_TRUE(encoder.flush().has_value());
    EXPECT_EQ(encoder.frames_submitted(), 4u)
        << "sent " << encoder.frames_submitted() << " frames for 4096 samples (" << short_returns << " short returns)";

    av_channel_layout_uninit(&encoder_settings.layout);
}

} // namespace
