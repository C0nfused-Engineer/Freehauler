// server.cpp - the dedicated server
#include "server.h"
#include "partsdata.h"
#include "contracts.h"
#include "economy.h"
#include "npcs.h"
#include "shipyard.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

static const float STEP = (float)(1.0 / SIM_HZ);

bool ValidPlayerName(const std::string &name)
{
    if (name.empty() || (int)name.size() > MAX_PLAYER_NAME || name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name)
        if (!(isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')) return false;
    return true;
}

static std::string Lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void Server::Log(const char *fmt, ...)
{
    if (!cfg.log) return;
    time_t t = time(nullptr);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
    printf("[%s] ", stamp);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

int Server::PlayerCount() const
{
    int n = 0;
    for (const auto &p : peers) if (p->joined && !p->remove) n++;
    return n;
}

Server::Peer *Server::FindByName(const std::string &name)
{
    for (auto &p : peers)
        if (p->joined && !p->remove && Lower(p->cap.name) == Lower(name)) return p.get();
    return nullptr;
}

void Server::Broadcast(const std::vector<uint8_t> &msg, const Peer *except)
{
    for (auto &p : peers)
        if (p->joined && !p->remove && p.get() != except) p->conn.Send(msg);
}

// ---------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------
bool Server::Start(const ServerConfig &config, std::string &error)
{
    cfg = config;
    std::error_code ec;
    fs::create_directories(fs::path(cfg.dataDir) / "players", ec);
    if (ec) { error = "Couldn't create the data folder " + cfg.dataDir + ": " + ec.message(); return false; }

    if (LoadWorld()) {
        Log("Loaded the galaxy from %s (seed %u, %d mined rocks)", (fs::path(cfg.dataDir) / "world.txt").string().c_str(),
            world.seed, (int)world.deltas.mined.size());
        if (world.seed != cfg.seed && cfg.seed != DEFAULT_GALAXY_SEED)
            Log("Note: --seed %u ignored; this data folder already has a galaxy (delete world.txt for a new one)", cfg.seed);
    } else {
        NewWorld(world, cfg.seed);
        Log("New galaxy, seed %u", world.seed);
        Log("Building its economy...");
        EnsureEconomy(world);                       // (it takes a while: better now than with captains waiting on the line)
        SaveWorld();
    }

    if (!socket.Open(cfg.port)) {
        error = "Couldn't open UDP port " + std::to_string(cfg.port) + " (" + socket.LastError() +
                "). Is another server already running on it?";
        return false;
    }
    lastAutosave = NetTime();
    Log("Listening on UDP port %u  (max %d captains%s)", cfg.port, cfg.maxPlayers, cfg.password.empty() ? "" : ", password set");
    return true;
}

void Server::Stop()
{
    if (!socket.IsOpen()) return;
    Writer w;
    w.U8(MSG_REJECT);
    w.Str(stopRequested ? "The host has closed the server" : "The server is shutting down");
    Broadcast(w.buf);
    double now = NetTime();
    for (int i = 0; i < 3; i++) SendPackets(now + i * RESEND_INTERVAL);   // a few tries at getting the word out
    SaveAll();
    socket.Close();
    Log("Stopped");
}

void Server::Run(const volatile bool &quit)
{
    double next = NetTime();
    while (!quit && !stopRequested) {
        double now = NetTime();
        if (now < next) { NetSleep(fmin(next - now, 0.002)); continue; }
        Tick();
        next += STEP;
        if (now - next > 0.5) next = now;           // fell far behind (machine asleep?): don't try to catch up
    }
}

// ---------------------------------------------------------------------------
// One tick
// ---------------------------------------------------------------------------
void Server::Tick()
{
    double now = NetTime();
    Receive(now);

    // Everyone sees everyone else where they are at the start of the tick
    world.traffic.clear();
    for (auto &p : peers) {
        if (!p->joined || p->remove) continue;
        const Captain &c = p->cap;
        Traffic t;
        t.id = c.id; t.name = c.name; t.shipName = c.shipName; t.ship = c.player.ship; t.hull = (uint8_t)c.player.build.hull;
        t.thrusting  = c.player.ship.throttle > 0.0f;
        t.hyper      = c.flight.last.hyper;
        t.mining     = c.flight.miningActive;
        t.miningRock = c.flight.miningRock;
        world.traffic.push_back(t);
    }

    {   // The other ships, around wherever the captains are
        std::vector<Captain *> caps;
        for (auto &p : peers) if (p->joined && !p->remove) caps.push_back(&p->cap);
        StepTraffic(world, caps.data(), (int)caps.size(), STEP);
    }

    // Each captain moves through the input steps its client has sent, in order. Normally
    // that's one per tick; after a hiccup (a late packet) a few at once, to catch up.
    for (auto &pp : peers) {
        Peer &p = *pp;
        if (!p.joined || p.remove) continue;
        p.inputTokens = fmin(p.inputTokens + 1.0, 40.0);
        int allowed = p.inputs.size() > 4 ? 3 : 1;
        for (int k = 0; k < allowed && !p.inputs.empty() && p.inputTokens >= 1.0; k++) {
            InputStep in = p.inputs.front();
            p.inputs.pop_front();
            StepCaptain(world, p.cap, in.controls, in.aim, STEP, true);
            p.lastProcessed = in.seq;
            p.inputTokens -= 1.0;
            ApplyDueCommands(p, now);
        }
        ApplyDueCommands(p, now);                    // stragglers whose input step never arrived
    }
    world.clock += STEP;
    UpdateEconomy(world);
    UpdateContracts(world);

    // Mined rocks: every captain's copy of the field, then (every 0.1 s) every client
    for (const auto &m : world.minedChanged) {
        for (auto &p : peers) if (p->joined) ApplyRockChange(p->cap, m.first, m.second);
        minedToSend[m.first] = m.second;
    }
    world.minedChanged.clear();
    if (tick % 12 == 0 && !minedToSend.empty()) {
        for (auto &msg : MsgMined(minedToSend)) Broadcast(msg);
        minedToSend.clear();
    }

    // Events with something to say go to the captain they happened to
    for (const GameEvent &e : world.events) {
        if (e.text.empty()) continue;
        std::vector<uint8_t> msg = MsgEvent(e);
        for (auto &p : peers)
            if (p->joined && !p->remove && (e.player < 0 || e.player == p->cap.id)) p->conn.Send(msg);
    }
    world.events.clear();

    if (tick % SNAPSHOT_EVERY == 0)
        for (auto &p : peers) {
            if (!p->joined || p->remove) continue;
            // The markets this captain has fresh prices for, and the news they haven't had
            for (const auto &kv : p->cap.know.prices) {
                double &sent = p->pricesSent[kv.first];
                if (sent == kv.second.at) continue;
                sent = kv.second.at;
                p->conn.Send(MsgPrices(kv.first, kv.second));
            }
            for (const auto &kv : p->cap.know.worlds) {               // ...and the worlds whose economy they've had a fresh look at
                double &sent = p->worldsSent[kv.first];
                if (sent == kv.second.at) continue;
                sent = kv.second.at;
                p->conn.Send(MsgWorld(kv.first, kv.second));
            }
            for (const EconNews &n : world.news)
                if (n.seq > p->newsSent) { p->conn.Send(MsgNews(n)); p->newsSent = n.seq; }
            if (p->cap.standing != p->standingSent) {
                Writer w;
                w.U8(MSG_STANDING);
                w.U8((uint8_t)std::min<size_t>(p->cap.standing.size(), 255));
                for (size_t i = 0; i < p->cap.standing.size() && i < 255; i++) w.F32(p->cap.standing[i]);
                p->conn.Send(w.buf);
                p->standingSent = p->cap.standing;
            }
            // Boards this captain has read (or that changed under them), their own contracts, their papers
            for (uint64_t key : p->cap.boardsToSend) {
                Writer w;
                w.U8(MSG_BOARD); w.U64(key); w.F64(p->cap.boards[key]);
                p->conn.Send(w.buf);
                for (const Contract &k : p->cap.offers) if (k.listKey == key) p->conn.Send(MsgContract(MSG_OFFER, k));
            }
            p->cap.boardsToSend.clear();
            uint64_t sig = 0x1234;
            for (const Contract &k : p->cap.jobs) sig = Mix64(sig ^ k.id ^ ((uint64_t)k.loaded << 40) ^ Mix64(k.scanMask + 1));
            if (sig != p->jobsSent) {
                p->jobsSent = sig;
                Writer w;
                w.U8(MSG_JOBS);
                p->conn.Send(w.buf);
                for (const Contract &k : p->cap.jobs) p->conn.Send(MsgContract(MSG_JOB, k));
            }
            {   // Their ship, when it's been changed
                Writer w;
                const ShipBuild &b = p->cap.player.build;
                w.U8(MSG_BUILD); w.I32(b.hull); w.U8(b.stretch); w.U8(b.mounts); w.U8((uint8_t)std::min<size_t>(b.parts.size(), 255));
                for (size_t i = 0; i < b.parts.size() && i < 255; i++) { w.I32(PackPart(b.parts[i])); w.I16(b.parts[i].maker); }
                if (w.buf != p->buildSent) { p->conn.Send(w.buf); p->buildSent = w.buf; }
            }
            if (p->cap.papers != p->papersSent) {
                Writer w;
                w.U8(MSG_PAPERS);
                w.U8((uint8_t)std::min<size_t>(p->cap.papers.size(), 255));
                for (size_t i = 0; i < p->cap.papers.size() && i < 255; i++) w.U8(p->cap.papers[i]);
                p->conn.Send(w.buf);
                p->papersSent = p->cap.papers;
            }
            // Each channel that has changed since it was last sent, and word of the ones that have closed
            std::vector<std::pair<TargetId, std::vector<uint8_t>>> now;
            for (const CommsChannel &ch : p->cap.comms.channels) now.push_back({ ch.with, MsgComms(ch) });
            for (const auto &was : p->commsSent) {
                bool still = false;
                for (const auto &is : now) if (is.first == was.first) still = true;
                if (!still) { CommsChannel gone; gone.with = was.first; p->conn.Send(MsgComms(gone, false)); }
            }
            for (const auto &is : now) {
                bool same = false;
                for (const auto &was : p->commsSent) if (was.first == is.first && was.second == is.second) same = true;
                if (!same) p->conn.Send(is.second);
            }
            p->commsSent = std::move(now);
        }

    for (auto &p : peers)
        if (!p->remove && now - p->conn.lastHeard > TIMEOUT) Drop(*p, "timed out");

    SendPackets(now);
    peers.erase(std::remove_if(peers.begin(), peers.end(), [](const std::unique_ptr<Peer> &p) { return p->remove; }), peers.end());

    if (now - lastAutosave > 60.0) { SaveAll(); lastAutosave = now; }
    tick++;
}

void Server::ApplyDueCommands(Peer &p, double now)
{
    for (size_t i = 0; i < p.commands.size();) {
        if (p.commands[i].step <= p.lastProcessed || now - p.commandsSince[i] > 1.0) {
            ApplyCommand(world, p.cap, p.commands[i]);
            p.commands.erase(p.commands.begin() + i);
            p.commandsSince.erase(p.commandsSince.begin() + i);
        } else {
            i++;
        }
    }
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
void Server::Receive(double now)
{
    uint8_t buf[2048];
    NetAddress from;
    for (int n = socket.RecvFrom(from, buf, sizeof buf); n > 0; n = socket.RecvFrom(from, buf, sizeof buf)) {
        Reader r(buf, (size_t)n);
        PacketHeader h;
        if (!ReadHeader(r, h) || h.token == 0) continue;            // not ours, or an older version

        Peer *p = nullptr;
        for (auto &q : peers)
            if (!q->remove && q->conn.addr == from) { p = q.get(); break; }
        if (p && p->conn.token != h.token) {                         // same address, new game session
            Drop(*p, "reconnected");
            p = nullptr;
        }
        if (!p) {
            if (h.flags & PACKET_FLAG_DISCONNECT) continue;
            peers.push_back(std::make_unique<Peer>());
            p = peers.back().get();
            p->conn.addr = from;
            p->conn.token = h.token;
            p->conn.lastHeard = now;
        }

        std::vector<std::vector<uint8_t>> delivered;
        std::vector<uint8_t> unreliable;
        p->conn.Receive(h, r, delivered, unreliable, now);
        for (const auto &msg : delivered) {
            if (p->remove) break;
            HandleMessage(*p, msg, now);
        }
        if (!p->joined && !p->remove && p->conn.Unacked() == 0) p->remove = true;   // never said hello
        if (p->joined && !p->remove && !unreliable.empty()) HandleInputs(*p, unreliable);
        if ((h.flags & PACKET_FLAG_DISCONNECT) && !p->remove) Drop(*p, "left");
    }
}

void Server::HandleMessage(Peer &p, const std::vector<uint8_t> &msg, double now)
{
    Reader r(msg);
    switch (r.U8()) {
    case MSG_HELLO: {
        if (p.joined) return;
        std::string name = r.Str(), password = r.Str(), ship = r.Str();
        uint64_t parts = r.U64();
        if (!ValidPlayerName(ship)) ship.clear();              // (optional; anything odd is just dropped)
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (!r.ok) return;
        if (!ValidPlayerName(name)) { Reject(p, "Pick a name of 1-20 letters, digits, spaces, - or _"); return; }
        if (!cfg.password.empty() && password != cfg.password) { Reject(p, "Wrong server password"); return; }
        if (parts != PartsHash()) { Reject(p, "Your ship parts (parts.txt) aren't the same as this server's"); return; }
        if (Peer *other = FindByName(name)) {
            if (other->conn.addr.ip == p.conn.addr.ip) Drop(*other, "reconnected from a new session");   // you, after a crash
            else { Reject(p, "A captain named " + name + " is already flying here"); return; }
        }
        if (PlayerCount() >= cfg.maxPlayers) { Reject(p, "The server is full"); return; }
        Join(p, name, ship);
        return;
    }
    case MSG_COMMAND: {
        if (!p.joined) return;
        Command c;
        uint8_t type = r.U8();
        c.arg  = r.I32();
        c.qty  = r.F64();
        c.step = r.U32();
        if (!r.ok || type >= Command::Count || !std::isfinite(c.qty)) return;
        c.type = (Command::Type)type;
        if (p.commands.size() < 64) { p.commands.push_back(c); p.commandsSince.push_back(now); }
        return;
    }
    case MSG_BYE:
        Drop(p, "left");
        return;
    case MSG_ADMIN: {
        std::string key = r.Str();
        uint8_t action = r.U8();
        if (!r.ok || !p.joined || cfg.adminKey.empty() || key != cfg.adminKey) return;
        if (action == ADMIN_STOP) {
            Log("%s (the host) is stopping the server", p.cap.name.c_str());
            stopRequested = true;
        }
        return;
    }
    default:
        return;                                     // unknown: a newer client; ignore
    }
}

void Server::HandleInputs(Peer &p, const std::vector<uint8_t> &payload)
{
    Reader r(payload);
    if (r.U8() != PAYLOAD_INPUTS) return;
    uint32_t echo  = r.U32();
    uint32_t first = r.U32();
    uint8_t  n     = r.U8();
    if (!r.ok) return;
    p.echoTimeMs = echo;
    for (uint32_t i = 0; i < n; i++) {
        InputStep in = ReadInput(r, first + i);
        if (!r.ok) break;
        if (in.seq <= p.lastReceived) continue;     // already have it (inputs are sent several times over)
        if (p.inputs.size() >= 360) break;          // three seconds behind: something's wrong, don't hoard
        p.inputs.push_back(in);
        p.lastReceived = in.seq;
    }
}

// ---------------------------------------------------------------------------
// Joining and leaving
// ---------------------------------------------------------------------------
void Server::Join(Peer &p, const std::string &name, const std::string &ship)
{
    p.joined = true;
    NewCaptain(world, p.cap, nextId++, name);
    p.cap.shipName = ship;
    Player saved;
    bool returning = LoadPlayer(name, saved, p.cap);
    for (const auto &b : p.cap.boards) p.cap.boardsToSend.push_back(b.first);      // (what they'd read of the boards, back to their client)
    if (returning) {
        saved.build = p.cap.player.build;           // (LoadPlayer read the ship straight into the captain)
        p.cap.player = saved;
        Refit(p.cap);
        p.cap.player.power.bank = p.cap.player.spec.bankTJ; p.cap.player.power.driveCap = p.cap.player.spec.driveCapTJ;
        p.cap.loaded = LoadedSystem();
        p.cap.field  = AsteroidField();
        UpdateLoadedSystem(world, p.cap);
    }
    world.events.erase(std::remove_if(world.events.begin(), world.events.end(),
                                      [&](const GameEvent &e) { return e.player == p.cap.id; }), world.events.end());

    Writer w;
    w.U8(MSG_WELCOME);
    w.I32(p.cap.id);
    w.Str(name);
    w.U32(world.seed);
    w.F64(world.clock);
    WritePlayer(w, p.cap.player);
    p.conn.Send(w.buf);
    for (auto &msg : MsgMined(world.deltas.mined)) p.conn.Send(msg);

    for (auto &q : peers) {
        if (!q->joined || q->remove || q.get() == &p) continue;
        Writer j;                                   // everyone already here, to the newcomer
        j.U8(MSG_JOINED); j.I32(q->cap.id); j.Str(q->cap.name); j.U8(0); j.Str(q->cap.shipName);
        p.conn.Send(j.buf);
    }
    Writer j;                                       // the newcomer, to everyone already here
    j.U8(MSG_JOINED); j.I32(p.cap.id); j.Str(name); j.U8(1); j.Str(ship);
    Broadcast(j.buf, &p);

    int others = PlayerCount() - 1;
    GameEvent hello;
    hello.type = EventType::Notice;
    hello.text = others == 0 ? "Welcome aboard, captain - you're the first one here"
                             : TextFormat("Welcome aboard - %d other captain%s flying", others, others == 1 ? "" : "s");
    p.conn.Send(MsgEvent(hello));
    Log("%s joined from %s (%s, id %d) - %d online", name.c_str(), p.conn.addr.ToString().c_str(),
        returning ? "returning" : "new captain", p.cap.id, PlayerCount());
}

void Server::Reject(Peer &p, const std::string &reason)
{
    Writer w;
    w.U8(MSG_REJECT);
    w.Str(reason);
    p.conn.Send(w.buf);
    double now = NetTime();
    for (int i = 0; i < 2; i++) {
        std::vector<uint8_t> pkt = p.conn.BuildPacket({}, now + i * RESEND_INTERVAL, PACKET_FLAG_DISCONNECT, true);
        socket.SendTo(p.conn.addr, pkt.data(), (int)pkt.size());
    }
    Log("Turned away %s: %s", p.conn.addr.ToString().c_str(), reason.c_str());
    p.remove = true;
}

void Server::Drop(Peer &p, const std::string &why)
{
    if (p.remove) return;
    p.remove = true;
    if (!p.joined) return;
    SavePlayer(p.cap);
    Writer w;
    w.U8(MSG_LEFT); w.I32(p.cap.id); w.Str(p.cap.name);
    Broadcast(w.buf, &p);
    Log("%s %s - %d online", p.cap.name.c_str(), why.c_str(), PlayerCount());
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
void Server::SendPackets(double now)
{
    bool snapshotTick = tick % SNAPSHOT_EVERY == 0;
    for (auto &pp : peers) {
        Peer &p = *pp;
        if (p.remove) continue;
        std::vector<uint8_t> payload;
        if (snapshotTick && p.joined && !p.remove) {
            const Captain &c = p.cap;
            Snapshot s;
            s.tick = tick; s.clock = world.clock; s.echoTimeMs = p.echoTimeMs;
            s.lastProcessed = p.lastProcessed; s.lastReceived = p.lastReceived;
            s.player = c.player;
            s.selected = c.flight.selected; s.lock = c.flight.lock;
            s.lockSuppressed = c.flight.lockSuppressed; s.lockBreak = c.flight.lockBreak;
            s.retargetCooldown = c.flight.retargetCooldown;
            s.miningActive = c.flight.miningActive; s.miningRock = c.flight.miningRock; s.miningStatus = c.flight.miningStatus;
            for (const Traffic &t : world.traffic)
                if (t.id != c.id && (!t.npc || t.ship.anchor == c.loaded.index)) s.others.push_back(t);
            ScopeTraffic(world, p.cap, s.others);              // (only what this captain's sensors hold, and no more of each than they've made out)
            auto dist = [&](const Traffic &t) { return Len(FromShip(world.systems, c.player.ship, t.ship.anchor, t.ship.pos)); };
            std::sort(s.others.begin(), s.others.end(), [&](const Traffic &a, const Traffic &b) { return a.npc != b.npc ? !a.npc : dist(a) < dist(b); });
            if (s.others.size() > (size_t)SNAPSHOT_MAX_SHIPS) s.others.resize(SNAPSHOT_MAX_SHIPS);
            if (p.npcTold.size() > 4000) p.npcTold.clear();
            for (Traffic &t : s.others) {
                if (!t.npc) continue;
                t.stance = t.level >= CONTACT_IDENTIFIED ? TrafficStance(world, c, t) : STANCE_NEUTRAL;
                // Who it is goes once it's been made out (the client shows no more of it than the contact's level allows)
                const Traffic *truth = t.level >= CONTACT_CLASSIFIED && !t.ghost ? world.FindTraffic(t.id) : nullptr;
                if (!truth) continue;
                auto told = p.npcTold.find(t.id);
                if (told != p.npcTold.end() && told->second == truth->tonnes) continue;
                p.npcTold[t.id] = truth->tonnes;
                Writer m;
                m.U8(MSG_NPC); m.I32(t.id); m.U8((uint8_t)truth->kind); m.U8(truth->hull); m.I32(truth->faction);
                m.Str(truth->name, 60); m.Str(truth->from, 60); m.Str(truth->dest, 60);
                m.I32(truth->good); m.F32(truth->tonnes);
                p.conn.Send(m.buf);
            }
            if (p.cap.marksChanged && fabs(now - p.marksSent) >= 0.5) {
                p.marksSent = now; p.cap.marksChanged = false;
                p.conn.Send(MsgMarks(world.clock, p.cap));
            }
            Writer w;
            WriteSnapshot(w, s);
            payload = std::move(w.buf);
        }
        std::vector<uint8_t> pkt = p.conn.BuildPacket(payload, now);
        if (!pkt.empty()) socket.SendTo(p.conn.addr, pkt.data(), (int)pkt.size());
    }
}

// ---------------------------------------------------------------------------
// Keeping progress: world.txt and players/<name>.txt in the data folder
// ---------------------------------------------------------------------------
// Written to a temporary file first, so a crash mid-save never leaves half a file
static bool WriteFileSafely(const fs::path &path, const std::string &text)
{
    fs::path tmp = path;
    tmp += ".tmp";
    FILE *f = fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) fs::rename(tmp, path, ec);
    return ok && !ec;
}

void Server::SaveAll()
{
    int n = 0;
    for (auto &p : peers) if (p->joined && !p->remove && SavePlayer(p->cap)) n++;
    bool ok = SaveWorld();
    Log("Saved the galaxy%s and %d captain%s", ok ? "" : " (FAILED)", n, n == 1 ? "" : "s");
}

bool Server::SaveWorld()
{
    std::string s = TextFormat("SPACEFLIGHT_WORLD 2\nseed %u\nclock %.17g\n", world.seed, world.clock);
    for (const auto &m : world.deltas.mined) s += TextFormat("mined %llu %.9g\n", (unsigned long long)m.first, m.second);
    WriteEconomy(world, s);
    WriteContracts(world, s);
    s += "end\n";
    return WriteFileSafely(fs::path(cfg.dataDir) / "world.txt", s);
}

bool Server::LoadWorld()
{
    FILE *f = fopen((fs::path(cfg.dataDir) / "world.txt").string().c_str(), "r");
    if (!f) return false;
    static char line[8192];
    int version = 0;
    std::vector<std::pair<std::string, std::string>> econ;
    unsigned seed = DEFAULT_GALAXY_SEED;
    double clock = 0.0;
    MinedMap mined;
    bool ended = false;
    if (fgets(line, sizeof line, f) && sscanf(line, "SPACEFLIGHT_WORLD %d", &version) == 1)
        while (fgets(line, sizeof line, f)) {
            unsigned long long id; float left;
            if (sscanf(line, "seed %u", &seed) == 1) continue;
            if (sscanf(line, "clock %lf", &clock) == 1) continue;
            if (sscanf(line, "mined %llu %f", &id, &left) == 2) { mined[(uint64_t)id] = left; continue; }
            if (!strncmp(line, "end", 3)) { ended = true; break; }
            char key[32] = "";                               // anything else: the economy's
            if (sscanf(line, "%31s", key) == 1) econ.push_back({ key, line + strlen(key) });
        }
    fclose(f);
    if (!ended || version < 2) return false;        // (version 1: the old twelve-star galaxy, which is gone)
    NewWorld(world, seed);
    world.clock = clock;
    world.deltas.mined = std::move(mined);
    EnsureEconomy(world);
    for (const auto &l : econ)
        if (!ReadEconomyLine(world, l.first.c_str(), l.second.c_str())) ReadContractLine(world, l.first.c_str(), l.second.c_str());
    return true;
}

std::string Server::PlayerPath(const std::string &name) const
{
    return (fs::path(cfg.dataDir) / "players" / (Lower(name) + ".txt")).string();
}

bool Server::SavePlayer(const Captain &c)
{
    const Player &p = c.player;
    const Ship &s = p.ship;
    std::string text = TextFormat("SPACEFLIGHT_PLAYER 3\nname %s\n", c.name.c_str());
    text += TextFormat("ship %lld %.17g %.17g %.17g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %.9g\n", (long long)s.anchor,
                       s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, s.rot.x, s.rot.y, s.rot.z, s.rot.w, s.dampeners ? 1 : 0,
                       s.throttle);
    text += TextFormat("fuel %.17g\ncredits %.17g\nmarked %.17g\n", p.fuel, p.credits, p.marked);
    for (int o = 0; o < GOOD_COUNT; o++)
        if (p.cargo[o] > 0.0) text += TextFormat("cargo %d %.17g\n", o, p.cargo[o]);
    for (size_t i = 0; i < c.standing.size(); i++)
        if (c.standing[i] != 0.0f) text += TextFormat("standing %d %.9g\n", (int)i, c.standing[i]);
    WriteBuild(c, text);
    WriteCaptainContracts(c, text);
    WriteStakes(c, text);
    text += "end\n";
    return WriteFileSafely(PlayerPath(c.name), text);
}

bool Server::LoadPlayer(const std::string &name, Player &out, Captain &cap)
{
    std::vector<float> &standing = cap.standing;
    FILE *f = fopen(PlayerPath(name).c_str(), "r");
    if (!f) return false;
    static char line[2048];
    Player p;
    bool haveShip = false, ended = false;
    int version = 0;
    if (fgets(line, sizeof line, f) && sscanf(line, "SPACEFLIGHT_PLAYER %d", &version) == 1)
        while (fgets(line, sizeof line, f)) {
            Ship &s = p.ship;
            int damp = 1, ore; double t;
            long long anchor = -1;
            char key[32] = "";
            if (sscanf(line, "%31s", key) == 1 && (ReadCaptainContractLine(cap, key, line + strlen(key)) || ReadBuildLine(cap, key, line + strlen(key)) || ReadStakeLine(cap, key, line + strlen(key)))) continue;
            if (sscanf(line, "ship %lld %lf %lf %lf %f %f %f %f %f %f %f %d %f", &anchor, &s.pos.x, &s.pos.y, &s.pos.z,
                       &s.vel.x, &s.vel.y, &s.vel.z, &s.rot.x, &s.rot.y, &s.rot.z, &s.rot.w, &damp, &s.throttle) >= 12) {
                s.anchor = anchor;
                s.dampeners = damp != 0;
                haveShip = true;
            }
            else if (sscanf(line, "fuel %lf", &p.fuel) == 1) {}
            else if (sscanf(line, "credits %lf", &p.credits) == 1) {}
            else if (sscanf(line, "marked %lf", &p.marked) == 1) {}
            else if (sscanf(line, "cargo %d %lf", &ore, &t) == 2) { if (version >= 3 && ore >= 0 && ore < GOOD_COUNT) p.cargo[ore] = t; }   // (2: the old goods)
            else if (sscanf(line, "standing %d %lf", &ore, &t) == 2) { if (ore >= 0 && ore < (int)standing.size()) standing[ore] = (float)t; }
            else if (!strncmp(line, "end", 3)) { ended = true; break; }
        }
    fclose(f);
    if (!haveShip || !ended || version < 2 || !world.systems.Exists(p.ship.anchor)) return false;
    out = p;
    return true;
}
