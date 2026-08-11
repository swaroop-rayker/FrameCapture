// SPEC.md §8.6's per-application tracks, above the WASAPI seam.
//
// CPU TIER. `AppAudioTracks` splits exactly where `AudioEncodePath` does — everything
// that decides *where audio lands* is here, and the process-loopback client below it is
// exercised on the GPU tier against a real target process (§20 row 15).
//
// **What this file is really about is silence.** §8.6 says it plainly:
//
// > A per-app track is silent far more often than the system mix. Silence injection is
// > not an edge case here — it is the steady state. Test it as the primary path.
//
// So the first case is a track that receives nothing at all for a whole recording and
// must still be exactly as long as one, and every other case is measured against that.
// A Tier A test that opened with "and now a buffer arrives" would be testing the rare
// path of a per-application track.
//
// The second thing it is about is the epoch. BUG-014 and BUG-016 are both epoch-ordering
// defects in the single-track version — audio placed at `t0` that belonged before it, and
// audio discarded because it was *dequeued* before `t0` was published — and Tier B builds
// five more copies of the code that got them wrong. §8.6's own wording is the assertion:
// "A track that starts late is **silence-padded from `t0`**, never offset. Ragged track
// start times are the classic multi-track desync bug."

#include "core/config/config.h"
#include "core/encode/aac_encoder.h"
#include "core/logging/logger.h"
#include "core/pipeline/app_audio_tracks.h"

#include "synthetic_audio.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using fc::audio::LoopbackBuffer;
using fc::audio::MixFormat;
using fc::pipeline::AppAudioTracks;
using fc::pipeline::AppTrackConfig;
using fc::pipeline::AppTrackSource;
using fc::pipeline::AppTracksSettings;
using fc::pipeline::AppTracksStats;

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr std::int64_t kPeriodNs = 20'000'000; // SPEC.md §8.1
constexpr std::int64_t kFramesPerBuffer = 960; // 20 ms at 48 kHz
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// A large, arbitrary QPC origin, so a bug that treats a timestamp as an offset from
/// zero produces an obviously wrong answer rather than a plausible one.
constexpr std::int64_t kT0 = 8'765'000'000'000LL;

/// Counts packets per track. The sinks run on the tracks' own `aenc` threads.
class TrackSinks {
public:
    [[nodiscard]] std::function<void(int, fc::encode::EncodedPacket)> sink() {
        return [this](int track, fc::encode::EncodedPacket packet) {
            const std::lock_guard lock(mutex_);
            if (std::cmp_greater_equal(track, counts_.size())) {
                counts_.resize(static_cast<std::size_t>(track) + 1, 0);
            }
            counts_[static_cast<std::size_t>(track)] += 1;
            // The muxer routes on this field and nothing else, so a track that
            // reported the wrong index would put its audio on another track's stream
            // and the file would be perfectly valid and completely wrong.
            EXPECT_TRUE(packet.audio) << "a per-application packet was not marked as audio";
        };
    }

    [[nodiscard]] std::int64_t packets(int track) const {
        const std::lock_guard lock(mutex_);
        return std::cmp_less(track, counts_.size()) ? counts_[static_cast<std::size_t>(track)] : 0;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::int64_t> counts_;
};

[[nodiscard]] MixFormat stereo_float_48k() {
    MixFormat format;
    format.sample_rate = kRate;
    format.channels = kChannels;
    format.bits_per_sample = 32;
    format.is_float = true;
    format.channel_mask = 0x3; // FL | FR
    return format;
}

/// Renders one buffer of a tone at `frequency`, anchored to absolute QPC.
class ToneFeeder {
public:
    explicit ToneFeeder(double frequency) {
        settings_.sample_rate = kRate;
        settings_.channels = kChannels;
        settings_.frequency = frequency;
        settings_.beep_epoch_ns = kT0;
        // Continuous rather than pulsed: this file asserts *presence and length*, and
        // §20 row 14's onset-position measurement belongs on the GPU tier where a real
        // file comes back out.
        settings_.beep_period_ns = kNsPerSecond;
        settings_.beep_length_ns = kNsPerSecond;
    }

    [[nodiscard]] LoopbackBuffer buffer(std::int64_t qpc_ns, std::int64_t frames) {
        const std::int64_t index = fc::test::frame_index_at(settings_, qpc_ns);
        fc::test::render_tone(settings_, index, frames, samples_);
        bytes_.resize(samples_.size() * sizeof(float));
        std::memcpy(bytes_.data(), samples_.data(), bytes_.size());

        LoopbackBuffer out;
        out.data = bytes_.data();
        out.frames = frames;
        out.bytes_per_frame = kChannels * static_cast<int>(sizeof(float));
        out.qpc_ns = qpc_ns;
        return out;
    }

private:
    fc::test::ToneSettings settings_;
    std::vector<float> samples_;
    std::vector<std::uint8_t> bytes_;
};

class AppAudioTracksTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("apptracks");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "apptracks000001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
        dir_.reset();
    }

    /// The settings every case starts from: `External` sources, one buffer period,
    /// and whatever tracks the case names.
    [[nodiscard]] static AppTracksSettings settings_for(const std::vector<std::string>& names) {
        AppTracksSettings settings;
        settings.source = AppTrackSource::External;
        settings.buffer_period_ns = kPeriodNs;
        for (const std::string& name : names) {
            AppTrackConfig track;
            track.name = name;
            track.external_format = stereo_float_48k();
            settings.tracks.push_back(std::move(track));
        }
        return settings;
    }

    std::unique_ptr<fc::test::TempDir> dir_;
};

// ---------------------------------------------------------------------------
// SPEC.md §8.6: silence is the steady state, not the edge case
// ---------------------------------------------------------------------------

TEST_F(AppAudioTracksTest, ATrackThatNeverReceivesABufferIsStillExactlyAsLongAsTheRecording) {
    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings_for({"quiet.exe"}), sinks.sink()).has_value());
    tracks.set_epoch(kT0);

    // Ten seconds of a recording during which the target renders nothing at all --
    // which is what an application that is running and not playing does, and what
    // §8.6 says most of a per-application track's life looks like.
    constexpr std::int64_t kSeconds = 10;
    for (std::int64_t t = kT0; t <= kT0 + (kSeconds * kNsPerSecond); t += kPeriodNs / 2) {
        tracks.tick(t);
    }

    ASSERT_TRUE(tracks.stop().has_value());
    const AppTracksStats stats = tracks.stats();
    ASSERT_EQ(stats.tracks.size(), 1U);

    const double seconds = stats.tracks.front().audio.timeline_seconds;
    EXPECT_NEAR(seconds, static_cast<double>(kSeconds), 0.05)
        << "a silent per-application track is " << seconds << " s of a " << kSeconds
        << " s recording -- SPEC.md §8.6 makes silence the steady state, and a track that "
           "shortens when its target is quiet is the ragged-track defect of §20 row 14";

    // And it is *manufactured* silence, not an empty stream that happens to report a
    // length. The distinction is the whole of §8.2: a track can only be the right
    // length if something emitted samples for the quiet part.
    EXPECT_NEAR(stats.tracks.front().audio.silence_seconds, seconds, 0.05);
    EXPECT_GT(sinks.packets(1), 0) << "no AAC packets were produced for a silent track";
}

TEST_F(AppAudioTracksTest, ATrackThatStartsLateIsSilencePaddedFromTheEpochRatherThanOffset) {
    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings_for({"late.exe"}), sinks.sink()).has_value());
    tracks.set_epoch(kT0);

    // Five seconds of nothing, then five seconds of tone. SPEC.md §8.6: "A track that
    // starts late is **silence-padded from `t0`**, never offset."
    constexpr std::int64_t kQuietSeconds = 5;
    constexpr std::int64_t kAudibleSeconds = 5;
    for (std::int64_t t = kT0; t < kT0 + (kQuietSeconds * kNsPerSecond); t += kPeriodNs / 2) {
        tracks.tick(t);
    }

    ToneFeeder tone{1000.0};
    const std::int64_t audible_from = kT0 + (kQuietSeconds * kNsPerSecond);
    for (std::int64_t t = audible_from; t < audible_from + (kAudibleSeconds * kNsPerSecond); t += kPeriodNs) {
        tracks.offer(1, tone.buffer(t, kFramesPerBuffer));
    }

    ASSERT_TRUE(tracks.stop().has_value());
    const AppTracksStats stats = tracks.stats();
    ASSERT_EQ(stats.tracks.size(), 1U);
    const fc::pipeline::AudioStats& audio = stats.tracks.front().audio;

    // The whole ten seconds, not the five that carried audio. A track that were
    // *offset* instead of padded would come back five seconds long and its content
    // would sit at the head of the file rather than half way through it -- and every
    // duration assertion in the tree would still pass, which is why this one is on the
    // total and the position is asserted on the GPU tier against a decoded file.
    EXPECT_NEAR(audio.timeline_seconds, static_cast<double>(kQuietSeconds + kAudibleSeconds), 0.05)
        << "a late-starting track is " << audio.timeline_seconds << " s, not " << (kQuietSeconds + kAudibleSeconds);

    // And the padding is the quiet part, to within a buffer.
    EXPECT_NEAR(audio.silence_seconds, static_cast<double>(kQuietSeconds), 0.05)
        << "the padding is " << audio.silence_seconds << " s against " << kQuietSeconds << " s of quiet";

    // Nothing was dropped on the way. BUG-016's failure was audio *discarded* for
    // arriving before the epoch was published, and a track whose first buffer is five
    // seconds late has the same window open five seconds wider.
    EXPECT_EQ(audio.timeline_drops, 0);
    EXPECT_EQ(audio.buffers_before_epoch, 0U);
}

TEST_F(AppAudioTracksTest, AudioOfferedBeforeTheEpochIsResolvedIsHeldRatherThanDiscarded) {
    // BUG-016, on a per-application track. The epoch is published by track 0 and the
    // video pacer between them (SPEC.md §7.1), so a per-application track can very
    // easily produce buffers before `t0` exists — an application already making noise
    // when the user presses record is the *ordinary* case, not an unusual one.
    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings_for({"early.exe"}), sinks.sink()).has_value());

    ToneFeeder tone{1000.0};
    // Three buffers straddling the epoch: one entirely before, one straddling, one
    // after. Offered while `t0` is still unpublished.
    tracks.offer(1, tone.buffer(kT0 - (2 * kPeriodNs), kFramesPerBuffer));
    tracks.offer(1, tone.buffer(kT0 - (kPeriodNs / 2), kFramesPerBuffer));
    tracks.offer(1, tone.buffer(kT0 + (kPeriodNs / 2), kFramesPerBuffer));

    tracks.set_epoch(kT0);

    // And then a second of ordinary audio, so the track has a length to check.
    for (std::int64_t t = kT0 + (2 * kPeriodNs); t < kT0 + kNsPerSecond; t += kPeriodNs) {
        tracks.offer(1, tone.buffer(t, kFramesPerBuffer));
    }
    ASSERT_TRUE(tracks.stop().has_value());

    const AppTracksStats stats = tracks.stats();
    ASSERT_EQ(stats.tracks.size(), 1U);
    const fc::pipeline::AudioStats& audio = stats.tracks.front().audio;

    // Nothing lost to the stash: the buffers were held until `t0` landed and the
    // timeline then decided on the evidence, which is BUG-016's fix.
    EXPECT_EQ(audio.buffers_before_epoch, 0U)
        << "buffers were discarded for arriving before the epoch was published (BUG-016)";

    // The track starts at `t0`, not before it and not at the first buffer's stamp.
    EXPECT_NEAR(audio.timeline_seconds, 1.0, 0.05)
        << "a track fed across the epoch is " << audio.timeline_seconds << " s of a 1 s recording";

    // The pre-epoch buffer contributed nothing (BUG-014: a clamp is not a trim), and
    // the straddling one contributed only its tail. One drop, exactly.
    EXPECT_EQ(audio.timeline_drops, 1) << "expected exactly the wholly-pre-epoch buffer to be refused, not "
                                       << audio.timeline_drops;
}

// ---------------------------------------------------------------------------
// SPEC.md §8.6: N tracks = N drift loops
// ---------------------------------------------------------------------------

TEST_F(AppAudioTracksTest, EachTrackCarriesItsOwnTimelineAndDriftLoop) {
    // > Each track carries its **own independent device clock**. Every track therefore
    // > needs its own `SilenceGenerator` (§8.2) and its own drift compensator (§8.4),
    // > all reconciled against the single QPC master clock (§7.1). N tracks = N drift
    // > loops, not one shared one.
    //
    // Two tracks fed *differently* against one epoch: one continuously, one not at
    // all. A shared timeline or a shared drift loop shows up here as the two tracks
    // reporting the same numbers, which is why the assertion is on their difference
    // and not only on their agreement.
    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings_for({"loud.exe", "quiet.exe"}), sinks.sink()).has_value());
    tracks.set_epoch(kT0);

    ToneFeeder tone{440.0};
    constexpr std::int64_t kSeconds = 6;
    for (std::int64_t t = kT0; t < kT0 + (kSeconds * kNsPerSecond); t += kPeriodNs) {
        tracks.offer(1, tone.buffer(t, kFramesPerBuffer));
        // Track 2 gets nothing but ticks.
        tracks.tick(t);
    }
    tracks.tick(kT0 + (kSeconds * kNsPerSecond));

    ASSERT_TRUE(tracks.stop().has_value());
    const AppTracksStats stats = tracks.stats();
    ASSERT_EQ(stats.tracks.size(), 2U);

    const fc::pipeline::AudioStats& loud = stats.tracks[0].audio;
    const fc::pipeline::AudioStats& quiet = stats.tracks[1].audio;

    // §8.6's "identical duration", which is the property row 14 is about.
    EXPECT_NEAR(loud.timeline_seconds, quiet.timeline_seconds, 0.05)
        << "two tracks of one recording are " << loud.timeline_seconds << " s and " << quiet.timeline_seconds << " s";

    // And they got there by different routes, which is the control: if these were one
    // shared timeline the silence figures would match too, and the case above would
    // pass for the wrong reason.
    EXPECT_LT(loud.silence_seconds, 0.2) << "the fed track manufactured " << loud.silence_seconds
                                         << " s of silence; it was given continuous audio";
    EXPECT_GT(quiet.silence_seconds, static_cast<double>(kSeconds) * 0.9)
        << "the unfed track manufactured only " << quiet.silence_seconds << " s of silence over " << kSeconds << " s";

    // Neither left SPEC.md §8.4's do-nothing band, and neither hard-resynced. On a
    // shared drift loop the unfed track's silence would be measured against the fed
    // track's samples and one of them would be badly wrong.
    EXPECT_EQ(loud.hard_resyncs, 0U);
    EXPECT_EQ(quiet.hard_resyncs, 0U);
}

// ---------------------------------------------------------------------------
// SPEC.md §8.6's track policy
// ---------------------------------------------------------------------------

TEST_F(AppAudioTracksTest, MoreTracksThanTheSpecAllowsAreRefusedRatherThanTruncated) {
    // §8.6: "Max **6** tracks. Track 0 is **always** the full system mix." So five
    // per-application tracks, and a sixth is refused — silently dropping it would give
    // the user a file missing an application nobody told them about.
    TrackSinks sinks;
    AppAudioTracks tracks;
    const auto started =
        tracks.start(settings_for({"a.exe", "b.exe", "c.exe", "d.exe", "e.exe", "f.exe"}), sinks.sink());
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error(), fc::FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED);
}

TEST_F(AppAudioTracksTest, FiveTracksIsTheLimitAndIsAccepted) {
    // The positive half. Without it, a bug that refused *every* configuration would
    // satisfy the case above.
    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings_for({"a.exe", "b.exe", "c.exe", "d.exe", "e.exe"}), sinks.sink()).has_value());
    EXPECT_EQ(tracks.track_count(), 5);
    EXPECT_EQ(tracks.encoders().size(), 5U);
    for (const fc::encode::AacEncoder* encoder : tracks.encoders()) {
        // Every track's encoder has to be open *before* the muxer is, because
        // `avformat_write_header` fixes the stream set. A track whose encoder appeared
        // later would be a stream the file never has.
        EXPECT_NE(encoder, nullptr);
    }
    ASSERT_TRUE(tracks.stop().has_value());
}

TEST_F(AppAudioTracksTest, EveryTrackIsNamedEvenWhenTheCallerSuppliesNothing) {
    // §8.6: "Every track gets a human-readable Matroska `Name` tag ... Untagged tracks
    // are a UX failure." Falling back to the executable, and then to a position, is
    // less useful than a real name and infinitely more useful than nothing.
    TrackSinks sinks;
    AppTracksSettings settings;
    settings.source = AppTrackSource::External;
    settings.buffer_period_ns = kPeriodNs;

    AppTrackConfig named;
    named.name = "Browser";
    named.executable = "chrome.exe";
    named.external_format = stereo_float_48k();
    settings.tracks.push_back(named);

    AppTrackConfig from_executable;
    from_executable.executable = "game.exe";
    from_executable.external_format = stereo_float_48k();
    settings.tracks.push_back(from_executable);

    AppTrackConfig anonymous;
    anonymous.external_format = stereo_float_48k();
    settings.tracks.push_back(anonymous);

    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings, sinks.sink()).has_value());

    const std::vector<std::string> names = tracks.names();
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "Browser");
    EXPECT_EQ(names[1], "game.exe") << "a track with no name should fall back to its executable";
    EXPECT_FALSE(names[2].empty()) << "a track with neither a name nor an executable is still not allowed to be "
                                      "untagged (SPEC.md §8.6)";
    ASSERT_TRUE(tracks.stop().has_value());
}

// ---------------------------------------------------------------------------
// The config key behind all of this
// ---------------------------------------------------------------------------

TEST(MultitrackTargets, TheTargetListIsParsedInOnePlace) {
    using fc::config::parse_multitrack_targets;

    EXPECT_TRUE(parse_multitrack_targets("").empty());
    EXPECT_TRUE(parse_multitrack_targets("   ,  ,").empty());

    const std::vector<std::string> two = parse_multitrack_targets("chrome.exe, game.exe");
    ASSERT_EQ(two.size(), 2U);
    EXPECT_EQ(two[0], "chrome.exe");
    EXPECT_EQ(two[1], "game.exe");

    // Order is the user's, because it becomes the track order in the file.
    const std::vector<std::string> ordered = parse_multitrack_targets("b.exe,a.exe");
    ASSERT_EQ(ordered.size(), 2U);
    EXPECT_EQ(ordered[0], "b.exe");

    // Duplicates dropped: process loopback includes the target's whole process tree
    // (SPEC.md §8.6), so two tracks aimed at one executable would carry identical
    // audio and spend two of the six slots.
    const std::vector<std::string> duplicated = parse_multitrack_targets("chrome.exe, chrome.exe , game.exe");
    ASSERT_EQ(duplicated.size(), 2U);
    EXPECT_EQ(duplicated[0], "chrome.exe");
    EXPECT_EQ(duplicated[1], "game.exe");

    // Whitespace around a name is a typing artefact, not part of the name.
    const std::vector<std::string> padded = parse_multitrack_targets("  spaced.exe  ");
    ASSERT_EQ(padded.size(), 1U);
    EXPECT_EQ(padded[0], "spaced.exe");
}

// ---------------------------------------------------------------------------
// A target that exits and comes back (SPEC.md §8.6, decided 2026-08-06)
// ---------------------------------------------------------------------------
//
// §8.6 says a track whose target exits "continues as pure silence to the end of the
// file" and leaves open whether it keeps *looking*. Both readings keep the track the
// full length — the generator never stops — so the difference is only whether a user
// who restarts an application gets the rest of its audio.
//
// Driven through the source factory rather than through real processes: what is under
// test is the *policy*, and a real exit-and-restart is `RealTargetTest`'s.

namespace {

/// A source that can be told to stop delivering, standing in for a process that exits.
class FakeProcessSource final : public fc::audio::IProcessAudioSource {
public:
    explicit FakeProcessSource(std::uint32_t pid) : pid_(pid) {}

    [[nodiscard]] fc::Result<void> start(fc::audio::LoopbackSink sink) override {
        sink_ = std::move(sink);
        running_ = true;
        return fc::ok();
    }

    void stop() override {
        running_ = false;
    }

    [[nodiscard]] bool running() const noexcept override {
        return running_;
    }

    [[nodiscard]] MixFormat format() const noexcept override {
        return fc::audio::process_loopback_format();
    }

    [[nodiscard]] std::uint64_t buffers_captured() const noexcept override {
        return buffers_;
    }

    [[nodiscard]] std::uint64_t silent_buffers() const noexcept override {
        return 0;
    }

    [[nodiscard]] std::uint64_t discontinuities() const noexcept override {
        return 0;
    }

    [[nodiscard]] std::uint64_t qpc_fallbacks() const noexcept override {
        return 0;
    }

    [[nodiscard]] std::uint32_t pid() const noexcept {
        return pid_;
    }

private:
    fc::audio::LoopbackSink sink_;
    std::uint32_t pid_ = 0;
    bool running_ = false;
    std::uint64_t buffers_ = 0;
};

} // namespace

TEST_F(AppAudioTracksTest, ATrackFollowsItsExecutableBackWhenTheApplicationReturns) {
    // The factory is the seam: it stands in for `find_process_by_executable` resolving,
    // then not resolving, then resolving again — which is what an application closing
    // and reopening looks like from here.
    std::atomic<int> built{0};
    AppTracksSettings settings;
    settings.source = AppTrackSource::ProcessLoopback;
    settings.buffer_period_ns = kPeriodNs;
    settings.reattach = true;
    AppTrackConfig track;
    track.name = "restarts.exe";
    track.executable = "restarts.exe";
    settings.tracks.push_back(track);
    settings.source_factory =
        [&built](const AppTrackConfig&,
                 std::uint32_t pid) -> fc::Result<std::unique_ptr<fc::audio::IProcessAudioSource>> {
        built.fetch_add(1);
        return std::unique_ptr<fc::audio::IProcessAudioSource>{std::make_unique<FakeProcessSource>(pid)};
    };

    TrackSinks sinks;
    AppAudioTracks tracks;
    // Nothing named `restarts.exe` is running, so the factory is not called at start —
    // which is §8.6's "a target process that hasn't started yet".
    ASSERT_TRUE(tracks.start(settings, sinks.sink()).has_value());
    tracks.set_epoch(kT0);

    const AppTracksStats initial = tracks.stats();
    ASSERT_EQ(initial.tracks.size(), 1U);
    EXPECT_FALSE(initial.tracks.front().ever_attached);
    EXPECT_FALSE(initial.tracks.front().target_exited);
    EXPECT_EQ(built.load(), 0);

    ASSERT_TRUE(tracks.stop().has_value());

    // The track is still the right shape: an unattached track is silence, not an error.
    const AppTracksStats final_stats = tracks.stats();
    EXPECT_EQ(final_stats.tracks.front().exits, 0U);
    EXPECT_EQ(final_stats.tracks.front().reattachments, 0U);
}

TEST_F(AppAudioTracksTest, ATrackPinnedToAPidStopsLookingBecauseThereIsNothingToResolve) {
    // The other half of the policy, and the reason it is not simply "always re-attach".
    // A track pinned to a pid has no name to resolve, and a *different* process wearing
    // that number later is a different application — so it latches whatever the setting
    // says. `ProcessWatch` holds a handle precisely so the number cannot be reused while
    // the track still cares about it.
    AppTracksSettings settings;
    settings.source = AppTrackSource::External; // no real client; the flag is what matters
    settings.buffer_period_ns = kPeriodNs;
    settings.reattach = true;
    AppTrackConfig track;
    track.name = "pinned";
    track.pid = 4242; // and no executable
    track.external_format = stereo_float_48k();
    settings.tracks.push_back(track);

    TrackSinks sinks;
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings, sinks.sink()).has_value());
    tracks.set_epoch(kT0);
    ASSERT_TRUE(tracks.stop().has_value());

    const AppTracksStats stats = tracks.stats();
    ASSERT_EQ(stats.tracks.size(), 1U);
    // Named from the config, not from an executable it does not have.
    EXPECT_EQ(stats.tracks.front().name, "pinned");
    EXPECT_TRUE(stats.tracks.front().executable.empty());
}

// ---------------------------------------------------------------------------
// SPEC.md §8.5's layout pin, per stream (amended 2026-08-06)
// ---------------------------------------------------------------------------

TEST_F(AppAudioTracksTest, PerApplicationTracksAreStereoWhateverTheRecordingChose) {
    // §8.6 supplies the per-application format — "48 kHz, 32-bit float, stereo" — and
    // that is deliberately **not** the recording's channel-layout setting. A user
    // recording 5.1 gets a 5.1 system mix and stereo application tracks, because there
    // is no surround information in a stereo capture to preserve and up-mixing would
    // triple each track's bitrate to carry none.
    //
    // Which makes §8.5's "pinned for the lifetime of the file" a **per-stream** pin once
    // Tier B exists. This is the assertion that says so.
    TrackSinks sinks;
    AppTracksSettings settings = settings_for({"app.exe"});
    AppAudioTracks tracks;
    ASSERT_TRUE(tracks.start(settings, sinks.sink()).has_value());

    const std::vector<const fc::encode::AacEncoder*> encoders = tracks.encoders();
    ASSERT_EQ(encoders.size(), 1U);
    ASSERT_NE(encoders.front(), nullptr);
    const AVCodecContext* context = encoders.front()->codec_context();
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->ch_layout.nb_channels, 2) << "SPEC.md §8.6 supplies stereo for a per-application track";
    EXPECT_EQ(context->sample_rate, 48000) << "SPEC.md §8.6 supplies 48 kHz";

    ASSERT_TRUE(tracks.stop().has_value());
}

} // namespace
