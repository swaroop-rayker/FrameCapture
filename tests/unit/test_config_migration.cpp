#include "core/config/migration.h"

#include "core/config/config.h"
#include "core/config/config_schema.h"
#include "temp_dir.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>

namespace {

using fc::config::MigrationOutcome;
using fc::config::MigrationStep;

std::string read_file(const std::filesystem::path& path) {
    const std::ifstream stream{path, std::ios::binary};
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void put_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream << contents;
}

/// Synthetic steps. The built-in chain has one real link and cannot exercise ordering,
/// gap detection or multi-step chaining on its own, so the machinery is tested against
/// these -- which is what let `migrate_1_to_2` land on already-proven infrastructure.
MigrationStep renaming_step(int from, int to, std::string_view name) {
    return MigrationStep{from, to, name, "renames video.old_key to video.new_key", [](toml::table& document) {
                             auto* video = document.get_as<toml::table>("video");
                             if (video == nullptr) {
                                 return fc::ok();
                             }
                             if (auto* old = video->get("old_key"); old != nullptr) {
                                 video->insert_or_assign("new_key", *old);
                                 video->erase("old_key");
                             }
                             return fc::ok();
                         }};
}

MigrationStep tagging_step(int from, int to, std::string_view name, std::string_view marker) {
    return MigrationStep{from, to, name, "adds a marker key", [marker](toml::table& document) {
                             document.insert_or_assign(std::string{marker}, true);
                             return fc::ok();
                         }};
}

MigrationStep failing_step(int from, int to) {
    return MigrationStep{from, to, "migrate_failing", "always fails",
                         [](toml::table&) -> fc::Result<void> { return fc::FcError::IO_CONFIG_MIGRATION_FAILED; }};
}

// ---------------------------------------------------------------------------
// The built-in chain
// ---------------------------------------------------------------------------

// The chain must reach the current version from every version that has ever shipped.
// Asserted as a property rather than as a list, so adding a v3 without its step fails
// here rather than at a user's first launch on the new build.
TEST(MigrationChain, BuiltInChainReachesTheCurrentVersionFromEveryEarlierOne) {
    const std::span<const MigrationStep> steps = fc::config::builtin_migrations();
    ASSERT_EQ(steps.size(), static_cast<std::size_t>(fc::config::kCurrentSchemaVersion - 1))
        << "one step per version transition, and no more";

    for (int from = 1; from < fc::config::kCurrentSchemaVersion; ++from) {
        toml::table document;
        document.insert_or_assign("schema_version", from);
        const auto outcome = fc::config::run_migrations(document, from, fc::config::kCurrentSchemaVersion, steps);
        ASSERT_TRUE(outcome.has_value()) << "no path from v" << from;
        EXPECT_EQ(outcome.value().to_version, fc::config::kCurrentSchemaVersion);
    }
}

TEST(MigrationChain, AVersion1FileMigratesToTheCurrentSchema) {
    auto result = fc::config::load_from_string("schema_version = 1\n[video]\nfps = 30\n");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().migrated);
    // The setting the file *did* carry is untouched by the migration. `migrate_1_to_2`
    // adds sections; a step that also rewrote an existing value would be doing two jobs.
    EXPECT_EQ(result.value().config.video.fps, 30);
    EXPECT_EQ(result.value().config.schema_version, fc::config::kCurrentSchemaVersion);
}

// The point of `migrate_1_to_2` being a no-op: a v1 file and a v2 file that both omit
// the new sections must produce identical settings. If they did not, the step would be
// hiding a transformation that ought to be explicit.
TEST(MigrationChain, MigratedDefaultsMatchAFreshFilesDefaults) {
    const auto migrated = fc::config::load_from_string("schema_version = 1\n");
    const auto fresh = fc::config::load_from_string("schema_version = 2\n");
    ASSERT_TRUE(migrated.has_value());
    ASSERT_TRUE(fresh.has_value());

    EXPECT_EQ(migrated.value().config.hotkeys.start, fresh.value().config.hotkeys.start);
    EXPECT_EQ(migrated.value().config.hotkeys.stop, fresh.value().config.hotkeys.stop);
    EXPECT_EQ(migrated.value().config.hotkeys.pause_resume, fresh.value().config.hotkeys.pause_resume);
    EXPECT_EQ(migrated.value().config.overlay.pill_enabled, fresh.value().config.overlay.pill_enabled);
    EXPECT_EQ(migrated.value().config.overlay.pill_corner, fresh.value().config.overlay.pill_corner);
    EXPECT_EQ(migrated.value().config.overlay.toast_max_visible, fresh.value().config.overlay.toast_max_visible);
}

TEST(MigrationChain, AVersion2FileNeedsNoMigration) {
    auto result = fc::config::load_from_string("schema_version = 2\n[video]\nfps = 30\n");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().migrated);
    EXPECT_TRUE(result.value().backup_path.empty());
}

// ---------------------------------------------------------------------------
// Ordering and chaining
// ---------------------------------------------------------------------------

TEST(MigrationChain, RunsStepsInOrderAcrossMultipleVersions) {
    toml::table document;
    document.insert_or_assign("schema_version", 1);

    const std::array steps{tagging_step(1, 2, "migrate_1_to_2", "step_a"),
                           tagging_step(2, 3, "migrate_2_to_3", "step_b"),
                           tagging_step(3, 4, "migrate_3_to_4", "step_c")};

    const auto result = fc::config::run_migrations(document, 1, 4, steps);
    ASSERT_TRUE(result.has_value());

    const MigrationOutcome& outcome = result.value();
    EXPECT_EQ(outcome.from_version, 1);
    EXPECT_EQ(outcome.to_version, 4);
    ASSERT_EQ(outcome.applied.size(), 3u);
    EXPECT_EQ(outcome.applied[0].name, "migrate_1_to_2");
    EXPECT_EQ(outcome.applied[1].name, "migrate_2_to_3");
    EXPECT_EQ(outcome.applied[2].name, "migrate_3_to_4");

    EXPECT_TRUE(document.contains("step_a"));
    EXPECT_TRUE(document.contains("step_b"));
    EXPECT_TRUE(document.contains("step_c"));
    EXPECT_EQ(document["schema_version"].value_or(0), 4);
}

TEST(MigrationChain, StepsFoundOutOfOrderInTheArrayStillRunInVersionOrder) {
    toml::table document;
    // Deliberately shuffled: correctness must come from the version numbers, not
    // from the order someone happened to register them in.
    const std::array steps{tagging_step(2, 3, "migrate_2_to_3", "second"),
                           tagging_step(1, 2, "migrate_1_to_2", "first")};

    const auto result = fc::config::run_migrations(document, 1, 3, steps);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().applied.size(), 2u);
    EXPECT_EQ(result.value().applied[0].name, "migrate_1_to_2");
    EXPECT_EQ(result.value().applied[1].name, "migrate_2_to_3");
}

TEST(MigrationChain, StopsAtTheTargetVersion) {
    toml::table document;
    const std::array steps{tagging_step(1, 2, "migrate_1_to_2", "a"), tagging_step(2, 3, "migrate_2_to_3", "b")};

    const auto result = fc::config::run_migrations(document, 1, 2, steps);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().to_version, 2);
    EXPECT_TRUE(document.contains("a"));
    EXPECT_FALSE(document.contains("b")) << "ran past the requested target";
}

TEST(MigrationChain, NoOpWhenAlreadyAtTheTargetVersion) {
    toml::table document;
    const std::array steps{tagging_step(1, 2, "migrate_1_to_2", "a")};

    const auto result = fc::config::run_migrations(document, 2, 2, steps);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().applied.empty());
    EXPECT_FALSE(document.contains("a"));
}

TEST(MigrationChain, TransformsValuesRatherThanJustBumpingTheVersion) {
    toml::table video;
    video.insert_or_assign("old_key", "carried across");
    toml::table document;
    document.insert_or_assign("video", video);

    const std::array steps{renaming_step(1, 2, "migrate_1_to_2")};
    ASSERT_TRUE(fc::config::run_migrations(document, 1, 2, steps).has_value());

    const auto* migrated = document.get_as<toml::table>("video");
    ASSERT_NE(migrated, nullptr);
    EXPECT_FALSE(migrated->contains("old_key"));
    EXPECT_EQ((*migrated)["new_key"].value_or(std::string_view{}), "carried across");
}

// ---------------------------------------------------------------------------
// Failure modes -- refuse rather than guess
// ---------------------------------------------------------------------------

TEST(MigrationChain, MissingLinkIsAnErrorRatherThanASkippedVersion) {
    toml::table document;
    // 1 -> 2 exists, 2 -> 3 does not.
    const std::array steps{tagging_step(1, 2, "migrate_1_to_2", "a")};

    const auto result = fc::config::run_migrations(document, 1, 3, steps);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::IO_CONFIG_MIGRATION_FAILED);
    // Stamping the target version while leaving the document at an intermediate
    // shape would be silent corruption.
    EXPECT_NE(document["schema_version"].value_or(0), 3);
}

TEST(MigrationChain, AFailingStepAbortsTheChain) {
    toml::table document;
    const std::array steps{tagging_step(1, 2, "migrate_1_to_2", "a"), failing_step(2, 3),
                           tagging_step(3, 4, "migrate_3_to_4", "c")};

    const auto result = fc::config::run_migrations(document, 1, 4, steps);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::IO_CONFIG_MIGRATION_FAILED);
    EXPECT_TRUE(document.contains("a"));
    EXPECT_FALSE(document.contains("c")) << "chain continued past a failed step";
}

TEST(MigrationChain, RefusesToDowngrade) {
    toml::table document;
    const auto result = fc::config::run_migrations(document, 5, 1, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fc::FcError::IO_CONFIG_MIGRATION_FAILED);
}

// ---------------------------------------------------------------------------
// Unknown-key preservation through a migration
// ---------------------------------------------------------------------------

TEST(MigrationChain, PreservesKeysNoStepKnowsAbout) {
    toml::table video;
    video.insert_or_assign("old_key", "x");
    video.insert_or_assign("someone_elses_key", 42);
    toml::table document;
    document.insert_or_assign("video", video);
    document.insert_or_assign("top_level_stranger", "still here");

    const std::array steps{renaming_step(1, 2, "migrate_1_to_2")};
    ASSERT_TRUE(fc::config::run_migrations(document, 1, 2, steps).has_value());

    EXPECT_EQ(document["top_level_stranger"].value_or(std::string_view{}), "still here");
    const auto* migrated = document.get_as<toml::table>("video");
    ASSERT_NE(migrated, nullptr);
    EXPECT_EQ((*migrated)["someone_elses_key"].value_or(0), 42);
}

// ---------------------------------------------------------------------------
// Backups -- SPEC.md §17's config.toml.bak.<version>
// ---------------------------------------------------------------------------

TEST(MigrationBackup, PathUsesTheDocumentedNaming) {
    const std::filesystem::path config = "C:/x/config.toml";
    EXPECT_EQ(fc::config::backup_path_for(config, 1).filename().string(), "config.toml.bak.1");
    EXPECT_EQ(fc::config::backup_path_for(config, 7).filename().string(), "config.toml.bak.7");
}

TEST(MigrationBackup, CopiesTheOriginalBeforeItIsRewritten) {
    const fc::test::TempDir dir{"backup"};
    const std::filesystem::path config = dir.path() / "config.toml";
    put_file(config, "schema_version = 1\noriginal = true\n");

    const auto result = fc::config::back_up_config(config, 1);
    ASSERT_TRUE(result.has_value());

    const std::filesystem::path& backup = result.value();
    ASSERT_TRUE(std::filesystem::exists(backup));
    EXPECT_EQ(backup.filename().string(), "config.toml.bak.1");
    EXPECT_EQ(read_file(backup), "schema_version = 1\noriginal = true\n");
    // The original must still be there -- a backup is a copy, not a move.
    EXPECT_TRUE(std::filesystem::exists(config));
}

TEST(MigrationBackup, NeverOverwritesAnExistingBackup) {
    const fc::test::TempDir dir{"backup"};
    const std::filesystem::path config = dir.path() / "config.toml";
    put_file(config, "second attempt\n");
    put_file(dir.path() / "config.toml.bak.1", "the only copy of the original\n");

    const auto result = fc::config::back_up_config(config, 1);
    ASSERT_TRUE(result.has_value());

    // The first backup is the user's original; a retry must not destroy it.
    EXPECT_EQ(read_file(dir.path() / "config.toml.bak.1"), "the only copy of the original\n");
    EXPECT_NE(result.value().filename().string(), "config.toml.bak.1");
    EXPECT_EQ(read_file(result.value()), "second attempt\n");
}

TEST(MigrationBackup, MissingFileIsNotAnError) {
    const fc::test::TempDir dir{"backup"};
    const auto result = fc::config::back_up_config(dir.path() / "absent.toml", 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty());
}

// ---------------------------------------------------------------------------
// End to end through load()
// ---------------------------------------------------------------------------

// With kCurrentSchemaVersion == 1 there is no valid *older* version, so the
// migrate-on-load branch is unreachable from disk today; it is covered directly by
// the run_migrations tests above. What is reachable is a nonsense version, and that
// must not be mistaken for a migration request.
TEST(MigrationBackup, AnOutOfRangeVersionIsClampedRatherThanTreatedAsAMigration) {
    const fc::test::TempDir dir{"migrate"};
    const std::filesystem::path config = dir.path() / "config.toml";
    const std::string original = "schema_version = -9999\n[video]\nfps = 30\n";
    put_file(config, original);

    const auto result = fc::config::load(config);
    ASSERT_TRUE(result.has_value()) << "a corrupt version must not make the file unloadable";
    EXPECT_EQ(result.value().config.video.fps, 30) << "the rest of the file is still usable";
    // Clamped to the schema floor and then treated as a *v1 file*, which is what the
    // chain does with any v1 file: it migrates it. The thing being ruled out is the
    // chain going looking for a `migrate_-9999_to_-9998` and refusing the load over a
    // step for a schema that never existed.
    //
    // This assertion used to read `EXPECT_FALSE(migrated)`, which held only while 1 was
    // both the clamp floor and the current version. That coincidence ended at v2, and
    // the assertion was testing it rather than the behaviour named in the test's title.
    EXPECT_EQ(result.value().config.schema_version, fc::config::kCurrentSchemaVersion);
    EXPECT_TRUE(std::filesystem::exists(config)) << "the original must never be deleted";
    EXPECT_EQ(read_file(config), original) << "load must not rewrite the file";
}

TEST(MigrationBackup, ZeroIsAlsoTreatedAsCorruptRatherThanAsSchemaZero) {
    // Version 0 was never a released schema, so it is garbage, not history.
    const fc::test::TempDir dir{"migrate"};
    const std::filesystem::path config = dir.path() / "config.toml";
    put_file(config, "schema_version = 0\n[video]\ncqp = 22\n");

    const auto result = fc::config::load(config);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().config.video.cqp, 22);
}

TEST(MigrationBackup, ANewerFileIsNotBackedUpOrRewritten) {
    const fc::test::TempDir dir{"migrate"};
    const std::filesystem::path config = dir.path() / "config.toml";
    const std::string original = "schema_version = 99\n[video]\nfps = 30\n";
    put_file(config, original);

    const auto result = fc::config::load(config);
    ASSERT_TRUE(result.has_value()) << "a newer file must still load, not fail";
    EXPECT_TRUE(result.value().backup_path.empty());
    EXPECT_EQ(read_file(config), original) << "load must not rewrite a newer file";
}

} // namespace
