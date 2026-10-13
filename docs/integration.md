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
| M2 Lander | D3D11 Present hook + MinHook setup | **Implemented — real MinHook path active in tests and DLL**. `D3D11Hook.cpp` hooks `D3D11CreateDeviceAndSwapChain` (11-parameter real signature) and the returned `IDXGISwapChain::Present` (vtable slot 8) plus `IDXGISwapChain::ResizeBuffers` (slot 13). MinHook is vendored at `third_party/minhook/` (submodule) and built into both `WalkerCore.dll` and `WalkerCoreTestLib`. `g_swap_chain_gen` defends against swap-chain recreation; `g_cached_aspect` lets the per-frame path skip `GetDesc` after the first `ResizeBuffers` call. Test DLL build verifies the real install/exit roundtrip — see `TestD3D11HookInstallUninstallRoundtrip` for an example that exits gracefully with the `'d3d11.dll not loaded yet — retry later'` warning because the test runner is not a D3D11 host (see `tests/TestD3DHookStubs.cpp` for the no-op adapter stubs that satisfy `walker_per_frame_tick` and `walker_set_swapchain_aspect` linkage). **M2 long-term (renderer hijack):** pre-work landed — function `0x140C22F40` identified as the state-machine dispatcher that calls `vtable[16]` on the camera state struct (per-frame camera update). MVP remains `GameFreeCameraShim` until either (a) runtime singleton resolve or (b) static signature scan is run; see the "M2 long-term — RE pre-work" section below. |
| M3 Lander | Real `IWorldGeometry` (PhysX SDK or memory scan) | **Catalog + slot dispatch + runtime scan done; live PxScene queries pending**. `kPhysXKnownMethods` carries 8 entries (release, addActor, simulate, shiftOrigin, sweep, raycast, overlap, **removeActor**) with absolute VAs from `scripts/physx-vta-scan/`. `ResolveKnownMethods` walks the vtable at RVA 0x25020A0 (ATS 1.61.3.1s, 118 slots) with a slot tolerance of 8 bytes and matches 7 of 8 in the main vtable (sweep lives in a separate 5-slot run at RVA 0x2507C08; removeActor=slot 14 was identified by tracing its `'PxScene::removeActor(): Actor'` error-path string). `PhysXAdapter::Install()` runs an in-memory scan against the live `amtrucks.exe` process (PE parser + `MemoryScanFromBufferWithTextRange` + `ResolveKnownMethods`) and caches the result in `PhysXAdapter::RuntimeCache`; `walker_dump_physx()` (F12 / programmatic) reports the live resolved slots. The on-disk real-binary test (`TestPhysXRealBinaryMatchesCatalog`) and the runtime-path mirror (`TestPhysXRealBinaryRuntimeScanPath`) prove the catalog matches the binary (gated by `WALKERCORE_REAL_BINARY_TEST_PATH`). Runtime `Raycast` / `SphereSweep` / `groundY` still fall back to no-hit because no live `PxScene` instance is wired — that requires hooking the game's PxScene global or RE of the SCS physics dispatcher. |
| M5 Lander | Per-truck door geometry from real truck models | **Implemented (14 built-in rows)**. `PerTruckDoorTable::BuiltIn()` ships ATS 1.61 truck geometry (Volvo VNL 2016 / 2009, Kenworth W900/T680/T880, Peterbilt 579/389, Freightliner Cascadia / Cascadia 2018, Mack Anthem, Western Star 49X/4900, International LT/LoneStar). `TruckDoorConfigFromTruckState` matches the live `truck.placement.id` against the table and applies the per-truck offset delta on top of the generic cabin-derived placeholder. `walker_dump_per_truck_door()` reports the matched row + resolved geometry on F12. |
| M7 Lander | Head tracker (FreeTrack + opentrack), shadow, light injection, door skeletal animation | **Partial — opentrack receiver done**. `OpentrackReceiver` listens on UDP port 4242 and pushes smoothed yaw/pitch/roll into `HeadTrackerProvider`. FreeTrack client linkage + shadow/flashlight injection still stubbed (require game memory scan or `light_source` global — see Lander contract below). |
| M8 Lander | Real fuel-station prefab list + SCS fuel channels | **Partial — per-truck tank capacities from the SCS configuration event**. `ScsTelemetryProvider` parses `fuel.capacity` and `adblue.capacity` (config attributes delivered by `SCS_TELEMETRY_EVENT_configuration` alongside `hook_position`) and stores them in `TruckState::fuel_capacity_l` / `adblue_capacity_l`. `FuelProvider::Tick` replaces the honest-stub 600 L fuel / 50 L AdBlue defaults with the per-truck values so a 380 L day-cab cannot be over-filled. The real station prefab list (the SCS-internal `prefab/list_station` registry) is still RE-blocked — the SCS SDK does not expose a station prefab callback, and locating the game's internal prefab registry requires a live debugger against `amtrucks.exe`. Until that RE lands, the player must park the truck in the placeholder anchor spot (3.5 m truck-left, 1 m rear) to fuel. |
| M9 Lander | Real trailer-attach API + per-trailer king-pin offsets | **Partial**. `CouplingProvider` tracks the live `hook_position` from the SCS configuration event and uses it to derive per-step waypoints (ApproachToKingPin / LowerLandingGear / ConnectAirLines / RaiseLandingGear) instead of fixed placeholders. `walker_dump_coupling()` reports the active step + progress + waypoint on F12. |
| M10 Lander | Vehicle-impact normal from PhysX collision results | **Partial**. `KnockoutProvider::Tick` logs the per-angle collision direction (TMRealWalk-style probe line: `KnockoutProvider: collision direction (angle=... deg, normal=...)`). `walker_dump_knockout()` reports the live state + magnitude-normalized impact direction + config thresholds (vel-change, snap-re-prime, recovery, fall-height) on F12. The hook into `PhysXAdapter::SphereSweep`'s contact normal remains — M3 runtime queries must be live before the hook can replace the velocity-change heuristic. |
| M4 Lander | Mouse-pump + WndProc subclass for input wiring | **Implemented**. `InputHook::OnRawInputData(const RAWINPUT&)` accumulates mouse motion / wheel / LMB+RMB into `last_` (data plane). `InputHook::OnRawInputMessage(LPARAM)` is the production path that calls `GetRawInputData` and forwards. `Sample()` now drains `mouse_dx` / `mouse_dy` / `wheel_delta` after the struct copy so per-frame motion doesn't accumulate across frames. New module `WindowSubclass` (`include/walkercore/WindowSubclass.h` + `src/provider/WindowSubclass.cpp`) installs a single WndProc shim on the game hwnd via `SetWindowLongPtrA + GWLP_WNDPROC`; the shim routes `WM_INPUT` to a registered sink (the InputHook's `OnRawInputMessage` trampoline with the InputHook instance as ctx) AND forwards every input message to `ImGui_ImplWin32_WndProcHandler` (no-op when no ImGui context is alive) AND chains to the original WndProc via `CallWindowProcA` (never swallows). The shim is installed by `ImGuiOverlay::InitializeForD3D11Device` after `ImGui_ImplWin32_Init` + `ImGui_ImplDX11_Init` succeed, uninstalled by `ImGuiOverlay::Shutdown` BEFORE `ImGui_ImplWin32_Shutdown` so the WndProc never forwards to a dead context. `InputHook::Install` registers the sink (safe even before the shim is installed — the sink is stored in module-local state); `InputHook::Uninstall` clears it if a shim is currently live. 27 new unit tests (11 RAWINPUT data plane + 16 WndProc state machine) pin motion / wheel / button edges / absolute-coordinate drop / keyboard raw-input drop / install-uninstall round-trip / sink routing / chain-to-original. Internal module (no ABI surface). |
| M11 Lander | Dear ImGui integration under D3D11 | **D3D11 integration + interactive input landed**. Dear ImGui v1.91 vendored + linked into both DLL and test builds via `imgui_vendor` static lib (`-w` warnings). `WALKERCORE_HAVE_IMGUI=1` set; 6 internal headers (`imgui_internal.h`, `imconfig.h`, `imstb_*`) added. `ImGuiOverlay` class extended with full D3D11 lifecycle (InitializeForD3D11Device / OnDeviceLost / OnDeviceCreated / RenderFrameIfEnabled / Shutdown); `D3D11Hook::HookedCreateDeviceAndSwapChain` calls `OnD3D11DeviceCaptured` (with `FindGameWindowHandle` EnumWindows helper); `HookedPresent` calls `RenderImGuiFrameIfEnabled` after the per-frame tick. M11.2 (commit `5370809`) renders live walker state (camera eye/yaw/pitch/FOV, player feet/mode, FSM state + transition progress). M11.3 (commit `00f3132`) shows the build hash + build date in the overlay header so deploy-side analysts can confirm "yes, this is the DLL I just copied" at a glance. M11.4 (commit `61ebc23`) installs the WndProc subclass (see M4 row) so the overlay is interactive — `ImGui_ImplWin32_WndProcHandler` is in the WndProc chain and the data plane's WM_INPUT pump routes into the same subclass. ABI contract: 21 → 37 total (M11: 3 scaffolding + 6 D3D11 + 9 build state + 1 dump_abi; M11.4 adds no exports — `WindowSubclass` is internal to the DLL). 16+ unit tests pin the lifecycle + version + null-safety + WndProc state machine contracts. |

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

### M2 GPU render push — split into MVP + long-term

**Status: MVP via the `GameFreeCameraShim` (see `include/walkercore/GameFreeCameraShim.h`,
`src/provider/GameFreeCameraShim.cpp`) — usable end-to-end. Long-term
renderer hijack still pending.**

Phase A of the M2 lander owns the D3D11 swap-chain hook (above). Phase
B — actually moving the in-game camera when F10 transitions
Driving → Walking — splits into two paths:

**MVP path (use today): `GameFreeCameraShim`.** Default-off with
**M2.5 smart-default auto-enable**. On every Driving → Walking
transition the shim synthesises a configurable VK press+release (default
`VK_NUMPAD0` = `0x60`; overridable via `[m2] dev_camera_toggle_vk = 0xHH`,
auto-detected from `controls.sii`) so the SCS developer free camera takes
over (assuming `g_developer "1"` is already in the game's `config.cfg` —
which it is on this host, see `game.log.txt:350`). The shim's
`g_developer` probe auto-reads the game's config and only fires keys when
detected; otherwise it logs an advisory instead of synthesising keys.

**Smart-default auto-enable (M2.5).** When three gates all pass, the
shim flips itself from `false` to `true` automatically on DLL load:
  1. The INI did **not** name `[m2] game_free_camera_shim = ...` (the
     `m2_game_free_camera_shim_explicit` flag is false — the user has
     not vetted the value).
  2. A `dev_camera_toggle` / `walk_mode` / `free_camera` binding is
     present in the user's `controls.sii` (auto-detected via
     `ControlsSiiParser` + `ScsKeyNameToVkForTesting` lookup table —
     covers F1-F12, numpad, modifiers, cursor keys, single letters).
  3. `g_developer = 1` (or `2`) is present in `config.cfg`
     (`g_developer` probe via OneDrive- and locale-aware candidate
     enumeration).

When the smart-default flips, the shim logs one line: `GameFreeCameraShim:
smart-default auto-enable (binding=<key>, VK=0xHH, g_developer=1, INI=[m2]
game_free_camera_shim untouched). The shim will now dispatch the toggle
key on every F10 transition.` The user can still disable it explicitly
via `[m2] game_free_camera_shim = false` — the explicit flag gates the
smart-default path so a user who has vetted `false` is not surprised by
a silent re-enable.

**Controls binding auto-detect (M2.5).** At DLL init the shim scans the
active profile's `controls.sii` (OneDrive- + locale-aware path probe,
parallels the `g_developer` probe) for the first `keyboard.*` token bound
to `dev_camera_toggle`. The detected key becomes the dispatch VK unless
the user explicitly pinned a value in the INI
(`m2_dev_camera_toggle_vk_explicit=true`). Falls back to the default
`0x60` when no binding is found.

ABI: `walker_game_free_camera_shim_set_enabled`, `_supported`,
`_on_enter_walking`, `_on_exit_walking`, `_toggle_now`,
`_dump_game_free_camera_shim`. The F12 dump surfaces
`ini_overrides_vk` / `auto_detected_key` / `controls_sii_path` for
verification; F10 Walking-entry log line shows the same fields inline.

The `g_developer` probe is OneDrive- and locale-aware. It first asks
the shell for `CSIDL_MYDOCUMENTS` (which transparently follows the
OneDrive Documents redirect when it is active), and on miss it
enumerates six candidate sub-folders relative to `CSIDL_PROFILE`:
`Documents` (canonical English), `My Documents` (XP-era),
`OneDrive\Documents` / `OneDrive\Documentos` (Spanish OneDrive),
`OneDrive - Personal\Documents` / `OneDrive - Personal\Documentos`
(multi-account OneDrive). Both `American Truck Simulator` and
`Euro Truck Simulator 2` stems are tried in each step so the same
DLL works on either game. The candidate list and the
`uset g_developer "X"` line scanner are exposed as
`GameFreeCameraShim::ProbeConfigCandidatesForTesting` and
`GameFreeCameraShim::ReadGDeveloperValueForTesting` and pinned by
the test suite (`test_game_free_camera_shim.cpp`).

A live `walker_dump_game_free_camera_shim` shows the resolved
path and value. If the probe still reports `supported=0` after
deploy, the path most likely points at an unsupported layout
(no config.cfg, redirected to a non-OneDrive Documents folder,
or a custom install); open the dump and adjust the probe
accordingly.

The reason we use the SCS free camera for the MVP — and the rendering
work the present hook will pick up in lander 2 below — comes from
the working reference: TM_Real_Walk_v1.0.0-beta4.5
`TMRealWalk.dll` (see `REA/scripts/tmrealwalk-re/analysis.md`). Its
authors explicitly rely on `g_developer` for the walking camera and
render *overlay geometry only* (shadow, prompts, hands) through
their Present hook. Their HLSL shaders are embedded in the DLL and
visible when their import directory is walked with the right
fuzziness. Our CopyData approach mirrors that.

**Long-term path: SCS camera-update hijack.** Replace SCS's
view+projection constant buffer write with our own matrices from
`walker_camera()`. Requires deeper engine RE
(see `REA/scripts/ats_1_61_3_1s.physx.functions.md`): locate the SCS
camera-update function in `amtrucks.exe`, identify the constant
buffer slot + register indices it writes, and either patch the
function's prologue to call a hook or write our matrices after it
runs. Blocked in the sandbox without live debugging. When the
present-hook overlay renderer lands (M11 lander, Dear ImGui via
single-header vendor), it overlays shadows + `[F]` prompts over
whatever camera is active — the SCS free camera for now, our
camera later.

> **v3 graphics-API correction.** The original v3 plan assumed D3D11
> (`ID3D11DeviceContext::UpdateSubresource` on a constant buffer
> pushed to the VS). A static RE pass on `amtrucks.exe` 1.61.3.1s
> (`REA/scripts/tmrealwalk-re/dump_d3d11_imports.py`, commit
> `a9701556`) shows the binary does **not** statically import
> `d3d11.dll` or `dxgi.dll` in its standard PE import directory —
> both libraries are loaded dynamically via `LoadLibraryA` + manual
> `GetProcAddress`, same pattern SCS uses for inline PhysX 3.4
> (commit `cfb403bb`). Combined with the existing code comment in
> `src/adapter/PluginEntry.cpp:114-119` ("ATS 1.61.x uses D3D12"),
> this means:
>   - `D3D11Hook` is a **dead path** on this build by design.
>   - v3 must replace SCS's writes via **D3D12**: snapshot the
>     `ID3D12GraphicsCommandList*` that SCS captures during the
>     per-frame draw, then call
>     `SetGraphicsRoot32BitConstants` / `SetGraphicsRootConstantBufferView`
>     from inside `Vtable16HookThunk` to inject `walker_camera()`'s
>     view+projection matrices.
> The hook point (per-frame `vtable[16]` trampoline) and singleton
> walk (camera state struct) discovered in v2 are still correct;
> only the matrix-replacement body changes.

#### M2 long-term — RE pre-work (post-M11.4 lander)

Two static scans in `REA/scripts/tmrealwalk-re/` located the function
the SCS camera uses to call the per-frame camera update:

1. `find_camera_refs.py` — scans `amtrucks.exe` (53 MB stripped) for
   strings like `g_flyspeed`, `g_debug_camera`, `free_camera`,
   `Only supported in free camera mode`, and reports every RIP-relative
   LEA/MOV in `.text` that targets one of those string VAs. Output:
   `find_camera_refs.{json,txt}` — **7 refs found**, four of them
   clustered in `0x140033xxx` (the SCS CVar registration table),
   three single refs (`free_camera` short form @ `0x140087082`,
   `camera_speed` @ `0x1407C4A2A`, and `"Only supported in free
   camera mode"` @ `0x140C230C7`).

2. `find_camera_function.py` + `dump_camera_window.py` — locate the
   function containing the `0x140C230C7` ref and disassemble it. Output:
   `find_camera_function.txt` (narrow) + `camera_function_0x140c22f40.txt`
   (full body). Entry point `0x140C22F40` is a state-machine dispatcher
   that, on the `state == -1` path, **calls `qword ptr [rdx + 0x80]`**
   (i.e. `vtable slot 16`) on the camera state struct after temporarily
   overriding yaw/pitch to `0x42B40000` (= 75.0°). That vtable slot 16
   is the per-frame camera update.

What the long-term hijack still needs (one of):

- **(a) Runtime singleton resolve.** Walk the game-camera global via a
  Ghidra-traced pointer chain at game start, read the camera state
  struct, read its vtable, take `vtable[16]` as the hook target.
  Requires a live debugging session against `amtrucks.exe` to capture
  the singleton offset at boot.

- **(b) Static signature scan.** Walk `.text` for the byte pattern
  `FF 92 80 00 00 00` (`call qword ptr [rdx + 0x80]`) and
  cross-check against callers of `0x140C22F40`. Automatable with a
  ~100-line scanner; would yield the vtable slot 16 function VA
  without needing a live debug session.

(b) has been validated: `scripts/tmrealwalk-re/find_vtable_slot16.py`
walks `.text` (31 MB) for `FF 92 80 00 00 00` — finds **101
occurrences**, of which exactly **one** survives the discriminator
filter (`movss [rbx+0x30], xmm6` + `movss [rbx+0x34], xmm7` +
`mov dword [rbx+0x30], 0x42B40000` within ±256 bytes). That single
hit is `0x140C2306D` — the vtable-slot-16 call inside the camera
state dispatcher at `0x140C22F40`. Encoding-bug note: the post-
restore `movss` instructions use **mod=01 (1-byte displacement)**,
so the bytes are `F3 0F 11 73 30` and `F3 0F 11 7B 34`, NOT
`F3 0F 11 B3 30 00 00 00` (which is mod=10, 4-byte displacement).

That makes path (b) the right path forward. Implementation plan for
the long-term M2 hijack:

  1. At game start (DLL load), capture `image_base` from the running
     `amtrucks.exe` process (PE optional header field 24/28 depending
     on PE32/PE32+). Compute the dispatcher absolute VA:
     `dispatcher_va = image_base + 0xC22F40` (RVA of `0x140C22F40`
     relative to image_base, since `amtrucks.exe`'s .text RVA is 0x1000
     and the function lives at offset 0xC21F40 inside .text).
  2. MinHook `dispatcher_va`. The trampoline must call the original
     dispatcher first (so SCS continues to think `g_developer`/`g_free_camera`
     flips the camera mode) and then replace the post-call state.
  3. In the trampoline, walk the same singleton path the original
     dispatcher uses (`mov rbx, [rip + 0x2A95E7F]` from `0x140C22F4A`,
     then `mov rbx, [rbx + 0x2030]`) to grab the camera state struct,
     read its vtable, then read `vtable[16]`. That gives us the per-frame
     camera matrix update function VA — which is the actual hot path.
  4. Hook `vtable[16]`. In the trampoline, after SCS writes its
     matrices to the constant buffer, replace them with the matrices
     from `walker_camera()`.

Steps 1–4 are pure runtime work; the captured VAs in the JSON
(`commit 07a0e579`) are pinned as evidence. No further RE required.

MVP `GameFreeCameraShim` remains the deployed path until the long-term
implementation lands.

### M3 Lander — Real IWorldGeometry

**File to implement:** `src/provider/PhysXAdapter.cpp` (currently a stub).

- Strategy 1 (preferred): link against the public PhysX 3.4 SDK
  (github.com/NVIDIA-Omniverse/PhysX). **PhysX 3.4 confirmed embedded
  in amtrucks.exe 1.61.3.1s** via static RE — every `PhysXCooking` /
  `PhysXCore` source-path literal in the binary shows
  `D:\manual\third\physx\version_patched\PhysX_3.4\Source\` (REA
  commit `cfb403bb`, `scripts/tmrealwalk-re/find_physx_version.py`).
  No v4/v5 ABI drift to worry about. Hook the game's `PxPhysics`
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

### M3.5 Lander — PxScene runtime hook (capture the live instance)

**File:** `src/provider/PxSceneHook.cpp`.

The M3 lander documents the vtable catalog (8 entries at VA 0x141AE0A0+
slots; full list in `include/walkercore/PhysXAdapter.h`) and runs an
on-disk signature scan + the catalog matcher at install time. What it
**cannot** do is locate the live `PxScene` instance itself: the
on-disk scan over `.rdata` / `.data` / `.bss` returns zero
candidates for the vtable pointer `0x1425020A0`
(see `REA/scripts/physx-vta-scan/find_pxscene_instances.py`).
The instance is allocated at runtime by `PxScene::create(...)` and
held in a register / local variable inside the game.

M3.5 closes the gap with the smallest viable runtime hook:
install MinHook on the `PxScene::addActor` function body at VA
`0x141AE0B62` (catalog slot 11, the entry the catalog confirmed
exists). When the game calls `addActor`, the x64 calling
convention places `this` (the PxScene instance) in `RCX`; the
trampoline captures RCX into a process-global atomic and forwards
the call to the original via MinHook's trampoline. The first
non-zero capture wins (subsequent calls only bump the call
counter, never overwrite) so downstream M3 work can ask "where
is the live PxScene?" without further RE.

**Why slot 11 specifically.** PxScene::addActor is the only
catalog method we can be confident is called by the game with a
stable `this` (PxScene*) in RCX. Other catalog methods may have
chained calls where `this` is a child object, not the scene.
`REA/scripts/physx-vta-scan/find_addactor_callers.py` confirms
this: 174 slot-11 callsites exist, but only a small subset are
true `PxScene::addActor` (the rest are `PxActor::slot11` and
similar). The hook at the function body dodges this entirely —
we read the parameter the compiler already set up for us.

**ABI:**

- `int walker_pxscene_supported(void)` — 1 when the hook installed
  AND the target VA is reachable inside this process
  (`amtrucks.exe` is loaded at the right image base), 0 otherwise.
- `uint64_t walker_pxscene_instance(void)` — the captured
  PxScene instance pointer; 0 until the first addActor call.
- `void walker_dump_pxscene(void)` — focused dump: supported flag,
  target VA + RVA, captured instance, addActor call count.

**Test seams:**
`PxSceneHook::ObserveAddActorForTesting` simulates the hook
entry (used by `tests/test_px_scene_hook.cpp`), and
`PxSceneHook::ResetForTesting` clears the captured pointer +
call count between tests.

**ABI contract impact:** 3 new exports (15 -> 18 total).

When the game is not running (unit tests, debug runs without
the game), the hook is a no-op: `Install()` returns true but
`target_reachable()` stays false, the captured pointer stays 0,
and the ABI surface is still callable so
`walker_dump_pxscene` reports "target reachable: false" instead
of crashing.

### M5 Lander — Per-truck door geometry

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
  real station prefab list. The SCS SDK does **not** publish a station
  prefab callback (`prefab/list_station` is not a registered SDK event
  or channel — only the in-game dev UI surfaces it). The real station
  registry is an internal SCS prefab list embedded in `amtrucks.exe`;
  locating it requires a live debugger against the game process (RE-
  blocked until a debug session is available).
- Tank capacities: **implemented (2ec6e97)**. `ScsTelemetryProvider` parses
  the SCS configuration attributes `fuel.capacity` and `adblue.capacity`
  (delivered by `SCS_TELEMETRY_EVENT_configuration` alongside
  `hook_position` and `cabin_position`) and stores them in
  `TruckState::fuel_capacity_l` / `adblue_capacity_l`. Validity gate
  rejects zero/negative/out-of-range values so a misconfigured mod or a
  future build reporting in mL cannot crash the fill rate. `FuelProvider
  ::Tick` replaces the honest-stub 600 L fuel / 50 L AdBlue defaults
  with the per-truck values; the fill rate clamps to the resolved
  capacity. Symmetric `adblue_capacity_l()` accessor exposed alongside
  the existing `tank_capacity_l()`.
- SCS channels `truck.fuel.amount` and `truck.adblue` are already
  registered (M1); the local `fuel_amount_l_` is synced from telemetry
  on every Tick when the channel is live.

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

**Status: D3D11 Present-path integration landed.** The full UI
design (camera preview, settings menu re-skin, real-time FSM
state) is out of scope for this commit — the goal of the
integration commit is to wire the ImGui context + D3D11 backend
into the existing D3D11 Present hook so the overlay actually
renders on top of the game's swap chain. Subsequent commits
extend the rendered content; the plumbing is now in place.

**What ships in this commit (M11 D3D11 integration):**

- `WALKERCORE_HAVE_IMGUI` build flag + ImGui static library
  `imgui_vendor` (CMake `add_library STATIC` with relaxed
  `-w` warnings to absorb the upstream C-style casts / sign
  conversions). Both the DLL build and the test build link the
  six ImGui sources (`imgui.cpp`, `imgui_draw.cpp`,
  `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_impl_dx11.cpp`,
  `imgui_impl_win32.cpp`) plus the three vendored internal
  headers (`imgui_internal.h`, `imconfig.h`, `imstb_*`).
- `include/walkercore/ImGuiOverlay.h` extended with the D3D11
  device lifecycle: `InitializeForD3D11Device(device, context,
  hwnd)`, `OnDeviceLost`, `OnDeviceCreated`, `RenderFrameIfEnabled`,
  `Shutdown`, plus `frames_rendered()` + `ImGuiVersionString()`
  accessors. Process-wide singleton routed through
  `OnD3D11DeviceCaptured` / `OnD3D11DeviceLost` /
  `RenderImGuiFrameIfEnabled` / `ShutdownImGuiOverlay` free
  functions in the `walkercore` namespace.
- `src/provider/ImGuiOverlay.cpp` now creates the ImGui
  context + Win32 + D3D11 backends on first device capture
  (with `EnumWindows`-resolved HWND), invalidates / re-creates
  GPU resources on device replacement, and runs the
  `ImGui::NewFrame` → status window → `ImGui::Render` →
  `ImGui_ImplDX11_RenderDrawData` cycle inside the per-frame
  `RenderFrameIfEnabled` path. The default render is a small
  top-left status window with the ImGui version + walk counters
  + frame counter so deploy-side analysts can confirm the
  rendering path is firing.
- `src/provider/D3D11Hook.cpp` extended with `FindGameWindowHandle`
  (EnumWindows + `GetWindowThreadProcessId` matching the current
  PID + `IsWindowVisible` filter) and three new wire-up points:
  - `HookedCreateDeviceAndSwapChain` calls
    `walkercore::OnD3D11DeviceLost` when the device pointer
    changes (rare — game recreating the D3D11 device), then
    `walkercore::OnD3D11DeviceCaptured(device, context, hwnd)`
    to spin up the ImGui context.
  - `HookedPresent` calls `walkercore::RenderImGuiFrameIfEnabled()`
    AFTER `walker_try_enter_per_frame_tick()` returns. Render
    runs on the game thread, after the per-frame simulation
    tick, so the overlay's status line is current.
  - `Uninstall` calls `walkercore::ShutdownImGuiOverlay()` before
    releasing the D3D11 context reference so the font texture
    view can still be released cleanly.
- 6 new ABI exports in `include/walkercore/PerFrame.h`:
  `walker_imgui_overlay_version`,
  `walker_imgui_overlay_frames_rendered`,
  `walker_shutdown_imgui_overlay`,
  `walker_on_d3d11_device_captured`,
  `walker_on_d3d11_device_lost`,
  `walker_render_imgui_frame_if_enabled`. ABI contract: 21 → 27
  total (the 3 M11 scaffolding exports are still there; this
  commit adds 6 more).
- `walker_dump_imgui_overlay` now also reports the ImGui
  version string and the per-DLL-load `frames_rendered` counter
  so analysts can confirm the Present hook is actually
  rendering. `walker_dump_state` picks up the new line
  automatically through the existing M11 overlay block.
- 6 new unit tests in `tests/test_imgui_overlay.cpp`:
  - `TestImGuiOverlayVersionStringIsNonNull` — pins the
    version-string contract for both vendored + scaffolding
    builds.
  - `TestImGuiOverlayInitializeRefusesNullDevice` — class
    rejects null device/context, supported stays false.
  - `TestImGuiOverlayInitializeIsIdempotent` — second init
    call after first failure stays safe.
  - `TestImGuiOverlayRenderNoOpWhenDisabled` — frame counter
    stays at 0 when enabled=false.
  - `TestImGuiOverlayRenderNoOpWhenSupportedFalse` —
    enabled=true without a D3D11 device still produces 0
    frames.
  - `TestImGuiOverlayOnDeviceLostIsSafeWhenUninitialized` —
    OnDeviceLost + Shutdown on an uninitialised overlay are
    safe.
- `tests/CMakeLists.txt` mirrors the DLL's ImGui static-lib
  build (with relaxed warnings) plus the `d3dcompiler` +
  `dwmapi` link line. The `WALKERCORE_HAVE_IMGUI=1` define is
  set on BOTH `WalkerCoreTestLib` and `WalkerCoreTests` so
  the test source files in `test_imgui_overlay.cpp` see the
  same `#define` value as the test lib.

**What does NOT ship (follow-up commits):**

- The M11 D3D11-integration commit was the plumbing. The follow-up
  M11.2 (commit `5370809`) shipped the live walker state render
  (camera eye + yaw + pitch + FOV; player feet + mode; FSM state +
  transition progress; walk counter; frame counter) so deploy-side
  analysts can see the live state at a glance. M11.3 (commit
  `00f3132`) added the build hash + build date to the overlay
  header so a one-glance comparison against the expected commit
  confirms "did I deploy the right DLL?". M11.4 (commit `61ebc23`)
  installed the WndProc subclass (see M4 row) so the overlay is
  interactive — `ImGui_ImplWin32_WndProcHandler` is in the WndProc
  chain and the same shim routes `WM_INPUT` to the data plane.
- The actual UI content (camera preview, settings menu re-skin,
  interactive dump buttons) is still out of scope. The overlay
  shows a status panel; clicking inside it does nothing yet
  because no widget has been wired to a callback. The first
  interactive widget (a "press to dump state" button that calls
  `walker_dump_state()` from ImGui) is the natural M11.5 follow-up
  once M11.4 has been deployed + verified.

**When the user enables the overlay:**

`[m11] imgui_overlay = true` in `%APPDATA%\WalkerCore\WalkerCore.ini`
+ game restart. F12 in-game confirms:
- `walker_dump_imgui_overlay` reports `enabled=true` +
  `supported=true` (after the D3D11 hook captures the device)
  + `imgui version=1.91.5` + `frames_rendered=N` (N>0 after
  the first frame).
- `walker_dump_state` shows the new `imgui overlay` block with
  the same flags + counters.

Game log should show:
- `ImGuiOverlay: enabled=1 (set [m11] imgui_overlay = true in
  WalkerCore.ini to keep)`
- `D3D11Hook: captured device <addr> swap chain <addr> (gen 1)`
- `ImGuiOverlay: initialized (device=<addr> context=<addr>
  hwnd=<addr> imgui=1.91.5)`

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