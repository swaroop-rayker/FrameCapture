#include "core/build_info.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <nlohmann/json_fwd.hpp>
#include <spdlog/version.h>
#include <toml++/toml.hpp>

#ifndef FC_PROJECT_VERSION
#error "FC_PROJECT_VERSION must be defined by the build system"
#endif

#ifndef FC_GTEST_VERSION
#error "FC_GTEST_VERSION must be defined by the build system"
#endif

namespace fc {
namespace {

std::string format_triple(unsigned major, unsigned minor, unsigned patch) {
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

/// FFmpeg packs its library versions into a single unsigned.
std::string format_libav(unsigned packed) {
    return format_triple(AV_VERSION_MAJOR(packed), AV_VERSION_MINOR(packed), AV_VERSION_MICRO(packed));
}

} // namespace

std::vector<DependencyVersion> dependency_versions() {
    return {
        // Queried from the loaded libraries, not from the headers: if a stale
        // avcodec DLL is next to the exe, this is where it shows up.
        {"FFmpeg", av_version_info(), DependencyLinkage::Linked},
        {"  libavcodec", format_libav(avcodec_version()), DependencyLinkage::Linked},
        {"  libavformat", format_libav(avformat_version()), DependencyLinkage::Linked},
        {"  libavutil", format_libav(avutil_version()), DependencyLinkage::Linked},
        {"  libswresample", format_libav(swresample_version()), DependencyLinkage::Linked},
        {"spdlog", format_triple(SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH), DependencyLinkage::Linked},
        {"toml++", format_triple(TOML_LIB_MAJOR, TOML_LIB_MINOR, TOML_LIB_PATCH), DependencyLinkage::HeaderOnly},
        {"nlohmann-json",
         format_triple(NLOHMANN_JSON_VERSION_MAJOR, NLOHMANN_JSON_VERSION_MINOR, NLOHMANN_JSON_VERSION_PATCH),
         DependencyLinkage::HeaderOnly},
        {"GoogleTest", FC_GTEST_VERSION, DependencyLinkage::TestOnly},
    };
}

std::string_view project_version() noexcept {
    return FC_PROJECT_VERSION;
}

std::string_view to_string(DependencyLinkage linkage) noexcept {
    switch (linkage) {
    case DependencyLinkage::Linked:
        return "linked";
    case DependencyLinkage::HeaderOnly:
        return "header-only";
    case DependencyLinkage::TestOnly:
        return "tests only";
    }
    return "unknown";
}

} // namespace fc
