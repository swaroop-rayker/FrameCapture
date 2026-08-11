// A headless stand-in for the GUI's *process-lifecycle* role (SPEC.md §3.1, §20 row 13).
//
// This is the "headless driver" docs/ACCEPTANCE.md names as part of M8's transport
// half. It does exactly what §3.1 says the GUI does, and nothing else:
//
//   * creates a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
//   * spawns `framecapture-engine --serve` into it,
//   * connects to §15.1's control pipe and completes the `hello` handshake,
//   * sends a heartbeat every second (see `GuiWatchdog` for why `get_stats` *is* the
//     heartbeat rather than a command §15.1 does not have),
//   * runs a small script of commands, then either exits cleanly or sits idle waiting
//     to be killed.
//
// **Why a separate binary rather than a fixture inside the test.** SPEC.md §20 row 13's
// test is "kill the GUI, assert the engine exits within 6 s having finalized the file".
// A fixture living in `fc_gpu_tests` cannot be killed without killing the test that
// would report the result -- the same reason `fc_crash_recorder` exists for row 3. The
// thing that dies has to be a process of its own.
//
// It is deliberately not a GUI and deliberately not a preview of one: no widgets, no
// config, no state machine. Everything it exercises is `fc_core`'s, so what row 13
// proves is the engine's behaviour rather than this file's.

#include "core/ipc/lifecycle.h"
#include "core/ipc/pipe_client.h"
#include "core/ipc/pipe_server.h"
#include "core/logging/logger.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

/// Emitted on stdout so the test can find the engine without guessing.
void announce(const char* key, const std::string& value) {
    std::printf("%s %s\n", key, value.c_str());
    std::fflush(stdout);
}

struct SpawnedEngine {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    DWORD pid = 0;
};

/// Spawns the engine **suspended**, assigns it to the job, then resumes.
///
/// The order is the whole point and it is easy to get wrong: a process created running
/// can execute -- and spawn children of its own -- in the window before
/// `AssignProcessToJobObject` lands, and anything it spawned in that window is outside
/// the job. Creating suspended closes the window entirely.
[[nodiscard]] bool spawn_engine(const std::filesystem::path& engine, fc::ipc::EngineJob& job,
                                const std::string& session_id, SpawnedEngine& out) {
    // Inherited by the child; this is how §3.1's pairing is communicated without
    // putting either value on a command line a user might see or script against.
    ::SetEnvironmentVariableA(fc::ipc::kJobNameEnvVar, job.name().c_str());
    ::SetEnvironmentVariableA(fc::ipc::kSessionEnvVar, session_id.c_str());

    std::wstring command = L"\"" + engine.wstring() + L"\" --serve";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};

    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup,
                         &info) == 0) {
        std::fprintf(stderr, "gui_host: CreateProcess failed, gle=%lu\n", ::GetLastError());
        return false;
    }

    if (!job.assign(info.hProcess).has_value()) {
        ::TerminateProcess(info.hProcess, 1);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        return false;
    }

    ::ResumeThread(info.hThread);

    out.process = info.hProcess;
    out.thread = info.hThread;
    out.pid = info.dwProcessId;
    return true;
}

/// The GUI's half of SPEC.md §3.1's heartbeat: one message per second, forever.
class Heartbeat {
public:
    explicit Heartbeat(fc::ipc::PipeClient& client) : client_(client) {
        thread_ = std::thread([this] {
            while (running_.load(std::memory_order_acquire)) {
                // The result is discarded: this is a liveness signal, not a query. A
                // failure means the engine is gone, which the main loop discovers on
                // its own next command.
                static_cast<void>(client_.request(fc::ipc::Command::GetStats));
                for (int i = 0; i < 10 && running_.load(std::memory_order_acquire); ++i) {
                    std::this_thread::sleep_for(fc::ipc::kHeartbeatInterval / 10);
                }
            }
        });
    }

    ~Heartbeat() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Heartbeat(const Heartbeat&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;
    Heartbeat(Heartbeat&&) = delete;
    Heartbeat& operator=(Heartbeat&&) = delete;

private:
    fc::ipc::PipeClient& client_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};

int usage() {
    std::fprintf(stderr, "usage: fc_gui_host <engine.exe> <command>...\n"
                         "  start <path>   start_record to <path>\n"
                         "  pause          pause_record\n"
                         "  resume         resume_record\n"
                         "  stop           stop_record\n"
                         "  sleep <ms>     wait\n"
                         "  idle           heartbeat forever, waiting to be killed\n"
                         "  shutdown       ask the engine to exit\n");
    return 2;
}

int run(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }

    static_cast<void>(fc::log::init());

    const std::filesystem::path engine_path{argv[1]};
    if (!std::filesystem::exists(engine_path)) {
        std::fprintf(stderr, "gui_host: no engine at %s\n", engine_path.string().c_str());
        return 2;
    }

    fc::ipc::EngineJob job;
    if (!job.create().has_value()) {
        std::fprintf(stderr, "gui_host: could not create the job object\n");
        return 2;
    }

    const std::string session = fc::ipc::new_session_id();
    SpawnedEngine engine;
    if (!spawn_engine(engine_path, job, session, engine)) {
        return 2;
    }

    announce("ENGINE_PID", std::to_string(engine.pid));
    announce("SESSION", session);

    int status = 0;
    {
        fc::ipc::PipeClient client;
        fc::ipc::PipeClientSettings client_settings;
        client_settings.session_id = session;
        client_settings.connect_timeout = 15s; // a cold engine opens D3D and DXGI first

        if (!client.connect(client_settings).has_value()) {
            std::fprintf(stderr, "gui_host: could not connect to the engine\n");
            ::TerminateProcess(engine.process, 1);
            status = 2;
        } else if (!client.handshake("fc_gui_host/1.0.0").has_value()) {
            std::fprintf(stderr, "gui_host: handshake failed\n");
            status = 2;
        } else {
            announce("READY", session);

            const Heartbeat heartbeat(client);

            for (int i = 2; i < argc && status == 0; ++i) {
                const std::string command{argv[i]};

                if (command == "start" && i + 1 < argc) {
                    nlohmann::json params = nlohmann::json::object();
                    params["output"] = std::string{argv[++i]};
                    params["audio"] = false;
                    if (const auto reply = client.request(fc::ipc::Command::StartRecord, params); reply.has_value()) {
                        announce("RECORDING", params["output"].get<std::string>());
                    } else {
                        std::fprintf(stderr, "gui_host: start_record failed (%d)\n", fc::error_code(reply.error()));
                        status = 3;
                    }
                } else if (command == "pause") {
                    status = client.request(fc::ipc::Command::PauseRecord).has_value() ? 0 : 3;
                    announce("PAUSED", "");
                } else if (command == "resume") {
                    status = client.request(fc::ipc::Command::ResumeRecord).has_value() ? 0 : 3;
                    announce("RESUMED", "");
                } else if (command == "stop") {
                    status = client.request(fc::ipc::Command::StopRecord).has_value() ? 0 : 3;
                    announce("STOPPED", "");
                } else if (command == "shutdown") {
                    static_cast<void>(client.request(fc::ipc::Command::Shutdown));
                    announce("SHUTDOWN", "");
                } else if (command == "sleep" && i + 1 < argc) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{std::stoi(argv[++i])});
                } else if (command == "idle") {
                    // SPEC.md §20 row 13's shape: hold the recording open and wait to
                    // be killed. Bounded so a test that fails to kill this leaves a
                    // stray process for seconds rather than forever.
                    announce("IDLE", "");
                    std::this_thread::sleep_for(5min);
                } else {
                    status = usage();
                }
            }
        }
    }

    // Only on the clean path. When this process is killed, none of this runs -- which
    // is exactly the condition row 13 puts the engine in, and the job handle closing
    // with it is the mechanism under test.
    if (engine.thread != nullptr) {
        ::CloseHandle(engine.thread);
    }
    if (engine.process != nullptr) {
        static_cast<void>(::WaitForSingleObject(engine.process, 10000));
        ::CloseHandle(engine.process);
    }
    job.close();
    fc::log::shutdown();
    return status;
}

} // namespace

int main(int argc, char** argv) {
    // SPEC.md §19: a thread's entry point wraps its body in a catch-all that logs and
    // classifies. `main` is the main thread's, which is why CLAUDE.md §4 permits
    // `catch(...)` here and nowhere else in this file.
    try { // FC_THREAD_ENTRY
        return run(argc, argv);
    } catch (const std::exception& error) { // FC_THREAD_ENTRY
        std::fprintf(stderr, "gui_host: exception escaped main: %s\n", error.what());
        return 1;
    } catch (...) { // FC_THREAD_ENTRY
        std::fprintf(stderr, "gui_host: non-standard exception escaped main\n");
        return 1;
    }
}
