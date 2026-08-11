#include "core/capture/dda/dda_capture.h"
#include "core/timing/qpc_clock.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/util/thread_utils.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fc::capture::dda {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kQueueCapacity = 4;

/// SPEC.md §4.3: "exponential backoff (10 ms -> 500 ms cap)".
constexpr auto kBackoffInitial = std::chrono::milliseconds{10};
constexpr auto kBackoffCap = std::chrono::milliseconds{500};

/// How long AcquireNextFrame waits before reporting "no screen change". Short
/// enough that a static desktop still produces duplicate frames at a usable rate.
constexpr UINT kAcquireTimeoutMs = 16;

/// Shared with the audio path and the test source, so the four call sites that
/// need QPC in nanoseconds cannot drift apart in how they convert it (BUG-019).
std::uint64_t qpc_now_ns() {
    return static_cast<std::uint64_t>(timing::qpc_now_ns());
}

} // namespace

struct DdaCapture::Impl {
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> recoveries{0};
    std::atomic<std::uint32_t> sequence{0};

    std::mutex mutex;
    std::condition_variable frame_ready;
    std::deque<CaptureFrame> queue;

    std::mutex start_mutex;
    std::condition_variable started;
    bool start_complete = false;
    Result<void> start_result = FcError::CAPTURE_INIT_FAILED;

    ID3D11Device* device = nullptr;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    /// Our own copy target: the texture DDA hands back must be released before
    /// ReleaseFrame, so it cannot be passed downstream directly.
    ComPtr<ID3D11Texture2D> latest;

    CaptureTarget target;
    gpu::AdapterId adapter;
    UINT width = 0;
    UINT height = 0;

    void push(CaptureFrame frame) {
        std::unique_lock lock(mutex);
        if (queue.size() >= kQueueCapacity) {
            CaptureFrame stale = queue.front();
            queue.pop_front();
            if (stale.texture != nullptr) {
                stale.texture->Release();
            }
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
        queue.push_back(frame);
        lock.unlock();
        frame_ready.notify_one();
    }

    void drain() {
        const std::lock_guard lock(mutex);
        for (CaptureFrame& frame : queue) {
            if (frame.texture != nullptr) {
                frame.texture->Release();
            }
        }
        queue.clear();
    }

    /// Emits the current contents of `latest` as a frame, AddRef'ing for the
    /// consumer.
    void emit(bool duplicated) {
        if (latest == nullptr) {
            return;
        }
        CaptureFrame frame;
        latest->AddRef();
        frame.texture = latest.Get();
        frame.qpc_ns = qpc_now_ns();
        frame.sequence = sequence.fetch_add(1, std::memory_order_relaxed);
        frame.adapter = adapter;
        frame.dxgi_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        frame.content = ContentRect{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
        frame.duplicated = duplicated;
        if (duplicated) {
            duplicates.fetch_add(1, std::memory_order_relaxed);
        }
        push(frame);
    }

    /// Creates the duplication and the copy target. Returns an error rather than
    /// throwing so the worker can back off and retry.
    Result<void> open_duplication() {
        duplication.Reset();

        const HResult hr = output->DuplicateOutput(device, &duplication);
        if (hr_failed(hr)) {
            // The affinity violation surfaces here, and only here.
            const FcError mapped = [hr] {
                if (hr == DXGI_ERROR_UNSUPPORTED) {
                    return FcError::DDA_ADAPTER_AFFINITY;
                }
                if (hr == DXGI_ERROR_ACCESS_DENIED) {
                    return FcError::DDA_ACCESS_DENIED;
                }
                return FcError::DDA_UNSUPPORTED;
            }();
            FC_LOG_ERROR(Subsystem::Capture, "DuplicateOutput failed",
                         LogFields{}
                             .add("hr", hresult_message(hr))
                             .add("hint", mapped == FcError::DDA_ADAPTER_AFFINITY
                                              ? "the device is not on the adapter that owns this output"
                                              : "")
                             .add_error(mapped));
            return mapped;
        }

        DXGI_OUTDUPL_DESC desc{};
        duplication->GetDesc(&desc);
        width = desc.ModeDesc.Width;
        height = desc.ModeDesc.Height;

        D3D11_TEXTURE2D_DESC copy_desc{};
        copy_desc.Width = width;
        copy_desc.Height = height;
        copy_desc.MipLevels = 1;
        copy_desc.ArraySize = 1;
        copy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        copy_desc.SampleDesc.Count = 1;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        latest.Reset();
        const HResult texture_hr = device->CreateTexture2D(&copy_desc, nullptr, &latest);
        if (hr_failed(texture_hr)) {
            return FcError::GPU_TEXTURE_CREATE_FAILED;
        }
        return ok();
    }
};

DdaCapture::DdaCapture() : impl_(std::make_unique<Impl>()) {}

DdaCapture::~DdaCapture() {
    stop();
}

Result<void> DdaCapture::start(ID3D11Device* device, const CaptureTarget& target) {
    if (device == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (target.is_window()) {
        // DDA duplicates outputs. Window capture requires WGC.
        return FcError::INTERNAL_NOT_IMPLEMENTED;
    }
    if (!target.is_display()) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }

    impl_->device = device;
    impl_->target = target;
    impl_->stopping.store(false, std::memory_order_release);
    impl_->start_complete = false;
    device->GetImmediateContext(&impl_->context);

    // Find the output matching the target monitor, on this device's adapter.
    //
    // Searching only this adapter's outputs is deliberate: if the monitor belongs
    // to a different adapter we want DDA_ADAPTER_AFFINITY, not a duplication on
    // the wrong device that returns S_OK and yields black frames.
    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        return FcError::GPU_DEVICE_CREATE_FAILED;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return FcError::GPU_ADAPTER_ENUMERATION_FAILED;
    }

    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        impl_->adapter =
            gpu::AdapterId{(static_cast<std::int64_t>(adapter_desc.AdapterLuid.HighPart) << 32) |
                           static_cast<std::int64_t>(static_cast<std::uint32_t>(adapter_desc.AdapterLuid.LowPart))};
    }

    impl_->output.Reset();
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> candidate;
        if (adapter->EnumOutputs(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(candidate->GetDesc(&desc))) {
            continue;
        }
        if (reinterpret_cast<std::uintptr_t>(desc.Monitor) == target.monitor) {
            if (FAILED(candidate.As(&impl_->output))) {
                return FcError::DDA_UNSUPPORTED;
            }
            break;
        }
    }

    if (impl_->output == nullptr) {
        FC_LOG_ERROR(Subsystem::Capture, "target monitor is not on this device's adapter",
                     LogFields{}
                         .add("monitor", static_cast<std::uint64_t>(target.monitor))
                         .add("adapter", impl_->adapter.to_string())
                         .add_error(FcError::DDA_ADAPTER_AFFINITY));
        return FcError::DDA_ADAPTER_AFFINITY;
    }

    impl_->worker = std::thread([this] {
        // FC_THREAD_ENTRY
        set_thread_name("capture");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        const Result<void> opened = impl_->open_duplication();
        {
            const std::lock_guard lock(impl_->start_mutex);
            impl_->start_result = opened;
            impl_->start_complete = true;
        }
        impl_->started.notify_all();

        if (!opened.has_value()) {
            clear_thread_name();
            return;
        }
        impl_->running.store(true, std::memory_order_release);

        auto backoff = kBackoffInitial;

        while (!impl_->stopping.load(std::memory_order_acquire)) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> resource;
            const HResult hr = impl_->duplication->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // SPEC.md §4.3: "no screen change" -> a duplicate frame with a
                // correctly advanced PTS, never a stall and never a gap.
                impl_->emit(true);
                continue;
            }

            if (hr == DXGI_ERROR_ACCESS_LOST) {
                // UAC prompt, secure desktop, resolution change, MUX switch.
                impl_->recoveries.fetch_add(1, std::memory_order_relaxed);
                FC_LOG_WARN(Subsystem::Capture, "duplication access lost; rebuilding",
                            LogFields{}
                                .add("backoff_ms", static_cast<std::int64_t>(backoff.count()))
                                .add_error(FcError::DDA_ACCESS_LOST));
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, kBackoffCap);
                if (impl_->open_duplication().has_value()) {
                    backoff = kBackoffInitial;
                }
                continue;
            }

            if (hr_failed(hr)) {
                FC_LOG_ERROR(Subsystem::Capture, "AcquireNextFrame failed",
                             LogFields{}.add("hr", hresult_message(hr)).add_error(FcError::CAPTURE_INIT_FAILED));
                break;
            }

            backoff = kBackoffInitial;

            // AccumulatedFrames == 0 means only the pointer moved, which is not a
            // desktop change worth a new frame.
            if (info.LastPresentTime.QuadPart != 0) {
                ComPtr<ID3D11Texture2D> acquired;
                if (SUCCEEDED(resource.As(&acquired))) {
                    // Copy immediately: the acquired surface must be released
                    // before ReleaseFrame, so it cannot travel downstream.
                    impl_->context->CopyResource(impl_->latest.Get(), acquired.Get());
                    impl_->emit(false);
                }
            } else {
                impl_->emit(true);
            }

            resource.Reset();
            impl_->duplication->ReleaseFrame();
        }

        impl_->running.store(false, std::memory_order_release);
        clear_thread_name();
    });

    std::unique_lock lock(impl_->start_mutex);
    if (!impl_->started.wait_for(lock, std::chrono::seconds{5}, [this] { return impl_->start_complete; })) {
        lock.unlock();
        stop();
        return FcError::CAPTURE_INIT_FAILED;
    }
    const Result<void> result = impl_->start_result;
    lock.unlock();

    if (!result.has_value()) {
        stop();
        return result.error();
    }

    FC_LOG_INFO(Subsystem::Capture, "DDA capture started",
                LogFields{}
                    .add("adapter", impl_->adapter.to_string())
                    .add("width", static_cast<std::int64_t>(impl_->width))
                    .add("height", static_cast<std::int64_t>(impl_->height)));
    return ok();
}

void DdaCapture::stop() {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->frame_ready.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->drain();
    impl_->duplication.Reset();
    impl_->latest.Reset();
    impl_->output.Reset();
    impl_->context.Reset();
    impl_->running.store(false, std::memory_order_release);
}

bool DdaCapture::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

Result<CaptureFrame> DdaCapture::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->frame_ready.wait_for(lock, timeout, [this] {
            return !impl_->queue.empty() || impl_->stopping.load(std::memory_order_acquire);
        })) {
        return FcError::CAPTURE_FRAME_TIMEOUT;
    }
    if (impl_->queue.empty()) {
        return FcError::CAPTURE_FRAME_TIMEOUT;
    }

    const CaptureFrame frame = impl_->queue.front();
    impl_->queue.pop_front();
    return frame;
}

void DdaCapture::release(const CaptureFrame& frame) {
    if (frame.texture != nullptr) {
        frame.texture->Release();
    }
}

std::uint64_t DdaCapture::dropped_frames() const noexcept {
    return impl_->dropped.load(std::memory_order_relaxed);
}

std::uint64_t DdaCapture::duplicate_frames() const noexcept {
    return impl_->duplicates.load(std::memory_order_relaxed);
}

std::uint64_t DdaCapture::access_lost_recoveries() const noexcept {
    return impl_->recoveries.load(std::memory_order_relaxed);
}

} // namespace fc::capture::dda
