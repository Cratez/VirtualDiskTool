#pragma once

#include <string>

#include "vdt/Result.h"
#include "vdt/Types.h"

namespace vdt::win {

/// <summary>
/// Formats a volume, given its GUID path. Only WindowsAttachment calls this, on the volume
/// it just created on a disk it has confirmed is virtual.
/// </summary>
Result<void> FormatVolume(const std::wstring& volume, const VolumeSpec& spec);

} // namespace vdt::win
