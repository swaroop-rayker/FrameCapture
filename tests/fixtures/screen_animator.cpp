#include "screen_animator.h"

#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <thread>

namespace fc::test {
namespace {

constexpr wchar_t kClassName[] = L"FrameCaptureScreenAnimator";

/// Left to right. **Black first and white last is load-bearing**:
/// `pattern_signature` reads the two extremes of the luma range off the two edges
/// of the output, and a desktop does not produce that combination by accident.
constexpr COLORREF kBars[] = {
    RGB(0, 0, 0),   RGB(0, 0, 255),   RGB(0, 255, 0),   RGB(0, 255, 255),
    RGB(255, 0, 0), RGB(255, 0, 255), RGB(255, 255, 0), RGB(255, 255, 255),
};

constexpr int kBarCount = static_cast<int>(std::size(kBars));

/// Pixels the marker block advances per tick. Large enough that consecutive
/// frames differ by more than a rounding error, small enough that a 1080p output
/// takes a couple of seconds to traverse.
constexpr int kBlockStride = 17;

/// The block alternates between these two greys as well as moving, so a tick
/// changes the picture even in the one position where a wrap could otherwise
/// repeat it. Neither grey is any of `kBars`, so the block is visible over all
/// eight -- a white block would vanish over the white bar for an eighth of its
/// travel, and produce no screen change at all while it was there.
constexpr COLORREF kBlockColours[] = {RGB(96, 96, 96), RGB(176, 176, 176)};

/// How long to let DWM composite the window before `start` reports success.
/// Without it a test's first captured frames are of the desktop underneath, which
/// is exactly the ambient content the fixture exists to exclude.
constexpr auto kSettle = std::chrono::milliseconds{250};

/// The fraction of the output width each signature strip covers. A sixteenth sits
/// comfortably inside an eighth-width bar, so the strips stay pure even if the
/// output width does not divide by eight.
constexpr int kSignatureStripDivisor = 16;

/// A memory DC with a bitmap selected into it, released in the right order.
///
/// CLAUDE.md §4 wants RAII on everything, and there are four early returns
/// between creating these and using them.
class MemorySurface {
public:
    MemorySurface() = default;

    ~MemorySurface() {
        reset();
    }

    MemorySurface(const MemorySurface&) = delete;
    MemorySurface& operator=(const MemorySurface&) = delete;
    MemorySurface(MemorySurface&&) = delete;
    MemorySurface& operator=(MemorySurface&&) = delete;

    [[nodiscard]] bool create(HDC reference, int width, int height) {
        reset();
        dc_ = CreateCompatibleDC(reference);
        if (dc_ == nullptr) {
            return false;
        }
        bitmap_ = CreateCompatibleBitmap(reference, width, height);
        if (bitmap_ == nullptr) {
            reset();
            return false;
        }
        previous_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));
        return true;
    }

    void reset() {
        if (dc_ != nullptr && previous_ != nullptr) {
            SelectObject(dc_, previous_);
        }
        previous_ = nullptr;
        if (bitmap_ != nullptr) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        if (dc_ != nullptr) {
            DeleteDC(dc_);
            dc_ = nullptr;
        }
    }

    [[nodiscard]] HDC get() const noexcept {
        return dc_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return dc_ != nullptr;
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP previous_ = nullptr;
};

/// `CloseHandle` on scope exit, for the two kernel objects the paint loop waits
/// on.
class KernelHandle {
public:
    KernelHandle() = default;

    ~KernelHandle() {
        reset();
    }

    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;
    KernelHandle(KernelHandle&&) = delete;
    KernelHandle& operator=(KernelHandle&&) = delete;

    void assign(HANDLE handle) {
        reset();
        handle_ = handle;
    }

    void reset() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

void fill(HDC dc, const RECT& rect, COLORREF colour) {
    const HBRUSH brush = CreateSolidBrush(colour);
    if (brush == nullptr) {
        return;
    }
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

} // namespace

PatternSignature pattern_signature(std::span<const std::uint8_t> luma, int width, int height) {
    PatternSignature signature;
    if (width <= 0 || height <= 0) {
        return signature;
    }
    const auto plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (luma.size() < plane) {
        return signature;
    }

    const int strip = std::max(1, width / kSignatureStripDivisor);
    const int rows = std::max(1, height / 3);
    const int right_begin = width - strip;

    double left_sum = 0.0;
    double right_sum = 0.0;
    for (int y = 0; y < rows; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < strip; ++x) {
            left_sum += luma[row + static_cast<std::size_t>(x)];
            right_sum += luma[row + static_cast<std::size_t>(right_begin + x)];
        }
    }

    const double samples = static_cast<double>(rows) * static_cast<double>(strip);
    signature.left_mean = left_sum / samples;
    signature.right_mean = right_sum / samples;
    return signature;
}

struct ScreenAnimator::Impl {
    Settings settings;

    std::thread worker;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> repaints{0};

    std::mutex start_mutex;
    std::condition_variable started;
    bool start_complete = false;
    Result<void> start_result = FcError::CAPTURE_INIT_FAILED;

    HWND window = nullptr;
    KernelHandle stop_event;
    KernelHandle timer;
    /// `{timer, stop_event}`, in the order `MsgWaitForMultipleObjects` wants them.
    HANDLE wait_handles[2] = {nullptr, nullptr};

    /// The composed picture, blitted to the window and repainted from on
    /// `WM_PAINT`, and the bars alone, which is what erases the block.
    MemorySurface frame;
    MemorySurface background;

    int origin_x = 0;
    int origin_y = 0;
    int width = 0;
    int height = 0;
    int block_width = 0;
    int block_height = 0;
    int block_top = 0;
    int block_x = 0;
    std::uint64_t index = 0;

    [[nodiscard]] RECT block_rect(int x) const {
        return RECT{x, block_top, x + block_width, block_top + block_height};
    }

    void paint_background() {
        const int bar = std::max(1, width / kBarCount);
        for (int i = 0; i < kBarCount; ++i) {
            const int left = i * bar;
            const int right = (i == kBarCount - 1) ? width : left + bar;
            const RECT rect{left, 0, right, height};
            fill(background.get(), rect, kBars[i]);
        }
        BitBlt(frame.get(), 0, 0, width, height, background.get(), 0, 0, SRCCOPY);
        fill(frame.get(), block_rect(block_x), kBlockColours[0]);
    }

    /// Advances the block one step and pushes only what changed to the window.
    /// Repainting the whole 1080p surface per tick would put a CPU fill inside
    /// every test that runs the fixture, which is the shape of cost BUG-032 was.
    ///
    /// **Through `InvalidateRect` + `UpdateWindow`, not straight to the window
    /// DC.** Blitting directly onto the window's DC updates the DWM redirection
    /// surface and never marks the window dirty, so the composited desktop does
    /// not change and neither WGC nor DDA sees anything -- measured at 60.1 Hz
    /// presented against 0.6 fps delivered. Validating an update region in
    /// `EndPaint` is what tells DWM there is something new to composite.
    void tick() {
        const auto span = static_cast<std::uint64_t>(std::max(1, width - block_width));
        const int previous_x = block_x;
        ++index;
        block_x = static_cast<int>((index * static_cast<std::uint64_t>(kBlockStride)) % span);

        const RECT stale = block_rect(previous_x);
        const RECT fresh = block_rect(block_x);

        BitBlt(frame.get(), stale.left, stale.top, block_width, block_height, background.get(), stale.left, stale.top,
               SRCCOPY);
        fill(frame.get(), fresh, kBlockColours[index % std::size(kBlockColours)]);

        const RECT dirty{std::min(stale.left, fresh.left), block_top, std::max(stale.right, fresh.right),
                         block_top + block_height};
        InvalidateRect(window, &dirty, FALSE);
        UpdateWindow(window);
        repaints.fetch_add(1, std::memory_order_relaxed);
    }

    static void pump() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    /// Waits until `due_ns`, staying responsive to messages and to `stop`.
    ///
    /// A high-resolution waitable timer rather than a sleep: the default timer
    /// granularity is ~15.6 ms, which cannot express a 60 Hz tick, and
    /// `timeBeginPeriod` is banned outright (CLAUDE.md §4) because it raises the
    /// resolution for every process on the machine -- including the one under
    /// test, whose pacing is often the thing being measured.
    void wait_until(std::int64_t due_ns) {
        for (;;) {
            const std::int64_t remaining = due_ns - timing::qpc_now_ns();
            if (remaining <= 0 || stopping.load(std::memory_order_acquire)) {
                return;
            }
            LARGE_INTEGER relative;
            relative.QuadPart = -(remaining / 100);
            if (wait_handles[0] != nullptr && relative.QuadPart != 0 &&
                SetWaitableTimer(wait_handles[0], &relative, 0, nullptr, nullptr, FALSE) != 0) {
                MsgWaitForMultipleObjects(2, wait_handles, FALSE, INFINITE, QS_ALLINPUT);
            } else {
                MsgWaitForMultipleObjects(1, &wait_handles[1], FALSE, 1, QS_ALLINPUT);
            }
            pump();
        }
    }

    void finish_start(Result<void> result) {
        {
            const std::lock_guard lock(start_mutex);
            start_result = result;
            start_complete = true;
        }
        started.notify_all();
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return 0;
        }

        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));

        if (message == WM_ERASEBKGND) {
            // The whole surface is painted from the memory DC; letting the system
            // erase first only adds a flash of the class background.
            return 1;
        }

        if (message == WM_PAINT && impl != nullptr && impl->frame.valid()) {
            PAINTSTRUCT paint;
            const HDC dc = BeginPaint(window, &paint);
            if (dc != nullptr) {
                // Only the invalidated region: a tick dirties the block's band,
                // and repainting 1080p per tick would make the fixture the most
                // expensive thing in the test.
                const RECT& area = paint.rcPaint;
                BitBlt(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, impl->frame.get(),
                       area.left, area.top, SRCCOPY);
                EndPaint(window, &paint);
            }
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }

    /// Registers the class once per process. A second animator in the same binary
    /// gets `ERROR_CLASS_ALREADY_EXISTS`, which is success.
    static bool ensure_class_registered() {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = 0;
        description.lpfnWndProc = &Impl::window_proc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kClassName;
        if (RegisterClassExW(&description) != 0) {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void run() {
        wait_handles[0] = timer.get();
        wait_handles[1] = stop_event.get();

        if (!ensure_class_registered()) {
            finish_start(FcError::CAPTURE_INIT_FAILED);
            return;
        }

        // WS_EX_NOACTIVATE so the fixture never steals focus, WS_EX_TOOLWINDOW so
        // `enumerate_windows` does not offer it as a capture source, WS_EX_TOPMOST
        // so it actually occludes -- a pattern the desktop is drawn over controls
        // nothing.
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                                 L"FrameCapture test pattern", WS_POPUP, origin_x, origin_y, width, height, nullptr,
                                 nullptr, GetModuleHandleW(nullptr), this);
        if (window == nullptr) {
            finish_start(FcError::CAPTURE_INIT_FAILED);
            return;
        }

        // Reference DC for the two compatible surfaces, and nothing else -- all
        // drawing reaches the screen through `WM_PAINT`, so there is no reason to
        // hold a window DC open for the run.
        const HDC reference = GetDC(window);
        const bool surfaces_created = reference != nullptr && frame.create(reference, width, height) &&
                                      background.create(reference, width, height);
        if (reference != nullptr) {
            ReleaseDC(window, reference);
        }
        if (!surfaces_created) {
            teardown();
            finish_start(FcError::CAPTURE_INIT_FAILED);
            return;
        }

        paint_background();
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SetWindowPos(window, HWND_TOPMOST, origin_x, origin_y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(window);

        // The whole point is to control *this* output. Landing on another one
        // would leave the test reading ambient content while looking controlled,
        // which is the failure being fixed, one level up.
        if (MonitorFromWindow(window, MONITOR_DEFAULTTONULL) != reinterpret_cast<HMONITOR>(settings.monitor)) {
            teardown();
            finish_start(FcError::CAPTURE_TARGET_NOT_FOUND);
            return;
        }

        wait_until(timing::qpc_now_ns() + std::chrono::duration_cast<std::chrono::nanoseconds>(kSettle).count());

        running.store(true, std::memory_order_release);
        finish_start(ok());

        const std::int64_t origin = timing::qpc_now_ns();
        while (!stopping.load(std::memory_order_acquire)) {
            pump();
            if (settings.fps <= 0) {
                // Static by request: the output holds one picture, so "the desktop
                // is idle" is a fact the test established rather than one it hoped
                // for.
                MsgWaitForMultipleObjects(1, &wait_handles[1], FALSE, 20, QS_ALLINPUT);
                continue;
            }
            const std::int64_t due = origin + (static_cast<std::int64_t>(index + 1) * 1'000'000'000 / settings.fps);
            wait_until(due);
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }
            tick();
        }

        running.store(false, std::memory_order_release);
        teardown();
    }

    /// GDI and window teardown, on the thread that created them -- `DestroyWindow`
    /// is only valid there.
    void teardown() {
        frame.reset();
        background.reset();
        if (window != nullptr) {
            DestroyWindow(window);
            window = nullptr;
        }
    }
};

ScreenAnimator::ScreenAnimator() : impl_(std::make_unique<Impl>()) {}

ScreenAnimator::~ScreenAnimator() {
    stop();
}

Result<void> ScreenAnimator::start(const Settings& settings) {
    if (impl_->running.load(std::memory_order_acquire) || impl_->worker.joinable()) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (settings.monitor == 0) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(settings.monitor), &info) == 0) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }

    impl_->settings = settings;
    impl_->stopping.store(false, std::memory_order_release);
    impl_->start_complete = false;
    impl_->repaints.store(0, std::memory_order_relaxed);
    impl_->index = 0;
    impl_->block_x = 0;
    impl_->origin_x = info.rcMonitor.left;
    impl_->origin_y = info.rcMonitor.top;
    impl_->width = info.rcMonitor.right - info.rcMonitor.left;
    impl_->height = info.rcMonitor.bottom - info.rcMonitor.top;

    // The block lives in the middle band, so the top third -- where
    // `pattern_signature` reads -- is bars and nothing else, whatever the block is
    // doing.
    impl_->block_width = std::max(8, impl_->width / 12);
    impl_->block_height = std::max(8, impl_->height / 6);
    impl_->block_top = impl_->height / 2;

    impl_->stop_event.assign(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (impl_->stop_event.get() == nullptr) {
        return FcError::CAPTURE_INIT_FAILED;
    }
    // A null timer is tolerated: `wait_until` falls back to a 1 ms message wait,
    // which is coarser but still correct.
    impl_->timer.assign(
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));

    impl_->worker = std::thread([this] {
        // FC_THREAD_ENTRY
        set_thread_name("animator");
        impl_->run();
        clear_thread_name();
    });

    std::unique_lock lock(impl_->start_mutex);
    if (!impl_->started.wait_for(lock, std::chrono::seconds{10}, [this] { return impl_->start_complete; })) {
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
    return ok();
}

void ScreenAnimator::stop() {
    impl_->stopping.store(true, std::memory_order_release);
    if (impl_->stop_event.get() != nullptr) {
        SetEvent(impl_->stop_event.get());
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->worker = std::thread{};
    impl_->running.store(false, std::memory_order_release);
    impl_->stop_event.reset();
    impl_->timer.reset();
}

bool ScreenAnimator::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

std::uint64_t ScreenAnimator::repaints() const noexcept {
    return impl_->repaints.load(std::memory_order_relaxed);
}

int ScreenAnimator::width() const noexcept {
    return impl_->width;
}

int ScreenAnimator::height() const noexcept {
    return impl_->height;
}

} // namespace fc::test
