#pragma once

#include "core/error/fc_error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fc {

/// The key-value map that SPEC.md §18 requires alongside every log line.
///
/// Renders as `key=value key2="value with spaces"`. Keys are expected to be
/// stable identifiers -- ERROR_CODES.md and the acceptance suite grep for them,
/// so renaming a key is a breaking change to the log contract.
///
/// **This type allocates.** Per CLAUDE.md §4 there is no logging above TRACE on
/// the capture or audio threads, and TRACE call sites are guarded by
/// `should_log()` in the FC_LOG_* macros, so no allocation happens on a hot path
/// with tracing off. Do not reach for LogFields inside a frame loop.
class LogFields {
public:
    LogFields() = default;

    LogFields& add(std::string_view key, std::string_view value);
    LogFields& add(std::string_view key, const char* value);
    LogFields& add(std::string_view key, bool value);
    LogFields& add(std::string_view key, std::int64_t value);
    LogFields& add(std::string_view key, int value);
    LogFields& add(std::string_view key, std::uint64_t value);
    LogFields& add(std::string_view key, double value);

    /// Adds the two fields that make an error greppable: `error=<NAME>` and
    /// `code=<numeric>`. This pairing is the log signature documented in
    /// docs/ERROR_CODES.md -- always use it rather than formatting by hand.
    LogFields& add_error(FcError error);

    [[nodiscard]] bool empty() const noexcept {
        return pairs_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return pairs_.size();
    }

    /// Appends `key=value` pairs, space-separated, to `out`. Values containing a
    /// space, quote, or equals sign are double-quoted with embedded quotes
    /// escaped, so a line stays parseable.
    void render_into(std::string& out) const;

    [[nodiscard]] std::string render() const;

private:
    std::vector<std::pair<std::string, std::string>> pairs_;
};

} // namespace fc
