// =====================================================================
//  RepositionController.cpp
// ---------------------------------------------------------------------
//  Implementation of the one-shot reposition hook declared in
//  RepositionController.h.
// =====================================================================

#include "RepositionController.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

namespace cigi {

namespace {

// NaN / range checks lifted out of isValidEntityControl so we can
// reuse them here without a std::string for the reject reason.
bool looksFinite(double v) {
    return (v == v) && ((v - v) == 0.0);
}

} // anonymous namespace

void RepositionController::request(double latDeg, double lonDeg, double altMetres,
                                   double yawDeg, double pitchDeg, double rollDeg) {
    if (!looksFinite(latDeg) || !looksFinite(lonDeg) || !looksFinite(altMetres)
     || !looksFinite(yawDeg) || !looksFinite(pitchDeg) || !looksFinite(rollDeg)) {
        std::cerr << "[reposition] rejected: non-finite value in input.\n";
        return;
    }
    if (latDeg < -90.0 || latDeg > 90.0) {
        std::cerr << "[reposition] rejected: latitude " << latDeg << " out of [-90,90].\n";
        return;
    }
    if (lonDeg < -180.0 || lonDeg > 180.0) {
        std::cerr << "[reposition] rejected: longitude " << lonDeg << " out of [-180,180].\n";
        return;
    }
    if (altMetres < -500.0 || altMetres > 15000.0) {
        std::cerr << "[reposition] rejected: altitude " << altMetres
                  << "m outside operational envelope.\n";
        return;
    }

    m_lat   = latDeg;
    m_lon   = lonDeg;
    m_alt   = altMetres;
    m_yaw   = yawDeg;
    m_pitch = pitchDeg;
    m_roll  = rollDeg;
    m_pending.store(true);
    std::cout << "[reposition] queued: lat=" << latDeg << " lon=" << lonDeg
              << " alt=" << altMetres << "m yaw=" << yawDeg
              << " pitch=" << pitchDeg << " roll=" << rollDeg << std::endl;
}

bool RepositionController::parseAndRequest(const std::string& csv) {
    // Split the comma-separated string. Accepts 4..6 fields:
    //   lat,lon,alt,yaw
    //   lat,lon,alt,yaw,pitch
    //   lat,lon,alt,yaw,pitch,roll
    std::vector<std::string> tok;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) tok.push_back(item);
    if (tok.size() < 4 || tok.size() > 6) {
        std::cerr << "[reposition] expected 'lat,lon,alt,yaw[,pitch,roll]' "
                     "(4-6 comma-separated numbers), got " << tok.size() << ".\n";
        return false;
    }
    double vals[6] = {0,0,0,0,0,0};
    for (size_t i = 0; i < tok.size(); ++i) {
        try { vals[i] = std::stod(tok[i]); }
        catch (...) {
            std::cerr << "[reposition] field " << (i+1) << " ('" << tok[i]
                      << "') is not a number.\n";
            return false;
        }
    }
    request(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
    return true;
}

bool RepositionController::consumeIfPending() {
    bool expected = true;
    return m_pending.compare_exchange_strong(expected, false);
}

EntityControl RepositionController::buildEntityControl() const {
    EntityControl ec{};
    // CIGI Entity Control header. We only need entityID=0 (ownship)
    // and a sane packetSize so any future isValidEntityControl check
    // accepts it.
    ec.packetID   = PACKET_ID_ENTITY_CONTROL;
    ec.packetSize = sizeof(EntityControl);
    ec.entityID   = 0;
    ec.lat        = m_lat;
    ec.lon        = m_lon;
    ec.alt        = m_alt;
    ec.yaw        = static_cast<float>(m_yaw);
    ec.pitch      = static_cast<float>(m_pitch);
    ec.roll       = static_cast<float>(m_roll);
    return ec;
}

} // namespace cigi

