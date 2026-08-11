#include "core/gpu/cross_adapter_probe.h"

#include "core/gpu/adapter_selector.h" // kCrossAdapterBudgetMs, for the log line
#include "core/gpu/d3d_device.h"
#include "core/logging/logger.h"
#include "core/timing/qpc_clock.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace fc::gpu {
namespace {

using Microsoft::WRL::ComPtr;

/// Finds the adapter with this LUID. Returns null when it has gone away, which on a
/// hybrid laptop mid-migration is a real state rather than an error.
[[nodiscard]] ComPtr<IDXGIAdapter1> adapter_by_id(const AdapterId& id) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        const auto luid = (static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                          static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart));
        if (luid == id.value) {
            return adapter;
        }
    }
    return nullptr;
}

[[nodiscard]] CrossAdapterCost unsupported(std::string reason) {
    CrossAdapterCost cost;
    cost.supported = false;
    cost.path = TransferPath::None;
    cost.detail = std::move(reason);
    return cost;
}

/// Turns a set of per-frame timings into the reported figure.
[[nodiscard]] CrossAdapterCost summarise(std::vector<double> samples, TransferPath path) {
    std::ranges::sort(samples);
    CrossAdapterCost cost;
    cost.supported = true;
    cost.path = path;
    cost.iterations = static_cast<int>(samples.size());
    cost.median_ms = samples[samples.size() / 2];
    cost.worst_ms = samples.back();
    // P99 rather than a mean: a transfer that is usually fast and occasionally 8 ms
    // still drops frames at 60 fps, and the mean hides exactly that.
    cost.p99_ms = samples[std::min(samples.size() - 1, (samples.size() * 99) / 100)];
    return cost;
}

/// SPEC.md §5.3's stated fallback: "staged CPU copy (`D3D11_USAGE_STAGING` + `Map`)".
///
/// A full 1080p BGRA round trip through system memory -- read back from the source
/// adapter, upload to the destination. ~8.3 MB each way. This is what the engine has to
/// use when the pair cannot share a texture, so it is the cost rule 2 must actually be
/// judged on for such a pair, and pricing it against the same 2.0 ms budget is the
/// whole point of measuring rather than assuming.
[[nodiscard]] CrossAdapterCost measure_staged(const D3dDevice& src, const D3dDevice& dst, int width, int height,
                                              int iterations) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> source_frame;
    if (FAILED(src.device()->CreateTexture2D(&desc, nullptr, &source_frame))) {
        return unsupported("the source adapter refused the staged-path frame texture");
    }

    D3D11_TEXTURE2D_DESC readback_desc = desc;
    readback_desc.Usage = D3D11_USAGE_STAGING;
    readback_desc.BindFlags = 0;
    readback_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(src.device()->CreateTexture2D(&readback_desc, nullptr, &readback))) {
        return unsupported("the source adapter refused a staging readback texture");
    }

    // The destination side. `DYNAMIC` + `WRITE_DISCARD` is the upload path the engine's
    // own staging ring uses, so the number describes the same work.
    D3D11_TEXTURE2D_DESC upload_desc = desc;
    upload_desc.Usage = D3D11_USAGE_DYNAMIC;
    upload_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Texture2D> upload;
    if (FAILED(dst.device()->CreateTexture2D(&upload_desc, nullptr, &upload))) {
        return unsupported("the destination adapter refused a dynamic upload texture");
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        const std::int64_t started = timing::qpc_now_ns();

        src.context()->CopyResource(readback.Get(), source_frame.Get());
        D3D11_MAPPED_SUBRESOURCE read{};
        if (FAILED(src.context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &read))) {
            return unsupported("mapping the source staging texture failed");
        }

        D3D11_MAPPED_SUBRESOURCE write{};
        if (FAILED(dst.context()->Map(upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &write))) {
            src.context()->Unmap(readback.Get(), 0);
            return unsupported("mapping the destination upload texture failed");
        }

        const auto row_bytes = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(write.pData) + (static_cast<std::size_t>(y) * write.RowPitch),
                        static_cast<const std::uint8_t*>(read.pData) + (static_cast<std::size_t>(y) * read.RowPitch),
                        row_bytes);
        }

        dst.context()->Unmap(upload.Get(), 0);
        src.context()->Unmap(readback.Get(), 0);

        samples.push_back(static_cast<double>(timing::qpc_now_ns() - started) / 1'000'000.0);
    }

    return summarise(std::move(samples), TransferPath::StagedSystemMemory);
}

} // namespace

std::string_view to_string(TransferPath value) noexcept {
    switch (value) {
    case TransferPath::None:
        return "none";
    case TransferPath::SharedTexture:
        return "shared_texture";
    case TransferPath::StagedSystemMemory:
        return "staged_system_memory";
    }
    return "unknown";
}

CrossAdapterCost measure_cross_adapter_cost(const AdapterId& source, const AdapterId& destination, int width,
                                            int height, int iterations) {
    if (source == destination) {
        return unsupported("source and destination are the same adapter");
    }
    if (width <= 0 || height <= 0 || iterations <= 0) {
        return unsupported("invalid probe dimensions");
    }

    const ComPtr<IDXGIAdapter1> source_adapter = adapter_by_id(source);
    const ComPtr<IDXGIAdapter1> destination_adapter = adapter_by_id(destination);
    if (source_adapter.Get() == nullptr || destination_adapter.Get() == nullptr) {
        return unsupported("one of the adapters is no longer present");
    }

    const Result<D3dDevice> source_device = create_device_on_adapter(source_adapter.Get());
    const Result<D3dDevice> destination_device = create_device_on_adapter(destination_adapter.Get());
    if (!source_device.has_value() || !destination_device.has_value()) {
        return unsupported("device creation failed on one of the adapters");
    }
    const D3dDevice& src = source_device.value();
    const D3dDevice& dst = destination_device.value();

    // SPEC.md §5.3's constraints, every one of which is validated here rather than
    // assumed: no mips, no MSAA, `USAGE_DEFAULT`, single subresource, and a format the
    // pair will actually share. A driver that refuses any of them refuses at
    // `CreateTexture2D`, and §5.3 says to fall back rather than crash.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // §6: never the _SRGB variant
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> shared;
    if (FAILED(src.device()->CreateTexture2D(&desc, nullptr, &shared))) {
        return measure_staged(src, dst, width, height, iterations); // §5.3's fallback
    }

    ComPtr<IDXGIResource1> resource;
    if (FAILED(shared.As(&resource))) {
        return measure_staged(src, dst, width, height, iterations);
    }
    HANDLE handle = nullptr;
    if (FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                            &handle)) ||
        handle == nullptr) {
        return unsupported("CreateSharedHandle failed");
    }

    ComPtr<ID3D11Device1> destination_device1;
    if (FAILED(dst.device()->QueryInterface(IID_PPV_ARGS(&destination_device1)))) {
        CloseHandle(handle);
        return measure_staged(src, dst, width, height, iterations);
    }

    ComPtr<ID3D11Texture2D> opened;
    const HRESULT opened_hr = destination_device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle); // the opened resource holds its own reference
    if (FAILED(opened_hr)) {
        return measure_staged(src, dst, width, height, iterations);
    }

    ComPtr<IDXGIKeyedMutex> source_mutex;
    ComPtr<IDXGIKeyedMutex> destination_mutex;
    if (FAILED(shared.As(&source_mutex)) || FAILED(opened.As(&destination_mutex))) {
        return measure_staged(src, dst, width, height, iterations);
    }

    // A private destination the copy lands in, so the timing covers a real
    // adapter-to-adapter move rather than a no-op onto the shared surface itself.
    D3D11_TEXTURE2D_DESC local = desc;
    local.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> landed;
    if (FAILED(dst.device()->CreateTexture2D(&local, nullptr, &landed))) {
        return measure_staged(src, dst, width, height, iterations);
    }

    // A staging texture on the destination, mapped after each copy. This is what makes
    // the timing honest: `CopyResource` only *queues* work, and a timer around the
    // queueing call reports sub-microsecond transfers on any adapter pair -- which
    // would make rule 2 fire everywhere and is precisely the "always use the fast GPU"
    // mistake §5.2 exists to prevent. Mapping forces the copy to have completed.
    D3D11_TEXTURE2D_DESC staging = local;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(dst.device()->CreateTexture2D(&staging, nullptr, &readback))) {
        return measure_staged(src, dst, width, height, iterations);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        const std::int64_t started = timing::qpc_now_ns();

        // Producer side: the capture adapter publishes the frame.
        if (FAILED(source_mutex->AcquireSync(0, 1000))) {
            return unsupported("the source keyed mutex timed out");
        }
        source_mutex->ReleaseSync(1);

        // Consumer side: the encode adapter takes it and copies it into its own memory.
        if (FAILED(destination_mutex->AcquireSync(1, 1000))) {
            return unsupported("the destination keyed mutex timed out");
        }
        dst.context()->CopyResource(landed.Get(), opened.Get());
        destination_mutex->ReleaseSync(0);

        // Force completion. Without this the loop measures command submission.
        dst.context()->CopyResource(readback.Get(), landed.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(dst.context()->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return unsupported("mapping the destination staging texture failed");
        }
        dst.context()->Unmap(readback.Get(), 0);

        samples.push_back(static_cast<double>(timing::qpc_now_ns() - started) / 1'000'000.0);
    }

    const CrossAdapterCost cost = summarise(std::move(samples), TransferPath::SharedTexture);

    FC_LOG_INFO(Subsystem::Gpu, "cross-adapter transfer measured",
                LogFields{}
                    .add("from", source.to_string())
                    .add("to", destination.to_string())
                    .add("p99_ms", cost.p99_ms)
                    .add("median_ms", cost.median_ms)
                    .add("worst_ms", cost.worst_ms)
                    .add("budget_ms", kCrossAdapterBudgetMs)
                    .add("path", to_string(cost.path))
                    .add("iterations", cost.iterations));
    return cost;
}

} // namespace fc::gpu
