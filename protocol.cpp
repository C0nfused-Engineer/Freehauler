// protocol.cpp - encoding game data, and the reliable channel
#include "protocol.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Game data
// ---------------------------------------------------------------------------
void WriteTarget(Writer &w, const TargetId &t)
{
    w.U8(t.type);
    if (t.type == TargetId::None) return;
    w.I64(t.system);
    w.I32(t.index);
    if (t.type == TargetId::Asteroid) w.U64(t.rock);
}

TargetId ReadTarget(Reader &r)
{
    TargetId t;
    uint8_t type = r.U8();
    if (type == TargetId::None || type > TargetId::Ship) return t;
    t.type   = (TargetId::Type)type;
    t.system = r.I64();
    t.index  = r.I32();
    if (t.type == TargetId::Asteroid) t.rock = r.U64();
    return t;
}

void WriteShip(Writer &w, const Ship &s)
{
    w.I64(s.anchor);
    w.F64(s.pos.x); w.F64(s.pos.y); w.F64(s.pos.z);
    w.F32(s.vel.x); w.F32(s.vel.y); w.F32(s.vel.z);
    w.F32(s.rot.x); w.F32(s.rot.y); w.F32(s.rot.z); w.F32(s.rot.w);
    w.U8((s.dampeners ? 1 : 0) | (s.allStop ? 2 : 0));
    w.F32(s.thrustUse);
    w.F32(s.throttle);
}

Ship ReadShip(Reader &r)
{
    Ship s;
    s.anchor = r.I64();
    s.pos.x = r.F64(); s.pos.y = r.F64(); s.pos.z = r.F64();
    s.vel.x = r.F32(); s.vel.y = r.F32(); s.vel.z = r.F32();
    s.rot.x = r.F32(); s.rot.y = r.F32(); s.rot.z = r.F32(); s.rot.w = r.F32();
    uint8_t f = r.U8();
    s.dampeners = (f & 1) != 0;
    s.allStop   = (f & 2) != 0;
    s.thrustUse = r.F32();
    s.throttle  = r.F32();
    return s;
}

void WritePlayer(Writer &w, const Player &p)
{
    WriteShip(w, p.ship);
    w.F64(p.fuel);
    w.F64(p.credits);
    uint8_t n = 0;                                 // only what's in the hold: index, tonnes
    for (int g = 0; g < GOOD_COUNT; g++) if (p.cargo[g] > 0.0) n++;
    w.U8(n);
    for (int g = 0; g < GOOD_COUNT; g++) if (p.cargo[g] > 0.0) { w.U8((uint8_t)g); w.F64(p.cargo[g]); }
}

Player ReadPlayer(Reader &r)
{
    Player p;
    p.ship    = ReadShip(r);
    p.fuel    = r.F64();
    p.credits = r.F64();
    uint8_t n = r.U8();
    for (int i = 0; i < n && r.ok; i++) {
        uint8_t g = r.U8();
        double t = r.F64();
        if (g < GOOD_COUNT && std::isfinite(t)) p.cargo[g] = t;
    }
    return p;
}

void WriteInput(Writer &w, const InputStep &in)
{
    const Controls &c = in.controls;
    w.F32(c.throttle);
    w.F32(c.yaw); w.F32(c.pitch); w.F32(c.roll);
    w.U8((c.hyper ? 1 : 0) | (c.fire ? 2 : 0) | (c.allStop ? 4 : 0) | (c.toggleDampeners ? 8 : 0) | (in.aim.steer ? 16 : 0));
    WriteTarget(w, in.aim.hovered);
    WriteTarget(w, in.aim.retarget);
    WriteTarget(w, in.aim.course);
}

// Inputs come from the network, so they're clamped to what a real client can send
static float Sane(float v, float lim) { return std::isfinite(v) ? fmaxf(-lim, fminf(lim, v)) : 0.0f; }

InputStep ReadInput(Reader &r, uint32_t seq)
{
    InputStep in;
    in.seq = seq;
    Controls &c = in.controls;
    c.throttle = Sane(r.F32(), 1.0f);
    c.yaw     = Sane(r.F32(), 90.0f);
    c.pitch   = Sane(r.F32(), 90.0f);
    c.roll    = Sane(r.F32(), 90.0f);
    uint8_t f = r.U8();
    c.hyper = (f & 1) != 0; c.fire = (f & 2) != 0; c.allStop = (f & 4) != 0; c.toggleDampeners = (f & 8) != 0;
    in.aim.steer    = (f & 16) != 0;
    in.aim.hovered  = ReadTarget(r);
    in.aim.retarget = ReadTarget(r);
    in.aim.course   = ReadTarget(r);
    return in;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
std::vector<uint8_t> MsgCommand(const Command &c)
{
    Writer w;
    w.U8(MSG_COMMAND);
    w.U8(c.type);
    w.I32(c.arg);
    w.F64(c.qty);
    w.U32(c.step);
    return w.buf;
}

std::vector<uint8_t> MsgEvent(const GameEvent &e)
{
    Writer w;
    w.U8(MSG_EVENT);
    w.U8((uint8_t)e.type);
    w.Str(e.text, 400);
    WriteTarget(w, e.where);
    w.I32(e.item);
    w.F64(e.amount);
    w.F64(e.value);
    return w.buf;
}

std::vector<uint8_t> MsgComms(const CommsChannel &c, bool open)
{
    // Newest messages first, as many as fit (the window only shows the last dozen lines anyway)
    size_t budget = MAX_MESSAGE - 64 - c.withName.size();
    size_t first = c.log.size();
    while (open && first > 0) {
        size_t need = 16 + 1 + 2 + std::min<size_t>(c.log[first - 1].text.size(), 400);
        if (need > budget || c.log.size() - first >= 255) break;
        budget -= need;
        first--;
    }
    Writer w;
    w.U8(MSG_COMMS);
    WriteTarget(w, c.with);
    w.U8(open ? 1 : 0);
    w.Str(c.withName, 64);
    w.U8((uint8_t)(c.log.size() - first));
    for (size_t i = first; i < c.log.size(); i++) {
        w.F64(c.log[i].sent);
        w.F64(c.log[i].at);
        w.U8(c.log[i].ours ? 1 : 0);
        w.Str(c.log[i].text, 400);
    }
    return w.buf;
}

std::vector<uint8_t> MsgPrices(uint64_t body, const PriceRecord &r)
{
    Writer w;
    w.U8(MSG_PRICES);
    w.U64(body); w.F64(r.at); w.I64(r.sys); w.Str(r.name, 80);
    w.I32(r.owner); w.I32(r.ruler); w.F32(r.tariff);
    for (int g = 0; g < GOOD_COUNT; g++) {
        w.U8(r.role[g]);
        if (r.role[g] & 127) { w.F32(r.price[g]); w.F32(r.stock[g]); w.F32(r.target[g]); }      // (only what it deals in)
    }
    return w.buf;
}

bool ReadPrices(Reader &r, uint64_t &body, PriceRecord &out)
{
    body = r.U64(); out.at = r.F64(); out.sys = r.I64(); out.name = r.Str();
    out.owner = r.I32(); out.ruler = r.I32(); out.tariff = r.F32();
    for (int g = 0; g < GOOD_COUNT; g++) {
        out.role[g] = r.U8();
        if (out.role[g] & 127) { out.price[g] = r.F32(); out.stock[g] = r.F32(); out.target[g] = r.F32(); }
    }
    return r.ok;
}

std::vector<uint8_t> MsgNews(const EconNews &n)
{
    Writer w;
    w.U8(MSG_NEWS);
    w.U32(n.seq); w.F64(n.at); w.I32(n.faction); w.I64(n.where); w.Str(n.text, 300);
    return w.buf;
}

std::vector<std::vector<uint8_t>> MsgMined(const MinedMap &m)
{
    const int PER_MESSAGE = 80;                    // 12 bytes each
    std::vector<std::vector<uint8_t>> out;
    auto it = m.begin();
    while (it != m.end()) {
        Writer w;
        w.U8(MSG_MINED);
        size_t countAt = w.Size();
        w.U16(0);
        uint16_t n = 0;
        for (; it != m.end() && n < PER_MESSAGE; ++it, ++n) { w.U64(it->first); w.F32(it->second); }
        w.buf[countAt] = (uint8_t)(n & 255); w.buf[countAt + 1] = (uint8_t)(n >> 8);
        out.push_back(std::move(w.buf));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
void WriteSnapshot(Writer &w, const Snapshot &s)
{
    w.U8(PAYLOAD_SNAPSHOT);
    w.U32(s.tick);
    w.F64(s.clock);
    w.U32(s.echoTimeMs);
    w.U32(s.lastProcessed);
    w.U32(s.lastReceived);
    WritePlayer(w, s.player);
    WriteTarget(w, s.selected);
    WriteTarget(w, s.lock);
    w.U8((s.lockSuppressed ? 1 : 0) | (s.miningActive ? 2 : 0));
    w.F32(s.lockBreak.x); w.F32(s.lockBreak.y);
    w.F32(s.retargetCooldown);
    w.U64(s.miningRock);
    w.Str(s.miningStatus, 100);
    size_t n = std::min<size_t>(s.others.size(), SNAPSHOT_MAX_SHIPS);
    w.U8((uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        const Traffic &t = s.others[i];
        w.I32(t.id);
        WriteShip(w, t.ship);
        w.U8((t.thrusting ? 1 : 0) | (t.hyper ? 2 : 0) | (t.mining ? 4 : 0));
        if (t.mining) w.U64(t.miningRock);
    }
}

bool ReadSnapshot(Reader &r, Snapshot &s)
{
    s.tick          = r.U32();
    s.clock         = r.F64();
    s.echoTimeMs    = r.U32();
    s.lastProcessed = r.U32();
    s.lastReceived  = r.U32();
    s.player        = ReadPlayer(r);
    s.selected      = ReadTarget(r);
    s.lock          = ReadTarget(r);
    uint8_t f = r.U8();
    s.lockSuppressed = (f & 1) != 0;
    s.miningActive   = (f & 2) != 0;
    s.lockBreak.x = r.F32(); s.lockBreak.y = r.F32();
    s.retargetCooldown = r.F32();
    s.miningRock   = r.U64();
    s.miningStatus = r.Str();
    uint8_t n = r.U8();
    s.others.clear();
    for (int i = 0; i < n && r.ok; i++) {
        Traffic t;
        t.id   = r.I32();
        t.ship = ReadShip(r);
        uint8_t tf = r.U8();
        t.thrusting = (tf & 1) != 0; t.hyper = (tf & 2) != 0; t.mining = (tf & 4) != 0;
        if (t.mining) t.miningRock = r.U64();
        s.others.push_back(t);
    }
    return r.ok;
}

// ---------------------------------------------------------------------------
// The reliable channel
// ---------------------------------------------------------------------------
bool ReadHeader(Reader &r, PacketHeader &h)
{
    if (r.U32() != PROTOCOL_MAGIC) return false;
    if (r.U16() != PROTOCOL_VERSION) return false;
    h.token = r.U32();
    h.flags = r.U8();
    h.ack   = r.U32();
    return r.ok;
}

void Connection::Send(std::vector<uint8_t> msg)
{
    outbox.push_back({ nextSend++, std::move(msg), -1.0 });
}

std::vector<uint8_t> Connection::BuildPacket(const std::vector<uint8_t> &unreliable, double now, uint8_t flags, bool force)
{
    Writer w;
    w.U32(PROTOCOL_MAGIC);
    w.U16(PROTOCOL_VERSION);
    w.U32(token);
    w.U8(flags);
    w.U32(nextRecv - 1);                        // everything up to here arrived
    size_t countAt = w.Size();
    w.U8(0);

    // Reliable messages that are new or due for a resend, oldest first, while they fit
    int budget = MAX_PACKET - (int)w.Size() - (int)unreliable.size();
    int count = 0;
    for (Pending &p : outbox) {
        if (count == 255) break;
        if (p.lastSent >= 0.0 && now - p.lastSent < RESEND_INTERVAL) continue;
        int size = 6 + (int)p.data.size();
        if (size > budget) break;
        w.U32(p.seq);
        w.U16((uint16_t)p.data.size());
        w.Bytes(p.data.data(), p.data.size());
        p.lastSent = now;
        budget -= size;
        count++;
    }
    w.buf[countAt] = (uint8_t)count;
    if (count == 0 && unreliable.empty() && flags == 0 && !force) return {};
    if ((int)(w.Size() + unreliable.size()) <= MAX_PACKET) w.Bytes(unreliable.data(), unreliable.size());
    return w.buf;
}

void Connection::Receive(const PacketHeader &h, Reader &r, std::vector<std::vector<uint8_t>> &delivered,
                         std::vector<uint8_t> &unreliable, double now)
{
    lastHeard = now;
    while (!outbox.empty() && outbox.front().seq <= h.ack) outbox.pop_front();   // they have these

    uint8_t n = r.U8();
    for (int i = 0; i < n && r.ok; i++) {
        uint32_t seq = r.U32();
        uint16_t len = r.U16();
        if (!r.Need(len)) break;
        std::vector<uint8_t> data(r.p, r.p + len);
        r.p += len;
        if (seq < nextRecv) continue;                              // a resend of something we have
        if (seq > nextRecv) { if (early.size() < 1024) early[seq] = std::move(data); continue; }
        delivered.push_back(std::move(data));
        nextRecv++;
        for (auto it = early.find(nextRecv); it != early.end(); it = early.find(nextRecv)) {
            delivered.push_back(std::move(it->second));
            early.erase(it);
            nextRecv++;
        }
    }
    if (r.ok) unreliable = r.Rest();
}
