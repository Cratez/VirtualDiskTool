#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "Cli.h"

namespace {

std::string ToUtf8(const wchar_t* text) {
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 1) {
		return {};
	}
	std::string utf8(static_cast<size_t>(size - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
	return utf8;
}

} // anonymous namespace

int wmain(int argc, wchar_t* argv[]) {
	std::vector<std::string> args;
	for (int index = 1; index < argc; ++index) {
		args.push_back(ToUtf8(argv[index]));
	}

	const UINT previousCodePage = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
	const int exitCode = vdt::cli::Run(args, std::cout, std::cerr, vdt::DefaultBackend());
	SetConsoleOutputCP(previousCodePage);
	return exitCode;
}
