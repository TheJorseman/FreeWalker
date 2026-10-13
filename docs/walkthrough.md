# WalkerCore v0.0.1 — Walkthrough del usuario

Esta guía asume ATS 1.61.3.1s (Steam) + ReShade + OptiScaler (DLSS5) + DXVK como stack objetivo, igual que el stack del autor. Para ETS2 el flujo es paralelo.

> **TL;DR.** El plugin ya hace lo que sabe hacer. Para que **la cámara se mueva visiblemente** al pulsar F10 hay dos pasos que **el usuario debe completar a mano** (el tercero es automático):
>
> 1. **Copiar `libWalkerCore.dll` a `bin\win_x64\plugins\`** (una vez por versión del juego).
> 2. **Añadir una línea a `<profile>\controls.sii`** que enlace un tecla a la acción `dev_camera_toggle`.
> 3. **Activar el shim — AUTOMÁTICO.** El smart-default auto-enable del M2.5 detecta el binding en controls.sii + `g_developer=1` y flipea `game_free_camera_shim` de `false → true` por su cuenta. Si prefieres controlarlo a mano, pon `[m2] game_free_camera_shim = true` en WalkerCore.ini (o ábrelo desde Ctrl+F10).

---

## 1. Estado actual del plugin (build `054BF84D…`)

Funcionalidad que **funciona end-to-end sin tocar nada**:

- **F10** transita `Driving → Walking` (FSM de 4 estados). Ver `00:00:40.292 : [WalkerCore] WalkerFsm: Driving → TransitionOut (tween 0.60s, source=F10)` en `game.log.txt`.
- **F** transita `Walking → Driving`. Recovery limpio con tween de 0.60s.
- **WASD + Shift + Ctrl + Space** son leídos por el `InputHook` (WH_KEYBOARD_LL) y propagados al `FrameLoop` en cada tick.
- **DevCameraCvarPatcher** parchea `g_developer` en memoria de `amtrucks.exe` (RVA `0x22408C0`, file offset `0x223F6C0`) en cada `scs_telemetry_init`. El path se valida con la cadena literal `g_developer\0` antes de escribir.
- **DoorAnim** tweens la puerta del camión (cabin → open al Walking entry, open → closed al Driving entry).
- **FuelPump** lee `fuel_capacity` y `adblue_capacity` desde el evento de configuración del SCS SDK — capacidades reales por camión, no 600L/50L hardcodeados.
- **CouplingProvider** auto-`ForceComplete` cuando `trailer.connected = true` dispara mid-FSM (M9 telemetry-driven).
- **KnockoutProvider** usa `||player.velocity||` (M10 Lander) en lugar de `dist/dt`, evitando spurious KO en transiciones.
- **SettingsMenu + ImGuiOverlay** exponen widgets reales (M11 lander) — sliders de movimiento, sliders de cámara, head-tracker toggle, key bindings.
- **OpentrackReceiver + HeadTrackerProvider** listos para opentrack/TrackIR (M7).
- **DiagnosticDump (F12)** muestra todos los contadores, los estados internos, los providers y las matrices de cámara.

Funcionalidad que **funciona pero el efecto visual no es inmediato**:

- **GameFreeCameraShim** envía una tecla a `amtrucks.exe` cuando entramos a `Walking` para activar la SCS dev free camera. El envío es correcto (SendInput contra el HWND del juego) — pero **si tu `controls.sii` no tiene un binding `mix dev_camera_toggle`**, la tecla cae en una acción distinta y la cámara no se desbloquea.

Funcionalidad que **está bloqueada por el mod stack**:

- **D3D11Hook / D3D12Hook / CameraHijack / PxSceneHook** son safe-default-off. La trampoline de MinHook rompe la alineación de 16-byte del `movdqa` downstream en `amtrucks.exe+0x1d5300a` cuando hay ReShade + OptiScaler + RenoDX encima de system `d3d11.dll`. Esto NO es un bug de WalkerCore — es una incompatibilidad conocida del mod stack.
- **PhasePushCameraToRenderer** en `FrameLoop` es un no-op stub — la pipeline D3D no se puede enganchar de forma segura con el mod stack actual. El workaround real es el shim: activar la SCS dev free camera nativa (que es fly-camera en modo developer), en vez de empujar matrices nosotros mismos.

---

## 2. Desplegar el DLL

```powershell
# Build path (cambia "054BF84D" por el SHA real del DLL que compiles)
$src = 'C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\build-ninja\libWalkerCore.dll'
$dst = 'C:\Users\migue\OneDrive\Documentos\American Truck Simulator\bin\win_x64\plugins\libWalkerCore.dll'
Copy-Item -Path $src -Destination $dst -Force
```

Verifica el SHA antes de copiar:

```powershell
Get-FileHash $src -Algorithm SHA256
```

El DLL actual del repo (HEAD `32a370c`):

```
SHA256: 054BF84DB611971AB6242A1E4D7750AA495E55CBBDEA4A4F531457FF28390E07
Size:   3,869,446 bytes
```

El juego carga el DLL al arrancar (vía el loader de SCS plugins). Verás en `game.log.txt`:

```
00:00:00.000 : [WalkerCore] loaded (build 054BF84D…)
```

Si **no** ves esa línea, el juego no cargó el plugin. Razones comunes:

- La ruta del DLL no es `bin\win_x64\plugins\` (algún mod loader reordena la lista).
- El DLL no se copió bien (verifica el SHA de destino).
- Otra copia más vieja está ganando la carrera (limpia `plugins/`).

---

## 3. Habilitar el shim (M2 GameFreeCameraShim)

**Automático desde M2.5 smart-default.** WalkerCore auto-detecta el binding `dev_camera_toggle` en controls.sii y flipea `game_free_camera_shim` de `false → true` por sí solo cuando se cumplen tres condiciones:

1. El INI **no** ha nombrado explícitamente `[m2] game_free_camera_shim = ...` (si lo has puesto a `false` a propósito, respetamos tu elección).
2. Hay un binding `dev_camera_toggle` / `walk_mode` / `free_camera` en controls.sii.
3. `g_developer=1` está en `config.cfg`.

Si las tres se cumplen, verás en game.log:

```
[INF] GameFreeCameraShim: smart-default auto-enable (binding=keyboard.f11, VK=0x7A, g_developer=1, INI=[m2] game_free_camera_shim untouched). The shim will now dispatch the toggle key on every F10 transition. To disable, set [m2] game_free_camera_shim = false in WalkerCore.ini.
```

**Si quieres controlarlo a mano**, edita `%APPDATA%\WalkerCore\WalkerCore.ini`:

```ini
[m2]
game_free_camera_shim = true
```

Si el archivo no existe, créalo. WalkerCore escribe un INI por defecto en el primer boot si no lo encuentra — pero el `game_free_camera_shim` default es `false` (post auto-migrate). El auto-migrate **fuerza a false** si lo tenías en `true` de una versión anterior, porque el default `Numpad-0` está bindeado a `dbglockpos` en el perfil stock.

Una vez añadido el binding de `dev_camera_toggle` (paso siguiente), ponlo en `true`.

---

## 4. Añadir el binding `dev_camera_toggle` al `controls.sii`

**Esta es la pieza que falta.** Sin esto, el shim no tiene tecla que enviar.

Abre tu `controls.sii`:

```
C:\Users\migue\OneDrive\Documentos\American Truck Simulator\profiles\6D696775656C31373534366D616C73\controls.sii
```

(El hex es el ID de tu perfil — `miguel17546mals`. Adáptalo al tuyo.)

Busca la última `config_lines[...]` y se inserta una nueva línea al final del bloque `config_lines`. Por ejemplo, si la última línea es `config_lines[510]: ...`, añade:

```
config_lines[511]: "mix dev_camera_toggle `keyboard.f11?0 | semantical.dev_camera_toggle?0`"
```

Eso bindea **F11** a `dev_camera_toggle`. Cualquier otra tecla que no esté ya bindeada funciona igual. La lista completa de tokens válidos:

- `keyboard.f10`, `keyboard.f11`, `keyboard.f12` (function keys)
- `keyboard.num5`, `keyboard.numpad5` (numpad)
- `keyboard.num0`..`keyboard.num9` (numpad)
- `keyboard.lshift`, `keyboard.lctrl`, `keyboard.lalt` (modifiers)
- `keyboard.space`, `keyboard.enter`, `keyboard.escape`, `keyboard.tab`, `keyboard.backquote`
- `keyboard.up`, `keyboard.down`, `keyboard.left`, `keyboard.right`
- `keyboard.a`..`keyboard.z` (single letters)

`?0` al final es un eje SCS — `0` significa "no eixo" (presión directa sin escala). Déjalo como está.

**Reinicia el juego.** WalkerCore auto-detecta el binding en el próximo boot. Verás en `game.log.txt`:

```
00:00:17.507 : [WalkerCore] GameFreeCameraShim: auto-detected dev_camera_toggle=keyboard.f11 (VK 0x7A) in
                C:\Users\migue\...\controls.sii — adopted as the toggle VK (was 0x60 default Numpad-0).
00:00:17.507 : <WARNING> GameFreeCameraShim: shim is currently DISABLED. The auto-detected binding
                (keyboard.f11, VK 0x7A) will NOT dispatch until you enable the shim. Set [m2]
                game_free_camera_shim = true in WalkerCore.ini (or open the settings menu with
                Ctrl+F10 and flip the M2 toggle). The dev_cvar_patcher already flipped
                g_developer to 1 in memory, so once the shim is enabled, the next F10 press will
                activate the dev free camera via the bound key.
```

Si el shim está desactivado, el log te avisa. Actívalo (paso 3) y reinicia otra vez.

Si en cambio no aparece ningún binding, el shim emite la guía:

```
00:00:17.507 : <WARNING> GameFreeCameraShim: controls.sii at <path> has NO binding
                for dev_camera_toggle / walk_mode / free_camera. Default
                toggle VK=0x60 (Numpad-0) in effect — your profile binds
                Numpad-0 to dbglockpos so the synthesised key will be
                ignored. Add this line to <path> and reload the save /
                restart the game:
    mix dev_camera_toggle `keyboard.f11?0 | semantical.dev_camera_toggle?0`
    (alternative keys: keyboard.f10, keyboard.num5, keyboard.numpad5
    — pick one that does not collide with an existing binding.
    WalkerCore will auto-detect the new binding on next launch via
    [m2] dev_camera_toggle_vk auto-probe.)
```

---

## 5. Verificar end-to-end

Con el shim habilitado y el binding detectado:

1. Pulsa **F12** en juego para abrir el dump de diagnóstico.
2. Busca la sección `--- WalkerCore game-free-camera shim ---`. Deberías ver:
   ```
   enabled             : true
   supported           : true
   walks_entered       : 0
   toggles_dispatched  : 0
   toggle_vk (hex)     : 0x7A      ← VK_F11 (tu binding)
   ini_overrides_vk    : false
   auto_detected_key   : keyboard.f11
   controls_sii_path   : C:\Users\migue\...\controls.sii
   ```
3. **Sal del camión** (pulsa F10). Debería aparecer en game.log:
   ```
   00:01:23.456 : [WalkerCore] WalkerFsm: Driving → TransitionOut (tween 0.60s, source=F10)
   00:01:24.056 : [WalkerCore] WalkerFsm: TransitionOut complete → Walking
   00:01:24.056 : [WalkerCore] CameraState on Walking entry: eye=(0.00, 0.00, 0.00) yaw=0.000 pitch=0.000 h_fov=1.400 aspect=1.778 valid=0
   00:01:24.057 : [WalkerCore] GameFreeCameraShim: Walking entry #1 -> dispatched 2 key event(s)
                   (gate=on, config-cfg[probe]='1', dev_cvar_patcher=patched 0->1 cvar_va=0x7FF…
                   toggle VK=0x7A [auto-detected], auto-detected-key=keyboard.f11, …
                   path=SendInput hwnd=0x…)
   ```
4. La SCS dev free camera debería activarse. WASD mueve la cámara, mouse rota, mouse wheel cambia velocidad, Shift corre.
5. **Vuelve al camión** (pulsa F). Debería aparecer:
   ```
   00:01:30.456 : [WalkerCore] WalkerFsm: Walking → TransitionIn (tween 0.60s, source=F)
   00:01:31.072 : [WalkerCore] GameFreeCameraShim: Walking exit -> dispatched 2 key event(s)
                   (walks_entered=1, toggles_dispatched=2, toggle VK=0x7A, …) — toggle
                   dispatched to deactivate the dev free camera so the cabin view
                   regains control when the user re-enters the truck.
   ```
   El toggle-dispatched=2 indica que un toggle ON (entrada) + un toggle OFF (salida) se enviaron. Net: cámara de cabina otra vez en control.

Si el paso 4 no funciona (la cámara no se mueve), comprueba:

| Síntoma | Causa probable | Fix |
|---------|---------------|-----|
| game.log dice `signature mismatch at VA ... got 'efresh\0'` | RVA del `g_developer` parcheada con offset de archivo en vez de RVA | Actualiza a HEAD ≥ 46e9fef (fix ya pusheado). |
| game.log dice `shim is currently DISABLED` | `[m2] game_free_camera_shim = false` | Ponlo a `true`. |
| game.log dice `controls.sii at <path> has NO binding for dev_camera_toggle` | No pegaste la línea de `controls.sii` | Pégala y reinicia. |
| El SendInput se logea pero la cámara no se mueve | El binding del SCS action puede estar a una acción distinta | Confirma que la acción en controls.sii es exactamente `dev_camera_toggle` (sin typo). |
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
| `~` (consola del juego, con `g_developer=1`) | `free_camera_u` toggles dev camera manualmente |

---

## 8. Referencias internas

- `docs/observations/tmrealwalk-findings.md` — RE del mod de referencia.
- `docs/observations/tmrealwalk-cam-manager-re.md` — Por qué RE-blocked.
- `docs/integration.md` — Contratos entre módulos + M-landers.
- `docs/roadmap.md` — Roadmap de ingeniería.

---

## 9. Si quieres desactivar WalkerCore temporalmente

Renombra `libWalkerCore.dll` en `bin\win_x64\plugins\` (o muévelo a otro directorio). El juego arranca sin él y todo el comportamiento vuelve al stock de SCS.

Para desactivarlo limpio, pon `[m2] game_free_camera_shim = false` en el INI — WalkerCore sigue cargando, pero el shim no dispatchea y la cabina del juego se queda en control total.