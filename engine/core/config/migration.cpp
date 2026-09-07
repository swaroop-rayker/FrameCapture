#include "core/config/migration.h"

#include "core/config/config_schema.h"
#include "core/logging/logger.h"

#include <algorithm>
#include <array>
#include <system_error>

namespace fc::config {
namespace {

const MigrationStep* find_step(std::span<const MigrationStep> steps, int from) {
    const auto it = std::ranges::find(steps, from, &MigrationStep::from_version);
    return it != steps.end() ? &*it : nullptr;
}

/// v1 -> v2 (M9.6): `[hotkeys]`, `[overlay]` and `[window]` arrive.
///
/// **It writes nothing, and that is the whole step.** Both sections are new, every key in
/// them has a default, and `validate` supplies a default for any key a document does not
/// carry -- so a v1 file read as v2 already produces exactly the values this function
/// would insert. Writing them here would mean a migrated file has the keys spelled out
/// while a fresh one does not, and `serialize` then has two shapes to be correct about.
///
/// What it therefore *is* is the version stamp and the backup, both of which
/// `run_migrations` does around it. That is not nothing: the backup is what SPEC.md §17
/// promises a user before their file is rewritten, and a v1 file loaded by this build is
/// rewritten the moment any setting is saved.
///
/// The alternative -- leaving `builtin_migrations()` empty and simply accepting a v1
/// document -- was rejected. `run_migrations` refuses a version it has no step for,
/// deliberately, because a silent version bump is how a document ends up at an
/// intermediate shape with the header claiming otherwise. A step that transforms nothing
/// is still a link in the chain, and the chain's value is that every link is present.
[[nodiscard]] Result<void> migrate_1_to_2(toml::table& document) {
    static_cast<void>(document);
    return ok();
}

} // namespace

std::span<const MigrationStep> builtin_migrations() {
    // Function-local rather than a namespace-scope `constexpr`: `MigrationStep` holds a
    // `std::function`, which has no constant initialisation, and a namespace-scope
    // non-constexpr array would be a static initialisation order problem waiting for its
    // first caller from another translation unit.
    static const std::array kBuiltinMigrations{
        MigrationStep{1, 2, "migrate_1_to_2", "adds [hotkeys], [overlay] and [window]; all default-only",
                      migrate_1_to_2},
    };
    return {kBuiltinMigrations.data(), kBuiltinMigrations.size()};
}

std::filesystem::path backup_path_for(const std::filesystem::path& config_path, int version) {
    std::filesystem::path backup = config_path;
    backup += ".bak." + std::to_string(version);
    return backup;
}

Result<std::filesystem::path> back_up_config(const std::filesystem::path& config_path, int version) {
    std::error_code ec;
    if (!std::filesystem::exists(config_path, ec)) {
        // Nothing to preserve; not an error.
        return std::filesystem::path{};
    }

    std::filesystem::path backup = backup_path_for(config_path, version);

    // Never clobber an existing backup. If a migration ran, failed, and is being
    // retried, the first backup is the only copy of the original.
    if (std::filesystem::exists(backup, ec)) {
        for (int suffix = 2; suffix < 100; ++suffix) {
            std::filesystem::path candidate = backup;
            candidate += "." + std::to_string(suffix);
            if (!std::filesystem::exists(candidate, ec)) {
                backup = candidate;
                break;
            }
        }
    }

    std::filesystem::copy_file(config_path, backup, std::filesystem::copy_options::none, ec);
    if (ec) {
        FC_LOG_ERROR(Subsystem::Config, "config backup failed",
                     LogFields{}
                         .add("from", config_path.string())
                         .add("to", backup.string())
                         .add("detail", ec.message())
                         .add_error(FcError::IO_CONFIG_MIGRATION_FAILED));
        return FcError::IO_CONFIG_MIGRATION_FAILED;
    }

    FC_LOG_INFO(Subsystem::Config, "config backed up before migration",
                LogFields{}.add("backup", backup.string()).add("schema_version", static_cast<std::int64_t>(version)));
    return backup;
}

Result<MigrationOutcome> run_migrations(toml::table& document, int from_version, int to_version,
                                        std::span<const MigrationStep> steps) {
    MigrationOutcome outcome;
    outcome.from_version = from_version;
    outcome.to_version = from_version;

    if (from_version == to_version) {
        return outcome;
    }
    if (from_version > to_version) {
        // A newer file than we understand. Never rewrite it downward -- that is
        // precisely the downgrade that must not destroy settings.
        FC_LOG_WARN(Subsystem::Config, "config schema is newer than this build",
                    LogFields{}
                        .add("file_version", static_cast<std::int64_t>(from_version))
                        .add("supported_version", static_cast<std::int64_t>(to_version)));
        return FcError::IO_CONFIG_MIGRATION_FAILED;
    }

    int current = from_version;
    while (current < to_version) {
        const MigrationStep* step = find_step(steps, current);
        if (step == nullptr) {
            // Refuse rather than skip: leaving the document at an intermediate
            // shape while stamping the target version is worse than failing.
            FC_LOG_ERROR(Subsystem::Config, "no migration step available",
                         LogFields{}
                             .add("from_version", static_cast<std::int64_t>(current))
                             .add("target_version", static_cast<std::int64_t>(to_version))
                             .add_error(FcError::IO_CONFIG_MIGRATION_FAILED));
            return FcError::IO_CONFIG_MIGRATION_FAILED;
        }

        if (step->apply) {
            const Result<void> applied = step->apply(document);
            if (!applied.has_value()) {
                FC_LOG_ERROR(Subsystem::Config, "migration step failed",
                             LogFields{}
                                 .add("step", step->name)
                                 .add("from_version", static_cast<std::int64_t>(step->from_version))
                                 .add("to_version", static_cast<std::int64_t>(step->to_version))
                                 .add_error(applied.error()));
                return applied.error();
            }
        }

        // SPEC.md §17: log every transformation.
        FC_LOG_INFO(Subsystem::Config, "migration applied",
                    LogFields{}
                        .add("step", step->name)
                        .add("from_version", static_cast<std::int64_t>(step->from_version))
                        .add("to_version", static_cast<std::int64_t>(step->to_version))
                        .add("description", step->description));

        outcome.applied.push_back(MigrationRecord{step->from_version, step->to_version, std::string{step->name}});
        current = step->to_version;
        document.insert_or_assign("schema_version", static_cast<std::int64_t>(current));
    }

    outcome.to_version = current;
    return outcome;
}

} // namespace fc::config
