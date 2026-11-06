#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "vdt/Backend.h"

namespace vdt::cli {

/// <summary>
/// Process exit codes. Scripts rely on these, so they must not change.
/// </summary>
enum ExitCode {
	kExitSuccess = 0,
	kExitFailure = 1, // The operation was attempted and failed.
	kExitUsage = 2    // The command line was wrong, so nothing was attempted.
};

/// <summary>
/// Runs one command. args excludes the program name and is UTF-8. Results go to out, one per
/// line, so that scripts can capture them, and everything else goes to err.
/// </summary>
int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, IBackend& backend);

} // namespace vdt::cli
