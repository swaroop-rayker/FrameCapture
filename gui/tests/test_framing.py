"""The wire format, from the Python side (SPEC.md §15.1).

Two implementations of one format can drift, so both are tested against the *same*
stated rules rather than against each other. Where a number appears here it is the
number SPEC.md §15.1 gives, not a number read out of the C++.
"""

from __future__ import annotations

import json

import pytest

from framecapture_gui.ipc.framing import (
    LENGTH_PREFIX_BYTES,
    MAX_MESSAGE_BYTES,
    FrameReader,
    FramingError,
    encode_frame,
    encode_message,
)


def test_the_length_prefix_is_four_bytes_little_endian() -> None:
    framed = encode_frame("hi")
    assert len(framed) == LENGTH_PREFIX_BYTES + 2
    # Asserted on the bytes rather than by round-tripping: a round trip passes just as
    # happily on big-endian, which is the thing §15.1 pins down.
    assert framed[:4] == b"\x02\x00\x00\x00"
    assert framed[4:] == b"hi"


def test_a_frame_round_trips() -> None:
    body = json.dumps({"cmd": "get_stats", "id": "7"}, separators=(",", ":"))
    reader = FrameReader()
    reader.feed(encode_frame(body))
    assert reader.next_frame() == body
    assert reader.next_frame() is None


def test_an_incomplete_frame_is_reported_as_incomplete_rather_than_as_an_error() -> None:
    """A partial read is the normal case on a stream, not a failure."""
    framed = encode_frame("abcdefgh")
    for prefix in range(len(framed)):
        reader = FrameReader()
        reader.feed(framed[:prefix])
        assert reader.next_frame() is None, f"a {prefix}-byte buffer was treated as complete"

    reader = FrameReader()
    reader.feed(framed)
    assert reader.next_frame() == "abcdefgh"


def test_a_message_over_the_limit_is_refused_by_the_sender() -> None:
    with pytest.raises(FramingError):
        encode_frame("x" * (MAX_MESSAGE_BYTES + 1))

    # Exactly at the limit is allowed. An off-by-one here would silently cap the
    # protocol one byte below what the spec says it carries.
    assert encode_frame("x" * MAX_MESSAGE_BYTES)


def test_an_oversized_declared_length_is_refused_before_anything_is_allocated() -> None:
    """A hostile prefix declaring ~4 GB must be an error, not a reservation."""
    reader = FrameReader()
    reader.feed(b"\xff\xff\xff\xff")
    with pytest.raises(FramingError):
        reader.next_frame()


def test_the_reader_splits_several_frames_out_of_one_read() -> None:
    stream = b"".join(encode_frame(body) for body in ('{"a":1}', '{"b":2}', '{"c":3}'))
    reader = FrameReader()
    reader.feed(stream)

    bodies = []
    while (body := reader.next_frame()) is not None:
        bodies.append(body)

    assert bodies == ['{"a":1}', '{"b":2}', '{"c":3}']
    assert reader.pending == 0, "the reader kept bytes it had already delivered"


def test_the_reader_reassembles_a_frame_split_across_reads() -> None:
    """The case the length prefix exists for."""
    framed = encode_frame('{"cmd":"hello"}')
    reader = FrameReader()

    reader.feed(framed[:3])  # not even the whole prefix
    assert reader.next_frame() is None
    reader.feed(framed[3:7])
    assert reader.next_frame() is None
    reader.feed(framed[7:])
    assert reader.next_frame() == '{"cmd":"hello"}'
    assert reader.pending == 0


def test_the_reader_does_not_grow_across_many_frames() -> None:
    """A reader that never compacts is an unbounded queue (CLAUDE.md hard rule 5)."""
    reader = FrameReader()
    framed = encode_frame("y" * 1000)
    for _ in range(5000):
        reader.feed(framed)
        assert reader.next_frame() is not None
    assert reader.pending == 0


def test_encode_message_emits_compact_json() -> None:
    framed = encode_message({"cmd": "hello", "id": "1"})
    body = framed[LENGTH_PREFIX_BYTES:].decode()
    assert " " not in body, "whitespace spends the 64 KB ceiling on nothing"
    assert json.loads(body) == {"cmd": "hello", "id": "1"}


def test_non_ascii_is_measured_in_bytes_not_characters() -> None:
    """The prefix counts UTF-8 bytes. A path with an umlaut in it is the ordinary case."""
    body = "café"  # 5 bytes, 4 characters
    framed = encode_frame(body)
    assert framed[:4] == b"\x05\x00\x00\x00"

    reader = FrameReader()
    reader.feed(framed)
    assert reader.next_frame() == body
