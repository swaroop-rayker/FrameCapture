#pragma once

// The control channel's wire format (SPEC.md §15.1).
//
//     4-byte little-endian length prefix + UTF-8 JSON body. Max message 64 KB.
//
// Deliberately a pure function over a byte buffer, with no pipe, no socket and no
// Windows headers: SPEC.md §20.1 lists the IPC contract as a unit-test target, and a
// framer that owns its transport can only be tested by standing one up.
//
// ---------------------------------------------------------------------------
// Why a length prefix at all, when the pipe is in message mode
// ---------------------------------------------------------------------------
// §15.1 specifies both. A message-mode named pipe already preserves boundaries, so the
// prefix looks redundant -- and it is not, for two reasons that only show up under
// failure. `ReadFile` on a message-mode pipe returns `ERROR_MORE_DATA` when the caller's
// buffer is smaller than the message, and the remainder is still queued; without a
// declared length the reader has no way to size the buffer up front and must either
// guess or loop with no idea when it is done. And a peer that writes in byte mode by
// mistake -- or a future transport that is not a pipe -- produces a stream that silently
// concatenates messages, which the prefix turns into a detectable error instead of a
// JSON parse failure ten messages later.
//
// So the prefix is the authority and the message mode is defence in depth.

#include "core/error/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::ipc {

/// SPEC.md §15.1's ceiling. Applies to the JSON body; the 4-byte prefix is on top.
///
/// It is a *limit*, not a buffer size -- `get_sources` on a machine with many windows
/// is the one response that could plausibly approach it, and exceeding it is reported
/// as `IPC_MESSAGE_TOO_LARGE` rather than truncated. A truncated JSON body is a parse
/// error at the far end, which names the wrong fault.
inline constexpr std::size_t kMaxMessageBytes = std::size_t{64} * 1024;

/// The length prefix's width. Little-endian, per §15.1.
inline constexpr std::size_t kLengthPrefixBytes = 4;

/// Prepends the length prefix to `body`.
///
/// Fails with `IPC_MESSAGE_TOO_LARGE` rather than emitting a frame the peer is obliged
/// to reject: the sender is the side that can still do something about it.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(std::string_view body);

/// What one `decode` step consumed and produced.
struct DecodedFrame {
    /// The JSON body, without the prefix.
    std::string body;
    /// Bytes consumed from the front of the buffer, prefix included.
    std::size_t consumed = 0;
};

/// Pulls one complete frame off the front of `buffer`.
///
/// Returns an empty `body` with `consumed == 0` when the buffer does not yet hold a
/// whole frame -- an incomplete read is the normal case on a stream, not an error, and
/// conflating the two would make every partial read log a failure.
///
/// Fails with `IPC_MESSAGE_TOO_LARGE` when the declared length exceeds
/// `kMaxMessageBytes`. That check is what stops a corrupt or hostile prefix from
/// becoming a 4 GB allocation, so it happens **before** anything is reserved.
[[nodiscard]] Result<DecodedFrame> decode_frame(std::span<const std::uint8_t> buffer);

/// Reassembles frames from a byte stream, holding whatever is left over.
///
/// The pipe is in message mode, so in practice each read yields exactly one frame --
/// but "in practice" is not a guarantee the reader can be written against, and
/// `ERROR_MORE_DATA` makes partial reads a documented outcome rather than a
/// hypothetical one.
class FrameReader {
public:
    /// Appends raw bytes as they came off the transport.
    void append(std::span<const std::uint8_t> bytes);

    /// Next complete frame, or `nullopt` when more bytes are needed.
    ///
    /// Call until it yields `nullopt`: one `append` can deliver several frames.
    [[nodiscard]] Result<std::optional<std::string>> next();

    /// Bytes held pending a complete frame. Non-zero between a partial read and the
    /// read that completes it; a value that only grows is a peer sending a prefix it
    /// never satisfies, which is what `kMaxMessageBytes` bounds.
    [[nodiscard]] std::size_t pending() const noexcept {
        return buffer_.size();
    }

    void clear() noexcept {
        buffer_.clear();
    }

private:
    std::vector<std::uint8_t> buffer_;
};

} // namespace fc::ipc
