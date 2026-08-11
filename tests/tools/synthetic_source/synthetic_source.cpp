#include "synthetic_source.h"

#include "core/timing/qpc_clock.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace fc::test {
namespace {

using Microsoft::WRL::ComPtr;

/// 75% amplitude, the standard for SMPTE ECR 1-1978 bars.
constexpr std::uint8_t kBarLevel = 191;

constexpr std::array kBars{
    BarColour{kBarLevel, kBarLevel, kBarLevel, "white"},
    BarColour{0, kBarLevel, kBarLevel, "yellow"},
    BarColour{kBarLevel, kBarLevel, 0, "cyan"},
    BarColour{0, kBarLevel, 0, "green"},
    BarColour{kBarLevel, 0, kBarLevel, "magenta"},
    BarColour{0, 0, kBarLevel, "red"},
    BarColour{kBarLevel, 0, 0, "blue"},
};

/// Limited-range luma midpoint, used to threshold barcode cells.
constexpr std::uint8_t kLumaThreshold = 126;

struct BgraPixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 255;
};

void write_pixel(std::vector<std::uint8_t>& out, int width, int x, int y, BgraPixel pixel) {
    const std::size_t offset =
        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)) * 4;
    out[offset + 0] = pixel.b;
    out[offset + 1] = pixel.g;
    out[offset + 2] = pixel.r;
    out[offset + 3] = pixel.a;
}

/// Memoises `render_bgra` across `SyntheticSource` instances.
///
/// The pattern is a pure function of (width, height, index, flash), so caching it is
/// sound. It exists because `start` is on SPEC.md §5.4 step 8's path: a session rebuild
/// constructs a fresh source on the new device inside a 350 ms budget, and rendering
/// eight 1080p patterns costs 458-501 ms on an unoptimised build (BUG-032). Cached, the
/// first `start` in the process pays for the fill and every rebuild after it pays only
/// a texture create and a copy.
///
/// Bounded, because the entries are 8 MB apiece at 1080p and an unbounded one would be
/// a slow leak in a long test binary. Eviction is oldest-first and the working set here
/// is a handful of frames, so it never thrashes in practice.
class PatternCache {
public:
    [[nodiscard]] static const std::vector<std::uint8_t>& get(int width, int height, std::uint32_t index, bool flash) {
        static PatternCache cache;
        return cache.lookup(width, height, index, flash);
    }

private:
    struct Key {
        int width;
        int height;
        std::uint32_t index;
        bool flash;

        [[nodiscard]] bool operator==(const Key& other) const noexcept = default;
    };

    struct Entry {
        Key key;
        std::vector<std::uint8_t> pixels;
    };

    /// 16 entries is 132 MB at 1080p, which is the ceiling this is chosen against.
    static constexpr std::size_t kMaxEntries = 16;

    [[nodiscard]] const std::vector<std::uint8_t>& lookup(int width, int height, std::uint32_t index, bool flash) {
        const Key key{width, height, index, flash};
        const std::lock_guard lock(mutex_);
        for (const Entry& entry : entries_) {
            if (entry.key == key) {
                return entry.pixels;
            }
        }
        if (entries_.size() >= kMaxEntries) {
            entries_.pop_front();
        }
        entries_.push_back(Entry{key, render_bgra(width, height, index, flash)});
        return entries_.back().pixels;
    }

    std::mutex mutex_;
    std::deque<Entry> entries_;
};

} // namespace

std::span<const BarColour> smpte_bars() noexcept {
    return {kBars.data(), kBars.size()};
}

std::vector<std::uint8_t> render_bgra(int width, int height, std::uint32_t index, bool flash) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    if (width <= 0 || height <= 0) {
        return out;
    }

    if (flash) {
        // SPEC.md §20 row 4's marker frame. Full white below the barcode, and
        // nothing else -- the moving bar would drag the mean luma down by a
        // frame-dependent amount and turn a fixed threshold into a tuned one.
        for (int y = bars_top(); y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                write_pixel(out, width, x, y, BgraPixel{255, 255, 255, 255});
            }
        }
    } else {
        // --- SMPTE bars, filling everything below the barcode -----------------
        const int bar_width = std::max(1, width / static_cast<int>(kBars.size()));
        for (int y = bars_top(); y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto bar = std::min<std::size_t>(static_cast<std::size_t>(x / bar_width), kBars.size() - 1);
                write_pixel(out, width, x, y, BgraPixel{kBars[bar].b, kBars[bar].g, kBars[bar].r, 255});
            }
        }

        // --- Moving bar -------------------------------------------------------
        //
        // Without motion, consecutive frames are identical and a pipeline that is
        // stuck on frame 0 looks exactly like one that is working.
        const int bar_x = static_cast<int>((index * 13u) % static_cast<std::uint32_t>(std::max(1, width)));
        for (int y = bars_top(); y < height; ++y) {
            for (int offset = 0; offset < 24; ++offset) {
                const int x = (bar_x + offset) % width;
                write_pixel(out, width, x, y, BgraPixel{0, 0, 0, 255});
            }
        }
    }

    // --- Frame-index barcode, on top of everything ---------------------------
    //
    // Pure black and white so it survives 4:2:0 chroma subsampling and limited
    // range: it is carried entirely by luma.
    for (int bit = 0; bit < kBarcodeBits; ++bit) {
        const bool set = ((index >> (kBarcodeBits - 1 - bit)) & 1u) != 0u;
        const std::uint8_t level = set ? 255 : 0;
        const int x0 = bit * kBarcodeCell;
        for (int y = 0; y < kBarcodeCell && y < height; ++y) {
            for (int x = x0; x < x0 + kBarcodeCell && x < width; ++x) {
                write_pixel(out, width, x, y, BgraPixel{level, level, level, 255});
            }
        }
    }

    return out;
}

double mean_luma_below_barcode(std::span<const std::uint8_t> luma, int width, int height) {
    if (width <= 0 || height <= bars_top()) {
        return 0.0;
    }
    if (luma.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return 0.0;
    }

    std::uint64_t sum = 0;
    for (int y = bars_top(); y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            sum += luma[row + static_cast<std::size_t>(x)];
        }
    }
    const auto count = static_cast<double>(width) * static_cast<double>(height - bars_top());
    return static_cast<double>(sum) / count;
}

std::optional<std::uint32_t> decode_barcode_from_luma(std::span<const std::uint8_t> luma, int width, int height) {
    if (width < kMinimumWidth || height < kBarcodeCell) {
        return std::nullopt;
    }
    if (luma.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return std::nullopt;
    }

    std::uint32_t index = 0;
    for (int bit = 0; bit < kBarcodeBits; ++bit) {
        // Sample the centre of the cell, away from any chroma or scaling edge
        // artefacts at its border.
        const int x = (bit * kBarcodeCell) + (kBarcodeCell / 2);
        const int y = kBarcodeCell / 2;
        const std::uint8_t sample =
            luma[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)];

        // A clean cell is near limited-range black (16) or white (235). Anything
        // in the middle means the frame is torn, blended, or not ours.
        if (sample > 60 && sample < 190) {
            return std::nullopt;
        }

        index = (index << 1) | (sample >= kLumaThreshold ? 1u : 0u);
    }
    return index;
}

struct SyntheticSource::Impl {
    Settings settings;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    /// Pre-rendered frames, when `Settings::prerendered_frames` asks for them. Empty
    /// otherwise, and then `texture` is rewritten per acquire as before.
    std::vector<ComPtr<ID3D11Texture2D>> prerendered;
    std::atomic<bool> running{false};
    std::uint32_t next_index = 0;
    std::uint64_t epoch_ns = 0;
    /// Wall-clock origin for `pace_to_real_time`. Distinct from `epoch_ns`, which is
    /// the *synthetic* origin the frame timestamps are built on.
    std::int64_t start_ns = 0;

    /// Fills `target` with frame `index`'s pattern. Shared by the per-acquire path
    /// and the pre-render loop so the two cannot draw different pictures.
    [[nodiscard]] Result<void> paint(ID3D11Texture2D* target, std::uint32_t index) const {
        const bool flash = settings.flash_interval_frames != 0 && (index % settings.flash_interval_frames) == 0;
        const std::vector<std::uint8_t>& pixels = PatternCache::get(settings.width, settings.height, index, flash);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(target, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return FcError::GPU_TEXTURE_CREATE_FAILED;
        }
        const auto row_bytes = static_cast<std::size_t>(settings.width) * 4;
        for (int y = 0; y < settings.height; ++y) {
            std::copy_n(pixels.data() + (static_cast<std::size_t>(y) * row_bytes), row_bytes,
                        static_cast<std::uint8_t*>(mapped.pData) + (static_cast<std::size_t>(y) * mapped.RowPitch));
        }
        context->Unmap(target, 0);
        return ok();
    }
};

SyntheticSource::SyntheticSource() : impl_(std::make_unique<Impl>()) {}

SyntheticSource::~SyntheticSource() {
    stop();
}

Result<void> SyntheticSource::configure(const Settings& settings) {
    if (settings.width < kMinimumWidth || settings.height <= bars_top() || (settings.width % 2) != 0 ||
        (settings.height % 2) != 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    impl_->settings = settings;
    return ok();
}

Result<void> SyntheticSource::start(ID3D11Device* device, const capture::CaptureTarget& /*target*/) {
    if (device == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->device = device;
    device->GetImmediateContext(&impl_->context);

    // A single DYNAMIC texture, rewritten per frame. The real backends hand out
    // pool textures; a test source does not need the pool, only the contract.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(impl_->settings.width);
    desc.Height = static_cast<UINT>(impl_->settings.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &impl_->texture))) {
        return FcError::GPU_TEXTURE_CREATE_FAILED;
    }

    // Through the shared conversion, not a local one. Multiplying the raw counter
    // by 1e9 before dividing overflows a 64-bit integer after about fifteen minutes
    // of uptime at a 10 MHz timer, and the wrapped result is a *consistent* garbage
    // epoch -- which every video-only test tolerates, because it measures
    // everything relative to it, and which disagrees with WASAPI's timestamps the
    // moment a recording has both streams (BUG-019).
    impl_->epoch_ns = static_cast<std::uint64_t>(timing::qpc_now_ns());

    // Pre-render, when asked, so `acquire` becomes an AddRef and a timestamp -- see
    // `Settings::prerendered_frames` for why that matters.
    //
    // Filled here rather than on first use, because filling on demand puts the cost
    // inside the capture loop instead: measured, that turned one fault into two
    // rebuilds, because the paints that followed a rebuild read as a fresh stall. The
    // cost is kept off SPEC.md §5.4's 350 ms rebuild budget by `PatternCache` instead,
    // which is where it belongs -- the pixels are identical on every device. BUG-032.
    impl_->prerendered.clear();
    if (impl_->settings.prerendered_frames > 0) {
        impl_->prerendered.reserve(impl_->settings.prerendered_frames);
        for (std::uint32_t i = 0; i < impl_->settings.prerendered_frames; ++i) {
            ComPtr<ID3D11Texture2D> frame;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &frame))) {
                return FcError::GPU_TEXTURE_CREATE_FAILED;
            }
            FC_TRY(impl_->paint(frame.Get(), i));
            impl_->prerendered.push_back(std::move(frame));
        }
    }

    impl_->next_index = 0;
    impl_->start_ns = timing::qpc_now_ns();
    impl_->running.store(true, std::memory_order_release);
    return ok();
}

void SyntheticSource::stop() {
    impl_->running.store(false, std::memory_order_release);
    impl_->prerendered.clear();
    impl_->texture.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
}

bool SyntheticSource::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

Result<capture::CaptureFrame> SyntheticSource::acquire(std::chrono::milliseconds /*timeout*/) {
    if (!impl_->running.load(std::memory_order_acquire)) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (impl_->settings.frame_limit != 0 && impl_->next_index >= impl_->settings.frame_limit) {
        return FcError::CAPTURE_FRAME_TIMEOUT;
    }

    const std::uint32_t index = impl_->next_index++;

    if (impl_->settings.pace_to_real_time) {
        // Wait until this frame is due, so the caller sees `fps` in wall clock. Real
        // backends block here for the same reason; a source that never does turns a
        // capture loop into an unbounded producer.
        const std::int64_t due = impl_->start_ns + ((static_cast<std::int64_t>(index) * 1'000'000'000) /
                                                    static_cast<std::int64_t>(std::max(1, impl_->settings.fps)));
        for (;;) {
            const std::int64_t remaining = due - timing::qpc_now_ns();
            if (remaining <= 0 || !impl_->running.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds{std::min<std::int64_t>(remaining, 2'000'000)});
        }
    }

    ID3D11Texture2D* surface = nullptr;
    if (impl_->prerendered.empty()) {
        FC_TRY(impl_->paint(impl_->texture.Get(), index));
        surface = impl_->texture.Get();
    } else {
        // Cycle. The picture repeats every `prerendered_frames`; `sequence` and the
        // timestamp below do not, so pacing and ordering are unaffected.
        surface = impl_->prerendered[index % impl_->prerendered.size()].Get();
    }

    capture::CaptureFrame frame;
    surface->AddRef(); // released by release()
    frame.texture = surface;
    // Synthetic but monotonic and evenly spaced, so a pacer sees a clean timeline.
    frame.qpc_ns = impl_->epoch_ns + ((static_cast<std::uint64_t>(index) * 1'000'000'000ull) /
                                      static_cast<std::uint64_t>(std::max(1, impl_->settings.fps)));
    frame.sequence = index;
    frame.dxgi_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    frame.content = capture::ContentRect{0, 0, impl_->settings.width, impl_->settings.height};
    return frame;
}

void SyntheticSource::release(const capture::CaptureFrame& frame) {
    if (frame.texture != nullptr) {
        frame.texture->Release();
    }
}

} // namespace fc::test
