<#
.SYNOPSIS
Runs the test suite and writes one report file holding everything needed to review the run.

.DESCRIPTION
Copy this script and VirtualDiskTests.exe into the same folder on the test machine, then run
it from an elevated PowerShell prompt. Without admin rights the integration tests are
reported as skipped. Nothing needs installing.

Exits with 0 only if every test passed, nothing but ReFS was skipped, and the machine's
disks and temporary folder were left as they were found. ReFS is allowed because many
Windows editions can't create it.

Where Windows won't show the disks, as in Windows Sandbox, the tests still run. The result
is then INCOMPLETE rather than PASS, because the machine couldn't be checked.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\Run-Tests.ps1
#>
param(
	[string]$ReportPath = (Join-Path $PSScriptRoot 'VirtualDiskTests-report.txt')
)

$ErrorActionPreference = 'Stop'

$machineProblems = @()

# Snapshots are evidence about the run, not the run itself, so failing to take one must not
# stop the tests. An empty snapshot counts as a failure: it would make any comparison pass.
function Get-Snapshot([string]$Name, [scriptblock]$Collect) {
	try {
		$snapshot = @(& $Collect)
		if ($snapshot.Count -eq 0) {
			$script:machineProblems += "${Name}: Windows reported none."
		}
		return $snapshot
	}
	catch {
		$script:machineProblems += "${Name}: $($_.Exception.Message)"
	}
}

function Get-DiskSnapshot {
	Get-Snapshot 'Disks' {
		Get-Disk -ErrorAction Stop | Sort-Object Number |
			ForEach-Object { '{0} | {1} | {2} | {3} | {4}' -f $_.Number, $_.FriendlyName, $_.BusType, $_.PartitionStyle, $_.Size }
	}
}

function Get-VolumeSnapshot {
	Get-Snapshot 'Volumes' {
		Get-Volume -ErrorAction Stop | Sort-Object Path |
			ForEach-Object { '{0} | {1} | {2} | {3} | {4}' -f $_.Path, $_.DriveLetter, $_.FileSystemLabel, $_.FileSystem, $_.Size }
	}
}

function Get-Leftovers {
	Get-ChildItem $env:TEMP -Directory -Filter 'vdt-*' -ErrorAction SilentlyContinue |
		Select-Object -ExpandProperty FullName
}

$testExe = Join-Path $PSScriptRoot 'VirtualDiskTests.exe'
if (-not (Test-Path $testExe)) {
	throw "VirtualDiskTests.exe must be in the same folder as this script: $PSScriptRoot"
}

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
	Write-Warning 'Not running as administrator: the integration tests will be skipped.'
}

# The registry works everywhere. ProductName still says Windows 10 on Windows 11, so the
# build number decides.
$os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$osName = if ([int]$os.CurrentBuildNumber -ge 22000) { 'Windows 11' } else { $os.ProductName }

$disksBefore = @(Get-DiskSnapshot)
$volumesBefore = @(Get-VolumeSnapshot)
$leftoversBefore = @(Get-Leftovers)

$xmlPath = Join-Path $env:TEMP "VirtualDiskTests-$PID.xml"
$started = Get-Date

# Windows PowerShell turns a program's stderr output into errors, which Stop would make fatal.
$ErrorActionPreference = 'Continue'
$output = & $testExe "--gtest_output=xml:$xmlPath" 2>&1 | ForEach-Object { $_.ToString(); Write-Host $_ }
$testExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'

$finished = Get-Date

$disksAfter = @(Get-DiskSnapshot)
$volumesAfter = @(Get-VolumeSnapshot)
$newLeftovers = @(Get-Leftovers | Where-Object { $leftoversBefore -notcontains $_ })

$machineChecked = $machineProblems.Count -eq 0
$diskChanges = @()
$volumeChanges = @()
if ($machineChecked) {
	$diskChanges = @(Compare-Object $disksBefore $disksAfter | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" })
	$volumeChanges = @(Compare-Object $volumesBefore $volumesAfter | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" })
}

$total = 0
$failed = 0
$skipped = 0
$failures = @()
$skips = @()
if (Test-Path $xmlPath) {
	[xml]$xml = Get-Content $xmlPath -Raw -Encoding UTF8
	$total = [int]$xml.testsuites.tests
	$failed = [int]$xml.testsuites.failures
	foreach ($case in $xml.SelectNodes('//testcase')) {
		$name = "$($case.classname).$($case.name)"
		if ($case.SelectSingleNode('failure')) {
			$failures += $name
		}
		elseif ($case.result -eq 'skipped') {
			$skipped++
			$skips += $name
		}
	}
	Remove-Item $xmlPath
}

$clean = $diskChanges.Count -eq 0 -and $volumeChanges.Count -eq 0 -and $newLeftovers.Count -eq 0
$unexpectedSkips = @($skips | Where-Object { $_ -notlike '*ReFsWorksOrFailsCleanly' })
$testsPassed = $testExitCode -eq 0 -and $total -gt 0 -and $unexpectedSkips.Count -eq 0

$result = if (-not $testsPassed -or -not $clean) { 'FAIL' } elseif ($machineChecked) { 'PASS' } else { 'INCOMPLETE' }
$machineState = if (-not $clean) { 'CHANGED, see below' } elseif ($machineChecked) { 'left as found' } else { 'NOT CHECKED, see below' }

$report = @(
	'VirtualDiskTool test report'
	"Result:     $result"
	"Tests:      $total run, $failed failed, $skipped skipped"
	"Machine:    $machineState"
	"Exit code:  $testExitCode"
	"Started:    $started"
	"Duration:   $([int]($finished - $started).TotalSeconds) seconds"
	"OS:         $osName, build $($os.CurrentBuildNumber).$($os.UBR)"
	"Account:    $env:USERNAME"
	"Elevated:   $elevated"
	"Binary:     $testExe"
	"SHA256:     $((Get-FileHash $testExe -Algorithm SHA256).Hash)"
	''
	'=== Failed tests ==='
	$failures
	''
	'=== Skipped tests ==='
	$skips
	''
	'=== Problems checking the machine ==='
	$machineProblems | Select-Object -Unique
	''
	'=== Disk changes ==='
	$diskChanges
	''
	'=== Volume changes ==='
	$volumeChanges
	''
	'=== New temporary folders ==='
	$newLeftovers
	''
	'=== Disks before ==='
	$disksBefore
	''
	'=== Volumes before ==='
	$volumesBefore
	''
	'=== Test output ==='
	$output
)
$report | Set-Content -Path $ReportPath -Encoding UTF8

Write-Host ''
Write-Host "Result: $result ($total run, $failed failed, $skipped skipped)"
Write-Host "Report: $ReportPath"
exit $(if ($result -eq 'PASS') { 0 } else { 1 })
