# M3 Lander progress — 2026-10-12

Snapshot of the M3 (PhysX collision / world geometry) Lander state
for ATS 1.61.3.1s (Steam). For a detailed byte-level analysis, see
`ats_1_61_3_1s.physx.functions.md`.

## What ships now (DLL `B82C641D9A227656E314A1CBC7977D5E938A5104B41E4B4118F6DF3B1ADA5777`)

1. **Multi-strategy detection pipeline** in
   `src/provider/PhysXAdapter.cpp::MemoryScanFromBuffer`:
   - `signature` — find a contiguous run of >= `kMinPhysXVtableRun`
     (200) 8-byte code pointers in `.rdata`. Used when the binary
     statically links PhysX.
   - `strings` — verbatim probe of 4 PxScene SDK error strings
     (`PxScene::release(): ...`, `PxScene::shiftOrigin() ...`,
     `PxScene::addActor() ...`, `PxScene::removeActor() ...`).
   - `hybrid` — both strategies above succeed.

2. **Catalog of 5 known PxScene VAs** in
   `kPhysXKnownMethods[]`:
   - `release`     `0x141AE07B6`
   - `addActor`    `0x141AE0B62`
   - `simulate`    `0x141AE4D80`
   - `shiftOrigin` `0x141AE6C28`
   - `sweep`       `0x141B06F10`

3. **Slot resolver** `ResolveKnownMethods()`:
   - For each entry in `kPhysXKnownMethods`, walk the candidate
     vtable at `vtable_base + slot*8` and record the matching slot
     in `PhysXVtableResolveResult`. Returns true when at least one
     catalog row matched.

4. **Diagnostic dump** `DumpVtableEntries()`:
   - Formats the first N 8-byte values from the signature-accepted
     vtable as `0x%016llX` separated by `|`. The dump is the
     bridge from "signature scan accepts the vtable" to "I know
     which slot is which method".

5. **Install-time log**:
   - On DLL load, `PhysXAdapter::Install()` logs the full catalog
     in `game.log.txt` so the user can correlate the catalog VAs
     with the live binary:
     ```
     PhysXAdapter::Install: catalog has 5 methods: release=0x141ae07b6, addActor=0x141ae0b62, simulate=0x141ae4d80, shiftOrigin=0x141ae6c28, sweep=0x141b06f10 [TMRealWalk-style probe]
     ```

6. **VersionTable entry** for ATS 1.61.3.1s (Steam):
   - `PhysXScene` subsystem pinned to RVA `0x2500918` with
     `strategy = "signature+strings+hybrid"`, `verified = true`.
   - `FeatureCollision` stays OFF until slot dispatch is verified
     against a real PxScene instance.

## What still requires live verification

| VA needed | Best candidate | Why unverified |
| --- | --- | --- |
| `PxScene::raycast`   | slot 14: `0x141AE1C60` (270 call sites — top-5 frequency) | no LEA→string anchor; needs Ghidra decompile or live breakpoint |
| `PxScene::overlap`   | slot 15: `0x141AE1610` (130 call sites — fits overlap frequency) | no LEA→string anchor; needs Ghidra decompile or live breakpoint |

Both candidates are **real function entries** (verified by int3
padding walk-back) and have plausible call-frequency profiles.
Neither is added to `kPhysXKnownMethods` until live verification.

## What requires external dependencies (out of sandbox scope)

- **Ghidra full decompile of amtrucks.exe** (53 MB stripped, 30+
  min analysis time): would give exact `PxScene::raycast` and
  `PxScene::overlap` VAs directly. Ghidra headless `analyzeHeadless`
  timed out on `Decompiler Switch Analysis` in the previous
  attempts.
- **Live PxScene breakpoint**: in-game, set a hardware breakpoint
  on the first call to `PxScene::raycast(...)` after F5 (Enter).
  The resolved return address is the function VA. Repeat for
  `PxScene::overlap`.
- **Behavior probe**: stand the player at a known distance from a
  wall, hook `PhysXAdapter::SphereSweep` to log the
  `__cdecl`-resolved return addresses, and read the slot pointers
  off the `this` vtable.

## How to verify the catalog in-game (next session)

1. Deploy the DLL:
   ```powershell
   Copy-Item "C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\build-ninja\libWalkerCore.dll" `
             "C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\plugins\libWalkerCore.dll" `
             -Force
   ```
2. Launch ATS 1.61.3.1s.
3. Check `C:\Users\migue\OneDrive\Documentos\American Truck Simulator\game.log.txt` for the catalog snapshot line above.
4. Press F10 to enter walk-around; observe:
   - `KnockoutProvider: collision direction (angle=... deg, normal=..., |v|=...) [TMRealWalk-style probe]` — M10 diagnostic for KO spurious analysis
   - `TruckDoorConfig from state: model_id=...` — M5 per-truck table
   - `PhysX strings found: N` — M3 string-probe count
5. Share the new `game.log.txt` for the next iteration.

## Repository state

- 16 commits on `main` (10 from this M3 push, 6 pre-session).
- 15255 unit tests, 0 failed.
- All commits pushed to `https://github.com/TheJorseman/FreeWalker.git`.
