#include "core/audio/loopback_capture.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/util/com_apartment.h"
#include "core/util/thread_utils.h"

#include <windows.h>
// Must follow windows.h.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <future>
#include <thread>
#include <utility>

namespace fc::audio {
namespace {

using Microsoft::WRL::ComPtr;

/// SPEC.md §8.1: a 20 ms buffer. WASAPI wants 100 ns units.
constexpr REFERENCE_TIME kBufferDuration = 20LL * 10'000;

constexpr std::int64_t kNsPerSecond = 1'000'000'000;

/// `kDeviceFriendlyName`, spelled out rather than pulled in.
///
/// `functiondiscoverykeys_devpkey.h` uses `DEFINE_PROPERTYKEY` without including
/// the header that defines it, which MSVC tolerates through an incidental include
/// chain and clang does not -- it fails to parse the whole translation unit, and
/// then clang-tidy reports nonsense from the broken AST. Including
/// `propkeydef.h` first does not help: it undefines the macro on the way out and
/// breaks MSVC instead.
///
/// One well-known GUID is cheaper than a header that only works on one compiler.
const PROPERTYKEY kDeviceFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                         14};

/// UTF-8 from a Windows wide string. Device names routinely contain non-ASCII --
/// "Écouteurs", "スピーカー" -- and truncating each wchar_t to a char would mangle
/// them in the log and the session preamble.
std::string narrow(const wchar_t* wide) {
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

/// `u64QPCPosition` is already in 100 ns units, so this is exact.
std::int64_t qpc_units_to_ns(std::uint64_t qpc_position) noexcept {
    return static_cast<std::int64_t>(qpc_position) * 100;
}

/// RAII for the MMCSS registration. SPEC.md §8.1 requires the audio thread to run
/// as "Pro Audio" at AVRT_PRIORITY_CRITICAL; without it the thread is descheduled
/// under load and the endpoint overruns, which shows up as discontinuities rather
/// than as anything obviously priority-related.
class MmcssRegistration {
public:
    MmcssRegistration() {
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (handle_ == nullptr) {
            FC_LOG_WARN(Subsystem::Audio, "MMCSS registration failed; the audio thread runs at normal priority",
                        LogFields{}.add("gle", static_cast<std::int64_t>(GetLastError())));
            return;
        }
        if (AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL) == 0) {
            FC_LOG_WARN(Subsystem::Audio, "could not raise the audio thread to AVRT_PRIORITY_CRITICAL",
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

/// Owns a `WAVEFORMATEX*` from `GetMixFormat`, which must be released with
/// `CoTaskMemFree` rather than `delete`.
class MixFormatHandle {
public:
    MixFormatHandle() = default;

    ~MixFormatHandle() {
        if (format_ != nullptr) {
            CoTaskMemFree(format_);
        }
    }

    MixFormatHandle(const MixFormatHandle&) = delete;
    MixFormatHandle& operator=(const MixFormatHandle&) = delete;
    MixFormatHandle(MixFormatHandle&&) = delete;
    MixFormatHandle& operator=(MixFormatHandle&&) = delete;

    [[nodiscard]] WAVEFORMATEX** address() noexcept {
        return &format_;
    }

    [[nodiscard]] WAVEFORMATEX* get() const noexcept {
        return format_;
    }

private:
    WAVEFORMATEX* format_ = nullptr;
};

/// Reads the endpoint's format, including the extensible fields when present.
///
/// A plain `WAVEFORMATEX` carries no channel mask, so a 5.1 endpoint that reports
/// one leaves the layout ambiguous -- which is why SPEC.md §8.1 says to negotiate
/// the `WAVEFORMATEXTENSIBLE` and why the mask is carried forward rather than
/// inferred from the channel count.
MixFormat describe(const WAVEFORMATEX* wfx) {
    MixFormat format;
    format.sample_rate = static_cast<int>(wfx->nSamplesPerSec);
    format.channels = wfx->nChannels;
    format.bits_per_sample = wfx->wBitsPerSample;
    format.is_float = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        format.channel_mask = extensible->dwChannelMask;
        format.is_float = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return format;
}

} // namespace

struct LoopbackCapture::Impl {
    Impl() = default;

    // Owns COM interfaces, OS event handles and a running thread. None of that
    // survives being copied, and moving it out from under the capture thread
    // would be worse.
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClock2> clock;

    HANDLE buffer_event = nullptr;
    HANDLE stop_event = nullptr;

    MixFormat format;
    std::string device_name;
    LoopbackSink sink;

    std::thread thread;
    std::atomic<bool> running{false};

    std::atomic<std::uint64_t> buffers{0};
    std::atomic<std::uint64_t> silent{0};
    std::atomic<std::uint64_t> discontinuities{0};

    std::string requested_device_id;
    std::promise<Result<void>> opened;

    void capture_loop();
    void drain_available();
    [[nodiscard]] Result<void> open_endpoint();

    ~Impl() {
        if (buffer_event != nullptr) {
            CloseHandle(buffer_event);
        }
        if (stop_event != nullptr) {
            CloseHandle(stop_event);
        }
    }
};

void LoopbackCapture::Impl::drain_available() {
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
            FC_LOG_ERROR(Subsystem::Audio, "IAudioCaptureClient::GetBuffer failed",
                         LogFields{}.add("hr", hresult_message(hr)).add_error(FcError::AUDIO_DEVICE_LOST));
            return;
        }

        LoopbackBuffer buffer;
        buffer.frames = frames;
        buffer.bytes_per_frame = format.bytes_per_frame();
        buffer.flags.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        buffer.flags.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        // SPEC.md §8.3: the QPC position is authoritative, not a sample counter.
        buffer.qpc_ns = qpc_units_to_ns(qpc_position);
        buffer.device_position_frames = static_cast<std::int64_t>(device_position);

        // A SILENT buffer's contents are undefined. The pointer is withheld so a
        // caller cannot read it by accident; the frame count still stands, and
        // the timeline fills the duration with zeros (SPEC.md §8.2).
        buffer.data = buffer.flags.silent ? nullptr : data;

        buffers.fetch_add(1, std::memory_order_relaxed);
        if (buffer.flags.silent) {
            silent.fetch_add(1, std::memory_order_relaxed);
        }
        if (buffer.flags.discontinuity) {
            discontinuities.fetch_add(1, std::memory_order_relaxed);
            FC_LOG_WARN(Subsystem::Audio, "WASAPI reported a data discontinuity",
                        LogFields{}.add("frames", static_cast<std::int64_t>(frames)));
        }

        if (sink) {
            sink(buffer);
        }

        // Released unconditionally: leaving a buffer checked out wedges the
        // endpoint far more thoroughly than dropping one would.
        if (const HRESULT released = capture->ReleaseBuffer(frames); FAILED(released)) {
            FC_LOG_ERROR(Subsystem::Audio, "IAudioCaptureClient::ReleaseBuffer failed",
                         LogFields{}.add("hr", hresult_message(released)));
            return;
        }
    }
}

void LoopbackCapture::Impl::capture_loop() {
    // FC_THREAD_ENTRY
    set_thread_name("fc-audio");

    // The audio thread owns its own apartment. WASAPI interfaces are apartment
    // sensitive and the thread that pumps them must be the one that initialised.
    //
    // The process-wide hold is separate and outlives this thread (BUG-037): the
    // `CoUninitialize` at the bottom must not be able to be the call that shuts the
    // process's MTA down between recordings.
    static_cast<void>(hold_process_mta());
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool owns_com = SUCCEEDED(com);

    try {
        // Opened here, not in `start`, and this is load-bearing. WASAPI interfaces
        // are apartment-sensitive: created on one thread and pumped from another
        // they are marshalled at best, and `CoCreateInstance` fails outright when
        // the creating thread never entered an apartment -- which is the common
        // case for a caller that has not initialised COM itself.
        //
        // Doing it here means the thread that pumps is the thread that created,
        // and a caller needs no COM knowledge at all.
        const Result<void> open_result = open_endpoint();
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
                // No buffer for two seconds. Not an error and not handled here:
                // silence is the timeline's problem, and it is watching the clock
                // rather than this thread (SPEC.md §8.2).
                continue;
            }
            if (result != WAIT_OBJECT_0 + 1) {
                FC_LOG_ERROR(
                    Subsystem::Audio, "audio wait failed",
                    LogFields{}.add("result", static_cast<std::int64_t>(result)).add_error(FcError::AUDIO_DEVICE_LOST));
                break;
            }
            drain_available();
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Audio, "audio thread terminated by an exception",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }

    if (owns_com) {
        CoUninitialize();
    }
    clear_thread_name();
}

namespace {

/// Enters a COM apartment for the duration of a call, and leaves it only if it was
/// the one that entered.
///
/// The capture thread initialises its own apartment because WASAPI interfaces are
/// apartment-sensitive and the thread that pumps must be the thread that created.
/// Enumeration has no such constraint -- it creates, reads and releases within one
/// call -- but it still needs *an* apartment, and its callers are threads that have
/// no reason to know that: the IPC thread answering `get_devices`, the watchdog
/// deciding where to migrate, a test's main thread.
///
/// `RPC_E_CHANGED_MODE` means the thread is already in an apartment of the other
/// kind. That is not an error here: the enumerator works from either, and the one
/// thing that would be wrong is uninitialising an apartment this scope did not
/// enter.
class ScopedApartment {
public:
    /// `enter()` takes the process-wide MTA hold before entering this scope's apartment,
    /// and that hold outlives the scope (BUG-037). Enumeration is called from the IPC
    /// thread between recordings, so without it the `CoUninitialize` below can be the
    /// call that shuts the process's MTA down and unloads the libraries a cached WinRT
    /// activation factory still points at.
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
        static_cast<void>(hold_process_mta());
        return SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }

    bool owned_ = false;
};

} // namespace

Result<std::vector<RenderEndpoint>> enumerate_render_endpoints() {
    const ScopedApartment apartment;

    ComPtr<IMMDeviceEnumerator> enumerator;
    FC_HR_AS(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
             FcError::AUDIO_INIT_FAILED);

    // The default is resolved first so each entry can be flagged. A failure here is
    // not fatal: a machine with endpoints but no default is unusual rather than
    // impossible, and the list is still worth returning.
    std::string default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(default_device->GetId(&raw))) {
            default_id = narrow(raw);
            CoTaskMemFree(raw);
        }
    }

    // `eRender` only. CLAUDE.md §2 rule 6 makes microphone capture a hard non-goal,
    // and an enumeration that returned `eCapture` endpoints would be the first step
    // toward accidentally offering one.
    ComPtr<IMMDeviceCollection> collection;
    FC_HR_AS(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection),
             FcError::AUDIO_NO_RENDER_ENDPOINT);

    UINT count = 0;
    FC_HR_AS(collection->GetCount(&count), FcError::AUDIO_NO_RENDER_ENDPOINT);

    std::vector<RenderEndpoint> endpoints;
    endpoints.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }

        RenderEndpoint endpoint;
        LPWSTR raw = nullptr;
        if (SUCCEEDED(device->GetId(&raw))) {
            endpoint.id = narrow(raw);
            CoTaskMemFree(raw);
        }
        endpoint.is_default = !endpoint.id.empty() && endpoint.id == default_id;

        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR) {
                endpoint.name = narrow(name.pwszVal);
            }
            PropVariantClear(&name);
        }

        // `Activate` + `GetMixFormat` reads the format without `Initialize`, so this
        // costs nothing and cannot disturb a stream already capturing from it.
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            MixFormatHandle mix;
            if (SUCCEEDED(client->GetMixFormat(mix.address()))) {
                endpoint.format = describe(mix.get());
            }
        }

        endpoints.push_back(std::move(endpoint));
    }

    if (endpoints.empty()) {
        return FcError::AUDIO_NO_RENDER_ENDPOINT;
    }
    return endpoints;
}

LoopbackCapture::LoopbackCapture() : impl_(std::make_unique<Impl>()) {}

LoopbackCapture::~LoopbackCapture() {
    stop();
}

Result<void> LoopbackCapture::Impl::open_endpoint() {
    const std::string& device_id = requested_device_id;

    ComPtr<IMMDeviceEnumerator> enumerator;
    FC_HR_AS(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
             FcError::AUDIO_INIT_FAILED);

    if (device_id.empty()) {
        // eRender + eConsole: the endpoint the system plays through. Loopback on a
        // render endpoint is what makes this capture output rather than input --
        // no eCapture endpoint is ever opened (CLAUDE.md §2 rule 6).
        FC_HR_AS(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), FcError::AUDIO_NO_RENDER_ENDPOINT);
    } else {
        const std::wstring wide(device_id.begin(), device_id.end());
        FC_HR_AS(enumerator->GetDevice(wide.c_str(), &device), FcError::AUDIO_NO_RENDER_ENDPOINT);
    }

    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(kDeviceFriendlyName, &name)) && name.vt == VT_LPWSTR) {
            device_name = narrow(name.pwszVal);
        }
        PropVariantClear(&name);
    }

    FC_HR_AS(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
             FcError::AUDIO_ENDPOINT_ACTIVATE_FAILED);

    MixFormatHandle mix;
    FC_HR_AS(client->GetMixFormat(mix.address()), FcError::AUDIO_MIX_FORMAT_UNSUPPORTED);
    format = describe(mix.get());

    // SPEC.md §8.1: loopback, event-driven, 20 ms.
    const HResult init =
        client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                           kBufferDuration, 0, mix.get(), nullptr);
    if (hr_failed(init)) {
        FC_LOG_ERROR(Subsystem::Audio, "loopback client initialisation failed",
                     LogFields{}
                         .add("hr", hresult_message(init))
                         .add("hint", "event-driven loopback requires Windows 10 1703 or newer")
                         .add_error(FcError::AUDIO_LOOPBACK_INIT_FAILED));
        return FcError::AUDIO_LOOPBACK_INIT_FAILED;
    }

    buffer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (buffer_event == nullptr || stop_event == nullptr) {
        return FcError::AUDIO_INIT_FAILED;
    }

    FC_HR_AS(client->SetEventHandle(buffer_event), FcError::AUDIO_LOOPBACK_INIT_FAILED);
    FC_HR_AS(client->GetService(IID_PPV_ARGS(&capture)), FcError::AUDIO_LOOPBACK_INIT_FAILED);

    // SPEC.md §8.3's cross-check does not come from here, and used to say it did
    // (BUG-041). The device position it compares against QPC is the `device_position`
    // out-parameter of `IAudioCaptureClient::GetBuffer`, which every endpoint supplies
    // and which `drain_available` already reads on every packet. `IAudioClock2` was
    // acquired here, warned about when absent, and never dereferenced -- so the warning
    // announced the loss of a feature that was still working, on endpoints where the
    // cross-check then reported normally once a second for the whole recording.
    //
    // Kept rather than deleted because §8.3 names `IAudioClock2::GetDevicePosition`
    // explicitly, so its availability is worth recording; demoted to DEBUG and reworded
    // to say what is actually true. Whether §8.3 should be re-pointed at `GetBuffer` --
    // which supplies the same quantity, from the same endpoint, without an optional
    // interface -- is the owner's call and is noted in docs/ACCEPTANCE.md.
    if (FAILED(client->GetService(IID_PPV_ARGS(&clock)))) {
        FC_LOG_DEBUG(Subsystem::Audio,
                     "IAudioClock2 is unavailable on this endpoint; the §8.3 cross-check is unaffected because it "
                     "reads the device position from IAudioCaptureClient::GetBuffer",
                     LogFields{});
    }

    FC_HR_AS(client->Start(), FcError::AUDIO_LOOPBACK_INIT_FAILED);

    FC_LOG_INFO(Subsystem::Audio, "loopback capture started",
                LogFields{}
                    .add("device", device_name)
                    .add("sample_rate", format.sample_rate)
                    .add("channels", format.channels)
                    .add("bits", format.bits_per_sample)
                    .add("float", format.is_float)
                    .add("channel_mask", static_cast<std::int64_t>(format.channel_mask)));
    return ok();
}

Result<void> LoopbackCapture::start(const std::string& device_id, LoopbackSink sink) {
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->requested_device_id = device_id;
    impl_->sink = std::move(sink);
    impl_->opened = std::promise<Result<void>>{};
    auto ready = impl_->opened.get_future();

    // Marked running before the thread starts so `stop` can tear it down even if
    // the endpoint turns out to be unopenable -- the thread still needs joining
    // either way.
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->capture_loop(); });

    Result<void> result = ready.get();
    if (!result.has_value()) {
        // The thread has already exited; joining it is what makes `start`'s
        // failure leave nothing behind.
        impl_->running.store(false, std::memory_order_release);
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    }
    return result;
}

void LoopbackCapture::stop() {
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

    FC_LOG_INFO(Subsystem::Audio, "loopback capture stopped",
                LogFields{}
                    .add("buffers", static_cast<std::int64_t>(impl_->buffers.load()))
                    .add("silent_buffers", static_cast<std::int64_t>(impl_->silent.load()))
                    .add("discontinuities", static_cast<std::int64_t>(impl_->discontinuities.load())));
}

bool LoopbackCapture::running() const noexcept {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

MixFormat LoopbackCapture::format() const noexcept {
    return impl_->format;
}

std::uint64_t LoopbackCapture::buffers_captured() const noexcept {
    return impl_->buffers.load(std::memory_order_relaxed);
}

std::uint64_t LoopbackCapture::silent_buffers() const noexcept {
    return impl_->silent.load(std::memory_order_relaxed);
}

std::uint64_t LoopbackCapture::discontinuities() const noexcept {
    return impl_->discontinuities.load(std::memory_order_relaxed);
}

std::string LoopbackCapture::device_name() const {
    return impl_->device_name;
}

} // namespace fc::audio
