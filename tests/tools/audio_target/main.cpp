// A process whose whole purpose is to behave like an application that plays audio
// (SPEC.md §8.6, §20 row 15).
//
// SPEC.md §8.6's Tier B intercepts what *another process* renders, through
// `ActivateAudioInterfaceAsync` and `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`. Nothing
// inside the test binary can stand in for that: the interception is keyed on a process
// id, and a fixture that played a tone from the test's own process would be capturing
// the process doing the capturing.
//
// So this is a separate executable, for the same reason `fc_crash_recorder` and
// `fc_gui_host` are.
//
// ---------------------------------------------------------------------------
// What it imitates, and why each one is here
// ---------------------------------------------------------------------------
// The first version rendered one sine wave through one shared-mode client at the
// endpoint's own format, which is the *easiest* thing an application can do. §8.6 names
// `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` and warns that the supplied format
// is a shipped-bug source, and neither of those is exercised by the easy case. So:
//
//   `--streams N`  N concurrent render clients in one process, at spaced frequencies.
//                  A browser with two tabs playing, or a game with music and effects on
//                  separate voices. The track should carry **all** of them.
//   `--child`      renders from a **child process** and nothing from the parent. A track
//                  aimed at the parent should still hear it — that is the whole of what
//                  `INCLUDE_TARGET_PROCESS_TREE` buys, and a browser is exactly this
//                  shape.
//   `--rate R`     requests R Hz rather than the endpoint's, with
//                  `AUTOCONVERTPCM`, which is what an application playing a 44.1 kHz
//                  file does. The audio engine resamples; process loopback still has to
//                  hand over 48 kHz.
//
// The tone is anchored to **QPC**, not to a sample counter, for the reason
// `synthetic_audio.h` records: the recording places audio by QPC (SPEC.md §8.3), and a
// generator with an origin of its own would make a test measure the difference between
// two clocks rather than the pipeline.
//
// Not part of the shipped engine, and it links no FrameCapture code — a helper sharing
// the engine's audio stack could hide a defect in it.

#include <windows.h>
// Must follow windows.h.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kBufferDuration = 20LL * 10'000; // 20 ms, as SPEC.md §8.1 uses

/// Spacing between the tones of a multi-stream target. Wide enough that `tone_energy`
/// cannot confuse two of them.
constexpr double kStreamSpacingHz = 700.0;

struct Options {
    double seconds = 3.0;
    double frequency = 1000.0;
    /// Deliberately modest. This plays through the machine's speakers during a test run,
    /// and a full-scale tone would be unpleasant and would clip the mix that track 0 is
    /// simultaneously recording. Divided across streams, so `--streams 3` is no louder
    /// than `--streams 1`.
    double amplitude = 0.2;
    int streams = 1;
    /// 0 follows the endpoint's mix format.
    int rate = 0;
    bool child = false;
};

[[nodiscard]] double option_value(int argc, char** argv, const char* name, double fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::strtod(argv[i + 1], nullptr);
        }
    }
    return fallback;
}

[[nodiscard]] bool option_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] double qpc_seconds() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart == 0) {
        return 0.0;
    }
    return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}

/// One render client, rendering one tone for the duration.
///
/// A whole client per stream rather than one client mixing them, because the point is to
/// be the shape an application actually is: a browser opens a client per audio element,
/// and the Windows audio engine mixes them. Mixing them here would test our arithmetic
/// instead of the engine's.
class RenderStream {
public:
    /// Opens and starts. Returns a non-zero exit code on failure.
    [[nodiscard]] int open(IMMDevice* device, const Options& options) {
        amplitude_ = options.amplitude / std::max(options.streams, 1);

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_))) {
            std::fprintf(stderr, "audio_target: could not activate the endpoint\n");
            return 5;
        }

        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client_->GetMixFormat(&mix)) || mix == nullptr) {
            std::fprintf(stderr, "audio_target: GetMixFormat failed\n");
            return 6;
        }

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        WAVEFORMATEXTENSIBLE requested{};
        const WAVEFORMATEX* format = mix;
        if (options.rate > 0 && std::cmp_not_equal(options.rate, mix->nSamplesPerSec)) {
            // What an application playing a 44.1 kHz file does: ask for its own rate and
            // let the audio engine resample. Without `AUTOCONVERTPCM` a shared-mode
            // client is simply refused, which is why the two flags travel together.
            requested.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            requested.Format.nChannels = 2;
            requested.Format.nSamplesPerSec = static_cast<DWORD>(options.rate);
            requested.Format.wBitsPerSample = 32;
            requested.Format.nBlockAlign = static_cast<WORD>(requested.Format.nChannels * 4);
            requested.Format.nAvgBytesPerSec = requested.Format.nSamplesPerSec * requested.Format.nBlockAlign;
            requested.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            requested.Samples.wValidBitsPerSample = 32;
            requested.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            requested.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            format = &requested.Format;
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }

        channels_ = format->nChannels;
        rate_ = static_cast<int>(format->nSamplesPerSec);
        const bool is_float =
            format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const int bytes_per_sample = format->wBitsPerSample / 8;

        const HRESULT initialised =
            client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, format, nullptr);
        CoTaskMemFree(mix);

        if (!is_float || bytes_per_sample != 4) {
            // Every Windows shared-mode render endpoint this project targets mixes in
            // 32-bit float. Refusing rather than writing the wrong bytes: a helper that
            // silently rendered noise would fail a test for a reason that is not the
            // test's.
            std::fprintf(stderr, "audio_target: endpoint is not 32-bit float\n");
            return 8;
        }
        if (FAILED(initialised)) {
            std::fprintf(stderr, "audio_target: Initialize failed (0x%08lX)\n",
                         static_cast<unsigned long>(initialised));
            return 7;
        }

        buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (buffer_event_ == nullptr || FAILED(client_->SetEventHandle(buffer_event_))) {
            std::fprintf(stderr, "audio_target: event setup failed\n");
            return 9;
        }
        if (FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
            std::fprintf(stderr, "audio_target: no render client\n");
            return 10;
        }
        if (FAILED(client_->GetBufferSize(&buffer_frames_))) {
            std::fprintf(stderr, "audio_target: GetBufferSize failed\n");
            return 11;
        }
        if (FAILED(client_->Start())) {
            std::fprintf(stderr, "audio_target: Start failed\n");
            return 12;
        }
        return 0;
    }

    /// Renders until `seconds` have elapsed since `began`.
    void run(double frequency, double began, double seconds) {
        DWORD mmcss_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
        const double omega = 2.0 * std::numbers::pi * frequency;

        while (qpc_seconds() - began < seconds) {
            if (WaitForSingleObject(buffer_event_, 200) != WAIT_OBJECT_0) {
                continue;
            }
            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding))) {
                break;
            }
            const UINT32 available = buffer_frames_ - padding;
            if (available == 0) {
                continue;
            }
            BYTE* raw = nullptr;
            if (FAILED(render_->GetBuffer(available, &raw))) {
                break;
            }

            // Phase from QPC at the head of this buffer, so the tone is a function of
            // wall clock rather than of how many buffers happen to have been rendered. A
            // sample-counter phase would drift against the recording's timeline by the
            // endpoint's clock error, which is precisely the quantity a test must not
            // conflate with a placement error.
            const double buffer_time = qpc_seconds() - began;
            auto* samples = reinterpret_cast<float*>(raw);
            for (UINT32 i = 0; i < available; ++i) {
                const double t = buffer_time + (static_cast<double>(i) / rate_);
                const auto value = static_cast<float>(amplitude_ * std::sin(omega * t));
                for (int c = 0; c < channels_; ++c) {
                    samples[(static_cast<std::size_t>(i) * channels_) + c] = value;
                }
            }
            if (FAILED(render_->ReleaseBuffer(available, 0))) {
                break;
            }
        }

        static_cast<void>(client_->Stop());
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
    }

    ~RenderStream() {
        if (buffer_event_ != nullptr) {
            CloseHandle(buffer_event_);
        }
    }

    RenderStream() = default;
    RenderStream(const RenderStream&) = delete;
    RenderStream& operator=(const RenderStream&) = delete;
    RenderStream(RenderStream&&) = delete;
    RenderStream& operator=(RenderStream&&) = delete;

    [[nodiscard]] int rate() const noexcept {
        return rate_;
    }

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE buffer_event_ = nullptr;
    UINT32 buffer_frames_ = 0;
    int channels_ = 2;
    int rate_ = 48000;
    double amplitude_ = 0.2;
};

/// Re-runs this executable with the same arguments minus `--child`, and waits for it.
///
/// The parent renders **nothing**. That is the point: a track aimed at this process id
/// hears the child only through `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`, and
/// a build that targeted the bare process would record silence while the machine was
/// audibly playing a tone — which is what a browser looks like.
[[nodiscard]] int run_as_parent(int argc, char** argv, double seconds) {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        std::fprintf(stderr, "audio_target: could not find my own path\n");
        return 20;
    }

    std::string command = "\"";
    for (const wchar_t* p = self; *p != L'\0'; ++p) {
        command.push_back(static_cast<char>(*p)); // build output paths are ASCII
    }
    command += "\"";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--child") == 0) {
            continue;
        }
        command += " ";
        command += argv[i];
    }

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == 0) {
        std::fprintf(stderr, "audio_target: could not spawn the rendering child\n");
        return 21;
    }
    std::printf("parent %lu spawned rendering child %lu\n", GetCurrentProcessId(), process.dwProcessId);
    std::fflush(stdout);

    // Plus a margin, so the parent outlives the child and the recording sees the tone
    // arrive from a process that is not the one it is watching.
    WaitForSingleObject(process.hProcess, static_cast<DWORD>((seconds + 5.0) * 1000.0));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

[[nodiscard]] int run(int argc, char** argv) {
    Options options;
    options.seconds = option_value(argc, argv, "--seconds", options.seconds);
    options.frequency = option_value(argc, argv, "--frequency", options.frequency);
    options.amplitude = option_value(argc, argv, "--amplitude", options.amplitude);
    options.streams = static_cast<int>(option_value(argc, argv, "--streams", options.streams));
    options.rate = static_cast<int>(option_value(argc, argv, "--rate", options.rate));
    options.child = option_flag(argc, argv, "--child");
    options.streams = std::max(1, std::min(options.streams, 8));

    if (options.child) {
        return run_as_parent(argc, argv, options.seconds);
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::fprintf(stderr, "audio_target: CoInitializeEx failed\n");
        return 2;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        std::fprintf(stderr, "audio_target: no device enumerator\n");
        return 3;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        std::fprintf(stderr, "audio_target: no default render endpoint\n");
        return 4;
    }

    std::vector<std::unique_ptr<RenderStream>> streams;
    for (int i = 0; i < options.streams; ++i) {
        auto stream = std::make_unique<RenderStream>();
        if (const int failed = stream->open(device.Get(), options); failed != 0) {
            return failed;
        }
        streams.push_back(std::move(stream));
    }

    // Announced on stdout so a test knows the tones have actually begun rather than
    // guessing from a sleep. Flushed, because a pipe the parent reads is buffered.
    std::printf("pid %lu rendering %d stream(s) from %.1f Hz for %.2f s at %d Hz\n", GetCurrentProcessId(),
                options.streams, options.frequency, options.seconds, streams.front()->rate());
    std::fflush(stdout);

    const double began = qpc_seconds();
    std::vector<std::thread> workers;
    workers.reserve(streams.size());
    for (std::size_t i = 1; i < streams.size(); ++i) {
        RenderStream* stream = streams[i].get();
        const double frequency = options.frequency + (static_cast<double>(i) * kStreamSpacingHz);
        workers.emplace_back([stream, frequency, began, &options] { stream->run(frequency, began, options.seconds); });
    }
    streams.front()->run(options.frequency, began, options.seconds);
    for (std::thread& worker : workers) {
        worker.join();
    }

    CoUninitialize();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // SPEC.md §19: a thread's entry point wraps its body in a catch-all that logs and
    // classifies. `main` is the main thread's, which is why CLAUDE.md §4 permits
    // `catch(...)` here and nowhere else in this file. The same shape `fc_gui_host` and
    // `fc_crash_recorder` use.
    try { // FC_THREAD_ENTRY
        return run(argc, argv);
    } catch (const std::exception& error) { // FC_THREAD_ENTRY
        std::fprintf(stderr, "audio_target: exception escaped main: %s\n", error.what());
        return 1;
    } catch (...) { // FC_THREAD_ENTRY
        std::fprintf(stderr, "audio_target: non-standard exception escaped main\n");
        return 1;
    }
}
