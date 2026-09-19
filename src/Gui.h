#pragma once

// =====================================================================
//  Gui.h
// ---------------------------------------------------------------------
//  The "simple GUI" layer of the bridge. A minimal Win32 window that:
//    - lists every Npcap-capturable NIC (via pcapcap::listDevices),
//      showing the friendly name first and the full device path for the
//      selected row,
//    - lets the operator pick one and START a live capture on it,
//    - can STOP the capture,
//    - mirrors std::cout / std::cerr (see GuiLog.h) into a scrolling log
//      pane plus a single live status line, and
//    - quits ONLY through the EXIT button -- the window's X button /
//      Alt+F4 are deliberately swallowed (WM_CLOSE and SC_CLOSE are
//      ignored), so the bridge can't be torn down by accident mid-run.
//
//  This sits on top of the same main() as the CLI: main() installs the
//  GuiLog tee and calls cigi::initFsUIPC() first, then calls
//  cigi::gui::run() here. The capture runs on a background thread (same
//  as live CLI mode); FSUIPC writes stay thread-safe via g_fsuipcMutex.
//
//  Deliberately plain Win32 -- no resource file, no MFC/ATL/Qt -- so it
//  builds with nothing beyond user32/gdi32 that the v140 toolset already
//  links. This file keeps no pcap/winsock dependency; NIC enumeration is
//  delegated to pcapcap::listDevices().
// =====================================================================

namespace cigi {
namespace gui {

// Blocks in a Win32 message loop until the operator clicks EXIT.
// Returns the process exit code (0 = clean exit).
int run();

} // namespace gui
} // namespace cigi
