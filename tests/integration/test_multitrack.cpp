// SPEC.md §8.6 Tier B, and §20 rows 14, 15 and 16.
//
// GPU TIER. Everything here goes through a real H.264 encoder and a real Matroska
// muxer and comes back out of a decoded file, because the claims are about what is in
// the file: how many tracks it has, how long each one is, where each one's audio sits,
// and what each one is called.
//
// ---------------------------------------------------------------------------
// Which case uses which source, and why they are not the same
// ---------------------------------------------------------------------------
// **Row 14 uses `AppTrackSource::External`.** Its claim is "distinct tone per track,
// identical durations, per-track sync < 20 ms" -- a statement about where the *timeline*
// puts audio. Making it through a real render -> process-loopback round trip would
// measure Windows' end-to-end audio latency, which is not bounded by this project and is
// not what row 14 is about. It is the same argument `AudioSource::External` records for
// row 4, one milestone later, and CLAUDE.md §5's "tests use the synthetic source, never
// the real desktop" is the same rule for the same reason.
//
// **Row 15 uses the real client**, against a real target process that renders a real
// tone and then exits. That is where `ActivateAudioInterfaceAsync`,
// `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`, the supplied format, the process watch and
// the teardown-on-exit are exercised -- none of which the external seam touches. It also
// checks the *content*: a row-15 test that only asserted "the track is the right length"
// would pass on a client that never captured a byte, which is BUG-044's lesson applied
// before the fact.
//
// **The Tier A control is the third thing here**, and it is half of §24's M9.5 exit
// criterion: "Tier A provably unaffected when Tier B is off". Two recordings of the same
// synthetic input, one with Tier B off and one with it on, and track 0's decoded samples
// compared **sample for sample** -- not its counters, which BUG-044 established can all
// be clean while the output is wrong.

#include "core/audio/process_loopback.h"
#include "core/capture/source_resolver.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/mux/segment_planner.h"
#include "core/pipeline/recording_session.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_audio.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using fc::gpu::AdapterClass;
using fc::gpu::AdapterInfo;
using fc::pipeline::AppTrackConfig;
using fc::pipeline::AppTrackSource;
using fc::pipeline::AudioSource;
using fc::pipeline::PipelineSettings;
using fc::pipeline::VideoPipeline;

// 720p for the reason `test_av_sync` records: the assertions are about timing, and
// decoding minutes of 1080p to inspect every frame buys no extra signal.
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kFps = 60;
constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr std::int64_t kNsPerSecond = 1'000'000'000;
constexpr std::int64_t kPeriodNs = 20'000'000;
constexpr std::int64_t kFramesPerBuffer = 960;

/// SPEC.md §20 row 14's tolerance.
constexpr double kSyncToleranceSeconds = 0.020;

/// The synthetic clock every stream in the deterministic cases is placed against.
constexpr std::int64_t kEpoch = 9'000'000'000LL;

/// Distinct per track, as §20 row 14 requires ("distinct tone per track"). Far enough
/// apart that `tone_energy` cannot confuse two of them, and all inside the band AAC at
/// 192 kbps reproduces cleanly.
///
/// Index 0 is the system mix; 1..3 are per-application tracks.
constexpr std::array<double, 4> kTrackFrequencies{1000.0, 1500.0, 2200.0, 3300.0};

/// `FC_MULTITRACK_SECONDS` overrides it. §8.6 asks for 30 minutes; twenty seconds is
/// what belongs in the routine suite, and the exit-criterion form is one variable away.
[[nodiscard]] int multitrack_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
    const char* value = std::getenv("FC_MULTITRACK_SECONDS");
    if (value == nullptr) {
        return 20;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 3) {
        return 20;
    }
    return static_cast<int>(std::min<long>(parsed, 4 * 60 * 60));
}

/// One track's tone generator, anchored to `kEpoch`.
[[nodiscard]] fc::test::ToneSettings tone_for(std::size_t track) {
    fc::test::ToneSettings tone;
    tone.sample_rate = kRate;
    tone.channels = kChannels;
    tone.frequency = kTrackFrequencies.at(track);
    tone.beep_epoch_ns = kEpoch;
    // One beep a second, on the exact second, on every track. That is what makes the
    // per-track sync question answerable: if all four tracks share one epoch, beep k is
    // at k seconds in every one of them, and a track that is offset shows up as a
    // constant displacement of its whole onset series.
    tone.beep_period_ns = kNsPerSecond;
    return tone;
}

class MultitrackTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("multitrack");

        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "multitrack00001";
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
            description_ = adapter.description;
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

    /// The settings shared by every deterministic recording here.
    [[nodiscard]] static PipelineSettings base_settings(const std::filesystem::path& output) {
        PipelineSettings settings;
        settings.output = output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.video.container = fc::config::Container::Mkv;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;
        settings.audio.source = AudioSource::External;
        settings.audio.channel_layout = fc::config::ChannelLayoutSetting::Stereo;
        settings.audio.buffer_period_ns = kPeriodNs;
        settings.audio.external_format.sample_rate = kRate;
        settings.audio.external_format.channels = kChannels;
        settings.audio.external_format.bits_per_sample = 32;
        settings.audio.external_format.is_float = true;
        settings.audio.external_format.channel_mask = 0x3;
        return settings;
    }

    /// Adds `count` per-application tracks, fed externally.
    static void add_app_tracks(PipelineSettings& settings, std::size_t count) {
        settings.audio.multitrack.enabled = true;
        settings.audio.multitrack.source = AppTrackSource::External;
        for (std::size_t i = 1; i <= count; ++i) {
            AppTrackConfig track;
            track.name = "app" + std::to_string(i) + ".exe";
            track.executable = track.name;
            track.external_format = settings.audio.external_format;
            settings.audio.multitrack.tracks.push_back(std::move(track));
        }
    }

    /// A pause: excise the timeline from `pause_frame` until `resume_frame`.
    struct PauseSpan {
        int pause_frame = 0;
        int resume_frame = 0;
    };

    /// Records `seconds` of the synthetic source, feeding every track its own tone.
    ///
    /// `app_tracks` is how many per-application tracks to open; 0 records Tier A.
    /// `segmentation` and `pauses` are the two features Tier B has to coexist with.
    [[nodiscard]] static bool record(const std::filesystem::path& output, int seconds, std::size_t app_tracks,
                                     const fc::config::SegmentationSettings* segmentation = nullptr,
                                     const std::vector<PauseSpan>* pauses = nullptr) {
        const int frames = seconds * kFps;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        source_settings.frame_limit = static_cast<std::uint32_t>(frames);
        source_settings.flash_interval_frames = static_cast<std::uint32_t>(kFps);

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value() ||
            !source.start(device_->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "the synthetic source would not start";
            return false;
        }

        PipelineSettings settings = base_settings(output);
        if (app_tracks > 0) {
            add_app_tracks(settings, app_tracks);
        }
        if (segmentation != nullptr) {
            settings.segmentation = *segmentation;
        }

        VideoPipeline pipeline;
        const auto started = pipeline.start(device_->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << description_ << ": pipeline start failed, " << fc::error_name(started.error());
            return false;
        }

        std::array<std::vector<float>, kTrackFrequencies.size()> scratch;
        std::int64_t next_audio_frame = 0;
        std::int64_t buffers_sent = 0;
        const std::int64_t total_buffers = (static_cast<std::int64_t>(seconds) * kNsPerSecond) / kPeriodNs;

        for (int index = 0; index < frames; ++index) {
            auto frame = source.acquire(std::chrono::milliseconds{500});
            if (!frame.has_value()) {
                ADD_FAILURE() << "the synthetic source stopped at frame " << index;
                break;
            }

            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns =
                static_cast<std::uint64_t>(kEpoch + ((static_cast<std::int64_t>(index) * kNsPerSecond) / kFps));

            while (buffers_sent < total_buffers &&
                   kEpoch + (buffers_sent * kPeriodNs) <= static_cast<std::int64_t>(stamped.qpc_ns)) {
                const std::int64_t qpc = kEpoch + (buffers_sent * kPeriodNs);

                // Track 0 first, always. It is the one that resolves the audio half of
                // the epoch (SPEC.md §7.1), and the per-application tracks are padded
                // from whatever it settles on.
                fc::test::render_tone(tone_for(0), next_audio_frame, kFramesPerBuffer, scratch[0]);
                fc::audio::LoopbackBuffer mix;
                mix.data = reinterpret_cast<const std::uint8_t*>(scratch[0].data());
                mix.frames = kFramesPerBuffer;
                mix.bytes_per_frame = kChannels * static_cast<int>(sizeof(float));
                mix.qpc_ns = qpc;
                pipeline.offer_audio(mix);

                for (std::size_t track = 1; track <= app_tracks; ++track) {
                    fc::test::render_tone(tone_for(track), next_audio_frame, kFramesPerBuffer, scratch[track]);
                    fc::audio::LoopbackBuffer buffer = mix;
                    buffer.data = reinterpret_cast<const std::uint8_t*>(scratch[track].data());
                    pipeline.offer_app_audio(static_cast<int>(track), buffer);
                }

                pipeline.tick_audio_silence(qpc);
                next_audio_frame += kFramesPerBuffer;
                ++buffers_sent;
            }

            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());

            // `pause_at`/`resume_at` rather than `pause()`/`resume()`, for the reason
            // `VideoPipeline::pause_at` records: both streams here are generated from a
            // *synthetic* clock, and a pause that read `qpc_now_ns()` would excise a span
            // of real time from a timeline made of synthetic time.
            if (pauses != nullptr) {
                for (const PauseSpan& span : *pauses) {
                    if (index == span.pause_frame) {
                        EXPECT_TRUE(pipeline.pause_at(static_cast<std::int64_t>(stamped.qpc_ns)).has_value());
                    }
                    if (index == span.resume_frame) {
                        EXPECT_TRUE(pipeline.resume_at(static_cast<std::int64_t>(stamped.qpc_ns)).has_value());
                    }
                }
            }
        }
        source.stop();

        const auto report = pipeline.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << "finalization failed: " << fc::error_name(report.error());
            return false;
        }
        if (!report.value().valid) {
            ADD_FAILURE() << "the output did not validate: " << report.value().detail;
            return false;
        }
        return true;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
    static std::unique_ptr<fc::gpu::GpuTopologyService> topology_;
    static std::unique_ptr<fc::gpu::D3dDevice> device_;
    static std::string encoder_name_;
    static std::string description_;
    static std::uint32_t pool_bind_flags_;
};

std::unique_ptr<fc::test::TempDir> MultitrackTest::dir_;
std::unique_ptr<fc::gpu::GpuTopologyService> MultitrackTest::topology_;
std::unique_ptr<fc::gpu::D3dDevice> MultitrackTest::device_;
std::string MultitrackTest::encoder_name_;
std::string MultitrackTest::description_;
std::uint32_t MultitrackTest::pool_bind_flags_ = 0;

// ---------------------------------------------------------------------------
// SPEC.md §20 row 14 -- `test_multitrack_alignment`
// ---------------------------------------------------------------------------

TEST_F(MultitrackTest, EveryTrackCarriesItsOwnToneWithTheSameDurationAndStaysInSync) {
    const int seconds = multitrack_seconds();
    const std::filesystem::path output = dir_->path() / "alignment.mkv";
    ASSERT_TRUE(record(output, seconds, 3));

    fc::test::DecodedMedia media;
    // Luma off: this case is about audio, and averaging every frame of a half-hour
    // recording is minutes of CPU for nothing (BUG-018's neighbourhood).
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;

    // Four audio streams plus the video one. §8.6: track 0 is the system mix, 1..3 are
    // the applications, and none of them may be missing -- "never drop it from the
    // container".
    ASSERT_EQ(media.audio_track_count(), 4U) << "the file carries " << media.audio_track_count() << " audio tracks";
    ASSERT_EQ(media.stream_count, 5);

    // §8.6: "Every track gets a human-readable Matroska `Name` tag ... Untagged tracks
    // are a UX failure." Read back out of the file, because a name set after
    // `avformat_write_header` never reaches it.
    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    ASSERT_NE(mix, nullptr);
    EXPECT_EQ(mix->name, "System Mix");
    for (std::size_t track = 1; track < 4; ++track) {
        const fc::test::DecodedAudioTrack* entry = media.audio_track(track);
        ASSERT_NE(entry, nullptr) << "track " << track << " is missing";
        EXPECT_EQ(entry->name, "app" + std::to_string(track) + ".exe");
    }

    // ---- identical durations (row 14's first clause) ----------------------
    ASSERT_FALSE(mix->planes.empty());
    const std::size_t mix_samples = mix->planes.front().size();
    ASSERT_GT(mix_samples, 0U);
    const double mix_seconds = static_cast<double>(mix_samples) / kRate;
    EXPECT_NEAR(mix_seconds, static_cast<double>(seconds), std::max(0.5, seconds * 0.01));

    for (std::size_t track = 1; track < 4; ++track) {
        const fc::test::DecodedAudioTrack* entry = media.audio_track(track);
        ASSERT_FALSE(entry->planes.empty()) << "track " << track << " decoded to nothing";
        const double entry_seconds = static_cast<double>(entry->planes.front().size()) / kRate;
        // One AAC frame is 1024 samples, so tracks whose flushes land a frame apart are
        // identical for every purpose §8.6 cares about. Anything beyond a buffer period
        // is a ragged track.
        EXPECT_NEAR(entry_seconds, mix_seconds, 0.02)
            << "track " << track << " is " << entry_seconds << " s against the system mix's " << mix_seconds << " s";
    }

    // ---- distinct tone per track (row 14's second clause) ------------------
    //
    // The content check, and the reason it is here rather than a packet count: a muxer
    // that routed every track's packets to stream 1 would produce four tracks of the
    // right length, four correct names, four healthy counters, and one tone repeated
    // four times. Only reading each track's own frequency back out sees that.
    for (std::size_t track = 0; track < 4; ++track) {
        const fc::test::DecodedAudioTrack* entry = media.audio_track(track);
        ASSERT_NE(entry, nullptr);
        double own = 0.0;
        double strongest_other = 0.0;
        std::size_t strongest_index = 0;
        for (std::size_t candidate = 0; candidate < kTrackFrequencies.size(); ++candidate) {
            const double energy = fc::test::tone_energy(entry->planes.front(), kTrackFrequencies[candidate], kRate);
            if (candidate == track) {
                own = energy;
            } else if (energy > strongest_other) {
                strongest_other = energy;
                strongest_index = candidate;
            }
        }
        EXPECT_GT(own, strongest_other * 4.0)
            << "track " << track << " is loudest at " << kTrackFrequencies[strongest_index] << " Hz, not at its own "
            << kTrackFrequencies[track] << " Hz -- the tracks are crossed";
    }

    // ---- per-track sync < 20 ms (row 14's third clause) --------------------
    //
    // Every track's beeps were generated on one clock at exact seconds from the shared
    // epoch, so beep k must land at k seconds in every track. A track that was *offset*
    // rather than silence-padded shows up as a constant displacement of its whole
    // series -- which is the classic desync §8.6 names, and which no duration assertion
    // can see.
    const std::vector<double> mix_onsets = fc::test::onset_times(*mix, 0, 128, 0.25, 0.25);
    ASSERT_GE(mix_onsets.size(), static_cast<std::size_t>(seconds) - 2)
        << "only " << mix_onsets.size() << " beeps were detected on the system mix over " << seconds << " s";

    double worst_offset = 0.0;
    std::size_t worst_track = 0;
    std::size_t compared = 0;
    for (std::size_t track = 1; track < 4; ++track) {
        const fc::test::DecodedAudioTrack* entry = media.audio_track(track);
        const std::vector<double> onsets = fc::test::onset_times(*entry, 0, 128, 0.25, 0.25);
        ASSERT_EQ(onsets.size(), mix_onsets.size())
            << "track " << track << " has " << onsets.size() << " beeps against the system mix's " << mix_onsets.size()
            << " -- a track with a different number of beeps is one that started somewhere else";
        for (std::size_t k = 0; k < onsets.size(); ++k) {
            const double offset = onsets[k] - mix_onsets[k];
            ++compared;
            if (std::abs(offset) > std::abs(worst_offset)) {
                worst_offset = offset;
                worst_track = track;
            }
        }
    }

    EXPECT_LT(std::abs(worst_offset), kSyncToleranceSeconds)
        << "track " << worst_track << " is " << (worst_offset * 1000.0)
        << " ms from the system mix at its worst mark, against SPEC.md §20 row 14's 20 ms";

    testing::Test::RecordProperty("seconds", seconds);
    testing::Test::RecordProperty("tracks", 4);
    testing::Test::RecordProperty("marks_compared", static_cast<int>(compared));
    testing::Test::RecordProperty("worst_offset_us", static_cast<int>(worst_offset * 1e6));
    testing::Test::RecordProperty("mix_ms", static_cast<int>(mix_seconds * 1000.0));
}

// ---------------------------------------------------------------------------
// SPEC.md §24's other half -- Tier A provably unaffected when Tier B is off
// ---------------------------------------------------------------------------

TEST_F(MultitrackTest, TierAIsSampleForSampleIdenticalWhetherTierBIsOnOrOff) {
    // The exit criterion's second clause, stated as a measurement against a control
    // rather than as an assurance. Two recordings of *identical* synthetic input --
    // same epoch, same frames, same buffers, same tone -- differing only in whether
    // three per-application tracks exist beside track 0.
    //
    // The two do share things: one mux queue, one mux thread, one `AVFormatContext`, one
    // bounded queue whose `Block` policy now has four producers instead of one. So
    // "structurally separate" is an argument, not evidence; this is the evidence.
    constexpr int kSeconds = 8;
    const std::filesystem::path without = dir_->path() / "tier_a_only.mkv";
    const std::filesystem::path with = dir_->path() / "tier_a_and_b.mkv";

    ASSERT_TRUE(record(without, kSeconds, 0));
    ASSERT_TRUE(record(with, kSeconds, 3));

    fc::test::DecodedMedia control;
    fc::test::DecodedMedia treated;
    fc::test::decode_media(without, fc::test::DecodeOptions{true, true, false}, control);
    fc::test::decode_media(with, fc::test::DecodeOptions{true, true, false}, treated);
    ASSERT_TRUE(control.opened) << control.detail;
    ASSERT_TRUE(treated.opened) << treated.detail;

    ASSERT_EQ(control.audio_track_count(), 1U) << "the control recorded more than the system mix";
    ASSERT_EQ(treated.audio_track_count(), 4U) << "Tier B did not open its tracks, so this compares two Tier A runs";

    ASSERT_TRUE(control.audio.present);
    ASSERT_TRUE(treated.audio.present);
    ASSERT_FALSE(control.audio.planes.empty());
    ASSERT_FALSE(treated.audio.planes.empty());

    // Structure first.
    EXPECT_EQ(control.audio.channels, treated.audio.channels);
    EXPECT_EQ(control.audio.sample_rate, treated.audio.sample_rate);
    EXPECT_EQ(control.audio.initial_padding, treated.audio.initial_padding);
    EXPECT_DOUBLE_EQ(control.audio.first_time, treated.audio.first_time);

    const std::vector<float>& reference = control.audio.planes.front();
    const std::vector<float>& measured = treated.audio.planes.front();
    EXPECT_EQ(reference.size(), measured.size())
        << "the system mix is " << measured.size() << " samples with Tier B on and " << reference.size()
        << " with it off";

    // And then the samples themselves. **Not the counters** -- BUG-044 wrote the wrong
    // pixels into the right number of frames with every counter in the pipeline clean,
    // and the only thing that saw it was reading the content back out. The equivalent
    // here is the decoded PCM of track 0.
    //
    // Bit-exact rather than within a tolerance: the input is deterministic, the encoder
    // settings are identical, and AAC is deterministic for identical input, so any
    // difference at all is Tier B having reached track 0.
    std::size_t differing = 0;
    double worst_difference = 0.0;
    const std::size_t common = std::min(reference.size(), measured.size());
    for (std::size_t i = 0; i < common; ++i) {
        const double difference = std::abs(static_cast<double>(reference[i]) - static_cast<double>(measured[i]));
        if (difference > 0.0) {
            ++differing;
            worst_difference = std::max(worst_difference, difference);
        }
    }
    EXPECT_EQ(differing, 0U) << differing << " of " << common << " system-mix samples changed when Tier B was enabled, "
                             << "worst by " << worst_difference;

    // The video half too, because the mux queue is shared and a frame lost to
    // interleaving pressure would be Tier B affecting the recording by a different
    // route than the audio one.
    EXPECT_EQ(control.video.frame_count, treated.video.frame_count)
        << "the video track is " << treated.video.frame_count << " frames with Tier B on and "
        << control.video.frame_count << " with it off";

    testing::Test::RecordProperty("samples_compared", static_cast<int>(common));
    testing::Test::RecordProperty("samples_differing", static_cast<int>(differing));
    testing::Test::RecordProperty("video_frames", static_cast<int>(control.video.frame_count));
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 16 -- `test_multitrack_mp4_rejected`
// ---------------------------------------------------------------------------

TEST_F(MultitrackTest, MultiTrackOnMp4IsRefusedByTheEngineAndNotOnlyByTheGui) {
    // > **MP4 is a hard block, not a soft warning.** ... the engine rejects a
    // > `configure` command that requests both with `FcError::MULTITRACK_REQUIRES_MKV`
    // > (3021).
    //
    // Row 16's own wording is "enforced engine-side, not just in the GUI", so this
    // asserts against the pipeline: no GUI, no IPC, nothing a headless caller could
    // bypass. The `configure` half is asserted from a separate process by
    // `test_the_engine_refuses_multi_track_audio_on_mp4` in `gui/tests`.
    const std::filesystem::path output = dir_->path() / "refused.mp4";
    PipelineSettings settings = base_settings(output);
    settings.video.container = fc::config::Container::Mp4;
    add_app_tracks(settings, 2);

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    ASSERT_FALSE(started.has_value()) << "multi-track audio was accepted on MP4";
    EXPECT_EQ(started.error(), fc::FcError::MULTITRACK_REQUIRES_MKV);

    // And nothing was left behind. A refusal that had already opened the container
    // would leave a zero-length `.mp4` beside the user's recordings.
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(MultitrackTest, TheSameConfigurationOnMkvIsAccepted) {
    // The positive control for the case above. Without it, a build that refused every
    // multi-track configuration -- or every configuration at all -- would satisfy it.
    PipelineSettings settings = base_settings(dir_->path() / "accepted.mkv");
    add_app_tracks(settings, 2);

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    ASSERT_TRUE(started.has_value()) << fc::error_name(started.error());
    static_cast<void>(pipeline.stop());
}

// ---------------------------------------------------------------------------
// SPEC.md §20 row 15 -- `test_multitrack_process_exit`
// ---------------------------------------------------------------------------

/// Spawns the audio target, which renders a tone for `seconds` and then exits.
class AudioTarget {
public:
    AudioTarget() = default;

    ~AudioTarget() {
        if (handle_ != nullptr) {
            // Only if it is somehow still running. The point of the case is that it
            // exits on its own; this is cleanup for a failed run.
            if (WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT) {
                TerminateProcess(handle_, 1);
            }
            CloseHandle(handle_);
        }
    }

    AudioTarget(const AudioTarget&) = delete;
    AudioTarget& operator=(const AudioTarget&) = delete;
    AudioTarget(AudioTarget&&) = delete;
    AudioTarget& operator=(AudioTarget&&) = delete;

    /// How the target should behave. Each field is one shape a real application has
    /// and the first version of this helper did not — see `tools/audio_target/main.cpp`.
    struct Shape {
        double seconds = 3.0;
        double frequency = 1000.0;
        /// Concurrent render clients in the one process, at `frequency`, `frequency +
        /// 700`, … A browser with two tabs playing.
        int streams = 1;
        /// Render from a **child** process, with the parent silent.
        bool child = false;
        /// Request this rate rather than the endpoint's. 0 follows the endpoint.
        int rate = 0;
    };

    [[nodiscard]] bool launch(const Shape& shape) {
        std::string command = std::string{"\""} + FC_AUDIO_TARGET_EXE + "\" --seconds " +
                              std::to_string(shape.seconds) + " --frequency " + std::to_string(shape.frequency) +
                              " --streams " + std::to_string(shape.streams);
        if (shape.rate > 0) {
            command += " --rate " + std::to_string(shape.rate);
        }
        if (shape.child) {
            command += " --child";
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process) == 0) {
            return false;
        }
        CloseHandle(process.hThread);
        handle_ = process.hProcess;
        pid_ = process.dwProcessId;
        return true;
    }

    [[nodiscard]] bool launch(double seconds, double frequency) {
        return launch(Shape{seconds, frequency, 1, false, 0});
    }

    [[nodiscard]] std::uint32_t pid() const noexcept {
        return pid_;
    }

    [[nodiscard]] bool has_exited() const noexcept {
        return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
    }

private:
    HANDLE handle_ = nullptr;
    std::uint32_t pid_ = 0;
};

TEST_F(MultitrackTest, ATargetThatExitsMidRecordingLeavesATrackSilencePaddedToFullDuration) {
    // The real client, against a real process, over a real endpoint. This is the only
    // case that exercises `ActivateAudioInterfaceAsync`, the supplied 48 kHz / float /
    // stereo format, `ProcessWatch`, and the teardown §8.6 requires when a target goes
    // ("A process exiting means tearing down that track's client, not reconfiguring
    // it").
    if (!fc::audio::process_loopback_available()) {
        // Not a skip in disguise: it fails, because CLAUDE.md §6 says a GPU-tier test
        // that passes on a machine without the hardware proves nothing, and the
        // reference rig has this.
        FAIL() << "process loopback is unavailable on this system; SPEC.md §8.6 requires Windows 10 19041+, "
                  "which §1 already makes the floor. Probe result: "
               << fc::hresult_message(fc::audio::process_loopback_probe_result());
    }

    constexpr int kRecordingSeconds = 12;
    constexpr double kTargetSeconds = 4.0;
    const std::filesystem::path output = dir_->path() / "process_exit.mkv";

    AudioTarget target;
    ASSERT_TRUE(target.launch(kTargetSeconds, 1500.0)) << "could not start the audio target process";
    ASSERT_NE(target.pid(), 0U);
    // Let it reach `Start()` before the recording attaches, so the track has something
    // on it from the first buffer rather than only after a poll.
    std::this_thread::sleep_for(std::chrono::milliseconds{400});

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = kWidth;
    source_settings.height = kHeight;
    source_settings.fps = kFps;

    fc::test::SyntheticSource source;
    ASSERT_TRUE(source.configure(source_settings).has_value());
    ASSERT_TRUE(source.start(device_->device(), fc::capture::CaptureTarget{}).has_value());

    PipelineSettings settings;
    settings.output = output;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.encoder_name = encoder_name_;
    settings.pool_bind_flags = pool_bind_flags_;
    // The real endpoint for track 0, and the real process-loopback client for track 1.
    // Real timestamps throughout, so this runs in real time (see
    // `test_loopback_recording`, which makes the same trade for the same reason).
    settings.audio.source = AudioSource::SystemLoopback;
    settings.audio.multitrack.enabled = true;
    settings.audio.multitrack.source = AppTrackSource::ProcessLoopback;
    AppTrackConfig track;
    track.name = "fc_audio_target.exe";
    // Pinned by pid rather than by name: the test knows exactly which process it
    // launched, and resolving by name could pick up a leftover from a previous run.
    track.pid = target.pid();
    settings.audio.multitrack.tracks.push_back(track);

    VideoPipeline pipeline;
    const auto started = pipeline.start(device_->device(), settings);
    ASSERT_TRUE(started.has_value()) << "pipeline start failed: " << fc::error_name(started.error());

    const auto begin = std::chrono::steady_clock::now();
    const auto until = begin + std::chrono::seconds{kRecordingSeconds};
    int submitted = 0;
    bool observed_exit = false;
    while (std::chrono::steady_clock::now() < until) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        ASSERT_TRUE(frame.has_value()) << "the synthetic source stopped at frame " << submitted;
        fc::capture::CaptureFrame stamped = frame.value();
        stamped.qpc_ns = static_cast<std::uint64_t>(fc::timing::qpc_now_ns());
        static_cast<void>(pipeline.submit(stamped));
        source.release(frame.value());
        ++submitted;

        if (!observed_exit && target.has_exited()) {
            observed_exit = true;
        }

        const auto next = begin + (std::chrono::nanoseconds{1'000'000'000} * submitted / kFps);
        std::this_thread::sleep_until(next);
    }
    source.stop();

    // The event the row is about actually happened, and it happened *during* the
    // recording rather than after it. Asserted before anything else is examined --
    // otherwise a target that outlived the recording would make every assertion below
    // pass while testing nothing (the control BUG-044's table needed).
    ASSERT_TRUE(observed_exit) << "the target process did not exit during the recording";

    const fc::pipeline::AppTracksStats tracks = pipeline.app_track_stats();
    const auto report = pipeline.stop();
    ASSERT_TRUE(report.has_value()) << "finalization failed: " << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    ASSERT_EQ(tracks.tracks.size(), 1U);
    const fc::pipeline::AppTrackStats& app = tracks.tracks.front();
    EXPECT_TRUE(app.ever_attached) << "the process-loopback client never attached to the target";
    EXPECT_TRUE(app.target_exited) << "the engine did not notice the target exiting (SPEC.md §20 row 15)";
    EXPECT_FALSE(app.attached) << "the client was left open on a process that no longer exists";
    EXPECT_GT(app.source_buffers, 0U) << "the process-loopback client delivered no buffers at all";

    testing::Test::RecordProperty("target_buffers", static_cast<int>(app.source_buffers));
    testing::Test::RecordProperty("qpc_fallbacks", static_cast<int>(app.qpc_fallbacks));
    testing::Test::RecordProperty("track_ms", static_cast<int>(app.audio.timeline_seconds * 1000.0));
    testing::Test::RecordProperty("track_silence_ms", static_cast<int>(app.audio.silence_seconds * 1000.0));

    // ---- the file ---------------------------------------------------------
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), 2U) << "the track was dropped from the container";

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    const fc::test::DecodedAudioTrack* app_track = media.audio_track(1);
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(app_track, nullptr);
    ASSERT_FALSE(mix->planes.empty());
    ASSERT_FALSE(app_track->planes.empty());
    EXPECT_EQ(app_track->name, "fc_audio_target.exe");

    const double mix_seconds = static_cast<double>(mix->planes.front().size()) / std::max(mix->sample_rate, 1);
    const double app_seconds =
        static_cast<double>(app_track->planes.front().size()) / std::max(app_track->sample_rate, 1);

    // §8.6: "that track continues as pure silence to the end of the file. Never
    // truncate a track." The target was gone for two thirds of the recording, so a
    // build that stopped the timeline when the client went away produces a track about
    // a third of the length -- which is exactly the failure this asserts against.
    EXPECT_NEAR(app_seconds, mix_seconds, 0.5)
        << "the per-application track is " << app_seconds << " s against the system mix's " << mix_seconds
        << " s; SPEC.md §20 row 15 requires it to run to the end of the file";
    EXPECT_GT(app_seconds, kRecordingSeconds * 0.9)
        << "the track is " << app_seconds << " s of a " << kRecordingSeconds << " s recording";

    // ---- and it carried the target's audio, not just the right number of samples ---
    //
    // The content check. Without it, a client that activated, delivered nothing, and
    // let the watchdog pad the whole track to full length would pass everything above.
    // The tone is only present in the first `kTargetSeconds`, so the comparison is
    // between the head of the track and its tail.
    const std::vector<float>& samples = app_track->planes.front();
    const auto head_end = static_cast<std::size_t>(kTargetSeconds * 0.75 * app_track->sample_rate);
    const auto tail_begin = static_cast<std::size_t>((kTargetSeconds + 3.0) * app_track->sample_rate);
    ASSERT_GT(samples.size(), tail_begin) << "the track is too short to have a tail to compare";

    const std::vector<float> head{samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(head_end)};
    const std::vector<float> tail{samples.begin() + static_cast<std::ptrdiff_t>(tail_begin), samples.end()};
    const double head_energy = fc::test::tone_energy(head, 1500.0, app_track->sample_rate);
    const double tail_energy = fc::test::tone_energy(tail, 1500.0, app_track->sample_rate);

    EXPECT_GT(head_energy, tail_energy * 8.0)
        << "the target's 1500 Hz tone is not louder before it exited (" << head_energy << ") than after it ("
        << tail_energy << ") -- either the client captured nothing, or the track is not the target's";

    testing::Test::RecordProperty("head_energy_e6", static_cast<int>(head_energy * 1e6));
    testing::Test::RecordProperty("tail_energy_e6", static_cast<int>(tail_energy * 1e6));
    testing::Test::RecordProperty("app_track_ms", static_cast<int>(app_seconds * 1000.0));
    testing::Test::RecordProperty("mix_track_ms", static_cast<int>(mix_seconds * 1000.0));
}

// ---------------------------------------------------------------------------
// Real applications, not one sine wave through one easy client
// ---------------------------------------------------------------------------
//
// Everything below drives the **real** process-loopback client against real target
// processes. The deterministic cases above prove where the timeline puts audio; these
// prove that what arrives is the right application's, in the shapes a real one has.

namespace {

/// A recording on the real endpoint with real process-loopback tracks.
///
/// Real timestamps throughout, so it runs in real time — the same trade
/// `test_loopback_recording` makes, for the same reason.
struct RealRecording {
    bool ok = false;
    fc::pipeline::AppTracksStats tracks;
    fc::pipeline::PipelineStats video;
};

} // namespace

class RealTargetTest : public MultitrackTest {
protected:
    /// Records `seconds` with one per-application track per entry in `targets`.
    ///
    /// `during` runs once per submitted frame with the elapsed seconds, so a case can
    /// make something happen *inside* the recording — a target exiting, or a
    /// replacement being launched.
    [[nodiscard]] static RealRecording record_real(const std::filesystem::path& output, int seconds,
                                                   const std::vector<AppTrackConfig>& targets,
                                                   const std::function<void(double)>& during = {}) {
        RealRecording out;

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kWidth;
        source_settings.height = kHeight;
        source_settings.fps = kFps;
        // The picture is not the subject; a repeating pattern keeps a real-time feed
        // comfortable at 60 fps on every preset (the warning in `synthetic_source.h`).
        source_settings.prerendered_frames = 8;

        fc::test::SyntheticSource source;
        if (!source.configure(source_settings).has_value() ||
            !source.start(device_->device(), fc::capture::CaptureTarget{}).has_value()) {
            ADD_FAILURE() << "the synthetic source would not start";
            return out;
        }

        PipelineSettings settings;
        settings.output = output;
        settings.video.width = kWidth;
        settings.video.height = kHeight;
        settings.video.fps = kFps;
        settings.encoder_name = encoder_name_;
        settings.pool_bind_flags = pool_bind_flags_;
        settings.audio.source = AudioSource::SystemLoopback;
        settings.audio.multitrack.enabled = true;
        settings.audio.multitrack.source = AppTrackSource::ProcessLoopback;
        settings.audio.multitrack.tracks = targets;

        VideoPipeline pipeline;
        const auto started = pipeline.start(device_->device(), settings);
        if (!started.has_value()) {
            ADD_FAILURE() << "pipeline start failed: " << fc::error_name(started.error());
            return out;
        }

        const auto begin = std::chrono::steady_clock::now();
        const auto until = begin + std::chrono::seconds{seconds};
        int submitted = 0;
        while (std::chrono::steady_clock::now() < until) {
            auto frame = source.acquire(std::chrono::milliseconds{200});
            if (!frame.has_value()) {
                ADD_FAILURE() << "the synthetic source stopped at frame " << submitted;
                break;
            }
            fc::capture::CaptureFrame stamped = frame.value();
            stamped.qpc_ns = static_cast<std::uint64_t>(fc::timing::qpc_now_ns());
            static_cast<void>(pipeline.submit(stamped));
            source.release(frame.value());
            ++submitted;
            if (during) {
                during(std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
            }
            std::this_thread::sleep_until(begin + (std::chrono::nanoseconds{1'000'000'000} * submitted / kFps));
        }
        source.stop();

        out.tracks = pipeline.app_track_stats();
        out.video = pipeline.stats();
        const auto report = pipeline.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << "finalization failed: " << fc::error_name(report.error());
            return out;
        }
        if (!report.value().valid) {
            ADD_FAILURE() << "the output did not validate: " << report.value().detail;
            return out;
        }
        out.ok = true;
        return out;
    }

    /// Energy at `frequency` on a track's first channel.
    [[nodiscard]] static double energy_at(const fc::test::DecodedAudioTrack& track, double frequency) {
        if (track.planes.empty()) {
            return 0.0;
        }
        return fc::test::tone_energy(track.planes.front(), frequency, track.sample_rate);
    }

    static void require_process_loopback() {
        if (!fc::audio::process_loopback_available()) {
            FAIL() << "process loopback is unavailable on this system; SPEC.md §8.6 requires Windows 10 19041+, "
                      "which §1 already makes the floor. Probe result: "
                   << fc::hresult_message(fc::audio::process_loopback_probe_result());
        }
    }
};

TEST_F(RealTargetTest, FiveRealApplicationsRecordToSixTracksThatDoNotBleedIntoEachOther) {
    // Two gaps in one case, because they are the same recording.
    //
    // **Six tracks at once**, which is SPEC.md §8.6's stated maximum and had never been
    // recorded end to end — five process-loopback clients, five capture threads, five
    // `aenc` threads, five drift loops and the system mix, all against one epoch.
    //
    // **Per-track isolation**, which is the property Tier B exists for and which the
    // deterministic cases cannot test at all: they feed each track separately, so a
    // build that mixed every application into every track would still pass them. Here
    // all five applications render to the same endpoint at the same time, so every
    // track has four other tones available to it and must carry exactly one.
    require_process_loopback();

    constexpr int kRecordingSeconds = 10;
    constexpr double kTargetSeconds = 9.0;
    constexpr std::size_t kTargets = 5;
    // Spaced so `tone_energy` cannot confuse two, and all inside the band AAC at
    // 192 kbps reproduces cleanly.
    constexpr std::array<double, kTargets> kFrequencies{800.0, 1500.0, 2400.0, 3600.0, 5200.0};

    std::array<AudioTarget, kTargets> targets;
    std::vector<AppTrackConfig> configured;
    for (std::size_t i = 0; i < kTargets; ++i) {
        AudioTarget::Shape shape;
        shape.seconds = kTargetSeconds;
        shape.frequency = kFrequencies[i];
        ASSERT_TRUE(targets[i].launch(shape)) << "could not start target " << i;
        ASSERT_NE(targets[i].pid(), 0U);
        AppTrackConfig track;
        track.name = "app" + std::to_string(i + 1) + ".exe";
        // Pinned by pid rather than resolved by name: five of these share one
        // executable, so a name would resolve all five to the same process.
        track.pid = targets[i].pid();
        configured.push_back(std::move(track));
    }
    // Let every target reach `Start()` before the recording attaches.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    const std::filesystem::path output = dir_->path() / "six_tracks.mkv";
    const RealRecording recorded = record_real(output, kRecordingSeconds, configured);
    ASSERT_TRUE(recorded.ok);

    ASSERT_EQ(recorded.tracks.tracks.size(), kTargets);
    for (const fc::pipeline::AppTrackStats& track : recorded.tracks.tracks) {
        EXPECT_TRUE(track.ever_attached) << track.name << " never attached to its target";
        EXPECT_GT(track.source_buffers, 0U) << track.name << " received no buffers";
    }

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), kTargets + 1)
        << "the file carries " << media.audio_track_count() << " audio tracks, not SPEC.md §8.6's maximum of 6";

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    ASSERT_NE(mix, nullptr);
    ASSERT_FALSE(mix->planes.empty());
    const double mix_seconds = static_cast<double>(mix->planes.front().size()) / std::max(mix->sample_rate, 1);

    // **The control comes first.** Every one of the five tones has to be present in the
    // system mix, or the isolation assertions below would be satisfied by targets that
    // simply never played — which is the shape BUG-044's table needed and did not have.
    for (std::size_t i = 0; i < kTargets; ++i) {
        EXPECT_GT(energy_at(*mix, kFrequencies[i]), 1e-4)
            << "the system mix has no energy at " << kFrequencies[i] << " Hz, so target " << i
            << " was not audible and this recording proves nothing about isolation";
    }

    double worst_ratio = 1e9;
    std::size_t worst_track = 0;
    for (std::size_t i = 0; i < kTargets; ++i) {
        const fc::test::DecodedAudioTrack* track = media.audio_track(i + 1);
        ASSERT_NE(track, nullptr);
        ASSERT_FALSE(track->planes.empty()) << "track " << (i + 1) << " decoded to nothing";
        EXPECT_EQ(track->name, "app" + std::to_string(i + 1) + ".exe");

        // §8.6: "All tracks share one timebase, one epoch (`t0`), and identical
        // duration" — with five real clients attaching at five slightly different
        // instants, which is where a track that measured from its own first buffer
        // would show up.
        const double seconds = static_cast<double>(track->planes.front().size()) / std::max(track->sample_rate, 1);
        EXPECT_NEAR(seconds, mix_seconds, 0.5)
            << track->name << " is " << seconds << " s against the system mix's " << mix_seconds << " s";

        const double own = energy_at(*track, kFrequencies[i]);
        double loudest_foreign = 0.0;
        for (std::size_t other = 0; other < kTargets; ++other) {
            if (other != i) {
                loudest_foreign = std::max(loudest_foreign, energy_at(*track, kFrequencies[other]));
            }
        }
        const double ratio = own / std::max(loudest_foreign, 1e-12);
        if (ratio < worst_ratio) {
            worst_ratio = ratio;
            worst_track = i + 1;
        }
        EXPECT_GT(own, loudest_foreign * 4.0)
            << track->name << " carries " << loudest_foreign << " at another application's frequency against " << own
            << " at its own — the tracks are bleeding into each other";
    }

    testing::Test::RecordProperty("tracks", static_cast<int>(kTargets + 1));
    testing::Test::RecordProperty("mix_ms", static_cast<int>(mix_seconds * 1000.0));
    testing::Test::RecordProperty("worst_isolation_ratio", static_cast<int>(worst_ratio));
    testing::Test::RecordProperty("worst_isolation_track", static_cast<int>(worst_track));
}

TEST_F(RealTargetTest, AudioFromAChildProcessReachesTheTrackAimedAtItsParent) {
    // SPEC.md §8.6 targets a PID with `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`,
    // and until this case nothing exercised the *tree* half of that: the helper rendered
    // from the very process the track named, which a bare-process mode would capture just
    // as well.
    //
    // A browser is the shape this covers. `chrome.exe` the parent renders nothing at all;
    // the audio comes from a renderer child. A build that dropped the tree flag would give
    // that user a silent track while their machine was audibly playing.
    require_process_loopback();

    constexpr int kRecordingSeconds = 8;
    constexpr double kTone = 2000.0;

    AudioTarget target;
    AudioTarget::Shape shape;
    shape.seconds = 7.0;
    shape.frequency = kTone;
    shape.child = true; // the parent spawns a renderer and renders nothing itself
    ASSERT_TRUE(target.launch(shape));
    ASSERT_NE(target.pid(), 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds{900}); // parent spawns, child starts

    const std::filesystem::path output = dir_->path() / "child_tree.mkv";
    AppTrackConfig parent;
    parent.name = "parent.exe";
    parent.pid = target.pid();
    const RealRecording recorded = record_real(output, kRecordingSeconds, {parent});
    ASSERT_TRUE(recorded.ok);
    ASSERT_EQ(recorded.tracks.tracks.size(), 1U);
    EXPECT_TRUE(recorded.tracks.tracks.front().ever_attached);

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), 2U);

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    const fc::test::DecodedAudioTrack* track = media.audio_track(1);
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(track, nullptr);

    // The control: the child really did play. Without it a track that captured nothing
    // would be indistinguishable from a child that rendered nothing.
    const double mix_energy = energy_at(*mix, kTone);
    ASSERT_GT(mix_energy, 1e-4) << "the child never rendered, so this proves nothing about the process tree";

    const double track_energy = energy_at(*track, kTone);
    EXPECT_GT(track_energy, 1e-4) << "the track aimed at the parent carries no energy at " << kTone
                                  << " Hz, but its child was rendering there — "
                                     "PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE is not in effect";

    testing::Test::RecordProperty("mix_energy_e6", static_cast<int>(mix_energy * 1e6));
    testing::Test::RecordProperty("track_energy_e6", static_cast<int>(track_energy * 1e6));
}

TEST_F(RealTargetTest, AnApplicationRenderingAtItsOwnRateAndOnTwoStreamsStillLandsWhole) {
    // Two more shapes of a real application, in one recording because they are
    // independent of each other:
    //
    //   * **44.1 kHz**, which is what an application playing an MP3 asks for. The audio
    //     engine resamples to the endpoint's rate; §8.6's supplied 48 kHz / float / stereo
    //     format is what process loopback then hands over, and "requesting the endpoint's
    //     native format here silently fails on some configurations" is the trap that makes
    //     it worth checking a *mismatched* source arrives intact.
    //   * **Two concurrent render clients**, which is what a browser with two tabs is. The
    //     track must carry both, because §8.6 captures a process, not a stream.
    require_process_loopback();

    constexpr int kRecordingSeconds = 8;
    constexpr double kFirstTone = 1200.0;
    // The helper spaces additional streams by 700 Hz.
    constexpr double kSecondTone = 1900.0;

    AudioTarget target;
    AudioTarget::Shape shape;
    shape.seconds = 7.0;
    shape.frequency = kFirstTone;
    shape.streams = 2;
    shape.rate = 44100;
    ASSERT_TRUE(target.launch(shape));
    ASSERT_NE(target.pid(), 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    const std::filesystem::path output = dir_->path() / "rate_and_streams.mkv";
    AppTrackConfig player;
    player.name = "player.exe";
    player.pid = target.pid();
    const RealRecording recorded = record_real(output, kRecordingSeconds, {player});
    ASSERT_TRUE(recorded.ok);
    ASSERT_EQ(recorded.tracks.tracks.size(), 1U);

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), 2U);

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    const fc::test::DecodedAudioTrack* track = media.audio_track(1);
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(track, nullptr);

    // §8.6 supplies 48 kHz whatever the application asked for, and the encoder pins it
    // (§8.5). A track that came back at 44.1 kHz would mean the supplied format was not
    // supplied.
    EXPECT_EQ(track->sample_rate, 48000) << "the per-application track is not at the supplied 48 kHz";
    EXPECT_EQ(track->channels, 2) << "the per-application track is not the supplied stereo";

    for (const double tone : {kFirstTone, kSecondTone}) {
        ASSERT_GT(energy_at(*mix, tone), 1e-4) << "the application never rendered at " << tone << " Hz";
        EXPECT_GT(energy_at(*track, tone), 1e-4)
            << "the track is missing the application's " << tone << " Hz stream; §8.6 captures a process, not a stream";
    }

    testing::Test::RecordProperty("track_rate", track->sample_rate);
    testing::Test::RecordProperty("first_tone_e6", static_cast<int>(energy_at(*track, kFirstTone) * 1e6));
    testing::Test::RecordProperty("second_tone_e6", static_cast<int>(energy_at(*track, kSecondTone) * 1e6));
}

// ---------------------------------------------------------------------------
// Tier B alongside the three features it shares a recording with
// ---------------------------------------------------------------------------
//
// Pause, segmentation and a §5.4 device loss each existed before Tier B and each
// *inherits* rather than gains a mechanism when it is on — every track is handed the
// same `PauseClock` the pacer gets, the rollover reopens with the same stream specs the
// first file used, and a same-adapter rebuild touches neither the muxer nor any audio
// path. "Inherits the mechanism" is an argument. BUG-044 is what happens when an
// argument of exactly that shape is not checked.

TEST_F(MultitrackTest, PauseAndResumeExciseTheSameTimeFromEveryTrack) {
    // SPEC.md §7.5's failure mode is "paused wall-clock time excised from one stream's
    // timeline but not the other's", and its answer is **one shared `paused_total_ns`**.
    // Tier B gives that number five more consumers, so what this asserts is that all of
    // them subtracted the same thing: every track the same length, and every track's
    // signal still where the system mix's is.
    constexpr int kSeconds = 12;
    const std::filesystem::path paused_output = dir_->path() / "paused_tracks.mkv";
    const std::filesystem::path control_output = dir_->path() / "unpaused_tracks.mkv";

    // Three pauses of 1 s, 2 s and 0.5 s — uneven, because equal ones can hide an error
    // that scales with the pause count.
    const std::vector<PauseSpan> pauses{
        {2 * kFps, 3 * kFps},
        {5 * kFps, 7 * kFps},
        {9 * kFps, (9 * kFps) + (kFps / 2)},
    };
    ASSERT_TRUE(record(paused_output, kSeconds, 3, nullptr, &pauses));
    ASSERT_TRUE(record(control_output, kSeconds, 3));

    fc::test::DecodedMedia paused;
    fc::test::DecodedMedia control;
    fc::test::decode_media(paused_output, fc::test::DecodeOptions{true, true, false}, paused);
    fc::test::decode_media(control_output, fc::test::DecodeOptions{true, true, false}, control);
    ASSERT_TRUE(paused.opened) << paused.detail;
    ASSERT_TRUE(control.opened) << control.detail;
    ASSERT_EQ(paused.audio_track_count(), 4U);
    ASSERT_EQ(control.audio_track_count(), 4U);

    const fc::test::DecodedAudioTrack* mix = paused.audio_track(0);
    ASSERT_NE(mix, nullptr);
    ASSERT_FALSE(mix->planes.empty());
    const double mix_seconds = static_cast<double>(mix->planes.front().size()) / kRate;
    const double control_seconds = static_cast<double>(control.audio_track(0)->planes.front().size()) / kRate;

    // 3.5 s of the 12 are gone, and they are gone from the file rather than from one
    // track. Asserted against the control rather than against a constant, so the figure
    // is "the pauses and nothing else".
    constexpr double kExcisedSeconds = 3.5;
    EXPECT_NEAR(control_seconds - mix_seconds, kExcisedSeconds, 0.15)
        << "the paused recording is " << mix_seconds << " s against an unpaused " << control_seconds << " s";

    // Every track the same length as the system mix — §8.6's "identical duration", with
    // §7.5's excision applied. A track that had *not* subtracted the paused total would
    // be 3.5 s longer, which no drift tolerance could hide.
    const std::vector<double> mix_onsets = fc::test::onset_times(*mix, 0, 128, 0.25, 0.25);
    ASSERT_GE(mix_onsets.size(), 5U) << "only " << mix_onsets.size() << " beeps survived the pauses";

    double worst_offset = 0.0;
    std::size_t worst_track = 0;
    for (std::size_t track = 1; track < 4; ++track) {
        const fc::test::DecodedAudioTrack* entry = paused.audio_track(track);
        ASSERT_NE(entry, nullptr);
        ASSERT_FALSE(entry->planes.empty());
        const double seconds = static_cast<double>(entry->planes.front().size()) / kRate;
        EXPECT_NEAR(seconds, mix_seconds, 0.05)
            << "track " << track << " is " << seconds << " s against the system mix's " << mix_seconds
            << " s across three pauses — the two subtracted different totals (SPEC.md §7.5)";

        // And the content is still where the mix's is. A track that excised the *wrong*
        // spans could still come out the right length, and would be audibly out of sync
        // — which is the failure §7.5 names and the reason this is not a length check.
        const std::vector<double> onsets = fc::test::onset_times(*entry, 0, 128, 0.25, 0.25);
        ASSERT_EQ(onsets.size(), mix_onsets.size()) << "track " << track << " has a different number of beeps";
        for (std::size_t k = 0; k < onsets.size(); ++k) {
            if (std::abs(onsets[k] - mix_onsets[k]) > std::abs(worst_offset)) {
                worst_offset = onsets[k] - mix_onsets[k];
                worst_track = track;
            }
        }
    }
    EXPECT_LT(std::abs(worst_offset), kSyncToleranceSeconds)
        << "track " << worst_track << " is " << (worst_offset * 1000.0) << " ms from the system mix after a pause";

    testing::Test::RecordProperty("paused_ms", static_cast<int>(mix_seconds * 1000.0));
    testing::Test::RecordProperty("control_ms", static_cast<int>(control_seconds * 1000.0));
    testing::Test::RecordProperty("excised_ms", static_cast<int>((control_seconds - mix_seconds) * 1000.0));
    testing::Test::RecordProperty("worst_offset_us", static_cast<int>(worst_offset * 1e6));
}

TEST_F(MultitrackTest, EverySegmentCarriesEveryTrackWithItsName) {
    // SPEC.md §11 opens a new `AVFormatContext` mid-recording, and §10.1 fixes its stream
    // set at `avformat_write_header` just as the first one's was. So a rollover that
    // reopened with fewer tracks — or with the names lost — would silently drop
    // applications at the seam, which §8.6's "never drop it from the container" forbids
    // and which no assertion in `test_segmentation.cpp` would notice, because none of
    // them looks at audio.
    constexpr int kSegmentMinutes = 1;
    constexpr int kSeconds = 150; // two and a half segments
    fc::config::SegmentationSettings segmentation;
    segmentation.enabled = true;
    segmentation.split_by_duration = true;
    segmentation.duration_minutes = kSegmentMinutes;
    segmentation.split_by_size = false;

    const std::filesystem::path base = dir_->path() / "segmented_tracks.mkv";
    ASSERT_TRUE(record(base, kSeconds, 3, &segmentation));

    // §11 names the first file `_part001` too, so there is no file at the base path.
    // `SegmentationTest` asserts that; this only relies on it.
    std::vector<std::filesystem::path> segments;
    for (int index = 1; index <= 8; ++index) {
        const std::filesystem::path part = fc::mux::segment_path(base, index);
        if (!std::filesystem::exists(part)) {
            break;
        }
        segments.push_back(part);
    }
    ASSERT_GE(segments.size(), 2U) << "the recording did not split, so this proves nothing about a rollover";

    for (const std::filesystem::path& segment : segments) {
        fc::test::DecodedMedia media;
        fc::test::decode_media(segment, fc::test::DecodeOptions{true, true, false}, media);
        ASSERT_TRUE(media.opened) << segment.filename().string() << ": " << media.detail;
        EXPECT_EQ(media.audio_track_count(), 4U)
            << segment.filename().string() << " carries " << media.audio_track_count()
            << " audio tracks; a segment that lost one has dropped an application at the seam";

        ASSERT_NE(media.audio_track(0), nullptr);
        EXPECT_EQ(media.audio_track(0)->name, "System Mix") << segment.filename().string();
        for (std::size_t track = 1; track < 4; ++track) {
            const fc::test::DecodedAudioTrack* entry = media.audio_track(track);
            ASSERT_NE(entry, nullptr) << segment.filename().string() << " is missing track " << track;
            ASSERT_FALSE(entry->planes.empty()) << segment.filename().string() << " track " << track << " is empty";
            // §8.6's names survive the rollover. They are set on the stream's metadata
            // before `write_header`, and a rollover writes a new header — so a name that
            // was only applied to the first file would come back empty here.
            EXPECT_EQ(entry->name, "app" + std::to_string(track) + ".exe") << segment.filename().string();
        }
    }

    testing::Test::RecordProperty("segments", static_cast<int>(segments.size()));
    testing::Test::RecordProperty("tracks_per_segment", 4);
}

TEST_F(RealTargetTest, ADeviceLossRebuildsTheEncoderWithoutDisturbingAnyAudioTrack) {
    // SPEC.md §5.4's same-adapter rebuild replaces the D3D device, the colour converter
    // and the video encoder while keeping the muxer, the pacer, the shared epoch and the
    // audio paths. With Tier B on, "the audio paths" is six of them, one of which is a
    // live process-loopback client attached to another process — and the rebuild happens
    // on the capture thread, which none of them runs on.
    //
    // Driven through `RecordingSession` rather than `VideoPipeline`, because the
    // migration procedure is the session's: the pipeline cannot pause its own capture.
    // Real audio throughout, so the tracks are the real thing while the device goes away
    // underneath them.
    require_process_loopback();

    constexpr int kRecordingSeconds = 12;
    const std::filesystem::path output = dir_->path() / "migration_tracks.mkv";

    AudioTarget target;
    AudioTarget::Shape shape;
    // Deliberately longer than the recording. This case is about a *device* going away,
    // not a target: the target has to still be attached when the counters are read, or
    // `attached` would be false for a reason that has nothing to do with §5.4. The
    // fixture terminates it on the way out.
    shape.seconds = static_cast<double>(kRecordingSeconds) + 8.0;
    shape.frequency = 1800.0;
    ASSERT_TRUE(target.launch(shape));
    ASSERT_NE(target.pid(), 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    fc::pipeline::SessionSettings settings;
    settings.output = output;
    settings.video.width = kWidth;
    settings.video.height = kHeight;
    settings.video.fps = kFps;
    settings.video.container = fc::config::Container::Mkv;
    if (const auto display = fc::capture::primary_display(); display.has_value()) {
        settings.target.monitor = display.value().monitor;
    }
    // The synthetic source rather than the desktop (CLAUDE.md §5), rebuilt on the new
    // device by the same factory — which is what makes it a fair stand-in for WGC here.
    settings.capture_factory = [](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
        fc::test::SyntheticSource::Settings source;
        source.width = kWidth;
        source.height = kHeight;
        source.fps = kFps;
        source.prerendered_frames = 8;
        source.pace_to_real_time = true;
        auto created = std::make_unique<fc::test::SyntheticSource>();
        FC_TRY(created->configure(source));
        FC_TRY(created->start(device, fc::capture::CaptureTarget{}));
        return std::unique_ptr<fc::capture::IScreenCapture>{std::move(created)};
    };
    settings.audio.source = AudioSource::SystemLoopback;
    settings.audio.multitrack.enabled = true;
    settings.audio.multitrack.source = AppTrackSource::ProcessLoopback;
    AppTrackConfig track;
    track.name = "migrating.exe";
    track.pid = target.pid();
    settings.audio.multitrack.tracks.push_back(track);

    fc::pipeline::RecordingSession session;
    const auto started = session.start(settings);
    ASSERT_TRUE(started.has_value()) << "session start failed: " << fc::error_name(started.error());

    std::this_thread::sleep_for(std::chrono::seconds{4});
    // The same seam §20 row 11 uses: the HRESULT the driver would have supplied, at the
    // point `DeviceWatcher::report_device_error` receives it. Everything after is the
    // production path.
    session.inject_device_error(static_cast<std::int32_t>(0x887A0005)); // DXGI_ERROR_DEVICE_REMOVED
    std::this_thread::sleep_for(std::chrono::seconds{kRecordingSeconds - 4});

    const fc::pipeline::SessionStats stats = session.stats();
    const auto report = session.stop();
    ASSERT_TRUE(report.has_value()) << "finalization failed: " << fc::error_name(report.error());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    // The event actually happened, and it stayed in one file. Asserted before anything
    // else is examined: a recording that never rebuilt would satisfy every assertion
    // below while testing nothing.
    ASSERT_GE(stats.rebuilds, 1U) << "no rebuild happened, so this proves nothing about §5.4 with Tier B on";
    ASSERT_EQ(stats.segments, 1U) << "the rebuild crossed adapters and split the file; this case needs the "
                                     "same-adapter path";
    EXPECT_LT(stats.worst_rebuild_gap_ns, 350'000'000)
        << "the rebuild took " << (stats.worst_rebuild_gap_ns / 1'000'000) << " ms against §5.4's 350 ms";

    ASSERT_EQ(stats.app_tracks.tracks.size(), 1U);
    const fc::pipeline::AppTrackStats& app = stats.app_tracks.tracks.front();
    // The client was never touched: it is attached to a process, not to a D3D device,
    // and a rebuild that tore it down would show up here as a detach it never asked for.
    EXPECT_TRUE(app.attached) << "the process-loopback client was torn down by a device rebuild";
    EXPECT_FALSE(app.target_exited);
    EXPECT_GT(app.source_buffers, 0U);

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), 2U) << "a track was lost across the rebuild";

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    const fc::test::DecodedAudioTrack* app_track = media.audio_track(1);
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(app_track, nullptr);
    ASSERT_FALSE(mix->planes.empty());
    ASSERT_FALSE(app_track->planes.empty());
    EXPECT_EQ(app_track->name, "migrating.exe");

    const double mix_seconds = static_cast<double>(mix->planes.front().size()) / std::max(mix->sample_rate, 1);
    const double app_seconds =
        static_cast<double>(app_track->planes.front().size()) / std::max(app_track->sample_rate, 1);
    EXPECT_NEAR(app_seconds, mix_seconds, 0.5)
        << "the per-application track is " << app_seconds << " s against the system mix's " << mix_seconds << " s";

    // And the audio spans the rebuild rather than stopping at it. The target rendered
    // throughout; a track that lost its second half would still be the right *length*,
    // because the watchdog would have padded it.
    const std::vector<float>& samples = app_track->planes.front();
    const auto midpoint = static_cast<std::size_t>(samples.size() / 2);
    const std::vector<float> before{samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(midpoint)};
    const std::vector<float> after{samples.begin() + static_cast<std::ptrdiff_t>(midpoint), samples.end()};
    const double before_energy = fc::test::tone_energy(before, shape.frequency, app_track->sample_rate);
    const double after_energy = fc::test::tone_energy(after, shape.frequency, app_track->sample_rate);
    EXPECT_GT(before_energy, 1e-4) << "the track carries nothing before the rebuild";
    EXPECT_GT(after_energy, 1e-4) << "the track carries nothing after the rebuild; the audio stopped at the seam";

    testing::Test::RecordProperty("rebuilds", static_cast<int>(stats.rebuilds));
    testing::Test::RecordProperty("worst_gap_ms", static_cast<int>(stats.worst_rebuild_gap_ns / 1'000'000));
    testing::Test::RecordProperty("app_track_ms", static_cast<int>(app_seconds * 1000.0));
    testing::Test::RecordProperty("mix_track_ms", static_cast<int>(mix_seconds * 1000.0));
    testing::Test::RecordProperty("before_energy_e6", static_cast<int>(before_energy * 1e6));
    testing::Test::RecordProperty("after_energy_e6", static_cast<int>(after_energy * 1e6));
}

TEST_F(RealTargetTest, ATrackReattachesWhenItsApplicationIsClosedAndReopened) {
    // SPEC.md §8.6 says a track whose target exits "continues as pure silence to the end
    // of the file" and leaves open whether it keeps *looking*. **Decided 2026-08-06 by
    // the owner: it looks**, following the **executable** — the same identity §8.6
    // already uses for a target that has not started yet.
    //
    // Two separate processes, both `fc_audio_target.exe`, playing the same tone. The
    // first exits a third of the way in; the second starts a third later. Between them
    // the track must be silent, and either side of that gap it must carry the tone —
    // which is the whole claim, and which no length assertion could see, because the
    // track is the full length under either reading.
    require_process_loopback();

    constexpr int kRecordingSeconds = 14;
    constexpr double kTone = 1700.0;
    constexpr double kFirstSeconds = 3.5;
    constexpr double kSecondLaunchAt = 8.0;
    constexpr double kSecondSeconds = 5.0;

    AudioTarget first;
    AudioTarget::Shape shape;
    shape.seconds = kFirstSeconds;
    shape.frequency = kTone;
    ASSERT_TRUE(first.launch(shape));
    ASSERT_NE(first.pid(), 0U);
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    AppTrackConfig track;
    track.name = "fc_audio_target.exe";
    // **By name, not by pid.** That is the point of the case: the pid the recording
    // starts with will be gone by the end, and only an executable can be followed back.
    track.executable = "fc_audio_target.exe";

    AudioTarget second;
    AudioTarget::Shape replacement;
    replacement.seconds = kSecondSeconds;
    replacement.frequency = kTone;
    bool launched = false;

    const std::filesystem::path output = dir_->path() / "reattach.mkv";
    const RealRecording recorded = record_real(output, kRecordingSeconds, {track}, [&](double elapsed) {
        if (!launched && elapsed >= kSecondLaunchAt) {
            launched = true;
            EXPECT_TRUE(second.launch(replacement)) << "could not start the replacement target";
        }
    });
    ASSERT_TRUE(recorded.ok);
    ASSERT_TRUE(launched) << "the replacement was never launched";
    ASSERT_NE(second.pid(), 0U);
    ASSERT_NE(second.pid(), first.pid()) << "the replacement reused the pid; this case needs two processes";

    ASSERT_EQ(recorded.tracks.tracks.size(), 1U);
    const fc::pipeline::AppTrackStats& app = recorded.tracks.tracks.front();
    EXPECT_TRUE(app.ever_attached);
    // The event happened, and it happened the way the case needs. Asserted before the
    // content, so a run where the first target outlived the recording cannot pass by
    // never having exited at all.
    EXPECT_GE(app.exits, 1U) << "the first target never exited";
    EXPECT_GE(app.reattachments, 1U) << "the track did not follow its executable back (SPEC.md §8.6, decided "
                                        "2026-08-06)";

    testing::Test::RecordProperty("exits", static_cast<int>(app.exits));
    testing::Test::RecordProperty("reattachments", static_cast<int>(app.reattachments));
    testing::Test::RecordProperty("buffers", static_cast<int>(app.source_buffers));

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_EQ(media.audio_track_count(), 2U);

    const fc::test::DecodedAudioTrack* mix = media.audio_track(0);
    const fc::test::DecodedAudioTrack* app_track = media.audio_track(1);
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(app_track, nullptr);
    ASSERT_FALSE(app_track->planes.empty());

    // §8.6's "never truncate a track" holds under either reading, so it is a control
    // here rather than the claim.
    const double mix_seconds = static_cast<double>(mix->planes.front().size()) / std::max(mix->sample_rate, 1);
    const double app_seconds =
        static_cast<double>(app_track->planes.front().size()) / std::max(app_track->sample_rate, 1);
    EXPECT_NEAR(app_seconds, mix_seconds, 0.5);

    // Three windows: while the first target played, while nothing did, and while the
    // replacement did. The middle one is what makes the outer two mean something.
    const std::vector<float>& samples = app_track->planes.front();
    const int rate = std::max(app_track->sample_rate, 1);
    const auto window = [&](double from, double to) {
        const auto begin = std::min(static_cast<std::size_t>(from * rate), samples.size());
        const auto end = std::min(static_cast<std::size_t>(to * rate), samples.size());
        return std::vector<float>{samples.begin() + static_cast<std::ptrdiff_t>(begin),
                                  samples.begin() + static_cast<std::ptrdiff_t>(end)};
    };

    const double before = fc::test::tone_energy(window(0.5, kFirstSeconds - 0.5), kTone, rate);
    const double gap = fc::test::tone_energy(window(kFirstSeconds + 1.0, kSecondLaunchAt - 0.5), kTone, rate);
    const double after =
        fc::test::tone_energy(window(kSecondLaunchAt + 1.5, kSecondLaunchAt + kSecondSeconds - 0.5), kTone, rate);

    EXPECT_GT(before, 1e-4) << "the track carries nothing while the first target was playing";
    EXPECT_GT(after, 1e-4) << "the track carries nothing after the application was reopened — it did not re-attach";
    EXPECT_LT(gap, before / 8.0) << "the track is not silent between the two targets, so the two windows either side "
                                    "prove nothing about re-attachment";

    testing::Test::RecordProperty("before_e6", static_cast<int>(before * 1e6));
    testing::Test::RecordProperty("gap_e6", static_cast<int>(gap * 1e6));
    testing::Test::RecordProperty("after_e6", static_cast<int>(after * 1e6));
}

} // namespace
