<#
.SYNOPSIS
    Real install-and-consume proof for Energy-Cost-Governor.

.DESCRIPTION
    Configure and build a Release tree with tests and benchmarks OFF, install it
    into a CLEAN prefix, build the independent out-of-tree consumer at
    examples/find_package_consumer against that prefix, run the consumer, and run
    the installed CLI. Every step prints the exact command line it ran, and the
    script ends with "VALIDATION: PASS" or "VALIDATION: FAIL" and a non-zero exit
    code when anything failed.

    Nothing here depends on the current working directory: the repository root is
    derived from this script's own location and every work path is absolute.

    No external modules are used.

.PARAMETER WorkDir
    Scratch root holding the library build, the install prefix, and the consumer
    build. Defaults to <system temp>\ecg-validate-install. It is deleted first if
    it already exists, so the run always starts clean.

.PARAMETER SourceDir
    Repository root. Defaults to the parent directory of this script.

.PARAMETER Generator
    CMake generator. Defaults to Ninja, which the MSVC developer environment
    supports without a Visual Studio generator.

.PARAMETER MsvcEnv
    The helper that puts cl.exe and the Windows SDK on PATH. Dot-sourced by this
    script so the caller does not have to prepare a shell first.

.PARAMETER Cleanup
    Delete WorkDir when the validation passes. Off by default so the artefacts
    can be inspected.

.EXAMPLE
    pwsh -File scripts\validate_install.ps1
.EXAMPLE
    pwsh -File scripts\validate_install.ps1 -WorkDir D:\scratch\ecg -Cleanup
#>
[CmdletBinding()]
param(
  [string] $WorkDir,
  [string] $SourceDir,
  [string] $Generator = 'Ninja',
  [string] $MsvcEnv = 'C:\Users\pauln\.ecg-scratch\msvc-env.ps1',
  [switch] $Cleanup
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
# PowerShell 7.3+ turns a native command's stderr into a terminating error under
# ErrorActionPreference = 'Stop', which would hide the exit code and the command
# line this script exists to report. Keep native failures as values.
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
  $PSNativeCommandUseErrorActionPreference = $false
}

# ---------------------------------------------------------------------------
# Reporting helpers
# ---------------------------------------------------------------------------

$script:StepResults = New-Object System.Collections.Generic.List[object]

function Write-Rule([string] $Text) {
  Write-Host ''
  Write-Host ('=' * 78)
  Write-Host $Text
  Write-Host ('=' * 78)
}

function Write-Step([int] $Number, [string] $Title) {
  Write-Host ''
  Write-Host ("--- step {0}: {1}" -f $Number, $Title)
}

function Format-CommandLine([string] $FilePath, [string[]] $Arguments) {
  $parts = @($FilePath) + $Arguments
  $quoted = foreach ($part in $parts) {
    if ($part -match '[\s"]') { '"' + $part + '"' } else { $part }
  }
  return ($quoted -join ' ')
}

# Runs a native command, echoes the exact command line, and returns its exit code
# and captured output. The command line is printed whether it succeeds or fails,
# because a validation report is worthless without the commands that produced it.
function Invoke-Native {
  param(
    [Parameter(Mandatory = $true)][string] $FilePath,
    [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]] $Arguments
  )
  Write-Host ('  $ ' + (Format-CommandLine $FilePath $Arguments))
  # A native command's stderr is diagnostic text to be reported, not a
  # terminating error: this function has to return the exit code either way.
  $previousPreference = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $output = & $FilePath @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previousPreference
  }
  return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

function Add-StepResult {
  param(
    [string] $Name,
    [ValidateSet('PASS', 'FAIL', 'SKIPPED')][string] $Status,
    [string[]] $Commands,
    [string] $Detail
  )
  $script:StepResults.Add([pscustomobject]@{
    Name     = $Name
    Status   = $Status
    Commands = $Commands
    Detail   = $Detail
  })
}

function Write-CommandOutput([string] $Output, [int] $MaxLines = 40) {
  $lines = @($Output -split "\r?\n" | Where-Object { $_ -ne '' })
  if ($lines.Count -le $MaxLines) {
    foreach ($line in $lines) { Write-Host ('    | ' + $line) }
    return
  }
  foreach ($line in $lines[0..($MaxLines - 1)]) { Write-Host ('    | ' + $line) }
  Write-Host ("    | ... {0} more line(s) suppressed" -f ($lines.Count - $MaxLines))
}

# Prints the per-step report and returns the process exit code.
function Write-Summary {
  Write-Rule 'validation summary'
  $failed = $false
  foreach ($step in $script:StepResults) {
    Write-Host ("  [{0,-7}] {1}" -f $step.Status, $step.Name)
    foreach ($command in $step.Commands) {
      Write-Host ("              $ {0}" -f $command)
    }
    if ($step.Detail) {
      Write-Host ("              {0}" -f $step.Detail)
    }
    if ($step.Status -eq 'FAIL') { $failed = $true }
  }
  Write-Host ''
  if ($failed) {
    Write-Host 'VALIDATION: FAIL'
    return 1
  }
  Write-Host 'VALIDATION: PASS'
  return 0
}

# ---------------------------------------------------------------------------
# Preconditions. The repository root comes from this script's own location, so
# the caller may run it from any working directory.
# ---------------------------------------------------------------------------

Write-Rule 'Energy-Cost-Governor install and consume validation'

$scriptDir = Split-Path -Parent $PSCommandPath
if (-not $SourceDir) { $SourceDir = Split-Path -Parent $scriptDir }
if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
  Add-StepResult -Name '0. locate the source tree' -Status 'FAIL' -Commands @() -Detail ("not a directory: {0}" -f $SourceDir)
  exit (Write-Summary)
}
$root = (Resolve-Path -LiteralPath $SourceDir).Path
$rootCMakeLists = Join-Path $root 'CMakeLists.txt'
$consumerSourceDir = Join-Path $root 'examples\find_package_consumer'
$consumerCMakeLists = Join-Path $consumerSourceDir 'CMakeLists.txt'

if (-not $WorkDir) { $WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) 'ecg-validate-install' }
$work = [System.IO.Path]::GetFullPath($WorkDir)
$buildDir = Join-Path $work 'build'
$prefixDir = Join-Path $work 'prefix'
$consumerBuildDir = Join-Path $work 'consumer-build'

Write-Host ("  repository root : {0}" -f $root)
Write-Host ("  work directory  : {0}" -f $work)
Write-Host ("  install prefix  : {0}" -f $prefixDir)
Write-Host ("  generator       : {0}" -f $Generator)

$missing = @()
foreach ($required in @($rootCMakeLists, (Join-Path $consumerSourceDir 'main.cpp'), $consumerCMakeLists)) {
  if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { $missing += $required }
}
if ($missing.Count -gt 0) {
  Add-StepResult -Name '0. locate the source tree' -Status 'FAIL' -Commands @() -Detail ("missing: {0}" -f ($missing -join '; '))
  exit (Write-Summary)
}

# The MSVC developer environment, so cl.exe and the Windows SDK are on PATH for
# this process. Dot-sourced, exactly as a caller would do by hand.
if (-not (Test-Path -LiteralPath $MsvcEnv -PathType Leaf)) {
  Add-StepResult -Name '0. developer environment' -Status 'FAIL' -Commands @(". '{0}'" -f $MsvcEnv) -Detail 'the MSVC environment helper does not exist'
  exit (Write-Summary)
}
try {
  . $MsvcEnv
} catch {
  Add-StepResult -Name '0. developer environment' -Status 'FAIL' -Commands @(". '{0}'" -f $MsvcEnv) -Detail ("dot-sourcing failed: {0}" -f $_.Exception.Message)
  exit (Write-Summary)
}

$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$clCommand = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cmakeCommand -or -not $clCommand) {
  $detail = @()
  if (-not $cmakeCommand) { $detail += 'cmake.exe not found on PATH' }
  if (-not $clCommand) { $detail += 'cl.exe not found on PATH' }
  Add-StepResult -Name '0. developer environment' -Status 'FAIL' -Commands @(". '{0}'" -f $MsvcEnv) -Detail ($detail -join '; ')
  exit (Write-Summary)
}
$cmake = $cmakeCommand.Source
Write-Host ("  cmake           : {0}" -f $cmake)
Write-Host ("  cl              : {0}" -f $clCommand.Source)
$cmakeVersion = (& $cmake --version | Select-Object -First 1)
Write-Host ("  cmake version   : {0}" -f $cmakeVersion)
Add-StepResult -Name '0. developer environment' -Status 'PASS' -Commands @(". '{0}'" -f $MsvcEnv) -Detail ("cmake {0}" -f $cmakeVersion)

# ---------------------------------------------------------------------------
# Step 1: a clean scratch root
# ---------------------------------------------------------------------------
Write-Step 1 'prepare a clean work directory'
$removeWork = "Remove-Item -LiteralPath '{0}' -Recurse -Force" -f $work
Write-Host ('  $ ' + $removeWork)
if (Test-Path -LiteralPath $work) {
  Remove-Item -LiteralPath $work -Recurse -Force
}
New-Item -ItemType Directory -Path $work | Out-Null
Add-StepResult -Name '1. clean work directory' -Status 'PASS' -Commands @($removeWork, ("New-Item -ItemType Directory -Path '{0}'" -f $work)) -Detail ("scratch root: {0}" -f $work)

# ---------------------------------------------------------------------------
# Step 2: Release configure and build, tests and benchmarks off
# ---------------------------------------------------------------------------
Write-Step 2 'configure and build Release (tests and benchmarks off)'
$configureArguments = @(
  '-S', $root,
  '-B', $buildDir,
  '-G', $Generator,
  '-DCMAKE_BUILD_TYPE=Release',
  '-DECG_BUILD_TESTS=OFF',
  '-DECG_BUILD_BENCHMARKS=OFF'
)
$configureCommand = Format-CommandLine $cmake $configureArguments
$configure = Invoke-Native -FilePath $cmake -Arguments $configureArguments
Write-CommandOutput $configure.Output

$buildArguments = @('--build', $buildDir, '--config', 'Release')
$buildCommand = Format-CommandLine $cmake $buildArguments
$build = Invoke-Native -FilePath $cmake -Arguments $buildArguments
Write-CommandOutput $build.Output

if ($configure.ExitCode -ne 0 -or $build.ExitCode -ne 0) {
  Add-StepResult -Name '2. configure and build Release' -Status 'FAIL' -Commands @($configureCommand, $buildCommand) -Detail ("configure exit {0}, build exit {1}" -f $configure.ExitCode, $build.ExitCode)
  exit (Write-Summary)
}
Add-StepResult -Name '2. configure and build Release' -Status 'PASS' -Commands @($configureCommand, $buildCommand) -Detail 'library and CLI built with tests and benchmarks disabled'

# ---------------------------------------------------------------------------
# Step 3: install into a prefix that is guaranteed to be clean
# ---------------------------------------------------------------------------
Write-Step 3 'install into a clean prefix'
$removePrefix = "Remove-Item -LiteralPath '{0}' -Recurse -Force" -f $prefixDir
Write-Host ('  $ ' + $removePrefix)
if (Test-Path -LiteralPath $prefixDir) {
  Remove-Item -LiteralPath $prefixDir -Recurse -Force
}
$installArguments = @('--install', $buildDir, '--prefix', $prefixDir, '--config', 'Release')
$installCommand = Format-CommandLine $cmake $installArguments
$install = Invoke-Native -FilePath $cmake -Arguments $installArguments
Write-CommandOutput $install.Output

$expectedArtifacts = @(
  (Join-Path $prefixDir 'include\ecg\engine.hpp'),
  (Join-Path $prefixDir 'include\ecg\ledger.hpp'),
  (Join-Path $prefixDir 'lib\cmake\ECG\ECGConfig.cmake'),
  (Join-Path $prefixDir 'lib\cmake\ECG\ECGConfigVersion.cmake'),
  (Join-Path $prefixDir 'lib\cmake\ECG\ECGTargets.cmake'),
  (Join-Path $prefixDir 'bin\ecg.exe')
)
$missingArtifacts = @()
foreach ($artifact in $expectedArtifacts) {
  if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) { $missingArtifacts += $artifact }
}
$libraryFiles = @(Get-ChildItem -LiteralPath (Join-Path $prefixDir 'lib') -Filter 'ecg_core.*' -File -ErrorAction SilentlyContinue)
if ($libraryFiles.Count -eq 0) { $missingArtifacts += (Join-Path $prefixDir 'lib\ecg_core.*') }

if ($install.ExitCode -ne 0 -or $missingArtifacts.Count -gt 0) {
  $detail = "install exit {0}" -f $install.ExitCode
  if ($missingArtifacts.Count -gt 0) { $detail = $detail + ("; missing: {0}" -f ($missingArtifacts -join ', ')) }
  Add-StepResult -Name '3. install into a clean prefix' -Status 'FAIL' -Commands @($removePrefix, $installCommand) -Detail $detail
  exit (Write-Summary)
}
Add-StepResult -Name '3. install into a clean prefix' -Status 'PASS' -Commands @($removePrefix, $installCommand) -Detail ("headers, package config, {0} artifact(s) and bin\ecg.exe installed under {1}" -f $libraryFiles.Count, $prefixDir)

# ---------------------------------------------------------------------------
# Step 4: build the independent out-of-tree consumer against the prefix
# ---------------------------------------------------------------------------
Write-Step 4 'configure and build the out-of-tree find_package consumer'
$consumerConfigureArguments = @(
  '-S', $consumerSourceDir,
  '-B', $consumerBuildDir,
  '-G', $Generator,
  '-DCMAKE_BUILD_TYPE=Release',
  ('-DCMAKE_PREFIX_PATH=' + $prefixDir)
)
$consumerConfigureCommand = Format-CommandLine $cmake $consumerConfigureArguments
$consumerConfigure = Invoke-Native -FilePath $cmake -Arguments $consumerConfigureArguments
Write-CommandOutput $consumerConfigure.Output

$consumerBuildArguments = @('--build', $consumerBuildDir, '--config', 'Release')
$consumerBuildCommand = Format-CommandLine $cmake $consumerBuildArguments
$consumerBuild = Invoke-Native -FilePath $cmake -Arguments $consumerBuildArguments
Write-CommandOutput $consumerBuild.Output

$consumerExe = Join-Path $consumerBuildDir 'ecg_find_package_consumer.exe'
if (-not (Test-Path -LiteralPath $consumerExe -PathType Leaf)) {
  $found = Get-ChildItem -LiteralPath $consumerBuildDir -Recurse -Filter 'ecg_find_package_consumer.exe' -File -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { $consumerExe = $found.FullName }
}
if ($consumerConfigure.ExitCode -ne 0 -or $consumerBuild.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $consumerExe -PathType Leaf)) {
  Add-StepResult -Name '4. build the out-of-tree consumer' -Status 'FAIL' -Commands @($consumerConfigureCommand, $consumerBuildCommand) -Detail ("configure exit {0}, build exit {1}, executable found: {2}" -f $consumerConfigure.ExitCode, $consumerBuild.ExitCode, (Test-Path -LiteralPath $consumerExe -PathType Leaf))
  exit (Write-Summary)
}
Add-StepResult -Name '4. build the out-of-tree consumer' -Status 'PASS' -Commands @($consumerConfigureCommand, $consumerBuildCommand) -Detail ("find_package(ECG CONFIG) resolved against {0}" -f $prefixDir)

# ---------------------------------------------------------------------------
# Step 5: run the consumer; only exit code 0 counts as proof
# ---------------------------------------------------------------------------
Write-Step 5 'run the consumer and require exit code 0'
$consumerRunCommand = Format-CommandLine $consumerExe @()
$consumerRun = Invoke-Native -FilePath $consumerExe -Arguments @()
Write-CommandOutput $consumerRun.Output 60
if ($consumerRun.ExitCode -ne 0) {
  Add-StepResult -Name '5. run the consumer' -Status 'FAIL' -Commands @($consumerRunCommand) -Detail ("exit code {0}; the consumer asserts its expected outcomes and failed" -f $consumerRun.ExitCode)
  exit (Write-Summary)
}
Add-StepResult -Name '5. run the consumer' -Status 'PASS' -Commands @($consumerRunCommand) -Detail 'exit code 0: fresh evidence produced allowed, stale evidence produced indeterminate'

# ---------------------------------------------------------------------------
# Step 6: the installed CLI must run from the prefix
# ---------------------------------------------------------------------------
Write-Step 6 'run the installed CLI (ecg.exe version)'
$cliExe = Join-Path $prefixDir 'bin\ecg.exe'
if (-not (Test-Path -LiteralPath $cliExe -PathType Leaf)) {
  Add-StepResult -Name '6. run the installed CLI' -Status 'FAIL' -Commands @() -Detail ("the installed CLI is missing: {0}" -f $cliExe)
  exit (Write-Summary)
}
$cliArguments = @('version')
$cliCommand = Format-CommandLine $cliExe $cliArguments
$cliRun = Invoke-Native -FilePath $cliExe -Arguments $cliArguments
Write-CommandOutput $cliRun.Output
$cliText = ([string] $cliRun.Output).Trim()
if ($cliRun.ExitCode -ne 0 -or $cliText.Length -eq 0) {
  Add-StepResult -Name '6. run the installed CLI' -Status 'FAIL' -Commands @($cliCommand) -Detail ("exit code {0}, output length {1}" -f $cliRun.ExitCode, $cliText.Length)
  exit (Write-Summary)
}
Add-StepResult -Name '6. run the installed CLI' -Status 'PASS' -Commands @($cliCommand) -Detail 'the installed ecg.exe answered its version command with exit code 0'

# ---------------------------------------------------------------------------
# Report, and clean up only when the caller asked and everything passed
# ---------------------------------------------------------------------------
$failures = @($script:StepResults | Where-Object { $_.Status -eq 'FAIL' })
if ($Cleanup -and $failures.Count -eq 0) {
  Write-Host ''
  Write-Host ('  $ Remove-Item -LiteralPath {0} -Recurse -Force' -f $work)
  Remove-Item -LiteralPath $work -Recurse -Force
} else {
  Write-Host ''
  Write-Host ("artefacts kept under: {0}" -f $work)
}
exit (Write-Summary)
