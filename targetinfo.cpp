// targetinfo.cpp - the facts shown about a target
#include "targetinfo.h"
#include "economy.h"
#include "format.h"

const double G_SI = 6.674e-11, MSUN_KG = 1.989e30;

static double BodyMassKg(const Body &b)
{
    return b.kind == Kind::Star ? b.massSolar * MSUN_KG : b.massEarth * MEARTH_KG;
}

static std::vector<InfoRow> ZoneBasics(const Captain &c, const Zone &z);

// "Metals (rich), Ices (some)": what's worth having here
static std::string ResourcesLine(const Body &b)
{
    std::string s;
    for (int r = 0; r < RES_COUNT; r++) {
        float v = b.resource[r];
        if (v < 0.2f) continue;
        s += (s.empty() ? "" : ", ") + std::string(ResourceName(r)) + (v >= 0.7f ? " (rich)" : v >= 0.45f ? "" : " (some)");
    }
    return s.empty() ? "Nothing of note" : s;
}

// "Ores" rows: the full list, a few names to a line
static void AddListRows(std::vector<InfoRow> &rows, const char *label, const std::string &list, int perLine = 3)
{
    std::string line;
    int n = 0;
    size_t at = 0;
    while (at <= list.size()) {
        size_t comma = list.find(", ", at);
        std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (n && n % perLine == 0) { rows.push_back({ n == perLine ? label : "", line + ","}); line.clear(); }
        line += (line.empty() ? "" : ", ") + item;
        n++;
        if (comma == std::string::npos) break;
        at = comma + 2;
    }
    if (!line.empty()) rows.push_back({ n <= perLine ? label : "", line });
}

// One line for the hover panel: what kind of world, and who lives there
std::string WorldSummary(const Body &b)
{
    if (b.kind == Kind::Star) return b.type;
    std::string s = b.type;
    if (b.population > 0)      s += "  -  pop " + FormatPopulation(b.population);
    else if (b.lifeLevel > 0)  s += "  -  native life";
    return s;
}

std::string StarSummary(const StarSystem &s)
{
    return std::string(StarDescription(s.stars[0])) +
           (s.stars.size() > 1 ? (s.stars.size() == 2 ? "  -  binary" : "  -  trinary") : "") +
           (s.develop > 0.0f ? "" : "  -  uncharted");
}

static std::string SystemKind(const StarSystem &s)
{
    if (s.stars.size() == 1) return "Single star";
    std::string k = s.stars.size() == 2 ? "Binary" : "Trinary";
    for (size_t i = 1; i < s.stars.size(); i++)
        k += TextFormat("  +%s %s", StarClassCode(s.stars[i]), s.stars[i].close ? "(close)" : "(wide)");
    return k;
}

// Rows for a star, from its StarInfo; 'L' adds what's only known once the system is loaded
static std::vector<InfoRow> StarRows(const StarSystem &s, const StarInfo &st, const LoadedSystem *L)
{
    std::vector<InfoRow> rows = {
        { "Type",        StarDescription(st) },
        { "Temperature", TextFormat("%s K", Thousands(st.temp).c_str()) },
        { "Mass",        TextFormat("%.2f Sun", st.mass) },
        { "Radius",      TextFormat("%s km  (%.2f Sun)", Thousands(st.radius / KM_U).c_str(), st.radius / RSUN_U) },
        { "Luminosity",  st.lum < 0.01 ? TextFormat("%.4f Sun", st.lum) : st.lum < 100 ? TextFormat("%.2f Sun", st.lum)
                                                                       : TextFormat("%s Sun", Thousands(st.lum).c_str()) },
        { "System",      SystemKind(s) },
        { "Age",         s.age < 0.1 ? TextFormat("%.0f million years", s.age * 1000.0) : TextFormat("%.1f billion years", s.age) },
        { "Heavy elements", TextFormat("%.0f%% of the Sun's", 100.0 * pow(10.0, s.metal)) },
        { "Habitable zone", s.hzOut < 0.01 * AU_U ? TextFormat("%s - %s km", Thousands(s.hzIn / KM_U).c_str(), Thousands(s.hzOut / KM_U).c_str())
                                                  : TextFormat("%.2f - %.2f AU", s.hzIn / AU_U, s.hzOut / AU_U) },
        { "Human presence", s.develop >= 0.75f ? "Core systems" : s.develop >= 0.3f ? "Settled space" : s.develop > 0.0f ? "Frontier" : "None - uncharted" } };
    if (L) {
        int planets = 0, dwarfs = 0, belts = 0, life = 0, stations = 0; double pop = 0;
        for (const Body &o : L->bodies) {
            if (o.kind == Kind::Planet) (o.dwarf ? dwarfs : planets)++;
            if (o.kind == Kind::Station) stations++;
            if (o.kind != Kind::Star) { pop += o.population; if (o.lifeLevel > 0) life++; }
        }
        for (const Zone &z : L->zones) if (!z.ring) belts++;
        rows.push_back({ "Planets", TextFormat("%d   (%d dwarf, %d belt%s)", planets, dwarfs, belts, belts == 1 ? "" : "s") });
        rows.push_back({ "Stations", TextFormat("%d", stations) });
        rows.push_back({ "Worlds with life", TextFormat("%d", life) });
        rows.push_back({ "Population", pop > 0 ? FormatPopulation(pop) : "None" });
    } else {
        rows.push_back({ "Planets", "Unknown - not yet in range" });
    }
    return rows;
}

// Economy, exports and imports of an inhabited world or station
static void AddEconomyRows(std::vector<InfoRow> &rows, const Body &b)
{
    if (b.population <= 0 || b.econ == Econ::None) return;
    Market m = MakeMarket(b);
    rows.push_back({ "Economy", EconName(b.econ) });
    std::string ex = ExportsLine(m, 99), im = ImportsLine(m, 99);
    if (!ex.empty()) AddListRows(rows, "Exports", ex, 2);
    if (!im.empty()) AddListRows(rows, "Imports", im, 2);
    rows.push_back({ "Fuel", TextFormat("%.0f cr/t", m.fuelPrice) });
}

static std::vector<InfoRow> StationRows(const LoadedSystem &L, const Body &b)
{
    const Body &host = L.bodies[b.parent];
    std::vector<InfoRow> rows = {
        { "Type", b.type },
        { "Orbit", host.kind == Kind::Star ? TextFormat("%.2f AU from %s", b.orbit / AU_U, host.name.c_str())
                                           : TextFormat("%s km from %s", Thousands(b.orbit / KM_U).c_str(), host.name.c_str()) },
        { "Size", TextFormat("%.1f km across", 2.0 * b.radius / KM_U) },
        { "Spin gravity", TextFormat("%.2f g", b.gravity) },
        { "Population", FormatPopulation(b.population) } };
    AddEconomyRows(rows, b);
    return rows;
}

static std::vector<InfoRow> BodyRows(const World &w, const Captain &c, int bi)
{
    const LoadedSystem &L = c.loaded;
    const StarSystem &S = w.systems[L.index];
    const Body &b = L.bodies[bi];
    std::vector<InfoRow> rows;
    if (b.kind == Kind::Star) {
        rows = StarRows(S, S.stars[bi], &L);                     // stars come first, in StarSystem order
        if (bi > 0) rows.insert(rows.begin() + 1, { "Orbit", TextFormat("%.2f AU from %s", b.orbit / AU_U, L.bodies[0].name.c_str()) });
        return rows;
    }
    if (b.kind == Kind::Station) return StationRows(L, b);
    const Body &host = b.kind == Kind::Moon ? L.bodies[b.parent] : L.bodies[0];
    double period;
    if (b.kind == Kind::Planet) {
        double mSun = 0.0;                                       // every star inside the orbit pulls on it
        for (const Body &o : L.bodies) if (o.kind == Kind::Star && o.orbit < b.orbit) mSun += o.massSolar;
        period = sqrt(pow(b.orbit / AU_U, 3.0) / mSun) * 365.25 * 86400.0;
    } else {
        double a = b.orbit * METERS_PER_UNIT;
        period = 2.0 * PI * sqrt(a * a * a / (G_SI * BodyMassKg(host)));
    }
    rows = { { "Type", b.kind == Kind::Moon ? b.type + " of " + host.name : b.type },
             { "Mass", b.massEarth >= 0.01 ? TextFormat("%.2f Earth", b.massEarth) : TextFormat("%.5f Earth", b.massEarth) },
             { "Radius", TextFormat("%s km  (%.2f Earth)", Thousands(b.radius / KM_U).c_str(), b.radius / KM_U / REARTH_KM) },
             { "Gravity", TextFormat("%.2f g", b.gravity) },
             { "Orbit", b.kind == Kind::Planet
                            ? TextFormat("%.2f AU%s", b.orbit / AU_U, b.inHZ ? "  (habitable zone)" : "")
                            : TextFormat("%s km", Thousands(b.orbit / KM_U).c_str()) },
             { "Orbital period", FormatPeriod(period) },
             { "Temperature", FormatTemp(b.tempK) },
             { "Atmosphere", b.atmosphere },
             { "Water", b.water },
             { "Life", b.life },
             { "Resources", ResourcesLine(b) },
             { "Settlement", b.population > 0 ? std::string(SettlementName(b.settlement)) + "  -  " + FormatPopulation(b.population)
                                              : std::string(SettlementName(b.settlement)) } };
    if (b.kind == Kind::Planet) {
        std::vector<std::string> moons;
        for (const Body &o : L.bodies) if (o.kind == Kind::Moon && o.parent == bi) moons.push_back(o.name);
        const Zone *ring = nullptr;
        for (const Zone &z : L.zones) if (z.ring && z.parent == bi) ring = &z;
        rows.push_back({ "Moons", moons.empty() ? "None" : TextFormat("%d", (int)moons.size()) });
        for (const std::string &mn : moons) rows.push_back({ "", "  " + mn });
        rows.push_back({ "Ring system", ring ? TextFormat("Yes  (%.1f - %.1f planet radii)", ring->rIn / b.radius, ring->rOut / b.radius)
                                             : "No" });
    }
    std::vector<std::string> stations;
    for (const Body &o : L.bodies) if (o.kind == Kind::Station && o.parent == bi) stations.push_back(o.name);
    if (!stations.empty()) {
        rows.push_back({ "Stations", TextFormat("%d", (int)stations.size()) });
        for (const std::string &sn : stations) rows.push_back({ "", "  " + sn });
    }
    AddEconomyRows(rows, b);
    return rows;
}

static std::vector<InfoRow> ZoneRows(const Captain &c, const Zone &z)
{
    std::vector<InfoRow> rows = ZoneBasics(c, z);
    AddListRows(rows, "Ores", CompositionOres(z.composition, 99));
    return rows;
}

static std::vector<InfoRow> ZoneBasics(const Captain &c, const Zone &z)
{
    const LoadedSystem &L = c.loaded;
    if (z.ring) {
        const Body &p = L.bodies[z.parent];
        int gaps = 0; for (float d : z.bandDensity) if (d <= 0.0f) gaps++;
        return { { "Type", "Ring system of " + p.name },
                 { "Extent", TextFormat("%s - %s km", Thousands(z.rIn / KM_U).c_str(), Thousands(z.rOut / KM_U).c_str()) },
                 { "", TextFormat("(%.1f - %.1f planet radii)", z.rIn / p.radius, z.rOut / p.radius) },
                 { "Bands", TextFormat("%d  (%d gap%s)", (int)z.bandDensity.size(), gaps, gaps == 1 ? "" : "s") },
                 { "Composition", CompositionName(z.composition) } };
    }
    return { { "Type", "Asteroid belt" },
             { "Extent", TextFormat("%.2f - %.2f AU from %s", z.rIn / AU_U, z.rOut / AU_U, L.bodies[0].name.c_str()) },
             { "Thickness", TextFormat("%.3f AU", 2.0 * z.halfH / AU_U) },
             { "Composition", CompositionName(z.composition) } };
}

// Another captain's ship
static std::vector<InfoRow> ShipRows(const World &w, const Traffic &t)
{
    float speed = Vector3Length(t.ship.vel);
    const char *drive = !t.hyper ? (speed > MAX_SPEED * 1.01f ? "Spooling down" : "Cruise")
                                 : (speed > HYPER_SPEED * 1.01 ? "Warp" : "Hyper");
    return { { "Captain", t.name },
             { "Speed", FormatSpeed(speed) },
             { "Drive", drive },
             { "Near", w.systems[t.ship.anchor].name },
             { "Activity", t.mining ? "Mining" : t.thrusting ? "Under thrust" : "Coasting" } };
}

std::vector<InfoRow> TargetInfo(const World &w, const Captain &c, TargetId t)
{
    t = CanonicalTarget(c, t);
    switch (t.type) {
    case TargetId::Star:
        if (!w.systems.Exists(t.system)) return {};
        return StarRows(w.systems[t.system], w.systems[t.system].stars[0], nullptr);
    case TargetId::Body:
        if (!TargetBody(c, t)) return {};
        return BodyRows(w, c, t.index);
    case TargetId::Zone:
        if (t.system != c.loaded.index || t.index < 0 || t.index >= (int)c.loaded.zones.size()) return {};
        return ZoneRows(c, c.loaded.zones[t.index]);
    case TargetId::Asteroid:
        if (const Asteroid *a = FindAsteroid(c.field, t.rock))
            return { { "Type", GoodData(a->ore).ice ? "Ice chunk" : "Asteroid" },
                     { "Radius", TextFormat("%.0f m", AsteroidRadius(*a) * METERS_PER_UNIT) },
                     { "Ore", GoodData(a->ore).name },
                     { "Remaining", TextFormat("%.1f of %.0f t", a->oreLeft, a->oreMax) } };
        return {};
    case TargetId::Ship:
        if (const Traffic *other = w.FindTraffic(t.index)) return ShipRows(w, *other);
        return {};
    default:
        return {};
    }
}
