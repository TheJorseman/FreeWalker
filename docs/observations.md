# Observations — arquitectura de TMRealWalk.dll v1.0.0-beta4.5

> **Propósito.** Este documento es metadata sobre la arquitectura del DLL — qué subsistemas tiene, qué APIs consume, qué nombres de settings/claves existen — para informar el diseño de WalkerCore. **No reproduce código fuente**, ni algoritmos, ni lógica de licenciamiento. Cada observación va acompañada de su fuente (`survey.json`, `strings_categorized.txt`).
>
> **Lo que NO está aquí** (y debe seguir así):
> - Pseudocódigo de funciones.
> - Constantes/offsets/magic numbers propios del binomero.
> - Implementación del chequeo de licencia online.
> - Algoritmos específicos de collision/animación.

## 0. Reproducibilidad de las observaciones**
Cada afirmación de este documento se puede verificar corriendo:

```powershell
cd C:\Users\migue\OneDrive\Documentos\GitHub\WalkerCore\docs\observations
python survey_dll.py "C:\Users\migue\OneDrive\Documentos\GitHub\rea\TM_Real_Walk_v1.0.0-beta4.5\TMRealWalk.dll" > survey.json
python strings_survey.py "C:\Users\migue\OneDrive\Documentos\GitHub\rea\TM_Real_Walk_v1.0.0-beta4.5\TMRealWalk.dll" > strings_categorized.txt
```

Dependencias: `pefile` (Python).

---

## 1. Metadatos del binario

| Campo | Valor | Fuente |
|-------|-------|--------|
| Tamaño | 14 081 648 bytes | `survey.json → file_info.size_bytes` |
| Formato | PE DLL, x86_64 | `survey.json → file_info` |
| Linker | MSVC 14.29 (Visual Studio 2019/2022) | `survey.json → linker_version` |
| OS version target | 6.0 (Windows Vista+) | `survey.json → os_version` |
| Image base | `0x180000000` | `survey.json → image_base` |
| Exports | **2**: `scs_telemetry_init`, `scs_telemetry_shutdown` | `survey.json → exports` |

**Implicación para WalkerCore.** El DLL expone exactamente la ABI mínima de SCS. WalkerCore necesita el mismo patrón.

---

## 2. Secciones

| Sección | Virtual size | Raw size | Entropía | Observación |
|---------|--------------|----------|----------|-------------|
| `.text` | 790 400 | 790 528 | 6.54 | Código (alta entropía, sin comprimir) |
| `.rdata` | 544 630 | 544 768 | 5.03 | Strings, constantes, vtables |
| `.data` | 104 208 | 14 336 | 2.87 | Variables globales inicializadas (mayormente BSS) |
| `.pdata` | 27 936 | 28 160 | 5.92 | Tabla de excepciones |
| `_RDATA` | 244 | 512 | 2.48 | Restos del linker |
| `.rsrc` | **12 680 408** | 12 680 704 | **8.00** | **Recursos embebidos — el 90 % del binario** |
| `.reloc` | 11 104 | 11 264 | 5.44 | Relocations |

**Observación clave.** El 90 % del DLL son recursos embebidos (probablemente WAVs, fuentes, texturas). El código real son ~800 KB. WalkerCore puede tener un tamaño muy inferior sin las texturas y sonidos embebidos.

**Recursos:**

| Tipo | Bytes |
|------|-------|
| `RT_RCDATA` | 12 679 346 |
| `RT_VERSION` | 896 |

Un solo bloque `RT_RCDATA` enorme. WalkerCore puede repartir sus recursos en varios archivos en el `plugins/` si quiere ser más modular.

---

## 3. Imports — qué APIs consume

Total: **316 imports** en 14 DLLs de Windows.

| DLL | # | Categorías principales |
|-----|---|------------------------|
| kernel32.dll | 135 | Memoria, threads, sync, file I/O, registry, hooks de bajo nivel |
| user32.dll | 66 | Window management, mensajes, hooks de teclado, raw input |
| gdiplus.dll | 52 | Render 2D: brushes, fuentes, imágenes, gradientes |
| gdi32.dll | 19 | Render 2D clásico |
| winhttp.dll | 10 | HTTP (chequeo online de licencia) |
| winmm.dll | 9 | Multimedia (timer, audio legacy) |
| advapi32.dll | 8 | Registry, services |
| bcrypt.dll | 6 | Crypto moderno (verificación de firma) |
| d3d11.dll | 2 | **Hook del render del juego** |
| d3dcompiler_47.dll | 1 | Compile shaders en runtime |
| shell32.dll | 2 | Sistema |
| crypt32.dll | 2 | Certificados |
| ole32.dll | 3 | COM |
| shlwapi.dll | 1 | Path helpers |

**Hallazgo clave:** `d3d11.dll` solo tiene 2 imports — confirma que el DLL **no usa D3D11 directamente para render**, sino que lo engancha (`IDXGISwapChain::Present`). Esto coincide con el patrón de "el plugin no toca la geometría del juego, solo reescribe los uniforms de la cámara justo antes del present".

**Hallazgo clave 2:** `gdiplus.dll` con 52 funciones y `gdi32.dll` con 19 → **el menú del plugin es una ventana 2D independiente, no parte del HUD del juego**. Esta es una decisión arquitectural importante que WalkerCore puede copiar o reemplazar (más limpio: renderizar dentro del HUD del juego con IMGUI/Direct2D, pero más invasivo).

**Hallazgo clave 3:** `d3dcompiler_47.dll` presente → hay al menos un shader que se compila en runtime. Probablemente el overlay/flashlight o el shadow.

---

## 4. Subsistemas identificados por strings

Análisis: 28 642 strings, 972 clasificadas como "interesantes" (16 categorías).

| Subsistema | # strings | Observaciones |
|------------|-----------|---------------|
| **Truck/trailer** | 257 | El subsistema más grande. Maneja kingpin, fifth wheel, attach key, air hoses, landing gear, tug test. |
| **Fuel** | 103 | Diesel + AdBlue/DEF, receipt, nozzle state, pump probe. |
| **Game lifecycle** | 81 | Enter/exit walk mode, attach/detach, pump. |
| **Player body** | 67 | Head bob, walking, running, crouching, knocked-out. |
| **Camera/render** | 66 | Free camera, hook de Present, manejo de seat camera. |
| **UI/tab/menu** | 59 | Configuración por tabs, prompts [F], fade overlay. |
| **Mouse/look/input** | 50 | Raw input, smooth look, controller via DirectInput. |
| **PhysX** | 40 | **Lectura de la escena PhysX del juego para collision** — el subsistema más complejo. |
| **Flashlight** | 38 | Light arrays del juego, beam size, daytime. |
| **File extensions** | 37 | Rutas a `game.log.txt`, `TMRealWalk.ini`, INIs de GSettings. |
| **Settings/config** | 32 | INI parser, reload al guardar, validación. |
| **Sound** | 32 | 8+ WAVs embebidos (puerta, pasos, linterna, etc.), audio retry. |
| **Localization** | 29 | 16+ idiomas (EN, Bahasa Melayu/Indonesia, DE, FR, ES, ...). |
| **Shadow** | 27 | Strength/softness/size/length/azimuth/contact/truck_fade. |
| **Head tracker** | 24 | TrackIR/opentrack. |
| **Logging** | 15 | Prefijos `[license]`, `[trailer]`, `[fuel]`, `[physx]`, etc. |
| **SCS/game** | 9 | Detección de versión: "ETS2 1.61.1.0", "ATS 1.61.3.1" (solo). |
| **Door/animation** | 6 | Door animation con body motion. |

**Comparación con el roadmap de WalkerCore.** Nuestro diseño inicial preveía módulos similares:

| Módulo WalkerCore (roadmap) | Subsistema TMRealWalk equivalente |
|----------------------------|-----------------------------------|
| `domain::PlayerState` | Player body + Camera/render + Mouse/input |
| `domain::TruckState` | Truck/trailer (la mitad superior) |
| `domain::WorldState` + `Collision` | **PhysX** (lectura de scene, ground-snap, fault handling) |
| `domain::Animation` | Player body (bob) + Door/animation |
| `domain::Config` | Settings/config |
| `domain::Localization` | Localization |
| `provider::ScsTelemetryProvider` | SCS/game (logging, channels, events) |
| `provider::D3D11Hook` | Camera/render (Present hook) |
| `provider::InputHook` | Mouse/look/input |
| `workflow::StateMachine` | Game lifecycle |

El roadmap cubre **todos** los subsistemas identificados. No hay sorpresas.

---

## 5. Hallazgos concretos que cambian la prioridad del roadmap

### 5.1 El chequeo online de licencia es complejo, no trivial

Los strings del bloque "Logging" incluyen manejo explícito de: **servidor de licencias, conteo de computadoras, computer marker (HWS), PC reset request**. TMRealWalk pasa tiempo significativo en este subsistema (muchos strings de error, flujo de reintentos).

**Implicación para WalkerCore:** el subsistema de licencia no entra en M0–M7. Sin licencia ni DRM es un objetivo explícito del proyecto (decisión D3, ya aceptada).

### 5.2 PhysX ground/collision es el subsistema más arriesgado

Los strings `physx` son reveladores del esfuerzo invertido:

```
[physx] scene search read %llu MB of %llu MB committed (resident pages only) in %llu ms
[physx] scene strings not found - ground/collision unavailable
[physx] scene functions not found - ground/collision unavailable
[physx] scene vtable not found - ground/collision unavailable
[physx] too many faults - ground/collision disabled for this session
[physx]       box half=(%.2f,%.2f%.%.2f) local=(%.2f,%.2f%.%.2f) localRot=(%.3f,%.3f%.%.3f,%.3f)
```

**Lo que hacen.** El plugin **escanea la memoria del juego buscando la vtable de `PxScene` de PhysX**, intenta identificarlo por strings o vtables conocidas, lee la geometría de colisión y la copia como una malla local. Esto es **frágil y rompe con cada versión del juego** (de ahí el changelog reciente: "much less memory (RAM). When a world loaded, the plugin's search for the game's collision scene read all of the game's reserved memory").

**Implicación para WalkerCore.** M3 (colisión) es el punto de mayor riesgo. Hay tres caminos posibles:

1. **Misma estrategia:** escanear memoria para encontrar `PxScene` (como TMRealWalk). Riesgo: misma fragilidad, hay que mantener offsets por versión.
2. **Usar el SDK público de PhysX 3.4** (`PxPhysics` exportado por el juego) para engancharse de forma estable. Menos invasivo, pero requiere entender cómo el juego inicializa PhysX.
3. **Pedirle al juego la malla vía API pública** (si existe). Más limpio si está disponible; dudoso que el SDK público lo exponga.

**Recomendación para el roadmap.** En M3, **probar primero la opción 2** (SDK público). Si no es viable, **caer a la opción 1 con offsets por build**, igual que TMRealWalk. Marcar M3 como fase de **riesgo alto** en el documento.

### 5.3 El DLL detecta versiones específicas del juego

```
ETS2 1.61.1.0
ETS2 1.61.1.1
ATS 1.61
ATS 1.61.2
ATS 1.61.3
ATS 1.61.3.1
```

Y el log:

```
[resolve] dxgi_present not found by signature; the known address of this ATS build is used
```

**Lo que hace.** Mantiene una tabla de versiones conocidas con offsets específicos. Si la versión no está en la tabla, **algunas features se desactivan automáticamente** (lo dijo también el changelog: "After a big game update some features may switch off by themselves").

**Implicación para WalkerCore.** M2 (camera) y M3 (collision) deben diseñarse con **tabla de offsets por juego/versión**, empezando vacía y poblándose por release. **Aceptar que las primeras semanas tras un update del juego la collision va a estar rota**.

### 5.4 La linterna usa las luces dinámicas del juego, no las propias

Los strings `[lights]` muestran que el plugin:

```
[lights] the light_source class was not found; daytime flashlight unavailable
[lights] own light %d: the game's destructor faulted; daytime flashlight turned off
[lights] flashlight uses %d light(s) of its own (%s)
```

El plugin **reusa el sistema de luces dinámicas del juego** en lugar de inyectar luz propia. Esto evita problemas de render order pero limita el control (de ahí el comentario en el changelog sobre la linterna diurna "switching off" cuando cambia el build).

**Implicación para WalkerCore.** En M7 (linterna), decidir:
- Reusar `light_source` del juego (menos código, mismo riesgo de version-breaking).
- Inyectar luz propia vía D3D11 (más código, más robusto).

**Recomendación.** Reusar si funciona; documentar como deuda técnica.

### 5.5 DXGI wrapper detection

```
[d3d_hook] dxgi.dll is in the game folder but its module is not listed: treated as a wrapper
```

El plugin detecta si el juego tiene una `dxgi.dll` envuelta (común con overlays como MSI Afterburner, RTSS, etc.) y la integra.

**Implicación para WalkerCore.** Importante para M2. El hook debe funcionar con DXGI wrappeado o nativo. **No skipeable**.

### 5.6 El plugin se autogestiona el enfoque y el modo de entrada

Strings como:

```
[input] Windows' staging window had the focus: %s
quit] could not open the game log to watch for Quit (%lu): the hidden driver is put back when a menu has been open for ...
[hands] mouse cursor %s, game %s
```

El plugin tiene que **devolver el foco al juego** después de abrir su propio menú (Ctrl+F10). Maneja explícitamente el caso "Windows Game Bar grabó el foco" (esto es también un fix documentado en el changelog).

**Implicación para WalkerCore.** M0/M1 ya lo registran.

### 5.7 8 WAVs embebidos

Los nombres de archivos WAV que el plugin busca junto al DLL:

```
TMRealWalk_Door.WAV
TMRealWalk_Footstep1.WAV
TMRealWalk_Flashlight.WAV
tmreceiptprint.WAV
tmbeep.WAV
tmtakenozle.WAV
tmputnozle.WAV
tmopencaptank.WAV
```

**Observación.** El plugin **busca los WAVs en disco, no los embebe directamente**. El `.rsrc` enorme que vimos antes probablemente son WAVs embebidos como respaldo o como texturas (los WAVs pesan ~10 MB si son decentes). Esto explica el tamaño.

**Implicación para WalkerCore.** Los sonidos pueden entregarse:
- Como WAVs en la misma carpeta del DLL (como TMRealWalk).
- O como WAVs/OGGs dentro del propio DLL si queremos distribución de un solo archivo.

**Recomendación.** Empezar con WAVs al lado del DLL (más fácil de iterar, sin recompilar).

### 5.8 Localización: 16+ idiomas, cadenas largas con caracteres especiales

Strings malayos, alemanes, franceses, etc. UTF-8 multi-byte correcto. Muestra que el subsistema de localization de TMRealWalk maneja múltiples encodings (UTF-8, latin-1).

**Implicación para WalkerCore.** M8 (i18n) debe diseñarse desde el principio con **almacenamiento en UTF-8** y un sistema de catálogos (no archivos INI con claves, algo como `gettext` o `ICU`). No es opcional si queremos añadir idiomas tarde.

---

## 6. Lo que el DLL **NO** hace

Lo que no vi en el escaneo de strings:

- **Networking/WebSocket propio.** Solo WinHTTP (cliente HTTP). No abre puertos, no escucha conexiones.
- **SQLite/database propio.** Solo INI.
- **GPU compute (CUDA/OpenCL).** No hay imports asociados.
- **Multiplayer detection.** El README dice "single-player only" pero el SDK de SCS expone `game.multiplayer`; no se encontraron strings que lo lean. **Confirmar esto en runtime cuando llegue M0.**

---

## 7. Conclusiones para el roadmap de WalkerCore

1. **El roadmap actual está bien fundamentado.** Cubre todos los subsistemas que tiene TMRealWalk. No hace falta añadir módulos nuevos.

2. **Refinar M3 como fase de riesgo.** Marcar explícitamente como "estrategia de collision por decidir; empezar por SDK público de PhysX 3.4, caer a escaneo de memoria si no funciona".

3. **Añadir M0.5 — tabla de offsets por versión.** Pequeña utilidad que lee la versión del juego y selecciona la tabla adecuada. Fundamental desde el principio.

4. **DXGI wrapper compatibility** desde M2, no como add-on tardío.

5. **Sistema de localization de strings** desde M0 (catálogo de strings en UTF-8, no hardcoded).

6. **Sonidos como archivos al lado del DLL**, no embebidos en M0–M7. Empezar simple.

7. **No incluir sistema de licencia en M0–M7**. Decisión explícita: WalkerCore es MIT sin DRM. M8+ si el usuario quiere monetizar, lo diseña aparte.

---

## 8. Archivos generados por este análisis

| Archivo | Propósito |
|---------|-----------|
| `docs/observations/survey_dll.py` | Script de survey PE (imports/exports/sections/resources/version) |
| `docs/observations/summarize.py` | Pretty-print de survey.json |
| `docs/observations/strings_survey.py` | Catálogo de strings con filtros sensibles |
| `docs/observations/survey.json` | Output completo del survey (35 KB) |
| `docs/observations/strings_categorized.txt` | Strings agrupadas por subsistema (12 KB) |

Los scripts quedan en el repo para que sean ejecutables de nuevo cuando alguien quiera revalidar (p. ej. tras un cambio de versión del juego).