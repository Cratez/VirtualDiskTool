#pragma once

#include <functional>
#include <string>

#include "vdt/Result.h"
#include "vdt/Types.h"
#include "windows/Win32.h"

namespace vdt::win {

/// <summary>
/// Runs attempt until it stops failing with ERROR_BUSY, up to attempts times in total, waiting
/// delayMs between tries. Any other result ends it immediately.
/// </summary>
Result<void> RetryWhileInUse(const std::function<Result<void>()>& attempt, int attempts, DWORD delayMs);

/// <summary>
/// Formats a volume, given its GUID path. Only WindowsAttachment calls this, on the volume
/// it just created on a disk it has confirmed is virtual.
/// </summary>
Result<void> FormatVolume(const std::wstring& volume, const VolumeSpec& spec);

} // namespace vdt::win
