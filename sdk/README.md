# SCS SDK — vendored

The official SCS Software Telemetry & Input SDK, version 1.15 (stable), downloaded from `https://download.eurotrucksimulator2.com/scs_sdk_1_15.zip`.

## Why we vendor it

WalkerCore depends on the SCS SDK ABI (`scs_telemetry_init` / `scs_telemetry_shutdown`). Vendoring the official headers keeps every contributor and CI run on the same SDK version. The SDK is distributed under the [SCS SDK license](sdk_license.txt) (free use for modding purposes; see terms there).

## Layout

- `include/` — public SDK headers. Add `${CMAKE_SOURCE_DIR}/sdk/include` to `include_directories` and you're done.
- `examples/` — official upstream examples. **Not compiled by WalkerCore.** Useful as reference when implementing new channels or events.
- `readme.txt`, `sdk_license.txt` — original SCS documentation and license.

## Building without the SDK

If you delete `include/`, CMake will fail with a clear message pointing here. We don't bundle a stub header — WalkerCore is meant to track the real ABI.

## Updating the SDK

```powershell
$ErrorActionPreference = 'Stop'
$old = "C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\sdk"
rm -- "$old\scs_sdk_1_XX.zip" 2>$null
Invoke-WebRequest -Uri "https://download.eurotrucksimulator2.com/scs_sdk_1_XX.zip" -OutFile "$old\scs_sdk_1_XX.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
# (extract over the existing include/ folder; the new files replace the old ones)
```

Verify build before committing.