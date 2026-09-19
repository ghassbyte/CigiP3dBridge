#pragma once

// =====================================================================
//  PcapCapture.h
// ---------------------------------------------------------------------
//  Mode 2 of the bridge: passively tap a NIC with Npcap, pull out
//  every UDP datagram from HOST (192.168.70.2) to dst port 8000, and
//  feed each one through processDatagram().
//
//  Why Npcap and not a bound UDP socket: a normal bind()+recvfrom()
//  socket only ever sees packets actually addressed to this machine.
//  When the bridge sits on a SPAN port / TAP that's mirroring HOST<-
//  >IG traffic that is NOT addressed to us, those packets never reach
//  any socket -- the kernel drops them before delivery. Npcap puts the
//  NIC into promiscuous mode (the same mechanism Wireshark uses), so
//  we see every frame the wire carries.
//
//  Layer walk inside each captured frame:
//      [Ethernet][(optional 802.1Q VLAN tag)][IPv4][UDP][CIGI payload]
//  pcap's BPF filter restricts WHICH packets reach the callback
//  ("udp and src host HOST and dst port DATA_PORT"); the in-software
//  walk then finds the actual payload offset, since BPF doesn't tell
//  us where the UDP payload starts (IP header length varies; VLAN
//  tag may or may not be present).
// =====================================================================

#include <string>

namespace cigi { class RepositionController; }

namespace pcapcap {

// Prints every Npcap-capable device as "[index] \\Device\\NPF_{...}
//  - Friendly description" and returns. Use the index or full name as
//  `device` in run().
void listCaptureDevices();

// Captures forever (or until pcap_loop returns, which today only
// happens on a fatal error -- Ctrl-C is the normal exit). The given
// device may be either a full Npcap name ("\\Device\\NPF_{GUID}") or
// the numeric index printed by listCaptureDevices(). `reposition`
// is wired in but the live mode never injects anything into it -- if
// you want to reposition while in live mode, switch to bin mode.
void run(const std::string& device,
         const std::string& hostIp,
         unsigned short    dataPort,
         cigi::RepositionController* reposition);

} // namespace pcapcap

