#pragma once

// =====================================================================
//  RepositionController.h
// ---------------------------------------------------------------------
//  One-shot reposition during bin replay.
//
//  Behaviour:
//    - Call request(lat, lon, altM, yawDeg, pitchDeg, rollDeg) from
//      anywhere (CLI handler, GUI button, console command, ...).
//    - On the NEXT call to processDatagram(), the controller's
//      consumeIfPending() returns true, and processDatagram() calls
//      back into applyReposition() with the synthesised Entity Control.
//    - applyReposition() runs the same cold-start code path the very
//      first Entity 0 packet runs (reposition + clear on-ground +
//      re-engage slew), so the aircraft SNAPS to the new pose. The
//      rest of the datagram's packets then flow as normal.
//
//  Why "one-shot" and not "continuous override":
//    - The brief asked for one-shot only. "Override" would mean every
//      subsequent bin packet's entity data gets clobbered by the
//      target until cleared -- different semantics, different code
//      path, can be added later as a second controller state if needed.
//
//  Why a flag-based "pending" instead of an immediate teleport:
//    - The bin replay loop runs on its own thread; the CLI handler
//      (or future GUI button) might run on the main thread. The flag
//      is the natural hand-off point. atomic<bool> makes it lock-free
//      for both readers.
// =====================================================================

#include <atomic>
#include <string>
#include "CigiTypes.h"   // cigi::EntityControl

namespace cigi {

class RepositionController {
public:
    RepositionController() = default;

    // Queue a one-shot teleport. Validates ranges inline (same envelope
    // as isValidEntityControl). On failure, prints a reason and leaves
    // any pending request in place.
    void request(double latDeg, double lonDeg, double altMetres,
                 double yawDeg, double pitchDeg, double rollDeg);

    // CLI helper: parses "lat,lon,alt,yaw[,pitch,roll]"
    // (degrees + metres). pitch/roll default to 0 (level). Returns
    // true on success.
    bool parseAndRequest(const std::string& csv);

    // Called from processDatagram. Returns true exactly once per
    // queued request, then clears the pending flag.
    bool consumeIfPending();

    // Builds the Entity Control packet that gets handed to
    // applyReposition(). Only valid immediately after a successful
    // consumeIfPending().
    EntityControl buildEntityControl() const;

    bool isPending() const { return m_pending.load(); }

private:
    std::atomic<bool> m_pending{false};
    double m_lat   = 0.0;
    double m_lon   = 0.0;
    double m_alt   = 0.0;
    double m_yaw   = 0.0;
    double m_pitch = 0.0;
    double m_roll  = 0.0;
};

} // namespace cigi
