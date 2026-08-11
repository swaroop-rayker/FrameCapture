#include "core/logging/crash_handler.h"

#include "core/logging/logger.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using fc::Subsystem;
using fc::crash::EnginePhase;
using fc::crash::Kind;

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return {};
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// Logger + crash handler both live in a temp directory, and both are torn down
/// even if an assertion fails -- leaving the crash handler installed would make
/// a later failure in this process write artifacts instead of reporting.
class CrashFixture : public ::testing::Test {
protected:
    CrashFixture() : dir_("crash") {}

    void start(bool write_minidump) {
        fc::log::Config log_config;
        log_config.directory = dir_.path() / "logs";
        log_config.ring_capacity = 64;
        log_config.level = fc::log::Level::Trace;
        log_config.session_id = "crashsession001";
        log_config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(log_config).has_value());

        fc::crash::Config crash_config;
        crash_config.directory = dir_.path() / "crashes";
        crash_config.write_minidump = write_minidump;
        ASSERT_TRUE(fc::crash::install(crash_config).has_value());
    }

    void TearDown() override {
        fc::crash::uninstall();
        fc::log::shutdown();
    }

    fc::test::TempDir dir_;
};

TEST_F(CrashFixture, InstallReportsItself) {
    start(false);
    EXPECT_TRUE(fc::crash::is_installed());
}

TEST_F(CrashFixture, SecondInstallIsRejected) {
    start(false);
    fc::crash::Config config;
    config.directory = dir_.path() / "crashes2";
    const auto second = fc::crash::install(config);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), fc::FcError::INTERNAL_INVALID_STATE);
}

// This is the core of SPEC.md §18's "ring-buffer flush" requirement: whatever the
// engine last said before it died has to reach disk.
TEST_F(CrashFixture, RingBufferIsFlushedToDiskOnCrash) {
    start(false);

    FC_LOG_INFO(Subsystem::Capture, "acquired frame", fc::LogFields{}.add("index", 41));
    FC_LOG_ERROR(Subsystem::Gpu, "device removed", fc::LogFields{}.add_error(fc::FcError::GPU_DEVICE_REMOVED));
    FC_LOG_INFO(Subsystem::Capture, "last-words-marker");
    fc::log::flush();

    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::Terminate, nullptr);

    ASSERT_TRUE(artifacts.ring_log_written);
    ASSERT_TRUE(std::filesystem::exists(artifacts.ring_log)) << artifacts.ring_log.string();

    const std::string dumped = read_file(artifacts.ring_log);
    EXPECT_NE(dumped.find("last-words-marker"), std::string::npos);
    EXPECT_NE(dumped.find("code=2005"), std::string::npos) << "error fields must survive the dump";
    EXPECT_NE(dumped.find("crashsession001"), std::string::npos) << "dump must be correlatable with the log file";
}

TEST_F(CrashFixture, RingDumpPreservesOrderAndEvictsOldest) {
    start(false);
    for (int i = 0; i < 200; ++i) {
        FC_LOG_INFO(Subsystem::App, "entry", fc::LogFields{}.add("i", i));
    }
    fc::log::flush();

    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::Terminate, nullptr);
    ASSERT_TRUE(artifacts.ring_log_written);

    const std::string dumped = read_file(artifacts.ring_log);
    // Ring capacity is 64, so only the last 64 survive.
    EXPECT_EQ(dumped.find("i=0 "), std::string::npos);
    EXPECT_NE(dumped.find("i=199"), std::string::npos);

    const std::size_t first = dumped.find("i=136");
    const std::size_t last = dumped.find("i=199");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(last, std::string::npos);
    EXPECT_LT(first, last) << "entries must be dumped oldest-first";
}

TEST_F(CrashFixture, WritesACrashReportJson) {
    start(false);
    fc::crash::set_engine_phase(EnginePhase::Recording);

    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::PureVirtualCall, nullptr);

    ASSERT_TRUE(artifacts.report_written);
    ASSERT_TRUE(std::filesystem::exists(artifacts.report_json));

    const std::string json = read_file(artifacts.report_json);
    EXPECT_NE(json.find("\"schema_version\": 1"), std::string::npos) << json;
    EXPECT_NE(json.find("\"session_id\": \"crashsession001\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"kind\": \"pure_virtual_call\""), std::string::npos) << json;
    // SPEC.md §18: the report carries the last known state machine position.
    EXPECT_NE(json.find("\"engine_phase\": \"recording\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"timestamp_utc\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"ring_log\""), std::string::npos) << json;
}

TEST_F(CrashFixture, CrashReportJsonEscapesWindowsPathSeparators) {
    start(false);
    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::Terminate, nullptr);
    ASSERT_TRUE(artifacts.report_written);

    const std::string json = read_file(artifacts.report_json);
    // A raw backslash would make the report invalid JSON and unreadable by the
    // diagnostic bundle tooling.
    EXPECT_EQ(json.find("\\\""), std::string::npos) << "unescaped backslash before a quote: " << json;
    EXPECT_NE(json.find("\\\\"), std::string::npos) << "expected escaped separators: " << json;
}

TEST_F(CrashFixture, EnginePhaseDefaultsToUninitializedAndRoundTrips) {
    start(false);
    EXPECT_EQ(fc::crash::engine_phase(), EnginePhase::Uninitialized);
    fc::crash::set_engine_phase(EnginePhase::Finalizing);
    EXPECT_EQ(fc::crash::engine_phase(), EnginePhase::Finalizing);
    EXPECT_EQ(fc::crash::to_string(EnginePhase::Finalizing), "finalizing");
}

TEST_F(CrashFixture, EveryHandlerKindHasADistinctLabel) {
    start(false);
    EXPECT_EQ(fc::crash::to_string(Kind::UnhandledException), "unhandled_exception");
    EXPECT_EQ(fc::crash::to_string(Kind::PureVirtualCall), "pure_virtual_call");
    EXPECT_EQ(fc::crash::to_string(Kind::Terminate), "terminate");
    EXPECT_EQ(fc::crash::to_string(Kind::InvalidParameter), "invalid_parameter");
}

// Exercises MiniDumpWriteDump for real. Separated from the ring-buffer tests
// because it is the slow one.
TEST_F(CrashFixture, WritesAUsableMinidump) {
    start(true);
    FC_LOG_INFO(Subsystem::App, "pre-dump");
    fc::log::flush();

    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::UnhandledException, nullptr);

    ASSERT_TRUE(artifacts.minidump_written);
    ASSERT_TRUE(std::filesystem::exists(artifacts.minidump));
    EXPECT_GT(std::filesystem::file_size(artifacts.minidump), 4096u);

    // "MDMP" is the minidump magic; a truncated or empty file would not have it.
    const std::string head = read_file(artifacts.minidump).substr(0, 4);
    EXPECT_EQ(head, "MDMP");
}

TEST_F(CrashFixture, MinidumpIsSkippedWhenDisabled) {
    start(false);
    const fc::crash::ArtifactPaths artifacts = fc::crash::write_artifacts(Kind::Terminate, nullptr);
    EXPECT_FALSE(artifacts.minidump_written);
    EXPECT_FALSE(std::filesystem::exists(artifacts.minidump));
}

TEST_F(CrashFixture, UninstallRestoresHandlers) {
    start(false);
    ASSERT_TRUE(fc::crash::is_installed());
    fc::crash::uninstall();
    EXPECT_FALSE(fc::crash::is_installed());
}

} // namespace
