#include "core/build_info.h"

#include <gtest/gtest.h>

// M0a's only assertion: the toolchain wired every declared dependency into the
// binary and each one can report a version. It is deliberately trivial -- its
// job is to prove that configure, build, link and ctest all work end to end.
TEST(BuildInfo, EveryDependencyReportsAVersion) {
    const std::vector<fc::DependencyVersion> deps = fc::dependency_versions();

    ASSERT_FALSE(deps.empty());
    for (const fc::DependencyVersion& dep : deps) {
        EXPECT_FALSE(dep.name.empty());
        EXPECT_FALSE(dep.version.empty()) << "dependency " << dep.name << " reported no version";
    }

    EXPECT_FALSE(fc::project_version().empty());
}
