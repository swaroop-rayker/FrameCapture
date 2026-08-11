#include "core/config/config.h"

#include "core/config/config_schema.h"
#include "core/util/atomic_write.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using fc::config::Config;
using fc::config::Container;
using fc::config::LoadOutcome;
using fc::config::VideoCodec;
using fc::config::Warning;
using fc::config::WarningKind;

std::vector<Warning> warnings_of_kind(const std::vector<Warning>& warnings, WarningKind kind) {
    std::vector<Warning> matched;
    std::ranges::copy_if(warnings, std::back_inserter(matched), [kind](const Warning& w) { return w.kind == kind; });
    return matched;
}

bool has_warning(const std::vector<Warning>& warnings, WarningKind kind, std::string_view key) {
    return std::ranges::any_of(warnings, [kind, key](const Warning& w) { return w.kind == kind && w.key == key; });
}

LoadOutcome must_load(std::string_view text) {
    auto result = fc::config::load_from_string(text);
    EXPECT_TRUE(result.has_value()) << "load_from_string failed unexpectedly";
    return result.has_value() ? std::move(result).value() : LoadOutcome{};
}

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream stream{path, std::ios::binary};
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

// SPEC.md §11 and CLAUDE.md §2 rule 7. This is a hard project invariant, not a
// preference: segmentation silently on would produce multiple files a user did not
// ask for.
TEST(ConfigDefaults, SegmentationIsDisabled) {
    EXPECT_FALSE(fc::config::defaults().segmentation.enabled);
    EXPECT_FALSE(must_load("").config.segmentation.enabled);
    // Even a file that mentions the section must not turn it on.
    EXPECT_FALSE(must_load("[segmentation]\nduration_minutes = 5\n").config.segmentation.enabled);
}

// SPEC.md §2.2 item 4: only h264 is validated in v1.
TEST(ConfigDefaults, CodecIsH264) {
    EXPECT_EQ(fc::config::defaults().video.codec, VideoCodec::H264);
    EXPECT_EQ(must_load("").config.video.codec, VideoCodec::H264);
}

TEST(ConfigDefaults, MatchTheDocumentedValues) {
    const Config config = fc::config::defaults();

    EXPECT_EQ(config.schema_version, fc::config::kCurrentSchemaVersion);
    EXPECT_EQ(config.video.width, 1920);
    EXPECT_EQ(config.video.height, 1080);
    EXPECT_EQ(config.video.fps, 60);
    EXPECT_EQ(config.video.container, Container::Mkv); // SPEC.md §10.2 recommends MKV
    EXPECT_EQ(config.video.rate_control, fc::config::RateControl::Cqp);
    EXPECT_EQ(config.video.cqp, 20);        // SPEC.md §9
    EXPECT_EQ(config.video.gop_seconds, 2); // SPEC.md §9
    EXPECT_EQ(config.video.max_b_frames, 2);
    EXPECT_EQ(config.video.pacing, fc::config::PacingMode::Cfr);
    EXPECT_FALSE(config.video.full_range); // SPEC.md §6: limited range by default
    EXPECT_EQ(config.audio.bitrate_kbps, 0);
    EXPECT_EQ(config.audio.max_tracks, 6); // SPEC.md §8.6
    EXPECT_FALSE(config.audio.multitrack_enabled);
    EXPECT_TRUE(config.advanced.capture_cursor);
    EXPECT_TRUE(config.advanced.hdr_tonemap);
    EXPECT_TRUE(config.updates.check_enabled);
    EXPECT_FALSE(config.general.output_directory.empty());
}

// ---------------------------------------------------------------------------
// Validation: never crash, never silently accept a bad value
// ---------------------------------------------------------------------------

TEST(ConfigValidation, AcceptsAValidFile) {
    const LoadOutcome outcome = must_load(R"(
schema_version = 1
[video]
fps = 30
container = "mp4"
cqp = 18
[audio]
channel_layout = "5.1"
)");

    EXPECT_EQ(outcome.config.video.fps, 30);
    EXPECT_EQ(outcome.config.video.container, Container::Mp4);
    EXPECT_EQ(outcome.config.video.cqp, 18);
    EXPECT_EQ(outcome.config.audio.channel_layout, fc::config::ChannelLayoutSetting::Surround51);
    EXPECT_TRUE(warnings_of_kind(outcome.warnings, WarningKind::WrongType).empty());
    EXPECT_TRUE(warnings_of_kind(outcome.warnings, WarningKind::Clamped).empty());
}

TEST(ConfigValidation, WrongTypeFallsBackToTheDefaultAndWarns) {
    const LoadOutcome outcome = must_load("[video]\ncqp = \"not a number\"\nfull_range = 7\n");

    EXPECT_EQ(outcome.config.video.cqp, 20);
    EXPECT_FALSE(outcome.config.video.full_range);
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::WrongType, "video.cqp"));
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::WrongType, "video.full_range"));
}

TEST(ConfigValidation, UnknownEnumValueFallsBackToTheDefaultAndWarns) {
    const LoadOutcome outcome = must_load("[video]\ncontainer = \"avi\"\nrate_control = \"cbr\"\n");

    EXPECT_EQ(outcome.config.video.container, Container::Mkv);
    // SPEC.md §9 rejects CBR outright; a file asking for it must not get it.
    EXPECT_EQ(outcome.config.video.rate_control, fc::config::RateControl::Cqp);
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::UnknownValue, "video.container"));
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::UnknownValue, "video.rate_control"));
}

TEST(ConfigValidation, MissingKeysUseDefaultsAndAreReported) {
    const LoadOutcome outcome = must_load("");
    // A fresh file reports one Missing warning per schema key; the GUI treats these
    // as informational rather than as problems.
    EXPECT_EQ(warnings_of_kind(outcome.warnings, WarningKind::Missing).size(), fc::config::schema_keys().size());
}

TEST(ConfigValidation, NeverThrowsOnHostileInput) {
    // Structurally valid TOML, semantically nonsense. Must produce a usable config.
    const LoadOutcome outcome = must_load(R"(
schema_version = -9999
[video]
fps = 0
cqp = 9999
bitrate_kbps = -1
gop_seconds = 100000
max_b_frames = 99
container = 42
[audio]
max_tracks = 0
[segmentation]
enabled = "yes"
duration_minutes = 0
)");

    EXPECT_EQ(outcome.config.video.fps, 60); // choice: falls back
    EXPECT_EQ(outcome.config.video.cqp, 51); // range: clamps
    EXPECT_EQ(outcome.config.video.bitrate_kbps, 1000);
    EXPECT_EQ(outcome.config.video.gop_seconds, 10);
    EXPECT_EQ(outcome.config.video.max_b_frames, 4);
    EXPECT_EQ(outcome.config.video.container, Container::Mkv);
    EXPECT_EQ(outcome.config.audio.max_tracks, 1);
    EXPECT_FALSE(outcome.config.segmentation.enabled);
    EXPECT_EQ(outcome.config.segmentation.duration_minutes, 1);
}

TEST(ConfigValidation, UnparseableFileIsAnErrorRatherThanASilentReset) {
    const auto result = fc::config::load_from_string("this is [not valid TOML");
    ASSERT_FALSE(result.has_value());
    // Replacing an unparseable file with defaults would discard settings the user
    // can still fix by hand.
    EXPECT_EQ(result.error(), fc::FcError::IO_CONFIG_PARSE_FAILED);
}

// ---------------------------------------------------------------------------
// Range clamping
// ---------------------------------------------------------------------------

TEST(ConfigClamping, ClampsContinuousRangesToTheNearestBound) {
    const LoadOutcome low = must_load("[video]\ncqp = -5\nbitrate_kbps = 1\n[audio]\nmax_tracks = -3\n");
    EXPECT_EQ(low.config.video.cqp, 0);
    EXPECT_EQ(low.config.video.bitrate_kbps, 1000);
    EXPECT_EQ(low.config.audio.max_tracks, 1);

    const LoadOutcome high = must_load("[video]\ncqp = 100\nbitrate_kbps = 999999999\n[audio]\nmax_tracks = 50\n");
    EXPECT_EQ(high.config.video.cqp, 51);
    EXPECT_EQ(high.config.video.bitrate_kbps, 200000);
    EXPECT_EQ(high.config.audio.max_tracks, 6);
}

TEST(ConfigClamping, ReportsEveryClamp) {
    const LoadOutcome outcome = must_load("[video]\ncqp = 100\n");
    ASSERT_TRUE(has_warning(outcome.warnings, WarningKind::Clamped, "video.cqp"));
    const auto clamped = warnings_of_kind(outcome.warnings, WarningKind::Clamped);
    ASSERT_EQ(clamped.size(), 1u);
    EXPECT_NE(clamped.front().detail.find("51"), std::string::npos) << clamped.front().detail;
}

TEST(ConfigClamping, DiscreteChoicesFallBackRatherThanClamp) {
    // 45 fps is between the allowed values; clamping to 60 would be a guess, and
    // silently recording at a rate the user did not ask for.
    const LoadOutcome outcome = must_load("[video]\nfps = 45\n");
    EXPECT_EQ(outcome.config.video.fps, 60);
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::UnknownValue, "video.fps"));
    EXPECT_FALSE(has_warning(outcome.warnings, WarningKind::Clamped, "video.fps"));
}

TEST(ConfigClamping, BoundaryValuesAreAcceptedUnchanged) {
    const LoadOutcome outcome = must_load("[video]\ncqp = 0\ngop_seconds = 10\n[audio]\nbitrate_kbps = 640\n");
    EXPECT_EQ(outcome.config.video.cqp, 0);
    EXPECT_EQ(outcome.config.video.gop_seconds, 10);
    EXPECT_EQ(outcome.config.audio.bitrate_kbps, 640);
    EXPECT_TRUE(warnings_of_kind(outcome.warnings, WarningKind::Clamped).empty());
}

// ---------------------------------------------------------------------------
// Unknown-key preservation -- SPEC.md §17's downgrade guarantee
// ---------------------------------------------------------------------------

TEST(ConfigUnknownKeys, SurviveALoadSaveRoundTrip) {
    const std::string original = R"(
schema_version = 1

[video]
fps = 30
future_video_option = "keep me"

[from_a_newer_build]
some_flag = true
some_number = 1234
nested = { a = 1, b = "two" }
)";

    const LoadOutcome outcome = must_load(original);
    const std::string written = fc::config::serialize(outcome.config);

    EXPECT_NE(written.find("future_video_option"), std::string::npos) << written;
    EXPECT_NE(written.find("keep me"), std::string::npos) << written;
    EXPECT_NE(written.find("from_a_newer_build"), std::string::npos) << written;
    EXPECT_NE(written.find("1234"), std::string::npos) << written;

    // And the result must still parse, which is what a duplicated section header
    // would break.
    const LoadOutcome reloaded = must_load(written);
    EXPECT_EQ(reloaded.config.video.fps, 30);
}

TEST(ConfigUnknownKeys, AreReportedSoTheUserKnowsTheyWereIgnored) {
    const LoadOutcome outcome = must_load("[video]\nmystery = 1\n");
    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::UnknownKeyPreserved, "video.mystery"));
}

TEST(ConfigUnknownKeys, KnownKeysAreNotReportedAsUnknown) {
    const LoadOutcome outcome = must_load("[video]\nfps = 60\ncqp = 20\n");
    EXPECT_FALSE(has_warning(outcome.warnings, WarningKind::UnknownKeyPreserved, "video.fps"));
    EXPECT_FALSE(has_warning(outcome.warnings, WarningKind::UnknownKeyPreserved, "video.cqp"));
}

TEST(ConfigUnknownKeys, SurviveRepeatedRoundTrips) {
    // A downgrade is not a single save: the older build may run for weeks.
    std::string text = "[future]\nkeep = \"forever\"\n";
    for (int i = 0; i < 5; ++i) {
        text = fc::config::serialize(must_load(text).config);
    }
    EXPECT_NE(text.find("keep"), std::string::npos) << text;
    EXPECT_NE(text.find("forever"), std::string::npos) << text;
}

TEST(ConfigUnknownKeys, ANewerSchemaIsPreservedAndFlagged) {
    const LoadOutcome outcome = must_load("schema_version = 99\n[video]\nfps = 30\nunknown_v99 = true\n");

    EXPECT_TRUE(has_warning(outcome.warnings, WarningKind::SchemaTooNew, "schema_version"));
    // Still usable, and nothing lost.
    EXPECT_EQ(outcome.config.video.fps, 30);
    EXPECT_NE(fc::config::serialize(outcome.config).find("unknown_v99"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Serialisation round trip
// ---------------------------------------------------------------------------

TEST(ConfigSerialisation, DefaultsRoundTripExactly) {
    const Config original = fc::config::defaults();
    const LoadOutcome reloaded = must_load(fc::config::serialize(original));

    EXPECT_EQ(reloaded.config.video.fps, original.video.fps);
    EXPECT_EQ(reloaded.config.video.container, original.video.container);
    EXPECT_EQ(reloaded.config.video.codec, original.video.codec);
    EXPECT_EQ(reloaded.config.video.cqp, original.video.cqp);
    EXPECT_EQ(reloaded.config.video.pacing, original.video.pacing);
    EXPECT_EQ(reloaded.config.audio.channel_layout, original.audio.channel_layout);
    EXPECT_EQ(reloaded.config.audio.max_tracks, original.audio.max_tracks);
    EXPECT_EQ(reloaded.config.segmentation.enabled, original.segmentation.enabled);
    EXPECT_EQ(reloaded.config.advanced.log_level, original.advanced.log_level);
    EXPECT_EQ(reloaded.config.updates.check_enabled, original.updates.check_enabled);
    EXPECT_EQ(reloaded.config.general.output_directory, original.general.output_directory);

    // A serialised default file is complete, so nothing should be missing.
    EXPECT_TRUE(warnings_of_kind(reloaded.warnings, WarningKind::Missing).empty());
}

TEST(ConfigSerialisation, EveryNonDefaultValueSurvives) {
    Config config = fc::config::defaults();
    config.video.fps = 30;
    config.video.container = Container::Mp4;
    config.video.rate_control = fc::config::RateControl::Lossless;
    config.video.cqp = 33;
    config.video.pacing = fc::config::PacingMode::Vfr;
    config.video.full_range = true;
    config.audio.channel_layout = fc::config::ChannelLayoutSetting::Surround71;
    config.audio.bitrate_kbps = 512;
    config.audio.multitrack_enabled = true;
    config.audio.max_tracks = 3;
    config.segmentation.enabled = true;
    config.segmentation.size_mb = 2048;
    config.advanced.capture_backend = fc::config::CaptureBackend::Dda;
    config.advanced.log_level = fc::config::LogLevelSetting::Trace;
    config.general.language = "de";

    const LoadOutcome reloaded = must_load(fc::config::serialize(config));

    EXPECT_EQ(reloaded.config.video.fps, 30);
    EXPECT_EQ(reloaded.config.video.container, Container::Mp4);
    EXPECT_EQ(reloaded.config.video.rate_control, fc::config::RateControl::Lossless);
    EXPECT_EQ(reloaded.config.video.cqp, 33);
    EXPECT_EQ(reloaded.config.video.pacing, fc::config::PacingMode::Vfr);
    EXPECT_TRUE(reloaded.config.video.full_range);
    EXPECT_EQ(reloaded.config.audio.channel_layout, fc::config::ChannelLayoutSetting::Surround71);
    EXPECT_EQ(reloaded.config.audio.bitrate_kbps, 512);
    EXPECT_TRUE(reloaded.config.audio.multitrack_enabled);
    EXPECT_EQ(reloaded.config.audio.max_tracks, 3);
    EXPECT_TRUE(reloaded.config.segmentation.enabled);
    EXPECT_EQ(reloaded.config.segmentation.size_mb, 2048);
    EXPECT_EQ(reloaded.config.advanced.capture_backend, fc::config::CaptureBackend::Dda);
    EXPECT_EQ(reloaded.config.advanced.log_level, fc::config::LogLevelSetting::Trace);
    EXPECT_EQ(reloaded.config.general.language, "de");
}

TEST(ConfigSerialisation, AlwaysStampsTheCurrentSchemaVersion) {
    const std::string text = fc::config::serialize(fc::config::defaults());
    EXPECT_NE(text.find("schema_version"), std::string::npos) << text;
    EXPECT_EQ(must_load(text).config.schema_version, fc::config::kCurrentSchemaVersion);
}

// ---------------------------------------------------------------------------
// Disk round trip
// ---------------------------------------------------------------------------

TEST(ConfigDisk, MissingFileYieldsDefaultsWithoutCreatingOne) {
    const fc::test::TempDir dir{"config"};
    const std::filesystem::path path = dir.path() / "config.toml";

    auto result = fc::config::load(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().file_existed);
    EXPECT_EQ(result.value().config.video.fps, 60);
    // Loading must not have side effects on disk.
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ConfigDisk, SaveThenLoadPreservesEverything) {
    const fc::test::TempDir dir{"config"};
    const std::filesystem::path path = dir.path() / "config.toml";

    Config config = fc::config::defaults();
    config.video.fps = 30;
    config.segmentation.enabled = true;
    ASSERT_TRUE(fc::config::save(config, path).has_value());
    ASSERT_TRUE(std::filesystem::exists(path));

    auto result = fc::config::load(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().file_existed);
    EXPECT_EQ(result.value().config.video.fps, 30);
    EXPECT_TRUE(result.value().config.segmentation.enabled);
}

TEST(ConfigDisk, SaveIsAtomicAndLeavesNoTempBehind) {
    const fc::test::TempDir dir{"config"};
    const std::filesystem::path path = dir.path() / "config.toml";

    ASSERT_TRUE(fc::config::save(fc::config::defaults(), path).has_value());
    EXPECT_FALSE(std::filesystem::exists(fc::atomic_temp_path(path)));
    EXPECT_NE(read_file(path).find("schema_version"), std::string::npos);
}

TEST(ConfigDisk, DefaultPathIsUnderLocalAppDataAndNotProgramFiles) {
    const std::filesystem::path path = fc::config::default_config_path();
    const std::string text = path.string();
    EXPECT_NE(text.find("FrameCapture"), std::string::npos) << text;
    EXPECT_EQ(path.filename(), "config.toml");
    // SPEC.md §17: never in Program Files.
    EXPECT_EQ(text.find("Program Files"), std::string::npos) << text;
}

// ---------------------------------------------------------------------------
// Schema table integrity, and the CONFIG.md gate
// ---------------------------------------------------------------------------

TEST(ConfigSchema, KeyPathsAreUniqueAndLookUpCorrectly) {
    std::set<std::string_view> seen;
    for (const fc::config::KeySpec& spec : fc::config::schema_keys()) {
        EXPECT_TRUE(seen.insert(spec.path).second) << "duplicate key " << spec.path;
        EXPECT_EQ(fc::config::find_key(spec.path), &spec) << spec.path;
    }
    EXPECT_EQ(fc::config::find_key("no.such.key"), nullptr);
}

TEST(ConfigSchema, RangesAreCoherent) {
    for (const fc::config::KeySpec& spec : fc::config::schema_keys()) {
        EXPECT_FALSE(spec.default_repr.empty()) << spec.path << " has no documented default";
        EXPECT_GE(spec.since_version, 1) << spec.path;
        EXPECT_LE(spec.since_version, fc::config::kCurrentSchemaVersion) << spec.path;
        if (spec.type == fc::config::KeyType::Integer) {
            EXPECT_LT(spec.min, spec.max) << spec.path << " has an empty or inverted range";
        }
        if (spec.type == fc::config::KeyType::Enum || spec.type == fc::config::KeyType::IntegerChoice) {
            EXPECT_FALSE(spec.allowed.empty()) << spec.path << " lists no allowed values";
        }
    }
}

// SPEC.md §22: CONFIG.md documents "every config key: type, range, default, effect,
// schema version introduced". Enforced rather than promised.
TEST(ConfigSchema, IsFullyDocumented) {
    const std::filesystem::path doc = std::filesystem::path{FC_DOCS_DIR} / "CONFIG.md";
    ASSERT_TRUE(std::filesystem::exists(doc)) << "missing " << doc.string();

    const std::string text = read_file(doc);
    ASSERT_FALSE(text.empty());

    for (const fc::config::KeySpec& spec : fc::config::schema_keys()) {
        // Documented under its leaf name inside its section heading.
        const std::size_t dot = spec.path.rfind('.');
        const std::string_view leaf = dot == std::string_view::npos ? spec.path : spec.path.substr(dot + 1);
        EXPECT_NE(text.find(leaf), std::string::npos) << spec.path << " is not documented in CONFIG.md";
    }
}

// ---------------------------------------------------------------------------
// Container <-> file extension (BUG-043)
// ---------------------------------------------------------------------------

// The defect these prevent: a real MP4 file named `.mkv`, because the extension was a
// literal in the GUI while the container came from configuration. `start_record` now
// reads the container *from the path*, so these two functions are the contract that
// makes a recording request mean the same thing on every machine.
TEST(ConfigSchema, EveryContainerRoundTripsThroughItsExtension) {
    for (const Container value : {Container::Mkv, Container::Mp4}) {
        const std::string_view ext = fc::config::extension_for(value);
        EXPECT_FALSE(ext.empty());
        EXPECT_EQ(ext.front(), '.') << "an extension must carry its dot, or paths concatenate wrongly";

        const auto back = fc::config::container_from_extension(ext);
        ASSERT_TRUE(back.has_value()) << ext << " does not map back to a container";
        EXPECT_EQ(*back, value);
    }
}

TEST(ConfigSchema, ExtensionMatchingIgnoresCase) {
    // A file dialog hands back whatever the user typed, and `.MP4` is the same request.
    EXPECT_EQ(fc::config::container_from_extension(".MP4"), Container::Mp4);
    EXPECT_EQ(fc::config::container_from_extension(".Mkv"), Container::Mkv);
}

TEST(ConfigSchema, AnExtensionWeDoNotWriteNamesNoContainer) {
    // `nullopt` is what makes `start_record` fall back to the configured container
    // rather than guessing, so these must not accidentally resolve.
    for (const std::string_view ext : {"", ".", ".avi", ".mov", ".mkv2", "mkv", ".webm"}) {
        EXPECT_FALSE(fc::config::container_from_extension(ext).has_value())
            << ext << " was accepted as a container we write";
    }
}

} // namespace
