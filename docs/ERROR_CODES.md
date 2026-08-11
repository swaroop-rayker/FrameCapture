# Error Codes

Every `FcError` defined in `engine/core/error/fc_error.h`, with its cause and
remediation (SPEC.md §19, §22).

`test_fc_error.ErrorCodesDocumentationIsComplete` fails the build if a declared
code is missing from this file, so this table cannot silently fall behind the enum.

## Contract

- **Codes are stable.** They appear in log lines, `crash_report.json`, and the IPC
  protocol. A code is never renumbered and a retired number is never reused.
- **The thousands digit is the subsystem:** 1xxx capture, 2xxx gpu, 3xxx audio,
  4xxx encode, 5xxx mux, 6xxx io, 7xxx ipc, 9xxx internal. 8xxx is unassigned.
- **Adding a code** means adding a row to `FC_ERROR_LIST` *and* a row here, in the
  same commit.

## Log signature

Every code shares one signature, emitted by `LogFields::add_error`:

```
<ISO-8601 UTC> | <level> | <thread> | <subsystem> | <session_id> | <message> error=<NAME> code=<NNNN>
```

So `code=2005` finds every device-removal event in a user's log, and
`error=MULTITRACK_REQUIRES_MKV` finds every rejected multi-track request. Where a
code carries extra diagnostic keys, they are named in its row.

`FC_HR` failures add `expr=`, `file=`, `line=`, `function=`, and `hr=` (the
HRESULT as `0x……… (SYMBOL): system message`).

## Prime-directive exceptions

SPEC.md §1 permits exactly two conditions to result in no playable output file:
**6007 `IO_DISK_FULL`** and **6006 `IO_FILE_HANDLE_LOST`**. Both must be detected
pre-emptively. `is_prime_directive_exception()` returns true for these two and
nothing else, and a unit test pins that set — widening it is a deliberate change to
the project's central guarantee, not an incidental consequence of adding a code.

---

## 0 — No error

| Code | Name | Meaning | Cause | Remediation |
|---|---|---|---|---|
| 0 | `NONE` | No error. | Returned by `hresult_to_fc_error()` for a successful HRESULT. | Not an error. Never stored in a `Result`'s error state — a `Result` in the value state carries no code at all. |

---

## 1xxx — Capture (SPEC.md §4, §14.2, §20 rows 1/5/9)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 1001 | `CAPTURE_INIT_FAILED` | Capture subsystem failed to initialise. | Generic bring-up failure after backend selection succeeded. | Check the preceding `ERROR` line for the specific cause; this is the roll-up, not the diagnosis. |
| 1002 | `CAPTURE_BACKEND_UNAVAILABLE` | No usable capture backend. | Both WGC (1020) and DDA (1040) were rejected — unsupported OS build combined with a session that denies duplication. | Requires Windows 10 1903+ for DDA or 2004+ for WGC. Check for an active Remote Desktop or secure-desktop session. |
| 1003 | `CAPTURE_TARGET_NOT_FOUND` | Capture target does not exist. | The configured monitor or window handle did not resolve at start. Adds `target=`. | Re-select the source. Stale window handles from a previous session are the usual cause. |
| 1004 | `CAPTURE_TARGET_GONE` | Capture target disappeared mid-recording. | The captured window closed, or the monitor was detached. | Recording continues per §14.2; the file stays valid. Re-select a source to resume live capture. |
| 1005 | `CAPTURE_TARGET_PROTECTED` | Capture target is DRM-protected. | The surface is output-protected (Netflix, some players). This is the second cause of all-black video in §20 row 1. | Not recoverable — the OS refuses to hand over the pixels. Surface a user-facing explanation rather than recording black frames. |
| 1006 | `CAPTURE_SOURCE_FORMAT_UNSUPPORTED` | Source surface format is not supported. | WGC delivered a format the pipeline cannot interpret. Adds `dxgi_format=`. | If the format is `R16G16B16A16Float`, system HDR is on and the tone-map path should have engaged (§4.2); reinterpreting those bits as BGRA8 is the black/neon-green bug. |
| 1007 | `CAPTURE_FRAME_TIMEOUT` | No frame delivered within the watchdog interval. | No frame for 3 × frame interval — a stalled compositor, or a blocked capture thread. §20 row 9. | The watchdog forces a session rebuild. Persistent occurrences mean a downstream consumer is blocking the capture thread, which CLAUDE.md §4 forbids. |
| 1008 | `CAPTURE_RESOLUTION_CHANGED` | Capture target resolution changed mid-recording. | Display mode change, or a captured window resized. Adds `from=`, `to=`. | Handled per §14.3: the encoder keeps its original geometry and the source is scaled. Not a failure. |
| 1009 | `CAPTURE_BLACK_FRAMES_DETECTED` | Capture produced uniformly black frames. | Post-init validation gate for §20 row 1: adapter/output mismatch (1042), protected content (1005), or a misread HDR format (1006). | Diagnostic, raised deliberately so a black recording fails loudly instead of completing. Check adapter affinity first. |
| 1020 | `WGC_UNSUPPORTED` | `Windows.Graphics.Capture` is unavailable on this OS build. | Windows 10 older than build 19041. | Expected on older builds; the engine falls back to DDA (§4.3). Informational, not fatal. |
| 1021 | `WGC_SESSION_CREATE_FAILED` | WGC capture session could not be created. | `GraphicsCaptureItem` creation was refused, or the target became invalid between selection and start. | Falls back to DDA. Persistent failure on a valid target suggests a graphics driver problem. |
| 1022 | `WGC_FRAME_POOL_FAILED` | WGC frame pool could not be created or was exhausted. | Frames are not being returned to the pool fast enough — a stalled downstream stage. §20 row 9. | Bounded queues plus the degradation ladder (§13) should absorb this. Repeated occurrences indicate the convert or encode stage is the bottleneck. |
| 1040 | `DDA_UNSUPPORTED` | DXGI Desktop Duplication is unavailable. | `DuplicateOutput` unsupported on this adapter/output combination. | Only relevant when WGC has already been rejected. Together with 1020 this yields 1002. |
| 1041 | `DDA_ACCESS_LOST` | Desktop duplication access was lost. | `DXGI_ERROR_ACCESS_LOST` — mode change, a fullscreen-exclusive app taking over, or a secure-desktop transition. §5.4. | Rebuild the duplication session. Routine and expected; must not end the recording. |
| 1042 | `DDA_ADAPTER_AFFINITY` | Duplication requested on an adapter that does not own the output. | **The all-black-video bug.** On a MUX-less laptop the panel is wired to the iGPU; a device created on the dGPU gets `S_OK` from `DuplicateOutput` and no valid content. §5.1. | Map `HMONITOR` → `IDXGIOutput` → parent `IDXGIAdapter` and create the capture device on *that* adapter. Never assume adapter 0. |
| 1043 | `DDA_ACCESS_DENIED` | Desktop duplication was denied by the session. | `DXGI_ERROR_ACCESS_DENIED` — the secure desktop (UAC prompt, lock screen) is active. | Transient. Retry once the secure desktop is dismissed; inject duplicate frames meanwhile so the timeline stays continuous. |

---

## 2xxx — GPU (SPEC.md §5, §6)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 2001 | `GPU_ADAPTER_ENUMERATION_FAILED` | DXGI adapter enumeration failed. | `CreateDXGIFactory1` or `EnumAdapters1` failed. | Indicates a broken graphics stack; nothing the engine can work around. Report the HRESULT. |
| 2002 | `GPU_NO_SUITABLE_ADAPTER` | No adapter satisfies the requirements. | Every adapter was software-only (WARP), or none supports the required feature level. | A hardware adapter with D3D11 feature level 11_0 is a hard requirement. |
| 2003 | `GPU_OUTPUT_OWNERSHIP_UNRESOLVED` | Could not determine which adapter owns the target output. | The `HMONITOR` → output → adapter walk failed. The precondition for avoiding 1042. | Do not fall back to adapter 0 — that is the black-frame bug. Fail and report instead. |
| 2004 | `GPU_DEVICE_CREATE_FAILED` | D3D11 device creation failed. | `D3D11CreateDevice` failed on the selected adapter. Adds `adapter=`. | With an explicit adapter, the driver type must be `D3D_DRIVER_TYPE_UNKNOWN`; passing `HARDWARE` is a common cause. |
| 2005 | `GPU_DEVICE_REMOVED` | The graphics device was removed. | `DXGI_ERROR_DEVICE_REMOVED` — driver update, TDR, external GPU unplugged, or a hybrid-GPU switch. §5.4, §20 row 11. | Run the §5.4 migration procedure: re-enumerate, rebuild the device and encoder, keep one continuous output file. Target gap < 350 ms. |
| 2006 | `GPU_DEVICE_RESET` | The graphics device was reset. | `DXGI_ERROR_DEVICE_RESET`, usually following a TDR. | Same migration path as 2005. |
| 2007 | `GPU_DEVICE_HUNG` | The graphics device stopped responding. | `DXGI_ERROR_DEVICE_HUNG` — a driver-level hang, frequently caused by another application. | Rebuild the device. If it recurs, degrade per §13 to reduce GPU load. |
| 2008 | `GPU_MIGRATION_FAILED` | Migration to a different adapter failed. | The §5.4 recovery path could not bring up a device on any remaining adapter. | The recording must still be finalised into a playable file — the prime directive holds. Stop cleanly rather than crashing. |
| 2009 | `GPU_SHADER_COMPILE_FAILED` | Colour-conversion compute shader failed to compile. | The BGRA→NV12 compute shader (§6) failed at runtime compilation. Adds `shader=`. | A build or driver problem, not a user problem. The CPU conversion fallback exists but costs ~1.5 cores at 1080p60. |
| 2010 | `GPU_CROSS_ADAPTER_TRANSFER_FAILED` | Cross-adapter texture transfer failed. | The §5.3 staging path failed — shared-handle creation or the keyed mutex. | Only active when encode rule 2 fires. Prefer encoding where the pixels already live (§5.2). |
| 2011 | `GPU_FENCE_WAIT_FAILED` | Waiting on a shared-texture fence failed. | A fence or keyed-mutex wait failed or timed out. Reading the texture anyway produces §20 row 5's torn frames. | Never `Map` or read a shared texture without a successful wait. Drop the frame instead. |
| 2012 | `GPU_TEXTURE_CREATE_FAILED` | D3D11 texture creation failed. | `CreateTexture2D` rejected the descriptor. Adds `format=`, `bind_flags=`. | `E_INVALIDARG` names no field; probe the accepted combination rather than guessing. See BUG-001. |
| 2013 | `GPU_HW_FRAMES_POOL_FAILED` | Hardware frame pool allocation failed. | `av_hwframe_ctx_init` failed for the `AV_PIX_FMT_D3D11` encoder-input pool. | **BUG-001:** FFmpeg passes `AVD3D11VAFramesContext.BindFlags` through untouched, and its default of `0` is rejected for NV12 arrays on the Radeon 780M — only `D3D11_BIND_DECODER` was accepted there. Resolve per adapter; see `docs/ENGINEERING_LOG.md`. |
| 2014 | `GPU_ENCODER_CAPABILITY_PROBE_FAILED` | Encoder capability probe failed for this adapter. | The per-codec per-adapter probe (§2.2 item 4) could not complete. | Treat the codec as unsupported on that adapter rather than assuming it works. |

---

## 3xxx — Audio (SPEC.md §8, §14.1, §20 rows 4/12/14–17)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 3001 | `AUDIO_INIT_FAILED` | Audio subsystem failed to initialise. | Roll-up for a bring-up failure. | Check the preceding `ERROR` line. |
| 3002 | `AUDIO_NO_RENDER_ENDPOINT` | No active audio render endpoint. | No default `eRender` device — every output disabled or unplugged. | Recording proceeds video-only; the silence generator (§8.2) keeps the timeline intact so nothing desyncs. |
| 3003 | `AUDIO_ENDPOINT_ACTIVATE_FAILED` | Audio endpoint activation failed. | `IMMDevice::Activate` failed, or the audio service is not running. Adds `endpoint=`. | Check that Windows Audio is running. Retry on the next device-change notification. |
| 3004 | `AUDIO_LOOPBACK_INIT_FAILED` | WASAPI loopback capture could not be started. | `IAudioClient::Initialize` with `AUDCLNT_STREAMFLAGS_LOOPBACK` failed. | Loopback requires a render endpoint in shared mode. Another application holding it exclusively is the usual cause. |
| 3005 | `AUDIO_MIX_FORMAT_UNSUPPORTED` | Endpoint mix format is not supported. | The endpoint reports a format beyond 7.1 or a non-PCM encoding. Adds `channels=`, `sample_rate=`, `bits=`. Also logged once, with `frames=`, when a captured buffer arrives without the frame size that says how to read its bytes (BUG-031) — that buffer is filled with silence rather than guessed at. | Tier A supports up to 7.1 (§8.5). Resample or downmix rather than failing the recording. |
| 3006 | `AUDIO_DEVICE_LOST` | The audio device became unavailable. | `AUDCLNT_E_DEVICE_INVALIDATED` — the endpoint was removed or reconfigured. §14.1. | Rebuild on the new default endpoint via `IMMNotificationClient`, padding the gap with silence. Must not shorten the timeline (§20 row 12). |
| 3007 | `AUDIO_RESAMPLER_INIT_FAILED` | libswresample context initialisation failed. | `swr_init` failed for the requested conversion. Adds `in_format=`, `out_format=`. | Usually an unsupported channel-layout pair. Verify the layout is fully specified, not just a channel count. |
| 3008 | `AUDIO_DRIFT_UNRECOVERABLE` | Audio clock drift exceeded the correctable range. | The device clock diverged beyond what `swr_set_compensation` can absorb (§8.4). Adds `drift_ms=`. | Hard-resync by inserting or dropping silence at a buffer boundary. §20 row 4 requires \|offset\| < 20 ms. |
| 3009 | `AUDIO_SILENCE_GENERATOR_STALLED` | The silence generator stopped filling loopback gaps. | The §8.2 watchdog missed its deadline. | **The single most damaging audio bug.** WASAPI loopback emits nothing while no audio plays; without silence injection you get 22 minutes of audio in a 30-minute file and everything after the first gap is desynced. Treat as critical. |
| 3010 | `AUDIO_CHANNEL_LAYOUT_UNSUPPORTED` | Channel layout cannot be signalled in this container. | The layout has no representation in the target container. Adds `layout=`, `container=`. | §20 row 17: the layout must be signalled in the codec context, in `AudioSpecificConfig`, **and** in the container. All three or 5.1 plays as stereo. |
| 3011 | `AUDIO_BUFFER_OVERRUN` | The audio capture buffer overran. | The audio thread did not drain in time — it blocked, which §12 forbids. | Audio is never dropped (§12 drop policy): degrade video instead. |
| 3020 | `PROCESS_LOOPBACK_UNSUPPORTED` | Per-process loopback requires Windows 10 20H1 or later. | `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` unavailable on this build, or the audio service refused the activation. Adds `hr=`. | Tier B is unavailable; Tier A is unaffected and must remain fully functional (§2.2 item 3). `start_record` degrades to the system mix and sends a `warning` event rather than refusing. **Read `hr=` before concluding the OS is too old** — `E_ILLEGAL_METHOD_CALL` (0x8000000E) is an engine fault, not a platform one, and reported this code on a perfectly capable machine (BUG-047). `process_loopback_probe_result()` carries the same value to a caller. |
| 3021 | `MULTITRACK_REQUIRES_MKV` | Multi-track audio requires the Matroska container. | Tier B was requested with MP4 selected. Refused in **three** places: `configure`/`save_config` (nothing is applied), `start_record` (the path's extension decides the container, so the setting alone is not enough — BUG-043), and `Muxer::open` as the last line. | §20 row 16: **enforced engine-side, not only in the GUI.** Multi-track in MP4 is invisible in most players, so a soft warning is not enough. The GUI must explain inline rather than silently greying the option out. |
| 3022 | `MULTITRACK_TRACK_LIMIT_EXCEEDED` | More audio tracks requested than the limit allows. | More than 6 discrete tracks requested (§8.6). Adds `requested=`, `limit=`. | Reduce the track count. The limit is a deliberate resource bound. `AppAudioTracks::start` **refuses** the configuration rather than truncating it; `start_record` drops the surplus targets and logs this, because losing the sixth application is a smaller loss than losing the recording. |
| 3023 | `MULTITRACK_TARGET_PROCESS_GONE` | A multi-track target process exited. | The application feeding a per-process track terminated. Adds `track=`, `name=`, `pid=`. §20 row 15. | Informational, at WARN. The track continues as silence to the end of the file; truncating it makes the file ragged or invalid. The poll does **not** re-attach if the application restarts — see ACCEPTANCE question 16. |
| 3024 | `MULTITRACK_TRACK_INIT_FAILED` | A per-process audio track failed to start. | Process-loopback client creation, `Initialize` or `Start` failed for one target. Adds `track=`, `name=`, `pid=`. | Tier B degrades to the tracks that did start; Tier A is unaffected. The failed track **still exists in the file** and is silence-padded to full duration — §8.6 forbids dropping a track from the container, and `avformat_write_header` has fixed the stream set by then in any case. |

---

## 4xxx — Encode (SPEC.md §2.2, §9, §13)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 4001 | `ENCODE_NO_HARDWARE_ENCODER` | No hardware video encoder is available. | Neither NVENC nor AMF is present or functional on any adapter. | Descend the §13 ladder. **Note:** the software rung needs libx264, which is an open licensing decision (CLAUDE.md §9) — until it is resolved this is terminal. |
| 4002 | `ENCODE_ENCODER_OPEN_FAILED` | Video encoder could not be opened. | `avcodec_open2` failed. Adds `encoder=`, `width=`, `height=`. | Check the level cap (4004) and the input pixel format. A busy encoder session from another application also causes this. |
| 4003 | `ENCODE_CODEC_UNSUPPORTED` | Requested codec is not supported in this version. | HEVC or AV1 requested. Adds `codec=`. | v1 validates H.264 only (§2.2 item 4). The enum accepts the value from day one; only `h264` is selectable and tested. |
| 4004 | `ENCODE_LEVEL_EXCEEDED` | Requested resolution or bitrate exceeds the codec level. | Beyond H.264 High @ L4.2. Adds `level=`, `max_bitrate=`. | v1 targets 1080p SDR. Reduce resolution, frame rate, or bitrate. |
| 4005 | `ENCODE_SUBMIT_FAILED` | Submitting a frame to the encoder failed. | `avcodec_send_frame` failed. Adds `pts=`. | If the HRESULT indicates device loss, this is really 2005 — run the migration path. |
| 4006 | `ENCODE_RECEIVE_FAILED` | Retrieving a packet from the encoder failed. | `avcodec_receive_packet` returned an error other than `EAGAIN`/`EOF`. | Same device-loss check as 4005. |
| 4007 | `ENCODE_ENCODER_LOST` | The encoder became invalid and must be recreated. | The underlying device went away beneath the encoder. | Recreate via the factory as part of §5.4 migration. Because the codebase uses one unified encoder interface, this is a factory call rather than a rewrite (§2.2 item 1). |
| 4008 | `ENCODE_AUDIO_ENCODER_OPEN_FAILED` | AAC encoder could not be opened. | The native FFmpeg `aac` encoder rejected the layout or sample rate. Adds `channels=`, `sample_rate=`. | Verify the channel layout is explicitly signalled. `libfdk_aac` is deliberately excluded for licensing reasons (§2.1). |
| 4009 | `ENCODE_ZERO_COPY_LOST` | The encoder is round-tripping frames through host memory. | `av_hwframe_transfer_data` fired on the hot path, or the encoder negotiated a software pixel format. | §2.2 item 1's guard rail. Verified on the reference rig by `scripts/spikes/amf_zerocopy`, which found `libavcodec` does not even import the transfer function — so this firing means the negotiated format regressed. |
| 4010 | `ENCODE_ALL_RUNGS_EXHAUSTED` | Every rung of the degradation ladder has been exhausted. | §13 reached the bottom without achieving a sustainable encode rate. | Stop cleanly and finalise a playable file. The prime directive still applies. |
| 4011 | `ENCODE_BITRATE_OUT_OF_RANGE` | Requested bitrate is outside the supported range. | Config value outside the encoder's accepted range. Adds `requested=`, `min=`, `max=`. | Per §17, an invalid config value falls back to the default and raises a GUI warning — it never crashes and never silently produces a bad recording. |

---

## 5xxx — Mux (SPEC.md §10, §11, §20 row 2)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 5001 | `MUX_OPEN_FAILED` | Output format context could not be opened. | `avformat_alloc_output_context2` failed, or the muxer is absent from the build. Adds `container=`. | The FFmpeg whitelist compiles in only `mp4`, `mov` and `matroska` (see `ports/ffmpeg/framecapture-whitelist.cmake`). Any other container is absent by design. |
| 5002 | `MUX_CONTAINER_MISMATCH` | Container does not match the requested output format. | §20 row 2: the muxer was chosen from the filename rather than from the container enum. | Always select the muxer from the container enum, never by parsing the extension. |
| 5003 | `MUX_STREAM_ADD_FAILED` | Adding a stream to the container failed. | `avformat_new_stream` failed, or parameters could not be copied. | Check that the codec parameters are fully populated, including colour tags (§6). |
| 5004 | `MUX_WRITE_HEADER_FAILED` | Writing the container header failed. | `avformat_write_header` failed. Adds `container=`, `movflags=`. | For MP4 the fragmented flags must be set here (§10.3); progressive-during-recording yields an unplayable brick after a crash. |
| 5005 | `MUX_WRITE_PACKET_FAILED` | Writing a packet failed. | `av_interleaved_write_frame` failed. Frequently an I/O error underneath — check for 6005/6007. | If the disk is full, 6007 applies and is a sanctioned prime-directive exception. |
| 5006 | `MUX_WRITE_TRAILER_FAILED` | Writing the container trailer failed. | `av_write_trailer` failed or was never reached. §20 row 2 — the direct cause of unplayable output. | Recovery (§10.4) must run. Never report success without it. |
| 5007 | `MUX_FINALIZE_FAILED` | Finalising the output file failed. | The §10.4 finalisation sequence did not complete. | Attempt recovery; if that fails too, report 5009 and keep the partial file rather than deleting it. |
| 5008 | `MUX_REMUX_FAILED` | Fragmented-to-progressive remux failed. | The lossless remux on clean stop failed (§10.3). | **Keep the fragmented file.** It is playable as-is; deleting it in favour of a failed remux destroys the recording. |
| 5009 | `MUX_RECOVERY_FAILED` | Recovering an unfinalised file failed. | §10.4 recovery could not repair a file left by a crash or power loss. | Preserve the partial file and tell the user where it is. Never delete a user's recording. |
| 5010 | `MUX_SEGMENT_NOT_KEYFRAME_ALIGNED` | Segment boundary is not keyframe-aligned. | A split was attempted mid-GOP (§11). | Splitting mid-GOP makes the next file's opening seconds unplayable garbage. Force a keyframe and split on it. |
| 5011 | `MUX_VALIDATION_FAILED` | The output file failed post-recording validation. | The §10.4 gate found a bad duration, missing stream, or wrong codec ID. | Do not report success. This gate is what stops §20 row 2 reaching the user. |

---

## 6xxx — I/O (SPEC.md §10.4, §13, §17, §19, §20 row 10)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 6001 | `IO_PATH_INVALID` | The output path is not usable. | Nonexistent directory, invalid characters, or over `MAX_PATH`. Adds `path=`. | Validate before recording starts, not on the first write. |
| 6002 | `IO_PERMISSION_DENIED` | Access to the path was denied. | ACLs, or a location requiring elevation. Adds `path=`. | Default to a per-user location; never write to Program Files (§17). |
| 6003 | `IO_DIRECTORY_CREATE_FAILED` | Could not create the target directory. | `create_directories` failed. Adds `path=`. | Check the parent's permissions and that the volume is mounted. |
| 6004 | `IO_FILE_OPEN_FAILED` | Could not open the file. | `CreateFile` failed. Adds `path=`. | A sharing violation means another process holds the file. |
| 6005 | `IO_FILE_WRITE_FAILED` | Writing to the file failed. | `WriteFile` failed. Adds `path=`, `bytes=`. | Check for disk full (6007) and handle loss (6006) first — both are more specific. |
| 6006 | `IO_FILE_HANDLE_LOST` | The output file handle became invalid. | Removable volume unplugged, or a network share dropped. | **Prime-directive exception (SPEC.md §1).** Detect pre-emptively; a valid output file cannot be guaranteed once the handle is gone. |
| 6007 | `IO_DISK_FULL` | The target volume is out of space. | `ERROR_DISK_FULL` on a write. | **Prime-directive exception (SPEC.md §1).** Should have been caught as 6008 first; reaching 6007 means pre-emptive detection failed. |
| 6008 | `IO_DISK_FULL_IMMINENT` | The target volume is about to run out of space. | Free space fell below the headroom threshold. Adds `free_bytes=`, `threshold_bytes=`. | Stop and finalise cleanly *now*, while a valid file is still achievable. This is the pre-emptive detection §19 requires — it is not itself a violation. |
| 6009 | `IO_DISK_TOO_SLOW` | The target volume cannot sustain the required write rate. | Sustained throughput below the encode bitrate. §20 row 10. Adds `measured_mbps=`, `required_mbps=`. | Degrade per §13 rather than dropping frames. Degradation must be deliberate. |
| 6020 | `IO_CONFIG_READ_FAILED` | The configuration file could not be read. | `config.toml` unreadable. Adds `path=`. | Fall back to defaults and warn. Never block startup on a config read. |
| 6021 | `IO_CONFIG_PARSE_FAILED` | The configuration file is not valid TOML. | Syntax error. Adds `path=`, `line=`, `column=`. | Back up the bad file, start from defaults, warn in the GUI. Do not overwrite the user's file silently. |
| 6022 | `IO_CONFIG_MIGRATION_FAILED` | Configuration schema migration failed. | A step in the §17 migration chain failed. Adds `from_version=`, `to_version=`. | The original must already have been backed up to `config.toml.bak.<version>`. **Never discard unknown keys** — downgrading must not destroy settings. |
| 6023 | `IO_ATOMIC_REPLACE_FAILED` | Atomic file replacement failed. | `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING` failed. Adds `from=`, `to=`. | The temp file remains. Retry; do not fall back to a non-atomic write, which risks a truncated config. |

---

## 7xxx — IPC (SPEC.md §3.1, §15)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 7001 | `IPC_PIPE_CREATE_FAILED` | The control pipe could not be created. | `CreateNamedPipe` failed — usually a stale instance from an orphaned engine. Adds `pipe=`. | Check for an orphaned engine process (see 7011 and §20 row 13). |
| 7002 | `IPC_PIPE_CONNECT_FAILED` | Connecting to the control pipe failed. | The engine has not created its endpoint yet, or it exited during startup. | Retry with backoff during the startup window, then give up and report. |
| 7003 | `IPC_PIPE_BROKEN` | The control pipe was broken. | The peer closed or crashed mid-exchange. | Engine side: finalise the recording and exit. Never leave an orphan (§3.1). |
| 7004 | `IPC_MESSAGE_MALFORMED` | A control message could not be parsed. | Invalid JSON or a missing required field. Adds `bytes=`. | Reject the single message; do not tear down the connection. |
| 7005 | `IPC_MESSAGE_TOO_LARGE` | A control message exceeded the size limit. | Length prefix beyond the cap. Adds `size=`, `limit=`. | Reject before allocating. This is the bound that stops a bad prefix becoming an OOM. |
| 7006 | `IPC_PROTOCOL_VERSION_MISMATCH` | The peer speaks an incompatible protocol version. | GUI and engine from different installs. Adds `local=`, `remote=`. | The protocol is additive-only for backward compatibility (§2.1); a mismatch means a genuinely breaking change and both sides must be updated. |
| 7007 | `IPC_UNKNOWN_COMMAND` | The peer sent an unrecognised command. | A newer GUI talking to an older engine. Adds `command=`. | Reply with an explicit error; ignoring it silently makes the GUI hang waiting. |
| 7008 | `IPC_SHM_CREATE_FAILED` | The preview shared-memory region could not be created. | `CreateFileMapping` failed. Adds `bytes=`. | Preview is optional — recording must continue without it. |
| 7009 | `IPC_SHM_MAP_FAILED` | The preview shared-memory region could not be mapped. | `MapViewOfFile` failed. | As 7008: degrade the preview, never the recording. |
| 7010 | `IPC_HEARTBEAT_TIMEOUT` | The peer stopped sending heartbeats. | No heartbeat inside the §3.1 window. Adds `last_seen_ms=`. | Engine side: finalise and exit within 6 s (§20 row 13). |
| 7011 | `IPC_PEER_GONE` | The peer process exited. | Detected via the Job Object or a process-exit wait. | The Job Object's kill-on-close is the primary orphan guard; this is the backstop. |
| 7012 | `IPC_ENGINE_ALREADY_RUNNING` | Another engine instance owns this user session. | SPEC.md §3.1's named mutex is already held — a second engine was launched, or a previous one has not exited yet. Adds `mutex=`. | Do not start. The existing engine's control pipe is the one to connect to; if it is wedged, the host's Job Object closing will reap it. `Local\` scope, so another *user's* engine never triggers this. |
| 7013 | `IPC_JOB_OBJECT_FAILED` | The process job object could not be created or joined. | Host side: `CreateJobObject`/`AssignProcessToJobObject` failed. Engine side: `OpenJobObject` on the name in `FC_ENGINE_JOB` failed. Adds `job=`, `gle=`. | Host side is fatal — without the job there is no orphan guarantee. Engine side is a **warning**, not fatal: the engine runs, but a host that dies will kill it outright rather than letting it finalise (see `lifecycle.h`). |

---

## 9xxx — Internal (SPEC.md §12, §19)

| Code | Name | Meaning | Likely cause | Remediation |
|---|---|---|---|---|
| 9001 | `INTERNAL_INVARIANT_VIOLATED` | An internal invariant was violated. | A defect. Adds `invariant=`. | Report with the log bundle. Not user-actionable. |
| 9002 | `INTERNAL_NOT_IMPLEMENTED` | The requested operation is not implemented. | A path reached before its milestone landed. | Should be unreachable in a release build. If a user sees it, a feature was exposed before it was finished. |
| 9003 | `INTERNAL_INVALID_ARGUMENT` | An argument was invalid. | Failed precondition, or the generic mapping of `E_INVALIDARG`. | Usually too generic to act on: prefer `FC_HR_AS` at call sites so a specific code is reported instead. |
| 9004 | `INTERNAL_INVALID_STATE` | The operation is not valid in the current state. | e.g. `log::init` called twice, or `start` while already recording. | A caller-sequencing defect. The state machine (§13) is authoritative. |
| 9005 | `INTERNAL_OUT_OF_MEMORY` | An allocation failed. | Genuine exhaustion, or an unbounded buffer. | Every queue is bounded (§12) precisely so a 200 ms hitch cannot become an OOM. |
| 9006 | `INTERNAL_TIMEOUT` | The operation timed out. | A bounded wait expired. Adds `timeout_ms=`, `operation=`. | All waits are bounded on purpose; a timeout is a designed outcome, not a hang. |
| 9007 | `INTERNAL_CANCELLED` | The operation was cancelled. | Shutdown requested mid-operation. | Normal during teardown. |
| 9008 | `INTERNAL_QUEUE_FULL` | A bounded queue was full and the item was dropped. | Backpressure. Adds `queue=`, `capacity=`, `policy=`. | Expected and observable by design (§12). Video drops oldest; **audio is never dropped** — degrade video instead. |
| 9009 | `INTERNAL_THREAD_START_FAILED` | A worker thread could not be started. | Thread creation failed. Adds `thread=`. | Fatal for the affected stage; finalise what exists. |
| 9010 | `INTERNAL_THREAD_JOIN_TIMEOUT` | A worker thread did not exit within the join timeout. | A thread did not observe its stop signal inside the 2 s bound (§12). Adds `thread=`. | Escalate and log; never wait unbounded. A stuck thread must not prevent finalisation. |
| 9011 | `INTERNAL_UNHANDLED_EXCEPTION` | An exception escaped to a thread entry point. | The `catch`-all at a thread entry fired (§19). Adds `thread=`, `what=`. | Exceptions never cross a thread or IPC boundary. The entry point logs, classifies, and transitions the state machine. |
| 9999 | `INTERNAL_UNKNOWN` | An unclassified internal error occurred. | Returned by `hresult_to_fc_error` for an unmapped HRESULT, and by `error_from_code` for an undeclared code. | Deliberately not a guess: inventing a plausible subsystem would send whoever reads the log to the wrong place. The `hr=` field carries the real value. |
