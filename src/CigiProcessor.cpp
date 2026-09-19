// =====================================================================
//  CigiProcessor.cpp
// ---------------------------------------------------------------------
//  Walks one CIGI datagram's chain of [packetID | packetSize | ...]
//  records and dispatches each to the right FSUIPC-writing function.
//
//  CIGI framing rules we depend on (per the CIGI v3.x ICD):
//    - The first byte of a packet is its ID (e.g. 2 = Entity Control).
//    - The second byte is the TOTAL packet size in bytes, INCLUDING
//      the ID + size bytes themselves. So a 48-byte Entity Control
//      packet has packetSize = 48.
//    - Packets are packed back-to-back inside a UDP payload with NO
//      padding or alignment between them.
//    - The payload MAY end mid-packet (a malformed/truncated capture);
//      we detect that by checking offset + packetSize > len and break.
//
//  We DO NOT parse:
//    - The IG Control / Host Control headers (those are above the
//      datagram layer; the existing reference doesn't bother either).
//    - Any packet ID we don't recognise.
// =====================================================================

#include "CigiProcessor.h"
#include "CigiToFsUIPC.h"

#include <iostream>

#include "RepositionController.h"

namespace cigi {

void processDatagram(const uint8_t* buf, int len, RepositionController* reposition) {
    if (!buf || len <= 0) return;

    // One-shot reposition: if pending, run it BEFORE walking the rest
    // of the datagram. This way the aircraft is teleported to the new
    // position before any other packets in the same UDP payload are
    // applied (which might otherwise overwrite it).
    if (reposition && reposition->consumeIfPending()) {
        EntityControl target = reposition->buildEntityControl();
        applyReposition(target);
    }

    int offset = 0;
    while (offset + 2 <= len) {
        const uint8_t packetID   = buf[offset];
        const uint8_t packetSize = buf[offset + 1];

        if (packetSize == 0)        break; // protocol violation
        if (offset + packetSize > len) break; // truncated capture

        switch (packetID) {
        case PACKET_ID_ENTITY_CONTROL: {
            if (packetSize < sizeof(EntityControl)) break;
            const EntityControl* ec = reinterpret_cast<const EntityControl*>(buf + offset);
            if (ec->entityID != 0) break; // not our aircraft
            std::string rejectReason;
            if (!isValidEntityControl(*ec, rejectReason)) {
                std::cerr << "\n[REJECTED] Entity 0 packet out of valid range, not applied: "
                          << rejectReason << " (lat=" << ec->lat << " lon=" << ec->lon
                          << " alt=" << ec->alt << " roll=" << ec->roll
                          << " pitch=" << ec->pitch << " yaw=" << ec->yaw
                          << "). Keeping last known-good state." << std::endl;
            } else if (!isColdStartDone()) {
                applyColdStartEntityToFs(*ec);
            } else {
                applyEntityToFs(*ec);
            }
            break;
        }
        case PACKET_ID_CELESTIAL_SPHERE_CONTROL: {
            if (packetSize < sizeof(CelestialSphereControl)) break;
            const CelestialSphereControl* csc =
                reinterpret_cast<const CelestialSphereControl*>(buf + offset);
            applyTimeToFs(*csc);
            break;
        }
        case PACKET_ID_COMPONENT_CONTROL: {
            if (packetSize < sizeof(ComponentControl)) break;
            const ComponentControl* cc =
                reinterpret_cast<const ComponentControl*>(buf + offset);
            // applyCloudPatternToFs checks componentID/instanceID/state internally.
            applyCloudPatternToFs(*cc);
            break;
        }
        default:
            // Unknown packet ID -- skip. We don't log every skip because
            // CIGI datagrams are dense and a per-packet log line would
            // spam stdout; if you need to debug a specific unknown
            // packet, add a temporary `std::cerr << "skip id=" << ...`
            // here.
            break;
        }
        offset += packetSize;
    }
}

} // namespace cigi
