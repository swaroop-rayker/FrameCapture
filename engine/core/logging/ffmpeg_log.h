#pragma once

namespace fc::log {

/// Routes libav* diagnostics into the FrameCapture logger.
///
/// FFmpeg writes to stderr by default, which bypasses every sink SPEC.md §18
/// mandates: such lines never reach the rotating file, never reach the crash ring
/// buffer, and never carry a session id. They also pollute the engine's own output
/// -- the encoder capability probe alone emits "AMF via D3D11." per adapter.
///
/// Maps FFmpeg levels onto ours and tags everything `subsystem=encode`. Idempotent.
void install_ffmpeg_bridge();

/// Restores FFmpeg's default callback.
void remove_ffmpeg_bridge();

} // namespace fc::log
