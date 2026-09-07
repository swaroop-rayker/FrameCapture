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
/// One step so far: `migrate_1_to_2` (M9.6), which adds `[hotkeys]` and `[overlay]`.
/// It transforms nothing -- both sections are default-only, and `validate` already
/// supplies a default for an absent key -- so what it contributes is the version stamp
/// and, through `run_migrations`, SPEC.md §17's backup before the file is rewritten.
/// See its comment for why an empty-bodied step is still the right shape.
///
/// When v3 arrives, append a step here and bump `kCurrentSchemaVersion`. The machinery
/// below -- ordering, gap detection, backup, per-step logging -- is implemented and
/// tested against synthetic steps in `test_config_migration`, so a real step only has to
/// supply its own transformation.
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
