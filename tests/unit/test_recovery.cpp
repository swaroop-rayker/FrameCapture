// The recovery sidecar (SPEC.md §10.4).
//
// CPU TIER. The sidecar is a small JSON file and a few filesystem rules, none of
// which needs hardware — which matters, because the sidecar is the mechanism that
// decides whether a crashed recording is *noticed at all*. The remux it triggers
// is the GPU tier's problem; whether the claim survives being written, read back,
// and discarded is this one's.

#include "core/mux/recovery.h"

#include "core/logging/logger.h"

#include "temp_dir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>

namespace {

using fc::mux::RecoveryRecord;

RecoveryRecord sample_record(const std::filesystem::path& output) {
    RecoveryRecord record;
    record.container = fc::config::Container::Mp4;
    record.output = output;
    record.engine_version = "1.2.3";
    record.session_id = "abcdef0123456789";
    record.width = 1920;
    record.height = 1080;
    record.fps = 60;
    record.video_codec = "h264";
    record.audio_codec = "aac";
    record.audio_channels = 6;
    record.audio_sample_rate = 48000;
    return record;
}

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("recovery");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "recoverytest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
        dir_.reset();
    }

    [[nodiscard]] std::filesystem::path output_path() const {
        return dir_->path() / "capture.mp4";
    }

    std::unique_ptr<fc::test::TempDir> dir_;
};

// ---------------------------------------------------------------------------
// The path
// ---------------------------------------------------------------------------

// Appended, not substituted. `capture.mp4.fcrecover` keeps the recording's own
// name, so the two sort together in a directory listing and two recordings that
// differ only by container cannot end up sharing one sidecar.
TEST_F(RecoveryTest, TheSidecarKeepsTheRecordingsFullName) {
    EXPECT_EQ(fc::mux::sidecar_path_for("C:/videos/capture.mp4").filename().string(), "capture.mp4.fcrecover");
    EXPECT_EQ(fc::mux::sidecar_path_for("C:/videos/capture.mkv").filename().string(), "capture.mkv.fcrecover");
    // The two do not collide, which `replace_extension` would have made them do.
    EXPECT_NE(fc::mux::sidecar_path_for("C:/videos/capture.mp4"), fc::mux::sidecar_path_for("C:/videos/capture.mkv"));
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST_F(RecoveryTest, ARecordSurvivesBeingWrittenAndReadBack) {
    const RecoveryRecord written = sample_record(output_path());
    ASSERT_TRUE(fc::mux::write_recovery_record(written).has_value());

    const auto read = fc::mux::read_recovery_record(fc::mux::sidecar_path_for(output_path()));
    ASSERT_TRUE(read.has_value()) << fc::error_name(read.error());
    const RecoveryRecord& record = read.value();

    EXPECT_EQ(record.container, fc::config::Container::Mp4);
    EXPECT_EQ(record.output, written.output);
    EXPECT_EQ(record.engine_version, "1.2.3");
    EXPECT_EQ(record.session_id, "abcdef0123456789");
    EXPECT_EQ(record.width, 1920);
    EXPECT_EQ(record.height, 1080);
    EXPECT_EQ(record.fps, 60);
    EXPECT_EQ(record.video_codec, "h264");
    EXPECT_EQ(record.audio_codec, "aac");
    EXPECT_EQ(record.audio_channels, 6);
    EXPECT_EQ(record.audio_sample_rate, 48000);

    // Stamped even though the caller left it empty. A record with no start time
    // cannot tell a user which recording they are being asked about.
    EXPECT_FALSE(record.started_utc.empty());
    EXPECT_NE(record.started_utc.find('T'), std::string::npos) << record.started_utc;
    EXPECT_EQ(record.started_utc.back(), 'Z') << record.started_utc;
}

// A video-only recording says so by omission rather than by a flag, and the
// distinction has to survive: recovery uses it to decide whether a file with no
// audio stream is a defect or the intent.
TEST_F(RecoveryTest, AVideoOnlyRecordingRoundTripsWithNoAudioCodec) {
    RecoveryRecord written = sample_record(output_path());
    written.audio_codec.clear();
    written.audio_channels = 0;
    written.audio_sample_rate = 0;
    ASSERT_TRUE(fc::mux::write_recovery_record(written).has_value());

    const auto read = fc::mux::read_recovery_record(fc::mux::sidecar_path_for(output_path()));
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read.value().audio_codec.empty());
    EXPECT_EQ(read.value().audio_channels, 0);
}

// ---------------------------------------------------------------------------
// Refusing what it cannot trust
// ---------------------------------------------------------------------------

// A sidecar is written by a process that then crashed, so it is exactly the kind
// of file that can be truncated. Acting on half of one would mean repairing a
// recording against the wrong description.
TEST_F(RecoveryTest, AMalformedSidecarIsRefusedRatherThanGuessedAt) {
    const std::filesystem::path sidecar = fc::mux::sidecar_path_for(output_path());

    struct Case {
        const char* name;
        const char* contents;
    };

    const Case cases[] = {
        {"truncated", R"({"schema": 1, "container": "mp)"},
        {"empty", ""},
        {"not an object", R"(["schema", 1])"},
        {"no schema", R"({"container": "mp4", "output": "C:/x.mp4"})"},
        {"future schema", R"({"schema": 99, "container": "mp4", "output": "C:/x.mp4"})"},
        {"unknown container", R"({"schema": 1, "container": "webm", "output": "C:/x.mp4"})"},
        {"no output path", R"({"schema": 1, "container": "mp4", "output": ""})"},
    };

    for (const Case& test_case : cases) {
        {
            std::ofstream stream(sidecar, std::ios::binary | std::ios::trunc);
            stream << test_case.contents;
        }
        const auto read = fc::mux::read_recovery_record(sidecar);
        EXPECT_FALSE(read.has_value()) << test_case.name << " was accepted";
        if (!read.has_value()) {
            EXPECT_EQ(read.error(), fc::FcError::MUX_RECOVERY_FAILED) << test_case.name;
        }
    }
}

TEST_F(RecoveryTest, ReadingASidecarThatIsNotThereFails) {
    const auto read = fc::mux::read_recovery_record(dir_->path() / "nothing.fcrecover");
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), fc::FcError::MUX_RECOVERY_FAILED);
}

// ---------------------------------------------------------------------------
// The lifecycle
// ---------------------------------------------------------------------------

TEST_F(RecoveryTest, ClearingRemovesTheSidecarAndIsSafeWhenThereIsNone) {
    ASSERT_TRUE(fc::mux::write_recovery_record(sample_record(output_path())).has_value());
    ASSERT_TRUE(std::filesystem::exists(fc::mux::sidecar_path_for(output_path())));

    fc::mux::clear_recovery_record(output_path());
    EXPECT_FALSE(std::filesystem::exists(fc::mux::sidecar_path_for(output_path())));

    // Idempotent, and quiet. Finalization calls this on a path that may already be
    // clean, and a completed recording must not fail over a missing file.
    fc::mux::clear_recovery_record(output_path());
    fc::mux::clear_recovery_record(dir_->path() / "never_existed.mp4");
}

// ---------------------------------------------------------------------------
// Discovery — what the next launch does
// ---------------------------------------------------------------------------

TEST_F(RecoveryTest, DiscoveryFindsEverySidecarAndNothingElse) {
    for (const char* name : {"one.mp4", "two.mkv", "three.mp4"}) {
        RecoveryRecord record = sample_record(dir_->path() / name);
        record.container =
            std::string_view{name}.ends_with(".mkv") ? fc::config::Container::Mkv : fc::config::Container::Mp4;
        ASSERT_TRUE(fc::mux::write_recovery_record(record).has_value());
    }

    // Decoys: a finished recording, and a file whose name merely contains the word.
    {
        std::ofstream{dir_->path() / "finished.mp4", std::ios::binary} << "not a sidecar";
    }
    {
        std::ofstream{dir_->path() / "fcrecover.txt", std::ios::binary} << "nor this";
    }

    const std::vector<std::filesystem::path> found = fc::mux::find_recoverable(dir_->path());
    EXPECT_EQ(found.size(), 3u);
    for (const std::filesystem::path& sidecar : found) {
        EXPECT_EQ(sidecar.extension().string(), ".fcrecover") << sidecar.string();
        EXPECT_TRUE(fc::mux::read_recovery_record(sidecar).has_value()) << sidecar.string();
    }
}

TEST_F(RecoveryTest, DiscoveryOnAMissingDirectoryIsEmptyRatherThanAnError) {
    EXPECT_TRUE(fc::mux::find_recoverable(dir_->path() / "no_such_directory").empty());
}

// ---------------------------------------------------------------------------
// Recovering nothing
// ---------------------------------------------------------------------------

// A sidecar whose recording is gone — deleted by the user, or on a volume that
// never came back — is a claim that can never be satisfied. Leaving it would
// prompt about a missing file on every launch forever.
TEST_F(RecoveryTest, ASidecarWhoseRecordingIsGoneIsDiscarded) {
    ASSERT_TRUE(fc::mux::write_recovery_record(sample_record(output_path())).has_value());
    const std::filesystem::path sidecar = fc::mux::sidecar_path_for(output_path());

    const auto outcome = fc::mux::recover(sidecar);
    ASSERT_TRUE(outcome.has_value()) << fc::error_name(outcome.error());
    EXPECT_FALSE(outcome.value().valid);
    EXPECT_FALSE(outcome.value().repaired);
    EXPECT_FALSE(std::filesystem::exists(sidecar)) << "the unsatisfiable claim was left on disk";
}

// An empty file is the same case: the process died between creating the output and
// writing anything into it.
TEST_F(RecoveryTest, ASidecarWhoseRecordingIsEmptyIsDiscarded) {
    ASSERT_TRUE(fc::mux::write_recovery_record(sample_record(output_path())).has_value());
    {
        const std::ofstream empty(output_path(), std::ios::binary);
    }
    ASSERT_TRUE(std::filesystem::exists(output_path()));

    const auto outcome = fc::mux::recover(fc::mux::sidecar_path_for(output_path()));
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome.value().valid);
    EXPECT_FALSE(std::filesystem::exists(fc::mux::sidecar_path_for(output_path())));
}

} // namespace
