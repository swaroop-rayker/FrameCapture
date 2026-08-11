#include "core/logging/log_fields.h"

#include <charconv>
#include <cstdio>

namespace fc {
namespace {

bool needs_quoting(std::string_view value) {
    if (value.empty()) {
        return true; // `key=` alone is ambiguous; `key=""` is not.
    }
    return value.find_first_of(" \t\"=\r\n") != std::string_view::npos;
}

void append_value(std::string& out, std::string_view value) {
    if (!needs_quoting(value)) {
        out.append(value);
        return;
    }

    out.push_back('"');
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        // Newlines would break the one-event-per-line contract.
        out.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    out.push_back('"');
}

} // namespace

LogFields& LogFields::add(std::string_view key, std::string_view value) {
    pairs_.emplace_back(std::string{key}, std::string{value});
    return *this;
}

LogFields& LogFields::add(std::string_view key, const char* value) {
    return add(key, value != nullptr ? std::string_view{value} : std::string_view{"<null>"});
}

LogFields& LogFields::add(std::string_view key, bool value) {
    return add(key, value ? std::string_view{"true"} : std::string_view{"false"});
}

LogFields& LogFields::add(std::string_view key, std::int64_t value) {
    char buffer[24] = {};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return add(key, std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)});
}

LogFields& LogFields::add(std::string_view key, int value) {
    return add(key, static_cast<std::int64_t>(value));
}

LogFields& LogFields::add(std::string_view key, std::uint64_t value) {
    char buffer[24] = {};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return add(key, std::string_view{buffer, static_cast<std::size_t>(result.ptr - buffer)});
}

LogFields& LogFields::add(std::string_view key, double value) {
    // Fixed 3 decimals: these are almost always milliseconds or megabytes per
    // second, and a stable width keeps columns readable when grepping.
    char buffer[32] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    if (written <= 0) {
        return add(key, std::string_view{"nan"});
    }
    return add(key, std::string_view{buffer, static_cast<std::size_t>(written)});
}

LogFields& LogFields::add_error(FcError error) {
    add("error", error_name(error));
    add("code", static_cast<std::int64_t>(error_code(error)));
    return *this;
}

void LogFields::render_into(std::string& out) const {
    for (const auto& [key, value] : pairs_) {
        if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
        out.append(key);
        out.push_back('=');
        append_value(out, value);
    }
}

std::string LogFields::render() const {
    std::string out;
    render_into(out);
    return out;
}

} // namespace fc
