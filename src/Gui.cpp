// =====================================================================
//  Gui.cpp
// ---------------------------------------------------------------------
//  Implementation of cigi::gui::run(). A plain Win32 (no resource file)
//  window. See Gui.h for the "why".
//
//  Include order note: this file only needs <windows.h> -- it never
//  touches sockets or pcap directly (NIC enumeration goes through
//  pcapcap::listDevices), so there is no winsock2.h/windows.h ordering
//  trap to worry about here. Keep it that way: don't add <pcap.h> or a
//  winsock header to this file.
//
//  We use the explicit ANSI ("A") Win32 API throughout so NIC names /
//  descriptions (narrow std::string, matching the rest of the codebase)
//  flow straight into the controls without wide-string conversion.
//
//  Layout (top -> bottom): NIC label, NIC listbox, "Selected NIC" read-
//  only path field, Refresh/START/STOP/EXIT buttons, "Live status" field
//  (the self-overwriting per-frame line), scrolling log pane, status
//  label. Sizes below are in 96-DPI design pixels and scaled by S().
//
//  The window is deliberately compact (640x494 client). The fonts are
//  NOT shrunk to achieve that -- readability is preserved by reclaiming
//  whitespace instead, so the operator keeps the 12pt/11pt text they had
//  while the mostly-empty NIC list and log pane give back their slack.
//  Every rect is derived from the k* layout constants below, so retuning
//  the size is a single-point change.
// =====================================================================

#include "Gui.h"
#include "GuiLog.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include "PcapCapture.h"

namespace cigi {
namespace gui {

namespace {

// Control IDs (unique within the window's child set).
constexpr int kIdListNics   = 1001;
constexpr int kIdBtnRefresh = 1002;
constexpr int kIdBtnStart   = 1003;
constexpr int kIdBtnStop    = 1004;
constexpr int kIdBtnExit    = 1005;
constexpr int kIdSelected   = 1006;
constexpr int kIdLiveStatus = 1007;
constexpr int kIdLog        = 1008;
constexpr int kIdStatusMsg  = 1009;

// Posted from the capture thread back to the UI thread when the capture
// loop exits, so the status label can be refreshed on the UI thread.
constexpr UINT kMsgCaptureEnded = WM_APP + 1;

// Live-mode filter defaults -- match the CLI (--host-ip / --data-port)
// and the documented network setup on the IG PC.
constexpr const char* kDefaultHostIp = "192.168.70.2";
constexpr unsigned short kDefaultPort = 8000;

// Keep the scrolling log bounded.
constexpr std::size_t kMaxLogLines = 2000;

// ---------------------------------------------------------------------
// Layout. All values are 96-DPI design pixels; every rect below is run
// through S() so higher-DPI displays scale the whole window instead of
// clipping it. The window is intentionally small -- re-run the smoke
// build (see AGENTS.md) and eyeball the result after changing any of
// these.
// ---------------------------------------------------------------------
constexpr int kMargin   = 12;   // outer padding on every side
constexpr int kGap      = 4;    // vertical gap between consecutive rows
constexpr int kContentW = 616;  // usable width inside the margins
constexpr int kLabelH   = 18;   // plain static label
constexpr int kFieldH   = 22;   // read-only edit field (and its row)
constexpr int kLabelW   = 96;   // leading label on a label+field row --
                                // sized for the widest one, "Selected NIC:",
                                // so it isn't clipped (STATICs don't ellipsize)
constexpr int kBtnW     = 88;
constexpr int kBtnH     = 26;
constexpr int kListH    = 150;  // NIC listbox, ~8 rows at 12pt
constexpr int kLogH     = 190;  // scrolling log pane

// Client size, computed from the row heights above so the two can't drift
// apart. kClientH sums: margin, then each row plus its trailing gap, then
// the closing margin.
constexpr int kClientW = kContentW + 2 * kMargin;
constexpr int kClientH = kMargin + kLabelH + kGap   // "Select NIC..." label
                       + kListH  + kGap             // NIC listbox
                       + kFieldH + kGap             // "Selected NIC" field
                       + kBtnH   + kGap             // Refresh/START/STOP/EXIT
                       + kFieldH + kGap             // "Live status" field
                       + kLogH   + kGap             // scrolling log pane
                       + kLabelH + kMargin;         // status label

HWND      g_hwnd      = nullptr;
HINSTANCE g_hInstance = nullptr;
int       g_dpi       = 96;
HFONT     g_uiFont    = nullptr;   // controls
HFONT     g_logFont   = nullptr;   // log pane (monospace)

std::thread       g_captureThread;
std::atomic<bool> g_capturing{false};

// The enumerated NICs, refreshed on startup and on the Refresh button.
std::vector<pcapcap::DeviceInfo> g_devices;

// Mirror of the log pane contents (bounded), so trimming can rebuild it.
std::deque<std::string> g_logLines;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Scale a 96-DPI design pixel to the current DPI.
int S(int v) { return MulDiv(v, g_dpi, 96); }

// Pack a control ID into the HMENU slot CreateWindow wants.
HMENU idm(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

HFONT makeFont(int pt, const char* face, bool bold = false) {
    return CreateFontA(-MulDiv(pt, g_dpi, 72), 0, 0, 0,
                       bold ? FW_BOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void setUiFont(HWND ctrl) {
    if (ctrl && g_uiFont) SendMessageA(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
}

void setLogFont(HWND ctrl) {
    if (ctrl && g_logFont) SendMessageA(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(g_logFont), TRUE);
}

// Listbox rows show the friendly name; fall back to the device path when
// the adapter has no description.
std::string displayName(const pcapcap::DeviceInfo& d) {
    return d.description.empty() ? d.name : d.description;
}

void updateSelectedNic(HWND hwnd) {
    HWND list = GetDlgItem(hwnd, kIdListNics);
    const int sel = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
    std::string text;
    if (sel != LB_ERR && sel >= 0 && sel < static_cast<int>(g_devices.size())) {
        text = g_devices[static_cast<std::size_t>(sel)].name;
    }
    SetWindowTextA(GetDlgItem(hwnd, kIdSelected), text.c_str());
}

// Give the listbox a horizontal scroll range wide enough for its longest
// row, so nothing is unreadable.
void updateListExtent(HWND hwnd) {
    HWND list = GetDlgItem(hwnd, kIdListNics);
    if (!list) return;
    HDC dc = GetDC(list);
    if (!dc) return;
    HGDIOBJ old = g_uiFont ? SelectObject(dc, g_uiFont) : nullptr;
    int maxW = 0;
    for (const auto& d : g_devices) {
        const std::string line = displayName(d);
        SIZE sz = {};
        if (GetTextExtentPoint32A(dc, line.c_str(), static_cast<int>(line.size()), &sz)
            && sz.cx > maxW) {
            maxW = sz.cx;
        }
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(list, dc);
    SendMessageA(list, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(maxW + S(24)), 0);
}

void refreshListbox(HWND hwnd) {
    HWND list = GetDlgItem(hwnd, kIdListNics);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    for (const auto& d : g_devices) {
        const std::string line = displayName(d);
        SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    if (!g_devices.empty()) {
        SendMessageA(list, LB_SETCURSEL, 0, 0);
    }
    updateListExtent(hwnd);
    updateSelectedNic(hwnd);
}

void scanDevices() {
    g_devices.clear();
    pcapcap::listDevices(g_devices);
}

// Append one line to the scrolling log pane, trimming the oldest lines
// when the cap is exceeded.
void appendLog(HWND hwnd, const std::string& line) {
    g_logLines.push_back(line);
    bool trimmed = false;
    while (g_logLines.size() > kMaxLogLines) {
        g_logLines.pop_front();
        trimmed = true;
    }

    HWND log = GetDlgItem(hwnd, kIdLog);
    if (!log) return;

    if (trimmed) {
        std::string all;
        for (const auto& l : g_logLines) { all += l; all += "\r\n"; }
        SetWindowTextA(log, all.c_str());
    } else {
        const int len = static_cast<int>(SendMessageA(log, WM_GETTEXTLENGTH, 0, 0));
        SendMessageA(log, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        std::string text = line;
        text += "\r\n";
        SendMessageA(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    }
    const int end = static_cast<int>(SendMessageA(log, WM_GETTEXTLENGTH, 0, 0));
    SendMessageA(log, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    SendMessageA(log, EM_SCROLLCARET, 0, 0);
}

// Stop any running capture and reap its thread. Safe to call even when
// nothing is capturing (no-op).
void stopCapture() {
    if (g_capturing.load()) {
        pcapcap::requestStop();
    }
    if (g_captureThread.joinable()) {
        g_captureThread.join();
    }
}

// Runs on the capture thread. pcapcap::run() blocks until the STOP/EXIT
// path raises requestStop() (or a fatal error), then returns.
void captureThreadFunc(std::string device) {
    pcapcap::run(device, kDefaultHostIp, kDefaultPort, nullptr);
    g_capturing = false;
    if (g_hwnd) {
        PostMessageA(g_hwnd, kMsgCaptureEnded, 0, 0);
    }
}

void createControls(HWND hwnd) {
    HWND c;

    // Left edge of the content column, and the indent/width of the read-only
    // fields that sit next to their labels ("Selected NIC" / "Live status").
    const int x      = kMargin;
    const int fieldX = kMargin + kLabelW + kGap;
    const int fieldW = kContentW - kLabelW - kGap;

    int y = kMargin;

    c = CreateWindowA("STATIC", "Select network interface (NIC) for live capture:",
                      WS_CHILD | WS_VISIBLE, S(x), S(y), S(kContentW), S(kLabelH),
                      hwnd, nullptr, g_hInstance, nullptr);
    setUiFont(c);
    y += kLabelH + kGap;

    c = CreateWindowA("LISTBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                          LBS_NOTIFY | WS_BORDER,
                      S(x), S(y), S(kContentW), S(kListH),
                      hwnd, idm(kIdListNics), g_hInstance, nullptr);
    setUiFont(c);
    y += kListH + kGap;

    c = CreateWindowA("STATIC", "Selected NIC:",
                      WS_CHILD | WS_VISIBLE, S(x), S(y + 2), S(kLabelW), S(kLabelH),
                      hwnd, nullptr, g_hInstance, nullptr);
    setUiFont(c);
    c = CreateWindowA("EDIT", "",
                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
                      S(fieldX), S(y), S(fieldW), S(kFieldH),
                      hwnd, idm(kIdSelected), g_hInstance, nullptr);
    setUiFont(c);
    y += kFieldH + kGap;

    c = CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      S(x), S(y), S(kBtnW), S(kBtnH),
                      hwnd, idm(kIdBtnRefresh), g_hInstance, nullptr);
    setUiFont(c);
    c = CreateWindowA("BUTTON", "START", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      S(x + kBtnW + kGap), S(y), S(kBtnW), S(kBtnH),
                      hwnd, idm(kIdBtnStart), g_hInstance, nullptr);
    setUiFont(c);
    c = CreateWindowA("BUTTON", "STOP", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      S(x + 2 * (kBtnW + kGap)), S(y), S(kBtnW), S(kBtnH),
                      hwnd, idm(kIdBtnStop), g_hInstance, nullptr);
    setUiFont(c);
    c = CreateWindowA("BUTTON", "EXIT", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      S(x + kContentW - kBtnW), S(y), S(kBtnW), S(kBtnH),
                      hwnd, idm(kIdBtnExit), g_hInstance, nullptr);
    setUiFont(c);
    y += kBtnH + kGap;

    c = CreateWindowA("STATIC", "Live status:",
                      WS_CHILD | WS_VISIBLE, S(x), S(y + 2), S(kLabelW), S(kLabelH),
                      hwnd, nullptr, g_hInstance, nullptr);
    setUiFont(c);
    c = CreateWindowA("EDIT", "",
                      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
                      S(fieldX), S(y), S(fieldW), S(kFieldH),
                      hwnd, idm(kIdLiveStatus), g_hInstance, nullptr);
    setUiFont(c);
    y += kFieldH + kGap;

    c = CreateWindowA("EDIT", "",
                      WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                          ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT,
                      S(x), S(y), S(kContentW), S(kLogH),
                      hwnd, idm(kIdLog), g_hInstance, nullptr);
    setLogFont(c);
    y += kLogH + kGap;

    c = CreateWindowA("STATIC", "Status: idle", WS_CHILD | WS_VISIBLE,
                      S(x), S(y), S(kContentW), S(kLabelH),
                      hwnd, idm(kIdStatusMsg), g_hInstance, nullptr);
    setUiFont(c);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_uiFont  = makeFont(12, "Segoe UI");
        g_logFont = makeFont(11, "Consolas");
        createControls(hwnd);
        scanDevices();
        refreshListbox(hwnd);

        // Start receiving mirrored console output (flushes the backlog).
        guilog::attach(hwnd);
        return 0;
    }

    case WM_COMMAND: {
        const int id   = LOWORD(wParam);
        const int code = HIWORD(wParam);

        if (id == kIdListNics && code == LBN_SELCHANGE) {
            updateSelectedNic(hwnd);
            return 0;
        }

        switch (id) {
        case kIdBtnRefresh:
            scanDevices();
            refreshListbox(hwnd);
            return 0;

        case kIdBtnStart: {
            HWND list = GetDlgItem(hwnd, kIdListNics);
            const int sel = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
            if (sel == LB_ERR || sel < 0 || sel >= static_cast<int>(g_devices.size())) {
                SetWindowTextA(GetDlgItem(hwnd, kIdStatusMsg), "Status: no NIC selected");
                return 0;
            }
            stopCapture();  // tear down any running capture first
            const std::string device = g_devices[static_cast<std::size_t>(sel)].name;
            const std::string status = "Status: capturing on " + device;
            SetWindowTextA(GetDlgItem(hwnd, kIdStatusMsg), status.c_str());
            g_capturing = true;
            g_captureThread = std::thread(captureThreadFunc, device);
            return 0;
        }

        case kIdBtnStop:
            stopCapture();
            SetWindowTextA(GetDlgItem(hwnd, kIdStatusMsg), "Status: stopped");
            return 0;

        case kIdBtnExit:
            stopCapture();
            DestroyWindow(hwnd);  // -> WM_DESTROY -> PostQuitMessage
            return 0;
        }
        return 0;
    }

    case guilog::kMsgLog: {
        std::string* s = reinterpret_cast<std::string*>(lParam);
        if (s) {
            appendLog(hwnd, *s);
            delete s;
        }
        return 0;
    }

    case guilog::kMsgStatus: {
        std::string* s = reinterpret_cast<std::string*>(lParam);
        if (s) {
            SetWindowTextA(GetDlgItem(hwnd, kIdLiveStatus), s->c_str());
            delete s;
        }
        return 0;
    }

    case kMsgCaptureEnded:
        g_capturing = false;
        SetWindowTextA(GetDlgItem(hwnd, kIdStatusMsg), "Status: capture ended");
        return 0;

    case WM_SYSCOMMAND:
        // Swallow SC_CLOSE (the X button / Alt+F4) -- see Gui.h.
        if ((wParam & 0xFFF0) == SC_CLOSE) return 0;
        return DefWindowProcA(hwnd, msg, wParam, lParam);

    case WM_CLOSE:
        // Deliberately swallowed: the window may only be closed via EXIT.
        return 0;

    case WM_DESTROY:
        guilog::detach();
        if (g_uiFont)  { DeleteObject(g_uiFont);  g_uiFont  = nullptr; }
        if (g_logFont) { DeleteObject(g_logFont); g_logFont = nullptr; }
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

} // anonymous namespace

int run() {
    // Opt into real DPI so fonts/layout aren't bitmap-scaled. Loaded
    // dynamically so we don't depend on a particular _WIN32_WINNT level.
    {
        typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            SetProcessDPIAwareFn fn = reinterpret_cast<SetProcessDPIAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware"));
            if (fn) fn();
        }
    }

    {
        HDC screen = GetDC(nullptr);
        g_dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
        if (screen) ReleaseDC(nullptr, screen);
        if (g_dpi <= 0) g_dpi = 96;
    }

    g_hInstance = GetModuleHandleA(nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInstance;
    wc.lpszClassName = "CigiP3dBridgeGui";
    wc.hCursor       = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512)); // IDC_ARROW
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassA(&wc)) return 1;

    // WS_SYSMENU keeps the caption + X button (so the operator can see
    // they're not supposed to use it); WM_CLOSE/SC_CLOSE are ignored.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, S(kClientW), S(kClientH) };
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowA("CigiP3dBridgeGui", "CigiP3dBridge", style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, g_hInstance, nullptr);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    stopCapture();  // safety net (normally already stopped via EXIT)
    return 0;
}

} // namespace gui
} // namespace cigi
