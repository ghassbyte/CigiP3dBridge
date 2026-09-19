#pragma once

// =====================================================================
//  CigiToFsUIPC.h
// ---------------------------------------------------------------------
//  The "FSUIPC-writing" layer of the bridge.
//
//  Everything in this file talks to the local Prepar3D instance via
//  FSUIPC's IPC client DLL -- it never touches the network, never
//  knows whether CIGI packets came from a live NIC or a recorded
//  bin file. That's the whole point of the split: the transport layer
//  (PcapCapture / BinPlayer) fills buffers, the CIGI processor walks
//  them, and the functions in here react.
//
//  Three thread-safety rules to remember:
//    1. FSUIPC's write buffer is PROCESS-WIDE, not per-thread. Calling
//       FSUIPC_Write(...) + FSUIPC_Process() from multiple threads
//       without a lock lets one thread's Process() flush a PARTIAL
//       sequence from another (e.g. new lat/lon queued but heading/
//       pitch/bank not yet) -- sending one frame with mismatched
//       position+attitude, which looks exactly like a sudden flip.
//    2. Every public function in this header takes g_fsuipcMutex for
//       its full Write+Process sequence. No exceptions.
//    3. The slew-hold watchdog (started by main()) only ever touches
//       SLEW_MODE_OFFSET, never any position offset, so it can't
//       collide with the entity-apply path.
//
//  Include order is deliberate:
//    - <winsock2.h> FIRST -- defines _WINSOCK2_ so the later
//      <windows.h> won't pull in the v1 <winsock.h>.
//    - <windows.h>  -- brings DWORD/WORD/BYTE that FSUIPC_User64.h
//      needs (and the SDK header doesn't include windows.h itself).
//    - <FSUIPC_User64.h> -- declares the IPC API.
// =====================================================================

#include <winsock2.h>
#include <windows.h>
#include <FSUIPC_User64.h>

#include <atomic>
#include <mutex>

#include "CigiTypes.h"

namespace cigi {

// Called once at startup, BEFORE any packets flow. Opens the FSUIPC
// IPC connection and starts the slew-hold watchdog thread.
bool initFsUIPC();

// Called once at shutdown. Stops the watchdog thread and closes the
// connection cleanly.
void shutdownFsUIPC();

// True once the first Entity 0 packet has been processed (cold start
// handoff completed). Used by the CIGI processor to decide whether to
// route subsequent entity packets to applyEntityToFs (normal) or
// applyColdStartEntityToFs (one-time reposition+slew-engage).
bool isColdStartDone();

// Per-frame entity update. Declared here (not just defined in the .cpp)
// because CigiProcessor.cpp calls it directly.
void applyEntityToFs(const EntityControl& ec);

// One-time cold-start handoff: write LLAPBH + clear on-ground, then
// engage slew. The very first Entity 0 packet of a session routes here.
void applyColdStartEntityToFs(const EntityControl& ec);

// One-shot teleport, used by RepositionController. Runs the same
// code path as the cold-start handoff.
void applyReposition(const EntityControl& ec);

// Time-of-day write, from a Celestial Sphere Control packet.
void applyTimeToFs(const CelestialSphereControl& csc);

// Cloud-pattern on/off, from a Component Control packet. Idempotent
// against duplicate frames (HOST re-sends each state 5x for reliability).
void applyCloudPatternToFs(const ComponentControl& cc);

} // namespace cigi
