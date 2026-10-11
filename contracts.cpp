// contracts.cpp - drawing up the boards, and taking, doing and failing contracts
#include "contracts.h"
#include "economy.h"
#include "format.h"
#include <algorithm>
#include <cstring>
#include <functional>

struct Dice {
    uint64_t s;
    double operator()() { s = Mix64(s); return (double)(s >> 11) * (1.0 / 9007199254740992.0); }
};

static double Clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static void   Tell(World &w, Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

static void Bump(const World &w, Captain &c, int faction, double by)
{
    int n = w.systems.factions.Count();
    if (faction < 0 || faction >= n) return;
    if ((int)c.standing.size() < n) c.standing.resize(n, 0.0f);
    c.standing[faction] = (float)Clamp(c.standing[faction] + by, -100.0, 100.0);
}

static bool IssuersCargo(ContractKind k) { return k == ContractKind::Haulage || k == ContractKind::Smuggling; }
static bool Collected(ContractKind k)    { return IssuersCargo(k) || k == ContractKind::Courier; }   // starts at the issuer's dock

// ---------------------------------------------------------------------------
// Words
// ---------------------------------------------------------------------------
const char *ContractKindName(ContractKind k)
{
    static const char *names[] = { "Haulage", "Supply order", "Smuggling", "Survey", "Courier" };
    return names[(int)k < (int)ContractKind::Count ? (int)k : 0];
}

std::string ContractLabel(const Contract &k)
{
    if (k.law == NO_FACTION) return ContractKindName(k.kind);
    return k.kind == ContractKind::Supply ? "Smuggling (supply order)" : "Smuggling";
}

std::string ContractTitle(const Contract &k)
{
    const char *good = k.good >= 0 && k.good < GOOD_COUNT ? GoodData(k.good).name : "cargo";
    switch (k.kind) {
    case ContractKind::Haulage:   return TextFormat("Haul %.0f t %s to %s", k.tonnes, good, k.toName.c_str());
    case ContractKind::Supply:    return TextFormat(k.law != NO_FACTION ? "Supply %.0f t %s to %s, quietly" : "Supply %.0f t %s to %s", k.tonnes, good, k.toName.c_str());
    case ContractKind::Smuggling: return TextFormat("Run %.0f t %s to %s, quietly", k.tonnes, good, k.toName.c_str());
    case ContractKind::Survey:    return TextFormat("Survey %d worlds of %s", k.need, k.surveyName.c_str());
    default:                      return TextFormat("Courier run to %s", k.toName.c_str());
    }
}

std::string ContractTerms(const FactionSet &fs, const Contract &k)
{
    std::string s;
    auto add = [&](const std::string &t) { s += (s.empty() ? "" : "; ") + t; };
    if (const Faction *g = fs.Get(k.guild)) add(g->name + " members");
    const Faction *judge = fs.Get(k.guild != NO_FACTION ? k.guild : k.issuer);
    if (judge && k.minStanding > 0.0f) add(std::string(RankName(*judge, k.minStanding)) + " or better");
    if (const Faction *l = fs.Get(k.licence)) add("bonded carrier (" + l->name + ")");
    return s;
}

std::string ContractBar(const World &w, const Captain &c, const Contract &k)
{
    const FactionSet &fs = w.systems.factions;
    if ((int)c.jobs.size() >= MAX_JOBS) return TextFormat("You already have %d contracts", MAX_JOBS);
    if (k.guild != NO_FACTION && !(c.Papers(k.guild) & PAPER_MEMBER)) return "For members of " + fs.Name(k.guild);
    int judge = k.guild != NO_FACTION ? k.guild : k.issuer;
    if (const Faction *f = fs.Get(judge)) {
        if (c.Standing(judge) < k.minStanding) return std::string("Needs standing: ") + RankName(*f, k.minStanding) + " with " + f->name;
        if (c.Standing(judge) < -20.0f) return f->name + " won't deal with you";
    }
    if (k.licence != NO_FACTION && !(c.Papers(k.licence) & PAPER_CARRIER)) return "Needs a bonded carrier's licence from " + fs.Name(k.licence);
    if (c.player.credits < k.collateral) return TextFormat("Needs a deposit of %s cr", Thousands(k.collateral).c_str());
    if (!k.wide && c.loaded.index != k.fromSys) return "Be in the " + (w.systems.Exists(k.fromSys) ? w.systems[k.fromSys].name : std::string("its")) + " system to accept";
    return "";
}

static uint64_t DockedAt(const Captain &c)
{
    const Body *b = TradePartner(c);
    return b ? b->id : 0;
}

std::string JobStatus(const World &w, const Captain &c, const Contract &k, bool *here)
{
    uint64_t at = DockedAt(c);
    if (here) *here = false;
    if (!k.loaded) {
        if (here) *here = at == k.fromBody;
        return (k.kind == ContractKind::Courier ? "Collect the dispatches at " : "Collect the cargo at ") + k.fromName;
    }
    if (k.kind == ContractKind::Survey) {
        if (k.Scanned() < k.need) return TextFormat("Scan %d more worlds of %s", k.need - k.Scanned(), k.surveyName.c_str());
        if (here) *here = at == k.fromBody;
        return "Bring the survey back to " + k.fromName;
    }
    if (k.kind != ContractKind::Courier && c.player.cargo[k.good] + 1e-6 < k.tonnes) {
        (void)w;
        return TextFormat("Find %.0f t more %s, for %s", k.tonnes - c.player.cargo[k.good], GoodData(k.good).name, k.toName.c_str());
    }
    if (here) *here = at == k.toBody;
    return "Deliver to " + k.toName;
}

SysId JobNextSystem(const Contract &k)
{
    if (!k.loaded) return k.fromSys;
    if (k.kind == ContractKind::Survey) return k.Scanned() < k.need ? k.surveySys : k.fromSys;
    return k.toSys;
}

// ---------------------------------------------------------------------------
// Guild chapters
// ---------------------------------------------------------------------------
static bool IsPort(Settlement s)
{
    return s == Settlement::OrbitalPort || s == Settlement::Starbase || s == Settlement::Waystation || s == Settlement::Freeport;
}

static bool Chapter(Settlement s, uint64_t bodyId, bool home, const Faction &guild)
{
    if (guild.kind != FactionKind::Guild || guild.guild == GuildKind::BountyHunters) return false;    // (the hunters have no work until there's fighting)
    if (home) return IsPort(s);                                    // the free port has a hall for everyone
    int h = (int)(Mix64(bodyId ^ (0x6111Dull * (uint64_t)(guild.id + 1))) % 100);
    switch (guild.guild) {
    case GuildKind::Traders:   return IsPort(s) && h < 50;
    case GuildKind::Explorers: return s == Settlement::ResearchStation || s == Settlement::ResearchOutpost || (IsPort(s) && h < 25);
    case GuildKind::Colonial:  return (s == Settlement::DomedColonies || s == Settlement::UndergroundColony || IsPort(s)) && h < 30;
    default:                   return false;
    }
}

bool HasChapter(const World &w, SysId sys, const Body &b, int guildFaction)
{
    const Faction *g = w.systems.factions.Get(guildFaction);
    return g && b.population > 0 && Chapter(b.settlement, b.id, sys == w.systems.home, *g);
}

// ---------------------------------------------------------------------------
// Drawing up a board
// ---------------------------------------------------------------------------
static double Ly(const MarketDef &a, const MarketDef &b) { return Len(a.ly - b.ly); }
static double TimeFor(double ly) { return 600.0 + 150.0 * ly; }          // ten minutes, and a few more per light year
static bool   Shady(const World &w, int market)                           // nobody here asks what's in the hold
{
    const Faction *owner = w.systems.factions.Get(w.econ.base->defs[market].owner);
    return !owner || owner->kind == FactionKind::Clan;
}

static void From(Contract &k, const MarketDef &d) { k.fromBody = d.bodyId; k.fromSys = d.sys; k.fromName = d.name; }
static void To(Contract &k, const MarketDef &d)   { k.toBody = d.bodyId; k.toSys = d.sys; k.toName = d.name; }

// The government whose contraband a contract's cargo is, if it starts or ends in its space (NO_FACTION: it's lawful
// all the way). Lawful markets there don't deal in it, but a freeport or a clan's station in the same space will, and
// getting it there means running that government's customs and patrols.
static int LawBroken(const World &w, const Contract &k)
{
    if (k.good < 0 || k.kind == ContractKind::Survey || k.kind == ContractKind::Courier) return NO_FACTION;
    for (uint64_t body : { k.toBody, k.fromBody }) {
        int m = w.econ.MarketOf(body);
        const Faction *law = m >= 0 ? w.systems.factions.Get(w.econ.base->defs[m].ruler) : nullptr;
        if (law && std::find(law->contraband.begin(), law->contraband.end(), k.good) != law->contraband.end()) return law->id;
    }
    return NO_FACTION;
}

// On to the board. False if it isn't posted (a government doesn't advertise for its own contraband).
static bool Post(World &w, Contract k)
{
    int law = LawBroken(w, k);
    if (law != NO_FACTION) {
        if (k.issuer == law || k.guild != NO_FACTION) return false;
        // It's smuggling, whatever it was drawn up as, and is labelled and paid as such
        if (k.law == NO_FACTION) k.pay *= 1.4f;
        k.law = law;
        if (k.kind == ContractKind::Haulage) k.kind = ContractKind::Smuggling;
        if (k.against == NO_FACTION) k.against = law;
        k.licence = NO_FACTION; k.minStanding = 0.0f;
    }
    k.id = ++w.contractSeq;
    if (k.expires <= 0.0) k.expires = w.clock + 2.0 * MARKET_DAY;
    k.pay = floorf(k.pay / 10.0f + 0.5f) * 10.0f;
    k.collateral = floorf(k.collateral / 10.0f + 0.5f) * 10.0f;
    w.contracts.push_back(k);
    return true;
}

// The markets a place deals with: its own system's, and those of the systems its hub has lanes to
static std::vector<int> Reach(const EconBase &base, int m, bool own, bool others)
{
    int hub = base.defs[m].hub;
    std::vector<int> hubs;
    if (others) for (const Lane &l : base.lanes) { if (l.a == hub) hubs.push_back(l.b); if (l.b == hub) hubs.push_back(l.a); }
    std::vector<int> out;
    for (int i = 0; i < (int)base.defs.size(); i++) {
        if (i == m) continue;
        int h = base.defs[i].hub;
        if ((own && h == hub) || std::find(hubs.begin(), hubs.end(), h) != hubs.end()) out.push_back(i);
    }
    return out;
}

// Better-paid work for those the issuer knows
static void Gate(Contract &k, Dice &dice)
{
    if (k.issuer == NO_FACTION && k.guild == NO_FACTION) return;
    double r = dice();
    if (r < 0.10)      { k.minStanding = 50.0f; k.pay *= 1.6f; }
    else if (r < 0.35) { k.minStanding = 20.0f; k.pay *= 1.3f; }
}

static int IssuerAt(const World &w, int m)
{
    int owner = w.econ.state[m].owner;
    return owner != NO_FACTION ? owner : NO_FACTION;
}

static bool MakeHaulage(const World &w, int m, int to, int g, double tonnes, double bonus, Contract &k)
{
    const EconBase &base = *w.econ.base;
    const MarketDef &a = base.defs[m], &b = base.defs[to];
    double pa = MarketPrice(w, m, g), pb = MarketPrice(w, to, g), ly = Ly(a, b);
    if (pa <= 0.0 || pb <= 0.0 || tonnes < 5.0) return false;
    k = Contract();
    k.kind = ContractKind::Haulage; k.good = g; k.tonnes = (float)floor(tonnes);
    From(k, a); To(k, b);
    k.issuer = IssuerAt(w, m);
    k.pay = (float)(k.tonnes * fmax(0.45 * (pb - pa), 0.04 * GoodData(g).basePrice + 4.0) * (1.0 + 0.15 * ly) * bonus + 60.0);
    k.collateral = (float)(k.tonnes * fmax(pa, pb) * 1.15);
    k.allowed = TimeFor(ly);
    const Faction *ruler = w.systems.factions.Get(a.ruler);
    if (k.collateral > BONDED_OVER && ruler) k.licence = ruler->id;
    return true;
}

static bool MakeSupply(const World &w, int m, int g, double tonnes, double premium, Contract &k)
{
    const MarketDef &d = w.econ.base->defs[m];
    double p = MarketPrice(w, m, g);
    if (p <= 0.0 || tonnes < 3.0) return false;
    k = Contract();
    k.kind = ContractKind::Supply; k.good = g; k.tonnes = (float)floor(tonnes);
    From(k, d); To(k, d);
    k.issuer = IssuerAt(w, m);
    k.pay = (float)(k.tonnes * fmin(p, 2.0 * GoodData(g).basePrice) * premium + 40.0);
    k.allowed = TimeFor(6.0) + 600.0;                              // (time to go and find it)
    k.loaded = false;
    return true;
}

static bool MakeCourier(const World &w, int m, int to, Contract &k)
{
    const MarketDef &a = w.econ.base->defs[m], &b = w.econ.base->defs[to];
    double ly = Ly(a, b);
    k = Contract();
    k.kind = ContractKind::Courier;
    From(k, a); To(k, b);
    k.issuer = IssuerAt(w, m);
    k.pay = (float)(140.0 + 130.0 * ly);
    k.allowed = 0.7 * TimeFor(ly);                                 // (no dawdling)
    return true;
}

// An empty system not far off, with worlds worth the trip
static bool MakeSurvey(const World &w, int m, Dice &dice, Contract &k)
{
    const MarketDef &d = w.econ.base->defs[m];
    std::vector<const StarSystem *> near, empty;
    w.systems.Nearby(d.sys, {}, 9.0 * LY_U, near);
    for (const StarSystem *s : near) if (s->develop <= 0.0f && s->id != d.sys) empty.push_back(s);
    std::sort(empty.begin(), empty.end(), [](const StarSystem *a, const StarSystem *b) { return a->id < b->id; });
    for (int tries = 0; tries < 5 && !empty.empty(); tries++) {
        const StarSystem *s = empty[(size_t)(dice() * empty.size()) % empty.size()];
        LoadedSystem L;
        GenerateSystemContents(*s, L, true);
        int worlds = 0;
        for (int i = 0; i < (int)L.bodies.size() && i < 64; i++) if (L.bodies[i].kind == Kind::Planet) worlds++;
        if (worlds < 2) continue;
        double ly = Len(w.systems.Between(d.sys, s->id)) / LY_U;
        k = Contract();
        k.kind = ContractKind::Survey;
        From(k, d); To(k, d);
        k.surveySys = s->id; k.surveyName = s->name;
        k.need = std::min(worlds, 2 + (int)(dice() * 3.0));
        k.pay = (float)(500.0 + 260.0 * ly + 220.0 * k.need);
        k.allowed = 2.0 * TimeFor(ly) + 300.0 * k.need;
        k.effect = ContractEffect::Charts;
        return true;
    }
    return false;
}

static const Shock *ShockAt(const World &w, int m)
{
    for (const Shock &s : w.econ.shocks) if (s.market == m && s.until > w.clock && (s.supply < 1.0f || s.demand > 1.0f)) return &s;
    return nullptr;
}

// What a market is shortest of, worst (by value) first
static std::vector<int> Shortages(const World &w, int m, double below)
{
    const MarketDef &d = w.econ.base->defs[m];
    const MarketState &s = w.econ.state[m];
    std::vector<int> goods;
    for (int g = 0; g < GOOD_COUNT; g++)
        if (d.role[g] == Role::Import && !d.banned[g] && s.stock[g] < below * d.target[g]) goods.push_back(g);
    auto worth = [&](int g) { return (d.target[g] - s.stock[g]) * GoodData(g).basePrice; };
    std::sort(goods.begin(), goods.end(), [&](int a, int b) { return worth(a) > worth(b); });
    return goods;
}

// A guild's work for its members, out of one of its chapters
static bool GuildJob(World &w, int m, const Faction &guild, Dice &dice, Contract &k)
{
    const EconBase &base = *w.econ.base;
    const MarketDef &d = base.defs[m];
    const MarketState &s = w.econ.state[m];
    std::vector<int> all = Reach(base, m, true, true), abroad;
    for (int i : all) if (base.defs[i].hub != d.hub) abroad.push_back(i);
    if (guild.guild == GuildKind::Traders && !abroad.empty()) {
        // A good run to another system, in something worth carrying
        int bestG = -1, bestTo = -1; double bestGain = 0.0;
        for (int tries = 0; tries < 24; tries++) {
            int g = (int)(dice() * GOOD_COUNT) % GOOD_COUNT, to = abroad[(size_t)(dice() * abroad.size()) % abroad.size()];
            const MarketDef &t = base.defs[to];
            if (d.target[g] <= 0.0f || t.target[g] <= 0.0f || t.banned[g] || d.banned[g] || s.stock[g] < 30.0f) continue;
            double gain = MarketPrice(w, to, g) - MarketPrice(w, m, g);
            if (gain > bestGain) { bestGain = gain; bestG = g; bestTo = to; }
        }
        if (bestG < 0 || !MakeHaulage(w, m, bestTo, bestG, fmin(0.5 * s.stock[bestG], 30.0 + 60.0 * dice()), 1.4, k)) return false;
        k.licence = NO_FACTION;                                    // (the guild's bond covers its members)
    } else if (guild.guild == GuildKind::Explorers) {
        if (!MakeSurvey(w, m, dice, k)) return false;
    } else if (guild.guild == GuildKind::Colonial) {
        // Somewhere small and far from help that's short of what keeps it going
        static const int needs[] = { GOOD_HABITAT_MODULES, GOOD_MACHINERY, GOOD_MEDICINES, REF_FOOD, REF_WATER };
        bool found = false;
        for (int tries = 0; tries < 16 && !all.empty() && !found; tries++) {
            int to = all[(size_t)(dice() * all.size()) % all.size()], g = needs[(int)(dice() * 5.0) % 5];
            const MarketDef &t = base.defs[to];
            if (t.scale > 400.0f || t.target[g] <= 0.0f || t.banned[g] || t.out[g] > 0.0f) continue;
            if (!MakeSupply(w, to, g, fmin((double)t.target[g], 10.0 + 40.0 * dice()), 1.5, k)) continue;
            From(k, d);                                            // (posted here; wanted there)
            k.allowed += TimeFor(Ly(d, t));
            found = true;
        }
        if (!found) return false;
    } else {
        return false;
    }
    k.guild = k.issuer = guild.id;
    return true;
}

static void MakeBoard(World &w, int m)
{
    const EconBase &base = *w.econ.base;
    const FactionSet &fs = w.systems.factions;
    const MarketDef &d = base.defs[m];
    const MarketState &s = w.econ.state[m];
    Dice dice{ Mix64(w.seed ^ d.bodyId ^ (uint64_t)(w.clock / BOARD_REFRESH + 1.0) * 0x9E3779B97F4A7C15ull) };
    std::vector<int> local = Reach(base, m, true, false), all = Reach(base, m, true, true), abroad;
    for (int i : all) if (base.defs[i].hub != d.hub) abroad.push_back(i);
    Contract k;
    int posted = 0;
    auto post = [&](Contract &c, bool gate = true) { if (gate) Gate(c, dice); if (Post(w, c)) posted++; };

    // Short of something: supply orders (and if something has gone wrong here, they're relief)
    const Shock *shock = ShockAt(w, m);
    std::vector<int> shorts = Shortages(w, m, 0.7);
    for (int i = 0; i < (int)shorts.size() && i < 2; i++) {
        int g = shorts[i];
        double want = fmin(d.target[g] - s.stock[g], 15.0 + 60.0 * dice());
        if (!MakeSupply(w, m, g, want, shock ? 1.5 : 1.25, k)) continue;
        if (shock) k.effect = ContractEffect::Relief;
        post(k, !shock);
    }
    // A glut of something it makes: haul it to wherever nearby is paying most
    int hauls = 0;
    for (int g = 0; g < GOOD_COUNT && hauls < 2; g++) {
        if (d.role[g] != Role::Export || s.stock[g] < 1.15f * d.target[g]) continue;
        int best = -1; double bestP = MarketPrice(w, m, g) * 1.15;
        for (int to : all) {
            const MarketDef &t = base.defs[to];
            if (t.target[g] <= 0.0f || t.banned[g]) continue;
            double p = MarketPrice(w, to, g);
            if (p > bestP) { bestP = p; best = to; }
        }
        if (best < 0) continue;
        double tonnes = fmin(0.3 * (s.stock[g] - d.target[g]) + 10.0, 15.0 + 70.0 * dice());
        if (MakeHaulage(w, m, best, g, tonnes, 1.0, k)) { post(k); hauls++; }
    }
    // Dispatches and passengers
    if (!all.empty() && dice() < 0.6) {
        const std::vector<int> &pool = !abroad.empty() && dice() < 0.5 ? abroad : all;
        if (MakeCourier(w, m, pool[(size_t)(dice() * pool.size()) % pool.size()], k)) post(k);
    }
    // Somewhere nobody asks questions: a run into a government's space with what it bans
    if (Shady(w, m) || d.settlement == Settlement::Freeport) {
        std::vector<std::pair<int, int>> runs;                     // (destination, good)
        for (int to : all) {
            const MarketDef &t = base.defs[to];
            const Faction *law = fs.Get(t.ruler);
            if (!law || law->contraband.empty() || !Shady(w, to)) continue;
            for (int g : law->contraband) runs.push_back({ to, g });
        }
        if (!runs.empty() && dice() < 0.8) {
            auto run = runs[(size_t)(dice() * runs.size()) % runs.size()];
            const MarketDef &t = base.defs[run.first];
            const Faction *law = fs.Get(t.ruler);
            k = Contract();
            k.kind = ContractKind::Smuggling; k.good = run.second; k.tonnes = (float)floor(5.0 + 25.0 * dice());
            From(k, d); To(k, t);
            const Faction *owner = fs.Get(s.owner);
            k.issuer = owner && owner->kind == FactionKind::Clan ? owner->id : NO_FACTION;
            k.against = law->id;
            double worth = GoodData(k.good).basePrice;
            k.pay = (float)(k.tonnes * worth * 0.5 * (1.0 + law->policing) * (1.0 + 0.1 * Ly(d, t)) + 200.0);
            k.collateral = (float)(k.tonnes * worth * 1.5);
            k.allowed = TimeFor(Ly(d, t));
            post(k, false);
        }
    }
    // The guilds, where they keep a chapter: for members only
    for (const Faction &guild : fs.list)
        if (Chapter(d.settlement, d.bodyId, d.sys == w.systems.home, guild) && GuildJob(w, m, guild, dice, k)) post(k);
    // A government wants a system looked at, now and then: anyone may
    if (IsPort(d.settlement) && fs.Get(d.ruler) && dice() < 0.2 && MakeSurvey(w, m, dice, k)) {
        k.issuer = d.ruler; k.pay *= 0.75f;
        post(k);
    }
    // And the everyday work, so there's always something
    for (int tries = 0; tries < 12 && posted < 3 && !all.empty(); tries++) {
        int to = all[(size_t)(dice() * all.size()) % all.size()], g = (int)(dice() * GOOD_COUNT) % GOOD_COUNT;
        const MarketDef &t = base.defs[to];
        double r = dice();
        if (r < 0.3)       { if (MakeCourier(w, m, to, k)) post(k); }
        else if (r < 0.65) { if (d.use[g] > 0.0f && !d.banned[g] && MakeSupply(w, m, g, 8.0 + 30.0 * dice(), 1.12, k)) post(k); }
        else if (d.target[g] > 0.0f && t.target[g] > 0.0f && !t.banned[g] && !d.banned[g] && s.stock[g] > 25.0f &&
                 MakeHaulage(w, m, to, g, fmin(0.4 * s.stock[g], 10.0 + 40.0 * dice()), 1.0, k)) post(k);
    }
}

static void EnsureBoard(World &w, int m)
{
    if (m < 0 || m >= w.econ.Count()) return;
    uint64_t id = w.econ.base->defs[m].bodyId;
    auto it = w.boardMade.find(id);
    if (it != w.boardMade.end() && w.clock - it->second < BOARD_REFRESH) return;
    w.contracts.erase(std::remove_if(w.contracts.begin(), w.contracts.end(), [&](const Contract &k) { return k.fromBody == id && !k.wide; }),
                      w.contracts.end());
    w.boardMade[id] = w.clock;
    MakeBoard(w, m);
}

// ---------------------------------------------------------------------------
// A faction's list: what its agenda needs doing, and its worst shortages
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

// The market of a faction's (owned, or failing that in its space) nearest a place; 'want': one that deals in this good
static int MarketNear(const World &w, int faction, DVec3 ly, int want, bool starbase = false)
{
    const EconBase &base = *w.econ.base;
    int best = -1; double bestD = 1e30;
    for (int i = 0; i < (int)base.defs.size(); i++) {
        const MarketDef &d = base.defs[i];
        bool owned = w.econ.state[i].owner == faction;
        if (!owned && d.ruler != faction) continue;
        if (want >= 0 && (d.target[want] <= 0.0f || d.banned[want])) continue;
        double dist = Len(d.ly - ly) * (owned ? 1.0 : 2.0) * (starbase && d.settlement != Settlement::Starbase ? 3.0 : 1.0) + (owned ? 0.0 : 5.0);
        if (dist < bestD) { bestD = dist; best = i; }
    }
    return best;
}

static void MakeFactionBoard(World &w, const Faction &f)
{
    const EconBase &base = *w.econ.base;
    const FactionSet &fs = w.systems.factions;
    const Economy &e = w.econ;
    Dice dice{ Mix64(w.seed ^ (0xFAC7ull * (uint64_t)(f.id + 1)) ^ (uint64_t)(w.clock / BOARD_REFRESH + 1.0) * 0x9E3779B97F4A7C15ull) };
    Contract k;
    int posted = 0;
    auto post = [&](Contract &c) { c.wide = true; if (c.issuer == NO_FACTION) c.issuer = f.id; if (Post(w, c)) posted++; };
    auto arms = [&](int market, int good, double tonnes, ContractEffect effect, int on, int against) {
        if (market < 0 || !MakeSupply(w, market, good, tonnes, 1.6, k)) return;
        k.issuer = f.id; k.effect = effect; k.effectOn = on; k.against = against;
        k.allowed += TimeFor(8.0);
        post(k);
    };

    for (const AgendaItem &item : FactionAgenda(fs, f.id, 6)) {
        if (posted >= 3) break;
        const std::string &t = item.text;
        const Faction *other = fs.Get(item.target);
        if (Starts(t, "Hunt ") || Starts(t, "Patrol its trade lanes") || Starts(t, "Escort its convoys") || Starts(t, "Post bounties") ||
            Starts(t, "Keep the lanes") || Starts(t, "Drive ")) {
            // Its patrols need shells and fuel, at the base nearest the trouble
            int clan = other && other->kind == FactionKind::Clan ? other->id : NearestClan(fs, f.seatLy);
            if (clan == NO_FACTION || clan == f.id) continue;
            int good = dice() < 0.6 ? GOOD_MUNITIONS : REF_FUEL;
            arms(MarketNear(w, f.id, fs.list[clan].seatLy, good, true), good, 20.0 + 40.0 * dice(), ContractEffect::ArmPatrols, clan, clan);
        } else if (Starts(t, "Raid ") && f.kind == FactionKind::Clan) {
            int good = dice() < 0.5 ? GOOD_WEAPONS : GOOD_MUNITIONS;
            arms(MarketNear(w, f.id, f.seatLy, good), good, 8.0 + 20.0 * dice(), ContractEffect::ArmClan, f.id, item.target);
        } else if ((Starts(t, "Issue letters of marque") || Starts(t, "Hire raiders")) && other) {
            int clan = NearestClan(fs, other->seatLy, f.id);
            if (clan == NO_FACTION) continue;
            arms(MarketNear(w, clan, fs.list[clan].seatLy, GOOD_WEAPONS), GOOD_WEAPONS, 8.0 + 20.0 * dice(), ContractEffect::ArmClan, clan, other->id);
        } else if (Starts(t, "Fund ") && other && other->kind == FactionKind::Clan) {
            arms(MarketNear(w, other->id, other->seatLy, GOOD_MUNITIONS), GOOD_MUNITIONS, 8.0 + 20.0 * dice(), ContractEffect::ArmClan, other->id, NO_FACTION);
        }
    }
    // Someone has closed their borders to it: it pays well to get its goods through anyway
    const int F = fs.Count();
    for (int o = 0; o < F && posted < 4; o++) {
        if (o == f.id || e.embargoUntil[(size_t)o * F + f.id] <= w.clock) continue;
        int from = MarketNear(w, f.id, fs.list[o].seatLy, -1), to = -1;
        if (from < 0) continue;
        double bestD = 1e30;
        for (int i = 0; i < (int)base.defs.size(); i++)
            if (base.defs[i].ruler == o && Ly(base.defs[i], base.defs[from]) < bestD) { bestD = Ly(base.defs[i], base.defs[from]); to = i; }
        if (to < 0) continue;
        for (int tries = 0; tries < 20; tries++) {
            int g = (int)(dice() * GOOD_COUNT) % GOOD_COUNT;
            if (base.defs[from].role[g] != Role::Export || base.defs[to].target[g] <= 0.0f || base.defs[to].banned[g]) continue;
            if (!MakeHaulage(w, from, to, g, 20.0 + 50.0 * dice(), 2.0, k)) continue;
            k.issuer = f.id; k.against = o;
            post(k);
            break;
        }
    }
    // Guilds: work for their members, from their seat
    if (f.kind == FactionKind::Guild) {
        int seat = -1;                                             // (the free port's hub: every guild has a hall there)
        for (int i = 0; i < (int)base.defs.size(); i++) if (base.defs[i].sys == w.systems.home && base.defs[i].hub == i) seat = i;
        for (int n = 0; n < 3 && seat >= 0; n++)
            if (GuildJob(w, seat, f, dice, k)) { k.pay *= 1.2f; Gate(k, dice); post(k); }
    }
    // What its own places are worst short of
    std::vector<std::pair<double, std::pair<int, int>>> shorts;    // (value, (market, good))
    for (int i = 0; i < (int)base.defs.size(); i++) {
        if (e.state[i].owner != f.id && (base.defs[i].ruler != f.id || e.state[i].owner != NO_FACTION)) continue;
        std::vector<int> goods = Shortages(w, i, 0.5);
        if (goods.empty()) continue;
        int g = goods[0];
        shorts.push_back({ (base.defs[i].target[g] - e.state[i].stock[g]) * GoodData(g).basePrice, { i, g } });
    }
    std::sort(shorts.begin(), shorts.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (int i = 0; i < (int)shorts.size() && posted < 6; i++) {
        int m = shorts[i].second.first, g = shorts[i].second.second;
        double want = fmin(base.defs[m].target[g] - e.state[m].stock[g], 40.0 + 60.0 * dice());
        if (!MakeSupply(w, m, g, want, 1.35, k)) continue;
        k.issuer = f.id;
        if (f.kind == FactionKind::Guild) k.guild = f.id;
        k.allowed += TimeFor(10.0);
        if (ShockAt(w, m)) k.effect = ContractEffect::Relief;
        Gate(k, dice);
        post(k);
    }
}

static void EnsureFactionBoard(World &w, int faction)
{
    const Faction *f = w.systems.factions.Get(faction);
    if (!f) return;
    uint64_t key = BoardKey(faction);
    auto it = w.boardMade.find(key);
    if (it != w.boardMade.end() && w.clock - it->second < BOARD_REFRESH) return;
    w.contracts.erase(std::remove_if(w.contracts.begin(), w.contracts.end(),
                                     [&](const Contract &k) { return k.wide && (k.issuer == faction || k.guild == faction); }), w.contracts.end());
    w.boardMade[key] = w.clock;
    MakeFactionBoard(w, *f);
}

void UpdateContracts(World &w)
{
    if (w.clock - w.contractsChecked < 5.0 && w.clock >= w.contractsChecked) return;
    w.contractsChecked = w.clock;
    w.contracts.erase(std::remove_if(w.contracts.begin(), w.contracts.end(), [&](const Contract &k) { return k.expires < w.clock; }),
                      w.contracts.end());
    for (auto it = w.boardMade.begin(); it != w.boardMade.end();)
        it = w.clock - it->second > 3.0 * MARKET_DAY ? w.boardMade.erase(it) : std::next(it);
}

// ---------------------------------------------------------------------------
// Looking at boards
// ---------------------------------------------------------------------------
static bool Shown(const Captain &c, const Contract &k) { return k.guild == NO_FACTION || (c.Papers(k.guild) & PAPER_MEMBER); }

static void Record(World &w, Captain &c, uint64_t key, const std::function<bool(const Contract &)> &on)
{
    c.offers.erase(std::remove_if(c.offers.begin(), c.offers.end(), [&](const Contract &k) { return k.listKey == key; }), c.offers.end());
    for (const Contract &k : w.contracts)
        if (on(k) && Shown(c, k)) { c.offers.push_back(k); c.offers.back().listKey = key; }
    c.boards[key] = w.clock;
    if (std::find(c.boardsToSend.begin(), c.boardsToSend.end(), key) == c.boardsToSend.end()) c.boardsToSend.push_back(key);
}

void RecordBoard(World &w, Captain &c, const Body &b)
{
    EnsureEconomy(w);
    EnsureBoard(w, w.econ.MarketOf(b.id));
    Record(w, c, b.id, [&](const Contract &k) { return k.fromBody == b.id; });
}

bool RequestFactionBoard(World &w, Captain &c, int faction)
{
    if (!w.systems.factions.Get(faction)) return false;
    EnsureEconomy(w);
    EnsureFactionBoard(w, faction);
    Record(w, c, BoardKey(faction), [&](const Contract &k) { return k.wide && (k.issuer == faction || k.guild == faction); });
    return true;
}

// ---------------------------------------------------------------------------
// Taking, doing, dropping
// ---------------------------------------------------------------------------
static void Forget(Captain &c, uint32_t id)
{
    for (const Contract &k : c.offers)
        if (k.id == id && std::find(c.boardsToSend.begin(), c.boardsToSend.end(), k.listKey) == c.boardsToSend.end()) c.boardsToSend.push_back(k.listKey);
    c.offers.erase(std::remove_if(c.offers.begin(), c.offers.end(), [&](const Contract &k) { return k.id == id; }), c.offers.end());
}

bool AcceptContract(World &w, Captain &c, uint32_t id)
{
    auto it = std::find_if(w.contracts.begin(), w.contracts.end(), [&](const Contract &k) { return k.id == id; });
    if (it == w.contracts.end() || it->expires < w.clock) {
        Forget(c, id);
        Tell(w, c, "That contract has gone: someone else took it, or it lapsed");
        return false;
    }
    std::string bar = ContractBar(w, c, *it);
    if (!bar.empty()) { Tell(w, c, bar); return false; }
    Contract k = *it;
    w.contracts.erase(it);
    Forget(c, id);
    c.player.credits -= k.collateral;
    k.deadline = w.clock + k.allowed;
    k.loaded = !Collected(k.kind);
    k.listKey = 0;
    c.jobs.push_back(k);
    std::string msg = "Contract accepted: " + ContractTitle(k);
    if (k.collateral > 0.0f) msg += TextFormat("  (deposit %s cr)", Thousands(k.collateral).c_str());
    Tell(w, c, msg);
    return true;
}

static Contract *Job(Captain &c, uint32_t id)
{
    for (Contract &k : c.jobs) if (k.id == id) return &k;
    return nullptr;
}

static void Drop(Captain &c, uint32_t id)
{
    c.jobs.erase(std::remove_if(c.jobs.begin(), c.jobs.end(), [&](const Contract &k) { return k.id == id; }), c.jobs.end());
}

static void Done(World &w, Captain &c, const Contract &k)
{
    const FactionSet &fs = w.systems.factions;
    Economy &e = w.econ;
    bool late = w.clock > k.deadline;
    double pay = late ? 0.5 * k.pay : k.pay, regard = late ? 0.0 : fmin(6.0, 1.5 + k.pay / 3000.0);
    c.player.credits += pay + k.collateral;
    Bump(w, c, k.issuer, regard);
    if (k.guild != k.issuer) Bump(w, c, k.guild, regard);
    Bump(w, c, k.against, -0.7 * fmin(6.0, 1.5 + k.pay / 3000.0));
    std::string said = late ? TextFormat("Delivered late: half pay, %s cr", Thousands(pay).c_str()) : TextFormat("Contract complete: %s cr", Thousands(pay).c_str());
    if (k.collateral > 0.0f) said += TextFormat(", and your %s cr deposit back", Thousands(k.collateral).c_str());
    Tell(w, c, said);

    int to = e.MarketOf(k.toBody);
    const Faction *on = fs.Get(k.effectOn), *by = fs.Get(k.issuer);
    switch (k.effect) {
    case ContractEffect::Relief:
        for (Shock &s : e.shocks)
            if (s.market == to && s.until > w.clock) { s.until -= MARKET_DAY; PostNews(w, k.issuer, k.toSys, "Relief has reached " + k.toName); break; }
        break;
    case ContractEffect::ArmPatrols:
        if (on && on->id < (int)e.raid.size()) {
            e.raid[on->id] = fmaxf(0.05f, e.raid[on->id] - 0.15f);
            e.raidUntil[on->id] = fmax(e.raidUntil[on->id], w.clock + 3.0 * MARKET_DAY);
            PostNews(w, k.issuer, k.toSys, (by ? by->name : k.toName) + " patrols, resupplied at " + k.toName + ", are pressing " + on->name);
        }
        break;
    case ContractEffect::ArmClan:
        if (on && on->id < (int)e.raid.size()) {
            e.raid[on->id] = fminf(1.0f, e.raid[on->id] + 0.15f);
            e.raidUntil[on->id] = fmax(e.raidUntil[on->id], w.clock + 3.0 * MARKET_DAY);
            PostNews(w, on->id, k.toSys, on->name + " have come by new arms and are bolder for it");
        }
        break;
    case ContractEffect::Charts:
        PostNews(w, k.issuer, k.surveySys, (by ? by->name : k.fromName) + " adds " + k.surveyName + " to its charts");
        break;
    default: break;
    }
}

bool AdvanceContract(World &w, Captain &c, uint32_t id)
{
    Contract *job = Job(c, id);
    if (!job) return false;
    Contract &k = *job;
    EnsureEconomy(w);
    Economy &e = w.econ;
    uint64_t at = DockedAt(c);
    if (!k.loaded) {
        if (at != k.fromBody) { Tell(w, c, "Dock at " + k.fromName + " to collect"); return false; }
        if (IssuersCargo(k.kind)) {
            if (c.player.spec.hold - c.player.CargoTotal() + 1e-6 < k.tonnes) { Tell(w, c, TextFormat("Needs %.0f t of room in the hold", k.tonnes)); return false; }
            c.player.cargo[k.good] += k.tonnes;
            int m = e.MarketOf(k.fromBody);
            if (m >= 0 && k.kind == ContractKind::Haulage) e.state[m].stock[k.good] = fmaxf(0.0f, e.state[m].stock[k.good] - k.tonnes);
            Tell(w, c, TextFormat("Loaded %.0f t %s for %s", k.tonnes, GoodData(k.good).name, k.toName.c_str()));
        } else {
            Tell(w, c, "Dispatches aboard for " + k.toName);
        }
        k.loaded = true;
        return true;
    }
    if (k.kind == ContractKind::Survey) {
        if (k.Scanned() < k.need) { Tell(w, c, TextFormat("%d more worlds of %s to scan", k.need - k.Scanned(), k.surveyName.c_str())); return false; }
        if (at != k.fromBody) { Tell(w, c, "Dock at " + k.fromName + " to hand the survey in"); return false; }
    } else {
        if (at != k.toBody) { Tell(w, c, "Dock at " + k.toName + " to deliver"); return false; }
        if (k.kind != ContractKind::Courier) {
            if (c.player.cargo[k.good] + 1e-6 < k.tonnes) {
                Tell(w, c, TextFormat("Needs %.0f t %s: you have %.1f t", k.tonnes, GoodData(k.good).name, c.player.cargo[k.good]));
                return false;
            }
            c.player.cargo[k.good] -= k.tonnes;
            if (c.player.cargo[k.good] < 1e-6) c.player.cargo[k.good] = 0.0;
            int m = e.MarketOf(k.toBody);
            if (m >= 0 && e.base->defs[m].target[k.good] > 0.0f) e.state[m].stock[k.good] += k.tonnes;
        }
    }
    Contract done = k;
    Drop(c, id);
    Done(w, c, done);
    return true;
}

// Out of time, or given up after the cargo was aboard
static void Fail(World &w, Captain &c, const Contract &k, const char *why)
{
    double penalty = fmin(8.0, 3.0 + k.pay / 4000.0);
    Bump(w, c, k.issuer, -penalty);
    if (k.guild != k.issuer) Bump(w, c, k.guild, -penalty);
    std::string said = std::string(why) + ": " + ContractTitle(k);
    if (k.collateral > 0.0f && k.loaded) said += TextFormat("  -  your %s cr deposit is forfeit", Thousands(k.collateral).c_str());
    else if (k.collateral > 0.0f) { c.player.credits += k.collateral; said += "  -  deposit returned"; }
    Tell(w, c, said);
}

bool AbandonContract(World &w, Captain &c, uint32_t id)
{
    Contract *job = Job(c, id);
    if (!job) return false;
    Contract k = *job;
    Drop(c, id);
    if (IssuersCargo(k.kind) && k.loaded) { Fail(w, c, k, "Contract abandoned"); return true; }
    // Nothing of theirs aboard: it goes back on the board, and they think a little less of you
    c.player.credits += k.collateral;
    Bump(w, c, k.issuer, -1.5);
    if (k.guild != k.issuer) Bump(w, c, k.guild, -1.5);
    Tell(w, c, "Contract handed back: " + ContractTitle(k));
    if (k.expires > w.clock) { k.deadline = 0.0; k.loaded = false; k.scanMask = 0; w.contracts.push_back(k); }
    return true;
}

bool JoinGuild(World &w, Captain &c, int faction)
{
    const Faction *f = w.systems.factions.Get(faction);
    if (!f || f->kind != FactionKind::Guild) return false;
    if (c.Papers(faction) & PAPER_MEMBER) { Tell(w, c, "You're already a member of " + f->name); return false; }
    if (f->guild == GuildKind::BountyHunters) { Tell(w, c, f->name + " aren't taking anyone on yet"); return false; }
    if (c.Standing(faction) < -20.0f) { Tell(w, c, f->name + " won't have you"); return false; }
    if (c.player.credits < GUILD_FEE) { Tell(w, c, TextFormat("Joining costs %s cr", Thousands(GUILD_FEE).c_str())); return false; }
    c.player.credits -= GUILD_FEE;
    c.SetPaper(faction, PAPER_MEMBER);
    Tell(w, c, "Welcome to " + f->name + ": its work is yours at any station with a chapter");
    return true;
}

bool BuyLicence(World &w, Captain &c, int faction, int paper)
{
    const Faction *f = w.systems.factions.Get(faction);
    if (!f || (f->kind != FactionKind::Government && f->kind != FactionKind::FreePort) || (paper != PAPER_CARRIER && paper != PAPER_PERMIT)) return false;
    bool carrier = paper == PAPER_CARRIER;
    const char *name = carrier ? "bonded carrier's licence" : "restricted goods permit";
    double cost = carrier ? CARRIER_COST : PERMIT_COST;
    float  need = carrier ? CARRIER_STANDING : PERMIT_STANDING;
    if (c.Papers(faction) & paper) { Tell(w, c, std::string("You already hold a ") + name + " from " + f->name); return false; }
    if (!carrier && f->contraband.empty()) { Tell(w, c, f->name + " bans nothing: no permit needed"); return false; }
    if (c.Standing(faction) < need) { Tell(w, c, std::string("A ") + name + " needs standing: " + RankName(*f, need) + " with " + f->name); return false; }
    if (c.player.credits < cost) { Tell(w, c, TextFormat("A %s costs %s cr", name, Thousands(cost).c_str())); return false; }
    c.player.credits -= cost;
    c.SetPaper(faction, (uint8_t)paper);
    Tell(w, c, f->name + " grants you a " + name);
    return true;
}

// ---------------------------------------------------------------------------
// Each step: customs, surveys, deadlines
// ---------------------------------------------------------------------------
// Docking at a lawful port with what its government bans
static void Customs(World &w, Captain &c, const Body &b)
{
    int m = w.econ.MarketOf(b.id);
    if (m < 0) return;
    const MarketDef &d = w.econ.base->defs[m];
    const Faction *law = w.systems.factions.Get(d.ruler);
    if (!law || (c.Papers(law->id) & PAPER_PERMIT)) return;
    double tonnes = 0.0;
    for (int g = 0; g < GOOD_COUNT; g++) if (d.banned[g] && c.player.cargo[g] > 0.05) tonnes += c.player.cargo[g];
    if (tonnes <= 0.0) return;
    Dice dice{ Mix64(w.seed ^ b.id ^ (uint64_t)(w.clock * 10.0) ^ ((uint64_t)c.id << 40)) };
    if (dice() > 0.25 + 0.6 * law->policing) { Tell(w, c, b.name + " customs wave you in without a search"); return; }
    SearchHold(w, c, law->id, b.name + " customs");
}

bool SearchHold(World &w, Captain &c, int lawId, const std::string &who)
{
    const Faction *law = w.systems.factions.Get(lawId);
    if (!law || (c.Papers(law->id) & PAPER_PERMIT)) return false;
    double value = 0.0, tonnes = 0.0;
    for (int g : law->contraband)
        if (c.player.cargo[g] > 0.05) { value += c.player.cargo[g] * GoodData(g).basePrice; tonnes += c.player.cargo[g]; c.player.cargo[g] = 0.0; }
    if (tonnes <= 0.0) return false;
    double fine = fmin(c.player.credits, 0.5 * value);
    c.player.credits -= fine;
    Bump(w, c, law->id, -fmin(12.0, 4.0 + value / 5000.0));
    Tell(w, c, TextFormat("%s searched the hold: %.0f t of contraband seized, fined %s cr", who.c_str(), tonnes, Thousands(fine).c_str()));
    return true;
}

void StepContracts(World &w, Captain &c)
{
    // Just come into dock somewhere
    int near = c.flight.nearBody;
    uint64_t at = DockedAt(c);
    if (at != c.dockedAt) {
        c.dockedAt = at;
        if (at != 0 && w.econ.Built() && near >= 0) {
            Customs(w, c, c.loaded.bodies[near]);
            for (const Contract &k : c.jobs) {
                bool here = false;
                std::string what = JobStatus(w, c, k, &here);
                if (here) { Tell(w, c, what + "  -  J: contracts"); break; }
            }
        }
    }
    // Surveys: pass close to a world of the system to scan it
    for (Contract &k : c.jobs) {
        if (k.kind != ContractKind::Survey || c.loaded.index != k.surveySys || k.Scanned() >= k.need) continue;
        for (int i = 0; i < (int)c.loaded.bodies.size() && i < 64; i++) {
            const Body &b = c.loaded.bodies[i];
            if (b.kind == Kind::Star || b.kind == Kind::Station || (k.scanMask >> i & 1)) continue;
            double up = Len(FromShip(w.systems, c.player.ship, c.loaded.index, b.local)) - b.radius;
            if (up > fmax(SCAN_RADII * b.radius, SCAN_MIN)) continue;
            k.scanMask |= 1ull << i;
            Tell(w, c, k.Scanned() >= k.need ? "Survey of " + k.surveyName + " complete  -  take it back to " + k.fromName
                                             : TextFormat("Surveyed %s  (%d of %d)", b.name.c_str(), k.Scanned(), k.need));
        }
    }
    // Deadlines: half pay once it's late, failed once it's half as late again
    for (size_t i = 0; i < c.jobs.size();) {
        Contract &k = c.jobs[i];
        if (w.clock > k.deadline + 0.5 * k.allowed) {
            Contract gone = k;
            c.jobs.erase(c.jobs.begin() + i);
            gone.loaded = gone.loaded && IssuersCargo(gone.kind);  // (a deposit never collected against comes back)
            Fail(w, c, gone, "Contract failed, out of time");
            continue;
        }
        i++;
    }
}

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
static std::string Line(const char *key, const Contract &k)
{
    return TextFormat("%s %u %d %d %d %d %d %d %d %.9g %d %llu %llu %lld %lld %lld %d %llu %d %.9g %.9g %.9g %.17g %.17g %.17g %d %llu",
                      key, k.id, (int)k.kind, (int)k.effect, k.issuer, k.guild, k.against, k.effectOn, k.licence, k.minStanding, k.wide ? 1 : 0,
                      (unsigned long long)k.fromBody, (unsigned long long)k.toBody, (long long)k.fromSys, (long long)k.toSys, (long long)k.surveySys,
                      k.need, (unsigned long long)k.scanMask, k.good, k.tonnes, k.pay, k.collateral, k.expires, k.allowed, k.deadline,
                      k.loaded ? 1 : 0, (unsigned long long)k.listKey)
           + std::string(" |") + k.fromName + "|" + k.toName + "|" + k.surveyName + TextFormat("|%d", k.law) + "\n";
}

static bool Parse(const char *rest, Contract &k)
{
    int kind, effect, wide, loaded, used = 0;
    unsigned long long from, to, mask, key;
    long long fromSys, toSys, surveySys;
    if (sscanf(rest, "%u %d %d %d %d %d %d %d %f %d %llu %llu %lld %lld %lld %d %llu %d %f %f %f %lf %lf %lf %d %llu%n",
               &k.id, &kind, &effect, &k.issuer, &k.guild, &k.against, &k.effectOn, &k.licence, &k.minStanding, &wide, &from, &to,
               &fromSys, &toSys, &surveySys, &k.need, &mask, &k.good, &k.tonnes, &k.pay, &k.collateral, &k.expires, &k.allowed, &k.deadline,
               &loaded, &key, &used) != 26) return false;
    if (kind < 0 || kind >= (int)ContractKind::Count || effect < 0 || effect >= (int)ContractEffect::Count || k.good >= GOOD_COUNT) return false;
    if (k.good < 0 && kind != (int)ContractKind::Survey && kind != (int)ContractKind::Courier) return false;
    k.kind = (ContractKind)kind; k.effect = (ContractEffect)effect; k.wide = wide != 0; k.loaded = loaded != 0;
    k.fromBody = from; k.toBody = to; k.scanMask = mask; k.listKey = key;
    k.fromSys = fromSys; k.toSys = toSys; k.surveySys = surveySys;
    std::string names = rest + used;
    while (!names.empty() && (names.back() == '\n' || names.back() == '\r')) names.pop_back();
    size_t a = names.find('|');
    if (a == std::string::npos) return true;
    size_t b = names.find('|', a + 1), c = b == std::string::npos ? b : names.find('|', b + 1);
    if (b == std::string::npos || c == std::string::npos) return true;
    k.fromName = names.substr(a + 1, b - a - 1); k.toName = names.substr(b + 1, c - b - 1); k.surveyName = names.substr(c + 1);
    size_t d = k.surveyName.find('|');                             // (newer saves: whose contraband it is)
    if (d != std::string::npos) { k.law = atoi(k.surveyName.c_str() + d + 1); k.surveyName.resize(d); }
    return true;
}

void WriteContracts(const World &w, std::string &out)
{
    out += TextFormat("contractseq %u\n", w.contractSeq);
    for (const auto &b : w.boardMade) out += TextFormat("boardmade %llu %.17g\n", (unsigned long long)b.first, b.second);
    for (const Contract &k : w.contracts) out += Line("contract", k);
}

bool ReadContractLine(World &w, const char *key, const char *rest)
{
    if (!strcmp(key, "contractseq")) { sscanf(rest, "%u", &w.contractSeq); return true; }
    if (!strcmp(key, "boardmade")) {
        unsigned long long id; double at;
        if (sscanf(rest, "%llu %lf", &id, &at) == 2) w.boardMade[id] = at;
        return true;
    }
    if (!strcmp(key, "contract")) {
        Contract k;
        if (Parse(rest, k)) { w.contracts.push_back(k); w.contractSeq = std::max(w.contractSeq, k.id); }
        return true;
    }
    return false;
}

void WriteCaptainContracts(const Captain &c, std::string &out)
{
    for (size_t i = 0; i < c.papers.size(); i++) if (c.papers[i]) out += TextFormat("papers %d %d\n", (int)i, (int)c.papers[i]);
    for (const Contract &k : c.jobs) out += Line("job", k);
    for (const auto &b : c.boards) out += TextFormat("boardseen %llu %.17g\n", (unsigned long long)b.first, b.second);
    for (const Contract &k : c.offers) out += Line("offer", k);
}

bool ReadCaptainContractLine(Captain &c, const char *key, const char *rest)
{
    if (!strcmp(key, "papers")) {
        int f, bits;
        if (sscanf(rest, "%d %d", &f, &bits) == 2 && f >= 0 && f < 4096) c.SetPaper(f, (uint8_t)bits);
        return true;
    }
    if (!strcmp(key, "boardseen")) {
        unsigned long long id; double at;
        if (sscanf(rest, "%llu %lf", &id, &at) == 2) c.boards[id] = at;
        return true;
    }
    bool job = !strcmp(key, "job");
    if (job || !strcmp(key, "offer")) {
        Contract k;
        if (Parse(rest, k)) (job ? c.jobs : c.offers).push_back(k);
        return true;
    }
    return false;
}
