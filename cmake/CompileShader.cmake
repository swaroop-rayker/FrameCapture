# Compiles HLSL to a C header containing the bytecode.
#
# Shaders are compiled at build time, not runtime: a shader that fails to compile
# should fail the build, not the recording (SPEC.md §6 / error 2009). Embedding the
# bytecode also removes a class of deployment bug where the .cso is missing next to
# the exe.

find_program(FC_FXC_EXECUTABLE
    NAMES fxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
    DOC "HLSL compiler from the Windows SDK"
)

if(NOT FC_FXC_EXECUTABLE)
    # Last resort: newest x64 fxc under any installed SDK.
    file(GLOB _fc_fxc_candidates
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe")
    if(_fc_fxc_candidates)
        list(SORT _fc_fxc_candidates)
        list(GET _fc_fxc_candidates -1 FC_FXC_EXECUTABLE)
    endif()
    unset(_fc_fxc_candidates)
endif()

if(NOT FC_FXC_EXECUTABLE)
    message(FATAL_ERROR
        "fxc.exe was not found. It ships with the Windows SDK; install the SDK "
        "component or configure from a Developer PowerShell.")
endif()

message(STATUS "FrameCapture: fxc        ${FC_FXC_EXECUTABLE}")

# fc_compile_shader(<target> SOURCE <file.hlsl> PROFILE cs_5_0 ENTRY main
#                   VARIABLE <symbol> OUTPUT_VAR <var-for-generated-header>
#                   [INCLUDES <file.hlsli> ...])
#
# The generated header declares `const BYTE <symbol>[]`, which the C++ side feeds
# straight to CreateComputeShader.
#
# INCLUDES lists .hlsli files the shader `#include`s. fxc resolves them itself
# (relative to the source), but CMake cannot see through the preprocessor -- so
# without naming them here, editing a shared header would leave every shader that
# includes it stale. `tone_map.hlsli` is shared by the encoder's converter and the
# preview's downscaler precisely so the two cannot drift, and a stale-rebuild bug
# would let them drift anyway, silently, one build at a time.
function(fc_compile_shader target)
    cmake_parse_arguments(ARG "" "SOURCE;PROFILE;ENTRY;VARIABLE;OUTPUT_VAR" "INCLUDES" ${ARGN})

    get_filename_component(_name "${ARG_SOURCE}" NAME_WE)
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/shaders/${_name}.h")

    # /WX: a shader warning is a defect like any other. /O3 because this runs per
    # frame. /Zi for a debuggable shader in PIX.
    add_custom_command(
        OUTPUT "${_generated}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${FC_FXC_EXECUTABLE}"
                /nologo
                /T ${ARG_PROFILE}
                /E ${ARG_ENTRY}
                /O3
                /WX
                /Fh "${_generated}"
                /Vn ${ARG_VARIABLE}
                "${ARG_SOURCE}"
        DEPENDS "${ARG_SOURCE}" ${ARG_INCLUDES}
        COMMENT "Compiling shader ${_name}.hlsl (${ARG_PROFILE})"
        VERBATIM
    )

    target_sources(${target} PRIVATE "${_generated}" "${ARG_SOURCE}" ${ARG_INCLUDES})
    set_source_files_properties("${ARG_SOURCE}" ${ARG_INCLUDES} PROPERTIES HEADER_FILE_ONLY TRUE)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")

    if(ARG_OUTPUT_VAR)
        set(${ARG_OUTPUT_VAR} "${_generated}" PARENT_SCOPE)
    endif()
endfunction()
