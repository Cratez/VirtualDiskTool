#pragma once

#include <windows.h>

#include <string>
#include <system_error>
#include <utility>

#include "vdt/Result.h"

namespace vdt::win {

/// <summary>
/// Owns a Win32 handle. Closing the handle of a Scoped attachment is what ends it, so this
/// is also what makes that cleanup survive early returns.
/// </summary>
class UniqueHandle {
public:
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE handle) noexcept : m_handle(handle) {}
	~UniqueHandle() { Reset(); }

	UniqueHandle(UniqueHandle&& other) noexcept
		: m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE)) {
	}

	UniqueHandle& operator=(UniqueHandle&& other) noexcept {
		if (this != &other) {
			Reset();
			m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
		}
		return *this;
	}

	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;

	HANDLE Get() const noexcept { return m_handle; }
	HANDLE* Put() noexcept { return &m_handle; }
	bool IsValid() const noexcept { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }

	void Reset() noexcept {
		if (IsValid()) {
			CloseHandle(m_handle);
		}
		m_handle = INVALID_HANDLE_VALUE;
	}

private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

inline Error Win32Error(std::string operation, DWORD code) {
	return Error{ std::error_code(static_cast<int>(code), std::system_category()), std::move(operation) };
}

/// <summary>
/// Takes a C string so that nothing can disturb GetLastError before it is read.
/// </summary>
inline Error LastError(const char* operation) {
	const DWORD code = GetLastError();
	return Win32Error(operation, code);
}

} // namespace vdt::win
