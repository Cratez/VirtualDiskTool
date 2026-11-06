// Tests of the real Windows backend that need no admin rights and change no disks, so they
// are safe anywhere.
//
// Nothing here may ever call WriteSinglePartition or ClearLayout on a real disk, even to
// prove it gets refused. If the check they rely on were broken, that would wipe the disk.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "FakeBackend.h"
#include "vdt/VirtualDisk.h"
#include "windows/BitLocker.h"
#include "windows/DiskLayout.h"
#include "windows/VolumeFormat.h"

namespace fs = std::filesystem;
using namespace vdt;
using test::TempDirectory;

namespace {

constexpr int kDisksToProbe = 64;

const Error kInUse = win::Win32Error("FormatEx", ERROR_BUSY);

// Win32_EncryptableVolume's ConversionStatus values.
constexpr uint32_t kFullyDecrypted = 0;
constexpr uint32_t kFullyEncrypted = 1;
constexpr uint32_t kEncrypting = 2;
constexpr uint32_t kDecrypting = 3;
constexpr uint32_t kEncryptionPaused = 4;
constexpr uint32_t kDecryptionPaused = 5;

constexpr ULONGLONG kLongEnoughNotToMatterMs = 60 * 60 * 1000;
constexpr ULONGLONG kShortTimeoutMs = 2000;

DiskSpec BlankSpec(uint64_t size = MiB(8)) {
	DiskSpec spec;
	spec.size = size;
	return spec;
}

/// <summary>
/// A status reader that gives each status in turn, then keeps repeating the last one.
/// </summary>
std::function<Result<uint32_t>()> StatusesInTurn(std::vector<uint32_t> statuses) {
	return [statuses = std::move(statuses), next = size_t(0)]() mutable -> Result<uint32_t> {
		const uint32_t status = statuses[next];
		next = std::min(next + 1, statuses.size() - 1);
		return status;
	};
}

} // anonymous namespace

TEST(RetryWhileInUseTest, DoesNotRepeatASuccess) {
	int calls = 0;
	Result<void> result = win::RetryWhileInUse([&]() -> Result<void> { ++calls; return {}; }, 5, 0);
	EXPECT_TRUE(result);
	EXPECT_EQ(calls, 1);
}

TEST(RetryWhileInUseTest, KeepsTryingUntilTheVolumeIsFree) {
	int calls = 0;
	Result<void> result = win::RetryWhileInUse([&]() -> Result<void> {
		return ++calls < 3 ? Result<void>(kInUse) : Result<void>();
	}, 5, 0);
	EXPECT_TRUE(result);
	EXPECT_EQ(calls, 3);
}

TEST(RetryWhileInUseTest, GivesUpAtTheLimitAndReportsInUse) {
	int calls = 0;
	Result<void> result = win::RetryWhileInUse([&]() -> Result<void> { ++calls; return kInUse; }, 5, 0);
	ASSERT_FALSE(result);
	EXPECT_EQ(calls, 5);
	EXPECT_EQ(result.GetError().code, kInUse.code);
}

// Only "in use" is worth repeating: formatting again can't fix anything else.
TEST(RetryWhileInUseTest, DoesNotRetryOtherFailures) {
	int calls = 0;
	Result<void> result = win::RetryWhileInUse([&]() -> Result<void> {
		++calls;
		return win::Win32Error("FormatEx", ERROR_GEN_FAILURE);
	}, 5, 0);
	ASSERT_FALSE(result);
	EXPECT_EQ(calls, 1);
}

// No stall allowance at all, because encrypting is progress however long it takes.
TEST(WaitForEncryptionTest, FinishesOnceFullyEncrypted) {
	Result<void> result = win::WaitForEncryption(StatusesInTurn({ kEncrypting, kEncrypting, kFullyEncrypted }), kLongEnoughNotToMatterMs, 0, 0);
	EXPECT_TRUE(result);
}

// The status can take a moment to change after Encrypt returns.
TEST(WaitForEncryptionTest, ToleratesAStatusThatIsSlowToChange) {
	Result<void> result = win::WaitForEncryption(StatusesInTurn({ kFullyDecrypted, kFullyDecrypted, kEncrypting, kFullyEncrypted }), kLongEnoughNotToMatterMs, kLongEnoughNotToMatterMs, 0);
	EXPECT_TRUE(result);
}

// Waiting for the full timeout would leave a volume mounted for up to 12 hours, in the clear.
TEST(WaitForEncryptionTest, StopsWaitingOnceEncryptionHasStopped) {
	for (const uint32_t status : { kFullyDecrypted, kDecrypting, kEncryptionPaused, kDecryptionPaused }) {
		Result<void> result = win::WaitForEncryption(StatusesInTurn({ kEncrypting, status }), kShortTimeoutMs, 0, 0);

		ASSERT_FALSE(result) << status;
		EXPECT_EQ(result.GetError().code, std::errc::io_error) << status;
		EXPECT_NE(result.GetError().Message().find(std::to_string(status)), std::string::npos);
	}
}

TEST(WaitForEncryptionTest, TimesOutIfItNeverFinishes) {
	Result<void> result = win::WaitForEncryption(StatusesInTurn({ kEncrypting }), 0, kLongEnoughNotToMatterMs, 0);

	ASSERT_FALSE(result);
	EXPECT_EQ(result.GetError().code, Errc::Timeout);
}

TEST(WaitForEncryptionTest, ReportsAFailureToReadTheStatus) {
	Result<void> result = win::WaitForEncryption([]() -> Result<uint32_t> { return kInUse; }, kLongEnoughNotToMatterMs, kLongEnoughNotToMatterMs, 0);

	ASSERT_FALSE(result);
	EXPECT_EQ(result.GetError().code, kInUse.code);
}

TEST(BitLockerErrorTest, GivesCallersTheCodesTheyCanActOn) {
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310027).code, Errc::WrongPassword);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310035).code, Errc::WrongPassword);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310011).code, Errc::TooSmallToEncrypt);
	EXPECT_EQ(win::ReturnValueError("Call", 0x8031006F).code, Errc::TooSmallToEncrypt);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310013).code, Errc::Unsupported);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310015).code, Errc::Unsupported);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310053).code, Errc::Unsupported);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310080).code, Errc::InvalidArgument);
	EXPECT_EQ(win::ReturnValueError("Call", 0x80310081).code, Errc::InvalidArgument);
}

TEST(BitLockerErrorTest, KeepsAnUnknownCodeInTheMessage) {
	EXPECT_NE(win::ReturnValueError("Call", 0x80310099).Message().find("0x80310099"), std::string::npos);
}

TEST(DiskLayoutTest, ParsesDiskNumbers) {
	Result<DWORD> number = win::ParseDiskNumber(L"\\\\.\\PhysicalDrive12");
	ASSERT_TRUE(number);
	EXPECT_EQ(*number, 12u);
}

TEST(DiskLayoutTest, RejectsAnythingElse) {
	for (const wchar_t* path : { L"", L"C:\\", L"\\\\.\\PhysicalDrive", L"\\\\.\\PhysicalDrive1x", L"\\\\.\\CdRom0" }) {
		EXPECT_FALSE(win::ParseDiskNumber(path));
	}
}

// Every real disk this machine has is a chance to catch the safety check saying yes wrongly.
TEST(DiskLayoutTest, OnlyAttachedImagesAreReportedAsVirtual) {
	Result<std::vector<AttachedImage>> attached = ListAttached();
	ASSERT_TRUE(attached) << attached.GetError().Message();

	int disksSeen = 0;
	size_t virtualDisks = 0;
	for (int index = 0; index < kDisksToProbe; ++index) {
		Result<bool> isVirtual = win::IsFileBackedVirtualDisk(L"\\\\.\\PhysicalDrive" + std::to_wstring(index));
		if (!isVirtual) {
			continue; // No disk with that number.
		}
		++disksSeen;
		virtualDisks += *isVirtual ? 1 : 0;
	}
	EXPECT_GT(disksSeen, 0);

	// Counting rather than matching by device, because finding the device needs admin rights.
	EXPECT_EQ(virtualDisks, attached->size());
}

TEST(DiskLayoutTest, AMissingDiskIsAnErrorRatherThanANo) {
	EXPECT_FALSE(win::IsFileBackedVirtualDisk(L"\\\\.\\PhysicalDrive9999"));
}

TEST(WindowsBackendTest, ReportsItsCapabilities) {
	const IBackend& backend = DefaultBackend();
	EXPECT_TRUE(backend.Supports(ImageFormat::Vhd));
	EXPECT_FALSE(backend.Supports(ImageFormat::Raw));
	EXPECT_TRUE(backend.Supports(FileSystem::Ntfs));
	EXPECT_FALSE(backend.Supports(FileSystem::Ext4));
	EXPECT_TRUE(backend.CleansUpAfterCrash());
}

TEST(WindowsBackendTest, ListingWorksWithoutAdminRights) {
	EXPECT_TRUE(ListAttached());
}

TEST(WindowsBackendTest, CreatesEachKindOfBlankImage) {
	TempDirectory directory;
	const fs::path dynamicVhd = directory.Path() / "dynamic.vhd";
	const fs::path dynamicVhdx = directory.Path() / "dynamic.vhdx";
	const fs::path fixedVhd = directory.Path() / "fixed.vhd";
	const fs::path fixedVhdx = directory.Path() / "fixed.vhdx";

	DiskSpec fixed = BlankSpec();
	fixed.allocation = Allocation::Fixed;

	Result<void> created = CreateBlankImage(dynamicVhd, BlankSpec());
	ASSERT_TRUE(created) << created.GetError().Message();
	created = CreateBlankImage(dynamicVhdx, BlankSpec());
	ASSERT_TRUE(created) << created.GetError().Message();
	created = CreateBlankImage(fixedVhd, fixed);
	ASSERT_TRUE(created) << created.GetError().Message();
	created = CreateBlankImage(fixedVhdx, fixed);
	ASSERT_TRUE(created) << created.GetError().Message();

	EXPECT_LT(fs::file_size(dynamicVhd), MiB(1));
	EXPECT_GE(fs::file_size(fixedVhd), MiB(8));
	EXPECT_GE(fs::file_size(fixedVhdx), MiB(8));
}

TEST(WindowsBackendTest, RefusesToOverwriteARealImage) {
	TempDirectory directory;
	const fs::path image = directory.Path() / "disk.vhdx";
	ASSERT_TRUE(CreateBlankImage(image, BlankSpec()));
	const auto size = fs::file_size(image);

	Result<void> again = CreateBlankImage(image, BlankSpec(MiB(16)));

	ASSERT_FALSE(again);
	EXPECT_EQ(again.GetError().code, Errc::AlreadyExists);
	EXPECT_EQ(fs::file_size(image), size);
}

TEST(WindowsBackendTest, DetachingAnUnattachedImageSaysSo) {
	TempDirectory directory;
	const fs::path image = directory.Path() / "disk.vhdx";
	ASSERT_TRUE(CreateBlankImage(image, BlankSpec()));

	Result<void> detached = DetachImage(image);

	ASSERT_FALSE(detached);
	EXPECT_EQ(detached.GetError().code, Errc::NotAttached);
}

TEST(WindowsBackendTest, AGarbageFileCannotBeAttached) {
	TempDirectory directory;
	const fs::path image = directory.Path() / "disk.vhdx";
	std::ofstream(image) << "this is not a virtual disk";

	EXPECT_FALSE(VirtualDisk::Open(image));
}
