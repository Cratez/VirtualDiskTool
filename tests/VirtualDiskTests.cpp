#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "FakeBackend.h"
#include "vdt/VirtualDisk.h"

namespace fs = std::filesystem;
using namespace vdt;
using test::FakeBackend;
using test::TempDirectory;

namespace {

using Calls = std::vector<std::string>;

class VirtualDiskTest : public testing::Test {
protected:
	fs::path Image(const char* name = "disk.vhdx") const {
		return directory.Path() / name;
	}

	static DiskSpec Spec() {
		DiskSpec spec;
		spec.size = MiB(8);
		return spec;
	}

	static DiskSpec BlankSpec() {
		DiskSpec spec = Spec();
		spec.volume.fileSystem = FileSystem::None;
		return spec;
	}

	static AttachOptions Persistent() {
		AttachOptions options;
		options.lifetime = Lifetime::Persistent;
		return options;
	}

	void WriteImageFile(const char* name = "disk.vhdx") const {
		std::ofstream(Image(name)) << "existing";
	}

	bool Called(const char* name) const {
		return std::find(backend.calls.begin(), backend.calls.end(), name) != backend.calls.end();
	}

	FakeBackend backend;
	TempDirectory directory;
};

} // anonymous namespace

// --- Creating ---

TEST_F(VirtualDiskTest, CreateRunsEveryStepInOrder) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk) << disk.GetError().Message();

	EXPECT_EQ(backend.calls, (Calls{ "CreateImage", "Attach", "IsFileBackedVirtualDisk", "Initialize", "Mount" }));
	EXPECT_EQ(disk->GetImagePath(), Image());
	EXPECT_EQ(disk->GetMountPath(), fs::path("FakeMount"));
}

TEST_F(VirtualDiskTest, CreateKeepsTheImageAfterDestruction) {
	{
		Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
		ASSERT_TRUE(disk);
	}
	EXPECT_TRUE(fs::exists(Image()));
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(VirtualDiskTest, CreateNeverOverwrites) {
	WriteImageFile();

	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::AlreadyExists);
	EXPECT_TRUE(backend.calls.empty());
	std::string contents;
	std::ifstream(Image()) >> contents;
	EXPECT_EQ(contents, "existing");
}

TEST_F(VirtualDiskTest, CreateRejectsZeroSize) {
	DiskSpec spec = Spec();
	spec.size = 0;
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), spec, {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::InvalidArgument);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, UnsupportedFormatFailsBeforeAnythingIsCreated) {
	backend.unsupportedFormats = { ImageFormat::Vhdx };
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::Unsupported);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, UnsupportedFileSystemFailsBeforeAnythingIsCreated) {
	backend.unsupportedFileSystems = { FileSystem::Default };
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::Unsupported);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, FormatOnAReadOnlyAttachmentIsRejectedUpFront) {
	AttachOptions options;
	options.readOnly = true;
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), options, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::InvalidArgument);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, AutoFormatFollowsTheExtension) {
	Result<VirtualDisk> vhd = VirtualDisk::Create(Image("a.vhd"), Spec(), {}, backend);
	ASSERT_TRUE(vhd);
	EXPECT_EQ(backend.lastSpec->format, ImageFormat::Vhd);

	Result<VirtualDisk> vhdx = VirtualDisk::Create(Image("b.VHDX"), Spec(), {}, backend);
	ASSERT_TRUE(vhdx);
	EXPECT_EQ(backend.lastSpec->format, ImageFormat::Vhdx);
}

TEST_F(VirtualDiskTest, FormatMustAgreeWithTheExtension) {
	DiskSpec spec = Spec();
	spec.format = ImageFormat::Vhd;
	Result<VirtualDisk> disk = VirtualDisk::Create(Image("disk.vhdx"), spec, {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::InvalidArgument);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, AVhdFormatNeedsAVhdExtension) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image("disk.img"), Spec(), {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::InvalidArgument);
}

TEST_F(VirtualDiskTest, RawImagesCanUseAnyOtherExtension) {
	DiskSpec spec = Spec();
	spec.format = ImageFormat::Raw;
	Result<VirtualDisk> disk = VirtualDisk::Create(Image("disk.img"), spec, {}, backend);
	EXPECT_TRUE(disk);
}

TEST_F(VirtualDiskTest, AnExtensionOutsideTheAnsiCodePageDoesNotThrow) {
	DiskSpec spec = Spec();
	spec.format = ImageFormat::Raw;
	EXPECT_TRUE(VirtualDisk::Create(directory.Path() / L"disk.テスト", spec, {}, backend));
}

TEST_F(VirtualDiskTest, NoFileSystemLeavesTheDiskBlank) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), BlankSpec(), {}, backend);
	ASSERT_TRUE(disk);
	EXPECT_EQ(backend.calls, (Calls{ "CreateImage", "Attach" }));
	EXPECT_TRUE(disk->GetMountPath().empty());
}

TEST_F(VirtualDiskTest, TheRequestedMountPathReachesTheBackend) {
	AttachOptions options;
	options.mountPath = fs::path("Z:\\");
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), options, backend);
	ASSERT_TRUE(disk);
	EXPECT_EQ(disk->GetMountPath(), fs::path("Z:\\"));
}

TEST_F(VirtualDiskTest, CreateBlankImageDoesNotAttach) {
	ASSERT_TRUE(CreateBlankImage(Image(), Spec(), backend));
	EXPECT_EQ(backend.calls, (Calls{ "CreateImage" }));
	EXPECT_TRUE(fs::exists(Image()));
}

TEST_F(VirtualDiskTest, CreateBlankImageNeverOverwrites) {
	WriteImageFile();
	Result<void> created = CreateBlankImage(Image(), Spec(), backend);
	ASSERT_FALSE(created);
	EXPECT_EQ(created.GetError().code, Errc::AlreadyExists);
	EXPECT_TRUE(backend.calls.empty());
}

namespace {

class CreateFailureTest : public VirtualDiskTest, public testing::WithParamInterface<const char*> {};

} // anonymous namespace

TEST_P(CreateFailureTest, LeavesNothingBehind) {
	backend.failures[GetParam()] = Error{ Errc::Timeout, GetParam() };

	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_FALSE(fs::exists(Image()));
	EXPECT_EQ(backend.attached, 0);
	EXPECT_EQ(backend.leaked, 0);
}

INSTANTIATE_TEST_SUITE_P(EveryStep, CreateFailureTest,
	testing::Values("CreateImage", "Attach", "IsFileBackedVirtualDisk", "Initialize", "Mount"));

// --- Temporary disks ---

TEST_F(VirtualDiskTest, TemporaryImageExistsThenDisappears) {
	fs::path image;
	{
		Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(), {}, backend);
		ASSERT_TRUE(disk) << disk.GetError().Message();
		image = disk->GetImagePath();
		EXPECT_EQ(image.extension(), ".vhdx");
		EXPECT_TRUE(fs::exists(image));
	}
	EXPECT_FALSE(fs::exists(image));
	EXPECT_FALSE(fs::exists(image.parent_path()));
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(VirtualDiskTest, TemporaryDisksGetUniquePaths) {
	Result<VirtualDisk> first = VirtualDisk::CreateTemporary(Spec(), {}, backend);
	Result<VirtualDisk> second = VirtualDisk::CreateTemporary(Spec(), {}, backend);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_NE(first->GetImagePath(), second->GetImagePath());
}

TEST_F(VirtualDiskTest, TemporaryDiskCannotBePersistent) {
	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(), Persistent(), backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::InvalidArgument);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, FailedTemporaryDiskLeavesNoDirectory) {
	backend.failures["Initialize"] = Error{ Errc::Timeout, "Initialize" };
	const auto countTemporary = [] {
		const auto entries = fs::directory_iterator(fs::temp_directory_path());
		return std::count_if(fs::begin(entries), fs::end(entries), [](const fs::directory_entry& entry) {
			const std::wstring name = entry.path().filename().wstring();
			return name.starts_with(L"vdt-") && !name.starts_with(L"vdt-test-");
		});
	};
	const auto before = countTemporary();

	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(countTemporary(), before);
}

// --- Safety ---

TEST_F(VirtualDiskTest, CreateRefusesATargetTheOsDoesNotConfirmAsVirtual) {
	backend.reportsVirtual = false;

	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::NotVirtualDisk);
	EXPECT_FALSE(Called("Initialize"));
}

TEST_F(VirtualDiskTest, CreateTreatsAFailedSafetyCheckAsARefusal) {
	backend.failures["IsFileBackedVirtualDisk"] = Error{ Errc::Timeout, "Query" };

	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::NotVirtualDisk);
	EXPECT_FALSE(Called("Initialize"));
}

TEST_F(VirtualDiskTest, FormatRefusesATargetTheOsDoesNotConfirmAsVirtual) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();
	backend.reportsVirtual = false;

	Result<void> formatted = disk->Format({});

	ASSERT_FALSE(formatted);
	EXPECT_EQ(formatted.GetError().code, Errc::NotVirtualDisk);
	EXPECT_FALSE(Called("Initialize"));
}

TEST_F(VirtualDiskTest, FormatTreatsAFailedSafetyCheckAsARefusal) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();
	backend.failures["IsFileBackedVirtualDisk"] = Error{ Errc::Timeout, "Query" };

	Result<void> formatted = disk->Format({});

	ASSERT_FALSE(formatted);
	EXPECT_EQ(formatted.GetError().code, Errc::NotVirtualDisk);
	EXPECT_FALSE(Called("Initialize"));
}

TEST_F(VirtualDiskTest, TheSafetyCheckRunsImmediatelyBeforeInitialize) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();

	ASSERT_TRUE(disk->Format({}));

	EXPECT_EQ(backend.calls, (Calls{ "Unmount", "IsFileBackedVirtualDisk", "Initialize", "Mount" }));
}

// --- Opening ---

TEST_F(VirtualDiskTest, OpenAttachesAndMounts) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	EXPECT_EQ(backend.calls, (Calls{ "ListAttached", "Attach", "Mount" }));
	EXPECT_EQ(disk->GetMountPath(), fs::path("FakeMount"));
}

TEST_F(VirtualDiskTest, OpenRefusesAnImageThatIsAlreadyAttached) {
	WriteImageFile();
	backend.attachedImages.push_back({ Image(), "device", "mount" });

	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::AlreadyAttached);
	EXPECT_FALSE(Called("Attach"));
}

TEST_F(VirtualDiskTest, OpenStopsIfItCannotTellWhatIsAttached) {
	WriteImageFile();
	backend.failures["ListAttached"] = Error{ Errc::Timeout, "List" };

	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_FALSE(Called("Attach"));
}

TEST_F(VirtualDiskTest, OpenIgnoresOtherAttachedImages) {
	WriteImageFile("a.vhdx");
	WriteImageFile("b.vhdx");
	backend.attachedImages.push_back({ Image("b.vhdx"), "device", "mount" });

	EXPECT_TRUE(VirtualDisk::Open(Image("a.vhdx"), {}, backend));
}

TEST_F(VirtualDiskTest, OpenNeverRunsADestructiveStep) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	EXPECT_FALSE(Called("Initialize"));
}

TEST_F(VirtualDiskTest, OpenFailsForAMissingFile) {
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, std::errc::no_such_file_or_directory);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, OpenRefusesAnythingThatIsNotAFile) {
	Result<VirtualDisk> disk = VirtualDisk::Open(directory.Path(), {}, backend);
	ASSERT_FALSE(disk);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, OpenAcceptsABlankDisk) {
	WriteImageFile();
	backend.failures["Mount"] = Error{ Errc::NoVolume, "Mount" };
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	EXPECT_TRUE(disk->GetMountPath().empty());
}

TEST_F(VirtualDiskTest, OpenFailsIfAnExplicitMountHasNoVolume) {
	WriteImageFile();
	backend.failures["Mount"] = Error{ Errc::NoVolume, "Mount" };
	AttachOptions options;
	options.mountPath = fs::path("Z:\\");

	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), options, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::NoVolume);
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(VirtualDiskTest, OpenDetachesIfMountingFails) {
	WriteImageFile();
	backend.failures["Mount"] = Error{ Errc::Timeout, "Mount" };

	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, Errc::Timeout);
	EXPECT_EQ(backend.attached, 0);
	EXPECT_EQ(backend.leaked, 0);
}

TEST_F(VirtualDiskTest, OpenNeverDeletesTheImage) {
	WriteImageFile();
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
		ASSERT_TRUE(disk);
	}
	EXPECT_TRUE(fs::exists(Image()));
}

// --- Lifetime ---

TEST_F(VirtualDiskTest, DestructionEndsAScopedAttachment) {
	WriteImageFile();
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
		ASSERT_TRUE(disk);
		EXPECT_EQ(backend.attached, 1);
	}
	EXPECT_EQ(backend.attached, 0);
	EXPECT_TRUE(Called("Detach"));
	EXPECT_EQ(backend.leaked, 0);
}

TEST_F(VirtualDiskTest, DestructionLeavesAPersistentAttachmentInPlace) {
	WriteImageFile();
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image(), Persistent(), backend);
		ASSERT_TRUE(disk);
	}
	EXPECT_FALSE(Called("Detach"));
	EXPECT_TRUE(Called("Release"));
	EXPECT_EQ(backend.leaked, 0);
}

TEST_F(VirtualDiskTest, DetachReportsFailureAndCanBeRetried) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	backend.failures["Detach"] = Error{ Errc::Timeout, "Detach" };

	Result<void> first = disk->Detach();
	ASSERT_FALSE(first);
	EXPECT_EQ(first.GetError().code, Errc::Timeout);
	EXPECT_EQ(backend.attached, 1);

	backend.failures.clear();
	EXPECT_TRUE(disk->Detach());
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(VirtualDiskTest, DetachDeletesATemporaryImage) {
	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(), {}, backend);
	ASSERT_TRUE(disk);
	const fs::path image = disk->GetImagePath();

	ASSERT_TRUE(disk->Detach());

	EXPECT_FALSE(fs::exists(image));
}

TEST_F(VirtualDiskTest, NothingWorksAfterDetachExceptTheImagePath) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	ASSERT_TRUE(disk->Detach());

	EXPECT_EQ(disk->Detach().GetError().code, Errc::NotAttached);
	EXPECT_EQ(disk->Format({}).GetError().code, Errc::NotAttached);
	EXPECT_EQ(disk->Release().GetError().code, Errc::NotAttached);
	EXPECT_TRUE(disk->GetMountPath().empty());
	EXPECT_EQ(disk->GetImagePath(), Image());
}

TEST_F(VirtualDiskTest, ReleaseGivesUpAPersistentAttachment) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), Persistent(), backend);
	ASSERT_TRUE(disk);

	ASSERT_TRUE(disk->Release());

	EXPECT_FALSE(Called("Detach"));
	EXPECT_TRUE(disk->GetMountPath().empty());
	EXPECT_EQ(backend.leaked, 0);
}

TEST_F(VirtualDiskTest, ReleaseRefusesAScopedAttachment) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);

	Result<void> released = disk->Release();

	ASSERT_FALSE(released);
	EXPECT_EQ(released.GetError().code, Errc::InvalidArgument);
	EXPECT_EQ(backend.attached, 1);
}

// --- Moving ---

TEST_F(VirtualDiskTest, AMovedFromDiskDoesNothingOnDestruction) {
	WriteImageFile();
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), {}, backend);
	ASSERT_TRUE(disk);
	{
		VirtualDisk moved = std::move(*disk);
		EXPECT_EQ(backend.attached, 1);
		EXPECT_EQ(moved.GetMountPath(), fs::path("FakeMount"));
		EXPECT_TRUE(disk->GetMountPath().empty());
		EXPECT_EQ(disk->Detach().GetError().code, Errc::NotAttached);
		EXPECT_EQ(backend.attached, 1);
	}
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(VirtualDiskTest, MovingATemporaryDiskMovesTheDeletion) {
	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(), {}, backend);
	ASSERT_TRUE(disk);
	const fs::path image = disk->GetImagePath();
	{
		VirtualDisk moved = std::move(*disk);
		EXPECT_TRUE(fs::exists(image));
	}
	EXPECT_FALSE(fs::exists(image));
}

TEST_F(VirtualDiskTest, MoveAssignmentEndsTheOldAttachment) {
	WriteImageFile("a.vhdx");
	WriteImageFile("b.vhdx");
	Result<VirtualDisk> first = VirtualDisk::Open(Image("a.vhdx"), {}, backend);
	Result<VirtualDisk> second = VirtualDisk::Open(Image("b.vhdx"), {}, backend);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	ASSERT_EQ(backend.attached, 2);

	*first = std::move(*second);

	EXPECT_EQ(backend.attached, 1);
	EXPECT_EQ(first->GetImagePath(), Image("b.vhdx"));
	EXPECT_EQ(backend.leaked, 0);
}

// --- Formatting ---

TEST_F(VirtualDiskTest, FormatPassesTheVolumeThroughAndRemounts) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);

	VolumeSpec volume;
	volume.fileSystem = FileSystem::ExFat;
	volume.partitionStyle = PartitionStyle::Mbr;
	volume.label = "Scratch";
	volume.quickFormat = false;
	ASSERT_TRUE(disk->Format(volume));

	EXPECT_EQ(backend.lastVolume->fileSystem, FileSystem::ExFat);
	EXPECT_EQ(backend.lastVolume->partitionStyle, PartitionStyle::Mbr);
	EXPECT_EQ(backend.lastVolume->label, "Scratch");
	EXPECT_FALSE(backend.lastVolume->quickFormat);
	EXPECT_EQ(disk->GetMountPath(), fs::path("FakeMount"));
}

TEST_F(VirtualDiskTest, FormattingToNoFileSystemLeavesItUnmounted) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	VolumeSpec volume;
	volume.fileSystem = FileSystem::None;

	ASSERT_TRUE(disk->Format(volume));

	EXPECT_TRUE(disk->GetMountPath().empty());
}

TEST_F(VirtualDiskTest, FormatStopsIfUnmountFails) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();
	backend.failures["Unmount"] = Error{ Errc::Timeout, "Unmount" };

	EXPECT_FALSE(disk->Format({}));

	EXPECT_FALSE(Called("Initialize"));
	EXPECT_EQ(disk->GetMountPath(), fs::path("FakeMount"));
}

TEST_F(VirtualDiskTest, FormatRejectsAnUnsupportedFileSystemWithoutUnmounting) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), {}, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();
	backend.unsupportedFileSystems = { FileSystem::Ext4 };
	VolumeSpec volume;
	volume.fileSystem = FileSystem::Ext4;

	Result<void> formatted = disk->Format(volume);

	ASSERT_FALSE(formatted);
	EXPECT_EQ(formatted.GetError().code, Errc::Unsupported);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(VirtualDiskTest, FormatRejectsAReadOnlyDisk) {
	WriteImageFile();
	AttachOptions options;
	options.readOnly = true;
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), options, backend);
	ASSERT_TRUE(disk);
	backend.calls.clear();

	Result<void> formatted = disk->Format({});

	ASSERT_FALSE(formatted);
	EXPECT_EQ(formatted.GetError().code, Errc::InvalidArgument);
	EXPECT_TRUE(backend.calls.empty());
}

// --- Documentation ---

// These are the examples from README.md and DESIGN.md. Keep them in step.
TEST_F(VirtualDiskTest, TheDocumentedExamplesCompileAndWork) {
	auto disk = VirtualDisk::CreateTemporary({ .size = MiB(64) }, {}, backend);
	ASSERT_TRUE(disk);
	EXPECT_EQ(backend.lastSpec->size, MiB(64));

	auto chosen = VirtualDisk::CreateTemporary({
		.size = GiB(1),
		.format = ImageFormat::Vhdx,
		.volume = { .fileSystem = FileSystem::ExFat, .label = "Scratch" } }, {}, backend);
	ASSERT_TRUE(chosen);
	EXPECT_EQ(backend.lastVolume->fileSystem, FileSystem::ExFat);
	EXPECT_EQ(backend.lastVolume->label, "Scratch");
}

// --- Free functions ---

TEST_F(VirtualDiskTest, DetachImageGoesToTheBackend) {
	EXPECT_TRUE(DetachImage(Image(), backend));
	EXPECT_EQ(backend.calls, (Calls{ "DetachImage" }));
}

TEST_F(VirtualDiskTest, ListAttachedGoesToTheBackend) {
	backend.attachedImages.push_back({ Image(), "device", "mount" });
	Result<std::vector<AttachedImage>> images = ListAttached(backend);
	ASSERT_TRUE(images);
	ASSERT_EQ(images->size(), 1u);
	EXPECT_EQ(images->front().image, Image());
}
