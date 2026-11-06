// Tests of the real Windows backend that need no admin rights and change no disks, so they
// are safe anywhere.
//
// Nothing here may ever call WriteSinglePartition or ClearLayout on a real disk, even to
// prove it gets refused. If the check they rely on were broken, that would wipe the disk.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "FakeBackend.h"
#include "vdt/VirtualDisk.h"
#include "windows/DiskLayout.h"

namespace fs = std::filesystem;
using namespace vdt;
using test::TempDirectory;

namespace {

constexpr int kDisksToProbe = 64;

DiskSpec BlankSpec(uint64_t size = MiB(8)) {
	DiskSpec spec;
	spec.size = size;
	return spec;
}

} // anonymous namespace

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
