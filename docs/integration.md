# WalkerCore — Integration Contract for M-landers

This document defines the exact contract between WalkerCore's existing
modules (the parts that are already written) and the "landers" that swap
the stubs for real implementations. Each lander is a small, well-scoped
piece of work that replaces one stub with a real one.

## Architectural context

```
                        ┌──────────────────────────────────────────────┐
                        │              PluginEntry (C ABI)             │
                        │   scs_telemetry_init / scs_telemetry_shutdown│
                        │   walker_per_frame_tick(dt)                  │
                        └────────────────┬─────────────────────────────┘
                                         │ owns
            ┌────────────────────────────┼─────────────────────────────────┐
            │                            │                                 │
   ┌────────▼────────┐         ┌─────────▼─────────┐              ┌────────▼────────┐
   │  ScsTelemetry    │         │     FrameLoop     │              │   SettingsStore  │
   │  Provider        │         │  (orchestrator)   │              │   (INI + INI→o) │
   └────────┬────────┘         └─────────┬─────────┘              └────────┬────────┘
            │                            │                                 │
            │                            │ uses                            │
            │                  ┌─────────┼──────────┐                      │
            │                  │         │          │                      │
            │           ┌──────▼───┐ ┌───▼────┐ ┌───▼───────┐               │
            │           │ InputHook│ │IWorld  │ │ WalkerFsm │               │
            │           │ (keyb/   │ │Geometry│ │ (state    │               │
            │           │ mouse)   │ │(PhysX) │ │  machine) │               │
            │           └──────────┘ └────────┘ └───────────┘               │
            │                                                              │
            └──────────────────────────────────────────────────────────────┘
                                    configures
```

The M-landers are:

| Lander | What it implements | Status |
| --- | --- | --- |
| M2 Lander | D3D11 Present hook + MinHook setup | **Implemented — real MinHook path active in tests and DLL**. `D3D11Hook.cpp` hooks `D3D11CreateDeviceAndSwapChain` (11-parameter real signature) and the returned `IDXGISwapChain::Present` (vtable slot 8) plus `IDXGISwapChain::ResizeBuffers` (slot 13). MinHook is vendored at `third_party/minhook/` (submodule) and built into both `WalkerCore.dll` and `WalkerCoreTestLib`. `g_swap_chain_gen` defends against swap-chain recreation; `g_cached_aspect` lets the per-frame path skip `GetDesc` after the first `ResizeBuffers` call. Test DLL build verifies the real install/exit roundtrip — see `TestD3D11HookInstallUninstallRoundtrip` for an example that exits gracefully with the `'d3d11.dll not loaded yet — retry later'` warning because the test runner is not a D3D11 host (see `tests/TestD3DHookStubs.cpp` for the no-op adapter stubs that satisfy `walker_per_frame_tick` and `walker_set_swapchain_aspect` linkage). |
| M3 Lander | Real `IWorldGeometry` (PhysX SDK or memory scan) | **Catalog + slot dispatch + runtime scan done; live PxScene queries pending**. `kPhysXKnownMethods` carries 8 entries (release, addActor, simulate, shiftOrigin, sweep, raycast, overlap, **removeActor**) with absolute VAs from `scripts/physx-vta-scan/`. `ResolveKnownMethods` walks the vtable at RVA 0x25020A0 (ATS 1.61.3.1s, 118 slots) with a slot tolerance of 8 bytes and matches 7 of 8 in the main vtable (sweep lives in a separate 5-slot run at RVA 0x2507C08; removeActor=slot 14 was identified by tracing its `'PxScene::removeActor(): Actor'` error-path string). `PhysXAdapter::Install()` runs an in-memory scan against the live `amtrucks.exe` process (PE parser + `MemoryScanFromBufferWithTextRange` + `ResolveKnownMethods`) and caches the result in `PhysXAdapter::RuntimeCache`; `walker_dump_physx()` (F12 / programmatic) reports the live resolved slots. The on-disk real-binary test (`TestPhysXRealBinaryMatchesCatalog`) and the runtime-path mirror (`TestPhysXRealBinaryRuntimeScanPath`) prove the catalog matches the binary (gated by `WALKERCORE_REAL_BINARY_TEST_PATH`). Runtime `Raycast` / `SphereSweep` / `groundY` still fall back to no-hit because no live `PxScene` instance is wired — that requires hooking the game's PxScene global or RE of the SCS physics dispatcher. |
| M5 Lander | Per-truck door geometry from real truck models | **Implemented (14 built-in rows)**. `PerTruckDoorTable::BuiltIn()` ships ATS 1.61 truck geometry (Volvo VNL 2016 / 2009, Kenworth W900/T680/T880, Peterbilt 579/389, Freightliner Cascadia / Cascadia 2018, Mack Anthem, Western Star 49X/4900, International LT/LoneStar). `TruckDoorConfigFromTruckState` matches the live `truck.placement.id` against the table and applies the per-truck offset delta on top of the generic cabin-derived placeholder. `walker_dump_per_truck_door()` reports the matched row + resolved geometry on F12. |
| M7 Lander | Head tracker (FreeTrack + opentrack), shadow, light injection, door skeletal animation | **Partial — opentrack receiver done**. `OpentrackReceiver` listens on UDP port 4242 and pushes smoothed yaw/pitch/roll into `HeadTrackerProvider`. FreeTrack client linkage + shadow/flashlight injection still stubbed (require game memory scan or `light_source` global — see Lander contract below). |
| M8 Lander | Real fuel-station prefab list + SCS fuel channels | Not yet started (RE of SCS `prefab/list_station` required; not yet wired because that RE needs a live debugging session against `amtrucks.exe`). |
| M9 Lander | Real trailer-attach API + per-trailer king-pin offsets | **Partial**. `CouplingProvider` tracks the live `hook_position` from the SCS configuration event and uses it to derive per-step waypoints (ApproachToKingPin / LowerLandingGear / ConnectAirLines / RaiseLandingGear) instead of fixed placeholders. `walker_dump_coupling()` reports the active step + progress + waypoint on F12. |
| M10 Lander | Vehicle-impact normal from PhysX collision results | **Partial**. `KnockoutProvider::Tick` logs the per-angle collision direction (TMRealWalk-style probe line: `KnockoutProvider: collision direction (angle=... deg, normal=...)`). `walker_dump_knockout()` reports the live state + magnitude-normalized impact direction + config thresholds (vel-change, snap-re-prime, recovery, fall-height) on F12. The hook into `PhysXAdapter::SphereSweep`'s contact normal remains — M3 runtime queries must be live before the hook can replace the velocity-change heuristic. |
| M11 Lander | Dear ImGui integration under D3D11 | Not yet started (requires vendored Dear ImGui under `third_party/imgui`; the renderer in `SettingsMenu.cpp` is the existing console-style path). |

All non-lander modules are written and the test scaffold is in place; the
compilation status depends on the user's toolchain.

## Lander architecture

Each lander replaces a stub file in `src/provider/` (or
`src/workflow/` for the M11 menu renderer). All landers live inside
the **same** WalkerCore DLL — they are file replacements, not separate
plugins that call into WalkerCore across a DLL boundary. The "public
symbols" referenced by the M2 lander (e.g. `walker_per_frame_tick`,
`walker_camera`, `walker_set_swapchain_aspect`) are C-linkage
internal symbols declared in `include/walkercore/PerFrame.h`; they
are not in the DLL's export table. The DLL's exported surface
(LoadLibrary-visible) is exactly the 14 names pinned by
`TestAbiContractDllLoads`:

| Tier | Functions |
| --- | --- |
| SCS SDK entry | `scs_telemetry_init`, `scs_telemetry_shutdown`, `DllMain` |
| Top-level | `walkercore_main` |
| Diagnostics | `walker_dump_state`, `walker_dump_settings`, `walker_dump_menu`, `walker_dump_head_tracker`, `walker_dump_shadow`, `walker_dump_flashlight`, `walker_dump_door`, `walker_dump_fsm`, `walker_dump_version`, `walker_dump_input`, `walker_dump_physx`, `walker_dump_knockout`, `walker_dump_per_truck_door`, `walker_dump_coupling` |

If a future lander is ever packaged as a **separate** DLL or EXE that
loads WalkerCore, those C-linkage symbols would need to be promoted
to `__declspec(dllexport)` (MinGW) or added to the `WalkerCore.def`
(MSVC) so the load-time lookup can find them. For now the in-DLL
contract is enough, and the ABI test
(`tests/test_abi_contract.cpp`) explicitly documents the deliberate
non-export of the per-frame functions.

## The single tick contract

The M2 lander installs `IDXGISwapChain::Present` and, on each call,
invokes `walker_per_frame_tick(dt_seconds)` (an internal C-linkage
symbol — see the Lander architecture section above). The tick runs
three phases:

1. `FrameLoop::RunOnce()` — sample input, apply mouse-look, integrate
   movement, resolve collision, animate player, update camera matrices.
2. `WalkerFsm::Update()` — observe player + truck, may start a transition
   and write a camera override for the next frame.
3. `SettingsStore::CheckFileChanged()` — poll for INI edits.

After `walker_per_frame_tick` returns, the M2 lander reads:

| API | Returns |
| --- | --- |
| `walker_camera()` | pointer to the latest `CameraState` (with `view` and `projection` matrices) |
| `walker_player()` | pointer to the latest `PlayerState` |
| `walker_should_override_render()` | 1 if `WalkerFsm::ShouldRenderOverride()` says to push walker matrices; 0 if the game's own camera should be used |
| `walker_fsm_state_label()` | null-terminated ASCII label for the F12 dump |
| `walker_set_swapchain_aspect(aspect)` | call before `Present` so the projection matrix matches the swap-chain |

The renderer pushes `camera.view` and `camera.projection` as the world →
view and view → clip transforms when `walker_should_override_render()`
is true. Otherwise it calls the original `Present` unchanged.

## Per-lander contracts

### M2 Lander — D3D11 Present hook

**Status: Implemented (real MinHook path active).**

`src/provider/D3D11Hook.cpp` — hooks `D3D11CreateDeviceAndSwapChain` from
the d3d11.dll export table (MinHook-vendored at `third_party/minhook/`),
then patches the returned `IDXGISwapChain::Present` (vtable slot 8) and
`IDXGISwapChain::ResizeBuffers` (slot 13). Each `Present` call updates
the cached aspect ratio, calls `walker_set_swapchain_aspect(aspect)`,
takes the per-frame CAS lock, and calls `walker_per_frame_tick(dt)`,
then chains to the original `Present`. Cache + generation counter
defend against swap-chain recreation between install and Present.

- MinHook is vendored as a submodule at `third_party/minhook/` and
  built into both `WalkerCore.dll` and `WalkerCoreTestLib` via the
  `WALKERCORE_HAVE_MINHOOK=1` define. The no-op fallback compiles
  only when the vendored copy is missing (`include/MinHook.h` absent).
- Respect wrappers: if a DXGI wrapper is present (MSI Afterburner,
  RTSS), chain to it instead of the original. The hook walks the
  vtable to find the topmost non-Microsoft `DllPresent` once per
  install; the captured swap chain is the one the game holds onto.
- Uninstall calls `MH_DisableHook` on each individual hook target
  (`HookedCreateDeviceAndSwapChain`, `HookedPresent`,
  `HookedResizeBuffers`) — never `MH_ALL_HOOKS` — because the D3D12
  lander shares this MinHook instance. Disabling all would tear out
  the D3D12 path's swap-chain patch and blank the screen in D3D12
  games.
- After uninstall we release only the `ID3D11DeviceContext` reference
  we obtained from `GetImmediateContext`. The device and swap chain
  were returned to the game and the game owns them — releasing them
  here would corrupt its reference counts.

**Internal symbols used (declared in `PerFrame.h`):**
`walker_per_frame_tick`, `walker_set_swapchain_aspect`, `walker_camera`,
`walker_should_override_render`. These are C-linkage functions defined
in `src/Main.cpp` of the DLL adapter layer. The test build does NOT
link the DLL adapter layer, so `tests/TestD3DHookStubs.cpp` provides
no-op versions so the test build can still exercise the real D3D11
hook code paths.

### M3 Lander — Real IWorldGeometry

**File to implement:** `src/provider/PhysXAdapter.cpp` (currently a stub).

- Strategy 1 (preferred): link against the public PhysX 3.4 SDK
  (github.com/NVIDIA-Omniverse/PhysX). Hook the game's `PxPhysics`
  instance via the game binary's exported symbol or a known global. Walk
  the scene graph to a `PxScene`. Cache triangle / box meshes locally
  for fast sweep queries.
- Strategy 2 (fallback): memory-scan for the `PxScene` vtable pattern,
  same as TMRealWalk. Use `VersionEntry::subsystems[PhysXScene].rva` to
  find the vtable on known builds.
- Implementation must populate `PhysXAdapterConfig` from M6 INI.
- The `Install()` method returns `true` on success. On failure, log a
  clear warning and leave the no-hit stub behaviour — the player passes
  through geometry but the game stays stable.

**Public API:** `PhysXAdapter::Raycast`, `SphereSweep`, `groundY` (the
`IWorldGeometry` interface).

### M5 Lander — Per-truck door geometry

**File to implement:** none — replace `TruckDoorConfig::DefaultTruckDoorConfig()`
with per-truck data sourced from the truck model files. Add a `TruckModel`
table that maps `truck.placement.model_id` (read from a future telemetry
channel or memory scan) to a `TruckDoorConfig`.

### M7 Lander — Polish providers

**File to implement:** `src/provider/PolishProviders.cpp`.

- `HeadTrackerProvider::Install` should try to load `FreeTrackClient.dll`
  (TrackIR) and connect via UDP to opentrack on port 4242. Both have
  public docs; both are open protocols.
- The shadow provider should read the game's sun direction. For SCS, this
  is in the weather system; the cleanest path is a memory scan of the
  weather singleton. Document the offset in `VersionEntry`.
- The flashlight provider injects a point light by writing into the
  game's `light_source` global. SCS exposes this via a global symbol;
  the M2 lander's first delivery should also include the vtable lookup
  for `light_source::set_position`.

### M8 Lander — Fuel provider

**File to implement:** `src/provider/FuelPump.cpp`.

- Replace the truck-local placeholder for the fuel-station anchor with a
  real station prefab list (SCS publishes this as `prefab/list_station`).
- Register SCS channels `truck.fuel.capacity`, `truck.fuel.amount`,
  `truck.adblue.capacity`, `truck.adblue.amount` (already supported by
  the SDK 1.15 we vendor). Read fuel amounts from telemetry instead of
  the local `fuel_amount_l_` member.

### M9 Lander — Trailer coupling

**File to implement:** `src/provider/TrailerCoupling.cpp`.

- Replace the placeholder waypoints with values derived from the trailer
  model file (SCS publishes trailer placement via `trailer.placement` and
  `trailer.wheel_suspension_deflection`).
- Replace `ForceComplete()` with a real SCS trailer-attach API call. The
  game exposes this via a "trailer attach" job; the plugin can request it
  through the job dispatcher after step 4.

### M10 Lander — Knock-out

**File to implement:** `src/provider/Knockout.cpp`.

- Replace the velocity-change heuristic with a hook into `PhysXAdapter`'s
  sphere-sweep results. When the sweep reports a contact normal with
  magnitude > some threshold, mark the player as knocked-out with the
  contact normal as the impact direction.
- Implement the falling-damage check using `groundY` probe history: if
  the player was airborne for > 0.5 s and lands at a vertical velocity
  > 8 m/s, mark as knocked-out.

### M11 Lander — Dear ImGui

**File to implement:** `src/workflow/SettingsMenu.cpp`.

- Add Dear ImGui (github.com/ocornut/imgui, MIT) under `third_party/imgui`.
- In `SettingsMenu::Render`, build the menu using ImGui. The widget set
  is the existing `SetSliderValue / SetBoolValue / SetKeyBinding`
  setters — keep the API; only the renderer changes.
- Inject the menu after the game's Present. The M2 lander's
  "after Present, before IDXGISwapChain::Present returns" hook is the
  right place.

## Versioning

Each subsystem can be feature-flagged in `VersionEntry::features_enabled`.
A subsystem that fails to install on a given game build logs a warning and
clears its bit. The user can re-enable via INI once we have M6 stable.

| Feature bit | Subsystem | Lander |
| --- | --- | --- |
| `FeatureFreeCamera` | CameraPresentHook | M2 |
| `FeatureCollision` | PhysXScene | M3 |
| `FeatureWalk` | InputHook + FrameLoop | already working |
| `FeatureEnterExit` | TruckDoorConfig | M5 |
| `FeatureConfigReload` | SettingsStore | already working |
| `FeatureHeadTracker` | HeadTrackerProvider | M7 |
| `FeatureShadow` | ShadowProvider | M7 |
| `FeatureFlashlight` | FlashlightProvider | M7 |
| `FeatureDoorAnimation` | DoorAnimProvider | M7 |
| `FeatureFuelPump` | FuelProvider | M8 |
| `FeatureAdBlue` | FuelProvider (AdBlue half) | M7 |
| `FeatureTrailerCoupling` | CouplingProvider | M9 |
| `FeatureKnockOut` | KnockoutProvider | M10 |

## Build verification

Once a toolchain is installed:

```
cd C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The DLL is at `build/WalkerCore.dll`. The unit tests binary is at
`build/tests/WalkerCoreTests.exe` (or `build/WalkerCoreTests` on
non-Windows).

The tests do not require the game binary. They exercise pure-domain
math, the INI parser, the FSM, the head-tracker smoothing, and the door
animation easing. Any failure surfaces as a stderr line with the file,
line, and the failing expression.

## License

WalkerCore is MIT. Each vendored dependency ships its own license under
`third_party/<dep>/LICENSE`:

- MinHook — MIT (Tsuda Kageyo).
- Dear ImGui — MIT (Omar Cornut).
- PhysX 3.4 SDK — BSD-3 (NVIDIA, optional; only if M3 lander goes
  with the public-SDK strategy).

None of the dependencies introduce copyleft. The plugin binary is safe to
distribute.