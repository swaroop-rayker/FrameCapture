#include "core/util/atomic_write.h"

#include "temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return {};
    }
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void put_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << contents;
}

TEST(AtomicWriteTest, WritesANewFile) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "out.txt";

    ASSERT_TRUE(fc::write_file_atomically(target, "hello").has_value());
    EXPECT_EQ(read_file(target), "hello");
}

TEST(AtomicWriteTest, ReplacesAnExistingFile) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "out.txt";
    put_file(target, "old content that is longer");

    ASSERT_TRUE(fc::write_file_atomically(target, "new").has_value());
    EXPECT_EQ(read_file(target), "new") << "replacement must not leave a tail of the old content";
}

TEST(AtomicWriteTest, CreatesMissingParentDirectories) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "nested" / "deeper" / "out.txt";

    ASSERT_TRUE(fc::write_file_atomically(target, "x").has_value());
    EXPECT_TRUE(std::filesystem::exists(target));
}

TEST(AtomicWriteTest, LeavesNoTempFileBehindOnSuccess) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "out.txt";

    ASSERT_TRUE(fc::write_file_atomically(target, "x").has_value());
    EXPECT_FALSE(std::filesystem::exists(fc::atomic_temp_path(target)));
}

TEST(AtomicWriteTest, RejectsAnEmptyPath) {
    const auto result = fc::write_file_atomically({}, "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::IO_PATH_INVALID);
}

TEST(AtomicWriteTest, HandlesAnEmptyPayload) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "empty.txt";
    put_file(target, "previous");

    ASSERT_TRUE(fc::write_file_atomically(target, "").has_value());
    EXPECT_TRUE(std::filesystem::exists(target));
    EXPECT_EQ(read_file(target), "");
}

TEST(AtomicWriteTest, WritesPayloadsLargerThanOneChunk) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "big.bin";
    // The write loop chunks at 1 MiB; cross that boundary.
    const std::string payload((3u * 1024u * 1024u) + 17u, 'z');

    ASSERT_TRUE(fc::write_file_atomically(target, payload).has_value());
    EXPECT_EQ(std::filesystem::file_size(target), payload.size());
    EXPECT_EQ(read_file(target), payload);
}

// ---------------------------------------------------------------------------
// Simulated interruption. The property under test is the one SPEC.md §17 exists to
// guarantee: the destination is never a mixture of old and new, and never absent.
// ---------------------------------------------------------------------------

class AtomicWriteFaultTest : public ::testing::TestWithParam<fc::AtomicWriteFault> {};

TEST_P(AtomicWriteFaultTest, ExistingFileSurvivesInterruptionUntouched) {
    const fc::test::TempDir dir{"atomicfault"};
    const std::filesystem::path target = dir.path() / "config.toml";
    const std::string original = "original = true\n";
    put_file(target, original);

    const auto result = fc::write_file_atomically(target, "replacement = true\n", GetParam());

    ASSERT_FALSE(result.has_value()) << "an interrupted write must report failure";
    EXPECT_TRUE(std::filesystem::exists(target));
    EXPECT_EQ(read_file(target), original) << "destination was modified before the rename";
}

TEST_P(AtomicWriteFaultTest, DestinationIsNotCreatedByAnInterruptedFirstWrite) {
    const fc::test::TempDir dir{"atomicfault"};
    const std::filesystem::path target = dir.path() / "config.toml";

    const auto result = fc::write_file_atomically(target, "content\n", GetParam());

    ASSERT_FALSE(result.has_value());
    // A half-written file at the real path would be worse than no file: the loader
    // would try to parse it.
    EXPECT_FALSE(std::filesystem::exists(target));
}

INSTANTIATE_TEST_SUITE_P(EveryFaultPoint, AtomicWriteFaultTest,
                         ::testing::Values(fc::AtomicWriteFault::BeforeWrite, fc::AtomicWriteFault::AfterWrite,
                                           fc::AtomicWriteFault::AfterFlush));

TEST(AtomicWriteTest, InterruptionLeavesTheTempFileForRecovery) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "config.toml";
    put_file(target, "old\n");

    ASSERT_FALSE(fc::write_file_atomically(target, "new content\n", fc::AtomicWriteFault::AfterFlush).has_value());

    // Deliberate: the temp holds the content the caller wanted written, so a crash
    // between flush and rename is recoverable rather than a silent loss.
    const std::filesystem::path temp = fc::atomic_temp_path(target);
    ASSERT_TRUE(std::filesystem::exists(temp));
    EXPECT_EQ(read_file(temp), "new content\n");
    EXPECT_EQ(read_file(target), "old\n");
}

TEST(AtomicWriteTest, ARetryAfterInterruptionSucceeds) {
    const fc::test::TempDir dir{"atomic"};
    const std::filesystem::path target = dir.path() / "config.toml";
    put_file(target, "old\n");

    ASSERT_FALSE(fc::write_file_atomically(target, "new\n", fc::AtomicWriteFault::AfterWrite).has_value());
    // A leftover temp must not block the next attempt.
    ASSERT_TRUE(fc::write_file_atomically(target, "new\n").has_value());
    EXPECT_EQ(read_file(target), "new\n");
    EXPECT_FALSE(std::filesystem::exists(fc::atomic_temp_path(target)));
}

} // namespace
