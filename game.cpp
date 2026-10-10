// game.cpp - new game, the simulation step, and target queries
#include "game.h"
#include "comms.h"
#include "economy.h"
#include "format.h"
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
            if (b.kind == Kind::Station) { start = &b; break; }
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
        if (d > fmax(SCAN_RADII * b.radius, SCAN_MIN)) continue;
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
        const Traffic *other = t.index != c.id ? w.FindTraffic(t.index) : nullptr;
        if (!other) return false;
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
// Hyper target lock: holding Shift while something is targeted swings the
// nose onto it and ignores small mouse movements. A deliberate movement breaks
// the lock; the view can switch it to another body (aim.retarget).
// Swings the nose toward a target (the lock, or a course just set on a map)
static void TurnToward(Ship &ship, DVec3 rel, float dt)
{
    Quaternion full = QuaternionFromVector3ToVector3(ShipForward(ship), ToF(Norm(rel)));
    Quaternion step = QuaternionSlerp(QuaternionIdentity(), full, 1.0f - expf(-LOCK_TURN_RATE * dt));
    ship.rot = QuaternionNormalize(QuaternionMultiply(step, ship.rot));
}

static void UpdateLock(World &w, Captain &c, Controls &in, const Aim &aim, float dt)
{
    FlightState &f = c.flight;
    Ship &ship = c.player.ship;
    if (!in.hyper) {
        f.lock = {};
        f.lockSuppressed = false;
        return;
    }
    // Autopilot to whatever's selected (which follows the reticle)
    if (!f.lock.valid() && !f.lockSuppressed && f.selected.valid()) {
        f.lock = f.selected;
        f.lockBreak = { 0, 0 };
    }
    // Nudging the cursor onto another body while locked switches the lock to it
    f.retargetCooldown = fmaxf(0.0f, f.retargetCooldown - dt);
    TargetId retarget = CanonicalTarget(c, aim.retarget);
    if (f.lock.valid() && retarget.valid() && retarget != f.lock && f.retargetCooldown <= 0.0f) {
        f.lock = retarget;
        f.lockBreak = { 0, 0 };
        f.retargetCooldown = 0.5f;                            // no flip-flopping between close bodies
    }
    DVec3 rel;
    if (f.lock.valid() && !FindTarget(w, c, f.lock, rel)) f.lock = {};
    if (!f.lock.valid()) return;

    // Mouse steering builds up toward breaking the lock, but decays,
    // so jitter never adds up; only a deliberate move gets through
    float k = expf(-LOCK_BREAK_DECAY * dt);
    f.lockBreak = { f.lockBreak.x * k + in.yaw, f.lockBreak.y * k + in.pitch };
    if (Vector2Length(f.lockBreak) > LOCK_BREAK_DEG) {
        f.lock = {};
        f.lockSuppressed = true;
        return;
    }
    in.yaw = in.pitch = 0.0f;                                 // ignore small mouse movement
    TurnToward(ship, rel, dt);
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

    Asteroid *a = f.selected.type == TargetId::Asteroid ? FindAsteroid(c.field, f.selected.rock) : nullptr;
    if (!a) { f.miningStatus = "MINING LASER: no asteroid targeted"; return; }
    double surf = Len(FromShip(w.systems, p.ship, c.loaded.index, a->local)) - AsteroidRadius(*a);
    double hold = p.CargoTotal();
    if (surf > MINING_RANGE) {
        f.miningStatus = "MINING LASER: out of range (" + FormatPlanetDist(surf) + ", need 1.5 km)";
        return;
    }
    if (hold >= CARGO_CAPACITY - 1e-6) { f.miningStatus = "CARGO HOLD FULL"; return; }

    double amount = fmin(MINING_RATE * dt, fmin((double)a->oreLeft, CARGO_CAPACITY - hold));
    a->oreLeft -= (float)amount;
    p.cargo[a->ore] += amount;
    w.deltas.mined[a->id] = a->oreLeft;
    w.minedChanged[a->id] = a->oreLeft;
    f.miningActive = true;
    f.miningRock   = a->id;
    f.miningStatus = TextFormat("MINING %s  +%.1f t/s", GoodData(a->ore).name, MINING_RATE);
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

    // Selection follows the reticle, falling back on the course set on a map; a hyper lock overrides it
    f.selected = CanonicalTarget(c, aim.hovered.valid() ? aim.hovered : aim.course);
    if (f.lock.valid()) f.lock = CanonicalTarget(c, f.lock);
    UpdateLock(w, c, in, aim, dt);
    DVec3 courseRel;
    if (aim.steer && !f.lock.valid() && FindTarget(w, c, aim.course, courseRel)) TurnToward(ship, courseRel, dt);
    if (f.lockSuppressed && !aim.hovered.valid() && !f.selected.valid()) f.lockSuppressed = false;
    if (f.lock.valid()) f.selected = f.lock;

    // Drive and flight
    DVec3  lockRel;
    double lockRadius = 0.0;
    bool   haveLock = f.lock.valid() && FindTarget(w, c, f.lock, lockRel, &lockRadius);
    f.drive = ComputeDrive(ship, w.systems, c.loaded, haveLock ? &lockRel : nullptr, lockRadius,
                           haveLock ? TargetName(w, c, f.lock) : std::string(), p.fuel > 0.0);
    UpdateShip(ship, in, dt, f.drive);
    f.last = in;

    // Fuel: the drive burns by speed (exponentially more as you go faster); cruise
    // thrusters only while working. On an empty tank thrusters run on reserve and
    // hyper limps on emergency power, so you're never stranded.
    f.burn = in.hyper ? (f.drive.noFuel ? 0.0 : DriveBurnRate(Vector3Length(ship.vel))) : CRUISE_BURN * ship.thrustUse;
    if (p.fuel <= 0.0) f.burn = 0.0;
    p.fuel = fmax(0.0, p.fuel - f.burn * dt);

    // Rocks around where the ship is NOW (after moving: in hyper one step can cross the whole
    // field). They exist at any speed so you can see when you've entered a belt, even in hyper.
    UpdateField(c.field, c.loaded, w.deltas.mined, ShipLocal(w, c));
    Collide(w, c);

    // Fuel scooping: skim close to a star
    f.scooping = false;
    if (c.loaded.index >= 0)
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Star && Len(FromShip(w.systems, ship, c.loaded.index, b.local)) < b.radius * SCOOP_RANGE) {
                f.scooping = p.fuel < FUEL_CAPACITY;
                p.fuel = fmin(FUEL_CAPACITY, p.fuel + SCOOP_RATE * dt);
            }

    // Helium-3 skimming: dip into a gas giant's upper atmosphere (not in hyper)
    f.skimming = f.skimFull = false;
    if (c.loaded.index >= 0 && !in.hyper)
        for (const Body &b : c.loaded.bodies)
            if (b.kind == Kind::Planet && b.giant && b.massEarth >= GIANT_MASS &&
                Len(FromShip(w.systems, ship, c.loaded.index, b.local)) < b.radius * SKIM_DEPTH) {
                double room = CARGO_CAPACITY - p.CargoTotal();
                f.skimming = room > 1e-6;
                f.skimFull = !f.skimming;
                if (authoritative && f.skimming) p.cargo[RAW_HELIUM3] += fmin(room, SKIM_RATE * dt);
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

    // Docked: the captain's record of this market stays current (to the second)
    if (authoritative && f.nearBody >= 0 && c.loaded.bodies[f.nearBody].population > 0 && w.econ.Built()) {
        auto rec = c.know.prices.find(c.loaded.bodies[f.nearBody].id);
        if (rec == c.know.prices.end() || w.clock - rec->second.at >= 1.0) RecordPrices(w, c, c.loaded.bodies[f.nearBody]);
    }

    // Things that vanished (mined out, unloaded) can't stay selected
    DVec3 rel;
    if (f.selected.valid() && !FindTarget(w, c, f.selected, rel)) f.selected = {};
}
