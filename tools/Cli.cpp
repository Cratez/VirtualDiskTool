#include "Cli.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string_view>

#include "vdt/BuildInfo.h"
#include "vdt/VirtualDisk.h"

namespace fs = std::filesystem;

namespace vdt::cli {

namespace {

constexpr std::string_view kHelp =
	"Usage: VirtualDiskTool <command> [options]\n"
	"\n"
	"Commands name image files, never drive letters.\n"
	"\n"
	"  create <image> --size <size> [options]\n"
	"      Creates an image holding one formatted volume.\n"
	"      --size <size>       Size such as 64M or 1G (suffixes K, M, G, T)\n"
	"      --format <format>   vhd or vhdx (default: from the file extension)\n"
	"      --fixed             Allocate the full size up front\n"
	"      --fs <name>         File system, or none for a blank image\n"
	"      --partition <style> gpt or mbr (default: gpt)\n"
	"      --label <text>      Volume label\n"
	"      --full-format       Do a full format instead of a quick one\n"
	"      --attach            Leave the disk attached and print where it is mounted\n"
	"      --mount <path>      With --attach: mount here, such as Z:\\ or an empty folder\n"
	"      --encrypt           Protect the volume with a password, using BitLocker\n"
	"      --recovery-key-file <path>\n"
	"                          With --encrypt, and required: save the recovery key here\n"
	"\n"
	"  attach <image> [--read-only] [--mount <path>] [--unlock | --recovery-key-file <path>]\n"
	"      Attaches an image until detached, and prints where it is mounted. For an encrypted\n"
	"      image, --unlock asks for the password, and --recovery-key-file reads a saved key.\n"
	"\n"
	"  detach <image>\n"
	"      Detaches an attached image.\n"
	"\n"
	"  format <image> --yes [--fs <name>] [--partition <style>] [--label <text>] [--full-format]\n"
	"                 [--encrypt --recovery-key-file <path>]\n"
	"      Erases a detached image and writes a new volume. --yes confirms the erase. This\n"
	"      needs no password, even for an encrypted image.\n"
	"\n"
	"  list\n"
	"      Prints each attached virtual disk: image, device, mount path, separated by tabs.\n"
	"\n"
	"  version\n"
	"      Prints the version, Windows API level and backend of this build.\n"
	"\n"
	"Passwords are never command line arguments. They are read from the console, or from\n"
	"standard input if it is redirected.\n"
	"\n"
	"Exit codes: 0 success, 1 the operation failed, 2 the command line was wrong.\n";

struct OptionRules {
	std::set<std::string_view> withValue;
	std::set<std::string_view> flags;
};

struct ParsedArguments {
	std::vector<std::string> positional;
	std::map<std::string, std::string, std::less<>> values;
	std::set<std::string, std::less<>> flags;

	bool Has(std::string_view flag) const { return flags.contains(flag); }

	std::optional<std::string> Get(std::string_view option) const {
		const auto found = values.find(option);
		return found == values.end() ? std::nullopt : std::optional<std::string>(found->second);
	}
};

Error UsageError(std::string message) {
	return Error{ Errc::InvalidArgument, std::move(message) };
}

/// <summary>
/// An unrecognized option, minus any value attached to it. Repeating --password=something
/// would put that password in whatever log captures the error.
/// </summary>
std::string WithoutValue(const std::string& arg) {
	if (arg.empty() || (arg.front() != '-' && arg.front() != '/')) {
		return arg;
	}
	return arg.substr(0, arg.find_first_of("=:"));
}

Result<ParsedArguments> Parse(const std::vector<std::string>& args, const OptionRules& rules) {
	ParsedArguments parsed;
	for (size_t index = 1; index < args.size(); ++index) {
		const std::string& arg = args[index];
		if (!arg.starts_with("--")) {
			parsed.positional.push_back(arg);
		}
		else if (rules.flags.contains(arg)) {
			parsed.flags.insert(arg);
		}
		else if (!rules.withValue.contains(arg)) {
			return UsageError("Unknown option: " + WithoutValue(arg));
		}
		else if (index + 1 == args.size()) {
			return UsageError("Missing value for " + arg);
		}
		else {
			parsed.values[arg] = args[++index];
		}
	}
	return parsed;
}

std::string Lowercase(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

Result<uint64_t> ParseSize(const std::string& text) {
	uint64_t number = 0;
	const char* const end = text.data() + text.size();
	const auto [next, error] = std::from_chars(text.data(), end, number);
	if (error != std::errc() || next == text.data()) {
		return UsageError("Invalid size: " + text);
	}

	uint64_t multiplier = 1;
	const std::string suffix = Lowercase(std::string(next, end));
	if (suffix == "k") multiplier = 1ull << 10;
	else if (suffix == "m") multiplier = 1ull << 20;
	else if (suffix == "g") multiplier = 1ull << 30;
	else if (suffix == "t") multiplier = 1ull << 40;
	else if (!suffix.empty()) return UsageError("Invalid size: " + text);

	if (number > std::numeric_limits<uint64_t>::max() / multiplier) {
		return UsageError("Size is too large: " + text);
	}
	return number * multiplier;
}

Result<FileSystem> ParseFileSystem(const std::string& text) {
	static const std::map<std::string, FileSystem, std::less<>> names = {
		{ "default", FileSystem::Default }, { "none", FileSystem::None },
		{ "ntfs", FileSystem::Ntfs }, { "refs", FileSystem::ReFs },
		{ "exfat", FileSystem::ExFat }, { "fat32", FileSystem::Fat32 },
		{ "fat", FileSystem::Fat }, { "udf", FileSystem::Udf },
		{ "ext4", FileSystem::Ext4 }, { "xfs", FileSystem::Xfs },
		{ "btrfs", FileSystem::Btrfs }, { "apfs", FileSystem::Apfs },
		{ "hfsplus", FileSystem::HfsPlus } };

	const auto found = names.find(Lowercase(text));
	if (found == names.end()) {
		return UsageError("Unknown file system: " + text);
	}
	return found->second;
}

Result<ImageFormat> ParseFormat(const std::string& text) {
	const std::string name = Lowercase(text);
	if (name == "vhd") return ImageFormat::Vhd;
	if (name == "vhdx") return ImageFormat::Vhdx;
	if (name == "raw") return ImageFormat::Raw;
	return UsageError("Unknown image format: " + text);
}

Result<VolumeSpec> ParseVolume(const ParsedArguments& parsed) {
	VolumeSpec volume;
	if (const auto name = parsed.Get("--fs")) {
		Result<FileSystem> fileSystem = ParseFileSystem(*name);
		if (!fileSystem) {
			return fileSystem.GetError();
		}
		volume.fileSystem = *fileSystem;
	}
	if (const auto style = parsed.Get("--partition")) {
		const std::string name = Lowercase(*style);
		if (name != "gpt" && name != "mbr") {
			return UsageError("Unknown partition style: " + *style);
		}
		volume.partitionStyle = name == "gpt" ? PartitionStyle::Gpt : PartitionStyle::Mbr;
	}
	volume.label = parsed.Get("--label").value_or("");
	volume.quickFormat = !parsed.Has("--full-format");
	return volume;
}

fs::path PathFromUtf8(const std::string& text) {
	return fs::path(std::u8string(text.begin(), text.end()));
}

std::string Utf8FromPath(const fs::path& path) {
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
}

int ReportUsage(const Error& error, std::ostream& err) {
	err << error.operation << "\nRun 'VirtualDiskTool help' for usage.\n";
	return kExitUsage;
}

int ReportFailure(const Error& error, std::ostream& err) {
	err << "Error: " << error.Message() << '\n';
	return kExitFailure;
}

std::optional<fs::path> ImageArgument(const ParsedArguments& parsed) {
	if (parsed.positional.size() != 1) {
		return std::nullopt;
	}
	return PathFromUtf8(parsed.positional.front());
}

/// <summary>
/// Creates an empty file, failing if the name is taken. Returns an errno value, or zero. This
/// is one step so that two runs can't both find a name free and then share the file.
/// </summary>
int CreateNewFile(const fs::path& path) {
	std::FILE* file = nullptr;
#ifdef _WIN32
	const int error = _wfopen_s(&file, path.c_str(), L"wx");
#else
	file = std::fopen(path.c_str(), "wx");
	const int error = file ? 0 : errno;
#endif
	if (file) {
		std::fclose(file);
	}
	return error;
}

/// <summary>
/// Where a new recovery key goes. Reserving the file before anything is created means a bad
/// path fails early, instead of after encrypting a disk nobody could then recover. A file
/// that never received a key is removed.
/// </summary>
class RecoveryKeyFile {
public:
	explicit RecoveryKeyFile(fs::path path) : m_path(std::move(path)) {}

	~RecoveryKeyFile() {
		if (m_reserved && !m_saved) {
			std::error_code ignored;
			fs::remove(m_path, ignored);
		}
	}

	RecoveryKeyFile(const RecoveryKeyFile&) = delete;
	RecoveryKeyFile& operator=(const RecoveryKeyFile&) = delete;

	Result<void> Reserve() {
		if (const int created = CreateNewFile(m_path); created != 0) {
			return Error{ std::error_code(created, std::generic_category()), "Recovery key file" };
		}

		// A device name such as NUL can open without being a file, and would lose the key.
		std::error_code error;
		if (!fs::is_regular_file(m_path, error)) {
			return Error{ std::make_error_code(std::errc::invalid_argument), "Recovery key file" };
		}
		m_reserved = true;
		return {};
	}

	Result<void> Save(const std::string& key) {
		std::ofstream file(m_path, std::ios::trunc);
		file << key << '\n';
		file.close();
		if (!file) {
			return Error{ std::make_error_code(std::errc::io_error), "Save recovery key" };
		}
		m_saved = true;
		return {};
	}

private:
	fs::path m_path;
	bool m_reserved = false;
	bool m_saved = false;
};

Result<std::string> ReadRecoveryKey(const fs::path& path) {
	std::ifstream file(path);
	std::string key;
	if (!std::getline(file, key)) {
		return Error{ std::make_error_code(std::errc::no_such_file_or_directory), "Read recovery key file" };
	}
	key = StripByteOrderMarks(std::move(key));
	while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) {
		key.pop_back();
	}

	// An empty key would read as no key at all, and so skip the check that one was needed.
	if (key.empty()) {
		return Error{ std::make_error_code(std::errc::invalid_argument), "Recovery key file has no key on its first line" };
	}
	return key;
}

struct Encryption {
	std::string password;
	std::unique_ptr<RecoveryKeyFile> keyFile; // Empty unless encrypting.
};

/// <summary>
/// Handles --encrypt and --recovery-key-file for the commands that write a volume. Returns
/// an exit code if the command should stop here.
/// </summary>
std::optional<int> PrepareEncryption(
	const ParsedArguments& parsed,
	const PasswordReader& readPassword,
	std::ostream& err,
	Encryption& encryption) {

	const std::optional<std::string> keyPath = parsed.Get("--recovery-key-file");
	if (!parsed.Has("--encrypt")) {
		if (keyPath) {
			return ReportUsage(UsageError("--recovery-key-file needs --encrypt here"), err);
		}
		return std::nullopt;
	}
	if (!keyPath) {
		return ReportUsage(UsageError("--encrypt needs --recovery-key-file, because a lost password can't be recovered otherwise"), err);
	}
	if (!readPassword) {
		return ReportUsage(UsageError("No way to read a password"), err);
	}

	auto keyFile = std::make_unique<RecoveryKeyFile>(PathFromUtf8(*keyPath));
	if (Result<void> reserved = keyFile->Reserve(); !reserved) {
		return ReportFailure(reserved.GetError(), err);
	}
	const std::optional<std::string> password = readPassword(true);
	if (!password || password->empty()) {
		return ReportUsage(UsageError("No password was given"), err);
	}
	encryption.password = *password;
	encryption.keyFile = std::move(keyFile);
	return std::nullopt;
}

int RunCreate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend, const PasswordReader& readPassword) {
	Result<ParsedArguments> parsed = Parse(args, {
		{ "--size", "--format", "--fs", "--partition", "--label", "--mount", "--recovery-key-file" },
		{ "--fixed", "--full-format", "--attach", "--encrypt" } });
	if (!parsed) {
		return ReportUsage(parsed.GetError(), err);
	}
	const std::optional<fs::path> image = ImageArgument(*parsed);
	if (!image) {
		return ReportUsage(UsageError("create needs exactly one image path"), err);
	}

	const std::optional<std::string> sizeText = parsed->Get("--size");
	if (!sizeText) {
		return ReportUsage(UsageError("create needs --size"), err);
	}
	Result<uint64_t> size = ParseSize(*sizeText);
	if (!size) {
		return ReportUsage(size.GetError(), err);
	}

	DiskSpec spec;
	spec.size = *size;
	spec.allocation = parsed->Has("--fixed") ? Allocation::Fixed : Allocation::Dynamic;
	if (const auto formatText = parsed->Get("--format")) {
		Result<ImageFormat> format = ParseFormat(*formatText);
		if (!format) {
			return ReportUsage(format.GetError(), err);
		}
		spec.format = *format;
	}
	Result<VolumeSpec> volume = ParseVolume(*parsed);
	if (!volume) {
		return ReportUsage(volume.GetError(), err);
	}
	spec.volume = *volume;

	const bool attach = parsed->Has("--attach");
	const std::optional<std::string> mount = parsed->Get("--mount");
	if (mount && !attach) {
		return ReportUsage(UsageError("--mount needs --attach"), err);
	}
	if (parsed->Has("--encrypt") && spec.volume.fileSystem == FileSystem::None) {
		return ReportUsage(UsageError("--encrypt needs a file system"), err);
	}

	Encryption encryption;
	if (const std::optional<int> exitCode = PrepareEncryption(*parsed, readPassword, err, encryption)) {
		return *exitCode;
	}
	spec.volume.password = encryption.password;

	// A blank image needs nothing that only attaching can do, and so needs no admin rights.
	if (!attach && spec.volume.fileSystem == FileSystem::None) {
		if (Result<void> created = CreateBlankImage(*image, spec, backend); !created) {
			return ReportFailure(created.GetError(), err);
		}
		out << Utf8FromPath(*image) << '\n';
		return kExitSuccess;
	}

	AttachOptions options;
	options.lifetime = attach ? Lifetime::Persistent : Lifetime::Scoped;
	if (mount) {
		options.mountPath = PathFromUtf8(*mount);
	}

	Result<VirtualDisk> disk = VirtualDisk::Create(*image, spec, options, backend);
	if (!disk) {
		return ReportFailure(disk.GetError(), err);
	}
	if (encryption.keyFile) {
		if (Result<void> saved = encryption.keyFile->Save(disk->GetRecoveryKey()); !saved) {
			// An image nobody can recover is worse than no image, so this goes too.
			(void)disk->Detach();
			std::error_code ignored;
			fs::remove(*image, ignored);
			return ReportFailure(saved.GetError(), err);
		}
	}
	if (attach) {
		out << Utf8FromPath(disk->GetMountPath()) << '\n';
		if (Result<void> released = disk->Release(); !released) {
			return ReportFailure(released.GetError(), err);
		}
		return kExitSuccess;
	}
	if (Result<void> detached = disk->Detach(); !detached) {
		return ReportFailure(detached.GetError(), err);
	}
	out << Utf8FromPath(*image) << '\n';
	return kExitSuccess;
}

int RunAttach(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend, const PasswordReader& readPassword) {
	Result<ParsedArguments> parsed = Parse(args, { { "--mount", "--recovery-key-file" }, { "--read-only", "--unlock" } });
	if (!parsed) {
		return ReportUsage(parsed.GetError(), err);
	}
	const std::optional<fs::path> image = ImageArgument(*parsed);
	if (!image) {
		return ReportUsage(UsageError("attach needs exactly one image path"), err);
	}

	AttachOptions options;
	options.lifetime = Lifetime::Persistent;
	options.readOnly = parsed->Has("--read-only");
	if (const auto mount = parsed->Get("--mount")) {
		options.mountPath = PathFromUtf8(*mount);
	}

	const std::optional<std::string> keyPath = parsed->Get("--recovery-key-file");
	if (keyPath && parsed->Has("--unlock")) {
		return ReportUsage(UsageError("Use --unlock or --recovery-key-file, not both"), err);
	}
	if (parsed->Has("--unlock")) {
		const std::optional<std::string> password = readPassword ? readPassword(false) : std::nullopt;
		if (!password || password->empty()) {
			return ReportUsage(UsageError("No password was given"), err);
		}
		options.password = *password;
	}
	if (keyPath) {
		Result<std::string> key = ReadRecoveryKey(PathFromUtf8(*keyPath));
		if (!key) {
			return ReportFailure(key.GetError(), err);
		}
		options.recoveryKey = *key;
	}

	Result<VirtualDisk> disk = VirtualDisk::Open(*image, options, backend);
	if (!disk) {
		return ReportFailure(disk.GetError(), err);
	}
	out << Utf8FromPath(disk->GetMountPath()) << '\n';
	if (Result<void> released = disk->Release(); !released) {
		return ReportFailure(released.GetError(), err);
	}
	return kExitSuccess;
}

int RunDetach(const std::vector<std::string>& args, std::ostream& err, IBackend& backend) {
	Result<ParsedArguments> parsed = Parse(args, {});
	if (!parsed) {
		return ReportUsage(parsed.GetError(), err);
	}
	const std::optional<fs::path> image = ImageArgument(*parsed);
	if (!image) {
		return ReportUsage(UsageError("detach needs exactly one image path"), err);
	}
	if (Result<void> detached = DetachImage(*image, backend); !detached) {
		return ReportFailure(detached.GetError(), err);
	}
	return kExitSuccess;
}

int RunFormat(const std::vector<std::string>& args, std::ostream& err, IBackend& backend, const PasswordReader& readPassword) {
	Result<ParsedArguments> parsed = Parse(args, {
		{ "--fs", "--partition", "--label", "--recovery-key-file" },
		{ "--full-format", "--yes", "--encrypt" } });
	if (!parsed) {
		return ReportUsage(parsed.GetError(), err);
	}
	const std::optional<fs::path> image = ImageArgument(*parsed);
	if (!image) {
		return ReportUsage(UsageError("format needs exactly one image path"), err);
	}
	if (!parsed->Has("--yes")) {
		return ReportUsage(UsageError("format erases the image: pass --yes to confirm"), err);
	}
	Result<VolumeSpec> volume = ParseVolume(*parsed);
	if (!volume) {
		return ReportUsage(volume.GetError(), err);
	}

	if (parsed->Has("--encrypt") && volume->fileSystem == FileSystem::None) {
		return ReportUsage(UsageError("--encrypt needs a file system"), err);
	}

	Encryption encryption;
	if (const std::optional<int> exitCode = PrepareEncryption(*parsed, readPassword, err, encryption)) {
		return *exitCode;
	}
	volume->password = encryption.password;

	// Erasing needs no password, because anyone who can reach the file could delete it anyway.
	AttachOptions options;
	options.allowLocked = true;
	Result<VirtualDisk> disk = VirtualDisk::Open(*image, options, backend);
	if (!disk) {
		return ReportFailure(disk.GetError(), err);
	}
	if (Result<void> formatted = disk->Format(*volume); !formatted) {
		return ReportFailure(formatted.GetError(), err);
	}
	if (encryption.keyFile) {
		if (Result<void> saved = encryption.keyFile->Save(disk->GetRecoveryKey()); !saved) {
			return ReportFailure(saved.GetError(), err);
		}
	}
	if (Result<void> detached = disk->Detach(); !detached) {
		return ReportFailure(detached.GetError(), err);
	}
	return kExitSuccess;
}

int RunList(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend) {
	if (args.size() != 1) {
		return ReportUsage(UsageError("list takes no arguments"), err);
	}
	Result<std::vector<AttachedImage>> images = ListAttached(backend);
	if (!images) {
		return ReportFailure(images.GetError(), err);
	}
	for (const AttachedImage& image : *images) {
		out << Utf8FromPath(image.image) << '\t' << Utf8FromPath(image.device) << '\t' << Utf8FromPath(image.mountPath) << '\n';
	}
	return kExitSuccess;
}

} // anonymous namespace

std::string StripByteOrderMarks(std::string line) {
	constexpr std::string_view kMark = "\xEF\xBB\xBF";
	size_t start = 0;
	while (std::string_view(line).substr(start).starts_with(kMark)) {
		start += kMark.size();
	}
	line.erase(0, start);
	return line;
}

int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend, const PasswordReader& readPassword) {
	if (args.empty()) {
		err << kHelp;
		return kExitUsage;
	}

	const std::string& command = args.front();
	if (command == "create") return RunCreate(args, out, err, backend, readPassword);
	if (command == "attach") return RunAttach(args, out, err, backend, readPassword);
	if (command == "detach") return RunDetach(args, err, backend);
	if (command == "format") return RunFormat(args, err, backend, readPassword);
	if (command == "list") return RunList(args, out, err, backend);
	if (command == "version") {
		out << "VirtualDiskTool " VDT_VERSION " (" << GetWindowsApiLevel() << " API level, " << backend.GetName() << " backend)\n";
		return kExitSuccess;
	}
	if (command == "help" || command == "--help") {
		out << kHelp;
		return kExitSuccess;
	}
	return ReportUsage(UsageError("Unknown command: " + WithoutValue(command)), err);
}

} // namespace vdt::cli
