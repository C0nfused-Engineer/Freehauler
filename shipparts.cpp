// shipparts.cpp - hulls, parts, and what they add up to
#include "shipparts.h"
#include <algorithm>

static const HullDef HULLS[HULL_COUNT] = {
    { "Courier",      "Small and quick: room for a good drive and not much else.",              150.0,  4,  10.0,   8000.0 },
    { "Hauler",       "The ship everyone starts in: a bit of everything.",                      400.0,  6,  20.0,  20000.0 },
    { "Prospector",   "Hardpoints to spare for lasers, scoops and skimmers.",                   450.0,  9,  26.0,  32000.0 },
    { "Cutter",       "A strong frame with mounts all round, built to chase and to fight.",     600.0, 10,  40.0,  60000.0 },
    { "Freighter",    "Four and a half times a hauler's room. Wants big engines.",             1800.0,  8,  90.0, 110000.0 },
    { "Bulk carrier", "A flying warehouse.",                                                   6500.0, 10, 320.0, 400000.0 },
};

const HullDef &Hull(int id) { return HULLS[id >= 0 && id < HULL_COUNT ? id : HULL_HAULER]; }

const char *PartTypeName(PartType t)
{
    static const char *names[] = { "Reactor", "Main engine", "Thrusters", "Hyper drive", "Warp drive", "Drive capacitor", "Capacitor bank",
                                   "Fuel tank", "Cargo bay", "Mining laser", "Fuel scoop", "Gas skimmer", "Scanner" };
    return names[(int)t < (int)PartType::Count ? (int)t : 0];
}

const char *GradeName(int grade)
{
    static const char *names[] = { "Standard", "Improved", "Advanced", "Elite" };
    return names[grade >= 0 && grade < PART_GRADES ? grade : 0];
}

bool IsExternal(PartType t)
{
    return t == PartType::Engine || t == PartType::Thrusters || t == PartType::MiningLaser || t == PartType::Scoop || t == PartType::Skimmer;
}

int FuelGoodOf(int reactorFuel)
{
    return reactorFuel == BURNS_DEUTERIUM ? REF_FUEL : reactorFuel == BURNS_HELIUM3 ? RAW_HELIUM3 : REF_REACTOR_FUEL;
}

bool Specialises(Slant s, PartType t)
{
    switch (s) {
    case Slant::Industry:    return t == PartType::Engine || t == PartType::Thrusters;
    case Slant::Shipping:    return t == PartType::Hold || t == PartType::Tank || t == PartType::HyperDrive;
    case Slant::Energy:      return t == PartType::Reactor || t == PartType::DriveCap || t == PartType::Bank;
    case Slant::Mining:      return t == PartType::MiningLaser || t == PartType::Scoop;
    case Slant::Finance:     return t == PartType::Scanner || t == PartType::WarpDrive;
    case Slant::Agriculture: return t == PartType::Skimmer || t == PartType::Tank;
    default:                 return false;
    }
}

// ---------------------------------------------------------------------------
// One part's numbers. Size doubles everything that scales; grade makes it
// lighter and better.
// ---------------------------------------------------------------------------
struct Numbers {
    double mass, volume, cost;      // at size 1, standard grade
    int    material;
};
static const Numbers BASE[(int)PartType::Count] = {
    /* Reactor      */ { 6.0, 15.0, 3000.0, GOOD_REACTOR_CORES },
    /* Engine       */ { 4.0, 10.0, 2500.0, GOOD_SHIP_COMPONENTS },
    /* Thrusters    */ { 1.5,  4.0, 1200.0, GOOD_SHIP_COMPONENTS },
    /* HyperDrive   */ { 3.0, 10.0, 3000.0, GOOD_SHIP_COMPONENTS },
    /* WarpDrive    */ { 5.0, 15.0, 5000.0, GOOD_SHIP_COMPONENTS },
    /* DriveCap     */ { 2.5, 12.0, 1500.0, GOOD_ELECTRONICS },
    /* Bank         */ { 1.0,  5.0,  800.0, GOOD_ELECTRONICS },
    /* Tank         */ { 0.5, 12.0,  300.0, GOOD_HULL_PLATING },
    /* Hold         */ { 1.0, 30.0,  400.0, GOOD_HULL_PLATING },
    /* MiningLaser  */ { 1.5,  5.0, 2000.0, GOOD_MACHINERY },
    /* Scoop        */ { 1.0,  5.0, 1500.0, GOOD_MACHINERY },
    /* Skimmer      */ { 1.0,  5.0, 1500.0, GOOD_MACHINERY },
    /* Scanner      */ { 0.3,  3.0, 1000.0, GOOD_INSTRUMENTS },
};

static double Scale(const Part &p)  { return pow(2.0, (double)std::max(1, std::min(PART_SIZES, (int)p.size)) - 1.0); }
static double Better(const Part &p) { return 1.0 + 0.15 * p.grade; }           // what it does
static double Leaner(const Part &p) { return 1.0 - 0.10 * p.grade; }           // what it weighs, or wastes

// What each kind of part does, as numbers (the same ones SpecOf adds up)
struct Does {
    double reactorMW = 0, eff = 0, fuelMJ = 0;
    double thrust = 0, engineMW = 0, turn = 0;
    double topSpeed = 0, p0 = 0, k = 0, maxMass = 0, jumpMJ = 0;
    double capMJ = 0, capEff = 0, bankMJ = 0, tank = 0, hold = 0;
    double rate = 0, range = 0, drawMW = 0, scan = 0;
};

static Does Work(const Part &p)
{
    Does d;
    double m = Scale(p), b = Better(p), l = Leaner(p);
    switch (p.type) {
    case PartType::Reactor:
        d.reactorMW = 100.0 * m * b * (p.variant == BURNS_DEUTERIUM ? 1.3 : p.variant == BURNS_HELIUM3 ? 1.6 : 1.0);
        d.eff = 0.40 + 0.04 * p.grade;
        d.fuelMJ = p.variant == BURNS_DEUTERIUM ? 8000.0 : p.variant == BURNS_HELIUM3 ? 20000.0 : 2500.0;
        break;
    case PartType::Engine:     d.thrust = 1312.5 * m * b; d.engineMW = 5.0 * m; break;
    case PartType::Thrusters:  d.turn = 13750.0 * m * b; break;
    case PartType::HyperDrive: d.topSpeed = HYPER_SPEED * (1.0 + 0.10 * p.grade); d.p0 = 2.5 * m; d.k = 6.6e-9 * l; d.maxMass = 225.0 * m; break;
    case PartType::WarpDrive:  d.topSpeed = WARP_SPEED * (1.0 + 0.15 * p.grade); d.p0 = 7.5 * m; d.k = 6.9e-15 * l; d.maxMass = 225.0 * m;
                               d.jumpMJ = 25.0 * (1.0 - 0.08 * p.grade); break;
    case PartType::DriveCap:   d.capMJ = 4000.0 * m * b; d.capEff = 0.90 + 0.02 * p.grade; break;
    case PartType::Bank:       d.bankMJ = 600.0 * m * b; break;
    case PartType::Tank:       d.tank = 10.0 * m; break;
    case PartType::Hold:       d.hold = 25.0 * m; break;
    case PartType::MiningLaser: d.rate = 2.0 * m * b; d.range = 300.0 * (1.0 + 0.2 * p.grade); d.drawMW = 8.0 * m; break;
    case PartType::Scoop:      d.rate = 1.0 * m * b; break;
    case PartType::Skimmer:    d.rate = 0.25 * m * b; break;
    case PartType::Scanner:    d.scan = (1.0 + 0.5 * (p.size - 1)) * (1.0 + 0.25 * p.grade); d.drawMW = 1.0 * m; break;
    default: break;
    }
    return d;
}

PartInfo Describe(const Part &p, const FactionSet *fs)
{
    static const double DEARER[PART_GRADES] = { 1.0, 2.5, 6.0, 15.0 };
    static const char *burns[] = { "Fission reactor", "Deuterium fusion reactor", "Helium-3 fusor" };
    const Numbers &n = BASE[(int)p.type < (int)PartType::Count ? (int)p.type : 0];
    double m = Scale(p);
    Does d = Work(p);
    PartInfo i;
    i.mass = n.mass * m * Leaner(p); i.volume = n.volume * m; i.cost = n.cost * m * DEARER[p.grade < PART_GRADES ? p.grade : 0];
    if (p.type == PartType::Reactor) {
        if (p.variant == BURNS_DEUTERIUM) i.cost *= 2.5;
        if (p.variant == BURNS_HELIUM3)   { i.cost *= 5.0; i.mass *= 0.6; }
    }
    i.external = IsExternal(p.type);
    i.material = n.material; i.tonnes = 0.5 * i.mass;
    const char *what = p.type == PartType::Reactor ? burns[p.variant < REACTOR_FUELS ? p.variant : 0] : PartTypeName(p.type);
    const Faction *maker = fs ? fs->Get(p.maker) : nullptr;
    std::string kind = what;
    kind[0] = (char)tolower((unsigned char)kind[0]);
    i.name = (maker ? maker->name + " " : std::string()) + GradeName(p.grade) + " " + kind + TextFormat(", size %d", (int)p.size);
    switch (p.type) {
    case PartType::Reactor:     i.does = TextFormat("%.0f MW at %.0f%% from %s", d.reactorMW, d.eff * 100.0, GoodData(FuelGoodOf(p.variant)).name); break;
    case PartType::Engine:      i.does = TextFormat("thrust %.0f, %.0f MW flat out", d.thrust, d.engineMW); break;
    case PartType::Thrusters:   i.does = TextFormat("turning %.0f", d.turn); break;
    case PartType::HyperDrive:  i.does = TextFormat("%.0f c, moves up to %.0f t", d.topSpeed / LIGHT_SEC_U, d.maxMass); break;
    case PartType::WarpDrive:   i.does = TextFormat("%.1f million c, up to %.0f t, %.0f MJ/t to jump", d.topSpeed / LIGHT_SEC_U / 1e6, d.maxMass, d.jumpMJ); break;
    case PartType::DriveCap:    i.does = TextFormat("%.0f MJ, %.0f%% in and out", d.capMJ, d.capEff * 100.0); break;
    case PartType::Bank:        i.does = TextFormat("%.0f MJ", d.bankMJ); break;
    case PartType::Tank:        i.does = TextFormat("%.0f t of fuel", d.tank); break;
    case PartType::Hold:        i.does = TextFormat("%.0f t of cargo", d.hold); break;
    case PartType::MiningLaser: i.does = TextFormat("%.1f t/s at %.1f km, %.0f MW", d.rate, d.range * METERS_PER_UNIT / 1000.0, d.drawMW); break;
    case PartType::Scoop:       i.does = TextFormat("%.1f t/s of deuterium off a star", d.rate); break;
    case PartType::Skimmer:     i.does = TextFormat("%.2f t/s of helium-3", d.rate); break;
    case PartType::Scanner:     i.does = TextFormat("%.1fx range, %.0f MW", d.scan, d.drawMW); break;
    default: break;
    }
    return i;
}

// ---------------------------------------------------------------------------
// The hull as built
// ---------------------------------------------------------------------------
double BuildVolume(const ShipBuild &b)     { return Hull(b.hull).volume * (1.0 + 0.12 * b.stretch); }
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
    return hardpoint ? h.cost * 0.20 * pow(1.8, b.mounts) : h.cost * 0.25 * pow(1.7, b.stretch);
}

static double HullMass(const ShipBuild &b) { return Hull(b.hull).mass * (1.0 + 0.06 * b.stretch + 0.03 * b.mounts); }

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
    s.thrust = s.turn = s.reactorMW = s.engineMW = s.bankMJ = s.driveCapMJ = 0.0;
    s.hyperSpeed = s.warpSpeed = s.hyperMaxMass = s.warpMaxMass = 0.0;
    s.tank = s.hold = s.laserRate = s.laserRange = s.laserMW = s.scoopRate = s.skimRate = 0.0;
    s.scan = 0.3;                                                  // (what a bare hull's own sensors manage)
    s.idleMW = 1.0;
    double effBy = 0.0, capBy = 0.0, biggest = 0.0;
    for (const Part &p : b.parts) {
        Does d = Work(p);
        s.dryMass += Describe(p).mass;
        switch (p.type) {
        case PartType::Reactor:
            s.reactorMW += d.reactorMW; effBy += d.eff * d.reactorMW;
            if (d.reactorMW > biggest) { biggest = d.reactorMW; s.fuelMJ = d.fuelMJ; s.fuelGood = FuelGoodOf(p.variant); }
            break;
        case PartType::Engine:    s.thrust += d.thrust; s.engineMW += d.engineMW; break;
        case PartType::Thrusters: s.turn += d.turn; break;
        case PartType::HyperDrive:                                 // (only the biggest drive counts: fields don't add)
            if (d.maxMass > s.hyperMaxMass) { s.hyperMaxMass = d.maxMass; s.hyperSpeed = d.topSpeed; s.hyperP0 = d.p0; s.hyperK = d.k; }
            break;
        case PartType::WarpDrive:
            if (d.maxMass > s.warpMaxMass) { s.warpMaxMass = d.maxMass; s.warpSpeed = d.topSpeed; s.warpP0 = d.p0; s.warpK = d.k; s.jumpMJ = d.jumpMJ; }
            break;
        case PartType::DriveCap:  s.driveCapMJ += d.capMJ; capBy += d.capEff * d.capMJ; break;
        case PartType::Bank:      s.bankMJ += d.bankMJ; break;
        case PartType::Tank:      s.tank += d.tank; break;
        case PartType::Hold:      s.hold += d.hold; break;
        case PartType::MiningLaser: s.laserRate += d.rate; s.laserRange = fmax(s.laserRange, d.range); s.laserMW += d.drawMW; break;
        case PartType::Scoop:     s.scoopRate += d.rate; break;
        case PartType::Skimmer:   s.skimRate += d.rate; break;
        case PartType::Scanner:   s.scan = fmax(s.scan, d.scan); s.idleMW += d.drawMW; break;
        default: break;
        }
    }
    s.reactorEff = s.reactorMW > 0.0 ? effBy / s.reactorMW : 0.40;
    s.capIn = s.capOut = s.driveCapMJ > 0.0 ? capBy / s.driveCapMJ : 0.90;
    s.idleMW += 0.005 * s.reactorMW;
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
                Make(PartType::Hold, 3), Make(PartType::MiningLaser, 1), Make(PartType::Skimmer, 1), Make(PartType::Scanner, 1) };
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
        while (left > 0.0) { int s = std::min(5, SizeFor(left, 25.0)); add(PartType::Hold, s); left -= 25.0 * pow(2.0, s - 1); }
        double full = Hull(b.hull).mass + 1.25 * fmax(25.0, load) + 30.0;                     // near enough, all up
        add(PartType::Tank, SizeFor(10.0 + 0.04 * full, 10.0));
        add(PartType::Reactor, SizeFor(60.0 + 0.12 * full, 100.0));
        add(PartType::Engine, SizeFor(0.45 * 21.0 * full, 1312.5));
        add(PartType::Thrusters, SizeFor(70.0 * full, 13750.0));
        add(PartType::HyperDrive, SizeFor(1.1 * full, 225.0));
        add(PartType::Scanner, 1);
        break;
    }
    case NpcKind::Miner:
        b.hull = HULL_PROSPECTOR;
        add(PartType::Reactor, 2); add(PartType::Engine, 2); add(PartType::Thrusters, 1); add(PartType::HyperDrive, 3);
        add(PartType::Tank, 2); add(PartType::Hold, 3); add(PartType::Hold, 2); add(PartType::MiningLaser, 2); add(PartType::Skimmer, 1);
        add(PartType::Scanner, 1);
        break;
    case NpcKind::Pirate:
        b.hull = HULL_COURIER;
        add(PartType::Reactor, 1, 1); add(PartType::Engine, 2); add(PartType::Thrusters, 1); add(PartType::HyperDrive, 1);
        add(PartType::Tank, 1); add(PartType::Hold, 1); add(PartType::Scanner, 1);
        break;
    case NpcKind::Patrol:
        b.hull = HULL_CUTTER;
        add(PartType::Reactor, 2, 1); add(PartType::Engine, 3); add(PartType::Thrusters, 2); add(PartType::HyperDrive, 2, 1);
        add(PartType::Tank, 2); add(PartType::Hold, 1); add(PartType::Bank, 2); add(PartType::Scanner, 2);
        break;
    default:                                                       // tankers
        b.hull = HULL_HAULER;
        add(PartType::Reactor, 2); add(PartType::Engine, 2); add(PartType::Thrusters, 2); add(PartType::HyperDrive, 2);
        add(PartType::Tank, 3); add(PartType::Tank, 3); add(PartType::Scanner, 1);
        break;
    }
    return b;
}
