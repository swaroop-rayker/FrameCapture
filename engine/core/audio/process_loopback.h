#pragma once

// Per-process audio capture — SPEC.md §8.6's Tier B mechanism.
//
// `LoopbackCapture` opens a *render endpoint* in loopback mode and captures the
// system mix. This opens a **virtual** device instead — `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`
// through `ActivateAudioInterfaceAsync` — and captures only what one process tree
// renders. That is the whole of Tier B's capture story; everything above it
// (timeline, silence, drift, resample, AAC) is `AudioEncodePath`, unchanged and
// already per-instance.
//
// Still `eRender` only. CLAUDE.md §2 rule 6 makes input capture a hard non-goal,
// and process loopback is a *render* interception: it hears what an application
// plays, never what a microphone hears.
//
// ---------------------------------------------------------------------------
// §8.6's four "shipped-bug sources", and where each one is answered
// ---------------------------------------------------------------------------
//   1. **The format is supplied, not negotiated.** `GetMixFormat` is not
//      implemented on this virtual device, so there is nothing to ask. §8.6 names
//      the answer: 48 kHz, 32-bit float, stereo. `process_loopback_format()`.
//   2. **Initialize-once.** The target PID is baked into the activation and
//      cannot be changed on a live client. A target that exits means tearing the
//      client down, not reconfiguring it — see `AppAudioTracks`.
//   3. **Its own device clock.** Handled upstream: each track gets its own
//      `AudioTimeline` and `DriftCompensator` against the one QPC epoch (§7.1).
//   4. **Silence is the steady state.** A per-app track is quiet most of the
//      time, so the §8.2 watchdog is the primary path here rather than an edge
//      case. Also handled upstream, and tested as the primary path.
//
// One trap §8.6 does not list and this file has to answer anyway: the virtual
// device does **not** always populate `u64QPCPosition`. See `qpc_fallbacks()`.

#include "core/audio/loopback_capture.h"
#include "core/error/hresult.h"
#include "core/error/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fc::audio {

/// The format §8.6 says to supply. Not a default that can be overridden — the
/// virtual device negotiates nothing, and "requesting the endpoint's native
/// format here silently fails on some configurations".
inline constexpr int kProcessLoopbackSampleRate = 48000;
inline constexpr int kProcessLoopbackChannels = 2;

/// 48 kHz / 32-bit float / stereo, with the stereo channel mask spelled out so
/// `resolve_channel_layout` has the same evidence it gets from a real endpoint.
[[nodiscard]] MixFormat process_loopback_format() noexcept;

/// One running process, for §8.6's "poll by executable name at 2 Hz" and for the
/// GUI's target picker.
struct ProcessEntry {
    std::uint32_t pid = 0;
    /// The image name only — `chrome.exe`, not a full path. That is the identity
    /// §8.6 names, and it is the one a user can type.
    std::string executable;
};

/// Every process this snapshot could see.
///
/// Best-effort by construction: a snapshot is a moment, and a process listed here
/// may be gone before the caller acts on it. Callers treat a stale PID as "not
/// resolved yet" rather than as an error, which is the same answer they need for a
/// target that has not started.
[[nodiscard]] Result<std::vector<ProcessEntry>> enumerate_processes();

/// Every distinct executable currently running, sorted by name.
///
/// One entry per **name**, not per process, carrying the same pid
/// `find_process_by_executable` would resolve — because that is the only way a picker
/// can mean the same thing as typing the name does. A machine with forty `chrome.exe`
/// processes offers one `chrome.exe`, and choosing it selects the browser rather than
/// whichever renderer happened to sort first.
///
/// Unfiltered by whether a process has ever played anything: WASAPI offers no way to
/// ask, an application that is silent now may not be in a moment, and §8.6 explicitly
/// supports naming a target that has not started at all. A picker that hid silent
/// applications would hide most of the ones a user wants.
[[nodiscard]] Result<std::vector<ProcessEntry>> enumerate_distinct_executables();

/// The **lowest** PID whose image name matches `executable`, case-insensitively,
/// or 0 when none does.
///
/// Lowest rather than newest deliberately: browsers and games spawn helper
/// processes, and the lowest PID of a name is overwhelmingly the parent that
/// started first. §8.6 targets a PID with
/// `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE`, so picking the parent is
/// what makes "chrome.exe" mean the browser rather than one renderer.
[[nodiscard]] std::uint32_t find_process_by_executable(std::string_view executable);

/// An open handle to a target process, kept for as long as the track follows it.
///
/// SPEC.md §20 row 15 turns on noticing that a target exited, and the obvious
/// implementation — re-open the PID each poll and see whether it opens — is wrong
/// in a way that only shows up rarely and then looks like a Tier B bug: **Windows
/// reuses PIDs.** A target that exits and whose number is handed to something else
/// between two polls reads as still alive, and the track goes on expecting audio
/// from a process that is not the one it was aimed at.
///
/// Holding a handle removes the question. The kernel keeps the process object (and
/// therefore its id) reserved while any handle to it exists, so this id cannot be
/// reused, and `alive()` is a wait on the object rather than a lookup by number.
class ProcessWatch {
public:
    ProcessWatch() = default;

    /// Opens `pid` for synchronisation. `valid()` is false if it could not be
    /// opened — the process is gone, or this token may not see it.
    explicit ProcessWatch(std::uint32_t pid) noexcept;

    ~ProcessWatch();

    ProcessWatch(const ProcessWatch&) = delete;
    ProcessWatch& operator=(const ProcessWatch&) = delete;
    ProcessWatch(ProcessWatch&& other) noexcept;
    ProcessWatch& operator=(ProcessWatch&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    /// True while the process is still running. False once it has exited, and
    /// false when `valid()` is false.
    [[nodiscard]] bool alive() const noexcept;

    [[nodiscard]] std::uint32_t pid() const noexcept {
        return pid_;
    }

    void reset() noexcept;

private:
    /// `void*` rather than `HANDLE`, so this header stays free of Windows headers
    /// the way `audio_timeline.h` does.
    void* handle_ = nullptr;
    std::uint32_t pid_ = 0;
};

/// SPEC.md §8.6: "Requires Windows 10 build 19041+ — which is already our floor
/// (§1), so no additional version gating is needed, but **probe at runtime
/// anyway**."
///
/// Probes by activating against this process's own PID and releasing it
/// immediately — the only honest answer, since an OS build number does not tell
/// you whether the audio service will actually hand over the interface. Cached
/// after the first call: the answer cannot change while the process runs, and the
/// probe costs an activation round trip.
[[nodiscard]] bool process_loopback_available();

/// The HRESULT the probe above got, for a diagnosis rather than a boolean.
///
/// "Tier B is unavailable" is not an answer anyone can act on. `AUDCLNT_E_*`,
/// `E_NOINTERFACE` and `ERROR_TIMEOUT` mean three completely different things, and the
/// difference between them is the difference between a policy setting, a build that is
/// too old, and an audio service that is wedged. Cached with the probe.
[[nodiscard]] HResult process_loopback_probe_result();

/// One per-application audio source.
///
/// Exists so the tracks above it can be driven by something other than Windows.
/// `ProcessLoopbackCapture` is the production implementation and is what every
/// real recording uses; the CPU tier supplies a deterministic generator instead,
/// for the same reason `AudioSource::External` exists for Tier A (SPEC.md §20
/// row 4 needs both streams generated from one clock) and for the same reason
/// `SessionSettings::capture_factory` exists.
///
/// **The seam supplies the input; it does not remove the decision.** Timeline
/// placement, silence injection, drift, the epoch and the muxing all sit above
/// this interface and are exercised identically either way.
class IProcessAudioSource {
public:
    IProcessAudioSource() = default;
    virtual ~IProcessAudioSource() = default;

    IProcessAudioSource(const IProcessAudioSource&) = delete;
    IProcessAudioSource& operator=(const IProcessAudioSource&) = delete;
    IProcessAudioSource(IProcessAudioSource&&) = delete;
    IProcessAudioSource& operator=(IProcessAudioSource&&) = delete;

    /// Begins delivering buffers to `sink`, which is invoked on this source's own
    /// thread and must not block (SPEC.md §12).
    [[nodiscard]] virtual Result<void> start(LoopbackSink sink) = 0;

    /// Stops and releases. Idempotent.
    virtual void stop() = 0;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// The format the buffers are in. Always `process_loopback_format()` for the
    /// production source, because §8.6 supplies it.
    [[nodiscard]] virtual MixFormat format() const noexcept = 0;

    [[nodiscard]] virtual std::uint64_t buffers_captured() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t silent_buffers() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t discontinuities() const noexcept = 0;

    /// Buffers whose `u64QPCPosition` was zero and were stamped from `qpc_now_ns()`
    /// instead. See `ProcessLoopbackCapture::qpc_fallbacks`.
    [[nodiscard]] virtual std::uint64_t qpc_fallbacks() const noexcept = 0;
};

/// Event-driven process loopback on a dedicated thread.
///
/// Shaped exactly like `LoopbackCapture` — same `LoopbackBuffer`, same sink, same
/// MMCSS registration, same "released unconditionally" discipline — so the encode
/// path above it cannot tell the two apart. What differs is how the client is
/// obtained and that the format is dictated rather than read.
class ProcessLoopbackCapture final : public IProcessAudioSource {
public:
    /// `pid` is fixed for this object's lifetime. §8.6: the client is
    /// initialize-once and the target cannot be changed on a live one.
    explicit ProcessLoopbackCapture(std::uint32_t pid);
    ~ProcessLoopbackCapture() override;

    ProcessLoopbackCapture(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture& operator=(const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture(ProcessLoopbackCapture&&) = delete;
    ProcessLoopbackCapture& operator=(ProcessLoopbackCapture&&) = delete;

    [[nodiscard]] Result<void> start(LoopbackSink sink) override;
    void stop() override;

    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] MixFormat format() const noexcept override;
    [[nodiscard]] std::uint64_t buffers_captured() const noexcept override;
    [[nodiscard]] std::uint64_t silent_buffers() const noexcept override;
    [[nodiscard]] std::uint64_t discontinuities() const noexcept override;

    /// Buffers the virtual device handed over with `u64QPCPosition == 0`.
    ///
    /// The real endpoint always stamps one and SPEC.md §8.3 makes it authoritative;
    /// the virtual device is not documented to, and a zero would place every such
    /// buffer at the epoch and destroy the track. Those buffers are stamped with
    /// `qpc_now_ns()` at the moment `GetBuffer` returned instead — the same clock,
    /// read one endpoint-latency later, which the §8.2 timeline's jitter guard
    /// absorbs (BUG-042).
    ///
    /// Reported rather than hidden: a recording where this is non-zero has a track
    /// whose placement is one buffer looser than §8.3 promises, and that is worth
    /// knowing before it is worth explaining.
    [[nodiscard]] std::uint64_t qpc_fallbacks() const noexcept override;

    [[nodiscard]] std::uint32_t pid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::audio
