// =====================================================================
//  CigiToFsUIPC.cpp
// ---------------------------------------------------------------------
//  Implements the FSUIPC-writing layer declared in CigiToFsUIPC.h.
//
//  Sections:
//    1. Process-wide state (mutex, cold-start flag, watchdog thread).
//    2. initFsUIPC / shutdownFsUIPC -- open/close + watchdog.
//    3. checkAndReassertSlew -- per-frame slew guard.
//    4. applyColdStartEntityToFs -- one-time reposition + slew engage.
//    5. applyEntityToFs -- per-frame LLAPBH write.
//    6. applyTimeToFs / applyCloudPatternToFs -- the other CIGI packets.
//    7. RepositionController hook (declared in RepositionController.h).
// =====================================================================

#include "CigiToFsUIPC.h"

#include <atomic>
#include <cstring>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <string>
#include <thread>

#include "CigiTypes.h"
#include "Conversion.h"
#include "FsuipcOffsets.h"

namespace cigi {

// ---------------------------------------------------------------------
//  1. Process-wide state
// ---------------------------------------------------------------------
namespace {

// FSUIPC's write buffer is shared across the whole process, not per-
// thread. Every Write+Process sequence in this file takes this lock.
std::mutex g_fsuipcMutex;

// Becomes true after applyColdStartEntityToFs finishes its
// reposition+slew-engage handoff. The slew-hold watchdog stays idle
// until this flips, and subsequent entity packets go through the
// normal applyEntityToFs path.
std::atomic<bool> g_coldStartDone{false};

// Watchdog thread lifecycle.
std::atomic<bool> g_running{true};
std::thread      g_slewThread;

// Pitch/bank write enable. Kept on by default -- turning it off makes
// the aircraft hold level pitch/roll while still following lat/lon/
// alt/heading, which is occasionally useful for debugging the
// conversion code in isolation. Toggle from main() if needed.
constexpr bool kEnablePitchBank = true;

} // anonymous namespace

// ---------------------------------------------------------------------
//  3. Slew guard -- reads current slew state and re-asserts if needed
// ---------------------------------------------------------------------
//  Without this, P3D can drop slew mode between the 2-second watchdog
//  reasserts (accidental key/joystick input, some internal state
//  change) and the aircraft then flies under normal physics for up to
//  2 seconds with no CIGI input -- it can roll/drift, then SNAP BACK
//  when slew re-engages. That reads exactly like a sudden flip with
//  no CIGI-side cause. Checking every frame shrinks the exposure from
//  2s down to ~10ms.
namespace {
void checkAndReassertSlew() {
    std::lock_guard<std::mutex> lock(g_fsuipcMutex);
    DWORD dwResult;
    WORD current = 0xFFFF;
    if (!FSUIPC_Read(SLEW_MODE_OFFSET, 2, &current, &dwResult) || !FSUIPC_Process(&dwResult)) {
        std::cerr << "[slew check] read failed. Error: " << dwResult << std::endl;
        return;
    }
    if (current == 0) {
        std::cerr << "\n[WARNING] Slew mode was OFF (dropped externally, not by this program) "
                     "-- re-engaging now." << std::endl;
        WORD one = 1;
        if (!FSUIPC_Write(SLEW_MODE_OFFSET, 2, &one, &dwResult) || !FSUIPC_Process(&dwResult)) {
            std::cerr << "[slew check] failed to re-engage slew. Error: " << dwResult << std::endl;
        }
    }
}
} // anonymous namespace

// ---------------------------------------------------------------------
//  2. Watchdog thread
// ---------------------------------------------------------------------
//  Reasserts slew mode every SLEW_REASSERT_INTERVAL_MS once the cold-
//  start handoff is complete. Stays idle before that -- the cold start
//  itself does the first slew write, and we don't want to start
//  reasserting before the aircraft has actually been repositioned.
namespace {
void slewHoldLoop() {
    while (g_running) {
        if (g_coldStartDone) {
            std::lock_guard<std::mutex> lock(g_fsuipcMutex);
            DWORD dwResult;
            WORD one = 1;
            if (!FSUIPC_Write(SLEW_MODE_OFFSET, 2, &one, &dwResult) || !FSUIPC_Process(&dwResult)) {
                std::cerr << "[slew] failed to assert slew mode. Error: " << dwResult << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(SLEW_REASSERT_INTERVAL_MS));
    }
}
} // anonymous namespace

bool initFsUIPC() {
    DWORD dwResult;
    if (!FSUIPC_Open(SIM_ANY, &dwResult)) {
        std::cerr << "Failed to open FSUIPC connection. Error: " << dwResult << std::endl;
        return false;
    }
    std::cout << "Connected to local P3D via FSUIPC." << std::endl;
    std::cout << "Waiting for the first CIGI Entity 0 packet to perform the cold-start handoff\n"
                 "(reposition + clear on-ground flag, THEN engage slew)..." << std::endl;
    g_running       = true;
    g_coldStartDone = false;
    g_slewThread    = std::thread(slewHoldLoop);
    return true;
}

void shutdownFsUIPC() {
    g_running = false;
    if (g_slewThread.joinable()) g_slewThread.join();
    FSUIPC_Close();
}

bool isColdStartDone() { return g_coldStartDone.load(); }

// ---------------------------------------------------------------------
//  4. One-time cold-start handoff
// ---------------------------------------------------------------------
//  Sequence and rationale (matches the original bridge):
//
//    1. Write the target LLAPBH AND clear the on-ground flag together,
//       while slew is still OFF. With slew off, FS's normal reposition
//       handling is in charge of reconciling "aircraft is now 6000 nm
//       away, marked airborne" -- the same path an ordinary "teleport"
//       write goes through. This is the moment to let FS do that
//       reconciliation; we don't want slew suppressing the bits of
//       the flight model that would otherwise settle it cleanly.
//
//    2. Only once that's landed does slew mode get turned on. From
//       here on, the on-ground flag is frozen (per FSUIPC's docs) at
//       the 0 we just set, and the per-frame applyEntityToFs path plus
//       the slew-hold watchdog take over.
//
//  If your aircraft still creeps/slides toward the capture's start
//  point instead of snapping, that means the ground-contact model
//  needs more than just this flag cleared for your specific
//  aircraft/scenery combo -- some add-ons track their own on-ground
//  state outside the offset.
void applyColdStartEntityToFs(const EntityControl& ec) {
    DWORD dwResult;
    long long latitudeFS  = conv::latitudeToFs(ec.lat);
    long long longitudeFS = conv::longitudeToFs(ec.lon);
    DWORD headingFS       = conv::headingToFs(ec.yaw);
    int altHigh = 0; unsigned int altLow = 0;
    conv::altitudeToFs(ec.alt, altHigh, altLow);
    WORD airborne = 0; // clear "on ground"

    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        ok = FSUIPC_Write(LATITUDE_OFFSET, 8, &latitudeFS,  &dwResult)
          && FSUIPC_Write(LONGITUDE_OFFSET,8, &longitudeFS, &dwResult)
          && FSUIPC_Write(ALTITUDE_HIGH_OFFSET,4, &altHigh,  &dwResult)
          && FSUIPC_Write(ALTITUDE_LOW_OFFSET, 4, &altLow,   &dwResult)
          && FSUIPC_Write(HEADING_OFFSET,    4, &headingFS, &dwResult)
          && FSUIPC_Write(ON_GROUND_OFFSET,  2, &airborne,  &dwResult);

        if (kEnablePitchBank) {
            int pitchFS = conv::pitchToFsUIPC(ec.pitch);
            int bankFS  = conv::bankToFsUIPC(ec.roll);
            ok = ok
              && FSUIPC_Write(PITCH_OFFSET, 4, &pitchFS, &dwResult)
              && FSUIPC_Write(BANK_OFFSET,  4, &bankFS,  &dwResult);
        }
        ok = ok && FSUIPC_Process(&dwResult);
    }
    if (!ok) {
        std::cerr << "\n[cold start] FSUIPC write/process failed. Error: " << dwResult << std::endl;
        return;
    }

    std::cout << std::fixed << std::setprecision(7)
              << "\n[cold start] Repositioned to lat=" << ec.lat
              << " lon=" << ec.lon << " alt=" << ec.alt
              << "m, cleared on-ground flag. Engaging slew mode now." << std::endl;

    {
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        WORD one = 1;
        if (FSUIPC_Write(SLEW_MODE_OFFSET, 2, &one, &dwResult) && FSUIPC_Process(&dwResult)) {
            std::cout << "[cold start] Slew mode ON. Handing off to normal per-frame updates." << std::endl;
        } else {
            std::cerr << "[cold start] Failed to engage slew mode. Error: " << dwResult << std::endl;
        }
    }
    g_coldStartDone.store(true);
}

// ---------------------------------------------------------------------
//  5. Per-frame LLAPBH write
// ---------------------------------------------------------------------
//
//  Each call:
//    - re-checks slew is on (per-frame guard, see checkAndReassertSlew)
//    - runs the same big-endian-safe conversions the reference bridge uses
//    - acquires the FSUIPC mutex for the whole Write+Process sequence
//      so the slew watchdog can never flush a partial update
//    - writes LLAPBH (and pitch/bank if enabled) in one atomic Process()
//    - logs the new pose to stdout with `\r` so it overwrites itself
//      in-place instead of scrolling
//
//  Anomaly detection: if any of roll/pitch/yaw jumps by more than 90
//  degrees in one frame, that's flagged. The reference bridge
//  identified this as a HOST reset (when the new values are also
//  nearly level) vs. a real anomaly (when they're not).
void applyEntityToFs(const EntityControl& ec) {
    DWORD dwResult;

    checkAndReassertSlew();

    // Diagnostic-only jump logging. No smoothing happens here -- the
    // original cause of single-frame jumps was the heading uint32
    // overflow, now fixed in conv::headingToFs.
    static bool  haveLast = false;
    static float lastRoll = 0, lastPitch = 0, lastYaw = 0;
    if (haveLast) {
        const bool bigJump =
            std::fabs(conv::angleShortestDelta(ec.roll,  lastRoll))  > 90.0f ||
            std::fabs(conv::angleShortestDelta(ec.pitch, lastPitch)) > 90.0f ||
            std::fabs(conv::angleShortestDelta(ec.yaw,   lastYaw))   > 90.0f;
        const bool looksLikeHostReset =
            bigJump && std::fabs(ec.roll) < 0.01f && std::fabs(ec.pitch) < 1.5f;
        if (looksLikeHostReset) {
            std::cout << "\n[HOST RESET] Attitude snapped to a level reference orientation "
                      << "(roll=" << ec.roll << " pitch=" << ec.pitch << " yaw=" << ec.yaw
                      << ") -- this is HOST's own known reset/parked state, not a bridge bug."
                      << std::endl;
        } else if (bigJump) {
            std::cerr << "\n[ANOMALY] Large single-frame attitude jump: "
                      << "roll "  << lastRoll  << "->" << ec.roll
                      << " pitch "<< lastPitch << "->" << ec.pitch
                      << " yaw "  << lastYaw   << "->" << ec.yaw
                      << " (entityType=" << ec.entityType << " parentID=" << ec.parentID << ")"
                      << std::endl;
        }
    }
    lastRoll = ec.roll; lastPitch = ec.pitch; lastYaw = ec.yaw; haveLast = true;

    long long latitudeFS  = conv::latitudeToFs(ec.lat);
    long long longitudeFS = conv::longitudeToFs(ec.lon);
    DWORD headingFS       = conv::headingToFs(ec.yaw);
    int altHigh = 0; unsigned int altLow = 0;
    conv::altitudeToFs(ec.alt, altHigh, altLow);

    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        ok = FSUIPC_Write(LATITUDE_OFFSET,  8, &latitudeFS,  &dwResult)
          && FSUIPC_Write(LONGITUDE_OFFSET, 8, &longitudeFS, &dwResult)
          && FSUIPC_Write(ALTITUDE_HIGH_OFFSET, 4, &altHigh, &dwResult)
          && FSUIPC_Write(ALTITUDE_LOW_OFFSET,  4, &altLow,  &dwResult)
          && FSUIPC_Write(HEADING_OFFSET,    4, &headingFS,  &dwResult);

        if (kEnablePitchBank) {
            int pitchFS = conv::pitchToFsUIPC(ec.pitch);
            int bankFS  = conv::bankToFsUIPC(ec.roll);
            ok = ok
              && FSUIPC_Write(PITCH_OFFSET, 4, &pitchFS, &dwResult)
              && FSUIPC_Write(BANK_OFFSET,  4, &bankFS,  &dwResult);
        }
        ok = ok && FSUIPC_Process(&dwResult);
    }
    if (!ok) {
        std::cerr << "FSUIPC write/process failed. Error: " << dwResult << std::endl;
        return;
    }
    std::cout << std::fixed << std::setprecision(8)
              << "Entity0 -> lat=" << ec.lat
              << " lon=" << ec.lon
              << " alt=" << ec.alt
              << " roll=" << ec.roll
              << " pitch=" << ec.pitch
              << " yaw="  << ec.yaw
              << "\r" << std::flush;
}

// ---------------------------------------------------------------------
//  6a. Time of day
// ---------------------------------------------------------------------
void applyTimeToFs(const CelestialSphereControl& csc) {
    DWORD dwResult;
    const uint8_t hour   = csc.hour;
    const uint8_t minute = csc.minute;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        ok = FSUIPC_Write(LOCAL_HOUR_OFFSET,   1, const_cast<uint8_t*>(&hour),   &dwResult)
          && FSUIPC_Write(LOCAL_MINUTE_OFFSET, 1, const_cast<uint8_t*>(&minute), &dwResult)
          && FSUIPC_Process(&dwResult);
    }
    if (!ok) {
        std::cerr << "FSUIPC time write/process failed. Error: " << dwResult << std::endl;
        return;
    }
    std::cout << "\nTime of day -> Local " << static_cast<int>(hour)
              << ":" << static_cast<int>(minute) << std::endl;
}

// ---------------------------------------------------------------------
//  6b. Cloud pattern
// ---------------------------------------------------------------------
//  Writes a NewWeather struct to the C800 window. HOST resends the
//  same state five times for reliability; we only react on actual
//  changes so we don't hammer FSUIPC with redundant writes.
namespace {
bool g_globalModePrimed = false;
int  g_lastCloudState   = -1;
}
void applyCloudPatternToFs(const ComponentControl& cc) {
    if (cc.componentID != CLOUD_PATTERN_COMPONENT_ID) return;
    if (cc.instanceID  != CLOUD_PATTERN_INSTANCE_ID ) return;
    if (cc.state       == g_lastCloudState) return;
    g_lastCloudState = cc.state;

    const bool on = (cc.state != 0);
    DWORD dwResult;

    std::cout << "\n[cloud pattern] state=" << (on ? "ON" : "OFF")
              << " baseAlt=" << cc.baseAltitudeM << "m" << std::endl;

    // One-time priming: tells FS to use global weather mode, so all
    // subsequent GLOB-ICAO writes apply everywhere.
    if (!g_globalModePrimed) {
        NewWeather priming;
        std::memset(&priming, 0, sizeof(priming));
        priming.uCommand = NW_GLOBAL;
        std::memcpy(priming.chICAO, "GLOB", 4);
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        if (FSUIPC_Write(WEATHER_NWI_WRITE_OFFSET, sizeof(priming), &priming, &dwResult)
         && FSUIPC_Process(&dwResult)) {
            g_globalModePrimed = true;
            std::cout << "[cloud pattern] Primed FSX/P3D into global weather mode." << std::endl;
        } else {
            std::cerr << "[cloud pattern] Failed to prime global weather mode. Error: " << dwResult << std::endl;
        }
    }

    NewWeather w;
    std::memset(&w, 0, sizeof(w));
    w.uCommand = NW_SETEXACT;
    std::memcpy(w.chICAO, "GLOB", 4);
    if (on) {
        w.nCloudsCtr = 1;
        w.Cloud[0].LowerAlt  = static_cast<uint16_t>(cc.baseAltitudeM);
        w.Cloud[0].UpperAlt  = static_cast<uint16_t>(cc.baseAltitudeM + CLOUD_THICKNESS_M);
        w.Cloud[0].Coverage  = CLOUD_COVERAGE_OKTAS_ON;
        w.Cloud[0].Type      = CLOUD_TYPE_STRATUS;
    }
    // else: nCloudsCtr stays 0 -- NW_SETEXACT with no cloud layers clears them.

    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_fsuipcMutex);
        ok = FSUIPC_Write(WEATHER_NWI_WRITE_OFFSET, sizeof(w), &w, &dwResult)
          && FSUIPC_Process(&dwResult);
    }
    if (!ok) {
        std::cerr << "[cloud pattern] FSUIPC weather write/process failed. Error: " << dwResult << std::endl;
        return;
    }
    if (on) {
        std::cout << "[cloud pattern] Global cloud layer set: base=" << w.Cloud[0].LowerAlt
                  << "m ceiling=" << w.Cloud[0].UpperAlt << "m coverage="
                  << static_cast<int>(CLOUD_COVERAGE_OKTAS_ON) << "/8 octas" << std::endl;
    } else {
        std::cout << "[cloud pattern] Global cloud layer cleared." << std::endl;
    }
}

// ---------------------------------------------------------------------
//  7. RepositionController hook
// ---------------------------------------------------------------------
//  Called by RepositionController when a one-shot teleport is pending.
//  Runs the same cold-start path (reposition + clear on-ground +
//  re-engage slew), then signals the controller that the request has
//  been delivered so it can clear its "pending" flag.
void applyReposition(const EntityControl& ec) {
    std::cout << "\n[reposition] one-shot teleport -> lat=" << ec.lat
              << " lon=" << ec.lon << " alt=" << ec.alt
              << "m yaw=" << ec.yaw << " pitch=" << ec.pitch << " roll=" << ec.roll
              << std::endl;
    applyColdStartEntityToFs(ec);
}

} // namespace cigi

