#include "core/error/hresult.h"

#include "core/error/result.h"

#include <gtest/gtest.h>

#include <windows.h>
// Must follow windows.h.
#include <dxgi.h>

#include <string>

namespace {

using fc::FcError;
using fc::HResult;

TEST(HResultTest, FailureIsExactlyANegativeValue) {
    EXPECT_TRUE(fc::hr_failed(static_cast<HResult>(E_FAIL)));
    EXPECT_TRUE(fc::hr_failed(static_cast<HResult>(DXGI_ERROR_DEVICE_REMOVED)));
    EXPECT_FALSE(fc::hr_failed(static_cast<HResult>(S_OK)));
    EXPECT_FALSE(fc::hr_failed(static_cast<HResult>(S_FALSE)));

    EXPECT_TRUE(fc::hr_succeeded(static_cast<HResult>(S_OK)));
    EXPECT_TRUE(fc::hr_succeeded(static_cast<HResult>(S_FALSE)));
    EXPECT_FALSE(fc::hr_succeeded(static_cast<HResult>(E_FAIL)));
}

TEST(HResultTest, NamesTheCodesFormatMessageDoesNotKnow) {
    // The entire reason the table exists: FormatMessage has nothing to say about
    // any of these, and they are the codes SPEC.md §5.4 and §14.1 hinge on.
    EXPECT_EQ(fc::hresult_name(static_cast<HResult>(DXGI_ERROR_DEVICE_REMOVED)), "DXGI_ERROR_DEVICE_REMOVED");
    EXPECT_EQ(fc::hresult_name(static_cast<HResult>(DXGI_ERROR_ACCESS_LOST)), "DXGI_ERROR_ACCESS_LOST");
    EXPECT_EQ(fc::hresult_name(static_cast<HResult>(DXGI_ERROR_DEVICE_HUNG)), "DXGI_ERROR_DEVICE_HUNG");
    EXPECT_EQ(fc::hresult_name(static_cast<HResult>(E_OUTOFMEMORY)), "E_OUTOFMEMORY");
}

TEST(HResultTest, UnknownCodeHasNoName) {
    EXPECT_TRUE(fc::hresult_name(static_cast<HResult>(0x81234567)).empty());
}

TEST(HResultTest, MessageAlwaysCarriesTheHexValue) {
    const std::string message = fc::hresult_message(static_cast<HResult>(DXGI_ERROR_DEVICE_REMOVED));
    EXPECT_NE(message.find("0x887A0005"), std::string::npos) << message;
    EXPECT_NE(message.find("DXGI_ERROR_DEVICE_REMOVED"), std::string::npos) << message;
}

TEST(HResultTest, MessageForAnUnknownCodeSaysSoExplicitly) {
    const std::string message = fc::hresult_message(static_cast<HResult>(0x81234567));
    EXPECT_NE(message.find("0x81234567"), std::string::npos) << message;
    // Must not look like the lookup was never attempted.
    EXPECT_FALSE(message == "0x81234567") << message;
}

TEST(HResultTest, MessageForAWin32CodeIncludesTheSystemDescription) {
    const std::string message = fc::hresult_message(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    EXPECT_NE(message.find(':'), std::string::npos) << message;
    EXPECT_GT(message.size(), 12u) << message;
}

TEST(HResultTest, MapsGpuDeviceLifetimeCodes) {
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_DEVICE_REMOVED)), FcError::GPU_DEVICE_REMOVED);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_DRIVER_INTERNAL_ERROR)),
              FcError::GPU_DEVICE_REMOVED);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_DEVICE_RESET)), FcError::GPU_DEVICE_RESET);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_DEVICE_HUNG)), FcError::GPU_DEVICE_HUNG);
}

TEST(HResultTest, MapsDuplicationCodes) {
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_ACCESS_LOST)), FcError::DDA_ACCESS_LOST);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_ACCESS_DENIED)), FcError::DDA_ACCESS_DENIED);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(DXGI_ERROR_SESSION_DISCONNECTED)),
              FcError::CAPTURE_TARGET_GONE);
}

TEST(HResultTest, MapsGenericComCodes) {
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_OUTOFMEMORY)), FcError::INTERNAL_OUT_OF_MEMORY);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_INVALIDARG)), FcError::INTERNAL_INVALID_ARGUMENT);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_NOTIMPL)), FcError::INTERNAL_NOT_IMPLEMENTED);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_ACCESSDENIED)), FcError::IO_PERMISSION_DENIED);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_ABORT)), FcError::INTERNAL_CANCELLED);
}

TEST(HResultTest, MapsWrappedWin32Codes) {
    EXPECT_EQ(fc::hresult_to_fc_error(HRESULT_FROM_WIN32(ERROR_DISK_FULL)), FcError::IO_DISK_FULL);
    EXPECT_EQ(fc::hresult_to_fc_error(HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)), FcError::IO_DISK_FULL);
    EXPECT_EQ(fc::hresult_to_fc_error(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)), FcError::IO_PATH_INVALID);
    EXPECT_EQ(fc::hresult_to_fc_error(HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)), FcError::IO_FILE_HANDLE_LOST);
    EXPECT_EQ(fc::hresult_to_fc_error(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)), FcError::INTERNAL_OUT_OF_MEMORY);
}

TEST(HResultTest, SuccessMapsToNone) {
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(S_OK)), FcError::NONE);
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(S_FALSE)), FcError::NONE);
}

TEST(HResultTest, UnmappedFailureIsUnknownRatherThanAGuess) {
    // Inventing a subsystem for an unclassified code would send whoever reads the
    // log to the wrong place.
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(0x81234567)), FcError::INTERNAL_UNKNOWN);
}

// ---------------------------------------------------------------------------
// The macros. These are what SPEC.md §19 actually mandates, so they are tested
// through real Result-returning functions rather than by inspection.
// ---------------------------------------------------------------------------

fc::Result<int> generic_mapping(HResult hr) {
    FC_HR(hr);
    return 1;
}

fc::Result<int> explicit_mapping(HResult hr) {
    FC_HR_AS(hr, FcError::GPU_TEXTURE_CREATE_FAILED);
    return 1;
}

TEST(HResultTest, FcHrReturnsTheMappedErrorOnFailure) {
    const auto result = generic_mapping(static_cast<HResult>(DXGI_ERROR_DEVICE_REMOVED));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FcError::GPU_DEVICE_REMOVED);
}

TEST(HResultTest, FcHrPassesThroughOnSuccess) {
    const auto result = generic_mapping(static_cast<HResult>(S_OK));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST(HResultTest, FcHrAsOverridesTheGenericMapping) {
    // E_INVALIDARG generically means "invalid argument", which tells a user
    // nothing. This is why call sites are expected to name the error.
    EXPECT_EQ(fc::hresult_to_fc_error(static_cast<HResult>(E_INVALIDARG)), FcError::INTERNAL_INVALID_ARGUMENT);

    const auto result = explicit_mapping(static_cast<HResult>(E_INVALIDARG));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FcError::GPU_TEXTURE_CREATE_FAILED);
}

TEST(HResultTest, FcHrLogEvaluatesToTheOutcome) {
    EXPECT_TRUE(FC_HR_LOG(static_cast<HResult>(S_OK)));
    EXPECT_FALSE(FC_HR_LOG(static_cast<HResult>(E_FAIL)));
}

} // namespace
