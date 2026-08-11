# fc_core -- every line of engine logic lives here so that it is testable
# without spawning a process (SPEC.md §21.1, CLAUDE.md §5).

add_library(fc_core STATIC
    build_info.cpp
    build_info.h
    subsystem.cpp
    subsystem.h

    config/config.cpp
    config/config.h
    config/config_schema.cpp
    config/config_schema.h
    config/migration.cpp
    config/migration.h

    audio/loopback_capture.cpp
    audio/loopback_capture.h
    audio/process_loopback.cpp
    audio/process_loopback.h
    audio/resampler.cpp
    audio/resampler.h
    audio/drift_compensator.cpp
    audio/drift_compensator.h
    audio/audio_timeline.cpp
    audio/audio_timeline.h

    capture/capture_frame.h
    capture/capture_factory.cpp
    capture/capture_factory.h
    capture/dda/dda_capture.cpp
    capture/dda/dda_capture.h
    capture/source_resolver.cpp
    capture/source_resolver.h
    capture/nv12_writer.cpp
    capture/nv12_writer.h
    capture/i_screen_capture.cpp
    capture/i_screen_capture.h
    capture/wgc/wgc_capture.cpp
    capture/wgc/wgc_capture.h

    color/nv12_converter.cpp
    color/nv12_converter.h

    encode/aac_encoder.cpp
    encode/aac_encoder.h
    encode/h264_encoder.cpp
    encode/video_encoder.h

    ffmpeg/av_raii.cpp
    ffmpeg/av_raii.h

    health/health_monitor.cpp
    health/health_monitor.h

    ipc/framing.cpp
    ipc/framing.h
    ipc/protocol.cpp
    ipc/protocol.h
    ipc/pipe_server.cpp
    ipc/pipe_server.h
    ipc/pipe_client.cpp
    ipc/pipe_client.h
    ipc/lifecycle.cpp
    ipc/lifecycle.h
    ipc/engine_service.cpp
    ipc/engine_service.h

    mux/muxer.cpp
    mux/muxer.h
    mux/segment_planner.cpp
    mux/segment_planner.h
    mux/recovery.cpp
    mux/recovery.h

    preview/preview_ring.cpp
    preview/preview_ring.h
    preview/preview_scaler.cpp
    preview/preview_scaler.h
    preview/preview_writer.cpp
    preview/preview_writer.h

    pipeline/app_audio_tracks.cpp
    pipeline/app_audio_tracks.h
    pipeline/audio_path.cpp
    pipeline/audio_path.h
    pipeline/recording_session.cpp
    pipeline/recording_session.h
    pipeline/video_pipeline.cpp
    pipeline/video_pipeline.h

    timing/session_epoch.h
    timing/qpc_clock.h
    timing/pause_clock.h
    timing/frame_pacer.cpp
    timing/frame_pacer.h

    gpu/adapter_info.cpp
    gpu/adapter_info.h
    gpu/adapter_selector.cpp
    gpu/adapter_selector.h
    gpu/cross_adapter_probe.cpp
    gpu/cross_adapter_probe.h
    gpu/d3d_device.cpp
    gpu/d3d_device.h
    gpu/device_lock.cpp
    gpu/device_lock.h
    gpu/device_watcher.cpp
    gpu/device_watcher.h
    gpu/encoder_probe.cpp
    gpu/encoder_probe.h
    gpu/gpu_topology.cpp
    gpu/gpu_topology.h

    error/fc_error.cpp
    error/fc_error.h
    error/hresult.cpp
    error/hresult.h
    error/result.cpp
    error/result.h

    logging/crash_handler.cpp
    logging/crash_handler.h
    logging/ffmpeg_log.cpp
    logging/ffmpeg_log.h
    logging/log_fields.cpp
    logging/log_fields.h
    logging/logger.cpp
    logging/logger.h
    logging/ring_sink.cpp
    logging/ring_sink.h
    logging/session_preamble.cpp
    logging/session_preamble.h

    util/bounded_queue.h
    util/worker.h
    util/dpi_awareness.cpp
    util/dpi_awareness.h
    util/atomic_write.cpp
    util/atomic_write.h
    util/com_apartment.cpp
    util/com_apartment.h
    util/thread_utils.cpp
    util/thread_utils.h
)
add_library(FrameCapture::core ALIAS fc_core)

target_include_directories(fc_core PUBLIC "${CMAKE_SOURCE_DIR}/engine")

target_compile_definitions(fc_core PRIVATE
    FC_PROJECT_VERSION="${PROJECT_VERSION}"
    FC_GTEST_VERSION="${GTest_VERSION}"
)

target_link_libraries(fc_core
    PUBLIC
        FrameCapture::FFmpeg
        spdlog::spdlog
        tomlplusplus::tomlplusplus
        nlohmann_json::nlohmann_json
    PRIVATE
        d3d11.lib     # device creation on a specific adapter (SPEC.md §5.1)
        dxgi.lib      # adapter and output enumeration
        windowsapp.lib # Windows.Graphics.Capture (SPEC.md §4.2)
        dwmapi.lib    # DWMWA_CLOAKED, to skip invisible shell windows
        dbghelp.lib   # MiniDumpWriteDump (SPEC.md §18)
        shell32.lib   # SHGetKnownFolderPath for %LOCALAPPDATA%
        ole32.lib     # CoTaskMemFree, paired with the above
        avrt.lib      # MMCSS Pro Audio for the audio thread (SPEC.md §8.1)
        mmdevapi.lib  # ActivateAudioInterfaceAsync for process loopback (SPEC.md §8.6)
        advapi32.lib  # token + SID + SDDL for the control pipe's ACL (SPEC.md §15.1)
)

# SPEC.md §21.1: /W4 /permissive- /WX on fc_core. No exceptions to /WX.
if(MSVC)
    target_compile_options(fc_core PRIVATE /W4 /permissive- /WX)
endif()

# SPEC.md §6 conversion shader, compiled to embedded bytecode at build time.
fc_compile_shader(fc_core
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/color/bgra_to_nv12.hlsl"
    PROFILE cs_5_0
    ENTRY main
    VARIABLE g_bgra_to_nv12_cs
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/color/tone_map.hlsli"
)

# SPEC.md §15.2's preview downscale. Shares `tone_map.hlsli` with the converter above,
# so an HDR desktop previews as the same picture it records.
fc_compile_shader(fc_core
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/preview/downscale_preview.hlsl"
    PROFILE cs_5_0
    ENTRY main
    VARIABLE g_downscale_preview_cs
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/color/tone_map.hlsli"
)

set_target_properties(fc_core PROPERTIES FOLDER "engine")
