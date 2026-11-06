#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vdt/Result.h"
#include "vdt/Types.h"
#include "windows/Win32.h"

namespace vdt::win {

/// <summary>
/// The number in \\.\PhysicalDriveN.
/// </summary>
Result<DWORD> ParseDiskNumber(const std::wstring& physicalPath);

/// <summary>
/// Asks the disk driver whether a device is backed by an image file. Needs no special rights.
/// </summary>
Result<bool> IsFileBackedVirtualDisk(const std::wstring& physicalPath);

Result<void> WaitForDevice(const std::wstring& physicalPath);
Result<void> WaitForDeviceGone(const std::wstring& physicalPath);

/// <summary>
/// True if the disk has at least one partition.
/// </summary>
Result<bool> HasPartition(const std::wstring& physicalPath);

/// <summary>
/// Erases the partition table. Fails with Errc::NotVirtualDisk unless the handle it writes
/// through reports a file-backed virtual disk.
/// </summary>
Result<void> ClearLayout(const std::wstring& physicalPath);

/// <summary>
/// Replaces the partition table with one that holds a single partition filling the disk.
/// Fails with Errc::NotVirtualDisk unless the handle it writes through reports a
/// file-backed virtual disk. eraseOutsideVolume also zeroes the parts of the disk the new
/// partition won't cover.
/// </summary>
Result<void> WriteSinglePartition(const std::wstring& physicalPath, const VolumeSpec& volume, bool eraseOutsideVolume);

/// <summary>
/// The GUID path of a volume on the given disk, ending in a backslash. Skips the volume
/// named by exclude, which lets a caller wait for a replacement to appear.
/// </summary>
std::optional<std::wstring> FindVolume(DWORD diskNumber, const std::wstring& exclude = {});
Result<std::wstring> WaitForVolume(DWORD diskNumber, const std::wstring& exclude = {});

/// <summary>
/// Every drive letter and folder a volume is mounted at.
/// </summary>
std::vector<std::wstring> GetMountPoints(const std::wstring& volume);

/// <summary>
/// Candidate paths for the image behind a virtual disk, as \\?\Volume{...}\ paths. Windows
/// doesn't document which of two volume names the path is relative to, so this returns both
/// and some candidates may not exist. Needs admin rights to open the disk.
/// </summary>
Result<std::vector<std::wstring>> GetBackingFiles(const std::wstring& physicalPath);

/// <summary>
/// The disk an attached image is surfaced as. This works from the disks towards the image,
/// because Windows can refuse to open an image that is attached and has been written to.
/// </summary>
std::optional<std::wstring> FindDiskForImage(const std::wstring& image);

/// <summary>
/// Asks the file system to let go of a volume, ahead of replacing it. Best effort.
/// </summary>
void DismountVolume(const std::wstring& volume);

} // namespace vdt::win
