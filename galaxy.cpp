// galaxy.cpp - generating the galaxy: stars, systems, worlds, belts and rings
#include "galaxy.h"
#include <algorithm>
#include <array>
#include <map>

static const Color MOON_COLOR = { 150, 150, 150, 255 };

const char *CompositionName(Composition c)
{
    switch (c) {
    case Composition::Metallic:     return "Metallic (M-type)";
    case Composition::Stony:        return "Stony (S-type)";
    case Composition::Carbonaceous: return "Carbonaceous (C-type)";
    case Composition::Icy:          return "Water, ammonia and methane ice";
    default:                        return "Mixed";
    }
}

const char *EconName(Econ e)
{
    switch (e) {
    case Econ::HighTech:     return "High-tech";
    case Econ::Agricultural: return "Agricultural";
    case Econ::Colony:       return "Frontier colony";
    case Econ::GasMining:    return "Gas mining";
    case Econ::Research:     return "Research";
    case Econ::Mining:       return "Mining";
    case Econ::Industrial:   return "Industrial";
    case Econ::Refinery:     return "Ore refining";
    case Econ::Skimmer:      return "Gas refining";
    case Econ::Freeport:     return "Free trade";
    case Econ::Shipyard:     return "Shipbuilding";
    case Econ::Power:        return "Power generation";
    default:                 return "None";
    }
}

const char *SettlementName(Settlement s)
{
    switch (s) {
    case Settlement::CoreWorld:         return "Core world";
    case Settlement::SettledWorld:      return "Settled world";
    case Settlement::DomedColonies:     return "Domed colonies";
    case Settlement::OrbitalHabitats:   return "Orbital habitats, gas mining";
    case Settlement::ResearchStation:   return "Research station";
    case Settlement::ResearchOutpost:   return "Research outpost";
    case Settlement::MiningOutpost:     return "Mining outpost";
    case Settlement::UndergroundColony: return "Underground colony";
    case Settlement::OrbitalPort:       return "Orbital port";
    case Settlement::Shipyard:          return "Orbital shipyard";
    case Settlement::SkimmerStation:    return "Gas skimmer station";
    case Settlement::OreTerminal:       return "Ore terminal";
    case Settlement::OreRefinery:       return "Ore refinery";
    case Settlement::Waystation:        return "Waystation";
    case Settlement::Freeport:          return "Freeport";
    case Settlement::Starbase:          return "Starbase";
    case Settlement::SolarCollector:    return "Solar collector";
    case Settlement::Greenhouse:        return "Greenhouse station";
    default:                            return "Uninhabited";
    }
}

const char *ResourceName(int r)
{
    static const char *names[RES_COUNT] = { "Metals", "Precious metals", "Minerals", "Ices", "Gases", "Biomass" };
    return r >= 0 && r < RES_COUNT ? names[r] : "";
}

// What a settlement's market makes and needs
static Econ EconFor(Settlement s)
{
    switch (s) {
    case Settlement::CoreWorld:         return Econ::HighTech;
    case Settlement::SettledWorld:      return Econ::Agricultural;
    case Settlement::DomedColonies:     return Econ::Colony;
    case Settlement::OrbitalHabitats:   return Econ::GasMining;
    case Settlement::MiningOutpost:     return Econ::Mining;
    case Settlement::UndergroundColony: return Econ::Industrial;
    case Settlement::ResearchStation:
    case Settlement::ResearchOutpost:   return Econ::Research;
    default:                            return Econ::None;
    }
}

// Random draws for generation, from core.h's portable generator: the same seed
// gives the same galaxy on every machine and compiler (multiplayer relies on it).
struct Gen {
    Rng64 r;
    explicit Gen(uint64_t seed) : r{ Mix64(seed) } {}
    double uni(double a, double b)    { return a + (b - a) * r.uni(); }
    double logUni(double a, double b) { return a * pow(b / a, r.uni()); }
    int    irng(int a, int b)         { return a + (int)(r.uni() * (b - a + 1)); }      // a..b inclusive
    bool   chance(double p)           { return r.uni() < p; }
    double gauss()                    { double u = fmax(r.uni(), 1e-300), v = r.uni(); return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v); }
    uint64_t bits()                   { r.uni(); return r.s; }
};
static double Clamp01(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }
static double ClampD(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

// Surface temperature of a world with oceans, from where it sits in the habitable
// zone: ~312 K at the inner edge down to ~276 K at the outer edge
static double TemperateK(double flux)
{
    double u = (log(flux) - log(HZ_FLUX_OUTER)) / (log(HZ_FLUX_INNER) - log(HZ_FLUX_OUTER));
    return 276.0 + 36.0 * Clamp01(u);
}

// Rock density at a point (star-relative): thins out toward the edges
double ZoneDensity(const Zone &z, DVec3 p)
{
    DVec3  q = p - z.center;
    double h = Dot(q, z.n);
    if (fabs(h) > z.halfH) return 0.0;
    double r = Len(q - z.n * h);
    if (r < z.rIn || r > z.rOut) return 0.0;
    double vert = 1.0 - (h / z.halfH) * (h / z.halfH);
    if (z.ring) {
        for (size_t i = 0; i + 1 < z.bandEdges.size(); i++)
            if (r >= z.bandEdges[i] && r < z.bandEdges[i + 1]) return z.density * z.bandDensity[i] * vert;
        return 0.0;
    }
    double edge = (z.rOut - z.rIn) * 0.15;
    double rad  = fmin(1.0, (r - z.rIn) / edge) * fmin(1.0, (z.rOut - r) / edge);
    return z.density * vert * rad;
}

// Nearest point on a zone's middle circle: where you arrive when you target it
DVec3 ZoneTargetPoint(const Zone &z, DVec3 shipLocal)
{
    DVec3 q = shipLocal - z.center;
    DVec3 inPlane = q - z.n * Dot(q, z.n);
    DVec3 dir = Len(inPlane) > 1.0 ? Norm(inPlane) : z.u;
    return z.center + dir * (0.5 * (z.rIn + z.rOut));
}

// How far out of a system's gravity field a point is: < 1 inside, > 1 outside.
double FieldRatio(const StarSystem &sys, DVec3 fromStar)
{
    double h = Dot(fromStar, sys.n);
    double r = Len(fromStar - sys.n * h);
    return sqrt((r / sys.fieldR) * (r / sys.fieldR) + (h / sys.fieldH) * (h / sys.fieldH));
}

// ---------------------------------------------------------------------------
// Stars
// ---------------------------------------------------------------------------
// Main-sequence relations: luminosity and radius (in Suns) from mass
static double MainSequenceLum(double m)    { return m < 0.43 ? 0.23 * pow(m, 2.3) : m < 2.0 ? pow(m, 4.0) : 1.4 * pow(m, 3.5); }
static double MainSequenceRadius(double m) { return m < 1.0 ? pow(m, 0.8) : pow(m, 0.57); }
static double MainSequenceLifeGyr(double m){ return 10.0 * pow(m, -2.5); }

// Spectral class by temperature; subclass 0-9 runs hot to cool within the class
static void Classify(StarInfo &s)
{
    struct Band { char cls; double hot, cool; Color color; };
    static const Band bands[] = {
        { 'O', 50000, 30000, { 140, 170, 255, 255 } }, { 'B', 30000, 10000, { 170, 200, 255, 255 } },
        { 'A', 10000, 7500,  { 225, 235, 255, 255 } }, { 'F', 7500,  6000,  { 255, 250, 225, 255 } },
        { 'G', 6000,  5200,  { 255, 232, 140, 255 } }, { 'K', 5200,  3700,  { 255, 175, 85, 255 } },
        { 'M', 3700,  2300,  { 255, 105, 65, 255 } },
    };
    for (const Band &b : bands) {
        if (s.temp >= b.cool || b.cls == 'M') {
            s.cls   = b.cls;
            s.sub   = (int)ClampD((b.hot - s.temp) / (b.hot - b.cool) * 10.0, 0.0, 9.0);
            s.color = b.color;
            break;
        }
    }
}

// Light from size and temperature (Suns)
static double LumFrom(double radiusSuns, double tempK) { return radiusSuns * radiusSuns * pow(tempK / 5772.0, 4.0); }

static StarInfo MainSequenceStar(double m)
{
    StarInfo s;
    double rSun = MainSequenceRadius(m);
    s.kind = StarKind::MainSequence;
    s.mass = m;
    s.lum  = MainSequenceLum(m);
    s.radius = rSun * RSUN_U;
    s.temp   = 5772.0 * pow(s.lum / (rSun * rSun), 0.25);
    Classify(s);
    return s;
}

// A star that has run out of hydrogen and swollen up: cool, huge and bright
static StarInfo GiantStar(double m, Gen &g)
{
    StarInfo s;
    double rSun = g.logUni(6.0, 60.0);
    s.kind = StarKind::Giant;
    s.mass = m;
    s.temp = 4900.0 - 1200.0 * log10(rSun / 6.0) + g.uni(-150.0, 150.0);
    s.radius = rSun * RSUN_U;
    s.lum  = LumFrom(rSun, s.temp);
    Classify(s);
    return s;
}

// The cooling core a Sun-like star leaves behind: the size of the Earth
static StarInfo WhiteDwarf(double m, double coolingGyr)
{
    StarInfo s;
    double rSun = 0.0126 * pow(m / 0.6, -1.0 / 3.0);
    s.kind = StarKind::WhiteDwarf;
    s.mass = m;
    s.temp = ClampD(25000.0 * pow(coolingGyr / 0.05, -0.35), 3500.0, 60000.0);
    s.radius = rSun * RSUN_U;
    s.lum  = LumFrom(rSun, s.temp);
    s.cls  = 'D';
    s.sub  = (int)ClampD(50400.0 / s.temp + 0.5, 1.0, 9.0);
    s.color = s.temp > 9000 ? Color{ 200, 215, 255, 255 } : s.temp > 6000 ? Color{ 240, 240, 250, 255 } : Color{ 255, 235, 200, 255 };
    return s;
}

// What a massive star's supernova leaves: more than a Sun, 22 km across
static StarInfo NeutronStar(double m, double ageGyr)
{
    StarInfo s;
    double rSun = 11.0 * KM_U / RSUN_U;
    s.kind = StarKind::NeutronStar;
    s.mass = m;
    s.temp = ClampD(6.0e5 * pow(ageGyr / 0.001, -0.25), 3.0e4, 1.0e6);
    s.radius = 11.0 * KM_U;
    s.lum  = LumFrom(rSun, s.temp);
    s.cls  = 'N';
    s.sub  = 0;
    s.color = { 195, 180, 255, 255 };
    return s;
}

// Too small to burn hydrogen: glows dimly with the heat it was born with
static StarInfo BrownDwarf(double m, double ageGyr, Gen &g)
{
    StarInfo s;
    double rSun = 0.1 * g.uni(0.9, 1.15);
    s.kind = StarKind::BrownDwarf;
    s.mass = m;
    s.temp = ClampD(2200.0 * pow(m / 0.07, 0.8) * pow(fmax(ageGyr, 0.1), -0.3), 250.0, 2400.0);
    s.radius = rSun * RSUN_U;
    s.lum  = LumFrom(rSun, s.temp);
    double hot = s.temp >= 1300 ? 2400 : s.temp >= 500 ? 1300 : 500, cool = s.temp >= 1300 ? 1300 : s.temp >= 500 ? 500 : 250;
    s.cls  = s.temp >= 1300 ? 'L' : s.temp >= 500 ? 'T' : 'Y';
    s.sub  = (int)ClampD((hot - s.temp) / (hot - cool) * 10.0, 0.0, 9.0);
    s.color = s.cls == 'L' ? Color{ 210, 80, 60, 255 } : s.cls == 'T' ? Color{ 170, 60, 110, 255 } : Color{ 110, 60, 120, 255 };
    return s;
}

const char *StarClassCode(const StarInfo &s)
{
    switch (s.kind) {
    case StarKind::Giant:       return TextFormat("%c%d III", s.cls, s.sub);
    case StarKind::WhiteDwarf:  return TextFormat("DA%d", s.sub);
    case StarKind::NeutronStar: return "NS";
    case StarKind::BrownDwarf:  return TextFormat("%c%d", s.cls, s.sub);
    default:                    return TextFormat("%c%d V", s.cls, s.sub);
    }
}

const char *StarDescription(const StarInfo &s)
{
    switch (s.kind) {
    case StarKind::Giant:
        return TextFormat("%c%d III %s", s.cls, s.sub, s.cls == 'M' ? "red giant" : s.cls == 'K' ? "orange giant" : "yellow giant");
    case StarKind::WhiteDwarf:  return TextFormat("DA%d white dwarf", s.sub);
    case StarKind::NeutronStar: return "Neutron star";
    case StarKind::BrownDwarf:  return TextFormat("%c%d brown dwarf", s.cls, s.sub);
    default: break;
    }
    const char *kind = s.cls == 'O' ? "blue star" : s.cls == 'B' ? "blue-white star" :
                       s.cls == 'A' ? "white star" : s.cls == 'F' ? "yellow-white star" :
                       s.cls == 'G' ? "yellow dwarf" : s.cls == 'K' ? "orange dwarf" : "red dwarf";
    return TextFormat("%c%d V %s", s.cls, s.sub, kind);
}

// The stars of a system, in the proportions of the real galaxy: about three in
// four are red dwarfs, Sun-like stars are a few percent, and the bright blue
// ones are rare finds. Also sets the system's age.
static void GenerateStars(StarSystem &sys)
{
    Gen g(sys.seed ^ 0x2F6B1D3Bull);

    // What the primary is, as a share of all systems
    enum { BD, M, K, G, F, A, B, O, WD, GIANT, NS, KINDS };
    static const double share[KINDS] = { 0.080, 0.650, 0.115, 0.068, 0.027, 0.0055, 0.0011, 0.00003, 0.045, 0.006, 0.0025 };
    static const double mLo[KINDS]   = { 0.013, 0.08, 0.45, 0.80, 1.04, 1.4, 2.1, 16.0 };
    static const double mHi[KINDS]   = { 0.075, 0.45, 0.80, 1.04, 1.40, 2.1, 16.0, 60.0 };
    double r = g.uni(0, 1), acc = 0.0;
    int pick = M;
    for (int k = 0; k < KINDS; k++) { acc += share[k]; if (r < acc) { pick = k; break; } }

    double disk = g.uni(0.3, 11.0);                      // how long ago this part of the disk made it
    StarInfo primary;
    if (pick == WD) {
        double m = ClampD(0.6 + 0.12 * g.gauss(), 0.45, 1.3);
        double progenitor = ClampD((m - 0.394) / 0.109, 0.9, 7.5);
        double cooling = g.uni(0.05, 9.0);
        sys.age = fmin(12.5, MainSequenceLifeGyr(progenitor) + cooling);
        primary = WhiteDwarf(m, cooling);
    } else if (pick == GIANT) {
        double m = g.logUni(0.9, 3.0);
        sys.age = fmin(12.5, MainSequenceLifeGyr(m) * 1.1);
        primary = GiantStar(m, g);
    } else if (pick == NS) {
        sys.age = g.logUni(0.001, 10.0);
        primary = NeutronStar(g.uni(1.2, 2.0), sys.age);
    } else if (pick == BD) {
        sys.age = disk;
        primary = BrownDwarf(g.logUni(mLo[BD], mHi[BD]), sys.age, g);
    } else {
        double m = g.logUni(mLo[pick], mHi[pick]);
        sys.age = fmin(disk, MainSequenceLifeGyr(m) * g.uni(0.05, 0.95));   // still burning hydrogen
        primary = MainSequenceStar(m);
    }
    sys.stars.clear();
    sys.stars.push_back(primary);

    // Company: heavier stars are more often born in pairs and threes
    double m1 = primary.mass;
    double pMulti = primary.kind == StarKind::MainSequence ? ClampD(0.22 + 0.24 * log10(m1 / 0.1), 0.15, 0.9)
                  : primary.kind == StarKind::Giant ? 0.4 : primary.kind == StarKind::WhiteDwarf ? 0.25 : 0.12;
    int companions = g.chance(pMulti) ? (g.chance(0.22) ? 2 : 1) : 0;
    for (int c = 0; c < companions; c++) {
        StarInfo s;
        double m2 = primary.kind == StarKind::MainSequence || primary.kind == StarKind::Giant ? m1 * g.uni(0.12, 0.98)
                  : primary.kind == StarKind::BrownDwarf ? m1 * g.uni(0.4, 1.0) : g.logUni(0.05, 0.6);
        if (primary.kind == StarKind::MainSequence && m1 > 1.0 && g.chance(0.06))
            s = WhiteDwarf(ClampD(0.6 + 0.1 * g.gauss(), 0.45, 1.0), g.uni(0.05, fmax(0.1, sys.age * 0.6)));   // like Sirius B
        else if (m2 < 0.075) s = BrownDwarf(fmax(m2, 0.013), sys.age, g);
        else                 s = MainSequenceStar(m2);
        // A binary is close or wide; a trinary has one of each
        s.close = companions == 2 ? (c == 0) : g.chance(0.5);
        if (s.close) s.sep = fmax(g.logUni(0.02, 0.4), 2.5 * (primary.radius + s.radius) / AU_U);
        else         s.sep = g.uni(1.5, 3.0);
        sys.stars.push_back(s);
    }

    sys.lumInner = 0.0;
    for (size_t i = 0; i < sys.stars.size(); i++)
        if (i == 0 || sys.stars[i].close) sys.lumInner += sys.stars[i].lum;
    // Habitable zone (where an Earth-like planet could keep liquid water), from total light
    sys.hzIn  = 0.95 * sqrt(sys.lumInner) * AU_U;
    sys.hzOut = 1.67 * sqrt(sys.lumInner) * AU_U;
    sys.starRadius = sys.stars[0].radius;
}

// ---------------------------------------------------------------------------
// The layout of a system: which orbits hold what
// ---------------------------------------------------------------------------
// Worked out twice for every system: cheaply when its star first appears (to
// size its gravity field), and again when its contents load.
//
// Planets form in a disk around the young star, between where dust can first
// survive its heat and the disk's outer edge, in orbits each about 1.5 - 2 times
// wider than the last. Past the frost line there's ice to build with, so that's
// where giants grow. Small stars have small disks and few giants; the hottest
// stars blow their disks away before much can form at all. Stars that have
// since died took their inner planets with them.
enum class PCls : int8_t { Rocky, MiniNeptune, IceGiant, GasGiant, Dwarf };
struct PlanetPlan { double a; PCls cls; double mass; bool icy; };     // orbit (units), mass (Earths), formed beyond the frost line
struct BeltPlan   { double rIn, rOut; bool ice, outer; };              // outer: the scattered belt beyond the last planet
struct SystemPlan {
    std::vector<PlanetPlan> planets;      // in order of orbit, dwarf planets included
    std::vector<BeltPlan>   belts;
    double frost = 0;                     // frost line when the planets formed (units)
    double innerMass = 1;                 // what the planets circle: the primary and any close companion (Suns)
    double edge = 0;                      // farthest thing that orbits the primary (units)
};

static SystemPlan PlanSystem(const StarSystem &sys)
{
    Gen g(sys.seed ^ 0x9E3779B1ull);
    SystemPlan plan;
    const StarInfo &st = sys.stars[0];

    // The star as it was when its planets formed
    double mForm = st.mass, lForm = sys.lumInner, rForm = st.radius;
    double closeSep = 0.0;
    plan.innerMass = 0.0;
    for (size_t i = 0; i < sys.stars.size(); i++)
        if (i == 0 || sys.stars[i].close) {
            plan.innerMass += sys.stars[i].mass;
            if (i > 0) closeSep = fmax(closeSep, sys.stars[i].sep * AU_U);
        }
    if (st.kind == StarKind::WhiteDwarf) mForm = ClampD((st.mass - 0.394) / 0.109, 0.9, 7.5);
    if (st.kind == StarKind::WhiteDwarf || st.kind == StarKind::Giant) {
        lForm = MainSequenceLum(mForm);
        rForm = MainSequenceRadius(mForm) * RSUN_U;
    }
    plan.frost = 2.7 * sqrt(lForm) * AU_U;

    if (st.kind == StarKind::NeutronStar) {
        // The supernova took everything. Now and then a few cinders gather from what fell back.
        if (g.chance(0.08)) {
            double a = g.uni(0.15, 0.4) * AU_U;
            int n = g.irng(1, 3);
            for (int i = 0; i < n; i++, a *= g.uni(1.4, 1.9)) plan.planets.push_back({ a, PCls::Rocky, g.logUni(0.02, 4.0), false });
        }
    } else {
        bool   brown = st.kind == StarKind::BrownDwarf;
        double rich  = pow(10.0, sys.metal);                    // heavy elements to build with (1 = the Sun's)
        double aMin  = fmax(fmax(0.035 * sqrt(lForm) * AU_U, 3.0 * rForm), 3.5 * closeSep);
        double aMax  = fmin(MAX_PLANET_ORBIT_AU, 35.0 * pow(mForm, 0.6) * g.uni(0.6, 1.4)) * AU_U;
        int    slots = (int)ClampD((3.0 + 7.0 * pow(fmin(mForm, 1.5), 0.4)) * g.uni(0.6, 1.1) * ClampD(pow(rich, 0.5), 0.5, 1.3) + 0.5, 1.0, 12.0);
        double a     = aMin * g.logUni(1.3, 8.0);
        double sizes = brown ? 0.3 : ClampD(pow(mForm, 0.5), 0.45, 1.3);      // small disks build small planets
        double pGiant = brown ? 0.0 : ClampD(0.5 * pow(mForm, 0.5) * pow(rich, 1.3), 0.03, 0.75);
        int    innerBelts = 0;

        for (int s = 0; s < slots && a <= aMax; s++) {
            // (every slot draws the same numbers whatever it turns out to hold)
            double occupied = g.uni(0, 1), kind = g.uni(0, 1), kind2 = g.uni(0, 1), massU = g.uni(0, 1), beltU = g.uni(0, 1);
            double edgeIn = g.uni(0.82, 0.9), edgeOut = g.uni(1.1, 1.22), step = g.uni(1.45, 2.2);
            bool beyond = a > plan.frost;
            auto mass = [&](double lo, double hi) { return lo * pow(hi / lo, massU); };
            bool isGiant = false;
            if (occupied < 0.8) {
                if (beyond && kind < pGiant * (a < 6.0 * plan.frost ? 1.0 : 0.5)) {
                    isGiant = true;
                    if (kind2 < (a < 4.0 * plan.frost ? 0.65 : 0.25))
                        plan.planets.push_back({ a, PCls::GasGiant, fmax(GAS_GIANT_MASS, mass(50.0, 1200.0) * fmin(1.0, pow(mForm, 0.7))), true });
                    else
                        plan.planets.push_back({ a, PCls::IceGiant, mass(GIANT_MASS, 48.0), true });
                } else if (beyond) {
                    plan.planets.push_back({ a, PCls::Rocky, mass(0.005, 4.0) * sizes, true });
                } else if (!brown && a < 0.6 * plan.frost && kind2 < 0.2) {
                    plan.planets.push_back({ a, PCls::MiniNeptune, mass(3.0, 13.0), false });
                } else {
                    plan.planets.push_back({ a, PCls::Rocky, mass(0.03, 5.0) * sizes, false });
                }
            } else if (s > 0 && innerBelts < 2 && beltU < 0.5) {
                // A planet never came together here: the rubble is still in orbit
                plan.belts.push_back({ a * edgeIn, a * edgeOut, false, false });
                innerBelts++;
                if (kind < 0.35) plan.planets.push_back({ a, PCls::Dwarf, 8e-5 * pow(10.0, massU), beyond });   // like Ceres
            }
            a *= step * (isGiant ? 1.15 : 1.0);                  // giants clear more room
        }

        // Once in a long while a giant migrates all the way in, clearing out its neighbours
        if (st.kind == StarKind::MainSequence && mForm > 0.6 && mForm < 2.0 && g.chance(0.012 * pow(rich, 1.2))) {
            double aHot = aMin * g.uni(1.1, 2.0);
            plan.planets.erase(std::remove_if(plan.planets.begin(), plan.planets.end(),
                                              [&](const PlanetPlan &p) { return p.a < 4.0 * aHot; }), plan.planets.end());
            plan.belts.erase(std::remove_if(plan.belts.begin(), plan.belts.end(),
                                            [&](const BeltPlan &b) { return b.rIn < 4.0 * aHot; }), plan.belts.end());
            plan.planets.insert(plan.planets.begin(), PlanetPlan{ aHot, PCls::GasGiant, g.logUni(80.0, 900.0), true });
        }

        // Dead and dying stars: what they swallowed is gone
        if (st.kind == StarKind::Giant) {
            double swallowed = 1.4 * st.radius;
            plan.planets.erase(std::remove_if(plan.planets.begin(), plan.planets.end(),
                                              [&](const PlanetPlan &p) { return p.a < swallowed; }), plan.planets.end());
            plan.belts.erase(std::remove_if(plan.belts.begin(), plan.belts.end(),
                                            [&](const BeltPlan &b) { return b.rIn < swallowed; }), plan.belts.end());
        }
        if (st.kind == StarKind::WhiteDwarf) {
            // ...and as the star shed most of its mass, the survivors' orbits widened
            double swallowed = 2.2 * sqrt(mForm / 1.5) * AU_U, widen = mForm / st.mass;
            std::vector<PlanetPlan> kept;
            for (const PlanetPlan &p : plan.planets) {
                bool lost = g.chance(0.3);
                if (p.a >= swallowed && p.a * widen <= MAX_PLANET_ORBIT_AU * AU_U && !lost) kept.push_back({ p.a * widen, p.cls, p.mass, p.icy });
            }
            plan.planets = kept;
            std::vector<BeltPlan> keptBelts;
            for (const BeltPlan &b : plan.belts)
                if (b.rIn >= swallowed && b.rOut * widen <= MAX_BELT_AU * AU_U) keptBelts.push_back({ b.rIn * widen, b.rOut * widen, b.ice, false });
            plan.belts = keptBelts;
        }

        // Beyond the last planet: a wide belt of leftover ice, and a few dwarf planets in it
        if (!brown && !plan.planets.empty() && mForm >= 0.2) {
            double last = plan.planets.back().a;
            double beltU = g.uni(0, 1), in = g.uni(1.35, 1.7), width = g.uni(1.3, 1.6), count = g.uni(0, 1);
            if (beltU < 0.6 && last * in * width <= MAX_BELT_AU * AU_U) {
                BeltPlan b = { last * in, last * in * width, true, true };
                plan.belts.push_back(b);
                int n = count < 0.3 ? 0 : count < 0.65 ? 1 : count < 0.9 ? 2 : 3;
                for (int i = 0; i < n; i++)
                    plan.planets.push_back({ b.rIn + (b.rOut - b.rIn) * (i + g.uni(0.15, 0.85)) / n, PCls::Dwarf, g.logUni(2e-4, 4e-3), true });   // like Pluto
            } else if (count < 0.25) {
                plan.planets.push_back({ last * g.uni(1.4, 2.0), PCls::Dwarf, g.logUni(2e-4, 4e-3), true });
            }
        }
    }

    std::sort(plan.planets.begin(), plan.planets.end(), [](const PlanetPlan &x, const PlanetPlan &y) { return x.a < y.a; });
    std::sort(plan.belts.begin(), plan.belts.end(), [](const BeltPlan &x, const BeltPlan &y) { return x.rIn < y.rIn; });
    for (const PlanetPlan &p : plan.planets) plan.edge = fmax(plan.edge, p.a * 1.03);      // (room for its moons)
    for (const BeltPlan &b : plan.belts)     plan.edge = fmax(plan.edge, b.rOut);
    return plan;
}

// Wide companions orbit well outside everything else
static double WideDistance(const StarInfo &s, const SystemPlan &plan)
{
    return fmin(s.sep * fmax(plan.edge, 15.0 * AU_U), fmax(1.3 * plan.edge, MAX_COMPANION_AU * AU_U));
}

// Radius (km) of a world from its mass (Earths)
static double WorldRadiusKm(double m, bool envelope, bool icy)
{
    if (envelope)                                           // deep hydrogen: Neptune 3.9 Earth radii, Jupiter 11
        return REARTH_KM * (m < 120.0 ? 3.9 * pow(m / 17.0, 0.55) : 11.4 * pow(m / 120.0, -0.02));
    if (m < 0.01)                                           // too small to squeeze itself: plain rock or ice
        return cbrt(3.0 * m * MEARTH_KG / (4.0 * PI * (icy ? 2000.0 : 3400.0))) / 1000.0;
    return REARTH_KM * 1.008 * pow(m, 0.279) * (icy ? 1.1 : 1.0);
}

// ---------------------------------------------------------------------------
// Worlds: climate, life, resources and settlements
// ---------------------------------------------------------------------------
// 'flux' is the starlight a world receives, in Earths (1 = what Earth gets).
// Equilibrium temperature follows from flux and albedo; the atmosphere then
// adds a greenhouse effect (Earth: +33 K, Venus: +500 K). Liquid surface water
// needs an atmosphere and 273-373 K. Native life favors liquid water and needs
// time: young systems have none, and nothing here has built a civilization.
// People live wherever they can make it work, habitable zone or not, but only
// inside human space ('develop' > 0), and more of them the nearer home.
struct WorldCtx {
    double flux;        // starlight received
    bool   icy;         // formed beyond the frost line: ice as well as rock
    double develop;     // human presence, 0..1
    int    lifeCap;     // the most life this system has had time for
    double metal;       // the system's heavy elements, [Fe/H]
};

static void DescribeWorld(Body &b, const Body *parent, const WorldCtx &ctx, Gen &g)
{
    auto uni    = [&](double lo, double hi) { return g.uni(lo, hi); };
    auto chance = [&](double p) { return g.chance(p); };
    const double flux = ctx.flux;
    auto teq    = [&](double albedo) { return 278.6 * pow(flux * (1.0 - albedo), 0.25); };
    auto rollLife = [&]() { double r = uni(0, 1); return std::min(ctx.lifeCap, r < 0.25 ? 0 : r < 0.55 ? 1 : r < 0.78 ? 2 : 3); };

    double m = b.massEarth;
    b.flux = flux;
    b.inHZ = flux >= HZ_FLUX_OUTER && flux <= HZ_FLUX_INNER;      // the same limits as the HZ rings

    bool   liquid = false, breathable = false, frozenWater = false;
    Color  col = MOON_COLOR;
    std::string lifeWhere;

    if (b.giant) {
        b.tempK = teq(0.35);                                         // cloud tops
        bool mini = m < GIANT_MASS, ice = m < GAS_GIANT_MASS;
        b.atmosphere = mini ? "Hydrogen, helium, water vapor" : ice ? "Hydrogen, helium, methane" : "Hydrogen, helium";
        b.water      = mini ? "Deep steam and water layers" : ice ? "Deep water-ammonia mantle" : "Traces in deep clouds";
        if (mini)     { b.type = b.tempK > 600 ? "Hot mini-Neptune" : "Mini-Neptune";
                        col  = b.tempK > 600 ? Color{ 170, 160, 255, 255 } : Color{ 150, 190, 230, 255 }; }
        else if (ice) { b.type = b.tempK > 600 ? "Hot Neptune" : "Ice giant";
                        col  = b.tempK > 600 ? Color{ 130, 150, 255, 255 } : Color{ 100, 200, 240, 255 }; }
        else          { b.type = b.tempK > 900 ? "Hot Jupiter" : b.tempK > 350 ? "Warm gas giant" : "Gas giant";
                        col  = b.tempK > 900 ? Color{ 255, 110, 60, 255 } : b.tempK > 350 ? Color{ 240, 165, 90, 255 }
                                             : Color{ 225, 190, 120, 255 }; }
        if (b.tempK > 230 && b.tempK < 400 && chance(0.06) && ctx.lifeCap >= 1) { b.lifeLevel = 1; lifeWhere = " (floating, cloud layer)"; }
    } else if (b.kind == Kind::Planet) {
        enum Atm { NONE, THIN_CO2, THIN_N2, N2_CO2, N2_CH4, THICK_CO2, N2_O2 };
        double t0 = teq(0.3);
        Atm atm;
        if (m < 0.02)        atm = NONE;                                    // too small to hold any air
        else if (m < 0.15)   atm = chance(0.35) ? (t0 > 175 ? THIN_CO2 : THIN_N2) : NONE;
        else if (t0 > 700)   atm = chance(0.35) ? THICK_CO2 : NONE;         // scorched, or stripped bare
        else if (t0 > 320)   atm = chance(0.55) ? THICK_CO2 : chance(0.5) ? THIN_CO2 : NONE;
        else if (t0 > 175)   atm = chance(0.8) ? N2_CO2 : THIN_CO2;
        else                 atm = chance(0.4) ? THIN_N2 : chance(0.3) ? N2_CH4 : NONE;
        double albedo = atm == THICK_CO2 ? 0.75 : atm == NONE ? 0.12 : atm == N2_CO2 ? 0.3 : 0.2;
        double green  = atm == THICK_CO2 ? uni(250, 480) : atm == N2_CO2 ? uni(15, 45) :
                        atm == N2_CH4 ? uni(10, 25) : atm == NONE ? 0.0 : uni(3, 12);
        b.tempK = teq(albedo) + green;

        // Liquid surface water: only inside the habitable zone, and only under a real
        // atmosphere (nitrogen with some CO2 - the kind that settles a world's climate).
        // Closer in than the zone, that air runs away into a Venus-like greenhouse and the
        // oceans boil off; farther out they freeze.
        if (atm == N2_CO2 && b.inHZ)                     b.tempK = TemperateK(flux) + (green - 30.0) * 0.2;
        else if (atm == N2_CO2 && flux > HZ_FLUX_INNER) { atm = THICK_CO2; b.tempK = teq(0.75) + 300.0 + green * 4.0; }
        else if (flux < HZ_FLUX_OUTER)                   b.tempK = fmin(b.tempK, 262.0);
        liquid = atm == N2_CO2 && b.inHZ;
        double ocean = liquid ? uni(0.05, 0.98) : 0.0;
        if (liquid) b.lifeLevel = rollLife();
        else if (atm != NONE && b.tempK > 180 && b.tempK < 273 && chance(0.15) && ctx.lifeCap >= 1) { b.lifeLevel = 1; lifeWhere = " (under the ice)"; }
        if (b.lifeLevel >= 2 && (atm == N2_CO2 || atm == THIN_CO2)) atm = N2_O2;   // photosynthesis oxygenates the air
        breathable = atm == N2_O2 && b.tempK > 250 && b.tempK < 322;

        static const char *atmName[] = { "None", "Thin carbon dioxide", "Thin nitrogen", "Nitrogen, carbon dioxide",
                                         "Nitrogen, methane", "Thick carbon dioxide (crushing)", "Nitrogen, oxygen" };
        b.atmosphere = std::string(atmName[atm]) + (breathable ? " - breathable" : "");
        if (liquid)                              b.water = ocean > 0.9 ? "Global ocean" : TextFormat("Oceans, %d%% of surface", (int)(ocean * 100));
        else if (atm != NONE && b.tempK < 273)   { b.water = chance(0.6) ? "Frozen oceans" : "Polar ice caps"; frozenWater = true; }
        else if (b.tempK < 200)                  { b.water = ctx.icy ? "Ice crust" : "Subsurface ice"; frozenWater = true; }
        else                                     b.water = "None";

        if (b.dwarf)                       { b.type = ctx.icy ? "Icy dwarf planet" : "Rocky dwarf planet";
                                             col = ctx.icy ? Color{ 190, 215, 230, 255 } : Color{ 165, 155, 145, 255 }; }
        else if (b.tempK > 1000)           { b.type = "Lava world";            col = { 255, 80, 40, 255 }; }
        else if (atm == THICK_CO2)         { b.type = "Greenhouse world";      col = { 240, 210, 120, 255 }; }
        else if (liquid && ocean > 0.9)    { b.type = "Ocean world";           col = { 60, 140, 255, 255 }; }
        else if (liquid && breathable && b.lifeLevel == 3) { b.type = "Garden world"; col = { 90, 225, 140, 255 }; }
        else if (liquid)                   { b.type = "Temperate terrestrial"; col = { 150, 200, 130, 255 }; }
        else if (b.tempK > 320)            { b.type = m < 0.2 ? "Hot rocky world" : "Desert world"; col = { 225, 150, 80, 255 }; }
        else if (b.tempK < 200 && (atm != NONE || ctx.icy)) { b.type = "Ice world"; col = { 210, 240, 255, 255 }; }
        else if (atm != NONE)              { b.type = "Tundra world";          col = { 170, 190, 210, 255 }; }
        else                               { b.type = "Barren rocky world";    col = { 170, 160, 150, 255 }; }
        if (m >= 2.0 && !b.dwarf) b.type += " (super-Earth)";
    } else {
        // Moons
        double t0 = teq(0.3);
        bool giantParent = parent && parent->giant && parent->massEarth >= GIANT_MASS;
        bool holdsAir = m > 0.012;                                    // Titan is 0.0225 Earths
        if (giantParent && b.orbit < parent->radius * 12.0 && chance(0.25)) {
            b.type = "Volcanic moon"; b.atmosphere = "Thin sulfur dioxide"; b.water = "None";
            b.tempK = teq(0.4) + uni(20, 60);                          // tidal heating
            col = { 240, 220, 90, 255 };
        } else if (t0 < 200) {
            if (holdsAir && chance(0.3)) {
                b.type = "Titan-like moon"; b.atmosphere = "Thick nitrogen, methane";
                b.water = "Methane lakes; water ocean below";
                b.tempK = teq(0.25) + uni(10, 20);
                frozenWater = true;
                if (chance(0.15) && ctx.lifeCap >= 1) { b.lifeLevel = 1; lifeWhere = " (subsurface ocean)"; }
                col = { 230, 190, 110, 255 };
            } else {
                bool sub = giantParent && chance(0.45);              // tidal heat keeps an ocean liquid under the ice
                b.type = "Icy moon"; b.atmosphere = "None"; b.water = sub ? "Subsurface ocean" : "Surface ice";
                b.tempK = teq(0.6);
                frozenWater = true;
                if (sub && chance(0.3) && ctx.lifeCap >= 1) { b.lifeLevel = 1; lifeWhere = " (subsurface ocean)"; }
                col = { 200, 235, 255, 255 };
            }
        } else if (holdsAir && b.inHZ && chance(0.35)) {
            b.tempK = TemperateK(flux) + (uni(15, 40) - 27.0) * 0.2;   // a moon big enough to hold air, in the zone
            liquid = true;
            b.lifeLevel = rollLife();
            breathable = b.lifeLevel >= 2 && b.tempK > 250 && b.tempK < 322;
            b.type = "Habitable moon";
            b.atmosphere = b.lifeLevel >= 2 ? (breathable ? "Nitrogen, oxygen - breathable" : "Nitrogen, oxygen") : "Nitrogen, carbon dioxide";
            b.water = TextFormat("Oceans, %d%% of surface", (int)uni(20, 90));
            col = { 110, 210, 150, 255 };
        } else {
            b.type = t0 > 320 ? "Scorched rocky moon" : "Rocky moon"; b.atmosphere = "None"; b.water = "None";
            b.tempK = teq(0.12);
            col = { 160, 155, 150, 255 };
        }
    }

    static const char *lifeName[] = { "None", "Microbial", "Simple multicellular", "Complex - flora and fauna" };
    b.life = std::string(lifeName[b.lifeLevel]) + lifeWhere;

    double rEarth = b.radius / KM_U / REARTH_KM;
    b.gravity = m / (rEarth * rEarth);

    // What's here to dig, pump, skim or grow (always the same draws, whatever the world)
    double rA = uni(0, 1), rB = uni(0, 1), rC = uni(0, 1), rD = uni(0, 1);
    if (b.giant) {
        b.resource[RES_GASES] = (float)(m >= GAS_GIANT_MASS ? 0.6 + 0.4 * rA : m >= GIANT_MASS ? 0.4 + 0.4 * rA : 0.2 + 0.3 * rA);
    } else {
        double heavy = Clamp01(0.5 + 0.8 * ctx.metal);                // metal-poor stars make metal-poor worlds
        double metals = ctx.icy ? 0.05 + 0.3 * rA : 0.25 + 0.6 * rA;
        b.resource[RES_METALS]   = (float)Clamp01(metals * (0.5 + heavy));
        b.resource[RES_PRECIOUS] = (float)Clamp01(b.resource[RES_METALS] * rB * rB * 1.6);
        b.resource[RES_MINERALS] = (float)Clamp01((ctx.icy ? 0.1 + 0.4 * rC : 0.25 + 0.7 * rC) * (0.6 + 0.8 * heavy));
        b.resource[RES_ICES]     = (float)(ctx.icy ? 0.5 + 0.5 * rD : frozenWater ? 0.2 + 0.4 * rD : 0.0);
        b.resource[RES_BIOMASS]  = (float)Clamp01(b.lifeLevel / 3.0 * (liquid ? 0.6 + 0.4 * rD : 0.25));
    }
    double ore = fmax(b.resource[RES_METALS], fmax(b.resource[RES_PRECIOUS], b.resource[RES_MINERALS]));

    // Settlements
    double dev = ctx.develop, roll = uni(0, 1), size = uni(0, 1);
    double mining = 0.18 * pow(dev, 0.7) * ore * ore;             // miners go where the ore is
    if (dev <= 0.0) {
        // nobody out here
    } else if (breathable && roll < 0.45 + 0.55 * dev) {
        b.population = pow(10.0, 6.0 + 1.5 * size + 2.2 * dev);
        b.settlement = b.population >= CORE_WORLD_POPULATION ? Settlement::CoreWorld : Settlement::SettledWorld;
    } else if (liquid && roll < 0.65 * sqrt(dev)) {
        b.population = pow(10.0, 3.6 + 2.0 * size + 1.7 * dev); b.settlement = Settlement::DomedColonies;
    } else if (b.giant && m >= GIANT_MASS && roll < 0.35 * pow(dev, 0.7)) {
        b.population = pow(10.0, 2.3 + 1.7 * size + 1.7 * dev); b.settlement = Settlement::OrbitalHabitats;
    } else if (b.lifeLevel >= 1 && roll < 0.3 * sqrt(dev)) {
        b.population = pow(10.0, 1.7 + 1.3 * size); b.settlement = Settlement::ResearchStation;
    } else if (!b.giant && b.tempK < 500 && roll < mining) {
        b.population = pow(10.0, 2.2 + 1.6 * size + 1.0 * dev); b.settlement = Settlement::MiningOutpost;
    } else if (!b.giant && b.tempK < 400 && m > 0.003 && roll < mining + 0.05 * dev) {
        b.population = pow(10.0, 3.2 + 1.8 * size + 1.5 * dev); b.settlement = Settlement::UndergroundColony;
    } else if (!b.giant && b.tempK >= 400 && roll < 0.03 * dev) {
        b.population = pow(10.0, 1.5 + 1.0 * size); b.settlement = Settlement::ResearchOutpost;
    }
    b.econ  = EconFor(b.settlement);
    b.lines = col;
}

// ---------------------------------------------------------------------------
// Asteroid belts (from the system's layout) and rings (around some giants)
// ---------------------------------------------------------------------------
static void GenerateZones(const StarSystem &sys, const SystemPlan &plan, LoadedSystem &out, bool lean)
{
    Gen g(sys.seed ^ 0x5bd1e995ull);
    out.zones.clear();

    int inner = 0, innerCount = 0;
    for (const BeltPlan &bp : plan.belts) if (!bp.outer) innerCount++;
    for (const BeltPlan &bp : plan.belts) {
        Gen bg(g.bits());                                      // each belt its own draws
        double c = 0.5 * (bp.rIn + bp.rOut), w = bp.rOut - bp.rIn;
        Zone z;
        z.ring = false; z.parent = 0; z.center = {}; z.n = sys.n; z.u = sys.u; z.v = sys.v;
        z.rIn = bp.rIn; z.rOut = bp.rOut; z.halfH = w * bg.uni(0.06, 0.12);
        z.density = BELT_DENSITY; z.sizeMin = 6.0f; z.sizeMax = 100.0f;     // 30-500 m rocks
        z.seed = bg.bits();
        // Inside the frost line the rubble is rock and metal; beyond it, carbon and ice
        double u = bg.uni(0, 1);
        if (bp.ice)                  z.composition = Composition::Icy;
        else if (c < plan.frost)     z.composition = u < 0.3 ? Composition::Metallic : u < 0.75 ? Composition::Stony : Composition::Mixed;
        else                         z.composition = u < 0.6 ? Composition::Carbonaceous : u < 0.85 ? Composition::Mixed : Composition::Icy;
        static const Color beltColor[] = { { 150, 135, 120, 255 }, { 135, 115, 85, 255 }, { 95, 90, 85, 255 }, { 110, 100, 85, 255 },
                                           { 125, 150, 170, 255 } };
        z.color = beltColor[(int)z.composition];
        if (bp.outer)             z.name = sys.name + " Ice Belt";
        else if (innerCount == 1) z.name = sys.name + " Belt";
        else                      z.name = sys.name + (inner == 0 ? " Inner Belt" : " Outer Belt");
        if (!bp.outer) inner++;
        // Point cloud for seeing the belt from a distance
        for (int k = 0; k < (lean ? 0 : 2500); k++) {
            double r  = ClampD(c + bg.gauss() * w * 0.22, z.rIn, z.rOut);
            double h  = ClampD(bg.gauss() * z.halfH * 0.35, -z.halfH, z.halfH);
            double an = bg.uni(0, 2 * PI);
            z.macro.push_back(z.u * (cos(an) * r) + z.v * (sin(an) * r) + z.n * h);
        }
        out.zones.push_back(z);
    }

    // --- Rings: around some giants, tilted onto the planet's own equator ---
    for (int pi = 0; pi < (int)out.bodies.size(); pi++) {
        const Body &p = out.bodies[pi];
        if (p.kind != Kind::Planet || !p.giant || p.massEarth < GIANT_MASS) continue;
        Gen rg(p.id ^ 0x21465ull);
        if (!rg.chance(p.massEarth >= GAS_GIANT_MASS ? 0.45 : 0.3)) continue;
        Zone z;
        z.name = p.name + " Rings"; z.ring = true; z.parent = pi; z.center = p.local;
        double tiltDir = rg.uni(0, 2 * PI), tilt = rg.uni(5, 27) * DEG2RAD;
        z.n = Norm(sys.n * cos(tilt) + (sys.u * cos(tiltDir) + sys.v * sin(tiltDir)) * sin(tilt));
        z.u = Norm(Cross(z.n, sys.u));
        z.v = Cross(z.n, z.u);
        z.rIn = p.radius * rg.uni(1.2, 1.45); z.rOut = p.radius * rg.uni(2.0, 2.6); z.halfH = 300.0;   // 1.5 km thick
        z.density = RING_DENSITY; z.sizeMin = 2.0f; z.sizeMax = 25.0f;                               // 10-125 m chunks
        z.seed = rg.bits();
        z.composition = Composition::Icy;
        z.color = Whiten(p.lines);
        // Bands of varying density, usually with one empty gap
        int bands = rg.irng(5, 9);
        std::vector<double> cuts;
        for (int k = 0; k < bands - 1; k++) cuts.push_back(rg.uni(z.rIn, z.rOut));
        std::sort(cuts.begin(), cuts.end());
        z.bandEdges.push_back(z.rIn);
        for (double cut : cuts) z.bandEdges.push_back(cut);
        z.bandEdges.push_back(z.rOut);
        for (int k = 0; k < bands; k++) z.bandDensity.push_back((float)rg.uni(0.35, 1.0));
        if (rg.chance(0.7)) z.bandDensity[rg.irng(1, bands - 2)] = 0.0f;
        out.zones.push_back(z);
    }
}

// ---------------------------------------------------------------------------
// Stations: orbital ports over settled worlds, skimmers at gas giants,
// refineries in the belts, and free-floating waystations. Only in human
// space, and fewer and smaller toward the frontier.
// ---------------------------------------------------------------------------
static void GenerateStations(const StarSystem &sys, LoadedSystem &out)
{
    const double dev = sys.develop;
    if (dev <= 0.0) return;
    Gen g(sys.seed ^ 0x7F4A7C15ull);
    auto uni    = [&](double a, double b) { return g.uni(a, b); };
    const int MAX_STATIONS = 8;
    const double reach = sqrt(dev);                                  // how likely the optional ones are out here
    int count = 0;

    auto add = [&](const std::string &name, DVec3 pos, int parent, uint64_t id, Settlement kind, Econ econ,
                   double popLo, double popHi, Color col) {
        if (count >= MAX_STATIONS) return;
        Gen sg(id);                                                  // each station its own draws
        Body s;
        s.name = name; s.local = pos; s.kind = Kind::Station; s.parent = parent; s.id = id;
        // The everyday wheel is 0.5 - 1.3 km across its radius; the special kinds are bigger, and
        // only the ones people live in the rim of need to spin fast
        double size = kind == Settlement::Starbase ? 4.0 : kind == Settlement::SolarCollector ? 5.0
                    : kind == Settlement::Greenhouse ? 2.5 : kind == Settlement::Shipyard ? 2.0
                    : kind == Settlement::SkimmerStation ? 1.5 : 1.0;
        double turn = kind == Settlement::Shipyard || kind == Settlement::SolarCollector || kind == Settlement::SkimmerStation ? 0.0
                    : kind == Settlement::Starbase ? 0.15 : kind == Settlement::Greenhouse ? 0.4 : 1.0;
        s.radius = sg.uni(100.0, 260.0) * size;
        s.orbit = Len(pos - out.bodies[parent].local);
        s.lines = col; s.fill = BLACK;
        s.type = SettlementName(kind); s.settlement = kind; s.econ = econ;
        s.population = pow(10.0, sg.uni(popLo, popHi) - (1.0 - dev));
        s.atmosphere = "Pressurized"; s.water = "Recycled"; s.life = "None";
        s.gravity = sg.uni(0.3, 1.0);                                // from its spin
        s.tempK = 293.0;
        s.spin = (float)(sg.uni(2.0, 7.0) * (sg.chance(0.5) ? 1 : -1) * turn);
        s.flux = out.bodies[parent].flux;
        out.bodies.push_back(s);
        count++;
    };
    // A point circling body 'bi' at 'rMin'..'rMax' of its radii, near the system plane
    auto around = [&](int bi, double rMin, double rMax, uint64_t id) {
        Gen pg(id ^ 0xA20ull);
        const Body &b = out.bodies[bi];
        double ang = pg.uni(0, 2 * PI), r = b.radius * pg.uni(rMin, rMax), tilt = pg.uni(-0.2, 0.2);
        return b.local + Norm(sys.u * cos(ang) + sys.v * sin(ang) + sys.n * tilt) * r;
    };
    auto orbiting = [&](int bi, const char *suffix, double rMin, double rMax, Settlement kind, Econ econ,
                        double popLo, double popHi, Color col) {
        uint64_t id = Mix64(out.bodies[bi].id ^ ((uint64_t)kind << 8) ^ 0x57A7ull);
        add(out.bodies[bi].name + suffix, around(bi, rMin, rMax, id), bi, id, kind, econ, popLo, popHi, col);
    };
    const Color PORT = { 200, 240, 255, 255 }, YARD = { 255, 200, 120, 255 }, REFINERY = { 255, 170, 90, 255 },
                SKIM = { 255, 230, 140, 255 }, LAB = { 140, 255, 200, 255 }, FREE = { 230, 200, 255, 255 },
                ORE = { 210, 180, 150, 255 }, SOLAR = { 120, 170, 255, 255 }, GARDEN = { 150, 240, 130, 255 };

    int worlds = (int)out.bodies.size();
    for (int bi = 0; bi < worlds; bi++) {
        const Body b = out.bodies[bi];                               // (a copy: adding stations moves the list)
        if (b.kind == Kind::Star) continue;
        bool giant = b.giant && b.massEarth >= GIANT_MASS;
        double roll = uni(0, 1), roll2 = uni(0, 1);                  // always two draws per world
        switch (b.econ) {
        case Econ::HighTech:
            if (roll < 0.95) orbiting(bi, " Starbase", 3.0, 6.0, Settlement::Starbase, b.econ, 4.5, 5.8, PORT);
            if (roll2 < 0.5) orbiting(bi, " Yards", 3.0, 6.0, Settlement::Shipyard, Econ::Shipyard, 3.7, 4.7, YARD);
            break;
        case Econ::Agricultural:
            if (roll < 0.8) orbiting(bi, " Highport", 3.0, 6.0, Settlement::OrbitalPort, b.econ, 4.0, 5.3, PORT);
            break;
        case Econ::Colony:
            if (roll < 0.45) orbiting(bi, " Orbital", 3.0, 6.0, Settlement::OrbitalPort, b.econ, 3.0, 4.0, PORT);
            if (roll2 < 0.4) orbiting(bi, " Gardens", 3.0, 6.0, Settlement::Greenhouse, Econ::Agricultural, 2.6, 3.6, GARDEN);
            break;
        case Econ::GasMining:
            if (roll < 0.8) orbiting(bi, " Skimmer Station", 1.6, 2.5, Settlement::SkimmerStation, Econ::Skimmer, 2.5, 3.5, SKIM);
            break;
        case Econ::Mining:
            if (roll < 0.4) orbiting(bi, " Ore Terminal", 3.0, 6.0, Settlement::OreTerminal, Econ::Mining, 2.5, 3.5, ORE);
            break;
        case Econ::Industrial:
            if (roll < 0.5) orbiting(bi, " Yards", 3.0, 6.0, Settlement::Shipyard, Econ::Shipyard, 3.5, 4.5, YARD);
            if (roll2 < 0.3) orbiting(bi, " Gardens", 3.0, 6.0, Settlement::Greenhouse, Econ::Agricultural, 2.6, 3.6, GARDEN);
            break;
        case Econ::Research:
            break;
        default:                                                     // uninhabited
            if (giant && roll < 0.2 * dev)
                orbiting(bi, " Skimmer Station", 1.6, 2.5, Settlement::SkimmerStation, Econ::Skimmer, 2.3, 3.3, SKIM);
            else if (!giant && roll < 0.03 * dev)
                orbiting(bi, " Research Station", 2.5, 5.0, Settlement::ResearchStation, Econ::Research, 1.6, 2.6, LAB);
            break;
        }
    }

    // Refineries in the belts (where the miners are)
    for (const Zone &z : out.zones) {
        if (z.ring) continue;
        double ang = uni(0, 2 * PI), roll = uni(0, 1);
        DVec3 pos = z.center + (z.u * cos(ang) + z.v * sin(ang)) * (0.5 * (z.rIn + z.rOut));
        if (roll < (z.composition == Composition::Icy ? 0.15 : 0.6) * dev)
            add(z.name + " Refinery", pos, 0, Mix64(z.seed ^ 0x4EF1ull), Settlement::OreRefinery, Econ::Refinery, 2.8, 3.9, REFINERY);
    }

    // Free-floating: a waystation between two planets, sometimes a freeport out at the edge
    std::vector<int> planets;
    for (int i = 0; i < worlds; i++) if (out.bodies[i].kind == Kind::Planet && !out.bodies[i].dwarf) planets.push_back(i);
    double roll = uni(0, 1), roll2 = uni(0, 1), ang = uni(0, 2 * PI), ang2 = uni(0, 2 * PI), pick = uni(0, 1);
    if (planets.size() >= 2 && roll < 0.3 * dev) {
        int i = (int)(pick * (planets.size() - 1));
        double r = 0.5 * (out.bodies[planets[i]].orbit + out.bodies[planets[i + 1]].orbit);
        add(sys.name + " Waystation", (sys.u * cos(ang) + sys.v * sin(ang)) * r, 0, Mix64(sys.seed ^ 0x3A57ull),
            Settlement::Waystation, Econ::Freeport, 3.0, 4.2, FREE);
    }
    if (!planets.empty() && roll2 < 0.12 * reach) {
        double r = out.bodies[planets.back()].orbit * 1.15;
        add(sys.name + " Freeport", (sys.u * cos(ang2) + sys.v * sin(ang2)) * r, 0, Mix64(sys.seed ^ 0xF4EEull),
            Settlement::Freeport, Econ::Freeport, 3.3, 4.5, FREE);
    }

    // A solar collector close in, where the light is strongest (around stars bright enough to bother with)
    double roll3 = uni(0, 1), ang3 = uni(0, 2 * PI), in = uni(0.15, 0.35);
    if (sys.lumInner > 0.02 && roll3 < 0.35 * dev) {
        double r = fmax(sys.hzIn * in, 4.0 * out.bodies[0].radius);
        add(sys.name + " Solar Array", (sys.u * cos(ang3) + sys.v * sin(ang3)) * r, 0, Mix64(sys.seed ^ 0x501A2ull),
            Settlement::SolarCollector, Econ::Power, 2.2, 3.2, SOLAR);
    }
}

// Everything in one system. Deterministic: the same system always comes out
// the same, so it can be thrown away and rebuilt. Every body draws from its
// own stream (seeded by its id), so tuning one world never changes another.
void GenerateSystemContents(const StarSystem &sys, LoadedSystem &out, bool lean)
{
    static const char *roman[] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII" };
    const SystemPlan plan = PlanSystem(sys);

    out.bodies.clear();
    out.orbits.clear();

    // --- Stars: the primary at the center, close companions beside it, wide ones far outside ---
    Gen sg(sys.seed ^ 0x6C8E9CF5ull);
    const bool multiple = sys.stars.size() > 1;
    for (size_t i = 0; i < sys.stars.size(); i++) {
        const StarInfo &st = sys.stars[i];
        double ang = sg.uni(0, 2 * PI), tilt = sg.uni(-0.15, 0.15);
        DVec3 pos = {};
        if (i > 0 && st.close) pos = (sys.u * cos(ang) + sys.v * sin(ang)) * (st.sep * AU_U);
        else if (i > 0)        pos = Norm(sys.u * cos(ang) + sys.v * sin(ang) + sys.n * tilt) * WideDistance(st, plan);
        Body b;
        b.name = multiple ? sys.name + " " + (char)('A' + i) : sys.name;
        b.local = pos; b.radius = st.radius;
        b.lines = Whiten(st.color); b.fill = st.color;
        b.kind = Kind::Star; b.parent = i == 0 ? -1 : 0; b.orbit = Len(pos);
        b.id = Mix64(sys.seed ^ (0x57A2ull + i));
        b.massSolar = st.mass;
        b.type  = StarDescription(st);
        b.tempK = st.temp;
        out.bodies.push_back(b);
    }
    const int starCount = (int)out.bodies.size();

    // --- Planets, dwarf planets and their moons ---
    std::vector<bool> icy(starCount, false);                    // per body: formed beyond the frost line
    int letter = 0, minor = 0;
    for (size_t p = 0; p < plan.planets.size(); p++) {
        const PlanetPlan &pp = plan.planets[p];
        uint64_t pid = Mix64(sys.seed ^ (0xB0D1E5ull + 0x10001ull * p));
        Gen pg(pid ^ 0x0B17ull);
        bool   envelope = pp.cls == PCls::MiniNeptune || pp.cls == PCls::IceGiant || pp.cls == PCls::GasGiant;
        double R   = WorldRadiusKm(pp.mass, envelope, pp.icy) * KM_U;
        double ang = pg.uni(0, 2 * PI);
        DVec3  pos = sys.u * (cos(ang) * pp.a) + sys.v * (sin(ang) * pp.a);

        Body b;
        b.name = pp.cls == PCls::Dwarf ? sys.name + TextFormat(" DP-%d", ++minor) : sys.name + " " + (char)('b' + letter++);
        b.local = pos; b.radius = R; b.kind = Kind::Planet; b.parent = 0; b.orbit = pp.a; b.id = pid;
        b.massEarth = pp.mass; b.giant = envelope; b.dwarf = pp.cls == PCls::Dwarf;
        int planetIndex = (int)out.bodies.size();
        out.bodies.push_back(b);
        icy.push_back(pp.icy);

        // Moons: big planets have families of them; they need room inside the planet's own
        // pull (its Hill sphere), so worlds huddled close to their star have none
        bool   big = pp.cls == PCls::IceGiant || pp.cls == PCls::GasGiant;
        double countU = pg.uni(0, 1);
        int moonCount = pp.cls == PCls::GasGiant ? pg.irng(2, 8) : pp.cls == PCls::IceGiant ? pg.irng(1, 5)
                      : pp.cls == PCls::MiniNeptune ? pg.irng(0, 2) : pp.cls == PCls::Dwarf ? (countU < 0.25 ? 1 : 0)
                      : pp.mass >= 0.3 ? (countU < 0.35 ? 1 : countU < 0.4 ? 2 : 0) : (countU < 0.1 ? 1 : 0);
        double hill = pp.a * cbrt(pp.mass * 3.003e-6 / (3.0 * plan.innerMass));
        double mo   = R * (envelope ? pg.uni(4, 9) : pg.uni(15, 50));   // Earth's Moon sits at ~60 Earth radii
        for (int m = 0; m < moonCount; m++) {
            double spacing = pg.uni(1.4, 2.0), massU = pg.uni(0, 1), ma = pg.uni(0, 2 * PI);
            if (m > 0) mo *= spacing;
            if (mo > 0.4 * hill) break;
            double mm = big ? 1e-4 * pow(300.0, massU) * (pp.cls == PCls::GasGiant ? 1.0 : 0.4)    // Ganymede is 0.025 Earths
                      : pp.cls == PCls::Dwarf ? pp.mass * (0.01 + 0.11 * massU)
                      : pp.mass * 2e-4 * pow(75.0, massU);                                         // our Moon is 0.012 of the Earth
            mm = fmax(mm, 2e-5);
            Body moon;
            moon.name = out.bodies[planetIndex].name + " " + roman[m];
            moon.local = pos + sys.u * (cos(ma) * mo) + sys.v * (sin(ma) * mo);
            moon.radius = WorldRadiusKm(mm, false, pp.icy) * KM_U;
            moon.lines = MOON_COLOR; moon.kind = Kind::Moon; moon.parent = planetIndex; moon.orbit = mo;
            moon.id = Mix64(pid + 1 + m);
            moon.massEarth = mm;
            out.bodies.push_back(moon);
            icy.push_back(pp.icy);
        }
    }

    // --- Climate, life, resources and settlements ---
    // Life needs time: nothing in a young system, and the dead and dying stars have burned
    // or frozen whatever they had. Small red dwarfs flare too hard for anything complex.
    const StarInfo &st = sys.stars[0];
    int lifeCap = sys.age < 0.6 ? 0 : sys.age < 2.0 ? 1 : sys.age < 3.5 ? 2 : 3;
    if (st.kind != StarKind::MainSequence)                   lifeCap = std::min(lifeCap, 1);
    else if (st.mass < 0.35)                                 lifeCap = std::min(lifeCap, 2);
    for (int bi = starCount; bi < (int)out.bodies.size(); bi++) {
        Body &b = out.bodies[bi];
        double flux = 0.0;                                       // light received, in Earths (1 = Earth's)
        for (int si = 0; si < starCount; si++) {
            double d = Len(b.local - out.bodies[si].local) / AU_U;
            flux += sys.stars[si].lum / (d * d);
        }
        Gen wg(b.id ^ 0x1B873593ull);
        DescribeWorld(b, b.kind == Kind::Moon ? &out.bodies[b.parent] : nullptr,
                      { flux, (bool)icy[bi], sys.develop, lifeCap, sys.metal }, wg);
    }
    // Orbit lines in each planet's color (dwarf planets fainter)
    for (const Body &b : out.bodies)
        if (b.kind == Kind::Planet) out.orbits.push_back({ sys.u, sys.v, b.orbit, Dim(b.lines, b.dwarf ? 0.12f : 0.25f) });
    // The habitable zone, as two faint green rings
    out.orbits.push_back({ sys.u, sys.v, sys.hzIn,  { 30, 110, 55, 255 } });
    out.orbits.push_back({ sys.u, sys.v, sys.hzOut, { 30, 110, 55, 255 } });

    GenerateZones(sys, plan, out, lean);
    GenerateStations(sys, out);
}

// ---------------------------------------------------------------------------
// The shape of the galaxy
// ---------------------------------------------------------------------------
// A disk thinning outward and upward, wound into spiral arms, with a dense bulge
// in the middle. Normalized so home's neighbourhood has LOCAL_DENSITY.
double GalaxyDensity(DVec3 p)
{
    const double ARM_WIND = 4.51, ARM_PHASE = 0.5;           // arms pitched 12.5 degrees; home on the inner edge of one
    double R = sqrt(p.x * p.x + p.z * p.z), h = fabs(p.y);
    if (R >= GALAXY_RADIUS_LY || h >= GALAXY_HALF_HEIGHT_LY) return 0.0;

    double vert = 0.9 * exp(-h / THIN_DISK_LY) + 0.1 * exp(-h / THICK_DISK_LY);
    double disk = exp(-(R - HOME_RADIUS_LY) / DISK_SCALE_LY) * vert;
    if (R > 1.0) {
        double phi = atan2(p.z, p.x) - log(R / HOME_RADIUS_LY) * ARM_WIND + ARM_PHASE;
        double s2 = 0.5 + 0.5 * cos(2.0 * phi), s4 = 0.5 + 0.5 * cos(4.0 * phi);      // two strong arms, two faint ones between
        double arm = 0.5 + 0.8 * s2 * s2 + 0.5 * s4 * s4;
        disk *= 1.0 + (arm - 1.0) * fmin(1.0, R / 8000.0);                             // the arms fade into the bulge
    }
    disk *= Clamp01((GALAXY_RADIUS_LY - R) / 10000.0) * Clamp01((GALAXY_HALF_HEIGHT_LY - h) / 1000.0);   // soft outer edges

    double rb = sqrt(R * R + (p.y / 0.6) * (p.y / 0.6));
    double bulge = 70.0 * exp(-rb / 1800.0);
    return fmin(MAX_DENSITY, LOCAL_DENSITY * (disk + bulge) / 0.957);
}

// ---------------------------------------------------------------------------
// Cells and systems
// ---------------------------------------------------------------------------
SysId CellId(int cx, int cy, int cz)
{
    if (cx < -32768 || cx > 32767 || cz < -32768 || cz > 32767 || cy < -2048 || cy > 2047) return NO_SYSTEM;
    return ((int64_t)(cx + 32768) << 28) | ((int64_t)(cz + 32768) << 12) | (int64_t)(cy + 2048);
}

void IdCell(SysId id, int &cx, int &cy, int &cz)
{
    cx = (int)((id >> 28) & 0xFFFF) - 32768;
    cz = (int)((id >> 12) & 0xFFFF) - 32768;
    cy = (int)(id & 0xFFF) - 2048;
}

static uint64_t CellHash(unsigned seed, int cx, int cy, int cz)
{
    return Mix64(Mix64(Mix64(Mix64(0x9A1A5EEDull ^ seed) ^ (uint64_t)(int64_t)cx) ^ (uint64_t)(int64_t)cy) ^ (uint64_t)(int64_t)cz);
}

uint64_t CellHashOf(unsigned seed, const StarSystem &s) { return CellHash(seed, s.cx, s.cy, s.cz); }

// Does this cell hold a system? (More often where the galaxy is denser.)
static bool CellHasSystem(unsigned seed, int cx, int cy, int cz, uint64_t &hash)
{
    if (CellId(cx, cy, cz) < 0) return false;
    double p = GalaxyDensity({ (cx + 0.5) * CELL_LY, (cy + 0.5) * CELL_LY, (cz + 0.5) * CELL_LY }) * CELL_LY * CELL_LY * CELL_LY;
    if (p <= 0.0) return false;
    hash = CellHash(seed, cx, cy, cz);
    return (double)(hash >> 11) * (1.0 / 9007199254740992.0) < p;
}

// Names. Inside human space stars have proper names, built from three parts so
// that no two systems there can share one: the cell's place relative to home is
// scrambled (reversibly) into one choice from each list, and the lists are shaped
// (one vowel group apiece; consonant end, vowel start) so a name splits only one way.
static const char *NAME_HEAD[64] = {
    "Kal", "Vor", "Tes", "Brin", "Ar", "El", "Mer", "Dan", "Sol", "Tar", "Ven", "Cor", "Lan", "Mar", "Nor", "Per",
    "Ras", "Sil", "Tor", "Val", "Zan", "Bel", "Cas", "Dor", "Fen", "Gal", "Hal", "Jor", "Kes", "Lor", "Mil", "Nar",
    "Or", "Pol", "Quin", "Rin", "Sar", "Tal", "Ul", "Vel", "Wen", "Xan", "Zor", "Ath", "Bar", "Cal", "Del", "Er",
    "Fal", "Gor", "Hel", "Is", "Jan", "Kor", "Lin", "Mor", "Nel", "Os", "Pal", "Ren", "Sen", "Thal", "Um", "Vin" };
static const char *NAME_MID[64] = {
    "ar", "en", "ol", "is", "um", "ad", "er", "an", "on", "il", "as", "or", "et", "ath", "end", "orn",
    "ist", "ald", "eth", "ir", "us", "am", "el", "ot", "ind", "ark", "ess", "orr", "ull", "ant", "ern", "ost",
    "ix", "ax", "ex", "ox", "ab", "eb", "ib", "ob", "ac", "ec", "ic", "oc", "ag", "eg", "ig", "og",
    "ap", "ep", "ip", "op", "av", "ev", "iv", "ov", "az", "ez", "iz", "oz", "aud", "ein", "ior", "oun" };
static const char *NAME_TAIL[64] = {
    "a", "e", "i", "o", "u", "is", "os", "us", "as", "es", "ia", "io", "ea", "eo", "ae", "ai",
    "ion", "ium", "eon", "ius", "ior", "an", "en", "in", "on", "un", "ar", "er", "ir", "or", "ur", "ax",
    "ex", "ix", "ox", "ux", "ath", "eth", "ith", "oth", "ael", "iel", "aen", "ien", "ann", "enn", "ell", "ill",
    "oll", "ard", "ond", "und", "ant", "ent", "ast", "est", "ost", "arn", "orn", "ern", "aud", "eis", "oum", "ous" };

static std::string ProperName(unsigned seed, int dx, int dy, int dz)
{
    const uint32_t MASK = 0x3FFFF;                                  // 18 bits: 64 * 64 * 64 names
    uint32_t v = (uint32_t)(dx + NAMED_CELLS) | (uint32_t)(dy + NAMED_CELLS) << 6 | (uint32_t)(dz + NAMED_CELLS) << 12;
    v = (v * 40503u + (uint32_t)Mix64(seed ^ 0x4E414D45ull)) & MASK;   // every step is reversible, so distinct cells stay distinct
    v ^= v >> 9;
    v = (v * 12829u) & MASK;
    v ^= v >> 11;
    v = (v * 51437u) & MASK;
    v ^= v >> 7;
    return std::string(NAME_HEAD[v & 63]) + NAME_MID[(v >> 6) & 63] + NAME_TAIL[(v >> 12) & 63];
}

// Everywhere else, a catalogue number: the surveyed region (40 ly across) and the star's
// number in it. Region names repeat only every sixteen regions (640 ly) in each direction,
// so no two stars anywhere near each other share one.
static std::string CatalogueName(unsigned seed, int cx, int cy, int cz)
{
    auto scramble = [](uint32_t v, uint32_t key) {              // reversible, on 12 bits
        v = (v * 2731u + key) & 0xFFF;
        v ^= v >> 6;
        v = (v * 1389u) & 0xFFF;
        return v ^ (v >> 5);
    };
    uint32_t key = (uint32_t)Mix64(seed ^ 0x5EC7ull);
    uint32_t rx = (uint32_t)(cx + 32768) >> 4, ry = (uint32_t)(cy + 2048) >> 4, rz = (uint32_t)(cz + 32768) >> 4;
    uint32_t region = scramble((rx & 15) | (ry & 15) << 4 | (rz & 15) << 8, key);
    uint32_t n = scramble((uint32_t)(cx & 15) | (uint32_t)(cy & 15) << 4 | (uint32_t)(cz & 15) << 8, key >> 12);
    return std::string(NAME_HEAD[region & 63]) + NAME_MID[region >> 6] + TextFormat(" %d", (int)n + 1);
}

// Size of the gravity field: around everything that orbits the primary
static void SizeField(StarSystem &s)
{
    SystemPlan plan = PlanSystem(s);
    double outer = plan.edge;
    for (size_t i = 1; i < s.stars.size(); i++)
        outer = fmax(outer, (s.stars[i].close ? s.stars[i].sep * AU_U : WideDistance(s.stars[i], plan)) + s.stars[i].radius);
    s.fieldR = fmax(outer * FIELD_EDGE_MARGIN, 10.0 * AU_U);
    s.fieldH = s.fieldR * FIELD_HEIGHT_RATIO;
}

// The system in a cell (which must hold one; 'hash' from CellHasSystem). 'home' is the
// cell human space is centered on, or null before a home has been chosen.
static StarSystem MakeSystem(unsigned seed, int cx, int cy, int cz, uint64_t hash, const int *home, const FactionSet *factions = nullptr)
{
    StarSystem s;
    Gen g(hash ^ 0x5157ull);
    s.id = CellId(cx, cy, cz);
    s.cx = cx; s.cy = cy; s.cz = cz;
    s.seed = Mix64(hash ^ 0x5EEDull);
    auto place = [&]() { return std::round(CELL_U * (CELL_MARGIN + g.uni(0, 1) * (1.0 - 2.0 * CELL_MARGIN)) / POS_GRID) * POS_GRID; };
    s.off.x = place(); s.off.y = place(); s.off.z = place();

    // Heavy elements: fewer far from the center, and in the old stars high above the disk
    DVec3  ly = { (cx + 0.5) * CELL_LY, (cy + 0.5) * CELL_LY, (cz + 0.5) * CELL_LY };
    double R = sqrt(ly.x * ly.x + ly.z * ly.z);
    s.metal = ClampD(-0.05 + 0.2 * g.gauss() - 0.018 * (R - HOME_RADIUS_LY) / 1000.0 - (fabs(ly.y) > 1500.0 ? 0.5 : 0.0), -2.0, 0.5);

    // The plane its planets orbit in: tipped a little from the galaxy's
    s.n = Norm({ g.uni(-0.45, 0.45), 1.0, g.uni(-0.45, 0.45) });
    s.u = Norm(Cross(s.n, { 0, 0, 1 }));
    s.v = Cross(s.n, s.u);

    // Human space
    double noise = g.uni(0.85, 1.15);
    if (home) {
        int dx = cx - home[0], dy = cy - home[1], dz = cz - home[2];
        double d = CELL_LY * sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz) * noise;
        s.develop = (float)Clamp01((FRONTIER_RADIUS_LY - d) / (FRONTIER_RADIUS_LY - CORE_RADIUS_LY));
        s.named = s.develop > 0.0f && dx >= -NAMED_CELLS && dx < NAMED_CELLS && dy >= -NAMED_CELLS && dy < NAMED_CELLS &&
                  dz >= -NAMED_CELLS && dz < NAMED_CELLS;
        if (s.named) s.name = ProperName(seed, dx, dy, dz);
        if (factions) s.ruler = TerritoryRuler(*factions, { dx * CELL_LY, dy * CELL_LY, dz * CELL_LY }, s.develop, hash);
    }
    if (!s.named) s.name = CatalogueName(seed, cx, cy, cz);

    GenerateStars(s);
    SizeField(s);
    return s;
}

// ---------------------------------------------------------------------------
// The galaxy: sectors made on demand
// ---------------------------------------------------------------------------
const Galaxy::Sector &Galaxy::SectorOf(int cx, int cy, int cz) const
{
    int sx = (cx + 32768) / SECTOR_CELLS, sy = (cy + 2048) / SECTOR_CELLS, sz = (cz + 32768) / SECTOR_CELLS;
    uint64_t key = ((uint64_t)sx << 40) | ((uint64_t)sz << 20) | (uint64_t)sy;
    auto it = sectors.find(key);
    if (it != sectors.end()) { it->second.used = tick; return it->second; }

    Sector &sec = sectors[key];
    sec.used = tick;
    for (int lz = 0; lz < SECTOR_CELLS; lz++)
        for (int ly = 0; ly < SECTOR_CELLS; ly++)
            for (int lx = 0; lx < SECTOR_CELLS; lx++) {
                int x = sx * SECTOR_CELLS - 32768 + lx, y = sy * SECTOR_CELLS - 2048 + ly, z = sz * SECTOR_CELLS - 32768 + lz;
                int8_t &slot = sec.slot[lx + SECTOR_CELLS * (ly + SECTOR_CELLS * lz)];
                uint64_t hash;
                slot = -1;
                if (!CellHasSystem(seed, x, y, z, hash)) continue;
                slot = (int8_t)sec.systems.size();
                sec.systems.push_back(MakeSystem(seed, x, y, z, hash, home >= 0 ? homeCell : nullptr, &factions));
            }
    return sec;
}

const StarSystem *Galaxy::AtCell(int cx, int cy, int cz) const
{
    if (CellId(cx, cy, cz) < 0) return nullptr;
    const Sector &sec = SectorOf(cx, cy, cz);
    int lx = (cx + 32768) % SECTOR_CELLS, ly = (cy + 2048) % SECTOR_CELLS, lz = (cz + 32768) % SECTOR_CELLS;
    int slot = sec.slot[lx + SECTOR_CELLS * (ly + SECTOR_CELLS * lz)];
    return slot < 0 ? nullptr : &sec.systems[slot];
}

const StarSystem *Galaxy::Find(SysId id) const
{
    if (id < 0 || id >= ((int64_t)1 << 44)) return nullptr;
    int cx, cy, cz;
    IdCell(id, cx, cy, cz);
    return AtCell(cx, cy, cz);
}

const StarSystem &Galaxy::operator[](SysId id) const
{
    static const StarSystem none = [] { StarSystem s; s.name = "Unknown"; s.stars.push_back(StarInfo()); s.n = { 0, 1, 0 };
                                        s.u = { 1, 0, 0 }; s.v = { 0, 0, 1 }; return s; }();
    const StarSystem *s = Find(id);
    return s ? *s : none;
}

DVec3 Galaxy::Between(SysId from, SysId to) const
{
    if (from == to) return {};
    const StarSystem *a = Find(from), *b = Find(to);
    const DVec3 mid = { 0.5 * CELL_U, 0.5 * CELL_U, 0.5 * CELL_U };
    int ax, ay, az, bx, by, bz;
    IdCell(from, ax, ay, az);
    IdCell(to, bx, by, bz);
    DVec3 oa = a ? a->off : mid, ob = b ? b->off : mid;
    return { (double)(bx - ax) * CELL_U + (ob.x - oa.x), (double)(by - ay) * CELL_U + (ob.y - oa.y),
             (double)(bz - az) * CELL_U + (ob.z - oa.z) };
}

DVec3 Galaxy::PositionLy(SysId anchor, DVec3 pos) const
{
    const StarSystem *a = Find(anchor);
    int cx, cy, cz;
    IdCell(anchor, cx, cy, cz);
    DVec3 p = pos + (a ? a->off : DVec3{});
    return { (cx + p.x / CELL_U) * CELL_LY, (cy + p.y / CELL_U) * CELL_LY, (cz + p.z / CELL_U) * CELL_LY };
}

void Galaxy::Nearby(SysId anchor, DVec3 pos, double radius, std::vector<const StarSystem *> &out) const
{
    const StarSystem *a = Find(anchor);
    if (!a) return;
    DVec3 p = a->off + pos;                                      // from the corner of the anchor's cell
    auto lo = [&](double v) { return (int)floor((v - radius) / CELL_U); };
    auto hi = [&](double v) { return (int)floor((v + radius) / CELL_U); };
    auto sector = [](int c, int bias) { return (c + bias) / SECTOR_CELLS; };
    int x0 = std::max(a->cx + lo(p.x), -32768), x1 = std::min(a->cx + hi(p.x), 32767);
    int y0 = std::max(a->cy + lo(p.y), -2048),  y1 = std::min(a->cy + hi(p.y), 2047);
    int z0 = std::max(a->cz + lo(p.z), -32768), z1 = std::min(a->cz + hi(p.z), 32767);
    if (x0 > x1 || y0 > y1 || z0 > z1) return;
    for (int sz = sector(z0, 32768); sz <= sector(z1, 32768); sz++)
        for (int sy = sector(y0, 2048); sy <= sector(y1, 2048); sy++)
            for (int sx = sector(x0, 32768); sx <= sector(x1, 32768); sx++) {
                const Sector &sec = SectorOf(sx * SECTOR_CELLS - 32768, sy * SECTOR_CELLS - 2048, sz * SECTOR_CELLS - 32768);
                for (const StarSystem &s : sec.systems) {
                    DVec3 rel = { (double)(s.cx - a->cx) * CELL_U + s.off.x - p.x, (double)(s.cy - a->cy) * CELL_U + s.off.y - p.y,
                                  (double)(s.cz - a->cz) * CELL_U + s.off.z - p.z };
                    if (Dot(rel, rel) <= radius * radius) out.push_back(&s);
                }
            }
}

const StarSystem *Galaxy::Nearest(SysId anchor, DVec3 pos, int cells) const
{
    const StarSystem *a = Find(anchor);
    if (!a) return nullptr;
    DVec3 p = a->off + pos;
    std::vector<const StarSystem *> found;
    Nearby(anchor, pos, cells * CELL_U, found);
    const StarSystem *best = nullptr;
    double bestD = 0.0;
    for (const StarSystem *s : found) {
        DVec3 rel = { (double)(s->cx - a->cx) * CELL_U + s->off.x - p.x, (double)(s->cy - a->cy) * CELL_U + s->off.y - p.y,
                      (double)(s->cz - a->cz) * CELL_U + s->off.z - p.z };
        double d = Dot(rel, rel);
        if (!best || d < bestD || (d == bestD && s->id < best->id)) { best = s; bestD = d; }
    }
    return best;
}

void Galaxy::Trim()
{
    const size_t   KEEP = 4096;          // sectors (a few thousand systems): plenty around every captain
    const uint32_t IDLE = 3000;          // calls since anyone looked
    tick++;
    if ((tick & 255) || sectors.size() <= KEEP) return;
    for (auto it = sectors.begin(); it != sectors.end();)
        it = tick - it->second.used > IDLE ? sectors.erase(it) : std::next(it);
}

// Is this a good place to start? 2: a long-lived Sun-like star with a breathable, crowded
// world, a rock belt with a refinery to start beside, and a giant to skim. 1: somewhere
// with people and a station, at least. 0: no.
static bool SunLike(const StarSystem &s)
{
    const StarInfo &st = s.stars[0];
    return s.stars.size() == 1 && st.kind == StarKind::MainSequence && st.mass > 0.7 && st.mass < 1.2 && s.age >= 3.5;
}

static int HomeScore(const StarSystem &s)
{
    LoadedSystem L;
    GenerateSystemContents(s, L);
    int planets = 0, inhabited = 0, stations = 0;
    bool core = false, refinery = false, giant = false;
    for (const Body &b : L.bodies) {
        if (b.kind == Kind::Planet && !b.dwarf) planets++;
        if (b.kind == Kind::Planet && b.population > 0) inhabited++;
        if (b.kind == Kind::Station) stations++;
        if (b.settlement == Settlement::CoreWorld && b.massEarth > 0.4 && b.massEarth < 3.0) core = true;
        if (b.settlement == Settlement::OreRefinery) refinery = true;
        if (b.giant && b.massEarth >= GIANT_MASS) giant = true;
    }
    bool rockBelt = !L.zones.empty() && !L.zones[0].ring && L.zones[0].composition != Composition::Icy;
    if (inhabited == 0 || stations == 0 || planets < 2) return 0;          // unplayable
    int score = 1;
    if (core && refinery && rockBelt && giant && planets >= 5 && SunLike(s)) score = 2;
    return score;
}

void Galaxy::Init(unsigned newSeed)
{
    seed = newSeed;
    home = NO_SYSTEM;
    sectors.clear();
    tick = 0;

    factions = FactionSet();

    // (The search builds a few dozen systems in full, and the factions look over the whole
    // of human space, so the answers are remembered)
    static std::map<unsigned, std::array<int, 3>> known;
    static std::map<unsigned, FactionSet> knownFactions;
    auto it = known.find(seed);
    if (it != known.end()) {
        for (int i = 0; i < 3; i++) homeCell[i] = it->second[i];
        home = CellId(homeCell[0], homeCell[1], homeCell[2]);
        factions = knownFactions[seed];
        return;
    }

    // Start looking where the Sun would be, and work outward a shell of cells at a time
    Gen g(Mix64(seed ^ 0x484F4D45ull));
    int c0[3] = { (int)floor((HOME_RADIUS_LY + g.uni(-400, 400)) / CELL_LY), (int)floor((65.0 + g.uni(-40, 40)) / CELL_LY),
                  (int)floor(g.uni(-400, 400) / CELL_LY) };
    int best[3] = { c0[0], c0[1], c0[2] }, bestScore = -1;
    for (int r = 0; r <= 60 && bestScore < 2; r++)
        for (int dz = -r; dz <= r && bestScore < 2; dz++)
            for (int dy = -r; dy <= r && bestScore < 2; dy++)
                for (int dx = -r; dx <= r && bestScore < 2; dx++) {
                    if (std::max(abs(dx), std::max(abs(dy), abs(dz))) != r) continue;       // just the shell
                    int c[3] = { c0[0] + dx, c0[1] + dy, c0[2] + dz };
                    uint64_t hash;
                    if (!CellHasSystem(seed, c[0], c[1], c[2], hash)) continue;
                    StarSystem s = MakeSystem(seed, c[0], c[1], c[2], hash, c);             // as it would be if it were home
                    int score = SunLike(s) || bestScore < 1 ? HomeScore(s) : 0;             // (only look closely at real candidates)
                    if (score > bestScore) { bestScore = score; best[0] = c[0]; best[1] = c[1]; best[2] = c[2]; }
                }
    homeCell[0] = best[0]; homeCell[1] = best[1]; homeCell[2] = best[2];
    home = CellId(best[0], best[1], best[2]);
    known[seed] = { best[0], best[1], best[2] };

    // Who holds the space around it. (Systems made so far have no rulers: start the sectors afresh.)
    factions = GenerateFactions(*this);
    knownFactions[seed] = factions;
    sectors.clear();
}
