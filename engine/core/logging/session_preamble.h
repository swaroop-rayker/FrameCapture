#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fc {

/// One adapter as it appears in the session preamble. Populated by the GPU
/// topology service at M1; empty until then.
struct AdapterSummary {
    std::string description;
    std::string driver_version;
    std::uint32_t vendor_id = 0;
    std::uint64_t dedicated_video_memory = 0;
    bool owns_target_output = false;
};

/// The mandatory session preamble (SPEC.md §18): "OS build, CPU, full adapter
/// enumeration with driver versions, display topology, audio endpoint format,
/// selected capture backend, selected encoder + settings, config hash."
///
/// Fields split into two groups by what exists today.
struct SessionPreamble {
    // --- Available now -----------------------------------------------------
    std::string app_version;
    std::string os_build;
    std::string cpu_brand;
    std::uint32_t cpu_logical_processors = 0;

    // --- Owned by later milestones ----------------------------------------
    // These are logged with `status=pending` and an explicit owning milestone, so
    // an empty adapter list reads as "not implemented yet" rather than as
    // "no adapters found" -- a distinction that matters when the black-frame bug
    // in SPEC.md §20 row 1 is being diagnosed from a user's log.
    std::vector<AdapterSummary> adapters; ///< M1
    std::string display_topology;         ///< M1
    std::string audio_endpoint_format;    ///< M4
    std::string capture_backend;          ///< M2
    std::string encoder;                  ///< M3
    std::string encoder_settings;         ///< M3
    std::string config_hash;              ///< M0 config subsystem
};

/// Gathers everything obtainable without a subsystem that does not exist yet:
/// app version, OS build, and CPU. Never fails -- an undeterminable field comes
/// back as "unknown" rather than aborting a recording over a log line.
[[nodiscard]] SessionPreamble collect_session_preamble();

/// Writes the preamble at INFO under `Subsystem::App`. One event per group so
/// that a single grep pulls a whole machine profile out of a user's log.
void log_session_preamble(const SessionPreamble& preamble);

/// Convenience: collect then log.
void log_session_preamble();

} // namespace fc
