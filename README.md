# TribalChiefObserver — Prototipo 0.1

Mod de Spore (Spore ModAPI) que **solo observa**: engancha la actualización de cada ciudadano (`cCreatureCitizen::Update`,
con la dirección que ModAPI ya trae; opcionalmente `ForwardWalkTree`), obtiene el cacique de la tribu del jugador y
escribe en un log si el ciudadano que se actualiza es el cacique. **No** omite `WalkTree`, no toca árboles, órdenes, locomoción, animaciones ni partidas,
y no usa `SetAvatar`.

## Estado honesto de esta entrega

| Elemento | Estado |
|---|---|
| Lógica pura (`ObserverCore.h`): parseo, verificación, identificación, límites de log | **Probada**: 51 comprobaciones con `g++` + ASan/UBSan (`tests/run_tests.sh`) |
| Pegamento (`ChiefObserver.cpp`) | **Simulado en Linux** con sustitutos mínimos de Windows/ModAPI (`tests/run_mock_tests.sh`, 9 escenarios) |
| Compilación a objeto con clang (x86, Windows) contra los headers **reales** de ModAPI | **Realizada**: los 3 `.cpp` generan `.o` sin errores ni avisos propios (`tests/clang_compile_check.sh`; ajustes solo en copias temporales, listados en el script) |
| Compilación y enlazado con **MSVC** (GitHub Actions, `windows-2022`, VS 2022, MSVC 14.44.35207) | **Realizada**: SDK de ModAPI construido, mod compilado y enlazado con 0 errores y sin avisos del compilador en mis archivos; `TribalChiefObserver.dll` x86 de 56 832 bytes (`build/` y release `ci-4` del repositorio) |
| Pruebas unitarias de `ObserverCore.h` compiladas con MSVC | **51 comprobaciones, 0 fallos** |
| Dirección de `cCreatureCitizen::Update` (modo predeterminado) | La da la tabla de ModAPI; **no verificada por mí** en ejecución |
| Dirección real de `ForwardWalkTree` (solo modo `thunk`) | **NO verificada** (ver más abajo) |
| Ejecución dentro de Spore | **NO realizada** |

Lo que la simulación y la compilación NO demuestran: que el hook funcione en el ejecutable real o que el puntero del cacique
coincida en el juego real. Eso solo se comprueba ejecutando Spore.

## Archivos

| Archivo | Contenido |
|---|---|
| `dllmain.cpp`, `stdafx.*`, `targetver.h` | Punto de entrada copiado de la plantilla `TemplatesExample` de ModAPI (`Initialize`, `Dispose`, `AttachDetours`) |
| `ChiefObserver.cpp/.h` | Detour, identificación del cacique, log, listener de modos de juego |
| `ObserverCore.h` | Lógica sin dependencias (probada) |
| `TribalChiefObserver.vcxproj` | Proyecto derivado de la plantilla (`v143`, `detours.lib`, `SporeModAPIBase.lib`, `SporeModAPI.lib`) |
| `TribalChiefObserver.ini.example` | Configuración |
| `tests/` | Pruebas unitarias, simulación en Linux y comprobación con clang |
| `.github/workflows/build.yml` | CI: construye el SDK de ModAPI, compila y enlaza el mod, ejecuta las pruebas y publica la DLL como pre-release |
| `build/TribalChiefObserver.dll` | DLL compilada por la CI (Release, Win32) |

No existía ningún proyecto de mod previo (no se subió ninguno y no hay ninguno en el entorno), así que el proyecto se creó
desde la estructura de la plantilla oficial. Si ya tienes uno, hay que integrar estos archivos en él en vez de usar este `.vcxproj`.

## Cómo funciona

1. `AttachDetours()` (dentro de `DllMain`, como exige la documentación de ModAPI) llama a `ChiefObserver::Attach`.
2. `Attach` lee `TribalChiefObserver.ini` (opcional). `hook_target` elige qué se engancha:
   - **`update` (predeterminado):** `cCreatureCitizen::Update`, con la dirección de la tabla de ModAPI para los ejecutables
     que soporta (disco y Steam de marzo de 2017). **No hay que configurar ninguna dirección.** `this` es el ciudadano.
   - `thunk`: `ForwardWalkTree`, solo si `thunk_address` y `expected_bytes` verifican (ver más abajo).
   - `none`: no instala nada (modo sonda).
3. Con `thunk`, si la verificación pasa, instala `static_detour` sobre `ForwardWalkTree`
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

## Cómo verificar la dirección del thunk (solo para `hook_target=thunk`)

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
3. Ejecutar Spore: no hace falta configurar nada. En `TribalChiefObserver.log` (junto a la DLL) debe aparecer
   `DetourAttach OK on cCreatureCitizen::Update`. Si el log dice `ABORT`, copia ese mensaje.
4. (Opcional) `hook_target=thunk` para enganchar `ForwardWalkTree`: completar `thunk_address` y `expected_bytes`.

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

## Compilación automática (GitHub Actions)

`.github/workflows/build.yml` (runner `windows-2022`) clona `Spore-Community/Spore-ModAPI`, construye `SporeModAPIBase.lib` y
`SporeModAPI.dll`/`.lib` (con `EXECUTABLE_TYPE=10`, igual que la CI del propio SDK), genera `SdkPathConfig.props`,
compila el mod, ejecuta las pruebas unitarias con MSVC y publica la DLL como pre-release.
La DLL enlaza estáticamente `SporeModAPIBase.lib` y `detours.lib`, e importa de `SporeModAPI.dll` solo:
`cGameNounManager_addresses::Get`, `IMessageManager_addresses::Get`, `baseAddress`, `AddPostInitFunction` y `AddDisposeFunction`.
En el juego se usa el `SporeModAPI.dll` de tu launcher, no el construido por la CI.
