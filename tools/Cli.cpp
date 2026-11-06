#include "Cli.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
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
	"\n"
	"  attach <image> [--read-only] [--mount <path>]\n"
	"      Attaches an image until detached, and prints where it is mounted.\n"
	"\n"
	"  detach <image>\n"
	"      Detaches an attached image.\n"
	"\n"
	"  format <image> --yes [--fs <name>] [--partition <style>] [--label <text>] [--full-format]\n"
	"      Erases a detached image and writes a new volume. --yes confirms the erase.\n"
	"\n"
	"  list\n"
	"      Prints each attached virtual disk: image, device, mount path, separated by tabs.\n"
	"\n"
	"  version\n"
	"      Prints the version, Windows API level and backend of this build.\n"
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
			return UsageError("Unknown option: " + arg);
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

int RunCreate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend) {
	Result<ParsedArguments> parsed = Parse(args, {
		{ "--size", "--format", "--fs", "--partition", "--label", "--mount" },
		{ "--fixed", "--full-format", "--attach" } });
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

int RunAttach(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend) {
	Result<ParsedArguments> parsed = Parse(args, { { "--mount" }, { "--read-only" } });
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

int RunFormat(const std::vector<std::string>& args, std::ostream& err, IBackend& backend) {
	Result<ParsedArguments> parsed = Parse(args, {
		{ "--fs", "--partition", "--label" },
		{ "--full-format", "--yes" } });
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

	Result<VirtualDisk> disk = VirtualDisk::Open(*image, {}, backend);
	if (!disk) {
		return ReportFailure(disk.GetError(), err);
	}
	if (Result<void> formatted = disk->Format(*volume); !formatted) {
		return ReportFailure(formatted.GetError(), err);
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

int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend) {
	if (args.empty()) {
		err << kHelp;
		return kExitUsage;
	}

	const std::string& command = args.front();
	if (command == "create") return RunCreate(args, out, err, backend);
	if (command == "attach") return RunAttach(args, out, err, backend);
	if (command == "detach") return RunDetach(args, err, backend);
	if (command == "format") return RunFormat(args, err, backend);
	if (command == "list") return RunList(args, out, err, backend);
	if (command == "version") {
		out << "VirtualDiskTool " VDT_VERSION " (" << GetWindowsApiLevel() << " API level, " << backend.GetName() << " backend)\n";
		return kExitSuccess;
	}
	if (command == "help" || command == "--help") {
		out << kHelp;
		return kExitSuccess;
	}
	return ReportUsage(UsageError("Unknown command: " + command), err);
}

} // namespace vdt::cli
