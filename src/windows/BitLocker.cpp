#include "windows/BitLocker.h"

#include "windows/Win32.h"

#include <mi.h>

#include <cstdio>
#include <optional>
#include <variant>
#include <vector>

namespace vdt::win {

namespace {

// This uses the MI API rather than COM because COM's route to the encrypted connection this
// namespace needs is process-wide security, which a library has no business setting. MI
// sessions use packet privacy by default. mi.dll is loaded at run time because it exports a
// single entry point, and because Windows 7 doesn't have it as standard.
constexpr const MI_Char* kNamespace = MI_T("root\\CIMV2\\Security\\MicrosoftVolumeEncryption");
constexpr const MI_Char* kClass = MI_T("Win32_EncryptableVolume");
constexpr const MI_Char* kProtectorName = MI_T("VirtualDiskTool");

constexpr MI_Uint32 kEncryptWholeVolume = 0;
constexpr MI_Uint32 kEncryptDataOnly = 1;
constexpr MI_Uint32 kFullyEncrypted = 1;
constexpr MI_Uint32 kEncryptionInProgress = 2;
constexpr MI_Uint32 kLocked = 1;

constexpr DWORD kPollIntervalMs = 100;
constexpr ULONGLONG kVolumeTimeoutMs = 10000;
constexpr ULONGLONG kDataOnlyTimeoutMs = 5ull * 60 * 1000;
constexpr ULONGLONG kWholeVolumeTimeoutMs = 12ull * 60 * 60 * 1000; // Depends on disk size.

// How long the status can say anything but "encrypting" before that counts as having stopped.
// Generous, because it can take a moment to change after Encrypt returns.
constexpr ULONGLONG kStalledTimeoutMs = 30000;

// Return values of the Win32_EncryptableVolume methods.
constexpr MI_Uint32 kDriveTooSmall = 0x80310011;
constexpr MI_Uint32 kWrongFileSystem = 0x80310013;
constexpr MI_Uint32 kNotSupported = 0x80310015;
constexpr MI_Uint32 kFailedAuthentication = 0x80310027;
constexpr MI_Uint32 kInvalidPasswordFormat = 0x80310035;
constexpr MI_Uint32 kNotAllowedInThisVersion = 0x80310053;
constexpr MI_Uint32 kVolumeTooSmall = 0x8031006F;
constexpr MI_Uint32 kInvalidPassphraseLength = 0x80310080;
constexpr MI_Uint32 kPassphraseTooSimple = 0x80310081;

using InitializeFunction = decltype(&MI_Application_InitializeV1);

struct Argument {
	const MI_Char* name;
	std::variant<const MI_Char*, MI_Uint32> value;
};

struct CallResult {
	MI_Uint32 returnValue = 0;
	MI_Uint32 number = 0;
	std::wstring text;
};

InitializeFunction LoadInitialize() {
	// A full path, because a bare name would search the application's directory first.
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return nullptr;
	}
	const std::wstring path = std::wstring(system) + L"\\mi.dll";
	const HMODULE module = LoadLibraryW(path.c_str());
	return module ? reinterpret_cast<InitializeFunction>(GetProcAddress(module, "MI_Application_InitializeV1")) : nullptr;
}

Error MiError(const std::string& operation, MI_Result result) {
	switch (result) {
	case MI_RESULT_ACCESS_DENIED:
		return Error{ std::make_error_code(std::errc::permission_denied), operation + " needs administrator rights" };
	case MI_RESULT_INVALID_NAMESPACE:
	case MI_RESULT_INVALID_CLASS:
	case MI_RESULT_NOT_SUPPORTED:
	case MI_RESULT_METHOD_NOT_FOUND:
	case MI_RESULT_METHOD_NOT_AVAILABLE:
		return Error{ Errc::Unsupported, operation };
	default:
		return Error{ std::make_error_code(std::errc::io_error), operation + " (MI result " + std::to_string(result) + ")" };
	}
}

bool SameVolume(const std::wstring& a, const std::wstring& b) {
	const auto trimmed = [](const std::wstring& path) {
		return path.ends_with(L'\\') ? path.substr(0, path.size() - 1) : path;
	};
	return _wcsicmp(trimmed(a).c_str(), trimmed(b).c_str()) == 0;
}

/// <summary>
/// A connection to BitLocker for one volume.
/// </summary>
class Connection {
public:
	Connection() = default;

	~Connection() {
		if (m_volume) {
			MI_Instance_Delete(m_volume);
		}
		if (m_session.ft) {
			MI_Session_Close(&m_session, nullptr, nullptr);
		}
		if (m_application.ft) {
			MI_Application_Close(&m_application);
		}
	}

	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	/// <summary>
	/// True once BitLocker has answered at all, which tells a system without it apart from one
	/// that has no entry for a volume.
	/// </summary>
	bool ReachedBitLocker() const noexcept { return m_reachedBitLocker; }

	Result<void> Open(const std::wstring& volume) {
		static const InitializeFunction initialize = LoadInitialize();
		if (!initialize) {
			return Error{ Errc::Unsupported, "Load mi.dll" };
		}

		MI_Result result = initialize(0, nullptr, nullptr, &m_application);
		if (result != MI_RESULT_OK) {
			return MiError("Start the MI API", result);
		}
		result = MI_Application_NewSession(&m_application, nullptr, nullptr, nullptr, nullptr, nullptr, &m_session);
		if (result != MI_RESULT_OK) {
			return MiError("Open an MI session", result);
		}

		// A new volume can take a moment to appear to BitLocker.
		const ULONGLONG deadline = GetTickCount64() + kVolumeTimeoutMs;
		for (;;) {
			if (Result<void> found = FindVolume(volume); !found) {
				return found;
			}
			if (m_volume) {
				return {};
			}
			if (GetTickCount64() >= deadline) {
				return Error{ Errc::Unsupported, "BitLocker has no entry for " + WideToUtf8(volume) + ", and listed " + std::to_string(m_lastCount) + " volumes (first: " + m_firstSeen + ")" };
			}
			Sleep(kPollIntervalMs);
		}
	}

	/// <summary>
	/// Calls a method and fails unless it returns zero. output names an out parameter to read.
	/// </summary>
	Result<CallResult> Call(const MI_Char* method, const std::vector<Argument>& arguments, const MI_Char* output = nullptr) {
		const std::string name = WideToUtf8(method);

		// A method with no arguments has to get no parameter set at all, because MI rejects
		// an empty one with MI_RESULT_INVALID_PARAMETER.
		MI_Instance* parameters = nullptr;
		MI_Result result = MI_RESULT_OK;
		if (!arguments.empty()) {
			result = MI_Application_NewInstance(&m_application, MI_T("__Parameters"), nullptr, &parameters);
		}
		if (result != MI_RESULT_OK) {
			return MiError(name, result);
		}
		for (const Argument& argument : arguments) {
			MI_Value value = {};
			MI_Type type = MI_UINT32;
			if (const MI_Char* const* text = std::get_if<const MI_Char*>(&argument.value)) {
				value.string = const_cast<MI_Char*>(*text);
				type = MI_STRING;
			}
			else {
				value.uint32 = std::get<MI_Uint32>(argument.value);
			}
			result = MI_Instance_AddElement(parameters, argument.name, &value, type, 0);
			if (result != MI_RESULT_OK) {
				MI_Instance_Delete(parameters);
				return MiError(name, result);
			}
		}

		MI_Operation operation = MI_OPERATION_NULL;
		MI_Session_Invoke(&m_session, 0, nullptr, kNamespace, nullptr, method, m_volume, parameters, nullptr, &operation);

		CallResult outcome;
		std::optional<Error> failure;
		MI_Boolean more = MI_TRUE;
		while (more) {
			const MI_Instance* instance = nullptr;
			MI_Result operationResult = MI_RESULT_OK;
			const MI_Char* message = nullptr;
			if (MI_Operation_GetInstance(&operation, &instance, &more, &operationResult, &message, nullptr) != MI_RESULT_OK) {
				failure = MiError(name, operationResult);
				break;
			}
			if (operationResult != MI_RESULT_OK) {
				failure = MiError(name, operationResult);
			}
			else if (instance) {
				ReadOutputs(*instance, output, outcome);
			}
		}
		MI_Operation_Close(&operation);
		if (parameters) {
			MI_Instance_Delete(parameters);
		}

		if (failure) {
			return *failure;
		}
		if (outcome.returnValue != 0) {
			return ReturnValueError(name, outcome.returnValue);
		}
		return outcome;
	}

private:
	static void ReadOutputs(const MI_Instance& instance, const MI_Char* output, CallResult& outcome) {
		MI_Value value = {};
		MI_Type type = MI_UINT32;
		if (MI_Instance_GetElement(&instance, MI_T("ReturnValue"), &value, &type, nullptr, nullptr) == MI_RESULT_OK) {
			outcome.returnValue = value.uint32;
		}
		if (!output || MI_Instance_GetElement(&instance, output, &value, &type, nullptr, nullptr) != MI_RESULT_OK) {
			return;
		}
		if (type == MI_STRING && value.string) {
			outcome.text = value.string;
		}
		else if (type == MI_UINT32) {
			outcome.number = value.uint32;
		}
	}

	/// <summary>
	/// Sets m_volume if BitLocker lists the volume. Not finding it isn't an error here.
	/// </summary>
	Result<void> FindVolume(const std::wstring& volume) {
		MI_Operation operation = MI_OPERATION_NULL;
		MI_Session_EnumerateInstances(&m_session, 0, nullptr, kNamespace, kClass, MI_FALSE, nullptr, &operation);

		std::optional<Error> failure;
		m_lastCount = 0;
		MI_Boolean more = MI_TRUE;
		while (more) {
			const MI_Instance* instance = nullptr;
			MI_Result result = MI_RESULT_OK;
			const MI_Char* message = nullptr;
			if (MI_Operation_GetInstance(&operation, &instance, &more, &result, &message, nullptr) != MI_RESULT_OK || result != MI_RESULT_OK) {
				failure = MiError("List BitLocker volumes", result);
				break;
			}
			if (!instance) {
				continue;
			}

			MI_Value value = {};
			MI_Type type = MI_STRING;
			if (MI_Instance_GetElement(instance, MI_T("DeviceID"), &value, &type, nullptr, nullptr) != MI_RESULT_OK || type != MI_STRING || !value.string) {
				continue;
			}
			if (m_lastCount++ == 0) {
				m_firstSeen = WideToUtf8(value.string);
			}
			if (!m_volume && SameVolume(value.string, volume)) {
				// The instance belongs to the operation, so it has to be copied to outlive it.
				if (const MI_Result cloned = MI_Instance_Clone(instance, &m_volume); cloned != MI_RESULT_OK) {
					failure = MiError("Copy the BitLocker volume", cloned);
				}
			}
		}
		MI_Operation_Close(&operation);

		if (failure) {
			return *failure;
		}
		m_reachedBitLocker = true;
		return {};
	}

	MI_Application m_application = MI_APPLICATION_NULL;
	MI_Session m_session = MI_SESSION_NULL;
	MI_Instance* m_volume = nullptr;
	bool m_reachedBitLocker = false;
	int m_lastCount = 0;
	std::string m_firstSeen;
};

/// <summary>
/// Clears a secret's wide copy on every way out. std::string copies are the caller's to manage.
/// </summary>
class WideSecret {
public:
	explicit WideSecret(std::wstring text) : m_text(std::move(text)) {}
	~WideSecret() { SecureZeroMemory(m_text.data(), m_text.size() * sizeof(wchar_t)); }

	WideSecret(const WideSecret&) = delete;
	WideSecret& operator=(const WideSecret&) = delete;

	const wchar_t* Get() const noexcept { return m_text.c_str(); }

private:
	std::wstring m_text;
};

} // anonymous namespace

Error ReturnValueError(const std::string& method, uint32_t code) {
	switch (code) {
	case kFailedAuthentication:
	case kInvalidPasswordFormat:
		return Error{ Errc::WrongPassword, method };
	case kDriveTooSmall:
	case kVolumeTooSmall:
		return Error{ Errc::TooSmallToEncrypt, method };
	case kWrongFileSystem:
	case kNotSupported:
	case kNotAllowedInThisVersion:
		return Error{ Errc::Unsupported, method };
	case kInvalidPassphraseLength:
	case kPassphraseTooSimple:
		return Error{ Errc::InvalidArgument, method + " (the password is too short or too simple)" };
	default: {
		char hex[16];
		std::snprintf(hex, sizeof(hex), "0x%08X", code);
		return Win32Error(method + " (" + hex + ")", code);
	}
	}
}

Result<void> WaitForEncryption(const std::function<Result<uint32_t>()>& readStatus, ULONGLONG timeoutMs, ULONGLONG stalledMs, DWORD pollMs) {
	const ULONGLONG start = GetTickCount64();
	ULONGLONG lastEncrypting = start;
	for (;;) {
		Result<uint32_t> status = readStatus();
		if (!status) {
			return status.GetError();
		}
		if (*status == kFullyEncrypted) {
			return {};
		}

		const ULONGLONG now = GetTickCount64();
		if (*status == kEncryptionInProgress) {
			lastEncrypting = now;
		}
		else if (now - lastEncrypting >= stalledMs) {
			return Error{ std::make_error_code(std::errc::io_error), "Encryption stopped (conversion status " + std::to_string(*status) + ")" };
		}
		if (now - start >= timeoutMs) {
			return Error{ Errc::Timeout, "Wait for encryption to finish" };
		}
		Sleep(pollMs);
	}
}

Result<std::string> ProtectVolume(const std::wstring& volume, const std::string& password, bool wholeVolume) {
	Result<std::wstring> wide = Utf8ToWide(password);
	if (!wide) {
		return wide.GetError();
	}
	const WideSecret passphrase(std::move(*wide));

	Connection connection;
	if (Result<void> opened = connection.Open(volume); !opened) {
		return opened.GetError();
	}

	Result<CallResult> protectedKey = connection.Call(MI_T("ProtectKeyWithPassphrase"), {
		{ MI_T("FriendlyName"), kProtectorName },
		{ MI_T("Passphrase"), passphrase.Get() } });
	if (!protectedKey) {
		return protectedKey.GetError();
	}

	Result<CallResult> recovery = connection.Call(MI_T("ProtectKeyWithNumericalPassword"), {
		{ MI_T("FriendlyName"), kProtectorName } }, MI_T("VolumeKeyProtectorID"));
	if (!recovery) {
		return recovery.GetError();
	}
	Result<CallResult> key = connection.Call(MI_T("GetKeyProtectorNumericalPassword"), {
		{ MI_T("VolumeKeyProtectorID"), recovery->text.c_str() } }, MI_T("NumericalPassword"));
	if (!key) {
		return key.GetError();
	}

	const MI_Uint32 flags = wholeVolume ? kEncryptWholeVolume : kEncryptDataOnly;
	Result<CallResult> encrypting = connection.Call(MI_T("Encrypt"), { { MI_T("EncryptionFlags"), flags } });
	if (!encrypting) {
		return encrypting.GetError();
	}

	const auto readStatus = [&]() -> Result<uint32_t> {
		Result<CallResult> status = connection.Call(MI_T("GetConversionStatus"), { { MI_T("PrecisionFactor"), MI_Uint32(0) } }, MI_T("ConversionStatus"));
		if (!status) {
			return status.GetError();
		}
		return status->number;
	};
	const ULONGLONG timeoutMs = wholeVolume ? kWholeVolumeTimeoutMs : kDataOnlyTimeoutMs;
	if (Result<void> finished = WaitForEncryption(readStatus, timeoutMs, kStalledTimeoutMs, kPollIntervalMs); !finished) {
		return finished.GetError();
	}
	return WideToUtf8(key->text);
}

Result<bool> IsVolumeLocked(const std::wstring& volume) {
	wchar_t fileSystem[MAX_PATH + 1];
	if (GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fileSystem, MAX_PATH + 1)) {
		return false;
	}
	const DWORD unreadable = GetLastError();

	// An unreadable volume might just be unformatted, so only BitLocker can say. A system
	// without it can't have locked anything. BitLocker doesn't list unformatted volumes, so it
	// takes both the volume saying it is one and BitLocker not listing it to settle that. Any
	// other failure means the answer isn't known, and calling that "not locked" would hand out
	// a volume nobody can read.
	Connection connection;
	if (Result<void> opened = connection.Open(volume); !opened) {
		const Error& error = opened.GetError();
		if (error.code == Errc::Unsupported && (!connection.ReachedBitLocker() || unreadable == ERROR_UNRECOGNIZED_VOLUME)) {
			return false;
		}
		return Error{ error.code, error.operation + " (reading the volume gave error " + std::to_string(unreadable) + ")" };
	}
	Result<CallResult> status = connection.Call(MI_T("GetLockStatus"), {}, MI_T("LockStatus"));
	if (!status) {
		return status.GetError();
	}
	return status->number == kLocked;
}

Result<void> UnlockVolume(const std::wstring& volume, const std::string& password) {
	Result<std::wstring> wide = Utf8ToWide(password);
	if (!wide) {
		return wide.GetError();
	}
	const WideSecret passphrase(std::move(*wide));

	Connection connection;
	if (Result<void> opened = connection.Open(volume); !opened) {
		return opened;
	}
	Result<CallResult> unlocked = connection.Call(MI_T("UnlockWithPassphrase"), { { MI_T("Passphrase"), passphrase.Get() } });
	if (!unlocked) {
		return unlocked.GetError();
	}
	return {};
}

Result<void> UnlockVolumeWithRecoveryKey(const std::wstring& volume, const std::string& recoveryKey) {
	Result<std::wstring> wide = Utf8ToWide(recoveryKey);
	if (!wide) {
		return wide.GetError();
	}
	const WideSecret key(std::move(*wide));

	Connection connection;
	if (Result<void> opened = connection.Open(volume); !opened) {
		return opened;
	}
	Result<CallResult> unlocked = connection.Call(MI_T("UnlockWithNumericalPassword"), { { MI_T("NumericalPassword"), key.Get() } });
	if (!unlocked) {
		return unlocked.GetError();
	}
	return {};
}

} // namespace vdt::win
