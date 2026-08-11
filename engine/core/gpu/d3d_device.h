#pragma once

#include "core/error/result.h"
#include "core/gpu/adapter_info.h"

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIAdapter1;

namespace fc::gpu {

/// A D3D11 device bound to one specific adapter.
///
/// The binding is the entire point. On a MUX-less laptop the panel is wired to the
/// integrated adapter, and a device created on the *wrong* adapter will make
/// `DuplicateOutput` return `S_OK` and then hand back nothing but black frames
/// (SPEC.md §5.1, §20 row 1). There is no API that tells you this went wrong.
class D3dDevice {
public:
    D3dDevice();
    ~D3dDevice();

    D3dDevice(const D3dDevice&) = delete;
    D3dDevice& operator=(const D3dDevice&) = delete;
    D3dDevice(D3dDevice&&) noexcept;
    D3dDevice& operator=(D3dDevice&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    /// Raw interfaces, for the capture/convert/encode layers. Non-owning.
    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;

    /// The adapter this device was actually created on -- read back from the device
    /// rather than remembered from the request, so a mismatch is impossible to miss.
    [[nodiscard]] AdapterId adapter_id() const noexcept;

    [[nodiscard]] std::uint32_t feature_level() const noexcept;

private:
    friend Result<D3dDevice> create_device_on_adapter(IDXGIAdapter1* adapter);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Creates a device on `adapter`.
///
/// Uses `D3D_DRIVER_TYPE_UNKNOWN`, which is mandatory when an explicit adapter is
/// supplied -- passing `HARDWARE` with a non-null adapter fails with `E_INVALIDARG`
/// and is a common cause of `GPU_DEVICE_CREATE_FAILED`.
///
/// Requests `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` (needed for the encoder paths) and
/// `BGRA_SUPPORT` (needed for WGC/DDA surfaces), and enables multithread protection
/// because FFmpeg drives the immediate context from its own threads.
[[nodiscard]] Result<D3dDevice> create_device_on_adapter(IDXGIAdapter1* adapter);

} // namespace fc::gpu
