#include "core/config/config.h"

#include "core/config/migration.h"
#include "core/logging/logger.h"
#include "core/util/atomic_write.h"

#include <windows.h>
// Must follow windows.h.
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fc::config {
namespace {

std::filesystem::path known_folder(const KNOWNFOLDERID& id, const wchar_t* env_fallback) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        std::filesystem::path result{raw};
        CoTaskMemFree(raw);
        return result;
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }

    if (env_fallback != nullptr) {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD written = GetEnvironmentVariableW(env_fallback, buffer, MAX_PATH);
        if (written > 0 && written < MAX_PATH) {
            return std::filesystem::path{buffer};
        }
    }
    return std::filesystem::path{"."};
}

void warn(std::vector<Warning>& warnings, WarningKind kind, std::string_view key, std::string detail) {
    warnings.push_back(Warning{kind, std::string{key}, std::move(detail)});
}

/// Resolves a dotted path against the document. Returns null when any segment is
/// missing or is not a table.
const toml::node* find_node(const toml::table& document, std::string_view path) {
    const toml::table* table = &document;
    std::size_t start = 0;

    while (true) {
        const std::size_t dot = path.find('.', start);
        const std::string_view segment = path.substr(start, dot == std::string_view::npos ? dot : dot - start);
        const toml::node* node = table->get(segment);
        if (node == nullptr) {
            return nullptr;
        }
        if (dot == std::string_view::npos) {
            return node;
        }
        table = node->as_table();
        if (table == nullptr) {
            return nullptr;
        }
        start = dot + 1;
    }
}

/// Splits "a.b.c" and inserts `value` at that path, creating intermediate tables.
template <typename T>
void assign_node(toml::table& document, std::string_view path, T&& value) {
    toml::table* table = &document;
    std::size_t start = 0;

    while (true) {
        const std::size_t dot = path.find('.', start);
        if (dot == std::string_view::npos) {
            table->insert_or_assign(path.substr(start), std::forward<T>(value));
            return;
        }
        const std::string_view segment = path.substr(start, dot - start);
        toml::node* existing = table->get(segment);
        if (existing == nullptr || existing->as_table() == nullptr) {
            // Replacing a non-table with a table would silently drop whatever was
            // there, so only create when the slot is genuinely absent or wrong-typed
            // (in which case validation already warned about it).
            table->insert_or_assign(segment, toml::table{});
            existing = table->get(segment);
        }
        table = existing->as_table();
        start = dot + 1;
    }
}

bool read_bool(const toml::table& document, std::string_view path, bool fallback, std::vector<Warning>& warnings) {
    const toml::node* node = find_node(document, path);
    if (node == nullptr) {
        warn(warnings, WarningKind::Missing, path, "not present; using default");
        return fallback;
    }
    if (const auto* value = node->as_boolean(); value != nullptr) {
        return value->get();
    }
    warn(warnings, WarningKind::WrongType, path, "expected a boolean; using default");
    return fallback;
}

std::int64_t read_integer(const toml::table& document, std::string_view path, std::int64_t fallback,
                          std::vector<Warning>& warnings) {
    const KeySpec* spec = find_key(path);
    const toml::node* node = find_node(document, path);
    if (node == nullptr) {
        warn(warnings, WarningKind::Missing, path, "not present; using default");
        return fallback;
    }

    const auto* value = node->as_integer();
    if (value == nullptr) {
        warn(warnings, WarningKind::WrongType, path, "expected an integer; using default");
        return fallback;
    }

    const std::int64_t raw = value->get();
    if (spec == nullptr) {
        return raw;
    }

    if (spec->type == KeyType::IntegerChoice) {
        // Discrete set: there is no sensible nearest value, so fall back.
        std::string_view remaining = spec->allowed;
        while (!remaining.empty()) {
            const std::size_t comma = remaining.find(',');
            const std::string_view token = remaining.substr(0, comma);
            std::int64_t candidate = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), candidate);
            if (parsed.ec == std::errc{} && candidate == raw) {
                return raw;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
        warn(warnings, WarningKind::UnknownValue, path,
             "value " + std::to_string(raw) + " is not one of {" + std::string{spec->allowed} + "}; using default");
        return fallback;
    }

    // Continuous range: clamp. A user asking for a bitrate above the ceiling means
    // "as high as possible", and resetting them to the default would be a surprise.
    if (raw < spec->min) {
        warn(warnings, WarningKind::Clamped, path,
             "value " + std::to_string(raw) + " below minimum " + std::to_string(spec->min) + "; clamped");
        return spec->min;
    }
    if (raw > spec->max) {
        warn(warnings, WarningKind::Clamped, path,
             "value " + std::to_string(raw) + " above maximum " + std::to_string(spec->max) + "; clamped");
        return spec->max;
    }
    return raw;
}

std::string read_string(const toml::table& document, std::string_view path, std::string fallback,
                        std::vector<Warning>& warnings) {
    const toml::node* node = find_node(document, path);
    if (node == nullptr) {
        warn(warnings, WarningKind::Missing, path, "not present; using default");
        return fallback;
    }
    if (const auto* value = node->as_string(); value != nullptr) {
        return value->get();
    }
    warn(warnings, WarningKind::WrongType, path, "expected a string; using default");
    return fallback;
}

/// Enum read. `parse` is the per-enum from_string.
template <typename E, typename Parse>
E read_enum(const toml::table& document, std::string_view path, E fallback, Parse parse,
            std::vector<Warning>& warnings) {
    const toml::node* node = find_node(document, path);
    if (node == nullptr) {
        warn(warnings, WarningKind::Missing, path, "not present; using default");
        return fallback;
    }
    const auto* value = node->as_string();
    if (value == nullptr) {
        warn(warnings, WarningKind::WrongType, path, "expected a string; using default");
        return fallback;
    }
    if (const std::optional<E> parsed = parse(value->get()); parsed.has_value()) {
        return *parsed;
    }

    const KeySpec* spec = find_key(path);
    warn(warnings, WarningKind::UnknownValue, path,
         "\"" + value->get() + "\" is not one of {" + std::string{spec != nullptr ? spec->allowed : ""} +
             "}; using default");
    return fallback;
}

/// Walks the document and reports every key that is not in the schema, so the GUI
/// can tell the user their file contains settings this build ignores but kept.
void report_unknown_keys(const toml::table& table, const std::string& prefix, std::vector<Warning>& warnings) {
    for (const auto& [key, node] : table) {
        const std::string path = prefix.empty() ? std::string{key.str()} : prefix + "." + std::string{key.str()};
        if (const toml::table* nested = node.as_table(); nested != nullptr) {
            if (find_key(path) == nullptr && nested->empty()) {
                warn(warnings, WarningKind::UnknownKeyPreserved, path, "unrecognised section; preserved");
            }
            report_unknown_keys(*nested, path, warnings);
            continue;
        }
        if (find_key(path) == nullptr) {
            warn(warnings, WarningKind::UnknownKeyPreserved, path, "unrecognised key; preserved verbatim");
        }
    }
}

} // namespace

std::vector<std::string> parse_multitrack_targets(std::string_view targets) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= targets.size()) {
        const std::size_t comma = targets.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? targets.size() : comma;
        std::string_view item = targets.substr(start, end - start);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.remove_prefix(1);
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            // Duplicates dropped rather than accepted. Two tracks aimed at one
            // executable would both resolve to the same PID, and process loopback
            // includes the target's whole process tree (SPEC.md §8.6) -- so the two
            // tracks would carry identical audio, spend two of the six slots, and
            // look like a bug in the muxer.
            const std::string value{item};
            if (std::ranges::find(out, value) == out.end()) {
                out.push_back(value);
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

std::string_view to_string(WarningKind kind) noexcept {
    switch (kind) {
    case WarningKind::WrongType:
        return "wrong_type";
    case WarningKind::Clamped:
        return "clamped";
    case WarningKind::UnknownValue:
        return "unknown_value";
    case WarningKind::Missing:
        return "missing";
    case WarningKind::UnknownKeyPreserved:
        return "unknown_key_preserved";
    case WarningKind::SchemaTooNew:
        return "schema_too_new";
    case WarningKind::Migrated:
        return "migrated";
    }
    return "unknown";
}

std::filesystem::path default_config_path() {
    return known_folder(FOLDERID_LocalAppData, L"LOCALAPPDATA") / "FrameCapture" / "config.toml";
}

Config defaults() {
    Config config;
    config.general.output_directory = known_folder(FOLDERID_Videos, L"USERPROFILE") / "FrameCapture";
    return config;
}

Config validate(const toml::table& document, std::vector<Warning>& warnings) {
    Config config = defaults();

    config.schema_version = static_cast<int>(read_integer(document, "schema_version", kCurrentSchemaVersion, warnings));

    const std::string output_dir =
        read_string(document, "general.output_directory", config.general.output_directory.string(), warnings);
    config.general.output_directory = std::filesystem::path{output_dir};
    config.general.filename_template =
        read_string(document, "general.filename_template", config.general.filename_template, warnings);
    config.general.language = read_string(document, "general.language", config.general.language, warnings);

    config.video.width = static_cast<int>(read_integer(document, "video.width", config.video.width, warnings));
    config.video.height = static_cast<int>(read_integer(document, "video.height", config.video.height, warnings));
    config.video.fps = static_cast<int>(read_integer(document, "video.fps", config.video.fps, warnings));
    config.video.container =
        read_enum(document, "video.container", config.video.container, container_from_string, warnings);
    config.video.codec = read_enum(document, "video.codec", config.video.codec, video_codec_from_string, warnings);
    config.video.encoder = read_enum(document, "video.encoder", config.video.encoder, encoder_from_string, warnings);
    config.video.rate_control =
        read_enum(document, "video.rate_control", config.video.rate_control, rate_control_from_string, warnings);
    config.video.cqp = static_cast<int>(read_integer(document, "video.cqp", config.video.cqp, warnings));
    config.video.bitrate_kbps =
        static_cast<int>(read_integer(document, "video.bitrate_kbps", config.video.bitrate_kbps, warnings));
    config.video.gop_seconds =
        static_cast<int>(read_integer(document, "video.gop_seconds", config.video.gop_seconds, warnings));
    config.video.max_b_frames =
        static_cast<int>(read_integer(document, "video.max_b_frames", config.video.max_b_frames, warnings));
    config.video.pacing = read_enum(document, "video.pacing", config.video.pacing, pacing_from_string, warnings);
    config.video.full_range = read_bool(document, "video.full_range", config.video.full_range, warnings);

    config.audio.device_id = read_string(document, "audio.device_id", config.audio.device_id, warnings);
    config.audio.channel_layout =
        read_enum(document, "audio.channel_layout", config.audio.channel_layout, channel_layout_from_string, warnings);
    config.audio.bitrate_kbps =
        static_cast<int>(read_integer(document, "audio.bitrate_kbps", config.audio.bitrate_kbps, warnings));
    config.audio.multitrack_enabled =
        read_bool(document, "audio.multitrack_enabled", config.audio.multitrack_enabled, warnings);
    config.audio.max_tracks =
        static_cast<int>(read_integer(document, "audio.max_tracks", config.audio.max_tracks, warnings));
    config.audio.multitrack_targets =
        read_string(document, "audio.multitrack_targets", config.audio.multitrack_targets, warnings);
    config.audio.multitrack_reattach =
        read_bool(document, "audio.multitrack_reattach", config.audio.multitrack_reattach, warnings);

    config.segmentation.enabled = read_bool(document, "segmentation.enabled", config.segmentation.enabled, warnings);
    config.segmentation.split_by_duration =
        read_bool(document, "segmentation.split_by_duration", config.segmentation.split_by_duration, warnings);
    config.segmentation.duration_minutes = static_cast<int>(
        read_integer(document, "segmentation.duration_minutes", config.segmentation.duration_minutes, warnings));
    config.segmentation.split_by_size =
        read_bool(document, "segmentation.split_by_size", config.segmentation.split_by_size, warnings);
    config.segmentation.size_mb =
        static_cast<int>(read_integer(document, "segmentation.size_mb", config.segmentation.size_mb, warnings));

    config.advanced.capture_backend = read_enum(document, "advanced.capture_backend", config.advanced.capture_backend,
                                                capture_backend_from_string, warnings);
    config.advanced.capture_cursor =
        read_bool(document, "advanced.capture_cursor", config.advanced.capture_cursor, warnings);
    config.advanced.hdr_tonemap = read_bool(document, "advanced.hdr_tonemap", config.advanced.hdr_tonemap, warnings);
    config.advanced.log_level =
        read_enum(document, "advanced.log_level", config.advanced.log_level, log_level_from_string, warnings);
    config.advanced.gpu_override =
        read_string(document, "advanced.gpu_override", config.advanced.gpu_override, warnings);

    config.updates.check_enabled = read_bool(document, "updates.check_enabled", config.updates.check_enabled, warnings);

    report_unknown_keys(document, {}, warnings);

    config.document = document;
    return config;
}

Result<LoadOutcome> load_from_string(std::string_view toml_text, const std::filesystem::path& origin) {
    LoadOutcome outcome;
    outcome.file_existed = true;

    // The vcpkg toml++ is a compiled shared library built with TOML_EXCEPTIONS on,
    // so toml::parse throws rather than returning a checkable result. Redefining
    // TOML_EXCEPTIONS here would be an ODR violation against that prebuilt DLL.
    //
    // Catching the specific exception type is permitted (CLAUDE.md §4 bans
    // catch(...) and empty catch blocks, not typed handlers), and this is exactly
    // the boundary SPEC.md §19 wants: an unparseable config is recoverable, so the
    // exception is converted to a Result and never escapes the config layer.
    toml::table document;
    try {
        document = toml::parse(toml_text, origin.string());
    } catch (const toml::parse_error& error) {
        FC_LOG_ERROR(Subsystem::Config, "config parse failed",
                     LogFields{}
                         .add("path", origin.string())
                         .add("line", static_cast<std::int64_t>(error.source().begin.line))
                         .add("column", static_cast<std::int64_t>(error.source().begin.column))
                         .add("detail", std::string{error.description()})
                         .add_error(FcError::IO_CONFIG_PARSE_FAILED));
        return FcError::IO_CONFIG_PARSE_FAILED;
    }

    // Read the declared version before validation, because validation clamps and a
    // clamped version would hide the real state of the file.
    int declared = kCurrentSchemaVersion;
    if (const toml::node* node = document.get("schema_version"); node != nullptr) {
        if (const auto* value = node->as_integer(); value != nullptr) {
            declared = static_cast<int>(value->get());
        }
    }

    // Clamp before branching. A garbage version such as -9999 or 0 is an
    // out-of-range integer, not a request to migrate from a schema that never
    // existed -- treating it as the latter turns a recoverable file into a hard
    // failure. Clamping is the same policy every other integer key gets.
    if (const KeySpec* version_spec = find_key("schema_version"); version_spec != nullptr) {
        const int low = static_cast<int>(version_spec->min);
        const int high = static_cast<int>(version_spec->max);
        if (declared < low || declared > high) {
            outcome.warnings.push_back(Warning{WarningKind::Clamped, "schema_version",
                                               "declared version " + std::to_string(declared) + " is outside " +
                                                   std::to_string(low) + ".." + std::to_string(high) + "; clamped"});
            declared = std::clamp(declared, low, high);
        }
    }

    if (declared > kCurrentSchemaVersion) {
        // Load what we understand, keep everything, and do not rewrite the file.
        outcome.warnings.push_back(Warning{WarningKind::SchemaTooNew, "schema_version",
                                           "file declares version " + std::to_string(declared) +
                                               " but this build "
                                               "supports " +
                                               std::to_string(kCurrentSchemaVersion) +
                                               "; unknown settings are preserved and the file will not be rewritten"});
        FC_LOG_WARN(Subsystem::Config, "config is from a newer build",
                    LogFields{}
                        .add("file_version", static_cast<std::int64_t>(declared))
                        .add("supported_version", static_cast<std::int64_t>(kCurrentSchemaVersion)));
    } else if (declared < kCurrentSchemaVersion) {
        if (!origin.empty()) {
            const Result<std::filesystem::path> backup = back_up_config(origin, declared);
            if (!backup.has_value()) {
                return backup.error();
            }
            outcome.backup_path = backup.value();
        }

        const Result<MigrationOutcome> migrated =
            run_migrations(document, declared, kCurrentSchemaVersion, builtin_migrations());
        if (!migrated.has_value()) {
            return migrated.error();
        }
        outcome.migrated = !migrated.value().applied.empty();
        for (const MigrationRecord& record : migrated.value().applied) {
            outcome.warnings.push_back(Warning{WarningKind::Migrated, "schema_version",
                                               record.name + " (" + std::to_string(record.from_version) + " -> " +
                                                   std::to_string(record.to_version) + ")"});
        }
    }

    outcome.config = validate(document, outcome.warnings);
    return outcome;
}

Result<LoadOutcome> load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // First launch. Not an error, and deliberately not written out here --
        // writing on load would create a file the user never asked for.
        LoadOutcome outcome;
        outcome.config = defaults();
        outcome.file_existed = false;
        FC_LOG_INFO(Subsystem::Config, "no config file; using defaults", LogFields{}.add("path", path.string()));
        return outcome;
    }

    const std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        FC_LOG_ERROR(Subsystem::Config, "config read failed",
                     LogFields{}.add("path", path.string()).add_error(FcError::IO_CONFIG_READ_FAILED));
        return FcError::IO_CONFIG_READ_FAILED;
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) {
        return FcError::IO_CONFIG_READ_FAILED;
    }

    return load_from_string(buffer.str(), path);
}

std::string serialize(const Config& config) {
    // Start from the document the config came from so unrecognised keys stay where
    // the user put them, then overwrite the keys this build owns.
    toml::table document = config.document;

    assign_node(document, "schema_version", static_cast<std::int64_t>(kCurrentSchemaVersion));

    assign_node(document, "general.output_directory", config.general.output_directory.string());
    assign_node(document, "general.filename_template", config.general.filename_template);
    assign_node(document, "general.language", config.general.language);

    assign_node(document, "video.width", static_cast<std::int64_t>(config.video.width));
    assign_node(document, "video.height", static_cast<std::int64_t>(config.video.height));
    assign_node(document, "video.fps", static_cast<std::int64_t>(config.video.fps));
    assign_node(document, "video.container", std::string{to_string(config.video.container)});
    assign_node(document, "video.codec", std::string{to_string(config.video.codec)});
    assign_node(document, "video.encoder", std::string{to_string(config.video.encoder)});
    assign_node(document, "video.rate_control", std::string{to_string(config.video.rate_control)});
    assign_node(document, "video.cqp", static_cast<std::int64_t>(config.video.cqp));
    assign_node(document, "video.bitrate_kbps", static_cast<std::int64_t>(config.video.bitrate_kbps));
    assign_node(document, "video.gop_seconds", static_cast<std::int64_t>(config.video.gop_seconds));
    assign_node(document, "video.max_b_frames", static_cast<std::int64_t>(config.video.max_b_frames));
    assign_node(document, "video.pacing", std::string{to_string(config.video.pacing)});
    assign_node(document, "video.full_range", config.video.full_range);

    assign_node(document, "audio.device_id", config.audio.device_id);
    assign_node(document, "audio.channel_layout", std::string{to_string(config.audio.channel_layout)});
    assign_node(document, "audio.bitrate_kbps", static_cast<std::int64_t>(config.audio.bitrate_kbps));
    assign_node(document, "audio.multitrack_enabled", config.audio.multitrack_enabled);
    assign_node(document, "audio.max_tracks", static_cast<std::int64_t>(config.audio.max_tracks));
    assign_node(document, "audio.multitrack_targets", config.audio.multitrack_targets);
    assign_node(document, "audio.multitrack_reattach", config.audio.multitrack_reattach);

    assign_node(document, "segmentation.enabled", config.segmentation.enabled);
    assign_node(document, "segmentation.split_by_duration", config.segmentation.split_by_duration);
    assign_node(document, "segmentation.duration_minutes",
                static_cast<std::int64_t>(config.segmentation.duration_minutes));
    assign_node(document, "segmentation.split_by_size", config.segmentation.split_by_size);
    assign_node(document, "segmentation.size_mb", static_cast<std::int64_t>(config.segmentation.size_mb));

    assign_node(document, "advanced.capture_backend", std::string{to_string(config.advanced.capture_backend)});
    assign_node(document, "advanced.capture_cursor", config.advanced.capture_cursor);
    assign_node(document, "advanced.hdr_tonemap", config.advanced.hdr_tonemap);
    assign_node(document, "advanced.log_level", std::string{to_string(config.advanced.log_level)});
    assign_node(document, "advanced.gpu_override", config.advanced.gpu_override);

    assign_node(document, "updates.check_enabled", config.updates.check_enabled);

    std::ostringstream out;
    out << "# FrameCapture configuration.\n"
           "# Generated automatically; hand edits are preserved, including keys this\n"
           "# build does not recognise. See docs/CONFIG.md for every key and its range.\n\n";
    out << document << '\n';
    return out.str();
}

Result<void> save(const Config& config, const std::filesystem::path& path) {
    const std::string text = serialize(config);
    FC_TRY(write_file_atomically(path, text));
    FC_LOG_INFO(
        Subsystem::Config, "config saved",
        LogFields{}.add("path", path.string()).add("schema_version", static_cast<std::int64_t>(kCurrentSchemaVersion)));
    return ok();
}

} // namespace fc::config
