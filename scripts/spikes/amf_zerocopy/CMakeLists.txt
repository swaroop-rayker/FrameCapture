# Standalone build for the AMF zero-copy spike.
#
# Deliberately NOT referenced by the root CMakeLists.txt. This is throwaway
# diagnostic code (SPEC.md §2.2 item 1); it must never end up in the engine
# build graph or the ctest suite.
#
# It consumes the dependency tree the main build already installed, so it does
# not trigger a second FFmpeg build.

cmake_minimum_required(VERSION 3.25)

get_filename_component(FC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(_fc_toolchain "${FC_REPO_ROOT}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    if(NOT EXISTS "${_fc_toolchain}" AND DEFINED ENV{VCPKG_ROOT})
        set(_fc_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    endif()
    if(NOT EXISTS "${_fc_toolchain}")
        message(FATAL_ERROR "vcpkg not found. Run scripts/bootstrap.ps1 in the repo root first.")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "${_fc_toolchain}" CACHE FILEPATH "vcpkg toolchain")
endif()

# Reuse the main build's installed tree rather than resolving our own manifest:
# two manifests sharing one installed directory would fight over its contents.
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "" FORCE)
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")
set(VCPKG_INSTALLED_DIR "${FC_REPO_ROOT}/build/vcpkg_installed" CACHE PATH "")

project(amf_zerocopy_spike LANGUAGES CXX)

if(NOT EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR
        "FFmpeg is not installed at ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}.\n"
        "Configure the main build first:  cmake --preset windows-msvc-release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(FFMPEG REQUIRED)

add_executable(amf_zerocopy main.cpp)

target_include_directories(amf_zerocopy SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(amf_zerocopy PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(amf_zerocopy PRIVATE
    ${FFMPEG_LIBRARIES}
    d3d11.lib
    dxgi.lib
    dxguid.lib
    psapi.lib
)

# /WX is intentionally absent: spike code is not held to fc_core's bar.
if(MSVC)
    target_compile_options(amf_zerocopy PRIVATE /W4 /permissive- /external:W0)
endif()

set_target_properties(amf_zerocopy PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
