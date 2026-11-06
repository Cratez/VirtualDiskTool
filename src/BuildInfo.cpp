#include "vdt/BuildInfo.h"

namespace vdt {

const char* GetWindowsApiLevel() noexcept {
#if _WIN32_WINNT >= 0x0A00
	return "Windows 10";
#else
	return "Windows 7";
#endif
}

} // namespace vdt
