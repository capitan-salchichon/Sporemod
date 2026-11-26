# TribalChiefObserver — Prototipo 0.1

Mod de Spore (Spore ModAPI) que **solo observa**: engancha `ForwardWalkTree`, toma su cuarto argumento
(la criatura que ejecuta el árbol de comportamiento), obtiene el cacique de la tribu del jugador y escribe en un log
si ambos son el mismo objeto. **No** omite `WalkTree`, no toca árboles, órdenes, locomoción, animaciones ni partidas,
y no usa `SetAvatar`.

## Estado honesto de esta entrega

| Elemento | Estado |
|---|---|
| Lógica pura (`ObserverCore.h`): parseo, verificación, identificación, límites de log | **Probada**: 51 comprobaciones con `g++` + ASan/UBSan (`tests/run_tests.sh`) |
| Pegamento (`ChiefObserver.cpp`) | **Simulado en Linux** con sustitutos mínimos de Windows/ModAPI (`tests/run_mock_tests.sh`, 9 escenarios) |
| Compilación a objeto con clang (x86, Windows) contra los headers **reales** de ModAPI | **Realizada**: los 3 `.cpp` generan `.o` sin errores ni avisos propios (`tests/clang_compile_check.sh`; ajustes solo en copias temporales, listados en el script) |
| Compilación con MSVC + ModAPI SDK y enlazado (`SporeModAPI.lib`, `detours.lib`) | **NO realizada**: el entorno no tiene Windows ni MSVC, y esas librerías no se pueden construir aquí |
| Dirección real de `ForwardWalkTree` en tu ejecutable | **NO verificada** (ver más abajo) |
| Ejecución dentro de Spore | **NO realizada** |

Lo que la simulación NO demuestra: que las firmas de ModAPI compilen tal cual en tu SDK, que el hook funcione en el
ejecutable real o que el puntero del cacique coincida en el juego real. Eso requiere compilar y ejecutar.

## Archivos

| Archivo | Contenido |
|---|---|
| `dllmain.cpp`, `stdafx.*`, `targetver.h` | Punto de entrada copiado de la plantilla `TemplatesExample` de ModAPI (`Initialize`, `Dispose`, `AttachDetours`) |
| `ChiefObserver.cpp/.h` | Detour, identificación del cacique, log, listener de modos de juego |
| `ObserverCore.h` | Lógica sin dependencias (probada) |
| `TribalChiefObserver.vcxproj` | Proyecto derivado de la plantilla (`v143`, `detours.lib`, `SporeModAPIBase.lib`, `SporeModAPI.lib`) |
| `TribalChiefObserver.ini.example` | Configuración |
| `tests/` | Pruebas unitarias y simulación en Linux |

No existía ningún proyecto de mod previo (no se subió ninguno y no hay ninguno en el entorno), así que el proyecto se creó
desde la estructura de la plantilla oficial. Si ya tienes uno, hay que integrar estos archivos en él en vez de usar este `.vcxproj`.

## Cómo funciona

1. `AttachDetours()` (dentro de `DllMain`, como exige la documentación de ModAPI) llama a `ChiefObserver::Attach`.
2. `Attach` lee `TribalChiefObserver.ini`. **Si no hay `thunk_address` o los bytes no coinciden, no instala nada** ("modo sonda").
3. Si la verificación pasa, instala `static_detour` sobre `ForwardWalkTree`
   (`void __cdecl (float, double, void*, void*, void*, void*, void*, void*)`; firma tomada del símbolo
   `?ForwardWalkTree@@YAXMNPAX00000@Z`, entrada byte-exacta del decomp). Un `static_assert` falla la compilación si
   el proyecto no usa `__cdecl` por defecto.
4. En cada llamada: observa y **siempre** ejecuta la función original con los argumentos originales.
   - `Simulator::cGameNounManager::Get()->GetPlayerTribe()` (en ModAPI es una lectura del campo `mpPlayerTribe`).
   - `tribe->mpLeader.get()` y `tribe->GetLeaderCitizen()`.
   - Compara el 4.º argumento con **ambos** punteros (para detectar si `cCreatureBase*` y `cCreatureCitizen*` difieren).
   - Ningún puntero se desreferencia ni se conserva como referencia entre llamadas: solo se comparan y se registran como números.
5. Seguridad: la parte de observación va protegida con `__try/__except` y `try/catch`; ante cualquier fallo se **desactiva
   sola** y el juego sigue. Si dos llamadas coinciden, la segunda no registra (no bloquea).

### Direcciones: convención de ModAPI
`Address(x)` de ModAPI es `baseAddress + x − 0x400000`. `thunk_address` se interpreta igual: es la dirección virtual de la
imagen enlazada en 0x400000 (la que muestran Ghidra/IDA). El mod comprueba además que `baseAddress` de ModAPI coincide con
la base real del módulo, que la dirección cae dentro de la imagen y que es memoria ejecutable legible.

## Cómo verificar la dirección del thunk (obligatorio antes de instalar el hook)

La dirección del decomp (0x00D995F0) pertenece a otra versión de `SporeApp.exe`. El SDK de ModAPI que revisé solo trae
direcciones para el ejecutable de disco y el de Steam de marzo de 2017, y ModAPI no declara `ForwardWalkTree`. El
desplazamiento −0x160 solo estaba confirmado en la región 0x00C0xxxx–0x00C2xxxx, así que **no se debe extrapolar**.

Procedimiento sugerido (requiere abrir tu ejecutable en Ghidra, x32dbg o similar):

1. Ir a `cCreatureCitizen::Update` en tu ejecutable (ModAPI declara su dirección: `0xC24210` en disco y `0xC24A30` en
   Steam de 2017).
2. En su bloque de IA, buscar la llamada con 8 argumentos en pila (un `float`, un `double` y seis punteros = 36 bytes)
   cuyo 4.º argumento es `this`. Esa función debe limitarse a reenviar los 8 argumentos a otra función (el intérprete `WalkTree`).
3. Anotar su dirección y los primeros 16 bytes o más como `thunk_address` y `expected_bytes`.
4. Para volcar los bytes con el propio mod: configurar solo `thunk_address`; el log escribirá
   `bytes at the candidate address (...)` y `NO_EXPECTED_BYTES`, sin instalar el hook. Compara ese volcado con tu
   desensamblador antes de copiarlo a `expected_bytes`.

`expected_bytes` protege contra una dirección equivocada o un ejecutable distinto. No sustituye el paso 2.

## Instalación y prueba

1. Compilar `TribalChiefObserver.vcxproj` (Visual Studio 2022, `v143`, Win32) con el ModAPI SDK instalado. Hace falta el
   archivo `SdkPathConfig.props` en la carpeta del proyecto (lo genera el instalador del SDK; cópialo de otro proyecto ModAPI).
2. Copiar `TribalChiefObserver.dll` a `mLibs` del ModAPI Launcher y `TribalChiefObserver.ini.example` como `TribalChiefObserver.ini`.
3. Primera ejecución sin dirección: debe aparecer `PROBE MODE` en `TribalChiefObserver.log` (junto a la DLL).
4. Completar `thunk_address` y `expected_bytes` y volver a ejecutar.

## Qué aparece en el log

| Línea | Significado |
|---|---|
| `main module: base=… ModAPI baseAddress=…` | base real y base de ModAPI |
| `bytes at the candidate address (n): …` | volcado para verificación manual |
| `verification of expected_bytes (n configured): OK \| MISMATCH \| NO_EXPECTED_BYTES …` | resultado de la verificación |
| `DetourAttach OK` / `PROBE MODE` / `ABORT: …` | hook instalado o motivo por el que no |
| `TRIBE appeared/changed/disappeared` | disponibilidad de la tribu del jugador (entrada/salida de Tribal, carga) |
| `LEADER appeared/changed/disappeared` | cambio de cacique |
| `MISMATCH rawLeader=… citizenLeader=…` | `mpLeader.get()` ≠ `GetLeaderCitizen()` (ajuste de puntero) |
| `CHIEF_DETECTED creature=… rawLeader=… citizenLeader=… matchRaw= matchCitizen= tribe= tribeAvailable= detections=` | el árbol lo ejecuta el cacique (una vez por cacique distinto) |
| `STATS calls= chiefCalls= otherCreatures= tribeUnavailable= leaderUnavailable= rawVsCitizenMismatch= …` | cada `stats_interval_ms` |
| `MODE_ENTER/MODE_EXIT id=0x… previous=0x…` | cambios de modo de juego (0x01654C02 = Tribal) |
| `FINAL …`, `FAULT …` | cierre / observador desactivado por un fallo |

El log se limita con `max_event_lines`; las líneas suprimidas se cuentan en `suppressedEventLines`.

## Escenarios de prueba dentro de Spore

| Escenario | Qué buscar |
|---|---|
| A. Partida normal en Tribal | `TRIBE appeared`, `LEADER appeared`, `CHIEF_DETECTED` |
| B. Cacique con IA | `chiefCalls` crece en `STATS` |
| C. Otros ciudadanos | `otherCreatures` crece mucho más que `chiefCalls`; el juego se comporta igual |
| D. Cambio de cacique (si se reproduce) | `LEADER changed` y un nuevo `CHIEF_DETECTED` |
| E. Salir de Tribal | `MODE_EXIT/MODE_ENTER`, `TRIBE disappeared` |
| F. Cargar partida | `TRIBE`/`LEADER changed` y un nuevo `CHIEF_DETECTED` |

## Pruebas locales

```sh
sh tests/run_tests.sh        # lógica pura
sh tests/run_mock_tests.sh   # simulación del pegamento (requiere g++)
sh tests/clang_compile_check.sh /ruta/a/Spore-ModAPI   # compilación a objeto contra los headers reales (requiere python3/pip)
```
