// =====================================================================
//  PcapCapture.cpp
// ---------------------------------------------------------------------
//  Implementation of pcapcap::listCaptureDevices() and pcapcap::run().
//
//  Build dependency: <pcap.h> + wpcap.lib from the Npcap SDK in
//  .\external\npcap-sdk-1.16\. Both are linked automatically via the
//  project's AdditionalDependencies (FSUIPCuser64.lib; wpcap.lib;
//  Packet.lib; ws2_32.lib).
//
//  Include-order notes (the classic Winsock trap):
//    - <winsock2.h> FIRST -- it activates the _WINSOCK2_ guard that
//      prevents <windows.h> from pulling in the v1 <winsock.h> later.
//    - Then <windows.h>, which on modern SDKs respects that guard.
//    - Then <pcap.h>.
//  If <windows.h> (or anything that pulls it in, like <pcap.h> via
//  its own internal includes) sneaks in before <winsock2.h>, you get
//  hundreds of "redefinition; different linkage" errors from
//  winsock2.h colliding with the winsock.h that <windows.h> pulled in.
//  Defining _WINSOCK2_ explicitly up front is the bullet-proof guard.
// =====================================================================

#ifndef _WINSOCK2_
#define _WINSOCK2_
#endif

#include "PcapCapture.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pcap.h>

#include "CigiProcessor.h"
#include "RepositionController.h"

namespace pcapcap {

namespace {

// BPF filter src host/dst port, populated from CLI args before opening
// the capture. Used by the in-callback IP-srcip sanity check.
in_addr  g_liveHostFilterAddr{};
uint16_t g_liveDataPort = 8000;

// Set by requestStop() (the GUI's STOP/EXIT). The capture loop checks
// this at least once per read timeout and returns when it's raised.
std::atomic<bool> g_stopRequested{false};

// Resolves a numeric index (as printed by listCaptureDevices) into
// the actual "\\Device\\NPF_{GUID}" name pcap_open_live() needs.
std::string resolveDeviceName(const std::string& deviceArg) {
    bool isNumeric = !deviceArg.empty() && std::all_of(
        deviceArg.begin(), deviceArg.end(),
        [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (!isNumeric) return deviceArg;

    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t* alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        std::cerr << "pcap_findalldevs failed: " << errbuf << std::endl;
        return deviceArg;
    }
    int target = std::stoi(deviceArg);
    int i = 0;
    std::string result = deviceArg;
    for (pcap_if_t* d = alldevs; d != nullptr; d = d->next, ++i) {
        if (i == target) { result = d->name; break; }
    }
    pcap_freealldevs(alldevs);
    return result;
}

// pcap callback. Runs on pcap's internal thread, NOT the main thread.
// processDatagram() is fully thread-safe (it takes g_fsuipcMutex
// internally for every FSUIPC write, and the CIGI parsing itself is
// pure data) so we just forward to it.
void livePacketHandler(u_char* /*user*/,
                       const struct pcap_pkthdr* header,
                       const u_char* packet) {
    if (header->caplen < 14) return;

    size_t offset = 14;
    uint16_t etherType = (static_cast<uint16_t>(packet[12]) << 8) | packet[13];
    if (etherType == 0x8100) { // 802.1Q VLAN tag
        if (header->caplen < 18) return;
        etherType = (static_cast<uint16_t>(packet[16]) << 8) | packet[17];
        offset = 18;
    }
    if (etherType != 0x0800) return; // not IPv4
    if (header->caplen < offset + 20) return;

    const uint8_t* ip = packet + offset;
    const uint8_t ihl = (ip[0] & 0x0F) * 4;
    const uint8_t proto = ip[9];
    if (proto != 17) return; // not UDP

    in_addr srcAddr{};
    std::memcpy(&srcAddr, ip + 12, 4);
    if (std::memcmp(&srcAddr, &g_liveHostFilterAddr, 4) != 0) return;

    if (header->caplen < offset + ihl + 8) return;
    const uint8_t* udp = ip + ihl;
    const uint16_t dstPort =
        (static_cast<uint16_t>(udp[2]) << 8) | udp[3];
    if (dstPort != g_liveDataPort) return;

    uint16_t udpLen = (static_cast<uint16_t>(udp[4]) << 8) | udp[5];
    const size_t payloadOffset = offset + ihl + 8;
    size_t payloadLen = (udpLen >= 8) ? (static_cast<size_t>(udpLen) - 8) : 0;
    if (payloadOffset + payloadLen > header->caplen) {
        // Truncated capture; use what we have rather than dropping.
        payloadLen = header->caplen - payloadOffset;
    }
    cigi::processDatagram(packet + payloadOffset,
                          static_cast<int>(payloadLen),
                          nullptr); // live mode never repositions
}

} // anonymous namespace

void listCaptureDevices() {
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t* alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        std::cerr << "pcap_findalldevs failed: " << errbuf << std::endl;
        return;
    }
    int i = 0;
    for (pcap_if_t* d = alldevs; d != nullptr; d = d->next, ++i) {
        std::cout << "[" << i << "] " << d->name;
        if (d->description) std::cout << " - " << d->description;
        std::cout << std::endl;
    }
    pcap_freealldevs(alldevs);
}

bool listDevices(std::vector<DeviceInfo>& out) {
    out.clear();
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t* alldevs = nullptr;
    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        std::cerr << "pcap_findalldevs failed: " << errbuf << std::endl;
        return false;
    }
    for (pcap_if_t* d = alldevs; d != nullptr; d = d->next) {
        DeviceInfo info;
        info.name        = d->name;
        info.description = d->description ? d->description : "";
        out.push_back(info);
    }
    pcap_freealldevs(alldevs);
    return true;
}

void requestStop() {
    g_stopRequested = true;
}

void run(const std::string& deviceArg,
         const std::string& hostIp,
         unsigned short    dataPort,
         cigi::RepositionController* /*reposition -- ignored in live mode*/) {
    const std::string device = resolveDeviceName(deviceArg);
    inet_pton(AF_INET, hostIp.c_str(), &g_liveHostFilterAddr);
    g_liveDataPort = dataPort;

    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_t* handle = pcap_open_live(device.c_str(),
                                    /*snaplen=*/65536,
                                    /*promisc=*/1,
                                    /*timeout_ms=*/1000,
                                    errbuf);
    if (!handle) {
        std::cerr << "pcap_open_live(" << device << ") failed: " << errbuf << std::endl;
        std::cerr << "Run with --list-devices to see available capture devices." << std::endl;
        return;
    }

    char filterExpr[256];
    std::snprintf(filterExpr, sizeof(filterExpr),
                  "udp and src host %s and dst port %u",
                  hostIp.c_str(), static_cast<unsigned>(dataPort));
    struct bpf_program filter;
    if (pcap_compile(handle, &filter, filterExpr, 1, PCAP_NETMASK_UNKNOWN) == -1
     || pcap_setfilter(handle, &filter) == -1) {
        std::cerr << "Warning: could not apply capture filter '" << filterExpr
                  << "' (" << pcap_geterr(handle) << "). Capturing all traffic "
                     "and filtering in software instead." << std::endl;
    } else {
        std::cout << "Capture filter: " << filterExpr << std::endl;
    }

    std::cout << "Tapping " << device << " for CIGI traffic from " << hostIp
              << " -> UDP " << dataPort << " (promiscuous capture)..." << std::endl;

    // Capture until requestStop() is called (or a fatal error). Using
    // pcap_dispatch in a loop -- rather than a single pcap_loop(..., 0)
    // -- lets us observe g_stopRequested between reads (at least once per
    // 1000ms read timeout) so the GUI can tear the capture down cleanly.
    g_stopRequested = false;
    while (!g_stopRequested.load()) {
        const int rc = pcap_dispatch(handle, /*maxPackets=*/1, livePacketHandler, nullptr);
        if (rc < 0) break;      // fatal error
        if (rc == 0) continue;  // read timeout, nothing captured this round
    }
    pcap_close(handle);
}

} // namespace pcapcap
