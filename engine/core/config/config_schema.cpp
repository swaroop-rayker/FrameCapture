#include "core/config/config_schema.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <utility>

namespace fc::config {
namespace {

template <typename E>
struct Naming {
    E value;
    std::string_view name;
};

template <typename E, std::size_t N>
std::string_view name_of(const std::array<Naming<E>, N>& table, E value) noexcept {
    for (const Naming<E>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return "unknown";
}

template <typename E, std::size_t N>
std::optional<E> value_of(const std::array<Naming<E>, N>& table, std::string_view name) noexcept {
    for (const Naming<E>& entry : table) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return std::nullopt;
}

constexpr std::array kContainers{
    Naming<Container>{Container::Mp4, "mp4"},
    Naming<Container>{Container::Mkv, "mkv"},
};

constexpr std::array kCodecs{
    Naming<VideoCodec>{VideoCodec::H264, "h264"},
    Naming<VideoCodec>{VideoCodec::Hevc, "hevc"},
    Naming<VideoCodec>{VideoCodec::Av1, "av1"},
};

constexpr std::array kEncoders{
    Naming<EncoderSelection>{EncoderSelection::Auto, "auto"},
    Naming<EncoderSelection>{EncoderSelection::Nvenc, "nvenc"},
    Naming<EncoderSelection>{EncoderSelection::Amf, "amf"},
    Naming<EncoderSelection>{EncoderSelection::X264, "x264"},
};

constexpr std::array kRateControls{
    Naming<RateControl>{RateControl::Cqp, "cqp"},
    Naming<RateControl>{RateControl::Vbr, "vbr"},
    Naming<RateControl>{RateControl::Lossless, "lossless"},
};

constexpr std::array kPacings{
    Naming<PacingMode>{PacingMode::Cfr, "cfr"},
    Naming<PacingMode>{PacingMode::Vfr, "vfr"},
};

constexpr std::array kLayouts{
    Naming<ChannelLayoutSetting>{ChannelLayoutSetting::Auto, "auto"},
    Naming<ChannelLayoutSetting>{ChannelLayoutSetting::Stereo, "stereo"},
    Naming<ChannelLayoutSetting>{ChannelLayoutSetting::Surround51, "5.1"},
    Naming<ChannelLayoutSetting>{ChannelLayoutSetting::Surround71, "7.1"},
};

constexpr std::array kBackends{
    Naming<CaptureBackend>{CaptureBackend::Auto, "auto"},
    Naming<CaptureBackend>{CaptureBackend::Wgc, "wgc"},
    Naming<CaptureBackend>{CaptureBackend::Dda, "dda"},
};

constexpr std::array kLogLevels{
    Naming<LogLevelSetting>{LogLevelSetting::Trace, "trace"},
    Naming<LogLevelSetting>{LogLevelSetting::Debug, "debug"},
    Naming<LogLevelSetting>{LogLevelSetting::Info, "info"},
    Naming<LogLevelSetting>{LogLevelSetting::Warn, "warn"},
    Naming<LogLevelSetting>{LogLevelSetting::Error, "error"},
    Naming<LogLevelSetting>{LogLevelSetting::Critical, "critical"},
};

constexpr std::array kOverlayCorners{
    Naming<OverlayCorner>{OverlayCorner::TopLeft, "top-left"},
    Naming<OverlayCorner>{OverlayCorner::TopRight, "top-right"},
    Naming<OverlayCorner>{OverlayCorner::BottomLeft, "bottom-left"},
    Naming<OverlayCorner>{OverlayCorner::BottomRight, "bottom-right"},
};

// clang-format off
constexpr std::array kSchema{
    // path                              type                     min      max        allowed                              default                              since
    KeySpec{"schema_version",            KeyType::Integer,          1,      1000,     "",                                  "2",                                   1},

    // [general] -- SPEC.md §16.4
    KeySpec{"general.output_directory",   KeyType::Path,             0,         0,     "",                                  "<Videos>\\FrameCapture",              1},
    KeySpec{"general.filename_template",  KeyType::String,           0,         0,     "",                                  "FrameCapture_%Y-%m-%d_%H-%M-%S",      1},
    KeySpec{"general.language",           KeyType::String,           0,         0,     "",                                  "en",                                  1},

    // [video] -- SPEC.md §6, §7, §9, §16.4
    KeySpec{"video.width",                KeyType::IntegerChoice,    0,         0,     "1920",                              "1920",                                1},
    KeySpec{"video.height",               KeyType::IntegerChoice,    0,         0,     "1080",                              "1080",                                1},
    KeySpec{"video.fps",                  KeyType::IntegerChoice,    0,         0,     "30,60",                             "60",                                  1},
    KeySpec{"video.container",            KeyType::Enum,             0,         0,     "mp4,mkv",                           "mkv",                                 1},
    KeySpec{"video.codec",                KeyType::Enum,             0,         0,     "h264,hevc,av1",                     "h264",                                1},
    KeySpec{"video.encoder",              KeyType::Enum,             0,         0,     "auto,nvenc,amf,x264",               "auto",                                1},
    KeySpec{"video.rate_control",         KeyType::Enum,             0,         0,     "cqp,vbr,lossless",                  "cqp",                                 1},
    KeySpec{"video.cqp",                  KeyType::Integer,          0,        51,     "",                                  "20",                                  1},
    KeySpec{"video.bitrate_kbps",         KeyType::Integer,       1000,    200000,     "",                                  "20000",                               1},
    KeySpec{"video.gop_seconds",          KeyType::Integer,          1,        10,     "",                                  "2",                                   1},
    KeySpec{"video.max_b_frames",         KeyType::Integer,          0,         4,     "",                                  "2",                                   1},
    KeySpec{"video.pacing",               KeyType::Enum,             0,         0,     "cfr,vfr",                           "cfr",                                 1},
    KeySpec{"video.full_range",           KeyType::Boolean,          0,         0,     "",                                  "false",                               1},

    // [audio] -- SPEC.md §8.5, §8.6
    KeySpec{"audio.device_id",            KeyType::String,           0,         0,     "",                                  "\"\" (default endpoint)",             1},
    KeySpec{"audio.channel_layout",       KeyType::Enum,             0,         0,     "auto,stereo,5.1,7.1",               "auto",                                1},
    KeySpec{"audio.bitrate_kbps",         KeyType::Integer,          0,       640,     "",                                  "0 (auto per layout)",                 1},
    KeySpec{"audio.multitrack_enabled",   KeyType::Boolean,          0,         0,     "",                                  "false",                               1},
    KeySpec{"audio.max_tracks",           KeyType::Integer,          1,         6,     "",                                  "6",                                   1},
    KeySpec{"audio.multitrack_targets",   KeyType::String,           0,         0,     "",                                  "\"\" (no per-application tracks)",    1},
    KeySpec{"audio.multitrack_reattach",  KeyType::Boolean,          0,         0,     "",                                  "true",                                1},

    // [segmentation] -- SPEC.md §11. Default OFF, always.
    KeySpec{"segmentation.enabled",           KeyType::Boolean,      0,         0,     "",                                  "false",                               1},
    KeySpec{"segmentation.split_by_duration", KeyType::Boolean,      0,         0,     "",                                  "true",                                1},
    KeySpec{"segmentation.duration_minutes",  KeyType::Integer,      1,      1440,     "",                                  "30",                                  1},
    KeySpec{"segmentation.split_by_size",     KeyType::Boolean,      0,         0,     "",                                  "false",                               1},
    KeySpec{"segmentation.size_mb",           KeyType::Integer,    100,    102400,     "",                                  "4096",                                1},

    // [advanced] -- SPEC.md §4.3, §6, §16.4, §18
    KeySpec{"advanced.capture_backend",   KeyType::Enum,             0,         0,     "auto,wgc,dda",                      "auto",                                1},
    KeySpec{"advanced.capture_cursor",    KeyType::Boolean,          0,         0,     "",                                  "true",                                1},
    KeySpec{"advanced.hdr_tonemap",       KeyType::Boolean,          0,         0,     "",                                  "true",                                1},
    KeySpec{"advanced.log_level",         KeyType::Enum,             0,         0,     "trace,debug,info,warn,error,critical", "info",                             1},
    KeySpec{"advanced.gpu_override",      KeyType::String,           0,         0,     "",                                  "\"\" (automatic)",                    1},

    // [updates] -- SPEC.md §21.3
    KeySpec{"updates.check_enabled",      KeyType::Boolean,          0,         0,     "",                                  "true",                                1},

    // [hotkeys] -- SPEC.md §16.5, M9.6 F3. Sequences are validated by the GUI, which is
    // what calls `RegisterHotKey` and is therefore the only thing that can tell a
    // well-formed binding from a bindable one. Stored as free strings for that reason:
    // a schema `allowed` list would have to enumerate every key combination there is.
    //
    // `start` and `stop` share a default deliberately -- that one sequence is the
    // start/stop toggle this project shipped with, and expressing it as two identical
    // bindings is what keeps it working while making the two actions separable.
    KeySpec{"hotkeys.enabled",            KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"hotkeys.start",              KeyType::String,           0,         0,     "",                                  "Ctrl+Shift+F9",                       2},
    KeySpec{"hotkeys.stop",               KeyType::String,           0,         0,     "",                                  "Ctrl+Shift+F9",                       2},
    KeySpec{"hotkeys.pause_resume",       KeyType::String,           0,         0,     "",                                  "Ctrl+Shift+F10",                      2},

    // [overlay] -- M9.6 F1 and F2. Drawn by the GUI, stored here because §17 makes this
    // file the single source of truth for settings.
    //
    // `pill_x`/`pill_y` are -1 until the user drags the pill somewhere, at which point
    // they are virtual-desktop coordinates. -1 rather than 0 because 0,0 is a real
    // position -- the top-left of the primary monitor -- and "unplaced" has to be
    // distinguishable from "placed in the corner the user happened to drag it to".
    KeySpec{"overlay.pill_enabled",       KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"overlay.pill_corner",        KeyType::Enum,             0,         0,     "top-left,top-right,bottom-left,bottom-right", "bottom-right",              2},
    KeySpec{"overlay.pill_monitor",       KeyType::String,           0,         0,     "",                                  "\"\" (the captured monitor)",         2},
    KeySpec{"overlay.pill_x",             KeyType::Integer,      -32768,     32767,     "",                                  "-1 (unplaced)",                       2},
    KeySpec{"overlay.pill_y",             KeyType::Integer,      -32768,     32767,     "",                                  "-1 (unplaced)",                       2},
    KeySpec{"overlay.toasts_enabled",     KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"overlay.toast_corner",       KeyType::Enum,             0,         0,     "top-left,top-right,bottom-left,bottom-right", "top-right",                 2},
    KeySpec{"overlay.toast_duration_s",   KeyType::Integer,          1,        60,     "",                                  "4",                                   2},
    KeySpec{"overlay.toast_max_visible",  KeyType::Integer,          1,         8,     "",                                  "4",                                   2},

    // [window] -- M9.6 Phase 4's View menu. Which parts of the main window are shown.
    //
    // Stored for the same reason [hotkeys] and [overlay] are: the engine never reads
    // these, and §17 makes this file the only settings store, so a second one for the
    // GUI's own keys would be the second source of truth §17 exists to prevent.
    //
    // Every one defaults to true. The window §16.2 specifies is the whole window, and a
    // panel is hidden only because a user hid it -- so a config file with no [window]
    // section, which is every file written before this milestone, loads the layout the
    // spec draws.
    KeySpec{"window.show_preview",        KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"window.show_sources",        KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"window.show_audio_mixer",    KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"window.show_controls",       KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"window.show_status",         KeyType::Boolean,          0,         0,     "",                                  "true",                                2},
    KeySpec{"window.always_on_top",       KeyType::Boolean,          0,         0,     "",                                  "false",                               2},
};
// clang-format on

} // namespace

std::string_view to_string(Container value) noexcept {
    return name_of(kContainers, value);
}

std::string_view extension_for(Container value) noexcept {
    switch (value) {
    case Container::Mp4:
        return ".mp4";
    case Container::Mkv:
        break;
    }
    return ".mkv";
}

std::optional<Container> container_from_extension(std::string_view extension) noexcept {
    // Compared in place rather than by lowering into a `std::string`: this is `noexcept`,
    // and an allocation is not -- clang-tidy's `bugprone-exception-escape` is right to
    // reject the tidier-looking version. Nothing here can throw.
    const auto equals_ignoring_case = [](std::string_view text, std::string_view lowercase) noexcept {
        if (text.size() != lowercase.size()) {
            return false;
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (static_cast<char>(std::tolower(c)) != lowercase[i]) {
                return false;
            }
        }
        return true;
    };

    if (equals_ignoring_case(extension, ".mp4")) {
        return Container::Mp4;
    }
    if (equals_ignoring_case(extension, ".mkv")) {
        return Container::Mkv;
    }
    return std::nullopt;
}

std::string_view to_string(VideoCodec value) noexcept {
    return name_of(kCodecs, value);
}

std::string_view to_string(EncoderSelection value) noexcept {
    return name_of(kEncoders, value);
}

std::string_view to_string(RateControl value) noexcept {
    return name_of(kRateControls, value);
}

std::string_view to_string(PacingMode value) noexcept {
    return name_of(kPacings, value);
}

std::string_view to_string(ChannelLayoutSetting value) noexcept {
    return name_of(kLayouts, value);
}

std::string_view to_string(CaptureBackend value) noexcept {
    return name_of(kBackends, value);
}

std::string_view to_string(LogLevelSetting value) noexcept {
    return name_of(kLogLevels, value);
}

std::string_view to_string(OverlayCorner value) noexcept {
    return name_of(kOverlayCorners, value);
}

std::optional<Container> container_from_string(std::string_view text) noexcept {
    return value_of(kContainers, text);
}

std::optional<VideoCodec> video_codec_from_string(std::string_view text) noexcept {
    return value_of(kCodecs, text);
}

std::optional<EncoderSelection> encoder_from_string(std::string_view text) noexcept {
    return value_of(kEncoders, text);
}

std::optional<RateControl> rate_control_from_string(std::string_view text) noexcept {
    return value_of(kRateControls, text);
}

std::optional<PacingMode> pacing_from_string(std::string_view text) noexcept {
    return value_of(kPacings, text);
}

std::optional<ChannelLayoutSetting> channel_layout_from_string(std::string_view text) noexcept {
    return value_of(kLayouts, text);
}

std::optional<CaptureBackend> capture_backend_from_string(std::string_view text) noexcept {
    return value_of(kBackends, text);
}

std::optional<LogLevelSetting> log_level_from_string(std::string_view text) noexcept {
    return value_of(kLogLevels, text);
}

std::optional<OverlayCorner> overlay_corner_from_string(std::string_view text) noexcept {
    return value_of(kOverlayCorners, text);
}

std::span<const KeySpec> schema_keys() noexcept {
    return {kSchema.data(), kSchema.size()};
}

const KeySpec* find_key(std::string_view path) noexcept {
    for (const KeySpec& spec : kSchema) {
        if (spec.path == path) {
            return &spec;
        }
    }
    return nullptr;
}

} // namespace fc::config
