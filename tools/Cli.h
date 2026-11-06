#pragma once

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "vdt/Backend.h"

namespace vdt::cli {

/// <summary>
/// Gets a password from the user, or nothing if no usable one was given. confirm means this
/// is a new password, so an interactive user should enter it twice.
/// </summary>
using PasswordReader = std::function<std::optional<std::string>(bool confirm)>;

/// <summary>
/// Process exit codes. Scripts rely on these, so they must not change.
/// </summary>
enum ExitCode {
	kExitSuccess = 0,
	kExitFailure = 1, // The operation was attempted and failed.
	kExitUsage = 2    // The command line was wrong, so nothing was attempted.
};

/// <summary>
/// Removes UTF-8 byte order marks from the start of a line read from a pipe or a file. Windows
/// PowerShell can put them there, and more than one, without anyone having typed them.
/// </summary>
std::string StripByteOrderMarks(std::string line);

/// <summary>
/// Runs one command. args excludes the program name and is UTF-8. Results go to out, one per
/// line, so that scripts can capture them, and everything else goes to err.
/// </summary>
int Run(
	const std::vector<std::string>& args,
	std::ostream& out,
	std::ostream& err,
	IBackend& backend,
	const PasswordReader& readPassword = {});

} // namespace vdt::cli
