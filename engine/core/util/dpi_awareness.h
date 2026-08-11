#pragma once

namespace fc {

/// Declares the process per-monitor DPI aware (V2).
///
/// **A screen recorder must see physical pixels.** Without this, Windows
/// virtualises what it reports to a DPI-unaware process: on a 1920x1080 panel at
/// 125% scaling, `DXGI_OUTPUT_DESC.DesktopCoordinates` comes back as 1536x864 while
/// `Windows.Graphics.Capture` still hands over a real 1920x1080 texture.
///
/// Every consequence of that mismatch is silent and misleading:
///   * the topology service reports the wrong output size;
///   * a pipeline sized from that rectangle rejects every captured frame, or worse,
///     rescales without being asked (SPEC.md §4.4 forbids that);
///   * the recording resolution silently depends on the user's scaling setting.
///
/// Must be called before any DXGI, window, or capture call. Idempotent: returns
/// false if awareness was already set, including by an application manifest, which
/// is not an error.
///
/// The shipping executable should *also* declare this in its manifest -- the
/// manifest applies before any code runs, which is strictly better. This call
/// covers test binaries and any host that does not carry one.
bool set_process_dpi_awareness();

/// True when the process sees physical pixels.
[[nodiscard]] bool is_process_dpi_aware();

} // namespace fc
