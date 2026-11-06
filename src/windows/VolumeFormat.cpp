#include "windows/VolumeFormat.h"

#include <mutex>
#include <optional>
#include <string>

#include "windows/Win32.h"

namespace vdt::win {

namespace {

// fmifs.dll has no public header. These values follow the ReactOS sources and Rufus, which
// both rely on them, and the integration tests check them against the real DLL.
constexpr DWORD kFixedMedia = 12;

// Something can briefly hold a brand new volume open, which makes FormatEx report it as in use.
// This has been seen after BitLocker's service started, but what holds it isn't known.
constexpr int kFormatAttempts = 20;
constexpr DWORD kFormatRetryDelayMs = 250;

enum FormatCommand : DWORD {
	kProgress = 0,
	kIncompatibleFileSystem = 3,
	kAccessDenied = 6,
	kVolumeInUse = 8,
	kDone = 11,
	kOutput = 14,
	kStructureProgress = 15
};

// The payload of kOutput.
struct TextOutput {
	DWORD lines;
	const char* text;
};

// The quick flag is a BOOL here, because a BOOLEAN only sets the lowest byte of its stack
// slot, and that would leave garbage for the DLL if it reads more.
using FormatCallback = BOOLEAN(WINAPI*)(DWORD command, DWORD subAction, PVOID info);
using FormatExFunction = VOID(WINAPI*)(PWCHAR root, DWORD media, PWCHAR fileSystem, PWCHAR label,
	BOOL quick, DWORD clusterSize, FormatCallback callback);

struct FormatOutcome {
	bool succeeded = false;
	std::optional<DWORD> problem;
	int progressReports = 0;
	std::string commands; // Everything but progress, in order.
	std::string output;   // What the format engine said.
};

// FormatEx passes no context to its callback, and nothing documents which thread calls it.
// Guarded by formatMutex, which FormatOnce holds for the whole call.
std::mutex formatMutex;
FormatOutcome* s_outcome = nullptr;

BOOLEAN WINAPI OnFormatProgress(DWORD command, DWORD, PVOID info) {
	if (command == kProgress || command == kStructureProgress) {
		++s_outcome->progressReports;
		return TRUE;
	}

	s_outcome->commands += ' ' + std::to_string(command);
	switch (command) {
	case kDone:
		// The payload says whether it worked: reaching this command alone proves nothing.
		s_outcome->succeeded = info != nullptr && *static_cast<const BOOLEAN*>(info) != FALSE;
		break;
	case kOutput:
		if (const auto* output = static_cast<const TextOutput*>(info); output != nullptr && output->text != nullptr) {
			s_outcome->output += output->text;
		}
		break;
	case kIncompatibleFileSystem:
	case kAccessDenied:
	case kVolumeInUse:
		s_outcome->problem = command;
		break;
	default:
		break;
	}
	return TRUE;
}

FormatExFunction LoadFormatEx() {
	// A full path, because a bare name would search the application's directory first.
	wchar_t system[MAX_PATH];
	const UINT length = GetSystemDirectoryW(system, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return nullptr;
	}
	const std::wstring path = std::wstring(system) + L"\\fmifs.dll";
	const HMODULE module = LoadLibraryW(path.c_str());
	return module ? reinterpret_cast<FormatExFunction>(GetProcAddress(module, "FormatEx")) : nullptr;
}

const wchar_t* FileSystemName(FileSystem fileSystem) noexcept {
	switch (fileSystem) {
	case FileSystem::Default:
	case FileSystem::Ntfs:  return L"NTFS";
	case FileSystem::ReFs:  return L"ReFS";
	case FileSystem::ExFat: return L"exFAT";
	case FileSystem::Fat32: return L"FAT32";
	case FileSystem::Fat:   return L"FAT";
	case FileSystem::Udf:   return L"UDF";
	default:                return nullptr;
	}
}

Error FormatFailure(const FormatOutcome& outcome) {
	if (outcome.problem == kIncompatibleFileSystem) {
		return Error{ Errc::Unsupported, "FormatEx" };
	}
	if (outcome.problem == kAccessDenied) {
		return Win32Error("FormatEx", ERROR_ACCESS_DENIED);
	}
	if (outcome.problem == kVolumeInUse) {
		return Win32Error("FormatEx", ERROR_BUSY);
	}

	// FormatEx has no error code, so what it did along the way is the only evidence there is.
	std::string operation = "FormatEx (" + std::to_string(outcome.progressReports) + " progress reports, commands:" + outcome.commands + ")";
	if (!outcome.output.empty()) {
		operation += " said \"" + outcome.output + "\"";
	}
	return Win32Error(std::move(operation), ERROR_GEN_FAILURE);
}

Result<void> FormatOnce(FormatExFunction formatEx, std::wstring root, std::wstring fileSystem, std::wstring label, bool quick) {
	const std::lock_guard<std::mutex> lock(formatMutex);
	FormatOutcome outcome;
	s_outcome = &outcome;
	formatEx(root.data(), kFixedMedia, fileSystem.data(), label.data(), quick ? TRUE : FALSE, 0, OnFormatProgress);
	s_outcome = nullptr;

	if (!outcome.succeeded) {
		return FormatFailure(outcome);
	}
	return {};
}

} // anonymous namespace

Result<void> RetryWhileInUse(const std::function<Result<void>()>& attempt, int attempts, DWORD delayMs) {
	const std::error_code inUse(ERROR_BUSY, std::system_category());
	for (int tries = 1;; ++tries) {
		Result<void> result = attempt();
		if (result || result.GetError().code != inUse || tries >= attempts) {
			return result;
		}
		Sleep(delayMs);
	}
}

Result<void> FormatVolume(const std::wstring& volume, const VolumeSpec& spec) {
	const wchar_t* name = FileSystemName(spec.fileSystem);
	if (!name) {
		return Error{ Errc::Unsupported, "File system" };
	}
	Result<std::wstring> label = Utf8ToWide(spec.label);
	if (!label) {
		return label.GetError();
	}

	static const FormatExFunction formatEx = LoadFormatEx();
	if (!formatEx) {
		return Win32Error("Load FormatEx", ERROR_PROC_NOT_FOUND);
	}

	// FormatEx rejects a volume GUID path that ends in a backslash.
	std::wstring root = volume;
	if (!root.empty() && root.back() == L'\\') {
		root.pop_back();
	}

	return RetryWhileInUse(
		[&] { return FormatOnce(formatEx, root, name, *label, spec.quickFormat); },
		kFormatAttempts,
		kFormatRetryDelayMs);
}

} // namespace vdt::win
