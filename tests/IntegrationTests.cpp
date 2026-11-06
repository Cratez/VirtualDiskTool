// Tests that attach real virtual disks. They need admin rights and are reported as skipped
// without them. They only ever touch images they create in their own temporary directory.
//
// A failed step must fail the test. Never write "if (result) { ...checks... }" here: that
// passes when the operation the test exists to check didn't work.

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "Cli.h"
#include "FakeBackend.h"
#include "vdt/VirtualDisk.h"
#include "windows/DiskLayout.h"
#include "windows/Win32.h"

namespace fs = std::filesystem;
using namespace vdt;
using test::TempDirectory;

namespace {

constexpr auto kDetachTimeout = std::chrono::seconds(30);
constexpr DWORD kCrashChildTimeoutMs = 60000;
constexpr DWORD kCrashChildExitCode = 3;
constexpr int kDisksToProbe = 64;

bool IsElevated() {
	win::UniqueHandle token;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.Put())) {
		return false;
	}
	TOKEN_ELEVATION elevation = {};
	DWORD size = 0;
	return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size)
		&& elevation.TokenIsElevated != 0;
}

struct VolumeDetails {
	std::wstring fileSystem;
	std::wstring label;
};

VolumeDetails GetVolumeDetails(const fs::path& mountPath) {
	std::wstring root = mountPath.native();
	if (root.back() != L'\\') {
		root += L'\\';
	}
	wchar_t label[MAX_PATH + 1] = {};
	wchar_t fileSystem[MAX_PATH + 1] = {};
	GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, fileSystem, MAX_PATH + 1);
	return { fileSystem, label };
}

bool SamePath(const fs::path& a, const fs::path& b) {
	std::error_code error;
	return fs::equivalent(a, b, error) && !error;
}

bool IsAttached(const fs::path& image) {
	Result<std::vector<AttachedImage>> attached = ListAttached();
	if (!attached) {
		ADD_FAILURE() << attached.GetError().Message();
		return false;
	}
	for (const AttachedImage& entry : *attached) {
		if (SamePath(entry.image, image)) {
			return true;
		}
	}
	return false;
}

// Whoever runs these tests may not be able to debug them, so a failed lookup has to put what
// Windows said into the report.
void ReportBackingFiles() {
	for (int index = 0; index < kDisksToProbe; ++index) {
		const std::wstring disk = L"\\\\.\\PhysicalDrive" + std::to_wstring(index);
		Result<bool> isVirtual = win::IsFileBackedVirtualDisk(disk);
		if (!isVirtual || !*isVirtual) {
			continue;
		}

		Result<std::vector<std::wstring>> files = win::GetBackingFiles(disk);
		if (!files) {
			ADD_FAILURE() << "PhysicalDrive" << index << ": " << files.GetError().Message();
			continue;
		}
		ADD_FAILURE() << "PhysicalDrive" << index << " has " << files->size() << " candidate(s)";
		for (const std::wstring& file : *files) {
			ADD_FAILURE() << "PhysicalDrive" << index << " candidate: " << file;
		}
	}
}

void WriteTextFile(const fs::path& path, const std::string& text) {
	std::ofstream file(path);
	file << text;
	ASSERT_TRUE(file.good()) << path;
}

std::string ReadTextFile(const fs::path& path) {
	std::ifstream file(path);
	std::string text;
	std::getline(file, text);
	return text;
}

class IntegrationTest : public testing::Test {
protected:
	void SetUp() override {
		if (!IsElevated()) {
			GTEST_SKIP() << "Needs administrator rights.";
		}
	}

	void TearDown() override {
		if (!IsElevated()) {
			return;
		}
		// A leak fails the test, then gets cleaned up so that it can't affect the next one.
		Result<std::vector<AttachedImage>> attached = ListAttached();
		ASSERT_TRUE(attached) << attached.GetError().Message();
		for (const AttachedImage& entry : *attached) {
			const fs::path relative = entry.image.lexically_relative(directory.Path());
			if (!relative.empty() && *relative.begin() != "..") {
				// Not the path itself, which throws for a name the ANSI code page can't represent.
				ADD_FAILURE() << "Left attached: " << entry.image.wstring();
				(void)DetachImage(entry.image);
			}
		}
	}

	fs::path Image(const std::string& name = "disk.vhdx") const {
		return directory.Path() / name;
	}

	static DiskSpec Spec(FileSystem fileSystem = FileSystem::Ntfs) {
		DiskSpec spec;
		spec.size = MiB(64);
		spec.volume.fileSystem = fileSystem;
		return spec;
	}

	TempDirectory directory;
};

using MatrixCase = std::tuple<ImageFormat, Allocation, FileSystem, PartitionStyle>;

class IntegrationMatrixTest : public IntegrationTest, public testing::WithParamInterface<MatrixCase> {};

std::wstring ExpectedName(FileSystem fileSystem) {
	switch (fileSystem) {
	case FileSystem::Ntfs:  return L"NTFS";
	case FileSystem::ExFat: return L"exFAT";
	case FileSystem::Fat32: return L"FAT32";
	default:                return L"";
	}
}

std::string CaseName(ImageFormat format, Allocation allocation, FileSystem fileSystem) {
	std::string name = format == ImageFormat::Vhd ? "Vhd" : "Vhdx";
	name += allocation == Allocation::Fixed ? "Fixed" : "Dynamic";
	name += fileSystem == FileSystem::Ntfs ? "Ntfs" : fileSystem == FileSystem::ExFat ? "ExFat" : "Fat32";
	return name;
}

std::string MatrixCaseName(const testing::TestParamInfo<MatrixCase>& info) {
	const auto& [format, allocation, fileSystem, style] = info.param;
	return CaseName(format, allocation, fileSystem) + (style == PartitionStyle::Gpt ? "Gpt" : "Mbr");
}

} // anonymous namespace

// The whole point of the library, in every combination that matters.
TEST_P(IntegrationMatrixTest, GivesAUsableVolumeAndCleansUp) {
	const auto& [format, allocation, fileSystem, style] = GetParam();
	DiskSpec spec = Spec(fileSystem);
	spec.format = format;
	spec.allocation = allocation;
	spec.volume.partitionStyle = style;
	spec.volume.label = "VDTTEST";

	fs::path image;
	{
		Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(spec);
		ASSERT_TRUE(disk) << disk.GetError().Message();
		image = disk->GetImagePath();

		ASSERT_FALSE(disk->GetMountPath().empty());
		const VolumeDetails details = GetVolumeDetails(disk->GetMountPath());
		EXPECT_EQ(details.fileSystem, ExpectedName(fileSystem));
		EXPECT_EQ(details.label, L"VDTTEST");

		const fs::path file = disk->GetMountPath() / "hello.txt";
		WriteTextFile(file, "hello");
		EXPECT_EQ(ReadTextFile(file), "hello");
	}
	EXPECT_FALSE(fs::exists(image));
	EXPECT_FALSE(fs::exists(image.parent_path()));
}

INSTANTIATE_TEST_SUITE_P(Matrix, IntegrationMatrixTest,
	testing::Combine(
		testing::Values(ImageFormat::Vhd, ImageFormat::Vhdx),
		testing::Values(Allocation::Dynamic, Allocation::Fixed),
		testing::Values(FileSystem::Ntfs, FileSystem::ExFat, FileSystem::Fat32),
		testing::Values(PartitionStyle::Gpt, PartitionStyle::Mbr)),
	MatrixCaseName);

TEST_F(IntegrationTest, TheDefaultsGiveAnNtfsVolume) {
	DiskSpec spec;
	spec.size = MiB(64);
	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(spec);
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"NTFS");
}

// The other half of the safety check: it has to say yes for a real attached image.
TEST_F(IntegrationTest, AnAttachedImageIsReportedAsVirtual) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec());
	ASSERT_TRUE(disk) << disk.GetError().Message();

	Result<std::vector<AttachedImage>> attached = ListAttached();
	ASSERT_TRUE(attached) << attached.GetError().Message();
	bool found = false;
	for (const AttachedImage& entry : *attached) {
		if (!SamePath(entry.image, Image())) {
			continue;
		}
		found = true;
		if (entry.device.empty()) {
			ReportBackingFiles();
		}
		EXPECT_EQ(entry.mountPath, disk->GetMountPath());
		Result<bool> isVirtual = win::IsFileBackedVirtualDisk(entry.device.native());
		ASSERT_TRUE(isVirtual) << isVirtual.GetError().Message();
		EXPECT_TRUE(*isVirtual);
	}
	EXPECT_TRUE(found) << "ListAttached did not report the image.";
}

TEST_F(IntegrationTest, DataSurvivesDetachAndReattach) {
	{
		Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec());
		ASSERT_TRUE(disk) << disk.GetError().Message();
		WriteTextFile(disk->GetMountPath() / "kept.txt", "kept");
		ASSERT_TRUE(disk->Detach());
	}
	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(ReadTextFile(disk->GetMountPath() / "kept.txt"), "kept");
}

TEST_F(IntegrationTest, DestructionDetaches) {
	{
		Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec());
		ASSERT_TRUE(disk) << disk.GetError().Message();
		EXPECT_TRUE(IsAttached(Image()));
	}
	EXPECT_FALSE(IsAttached(Image()));
	EXPECT_TRUE(fs::exists(Image()));
}

TEST_F(IntegrationTest, APersistentAttachmentOutlivesItsObject) {
	ASSERT_TRUE(CreateBlankImage(Image(), Spec()));
	AttachOptions options;
	options.lifetime = Lifetime::Persistent;
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image(), options);
		ASSERT_TRUE(disk) << disk.GetError().Message();
		ASSERT_TRUE(disk->Release());
	}
	EXPECT_TRUE(IsAttached(Image()));

	Result<void> detached = DetachImage(Image());
	ASSERT_TRUE(detached) << detached.GetError().Message();
	EXPECT_FALSE(IsAttached(Image()));
}

TEST_F(IntegrationTest, DetachWorksOnAPersistentAttachment) {
	ASSERT_TRUE(CreateBlankImage(Image(), Spec()));
	AttachOptions options;
	options.lifetime = Lifetime::Persistent;
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), options);
	ASSERT_TRUE(disk) << disk.GetError().Message();

	Result<void> detached = disk->Detach();

	ASSERT_TRUE(detached) << detached.GetError().Message();
	EXPECT_FALSE(IsAttached(Image()));
}

// Writing to an attached disk changes what Windows allows, so a blank disk proves nothing here.
TEST_F(IntegrationTest, DetachWorksAfterAPersistentDiskIsWrittenTo) {
	AttachOptions options;
	options.lifetime = Lifetime::Persistent;
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), options);
	ASSERT_TRUE(disk) << disk.GetError().Message();
	WriteTextFile(disk->GetMountPath() / "kept.txt", "kept");

	Result<void> detached = disk->Detach();

	ASSERT_TRUE(detached) << detached.GetError().Message();
	EXPECT_FALSE(IsAttached(Image()));
}

TEST_F(IntegrationTest, FormatReplacesTheContents) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(FileSystem::Ntfs));
	ASSERT_TRUE(disk) << disk.GetError().Message();
	WriteTextFile(disk->GetMountPath() / "old.txt", "old");

	VolumeSpec volume;
	volume.fileSystem = FileSystem::ExFat;
	Result<void> formatted = disk->Format(volume);

	ASSERT_TRUE(formatted) << formatted.GetError().Message();
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"exFAT");
	EXPECT_FALSE(fs::exists(disk->GetMountPath() / "old.txt"));
}

TEST_F(IntegrationTest, ABlankDiskHasNoMountUntilFormatted) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(FileSystem::None));
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_TRUE(disk->GetMountPath().empty());

	Result<void> formatted = disk->Format({});

	ASSERT_TRUE(formatted) << formatted.GetError().Message();
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"NTFS");
}

TEST_F(IntegrationTest, FormattingToNoFileSystemEmptiesTheDisk) {
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	VolumeSpec volume;
	volume.fileSystem = FileSystem::None;

	Result<void> formatted = disk->Format(volume);

	ASSERT_TRUE(formatted) << formatted.GetError().Message();
	EXPECT_TRUE(disk->GetMountPath().empty());
}

TEST_F(IntegrationTest, MountsAtAChosenFolder) {
	const fs::path folder = Image("mount");
	fs::create_directories(folder);
	AttachOptions options;
	options.mountPath = folder;

	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), options);

	ASSERT_TRUE(disk) << disk.GetError().Message();
	WriteTextFile(folder / "hello.txt", "hello");
	EXPECT_EQ(ReadTextFile(folder / "hello.txt"), "hello");
	ASSERT_TRUE(disk->Detach());
	EXPECT_TRUE(fs::is_empty(folder));
}

TEST_F(IntegrationTest, MountsAtAChosenDriveLetter) {
	const DWORD used = GetLogicalDrives();
	wchar_t letter = L'Z';
	while (letter >= L'D' && (used & (1u << (letter - L'A')))) {
		--letter;
	}
	ASSERT_GE(letter, L'D') << "No free drive letter.";
	const std::wstring root = { letter, L':', L'\\' };

	AttachOptions options;
	options.mountPath = fs::path(root);
	Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec(), options);

	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(disk->GetMountPath(), fs::path(root));
	ASSERT_TRUE(disk->Detach());
	EXPECT_FALSE(GetLogicalDrives() & (1u << (letter - L'A')));
}

TEST_F(IntegrationTest, AReadOnlyAttachmentCannotBeWritten) {
	{
		Result<VirtualDisk> disk = VirtualDisk::Create(Image(), Spec());
		ASSERT_TRUE(disk) << disk.GetError().Message();
		WriteTextFile(disk->GetMountPath() / "kept.txt", "kept");
	}

	AttachOptions options;
	options.readOnly = true;
	Result<VirtualDisk> disk = VirtualDisk::Open(Image(), options);

	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(ReadTextFile(disk->GetMountPath() / "kept.txt"), "kept");
	std::ofstream file(disk->GetMountPath() / "new.txt");
	EXPECT_FALSE(file.is_open());
}

TEST_F(IntegrationTest, UnicodePathsAndLabelsWork) {
	const fs::path image = directory.Path() / L"テスト.vhdx";
	DiskSpec spec = Spec();
	spec.volume.label = reinterpret_cast<const char*>(u8"テスト");

	Result<VirtualDisk> disk = VirtualDisk::Create(image, spec);

	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).label, L"テスト");
}

// A full format writes across the whole volume, unlike a quick one, so the storage underneath
// matters. This keeps going after a failure, to show which combinations are affected.
TEST_F(IntegrationTest, FullFormatWorks) {
	for (const ImageFormat format : { ImageFormat::Vhd, ImageFormat::Vhdx }) {
		for (const Allocation allocation : { Allocation::Dynamic, Allocation::Fixed }) {
			for (const FileSystem fileSystem : { FileSystem::Ntfs, FileSystem::ExFat, FileSystem::Fat32 }) {
				SCOPED_TRACE(CaseName(format, allocation, fileSystem));
				DiskSpec spec = Spec(fileSystem);
				spec.format = format;
				spec.allocation = allocation;
				spec.volume.quickFormat = false;

				Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(spec);

				if (!disk) {
					ADD_FAILURE() << disk.GetError().Message();
					continue;
				}
				EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, ExpectedName(fileSystem));
			}
		}
	}
}

TEST_F(IntegrationTest, FullNtfsFormatWorksOnLargerDisks) {
	for (const uint64_t size : { MiB(256), GiB(1) }) {
		SCOPED_TRACE(std::to_string(size / MiB(1)) + " MiB");
		DiskSpec spec = Spec();
		spec.size = size;
		spec.volume.quickFormat = false;

		Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(spec);

		if (!disk) {
			ADD_FAILURE() << disk.GetError().Message();
			continue;
		}
		EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"NTFS");
	}
}

TEST_F(IntegrationTest, TwoDisksCanBeAttachedAtOnce) {
	Result<VirtualDisk> first = VirtualDisk::CreateTemporary(Spec());
	Result<VirtualDisk> second = VirtualDisk::CreateTemporary(Spec());
	ASSERT_TRUE(first) << first.GetError().Message();
	ASSERT_TRUE(second) << second.GetError().Message();
	EXPECT_NE(first->GetMountPath(), second->GetMountPath());
}

TEST_F(IntegrationTest, FatAndUdfVolumesWork) {
	for (const FileSystem fileSystem : { FileSystem::Fat, FileSystem::Udf }) {
		Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(Spec(fileSystem));
		ASSERT_TRUE(disk) << disk.GetError().Message();
		EXPECT_FALSE(GetVolumeDetails(disk->GetMountPath()).fileSystem.empty());
	}
}

// Whether ReFS can be created depends on the Windows edition, so this only insists that a
// refusal is clean. Run-Tests.ps1 and the teardown check will catch anything left behind.
TEST_F(IntegrationTest, ReFsWorksOrFailsCleanly) {
	DiskSpec spec = Spec(FileSystem::ReFs);
	spec.size = GiB(2);

	Result<VirtualDisk> disk = VirtualDisk::CreateTemporary(spec);

	if (!disk) {
		GTEST_SKIP() << "ReFS unavailable: " << disk.GetError().Message();
	}
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"ReFS");
}

// A child process attaches a disk and dies without running any destructor.
TEST_F(IntegrationTest, AScopedAttachmentEndsWhenTheProcessDies) {
	ASSERT_TRUE(CreateBlankImage(Image(), Spec()));

	wchar_t self[MAX_PATH];
	ASSERT_NE(GetModuleFileNameW(nullptr, self, MAX_PATH), 0u);
	std::wstring commandLine = L"\"" + std::wstring(self) + L"\" --crash-child \"" + Image().native() + L"\"";

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	ASSERT_TRUE(CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
	win::UniqueHandle processHandle(process.hProcess);
	win::UniqueHandle threadHandle(process.hThread);

	ASSERT_EQ(WaitForSingleObject(process.hProcess, kCrashChildTimeoutMs), WAIT_OBJECT_0);
	DWORD exitCode = 0;
	ASSERT_TRUE(GetExitCodeProcess(process.hProcess, &exitCode));
	ASSERT_EQ(exitCode, kCrashChildExitCode) << "The child failed before it could crash.";

	const auto deadline = std::chrono::steady_clock::now() + kDetachTimeout;
	while (IsAttached(Image()) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	EXPECT_FALSE(IsAttached(Image()));
}

// Only means something unelevated. It can't skip when elevated, because the VM run treats any
// skip other than ReFS as a failure.
TEST(WithoutAdminRightsTest, AttachingReportsAPortablePermissionError) {
	if (IsElevated()) {
		return;
	}
	TempDirectory directory;
	const fs::path image = directory.Path() / "disk.vhdx";
	DiskSpec spec;
	spec.size = MiB(8);
	ASSERT_TRUE(CreateBlankImage(image, spec));

	Result<VirtualDisk> disk = VirtualDisk::Open(image);

	ASSERT_FALSE(disk);
	EXPECT_EQ(disk.GetError().code, std::errc::permission_denied);
	EXPECT_NE(disk.GetError().Message().find("administrator"), std::string::npos);
}

// --- The command line tool, against real disks ---

namespace {

std::string Utf8(const fs::path& path) {
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
}

fs::path PathFromUtf8(const std::string& text) {
	return fs::path(std::u8string(text.begin(), text.end()));
}

bool PathExists(const fs::path& path) {
	std::error_code error;
	return fs::exists(path, error) && !error;
}

class CliIntegrationTest : public IntegrationTest {
protected:
	int Run(const std::vector<std::string>& args) {
		out.str("");
		err.str("");
		return cli::Run(args, out, err, DefaultBackend());
	}

	std::string ImageArgument(const std::string& name = "disk.vhdx") const {
		return Utf8(Image(name));
	}

	// The one path a command printed.
	fs::path PrintedPath() const {
		std::string text = out.str();
		text.erase(text.find_last_not_of("\r\n") + 1);
		return PathFromUtf8(text);
	}

	// The fields `list` printed for an image, if it listed that image.
	std::optional<std::vector<std::string>> ListedFields(const fs::path& image) const {
		std::istringstream lines(out.str());
		for (std::string line; std::getline(lines, line);) {
			std::vector<std::string> fields;
			std::istringstream columns(line);
			for (std::string field; std::getline(columns, field, '\t');) {
				fields.push_back(field);
			}
			if (!fields.empty() && SamePath(PathFromUtf8(fields.front()), image)) {
				return fields;
			}
		}
		return std::nullopt;
	}

	std::ostringstream out;
	std::ostringstream err;
};

} // anonymous namespace

TEST_F(CliIntegrationTest, CreateLeavesAFormattedDetachedImage) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--fs", "ntfs", "--label", "VDTCLI" }), cli::kExitSuccess) << err.str();

	EXPECT_TRUE(SamePath(PrintedPath(), Image()));
	EXPECT_FALSE(IsAttached(Image()));

	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	const VolumeDetails details = GetVolumeDetails(disk->GetMountPath());
	EXPECT_EQ(details.fileSystem, L"NTFS");
	EXPECT_EQ(details.label, L"VDTCLI");
}

TEST_F(CliIntegrationTest, CreateAppliesEveryOptionToARealDisk) {
	ASSERT_EQ(Run({ "create", ImageArgument("disk.vhd"), "--size", "64M", "--format", "vhd", "--fixed",
		"--partition", "mbr", "--fs", "fat32", "--label", "VDTCLI", "--full-format" }), cli::kExitSuccess) << err.str();

	EXPECT_GE(fs::file_size(Image("disk.vhd")), MiB(64));

	Result<VirtualDisk> disk = VirtualDisk::Open(Image("disk.vhd"));
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(GetVolumeDetails(disk->GetMountPath()).fileSystem, L"FAT32");
}

TEST_F(CliIntegrationTest, CreateWithNoFileSystemMakesABlankImage) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "16M", "--fs", "none" }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(IsAttached(Image()));
	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_TRUE(disk->GetMountPath().empty());
}

TEST_F(CliIntegrationTest, CreateAttachStaysMountedUntilDetach) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--attach" }), cli::kExitSuccess) << err.str();
	const fs::path mount = PrintedPath();
	ASSERT_FALSE(mount.empty());
	EXPECT_TRUE(IsAttached(Image()));
	WriteTextFile(mount / "kept.txt", "kept");

	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(IsAttached(Image()));
	EXPECT_FALSE(PathExists(mount));

	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(ReadTextFile(disk->GetMountPath() / "kept.txt"), "kept");
}

TEST_F(CliIntegrationTest, CreateAttachMountsAtAChosenFolder) {
	const fs::path folder = Image("mount");
	fs::create_directories(folder);

	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--attach", "--mount", Utf8(folder) }), cli::kExitSuccess) << err.str();

	WriteTextFile(folder / "hello.txt", "hello");
	EXPECT_EQ(ReadTextFile(folder / "hello.txt"), "hello");
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
	EXPECT_TRUE(fs::is_empty(folder));
}

// FAT32 needs far more than 8 MiB, so this fails after attaching and partitioning, which is
// the worst point to fail at: there is a real disk to detach and a real image to remove.
TEST_F(CliIntegrationTest, AFailedCreateLeavesNothingBehind) {
	EXPECT_EQ(Run({ "create", ImageArgument(), "--size", "8M", "--fs", "fat32" }), cli::kExitFailure);

	EXPECT_TRUE(out.str().empty());
	EXPECT_FALSE(err.str().empty());
	EXPECT_FALSE(PathExists(Image()));
	EXPECT_FALSE(IsAttached(Image()));
}

TEST_F(CliIntegrationTest, CreateRefusesToOverwriteARealImage) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M" }), cli::kExitSuccess) << err.str();
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image());
		ASSERT_TRUE(disk) << disk.GetError().Message();
		WriteTextFile(disk->GetMountPath() / "kept.txt", "kept");
	}

	EXPECT_EQ(Run({ "create", ImageArgument(), "--size", "64M" }), cli::kExitFailure);

	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	EXPECT_EQ(ReadTextFile(disk->GetMountPath() / "kept.txt"), "kept");
}

TEST_F(CliIntegrationTest, AttachPrintsWhereTheVolumeIs) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--label", "VDTCLI" }), cli::kExitSuccess) << err.str();

	ASSERT_EQ(Run({ "attach", ImageArgument() }), cli::kExitSuccess) << err.str();

	EXPECT_EQ(GetVolumeDetails(PrintedPath()).label, L"VDTCLI");
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, AttachOfABlankImagePrintsNoPath) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "16M", "--fs", "none" }), cli::kExitSuccess) << err.str();

	ASSERT_EQ(Run({ "attach", ImageArgument() }), cli::kExitSuccess) << err.str();

	EXPECT_TRUE(PrintedPath().empty());
	EXPECT_TRUE(IsAttached(Image()));
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, AttachReadOnlyCannotBeWritten) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M" }), cli::kExitSuccess) << err.str();

	ASSERT_EQ(Run({ "attach", ImageArgument(), "--read-only" }), cli::kExitSuccess) << err.str();

	std::ofstream file(PrintedPath() / "new.txt");
	EXPECT_FALSE(file.is_open());
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, AttachingTwiceFailsAndLeavesTheFirstAlone) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--attach" }), cli::kExitSuccess) << err.str();
	const fs::path mount = PrintedPath();
	WriteTextFile(mount / "kept.txt", "kept");

	EXPECT_EQ(Run({ "attach", ImageArgument() }), cli::kExitFailure);

	EXPECT_EQ(ReadTextFile(mount / "kept.txt"), "kept");
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, DetachOfAnUnattachedImageFails) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "16M", "--fs", "none" }), cli::kExitSuccess) << err.str();

	EXPECT_EQ(Run({ "detach", ImageArgument() }), cli::kExitFailure);
}

TEST_F(CliIntegrationTest, DetachWorksForAVhdThatWasWrittenTo) {
	ASSERT_EQ(Run({ "create", ImageArgument("disk.vhd"), "--size", "64M", "--attach" }), cli::kExitSuccess) << err.str();
	WriteTextFile(PrintedPath() / "kept.txt", "kept");

	ASSERT_EQ(Run({ "detach", ImageArgument("disk.vhd") }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(IsAttached(Image("disk.vhd")));
}

TEST_F(CliIntegrationTest, ListDescribesAnImageOnlyWhileItIsAttached) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--attach" }), cli::kExitSuccess) << err.str();
	const fs::path mount = PrintedPath();

	ASSERT_EQ(Run({ "list" }), cli::kExitSuccess) << err.str();
	const std::optional<std::vector<std::string>> fields = ListedFields(Image());
	ASSERT_TRUE(fields.has_value()) << out.str();
	ASSERT_EQ(fields->size(), 3u) << out.str();
	EXPECT_TRUE((*fields)[1].starts_with("\\\\.\\PhysicalDrive")) << out.str();
	EXPECT_EQ(PathFromUtf8((*fields)[2]), mount);

	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();

	ASSERT_EQ(Run({ "list" }), cli::kExitSuccess) << err.str();
	EXPECT_FALSE(ListedFields(Image()).has_value()) << out.str();
}

TEST_F(CliIntegrationTest, FormatReplacesAnImagesContents) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--fs", "ntfs" }), cli::kExitSuccess) << err.str();
	{
		Result<VirtualDisk> disk = VirtualDisk::Open(Image());
		ASSERT_TRUE(disk) << disk.GetError().Message();
		WriteTextFile(disk->GetMountPath() / "old.txt", "old");
	}

	ASSERT_EQ(Run({ "format", ImageArgument(), "--fs", "exfat", "--label", "VDTNEW", "--yes" }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(IsAttached(Image()));
	Result<VirtualDisk> disk = VirtualDisk::Open(Image());
	ASSERT_TRUE(disk) << disk.GetError().Message();
	const VolumeDetails details = GetVolumeDetails(disk->GetMountPath());
	EXPECT_EQ(details.fileSystem, L"exFAT");
	EXPECT_EQ(details.label, L"VDTNEW");
	EXPECT_FALSE(PathExists(disk->GetMountPath() / "old.txt"));
}

// Erasing a disk someone has mounted must never happen, written to yet or not.
TEST_F(CliIntegrationTest, FormatRefusesAnAttachedImageBeforeItIsWrittenTo) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--fs", "ntfs", "--label", "VDTCLI" }), cli::kExitSuccess) << err.str();
	ASSERT_EQ(Run({ "attach", ImageArgument() }), cli::kExitSuccess) << err.str();
	const fs::path mount = PrintedPath();

	EXPECT_EQ(Run({ "format", ImageArgument(), "--fs", "exfat", "--yes" }), cli::kExitFailure);

	EXPECT_EQ(GetVolumeDetails(mount).fileSystem, L"NTFS");
	EXPECT_EQ(GetVolumeDetails(mount).label, L"VDTCLI");
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, FormatRefusesAnImageThatIsAttached) {
	ASSERT_EQ(Run({ "create", ImageArgument(), "--size", "64M", "--fs", "ntfs", "--attach" }), cli::kExitSuccess) << err.str();
	const fs::path mount = PrintedPath();
	WriteTextFile(mount / "kept.txt", "kept");

	EXPECT_EQ(Run({ "format", ImageArgument(), "--fs", "exfat", "--yes" }), cli::kExitFailure);

	EXPECT_EQ(ReadTextFile(mount / "kept.txt"), "kept");
	EXPECT_EQ(GetVolumeDetails(mount).fileSystem, L"NTFS");
	ASSERT_EQ(Run({ "detach", ImageArgument() }), cli::kExitSuccess) << err.str();
}

TEST_F(CliIntegrationTest, UnicodePathsWorkFromStartToFinish) {
	const fs::path image = directory.Path() / L"テスト.vhdx";

	ASSERT_EQ(Run({ "create", Utf8(image), "--size", "64M", "--attach" }), cli::kExitSuccess) << err.str();

	ASSERT_EQ(Run({ "list" }), cli::kExitSuccess) << err.str();
	EXPECT_TRUE(ListedFields(image).has_value()) << out.str();

	ASSERT_EQ(Run({ "detach", Utf8(image) }), cli::kExitSuccess) << err.str();
	EXPECT_FALSE(IsAttached(image));
}

int RunCrashChild(const fs::path& image) {
	Result<VirtualDisk> disk = VirtualDisk::Open(image);
	if (!disk) {
		return 1;
	}
	TerminateProcess(GetCurrentProcess(), kCrashChildExitCode);
	return 2;
}
