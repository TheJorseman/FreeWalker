# WalkerCore - F10 readiness check.
#
# Run from any PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\verify_f10_readiness.ps1
#
# What it does:
#   1. Verifies the DLL at plugins/libWalkerCore.dll is bit-for-bit the
#      same as build-ninja/libWalkerCore.dll (catches old deployments).
#   2. Locates the user's active controls.sii (OneDrive- + locale-aware
#      candidate enumeration, parallels GameFreeCameraShim's runtime
#      probe).
#   3. Scans controls.sii for a `mix dev_camera_toggle` binding. If
#      found, prints the resolved SCS token and the inferred Win32 VK.
#      If not found, prints the exact line the user needs to add.
#   4. Locates config.cfg and reports the `g_developer` value the
#      shim's runtime probe will read.
#   5. Predicts what the shim will do at F10 based on the three
#      M2.5 smart-default gates (INI explicit, binding found,
#      g_developer supported).
#
# Pure read-only - never copies files, never edits the game config.
# This script is a sibling of scripts/diagnose.ps1 and shares the
# same candidate list hard-coded (one-line edit if it drifts).

[CmdletBinding()]
param(
    [string]$GameDir      = 'C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64',
    [string]$ProfileDir   = "$env:USERPROFILE\OneDrive\Documentos\American Truck Simulator",
    [string]$SourceDll    = "$PSScriptRoot\..\build-ninja\libWalkerCore.dll",
    [string]$DeployedDll  = '',
    [string]$RoamingIni   = "$env:APPDATA\WalkerCore\WalkerCore.ini"
)

$ErrorActionPreference = 'Continue'
if (-not $DeployedDll) {
    $DeployedDll = Join-Path $GameDir 'plugins\libWalkerCore.dll'
}

function Emit([string]$Line) {
    Write-Host $Line
}

function Section([string]$title) {
    Emit ''
    Emit ('=' * 78)
    Emit ('===  ' + $title)
    Emit ('=' * 78)
}

# SCS keyboard token -> Win32 VK. Subset that covers real ATS / ETS2
# dev_camera_toggle bindings. Mirrors
# GameFreeCameraShim::ScsKeyNameToVkForTesting in
# src/provider/GameFreeCameraShim.cpp. Keep both tables in sync if
# extended.
function ScsKeyToVk([string]$name) {
    if ($name.StartsWith('keyboard.', [System.StringComparison]::Ordinal)) {
        $key = $name.Substring('keyboard.'.Length)
    } else {
        return 0
    }
    $map = @{
        'f1' = 0x70; 'f2' = 0x71; 'f3' = 0x72; 'f4' = 0x73
        'f5' = 0x74; 'f6' = 0x75; 'f7' = 0x76; 'f8' = 0x77
        'f9' = 0x78; 'f10' = 0x79; 'f11' = 0x7A; 'f12' = 0x7B
        'num0' = 0x60; 'num1' = 0x61; 'num2' = 0x62; 'num3' = 0x63
        'num4' = 0x64; 'num5' = 0x65; 'num6' = 0x66; 'num7' = 0x67
        'num8' = 0x68; 'num9' = 0x69
        'numpad0' = 0x60; 'numpad1' = 0x61; 'numpad2' = 0x62
        'numpad3' = 0x63; 'numpad4' = 0x64; 'numpad5' = 0x65
        'numpad6' = 0x66; 'numpad7' = 0x67; 'numpad8' = 0x68
        'numpad9' = 0x69
        'numperiod' = 0x6E; 'numslash' = 0x6F; 'numstar' = 0x6A
        'numminus' = 0x6D; 'numplus' = 0x6B; 'numlock' = 0x90
        'lshift' = 0xA0; 'rshift' = 0xA1
        'lctrl'  = 0xA2; 'rctrl'  = 0xA3
        'lalt'   = 0xA4; 'ralt'   = 0xA5
        'space'    = 0x20; 'enter'    = 0x0D; 'escape'   = 0x1B
        'tab'      = 0x09; 'backquote'= 0xC0
        'home'   = 0x24; 'end'    = 0x23; 'insert' = 0x2D
        'delete' = 0x2E; 'pgup'   = 0x21; 'pgdn'   = 0x22
        'up'     = 0x26; 'down'   = 0x28
        'left'   = 0x25; 'right'  = 0x27
    }
    if ($map.ContainsKey($key)) { return $map[$key] }
    if ($key.Length -eq 1 -and $key -match '^[0-9]$') {
        return [int][char]$key[0]
    }
    if ($key.Length -eq 1 -and $key -match '^[a-z]$') {
        return [int][char]([char]([int][char]$key[0] - [int][char]'a' + [int][char]'A'))
    }
    return 0
}

Emit ('WalkerCore F10 readiness check - ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))

Section '1. DLL identity (source vs deployed)'
if (Test-Path $SourceDll) {
    $srcHash = (Get-FileHash -Algorithm SHA256 $SourceDll).Hash
    Emit ('Source DLL  : ' + $SourceDll)
    Emit ('             sha256 = ' + $srcHash + '   size = ' + (Get-Item $SourceDll).Length + ' bytes')
} else {
    Emit 'Source DLL: NOT FOUND - build first (cmake -G Ninja ... + ninja)'
    $srcHash = '<missing>'
}
if (Test-Path $DeployedDll) {
    $depHash = (Get-FileHash -Algorithm SHA256 $DeployedDll).Hash
    Emit ('Deployed DLL: ' + $DeployedDll)
    Emit ('             sha256 = ' + $depHash + '   size = ' + (Get-Item $DeployedDll).Length + ' bytes')
    if ($srcHash -ne '<missing>' -and $srcHash -eq $depHash) {
        Emit '             MATCH  -> deployed copy is the freshly-built binary'
    } else {
        Emit '             MISMATCH -> deployed copy is older than the build output.'
        Emit ('             Re-copy with: Copy-Item "{0}" "{1}" -Force' -f $SourceDll, $DeployedDll)
    }
} else {
    Emit ('Deployed DLL: NOT FOUND at ' + $DeployedDll)
    Emit ('             Game cannot load the plugin. Copy with:')
    Emit ('             Copy-Item "{0}" "{1}" -Force' -f $SourceDll, $DeployedDll)
}

Section '2. controls.sii auto-detect probe'
Emit ('Search root: ' + $ProfileDir)
# Multi-profile installs put controls.sii under <ProfileDir>/profiles/<id>/.
# Walk the profiles dir for any controls.sii (the runtime does the same
# via GameFreeCameraShim::ProbeActiveControlsSii — picks the first one
# that exists).
$controlsSiiCandidates = @(
    (Join-Path $ProfileDir 'controls.sii')
)
$profilesDir = Join-Path $ProfileDir 'profiles'
if (Test-Path $profilesDir) {
    Get-ChildItem -LiteralPath $profilesDir -Directory -EA SilentlyContinue |
        ForEach-Object {
            $controlsSiiCandidates += (Join-Path $_.FullName 'controls.sii')
        }
}
$controlsSii = $null
foreach ($c in $controlsSiiCandidates) {
    if (Test-Path $c) { $controlsSii = $c; break }
}
$bindingFound = $false
$bindingKey = ''
$bindingLine = 0
if ($controlsSii) {
    Emit ('Found: ' + $controlsSii)
    $lines = Get-Content -LiteralPath $controlsSii -EA SilentlyContinue
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        # Match `mix dev_camera_toggle `keyboard.X?0 | ...`` (with literal backtick).
        if ($line -match '^config_lines\[\d+\]:\s*"mix\s+dev_camera_toggle\s+`([^`]+)`"') {
            $inside = $Matches[1]
            # Pick first keyboard.* token.
            $tokens = $inside -split '\|'
            foreach ($t in $tokens) {
                $t = $t.Trim()
                $qm = $t.IndexOf('?')
                if ($qm -ge 0) { $t = $t.Substring(0, $qm) }
                if ($t.StartsWith('keyboard.', [System.StringComparison]::Ordinal)) {
                    $bindingFound = $true
                    $bindingKey = $t
                    $bindingLine = $i + 1
                    break
                }
            }
            if ($bindingFound) { break }
        }
    }
    if ($bindingFound) {
        $vk = ScsKeyToVk $bindingKey
        Emit ('Binding: line {0} -> `{1}` -> VK 0x{2:X}' -f $bindingLine, $bindingKey, $vk)
    } else {
        Emit 'Binding: NONE for `mix dev_camera_toggle`'
        Emit '         The M2.5 auto-detect will skip (gate 2 of 3 fails).'
        Emit '         Add this line under your active profile in controls.sii:'
        Emit '           config_lines[N+1]: "mix dev_camera_toggle \`keyboard.f11?0 | semantical.dev_camera_toggle?0\`"'
        Emit '         (substitute f11?0 for any key you prefer - the shim auto-detects F1-F12,'
        Emit '          numpad, modifiers, single letters, cursor keys.)'
    }
} else {
    Emit ('controls.sii NOT FOUND in any candidate under ' + $ProfileDir)
    Emit '         Probed:'
    foreach ($c in $controlsSiiCandidates) { Emit ('           - ' + $c) }
    Emit '         Check that $ProfileDir is correct (the script defaults to OneDrive\Documentos\American Truck Simulator).'
    Emit '         Common alternates: OneDrive\Documents\American Truck Simulator, Documents\American Truck Simulator.'
}

Section '3. config.cfg g_developer probe'
$configCfg = Join-Path $ProfileDir 'config.cfg'
$gdev = ''
if (Test-Path $configCfg) {
    Emit ('Found: ' + $configCfg)
    $m = Select-String -Path $configCfg -Pattern 'uset g_developer\s+"([^"]+)"' -EA SilentlyContinue
    if ($m -and $m.Matches -and $m.Matches.Count -gt 0) {
        $gdev = $m.Matches[0].Groups[1].Value
        Emit ('g_developer = "{0}"' -f $gdev)
        if ($gdev -eq '1' -or $gdev -eq '2') {
            Emit '         -> supported (gate 3 of 3 passes)'
        } else {
            Emit '         -> NOT supported. Change to "1" in config.cfg (gate 3 of 3 fails).'
        }
    } else {
        Emit 'g_developer: NOT FOUND in config.cfg'
        Emit '         Add this line: uset g_developer "1"'
        Emit '         (DevCameraCvarPatcher auto-sets it at runtime, but a clean config is clearer.)'
    }
} else {
    Emit ('config.cfg NOT FOUND at ' + $configCfg)
}

Section '4. WalkerCore.ini shim-enable probe (gate 1 of 3)'
# M2.5 hotfix: the M2.5-managed sentinel `# walkercore:managed:<key>`
# suppresses the `*_explicit` flag. A line that LOOKS user-written but
# has the sentinel right below it is auto-managed — the smart-default
# can still flip it. We mirror that logic in this probe.
$iniExplicit = $false
$iniShimValue = ''
$iniShimManaged = $false
if (Test-Path $RoamingIni) {
    Emit ('Found: ' + $RoamingIni)
    $content = Get-Content -LiteralPath $RoamingIni -EA SilentlyContinue
    $inM2 = $false
    $prev = ''
    foreach ($line in $content) {
        $trimmed = $line.Trim()
        # Sentinel marker: appears on a line of its own, inside [m2].
        if ($inM2 -and $trimmed -match '^#\s*walkercore:managed:game_free_camera_shim\s*$') {
            $iniShimManaged = $true
        }
        if ($line -match '^\s*\[m2\]\s*$') { $inM2 = $true; $prev = ''; continue }
        if ($line -match '^\s*\[') { $inM2 = $false; $prev = ''; continue }
        if ($inM2 -and $line -match '^\s*game_free_camera_shim\s*=\s*(.+?)\s*$') {
            $iniShimValue = $Matches[1].Trim()
            $iniExplicit = $true
        }
        $prev = $line
    }
    if (-not $iniExplicit) {
        Emit '[m2] game_free_camera_shim: NOT PRESENT -> smart-default auto-enable is allowed'
    } elseif ($iniShimManaged) {
        Emit ('[m2] game_free_camera_shim = {0} + sentinel -> auto-managed (smart-default ALLOWED)' -f $iniShimValue)
        $iniExplicit = $false  # override: sentinel wins
    } else {
        Emit ('[m2] game_free_camera_shim = {0} (EXPLICIT in INI -> smart-default SKIPPED)' -f $iniShimValue)
    }
} else {
    Emit ('WalkerCore.ini NOT FOUND at ' + $RoamingIni)
    Emit '         (The shim will be created with smart-default; user can later add explicit overrides.)'
}

Section '5. M2.5 smart-default prediction'
$gate1 = -not $iniExplicit
$gate2 = $bindingFound
$gate3 = ($gdev -eq '1' -or $gdev -eq '2')
Emit ('Gate 1 (INI did not name game_free_camera_shim) : ' + $(if ($gate1) {'PASS'} else {'FAIL'}))
Emit ('Gate 2 (controls.sii has dev_camera_toggle bind) : ' + $(if ($gate2) {'PASS'} else {'FAIL'}))
Emit ('Gate 3 (config.cfg has g_developer = 1 or 2)   : ' + $(if ($gate3) {'PASS'} else {'FAIL'}))
if ($gate1 -and $gate2 -and $gate3) {
    Emit ''
    Emit 'PREDICTION: shim will flip enabled_ from false to true at DLL load.'
    Emit '            F10 -> Walking entry -> shim dispatch -> SCS free camera (assuming dev_camera_toggle is bound to the SCS developer free camera in-game).'
    Emit '            If F10 does NOT move the camera, the SCS engine may not consume dev_camera_toggle on this build.'
    Emit '            Capture the F12 dump + game.log and we will diagnose.'
} else {
    Emit ''
    Emit 'PREDICTION: shim will stay disabled (default false).'
    Emit '            F10 will move the walker FSM but the camera will NOT switch to SCS free camera.'
    Emit '            Fix the failing gate(s) above, then restart ATS.'
}

Section 'Done'
Emit ''
Emit 'Re-run this script any time. It is read-only.'