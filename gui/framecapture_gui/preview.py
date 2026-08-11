"""The reader half of SPEC.md §15.2's preview channel.

The engine writes downscaled BGRA frames into a named shared-memory ring; this attaches to
it and hands out a ``QImage`` wrapped over the current slot.

**Zero media processing (SPEC.md §16.1).** There is no numpy here, no PIL, no per-pixel
Python, and -- deliberately -- no copy: ``QImage`` is constructed over a ``memoryview`` of
the mapping itself, so a preview frame goes from the GPU's readback straight to the
compositor with the GUI process touching none of its bytes. The only per-frame work in this
file is unpacking a 64-bit counter out of the header.

---------------------------------------------------------------------------
The header layout, and why it is duplicated here
---------------------------------------------------------------------------
The offsets below mirror ``engine/core/preview/preview_ring.h``. That is two places
deciding one thing, which is the shape BUG-043 came in, so it is not left to good
intentions: the engine reports the geometry in ``start_preview``'s response and this class
*checks the header against it*, and ``test_preview_reader.py`` asserts the two files agree
field by field. The alternative -- having the GUI trust the response and never read the
header -- would silently misread a section written by a different build.

---------------------------------------------------------------------------
Tearing, and why this reads rather than locks
---------------------------------------------------------------------------
§15.2 requires the preview to be "independently droppable ... with zero effect on the
recording", so nothing here may make the engine wait -- not a lock, not an acknowledgement,
not even a handshake. Instead the write index is read before and after using a frame: three
slots means the writer must publish twice more before it reaches the slot being read, so a
reader that finishes within one publication read memory nobody was writing. A reader that
did not simply drops the frame, and the engine never learns it exists.
"""

from __future__ import annotations

import logging
import mmap
import struct
from dataclasses import dataclass
from typing import Any

from PySide6.QtGui import QImage

_log = logging.getLogger(__name__)

#: 'FCPV' little-endian, matching ``fc::preview::kPreviewMagic``.
MAGIC = 0x56504346
VERSION = 1

# Byte offsets into the header. `engine/core/preview/preview_ring.h` is the original.
OFFSET_MAGIC = 0
OFFSET_VERSION = 4
OFFSET_WIDTH = 8
OFFSET_HEIGHT = 12
OFFSET_STRIDE = 16
OFFSET_SLOT_BYTES = 20
OFFSET_SLOT_COUNT = 24
OFFSET_FORMAT = 28
OFFSET_FPS = 32
OFFSET_WRITE_INDEX = 40
OFFSET_DROPPED = 48
OFFSET_LAST_QPC_NS = 56
HEADER_BYTES = 128

#: The only pixel format §15.2 allows: B, G, R, A in memory order.
FORMAT_BGRA8 = 0


class PreviewUnavailableError(Exception):
    """The section is absent, or is not a preview this build understands."""


@dataclass(frozen=True)
class PreviewGeometry:
    """What the header says the section holds."""

    width: int
    height: int
    stride: int
    slot_bytes: int
    slots: int
    fps: int

    @property
    def total_bytes(self) -> int:
        return HEADER_BYTES + (self.slot_bytes * self.slots)


class PreviewChannel:
    """An attached preview ring.

    Not thread-safe and not meant to be: everything here runs on the GUI thread, driven by
    the widget's repaint timer.
    """

    def __init__(self) -> None:
        self._map: mmap.mmap | None = None
        self._view: memoryview | None = None
        self._geometry: PreviewGeometry | None = None
        self._section = ""
        #: Frames the writer had lapped past before this reader could use them.
        self.torn_frames = 0
        self._last_sequence = 0

    # -- lifecycle ----------------------------------------------------------

    def attach(self, response: dict[str, Any]) -> None:
        """Attach to the section ``start_preview`` described.

        Raises ``PreviewUnavailableError`` rather than returning a status, because every caller
        treats a missing preview the same way -- show the placeholder -- and an exception
        keeps that in one place.
        """
        section = str(response.get("section", ""))
        if not section:
            raise PreviewUnavailableError("the engine reported no preview section")

        expected_bytes = int(response.get("bytes", 0))
        if expected_bytes <= HEADER_BYTES:
            raise PreviewUnavailableError(f"implausible section size {expected_bytes}")

        self.detach()
        try:
            # `mmap` with a tagname and no file *creates* the section when it does not
            # exist, rather than failing -- so a GUI attaching to an engine that never
            # armed the preview would get a page of zeros. The magic check below is what
            # turns that into an error instead of an all-black frame that never updates.
            mapping = mmap.mmap(-1, expected_bytes, tagname=section)
        except OSError as error:
            raise PreviewUnavailableError(f"could not map {section}: {error}") from error

        try:
            geometry = self._read_header(mapping, expected_bytes)
        except PreviewUnavailableError:
            mapping.close()
            raise

        self._map = mapping
        self._view = memoryview(mapping)
        self._geometry = geometry
        self._section = section
        self.torn_frames = 0
        self._last_sequence = 0
        _log.info("preview attached: %s %dx%d @%dfps", section, geometry.width, geometry.height, geometry.fps)

    @staticmethod
    def _read_header(mapping: mmap.mmap, expected_bytes: int) -> PreviewGeometry:
        magic, version = struct.unpack_from("<II", mapping, OFFSET_MAGIC)
        if magic != MAGIC:
            raise PreviewUnavailableError("the section is not a FrameCapture preview")
        if version != VERSION:
            raise PreviewUnavailableError(f"preview layout version {version} is not {VERSION}")

        width, height, stride, slot_bytes, slots, pixel_format, fps = struct.unpack_from(
            "<IIIIIII", mapping, OFFSET_WIDTH
        )
        if pixel_format != FORMAT_BGRA8:
            raise PreviewUnavailableError(f"pixel format {pixel_format} is not BGRA8")
        if width <= 0 or height <= 0 or slots < 2 or stride < width * 4:
            raise PreviewUnavailableError(f"implausible geometry {width}x{height} stride={stride} slots={slots}")

        geometry = PreviewGeometry(width, height, stride, slot_bytes, slots, fps)
        if geometry.total_bytes > expected_bytes:
            # The header describes more than was mapped. Refused rather than clamped:
            # reading a slot past the end of the mapping is an access violation, and the
            # only honest response to a section a different build wrote is to not use it.
            raise PreviewUnavailableError(
                f"the header describes {geometry.total_bytes} bytes but only {expected_bytes} were mapped"
            )
        return geometry

    def detach(self) -> None:
        """Release the mapping. Idempotent, and **never raises**.

        Both releases are guarded, and that is not defensive programming -- it is the
        direct consequence of handing out zero-copy frames.

        A ``QImage`` built over a slice of the mapping holds a *buffer export* on it for as
        long as it lives. CPython refuses to release an exported ``memoryview`` and refuses
        to close an ``mmap`` with exports outstanding, both with ``BufferError``. So a
        caller that still has the last frame on screen when the engine goes away would,
        without this, get an exception out of ``detach`` -- measured, from
        ``test_the_gui_attaches_to_the_preview_and_receives_frames``, which held the image
        it had just asserted on. The consequences would have been a window that cannot
        close and an engine restart that cannot re-attach.

        Dropping the references and letting the unmap happen later is safe, and precisely
        because of the same refusal: CPython will not unmap memory something is still
        pointing into, so a surviving ``QImage`` reads a *live* mapping. A section outlives
        the engine's own handle until the last mapping goes -- that is how the kernel object
        is refcounted -- so the worst case is a frame frozen on the last thing published.
        Not a crash, and not a blank window.
        """
        view, mapping = self._view, self._map
        self._view = None
        self._map = None
        self._geometry = None
        self._section = ""
        self._last_sequence = 0

        if view is not None:
            try:
                view.release()
            except BufferError:
                _log.debug("a preview frame is still referenced; the view is released with it")
        if mapping is not None:
            try:
                mapping.close()
            except BufferError:
                _log.debug("a preview frame is still on screen; the mapping is unmapped when it is dropped")

    @property
    def attached(self) -> bool:
        return self._map is not None

    @property
    def geometry(self) -> PreviewGeometry | None:
        return self._geometry

    @property
    def dropped(self) -> int:
        """Preview frames the engine skipped (§15.2's droppability), as it reports them."""
        if self._map is None:
            return 0
        return int(struct.unpack_from("<Q", self._map, OFFSET_DROPPED)[0])

    @property
    def sequence(self) -> int:
        """The engine's publication count, or 0 when nothing has been published."""
        if self._map is None:
            return 0
        return int(struct.unpack_from("<Q", self._map, OFFSET_WRITE_INDEX)[0])

    # -- frames -------------------------------------------------------------

    def latest_image(self) -> QImage | None:
        """A ``QImage`` over the most recently published frame, or ``None``.

        The image **borrows** the mapping: it is valid until the writer laps the slot,
        which is why the caller must paint it and drop it rather than keep it. Returning a
        copy instead would put a two-megabyte memcpy per frame in Python, which is the one
        thing SPEC.md §16.1 rules out by name.
        """
        if self._map is None or self._view is None or self._geometry is None:
            return None

        before = self.sequence
        if before == 0:
            return None
        if before == self._last_sequence:
            return None  # nothing new; the caller keeps whatever it drew last

        geometry = self._geometry
        slot = (before - 1) % geometry.slots
        start = HEADER_BYTES + (slot * geometry.slot_bytes)
        pixels = self._view[start : start + geometry.slot_bytes]

        # Format_RGB32 is 0xffRRGGBB packed into a uint32, which on a little-endian machine
        # is B, G, R, A in memory -- exactly what the engine writes. Format_ARGB32 would
        # ask Qt to honour an alpha channel the shader hard-codes to 1.
        image = QImage(pixels, geometry.width, geometry.height, geometry.stride, QImage.Format.Format_RGB32)

        # The writer may have lapped this slot while the QImage was being built. Three
        # slots buy one publication of slack -- see `lap_distance` in preview_ring.cpp for
        # why it is one and not two.
        if self.sequence - before > geometry.slots - 2:
            self.torn_frames += 1
            return None

        self._last_sequence = before
        return image
