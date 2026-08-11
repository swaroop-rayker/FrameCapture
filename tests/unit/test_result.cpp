#include "core/error/result.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace {

using fc::FcError;
using fc::Result;

TEST(ResultTest, HoldsAValue) {
    const Result<int> result{42};
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(*result, 42);
}

TEST(ResultTest, HoldsAnError) {
    const Result<int> result{FcError::CAPTURE_INIT_FAILED};
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error(), FcError::CAPTURE_INIT_FAILED);
}

TEST(ResultTest, ImplicitConstructionFromBothStates) {
    const auto make = [](bool succeed) -> Result<std::string> {
        if (succeed) {
            return std::string{"ok"};
        }
        return FcError::IO_FILE_OPEN_FAILED;
    };

    EXPECT_EQ(make(true).value(), "ok");
    EXPECT_EQ(make(false).error(), FcError::IO_FILE_OPEN_FAILED);
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    Result<std::unique_ptr<int>> result{std::make_unique<int>(7)};
    ASSERT_TRUE(result.has_value());

    const std::unique_ptr<int> taken = std::move(result).value();
    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, 7);
}

TEST(ResultTest, VoidSpecialisationCarriesSuccess) {
    const Result<void> result = fc::ok();
    EXPECT_TRUE(result.has_value());
}

TEST(ResultTest, VoidSpecialisationCarriesError) {
    const Result<void> result{FcError::MUX_WRITE_TRAILER_FAILED};
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FcError::MUX_WRITE_TRAILER_FAILED);
}

TEST(ResultTest, ValueOrReturnsTheFallbackOnError) {
    EXPECT_EQ(Result<int>{5}.value_or(99), 5);
    EXPECT_EQ(Result<int>{FcError::INTERNAL_TIMEOUT}.value_or(99), 99);
}

TEST(ResultTest, MapTransformsTheValue) {
    const Result<int> ok{21};
    const Result<int> bad{FcError::INTERNAL_CANCELLED};

    const auto doubled = ok.map([](int v) { return v * 2; });
    ASSERT_TRUE(doubled.has_value());
    EXPECT_EQ(doubled.value(), 42);

    const auto propagated = bad.map([](int v) { return v * 2; });
    ASSERT_FALSE(propagated.has_value());
    EXPECT_EQ(propagated.error(), FcError::INTERNAL_CANCELLED);
}

TEST(ResultTest, MapCanChangeTheValueType) {
    const Result<int> ok{3};
    const auto stringified = ok.map([](int v) { return std::to_string(v); });
    ASSERT_TRUE(stringified.has_value());
    EXPECT_EQ(stringified.value(), "3");
}

TEST(ResultTest, AndThenChainsResultReturningOperations) {
    const auto halve = [](const int& v) -> Result<int> {
        if (v % 2 != 0) {
            return FcError::INTERNAL_INVALID_ARGUMENT;
        }
        return v / 2;
    };

    EXPECT_EQ(Result<int>{8}.and_then(halve).value(), 4);
    EXPECT_EQ(Result<int>{7}.and_then(halve).error(), FcError::INTERNAL_INVALID_ARGUMENT);
    EXPECT_EQ(Result<int>{FcError::INTERNAL_TIMEOUT}.and_then(halve).error(), FcError::INTERNAL_TIMEOUT);
}

// ---------------------------------------------------------------------------
// Propagation macros
// ---------------------------------------------------------------------------

Result<int> parse_positive(int input) {
    if (input <= 0) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    return input;
}

Result<void> require_positive(int input) {
    FC_TRY(parse_positive(input));
    return fc::ok();
}

Result<int> doubled_if_positive(int input) {
    FC_TRY_ASSIGN(const int value, parse_positive(input));
    return value * 2;
}

Result<int> two_step(int a, int b) {
    FC_TRY_ASSIGN(const int first, parse_positive(a));
    FC_TRY_ASSIGN(const int second, parse_positive(b));
    return first + second;
}

TEST(ResultTest, FcTryPropagatesTheError) {
    EXPECT_TRUE(require_positive(1).has_value());

    const Result<void> failed = require_positive(-1);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error(), FcError::INTERNAL_INVALID_ARGUMENT);
}

TEST(ResultTest, FcTryAssignBindsTheValue) {
    const Result<int> ok = doubled_if_positive(10);
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok.value(), 20);

    const Result<int> bad = doubled_if_positive(0);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error(), FcError::INTERNAL_INVALID_ARGUMENT);
}

TEST(ResultTest, FcTryAssignWorksTwiceInOneScope) {
    // Guards against the macro's temporary name colliding with itself.
    EXPECT_EQ(two_step(2, 3).value(), 5);
    EXPECT_EQ(two_step(2, -3).error(), FcError::INTERNAL_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Misuse is a defect, and it is loud rather than undefined.
// ---------------------------------------------------------------------------

TEST(ResultDeathTest, ReadingTheValueOfAnErrorTerminates) {
    EXPECT_DEATH(
        {
            const Result<int> bad{FcError::GPU_DEVICE_REMOVED};
            // Deliberate misuse. Must not return garbage.
            const volatile int sink = bad.value();
            static_cast<void>(sink);
        },
        "FATAL");
}

TEST(ResultDeathTest, ReadingTheErrorOfAValueTerminates) {
    EXPECT_DEATH(
        {
            const Result<int> good{1};
            const volatile int sink = static_cast<int>(good.error());
            static_cast<void>(sink);
        },
        "FATAL");
}

} // namespace
