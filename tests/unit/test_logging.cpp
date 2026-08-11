#include "core/logging/logger.h"

#include "core/logging/log_fields.h"
#include "core/logging/ring_sink.h"
#include "core/logging/session_preamble.h"
#include "core/util/thread_utils.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <optional>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using fc::LogFields;
using fc::Subsystem;
using fc::log::Level;

bool any_contains(const std::vector<std::string>& lines, std::string_view needle) {
    return std::ranges::any_of(lines,
                               [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
}

/// Brings the logger up in a temp directory and guarantees teardown, so one
/// failing assertion cannot leave the process-wide logger installed.
class LoggerFixture : public ::testing::Test {
protected:
    explicit LoggerFixture() : dir_("log") {}

    void start(std::size_t ring_capacity = 2000, Level level = Level::Trace) {
        fc::log::Config config;
        config.directory = dir_.path();
        config.ring_capacity = ring_capacity;
        config.level = level;
        config.session_id = "testsession0001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
    }

    fc::test::TempDir dir_;
};

// ---------------------------------------------------------------------------
// LogFields rendering. Pure, so no logger needed.
// ---------------------------------------------------------------------------

TEST(LogFieldsTest, RendersKeyValuePairs) {
    LogFields fields;
    fields.add("monitor", "DISPLAY1").add("fps", 60);
    EXPECT_EQ(fields.render(), "monitor=DISPLAY1 fps=60");
}

TEST(LogFieldsTest, QuotesValuesThatWouldBreakParsing) {
    EXPECT_EQ(LogFields{}.add("desc", "AMD Radeon 780M").render(), "desc=\"AMD Radeon 780M\"");
    EXPECT_EQ(LogFields{}.add("empty", "").render(), "empty=\"\"");
    EXPECT_EQ(LogFields{}.add("eq", "a=b").render(), "eq=\"a=b\"");
}

TEST(LogFieldsTest, EscapesEmbeddedQuotes) {
    EXPECT_EQ(LogFields{}.add("q", "say \"hi\"").render(), "q=\"say \\\"hi\\\"\"");
}

TEST(LogFieldsTest, NeverEmitsANewline) {
    const std::string rendered = LogFields{}.add("multi", "line1\nline2").render();
    EXPECT_EQ(rendered.find('\n'), std::string::npos) << rendered;
}

TEST(LogFieldsTest, RendersTypedValues) {
    EXPECT_EQ(LogFields{}.add("b", true).render(), "b=true");
    EXPECT_EQ(LogFields{}.add("i", static_cast<std::int64_t>(-5)).render(), "i=-5");
    EXPECT_EQ(LogFields{}.add("d", 1.5).render(), "d=1.500");
    EXPECT_EQ(LogFields{}.add("null", static_cast<const char*>(nullptr)).render(), "null=<null>");
}

// SPEC.md §19 / docs/ERROR_CODES.md: the error log signature is this exact pairing.
TEST(LogFieldsTest, ErrorRendersAsNameAndCode) {
    EXPECT_EQ(LogFields{}.add_error(fc::FcError::MULTITRACK_REQUIRES_MKV).render(),
              "error=MULTITRACK_REQUIRES_MKV code=3021");
}

// ---------------------------------------------------------------------------
// Ring sink, independent of the logger.
// ---------------------------------------------------------------------------

TEST(RingSinkTest, KeepsTheMostRecentEntriesAndEvictsTheOldest) {
    fc::RingSink sink{8};
    EXPECT_EQ(sink.capacity(), 8u);
    EXPECT_EQ(sink.size(), 0u);

    for (int i = 0; i < 20; ++i) {
        // log_msg holds a string_view over the payload, so the string has to
        // outlive the log() call -- a temporary here dangles and writes garbage.
        const std::string payload = "entry" + std::to_string(i);
        const spdlog::details::log_msg msg{"test", spdlog::level::info, payload};
        sink.log(msg);
    }

    EXPECT_EQ(sink.size(), 8u);

    const std::vector<std::string> snapshot = sink.snapshot();
    ASSERT_EQ(snapshot.size(), 8u);
    // 0..11 must have been evicted; 12..19 must survive, oldest first.
    EXPECT_FALSE(any_contains(snapshot, "entry0 "));
    EXPECT_FALSE(any_contains(snapshot, "entry11"));
    EXPECT_TRUE(any_contains(snapshot, "entry12"));
    EXPECT_TRUE(any_contains(snapshot, "entry19"));
    EXPECT_NE(snapshot.front().find("entry12"), std::string::npos) << snapshot.front();
    EXPECT_NE(snapshot.back().find("entry19"), std::string::npos) << snapshot.back();
}

TEST(RingSinkTest, ClearEmptiesTheBuffer) {
    fc::RingSink sink{4};
    const spdlog::details::log_msg msg{"test", spdlog::level::info, "hello"};
    sink.log(msg);
    ASSERT_EQ(sink.size(), 1u);
    sink.clear();
    EXPECT_EQ(sink.size(), 0u);
    EXPECT_TRUE(sink.snapshot().empty());
}

TEST(RingSinkTest, WritesItselfToAFile) {
    const fc::test::TempDir dir{"ring"};
    fc::RingSink sink{4};
    for (int i = 0; i < 3; ++i) {
        const std::string payload = "line" + std::to_string(i);
        const spdlog::details::log_msg msg{"test", spdlog::level::info, payload};
        sink.log(msg);
    }

    const std::filesystem::path out = dir.path() / "ring.log";
    ASSERT_TRUE(sink.write_to_file_best_effort(out.wstring().c_str()));
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::ifstream stream{out};
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    EXPECT_NE(text.find("line0"), std::string::npos);
    EXPECT_NE(text.find("line2"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

TEST_F(LoggerFixture, InitCreatesTheDirectoryAndFile) {
    start();
    FC_LOG_INFO(Subsystem::App, "hello");
    fc::log::flush();

    const std::filesystem::path file = fc::log::log_file_path();
    EXPECT_FALSE(file.empty());
    EXPECT_TRUE(std::filesystem::exists(file)) << file.string();
}

TEST_F(LoggerFixture, SecondInitWithoutShutdownIsRejected) {
    start();
    fc::log::Config config;
    config.directory = dir_.path();
    const auto second = fc::log::init(config);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), fc::FcError::INTERNAL_INVALID_STATE);
}

TEST_F(LoggerFixture, SessionIdIsStampedOnEveryLine) {
    start();
    EXPECT_EQ(fc::log::session_id(), "testsession0001");

    FC_LOG_INFO(Subsystem::Capture, "first");
    FC_LOG_WARN(Subsystem::Gpu, "second");
    fc::log::flush();

    const std::vector<std::string> lines = fc::log::ring_snapshot();
    ASSERT_GE(lines.size(), 2u);
    for (const std::string& line : lines) {
        EXPECT_NE(line.find("testsession0001"), std::string::npos) << line;
    }
}

// SPEC.md §18 requires all of these on every line.
TEST_F(LoggerFixture, EveryLineCarriesTheMandatoryStructuredFields) {
    start();
    fc::set_thread_name("unittest");
    FC_LOG_ERROR(Subsystem::Mux, "trailer missing", LogFields{}.add_error(fc::FcError::MUX_WRITE_TRAILER_FAILED));
    fc::log::flush();

    const std::vector<std::string> lines = fc::log::ring_snapshot();
    ASSERT_FALSE(lines.empty());
    const std::string& line = lines.back();

    EXPECT_NE(line.find('T'), std::string::npos) << line;        // ISO-8601 date/time separator
    EXPECT_NE(line.find("Z |"), std::string::npos) << line;      // UTC marker
    EXPECT_NE(line.find("error"), std::string::npos) << line;    // level
    EXPECT_NE(line.find("unittest"), std::string::npos) << line; // thread name
    EXPECT_NE(line.find("mux"), std::string::npos) << line;      // subsystem
    EXPECT_NE(line.find("testsession0001"), std::string::npos) << line;
    EXPECT_NE(line.find("trailer missing"), std::string::npos) << line;
    EXPECT_NE(line.find("code=5006"), std::string::npos) << line; // key-value map
}

// Regression guard for the async-formatter trap: the pattern must resolve the
// *originating* thread's name, not the log worker's.
TEST_F(LoggerFixture, ThreadNameIsTheProducerNotTheLogWorker) {
    start();

    std::thread worker{[] {
        fc::set_thread_name("capture");
        FC_LOG_INFO(Subsystem::Capture, "frame acquired");
    }};
    worker.join();
    fc::log::flush();

    const std::vector<std::string> lines = fc::log::ring_snapshot();
    EXPECT_TRUE(any_contains(lines, "capture")) << "producer thread name missing";
    EXPECT_FALSE(any_contains(lines, "logworker")) << "line was attributed to the async worker";
}

TEST_F(LoggerFixture, LevelFilteringSuppressesQuieterLevels) {
    start(2000, Level::Warn);

    EXPECT_FALSE(fc::log::should_log(Level::Trace));
    EXPECT_FALSE(fc::log::should_log(Level::Info));
    EXPECT_TRUE(fc::log::should_log(Level::Warn));
    EXPECT_TRUE(fc::log::should_log(Level::Critical));

    FC_LOG_INFO(Subsystem::App, "suppressed-line");
    FC_LOG_WARN(Subsystem::App, "kept-line");
    fc::log::flush();

    const std::vector<std::string> lines = fc::log::ring_snapshot();
    EXPECT_FALSE(any_contains(lines, "suppressed-line"));
    EXPECT_TRUE(any_contains(lines, "kept-line"));
}

TEST_F(LoggerFixture, SetLevelTakesEffectImmediately) {
    start(2000, Level::Error);
    EXPECT_FALSE(fc::log::should_log(Level::Debug));
    fc::log::set_level(Level::Debug);
    EXPECT_TRUE(fc::log::should_log(Level::Debug));
    EXPECT_EQ(fc::log::level(), Level::Debug);
}

TEST_F(LoggerFixture, RingHonoursItsConfiguredCapacity) {
    start(16);
    for (int i = 0; i < 100; ++i) {
        FC_LOG_INFO(Subsystem::App, "entry", LogFields{}.add("i", i));
    }
    fc::log::flush();

    EXPECT_EQ(fc::log::ring_size(), 16u);
    const std::vector<std::string> lines = fc::log::ring_snapshot();
    EXPECT_TRUE(any_contains(lines, "i=99"));
    EXPECT_FALSE(any_contains(lines, "i=0 "));
}

TEST(LoggerTest, LoggingBeforeInitIsANoOpRatherThanACrash) {
    // Code paths that run before init() must not have to check.
    EXPECT_FALSE(fc::log::is_initialized());
    EXPECT_FALSE(fc::log::should_log(Level::Critical));
    FC_LOG_CRITICAL(Subsystem::Internal, "before init");
    EXPECT_TRUE(fc::log::ring_snapshot().empty());
    EXPECT_EQ(fc::log::ring_sink(), nullptr);
}

TEST_F(LoggerFixture, ShutdownIsIdempotent) {
    start();
    EXPECT_TRUE(fc::log::is_initialized());
    fc::log::shutdown();
    EXPECT_FALSE(fc::log::is_initialized());
    fc::log::shutdown(); // must not crash
    EXPECT_FALSE(fc::log::is_initialized());
}

// ---------------------------------------------------------------------------
// Session preamble (SPEC.md §18)
// ---------------------------------------------------------------------------

TEST(SessionPreambleTest, CollectsWhatIsAvailableWithoutGuessing) {
    const fc::SessionPreamble preamble = fc::collect_session_preamble();

    EXPECT_FALSE(preamble.app_version.empty());
    EXPECT_NE(preamble.os_build, "unknown") << preamble.os_build;
    EXPECT_NE(preamble.cpu_brand, "unknown") << preamble.cpu_brand;
    EXPECT_GT(preamble.cpu_logical_processors, 0u);

    // Hardware enumeration is M1's. It must be empty, not fabricated.
    EXPECT_TRUE(preamble.adapters.empty());
    EXPECT_TRUE(preamble.capture_backend.empty());
}

TEST_F(LoggerFixture, PreambleMarksUnimplementedFieldsAsPending) {
    start();
    fc::log_session_preamble();
    fc::log::flush();

    const std::vector<std::string> lines = fc::log::ring_snapshot();
    EXPECT_TRUE(any_contains(lines, "session preamble begin"));
    EXPECT_TRUE(any_contains(lines, "session preamble end"));
    EXPECT_TRUE(any_contains(lines, "os_build="));
    EXPECT_TRUE(any_contains(lines, "cpu="));

    // The distinction that matters when reading a user's log: "not implemented"
    // must not look like "no adapters found".
    EXPECT_TRUE(any_contains(lines, "<pending>"));
    EXPECT_TRUE(any_contains(lines, "M1 gpu topology service"));
}

} // namespace

// ---------------------------------------------------------------------------
// Level names crossing a process boundary (SPEC.md §15.1's `set_log_level`, and
// `FC_LOG_LEVEL`)
// ---------------------------------------------------------------------------

// One parser serves both, so a level the environment accepts and the IPC command
// rejects is not a state the two can reach. This is the case that fails if either
// grows a table of its own.
TEST(LogLevelNames, EverySpelledLevelParsesBackCaseInsensitively) {
    for (const fc::log::Level level :
         {fc::log::Level::Trace, fc::log::Level::Debug, fc::log::Level::Info, fc::log::Level::Warn,
          fc::log::Level::Error, fc::log::Level::Critical, fc::log::Level::Off}) {
        const std::string upper{fc::log::to_string(level)};
        std::string lower = upper;
        for (char& c : lower) {
            c = static_cast<char>(c | 0x20);
        }

        const std::optional<fc::log::Level> from_upper = fc::log::level_from_string(upper);
        const std::optional<fc::log::Level> from_lower = fc::log::level_from_string(lower);

        ASSERT_TRUE(from_upper.has_value()) << "'" << upper << "' does not parse";
        ASSERT_TRUE(from_lower.has_value()) << "'" << lower << "' does not parse; humans write lower case";
        EXPECT_EQ(*from_upper, level);
        EXPECT_EQ(*from_lower, level);
    }
}

TEST(LogLevelNames, AnUnrecognisedNameIsRefusedRatherThanDefaulted) {
    // Refused, not silently mapped to Info: a typo in `FC_LOG_LEVEL` that quietly
    // produced the default would look exactly like the override not working.
    for (const char* name : {"", "verbose", "tracee", "trac", "INFORMATION", "0"}) {
        EXPECT_FALSE(fc::log::level_from_string(name).has_value()) << "accepted '" << name << "'";
    }
}
