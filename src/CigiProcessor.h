#pragma once

// =====================================================================
//  CigiProcessor.h
// ---------------------------------------------------------------------
//  Walks one CIGI datagram's chain of [packetID | packetSize | ...]
//  records and dispatches each to the right FSUIPC-writing function.
//
//  A CIGI datagram is just a sequence of these records back-to-back
//  inside a single UDP payload; there is no per-packet framing or
//  alignment. The processor's job is:
//    - Skip packets we don't recognise.
//    - Reject malformed packets (size = 0, size past end-of-buffer,
//      size smaller than the documented struct).
//    - For entity-0 packets, decide between cold-start and per-frame
//      paths based on whether the cold-start handoff has happened yet.
//
//  This is also the single integration point where the new
//  RepositionController hooks in: if a one-shot reposition is pending,
//  the controller injects its own Entity Control packet BEFORE
//  walking the chain, so the aircraft is teleported before any
//  further datagram data is applied.
// =====================================================================

#include <cstdint>
#include "CigiTypes.h"

namespace cigi {

class RepositionController;

// processDatagram -- call this for every received UDP payload, whether
// from the live NIC tap or from the bin-file replay.
//
// `reposition` may be null; if non-null and a one-shot reposition is
// pending, it runs first (synchronously, before the walk continues).
void processDatagram(const uint8_t* buf, int len, RepositionController* reposition);

} // namespace cigi

// The actual FSUIPC-writing entry points live in CigiToFsUIPC.h
// (applyEntityToFs, applyColdStartEntityToFs, applyReposition,
// applyTimeToFs, applyCloudPatternToFs). Including it here too means
// processDatagram's callers don't have to chase a second header.
#include "CigiToFsUIPC.h"
