#pragma once

// Creating a D3D11 device on a *named* adapter, for GPU-tier tests.
//
// fc_core exposes create_device_on_adapter(IDXGIAdapter1*), because production
// code reaches an adapter by walking the topology and already holds the DXGI
// interface. Tests instead hold an AdapterId -- the packed LUID the topology
// reports -- and have to find the adapter again. This is that lookup, in one
// place rather than copied into every GPU test that needs it.

#include "core/error/fc_error.h"
#include "core/gpu/adapter_info.h"
#include "core/gpu/d3d_device.h"

#include <windows.h>
// Must follow windows.h.
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace fc::test {

/// The device on the adapter identified by `id`, or `GPU_NO_SUITABLE_ADAPTER` if
/// that adapter is no longer present.
[[nodiscard]] inline Result<gpu::D3dDevice> device_for_adapter(gpu::AdapterId id) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return FcError::GPU_ADAPTER_ENUMERATION_FAILED;
    }

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        // Must match how the topology packs a LUID, including the unsigned cast on
        // the low part -- sign-extending it produces an id that never compares equal.
        const gpu::AdapterId current{(static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                                     static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart))};
        if (current == id) {
            return gpu::create_device_on_adapter(adapter.Get());
        }
    }
    return FcError::GPU_NO_SUITABLE_ADAPTER;
}

} // namespace fc::test
