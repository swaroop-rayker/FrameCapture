#include "core/error/fc_error.h"

#include <algorithm>
#include <array>

namespace fc {
namespace {

// NOLINTBEGIN(bugprone-macro-parentheses): `name` is pasted into `FcError::name`
// and stringified, neither of which permits parentheses.
constexpr std::array kErrors = {
#define FC_ERROR_DECLARE_INFO(name, code, message) FcErrorInfo{FcError::name, (code), #name, (message)},
    FC_ERROR_LIST(FC_ERROR_DECLARE_INFO)
#undef FC_ERROR_DECLARE_INFO
};

// NOLINTEND(bugprone-macro-parentheses)

const FcErrorInfo* find(FcError error) noexcept {
    const auto it = std::ranges::find(kErrors, error, &FcErrorInfo::error);
    return it != kErrors.end() ? &*it : nullptr;
}

} // namespace

std::string_view error_name(FcError error) noexcept {
    const FcErrorInfo* info = find(error);
    return info != nullptr ? info->name : "UNRECOGNISED";
}

std::string_view error_message(FcError error) noexcept {
    const FcErrorInfo* info = find(error);
    return info != nullptr ? info->message : "unrecognised error code";
}

Subsystem subsystem_of(FcError error) noexcept {
    const int code = error_code(error);
    if (code == 0) {
        return Subsystem::None;
    }

    switch (code / 1000) {
    case 1:
        return Subsystem::Capture;
    case 2:
        return Subsystem::Gpu;
    case 3:
        return Subsystem::Audio;
    case 4:
        return Subsystem::Encode;
    case 5:
        return Subsystem::Mux;
    case 6:
        return Subsystem::Io;
    case 7:
        return Subsystem::Ipc;
    case 9:
        return Subsystem::Internal;
    default:
        // A code outside every declared group is itself a defect. Do not invent
        // a plausible subsystem for it.
        return Subsystem::None;
    }
}

bool is_prime_directive_exception(FcError error) noexcept {
    // SPEC.md §1 / CLAUDE.md §1: only disk-full and file-handle-loss may result
    // in no playable output. IO_DISK_FULL_IMMINENT is the pre-emptive warning,
    // not the violation, so it is deliberately absent.
    return error == FcError::IO_DISK_FULL || error == FcError::IO_FILE_HANDLE_LOST;
}

std::span<const FcErrorInfo> all_errors() noexcept {
    return {kErrors.data(), kErrors.size()};
}

FcError error_from_code(int code) noexcept {
    const auto it = std::ranges::find(kErrors, code, &FcErrorInfo::code);
    return it != kErrors.end() ? it->error : FcError::INTERNAL_UNKNOWN;
}

} // namespace fc
