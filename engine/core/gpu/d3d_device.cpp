#include "core/gpu/d3d_device.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace fc::gpu {
namespace {

using Microsoft::WRL::ComPtr;

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

struct D3dDevice::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    AdapterId adapter_id;
    std::uint32_t feature_level = 0;
};

D3dDevice::D3dDevice() : impl_(std::make_unique<Impl>()) {}

D3dDevice::~D3dDevice() = default;
D3dDevice::D3dDevice(D3dDevice&&) noexcept = default;
D3dDevice& D3dDevice::operator=(D3dDevice&&) noexcept = default;

bool D3dDevice::valid() const noexcept {
    return impl_ != nullptr && impl_->device != nullptr;
}

ID3D11Device* D3dDevice::device() const noexcept {
    return impl_ != nullptr ? impl_->device.Get() : nullptr;
}

ID3D11DeviceContext* D3dDevice::context() const noexcept {
    return impl_ != nullptr ? impl_->context.Get() : nullptr;
}

AdapterId D3dDevice::adapter_id() const noexcept {
    return impl_ != nullptr ? impl_->adapter_id : AdapterId{};
}

std::uint32_t D3dDevice::feature_level() const noexcept {
    return impl_ != nullptr ? impl_->feature_level : 0;
}

Result<D3dDevice> create_device_on_adapter(IDXGIAdapter1* adapter) {
    if (adapter == nullptr) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    D3dDevice result;

    static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // Seeded rather than value-initialised: D3D_FEATURE_LEVEL has no zero
    // enumerator, and D3D11CreateDevice overwrites this on success anyway.
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    // DRIVER_TYPE_UNKNOWN is required with an explicit adapter.
    // FC_HR_AS is not used here because the failure path logs the adapter
    // description, which needs the desc read first.
    // FC_LINT_OK: the result is checked on the line after this call.
    const HResult hr = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION, &result.impl_->device, &obtained, &result.impl_->context);
    if (hr_failed(hr)) {
        DXGI_ADAPTER_DESC1 desc{};
        static_cast<void>(adapter->GetDesc1(&desc));
        FC_LOG_ERROR(Subsystem::Gpu, "D3D11 device creation failed",
                     LogFields{}
                         .add("adapter", narrow(desc.Description))
                         .add("vendor_id", static_cast<std::uint64_t>(desc.VendorId))
                         .add("device_id", static_cast<std::uint64_t>(desc.DeviceId))
                         .add("hr", hresult_message(hr))
                         .add_error(FcError::GPU_DEVICE_CREATE_FAILED));
        return FcError::GPU_DEVICE_CREATE_FAILED;
    }

    result.impl_->feature_level = static_cast<std::uint32_t>(obtained);

    // FFmpeg's D3D11VA context drives the immediate context from its own threads,
    // and so does the capture pipeline. Without this, concurrent use is undefined.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(result.impl_->device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    // Read the adapter identity back off the device rather than trusting the one we
    // asked for. If these ever disagree, everything downstream is wrong and we want
    // to know here, not from a black recording.
    ComPtr<IDXGIDevice> dxgi_device;
    if (SUCCEEDED(result.impl_->device.As(&dxgi_device))) {
        ComPtr<IDXGIAdapter> actual;
        if (SUCCEEDED(dxgi_device->GetAdapter(&actual))) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(actual->GetDesc(&desc))) {
                result.impl_->adapter_id =
                    AdapterId{(static_cast<std::int64_t>(desc.AdapterLuid.HighPart) << 32) |
                              static_cast<std::int64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.LowPart))};
            }
        }
    }

    return result;
}

} // namespace fc::gpu
