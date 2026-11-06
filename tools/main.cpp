#include <windows.h>

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Cli.h"

namespace {

constexpr DWORD kConsoleReadSize = 256;

std::string ToUtf8(const wchar_t* text) {
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1) {
		return {};
	}
	std::string utf8(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
	return utf8;
}

/// <summary>
/// Reads a line from the console without showing what is typed.
/// </summary>
std::optional<std::string> ReadHiddenLine(HANDLE input, DWORD mode, const char* prompt) {
	std::cerr << prompt << std::flush;
	SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);

	std::wstring line;
	bool complete = false;
	while (!complete) {
		wchar_t buffer[kConsoleReadSize];
		DWORD read = 0;
		if (!ReadConsoleW(input, buffer, kConsoleReadSize, &read, nullptr) || read == 0) {
			break;
		}
		line.append(buffer, read);
		SecureZeroMemory(buffer, sizeof(buffer));
		complete = line.find(L'\n') != std::wstring::npos;
	}

	SetConsoleMode(input, mode);
	std::cerr << '\n';
	if (!complete) {
		return std::nullopt;
	}

	const size_t end = line.find_first_of(L"\r\n");
	const std::string password = ToUtf8(line.substr(0, end).c_str());
	SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
	return password;
}

std::optional<std::string> ReadPassword(bool confirm) {
	const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	DWORD mode = 0;
	if (!GetConsoleMode(input, &mode)) {
		// Redirected, so this is a script: one line, and nothing to confirm.
		std::string line;
		if (!std::getline(std::cin, line)) {
			return std::nullopt;
		}
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		return vdt::cli::StripByteOrderMarks(std::move(line));
	}

	const std::optional<std::string> password = ReadHiddenLine(input, mode, "Password: ");
	if (!password || !confirm) {
		return password;
	}
	if (ReadHiddenLine(input, mode, "Confirm password: ") != password) {
		std::cerr << "The passwords don't match.\n";
		return std::nullopt;
	}
	return password;
}

} // anonymous namespace

int wmain(int argc, wchar_t* argv[]) {
	std::vector<std::string> args;
	for (int index = 1; index < argc; ++index) {
		args.push_back(ToUtf8(argv[index]));
	}

	const UINT previousCodePage = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
	const int exitCode = vdt::cli::Run(args, std::cout, std::cerr, vdt::DefaultBackend(), ReadPassword);
	SetConsoleOutputCP(previousCodePage);
	return exitCode;
}
