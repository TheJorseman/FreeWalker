# TMRealWalk — reference architecture study (v1.0.0-beta4.5)

Side-by-side analysis of `TMRealWalk.dll` (14.08 MB, SHA256
`5C2DECFB41859A80EB89C133E8EC58BDC24696BD7D89E45C24220D85ADF014A8`) against the
WalkerCore M-lander contracts. This document does **not** copy code or
reverse-engineer proprietary logic. It is the publicly-observable string
inventory + section layout that informs our M3 (PhysX collision) and
M2/M11 (free-camera + UI) design decisions.

Source files:

| File | SHA256 | Size |
| --- | --- | --- |
| `TMRealWalk.dll` | `5C2DECFB41859A80EB89C133E8EC58BDC24696BD7D89E45C24220D85ADF014A8` | 14 081 648 bytes |
| `TMRealWalk.ini` | (live-reloaded) | 25 872 bytes |
| `README.txt` | (docs) | 11 700 bytes |

The DLL exposes imports against:

- `d3d11.dll`, `D3DCOMPILER_47.dll` (DX11 chain)
- `WINHTTP.dll`, `CRYPT32.dll`, `bcrypt.dll` (online licence + 7-day offline)
- `USER32.dll`, `WINMM.dll`, `GDI32.dll`, `SHLWAPI.dll`, `gdiplus.dll` (UI + input)
- `KERNEL32.dll`, `ole32.dll`

Our minimal WalkerCore set (`d3d11`, `dxgi`, `ws2_32` for M7) is a clean
subset.

## Collision world (`[physx]` strings)

The DLL publishes its collision-strategy outcome to `game.log.txt` via the
prefixed lines below. They reveal a three-stage fallback we can mirror
in M3:

```
[physx] scene strings not found - ground/collision unavailable
[physx] scene functions not found - ground/collision unavailable
[physx] scene vtable not found - ground/collision unavailable
[physx] scene vtable = +0x%llX           <-- vtable base when found
[physx] scene search read %llu MB of %llu MB committed (resident pages only) in %llu ms
[physx] no .text section                 <-- portability guard
```

The four literal strings the scanner searches for in `amtrucks.exe`'s
`.rdata` are themselves observable in the DLL's data section (at string
offsets `0xC8060`..`0xC80FF`):

| RVA sample string | Purpose |
| --- | --- |
| `PxScene::release(): Scene is still being simulated` | Identifies `PxScene::release` slot (idx 0) |
| `PxScene::shiftOrigin() not allowed` | Identifies `PxScene::shiftOrigin` slot |

A third diagnostic line worth using in our M3 Lander:

```
[physx]   angle %3.0f: %s%.2f%c normal=(%.2f,%.2f,%.2f)
[physx] dynamic bodies near the truck (truck telemetry world=(%.2f,%.2f,%.2f))
```

The first reports per-angle collision normals; the second prints the truck
telemetry placement used as the search anchor. We can mirror both as
runtime probes once the vtable is resolved.

## Pose probe (`[pose-probe]`)

```
[pose-probe] looking for placement blocks within %.2f m of chunk (%d,%d) local (%.2f, %.2f, %.2f) ...
```

TMRealWalk falls back to **placement blocks** (game-side data already in
memory) for *far* ground queries where a PhysX raycast would be too
expensive. The chunk coordinates `(chunk_x, chunk_y)` come from the game's
streamed world. The radius and offset thresholds are user-tunable
(`walker.far_ground_offset`).

This matches our M3 Lander idea of using `truck.world.placement` as a
ground anchor instead of pure PhysX — a screenshot from the live `game.log.txt`
showed:

- `[physx] dynamic bodies near the truck (truck telemetry world=(%.2f,%.2f,%.2f)):`

So TMRealWalk ties its collision queries to `truck.world.placement`. Our
M9 Lander already registers `trailer.world.placement`; M3 should consume
the same channel.

## Camera + free-camera path

```
[camera] expected exactly one singleton store in the manager creator, found %d
[camera] manager singleton pointer at exe+0x%llX (creator exe+0x%llX)
[camera] watching active %s (object %p) - baseline (w,x,y,z) = (%g,%g,%g,%g) - look around now (mouse or head tracker)
[camera] g_developer found at exe+0x%llX
[camera] g_developer was 0: set to 1 in memory for the free camera (config.cfg unchanged)
[camera] g_developer was not found: if walking does not start, set uset g_developer "1" in config.cfg
```

The key takeaway: TMRealWalk **patches `g_developer` in memory to `1`** so
the game activates its own free camera instead of us hooking D3D11 to draw
a separate camera. The same string is what the user has to set in
`config.cfg` manually. TMRealWalk also reports its address so it can be
verified (`uset g_developer "1"` at `documents/<game>/config.cfg`).

The singleton-creator pattern they use is a known compile-time idiom in
SCS engine code:

```cpp
// Reduce boilerplate at every call-site:
T* f() { return (g_singleton = new T())->m_self; }
```

A scanner finds the `MOV [...]` that writes the singleton into a global,
and tracks it back to find the live pointer. We currently drive our own
D3D11 hook (M2 lander), so for our purposes this is a **fallback path**
that we can document but do not need to ship.

The baseline quaternion `(w,x,y,z)` they capture before the user is
allowed to "look around" is the same idea as our
`WalkerFsm::TransitionToWalking` snapshot — make the walker-mode camera
start exactly where the cabin camera was when the user pressed F10.

## Cabin exit thresholds

```
[cabin-exit] stopped+handbraked - baseline %s (w=%g x=%g y=%g z=%g)
[cabin-exit] speed=%.2f brake=%d yaw=%.1f side=%.0f threshold=%.0f -> %s
[cabin-exit] cannot read: rotation=%s side=%s
```

They require **stopped + handbrake** to enable cabin exit. Our
`WalkerFsm::IsExitAllowed` only checks handbrake. We should mirror their
`speed_threshold` (configurable in INI as `walker.cabin_exit_speed_threshold`)
and `yaw_threshold` (`walker.cabin_exit_yaw_threshold`) in the per-truck
datafile.

## Door geometry

```
walker.door_outward      (INI: outward offset, m)
walker.door_hop_distance (INI: 1.5 m default)
walker.door_shake_cabin  (INI: shake cabin on open)
walker.door_shake_cabin_strength
```

Our M5 Lander uses `cabin_position` as the door anchor and
`head_position` as the seat anchor. TMRealWalk's `walker.door_outward` is
a *truck-local* outward offset (in the door's swing plane). Our
`TruckDoorConfigFromTruckState` produces driver door = `cabin − (0.45,
0.55, 0.40)` which is approximately the same magnitude as TMRealWalk's
default. For trucks with cab-over engines the door pivot is in front of
the cabin (e.g. cabin + (0.5, 0.55, 0)) — we should add a `model_id →
door_outward_override` row in our per-truck data file.

## Knock-out vehicle impact

```
[knock] the flashlight is knocked out of the hand
knock.fall_height      (INI: 4 m default)
```

TMRealWalk detects vehicle impacts by scanning dynamic bodies near the
walker (see `[physx] dynamic bodies near the truck (truck telemetry
world=(...))`), which matches our M10 Lander plan once M3 vtable dispatch
is in place. The `fall_height = 4 m` line is the same threshold we ship
for our `KnockoutProvider`.

## Trailer attach (M9 reference)

```
[trailer] the game's attach key: 0x%02X (keyboard.%s in the game's controls)
[trailer] %s: pressing the attach key (0x%02X), kingpin %.2f m from the fifth wheel
[trailer] back in the cabin (unseen)
[trailer] the trailer is hitched but its coupling is not finished (link state 3)
[trailer] the trailer is hitched but ...
```

They probe **distance from kingpin to fifth wheel** and decide which step
prompt to show (1: lower landing gear, 2: detach hoses, 3: pull pin).

Our M9 Lander already tracks `trailer.world.placement` and a placeholder
hook offset. When the user enables the per-truck datafile, we should add
a row `model_id → kingpin_to_fifthwheel_offset` so the UI prompts can
match TMRealWalk's behaviour.

## Mouse input

```
[mouse] first raw mouse packet received
[mouse] could not create the input window (%lu)
[mouse] raw input registration failed (%lu)
```

TMRealWalk creates a hidden window to receive raw mouse input because the
game window may not deliver mouse events when the cursor is unlocked.
Our `InputHook` uses keyboard hook + raw-input; we can stay with the
simpler approach until we see reports of missing mouse look in the game
log.

## Walk tuning surface (INI excerpt)

```
walker.walk_speed = 1.75
walker.sprint_multiplier = 2.6
walker.acceleration_time = 0.15
walker.jump_speed = 4.8
walker.gravity = 9.81
walker.max_climb_height = 1.3
walker.climb_duration = 0.55
walker.climb_slide_extra = 0.35
walker.high_jump_factor = 1.3
walker.step_down_max = 0.6
walker.air_control = 0.25
walker.eye_height = 1.70
walker.crouch_eye_height = 1.00
walker.step_length = 0.93
```

Our defaults in `WalkerCore.ini` should converge on the same numbers — we
already ship `walk_speed = 1.75` and `gravity = 9.81` from the diagnostic
build; M3's `eye_height` should also move to `1.70`.

## What this means for WalkerCore M-landers

| M Lander | New idea to adopt |
| --- | --- |
| M2 camera | Document `g_developer` patching as a documented fallback; ours continues to use the D3D11 hook (no behaviour change). |
| M3 PhysX | Add a **3-strategy** scan: strings → signature → no-match. Each failure is logged so the user knows the failure mode. Adopt the `[physx]   angle: ... normal=(..)` runtime probe for the verified build. |
| M5 door | Per-truck data file should accept `door_outward` and `door_hop_distance` overrides. |
| M7 polish | Head tracker (FreeTrack + opentrack) is exactly the same protocol — our `OpentrackReceiver` matches. FreeTrack client is left for a follow-up Lander (out of sandbox). |
| M9 trailer | Add `kingpin_offset_m` to the per-truck data file. We already publish `trailer.world.placement`; the remaining gap is the offset table. |
| M10 knock-out | Use the truck telemetry world placement as the collision-query anchor (mirrors TMRealWalk's `[physx] dynamic bodies near the truck`). |
| M11 settings | The INI live-reload and the `walk_speed=1.75` defaults are matched. Dear ImGui integration remains the missing piece for the in-game settings window (we already hook D3D11, so the renderer slot is open). |

## What we **do not** copy

- TMRealWalk's per-truck tuning logic is a closed-source implementation;
  we study the **inputs** (knobs) and **outputs** (log lines) but do not
  re-implement their solver.
- The plugin uses WINHTTP + an online licence service; we ship none of
  that. WalkerCore remains GPL-3.0-or-later (rel. desde MIT 2026-10-13),
  offline-friendly.
- TMRealWalk's coupling animation (multi-step tractor + hoses + pin) is
  driven by their own pose sequences; we do not RE the kingpin geometry.
