# WalkerCore

[![CI](https://github.com/<owner>/WalkerCore/actions/workflows/ci.yml/badge.svg)](.github/workflows/ci.yml)

Plugin DLL de caminata para Euro Truck Simulator 2 y American Truck Simulator (single-player).

Inspirado en walkers comerciales como TMRealWalk; escrito desde cero sobre el SDK público de telemetría de SCS y los headers públicos de PhysX 3.4. Sin ingeniería inversa de productos de terceros.

> **Estado.** Implementación en curso. DLL compilable (`libWalkerCore.dll`, 14 exports,
> ~2.9 MB); módulo de locomotor (M1/M2 + M4 + M6 + M8 partial) completo, con FSM
> walker/per-frame/per-frame-conductor exhaustivamente probado en host. Los
> landers M3 (PhysX), M5 (per-truck door config), M7 (head tracker / flashlight /
> shadow / door-anim real feed), M9 (trailer attach), M10 (knockout desde PhysX)
> y M11 (ImGui) están honest-stub con uno-shot warn que se degrada limpiamente
> cuando falta la dependencia nativa. Ver
> [`docs/roadmap.md`](docs/roadmap.md) y [`docs/integration.md`](docs/integration.md).

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

MIT — ver [`LICENSE`](LICENSE).

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

## Referencias

- [`docs/roadmap.md`](docs/roadmap.md) — fases M0..M11, criterios de aceptación
- [`docs/integration.md`](docs/integration.md) — contrato entre módulos y los M-landers
- [`docs/testing.md`](docs/testing.md) — qué verifican los tests y qué requiere juego real
- [`docs/architecture.mermaid`](docs/architecture.mermaid) — diagrama de capas
- [`docs/decisions/`](docs/decisions) — ADRs por decisión abierta
- [`AGENTS.md`](AGENTS.md) — convenciones del repo
