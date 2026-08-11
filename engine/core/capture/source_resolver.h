#pragma once

#include "core/error/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fc::capture {

/// A monitor, identified the way SPEC.md §4.1 requires: "by stable DISPLAYCONFIG
/// path ID (not by index; indices reshuffle on hotplug)".
struct DisplaySource {
    /// The connector's DISPLAYCONFIG target id. Stable for as long as the monitor
    /// stays on the same output, and unaffected by enumeration order.
    std::uint32_t target_id = 0;

    /// The monitor's device interface path, e.g.
    /// `\\?\DISPLAY#GSM5B09#5&...#{e6f07b5f-...}`. Derived from EDID, so it
    /// survives reboots and cable swaps between ports on the same machine. This is
    /// what config should persist.
    std::string stable_id;

    /// GDI name, e.g. `\\.\DISPLAY1`. Convenient for logs, useless as an identity:
    /// it renumbers.
    std::string device_name;

    /// e.g. "Generic PnP Monitor". For the GUI's source list.
    std::string friendly_name;

    /// Current HMONITOR, as an opaque value. Valid only until the topology
    /// changes -- re-resolve after any display change.
    std::uintptr_t monitor = 0;

    /// Physical pixels. Correct only in a DPI-aware process (see BUG-004).
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;

    bool primary = false;
};

/// A capturable top-level window.
struct WindowSource {
    std::uintptr_t hwnd = 0;
    std::string title;
    /// Window class plus process name are what SPEC.md §4.1's re-acquisition
    /// fallback matches on when an HWND dies.
    std::string class_name;
    std::string process_name;
    std::uint32_t process_id = 0;
};

/// All active displays, in DISPLAYCONFIG order.
[[nodiscard]] Result<std::vector<DisplaySource>> enumerate_displays();

/// Top-level, visible, non-cloaked windows with a title, excluding our own.
///
/// Cloaked windows are filtered because Windows keeps invisible shells around --
/// capturing one yields a permanently black recording.
[[nodiscard]] Result<std::vector<WindowSource>> enumerate_windows();

/// Re-resolves a display by its stable id.
///
/// Returns `CAPTURE_TARGET_NOT_FOUND` when the monitor is gone, which is a normal
/// event (unplugged, powered off) and not a defect.
[[nodiscard]] Result<DisplaySource> resolve_display(const std::string& stable_id);

/// The display containing the desktop origin.
[[nodiscard]] Result<DisplaySource> primary_display();

/// SPEC.md §4.1's window re-acquisition: if `previous.hwnd` is dead, find a live
/// window with the same class in the same executable.
///
/// Matching on process name and class rather than on title, because a title
/// changes constantly (document name, tab, playback position) and would make
/// re-acquisition fail exactly when it is needed.
[[nodiscard]] Result<WindowSource> reacquire_window(const WindowSource& previous);

} // namespace fc::capture
