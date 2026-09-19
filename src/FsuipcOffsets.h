#pragma once

// =====================================================================
//  FsuipcOffsets.h
// ---------------------------------------------------------------------
//  All FSUIPC offset constants and the New Weather Interface struct in
//  one place, so other files don't have to keep redefining them.
//
//  This header does NOT include FSUIPC_User64.h or <windows.h> --
//  includers that need DWORD/WORD types include them themselves
//  (typically the .cpp that uses these offsets). That keeps this
//  header dependency-light and avoids the classic include-order trap
//  where FSUIPC_User64.h sees a half-defined DWORD.
//
//  Source of truth for the offsets: "FSUIPC for Programmers.docx"
//  shipped with the installed FSUIPC6 SDK on this machine. Every value
//  here was cross-checked against that document.
//
//  Sign conventions to remember when writing:
//    0x0578 Pitch -- negative = pitch up, positive = pitch down
//    0x057C Bank  -- negative = bank right, positive = bank left
//    CIGI pitch is +up, CIGI roll is +right -- so both need negating
//    when converting from CIGI to FSUIPC. See Conversion.h.
// =====================================================================

#include <cstdint>
#include <cstddef>   // offsetof

// ---------------------------------------------------------------------
//  Player aircraft position (LLAPBH -- the standard slew/reposition set)
// ---------------------------------------------------------------------
constexpr uint32_t LATITUDE_OFFSET      = 0x0560; // 8 bytes, SINT64, see Conversion.h
constexpr uint32_t LONGITUDE_OFFSET     = 0x0568; // 8 bytes, SINT64
constexpr uint32_t ALTITUDE_LOW_OFFSET   = 0x0570; // 4 bytes, UINT32 (fractional metres)
constexpr uint32_t ALTITUDE_HIGH_OFFSET  = 0x0574; // 4 bytes, INT32  (integer metres)
constexpr uint32_t HEADING_OFFSET        = 0x0580; // 4 bytes, UINT32, 0..2^32 = 0..360 deg
constexpr uint32_t PITCH_OFFSET          = 0x0578; // 4 bytes, INT32,  +/-180 deg range
constexpr uint32_t BANK_OFFSET           = 0x057C; // 4 bytes, INT32,  +/-180 deg range

// ---------------------------------------------------------------------
//  Time of day
// ---------------------------------------------------------------------
constexpr uint32_t LOCAL_HOUR_OFFSET     = 0x0238; // 1 byte
constexpr uint32_t LOCAL_MINUTE_OFFSET   = 0x0239; // 1 byte

// ---------------------------------------------------------------------
//  Mode / state flags used by the cold-start / slew flow
// ---------------------------------------------------------------------
constexpr uint32_t ON_GROUND_OFFSET      = 0x0366; // 2 bytes, WORD, 0=airborne, 1=on ground
constexpr uint32_t SLEW_MODE_OFFSET      = 0x05DC; // 2 bytes, WORD, 0=off, 1=on

// Background thread reasserts slew this often (ms). Per-frame
// checkAndReassertSlew() in CigiToFsUIPC.cpp covers the gap between
// these reassertions.
constexpr int   SLEW_REASSERT_INTERVAL_MS = 2000;

// ---------------------------------------------------------------------
//  New Weather Interface (NWI)
// ---------------------------------------------------------------------
//  The SDK documents four 1024-byte instances: C000 (read: at aircraft),
//  C400 (read: last global), C800 (WRITE), CC00 (read: selected station).
//  We only ever write C800.
//
//  Struct layout mirrors FSUIPC SDK's NewWeather.h; the static_asserts
//  confirm both the overall 1024-byte size AND a couple of key field
//  offsets against the comments in that header (chICAO @ 0x008,
//  nCloudsCtr @ 0x27C, Cloud[] @ 0x280).
// ---------------------------------------------------------------------
constexpr uint32_t WEATHER_NWI_WRITE_OFFSET = 0xC800;

// NWI uCommand values, from NewWeather.h:
//   NW_SETEXACT -- write the supplied weather struct exactly, bypassing
//                  FSUIPC's own smoothing/user-filter settings.
//   NW_GLOBAL   -- one-time mode switch ("GLOB" ICAO). Only needed once;
//                  after this, every GLOB-ICAO NW_SETEXACT write applies
//                  everywhere in the sim.
constexpr uint16_t NW_SETEXACT = 2;
constexpr uint16_t NW_GLOBAL   = 5;

// ---------------------------------------------------------------------
//  NewWeather sub-structs (exactly as the SDK defines them)
// ---------------------------------------------------------------------
#pragma pack(push, 1)
struct NewWind {
    uint16_t UpperAlt;
    uint16_t Speed;
    uint16_t Gust;
    uint16_t Direction;   // 65536 = 360 degrees
    uint8_t  Turbulence;  // 0-4
    uint8_t  Shear;       // 0-3
    uint16_t Variance;
    uint16_t SpeedFract;
    uint16_t GapAbove;
};
struct NewVis {
    uint16_t UpperAlt;
    int16_t  LowerAlt;
    uint16_t Range;       // 1/100ths sm
    uint16_t Spare;
};
struct NewCloud {
    uint16_t UpperAlt;
    uint16_t LowerAlt;
    uint16_t Deviation;
    uint8_t  Coverage;    // octas, 0-8 (NOT the old 0-65535 encoding)
    uint8_t  Type;        // 1-10 (8=stratus, FS98-style cloud-type table)
    uint8_t  Turbulence;  // 0-4
    uint8_t  Icing;       // 0-4
    int16_t  PrecipBase;
    uint8_t  PrecipType;  // 0-2
    uint8_t  PrecipRate;  // 0-5
    uint8_t  TopShape;
    uint8_t  Spare;
};
struct NewTemp {
    uint16_t Alt;
    int16_t  Day;
    int16_t  DayNightVar;
    int16_t  DewPoint;
};
struct NewPress {
    uint16_t Pressure;    // 16 x mb
    int16_t  Drift;
};
struct NewWeather {
    uint16_t uCommand;
    uint16_t uFlags;
    uint32_t ulSignature;
    char     chICAO[4];           // "GLOB" for global weather
    uint16_t uDynamics;           // 0=none, 4=extreme
    uint16_t uSeconds;
    double   dLatitude;
    double   dLongitude;
    int32_t  nElevation;
    uint32_t ulTimeStamp;
    NewPress Press;
    NewVis   Vis;
    int32_t  nTempCtr;
    NewTemp  Temp[24];
    int32_t  nWindsCtr;
    NewWind  Wind[24];
    int32_t  nCloudsCtr;
    NewCloud Cloud[16];
    char     chUTCstamp[6];
    uint16_t fWriteback;
    uint32_t ulSettingTimeStamp;
    int32_t  nUpperVisCtr;
    NewVis   UpperVis[12];
    uint16_t uSpare[8];
};
#pragma pack(pop)

static_assert(sizeof(NewWeather) == 1024,        "NewWeather must be exactly 1024 bytes");
static_assert(offsetof(NewWeather, chICAO)    == 0x008, "chICAO offset mismatch");
static_assert(offsetof(NewWeather, nCloudsCtr)== 0x27C, "nCloudsCtr offset mismatch");
static_assert(offsetof(NewWeather, Cloud)     == 0x280, "Cloud[] offset mismatch");

// Tunables for the cloud-pattern write. CIGI's Component Control only
// gives an on/off flag + cloud base altitude; everything else is left
// at zero/default. Adjust to taste if your scenery needs a specific
// cloud type or thickness.
constexpr uint8_t CLOUD_COVERAGE_OKTAS_ON = 8;   // full overcast
constexpr uint8_t CLOUD_TYPE_STRATUS      = 8;
constexpr float  CLOUD_THICKNESS_M        = 600.0f;  // ~2000 ft
