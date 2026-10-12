# Testing WalkerCore

Two layers of verification coexist:

1. **Automated unit + integration tests** (run on every CI build, and
   locally via `scripts\ci_test.ps1`). These exercise pure-domain math,
   `FrameLoop` / `WalkerFsm` / provider state without ever loading
   the DLL or the game.

2. **Manual in-game verification** for anything that depends on the
   game process, the D3D11 present hook, or the SCS SDK at runtime.
   These happen on a host with ETS2 / ATS running and are documented
   per lander in `docs/integration.md`.

## Toolchain

- **Compiler:** MinGW-w64 GCC ≥ 13 (16.2.0 tested). Toolchain root
  `C:\msys64\mingw64\bin` — must be on `$env:PATH` for every cmake /
  ninja invocation (the wrapper script does this automatically).
- **Build system:** CMake ≥ 3.20 (4.4.4 tested) + Ninja.
- **Test harness:** handcrafted `tests/TestHarness.h` with `EXPECT_EQ`,
  `EXPECT_NEAR`, `EXPECT_TRUE`, `EXPECT_FALSE`, `EXPECT_LT`, `EXPECT_GT`,
  `EXPECT_STREQ`. No gtest / doctest dependency. Native exe is a single
  binary that prints one line per test plus a final `TEST_SUMMARY`.

## How to build + run the tests

Recommended (one command does both):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci_test.ps1
```

Equivalent manual steps:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja
.\build-ninja\tests\WalkerCoreTests.exe
```

Expected exit code: 0 on pass. The exe prints the test name + `ok`
for each case, then a final summary `NNN checks, M failed`. Any failed
expectation is printed inline with the `EXPECT_*` line and source
location.

## What the automated tests cover

Each probe follows one of two patterns:

- **Field-exhaustive probe.** For a given domain struct (e.g.
  `WalkerFsmInputs`, `MovementConfig`, `ShadowProvider::Compute`,
  `CameraState_Update`), drive the unit through isolated scenarios
  that vary one field at a time and assert the relevant observable
  changes. Pins that no field is silently swallowed.
- **State-machine / contract probe.** Drive the unit through the
  documented transitions and assert the documented outputs. Covers
  both positive cases and negative controls (e.g. "feature off →
  no observable change").
- **Perf budget probe.** Pin a per-call (or per-tick) ceiling on the
  hot path. Threshold is set 10× above the expected cost so the test
  does not flake on slow CI but still catches a 10× regression.

Coverage by topic:

| Module                                  | Tests |
|-----------------------------------------|-------|
| Camera math (`CameraState`)             | view + projection matrices; `aspect / tan(half_h_fov)` vertical scale; full field-exhaustive; perf budget |
| `MovementConfig`                        | defaults sane; every field has observable effect (12 consumed; 2 honest-no-consumer for M3 lander) |
| `WalkerFsm`                             | every state transition; transition tween math (4 cases); state-name exhaustiveness; field-exhaustive inputs |
| `FrameLoop::RunOnce`                    | integration; diag override; no-FSM no-op; **diagonal motion is speed-bounded**; **crouch wins over run** priority |
| Per-frame perf budgets                  | `FrameLoop::RunOnce` ≤ 200 µs/tick; full conductor ≤ 200 µs/tick; `InputHook::Sample` ≤ 10 µs/call; `CameraState_Update` ≤ 5 µs/call |
| Honest-stub warn contracts              | every M-lander stub (M3 PhysX, M5 door-config is consumed via `TruckDoorConfig`, M7 polish, M9 coupling, M10 knockout, plus M5/M8 fuel) |
| ABI contract                            | `LoadLibraryA` + `GetProcAddress` resolvable for the 14 documented exports; `walker_dump_*` safe to call without a provider wired |
| Head / shadow / flashlight / door providers | provider-level + per-field observable-effect |
| INI / settings menu                     | parser, roundtrip, every-documented-slider writes through, every-documented-bool writes through, every-documented-key-binding writes through |

## What still needs manual in-game verification

- **The DLL actually being loaded by the game.** Until SCS spawns
  our `scs_telemetry_init`, nothing on the running game's per-frame
  path is exercised. CI can't catch this; this needs `bin/win_x64/plugins/`
  copy + load + observe `walker_dump_state` on F12.
- **D3D11 Present hook (M2 lander).** The hook calls into a per-frame
  conductor that ultimately routes to `walker_per_frame_tick(dt)`.
  Unit tests cover the conductor chain in isolation; the D3D11-specific
  glue (vtable slot-8 Present rebind, swap-chain recreation defence)
  only runs inside the game process.
- **Real PhysXAdapter (M3 lander).** Stubbed with honest no-hit. When
  the M3 lander lands and is configured to read the real SDK, every
  SphereSweep returns valid distance-to-hit and the
  `PhaseResolveCollision` then settles to ground. Confirm in-game
  by walking over uneven terrain.
- **Head tracker / TrackIR / opentrack feeds (M7 lander).** Polish
  providers warn once; once installed against a real TrackIR /
  opentrack source, the per-tick yaw/pitch should match the device's
  reported pose. Confirm by aiming the head in-game.
- **Trailer attach / detach (M9 lander).** The stub logs once and
  returns no coupling change. Real lander will report attach events
  to the diagnostic dump.

A clean **automated** test run is **necessary but not sufficient**:
the DLL must also be verified inside the game.
