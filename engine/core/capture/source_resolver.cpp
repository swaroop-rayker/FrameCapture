#include "core/capture/source_resolver.h"

#include "core/logging/logger.h"

#include <windows.h>
// Must follow windows.h.
#include <dwmapi.h>
#include <psapi.h>

#include <algorithm>
#include <vector>

namespace fc::capture {
namespace {

std::string narrow(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0') {
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

/// Maps a GDI device name (`\\.\DISPLAY1`) to its current HMONITOR and rectangle.
struct MonitorMatch {
    std::uintptr_t monitor = 0;
    RECT rect{};
    bool primary = false;
    bool found = false;
};

BOOL CALLBACK monitor_proc(HMONITOR monitor, HDC /*hdc*/, LPRECT /*clip*/, LPARAM data) {
    auto* context = reinterpret_cast<std::vector<std::pair<std::wstring, MonitorMatch>>*>(data);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == 0) {
        return TRUE;
    }

    MonitorMatch match;
    match.monitor = reinterpret_cast<std::uintptr_t>(monitor);
    match.rect = info.rcMonitor;
    match.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    match.found = true;
    context->emplace_back(info.szDevice, match);
    return TRUE;
}

std::vector<std::pair<std::wstring, MonitorMatch>> current_monitors() {
    std::vector<std::pair<std::wstring, MonitorMatch>> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &monitor_proc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

bool is_capturable_window(HWND window) {
    if (IsWindowVisible(window) == 0 || IsIconic(window) != 0) {
        return false;
    }
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return false; // tool windows, dialogs owned by something else
    }

    // Windows keeps invisible shell windows around. Capturing one gives a
    // permanently black recording, so they are filtered rather than offered.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != FALSE) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((style & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    RECT rect{};
    return GetWindowRect(window, &rect) != 0 && (rect.right - rect.left) > 0 && (rect.bottom - rect.top) > 0;
}

std::string process_name_of(DWORD process_id) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, path, &size) != 0) {
        const wchar_t* leaf = std::max(wcsrchr(path, L'\\'), wcsrchr(path, L'/'));
        name = narrow(leaf != nullptr ? leaf + 1 : path);
    }
    CloseHandle(process);
    return name;
}

BOOL CALLBACK window_proc(HWND window, LPARAM data) {
    auto* out = reinterpret_cast<std::vector<WindowSource>*>(data);

    if (!is_capturable_window(window)) {
        return TRUE;
    }

    wchar_t title[512] = {};
    if (GetWindowTextW(window, title, 512) == 0) {
        return TRUE; // untitled windows are not useful sources
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id == GetCurrentProcessId()) {
        return TRUE;
    }

    wchar_t class_name[256] = {};
    GetClassNameW(window, class_name, 256);

    WindowSource source;
    source.hwnd = reinterpret_cast<std::uintptr_t>(window);
    source.title = narrow(title);
    source.class_name = narrow(class_name);
    source.process_id = process_id;
    source.process_name = process_name_of(process_id);
    out->push_back(std::move(source));
    return TRUE;
}

} // namespace

Result<std::vector<DisplaySource>> enumerate_displays() {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return FcError::CAPTURE_INIT_FAILED;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return FcError::CAPTURE_INIT_FAILED;
    }
    paths.resize(path_count);

    const auto monitors = current_monitors();
    std::vector<DisplaySource> displays;

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        // Zero-initialising these two is the documented Win32 contract: every field
        // not assigned below must be zero. clang-tidy objects because the first
        // member is a DISPLAYCONFIG_DEVICE_INFO_TYPE, an enum with no zero-valued
        // enumerator -- but the very next line assigns it a valid one.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path.sourceInfo.adapterId;
        source_name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
            continue;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) -- see above.
        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name{};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = path.targetInfo.adapterId;
        target_name.header.id = path.targetInfo.id;
        const bool have_target = DisplayConfigGetDeviceInfo(&target_name.header) == ERROR_SUCCESS;

        DisplaySource display;
        display.target_id = path.targetInfo.id;
        display.device_name = narrow(source_name.viewGdiDeviceName);
        if (have_target) {
            // monitorDevicePath is EDID-derived, so it survives reboots -- unlike
            // the GDI name and unlike any index.
            display.stable_id = narrow(target_name.monitorDevicePath);
            display.friendly_name = narrow(target_name.monitorFriendlyDeviceName);
        }
        if (display.stable_id.empty()) {
            // Fall back to the connector id so a display is never unaddressable.
            display.stable_id = "target:" + std::to_string(path.targetInfo.id);
        }
        if (display.friendly_name.empty()) {
            display.friendly_name = display.device_name;
        }

        for (const auto& [gdi_name, match] : monitors) {
            if (gdi_name == source_name.viewGdiDeviceName) {
                display.monitor = match.monitor;
                display.left = match.rect.left;
                display.top = match.rect.top;
                display.width = match.rect.right - match.rect.left;
                display.height = match.rect.bottom - match.rect.top;
                display.primary = match.primary;
                break;
            }
        }

        if (display.monitor != 0) {
            displays.push_back(std::move(display));
        }
    }

    if (displays.empty()) {
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }
    return displays;
}

Result<std::vector<WindowSource>> enumerate_windows() {
    std::vector<WindowSource> windows;
    EnumWindows(&window_proc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

Result<DisplaySource> resolve_display(const std::string& stable_id) {
    auto displays = enumerate_displays();
    if (!displays.has_value()) {
        return displays.error();
    }

    const auto it = std::ranges::find(displays.value(), stable_id, &DisplaySource::stable_id);
    if (it == displays.value().end()) {
        FC_LOG_WARN(Subsystem::Capture, "configured display is not present",
                    LogFields{}.add("stable_id", stable_id).add_error(FcError::CAPTURE_TARGET_NOT_FOUND));
        return FcError::CAPTURE_TARGET_NOT_FOUND;
    }
    return *it;
}

Result<DisplaySource> primary_display() {
    auto displays = enumerate_displays();
    if (!displays.has_value()) {
        return displays.error();
    }

    const auto it = std::ranges::find(displays.value(), true, &DisplaySource::primary);
    if (it != displays.value().end()) {
        return *it;
    }
    return displays.value().front();
}

Result<WindowSource> reacquire_window(const WindowSource& previous) {
    // Still alive and still the same window? Nothing to do.
    if (previous.hwnd != 0) {
        auto* window = reinterpret_cast<HWND>(previous.hwnd);
        if (IsWindow(window) != 0 && is_capturable_window(window)) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(window, &process_id);
            if (process_id == previous.process_id) {
                return previous;
            }
        }
    }

    auto windows = enumerate_windows();
    if (!windows.has_value()) {
        return windows.error();
    }

    // Match on process name + class. Titles change constantly -- document name,
    // browser tab, playback position -- so matching on one would fail precisely
    // when re-acquisition matters.
    const auto it = std::ranges::find_if(windows.value(), [&previous](const WindowSource& candidate) {
        return !previous.process_name.empty() && candidate.process_name == previous.process_name &&
               candidate.class_name == previous.class_name;
    });

    if (it == windows.value().end()) {
        FC_LOG_WARN(Subsystem::Capture, "capture target window could not be re-acquired",
                    LogFields{}
                        .add("process", previous.process_name)
                        .add("class", previous.class_name)
                        .add_error(FcError::CAPTURE_TARGET_GONE));
        return FcError::CAPTURE_TARGET_GONE;
    }

    FC_LOG_INFO(Subsystem::Capture, "capture target window re-acquired",
                LogFields{}.add("process", it->process_name).add("class", it->class_name).add("title", it->title));
    return *it;
}

} // namespace fc::capture
