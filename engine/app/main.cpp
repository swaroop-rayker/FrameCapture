// framecapture-engine entry point.
//
// Thin by construction (CLAUDE.md §5): this file wires up diagnostics and prints
// what it was built against. Logic belongs in fc_core.

#include "core/build_info.h"
#include "core/gpu/gpu_topology.h"
#include "core/ipc/engine_service.h"
#include "core/logging/crash_handler.h"
#include "core/logging/ffmpeg_log.h"
#include "core/logging/logger.h"
#include "core/logging/session_preamble.h"
#include "core/util/dpi_awareness.h"
#include "core/util/thread_utils.h"

#include <cstdio>
#include <exception>
#include <string_view>

namespace {

/// Runs the control plane until something asks it to stop (SPEC.md §15.1, §3.1).
///
/// Everything here is one call into `fc_core`, which is the point: CLAUDE.md §5 says
/// `main.cpp` is thin, and a control loop written in the app layer would be a control
/// loop no test could reach without spawning a process.
int serve() {
    fc::ipc::EngineService service;

    fc::ipc::EngineServiceSettings settings;
    // Production defaults: SPEC.md §3.1's mutex, heartbeat and shutdown handlers are
    // all on. The seams that turn them off exist for tests that run several engines at
    // once, and nothing here uses them.
    settings.enforce_single_instance = true;
    settings.enable_heartbeat = true;
    settings.enable_shutdown_signals = true;

    if (const fc::Result<void> started = service.start(settings); !started.has_value()) {
        FC_LOG_CRITICAL(fc::Subsystem::Ipc, "the control plane could not start",
                        fc::LogFields{}.add_error(started.error()));
        return 2;
    }

    std::printf("FrameCapture engine listening on %s\n", service.pipe_path().c_str());
    std::fflush(stdout);

    // Blocks until `shutdown`, a lost host heartbeat, or an OS session end -- and
    // finalizes any recording in flight before returning (SPEC.md §3.1).
    service.run();
    service.stop();
    return 0;
}

int run(bool serve_mode) {
    fc::set_thread_name("main");

    // Before anything touches DXGI: a DPI-unaware process is told a virtualised
    // desktop size and would size the whole pipeline wrongly (see BUG-004).
    fc::set_process_dpi_awareness();

    // Logging first: everything after this point is diagnosable. A logger that
    // cannot open its file is reported but is not fatal -- the engine's job is to
    // record, not to log.
    if (const fc::Result<void> started = fc::log::init(); !started.has_value()) {
        std::fprintf(stderr, "warning: logging unavailable (%.*s)\n",
                     static_cast<int>(fc::error_name(started.error()).size()), fc::error_name(started.error()).data());
    }

    // FFmpeg writes to stderr unless told otherwise, which would bypass every
    // sink SPEC.md §18 requires.
    fc::log::install_ffmpeg_bridge();

    // Crash handler second, so a fault during startup still produces artifacts,
    // and so the ring buffer it dumps already exists (SPEC.md §18).
    if (const fc::Result<void> installed = fc::crash::install(); !installed.has_value()) {
        FC_LOG_WARN(fc::Subsystem::App, "crash handler unavailable", fc::LogFields{}.add_error(installed.error()));
    }

    // GPU topology before the preamble, so the mandatory adapter enumeration in
    // SPEC.md §18 carries real data rather than the M1 placeholder.
    fc::SessionPreamble preamble = fc::collect_session_preamble();
    fc::gpu::GpuTopologyService gpu_topology;
    if (const fc::Result<void> discovered = gpu_topology.refresh(); discovered.has_value()) {
        fc::fill_preamble_from_topology(preamble, gpu_topology.topology());
    } else {
        FC_LOG_ERROR(fc::Subsystem::Gpu, "GPU discovery failed", fc::LogFields{}.add_error(discovered.error()));
    }

    fc::log_session_preamble(preamble);
    gpu_topology.log_topology();

    if (const auto selection = gpu_topology.select_for_primary_display(); selection.has_value()) {
        std::printf("\nEncoder selection: %s\n", selection.value().rationale.c_str());
    }

    std::printf("FrameCapture engine %.*s\n\n", static_cast<int>(fc::project_version().size()),
                fc::project_version().data());
    std::printf("Linked dependencies:\n");

    for (const fc::DependencyVersion& dep : fc::dependency_versions()) {
        const std::string_view linkage = fc::to_string(dep.linkage);
        std::printf("  %-16.*s %-12s (%.*s)\n", static_cast<int>(dep.name.size()), dep.name.data(), dep.version.c_str(),
                    static_cast<int>(linkage.size()), linkage.data());
    }

    // The diagnostics above run either way -- SPEC.md §18 requires the adapter
    // enumeration in every session's preamble, and a served session is still a session.
    const int status = serve_mode ? serve() : 0;

    fc::crash::set_engine_phase(fc::crash::EnginePhase::Stopped);
    fc::log::remove_ffmpeg_bridge();
    fc::crash::uninstall();
    fc::log::shutdown();
    return status;
}

/// `--serve` opens SPEC.md §15.1's control channel and runs until told to stop.
///
/// A flag rather than the default, and deliberately so: without it the binary keeps the
/// M0-onward behaviour of printing its diagnostics and exiting, which is what makes it
/// usable as a one-shot probe of the rig. Making the control plane the default would
/// turn every such invocation into a process that never returns, which reads as a hang.
///
/// `scripts/run-dev.ps1` and M8b's GUI both pass the flag.
[[nodiscard]] bool wants_serve(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::string_view{argv[i]} == "--serve") {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // SPEC.md §19: "each thread's entry point wraps its body in a catch-all that
    // logs, classifies, and transitions the state machine". main is the main
    // thread's entry point, which is why CLAUDE.md §4 permits catch(...) here and
    // nowhere else in this file.
    try { // FC_THREAD_ENTRY
        return run(wants_serve(argc, argv));
    } catch (const std::exception& error) { // FC_THREAD_ENTRY
        fc::crash::set_engine_phase(fc::crash::EnginePhase::Faulted);
        FC_LOG_CRITICAL(fc::Subsystem::App, "exception escaped main",
                        fc::LogFields{}.add("what", error.what()).add_error(fc::FcError::INTERNAL_UNHANDLED_EXCEPTION));
        fc::log::flush();
        return 1;
    } catch (...) { // FC_THREAD_ENTRY
        fc::crash::set_engine_phase(fc::crash::EnginePhase::Faulted);
        FC_LOG_CRITICAL(fc::Subsystem::App, "non-standard exception escaped main",
                        fc::LogFields{}.add_error(fc::FcError::INTERNAL_UNHANDLED_EXCEPTION));
        fc::log::flush();
        return 1;
    }
}
