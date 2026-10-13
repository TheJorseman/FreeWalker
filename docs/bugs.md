# WalkerCore — Bug analysis por milestone

> **Propósito.** Inventario de bugs abiertos / regresiones / workarounds pendientes por M, con severidad y plan de mitigación. Snapshot a HEAD `43c9e06` (Oct 2026). Se regenera después de cada ciclo de fixes.

## Severidades

- **Bloqueante.** El usuario no puede completar el caso de uso del M (e.g. `walking → dev camera` no funciona aunque el shim esté activo).
- **Mayor.** Funcionalidad parcial / workaround feo documentado; el usuario puede seguir pero con pérdida.
- **Menor.** No bloquea al usuario directamente; deuda técnica que complica la siguiente iteración.
- **RE-bloqueado.** No se puede cerrar sin un live debugger o un RE pass del binario del juego que el autor no tiene capacidad de hacer sin juego corriendo.

---

## M0 — Skeleton compilable
- ✅ Sin bugs abiertos.

## M0.5 — Tabla de offsets por versión
- ✅ Sin bugs abiertos. Cubre ETS2 1.61.1.0..1.61.1.1 + ATS 1.61..1.61.3.1.

## M1 — Lectura de estado (SCS telemetry)
- ✅ Sin bugs abiertos. 4 canales registrados (`truck.world.placement`, `truck.local.velocity.linear`, `truck.brake.parking`, `trailer.connected`).

## M2 — GameFreeCameraShim
**Estado general.** Smart-default auto-enable + o_im_freecam probe + F11 default + one-shot INI migration en HEAD `c46fc23`.

- ✅ Bug CERRADO en M2.5 (`1cdd8a5` + `481958f` + `c46fc23`): el action list original era `{"dev_camera_toggle", "camdbg", "walk_mode", "free_camera"}` — todos rechazados por el parser de input SCS 1.3+ (`[inp] "<name>" is not valid object name`). Narrowed a `o_im_freecam` por RE del binario (Oct 2026).
- ✅ Bug CERRADO en M2.5 (`481958f`): el log "Walking entry #N → dispatched %d %s" tenía `%s` huérfano que crashaba el test `TestGameFreeCameraShimDispatchSkipsWithoutHwnd` (exit 0xC0000005). Eliminado el placeholder.
- ✅ Bug CERRADO en M2.7 (`c46fc23`): el default VK = 0x60 (VK_NUMPAD0) no toggla el dev cam en `amtrucks.exe` 1.61.3.1s. La evidencia previa que apuntaba a F11 quedó invalidada por observación live del user (Oct 2026): "F11 me lanza a la camara 0", lo que sugiere que **F11 no es el dev free cam toggle directo** — es el atajo del dev console o un cycling-to-cam-0 que coexiste. LaSendInput del shim lo dispara porque la action registry lo enruta, pero el comportamiento visible es "camera reset to 0", no dev free camera. Se mantienendió el default 0x7A como la mejor-apuesta válida en la ausencia de otro anchor (RE: 0 hits para `dev_camera_toggle`/`camdbg`/`walk_mode`/`free_camera`), y el user puede override con `[m2] dev_camera_toggle_vk = 0x77` (F8) u otro VK. INI migration 0x30/0x60 → 0x7A añadida.
- ✅ Bug CERRADO en M2.7a (HEAD actual): stale strings en `GameFreeCameraShim.cpp` y `.h` que aún decían "top-row '0' / VK_0" o "0x30" cuando el default real es 0x7A (F11). Limpiados: el warning ahora describe el comportamiento real del shim (SendInput al action registry) sin etiquetar erróneamente a F11 como hardcoded SCS dev cam shortcut. Header doc actualizado para notar la observación del user.
- ⚠️ Pendiente MAYOR: el user debe activar el shim manualmente (`game_free_camera_shim = true` en INI) o añadir `mix o_im_freecam \`keyboard.f8?0\`` en `controls.sii`. El smart-default no flipea si no encuentra binding a `o_im_freecam` en el profile del usuario (la mayoría de las configs stock no lo tienen). Documentado en `docs/walkthrough.md` §3.
- ⚠️ Pendiente MAYOR: el shim requiere `g_console=1` (no solo `g_developer=1`) para que la dispatch llegue al dev free cam. La mayoria de las configs stock NO tienen `g_console=1` — hay que editar `config.cfg` a mano o ejecutar `g_console 1` en la dev console del juego. Documentado en walkthrough §4.
- ⚠️ Pendiente MAYOR (Oct 2026 user report): F11 nativo en ATS tiene un efecto visible no controlado por WalkerCore (cycles to camera 0). Para evitar la interacción, el user puede fijar `dev_camera_toggle_vk = 0x77` (F8) u otro VK que no choque con el binding nativo del juego.

## M2 long-term — Camera state dispatcher hijack (in-process)
**Estado general.** Stub MinHook-ready, default `false` (safe). RE-bloqueado para producción — la signature del dispatcher de SCS camera state en `amtrucks.exe` no se ha encontrado todavía.

- ✅ Bug CERRADO en M2 long-term v1+v2 (HEAD actual): el camera state dispatcher **SÍ está localizado** vía signature walk en REA repo (`07a0e579` commit, Oct 2026). RVA 0xC22F40 confirmado por la técnica de "unique signature scan": de 101 hits de `FF 92 80 00 00 00` (call qword ptr [rdx+0x80]) en `.text` (31 MB), el filtro único (pre-call override `mov dword [rbx+0x30], 0x42B40000` + post-call restore `F3 0F 11 73 30` + `F3 0F 11 7B 34` dentro de ±256 bytes) redujo los 101 a **1 solo survivor**: 0x140C2306D, el vtable[16] call dentro del camera state dispatcher. Por Transitividad, la función que contiene esa call (la dispatcher) está en 0x140C22F40 (RVA 0xC22F40). El CameraHijack provider en `src/provider/CameraHijack.cpp` ya implementa v1 (dispatcher MinHook) + v2 (lazy singleton walk + vtable[16] MinHook), con default `camera_hijack_install = false`. 13 tests unitarios cubren el resolver math.
- ℹ️ Pendiente v3: matrix override — reemplazar las matrices que el SCS state machine escribe en el GPU constant buffer. Requiere vtable[16] trampoline que lea/escriba el constant buffer push. La estructura del buffer no se ha RE-ado por estar en runtime; el próx ciclo con live debugger o render-doc capture avanzaría v3.
- 🔒 RE-BLOQUEADO MAYOR: `DevCameraCvarPatcher` parchea `g_developer` en memoria. El RVA 0x22408C0 (file offset 0x223F6C0) es donde vive el string del cvar, pero el value field real está más lejos (la probe ±8 kB no encuentra firma consistente). Fix requiere signature del layout del cvar struct en este build específico — el autor tiene `g_developer=1` en `config.cfg` así que el patcher es nice-to-have, no bloqueante.
- ℹ️ Menor: durante el ciclo M2.5, los 4 MinHooks críticos (`d3d11_hook`, `d3d12_hook`, `camera_hijack_install`, `px_scene_hook_install`) se auto-migran a `false` si un INI legacy los tenía en `true` con un WARNING explicando la incompatibilidad de mod stack (ReShade+OptiScaler+RenoDX → `movdqa` 16-byte misalignment en `amtrucks.exe+0x1d5300a`). El sentinel system no se respeta para estas claves porque la regresión es unsafe, no configurable. Documentado en `docs/integration.md`.

## M3 — PxScene queries
**Estado general.** Catalog de 8 métodos identificado (release/2, addActor/11, simulate/57, shiftOrigin/117, sweep/4-of-5, raycast/15, overlap/13, removeActor/14). PxScene vtable @ RVA 0x25020A0.

- ✅ Bug CERRADO en M3.1 (HEAD actual): `sweep_slot` no estaba en el main vtable (0x25020A0) — vive en un 5-slot run independiente @ 0x2507C08. La solución añadida es un nuevo helper `PhysXAdapter::ResolveSweepInSecondaryVtable(bytes, size, secondary_offset)` que escanea exactamente los 5 slots del run secundario, busca el catalog `sweep` VA (0x141B06F10) dentro de `kPhysXResolveSlotTolerance=8` bytes, y devuelve el slot index local (0..4). La integración en `PhysXAdapter_RuntimeScan` corre el helper después del main scan si el offset 0x2507C08 está dentro del .rdata, y deja `sweep_slot = 4` + `has_secondary_sweep = true` cuando hay match. 6 nuevos unit tests cubren: catalog-sweep-en-slot-4, no-match, tolerance positiva (+5 bytes), tolerance rechazada (+32 bytes), offset honrado, constantes del header.
- 🔒 RE-BLOQUEADO MENOR: el slot 14 (`PxScene::removeActor`) está confirmado por el string `"PxScene::removeActor(): Actor"` en el error path, pero el prologue no encaja con `addShape`/`setActorFlag` esperados — sigue marcado como "thin dispatcher" en `docs/observations/m3-progress-2026-10.md`. El nombre podría ser `addActors` (plural) o un wrapper interno. Live breakpoint needed para identificarlo con certeza.
- ℹ️ Mayor: `PhysXAdapter` corre como `no live process` durante los tests unitarios y durante el dump inicial — el `GetModuleHandleA("amtrucks.exe")` retorna NULL fuera del juego. Solo en producción real las queries retornan hits. Los tests sintéticos en `tests/test_physx_adapter.cpp` validan la lógica del catalog sin necesidad del juego.
- ℹ️ Mayor: `PhasePushCameraToRenderer` + `PhaseResolveCollision` son stubs hasta que M3 hook esté realmente live con `px_scene_hook_install = true`. Sin hook real: ground-snap a `floor_y = truck.placement.y` (clamp duro, evita caída infinita), collision queries retornan no-hit, player atraviesa geometría del mundo. Workaround aceptado.
- ✅ CERRADO: hooks reales de `PxScene::addActor` Y `PxScene::release` Y otros se pueden SkIPEAR porque `2/8 vtable slots` son carga/descarga, no query. Mantener el resto (`raycast`, `overlap`, `sweep`) como live hooks una vez `px_scene_hook_install = true` requiere reverificar mod-stack safety. Actualizado: PxSceneHook.cpp describe los 4 critical hooks como opt-in con sentinel manual.

## M4 — Caminar (input + integración física)
- ✅ Sin bugs abiertos. WASD + Shift + Ctrl + Space + E/Q/R + Mouse + Wheel todos via InputHook (WH_KEYBOARD_LL) + WM_INPUT. Movimiento integrado en PhaseIntegrateMovement.

## M5 — Entrar / salir (per-truck door)
- ⚠️ Menor: 14 ATS trucks hardcoded en `PerTruckDoorTable.cpp`. Trucks nuevos (DLCs) caen al placeholder genérico. Sin auto-extend mechanism. RE-blocked: la lista completa de `model_id` requiere enumerar TODOS los prefabs del juego.

## M6 — Configuración (INI)
- ✅ CERRADO en `c46fc23` (M2.7): migration block one-shot en LoadFromPath para sentinel-managed values con defaults legacy (0x30/0x60 → 0x7A). Ver `src/adapter/Settings.cpp`.
- ✅ CERRADO en `c46fc23`: comentario "Default VK_NUMPAD0 (0x60)" en `dev_camera_toggle_vk` save-block reemplazado por docs VK_F11. Validated por el test round-trip.
- ⚠️ Menor: los hot-paths de sentinel management requieren que el archivo INI esté bien-formado (UTF-8 sin BOM, líneas LF/CRLF aceptadas). Una edición manual con caracteres non-ASCII puede romper el sentinel parser. No documentado en walkthrough — pendiente.

## M7 — Polish mínimo (linterna, head tracker, shadow, door anim)
- ⚠️ Menor: `PolishProvider::HeadTrackerProvider` es stub (log WARNING al boot). TrackIR/opentrack feed no se aplica aún. Hay `OpentrackReceiver.cpp` completo y compilable, pero el mixer con la pipeline mouse-look está pendiente.
- ⚠️ Menor: linterna + shadow + door anim solo computan state, no empujan render pipelines (M7 lander partial).
- ⚠️ Menor: 8 WAVs embebidos en TMRealWalk (`TMRealWalk_Door.WAV`, `TMRealWalk_Footstep1.WAV`, …) sugieren que el siguiente M lander debería añadirlos al lado del DLL para footstep/door sound. No implementado aún. Spec en `docs/observations/tmrealwalk-findings.md`.

## M8 — Fuel + station prefab
- ⚠️ Mayor: `FuelPump` lee capacidades per-truck vía configuration event (real). Pero la posición de estaciones (gas station, service area) sigue siendo un placeholder truck-local. El RE del registry `prefab/list_station` en `amtrucks.exe` no se ha hecho — RE-bloqueado.
- ⚠️ Mayor: `AdBlue` (DEF) pump stub igual que diesel — capacidades reales, posición stub.

## M9 — Trailer coupling
- ⚠️ Mayor: `CouplingProvider` telemetry funciona (auto-ForceComplete en `trailer.connected = true`). Waypoint `ApproachToKingPin` es placeholder truck-local hasta que el configuration event entregue `hook_position` por truck. Sin `hook_position`, no sabe dónde está el king pin en world space — el raycast es fake.

## M10 — Knockout
- ✅ Sin bugs abiertos. Heuristic `||player.velocity||` activa (commit `7591c45`). El snapshot de posición prior FSM se re-prima para evitar spurious KO en transiciones.
- ℹ️ Menor: la collision normal real viene de PhysX (post-M3 lander con `px_scene_hook_install = true`). Hasta entonces, la heurística velocity-change es el fallback.

## M11 — Settings menu + ImGui overlay
- ✅ Sin bugs abiertos. F12 dump + Ctrl+F10 menu. Sliders reales (movement + camera), save/load INI, head-tracker toggle, key bindings.

## Game.log audit (líneas relevantes)

Del game.log más reciente (DLL `6CF26C84…`, pre-M2.7 build):

| Línea | Severidad | Mensaje | Acción |
|---|---|---|---|
| 838 | ⚠️ Mayor | `PhysXAdapter::Install: amtrucks.exe not in-process or scan failed` | Esperado en test environment. Producción live en juego. |
| 849 | ⚠️ Menor | `DevCameraCvarPatcher: skipped (could not discover value field for g_developer within +/- 8 kB of name VA 0x7FF631EF08C0)` | Workaround: `g_developer=1` ya está en `config.cfg`. |
| 850 | ℹ️ Info | `M3 lander: PxScene::addActor hook SKIPPED (INI gate px_scene_hook_install = false)` | Por diseño, default-off porque rompe mod stack. |
| 852 | ⚠️ Menor | `PolishProvider HeadTrackerProvider: stub active` | M7 lander partial. |
| 854-855 | ℹ️ Info | `D3D11 / D3D12 hook SKIPPED` | Por diseño, default-off porque rompe mod stack. |
| 1355 | ⚠️ Mayor | `GameFreeCameraShim: Walking entry #1 (gate=off, ...)` | El shim está apagado en el INI del usuario. Acción: `game_free_camera_shim = true` o añadir `mix o_im_freecam` en controls.sii. |
| 1357 | ⚠️ Menor | `FuelProvider: stub active — station position is a truck-local placeholder (M8 partial)` | M8 partial. Real impl needs prefab RE. |
| 1358 | ⚠️ Mayor | `CouplingProvider: M9 lander partial — telemetry works but ApproachToKingPin waypoint is placeholder` | M9 partial. Real hook_position needed. |
| 1363 | ⚠️ Mayor | `PhysXAdapter: stub — collision queries return no-hit. Player will pass through world geometry.` | Esperado sin `px_scene_hook_install = true`. Y-clamp previene caída infinita. |

---

## Próximos pasos sugeridos (orden de impacto)

1. **HIGH — shim M2.7 verification.** Confirmar que tras reiniciar ATS con la DLL `764A5724…`, el INI se migra a `0x7a`, y F10 + `game_free_camera_shim = true` activa dev free cam. Si no, investigar g_console=1 missing.
2. **MEDIUM — M2 long-term RE plan.** Live debugger (LLDB o WinDbg) atachado a `amtrucks.exe` corriendo para:
   - Identificar el camera state dispatcher (camera free-roaming manager vtable slot)
   - Identificar el slot 14 de PxScene (probablemente `addActors` por contexto de firma)
   - Identificar el layout real del cvar struct de `g_developer`
3. **MEDIUM — M3 sweep_slot fix.** Añadir segundo scan pass contra 0x2507C08 a `PhysXAdapter::ResolveKnownMethods`. Pure code change, no RE needed.
4. **MEDIUM — M9 hook_position discovery.** RE del `truck.hook_position` channel en el SCS telemetry SDK. Si existe (probable, la mayoria de los campos truck.* están en el SDK), añadir al `ScsTelemetryProvider` channel registration.
5. **LOW — M8 station prefab RE.** Escaneo del registry `prefab/list_station` — requiere debug pass contra juego corriendo.
6. **LOW — M5 PerTruckDoorTable extension.** Bucle per DLC truck — sin RE cuando hay catálogo prefab.
7. **LOW — M7 PolishProvider stubs.** Reemplazar los logs WARNING por comportamiento real cuando las dependencias estén ready (PhX para ground-snap real, D3D hook para push).
