#include "core/logging/ffmpeg_log.h"

#include "core/logging/logger.h"

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace fc::log {
namespace {

std::atomic<bool> g_installed{false};

Level map_level(int av_level) noexcept {
    if (av_level <= AV_LOG_FATAL) {
        return Level::Critical;
    }
    if (av_level <= AV_LOG_ERROR) {
        return Level::Error;
    }
    if (av_level <= AV_LOG_WARNING) {
        return Level::Warn;
    }
    if (av_level <= AV_LOG_INFO) {
        return Level::Info;
    }
    if (av_level <= AV_LOG_VERBOSE) {
        return Level::Debug;
    }
    return Level::Trace;
}

// NOLINTNEXTLINE(cert-dcl50-cpp): the C callback signature is fixed by libavutil.
void callback(void* avcl, int level, const char* format, va_list args) {
    if (level > av_log_get_level()) {
        return;
    }

    const Level mapped = map_level(level);
    if (!should_log(mapped)) {
        return;
    }

    // Fixed buffer: this can be called from the encode thread, where an allocation
    // is unwelcome (CLAUDE.md §4). FFmpeg messages are short; truncation is
    // preferable to a heap round trip on the hot path.
    std::array<char, 1024> buffer{};
    int prefix = 1;
    const int written =
        av_log_format_line2(avcl, level, format, args, buffer.data(), static_cast<int>(buffer.size()), &prefix);
    if (written <= 0) {
        return;
    }

    // av_log_format_line2 returns the length it *would* have written, so clamp to
    // what the buffer actually holds.
    const std::size_t produced = std::min(static_cast<std::size_t>(written), buffer.size() - 1);
    std::string_view text{buffer.data(), produced};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return;
    }

    write(mapped, Subsystem::Encode, text, LogFields{}.add("origin", "libav"));
}

} // namespace

void install_ffmpeg_bridge() {
    if (g_installed.exchange(true)) {
        return;
    }
    // AV_LOG_INFO is FFmpeg's default; our own level gate in the callback decides
    // what actually gets written, so nothing is lost by leaving this permissive.
    av_log_set_level(AV_LOG_INFO);
    av_log_set_callback(&callback);
}

void remove_ffmpeg_bridge() {
    if (!g_installed.exchange(false)) {
        return;
    }
    av_log_set_callback(&av_log_default_callback);
}

} // namespace fc::log
