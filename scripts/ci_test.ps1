# WalkerCore — local CI helper (PowerShell).
#
# Collapses the four-step iteration loop into one command:
#
#   1. Prepend $env:PATH with the MinGW-w64 toolchain.
#   2. Run `cmake --build` (incremental; ninja skips untouched files).
#   3. Run `WalkerCoreTests.exe` directly (bypasses ctest plumbing for
#      per-test progress visibility).
#   4. Print a tail summary of pass/fail counts.
#
# Usage (from the repo root):
#
#   pwsh -File scripts/ci_test.ps1
#
# The script is idempotent — running it twice in a row only rebuilds
# the files that changed. Configure is only re-run if `build-ninja/`
# is missing (e.g. fresh checkout).
#
# To force a full re-configure (e.g. after editing CMakeLists.txt):
#
#   pwsh -File scripts/ci_test.ps1 -Reconfigure
#
# To use MSVC instead of MinGW:
#
#   pwsh -File scripts/ci_test.ps1 -Toolchain msvc
#
# Notes
#  - The MinGW toolchain root is `C:\msys64\mingw64\bin`. Override
#    with -MingwBin if your install is elsewhere.
#  - We deliberately do NOT add this script to CI itself — CI uses
#    the matrix in `.github/workflows/ci.yml`. This script is purely
#    a developer ergonomics wrapper so every iteration step is a
#    single shell command.

[CmdletBinding()]
param(
    [switch]$Reconfigure,
    [ValidateSet('mingw', 'msvc')]
    [string]$Toolchain = 'mingw',
    [string]$MingwBin = 'C:\msys64\mingw64\bin',
    [string]$BuildDir = 'build-ninja',
    [string]$SourceDir = '.',
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------------------
# Sanity.
# ----------------------------------------------------------------------------
if (-not (Test-Path $SourceDir)) {
    Write-Error "Source directory not found: $SourceDir"
}
$SourceDir = (Resolve-Path $SourceDir).Path
Write-Host "WalkerCore local CI helper"
Write-Host "  source     = $SourceDir"
Write-Host "  build dir  = $BuildDir"
Write-Host "  toolchain  = $Toolchain"
Write-Host "  reconfigure= $($Reconfigure.IsPresent)"

# ----------------------------------------------------------------------------
# PATH setup.
# ----------------------------------------------------------------------------
if ($Toolchain -eq 'mingw') {
    if (-not (Test-Path $MingwBin)) {
        Write-Error "MinGW bin dir not found: $MingwBin (override with -MingwBin)"
    }
    if (-not ($env:PATH -like "*$MingwBin*")) {
        $env:PATH = "$MingwBin;$env:PATH"
        Write-Host "  PATH       = $MingwBin prepended"
    }
    $cmake = 'cmake'
    $generator = 'Ninja'
    $configureExtra = @('-DWALKERCORE_BUILD_TESTS=ON')
    $buildExtra = @()
    $testExe = Join-Path $BuildDir 'tests\WalkerCoreTests.exe'
}
else {
    # MSVC — assume Developer Command Prompt or cmake.exe is on PATH
    # with the MSVC toolchain already loaded. We don't try to set up
    # vcvarsall here because that's environment-specific.
    $cmake = 'cmake'
    $generator = 'Ninja'
    $configureExtra = @('-DWALKERCORE_BUILD_TESTS=ON', "-DCMAKE_BUILD_TYPE=$Config")
    $buildExtra = @('--config', $Config)
    $testExe = Join-Path $BuildDir 'tests\WalkerCoreTests.exe'
}

# ----------------------------------------------------------------------------
# Configure (only if needed).
# ----------------------------------------------------------------------------
$needsConfigure = $Reconfigure.IsPresent -or -not (Test-Path $BuildDir)
if ($needsConfigure) {
    Write-Host ""
    Write-Host "[configure] $cmake -S `"$SourceDir`" -B `"$BuildDir`" -G `"$generator`" $($configureExtra -join ' ')"
    & $cmake -S $SourceDir -B $BuildDir -G $generator @configureExtra
    if ($LASTEXITCODE -ne 0) {
        Write-Error "cmake configure failed (exit $LASTEXITCODE)"
    }
}
else {
    Write-Host ""
    Write-Host "[configure] skipped (build dir exists; pass -Reconfigure to force)"
}

# ----------------------------------------------------------------------------
# Build.
# ----------------------------------------------------------------------------
Write-Host ""
Write-Host "[build] $cmake --build `"$BuildDir`" $($buildExtra -join ' ')"
& $cmake --build $BuildDir @buildExtra
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake build failed (exit $LASTEXITCODE)"
}

# ----------------------------------------------------------------------------
# Test.
# ----------------------------------------------------------------------------
$absTestExe = Join-Path $SourceDir $testExe
if (-not (Test-Path $absTestExe)) {
    Write-Error "Test executable not found: $absTestExe"
}
Write-Host ""
Write-Host "[test] $absTestExe"
Write-Host ""
# Run the test exe WITHOUT redirecting through Out-String — piping
# native-exe output through Out-String converts a non-zero native
# exit code into a PowerShell RemoteException even when the test
# itself succeeded. We want the real exit code propagated.
& $absTestExe
$testExit = $LASTEXITCODE

Write-Host ""
if ($testExit -eq 0) {
    Write-Host "[done] PASS — build + tests clean" -ForegroundColor Green
}
else {
    Write-Host "[done] FAIL — tests reported $testExit failures" -ForegroundColor Red
}
exit $testExit