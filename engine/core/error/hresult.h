#pragma once

#include "core/error/fc_error.h"

#include <string>
#include <string_view>

namespace fc {

/// Mirror of the Win32 `HRESULT`. Declared as a plain `long` so that this header
/// -- included by most of the engine -- does not drag in <windows.h>. It also
/// makes the mapping below unit-testable with synthetic values, which is why
/// `test_hresult` can run on the CPU tier with no COM calls at all.
using HResult = long;

/// A failed HRESULT is exactly a negative one; this is the definition `FAILED()`
/// expands to.
[[nodiscard]] constexpr bool hr_failed(HResult hr) noexcept {
    return hr < 0;
}

[[nodiscard]] constexpr bool hr_succeeded(HResult hr) noexcept {
    return hr >= 0;
}

/// Human-readable description, formatted as
/// `0x887A0005 (DXGI_ERROR_DEVICE_REMOVED): <system message>`.
///
/// `FormatMessage` knows nothing about DXGI, D3D11 or WASAPI HRESULTs -- it
/// returns nothing useful for the exact codes this project cares about most. So a
/// table of known graphics and audio codes is consulted first, and the system
/// message is appended only when there is one.
[[nodiscard]] std::string hresult_message(HResult hr);

/// The symbolic constant name, e.g. "DXGI_ERROR_ACCESS_LOST", or an empty view
/// when the code is not in the table.
[[nodiscard]] std::string_view hresult_name(HResult hr) noexcept;

/// Best-effort classification into the error domain.
///
/// Deliberately conservative: codes with no specific meaning map to
/// `INTERNAL_UNKNOWN` rather than to a plausible-sounding subsystem error. When a
/// call site knows better than this mapping -- and it usually does -- it should
/// use `FC_HR_AS` to name the error explicitly.
[[nodiscard]] FcError hresult_to_fc_error(HResult hr) noexcept;

namespace detail {

/// Logs the failing expression, file, line, function, HRESULT and its message at
/// ERROR, under the subsystem implied by `error`. Returns `error` so the FC_HR
/// macros can `return` it directly.
FcError report_hresult(std::string_view expression, std::string_view file, int line, std::string_view function,
                       HResult hr, FcError error);

/// Backs FC_HR_LOG. Returns true when `hr` succeeded; logs the full context and
/// returns false when it did not.
bool check_hresult(std::string_view expression, std::string_view file, int line, std::string_view function, HResult hr);

} // namespace detail
} // namespace fc

// ---------------------------------------------------------------------------
// SPEC.md §19: "Every HRESULT is checked. A FC_HR(expr) macro logs the failing
// expression, file, line, HRESULT value, and the human-readable FormatMessage
// string. No bare `hr = foo();` with an unchecked result."
//
// scripts/lint.ps1 greps for unchecked assignments; these macros are the
// sanctioned way to satisfy it.
// ---------------------------------------------------------------------------

/// Checks `expr`; on failure logs the full context and returns the generically
/// mapped `FcError` from the enclosing Result-returning function.
///
/// Prefer FC_HR_AS wherever a specific code is meaningful -- the generic mapping
/// turns most COM failures into INTERNAL_UNKNOWN or INTERNAL_INVALID_ARGUMENT,
/// which tells a user nothing.
#define FC_HR(expr)                                                                                                    \
    do {                                                                                                               \
        const ::fc::HResult fc_hr_status = (expr);                                                                     \
        if (::fc::hr_failed(fc_hr_status)) {                                                                           \
            return ::fc::detail::report_hresult(#expr, __FILE__, __LINE__, __FUNCTION__, fc_hr_status,                 \
                                                ::fc::hresult_to_fc_error(fc_hr_status));                              \
        }                                                                                                              \
    } while (false)

/// Checks `expr`; on failure logs the full context and returns `error`.
#define FC_HR_AS(expr, error)                                                                                          \
    do {                                                                                                               \
        const ::fc::HResult fc_hr_status = (expr);                                                                     \
        if (::fc::hr_failed(fc_hr_status)) {                                                                           \
            return ::fc::detail::report_hresult(#expr, __FILE__, __LINE__, __FUNCTION__, fc_hr_status, (error));       \
        }                                                                                                              \
    } while (false)

/// Checks `expr`, logs on failure, and evaluates to `true` on success.
///
/// For paths that cannot return: destructors, teardown, and best-effort cleanup
/// where a failure is worth recording but there is nothing left to abort.
///
/// Delegates to a function rather than an immediately-invoked lambda. Two reasons,
/// both learned the hard way: a lambda's `__FUNCTION__` names its `operator()`
/// instead of the calling function, and a capture list containing a comma is seen
/// by the preprocessor as an argument separator -- which breaks the macro the
/// moment it is nested inside another one, such as `EXPECT_TRUE(FC_HR_LOG(...))`.
/// Square brackets do not protect commas; only parentheses do.
#define FC_HR_LOG(expr) ::fc::detail::check_hresult(#expr, __FILE__, __LINE__, __FUNCTION__, (expr))
