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
    w.U8((s.dampeners ? 1 : 0) | (s.allStop ? 2 : 0) | (s.dropout ? 4 : 0));
    w.F32(s.thrustUse);
    w.F32(s.throttle);
    w.F32(s.ftl);
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
    s.dropout   = (f & 4) != 0;
    s.thrustUse = r.F32();
    s.throttle  = r.F32();
    s.ftl       = r.F32();
    return s;
}

void WritePlayer(Writer &w, const Player &p)
{
    WriteShip(w, p.ship);
    w.F64(p.fuel);
    w.F64(p.credits); w.F64(p.marked);
    w.F32((float)p.power.bank); w.F32((float)p.power.driveCap); w.F32(p.power.engines);
    w.U8((uint8_t)((p.power.mode & 7) | (p.power.cold ? 8 : 0) | (p.power.radar ? 32 : 0) | (p.power.engaged ? 128 : 0) | (p.power.warping ? 64 : 0)));
    w.F32(p.power.restart);
    w.F32((float)p.passengers); w.F32((float)p.steerage);
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
    p.credits = r.F64(); p.marked = r.F64();
    p.power.bank = r.F32(); p.power.driveCap = r.F32(); p.power.engines = r.F32();
    uint8_t pm = r.U8();
    p.power.mode = (uint8_t)((pm & 7) % POWER_MODES); p.power.engaged = (pm & 128) != 0; p.power.warping = (pm & 64) != 0;
    p.power.radar = (pm & 32) != 0; p.power.cold = (pm & 8) != 0;
    p.power.restart = r.F32();
    p.passengers = r.F32(); p.steerage = r.F32();
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
    w.U8((c.hyper ? 1 : 0) | (c.fire ? 2 : 0) | (c.allStop ? 4 : 0) | (c.toggleDampeners ? 8 : 0) | (in.aim.steer ? 16 : 0) |
         (c.cyclePower ? 32 : 0) | (c.toggleRadar ? 64 : 0) | (c.toggleReactor ? 128 : 0));
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
    c.hyper = (f & 1) != 0; c.fire = (f & 2) != 0; c.allStop = (f & 4) != 0; c.toggleDampeners = (f & 8) != 0; c.cyclePower = (f & 32) != 0;
    c.toggleRadar = (f & 64) != 0; c.toggleReactor = (f & 128) != 0;
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

std::vector<uint8_t> MsgContract(uint8_t type, const Contract &k)
{
    Writer w;
    w.U8(type);
    w.U32(k.id); w.U8((uint8_t)k.kind); w.U8((uint8_t)k.effect);
    w.I32(k.issuer); w.I32(k.guild); w.I32(k.against); w.I32(k.effectOn); w.I32(k.licence); w.I32(k.law);
    w.F32(k.minStanding); w.U8(k.wide ? 1 : 0);
    w.U64(k.fromBody); w.U64(k.toBody); w.I64(k.fromSys); w.I64(k.toSys);
    w.Str(k.fromName, 80); w.Str(k.toName, 80);
    w.I64(k.surveySys); w.Str(k.surveyName, 80); w.I32(k.need); w.U64(k.scanMask);
    w.I32(k.good); w.F32(k.tonnes); w.F32(k.pay); w.F32(k.collateral);
    w.F64(k.expires); w.F64(k.allowed); w.F64(k.deadline); w.U8(k.loaded ? 1 : 0); w.U64(k.listKey);
    return w.buf;
}

bool ReadContract(Reader &r, Contract &k)
{
    k.id = r.U32();
    uint8_t kind = r.U8(), effect = r.U8();
    k.issuer = r.I32(); k.guild = r.I32(); k.against = r.I32(); k.effectOn = r.I32(); k.licence = r.I32(); k.law = r.I32();
    k.minStanding = r.F32(); k.wide = r.U8() != 0;
    k.fromBody = r.U64(); k.toBody = r.U64(); k.fromSys = r.I64(); k.toSys = r.I64();
    k.fromName = r.Str(); k.toName = r.Str();
    k.surveySys = r.I64(); k.surveyName = r.Str(); k.need = r.I32(); k.scanMask = r.U64();
    k.good = r.I32(); k.tonnes = r.F32(); k.pay = r.F32(); k.collateral = r.F32();
    k.expires = r.F64(); k.allowed = r.F64(); k.deadline = r.F64(); k.loaded = r.U8() != 0; k.listKey = r.U64();
    if (!r.ok || kind >= (uint8_t)ContractKind::Count || effect >= (uint8_t)ContractEffect::Count || k.good >= GOOD_COUNT || k.good < -1) return false;
    k.kind = (ContractKind)kind; k.effect = (ContractEffect)effect;
    return true;
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
    size_t countAt = w.Size(), start = w.Size() + 1, n = 0;
    w.U8(0);
    for (; n < s.others.size() && n < (size_t)SNAPSHOT_MAX_SHIPS && w.Size() - start < (size_t)SNAPSHOT_SHIP_BYTES; n++) {
        const Traffic &t = s.others[n];
        w.I32(t.id);
        uint8_t flags = (t.thrusting ? 1 : 0) | (t.hyper ? 2 : 0) | (t.mining ? 4 : 0);
        // (how it stands on the scope: what's known of it, by which sensors, and if it's lost, for how long)
        w.U8((uint8_t)((t.level & 3) | (t.ghost ? 4 : 0) | ((t.by & 15) << 3)));
        w.U8((uint8_t)fminf(255.0f, fmaxf(0.0f, t.age)));
        if (t.npc) {                                   // NPCs: no flight state to speak of, and a coarse heading will do
            w.I64(t.ship.anchor);
            w.F64(t.ship.pos.x); w.F64(t.ship.pos.y); w.F64(t.ship.pos.z);
            w.F32(t.ship.vel.x); w.F32(t.ship.vel.y); w.F32(t.ship.vel.z);
            const Quaternion &q = t.ship.rot;
            for (float c : { q.x, q.y, q.z, q.w }) w.U8((uint8_t)(int8_t)(c * 127.0f));
            w.U8(flags | (uint8_t)(t.stance << 4));
            continue;
        }
        WriteShip(w, t.ship);
        w.U8(flags);
        if (t.mining) w.U64(t.miningRock);
    }
    w.buf[countAt] = (uint8_t)n;
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
        uint8_t scope = r.U8();
        t.level = scope & 3; t.ghost = (scope & 4) != 0; t.by = (scope >> 3) & 15;
        t.age = (float)r.U8();
        if (t.id >= NPC_ID_BASE) {
            t.npc = true;
            t.ship.anchor = r.I64();
            t.ship.pos.x = r.F64(); t.ship.pos.y = r.F64(); t.ship.pos.z = r.F64();
            t.ship.vel.x = r.F32(); t.ship.vel.y = r.F32(); t.ship.vel.z = r.F32();
            float q[4];
            for (float &c : q) c = (float)(int8_t)r.U8() / 127.0f;
            t.ship.rot = QuaternionNormalize({ q[0], q[1], q[2], q[3] });
            uint8_t tf = r.U8();
            t.thrusting = (tf & 1) != 0; t.hyper = (tf & 2) != 0; t.mining = (tf & 4) != 0;
            t.stance = (Stance)((tf >> 4) & 3);
            s.others.push_back(t);
            continue;
        }
        t.ship = ReadShip(r);
        uint8_t tf = r.U8();
        t.thrusting = (tf & 1) != 0; t.hyper = (tf & 2) != 0; t.mining = (tf & 4) != 0;
        if (t.mining) t.miningRock = r.U64();
        s.others.push_back(t);
    }
    return r.ok;
}

std::vector<uint8_t> MsgWorld(uint64_t body, const WorldRecord &r)
{
    Writer w;
    w.U8(MSG_WORLD);
    w.U64(body); w.F64(r.at); w.Str(r.name, 80); w.F64(r.pop);
    w.F32(r.labour); w.F32(r.employed); w.F32(r.housing); w.F32(r.cellsMade); w.F32(r.cellsUsed);
    w.F32(r.living); w.F32(r.trend); w.F32(r.refugees); w.F32(r.leaving); w.F32(r.stuck); w.U8(r.border); w.Str(r.goingTo, 80); w.Str(r.barredAt, 80);
    for (int role = 0; role < ROLE_COUNT; role++) { w.F32(r.fleet[role]); w.F32(r.busy[role]); }
    w.U8((uint8_t)std::min<size_t>(r.rows.size(), 60));
    for (size_t i = 0; i < r.rows.size() && i < 60; i++) {
        const IndustryRow &in = r.rows[i];
        w.U8(in.type); w.I16(in.owner); w.U8(in.shortOf);
        for (float v : { in.site, in.cap, in.run, in.output, in.profit, in.building, in.buildLeft, in.stake, in.pending, in.cost, in.step }) w.F32(v);
    }
    return w.buf;
}

bool ReadWorld(Reader &r, uint64_t &body, WorldRecord &out)
{
    body = r.U64(); out.at = r.F64(); out.name = r.Str(); out.pop = r.F64();
    out.labour = r.F32(); out.employed = r.F32(); out.housing = r.F32(); out.cellsMade = r.F32(); out.cellsUsed = r.F32();
    out.living = r.F32(); out.trend = r.F32(); out.refugees = r.F32(); out.leaving = r.F32(); out.stuck = r.F32(); out.border = r.U8();
    out.goingTo = r.Str(); out.barredAt = r.Str();
    for (int role = 0; role < ROLE_COUNT; role++) { out.fleet[role] = r.F32(); out.busy[role] = r.F32(); }
    if (out.border > BORDER_CLOSED) return false;
    uint8_t n = r.U8();
    out.rows.clear();
    for (int i = 0; i < n && r.ok; i++) {
        IndustryRow in;
        in.type = r.U8(); in.owner = r.I16(); in.shortOf = r.U8();
        for (float *v : { &in.site, &in.cap, &in.run, &in.output, &in.profit, &in.building, &in.buildLeft, &in.stake, &in.pending, &in.cost, &in.step }) *v = r.F32();
        if (in.type >= IND_COUNT) return false;
        out.rows.push_back(in);
    }
    return r.ok;
}

std::vector<uint8_t> MsgMarks(double clock, const Captain &c)
{
    Writer w;
    w.U8(MSG_MARKS);
    w.Str(c.trafficControl, 60);
    w.U8((uint8_t)std::min<size_t>(c.marks.size(), 24));
    for (size_t i = 0; i < c.marks.size() && i < 24; i++) {
        const SensorMark &m = c.marks[i];
        w.U8(m.kind); w.I32(m.source); w.I64(m.anchor);
        w.F64(m.from.x); w.F64(m.from.y); w.F64(m.from.z);
        w.F32((float)m.dir.x); w.F32((float)m.dir.y); w.F32((float)m.dir.z);
        w.F32(m.sigma); w.F64(m.rLo); w.F64(m.rHi); w.I32(m.counts);
        w.F32((float)(clock - m.at));                              // (its age: the client's clock isn't quite ours)
        w.Str(m.note, 60);
    }
    return w.buf;
}

bool ReadMarks(Reader &r, double clock, Captain &c)
{
    std::string control = r.Str();
    std::vector<SensorMark> marks;
    uint8_t n = r.U8();
    for (int i = 0; i < n && r.ok; i++) {
        SensorMark m;
        m.kind = r.U8(); m.source = r.I32(); m.anchor = r.I64();
        m.from.x = r.F64(); m.from.y = r.F64(); m.from.z = r.F64();
        m.dir.x = r.F32(); m.dir.y = r.F32(); m.dir.z = r.F32();
        m.sigma = r.F32(); m.rLo = r.F64(); m.rHi = r.F64(); m.counts = r.I32();
        m.at = clock - r.F32();
        m.note = r.Str();
        if (m.kind > MARK_FIX) return false;
        marks.push_back(m);
    }
    if (!r.ok) return false;
    c.trafficControl = control; c.marks = std::move(marks);
    return true;
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
