# WalkerCore — Diagnostic Dumps Reference (F12 + programmatic)

WalkerCore ships **14 diagnostic dump functions** exposed via the C ABI.
All of them are reachable through `walker_dump_state()` (the F12 dump)
or programmatically through their dedicated exported names. The dump is
plain-text, single-line per row, ASCII — every sink (game log,
OutputDebugStringW, stderr) renders it legibly.

## Quick index

| Dump function | Module | When to call | Output header |
|---|---|---|---|
| `walker_dump_state` | aggregate | F12 | `--- WalkerCore state dump ---` |
| `walker_dump_settings` | SettingsStore | after editing INI | `--- WalkerCore settings ---` |
| `walker_dump_menu` | SettingsMenu | after Ctrl+F10 toggle | `--- WalkerCore settings menu ---` |
| `walker_dump_version` | VersionTable | first F12 / build verify | `--- WalkerCore version ---` |
| `walker_dump_fsm` | WalkerFsm | while transitioning | `--- WalkerCore fsm ---` |
| `walker_dump_input` | InputHook | key/mouse diagnostics | `--- WalkerCore input ---` |
| `walker_dump_physx` | M3 PhysXAdapter | catalog + slot dispatch | `--- WalkerCore physx ---` |
| `walker_dump_per_truck_door` | M5 PerTruckDoor | truck geometry verify | `--- WalkerCore per-truck door ---` |
| `walker_dump_coupling` | M9 CouplingProvider | trailer-attach step | `--- WalkerCore coupling ---` |
| `walker_dump_knockout` | M10 KnockoutProvider | KO spurious debug | `--- WalkerCore knockout ---` |
| `walker_dump_head_tracker` | M7 HeadTracker | TrackIR/opentrack | `--- WalkerCore head tracker ---` |
| `walker_dump_shadow` | M7 Shadow | sun + ground probe | `--- WalkerCore shadow ---` |
| `walker_dump_flashlight` | M7 Flashlight | G toggle / cone direction | `--- WalkerCore flashlight ---` |
| `walker_dump_door` | M7 DoorAnim | open/close animation | `--- WalkerCore door ---` |

## M3 PhysX — `walker_dump_physx`

Catalog of 7 known method VAs (`kPhysXKnownMethods`) plus the actual
resolved slot from the runtime scan that `PhysXAdapter::Install()`
runs against `GetModuleHandleA("amtrucks.exe")` + the in-memory PE
parser + `MemoryScanFromBufferWithTextRange` + `ResolveKnownMethods`.

Sample (ATS 1.61.3.1s, image base `0x140000000`):
```
--- WalkerCore physx ---
physx installed       : true
capsule radius        : 0.350 m
capsule height        : 1.850 m
max sweep distance    : 1.000 m
max slope angle       : 0.870 rad
disable collisions    : false
physx runtime         : live strategy=hybrid vtable_off=0x25020A0 run=118 string_probes=4
catalog size          : 7
physx[0] release      : va=0x141AE07B6 expected_slot=2   resolved_slot=2
physx[1] addActor     : va=0x141AE0B62 expected_slot=11  resolved_slot=11
physx[2] simulate     : va=0x141AE4D80 expected_slot=57  resolved_slot=57
physx[3] shiftOrigin  : va=0x141AE6C28 expected_slot=117 resolved_slot=117
physx[4] sweep        : va=0x141B06F10 expected_slot=-1  resolved_slot=-1
physx[5] raycast      : va=0x141AE1610 expected_slot=15  resolved_slot=15
physx[6] overlap      : va=0x141AE0F70 expected_slot=13  resolved_slot=13
physx resolve         : tolerance=8 B min_run=100
--- end physx ---
```

If `runtime` is `(no live process)` the DLL is not loaded into
`amtrucks.exe` (e.g. the test runner, or a process where the
`GetModuleHandleA` lookup didn't resolve the module name). The
expected_slot column is hardcoded from offline RE on the 1.61.3.1s
binary (catalog matches at 6 of 7 slots; sweep lives in a separate
5-slot run at RVA 0x2507C08 so the main vtable returns -1 for it
deliberately).

## M5 per-truck door — `walker_dump_per_truck_door`

Resolves the live `truck.placement.id` against the built-in
`PerTruckDoorTable` (14 ATS 1.61 trucks) and reports the resolved
door pivot + cabin extents + seat local. Sample for a Volvo VNL 2016
(`vehicle.volvo.vnl_2016` is one of the 14 built-in rows):
```
--- WalkerCore per-truck door ---
model_id             : vehicle.volvo.vnl_2016 (matched built-in row)
config valid         : true
has_passenger_door   : false
driver_door.position : (-0.95, 1.55, -1.60)
driver_door.anchor   : (-1.45, 0.00, -1.60)
driver_door.yaw_off  : 1.571 rad
cabin_min/max        : (-1.10, 1.00, -1.70) .. (1.10, 2.95, 0.40)
driver_seat_local    : (-0.45, 1.55, -0.50)
door_max_open_rad    : 1.571 rad (90.0 deg)
door_open_duration_s : 0.60
door_activation_dist : 1.50 m
cabin_inside_dist    : 0.40 m
--- end per-truck door ---
```

When the model_id is not in the table, the dump reports
`(no built-in row)` and the geometry falls back to the generic
placeholder (still functional, just not tuned to that truck).

## M9 trailer coupling — `walker_dump_coupling`

Reports the active step in the 5-step coupling sequence and the
truck-local waypoint the player has to reach. Sample for the player
in the Approach step:
```
--- WalkerCore coupling ---
active              : true
step                : ApproachToKingPin (1)
progress            : 0.450
waypoint_local      : (-1.40, 0.00, 2.00)
waypoint_radius_m   : 1.00
--- end coupling ---
```

The step name comes from `CouplingStepName()` (helper added in this
session); the (1) is the underlying enum ordinal for cross-reference.

## M10 knock-out — `walker_dump_knockout`

Reports live KO state + the 4 config thresholds. The
`impact_direction_mag` row is a sanity check on the heuristic's
normalisation — anything wildly off `1.0` is suspicious. Sample for a
real KO in progress:
```
--- WalkerCore knockout ---
knocked_out          : true
time_remaining_s     : 1.20
impact_direction     : (-0.700, 0.000, 0.700)
config vel_change_mps: 25.0
config snap_reprime_m : 1.00
config recovery_s    : 1.50
config fall_height_m : 4.00
impact_direction_mag : 0.995 (expect ≈ 1.0 when normalised)
--- end knockout ---
```

## Common patterns for live debugging

- **"Did the F10 trigger fire?"** → `walker_dump_fsm` shows
  `fsm state : TransitionOut t=0.12` while the transition tween runs.
- **"Did F10 actually move the camera?"** → `walker_dump_state`
  `camera eye            : (..., ..., ...)` should change between
  frames after F10.
- **"Did M3 find the catalog in-process?"** → `walker_dump_physx`
  `physx runtime         : live strategy=hybrid ...`. If it's
  `(no live process)`, the GetModuleHandleA lookup needs a different
  module name (some launchers rename the binary).
- **"Did the per-truck override fire?"** → `walker_dump_per_truck_door`
  `model_id : vehicle.X (matched built-in row)` vs `(no built-in row)`.
- **"Why did I get knocked out?"** → `walker_dump_knockout`
  `impact_direction : (x, y, z)` tells you which direction the
  heuristic thinks the hit came from.

## Where these are wired

- `src/adapter/DiagnosticDump.cpp` — the 14 `walker_dump_*` impls.
- `src/adapter/PluginEntry.cpp` — wires each provider into the
  diagnostic mirror via `diagnostic_internal::SetX(...)` after `Install`.
- `include/walkercore/DiagnosticAccess.h` — the accessors.
- `tests/test_abi_contract.cpp` — pins the exported symbol set so a
  regression that adds or removes an export breaks the build/test.
- `tests/test_diagnostic_dump.cpp` — focused coverage for the
  knockout, per-truck door, and coupling dumps (plus the
  uninitialised-path sanity checks for every dump).