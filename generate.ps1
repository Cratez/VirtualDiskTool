<#
.SYNOPSIS
Generates the Visual Studio solution and project files for a CMake preset.

.EXAMPLE
./generate.ps1 vs2026
#>
param(
	[Parameter(Mandatory)]
	[ValidateSet('vs2022', 'vs2026')]
	[string]$Preset
)

$ErrorActionPreference = 'Stop'

# Each preset uses the CMake bundled with its own Visual Studio: cmake is usually not on
# PATH, and the CMake in VS 2022 can't generate VS 2026 projects.
$versionRange = @{ vs2022 = '[17.0,18.0)'; vs2026 = '[18.0,19.0)' }[$Preset]

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
	throw 'vswhere.exe not found. Is Visual Studio installed?'
}

# vswhere leaves out installations that are incomplete, such as one being updated.
$installPath = & $vswhere -version $versionRange -prerelease -latest -property installationPath
if (-not $installPath) {
	throw "No complete Visual Studio installation found for preset $Preset (version $versionRange)."
}

$cmake = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) {
	throw "CMake not found in $installPath. Install the 'C++ CMake tools for Windows' component."
}

# The presets find vcpkg through VCPKG_ROOT, which only a Developer Command Prompt sets.
$vcpkgRoot = Join-Path $installPath 'VC\vcpkg'
if (-not (Test-Path (Join-Path $vcpkgRoot 'vcpkg.exe'))) {
	throw "vcpkg not found in $installPath. Install the 'vcpkg package manager' component."
}
$env:VCPKG_ROOT = $vcpkgRoot

Push-Location $PSScriptRoot
try {
	& $cmake --preset $Preset
	if ($LASTEXITCODE -ne 0) {
		throw "CMake failed with exit code $LASTEXITCODE."
	}

	$solution = Get-ChildItem "build/$Preset" -Filter 'VirtualDiskTool.sln*' | Select-Object -First 1
	Write-Host "Solution: $($solution.FullName)"
}
finally {
	Pop-Location
}
