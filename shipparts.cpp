// shipparts.cpp - hulls, parts, and what they add up to
#include "shipparts.h"
#include "partsdata.h"
#include "format.h"
#include <algorithm>

const HullDef &Hull(int id) { return Parts().hulls[id >= 0 && id < HULL_COUNT ? id : HULL_HAULER]; }

const char *PartTypeName(PartType t)
{
    static const char *names[] = { "Reactor", "Main engine", "Thrusters", "Hyper drive", "Warp drive", "Drive capacitor", "Capacitor bank",
                                   "Fuel tank", "Cargo bay", "Mining laser", "Fuel scoop", "Gas skimmer", "Telescope", "Radar", "Infrared array",
                                   "Tachyon detector", "Passenger cabins" };
    return names[(int)t < (int)PartType::Count ? (int)t : 0];
}

const char *GradeName(int grade) { return Parts().rules.gradeName[grade >= 0 && grade < PART_GRADES ? grade : 0].c_str(); }

bool IsExternal(PartType t)
{
    return t == PartType::Engine || t == PartType::Thrusters || t == PartType::MiningLaser || t == PartType::Scoop || t == PartType::Skimmer ||
           t == PartType::Telescope || t == PartType::Radar || t == PartType::IrArray;
}

int FuelGoodOf(int reactorFuel) { return PartModelOf(PartType::Reactor, reactorFuel).fuel; }

bool Specialises(Slant s, PartType t)
{
    switch (s) {
    case Slant::Industry:    return t == PartType::Engine || t == PartType::Thrusters;
    case Slant::Shipping:    return t == PartType::Hold || t == PartType::Tank || t == PartType::HyperDrive || t == PartType::Radar || t == PartType::Cabins;
    case Slant::Energy:      return t == PartType::Reactor || t == PartType::DriveCap || t == PartType::Bank || t == PartType::IrArray;
    case Slant::Mining:      return t == PartType::MiningLaser || t == PartType::Scoop;
    case Slant::Finance:     return t == PartType::Telescope || t == PartType::WarpDrive || t == PartType::TachyonDetector;
    case Slant::Agriculture: return t == PartType::Skimmer || t == PartType::Tank;
    default:                 return false;
    }
}

// ---------------------------------------------------------------------------
// One part's numbers: its model's (partsdata.h), in its size and grade. Size
// doubles everything that scales; grade makes it lighter and better.
// ---------------------------------------------------------------------------
static double Scale(const Part &p)  { return pow(2.0, (double)std::max(1, std::min(PART_SIZES, (int)p.size)) - 1.0); }
static double Better(const Part &p) { return 1.0 + Parts().rules.better * p.grade; }           // what it does
static double Leaner(const Part &p) { return 1.0 - Parts().rules.leaner * p.grade; }           // what it weighs, or wastes

// What each kind of part does, as numbers (the same ones SpecOf adds up)
struct Does {
    double reactorTW = 0, eff = 0, fuelTJ = 0, burn = 0, floor = 0, restart = 0;
    double curve[REACTOR_STEPS + 1] = {};
    double thrust = 0, engineTW = 0, turn = 0;
    double topSpeed = 0, p0 = 0, k = 0, n = 0, accel = 0, maxMass = 0, jumpTJ = 0;
    double capTJ = 0, capEff = 0, bankTJ = 0, tank = 0, hold = 0;
    double rate = 0, range = 0, drawTW = 0, scan = 0;
    double area = 0, depth = 0, catches = 0, res = 0;              // tachyon detectors
    double berths = 0;
};

static Does Work(const Part &p)
{
    Does d;
    const PartModel &md = PartModelOf(p);
    double m = Scale(p), b = Better(p), l = Leaner(p);
    switch (p.type) {
    case PartType::Reactor:
        // (heat at the burn rate times the fuel's energy; power, that times the efficiency there)
        d.burn = md.burn * m * b; d.floor = md.floor; d.restart = md.restart;
        for (int i = 0; i <= REACTOR_STEPS; i++) d.curve[i] = ReactorCurveAt(md, (double)i / REACTOR_STEPS) + md.efficiencyGrade * p.grade;
        d.fuelTJ = Parts().fuelTJ[md.fuel];
        d.eff = d.curve[REACTOR_STEPS];
        d.reactorTW = d.burn * d.fuelTJ * d.eff;
        break;
    case PartType::Engine:     d.thrust = md.thrust * m * b; d.engineTW = md.draw * m; break;
    case PartType::Thrusters:  d.turn = md.turn * m * b; break;
    case PartType::HyperDrive:
    case PartType::WarpDrive:
        d.topSpeed = (p.type == PartType::WarpDrive ? WARP_SPEED : HYPER_SPEED) * md.speed * (1.0 + md.speedGrade * p.grade);
        d.p0 = md.fieldBase * m; d.k = md.fieldMass * l; d.n = md.fieldPower; d.accel = md.accel; d.maxMass = md.maxMass * m;
        d.jumpTJ = md.jump * (1.0 - md.jumpGrade * p.grade);
        break;
    case PartType::DriveCap:   d.capTJ = md.capacity * m * b; d.capEff = md.efficiency + md.efficiencyGrade * p.grade; break;
    case PartType::Bank:       d.bankTJ = md.capacity * m * b; break;
    case PartType::Tank:       d.tank = md.capacity * m; break;
    case PartType::Hold:       d.hold = md.capacity * m; break;
    case PartType::MiningLaser: d.rate = md.rate * m * b; d.range = md.range * (1.0 + md.rangeGrade * p.grade); d.drawTW = md.draw * m; break;
    case PartType::Scoop:
    case PartType::Skimmer:    d.rate = md.rate * m * b; break;
    // (sensors: each size reaches so much further than the smallest, each grade so much further again)
    case PartType::Telescope:
    case PartType::Radar:
    case PartType::IrArray:    d.scan = (1.0 + md.reachSize * (p.size - 1)) * (1.0 + md.reachGrade * p.grade); d.drawTW = md.draw * m; break;
    case PartType::Cabins:     d.berths = md.capacity * m; d.drawTW = md.draw * m; break;
    // (a tachyon detector is a volume: twice the size is twice the volume, shared between its face and its depth.
    // Better grades catch more of what decays inside, and place it better.)
    case PartType::TachyonDetector:
        d.area = md.area * pow(m, 2.0 / 3.0); d.depth = md.depth * pow(m, 1.0 / 3.0); d.catches = md.catches + md.catchesGrade * p.grade;
        d.res = md.resolution / (1.0 + md.resolutionGrade * p.grade) / pow(m, 1.0 / 3.0); d.drawTW = md.draw * m;
        break;
    default: break;
    }
    return d;
}

double PartRating(PartType type, int grade, int variant)
{
    Part p;
    p.type = type; p.size = 1; p.grade = (uint8_t)std::max(0, std::min(PART_GRADES - 1, grade)); p.variant = (uint8_t)variant;
    Does d = Work(p);
    switch (type) {
    case PartType::Reactor:    return d.reactorTW;
    case PartType::Engine:     return d.thrust;
    case PartType::Thrusters:  return d.turn;
    case PartType::HyperDrive:
    case PartType::WarpDrive:  return d.maxMass;
    case PartType::DriveCap:   return d.capTJ;
    case PartType::Bank:       return d.bankTJ;
    case PartType::Tank:       return d.tank;
    case PartType::Hold:       return d.hold;
    case PartType::Cabins:     return d.berths;
    default:                   return d.rate > 0.0 ? d.rate : d.scan;
    }
}

PartInfo Describe(const Part &p, const FactionSet *fs)
{
    const PartModel &n = PartModelOf(p);
    double m = Scale(p);
    Does d = Work(p);
    PartInfo i;
    i.mass = n.mass * m * Leaner(p); i.volume = n.volume * m; i.cost = n.cost * m * Parts().rules.gradeCost[p.grade < PART_GRADES ? p.grade : 0];
    i.external = IsExternal(p.type);
    i.material = n.material; i.tonnes = 0.5 * i.mass;
    const Faction *maker = fs ? fs->Get(p.maker) : nullptr;
    std::string kind = n.name;
    kind[0] = (char)tolower((unsigned char)kind[0]);
    i.name = (maker ? maker->name + " " : std::string()) + GradeName(p.grade) + " " + kind + TextFormat(", size %d", (int)p.size);
    switch (p.type) {
    case PartType::Reactor: {
        int best = 0;
        for (int k = 1; k <= REACTOR_STEPS; k++) if (d.curve[k] > d.curve[best]) best = k;
        i.does = TextFormat("%s TW at %.0f%%, best %.0f%% at %d%% load, idles at %.0f%% burn, on %s", Thousands(d.reactorTW).c_str(), d.eff * 100.0, d.curve[best] * 100.0,
                            best * 100 / REACTOR_STEPS, d.floor * 100.0, GoodData(n.fuel).name);
        break;
    }
    case PartType::Engine:      i.does = TextFormat("thrust %.0f, %.0f TW flat out", d.thrust, d.engineTW); break;
    case PartType::Thrusters:   i.does = TextFormat("turning %.0f", d.turn); break;
    case PartType::HyperDrive:  i.does = TextFormat("%.0f c, moves up to %.0f t", d.topSpeed / LIGHT_SEC_U, d.maxMass); break;
    case PartType::WarpDrive:   i.does = TextFormat("%.1f million c, up to %.0f t, %.0f TJ/t to jump", d.topSpeed / LIGHT_SEC_U / 1e6, d.maxMass, d.jumpTJ); break;
    case PartType::DriveCap:    i.does = TextFormat("%.0f TJ, %.0f%% in and out", d.capTJ, d.capEff * 100.0); break;
    case PartType::Bank:        i.does = TextFormat("%.0f TJ", d.bankTJ); break;
    case PartType::Tank:        i.does = TextFormat("%.0f t of fuel", d.tank); break;
    case PartType::Hold:        i.does = TextFormat("%.0f t of cargo", d.hold); break;
    case PartType::MiningLaser: i.does = TextFormat("%.1f t/s at %.1f km, %.0f TW", d.rate, d.range * METERS_PER_UNIT / 1000.0, d.drawTW); break;
    case PartType::Scoop:       i.does = TextFormat("%.1f t/s of deuterium off a star", d.rate); break;
    case PartType::Skimmer:     i.does = TextFormat("%.2f t/s of helium-3", d.rate); break;
    case PartType::Telescope:   i.does = TextFormat("%.1fx range, %.1f TW", d.scan, d.drawTW); break;
    case PartType::Radar:       i.does = TextFormat("%.1fx range, %.0f TW when on", d.scan, d.drawTW); break;
    case PartType::IrArray:     i.does = TextFormat("%.1fx range, %.1f TW", d.scan, d.drawTW); break;
    case PartType::Cabins:      i.does = TextFormat("%.0f berths, %.1f TW", d.berths, d.drawTW); break;
    case PartType::TachyonDetector: i.does = TextFormat("%.0f m2 x %.1f m, catches %.0f%%, %.1f deg", d.area, d.depth, d.catches * 100.0, d.res); break;
    default: break;
    }
    return i;
}

// ---------------------------------------------------------------------------
// The hull as built
// ---------------------------------------------------------------------------
double BuildVolume(const ShipBuild &b)     { return Hull(b.hull).volume * (1.0 + Parts().rules.stretchRoom * b.stretch); }
int    BuildHardpoints(const ShipBuild &b) { return Hull(b.hull).hardpoints + b.mounts; }

double VolumeUsed(const ShipBuild &b)
{
    double v = 0.0;
    for (const Part &p : b.parts) v += Describe(p).volume;
    return v;
}

int HardpointsUsed(const ShipBuild &b)
{
    int n = 0;
    for (const Part &p : b.parts) if (IsExternal(p.type)) n++;
    return n;
}

// Each one dearer than the last: a few are worth it, then a bigger hull is
double StretchCost(const ShipBuild &b, bool hardpoint)
{
    const HullDef &h = Hull(b.hull);
    const PartRules &r = Parts().rules;
    return hardpoint ? h.cost * r.mountCost * pow(r.mountCostGrowth, b.mounts) : h.cost * r.stretchCost * pow(r.stretchCostGrowth, b.stretch);
}

static double HullMass(const ShipBuild &b) { return Hull(b.hull).mass * (1.0 + Parts().rules.stretchMass * b.stretch + Parts().rules.mountMass * b.mounts); }

double BuildValue(const ShipBuild &b)
{
    double v = Hull(b.hull).cost;
    ShipBuild step = b;
    for (step.stretch = 0; step.stretch < b.stretch; step.stretch++) v += StretchCost(step, false);
    for (step.mounts = 0; step.mounts < b.mounts; step.mounts++) v += StretchCost(step, true);
    for (const Part &p : b.parts) v += Describe(p).cost;
    return v;
}

// ---------------------------------------------------------------------------
// Adding it all up
// ---------------------------------------------------------------------------
ShipSpec SpecOf(const ShipBuild &b)
{
    ShipSpec s;
    s.dryMass = HullMass(b);
    s.thrust = s.turn = s.reactorTW = s.engineTW = s.bankTJ = s.driveCapTJ = 0.0;
    s.reactorBurn = s.reactorFloor = s.reactorRestart = 0.0;
    double curveBy[REACTOR_STEPS + 1] = {}, floorBy = 0.0;
    s.hyperSpeed = s.warpSpeed = s.hyperMaxMass = s.warpMaxMass = 0.0;
    s.tank = s.hold = s.laserRate = s.laserRange = s.laserTW = s.scoopRate = s.skimRate = 0.0;
    s.berths = 0.0;
    s.scan = s.radar = s.ir = 0.3;                                 // (what a bare hull's own sensors manage)
    s.radarTW = 16.0;
    s.tachArea = 0.5; s.tachDepth = 0.5; s.tachEff = 0.3; s.tachRes = 30.0;
    s.hullSize = BuildVolume(b) / Hull(HULL_HAULER).volume;
    double tach = 0.0;
    s.idleTW = 32.0;
    double capBy = 0.0, biggest = 0.0;
    for (const Part &p : b.parts) {
        Does d = Work(p);
        s.dryMass += Describe(p).mass;
        switch (p.type) {
        case PartType::Reactor:
            // (reactors aboard run together, at the same share of what each can burn)
            s.reactorBurn += d.burn; floorBy += d.floor * d.burn; s.reactorRestart = fmax(s.reactorRestart, d.restart);
            for (int k = 0; k <= REACTOR_STEPS; k++) curveBy[k] += d.curve[k] * d.burn;
            if (d.burn * d.fuelTJ > biggest) { biggest = d.burn * d.fuelTJ; s.fuelTJ = d.fuelTJ; s.fuelGood = PartModelOf(p).fuel; }
            break;
        case PartType::Engine:    s.thrust += d.thrust; s.engineTW += d.engineTW; break;
        case PartType::Thrusters: s.turn += d.turn; break;
        case PartType::HyperDrive:                                 // (only the biggest drive counts: fields don't add)
            if (d.maxMass > s.hyperMaxMass) { s.hyperMaxMass = d.maxMass; s.hyperSpeed = d.topSpeed; s.hyperP0 = d.p0; s.hyperK = d.k; s.hyperN = d.n; s.hyperAccel = d.accel; }
            break;
        case PartType::WarpDrive:
            if (d.maxMass > s.warpMaxMass) { s.warpMaxMass = d.maxMass; s.warpSpeed = d.topSpeed; s.warpP0 = d.p0; s.warpK = d.k; s.warpN = d.n; s.warpAccel = d.accel; s.jumpTJ = d.jumpTJ; }
            break;
        case PartType::DriveCap:  s.driveCapTJ += d.capTJ; capBy += d.capEff * d.capTJ; break;
        case PartType::Bank:      s.bankTJ += d.bankTJ; break;
        case PartType::Tank:      s.tank += d.tank; break;
        case PartType::Hold:      s.hold += d.hold; break;
        case PartType::MiningLaser: s.laserRate += d.rate; s.laserRange = fmax(s.laserRange, d.range); s.laserTW += d.drawTW; break;
        case PartType::Scoop:     s.scoopRate += d.rate; break;
        case PartType::Skimmer:   s.skimRate += d.rate; break;
        // (of each kind of sensor, only the best one fitted counts)
        case PartType::Telescope: s.scan = fmax(s.scan, d.scan); s.idleTW += d.drawTW; break;
        case PartType::Radar:     if (d.scan > s.radar) { s.radar = d.scan; s.radarTW = d.drawTW; } break;
        case PartType::IrArray:   s.ir = fmax(s.ir, d.scan); s.idleTW += d.drawTW; break;
        case PartType::Cabins:    s.berths += d.berths; s.idleTW += d.drawTW; break;
        case PartType::TachyonDetector:
            if (d.area * d.depth * d.catches > tach) { tach = d.area * d.depth * d.catches; s.tachArea = d.area; s.tachDepth = d.depth; s.tachEff = d.catches; s.tachRes = d.res; }
            s.idleTW += d.drawTW;
            break;
        default: break;
        }
    }
    if (s.reactorBurn > 0.0) {
        for (int k = 0; k <= REACTOR_STEPS; k++) s.reactorCurve[k] = curveBy[k] / s.reactorBurn;
        s.reactorFloor = floorBy / s.reactorBurn;
    }
    s.reactorEff = s.reactorCurve[REACTOR_STEPS];
    s.reactorTW = s.reactorBurn * s.fuelTJ * s.reactorEff;
    s.capIn = s.capOut = s.driveCapTJ > 0.0 ? capBy / s.driveCapTJ : 0.90;
    s.idleTW += 0.005 * s.reactorTW;
    return s;
}

static Part Make(PartType type, int size, int grade = 0, int variant = 0)
{
    Part p;
    p.type = type; p.size = (uint8_t)size; p.grade = (uint8_t)grade; p.variant = (uint8_t)variant;
    return p;
}

ShipBuild StarterBuild()
{
    ShipBuild b;
    b.hull = HULL_HAULER;
    b.parts = { Make(PartType::Reactor, 2), Make(PartType::Engine, 2), Make(PartType::Thrusters, 2), Make(PartType::HyperDrive, 2),
                Make(PartType::WarpDrive, 2), Make(PartType::DriveCap, 2), Make(PartType::Bank, 1), Make(PartType::Tank, 3), Make(PartType::Tank, 1),
                Make(PartType::Hold, 3), Make(PartType::MiningLaser, 1), Make(PartType::Skimmer, 1), Make(PartType::Telescope, 1),
                Make(PartType::Radar, 1), Make(PartType::IrArray, 1), Make(PartType::TachyonDetector, 1) };
    return b;
}

// The smallest size of a part that does at least 'need' of what it does (capped at the biggest made)
static int SizeFor(double need, double atSizeOne)
{
    int s = 1;
    while (s < PART_SIZES && atSizeOne * pow(2.0, s - 1) < need) s++;
    return s;
}

ShipBuild NpcBuild(NpcKind kind, double load)
{
    ShipBuild b;
    auto add = [&](PartType t, int size, int grade = 0) { b.parts.push_back(Make(t, size, grade)); };
    switch (kind) {
    case NpcKind::Freighter: {
        // Built around its load: bays for it, then whatever reactor, engines and drive that much ship needs
        b.hull = load <= 100.0 ? HULL_HAULER : load <= 700.0 ? HULL_FREIGHTER : HULL_BULK;
        double left = fmax(25.0, load);
        const double bay = PartRating(PartType::Hold);
        while (left > 0.0) { int s = std::min(5, SizeFor(left, bay)); add(PartType::Hold, s); left -= bay * pow(2.0, s - 1); }
        double full = Hull(b.hull).mass + 1.25 * fmax(25.0, load) + 30.0;                     // near enough, all up
        add(PartType::Tank, SizeFor(10.0 + 0.04 * full, PartRating(PartType::Tank)));
        add(PartType::Reactor, SizeFor(1920.0 + 3.84 * full, PartRating(PartType::Reactor)));
        add(PartType::Engine, SizeFor(0.45 * 21.0 * full, PartRating(PartType::Engine)));
        add(PartType::Thrusters, SizeFor(70.0 * full, PartRating(PartType::Thrusters)));
        add(PartType::HyperDrive, SizeFor(1.1 * full, PartRating(PartType::HyperDrive)));
        add(PartType::Radar, 1);
        break;
    }
    case NpcKind::Miner:
        b.hull = HULL_PROSPECTOR;
        add(PartType::Reactor, 2); add(PartType::Engine, 2); add(PartType::Thrusters, 1); add(PartType::HyperDrive, 3);
        add(PartType::Tank, 2); add(PartType::Hold, 3); add(PartType::Hold, 2); add(PartType::MiningLaser, 2); add(PartType::Skimmer, 1);
        add(PartType::Radar, 1);
        break;
    case NpcKind::Pirate:
        b.hull = HULL_COURIER;
        add(PartType::Reactor, 1, 1); add(PartType::Engine, 2); add(PartType::Thrusters, 1); add(PartType::HyperDrive, 1);
        add(PartType::Tank, 1); add(PartType::Hold, 1);
        // (raiders hunt by heat and by eye, with the radar off: it would only say where they are)
        add(PartType::IrArray, 2); add(PartType::Telescope, 1); add(PartType::Radar, 1); add(PartType::TachyonDetector, 1);
        break;
    case NpcKind::Patrol:
        b.hull = HULL_CUTTER;
        add(PartType::Reactor, 2, 1); add(PartType::Engine, 3); add(PartType::Thrusters, 2); add(PartType::HyperDrive, 2, 1);
        add(PartType::Tank, 2); add(PartType::Hold, 1); add(PartType::Bank, 2);
        add(PartType::Radar, 2); add(PartType::IrArray, 2); add(PartType::Telescope, 2); add(PartType::TachyonDetector, 1);
        break;
    default:                                                       // tankers
        b.hull = HULL_HAULER;
        add(PartType::Reactor, 2); add(PartType::Engine, 2); add(PartType::Thrusters, 2); add(PartType::HyperDrive, 2);
        add(PartType::Tank, 3); add(PartType::Tank, 3); add(PartType::Radar, 1);
        break;
    }
    return b;
}
