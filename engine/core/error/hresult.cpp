#include "core/error/hresult.h"

#include "core/logging/logger.h"

#include <windows.h>
// These must follow windows.h.
#include <audioclient.h>
#include <dxgi.h>

#include <array>
#include <cstdio>

namespace fc {
namespace {

struct HResultName {
    HResult hr;
    std::string_view name;
};

#define FC_HR_NAME(symbol)                                                                                             \
    HResultName {                                                                                                      \
        static_cast<HResult>(symbol), #symbol                                                                          \
    }

/// Codes worth naming. `FormatMessage` returns nothing useful for any of the
/// DXGI, D3D or WASAPI entries, which are precisely the ones this project spends
/// its time diagnosing (SPEC.md §5.4, §14.1).
///
constexpr std::array kNames = {
    // --- DXGI (SPEC.md §5.4) ---
    FC_HR_NAME(DXGI_ERROR_DEVICE_HUNG),
    FC_HR_NAME(DXGI_ERROR_DEVICE_REMOVED),
    FC_HR_NAME(DXGI_ERROR_DEVICE_RESET),
    FC_HR_NAME(DXGI_ERROR_DRIVER_INTERNAL_ERROR),
    FC_HR_NAME(DXGI_ERROR_INVALID_CALL),
    FC_HR_NAME(DXGI_ERROR_ACCESS_LOST),
    FC_HR_NAME(DXGI_ERROR_ACCESS_DENIED),
    FC_HR_NAME(DXGI_ERROR_NOT_FOUND),
    FC_HR_NAME(DXGI_ERROR_MORE_DATA),
    FC_HR_NAME(DXGI_ERROR_UNSUPPORTED),
    FC_HR_NAME(DXGI_ERROR_WAS_STILL_DRAWING),
    FC_HR_NAME(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE),
    FC_HR_NAME(DXGI_ERROR_SESSION_DISCONNECTED),
    FC_HR_NAME(DXGI_ERROR_NAME_ALREADY_EXISTS),

    // --- WASAPI (SPEC.md §8, §14.1) ---
    FC_HR_NAME(AUDCLNT_E_DEVICE_INVALIDATED),
    FC_HR_NAME(AUDCLNT_E_UNSUPPORTED_FORMAT),
    FC_HR_NAME(AUDCLNT_E_BUFFER_TOO_LARGE),
    FC_HR_NAME(AUDCLNT_E_BUFFER_SIZE_ERROR),
    FC_HR_NAME(AUDCLNT_E_SERVICE_NOT_RUNNING),
    FC_HR_NAME(AUDCLNT_E_DEVICE_IN_USE),
    FC_HR_NAME(AUDCLNT_E_NOT_INITIALIZED),
    FC_HR_NAME(AUDCLNT_E_ALREADY_INITIALIZED),

    // --- Generic COM ---
    FC_HR_NAME(E_ABORT),
    FC_HR_NAME(E_ACCESSDENIED),
    FC_HR_NAME(E_FAIL),
    FC_HR_NAME(E_HANDLE),
    FC_HR_NAME(E_INVALIDARG),
    FC_HR_NAME(E_NOINTERFACE),
    FC_HR_NAME(E_NOTIMPL),
    FC_HR_NAME(E_OUTOFMEMORY),
    FC_HR_NAME(E_POINTER),
    FC_HR_NAME(E_UNEXPECTED),

    // --- Wrapped Win32 (SPEC.md §10.4, §13) ---
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_DISK_FULL)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)),
    FC_HR_NAME(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED)),
};

#undef FC_HR_NAME

std::string system_message(HResult hr) {
    LPWSTR buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        if (buffer != nullptr) {
            LocalFree(buffer);
        }
        return {};
    }

    std::wstring wide{buffer, length};
    LocalFree(buffer);

    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
        wide.pop_back();
    }
    if (wide.empty()) {
        return {};
    }

    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), needed, nullptr,
                        nullptr);
    return narrow;
}

std::string_view basename(std::string_view path) {
    const std::size_t cut = path.find_last_of("\\/");
    return cut == std::string_view::npos ? path : path.substr(cut + 1);
}

} // namespace

std::string_view hresult_name(HResult hr) noexcept {
    for (const HResultName& entry : kNames) {
        if (entry.hr == hr) {
            return entry.name;
        }
    }
    return {};
}

std::string hresult_message(HResult hr) {
    char hex[16] = {};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(hr));

    std::string out{hex};

    const std::string_view name = hresult_name(hr);
    if (!name.empty()) {
        out += " (";
        out.append(name);
        out += ")";
    }

    const std::string system = system_message(hr);
    if (!system.empty()) {
        out += ": ";
        out += system;
    } else if (name.empty()) {
        // Say so explicitly rather than returning a bare hex code that looks
        // like the lookup simply was not attempted.
        out += ": no system description available";
    }

    return out;
}

FcError hresult_to_fc_error(HResult hr) noexcept {
    if (hr_succeeded(hr)) {
        return FcError::NONE;
    }

    switch (hr) {
    // --- GPU device lifetime (SPEC.md §5.4) ---
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return FcError::GPU_DEVICE_REMOVED;
    case DXGI_ERROR_DEVICE_RESET:
        return FcError::GPU_DEVICE_RESET;
    case DXGI_ERROR_DEVICE_HUNG:
        return FcError::GPU_DEVICE_HUNG;

    // --- Desktop duplication ---
    case DXGI_ERROR_ACCESS_LOST:
        return FcError::DDA_ACCESS_LOST;
    case DXGI_ERROR_ACCESS_DENIED:
        return FcError::DDA_ACCESS_DENIED;
    case DXGI_ERROR_SESSION_DISCONNECTED:
        return FcError::CAPTURE_TARGET_GONE;
    case DXGI_ERROR_UNSUPPORTED:
        return FcError::INTERNAL_NOT_IMPLEMENTED;

    // --- Audio (SPEC.md §14.1) ---
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return FcError::AUDIO_DEVICE_LOST;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return FcError::AUDIO_MIX_FORMAT_UNSUPPORTED;
    case AUDCLNT_E_BUFFER_TOO_LARGE:
    case AUDCLNT_E_BUFFER_SIZE_ERROR:
        return FcError::AUDIO_BUFFER_OVERRUN;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
    case AUDCLNT_E_DEVICE_IN_USE:
        return FcError::AUDIO_ENDPOINT_ACTIVATE_FAILED;

    // --- Generic COM ---
    case E_OUTOFMEMORY:
        return FcError::INTERNAL_OUT_OF_MEMORY;
    case E_INVALIDARG:
    case E_POINTER:
        return FcError::INTERNAL_INVALID_ARGUMENT;
    case E_NOTIMPL:
    case E_NOINTERFACE:
        return FcError::INTERNAL_NOT_IMPLEMENTED;
    case E_ACCESSDENIED:
        return FcError::IO_PERMISSION_DENIED;
    case E_HANDLE:
        return FcError::IO_FILE_HANDLE_LOST;
    case E_ABORT:
        return FcError::INTERNAL_CANCELLED;

    default:
        break;
    }

    // Wrapped Win32 codes cannot appear as case labels portably, so they are
    // compared here.
    if (hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)) {
        return FcError::IO_DISK_FULL;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return FcError::IO_PATH_INVALID;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
        return FcError::IO_PERMISSION_DENIED;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)) {
        return FcError::IO_FILE_HANDLE_LOST;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)) {
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }

    // Deliberately not a plausible guess. An unmapped HRESULT is unclassified,
    // and saying so is more useful than inventing a subsystem for it.
    return FcError::INTERNAL_UNKNOWN;
}

namespace detail {

FcError report_hresult(std::string_view expression, std::string_view file, int line, std::string_view function,
                       HResult hr, FcError error) {
    LogFields fields;
    fields.add("expr", expression)
        .add("file", basename(file))
        .add("line", line)
        .add("function", function)
        .add("hr", hresult_message(hr))
        .add_error(error);

    FC_LOG_ERROR(subsystem_of(error), "HRESULT check failed", fields);
    return error;
}

bool check_hresult(std::string_view expression, std::string_view file, int line, std::string_view function,
                   HResult hr) {
    if (hr_succeeded(hr)) {
        return true;
    }
    report_hresult(expression, file, line, function, hr, hresult_to_fc_error(hr));
    return false;
}

} // namespace detail
} // namespace fc
