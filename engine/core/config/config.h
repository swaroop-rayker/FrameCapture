#pragma once

#include "core/config/config_schema.h"
#include "core/error/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace fc::config {

/// Why a value was not taken as written. Every one of these is surfaced to the GUI
/// (SPEC.md §17: "raises a GUI warning"); none of them is fatal.
enum class WarningKind {
    /// Value had the wrong TOML type. Replaced with the default.
    WrongType,
    /// Numeric value outside the schema range. Clamped to the nearest bound.
    Clamped,
    /// Enum or discrete-integer value not in the allowed set. Replaced with the default.
    UnknownValue,
    /// Key absent. Default used. Informational -- a fresh install produces one per key.
    Missing,
    /// Key is not in the schema. Kept verbatim so a downgrade cannot lose it.
    UnknownKeyPreserved,
    /// File declares a schema newer than this build understands.
    SchemaTooNew,
    /// A migration step ran.
    Migrated,
};

[[nodiscard]] std::string_view to_string(WarningKind kind) noexcept;

struct Warning {
    WarningKind kind = WarningKind::Missing;
    std::string key;
    std::string detail;
};

struct GeneralSettings {
    std::filesystem::path output_directory;
    std::string filename_template = "FrameCapture_%Y-%m-%d_%H-%M-%S";
    std::string language = "en";
};

struct VideoSettings {
    /// Locked to 1080p in v1 (SPEC.md §16.4). Present as keys so v1.1 can widen the
    /// allowed set without changing the file's shape.
    int width = 1920;
    int height = 1080;
    int fps = 60;
    Container container = Container::Mkv;
    VideoCodec codec = VideoCodec::H264;
    EncoderSelection encoder = EncoderSelection::Auto;
    RateControl rate_control = RateControl::Cqp;
    int cqp = 20;
    int bitrate_kbps = 20000;
    int gop_seconds = 2;
    int max_b_frames = 2;
    PacingMode pacing = PacingMode::Cfr;
    bool full_range = false;
};

struct AudioSettings {
    std::string device_id; ///< Empty = default render endpoint.
    ChannelLayoutSetting channel_layout = ChannelLayoutSetting::Auto;
    /// 0 = derive from the layout: 192 stereo / 384 5.1 / 512 7.1 (SPEC.md §8.5).
    int bitrate_kbps = 0;
    bool multitrack_enabled = false;
    int max_tracks = 6;

    /// SPEC.md §8.6's per-application targets: executable names, comma-separated,
    /// e.g. `chrome.exe, game.exe`. Empty means Tier B has nothing to record even
    /// when `multitrack_enabled` is true, which is the state a fresh install is in.
    ///
    /// **Names, not PIDs, and a string rather than a list.** §8.6 identifies a
    /// target by executable name because that is the identity that survives the
    /// application being restarted — "poll by executable name at 2 Hz" — and a PID
    /// written to a config file is meaningless by the next boot. A single string
    /// rather than a TOML array because §17's schema table has no array type, and
    /// adding one for a key the GUI presents as a text field would be a schema
    /// change carried by one caller.
    std::string multitrack_targets;

    /// Whether a per-application track follows its executable back if the application
    /// exits and is reopened mid-recording (SPEC.md §8.6).
    ///
    /// **Decided 2026-08-06 by the owner: it follows.** §8.6 says a track whose target
    /// exits "continues as pure silence to the end of the file" and leaves open whether
    /// it keeps *looking*; a track follows its executable, which is already the identity
    /// §8.6 uses for a target that has not started yet.
    ///
    /// The track's length is identical either way — its silence generator never stops —
    /// so this changes only whether a user who restarts an application gets the rest of
    /// its audio or silence. False gives §8.6's literal reading.
    bool multitrack_reattach = true;
};

/// Splits `audio.multitrack_targets` into executable names, trimmed, in order,
/// with blanks and duplicates removed.
///
/// One place decides what that string means. The GUI shows it, the engine acts on
/// it, and BUG-043's lesson is what happens when two layers each have their own
/// idea of one setting.
[[nodiscard]] std::vector<std::string> parse_multitrack_targets(std::string_view targets);

struct SegmentationSettings {
    /// SPEC.md §11 and CLAUDE.md §2 rule 7: default false, always.
    bool enabled = false;
    bool split_by_duration = true;
    int duration_minutes = 30;
    bool split_by_size = false;
    int size_mb = 4096;
};

struct AdvancedSettings {
    CaptureBackend capture_backend = CaptureBackend::Auto;
    bool capture_cursor = true;
    bool hdr_tonemap = true;
    LogLevelSetting log_level = LogLevelSetting::Info;
    std::string gpu_override; ///< Empty = automatic selection per SPEC.md §5.2.
};

struct UpdateSettings {
    bool check_enabled = true;
};

/// SPEC.md §16.5's rebindable global hotkeys (M9.6 F3).
///
/// **The engine stores these and never acts on them.** `RegisterHotKey` is the GUI's --
/// §16.5 says so, and the engine has no message pump to receive `WM_HOTKEY` on. What the
/// engine owns is the file, per §17, so the GUI reads these through `get_config` and
/// writes them through `save_config` like every other setting.
///
/// Sequences are stored as the user writes them (`"Ctrl+Shift+F9"`). Nothing here
/// validates them: whether a sequence is *parseable* is `hotkeys.parse_sequence`'s
/// question and whether it is *bindable* is Windows', and neither can be answered from
/// this process. An unparseable sequence is reported by the GUI and costs one
/// accelerator, not a failed load.
struct HotkeySettings {
    bool enabled = true;
    std::string start = "Ctrl+Shift+F9";
    /// Defaults to `start`'s sequence: one binding, dispatched as a toggle by current
    /// state. That is the behaviour this project shipped with, expressed so the two
    /// actions can be separated by anyone who wants them separate.
    std::string stop = "Ctrl+Shift+F9";
    std::string pause_resume = "Ctrl+Shift+F10";
};

/// The on-screen overlay's placement and behaviour (M9.6 F1, F2).
///
/// Also stored-not-acted-on: the pill and the toasts are GUI windows. What makes them
/// *safe* -- `WDA_EXCLUDEFROMCAPTURE`, so nothing here reaches the recording -- is the
/// GUI's `overlay.exclusion` module, and no engine code participates.
struct OverlaySettings {
    bool pill_enabled = true;
    OverlayCorner pill_corner = OverlayCorner::BottomRight;
    /// Display device path of the monitor to place the pill on, or empty for "whichever
    /// monitor is being captured". A path rather than an index, for SPEC.md §4.1's
    /// reason: indices reshuffle on hotplug.
    std::string pill_monitor;
    /// Virtual-desktop coordinates of the pill's top-left, or -1 for "never dragged;
    /// use `pill_corner`". See the schema table for why -1 rather than 0.
    int pill_x = -1;
    int pill_y = -1;

    bool toasts_enabled = true;
    OverlayCorner toast_corner = OverlayCorner::TopRight;
    /// How long an informational toast stays up. Warnings get longer and errors stay
    /// until dismissed; both are the GUI's policy, and this is the number they scale.
    int toast_duration_s = 4;
    int toast_max_visible = 4;
};

/// Which parts of the main window are on screen (M9.6 Phase 4, SPEC.md §16.2).
///
/// Stored-not-acted-on, like `HotkeySettings` and `OverlaySettings` above: the engine
/// has no window. What it owns is the file (§17), and the View menu's toggles have to
/// survive a restart to be worth having -- a layout that resets every launch is a
/// layout the user stops adjusting.
///
/// All-true defaults, because §16.2 draws the whole window and a panel is absent only
/// when a user chose to hide it. `always_on_top` is the exception and defaults off: it
/// changes how the window behaves against every *other* application on the machine, and
/// that is not a default anyone asked for.
struct WindowSettings {
    bool show_preview = true;
    bool show_sources = true;
    bool show_audio_mixer = true;
    bool show_controls = true;
    bool show_status = true;
    bool always_on_top = false;
};

/// The validated configuration, plus the document it came from.
///
/// The raw document is retained deliberately. Saving rewrites the *known* keys
/// inside this table and serialises the whole thing, so keys this build does not
/// recognise survive a load/save cycle in their original section. Splitting them out
/// and re-appending would duplicate section headers and produce a file that no
/// longer parses.
struct Config {
    int schema_version = kCurrentSchemaVersion;
    GeneralSettings general;
    VideoSettings video;
    AudioSettings audio;
    SegmentationSettings segmentation;
    AdvancedSettings advanced;
    UpdateSettings updates;
    HotkeySettings hotkeys;
    OverlaySettings overlay;
    WindowSettings window;

    /// Everything that was in the file, including unrecognised keys.
    toml::table document;
};

struct LoadOutcome {
    Config config;
    std::vector<Warning> warnings;
    bool file_existed = false;
    bool migrated = false;
    std::filesystem::path backup_path;
};

/// `%LOCALAPPDATA%\FrameCapture\config.toml` (SPEC.md §17 -- never Program Files).
[[nodiscard]] std::filesystem::path default_config_path();

/// Defaults for a machine with no config file. `general.output_directory` resolves
/// to the user's Videos folder.
[[nodiscard]] Config defaults();

/// Loads, migrates if needed, and validates.
///
/// A missing file is not an error: it yields defaults and `file_existed == false`,
/// which is what a first launch looks like. A file that cannot be *parsed* is an
/// error, because silently replacing an unparseable file would discard settings the
/// user can still fix by hand.
[[nodiscard]] Result<LoadOutcome> load(const std::filesystem::path& path = default_config_path());

/// Same validation path, from a string. This is the seam the unit tests use.
[[nodiscard]] Result<LoadOutcome> load_from_string(std::string_view toml_text,
                                                   const std::filesystem::path& origin = {});

/// Applies the validated values back into `config.document` and serialises it.
[[nodiscard]] std::string serialize(const Config& config);

/// Serialises and writes atomically (SPEC.md §17).
[[nodiscard]] Result<void> save(const Config& config, const std::filesystem::path& path = default_config_path());

/// Validates an already-parsed document in place, returning the typed result.
/// Exposed so the GUI can validate a candidate edit without touching disk.
[[nodiscard]] Config validate(const toml::table& document, std::vector<Warning>& warnings);

} // namespace fc::config
