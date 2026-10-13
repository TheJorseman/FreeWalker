# WalkerCore v0.0.1 — Walkthrough del usuario

Esta guía asume ATS 1.61.3.1s (Steam) + ReShade + OptiScaler (DLSS5) + DXVK como stack objetivo, igual que el stack del autor. Para ETS2 el flujo es paralelo.

> **TL;DR.** El plugin ya hace lo que sabe hacer. Para que **la cámara se mueva visiblemente** al pulsar F10 hay **un solo paso manual** (el resto es automático):
>
> 1. **Copiar `libWalkerCore.dll` a `bin\win_x64\plugins\`** (una vez por versión del juego).
> 2. **Activar el shim — MANUAL o AUTOMÁTICO.** Opción A: pon `[m2] game_free_camera_shim = true` en `%APPDATA%\WalkerCore\WalkerCore.ini` (recomendado). Opción B: añade `mix o_im_freecam \`keyboard.f8?0\`` al final de tu `controls.sii` y el smart-default auto-enable del M2.5 lo activará solo cuando `g_developer=1` esté en `config.cfg`.
> 3. **`g_developer=1` + `g_console=1`** en `Documents\American Truck Simulator\config.cfg` — sin estos dos cvars a `1`, el dev free camera del SCS no se activa aunque el shim dispatche correctamente.
>
> Verificación: entrando a Walking con `game_free_camera_shim = true` + `g_console=1` + `o_im_freecam` binding, el SendInput del shim debería llevar el dev free camera a la vista correcta. **Nota Oct 2026**: la verificación previa que decía "F11 presses salta a dev free cam con overlay" fue invalidada por una live-test del user que observó "F11 me lleva a camera 0" — el comportamiento en este build no es lo que se creía. Por eso el default M2.7 sigue siendo 0x7A pero se recomienda al user fijar `dev_camera_toggle_vk = 0x77` (F8) en el INI y bindear `o_im_freecam \`keyboard.f8?0\`` en controls.sii — el F8 evita el binding nativo de F11 y va por la pipeline de input normal.

---

## 1. Estado actual del plugin (HEAD `c46fc23`)

Funcionalidad que **funciona end-to-end sin tocar nada**:

- **F10** transita `Driving → Walking` (FSM de 4 estados). Ver `00:00:40.292 : [WalkerCore] WalkerFsm: Driving → TransitionOut (tween 0.60s, source=F10)` en `game.log.txt`.
- **F** transita `Walking → Driving`. Recovery limpio con tween de 0.60s.
- **WASD + Shift + Ctrl + Space** son leídos por el `InputHook` (WH_KEYBOARD_LL) y propagados al `FrameLoop` en cada tick.
- **DoorAnim** tweens la puerta del camión (cabin → open al Walking entry, open → closed al Driving entry).
- **FuelPump** lee `fuel_capacity` y `adblue_capacity` desde el evento de configuración del SCS SDK — capacidades reales por camión, no 600L/50L hardcodeados.
- **CouplingProvider** auto-`ForceComplete` cuando `trailer.connected = true` dispara mid-FSM (M9 telemetry-driven).
- **KnockoutProvider** usa `||player.velocity||` (M10 Lander) en lugar de `dist/dt`, evitando spurious KO en transiciones.
- **SettingsMenu + ImGuiOverlay** exponen widgets reales (M11 lander) — sliders de movimiento, sliders de cámara, head-tracker toggle, key bindings.
- **OpentrackReceiver + HeadTrackerProvider** listos para opentrack/TrackIR (M7).
- **DiagnosticDump (F12)** muestra todos los contadores, los estados internos, los providers y las matrices de cámara.
- **GameFreeCameraShim M2.7** sintetiza VK_F11 (0x7A, default) al entrar a Walking si el shim está activo + `g_developer=1` AND `g_console=1`. El default legacy M2.5/2.6 era VK_0/Numpad-0 (0x30/0x60) — ninguno de esos toggles el dev free camera en `amtrucks.exe` 1.61.3.1s. La verificación live del switch a F11 fue invalidada por observación posterior del user (Oct 2026): "F11 me lleva a camera 0" en vez de dev free cam — el comportamiento real es ambiguo. Por eso la recomendación actual es fijar el user `dev_camera_toggle_vk = 0x77` (F8) en el INI y bindear `o_im_freecam \`keyboard.f8?0\`` en `controls.sii`.

Funcionalidad que **funciona pero el efecto visual no es inmediato**:

- **GameFreeCameraShim** envía el `toggle_vk_` (default VK_F11 = 0x7A) a `amtrucks.exe` cuando entramos a `Walking`. El envío es correcto (SendInput contra el HWND del juego) — pero sin `g_developer=1` AND `g_console=1` en `config.cfg` el SCS dev shortcut se ignora y la cámara no se desbloquea. **Recomendación**: usar `dev_camera_toggle_vk = 0x77` (F8) en el INI y bindear `o_im_freecam \`keyboard.f8?0\`` en `controls.sii` — F8 evita la interacción nativa de F11 con `cam 0` cycling.

Funcionalidad que **está bloqueada por el mod stack**:

- **D3D11Hook / D3D12Hook / CameraHijack / PxSceneHook** son safe-default-off. La trampoline de MinHook rompe la alineación de 16-byte del `movdqa` downstream en `amtrucks.exe+0x1d5300a` cuando hay ReShade + OptiScaler + RenoDX encima de system `d3d11.dll`. Esto NO es un bug de WalkerCore — es una incompatibilidad conocida del mod stack.
- **PhasePushCameraToRenderer** en `FrameLoop` es un no-op stub — la pipeline D3D no se puede enganchar de forma segura con el mod stack actual. El workaround real es el shim: activar la SCS dev free camera nativa (que es fly-camera en modo developer), en vez de empujar matrices nosotros mismos.

---

## 2. Desplegar el DLL

```powershell
# Build path
$src = 'C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\build-ninja\libWalkerCore.dll'
$dst = 'C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\plugins\libWalkerCore.dll'
Set-Location 'C:\Program Files (x86)\Steam\steamapps\common\American Truck Simulator\bin\win_x64\plugins'
Copy-Item -LiteralPath $src -Destination 'libWalkerCore.dll' -Force
```

Verifica el SHA antes de copiar:

```powershell
Get-FileHash $src -Algorithm SHA256
```

El DLL actual del repo (HEAD `c46fc23`, M2.7):

```
SHA256: 764A5724EBE50C66975D04AA184690B668A0F6B0E39F00C6E8C77E4744CA3E5F
Size:   ~3.93 MB
```

(Para confirmar el SHA actual del DLL desplegado: `(Get-FileHash "$dst" -Algorithm SHA256).Hash`.)

El juego carga el DLL al arrancar (vía el loader de SCS plugins). Verás en `game.log.txt`:

```
00:00:00.000 : [WalkerCore] loaded (build 764A5724…)
```

Si **no** ves esa línea, el juego no cargó el plugin. Razones comunes:

- La ruta del DLL no es `bin\win_x64\plugins\` (algún mod loader reordena la lista).
- El DLL no se copió bien (verifica el SHA de destino).
- Otra copia más vieja está ganando la carrera (limpia `plugins/`).

---

## 3. Activar el shim (M2 GameFreeCameraShim)

Hay dos caminos:

### Opción A — manual, sin tocar `controls.sii`

Edita `%APPDATA%\WalkerCore\WalkerCore.ini` y cambia:

```ini
[m2]
game_free_camera_shim = true
```

(Borra la línea `# walkercore:managed:game_free_camera_shim` que está justo debajo si quieres que el valor quede "explícitamente seteado por el usuario" — sin sentinel el parser lo trata como `explicit=true` y el smart-default no intentará sobreescribirlo.)

Sin más cambios. El shim queda activo y usará el default VK_F11 (0x7A, M2.7) cuando entres a Walking. Reinicia ATS para que cargue el INI.

### Opción B — automática vía `controls.sii` (smart-default M2.5)

El smart-default detecta un binding a `o_im_freecam` en `controls.sii` + `g_developer=1` y flipea `game_free_camera_shim` de `false → true` por su cuenta. Pasos:

1. Abre `Documents\American Truck Simulator\profiles\<id>\controls.sii` (el `<id>` activo está en `game.log` o en `last_session_config.sii`).
2. Añade `config_lines[601]` (o el siguiente índice libre):
   ```
   config_lines[601]: "mix o_im_freecam `keyboard.f8?0`"
   ```
   Eso bindea F8 a `o_im_freecam` — la única acción SCS válida confirmada por RE del parser de input (`dev_camera_toggle` / `camdbg` / `walk_mode` / `free_camera` no existen como objetos válidos en amtrucks.exe 1.61.3.1s).
3. Reinicia ATS.

> **Por qué F8 y no F11.** F8 es la primera function key libre por scan de tu `controls.sii`, sin colisiones. F11 nativo en este build tiene un efecto no controlado (cycles a camera 0), y aunque el shim dispare SendInput(F11), el receiver SCS tiene prioridad nativa sobre la action registry. F8 va por la pipeline de input normal sin colisión.

---

## 4. Configurar los cvars en `config.cfg`

El shim necesita `g_developer=1` AND `g_console=1` en `config.cfg`. Edita `Documents\American Truck Simulator\config.cfg` y añade (o verifica que existan):

```
uset g_developer "1"
uset g_console "1"
```

Sin ambos a `1`, el SCS dev shortcut F11 se ignora. El shim puede estar enviando VK_0x7A correctamente, pero la cámara no se mueve si falta alguno de los cvars.

> **Alternativa.** Si no quieres editar `config.cfg` a mano, abre la dev console del juego (~) tras cargar un save y ejecuta `g_developer 1` y `g_console 1`. Los cambios son para la sesión actual pero suficientes para verificar el shim.

---

## 5. Verificar end-to-end

Con el shim habilitado + binding detectado + cvars correctos:

1. Pulsa **F12** en juego para abrir el dump de diagnóstico.
2. Busca la sección `--- WalkerCore game-free-camera shim ---`. Deberías ver:
   ```
   enabled             : true
   supported           : true
   walks_entered       : 0
   toggles_dispatched  : 0
   toggle_vk (hex)     : 0x7a      ← VK_F11 (default M2.7)
   ini_overrides_vk    : false
   auto_detected_key   : (none)
   controls_sii_path   : C:\Users\migue\...\controls.sii
   ```
3. **Sal del camión** (pulsa F10). Debería aparecer en `game.log`:
   ```
   [WalkerCore] WalkerFsm: Driving → TransitionOut (tween 0.60s, source=F10)
   [WalkerCore] WalkerFsm: TransitionOut complete → Walking
   [WalkerCore] CameraState on Walking entry: eye=(...) yaw=... pitch=0.000 ...
   [WalkerCore] GameFreeCameraShim: Walking entry #1 (gate=on, config-cfg[probe]='1',
                  hwnd=0x...) — dispatched toggle VK=0x7a
   ```
4. La SCS dev free camera debería activarse (vista libre con overlay de debug mostrando tu world-space position). WASD mueve la cámara, mouse rota, mouse wheel cambia velocidad, Shift corre.

Si el paso 4 no funciona (la cámara no se mueve), comprueba:

| Síntoma | Causa probable | Fix |
|---------|---------------|-----|
| `gate=off` en el log | El shim está deshabilitado en el INI | `game_free_camera_shim = true` en `[m2]` |
| `dev_cvar_patcher=skipped cvar_va=0x0` | DevCameraCvarPatcher RVA miss | Asegúrate `g_developer=1` está en `config.cfg` (no es necesario patch en memoria si ya está en disco) |
| `Walking entry #N` pero sin `dispatched toggle VK=` | El shim se saltó el dispatch | Revisa el log — debería decir por qué. Posible: `gate=off`, `config-cfg[probe]='0'` (g_developer falta), o `hwnd=NULL` (juego no tenía foco). |
| Press F10 → presiona F11 manual → no pasa nada | `g_console=0` o `g_developer=0` | Verifica `uset g_console "1"` y `uset g_developer "1"` en `config.cfg` |
| El SendInput se logea pero F11 manual en game no hace nada | El mod stack (ReShade + OptiScaler + DXVK) está bloqueando SendInput | Workaround: cambia a binding `keyboard.X` real (no uses F11). `mix o_im_freecam \`keyboard.f8?0\`` en controls.sii + `dev_camera_toggle_vk = 0x77` en INI — el shim sintetizará F8 que va por la pipeline de input normal. |
| `path=none` en el log | El HWND del juego no se enlazó al inicio | Espera a la pantalla principal (no splash) antes de F10. El shim re-bindea cada Walking entry. |

---

## 6. Lo que NO está implementado (y por qué)

| M | Estado | Razón |
|---|--------|-------|
| D3D11 Present hook | OFF por default | El trampoline de MinHook rompe `movdqa` 16-byte alignment en `amtrucks.exe+0x1d5300a` con ReShade + OptiScaler + RenoDX. |
| D3D12 Present hook | OFF por default | Mismo rationale. |
| CameraHijack | OFF por default | Same. |
| PxScene::addActor hook | OFF por default | Same. |
| PhasePushCameraToRenderer | No-op stub | El D3D push no se puede enganchar de forma segura. Solución real: SCS dev free camera vía shim (que es lo que hace este walkthrough). |
| 3D world collision (sphere-cast) | SCS SDK stub | El hook de PhysX tiene el mismo crash. Por ahora, un piso duro (`floor_y = truck.placement.y`) previene caída infinita. |
| M8 station prefab | Parcial | `fuel_capacity` / `adblue_capacity` llegan por configuration event (real). Posición de estaciones requiere RE de `prefab/list_station` en `amtrucks.exe` — RE-blocked sin live debugger. |
| M9 coupling waypoint | Parcial | `trailer.connected = true` drives `OnTrailerAttachStateChanged` (telemetry). `hook_position` vía configuration event — cuando llega, el waypoint world-space gana al placeholder truck-local. |

Lo que **sí** está implementado (no depende de hooks):

- 4-channel SCS telemetry (placement, velocity, parking, trailer).
- 9+ per-truck cabin / head anchors vía SCS configuration event.
- Mouse look + WASD integration (input → camera en cada frame).
- Ground-snap floor clamp (sin colisión real, pero evita caída infinita).
- KnockoutProvider con `||v||` metric.
- FuelPump con capacidades per-truck reales.
- DoorAnim tween on transition.
- F12 dump completo.
- ImGui settings menu (sliders reales, save/load INI).

---

## 7. Comandos útiles

| Acción | Resultado |
|--------|-----------|
| `F10` en juego | Driving ↔ Walking (tween 0.60s) |
| `F` en Walking cerca del camión | Walking → Driving (recovery) |
| `Ctrl+F10` | Abre el settings menu (ImGui) |
| `F12` | Dump de diagnóstico a game.log |
| `~` (consola del juego, con `g_developer=1` AND `g_console=1`) | Toggles dev mode (manual; permite spawn, climate, time, etc.) |
| `F11` (con `g_developer=1` AND `g_console=1`) | Toggle SCS dev free camera — mismo flujo que el shim |

---

## 8. Referencias internas

- `docs/observations/tmrealwalk-findings.md` — RE del mod de referencia.
- `docs/observations/tmrealwalk-cam-manager-re.md` — Por qué RE-blocked para el camera manager dispatcher (signature drift entre builds).
- `docs/integration.md` — Contratos entre módulos + M-landers.
- `docs/roadmap.md` — Roadmap de ingeniería.

---

## 9. Si quieres desactivar WalkerCore temporalmente

Renombra `libWalkerCore.dll` en `bin\win_x64\plugins\` (o muévelo a otro directorio). El juego arranca sin él y todo el comportamiento vuelve al stock de SCS.

Para desactivarlo limpio, pon `[m2] game_free_camera_shim = false` en el INI — WalkerCore sigue cargando, pero el shim no dispatchea y la cabina del juego se queda en control total.
