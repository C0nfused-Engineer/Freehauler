// npcs.cpp - flying the other ships
#include "npcs.h"
#include "contracts.h"
#include "economy.h"
#include "format.h"
#include "sensors.h"
#include "shipparts.h"
#include <algorithm>

static const double DISPATCH_WAIT = 90.0;        // a dock's goods wait this long for a ship, unless there's a shipload sooner...
static const double DISPATCH_GAP  = 30.0;        // ...but ships for the same place don't leave closer together than this
static const double SIGHT = 1.5 * AU_U;          // how far pirates and patrols notice things
// (at hyper speeds two ships holding station on each other are still hundreds of km apart: that's alongside)
static const double BOARD_RANGE = 200.0 * KM_U, BOARD_SECONDS = 4.0;       // a captain boards a ship held this close for this long
static const double ROB_RANGE = 1000.0 * KM_U, CATCH_RANGE = 10.0 * KM_U, SCAN_RANGE = 1000.0 * KM_U;

static double Roll(World &w) { w.npcDice = Mix64(w.npcDice + 0x9E3779B97F4A7C15ull); return (double)(w.npcDice >> 11) * (1.0 / 9007199254740992.0); }
static void   Tell(World &w, const Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

// A system ships are flown in, as seen through the captain who's there
struct Live {
    SysId             sys;
    const Captain    *eye;       // (its loaded system says where everything is)
    const StarSystem *star;
    int               hub;       // the system's hub market, or -1: nobody lives here
    DVec3             ly;        // light years from home
};

const char *NpcKindName(NpcKind k)
{
    static const char *names[] = { "Freighter", "Patrol ship", "Raider", "Mining ship", "Tanker" };
    return names[(int)k < (int)NpcKind::Count ? (int)k : 0];
}

const Npc *FindNpc(const World &w, int id)
{
    for (const Npc &n : w.npcs) if (n.id == id) return &n;
    return nullptr;
}

Stance ShipStance(const World &w, const Captain &c, const Npc &n)
{
    float s = c.Standing(n.faction);
    bool onMe = n.prey == c.id && (n.doing == NpcDo::Hunt || n.doing == NpcDo::Inspect);
    if (n.kind == NpcKind::Pirate) {
        if (s >= 50.0f) return STANCE_ALLIED;
        if (s >= 20.0f) return STANCE_NEUTRAL;
        return onMe && !n.demanded ? STANCE_ALERTED : STANCE_HOSTILE;
    }
    (void)w;
    if (onMe) return STANCE_ALERTED;
    if (s < -50.0f) return STANCE_HOSTILE;
    if (s >= 50.0f || (c.Papers(n.faction) & PAPER_MEMBER)) return STANCE_ALLIED;
    return STANCE_NEUTRAL;
}

Stance TrafficStance(const World &w, const Captain &c, const Traffic &t)
{
    if (!t.npc) return STANCE_NEUTRAL;
    const Npc *n = FindNpc(w, t.id);
    return n ? ShipStance(w, c, *n) : STANCE_NEUTRAL;
}

// ---------------------------------------------------------------------------
// Where things are
// ---------------------------------------------------------------------------
static const Live *LiveAt(const std::vector<Live> &live, SysId sys)
{
    for (const Live &l : live) if (l.sys == sys) return &l;
    return nullptr;
}

static const Body *BodyOf(const World &w, const Live &l, int market)
{
    if (market < 0 || market >= w.econ.Count()) return nullptr;
    const MarketDef &d = w.econ.base->defs[market];
    if (d.sys != l.sys || d.body < 0 || d.body >= (int)l.eye->loaded.bodies.size()) return nullptr;
    return &l.eye->loaded.bodies[d.body];
}

static double Standoff(const Body &b) { return b.kind == Kind::Station ? b.radius + 2.5 * KM_U : b.radius * 1.4; }

static DVec3 RandomDir(World &w)
{
    for (;;) {
        DVec3 d = { Roll(w) * 2.0 - 1.0, Roll(w) * 2.0 - 1.0, Roll(w) * 2.0 - 1.0 };
        double l = Len(d);
        if (l > 0.1 && l <= 1.0) return d * (1.0 / l);
    }
}

// Beside a body, off to one side
static DVec3 Beside(World &w, const Body &b) { return b.local + RandomDir(w) * (Standoff(b) * (1.0 + 0.3 * Roll(w))); }

// Where ships to and from another system leave the gravity field: out of the plane, on the side the other system is
static DVec3 JumpPoint(const World &w, const Live &l, SysId other)
{
    const StarSystem &s = *l.star;
    DVec3 d = w.systems.Exists(other) ? Norm(w.systems.Between(l.sys, other)) : s.n;
    double up = Dot(d, s.n);
    d = Norm(d + s.n * (up >= 0.0 ? 1.5 : -1.5));
    double in = Len(d - s.n * Dot(d, s.n)), out = fabs(Dot(d, s.n));
    double k = 1.0 / sqrt(in * in / (s.fieldR * s.fieldR) + out * out / (s.fieldH * s.fieldH));
    return d * (k * 0.97);
}

static Quaternion LookRot(DVec3 dir, DVec3 up)
{
    if (fabs(Dot(dir, up)) > 0.98) up = Norm(DVec3{ up.y, up.z, up.x } + up * 0.3);
    return QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, ToF(dir), ToF(up))));
}

// Head for a point (matching 'goalVel' there), easing off on the way in
// ---------------------------------------------------------------------------
// The autopilot. NPC ships fly by the same rules as a captain's (FlyShip): thrust
// over mass, turning by thrusters, a hyper drive that slows for gravity wells and
// stops short of what it's pointed at, a reactor burning fuel for what's drawn.
// The pilot is this: point the nose at where it's going (around anything in the
// way), hold the drive in until it's there.
// ---------------------------------------------------------------------------
static World      *g_world = nullptr;       // (what's being stepped: set by StepNpc)
static const Live *g_live = nullptr;

// What a ship of each kind is: one of the template builds (shipparts.h), from the same hulls and parts a
// captain could buy. Crews run their drives well inside what they'll do: it's far cheaper on the reactor.
static void Fit(Npc &n, double load)
{
    ShipBuild build = NpcBuild(n.kind, load);
    n.spec = SpecOf(build);
    n.hull = (uint8_t)build.hull;
    n.power.radar = n.kind != NpcKind::Pirate;                     // (raiders don't announce themselves)
    n.tank = n.spec.tank;
    n.fuel = n.tank;
    n.power.bank = n.spec.bankTJ; n.power.driveCap = n.spec.driveCapTJ;
    n.ship.ftl = n.kind == NpcKind::Freighter ? 0.45f : n.kind == NpcKind::Miner ? 0.40f : n.kind == NpcKind::Pirate ? 0.65f
               : n.kind == NpcKind::Patrol ? 0.80f : 0.70f;
}

// ...and the freighters are their owners' own designs for the job (fleet.h)
static void FitDesign(Npc &n, const ShipDesign &d)
{
    n.spec = d.spec;
    n.hull = (uint8_t)d.build.hull;
    n.power.radar = true;
    n.tank = n.spec.tank;
    n.fuel = n.tank;
    n.power.bank = n.spec.bankTJ; n.power.driveCap = n.spec.driveCapTJ;
    n.ship.ftl = (float)d.cruise;
}

// Around a port, ships fly on their engines, and no faster than this for each unit they are from it: a freighter
// takes a minute or so to work clear of a station, or to come alongside one, and can be flown up to on the way.
static const double PORT_ZONE = 20000.0 * KM_U, PORT_RATE = 0.12;

// 'body': the place it's bound for (the drive stops short of a body by itself), or null for a point in space
// (it stops 500 m off). 'goalVel': how the goal is moving (a chase flies the quarry's course plus whatever closes
// the gap, not straight at where it is). Sets n.arrived.
static void Auto(Npc &n, DVec3 goal, const Body *body, DVec3 goalVel, float dt)
{
    World &w = *g_world;
    const Live &l = *g_live;
    Ship &s = n.ship;
    s.anchor = l.sys; s.pos = n.pos; s.vel = ToF(n.vel); s.rot = n.rot;

    DVec3  rel = goal - n.pos;
    double dist = Len(rel), stop = body ? StopDistance(*body) : STOP_LOCK;
    bool   there = dist <= stop * 1.03 + 60.0;
    bool   chase = !body && Len(goalVel) > 1.0;
    DVec3  aim = chase ? goalVel + rel * WELL_K : rel;
    bool   detour = false;
    if (!there && dist > 1e-6) {
        // Something in the way? Go round it: aim past its edge, on the side the course already leans to
        DVec3  dir = rel * (1.0 / dist);
        double nearest = dist;
        for (const Body &b : l.eye->loaded.bodies) {
            if (&b == body) continue;
            DVec3  to = b.local - n.pos;
            double along = Dot(to, dir), clear = StopDistance(b) * 1.3;
            if (along <= 0.0 || along >= nearest) continue;
            DVec3  off = to - dir * along;                         // from the course line to the body's centre
            double miss = Len(off);
            if (miss >= clear) continue;
            DVec3 side = miss > 1e-3 * clear ? off * (-1.0 / miss) : Norm(Cross(dir, l.star->n) + l.star->n * 0.3);
            aim = to + side * (clear * 1.25);
            nearest = along; detour = true;
        }
    }
    // Leaving port, or coming into one: on the engines, gently
    bool   gentle = false;
    double pace = 1e30;
    if (n.leftClear > 0.0) {
        double out = Len(n.pos - n.left) - n.leftClear;
        if (out < PORT_ZONE) { gentle = true; pace = 60.0 + PORT_RATE * fmax(0.0, out); } else n.leftClear = 0.0;
    }
    if (body && dist - stop < PORT_ZONE && !detour) { gentle = true; pace = fmin(pace, 20.0 + PORT_RATE * fmax(0.0, dist - stop)); }
    if (there) gentle = false;

    // Swing the nose onto it, as fast as the thrusters allow
    double  mass = n.spec.dryMass + n.fuel + n.total;
    Vector3 fwd = ShipForward(s), want = Len(aim) > 1e-6 ? ToF(Norm(aim)) : fwd;
    // (measured by the cross product: at hyper speeds a hundredth of a degree off course is thousands of km a second)
    float   angle = atan2f(Vector3Length(Vector3CrossProduct(fwd, want)), Vector3DotProduct(fwd, want)) * RAD2DEG;
    if (angle > 0.0f && !there) {
        float      turn = fminf(angle, (float)(n.spec.turn / mass * dt));
        Quaternion full = QuaternionFromVector3ToVector3(fwd, want);
        s.rot = QuaternionNormalize(QuaternionMultiply(turn >= angle ? full : QuaternionSlerp(QuaternionIdentity(), full, turn / angle), s.rot));
        angle -= turn;
    }
    Controls in;
    in.hyper = !there && angle < 3.0f;
    LockInfo lock;
    lock.rel = rel;
    if (chase) lock.speed = fmax(0.0, Len(aim) - WELL_K * fmax(0.0, dist - STOP_LOCK));       // (so the drive's limit is the speed of that course)
    FlightReport r;
    if (gentle) {
        // Thrust toward the pace wanted along the course, as hard as the engines can change the ship's velocity
        DVec3  wantVel = Len(aim) > 1e-6 ? Norm(aim) * pace : DVec3{};
        DVec3  change = wantVel - ToD(s.vel);
        double most = THRUST_K * n.spec.thrust / mass * dt, ask = Len(change);
        if (ask > most) change = change * (most / ask);
        s.vel = ToF(ToD(s.vel) + change);
        s.pos = s.pos + ToD(s.vel) * (double)dt;
        n.power.engaged = n.power.warping = false;
        double engineTW = n.spec.engineTW * fmin(1.0, ask / fmax(most, 1e-9));
        double load = n.spec.idleTW + (RadarOn(n.spec, n.power) ? n.spec.radarTW : 0.0) + engineTW;
        double burn = ReactorBurnFor(n.spec, load);
        n.fuel = fmax(0.0, n.fuel - burn * dt);
        n.power.heat = (float)(fmax(0.0, burn * n.spec.fuelTJ - load) + PLUME_HEAT * engineTW);
    } else {
        r = FlyShip(s, n.spec, n.power, n.fuel, mass, in, dt, w.systems, l.eye->loaded, !body && !detour && !there ? &lock : nullptr);
    }
    if (n.fuel < 0.15 * n.tank) n.fuel = n.tank;                   // (NPC crews top up as they go: their fuel isn't counted against anyone's stock yet)
    // Bodies are solid for them too: stopped at the surface, sliding along it
    for (const Body &b : l.eye->loaded.bodies) {
        DVec3  away = s.pos - b.local;
        double d = Len(away), least = b.radius + 20.0;
        if (d >= least || d < 1e-9) continue;
        DVec3 out = away * (1.0 / d);
        s.pos = s.pos + out * (least - d);
        float into = Vector3DotProduct(s.vel, ToF(out));
        if (into < 0.0f) s.vel = Vector3Subtract(s.vel, Vector3Scale(ToF(out), into));
    }
    n.pos = s.pos; n.vel = ToD(s.vel); n.rot = s.rot;
    n.arrived = there || (r.drive.holding && !detour);
}

static void Fly(Npc &n, DVec3 goal, DVec3 goalVel, float dt) { Auto(n, goal, nullptr, goalVel, dt); }
static void FlyTo(Npc &n, const Body &b, float dt)           { Auto(n, b.local, &b, {}, dt); }
static void Hold(Npc &n, float dt)                           { Auto(n, n.pos, nullptr, {}, dt); }

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
static std::string ShipName(World &w, NpcKind kind, const Faction *f)
{
    static const char *first[]  = { "Patient", "Long", "Lucky", "Iron", "Quiet", "Golden", "Stubborn", "Far", "Honest", "Second", "Low", "Bright",
                                    "Old", "Slow", "Grey", "Steady", "Late", "Plain", "Rusty", "Kind" };
    static const char *second[] = { "Mule", "Haul", "Margin", "Harvest", "Tide", "Ledger", "Lantern", "Promise", "Ox", "Wind", "Bargain", "Furrow",
                                    "Anvil", "Pilgrim", "Reckoning", "Burden", "Meridian", "Compass", "Chance", "Shepherd" };
    int n = 100 + (int)(Roll(w) * 900.0);
    switch (kind) {
    case NpcKind::Patrol: return (f ? f->name : std::string("System")) + TextFormat(" patrol %d", n);
    case NpcKind::Pirate: return (f ? f->name : std::string("Pirate")) + TextFormat(" raider %d", n % 100);
    case NpcKind::Miner:  return TextFormat("Prospector %d", n);
    default:              return std::string(first[(int)(Roll(w) * 20.0) % 20]) + " " + second[(int)(Roll(w) * 20.0) % 20];
    }
}

static Npc &Spawn(World &w, NpcKind kind, const Live &l, DVec3 pos, int faction)
{
    Npc n;
    n.id = NPC_ID_BASE + (++w.npcSeq);
    n.kind = kind; n.sys = l.sys; n.pos = pos; n.faction = faction;
    n.name = ShipName(w, kind, w.systems.factions.Get(faction));
    n.rot = LookRot(RandomDir(w), l.star->n);
    n.look = w.clock + Roll(w);
    Fit(n, 0.0);
    w.npcs.push_back(n);
    return w.npcs.back();
}

static int BulkGood(const float *cargo)
{
    int best = -1;
    for (int g = 0; g < GOOD_COUNT; g++) if (cargo[g] > 0.0f && (best < 0 || cargo[g] > cargo[best])) best = g;
    return best;
}

// ---------------------------------------------------------------------------
// Which systems are flown, and settling what no longer is
// ---------------------------------------------------------------------------
static int NearestClanAt(const World &w, DVec3 ly)
{
    int best = NO_FACTION; double bestD = 1e30;
    for (const Faction &f : w.systems.factions.list) {
        if (f.kind != FactionKind::Clan) continue;
        double d = Len(ly - f.seatLy) / fmax(1.0, (double)f.reach);
        if (d < bestD) { bestD = d; best = f.id; }
    }
    return best;
}

static DVec3 LurkPoint(World &w, const Live &l)
{
    const Body *hub = BodyOf(w, l, l.hub);
    std::vector<const Body *> others;
    if (hub)
        for (int i = 0; i < w.econ.Count(); i++)
            if (i != l.hub && w.econ.base->defs[i].hub == l.hub) if (const Body *b = BodyOf(w, l, i)) others.push_back(b);
    if (hub && !others.empty()) {                              // off a lane between two of its markets
        const Body *o = others[(size_t)(Roll(w) * others.size()) % others.size()];
        return hub->local + (o->local - hub->local) * (0.3 + 0.4 * Roll(w)) + RandomDir(w) * (0.05 * AU_U);
    }
    for (const Zone &z : l.eye->loaded.zones) if (!z.ring) return ZoneTargetPoint(z, RandomDir(w) * z.rIn) + RandomDir(w) * (2000.0 * KM_U);
    return RandomDir(w) * (2.0 * AU_U);
}

// The ships a system keeps about it while someone's there to see: its miners, its patrols, and whoever preys on it
static void Populate(World &w, const Live &l)
{
    const EconBase &base = *w.econ.base;
    const FactionSet &fs = w.systems.factions;
    int patrols = 0, pirates = 0;
    std::vector<int> minersAt;
    for (const Npc &n : w.npcs) {
        if (n.sys != l.sys) continue;
        if (n.kind == NpcKind::Patrol && n.post < 0) patrols++;
        if (n.kind == NpcKind::Pirate) pirates++;
        if (n.kind == NpcKind::Miner) minersAt.push_back(n.from);
    }
    double traffic = l.hub >= 0 ? base.defs[l.hub].traffic : 0.0;
    std::vector<int> markets;
    for (int i = 0; i < w.econ.Count(); i++) if (l.hub >= 0 && base.defs[i].hub == l.hub) markets.push_back(i);

    // Miners and skimmers: wherever raw materials are produced
    int miners = (int)minersAt.size();
    for (int m : markets) {
        const MarketDef &d = base.defs[m];
        bool digs = false;
        for (int g = 0; g <= RAW_HELIUM3; g++) if (w.econ.state[m].out[g] > 0.0f) digs = true;
        const Body *b = BodyOf(w, l, m);
        if (!digs || !b) continue;
        int want = std::max(1, std::min(3, (int)(d.scale / 80.0f + 0.5f)));
        int have = (int)std::count(minersAt.begin(), minersAt.end(), m);
        for (; have < want && miners < 10; have++, miners++) {
            Npc &n = Spawn(w, NpcKind::Miner, l, Beside(w, *b), w.econ.state[m].owner);
            n.from = n.to = m;
            n.doing = NpcDo::Dock; n.until = w.clock + 5.0 + 60.0 * Roll(w);       // (they don't all set out at once)
        }
    }
    // Patrols: the ruler's, by how heavily it polices and how much there is to guard
    const Faction *ruler = fs.Get(l.star->ruler);
    int wantPatrols = 0;
    if (ruler && !markets.empty()) {
        wantPatrols = (int)(ruler->policing * traffic / 4000.0 + 0.5);
        if (wantPatrols == 0 && ruler->policing * traffic > 150.0) wantPatrols = 1;
        wantPatrols = std::min(wantPatrols, 5);
    }
    for (; patrols < wantPatrols; patrols++) {
        const Body *b = BodyOf(w, l, markets[(size_t)(Roll(w) * markets.size()) % markets.size()]);
        if (!b) break;
        Npc &n = Spawn(w, NpcKind::Patrol, l, Beside(w, *b), ruler->id);
        n.doing = NpcDo::Dock; n.until = w.clock + 20.0 * Roll(w);
    }
    // Pickets: warships stationed at each of the state's sensor outposts. They keep close to it, and see off whatever comes for it.
    for (int bi = 0; bi < (int)l.eye->loaded.bodies.size() && ruler; bi++) {
        const Body &b = l.eye->loaded.bodies[bi];
        if (b.settlement != Settlement::SensorOutpost) continue;
        int have = 0;
        for (const Npc &n : w.npcs) if (n.sys == l.sys && n.kind == NpcKind::Patrol && n.post == bi) have++;
        for (; have < OUTPOST_PICKETS; have++) {
            Npc &n = Spawn(w, NpcKind::Patrol, l, Beside(w, b), ruler->id);
            n.post = bi;
            n.name = ruler->name + TextFormat(" picket %d", 100 + (int)(Roll(w) * 900.0));
            n.doing = NpcDo::Dock; n.until = w.clock + 20.0 * Roll(w);
        }
    }
    // Pirates: by how hard the clans are raiding hereabouts
    double raid = RaidLevel(w, l.ly);
    int wantPirates = (int)(raid * fmin(4.0, 1.0 + traffic / 3000.0) + 0.35);
    if (markets.empty()) wantPirates = raid > 0.25 ? 1 : 0;
    int clan = NearestClanAt(w, l.ly);
    for (; pirates < wantPirates && clan != NO_FACTION; pirates++) {
        Npc &n = Spawn(w, NpcKind::Pirate, l, LurkPoint(w, l), clan);
        n.doing = NpcDo::Lurk; n.goal = n.pos;
    }
}

static void Rezone(World &w, std::vector<Live> &live)
{
    Economy &e = w.econ;
    const EconBase &base = *e.base;
    const int N = e.Count();
    e.zone.assign(N, 0);
    for (int i = 0; i < N; i++)
        for (Live &l : live) if (base.defs[i].sys == l.sys) { e.zone[i] = 2; l.hub = base.defs[i].hub; }
    for (const Lane &l : base.lanes) {
        if (e.zone[l.a] == 2 && e.zone[l.b] == 0) e.zone[l.b] = 1;
        if (e.zone[l.b] == 2 && e.zone[l.a] == 0) e.zone[l.a] = 1;
    }
    // Out of sight: what was being carried arrives; whoever was hanging about is gone
    for (size_t i = 0; i < w.npcs.size();) {
        Npc &n = w.npcs[i];
        if (LiveAt(live, n.sys)) { i++; continue; }
        int dest = n.kind == NpcKind::Miner ? n.from : n.to;
        if (dest >= 0 && n.total > 0.0f) e.Land(dest, n.cargo);
        if (n.credits > 0.0 && n.to >= 0) LandSpecie(e, n.to, n.payee, n.credits);
        w.npcs.erase(w.npcs.begin() + i);
    }
    for (size_t i = 0; i < e.waiting.size();) {
        const Shipment &s = e.waiting[i];
        bool same = base.defs[s.from].hub == base.defs[s.to].hub;
        if (same ? e.zone[s.from] == 2 : (e.zone[s.from] || e.zone[s.to])) { i++; continue; }
        e.Land(s.to, s.cargo);
        e.waiting.erase(e.waiting.begin() + i);
    }
    for (size_t i = 0; i < w.transits.size();) {
        const Transit &t = w.transits[i];
        if (e.zone[t.from] || e.zone[t.to]) { i++; continue; }
        e.Land(t.to, t.cargo);
        LandSpecie(e, t.to, t.payee, t.credits);
        w.transits.erase(w.transits.begin() + i);
    }
    for (size_t i = 0; i < e.specie.size();) {
        const Specie &s = e.specie[i];
        if (e.zone[s.from] || e.zone[s.to]) { i++; continue; }
        LandSpecie(e, s.to, s.faction, s.credits);
        e.specie.erase(e.specie.begin() + i);
    }
    for (int i = 0; i < N; i++) {
        if (e.zone[i] == 2) continue;
        float *y = &e.yield[(size_t)i * GOOD_COUNT];
        bool any = false;
        for (int g = 0; g <= RAW_HELIUM3; g++) if (y[g] > 0.0f) any = true;
        if (!any) continue;
        e.Land(i, y);
        for (int g = 0; g < GOOD_COUNT; g++) y[g] = 0.0f;
    }
    w.liveSystems.clear();
    for (const Live &l : live) { w.liveSystems.push_back(l.sys); Populate(w, l); }
    w.trafficZoned = w.clock;
}

// ---------------------------------------------------------------------------
// Freight: from the docks into ships, and lane ships arriving
// ---------------------------------------------------------------------------
static double LaneTime(const MarketDef &a, const MarketDef &b) { return 20.0 + 6.0 * Len(a.ly - b.ly); }
static const int SHIPS_A_GO = 3;                 // the most ships that cast off from one dock for one place together

// The job a load is: about the system, or between systems dry or in tanks
static int RoleFor(const MarketDef &a, const MarketDef &b, const float *cargo, float total)
{
    if (a.hub == b.hub) return ROLE_HAULER;
    float tanked = 0.0f;
    for (int g = 0; g < GOOD_COUNT; g++) if (GoesByTanker(g)) tanked += cargo[g];
    return tanked > 0.5f * total ? ROLE_TANKER : ROLE_BULK;
}

// Whose ship takes it: one of those based at the hub it leaves from, by how many each owner has there
static int ShipOwner(World &w, int from, int role)
{
    const Economy &e = w.econ;
    int hub = e.base->defs[from].hub, owner = NO_FACTION;
    if (hub < 0 || hub >= (int)e.fleets.size()) return owner;
    double all = 0.0;
    for (const FleetAt &f : e.fleets[hub]) all += f.ships[role];
    double pick = Roll(w) * all;
    for (const FleetAt &f : e.fleets[hub]) {
        if (f.ships[role] <= 0.0f) continue;
        owner = f.owner; pick -= f.ships[role];
        if (pick <= 0.0) break;
    }
    return owner;
}

static void Dispatch(World &w, const std::vector<Live> &live)
{
    Economy &e = w.econ;
    const EconBase &base = *e.base;
    // Money on its way home (econ.h): a paymaster's ship for each run, one of its owner's own
    for (const Specie &s : e.specie) {
        const MarketDef &a = base.defs[s.from], &b = base.defs[s.to];
        const Live *here = LiveAt(live, a.sys);
        const Body *dock = here ? BodyOf(w, *here, s.from) : nullptr;
        std::string name = w.systems.factions.Name(s.faction) + TextFormat(" paymaster %d", 10 + (int)(Roll(w) * 90.0));
        if (dock) {
            Npc &n = Spawn(w, NpcKind::Freighter, *here, Beside(w, *dock), s.faction);
            n.name = name; n.from = s.from; n.to = s.to; n.total = 0.0f; n.credits = s.credits; n.payee = s.faction;
            FitDesign(n, FleetDesign(base, s.faction, a.hub == b.hub ? ROLE_HAULER : ROLE_TANKER));
            n.jumping = a.hub != b.hub;
            n.doing = NpcDo::Dock; n.until = w.clock + 10.0 + 15.0 * Roll(w);       // casting off
            n.left = dock->local; n.leftClear = dock->radius;
        } else {
            Transit t;
            t.from = s.from; t.to = s.to; t.faction = s.faction; t.credits = s.credits; t.payee = s.faction;
            t.arrive = w.clock + LaneTime(a, b) + 40.0;
            t.name = name;
            w.transits.push_back(t);
        }
    }
    e.specie.clear();
    for (size_t i = 0; i < e.waiting.size();) {
        Shipment &s = e.waiting[i];
        const MarketDef &a = base.defs[s.from], &b = base.defs[s.to];
        double shipload = fmax(40.0, fmin(4000.0, 0.4 * a.scale));
        double waited = w.clock - s.since;
        if (s.total <= 0.0f) {                                     // (an idle dock is remembered for a while: see DISPATCH_GAP)
            if (waited > 300.0 || waited < 0.0) e.waiting.erase(e.waiting.begin() + i); else i++;
            continue;
        }
        if (waited >= 0.0 && waited < (s.total >= shipload ? DISPATCH_GAP : DISPATCH_WAIT)) { i++; continue; }
        const Live *here = LiveAt(live, a.sys);
        const Body *dock = here ? BodyOf(w, *here, s.from) : nullptr;
        if (s.total < 2.0f) {
            e.Land(s.to, s.cargo);                                 // (not worth a ship: it goes in someone's spare corner)
            for (int g = 0; g < GOOD_COUNT; g++) s.cargo[g] = 0.0f;
            s.total = 0.0f; s.since = w.clock;
            i++;
            continue;
        }
        // Into ships of the fleet based here: as many as it takes (a few at a time), each fuelled from the port's
        // stock for the run (or the far end's, if this one has none to spare). No fuel, and it waits on the dock.
        const int role = RoleFor(a, b, s.cargo, s.total);
        for (int ship = 0; ship < SHIPS_A_GO && s.total >= 2.0f; ship++) {
            int owner = ShipOwner(w, s.from, role);
            const ShipDesign *d = &FleetDesign(base, owner, role);
            if (role == ROLE_HAULER && s.total > d->cargo && FleetDesign(base, owner, ROLE_BULK).cargo > d->cargo) d = &FleetDesign(base, owner, ROLE_BULK);
            // (a part load between systems goes by tramp, not in a quarter-full bulk carrier)
            if (role != ROLE_HAULER && s.total < 0.25 * d->cargo && FleetDesign(base, NO_FACTION, role).cargo < d->cargo) { owner = NO_FACTION; d = &FleetDesign(base, owner, role); }
            // (what the run burns goes by what the ship weighs, and it's lighter part loaded)
            double fuel = (role == ROLE_HAULER ? d->localFuel : d->Fuel(Len(a.ly - b.ly))) * (d->mass - d->cargo + fmin((double)s.total, d->cargo)) / d->mass;
            if (!Bunker(e, s.from, d->fuelGood, fuel) && !Bunker(e, s.to, d->fuelGood, fuel)) break;
            float share = d->cargo < s.total ? (float)(d->cargo / s.total) : 1.0f, load[GOOD_COUNT], total = 0.0f;
            for (int g = 0; g < GOOD_COUNT; g++) { load[g] = s.cargo[g] * share; total += load[g]; s.cargo[g] -= load[g]; }
            s.total = share < 1.0f ? s.total - total : 0.0f;
            if (dock) {
                Npc &n = Spawn(w, NpcKind::Freighter, *here, Beside(w, *dock), owner);
                n.from = s.from; n.to = s.to; n.total = total;
                for (int g = 0; g < GOOD_COUNT; g++) n.cargo[g] = load[g];
                FitDesign(n, *d);
                n.jumping = a.hub != b.hub;
                n.doing = NpcDo::Dock; n.until = w.clock + 10.0 + 15.0 * Roll(w) + 8.0 * ship;       // casting off
                n.left = dock->local; n.leftClear = dock->radius;
            } else {
                Transit t;
                t.from = s.from; t.to = s.to; t.total = total; t.faction = owner;
                for (int g = 0; g < GOOD_COUNT; g++) t.cargo[g] = load[g];
                t.arrive = w.clock + LaneTime(a, b) + 40.0 + 8.0 * ship;
                t.name = ShipName(w, NpcKind::Freighter, w.systems.factions.Get(owner));
                w.transits.push_back(t);
            }
            s.since = w.clock;
        }
        if (s.total < 2.0f && s.total > 0.0f) { e.Land(s.to, s.cargo); for (int g = 0; g < GOOD_COUNT; g++) s.cargo[g] = 0.0f; s.total = 0.0f; }     // (the sweepings)
        i++;
    }
    for (size_t i = 0; i < w.transits.size();) {
        Transit &t = w.transits[i];
        if (w.clock < t.arrive) { i++; continue; }
        const MarketDef &a = base.defs[t.from], &b = base.defs[t.to];
        const Live *here = LiveAt(live, b.sys);
        if (here && BodyOf(w, *here, t.to)) {                      // drops out of warp at the edge, inbound
            Npc &n = Spawn(w, NpcKind::Freighter, *here, JumpPoint(w, *here, a.sys) + RandomDir(w) * (5000.0 * KM_U), t.faction);
            n.name = t.name;
            n.from = t.from; n.to = t.to; n.total = t.total; n.credits = t.credits; n.payee = t.payee;
            for (int g = 0; g < GOOD_COUNT; g++) n.cargo[g] = t.cargo[g];
            const ShipDesign &d = FleetDesign(base, t.faction, RoleFor(a, b, t.cargo, t.total));
            if (t.total <= d.cargo * 1.01) FitDesign(n, d); else Fit(n, n.total);       // (a load from before there were fleets: whatever will carry it)
            w.bursts.push_back({ n.sys, n.pos, n.spec.jumpTJ * (n.spec.dryMass + n.total), n.id });
        } else {
            e.Land(t.to, t.cargo);
            LandSpecie(e, t.to, t.payee, t.credits);
        }
        w.transits.erase(w.transits.begin() + i);
    }
}

// ---------------------------------------------------------------------------
// One ship, one step
// ---------------------------------------------------------------------------
static Npc *NpcById(World &w, int id)
{
    for (Npc &n : w.npcs) if (n.id == id) return &n;
    return nullptr;
}

static Captain *CaptainById(Captain *const *caps, int count, int id, SysId sys)
{
    for (int i = 0; i < count; i++) if (caps[i]->id == id && caps[i]->loaded.index == sys) return caps[i];
    return nullptr;
}

// Docked somewhere that keeps the peace: pirates don't follow you in
static bool InPort(const World &w, const Captain &c)
{
    int near = c.flight.nearBody;
    if (near < 0 || near >= (int)c.loaded.bodies.size() || c.loaded.bodies[near].population <= 0) return false;
    const Faction *owner = w.systems.factions.Get(c.loaded.bodies[near].owner);
    return owner && owner->kind != FactionKind::Clan;
}

// Takes a share of a hold, the dearest goods first; returns the tonnes taken
static double Plunder(double *cargo, double share)
{
    double total = 0.0;
    for (int g = 0; g < GOOD_COUNT; g++) total += cargo[g];
    double want = total * share, took = 0.0;
    std::vector<int> order;
    for (int g = 0; g < GOOD_COUNT; g++) if (cargo[g] > 0.0) order.push_back(g);
    std::sort(order.begin(), order.end(), [](int a, int b) { return GoodData(a).basePrice > GoodData(b).basePrice; });
    for (int g : order) {
        double q = fmin(cargo[g], want - took);
        cargo[g] -= q; took += q;
        if (cargo[g] < 1e-6) cargo[g] = 0.0;
        if (took >= want - 1e-9) break;
    }
    return took;
}

static void News(World &w, int faction, SysId where, const std::string &text)
{
    if (w.clock - w.trafficNews < 240.0 && w.clock >= w.trafficNews) return;      // (not every scuffle makes the news)
    w.trafficNews = w.clock;
    PostNews(w, faction, where, text);
}

static void SlinkOff(World &w, Npc &n, const Live &l)
{
    n.doing = NpcDo::Leave; n.prey = -1; n.close = 0.0; n.demanded = false;
    n.goal = JumpPoint(w, l, NO_SYSTEM) + RandomDir(w) * (0.2 * AU_U);
}

// ---------------------------------------------------------------------------
// What a crew knows (sensors.h)
// ---------------------------------------------------------------------------
// Does this ship know where that one is? By its own sensors; and a patrol has its system's traffic picture too.
// 'held': it had it a moment ago (keeping a contact is easier than finding one).
static bool Holds(const World &w, const Npc &n, int id, DVec3 at, const Signature &sig, bool held)
{
    if (Sense(FitOf(n.spec, n.power), sig, Len(at - n.pos), held ? HOLD_GAIN : 1.0) > CONTACT_NONE) return true;
    if (n.kind != NpcKind::Patrol) return false;
    auto sys = w.picture.find(n.sys);
    if (sys == w.picture.end()) return false;
    auto it = sys->second.find(id);
    return it != sys->second.end() && it->second > CONTACT_NONE;
}

bool NpcHolds(const World &w, const Npc &n, const Captain &c)
{
    return c.loaded.index == n.sys && Holds(w, n, c.id, ShipLocal(w, c), SignatureOf(c.player.spec, c.player.power), false);
}

static bool HoldsNpc(const World &w, const Npc &n, const Npc &p) { return Holds(w, n, p.id, p.pos, SignatureOf(p.spec, p.power), false); }

// A chase: where to fly. At the quarry while it's held; once it's lost, at where it was last seen, and after a
// while of that (false) nowhere.
static bool Chase(World &w, Npc &n, int id, DVec3 at, DVec3 vel, const Signature &sig, DVec3 &to, DVec3 &toVel, bool &held)
{
    held = Holds(w, n, id, at, sig, true);
    if (held) { n.seenAt = w.clock; n.lastPos = at; to = at; toVel = vel; return true; }
    to = n.lastPos; toVel = {};
    return w.clock - n.seenAt < LOSE_AFTER && w.clock >= n.seenAt;
}

// Returns false when the ship is done with (docked for good, jumped, run off)
static bool StepNpc(World &w, Npc &n, const Live &l, Captain *const *caps, int count, float dt)
{
    Economy &e = w.econ;
    const EconBase &base = *e.base;
    const double now = w.clock;
    g_world = &w; g_live = &l;
    if (n.tank <= 0.0) Fit(n, n.total);                          // (a ship put there by hand)

    switch (n.kind) {
    // ---------------- Freighters: there, and done ----------------
    case NpcKind::Freighter: {
        // A paymaster's ship, and a captain alongside who means to have what it carries: held there long enough, they do
        if (n.credits > 0.0)
            for (int k = 0; k < count; k++) {
                Captain &c = *caps[k];
                if (c.boardTarget != n.id || c.loaded.index != n.sys) continue;
                c.boardTime = n.doing != NpcDo::Dock && Len(ShipLocal(w, c) - n.pos) < BOARD_RANGE ? c.boardTime + dt : 0.0;
                if (c.boardTime < BOARD_SECONDS) continue;
                const FactionSet &fs = w.systems.factions;
                c.player.marked += n.credits;
                if ((int)c.standing.size() < fs.Count()) c.standing.resize(fs.Count(), 0.0f);
                if (n.payee >= 0 && n.payee < fs.Count()) c.standing[n.payee] = fmaxf(-100.0f, c.standing[n.payee] - 35.0f);
                Tell(w, c, TextFormat("Boarded the %s: %s cr in marked credits brought across. %s won't forget it", n.name.c_str(), Thousands(n.credits).c_str(), fs.Name(n.payee).c_str()));
                News(w, n.payee, n.sys, TextFormat("The %s has been boarded in the %s system and robbed of %s cr", n.name.c_str(), l.star->name.c_str(), Thousands(n.credits).c_str()));
                n.credits = 0.0;
                c.boardTarget = -1; c.boardTime = 0.0;
                break;
            }
        if (n.doing == NpcDo::Dock) {                              // casting off, or tied up and unloading
            if (now < n.until) return true;
            if (n.close > 0.0) { e.Land(n.to, n.cargo); LandSpecie(e, n.to, n.payee, n.credits); return false; }               // (unloaded)
            n.doing = NpcDo::Travel;
        }
        if (n.jumping) {
            DVec3 out = JumpPoint(w, l, base.defs[n.to].sys);
            Fly(n, out, {}, dt);
            if (!n.arrived) return true;
            Transit t;                                             // gone to warp: it arrives when it arrives
            t.from = n.from; t.to = n.to; t.total = n.total; t.faction = n.faction; t.name = n.name; t.credits = n.credits; t.payee = n.payee;
            for (int g = 0; g < GOOD_COUNT; g++) t.cargo[g] = n.cargo[g];
            t.arrive = now + LaneTime(base.defs[n.from], base.defs[n.to]);
            w.transits.push_back(t);
            return false;
        }
        const Body *dest = BodyOf(w, l, n.to);
        if (!dest) { e.Land(n.to, n.cargo); LandSpecie(e, n.to, n.payee, n.credits); return false; }
        FlyTo(n, *dest, dt);
        if (n.arrived) { n.doing = NpcDo::Dock; n.until = now + 30.0 + 30.0 * Roll(w); n.close = 1.0; n.vel = {}; n.fuel = n.tank; }       // tied up, unloading
        return true;
    }
    // ---------------- Miners: out to the rock, back with the day's ore ----------------
    case NpcKind::Miner: {
        const Body *home = BodyOf(w, l, n.from);
        if (!home) return false;
        if (n.doing == NpcDo::Dock) {
            if (now < n.until) return true;
            // Where the work is: a belt if the system has one, the giant it skims, or just off its own world
            bool skims = e.state[n.from].out[RAW_HELIUM3] > 0.0f && home->parent >= 0 && home->parent < (int)l.eye->loaded.bodies.size() &&
                         l.eye->loaded.bodies[home->parent].giant;
            n.goal = home->local + RandomDir(w) * (fmax(home->radius * 4.0, 30000.0 * KM_U));
            if (skims) { const Body &g = l.eye->loaded.bodies[home->parent]; n.goal = g.local + RandomDir(w) * (g.radius * 1.3); }
            else for (const Zone &z : l.eye->loaded.zones)
                if (!z.ring) { n.goal = ZoneTargetPoint(z, home->local) + RandomDir(w) * (3000.0 * KM_U); break; }
            n.doing = NpcDo::Travel;
            n.left = home->local; n.leftClear = home->radius;
        }
        if (n.doing == NpcDo::Travel) {
            Fly(n, n.goal, {}, dt);
            if (n.arrived) { n.doing = NpcDo::Work; n.until = now + 50.0 + 50.0 * Roll(w); }
            return true;
        }
        if (n.doing == NpcDo::Work) {
            Hold(n, dt);
            if (now < n.until) return true;
            // What its home has dug since the last load came in is in this hold now
            float *y = &e.yield[(size_t)n.from * GOOD_COUNT];
            for (int g = 0; g < GOOD_COUNT; g++) { n.cargo[g] += y[g]; n.total += y[g]; y[g] = 0.0f; }
            n.doing = NpcDo::Leave;                                // (homeward)
        }
        FlyTo(n, *home, dt);
        if (n.arrived) {
            n.fuel = n.tank;
            e.Land(n.from, n.cargo);
            for (int g = 0; g < GOOD_COUNT; g++) n.cargo[g] = 0.0f;
            n.total = 0.0f; n.vel = {};
            n.doing = NpcDo::Dock; n.until = now + 15.0 + 20.0 * Roll(w);
        }
        return true;
    }
    // ---------------- Patrols ----------------
    case NpcKind::Patrol: {
        const Faction *law = w.systems.factions.Get(n.faction);
        if (n.doing == NpcDo::Hunt) {
            Npc *p = NpcById(w, n.prey);
            DVec3 to, toVel;
            bool  held = false;
            if (!p || p->sys != n.sys || !Chase(w, n, p->id, p->was, p->vel, SignatureOf(p->spec, p->power), to, toVel, held)) {
                n.doing = NpcDo::Dock; n.until = now; n.prey = -1;     // (gone, or lost)
                return true;
            }
            n.close = held && Len(p->was - n.was) < 50.0 * KM_U ? n.close + dt : 0.0;
            Fly(n, to, toVel, dt);
            if (n.close > 2.0) {                                   // seen off: it runs for it, and its clan thinks twice
                if (p->faction >= 0 && p->faction < (int)e.raid.size()) e.raid[p->faction] = fmaxf(0.05f, e.raid[p->faction] - 0.02f);
                News(w, n.faction, n.sys, (law ? law->name : std::string("A")) + " patrol has run off raiders in the " + l.star->name + " system");
                SlinkOff(w, *p, l);
                n.doing = NpcDo::Dock; n.until = now; n.prey = -1; n.close = 0.0;
            }
            return true;
        }
        if (n.doing == NpcDo::Inspect) {
            Captain *c = CaptainById(caps, count, n.prey, n.sys);
            DVec3 at = c ? ShipLocal(w, *c) : DVec3{}, to, toVel;
            bool  held = false;
            if (!c || now - n.since > 240.0 ||
                !Chase(w, n, c->id, at, ToD(c->player.ship.vel), SignatureOf(c->player.spec, c->player.power), to, toVel, held)) {
                if (c && !held && now - n.since <= 240.0) Tell(w, *c, n.name + " has lost track of you");
                n.doing = NpcDo::Dock; n.until = now; n.prey = -1;
                return true;
            }
            Fly(n, to, toVel, dt);
            n.close = held && Len(at - n.pos) < SCAN_RANGE ? n.close + dt : 0.0;
            if (n.close > 3.0) {
                if (!SearchHold(w, *c, n.faction, n.name)) Tell(w, *c, n.name + " scanned your hold: all in order");
                c->scanSafeUntil = now + SCAN_PEACE;
                n.doing = NpcDo::Dock; n.until = now + 5.0; n.prey = -1; n.close = 0.0;
            }
            return true;
        }
        // On its beat: from one place to the next, with an eye out
        if (now >= n.look) {
            n.look = now + 1.0;
            for (Npc &p : w.npcs)
                if (p.sys == n.sys && p.kind == NpcKind::Pirate && p.doing != NpcDo::Leave && HoldsNpc(w, n, p)) {
                    n.doing = NpcDo::Hunt; n.prey = p.id; n.close = 0.0; n.seenAt = now; n.lastPos = p.pos;
                    return true;
                }
            for (int i = 0; i < count && law; i++) {
                Captain &c = *caps[i];
                if (c.loaded.index != n.sys || now < c.scanSafeUntil || Len(ShipLocal(w, c) - n.pos) > 0.5 * SIGHT || !NpcHolds(w, n, c)) continue;
                bool taken = false;                                // (one patrol at a time)
                for (const Npc &o : w.npcs) if (o.doing == NpcDo::Inspect && o.prey == c.id) taken = true;
                double interest = 0.04 + 0.08 * law->policing + (c.Standing(n.faction) < -20.0f ? 0.3 : 0.0);
                if (taken || Roll(w) > interest) continue;
                n.doing = NpcDo::Inspect; n.prey = c.id; n.close = 0.0; n.since = now; n.seenAt = now; n.lastPos = ShipLocal(w, c);
                Tell(w, c, n.name + ": hold your course and stand by to be scanned");
                return true;
            }
        }
        if (n.doing == NpcDo::Dock) {
            Hold(n, dt);
            if (now < n.until) return true;
            if (n.post >= 0) {                                     // a picket: another station off its outpost, never far from it
                if (n.post >= (int)l.eye->loaded.bodies.size()) return false;
                const Body &b = l.eye->loaded.bodies[n.post];
                n.goal = b.local + RandomDir(w) * (b.radius + (10.0 + 90.0 * Roll(w)) * KM_U);
                n.doing = NpcDo::Travel;
                return true;
            }
            std::vector<const Body *> stops;
            for (int i = 0; i < e.Count(); i++) if (base.defs[i].hub == l.hub) if (const Body *b = BodyOf(w, l, i)) stops.push_back(b);
            if (stops.empty()) return false;
            const Body *b = stops[(size_t)(Roll(w) * stops.size()) % stops.size()];
            n.goal = b->local + RandomDir(w) * (Standoff(*b) * 3.0);
            n.doing = NpcDo::Travel;
        }
        Fly(n, n.goal, {}, dt);
        if (n.arrived) { n.doing = NpcDo::Dock; n.until = now + 20.0 + 30.0 * Roll(w); n.fuel = n.tank; }
        return true;
    }
    // ---------------- Pirates ----------------
    case NpcKind::Pirate: {
        if (n.doing == NpcDo::Leave) {
            Fly(n, n.goal, {}, dt);
            return !n.arrived;
        }
        // A patrol close by: break off and run
        if (now >= n.look || n.doing == NpcDo::Hunt) {
            for (const Npc &p : w.npcs)
                if (p.sys == n.sys && p.kind == NpcKind::Patrol && Len(p.pos - n.pos) < 0.2 * SIGHT && n.doing != NpcDo::Flee &&
                    (HoldsNpc(w, n, p) || HearsRadar(SignatureOf(p.spec, p.power), Len(p.pos - n.pos)))) {
                    n.goal = n.pos + Norm(n.pos - p.pos) * (1.5 * AU_U);
                    n.doing = NpcDo::Flee; n.until = now + 25.0; n.prey = -1; n.demanded = false;
                    break;
                }
        }
        if (n.doing == NpcDo::Flee) {
            Fly(n, n.goal, {}, dt);
            if (now >= n.until) { n.doing = NpcDo::Lurk; n.goal = n.pos; }
            return true;
        }
        if (n.doing == NpcDo::Hunt && n.prey >= NPC_ID_BASE) {     // a freighter or a miner
            Npc *p = NpcById(w, n.prey);
            const Body *port = p ? BodyOf(w, l, p->kind == NpcKind::Miner ? p->from : p->to) : nullptr;
            bool safe = port && !p->jumping && Len(p->pos - port->local) < Standoff(*port) + 20000.0 * KM_U;
            DVec3 to, toVel;
            bool  held = false;
            if (!p || p->sys != n.sys || (p->total < 1.0f && p->credits <= 0.0) || safe || now - n.since > 200.0 ||
                !Chase(w, n, p->id, p->was, p->vel, SignatureOf(p->spec, p->power), to, toVel, held)) { n.doing = NpcDo::Lurk; n.goal = n.pos; n.prey = -1; return true; }
            n.close = held && Len(p->was - n.was) < ROB_RANGE ? n.close + dt : 0.0;
            Fly(n, to, toVel, dt);
            if (n.close > 3.0) {                                   // boarded: half the hold, and it never gets where it was going
                float lost[GOOD_COUNT];
                float taken = 0.0f;
                for (int g = 0; g < GOOD_COUNT; g++) { lost[g] = (float)(p->cargo[g] * PIRATE_SHARE); p->cargo[g] -= lost[g]; taken += lost[g]; }
                p->total -= taken;
                e.Land(p->kind == NpcKind::Miner ? p->from : p->to, lost, true);
                const Faction *clan = w.systems.factions.Get(n.faction);
                if (p->credits > 0.0) {                            // (a paymaster's ship: the money, which is worth something to them once it's been passed on)
                    double coin = p->credits * PIRATE_SHARE;
                    p->credits -= coin;
                    FenceLoot(e, n.faction, coin);
                    News(w, n.faction, n.sys, (clan ? clan->name : std::string("Pirates")) + TextFormat(" have taken %s cr from the %s in the %s system", Thousands(coin).c_str(),
                                                                                                        p->name.c_str(), l.star->name.c_str()));
                    SlinkOff(w, n, l);
                    return true;
                }
                News(w, n.faction, n.sys, (clan ? clan->name : std::string("Pirates")) + TextFormat(" have robbed the %s of %.0f t in the %s system",
                                                                                                    p->name.c_str(), taken, l.star->name.c_str()));
                SlinkOff(w, n, l);
            }
            return true;
        }
        if (n.doing == NpcDo::Hunt) {                              // a captain
            Captain *c = CaptainById(caps, count, n.prey, n.sys);
            DVec3 at = c ? ShipLocal(w, *c) : DVec3{}, to, toVel;
            double d = c ? Len(at - n.pos) : 0.0;
            bool held = false;
            bool off = !c || InPort(w, *c) || now < c->pirateSafeUntil || c->player.CargoTotal() < 1.0 || now - n.since > 240.0 ||
                       !Chase(w, n, c->id, at, ToD(c->player.ship.vel), SignatureOf(c->player.spec, c->player.power), to, toVel, held);
            if (off) {
                if (c && c->demandFrom == n.id) {
                    c->demandFrom = -1;
                    if (now >= c->pirateSafeUntil) Tell(w, *c, n.name + (held || InPort(w, *c) ? " has broken off" : " has lost you"));
                }
                n.doing = NpcDo::Lurk; n.goal = n.pos; n.prey = -1; n.demanded = false;
                return true;
            }
            Fly(n, to, toVel, dt);
            if (!n.demanded && held && d < 5000.0 * KM_U) {
                n.demanded = true;
                c->demandFrom = n.id; c->demandTonnes = ceil(c->player.CargoTotal() * PIRATE_DEMAND);
                Tell(w, *c, TextFormat("%s: heave to and send over %.0f t, or we take what we like  -  H: hand it over", n.name.c_str(), c->demandTonnes));
            }
            n.close = n.demanded && held && d < CATCH_RANGE ? n.close + dt : 0.0;
            if (n.close > 4.0) {
                double took = Plunder(c->player.cargo, PIRATE_SHARE);
                Tell(w, *c, TextFormat("%s boarded you and took %.0f t from the hold", n.name.c_str(), took));
                c->pirateSafeUntil = now + PIRATE_PEACE; c->demandFrom = -1;
                SlinkOff(w, n, l);
            }
            return true;
        }
        // Lurking: drift, and look for something worth taking
        if (Len(n.goal - n.pos) > 0.02 * AU_U) Fly(n, n.goal, {}, dt); else Hold(n, dt);
        if (now < n.look) return true;
        n.look = now + 1.0;
        for (int i = 0; i < count; i++) {
            Captain &c = *caps[i];
            if (c.loaded.index != n.sys || c.Standing(n.faction) >= 20.0f || now < c.pirateSafeUntil || c.player.CargoTotal() < 5.0 || InPort(w, c) ||
                !NpcHolds(w, n, c)) continue;                     // (what it can't see, it can't rob)
            bool taken = false;
            for (const Npc &o : w.npcs) if (o.kind == NpcKind::Pirate && o.doing == NpcDo::Hunt && o.prey == c.id) taken = true;
            if (taken) continue;
            n.doing = NpcDo::Hunt; n.prey = c.id; n.close = 0.0; n.since = now; n.demanded = false; n.seenAt = now; n.lastPos = ShipLocal(w, c);
            return true;
        }
        Npc *best = nullptr; double bestD = 3.0 * AU_U;
        for (Npc &p : w.npcs) {
            if (p.sys != n.sys || (p.kind != NpcKind::Freighter && p.kind != NpcKind::Miner) || (p.total < 5.0f && p.credits <= 0.0) || p.doing == NpcDo::Dock) continue;
            double d = Len(p.pos - n.pos);
            if (d < bestD && HoldsNpc(w, n, p)) { bestD = d; best = &p; }
        }
        if (best) { n.doing = NpcDo::Hunt; n.prey = best->id; n.close = 0.0; n.since = now; n.seenAt = now; n.lastPos = best->pos; }
        return true;
    }
    // ---------------- Tankers: out to whoever called, then home ----------------
    case NpcKind::Tanker: {
        if (n.doing == NpcDo::Leave) { Fly(n, n.goal, {}, dt); return !n.arrived; }
        Captain *c = CaptainById(caps, count, n.prey, n.sys);
        if (!c) return false;                                      // (they've gone)
        DVec3 at = ShipLocal(w, *c);
        Fly(n, at, ToD(c->player.ship.vel), dt);
        n.close = Len(at - n.pos) < 5.0 * KM_U ? n.close + dt : 0.0;
        if (n.close > 2.0) {
            // (whatever the captain's reactor burns, at three times the going rate)
            double tonnes = fmin(TANKER_LOAD, c->player.spec.tank - c->player.fuel);
            double bill = fmin(c->player.credits, TANKER_CALLOUT + tonnes * 3.0 * GoodData(c->player.spec.fuelGood).basePrice);
            c->player.fuel += tonnes; c->player.credits -= bill;
            Tell(w, *c, TextFormat("%s pumped %.0f t of fuel across: %s cr", n.name.c_str(), tonnes, Thousands(bill).c_str()));
            n.doing = NpcDo::Leave; n.goal = JumpPoint(w, l, NO_SYSTEM); n.close = 0.0;
        }
        return true;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// Sensing: tachyon bursts and who caught them, the systems' networks and their
// traffic pictures, and what each captain's scope holds (sensors.h)
// ---------------------------------------------------------------------------
// A place that keeps a watch and shares it: any port not a clan's, and a state's sensor outposts
static bool KeepsWatch(const World &w, const Body &b)
{
    if (b.settlement == Settlement::SensorOutpost) return true;
    if (b.population <= 0) return false;
    const Faction *f = w.systems.factions.Get(b.owner);
    return !f || f->kind != FactionKind::Clan;
}

// A network's report of a burst has come in: who hears of it, and who goes to look
static void Respond(World &w, const Live &l, const World::Alert &a, Captain *const *caps, int count)
{
    // Where to look. A fix is a place. A lone bearing isn't, but ships mostly come out of warp where the gravity
    // field makes them: where the bearing crosses the edge of the field is the first place to try. With neither
    // (a port's own detector says only that something has arrived), there's just the alarm.
    DVec3  where = a.pos;
    double err = a.err;
    bool   placed = a.fixed;
    if (!placed && a.bearing) {
        double best = 1e30;
        for (int k = 0; k <= 60; k++) {
            DVec3  p = a.from + a.dir * (a.rLo + (a.rHi - a.rLo) * k / 60.0);
            double off = fabs(FieldRatio(*l.star, p) - 1.0);
            if (off < best) { best = off; where = p; }
        }
        err = Len(where - a.from) * a.sigma * DEG2RAD + 0.1 * (a.rHi - a.rLo);
        placed = true;
    }
    // Scheduled traffic comes in at the jump points, where it's expected: nobody turns out for that
    bool expected = false;
    if (a.fixed && l.hub >= 0)
        for (const Lane &lane : w.econ.base->lanes) {
            int other = lane.a == l.hub ? lane.b : lane.b == l.hub ? lane.a : -1;
            // (anywhere the fix could be, give or take: three times what it claims, and the spread ships arrive over)
            if (other >= 0 && Len(JumpPoint(w, l, w.econ.base->defs[other].sys) - a.pos) < 3.0 * a.err + 20000.0 * KM_U) expected = true;
        }
    if (expected) return;

    // Captains in reach of traffic control are told (not the one it's about)
    for (int i = 0; i < count; i++) {
        Captain &c = *caps[i];
        if (c.loaded.index != l.sys || c.id == a.source || c.trafficControl.empty()) continue;
        SensorMark m;
        m.anchor = l.sys; m.at = w.clock; m.note = c.trafficControl; m.counts = a.counts;
        if (a.fixed) { m.kind = MARK_FIX; m.from = a.pos; m.rLo = a.err; }
        else if (a.bearing) { m.kind = MARK_TACHYON; m.from = a.from; m.dir = a.dir; m.sigma = a.sigma; m.rLo = a.rLo; m.rHi = a.rHi; }
        if (a.fixed || a.bearing) AddMark(c, m);
        Tell(w, c, c.trafficControl + (a.fixed ? ": unscheduled arrival, " + FormatPlanetDist(Len(a.pos - ShipLocal(w, c))) + " from you  -  position marked"
                                     : a.bearing ? std::string(": unscheduled arrival, bearing only  -  marked")
                                                 : std::string(": a ship has dropped out of warp somewhere in the system")));
    }
    if (!placed) return;
    // The nearest patrol with nothing better to do goes to look
    Npc *best = nullptr;
    for (Npc &n : w.npcs) {
        if (n.sys != l.sys || n.kind != NpcKind::Patrol || n.post >= 0 || (n.doing != NpcDo::Dock && n.doing != NpcDo::Travel)) continue;     // (pickets stay at their posts)
        if (!best || Len(n.pos - where) < Len(best->pos - where)) best = &n;
    }
    if (best) { best->goal = where + RandomDir(w) * fmin(err, 0.3 * AU_U); best->doing = NpcDo::Travel; }
}

static void Sensing(World &w, const std::vector<Live> &live, Captain *const *caps, int count)
{
    const double now = w.clock;

    // --- Warp bubbles that collapsed this step: who caught the tachyons? ---
    for (const World::Burst &b : w.bursts) {
        // Ships' own detectors. A captain gets a bearing on the scope, and a rough idea of the range...
        for (int i = 0; i < count; i++) {
            Captain &c = *caps[i];
            if (c.loaded.index != b.sys || c.id == b.source) continue;
            TachyonDetection d;
            if (!DetectBurst(ShipDetector(c.player.spec), ShipLocal(w, c), b.pos, b.mj, w.npcDice, d)) continue;
            SensorMark m;
            m.kind = MARK_TACHYON; m.anchor = b.sys; m.from = d.from; m.dir = d.bearing; m.sigma = (float)d.sigma;
            m.rLo = d.rLo; m.rHi = d.rHi; m.counts = d.counts; m.at = now;
            AddMark(c, m);
            Tell(w, c, TextFormat("Tachyon burst: %d counts, %.1f to %.1f AU off  -  bearing marked", d.counts, d.rLo / AU_U, d.rHi / AU_U));
        }
        // ...and a raider lying in wait goes to look, as near as it can tell where that is
        for (Npc &n : w.npcs) {
            if (n.sys != b.sys || n.id == b.source || n.kind != NpcKind::Pirate || n.doing != NpcDo::Lurk || now < n.heard) continue;
            TachyonDetection d;
            if (!DetectBurst(ShipDetector(n.spec), n.pos, b.pos, b.mj, w.npcDice, d)) continue;
            double guess = sqrt(d.rLo * d.rHi);
            if (guess > 4.0 * AU_U) continue;                      // (too far to be worth the trip)
            n.goal = n.pos + d.bearing * guess;
            n.heard = now + 60.0;
        }
        // The system's network: the outposts' bearings, crossed if two or more of them caught it; and the ports'
        // own detectors, which say only that something has arrived
        const Live *l = LiveAt(live, b.sys);
        if (!l) continue;
        std::vector<TachyonDetection> got;
        int alarms = 0;
        for (const Body &body : l->eye->loaded.bodies) {
            if (!KeepsWatch(w, body)) continue;
            bool outpost = body.settlement == Settlement::SensorOutpost;
            TachyonDetection d;
            if (!DetectBurst(outpost ? OutpostDetector() : PortDetector(), body.local, b.pos, b.mj, w.npcDice, d)) continue;
            if (outpost) got.push_back(d); else alarms++;
        }
        if (got.empty() && !alarms) continue;
        World::Alert a = {};
        a.sys = b.sys; a.source = b.source;
        a.due = now + NET_RELAY * std::max<size_t>(1, got.size());  // (each report is relayed in turn)
        a.fixed = Triangulate(got, a.pos, a.err);
        if (!a.fixed && !got.empty()) {
            const TachyonDetection *best = &got[0];
            for (const TachyonDetection &d : got) if (d.counts > best->counts) best = &d;
            a.bearing = true; a.from = best->from; a.dir = best->bearing; a.sigma = (float)best->sigma; a.rLo = best->rLo; a.rHi = best->rHi;
        }
        for (const TachyonDetection &d : got) a.counts += d.counts;
        w.alerts.push_back(a);
    }
    w.bursts.clear();

    for (size_t i = 0; i < w.alerts.size();) {
        const Live *l = LiveAt(live, w.alerts[i].sys);
        if (l && now < w.alerts[i].due && now > w.alerts[i].due - 600.0) { i++; continue; }
        World::Alert a = w.alerts[i];
        w.alerts.erase(w.alerts.begin() + i);
        if (l) Respond(w, *l, a, caps, count);
    }

    // --- The traffic picture: what each system's ports, outposts and patrols hold between them (once a second) ---
    if (now - w.pictureAt >= 1.0 || now < w.pictureAt) {
        w.pictureAt = now;
        w.picture.clear();
        for (const Live &l : live) {
            std::vector<std::pair<DVec3, SensorFit>> eyes;
            for (const Body &body : l.eye->loaded.bodies) if (KeepsWatch(w, body)) eyes.push_back({ body.local, PortSensors(body) });
            if (eyes.empty()) continue;                            // (nobody here to keep one)
            for (const Npc &n : w.npcs) if (n.sys == l.sys && n.kind == NpcKind::Patrol) eyes.push_back({ n.pos, FitOf(n.spec, n.power) });
            auto &picture = w.picture[l.sys];
            auto look = [&](int id, DVec3 at, const Signature &sig) {
                int best = CONTACT_NONE;
                for (const auto &e : eyes) best = std::max(best, Sense(e.second, sig, Len(at - e.first)));
                if (best > CONTACT_NONE) picture[id] = (uint8_t)best;
            };
            for (const Npc &n : w.npcs) if (n.sys == l.sys) look(n.id, n.pos, SignatureOf(n.spec, n.power));
            for (int i = 0; i < count; i++)
                if (caps[i]->loaded.index == l.sys) look(caps[i]->id, ShipLocal(w, *caps[i]), SignatureOf(caps[i]->player.spec, caps[i]->player.power));
        }
    }

    // --- Each captain's scope ---
    for (int i = 0; i < count; i++) {
        Captain &c = *caps[i];
        const Live *l = LiveAt(live, c.loaded.index);
        DVec3 here = ShipLocal(w, c);
        // In reach of a lawful port, its traffic control passes on the system's picture
        std::string control;
        if (l) {
            double best = TRAFFIC_CONTROL;
            for (const Body &body : c.loaded.bodies) {
                if (body.population <= 0 || !KeepsWatch(w, body)) continue;
                double d = Len(body.local - here) - body.radius;
                if (d < best) { best = d; control = body.name; }
            }
        }
        if (control != c.trafficControl) { c.trafficControl = control; c.marksChanged = true; }

        std::vector<SensorTarget> targets;
        if (l)
            for (const Npc &n : w.npcs)
                if (n.sys == l->sys) targets.push_back({ n.id, n.pos - here, n.vel, SignatureOf(n.spec, n.power) });
        for (int k = 0; k < count; k++) {
            const Captain &o = *caps[k];
            if (k == i) continue;
            DVec3 rel = FromShip(w.systems, c.player.ship, o.player.ship.anchor, o.player.ship.pos);
            if (Len(rel) < 200.0 * AU_U) targets.push_back({ o.id, rel, ToD(o.player.ship.vel), SignatureOf(o.player.spec, o.player.power) });
        }
        auto shared = l ? w.picture.find(l->sys) : w.picture.end();
        UpdateContacts(w, c, targets, !control.empty() && shared != w.picture.end() ? &shared->second : nullptr);
    }
}

// ---------------------------------------------------------------------------
// Everything, one step
// ---------------------------------------------------------------------------
void StepTraffic(World &w, Captain *const *caps, int count, float dt)
{
    w.traffic.erase(std::remove_if(w.traffic.begin(), w.traffic.end(), [](const Traffic &t) { return t.npc; }), w.traffic.end());
    if (!w.econ.Built()) return;
    const EconBase &base = *w.econ.base;
    const FactionSet &fs = w.systems.factions;

    std::vector<Live> live;
    for (int i = 0; i < count; i++) {
        SysId sys = caps[i]->loaded.index;
        if (sys == NO_SYSTEM || LiveAt(live, sys) || !w.systems.Exists(sys)) continue;
        const StarSystem &s = w.systems[sys];
        live.push_back({ sys, caps[i], &s, -1,
                         { (s.cx - fs.homeCell[0]) * CELL_LY, (s.cy - fs.homeCell[1]) * CELL_LY, (s.cz - fs.homeCell[2]) * CELL_LY } });
    }
    bool same = live.size() == w.liveSystems.size();
    for (size_t i = 0; same && i < live.size(); i++) same = live[i].sys == w.liveSystems[i];
    if (!same || w.clock - w.trafficZoned > 15.0 || w.clock < w.trafficZoned || (int)w.econ.zone.size() != w.econ.Count()) Rezone(w, live);
    else for (Live &l : live)
        for (int i = 0; i < w.econ.Count(); i++) if (base.defs[i].sys == l.sys) { l.hub = base.defs[i].hub; break; }

    Dispatch(w, live);
    for (Npc &n : w.npcs) n.was = n.pos;
    for (size_t i = 0; i < w.npcs.size();) {
        const Live *l = LiveAt(live, w.npcs[i].sys);
        int id = w.npcs[i].id;
        bool keep = l && StepNpc(w, w.npcs[i], *l, caps, count, dt);
        // (StepNpc may have added ships: find this one again before dropping it)
        if (!keep) { for (size_t k = 0; k < w.npcs.size(); k++) if (w.npcs[k].id == id) { w.npcs.erase(w.npcs.begin() + k); break; } }
        else i++;
    }

    // What other ships have said, as it arrives; demands whose pirate has gone; tankers called for
    for (int i = 0; i < count; i++) {
        Captain &c = *caps[i];
        if (c.tankerCalled) {
            c.tankerCalled = false;
            const Live *l = LiveAt(live, c.loaded.index);
            bool coming = false;
            for (const Npc &n : w.npcs) if (n.kind == NpcKind::Tanker && n.prey == c.id && n.doing != NpcDo::Leave) coming = true;
            if (!l)          Tell(w, c, "No tanker will come out here: get into a system");
            else if (coming) Tell(w, c, "A tanker is already on its way");
            else {
                // From the nearest port, if there is one; otherwise it has to jump in
                const Body *from = nullptr;
                DVec3 here = ShipLocal(w, c);
                for (int m = 0; m < w.econ.Count(); m++)
                    if (const Body *b = BodyOf(w, *l, m)) if (!from || Len(b->local - here) < Len(from->local - here)) from = b;
                Npc &n = Spawn(w, NpcKind::Tanker, *l, from ? Beside(w, *from) : JumpPoint(w, *l, NO_SYSTEM), l->star->ruler);
                n.name = TextFormat("Tanker %d", 100 + n.id % 900);
                n.doing = NpcDo::Deliver; n.prey = c.id;
                Tell(w, c, n.name + (from ? " is coming out from " + from->name : std::string(" is jumping in")) +
                              TextFormat("  -  %.0f cr callout, %.0f cr/t", TANKER_CALLOUT, 3.0 * GoodData(c.player.spec.fuelGood).basePrice));
            }
        }
        for (size_t k = 0; k < c.shipReplies.size();)
            if (c.shipReplies[k].first <= w.clock) { Tell(w, c, c.shipReplies[k].second); c.shipReplies.erase(c.shipReplies.begin() + k); }
            else k++;
        if (c.demandFrom >= 0 && !FindNpc(w, c.demandFrom)) c.demandFrom = -1;
    }

    Sensing(w, live, caps, count);

    // On everyone's scopes
    for (const Npc &n : w.npcs) {
        Traffic t;
        t.id = n.id; t.name = n.name; t.npc = true; t.kind = n.kind; t.faction = n.faction;
        t.ship.anchor = n.sys; t.ship.pos = n.pos; t.ship.vel = ToF(n.vel); t.ship.rot = n.rot;
        double v = Len(n.vel);
        t.thrusting = v > 20.0; t.hyper = n.power.engaged;
        t.mining = n.kind == NpcKind::Miner && n.doing == NpcDo::Work;
        t.good = BulkGood(n.cargo); t.tonnes = n.total; t.hull = n.hull;
        if (n.kind == NpcKind::Freighter || n.kind == NpcKind::Miner) {
            if (n.from >= 0) t.from = base.defs[n.from].name;
            if (n.to >= 0)   t.dest = base.defs[n.to].name;
        }
        if (count == 1) t.stance = ShipStance(w, *caps[0], n);
        w.traffic.push_back(t);
    }
}

// ---------------------------------------------------------------------------
// Dealing with them
// ---------------------------------------------------------------------------
bool BoardShip(World &w, Captain &c, int id)
{
    const Npc *n = FindNpc(w, id);
    if (!n || n->sys != c.loaded.index) return false;
    if (n->kind != NpcKind::Freighter || n->credits <= 0.0) { Tell(w, c, n->name + " carries nothing worth boarding her for: it's the paymasters' ships that carry coin"); return false; }
    c.boardTarget = id; c.boardTime = 0.0;
    Tell(w, c, TextFormat("Closing to board the %s: hold within %.0f km of her for %.0f seconds, under way", n->name.c_str(), BOARD_RANGE / KM_U, BOARD_SECONDS));
    return true;
}

bool HailShip(World &w, Captain &c, int id)
{
    const Npc *n = FindNpc(w, id);
    if (!n || n->sys != c.loaded.index) return false;
    const FactionSet &fs = w.systems.factions;
    const EconBase &base = *w.econ.base;
    const Faction *f = fs.Get(n->faction);
    Stance stance = ShipStance(w, c, *n);
    std::string say;
    switch (n->kind) {
    case NpcKind::Freighter: {
        int g = BulkGood(n->cargo);
        const std::string to = n->to >= 0 ? base.defs[n->to].name : std::string("nowhere in particular");
        if (stance == STANCE_HOSTILE) say = "We've nothing to say to you.";
        else if (n->credits > 0.0) say = "Paymaster's business for " + fs.Name(n->payee) + ", bound for " + to + ". Keep your distance, captain.";
        else if (g < 0) say = "Running light to " + to + ". Fly safe, captain.";
        else say = TextFormat("Bound for %s with %.0f t, mostly %s. They're paying about %.0f for it there.", to.c_str(), n->total, GoodData(g).name,
                              MarketPrice(w, n->to, g));
        break;
    }
    case NpcKind::Miner:
        say = n->from >= 0 ? "Working out of " + base.defs[n->from].name + ". Keep clear of the cutting beam, captain." : std::string("Busy, captain.");
        break;
    case NpcKind::Patrol:
        say = stance == STANCE_ALERTED ? "Hold your course and stand by to be scanned."
            : stance == STANCE_HOSTILE ? "You're known to us. Give us a reason."
            : std::string("This is a ") + (f ? f->name : std::string("system")) + " patrol. Keep your papers in order, captain.";
        break;
    default:
        say = stance == STANCE_ALLIED ? "Clan business, kin. Good hunting."
            : stance == STANCE_NEUTRAL ? "Clan business. Move along, friend."
            : "That's a fine-looking hold you've got there.";
        break;
    }
    double dist = Len(ShipLocal(w, c) - n->pos);
    c.shipReplies.push_back({ w.clock + 2.0 * dist / LIGHT_SEC_U + 0.6, n->name + ": " + say });
    Tell(w, c, "Hailing " + n->name + "...");
    return true;
}

bool CallTanker(World &w, Captain &c)
{
    if (c.player.fuel > 0.2 * c.player.spec.tank) { Tell(w, c, "The tanker service is for ships that are nearly dry"); return false; }
    c.tankerCalled = true;                                         // (it's sent on the next step: StepTraffic)
    return true;
}

bool PayPirates(World &w, Captain &c)
{
    Npc *n = nullptr;
    for (Npc &p : w.npcs) if (p.id == c.demandFrom && p.prey == c.id && p.doing == NpcDo::Hunt) n = &p;
    if (!n) { Tell(w, c, "Nobody is demanding anything of you"); return false; }
    double total = c.player.CargoTotal();
    double took = Plunder(c.player.cargo, total > 0.0 ? fmin(1.0, c.demandTonnes / total) : 0.0);
    Tell(w, c, TextFormat("Sent %.0f t over to %s: it turns away", took, n->name.c_str()));
    c.pirateSafeUntil = w.clock + PIRATE_PEACE; c.demandFrom = -1;
    n->doing = NpcDo::Lurk; n->goal = n->pos; n->prey = -1; n->demanded = false; n->look = w.clock + 60.0;
    return true;
}
