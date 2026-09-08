# Two tiers, two binaries (SPEC.md §20.1).
#
#   fc_tests      CPU tier  -- label `cpu`. Hermetic, runs anywhere.
#   fc_gpu_tests  GPU tier  -- label `gpu`. Needs real adapters and a real display.
#
# Splitting the binaries rather than labelling within one is what lets
# `ctest --preset ... -L gpu` (SPEC.md §21.2) select the hardware tier exactly,
# and `-LE gpu` select the tier you can run on a machine with no GPU.

# The deterministic test pattern (SPEC.md §20.1). Declared before `fc_tests`
# because the CPU tier uses its audio half -- the 1 kHz tone generator needs no
# hardware, and SPEC.md §20 row 4's arithmetic is asserted without any.
add_library(fc_synthetic_source STATIC
    tools/synthetic_source/synthetic_source.cpp
    tools/synthetic_source/synthetic_source.h
    tools/synthetic_source/synthetic_audio.cpp
    tools/synthetic_source/synthetic_audio.h
)
target_include_directories(fc_synthetic_source PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tools/synthetic_source")
target_link_libraries(fc_synthetic_source PUBLIC FrameCapture::core)
set_target_properties(fc_synthetic_source PROPERTIES FOLDER "tests")
if(MSVC)
    target_compile_options(fc_synthetic_source PRIVATE /W4 /permissive- /WX)
endif()

add_executable(fc_tests
    unit/test_adapter_selector.cpp
    unit/test_atomic_write.cpp
    unit/test_build_info.cpp
    unit/test_capture_factory.cpp
    unit/test_codec_availability.cpp
    unit/test_config.cpp
    unit/test_config_migration.cpp
    unit/test_crash_artifacts.cpp
    unit/test_device_watcher.cpp
    unit/test_app_audio_tracks.cpp
    unit/test_audio_path.cpp
    unit/test_audio_timeline.cpp
    unit/test_bounded_queue.cpp
    unit/test_drift_compensator.cpp
    unit/test_fc_error.cpp
    unit/test_finalize_progress.cpp
    unit/test_frame_pacer.cpp
    unit/test_gui_watchdog.cpp
    unit/test_health_monitor.cpp
    unit/test_hresult.cpp
    unit/test_ipc_protocol.cpp
    unit/test_logging.cpp
    unit/test_pause_clock.cpp
    unit/test_preview_ring.cpp
    unit/test_recovery.cpp
    unit/test_result.cpp
    unit/test_segment_planner.cpp
    unit/test_session_epoch.cpp
)

target_include_directories(fc_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Lets test_fc_error verify that every declared code appears in ERROR_CODES.md,
# turning SPEC.md §19's documentation requirement into a build gate.
target_compile_definitions(fc_tests PRIVATE FC_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")

target_link_libraries(fc_tests PRIVATE FrameCapture::core fc_synthetic_source GTest::gtest GTest::gtest_main)

set_target_properties(fc_tests PROPERTIES FOLDER "tests")

include(GoogleTest)
gtest_discover_tests(fc_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "cpu"
)

# ---------------------------------------------------------------------------
# GPU tier
#
# These do not skip when hardware is absent -- they fail. A GPU test that passes
# on a machine with no GPU proves nothing (CLAUDE.md §6).
# ---------------------------------------------------------------------------

# A recorder whose whole purpose is to be killed (SPEC.md §20 row 3).
#
# `test_crash_recovery` needs a `TerminateProcess` against a real recording, and a
# process cannot terminate itself in a way that proves anything. Separate binary,
# not part of the shipped engine.
add_executable(fc_crash_recorder tools/crash_recorder/main.cpp)
target_link_libraries(fc_crash_recorder PRIVATE FrameCapture::core fc_synthetic_source)
set_target_properties(fc_crash_recorder PROPERTIES FOLDER "tests")
if(MSVC)
    target_compile_options(fc_crash_recorder PRIVATE /W4 /permissive- /WX)
endif()

# A process to intercept and then lose (SPEC.md §8.6, §20 row 15).
#
# Separate for the reason the other two are: process loopback is keyed on a *process
# id*, so a fixture inside `fc_gpu_tests` playing a tone would be the process doing the
# capturing. Links no FrameCapture code — a helper sharing the engine's audio stack
# could hide a defect in it.
add_executable(fc_audio_target tools/audio_target/main.cpp)
target_link_libraries(fc_audio_target PRIVATE avrt.lib ole32.lib)
set_target_properties(fc_audio_target PROPERTIES FOLDER "tests")
if(MSVC)
    target_compile_options(fc_audio_target PRIVATE /W4 /permissive- /WX)
endif()

# A stand-in for the GUI's process-lifecycle role (SPEC.md §3.1, §20 row 13).
#
# Separate for the same reason `fc_crash_recorder` is: row 13's test kills it, and a
# fixture inside `fc_gpu_tests` cannot be killed without killing the reporter.
add_executable(fc_gui_host tools/gui_host/main.cpp)
target_link_libraries(fc_gui_host PRIVATE FrameCapture::core)
set_target_properties(fc_gui_host PROPERTIES FOLDER "tests")
if(MSVC)
    target_compile_options(fc_gui_host PRIVATE /W4 /permissive- /WX)
endif()

add_executable(fc_gpu_tests
    fixtures/decoded_media.cpp
    fixtures/decoded_media.h
    fixtures/screen_animator.cpp
    fixtures/screen_animator.h
    integration/gpu_test_main.cpp
    integration/test_pipeline_synthetic.cpp
    integration/test_screen_animator.cpp
    integration/test_synthetic_source.cpp
    integration/test_capture_sustained.cpp
    integration/test_capture_to_nv12.cpp
    integration/test_color_conversion.cpp
    integration/test_gpu_migration.cpp
    integration/test_gpu_topology.cpp
    integration/test_audio_capture.cpp
    integration/test_audio_device_migration.cpp
    integration/test_amf_zero_copy.cpp
    integration/test_audio_encode.cpp
    integration/test_av_sync.cpp
    integration/test_cfr_exactness.cpp
    integration/test_channel_layout.cpp
    integration/test_crash_recovery.cpp
    integration/test_finalize_throughput.cpp
    integration/test_loopback_recording.cpp
    integration/test_migration_parameter_sets.cpp
    integration/test_multitrack.cpp
    integration/test_slow_disk.cpp
    integration/test_software_encoder.cpp
    integration/test_sustained_60fps.cpp
    integration/test_throughput.cpp
    integration/test_orphan_prevention.cpp
    integration/test_overlay_exclusion.cpp
    integration/test_pause_resume.cpp
    integration/test_preview.cpp
    integration/test_second_recording.cpp
    integration/test_segmentation.cpp
    integration/test_video_encode_mkv.cpp
    integration/test_watchdog_recovery.cpp
    # SPEC.md §20.1's chaos tier. On the GPU target because randomised fault injection
    # against anything but the real encode-and-mux chain proves nothing -- the claim is
    # the prime directive, and a mock cannot fail to produce a file the way hardware can.
    chaos/test_chaos_injection.cpp
)

target_include_directories(fc_gpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# So test_crash_recovery can spawn the helper without guessing at the layout.
target_compile_definitions(fc_gpu_tests PRIVATE
    FC_CRASH_RECORDER_EXE="$<TARGET_FILE:fc_crash_recorder>"
    # SPEC.md §20 row 13 needs both halves of the §3.1 pairing by path: the host it
    # kills, and the engine that host spawns.
    FC_GUI_HOST_EXE="$<TARGET_FILE:fc_gui_host>"
    FC_ENGINE_EXE="$<TARGET_FILE:framecapture-engine>"
    # SPEC.md §20 row 15 needs a real process to intercept and then lose (SPEC.md §8.6).
    FC_AUDIO_TARGET_EXE="$<TARGET_FILE:fc_audio_target>")
add_dependencies(fc_gpu_tests fc_crash_recorder fc_gui_host fc_audio_target framecapture-engine)

# gtest_main is deliberately absent: gpu_test_main.cpp provides main so DPI
# awareness is set before any test body runs.
# pdh.lib is for `test_sustained_60fps` only: SPEC.md §20 row 6 names a GPU *load
# level*, and the `GPU Engine` performance counters are the only way to report one
# without a vendor SDK. Test-only -- fc_core links no telemetry (SPEC.md §0.2).
target_link_libraries(fc_gpu_tests PRIVATE FrameCapture::core fc_synthetic_source GTest::gtest pdh.lib)

set_target_properties(fc_gpu_tests PROPERTIES FOLDER "tests")

gtest_discover_tests(fc_gpu_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS "gpu"
)
