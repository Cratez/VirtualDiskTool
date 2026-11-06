#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "Cli.h"
#include "FakeBackend.h"

namespace fs = std::filesystem;
using namespace vdt;
using test::FakeBackend;
using test::TempDirectory;

namespace {

using Calls = std::vector<std::string>;

class CliTest : public testing::Test {
protected:
	int Run(std::vector<std::string> args) {
		out.str("");
		err.str("");
		return cli::Run(args, out, err, backend);
	}

	std::string Image(const char* name = "disk.vhdx") const {
		return (directory.Path() / name).string();
	}

	void WriteImageFile() const {
		std::ofstream(Image()) << "existing";
	}

	FakeBackend backend;
	TempDirectory directory;
	std::ostringstream out;
	std::ostringstream err;
};

} // anonymous namespace

// --- General ---

TEST_F(CliTest, NoArgumentsIsAUsageError) {
	EXPECT_EQ(Run({}), cli::kExitUsage);
	EXPECT_NE(err.str().find("Usage:"), std::string::npos);
}

TEST_F(CliTest, UnknownCommandIsAUsageError) {
	EXPECT_EQ(Run({ "frobnicate" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, HelpGoesToStandardOutput) {
	EXPECT_EQ(Run({ "help" }), cli::kExitSuccess);
	EXPECT_NE(out.str().find("Usage:"), std::string::npos);
}

TEST_F(CliTest, VersionNamesTheBackend) {
	EXPECT_EQ(Run({ "version" }), cli::kExitSuccess);
	EXPECT_NE(out.str().find("Fake backend"), std::string::npos);
	EXPECT_EQ(out.str().find("VirtualDiskTool ("), std::string::npos) << "The version number is missing.";
}

// --- create ---

TEST_F(CliTest, CreateFormatsThenDetachesByDefault) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "64M" }), cli::kExitSuccess);
	EXPECT_EQ(backend.calls, (Calls{ "CreateImage", "Attach", "IsFileBackedVirtualDisk", "Initialize", "Mount", "Detach" }));
	EXPECT_EQ(backend.lastSpec->size, MiB(64));
	EXPECT_EQ(backend.lastOptions->lifetime, Lifetime::Scoped);
	EXPECT_EQ(out.str(), Image() + "\n");
}

TEST_F(CliTest, CreatePassesEveryOptionThrough) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "1G", "--format", "vhdx", "--fixed", "--fs", "exfat",
		"--partition", "mbr", "--label", "Scratch", "--full-format" }), cli::kExitSuccess);
	EXPECT_EQ(backend.lastSpec->size, GiB(1));
	EXPECT_EQ(backend.lastSpec->format, ImageFormat::Vhdx);
	EXPECT_EQ(backend.lastSpec->allocation, Allocation::Fixed);
	EXPECT_EQ(backend.lastVolume->fileSystem, FileSystem::ExFat);
	EXPECT_EQ(backend.lastVolume->partitionStyle, PartitionStyle::Mbr);
	EXPECT_EQ(backend.lastVolume->label, "Scratch");
	EXPECT_FALSE(backend.lastVolume->quickFormat);
}

TEST_F(CliTest, CreateWithNoFileSystemDoesNotAttach) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--fs", "none" }), cli::kExitSuccess);
	EXPECT_EQ(backend.calls, (Calls{ "CreateImage" }));
}

TEST_F(CliTest, CreateAttachLeavesItAttachedAndPrintsTheMount) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--attach", "--mount", "Z:\\" }), cli::kExitSuccess);
	EXPECT_EQ(backend.lastOptions->lifetime, Lifetime::Persistent);
	EXPECT_EQ(out.str(), "Z:\\\n");
	EXPECT_EQ(backend.calls.back(), "Release");
}

TEST_F(CliTest, MountNeedsAttach) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--mount", "Z:\\" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, CreateNeedsASize) {
	EXPECT_EQ(Run({ "create", Image() }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, CreateNeedsExactlyOneImage) {
	EXPECT_EQ(Run({ "create", "--size", "8M" }), cli::kExitUsage);
	EXPECT_EQ(Run({ "create", Image("a.vhdx"), Image("b.vhdx"), "--size", "8M" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, SizesAcceptEverySuffixInEitherCase) {
	const std::pair<const char*, uint64_t> sizes[] = {
		{ "1024", 1024 }, { "1k", 1024 }, { "1K", 1024 }, { "2m", MiB(2) },
		{ "2M", MiB(2) }, { "3g", GiB(3) }, { "3G", GiB(3) }, { "1T", GiB(1024) } };
	int index = 0;
	for (const auto& [text, expected] : sizes) {
		const std::string image = Image(("disk" + std::to_string(index++) + ".vhdx").c_str());
		EXPECT_EQ(Run({ "create", image, "--size", text, "--fs", "none" }), cli::kExitSuccess) << text;
		EXPECT_EQ(backend.lastSpec->size, expected) << text;
	}
}

TEST_F(CliTest, InvalidSizesAreUsageErrors) {
	const std::string tooBig = std::to_string(std::numeric_limits<uint64_t>::max()) + "K";
	for (const std::string& size : std::vector<std::string>{ "", "abc", "12X", "-5", "1.5G", "M", "1MB", tooBig }) {
		EXPECT_EQ(Run({ "create", Image(), "--size", size }), cli::kExitUsage) << size;
	}
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, UnknownFileSystemNeverFallsBackToAnother) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--fs", "ntsf" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, FileSystemNamesIgnoreCase) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--fs", "NTFS" }), cli::kExitSuccess);
	EXPECT_EQ(backend.lastVolume->fileSystem, FileSystem::Ntfs);
}

TEST_F(CliTest, UnknownFormatAndPartitionStyleAreUsageErrors) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--format", "qcow2" }), cli::kExitUsage);
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--partition", "apm" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, UnknownOptionsAndMissingValuesAreUsageErrors) {
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M", "--force" }), cli::kExitUsage);
	EXPECT_EQ(Run({ "create", Image(), "--size" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, FailuresGoToStandardErrorWithExitCodeOne) {
	backend.failures["Attach"] = Error{ Errc::Timeout, "Attach" };
	EXPECT_EQ(Run({ "create", Image(), "--size", "8M" }), cli::kExitFailure);
	EXPECT_TRUE(out.str().empty());
	EXPECT_NE(err.str().find("Attach"), std::string::npos);
}

// --- attach, detach ---

TEST_F(CliTest, AttachIsPersistentAndPrintsTheMount) {
	WriteImageFile();
	EXPECT_EQ(Run({ "attach", Image() }), cli::kExitSuccess);
	EXPECT_EQ(backend.lastOptions->lifetime, Lifetime::Persistent);
	EXPECT_FALSE(backend.lastOptions->readOnly);
	EXPECT_EQ(out.str(), "FakeMount\n");
	EXPECT_EQ(backend.attached, 0);
	EXPECT_EQ(backend.leaked, 0);
}

TEST_F(CliTest, AttachPassesItsOptionsThrough) {
	WriteImageFile();
	EXPECT_EQ(Run({ "attach", Image(), "--read-only", "--mount", "Z:\\" }), cli::kExitSuccess);
	EXPECT_TRUE(backend.lastOptions->readOnly);
	EXPECT_EQ(backend.lastOptions->mountPath, fs::path("Z:\\"));
}

TEST_F(CliTest, AttachOfAMissingImageFails) {
	EXPECT_EQ(Run({ "attach", Image() }), cli::kExitFailure);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, DetachGoesThroughTheBackend) {
	EXPECT_EQ(Run({ "detach", Image() }), cli::kExitSuccess);
	EXPECT_EQ(backend.calls, (Calls{ "DetachImage" }));
}

TEST_F(CliTest, DetachReportsNotAttached) {
	backend.failures["DetachImage"] = Error{ Errc::NotAttached, "DetachImage" };
	EXPECT_EQ(Run({ "detach", Image() }), cli::kExitFailure);
}

// --- format ---

TEST_F(CliTest, FormatRequiresConfirmation) {
	WriteImageFile();
	EXPECT_EQ(Run({ "format", Image(), "--fs", "ntfs" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, FormatErasesAnImage) {
	WriteImageFile();
	EXPECT_EQ(Run({ "format", Image(), "--fs", "exfat", "--yes" }), cli::kExitSuccess);
	EXPECT_EQ(backend.lastVolume->fileSystem, FileSystem::ExFat);
	EXPECT_EQ(backend.calls.back(), "Detach");
}

TEST_F(CliTest, FormatRefusesAnAttachedImage) {
	WriteImageFile();
	backend.attachedImages.push_back({ Image(), "device", "mount" });

	EXPECT_EQ(Run({ "format", Image(), "--fs", "exfat", "--yes" }), cli::kExitFailure);

	EXPECT_EQ(backend.calls, (Calls{ "ListAttached" }));
	EXPECT_NE(err.str().find("already attached"), std::string::npos);
}

// The first attempt at this tool took a drive letter here, so a typo could erase a real drive.
TEST_F(CliTest, FormatNeverReachesTheBackendForADriveLetter) {
	for (const char* target : { "C", "C:", "C:\\", "D:", "D:\\", "\\\\.\\PhysicalDrive0", "\\\\.\\C:" }) {
		EXPECT_EQ(Run({ "format", target, "--fs", "ntfs", "--yes" }), cli::kExitFailure) << target;
		EXPECT_TRUE(backend.calls.empty()) << target;
	}
}

// --- list ---

TEST_F(CliTest, ListPrintsOneTabSeparatedLinePerDisk) {
	backend.attachedImages.push_back({ "a.vhdx", "device1", "Z:\\" });
	backend.attachedImages.push_back({ "b.vhdx", "device2", "" });
	EXPECT_EQ(Run({ "list" }), cli::kExitSuccess);
	EXPECT_EQ(out.str(), "a.vhdx\tdevice1\tZ:\\\nb.vhdx\tdevice2\t\n");
}

TEST_F(CliTest, ListTakesNoArguments) {
	EXPECT_EQ(Run({ "list", "extra" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliTest, NonAsciiPathsSurviveTheRoundTrip) {
	const std::string name = reinterpret_cast<const char*>(u8"\u30c6\u30b9\u30c8.vhdx");
	const std::string image = (directory.Path().string() + "\\" + name);
	EXPECT_EQ(Run({ "create", image, "--size", "8M", "--fs", "none" }), cli::kExitSuccess);
	EXPECT_EQ(out.str(), image + "\n");
	EXPECT_TRUE(fs::exists(fs::path(std::u8string(image.begin(), image.end()))));
}
