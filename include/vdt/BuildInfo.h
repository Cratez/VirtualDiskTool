#pragma once

namespace vdt {

/// <summary>
/// Name of the Windows API level this build was compiled against.
/// Decided by the toolset selected at configure time. This is not a support claim.
/// </summary>
const char* GetWindowsApiLevel() noexcept;

} // namespace vdt
