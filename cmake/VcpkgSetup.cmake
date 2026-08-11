# Resolves the vcpkg toolchain file before project() runs.
#
# Search order:
#   1. A CMAKE_TOOLCHAIN_FILE the caller already supplied  -- always wins.
#   2. <repo>/vcpkg                                        -- what bootstrap.ps1 creates.
#   3. $ENV{VCPKG_ROOT}                                    -- a system-wide install.
#
# Failing with an actionable message beats failing with 200 lines of
# "Could not find a package configuration file provided by FFMPEG".

if(DEFINED CMAKE_TOOLCHAIN_FILE)
    return()
endif()

set(_fc_vcpkg_candidates "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg")
if(DEFINED ENV{VCPKG_ROOT})
    list(APPEND _fc_vcpkg_candidates "$ENV{VCPKG_ROOT}")
endif()

foreach(_fc_candidate IN LISTS _fc_vcpkg_candidates)
    set(_fc_toolchain "${_fc_candidate}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${_fc_toolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${_fc_toolchain}" CACHE FILEPATH "vcpkg toolchain")
        message(STATUS "FrameCapture: using vcpkg at ${_fc_candidate}")
        unset(_fc_toolchain)
        unset(_fc_candidate)
        unset(_fc_vcpkg_candidates)
        return()
    endif()
endforeach()

message(FATAL_ERROR
    "vcpkg was not found.\n"
    "Run .\\scripts\\bootstrap.ps1 from the repository root, or set VCPKG_ROOT to an "
    "existing vcpkg installation.\n"
    "Searched: ${_fc_vcpkg_candidates}")
