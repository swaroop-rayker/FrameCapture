#include "core/mux/recovery.h"

#include "core/logging/logger.h"
#include "core/util/atomic_write.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <sstream>

namespace fc::mux {
namespace {

/// Bumped when the record's shape changes incompatibly. A build that does not
/// recognise the version refuses the record rather than reading fields that may
/// have moved -- an unfinished recording is not worth a wrong repair.
constexpr int kRecordSchema = 1;

/// ISO-8601 UTC, for the notice a user reads.
[[nodiscard]] std::string utc_now() {
    // Wall clock, deliberately, and this is the one place it belongs. SPEC.md
    // §7.1's ban on system_clock is about *media timing*, where QPC is the only
    // clock. This timestamp has to survive a reboot and mean something to a person
    // -- "the recording you lost started at 14:32" -- and a monotonic counter that
    // resets at boot can do neither.
    // FC_LINT_OK
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    std::tm utc{};
    if (gmtime_s(&utc, &seconds) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string{buffer.data(), written};
}

} // namespace

std::filesystem::path sidecar_path_for(const std::filesystem::path& output) {
    std::filesystem::path sidecar = output;
    // `+=`, not `replace_extension`: `capture.mp4.fcrecover` keeps the recording's
    // own name intact, so the two sort together and two recordings that differ only
    // by container cannot share a sidecar.
    sidecar += kRecoverySuffix;
    return sidecar;
}

Result<void> write_recovery_record(const RecoveryRecord& record) {
    if (record.output.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    nlohmann::json document;
    document["schema"] = kRecordSchema;
    document["container"] = config::to_string(record.container);
    document["output"] = record.output.generic_string();
    document["engine_version"] = record.engine_version;
    document["session_id"] = record.session_id;
    document["started_utc"] = record.started_utc.empty() ? utc_now() : record.started_utc;
    document["width"] = record.width;
    document["height"] = record.height;
    document["fps"] = record.fps;
    document["video_codec"] = record.video_codec;
    document["audio_codec"] = record.audio_codec;
    document["audio_channels"] = record.audio_channels;
    document["audio_sample_rate"] = record.audio_sample_rate;
    document["audio_initial_padding"] = record.audio_initial_padding;

    const std::filesystem::path sidecar = sidecar_path_for(record.output);

    // Atomically, because a sidecar is a *claim* that a recording is unfinished and
    // a half-written one would be a claim nobody can evaluate. The same sequence
    // SPEC.md §17 uses for the config file.
    if (const Result<void> written = write_file_atomically(sidecar, document.dump(2)); !written.has_value()) {
        FC_LOG_ERROR(Subsystem::Mux, "could not write the recovery sidecar; a crash will not be repairable",
                     LogFields{}.add("path", sidecar.string()).add_error(written.error()));
        return written.error();
    }

    FC_LOG_INFO(Subsystem::Mux, "recovery sidecar written",
                LogFields{}.add("path", sidecar.string()).add("container", config::to_string(record.container)));
    return ok();
}

Result<RecoveryRecord> read_recovery_record(const std::filesystem::path& sidecar) {
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec)) {
        return FcError::MUX_RECOVERY_FAILED;
    }

    const std::ifstream stream(sidecar, std::ios::binary);
    if (!stream) {
        return FcError::IO_FILE_OPEN_FAILED;
    }
    std::ostringstream text;
    text << stream.rdbuf();

    // `parse` throws on malformed input, and a sidecar is untrusted in the same
    // sense a config file is -- it may be a leftover from a different build, or
    // truncated by the very crash it records. Caught by type; CLAUDE.md §4 bans
    // the catch-all and the empty handler, not this.
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text.str());
    } catch (const std::exception& e) {
        FC_LOG_WARN(Subsystem::Mux, "the recovery sidecar is not valid JSON",
                    LogFields{}.add("path", sidecar.string()).add("what", e.what()));
        return FcError::MUX_RECOVERY_FAILED;
    }

    if (!document.is_object() || !document.contains("schema") || !document["schema"].is_number_integer() ||
        document["schema"].get<int>() != kRecordSchema) {
        FC_LOG_WARN(Subsystem::Mux, "the recovery sidecar is from an unrecognised schema; refusing to interpret it",
                    LogFields{}.add("path", sidecar.string()));
        return FcError::MUX_RECOVERY_FAILED;
    }

    const auto text_field = [&document](const char* key) -> std::string {
        return document.contains(key) && document[key].is_string() ? document[key].get<std::string>() : std::string{};
    };
    const auto int_field = [&document](const char* key) -> int {
        return document.contains(key) && document[key].is_number_integer() ? document[key].get<int>() : 0;
    };

    RecoveryRecord record;
    const std::optional<config::Container> container = config::container_from_string(text_field("container"));
    if (!container.has_value()) {
        FC_LOG_WARN(Subsystem::Mux, "the recovery sidecar names a container this build does not know",
                    LogFields{}.add("path", sidecar.string()).add("container", text_field("container")));
        return FcError::MUX_RECOVERY_FAILED;
    }
    record.container = *container;

    const std::string output = text_field("output");
    if (output.empty()) {
        return FcError::MUX_RECOVERY_FAILED;
    }
    record.output = std::filesystem::path{output};

    record.engine_version = text_field("engine_version");
    record.session_id = text_field("session_id");
    record.started_utc = text_field("started_utc");
    record.width = int_field("width");
    record.height = int_field("height");
    record.fps = int_field("fps");
    record.video_codec = text_field("video_codec");
    record.audio_codec = text_field("audio_codec");
    record.audio_channels = int_field("audio_channels");
    record.audio_sample_rate = int_field("audio_sample_rate");
    record.audio_initial_padding = int_field("audio_initial_padding");
    return record;
}

void clear_recovery_record(const std::filesystem::path& output) noexcept {
    // Composing the path allocates, so even this can throw on an exhausted heap --
    // and an exception escaping a `noexcept` function terminates the process. That
    // would turn "the sidecar could not be deleted" into "the recording that just
    // finished takes the process down with it", which is the exact inversion of
    // what this function is for. Caught by type, not with a catch-all (CLAUDE.md
    // §4), and swallowed on purpose: see the header.
    try {
        const std::filesystem::path sidecar = sidecar_path_for(output);
        std::error_code ec;
        std::filesystem::remove(sidecar, ec);
        static_cast<void>(ec);
    } catch (const std::exception& e) {
        // Reported at TRACE and no higher. There is genuinely nothing to do about
        // it -- the recording is finished and validated -- and a leftover sidecar
        // costs one spurious prompt on the next launch. The line exists so the
        // handler is not empty and so a reader of a trace log can see it happened.
        FC_LOG_TRACE(Subsystem::Mux, "clearing the recovery sidecar threw", LogFields{}.add("what", e.what()));
    }
}

std::vector<std::filesystem::path> find_recoverable(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return found;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == kRecoverySuffix) {
            found.push_back(entry.path());
        }
    }
    return found;
}

Result<RecoveryOutcome> recover(const std::filesystem::path& sidecar) {
    FC_TRY_ASSIGN(const RecoveryRecord record, read_recovery_record(sidecar));

    RecoveryOutcome outcome;
    outcome.output = record.output;

    std::error_code ec;
    if (!std::filesystem::exists(record.output, ec) || std::filesystem::file_size(record.output, ec) == 0) {
        // The sidecar outlived its recording. Nothing to repair and nothing to
        // warn about on the next launch either, so the claim is retracted.
        outcome.detail = "the recording named by the sidecar is missing or empty";
        FC_LOG_WARN(Subsystem::Mux, "discarding a recovery sidecar with no recording",
                    LogFields{}.add("path", record.output.string()));
        clear_recovery_record(record.output);
        return outcome;
    }

    ValidationExpectation expectation;
    expectation.audio = !record.audio_codec.empty();
    expectation.audio_channels = record.audio_channels;
    // No duration expectation on purpose. A recording that was killed is
    // legitimately shorter than whatever it was going to be, and there is nothing
    // to compare against -- the pacer's frame count died with the process.

    if (record.container == config::Container::Mp4) {
        FC_TRY_ASSIGN(outcome.report, finalize_in_place(record.output, expectation, record.audio_initial_padding));
        outcome.repaired = true;
    } else {
        // Matroska needs no repair to be playable: a truncated file decodes to its
        // last complete cluster (SPEC.md §10.2). Rewriting it would risk a working
        // recording to gain an index, which is the wrong trade on a file the user
        // has already nearly lost once.
        FC_TRY_ASSIGN(outcome.report, Muxer::validate(record.output, expectation));
    }

    outcome.valid = outcome.report.valid;
    outcome.detail = outcome.report.detail;

    if (outcome.valid) {
        clear_recovery_record(record.output);
        FC_LOG_INFO(Subsystem::Mux, "unfinished recording recovered",
                    LogFields{}
                        .add("path", record.output.string())
                        .add("repaired", outcome.repaired)
                        .add("duration_s", outcome.report.duration_seconds)
                        .add("started_utc", record.started_utc));
    } else {
        // The sidecar stays. A recording that could not be made valid is one the
        // user should keep being told about.
        FC_LOG_ERROR(Subsystem::Mux, "an unfinished recording could not be recovered",
                     LogFields{}
                         .add("path", record.output.string())
                         .add("detail", outcome.detail)
                         .add_error(FcError::MUX_RECOVERY_FAILED));
    }
    return outcome;
}

} // namespace fc::mux
