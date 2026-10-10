// net.cpp - UDP sockets: Winsock 2 on Windows, BSD sockets elsewhere
#include "net.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <mmsystem.h>
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    typedef int socklen_t;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <time.h>
    #include <unistd.h>
#endif

std::string NetAddress::ToString() const
{
    char buf[32];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return buf;
}

bool NetInit()
{
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    timeBeginPeriod(1);                          // 1 ms Sleep() resolution for the server loop
#endif
    return true;
}

void NetShutdown()
{
#if defined(_WIN32)
    timeEndPeriod(1);
    WSACleanup();
#endif
}

bool NetResolve(const std::string &host, uint16_t port, NetAddress &out)
{
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_INET;                 // IPv4 keeps addresses simple; fine for a test server
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    const sockaddr_in *sin = (const sockaddr_in *)res->ai_addr;
    out.ip   = ntohl(sin->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(res);
    return true;
}

std::string NetLocalAddress()
{
    // "Connect" a UDP socket toward a public address (nothing is sent) and ask
    // which local address the OS picked for that route
    std::string result = "127.0.0.1";
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return result;
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return result;
#endif
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);         // 8.8.8.8
    if (connect(s, (const sockaddr *)&to, sizeof to) == 0) {
        sockaddr_in me = {};
        socklen_t len = sizeof me;
        if (getsockname(s, (sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr != 0) {
            NetAddress a;
            a.ip = ntohl(me.sin_addr.s_addr);
            result = a.ToString();
            result.resize(result.rfind(':'));
        }
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return result;
}

bool UdpSocket::Open(uint16_t port)
{
    Close();
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Without this, an ICMP "port unreachable" (e.g. a client that just quit)
    // makes the NEXT recvfrom fail with WSAECONNRESET
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof reportReset, nullptr, 0, &bytes, nullptr, nullptr);
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int bufSize = 1 << 20;                       // room for bursts of packets between ticks
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&bufSize, sizeof bufSize);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&bufSize, sizeof bufSize);

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    handle = (intptr_t)s;
    if (bind(s, (const sockaddr *)&addr, sizeof addr) != 0) { Close(); return false; }
    return true;
}

void UdpSocket::Close()
{
    if (handle == INVALID) return;
#if defined(_WIN32)
    closesocket((SOCKET)handle);
#else
    close((int)handle);
#endif
    handle = INVALID;
}

bool UdpSocket::SendTo(const NetAddress &to, const void *data, int len)
{
    if (handle == INVALID) return false;
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port        = htons(to.port);
#if defined(_WIN32)
    int sent = sendto((SOCKET)handle, (const char *)data, len, 0, (const sockaddr *)&addr, sizeof addr);
#else
    int sent = (int)sendto((int)handle, data, (size_t)len, 0, (const sockaddr *)&addr, sizeof addr);
#endif
    return sent == len;
}

int UdpSocket::RecvFrom(NetAddress &from, void *buf, int cap)
{
    if (handle == INVALID) return -1;
    for (;;) {
        sockaddr_in addr = {};
        socklen_t   alen = sizeof addr;
#if defined(_WIN32)
        int n = recvfrom((SOCKET)handle, (char *)buf, cap, 0, (sockaddr *)&addr, &alen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 0;
            if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;      // a stale ICMP report / an oversized datagram
            return -1;
        }
#else
        int n = (int)recvfrom((int)handle, buf, (size_t)cap, 0, (sockaddr *)&addr, &alen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == ECONNREFUSED || errno == EINTR) continue;
            return -1;
        }
#endif
        from.ip   = ntohl(addr.sin_addr.s_addr);
        from.port = ntohs(addr.sin_port);
        return n;
    }
}

std::string UdpSocket::LastError() const
{
#if defined(_WIN32)
    return "Winsock error " + std::to_string(WSAGetLastError());
#else
    return strerror(errno);
#endif
}

static volatile bool *g_quitFlag = nullptr;

#if defined(_WIN32)
static BOOL WINAPI OnConsoleEvent(DWORD type)
{
    (void)type;
    if (g_quitFlag) *g_quitFlag = true;
    Sleep(3000);                                  // closing the window: give the main loop time to save
    return TRUE;
}
#else
static void OnSignal(int) { if (g_quitFlag) *g_quitFlag = true; }
#endif

void NetCatchQuit(volatile bool *flag)
{
    g_quitFlag = flag;
#if defined(_WIN32)
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
#else
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#endif
}

double NetTime()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void NetSleep(double seconds)
{
    if (seconds <= 0.0) return;
#if defined(_WIN32)
    Sleep((DWORD)(seconds * 1000.0));
#else
    timespec ts;
    ts.tv_sec  = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
#endif
}
