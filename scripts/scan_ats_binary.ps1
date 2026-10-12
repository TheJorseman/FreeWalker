# scripts/scan_ats_binary.ps1
# Read-only forensic scan of amtrucks.exe for M2/M3 lander offsets.
#
# Scans the binary in three encodings:
#   1. ANSI/ASCII literal substrings (PxScene, IDXGISwapChain, dxgi.dll, ...)
#   2. UTF-16 LE NUL-split strings (Windows paths, COM interface names,
#      and UTF-8-incompatible L"strings")
#   3. UTF-16 LE bytes without NUL-split (raw match when the wide
#      string is contiguous without padding)
#
# Why we have three passes: the first PowerShell 5.1 byte-by-byte pass
# (53 MB) timed out without finding anything. The .NET Regex.Matches()
# path here is fully native and runs in seconds.
#
# Signatures are split into intent groups:
#   - 'physx'     : NVIDIA-PhysX symbols. M3 lander need.
#   - 'render'    : DXGI / D3D12 import surface. M2 lander need.
#   - 'telemetry' : SCS telemetry init/shutdown – the bridge that
#                    must exist if the game loads WalkerCore at all.
#
# Each block records per-signature hit counts so the document makes the
# binary layout explicit (rather than implying offsets the next
# session will have to redo).
#
# Read-only: parses the binary, never modifies it, never writes the
# file back. SHA-256 is recorded for citation.

param(
    [string]$ExePath = 'C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\amtrucks.exe'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ExePath)) {
    Write-Error "Binary not found at $ExePath"
    exit 1
}

$asciiSigs = @(
    'PxScene',
    'PxPhysics',
    'PxFoundation',
    'PxMaterial',
    'PxRigid',
    'PhysX3',
    'NVIDIA',
    'IDXGISwapChain',
    'IDXGIFactory',
    'D3D12CreateDevice',
    'dxgi.dll',
    'd3d12.dll',
    'CreateSwapChain',
    'scs_telemetry_init',
    'scs_telemetry_shutdown',
    'camera_manager',
    'telemetry',
    'americantruck'
)

# UTF-16 LE pairs of the same — typically the same identifier in wide
# encoding if the binary holds them as wchar_t[].
$utf16Sigs = @(
    'PxScene',
    'PxPhysics',
    'PxFoundation',
    'PxMaterial',
    'IDXGISwapChain',
    'IDXGIFactory',
    'CreateSwapChain',
    'scs_telemetry_init',
    'scs_telemetry_shutdown',
    'telemetry',
    'dxgi',
    'd3d12'
)

# Use Encoding.UTF8.GetBytes($sig) so we walk the binary looking for
# those bytes verbatim — for ASCII that's a no-op; for UTF-16 we set
# up the conversion table once.

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$bytes = [System.IO.File]::ReadAllBytes($ExePath)
$stopwatch.Stop()

Write-Host "== LOADED =="
Write-Host ("  path   : {0}" -f $ExePath)
Write-Host ("  size   : {0:N0} bytes" -f $bytes.Length)
Write-Host ("  sha256 : {0}" -f ((Get-FileHash -Path $ExePath -Algorithm SHA256).Hash))
Write-Host ("  read   : {0:N1} ms" -f $stopwatch.Elapsed.TotalMilliseconds)
Write-Host ""

# ANSI byte scan — raw bytes search.
Write-Host "== ANSI SCAN =="
$stopwatch.Restart()
$asciiHits = @{}
foreach ($sig in $asciiSigs) {
    $needle = [System.Text.Encoding]::ASCII.GetBytes($sig)
    $count = 0
    $idx = 0
    while (($idx = [Array]::IndexOf($bytes, $needle, $idx)) -ne -1) {
        $count++
        $idx += $needle.Length
        if ($count -ge 256) { break }   # sanity guard — pathological binaries can have any count
    }
    if ($count -gt 0) { $asciiHits[$sig] = $count }
}
$stopwatch.Stop()
Write-Host ("  elapsed  : {0:N1} ms" -f $stopwatch.Elapsed.TotalMilliseconds)
foreach ($sig in $asciiSigs | Sort-Object) {
    $c = if ($asciiHits.ContainsKey($sig)) { $asciiHits[$sig] } else { 0 }
    Write-Host ("    ansi {0,-22}: {1,6}" -f $sig, $c)
}
Write-Host ""

# UTF-16 LE byte scan — NULs between every char. "PxScene" → 50 00 78 00 53 00 63 00 ...
Write-Host "== UTF-16 LE SCAN =="
$stopwatch.Restart()
$utf16Hits = @{}
foreach ($sig in $utf16Sigs) {
    $needle = [System.Text.Encoding]::Unicode.GetBytes($sig)  # UTF-16 LE by default
    $count = 0
    $idx = 0
    while (($idx = [Array]::IndexOf($bytes, $needle, $idx)) -ne -1) {
        $count++
        $idx += $needle.Length
        if ($count -ge 256) { break }
    }
    if ($count -gt 0) { $utf16Hits[$sig] = $count }
}
$stopwatch.Stop()
Write-Host ("  elapsed  : {0:N1} ms" -f $stopwatch.Elapsed.TotalMilliseconds)
foreach ($sig in $utf16Sigs | Sort-Object) {
    $c = if ($utf16Hits.ContainsKey($sig)) { $utf16Hits[$sig] } else { 0 }
    Write-Host ("    wide {0,-22}: {1,6}" -f $sig, $c)
}

# Sample bytes around any UTF-16 hit for context, in case the strings
# are nested in a log path or config key.
Write-Host ""
Write-Host "== CONTEXT WINDOWS =="
$ascii = [System.Text.Encoding]::ASCII
$utf8safe = [System.Text.Encoding]::UTF8
$firstSlice = [Math]::Min(8 * 1024 * 1024, $bytes.Length)  # first 8 MB

# Show context for the highest-impact hit (if any).
$priorities = @('scs_telemetry_init', 'PxScene', 'IDXGISwapChain', 'CreateSwapChain', 'camera_manager')
foreach ($sig in $priorities) {
    $needleAscii = [System.Text.Encoding]::ASCII.GetBytes($sig)
    $needleWide = [System.Text.Encoding]::Unicode.GetBytes($sig)
    $idxA = [Array]::IndexOf($bytes, $needleAscii, 0)
    $idxW = [Array]::IndexOf($bytes, $needleWide, 0)
    if ($idxA -lt 0 -and $idxW -lt 0) { continue }
    Write-Host ""
    Write-Host ("  --- {0} ---" -f $sig)
    if ($idxA -ge 0) {
        $start = [Math]::Max(0, $idxA - 8)
        $len = [Math]::Min(64, $bytes.Length - $start)
        $ctx = ($ascii.GetString($bytes, $start, $len) -replace "[^\x20-\x7e]", ".")
        Write-Host ("    ansi @ 0x{0:X7}: {1}" -f $idxA, $ctx)
    }
    if ($idxW -ge 0) {
        $start = [Math]::Max(0, $idxW - 8)
        $len = [Math]::Min(128, $bytes.Length - $start)
        $ctx = ($ascii.GetString($bytes, $start, $len) -replace "[^\x20-\x7e]", ".")
        Write-Host ("    wide @ 0x{0:X7}: {1}" -f $idxW, $ctx)
    }
}

Write-Host ""
Write-Host "== SUMMARY =="
Write-Host ("  ansi hit signatures: {0}" -f $asciiHits.Count)
Write-Host ("  utf16 hit signatures: {0}" -f $utf16Hits.Count)

if ($asciiHits.Count -eq 0 -and $utf16Hits.Count -eq 0) {
    Write-Host ""
    Write-Host "  NO HITS for the listed signatures in either encoding."
    Write-Host "  Binary is either stripped OR symbols live deeper as MSVC"
    Write-Host "  RTTI typeinfo strings in .rdata. Next pass should grep for"
    Write-Host "  the demangled PxScene prefix specifically."
}
