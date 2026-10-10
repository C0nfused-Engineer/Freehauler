// net.h - UDP sockets, addresses and a clock.
//
// On Windows this is Winsock 2 (link with -lws2_32 -lwinmm); elsewhere it's
// BSD sockets, so the dedicated server also builds on a Linux box or VPS.
//
// Deliberately free of raylib: <winsock2.h> drags in <windows.h>, whose
// names (Rectangle, CloseWindow, DrawText, ShowCursor...) collide with
// raylib's. Only net.cpp includes the Windows headers.
#pragma once
#include <cstdint>
#include <string>

struct NetAddress {
    uint32_t ip   = 0;        // IPv4, host byte order
    uint16_t port = 0;        // host byte order

    bool operator==(const NetAddress &o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const NetAddress &o) const { return !(*this == o); }
    std::string ToString() const;  // "203.0.113.7:23500"
};

bool NetInit();               // WSAStartup on Windows; call once before anything else
void NetShutdown();
// "myserver.example.com" or "203.0.113.7" -> address; false if it can't be resolved
bool NetResolve(const std::string &host, uint16_t port, NetAddress &out);
// This machine's address on the local network (what LAN players type to join), e.g. "192.168.1.20"
std::string NetLocalAddress();

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { Close(); }
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    bool Open(uint16_t port = 0);    // bind to a port (0 = any free one); non-blocking
    void Close();
    bool IsOpen() const { return handle != INVALID; }
    bool SendTo(const NetAddress &to, const void *data, int len);
    // One datagram: its size, 0 if none is waiting, -1 on a real error
    int  RecvFrom(NetAddress &from, void *buf, int cap);
    std::string LastError() const;

private:
    static const intptr_t INVALID = -1;
    intptr_t handle = INVALID;
};

// Ctrl+C (and closing the console window on Windows) sets *flag instead of killing the
// process, so the server can save before it exits
void NetCatchQuit(volatile bool *flag);

double NetTime();               // seconds since the program started (monotonic)
void   NetSleep(double seconds);   // ~1 ms resolution
