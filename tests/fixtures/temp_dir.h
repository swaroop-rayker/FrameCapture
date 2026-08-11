#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace fc::test {

/// A unique directory under the system temp path, removed on destruction.
///
/// Tests must never write into `%LOCALAPPDATA%\FrameCapture` -- that is the real
/// user's log and crash directory, and a test run should not leave artifacts
/// there or race a running engine.
class TempDir {
public:
    explicit TempDir(std::string_view label) {
        static std::atomic<unsigned> counter{0};
        const unsigned unique = counter.fetch_add(1, std::memory_order_relaxed);
        path_ =
            std::filesystem::temp_directory_path() / ("fc_test_" + std::string{label} + "_" + std::to_string(unique));

        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    // remove_all's error_code overload does not throw for filesystem errors; only
    // an allocation failure deep inside it could escape, and a test process that is
    // out of memory has already failed.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace fc::test
