#pragma once

#include "core/error/result.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// toml++ appears in this header because a migration step operates on the parsed
// document. Only config translation units include it (SPEC.md §17: the engine is
// stateless with respect to disk config), so the compile-time cost is contained.
#include <toml++/toml.hpp>

namespace fc::config {

/// One step of the ordered migration chain (SPEC.md §17).
///
/// A step transforms the *whole document*, not just the keys it owns, and must
/// leave keys it does not understand alone -- preserving unknown keys is what stops
/// a downgrade from destroying settings.
struct MigrationStep {
    int from_version = 0;
    int to_version = 0;
    std::string_view name;        ///< e.g. "migrate_1_to_2", used in log lines.
    std::string_view description; ///< What it changes, for the log and CONFIG.md.
    std::function<Result<void>(toml::table&)> apply;
};

/// The built-in chain, ordered by `from_version`.
///
/// **Currently empty, and correctly so:** v1 is the first schema, so there is no
/// earlier shape to migrate from. Writing a `migrate_1_to_2` now would mean
/// inventing a v2 that does not exist.
///
/// When v2 arrives, append a step here and bump `kCurrentSchemaVersion`. The
/// machinery below -- ordering, gap detection, backup, per-step logging -- is
/// implemented and tested against synthetic steps in `test_config_migration`, so a
/// real step only has to supply its own transformation.
[[nodiscard]] std::span<const MigrationStep> builtin_migrations();

struct MigrationRecord {
    int from_version = 0;
    int to_version = 0;
    std::string name;
};

struct MigrationOutcome {
    int from_version = 0;
    int to_version = 0;
    std::vector<MigrationRecord> applied;
    /// Where the pre-migration file was copied, empty if no backup was needed.
    std::filesystem::path backup_path;
};

/// `<path>.bak.<version>` -- SPEC.md §17's backup naming.
[[nodiscard]] std::filesystem::path backup_path_for(const std::filesystem::path& config_path, int version);

/// Runs the chain from `from_version` up to `to_version`, in order.
///
/// Fails rather than guessing if the chain cannot get there: a missing link would
/// otherwise silently leave the document at an intermediate version while the
/// header claimed otherwise.
///
/// `steps` is injectable so the machinery is testable without a real v2.
[[nodiscard]] Result<MigrationOutcome> run_migrations(toml::table& document, int from_version, int to_version,
                                                      std::span<const MigrationStep> steps);

/// Copies `config_path` to `<path>.bak.<version>` before it is rewritten. Never
/// overwrites an existing backup -- a second migration attempt must not clobber the
/// only copy of the user's original file.
[[nodiscard]] Result<std::filesystem::path> back_up_config(const std::filesystem::path& config_path, int version);

} // namespace fc::config
