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

inline Result<std::wstring> Utf8ToWide(const std::string& text) {
	if (text.empty()) {
		return std::wstring();
	}
	const int size = static_cast<int>(text.size());
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
	if (length == 0) {
		return LastError("Convert text from UTF-8");
	}
	std::wstring wide(static_cast<size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, wide.data(), length);
	return wide;
}

inline std::string WideToUtf8(const std::wstring& text) {
	if (text.empty()) {
		return {};
	}
	const int size = static_cast<int>(text.size());
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), size, utf8.data(), length, nullptr, nullptr);
	return utf8;
}

} // namespace vdt::win
