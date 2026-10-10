// protocol.h - what the client and server say to each other over UDP.
//
// Every datagram is one packet:
//
//   header      magic, protocol version, connection token, flags, reliable ack
//   reliable    zero or more messages that must arrive, in order (resent until acked)
//   unreliable  at most one payload that's only useful if it's fresh:
//               client -> server  the latest input steps (each sent a few times over)
//               server -> client  a snapshot of the world around you
//
// Reliable messages carry joins, commands, events, comms logs and mined-rock
// updates. They're resent every RESEND_INTERVAL until the other side's ack
// covers them, and delivered in order.
//
// The server is authoritative. Each client simulates its own ship ahead of the
// server (prediction) using exactly the inputs it sends, and the server steps
// that captain with the same inputs in the same order, so the two agree; when
// they don't (a rock mined by someone else, float differences between
// machines), the client snaps to the server's state and replays its newer
// inputs on top (reconciliation). Other ships are drawn a little in the past,
// smoothly interpolated between snapshots.
#pragma once
#include "commands.h"
#include "game.h"
#include "net.h"
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

const uint32_t PROTOCOL_MAGIC   = 0x31504D53;   // "SMP1"
const uint16_t PROTOCOL_VERSION = 9;            // bump when anything below changes (9: what markets want)
const uint16_t DEFAULT_PORT     = 23500;
const int      MAX_PACKET       = 1400;         // stays under a typical 1500-byte MTU
const int      MAX_MESSAGE      = 1100;         // biggest reliable message
const double   RESEND_INTERVAL  = 0.15;         // seconds before an unacked reliable message goes again
const double   TIMEOUT          = 10.0;         // seconds of silence before a connection is dropped
const int      SNAPSHOT_EVERY   = 4;            // server ticks per snapshot (120 Hz / 4 = 30 per second)
const int      MAX_PLAYER_NAME  = 20;

// ---------------------------------------------------------------------------
// Bytes in and out (little-endian)
// ---------------------------------------------------------------------------
struct Writer {
    std::vector<uint8_t> buf;
    void Bytes(const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; buf.insert(buf.end(), b, b + n); }
    void U8(uint8_t v)   { buf.push_back(v); }
    void U16(uint16_t v) { for (int i = 0; i < 2; i++) buf.push_back((uint8_t)(v >> (8 * i))); }
    void U32(uint32_t v) { for (int i = 0; i < 4; i++) buf.push_back((uint8_t)(v >> (8 * i))); }
    void U64(uint64_t v) { for (int i = 0; i < 8; i++) buf.push_back((uint8_t)(v >> (8 * i))); }
    void I16(int16_t v)  { U16((uint16_t)v); }
    void I32(int32_t v)  { U32((uint32_t)v); }
    void I64(int64_t v)  { U64((uint64_t)v); }
    void F32(float v)    { uint32_t u; memcpy(&u, &v, 4); U32(u); }
    void F64(double v)   { uint64_t u; memcpy(&u, &v, 8); U64(u); }
    void Str(const std::string &s, size_t max = 1000)
    {
        size_t n = s.size() < max ? s.size() : max;
        U16((uint16_t)n);
        Bytes(s.data(), n);
    }
    size_t Size() const { return buf.size(); }
};

// Reading past the end (a truncated or garbage packet) returns zeros and clears 'ok'
struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    Reader(const uint8_t *data, size_t n) : p(data), end(data + n) {}
    explicit Reader(const std::vector<uint8_t> &v) : p(v.data()), end(v.data() + v.size()) {}
    bool Need(size_t n) { if ((size_t)(end - p) < n) { ok = false; p = end; return false; } return true; }
    size_t Left() const { return (size_t)(end - p); }
    uint8_t  U8()  { if (!Need(1)) return 0; return *p++; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); p += 4; return v; }
    uint64_t U64() { if (!Need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); p += 8; return v; }
    int16_t  I16() { return (int16_t)U16(); }
    int32_t  I32() { return (int32_t)U32(); }
    int64_t  I64() { return (int64_t)U64(); }
    float    F32() { uint32_t u = U32(); float v; memcpy(&v, &u, 4); return v; }
    double   F64() { uint64_t u = U64(); double v; memcpy(&v, &u, 8); return v; }
    std::string Str() { uint16_t n = U16(); if (!Need(n)) return ""; std::string s((const char *)p, n); p += n; return s; }
    std::vector<uint8_t> Rest() { std::vector<uint8_t> v(p, end); p = end; return v; }
};

// ---------------------------------------------------------------------------
// Game data on the wire
// ---------------------------------------------------------------------------
void     WriteTarget(Writer &w, const TargetId &t);
TargetId ReadTarget(Reader &r);
void     WriteShip(Writer &w, const Ship &s);
Ship     ReadShip(Reader &r);
void     WritePlayer(Writer &w, const Player &p);
Player   ReadPlayer(Reader &r);

// One fixed step of input: exactly what the server needs to repeat the
// client's simulation of its own ship
struct InputStep {
    uint32_t seq = 0;          // 1, 2, 3, ... per connection
    Controls controls;
    Aim      aim;
};
void      WriteInput(Writer &w, const InputStep &in);
InputStep ReadInput(Reader &r, uint32_t seq);

// ---------------------------------------------------------------------------
// Reliable messages (first byte = type)
// ---------------------------------------------------------------------------
enum MsgType : uint8_t {
    // client -> server
    MSG_HELLO = 1,          // str name, str password, str ship name
    MSG_COMMAND,            // u8 type, i32 arg, f64 qty, u32 step
    MSG_BYE,                // leaving
    MSG_ADMIN,              // str key, u8 action (1 = stop the server): from the player who started it
    // server -> client
    MSG_WELCOME = 32,       // i32 id, str name, u32 seed, f64 clock, Player
    MSG_REJECT,             // str reason (then the server forgets you)
    MSG_MINED,              // u16 n, n x (u64 rock, f32 ore left)
    MSG_EVENT,              // u8 type, str text, target, i32 item, f64 amount, f64 value
    MSG_COMMS,              // one channel: target with, u8 open (0: it's closed), str withName, u8 n, n x (f64 sent, f64 at, u8 ours, str text)
    MSG_JOINED,             // i32 id, str name, u8 justNow (0: already here when you joined), str ship name
    MSG_LEFT,               // i32 id, str name
    MSG_STANDING,           // u8 n, n x f32: your standing with each faction, by id
    MSG_PRICES,             // one market as you now know it: see MsgPrices
    MSG_NEWS,               // u32 seq, f64 at, i32 faction, i64 where, str text
};

// Unreliable payload types (first byte after the reliable section)
enum PayloadType : uint8_t {
    PAYLOAD_NONE = 0,
    PAYLOAD_INPUTS,         // client -> server: u32 clientTimeMs, u32 firstSeq, u8 n, n x InputStep
    PAYLOAD_SNAPSHOT,       // server -> client: see BuildSnapshot / ReadSnapshot
};

const uint8_t ADMIN_STOP = 1;

const uint8_t PACKET_FLAG_DISCONNECT = 1;   // "I'm leaving" (sent a few times on exit)

std::vector<uint8_t> MsgCommand(const Command &c);
std::vector<uint8_t> MsgEvent(const GameEvent &e);
std::vector<uint8_t> MsgComms(const CommsChannel &c, bool open = true);   // the latest messages that fit
std::vector<uint8_t> MsgPrices(uint64_t body, const PriceRecord &r);
bool                 ReadPrices(Reader &r, uint64_t &body, PriceRecord &out);
std::vector<uint8_t> MsgNews(const EconNews &n);
std::vector<std::vector<uint8_t>> MsgMined(const MinedMap &m); // split into messages that fit

// ---------------------------------------------------------------------------
// Snapshots: the server's view, 30 times a second
// ---------------------------------------------------------------------------
const int SNAPSHOT_MAX_SHIPS = 14;          // nearest others per snapshot (keeps it in one packet)

struct Snapshot {
    uint32_t tick = 0;                      // server tick it was taken on
    double   clock = 0.0;                   // world clock
    uint32_t echoTimeMs = 0;                // the client's timestamp from its latest packet (for ping)
    uint32_t lastProcessed = 0;             // your last input step the server has simulated...
    uint32_t lastReceived = 0;              // ...and the last one it has received
    // Your captain after 'lastProcessed'
    Player   player;
    TargetId selected, lock;
    bool     lockSuppressed = false;
    Vector2  lockBreak = { 0, 0 };
    float    retargetCooldown = 0.0f;
    bool     miningActive = false;
    uint64_t miningRock = 0;
    std::string miningStatus;
    // Everyone else (no names: those come once, in MSG_JOINED)
    std::vector<Traffic> others;
};
void WriteSnapshot(Writer &w, const Snapshot &s);
bool ReadSnapshot(Reader &r, Snapshot &s);

// ---------------------------------------------------------------------------
// A connection: the reliable channel to one peer
// ---------------------------------------------------------------------------
struct PacketHeader {
    uint32_t token = 0;        // chosen by the client; tells reconnects from the same address apart
    uint8_t  flags = 0;
    uint32_t ack   = 0;        // highest reliable message received in order
};
// Checks magic and version; on success 'rest' is positioned after the header
bool ReadHeader(Reader &r, PacketHeader &h);

class Connection {
public:
    NetAddress addr;
    uint32_t   token = 0;
    double     lastHeard = 0.0;

    void Send(std::vector<uint8_t> msg);                       // queue a reliable message
    // Header + reliable messages that are due + the unreliable payload (dropped
    // if even it won't fit). Empty if there's nothing at all to send.
    std::vector<uint8_t> BuildPacket(const std::vector<uint8_t> &unreliable, double now, uint8_t flags = 0, bool force = false);
    // Everything after a header we've already read: the in-order reliable
    // messages that are now complete go into 'delivered'
    void Receive(const PacketHeader &h, Reader &r, std::vector<std::vector<uint8_t>> &delivered,
                 std::vector<uint8_t> &unreliable, double now);
    size_t Unacked() const { return outbox.size(); }

private:
    struct Pending { uint32_t seq; std::vector<uint8_t> data; double lastSent = -1.0; };
    uint32_t nextSend = 1;
    std::deque<Pending> outbox;
    uint32_t nextRecv = 1;
    std::map<uint32_t, std::vector<uint8_t>> early;            // arrived ahead of a missing one
};
