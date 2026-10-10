// galaxy.cpp - generating stars, systems, worlds, belts and rings
#include "galaxy.h"
#include <algorithm>
#include <random>

static const Color PLANET_COLORS[] = {
    { 255, 140,   0, 255 }, {  80, 180, 255, 255 }, { 120, 255, 120, 255 },
    { 255,  90, 200, 255 }, { 255, 220,  60, 255 }, { 255,  70,  70, 255 },
    { 170, 110, 255, 255 }, {   0, 230, 210, 255 },
};
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
    default:                 return "None";
    }
}

// Surface temperature of a world with oceans, from where it sits in the habitable
// zone: ~312 K at the inner edge down to ~276 K at the outer edge
static double TemperateK(double flux)
{
    double u = (log(flux) - log(HZ_FLUX_OUTER)) / (log(HZ_FLUX_INNER) - log(HZ_FLUX_OUTER));
    return 276.0 + 36.0 * Clamp((float)u, 0.0f, 1.0f);
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
// Main-sequence relations: luminosity and radius from mass, temperature from both
static StarInfo StarFromMass(double m)
{
    StarInfo s = {};
    s.mass = m;
    s.lum  = m < 0.43 ? 0.23 * pow(m, 2.3) : m < 2.0 ? pow(m, 4.0) : 1.4 * pow(m, 3.5);
    double rSun = m < 1.0 ? pow(m, 0.8) : pow(m, 0.57);
    s.radius = rSun * RSUN_U;
    s.temp   = 5772.0 * pow(s.lum / (rSun * rSun), 0.25);

    // Spectral class by temperature; subclass 0-9 runs hot to cool within the class
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
            s.sub   = (int)Clamp((float)((b.hot - s.temp) / (b.hot - b.cool) * 10.0), 0.0f, 9.0f);
            s.color = b.color;
            break;
        }
    }
    return s;
}

const char *StarDescription(const StarInfo &s)
{
    const char *kind = s.cls == 'O' ? "blue star" : s.cls == 'B' ? "blue-white star" :
                       s.cls == 'A' ? "white star" : s.cls == 'F' ? "yellow-white star" :
                       s.cls == 'G' ? "yellow dwarf" : s.cls == 'K' ? "orange dwarf" : "red dwarf";
    return TextFormat("%c%d V %s", s.cls, s.sub, kind);
}

// The stars of a system. Uses its own random generator so system names, seeds
// and planet layouts stay the same as before stars got types.
// forceClass: index into the class table below (0 = O ... 6 = M), or -1 for random.
// forceCompanions / forceWide: -1 for random.
static void GenerateStars(StarSystem &sys, int forceClass = -1, int forceCompanions = -1, int forceWide = -1)
{
    std::mt19937 rng(sys.seed ^ 0x2F6B1D3Bu);
    auto uni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto logUni = [&](double a, double b) { return exp(uni(log(a), log(b))); };

    // Primary: weighted toward the common cool stars, but with hot stars more common than in
    // reality (about 3 in 4 real stars are red dwarfs), so a small galaxy shows every color
    struct Pick { double weight, mLo, mHi; };
    static const Pick picks[] = { { 0.04, 16.0, 40.0 }, { 0.08, 2.1, 16.0 }, { 0.12, 1.4, 2.1 },
                                  { 0.15, 1.04, 1.4 }, { 0.19, 0.8, 1.04 }, { 0.20, 0.45, 0.8 }, { 0.22, 0.1, 0.45 } };
    double r = uni(0, 1), acc = 0.0;
    double mass = 1.0;
    for (const Pick &p : picks) { acc += p.weight; if (r <= acc) { mass = logUni(p.mLo, p.mHi); break; } }
    if (forceClass >= 0) mass = logUni(picks[forceClass].mLo, picks[forceClass].mHi);
    sys.stars.clear();
    sys.stars.push_back(StarFromMass(mass));

    // Multiplicity: ~60% single, ~28% binary, ~12% trinary
    double mult = uni(0, 1);
    int companions = mult < 0.60 ? 0 : mult < 0.88 ? 1 : 2;
    if (forceCompanions >= 0) companions = forceCompanions;
    for (int c = 0; c < companions; c++) {
        StarInfo s = StarFromMass(mass * uni(0.15, 0.95));
        // A binary is close or wide; a trinary has one of each
        s.close = companions == 2 ? (c == 0) : uni(0, 1) < 0.55;
        if (companions == 1 && forceWide >= 0) s.close = forceWide == 0;
        s.sep   = s.close ? uni(0.04, 0.12) : uni(1.3, 2.0);
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

// Planetary systems scale with their star's light: a red dwarf's planets huddle
// close in, a hot star's spread far out (clamped so systems stay playable).
static double OrbitScale(const StarSystem &sys) { return Clamp((float)sqrt(sys.lumInner), 0.15f, 6.0f); }

// Asteroid belts (in the widest gaps between planets) and rings (around some
// gas giants). Uses its own random generator, so adding these didn't change
// the planets of any existing system.
static void GenerateZones(const StarSystem &sys, LoadedSystem &out)
{
    std::mt19937 rng(sys.seed ^ 0x5bd1e995u);
    auto uni   = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto irng  = [&](int a, int b)       { return std::uniform_int_distribution<int>(a, b)(rng); };
    auto gauss = [&]()                   { return std::normal_distribution<double>(0.0, 1.0)(rng); };
    out.zones.clear();

    std::vector<int> planets;
    for (int i = 0; i < (int)out.bodies.size(); i++) if (out.bodies[i].kind == Kind::Planet) planets.push_back(i);

    // --- Belts: in the widest gaps between neighbouring orbits ---
    std::vector<std::pair<double, int>> gaps;   // (outer/inner orbit ratio, inner planet slot)
    for (int i = 0; i + 1 < (int)planets.size(); i++) {
        double ratio = Len(out.bodies[planets[i + 1]].local) / Len(out.bodies[planets[i]].local);
        if (ratio >= 1.75) gaps.push_back({ ratio, i });
    }
    std::sort(gaps.begin(), gaps.end(), [](auto &a, auto &b) { return a.first > b.first; });
    int beltCount = 0;
    if (!gaps.empty() && uni(0, 1) < 0.8) beltCount++;
    if (gaps.size() > 1 && uni(0, 1) < 0.3) beltCount++;
    for (int b = 0; b < beltCount; b++) {
        double a0 = Len(out.bodies[planets[gaps[b].second]].local);
        double a1 = Len(out.bodies[planets[gaps[b].second + 1]].local);
        double c  = sqrt(a0 * a1), w = (a1 - a0) * uni(0.15, 0.3);
        Zone z;
        z.ring = false; z.parent = 0; z.center = {}; z.n = sys.n; z.u = sys.u; z.v = sys.v;
        z.rIn = c - 0.5 * w; z.rOut = c + 0.5 * w; z.halfH = w * uni(0.06, 0.12);
        z.density = BELT_DENSITY; z.sizeMin = 6.0f; z.sizeMax = 100.0f;     // 30-500 m rocks
        z.seed = (unsigned)rng(); z.color = { 110, 100, 85, 255 };
        // Its makeup comes from a hash of its seed (no extra draws, so nothing else shifts)
        z.composition = (Composition)(Mix64((uint64_t)z.seed ^ 0xC0FFEEull) % 4);
        static const Color beltColor[] = { { 150, 135, 120, 255 }, { 135, 115, 85, 255 }, { 95, 90, 85, 255 }, { 110, 100, 85, 255 } };
        z.color = beltColor[(int)z.composition];
        // Point cloud for seeing the belt from a distance
        for (int k = 0; k < 2500; k++) {
            double r  = Clamp((float)(c + gauss() * w * 0.22), (float)z.rIn, (float)z.rOut);
            double h  = Clamp((float)(gauss() * z.halfH * 0.35), (float)-z.halfH, (float)z.halfH);
            double an = uni(0, 2 * PI);
            z.macro.push_back(z.u * (cos(an) * r) + z.v * (sin(an) * r) + z.n * h);
        }
        out.zones.push_back(z);
    }
    // Names: "Belt", or "Inner Belt" / "Outer Belt" when there are two
    std::sort(out.zones.begin(), out.zones.end(), [](const Zone &a, const Zone &b) { return a.rIn < b.rIn; });
    for (size_t i = 0; i < out.zones.size(); i++)
        out.zones[i].name = sys.name + (out.zones.size() == 1 ? " Belt" : (i == 0 ? " Inner Belt" : " Outer Belt"));

    // --- Rings: around some gas giants, tilted onto the planet's own equator ---
    for (int pi : planets) {
        const Body &p = out.bodies[pi];
        if (p.radius < 20000 * KM_U || uni(0, 1) > 0.45) continue;
        Zone z;
        z.name = p.name + " Rings"; z.ring = true; z.parent = pi; z.center = p.local;
        double tiltDir = uni(0, 2 * PI), tilt = uni(5, 27) * DEG2RAD;
        z.n = Norm(sys.n * cos(tilt) + (sys.u * cos(tiltDir) + sys.v * sin(tiltDir)) * sin(tilt));
        Vector3 u = Vector3Normalize(Vector3CrossProduct(ToF(z.n), ToF(sys.u)));
        z.u = ToD(u); z.v = ToD(Vector3CrossProduct(ToF(z.n), u));
        z.rIn = p.radius * uni(1.2, 1.45); z.rOut = p.radius * uni(2.0, 2.6); z.halfH = 300.0;   // 1.5 km thick
        z.density = RING_DENSITY; z.sizeMin = 2.0f; z.sizeMax = 25.0f;                          // 10-125 m chunks
        z.seed = (unsigned)rng();
        z.composition = Composition::Icy;
        z.color = { (unsigned char)((p.lines.r + 255) / 2), (unsigned char)((p.lines.g + 255) / 2),
                    (unsigned char)((p.lines.b + 255) / 2), 255 };
        // Bands of varying density, usually with one empty gap
        int bands = irng(5, 9);
        std::vector<double> cuts;
        for (int k = 0; k < bands - 1; k++) cuts.push_back(uni(z.rIn, z.rOut));
        std::sort(cuts.begin(), cuts.end());
        z.bandEdges.push_back(z.rIn);
        for (double cut : cuts) z.bandEdges.push_back(cut);
        z.bandEdges.push_back(z.rOut);
        for (int k = 0; k < bands; k++) z.bandDensity.push_back((float)uni(0.35, 1.0));
        if (uni(0, 1) < 0.7) z.bandDensity[irng(1, bands - 2)] = 0.0f;
        out.zones.push_back(z);
    }
}

// ---------------------------------------------------------------------------
// Worlds: climate, life and settlements
// ---------------------------------------------------------------------------
// 'flux' is the starlight a world receives, in Earths (1 = what Earth gets).
// Equilibrium temperature follows from flux and albedo; the atmosphere then
// adds a greenhouse effect (Earth: +33 K, Venus: +500 K). Liquid surface water
// needs an atmosphere and 273-373 K. Native life favors liquid water; people
// live wherever they can make it work, habitable zone or not.
static void DescribeWorld(Body &b, const Body *parent, double flux, const StarSystem &sys, std::mt19937 &rng)
{
    auto uni    = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto chance = [&](double p) { return uni(0.0, 1.0) < p; };
    auto teq    = [&](double albedo) { return 278.6 * pow(flux * (1.0 - albedo), 0.25); };
    auto rollLife = [&]() { double r = uni(0, 1); return r < 0.25 ? 0 : r < 0.55 ? 1 : r < 0.78 ? 2 : 3; };

    (void)sys;
    double km = b.radius / KM_U;
    b.flux = flux;
    b.inHZ = flux >= HZ_FLUX_OUTER && flux <= HZ_FLUX_INNER;      // the same limits as the HZ rings

    bool   giant = b.kind == Kind::Planet && km >= 20000;
    bool   liquid = false, breathable = false;
    double density = 3000.0;
    Color  col = MOON_COLOR;
    std::string lifeWhere;

    if (giant) {
        b.tempK = teq(0.35);                                         // cloud tops
        bool ice = km < 35000;
        density = ice ? 1600.0 : 1300.0;
        b.atmosphere = ice ? "Hydrogen, helium, methane" : "Hydrogen, helium";
        b.water      = ice ? "Deep water-ammonia mantle" : "Traces in deep clouds";
        if (ice) { b.type = b.tempK > 600 ? "Hot Neptune" : "Ice giant";
                   col  = b.tempK > 600 ? Color{ 130, 150, 255, 255 } : Color{ 100, 200, 240, 255 }; }
        else     { b.type = b.tempK > 900 ? "Hot Jupiter" : b.tempK > 350 ? "Warm gas giant" : "Gas giant";
                   col  = b.tempK > 900 ? Color{ 255, 110, 60, 255 } : b.tempK > 350 ? Color{ 240, 165, 90, 255 }
                                        : Color{ 225, 190, 120, 255 }; }
        if (b.tempK > 230 && b.tempK < 400 && chance(0.06)) { b.lifeLevel = 1; lifeWhere = " (floating, cloud layer)"; }
    } else if (b.kind == Kind::Planet) {
        enum Atm { NONE, THIN_CO2, THIN_N2, N2_CO2, N2_CH4, THICK_CO2, N2_O2 };
        double t0 = teq(0.3);
        Atm atm;
        if (km < 2500)       atm = chance(0.15) ? THIN_CO2 : NONE;          // too small to hold much air
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
        else if (atm != NONE && b.tempK > 180 && b.tempK < 273 && chance(0.15)) { b.lifeLevel = 1; lifeWhere = " (under the ice)"; }
        if (b.lifeLevel >= 2 && (atm == N2_CO2 || atm == THIN_CO2)) atm = N2_O2;   // photosynthesis oxygenates the air
        breathable = atm == N2_O2 && b.tempK > 250 && b.tempK < 322;

        static const char *atmName[] = { "None", "Thin carbon dioxide", "Thin nitrogen", "Nitrogen, carbon dioxide",
                                         "Nitrogen, methane", "Thick carbon dioxide (crushing)", "Nitrogen, oxygen" };
        b.atmosphere = std::string(atmName[atm]) + (breathable ? " - breathable" : "");
        if (liquid)                              b.water = ocean > 0.9 ? "Global ocean" : TextFormat("Oceans, %d%% of surface", (int)(ocean * 100));
        else if (atm != NONE && b.tempK < 273)   b.water = chance(0.6) ? "Frozen oceans" : "Polar ice caps";
        else if (b.tempK < 200)                  b.water = "Subsurface ice";
        else                                     b.water = "None";

        if (b.tempK > 1000)                { b.type = "Lava world";            col = { 255, 80, 40, 255 }; }
        else if (atm == THICK_CO2)         { b.type = "Greenhouse world";      col = { 240, 210, 120, 255 }; }
        else if (liquid && ocean > 0.9)    { b.type = "Ocean world";           col = { 60, 140, 255, 255 }; }
        else if (liquid && breathable && b.lifeLevel == 3) { b.type = "Garden world"; col = { 90, 225, 140, 255 }; }
        else if (liquid)                   { b.type = "Temperate terrestrial"; col = { 150, 200, 130, 255 }; }
        else if (b.tempK > 320)            { b.type = km < 3000 ? "Hot rocky world" : "Desert world"; col = { 225, 150, 80, 255 }; }
        else if (b.tempK < 200 && atm != NONE) { b.type = "Ice world";         col = { 210, 240, 255, 255 }; }
        else if (atm != NONE)              { b.type = "Tundra world";          col = { 170, 190, 210, 255 }; }
        else                               { b.type = "Barren rocky world";    col = { 170, 160, 150, 255 }; }
        if (km >= 7000) b.type += " (super-Earth)";
        density = km < 3000 ? 3900.0 : km < 7000 ? 5500.0 : 6000.0;
    } else {
        // Moons
        double t0 = teq(0.3);
        bool giantParent = parent && parent->radius / KM_U >= 20000;
        if (giantParent && b.orbit < parent->radius * 12.0 && chance(0.25)) {
            b.type = "Volcanic moon"; b.atmosphere = "Thin sulfur dioxide"; b.water = "None";
            b.tempK = teq(0.4) + uni(20, 60);                          // tidal heating
            col = { 240, 220, 90, 255 };
        } else if (t0 < 200) {
            if (km > 1800 && chance(0.3)) {
                b.type = "Titan-like moon"; b.atmosphere = "Thick nitrogen, methane";
                b.water = "Methane lakes; water ocean below";
                b.tempK = teq(0.25) + uni(10, 20);
                if (chance(0.15)) { b.lifeLevel = 1; lifeWhere = " (subsurface ocean)"; }
                col = { 230, 190, 110, 255 };
            } else {
                bool sub = giantParent && chance(0.45);              // tidal heat keeps an ocean liquid under the ice
                b.type = "Icy moon"; b.atmosphere = "None"; b.water = sub ? "Subsurface ocean" : "Surface ice";
                b.tempK = teq(0.6);
                if (sub && chance(0.3)) { b.lifeLevel = 1; lifeWhere = " (subsurface ocean)"; }
                col = { 200, 235, 255, 255 }; density = 1900.0;
            }
        } else if (km > 1800 && b.inHZ && chance(0.35)) {
            b.tempK = TemperateK(flux) + (uni(15, 40) - 27.0) * 0.2;   // a moon big enough to hold air, in the zone
            liquid = true;
            if (liquid) b.lifeLevel = rollLife();
            breathable = b.lifeLevel >= 2 && b.tempK > 250 && b.tempK < 322;
            b.type = "Habitable moon";
            b.atmosphere = b.lifeLevel >= 2 ? (breathable ? "Nitrogen, oxygen - breathable" : "Nitrogen, oxygen") : "Nitrogen, carbon dioxide";
            b.water = liquid ? TextFormat("Oceans, %d%% of surface", (int)uni(20, 90)) : "Polar ice caps";
            col = { 110, 210, 150, 255 };
        } else {
            b.type = t0 > 320 ? "Scorched rocky moon" : "Rocky moon"; b.atmosphere = "None"; b.water = "None";
            b.tempK = teq(0.12);
            col = { 160, 155, 150, 255 };
        }
    }

    static const char *lifeName[] = { "None", "Microbial", "Simple multicellular", "Complex - flora and fauna" };
    b.life = std::string(lifeName[b.lifeLevel]) + lifeWhere;

    double r = b.radius * METERS_PER_UNIT;
    b.gravity = 6.674e-11 * (4.0 / 3.0 * PI * r * r * r * density) / (r * r) / 9.81;

    // Settlements: people live wherever they can make it work
    double roll = uni(0, 1);
    if (breathable && roll < 0.85) {
        b.population = pow(10.0, uni(7.5, 9.7));
        b.settlement = b.population >= 1e9 ? "Core world" : "Settled world";
    } else if (liquid && roll < 0.5) {
        b.population = pow(10.0, uni(5.0, 7.3)); b.settlement = "Domed colonies";
    } else if (giant && roll < 0.3) {
        b.population = pow(10.0, uni(3.5, 5.7)); b.settlement = "Orbital habitats, gas mining";
    } else if (b.lifeLevel >= 1 && roll < 0.35) {
        b.population = pow(10.0, uni(1.7, 3.0)); b.settlement = "Research station";
    } else if (!giant && b.tempK < 400 && roll < 0.18) {
        b.population = pow(10.0, uni(2.7, 4.7)); b.settlement = "Mining outpost";
    } else if (!giant && b.tempK < 400 && roll < 0.26) {
        b.population = pow(10.0, uni(4.0, 6.5)); b.settlement = "Underground colony";
    } else if (b.tempK >= 400 && roll < 0.06) {
        b.population = pow(10.0, uni(1.5, 2.5)); b.settlement = "Research outpost";
    } else {
        b.settlement = "Uninhabited";
    }
    b.econ = b.settlement == "Core world"     ? Econ::HighTech     : b.settlement == "Settled world"      ? Econ::Agricultural
           : b.settlement == "Domed colonies" ? Econ::Colony       : b.settlement == "Orbital habitats, gas mining" ? Econ::GasMining
           : b.settlement == "Mining outpost" ? Econ::Mining       : b.settlement == "Underground colony" ? Econ::Industrial
           : b.population > 0                 ? Econ::Research     : Econ::None;
    b.lines = col;
}

// ---------------------------------------------------------------------------
// Stations: orbital ports over settled worlds, skimmers at gas giants,
// refineries in the belts, and free-floating waystations. Own random
// generator, so adding them changed nothing else in any system.
// ---------------------------------------------------------------------------
static void GenerateStations(const StarSystem &sys, LoadedSystem &out)
{
    std::mt19937 rng(sys.seed ^ 0x7F4A7C15u);
    auto uni    = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto chance = [&](double p) { return uni(0.0, 1.0) < p; };
    const int MAX_STATIONS = 8;
    int count = 0;

    auto add = [&](const std::string &name, DVec3 pos, int parent, const char *type, Econ econ,
                   double popLo, double popHi, Color col) {
        if (count >= MAX_STATIONS) return;
        Body s;
        s.name = name; s.local = pos; s.kind = Kind::Station; s.parent = parent;
        s.radius = uni(100.0, 260.0);                                // 0.5 - 1.3 km
        s.orbit = Len(pos - out.bodies[parent].local);
        s.lines = col; s.fill = BLACK;
        s.type = type; s.settlement = type; s.econ = econ;
        s.population = pow(10.0, uni(popLo, popHi));
        s.atmosphere = "Pressurized"; s.water = "Recycled"; s.life = "None";
        s.gravity = uni(0.3, 1.0);                                   // from its spin
        s.tempK = 293.0;
        s.spin = (float)(uni(2.0, 7.0) * (chance(0.5) ? 1 : -1));
        s.flux = out.bodies[parent].flux;
        out.bodies.push_back(s);
        count++;
    };
    // A point circling body 'bi' at 'rMin'..'rMax' of its radii, near the system plane
    auto around = [&](int bi, double rMin, double rMax) {
        const Body &b = out.bodies[bi];
        double ang = uni(0, 2 * PI), r = b.radius * uni(rMin, rMax), tilt = uni(-0.2, 0.2);
        return b.local + Norm(sys.u * cos(ang) + sys.v * sin(ang) + sys.n * tilt) * r;
    };
    const Color PORT = { 200, 240, 255, 255 }, YARD = { 255, 200, 120, 255 }, REFINERY = { 255, 170, 90, 255 },
                SKIM = { 255, 230, 140, 255 }, LAB = { 140, 255, 200, 255 }, FREE = { 230, 200, 255, 255 },
                ORE = { 210, 180, 150, 255 };

    int worlds = (int)out.bodies.size();
    for (int bi = 0; bi < worlds; bi++) {
        const Body &b = out.bodies[bi];
        if (b.kind == Kind::Star) continue;
        bool giant = b.kind == Kind::Planet && b.radius >= 20000 * KM_U;
        double roll = uni(0, 1), roll2 = uni(0, 1);                  // always two draws per world
        std::string n = b.name;
        switch (b.econ) {
        case Econ::HighTech:
            if (roll < 0.95) add(n + " Highport", around(bi, 3.0, 6.0), bi, "Orbital port", b.econ, 4.5, 5.8, PORT);
            if (roll2 < 0.5) add(n + " Yards", around(bi, 3.0, 6.0), bi, "Orbital shipyard", Econ::Shipyard, 3.7, 4.7, YARD);
            break;
        case Econ::Agricultural:
            if (roll < 0.8) add(n + " Highport", around(bi, 3.0, 6.0), bi, "Orbital port", b.econ, 4.0, 5.3, PORT);
            break;
        case Econ::Colony:
            if (roll < 0.45) add(n + " Orbital", around(bi, 3.0, 6.0), bi, "Orbital port", b.econ, 3.0, 4.0, PORT);
            break;
        case Econ::GasMining:
            if (roll < 0.8) add(n + " Skimmer Station", around(bi, 1.6, 2.5), bi, "Gas skimmer station", Econ::Skimmer, 2.5, 3.5, SKIM);
            break;
        case Econ::Mining:
            if (roll < 0.4) add(n + " Ore Terminal", around(bi, 3.0, 6.0), bi, "Ore terminal", Econ::Mining, 2.5, 3.5, ORE);
            break;
        case Econ::Industrial:
            if (roll < 0.5) add(n + " Yards", around(bi, 3.0, 6.0), bi, "Orbital shipyard", Econ::Shipyard, 3.5, 4.5, YARD);
            break;
        case Econ::Research:
            break;
        default:                                                     // uninhabited
            if (giant && roll < 0.3) add(n + " Skimmer Station", around(bi, 1.6, 2.5), bi, "Gas skimmer station", Econ::Skimmer, 2.3, 3.3, SKIM);
            else if (!giant && roll < 0.07) add(n + " Research Station", around(bi, 2.5, 5.0), bi, "Research station", Econ::Research, 1.6, 2.6, LAB);
            break;
        }
    }

    // Refineries in the belts (where the miners are)
    for (const Zone &z : out.zones) {
        if (z.ring) continue;
        double ang = uni(0, 2 * PI), roll = uni(0, 1);
        DVec3 pos = z.center + (z.u * cos(ang) + z.v * sin(ang)) * (0.5 * (z.rIn + z.rOut));
        if (roll < 0.85) add(z.name + " Refinery", pos, 0, "Ore refinery", Econ::Refinery, 2.8, 3.9, REFINERY);
    }

    // Free-floating: a waystation between two planets, sometimes a freeport out at the edge
    std::vector<int> planets;
    for (int i = 0; i < worlds; i++) if (out.bodies[i].kind == Kind::Planet) planets.push_back(i);
    double roll = uni(0, 1), roll2 = uni(0, 1), ang = uni(0, 2 * PI), ang2 = uni(0, 2 * PI), pick = uni(0, 1);
    if (planets.size() >= 2 && roll < 0.6) {
        int i = (int)(pick * (planets.size() - 1));
        double r = 0.5 * (out.bodies[planets[i]].orbit + out.bodies[planets[i + 1]].orbit);
        add(sys.name + " Waystation", (sys.u * cos(ang) + sys.v * sin(ang)) * r, 0, "Waystation", Econ::Freeport, 3.0, 4.2, FREE);
    }
    if (!planets.empty() && roll2 < 0.3) {
        double r = out.bodies[planets.back()].orbit * 1.15;
        add(sys.name + " Freeport", (sys.u * cos(ang2) + sys.v * sin(ang2)) * r, 0, "Freeport", Econ::Freeport, 3.3, 4.5, FREE);
    }
}

// Planets and moons of one system. Deterministic: the same seed always
// produces the same system, so it can be thrown away and rebuilt.
void GenerateSystemContents(const StarSystem &sys, LoadedSystem &out)
{
    std::mt19937 rng(sys.seed);
    auto uni  = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto irng = [&](int a, int b)       { return std::uniform_int_distribution<int>(a, b)(rng); };
    const char *roman[] = { "I", "II", "III", "IV", "V", "VI" };

    out.bodies.clear();
    out.orbits.clear();

    // --- Stars: the primary at the center, close companions just outside it ---
    std::mt19937 srng(sys.seed ^ 0x6C8E9CF5u);                // own generator: keeps the planet draws unchanged
    auto suni = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(srng); };
    const double scale = OrbitScale(sys);
    const bool multiple = sys.stars.size() > 1;
    std::vector<int> starBodies;
    for (size_t i = 0; i < sys.stars.size(); i++) {
        const StarInfo &st = sys.stars[i];
        DVec3 pos = {};
        if (i > 0 && st.close) {
            double ang = suni(0, 2 * PI), sep = st.sep * scale * AU_U;
            pos = sys.u * (cos(ang) * sep) + sys.v * (sin(ang) * sep);
        }
        Body b = { multiple ? sys.name + " " + (char)('A' + i) : sys.name, pos, st.radius,
                   Color{ (unsigned char)((st.color.r + 255) / 2), (unsigned char)((st.color.g + 255) / 2),
                          (unsigned char)((st.color.b + 255) / 2), 255 },
                   st.color, Kind::Star, i == 0 ? -1 : 0, Len(pos) };
        b.massSolar = st.mass;
        b.type  = StarDescription(st);
        b.tempK = st.temp;
        starBodies.push_back((int)out.bodies.size());
        out.bodies.push_back(b);
    }

    int    planetCount = irng(5, 9);
    double a = uni(0.3, 0.7) * AU_U;                        // first orbit (before scaling to the star)

    for (int p = 0; p < planetCount; p++) {
        if (p > 0) a *= uni(1.55, 2.1);                     // roughly geometric spacing (Sol: ~1.7)

        // Rocky worlds inside ~2 AU (scaled); past that, mostly gas giants
        bool   giant = a > 2.2 * AU_U && uni(0, 1) < 0.65;
        double R     = (giant ? uni(20000, 72000) : uni(2000, 7500)) * KM_U;

        double ang = uni(0, 2 * PI);
        double ar  = a * scale;                             // actual orbit
        DVec3  pos = sys.u * (cos(ang) * ar) + sys.v * (sin(ang) * ar);
        Color  col = PLANET_COLORS[irng(0, 7)];             // replaced by a type color below
        std::string pname = sys.name + " " + (char)('b' + p);
        int planetIndex = (int)out.bodies.size();
        out.bodies.push_back({ pname, pos, R, col, BLACK, Kind::Planet, 0, ar });

        int    moonCount = giant ? irng(1, 4) : (uni(0, 1) < 0.3 ? 1 : 0);
        double mo = R * (giant ? uni(5, 10) : uni(20, 50)); // Earth's Moon sits at ~60 Earth radii
        for (int m = 0; m < moonCount; m++) {
            if (m > 0) mo *= uni(1.5, 2.2);
            double mr = uni(300, fmin(2600, 0.4 * R / KM_U)) * KM_U;
            double ma = uni(0, 2 * PI);
            DVec3  mp = pos + sys.u * (cos(ma) * mo) + sys.v * (sin(ma) * mo);
            out.bodies.push_back({ pname + " " + roman[m], mp, mr, MOON_COLOR, BLACK, Kind::Moon, planetIndex, mo });
        }
    }

    // --- Wide companions: beyond the outermost planet ---
    double outer = 0.0;
    for (const Body &b : out.bodies) if (b.kind == Kind::Planet) outer = fmax(outer, b.orbit);
    for (size_t i = 1; i < sys.stars.size(); i++) {
        if (sys.stars[i].close) continue;
        Body &b = out.bodies[starBodies[i]];
        double ang = suni(0, 2 * PI), tilt = suni(-0.15, 0.15);
        b.local = Norm(sys.u * cos(ang) + sys.v * sin(ang) + sys.n * tilt) * (sys.stars[i].sep * outer);
        b.orbit = Len(b.local);
    }

    // --- Climate, life and settlements ---
    std::vector<std::pair<DVec3, double>> lights;                // star positions and luminosities
    for (size_t i = 0; i < sys.stars.size(); i++) lights.push_back({ out.bodies[starBodies[i]].local, sys.stars[i].lum });
    std::mt19937 wrng(sys.seed ^ 0x1B873593u);
    for (int bi = 0; bi < (int)out.bodies.size(); bi++) {
        Body &b = out.bodies[bi];
        if (b.kind == Kind::Star) continue;
        double flux = 0.0;                                       // light received, in Earths (1 = Earth's)
        for (auto &l : lights) { double d = Len(b.local - l.first) / AU_U; flux += l.second / (d * d); }
        DescribeWorld(b, b.kind == Kind::Moon ? &out.bodies[b.parent] : nullptr, flux, sys, wrng);
    }
    // Orbit lines in each planet's (new) color
    for (const Body &b : out.bodies)
        if (b.kind == Kind::Planet) out.orbits.push_back({ sys.u, sys.v, b.orbit, Dim(b.lines, 0.25f) });
    // The habitable zone, as two faint green rings
    out.orbits.push_back({ sys.u, sys.v, sys.hzIn,  { 30, 110, 55, 255 } });
    out.orbits.push_back({ sys.u, sys.v, sys.hzOut, { 30, 110, 55, 255 } });

    GenerateZones(sys, out);
    GenerateStations(sys, out);
}

std::vector<StarSystem> GenerateGalaxy(unsigned seed)
{
    std::mt19937 rng(seed);
    auto uni  = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    auto irng = [&](int a, int b)       { return std::uniform_int_distribution<int>(a, b)(rng); };

    const char *syllables[] = { "ka", "vor", "ze", "tal", "ri", "os", "men", "dra",
                                "lu", "ix", "bel", "sha", "tor", "ny", "quel", "ar" };
    auto makeName = [&]() {
        std::string n;
        int count = irng(2, 3);
        for (int i = 0; i < count; i++) n += syllables[irng(0, 15)];
        n[0] = (char)toupper(n[0]);
        return n;
    };

    // Star positions in light years, in a flat disk, home at the origin
    std::vector<DVec3> posLy = { { 0, 0, 0 } };
    for (int tries = 0; (int)posLy.size() < SYSTEM_COUNT && tries < 100000; tries++) {
        double a = uni(0, 2 * PI), r = GALAXY_RADIUS_LY * sqrt(uni(0.0, 1.0));
        DVec3  p = { r * cos(a), uni(-0.5, 0.5) * GALAXY_THICKNESS_LY, r * sin(a) };
        bool ok = true;
        for (const DVec3 &o : posLy) if (Len(p - o) < MIN_SEPARATION_LY) { ok = false; break; }
        if (ok) posLy.push_back(p);
    }

    // Snap to a coarse grid so star-to-star differences are exact in double precision
    auto snap = [](double x) { return std::round(x / 1e6) * 1e6; };

    std::vector<StarSystem> systems;
    for (const DVec3 &p : posLy) {
        StarSystem s;
        s.name       = makeName();
        s.gal        = { snap(p.x * LY_U), snap(p.y * LY_U), snap(p.z * LY_U) };
        s.starRadius = uni(0.4, 1.8) * RSUN_U;     // (draw kept so names/seeds don't shift; replaced below)
        s.seed       = (unsigned)rng();
        GenerateStars(s);

        Vector3 n = Vector3Normalize({ (float)uni(-0.45, 0.45), 1.0f, (float)uni(-0.45, 0.45) });
        Vector3 u = Vector3Normalize(Vector3CrossProduct(n, { 0, 0, 1 }));
        s.n = ToD(n); s.u = ToD(u); s.v = ToD(Vector3CrossProduct(n, u));
        systems.push_back(s);
    }

    // Galaxy director: a dozen systems is too few to rely on luck, so make sure there's at
    // least one O, B and A star, a trinary and a wide binary, by upgrading some of the
    // (common) red and orange dwarf systems. The home system is never changed.
    std::vector<int> pool;
    for (int i = 1; i < (int)systems.size(); i++)
        if (systems[i].stars[0].cls == 'M' || systems[i].stars[0].cls == 'K') pool.push_back(i);
    auto has = [&](auto pred) { for (const StarSystem &s : systems) if (pred(s)) return true; return false; };
    const char *hot = "OBA";
    for (int c = 0; c < 3 && !pool.empty(); c++)
        if (!has([&](const StarSystem &s) { return s.stars[0].cls == hot[c]; })) {
            int i = pool.back(); pool.pop_back();
            GenerateStars(systems[i], c, (int)systems[i].stars.size() - 1);
        }
    if (!pool.empty() && !has([](const StarSystem &s) { return s.stars.size() == 3; })) {
        int i = pool.back(); pool.pop_back();
        GenerateStars(systems[i], -1, 2);
    }
    if (!pool.empty() && !has([](const StarSystem &s) { return s.stars.size() == 2 && !s.stars[1].close; })) {
        int i = pool.back(); pool.pop_back();
        GenerateStars(systems[i], -1, 1, 1);
    }

    // Size each gravity field from the actual planets and companions
    for (StarSystem &s : systems) {
        LoadedSystem tmp;
        s.fieldR = 1.0;
        GenerateSystemContents(s, tmp);
        double outer = 0.0;
        for (const Body &b : tmp.bodies)
            if (b.kind != Kind::Station) outer = fmax(outer, Len(b.local) + b.radius);   // (stations don't move the field edge)
        s.fieldR = fmax(outer * FIELD_EDGE_MARGIN, 10.0 * AU_U);
        s.fieldH = s.fieldR * FIELD_HEIGHT_RATIO;
    }
    return systems;
}
