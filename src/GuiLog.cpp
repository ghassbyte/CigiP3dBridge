// =====================================================================
//  GuiLog.cpp
// ---------------------------------------------------------------------
//  Implementation of the console -> GUI mirror declared in GuiLog.h.
//
//  Only <windows.h> is needed here (for PostMessageA) -- no winsock, no
//  pcap -- so there is no header-ordering trap. Keep it that way.
// =====================================================================

#include "GuiLog.h"

#include <windows.h>

#include <cstddef>
#include <deque>
#include <iostream>
#include <mutex>
#include <new>
#include <streambuf>
#include <string>

namespace cigi {
namespace guilog {

namespace {

// Cap the pre-attach backlog so a startup that never opens a window (or a
// very chatty one) can't grow without bound.
constexpr std::size_t kMaxBacklog   = 2000;
// Guard against a runaway line (e.g. a '\r'-less stream) eating memory.
constexpr std::size_t kMaxLineChars = 1u << 16;

std::mutex g_mutex;

// Target window (nullptr until attach()).
void* g_hwnd = nullptr;

// The partial line currently being accumulated.
std::string g_line;

// Undelivered log lines (window not attached yet) + latest status line.
// Status is kept as a single "latest" value: the per-frame line must not
// accumulate.
std::deque<std::string> g_backlog;
std::string g_lastStatus;
bool        g_hasStatus = false;

// Saved original buffers and the tee objects (owned, freed by uninstall).
std::streambuf* g_coutBuf  = nullptr;
std::streambuf* g_cerrBuf  = nullptr;
std::streambuf* g_coutTee  = nullptr;
std::streambuf* g_cerrTee  = nullptr;
bool            g_installed = false;

// Post the line to the window, or buffer it. Must hold g_mutex.
void deliverLocked(unsigned msg, const std::string& line, bool isStatus) {
    if (g_hwnd) {
        std::string* copy = new (std::nothrow) std::string(line);
        if (!copy) return;
        if (!PostMessageA(static_cast<HWND>(g_hwnd), msg, 0,
                          reinterpret_cast<LPARAM>(copy))) {
            delete copy;  // window disappeared; drop the line
        }
    } else if (isStatus) {
        g_lastStatus = line;
        g_hasStatus  = true;
    } else {
        g_backlog.push_back(line);
        while (g_backlog.size() > kMaxBacklog) g_backlog.pop_front();
    }
}

// The tee streambuf. Writes every byte through to the original console
// buffer, then feeds it to the GUI sink with the '\n'/'\r' convention.
class TeeBuf : public std::streambuf {
public:
    explicit TeeBuf(std::streambuf* original) : m_original(original) {}

protected:
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::not_eof(ch);
        const char c = static_cast<char>(ch);
        if (m_original) m_original->sputc(c);
        feed(c);
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (m_original && n > 0) m_original->sputn(s, n);
        for (std::streamsize i = 0; i < n; ++i) feed(s[i]);
        return n;
    }

    int sync() override {
        if (m_original) m_original->pubsync();
        return 0;
    }

private:
    static void feed(char c) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (c != '\n' && c != '\r') {
            if (g_line.size() < kMaxLineChars) g_line.push_back(c);
            return;
        }
        std::string line;
        line.swap(g_line);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // \r\n
        if (line.empty()) return;  // skip blank separator lines
        if (c == '\n') deliverLocked(kMsgLog,    line, false);
        else           deliverLocked(kMsgStatus, line, true);
    }

    std::streambuf* m_original;
};

} // anonymous namespace

void install() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_installed) return;
    g_coutBuf = std::cout.rdbuf();
    g_cerrBuf = std::cerr.rdbuf();
    g_coutTee = new TeeBuf(g_coutBuf);
    g_cerrTee = new TeeBuf(g_cerrBuf);
    std::cout.rdbuf(g_coutTee);
    std::cerr.rdbuf(g_cerrTee);
    g_installed = true;
}

void attach(void* hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hwnd = hwnd;
    for (const auto& line : g_backlog) deliverLocked(kMsgLog, line, false);
    g_backlog.clear();
    if (g_hasStatus) {
        deliverLocked(kMsgStatus, g_lastStatus, true);
        g_hasStatus = false;
    }
}

void detach() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hwnd = nullptr;
}

void uninstall() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_installed) return;
    if (!g_line.empty()) {
        deliverLocked(kMsgLog, g_line, false);
        g_line.clear();
    }
    std::cout.rdbuf(g_coutBuf);
    std::cerr.rdbuf(g_cerrBuf);
    delete g_coutTee; g_coutTee = nullptr;
    delete g_cerrTee; g_cerrTee = nullptr;
    g_coutBuf = nullptr;
    g_cerrBuf = nullptr;
    g_installed = false;
}

} // namespace guilog
} // namespace cigi
