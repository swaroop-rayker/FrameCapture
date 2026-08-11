# FrameCapture FFmpeg component whitelist
#
# Consumed by the overlay portfile.cmake. Sets FRAMECAPTURE_FFMPEG_OPTIONS.
#
# Policy (SPEC.md §2.1): start from `--disable-everything` and re-enable only the
# components v1 actually ships. Two consequences we want:
#   1. No network egress is possible from the engine process, because no network
#      code is compiled in at all (SPEC.md §0.2).
#   2. Binary size and attack surface track the feature set, not FFmpeg's
#      catalogue of ~2000 components.
#
# Adding a line here is a spec change. Cite the section in the comment.

set(_fc_opts "")

# ---------------------------------------------------------------------------
# Baseline: nothing is enabled unless it appears below.
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --disable-everything)

# ---------------------------------------------------------------------------
# Network: removed at compile time. SPEC.md §0.2 / §2.1.
#
# `--disable-protocols` also removes the `file` protocol, which libavformat
# needs for avio_open() on the output path -- without it the muxer cannot write
# anything at all, which would violate the prime directive (CLAUDE.md §1). We
# therefore disable the whole protocol list and re-enable `file` alone. This is
# the standard "no protocols except local file I/O" idiom and grants no network
# capability: `--disable-network` has already removed every socket-using
# protocol, the TLS layer, and the network utility code from the build.
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --disable-network)
list(APPEND _fc_opts --disable-protocols --enable-protocol=file)

# ---------------------------------------------------------------------------
# Capture/playback devices: the engine owns WGC/DDA and WASAPI directly.
# SPEC.md §4, §8. avdevice is not even built (see the manifest feature list).
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --disable-devices --disable-indevs --disable-outdevs)

# ---------------------------------------------------------------------------
# Encoders. H.264 High @ L4.2 only in v1 (SPEC.md §2.2 item 4), AAC-LC for audio
# (SPEC.md §2.1). Hardware encoders are selected per-adapter at runtime
# (SPEC.md §5.2); both are compiled in because either adapter may win.
#
# libx264 is present as of 2026-07-30, by the owner's decision. It is the
# software encoder degradation-ladder rung 5 (SPEC.md §13) falls back to when no
# hardware encoder is available, and without it a machine whose encoders all fail
# has no way to keep recording.
#
# **It makes the distributed binary GPLv2.** libx264 is GPLv2, `--enable-gpl`
# makes the whole FFmpeg build GPL, and linking that into FrameCapture puts the
# distribution under GPLv2 as well. That is the accepted cost of rung 5, recorded
# here and in CLAUDE.md §9 so nobody has to rediscover why the flag is set.
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --enable-gpl)                  # required by libx264; see above
list(APPEND _fc_opts --enable-libx264)
list(APPEND _fc_opts --enable-encoder=libx264)      # software fallback, SPEC.md §13 rung 5
list(APPEND _fc_opts --enable-encoder=h264_nvenc)   # NVIDIA, SPEC.md §5.2
list(APPEND _fc_opts --enable-encoder=h264_amf)     # AMD, SPEC.md §5.2
list(APPEND _fc_opts --enable-encoder=aac)          # native AAC-LC, SPEC.md §2.1

# ---------------------------------------------------------------------------
# Decoders. Not used by the recording path. Required by the MP4 finalizer's
# stream copy (SPEC.md §10.3) and by the acceptance suite, which decodes its own
# output to verify colour and frame content (SPEC.md §20).
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --enable-decoder=h264)
list(APPEND _fc_opts --enable-decoder=aac)

# ---------------------------------------------------------------------------
# Containers. MKV is the recommended default, MP4 the crash-safe fragmented
# path. SPEC.md §10.2, §10.3. `mov` is the demuxer that reads MP4, needed by the
# fragmented -> progressive remux on clean stop.
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --enable-muxer=mp4 --enable-muxer=matroska)
list(APPEND _fc_opts --enable-demuxer=mov --enable-demuxer=matroska)

# ---------------------------------------------------------------------------
# Parsers and bitstream filters. libavformat inserts these implicitly; a missing
# one surfaces as a mux failure at runtime, not a link error.
# ---------------------------------------------------------------------------
list(APPEND _fc_opts --enable-parser=h264 --enable-parser=aac)
list(APPEND _fc_opts --enable-bsf=extract_extradata)   # encoders that emit in-band SPS/PPS
list(APPEND _fc_opts --enable-bsf=h264_mp4toannexb)    # AVCC -> Annex B on remux
list(APPEND _fc_opts --enable-bsf=aac_adtstoasc)       # ADTS -> ASC for MP4/MKV
list(APPEND _fc_opts --enable-bsf=null)                # identity path for stream copy

list(JOIN _fc_opts " " FRAMECAPTURE_FFMPEG_OPTIONS)
unset(_fc_opts)
