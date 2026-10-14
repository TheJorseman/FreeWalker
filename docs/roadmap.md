# WalkerCore — Roadmap de ingeniería

Plugin de caminata-para-ETS2/ATS inspirado en walkers comerciales (TMRealWalk y similares), construido desde cero sobre el SDK público de telemetría de SCS y PhysX 3.4. Sin ingeniería inversa de binarios de terceros; todo el código se escribe propio.

> **Alcance honesto.** TMRealWalk.dll v1.0.0-beta4.5 son ~14 MB de código compilado y varios años de iteración con su comunidad. Este roadmap plantea una base sólida y limpia sobre la que iterar. No esperes terminar con un producto equivalente en una sola sesión.

---

## 1. Resumen ejecutivo

- **Nombre del proyecto.** `WalkerCore` (DLL: `libWalkerCore.dll`, 14 exports).
- **Compatibilidad.** ETS2 y ATS, build de septiembre 2026 (alineado con TMRealWalk beta 4.5).
- **Toolchain de compilación.** MinGW-w64 GCC ≥ 13 (probado con GCC 16.2.0 en `C:\msys64\mingw64\bin`) + CMake ≥ 3.20 + Ninja.
- **Licencia.** **GPL-3.0-or-later** (relicenciado desde MIT el 2026-10-13, ver [`LICENSE`](../LICENSE)).
- **Sin telemetría, sin DRM, sin monetización integrada.**
- **Multiplayer.** El plugin **no bloquea** TruckersMP ni convoy por defecto — expone el riesgo en el log, pero deja que el usuario decida. Se documenta claramente en el README que su uso allí puede romper las reglas del servicio.
- **Carpeta de datos de usuario.** `%APPDATA%\WalkerCore` (default; cambiar vía ADR si hace falta).
- **Idioma inicial.** Solo inglés en M0; infraestructura de localización lista desde el inicio.

---

## 2. Arquitectura de alto nivel

### 2.1 Capas y dirección (dependencias hacia el centro)

```
+-----------------------------------------------------------+
| adapters (CLI / SCM plugin DLL / diagnóstico en consola)  |
+-----------------------------------------------------------+
| shared application workflows (FSM, render loop, input)    |
+-----------------------------------------------------------+
| provider adapters (SCS SDK, D3D11 hook, input hook)       |
+-----------------------------------------------------------+
| domain semantics (PlayerState, WorldState, TruckState,    |
|               Collision, Animation, Config, Localization)   |
+-----------------------------------------------------------+
```

Reglas:
- **Dominio** no incluye ningún header de SCS, win32 ni D3D. Solo tipos C++ puros y estructuras.
- **Providers** aíslan las llamadas al SCS SDK y a las APIs nativas.
- **Workflows** combinan providers y dominio para ejecutar el ciclo de cada frame y las transiciones de estado.
- **Adapters** exponen los puntos de entrada (`DllMain`, `scs_telemetry_init`) y diagnóstico.

### 2.2 Módulos

| Módulo | Responsabilidad | Salidas | Entradas que consume |
|--------|-----------------|---------|----------------------|
| `domain::PlayerState` | Posición, rotación, altura de cabeza, modo (idle/walk/run/correr/agachado/salto/aire), velocidad, animación de bob/sway | lectura/escritura atómica por frame | `Input`, `Config`, `World` |
| `domain::TruckState` | Snapshot del estado del camión del SDK de SCS (placement, rotación, velocidad, acoplado, parking) | snapshot inmutable por frame | `scs::TelemetryProvider` |
| `domain::WorldState` | Malla de colisión del mundo, índice espacial para raycast, gestor de cuerpos físicos simples | consultas de raycast, ground-snap | lectura periódica de la malla |
| `domain::Collision` | Algoritmos: sphere-cast contra triángulos, AABB, deslizamiento sobre normales | resuelve movimiento propuesto | `WorldState`, posición/tiempo |
| `domain::Animation` | Curvas de bob/sway, blends, transiciones de pose | offset de cabeza, offset de manos | `PlayerState`, `Time`, `Config` |
| `domain::Config` | INI parser/writer, schema, defaults, validación | `Config` inmutable | disco |
| `domain::Localization` | Catálogo de strings, fallbacks | string localizado | `Config` (idioma) |
| `provider::ScsTelemetryProvider` | `scs_telemetry_init`, registro de canales, dispatch de eventos, configuración inicial | `TruckState`, eventos de ciclo | SCS SDK |
| `provider::D3D11Hook` | Hook de `IDXGISwapChain::Present`, expone una función para escribir view/projection antes del render | oportunidad por frame | DXGI + D3D11 nativos |
| `provider::InputHook` | `WH_KEYBOARD_LL` + raw input para ratón | input state + eventos | Windows hwnd del juego |
| `workflow::FrameLoop` | Pump de SCS, snapshot de estados, integración de física, push a `D3D11Hook` | frame completo | todos los módulos anteriores |
| `workflow::StateMachine` | FSM `IN_CABIN` ⇄ `WALKING` con sub-estados | transiciones | `PlayerState`, `TruckState`, `Input` |
| `adapter::PluginEntry` | `DllMain`, `scs_telemetry_init`, `scs_telemetry_shutdown`, logging a `[WalkerCore]` en game.log.txt | nada | SCS SDK |
| `adapter::Diagnostics` | Versión, build, fingerprint, dump en stdout al pulsar F12 | nada | todos |

### 2.3 Flujo por frame

```
[Game thread]
   |
   v
scs_telemetry_pump (callback)
   |
   +-> snapshot TruckState
   +-> read input events queued from InputHook
   +-> StateMachine::tick(dt)
       -> integrate movement (Collision::resolve)
       -> Animation::tick(dt)
   +-> PlayerState::publish to ChannelHook (optional telemetry)
   +-> queue D3D11 transform for next Present
   |
   v
[D3D11 hook on Present]
   |
   +-> write view + projection matrices (camera follows PlayerState.head)
   +-> flush UI overlay (prompts on foot, knocked-out blur, etc.)
```

### 2.4 Estados

```
            +-----------+   F10 (cabin door free)   +-----------+
            | IN_CABIN  | -----------------------> | WALKING    |
            +-----------+                          +-----------+
                ^                                         |
                |  F (door prompt)                      F8 (teleport) | F10 | F
                |                                         v
            +-----------+    (knocked out)         +-----------+
            | FALLEN    | <--------------------- | AIRBORNE  |
            +-----------+                          +-----------+
```

Sub-estados de `WALKING`: `IDLE`, `WALK`, `RUN`, `CROUCH_IDLE`, `CROUCH_WALK`, `JUMP`, `STUMBLE`, `KNOCKED_OUT`.

---

## 3. Layout de archivos

El layout real del árbol (a enero 2026) está en la raíz del repo. La forma
canónica del código:

```
WalkerCore/
├── CMakeLists.txt                 # build con CMake + Ninja (MinGW primary)
├── AGENTS.md                      # convenciones para futuros agents
├── README.md                      # instalación, controles, build
├── LICENSE                        # GPL-3.0-or-later (rel. desde MIT 2026-10-13)
├── docs/
│   ├── roadmap.md                 # este documento
│   ├── architecture.mermaid       # diagrama de capas (provider/workflow/domain/adapter)
│   ├── integration.md             # contrato entre módulos y M-landers
│   ├── testing.md                 # qué verifican los tests automatizados y qué requiere juego
│   └── decisions/                 # ADRs (architectural decision records)
├── sdk/                           # SCS SDK (vendored, viene de modding.scssoft.com)
│   ├── scs_telemetry.h
│   ├── scssdk_telemetry.h
│   └── ...
├── third_party/
│   └── minhook/                   # MinHook (BSD-2) para hookear Present
├── scripts/
│   └── ci_test.ps1                # wrapper: build + tests en un comando
├── include/walkercore/            # headers públicos del dominio (api para tests)
│   ├── CameraState.h
│   ├── FrameLoop.h
│   ├── InputState.h
│   ├── MovementConfig.h
│   ├── PlayerState.h
│   ├── TruckState.h
│   ├── TruckDoorConfig.h
│   ├── WalkerFsm.h
│   ├── WorldState.h
│   ├── Knockout.h
│   ├── HeadTracker.h
│   ├── Flashlight.h
│   ├── DoorAnim.h
│   ├── FuelPump.h
│   ├── Shadow.h
│   ├── Settings.h
│   ├── SettingsMenu.h
│   ├── Time.h
│   ├── Version.h
│   ├── Log.h
│   └── ...
├── src/
│   ├── domain/                    # math pura, sin SCS / D3D / win32
│   │   ├── CameraState.cpp        # view + projection matrices
│   │   ├── GameVersion.cpp
│   │   └── VersionTable.cpp
│   ├── workflow/                  # coordina providers + dominio
│   │   ├── FrameLoop.cpp          # PhaseSampleInput / Integrate / Resolve / Animate / PushCamera
│   │   ├── SettingsMenu.cpp
│   │   └── WalkerFsm.cpp          # Driving / TransitionOut / Walking / TransitionIn
│   ├── provider/                  # adapters para SCS, D3D11, raw input, head tracker, etc.
│   │   ├── D3D11Hook.cpp          # M2 lander — MinHook'd Present + per-frame conductor
│   │   ├── InputHook.cpp          # M4 — WH_KEYBOARD_LL + raw input
│   │   ├── ScsTelemetryProvider.cpp # M1 — registra canales SCS
│   │   ├── PhysXAdapter.cpp       # M3 stub — open world, stub-warn
│   │   ├── Knockout.cpp           # M10 stub — heurística velocidad
│   │   ├── TrailerCoupling.cpp    # M9 stub — waypoint anchor
│   │   ├── FuelPump.cpp           # M5/M8 partial — station position placeholder
│   │   └── PolishProviders.cpp    # M7 stub — flash/head/door anim
│   └── adapter/                   # C ABI + log facade
│       ├── PluginEntry.cpp        # DllMain + scs_telemetry_{init,shutdown}
│       ├── DiagnosticDump.cpp     # F12 dump
│       ├── Log.cpp                # ringbuffer + game.log mirror
│       └── ...
├── tests/                         # nativos, no cargan la DLL
│   ├── TestHarness.h              # framework propio (EXPECT_EQ, EXPECT_NEAR, ...)
│   ├── TestStubReset.h            # extern "C" reset hooks para honest-stub warns
│   ├── test_main.cpp              # RUN_TEST(...) por cada caso + TEST_SUMMARY final
│   └── test_*.cpp                 # uno por componente probado
└── build-ninja/libWalkerCore.dll   # output del build
```

El conjunto exacto de archivos .cpp y .h vive en disco (no se mantiene
manualmente este árbol en el roadmap — `git ls-files` es la fuente
de verdad).

---

## 4. Decisiones de implementación

| Decisión | Opción propuesta | Motivo | Alternativas |
|----------|------------------|--------|--------------|
| Estilo de export DLL | `extern "C"` (C ABI) | Coexistencia con cualquier toolchain del SCS SDK | C++ ABI (riesgo) |
| Runtime C | estático de MinGW | Sin dependencia de `libgcc_s_seh-1.dll` | dinámico (más simple, +1 DLL) |
| STL en frontera DLL | prohibido | Evitar incompatibilidades ABI de método | encapsular (más código) |
| Logging | `scs_log` al game.log.txt + ringbuffer interno para `F12` dump | Soporte oficial + diagnóstico | OutputDebugString (no visible) |
| Hook de cámara | D3D11 swap-chain Present | Más invasivo pero estable entre versiones del juego | free-camera del juego (necesita `g_developer 1`, es lo que usa TMRealWalk) |
| Hook de input | `WH_KEYBOARD_LL` + raw input | Aislar keys sin tocar el buffer de DirectInput del juego | leer buffer del juego (frágil entre versiones) |
| Malla de colisión | recostruir desde los modelos del juego | Permite precisión real pero requiere entender el formato .pmd/.pmg | usar PhysX directamente (mucho más trabajo) |
| Persistencia | INI en la carpeta del plugin | Compatible con el ecosistema | JSON (no, los demás mods son INI) |
| Idiomas | empezar en EN, infraestructura lista | Evita scope creep | traducir desde día 1 |
| Telemetría | **NO** enviar nada a la red | Compat con la política de no monetización | scope creep |

### Decisiones tomadas en sesión

| ID | Decisión | Estado |
|----|----------|--------|
| D1 | Nombre del binario: `libWalkerCore.dll` (prefijo `lib` por convención CMake) | Aceptada |
| D2 | Licencia: GPL-3.0-or-later (rel. desde MIT 2026-10-13) | Aceptada |
| D3 | Multiplayer: no se bloquea por defecto (warning en log) | Aceptada |
| D4 | Toolchain: MinGW-w64 GCC | Aceptada |
| D5 | Datos de usuario: `%APPDATA%\WalkerCore` | Default (cambiable vía ADR) |
| D6 | Idiomas: solo EN en M0, infraestructura lista | Default (cambiable vía ADR) |

Otras decisiones de arquitectura (estilo de export, runtime C, sin STL en frontera, hook de cámara D3D11, hook de input raw, sin telemetría) están en la tabla de arriba; cualquier cambio se registra en `docs/decisions/` como ADR.

---

## 5. Fases de implementación

### M0 — Esqueleto compilable ✅ (código listo, sin build verificado por falta de compilador en la máquina)

- `DllMain` que escribe `[WalkerCore] loaded (build<hash>)` en `game.log.txt` y devuelve `TRUE`.
- `scs_telemetry_init` registrado con SDK version correcta (1.00 y 1.01).
- `scs_telemetry_shutdown` que escribe `[WalkerCore] unloaded`.
- Build con CMake produce `WalkerCore.dll` que el juego carga sin crashear.
- **Estado actual.** Código escrito, pendiente de que el usuario instale un compilador (gcc/cl/clang + cmake) para verificar.

### M0.5 — Tabla de offsets por versión del juego ✅ (código listo)

- `GameIdentity` que identifica el juego (`eut2` / `ats`) y la versión SDK reportada.
- `VersionTable` con `Lookup(GameIdentity)` que devuelve el `VersionEntry` correspondiente o `nullptr`.
- `VersionEntry` con `SubsystemEntry` por subsistema y bitmask de `Feature`.
- `PluginEntry` loguea qué build detectó y avisa si no está en la tabla.
- Lista cerrada inicial: ETS2 1.61.1.0..1.61.1.1, ATS 1.61..1.61.3.1 (alineado con TMRealWalk).
- Detalle en [ADR-001](decisions/ADR-001-version-table.md).

### M1 — Lectura de estado

### M1 — Lectura de estado ✅ (código listo)

- `TruckState` (dominio puro, sin SCS en headers): `Placement`, `Vec3`, `Orientation`, booleans y `has_*` flags.
- `ScsTelemetryProvider` (provider) registra 4 canales vía `v100->register_for_channel`:
  - `truck.world.placement` (dplacement)
  - `truck.local.velocity.linear` (fvector) — cache de `speed_mps`
  - `truck.brake.parking` (bool)
  - `trailer.connected` (bool)
- Callbacks `extern "C"` con `SCSAPIFUNC`, contexto = `this` del provider.
- `PluginEntry` instancia el provider en `scs_telemetry_init`, destruye en `scs_telemetry_shutdown`. El SDK auto-unregistra canales al shutdown.
- **Estado actual.** Código escrito, pendiente de build verification.

### M2 — Cámara (parcial: código de orquestación listo, hook D3D11 es stub)

- `PlayerState` (dominio) — posición de pies, yaw/pitch en radianes, altura de ojo, modo de movimiento.
- `CameraState` (dominio) — eye position, matrices view + projection (row-major float 4×4, FOV horizontal, D3D LH clip).
- `FrameLoop` (workflow) — `RunOnce()` orquesta las fases: read mouse → integrate movement → resolve collision → push camera. Cada fase es stub M2 y se reemplaza en M3/M4.
- `D3D11Hook` (provider) — interfaz completa + stub de implementación. **El hook real requiere MinHook y se implementa cuando el compilador esté listo**; mientras es compile-only con logging de "stub".
- Integración en `PluginEntry`: instancia `FrameLoop`, lo enlaza con el `ScsTelemetryProvider`, lo destruye en shutdown.
- Pendiente para M2 real: vendorar MinHook, implementar `D3D11CreateDeviceAndSwapChain` wrapper + Present vtable hook.
- Canal `walkercore.config` con un campo de prueba (un bool `debug_overlay`).
- **Aceptación.** El panel de diagnóstico (F12) muestra la posición y orientación del camión en tiempo real.

### M2 — Cámara controlada
- Hook `IDXGISwapChain::Present` con MinHook.
- Enganchar `ID3D11Device::CreateRenderTargetView` para capturar el `ID3D11DeviceContext`.
- Reescribir view + projection cada frame desde `PlayerState`.
- Toggle con F10 desde `IN_CABIN`.
- Detección de TruckersMP/convoy: si está activo, F10 muestra un mensaje y no entra a `WALKING`.
- **Aceptación.** F10 saca la cámara del camión. F10 otra vez la devuelve. En TruckersMP el plugin muestra el mensaje y no se mueve.

### M4 — Caminar
- WASD = avance/retroceso/strafe. Shift = correr. Ctrl = agachar. Space = salto. E/Q = subir/bajar ojos (R = reset). Mouse = look. Wheel = velocidad de paso.
- Integración con timestep fijo (`dt = 1/60`, substep si frame lento).
- Ground snap con `WorldState`.
- Colisión: sphere-cast contra AABB del mundo.
- Animación: bob vertical + sway lateral sinusoidal modulado por velocidad.
- **Aceptación.** El personaje camina, corre, se agacha, salta, choca contra edificios. Sale del camión al pulsar F10.

### M5 — Entrar / salir
- Detección de puerta (raycast hacia la cabina del camión desde la posición del jugador).
- Transición: 1.5 s blend cámara a posición de asiento.
- Sub-estado `ENTERING` / `EXITING` con tiempo protegido.
- **Aceptación.** Pulsar F cerca de la puerta del propio camión te sienta. Pulsar F10 dentro de la cabina te baja.

### M6 — Configuración
- INI parser simple (`key = value`, secciones, comentarios, comillas).
- Defaults sensatos para velocidad de paso, sensibilidad de ratón, etc.
- Live reload al guardar el INI (watch file).
- **Aceptación.** Editar el INI con un game en marcha cambia el comportamiento sin reiniciar.

### M7 — Pulido mínimo
- Head tracker (TrackIR/opentrack): mezcla con el look de ratón.
- Sombra: mesh plano con textura procedural sobre la posición del jugador.
- Soft-chrome: gradiente descendiente del personaje.
- **Aceptación.** Una sombra blanda sigue al jugador y se proyecta en superficies inclinadas.

### M8 — Features avanzadas (futuras)
- Linterna con haz dinámico.
- Acoplar/descabollar tráler paso a paso.
- Impacto de tráler / sonido de atropello.
- Localización a 16 idiomas.
- Pantalla de settings con tabs.

---

## 6. Estrategia de prueba

| Capa | Cómo |
|------|------|
| Dominio (PlayerState, Collision, Config) | Ejecutables de test nativos en CMake (gtest o doctest) sin tocar DLL |
| Providers | Test de carga/descarga del SDK con un transport simulado (header de SCS tiene stubs) |
| Frame loop | Test de integración: tick del FrameLoop con mock de providers, assert de invariantes |
| D3D11 hook | Solo verificable en juego real |
| INI parsing | Tabla de strings + asserts |

**No hay manera de verificar el rendering sin juego.** Las fases que tocan cámara o input se cierran manualmente en ETS2/ATS. Para cada fase:
1. Build local (MinGW).
3. Copiar al `plugins/` correspondiente.
4. Arrancar juego, cargar save.
5. Verificar criterio de aceptación.
6. Capturar el game.log.txt.
7. Iterar.

---

## 7. Riesgos y desconocidos

| Riesgo | Mitigación |
|--------|-----------|
| Formato de malla de colusión de SCS no documentado al detalle | Empezar con raycast contra AABB aproximada por edificio; refinar cuando sepamos |
| Cambios entre versiones del juego rompan offsets | Mantener provider `ScsTelemetryProvider` fino y detrás de interfaz para reemplazarlo rápido |
| Antivirus marquen el DLL como sospechoso | Firmar el binario (self-signed), versionar visible, README explicando por qué necesita hooks |
| Multiplayer detection falle | Confirmar lectura de `game.multiplayer` en SDK |
| TMRealWalk siga evolucionando y deje obsoleto lo nuestro | Es esperado; nuestra ventaja es ser código abierto, simple y modificable |

---

## 8. Próximo paso

Una vez confirmado este roadmap, arrancamos con **M0**:

1. Vendorar la SDK de SCS (header files) en `sdk/`.
2. Vendorar MinHook en `third_party/minhook/`.
4. Escribir `CMakeLists.txt`, `src/Main.cpp`, `src/adapter/PluginEntry.cpp`, `src/adapter/Log.cpp`.
5. Build con MinGW.
6. Probar copiando al `plugins/` de ATS o ETS2.
7. Verificar las dos líneas en `game.log.txt`.