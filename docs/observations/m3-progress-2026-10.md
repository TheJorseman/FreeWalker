# M3 Lander progress — 2026-10-12

Snapshot of the M3 (PhysX collision / world geometry) Lander state
for ATS 1.61.3.1s (Steam). For a detailed byte-level analysis, see
`ats_1_61_3_1s.physx.functions.md`.

## Catalog

7 entries in `walkercore::kPhysXKnownMethods`:

| # | Method | VA | Slot |
| - | ------ | -- | ---- |
| 0 | `release`      | `0x141AE07B6` | 2   |
| 1 | `addActor`     | `0x141AE0B62` | 11  |
| 2 | `simulate`     | `0x141AE4D80` | 57  |
| 3 | `shiftOrigin`  | `0x141AE6C28` | 117 |
| 4 | `sweep`        | `0x141B06F10` | NOT in 0x25020A0 |
| 5 | `raycast`      | `0x141AE1610` | 15  |
| 6 | `overlap`      | `0x141AE0F70` | 13  |

## PxScene vtable

The PxScene vtable lives at **RVA 0x25020A0** (118 slots). The
earlier "0x2500EA0" reference was a misidentification (that RVA
contains ASCII strings from `PxScene::addActor() ... is not valid`,
not a vtable). The slot 2/11/57/117 catalog matches from the
earlier pass were against the same underlying data, but the
catalog comment should be updated to point at 0x25020A0.

## Resolution

`ResolveKnownMethods` matches all 7 catalog VAs against any
candidate vtable buffer. For ATS 1.61.3.1s the vtable @ 0x25020A0
contains 5 exact slot matches (release/2, addActor/11, simulate/57,
raycast/15, overlap/13) and 1 near-match (shiftOrigin at slot 117,
Δ=-8 bytes). `sweep` does NOT live in this vtable — it lives in a
separate 5-slot run starting at RVA 0x2507C08 (slot 4).

## Slot dispatch

The `PhysXVtableResolveResult` carries all 7 slot fields:
- `release_slot`
- `add_actor_slot`
- `simulate_slot`
- `shift_origin_slot`
- `sweep_slot` (always -1 against 0x25020A0; the live binary has to
  also probe 0x2507C08)
- `raycast_slot`
- `overlap_slot`

## How raycast/overlap were located

These two methods do not emit assertion strings in the binary.
Without a string anchor, the LEA-walk-back chain has no first hop.
The identification used:

1. Disassembled every slot's function in the 0x25020A0 vtable
   (118 functions).
2. Recognized the public-API hit-buffer init pattern by inspecting
   each function's prologue + first 0x100 bytes for the
   `nbMaxTouches` constant:
   - `PxRaycastBuffer::PxRaycastBuffer()` initializes
     `nbMaxTouches = 0x40 = 64`
   - `PxOverlapBuffer::PxOverlapBuffer()` initializes
     `nbMaxTouches = 0x8 = 8`
3. Slot 15 (`PxScene::raycast`) has a 0x490-byte stack frame and
   `nbMaxTouches = 0x40`.
4. Slot 13 (`PxScene::overlap`) is a thin dispatcher
   `xor r9d, r9d; jmp 0x141AE0FD0`; the tail target has a 0x2c0-byte
   stack frame and `nbMaxTouches = 0x8`.

## Next-session verification guide

To confirm the catalog entries against the running game:

1. Deploy DLL (the `Commit 5427136` build) with:
   ```powershell
   Copy-Item "C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\build-ninja\libWalkerCore.dll" "C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\plugins\libWalkerCore.dll" -Force
   ```
2. Start ATS, load into a truck, press F10 to enter walk mode.
3. Read `C:\Users\migue\OneDrive\Documentos\American Truck Simulator\game.log.txt`.
   Expected new lines:
   - `PhysXAdapter::Install: catalog has 7 methods: release VA=0x..., ..., raycast VA=0x..., overlap VA=0x... [TMRealWalk-style probe]`
   - The `KnockoutProvider: collision direction (angle=... deg, normal=(...), |v|=...) [TMRealWalk-style probe]` M10 log line after F10 with the new DLL.
4. Add a `walker_dump_state` (F12) call to `ResolveKnownMethods` against the live amtrucks.exe `.rdata`. This will print `release_slot=2, add_actor_slot=11, simulate_slot=57, shift_origin_slot=117, sweep_slot=-1 (in 0x25020A0), raycast_slot=15, overlap_slot=13`.

## What's still unverified

- **Slot 14 (0x141AE1C60)** — the 5th most-called vtable method in
  the entire binary (241 call sites). It's a thin dispatcher that
  calls `[arg2.vtable+0x30]` then tail-jumps to 0x141AE1CF0 (which
  dispatches on geometry type). It is **NOT** raycast or overlap
  (those are slot 15 and 13 respectively, and the slot-14 prologue
  doesn't match their signatures). The pattern looks like a
  PxScene method that takes an actor or shape — possibly
  `PxScene::somethingWithActorComponent` (e.g. addShape / setActorFlag).
  Ghidra decompile or live breakpoint needed to identify it.

## What's still TBD

- **sweep slot resolution against the live vtable** — sweep's
  catalog VA is correct (matches at slot 4 of a separate 5-slot run
  @ 0x2507C08), but `ResolveKnownMethods` against the main 0x25020A0
  vtable returns `sweep_slot = -1`. Need a second scan pass against
  0x2507C08 to fill this in.
- **Live PxScene verification** — none of the catalog slots have
  been confirmed against a live running PxScene instance. The
  ResolveKnownMethods in-process scan is the next step.
- **Real-binary test** — `tests/test_physx_adapter.cpp` tests use
  synthetic buffers; no test currently reads amtrucks.exe from
  disk and runs the full pipeline against the real `.rdata`.