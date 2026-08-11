#include "core/audio/process_loopback.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"
#include "core/util/com_apartment.h"
#include "core/util/thread_utils.h"

#include <windows.h>
// Must follow windows.h.
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace fc::audio {
namespace {

using Microsoft::WRL::ComPtr;

/// SPEC.md §8.1's 20 ms buffer, in WASAPI's 100 ns units. The same period the
/// system-mix path uses, so both tiers' silence watchdogs run on one threshold.
constexpr REFERENCE_TIME kBufferDuration = 20LL * 10'000;

/// How long `start` waits for `ActivateAudioInterfaceAsync` to call back.
///
/// Bounded because the completion handler is the only thing that will ever signal,
/// and a caller wedged forever on an audio service that did not answer is worse
/// than a track that failed to start — Tier B degrades to the tracks that did
/// (`MULTITRACK_TRACK_INIT_FAILED`), and Tier A is untouched either way.
constexpr DWORD kActivationTimeoutMs = 5'000;

/// `u64QPCPosition` is in 100 ns units, so this is exact.
[[nodiscard]] std::int64_t qpc_units_to_ns(std::uint64_t qpc_position) noexcept {
    return static_cast<std::int64_t>(qpc_position) * 100;
}

/// RAII for the MMCSS registration, as SPEC.md §8.1 requires of every thread that
/// pumps an audio endpoint. Duplicated from `loopback_capture.cpp` rather than
/// shared: it is nine lines, and the two files are the only two that need it.
class MmcssRegistration {
public:
    MmcssRegistration() {
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (handle_ == nullptr) {
            FC_LOG_WARN(Subsystem::Audio, "MMCSS registration failed on a process-loopback thread",
                        LogFields{}.add("gle", static_cast<std::int64_t>(GetLastError())));
            return;
        }
        if (AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL) == 0) {
            FC_LOG_WARN(Subsystem::Audio, "could not raise a process-loopback thread to AVRT_PRIORITY_CRITICAL",
                        LogFields{}.add("gle", static_cast<std::int64_t>(GetLastError())));
        }
    }

    ~MmcssRegistration() {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

    MmcssRegistration(const MmcssRegistration&) = delete;
    MmcssRegistration& operator=(const MmcssRegistration&) = delete;
    MmcssRegistration(MmcssRegistration&&) = delete;
    MmcssRegistration& operator=(MmcssRegistration&&) = delete;

private:
    HANDLE handle_ = nullptr;
};

/// The completion handler `ActivateAudioInterfaceAsync` calls back on.
///
/// **Its lifetime is the caller's stack frame, and its reference counting is
/// deliberately inert.** COM's rule is that the operation holds a reference until
/// it completes; the rule this satisfies instead is stronger — `activate_client`
/// blocks on `done_` until the callback has *returned*, so the object cannot
/// outlive its use. Real refcounting here would mean a heap allocation and a
/// `delete` on a callback thread, both of which CLAUDE.md §4 bans, to protect
/// against an outcome the wait already makes impossible.
///
/// **`IAgileObject` is not optional, and its absence is not a silent degradation.**
/// `ActivateAudioInterfaceAsync` queries the handler for agility before it does
/// anything else and returns `E_ILLEGAL_METHOD_CALL` (0x8000000E) if the answer is
/// no — measured, on the first version of this file, which implemented only
/// `IActivateAudioInterfaceCompletionHandler` and therefore reported "process
/// loopback is unavailable on this system" on a machine where it is perfectly
/// available. It is a marker interface with no methods; answering the QI is the
/// whole of implementing it.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~ActivationHandler() {
        if (done_ != nullptr) {
            CloseHandle(done_);
        }
    }

    ActivationHandler(const ActivationHandler&) = delete;
    ActivationHandler& operator=(const ActivationHandler&) = delete;
    ActivationHandler(ActivationHandler&&) = delete;
    ActivationHandler& operator=(ActivationHandler&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return 2; // see the class comment: lifetime is the caller's frame
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activation = E_UNEXPECTED;
        ComPtr<IUnknown> activated;
        if (operation != nullptr) {
            const HRESULT queried = operation->GetActivateResult(&activation, activated.GetAddressOf());
            if (FAILED(queried)) {
                activation = queried;
            }
        }
        result_ = activation;
        if (SUCCEEDED(activation) && activated != nullptr) {
            static_cast<void>(activated.As(&client_));
        }
        if (done_ != nullptr) {
            SetEvent(done_);
        }
        return S_OK;
    }

    /// Waits for the callback, then yields what it produced.
    [[nodiscard]] HRESULT wait(ComPtr<IAudioClient>& out) const {
        if (done_ == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (WaitForSingleObject(done_, kActivationTimeoutMs) != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        out = client_;
        return result_;
    }

private:
    HANDLE done_ = nullptr;
    HRESULT result_ = E_UNEXPECTED;
    ComPtr<IAudioClient> client_;
};

/// Issues the activation and returns the client, or the HRESULT that refused it.
///
/// The caller must already be in a COM apartment.
[[nodiscard]] HRESULT activate_client(std::uint32_t pid, ComPtr<IAudioClient>& out) {
    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.TargetProcessId = pid;
    // §8.6: "targeting a PID with `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`".
    // The tree and not the bare process, because a browser or a game renders from
    // child processes and a track aimed at the parent alone would be silent while
    // the application was audibly playing.
    activation.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT parameter{};
    PropVariantInit(&parameter);
    parameter.vt = VT_BLOB;
    parameter.blob.cbSize = sizeof(activation);
    parameter.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);

    ActivationHandler handler;
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT issued = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                       &parameter, &handler, operation.GetAddressOf());
    // Not `PropVariantClear`: the blob is `activation`, a stack object this
    // function owns, and clearing would hand it to `CoTaskMemFree`.
    if (FAILED(issued)) {
        return issued;
    }
    return handler.wait(out);
}

/// The `WAVEFORMATEXTENSIBLE` §8.6 says to supply: 48 kHz, 32-bit float, stereo.
[[nodiscard]] WAVEFORMATEXTENSIBLE supplied_format() noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(kProcessLoopbackChannels);
    format.Format.nSamplesPerSec = static_cast<DWORD>(kProcessLoopbackSampleRate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(format.Format.nChannels * (format.Format.wBitsPerSample / 8));
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = format.Format.wBitsPerSample;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

/// Enters an MTA for the duration of a call, leaving only if it was the one that
/// entered. The same shape `loopback_capture.cpp` uses, and for the same reason:
/// callers (the IPC thread answering a target list, a test's main thread) have no
/// reason to know about apartments.
class ScopedApartment {
public:
    ScopedApartment() : owned_(enter()) {}

    ~ScopedApartment() {
        if (owned_) {
            CoUninitialize();
        }
    }

    ScopedApartment(const ScopedApartment&) = delete;
    ScopedApartment& operator=(const ScopedApartment&) = delete;
    ScopedApartment(ScopedApartment&&) = delete;
    ScopedApartment& operator=(ScopedApartment&&) = delete;

private:
    static bool enter() {
        // The process-wide hold outlives this scope (BUG-037).
        static_cast<void>(hold_process_mta());
        return SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }

    bool owned_ = false;
};

/// UTF-8 from a wide image name. Executable names are ASCII far more often than
/// device names are, and "far more often" is not a reason to mangle the rest.
[[nodiscard]] std::string narrow(const wchar_t* wide) {
    if (wide == nullptr || *wide == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string narrowed(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrowed.data(), needed, nullptr, nullptr);
    return narrowed;
}

/// ASCII case-insensitive comparison.
///
/// Deliberately ASCII rather than locale-aware: Windows image names are compared
/// case-insensitively by the shell, a user types `Chrome.exe` as readily as
/// `chrome.exe`, and a locale-sensitive fold would make the same config file
/// resolve differently on two machines.
[[nodiscard]] bool same_executable(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

MixFormat process_loopback_format() noexcept {
    MixFormat format;
    format.sample_rate = kProcessLoopbackSampleRate;
    format.channels = kProcessLoopbackChannels;
    format.bits_per_sample = 32;
    format.is_float = true;
    format.channel_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    return format;
}

Result<std::vector<ProcessEntry>> enumerate_processes() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        FC_LOG_WARN(Subsystem::Audio, "could not snapshot the process list",
                    LogFields{}.add("gle", static_cast<std::int64_t>(GetLastError())));
        return FcError::INTERNAL_INVALID_STATE;
    }

    std::vector<ProcessEntry> entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            ProcessEntry item;
            item.pid = entry.th32ProcessID;
            item.executable = narrow(entry.szExeFile);
            if (item.pid != 0 && !item.executable.empty()) {
                entries.push_back(std::move(item));
            }
        } while (Process32NextW(snapshot, &entry) != 0);
    }
    CloseHandle(snapshot);
    return entries;
}

Result<std::vector<ProcessEntry>> enumerate_distinct_executables() {
    FC_TRY_ASSIGN(std::vector<ProcessEntry> entries, enumerate_processes());

    // Lowest pid per name, which is exactly what `find_process_by_executable` picks —
    // so an entry from this list and the name typed by hand resolve to the same target.
    std::vector<ProcessEntry> distinct;
    for (ProcessEntry& entry : entries) {
        const auto found = std::ranges::find_if(distinct, [&entry](const ProcessEntry& kept) {
            return same_executable(kept.executable, entry.executable);
        });
        if (found == distinct.end()) {
            distinct.push_back(std::move(entry));
        } else if (entry.pid < found->pid) {
            found->pid = entry.pid;
        }
    }

    std::ranges::sort(distinct, [](const ProcessEntry& left, const ProcessEntry& right) {
        // Case-insensitive, so a picker does not put every capitalised name first.
        return std::ranges::lexicographical_compare(left.executable, right.executable, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) < std::tolower(static_cast<unsigned char>(b));
        });
    });
    return distinct;
}

std::uint32_t find_process_by_executable(std::string_view executable) {
    if (executable.empty()) {
        return 0;
    }
    const Result<std::vector<ProcessEntry>> listed = enumerate_processes();
    if (!listed.has_value()) {
        return 0;
    }

    std::uint32_t best = 0;
    for (const ProcessEntry& entry : listed.value()) {
        if (!same_executable(entry.executable, executable)) {
            continue;
        }
        // Lowest, not first: the snapshot's order is the kernel's and is not the
        // start order. See the header for why lowest is the parent in practice.
        if (best == 0 || entry.pid < best) {
            best = entry.pid;
        }
    }
    return best;
}

ProcessWatch::ProcessWatch(std::uint32_t pid) noexcept : pid_(pid) {
    if (pid == 0) {
        return;
    }
    // `SYNCHRONIZE` is all `alive()` needs, and asking for no more is what lets a
    // track follow a process running at a different integrity level than this one.
    handle_ = OpenProcess(SYNCHRONIZE, FALSE, pid);
}

ProcessWatch::~ProcessWatch() {
    reset();
}

ProcessWatch::ProcessWatch(ProcessWatch&& other) noexcept : handle_(other.handle_), pid_(other.pid_) {
    other.handle_ = nullptr;
    other.pid_ = 0;
}

ProcessWatch& ProcessWatch::operator=(ProcessWatch&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        pid_ = other.pid_;
        other.handle_ = nullptr;
        other.pid_ = 0;
    }
    return *this;
}

void ProcessWatch::reset() noexcept {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    pid_ = 0;
}

bool ProcessWatch::valid() const noexcept {
    return handle_ != nullptr;
}

bool ProcessWatch::alive() const noexcept {
    if (handle_ == nullptr) {
        return false;
    }
    // A process object signals when the process exits, so a zero-timeout wait is
    // the whole question: `WAIT_TIMEOUT` means still running.
    return WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
}

namespace {

/// Runs the probe once. See `process_loopback_available`.
[[nodiscard]] HRESULT probe_once() {
    const ScopedApartment apartment;
    ComPtr<IAudioClient> client;
    // Against our own PID. Nothing is initialised or started, so this captures nothing
    // and disturbs nothing; the question is only whether the audio service will hand
    // over the interface at all.
    //
    // Not `FC_HR`: a failure here is the *answer*, not an error. Tier B being
    // unavailable degrades to Tier A (SPEC.md §8.6) and logging it as an engine fault
    // would put an ERROR in every log on a machine that is working exactly as designed.
    const HRESULT activation = activate_client(GetCurrentProcessId(), client); // NOLINT(fc-unchecked-hresult)
    if (SUCCEEDED(activation) && client == nullptr) {
        return E_NOINTERFACE;
    }
    return activation;
}

/// Cached: the answer cannot change while this process runs, and the probe costs an
/// activation round trip against the audio service. Function-local `static`
/// initialisation is thread-safe from C++11 onwards, which matters because the first
/// caller may be the IPC thread and the second a track starting.
[[nodiscard]] HRESULT cached_probe() {
    static const HRESULT kResult = [] {
        const HRESULT probed = probe_once();
        if (FAILED(probed)) {
            FC_LOG_WARN(
                Subsystem::Audio,
                "process loopback is unavailable on this system; Tier B cannot start and Tier A is unaffected",
                LogFields{}.add("hr", hresult_message(probed)).add_error(FcError::PROCESS_LOOPBACK_UNSUPPORTED));
        } else {
            FC_LOG_INFO(Subsystem::Audio, "process loopback probed available", LogFields{});
        }
        return probed;
    }();
    return kResult;
}

} // namespace

bool process_loopback_available() {
    return SUCCEEDED(cached_probe());
}

HResult process_loopback_probe_result() {
    return cached_probe();
}

struct ProcessLoopbackCapture::Impl {
    explicit Impl(std::uint32_t target) : pid(target) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() {
        if (buffer_event != nullptr) {
            CloseHandle(buffer_event);
        }
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
    }

    std::uint32_t pid = 0;

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;

    HANDLE buffer_event = nullptr;
    HANDLE stop_event = nullptr;

    MixFormat format = process_loopback_format();
    LoopbackSink sink;

    std::thread thread;
    std::atomic<bool> running{false};

    std::atomic<std::uint64_t> buffers{0};
    std::atomic<std::uint64_t> silent{0};
    std::atomic<std::uint64_t> discontinuities{0};
    std::atomic<std::uint64_t> qpc_fallbacks{0};
    /// Latches the QPC-fallback report so it is logged once rather than per buffer
    /// on the one thread SPEC.md §12 forbids to log (CLAUDE.md hard rule 4).
    std::atomic_flag qpc_fallback_reported;

    std::promise<Result<void>> opened;

    void capture_loop();
    void drain_available();
    [[nodiscard]] Result<void> open_client();
};

Result<void> ProcessLoopbackCapture::Impl::open_client() {
    ComPtr<IAudioClient> activated;
    const HResult activation = activate_client(pid, activated);
    if (hr_failed(activation) || activated == nullptr) {
        FC_LOG_ERROR(Subsystem::Audio, "process loopback activation failed",
                     LogFields{}
                         .add("pid", static_cast<std::int64_t>(pid))
                         .add("hr", hresult_message(activation))
                         .add_error(FcError::MULTITRACK_TRACK_INIT_FAILED));
        return FcError::MULTITRACK_TRACK_INIT_FAILED;
    }
    client = activated;

    // §8.6: the format is **supplied**, not negotiated. `GetMixFormat` is not
    // implemented on this virtual device — there is no endpoint behind it to have
    // a mix format — and asking the real endpoint what it wants and passing that
    // here is the documented way to make activation succeed and initialisation
    // fail on some configurations.
    const WAVEFORMATEXTENSIBLE format_request = supplied_format();
    const HResult init =
        client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                           kBufferDuration, 0, &format_request.Format, nullptr);
    if (hr_failed(init)) {
        FC_LOG_ERROR(Subsystem::Audio, "process loopback client initialisation failed",
                     LogFields{}
                         .add("pid", static_cast<std::int64_t>(pid))
                         .add("hr", hresult_message(init))
                         .add("hint", "the format is supplied rather than negotiated (SPEC.md §8.6)")
                         .add_error(FcError::MULTITRACK_TRACK_INIT_FAILED));
        return FcError::MULTITRACK_TRACK_INIT_FAILED;
    }

    buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (buffer_event == nullptr || stop_event == nullptr) {
        return FcError::AUDIO_INIT_FAILED;
    }

    FC_HR_AS(client->SetEventHandle(buffer_event), FcError::MULTITRACK_TRACK_INIT_FAILED);
    FC_HR_AS(client->GetService(IID_PPV_ARGS(&capture)), FcError::MULTITRACK_TRACK_INIT_FAILED);
    FC_HR_AS(client->Start(), FcError::MULTITRACK_TRACK_INIT_FAILED);

    FC_LOG_INFO(Subsystem::Audio, "process loopback capture started",
                LogFields{}
                    .add("pid", static_cast<std::int64_t>(pid))
                    .add("sample_rate", format.sample_rate)
                    .add("channels", format.channels)
                    .add("mode", "include_target_process_tree"));
    return ok();
}

void ProcessLoopbackCapture::Impl::drain_available() {
    for (;;) {
        UINT32 packet_frames = 0;
        if (FAILED(capture->GetNextPacketSize(&packet_frames)) || packet_frames == 0) {
            return;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device_position = 0;
        UINT64 qpc_position = 0;

        const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return;
        }
        if (FAILED(hr)) {
            FC_LOG_ERROR(Subsystem::Audio, "process loopback GetBuffer failed",
                         LogFields{}
                             .add("pid", static_cast<std::int64_t>(pid))
                             .add("hr", hresult_message(hr))
                             .add_error(FcError::AUDIO_DEVICE_LOST));
            return;
        }

        LoopbackBuffer buffer;
        buffer.frames = frames;
        buffer.bytes_per_frame = format.bytes_per_frame();
        buffer.flags.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        buffer.flags.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;

        // SPEC.md §8.3 makes `u64QPCPosition` authoritative, and on a real endpoint
        // it always arrives. The virtual device is not documented to supply one, and
        // a zero here would place every buffer of this track at `t0` — the timeline
        // would accept the first, drop the rest as overlapping, and the track would
        // be 20 ms long. Stamping from the same clock at the moment the buffer was
        // handed over is a strictly weaker guarantee than §8.3's and the only
        // available one; the §8.2 jitter guard absorbs the difference (BUG-042).
        if (qpc_position != 0) {
            buffer.qpc_ns = qpc_units_to_ns(qpc_position);
        } else {
            buffer.qpc_ns = timing::qpc_now_ns();
            qpc_fallbacks.fetch_add(1, std::memory_order_relaxed);
            if (!qpc_fallback_reported.test_and_set(std::memory_order_relaxed)) {
                FC_LOG_WARN(Subsystem::Audio,
                            "the process-loopback device supplied no QPC position; this track is stamped from the "
                            "engine clock instead",
                            LogFields{}.add("pid", static_cast<std::int64_t>(pid)));
            }
        }
        buffer.device_position_frames = static_cast<std::int64_t>(device_position);
        buffer.data = buffer.flags.silent ? nullptr : data;

        buffers.fetch_add(1, std::memory_order_relaxed);
        if (buffer.flags.silent) {
            silent.fetch_add(1, std::memory_order_relaxed);
        }
        if (buffer.flags.discontinuity) {
            discontinuities.fetch_add(1, std::memory_order_relaxed);
        }

        if (sink) {
            sink(buffer);
        }

        if (const HRESULT released = capture->ReleaseBuffer(frames); FAILED(released)) {
            FC_LOG_ERROR(Subsystem::Audio, "process loopback ReleaseBuffer failed",
                         LogFields{}.add("hr", hresult_message(released)));
            return;
        }
    }
}

void ProcessLoopbackCapture::Impl::capture_loop() {
    // FC_THREAD_ENTRY
    //
    // Named per track so a minidump distinguishes six of these (SPEC.md §12,
    // CLAUDE.md §4). The PID is the only stable thing to name it by — the track
    // index is the muxer's numbering and this object does not know it.
    set_thread_name("fc-ploop-" + std::to_string(pid));

    static_cast<void>(hold_process_mta());
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(com);

    try {
        // Activated here, not in `start`: the client is apartment-sensitive in the
        // same way `LoopbackCapture`'s is, so the thread that pumps must be the
        // thread that created.
        const Result<void> open_result = open_client();
        const bool ready = open_result.has_value();
        opened.set_value(open_result);
        if (!ready) {
            if (owns_com) {
                CoUninitialize();
            }
            clear_thread_name();
            return;
        }

        const MmcssRegistration mmcss;

        const std::array<HANDLE, 2> waits{stop_event, buffer_event};
        for (;;) {
            const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 2000);
            if (result == WAIT_OBJECT_0) {
                break; // stop_event
            }
            if (result == WAIT_TIMEOUT) {
                // Two seconds with nothing from the target. On a per-app track this
                // is not merely normal, it is **the steady state** (SPEC.md §8.6) —
                // an application that is not playing anything renders nothing. The
                // timeline's watchdog is what keeps the track the right length; this
                // thread has nothing to do about it.
                continue;
            }
            if (result != WAIT_OBJECT_0 + 1) {
                FC_LOG_ERROR(Subsystem::Audio, "process loopback wait failed",
                             LogFields{}
                                 .add("pid", static_cast<std::int64_t>(pid))
                                 .add("result", static_cast<std::int64_t>(result))
                                 .add_error(FcError::AUDIO_DEVICE_LOST));
                break;
            }
            drain_available();
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "process loopback thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }

    if (owns_com) {
        CoUninitialize();
    }
    clear_thread_name();
}

ProcessLoopbackCapture::ProcessLoopbackCapture(std::uint32_t pid) : impl_(std::make_unique<Impl>(pid)) {}

ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    stop();
}

Result<void> ProcessLoopbackCapture::start(LoopbackSink sink) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (impl_->pid == 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    impl_->sink = std::move(sink);
    impl_->opened = std::promise<Result<void>>{};
    auto ready = impl_->opened.get_future();

    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->capture_loop(); });

    Result<void> result = ready.get();
    if (!result.has_value()) {
        impl_->running.store(false, std::memory_order_release);
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    }
    return result;
}

void ProcessLoopbackCapture::stop() {
    if (!impl_ || !impl_->running.exchange(false)) {
        return;
    }
    if (impl_->stop_event != nullptr) {
        SetEvent(impl_->stop_event);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->client.Get() != nullptr) {
        static_cast<void>(impl_->client->Stop());
    }

    FC_LOG_INFO(Subsystem::Audio, "process loopback capture stopped",
                LogFields{}
                    .add("pid", static_cast<std::int64_t>(impl_->pid))
                    .add("buffers", static_cast<std::int64_t>(impl_->buffers.load()))
                    .add("silent_buffers", static_cast<std::int64_t>(impl_->silent.load()))
                    .add("qpc_fallbacks", static_cast<std::int64_t>(impl_->qpc_fallbacks.load())));
}

bool ProcessLoopbackCapture::running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

MixFormat ProcessLoopbackCapture::format() const noexcept {
    return impl_->format;
}

std::uint64_t ProcessLoopbackCapture::buffers_captured() const noexcept {
    return impl_->buffers.load(std::memory_order_relaxed);
}

std::uint64_t ProcessLoopbackCapture::silent_buffers() const noexcept {
    return impl_->silent.load(std::memory_order_relaxed);
}

std::uint64_t ProcessLoopbackCapture::discontinuities() const noexcept {
    return impl_->discontinuities.load(std::memory_order_relaxed);
}

std::uint64_t ProcessLoopbackCapture::qpc_fallbacks() const noexcept {
    return impl_->qpc_fallbacks.load(std::memory_order_relaxed);
}

std::uint32_t ProcessLoopbackCapture::pid() const noexcept {
    return impl_->pid;
}

} // namespace fc::audio
