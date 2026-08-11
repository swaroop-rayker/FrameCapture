// SPEC.md §20 row 13 — orphaned engine process.
//
//   > `test_orphan_prevention`: kill the GUI, assert the engine exits within 6 s having
//   > finalized the file.
//
// GPU TIER, and it has to be: the engine under test opens a real capture session and a
// real encoder, and the whole claim is about what happens to *that* when its host dies.
// A mock engine would prove the test harness works.
//
// ---------------------------------------------------------------------------
// What is actually being killed, and why it is a separate process
// ---------------------------------------------------------------------------
// `fc_gui_host` plays SPEC.md §3.1's GUI role -- job object, spawn, handshake,
// heartbeat -- and nothing else. It is killed with `TerminateProcess`, which is the
// harshest available death: no unwinding, no atexit, no chance to send `shutdown`. That
// matters, because a host that got to say goodbye would exercise the easy path and
// prove nothing about the hard one.
//
// The engine must then, entirely on its own:
//   1. notice within SPEC.md §3.1's 5 s heartbeat timeout,
//   2. finalize the recording cleanly -- "It does **not** discard the file",
//   3. exit,
//
// all inside row 13's 6 s budget. The three are asserted separately, because "the
// engine exited" and "the engine exited having done its job" are different claims and
// only the second one is worth anything to a user.

#include "core/error/fc_error.h"
#include "core/ipc/lifecycle.h"
#include "core/mux/muxer.h"
#include "core/mux/recovery.h"
#include "temp_dir.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/// SPEC.md §20 row 13's bound. 5 s of heartbeat timeout plus one.
///
/// **In milliseconds deliberately.** Written as `6s`, every comparison against a
/// millisecond measurement silently compares 5716 against 6 -- which fails a passing
/// engine and would just as happily pass a failing one if the numbers fell the other
/// way. Measured once, exactly that way round.
constexpr std::chrono::milliseconds kExitBudget = 6000ms;

/// How long the recording runs before the host is killed. Long enough that there is
/// real content to lose -- a kill at 200 ms would leave a file whose validity says more
/// about the container's header than about the finalize path.
constexpr auto kRecordSeconds = 3s;

struct HostProcess {
    HANDLE process = nullptr;
    DWORD pid = 0;
    HANDLE stdout_read = nullptr;
};

/// Spawns `fc_gui_host` with its stdout piped, so the test can read the engine's pid
/// out of it rather than guessing which `framecapture-engine.exe` is the right one.
[[nodiscard]] bool spawn_host(const std::vector<std::string>& args, HostProcess& out) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (::CreatePipe(&read_end, &write_end, &inheritable, 0) == 0) {
        return false;
    }
    // The read end must not be inherited, or the child holds a copy and the pipe never
    // reports EOF when the child dies -- which would turn every read below into a hang.
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::string command = std::string{"\""} + FC_GUI_HOST_EXE + "\" \"" + FC_ENGINE_EXE + "\"";
    for (const std::string& arg : args) {
        command += " " + arg;
    }
    std::wstring wide(command.begin(), command.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION info{};
    const BOOL created =
        ::CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info);
    ::CloseHandle(write_end);
    if (created == 0) {
        ::CloseHandle(read_end);
        return false;
    }

    ::CloseHandle(info.hThread);
    out.process = info.hProcess;
    out.pid = info.dwProcessId;
    out.stdout_read = read_end;
    return true;
}

/// Reads the host's stdout until `marker` appears or the deadline passes.
///
/// Returns the whole transcript so far either way, so a failure message can show what
/// the host actually said rather than only that it did not say the expected thing.
[[nodiscard]] std::string read_until(HANDLE pipe, std::string_view marker, std::chrono::milliseconds deadline_after,
                                     std::string& transcript) {
    const auto deadline = std::chrono::steady_clock::now() + deadline_after;
    std::array<char, 512> chunk{};

    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == 0) {
            break; // child gone
        }
        if (available == 0) {
            std::this_thread::sleep_for(20ms);
            continue;
        }
        const DWORD wanted = std::min(available, static_cast<DWORD>(chunk.size()));
        DWORD read = 0;
        if (::ReadFile(pipe, chunk.data(), wanted, &read, nullptr) == 0 || read == 0) {
            break;
        }
        transcript.append(chunk.data(), read);
        if (transcript.find(marker) != std::string::npos) {
            return transcript;
        }
    }
    return transcript;
}

[[nodiscard]] DWORD pid_from_transcript(const std::string& transcript) {
    const std::size_t at = transcript.find("ENGINE_PID ");
    if (at == std::string::npos) {
        return 0;
    }
    return static_cast<DWORD>(std::strtoul(transcript.c_str() + at + 11, nullptr, 10));
}

/// Waits for a pid to leave the process table, and reports how long it took.
///
/// Opened with `SYNCHRONIZE` only: this is a wait, not an inspection, and asking for
/// more rights than the operation needs is how a test starts failing on a hardened
/// machine for reasons unrelated to what it asserts.
[[nodiscard]] std::chrono::milliseconds wait_for_exit(DWORD pid, std::chrono::milliseconds budget) {
    const auto began = std::chrono::steady_clock::now();
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return 0ms; // already gone
    }
    const DWORD outcome = ::WaitForSingleObject(process, static_cast<DWORD>(budget.count()));
    ::CloseHandle(process);
    if (outcome != WAIT_OBJECT_0) {
        return budget + 1ms; // over budget, by a value the caller can see is over
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
}

class OrphanPreventionTest : public ::testing::Test {
protected:
    fc::test::TempDir temp_{"orphan"};
};

// ---------------------------------------------------------------------------
// Row 13, exactly as SPEC.md §20 words it
// ---------------------------------------------------------------------------

TEST_F(OrphanPreventionTest, KillingTheHostLeavesNoEngineAndAFinalizedFile) {
    const std::filesystem::path output = temp_.path() / "orphan.mkv";

    HostProcess host;
    ASSERT_TRUE(spawn_host({"start", "\"" + output.string() + "\"", "idle"}, host))
        << "could not spawn the host that plays SPEC.md §3.1's GUI role";

    std::string transcript;
    static_cast<void>(read_until(host.stdout_read, "RECORDING", 30s, transcript));
    ASSERT_NE(transcript.find("READY"), std::string::npos) << "the host never completed the handshake:\n" << transcript;
    ASSERT_NE(transcript.find("RECORDING"), std::string::npos) << "the engine never started recording:\n" << transcript;

    const DWORD engine_pid = pid_from_transcript(transcript);
    ASSERT_NE(engine_pid, 0U) << "the host did not announce the engine's pid:\n" << transcript;

    // Let a real recording accumulate. Without this the file's validity would be a
    // statement about `avformat_write_header` and not about the finalize path.
    std::this_thread::sleep_for(kRecordSeconds);

    // The kill. `TerminateProcess` is the harshest death available: the host runs no
    // cleanup, sends no `shutdown`, and closes its job handle only because the kernel
    // reclaims it. Everything after this is the engine acting alone.
    const auto killed_at = std::chrono::steady_clock::now();
    ASSERT_NE(::TerminateProcess(host.process, 1), 0) << "could not kill the host";

    const std::chrono::milliseconds exit_after = wait_for_exit(engine_pid, kExitBudget);
    const auto observed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - killed_at);

    ::CloseHandle(host.process);
    ::CloseHandle(host.stdout_read);

    // 1. It exited, and inside row 13's budget.
    EXPECT_LE(exit_after.count(), kExitBudget.count())
        << "the engine outlived its host by more than SPEC.md §20 row 13's 6 s; it is an orphan holding a capture "
           "session and an open file handle. Observed "
        << observed_ms.count() << " ms";

    // 2. It did not merely die -- it left a file behind. SPEC.md §3.1: "It does **not**
    //    discard the file."
    ASSERT_TRUE(std::filesystem::exists(output)) << "the engine exited without leaving an output file at all";
    EXPECT_GT(std::filesystem::file_size(output), 0U) << "the output file is empty";

    // 3. And the file is playable, which is the prime directive (CLAUDE.md §1) and the
    //    only part of this a user would ever notice.
    fc::mux::ValidationExpectation expectation;
    expectation.duration_seconds = 0.0; // whatever it got to is fine; validity is not
    const fc::Result<fc::mux::ValidationReport> report = fc::mux::Muxer::validate(output, expectation);
    ASSERT_TRUE(report.has_value()) << "the file left behind could not be validated at all";
    EXPECT_TRUE(report.value().valid) << "the file left behind is not playable: " << report.value().detail;
    EXPECT_GT(report.value().decoded_frames, 0) << "the file has no decodable frames";

    // The margin is thin by construction and that is worth stating rather than
    // discovering: 5 s of the 6 s budget is SPEC.md §3.1's heartbeat timeout, so
    // everything the engine does after noticing -- drain, flush, trailer, validate --
    // has to fit in the remaining second. Measured at ~700 ms on this rig. A recording
    // whose finalize is slower than that (a very large MP4's remux, a stalled disk)
    // would exceed row 13's bound without anything being wrong with the *mechanism*,
    // which is a point for the owner rather than a reason to loosen the assertion here.
    std::printf("[row 13] engine exited %lld ms after the host was killed (budget %lld ms); "
                "file valid with %lld frames, %.3f s\n",
                static_cast<long long>(exit_after.count()), static_cast<long long>(kExitBudget.count()),
                static_cast<long long>(report.value().decoded_frames), report.value().duration_seconds);

    // 4. The sidecar is gone. SPEC.md §10.4 makes its presence the claim that a
    //    recording is unfinished, and it is retracted only when the file has been
    //    *proven* finished -- so a stale one would mean the next launch offers to
    //    repair a recording that needs nothing.
    EXPECT_FALSE(std::filesystem::exists(fc::mux::sidecar_path_for(output)))
        << "the recovery sidecar survived a clean finalize; the next launch would offer to repair a finished file";
}

// The negative control. Without it, an engine that exited *immediately* on any host
// death -- discarding the recording -- would pass the case above just as well, and so
// would one that never started recording in the first place.
TEST_F(OrphanPreventionTest, AHostThatShutsDownCleanlyAlsoLeavesNoEngineAndAValidFile) {
    const std::filesystem::path output = temp_.path() / "clean.mkv";

    HostProcess host;
    ASSERT_TRUE(spawn_host({"start", "\"" + output.string() + "\"", "sleep", "3000", "stop", "shutdown"}, host));

    std::string transcript;
    static_cast<void>(read_until(host.stdout_read, "SHUTDOWN", 60s, transcript));
    ASSERT_NE(transcript.find("STOPPED"), std::string::npos) << "stop_record did not complete:\n" << transcript;

    const DWORD engine_pid = pid_from_transcript(transcript);
    ASSERT_NE(engine_pid, 0U);

    EXPECT_EQ(::WaitForSingleObject(host.process, 30000), WAIT_OBJECT_0) << "the host did not exit on its own";
    const std::chrono::milliseconds exit_after = wait_for_exit(engine_pid, kExitBudget);
    EXPECT_LE(exit_after.count(), kExitBudget.count()) << "the engine did not exit after a commanded shutdown";

    ::CloseHandle(host.process);
    ::CloseHandle(host.stdout_read);

    fc::mux::ValidationExpectation expectation;
    expectation.duration_seconds = 0.0;
    const fc::Result<fc::mux::ValidationReport> report = fc::mux::Muxer::validate(output, expectation);
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report.value().valid) << report.value().detail;
    EXPECT_GT(report.value().decoded_frames, 0);

    std::printf("[row 13 control] clean shutdown: engine exited %lld ms after the host, file valid with %lld frames\n",
                static_cast<long long>(exit_after.count()), report.value().decoded_frames);
}

// SPEC.md §3.1: "Exactly **one** engine instance per user session, enforced by a named
// mutex." Asserted directly rather than through the host, because the failure it
// prevents -- two engines each holding a capture session and each writing a file -- is
// invisible from the outside until both files turn out to be broken.
TEST_F(OrphanPreventionTest, ASecondEngineRefusesToStart) {
    fc::ipc::SingleInstanceGuard first;
    ASSERT_TRUE(first.acquire("framecapture-engine-test-instance").has_value());
    EXPECT_TRUE(first.held());

    fc::ipc::SingleInstanceGuard second;
    const fc::Result<void> refused = second.acquire("framecapture-engine-test-instance");
    ASSERT_FALSE(refused.has_value()) << "a second engine acquired the single-instance mutex";
    EXPECT_EQ(refused.error(), fc::FcError::IPC_ENGINE_ALREADY_RUNNING);
    EXPECT_FALSE(second.held());
}

} // namespace
