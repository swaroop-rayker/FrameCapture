#include "core/capture/wgc/wgc_capture.h"
#include "core/timing/qpc_clock.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/util/com_apartment.h"
#include "core/util/thread_utils.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace fc::capture::wgc {
namespace {

using Microsoft::WRL::ComPtr;
namespace wgc_ns = winrt::Windows::Graphics::Capture;
namespace dx_ns = winrt::Windows::Graphics::DirectX;
namespace d3d_ns = winrt::Windows::Graphics::DirectX::Direct3D11;

/// SPEC.md §4.2: "Frame pool size: 3 buffers. Larger pools mask backpressure and
/// increase latency; smaller pools drop frames on GPU hiccups."
constexpr std::int32_t kFramePoolSize = 3;

/// Bounded, with an explicit drop-oldest policy (CLAUDE.md §4: no unbounded
/// queues). Video drops oldest; the alternative is an OOM from a 200 ms hitch.
constexpr std::size_t kQueueCapacity = 4;

/// Shared with the audio path and the test source, so the four call sites that
/// need QPC in nanoseconds cannot drift apart in how they convert it (BUG-019).
std::uint64_t qpc_now_ns() {
    return static_cast<std::uint64_t>(timing::qpc_now_ns());
}

/// Wraps our D3D11 device as the WinRT device WGC requires.
Result<d3d_ns::IDirect3DDevice> wrap_device(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        return FcError::GPU_DEVICE_CREATE_FAILED;
    }

    winrt::com_ptr<::IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) {
        return FcError::WGC_SESSION_CREATE_FAILED;
    }
    return inspectable.as<d3d_ns::IDirect3DDevice>();
}

ID3D11Texture2D* texture_from_surface(const d3d_ns::IDirect3DSurface& surface) {
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }
    return texture.Detach(); // reference handed to the caller
}

} // namespace

bool is_supported() {
    // Before the first WinRT call of the process, not merely before the first capture
    // (BUG-037). C++/WinRT caches the activation factory this reaches in a process-wide
    // static and never revalidates it; if the MTA is torn down between recordings the
    // library it belongs to is unloaded and the *second* call here faults on a vtable
    // that is no longer mapped. See `core/util/com_apartment.h`.
    static_cast<void>(hold_process_mta());

    try {
        // Probe the API rather than the OS build (SPEC.md §4.2).
        return wgc_ns::GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

struct WgcCapture::Impl {
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint32_t> sequence{0};

    std::mutex mutex;
    std::condition_variable frame_ready;
    std::deque<CaptureFrame> queue;

    /// Startup handshake, so `start()` reports a real failure instead of returning
    /// success and producing nothing.
    std::mutex start_mutex;
    std::condition_variable started;
    bool start_complete = false;
    Result<void> start_result = FcError::CAPTURE_INIT_FAILED;

    ID3D11Device* device = nullptr;
    CaptureTarget target;
    gpu::AdapterId adapter;

    void push(CaptureFrame frame) {
        std::unique_lock lock(mutex);
        if (queue.size() >= kQueueCapacity) {
            // Drop oldest: the newest frame is the one worth keeping, and the
            // capture thread must never block on a slow consumer (SPEC.md §12).
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
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}

WgcCapture::~WgcCapture() {
    stop();
}

Result<void> WgcCapture::start(ID3D11Device* device, const CaptureTarget& target) {
    if (device == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (!target.is_display() && !target.is_window()) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }
    if (!is_supported()) {
        return FcError::WGC_UNSUPPORTED;
    }

    impl_->device = device;
    impl_->target = target;
    impl_->stopping.store(false, std::memory_order_release);
    impl_->start_complete = false;

    {
        // Record which adapter the frames will live in, so downstream stages can
        // assert they are not about to use a texture from the wrong device.
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    impl_->adapter =
                        gpu::AdapterId{(static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                                       static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart))};
                }
            }
        }
    }

    impl_->worker = std::thread([this] {
        // FC_THREAD_ENTRY
        set_thread_name("capture");
        // SPEC.md §4.4: above-normal priority, MMCSS "Capture".
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        // SPEC.md §4.2: MTA, on this thread, and the WinRT objects are touched
        // from here only. The process-wide hold is what makes the matching
        // `uninit_apartment` below safe to run between recordings (BUG-037): without it,
        // this thread leaving is the last thread leaving, and the MTA goes with it.
        static_cast<void>(hold_process_mta());
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        wgc_ns::Direct3D11CaptureFramePool pool{nullptr};
        wgc_ns::GraphicsCaptureSession session{nullptr};
        wgc_ns::GraphicsCaptureItem item{nullptr};

        try { // FC_THREAD_ENTRY
            auto interop = winrt::get_activation_factory<wgc_ns::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
            winrt::com_ptr<::IInspectable> raw_item;
            const HRESULT hr =
                impl_->target.is_window()
                    ? interop->CreateForWindow(reinterpret_cast<HWND>(impl_->target.window),
                                               winrt::guid_of<wgc_ns::GraphicsCaptureItem>(), raw_item.put_void())
                    : interop->CreateForMonitor(reinterpret_cast<HMONITOR>(impl_->target.monitor),
                                                winrt::guid_of<wgc_ns::GraphicsCaptureItem>(), raw_item.put_void());
            if (FAILED(hr) || !raw_item) {
                const std::lock_guard lock(impl_->start_mutex);
                impl_->start_result = FcError::CAPTURE_TARGET_NOT_FOUND;
                impl_->start_complete = true;
                impl_->started.notify_all();
                winrt::uninit_apartment();
                return;
            }
            item = raw_item.as<wgc_ns::GraphicsCaptureItem>();

            // SPEC.md §14.2: "Monitor unplugged -> migrate to the primary display or stop
            // cleanly." This is the only notice WGC gives that the thing being captured has
            // gone away -- there is no error from anywhere else, because a push-only
            // backend with nothing to push looks exactly like an idle desktop.
            //
            // **That distinction is the whole reason this subscription exists.** The
            // session used to infer a dead capture from silence, which fired on every idle
            // machine (see `RecordingSession`'s row 9 note); it now keys on `running()`
            // instead, and without this handler an unplugged monitor would leave WGC
            // reporting itself alive and quiet forever. Trading a false positive for a
            // false negative would not have been a fix.
            item.Closed([this](const wgc_ns::GraphicsCaptureItem&, const winrt::Windows::Foundation::IInspectable&) {
                FC_LOG_WARN(Subsystem::Capture, "the capture target closed; the session is no longer live",
                            LogFields{}
                                .add("target", impl_->target.is_window() ? "window" : "display")
                                .add_error(FcError::CAPTURE_TARGET_GONE));
                impl_->running.store(false, std::memory_order_release);
                // So a consumer blocked in `acquire` finds out now rather than after its
                // timeout, and so `stop` is not delayed behind one.
                impl_->frame_ready.notify_all();
            });

            auto winrt_device = wrap_device(impl_->device);
            if (!winrt_device.has_value()) {
                const std::lock_guard lock(impl_->start_mutex);
                impl_->start_result = winrt_device.error();
                impl_->start_complete = true;
                impl_->started.notify_all();
                winrt::uninit_apartment();
                return;
            }

            const auto size = item.Size();

            // SPEC.md §4.2: request BGRA8. If the system hands back FP16 the
            // converter refuses and the tone-map branch is required.
            pool = wgc_ns::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrt_device.value(), dx_ns::DirectXPixelFormat::B8G8R8A8UIntNormalized, kFramePoolSize, size);

            session = pool.CreateCaptureSession(item);
            session.IsCursorCaptureEnabled(impl_->target.capture_cursor);

            // Win11 22000+ only. Probed by property presence, never by OS version.
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    winrt::name_of<wgc_ns::GraphicsCaptureSession>(), L"IsBorderRequired")) {
                session.IsBorderRequired(false);
            }

            pool.FrameArrived([this](const wgc_ns::Direct3D11CaptureFramePool& sender, const auto&) {
                // SPEC.md §4.2: acquire, get the texture, enqueue, return. No
                // conversion, no encoding, no disk, no logging above TRACE.
                auto frame = sender.TryGetNextFrame();
                if (frame == nullptr) {
                    return;
                }

                CaptureFrame out;
                out.texture = texture_from_surface(frame.Surface());
                if (out.texture == nullptr) {
                    return;
                }

                D3D11_TEXTURE2D_DESC desc{};
                out.texture->GetDesc(&desc);

                out.qpc_ns = qpc_now_ns();
                out.sequence = impl_->sequence.fetch_add(1, std::memory_order_relaxed);
                out.adapter = impl_->adapter;
                out.dxgi_format = static_cast<std::uint32_t>(desc.Format);
                const auto content = frame.ContentSize();
                out.content = ContentRect{0, 0, content.Width, content.Height};
                impl_->push(out);
            });

            session.StartCapture();

            {
                const std::lock_guard lock(impl_->start_mutex);
                impl_->start_result = ok();
                impl_->start_complete = true;
            }
            impl_->started.notify_all();
            impl_->running.store(true, std::memory_order_release);

            // FrameArrived is free-threaded, so this thread only has to stay alive
            // to own the WinRT objects.
            while (!impl_->stopping.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        } catch (const winrt::hresult_error& error) { // FC_THREAD_ENTRY
            const std::lock_guard lock(impl_->start_mutex);
            impl_->start_result = FcError::WGC_SESSION_CREATE_FAILED;
            impl_->start_complete = true;
            impl_->started.notify_all();
            FC_LOG_ERROR(Subsystem::Capture, "WGC session failed",
                         LogFields{}
                             .add("hr", hresult_message(static_cast<HResult>(error.code())))
                             .add_error(FcError::WGC_SESSION_CREATE_FAILED));
        }

        if (session != nullptr) {
            session.Close();
        }
        if (pool != nullptr) {
            pool.Close();
        }
        impl_->running.store(false, std::memory_order_release);
        clear_thread_name();
        winrt::uninit_apartment();
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

    FC_LOG_INFO(Subsystem::Capture, "WGC capture started",
                LogFields{}
                    .add("target", impl_->target.is_window() ? "window" : "display")
                    .add("adapter", impl_->adapter.to_string())
                    .add("pool_size", static_cast<std::int64_t>(kFramePoolSize))
                    .add("cursor", impl_->target.capture_cursor));
    return ok();
}

void WgcCapture::stop() {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->frame_ready.notify_all();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->drain();
    impl_->running.store(false, std::memory_order_release);
}

bool WgcCapture::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

Result<CaptureFrame> WgcCapture::acquire(std::chrono::milliseconds timeout) {
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

void WgcCapture::release(const CaptureFrame& frame) {
    if (frame.texture != nullptr) {
        frame.texture->Release();
    }
}

std::uint64_t WgcCapture::dropped_frames() const noexcept {
    return impl_->dropped.load(std::memory_order_relaxed);
}

} // namespace fc::capture::wgc
