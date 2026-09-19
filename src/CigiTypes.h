#pragma once

// =====================================================================
//  CigiTypes.h
// ---------------------------------------------------------------------
//  Wire-format structs for the CIGI v3.x packets this bridge cares about.
//
//  Every struct here uses #pragma pack(push,1) so its in-memory layout
//  matches exactly what's on the network -- the CIGI protocol is
//  little-endian, no padding, no alignment slack, and our parser casts
//  directly from a received buffer into these structs.
//
//  The static_assert at the bottom of each struct catches any future
//  change that would push the layout off the documented byte count
//  (CIGI Entity Control = 48 bytes, Celestial Sphere Control = 16,
//  Component Control = 32).
//
//  Field meanings:
//    Entity Control (ownship motion, HOST -> us at ~60 Hz):
//      roll/pitch/yaw  -- degrees, aerospace sign convention
//                         (+pitch=nose up, +roll=right bank,
//                          +yaw=clockwise from north)
//      lat/lon/alt     -- geodetic degrees + metres AMSL
//      entityID=0      -- "the aircraft HOST is driving"
//    Celestial Sphere Control -- sets P3D's local hour/minute
//    Component Control -- vendor-defined toggles; for THIS HOST the
//                         cloud pattern is componentID=0x1001,
//                         instanceID=0x9001, state=1/0, baseAltitudeM
//                         at bytes[16:20].
// =====================================================================

#include <cstdint>
#include <string>

namespace cigi {

constexpr uint8_t PACKET_ID_ENTITY_CONTROL           = 2;
constexpr uint8_t PACKET_ID_COMPONENT_CONTROL        = 4;
constexpr uint8_t PACKET_ID_CELESTIAL_SPHERE_CONTROL = 9;

#pragma pack(push, 1)
struct EntityControl {
    uint8_t  packetID;
    uint8_t  packetSize;
    uint16_t entityID;
    uint8_t  stateFlags;
    uint8_t  animFlags;
    uint8_t  alpha;
    uint8_t  reserved;
    uint16_t entityType;
    uint16_t parentID;
    float    roll;
    float    pitch;
    float    yaw;
    double   lat;
    double   lon;
    double   alt;
};
#pragma pack(pop)
static_assert(sizeof(EntityControl) == 48, "EntityControl must be 48 bytes");

#pragma pack(push, 1)
struct CelestialSphereControl {
    uint8_t  packetID;
    uint8_t  packetSize;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  flags;
    uint8_t  reserved1;
    uint32_t date;
    uint8_t  starFieldIntensity;
    uint8_t  reserved2[5];
};
#pragma pack(pop)
static_assert(sizeof(CelestialSphereControl) == 16, "CelestialSphereControl must be 16 bytes");

#pragma pack(push, 1)
struct ComponentControl {
    uint8_t  packetID;
    uint8_t  packetSize;
    uint16_t componentID;
    uint16_t instanceID;
    uint8_t  componentClass;
    uint8_t  state;
    float    param1;
    float    param2;
    float    baseAltitudeM;
    float    param4;
    uint8_t  reserved[8];
};
#pragma pack(pop)
static_assert(sizeof(ComponentControl) == 32, "ComponentControl must be 32 bytes");

// Cloud-pattern component / instance IDs as emitted by THIS HOST.
// If you ever connect to a different HOST with a different ICD, these
// are the only two constants you'd need to update.
constexpr uint16_t CLOUD_PATTERN_COMPONENT_ID = 0x1001;
constexpr uint16_t CLOUD_PATTERN_INSTANCE_ID  = 0x9001;

// Inline finite check, kept here to make this header dependency-light
// (no <cmath>).
inline bool isFinite(double v) {
    return (v == v) && ((v - v) == 0.0);
}

// Range validation. fmod()-wrapping alone won't rescue a NaN
// (fmod(NaN,360) is NaN), and casting NaN to int is undefined
// behaviour on MSVC -- so corrupted/malformed packets must be
// rejected outright, not "fixed" downstream.
//
// Altitude limits are deliberately generous (below Dead-Sea-level
// airports up to well above normal cruise) -- a corruption guard,
// not an operating-area fence.
inline bool isValidEntityControl(const EntityControl& ec, std::string& reason) {
    constexpr double ALT_MIN_M = -500.0;
    constexpr double ALT_MAX_M = 15000.0;
    if (!isFinite(ec.lat) || !isFinite(ec.lon) || !isFinite(ec.alt)
        || !isFinite(ec.roll) || !isFinite(ec.pitch) || !isFinite(ec.yaw)) {
        reason = "non-finite value (NaN/Infinity) in one or more fields";
        return false;
    }
    if (ec.lat < -90.0 || ec.lat > 90.0) {
        reason = "latitude out of range";
        return false;
    }
    if (ec.lon < -180.0 || ec.lon > 180.0) {
        reason = "longitude out of range";
        return false;
    }
    if (ec.alt < ALT_MIN_M || ec.alt > ALT_MAX_M) {
        reason = "altitude out of range";
        return false;
    }
    return true;
}

} // namespace cigi
