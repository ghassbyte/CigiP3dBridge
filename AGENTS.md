# AGENTS.md

Guidance for AI coding agents and human collaborators working in this
repo. If you''re an agent, read this before touching anything; if
you''re a human, this is the equivalent of an extended `CONTRIBUTING.md`.

## What this project is

`CigiP3dBridge` decodes CIGI v3.x traffic from a HOST image generator
and writes the resulting aircraft position/attitude/time-of-day into
the local Prepar3D instance via FSUIPC. It has two operating modes
(`--bin` replay, `--live` Npcap tap) and a one-shot reposition hook.

## Read these first

In this order:

1. `README.md` -- the user-facing overview, build instructions, CLI
   examples.
2. `.\src\CigiTypes.h` -- the CIGI wire-format structs and the
   `cigi::` namespace where everything lives. Once you understand
   what `EntityControl` / `CelestialSphereControl` /
   `ComponentControl` look like on the wire, the rest is obvious.
3. `.\src\CigiProcessor.cpp` -- the datagram walker. This is the
   single integration point between the transport layer (bin/live)
   and the FSUIPC-writing layer.
4. `.\src\CigiToFsUIPC.cpp` -- where the actual FSUIPC work happens.
   Read this if you''re debugging position / slew / cold-start
   behaviour.
5. `.\src\Conversion.h` -- the math. `headingToFs` in particular has
   the uint32-overflow fix; do not revert it.

## Hard rules (do not break)

These exist because breaking them silently produces wrong-looking
output, not a compile error. Take them seriously.

### 1. Every FSUIPC write takes the mutex

`FSUIPC_Write(...) + FSUIPC_Process()` is NOT thread-safe across
threads -- FSUIPC''s write buffer is process-wide. Every function in
`CigiToFsUIPC.cpp` that does an `FSUIPC_Write` (or `FSUIPC_Read`)
MUST hold `g_fsuipcMutex` for the **entire** sequence, not just
one call. The reason is in the file''s header comment.

Adding a new write site? Find the existing `std::lock_guard` blocks
in `applyEntityToFs` / `applyColdStartEntityToFs` /
`applyCloudPatternToFs` / `checkAndReassertSlew` /
`applyTimeToFs` / `slewHoldLoop` / `applyReposition` and copy the
pattern. Do not split a logical update across two separate lock
acquisitions.

### 2. Heading conversion is `DWORD` (unsigned)

`convertHeadingToFS` returns `DWORD`, not `int`. The FSUIPC heading
offset is documented as **unsigned** 32-bit; casting the
out-of-range double to `int` is undefined behaviour and on MSVC
collapses to `INT_MIN` (= 180 deg) for the entire 180..360 range,
producing a silent "stuck at 180" bug. See the function''s comment in
`Conversion.h` for the full write-up.

### 3. Pitch/bank sign flip

CIGI convention: `+pitch = nose up`, `+roll = right bank`.
FSUIPC convention: `-pitch = pitch up`, `-roll = bank right`.
Use `conv::pitchToFsUIPC` / `conv::bankToFsUIPC` -- they negate
internally. Do NOT call `conv::angleToFs` directly for these two
fields; you''ll get the sign wrong and the aircraft will bank the
opposite way.

### 4. Slew flow is "reposition first, then engage"

The cold-start path (and the one-shot reposition path that shares
it) writes LLAPBH + clears `0x0366` while slew is OFF, **then**
turns slew on at `0x05DC`. Do not reorder this -- FS needs its
normal reposition handling to reconcile "aircraft teleported 6000
nm, marked airborne" without slew suppressing the bits of the
flight model that would otherwise settle it cleanly.

### 5. Range-check before converting

`isValidEntityControl` (`CigiTypes.h`) rejects NaN, Infinity, and
out-of-range lat/lon/alt before anything touches FSUIPC. Corrupted
or malformed packets in a capture will absolutely happen; do not
rely on `fmod()` to rescue a NaN (it won''t -- `fmod(NaN, 360) ==
NaN`). The processor calls `isValidEntityControl` and skips the
packet on rejection; preserve that path.

## Project structure conventions

* Everything goes in `namespace cigi` (or a sub-namespace like
  `binplayer::` / `pcapcap::` / `conv::`).
* One `.cpp`/`.h` pair per subsystem. New subsystem = new pair;
  don''t grow an existing file past ~500 lines without splitting.
* Every header gets a top-of-file comment explaining what it does
  and why it exists. Future you (or a future agent) will thank you.
* Static asserts on every wire struct (`static_assert(sizeof(X) ==
  48, ...)`). If you add a field, the assert will catch it.
* No global state except inside `namespace cigi { namespace { } }`
  blocks in the relevant `.cpp`. Don''t add a `static` at file
  scope in a header.

## Build

* The project is **x64 only**.
* Platform Toolset **v140** (Visual Studio 2015). Don''t change this
  without confirming the prebuilt `FSUIPCuser64.lib` under
  `external\UIPC64_SDK_C_version2\` was built with the same
  toolset -- mixing toolset versions produces LNK1257 / LNK1047
  "object compiled with a different version of the compiler".
* Use `MultiThreaded` (`/MT`) for Release, `MultiThreadedDebug`
  (`/MTd`) for Debug. Mixing these (e.g. `/MTd` in Release config)
  causes LNK2001 for `_calloc_dbg` / `_free_dbg`.
* `WholeProgramOptimization` is `true` by default for Release; if
  you ever change it, ensure FSUIPC''s `.lib` was built with the
  same `/GL` setting or expect LTCG warnings.

### Smoke build

```bat
"%VS140COMNTOOLS%\..\..\VC\vcvarsall.bat" x64
msbuild CigiP3dBridge.sln /p:Configuration=Release /p:Platform=x64 /m
```

Should produce `x64\Release\CigiP3dBridge.exe` with no errors.
Debug builds with `/MTd` should also succeed.

## Common gotchas

### Winsock header order

`<winsock2.h>` MUST be included before `<windows.h>`. `<windows.h>`
otherwise pulls in the v1 `<winsock.h>` which redeclares every
socket struct as a different type. See `CigiToFsUIPC.h` and
`PcapCapture.cpp` for the canonical include order; copy the
pattern when adding a new `.cpp` that needs both.

### `FSUIPC_User64.h` does not include its own prerequisites

That header uses `DWORD`, `BOOL`, `BYTE` without including
`<windows.h>` itself. Include `<windows.h>` (or equivalent) BEFORE
including `FSUIPC_User64.h`, or you get a wall of `error C4430:
missing type specifier - int assumed`.

### `g_fsuipcMutex` is declared inside `CigiToFsUIPC.cpp`

It is intentionally NOT in the header, to make accidental misuse
harder. If you need to add a new write site, put it in
`CigiToFsUIPC.cpp` (where the mutex lives) rather than in a new
file.

### Cloud-pattern ICD may differ

The cloud-pattern component / instance IDs (`0x1001` / `0x9001`)
and the field meanings in `ComponentControl` are specific to the
HOST this project was built against. If you connect to a different
HOST, update the constants in `CigiTypes.h` and the parsing in
`CigiToFsUIPC.cpp::applyCloudPatternToFs`.

### Reposition during live mode is intentionally not supported

Live mode follows the wire; overriding it would mean ignoring real
CIGI traffic. If you really need it, plumb the controller into
`PcapCapture::livePacketHandler` -- the seam is the same
`consumeIfPending()` + `applyReposition()` flow the bin path uses.

## Adding new features

Before adding a new CIGI packet type:

1. Add its wire struct + `PACKET_ID_*` constant to `CigiTypes.h`
   with a `static_assert(sizeof(...) == N, ...)`.
2. Add the dispatch case to `processDatagram` in `CigiProcessor.cpp`.
3. Add a new `apply<X>ToFs(...)` function in `CigiToFsUIPC.{h,cpp}`
   following the same lock-and-write pattern as the existing ones.
4. Document the new offset / behaviour in `FsuipcOffsets.h` (with a
   source-of-truth reference, ideally a page in
   `FSUIPC for Programmers.docx`).

Before adding a new transport:

1. Add a new `.cpp`/`.h` pair under `src/` with its own namespace.
2. The transport''s job ends at: pull bytes out of wherever they
   come from, and call `cigi::processDatagram(buf, len, reposition)`.
3. Add the new mode to `main.cpp`''s `parseArgs` and `printUsage`.

## Things deliberately NOT here

* **GUI**. The brief said "later". `RepositionController` exposes
  `request(...)` which is what a future button handler will call.
* **Source-port-lock warning**. The reference bridge had it; this
  one doesn''t. Re-add if you ever see two streams silently merging
  on a debug HOST.
* **Local-stall watchdog**. Same -- the reference had a
  `STALL_THRESHOLD_MS` block marked `DEBUG -- DELETE LATER`. We
  deleted it.
