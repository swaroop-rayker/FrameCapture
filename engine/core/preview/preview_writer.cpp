#include "core/preview/preview_writer.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"
#include "core/preview/preview_scaler.h"
#include "core/timing/qpc_clock.h"
#include "core/util/thread_utils.h"
#include "core/util/worker.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace fc::preview {
namespace {

using Microsoft::WRL::ComPtr;

/// Staging surfaces between the dispatch and the readback.
///
/// **Two, and the third one is not an oversight.** At most two are ever held at once -- one
/// that `fc-preview` has taken and is mapping, and one waiting as `pending_slot` -- because
/// `offer` releases the frame it displaces in the same call that replaces it. A third slot
/// is therefore never handed out, and worse, it makes the free list *impossible* to empty:
/// with three, `PreviewWriterStats::dropped_no_slot` is unreachable code and a preview
/// deliberately wedged to test the drop path shows zero drops. Measured exactly that way --
/// see `ARecordingIsUnaffectedByAPreviewThatCannotKeepUp`, whose control caught it.
///
/// Two is also sufficient. The healthy case releases a surface within a millisecond or two
/// of taking it, against a 33 ms preview period, so the free list is never empty when a
/// frame arrives; the exhaustion path only opens when `fc-preview` is genuinely stuck for
/// longer than a whole period, which is precisely when a preview frame *should* be dropped.
constexpr int kStagingSlots = 2;

/// How long `fc-preview` will keep asking for a staging surface the GPU has not finished
/// writing, before giving the frame up.
///
/// The map is `DO_NOT_WAIT`, so this is a retry budget rather than a timeout: 8 ms at one
/// millisecond a try, against a 33 ms preview period. A copy that has not landed in a
/// quarter of a frame period is one the next frame will supersede anyway.
constexpr int kReadbackRetries = 8;
constexpr auto kReadbackRetryDelay = std::chrono::milliseconds{1};

} // namespace

struct PreviewWriter::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    PreviewRing* ring = nullptr;
    PreviewWriterSettings settings;
    PreviewScaler scaler;

    std::vector<ComPtr<ID3D11Texture2D>> staging;

    std::thread thread;
    std::promise<void> done;
    std::future<void> finished;
    std::atomic<bool> running{false};

    /// Guards the free list and the single hand-off slot. Never held across a D3D call --
    /// SPEC.md §12's rule, and the reason `offer` acquires a slot, unlocks, dispatches, and
    /// only then locks again to publish the index.
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<int> free_slots;
    int pending_slot = -1;
    std::int64_t pending_qpc_ns = 0;
    bool exiting = false;

    /// The source timestamp of the last frame that made it past the rate limiter. Capture
    /// thread only.
    std::int64_t last_dispatch_ns = 0;
    std::int64_t period_ns = 0;

    mutable std::mutex stats_mutex;
    PreviewWriterStats stats;

    void preview_loop();

    /// Copies one staging surface into the ring and publishes it. `fc-preview` only.
    void read_back_and_publish(int slot, std::int64_t qpc_ns);

    void release_slot(int slot) {
        if (slot < 0) {
            return;
        }
        const std::lock_guard lock(mutex);
        free_slots.push_back(slot);
    }
};

void PreviewWriter::Impl::read_back_and_publish(int slot, std::int64_t qpc_ns) {
    ID3D11Texture2D* surface = staging[static_cast<std::size_t>(slot)].Get();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
    for (int attempt = 0; attempt < kReadbackRetries; ++attempt) {
        hr = context->Map(surface, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            break;
        }
        std::this_thread::sleep_for(kReadbackRetryDelay);
    }

    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        ring->note_dropped();
        const std::lock_guard lock(stats_mutex);
        ++stats.dropped_readback_busy;
        return;
    }
    if (FAILED(hr)) {
        ring->note_dropped();
        FC_LOG_WARN(Subsystem::Ipc, "a preview frame could not be read back",
                    LogFields{}.add("hr", hresult_message(hr)).add_error(FcError::IPC_SHM_MAP_FAILED));
        const std::lock_guard lock(stats_mutex);
        ++stats.failures;
        return;
    }

    std::uint8_t* destination = ring->next_slot();
    if (destination != nullptr) {
        const PreviewGeometry& geometry = ring->geometry();
        const std::size_t stride = geometry.stride();
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        if (mapped.RowPitch == stride) {
            // The common case on both reference adapters, and the one worth having: a
            // 960-pixel row is 3840 bytes, which every driver measured pitches exactly.
            std::memcpy(destination, source, stride * static_cast<std::size_t>(geometry.height));
        } else {
            // A padded pitch has to be un-padded here rather than described in the header:
            // the GUI wraps a `QImage` straight over this memory (§16.1), and a stride it
            // had to be told about is a second place the layout is decided.
            for (int y = 0; y < geometry.height; ++y) {
                std::memcpy(destination + (static_cast<std::size_t>(y) * stride),
                            source + (static_cast<std::size_t>(y) * mapped.RowPitch), stride);
            }
        }
    }
    context->Unmap(surface, 0);

    if (settings.injected_publish_stall_ns > 0) {
        // Chaos injection only -- see the settings note. Deliberately *after* the map and
        // before the publish, which is where a slow consumer's cost would actually land.
        std::this_thread::sleep_for(std::chrono::nanoseconds{settings.injected_publish_stall_ns});
    }

    if (destination != nullptr) {
        ring->publish(qpc_ns);
        const std::lock_guard lock(stats_mutex);
        ++stats.published;
    }
}

void PreviewWriter::Impl::preview_loop() {
    // FC_THREAD_ENTRY
    const ScopedSignal signal(done);
    set_thread_name("fc-preview");
    // Below every thread that carries the recording. SPEC.md §12's table has no row for
    // this thread; the priority states its place in the order that table describes --
    // preview loses to capture, convert, encode, audio and mux, always.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        for (;;) {
            int slot = -1;
            std::int64_t qpc_ns = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return exiting || pending_slot >= 0; });
                if (pending_slot < 0) {
                    break; // exiting, with nothing left to publish
                }
                slot = pending_slot;
                qpc_ns = pending_qpc_ns;
                pending_slot = -1;
            }

            read_back_and_publish(slot, qpc_ns);
            release_slot(slot);
        }
    } catch (const std::exception& e) { // FC_THREAD_ENTRY
        FC_LOG_ERROR(Subsystem::Ipc, "the preview thread terminated by an exception; the recording is unaffected",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
    clear_thread_name();
}

PreviewWriter::PreviewWriter() : impl_(std::make_unique<Impl>()) {}

PreviewWriter::~PreviewWriter() {
    // `stop` joins a thread through `await_worker`, and `std::future::wait_for` throws
    // `future_error`. An exception leaving a destructor terminates the process, which for
    // a *preview* would be the worst possible trade: the one subsystem whose whole contract
    // is "degrade rather than affect the recording" taking the recording down with it.
    //
    // Caught by type and logged -- CLAUDE.md §4 bans both the catch-all outside a thread
    // entry and the empty handler. Same shape as `VideoPipeline::~VideoPipeline`.
    try {
        stop();
    } catch (const std::exception& e) {
        FC_LOG_ERROR(Subsystem::Ipc, "the preview writer's destructor could not stop it cleanly",
                     LogFields{}.add("what", e.what()).add_error(FcError::INTERNAL_UNHANDLED_EXCEPTION));
    }
}

Result<void> PreviewWriter::start(ID3D11Device* device, ID3D11DeviceContext* context, PreviewRing* ring,
                                  const PreviewWriterSettings& settings) {
    if (device == nullptr || context == nullptr || ring == nullptr || !ring->open() || settings.fps <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    stop();

    impl_->device = device;
    impl_->context = context;
    impl_->ring = ring;
    impl_->settings = settings;
    impl_->period_ns = 1'000'000'000LL / settings.fps;
    impl_->last_dispatch_ns = 0;

    const PreviewGeometry& geometry = ring->geometry();

    PreviewScalerSettings scaler_settings;
    scaler_settings.width = geometry.width;
    scaler_settings.height = geometry.height;
    scaler_settings.tone_map_hdr = settings.tone_map_hdr;
    scaler_settings.sdr_white_nits = settings.sdr_white_nits;
    impl_->scaler = PreviewScaler{};
    FC_TRY(impl_->scaler.initialize(device, scaler_settings));

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = static_cast<UINT>(geometry.width);
    staging_desc.Height = static_cast<UINT>(geometry.height);
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    impl_->staging.clear();
    impl_->staging.resize(kStagingSlots);
    impl_->free_slots.clear();
    for (int i = 0; i < kStagingSlots; ++i) {
        FC_HR_AS(device->CreateTexture2D(&staging_desc, nullptr, &impl_->staging[static_cast<std::size_t>(i)]),
                 FcError::GPU_TEXTURE_CREATE_FAILED);
        impl_->free_slots.push_back(i);
    }

    {
        const std::lock_guard lock(impl_->stats_mutex);
        impl_->stats = PreviewWriterStats{};
    }
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->exiting = false;
        impl_->pending_slot = -1;
    }

    impl_->done = std::promise<void>{};
    impl_->finished = impl_->done.get_future();
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread([impl = impl_.get()] { impl->preview_loop(); });

    FC_LOG_INFO(Subsystem::Ipc, "preview writer started",
                LogFields{}
                    .add("width", geometry.width)
                    .add("height", geometry.height)
                    .add("fps", settings.fps)
                    .add("stall_ms", settings.injected_publish_stall_ns / 1'000'000));
    return ok();
}

void PreviewWriter::offer(ID3D11Texture2D* source, std::int64_t qpc_ns) {
    if (!impl_->running.load(std::memory_order_acquire) || source == nullptr) {
        return;
    }
    const std::int64_t began_ns = timing::qpc_now_ns();

    {
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.offered;
    }

    // SPEC.md §15.2's 30 fps, decided on the *source* clock. Doing it on a count -- "every
    // other frame" -- would tie the preview's rate to the capture rate, so a recording
    // degraded to 30 fps by §13 rung 3 would preview at 15.
    if (impl_->last_dispatch_ns != 0 && (qpc_ns - impl_->last_dispatch_ns) < impl_->period_ns) {
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.rate_limited;
        return;
    }

    int slot = -1;
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->free_slots.empty()) {
            slot = impl_->free_slots.back();
            impl_->free_slots.pop_back();
        }
    }
    if (slot < 0) {
        // `fc-preview` has all three surfaces. The preview loses this frame; the recording
        // does not learn about it. §15.2's droppability, at the only point it can happen.
        impl_->ring->note_dropped();
        const std::lock_guard lock(impl_->stats_mutex);
        ++impl_->stats.dropped_no_slot;
        return;
    }

    const Result<void> dispatched = impl_->scaler.dispatch(impl_->context.Get(), source);
    if (!dispatched.has_value()) {
        impl_->release_slot(slot);
        impl_->ring->note_dropped();
        // At WARN and not per frame: a source format the preview cannot take is a standing
        // condition, not an event, and §4 bans per-frame logging above TRACE.
        const std::lock_guard lock(impl_->stats_mutex);
        if (impl_->stats.failures == 0) {
            FC_LOG_WARN(Subsystem::Color, "the preview dispatch failed; the recording is unaffected",
                        LogFields{}.add_error(dispatched.error()));
        }
        ++impl_->stats.failures;
        return;
    }

    impl_->context->CopyResource(impl_->staging[static_cast<std::size_t>(slot)].Get(), impl_->scaler.output());
    impl_->last_dispatch_ns = qpc_ns;

    int displaced = -1;
    {
        const std::lock_guard lock(impl_->mutex);
        displaced = impl_->pending_slot;
        impl_->pending_slot = slot;
        impl_->pending_qpc_ns = qpc_ns;
    }
    impl_->wake.notify_one();

    if (displaced >= 0) {
        // Drop-oldest, one slot deep. The newest frame is the one a preview wants, and
        // holding a queue of them would show the user the past.
        impl_->release_slot(displaced);
        impl_->ring->note_dropped();
    }

    const std::int64_t elapsed_ns = timing::qpc_now_ns() - began_ns;
    const std::lock_guard lock(impl_->stats_mutex);
    ++impl_->stats.dispatched;
    if (displaced >= 0) {
        ++impl_->stats.dropped_stale;
    }
    impl_->stats.total_offer_ns += elapsed_ns;
    ++impl_->stats.offer_samples;
    impl_->stats.worst_offer_ns = std::max(impl_->stats.worst_offer_ns, elapsed_ns);
}

void PreviewWriter::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->exiting = true;
    }
    impl_->wake.notify_all();
    static_cast<void>(await_worker(impl_->finished, impl_->thread, "fc-preview", Subsystem::Ipc));

    impl_->staging.clear();
    impl_->scaler = PreviewScaler{};
    impl_->context.Reset();
    impl_->device.Reset();

    const PreviewWriterStats final_stats = stats();
    FC_LOG_INFO(Subsystem::Ipc, "preview writer stopped",
                LogFields{}
                    .add("offered", final_stats.offered)
                    .add("published", final_stats.published)
                    .add("rate_limited", final_stats.rate_limited)
                    .add("dropped_no_slot", final_stats.dropped_no_slot)
                    .add("dropped_stale", final_stats.dropped_stale)
                    .add("dropped_readback_busy", final_stats.dropped_readback_busy)
                    .add("failures", final_stats.failures)
                    .add("offer_worst_us", final_stats.worst_offer_ns / 1000)
                    .add("offer_mean_us", final_stats.mean_offer_ns() / 1000));
}

PreviewWriterStats PreviewWriter::stats() const {
    const std::lock_guard lock(impl_->stats_mutex);
    return impl_->stats;
}

} // namespace fc::preview
