"""The control channel's wire format (SPEC.md §15.1).

    4-byte little-endian length prefix + UTF-8 JSON body. Max message 64 KB.

The Python mirror of ``engine/core/ipc/framing.h``. Two implementations of one wire
format is a cost worth naming: they can drift. What makes it acceptable is that the
format is four lines of arithmetic, both sides are tested against the *same* stated
rules, and the alternative -- a C extension binding the C++ one -- would put a build
step between the GUI and its ability to start.

The prefix is the authority, not the pipe's message mode. The engine creates the pipe
with ``PIPE_TYPE_MESSAGE``, but this client reads it as a byte stream, so message
boundaries here come from the prefix alone. That is the arrangement ``IPC_PROTOCOL.md``
describes, and it is why the prefix exists at all.
"""

from __future__ import annotations

import json
from typing import Any

#: SPEC.md §15.1's ceiling. Applies to the JSON body; the prefix is on top.
MAX_MESSAGE_BYTES = 64 * 1024

#: Width of the length prefix, little-endian per §15.1.
LENGTH_PREFIX_BYTES = 4


class FramingError(Exception):
    """A frame that cannot be recovered from.

    Distinct from "not enough bytes yet", which is not an error and is signalled by
    returning ``None``. Conflating the two would make every partial read on a stream
    look like a failure.
    """


def encode_frame(body: str) -> bytes:
    """Prepend the length prefix to ``body``.

    Raises ``FramingError`` rather than emitting a frame the peer is obliged to reject:
    the sender is the side that can still do something about it.
    """
    encoded = body.encode("utf-8")
    if len(encoded) > MAX_MESSAGE_BYTES:
        raise FramingError(f"message of {len(encoded)} bytes exceeds the {MAX_MESSAGE_BYTES} byte limit")
    return len(encoded).to_bytes(LENGTH_PREFIX_BYTES, "little") + encoded


def encode_message(message: dict[str, Any]) -> bytes:
    """Serialise and frame one message."""
    # `separators` drops the whitespace `json.dumps` inserts by default. It is not
    # about bandwidth -- it is that the 64 KB ceiling should be spent on content.
    return encode_frame(json.dumps(message, separators=(",", ":")))


class FrameReader:
    """Reassembles frames from a byte stream, holding whatever is left over.

    One ``feed`` can yield several frames or none. Call ``next_frame`` until it returns
    ``None``.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, chunk: bytes) -> None:
        self._buffer.extend(chunk)

    def next_frame(self) -> str | None:
        """The next complete frame, or ``None`` when more bytes are needed."""
        if len(self._buffer) < LENGTH_PREFIX_BYTES:
            return None

        length = int.from_bytes(self._buffer[:LENGTH_PREFIX_BYTES], "little")

        # Before anything is sliced or decoded. A corrupt or hostile prefix declaring
        # 4 GB must be an error now rather than an allocation first.
        if length > MAX_MESSAGE_BYTES:
            raise FramingError(f"declared length {length} exceeds the {MAX_MESSAGE_BYTES} byte limit")

        end = LENGTH_PREFIX_BYTES + length
        if len(self._buffer) < end:
            return None

        body = bytes(self._buffer[LENGTH_PREFIX_BYTES:end]).decode("utf-8", errors="replace")

        # Deleted from the front rather than tracked with an offset. The buffer holds at
        # most one partial message, so this copies kilobytes at worst -- and an offset
        # that is only compacted "sometimes" is how a long-lived reader grows without
        # bound.
        del self._buffer[:end]
        return body

    @property
    def pending(self) -> int:
        """Bytes held pending a complete frame.

        Non-zero between a partial read and the read that completes it. A value that
        only grows is a peer sending a prefix it never satisfies, which
        ``MAX_MESSAGE_BYTES`` bounds.
        """
        return len(self._buffer)

    def clear(self) -> None:
        self._buffer.clear()
