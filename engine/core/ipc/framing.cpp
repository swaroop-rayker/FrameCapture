#include "core/ipc/framing.h"

#include <algorithm>

namespace fc::ipc {

Result<std::vector<std::uint8_t>> encode_frame(std::string_view body) {
    if (body.size() > kMaxMessageBytes) {
        return FcError::IPC_MESSAGE_TOO_LARGE;
    }

    std::vector<std::uint8_t> frame;
    frame.reserve(kLengthPrefixBytes + body.size());

    // Little-endian by hand rather than by memcpy of a `std::uint32_t`: SPEC.md §15.1
    // specifies the byte order, and reading it off the host's happens to work on x64
    // and is not what the spec says.
    const auto length = static_cast<std::uint32_t>(body.size());
    frame.push_back(static_cast<std::uint8_t>(length & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((length >> 16U) & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((length >> 24U) & 0xFFU));

    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

Result<DecodedFrame> decode_frame(std::span<const std::uint8_t> buffer) {
    if (buffer.size() < kLengthPrefixBytes) {
        return DecodedFrame{}; // not an error: the prefix has not fully arrived
    }

    const std::uint32_t length = static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8U) |
                                 (static_cast<std::uint32_t>(buffer[2]) << 16U) |
                                 (static_cast<std::uint32_t>(buffer[3]) << 24U);

    // Before any allocation, and before waiting for the body. A prefix declaring 4 GB
    // is either corruption or an attack, and in both cases the answer is to fail now
    // rather than to reserve first and find out afterwards.
    if (length > kMaxMessageBytes) {
        return FcError::IPC_MESSAGE_TOO_LARGE;
    }

    if (buffer.size() < kLengthPrefixBytes + length) {
        return DecodedFrame{}; // body still arriving
    }

    DecodedFrame decoded;
    const auto* start = reinterpret_cast<const char*>(buffer.data()) + kLengthPrefixBytes;
    decoded.body.assign(start, length);
    decoded.consumed = kLengthPrefixBytes + length;
    return decoded;
}

void FrameReader::append(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

Result<std::optional<std::string>> FrameReader::next() {
    FC_TRY_ASSIGN(const DecodedFrame decoded, decode_frame(buffer_));
    if (decoded.consumed == 0) {
        return std::optional<std::string>{};
    }

    // Erase from the front rather than keeping an offset. The buffer holds at most one
    // partial message by construction, so this moves kilobytes at worst, and an offset
    // that is only compacted "sometimes" is how a long-lived reader grows without
    // bound -- which CLAUDE.md hard rule 5 forbids for exactly this reason.
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
    return std::optional<std::string>{decoded.body};
}

} // namespace fc::ipc
