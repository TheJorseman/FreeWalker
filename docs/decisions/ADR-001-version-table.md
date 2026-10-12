# ADR-001 — Per-build version table

- **Date:** 2024-10-10
- **Status:** Aceptada (sesión de planeación)

## Contexto

TMRealWalk's strings reveal that el plugin mantiene una lista cerrada de builds del juego que ha validado, y degrada graciosamente para builds desconocidas:

```
ETS2 1.61.1.0
ETS2 1.61.1.1
ATS 1.61
ATS 1.61.2
ATS 1.61.3
ATS 1.61.3.1

[resolve]
dxgi_present not found by signature; the known address of this ATS build is used
```

Cuando una build sale del horno (o cuando un parche cambia offsets internos), los subsistemas del DLL se desactivan uno a uno. Sin esta tabla, los hooks de cámara y la lectura de PhysX fallan de forma intermitente.

## Decisión

WalkerCore lleva una **tabla de offsets por build** desde M0.5, con estructura explícita (`VersionEntry` con `SubsystemEntry[4]` y bitmask de `Feature`). Cada subsistema declara qué offsets necesita (`CameraPresentHook`, `CameraManager`, `PhysXScene`, `LightSourceClass`). Cuando un subsistema se implementa (M2+), abre `VersionEntry` y rellena el `rva` + `verified=true` para los builds que probamos.

Para builds desconocido: emitimos un warning, devolvemos `nullptr` desde `VersionTable::Lookup`, y los subsistemas verifican `entry != nullptr` antes de activarse. Sin fallback silencioso a offsets hardcoded de una versión distinta.

## Consecuencias

**Positivas:**
- Cuando salga un nuevo parche del juego, WalkerCore degrada de forma controlada (log claro, subsistemas desactivados) en lugar de crashear.
- Es fácil añadir una nueva build a la tabla sin recompilar el resto.
- La tabla es inspeccionable (F12 dump = `VersionTable::DumpToLog()`), útil para reportar bugs.

**Negativas:**
- Mantenimiento constante: cada parche del juego obliga a verificar y posiblemente añadir fila.
- Tests de regresión por versión son caros sin CI de juegos — se hacen a mano por ahora.

**Riesgos aceptados:**
- Las primeras dos semanas tras un parche grande del juego, las colisiones y la cámara pueden estar desactivadas. Es esperado y consistente con el comportamiento de TMRealWalk.