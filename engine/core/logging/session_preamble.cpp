#include "core/logging/session_preamble.h"

#include "core/build_info.h"
#include "core/logging/logger.h"

#include <windows.h>

#include <intrin.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

namespace fc {
namespace {

/// Layout-compatible with RTL_OSVERSIONINFOW. Declared locally so this file does
/// not need <winternl.h>.
struct RtlOsVersionInfoW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfoW*);

std::string registry_string(const wchar_t* value) {
    wchar_t buffer[256] = {};
    DWORD bytes = sizeof(buffer);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value,
                                        RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    if (status != ERROR_SUCCESS) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

DWORD registry_dword(const wchar_t* value) {
    DWORD result = 0;
    DWORD bytes = sizeof(result);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value,
                                        RRF_RT_REG_DWORD, nullptr, &result, &bytes);
    return status == ERROR_SUCCESS ? result : 0;
}

/// `RtlGetVersion` rather than `GetVersionEx`: the latter is subject to
/// compatibility shims and lies about the build on a manifest-less binary, which
/// is exactly the value we need to trust when triaging an OS-specific capture bug.
std::string detect_os_build() {
    std::string version = "unknown";

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        // Cast the FARPROC straight to the target signature. Routing it through
        // void* is a separate object-pointer conversion that happens to work on
        // Win32 but is not what the language guarantees.
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr) {
            RtlOsVersionInfoW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtl_get_version(&info) == 0) {
                const DWORD ubr = registry_dword(L"UBR");
                char buffer[64] = {};
                std::snprintf(buffer, sizeof(buffer), "%lu.%lu.%lu.%lu", info.dwMajorVersion, info.dwMinorVersion,
                              info.dwBuildNumber, ubr);
                version = buffer;
            }
        }
    }

    const std::string product = registry_string(L"ProductName");
    const std::string display = registry_string(L"DisplayVersion");
    if (!product.empty()) {
        version += " (" + product;
        if (!display.empty()) {
            version += " " + display;
        }
        version += ")";
    }
    return version;
}

std::string detect_cpu_brand() {
    // Leaves 0x80000002..0x80000004 hold the 48-byte brand string.
    std::array<int, 4> regs{};
    // __cpuid takes a signed leaf; 0x80000000 does not fit in a positive int, so
    // the conversion is spelled out rather than left implementation-defined.
    __cpuid(regs.data(), static_cast<int>(0x80000000U));
    if (static_cast<unsigned>(regs[0]) < 0x80000004U) {
        return "unknown";
    }

    std::array<char, 49> brand{};
    for (std::size_t leaf = 0; leaf < 3; ++leaf) {
        __cpuid(regs.data(), static_cast<int>(0x80000002U + leaf));
        std::memcpy(brand.data() + (leaf * 16), regs.data(), 16);
    }
    brand[48] = '\0';

    std::string_view view{brand.data()};
    while (!view.empty() && (view.front() == ' ')) {
        view.remove_prefix(1);
    }
    while (!view.empty() && (view.back() == ' ' || view.back() == '\0')) {
        view.remove_suffix(1);
    }
    return view.empty() ? std::string{"unknown"} : std::string{view};
}

/// Marker for a field a later milestone owns. Distinguishable from a genuinely
/// empty result.
constexpr std::string_view kPending = "<pending>";

std::string_view or_pending(const std::string& value) {
    return value.empty() ? kPending : std::string_view{value};
}

} // namespace

SessionPreamble collect_session_preamble() {
    SessionPreamble preamble;
    preamble.app_version = std::string{project_version()};
    preamble.os_build = detect_os_build();
    preamble.cpu_brand = detect_cpu_brand();
    preamble.cpu_logical_processors = std::thread::hardware_concurrency();
    // Everything else is deliberately left empty; log_session_preamble marks the
    // owning milestone.
    return preamble;
}

void log_session_preamble(const SessionPreamble& preamble) {
    FC_LOG_INFO(Subsystem::App, "session preamble begin",
                LogFields{}.add("session_id", log::session_id()).add("app_version", preamble.app_version));

    FC_LOG_INFO(Subsystem::App, "host",
                LogFields{}
                    .add("os_build", preamble.os_build)
                    .add("cpu", preamble.cpu_brand)
                    .add("logical_processors", static_cast<std::int64_t>(preamble.cpu_logical_processors)));

    if (preamble.adapters.empty()) {
        FC_LOG_INFO(Subsystem::App, "adapter enumeration",
                    LogFields{}.add("status", kPending).add("owner", "M1 gpu topology service"));
    } else {
        for (std::size_t i = 0; i < preamble.adapters.size(); ++i) {
            const AdapterSummary& adapter = preamble.adapters[i];
            FC_LOG_INFO(Subsystem::App, "adapter",
                        LogFields{}
                            .add("index", static_cast<std::int64_t>(i))
                            .add("description", adapter.description)
                            .add("driver_version", adapter.driver_version)
                            .add("vendor_id", static_cast<std::uint64_t>(adapter.vendor_id))
                            .add("vram_bytes", adapter.dedicated_video_memory)
                            .add("owns_target_output", adapter.owns_target_output));
        }
    }

    FC_LOG_INFO(Subsystem::App, "session configuration",
                LogFields{}
                    .add("display_topology", or_pending(preamble.display_topology))
                    .add("audio_endpoint_format", or_pending(preamble.audio_endpoint_format))
                    .add("capture_backend", or_pending(preamble.capture_backend))
                    .add("encoder", or_pending(preamble.encoder))
                    .add("encoder_settings", or_pending(preamble.encoder_settings))
                    .add("config_hash", or_pending(preamble.config_hash)));

    FC_LOG_INFO(Subsystem::App, "session preamble end", LogFields{});
}

void log_session_preamble() {
    log_session_preamble(collect_session_preamble());
}

} // namespace fc
