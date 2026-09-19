// =====================================================================
//  main.cpp
// ---------------------------------------------------------------------
//  CLI entry point for CigiP3dBridge.
//
//  Modes:
//    --bin <file.bin>                replay a recorded CIGI capture
//    --live <device-or-index>        tap a NIC with Npcap (promiscuous)
//    --list-devices                  list Npcap capture devices and exit
//
//  Live-mode filters (defaults match the network setup on this IG PC):
//    --host-ip  192.168.70.2         src IP to accept
//    --data-port 8000                dst UDP port to accept
//
//  Reposition (only meaningful during --bin replay):
//    --reposition lat,lon,alt,yaw[,pitch,roll]
//                                   queue a one-shot teleport. Can be
//                                   passed multiple times -- each one
//                                   replaces the previous. The teleport
//                                   fires on the NEXT received
//                                   datagram (the very next packet
//                                   from the bin file).
//
//  The brief calls for a "simple GUI later" -- the GUI will sit on top
//  of this same main(): the GUI button handlers will just call
//  reposition.parseAndRequest() or the bin/live runners directly.
//  For now, the CLI does everything.
// =====================================================================

#include <winsock2.h>
#include <iostream>
#include <string>
#include <vector>

#include "BinPlayer.h"
#include "CigiToFsUIPC.h"
#include "PcapCapture.h"
#include "RepositionController.h"

namespace {

struct Args {
    std::string mode;             // "bin" or "live"
    std::string binFile;
    std::string liveDevice;
    std::string hostIp      = "192.168.70.2";
    unsigned short dataPort = 8000;
    std::vector<std::string> repositionRequests; // raw "lat,lon,alt,yaw[,p,r]" strings
};

// Minimal argv parser. Supports the flags listed above; anything else
// is ignored with a warning. No flag library needed for this small set.
Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--bin"          && i+1 < argc) { a.mode = "bin";  a.binFile   = argv[++i]; }
        else if (arg == "--live"         && i+1 < argc) { a.mode = "live"; a.liveDevice= argv[++i]; }
        else if (arg == "--host-ip"      && i+1 < argc) { a.hostIp      = argv[++i]; }
        else if (arg == "--data-port"    && i+1 < argc) { a.dataPort    = static_cast<unsigned short>(std::stoi(argv[++i])); }
        else if (arg == "--reposition"   && i+1 < argc) { a.repositionRequests.emplace_back(argv[++i]); }
        else if (arg == "--list-devices") { pcapcap::listCaptureDevices(); std::exit(0); }
        else {
            std::cerr << "[warn] ignoring unknown argument: " << arg << "\n";
        }
    }
    return a;
}

void printUsage(const char* exe) {
    std::cerr <<
        "Usage:\n"
        "  " << exe << " --bin <file.bin> [--reposition lat,lon,alt,yaw[,pitch,roll]] ...\n"
        "  " << exe << " --list-devices\n"
        "  " << exe << " --live <device_or_index> [--host-ip 192.168.70.2] [--data-port 8000]\n"
        "\n"
        "--reposition may be passed multiple times; each request replaces\n"
        "the previous one. The teleport fires on the next received datagram.\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.mode.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    // Winsock isn't strictly needed for either mode (bin uses stdio,
    // live uses pcap), but FSUIPC_Open/SIM_ANY likes it initialised
    // on some P3D installs. Cheap to do, harmless if unused.
    WSADATA wsad;
    if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    if (!cigi::initFsUIPC()) {
        WSACleanup();
        return 1;
    }

    cigi::RepositionController reposition;
    for (const auto& csv : args.repositionRequests) {
        reposition.parseAndRequest(csv);
    }

    if (args.mode == "bin") {
        binplayer::run(args.binFile, &reposition);
    } else {
        pcapcap::run(args.liveDevice, args.hostIp, args.dataPort, &reposition);
    }

    cigi::shutdownFsUIPC();
    WSACleanup();
    return 0;
}


