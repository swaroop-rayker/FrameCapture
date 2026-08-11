#include "core/error/fc_error.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

TEST(FcError, EveryCodeIsUnique) {
    std::set<int> seen;
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_TRUE(seen.insert(info.code).second) << "duplicate numeric code " << info.code << " on " << info.name;
    }
}

TEST(FcError, EveryNameIsUnique) {
    std::set<std::string_view> seen;
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_TRUE(seen.insert(info.name).second) << "duplicate name " << info.name;
    }
}

TEST(FcError, EveryCodeFallsInADeclaredSubsystemGroup) {
    // SPEC.md §19: 1xxx capture, 2xxx gpu, 3xxx audio, 4xxx encode, 5xxx mux,
    // 6xxx io, 7xxx ipc, 9xxx internal. 8xxx is deliberately not a group.
    const std::set<int> allowed{0, 1, 2, 3, 4, 5, 6, 7, 9};
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_TRUE(allowed.contains(info.code / 1000))
            << info.name << " (" << info.code << ") is outside every declared group";
    }
}

TEST(FcError, EveryCodeReportsANameAndAMessage) {
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_FALSE(fc::error_name(info.error).empty()) << info.code;
        EXPECT_FALSE(fc::error_message(info.error).empty()) << info.name;
        EXPECT_EQ(fc::error_name(info.error), info.name);
        EXPECT_EQ(fc::error_code(info.error), info.code);
    }
}

TEST(FcError, SubsystemIsDerivedFromTheThousandsDigit) {
    EXPECT_EQ(fc::subsystem_of(fc::FcError::NONE), fc::Subsystem::None);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::CAPTURE_INIT_FAILED), fc::Subsystem::Capture);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::DDA_ACCESS_LOST), fc::Subsystem::Capture);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::GPU_DEVICE_REMOVED), fc::Subsystem::Gpu);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::MULTITRACK_REQUIRES_MKV), fc::Subsystem::Audio);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::ENCODE_ZERO_COPY_LOST), fc::Subsystem::Encode);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::MUX_WRITE_TRAILER_FAILED), fc::Subsystem::Mux);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::IO_DISK_FULL), fc::Subsystem::Io);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::IPC_HEARTBEAT_TIMEOUT), fc::Subsystem::Ipc);
    EXPECT_EQ(fc::subsystem_of(fc::FcError::INTERNAL_UNKNOWN), fc::Subsystem::Internal);
}

TEST(FcError, EverySubsystemIsConsistentWithItsCode) {
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        const fc::Subsystem subsystem = fc::subsystem_of(info.error);
        EXPECT_NE(fc::to_string(subsystem), "unknown") << info.name;
        if (info.code != 0) {
            EXPECT_EQ(static_cast<int>(subsystem), info.code / 1000) << info.name;
        }
    }
}

// SPEC.md §20 row 16 names this code and this number verbatim. If this test fails,
// the spec and the implementation disagree about a documented contract.
TEST(FcError, SpecMandatedCodesHaveTheirSpecifiedNumbers) {
    EXPECT_EQ(fc::error_code(fc::FcError::MULTITRACK_REQUIRES_MKV), 3021);
    EXPECT_EQ(fc::error_name(fc::FcError::MULTITRACK_REQUIRES_MKV), "MULTITRACK_REQUIRES_MKV");
}

TEST(FcError, NoneIsZero) {
    EXPECT_EQ(fc::error_code(fc::FcError::NONE), 0);
}

TEST(FcError, CodeLookupRoundTrips) {
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_EQ(fc::error_from_code(info.code), info.error) << info.name;
    }
}

TEST(FcError, UndeclaredCodeMapsToUnknownRatherThanAPlausibleGuess) {
    EXPECT_EQ(fc::error_from_code(8123), fc::FcError::INTERNAL_UNKNOWN);
    EXPECT_EQ(fc::error_from_code(-1), fc::FcError::INTERNAL_UNKNOWN);
    EXPECT_EQ(fc::error_from_code(1999), fc::FcError::INTERNAL_UNKNOWN);
}

TEST(FcError, UnrecognisedValueDoesNotClaimAName) {
    const auto bogus = static_cast<fc::FcError>(8123);
    EXPECT_EQ(fc::error_name(bogus), "UNRECOGNISED");
    EXPECT_FALSE(fc::error_message(bogus).empty());
}

// CLAUDE.md §1 / SPEC.md §1: only these two conditions may result in no playable
// output file. Widening this set is a change to the project's prime directive and
// must be a deliberate decision, not a side effect of adding an error code.
TEST(FcError, PrimeDirectiveExceptionsAreExactlyDiskFullAndHandleLoss) {
    int count = 0;
    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        if (fc::is_prime_directive_exception(info.error)) {
            ++count;
            EXPECT_TRUE(info.error == fc::FcError::IO_DISK_FULL || info.error == fc::FcError::IO_FILE_HANDLE_LOST)
                << info.name << " must not be exempt from the prime directive";
        }
    }
    EXPECT_EQ(count, 2);

    // The pre-emptive warning is not itself a violation.
    EXPECT_FALSE(fc::is_prime_directive_exception(fc::FcError::IO_DISK_FULL_IMMINENT));
}

// ---------------------------------------------------------------------------
// SPEC.md §19: "Every code is documented in docs/ERROR_CODES.md with cause and
// remediation." This test makes that a build gate rather than a promise.
// ---------------------------------------------------------------------------
TEST(FcError, ErrorCodesDocumentationIsComplete) {
    const std::filesystem::path doc = std::filesystem::path{FC_DOCS_DIR} / "ERROR_CODES.md";
    ASSERT_TRUE(std::filesystem::exists(doc)) << "missing " << doc.string();

    const std::ifstream stream{doc};
    ASSERT_TRUE(stream.is_open());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();

    for (const fc::FcErrorInfo& info : fc::all_errors()) {
        EXPECT_NE(text.find(info.name), std::string::npos) << info.name << " is not documented in ERROR_CODES.md";
        EXPECT_NE(text.find(std::to_string(info.code)), std::string::npos)
            << "numeric code " << info.code << " (" << info.name << ") is not documented in ERROR_CODES.md";
    }
}

} // namespace
