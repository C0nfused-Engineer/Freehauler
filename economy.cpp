// economy.cpp - building the markets, running the economy, and trading
#include "economy.h"
#include "format.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

static double ClampD(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static double Roll(Economy &e) { e.dice = Mix64(e.dice); return (double)(e.dice >> 11) * (1.0 / 9007199254740992.0); }

// ---------------------------------------------------------------------------
// What each kind of place makes and uses, as shares of its scale
// ---------------------------------------------------------------------------
static bool IsPort(Settlement s)
{
    return s == Settlement::OrbitalPort || s == Settlement::Starbase || s == Settlement::Waystation || s == Settlement::Freeport;
}

static void Industry(MarketDef &d, const Body &b, const Body *ground, const Faction *owner)
{
    auto make = [&](int g, double share) { d.out[g] += (float)(d.scale * share); };
    auto need = [&](int g, double share) { d.use[g] += (float)(d.scale * share); };
    // What's in the ground: a station works what its world has
    const float *res = ground ? ground->resource : b.resource;
    float metals = res[RES_METALS], precious = res[RES_PRECIOUS], minerals = res[RES_MINERALS], ices = res[RES_ICES], bio = res[RES_BIOMASS];
    if (!ground && b.kind == Kind::Station) { metals = minerals = 0.5f; precious = 0.15f; ices = 0.3f; }

    // Everybody eats, drinks and burns fuel; people on worlds also shop
    if (b.kind == Kind::Station) { need(REF_FOOD, 0.10); need(REF_WATER, 0.04); need(REF_FUEL, 0.06); need(GOOD_LIQUOR, 0.01); }
    else { need(REF_FOOD, 0.25); need(REF_WATER, 0.08); need(GOOD_CONSUMER, 0.06); need(GOOD_MEDICINES, 0.02);
           need(GOOD_LIQUOR, 0.015); need(REF_FUEL, 0.03); need(GOOD_ELECTRONICS, 0.012); need(GOOD_LUXURY, 0.002); }
    if (b.settlement == Settlement::MiningOutpost || b.settlement == Settlement::OreRefinery || b.settlement == Settlement::UndergroundColony ||
        b.settlement == Settlement::OreTerminal) { need(GOOD_ROBOTICS, 0.006); need(GOOD_ELECTRONICS, 0.01); }

    switch (b.settlement) {
    case Settlement::CoreWorld:
        make(GOOD_ELECTRONICS, 0.10); make(GOOD_ROBOTICS, 0.04); make(GOOD_INSTRUMENTS, 0.03); make(GOOD_MEDICINES, 0.06);
        make(GOOD_CONSUMER, 0.14); make(GOOD_LUXURY, 0.02); make(GOOD_SHIP_COMPONENTS, 0.03); make(GOOD_MACHINERY, 0.05);
        make(GOOD_WEAPONS, 0.015); make(GOOD_REACTOR_CORES, 0.015); make(REF_CHEMICALS, 0.05); make(REF_FERTILIZER, 0.04);
        make(REF_WATER, 0.06);
        make(RAW_GRAIN, 0.18); make(RAW_LIVESTOCK, 0.045); make(REF_FOOD, 0.20);     // (it feeds itself, mostly)
        need(GOOD_LUXURY, 0.006);
        break;
    case Settlement::SettledWorld:
        make(RAW_GRAIN, 0.55); make(RAW_LIVESTOCK, 0.15); make(REF_FOOD, 0.45); make(GOOD_LIQUOR, 0.05); make(REF_WATER, 0.12);
        make(RAW_FLORA, 0.012 * (0.3 + bio));
        need(REF_FERTILIZER, 0.05); need(GOOD_MACHINERY, 0.02);
        break;
    case Settlement::DomedColonies:
        make(REF_POLYMERS, 0.05); make(REF_WATER, 0.05 + 0.05 * ices); make(RAW_FLORA, 0.004 * bio);
        need(GOOD_HABITAT_MODULES, 0.02); need(GOOD_MACHINERY, 0.02);
        break;
    case Settlement::OrbitalHabitats: case Settlement::SkimmerStation:
        make(RAW_HELIUM3, 0.10); make(REF_FUEL, 0.60);
        need(GOOD_MACHINERY, 0.02);
        break;
    case Settlement::MiningOutpost: case Settlement::OreTerminal:
        make(ORE_IRON, 0.50 * metals); make(ORE_COPPER, 0.25 * metals); make(ORE_TITANIUM, 0.15 * metals);
        make(ORE_PRECIOUS, 0.05 * precious); make(ORE_RARE_EARTHS, 0.08 * minerals); make(ORE_URANIUM, 0.05 * minerals);
        make(ORE_SILICATES, 0.40 * minerals); make(ORE_CARBON, 0.25 * (1.0f - metals) + 0.05);
        make(ORE_WATER_ICE, 0.50 * ices); make(ORE_VOLATILES, 0.30 * ices);
        need(GOOD_MACHINERY, 0.03);
        break;
    case Settlement::OreRefinery:
        // (its own miners work the belt around it)
        make(ORE_IRON, 0.30); make(ORE_SILICATES, 0.20); make(ORE_CARBON, 0.20); make(ORE_COPPER, 0.10); make(ORE_WATER_ICE, 0.10);
        make(REF_STEEL, 0.30); make(REF_ALLOYS, 0.12); make(REF_CONDUCTORS, 0.12); make(REF_SILICON, 0.10); make(REF_CERAMICS, 0.15);
        make(REF_POLYMERS, 0.10); make(REF_WATER, 0.08);
        need(GOOD_MACHINERY, 0.03);
        break;
    case Settlement::UndergroundColony:
        make(REF_STEEL, 0.15); make(GOOD_MACHINERY, 0.12); make(GOOD_HULL_PLATING, 0.08); make(GOOD_HABITAT_MODULES, 0.06);
        make(REF_CHEMICALS, 0.08); make(REF_FERTILIZER, 0.06); make(REF_REACTOR_FUEL, 0.02); make(GOOD_MUNITIONS, 0.04);
        make(GOOD_WEAPONS, 0.02); make(GOOD_REACTOR_CORES, 0.02); make(REF_WATER, 0.04 * ices);
        break;
    case Settlement::Shipyard:
        need(GOOD_HULL_PLATING, 0.25); need(GOOD_SHIP_COMPONENTS, 0.15); need(GOOD_REACTOR_CORES, 0.04); need(GOOD_WEAPONS, 0.03);
        need(GOOD_MACHINERY, 0.05);
        break;
    case Settlement::ResearchStation: case Settlement::ResearchOutpost:
        make(RAW_FLORA, 0.03 * bio); make(GOOD_MEDICINES, 0.01);
        need(GOOD_INSTRUMENTS, 0.05); need(GOOD_ELECTRONICS, 0.03);
        break;
    case Settlement::SolarCollector:
        make(REF_CHEMICALS, 0.20); make(REF_SILICON, 0.15); make(REF_REACTOR_FUEL, 0.03);
        need(GOOD_ELECTRONICS, 0.02);
        break;
    case Settlement::Greenhouse:
        make(RAW_GRAIN, 0.50); make(REF_FOOD, 0.30); make(RAW_FLORA, 0.03);
        need(REF_FERTILIZER, 0.15); need(REF_WATER, 0.10);
        break;
    case Settlement::Starbase:
        // A state's ships are armed and fuelled here; the more warlike the state, the more so
        need(GOOD_WEAPONS, 0.010 * (owner ? 1.0 + owner->axis[AX_FORCE] : 1.0)); need(GOOD_MUNITIONS, 0.03); need(REF_FUEL, 0.10);
        break;
    default:
        need(REF_FUEL, 0.06);                                      // ports, waystations, freeports: traffic
        break;
    }
}

// A good's place in a market: made here, needed here, or just passing through
static void SetTargets(MarketDef &d, bool hub, const Faction *ruler, bool lawful)
{
    float inputs[GOOD_COUNT] = {};
    for (int g = 0; g < GOOD_COUNT; g++)
        for (const Ingredient &in : GoodData(g).recipe) if (in.tonnes > 0.0f) inputs[in.good] += d.out[g] * in.tonnes;
    float base = hub ? fmaxf(15.0f, 0.4f * d.traffic) : 0.0f;     // a hub keeps a little of everything its system handles
    for (int g = 0; g < GOOD_COUNT; g++) {
        float demand = d.use[g] + inputs[g];
        d.target[g] = fmaxf(STOCK_DAYS * fmaxf(d.out[g], demand), base);
        d.role[g] = d.target[g] <= 0.0f ? Role::None : d.out[g] > demand * 1.05f ? Role::Export : demand > d.out[g] * 1.05f ? Role::Import : Role::Trade;
    }
    // What the law here bans, lawful places don't touch
    if (ruler && lawful)
        for (int g : ruler->contraband) { d.target[g] = d.out[g] = d.use[g] = 0.0f; d.role[g] = Role::None; d.banned[g] = true; }
}

static void SettleStocks(EconBase &base, const FactionSet &fs);

// Every market in human space. (Slow: it builds every inhabited system. Done once per galaxy.)
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
        int first = (int)base->defs.size(), hub = -1;
        double hubScore = 0.0;
        for (int bi = 0; bi < (int)L.bodies.size(); bi++) {
            const Body &b = L.bodies[bi];
            if (b.population <= 0) continue;
            MarketDef d;
            d.sys = s->id; d.body = bi; d.bodyId = b.id; d.name = b.name; d.settlement = b.settlement;
            d.ruler = s->ruler; d.owner = b.owner; d.station = b.kind == Kind::Station;
            d.ly = { (s->cx - fs.homeCell[0]) * CELL_LY, (s->cy - fs.homeCell[1]) * CELL_LY, (s->cz - fs.homeCell[2]) * CELL_LY };
            d.scale = (float)(2.0 * pow(b.population, 0.45));
            const Body *ground = b.kind == Kind::Station && b.parent > 0 && L.bodies[b.parent].kind != Kind::Star ? &L.bodies[b.parent] : nullptr;
            Industry(d, b, ground, fs.Get(b.owner));
            double score = d.scale * (IsPort(b.settlement) ? 4.0 : 1.0);     // ports first; failing one, the biggest place
            if (score > hubScore) { hubScore = score; hub = (int)base->defs.size(); }
            base->byBody[d.bodyId] = (int)base->defs.size();
            base->defs.push_back(d);
        }
        if (hub < 0) continue;
        hubs.push_back(hub);
        for (int i = first; i < (int)base->defs.size(); i++) { base->defs[i].hub = hub; base->defs[hub].traffic += base->defs[i].scale; }
    }
    // What the law bans, lawful places neither make nor use (before the sums below: it isn't supply)
    for (MarketDef &d : base->defs) {
        const Faction *owner = fs.Get(d.owner), *ruler = fs.Get(d.ruler);
        if (ruler && owner && owner->kind != FactionKind::Clan)
            for (int g : ruler->contraband) { d.out[g] = d.use[g] = 0.0f; d.banned[g] = true; }
    }
    // Human space as a whole makes a little more of everything than it uses (the shares above only say who
    // makes what): scale each good's output to 125% of the demand for it. Finished goods first, since
    // how much of them is made sets how much of their inputs is wanted.
    for (int g = GOOD_COUNT - 1; g >= 0; g--) {
        double made = 0.0, wanted = 0.0;
        for (const MarketDef &d : base->defs) {
            made += d.out[g];
            wanted += d.use[g];
            for (int p = g + 1; p < GOOD_COUNT; p++)
                for (const Ingredient &in : GoodData(p).recipe) if (in.good == g && in.tonnes > 0.0f) wanted += d.out[p] * in.tonnes;
        }
        if (made <= 0.0 || wanted <= 0.0) continue;
        float k = (float)(1.25 * wanted / made);
        for (MarketDef &d : base->defs) d.out[g] *= k;
    }
    for (MarketDef &d : base->defs) {
        const Faction *owner = fs.Get(d.owner);
        SetTargets(d, d.hub == (int)(&d - base->defs.data()), fs.Get(d.ruler), owner && owner->kind != FactionKind::Clan);
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
    SettleStocks(*base, fs);
    known[gal.seed] = base;
    return base;
}

// ---------------------------------------------------------------------------
// Prices and the daily round
// ---------------------------------------------------------------------------
// A deal moves the price as it goes: it's done in slices, each at the price the last one left
const int SLICES = 10;

// Price against stock: at its target a good costs its base price; a glut can't push it under PRICE_FLOOR
// of that, and a market with none left pays PRICE_CAP times it
const double PRICE_FLOOR = 0.3, PRICE_CAP = 6.0;

static double PriceOf(const MarketDef &d, const MarketState &s, int g, double pricing = 1.0)
{
    if (d.target[g] <= 0.0f) return 0.0;
    double have = fmax((double)s.stock[g], 0.02 * d.target[g]) / d.target[g];
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

// Goods move from a to b (or back) wherever the price gap is worth the trip.
// 'costAB': what it costs to bring a good into b from a, as a share of its price (tariffs).
// (prices as they stood at the start of the round: 'pa' and 'pb', a row of GOOD_COUNT each)
static void Trade(const MarketDef &da, MarketState &sa, const MarketDef &db, MarketState &sb, double capacity,
                  double costAB, double costBA, const float *pricesA, const float *pricesB)
{
    for (int g = 0; g < GOOD_COUNT; g++) {
        if (da.target[g] <= 0.0f || db.target[g] <= 0.0f) continue;
        double pa = pricesA[g], pb = pricesB[g];
        bool   toB = pb > pa;
        const MarketDef &from = toB ? da : db;
        MarketState &src = toB ? sa : sb, &dst = toB ? sb : sa;
        double gap = (toB ? pb / pa : pa / pb) - 1.0 - 0.03 - (toB ? costAB : costBA);     // (3%: the hauler's cut)
        if (gap <= 0.0) continue;
        double spare = src.stock[g] - 0.25 * from.target[g];                                // nobody sells their last
        if (spare <= 0.0) continue;
        double q = fmin(fmin(1.5 * capacity * gap, spare), 0.3 * src.stock[g]);
        src.stock[g] -= (float)q;
        dst.stock[g] += (float)q;
    }
}

static const double USUAL_RAID = 0.3;       // what clans get up to when nobody's paying or chasing them

static void RunDays(Economy &e, const EconBase &base, const FactionSet &fs, double days, double clock)
{
    const int N = (int)base.defs.size();
    static std::vector<float> supply, demand;
    supply.assign(N, 1.0f); demand.assign(N, 1.0f);
    for (size_t i = 0; i < e.shocks.size();) {
        if (e.shocks[i].until <= clock) { e.shocks.erase(e.shocks.begin() + i); continue; }
        supply[e.shocks[i].market] *= e.shocks[i].supply; demand[e.shocks[i].market] *= e.shocks[i].demand;
        i++;
    }
    auto boost = [&](int faction) { return faction >= 0 && faction < (int)e.output.size() && e.outputUntil[faction] > clock ? e.output[faction] : 1.0f; };

    // Use, then make (raw goods first, so a day's ore can become a day's steel)
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        MarketState &s = e.state[i];
        float made = supply[i] * boost(s.owner) * (d.ruler != s.owner ? boost(d.ruler) : 1.0f);
        for (int g = 0; g < GOOD_COUNT; g++)
            if (d.use[g] > 0.0f) {
                // (what's scarce gets rationed: people make do with less of it)
                float ration = fminf(1.0f, 0.3f + 0.7f * s.stock[g] / d.target[g]);
                s.stock[g] = fmaxf(0.0f, s.stock[g] - d.use[g] * demand[i] * ration * (float)days);
            }
        for (int g = 0; g < GOOD_COUNT; g++) {
            if (d.out[g] <= 0.0f) continue;
            float room = fminf(1.0f, fmaxf(0.0f, 2.0f - s.stock[g] / d.target[g]));       // overstocked: the lines slow down
            float q = d.out[g] * made * room * (float)days;
            for (const Ingredient &in : GoodData(g).recipe) if (in.tonnes > 0.0f) q = fminf(q, s.stock[in.good] / in.tonnes);
            if (q <= 0.0f) continue;
            for (const Ingredient &in : GoodData(g).recipe) if (in.tonnes > 0.0f) s.stock[in.good] -= q * in.tonnes;
            s.stock[g] += q;
        }
    }
    // Everyone's prices, now that the day's making and using is done
    static std::vector<float> prices;
    prices.resize((size_t)N * GOOD_COUNT);
    for (int i = 0; i < N; i++) {
        double pricing = Pricing(e, e.state[i].owner, clock);
        for (int g = 0; g < GOOD_COUNT; g++) prices[(size_t)i * GOOD_COUNT + g] = (float)PriceOf(base.defs[i], e.state[i], g, pricing);
    }
    auto row = [&](int i) { return &prices[(size_t)i * GOOD_COUNT]; };
    // Trade: every market with its system's hub...
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        if (d.hub == i || d.hub < 0) continue;
        const MarketDef &h = base.defs[d.hub];
        Trade(d, e.state[i], h, e.state[d.hub], 2.0 * d.scale * days, 0.0, 0.0, row(i), row(d.hub));
    }
    // ...and hubs with each other, along the lanes: less with distance, less still through raiders, not at all across an embargo
    const int F = fs.Count();
    for (const Lane &l : base.lanes) {
        const MarketDef &a = base.defs[l.a], &b = base.defs[l.b];
        bool borders = a.ruler != b.ruler && a.ruler >= 0 && b.ruler >= 0;
        if (borders && (e.embargoUntil[a.ruler * F + b.ruler] > clock || e.embargoUntil[b.ruler * F + a.ruler] > clock)) continue;
        double raids = fmax(RaidAt(e, fs, a.ly), RaidAt(e, fs, b.ly));
        // (a big port sends its own ships to a small one: the lane carries more than the small one could alone)
        double capacity = sqrt((double)a.traffic * b.traffic) * 1.5 / (1.0 + l.ly / 10.0) * (1.0 - 0.85 * raids) * days;
        double intoB = a.ruler != b.ruler && b.ruler >= 0 ? e.tariff[b.ruler] : 0.0, intoA = a.ruler != b.ruler && a.ruler >= 0 ? e.tariff[a.ruler] : 0.0;
        Trade(a, e.state[l.a], b, e.state[l.b], capacity, intoB, intoA, row(l.a), row(l.b));
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
        e.state[i].owner = base.defs[i].owner;
        for (int g = 0; g < GOOD_COUNT; g++)
            e.state[i].stock[g] = base.stock0.empty() ? base.defs[i].target[g] : base.stock0[(size_t)i * GOOD_COUNT + g];
    }
    e.tariff.assign(F, 0.0f);
    for (const Faction &f : fs.list) e.tariff[f.id] = f.tariff;
    e.embargoUntil.assign((size_t)F * F, 0.0);
    e.raid.assign(F, 0.0f);
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) e.raid[f.id] = (float)USUAL_RAID;
    e.raidUntil.assign(F, 0.0);
    e.output.assign(F, 1.0f);  e.outputUntil.assign(F, 0.0);
    e.pricing.assign(F, 1.0f); e.pricingUntil.assign(F, 0.0);
    e.nextAct.assign(F, 0.0);
    e.lastAct.assign(F, -1);
    e.shocks.clear();
}

// A new galaxy's markets have been trading for a while: run it in until stocks stop lurching
static void SettleStocks(EconBase &base, const FactionSet &fs)
{
    Economy e;
    InitState(e, base, fs);
    for (int step = 0; step < 120; step++) RunDays(e, base, fs, 0.25, 0.0);
    for (int step = 0; step < 120; step++) RunDays(e, base, fs, 0.05, 0.0);       // (...then at something like the pace it will really run at)
    base.stock0.resize(base.defs.size() * GOOD_COUNT);
    for (size_t i = 0; i < base.defs.size(); i++)
        for (int g = 0; g < GOOD_COUNT; g++) base.stock0[i * GOOD_COUNT + g] = e.state[i].stock[g];
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
    bool farms = d.out[RAW_GRAIN] > 0.0f, mines = d.out[ORE_IRON] > 0.0f || d.out[ORE_SILICATES] > 0.0f;
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
    w.econ.simulated = w.clock;
    w.econ.dice = Mix64(w.seed ^ 0xEC0DEull);
}

void StepEconomy(World &w, double days)
{
    EnsureEconomy(w);
    RunDays(w.econ, *w.econ.base, w.systems.factions, days, w.clock);
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

double MarketPrice(const World &w, int market, int good)
{
    if (market < 0 || market >= w.econ.Count()) return 0.0;
    return PriceOf(w.econ.base->defs[market], w.econ.state[market], good, Pricing(w.econ, w.econ.state[market].owner, w.clock));
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
        m.role[g] = d.role[g];
        m.banned[g] = d.banned[g];
        m.price[g] = MarketPrice(w, i, g);
        m.stock[g] = s.stock[g];
        m.target[g] = d.target[g];
    }
    m.owner = s.owner; m.ruler = d.ruler;
    m.tariff = d.ruler >= 0 ? w.econ.tariff[d.ruler] : 0.0;
    m.edge = Edge(c, s.owner);
    m.fuelPrice = m.Sells(REF_FUEL) ? m.BuyPrice(REF_FUEL) : 0.0;
    return m;
}

Market LiveMarket(const World &w, const Captain &c, const Body &b) { return LiveAt(w, c, w.econ.MarketOf(b.id)); }

bool KnownMarket(const Captain &c, const Body &b, Market &m, double *at)
{
    auto it = c.know.prices.find(b.id);
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
    m.fuelPrice = m.Sells(REF_FUEL) ? m.BuyPrice(REF_FUEL) : 0.0;
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
    double room = CARGO_CAPACITY - p.CargoTotal();
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
    if (!LiveAt(w, c, i).Sells(REF_FUEL)) { Tell(w, c, b->name + " has no fuel to sell"); return false; }
    double room = FUEL_CAPACITY - p.fuel;
    if (room <= 1e-6)  { Tell(w, c, "Tank already full");  return false; }
    double cost, got = Purchase(w, c, i, REF_FUEL, fmin(qty, room), p.credits, cost);
    if (got <= 1e-6)   { Tell(w, c, "Not enough credits"); return false; }
    p.fuel    += got;
    p.credits -= cost;
    GoodCustomer(w, c, i, cost);
    RecordAt(w, c, i);
    w.Emit(EventType::BoughtFuel, TextFormat("Bought %.1f t fuel for %s cr", got, Thousands(cost).c_str()),
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
        for (int g = 0; g < GOOD_COUNT; g++) line += TextFormat(" %.6g", e.state[i].stock[g]);
        out += line + "\n";
        if (e.state[i].owner != d.owner) out += TextFormat("owner %llu %d\n", (unsigned long long)d.bodyId, e.state[i].owner);
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

void WritePrices(const Captain &c, std::string &out)
{
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

bool ReadPriceLine(Captain &c, const char *key, const char *rest)
{
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
