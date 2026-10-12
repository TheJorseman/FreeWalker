# WalkerCore - diagnostic dump script.
#
# Run from any PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\diagnose.ps1
#
# What it does:
#   1. Inventories mods in the game directory (ReShade, RenoDX, DXVK,
#      our own libWalkerCore.dll, the SCS plugin shim, etc.).
#   2. Verifies the DLL on disk is bit-for-bit the same as the build
#      output (catches old deployments left behind after a rebuild).
#   3. If ATS is running, dumps the process module list - focused on
#      the graphics subsystem (dxgi / d3d11 / d3d12 / renodx /
#      reshade / libWalkerCore / dxvk).
#   4. Pulls every [WalkerCore] line + the F12/screenshot lines
#      from the most recent game.log.txt and groups them so the
#      pattern of "hook fired / hook failed / dump triggered" is
#      obvious at a glance.
#   5. Dumps the current WalkerCore.ini verbatim so we can see every
#      bind + head_tracker + features_override setting.
#   6. Writes the full output to walkercore_diag_<timestamp>.txt
#      next to the user's Documents folder AND mirrors it to console.
#
# Pure read-only - never modifies files, never disables anything,
# never restarts ATS.

[CmdletBinding()]
param(
    [string]$GameDir    = 'C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64',
    [string]$ProfileDir = 'C:\Users\migue\OneDrive\Documentos\American Truck Simulator',
    [string]$RoamingDir = "$env:APPDATA\WalkerCore",
    [string]$SourceDll  = "$PSScriptRoot\..\build-ninja\libWalkerCore.dll",
    [string]$DeployedDll = '',
    [string]$OutputDir  = "$env:USERPROFILE\Documents"
)

$ErrorActionPreference = 'Continue'
if (-not $DeployedDll) {
    $DeployedDll = Join-Path $GameDir 'plugins\libWalkerCore.dll'
}

$stamp = (Get-Date -Format 'yyyyMMdd_HHmmss')
$logFile = Join-Path $OutputDir "walkercore_diag_$stamp.txt"
New-Item -Path $OutputDir -ItemType Directory -Force | Out-Null

function Emit {
    param([Parameter(Position=0, ValueFromPipeline=$true)][string]$Line)
    Write-Host $Line
    Add-Content -LiteralPath $logFile -Value $Line -Encoding utf8
}

function Section([string]$title) {
    Emit ''
    Emit ('=' * 78)
    Emit ('===  ' + $title)
    Emit ('=' * 78)
}

Emit ('WalkerCore diagnostic dump - ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Emit ('Log file: ' + $logFile)

Section '1. Game install + mods inventory'
if (Test-Path $GameDir) {
    Emit ('Game bin dir: ' + $GameDir)
    # Case-insensitive - Windows filenames are case-insensitive but PowerShell's
    # regex is case-sensitive by default, so 'renodx-' would not match '^(RenoDX)'.
    $pat = '^(dxgi|d3d11|d3d12|d3dcompiler|ReShade|reshade|renodx|Addon|libWalker|dxvk|vulkan|nvngx|_mgs|opengl)\.dll$'
    $rows = Get-ChildItem -Path $GameDir -Filter '*.dll' -EA SilentlyContinue |
        Where-Object { $_.Name -imatch $pat -or $_.Name -imatch '^(ReShade|RenoDX|ReShadePreset|dxvk)' } |
        Sort-Object Name
    if ($rows) {
        $rows | ForEach-Object {
            $size = '{0:N1} KiB' -f ($_.Length / 1024)
            $line = '  {0,-44} {1,12}' -f $_.Name, $size
            Emit $line
        }
    } else {
        Emit '  (no matching DLLs found)'
    }
    $logs = Get-ChildItem -Path $GameDir -Filter '*.log*' -EA SilentlyContinue |
        Where-Object { $_.LastWriteTime -gt (Get-Date).AddHours(-12) } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 6
    if ($logs) {
        Emit ''
        Emit '  Recent log sidecars:'
        $logs | ForEach-Object {
            Emit ('    [log] {0,-44} {1,12} {2}' -f $_.Name, ('{0:N1} KiB' -f ($_.Length / 1024)), $_.LastWriteTime.ToString('HH:mm:ss'))
        }
    }
} else {
    Emit ('Game bin dir not found: ' + $GameDir)
}

Section '2. plugins/ deployment + DLL identity'
$pluginDir = Split-Path -Parent $DeployedDll
Emit ('Plugin folder: ' + $pluginDir)
if (Test-Path $pluginDir) {
    Get-ChildItem -Path $pluginDir -EA SilentlyContinue |
        Sort-Object Name |
        ForEach-Object {
            Emit ('  {0,-44} {1,12} {2}' -f $_.Name, ('{0:N0} bytes' -f $_.Length), $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
        }
} else {
    Emit '  (missing - game cannot load the plugin)'
}

Emit ''
if (Test-Path $SourceDll) {
    $srcHash = (Get-FileHash -Algorithm SHA256 $SourceDll).Hash
    Emit ('Source DLL  : ' + $SourceDll)
    Emit ('             sha256 = ' + $srcHash + '   size = ' + (Get-Item $SourceDll).Length + ' bytes')
} else {
    Emit ('Source DLL  : ' + $SourceDll + '  (NOT FOUND - build first)')
    $srcHash = '<missing>'
}

if (Test-Path $DeployedDll) {
    $depHash = (Get-FileHash -Algorithm SHA256 $DeployedDll).Hash
    Emit ('Deployed DLL: ' + $DeployedDll)
    Emit ('             sha256 = ' + $depHash + '   size = ' + (Get-Item $DeployedDll).Length + ' bytes')
    if ($srcHash -ne '<missing>' -and $srcHash -eq $depHash) {
        Emit '             MATCH  -> deployed copy is the freshly-built binary'
    } else {
        Emit '             MISMATCH -> deployed copy is older than the build output. Re-run scripts/install.bat (or copy the new DLL).'
    }
} else {
    Emit ('Deployed DLL: ' + $DeployedDll + '  (NOT FOUND - game has no plugin to load)')
}

Section '3. Running amtrucks.exe + graphics module inventory'
$jobs = @(Get-CimInstance Win32_Process -Filter "Name='amtrucks.exe'" -ErrorAction SilentlyContinue)
if ($jobs -and $jobs.Count -gt 0) {
    foreach ($j in $jobs) {
        if ($null -eq $j) { continue }
        Emit ('amtrucks.exe PID ' + $j.ProcessId + ', started ' + $j.StartTime.ToString('HH:mm:ss') + ', uptime ' + ((Get-Date) - $j.StartTime))
        $p = Get-Process -Id $j.ProcessId -EA SilentlyContinue
        if ($p) {
            Emit ('  Working set: {0:N2} GiB' -f ($p.WorkingSet64 / 1GB))
            Emit ''
            Emit '  Graphics-related modules loaded into the process:'
            $p.Modules | Where-Object { $_.ModuleName -imatch '^(d3d1[12]|dxgi|D3D12Core|d3dcompiler|ReShade|reshade|renodx|libWalker|dxvk|vulkan|nvngx)\.' } |
                Sort-Object ModuleName |
                ForEach-Object {
                    $base = '0x{0:X}' -f $_.BaseAddress.ToInt64()
                    Emit ('    {0,-32} base={1,-18} size={2,9:N0}   {3}' -f $_.ModuleName, $base, $_.ModuleMemorySize, $_.FileName)
                }
        }
    }
} else {
    Emit 'amtrucks.exe is not running.'
}

Section '4. game.log.txt - WalkerCore init + per-frame activity'
$log = Join-Path $ProfileDir 'game.log.txt'
if (Test-Path $log) {
    $info = Get-Item $log
    Emit ('Path     : ' + $log)
    Emit ('Size     : {0:N1} KiB' -f ($info.Length / 1024))
    Emit ('Modified : ' + $info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    Emit ''
    $walkerLines = Select-String -Path $log -Pattern '\[WalkerCore\]|MH_CreateHook.*failed|hook live|per_frame|M2 lander|ver=|features=0x|screenshot' -SimpleMatch:$false -EA SilentlyContinue
    if ($walkerLines) {
        $walkerLines | Select-Object -Last 35 | ForEach-Object {
            Emit ('{0,5}: {1}' -f $_.LineNumber, $_.Line.Trim())
        }
    } else {
        Emit '(no WalkerCore lines found - DLL not loaded or wrong profile dir)'
    }
    Emit ''
    Emit 'Per-frame event histogram:'
    $counts = @{}
    Select-String -Path $log -Pattern '\[WalkerCore\]|MH_CreateHook.*failed|screenshot' -SimpleMatch:$false -EA SilentlyContinue | ForEach-Object {
        $line = $_.Line
        if     ($line -match 'D3D11Hook::Install')                                     { $counts['D3D11 install success'] = ($counts['D3D11 install success'] + 1) }
        elseif ($line -match 'D3D12Hook::Install: hooked D3D12')                       { $counts['D3D12 install success (D3D12CreateDevice)'] = ($counts['D3D12 install success (D3D12CreateDevice)'] + 1) }
        elseif ($line -match 'D3D12Hook::Install: hooked CreateDXGIFactory1')          { $counts['D3D12 install success (CreateDXGIFactory1)'] = ($counts['D3D12 install success (CreateDXGIFactory1)'] + 1) }
        elseif ($line -match 'MH_CreateHook on CreateSwapChain failed')               { $counts['CreateSwapChain patch FAILED (vtable slot 10)'] = ($counts['CreateSwapChain patch FAILED (vtable slot 10)'] + 1) }
        elseif ($line -match 'game build matched')                                     { $counts['game build matched (version table)'] = ($counts['game build matched (version table)'] + 1) }
        elseif ($line -match 'game build not in version table')                        { $counts['game build NOT in version table'] = ($counts['game build NOT in version table'] + 1) }
        elseif ($line -match 'Keyboard hook live')                                     { $counts['InputHook live'] = ($counts['InputHook live'] + 1) }
        elseif ($line -match 'M2 lander: D3D11 hook active')                           { $counts['M2 lander: D3D11 hook active'] = ($counts['M2 lander: D3D11 hook active'] + 1) }
        elseif ($line -match 'M2 lander: D3D12')                                       { $counts['M2 lander: D3D12 factory hook active'] = ($counts['M2 lander: D3D12 factory hook active'] + 1) }
        elseif ($line -match 'frame loop \+ FSM')                                      { $counts['frame loop + FSM + all live'] = ($counts['frame loop + FSM + all live'] + 1) }
        elseif ($line -match ': screenshot$')                                          { $counts['in-game screenshot fired (NOT our dump)'] = ($counts['in-game screenshot fired (NOT our dump)'] + 1) }
    }
    if ($counts.Count -gt 0) {
        $counts.GetEnumerator() | Sort-Object Name | ForEach-Object {
            Emit ('  {0,-58} {1,5}' -f $_.Name, $_.Value)
        }
    } else {
        Emit '  (no events to histogram)'
    }
} else {
    Emit ('game.log.txt NOT FOUND at ' + $log)
    Emit '  (check Documents\My Games\American Truck Simulator\game.log.txt as alternate path,'
    Emit '   or check the OneDrive redirect if your Documents is in OneDrive.)'
}

Section '5. WalkerCore.ini (full content)'
$ini = Join-Path $RoamingDir 'WalkerCore.ini'
if (Test-Path $ini) {
    Emit ('Path: ' + $ini)
    Emit '----'
    Get-Content -LiteralPath $ini -EA SilentlyContinue | ForEach-Object { Emit $_ }
    Emit '----'
} else {
    Emit ('WalkerCore.ini not found at ' + $ini)
}

Section '6. Last 10 user-action lines (screenshot / world interactions)'
if (Test-Path $log) {
    Emit 'Filtered events from the bottom of game.log.txt:'
    Select-String -Path $log -Pattern 'screenshot|save completed|used_vehicles|joined|Hired|disconnected|game save' -SimpleMatch:$false -EA SilentlyContinue |
        Select-Object -Last 15 |
        ForEach-Object { Emit ('  ' + $_.Line.Trim()) }
}

Section 'Done'
Emit ''
Emit ('Output written to: ' + $logFile)
Emit 'Paste that file (or the console block above) back to the assistant.'
Emit ''
Emit 'Quick interpretation guide:'
Emit '  * D3D12 install success >= 1  AND  CreateSwapChain patch FAILED >= 1'
Emit '      -> ReShade/RenoDX are loaded BEFORE us and occupy the IDXGIFactory vtable slot 10.'
Emit '         Options:'
Emit '         (a) disable renodx addons + ReShade dxgi.dll shim for this session to confirm'
Emit '             the rest of the plugin is wired; then ship a coexistence fix.'
Emit '         (b) ship an IAT hook on dxgi!CreateSwapChain so we attach even when the'
Emit '             factory vtable is unreachable.'
Emit '         (c) hook the d3d12.dll command-queue level instead of the factory vtable.'
Emit '  * InputHook live >= 1  AND  no FSM transitions logged'
Emit '      -> per-frame tick is not running (see D3D12 hook chain state above).'
Emit '  * in-game screenshot fired rising each time you press F12'
Emit '      -> expected. F12 collides with ATS built-in screenshot. Rebind dump_state_vk'
Emit '         to 122 (VK_F11) or 45 (VK_INSERT) in WalkerCore.ini.'
