// FrameCapture spike: does h264_amf's FFmpeg wrapper round-trip through host
// memory on this driver? (SPEC.md §2.2 item 1)
//
// THROWAWAY DIAGNOSTIC CODE. Not part of fc_core, not part of the test suite.
// It is deliberately a single translation unit with a linear main(), because
// the value here is "one file you can read top to bottom and trust", not
// reusability. Do not lift code out of this file into the engine.
//
// See README.md in this directory for how to interpret the output.

// d3d11.h pulls in d3d10_1.h -> d3d10.h, which is where ID3D10Multithread
// lives. Including <d3d10.h> directly trips the SDK's include-order guard.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kDefaultFrames = 1000;
constexpr int kWarmupFrames = 30;
constexpr int kPatternCount = 64; // distinct source frames, cycled
constexpr int kPoolSize = 20;
constexpr UINT kAmdVendorId = 0x1002;

// ---------------------------------------------------------------------------
// Error plumbing. A spike still has to fail loudly -- a benchmark that silently
// falls back to a software path produces a confident, wrong answer.
// ---------------------------------------------------------------------------

bool g_failed = false;

bool fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "FATAL: %s failed, hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
    g_failed = true;
    return false;
}

bool fail_av(const char* what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    std::fprintf(stderr, "FATAL: %s failed: %s (%d)\n", what, buf, err);
    g_failed = true;
    return false;
}

#define SPIKE_HR(expr)                                                                                                 \
    do {                                                                                                               \
        const HRESULT _hr = (expr);                                                                                    \
        if (FAILED(_hr)) {                                                                                             \
            return fail(#expr, _hr);                                                                                   \
        }                                                                                                              \
    } while (0)

#define SPIKE_AV(expr)                                                                                                 \
    do {                                                                                                               \
        const int _err = (expr);                                                                                       \
        if (_err < 0) {                                                                                                \
            return fail_av(#expr, _err);                                                                               \
        }                                                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// Question (1): is av_hwframe_transfer_data ever invoked on the encode path?
//
// The AMF wrapper lives in avcodec-*.dll and calls into avutil-*.dll through
// its import address table. Rewriting that IAT entry counts every call that
// crosses the DLL boundary, which is exactly the host round-trip we are hunting.
//
// Limitation, stated up front: this detects only calls that go through
// av_hwframe_transfer_data. It cannot see a copy performed inside the AMF
// runtime or a direct ID3D11DeviceContext::CopyResource. A zero here is
// necessary but not sufficient -- the per-frame timing is the backstop.
// ---------------------------------------------------------------------------

using HwTransferFn = int (*)(AVFrame*, const AVFrame*, int);

std::atomic<long> g_transfer_calls{0};
HwTransferFn g_real_transfer = nullptr;

// Modules whose IAT we rewrote, and how many entries in each.
std::vector<std::pair<std::string, int>> g_patch_sites;

// True when avutil exports the symbol at all. If it does not, a zero call count
// means nothing and the whole measurement is void.
bool g_symbol_exported = false;

int hooked_hwframe_transfer_data(AVFrame* dst, const AVFrame* src, int flags) {
    g_transfer_calls.fetch_add(1, std::memory_order_relaxed);
    return g_real_transfer(dst, src, flags);
}

int patch_module_iat(HMODULE module, const char* symbol, void* replacement, void** out_original) {
    auto* const base = reinterpret_cast<std::uint8_t*>(module);
    auto* const dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return 0;
    }

    int patched = 0;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name != 0; ++desc) {
        if (desc->OriginalFirstThunk == 0 || desc->FirstThunk == 0) {
            continue;
        }

        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* addr_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++addr_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }

            auto* const import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(import->Name, symbol) != 0) {
                continue;
            }

            DWORD old_protect = 0;
            if (VirtualProtect(&addr_thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protect) == 0) {
                continue;
            }
            if (*out_original == nullptr) {
                *out_original = reinterpret_cast<void*>(addr_thunk->u1.Function);
            }
            addr_thunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&addr_thunk->u1.Function, sizeof(void*), old_protect, &old_protect);
            ++patched;
        }
    }
    return patched;
}

void install_transfer_hook() {
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed) == 0) {
        std::fprintf(stderr, "WARNING: EnumProcessModules failed; hook not installed.\n");
        return;
    }

    void* original = nullptr;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, sizeof(name)) == 0) {
            continue;
        }

        // Does avutil actually export the symbol? Without this check a zero
        // call count is indistinguishable from a broken experiment.
        if (_strnicmp(name, "avutil-", 7) == 0 && GetProcAddress(modules[i], "av_hwframe_transfer_data") != nullptr) {
            g_symbol_exported = true;
            if (original == nullptr) {
                original = reinterpret_cast<void*>(GetProcAddress(modules[i], "av_hwframe_transfer_data"));
            }
        }

        const int n = patch_module_iat(modules[i], "av_hwframe_transfer_data",
                                       reinterpret_cast<void*>(&hooked_hwframe_transfer_data), &original);
        if (n > 0) {
            g_patch_sites.emplace_back(name, n);
        }
    }

    g_real_transfer = reinterpret_cast<HwTransferFn>(original);
}

// ---------------------------------------------------------------------------
// Clock. QPC only (CLAUDE.md §4 / SPEC.md §7.1) -- even in throwaway code,
// because a benchmark on the wrong clock is worse than no benchmark.
// ---------------------------------------------------------------------------

std::int64_t qpc_frequency() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

std::int64_t qpc_now() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---------------------------------------------------------------------------
// Adapter selection.
// ---------------------------------------------------------------------------

std::string to_utf8(const WCHAR* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct AdapterInfo {
    ComPtr<IDXGIAdapter1> adapter;
    std::string description;
    UINT vendor_id = 0;
    SIZE_T dedicated_video_memory = 0;
};

const char* vendor_name(UINT id) {
    switch (id) {
    case 0x1002:
        return "AMD";
    case 0x10DE:
        return "NVIDIA";
    case 0x8086:
        return "Intel";
    case 0x1414:
        return "Microsoft";
    default:
        return "unknown";
    }
}

bool enumerate_adapters(std::vector<AdapterInfo>& out) {
    ComPtr<IDXGIFactory1> factory;
    SPIKE_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        SPIKE_HR(adapter->GetDesc1(&desc));
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue; // WARP tells us nothing about AMF.
        }
        out.push_back({adapter, to_utf8(desc.Description), desc.VendorId, desc.DedicatedVideoMemory});
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Synthetic NV12 pattern. Real motion, so the encoder has real residual to
// chew on -- a static image would flatter the numbers badly.
// ---------------------------------------------------------------------------

void fill_nv12_pattern(std::uint8_t* base, UINT row_pitch, int frame_index) {
    const int bar_x = (frame_index * 29) % kWidth;

    for (int y = 0; y < kHeight; ++y) {
        std::uint8_t* row = base + static_cast<std::size_t>(y) * row_pitch;
        for (int x = 0; x < kWidth; ++x) {
            int luma = 16 + ((x / 8 + y / 8 + frame_index * 3) % 200);
            const int dx = x - bar_x;
            if (dx >= 0 && dx < 96) {
                luma = 235; // moving white bar
            }
            row[x] = static_cast<std::uint8_t>(std::clamp(luma, 16, 235));
        }
    }

    // NV12: the interleaved chroma plane follows the luma plane, same pitch.
    std::uint8_t* chroma = base + static_cast<std::size_t>(row_pitch) * kHeight;
    for (int y = 0; y < kHeight / 2; ++y) {
        std::uint8_t* row = chroma + static_cast<std::size_t>(y) * row_pitch;
        for (int x = 0; x < kWidth / 2; ++x) {
            const int u = 128 + ((y / 4 + frame_index) % 64) - 32;
            const int v = 128 + ((x / 4 - frame_index) % 64) - 32;
            row[2 * x + 0] = static_cast<std::uint8_t>(std::clamp(u, 16, 240));
            row[2 * x + 1] = static_cast<std::uint8_t>(std::clamp(v, 16, 240));
        }
    }
}

// ---------------------------------------------------------------------------
// FFmpeg leaves the encoder-input pool's BindFlags up to the caller and its
// default (D3D11_BIND_DECODER) is rejected for NV12 arrays on this part. Rather
// than hard-code a guess, ask the driver what it will accept and print the
// answer -- M2/M3 will need this same matrix.
// ---------------------------------------------------------------------------

struct BindFlagCandidate {
    UINT flags;
    const char* name;
};

constexpr BindFlagCandidate kBindCandidates[] = {
    {D3D11_BIND_RENDER_TARGET, "RENDER_TARGET"},
    {D3D11_BIND_SHADER_RESOURCE, "SHADER_RESOURCE"},
    {D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "SHADER_RESOURCE|RENDER_TARGET"},
    {D3D11_BIND_DECODER, "DECODER"},
    {0, "none"},
};

UINT probe_pool_bind_flags(ID3D11Device* device, UINT array_size) {
    std::printf("NV12 pool BindFlags probe (%dx%d, ArraySize=%u)\n", kWidth, kHeight, array_size);

    UINT chosen = 0;
    bool have_choice = false;

    for (const BindFlagCandidate& candidate : kBindCandidates) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = array_size;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = candidate.flags;

        ComPtr<ID3D11Texture2D> probe;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &probe);
        std::printf("  %-32s %s", candidate.name, SUCCEEDED(hr) ? "ok" : "rejected");
        if (FAILED(hr)) {
            std::printf(" (0x%08lX)", static_cast<unsigned long>(hr));
        }
        std::printf("\n");

        if (SUCCEEDED(hr) && !have_choice) {
            chosen = candidate.flags;
            have_choice = true;
        }
    }
    std::printf("\n");
    return chosen;
}

bool build_pattern_textures(ID3D11Device* device, ID3D11DeviceContext* context,
                            std::vector<ComPtr<ID3D11Texture2D>>& out) {
    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = kWidth;
    staging_desc.Height = kHeight;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_NV12;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Texture2D> staging;
    SPIKE_HR(device->CreateTexture2D(&staging_desc, nullptr, &staging));

    D3D11_TEXTURE2D_DESC gpu_desc = staging_desc;
    gpu_desc.Usage = D3D11_USAGE_DEFAULT;
    gpu_desc.CPUAccessFlags = 0;
    gpu_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    out.reserve(kPatternCount);
    for (int i = 0; i < kPatternCount; ++i) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        SPIKE_HR(context->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped));
        fill_nv12_pattern(static_cast<std::uint8_t*>(mapped.pData), mapped.RowPitch, i);
        context->Unmap(staging.Get(), 0);

        ComPtr<ID3D11Texture2D> gpu;
        SPIKE_HR(device->CreateTexture2D(&gpu_desc, nullptr, &gpu));
        context->CopyResource(gpu.Get(), staging.Get());
        out.push_back(gpu);
    }

    context->Flush();
    return true;
}

// ---------------------------------------------------------------------------
// Reporting helpers.
// ---------------------------------------------------------------------------

void row(const char* label, const char* value) {
    std::printf("  %-34s %s\n", label, value);
}

void row(const char* label, const std::string& value) {
    row(label, value.c_str());
}

void row_num(const char* label, double value, const char* unit) {
    std::printf("  %-34s %.3f %s\n", label, value, unit);
}

void row_int(const char* label, long long value) {
    std::printf("  %-34s %lld\n", label, value);
}

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto n = static_cast<double>(sorted.size());
    auto idx = static_cast<std::size_t>(p * n);
    if (idx >= sorted.size()) {
        idx = sorted.size() - 1;
    }
    return sorted[idx];
}

std::string pix_fmt_name(AVPixelFormat fmt) {
    const char* n = av_get_pix_fmt_name(fmt);
    return n != nullptr ? n : "<none>";
}

std::string supported_pix_fmts(const AVCodec* codec) {
    const void* cfg = nullptr;
    int n = 0;
    const int err = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n);
    if (err < 0 || cfg == nullptr) {
        return "<unavailable>";
    }

    const auto* fmts = static_cast<const AVPixelFormat*>(cfg);
    std::string s;
    for (int i = 0; i < n; ++i) {
        if (!s.empty()) {
            s += ", ";
        }
        s += pix_fmt_name(fmts[i]);
    }
    return s.empty() ? "<none>" : s;
}

} // namespace

int main(int argc, char** argv) {
    int frame_count = kDefaultFrames;
    std::string adapter_match = "780M";
    bool list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            frame_count = std::atoi(argv[++i]);
        } else if (arg == "--adapter" && i + 1 < argc) {
            adapter_match = argv[++i];
        } else if (arg == "--list-adapters") {
            list_only = true;
        } else if (arg == "--help") {
            std::printf("usage: amf_zerocopy [--frames N] [--adapter SUBSTRING] [--list-adapters]\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    std::printf("=== FrameCapture spike: h264_amf zero-copy (SPEC.md 2.2 item 1) ===\n\n");

    // Hook before anything touches libavcodec.
    install_transfer_hook();

    // -----------------------------------------------------------------------
    // Adapter
    // -----------------------------------------------------------------------
    std::vector<AdapterInfo> adapters;
    if (!enumerate_adapters(adapters)) {
        std::fprintf(stderr, "FATAL: no hardware DXGI adapters found.\n");
        return 1;
    }

    std::printf("Adapters present\n");
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        std::printf("  [%zu] %-44s %-9s %5llu MiB VRAM\n", i, adapters[i].description.c_str(),
                    vendor_name(adapters[i].vendor_id),
                    static_cast<unsigned long long>(adapters[i].dedicated_video_memory / (1024 * 1024)));
    }
    std::printf("\n");

    if (list_only) {
        return 0;
    }

    const AdapterInfo* chosen = nullptr;
    for (const AdapterInfo& a : adapters) {
        if (a.description.find(adapter_match) != std::string::npos) {
            chosen = &a;
            break;
        }
    }
    if (chosen == nullptr) {
        for (const AdapterInfo& a : adapters) {
            if (a.vendor_id == kAmdVendorId) {
                chosen = &a;
                std::fprintf(stderr,
                             "WARNING: no adapter matching \"%s\"; falling back to the first AMD adapter (%s).\n"
                             "         Results do not describe the reference 780M.\n\n",
                             adapter_match.c_str(), a.description.c_str());
                break;
            }
        }
    }
    if (chosen == nullptr) {
        std::fprintf(stderr,
                     "FATAL: no AMD adapter present. h264_amf cannot be measured on this machine.\n"
                     "       This benchmark is only meaningful on the reference rig (Radeon 780M).\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // D3D11 device on that adapter
    // -----------------------------------------------------------------------
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};

    // DRIVER_TYPE_UNKNOWN is mandatory when an explicit adapter is supplied.
    const HRESULT dev_hr = D3D11CreateDevice(chosen->adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                             D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, &obtained, &context);
    if (FAILED(dev_hr)) {
        fail("D3D11CreateDevice", dev_hr);
        return 1;
    }

    // FFmpeg drives the immediate context from its own threads.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    // -----------------------------------------------------------------------
    // hwdevice + hwframes over our device (never av_hwdevice_ctx_create, which
    // would pick its own adapter and defeat the point of the experiment)
    // -----------------------------------------------------------------------
    AVBufferRef* hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hw_device == nullptr) {
        std::fprintf(stderr, "FATAL: av_hwdevice_ctx_alloc(D3D11VA) returned null.\n");
        return 1;
    }
    {
        auto* dev_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(dev_ctx->hwctx);
        d3d->device = device.Get();
        d3d->device->AddRef(); // FFmpeg releases this on teardown.
        const int err = av_hwdevice_ctx_init(hw_device);
        if (err < 0) {
            fail_av("av_hwdevice_ctx_init", err);
            return 1;
        }
    }

    AVBufferRef* hw_frames = av_hwframe_ctx_alloc(hw_device);
    if (hw_frames == nullptr) {
        std::fprintf(stderr, "FATAL: av_hwframe_ctx_alloc returned null.\n");
        return 1;
    }
    {
        auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames->data);
        frames_ctx->format = AV_PIX_FMT_D3D11;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = kWidth;
        frames_ctx->height = kHeight;
        frames_ctx->initial_pool_size = kPoolSize;

        // Left at zero, FFmpeg defaults the pool to D3D11_BIND_DECODER, which
        // CreateTexture2D rejects for an NV12 encoder-input array on this part.
        auto* d3d_frames = static_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
        d3d_frames->BindFlags = probe_pool_bind_flags(device.Get(), kPoolSize);
        const int err = av_hwframe_ctx_init(hw_frames);
        if (err < 0) {
            fail_av("av_hwframe_ctx_init", err);
            return 1;
        }
    }

    // -----------------------------------------------------------------------
    // Encoder
    // -----------------------------------------------------------------------
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_amf");
    if (codec == nullptr) {
        std::fprintf(stderr, "FATAL: h264_amf is not present in this libavcodec build.\n");
        return 1;
    }

    AVCodecContext* enc = avcodec_alloc_context3(codec);
    if (enc == nullptr) {
        std::fprintf(stderr, "FATAL: avcodec_alloc_context3 returned null.\n");
        return 1;
    }

    enc->width = kWidth;
    enc->height = kHeight;
    enc->pix_fmt = AV_PIX_FMT_D3D11;
    enc->sw_pix_fmt = AV_PIX_FMT_NV12;
    enc->time_base = AVRational{1, 60000}; // SPEC.md §7.2
    enc->framerate = AVRational{60, 1};
    enc->gop_size = 120;
    enc->max_b_frames = 0; // a recorder has no use for reorder latency
    enc->bit_rate = 20'000'000;
    enc->hw_frames_ctx = av_buffer_ref(hw_frames);
    if (enc->hw_frames_ctx == nullptr) {
        std::fprintf(stderr, "FATAL: av_buffer_ref(hw_frames) returned null.\n");
        return 1;
    }

    const std::string requested = pix_fmt_name(enc->pix_fmt);
    const std::string supported = supported_pix_fmts(codec);

    {
        const int err = avcodec_open2(enc, codec, nullptr);
        if (err < 0) {
            fail_av("avcodec_open2(h264_amf)", err);
            return 1;
        }
    }

    // -----------------------------------------------------------------------
    // Source patterns
    // -----------------------------------------------------------------------
    std::vector<ComPtr<ID3D11Texture2D>> patterns;
    if (!build_pattern_textures(device.Get(), context.Get(), patterns)) {
        return 1;
    }

    // -----------------------------------------------------------------------
    // Encode loop
    //
    // Only avcodec_send_frame + the receive drain are timed. Staging the next
    // source frame into the pool texture is a device-to-device copy we perform,
    // not something the encoder does, so counting it would inflate the answer.
    // -----------------------------------------------------------------------
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (frame == nullptr || packet == nullptr) {
        std::fprintf(stderr, "FATAL: frame/packet allocation failed.\n");
        return 1;
    }

    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(frame_count));

    const double ticks_per_ms = static_cast<double>(qpc_frequency()) / 1000.0;
    long long packets = 0;
    long long bytes = 0;
    long transfers_before_loop = 0;

    const int total_iterations = kWarmupFrames + frame_count;
    for (int i = 0; i < total_iterations; ++i) {
        const bool warming = i < kWarmupFrames;
        if (i == kWarmupFrames) {
            transfers_before_loop = g_transfer_calls.load(std::memory_order_relaxed);
        }

        av_frame_unref(frame);
        {
            const int err = av_hwframe_get_buffer(hw_frames, frame, 0);
            if (err < 0) {
                fail_av("av_hwframe_get_buffer", err);
                return 1;
            }
        }

        auto* const dst_texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        const auto dst_index = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
        context->CopySubresourceRegion(dst_texture, dst_index, 0, 0, 0,
                                       patterns[static_cast<std::size_t>(i % kPatternCount)].Get(), 0, nullptr);

        frame->pts = static_cast<std::int64_t>(i) * 1000; // 60 fps in a 1/60000 timebase

        const std::int64_t t0 = qpc_now();

        int err = avcodec_send_frame(enc, frame);
        if (err < 0) {
            fail_av("avcodec_send_frame", err);
            return 1;
        }
        while (true) {
            err = avcodec_receive_packet(enc, packet);
            if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
                break;
            }
            if (err < 0) {
                fail_av("avcodec_receive_packet", err);
                return 1;
            }
            if (!warming) {
                ++packets;
                bytes += packet->size;
            }
            av_packet_unref(packet);
        }

        const std::int64_t t1 = qpc_now();
        if (!warming) {
            timings.push_back(static_cast<double>(t1 - t0) / ticks_per_ms);
        }
    }

    // Drain.
    avcodec_send_frame(enc, nullptr);
    while (avcodec_receive_packet(enc, packet) == 0) {
        ++packets;
        bytes += packet->size;
        av_packet_unref(packet);
    }

    const long transfers_total = g_transfer_calls.load(std::memory_order_relaxed);
    const long transfers_in_loop = transfers_total - transfers_before_loop;

    const AVPixelFormat negotiated = enc->pix_fmt;
    const AVPixelFormat negotiated_sw = enc->sw_pix_fmt;

    // -----------------------------------------------------------------------
    // Report
    // -----------------------------------------------------------------------
    std::vector<double> sorted = timings;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0.0;
    for (const double t : timings) {
        sum += t;
    }
    const double mean = timings.empty() ? 0.0 : sum / static_cast<double>(timings.size());

    std::printf("Device under test\n");
    row("adapter", chosen->description);
    row("vendor", vendor_name(chosen->vendor_id));
    row_int("feature level", static_cast<long long>(obtained));
    std::printf("\n");

    std::printf("(1) av_hwframe_transfer_data\n");
    row("exported by avutil", g_symbol_exported ? "yes -- the hook is meaningful" : "NO -- RESULT IS VOID");
    if (g_patch_sites.empty()) {
        row("imported by libavcodec", "no -- libavcodec cannot reach it at all");
        row("IAT patch sites", "0");
    } else {
        row("imported by libavcodec", "yes");
        for (const auto& [name, n] : g_patch_sites) {
            std::printf("  %-34s %d IAT entr%s patched\n", name.c_str(), n, n == 1 ? "y" : "ies");
        }
    }
    row_int("invocations during warm-up", transfers_before_loop);
    row_int("invocations on encode path", transfers_in_loop);
    std::printf("\n");

    std::printf("(3) Pixel format\n");
    row("requested", requested);
    row("negotiated (pix_fmt)", pix_fmt_name(negotiated));
    row("negotiated (sw_pix_fmt)", pix_fmt_name(negotiated_sw));
    row("encoder supports", supported);
    std::printf("\n");

    std::printf("(2) Encode time, %d frames @ %dx%d\n", static_cast<int>(timings.size()), kWidth, kHeight);
    row_num("mean", mean, "ms");
    row_num("p50", percentile(sorted, 0.50), "ms");
    row_num("p99", percentile(sorted, 0.99), "ms");
    row_num("min", sorted.empty() ? 0.0 : sorted.front(), "ms");
    row_num("max", sorted.empty() ? 0.0 : sorted.back(), "ms");
    row_num("throughput", mean > 0.0 ? 1000.0 / mean : 0.0, "fps");
    row_num("60 fps frame budget used", mean > 0.0 ? (mean / 16.667) * 100.0 : 0.0, "%");
    row_int("packets", packets);
    row_int("bytes", bytes);
    std::printf("\n");

    std::printf("Verdict (see README.md)\n");
    const bool fmt_ok = negotiated == AV_PIX_FMT_D3D11;
    const bool no_transfer = transfers_in_loop == 0;

    if (!g_symbol_exported) {
        row("conclusion", "INVALID RUN -- avutil does not export the symbol under test");
    } else if (!fmt_ok) {
        row("conclusion", "encoder did NOT negotiate d3d11 -- frames go through host memory");
    } else if (!no_transfer) {
        row("conclusion", "HOST ROUND-TRIP CONFIRMED -- the 2.2 guard rail has fired");
    } else if (g_patch_sites.empty()) {
        row("av_hwframe_transfer_data", "unreachable -- libavcodec does not import it");
        row("conclusion", "no wrapper-level round-trip is possible; see README on what this does NOT rule out");
    } else {
        row("av_hwframe_transfer_data", "hooked, never invoked");
        row("conclusion", "no wrapper-level round-trip observed; see README on what this does NOT rule out");
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    av_buffer_unref(&hw_frames);
    av_buffer_unref(&hw_device);

    return g_failed ? 1 : 0;
}
