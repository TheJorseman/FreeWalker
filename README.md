# WalkerCore

[![CI](https://github.com/<owner>/WalkerCore/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

Plugin DLL de caminata para Euro Truck Simulator 2 y American Truck Simulator (single-player).

Inspirado en walkers comerciales como TMRealWalk; escrito desde cero sobre el SDK público de telemetría de SCS y los headers públicos de PhysX 3.4. Sin ingeniería inversa de productos de terceros.

> **Estado.** DLL compilable (`libWalkerCore.dll`, 45 exports, ~4.7 MB). Locomotor
> (M1 + M2 MVP + M4 mouse-pump + M5 per-truck door + M6 INI) end-to-end. M7
> opentrack + PolishProviders computan (game-side integration de sun / point
> light / door skeletal sigue stub). M9 single-trailer hook landed (king-pin
> world-space real cuando SCS entrega `trailer` config event); multi-trailer
> con `trailer.0` / `trailer.1` queda pending. M10 knockout con velocity-
> change heuristic + log de colisión (PhysX-derived collision normal
> pendiente). M11 D3D11 ImGui overlay live con state + botones interactivos
> + M2 Dev camera section (shim/dev_cvar_patcher/dev_camera_toggle_vk).
> M3 PhysX catalog + M3.5 addActor hook activos (SphereSweep/Raycast siguen
> no-hit sin llamada real al PxScene). M2 long-term CameraHijack bloqueado
> por `movdqa` #GP en amtrucks.exe+0x1d5300a con ReShade+OptiScaler+RenoDX.
> Ver [`docs/roadmap.md`](docs/roadmap.md), [`docs/integration.md`](docs/integration.md),
> y [`docs/bugs.md`](docs/bugs.md).

## Requisitos

- Windows 10 x64
- MinGW-w64 GCC ≥ 13 (probado con GCC 16.2.0 en `C:\msys64\mingw64\bin`)
- CMake ≥ 3.20 (probado con CMake 4.4.4)
- Ninja (probado con el paquete de CMake)
- Windows SDK 10 (`C:\Program Files (x86)\Windows Kits\10\Include`) — `d3d11.h` solo se necesita si MinHook no está vendoreado
- ETS2 o ATS build ≥ septiembre 2026

## Cómo se compila

Atajo de un solo comando (recomendado):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci_test.ps1
```

Este wrapper pone `C:\msys64\mingw64\bin` en `$env:PATH`, corre
`cmake --build build-ninja`, y lanza `build-ninja\tests\WalkerCoreTests.exe`
directamente. Flags:

- `-Reconfigure` — fuerza `cmake -S . -B build-ninja -G Ninja`
- `-Toolchain msvc` — usa MSVC + `-DCMAKE_BUILD_TYPE=Release`
- `-MingwBin <path>` — override del toolchain root.

Manual:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
cmake -S . -B build-ninja -G Ninja
cmake --build build-ninja
```

Salida: `build-ninja\libWalkerCore.dll`. El ejecutable de tests:
`build-ninja\tests\WalkerCoreTests.exe`. Para instalar en el juego,
copiar la DLL a `bin\win_x64\plugins\` del juego (ETS2 o ATS).

## Tests

Build + test en un comando: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ci_test.ps1`.

Los tests se ejecutan en proceso nativo (no cargan la DLL). Cubre:

- FrameLoop y WalkerFsm state machine (incluyendo `PhaseIntegrateMovement`,
  `PhaseResolveCollision`, `PhaseAnimatePlayer`)
- Camera math (view + projection matrices, `aspect / tan(half_h_fov)` vertical scale)
- MovementConfig field-exhaustive (cada campo observable)
- Honest-stub one-shot warn contract para cada M-lander stub
- ABI contract: `LoadLibraryA` + `GetProcAddress` sobre los 14 exports documentados
- Perf budgets para el hot path (`FrameLoop::RunOnce`, conductor completo,
  `InputHook::Sample`, `CameraState_Update`)

El comportamiento in-game (D3D11 hook, captura física) sigue requiriendo
verificación manual con el juego corriendo; ver [`docs/testing.md`](docs/testing.md).

## Licencia

**GPL-3.0-or-later** — ver [`LICENSE`](LICENSE).  Relicenciado desde MIT el
2026-10-13 para permitir derivar trabajo de
[aefly/motioncab](https://github.com/aefly/motioncab) (GPL-3.0).  Toda
obra derivada debe también ser GPL-3.0-or-later.

## Lo que NO hace

- **TruckersMP / convoy Steam.** El plugin no bloquea el multiplayer por defecto
  — logea un aviso pero deja la decisión al usuario. Los walkers comerciales
  tienen la misma limitación; usar un walker en multiplayer puede romper las
  reglas del servicio.
- **Red.** No envía datos, no tiene DRM, no tiene telemetría, no se actualiza
  solo.
- **Plugins de terceros.** No depende de VC++ Redistributable. Trae un single
  binario estático en MinGW.

## Aviso de antivirus

Algunos antivirus marcan DLLs que enganchan input y D3D11 como falsos positivos.
Si pasa, excluir `bin\win_x64\plugins\` del juego en el antivirus.

## Controles (key bindings por defecto)

| Tecla | Acción |
|---|---|
| **F10** | Toggle walk mode (Driving ↔ Walking). Cuando el shim está activo y `g_developer = 1` en el `config.cfg` del juego, sintetiza la tecla F11 (default M2.7) al entrar a Walking para que el SCS dev free camera tome el control. Live-verified Oct 2026: F11 (con `g_developer=1` AND `g_console=1`) es el toggle real del dev free cam en amtrucks.exe 1.61.3.1s — no `0` ni `Numpad-0` como se asumió en versiones M2.5/M2.6. M2.5: el shim se auto-activa cuando detecta un binding a `o_im_freecam` en `controls.sii` (la única acción SCS válida confirmada por RE); si tu profile no la tiene, pon `game_free_camera_shim = true` en `WalkerCore.ini` y el shim usará el default F11. **Recomendado:** `mix o_im_freecam \`keyboard.f8?0\`` en controls.sii + shim auto-enable — F8 evita colisión con `bugreport*` / `menustereo` que sufren F11 en el perfil stock. |
| **Ctrl + F10** | Abre / cierra el menú de settings en juego. |
| **F** | Interact (entrar / salir de cab, fuel, coupling). |
| **F12** | Dump completo de estado a `game.log.txt` (todos los `walker_dump_*`). |
| **W A S D** | Caminar (W forward / S backward / A strafe left / D strafe right). |
| **Shift** | Correr. |
| **Ctrl** | Agacharse (default; mantener Ctrl + F10 = menu). |
| **Space** | Saltar. |
| **E / Q / R** | Ojos arriba / abajo / reset. |
| **G** | Linterna. |
| **LMB** | Zoom (mouse button). |
| **RMB** | Linterna alterna. |
| **Mouse** | Look (yaw + pitch). El delta se captura vía `WM_INPUT` (M4) y se integra en `FrameLoop::PhaseApplyMouseLook`. |
| **Wheel** | Velocidad de paso (placeholder; el M4 lander lo lee). |

Todas las teclas son configurables vía `[bindings]` en
`%APPDATA%\WalkerCore\WalkerCore.ini` (M6 lander). Los defaults
están en `include/walkercore/InputHook.h:33-50`.

## INI gates

| Sección | Key | Default | Descripción |
|---|---|---|---|
| `[m2]` | `game_free_camera_shim` | `false` (M2.5: smart-default auto-flips a `true` cuando binding a `o_im_freecam` + `g_developer=1`) | Cuando activo, F10 sintetiza VK_F11 (0x7A, default M2.7 — verificado live en ATS 1.61.3.1s) o el `keyboard.X` bindeado a `o_im_freecam` en `controls.sii` (la única acción SCS válida por RE), configurable vía `[m2] dev_camera_toggle_vk = 0xHH`. Auto-detecta `g_developer` en `OneDrive\Documentos\American Truck Simulator\config.cfg` (también es_ES / OneDrive Personal) y auto-detecta el binding del usuario en `controls.sii`. M2.7b migration: solo fire-disablea el shim cuando `dev_camera_toggle_vk` es `0x30` (top-row '0') o `0x60` (Numpad-0) — los dos legacy defaults donde el shim no-op silenciosamente. Con F11 (0x7A) u otro VK válido, la intención del usuario se preserva. M2.7 migration (legacy): INIs viejos con `dev_camera_toggle_vk = 0x30` o `0x60` se reescriben automáticamente a `0x7A` en el primer boot. |
| `[m11]` | `imgui_overlay` | `false` | OFF por defecto. Cuando `true`, overlay ImGui sobre el swap chain con state en vivo + 2 botones (Dump state / Dump PxScene) + PxScene live status. |

## Referencias

- [`docs/roadmap.md`](docs/roadmap.md) — fases M0..M11, criterios de aceptación
- [`docs/integration.md`](docs/integration.md) — contrato entre módulos y los M-landers
- [`docs/testing.md`](docs/testing.md) — qué verifican los tests y qué requiere juego real
- [`docs/architecture.mermaid`](docs/architecture.mermaid) — diagrama de capas
- [`docs/decisions/`](docs/decisions) — ADRs por decisión abierta
- [`AGENTS.md`](AGENTS.md) — convenciones del repo
