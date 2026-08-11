#include "core/util/thread_utils.h"

#include <windows.h>

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace fc {
namespace {

// Registry lifetime note: intentionally a function-local static so it outlives
// any thread that might touch it during shutdown. Reads happen on the log worker
// thread, writes only when a thread is created or destroyed -- never on a hot
// path, so a shared_mutex is the right trade here.
struct NameRegistry {
    std::shared_mutex mutex;
    std::unordered_map<std::size_t, std::string> names;
};

NameRegistry& registry() {
    static NameRegistry instance;
    return instance;
}

std::size_t current_id() noexcept {
    return static_cast<std::size_t>(GetCurrentThreadId());
}

std::wstring widen(std::string_view narrow) {
    if (narrow.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow.data(), static_cast<int>(narrow.size()), wide.data(), needed);
    return wide;
}

} // namespace

void set_thread_name(std::string_view name) {
    const std::wstring wide = widen(name);
    if (!wide.empty()) {
        // Failure is not actionable -- a thread without a description is a
        // debugging inconvenience, not a runtime fault -- but it must not be
        // silent either, so the HRESULT is deliberately discarded here and the
        // registry below remains authoritative for logging.
        static_cast<void>(SetThreadDescription(GetCurrentThread(), wide.c_str()));
    }

    NameRegistry& reg = registry();
    const std::unique_lock lock(reg.mutex);
    reg.names[current_id()] = std::string{name};
}

std::string current_thread_name() {
    return thread_name_for(current_id());
}

std::string thread_name_for(std::size_t thread_id) {
    NameRegistry& reg = registry();
    const std::shared_lock lock(reg.mutex);
    const auto it = reg.names.find(thread_id);
    return it != reg.names.end() ? it->second : std::string{"unnamed"};
}

void clear_thread_name() {
    NameRegistry& reg = registry();
    const std::unique_lock lock(reg.mutex);
    reg.names.erase(current_id());
}

} // namespace fc
