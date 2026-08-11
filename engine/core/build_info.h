#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace fc {

/// How a third-party component reaches the shipped engine binary.
enum class DependencyLinkage {
    /// Linked into framecapture-engine; the reported version is queried from
    /// the library at runtime, so it is the version actually loaded.
    Linked,
    /// Header-only in this build; the version comes from the headers the
    /// translation unit was compiled against.
    HeaderOnly,
    /// Not part of the engine binary at all. Reported so that `--version`
    /// output fully describes the dependency set.
    TestOnly,
};

struct DependencyVersion {
    std::string_view name;
    std::string version;
    DependencyLinkage linkage;
};

/// Every third-party component this build depends on, in report order.
[[nodiscard]] std::vector<DependencyVersion> dependency_versions();

/// The FrameCapture version, from the CMake project() declaration.
[[nodiscard]] std::string_view project_version() noexcept;

/// Human-readable label for a linkage kind.
[[nodiscard]] std::string_view to_string(DependencyLinkage linkage) noexcept;

} // namespace fc
