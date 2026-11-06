#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace vdt {

/// <summary>
/// Failures with the same meaning on every platform. Errors reported by the operating
/// system use std::system_category instead, so they keep their native code and can still
/// be compared portably, e.g. error.code == std::errc::permission_denied.
/// </summary>
enum class Errc {
	InvalidArgument = 1, // A spec, option or path failed validation.
	Unsupported,         // This backend or OS cannot do what was asked.
	AlreadyExists,       // Refused to overwrite an existing image.
	NotAttached,         // The image is not attached.
	AlreadyAttached,     // Refused: the image is attached already.
	NotVirtualDisk,      // Refused: the target is not a file-backed virtual disk.
	NoVolume,            // The disk has no volume to mount.
	Timeout              // The OS did not bring the disk or volume online in time.
};

const std::error_category& ErrorCategory() noexcept;

inline std::error_code make_error_code(Errc code) noexcept {
	return { static_cast<int>(code), ErrorCategory() };
}

} // namespace vdt

template <>
struct std::is_error_code_enum<vdt::Errc> : std::true_type {};

namespace vdt {

/// <summary>
/// A failure, and the step that produced it.
/// </summary>
struct Error {
	std::error_code code;
	std::string operation; // What was being attempted, e.g. "AttachVirtualDisk".

	std::string Message() const { return operation + ": " + code.message(); }
};

/// <summary>
/// Either a value or an Error. The library reports failure this way and does not throw,
/// apart from std::bad_alloc when memory runs out. Reading the value of a failed Result,
/// or the error of a successful one, aborts the process: check the Result first.
/// </summary>
template <typename T>
class [[nodiscard]] Result {
public:
	Result(T value) : m_state(std::in_place_index<0>, std::move(value)) {}
	Result(Error error) : m_state(std::in_place_index<1>, std::move(error)) {}

	explicit operator bool() const noexcept { return m_state.index() == 0; }

	T& operator*() & noexcept { return Value(); }
	const T& operator*() const& noexcept { return Value(); }
	T&& operator*() && noexcept { return std::move(Value()); }
	T* operator->() noexcept { return &Value(); }
	const T* operator->() const noexcept { return &Value(); }

	const Error& GetError() const noexcept {
		const Error* error = std::get_if<1>(&m_state);
		if (!error) std::abort();
		return *error;
	}

private:
	T& Value() noexcept {
		T* value = std::get_if<0>(&m_state);
		if (!value) std::abort();
		return *value;
	}

	const T& Value() const noexcept {
		const T* value = std::get_if<0>(&m_state);
		if (!value) std::abort();
		return *value;
	}

	std::variant<T, Error> m_state;
};

template <>
class [[nodiscard]] Result<void> {
public:
	Result() = default;
	Result(Error error) : m_error(std::move(error)) {}

	explicit operator bool() const noexcept { return !m_error.has_value(); }

	const Error& GetError() const noexcept {
		if (!m_error) std::abort();
		return *m_error;
	}

private:
	std::optional<Error> m_error;
};

} // namespace vdt
