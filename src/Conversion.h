#pragma once

// =====================================================================
//  Conversion.h
// ---------------------------------------------------------------------
//  Conversions between CIGI's units (degrees lat/lon/alt in metres,
//  heading/pitch/bank in degrees, aerospace sign convention) and the
//  bizarre fixed-point encodings FSUIPC uses for its player-aircraft
//  position/attitude offsets.
//
//  The math itself is documented in FSUIPC_for_Programmers.docx --
//  each constant below is the divisor from that table. The two
//  surprising bits (heading uint32 wrap; pitch/bank int32 wrap) are
//  explained inline at the functions that handle them.
// =====================================================================

#include <cmath>
#include <cstdint>
#include <windows.h>

namespace conv {

// ---------------------------------------------------------------------
//  Latitude / longitude
// ---------------------------------------------------------------------
//  FSUIPC stores player lat/lon as a 64-bit signed integer in units of
//  "90 / (10001750 * 2^32)" degrees for lat and "360 / (2^64)" for lon.
//  Yes, really. The reference bridge's exact constants are kept
//  verbatim -- they're tested to land aircraft within a few centimetres
//  on the runway threshold.
// ---------------------------------------------------------------------
inline long long latitudeToFs(double latDeg) {
    return static_cast<long long>(latDeg / (90.0 / (10001750.0 * 65536.0 * 65536.0)));
}

inline long long longitudeToFs(double lonDeg) {
    return static_cast<long long>(lonDeg / (360.0 / (65536.0 * 65536.0 * 65536.0 * 65536.0)));
}

// ---------------------------------------------------------------------
//  Heading
// ---------------------------------------------------------------------
//  FSUIPC heading (0x0580) is documented as UNSIGNED 32-bit, spanning
//  0..2^32 for 0..360 degrees. Casting an out-of-range double to a
//  SIGNED int32 is undefined behaviour -- on MSVC it collapses to the
//  exact INT_MIN bit pattern every time, which FSUIPC reads back as
//  exactly 180 degrees for the entire 180..360 range. That looks like
//  "stuck at 180, then suddenly snaps at 360->0".
//
//  Returning DWORD (unsigned) and never funnelling the raw value
//  through a signed type removes the overflow entirely.
// ---------------------------------------------------------------------
inline DWORD headingToFs(double headingDeg) {
    headingDeg = std::fmod(headingDeg, 360.0);
    if (headingDeg < 0.0) headingDeg += 360.0;
    // Heading very close to 360 can round up to exactly 2^32 (one past
    // the valid range) due to floating-point rounding. Wrapping
    // through a 64-bit integer with modulo handles that cleanly.
    const unsigned long long raw = static_cast<unsigned long long>(headingDeg * (65536.0 * 65536.0 / 360.0))
                                 % 4294967296ULL;
    return static_cast<DWORD>(raw);
}

// ---------------------------------------------------------------------
//  Pitch / bank
// ---------------------------------------------------------------------
//  Same overflow hazard as heading, but for the +/-180 symmetric range.
//  Wrap first, then cast -- the cast is always in-range.
// ---------------------------------------------------------------------
inline int angleToFs(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg >= 180.0) deg -= 360.0;
    if (deg < -180.0) deg += 360.0;
    return static_cast<int>(deg / (360.0 / (65536.0 * 65536.0)));
}

// FSUIPC sign convention is the OPPOSITE of CIGI's:
//   FSUIPC Pitch (0x0578): -ve = pitch up, +ve = pitch down
//   FSUIPC Bank  (0x057C): -ve = bank right, +ve = bank left
//   CIGI convention:      +pitch = nose up, +roll = right bank
// So both need negating when converting CIGI -> FSUIPC.
inline int pitchToFsUIPC(double cigiPitchDeg) { return angleToFs(-cigiPitchDeg); }
inline int bankToFsUIPC (double cigiRollDeg ) { return angleToFs(-cigiRollDeg ); }

// ---------------------------------------------------------------------
//  Altitude (split into 16E16 high + low halves, FSUIPC's native form)
// ---------------------------------------------------------------------
inline void altitudeToFs(double altMetres, int& highOut, unsigned int& lowOut) {
    const int high = static_cast<int>(altMetres);
    const unsigned int low = static_cast<unsigned int>((altMetres - high) * (65536.0 * 65536.0));
    highOut = high;
    lowOut  = low;
}

// ---------------------------------------------------------------------
//  Shortest-arc delta between two angles on a circle.
//  Used purely for jump-size diagnostics in the main loop; not used
//  for any actual smoothing (the original cause of single-frame jumps
//  was the heading uint32 overflow, now fixed).
// ---------------------------------------------------------------------
inline float angleShortestDelta(float fromDeg, float toDeg) {
    return std::fmod(toDeg - fromDeg + 540.0f, 360.0f) - 180.0f;
}

} // namespace conv
