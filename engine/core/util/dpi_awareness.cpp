#include "core/util/dpi_awareness.h"

#include <windows.h>

namespace fc {

bool set_process_dpi_awareness() {
    // Resolved dynamically: SetProcessDpiAwarenessContext needs Windows 10 1703+,
    // and while that is below our floor of build 19041, a missing export must
    // degrade rather than fail to load the process.
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        const auto set_context =
            reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context != nullptr) {
            // V2 rather than plain PER_MONITOR_AWARE: V2 also makes child windows
            // and non-client areas scale correctly, and it is the context Windows
            // treats as fully modern.
            return set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
        }
    }

    // Pre-1703 fallback. Deprecated, but it is the only option there.
    return SetProcessDPIAware() != FALSE;
}

bool is_process_dpi_aware() {
    using GetContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)();
    using GetAwarenessFn = DPI_AWARENESS(WINAPI*)(DPI_AWARENESS_CONTEXT);

    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        const auto get_context = reinterpret_cast<GetContextFn>(GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
        const auto get_awareness =
            reinterpret_cast<GetAwarenessFn>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
        if (get_context != nullptr && get_awareness != nullptr) {
            const DPI_AWARENESS awareness = get_awareness(get_context());
            return awareness == DPI_AWARENESS_SYSTEM_AWARE || awareness == DPI_AWARENESS_PER_MONITOR_AWARE;
        }
    }
    return IsProcessDPIAware() != FALSE;
}

} // namespace fc
