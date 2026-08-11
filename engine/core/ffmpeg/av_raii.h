#pragma once

// RAII ownership for the libav* objects the engine creates.
//
// CLAUDE.md §4: every FFmpeg object gets a purpose-built wrapper, and the tree
// holds zero raw owning pointers. Each libav* type has its own two-argument
// destructor convention -- `av_frame_free(&f)`, `avcodec_free_context(&c)`,
// `avformat_free_context(c)` (one argument, and *not* paired with the alloc that
// opened an output file) -- so this is a set of small explicit wrappers rather
// than one generic template. Getting the pairing wrong leaks or double-frees, and
// neither shows up in a functional test.
//
// The wrappers are move-only. An encoder or muxer owns exactly one of each, and a
// copy would mean two objects racing to free the same pointer.

// FFmpeg's headers carry no `extern "C"` guard of their own -- not one of them
// mentions __cplusplus -- so every C++ consumer has to supply it. Omitting it here
// does not fail to compile: the declarations simply acquire C++ linkage, and the
// mismatch only surfaces at link time as unresolved symbols with mangled names,
// in whichever executable happens to link first. Wrapping it once, here, is what
// keeps every downstream include correct by construction.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}

#include <string>
#include <utility>

namespace fc::ff {

/// `av_strerror` text for an AVERROR, for logs and error detail strings.
[[nodiscard]] std::string error_text(int averror);

/// Owns an `AVFrame*`. Freed with `av_frame_free`.
class Frame {
public:
    Frame() = default;

    explicit Frame(AVFrame* frame) noexcept : frame_(frame) {}

    ~Frame() {
        reset();
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    Frame(Frame&& other) noexcept : frame_(std::exchange(other.frame_, nullptr)) {}

    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            reset();
            frame_ = std::exchange(other.frame_, nullptr);
        }
        return *this;
    }

    /// Allocates an empty frame. False when out of memory.
    [[nodiscard]] bool alloc() {
        reset();
        frame_ = av_frame_alloc();
        return frame_ != nullptr;
    }

    void reset() noexcept {
        if (frame_ != nullptr) {
            av_frame_free(&frame_);
        }
    }

    /// Releases the frame's buffers but keeps the frame itself, so it can be
    /// refilled without a reallocation per frame (CLAUDE.md §4: no allocation on
    /// the hot path).
    void unref() noexcept {
        if (frame_ != nullptr) {
            av_frame_unref(frame_);
        }
    }

    [[nodiscard]] AVFrame* get() const noexcept {
        return frame_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return frame_ != nullptr;
    }

    AVFrame* operator->() const noexcept {
        return frame_;
    }

private:
    AVFrame* frame_ = nullptr;
};

/// Owns an `AVPacket*`. Freed with `av_packet_free`.
class Packet {
public:
    Packet() = default;

    ~Packet() {
        reset();
    }

    Packet(const Packet&) = delete;
    Packet& operator=(const Packet&) = delete;

    Packet(Packet&& other) noexcept : packet_(std::exchange(other.packet_, nullptr)) {}

    Packet& operator=(Packet&& other) noexcept {
        if (this != &other) {
            reset();
            packet_ = std::exchange(other.packet_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool alloc() {
        reset();
        packet_ = av_packet_alloc();
        return packet_ != nullptr;
    }

    void reset() noexcept {
        if (packet_ != nullptr) {
            av_packet_free(&packet_);
        }
    }

    void unref() noexcept {
        if (packet_ != nullptr) {
            av_packet_unref(packet_);
        }
    }

    [[nodiscard]] AVPacket* get() const noexcept {
        return packet_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return packet_ != nullptr;
    }

    AVPacket* operator->() const noexcept {
        return packet_;
    }

private:
    AVPacket* packet_ = nullptr;
};

/// Owns an `AVCodecContext*`. Freed with `avcodec_free_context`, which also closes
/// it -- there is no separate `avcodec_close` step in modern libavcodec.
class CodecContext {
public:
    CodecContext() = default;

    ~CodecContext() {
        reset();
    }

    CodecContext(const CodecContext&) = delete;
    CodecContext& operator=(const CodecContext&) = delete;

    CodecContext(CodecContext&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

    CodecContext& operator=(CodecContext&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool alloc(const AVCodec* codec) {
        reset();
        ctx_ = avcodec_alloc_context3(codec);
        return ctx_ != nullptr;
    }

    void reset() noexcept {
        if (ctx_ != nullptr) {
            avcodec_free_context(&ctx_);
        }
    }

    [[nodiscard]] AVCodecContext* get() const noexcept {
        return ctx_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ctx_ != nullptr;
    }

    AVCodecContext* operator->() const noexcept {
        return ctx_;
    }

private:
    AVCodecContext* ctx_ = nullptr;
};

/// Owns an `AVBufferRef*` -- hardware device and hardware frames contexts.
/// Unreffed with `av_buffer_unref`.
class BufferRef {
public:
    BufferRef() = default;

    explicit BufferRef(AVBufferRef* ref) noexcept : ref_(ref) {}

    ~BufferRef() {
        reset();
    }

    BufferRef(const BufferRef&) = delete;
    BufferRef& operator=(const BufferRef&) = delete;

    BufferRef(BufferRef&& other) noexcept : ref_(std::exchange(other.ref_, nullptr)) {}

    BufferRef& operator=(BufferRef&& other) noexcept {
        if (this != &other) {
            reset();
            ref_ = std::exchange(other.ref_, nullptr);
        }
        return *this;
    }

    void reset() noexcept {
        if (ref_ != nullptr) {
            av_buffer_unref(&ref_);
        }
    }

    /// A new reference to the same underlying buffer, for handing to a codec
    /// context that will own its own reference.
    [[nodiscard]] AVBufferRef* new_ref() const {
        return ref_ != nullptr ? av_buffer_ref(ref_) : nullptr;
    }

    /// Address of the raw pointer, for the libav* calls that allocate in place
    /// (`av_hwdevice_ctx_create`). Resets any existing reference first, so the
    /// call cannot silently overwrite one.
    [[nodiscard]] AVBufferRef** address_for_alloc() noexcept {
        reset();
        return &ref_;
    }

    [[nodiscard]] AVBufferRef* get() const noexcept {
        return ref_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ref_ != nullptr;
    }

private:
    AVBufferRef* ref_ = nullptr;
};

/// Owns an `AVDictionary*` of muxer/encoder options.
///
/// The libav* convention is that a successful `avcodec_open2` / `avformat_*`
/// *consumes* the entries it recognises and leaves the rest, so `remaining()`
/// after a call is the set of options the component silently ignored. That is
/// worth logging: a mistyped private option otherwise disappears without a word.
class Dictionary {
public:
    Dictionary() = default;

    ~Dictionary() {
        reset();
    }

    Dictionary(const Dictionary&) = delete;
    Dictionary& operator=(const Dictionary&) = delete;

    Dictionary(Dictionary&& other) noexcept : dict_(std::exchange(other.dict_, nullptr)) {}

    Dictionary& operator=(Dictionary&& other) noexcept {
        if (this != &other) {
            reset();
            dict_ = std::exchange(other.dict_, nullptr);
        }
        return *this;
    }

    /// Returns the AVERROR from `av_dict_set`, or 0.
    int set(const char* key, const char* value) {
        return av_dict_set(&dict_, key, value, 0);
    }

    int set(const char* key, std::int64_t value) {
        return av_dict_set_int(&dict_, key, value, 0);
    }

    void reset() noexcept {
        if (dict_ != nullptr) {
            av_dict_free(&dict_);
        }
    }

    /// `"key=value, key=value"` for whatever is still in the dictionary.
    [[nodiscard]] std::string remaining() const;

    /// Address of the raw pointer, for the libav* calls that take `AVDictionary**`
    /// and consume entries from it.
    [[nodiscard]] AVDictionary** address() noexcept {
        return &dict_;
    }

    [[nodiscard]] AVDictionary* get() const noexcept {
        return dict_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return dict_ == nullptr || av_dict_count(dict_) == 0;
    }

private:
    AVDictionary* dict_ = nullptr;
};

/// Owns a `SwrContext*` -- the resampler that converts the endpoint's mix format
/// to the engine's canonical 48 kHz planar float (SPEC.md §8.1).
///
/// Allocation is the odd one out: `swr_alloc_set_opts2` writes through an
/// out-parameter and returns an AVERROR, rather than returning the pointer like
/// `avcodec_alloc_context3`. It also *reuses* an existing context if the pointer
/// is non-null, so handing it a live one silently reconfigures rather than
/// replaces -- `alloc` resets first so that cannot happen by accident.
class SwrContextRef {
public:
    SwrContextRef() = default;

    ~SwrContextRef() {
        reset();
    }

    SwrContextRef(const SwrContextRef&) = delete;
    SwrContextRef& operator=(const SwrContextRef&) = delete;

    SwrContextRef(SwrContextRef&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

    SwrContextRef& operator=(SwrContextRef&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    /// Wraps `swr_alloc_set_opts2`. Returns its AVERROR, or 0. Does **not** call
    /// `swr_init` -- the caller may still need to set options (notably
    /// `swr_set_compensation` for SPEC.md §8.4's soft resync) before init.
    [[nodiscard]] int alloc(const AVChannelLayout* out_layout, AVSampleFormat out_format, int out_rate,
                            const AVChannelLayout* in_layout, AVSampleFormat in_format, int in_rate) {
        reset();
        return swr_alloc_set_opts2(&ctx_, out_layout, out_format, out_rate, in_layout, in_format, in_rate, 0, nullptr);
    }

    void reset() noexcept {
        if (ctx_ != nullptr) {
            swr_free(&ctx_);
        }
    }

    [[nodiscard]] SwrContext* get() const noexcept {
        return ctx_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ctx_ != nullptr;
    }

private:
    SwrContext* ctx_ = nullptr;
};

/// Owns an *input* `AVFormatContext*`.
///
/// Separate from `OutputFormatContext` because the two have different destructors
/// that are easy to confuse: an input context is released with
/// `avformat_close_input`, which also closes its AVIO handle, while an output
/// context needs `avio_closep` first and then `avformat_free_context`. Calling the
/// output pairing on an input context leaks the file handle.
class InputFormatContext {
public:
    InputFormatContext() = default;

    ~InputFormatContext() {
        reset();
    }

    InputFormatContext(const InputFormatContext&) = delete;
    InputFormatContext& operator=(const InputFormatContext&) = delete;

    InputFormatContext(InputFormatContext&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

    InputFormatContext& operator=(InputFormatContext&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    /// Wraps `avformat_open_input`. Returns its AVERROR, or 0.
    [[nodiscard]] int open(const char* filename) {
        reset();
        return avformat_open_input(&ctx_, filename, nullptr, nullptr);
    }

    void reset() noexcept {
        if (ctx_ != nullptr) {
            avformat_close_input(&ctx_);
        }
    }

    [[nodiscard]] AVFormatContext* get() const noexcept {
        return ctx_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ctx_ != nullptr;
    }

    AVFormatContext* operator->() const noexcept {
        return ctx_;
    }

private:
    AVFormatContext* ctx_ = nullptr;
};

/// Owns an output `AVFormatContext*`.
///
/// Output contexts are the asymmetric case and the easiest to leak. `avformat_free_context`
/// releases the context but **not** the `AVIOContext` opened by `avio_open2`; that
/// needs `avio_closep` first, and only when `AVFMT_NOFILE` is clear. Doing it in
/// the wrong order leaves the file handle open, which on Windows means the output
/// cannot be moved or deleted -- and the MP4 finalizer in M5 does exactly that.
class OutputFormatContext {
public:
    OutputFormatContext() = default;

    ~OutputFormatContext() {
        reset();
    }

    OutputFormatContext(const OutputFormatContext&) = delete;
    OutputFormatContext& operator=(const OutputFormatContext&) = delete;

    OutputFormatContext(OutputFormatContext&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

    OutputFormatContext& operator=(OutputFormatContext&& other) noexcept {
        if (this != &other) {
            reset();
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    /// Wraps `avformat_alloc_output_context2`. Returns its AVERROR, or 0.
    [[nodiscard]] int alloc(const char* format_name, const char* filename) {
        reset();
        return avformat_alloc_output_context2(&ctx_, nullptr, format_name, filename);
    }

    /// Closes the AVIO handle if one is open, then frees the context. Safe to call
    /// more than once, which matters because finalization is required to be
    /// idempotent (SPEC.md §10.4).
    void reset() noexcept {
        if (ctx_ == nullptr) {
            return;
        }
        close_io();
        avformat_free_context(ctx_);
        ctx_ = nullptr;
    }

    /// Closes just the AVIO handle, leaving the context readable. This is the step
    /// that actually releases the file on disk.
    void close_io() noexcept {
        if (ctx_ != nullptr && ctx_->pb != nullptr && (ctx_->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&ctx_->pb);
        }
    }

    [[nodiscard]] AVFormatContext* get() const noexcept {
        return ctx_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ctx_ != nullptr;
    }

    AVFormatContext* operator->() const noexcept {
        return ctx_;
    }

private:
    AVFormatContext* ctx_ = nullptr;
};

} // namespace fc::ff
