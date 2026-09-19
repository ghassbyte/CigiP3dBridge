#pragma once

// =====================================================================
//  GuiLog.h
// ---------------------------------------------------------------------
//  Console -> GUI output mirror.
//
//  The bridge logs through std::cout / std::cerr with console conventions:
//    - normal messages end in '\n'  (a new, scrolling line),
//    - the per-frame entity status ends in '\r' (a self-overwriting
//      status line -- "Entity0 -> lat=... yaw=..." at CIGI frame rate).
//
//  guilog::install() swaps both streams' streambuf for a tee that keeps
//  writing to the real console AND forwards to a GUI sink, preserving
//  those conventions:
//      '\n'  -> append a LOG line,
//      '\r'  -> update the single LIVE STATUS line.
//  That way the ~60 Hz status line never floods the log pane.
//
//  Output arrives on whichever thread printed (main thread + the pcap
//  capture thread), so delivery to the window is via PostMessage with a
//  heap std::string*; the UI thread owns and deletes it. Output emitted
//  before the window exists is buffered and flushed by attach().
//
//  This header deliberately exposes no Windows types: attach() takes an
//  opaque void* window handle so this module compiles without <windows.h>
//  in its interface.
// =====================================================================

namespace cigi {
namespace guilog {

// Window messages posted to the attached HWND. WM_APP == 0x8000.
//   kMsgLog:    LPARAM is a heap std::string* the UI thread must delete.
//   kMsgStatus: LPARAM is a heap std::string* the UI thread must delete.
constexpr unsigned kMsgLog    = 0x8000u + 2u;
constexpr unsigned kMsgStatus = 0x8000u + 3u;

// Start capturing std::cout / std::cerr into the GUI sink while still
// writing through to the console. Idempotent.
void install();

// Register the GUI window that should receive log/status messages and
// flush anything buffered so far. `hwnd` is an HWND passed as void*.
void attach(void* hwnd);

// Stop delivering to the window (it is going away). Buffered output is
// kept for a later attach().
void detach();

// Restore the original std::cout / std::cerr buffers and flush any
// partial line. Idempotent.
void uninstall();

} // namespace guilog
} // namespace cigi
