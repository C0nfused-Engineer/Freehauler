// economy.cpp - building the markets, running the economy, and trading
#include "economy.h"
#include "format.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

static double ClampD(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static const int FUEL_GOODS[3] = { REF_REACTOR_FUEL, REF_FUEL, RAW_HELIUM3 };      // what ships burn, in the order MarketDef::fleetFuel keeps them
static void SeedFleets(EconBase &base, const FactionSet &fs, const std::vector<double> *carried);
static void Shipping(Economy &e, const EconBase &base, const FactionSet &fs, double days);
static void Develop(Economy &e, const EconBase &base, double clock);
static void RouteToCapitals(EconBase &base, const FactionSet &fs);
double BuiltUp(const MarketDef &d, const MarketState &s);
double Crowding(const MarketDef &d, const MarketState &s);
static void FleetNeeds(EconBase &base, const std::vector<double> &carried);
static double Roll(Economy &e) { e.dice = Mix64(e.dice); return (double)(e.dice >> 11) * (1.0 / 9007199254740992.0); }

// ---------------------------------------------------------------------------
// The worlds: who lives there, what they use, and what industry they start with
// ---------------------------------------------------------------------------
static bool IsPort(Settlement s)
{
    return s == Settlement::OrbitalPort || s == Settlement::Starbase || s == Settlement::Waystation || s == Settlement::Freeport;
}

// What a place's people, ports and yards use up, a day (power aside: that's their cities', in plasma cells)
static void Consumption(float *use, const Body &b, double scale, const Faction *owner)
{
    auto need = [&](int g, double share) { use[g] += (float)(scale * share); };
    if (b.kind == Kind::Station) { need(REF_FOOD, 0.10); need(REF_WATER, 0.04); need(GOOD_LIQUOR, 0.01);
                                   need(REF_REACTOR_FUEL, 0.02); need(REF_FUEL, 0.01); }             // (and the ships that call burn fuel)
    else { need(REF_FOOD, 0.25); need(REF_WATER, 0.08); need(GOOD_CONSUMER, 0.06); need(GOOD_MEDICINES, 0.02);
           need(GOOD_LIQUOR, 0.015); need(GOOD_ELECTRONICS, 0.012); need(GOOD_LUXURY, 0.002); }
    switch (b.settlement) {
    case Settlement::CoreWorld:         need(GOOD_LUXURY, 0.006); need(RAW_HELIUM3, 0.003); need(GOOD_MACHINERY, 0.01); break;
    case Settlement::SettledWorld:      need(REF_FERTILIZER, 0.05); need(GOOD_MACHINERY, 0.02); break;
    case Settlement::DomedColonies:     need(GOOD_HABITAT_MODULES, 0.02); need(GOOD_MACHINERY, 0.02); break;
    case Settlement::OrbitalHabitats: case Settlement::SkimmerStation: need(GOOD_MACHINERY, 0.02); break;
    case Settlement::MiningOutpost: case Settlement::OreTerminal: case Settlement::OreRefinery:
        need(GOOD_MACHINERY, 0.03); need(GOOD_ROBOTICS, 0.006); need(GOOD_ELECTRONICS, 0.01); break;
    case Settlement::UndergroundColony: need(GOOD_ROBOTICS, 0.006); need(GOOD_ELECTRONICS, 0.01); break;
    case Settlement::Shipyard:          need(GOOD_WEAPONS, 0.03); break;                    // (what its ships are built from, the yard itself takes: Work)
    case Settlement::ResearchStation: case Settlement::ResearchOutpost: need(GOOD_INSTRUMENTS, 0.05); need(GOOD_ELECTRONICS, 0.03); break;
    case Settlement::SolarCollector:    need(GOOD_ELECTRONICS, 0.02); break;
    case Settlement::Greenhouse:        need(REF_FERTILIZER, 0.15); need(REF_WATER, 0.10); break;
    case Settlement::Starbase:
        // A state's ships are armed and fuelled here; the more warlike the state, the more so
        need(GOOD_WEAPONS, 0.010 * (owner ? 1.0 + owner->axis[AX_FORCE] : 1.0)); need(GOOD_MUNITIONS, 0.03);
        need(REF_FUEL, 0.06); need(RAW_HELIUM3, 0.02); need(REF_REACTOR_FUEL, 0.02);       // (a fleet's best ships burn the best fuel)
        break;
    case Settlement::OrbitalPort: case Settlement::Waystation: case Settlement::Freeport:
        need(REF_REACTOR_FUEL, 0.03); need(REF_FUEL, 0.015); break;                         // traffic
    default: break;
    }
}

static SiteInfo SiteOf(const StarSystem &s, const LoadedSystem &L, const Body &b)
{
    SiteInfo si;
    const Body *ground = b.kind == Kind::Station && b.parent > 0 && L.bodies[b.parent].kind != Kind::Star ? &L.bodies[b.parent] : nullptr;
    const Body *above = b.kind == Kind::Moon && b.parent > 0 ? &L.bodies[b.parent] : nullptr;
    for (int r = 0; r < RES_COUNT; r++) si.res[r] = b.kind == Kind::Station ? (ground ? ground->resource[r] : 0.0f) : b.resource[r];
    si.flux = (float)b.flux; si.tempK = (float)b.tempK; si.gravity = (float)b.gravity;
    si.surface = b.kind != Kind::Station;
    si.openAir = b.settlement == Settlement::CoreWorld || b.settlement == Settlement::SettledWorld;
    si.giant = (ground && ground->giant) || (above && above->giant);
    si.belt = b.settlement == Settlement::OreRefinery;
    si.yard = b.settlement == Settlement::Shipyard || b.settlement == Settlement::Starbase;
    si.strangeStar = !s.stars.empty() && s.stars[0].kind != StarKind::MainSequence;
    si.life = (int8_t)(b.kind == Kind::Station ? (ground ? ground->lifeLevel : 0) : b.lifeLevel);
    return si;
}

static void AddIndustry(MarketDef &d, int type, int owner, double site, double cap, const SiteInfo &where)
{
    for (IndustryDef &o : d.ind) if (o.type == type && o.owner == owner) { o.cap += (float)cap; return; }
    IndustryDef in;
    in.type = (uint8_t)type; in.owner = (int16_t)owner; in.site = (float)site; in.cap = (float)cap;
    in.outs = (uint8_t)IndustryProducts(type, where, in.out);
    d.ind.push_back(in);
}

// The plasma cells an economy's industries and cities take a day, flat out
static double CellsWanted(const MarketDef &d)
{
    double cells = 0.0;
    for (const IndustryDef &in : d.ind) cells += IsCivic(in.type) ? CivicCells(in.type, in.cap, in.site) : IndustryData(in.type).cells * in.cap;
    return cells;
}

// One body's part in an economy: what it uses, and the works it starts with
static void Settle(MarketDef &d, const StarSystem &s, const LoadedSystem &L, const Body &b, const Faction *owner)
{
    double scale = 2.0 * pow(b.population, 0.45);
    Consumption(d.use0, b, scale, owner);
    SiteInfo here = SiteOf(s, L, b);
    for (const IndustrySeed *seed = SettlementIndustries(b.settlement); seed->type != IND_COUNT; seed++) {
        int type = seed->type;
        // (what can't be done here is done the way that can: no open fields, then domes; no ground, then tanks)
        if (type == IND_FARM && SiteQuality(type, here) <= 0.0) type = IND_AGRIDOME;
        if (type == IND_AGRIDOME && SiteQuality(type, here) <= 0.0) type = IND_HYDROPONICS;
        double site = SiteQuality(type, here);
        if (site > 0.0) AddIndustry(d, type, b.owner, site, seed->share * scale, here);
    }
    d.pop += b.population;
}

// Its power, its cities, and its industries cut to the labour it has
static void FitOut(MarketDef &d)
{
    d.scale = (float)(2.0 * pow(d.pop, 0.45));
    d.room = d.station ? ClampD(ROOM_STATION * d.pop, ROOM_STATION_MIN, ROOM_STATION_MAX)
           : d.site.openAir ? ClampD(ROOM_SKY * d.pop, ROOM_SKY_MIN, ROOM_SKY_MAX) : ClampD(ROOM_GROUND * d.pop, ROOM_GROUND_MIN, ROOM_GROUND_MAX);
    d.room = fmax(d.room, 1.5 * d.pop);
    const double labour = LABOUR_SHARE * d.pop;
    AddIndustry(d, IND_CITY, d.owner, SiteQuality(IND_CITY, d.site), CityFor(HOUSING_SPARE * d.pop), d.site);
    AddIndustry(d, IND_SERVICES, d.owner, SiteQuality(IND_SERVICES, d.site), CityFor(d.pop), d.site);
    // Power: whatever suits the place best. (The smallest outposts have theirs shipped in, half of it.)
    int power = IND_FISSION;
    bool fuels = false;
    for (const IndustryDef &in : d.ind) if (in.type == IND_SKIMMER || in.type == IND_CHEMICAL) fuels = true;
    if (SiteQuality(IND_SOLAR, d.site) >= 0.6) power = IND_SOLAR;
    else if (SiteQuality(IND_GEOTHERMAL, d.site) > 0.0) power = IND_GEOTHERMAL;
    else if (fuels && d.pop > 2e5) power = IND_FUSION;
    bool solarAlready = false;
    for (const IndustryDef &in : d.ind) if (in.type == IND_SOLAR) solarAlready = true;        // (a solar collector is all power: it needs no more)
    for (int pass = 0; pass < 8; pass++) {                          // (round and round: the power it needs depends on its size, and its size on the labour the power leaves)
        double need = 0.0, budget = EMPLOYED_AT_START * labour;
        for (const IndustryDef &in : d.ind) {
            if (IsCivic(in.type)) budget -= IndustryLabour(in.type, in.cap, in.site);
            else need += IndustryLabour(in.type, in.cap, in.site);
        }
        double f = need > 0.0 ? pow(fmax(budget, 0.05 * labour) / need, 1.0 / LABOUR_POWER) : 1.0;
        for (IndustryDef &in : d.ind) if (!IsCivic(in.type)) in.cap *= (float)f;
        if (solarAlready) continue;
        IndustryDef *plant = nullptr;
        for (IndustryDef &in : d.ind) if (in.type == power) plant = &in;
        double cells = CellsWanted(d) * (d.pop < 300.0 ? 0.5 : 1.15);
        if (plant) plant->cap = (float)cells;
        else AddIndustry(d, power, d.owner, fmax(1.0, SiteQuality(power, d.site)), cells, d.site);
    }
    // What else could be started here, one day
    for (int type = 0; type < IND_COUNT; type++) {
        bool have = false;
        for (const IndustryDef &in : d.ind) if (in.type == type) have = true;
        double site = SiteQuality(type, d.site);
        if (!have && site > 0.0) AddIndustry(d, type, d.owner, site, 0.0, d.site);
    }
}

const int    FLEET_ROUNDS = 2;          // times a new galaxy's trade is measured and its fleets fitted to it
const double SEED_SURPLUS = 1.05;       // to begin with, human space makes this many times what it uses of each good

// How much of a good a unit of capacity turns out a day. To begin with, human space as a whole makes a little more of
// everything than it uses: the industries above say who makes what, and this sets how much that comes to. (From
// there on it's up to the industries: there is no rule that keeps it so.)
static void SetYields(EconBase &base)
{
    for (int g = 0; g < GOOD_COUNT; g++) base.perCap[g] = 1.0f;
    for (int pass = 0; pass < 8; pass++) {
        double made[GOOD_COUNT] = {}, wanted[GOOD_COUNT] = {};
        for (const MarketDef &d : base.defs) {
            for (int g = 0; g < GOOD_COUNT; g++) wanted[g] += d.use0[g];
            wanted[REF_CELLS] += CellsWanted(d);
            for (int k = 0; k < 3; k++) wanted[FUEL_GOODS[k]] += d.fleetFuel[k];
            for (const IndustryDef &in : d.ind)
                if (in.type == IND_SHIPYARD) {
                    double ship = in.cap * base.yardYield;
                    wanted[GOOD_HULL_PLATING] += YARD_PLATING * ship; wanted[GOOD_SHIP_COMPONENTS] += YARD_COMPONENTS * ship;
                    wanted[GOOD_REACTOR_CORES] += YARD_CORES * ship; wanted[GOOD_MACHINERY] += YARD_MACHINERY * ship;
                }
            for (const IndustryDef &in : d.ind) {
                const IndustryInfo &info = IndustryData(in.type);
                for (int p = 0; p < in.outs; p++) {
                    int g = in.out[p].good;
                    if (d.banned[g]) continue;
                    double q = in.cap * in.out[p].share * base.perCap[g];
                    made[g] += q;
                    for (const Ingredient &i : GoodData(g).recipe) if (i.tonnes > 0.0f) wanted[i.good] += q * i.tonnes;
                    for (const Ingredient &i : info.extra) if (i.tonnes > 0.0f) wanted[i.good] += q * i.tonnes;
                }
            }
        }
        for (int g = 0; g < GOOD_COUNT; g++)
            if (made[g] > 0.0 && wanted[g] > 0.0) base.perCap[g] = (float)ClampD(base.perCap[g] * SEED_SURPLUS * wanted[g] / made[g], 0.001, 1000.0);
    }
}

// What an economy's industries add up to, as they stand: what it makes and needs, the stock it wants of each good,
// and so what each is to it (made here, needed here, or passing through)
static void Derive(const EconBase &base, int i, MarketState &s)
{
    const MarketDef &d = base.defs[i];
    float demand[GOOD_COUNT] = {};
    // (people's wants follow their numbers the way a place's whole scale does)
    float people = d.pop > 0.0 ? (float)pow(fmax(s.pop, 1.0) / d.pop, 0.45) : 1.0f;
    for (int g = 0; g < GOOD_COUNT; g++) { s.out[g] = 0.0f; s.use[g] = d.use0[g] * people; }
    for (size_t k = 0; k < d.ind.size() && k < s.ind.size(); k++) {
        const IndustryDef &in = d.ind[k];
        double cap = s.ind[k].cap;
        if (cap <= 0.0) continue;
        const IndustryInfo &info = IndustryData(in.type);
        demand[REF_CELLS] += (float)(IsCivic(in.type) ? CivicCells(in.type, cap, in.site) : info.cells * cap);
        for (int p = 0; p < in.outs; p++) {
            int g = in.out[p].good;
            if (d.banned[g]) continue;
            float q = (float)(cap * in.out[p].share * base.perCap[g]);
            s.out[g] += q;
            for (const Ingredient &n : GoodData(g).recipe) if (n.tonnes > 0.0f) demand[n.good] += q * n.tonnes;
            for (const Ingredient &n : info.extra) if (n.tonnes > 0.0f) demand[n.good] += q * n.tonnes;
        }
    }
    for (int k = 0; k < 3; k++) demand[FUEL_GOODS[k]] += d.fleetFuel[k];  // (the ships that fill up here)
    for (size_t k = 0; k < d.ind.size() && k < s.ind.size(); k++)
        if (d.ind[k].type == IND_SHIPYARD && s.ind[k].cap > 0.0f) {
            float ship = s.ind[k].cap * base.yardYield;
            demand[GOOD_HULL_PLATING] += (float)YARD_PLATING * ship; demand[GOOD_SHIP_COMPONENTS] += (float)YARD_COMPONENTS * ship;
            demand[GOOD_REACTOR_CORES] += (float)YARD_CORES * ship; demand[GOOD_MACHINERY] += (float)YARD_MACHINERY * ship;
        }
    float hub = d.hub == i ? fmaxf(15.0f, 0.4f * d.traffic) : 0.0f;       // a hub keeps a little of everything its system handles
    // Everywhere keeps what it takes to build with: enough for the next few additions to its cities
    float build[GOOD_COUNT] = {};
    for (size_t k = 0; k < d.ind.size() && k < s.ind.size(); k++)
        if (d.ind[k].type == IND_CITY && s.ind[k].cap > 0.0f) {
            double people = 3.0 * (CityHousing(s.ind[k].cap * (1.0 + CITY_STEP)) - CityHousing(s.ind[k].cap)) / fmax((double)d.ind[k].site, 0.1);
            build[REF_STEEL] = (float)(CITY_STEEL * people); build[GOOD_MACHINERY] = (float)(CITY_MACHINERY * people); build[GOOD_HABITAT_MODULES] = (float)(CITY_MODULES * people);
        }
    for (int g = 0; g < GOOD_COUNT; g++) {
        float need = s.use[g] + demand[g];
        s.target[g] = d.banned[g] ? 0.0f : fmaxf(fmaxf(STOCK_DAYS * fmaxf(s.out[g], need), hub), build[g]);
        s.role[g] = s.target[g] <= 0.0f ? Role::None : s.out[g] > need * 1.05f ? Role::Export : need > s.out[g] * 1.05f ? Role::Import : Role::Trade;
        if (d.banned[g]) s.use[g] = s.out[g] = 0.0f;
    }
}

static std::vector<double> SettleStocks(EconBase &base, const FactionSet &fs, bool measure);

// Every economy in human space. (Slow: it builds every inhabited system. Done once per galaxy.)
static std::shared_ptr<const EconBase> BuildBase(const Galaxy &gal)
{
    static std::map<unsigned, std::shared_ptr<const EconBase>> known;
    auto it = known.find(gal.seed);
    if (it != known.end()) return it->second;

    auto base = std::make_shared<EconBase>();
    const FactionSet &fs = gal.factions;
    std::vector<const StarSystem *> all;
    gal.Nearby(gal.home, {}, FRONTIER_RADIUS_LY * 1.25 * LY_U, all);
    std::sort(all.begin(), all.end(), [](const StarSystem *a, const StarSystem *b) { return a->id < b->id; });
    std::vector<int> hubs;
    for (const StarSystem *s : all) {
        if (s->develop <= 0.0f) continue;
        LoadedSystem L;
        GenerateSystemContents(*s, L, true);
        AssignOwners(fs, *s, L);
        int first = (int)base->defs.size();
        std::vector<int> unitOf(L.bodies.size(), -1);
        std::vector<double> portScore;                             // (per economy of this system: how good a port it has)
        for (int bi = 0; bi < (int)L.bodies.size(); bi++) {
            const Body &b = L.bodies[bi];
            if (b.population <= 0) continue;
            // A station over a world that people live on is part of that world's economy
            int root = b.kind == Kind::Station && b.parent > 0 && L.bodies[b.parent].kind != Kind::Star && L.bodies[b.parent].population > 0 ? b.parent : bi;
            if (unitOf[root] < 0) {
                const Body &r = L.bodies[root];
                MarketDef d;
                d.sys = s->id; d.body = root; d.bodyId = r.id; d.name = r.name; d.settlement = r.settlement;
                d.ruler = s->ruler; d.owner = r.owner; d.station = r.kind == Kind::Station;
                d.ly = { (s->cx - fs.homeCell[0]) * CELL_LY, (s->cy - fs.homeCell[1]) * CELL_LY, (s->cz - fs.homeCell[2]) * CELL_LY };
                d.site = SiteOf(*s, L, r);
                unitOf[root] = (int)base->defs.size();
                base->defs.push_back(d);
                portScore.push_back(0.0);
            }
            MarketDef &d = base->defs[unitOf[root]];
            Settle(d, *s, L, b, fs.Get(b.owner));
            base->byBody[b.id] = unitOf[root];
            if (IsPort(b.settlement)) portScore[unitOf[root] - first] = 4.0;
        }
        // What the law bans, lawful places neither make nor use
        int hub = -1;
        double hubScore = 0.0;
        for (int i = first; i < (int)base->defs.size(); i++) {
            MarketDef &d = base->defs[i];
            const Faction *owner = fs.Get(d.owner), *ruler = fs.Get(d.ruler);
            if (ruler && owner && owner->kind != FactionKind::Clan)
                for (int g : ruler->contraband) { d.use0[g] = 0.0f; d.banned[g] = true; }
            FitOut(d);
            double score = d.scale * fmax(1.0, portScore[i - first]);      // ports first; failing one, the biggest place
            if (score > hubScore) { hubScore = score; hub = i; }
        }
        if (hub < 0) continue;
        hubs.push_back(hub);
        for (int i = first; i < (int)base->defs.size(); i++) { base->defs[i].hub = hub; base->defs[hub].traffic += base->defs[i].scale; }
    }
    // Lanes: each hub with its nearest few, within range
    for (size_t i = 0; i < hubs.size(); i++) {
        std::vector<std::pair<float, int>> near;
        for (size_t j = 0; j < hubs.size(); j++) {
            if (i == j) continue;
            float ly = (float)Len(base->defs[hubs[i]].ly - base->defs[hubs[j]].ly);
            if (ly <= LANE_RANGE_LY) near.push_back({ ly, hubs[j] });
        }
        std::sort(near.begin(), near.end());
        for (size_t k = 0; k < near.size() && k < 7; k++) {
            int a = std::min(hubs[i], near[k].second), b = std::max(hubs[i], near[k].second);
            bool have = false;
            for (const Lane &l : base->lanes) if (l.a == a && l.b == b) { have = true; break; }
            if (!have) base->lanes.push_back({ a, b, near[k].first });
        }
    }
    RouteToCapitals(*base, fs);
    // Fleets, in two goes: a generous guess, to see what trade there is to carry when ships and fuel are no object;
    // then the fleets that trade needs, the fuel those burn, and everything settled again with both to pay for
    SeedFleets(*base, fs, nullptr);
    SetYields(*base);
    // (round and round: the fuel the fleets burn is itself cargo, and takes tankers, which burn fuel)
    for (int round = 0; round < FLEET_ROUNDS; round++) {
        std::vector<double> carried = SettleStocks(*base, fs, true);
        SeedFleets(*base, fs, &carried);
        FleetNeeds(*base, carried);
        SetYields(*base);
    }
    SettleStocks(*base, fs, false);
    known[gal.seed] = base;
    return base;
}

// ---------------------------------------------------------------------------
// Prices and the daily round
// ---------------------------------------------------------------------------
// A deal moves the price as it goes: it's done in slices, each at the price the last one left
const int SLICES = 10;
double g_flow[5][GOOD_COUNT], g_flowDays, g_bunkered;

// Price against stock: at its target a good costs its base price; a glut can't push it under PRICE_FLOOR
// of that, and a market with none left pays PRICE_CAP times it
const double PRICE_FLOOR = 0.3, PRICE_CAP = 6.0;
const double YARD_GROW_ABOVE = 1.1;     // a yard is added to where shipping has been this busy: more cargo than ships
const double YARD_SPARE = 1.5;          // flat out, the yards between them can build this many times what the fleets they began with lose to wear
const double DEAL_SHARE = 0.5;          // how much more dealing there is in a good for each tonne a day of it the two places handle
const double BUNKER_RESERVE = 0.25;     // the share of the fuel a port wants in store that it won't sell to ships

static double PriceOf(const MarketState &s, int g, double pricing = 1.0)
{
    if (s.target[g] <= 0.0f) return 0.0;
    double have = fmax((double)s.stock[g], 0.02 * s.target[g]) / s.target[g];
    return GoodData(g).basePrice * ClampD(pow(have, -0.6), PRICE_FLOOR, PRICE_CAP) * pricing;
}

static double Pricing(const Economy &e, int faction, double clock)
{
    return faction >= 0 && faction < (int)e.pricing.size() && e.pricingUntil[faction] > clock ? e.pricing[faction] : 1.0;
}

// How hard the clans are hitting shipping at a place: 0 quiet .. 1 nothing gets through
static double RaidAt(const Economy &e, const FactionSet &fs, DVec3 ly)
{
    double worst = 0.0;
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Clan || f.id >= (int)e.raid.size()) continue;
        double near = 1.0 - Len(ly - f.seatLy) / f.reach;
        if (near > 0.0) worst = fmax(worst, e.raid[f.id] * fmin(1.0, near * 1.5));
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Shipping (fleet.h): what the ships based at a hub can carry, and what a run costs
// ---------------------------------------------------------------------------
const ShipDesign &FleetDesign(const EconBase &base, int faction, int role)
{
    size_t i = (size_t)(faction + 1) * ROLE_COUNT + role;
    return base.designs[i < base.designs.size() ? i : (size_t)role];
}

static int FuelSlot(int good) { return good == REF_FUEL ? 1 : good == RAW_HELIUM3 ? 2 : 0; }
// What goes by tanker
static bool Tanked(int g) { return g == REF_FUEL || g == REF_REACTOR_FUEL || g == RAW_HELIUM3 || g == REF_CELLS; }

bool GoesByTanker(int good) { return Tanked(good); }

bool Bunker(Economy &e, int market, int good, double tonnes)
{
    if (market < 0 || market >= e.Count() || good < 0 || good >= GOOD_COUNT) return false;
    MarketState &s = e.state[market];
    if (s.stock[good] - BUNKER_RESERVE * s.target[good] < tonnes) return false;
    s.stock[good] -= (float)tonnes;
    g_flow[3][good] += tonnes; g_bunkered += tonnes;
    return true;
}

// The ships of each role based at a hub, taken together: how many, and what the average one carries, takes and burns
struct Carrier {
    double ships[ROLE_COUNT] = {};
    double load[ROLE_COUNT] = {};                   // tonnes (liners: people) a ship
    double fixedSec[ROLE_COUNT] = {}, secPerLy[ROLE_COUNT] = {}, fixedFuel[ROLE_COUNT] = {}, fuelPerLy[ROLE_COUNT] = {};
    double mix[ROLE_COUNT][3] = {};                 // the share of that fuel that is each of the three kinds
    // What one ship of the role moves a day on a run of so many light years, and what each tonne (or head) of it burns
    double Daily(int role, double ly) const { double t = fixedSec[role] + secPerLy[role] * ly; return t > 0.0 ? load[role] * MARKET_DAY / t : 0.0; }
    double FuelPer(int role, double ly) const { return load[role] > 0.0 ? (fixedFuel[role] + fuelPerLy[role] * ly) / load[role] : 0.0; }
};

static Carrier CarrierAt(const Economy &e, const EconBase &base, int hub)
{
    Carrier c;
    if (hub < 0 || hub >= (int)e.fleets.size()) return c;
    for (const FleetAt &f : e.fleets[hub])
        for (int r = 0; r < ROLE_COUNT; r++) {
            double n = f.ships[r];
            if (n <= 0.0) continue;
            const ShipDesign &d = FleetDesign(base, f.owner, r);
            bool local = r == ROLE_HAULER;
            c.ships[r] += n;
            c.load[r] += n * (r == ROLE_LINER ? d.berths : d.cargo);
            c.fixedSec[r] += n * (local ? d.localSec : d.fixedSec); c.secPerLy[r] += n * (local ? 0.0 : d.secPerLy);
            c.fixedFuel[r] += n * (local ? d.localFuel : d.fixedFuel); c.fuelPerLy[r] += n * (local ? 0.0 : d.fuelPerLy);
            c.mix[r][FuelSlot(d.fuelGood)] += n * (local ? d.localFuel : d.fixedFuel);
        }
    for (int r = 0; r < ROLE_COUNT; r++) {
        if (c.ships[r] <= 0.0) continue;
        double fuel = c.mix[r][0] + c.mix[r][1] + c.mix[r][2];
        for (int k = 0; k < 3; k++) c.mix[r][k] = fuel > 0.0 ? c.mix[r][k] / fuel : (k == 0 ? 1.0 : 0.0);
        c.load[r] /= c.ships[r]; c.fixedSec[r] /= c.ships[r]; c.secPerLy[r] /= c.ships[r]; c.fixedFuel[r] /= c.ships[r]; c.fuelPerLy[r] /= c.ships[r];
    }
    return c;
}

// A run between two markets, as trade sees it: for dry goods [0] and tanked ones [1], what can be carried a day,
// and what carrying a tonne takes in fuel (by kind)
struct Haul {
    double vol = 0.0;                               // how much dealing there is between the two: what would move of a good for a given price gap
    double cap[2] = { 0.0, 0.0 };
    double fuelPer[2] = { 0.0, 0.0 };
    double mix[2][3] = { { 1, 0, 0 }, { 1, 0, 0 } };
    bool   burns = true;                            // the fuel comes out of the destination's stock (not where ships are flown: they buy their own)
    bool   limited = true;                          // ...and with none there, nothing moves (building a galaxy, it's found from somewhere: the fleets are sized by what would move)
};

// What it costs to land a tonne at a market from over that run: the fuel, at what fuel fetches there, and the rest of running a ship
static double HaulCost(const Haul &h, int cls, const MarketState &at, double pricing)
{
    double fuel = 0.0;
    for (int k = 0; k < 3; k++) {
        if (h.mix[cls][k] <= 0.0) continue;
        double p = PriceOf(at, FUEL_GOODS[k], pricing);
        fuel += h.mix[cls][k] * (p > 0.0 ? p : PRICE_CAP * GoodData(FUEL_GOODS[k]).basePrice);     // (not sold there: as dear as it gets)
    }
    return h.fuelPer[cls] * fuel * (1.0 + RUNNING_COSTS);
}

// Goods move from a to b (or back) wherever the price gap is worth the trip: more than the tariff, and what the
// ships charge to carry it. No more moves in a day than the ships can carry; and it takes fuel, at the far end.
// (prices as they stood at the start of the round: 'pa' and 'pb', a row of GOOD_COUNT each)
// 'ships': the goods go onto the dock for a ship (Economy::Ship) instead of straight into the other market's stock.
// 'load', if given, has added to it what there was to carry and what the ships could have, for dry goods and then tanked.
static void Trade(MarketState &sa, MarketState &sb, const Haul &h, double days,
                    double tariffAB, double tariffBA, const float *pricesA, const float *pricesB,
                    Economy *ships = nullptr, int ia = -1, int ib = -1, double clock = 0.0, double *moved = nullptr, double *load = nullptr, double *money = nullptr)
{
    double q[GOOD_COUNT], wanted[2] = { 0.0, 0.0 };
    bool   toB[GOOD_COUNT];
    const double costB[2] = { HaulCost(h, 0, sb, 1.0), HaulCost(h, 1, sb, 1.0) }, costA[2] = { HaulCost(h, 0, sa, 1.0), HaulCost(h, 1, sa, 1.0) };
    for (int g = 0; g < GOOD_COUNT; g++) {
        q[g] = 0.0;
        if (sa.target[g] <= 0.0f || sb.target[g] <= 0.0f) continue;
        double pa = pricesA[g], pb = pricesB[g];
        if (pa <= 0.0 || pb <= 0.0) continue;
        toB[g] = pb > pa;
        const int cls = Tanked(g) ? 1 : 0;
        MarketState &src = toB[g] ? sa : sb;
        double from = toB[g] ? pa : pb, to = toB[g] ? pb : pa;
        // (what's left of the price there after the tariff and the freight, against the price here)
        double gap = (to * (1.0 - (toB[g] ? tariffAB : tariffBA)) - (toB[g] ? costB[cls] : costA[cls])) / from - 1.0;
        if (gap <= 0.0) continue;
        double spare = src.stock[g] - 0.25 * src.target[g];                                 // nobody sells their last
        if (spare <= 0.0) continue;
        // (there's more dealing in what the two places handle a lot of)
        double deal = h.vol + DEAL_SHARE * (sa.target[g] + sb.target[g]) / STOCK_DAYS;
        q[g] = fmin(fmin(1.5 * deal * days * gap, spare), 0.3 * src.stock[g]);
        wanted[cls] += q[g];
    }
    // No more than the ships can carry
    double scale[2] = { 1.0, 1.0 };
    for (int cls = 0; cls < 2; cls++) {
        double can = h.cap[cls] * days;
        if (wanted[cls] > can) scale[cls] = can > 0.0 ? can / wanted[cls] : 0.0;
        if (load) { load[2 * cls] += wanted[cls]; load[2 * cls + 1] += can; }
    }
    // ...or than there's fuel for: each way separately. Ships fill up where they unload if they can, and where they
    // loaded if they can't (or nothing would ever reach a port that had run dry, the fuel it's waiting for least of all).
    if (h.burns && !ships)
        for (int way = 0; way < 2; way++) {
            MarketState &at = way == 0 ? sb : sa, &back = way == 0 ? sa : sb;
            double need[3] = { 0.0, 0.0, 0.0 };
            for (int g = 0; g < GOOD_COUNT; g++) {
                if (q[g] <= 0.0 || toB[g] != (way == 0)) continue;
                const int cls = Tanked(g) ? 1 : 0;
                for (int k = 0; k < 3; k++) need[k] += q[g] * scale[cls] * h.fuelPer[cls] * h.mix[cls][k];
            }
            // (a port keeps back what its own power plants run on: ships get what's over)
            double fuelled = 1.0, there[3], behind[3];
            for (int k = 0; k < 3; k++) {
                const int f = FUEL_GOODS[k];
                there[k] = fmax(0.0, (double)at.stock[f] - (h.limited ? BUNKER_RESERVE * at.target[f] : 0.0));
                behind[k] = fmax(0.0, (double)back.stock[f] - (h.limited ? BUNKER_RESERVE * back.target[f] : 0.0));
                if (need[k] > 0.0 && h.limited) fuelled = fmin(fuelled, (there[k] + behind[k]) / need[k]);
            }
            for (int k = 0; k < 3; k++) {
                double want = need[k] * fuelled, here = fmin(want, there[k]);
                g_flow[3][FUEL_GOODS[k]] += want; g_flow[4][FUEL_GOODS[k]] += need[k] - want;
                at.stock[FUEL_GOODS[k]] = fmaxf(0.0f, at.stock[FUEL_GOODS[k]] - (float)here);
                back.stock[FUEL_GOODS[k]] = fmaxf(0.0f, back.stock[FUEL_GOODS[k]] - (float)(want - here));
            }
            if (fuelled < 1.0) for (int g = 0; g < GOOD_COUNT; g++) if (q[g] > 0.0 && toB[g] == (way == 0)) q[g] *= fuelled;
        }
    for (int g = 0; g < GOOD_COUNT; g++) {
        double move = q[g] * scale[Tanked(g) ? 1 : 0];
        if (move <= 0.0) continue;
        MarketState &src = toB[g] ? sa : sb, &dst = toB[g] ? sb : sa;
        move = fmin(move, (double)src.stock[g]);
        src.stock[g] -= (float)move;
        if (moved) moved[Tanked(g) ? 1 : 0] += move;
        if (money) {            // credits: the tariff on what went into b, and into a; the carriers' cut, dry and tanked; and what it was all worth
            const int cls = Tanked(g) ? 1 : 0;
            double price = toB[g] ? pricesB[g] : pricesA[g];
            money[toB[g] ? 0 : 1] += move * price * (toB[g] ? tariffAB : tariffBA);
            money[2 + cls] += move * (toB[g] ? costB[cls] : costA[cls]) * RUNNING_COSTS / (1.0 + RUNNING_COSTS);
            money[4] += move * price;
        }
        if (ships) ships->Ship(toB[g] ? ia : ib, toB[g] ? ib : ia, g, (float)move, clock);
        else       dst.stock[g] += (float)move;
    }
}

// What a lane can carry: the ships each end has put on it (bulk freighters for most things, tankers for fuel)
static Haul LaneHaul(const Lane &l, const Carrier &a, const Carrier &b, double raids)
{
    Haul h;
    h.vol = l.vol * (1.0 - 0.85 * raids);
    const int roles[2] = { ROLE_BULK, ROLE_TANKER };
    for (int cls = 0; cls < 2; cls++) {
        const int r = roles[cls];
        double na = a.ships[r] * l.shareA, nb = b.ships[r] * l.shareB, ca = na * a.Daily(r, l.ly), cb = nb * b.Daily(r, l.ly);
        h.cap[cls] = (ca + cb) * (1.0 - 0.85 * raids);
        double all = ca + cb;
        h.fuelPer[cls] = all > 0.0 ? (ca * a.FuelPer(r, l.ly) + cb * b.FuelPer(r, l.ly)) / all : 0.0;
        for (int k = 0; k < 3; k++) h.mix[cls][k] = all > 0.0 ? (ca * a.mix[r][k] + cb * b.mix[r][k]) / all : (k == 0 ? 1.0 : 0.0);
    }
    return h;
}

// ...and between a hub and one of its system's markets: the hub's local haulers
static Haul LocalHaul(const MarketDef &d, const Carrier &hub)
{
    Haul h;
    h.vol = 2.0 * d.scale;
    double cap = hub.ships[ROLE_HAULER] * d.haulShare * hub.Daily(ROLE_HAULER, 0.0);
    for (int cls = 0; cls < 2; cls++) {
        h.cap[cls] = cap; h.fuelPer[cls] = hub.FuelPer(ROLE_HAULER, 0.0);
        for (int k = 0; k < 3; k++) h.mix[cls][k] = hub.mix[ROLE_HAULER][k];
    }
    return h;
}

// The fleets a new galaxy starts with: at every hub, enough ships of each kind for the trade it does (and a little
// over), shared out among whoever is there to own them
// ('carried': tonnes a day found to move, for each lane dry then tanked, then each market's local run; or null, for a first guess from the size of the places)
static void SeedFleets(EconBase &base, const FactionSet &fs, const std::vector<double> *carried)
{
    const int N = (int)base.defs.size(), F = fs.Count();
    const size_t L = base.lanes.size();
    base.designs.clear();
    for (int f = NO_FACTION; f < F; f++)
        for (int r = 0; r < ROLE_COUNT; r++) base.designs.push_back(DesignShip(fs, f, r));
    base.fleets0.assign(N, {});
    std::vector<double> need[ROLE_COUNT];
    for (auto &v : need) v.assign(N, 0.0);
    const ShipDesign &bulk = FleetDesign(base, NO_FACTION, ROLE_BULK), &tank = FleetDesign(base, NO_FACTION, ROLE_TANKER),
                     &liner = FleetDesign(base, NO_FACTION, ROLE_LINER), &haul = FleetDesign(base, NO_FACTION, ROLE_HAULER);
    // (counted in ships of the plainest design: an owner with bigger ones has fewer, below)
    for (size_t li = 0; li < L; li++) {
        Lane &l = base.lanes[li];
        double guess = 0.5 * sqrt((double)base.defs[l.a].traffic * base.defs[l.b].traffic) * 1.5 / (1.0 + l.ly / 10.0);   // tonnes a day the lane might carry
        l.vol = (float)(2.0 * guess);
        double trade = carried ? fmax((*carried)[2 * li], 0.02 * guess) : 8.0 * guess, fuels = carried ? fmax((*carried)[2 * li + 1], 0.005 * guess) : 2.0 * guess;
        double perBulk = bulk.cargo * MARKET_DAY / bulk.Seconds(l.ly), perTank = tank.cargo * MARKET_DAY / tank.Seconds(l.ly),
               perLiner = liner.berths * MARKET_DAY / liner.Seconds(l.ly);
        l.shareA = (float)(0.5 * trade / perBulk); l.shareB = l.shareA;        // (for now: each end's ships on this lane; made a share below)
        for (int end : { l.a, l.b }) {
            need[ROLE_BULK][end] += 0.5 * FLEET_SPARE * trade / perBulk;
            need[ROLE_TANKER][end] += 0.5 * FLEET_SPARE * fuels / perTank;
            need[ROLE_LINER][end] += 0.5 * FLEET_SPARE * 0.3 * guess * PEOPLE_PER_TONNE / perLiner;
        }
    }
    std::vector<double> onLanes(N, 0.0);
    for (const Lane &l : base.lanes) { onLanes[l.a] += l.shareA; onLanes[l.b] += l.shareB; }
    for (Lane &l : base.lanes) { l.shareA = onLanes[l.a] > 0.0 ? (float)(l.shareA / onLanes[l.a]) : 0.0f; l.shareB = onLanes[l.b] > 0.0 ? (float)(l.shareB / onLanes[l.b]) : 0.0f; }
    std::vector<double> local(N, 0.0);
    for (int i = 0; i < N; i++) {
        MarketDef &d = base.defs[i];
        if (d.hub < 0 || d.hub == i) continue;
        d.haulShare = (float)(carried ? fmax((*carried)[2 * L + i], 0.05 * d.scale) : 8.0 * d.scale);     // (tonnes a day; made a share below)
        local[d.hub] += d.haulShare;
    }
    for (int i = 0; i < N; i++) {
        MarketDef &d = base.defs[i];
        if (d.hub < 0 || d.hub == i) continue;
        need[ROLE_HAULER][d.hub] = FLEET_SPARE * local[d.hub] / (haul.cargo * MARKET_DAY / haul.localSec);
        d.haulShare = local[d.hub] > 0.0 ? (float)(d.haulShare / local[d.hub]) : 0.0f;
    }
    // Who owns them: the government whose space it is, the companies with works in the system, and independent owners
    for (int hub = 0; hub < N; hub++) {
        if (base.defs[hub].hub != hub) continue;
        std::vector<std::pair<int, double>> owners;
        auto add = [&](int who, double weight) { for (auto &o : owners) if (o.first == who) { o.second += weight; return; } owners.push_back({ who, weight }); };
        double all = 0.0;
        for (int i = 0; i < N; i++) {
            if (base.defs[i].hub != hub) continue;
            for (const IndustryDef &in : base.defs[i].ind) {
                const Faction *f = fs.Get(in.owner);
                if (in.cap <= 0.0f || IsCivic(in.type) || !f || f->kind == FactionKind::Guild) continue;
                add(in.owner, in.cap); all += in.cap;
            }
        }
        if (all <= 0.0) all = 1.0;
        for (auto &o : owners) o.second *= 0.75 / all;
        add(NO_FACTION, owners.empty() ? 1.0 : 0.25);
        for (auto &o : owners) {
            FleetAt f;
            f.owner = (int16_t)o.first;
            for (int r = 0; r < ROLE_COUNT; r++) {
                const ShipDesign &plain = FleetDesign(base, NO_FACTION, r), &own = FleetDesign(base, o.first, r);
                double a = r == ROLE_LINER ? plain.berths : plain.cargo, b = r == ROLE_LINER ? own.berths : own.cargo;
                double ta = r == ROLE_HAULER ? plain.localSec : plain.Seconds(5.0), tb = r == ROLE_HAULER ? own.localSec : own.Seconds(5.0);
                f.ships[r] = (float)(need[r][hub] * o.second * (b > 0.0 && ta > 0.0 ? (a / ta) / (b / tb) : 1.0));
            }
            base.fleets0[hub].push_back(f);
        }
    }
}

// The fuel that carrying what was found to move ('carried', as for SeedFleets) burns at each market a day (so that
// it's made, and kept in store); and how much ship a unit of yard has to turn out for the yards between them to keep the fleets up
static void FleetNeeds(EconBase &base, const std::vector<double> &carried)
{
    Economy e;
    e.base = std::shared_ptr<const EconBase>(&base, [](const EconBase *) {});
    e.fleets = base.fleets0;
    const int N = (int)base.defs.size();
    std::vector<Carrier> at(N);
    for (int i = 0; i < N; i++) { for (float &f : base.defs[i].fleetFuel) f = 0.0f; if (base.defs[i].hub == i) at[i] = CarrierAt(e, base, i); }
    const size_t L = base.lanes.size();
    for (size_t li = 0; li < L; li++) {
        const Lane &l = base.lanes[li];
        Haul h = LaneHaul(l, at[l.a], at[l.b], 0.0);
        // (half of what a run burns is drawn at each end)
        for (int cls = 0; cls < 2; cls++)
            for (int k = 0; k < 3; k++) {
                float fuel = (float)(0.5 * carried[2 * li + cls] * h.fuelPer[cls] * h.mix[cls][k]);
                base.defs[l.a].fleetFuel[k] += fuel; base.defs[l.b].fleetFuel[k] += fuel;
            }
    }
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        if (d.hub < 0 || d.hub == i) continue;
        double tonnes = carried[2 * L + i];
        for (int k = 0; k < 3; k++) {
            float fuel = (float)(0.5 * tonnes * at[d.hub].FuelPer(ROLE_HAULER, 0.0) * at[d.hub].mix[ROLE_HAULER][k]);
            base.defs[i].fleetFuel[k] += fuel; base.defs[d.hub].fleetFuel[k] += fuel;
        }
    }
    double wear = 0.0, yards = 0.0;
    for (int i = 0; i < N; i++) {
        for (const FleetAt &f : base.fleets0[i]) for (int r = 0; r < ROLE_COUNT; r++) wear += f.ships[r] * FleetDesign(base, f.owner, r).tonnage / SHIP_LIFE_DAYS;
        for (const IndustryDef &in : base.defs[i].ind) if (in.type == IND_SHIPYARD) yards += in.cap;
    }
    base.yardYield = yards > 0.0 ? (float)(YARD_SPARE * wear / yards) : 0.0f;
}

// ---------------------------------------------------------------------------
// Money (econ.h)
// ---------------------------------------------------------------------------
const double PROFIT_TAX     = 0.12;     // the share of a works' profit a government takes, if it isn't the government's own...
const double PLANNERS_TAX   = 0.10;     // ...and how much more a government that plans its economy takes
const double DEVELOP_DAYS   = 0.25;     // how often owners look at what to build
const double HEAD_TAX       = 0.0005;   // credits a head a day, where people live as well as they can (less where they don't)
const double TAX_MOST       = 2.0, TAX_LEAST = 0.6;        // how far a government will squeeze, or ease, against what its politics say
const double TAX_STEP       = 0.05;
const double TAX_LOOK_DAYS  = 2.0;      // how often it thinks about it
const double TAX_SHORT      = 0.10;     // it squeezes when what it couldn't afford comes to this share of its income...
const double TAX_FLUSH_DAYS = 40.0;     // ...and eases when it has this many days' spending put by
const double CROWD_FROM     = 0.5;      // a place starts to feel full with this share of the people it can hold
const double LIVING_CROWD   = 0.25;     // ...and what being quite full takes off its living standard: enough that people leave
const double BUILT_UP_POWER = 1.5;      // adding to a world's industry costs more by how much it has grown already, to this power
const double LIVING_TAX     = 0.06;     // what being taxed twice as hard takes off a world's living standard
const double COLLECT_DAYS   = 0.5;      // how long a hop along the lanes takes money on its way home
const int    SAIL_EVERY     = 6;        // a market's coffer is sent on its way every so many of those: every three days
const double COFFER_LEAST   = 2000.0;   // ...if there's this much in it
const double CONVOY_LEAST   = 1.0e6;    // money passing through a hub goes straight on if it comes to this; less waits for the hub's own sailing
const double SPECIE_RISK    = 0.15;     // the share of money on the move that raiders at their worst take, a hop
const double FENCE_SHARE    = 0.4;      // what stolen goods and money are worth to a clan once they've been turned into something spendable
const double LOOT_SHARE     = 0.10;     // the share of the trade through raided space that raiders at their worst make off with
const double YARD_MARGIN    = 0.3;      // the share of a ship's price that is its builder's to keep
const double OWN_BUILD      = 0.3;      // an owner adding to its own works pays this share of the fee a captain would, and the materials
const double START_GOVERNMENT = 2.0e8, START_COMPANY = 4.0e7, START_CLAN = 5.0e6, START_OTHER = 2.0e7;       // what a faction has put by when a galaxy begins
const double UPKEEP_MEMORY  = 20.0;     // days: how slowly a faction's commitments follow its income
const double OVERHEAD_GOVERNMENT = 0.90, OVERHEAD_COMPANY = 0.80, OVERHEAD_CLAN = 0.70, OVERHEAD_OTHER = 0.85;
const double BOOKS_DAYS     = 5.0;      // the memory of the income and spending figures

const char *IncomeName(int kind)
{
    static const char *names[] = { "tariffs", "tax on profits", "tax on its people", "its works' profit", "freight", "building ships", "loot" };
    return names[kind >= 0 && kind < INC_COUNT ? kind : 0];
}

static bool Counted(const Economy &e, int faction) { return faction >= 0 && faction < (int)e.money.size(); }

double TariffNow(const Economy &e, int ruler)
{
    return ruler >= 0 && ruler < (int)e.tariff.size() ? fmin(0.5, (double)e.tariff[ruler] * (Counted(e, ruler) ? e.money[ruler].squeeze : 1.0f)) : 0.0;
}

static double ProfitTaxNow(const Economy &e, int ruler) { return Counted(e, ruler) ? fmin(0.5, (double)e.money[ruler].profitTax * e.money[ruler].squeeze) : 0.0; }

// Money earned at a market: into its owner's coffer there (or straight into the treasury, if that's where the capital is)
static void Pay(Economy &e, int market, int faction, double credits, int kind)
{
    if (!e.paying || !Counted(e, faction) || !(credits > 0.0)) return;
    const size_t F = e.money.size();
    FactionMoney &m = e.money[faction];
    m.incomeNow[kind] += credits;
    int capital = faction < (int)e.base->capital.size() ? e.base->capital[faction] : -1;
    if (capital < 0 || market == capital || market < 0 || ((size_t)market + 1) * F > e.coffer.size()) m.treasury += credits;
    else e.coffer[(size_t)market * F + faction] += credits;
}

// ...and lost there: out of what's waiting, as far as that goes
static void Lose(Economy &e, int market, int faction, double credits)
{
    if (!e.paying || !Counted(e, faction) || !(credits > 0.0)) return;
    const size_t F = e.money.size();
    int capital = faction < (int)e.base->capital.size() ? e.base->capital[faction] : -1;
    double &from = capital < 0 || market == capital || ((size_t)market + 1) * F > e.coffer.size() ? e.money[faction].treasury : e.coffer[(size_t)market * F + faction];
    from = fmax(0.0, from - credits);
}

// Has it the money at its capital? (Owners nobody keeps books for always have.) Spends it if so; notes the want if not.
// ('asked': how much of a day's asking this is, for something that's asked for again and again until it can be paid for)
static bool Spend(Economy &e, int faction, double credits, double asked = 1.0)
{
    if (!e.paying || !Counted(e, faction)) return true;
    FactionMoney &m = e.money[faction];
    if (m.treasury < credits) { m.deniedNow += credits * asked; return false; }
    m.treasury -= credits; m.spentNow += credits;
    return true;
}

// The ships of a role based at two hubs (or one) share what was paid to carry something between them
static void PayFleets(Economy &e, int hubA, int hubB, int role, double credits)
{
    if (!e.paying || !(credits > 0.0)) return;
    double all = 0.0;
    for (int hub : { hubA, hubB }) if (hub >= 0 && hub < (int)e.fleets.size()) for (const FleetAt &f : e.fleets[hub]) all += f.ships[role];
    if (all <= 0.0) return;
    for (int hub : { hubA, hubB }) if (hub >= 0 && hub < (int)e.fleets.size()) for (const FleetAt &f : e.fleets[hub]) Pay(e, hub, f.owner, credits * f.ships[role] / all, INC_FREIGHT);
}

static int ClanAt(const FactionSet &fs, DVec3 ly)
{
    int clan = NO_FACTION; double best = 1e30;
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) { double d = Len(ly - f.seatLy); if (d < best) { best = d; clan = f.id; } }
    return clan;
}

// What raiders make off with is theirs, for what they can get for it
static void Loot(Economy &e, const FactionSet &fs, DVec3 ly, double credits)
{
    int clan = ClanAt(fs, ly);
    if (!e.paying || !Counted(e, clan) || !(credits > 0.0)) return;
    e.money[clan].treasury += FENCE_SHARE * credits; e.money[clan].incomeNow[INC_LOOT] += FENCE_SHARE * credits;
}

// The way home: for every faction with a seat, the next hub along the lanes from each hub to its capital
static void RouteToCapitals(EconBase &base, const FactionSet &fs)
{
    const int N = (int)base.defs.size(), F = fs.Count();
    base.capital.assign(F, -1);
    base.toCapital.assign((size_t)F * N, -1);
    std::vector<std::vector<int>> next(N);
    for (const Lane &l : base.lanes) { next[l.a].push_back(l.b); next[l.b].push_back(l.a); }
    for (const Faction &f : fs.list) {
        for (int i = 0; i < N; i++) if (base.defs[i].hub == i && base.defs[i].sys == f.seat) base.capital[f.id] = i;
        int cap = base.capital[f.id];
        if (cap < 0) continue;
        int *to = &base.toCapital[(size_t)f.id * N];
        std::vector<int> queue = { cap };
        std::vector<char> seen(N, 0);
        seen[cap] = 1;
        for (size_t q = 0; q < queue.size(); q++)
            for (int j : next[queue[q]]) if (!seen[j]) { seen[j] = 1; to[j] = queue[q]; queue.push_back(j); }
    }
}

// Money goes home: from a system's markets to its hub, from a hub to the next one toward the capital, a hop each time.
// A coffer waits for its market's sailing day (they don't all sail at once); what's already on the move goes straight
// on, so what leaves a hub is everything of its owner's that has gathered there. Raiders along the way take their share.
static void Collect(Economy &e, const EconBase &base, const FactionSet &fs)
{
    const int N = (int)base.defs.size();
    const size_t F = e.money.size();
    if (!F || e.coffer.size() != (size_t)N * F) return;
    if (e.moving.size() != e.coffer.size()) e.moving.assign(e.coffer.size(), 0.0);
    const int tick = e.sailings++;
    static std::vector<double> arriving;
    arriving.assign(e.coffer.size(), 0.0);
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        double raids = -1.0;
        const bool sails = (tick + i) % SAIL_EVERY == 0;
        for (size_t f = 0; f < F; f++) {
            double &waiting = e.coffer[(size_t)i * F + f], &here = e.moving[(size_t)i * F + f];
            if (sails && waiting >= COFFER_LEAST) { here += waiting; waiting = 0.0; }
            // (a little that has come in from elsewhere waits for this place's own sailing; a convoy's worth goes straight on)
            if (here <= 0.0 || (!sails && here < CONVOY_LEAST)) continue;
            int capital = base.capital[f], to = d.hub != i ? d.hub : base.toCapital[f * N + i];
            if (i == capital || capital < 0 || to < 0) { e.money[f].treasury += here; here = 0.0; continue; }      // (home; or no way there by the lanes: it's wired)
            if (e.zone.size() == (size_t)N && (e.zone[i] || e.zone[to])) {      // (where ships are flown, a paymaster's ship carries it)
                Specie *run = nullptr;
                for (Specie &s : e.specie) if (s.from == i && s.to == to && s.faction == (int)f) run = &s;
                if (!run) { e.specie.push_back(Specie()); run = &e.specie.back(); run->from = i; run->to = to; run->faction = (int)f; }
                run->credits += here; here = 0.0;
                continue;
            }
            if (raids < 0.0) raids = RaidAt(e, fs, d.ly);
            double lost = here * SPECIE_RISK * raids;
            if (lost > 0.0) { int clan = ClanAt(fs, d.ly); if (clan != (int)f) Loot(e, fs, d.ly, lost); else lost = 0.0; }
            if (to == capital) e.money[f].treasury += here - lost; else arriving[(size_t)to * F + f] += here - lost;
            here = 0.0;
        }
    }
    for (size_t k = 0; k < e.moving.size(); k++) e.moving[k] += arriving[k];
}

void LandSpecie(Economy &e, int market, int faction, double credits)
{
    if (!Counted(e, faction) || !(credits > 0.0)) return;
    const size_t F = e.money.size();
    int capital = faction < (int)e.base->capital.size() ? e.base->capital[faction] : -1;
    if (capital < 0 || market == capital || market < 0 || ((size_t)market + 1) * F > e.moving.size()) e.money[faction].treasury += credits;
    else e.moving[(size_t)market * F + faction] += credits;       // (it goes on from here at the next hop)
}

void FenceLoot(Economy &e, int clan, double credits)
{
    if (!Counted(e, clan) || !(credits > 0.0)) return;
    e.money[clan].treasury += FENCE_SHARE * credits; e.money[clan].incomeNow[INC_LOOT] += FENCE_SHARE * credits;
}

// The books: what each faction has been earning and spending a day, lately
static void MakeUpBooks(Economy &e, double days)
{
    const double fresh = 1.0 - exp(-days / BOOKS_DAYS), slow = 1.0 - exp(-days / UPKEEP_MEMORY);
    for (FactionMoney &m : e.money) {
        // Its overhead: so much of what it's used to earning, out of what has reached the capital. What it can't pay is a want like any other.
        double earned = 0.0;
        for (double k : m.incomeNow) earned += k;
        m.usual += (float)((earned / days - m.usual) * slow);
        double due = m.overhead * m.usual * days, paid = fmin(m.treasury, due);
        m.treasury -= paid; m.deniedNow += due - paid;
        m.upkeep += (float)((paid / days - m.upkeep) * fresh);
        for (int k = 0; k < INC_COUNT; k++) { m.income[k] += (float)((m.incomeNow[k] / days - m.income[k]) * fresh); m.incomeNow[k] = 0.0; }
        m.spent += (float)((m.spentNow / days - m.spent) * fresh); m.spentNow = 0.0;
        m.denied += (float)((m.deniedNow / days - m.denied) * fresh); m.deniedNow = 0.0;
    }
}

// Governments look at their taxes: harder when they keep finding they can't pay for what they want, easier when there's plenty put by
static void AdjustTaxes(Economy &e, const FactionSet &fs)
{
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Government || !Counted(e, f.id)) continue;
        FactionMoney &m = e.money[f.id];
        double income = 0.0;
        for (float k : m.income) income += k;
        const double outgo = (double)m.spent + m.upkeep;
        if (m.denied > TAX_SHORT * (income + 1.0)) m.squeeze = (float)fmin(TAX_MOST, m.squeeze + TAX_STEP);
        else if (m.treasury > TAX_FLUSH_DAYS * fmax(outgo, 0.2 * income)) m.squeeze = (float)fmax(TAX_LEAST, m.squeeze - TAX_STEP);
        else if (m.squeeze < 1.0f) m.squeeze = (float)fmin(1.0, m.squeeze + TAX_STEP);          // (neither: back toward what its politics say)
        else if (m.squeeze > 1.0f && m.denied <= 0.0f) m.squeeze = (float)fmax(1.0, m.squeeze - TAX_STEP);
    }
}

// Fleets from day to day: ships wear out; raiders take some, and keep them; the yards' new ships go where shipping is
// shortest; and ships move along the lanes from where there's little for them to do to where there's plenty
static void Shipping(Economy &e, const EconBase &base, const FactionSet &fs, double days)
{
    const int N = (int)base.defs.size();
    const float aged = (float)exp(-days / SHIP_LIFE_DAYS);
    for (int hub = 0; hub < N; hub++) {
        if (e.fleets[hub].empty()) continue;
        double raids = RaidAt(e, fs, base.defs[hub].ly);
        int clan = NO_FACTION;
        if (raids > 0.0) {
            double best = 1e30;
            for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) { double d = Len(base.defs[hub].ly - f.seatLy); if (d < best) { best = d; clan = f.id; } }
        }
        float taken = (float)(RAID_SHIP_LOSS * raids * days);
        for (FleetAt &f : e.fleets[hub])
            for (int r = 0; r < ROLE_COUNT; r++) {
                if (clan >= 0 && clan < (int)e.clanShips.size() && f.owner != clan) { e.clanShips[clan] += f.ships[r] * taken; f.ships[r] *= 1.0f - taken; }
                f.ships[r] *= aged;
            }
    }
    for (float &s : e.clanShips) s *= aged;
    // New ships, from the slips: to the role that's busiest here, and the owner who has most of it
    for (int i = 0; i < N; i++) {
        if (e.slips[i] <= 0.0f) continue;
        int hub = base.defs[i].hub;
        if (hub < 0 || e.fleets[hub].empty()) { e.slips[i] = 0.0f; continue; }
        int role = 0;
        for (int r = 1; r < ROLE_COUNT; r++) if (e.busy[(size_t)hub * ROLE_COUNT + r] > e.busy[(size_t)hub * ROLE_COUNT + role]) role = r;
        // ...who can pay for them (a ship costs what its design does, by the tonne): the one with most is asked first, and
        // its going without is noted. Failing everyone, independent owners take them, whose money is nobody's to count.
        std::vector<FleetAt> &here = e.fleets[hub];
        auto price = [&](int owner) { const ShipDesign &d = FleetDesign(base, owner, role); return d.tonnage > 0.0 ? e.slips[i] * d.value / d.tonnage : 0.0; };
        FleetAt *to = nullptr, *first = nullptr;
        for (FleetAt &f : here) if (!first || f.ships[role] > first->ships[role]) first = &f;
        if (first && Spend(e, first->owner, price(first->owner))) to = first;
        if (!to) {
            for (FleetAt &f : here)
                if (&f != first && (!to || f.ships[role] > to->ships[role]) && (!Counted(e, f.owner) || e.money[f.owner].treasury >= price(f.owner))) to = &f;
            if (to && !Spend(e, to->owner, price(to->owner))) to = nullptr;
        }
        if (!to) {
            for (FleetAt &f : here) if (f.owner == NO_FACTION) to = &f;
            if (!to) { FleetAt n; n.owner = NO_FACTION; here.push_back(n); to = &here.back(); }
        }
        int builder = NO_FACTION;                                  // (its builder is paid, whoever took it)
        for (size_t k = 0; k < base.defs[i].ind.size() && k < e.state[i].ind.size(); k++) if (base.defs[i].ind[k].type == IND_SHIPYARD) builder = e.state[i].ind[k].owner;
        Pay(e, i, builder, YARD_MARGIN * price(to->owner), INC_YARDS);
        double each = FleetDesign(base, to->owner, role).tonnage;
        if (each > 0.0) to->ships[role] += (float)(e.slips[i] / each);
        e.slips[i] = 0.0f;
    }
    // Ships follow the work: along each lane, from the end with less for them to do to the end with more
    for (const Lane &l : base.lanes)
        for (int r : { ROLE_BULK, ROLE_TANKER, ROLE_LINER }) {
            float ba = e.busy[(size_t)l.a * ROLE_COUNT + r], bb = e.busy[(size_t)l.b * ROLE_COUNT + r];
            if (fabsf(ba - bb) < 0.1f) continue;
            int from = ba < bb ? l.a : l.b, to = ba < bb ? l.b : l.a;
            float share = (float)fmin(0.5, FLEET_REBALANCE * fabsf(ba - bb) * days);
            for (FleetAt &f : e.fleets[from]) {
                float go = f.ships[r] * share;
                if (go <= 0.0f) continue;
                FleetAt *home = nullptr;
                for (FleetAt &t : e.fleets[to]) if (t.owner == f.owner) home = &t;
                if (!home) { FleetAt n; n.owner = f.owner; e.fleets[to].push_back(n); home = &e.fleets[to].back(); }
                f.ships[r] -= go; home->ships[r] += go;
            }
        }
}

// Who goes short first, when there isn't the labour or the power for everything: lowest of these. Power, housing
// and food before all; then what the owner cares most about; then what pays.
static double Priority(int type, const Faction *owner, double worth)
{
    if (type == IND_CITY) return 1e13;
    if (IsPower(type)) return 1e12;
    if (type == IND_SERVICES) return 0.5;                          // (services take what hands are left over)
    if (type == IND_FARM || type == IND_AGRIDOME || type == IND_HYDROPONICS || type == IND_FOOD) return 1e10;
    double favour = 1.0;
    if (owner && owner->kind == FactionKind::Government) {
        // A state that reaches for force keeps its arms running; one that plans keeps its heavy industry
        if (type == IND_ARMS || type == IND_MUNITIONS || type == IND_REACTOR) favour = 1.0 + 6.0 * fmax(0.0, (double)owner->axis[AX_FORCE]);
        if (type == IND_HEAVY || type == IND_SMELTER) favour = 1.0 + 2.0 * fmax(0.0, (double)owner->axis[AX_MARKETS]);
    } else if (owner && owner->kind == FactionKind::Conglomerate) {
        bool line = false;
        switch (owner->slant) {
        case Slant::Industry:    line = type == IND_HEAVY || type == IND_SMELTER || type == IND_FOUNDRY; break;
        case Slant::Mining:      line = type == IND_MINE || type == IND_ICEWORKS || type == IND_SKIMMER; break;
        case Slant::Energy:      line = type == IND_REACTOR || type == IND_CHEMICAL; break;
        case Slant::Agriculture: line = type == IND_XENO || type == IND_PHARMA; break;
        case Slant::Shipping:    line = type == IND_PRECISION || type == IND_ROBOTICS; break;
        case Slant::Finance:     line = type == IND_LIGHT || type == IND_LAB; break;
        default: break;
        }
        if (line) favour = 8.0;
    } else if (owner && owner->kind == FactionKind::Clan) {
        if (type == IND_ARMS || type == IND_MUNITIONS) favour = 8.0;
    }
    return fmax(worth, 1.0) * favour;
}

static const double USUAL_RAID = 0.3;       // what clans get up to when nobody's paying or chasing them

// One economy's day (or 'days' of one): its people use what they use, and its industries run as far as its labour,
// its power and their materials allow
static void Work(Economy &e, const EconBase &base, const FactionSet &fs, int i, double days, float supply, float demand, double clock, bool flown)
{
    const MarketDef &d = base.defs[i];
    MarketState &s = e.state[i];
    auto boost = [&](int faction) { return faction >= 0 && faction < (int)e.output.size() && e.outputUntil[faction] > clock ? e.output[faction] : 1.0f; };
    const float wealth = (float)ClampD(1.0 + WEALTH_SLOPE * (s.living - WEALTH_AT), WEALTH_MIN, WEALTH_MAX);
    // (its government taxes its people: more where they're doing well)
    if (e.paying && Counted(e, d.ruler)) Pay(e, i, d.ruler, s.pop * e.money[d.ruler].headTax * e.money[d.ruler].squeeze * s.living * days, INC_HEADS);
    for (int g = 0; g < GOOD_COUNT; g++)
        if (s.use[g] > 0.0f && s.target[g] > 0.0f) {
            // (what's scarce gets rationed: people make do with less of it)
            float ration = fminf(1.0f, 0.3f + 0.7f * s.stock[g] / s.target[g]);
            bool comfort = g == GOOD_CONSUMER || g == GOOD_LUXURY || g == GOOD_LIQUOR || g == GOOD_MEDICINES || g == GOOD_ELECTRONICS;
            g_flow[2][g] += fminf(s.stock[g], s.use[g] * demand * ration * (comfort ? wealth : 1.0f) * (float)days);
            s.stock[g] = fmaxf(0.0f, s.stock[g] - s.use[g] * demand * ration * (comfort ? wealth : 1.0f) * (float)days);
        }
    const double pricing = Pricing(e, s.owner, clock);
    // In order of who comes first
    const int n = (int)std::min(d.ind.size(), s.ind.size());
    int order[IND_COUNT * 2];
    double rank[IND_COUNT * 2];
    int count = 0;
    for (int k = 0; k < n && count < IND_COUNT * 2; k++) {
        IndustryState &st = s.ind[k];
        st.run = 0.0f; st.shortOf = 0; st.output = st.profit = 0.0f;
        if (d.ind[k].type == IND_SERVICES) st.cap = (float)CityFor(s.pop);      // (as big as the people there are: nobody builds them)
        if (st.cap <= 0.0f) continue;
        double worth = 0.0;
        for (int p = 0; p < d.ind[k].outs; p++) worth += st.cap * d.ind[k].out[p].share * base.perCap[d.ind[k].out[p].good] * PriceOf(s, d.ind[k].out[p].good);
        rank[count] = Priority(d.ind[k].type, fs.Get(st.owner), worth);
        order[count++] = k;
    }
    for (int a = 1; a < count; a++)                                 // (a handful: insertion sort)
        for (int b = a; b > 0 && rank[b] > rank[b - 1]; b--) { std::swap(rank[b], rank[b - 1]); std::swap(order[b], order[b - 1]); }

    s.labour = (float)(LABOUR_SHARE * s.pop);
    double labourLeft = s.labour;
    s.services = 1.0f; s.servicesLabour = 0.0f;
    s.cellsMade = s.cellsUsed = 0.0f;
    s.housing = 0.0f;
    for (int a = 0; a < count; a++) {
        const int k = order[a];
        const IndustryDef &in = d.ind[k];
        const IndustryInfo &info = IndustryData(in.type);
        IndustryState &st = s.ind[k];
        const bool city = IsCivic(in.type);
        // How much of it is wanted: lines slow down as their product piles up
        float room[MAX_PRODUCTS];
        double want = city ? 1.0 : 0.0;
        for (int p = 0; p < in.outs; p++) {
            int g = in.out[p].good;
            room[p] = d.banned[g] || s.target[g] <= 0.0f ? 0.0f : fminf(1.0f, fmaxf(0.0f, 2.0f - s.stock[g] / s.target[g]));
            want += in.out[p].share * room[p];
        }
        // (a city runs as much of itself as there are people to house: a town that has shrunk doesn't need the staff it had)
        if (in.type == IND_CITY) want = fmin(1.0, CityFor(HOUSING_SPARE * s.pop) / st.cap);
        // (a yard builds as hard as shipping hereabouts is stretched: never quite idle, flat out when there's more to carry than ships to carry it;
        // and just enough to make up for wear when the busiest ships are as busy as fleets are meant to be)
        const bool yard = in.type == IND_SHIPYARD;
        if (yard) {
            float busiest = 0.0f;
            if (d.hub >= 0 && (size_t)(d.hub + 1) * ROLE_COUNT <= e.busy.size()) for (int r = 0; r < ROLE_COUNT; r++) busiest = fmaxf(busiest, e.busy[(size_t)d.hub * ROLE_COUNT + r]);
            want = ClampD(busiest - (1.0 / FLEET_SPARE - 1.0 / YARD_SPARE), 0.15, 1.0);
        }
        if (want <= 1e-6) { st.shortOf = SHORT_DEMAND; continue; }
        if (want < 0.5) st.shortOf |= SHORT_DEMAND;
        // Labour: what it gets of what it asks
        double need = IndustryLabour(in.type, st.cap * want, in.site), got = fmin(need, labourLeft);
        labourLeft -= got;
        double manned = need > 0.0 ? pow(got / need, 1.0 / LABOUR_POWER) : 1.0;
        if (manned < 0.98) st.shortOf |= SHORT_LABOUR;
        // Power
        double cells = (city ? CivicCells(in.type, st.cap * want, in.site) : info.cells * st.cap * want) * manned * days, powered = 1.0;
        if (cells > 0.0) {
            powered = fmin(1.0, s.stock[REF_CELLS] / cells);
            if (powered < 0.98) st.shortOf |= SHORT_CELLS;
            s.stock[REF_CELLS] = fmaxf(0.0f, s.stock[REF_CELLS] - (float)(cells * powered));
            s.cellsUsed += (float)(cells * powered / days);
        }
        double level = manned * powered;                           // (of what was wanted)
        if (yard) {
            static const int from[4] = { GOOD_HULL_PLATING, GOOD_SHIP_COMPONENTS, GOOD_REACTOR_CORES, GOOD_MACHINERY };
            static const double share[4] = { YARD_PLATING, YARD_COMPONENTS, YARD_CORES, YARD_MACHINERY };
            double asked = st.cap * want * base.yardYield * days, ship = asked * level;
            for (int m = 0; m < 4; m++) ship = fmin(ship, s.stock[from[m]] / share[m]);
            ship = fmax(ship, 0.0);
            for (int m = 0; m < 4; m++) s.stock[from[m]] = fmaxf(0.0f, s.stock[from[m]] - (float)(ship * share[m]));
            if (ship < 0.98 * asked * level) st.shortOf |= SHORT_MATERIALS;
            if ((size_t)i < e.slips.size()) e.slips[i] += (float)ship;
            st.run = asked > 0.0 ? (float)(want * ship / asked) : 0.0f;
            st.output = (float)(ship / days);
            continue;
        }
        if (city) {
            if (in.type == IND_CITY) s.housing = (float)CityHousing(st.cap * want * level); else { s.services = (float)level; s.servicesLabour = (float)got; }
            st.run = (float)level;
            continue;
        }
        // Materials: each product by its own recipe; and what the industry itself uses besides
        float made = supply * boost(st.owner) * (d.ruler != st.owner ? boost(d.ruler) : 1.0f);
        double q[MAX_PRODUCTS], total = 0.0, asked = 0.0, value = 0.0;
        for (int p = 0; p < in.outs; p++) {
            int g = in.out[p].good;
            double full = st.cap * in.out[p].share * base.perCap[g] * room[p] * days * made;
            q[p] = full * level;
            asked += full;
            for (const Ingredient &m : GoodData(g).recipe) if (m.tonnes > 0.0f) q[p] = fmin(q[p], s.stock[m.good] / m.tonnes);
            q[p] = fmax(q[p], 0.0);
            for (const Ingredient &m : GoodData(g).recipe) if (m.tonnes > 0.0f) s.stock[m.good] = fmaxf(0.0f, s.stock[m.good] - (float)(q[p] * m.tonnes));
            total += q[p];
            for (const Ingredient &m : GoodData(g).recipe) if (m.tonnes > 0.0f) g_flow[1][m.good] += q[p] * m.tonnes;
        }
        double fed = 1.0;
        for (const Ingredient &m : info.extra) if (m.tonnes > 0.0f && total > 0.0) fed = fmin(fed, s.stock[m.good] / (total * m.tonnes));
        if (fed < 1.0) {                                           // (what was drawn for output that can't now be made goes back)
            for (int p = 0; p < in.outs; p++)
                for (const Ingredient &m : GoodData(in.out[p].good).recipe) if (m.tonnes > 0.0f) s.stock[m.good] += (float)(q[p] * (1.0 - fed) * m.tonnes);
            for (int p = 0; p < in.outs; p++) q[p] *= fed;
            total *= fed;
        }
        for (const Ingredient &m : info.extra) if (m.tonnes > 0.0f) s.stock[m.good] = fmaxf(0.0f, s.stock[m.good] - (float)(total * m.tonnes));
        if (total < 0.98 * asked * level) st.shortOf |= SHORT_MATERIALS;
        for (int p = 0; p < in.outs; p++) {
            int g = in.out[p].good;
            if (q[p] <= 0.0) continue;
            g_flow[0][g] += q[p];
            value += q[p] * (PriceOf(s, g, pricing) - INDUSTRY_COST * GoodData(g).basePrice);
            if (flown && e.zone[i] == 2 && g <= RAW_HELIUM3) {     // (dug or skimmed: it comes in when the miners do)
                e.yield[(size_t)i * GOOD_COUNT + g] += (float)q[p]; e.afloat[(size_t)i * GOOD_COUNT + g] += (float)q[p];
            } else s.stock[g] += (float)q[p];
            if (g == REF_CELLS) s.cellsMade += (float)(q[p] / days);
        }
        st.run = asked > 0.0 ? (float)(want * total / asked) : 0.0f;
        st.output = (float)(total / days);
        st.profit = (float)(value / days);
        // (the profit is its owner's, less what the government of the place takes of works that aren't its own; a loss is its owner's too)
        double tax = value > 0.0 && d.ruler != st.owner ? value * ProfitTaxNow(e, d.ruler) : 0.0;
        Pay(e, i, d.ruler, tax, INC_TAX);
        if (value - tax > 0.0) Pay(e, i, st.owner, value - tax, INC_PROFIT); else Lose(e, i, st.owner, tax - value);
        st.earned += (value - tax) / st.cap;
    }
    s.employed = (float)(s.labour - labourLeft);
    // New capacity comes on line
    bool grew = false;
    for (int k = 0; k < n; k++) {
        IndustryState &st = s.ind[k];
        if (st.building > 0.0f) st.buildLeft -= (float)days;
        if (st.building > 0.0f && st.buildLeft <= 0.0f) { st.cap += st.building; st.building = 0.0f; st.buildLeft = 0.0f; grew = true; }
    }
    if (grew) Derive(base, i, s);
}

// What an addition to an industry takes: how much capacity, and the materials for it
static double UpgradeStep(const IndustryState &st, const MarketDef &d, int type = IND_FARM)
{
    if (type == IND_CITY) return fmax(0.2, CITY_STEP * st.cap);
    return st.cap > 0.0f ? fmax(UPGRADE_MIN, UPGRADE_STEP * st.cap) : fmax(UPGRADE_MIN, 0.02 * d.scale);
}
static void UpgradeNeeds(int type, double step, double need[3], double cap = 0.0, double site = 1.0)
{
    if (type == IND_CITY) {                                        // (so much a head, and more where there's no sky to build under)
        double people = (CityHousing(cap + step) - CityHousing(cap)) / fmax(site, 0.1);
        need[0] = CITY_STEEL * people; need[1] = CITY_MACHINERY * people; need[2] = CITY_MODULES * people;
        return;
    }
    need[0] = UPGRADE_STEEL * step; need[1] = UPGRADE_MACHINERY * step;
    need[2] = type == IND_AGRIDOME || type == IND_HYDROPONICS ? UPGRADE_MODULES * step : 0.0;
}
static const int UPGRADE_GOODS[3] = { REF_STEEL, GOOD_MACHINERY, GOOD_HABITAT_MODULES };

// Is there the labour and the materials to add to this industry now? (An owner won't strip the market bare for it.)
static bool CanUpgrade(const MarketDef &d, const MarketState &s, int k, double step, double keep)
{
    const IndustryState &st = s.ind[k];
    if (st.building > 0.0f) return false;
    double more = IndustryLabour(d.ind[k].type, st.cap + step, d.ind[k].site) - IndustryLabour(d.ind[k].type, st.cap, d.ind[k].site);
    // (the hands for it: whoever is idle, and whoever is in services, which give way to anything worth building)
    if (d.ind[k].type != IND_CITY && more > s.labour - s.employed + s.servicesLabour) return false;
    double need[3];
    UpgradeNeeds(d.ind[k].type, step, need, st.cap, d.ind[k].site);
    for (int m = 0; m < 3; m++) if (need[m] > 0.0 && s.stock[UPGRADE_GOODS[m]] - keep * s.target[UPGRADE_GOODS[m]] < need[m]) return false;
    return true;
}

static void StartUpgrade(MarketState &s, const MarketDef &d, int k, double step)
{
    double need[3];
    UpgradeNeeds(d.ind[k].type, step, need, s.ind[k].cap, d.ind[k].site);
    for (int m = 0; m < 3; m++) s.stock[UPGRADE_GOODS[m]] = fmaxf(0.0f, s.stock[UPGRADE_GOODS[m]] - (float)need[m]);
    s.ind[k].building = (float)step; s.ind[k].buildLeft = (float)UPGRADE_DAYS;
}

// How much dearer it is to add to a world's industry than it was when a galaxy began: the easy sites go first, and
// everything after is harder. (All its works together: a world that has doubled them pays nearly three times as
// much for the next addition, to any of them. Past a point a new place is the cheaper one to build in.)
double BuiltUp(const MarketDef &d, const MarketState &s)
{
    double now = 0.0, began = 0.0;
    for (size_t k = 0; k < d.ind.size() && k < s.ind.size(); k++) {
        if (IsCivic(d.ind[k].type)) continue;
        now += s.ind[k].cap + s.ind[k].building; began += d.ind[k].cap;
    }
    began = fmax(began, 0.05 * d.scale);                           // (a new place: against what a place its size would start with)
    return began > 0.0 ? pow(fmax(1.0, now / began), BUILT_UP_POWER) : 1.0;
}

// What an addition costs its owner: the materials, at what they fetch there, and the builders
static double UpgradeCost(const EconBase &base, const MarketDef &d, const MarketState &s, int k, double step)
{
    double usual = 0.0, need[3], cost = 0.0;
    for (int p = 0; p < d.ind[k].outs; p++) usual += d.ind[k].out[p].share * base.perCap[d.ind[k].out[p].good] * GoodData(d.ind[k].out[p].good).basePrice;
    if (!IsCivic(d.ind[k].type)) cost = OWN_BUILD * fmax(INVEST_FEE, INVEST_DAYS * (1.0 - INDUSTRY_COST) * usual) * step;
    UpgradeNeeds(d.ind[k].type, step, need, s.ind[k].cap, d.ind[k].site);
    for (int m = 0; m < 3; m++) cost += need[m] * PriceOf(s, UPGRADE_GOODS[m]);
    return cost * BuiltUp(d, s);
}

// The owners look at what's worth building: more of what's selling well above its usual price, where there's the
// labour to run it and the materials to build it; and more city where people are short of a roof
static void Develop(Economy &e, const EconBase &base, double clock)
{
    for (int i = 0; i < (int)base.defs.size(); i++) {
        const MarketDef &d = base.defs[i];
        MarketState &s = e.state[i];
        int best = -1; double bestGain = 0.0;
        for (int k = 0; k < (int)s.ind.size() && k < (int)d.ind.size(); k++) {
            const IndustryDef &in = d.ind[k];
            const IndustryState &st = s.ind[k];
            if (st.building > 0.0f) { best = -1; break; }          // (one thing at a time)
            double gain = 0.0;
            if (in.type == IND_SERVICES) continue;
            // (people need a roof before anything else; but nobody builds for more than the place can hold)
            if (in.type == IND_CITY) gain = s.pop > 0.0 && CityHousing(st.cap) < fmin(1.03 * s.pop, d.room) ? 10.0 : 0.0;
            else if (in.type == IND_SHIPYARD) {
                // (more slips where there has been more to carry than ships to carry it)
                float busiest = 0.0f;
                if (st.cap > 0.0f && d.hub >= 0 && (size_t)(d.hub + 1) * ROLE_COUNT <= e.busy.size())
                    for (int r = 0; r < ROLE_COUNT; r++) busiest = fmaxf(busiest, e.busy[(size_t)d.hub * ROLE_COUNT + r]);
                gain = busiest > YARD_GROW_ABOVE ? busiest : 0.0;
            } else {
                double worth = 0.0, usual = 0.0;
                for (int p = 0; p < in.outs; p++) {
                    int g = in.out[p].good;
                    if (d.banned[g] || s.target[g] <= 0.0f || s.stock[g] > s.target[g]) continue;
                    worth += in.out[p].share * base.perCap[g] * PriceOf(s, g); usual += in.out[p].share * base.perCap[g] * GoodData(g).basePrice;
                }
                double ratio = usual > 0.0 ? worth / usual : 0.0;
                // (what's left of it after the government's share, against what's usually left)
                if (st.owner != d.ruler) ratio *= (1.0 - ProfitTaxNow(e, d.ruler)) / (1.0 - PROFIT_TAX);
                if (ratio >= (st.cap > 0.0f ? UPGRADE_PRICE : FOUND_PRICE)) gain = ratio;
            }
            if (gain > bestGain && CanUpgrade(d, s, k, UpgradeStep(st, d, in.type), in.type == IND_CITY ? 0.05 : 0.25)) { bestGain = gain; best = k; }
        }
        // (...if its owner has the money for it at its capital)
        if (best >= 0) {
            double step = UpgradeStep(s.ind[best], d, d.ind[best].type);
            // (a city is built by the people who'll live in it: nobody's treasury pays for that)
            if (IsCivic(d.ind[best].type) || Spend(e, s.ind[best].owner, UpgradeCost(base, d, s, best, step), DEVELOP_DAYS / UPGRADE_DAYS)) StartUpgrade(s, d, best, step);
        }
    }
    e.developed = clock;
}

// ---------------------------------------------------------------------------
// People: how well they live, and so whether there are more of them tomorrow,
// or fewer, or the same number somewhere else
// ---------------------------------------------------------------------------
// How full a place is: nothing until it's half of what it can ever hold, then more and more steeply up to 1 when it's full
double Crowding(const MarketDef &d, const MarketState &s)
{
    if (d.room <= 0.0) return 0.0;
    double over = ClampD((s.pop / d.room - CROWD_FROM) / (1.0 - CROWD_FROM), 0.0, 1.0);
    return over * over;
}

// A world's living standard, 0 .. 1: food on the table and in store, a roof, work, water, medicine, a few comforts,
// and not being raided
double LivingStandard(const Economy &e, const FactionSet &fs, int i)
{
    const MarketDef &d = e.base->defs[i];
    const MarketState &s = e.state[i];
    // (how well supplied it is with a good: 1 with 'full' of its target stock in hand; and 1 if it doesn't deal in it)
    auto have = [&](int g, double full) { return s.target[g] > 0.0f ? ClampD(s.stock[g] / (full * s.target[g]), 0.0, 1.0) : 1.0; };
    // (fed: having in store what nobody would sell below, a quarter of the usual stock. Past that, it's reserve.)
    double food = have(REF_FOOD, FED_AT), reserve = s.target[REF_FOOD] > 0.0f ? ClampD((s.stock[REF_FOOD] / s.target[REF_FOOD] - FED_AT) / (1.0 - FED_AT), 0.0, 1.0) : 1.0;
    double roof = s.pop > 0.0 ? fmin(1.0, s.housing / s.pop) : 1.0, work = s.labour > 0.0f ? fmin(1.0, (double)s.employed / s.labour) : 1.0;
    // (comforts: what there is to buy, and the services to go with it)
    double comfort = 0.5 * (have(GOOD_CONSUMER, 1.0) + have(GOOD_LIQUOR, 1.0) + have(GOOD_LUXURY, 1.0)) / 3.0 + 0.5 * s.services;
    return LIVING_FOOD * food + LIVING_RESERVE * reserve + LIVING_HOUSING * roof + LIVING_WORK * work + LIVING_WATER * have(REF_WATER, FED_AT) +
           LIVING_MEDICINE * have(GOOD_MEDICINES, FED_AT) + LIVING_COMFORT * comfort + LIVING_SAFETY * (1.0 - RaidAt(e, fs, d.ly)) -
           LIVING_TAX * (Counted(e, d.ruler) ? e.money[d.ruler].squeeze - 1.0 : 0.0) -     // (and how hard they're taxed, against the usual)
           LIVING_CROWD * Crowding(d, s);                                                   // (and how full the place is)
}

// How a government's borders stand to begin with: the inward-looking shut theirs, the expansionist open them
static uint8_t BorderByNature(const Faction &f)
{
    if (f.kind != FactionKind::Government) return BORDER_OPEN;
    return f.axis[AX_OUTLOOK] < -0.35f ? BORDER_CLOSED : f.axis[AX_OUTLOOK] > 0.25f ? BORDER_OPEN : BORDER_SELECTIVE;
}

// Will this place take people from that one? 0 no (the border is shut, or it only takes those it has work and
// room for), otherwise the share of those who'd come that it lets in
static double LetIn(const Economy &e, int from, int to)
{
    int there = e.base->defs[to].ruler;
    if (there < 0 || there == e.base->defs[from].ruler || there >= (int)e.border.size()) return 1.0;
    const MarketState &s = e.state[to];
    switch (e.border[there]) {
    case BORDER_CLOSED:    return 0.0;
    case BORDER_SELECTIVE: return s.employed > SELECTIVE_WORK * s.labour && s.housing > s.pop ? SELECTIVE_SHARE : 0.0;
    default:               return 1.0;
    }
}

static void People(World *news, Economy &e, const EconBase &base, const FactionSet &fs, double days, double clock, const std::vector<Carrier> &at)
{
    const int N = (int)base.defs.size();
    static std::vector<double> living, push, delta;
    static std::vector<std::vector<std::pair<int, float>>> ways;   // from each economy: where people can go from it, and how many a day
    static std::vector<std::vector<int>> wayLane;                  // ...and which lane that is (-1: within the system)
    living.resize(N); push.resize(N); delta.assign(N, 0.0);
    static const EconBase *waysFor = nullptr;
    if (waysFor != &base) {
        waysFor = &base;
        ways.assign(N, {}); wayLane.assign(N, {});
        for (int i = 0; i < N; i++) {
            int hub = base.defs[i].hub;
            if (hub < 0 || hub == i) continue;
            float cap = (float)(2.0 * base.defs[i].scale * PEOPLE_PER_TONNE);
            ways[i].push_back({ hub, cap }); ways[hub].push_back({ i, cap });
            wayLane[i].push_back(-1); wayLane[hub].push_back(-1);
        }
        for (size_t k = 0; k < base.lanes.size(); k++) {
            const Lane &l = base.lanes[k];
            ways[l.a].push_back({ l.b, 0.0f }); ways[l.b].push_back({ l.a, 0.0f });
            wayLane[l.a].push_back((int)k); wayLane[l.b].push_back((int)k);
        }
    }
    // Between systems people go by liner: as many a day as the liners on that lane have berths for
    for (int i = 0; i < N; i++)
        for (size_t k = 0; k < ways[i].size(); k++) {
            if (wayLane[i][k] < 0) continue;
            const Lane &l = base.lanes[wayLane[i][k]];
            ways[i][k].second = (float)(at[l.a].ships[ROLE_LINER] * l.shareA * at[l.a].Daily(ROLE_LINER, l.ly) + at[l.b].ships[ROLE_LINER] * l.shareB * at[l.b].Daily(ROLE_LINER, l.ly));
        }
    for (int i = 0; i < N; i++) {
        MarketState &s = e.state[i];
        s.living = (float)(living[i] = LivingStandard(e, fs, i));
        // Who wants out: more the worse it is, and anyone without a roof
        double leave = s.pop * LEAVE_MAX * ClampD((LEAVE_BELOW - living[i]) / 0.3, 0.0, 1.0) + CROWD_LEAVE * fmax(0.0, s.pop - s.housing);
        push[i] = fmin(leave * days, fmax(0.0, s.pop - SKELETON_CREW));
        s.barredTo = -1; s.bestTo = -1;
    }
    // They go a hop at a time, along the lanes, to wherever is better and will have them
    for (int i = 0; i < N; i++) {
        MarketState &s = e.state[i];
        s.leaving = s.stuck = 0.0f;
        if (push[i] <= 0.0) {
            if (base.defs[i].hub == i && (size_t)(i + 1) * ROLE_COUNT <= e.busy.size()) {      // (nobody much travelling: the liners here are quiet)
                float &b = e.busy[(size_t)i * ROLE_COUNT + ROLE_LINER];
                b += (0.3f - b) * (float)(1.0 - exp(-days / 3.0));
            }
            continue;
        }
        double weights[64], lets[64], total = 0.0, best = 0.0, barred = 0.0;
        int n = (int)std::min<size_t>(ways[i].size(), 64);
        for (int k = 0; k < n; k++) {
            int j = ways[i][k].first;
            const MarketState &t = e.state[j];
            weights[k] = 0.0; lets[k] = 1.0;
            if (living[j] < living[i] + 0.05) continue;
            double room = fmin(fmax(0.0, t.housing - t.pop) + 0.02 * t.pop, fmax(0.0, base.defs[j].room - t.pop)), let = LetIn(e, i, j), draw = (living[j] - living[i]) * room;
            if (let <= 0.0) { if (draw > barred) { barred = draw; s.barredTo = j; } continue; }       // (turned away at the border)
            weights[k] = draw * let; lets[k] = let;
            total += weights[k];
            if (weights[k] > best) { best = weights[k]; s.bestTo = j; }
        }
        double gone = 0.0;
        for (int k = 0; k < n && total > 0.0; k++) {
            if (weights[k] <= 0.0) continue;
            int j = ways[i][k].first;
            const MarketState &t = e.state[j];
            double room = fmin(fmax(0.0, t.housing - t.pop) + 0.02 * t.pop, fmax(0.0, base.defs[j].room - t.pop));
            double q = fmin(fmin(push[i] * weights[k] / total, ways[i][k].second * days), 0.5 * room) * lets[k];
            delta[i] -= q; delta[j] += q; gone += q;
        }
        s.leaving = (float)(gone / days);
        s.stuck = (float)((push[i] - gone) / days);
        // (the liners here: busy if there were more to carry than berths)
        if (base.defs[i].hub == i && (size_t)(i + 1) * ROLE_COUNT <= e.busy.size()) {
            float &b = e.busy[(size_t)i * ROLE_COUNT + ROLE_LINER];
            b += ((s.stuck > 1.0f ? 1.5f : s.leaving > 1.0f ? 0.7f : 0.3f) - b) * (float)(1.0 - exp(-days / 3.0));
        }
    }
    for (int i = 0; i < N; i++) {
        MarketState &s = e.state[i];
        double before = s.pop;
        if (delta[i] > 0.0) s.refugees += (float)delta[i];          // (new arrivals: displaced people, until they've settled in)
        s.refugees *= (float)exp(-0.1 * days);
        s.pop += delta[i];
        // More of them, where life is good and there's a roof for them...
        // (and slower as the place fills up)
        const double space = base.defs[i].room > 0.0 ? ClampD(1.0 - s.pop / base.defs[i].room, 0.0, 1.0) : 1.0;
        if (living[i] > GROW_ABOVE && s.pop < s.housing)
            s.pop = fmin((double)s.housing, s.pop * (1.0 + GROWTH_MAX * space * ClampD((living[i] - GROW_ABOVE) / 0.15, 0.0, 1.0) * days));
        // ...and fewer where there's nothing to eat
        double fed = s.target[REF_FOOD] > 0.0f ? ClampD(s.stock[REF_FOOD] / (FED_AT * s.target[REF_FOOD]), 0.0, 1.0) : 1.0;
        if (fed < STARVE_BELOW) s.pop *= 1.0 - STARVE_MAX * (STARVE_BELOW - fed) / STARVE_BELOW * days;
        s.pop = fmax(s.pop, fmin(before, SKELETON_CREW));           // (a few caretakers always stay)
        s.trend = before > 0.0 ? (float)((s.pop - before) / before / days) : 0.0f;
        if (fabs(s.pop - s.popDerived) > 0.01 * s.popDerived) { s.popDerived = s.pop; Derive(base, i, s); }
    }
    // Governments look at their borders now and then: one swamped with new arrivals tightens them, one short of
    // hands (or no longer swamped) eases them back toward what comes naturally to it
    if (clock - e.bordersLooked < 2.0 * MARKET_DAY && clock >= e.bordersLooked) return;
    e.bordersLooked = clock;
    const int F = fs.Count();
    std::vector<double> people(F, 0.0), arrived(F, 0.0), served(F, 0.0);
    for (int i = 0; i < N; i++) {
        int r = base.defs[i].ruler;
        if (r < 0 || r >= F) continue;
        people[r] += e.state[i].pop; arrived[r] += e.state[i].refugees; served[r] += e.state[i].services * e.state[i].pop;
    }
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Government || people[f.id] <= 0.0 || f.id >= (int)e.border.size()) continue;
        uint8_t was = e.border[f.id], nature = BorderByNature(f);
        // (short of hands: so short that its services are going unstaffed)
        double strain = arrived[f.id] / people[f.id], staffed = served[f.id] / people[f.id];
        if (strain > 0.03 && was < BORDER_CLOSED) e.border[f.id] = was + 1;
        // (a shortage eases a border one step from what comes naturally, and no further: a closed state lets in whom it needs, not all comers)
        else if ((strain < 0.005 && was > nature) || (staffed < 0.5 && was > BORDER_OPEN && was + 1 > nature)) e.border[f.id] = was - 1;
        else if (strain < 0.005 && staffed > 0.8 && was < nature) e.border[f.id] = was + 1;       // (and no longer short: back to its own way)
        if (e.border[f.id] == was || !news) continue;
        static const char *now[] = { " opens its borders to all comers", " will take only the migrants it has work and room for", " closes its borders to migrants" };
        PostNews(*news, f.id, f.seat, f.name + now[e.border[f.id]]);
    }
}

// (sizing fleets: what there was to carry, where the ships there were couldn't take it all; but no more than a few times what they did)
static double Wanted(double wanted, double moved) { return fmax(moved, fmin(wanted, 3.0 * moved)); }

static void RunDays(Economy &e, const EconBase &base, const FactionSet &fs, double days, double clock, World *live = nullptr)
{
    g_flowDays += days;
    e.paying = live != nullptr;
    const int N = (int)base.defs.size();
    static std::vector<float> supply, demand;
    supply.assign(N, 1.0f); demand.assign(N, 1.0f);
    for (size_t i = 0; i < e.shocks.size();) {
        if (e.shocks[i].until <= clock) { e.shocks.erase(e.shocks.begin() + i); continue; }
        supply[e.shocks[i].market] *= e.shocks[i].supply; demand[e.shocks[i].market] *= e.shocks[i].demand;
        i++;
    }
    const bool flown = (int)e.zone.size() == N;                    // (somewhere, ships are carrying the trade)
    for (int i = 0; i < N; i++) Work(e, base, fs, i, days, supply[i], demand[i], clock, flown);

    // Everyone's prices, now that the day's making and using is done
    static std::vector<float> prices;
    prices.resize((size_t)N * GOOD_COUNT);
    for (int i = 0; i < N; i++) {
        double pricing = Pricing(e, e.state[i].owner, clock);
        for (int g = 0; g < GOOD_COUNT; g++) prices[(size_t)i * GOOD_COUNT + g] = (float)PriceOf(e.state[i], g, pricing);
    }
    auto row = [&](int i) { return &prices[(size_t)i * GOOD_COUNT]; };
    const bool measuring = e.measured.size() == 2 * base.lanes.size() + (size_t)N;
    if (measuring) e.measuredDays += days;
    // The ships there are to carry it
    static std::vector<Carrier> at;
    static std::vector<double> busyNow;                 // for each hub and role: what there was to carry, and what its ships could have
    at.assign(N, Carrier());
    busyNow.assign((size_t)N * ROLE_COUNT * 2, 0.0);
    for (int i = 0; i < N; i++) if (base.defs[i].hub == i) at[i] = CarrierAt(e, base, i);
    auto note = [&](int hub, int role, double wanted, double can) { double *b = &busyNow[((size_t)hub * ROLE_COUNT + role) * 2]; b[0] += wanted; b[1] += can; };
    // Trade: every market with its system's hub...
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        if (d.hub == i || d.hub < 0) continue;
        Haul h = LocalHaul(d, at[d.hub]);
        double moved[2] = { 0.0, 0.0 }, load[4] = { 0.0, 0.0, 0.0, 0.0 };
        if (measuring) h.limited = false;
        double paid[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
        Trade(e.state[i], e.state[d.hub], h, days, 0.0, 0.0, row(i), row(d.hub), flown && e.zone[i] == 2 ? &e : nullptr, i, d.hub, clock, moved, load, paid);
        PayFleets(e, d.hub, -1, ROLE_HAULER, paid[2] + paid[3]);
        note(d.hub, ROLE_HAULER, load[0] + load[2], load[1] + load[3]);
        if (measuring) e.measured[2 * base.lanes.size() + i] += Wanted(load[0], moved[0]) + Wanted(load[2], moved[1]);
    }
    // ...and hubs with each other, along the lanes: less with distance, less still through raiders, not at all across an embargo
    const int F = fs.Count();
    for (size_t li = 0; li < base.lanes.size(); li++) {
        const Lane &l = base.lanes[li];
        const MarketDef &a = base.defs[l.a], &b = base.defs[l.b];
        bool borders = a.ruler != b.ruler && a.ruler >= 0 && b.ruler >= 0;
        if (borders && (e.embargoUntil[a.ruler * F + b.ruler] > clock || e.embargoUntil[b.ruler * F + a.ruler] > clock)) continue;
        bool byShip = flown && (e.zone[l.a] || e.zone[l.b]);
        // (where the ships are flown, so are the pirates: what they take is taken from real holds, not off the top)
        double raids = flown && (e.zone[l.a] == 2 || e.zone[l.b] == 2) ? 0.0 : fmax(RaidAt(e, fs, a.ly), RaidAt(e, fs, b.ly));
        double intoB = a.ruler != b.ruler ? TariffNow(e, b.ruler) : 0.0, intoA = a.ruler != b.ruler ? TariffNow(e, a.ruler) : 0.0;
        Haul h = LaneHaul(l, at[l.a], at[l.b], raids);
        double moved[2] = { 0.0, 0.0 }, load[4] = { 0.0, 0.0, 0.0, 0.0 };
        if (measuring) h.limited = false;
        double paid[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
        Trade(e.state[l.a], e.state[l.b], h, days, intoB, intoA, row(l.a), row(l.b), byShip ? &e : nullptr, l.a, l.b, clock, moved, load, paid);
        // (who was paid: the governments their tariffs, the carriers their cut; and raiders, what they got away with)
        Pay(e, l.b, b.ruler, paid[0], INC_TARIFF); Pay(e, l.a, a.ruler, paid[1], INC_TARIFF);
        PayFleets(e, l.a, l.b, ROLE_BULK, paid[2]); PayFleets(e, l.a, l.b, ROLE_TANKER, paid[3]);
        if (raids > 0.0) Loot(e, fs, a.ly, LOOT_SHARE * raids * paid[4]);
        if (measuring) { e.measured[2 * li] += Wanted(load[0], moved[0]); e.measured[2 * li + 1] += Wanted(load[2], moved[1]); }
        for (int end : { l.a, l.b }) { note(end, ROLE_BULK, load[0], load[1]); note(end, ROLE_TANKER, load[2], load[3]); }
    }
    // (how busy shipping has been: a few days' memory of it)
    const float fresh = (float)(1.0 - exp(-days / 3.0));
    for (int i = 0; i < N; i++)
        if (base.defs[i].hub == i)
            for (int r : { ROLE_BULK, ROLE_HAULER, ROLE_TANKER }) {
                const double *n = &busyNow[((size_t)i * ROLE_COUNT + r) * 2];
                float now = n[1] > 0.0 ? (float)fmin(n[0] / n[1], 4.0) : n[0] > 0.0 ? 4.0f : 0.0f;
                float &b = e.busy[(size_t)i * ROLE_COUNT + r];
                b += (now - b) * fresh;
            }
    if (live) { People(live, e, base, fs, days, clock, at); Shipping(e, base, fs, days); }
    if (live) {
        if (clock - e.collected >= COLLECT_DAYS * MARKET_DAY || clock < e.collected) { Collect(e, base, fs); e.collected = clock; }
        if (clock - e.ratesLooked >= TAX_LOOK_DAYS * MARKET_DAY || clock < e.ratesLooked) { AdjustTaxes(e, fs); e.ratesLooked = clock; }
        MakeUpBooks(e, days);
    }
    // Raiders left alone drift back to their usual level
    for (const Faction &f : fs.list)
        if (f.kind == FactionKind::Clan && e.raidUntil[f.id] <= clock) {
            float step = (float)(0.1 * days);
            e.raid[f.id] += fmaxf(-step, fminf(step, (float)USUAL_RAID - e.raid[f.id]));
        }
}

static void InitState(Economy &e, const EconBase &base, const FactionSet &fs)
{
    const int N = (int)base.defs.size(), F = fs.Count();
    e.state.assign(N, MarketState());
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        MarketState &s = e.state[i];
        s.owner = d.owner; s.pop = s.popDerived = d.pop;
        s.ind.resize(d.ind.size());
        for (size_t k = 0; k < d.ind.size(); k++) { s.ind[k].cap = d.ind[k].cap; s.ind[k].owner = d.ind[k].owner; }
        Derive(base, i, s);
        for (int g = 0; g < GOOD_COUNT; g++)
            s.stock[g] = base.stock0.empty() ? s.target[g] : base.stock0[(size_t)i * GOOD_COUNT + g];
        s.labour = (float)(LABOUR_SHARE * s.pop); s.employed = (float)(EMPLOYED_AT_START * s.labour);
        s.housing = (float)(HOUSING_SPARE * s.pop);
    }
    e.tariff.assign(F, 0.0f);
    for (const Faction &f : fs.list) e.tariff[f.id] = f.tariff;
    e.money.assign(F, FactionMoney());
    for (const Faction &f : fs.list) {
        FactionMoney &m = e.money[f.id];
        m.treasury = f.kind == FactionKind::Government ? START_GOVERNMENT * fmax(0.3, (double)f.strength) : f.kind == FactionKind::Conglomerate ? START_COMPANY
                   : f.kind == FactionKind::Clan ? START_CLAN : START_OTHER;
        m.overhead = (float)(f.kind == FactionKind::Government ? OVERHEAD_GOVERNMENT : f.kind == FactionKind::Conglomerate ? OVERHEAD_COMPANY : f.kind == FactionKind::Clan ? OVERHEAD_CLAN : OVERHEAD_OTHER);
        if (f.kind == FactionKind::Government) { m.profitTax = (float)(PROFIT_TAX + PLANNERS_TAX * fmax(0.0, (double)f.axis[AX_MARKETS])); m.headTax = (float)HEAD_TAX; }
    }
    e.coffer.assign((size_t)N * F, 0.0);
    e.moving.assign((size_t)N * F, 0.0);
    e.sailings = 0;
    e.collected = e.ratesLooked = 0.0;
    e.embargoUntil.assign((size_t)F * F, 0.0);
    e.raid.assign(F, 0.0f);
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) e.raid[f.id] = (float)USUAL_RAID;
    e.raidUntil.assign(F, 0.0);
    e.output.assign(F, 1.0f);  e.outputUntil.assign(F, 0.0);
    e.pricing.assign(F, 1.0f); e.pricingUntil.assign(F, 0.0);
    e.nextAct.assign(F, 0.0);
    e.lastAct.assign(F, -1);
    e.shocks.clear();
    e.zone.assign(N, 0);
    e.waiting.clear();
    e.yield.assign((size_t)N * GOOD_COUNT, 0.0f);
    e.afloat.assign((size_t)N * GOOD_COUNT, 0.0f);
    e.developed = 0.0;
    e.fleets = base.fleets0;
    e.fleets.resize(N);
    e.busy.assign((size_t)N * ROLE_COUNT, (float)(1.0 / FLEET_SPARE));
    e.slips.assign(N, 0.0f);
    e.clanShips.assign(F, 0.0f);
    e.border.assign(F, BORDER_OPEN);
    for (const Faction &f : fs.list) e.border[f.id] = BorderByNature(f);
    e.bordersLooked = 0.0;
}

// A new galaxy's markets have been trading for a while: run it in until stocks stop lurching
// ('measure': with fuel for the asking; and returns the tonnes a day that moved on each lane and local run, for sizing fleets by)
static std::vector<double> SettleStocks(EconBase &base, const FactionSet &fs, bool measure)
{
    Economy e;
    base.stock0.clear();
    InitState(e, base, fs);
    // (measuring is a rougher job: it's done more than once, and a new galaxy shouldn't keep anyone waiting)
    for (int step = 0; step < (measure ? 40 : 100); step++) RunDays(e, base, fs, 0.25, 0.0);
    for (int step = 0; step < (measure ? 20 : 100); step++) RunDays(e, base, fs, 0.05, 0.0);       // (...then at something like the pace it will really run at)
    if (measure) {
        // (...and what moves is counted at exactly that pace: in bigger steps trade overshoots, and more seems to be wanted than is)
        e.measured.assign(2 * base.lanes.size() + base.defs.size(), 0.0); e.measuredDays = 0.0;
        for (int step = 0; step < 40; step++) RunDays(e, base, fs, ECON_TICK / MARKET_DAY, 0.0);
    }
    std::vector<double> carried = e.measured;
    for (double &c : carried) c /= fmax(e.measuredDays, 1e-9);
    base.stock0.resize(base.defs.size() * GOOD_COUNT);
    for (size_t i = 0; i < base.defs.size(); i++)
        for (int g = 0; g < GOOD_COUNT; g++) base.stock0[i * GOOD_COUNT + g] = e.state[i].stock[g];
    return carried;
}

// ---------------------------------------------------------------------------
// News
// ---------------------------------------------------------------------------
static void Report(World &w, int faction, SysId where, const std::string &text)
{
    EconNews n;
    n.at = w.clock; n.faction = faction; n.where = where; n.text = text; n.seq = ++w.newsSeq;
    w.news.push_back(n);
    if (w.news.size() > 80) w.news.erase(w.news.begin());
}

void PostNews(World &w, int faction, SysId where, const std::string &text) { Report(w, faction, where, text); }

// ---------------------------------------------------------------------------
// The factions act: each, every few days, does the most pressing thing on its
// agenda that it has the means to do
// ---------------------------------------------------------------------------
static bool Starts(const std::string &s, const char *prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

static int NearestClan(const FactionSet &fs, DVec3 ly, int prefer = NO_FACTION)
{
    int best = NO_FACTION; double bestD = 1e30;
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Clan) continue;
        double d = Len(ly - f.seatLy) * (f.patron == prefer && prefer != NO_FACTION ? 0.4 : 1.0);
        if (d < bestD) { bestD = d; best = f.id; }
    }
    return best;
}

// A station to take over: 'from' owns it now (NO_FACTION: independents), within reach of 'by', in 'inSpace' (or anywhere)
static int FindStation(const Economy &e, const Faction &by, int from, int inSpace)
{
    int best = -1; double bestD = 40.0;
    for (int i = 0; i < e.Count(); i++) {
        const MarketDef &d = e.base->defs[i];
        if (!d.station || e.state[i].owner != from || d.ruler == 0 || (inSpace != NO_FACTION && d.ruler != inSpace)) continue;
        double dist = Len(d.ly - by.seatLy);
        if (dist < bestD) { bestD = dist; best = i; }
    }
    return best;
}

// The agenda is written in words (factions.cpp); this is what the words mean here.
static bool Act(World &w, const Faction &f, const AgendaItem &item)
{
    Economy &e = w.econ;
    const FactionSet &fs = w.systems.factions;
    const double day = MARKET_DAY, now = w.clock;
    const std::string &t = item.text;
    const Faction *other = fs.Get(item.target);
    const int F = fs.Count();
    auto clanNews = [&](int clan, const char *what) { Report(w, clan, fs.list[clan].seat, fs.list[clan].name + what + w.systems[fs.list[clan].seat].name); };

    if (Starts(t, "Raise tariffs") || Starts(t, "Cut tariffs")) {
        bool up = Starts(t, "Raise");
        float was = e.tariff[f.id];
        e.tariff[f.id] = (float)ClampD(was + (up ? 0.03 : -0.03), 0.0, 0.35);
        if (e.tariff[f.id] == was) return false;
        Report(w, f.id, f.seat, f.name + (up ? " raises its tariff to " : " cuts its tariff to ") + TextFormat("%.0f%%", e.tariff[f.id] * 100.0f));
        return true;
    }
    if (Starts(t, "Press its border") && other) {
        if (e.embargoUntil[f.id * F + other->id] > now) return false;
        e.embargoUntil[f.id * F + other->id] = now + 6.0 * day;
        Report(w, f.id, f.seat, f.name + " closes its borders to " + other->name + " trade");
        return true;
    }
    if ((Starts(t, "Issue letters of marque") || Starts(t, "Hire raiders")) && other) {
        int clan = NearestClan(fs, other->seatLy, f.id);
        if (clan == NO_FACTION) return false;
        e.raid[clan] = fminf(1.0f, e.raid[clan] + 0.4f);
        e.raidUntil[clan] = now + 5.0 * day;
        Report(w, f.id, fs.list[clan].seat, std::string(f.kind == FactionKind::Government ? "Privateers with " : "Raiders in the pay of ") + f.name +
                                            (f.kind == FactionKind::Government ? " papers" : "") + " are hitting " + other->name + " shipping");
        return true;
    }
    if (Starts(t, "Fund ") && other) {
        e.raid[other->id] = fminf(1.0f, e.raid[other->id] + 0.3f);
        e.raidUntil[other->id] = now + 5.0 * day;
        clanNews(other->id, " are out in force around ");
        return true;
    }
    if (Starts(t, "Hunt ") || Starts(t, "Patrol its trade lanes") || Starts(t, "Escort its convoys") || Starts(t, "Post bounties") ||
        Starts(t, "Keep the lanes") || Starts(t, "Drive ")) {
        int clan = other && other->kind == FactionKind::Clan ? other->id : NearestClan(fs, f.seatLy);
        if (clan == NO_FACTION || e.raid[clan] <= 0.06f) return false;
        e.raid[clan] = fmaxf(0.05f, e.raid[clan] - 0.3f);
        e.raidUntil[clan] = now + 4.0 * day;
        Report(w, f.id, fs.list[clan].seat, f.name + (f.kind == FactionKind::Clan ? " have driven off " : f.kind == FactionKind::Guild ? " hunters have scattered "
                                            : " patrols have scattered ") + fs.list[clan].name);
        return true;
    }
    if (Starts(t, "Raid ")) {
        e.raid[f.id] = fminf(1.0f, e.raid[f.id] + 0.35f);
        e.raidUntil[f.id] = now + 4.0 * day;
        clanNews(f.id, " are blockading the lanes around ");
        return true;
    }
    if (Starts(t, "Lie low")) {
        if (e.raid[f.id] <= 0.12f) return false;
        e.raid[f.id] = 0.1f;
        e.raidUntil[f.id] = now + 3.0 * day;
        clanNews(f.id, " have gone quiet around ");
        return true;
    }
    if (Starts(t, "Undercut ") && other) {
        e.pricing[f.id] = 0.85f;
        e.pricingUntil[f.id] = now + 5.0 * day;
        Report(w, f.id, f.seat, f.name + " starts a price war with " + other->name + ": 15% off at its stations");
        return true;
    }
    if ((Starts(t, "Court investment") || Starts(t, "Build more") || Starts(t, "Lobby ")) && other) {
        const Faction &company = f.kind == FactionKind::Conglomerate ? f : *other;
        const Faction &state   = f.kind == FactionKind::Conglomerate ? *other : f;
        if (e.outputUntil[company.id] > now) return false;
        e.output[company.id] = 1.3f;
        e.outputUntil[company.id] = now + 6.0 * day;
        Report(w, f.id, state.seat, state.name + " subsidises " + company.name + ": its plants are running flat out");
        return true;
    }
    if (Starts(t, "Seize ") && other) {
        int m = FindStation(e, f, other->id, f.id);
        if (m < 0) return false;
        e.state[m].owner = f.id;
        Report(w, f.id, e.base->defs[m].sys, f.name + " seizes " + e.base->defs[m].name + " from " + other->name);
        return true;
    }
    if (Starts(t, "Buy out independent")) {
        int m = FindStation(e, f, NO_FACTION, NO_FACTION);
        if (m < 0) return false;
        e.state[m].owner = f.id;
        Report(w, f.id, e.base->defs[m].sys, f.name + " buys " + e.base->defs[m].name);
        return true;
    }
    return false;                                                  // (pacts, claims, chapters: nothing to do about those yet)
}

static void FactionsAct(World &w)
{
    Economy &e = w.econ;
    const FactionSet &fs = w.systems.factions;
    for (const Faction &f : fs.list) {
        if (e.nextAct[f.id] == 0.0) e.nextAct[f.id] = w.clock + (1.0 + 6.0 * Roll(e)) * MARKET_DAY;   // (a staggered start)
        if (w.clock < e.nextAct[f.id]) continue;
        e.nextAct[f.id] = w.clock + (4.0 + 4.0 * Roll(e)) * MARKET_DAY;
        // Down the agenda, likelier the higher; not what it did last time
        std::vector<AgendaItem> agenda = FactionAgenda(fs, f.id, 8);
        for (int i = 0; i < (int)agenda.size(); i++) {
            int kind = (int)(NameHash(agenda[i].text.substr(0, 8)) & 0x7FFFFFFF);
            if (kind == e.lastAct[f.id] || Roll(e) < 0.35) continue;
            if (Act(w, f, agenda[i])) { e.lastAct[f.id] = kind; break; }
        }
    }
}

// Now and then something happens to a market that nobody planned
static void LocalShocks(World &w, double days)
{
    Economy &e = w.econ;
    const int N = e.Count();
    if (N == 0 || Roll(e) >= 0.0006 * N * days) return;      // (a couple a day, somewhere in human space)
    int m = (int)(Roll(e) * N) % N;
    const MarketDef &d = e.base->defs[m];
    if (d.sys == w.systems.home) return;                           // (the free port keeps an even keel)
    double until = w.clock + (3.0 + 3.0 * Roll(e)) * MARKET_DAY, r = Roll(e);
    const MarketState &ms = e.state[m];
    bool farms = ms.out[RAW_GRAIN] > 0.0f, mines = ms.out[ORE_IRON] > 0.0f || ms.out[ORE_SILICATES] > 0.0f;
    if (farms && r < 0.5)       { e.shocks.push_back({ m, 0.3f, 1.0f, until }); Report(w, NO_FACTION, d.sys, "The harvest has failed at " + d.name); }
    else if (mines && r < 0.5)  { e.shocks.push_back({ m, 2.0f, 1.0f, until }); Report(w, NO_FACTION, d.sys, "A rich strike at " + d.name + ": ore is pouring out"); }
    else if (r < 0.62)          { e.shocks.push_back({ m, 0.2f, 1.0f, until }); Report(w, NO_FACTION, d.sys, "Workers are out on strike at " + d.name); }
    else if (r < 0.74)          { e.shocks.push_back({ m, 0.4f, 0.6f, until }); Report(w, NO_FACTION, d.sys, "An accident has closed half the docks at " + d.name); }
    else if (r < 0.88)          { e.shocks.push_back({ m, 1.0f, 1.7f, until }); Report(w, NO_FACTION, d.sys, d.name + " is booming: it can't get enough of anything"); }
    else                        { e.shocks.push_back({ m, 1.0f, 0.5f, until }); Report(w, NO_FACTION, d.sys, "Hard times at " + d.name + ": nobody's buying"); }
}

// ---------------------------------------------------------------------------
// Running it
// ---------------------------------------------------------------------------
void EnsureEconomy(World &w)
{
    if (w.econ.Built()) return;
    w.econ.base = BuildBase(w.systems);
    InitState(w.econ, *w.econ.base, w.systems.factions);
    RunDays(w.econ, *w.econ.base, w.systems.factions, 1e-4, w.clock);     // (a moment's work, so there's something to say about how each industry is running)
    w.econ.simulated = w.clock;
    w.econ.dice = Mix64(w.seed ^ 0xEC0DEull);
}

void StepEconomy(World &w, double days)
{
    EnsureEconomy(w);
    RunDays(w.econ, *w.econ.base, w.systems.factions, days, w.clock, &w);
    if (w.clock - w.econ.developed >= DEVELOP_DAYS * MARKET_DAY || w.clock < w.econ.developed) Develop(w.econ, *w.econ.base, w.clock);
    FactionsAct(w);
    LocalShocks(w, days);
}

void UpdateEconomy(World &w)
{
    EnsureEconomy(w);
    double behind = w.clock - w.econ.simulated;
    if (behind < ECON_TICK) return;
    // Normally one tick; after a long jump of the clock, a few bigger ones
    int steps = (int)fmin(24.0, floor(behind / ECON_TICK));
    double each = behind / steps;
    for (int i = 0; i < steps; i++) StepEconomy(w, each / MARKET_DAY);
    w.econ.simulated = w.clock;
}

double RaidLevel(const World &w, DVec3 ly) { return w.econ.Built() ? RaidAt(w.econ, w.systems.factions, ly) : 0.0; }

double MarketPrice(const World &w, int market, int good)
{
    if (market < 0 || market >= w.econ.Count()) return 0.0;
    return PriceOf(w.econ.state[market], good, Pricing(w.econ, w.econ.state[market].owner, w.clock));
}

// ---------------------------------------------------------------------------
// Markets as captains see them
// ---------------------------------------------------------------------------
// The same sum a sale does (SellGoods), from what's on record
double Market::SellValue(int g, double qty) const
{
    if (!Buys(g) || qty <= 0.0) return 0.0;
    if (target[g] <= 0.0 || price[g] <= 0.0) return qty * SellPrice(g);
    auto curve = [&](double have) { return ClampD(pow(fmax(have, 0.02 * target[g]) / target[g], -0.6), PRICE_FLOOR, PRICE_CAP); };
    double scale = price[g] / curve(stock[g]) * 0.96 * (1.0 + edge) * (1.0 - tariff), total = 0.0;
    for (int s = 0; s < SLICES; s++) total += qty / SLICES * scale * curve(stock[g] + s * qty / SLICES);
    return total;
}

std::vector<int> Market::Listed() const
{
    std::vector<int> out;
    for (Role r : { Role::Export, Role::Import, Role::Trade })
        for (int g = 0; g < GOOD_COUNT; g++) if (role[g] == r) out.push_back(g);
    return out;
}

static std::string RoleLine(const Market &m, Role r, int max)
{
    // The made things before the dug things: they say more about a place
    std::vector<int> goods;
    for (int g = 0; g < GOOD_COUNT; g++) if (m.role[g] == r) goods.push_back(g);
    std::stable_sort(goods.begin(), goods.end(), [&](int a, int b) { return GoodTier(a) > GoodTier(b); });
    std::string s;
    for (int i = 0; i < (int)goods.size(); i++) {
        if (i == max) { s += ", ..."; break; }
        s += (i ? ", " : "") + std::string(GoodData(goods[i]).name);
    }
    return s;
}
std::string ExportsLine(const Market &m, int max) { return RoleLine(m, Role::Export, max); }
std::string ImportsLine(const Market &m, int max) { return RoleLine(m, Role::Import, max); }

// What a captain's standing with a station's owner is worth there: up to 3% either way
// (less than the spread: the best of friends still can't sell for more than they'd pay)
double StandingEdge(const Captain &c, int owner) { return 0.03 * ClampD(c.Standing(owner) / 100.0, -1.0, 1.0); }
static double Edge(const Captain &c, int owner) { return StandingEdge(c, owner); }

static Market LiveAt(const World &w, const Captain &c, int i)
{
    Market m;
    if (i < 0 || i >= w.econ.Count()) return m;
    const MarketDef &d = w.econ.base->defs[i];
    const MarketState &s = w.econ.state[i];
    for (int g = 0; g < GOOD_COUNT; g++) {
        m.role[g] = s.role[g];
        m.banned[g] = d.banned[g];
        m.price[g] = MarketPrice(w, i, g);
        m.stock[g] = s.stock[g];
        m.target[g] = s.target[g];
        // With the government's permit, what it bans is bought from you under licence (at a good price; never sold)
        if (d.banned[g] && (c.Papers(d.ruler) & PAPER_PERMIT)) { m.banned[g] = false; m.role[g] = Role::Import; m.price[g] = 1.4 * GoodData(g).basePrice; m.stock[g] = 0.0; }
    }
    m.owner = s.owner; m.ruler = d.ruler;
    m.tariff = TariffNow(w.econ, d.ruler);
    m.edge = Edge(c, s.owner);
    m.fuelPrice = m.Sells(c.player.spec.fuelGood) ? m.BuyPrice(c.player.spec.fuelGood) : 0.0;      // (the fuel this captain's reactor burns)
    return m;
}

Market LiveMarket(const World &w, const Captain &c, const Body &b) { return LiveAt(w, c, w.econ.MarketOf(b.id)); }

// A station over a world people live on is part of that world's economy: its records are the world's
uint64_t EconomyBody(const Captain &c, const Body &b)
{
    if (b.kind != Kind::Station || b.parent <= 0 || b.parent >= (int)c.loaded.bodies.size()) return b.id;
    const Body &p = c.loaded.bodies[b.parent];
    return p.kind != Kind::Star && p.population > 0 ? p.id : b.id;
}

bool KnownMarket(const Captain &c, const Body &b, Market &m, double *at)
{
    auto it = c.know.prices.find(EconomyBody(c, b));
    if (it == c.know.prices.end()) return false;
    const PriceRecord &r = it->second;
    m = Market();
    for (int g = 0; g < GOOD_COUNT; g++) {
        m.role[g] = (Role)(r.role[g] & 127);
        m.banned[g] = (r.role[g] & 128) != 0;
        m.price[g] = r.price[g];
        m.stock[g] = r.stock[g];
        m.target[g] = r.target[g];
    }
    m.owner = r.owner; m.ruler = r.ruler; m.tariff = r.tariff;
    m.edge = Edge(c, r.owner);
    m.fuelPrice = m.Sells(c.player.spec.fuelGood) ? m.BuyPrice(c.player.spec.fuelGood) : 0.0;
    if (at) *at = r.at;
    return true;
}

static void RecordAt(World &w, Captain &c, int i)
{
    if (i < 0 || i >= w.econ.Count()) return;
    const MarketDef &d = w.econ.base->defs[i];
    Market m = LiveAt(w, c, i);
    PriceRecord &r = c.know.prices[d.bodyId];
    r.at = w.clock; r.sys = d.sys; r.name = d.name; r.owner = m.owner; r.ruler = m.ruler; r.tariff = (float)m.tariff;
    for (int g = 0; g < GOOD_COUNT; g++) {
        r.price[g] = (float)m.price[g];
        r.stock[g] = (float)m.stock[g];
        r.target[g] = (float)m.target[g];
        r.role[g]  = (uint8_t)((int)m.role[g] | (m.banned[g] ? 128 : 0));
    }
}

void RecordPrices(World &w, Captain &c, const Body &b)
{
    if (w.econ.Built()) RecordAt(w, c, w.econ.MarketOf(b.id));
}

const Body *TradePartner(const Captain &c)
{
    if (c.flight.nearBody < 0 || c.flight.nearBody >= (int)c.loaded.bodies.size()) return nullptr;
    const Body &b = c.loaded.bodies[c.flight.nearBody];
    return b.population > 0 ? &b : nullptr;
}

// ---------------------------------------------------------------------------
// Trading
// ---------------------------------------------------------------------------
static void Tell(World &w, Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

// Doing business with someone makes you known to them: a little to whoever owns the place, less to whoever rules it
static void GoodCustomer(World &w, Captain &c, int market, double value)
{
    auto bump = [&](int faction, double by) {
        if (faction < 0 || faction >= w.systems.factions.Count()) return;
        if ((int)c.standing.size() < w.systems.factions.Count()) c.standing.resize(w.systems.factions.Count(), 0.0f);
        c.standing[faction] = (float)fmin(100.0, c.standing[faction] + by);
    };
    int owner = w.econ.state[market].owner, ruler = w.econ.base->defs[market].ruler;
    bump(owner, value / 40000.0);
    if (ruler != owner) bump(ruler, value / 120000.0);
}

// The market the captain is docked at, or -1 (with a word about why)
static int Docked(World &w, Captain &c, const Body *&b)
{
    b = TradePartner(c);
    if (!b) return -1;
    EnsureEconomy(w);
    int i = w.econ.MarketOf(b->id);
    if (i < 0) Tell(w, c, b->name + " has no market");
    return i;
}


bool SellGoods(World &w, Captain &c, int good, double qty)
{
    const Body *b;
    int i = Docked(w, c, b);
    if (i < 0 || good < 0 || good >= GOOD_COUNT) return false;
    Player &p = c.player;
    Market m = LiveAt(w, c, i);
    if (m.banned[good]) { Tell(w, c, std::string(GoodData(good).name) + " is contraband here: " + b->name + " won't touch it"); return false; }
    if (!m.Buys(good))  { Tell(w, c, b->name + " doesn't trade in " + GoodData(good).name); return false; }
    qty = fmin(qty, p.cargo[good]);
    if (qty <= 1e-6) { Tell(w, c, std::string("No ") + GoodData(good).name + " in the hold"); return false; }
    double paid = 0.0;
    for (int s = 0; s < SLICES; s++) {
        paid += qty / SLICES * LiveAt(w, c, i).SellPrice(good);
        w.econ.state[i].stock[good] += (float)(qty / SLICES);
    }
    p.cargo[good] -= qty;
    if (p.cargo[good] < 1e-6) p.cargo[good] = 0.0;
    p.credits += paid;
    if (m.tariff > 0.0 && m.tariff < 1.0) Pay(w.econ, i, m.ruler, paid * m.tariff / (1.0 - m.tariff), INC_TARIFF);      // (what the tariff took of it is the government's)
    GoodCustomer(w, c, i, paid);
    RecordAt(w, c, i);
    w.Emit(EventType::SoldGoods, TextFormat("Sold %.1f t %s for %s cr", qty, GoodData(good).name, Thousands(paid).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), good, qty, paid, c.id);
    return true;
}

// Buys up to 'qty' of a good at market i for at most 'credits'; returns the tonnes and the cost
static double Purchase(World &w, const Captain &c, int i, int good, double qty, double credits, double &cost)
{
    cost = 0.0;
    double got = 0.0;
    for (int s = 0; s < SLICES; s++) {
        Market m = LiveAt(w, c, i);
        double q = fmin(qty / SLICES, fmax(0.0, m.stock[good] - 0.01));
        double price = m.BuyPrice(good);
        if (price <= 0.0) break;
        q = fmin(q, (credits - cost) / price);
        if (q <= 1e-9) break;
        cost += q * price;
        got  += q;
        w.econ.state[i].stock[good] -= (float)q;
    }
    return got;
}

bool BuyGoods(World &w, Captain &c, int good, double qty)
{
    const Body *b;
    int i = Docked(w, c, b);
    if (i < 0 || good < 0 || good >= GOOD_COUNT) return false;
    Player &p = c.player;
    Market m = LiveAt(w, c, i);
    if (!m.Sells(good)) { Tell(w, c, b->name + " has no " + GoodData(good).name + " for sale"); return false; }
    double room = p.spec.hold - p.CargoTotal();
    if (room <= 1e-6) { Tell(w, c, "Cargo hold full"); return false; }
    double cost, got = Purchase(w, c, i, good, fmin(qty, room), p.credits, cost);
    if (got <= 1e-6)  { Tell(w, c, "Not enough credits"); return false; }
    p.cargo[good] += got;
    p.credits -= cost;
    GoodCustomer(w, c, i, cost);
    RecordAt(w, c, i);
    w.Emit(EventType::BoughtGoods, TextFormat("Bought %.1f t %s for %s cr", got, GoodData(good).name, Thousands(cost).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), good, got, cost, c.id);
    return true;
}

bool BuyFuel(World &w, Captain &c, double qty)
{
    const Body *b;
    int i = Docked(w, c, b);
    if (i < 0) return false;
    Player &p = c.player;
    const int fuel = p.spec.fuelGood;
    if (!LiveAt(w, c, i).Sells(fuel)) { Tell(w, c, b->name + " has no " + GoodData(fuel).name + " for your reactor"); return false; }
    double room = p.spec.tank - p.fuel;
    if (room <= 1e-6)  { Tell(w, c, "Tank already full");  return false; }
    double cost, got = Purchase(w, c, i, fuel, fmin(qty, room), p.credits, cost);
    if (got <= 1e-6)   { Tell(w, c, "Not enough credits"); return false; }
    p.fuel    += got;
    p.credits -= cost;
    GoodCustomer(w, c, i, cost);
    RecordAt(w, c, i);
    w.Emit(EventType::BoughtFuel, TextFormat("Bought %.1f t %s for %s cr", got, GoodData(fuel).name, Thousands(cost).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), -1, got, cost, c.id);
    return true;
}

bool BuyTradeData(World &w, Captain &c)
{
    const Body *b;
    int i = Docked(w, c, b);
    if (i < 0) return false;
    if (c.player.credits < TRADE_DATA_COST) { Tell(w, c, "Not enough credits"); return false; }
    // Its own system, and every system its hub has a lane to
    const EconBase &base = *w.econ.base;
    int hub = base.defs[i].hub;
    std::vector<int> hubs = { hub };
    for (const Lane &l : base.lanes) { if (l.a == hub) hubs.push_back(l.b); if (l.b == hub) hubs.push_back(l.a); }
    int markets = 0;
    for (int m = 0; m < w.econ.Count(); m++)
        if (std::find(hubs.begin(), hubs.end(), base.defs[m].hub) != hubs.end()) { RecordAt(w, c, m); markets++; }
    c.player.credits -= TRADE_DATA_COST;
    Tell(w, c, TextFormat("Bought trade data: %d markets in %d systems", markets, (int)hubs.size()));
    return true;
}

// ---------------------------------------------------------------------------
// A world's economy as a captain sees it, and putting money into it
// ---------------------------------------------------------------------------
// What funding the next addition to an industry would cost a captain: the materials at what the market asks for
// them, and the builders' fee. 0: it can't be done just now (something is already being built, there isn't the
// labour to run more, or the materials aren't to be had).
static double FundingCost(const World &w, const Captain &c, int i, int k, double *stepOut = nullptr)
{
    const MarketDef &d = w.econ.base->defs[i];
    const MarketState &s = w.econ.state[i];
    if (k < 0 || k >= (int)s.ind.size() || IsCivic(d.ind[k].type)) return 0.0;
    double step = UpgradeStep(s.ind[k], d);
    if (stepOut) *stepOut = step;
    for (const IndustryState &o : s.ind) if (o.building > 0.0f) return 0.0;
    if (!CanUpgrade(d, s, k, step, 0.0)) return 0.0;
    // (the builders' fee: what the addition should earn in a few weeks at usual prices)
    double usual = 0.0;
    for (int p = 0; p < d.ind[k].outs; p++) usual += d.ind[k].out[p].share * w.econ.base->perCap[d.ind[k].out[p].good] * GoodData(d.ind[k].out[p].good).basePrice;
    double need[3], cost = fmax(INVEST_FEE, INVEST_DAYS * (1.0 - INDUSTRY_COST) * usual) * step;
    UpgradeNeeds(d.ind[k].type, step, need);
    for (int m = 0; m < 3; m++) cost += need[m] * PriceOf(s, UPGRADE_GOODS[m], Pricing(w.econ, s.owner, w.clock)) * 1.04 * (1.0 - StandingEdge(c, s.owner));
    return cost * BuiltUp(d, s);
}

static Stake *FindStake(Captain &c, uint64_t body, int type)
{
    for (Stake &st : c.stakes) if (st.body == body && st.type == type) return &st;
    return nullptr;
}

void RecordWorld(World &w, Captain &c, const Body &b)
{
    if (!w.econ.Built()) return;
    int i = w.econ.MarketOf(b.id);
    if (i < 0) return;
    const MarketDef &d = w.econ.base->defs[i];
    const MarketState &s = w.econ.state[i];
    WorldRecord &r = c.know.worlds[d.bodyId];
    r.at = w.clock; r.name = d.name; r.pop = s.pop;
    r.labour = s.labour; r.employed = s.employed; r.housing = s.housing; r.cellsMade = s.cellsMade; r.cellsUsed = s.cellsUsed;
    r.living = s.living; r.trend = s.trend; r.refugees = s.refugees; r.leaving = s.leaving; r.stuck = s.stuck;
    r.border = d.ruler >= 0 && d.ruler < (int)w.econ.border.size() ? w.econ.border[d.ruler] : (uint8_t)BORDER_OPEN;
    r.goingTo = s.bestTo >= 0 && s.leaving > 0.5f ? w.econ.base->defs[s.bestTo].name : std::string();
    r.barredAt = s.barredTo >= 0 ? w.systems.factions.Name(w.econ.base->defs[s.barredTo].ruler) : std::string();
    for (int role = 0; role < ROLE_COUNT; role++) {
        r.fleet[role] = 0.0f; r.busy[role] = 0.0f;
        if (d.hub < 0 || (size_t)d.hub >= w.econ.fleets.size()) continue;         // (the ships are based at its system's hub)
        for (const FleetAt &fl : w.econ.fleets[d.hub]) r.fleet[role] += fl.ships[role];
        r.busy[role] = w.econ.busy[(size_t)d.hub * ROLE_COUNT + role];
    }
    r.rows.clear();
    for (int k = 0; k < (int)s.ind.size() && k < (int)d.ind.size(); k++) {
        const IndustryState &st = s.ind[k];
        IndustryRow row;
        row.type = d.ind[k].type; row.owner = st.owner; row.site = d.ind[k].site; row.cap = st.cap; row.run = st.run; row.output = st.output;
        row.profit = st.profit; row.building = st.building; row.buildLeft = st.buildLeft; row.shortOf = st.shortOf;
        if (const Stake *mine = FindStake(c, d.bodyId, row.type)) {
            row.stake = mine->cap;
            row.pending = (float)fmax(0.0, mine->cap * (st.earned - mine->mark));
        }
        double step = 0.0;
        row.cost = (float)FundingCost(w, c, i, k, &step);
        row.step = (float)step;
        r.rows.push_back(row);
    }
}

static void TellCaptain(World &w, Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

bool InvestIn(World &w, Captain &c, int industry)
{
    const Body *b = TradePartner(c);
    if (!b) { TellCaptain(w, c, "Dock at the world to put money into its industry"); return false; }
    EnsureEconomy(w);
    int i = w.econ.MarketOf(b->id);
    if (i < 0) return false;
    const MarketDef &d = w.econ.base->defs[i];
    MarketState &s = w.econ.state[i];
    if (industry < 0 || industry >= (int)s.ind.size()) return false;
    const char *what = IndustryData(d.ind[industry].type).name;
    double step = 0.0, cost = FundingCost(w, c, i, industry, &step);
    if (cost <= 0.0) {
        bool busy = false;
        for (const IndustryState &o : s.ind) if (o.building > 0.0f) busy = true;
        TellCaptain(w, c, busy ? d.name + " is already building: one thing at a time"
                               : std::string("Nothing can be added to ") + what + " at " + d.name + " now: it wants the labour to run it, and steel and machinery to build it");
        return false;
    }
    if (c.player.credits < cost) { TellCaptain(w, c, TextFormat("Not enough credits: adding to %s takes %s cr", what, Thousands(cost).c_str())); return false; }
    c.player.credits -= cost;
    StartUpgrade(s, d, industry, step);
    Stake *mine = FindStake(c, d.bodyId, d.ind[industry].type);
    if (!mine) { c.stakes.push_back(Stake()); mine = &c.stakes.back(); mine->body = d.bodyId; mine->type = d.ind[industry].type; mine->mark = s.ind[industry].earned; }
    else {
        // (what the old stake has earned so far is kept: the mark moves so that it comes to the same on the bigger one)
        double owed = mine->cap * (s.ind[industry].earned - mine->mark);
        mine->mark = s.ind[industry].earned - owed / (mine->cap + step);
    }
    mine->cap += (float)step;
    // (it makes you known to whoever's works they are)
    int owner = s.ind[industry].owner;
    if (owner >= 0 && owner < w.systems.factions.Count()) {
        if ((int)c.standing.size() < w.systems.factions.Count()) c.standing.resize(w.systems.factions.Count(), 0.0f);
        c.standing[owner] = (float)fmin(100.0, c.standing[owner] + cost / 20000.0);
    }
    TellCaptain(w, c, TextFormat("Funded %s at %s for %s cr: %.1f more capacity in %.0f days, and a share of what it earns", what, d.name.c_str(),
                                 Thousands(cost).c_str(), step, UPGRADE_DAYS));
    RecordWorld(w, c, *b);
    return true;
}

double CollectStakes(World &w, Captain &c, const Body &b)
{
    if (!w.econ.Built() || c.stakes.empty()) return 0.0;
    int i = w.econ.MarketOf(b.id);
    if (i < 0) return 0.0;
    const MarketDef &d = w.econ.base->defs[i];
    const MarketState &s = w.econ.state[i];
    double paid = 0.0;
    for (Stake &st : c.stakes) {
        if (st.body != d.bodyId) continue;
        for (int k = 0; k < (int)s.ind.size() && k < (int)d.ind.size(); k++) {
            if (d.ind[k].type != st.type) continue;
            double owed = st.cap * (s.ind[k].earned - st.mark);
            st.mark = s.ind[k].earned;                             // (a loss isn't carried: a stake can only pay, or not)
            if (owed > 0.0) paid += owed;
            break;
        }
    }
    if (paid >= 1.0) {
        c.player.credits += paid;
        TellCaptain(w, c, TextFormat("%s paid out %s cr on your stakes in its industry", d.name.c_str(), Thousands(paid).c_str()));
    } else paid = 0.0;
    return paid;
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
void WriteEconomy(const World &w, std::string &out)
{
    const Economy &e = w.econ;
    if (!e.Built()) return;
    const int F = w.systems.factions.Count();
    out += TextFormat("econ %.17g %llu\n", e.simulated, (unsigned long long)e.dice);
    for (int i = 0; i < e.Count(); i++) {
        const MarketDef &d = e.base->defs[i];
        std::string line = TextFormat("stock %llu", (unsigned long long)d.bodyId);
        for (int g = 0; g < GOOD_COUNT; g++)                       // (with what's on its way there: a save has no ships in flight)
            line += TextFormat(" %.6g", e.state[i].stock[g] + (e.afloat.empty() ? 0.0f : e.afloat[(size_t)i * GOOD_COUNT + g]));
        out += line + "\n";
        if (e.state[i].owner != d.owner) out += TextFormat("owner %llu %d\n", (unsigned long long)d.bodyId, e.state[i].owner);
        out += TextFormat("people %llu %.17g %.9g\n", (unsigned long long)d.bodyId, e.state[i].pop, e.state[i].refugees);
        for (size_t k = 0; k < e.state[i].ind.size() && k < d.ind.size(); k++) {
            const IndustryState &st = e.state[i].ind[k];
            if (st.cap != d.ind[k].cap || st.building > 0.0f || st.earned != 0.0 || st.owner != d.ind[k].owner)
                out += TextFormat("industry %llu %d %d %.9g %.9g %.9g %.17g %d\n", (unsigned long long)d.bodyId, (int)k, (int)d.ind[k].type, st.cap,
                                  st.building, st.buildLeft, st.earned, (int)st.owner);
        }
    }
    for (int f = 0; f < F && f < (int)e.border.size(); f++) out += TextFormat("border %d %d\n", f, (int)e.border[f]);
    for (int i = 0; i < e.Count() && i < (int)e.fleets.size(); i++)
        for (const FleetAt &fl : e.fleets[i])
            out += TextFormat("fleet %llu %d %.9g %.9g %.9g %.9g\n", (unsigned long long)e.base->defs[i].bodyId, (int)fl.owner, fl.ships[0], fl.ships[1], fl.ships[2], fl.ships[3]);
    for (int f = 0; f < F && f < (int)e.clanShips.size(); f++) if (e.clanShips[f] > 0.0f) out += TextFormat("clanships %d %.9g\n", f, e.clanShips[f]);
    for (int f = 0; f < F && f < (int)e.money.size(); f++) out += TextFormat("money %d %.17g %.9g %.9g\n", f, e.money[f].treasury, e.money[f].squeeze, e.money[f].usual);
    for (int i = 0; i < e.Count(); i++)
        for (int f = 0; f < F; f++) {
            double there = e.Coffer(i, f) + e.Moving(i, f);         // (with what's passing through, and what's waiting for a ship to it: a save has no ships in flight)
            for (const Specie &s : e.specie) if (s.to == i && s.faction == f) there += s.credits;
            if (there >= 1.0) out += TextFormat("coffer %llu %d %.17g\n", (unsigned long long)e.base->defs[i].bodyId, f, there);
        }
    for (int f = 0; f < F; f++) {
        out += TextFormat("faction %d %.9g %.9g %.17g %.9g %.17g %.9g %.17g %.17g\n", f, e.tariff[f], e.raid[f], e.raidUntil[f],
                          e.output[f], e.outputUntil[f], e.pricing[f], e.pricingUntil[f], e.nextAct[f]);
        for (int o = 0; o < F; o++)
            if (e.embargoUntil[f * F + o] > w.clock) out += TextFormat("embargo %d %d %.17g\n", f, o, e.embargoUntil[f * F + o]);
    }
    for (const Shock &s : e.shocks)
        out += TextFormat("shock %llu %.9g %.9g %.17g\n", (unsigned long long)e.base->defs[s.market].bodyId, s.supply, s.demand, s.until);
    for (const EconNews &n : w.news) out += TextFormat("news %.17g %d %lld %s\n", n.at, n.faction, (long long)n.where, n.text.c_str());
}

bool ReadEconomyLine(World &w, const char *key, const char *rest)
{
    Economy &e = w.econ;
    const int F = w.systems.factions.Count();
    unsigned long long id;
    if (!strcmp(key, "econ")) {
        unsigned long long dice;
        if (sscanf(rest, "%lf %llu", &e.simulated, &dice) == 2) e.dice = dice;
        return true;
    }
    if (!strcmp(key, "stock")) {
        int used = 0;
        if (sscanf(rest, "%llu%n", &id, &used) != 1) return true;
        int i = e.MarketOf(id);
        const char *at = rest + used;
        for (int g = 0; i >= 0 && g < GOOD_COUNT; g++) {
            float v; int n = 0;
            if (sscanf(at, "%f%n", &v, &n) != 1) break;
            e.state[i].stock[g] = fmaxf(0.0f, v);
            at += n;
        }
        return true;
    }
    if (!strcmp(key, "people")) {
        double pop; float refugees = 0.0f;
        if (sscanf(rest, "%llu %lf %f", &id, &pop, &refugees) >= 2 && e.MarketOf(id) >= 0 && pop > 0.0) {
            MarketState &ms = e.state[e.MarketOf(id)];
            ms.pop = ms.popDerived = pop; ms.refugees = fmaxf(0.0f, refugees);
            Derive(*e.base, e.MarketOf(id), ms);
        }
        return true;
    }
    if (!strcmp(key, "industry")) {
        int k, type, owner; float cap, building, left; double earned;
        if (sscanf(rest, "%llu %d %d %f %f %f %lf %d", &id, &k, &type, &cap, &building, &left, &earned, &owner) == 8 && e.MarketOf(id) >= 0) {
            int i = e.MarketOf(id);
            if (k >= 0 && k < (int)e.state[i].ind.size() && e.base->defs[i].ind[k].type == type && cap >= 0.0f && owner >= NO_FACTION && owner < F) {
                IndustryState &st = e.state[i].ind[k];
                st.cap = cap; st.building = fmaxf(0.0f, building); st.buildLeft = left; st.earned = earned; st.owner = (int16_t)owner;
                Derive(*e.base, i, e.state[i]);
            }
        }
        return true;
    }
    if (!strcmp(key, "owner")) {
        int owner;
        if (sscanf(rest, "%llu %d", &id, &owner) == 2 && e.MarketOf(id) >= 0 && owner >= NO_FACTION && owner < F) e.state[e.MarketOf(id)].owner = owner;
        return true;
    }
    if (!strcmp(key, "faction")) {
        int f; float tariff, raid, output, pricing; double raidUntil, outputUntil, pricingUntil, next;
        if (sscanf(rest, "%d %f %f %lf %f %lf %f %lf %lf", &f, &tariff, &raid, &raidUntil, &output, &outputUntil, &pricing, &pricingUntil, &next) == 9 &&
            f >= 0 && f < F) {
            e.tariff[f] = tariff; e.raid[f] = raid; e.raidUntil[f] = raidUntil; e.output[f] = output; e.outputUntil[f] = outputUntil;
            e.pricing[f] = pricing; e.pricingUntil[f] = pricingUntil; e.nextAct[f] = next;
        }
        return true;
    }
    if (!strcmp(key, "fleet")) {
        int owner; FleetAt fl;
        if (sscanf(rest, "%llu %d %f %f %f %f", &id, &owner, &fl.ships[0], &fl.ships[1], &fl.ships[2], &fl.ships[3]) == 6 && e.MarketOf(id) >= 0 && owner >= NO_FACTION && owner < F) {
            std::vector<FleetAt> &at = e.fleets[e.MarketOf(id)];
            if (!e.fleetsRead) { for (auto &v : e.fleets) v.clear(); e.fleetsRead = true; }       // (the saved fleets replace the ones a new galaxy starts with)
            fl.owner = (int16_t)owner;
            at.push_back(fl);
        }
        return true;
    }
    if (!strcmp(key, "money")) {
        int f; double treasury; float squeeze, usual = 0.0f;
        if (sscanf(rest, "%d %lf %f %f", &f, &treasury, &squeeze, &usual) >= 3 && f >= 0 && f < (int)e.money.size() && treasury >= 0.0 && squeeze > 0.0f) {
            e.money[f].treasury = treasury; e.money[f].squeeze = squeeze; e.money[f].usual = fmaxf(0.0f, usual);
        }
        return true;
    }
    if (!strcmp(key, "coffer")) {
        unsigned long long id; int f; double credits;
        if (sscanf(rest, "%llu %d %lf", &id, &f, &credits) == 3 && f >= 0 && f < (int)e.money.size() && credits >= 0.0) {
            int i = e.MarketOf(id);
            if (i >= 0 && ((size_t)i + 1) * e.money.size() <= e.coffer.size()) e.coffer[(size_t)i * e.money.size() + f] = credits;
        }
        return true;
    }
    if (!strcmp(key, "clanships")) {
        int f; float n;
        if (sscanf(rest, "%d %f", &f, &n) == 2 && f >= 0 && f < (int)e.clanShips.size()) e.clanShips[f] = n;
        return true;
    }
    if (!strcmp(key, "border")) {
        int f, policy;
        if (sscanf(rest, "%d %d", &f, &policy) == 2 && f >= 0 && f < (int)e.border.size() && policy >= BORDER_OPEN && policy <= BORDER_CLOSED) e.border[f] = (uint8_t)policy;
        return true;
    }
    if (!strcmp(key, "embargo")) {
        int a, b; double until;
        if (sscanf(rest, "%d %d %lf", &a, &b, &until) == 3 && a >= 0 && b >= 0 && a < F && b < F) e.embargoUntil[a * F + b] = until;
        return true;
    }
    if (!strcmp(key, "shock")) {
        Shock s;
        if (sscanf(rest, "%llu %f %f %lf", &id, &s.supply, &s.demand, &s.until) == 4 && (s.market = e.MarketOf(id)) >= 0) e.shocks.push_back(s);
        return true;
    }
    if (!strcmp(key, "news")) {
        EconNews n; long long where; int used = 0;
        if (sscanf(rest, "%lf %d %lld %n", &n.at, &n.faction, &where, &used) >= 3) {
            n.where = where;
            n.text = rest + used;
            while (!n.text.empty() && (n.text.back() == '\n' || n.text.back() == '\r')) n.text.pop_back();
            n.seq = ++w.newsSeq;
            w.news.push_back(n);
        }
        return true;
    }
    return false;
}

void WriteStakes(const Captain &c, std::string &out)
{
    for (const Stake &st : c.stakes) out += TextFormat("stake %llu %d %.9g %.17g\n", (unsigned long long)st.body, (int)st.type, st.cap, st.mark);
}

void WritePrices(const Captain &c, std::string &out)
{
    WriteStakes(c, out);
    for (const auto &kv : c.know.prices) {
        const PriceRecord &r = kv.second;
        std::string line = TextFormat("prices %llu %.17g %lld %d %d %.6g", (unsigned long long)kv.first, r.at, (long long)r.sys, r.owner, r.ruler, r.tariff);
        for (int g = 0; g < GOOD_COUNT; g++) line += TextFormat(" %d %.9g %.9g", (int)r.role[g], r.price[g], r.stock[g]);
        out += line + " | " + r.name + "\n";
        line = TextFormat("wants %llu", (unsigned long long)kv.first);
        for (int g = 0; g < GOOD_COUNT; g++) line += TextFormat(" %.9g", r.target[g]);
        out += line + "\n";
    }
}

bool ReadStakeLine(Captain &c, const char *key, const char *rest)
{
    if (!strcmp(key, "stake")) {
        unsigned long long body; int type; Stake st;
        if (sscanf(rest, "%llu %d %f %lf", &body, &type, &st.cap, &st.mark) == 4 && type >= 0 && type < IND_COUNT && st.cap > 0.0f) {
            st.body = body; st.type = (uint8_t)type;
            c.stakes.push_back(st);
        }
        return true;
    }
    return false;
}

bool ReadPriceLine(Captain &c, const char *key, const char *rest)
{
    if (ReadStakeLine(c, key, rest)) return true;
    if (!strcmp(key, "wants")) {                                   // (follows its 'prices' line)
        unsigned long long id; int used = 0;
        if (sscanf(rest, "%llu%n", &id, &used) != 1 || !c.know.prices.count(id)) return true;
        PriceRecord &r = c.know.prices[id];
        const char *at = rest + used;
        for (int g = 0; g < GOOD_COUNT; g++) {
            int n = 0;
            if (sscanf(at, "%f%n", &r.target[g], &n) != 1) break;
            at += n;
        }
        return true;
    }
    if (strcmp(key, "prices")) return false;
    PriceRecord r;
    unsigned long long id; long long sys; int used = 0;
    if (sscanf(rest, "%llu %lf %lld %d %d %f%n", &id, &r.at, &sys, &r.owner, &r.ruler, &r.tariff, &used) != 6) return true;
    r.sys = sys;
    const char *at = rest + used;
    for (int g = 0; g < GOOD_COUNT; g++) {
        int role, n = 0;
        if (sscanf(at, "%d %f %f%n", &role, &r.price[g], &r.stock[g], &n) != 3) return true;
        r.role[g] = (uint8_t)role;
        at += n;
    }
    if (const char *bar = strstr(at, "| ")) r.name = bar + 2;
    while (!r.name.empty() && (r.name.back() == '\n' || r.name.back() == '\r')) r.name.pop_back();
    c.know.prices[id] = r;
    return true;
}
