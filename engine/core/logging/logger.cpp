#include "core/logging/logger.h"

#include "core/logging/ring_sink.h"
#include "core/util/thread_utils.h"

#include <windows.h>
// shlobj.h must follow windows.h.
#include <shlobj.h>

#include <spdlog/async.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <system_error>

namespace fc::log {
namespace {

/// Largest `Subsystem` value + 1. A dense array keyed by the enum value keeps
/// `write()` lock-free; `static_assert`-style guarding lives in logger_for().
constexpr std::size_t kSubsystemSlots = 32;

spdlog::level::level_enum to_spdlog(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

/// Resolves `%N` to the *originating* thread's name.
///
/// The lookup is by `msg.thread_id` rather than by thread-local state because
/// this formatter runs on the async worker thread. See thread_utils.h.
class ThreadNameFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm& /*local_time*/,
                spdlog::memory_buf_t& dest) override {
        const std::string name = thread_name_for(msg.thread_id);
        dest.append(name.data(), name.data() + name.size());
    }

    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<ThreadNameFlag>();
    }
};

struct State {
    std::mutex mutex;
    bool initialized = false;
    std::string session_id;
    std::atomic<Level> level{Level::Info};

    /// Messages handed to spdlog. Paired with RingSink::processed() and the pool's
    /// overrun counter to build a real flush barrier.
    std::atomic<std::uint64_t> submitted{0};

    std::shared_ptr<RingSink> ring;
    std::shared_ptr<spdlog::details::thread_pool> pool;
    std::vector<spdlog::sink_ptr> sinks;
    std::array<std::shared_ptr<spdlog::logger>, kSubsystemSlots> loggers{};
    std::filesystem::path log_path;
};

State& state() {
    static State instance;
    return instance;
}

/// `FC_LOG_LEVEL`, or `nullopt` when unset or unrecognised.
///
/// Declared here so `init` reads it, but the *parsing* is `log::level_from_string` --
/// the same function SPEC.md §15.1's `set_log_level` uses, so the two can never disagree
/// about what a level is called.
std::optional<Level> level_from_environment() {
    std::array<char, 32> buffer{};
    const DWORD length = ::GetEnvironmentVariableA("FC_LOG_LEVEL", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }
    return level_from_string(std::string_view{buffer.data(), length});
}

std::string generate_session_id() {
    // Not a UUID on purpose: 16 hex characters are short enough to sit on every
    // log line and unique enough to correlate a crash report with a log file.
    const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device device;
    const auto entropy = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    const std::uint64_t mixed = now ^ entropy ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 48);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        id[15 - i] = kHex[(mixed >> (i * 4)) & 0xFu];
    }
    return id;
}

/// Subsystems that get their own spdlog logger. The logger *name* is what `%n`
/// prints, which is how the mandatory `subsystem` field reaches every line
/// without a per-message lookup.
constexpr std::array kSubsystems = {
    Subsystem::None,  Subsystem::Capture, Subsystem::Gpu,      Subsystem::Audio,    Subsystem::Encode,
    Subsystem::Mux,   Subsystem::Io,      Subsystem::Ipc,      Subsystem::Internal, Subsystem::Clock,
    Subsystem::Color, Subsystem::Config,  Subsystem::Pipeline, Subsystem::App,      Subsystem::Health,
};

spdlog::logger* logger_for(Subsystem subsystem) noexcept {
    State& s = state();
    if (!s.initialized) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(subsystem);
    if (index >= kSubsystemSlots) {
        return s.loggers[static_cast<std::size_t>(Subsystem::Internal)].get();
    }
    if (s.loggers[index] != nullptr) {
        return s.loggers[index].get();
    }
    return s.loggers[static_cast<std::size_t>(Subsystem::Internal)].get();
}

std::string build_pattern(std::string_view session) {
    // SPEC.md §18 mandates, on every line: ISO-8601 UTC timestamp, level, thread
    // name, subsystem, session_id, message, key-value map. The map is rendered
    // into %v by write().
    std::string pattern = "%Y-%m-%dT%H:%M:%S.%eZ | %-8l | %-12N | %-9n | ";
    for (const char c : session) {
        // A '%' here would be read as a pattern flag. Session ids are hex, so
        // this never fires -- it is here so that it cannot start firing.
        pattern.push_back(c == '%' ? '_' : c);
    }
    pattern += " | %v";
    return pattern;
}

/// Longest we will wait for the async worker to drain. A flush must not become an
/// unbounded wait -- if the worker is wedged, losing log tail is preferable to
/// hanging a shutdown that still has a file to finalise.
constexpr auto kFlushTimeout = std::chrono::seconds{2};

/// Waits until every message submitted before the call has been written by every
/// sink, then flushes the sinks' own buffers. Caller must hold State::mutex.
void flush_locked(State& s) {
    if (s.ring == nullptr || s.pool == nullptr) {
        return;
    }

    const std::uint64_t target = s.submitted.load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + kFlushTimeout;

    while (std::chrono::steady_clock::now() < deadline) {
        // Dropped messages never reach a sink, so they have to be counted as
        // accounted-for or this loop would always time out under backpressure.
        const std::uint64_t done = s.ring->processed() + s.pool->overrun_counter();
        if (done >= target) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }

    // Flush the sinks directly rather than through the queue: spdlog's async flush
    // is fire-and-forget, so posting one would tell us nothing. The _mt sinks are
    // safe to flush from this thread.
    for (const spdlog::sink_ptr& sink : s.sinks) {
        sink->flush();
    }
}

} // namespace

std::string_view to_string(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRITICAL";
    case Level::Off:
        return "OFF";
    }
    return "UNKNOWN";
}

std::optional<Level> level_from_string(std::string_view name) noexcept {
    for (const Level candidate :
         {Level::Trace, Level::Debug, Level::Info, Level::Warn, Level::Error, Level::Critical, Level::Off}) {
        const std::string_view spelling = to_string(candidate);
        if (spelling.size() != name.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < spelling.size(); ++i) {
            // ASCII fold. The names are all ASCII by construction, so this needs none of
            // the locale machinery `std::tolower` drags in -- and `std::tolower` on a
            // possibly-negative `char` is undefined, which is a trap this avoids.
            const char a = static_cast<char>(spelling[i] | 0x20);
            const char b = static_cast<char>(name[i] | 0x20);
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::filesystem::path default_log_directory() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw)) && raw != nullptr) {
        const std::filesystem::path base{raw};
        CoTaskMemFree(raw);
        return base / "FrameCapture" / "logs";
    }
    if (raw != nullptr) {
        CoTaskMemFree(raw);
    }

    // SHGetKnownFolderPath can fail in a stripped environment; the env var is the
    // documented fallback and is what SPEC.md §18 names anyway.
    wchar_t buffer[MAX_PATH] = {};
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (written > 0 && written < MAX_PATH) {
        return std::filesystem::path{buffer} / "FrameCapture" / "logs";
    }
    return std::filesystem::path{"."} / "FrameCapture" / "logs";
}

Result<void> init(const Config& config) {
    State& s = state();
    const std::lock_guard lock(s.mutex);

    if (s.initialized) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    const std::filesystem::path directory = config.directory.empty() ? default_log_directory() : config.directory;

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return FcError::IO_DIRECTORY_CREATE_FAILED;
    }

    s.session_id = config.session_id.empty() ? generate_session_id() : config.session_id;
    s.log_path = directory / config.filename;

    std::vector<spdlog::sink_ptr> sinks;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(s.log_path.string(),
                                                                               config.max_file_size, config.max_files));
    } catch (const spdlog::spdlog_ex&) {
        // Specific type, not catch(...): a sink that cannot open its file is a
        // recoverable, classifiable failure (CLAUDE.md §4).
        s.session_id.clear();
        s.log_path.clear();
        return FcError::IO_FILE_OPEN_FAILED;
    }

    if (config.enable_msvc_sink) {
        sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
    }

    // The ring sink goes LAST on purpose. flush_locked() uses its completion count
    // as the barrier, and sinks are driven in order for a given message, so "the
    // ring wrote message N" implies every earlier sink also wrote it.
    s.ring = std::make_shared<RingSink>(config.ring_capacity);
    sinks.push_back(s.ring);

    const std::string pattern = build_pattern(s.session_id);
    for (const spdlog::sink_ptr& sink : sinks) {
        auto formatter = std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc,
                                                                     std::string{spdlog::details::os::default_eol});
        formatter->add_flag<ThreadNameFlag>('N').set_pattern(pattern);
        sink->set_formatter(std::move(formatter));
    }

    // Bounded queue with drop-oldest. A blocking queue would let a slow disk
    // stall the capture thread, which CLAUDE.md §4 forbids outright.
    auto pool = std::make_shared<spdlog::details::thread_pool>(config.async_queue_size, 1u,
                                                               [] { set_thread_name("logworker"); });

    for (const Subsystem subsystem : kSubsystems) {
        auto logger =
            std::make_shared<spdlog::async_logger>(std::string{fc::to_string(subsystem)}, sinks.begin(), sinks.end(),
                                                   pool, spdlog::async_overflow_policy::overrun_oldest);
        logger->set_level(spdlog::level::trace); // gating happens in should_log()
        logger->flush_on(spdlog::level::err);
        s.loggers[static_cast<std::size_t>(subsystem)] = std::move(logger);
    }

    // async_logger holds only a weak_ptr to the pool, so State owns the strong
    // reference. Keeping it here rather than in a function-local static makes
    // teardown ordering explicit instead of dependent on static destruction.
    s.pool = std::move(pool);
    s.sinks = std::move(sinks);

    s.submitted.store(0, std::memory_order_relaxed);

    // `FC_LOG_LEVEL` overrides the configured level, so a developer can raise verbosity
    // without editing config or rebuilding -- SPEC.md §21.2's `run-dev.ps1` sets it, and
    // so does anyone reproducing a bug report.
    //
    // The environment wins over `config.level` deliberately: it is the more specific and
    // more transient of the two, and a caller that passed a level explicitly is the one
    // most likely to be a *test*, which sets the directory and session id in the same
    // breath. An unrecognised value is ignored rather than refused -- a typo in an
    // environment variable must not stop the engine from starting.
    if (const std::optional<Level> from_env = level_from_environment(); from_env.has_value()) {
        s.level.store(*from_env, std::memory_order_relaxed);
    } else {
        s.level.store(config.level, std::memory_order_relaxed);
    }

    s.initialized = true;
    return ok();
}

void shutdown() {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    if (!s.initialized) {
        return;
    }

    // Drain before tearing anything down: the tail of the log is the part you
    // need after a failure, so losing it at shutdown defeats the purpose.
    flush_locked(s);

    for (auto& logger : s.loggers) {
        logger.reset();
    }

    // Order matters: loggers, then the pool (its destructor joins the worker),
    // then the sinks the worker was writing to.
    s.pool.reset();
    s.sinks.clear();
    s.ring.reset();

    s.log_path.clear();
    s.session_id.clear();
    s.initialized = false;
}

bool is_initialized() noexcept {
    return state().initialized;
}

std::string_view session_id() noexcept {
    return state().session_id;
}

void set_level(Level level) noexcept {
    state().level.store(level, std::memory_order_relaxed);
}

Level level() noexcept {
    return state().level.load(std::memory_order_relaxed);
}

bool should_log(Level level) noexcept {
    const State& s = state();
    if (!s.initialized) {
        return false;
    }
    const Level threshold = s.level.load(std::memory_order_relaxed);
    return threshold != Level::Off && level >= threshold;
}

void write(Level level, Subsystem subsystem, std::string_view message) {
    spdlog::logger* logger = logger_for(subsystem);
    if (logger == nullptr) {
        return;
    }
    logger->log(to_spdlog(level), spdlog::string_view_t{message.data(), message.size()});
    state().submitted.fetch_add(1, std::memory_order_release);
}

void write(Level level, Subsystem subsystem, std::string_view message, const LogFields& fields) {
    if (fields.empty()) {
        write(level, subsystem, message);
        return;
    }

    std::string combined;
    combined.reserve(message.size() + (32u * fields.size()));
    combined.append(message);
    combined.push_back(' ');
    fields.render_into(combined);
    write(level, subsystem, combined);
}

void flush() {
    State& s = state();
    const std::lock_guard lock(s.mutex);
    flush_locked(s);
}

std::vector<std::string> ring_snapshot() {
    const RingSink* sink = ring_sink();
    return sink != nullptr ? sink->snapshot() : std::vector<std::string>{};
}

std::size_t ring_size() {
    const RingSink* sink = ring_sink();
    return sink != nullptr ? sink->size() : 0;
}

RingSink* ring_sink() noexcept {
    return state().ring.get();
}

std::filesystem::path log_file_path() {
    return state().log_path;
}

} // namespace fc::log
