# AGENTS.md — convenciones del repo

Para futuros agents que toquen este código.

## Estilo

- C++20, sin STL en la frontera DLL (solo en módulos internos).
- Headers en `include/` o junto al `.cpp` (`domain/Foo.h` + `domain/Foo.cpp`).
- Sin `try`/`catch` que cruce frontera ABI.
- `extern "C"` para todo lo exportado.
- Logs por canal: `[WalkerCore] <módulo> <mensaje>`.

## Build

- CMake + MinGW primario. Mantener portable.
- Ningún `cl.exe` específico en scripts.
- Outputs versionados: build embebe SHA del commit (`WALKERCORE_BUILD` macro).

## Decisiones de arquitectura

- Dominio sin dependencias de SCS, D3D, ni win32. Ver `docs/architecture.md`.
- Providers aíslan las llamadas externas (SCS SDK, D3D11, raw input).
- Workflows son los únicos que combinan providers + dominio.
- Adapter expone `DllMain`, `scs_telemetry_init`, `scs_telemetry_shutdown`.

## No hacer

- No usar STL en headers exportados (riesgo de incompatibilidad ABI).
- No meter dependencias que necesiten VC++ Redistributable.
- No conectar a internet (sin telemetría, sin actualizaciones remotas).
- No reproducir código de TMRealWalk u otros walkers comerciales.

## Referencias

- `docs/roadmap.md` — fases M0..M8 y criterios de aceptación.
- `docs/architecture.md` — pendiente de escribir tras M0.
- `docs/decisions/` — ADRs de cada decisión abierta.

## Tests

- Framework propio en `tests/TestHarness.h` (`EXPECT_EQ`, `EXPECT_NEAR`,
  `EXPECT_TRUE`, `EXPECT_FALSE`, `EXPECT_LT`, `EXPECT_GT`, `EXPECT_STREQ`).
  **No** se depende de gtest / doctest; el harness vive en el repo y
  el ejecutable es un solo binario nativo que imprime una línea por
  test y un `TEST_SUMMARY` final.
- Tests cubren dominio puro (math, FSM) + workflows (FrameLoop,
  State transitions) + contratos honest-stub (one-shot warn).
  NO cargan la DLL — la DLL solo se verifica con el probe ABI
  (LoadLibrary + GetProcAddress) que resuelve los 14 exports.
- D3D11 / input / SCS SDK: solo verificación manual en juego (ver
  `docs/testing.md`).

### Iteración local

Atajo de un solo comando para el ciclo build + test local (MinGW):

```pwsh
pwsh -File scripts/ci_test.ps1
```

Hace tres cosas: prepend `$env:PATH` con `C:\msys64\mingw64\bin`,
corre `cmake --build build-ninja` (incremental), y corre
`build-ninja/tests/WalkerCoreTests.exe` directamente. Flags:

- `-Reconfigure` — fuerza `cmake -S . -B build-ninja -G Ninja`
  (útil tras editar `CMakeLists.txt` o `third_party/minhook`).
- `-Toolchain msvc` — usa MSVC + `-DCMAKE_BUILD_TYPE=Release`
  (asume que `cl.exe` ya está en PATH vía Developer Command Prompt).
- `-MingwBin <path>` — override del toolchain root por defecto.