#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
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

// --- encryption ---

TEST(StripByteOrderMarksTest, RemovesThemFromTheStartOnly) {
	EXPECT_EQ(cli::StripByteOrderMarks("pw"), "pw");
	EXPECT_EQ(cli::StripByteOrderMarks("\xEF\xBB\xBFpw"), "pw");
	EXPECT_EQ(cli::StripByteOrderMarks("\xEF\xBB\xBF\xEF\xBB\xBFpw"), "pw");
	EXPECT_EQ(cli::StripByteOrderMarks("\xEF\xBB\xBF"), "");
	EXPECT_EQ(cli::StripByteOrderMarks(""), "");
	EXPECT_EQ(cli::StripByteOrderMarks("p\xEF\xBB\xBFw"), "p\xEF\xBB\xBFw");
}

namespace {

class CliEncryptionTest : public CliTest {
protected:
	int RunWithPassword(std::vector<std::string> args, std::optional<std::string> password = "password") {
		out.str("");
		err.str("");
		return cli::Run(args, out, err, backend, [&](bool confirm) {
			lastConfirm = confirm;
			++passwordReads;
			return password;
		});
	}

	std::string KeyFile() const {
		return Image("disk.key");
	}

	std::string ReadKeyFile() const {
		std::ifstream file(KeyFile());
		std::string key;
		std::getline(file, key);
		return key;
	}

	bool lastConfirm = false;
	int passwordReads = 0;
};

} // anonymous namespace

TEST_F(CliEncryptionTest, CreateEncryptsAndSavesTheRecoveryKey) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", KeyFile() }), cli::kExitSuccess) << err.str();

	EXPECT_EQ(backend.lastEncryptionPassword, "password");
	EXPECT_TRUE(lastConfirm);
	EXPECT_EQ(ReadKeyFile(), "recovery-key");
}

TEST_F(CliEncryptionTest, EncryptRequiresARecoveryKeyFile) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt" }), cli::kExitUsage);
	EXPECT_NE(err.str().find("--recovery-key-file"), std::string::npos);
	EXPECT_TRUE(backend.calls.empty());
	EXPECT_EQ(passwordReads, 0);
}

TEST_F(CliEncryptionTest, ARecoveryKeyFileNeedsEncrypt) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--recovery-key-file", KeyFile() }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, EncryptNeedsAFileSystem) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--fs", "none", "--encrypt", "--recovery-key-file", KeyFile() }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, APasswordIsNeverAnArgument) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", KeyFile(), "--password", "secret" }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, NoPasswordStopsEverythingAndLeavesNoKeyFile) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", KeyFile() }, std::nullopt), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
	EXPECT_FALSE(fs::exists(KeyFile()));
}

TEST_F(CliEncryptionTest, AnExistingKeyFileIsNeverOverwritten) {
	std::ofstream(KeyFile()) << "precious";

	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", KeyFile() }), cli::kExitFailure);

	EXPECT_TRUE(backend.calls.empty());
	EXPECT_EQ(passwordReads, 0);
	EXPECT_EQ(ReadKeyFile(), "precious");
}

TEST_F(CliEncryptionTest, AnUnwritableKeyFileFailsBeforeCreatingAnything) {
	const std::string path = Image("missing-folder\\disk.key");

	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", path }), cli::kExitFailure);

	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, AFailedCreateLeavesNoKeyFile) {
	backend.failures["Encrypt"] = Error{ Errc::TooSmallToEncrypt, "Encrypt" };

	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", KeyFile() }), cli::kExitFailure);

	EXPECT_FALSE(fs::exists(KeyFile()));
	EXPECT_FALSE(fs::exists(Image()));
}

TEST_F(CliEncryptionTest, AttachOfALockedImageExplainsWhatToDo) {
	WriteImageFile();
	backend.diskIsLocked = true;

	EXPECT_EQ(RunWithPassword({ "attach", Image() }), cli::kExitFailure);

	EXPECT_NE(err.str().find("password"), std::string::npos);
	EXPECT_EQ(passwordReads, 0);
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(CliEncryptionTest, AttachUnlockAsksForThePassword) {
	WriteImageFile();
	backend.diskIsLocked = true;

	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--unlock" }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(lastConfirm);
	EXPECT_FALSE(backend.diskIsLocked);
}

TEST_F(CliEncryptionTest, AttachWithAWrongPasswordFails) {
	WriteImageFile();
	backend.diskIsLocked = true;

	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--unlock" }, "wrong"), cli::kExitFailure);

	EXPECT_TRUE(out.str().empty());
	EXPECT_EQ(backend.attached, 0);
}

TEST_F(CliEncryptionTest, AttachReadsARecoveryKeyFile) {
	WriteImageFile();
	backend.diskIsLocked = true;
	std::ofstream(KeyFile()) << "recovery-key\r\n";

	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--recovery-key-file", KeyFile() }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(backend.diskIsLocked);
	EXPECT_EQ(passwordReads, 0);
}

TEST_F(CliEncryptionTest, AttachIgnoresAByteOrderMarkInAKeyFile) {
	WriteImageFile();
	backend.diskIsLocked = true;
	std::ofstream(KeyFile()) << "\xEF\xBB\xBF" "recovery-key\r\n";

	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--recovery-key-file", KeyFile() }), cli::kExitSuccess) << err.str();

	EXPECT_FALSE(backend.diskIsLocked);
}

// An empty key would read as no key, so attaching a plain image would quietly succeed.
TEST_F(CliEncryptionTest, AKeyFileWithNoKeyIsAnError) {
	WriteImageFile();
	std::ofstream(KeyFile()) << "\nrecovery-key\n";

	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--recovery-key-file", KeyFile() }), cli::kExitFailure);

	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, APasswordGivenAsAnOptionIsNotRepeatedBack) {
	const std::vector<std::vector<std::string>> attempts = {
		{ "create", Image(), "--size", "8M", "--password=hunter2" },
		{ "create", Image(), "--size", "8M", "--password:hunter2" },
		{ "--password=hunter2", "create", Image(), "--size", "8M" },
		{ "--password:hunter2", "create", Image(), "--size", "8M" },
		{ "/password:hunter2", "create", Image(), "--size", "8M" } };

	for (const std::vector<std::string>& attempt : attempts) {
		EXPECT_EQ(RunWithPassword(attempt), cli::kExitUsage);
		EXPECT_EQ(err.str().find("hunter2"), std::string::npos) << err.str();
		EXPECT_NE(err.str().find("password"), std::string::npos) << err.str();
	}
}

// The key would vanish, and the disk couldn't be recovered.
TEST_F(CliEncryptionTest, ADeviceCannotHoldARecoveryKey) {
	EXPECT_EQ(RunWithPassword({ "create", Image(), "--size", "8M", "--encrypt", "--recovery-key-file", "NUL" }), cli::kExitFailure);

	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, AttachRefusesBothWaysOfUnlocking) {
	WriteImageFile();
	EXPECT_EQ(RunWithPassword({ "attach", Image(), "--unlock", "--recovery-key-file", KeyFile() }), cli::kExitUsage);
	EXPECT_TRUE(backend.calls.empty());
}

TEST_F(CliEncryptionTest, FormatErasesALockedImageWithoutAPassword) {
	WriteImageFile();
	backend.diskIsLocked = true;

	EXPECT_EQ(RunWithPassword({ "format", Image(), "--yes" }), cli::kExitSuccess) << err.str();

	EXPECT_EQ(passwordReads, 0);
	EXPECT_EQ(backend.calls.back(), "Detach");
}

TEST_F(CliEncryptionTest, FormatCanEncrypt) {
	WriteImageFile();

	EXPECT_EQ(RunWithPassword({ "format", Image(), "--yes", "--encrypt", "--recovery-key-file", KeyFile() }), cli::kExitSuccess) << err.str();

	EXPECT_EQ(backend.lastEncryptionPassword, "password");
	EXPECT_EQ(ReadKeyFile(), "recovery-key");
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
