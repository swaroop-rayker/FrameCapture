r"""The capture-exclusion primitive (M9.6 Phase 0, §20 row 19).

These assert the *window-level* half: that the affinity is set, that it is read back,
that it survives a handle recreation, and that a popup opened from an overlay is stamped
too. What they cannot assert is the half that matters most — that a frame captured
through WGC or DDA actually lacks the overlay and lacks a black rectangle where it was.
Only a real capture can prove that, and it is `test_overlay_exclusion.cpp` in the GPU
tier.

The split is deliberate rather than a limitation. `SetWindowDisplayAffinity` returning
success is the API's claim; the C++ test is the measurement. A Python test that stopped
at "the call succeeded" and reported the feature done would be exactly the "green test
that proves nothing" CLAUDE.md §6 warns about.

These need a **real** window handle, so they skip under the offscreen platform plugin.
Skipped loudly, per `addopts = -ra`: a silently-skipped exclusion test is the same
hazard as a silently-skipped hardware one.
"""

from __future__ import annotations

from collections.abc import Iterator

import pytest
from PySide6.QtCore import QCoreApplication, Qt
from PySide6.QtWidgets import QMenu, QWidget

from framecapture_gui.overlay.exclusion import (
    OVERLAY_PROPERTY,
    WDA_EXCLUDEFROMCAPTURE,
    WDA_MONITOR,
    WDA_NONE,
    ExclusionSupport,
    affinity_of,
    exclude_from_capture,
    guard,
    install_popup_guard,
    probe,
)


@pytest.fixture
def overlay_window(qapp: QCoreApplication) -> Iterator[QWidget]:
    """A real top-level window, shown offscreen.

    Shown rather than merely constructed: `winId()` on an unshown widget can return a
    handle that is not yet the one the window manager knows about, and the whole subject
    here is what happens to the real HWND.
    """
    del qapp
    window = QWidget(None, Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint)
    window.resize(120, 40)
    window.move(-32000, -32000)
    window.show()
    if not int(window.winId()):
        window.deleteLater()
        pytest.skip("no native window handle (offscreen platform plugin)")
    try:
        yield window
    finally:
        window.hide()
        window.deleteLater()


# ---------------------------------------------------------------------------
# The constants
# ---------------------------------------------------------------------------


def test_the_two_affinity_constants_are_not_confused() -> None:
    """The defect this module exists to prevent, pinned as a value.

    `WDA_MONITOR` (0x01) also hides a window from captures — by painting **black** into
    them. It is one hex digit from the right answer and it is what pre-2020 search
    results hand you, so the black-cutout bug is a typo away at all times. Asserting the
    values means a swap fails here rather than in somebody's recording.
    """
    assert WDA_NONE == 0x00
    assert WDA_MONITOR == 0x01
    assert WDA_EXCLUDEFROMCAPTURE == 0x11
    assert WDA_MONITOR != WDA_EXCLUDEFROMCAPTURE


# ---------------------------------------------------------------------------
# Applying it
# ---------------------------------------------------------------------------


def test_a_window_starts_capturable(overlay_window: QWidget) -> None:
    """The negative control.

    Without this, every assertion below could pass on a window that was never capturable
    to begin with, and the suite would prove nothing at all.
    """
    assert affinity_of(int(overlay_window.winId())) == WDA_NONE


def test_excluding_a_window_takes_and_reads_back(overlay_window: QWidget) -> None:
    handle = int(overlay_window.winId())
    assert exclude_from_capture(handle) is ExclusionSupport.EXCLUDED
    assert affinity_of(handle) == WDA_EXCLUDEFROMCAPTURE


def test_an_invalid_handle_is_reported_rather_than_assumed_good(overlay_window: QWidget) -> None:
    """A refusal must never read as success.

    `exclude_from_capture` returning `EXCLUDED` for a call that failed is the one bug in
    this module that would ship an overlay into a recording, because every caller trusts
    that return value to decide whether to show anything at all.
    """
    del overlay_window
    assert exclude_from_capture(0) is ExclusionSupport.UNKNOWN
    # 0xDEAD_BEEF is not a window. `SetWindowDisplayAffinity` fails with
    # ERROR_INVALID_WINDOW_HANDLE, which must surface as UNSUPPORTED.
    assert exclude_from_capture(0xDEADBEEF) is ExclusionSupport.UNSUPPORTED


def test_guard_marks_stamps_and_is_idempotent(overlay_window: QWidget) -> None:
    assert guard(overlay_window) is ExclusionSupport.EXCLUDED
    assert overlay_window.property(OVERLAY_PROPERTY) is True
    assert affinity_of(int(overlay_window.winId())) == WDA_EXCLUDEFROMCAPTURE

    guards_before = sum(1 for child in overlay_window.children() if type(child).__name__ == "_AffinityGuard")
    assert guard(overlay_window) is ExclusionSupport.EXCLUDED
    guards_after = sum(1 for child in overlay_window.children() if type(child).__name__ == "_AffinityGuard")
    assert guards_after == guards_before == 1, "guarding twice stacked a second event filter"


# ---------------------------------------------------------------------------
# The failure that only shows up later
# ---------------------------------------------------------------------------


def test_the_affinity_survives_a_native_handle_recreation(overlay_window: QWidget) -> None:
    """Display affinity belongs to an HWND, not to a QWidget.

    Qt destroys and recreates the native handle on `setWindowFlags`, on reparenting, and
    on some screen changes. The affinity does **not** come with it — so a pill that was
    correctly excluded at startup silently loses its exclusion the first time the user
    drags it to a second monitor, and every frame after that contains a pill. That is
    the whole reason `_AffinityGuard` watches `WinIdChange` rather than stamping once.
    """
    guard(overlay_window)
    original = int(overlay_window.winId())
    assert affinity_of(original) == WDA_EXCLUDEFROMCAPTURE

    # Forces Qt to drop and rebuild the native window.
    overlay_window.setWindowFlags(
        Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint
    )
    overlay_window.show()
    QCoreApplication.processEvents()

    rebuilt = int(overlay_window.winId())
    assert affinity_of(rebuilt) == WDA_EXCLUDEFROMCAPTURE, (
        "the affinity was lost when Qt recreated the handle"
        if rebuilt != original
        else "the handle was reused and the affinity still went missing"
    )


def test_a_popup_opened_from_an_overlay_is_stamped_too(overlay_window: QWidget, qapp: QCoreApplication) -> None:
    """A Qt popup is its own top-level HWND with its own default affinity.

    So an excluded pill still puts its own right-click menu into the recording, and
    nobody finds out until they watch the file back. Windows does not inherit display
    affinity down an ownership chain, and neither does Qt.
    """
    guard(overlay_window)
    install_popup_guard(qapp)

    menu = QMenu(overlay_window)
    menu.addAction("Stop recording")
    try:
        menu.popup(overlay_window.mapToGlobal(overlay_window.rect().center()))
        QCoreApplication.processEvents()
        handle = int(menu.winId())
        if not handle:
            pytest.skip("the popup got no native handle")
        assert affinity_of(handle) == WDA_EXCLUDEFROMCAPTURE, "a menu opened from the overlay would be recorded"
    finally:
        menu.close()
        menu.deleteLater()


def test_an_unrelated_window_is_left_capturable(qapp: QCoreApplication) -> None:
    """The guard must not stamp the whole application.

    A user may legitimately want to record FrameCapture's own window. Exclusion applies
    to overlay surfaces, and the set of overlay surfaces is explicit — not "everything
    that happens to be frameless".
    """
    install_popup_guard(qapp)
    plain = QWidget(None, Qt.WindowType.Tool)
    plain.resize(10, 10)
    plain.move(-32000, -32000)
    plain.show()
    QCoreApplication.processEvents()
    try:
        handle = int(plain.winId())
        if not handle:
            pytest.skip("no native window handle (offscreen platform plugin)")
        assert affinity_of(handle) == WDA_NONE, "an ordinary window was excluded from capture"
    finally:
        plain.hide()
        plain.deleteLater()


# ---------------------------------------------------------------------------
# The startup probe
# ---------------------------------------------------------------------------


def test_the_probe_answers_for_this_machine(qapp: QCoreApplication) -> None:
    """SPEC.md §1's floor is Windows 10 build 19041, which is the build that introduced
    `WDA_EXCLUDEFROMCAPTURE` — so on any supported platform the probe should succeed.

    `UNKNOWN` is accepted only for the offscreen plugin, where there is no handle to
    stamp. `UNSUPPORTED` is a real answer and would mean the overlay must not be shown;
    it is asserted against here because on this machine it would be a regression.
    """
    del qapp
    result = probe()
    assert result in (ExclusionSupport.EXCLUDED, ExclusionSupport.UNKNOWN)
    if result is ExclusionSupport.UNKNOWN:
        pytest.skip("no native handles available; the probe could not run")
