# CigiP3dBridge

> [!IMPORTANT]
> **This repository is the initial work and is no longer used.**
> It is kept as a reference only, and will not be maintained or developed
> further. The latest work lives at
> [ghassbyte/CigiP3dSimConnect](https://github.com/ghassbyte/CigiP3dSimConnect)
> (private repository).

A small Windows console app that decodes CIGI v3.x (Common Image Generator
Interface) traffic sent from a HOST image generator to a master IG and
writes the aircraft''s position / attitude / time-of-day into the local
Prepar3D instance via FSUIPC.

Two operating modes plus a one-shot reposition-during-replay hook:

| Mode               | Purpose                                                 |
| ------------------ | ------------------------------------------------------- |
| `--bin <file>`     | Replay a recorded `.bin` capture at its original timing |
| `--live <device>`  | Passively tap a NIC with Npcap (promiscuous mode)       |
| `--reposition ...` | Queue a one-shot teleport, fires on the next datagram   |

## Network layout

```
 +-----------+   CIGI v3.x UDP   +---------------+
 |  HOST     | ================> |  Master IG    |
 | .2 /8000  |                   | (this app)    |
 +-----------+                   +---------------+
                                       | FSUIPC
                                       v
                                 +---------------+
                                 | Prepar3D      |
                                 +---------------+
```

* HOST source IP: `192.168.70.2` (override with `--host-ip`)
* HOST dst UDP port: `8000` (override with `--data-port`)
* Master IG runs the bridge; FSUIPC inside P3D must be loaded.

The HOST <-> master IG link is mirrored to this PC via a network TAP
(or SPAN port). That''s why live capture uses Npcap in promiscuous mode
rather than a normal bound socket -- a socket only sees packets actually
addressed to its own IP; we''re passively tapping traffic addressed to
someone else.

## Build

Open `CigiP3dBridge.sln` in Visual Studio 2015 or newer and build
`x64`. Two prebuilt SDKs sit under `.\external\` and are referenced via
relative paths in the project file, so no environment setup is needed:

* `external\npcap-sdk-1.16\Include` and `Lib\x64` -- Npcap SDK for live capture.
* `external\UIPC64_SDK_C_version2` -- FSUIPC SDK for writes to P3D.

### Toolchain

* Platform Toolset **v140** (Visual Studio 2015). VS 2017 / 2019 / 2022
  can all consume v140 projects if the v140 toolset is installed --
  it isn''t shipped with newer VS versions by default, so you may need
  to download the "Visual Studio 2015 Build Tools" component.
* Windows 10 SDK `10.0.22621.0` (matches what the project was generated
  against; newer 10.x SDKs are fine).
* Target platform: **x64 only**.
* Runtime: `/MT` static CRT in Release, `/MTd` in Debug.
* Linker: admin elevation (`/level=''requireAdministrator''`) is
  required because Npcap''s capture needs raw socket / privileged access.
* Link libs: `FSUIPCuser64.lib`, `wpcap.lib`, `Packet.lib`, `ws2_32.lib`.

### Command-line build (no IDE)

```bat
"%VS140COMNTOOLS%\..\..\VC\vcvarsall.bat" x64
msbuild CigiP3dBridge.sln /p:Configuration=Release /p:Platform=x64 /m
```

Output goes to `x64\Release\CigiP3dBridge.exe` (Debug = `x64\Debug\`).

## Run

```
CigiP3dBridge.exe                       (no args: launch the GUI)
CigiP3dBridge.exe --gui
CigiP3dBridge.exe --list-devices
CigiP3dBridge.exe --live 0 --host-ip 192.168.70.2 --data-port 8000
CigiP3dBridge.exe --bin E:\path\to\capture.bin
CigiP3dBridge.exe --bin E:\path\to\capture.bin --reposition -6.125,106.655,0,90
```

### GUI

Running with no arguments (or `--gui`) opens a small window that lists
every Npcap-capturable NIC, lets you pick one, and START/STOP a live
capture on it (using the default `--host-ip 192.168.70.2 --data-port
8000` filter). The window **cannot** be closed with its X button or
Alt+F4 -- exit only via the EXIT button -- so the bridge isn't torn
down by accident mid-capture.

The CLI modes (`--bin`, `--live`, `--list-devices`) still work exactly
as before for headless or scripted use.

`--reposition` accepts `lat,lon,alt_m,yaw_deg[,pitch_deg,roll_deg]`.
You can pass it multiple times; the last one wins. The teleport fires
on the **next** received datagram (the next bin packet), then the
controller is empty again until you queue another.

### Example: replay a capture but jump to WIII halfway through

```
CigiP3dBridge.exe --bin cgk.bin --reposition -6.125,106.655,10,90
```

Will follow the capture normally until the next CIGI datagram arrives,
at which point it teleports the aircraft to CGK''s coordinates
(-6.125, 106.655) at 10 m altitude, heading 90, before continuing to
apply the rest of the datagram.

## How the code is organised

The source is split into one `.cpp`/`.h` pair per subsystem under
`.\src\`, so each piece stays small enough to read on its own:

| File                        | Responsibility                                    |
| --------------------------- | ------------------------------------------------- |
| `CigiTypes.h`               | Wire structs (Entity/Celestial/Component Control) |
| `FsuipcOffsets.h`           | All FSUIPC offsets + the NewWeather struct        |
| `Conversion.h`              | CIGI -> FSUIPC fixed-point conversions            |
| `CigiProcessor.h/.cpp`      | Walks a CIGI datagram, dispatches each packet     |
| `CigiToFsUIPC.h/.cpp`       | FSUIPC writes + slew-hold watchdog                |
| `BinPlayer.h/.cpp`          | `.bin` replay                                     |
| `PcapCapture.h/.cpp`        | Npcap live capture                                |
| `RepositionController.h/.cpp` | One-shot teleport during replay                |
| `Gui.h/.cpp`                 | Simple Win32 GUI: NIC picker + START/STOP/EXIT |
| `main.cpp`                  | CLI, FSUIPC open/close, mode dispatch              |

Each header has a comment at the top explaining what the file does and
why it exists.

## Threading model

The transport layer (`BinPlayer` / `PcapCapture`) and the FSUIPC-writing
layer are decoupled by `CigiProcessor::processDatagram`. The bin
replay calls it on its own thread; the pcap callback runs on Npcap''s
internal thread; both are safe to call concurrently because every
FSUIPC `Write(...) + Process()` sequence takes the single
`g_fsuipcMutex` for its full duration.

A separate watchdog thread reasserts slew mode every two seconds (a
per-frame `checkAndReassertSlew()` in `CigiToFsUIPC.cpp` covers the
gaps). The watchdog only ever touches `SLEW_MODE_OFFSET`; the entity-
apply path only ever touches position/attitude offsets; so they can
never collide.

## Sign conventions to remember

| Offset                  | CIGI              | FSUIPC                  |
| ----------------------- | ----------------- | ----------------------- |
| Pitch (0x0578)          | + = nose up       | - = pitch up, + = down  |
| Bank (0x057C)           | + = right bank    | - = bank right, + = left|
| Heading (0x0580)        | 0..360            | UINT32, wrap at 2^32    |
| On-ground (0x0366)      | n/a               | 0 = airborne, 1 = ground|
| Slew (0x05DC)           | n/a               | 0 = off, 1 = on         |

Conversion functions in `Conversion.h` handle the sign flips and the
heading uint32 wrap (the classic "stuck at 180" bug when a signed cast
collapses an out-of-range double).

## Cold-start handoff

The very first `Entity 0` CIGI packet of any session triggers a
one-time handoff, which deliberately reproduces in code what the manual
"disable slew, then re-enable it" workaround does. A plain
slew-engage is not enough, for two reasons:

* `0x0366` (on-ground) is **not refreshed while in slew mode**, and
  writes issued while in slew mode **are not reflected either** -- so
  the flag cannot simply be cleared once slew is on.
* Cleared while slew is OFF, the still-live ground model puts it
  straight back to `1`, unless the sim first gets a moment to see the
  aircraft sitting at the (airborne) commanded altitude.

So the handoff walks a small state machine, one step per incoming
`Entity 0` packet:

1. **Start** -- force slew OFF (`0x05DC=0` **and** the companion
   `0x05DE` control write), write the target LLAPBH, and clear
   `0x0366`. Begin the settle window.
2. **Settle** -- keep writing the pose with slew OFF until `0x0366`
   reads `0` (the ground model has accepted "airborne") or the settle
   window (`ON_GROUND_SETTLE_MS`) expires.
3. **Engage** -- turn slew ON (`0x05DC=1` **and** `0x05DE`) and
   **confirm it by reading `0x05DC` back**. On-ground is now frozen at
   whatever the settle reached, so the aircraft is no longer clamped to
   the ground. If `0x0366` is still `1` here, another settle pass is
   run, bounded by `MAX_SETTLE_PASSES`.
4. **Done** -- the per-frame `applyEntityToFs` path and the slew
   watchdog take over.

From there the per-frame path deliberately does **not** re-write
`0x0366` (ineffective in slew mode, and the flag is already where the
settle left it). A 1 Hz `[diag]` line logs the sim''s actual pose
against the last commanded pose, so a "write fight" (oscillating around
the command) can be told apart from live-mode lag (steadily stepping) or
a render-only artifact (delta ~ 0).

The same code path runs for a `--reposition` teleport (queued during
bin replay).

## Capturing traffic to a `.bin`

The `.bin` format this app expects is a sequence of:

```
[uint32 deltaMicros][uint16 len][uint8[len] payload]
[uint32 deltaMicros][uint16 len][uint8[len] payload]
...
```

`deltaMicros` is the gap **before** this packet (microseconds since
the previous packet; 0 for the first packet is normal). `payload` is
a full CIGI datagram (chain of `[id | size | ...]` records).

This is the same format `DirectBinPlayer.exe` used with the previous
bridge, so existing captures replay unchanged.

## Limitations / known gaps

* **No continuous GUI controls.** The GUI (`--gui` / no args) covers NIC
  selection and live-capture START/STOP; reposition and bin replay are
  still driven from the CLI. `RepositionController::request(...)` is the
  seam a future button handler would call to add reposition to the GUI.
* **Live mode doesn''t accept `--reposition`.** In live mode you''re
  already following the wire; overriding that would mean ignoring
  real CIGI traffic. If you want to jump while connected live, switch
  to bin mode.
* **Cloud pattern only.** Other CIGI Component Control sub-IDs are
  logged for awareness but not mapped. The component IDs
  (`0x1001` / `0x9001`) live in `CigiTypes.h` -- update them if you
  connect to a HOST with a different ICD.
* **One-shot teleport only.** No continuous-override mode.
