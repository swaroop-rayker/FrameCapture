// SPEC.md §20 row 3 -- corrupted file after crash.
//
// GPU TIER.
//
// > `test_crash_recovery`: `TerminateProcess` at t=30 s, assert the file decodes
// > and duration ≥ 28 s
//
// The kill has to be real. A test that closed the muxer politely, or that threw an
// exception and let a destructor run, would exercise the finalization path — which
// is the path that by definition does not run when a process dies. So the
// recording happens in `fc_crash_recorder`, a separate process, and this test
// `TerminateProcess`es it: no trailer, no flush, no destructor, nothing.
//
// What must then be true is SPEC.md §10.3's claim, stated in its strongest form:
// **the file left behind plays.** Not "can be repaired into something that plays"
// — plays, as it sits, before anything touches it. The repair afterwards upgrades
// it to a progressive MP4 that seeks instantly; it does not rescue it.

#include "core/logging/logger.h"
#include "core/mux/muxer.h"
#include "core/mux/recovery.h"

#include "decoded_media.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/// SPEC.md row 3's duration. `FC_CRASH_SECONDS` shortens the run when iterating.
constexpr int kDefaultSeconds = 30;

/// How much of the tail a kill may cost -- and why it is a *duration*, not a
/// fraction.
///
/// `frag_keyframe` closes a fragment when the **next** keyframe arrives, so
/// whatever has accumulated since the last one is still open in libavformat when
/// the process dies. SPEC.md §9 fixes the GOP at two seconds, so the loss is
/// bounded by one GOP no matter how long the recording ran. Row 3's "28 s of 30 s"
/// is exactly that one GOP, written as a pair of numbers rather than as a rule.
///
/// This test first used the ratio, 28/30, and it was wrong in both directions: on
/// a ten-second run it demanded losing under 0.67 s, which one GOP makes
/// impossible, and on an hour-long run it would have accepted losing four minutes.
/// Measured here at ten seconds: 1.995 s lost, one GOP almost exactly.
constexpr double kMaxTailLossSeconds = 2.0;

/// The same bound for MP4, which no longer waits for a keyframe to close a fragment
/// (BUG-034).
///
/// `frag_duration` cuts a fragment every 500 ms, so what a kill costs is that plus
/// however far the muxer trails the capture — measured at 39–44 ms. One second is
/// twice the fragment and still less than half the old bound, so a regression to
/// keyframe-only fragmentation fails here instead of passing with 121 ms to spare.
///
/// MKV keeps 2.0: SPEC.md §10.2 specifies `cluster_time_limit=2000` in as many words,
/// so its recoverability unit is still two seconds and tightening it is the owner's
/// call, not a test's.
constexpr double kMaxMp4TailLossSeconds = 1.0;

/// The last frame's presentation time is one frame short of the duration, and the
/// keyframe cadence is not perfectly aligned to the kill. A tenth of a second
/// absorbs both without absorbing anything that would matter.
constexpr double kQuantisationSlack = 0.1;

[[nodiscard]] double minimum_decoded(int seconds, double max_loss = kMaxTailLossSeconds) {
    return seconds - max_loss - kQuantisationSlack;
}

[[nodiscard]] double minimum_decoded_mp4(int seconds) {
    return minimum_decoded(seconds, kMaxMp4TailLossSeconds);
}

[[nodiscard]] int crash_seconds() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test setup.
    const char* value = std::getenv("FC_CRASH_SECONDS");
    if (value == nullptr) {
        return kDefaultSeconds;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed < 8) {
        return kDefaultSeconds;
    }
    return static_cast<int>(std::min<long>(parsed, 60 * 60));
}

/// A child process, killed rather than asked to stop.
class Recorder {
public:
    Recorder(const std::filesystem::path& output, const std::filesystem::path& ready, const char* container, bool audio,
             int stop_after = 0) {
        std::wstring command = L"\"";
        command += std::filesystem::path{FC_CRASH_RECORDER_EXE}.wstring();
        command += L"\" --output \"";
        command += output.wstring();
        command += L"\" --ready \"";
        command += ready.wstring();
        command += L"\" --container ";
        command += std::filesystem::path{container}.wstring();
        if (!audio) {
            command += L" --no-audio";
        }
        if (stop_after > 0) {
            command += L" --stop-after " + std::to_wstring(stop_after);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        started_ = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &startup, &info_) != 0;
    }

    ~Recorder() {
        // Belt and braces: a test that fails an assertion before the kill must not
        // leave a recorder running against the machine.
        kill();
        if (info_.hProcess != nullptr) {
            CloseHandle(info_.hProcess);
        }
        if (info_.hThread != nullptr) {
            CloseHandle(info_.hThread);
        }
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) = delete;
    Recorder& operator=(Recorder&&) = delete;

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

    /// Waits for the child to report that frames are reaching the encoder.
    [[nodiscard]] bool await_ready(const std::filesystem::path& marker, std::chrono::seconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(marker)) {
                return true;
            }
            if (exited()) {
                return false; // it failed to start; no point waiting out the timeout
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        return false;
    }

    [[nodiscard]] bool exited() const {
        DWORD code = 0;
        return GetExitCodeProcess(info_.hProcess, &code) != 0 && code != STILL_ACTIVE;
    }

    [[nodiscard]] DWORD exit_code() const {
        DWORD code = 0;
        GetExitCodeProcess(info_.hProcess, &code);
        return code;
    }

    /// Waits for the child to finish on its own, for the clean-stop case.
    [[nodiscard]] bool await_exit(std::chrono::seconds timeout) const {
        return info_.hProcess != nullptr &&
               WaitForSingleObject(info_.hProcess, static_cast<DWORD>(timeout.count() * 1000)) == WAIT_OBJECT_0;
    }

    /// The event under test. No unwinding, no destructors, no trailer.
    void kill() const {
        if (info_.hProcess != nullptr && !exited()) {
            TerminateProcess(info_.hProcess, 1);
            WaitForSingleObject(info_.hProcess, 5000);
        }
    }

private:
    PROCESS_INFORMATION info_{};
    bool started_ = false;
};

class CrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::make_unique<fc::test::TempDir>("crashrec");
        fc::log::Config config;
        config.directory = dir_->path() / "parentlogs";
        config.session_id = "crashrectest001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    void TearDown() override {
        fc::log::shutdown();
        dir_.reset();
    }

    /// How far the container trailed the capture at the instant of the kill, in
    /// seconds, read from the progress file the recorder rewrites as it runs.
    ///
    /// This is the number BUG-034 needed and did not have. A killed recording decodes
    /// to its last *complete* fragment, so the loss is the open fragment **plus** this
    /// lag — and without it, two occurrences produced a duration and no way to tell
    /// whether the muxer was behind or the bytes were merely unflushed. It turned out
    /// to be neither: the lag was 39–44 ms and a closed fragment was sitting in the
    /// AVIO buffer.
    ///
    /// Returns a negative value when the recorder wrote no progress file.
    [[nodiscard]] double mux_lag_at_kill(const char* name) const {
        return progress_field(name, "lag_s=");
    }

    /// How long the recorder had actually been recording when it was killed.
    ///
    /// The parent sleeps `seconds` from the *ready marker*, but the recording's
    /// timeline starts a little earlier, so the content is legitimately longer than
    /// `seconds`. That never mattered while a kill cost two seconds — the decoded
    /// length was nowhere near the upper bound. Now that a kill costs under half a
    /// second (BUG-034) the upper bound is the tight one, measured at 30.005 s
    /// against a 30.1 s limit, and guessing at the skew would make this fail on a
    /// machine whose audio starts more slowly.
    [[nodiscard]] double recorded_elapsed_at_kill(const char* name) const {
        return progress_field(name, "elapsed_s=");
    }

    [[nodiscard]] double progress_field(const char* name, std::string_view key) const {
        const std::filesystem::path path = dir_->path() / (std::string{name} + ".progress");
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return -1.0;
        }
        for (std::string line; std::getline(in, line);) {
            if (line.starts_with(key)) {
                return std::strtod(line.c_str() + key.size(), nullptr);
            }
        }
        return -1.0;
    }

    /// Records for `seconds`, then kills the process. Returns the output path.
    std::filesystem::path record_and_kill(const char* container, const char* name, int seconds, bool audio = true) {
        std::filesystem::path output = dir_->path() / name;
        const std::filesystem::path ready = dir_->path() / (std::string{name} + ".ready");

        const Recorder recorder(output, ready, container, audio);
        if (!recorder.started()) {
            ADD_FAILURE() << "could not spawn " << FC_CRASH_RECORDER_EXE;
            return {};
        }
        if (!recorder.await_ready(ready, std::chrono::seconds{30})) {
            ADD_FAILURE() << "the recorder never reported frames reaching the encoder";
            return {};
        }

        // From the moment frames are flowing, not from spawn: start-up cost must
        // not be charged against the recording's length.
        std::this_thread::sleep_for(std::chrono::seconds{seconds});
        if (recorder.exited()) {
            ADD_FAILURE() << "the recorder exited on its own; nothing was crash-tested";
            return {};
        }
        recorder.kill();
        return output;
    }

    std::unique_ptr<fc::test::TempDir> dir_;
};

// ---------------------------------------------------------------------------
// SPEC.md §20 row 3
// ---------------------------------------------------------------------------

TEST_F(CrashRecoveryTest, AKilledMp4RecordingIsPlayableWhereItStopped) {
    const int seconds = crash_seconds();
    const std::filesystem::path output = record_and_kill("mp4", "killed.mp4", seconds);
    ASSERT_FALSE(output.empty());
    ASSERT_TRUE(std::filesystem::exists(output)) << "the killed process left no file at all";

    // Before any repair. This is the assertion the fragmented recipe exists to
    // make true: a progressive MP4 killed here would have no `moov` atom and would
    // not open.
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << "the killed recording does not even open: " << media.detail;
    ASSERT_TRUE(media.video.present);
    ASSERT_FALSE(media.video.times.empty()) << "no video frame decoded out of the killed recording";

    const double decoded_seconds = media.video.times.back();
    EXPECT_GE(decoded_seconds, minimum_decoded_mp4(seconds))
        << "the killed recording decodes to " << decoded_seconds << " s of a " << seconds
        << " s recording; a fragment is closed every " << kMaxMp4TailLossSeconds
        << " s at most, so that is the whole of what a kill may cost (BUG-034)";

    // The other direction matters too. A file reporting *more* than the recording
    // lasted would mean the timestamps are wrong, not that nothing was lost.
    //
    // Against what the recorder says it recorded, not against the nominal `seconds`:
    // the timeline starts before the ready marker the parent counts from, so a decoded
    // length a little over `seconds` is correct rather than suspicious. Compared to a
    // constant this would now sit 95 ms from failing on a healthy run.
    const double recorded = recorded_elapsed_at_kill("killed.mp4");
    ASSERT_GT(recorded, 0.0) << "the recorder wrote no progress file";
    EXPECT_LE(decoded_seconds, recorded + kQuantisationSlack)
        << "the killed recording claims " << decoded_seconds << " s where the recorder ran for " << recorded << " s";

    // **The mechanism, not just the outcome** (BUG-034). The file's shortfall has two
    // terms: how far the muxer trailed the capture, and the fragment still open when
    // the process died. Asserting only the total let a defect in the second term hide
    // for two occurrences behind a plausible story about the first.
    //
    // Splitting them means a future failure says which one moved. If the lag is small
    // and the loss is large, bytes that were written are not reaching the disk — which
    // is exactly what was happening, and what `AVFMT_FLAG_FLUSH_PACKETS` fixed.
    const double lag = mux_lag_at_kill("killed.mp4");
    const double loss = seconds - decoded_seconds;
    std::printf("[ RUN INFO ] decoded %.3f s of %d s: mux lag at kill %.3f s, open fragment %.3f s\n", decoded_seconds,
                seconds, lag, loss - std::max(0.0, lag));

    ASSERT_GE(lag, 0.0) << "the recorder wrote no progress file; the split below is not measurable";
    EXPECT_LT(lag, kMaxMp4TailLossSeconds)
        << "the container was " << lag
        << " s behind the capture at the kill. That is a pipeline problem, not a flushing one, and it is not what "
           "BUG-034 turned out to be";
    EXPECT_LT(loss - lag, kMaxMp4TailLossSeconds)
        << "the file lost " << (loss - lag) << " s beyond the muxer's own lag, against a fragment closed every "
        << (static_cast<double>(kMaxMp4TailLossSeconds) / 2.0)
        << " s. Bytes handed to libavformat are not reaching the disk (BUG-034)";

    testing::Test::RecordProperty("seconds", seconds);
    testing::Test::RecordProperty("decoded_ms", static_cast<int>(decoded_seconds * 1000));
    testing::Test::RecordProperty("lost_ms", static_cast<int>(loss * 1000));
    testing::Test::RecordProperty("mux_lag_ms", static_cast<int>(lag * 1000));
    testing::Test::RecordProperty("bytes", static_cast<int>(std::filesystem::file_size(output)));
}

// The other half of §10.4: a killed recording leaves a sidecar, the next launch
// finds it, and the repair turns the fragmented file into the progressive one a
// clean stop would have produced.
TEST_F(CrashRecoveryTest, TheNextLaunchFindsTheSidecarAndFinishesTheJob) {
    const int seconds = crash_seconds();
    const std::filesystem::path output = record_and_kill("mp4", "repairable.mp4", seconds);
    ASSERT_FALSE(output.empty());

    // Discovered by scanning, exactly as the GUI will on launch — not by knowing
    // the path, which would skip the mechanism that makes recovery possible at all.
    const std::vector<std::filesystem::path> sidecars = fc::mux::find_recoverable(dir_->path());
    ASSERT_EQ(sidecars.size(), 1u) << "the killed recording left " << sidecars.size() << " sidecars";

    const auto record = fc::mux::read_recovery_record(sidecars.front());
    ASSERT_TRUE(record.has_value()) << fc::error_name(record.error());
    EXPECT_EQ(record.value().container, fc::config::Container::Mp4);
    EXPECT_EQ(record.value().output, std::filesystem::absolute(output));
    EXPECT_FALSE(record.value().started_utc.empty());
    EXPECT_FALSE(record.value().engine_version.empty());

    const std::uintmax_t fragmented_size = std::filesystem::file_size(output);

    const auto outcome = fc::mux::recover(sidecars.front());
    ASSERT_TRUE(outcome.has_value()) << fc::error_name(outcome.error());
    EXPECT_TRUE(outcome.value().valid) << outcome.value().detail;
    EXPECT_TRUE(outcome.value().repaired);

    // The claim is retracted only once the recording is genuinely finished, so a
    // second launch does not prompt about it again.
    EXPECT_TRUE(fc::mux::find_recoverable(dir_->path()).empty());

    // And the repaired file is still the recording, not a shorter one. A remux
    // that dropped the tail would satisfy every structural check and lose content.
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    ASSERT_FALSE(media.video.times.empty());
    EXPECT_GE(media.video.times.back(), minimum_decoded_mp4(seconds));
    EXPECT_TRUE(media.audio.present) << "the audio track did not survive the repair";

    testing::Test::RecordProperty("fragmented_bytes", static_cast<int>(fragmented_size));
    testing::Test::RecordProperty("progressive_bytes", static_cast<int>(std::filesystem::file_size(output)));
    testing::Test::RecordProperty("repaired_ms", static_cast<int>(media.video.times.back() * 1000));
}

// Matroska's own crash story (SPEC.md §10.2), asserted rather than assumed. It
// needs no repair, which is a claim worth checking against a real kill.
TEST_F(CrashRecoveryTest, AKilledMkvRecordingIsPlayableWithoutRepair) {
    const int seconds = crash_seconds();
    const std::filesystem::path output = record_and_kill("mkv", "killed.mkv", seconds);
    ASSERT_FALSE(output.empty());

    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << "the killed Matroska does not open: " << media.detail;
    ASSERT_FALSE(media.video.times.empty());
    EXPECT_GE(media.video.times.back(), minimum_decoded(seconds))
        << "decoded " << media.video.times.back() << " s of a " << seconds << " s recording";

    // Recovery leaves it alone and says so.
    const std::vector<std::filesystem::path> sidecars = fc::mux::find_recoverable(dir_->path());
    ASSERT_EQ(sidecars.size(), 1u);
    const auto outcome = fc::mux::recover(sidecars.front());
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome.value().valid) << outcome.value().detail;
    EXPECT_FALSE(outcome.value().repaired) << "Matroska was rewritten when it did not need to be";
}

// ---------------------------------------------------------------------------
// The other half of §10.3 -- what a clean stop produces
// ---------------------------------------------------------------------------

/// Top-level MP4 box types, in file order.
///
/// Read directly rather than inferred from libavformat, because the property under
/// test *is* the byte layout: `faststart` moves `moov` ahead of `mdat`, and a
/// decoder will happily open the file either way. Asking libavformat whether the
/// file is progressive would be asking the wrong witness.
[[nodiscard]] std::vector<std::string> top_level_boxes(const std::filesystem::path& path) {
    std::vector<std::string> boxes;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return boxes;
    }

    std::uintmax_t offset = 0;
    const std::uintmax_t size = std::filesystem::file_size(path);
    while (offset + 8 <= size && boxes.size() < 64) {
        file.seekg(static_cast<std::streamoff>(offset));
        std::array<char, 8> header{};
        if (!file.read(header.data(), header.size())) {
            break;
        }
        // Big-endian 32-bit length, then a four-character type.
        const auto length = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
                            static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
        boxes.emplace_back(header.data() + 4, 4);
        if (length < 8) {
            break; // 0 means "to end of file", 1 means a 64-bit size; either ends the walk
        }
        offset += length;
    }
    return boxes;
}

[[nodiscard]] std::ptrdiff_t index_of(const std::vector<std::string>& boxes, std::string_view type) {
    const auto found = std::ranges::find(boxes, type);
    return found == boxes.end() ? -1 : std::distance(boxes.begin(), found);
}

// SPEC.md §10.3's clean-stop half. The recording is fragmented while it runs and
// progressive once it stops, and "progressive" is a statement about where the
// `moov` atom sits -- so that is what is checked.
TEST_F(CrashRecoveryTest, ACleanlyStoppedMp4IsProgressiveAndLeavesNoSidecar) {
    const std::filesystem::path output = dir_->path() / "clean.mp4";
    const std::filesystem::path ready = dir_->path() / "clean.ready";

    // Allowed to stop itself this time, which is the whole point: `--max-seconds`
    // expires, the process returns from `record`, and nothing kills it. The
    // recorder leaks its pipeline deliberately so a *killed* run cannot finalize,
    // so this path finalizes explicitly instead -- see `--stop-after`.
    const Recorder recorder(output, ready, "mp4", true, /*stop_after=*/4);
    ASSERT_TRUE(recorder.started());
    ASSERT_TRUE(recorder.await_ready(ready, std::chrono::seconds{30}));

    // While it is still recording the claim is on disk. That invariant is what the
    // clean path has to retract, so it is worth asserting before it does.
    EXPECT_TRUE(std::filesystem::exists(fc::mux::sidecar_path_for(output)))
        << "an in-progress recording left no sidecar; a crash would go unnoticed";

    ASSERT_TRUE(recorder.await_exit(std::chrono::seconds{60})) << "the recorder never stopped on its own";
    EXPECT_EQ(recorder.exit_code(), 0) << "the recorder did not stop cleanly";

    // Retracted, because the recording finished and validated.
    EXPECT_FALSE(std::filesystem::exists(fc::mux::sidecar_path_for(output)))
        << "a finished recording still claims to be unfinished";
    EXPECT_TRUE(fc::mux::find_recoverable(dir_->path()).empty());

    // No leftover from the swap.
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir_->path())) {
        EXPECT_EQ(entry.path().extension().string() == ".tmp", false)
            << "the remux left " << entry.path().filename().string() << " behind";
    }

    const std::vector<std::string> boxes = top_level_boxes(output);
    ASSERT_FALSE(boxes.empty()) << "the output has no readable box structure";

    const std::ptrdiff_t moov = index_of(boxes, "moov");
    const std::ptrdiff_t mdat = index_of(boxes, "mdat");
    ASSERT_GE(moov, 0) << "no moov atom at all";
    ASSERT_GE(mdat, 0) << "no mdat atom at all";
    EXPECT_LT(moov, mdat) << "the moov atom is behind the media data; faststart did not run, so this file "
                             "seeks by reading to the end first";
    EXPECT_EQ(index_of(boxes, "moof"), -1) << "the finished file still carries fragments";

    // And it is still the recording.
    fc::test::DecodedMedia media;
    fc::test::decode_media(output, fc::test::DecodeOptions{true, true, false}, media);
    ASSERT_TRUE(media.opened) << media.detail;
    EXPECT_EQ(media.stream_count, 2);
    ASSERT_FALSE(media.video.times.empty());
    EXPECT_GT(media.video.times.back(), 2.0);
    EXPECT_TRUE(media.audio.present);
}

} // namespace
