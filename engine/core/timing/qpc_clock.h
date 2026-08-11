#pragma once

// The master clock (SPEC.md §7.1).
//
// > `QueryPerformanceCounter` is the single source of truth for the entire
// > engine. Convert to nanoseconds once. Never use `GetTickCount`,
// > `std::chrono::system_clock`, or `time()` for media timing.
//
// The conversion is done in one place so the frequency is queried once, the
// rounding is identical everywhere, and — the part that turned out to matter — no
// call site can get it wrong on its own. Splitting the counter into whole seconds
// and a remainder before scaling is what keeps it correct: `counter * 1e9`
// overflows a signed 64-bit integer once the counter passes 9.22e9, which at the
// 10 MHz timer modern Windows uses is about **fifteen minutes of uptime**. Past
// that the naive form returns wrapped nonsense, and it does so *consistently*,
// which is why a video-only recording never notices and a recording with audio
// does (BUG-019).
//
// Header-only. This is two integer divisions on a path the capture and audio
// threads take per buffer, and a call into another translation unit buys nothing.

#include <windows.h>

#include <cstdint>

namespace fc::timing {

/// Ticks per second, queried once. `QueryPerformanceFrequency` is fixed at boot
/// and documented never to change while the system runs.
[[nodiscard]] inline std::int64_t qpc_frequency() noexcept {
    static const std::int64_t kFrequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    return kFrequency;
}

/// Nanoseconds since an arbitrary but monotonic origin.
[[nodiscard]] inline std::int64_t qpc_now_ns() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::int64_t frequency = qpc_frequency();
    const std::int64_t seconds = counter.QuadPart / frequency;
    const std::int64_t remainder = counter.QuadPart % frequency;
    return (seconds * 1'000'000'000) + ((remainder * 1'000'000'000) / frequency);
}

/// Converts WASAPI's `u64QPCPosition` to nanoseconds. That field is documented in
/// 100 ns units rather than in QPC ticks, so this is exact and does not involve
/// the frequency at all (SPEC.md §8.3).
[[nodiscard]] inline std::int64_t qpc_units_to_ns(std::uint64_t qpc_position) noexcept {
    return static_cast<std::int64_t>(qpc_position) * 100;
}

} // namespace fc::timing
