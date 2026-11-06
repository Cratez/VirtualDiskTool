#include <windows.h>

#include <VersionHelpers.h>
#include <virtdisk.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vdt/Backend.h"
#include "windows/DiskLayout.h"
#include "windows/VolumeFormat.h"
#include "windows/Win32.h"

namespace fs = std::filesystem;

namespace vdt::win {

namespace {

constexpr ULONG kVhdSectorSize = 512;
constexpr size_t kPhysicalPathCapacity = 512;
constexpr std::wstring_view kLongPathPrefix = L"\\\\?\\";

Result<std::wstring> NativePath(const fs::path& path) {
	std::error_code error;
	const fs::path absolute = fs::absolute(path, error);
	if (error) {
		return Error{ error, "Resolve path" };
	}
	return absolute.native();
}

std::wstring WithTrailingBackslash(std::wstring path) {
	if (path.empty() || path.back() != L'\\') {
		path += L'\\';
	}
	return path;
}

bool IsVhd(const std::wstring& path) {
	const size_t dot = path.rfind(L'.');
	return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".vhd") == 0;
}

// A zeroed VIRTUAL_STORAGE_TYPE is the unknown type, which makes Windows go by the file
// extension. VirtualDisk has already checked that the extension matches the format.
Result<UniqueHandle> OpenWithVersion1(const std::wstring& image, VIRTUAL_DISK_ACCESS_MASK access, ULONG rwDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT) {
	VIRTUAL_STORAGE_TYPE storageType = {};
	OPEN_VIRTUAL_DISK_PARAMETERS parameters = {};
	parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
	parameters.Version1.RWDepth = rwDepth;

	UniqueHandle handle;
	const DWORD result = OpenVirtualDisk(&storageType, image.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, handle.Put());
	if (result != ERROR_SUCCESS) {
		return Win32Error("OpenVirtualDisk", result);
	}
	return std::move(handle);
}

Result<UniqueHandle> OpenForAttach(const std::wstring& image, bool readOnly) {
	if (IsVhd(image)) {
		// Version 1 is what Windows 7 understands, and it is enough for VHD.
		return OpenWithVersion1(image, static_cast<VIRTUAL_DISK_ACCESS_MASK>(
			(readOnly ? VIRTUAL_DISK_ACCESS_ATTACH_RO : VIRTUAL_DISK_ACCESS_ATTACH_RW) | VIRTUAL_DISK_ACCESS_GET_INFO));
	}

	VIRTUAL_STORAGE_TYPE storageType = {};
	OPEN_VIRTUAL_DISK_PARAMETERS parameters = {};
	parameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;
	parameters.Version2.ReadOnly = readOnly ? TRUE : FALSE;

	UniqueHandle handle;
	const DWORD result = OpenVirtualDisk(&storageType, image.c_str(), VIRTUAL_DISK_ACCESS_NONE, OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, handle.Put());
	if (result != ERROR_SUCCESS) {
		return Win32Error("OpenVirtualDisk", result);
	}
	return std::move(handle);
}

Result<std::wstring> GetPhysicalPath(HANDLE disk) {
	std::wstring path(kPhysicalPathCapacity, L'\0');
	ULONG size = static_cast<ULONG>(path.size() * sizeof(wchar_t));
	const DWORD result = GetVirtualDiskPhysicalPath(disk, &size, path.data());
	if (result != ERROR_SUCCESS) {
		return Win32Error("GetVirtualDiskPhysicalPath", result);
	}
	path.resize(std::wcslen(path.c_str()));
	return path;
}

/// <summary>
/// A handle that can detach an attached image. Only version 1 handles can.
/// </summary>
Result<UniqueHandle> OpenForDetach(const std::wstring& image) {
	// Windows refuses the default read/write open of an attached disk that has been written
	// to, so that falls back to a depth of 0, which is the documented read-only open.
	Result<UniqueHandle> handle = OpenWithVersion1(image, VIRTUAL_DISK_ACCESS_DETACH);
	if (handle) {
		return handle;
	}
	return OpenWithVersion1(image, VIRTUAL_DISK_ACCESS_DETACH, 0);
}

Result<void> DetachThrough(HANDLE handle) {
	const DWORD result = DetachVirtualDisk(handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
	if (result != ERROR_SUCCESS) {
		return Win32Error("DetachVirtualDisk", result);
	}
	return {};
}

class WindowsAttachment final : public IAttachment {
public:
	WindowsAttachment(UniqueHandle handle, std::wstring image, Lifetime lifetime)
		: m_handle(std::move(handle)), m_image(std::move(image)), m_lifetime(lifetime) {
	}

	~WindowsAttachment() override {
		if (m_attached) {
			(void)Detach();
		}
	}

	WindowsAttachment(const WindowsAttachment&) = delete;
	WindowsAttachment& operator=(const WindowsAttachment&) = delete;

	/// <summary>
	/// Finds the disk Windows created. Kept out of the constructor so that a failure here
	/// still reaches the destructor and undoes the attach.
	/// </summary>
	Result<void> Locate() {
		Result<std::wstring> physicalPath = GetPhysicalPath(m_handle.Get());
		if (!physicalPath) {
			return physicalPath.GetError();
		}
		Result<DWORD> diskNumber = ParseDiskNumber(*physicalPath);
		if (!diskNumber) {
			return diskNumber.GetError();
		}
		m_physicalPath = std::move(*physicalPath);
		m_diskNumber = *diskNumber;
		return WaitForDevice(m_physicalPath);
	}

	Result<bool> IsFileBackedVirtualDisk() override {
		return win::IsFileBackedVirtualDisk(m_physicalPath);
	}

	Result<void> Initialize(const VolumeSpec& volume) override {
		if (Result<void> unmounted = Unmount(); !unmounted) {
			return unmounted;
		}

		const std::wstring previous = FindVolume(m_diskNumber).value_or(L"");
		if (!previous.empty()) {
			DismountVolume(previous);
		}

		if (volume.fileSystem == FileSystem::None) {
			return ClearLayout(m_physicalPath);
		}
		if (Result<void> written = WriteSinglePartition(m_physicalPath, volume); !written) {
			return written;
		}

		// The old volume can linger briefly, and formatting it by mistake would be wasted work
		// at best, so wait for the one the new partition produces.
		Result<std::wstring> created = WaitForVolume(m_diskNumber, previous);
		if (!created) {
			return created.GetError();
		}
		if (Result<void> formatted = FormatVolume(*created, volume); !formatted) {
			return formatted;
		}

		// Windows can give a new volume a drive letter by itself, and a volume can only have
		// one. Clearing it leaves Mount in charge of where the volume ends up.
		for (const std::wstring& mountPoint : GetMountPoints(*created)) {
			DeleteVolumeMountPointW(mountPoint.c_str());
		}
		return {};
	}

	Result<fs::path> Mount(const std::optional<fs::path>& mountPath) override {
		Result<bool> hasPartition = HasPartition(m_physicalPath);
		if (!hasPartition) {
			return hasPartition.GetError();
		}
		if (!*hasPartition) {
			return Error{ Errc::NoVolume, "Mount" };
		}

		Result<std::wstring> volume = WaitForVolume(m_diskNumber);
		if (!volume) {
			return volume.GetError();
		}

		if (mountPath) {
			Result<std::wstring> target = NativePath(*mountPath);
			if (!target) {
				return target.GetError();
			}
			return MountAt(WithTrailingBackslash(std::move(*target)), *volume);
		}

		// A volume already mounted somewhere stays there, and isn't ours to remove later.
		if (const std::vector<std::wstring> existing = GetMountPoints(*volume); !existing.empty()) {
			return fs::path(existing.front());
		}

		for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
			if (GetLogicalDrives() & (1u << (letter - L'A'))) {
				continue;
			}
			const std::wstring root = { letter, L':', L'\\' };
			if (SetVolumeMountPointW(root.c_str(), volume->c_str())) {
				m_mountPoint = root;
				return fs::path(root);
			}
		}
		return Win32Error("Find a free drive letter", ERROR_NO_MORE_ITEMS);
	}

	Result<void> Unmount() override {
		if (m_mountPoint.empty()) {
			return {};
		}
		if (!DeleteVolumeMountPointW(m_mountPoint.c_str())) {
			return LastError("DeleteVolumeMountPoint");
		}
		m_mountPoint.clear();
		return {};
	}

	Result<void> Detach() override {
		if (!m_attached) {
			return Error{ Errc::NotAttached, "Detach" };
		}
		if (Result<void> unmounted = Unmount(); !unmounted) {
			return unmounted;
		}

		// A Persistent attachment outlives its handle, so closing it first is safe, and avoids
		// a second open of the image conflicting with this one.
		m_handle.Reset();
		if (m_lifetime == Lifetime::Persistent) {
			Result<UniqueHandle> detacher = OpenForDetach(m_image);
			if (!detacher) {
				return detacher.GetError();
			}
			if (Result<void> detached = DetachThrough(detacher->Get()); !detached) {
				return detached;
			}
			m_attached = false;
			return {};
		}

		// Closing the last handle is what ends a Scoped attachment. Waiting here means the
		// image file can be deleted as soon as this returns.
		m_attached = false;
		return m_physicalPath.empty() ? Result<void>() : WaitForDeviceGone(m_physicalPath);
	}

	void Release() noexcept override {
		m_handle.Reset();
		m_attached = false;
	}

private:
	Result<fs::path> MountAt(std::wstring target, const std::wstring& volume) {
		if (!SetVolumeMountPointW(target.c_str(), volume.c_str())) {
			return LastError("SetVolumeMountPoint");
		}
		m_mountPoint = target;
		return fs::path(std::move(target));
	}

	UniqueHandle m_handle;
	std::wstring m_image;
	std::wstring m_physicalPath;
	std::wstring m_mountPoint; // Only set for a mount this object made.
	DWORD m_diskNumber = 0;
	Lifetime m_lifetime;
	bool m_attached = true;
};

class WindowsBackend final : public IBackend {
public:
	const char* GetName() const noexcept override {
		return "Windows";
	}

	ImageFormat GetDefaultFormat() const noexcept override {
		return Supports(ImageFormat::Vhdx) ? ImageFormat::Vhdx : ImageFormat::Vhd;
	}

	bool Supports(ImageFormat format) const noexcept override {
		switch (format) {
		case ImageFormat::Auto:
		case ImageFormat::Vhd:  return true;
		case ImageFormat::Vhdx: return IsWindows8OrGreater();
		default:                return false;
		}
	}

	bool Supports(FileSystem fileSystem) const noexcept override {
		switch (fileSystem) {
		case FileSystem::Default:
		case FileSystem::None:
		case FileSystem::Ntfs:
		case FileSystem::ReFs:
		case FileSystem::ExFat:
		case FileSystem::Fat32:
		case FileSystem::Fat:
		case FileSystem::Udf:
			return true;
		default:
			return false;
		}
	}

	bool CleansUpAfterCrash() const noexcept override {
		return true;
	}

	Result<void> CreateImage(const fs::path& image, const DiskSpec& spec) override {
		Result<std::wstring> path = NativePath(image);
		if (!path) {
			return path.GetError();
		}

		CREATE_VIRTUAL_DISK_PARAMETERS parameters = {};
		VIRTUAL_DISK_ACCESS_MASK access = VIRTUAL_DISK_ACCESS_NONE; // The only mask version 2 accepts.
		if (spec.format == ImageFormat::Vhd) {
			// Version 1 is what Windows 7 understands, and it is enough for VHD.
			parameters.Version = CREATE_VIRTUAL_DISK_VERSION_1;
			parameters.Version1.MaximumSize = spec.size;
			parameters.Version1.SectorSizeInBytes = kVhdSectorSize;
			access = VIRTUAL_DISK_ACCESS_CREATE;
		}
		else {
			parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
			parameters.Version2.MaximumSize = spec.size;
		}

		const CREATE_VIRTUAL_DISK_FLAG flags = spec.allocation == Allocation::Fixed
			? CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION
			: CREATE_VIRTUAL_DISK_FLAG_NONE;

		VIRTUAL_STORAGE_TYPE storageType = {};
		UniqueHandle handle;
		const DWORD result = CreateVirtualDisk(&storageType, path->c_str(), access, nullptr, flags, 0, &parameters, nullptr, handle.Put());
		if (result == ERROR_FILE_EXISTS) {
			return Error{ Errc::AlreadyExists, "CreateVirtualDisk" };
		}
		if (result != ERROR_SUCCESS) {
			return Win32Error("CreateVirtualDisk", result);
		}
		return {};
	}

	Result<std::unique_ptr<IAttachment>> Attach(const fs::path& image, const AttachOptions& options) override {
		Result<std::wstring> path = NativePath(image);
		if (!path) {
			return path.GetError();
		}
		Result<UniqueHandle> handle = OpenForAttach(*path, options.readOnly);
		if (!handle) {
			return handle.GetError();
		}

		// Drive letters are left to Mount, so that only one thing ever assigns them.
		ATTACH_VIRTUAL_DISK_FLAG flags = ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER;
		if (options.lifetime == Lifetime::Persistent) {
			flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(flags | ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME);
		}
		if (options.readOnly) {
			flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
		}

		ATTACH_VIRTUAL_DISK_PARAMETERS parameters = {};
		parameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
		const DWORD result = AttachVirtualDisk(handle->Get(), nullptr, flags, 0, &parameters, nullptr);
		if (result == ERROR_PRIVILEGE_NOT_HELD) {
			// The most likely failure of all, and one the standard library doesn't map to
			// permission_denied, so a portable comparison would otherwise miss it.
			return Error{ std::make_error_code(std::errc::permission_denied), "AttachVirtualDisk needs administrator rights" };
		}
		if (result != ERROR_SUCCESS) {
			return Win32Error("AttachVirtualDisk", result);
		}

		auto attachment = std::make_unique<WindowsAttachment>(std::move(*handle), std::move(*path), options.lifetime);
		if (Result<void> located = attachment->Locate(); !located) {
			return located.GetError();
		}
		return std::unique_ptr<IAttachment>(std::move(attachment));
	}

	Result<void> DetachImage(const fs::path& image) override {
		Result<std::wstring> path = NativePath(image);
		if (!path) {
			return path.GetError();
		}

		Result<std::vector<std::wstring>> attached = GetAttachedImages();
		if (!attached) {
			return attached.GetError();
		}
		const bool isAttached = std::any_of(attached->begin(), attached->end(), [&](const std::wstring& entry) {
			std::error_code error;
			return fs::equivalent(entry, *path, error) && !error;
		});
		if (!isAttached) {
			return Error{ Errc::NotAttached, "DetachImage" };
		}

		// Opened before touching the mounts, so that failing here changes nothing.
		Result<UniqueHandle> handle = OpenForDetach(*path);
		if (!handle) {
			return handle.GetError();
		}

		// A mount left behind would come back the next time this image is attached.
		if (const std::optional<std::wstring> device = FindDiskForImage(*path)) {
			if (Result<DWORD> diskNumber = ParseDiskNumber(*device); diskNumber) {
				if (const std::optional<std::wstring> volume = FindVolume(*diskNumber)) {
					for (const std::wstring& mountPoint : GetMountPoints(*volume)) {
						DeleteVolumeMountPointW(mountPoint.c_str());
					}
				}
			}
		}
		return DetachThrough(handle->Get());
	}

	Result<std::vector<AttachedImage>> ListAttached() override {
		Result<std::vector<std::wstring>> paths = GetAttachedImages();
		if (!paths) {
			return paths.GetError();
		}

		std::vector<AttachedImage> images;
		for (const std::wstring& path : *paths) {
			images.push_back(Describe(path));
		}
		return images;
	}

private:
	static Result<std::vector<std::wstring>> GetAttachedImages() {
		std::vector<wchar_t> buffer(4096);
		for (;;) {
			ULONG size = static_cast<ULONG>(buffer.size() * sizeof(wchar_t));
			const DWORD result = GetAllAttachedVirtualDiskPhysicalPaths(&size, buffer.data());
			if (result == ERROR_SUCCESS) {
				break;
			}
			if (result != ERROR_INSUFFICIENT_BUFFER) {
				return Win32Error("GetAllAttachedVirtualDiskPhysicalPaths", result);
			}
			buffer.resize(size / sizeof(wchar_t) + 1);
		}

		std::vector<std::wstring> paths;
		for (const wchar_t* entry = buffer.data(); *entry != L'\0'; entry += std::wcslen(entry) + 1) {
			std::wstring path = entry;
			if (path.starts_with(kLongPathPrefix)) {
				path.erase(0, kLongPathPrefix.size());
			}
			paths.push_back(std::move(path));
		}
		return paths;
	}

	static AttachedImage Describe(const std::wstring& path) {
		AttachedImage image;
		image.image = path;

		// The rest is detail, and a failure here shouldn't hide that the image is attached.
		const std::optional<std::wstring> device = FindDiskForImage(path);
		if (!device) {
			return image;
		}
		image.device = *device;

		if (Result<DWORD> diskNumber = ParseDiskNumber(*device); diskNumber) {
			if (const std::optional<std::wstring> volume = FindVolume(*diskNumber)) {
				const std::vector<std::wstring> mountPoints = GetMountPoints(*volume);
				if (!mountPoints.empty()) {
					image.mountPath = mountPoints.front();
				}
			}
		}
		return image;
	}
};

} // anonymous namespace

} // namespace vdt::win

namespace vdt {

IBackend& DefaultBackend() {
	static win::WindowsBackend backend;
	return backend;
}

} // namespace vdt
