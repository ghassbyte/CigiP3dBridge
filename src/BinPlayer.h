#pragma once

// =====================================================================
//  BinPlayer.h
// ---------------------------------------------------------------------
//  Mode 1 of the bridge: read a recorded CIGI bin file and replay it
//  into processDatagram() at the original timing.
//
//  File format (matches the original DirectBinPlayer.exe and the
//  reference bridge's reader):
//      [uint32 deltaMicros][uint16 len][uint8[len] payload]
//      [uint32 deltaMicros][uint16 len][uint8[len] payload]
//      ...
//
//  deltaMicros is the gap BEFORE this packet (microseconds since the
//  previous packet). The very first packet's deltaMicros is typically
//  zero; that's fine.
//
//  payload is a CIGI datagram (a chain of [id | size | ...] records,
//  as the CIGI processor walks). The same file format works for any
//  length of CIGI datagram inside one "packet" record.
// =====================================================================

#include <cstdint>
#include <string>

namespace cigi { class RepositionController; }

namespace binplayer {

// Reads the whole .bin into memory, then sleeps deltaMicros before
// each payload and feeds it through the CIGI processor. `reposition`
// may be null; if non-null, the controller can inject one-shot
// teleports while the replay is in progress.
void run(const std::string& path, cigi::RepositionController* reposition);

} // namespace binplayer

