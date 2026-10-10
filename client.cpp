// client.cpp - connecting, prediction and reconciliation, smoothing other ships
#include "client.h"
#include <algorithm>
#include <cmath>

static const float  STEP = (float)(1.0 / SIM_HZ);
static const double INTERP_DELAY = 12.0;        // draw other ships this many server ticks in the past (0.1 s)

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------
bool NetClient::Connect(const std::string &hostName, uint16_t port, const std::string &playerName,
                        const std::string &pass, std::string &error)
{
    Disconnect();
    host = hostName;
    name = playerName;
    password = pass;
    if (!NetResolve(hostName, port, server)) { error = "Can't find the server \"" + hostName + "\""; return false; }
    if (!socket.Open(0)) { error = "Couldn't open a UDP socket: " + socket.LastError(); return false; }

    conn = Connection();
    conn.addr = server;
    uint64_t seed = (uint64_t)(NetTime() * 1e9) ^ ((uint64_t)(uintptr_t)this << 16);
    conn.token = (uint32_t)Mix64(seed) | 1u;             // nonzero, different every session
    Writer w;
    w.U8(MSG_HELLO);
    w.Str(name);
    w.Str(password);
    conn.Send(w.buf);
    startedAt = NetTime();
    conn.lastHeard = startedAt;
    state = State::Connecting;
    message.clear();
    lastSeq = serverReceived = reconciledTo = lastSnapTick = 0;
    haveSnap = false;
    names.clear();
    samples.clear();
    return true;
}

void NetClient::Disconnect()
{
    if (socket.IsOpen() && (state == State::Joined || state == State::Connecting)) {
        if (state == State::Joined && stopServerOnLeave && !adminKey.empty()) {
            Writer a;                                    // our own server: stop it on the way out
            a.U8(MSG_ADMIN);
            a.Str(adminKey);
            a.U8(ADMIN_STOP);
            conn.Send(a.buf);
        }
        Writer w;
        w.U8(MSG_BYE);
        conn.Send(w.buf);
        double now = NetTime();
        for (int i = 0; i < 3; i++) SendPacket(conn.BuildPacket({}, now + i * RESEND_INTERVAL, PACKET_FLAG_DISCONNECT, true));
    }
    socket.Close();
    if (state == State::Joined || state == State::Connecting) state = State::Idle;
}

bool NetClient::Lose()
{
    if (simulatedLoss <= 0.0f) return false;
    lossState = Mix64(lossState);
    return (lossState >> 11) * (1.0 / 9007199254740992.0) < simulatedLoss;
}

void NetClient::SendPacket(const std::vector<uint8_t> &pkt)
{
    if (pkt.empty() || Lose()) return;
    socket.SendTo(server, pkt.data(), (int)pkt.size());
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
void NetClient::Poll(GameState &g, double now)
{
    joinedThisFrame = false;
    if (!socket.IsOpen()) return;
    uint8_t buf[2048];
    NetAddress from;
    Snapshot newest;
    bool gotSnapshot = false;
    for (int n = socket.RecvFrom(from, buf, sizeof buf); n > 0; n = socket.RecvFrom(from, buf, sizeof buf)) {
        if (from != server || Lose()) continue;
        Reader r(buf, (size_t)n);
        PacketHeader h;
        if (!ReadHeader(r, h) || h.token != conn.token) continue;
        std::vector<std::vector<uint8_t>> delivered;
        std::vector<uint8_t> payload;
        conn.Receive(h, r, delivered, payload, now);
        for (const auto &msg : delivered) HandleMessage(g, msg);
        if (state != State::Joined || payload.empty()) continue;
        Reader pr(payload);
        if (pr.U8() != PAYLOAD_SNAPSHOT) continue;
        Snapshot s;
        if (ReadSnapshot(pr, s) && (!gotSnapshot || s.tick > newest.tick)) {
            if (s.tick > lastSnapTick || !haveSnap) {
                // Other ships: keep every snapshot's positions for smoothing
                for (const Traffic &t : s.others) {
                    auto &list = samples[t.id];
                    if (list.empty() || list.back().tick < s.tick) list.push_back({ s.tick, t });
                    if (list.size() > 16) list.erase(list.begin());
                }
                newest = std::move(s);
                gotSnapshot = true;
            }
        }
    }
    if (gotSnapshot) HandleSnapshot(g, newest, now);

    if (state == State::Connecting && now - startedAt > TIMEOUT) {
        state = State::Lost;
        message = "No answer from " + host + " - is the server running, and the port open?";
    }
    if (state == State::Joined && now - conn.lastHeard > TIMEOUT) {
        state = State::Lost;
        message = "Lost contact with the server";
    }
}

void NetClient::HandleMessage(GameState &g, const std::vector<uint8_t> &msg)
{
    Reader r(msg);
    switch (r.U8()) {
    case MSG_WELCOME: {
        int id = r.I32();
        std::string myName = r.Str();
        unsigned seed = r.U32();
        double clock = r.F64();
        Player p = ReadPlayer(r);
        if (!r.ok) return;
        GameState fresh;
        NewWorld(fresh, seed);
        NewCaptain(fresh, fresh, id, myName);
        if (fresh.systems.Exists(p.ship.anchor)) {
            fresh.player = p;
            fresh.loaded = LoadedSystem();
            fresh.field  = AsteroidField();
            UpdateLoadedSystem(fresh);
        }
        fresh.clock = clock;
        fresh.events.clear();
        g = std::move(fresh);
        state = State::Joined;
        joinedThisFrame = true;
        return;
    }
    case MSG_REJECT:
        message = r.Str();
        state = state == State::Joined ? State::Lost : State::Rejected;
        return;
    case MSG_MINED: {
        uint16_t n = r.U16();
        for (int i = 0; i < n && r.ok; i++) {
            uint64_t rock = r.U64();
            float left = r.F32();
            if (!r.ok) break;
            g.deltas.mined[rock] = left;
            ApplyRockChange(g, rock, left);
        }
        return;
    }
    case MSG_EVENT: {
        GameEvent e;
        e.type   = (EventType)r.U8();
        e.text   = r.Str();
        e.where  = ReadTarget(r);
        e.item   = r.I32();
        e.amount = r.F64();
        e.value  = r.F64();
        e.player = g.id;
        if (r.ok) g.events.push_back(e);
        return;
    }
    case MSG_STANDING: {
        uint8_t n = r.U8();
        std::vector<float> s;
        for (int i = 0; i < n && r.ok; i++) s.push_back(r.F32());
        if (r.ok) g.standing = s;
        return;
    }
    case MSG_COMMS: {
        CommsChannel c;
        c.with = ReadTarget(r);
        bool open = r.U8() != 0;
        c.withName = r.Str();
        uint8_t n = r.U8();
        for (int i = 0; i < n && r.ok; i++) {
            CommsMsg m;
            m.sent = r.F64();
            m.at   = r.F64();
            m.ours = r.U8() != 0;
            m.text = r.Str();
            c.log.push_back(m);
        }
        if (!r.ok) return;
        std::vector<CommsChannel> &all = g.comms.channels;
        all.erase(std::remove_if(all.begin(), all.end(), [&](const CommsChannel &o) { return o.with == c.with; }), all.end());
        if (open) all.push_back(c);
        return;
    }
    case MSG_JOINED: {
        int id = r.I32();
        std::string who = r.Str();
        bool justNow = r.U8() != 0;          // (0: was already here when we joined)
        if (!r.ok) return;
        names[id] = who;
        if (justNow) g.events.push_back({ EventType::PlayerJoined, "Captain " + who + " joined", {}, -1, 0.0, 0.0, g.id });
        return;
    }
    case MSG_LEFT: {
        int id = r.I32();
        std::string who = r.Str();
        if (!r.ok) return;
        names.erase(id);
        samples.erase(id);
        g.events.push_back({ EventType::PlayerLeft, "Captain " + who + " left", {}, -1, 0.0, 0.0, g.id });
        return;
    }
    default:
        return;                                // something a newer server sends; ignore
    }
}

void NetClient::HandleSnapshot(GameState &g, const Snapshot &s, double now)
{
    // Ping: the server echoes the timestamp of our latest packet
    if (s.echoTimeMs) {
        int rtt = (int)((uint32_t)(now * 1000.0) - s.echoTimeMs);
        if (rtt >= 0 && rtt < 10000) pingMs = pingMs == 0 ? rtt : (pingMs * 7 + rtt) / 8;
    }

    // Clocks: the world's (comms replies, rock spin) and the server tick estimate (smoothing)
    if (fabs(g.clock - s.clock) > 1.0) g.clock = s.clock;
    else g.clock += (s.clock - g.clock) * 0.1;
    double drift = (double)s.tick - tickClock;
    if (!haveSnap || fabs(drift) > 60.0) tickClock = s.tick;
    else tickClock += drift * 0.1;
    lastSnapTick = s.tick;
    haveSnap = true;
    serverReceived = std::max(serverReceived, s.lastReceived);

    // Things only the server decides: credits, cargo, the mining laser
    g.player.credits = s.player.credits;
    for (int o = 0; o < GOOD_COUNT; o++) g.player.cargo[o] = s.player.cargo[o];
    g.flight.miningActive = s.miningActive;
    g.flight.miningRock   = s.miningRock;
    g.flight.miningStatus = s.miningStatus;

    // Ships that dropped out of the snapshot (left, or no longer among the nearest)
    for (auto it = samples.begin(); it != samples.end();)
        it = !it->second.empty() && s.tick > it->second.back().tick + 120 ? samples.erase(it) : std::next(it);

    if (s.lastProcessed > reconciledTo) Reconcile(g, s);
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------
void NetClient::Remember(GameState &g, uint32_t seq, const Controls &c, const Aim &aim)
{
    Predicted &h = history[seq % HISTORY];
    h.seq = seq; h.controls = c; h.aim = aim;
    h.ship = g.player.ship; h.fuel = g.player.fuel;
    const FlightState &f = g.flight;
    h.selected = f.selected; h.lock = f.lock; h.lockSuppressed = f.lockSuppressed;
    h.lockBreak = f.lockBreak; h.retargetCooldown = f.retargetCooldown;
}

void NetClient::Step(GameState &g, const Controls &c, const Aim &aim, float dt)
{
    uint32_t seq = ++lastSeq;
    g.clock += dt;
    StepCaptain(g, g, c, aim, dt, false);           // flight and fuel; the server does mining and comms
    Remember(g, seq, c, aim);
}

// Did we predict what the server got? Same code and the same inputs, so normally
// yes, to the last bit; a rock someone else mined, or a different CPU's maths, can
// make a small difference.
static bool Agrees(const Ship &a, double fuelA, const Ship &b, double fuelB)
{
    if (a.anchor != b.anchor || a.dampeners != b.dampeners || a.allStop != b.allStop) return false;
    if (fabsf(a.throttle - b.throttle) > 1e-4f) return false;
    double speed = Vector3Length(b.vel);
    if (Len(a.pos - b.pos) > 0.5 + 1e-6 * speed) return false;
    if (Vector3Length(Vector3Subtract(a.vel, b.vel)) > 0.05f + 1e-5f * speed) return false;
    float dot = fabsf(a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z + a.rot.w * b.rot.w);
    if (dot < 0.99999f) return false;              // ~0.5 degree
    return fabs(fuelA - fuelB) < 0.02;
}

void NetClient::Reconcile(GameState &g, const Snapshot &s)
{
    uint32_t S = s.lastProcessed;
    reconciledTo = S;
    const Predicted &h = history[S % HISTORY];
    bool known = h.seq == S;
    if (known && Agrees(h.ship, h.fuel, s.player.ship, s.player.fuel) && h.lock == s.lock &&
        h.lockSuppressed == s.lockSuppressed)
        return;

    // Rewind to what the server says we were after step S, then replay what we've done since
    g.player.ship = s.player.ship;
    g.player.fuel = s.player.fuel;
    FlightState &f = g.flight;
    f.selected = s.selected; f.lock = s.lock; f.lockSuppressed = s.lockSuppressed;
    f.lockBreak = s.lockBreak; f.retargetCooldown = s.retargetCooldown;
    corrections++;
    if (!known || lastSeq - S >= (uint32_t)HISTORY) return;   // too far behind to replay: just take the server's word
    for (uint32_t seq = S + 1; seq <= lastSeq; seq++) {
        Predicted &p = history[seq % HISTORY];
        if (p.seq != seq) break;
        StepCaptain(g, g, p.controls, p.aim, STEP, false);
        Remember(g, seq, p.controls, p.aim);
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
void NetClient::SendCommand(Command c)
{
    if (state != State::Joined) return;
    c.step = lastSeq;                                // apply it after the step we've just flown
    conn.Send(MsgCommand(c));
}

void NetClient::Flush(double now)
{
    if (!socket.IsOpen() || state == State::Rejected || state == State::Lost) return;
    std::vector<uint8_t> payload;
    if (state == State::Joined) {
        // Every input step the server doesn't have yet (up to the 24 newest): a lost
        // packet costs nothing, the next one carries the same steps again
        uint32_t first = std::max(serverReceived + 1, lastSeq >= 23 ? lastSeq - 23 : 1u);
        uint32_t n = lastSeq >= first ? lastSeq - first + 1 : 0;
        Writer w;
        w.U8(PAYLOAD_INPUTS);
        w.U32((uint32_t)(now * 1000.0));
        w.U32(first);
        w.U8((uint8_t)n);
        for (uint32_t i = 0; i < n; i++) {
            const Predicted &p = history[(first + i) % HISTORY];
            InputStep in;
            in.seq = first + i;
            if (p.seq == in.seq) { in.controls = p.controls; in.aim = p.aim; }
            WriteInput(w, in);
        }
        payload = std::move(w.buf);
    }
    SendPacket(conn.BuildPacket(payload, now, 0, state == State::Joined));
}

// ---------------------------------------------------------------------------
// Other ships, smoothed
// ---------------------------------------------------------------------------
static Ship InterpolateShip(const Galaxy &sys, const Ship &a, const Ship &b, double t)
{
    Ship s = b;
    DVec3 aPos = a.pos + sys.Between(b.anchor, a.anchor);           // a in b's frame (exact: star positions are on a grid)
    s.pos = aPos + (b.pos - aPos) * t;
    s.vel = Vector3Lerp(a.vel, b.vel, (float)t);
    s.rot = QuaternionSlerp(a.rot, b.rot, (float)t);
    return s;
}

void NetClient::UpdateTraffic(GameState &g, float frameDt)
{
    g.traffic.clear();
    if (state != State::Joined || !haveSnap) return;
    tickClock += frameDt * SIM_HZ;
    double renderTick = tickClock - INTERP_DELAY;
    for (const auto &entry : samples) {
        const std::vector<Sample> &list = entry.second;
        if (list.empty()) continue;
        Traffic t;
        if (renderTick >= list.back().tick) {
            // Past the newest snapshot (a late packet): coast along a little
            t = list.back().t;
            double ahead = fmin((renderTick - list.back().tick) / SIM_HZ, 0.25);
            t.ship.pos = t.ship.pos + ToD(t.ship.vel) * ahead;
        } else if (renderTick <= list.front().tick) {
            t = list.front().t;
        } else {
            size_t i = 1;
            while (i < list.size() && list[i].tick < renderTick) i++;
            const Sample &a = list[i - 1], &b = list[i];
            double u = (renderTick - a.tick) / (double)(b.tick - a.tick);
            t = b.t;
            t.ship = InterpolateShip(g.systems, a.t.ship, b.t.ship, u);
        }
        auto nm = names.find(entry.first);
        t.name = nm != names.end() ? nm->second : "Captain " + std::to_string(entry.first);
        g.traffic.push_back(t);
    }
}
