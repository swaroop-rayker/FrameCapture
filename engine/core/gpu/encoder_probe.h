#pragma once

#include "core/gpu/adapter_info.h"

#include <cstdint>
#include <string>

struct IDXGIAdapter1;

namespace fc::gpu {

/// SPEC.md §5.1 step 5: "Probe encode capability per adapter by **actually creating
/// and immediately destroying a throwaway encoder session** at 1920x1080@60. Do not
/// trust vendor ID heuristics or driver version tables -- probe."
///
/// The reason the spec is emphatic: a vendor id tells you which encoder *exists*,
/// not whether it will open. An encoder busy with another application, a driver that
/// advertises NVENC on a card whose session limit is exhausted, and a laptop whose
/// dGPU is powered down all look identical to a heuristic and different to a probe.
///
/// Probing costs roughly 100-200 ms per adapter, which is why the result is cached.
struct ProbeSettings {
    int width = 1920;
    int height = 1080;
    int fps = 60;
};

/// Attempts a real encoder session on `adapter`.
///
/// Never fails as an operation -- an adapter that cannot encode is a fact, not an
/// error. The returned capability carries `probed = true` either way, with `detail`
/// explaining a negative result.
[[nodiscard]] EncoderCapability probe_encoder(IDXGIAdapter1* adapter, std::uint32_t vendor_id,
                                              const ProbeSettings& settings = {});

/// Finds a `D3D11_TEXTURE2D_DESC::BindFlags` value the adapter will accept for an
/// NV12 encoder-input texture array.
///
/// FFmpeg passes `AVD3D11VAFramesContext.BindFlags` through to `CreateTexture2D`
/// untouched and supplies no default, and the accepted value is **not portable
/// between adapters** -- on the Radeon 780M the conventional `RENDER_TARGET` is
/// rejected and only `DECODER` works, which is counter-intuitive for an encoder
/// input pool. See BUG-001 in docs/ENGINEERING_LOG.md.
///
/// Returns 0 when nothing was accepted.
[[nodiscard]] std::uint32_t probe_nv12_pool_bind_flags(void* d3d11_device, int width, int height, unsigned array_size);

} // namespace fc::gpu
