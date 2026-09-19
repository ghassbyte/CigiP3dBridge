// =====================================================================
//  BinPlayer.cpp
// ---------------------------------------------------------------------
//  Implementation of binplayer::run() declared in BinPlayer.h.
// =====================================================================

#include "BinPlayer.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "CigiProcessor.h"
#include "RepositionController.h"

namespace binplayer {

namespace {

// One packet's worth of file content. We slurp the whole file into a
// vector of these up front, so the replay loop is just a tight
// sleep+dispatch -- no file I/O on the hot path.
struct ReplayPacket {
    uint32_t          deltaMicros = 0;
    std::vector<uint8_t> payload;
};

bool loadReplayFile(const std::string& path, std::vector<ReplayPacket>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
        std::cerr << "Could not open bin file: " << path << std::endl;
        return false;
    }
    while (true) {
        uint32_t deltaMicros = 0;
        uint16_t len         = 0;
        if (std::fread(&deltaMicros, sizeof(deltaMicros), 1, f) != 1) break;
        if (std::fread(&len,         sizeof(len),         1, f) != 1) break;
        ReplayPacket pkt;
        pkt.deltaMicros = deltaMicros;
        pkt.payload.resize(len);
        if (len > 0 && std::fread(pkt.payload.data(), 1, len, f) != len) break;
        out.push_back(std::move(pkt));
    }
    std::fclose(f);
    return !out.empty();
}

} // anonymous namespace

void run(const std::string& path, cigi::RepositionController* reposition) {
    std::vector<ReplayPacket> packets;
    if (!loadReplayFile(path, packets)) return;

    std::cout << "Loaded " << packets.size() << " packets from " << path << std::endl;

    for (const auto& pkt : packets) {
        if (pkt.deltaMicros > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(pkt.deltaMicros));
        }
        cigi::processDatagram(pkt.payload.data(),
                              static_cast<int>(pkt.payload.size()),
                              reposition);
    }
    std::cout << "\nBin replay complete (" << packets.size() << " packets)." << std::endl;
}

} // namespace binplayer
