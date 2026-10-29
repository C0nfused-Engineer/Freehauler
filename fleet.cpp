// fleet.cpp - designing a faction's ships, and the sums for a run in one
#include "fleet.h"
#include "game.h"
#include "partsdata.h"
#include <map>
#include <algorithm>

const char *ShipRoleName(int role)
{
    static const char *names[] = { "bulk freighter", "local hauler", "tanker", "liner" };
    return names[role >= 0 && role < ROLE_COUNT ? role : 0];
}

static Part Make(PartType type, int size, int grade = 0, int variant = 0)
{
    Part p;
    p.type = type; p.size = (uint8_t)std::max(1, std::min(PART_SIZES, size)); p.grade = (uint8_t)std::max(0, std::min(PART_GRADES - 1, grade)); p.variant = (uint8_t)variant;
    return p;
}

// The smallest size of a part that does at least 'need' of what it does at size one (the biggest made, if none does)
static int SizeFor(double need, double atSizeOne)
{
    int s = 1;
    while (s < PART_SIZES && atSizeOne * pow(2.0, s - 1) < need) s++;
    return s;
}

// What a faction's character comes to, in a ship
struct Taste {
    int     grade = 0;                  // what it can build to
    Slant   slant = Slant::Count;       // (conglomerates: the parts it's good at come a grade better)
    uint8_t reactor = BURNS_REACTOR_FUEL;   // the best fuel it can build a reactor for (and any lesser one)
    double  haste = 4.0;                // what a ship's time is worth to it, against what the ship cost
    double  cruise = FLEET_CRUISE;      // how hard it runs its drives
    double  engines = 1.0;              // engines and thrusters, against what the ship strictly needs
    int     stretch = 0;                // times it lengthens a hull for more room
    bool    small = false;              // makes do with the smaller hulls
};

static Taste TasteOf(const Faction *f)
{
    Taste t;
    if (!f) { t.small = true; return t; }                          // independent owners: standard parts, modest hulls
    switch (f->kind) {
    case FactionKind::Government:
        t.grade = f->strength >= 1.0f ? 1 : 0;
        if (f->axis[AX_OUTLOOK] > 0.3f) t.reactor = BURNS_DEUTERIUM;                           // the forward-looking fit fusion
        if (f->axis[AX_FORCE] > 0.3f) { t.cruise = 0.65; t.engines = 1.6; t.haste = 8.0; }     // the warlike want speed in hand
        if (f->axis[AX_MARKETS] > 0.3f) t.stretch = 1;                                         // planners build big
        break;
    case FactionKind::Conglomerate:
        t.grade = 1; t.slant = f->slant;
        if (f->slant == Slant::Shipping) { t.stretch = 3; t.cruise = 0.45; t.engines = 0.8; t.haste = 2.5; } // carrying is the business: every cubic metre, and no fuel wasted
        if (f->slant == Slant::Energy)   t.reactor = BURNS_HELIUM3;
        if (f->slant == Slant::Finance)  { t.cruise = 0.6; t.reactor = BURNS_DEUTERIUM; t.haste = 10.0; }      // time is money
        if (f->slant == Slant::Industry) t.stretch = 1;
        break;
    case FactionKind::Clan:
        t.small = true; t.cruise = 0.7; t.engines = 1.8; t.haste = 8.0;     // small, quick, and able to run
        break;
    case FactionKind::FreePort:
        t.grade = 1; t.reactor = BURNS_DEUTERIUM;
        break;
    default:
        t.grade = 1;
        break;
    }
    return t;
}

// Fits out a hull around so many units of what it carries; false if it won't all go in
struct Fit;
static bool FitOut(ShipBuild &b, const Taste &t, int role, int units, int unitSize, double &cruise, const Fit &fit);

// Which model of each part a design is built around (partsdata.h)
struct Fit { int reactor = 0, warp = 0, hyper = 0, cap = 0; };

static bool FitOut(ShipBuild &b, const Taste &t, int role, int units, int unitSize, double &cruise, const Fit &fit)
{
    auto grade = [&](PartType type) { return t.slant != Slant::Count && Specialises(t.slant, type) ? t.grade + 1 : t.grade; };
    const PartType load = role == ROLE_LINER ? PartType::Cabins : PartType::Hold;
    const double perUnit = role == ROLE_LINER ? 0.0 : PartRating(PartType::Hold) * pow(2.0, unitSize - 1);       // tonnes each carries (people weigh next to nothing)
    double mass = Hull(b.hull).mass * 1.3 + units * perUnit * 1.1 + 60.0, fuel = 40.0;
    cruise = t.cruise;
    for (int pass = 0; pass < 5; pass++) {
        b.parts.clear();
        for (int i = 0; i < units; i++) b.parts.push_back(Make(load, unitSize, grade(load)));
        double full = mass + fuel;
        // Drives that can move it, and a capacitor that can jump it
        const double warpMoves = PartRating(PartType::WarpDrive, 0, fit.warp), hyperMoves = PartRating(PartType::HyperDrive, 0, fit.hyper);
        int warp = SizeFor(1.05 * full, warpMoves), hyper = SizeFor(1.05 * full, hyperMoves);
        if (warpMoves * pow(2.0, warp - 1) < full || hyperMoves * pow(2.0, hyper - 1) < full) return false;      // (nothing made will shift it)
        if (role != ROLE_HAULER) b.parts.push_back(Make(PartType::WarpDrive, warp, grade(PartType::WarpDrive), fit.warp));
        b.parts.push_back(Make(PartType::HyperDrive, hyper, grade(PartType::HyperDrive), fit.hyper));
        ShipSpec probe = SpecOf(b);
        if (role != ROLE_HAULER) {
            double jump = probe.jumpTJ * full / PartModelOf(PartType::DriveCap, fit.cap).efficiency * 1.15, each = PartRating(PartType::DriveCap, grade(PartType::DriveCap), fit.cap);
            int cap = SizeFor(jump, each);
            b.parts.push_back(Make(PartType::DriveCap, cap, grade(PartType::DriveCap), fit.cap));
            if (each * pow(2.0, cap - 1) < jump) b.parts.push_back(Make(PartType::DriveCap, cap, grade(PartType::DriveCap), fit.cap));
        }
        // A reactor that holds the field at the speed it cruises at (or the speed comes down to what the biggest will hold)
        double perSize = PartRating(PartType::Reactor, grade(PartType::Reactor), fit.reactor);
        double need = 0.0;
        for (int tries = 0; tries < 12; tries++) {
            double field = role == ROLE_HAULER ? DrivePower(probe, cruise * probe.hyperSpeed, full, false) : DrivePower(probe, cruise * probe.warpSpeed, full, true);
            need = 1.1 * (field + 384.0);
            if (need <= perSize * pow(2.0, PART_SIZES - 1) || cruise <= 0.2) break;
            cruise *= 0.9;
        }
        b.parts.push_back(Make(PartType::Reactor, SizeFor(need, perSize), grade(PartType::Reactor), fit.reactor));
        b.parts.push_back(Make(PartType::Engine, SizeFor(0.3 * 21.0 * full * t.engines, PartRating(PartType::Engine)), grade(PartType::Engine)));
        b.parts.push_back(Make(PartType::Thrusters, SizeFor(50.0 * full * t.engines, PartRating(PartType::Thrusters)), grade(PartType::Thrusters)));
        b.parts.push_back(Make(PartType::Radar, 1, grade(PartType::Radar)));
        // Tanks for the longest lane and home again
        double left = fuel * 1.1;
        const double tank = PartRating(PartType::Tank);
        while (left > 0.0) { int s = std::min(PART_SIZES, SizeFor(left, tank)); b.parts.push_back(Make(PartType::Tank, s)); left -= tank * pow(2.0, s - 1); }
        // ...which says what it weighs, and so what all of that should have been
        ShipSpec s = SpecOf(b);
        double loaded = s.dryMass + (role == ROLE_LINER ? s.berths * PERSON_CABIN : s.hold);
        double eff = s.reactorEff * s.fuelTJ, v = cruise * (role == ROLE_HAULER ? s.hyperSpeed : s.warpSpeed);
        double burn = role == ROLE_HAULER ? 6.0 * ReactorBurnFor(s, DrivePower(s, v, loaded, false) + s.idleTW) * (LOCAL_LEG_AU * AU_U / v)
                                          : 2.0 * (s.jumpTJ * loaded / (s.capIn * s.capOut) / eff +
                                                   ReactorBurnFor(s, DrivePower(s, v, loaded, true) + s.idleTW) * (FLEET_RANGE_LY * LY_U / v));
        if (fabs(loaded - mass) < 0.02 * mass && fabs(burn - fuel) < 0.05 * fuel + 1.0) { mass = loaded; fuel = burn; break; }
        mass = loaded; fuel = burn;
    }
    return VolumeUsed(b) <= BuildVolume(b) && HardpointsUsed(b) <= BuildHardpoints(b);
}

// The time and the fuel for a run, from the flight model's own laws
static void TripFigures(ShipDesign &d, int role)
{
    const ShipSpec &s = d.spec;
    // (charging for a jump, a reactor runs flat out; under way it burns what holds the field, at whatever efficiency it has there)
    const double eff = s.reactorEff * s.fuelTJ, spare = s.reactorTW - s.idleTW;
    // (in a system: under the hyper field, as fast as it's run or as the reactor will hold)
    double hyper = fmin(d.cruise * s.hyperSpeed, DriveSpeedFor(s, spare, d.mass, false));
    hyper = fmax(hyper, 0.02 * AU_U);
    auto leg = [&](double au, double &sec, double &fuel) {
        double t = au * AU_U / hyper;
        sec += t; fuel += ReactorBurnFor(s, DrivePower(s, hyper, d.mass, false) + s.idleTW) * t;
    };
    d.localSec = DOCK_SECONDS; d.localFuel = ReactorBurnFor(s, s.idleTW) * DOCK_SECONDS;
    leg(LOCAL_LEG_AU, d.localSec, d.localFuel);
    if (role == ROLE_HAULER || s.warpSpeed <= 0.0) { d.fixedSec = d.localSec; d.fixedFuel = d.localFuel; d.secPerLy = 1e9; d.fuelPerLy = 1e9; return; }
    // Between systems: out to where it can jump, the jump, the passage, and in at the other end
    double warp = fmin(d.cruise * s.warpSpeed, DriveSpeedFor(s, spare, d.mass, true));
    warp = fmax(warp, s.hyperSpeed);
    d.fixedSec = DOCK_SECONDS; d.fixedFuel = ReactorBurnFor(s, s.idleTW) * DOCK_SECONDS;
    leg(SYSTEM_LEG_AU, d.fixedSec, d.fixedFuel); leg(SYSTEM_LEG_AU, d.fixedSec, d.fixedFuel);
    const double charge = s.jumpTJ * d.mass / (s.capIn * s.capOut);           // (the bubble, and what's lost charging for it and letting it go)
    d.fixedFuel += charge / eff;
    // (it charges on the way out, with what the reactor has to spare: a small reactor may have to wait at the edge for the rest)
    {
        double out = SYSTEM_LEG_AU * AU_U / hyper, onTheWay = fmax(0.0, spare - DrivePower(s, hyper, d.mass, false)) * out;
        if (onTheWay < charge && spare > 0.0) { double wait = (charge - onTheWay) / spare; d.fixedSec += wait; d.fixedFuel += s.idleTW * wait / eff; }
    }
    d.secPerLy = LY_U / warp;
    d.fuelPerLy = ReactorBurnFor(s, DrivePower(s, warp, d.mass, true) + s.idleTW) * d.secPerLy;
}

// A design for a job around those parts ('fits': false if nothing would go in the hull)
static ShipDesign Draw(const Taste &t, int role, const Fit &fit, bool &fits)
{
    ShipDesign d;
    // The hull for the job; and as much of what it carries as will go in with everything it takes to move it
    int hull = role == ROLE_HAULER ? HULL_HAULER : role == ROLE_BULK && !t.small ? HULL_BULK : HULL_FREIGHTER;
    if (role == ROLE_HAULER && !t.small && t.stretch >= 3) hull = HULL_FREIGHTER;
    int unitSize = hull == HULL_BULK ? 6 : hull == HULL_FREIGHTER ? 5 : 3;
    fits = false;
    double most = 0.0;
    for (int stretch = std::min(t.stretch, MAX_STRETCH); stretch >= 0 && !fits; stretch--)
        for (int size = unitSize; size >= 1; size--)
            for (int units = 8; units >= 1; units--) {
                if (units * pow(2.0, size - 1) <= most) break;                // (no better than what's in hand already)
                ShipBuild b;
                b.hull = hull; b.stretch = (uint8_t)stretch;
                double cruise = t.cruise;
                if (!FitOut(b, t, role, units, size, cruise, fit)) continue;
                d.build = b; d.cruise = cruise; fits = true; most = units * pow(2.0, size - 1);
                break;
            }
    if (!fits) { d.build = NpcBuild(NpcKind::Freighter, 60.0); d.cruise = t.cruise; }      // (a plain freighter, if it comes to that)
    d.spec = SpecOf(d.build);
    d.cargo = role == ROLE_LINER ? 0.0 : d.spec.hold;
    d.berths = role == ROLE_LINER ? d.spec.berths : 0.0;
    d.fuelGood = d.spec.fuelGood;
    d.mass = d.spec.dryMass + d.cargo + d.berths * PERSON_CABIN + 0.6 * d.spec.tank;
    TripFigures(d, role);
    d.value = BuildValue(d.build);
    return d;
}

// What carrying a tonne (or a passenger) a usual run in it costs its owner: the fuel, the rest of running it, and the
// ship's own time, which is worth more to some than to others
static double Bill(const ShipDesign &d, int role, const Taste &t)
{
    double carries = role == ROLE_LINER ? d.berths : d.cargo;
    if (carries <= 0.0) return 1e30;
    double fuel = role == ROLE_HAULER ? d.localFuel : d.Fuel(USUAL_RUN_LY), sec = role == ROLE_HAULER ? d.localSec : d.Seconds(USUAL_RUN_LY);
    return (fuel * GoodData(d.fuelGood).basePrice * (1.0 + RUNNING_COSTS) + t.haste * (role == ROLE_LINER ? 3.0 : 1.0) * d.value * sec / (SHIP_LIFE_DAYS * MARKET_DAY)) / carries;
}

ShipDesign DesignShip(const FactionSet &fs, int faction, int role)
{
    const Faction *f = fs.Get(faction);
    Taste t = TasteOf(f);
    // A ship that jumps for a living is built around its fuel bill: even with the reactor and drives that suit it best,
    // fission fuel for a run weighs a twenty-fifth of the cargo, and human space can't make that much. So long-haul
    // ships get a fuel better than their builders' ships about the system do. (Not the clans': they fly what they can
    // keep running.) That's as intended: fission is for work about a system, fusion for between them.
    if (role != ROLE_HAULER && !(f && f->kind == FactionKind::Clan) && t.reactor < BURNS_HELIUM3) t.reactor++;
    // (many factions are of a kind, and a design takes some working out: once for each kind)
    static std::map<std::string, ShipDesign> drawn;
    std::string key = TextFormat("%llx %d %d %d %d %.3f %.3f %d %d %.3f", (unsigned long long)PartsHash(), role, t.grade, (int)t.slant, (int)t.reactor, t.cruise, t.engines, t.stretch,
                                 (int)t.small, t.haste);
    auto known = drawn.find(key);
    if (known == drawn.end()) {
        // The parts for the job. Its builders' character says what fuels they can build a reactor for; within that,
        // whatever combination of reactor, drives and capacitor carries a tonne cheapest: tried a part at a time,
        // from the everyday ones, until no change is for the better.
        Fit fit;
        fit.reactor = t.reactor;
        bool fits = false;
        ShipDesign best = Draw(t, role, fit, fits);
        double bill = fits ? Bill(best, role, t) : 1e30;
        const PartType kinds[4] = { PartType::Reactor, PartType::WarpDrive, PartType::DriveCap, PartType::HyperDrive };
        for (int round = 0; round < 3; round++) {
            bool better = false;
            for (PartType kind : kinds) {
                if (role == ROLE_HAULER && (kind == PartType::WarpDrive || kind == PartType::DriveCap)) continue;       // (it has neither)
                int &which = kind == PartType::Reactor ? fit.reactor : kind == PartType::WarpDrive ? fit.warp : kind == PartType::DriveCap ? fit.cap : fit.hyper;
                const int now = which;
                int chosen = now;
                for (int v = 0; v < ModelCount(kind); v++) {
                    if (v == now) continue;
                    if (kind == PartType::Reactor) {               // (only what they can fuel)
                        bool can = false;
                        for (int fuel = 0; fuel <= t.reactor && fuel < REACTOR_FUELS; fuel++) if (FuelGoodOf(v) == FuelGoodOf(fuel)) can = true;
                        if (!can) continue;
                    }
                    which = v;
                    bool ok = false;
                    ShipDesign d = Draw(t, role, fit, ok);
                    double b = ok ? Bill(d, role, t) : 1e30;
                    if (b < 0.995 * bill) { bill = b; best = d; chosen = v; better = true; }
                }
                which = chosen;
            }
            if (!better) break;
        }
        // What a yard builds it from: the hull is plating, and each part is what it's made of
        best.materials[GOOD_HULL_PLATING] += (float)(Hull(best.build.hull).mass * (1.0 + Parts().rules.stretchMass * best.build.stretch));
        for (const Part &p : best.build.parts) { PartInfo i = Describe(p); if (i.material >= 0) best.materials[i.material] += (float)i.tonnes; }
        for (int g = 0; g < GOOD_COUNT; g++) best.tonnage += best.materials[g];
        known = drawn.insert({ key, best }).first;
    }
    ShipDesign d = known->second;
    d.name = (f ? f->name : std::string("Independent")) + " " + ShipRoleName(role);
    return d;
}
