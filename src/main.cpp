// =====================================================================
//  main.cpp
// ---------------------------------------------------------------------
//  CLI entry point for CigiP3dBridge.
//
//  Modes:
//    (no args | --gui)               launch the GUI (NIC picker + EXIT)
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
//  The brief calls for a "simple GUI later" -- the GUI sits on top of
//  this same main(): launched with no args (or an explicit --gui), it
//  runs cigi::gui::run(), whose button handlers pick a NIC and call
//  pcapcap::run() on a background thread. The CLI modes below still
//  work unchanged for headless / scripted use.
// =====================================================================

#include <winsock2.h>
#include <windows.h>   // GetConsoleWindow / GetConsoleProcessList / ShowWindow
#include <iostream>
#include <string>
#include <vector>

#include "BinPlayer.h"
#include "CigiToFsUIPC.h"
#include "Gui.h"
#include "GuiLog.h"
#include "PcapCapture.h"
#include "RepositionController.h"

namespace {

struct Args {
    std::string mode;             // "bin" or "live" (empty => GUI)
    bool        gui  = false;     // explicit --gui
    std::string binFile;
    std::string liveDevice;
    std::string hostIp      = "192.168.70.2";
    unsigned short dataPort = 8000;
    std::vector<std::string> repositionRequests; // raw "lat,lon,alt,yaw[,p,r]" strings
};

void printUsage(const char* exe);

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
        else if (arg == "--gui")                        { a.gui = true; }
        else if (arg == "--list-devices") { pcapcap::listCaptureDevices(); std::exit(0); }
        else if (arg == "--help" || arg == "-h") { printUsage(argv[0]); std::exit(0); }
        else {
            std::cerr << "[warn] ignoring unknown argument: " << arg << "\n";
        }
    }
    return a;
}

void printUsage(const char* exe) {
    std::cerr <<
        "Usage:\n"
        "  " << exe << "                                  (no args: launch the GUI)\n"
        "  " << exe << " --gui\n"
        "  " << exe << " --bin <file.bin> [--reposition lat,lon,alt,yaw[,pitch,roll]] ...\n"
        "  " << exe << " --list-devices\n"
        "  " << exe << " --live <device_or_index> [--host-ip 192.168.70.2] [--data-port 8000]\n"
        "\n"
        "--reposition may be passed multiple times; each request replaces\n"
        "the previous one. The teleport fires on the next received datagram.\n"
        "The GUI lets you pick a NIC and START/STOP a live capture; exit\n"
        "only via its EXIT button.\n";
}

} // anonymous namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    // ---- GUI mode (no args, or explicit --gui) ----------------------
    // Same startup as the CLI (Winsock + FSUIPC open) so the capture
    // thread can write to P3D; the window loop blocks until EXIT.
    if (args.gui || args.mode.empty()) {
        WSADATA wsad;
        if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
            std::cerr << "WSAStartup failed." << std::endl;
            return 1;
        }

        // Mirror std::cout/std::cerr into the GUI before FSUIPC opens, so
        // even "Connected to local P3D via FSUIPC." is captured (it is
        // buffered until the window attaches).
        cigi::guilog::install();

        // GUI-only look: hide our own console window -- but never a shell
        // we were launched from. GetConsoleProcessList() returns 1 only
        // when this process owns the console outright.
        HWND console = GetConsoleWindow();
        if (console) {
            DWORD owners[2] = {};
            if (GetConsoleProcessList(owners, 2) == 1) {
                ShowWindow(console, SW_HIDE);
            }
        }

        if (!cigi::initFsUIPC()) {
            cigi::guilog::uninstall();
            WSACleanup();
            return 1;
        }
        const int rc = cigi::gui::run();
        cigi::shutdownFsUIPC();
        cigi::guilog::uninstall();
        WSACleanup();
        return rc;
    }

    // ---- CLI modes (bin / live) -------------------------------------

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


