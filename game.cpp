// game.cpp - new game, the simulation step, and target queries
#include "game.h"
#include "comms.h"
#include "contracts.h"
#include "economy.h"
#include "format.h"
#include "sensors.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Systems loading in and out
// ---------------------------------------------------------------------------
static void LoadSystem(World &w, Captain &c, SysId i)
{
    GenerateSystemContents(w.systems[i], c.loaded);
    AssignOwners(w.systems.factions, w.systems[i], c.loaded);
    c.loaded.index = i;
    TraceLog(LOG_INFO, "SYSTEM: loaded %s (%d bodies)", w.systems[i].name.c_str(), (int)c.loaded.bodies.size());
    w.Emit(EventType::SystemLoaded, "", TargetId::MakeStar(i), -1, 0.0, 0.0, c.id);
}

static void UnloadSystem(World &w, Captain &c)
{
    SysId i = c.loaded.index;
    TraceLog(LOG_INFO, "SYSTEM: unloaded %s", w.systems[i].name.c_str());
    c.loaded = LoadedSystem();
    c.flight.nearBody = -1;
    w.Emit(EventType::SystemUnloaded, "", TargetId::MakeStar(i), -1, 0.0, 0.0, c.id);
}

void UpdateLoadedSystem(World &w, Captain &c)
{
    Ship &ship = c.player.ship;
    w.systems.Trim();
    Rebase(ship, w.systems);
    double anchorDist = Len(ship.pos);                   // ship is stored relative to its anchor star
    if (c.loaded.index >= 0 && (c.loaded.index != ship.anchor || anchorDist > UNLOAD_DISTANCE_LY * LY_U)) UnloadSystem(w, c);
    if (c.loaded.index < 0 && anchorDist < LOAD_DISTANCE_LY * LY_U) LoadSystem(w, c, ship.anchor);
}

void NewWorld(World &w, unsigned seed)
{
    w = World();
    w.seed    = seed;
    w.systems.Init(seed);
}

void NewCaptain(World &w, Captain &c, int id, const std::string &name)
{
    c = Captain();
    c.id   = id;
    c.name = name;
    Ship &ship = c.player.ship;
    const SysId home = w.systems.home;
    c.know.charted.insert(home);                             // everyone knows their own home
    c.standing.assign(w.systems.factions.Count(), 0.0f);     // ...and starts as nobody's friend or enemy
    ship.anchor = home;
    LoadSystem(w, c, home);

    // Start beside a station in the home system (the belt refinery if there is
    // one), or else its second planet: along its orbit, so the routes in and out
    // of the system don't pass through it, a little above the plane. In
    // multiplayer each newcomer starts a little further along, side by side.
    const Body *start = nullptr;
    for (const Body &b : c.loaded.bodies)
        if (b.kind == Kind::Station && b.econ == Econ::Refinery) { start = &b; break; }
    if (!start)
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Station && b.population > 0) { start = &b; break; }
    if (!start) {
        int planets = 0;
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Planet && planets++ == 1) { start = &b; break; }
    }
    if (!start) start = &c.loaded.bodies[0];                 // (a home with nothing but its star)
    double away = start->kind == Kind::Station ? fmax(start->radius * 3.0, 4.0 * KM_U) : start->radius * 3.0;
    DVec3 n       = w.systems[home].n;
    DVec3 out     = Len(start->local) > 0.0 ? Norm(start->local) : w.systems[home].u;
    DVec3 tangent = Cross(n, out);
    ship.pos = start->local + tangent * away + n * (away * 0.2);
    Vector3 look = ToF(Norm(start->local - ship.pos));
    ship.rot = QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, look, { 0, 1, 0 })));
    if (id > 0) ship.pos = ship.pos + Cross(ToD(look), n) * (12.0 * ((id - 1) % 16 + 1));   // 60 m apart
}

unsigned SeedFromText(const std::string &text, uint64_t entropy)
{
    std::string t = text;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    if (t.empty()) return (unsigned)(Mix64(entropy) % 1000000000ull) + 1;          // (short enough to read out to a friend)
    bool digits = t.size() <= 9;
    for (char c : t) if (c < '0' || c > '9') digits = false;
    return digits ? (unsigned)strtoul(t.c_str(), nullptr, 10) : (unsigned)(NameHash(t) % 4000000000ull);
}

void NewGame(GameState &g, unsigned seed)
{
    g = GameState();
    NewWorld(g, seed);
    NewCaptain(g, g);
    g.events.clear();
}

bool Known(const Captain &c, int body)
{
    if (c.loaded.index < 0 || body < 0 || body >= (int)c.loaded.bodies.size()) return false;
    if (c.know.charted.count(c.loaded.index)) return true;
    for (int b = body, hops = 0; b >= 0 && hops < 4; b = c.loaded.bodies[b].parent, hops++) {
        const Body &o = c.loaded.bodies[b];
        if (o.kind == Kind::Star) return b == body;              // stars are plain to see; what orbits them isn't
        if (c.know.scanned.count(o.id)) return true;
    }
    return false;
}

void UpdateKnowledge(World &w, Captain &c)
{
    if (c.loaded.index < 0 || c.know.charted.count(c.loaded.index)) return;
    // Anyone who has answered a hail sends their charts along
    for (const CommsChannel &ch : c.comms.channels)
        if (ch.with.system == c.loaded.index && CommsAnswered(w, c, ch.with)) {
            c.know.charted.insert(c.loaded.index);
            w.Emit(EventType::Notice, ch.withName + " sent charts of the " + w.systems[c.loaded.index].name + " system",
                   {}, -1, 0.0, 0.0, c.id);
            return;
        }
    // Otherwise it's what the ship's own sensors pick up in passing
    for (int bi = 0; bi < (int)c.loaded.bodies.size(); bi++) {
        const Body &b = c.loaded.bodies[bi];
        if (b.kind == Kind::Star || c.know.scanned.count(b.id)) continue;
        double d = Len(FromShip(w.systems, c.player.ship, c.loaded.index, b.local)) - b.radius;
        if (d > fmax(SCAN_RADII * b.radius, SCAN_MIN) * c.player.spec.scan) continue;      // (as far as the scanner reaches)
        bool news = !Known(c, bi);
        c.know.scanned.insert(b.id);
        if (news) w.Emit(EventType::Notice, "Scanned " + b.name, TargetId::MakeBody(c.loaded.index, bi), -1, 0.0, 0.0, c.id);
    }
}

void ApplyRockChange(Captain &c, uint64_t rock, float oreLeft)
{
    if (oreLeft <= 0.001f) { RemoveAsteroid(c.field, rock); return; }
    if (Asteroid *a = FindAsteroid(c.field, rock)) a->oreLeft = oreLeft;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
DVec3 ShipLocal(const World &w, const Captain &c)
{
    return c.loaded.index >= 0 ? FromShip(w.systems, c.player.ship, c.loaded.index) * -1.0 : DVec3{};
}

TargetId CanonicalTarget(const Captain &c, TargetId t)
{
    if (t.type == TargetId::Star && t.system == c.loaded.index) return TargetId::MakeBody(t.system, 0);
    if (t.type == TargetId::Body && t.index == 0 && t.system != c.loaded.index) return TargetId::MakeStar(t.system);
    return t;
}

const Body *TargetBody(const Captain &c, TargetId t)
{
    if (t.type != TargetId::Body || t.system != c.loaded.index || t.system < 0) return nullptr;
    if (t.index < 0 || t.index >= (int)c.loaded.bodies.size()) return nullptr;
    return &c.loaded.bodies[t.index];
}

bool FindTarget(const World &w, const Captain &c, TargetId t, DVec3 &rel, double *radius)
{
    t = CanonicalTarget(c, t);
    const Ship &ship = c.player.ship;
    double r = 0.0;
    switch (t.type) {
    case TargetId::None:
        return false;
    case TargetId::Star:
        if (!w.systems.Exists(t.system)) return false;
        rel = FromShip(w.systems, ship, t.system);
        r   = w.systems[t.system].starRadius;
        break;
    case TargetId::Body: {
        const Body *b = TargetBody(c, t);
        if (!b) return false;
        rel = FromShip(w.systems, ship, t.system, b->local);
        r   = b->radius;
        break;
    }
    case TargetId::Zone:
        if (t.system != c.loaded.index || t.index < 0 || t.index >= (int)c.loaded.zones.size()) return false;
        rel = FromShip(w.systems, ship, t.system, ZoneTargetPoint(c.loaded.zones[t.index], ShipLocal(w, c)));
        break;
    case TargetId::Asteroid: {
        if (t.system != c.loaded.index || c.field.system != t.system) return false;
        const Asteroid *a = FindAsteroid(c.field, t.rock);
        if (!a) return false;
        rel = FromShip(w.systems, ship, t.system, a->local);
        r   = AsteroidRadius(*a);
        break;
    }
    case TargetId::Ship: {
        // (a ship can be targeted while a sensor holds it: not once it's lost, and its ghost is all that's left)
        const Traffic *other = t.index != c.id ? w.FindTraffic(t.index) : nullptr;
        if (!other || other->ghost) return false;
        if (c.sensing) { const Contact *k = FindContact(c, t.index); if (!k || !k->Held()) return false; }
        rel = FromShip(w.systems, ship, other->ship.anchor, other->ship.pos);
        r   = SHIP_RADIUS;
        break;
    }
    }
    if (radius) *radius = r;
    return true;
}

std::string TargetName(const World &w, const Captain &c, TargetId t)
{
    t = CanonicalTarget(c, t);
    switch (t.type) {
    case TargetId::Star:
        return w.systems.Exists(t.system) ? w.systems[t.system].name : "";
    case TargetId::Body:
        if (const Body *b = TargetBody(c, t)) return b->name;
        return "";
    case TargetId::Zone:
        return t.system == c.loaded.index && t.index >= 0 && t.index < (int)c.loaded.zones.size()
                   ? c.loaded.zones[t.index].name : "";
    case TargetId::Asteroid:
        if (const Asteroid *a = FindAsteroid(c.field, t.rock)) return a->name;
        return "";
    case TargetId::Ship:
        if (const Traffic *other = w.FindTraffic(t.index)) return other->name;
        return "";
    default:
        return "";
    }
}

// ---------------------------------------------------------------------------
// The simulation step
// ---------------------------------------------------------------------------
// Target lock: the pilot locks onto a destination (and releases it again); while
// it's locked, holding Shift swings the nose onto it and flies there.
// Swings the nose toward a target (the lock, or a course just set on a map)
// (no faster than the thrusters can swing it: 'most' degrees this step)
static void TurnToward(Ship &ship, DVec3 rel, float dt, float most)
{
    Vector3 fwd = ShipForward(ship), want = ToF(Norm(rel));
    float   angle = acosf(Clamp(Vector3DotProduct(fwd, want), -1.0f, 1.0f)) * RAD2DEG;
    if (angle < 1e-4f) return;
    float   turn = fminf(angle * (1.0f - expf(-LOCK_TURN_RATE * dt)), most);
    Quaternion full = QuaternionFromVector3ToVector3(fwd, want);
    Quaternion step = QuaternionSlerp(QuaternionIdentity(), full, turn / angle);
    ship.rot = QuaternionNormalize(QuaternionMultiply(step, ship.rot));
}

static void UpdateLock(World &w, Captain &c, Controls &in, const Aim &aim, float dt)
{
    FlightState &f = c.flight;
    // The lock is the destination the pilot has set: by locking onto what they were looking at, or by picking a
    // course on a map. It stays until they release it. With none, the ship goes where it's pointed.
    f.lock = CanonicalTarget(c, aim.course);
    DVec3 rel;
    if (f.lock.valid() && !FindTarget(w, c, f.lock, rel)) f.lock = {};
    if (!f.lock.valid() || !in.hyper) return;
    // Under drive, the nose is held on it (the mouse does nothing until the lock is released)
    in.yaw = in.pitch = 0.0f;
    TurnToward(c.player.ship, rel, dt, (float)(c.player.spec.turn / c.player.Mass() * dt));
}

// Solid bodies and rocks: the ship stops at the surface instead of passing through
static void Collide(World &w, Captain &c)
{
    if (c.loaded.index < 0) return;
    Ship &ship = c.player.ship;
    for (const Body &b : c.loaded.bodies) {
        DVec3  away = FromShip(w.systems, ship, c.loaded.index, b.local) * -1.0;   // body center -> ship
        double d = Len(away), minD = b.radius + 20.0;                               // 100 m above the surface
        if (d >= minD) continue;
        DVec3 nrm = Norm(away);
        ship.pos = ship.pos + nrm * (minD - d);
        float into = Vector3DotProduct(ship.vel, ToF(nrm));
        if (into < 0.0f) ship.vel = Vector3Subtract(ship.vel, Vector3Scale(ToF(nrm), into));
    }
    // Rocks are solid at cruise speeds; in hyper you pass through the field
    if (Vector3Length(ship.vel) >= APPROACH_SPEED * 4.0f) return;
    DVec3 here = ShipLocal(w, c);
    for (auto &cell : c.field.cells)
        for (const Asteroid &a : cell.second) {
            DVec3  away = here - a.local;
            double d = Len(away), minD = AsteroidRadius(a) * 0.9 + 2.5;   // ship is ~2.5 units across
            if (d >= minD || d < 1e-6) continue;
            DVec3 nrm = Norm(away);
            ship.pos = ship.pos + nrm * (minD - d);
            here = here + nrm * (minD - d);
            float into = Vector3DotProduct(ship.vel, ToF(nrm));
            if (into < 0.0f) ship.vel = Vector3Subtract(ship.vel, Vector3Scale(ToF(nrm), into));
        }
}

// Mining laser: fire on a targeted rock in range
static void UpdateMining(World &w, Captain &c, const Controls &in, float dt)
{
    FlightState &f = c.flight;
    Player &p = c.player;
    f.miningActive = false;
    f.miningStatus.clear();
    if (!in.fire || in.hyper) return;

    if (p.spec.laserRate <= 0.0) { f.miningStatus = "NO MINING LASER FITTED"; return; }
    Asteroid *a = f.selected.type == TargetId::Asteroid ? FindAsteroid(c.field, f.selected.rock) : nullptr;
    if (!a) { f.miningStatus = "MINING LASER: no asteroid targeted"; return; }
    double surf = Len(FromShip(w.systems, p.ship, c.loaded.index, a->local)) - AsteroidRadius(*a);
    double hold = p.CargoTotal();
    if (surf > p.spec.laserRange) {
        f.miningStatus = "MINING LASER: out of range (" + FormatPlanetDist(surf) + ", need " + FormatPlanetDist(p.spec.laserRange) + ")";
        return;
    }
    if (hold >= p.spec.hold - 1e-6) { f.miningStatus = "CARGO HOLD FULL"; return; }

    double amount = fmin(p.spec.laserRate * dt, fmin((double)a->oreLeft, p.spec.hold - hold));
    a->oreLeft -= (float)amount;
    p.cargo[a->ore] += amount;
    w.deltas.mined[a->id] = a->oreLeft;
    w.minedChanged[a->id] = a->oreLeft;
    f.miningActive = true;
    f.miningRock   = a->id;
    f.miningStatus = TextFormat("MINING %s  +%.1f t/s", GoodData(a->ore).name, p.spec.laserRate);
    if (a->oreLeft <= 0.001f) {
        w.deltas.mined[a->id] = 0.0f;
        w.minedChanged[a->id] = 0.0f;
        f.miningStatus = "ASTEROID MINED OUT";
        f.miningActive = false;
        w.Emit(EventType::AsteroidDepleted, "", TargetId::MakeRock(c.loaded.index, a->id), a->ore, 0.0, 0.0, c.id);
        RemoveAsteroid(c.field, a->id);
    }
}

void StepCaptain(World &w, Captain &c, const Controls &input, const Aim &aim, float dt, bool authoritative)
{
    Controls in = input;
    FlightState &f = c.flight;
    Player &p = c.player;
    Ship &ship = p.ship;

    UpdateLoadedSystem(w, c);

    // Selection (what F hails, the laser mines, the info panel describes) follows the reticle, falling back on the lock
    f.selected = CanonicalTarget(c, aim.hovered.valid() ? aim.hovered : aim.course);
    UpdateLock(w, c, in, aim, dt);
    DVec3 courseRel;
    if (aim.steer && !in.hyper && FindTarget(w, c, aim.course, courseRel)) TurnToward(ship, courseRel, dt, (float)(p.spec.turn / p.Mass() * dt));
    if (!f.selected.valid()) f.selected = f.lock;

    // Drive and flight
    // (the reactor burns fuel for what's drawn; on an empty tank the engines and the hyper drive
    // limp on the emergency cell, so you're never quite stranded)
    LockInfo lock;
    bool haveLock = f.lock.valid() && FindTarget(w, c, f.lock, lock.rel, &lock.radius);
    if (haveLock) {
        lock.name = TargetName(w, c, f.lock);
        if (f.lock.type == TargetId::Ship) if (const Traffic *other = w.FindTraffic(f.lock.index)) lock.speed = Vector3Length(other->ship.vel);
    }
    FlightReport flown = FlyShip(ship, p.spec, p.power, p.fuel, p.Mass(), in, dt, w.systems, c.loaded, haveLock ? &lock : nullptr, f.miningActive);
    f.drive = flown.drive; f.burn = flown.burn; f.load = flown.load;
    f.driveCharging = flown.charging; f.driveTooHeavy = flown.tooHeavy; f.driveStarved = flown.starved;
    f.driveNone = flown.noDrive; f.drivePower = flown.drivePower; f.efficiency = flown.efficiency; f.driveCold = flown.cold;
    in.hyper = p.power.engaged;                               // (what the drive did, not what was asked)
    f.last = in;
    if (authoritative && flown.collapse > 0.0 && c.loaded.index >= 0)          // out of warp: the bubble's collapse can be picked up
        w.bursts.push_back({ c.loaded.index, ShipLocal(w, c), flown.collapse, c.id });

    // Rocks around where the ship is NOW (after moving: in hyper one step can cross the whole
    // field). They exist at any speed so you can see when you've entered a belt, even in hyper.
    UpdateField(c.field, c.loaded, w.deltas.mined, ShipLocal(w, c));
    Collide(w, c);

    // Fuel scooping: skim close to a star
    f.scooping = false;
    if (c.loaded.index >= 0)
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Star && Len(FromShip(w.systems, ship, c.loaded.index, b.local)) < b.radius * SCOOP_RANGE) {
                // (a scoop takes in deuterium, for a deuterium fusion reactor: other reactors' fuel has to be bought)
                if (p.spec.scoopRate <= 0.0 || p.spec.fuelGood != REF_FUEL) break;
                f.scooping = p.fuel < p.spec.tank;
                p.fuel = fmin(p.spec.tank, p.fuel + p.spec.scoopRate * dt);
            }

    // Helium-3 skimming: dip into a gas giant's upper atmosphere (not in hyper)
    f.skimming = f.skimFull = false;
    if (c.loaded.index >= 0 && !in.hyper && p.spec.skimRate > 0.0)
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Planet && b.giant && b.massEarth >= GIANT_MASS &&
                Len(FromShip(w.systems, ship, c.loaded.index, b.local)) < b.radius * SKIM_DEPTH) {
                double room = p.spec.hold - p.CargoTotal();
                f.skimming = room > 1e-6;
                f.skimFull = !f.skimming;
                if (authoritative && f.skimming) p.cargo[RAW_HELIUM3] += fmin(room, p.spec.skimRate * dt);
                break;
            }

    if (authoritative) UpdateMining(w, c, in, dt);

    // Interaction: the nearest planet, moon or station within its interaction radius
    f.nearBody = -1;
    if (c.loaded.index >= 0) {
        double best = 1e300;
        for (int bi = 0; bi < (int)c.loaded.bodies.size(); bi++) {
            const Body &b = c.loaded.bodies[bi];
            if (b.kind == Kind::Star) continue;
            double d = Len(FromShip(w.systems, ship, c.loaded.index, b.local)) - b.radius;   // altitude
            double range = b.kind == Kind::Station ? STATION_RANGE : fmax(b.radius * (INTERACT_RADII - 1.0), INTERACT_MIN);
            if (d <= range && d < best) { best = d; f.nearBody = bi; }
        }
    }

    if (authoritative) UpdateComms(w, c);
    if (authoritative) StepContracts(w, c);

    // Docked: the captain's record of this market stays current (to the second)
    if (authoritative && f.nearBody >= 0 && c.loaded.bodies[f.nearBody].population > 0 && w.econ.Built()) {
        const Body &port = c.loaded.bodies[f.nearBody];
        auto rec = c.know.prices.find(EconomyBody(c, port));
        if (rec == c.know.prices.end() || w.clock - rec->second.at >= 1.0) {
            RecordPrices(w, c, port);
            CollectStakes(w, c, port);                         // (what the captain's shares in its industry have earned is paid over)
            RecordWorld(w, c, port);
        }
    }

    // Things that vanished (mined out, unloaded) can't stay selected
    DVec3 rel;
    if (f.selected.valid() && !FindTarget(w, c, f.selected, rel)) f.selected = {};
}
