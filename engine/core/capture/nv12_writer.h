#pragma once

#include "core/error/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace fc::capture {

/// Appends raw NV12 frames to a file.
///
/// M2's output format: no container, no codec, just planar frames back to back --
/// which is what `ffplay -f rawvideo -pix_fmt nv12 -s WxH` expects. It exists so
/// capture and colour can be proven correct before the encoder and muxer are in the
/// picture, and so a failure at M3 can be bisected against a known-good source.
///
/// Buffered and flushed explicitly. Not thread-safe: one writer, one thread.
class Nv12Writer {
public:
    Nv12Writer();
    ~Nv12Writer();

    Nv12Writer(const Nv12Writer&) = delete;
    Nv12Writer& operator=(const Nv12Writer&) = delete;
    Nv12Writer(Nv12Writer&&) noexcept;
    Nv12Writer& operator=(Nv12Writer&&) noexcept;

    [[nodiscard]] Result<void> open(const std::filesystem::path& path, int width, int height);

    /// `frame` must be exactly `width * height * 3 / 2` bytes.
    [[nodiscard]] Result<void> write(std::span<const std::uint8_t> frame);

    /// Flushes and closes. Also called by the destructor, but call it explicitly
    /// when you care whether it succeeded.
    [[nodiscard]] Result<void> close();

    [[nodiscard]] std::uint64_t frames_written() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

    /// Bytes one frame must contain, for the caller to size its buffer.
    [[nodiscard]] static std::size_t frame_size(int width, int height) noexcept {
        return (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3) / 2;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::capture
