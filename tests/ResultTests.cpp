#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "vdt/Result.h"

using namespace vdt;

TEST(ResultTest, HoldsAValue) {
	Result<int> result = 7;
	ASSERT_TRUE(result);
	EXPECT_EQ(*result, 7);
}

TEST(ResultTest, HoldsAnError) {
	Result<int> result = Error{ Errc::Timeout, "Wait" };
	ASSERT_FALSE(result);
	EXPECT_EQ(result.GetError().code, Errc::Timeout);
	EXPECT_EQ(result.GetError().operation, "Wait");
}

TEST(ResultTest, HoldsMoveOnlyValues) {
	Result<std::unique_ptr<int>> result = std::make_unique<int>(3);
	ASSERT_TRUE(result);
	std::unique_ptr<int> value = std::move(*result);
	EXPECT_EQ(*value, 3);
}

TEST(ResultTest, VoidResultDefaultsToSuccess) {
	EXPECT_TRUE(Result<void>());
	EXPECT_FALSE(Result<void>(Error{ Errc::Timeout, "Wait" }));
}

TEST(ResultDeathTest, ReadingTheValueOfAFailureAborts) {
	Result<int> result = Error{ Errc::Timeout, "Wait" };
	EXPECT_DEATH((void)*result, "");
}

TEST(ResultDeathTest, ReadingTheErrorOfASuccessAborts) {
	Result<int> result = 7;
	EXPECT_DEATH((void)result.GetError(), "");
	Result<void> empty;
	EXPECT_DEATH((void)empty.GetError(), "");
}

TEST(ErrorTest, MessageNamesTheStepAndTheReason) {
	const Error error{ Errc::NotVirtualDisk, "Format" };
	EXPECT_EQ(error.Message(), "Format: refused: the target is not a file-backed virtual disk");
}

TEST(ErrorTest, EveryCodeHasItsOwnMessage) {
	const Errc codes[] = { Errc::InvalidArgument, Errc::Unsupported, Errc::AlreadyExists,
		Errc::NotAttached, Errc::AlreadyAttached, Errc::NotVirtualDisk, Errc::NoVolume, Errc::Timeout,
		Errc::Locked, Errc::WrongPassword, Errc::TooSmallToEncrypt };
	for (const Errc code : codes) {
		EXPECT_NE(make_error_code(code).message(), "unknown error");
	}
}

#ifdef _WIN32
TEST(ErrorTest, OsErrorsCompareAgainstPortableConditions) {
	const std::error_code accessDenied(5, std::system_category()); // ERROR_ACCESS_DENIED
	EXPECT_EQ(accessDenied, std::errc::permission_denied);
}
#endif
