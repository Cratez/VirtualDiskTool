#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vdt {

constexpr uint64_t MiB(uint64_t count) noexcept { return count * 1024 * 1024; }
constexpr uint64_t GiB(uint64_t count) noexcept { return MiB(count) * 1024; }

/// <summary>
/// Image file format. A backend that cannot handle a value reports Errc::Unsupported.
/// </summary>
enum class ImageFormat {
	Auto, // The best format the platform supports.
	Vhd,
	Vhdx,
	Raw
};

/// <summary>
/// How the image file claims space on the host.
/// </summary>
enum class Allocation {
	Dynamic, // Grows as data is written.
	Fixed    // Takes its full size up front.
};

enum class PartitionStyle {
	Gpt,
	Mbr
};

/// <summary>
/// File system for a volume. A backend that cannot create a value reports
/// Errc::Unsupported and never substitutes another file system.
/// </summary>
enum class FileSystem {
	Default, // The platform's native file system.
	None,    // Leave the disk blank: no partition table, nothing mounted.
	Ntfs,
	ReFs,
	ExFat,
	Fat32,
	Fat,
	Udf,
	Ext4,
	Xfs,
	Btrfs,
	Apfs,
	HfsPlus
};

/// <summary>
/// The single volume a disk holds.
/// </summary>
struct VolumeSpec {
	FileSystem fileSystem = FileSystem::Default;
	PartitionStyle partitionStyle = PartitionStyle::Gpt;
	std::string label; // UTF-8. May be empty.
	bool quickFormat = true;

	// Encrypts the volume, straight after formatting it. UTF-8. Empty means no encryption.
	std::string password;
};

/// <summary>
/// What to create. Only size is required.
/// </summary>
struct DiskSpec {
	uint64_t size = 0; // Bytes.
	ImageFormat format = ImageFormat::Auto;
	Allocation allocation = Allocation::Dynamic;
	VolumeSpec volume;
};

/// <summary>
/// How long an attachment lasts.
/// </summary>
enum class Lifetime {
	Scoped,    // Ends with the owning object. Where the OS allows it, also when the process dies.
	Persistent // Survives the process until detached explicitly.
};

struct AttachOptions {
	Lifetime lifetime = Lifetime::Scoped;
	bool readOnly = false;

	// Where to mount the volume: "Z:\" or an empty folder on Windows, an empty directory
	// elsewhere. When unset, the backend chooses.
	std::optional<std::filesystem::path> mountPath;

	// Unlocks an encrypted image. Give one of these at most. UTF-8.
	std::string password;
	std::string recoveryKey;

	// Attaches an encrypted image without unlocking it, instead of failing with Errc::Locked.
	// Its files stay unreadable, but VirtualDisk::Format can still erase it.
	bool allowLocked = false;
};

/// <summary>
/// One image the OS currently has attached.
/// </summary>
struct AttachedImage {
	std::filesystem::path image;
	std::filesystem::path device;    // e.g. \\.\PhysicalDrive7 or /dev/loop3. Empty if it couldn't be found, which usually means no admin rights.
	std::filesystem::path mountPath; // Empty if not mounted, or if device is empty.
};

} // namespace vdt
