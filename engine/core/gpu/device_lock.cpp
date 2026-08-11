#include "core/gpu/device_lock.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11_4.h>

namespace fc::gpu {

ScopedDeviceLock::ScopedDeviceLock(ID3D11Multithread* multithread) noexcept : multithread_(multithread) {
    if (multithread_ != nullptr) {
        multithread_->Enter();
    }
}

ScopedDeviceLock::~ScopedDeviceLock() {
    if (multithread_ != nullptr) {
        multithread_->Leave();
    }
}

ID3D11Multithread* acquire_multithread(ID3D11DeviceContext* context) noexcept {
    if (context == nullptr) {
        return nullptr;
    }
    ID3D11Multithread* multithread = nullptr;
    // FC_LINT_OK: a device that does not offer the interface is handled by returning null,
    // which every caller treats as "nothing to serialise against". There is no error to
    // report and nothing to fail.
    if (FAILED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        return nullptr;
    }
    multithread->SetMultithreadProtected(TRUE);
    return multithread;
}

} // namespace fc::gpu
