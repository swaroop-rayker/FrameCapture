// What the FFmpeg build does and does not contain (SPEC.md §0.2, §2.1, §13).
//
// CPU TIER. Every assertion here is a lookup against the linked libavcodec and
// libavformat, so none of it needs hardware — including the checks about hardware
// *encoders*, because "is `h264_amf` compiled in" is a different question from
// "does this machine have an AMD GPU".
//
// `ports/ffmpeg/framecapture-whitelist.cmake` starts from `--disable-everything`
// and re-enables a named list. That file is a policy document, and until now
// nothing checked that the policy survived contact with the build: a mistyped
// `--enable-encoder=` is a no-op that produces a working FFmpeg missing exactly one
// component, and the failure surfaces at runtime as a mux or encode error with no
// hint that the build is the cause.
//
// **The negative assertions matter more than the positive ones.** SPEC.md §0.2
// makes "no network capability" a hard non-goal, and §2.1 implements it by
// compiling no network code at all. That is a claim about the binary that is
// otherwise entirely unverified — a future `--enable-protocol=https` added to fix
// some unrelated download would silently give the engine a socket.

#include "core/ffmpeg/av_raii.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

[[nodiscard]] bool has_encoder(const char* name) {
    return avcodec_find_encoder_by_name(name) != nullptr;
}

[[nodiscard]] bool has_decoder(const char* name) {
    return avcodec_find_decoder_by_name(name) != nullptr;
}

[[nodiscard]] bool has_muxer(const char* name) {
    return av_guess_format(name, nullptr, nullptr) != nullptr;
}

[[nodiscard]] bool has_demuxer(const char* name) {
    return av_find_input_format(name) != nullptr;
}

/// Every protocol libavformat was built with, as a set of names.
[[nodiscard]] std::vector<std::string> protocols() {
    std::vector<std::string> names;
    void* opaque = nullptr;
    const char* name = nullptr;
    // Output protocols first, then input: a network capability in either direction
    // is a network capability.
    for (int output = 0; output <= 1; ++output) {
        opaque = nullptr;
        while ((name = avio_enum_protocols(&opaque, output)) != nullptr) {
            names.emplace_back(name);
        }
    }
    return names;
}

// ---------------------------------------------------------------------------
// SPEC.md §0.2 -- the engine has no network capability, by construction
// ---------------------------------------------------------------------------

TEST(CodecAvailability, TheOnlyProtocolIsLocalFileIo) {
    const std::vector<std::string> available = protocols();

    // `file` is required: `avio_open` cannot write the output without it, and a
    // muxer that cannot write is a violation of the prime directive.
    EXPECT_NE(std::ranges::find(available, "file"), available.end())
        << "the `file` protocol is missing; the muxer cannot write anything";

    // Everything else. Named individually rather than checked as "size == 1",
    // because a future build might legitimately gain `pipe` or `fd` and the useful
    // assertion is about *network*, not about the count.
    for (const char* forbidden : {"http", "https", "tcp", "udp", "rtmp", "rtmps", "rtp", "srt", "tls", "ftp", "sftp",
                                  "rtsp", "hls", "ws", "wss", "ipfs", "gopher"}) {
        EXPECT_EQ(std::ranges::find(available, forbidden), available.end())
            << "the engine was built with the `" << forbidden
            << "` protocol; SPEC.md §0.2 makes network capability a hard non-goal and §2.1 implements it by "
               "compiling none in";
    }
}

// The engine binds no sockets, so no networking muxer should exist to want one.
TEST(CodecAvailability, NoStreamingMuxersAreCompiledIn) {
    for (const char* forbidden : {"rtsp", "rtp", "hls", "dash", "flv"}) {
        EXPECT_FALSE(has_muxer(forbidden))
            << "the `" << forbidden << "` muxer is compiled in; streaming is a hard non-goal (SPEC.md §0.2)";
    }
}

// SPEC.md §0.2 again: no microphone, no webcam, no virtual camera. The engine owns
// WGC/DDA and WASAPI directly and libavdevice is not built at all.
TEST(CodecAvailability, NoCaptureDevicesAreCompiledIn) {
    for (const char* forbidden : {"dshow", "gdigrab", "vfwcap", "lavfi"}) {
        EXPECT_FALSE(has_demuxer(forbidden))
            << "the `" << forbidden << "` input device is compiled in (SPEC.md §0.2, §4)";
    }
}

// ---------------------------------------------------------------------------
// What the whitelist promises is present
// ---------------------------------------------------------------------------

TEST(CodecAvailability, EveryEncoderTheWhitelistNamesIsPresent) {
    // SPEC.md §5.2 -- selected per adapter at runtime, so both are compiled in
    // whichever adapter this machine happens to have.
    EXPECT_TRUE(has_encoder("h264_nvenc")) << "NVIDIA's encoder is missing (SPEC.md §5.2)";
    EXPECT_TRUE(has_encoder("h264_amf")) << "AMD's encoder is missing (SPEC.md §5.2)";

    // SPEC.md §2.1 -- AAC-LC for audio.
    EXPECT_TRUE(has_encoder("aac")) << "the native AAC encoder is missing (SPEC.md §8.5)";
}

// SPEC.md §13 rung 5: when no hardware encoder is available the ladder falls back
// to software, and without libx264 there is no rung 5 at all -- a machine whose
// encoders all fail simply stops recording.
//
// Added 2026-07-30 with the owner's decision to accept GPLv2 on the distribution
// (CLAUDE.md §9). This test is what makes the dependency's *presence* verifiable;
// the software submit path that uses it is M6.
TEST(CodecAvailability, TheSoftwareFallbackEncoderIsPresent) {
    const AVCodec* x264 = avcodec_find_encoder_by_name("libx264");
    ASSERT_NE(x264, nullptr) << "libx264 is not in this build, so SPEC.md §13 rung 5 has nothing to fall back to";

    // It has to accept the format the converter produces, or the fallback would
    // need a second conversion on the one path that is already the slow one.
    bool accepts_nv12 = false;
    const AVPixelFormat* formats = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, x264, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     reinterpret_cast<const void**>(&formats), &count) >= 0 &&
        formats != nullptr) {
        for (int i = 0; i < count; ++i) {
            if (formats[i] == AV_PIX_FMT_NV12) {
                accepts_nv12 = true;
                break;
            }
        }
    }
    EXPECT_TRUE(accepts_nv12) << "libx264 does not accept NV12; rung 5 would need an extra conversion";
}

TEST(CodecAvailability, EveryDecoderTheAcceptanceSuiteNeedsIsPresent) {
    // Not used while recording. Needed by the MP4 finalizer's stream copy
    // (SPEC.md §10.3) and by every test that decodes its own output to check it.
    EXPECT_TRUE(has_decoder("h264"));
    EXPECT_TRUE(has_decoder("aac"));
}

TEST(CodecAvailability, BothContainersAndTheirDemuxersArePresent) {
    EXPECT_TRUE(has_muxer("matroska")) << "SPEC.md §10.2";
    EXPECT_TRUE(has_muxer("mp4")) << "SPEC.md §10.3";
    // `mov` is the demuxer that reads MP4 back, which the fragmented -> progressive
    // remux depends on.
    EXPECT_TRUE(has_demuxer("mov,mp4,m4a,3gp,3g2,mj2") || has_demuxer("mov")) << "SPEC.md §10.3's remux reads this";
    EXPECT_TRUE(has_demuxer("matroska,webm") || has_demuxer("matroska"));
}

// v1 ships H.264 only (SPEC.md §9). The enum has three values and the capability
// probe reports all three, but the other two must not be *encodable* or a config
// change could silently produce a file the acceptance matrix never covered.
TEST(CodecAvailability, NoCodecBeyondH264IsEncodable) {
    for (const char* absent :
         {"hevc_nvenc", "hevc_amf", "av1_nvenc", "av1_amf", "libx265", "libaom-av1", "libsvtav1"}) {
        EXPECT_FALSE(has_encoder(absent))
            << "`" << absent << "` is compiled in; v1's acceptance matrix covers H.264 only (SPEC.md §9)";
    }
}

} // namespace
