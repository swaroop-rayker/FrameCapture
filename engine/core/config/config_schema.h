#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace fc::config {

/// Bumped whenever the on-disk shape changes. Every file carries it (SPEC.md §17).
///
/// v1 is the first schema, so `builtin_migrations()` is empty. See migration.h.
constexpr int kCurrentSchemaVersion = 1;

// ---------------------------------------------------------------------------
// Enumerated values.
//
// Each has a stable lowercase spelling that is what appears in the TOML file.
// Renaming one is a breaking change to a user's config, so don't.
// ---------------------------------------------------------------------------

enum class Container { Mp4, Mkv };

/// SPEC.md §2.2 item 4 / §9: the enum defines all three from day one so that
/// enabling HEVC or AV1 in v1.1 is a config change and a validation pass rather
/// than a refactor. Only `H264` is selectable in the GUI and validated in v1.
enum class VideoCodec { H264, Hevc, Av1 };

enum class EncoderSelection { Auto, Nvenc, Amf, X264 };

/// SPEC.md §9: CBR is deliberately absent -- it wastes bits on static screens,
/// which is most of a screen recording.
enum class RateControl { Cqp, Vbr, Lossless };

/// SPEC.md §7.2 / §7.3. CFR is the default; VFR is opt-in and fragile in MP4.
enum class PacingMode { Cfr, Vfr };

enum class ChannelLayoutSetting { Auto, Stereo, Surround51, Surround71 };

enum class CaptureBackend { Auto, Wgc, Dda };

enum class LogLevelSetting { Trace, Debug, Info, Warn, Error, Critical };

[[nodiscard]] std::string_view to_string(Container value) noexcept;
[[nodiscard]] std::string_view to_string(VideoCodec value) noexcept;
[[nodiscard]] std::string_view to_string(EncoderSelection value) noexcept;
[[nodiscard]] std::string_view to_string(RateControl value) noexcept;
[[nodiscard]] std::string_view to_string(PacingMode value) noexcept;
[[nodiscard]] std::string_view to_string(ChannelLayoutSetting value) noexcept;
[[nodiscard]] std::string_view to_string(CaptureBackend value) noexcept;
[[nodiscard]] std::string_view to_string(LogLevelSetting value) noexcept;

/// The file extension a container must be written with, leading dot included (BUG-043).
///
/// Separate from `to_string` because they are different questions that happen to have
/// the same answer today: `to_string` names the *setting*, and this names the *file*.
/// They would diverge the moment a container gained an alias -- `.m4v` for MP4, say --
/// and a caller that used the setting's name as an extension would then be wrong without
/// anything changing at its own call site.
[[nodiscard]] std::string_view extension_for(Container value) noexcept;

/// The container a file extension names, or `nullopt` if it names none we write.
///
/// The inverse of `extension_for`, and the reason `start_record` is self-describing: a
/// path that ends in `.mp4` means MP4 whatever this engine's configuration says, so a
/// request means the same thing on every machine. Case-insensitive -- `.MP4` off a file
/// dialog is the same request as `.mp4`.
[[nodiscard]] std::optional<Container> container_from_extension(std::string_view extension) noexcept;

[[nodiscard]] std::optional<Container> container_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<VideoCodec> video_codec_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<EncoderSelection> encoder_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<RateControl> rate_control_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<PacingMode> pacing_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<ChannelLayoutSetting> channel_layout_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<CaptureBackend> capture_backend_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<LogLevelSetting> log_level_from_string(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// The schema table.
//
// SPEC.md §17 requires validation to be "schema-driven with explicit ranges".
// This table *is* those ranges -- the reader functions in config.cpp look their
// bounds up here rather than repeating literals, so a range exists in exactly one
// place and the documentation cannot describe a bound the code does not enforce.
//
// `test_config.SchemaIsDocumented` fails the build if a key here is missing from
// docs/CONFIG.md.
// ---------------------------------------------------------------------------

enum class KeyType {
    Boolean,
    /// Continuous integer range [min, max]. Out-of-range values are clamped.
    Integer,
    /// Discrete integer set listed in `allowed`. Unlisted values fall back.
    IntegerChoice,
    /// Named value from `allowed`. Unrecognised values fall back.
    Enum,
    String,
    Path,
};

struct KeySpec {
    std::string_view path; ///< Dotted TOML path, e.g. "video.fps".
    KeyType type;
    std::int64_t min = 0;          ///< Integer only.
    std::int64_t max = 0;          ///< Integer only.
    std::string_view allowed;      ///< Comma-separated, for Enum and IntegerChoice.
    std::string_view default_repr; ///< Default as it appears in the file.
    int since_version = 1;         ///< Schema version that introduced the key.
};

/// Every key in the v1 schema, in file order.
[[nodiscard]] std::span<const KeySpec> schema_keys() noexcept;

/// Lookup by dotted path. Null for an unknown key -- which is not an error;
/// unknown keys are preserved (SPEC.md §17).
[[nodiscard]] const KeySpec* find_key(std::string_view path) noexcept;

} // namespace fc::config
