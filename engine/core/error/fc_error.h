#pragma once

#include "core/subsystem.h"

#include <span>
#include <string_view>

namespace fc {

// ---------------------------------------------------------------------------
// The single source of truth for the error domain (SPEC.md §19).
//
// One list generates the enum, the name table, and the message table, so those
// three can never drift apart. Adding a row here is the *only* way to add an
// error code.
//
// Rules for adding a row:
//   1. Codes are STABLE. They appear in logs, crash reports, and the IPC
//      protocol. Never renumber, never reuse a retired number.
//   2. The thousands digit is the subsystem: 1 capture, 2 gpu, 3 audio,
//      4 encode, 5 mux, 6 io, 7 ipc, 9 internal.
//   3. Every code must trace to a SPEC.md section. Cite it in the comment. A
//      code you cannot cite is a code you are guessing at.
//   4. Every code must get a row in docs/ERROR_CODES.md in the same commit.
//      `test_fc_error.ErrorCodesDocumentationIsComplete` enforces this.
//   5. The message is a short, lowercase, non-localised phrase. Remediation
//      belongs in ERROR_CODES.md, not here.
// ---------------------------------------------------------------------------

// clang-format off
#define FC_ERROR_LIST(X)                                                                                               \
    /* ----- no error -------------------------------------------------------------------------------------------- */  \
    X(NONE, 0, "no error")                                                                                             \
                                                                                                                       \
    /* ----- 1xxx capture (SPEC.md §4, §14.2, §20 rows 1/5/9) ---------------------------------------------------- */  \
    X(CAPTURE_INIT_FAILED,                 1001, "capture subsystem failed to initialise")                             \
    X(CAPTURE_BACKEND_UNAVAILABLE,         1002, "no usable capture backend")                                          \
    X(CAPTURE_TARGET_NOT_FOUND,            1003, "capture target does not exist")                                      \
    X(CAPTURE_TARGET_GONE,                 1004, "capture target disappeared mid-recording")                           \
    X(CAPTURE_TARGET_PROTECTED,            1005, "capture target is DRM-protected")                                    \
    X(CAPTURE_SOURCE_FORMAT_UNSUPPORTED,   1006, "source surface format is not supported")                             \
    X(CAPTURE_FRAME_TIMEOUT,               1007, "no frame delivered within the watchdog interval")                    \
    X(CAPTURE_RESOLUTION_CHANGED,          1008, "capture target resolution changed mid-recording")                     \
    X(CAPTURE_BLACK_FRAMES_DETECTED,       1009, "capture produced uniformly black frames")                            \
    X(WGC_UNSUPPORTED,                     1020, "Windows.Graphics.Capture is unavailable on this OS build")           \
    X(WGC_SESSION_CREATE_FAILED,           1021, "WGC capture session could not be created")                           \
    X(WGC_FRAME_POOL_FAILED,               1022, "WGC frame pool could not be created or was exhausted")               \
    X(DDA_UNSUPPORTED,                     1040, "DXGI Desktop Duplication is unavailable")                            \
    X(DDA_ACCESS_LOST,                     1041, "desktop duplication access was lost")                                \
    X(DDA_ADAPTER_AFFINITY,                1042, "duplication requested on an adapter that does not own the output")   \
    X(DDA_ACCESS_DENIED,                   1043, "desktop duplication was denied by the session")                      \
                                                                                                                       \
    /* ----- 2xxx gpu (SPEC.md §5, §6) ----------------------------------------------------------------------------- */\
    X(GPU_ADAPTER_ENUMERATION_FAILED,      2001, "DXGI adapter enumeration failed")                                    \
    X(GPU_NO_SUITABLE_ADAPTER,             2002, "no adapter satisfies the requirements")                              \
    X(GPU_OUTPUT_OWNERSHIP_UNRESOLVED,     2003, "could not determine which adapter owns the target output")           \
    X(GPU_DEVICE_CREATE_FAILED,            2004, "D3D11 device creation failed")                                       \
    X(GPU_DEVICE_REMOVED,                  2005, "the graphics device was removed")                                    \
    X(GPU_DEVICE_RESET,                    2006, "the graphics device was reset")                                      \
    X(GPU_DEVICE_HUNG,                     2007, "the graphics device stopped responding")                             \
    X(GPU_MIGRATION_FAILED,                2008, "migration to a different adapter failed")                            \
    X(GPU_SHADER_COMPILE_FAILED,           2009, "colour-conversion compute shader failed to compile")                 \
    X(GPU_CROSS_ADAPTER_TRANSFER_FAILED,   2010, "cross-adapter texture transfer failed")                              \
    X(GPU_FENCE_WAIT_FAILED,               2011, "waiting on a shared-texture fence failed")                           \
    X(GPU_TEXTURE_CREATE_FAILED,           2012, "D3D11 texture creation failed")                                      \
    X(GPU_HW_FRAMES_POOL_FAILED,           2013, "hardware frame pool allocation failed")                              \
    X(GPU_ENCODER_CAPABILITY_PROBE_FAILED, 2014, "encoder capability probe failed for this adapter")                   \
                                                                                                                       \
    /* ----- 3xxx audio (SPEC.md §8, §14.1, §20 rows 4/12/14-17) ------------------------------------------------- */  \
    X(AUDIO_INIT_FAILED,                   3001, "audio subsystem failed to initialise")                               \
    X(AUDIO_NO_RENDER_ENDPOINT,            3002, "no active audio render endpoint")                                    \
    X(AUDIO_ENDPOINT_ACTIVATE_FAILED,      3003, "audio endpoint activation failed")                                   \
    X(AUDIO_LOOPBACK_INIT_FAILED,          3004, "WASAPI loopback capture could not be started")                       \
    X(AUDIO_MIX_FORMAT_UNSUPPORTED,        3005, "endpoint mix format is not supported")                               \
    X(AUDIO_DEVICE_LOST,                   3006, "the audio device became unavailable")                                \
    X(AUDIO_RESAMPLER_INIT_FAILED,         3007, "libswresample context initialisation failed")                        \
    X(AUDIO_DRIFT_UNRECOVERABLE,           3008, "audio clock drift exceeded the correctable range")                   \
    X(AUDIO_SILENCE_GENERATOR_STALLED,     3009, "the silence generator stopped filling loopback gaps")                \
    X(AUDIO_CHANNEL_LAYOUT_UNSUPPORTED,    3010, "channel layout cannot be signalled in this container")               \
    X(AUDIO_BUFFER_OVERRUN,                3011, "the audio capture buffer overran")                                   \
    X(PROCESS_LOOPBACK_UNSUPPORTED,        3020, "per-process loopback requires Windows 10 20H1 or later")             \
    X(MULTITRACK_REQUIRES_MKV,             3021, "multi-track audio requires the Matroska container")                  \
    X(MULTITRACK_TRACK_LIMIT_EXCEEDED,     3022, "more audio tracks requested than the limit allows")                  \
    X(MULTITRACK_TARGET_PROCESS_GONE,      3023, "a multi-track target process exited")                                \
    X(MULTITRACK_TRACK_INIT_FAILED,        3024, "a per-process audio track failed to start")                          \
                                                                                                                       \
    /* ----- 4xxx encode (SPEC.md §2.2, §9, §13) ------------------------------------------------------------------ */\
    X(ENCODE_NO_HARDWARE_ENCODER,          4001, "no hardware video encoder is available")                             \
    X(ENCODE_ENCODER_OPEN_FAILED,          4002, "video encoder could not be opened")                                  \
    X(ENCODE_CODEC_UNSUPPORTED,            4003, "requested codec is not supported in this version")                   \
    X(ENCODE_LEVEL_EXCEEDED,               4004, "requested resolution or bitrate exceeds the codec level")            \
    X(ENCODE_SUBMIT_FAILED,                4005, "submitting a frame to the encoder failed")                           \
    X(ENCODE_RECEIVE_FAILED,               4006, "retrieving a packet from the encoder failed")                        \
    X(ENCODE_ENCODER_LOST,                 4007, "the encoder became invalid and must be recreated")                   \
    X(ENCODE_AUDIO_ENCODER_OPEN_FAILED,    4008, "AAC encoder could not be opened")                                    \
    X(ENCODE_ZERO_COPY_LOST,               4009, "the encoder is round-tripping frames through host memory")           \
    X(ENCODE_ALL_RUNGS_EXHAUSTED,          4010, "every rung of the degradation ladder has been exhausted")            \
    X(ENCODE_BITRATE_OUT_OF_RANGE,         4011, "requested bitrate is outside the supported range")                   \
                                                                                                                       \
    /* ----- 5xxx mux (SPEC.md §10, §11, §20 row 2) ---------------------------------------------------------------- */\
    X(MUX_OPEN_FAILED,                     5001, "output format context could not be opened")                          \
    X(MUX_CONTAINER_MISMATCH,              5002, "container does not match the requested output format")               \
    X(MUX_STREAM_ADD_FAILED,               5003, "adding a stream to the container failed")                            \
    X(MUX_WRITE_HEADER_FAILED,             5004, "writing the container header failed")                                \
    X(MUX_WRITE_PACKET_FAILED,             5005, "writing a packet failed")                                            \
    X(MUX_WRITE_TRAILER_FAILED,            5006, "writing the container trailer failed")                               \
    X(MUX_FINALIZE_FAILED,                 5007, "finalising the output file failed")                                  \
    X(MUX_REMUX_FAILED,                    5008, "fragmented-to-progressive remux failed")                             \
    X(MUX_RECOVERY_FAILED,                 5009, "recovering an unfinalised file failed")                              \
    X(MUX_SEGMENT_NOT_KEYFRAME_ALIGNED,    5010, "segment boundary is not keyframe-aligned")                           \
    X(MUX_VALIDATION_FAILED,               5011, "the output file failed post-recording validation")                   \
                                                                                                                       \
    /* ----- 6xxx io (SPEC.md §10.4, §13, §17, §19, §20 row 10) -------------------------------------------------- */  \
    X(IO_PATH_INVALID,                     6001, "the output path is not usable")                                      \
    X(IO_PERMISSION_DENIED,                6002, "access to the path was denied")                                      \
    X(IO_DIRECTORY_CREATE_FAILED,          6003, "could not create the target directory")                              \
    X(IO_FILE_OPEN_FAILED,                 6004, "could not open the file")                                            \
    X(IO_FILE_WRITE_FAILED,                6005, "writing to the file failed")                                         \
    X(IO_FILE_HANDLE_LOST,                 6006, "the output file handle became invalid")                              \
    X(IO_DISK_FULL,                        6007, "the target volume is out of space")                                  \
    X(IO_DISK_FULL_IMMINENT,               6008, "the target volume is about to run out of space")                     \
    X(IO_DISK_TOO_SLOW,                    6009, "the target volume cannot sustain the required write rate")           \
    X(IO_CONFIG_READ_FAILED,               6020, "the configuration file could not be read")                           \
    X(IO_CONFIG_PARSE_FAILED,              6021, "the configuration file is not valid TOML")                           \
    X(IO_CONFIG_MIGRATION_FAILED,          6022, "configuration schema migration failed")                              \
    X(IO_ATOMIC_REPLACE_FAILED,            6023, "atomic file replacement failed")                                     \
                                                                                                                       \
    /* ----- 7xxx ipc (SPEC.md §3.1, §15) -------------------------------------------------------------------------- */\
    X(IPC_PIPE_CREATE_FAILED,              7001, "the control pipe could not be created")                              \
    X(IPC_PIPE_CONNECT_FAILED,             7002, "connecting to the control pipe failed")                              \
    X(IPC_PIPE_BROKEN,                     7003, "the control pipe was broken")                                        \
    X(IPC_MESSAGE_MALFORMED,               7004, "a control message could not be parsed")                              \
    X(IPC_MESSAGE_TOO_LARGE,               7005, "a control message exceeded the size limit")                          \
    X(IPC_PROTOCOL_VERSION_MISMATCH,       7006, "the peer speaks an incompatible protocol version")                   \
    X(IPC_UNKNOWN_COMMAND,                 7007, "the peer sent an unrecognised command")                              \
    X(IPC_SHM_CREATE_FAILED,               7008, "the preview shared-memory region could not be created")              \
    X(IPC_SHM_MAP_FAILED,                  7009, "the preview shared-memory region could not be mapped")               \
    X(IPC_HEARTBEAT_TIMEOUT,               7010, "the peer stopped sending heartbeats")                                \
    X(IPC_PEER_GONE,                       7011, "the peer process exited")                                            \
    X(IPC_ENGINE_ALREADY_RUNNING,          7012, "another engine instance owns this user session")                     \
    X(IPC_JOB_OBJECT_FAILED,               7013, "the process job object could not be created or joined")              \
                                                                                                                       \
    /* ----- 9xxx internal (SPEC.md §12, §19) ---------------------------------------------------------------------- */\
    X(INTERNAL_INVARIANT_VIOLATED,         9001, "an internal invariant was violated")                                 \
    X(INTERNAL_NOT_IMPLEMENTED,            9002, "the requested operation is not implemented")                         \
    X(INTERNAL_INVALID_ARGUMENT,           9003, "an argument was invalid")                                            \
    X(INTERNAL_INVALID_STATE,              9004, "the operation is not valid in the current state")                     \
    X(INTERNAL_OUT_OF_MEMORY,              9005, "an allocation failed")                                               \
    X(INTERNAL_TIMEOUT,                    9006, "the operation timed out")                                            \
    X(INTERNAL_CANCELLED,                  9007, "the operation was cancelled")                                        \
    X(INTERNAL_QUEUE_FULL,                 9008, "a bounded queue was full and the item was dropped")                  \
    X(INTERNAL_THREAD_START_FAILED,        9009, "a worker thread could not be started")                               \
    X(INTERNAL_THREAD_JOIN_TIMEOUT,        9010, "a worker thread did not exit within the join timeout")               \
    X(INTERNAL_UNHANDLED_EXCEPTION,        9011, "an exception escaped to a thread entry point")                       \
    X(INTERNAL_UNKNOWN,                    9999, "an unclassified internal error occurred")
// clang-format on

/// Typed error domain. Numeric values are a stable external contract.
///
/// NOLINTBEGIN(readability-identifier-naming): these enumerators are
/// SCREAMING_SNAKE_CASE rather than the project's CamelCase because SPEC.md §20
/// row 16 specifies the spelling `FcError::MULTITRACK_REQUIRES_MKV` verbatim.
/// The spec wins over the style rule; the whole enum stays internally consistent.
enum class FcError : int {
#define FC_ERROR_DECLARE_ENUM(name, code, message) name = (code),
    FC_ERROR_LIST(FC_ERROR_DECLARE_ENUM)
#undef FC_ERROR_DECLARE_ENUM
};

// NOLINTEND(readability-identifier-naming)

struct FcErrorInfo {
    FcError error;
    int code;
    std::string_view name;    ///< The enumerator spelling, e.g. "CAPTURE_INIT_FAILED".
    std::string_view message; ///< Short non-localised phrase.
};

/// Numeric code. Equal to the underlying enum value; use this at boundaries
/// (logs, IPC, crash reports) rather than casting.
[[nodiscard]] constexpr int error_code(FcError error) noexcept {
    return static_cast<int>(error);
}

/// The enumerator spelling. "UNRECOGNISED" for a value not in the list.
[[nodiscard]] std::string_view error_name(FcError error) noexcept;

/// The short phrase from the table above.
[[nodiscard]] std::string_view error_message(FcError error) noexcept;

/// Owning subsystem, derived from the thousands digit.
[[nodiscard]] Subsystem subsystem_of(FcError error) noexcept;

/// True for codes that SPEC.md §1 permits to violate the prime directive: the
/// only two conditions under which a valid output file cannot be guaranteed.
/// Both must be detected pre-emptively, never discovered on a failed write.
[[nodiscard]] bool is_prime_directive_exception(FcError error) noexcept;

/// Every declared code, in declaration order. Used by the unit tests to assert
/// uniqueness, grouping, and documentation completeness.
[[nodiscard]] std::span<const FcErrorInfo> all_errors() noexcept;

/// Lookup by numeric code, for decoding a value that arrived over IPC or out of
/// a crash report. Returns `INTERNAL_UNKNOWN` if the code is not declared.
[[nodiscard]] FcError error_from_code(int code) noexcept;

} // namespace fc
