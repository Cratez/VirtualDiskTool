#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "vdt/Result.h"
#include "windows/Win32.h"

namespace vdt::win {

/// <summary>
/// Encrypts a freshly formatted volume, given its GUID path, and returns a new recovery key.
/// Without wholeVolume only data in use gets encrypted, which is only safe if the volume's
/// free space has never held data.
/// </summary>
Result<std::string> ProtectVolume(const std::wstring& volume, const std::string& password, bool wholeVolume);

/// <summary>
/// True if a volume is encrypted and locked. Doesn't need WMI for a readable volume, so
/// ordinary disks keep working where WMI is blocked, as it is in Windows Sandbox. For an
/// unreadable one it fails if BitLocker can't give an answer. It only says no without one on a
/// system with no BitLocker, or for a volume that reports being unformatted and that BitLocker
/// doesn't list.
/// </summary>
Result<bool> IsVolumeLocked(const std::wstring& volume);

Result<void> UnlockVolume(const std::wstring& volume, const std::string& password);
Result<void> UnlockVolumeWithRecoveryKey(const std::wstring& volume, const std::string& recoveryKey);

/// <summary>
/// Polls readStatus, which returns Win32_EncryptableVolume's ConversionStatus, until the volume
/// is fully encrypted. Fails with Errc::Timeout after timeoutMs. Fails sooner if the status
/// stays anything but "encrypting" for stalledMs, because then it has stopped and won't finish.
/// </summary>
Result<void> WaitForEncryption(const std::function<Result<uint32_t>()>& readStatus, ULONGLONG timeoutMs, ULONGLONG stalledMs, DWORD pollMs);

/// <summary>
/// The error for a failure code from a Win32_EncryptableVolume method.
/// </summary>
Error ReturnValueError(const std::string& method, uint32_t code);

} // namespace vdt::win
