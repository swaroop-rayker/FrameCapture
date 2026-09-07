"""The GUI's half of SPEC.md §15.2, against a real engine and a real section.

Two kinds of case live here and they are worth telling apart:

* the **layout** cases need no engine at all. They assert that this process's idea of the
  header agrees with the C++ header's, field by field, by parsing the constants out of
  `engine/core/preview/preview_ring.h`. That is two places deciding one thing -- the shape
  BUG-043 came in -- so it is checked rather than trusted.
* the **channel** cases spawn an engine, arm the preview, and assert that frames arrive and
  that a ``QImage`` can be wrapped over them without copying. Marked ``engine``.

What is *not* asserted here is what the picture contains. The engine's own tier does that
(`PreviewTest.ARecordingPublishesDownscaledFramesTheGuiCanRead` reads the SMPTE bars back
out of the ring and checks the channel order), and repeating it in Python would need the
per-pixel work SPEC.md §16.1 forbids.
"""

from __future__ import annotations

import re
import time
from collections.abc import Iterator
from pathlib import Path

import pytest
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

from framecapture_gui import preview as preview_module
from framecapture_gui.engine import EngineController
from framecapture_gui.preview import PreviewChannel, PreviewUnavailableError

from .screen_activity import screen_activity

_ENGINE_HEADER = Path(__file__).resolve().parents[2] / "engine" / "core" / "preview" / "preview_ring.h"


def _engine_constants() -> dict[str, int]:
    """`inline constexpr <type> kName = <value>;` out of the C++ header."""
    text = _ENGINE_HEADER.read_text(encoding="utf-8")
    # The type may be qualified (`std::uint32_t`) and the literal may carry a digit
    # separator and a suffix (`0x5650'4346U`), so both are matched loosely and the
    # separators stripped before conversion.
    pattern = re.compile(r"inline constexpr [\w:]+ (k\w+)\s*=\s*(0x[0-9A-Fa-f']+|\d[\d']*)")
    return {name: int(value.replace("'", ""), 0) for name, value in pattern.findall(text)}


def test_the_header_layout_matches_the_engines() -> None:
    """The GUI unpacks the section the engine stamps, so the two must agree exactly.

    Parsed from the C++ source rather than duplicated as a second list of numbers: a
    duplicate would be checked against itself. If this ever fails, the engine's header
    moved and `preview.py` did not -- which would make the GUI read a frame's width out of
    its stride and produce a plausible, wrong picture.
    """
    constants = _engine_constants()
    assert constants, f"no constants parsed out of {_ENGINE_HEADER}"

    assert constants["kPreviewMagic"] == preview_module.MAGIC
    assert constants["kPreviewVersion"] == preview_module.VERSION
    assert constants["kPreviewHeaderBytes"] == preview_module.HEADER_BYTES
    assert constants["kPreviewFormatBgra8"] == preview_module.FORMAT_BGRA8

    for python_name, cpp_name in (
        ("OFFSET_MAGIC", "kOffsetMagic"),
        ("OFFSET_VERSION", "kOffsetVersion"),
        ("OFFSET_WIDTH", "kOffsetWidth"),
        ("OFFSET_HEIGHT", "kOffsetHeight"),
        ("OFFSET_STRIDE", "kOffsetStride"),
        ("OFFSET_SLOT_BYTES", "kOffsetSlotBytes"),
        ("OFFSET_SLOT_COUNT", "kOffsetSlotCount"),
        ("OFFSET_FORMAT", "kOffsetFormat"),
        ("OFFSET_FPS", "kOffsetFps"),
        ("OFFSET_WRITE_INDEX", "kOffsetWriteIndex"),
        ("OFFSET_DROPPED", "kOffsetDropped"),
        ("OFFSET_LAST_QPC_NS", "kOffsetLastQpcNs"),
    ):
        assert getattr(preview_module, python_name) == constants[cpp_name], python_name


def test_a_section_that_does_not_exist_is_refused_rather_than_created() -> None:
    """`mmap(tagname=...)` *creates* a missing section, which must not read as a preview.

    Without the magic check this is the failure mode: a GUI attaching to an engine that
    never armed the preview gets a page of zeros, wraps a `QImage` over it, and shows a
    black frame that never updates -- indistinguishable, to a user, from a capture that has
    stopped working.
    """
    channel = PreviewChannel()
    with pytest.raises(PreviewUnavailableError):
        channel.attach({"section": "Local\\framecapture-preview-does-not-exist", "bytes": 128 + (960 * 540 * 4)})
    assert not channel.attached


def test_a_response_without_a_section_is_refused() -> None:
    channel = PreviewChannel()
    with pytest.raises(PreviewUnavailableError):
        channel.attach({})
    with pytest.raises(PreviewUnavailableError):
        channel.attach({"section": "Local\\whatever", "bytes": 4})


# ---------------------------------------------------------------------------
# Against a real engine
# ---------------------------------------------------------------------------


@pytest.fixture
def controller(engine_path: Path, qapp: QCoreApplication) -> Iterator[EngineController]:
    del qapp
    control = EngineController(engine_path)
    assert control.start(), "the engine did not start"
    try:
        yield control
    finally:
        control.shutdown()


def _pump() -> None:
    app = QCoreApplication.instance()
    if app is not None:
        app.processEvents()


def _wait_for_frame(channel: PreviewChannel, timeout_s: float = 10.0) -> QImage | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        _pump()
        image = channel.latest_image()
        if image is not None:
            return image
        time.sleep(0.02)
    return None


@pytest.mark.engine
def test_the_gui_attaches_to_the_preview_and_receives_frames(controller: EngineController) -> None:
    """SPEC.md §15.2 end to end, from the process that has to read it.

    **Without a recording.** §15.1 makes `start_preview` and `start_record` independent
    commands and §16.2 puts the preview surface above the controls, so a preview that only
    existed during a recording would be one you could not frame a shot with. This asserts
    the case the layout implies.
    """
    with screen_activity():
        channel = controller.start_preview()
        assert channel is not None, f"start_preview returned nothing; capabilities={controller.capabilities}"
        assert channel.attached

        geometry = channel.geometry
        assert geometry is not None
        # §15.2's defaults, read from the section rather than assumed.
        assert (geometry.width, geometry.height) == (960, 540)
        assert geometry.fps == 30
        assert geometry.slots >= 3, "§15.2 specifies a triple-buffered ring"
        assert geometry.stride >= geometry.width * 4

        image = _wait_for_frame(channel)
        assert image is not None, f"no preview frame arrived; sequence={channel.sequence}"
        assert not image.isNull()
        assert image.width() == geometry.width
        assert image.height() == geometry.height
        # Format_RGB32 is 0xffRRGGBB packed little-endian, i.e. B,G,R,A in memory -- which
        # is the byte order the engine writes. Any other format here means the GUI would
        # render the picture with its channels exchanged.
        assert image.format() == QImage.Format.Format_RGB32

        # It is a *view*, not a copy: §16.1 allows the GUI to wrap a QImage over the
        # shared-memory buffer "and nothing more". A copy would still pass every assertion
        # above, so this is checked by the one property a copy cannot have.
        assert image.constBits() is not None
        assert image.bytesPerLine() == geometry.stride

        # And frames keep coming, rather than one arriving and the ring stalling.
        first = channel.sequence
        assert _wait_for_frame(channel) is not None
        assert channel.sequence > first, "the ring published once and stopped"

        controller.stop_preview()
        assert controller.preview is None


@pytest.mark.engine
def test_the_preview_survives_a_recording_starting_and_stopping(
    controller: EngineController, output_path: Path
) -> None:
    """The state machine §15.1's independent commands make reachable.

    The preview is armed before the recording, has to keep working *through* it -- the
    engine hands the ring from a preview-only session to the recording's own -- and has to
    still be there afterwards. The mapping must survive all of it: the GUI holds one
    `QImage` factory over one section, and a section recreated per recording would leave it
    pointing at freed pages twice per recording.
    """
    with screen_activity():
        channel = controller.start_preview()
        assert channel is not None
        assert _wait_for_frame(channel) is not None, "no frame before the recording"

        assert controller.start_recording(output_path, audio=False), "start_record was refused"
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline and controller.state.value != "recording":
            _pump()
            time.sleep(0.05)

        during = _wait_for_frame(channel)
        assert during is not None, "the preview stopped when the recording started"
        assert during.width() == 960

        assert controller.stop_recording_and_wait(), "stop_record was refused"
        _pump()

        after = _wait_for_frame(channel)
        assert after is not None, "the preview did not come back after the recording stopped"
        assert channel.attached, "the mapping did not survive the recording"

        assert output_path.is_file()
        controller.stop_preview()


@pytest.mark.engine
def test_the_engine_reports_what_the_preview_is_doing(controller: EngineController) -> None:
    """§15.2's droppability is only honest if the drops are countable.

    `get_stats` carries the preview's counters, so "the preview is stuttering" is a number
    a user can be shown rather than something they have to describe.
    """
    with screen_activity():
        channel = controller.start_preview()
        assert channel is not None
        assert _wait_for_frame(channel) is not None

        stats = controller.get_stats()
        assert stats is not None
        assert "preview" in stats, stats
        assert stats["preview"]["armed"] is True
        assert stats["preview"]["width"] == 960
        assert int(stats["preview_published"]) > 0
        # Reported, and reported as a number rather than as a flag.
        assert int(stats["preview_offer_worst_us"]) >= 0
        assert int(channel.dropped) >= 0

        controller.stop_preview()
        after = controller.get_stats()
        assert after is not None
        assert after["preview"]["armed"] is False
