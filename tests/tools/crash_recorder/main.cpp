// A recorder that exists to be killed (SPEC.md §20 row 3).
//
// `test_crash_recovery` needs a `TerminateProcess` against a *real* recording, and
// a process cannot terminate itself in a way that proves anything — the point is
// that no destructor runs, no trailer is written, and no finalization happens. So
// the recording has to live in a separate process, and this is the smallest one
// that is still a real recording: the same `VideoPipeline`, the same synthetic
// source, the same muxer, driven at wall-clock rate.
//
// By default it never exits on its own: the parent kills it, and everything it
// leaves behind is what a user would find after a power cut. `--stop-after N`
// finalizes cleanly instead, so both halves of SPEC.md §10.3 -- the killed
// recording and the finished one -- run through the same recorder rather than
// through two that might quietly differ.
//
// Not part of the shipped engine. `engine/app/main.cpp` is the real entry point;
// this is test tooling and lives with the tests.

#include "core/capture/capture_frame.h"
#include "core/config/config_schema.h"
#include "core/gpu/adapter_info.h"
#include "core/gpu/d3d_device.h"
#include "core/gpu/gpu_topology.h"
#include "core/logging/logger.h"
#include "core/pipeline/video_pipeline.h"
#include "core/timing/qpc_clock.h"

#include "synthetic_source.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::filesystem::path output;
    /// Created once frames are genuinely flowing, so the parent kills a recording
    /// that has started rather than one that is still opening an encoder.
    std::filesystem::path ready_marker;
    fc::config::Container container = fc::config::Container::Mp4;
    int width = 1280;
    int height = 720;
    int fps = 60;
    bool audio = true;
    /// Safety valve. If the parent dies without killing this process, it must not
    /// be left recording to the user's disk forever.
    int max_seconds = 600;

    /// Stop *cleanly* after this many seconds and exit 0. Zero means never, which
    /// is the crash case: the process records until something kills it.
    ///
    /// This is what lets one binary serve both halves of SPEC.md §10.3 -- the
    /// killed recording and the finalized one -- so the clean path is exercised
    /// through exactly the same recorder rather than through a second one that
    /// might differ.
    int stop_after = 0;
};

/// `std::atoi` cannot report a bad argument, and a mistyped `--fps` that silently
/// becomes zero would produce a confusing failure three layers down.
[[nodiscard]] int to_int(const std::string& text, int fallback) {
    int value = fallback;
    const char* first = text.data();
    const char* last = first + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? value : fallback;
}

[[nodiscard]] bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };

        if (flag == "--output") {
            options.output = value();
        } else if (flag == "--ready") {
            options.ready_marker = value();
        } else if (flag == "--container") {
            const auto container = fc::config::container_from_string(value());
            if (!container.has_value()) {
                return false;
            }
            options.container = *container;
        } else if (flag == "--width") {
            options.width = to_int(value(), 1280);
        } else if (flag == "--height") {
            options.height = to_int(value(), 720);
        } else if (flag == "--fps") {
            options.fps = to_int(value(), 60);
        } else if (flag == "--no-audio") {
            options.audio = false;
        } else if (flag == "--max-seconds") {
            options.max_seconds = to_int(value(), 600);
        } else if (flag == "--stop-after") {
            options.stop_after = to_int(value(), 0);
        } else {
            std::fprintf(stderr, "crash_recorder: unknown flag %s\n", flag.c_str());
            return false;
        }
    }
    return !options.output.empty();
}

/// The first adapter that reports a usable H.264 encoder.
[[nodiscard]] bool pick_adapter(fc::gpu::GpuTopologyService& topology, std::unique_ptr<fc::gpu::D3dDevice>& device,
                                std::string& encoder_name, std::uint32_t& pool_bind_flags) {
    if (!topology.refresh().has_value()) {
        return false;
    }
    for (const fc::gpu::AdapterInfo& adapter : topology.topology().adapters) {
        if (adapter.adapter_class == fc::gpu::AdapterClass::Software || !adapter.can_encode()) {
            continue;
        }
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        for (UINT i = 0;; ++i) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) {
                continue;
            }
            const fc::gpu::AdapterId id{
                (static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart))};
            if (id != adapter.id) {
                continue;
            }
            auto created = fc::gpu::create_device_on_adapter(candidate.Get());
            if (!created.has_value()) {
                continue;
            }
            device = std::make_unique<fc::gpu::D3dDevice>(std::move(created).value());
            encoder_name = adapter.encode.encoder_name;
            pool_bind_flags = adapter.encode.nv12_pool_bind_flags;
            return true;
        }
    }
    return false;
}

int record(const Options& options) {
    fc::log::Config log_config;
    log_config.directory = options.output.parent_path() / "logs";
    log_config.session_id = "crashrecorder01";
    log_config.enable_msvc_sink = false;
    if (!fc::log::init(log_config).has_value()) {
        std::fprintf(stderr, "crash_recorder: logging would not start\n");
        return 2;
    }

    fc::gpu::GpuTopologyService topology;
    std::unique_ptr<fc::gpu::D3dDevice> device;
    std::string encoder_name;
    std::uint32_t pool_bind_flags = 0;
    if (!pick_adapter(topology, device, encoder_name, pool_bind_flags)) {
        std::fprintf(stderr, "crash_recorder: no adapter reported a usable H.264 encoder\n");
        return 3;
    }

    fc::test::SyntheticSource::Settings source_settings;
    source_settings.width = options.width;
    source_settings.height = options.height;
    source_settings.fps = options.fps;

    fc::test::SyntheticSource source;
    if (!source.configure(source_settings).has_value() ||
        !source.start(device->device(), fc::capture::CaptureTarget{}).has_value()) {
        std::fprintf(stderr, "crash_recorder: the synthetic source would not start\n");
        return 4;
    }

    fc::pipeline::PipelineSettings settings;
    settings.output = options.output;
    settings.video.width = options.width;
    settings.video.height = options.height;
    settings.video.fps = options.fps;
    settings.video.container = options.container;
    settings.encoder_name = encoder_name;
    settings.pool_bind_flags = pool_bind_flags;
    if (options.audio) {
        settings.audio.source = fc::pipeline::AudioSource::SystemLoopback;
    }

    // Deliberately heap-allocated and deliberately never freed. A `VideoPipeline`
    // on the stack would be destroyed by a clean exit and its destructor would
    // finalize the recording (CLAUDE.md §1) -- which is exactly the thing this
    // program exists not to do. It is killed, so nothing here ever unwinds; the
    // leak is the honest expression of that rather than an oversight, and it is
    // the only way to be sure no finalization can sneak in.
    // FC_LINT_OK
    auto* pipeline = new fc::pipeline::VideoPipeline(); // NOLINT(cppcoreguidelines-owning-memory)
    const auto started = pipeline->start(device->device(), settings);
    if (!started.has_value()) {
        const std::string name{fc::error_name(started.error())};
        std::fprintf(stderr, "crash_recorder: pipeline start failed (%s)\n", name.c_str());
        return 5;
    }

    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds{options.max_seconds};
    const auto stop_at = options.stop_after > 0 ? begin + std::chrono::seconds{options.stop_after}
                                                : std::chrono::steady_clock::time_point::max();
    long long submitted = 0;
    bool announced = false;

    // BUG-034's instrumentation. A killed recording decodes to its last *complete*
    // fragment, so what it costs is decided by how far the container trails the
    // capture at the instant of the kill -- and nothing measured that, which left two
    // occurrences of row 3 losing a whole extra fragment with a number and no cause.
    //
    // Rewritten whole and closed every time, next to the output, so it survives
    // `TerminateProcess`: there is no unwinding and no flush to rely on.
    const std::filesystem::path progress =
        options.ready_marker.empty() ? std::filesystem::path{}
                                     : std::filesystem::path{options.ready_marker}.replace_extension(".progress");
    auto last_progress = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        auto frame = source.acquire(std::chrono::milliseconds{200});
        if (!frame.has_value()) {
            break;
        }
        // Capture-time stamping, as the real backends do (BUG-019's lesson): the
        // audio half is on the wall clock and the video half has to share it.
        fc::capture::CaptureFrame stamped = frame.value();
        stamped.qpc_ns = static_cast<std::uint64_t>(fc::timing::qpc_now_ns());
        static_cast<void>(pipeline->submit(stamped));
        source.release(frame.value());
        ++submitted;

        // Announced only once frames have actually reached the encoder. Killing a
        // process that is still opening a device would measure start-up, not
        // crash-safety.
        if (!announced && pipeline->stats().frames_encoded > 0 && !options.ready_marker.empty()) {
            const std::ofstream marker(options.ready_marker, std::ios::binary);
            announced = true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!progress.empty() && announced && now - last_progress >= std::chrono::milliseconds{200}) {
            last_progress = now;
            const fc::pipeline::PipelineStats snapshot = pipeline->stats();
            const double elapsed_s = std::chrono::duration<double>(now - begin).count();
            const double muxed_s = static_cast<double>(snapshot.last_muxed_pts_ns) / 1'000'000'000.0;
            std::ofstream out(progress, std::ios::binary | std::ios::trunc);
            out << "elapsed_s=" << elapsed_s << "\nlast_muxed_s=" << muxed_s << "\nlag_s=" << (elapsed_s - muxed_s)
                << "\nsubmitted=" << submitted << "\nencoded=" << snapshot.frames_encoded
                << "\npackets_muxed=" << snapshot.packets_muxed << "\n";
        }

        std::this_thread::sleep_until(begin + (std::chrono::nanoseconds{1'000'000'000} * submitted / options.fps));

        if (std::chrono::steady_clock::now() >= stop_at) {
            // The clean path. `stop` writes the trailer, remuxes if MP4, validates
            // and clears the sidecar -- everything a kill skips.
            source.stop();
            const auto report = pipeline->stop();
            if (!report.has_value()) {
                const std::string name{fc::error_name(report.error())};
                std::fprintf(stderr, "crash_recorder: finalization failed (%s)\n", name.c_str());
                return 8;
            }
            if (!report.value().valid) {
                std::fprintf(stderr, "crash_recorder: the output did not validate (%s)\n",
                             report.value().detail.c_str());
                return 9;
            }
            fc::log::shutdown();
            return 0;
        }
    }

    // Only reached if the parent never killed us. Say so loudly: a passing test
    // that got here proved nothing about crash-safety.
    std::fprintf(stderr, "crash_recorder: reached the %d s safety limit without being killed\n", options.max_seconds);
    return 6;
}

} // namespace

int main(int argc, char** argv) {
    // FC_THREAD_ENTRY: the process entry point. An exception escaping here aborts
    // with no diagnostic at all, and the parent only ever sees an exit code -- so
    // the message has to be written from inside the guard. Argument parsing is
    // inside it too: building a `std::string` from a command-line argument
    // allocates, and building a `std::filesystem::path` from one also converts.
    try {
        Options options;
        if (!parse(argc, argv, options)) {
            std::fprintf(stderr, "usage: crash_recorder --output <path> [--ready <path>] [--container mp4|mkv]\n"
                                 "                      [--width N] [--height N] [--fps N] [--no-audio]\n"
                                 "                      [--max-seconds N]\n");
            return 1;
        }
        return record(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "crash_recorder: %s\n", e.what());
        return 7;
    }
}
