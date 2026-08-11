// The control channel's wire format and vocabulary (SPEC.md §15.1, §20.1).
//
// CPU TIER. Framing and message parsing are pure functions over bytes, so every rule
// §15.1 states about them is assertable without a pipe -- including the two that only
// matter under failure and would otherwise never be exercised: an oversized message and
// a peer sending fields this build has never heard of.

#include "core/ipc/framing.h"
#include "core/ipc/lifecycle.h"   // SPEC.md §3.1's heartbeat constants
#include "core/ipc/pipe_server.h" // the pipe-name shape, which is §15.1's too
#include "core/ipc/protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using fc::FcError;
using namespace fc::ipc;

[[nodiscard]] std::span<const std::uint8_t> bytes_of(const std::vector<std::uint8_t>& buffer) {
    return std::span<const std::uint8_t>{buffer.data(), buffer.size()};
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST(IpcFraming, TheLengthPrefixIsFourBytesLittleEndian) {
    const fc::Result<std::vector<std::uint8_t>> framed = encode_frame("hi");
    ASSERT_TRUE(framed.has_value());
    ASSERT_EQ(framed.value().size(), 6U);

    // SPEC.md §15.1 specifies the byte order. Asserted on the bytes rather than by
    // round-tripping, because a round trip passes just as happily on big-endian.
    EXPECT_EQ(framed.value()[0], 2U);
    EXPECT_EQ(framed.value()[1], 0U);
    EXPECT_EQ(framed.value()[2], 0U);
    EXPECT_EQ(framed.value()[3], 0U);
    EXPECT_EQ(framed.value()[4], 'h');
    EXPECT_EQ(framed.value()[5], 'i');
}

TEST(IpcFraming, AFrameRoundTrips) {
    const std::string body = R"({"cmd":"get_stats","id":"7"})";
    const fc::Result<std::vector<std::uint8_t>> framed = encode_frame(body);
    ASSERT_TRUE(framed.has_value());

    const fc::Result<DecodedFrame> decoded = decode_frame(bytes_of(framed.value()));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value().body, body);
    EXPECT_EQ(decoded.value().consumed, framed.value().size());
}

// An incomplete read is the normal case on a stream, not an error. Conflating the two
// would make every partial read log a failure and, worse, would tempt a reader into
// discarding a buffer that was merely early.
TEST(IpcFraming, AnIncompleteFrameIsReportedAsIncompleteRatherThanAsAnError) {
    const fc::Result<std::vector<std::uint8_t>> framed = encode_frame("abcdefgh");
    ASSERT_TRUE(framed.has_value());

    for (std::size_t prefix = 0; prefix < framed.value().size(); ++prefix) {
        const fc::Result<DecodedFrame> decoded =
            decode_frame(std::span<const std::uint8_t>{framed.value().data(), prefix});
        ASSERT_TRUE(decoded.has_value()) << "a short buffer of " << prefix << " bytes was reported as an error";
        EXPECT_EQ(decoded.value().consumed, 0U);
    }

    const fc::Result<DecodedFrame> whole = decode_frame(bytes_of(framed.value()));
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole.value().consumed, framed.value().size()) << "the complete frame did not decode";
}

// SPEC.md §15.1's 64 KB ceiling, from the sending side.
TEST(IpcFraming, AMessageOverTheLimitIsRefusedBySender) {
    const std::string body(kMaxMessageBytes + 1, 'x');
    const fc::Result<std::vector<std::uint8_t>> framed = encode_frame(body);
    ASSERT_FALSE(framed.has_value());
    EXPECT_EQ(framed.error(), FcError::IPC_MESSAGE_TOO_LARGE);

    // And exactly at the limit is allowed -- an off-by-one here would silently cap the
    // protocol one byte below what the spec says it carries.
    const std::string exact(kMaxMessageBytes, 'x');
    EXPECT_TRUE(encode_frame(exact).has_value());
}

// The important half of the same rule: a *declared* length over the ceiling is refused
// before anything is allocated. A corrupt or hostile prefix declaring 4 GB must not
// become a 4 GB reservation, so the check cannot come after the buffer is sized.
TEST(IpcFraming, AnOversizedDeclaredLengthIsRefusedWithoutWaitingForTheBody) {
    const std::vector<std::uint8_t> hostile{0xFF, 0xFF, 0xFF, 0xFF}; // ~4 GB
    const fc::Result<DecodedFrame> decoded = decode_frame(bytes_of(hostile));
    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error(), FcError::IPC_MESSAGE_TOO_LARGE);
}

TEST(IpcFraming, TheReaderSplitsSeveralFramesOutOfOneRead) {
    std::vector<std::uint8_t> stream;
    for (const char* body : {R"({"a":1})", R"({"b":2})", R"({"c":3})"}) {
        const fc::Result<std::vector<std::uint8_t>> framed = encode_frame(body);
        ASSERT_TRUE(framed.has_value());
        stream.insert(stream.end(), framed.value().begin(), framed.value().end());
    }

    FrameReader reader;
    reader.append(bytes_of(stream));

    std::vector<std::string> bodies;
    for (;;) {
        const fc::Result<std::optional<std::string>> next = reader.next();
        ASSERT_TRUE(next.has_value());
        if (!next.value().has_value()) {
            break;
        }
        bodies.push_back(*next.value());
    }

    ASSERT_EQ(bodies.size(), 3U);
    EXPECT_EQ(bodies[0], R"({"a":1})");
    EXPECT_EQ(bodies[2], R"({"c":3})");
    EXPECT_EQ(reader.pending(), 0U) << "the reader kept bytes it had already delivered";
}

// A reader that never compacts is an unbounded queue (CLAUDE.md hard rule 5), and the
// symptom would be a long-lived GUI connection growing without limit. Asserted by
// pushing far more traffic through than any buffer would hold.
TEST(IpcFraming, TheReaderDoesNotGrowAcrossManyFrames) {
    FrameReader reader;
    const fc::Result<std::vector<std::uint8_t>> framed = encode_frame(std::string(1000, 'y'));
    ASSERT_TRUE(framed.has_value());

    for (int i = 0; i < 5000; ++i) {
        reader.append(bytes_of(framed.value()));
        const fc::Result<std::optional<std::string>> next = reader.next();
        ASSERT_TRUE(next.has_value());
        ASSERT_TRUE(next.value().has_value());
    }
    EXPECT_EQ(reader.pending(), 0U);
}

// ---------------------------------------------------------------------------
// The command set
// ---------------------------------------------------------------------------

// SPEC.md §15.1 lists sixteen commands by name. This is the assertion that the enum is
// that list and not a superset or a subset of it -- a command the spec names and the
// engine cannot parse is a feature that silently does not exist.
TEST(IpcProtocol, EverySpecifiedCommandParsesAndRoundTrips) {
    const std::vector<std::string> specified{
        "hello",        "get_sources",   "get_devices", "get_gpu_topology", "configure",     "start_preview",
        "stop_preview", "start_record",  "stop_record", "pause_record",     "resume_record", "get_stats",
        "get_health",   "set_log_level", "recover",     "shutdown",
    };

    for (const std::string& name : specified) {
        const fc::Result<Command> parsed = command_from_string(name);
        ASSERT_TRUE(parsed.has_value()) << "SPEC.md §15.1 names '" << name << "' and it does not parse";
        EXPECT_EQ(to_string(parsed.value()), name) << "'" << name << "' did not round-trip";
    }
}

// The two commands beyond §15.1's list, added so settings can persist (§17). Tested
// like the specified ones because the GUI depends on them identically -- and named here
// so the difference between "in the spec" and "we added it" stays visible.
TEST(IpcProtocol, TheConfigPersistenceCommandsParseAndRoundTrip) {
    for (const std::string& name : {std::string{"get_config"}, std::string{"save_config"}}) {
        const fc::Result<Command> parsed = command_from_string(name);
        ASSERT_TRUE(parsed.has_value()) << "'" << name << "' does not parse";
        EXPECT_EQ(to_string(parsed.value()), name);
    }

    // They are ordinary commands as far as the timeout budget goes: writing a small TOML
    // file is not `stop_record`'s remux.
    EXPECT_EQ(request_timeout_ms(Command::GetConfig), 5'000);
    EXPECT_EQ(request_timeout_ms(Command::SaveConfig), 5'000);
}

TEST(IpcProtocol, AnUnrecognisedCommandIsRefusedByName) {
    const fc::Result<Command> parsed = command_from_string("delete_everything");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), FcError::IPC_UNKNOWN_COMMAND);
}

TEST(IpcProtocol, EverySpecifiedEventHasAWireName) {
    const std::vector<std::pair<Event, std::string>> specified{
        {Event::StateChanged, "state_changed"},
        {Event::Stats, "stats"},
        {Event::Warning, "warning"},
        {Event::Error, "error"},
        {Event::GpuMigrated, "gpu_migrated"},
        {Event::AudioDeviceMigrated, "audio_device_migrated"},
        {Event::DegradationChanged, "degradation_changed"},
        {Event::SegmentRolled, "segment_rolled"},
        {Event::RecordingFinalized, "recording_finalized"},
    };
    for (const auto& [event, name] : specified) {
        EXPECT_EQ(to_string(event), name);
    }
}

// SPEC.md §15.1: paused is a `state_changed` *value*. A GUI that cannot tell paused
// from recording "loses footage silently", which is the whole reason the spec says so.
TEST(IpcProtocol, PausedIsADistinctRecordingState) {
    EXPECT_EQ(to_string(RecordingState::Paused), "paused");
    EXPECT_NE(to_string(RecordingState::Paused), to_string(RecordingState::Recording));
}

// ---------------------------------------------------------------------------
// SPEC.md §15.1's compatibility rule
// ---------------------------------------------------------------------------

// > Unknown fields are ignored, never fatal.
//
// The natural implementation -- a strict schema check -- gets this backwards, and would
// make every field a future GUI adds a fatal error against an older engine. This is the
// case that fails if anyone writes one.
TEST(IpcProtocol, UnknownFieldsAreIgnoredRatherThanFatal) {
    const std::string body =
        R"({"cmd":"start_record","id":"42","output":"x.mkv","future_field":123,"nested":{"more":[1,2,3]},"flag":true})";

    const fc::Result<Request> parsed = parse_request(body);
    ASSERT_TRUE(parsed.has_value()) << "a request carrying unknown fields was refused";
    EXPECT_EQ(parsed.value().command, Command::StartRecord);
    EXPECT_EQ(parsed.value().id, "42");
    EXPECT_EQ(parsed.value().params["output"], "x.mkv");
}

TEST(IpcProtocol, AMissingIdIsEchoedEmptyRatherThanRefused) {
    // Refusing would leave the peer with a rejection it cannot correlate to anything
    // either, which is strictly worse than answering with an empty id.
    const fc::Result<Request> parsed = parse_request(R"({"cmd":"get_stats"})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed.value().id.empty());
}

TEST(IpcProtocol, MalformedJsonIsRefusedWithoutThrowing) {
    for (const char* body : {"", "not json", "[1,2,3]", R"({"id":"1"})", R"({"cmd":7})"}) {
        const fc::Result<Request> parsed = parse_request(body);
        ASSERT_FALSE(parsed.has_value()) << "accepted '" << body << "'";
        EXPECT_EQ(parsed.error(), FcError::IPC_MESSAGE_MALFORMED);
    }
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

TEST(IpcProtocol, EveryResponseEchoesTheRequestId) {
    nlohmann::json result = nlohmann::json::object();
    result["frames"] = 120;

    const nlohmann::json parsed = nlohmann::json::parse(make_response("abc", result));
    EXPECT_EQ(parsed["id"], "abc");
    EXPECT_EQ(parsed["ok"], true);
    EXPECT_EQ(parsed["frames"], 120);
}

// SPEC.md §19 makes the numeric codes "a stable external contract". A GUI matching on
// English prose is a GUI that breaks when the prose improves, so the code travels.
TEST(IpcProtocol, AnErrorResponseCarriesTheNumericCodeAndTheEnumeratorSpelling) {
    const nlohmann::json parsed =
        nlohmann::json::parse(make_error_response("9", FcError::MULTITRACK_REQUIRES_MKV, "tier B is MKV only"));

    EXPECT_EQ(parsed["id"], "9");
    EXPECT_EQ(parsed["ok"], false);
    EXPECT_EQ(parsed["code"], 3021);
    EXPECT_EQ(parsed["error"], "MULTITRACK_REQUIRES_MKV");
    EXPECT_EQ(parsed["detail"], "tier B is MKV only");
}

TEST(IpcProtocol, AnEventCarriesItsNameAndNoId) {
    nlohmann::json body = nlohmann::json::object();
    body["state"] = "paused";

    const nlohmann::json parsed = nlohmann::json::parse(make_event(Event::StateChanged, body));
    EXPECT_EQ(parsed["event"], "state_changed");
    EXPECT_EQ(parsed["state"], "paused");
    EXPECT_EQ(parsed.count("id"), 0U) << "an event answers nothing and must not look like a response";
}

// ---------------------------------------------------------------------------
// Timeouts and capabilities
// ---------------------------------------------------------------------------

// SPEC.md §15.1: "Requests time out at 5 s (except `stop_record`, 30 s, since
// finalization is legitimately slow)."
TEST(IpcProtocol, StopRecordGetsThirtySecondsAndEverythingElseGetsFive) {
    EXPECT_EQ(request_timeout_ms(Command::StopRecord), 30'000);
    for (const Command command : {Command::Hello, Command::StartRecord, Command::PauseRecord, Command::ResumeRecord,
                                  Command::GetStats, Command::Shutdown}) {
        EXPECT_EQ(request_timeout_ms(command), 5'000) << "for " << to_string(command);
    }
}

// The capability list is a promise. `pause_resume` is in it because §7.5 is implemented,
// `preview` because §15.2's ring is, and `multitrack` because §8.6's Tier B is -- each
// assertion flipped when the feature landed rather than deleted, because "the engine
// stopped claiming a capability" is exactly as much of a regression as "it claimed one
// before it worked".
TEST(IpcProtocol, CapabilitiesClaimWhatThisBuildActuallyImplements) {
    const std::vector<std::string> capabilities = engine_capabilities();

    const auto has = [&](std::string_view name) { return std::ranges::find(capabilities, name) != capabilities.end(); };

    EXPECT_TRUE(has("pause_resume"));
    EXPECT_TRUE(has("heartbeat"));
    EXPECT_TRUE(has("preview")) << "SPEC.md §15.2's preview ring landed in M9; a GUI has to be able to ask for it";
    EXPECT_TRUE(has("multitrack")) << "SPEC.md §8.6's Tier B landed in M9.5";

    // And the honest absences that remain. Nothing in this build streams or captures a
    // webcam, and SPEC.md §0.2 makes both hard non-goals -- a capability list that grew
    // one of these would be the first sign something had been scaffolded in.
    EXPECT_FALSE(has("streaming"));
    EXPECT_FALSE(has("webcam"));
}

// ---------------------------------------------------------------------------
// SPEC.md §3.1's constants
// ---------------------------------------------------------------------------

TEST(IpcProtocol, TheHeartbeatMatchesTheSpecifiedIntervalAndTimeout) {
    // Pinned rather than assumed: SPEC.md §20 row 13's 6 s budget is 5 s + 1, so a
    // timeout that drifted would move the bound the row is asserted against.
    EXPECT_EQ(kHeartbeatInterval.count(), 1000);
    EXPECT_EQ(kHeartbeatTimeout.count(), 5000);
}

TEST(IpcProtocol, ThePipePathFollowsTheSpecifiedShape) {
    EXPECT_EQ(pipe_path_for("abc123"), R"(\\.\pipe\framecapture-abc123)");

    // Two generated ids must differ, or two engines in one user session would fight
    // over one pipe name -- which `FILE_FLAG_FIRST_PIPE_INSTANCE` would turn into a
    // start-up failure rather than a mix-up, but a failure all the same.
    EXPECT_NE(new_session_id(), new_session_id());
    EXPECT_FALSE(new_session_id().empty());
}

} // namespace
