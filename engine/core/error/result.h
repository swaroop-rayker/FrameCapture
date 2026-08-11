#pragma once

#include "core/error/fc_error.h"

#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace fc {

/// Reports misuse of `Result` -- calling `value()` on an error state, or
/// `error()` on a value state. This is a programming defect, not a runtime
/// failure, so it is not itself recoverable: it logs at CRITICAL and terminates,
/// which the crash handler turns into a minidump you can read.
///
/// Declared here and defined in result.cpp so that result.h stays free of any
/// dependency on the logger.
[[noreturn]] void result_misuse(std::string_view what, FcError error) noexcept;

/// Expected-style result for recoverable failures (SPEC.md §19).
///
/// Built on std::variant deliberately: a hand-rolled discriminated union is a
/// classic source of lifetime bugs, and this type sits under every subsystem.
///
/// `[[nodiscard]]` on the class means *any* function returning a Result warns if
/// its result is dropped. With /WX on fc_core that is a build error, which is the
/// point -- an ignored Result is an unchecked failure.
template <typename T, typename E = FcError>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<E>>,
                  "Result<T, E> requires distinct T and E so the active state is unambiguous");
    static_assert(!std::is_reference_v<T>, "Result cannot hold a reference; wrap it or use a pointer");

public:
    using value_type = T;
    using error_type = E;

    // Implicit on purpose: `return texture;` and `return FcError::X;` both read
    // naturally at call sites and cannot be confused for one another.
    constexpr Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}

    constexpr Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] constexpr const T& value() const& {
        if (!has_value()) {
            result_misuse("Result::value() called on an error state", error_unchecked());
        }
        return std::get<0>(storage_);
    }

    [[nodiscard]] constexpr T&& value() && {
        if (!has_value()) {
            result_misuse("Result::value() called on an error state", error_unchecked());
        }
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] constexpr const E& error() const& {
        if (has_value()) {
            result_misuse("Result::error() called on a value state", FcError::INTERNAL_INVARIANT_VIOLATED);
        }
        return std::get<1>(storage_);
    }

    [[nodiscard]] constexpr const T& operator*() const& {
        return value();
    }

    [[nodiscard]] constexpr T&& operator*() && {
        return std::move(*this).value();
    }

    [[nodiscard]] constexpr const T* operator->() const {
        return &value();
    }

    /// The one accessor that never terminates. Use it when a default is genuinely
    /// correct -- not to paper over an error you should have handled.
    template <typename U>
    [[nodiscard]] constexpr T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

    /// Applies `fn` to the value, propagating the error untouched.
    template <typename Fn>
    [[nodiscard]] constexpr auto map(Fn&& fn) const& -> Result<std::invoke_result_t<Fn, const T&>, E> {
        using Mapped = Result<std::invoke_result_t<Fn, const T&>, E>;
        return has_value() ? Mapped{std::forward<Fn>(fn)(std::get<0>(storage_))} : Mapped{error_unchecked()};
    }

    /// Chains an operation that itself returns a Result.
    template <typename Fn>
    [[nodiscard]] constexpr auto and_then(Fn&& fn) const& -> std::invoke_result_t<Fn, const T&> {
        using Chained = std::invoke_result_t<Fn, const T&>;
        return has_value() ? std::forward<Fn>(fn)(std::get<0>(storage_)) : Chained{error_unchecked()};
    }

private:
    [[nodiscard]] constexpr const E& error_unchecked() const noexcept {
        return *std::get_if<1>(&storage_);
    }

    std::variant<T, E> storage_;
};

/// Void specialisation: an operation that either succeeded or produced an error.
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    constexpr Result() : storage_(std::in_place_index<0>) {}

    constexpr Result(E error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] constexpr const E& error() const& {
        if (has_value()) {
            result_misuse("Result<void>::error() called on a success state", FcError::INTERNAL_INVARIANT_VIOLATED);
        }
        return std::get<1>(storage_);
    }

private:
    std::variant<std::monostate, E> storage_;
};

/// Explicit success for `Result<void>`, so `return ok();` reads as a decision
/// rather than a bare `return {};`.
[[nodiscard]] inline Result<void> ok() noexcept {
    return Result<void>{};
}

} // namespace fc

// ---------------------------------------------------------------------------
// Propagation macros.
//
// Both require the enclosing function to return a Result whose error type is
// constructible from the propagated error -- which is the normal case.
// ---------------------------------------------------------------------------

#define FC_DETAIL_CONCAT_INNER(a, b) a##b
#define FC_DETAIL_CONCAT(a, b) FC_DETAIL_CONCAT_INNER(a, b)

/// Evaluates `expr`; on failure, returns its error from the enclosing function.
/// Discards any success value -- use FC_TRY_ASSIGN when you need it.
#define FC_TRY(expr)                                                                                                   \
    do {                                                                                                               \
        auto&& fc_try_result = (expr);                                                                                 \
        if (!fc_try_result.has_value()) {                                                                              \
            return fc_try_result.error();                                                                              \
        }                                                                                                              \
    } while (false)

/// Declares `decl` bound to the success value of `expr`, or returns the error.
/// Deliberately not wrapped in a loop: the declaration must outlive the macro.
///
///     FC_TRY_ASSIGN(const int width, parse_width(text));
///
/// NOLINTBEGIN(bugprone-macro-parentheses): `decl` is a declarator, not an
/// expression -- parenthesising it would not compile.
#define FC_TRY_ASSIGN(decl, expr)                                                                                      \
    auto FC_DETAIL_CONCAT(fc_assign_, __LINE__) = (expr);                                                              \
    if (!FC_DETAIL_CONCAT(fc_assign_, __LINE__).has_value()) {                                                         \
        return FC_DETAIL_CONCAT(fc_assign_, __LINE__).error();                                                         \
    }                                                                                                                  \
    decl = std::move(FC_DETAIL_CONCAT(fc_assign_, __LINE__)).value()
// NOLINTEND(bugprone-macro-parentheses)
