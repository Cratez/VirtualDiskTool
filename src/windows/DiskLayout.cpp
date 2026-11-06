#include "windows/DiskLayout.h"

#include <combaseapi.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string_view>

namespace vdt::win {

namespace {

constexpr std::wstring_view kPhysicalDrivePrefix = L"\\\\.\\PhysicalDrive";
constexpr DWORD kDeviceTimeoutMs = 30000;
constexpr DWORD kPollIntervalMs = 100;
constexpr LONGLONG kPartitionAlignment = 1024 * 1024;
constexpr DWORD kMaxGptPartitions = 128;
constexpr DWORD kMbrPartitions = 4;
constexpr int kMaxDisks = 64;
constexpr size_t kDependencyBufferSize = 4096;

// PARTITION_BASIC_DATA_GUID from diskguid.h, which would need initguid.h here.
constexpr GUID kBasicDataPartition = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

template <typename Predicate>
bool WaitUntil(Predicate ready) {
	const ULONGLONG deadline = GetTickCount64() + kDeviceTimeoutMs;
	for (;;) {
		if (ready()) return true;
		if (GetTickCount64() >= deadline) return false;
		Sleep(kPollIntervalMs);
	}
}

Result<void> Ioctl(HANDLE device, DWORD code, const char* operation, void* in, DWORD inSize, void* out, DWORD outSize) {
	DWORD returned = 0;
	if (!DeviceIoControl(device, code, in, inSize, out, outSize, &returned, nullptr)) {
		return LastError(operation);
	}
	return {};
}

UniqueHandle OpenForQuery(const std::wstring& path) {
	return UniqueHandle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
}

Result<bool> IsFileBacked(HANDLE disk) {
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	// Left zeroed if the driver returns less than expected, which reads as BusTypeUnknown
	// and so fails closed.
	alignas(STORAGE_DEVICE_DESCRIPTOR) BYTE buffer[1024] = {};
	if (Result<void> queried = Ioctl(disk, IOCTL_STORAGE_QUERY_PROPERTY, "Query disk bus type", &query, sizeof(query), buffer, sizeof(buffer)); !queried) {
		return queried.GetError();
	}
	return reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer)->BusType == BusTypeFileBackedVirtual;
}

/// <summary>
/// The only way this file gets a writable disk handle. The check runs on the same handle
/// the caller writes through, so the target cannot change in between.
/// </summary>
Result<UniqueHandle> OpenVirtualDiskForWriting(const std::wstring& physicalPath) {
	UniqueHandle disk(CreateFileW(physicalPath.c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
	if (!disk.IsValid()) {
		return LastError("Open disk for writing");
	}

	Result<bool> isVirtual = IsFileBacked(disk.Get());
	if (!isVirtual) {
		return isVirtual.GetError();
	}
	if (!*isVirtual) {
		return Error{ Errc::NotVirtualDisk, "Open disk for writing" };
	}
	return std::move(disk);
}

Result<GUID> NewGuid() {
	GUID guid = {};
	const HRESULT result = CoCreateGuid(&guid);
	if (FAILED(result)) {
		return Win32Error("CoCreateGuid", static_cast<DWORD>(result));
	}
	return guid;
}

BYTE MbrPartitionType(FileSystem fileSystem) noexcept {
	switch (fileSystem) {
	case FileSystem::Fat32: return PARTITION_FAT32_XINT13;
	case FileSystem::Fat:   return PARTITION_HUGE;
	default:                return PARTITION_IFS;
	}
}

constexpr DWORD LayoutSize(DWORD partitionCount) noexcept {
	return static_cast<DWORD>(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + sizeof(PARTITION_INFORMATION_EX) * partitionCount);
}

Result<void> ReadLayout(HANDLE disk, std::vector<BYTE>& buffer) {
	buffer.assign(LayoutSize(kMaxGptPartitions), 0);
	return Ioctl(disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, "Read partition table", nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()));
}

} // anonymous namespace

Result<DWORD> ParseDiskNumber(const std::wstring& physicalPath) {
	if (physicalPath.size() <= kPhysicalDrivePrefix.size()
		|| _wcsnicmp(physicalPath.c_str(), kPhysicalDrivePrefix.data(), kPhysicalDrivePrefix.size()) != 0) {
		return Error{ Errc::InvalidArgument, "Unexpected disk path" };
	}
	wchar_t* end = nullptr;
	const unsigned long number = std::wcstoul(physicalPath.c_str() + kPhysicalDrivePrefix.size(), &end, 10);
	if (*end != L'\0') {
		return Error{ Errc::InvalidArgument, "Unexpected disk path" };
	}
	return static_cast<DWORD>(number);
}

Result<bool> IsFileBackedVirtualDisk(const std::wstring& physicalPath) {
	UniqueHandle disk = OpenForQuery(physicalPath);
	if (!disk.IsValid()) {
		return LastError("Open disk");
	}
	return IsFileBacked(disk.Get());
}

Result<void> WaitForDevice(const std::wstring& physicalPath) {
	if (!WaitUntil([&] { return OpenForQuery(physicalPath).IsValid(); })) {
		return Error{ Errc::Timeout, "Wait for disk to appear" };
	}
	return {};
}

Result<void> WaitForDeviceGone(const std::wstring& physicalPath) {
	if (!WaitUntil([&] { return !OpenForQuery(physicalPath).IsValid(); })) {
		return Error{ Errc::Timeout, "Wait for disk to detach" };
	}
	return {};
}

Result<bool> HasPartition(const std::wstring& physicalPath) {
	UniqueHandle disk = OpenForQuery(physicalPath);
	if (!disk.IsValid()) {
		return LastError("Open disk");
	}

	std::vector<BYTE> buffer;
	if (Result<void> read = ReadLayout(disk.Get(), buffer); !read) {
		return read.GetError();
	}
	const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
	if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
		return layout->PartitionCount > 0;
	}
	if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
		return std::any_of(layout->PartitionEntry, layout->PartitionEntry + layout->PartitionCount,
			[](const PARTITION_INFORMATION_EX& entry) { return entry.Mbr.PartitionType != PARTITION_ENTRY_UNUSED; });
	}
	return false;
}

Result<void> ClearLayout(const std::wstring& physicalPath) {
	Result<UniqueHandle> disk = OpenVirtualDiskForWriting(physicalPath);
	if (!disk) {
		return disk.GetError();
	}
	if (Result<void> deleted = Ioctl(disk->Get(), IOCTL_DISK_DELETE_DRIVE_LAYOUT, "Erase partition table", nullptr, 0, nullptr, 0); !deleted) {
		return deleted;
	}
	return Ioctl(disk->Get(), IOCTL_DISK_UPDATE_PROPERTIES, "Refresh disk", nullptr, 0, nullptr, 0);
}

Result<void> WriteSinglePartition(const std::wstring& physicalPath, const VolumeSpec& volume) {
	Result<UniqueHandle> disk = OpenVirtualDiskForWriting(physicalPath);
	if (!disk) {
		return disk.GetError();
	}

	DISK_GEOMETRY_EX geometry = {};
	if (Result<void> read = Ioctl(disk->Get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, "Read disk geometry", nullptr, 0, &geometry, sizeof(geometry)); !read) {
		return read;
	}

	Result<GUID> diskId = NewGuid();
	if (!diskId) {
		return diskId.GetError();
	}

	// Only matters when reformatting. A blank disk has no layout to delete, and if this fails
	// on a real one then the next call will too, and report it.
	(void)Ioctl(disk->Get(), IOCTL_DISK_DELETE_DRIVE_LAYOUT, "Erase partition table", nullptr, 0, nullptr, 0);

	const bool gpt = volume.partitionStyle == PartitionStyle::Gpt;
	CREATE_DISK createDisk = {};
	if (gpt) {
		createDisk.PartitionStyle = PARTITION_STYLE_GPT;
		createDisk.Gpt.DiskId = *diskId;
		createDisk.Gpt.MaxPartitionCount = kMaxGptPartitions;
	}
	else {
		createDisk.PartitionStyle = PARTITION_STYLE_MBR;
		createDisk.Mbr.Signature = diskId->Data1 | 1; // Zero means no signature.
	}
	if (Result<void> created = Ioctl(disk->Get(), IOCTL_DISK_CREATE_DISK, "Create partition table", &createDisk, sizeof(createDisk), nullptr, 0); !created) {
		return created;
	}
	if (Result<void> refreshed = Ioctl(disk->Get(), IOCTL_DISK_UPDATE_PROPERTIES, "Refresh disk", nullptr, 0, nullptr, 0); !refreshed) {
		return refreshed;
	}

	// Reading the table back gives the usable area Windows actually reserved for GPT.
	std::vector<BYTE> buffer;
	if (Result<void> read = ReadLayout(disk->Get(), buffer); !read) {
		return read;
	}
	auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());

	const LONGLONG firstUsable = gpt ? layout->Gpt.StartingUsableOffset.QuadPart : 0;
	const LONGLONG endOfUsable = gpt ? firstUsable + layout->Gpt.UsableLength.QuadPart : geometry.DiskSize.QuadPart;
	const LONGLONG start = std::max(kPartitionAlignment, (firstUsable + kPartitionAlignment - 1) / kPartitionAlignment * kPartitionAlignment);
	const LONGLONG length = (endOfUsable - start) / kPartitionAlignment * kPartitionAlignment;
	if (length <= 0) {
		return Error{ Errc::InvalidArgument, "Disk is too small for a partition" };
	}

	const DWORD entries = gpt ? 1 : kMbrPartitions;
	layout->PartitionCount = entries;
	for (DWORD index = 0; index < entries; ++index) {
		PARTITION_INFORMATION_EX& entry = layout->PartitionEntry[index];
		entry = {};
		entry.PartitionStyle = gpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
		entry.RewritePartition = TRUE; // On the unused MBR entries, this is what clears them.
	}

	PARTITION_INFORMATION_EX& partition = layout->PartitionEntry[0];
	partition.StartingOffset.QuadPart = start;
	partition.PartitionLength.QuadPart = length;
	partition.PartitionNumber = 1;
	if (gpt) {
		Result<GUID> partitionId = NewGuid();
		if (!partitionId) {
			return partitionId.GetError();
		}
		partition.Gpt.PartitionType = kBasicDataPartition;
		partition.Gpt.PartitionId = *partitionId;
	}
	else {
		partition.Mbr.PartitionType = MbrPartitionType(volume.fileSystem);
		partition.Mbr.RecognizedPartition = TRUE;
		partition.Mbr.HiddenSectors = static_cast<DWORD>(start / geometry.Geometry.BytesPerSector);
	}

	if (Result<void> written = Ioctl(disk->Get(), IOCTL_DISK_SET_DRIVE_LAYOUT_EX, "Write partition table", layout, LayoutSize(entries), nullptr, 0); !written) {
		return written;
	}
	return Ioctl(disk->Get(), IOCTL_DISK_UPDATE_PROPERTIES, "Refresh disk", nullptr, 0, nullptr, 0);
}

std::optional<std::wstring> FindVolume(DWORD diskNumber, const std::wstring& exclude) {
	wchar_t name[MAX_PATH];
	const HANDLE search = FindFirstVolumeW(name, MAX_PATH);
	if (search == INVALID_HANDLE_VALUE) {
		return std::nullopt;
	}

	std::optional<std::wstring> found;
	do {
		const std::wstring volume(name);
		if (volume == exclude) {
			continue;
		}

		// Opening a volume needs the path without its trailing backslash.
		UniqueHandle handle = OpenForQuery(volume.substr(0, volume.size() - 1));
		if (!handle.IsValid()) {
			continue;
		}

		STORAGE_DEVICE_NUMBER number = {};
		if (Ioctl(handle.Get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, "Query volume", nullptr, 0, &number, sizeof(number))
			// Device numbers are per type: CdRom0 is also number 0, and must never match a disk.
			&& number.DeviceType == FILE_DEVICE_DISK
			&& number.DeviceNumber == diskNumber) {
			found = volume;
			break;
		}
	} while (FindNextVolumeW(search, name, MAX_PATH));

	FindVolumeClose(search);
	return found;
}

Result<std::wstring> WaitForVolume(DWORD diskNumber, const std::wstring& exclude) {
	std::optional<std::wstring> volume;
	if (!WaitUntil([&] { return (volume = FindVolume(diskNumber, exclude)).has_value(); })) {
		return Error{ Errc::Timeout, "Wait for volume to appear" };
	}
	return *volume;
}

std::vector<std::wstring> GetMountPoints(const std::wstring& volume) {
	std::vector<wchar_t> names(MAX_PATH);
	DWORD needed = 0;
	while (!GetVolumePathNamesForVolumeNameW(volume.c_str(), names.data(), static_cast<DWORD>(names.size()), &needed)) {
		if (GetLastError() != ERROR_MORE_DATA) {
			return {};
		}
		names.resize(needed);
	}

	std::vector<std::wstring> mountPoints;
	for (const wchar_t* name = names.data(); *name != L'\0'; name += std::wcslen(name) + 1) {
		mountPoints.emplace_back(name);
	}
	return mountPoints;
}

Result<std::vector<std::wstring>> GetBackingFiles(const std::wstring& physicalPath) {
	UniqueHandle disk(CreateFileW(physicalPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
	if (!disk.IsValid()) {
		return LastError("Open disk to read its dependencies");
	}

	constexpr auto flags = static_cast<GET_STORAGE_DEPENDENCY_FLAG>(
		GET_STORAGE_DEPENDENCY_FLAG_HOST_VOLUMES | GET_STORAGE_DEPENDENCY_FLAG_DISK_HANDLE);

	std::vector<BYTE> buffer(kDependencyBufferSize);
	DWORD result = ERROR_SUCCESS;
	for (;;) {
		auto* info = reinterpret_cast<STORAGE_DEPENDENCY_INFO*>(buffer.data());
		info->Version = STORAGE_DEPENDENCY_INFO_VERSION_2;
		ULONG needed = 0;
		result = GetStorageDependencyInformation(disk.Get(), flags, static_cast<ULONG>(buffer.size()), info, &needed);
		if (result != ERROR_INSUFFICIENT_BUFFER || needed <= buffer.size()) {
			break;
		}
		buffer.resize(needed);
	}
	if (result != ERROR_SUCCESS) {
		return Win32Error("GetStorageDependencyInformation", result);
	}

	const auto* info = reinterpret_cast<const STORAGE_DEPENDENCY_INFO*>(buffer.data());
	std::vector<std::wstring> files;
	for (ULONG index = 0; index < info->NumberEntries; ++index) {
		const STORAGE_DEPENDENCY_INFO_TYPE_2& entry = info->Version2Entries[index];
		if (!entry.DependentVolumeRelativePath) {
			continue;
		}
		std::wstring_view relative = entry.DependentVolumeRelativePath;
		if (relative.starts_with(L'\\')) {
			relative.remove_prefix(1);
		}
		for (const PWSTR volume : { entry.HostVolumeName, entry.DependentVolumeName }) {
			if (!volume) {
				continue;
			}
			std::wstring file = volume;
			if (!file.ends_with(L'\\')) {
				file += L'\\';
			}
			files.push_back(file.append(relative));
		}
	}
	return files;
}

std::optional<std::wstring> FindDiskForImage(const std::wstring& image) {
	for (int index = 0; index < kMaxDisks; ++index) {
		const std::wstring disk = std::wstring(kPhysicalDrivePrefix) + std::to_wstring(index);
		Result<bool> isVirtual = IsFileBackedVirtualDisk(disk);
		if (!isVirtual || !*isVirtual) {
			continue;
		}

		Result<std::vector<std::wstring>> files = GetBackingFiles(disk);
		if (!files) {
			continue;
		}
		for (const std::wstring& file : *files) {
			std::error_code error;
			if (std::filesystem::equivalent(file, image, error) && !error) {
				return disk;
			}
		}
	}
	return std::nullopt;
}

void DismountVolume(const std::wstring& volume) {
	UniqueHandle handle(CreateFileW(volume.substr(0, volume.size() - 1).c_str(), GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
	if (!handle.IsValid()) {
		return;
	}
	(void)Ioctl(handle.Get(), FSCTL_LOCK_VOLUME, "Lock volume", nullptr, 0, nullptr, 0);
	(void)Ioctl(handle.Get(), FSCTL_DISMOUNT_VOLUME, "Dismount volume", nullptr, 0, nullptr, 0);
}

} // namespace vdt::win
