#pragma once

// AAC-LC encoding for Tier A (SPEC.md §8.5).
//
// **Channel layout has to be signalled in three places**, and getting two of
// three right is the classic 5.1-plays-as-stereo bug (SPEC.md §20 row 17):
//
//   1. `AVCodecContext::ch_layout` -- what the encoder is told to produce.
//   2. The AAC `AudioSpecificConfig`, carried as codec extradata. This is what a
//      decoder reads first, and it overrides the container when they disagree.
//   3. The container's own fields, written by the muxer from `codecpar`.
//
// (2) only exists if the encoder is opened with `AV_CODEC_FLAG_GLOBAL_HEADER` --
// the same flag whose absence broke the MKV header on NVENC in M3 (BUG-006).
// Here the consequence is different and quieter: the file muxes fine and plays
// with the wrong speaker mapping.

#include "core/encode/video_encoder.h"
#include "core/error/result.h"
#include "core/ffmpeg/av_raii.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace fc::encode {

struct AudioEncoderSettings {
    int sample_rate = 48000;

    /// Pinned for the file's lifetime (SPEC.md §8.5). Owned by the caller and
    /// copied on `open`.
    AVChannelLayout layout{};

    /// 0 selects the SPEC.md §8.5 default for the layout: 192 stereo, 384 for
    /// 5.1, 512 for 7.1.
    int bitrate_kbps = 0;

    /// See the header comment. Both containers v1 targets need the parameter sets
    /// out of band.
    bool global_header = true;
};

/// AAC-LC encoder over libavcodec's native encoder.
///
/// Threading: driven from the `aenc` thread (SPEC.md §12). Not thread-safe.
class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();

    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;
    AacEncoder(AacEncoder&&) = delete;
    AacEncoder& operator=(AacEncoder&&) = delete;

    [[nodiscard]] Result<void> open(const AudioEncoderSettings& settings);

    /// Submits canonical-format samples, starting `offset` samples into `frame`.
    /// `frame` must be 48 kHz planar float in the pinned layout.
    ///
    /// AAC consumes a fixed 1024 samples per frame, so buffers are accumulated
    /// internally and emitted when full -- the caller does not have to align.
    ///
    /// Returns the number of samples consumed. A **short return** -- fewer than
    /// `frame->nb_samples - offset` -- means libavcodec's output queue is full and
    /// will not accept more input: drain it with `receive` and call again with
    /// `offset` advanced by the returned count. That is backpressure, not failure
    /// (BUG-011), and resuming from the returned offset rather than from the start
    /// is what stops the already-staged samples from being encoded twice
    /// (BUG-013).
    [[nodiscard]] Result<int> submit(const AVFrame* frame, int offset = 0);

    /// End of stream. Drain with `receive` afterwards.
    ///
    /// Returns `INTERNAL_QUEUE_FULL` when the trailing partial frame could not be
    /// handed over because output is pending; drain and call again. The flush is
    /// only marked done once it has actually succeeded, so the retry is safe.
    [[nodiscard]] Result<void> flush();

    [[nodiscard]] Result<std::optional<EncodedPacket>> receive();

    [[nodiscard]] const AVCodecContext* codec_context() const noexcept;

    /// Samples the encoder wants per call. 1024 for AAC-LC.
    [[nodiscard]] int frame_size() const noexcept;

    [[nodiscard]] std::uint64_t frames_submitted() const noexcept;
    [[nodiscard]] std::uint64_t packets_received() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::encode
