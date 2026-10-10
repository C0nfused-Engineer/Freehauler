// factions.cpp - generating the factions, their territory and what they own
#include "factions.h"
#include "galaxy.h"
#include "goods.h"
#include <algorithm>
#include <cctype>

// Draws for faction generation (core.h's portable generator: the same on every machine)
struct Dice {
    Rng64 r;
    explicit Dice(uint64_t seed) : r{ Mix64(seed) } {}
    double uni(double a, double b) { return a + (b - a) * r.uni(); }
    int    pick(int n) { return std::min(n - 1, (int)(r.uni() * n)); }
    bool   chance(double p) { return r.uni() < p; }
};
static float Clamp01f(double x) { return (float)(x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x); }
static double Hash01(uint64_t h) { return (double)(Mix64(h) >> 11) * (1.0 / 9007199254740992.0); }

const char *KindName(FactionKind k)
{
    switch (k) {
    case FactionKind::FreePort:     return "Free port";
    case FactionKind::Government:   return "Government";
    case FactionKind::Conglomerate: return "Conglomerate";
    case FactionKind::Guild:        return "Guild";
    default:                        return "Pirate clan";
    }
}

const char *SlantName(Slant s)
{
    static const char *names[] = { "Heavy industry", "Shipping", "Agriculture and biotech", "Energy", "Finance", "Mining" };
    return names[(int)s % (int)Slant::Count];
}

const char *TraitName(int t)
{
    static const char *names[TR_COUNT] = { "Aggression", "Ambition", "Greed", "Caution", "Honor", "Zeal" };
    return t >= 0 && t < TR_COUNT ? names[t] : "";
}

const char *AxisName(int a, bool positive)
{
    static const char *names[AX_COUNT][2] = { { "Libertarian", "Authoritarian" }, { "Free-trading", "Protectionist" },
                                              { "Pacifist", "Militarist" }, { "Insular", "Expansionist" } };
    return a >= 0 && a < AX_COUNT ? names[a][positive ? 1 : 0] : "";
}

std::string IdeologyBlurb(const Faction &f)
{
    // The axes it feels strongly about, strongest first
    int order[AX_COUNT] = { 0, 1, 2, 3 };
    std::sort(order, order + AX_COUNT, [&](int a, int b) { return fabsf(f.axis[a]) > fabsf(f.axis[b]); });
    std::string s;
    for (int a : order)
        if (fabsf(f.axis[a]) >= 0.3f) {
            std::string word = AxisName(a, f.axis[a] > 0.0f);
            if (!s.empty()) word[0] = (char)tolower(word[0]);
            s += (s.empty() ? "" : ", ") + word;
        }
    return s.empty() ? "Moderate" : s;
}

const char *StanceName(float r)
{
    return r >= 45.0f ? "Allied" : r >= 15.0f ? "Friendly" : r > -20.0f ? "Neutral" : r > -50.0f ? "Rivals" : "Hostile";
}

const char *RankName(const Faction &f, float s)
{
    int step = s < -60.0f ? 0 : s < -20.0f ? 1 : s < 20.0f ? 2 : s < 50.0f ? 3 : s < 80.0f ? 4 : 5;
    static const char *state[]   = { "Enemy of the state", "Under suspicion", "Foreigner", "Resident", "Citizen", "Honoured citizen" };
    static const char *company[] = { "Blacklisted", "Poor risk", "Unknown", "Contractor", "Preferred partner", "Executive partner" };
    static const char *guild[]   = { "Expelled", "Unwelcome", "Outsider", "Associate", "Member", "Master" };
    static const char *clan[]    = { "Marked for death", "Prey", "Stranger", "Tolerated", "Friend of the clan", "Blood kin" };
    switch (f.kind) {
    case FactionKind::Conglomerate: return company[step];
    case FactionKind::Guild:        return guild[step];
    case FactionKind::Clan:         return clan[step];
    default:                        return state[step];
    }
}

std::string GoalText(const FactionSet &fs, const Goal &g)
{
    switch (g.type) {
    case GoalType::Expand:   return "Grow: more systems, more stations";
    case GoalType::Secure:   return "Keep its borders and trade lanes safe";
    case GoalType::Humble:   return "Bring down " + fs.Name(g.target, "its rivals");
    case GoalType::Wealth:   return "Get rich";
    case GoalType::Convert:  return "See its way of life spread";
    case GoalType::Monopoly: return "Own its line of business outright";
    case GoalType::Plunder:  return "Take what others carry";
    default:                 return "Look after its own";
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
static std::string GovernmentForm(const Faction &f, Dice &d, std::vector<std::string> &used)
{
    static const char *const FORMS[][3] = {
        { "Hegemony", "Dominion", "Imperium" },           { "Directorate", "Union", "Collective" },
        { "Protectorate", "Mandate", "Sovereignty" },     { "Free League", "Merchant Republic", "Concord" },
        { "Republic", "Commonwealth", "Assembly" },       { "Trade Federation", "Exchange", "Hanse" },
        { "Commune", "Cooperative", "Syndicate" },        { "Federation", "Ascendancy", "Reach" },
        { "Confederation", "Alliance", "Compact" } };
    float o = f.axis[AX_ORDER], m = f.axis[AX_MARKETS], w = f.axis[AX_FORCE], x = f.axis[AX_OUTLOOK];
    int kind = o > 0.3f && w > 0.3f ? 0 : o > 0.3f && m > 0.3f ? 1 : o > 0.3f ? 2 : o < -0.3f && m < -0.3f ? 3 : o < -0.3f ? 4
             : m < -0.3f ? 5 : m > 0.3f ? 6 : x > 0.3f ? 7 : 8;
    // One of its own kind's names that no other state is using; failing that, one of the plain ones
    int start = d.pick(3);
    for (int i = 0; i < 6; i++) {
        std::string form = FORMS[i < 3 ? kind : 8][(start + i) % 3];
        if (std::find(used.begin(), used.end(), form) == used.end()) { used.push_back(form); return form; }
    }
    return FORMS[kind][start];
}

static std::string CompanyStem(Dice &d)
{
    static const char *a[] = { "Hal", "Or", "Tann", "Vey", "Kess", "Mar", "Dra", "Sol", "Ar", "Bren", "Cas", "Lu", "No", "Py", "Ren", "Ty" };
    static const char *b[] = { "vern", "ix", "hauser", "lan", "ler", "dane", "ko", "veig", "gent", "mar", "tor", "men", "vak", "re", "nor", "sen" };
    return std::string(a[d.pick(16)]) + b[d.pick(16)];
}

static std::string CompanyName(Slant s, Dice &d)
{
    static const char *suffix[(int)Slant::Count][3] = {
        { "Heavy Industries", "Works", "Foundries" }, { "Lines", "Transit", "Freight" }, { "Biosystems", "Agrarian", "Harvest" },
        { "Energy", "Fusion", "Power" }, { "Capital", "Holdings", "Trust" }, { "Extraction", "Mining", "Deep Core" } };
    std::string stem = CompanyStem(d);
    if (d.chance(0.35)) stem += "-" + CompanyStem(d);
    return stem + " " + suffix[(int)s][d.pick(3)];
}

static std::string ClanName(Dice &d, std::vector<std::string> &used)
{
    static const char *a[] = { "Red", "Ash", "Black", "Hollow", "Dust", "Pale", "Iron", "Broken", "Silent", "Burnt", "Cold", "Last",
                               "Crooked", "Starving", "Blind", "Rust" };
    static const char *b[] = { "Wake", "Reavers", "Lanterns", "Suns", "Kings", "Hand", "Jackals", "Tide", "Choir", "Wolves", "Ledger",
                               "Crows", "Knives", "Orphans", "Comet", "Sons" };
    // (no word twice among the clans, while there are words left: 'used' holds the words, not the names)
    for (int tries = 0; tries < 200; tries++) {
        const char *first = a[d.pick(16)], *second = b[d.pick(16)];
        bool fresh = std::find(used.begin(), used.end(), first) == used.end() && std::find(used.begin(), used.end(), second) == used.end();
        if (!fresh && tries < 150) continue;
        used.push_back(first); used.push_back(second);
        return std::string("The ") + first + " " + second;
    }
    return "The Nameless";
}

static Color FromHue(double hue, double sat, double val)
{
    double h = fmod(fmod(hue, 360.0) + 360.0, 360.0) / 60.0, c = val * sat, x = c * (1.0 - fabs(fmod(h, 2.0) - 1.0)), m = val - c;
    double r = h < 1 ? c : h < 2 ? x : h < 4 ? 0 : h < 5 ? x : c;
    double g = h < 1 ? x : h < 3 ? c : h < 4 ? x : 0;
    double b = h < 2 ? 0 : h < 3 ? x : h < 5 ? c : x;
    return { (unsigned char)((r + m) * 255), (unsigned char)((g + m) * 255), (unsigned char)((b + m) * 255), 255 };
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------
static double AxisDistance(const Faction &a, const Faction &b)
{
    double s = 0.0;
    for (int i = 0; i < AX_COUNT; i++) s += (a.axis[i] - b.axis[i]) * (a.axis[i] - b.axis[i]);
    return sqrt(s);                                                // 0 .. 4
}

// Temperament follows ideology, loosely
static void RollTraits(Faction &f, Dice &d)
{
    auto n = [&]() { return d.uni(-0.2, 0.2); };
    f.trait[TR_AGGRESSION] = Clamp01f(0.45 + 0.35 * f.axis[AX_FORCE] + n());
    f.trait[TR_AMBITION]   = Clamp01f(0.50 + 0.35 * f.axis[AX_OUTLOOK] + n());
    f.trait[TR_GREED]      = Clamp01f(0.50 - 0.25 * f.axis[AX_MARKETS] + n());
    f.trait[TR_CAUTION]    = Clamp01f(0.50 - 0.30 * f.axis[AX_FORCE] - 0.15 * f.axis[AX_OUTLOOK] + n());
    f.trait[TR_HONOR]      = Clamp01f(0.50 + 0.15 * f.axis[AX_ORDER] + d.uni(-0.35, 0.35));
    f.trait[TR_ZEAL]       = Clamp01f(0.15 + 0.3 * fabsf(f.axis[AX_ORDER]) + 0.25 * fabsf(f.axis[AX_MARKETS]) + n());
}

// Somewhere a clan could work from: a station out past the borders
static bool HasStation(const StarSystem &s)
{
    LoadedSystem L;
    GenerateSystemContents(s, L);
    for (const Body &b : L.bodies) if (b.kind == Kind::Station) return true;
    return false;
}

// A system someone could run things from: a steady Sun-like star with a settled, breathable world
static bool HasSettledWorld(const StarSystem &s)
{
    LoadedSystem L;
    GenerateSystemContents(s, L);
    for (const Body &b : L.bodies)
        if (b.settlement == Settlement::CoreWorld || b.settlement == Settlement::SettledWorld) return true;
    return false;
}

struct Site { const StarSystem *sys; DVec3 ly; };

// The candidate farthest from everything already taken (and at least 'clear' from it), or -1
static int Farthest(const std::vector<Site> &sites, const std::vector<DVec3> &taken, double clear, const std::vector<bool> &dead)
{
    int best = -1; double bestD = clear;
    for (int i = 0; i < (int)sites.size(); i++) {
        if (dead[i]) continue;
        double nearest = 1e30;
        for (const DVec3 &t : taken) nearest = fmin(nearest, Len(sites[i].ly - t));
        if (nearest > bestD) { bestD = nearest; best = i; }
    }
    return best;
}

FactionSet GenerateFactions(const Galaxy &g)
{
    FactionSet fs;
    Dice d(Mix64(g.seed ^ 0xFAC7105ull));
    const StarSystem &home = g[g.home];
    fs.homeCell[0] = home.cx; fs.homeCell[1] = home.cy; fs.homeCell[2] = home.cz;

    std::vector<const StarSystem *> all;
    g.Nearby(g.home, {}, FRONTIER_RADIUS_LY * 1.25 * LY_U, all);
    std::sort(all.begin(), all.end(), [](const StarSystem *a, const StarSystem *b) { return a->id < b->id; });
    auto lyOf = [&](const StarSystem *s) {
        return DVec3{ (s->cx - home.cx) * CELL_LY, (s->cy - home.cy) * CELL_LY, (s->cz - home.cz) * CELL_LY };
    };
    auto add = [&](FactionKind kind) -> Faction & {
        Faction f;
        f.id = fs.Count(); f.kind = kind;
        fs.list.push_back(f);
        return fs.list.back();
    };
    auto steady = [](const StarSystem *s) {
        const StarInfo &st = s->stars[0];
        return st.kind == StarKind::MainSequence && st.mass > 0.6 && st.mass < 1.4 && s->age >= 2.5;
    };

    // --- The free port: home ---
    {
        Faction &f = add(FactionKind::FreePort);
        f.name = home.name + " Free Port";
        f.color = { 235, 235, 235, 255 };
        f.seat = g.home;
        f.axis[AX_ORDER] = -0.3f; f.axis[AX_MARKETS] = -0.9f; f.axis[AX_FORCE] = -0.5f;
        RollTraits(f, d);
        f.trait[TR_CAUTION] = 0.8f; f.trait[TR_HONOR] = 0.8f;
        f.policing = 0.9f;                                         // (safe, and nothing is banned or taxed)
        f.goals = { { GoalType::Serve, NO_FACTION, 1.0f }, { GoalType::Wealth, NO_FACTION, 0.6f } };
    }

    // --- Governments: capitals spread through settled space, ideologies spread through the possibilities ---
    const int governments = 5 + d.pick(3);
    {
        std::vector<Site> sites;
        for (const StarSystem *s : all)
            if (s->id != g.home && s->develop >= 0.4f && s->develop <= 0.9f && steady(s)) sites.push_back({ s, lyOf(s) });
        std::vector<bool> dead(sites.size(), false);
        std::vector<DVec3> taken = { DVec3{} };                    // (keep clear of the free port too)
        std::vector<std::string> forms;
        const double hue0 = d.uni(0.0, 360.0);
        while ((int)taken.size() - 1 < governments) {
            int i = Farthest(sites, taken, 8.0, dead);
            if (i < 0) break;
            dead[i] = true;
            if (!HasSettledWorld(*sites[i].sys)) continue;
            Faction &f = add(FactionKind::Government);
            int nth = (int)taken.size() - 1;
            f.seat = sites[i].sys->id; f.seatLy = sites[i].ly;
            taken.push_back(sites[i].ly);
            // Ideology: of a batch of random ones, the one least like the states that already exist
            Faction best = f; double bestD = -1.0;
            for (int k = 0; k < 40; k++) {
                Faction c = f;
                for (int a = 0; a < AX_COUNT; a++) c.axis[a] = (float)d.uni(-1.0, 1.0);
                double nearest = 1e30;
                for (const Faction &o : fs.list) if (o.kind == FactionKind::Government && o.id != f.id) nearest = fmin(nearest, AxisDistance(c, o));
                if (nearest > bestD) { bestD = nearest; best = c; }
            }
            for (int a = 0; a < AX_COUNT; a++) f.axis[a] = best.axis[a];
            RollTraits(f, d);
            f.strength = (float)d.uni(0.85, 1.25);                 // roughly equal; some a bit stronger
            f.name     = sites[i].sys->name + " " + GovernmentForm(f, d, forms);
            f.color    = FromHue(hue0 + 360.0 * nth / governments, 0.6, 1.0);
            f.tariff   = (float)(0.03 + 0.11 * (f.axis[AX_MARKETS] + 1.0));
            f.policing = Clamp01f(fmax(0.15, 0.55 + 0.35 * f.axis[AX_ORDER] + 0.1 * f.axis[AX_FORCE]));
            // What it bans says what it fears
            struct Ban { int good; double urge; };
            const Ban bans[] = { { GOOD_SPIRITS,      0.4 * f.axis[AX_ORDER] - 0.4 * f.axis[AX_OUTLOOK] },
                                 { GOOD_LUXURY,       0.7 * f.axis[AX_MARKETS] },
                                 { GOOD_EXOTIC_BIO,   0.3 * f.axis[AX_ORDER] - 0.5 * f.axis[AX_OUTLOOK] },
                                 { GOOD_ROBOTICS,    -0.7 * f.axis[AX_OUTLOOK] },
                                 { GOOD_RESEARCH_DATA, 0.6 * f.axis[AX_ORDER] },
                                 { ORE_URANIUM,      -0.7 * f.axis[AX_FORCE] },
                                 { GOOD_REACTOR_PARTS, -0.5 * f.axis[AX_FORCE] + 0.2 * f.axis[AX_ORDER] } };
            for (const Ban &b : bans) if (b.urge + d.uni(-0.15, 0.15) > 0.3) f.contraband.push_back(b.good);
        }
    }

    // --- Conglomerates: each with a line it leans to, headquartered somewhere busy ---
    const int companies = 5 + d.pick(2);
    {
        std::vector<Site> sites;
        std::vector<DVec3> taken;
        for (const Faction &f : fs.list) taken.push_back(f.seatLy);
        for (const StarSystem *s : all)
            if (s->id != g.home && s->develop >= 0.4f && steady(s)) sites.push_back({ s, lyOf(s) });
        std::vector<bool> dead(sites.size(), false);
        int slants[(int)Slant::Count];
        for (int i = 0; i < (int)Slant::Count; i++) slants[i] = i;
        for (int i = (int)Slant::Count - 1; i > 0; i--) std::swap(slants[i], slants[d.pick(i + 1)]);
        std::vector<std::string> names;
        for (int c = 0; c < companies; c++) {
            Faction &f = add(FactionKind::Conglomerate);
            f.slant = (Slant)slants[c % (int)Slant::Count];
            do f.name = CompanyName(f.slant, d); while (std::find(names.begin(), names.end(), f.name) != names.end());
            names.push_back(f.name);
            int i = Farthest(sites, taken, 4.0, dead);
            if (i >= 0) { dead[i] = true; f.seat = sites[i].sys->id; f.seatLy = sites[i].ly; taken.push_back(f.seatLy); }
            else        { f.seat = g.home; }
            f.axis[AX_ORDER]   = (float)d.uni(-0.6, 0.6);
            f.axis[AX_MARKETS] = (float)d.uni(-1.0, -0.4);         // business likes open markets
            f.axis[AX_FORCE]   = (float)d.uni(-0.6, 0.4);
            f.axis[AX_OUTLOOK] = (float)d.uni(0.0, 1.0);
            RollTraits(f, d);
            f.trait[TR_GREED] = Clamp01f(f.trait[TR_GREED] + 0.25);
            f.strength = (float)d.uni(0.8, 1.3);
            f.color = FromHue(d.uni(0.0, 360.0), 0.3, 0.9);
        }
    }

    // --- Guilds: the same four careers in every galaxy, seated at the free port ---
    {
        struct G { GuildKind kind; const char *name; Color color; };
        const G guilds[] = { { GuildKind::Traders, "Traders' Guild", { 255, 215, 120, 255 } },
                             { GuildKind::Explorers, "Explorers' Society", { 140, 255, 220, 255 } },
                             { GuildKind::Colonial, "Colonial League", { 170, 230, 140, 255 } },
                             { GuildKind::BountyHunters, "Bounty Hunters' Lodge", { 255, 150, 110, 255 } } };
        for (const G &gd : guilds) {
            Faction &f = add(FactionKind::Guild);
            f.guild = gd.kind; f.name = gd.name; f.color = gd.color; f.seat = g.home;
            f.axis[AX_ORDER]   = gd.kind == GuildKind::BountyHunters ? 0.4f : -0.3f;
            f.axis[AX_MARKETS] = gd.kind == GuildKind::Traders ? -0.8f : -0.2f;
            f.axis[AX_FORCE]   = gd.kind == GuildKind::BountyHunters ? 0.7f : -0.3f;
            f.axis[AX_OUTLOOK] = gd.kind == GuildKind::Explorers || gd.kind == GuildKind::Colonial ? 0.8f : 0.0f;
            RollTraits(f, d);
            f.trait[TR_HONOR] = Clamp01f(f.trait[TR_HONOR] + 0.2);
        }
    }

    // --- Clans: small, and many: out on the frontier, well apart, each with its own patch ---
    const int clans = 10 + d.pick(5);
    {
        std::vector<Site> sites;
        for (const StarSystem *s : all)
            if (s->develop > 0.0f && TerritoryRuler(fs, lyOf(s), s->develop, CellHashOf(g.seed, *s)) == NO_FACTION) sites.push_back({ s, lyOf(s) });
        std::vector<bool> dead(sites.size(), false);
        std::vector<DVec3> taken;
        std::vector<std::string> names;
        // The first anywhere; the rest as far from each other as they can get. Each needs a station to
        // work from: that's its base (AssignOwners gives it to them).
        for (int tries = 0; tries < 200 && taken.empty() && !sites.empty(); tries++) {
            int first = d.pick((int)sites.size());
            if (dead[first]) continue;
            dead[first] = true;
            if (!HasStation(*sites[first].sys)) continue;
            taken.push_back(sites[first].ly);
            Faction &f = add(FactionKind::Clan);
            f.seat = sites[first].sys->id; f.seatLy = sites[first].ly;
        }
        while (!taken.empty() && (int)taken.size() < clans) {
            int i = Farthest(sites, taken, 6.0, dead);
            if (i < 0) break;
            dead[i] = true;
            if (!HasStation(*sites[i].sys)) continue;
            Faction &f = add(FactionKind::Clan);
            f.seat = sites[i].sys->id; f.seatLy = sites[i].ly;
            taken.push_back(f.seatLy);
        }
        for (Faction &f : fs.list) {
            if (f.kind != FactionKind::Clan) continue;
            f.name  = ClanName(d, names);
            f.reach = (float)d.uni(7.0, 12.0);
            f.axis[AX_ORDER]   = (float)d.uni(-1.0, -0.3);
            f.axis[AX_MARKETS] = (float)d.uni(-1.0, 0.0);
            f.axis[AX_FORCE]   = (float)d.uni(0.4, 1.0);
            f.axis[AX_OUTLOOK] = (float)d.uni(-0.5, 0.5);
            RollTraits(f, d);
            f.trait[TR_HONOR] = Clamp01f(f.trait[TR_HONOR] - 0.25);
            f.strength = (float)d.uni(0.15, 0.4);
            f.color = FromHue(d.uni(-25.0, 25.0) + 360.0, d.uni(0.6, 0.9), d.uni(0.7, 1.0));
            // Some raid for a paymaster: a hard-nosed state nearby that would rather not be seen doing it
            double best = 1e30;
            for (const Faction &gov : fs.list) {
                if (gov.kind != FactionKind::Government || gov.axis[AX_FORCE] < 0.0f || gov.trait[TR_HONOR] > 0.6f) continue;
                double dist = Len(gov.seatLy - f.seatLy);
                if (dist < best) { best = dist; f.patron = gov.id; }
            }
            if (!d.chance(0.35)) f.patron = NO_FACTION;
        }
    }

    // --- How they all get on ---
    const int n = fs.Count();
    fs.relation.assign((size_t)n * n, 0.0f);
    auto set = [&](int a, int b, double v) { fs.relation[a * n + b] = fs.relation[b * n + a] = (float)fmax(-100.0, fmin(100.0, v)); };
    for (int a = 0; a < n; a++) {
        fs.relation[a * n + a] = 100.0f;
        for (int b = a + 1; b < n; b++) {
            const Faction &A = fs.list[a], &B = fs.list[b];
            FactionKind ka = A.kind, kb = B.kind;
            double noise = d.uni(-10.0, 10.0), v = 0.0;
            double apart = Len(A.seatLy - B.seatLy);
            const Faction &clan = ka == FactionKind::Clan ? A : B, &other = ka == FactionKind::Clan ? B : A;
            if (ka == FactionKind::Clan && kb == FactionKind::Clan)
                v = -10.0 - 30.0 * fmax(0.0, 1.0 - apart / 25.0) + noise;                // neighbours fight over the same lanes
            else if (ka == FactionKind::Clan || kb == FactionKind::Clan) {
                if (other.kind == FactionKind::Guild)
                    v = other.guild == GuildKind::BountyHunters ? -90.0 : -45.0 + noise;
                else if (other.kind == FactionKind::Government)
                    v = clan.patron == other.id ? 25.0 + noise : -35.0 - 55.0 * other.policing + noise;
                else
                    v = -60.0 + noise;                                                   // conglomerates and the free port: their cargo is the prey
            }
            else if (ka == FactionKind::Government && kb == FactionKind::Government) {
                // Like minds get on; neighbours with ambitions don't; soldiers trust nobody
                double friction = fmax(0.0, 1.0 - apart / 45.0) * (A.trait[TR_AMBITION] + B.trait[TR_AMBITION]) * 22.0;
                // (the states are picked to differ, so 'like' is relative: about 2 apart is ordinary)
                v = 68.0 - 34.0 * AxisDistance(A, B) - friction - 14.0 * (A.trait[TR_AGGRESSION] + B.trait[TR_AGGRESSION] - 1.0) + 1.8 * noise;
            }
            else if (ka == FactionKind::Conglomerate && kb == FactionKind::Conglomerate)
                v = -15.0 - 20.0 * fmax(0.0, 1.0 - apart / 30.0) - 20.0 * (A.trait[TR_GREED] + B.trait[TR_GREED] - 1.0) + noise;
            else if (ka == FactionKind::FreePort || kb == FactionKind::FreePort)
                v = 30.0 + noise * 0.5;                                                  // everybody needs the free port
            else {
                // A state and a company or guild: open markets welcome business, closed ones don't
                const Faction &gov = ka == FactionKind::Government ? A : B, &biz = ka == FactionKind::Government ? B : A;
                if (gov.kind == FactionKind::Government)
                    v = 15.0 - 35.0 * gov.axis[AX_MARKETS] + (apart < 12.0 ? 20.0 : 0.0) + noise;
                else
                    v = 20.0 + noise;                                                    // companies and guilds: partners, mostly
                if (biz.kind == FactionKind::Guild && biz.guild == GuildKind::BountyHunters && gov.kind == FactionKind::Government)
                    v = 10.0 + 40.0 * gov.policing + noise;
            }
            set(a, b, v);
        }
    }

    // However the dice fell, there's a feud and a friendship among the states: stretch their
    // relations so the worst pair are rivals and the best are friends
    {
        float lo = 1e9f, hi = -1e9f;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                if (fs.list[a].kind == FactionKind::Government && fs.list[b].kind == FactionKind::Government) {
                    lo = fminf(lo, fs.Relation(a, b)); hi = fmaxf(hi, fs.Relation(a, b));
                }
        float toLo = fminf(lo, -45.0f), toHi = fmaxf(hi, 30.0f);
        for (int a = 0; a < n && hi > lo; a++)
            for (int b = a + 1; b < n; b++)
                if (fs.list[a].kind == FactionKind::Government && fs.list[b].kind == FactionKind::Government)
                    set(a, b, toLo + (fs.Relation(a, b) - lo) / (hi - lo) * (toHi - toLo));
    }

    // --- Goals: what each one's character and situation make it want ---
    for (Faction &f : fs.list) {
        int rival = NO_FACTION; float worst = -20.0f;
        for (const Faction &o : fs.list)
            if (o.id != f.id && o.kind == f.kind && fs.Relation(f.id, o.id) < worst) { worst = fs.Relation(f.id, o.id); rival = o.id; }
        struct Want { GoalType type; int target; double weight; };
        std::vector<Want> wants;
        auto want = [&](GoalType type, int target, double weight) { wants.push_back({ type, target, weight }); };
        switch (f.kind) {
        case FactionKind::Government:
            want(GoalType::Expand, NO_FACTION, f.trait[TR_AMBITION]); want(GoalType::Secure, NO_FACTION, f.trait[TR_CAUTION]); want(GoalType::Wealth, NO_FACTION, f.trait[TR_GREED]); want(GoalType::Convert, NO_FACTION, f.trait[TR_ZEAL]);
            if (rival != NO_FACTION) want(GoalType::Humble, rival, f.trait[TR_AGGRESSION] * (0.6 - worst / 100.0));
            break;
        case FactionKind::Conglomerate:
            want(GoalType::Wealth, NO_FACTION, f.trait[TR_GREED]); want(GoalType::Monopoly, NO_FACTION, 0.5 + 0.4 * f.trait[TR_AMBITION]); want(GoalType::Expand, NO_FACTION, f.trait[TR_AMBITION]); want(GoalType::Secure, NO_FACTION, f.trait[TR_CAUTION] * 0.8);
            if (rival != NO_FACTION) want(GoalType::Humble, rival, 0.3 + f.trait[TR_AGGRESSION] * 0.6);
            break;
        case FactionKind::Clan:
            want(GoalType::Plunder, NO_FACTION, 1.0); want(GoalType::Wealth, NO_FACTION, f.trait[TR_GREED]); want(GoalType::Expand, NO_FACTION, f.trait[TR_AMBITION] * 0.8);
            if (rival != NO_FACTION) want(GoalType::Humble, rival, f.trait[TR_AGGRESSION] * 0.7);
            break;
        default:
            want(GoalType::Serve, NO_FACTION, 1.0); want(GoalType::Secure, NO_FACTION, f.trait[TR_CAUTION]); want(GoalType::Expand, NO_FACTION, f.trait[TR_AMBITION] * 0.7);
            break;
        }
        for (Want &w : wants) w.weight += d.uni(-0.12, 0.12);
        std::sort(wants.begin(), wants.end(), [](const Want &a, const Want &b) { return a.weight > b.weight; });
        if (f.kind != FactionKind::FreePort) f.goals.clear();
        for (size_t i = 0; i < wants.size() && f.goals.size() < 3; i++)
            if (f.kind != FactionKind::FreePort) f.goals.push_back({ wants[i].type, wants[i].target, Clamp01f(wants[i].weight) });
    }
    return fs;
}

// ---------------------------------------------------------------------------
// Territory and ownership
// ---------------------------------------------------------------------------
int TerritoryRuler(const FactionSet &fs, DVec3 ly, float develop, uint64_t hash)
{
    if (fs.list.empty() || develop <= 0.0f) return NO_FACTION;
    if (Len(ly) < 0.5 * CELL_LY) return 0;                         // home: the free port
    // The frontier is nobody's: beyond every state's reach, with a ragged edge
    if (develop < 0.10f || (develop < 0.24f && Hash01(hash ^ 0x7E221ull) < 0.5)) return NO_FACTION;
    int best = NO_FACTION; double bestD = 1e30;
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Government) continue;
        double dist = Len(ly - f.seatLy) / f.strength;             // the stronger a state, the farther its border
        if (dist < bestD) { bestD = dist; best = f.id; }
    }
    return best;
}

// Which conglomerate has its name on something: the big ones, the near ones, and the ones whose line it is
static int PickCompany(const FactionSet &fs, DVec3 ly, Slant line, uint64_t hash)
{
    double total = 0.0;
    std::vector<std::pair<int, double>> odds;
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Conglomerate) continue;
        double w = f.strength * (f.slant == line ? 3.0 : 1.0) / (1.0 + Len(ly - f.seatLy) / 25.0);
        odds.push_back({ f.id, w });
        total += w;
    }
    double r = Hash01(hash) * total;
    for (const auto &o : odds) { if (r < o.second) return o.first; r -= o.second; }
    return odds.empty() ? NO_FACTION : odds.back().first;
}

static int GuildOf(const FactionSet &fs, GuildKind kind)
{
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Guild && f.guild == kind) return f.id;
    return NO_FACTION;
}

void AssignOwners(const FactionSet &fs, const StarSystem &sys, LoadedSystem &L)
{
    if (fs.list.empty()) return;
    const int ruler = sys.ruler;
    DVec3 ly = { (sys.cx - fs.homeCell[0]) * CELL_LY, (sys.cy - fs.homeCell[1]) * CELL_LY, (sys.cz - fs.homeCell[2]) * CELL_LY };
    // A clan that haunts this stretch of space (unclaimed space only: a state would have burned them out)
    int clan = NO_FACTION;
    if (ruler == NO_FACTION)
        for (const Faction &f : fs.list)
            if (f.kind == FactionKind::Clan && Len(ly - f.seatLy) <= f.reach &&
                (clan == NO_FACTION || Len(ly - f.seatLy) < Len(ly - fs.list[clan].seatLy))) clan = f.id;

    int lair = NO_FACTION;                                          // this is a clan's own base
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan && f.seat == sys.id) lair = f.id;

    for (Body &b : L.bodies) {
        b.owner = NO_FACTION;
        if (b.population <= 0) continue;
        if (lair != NO_FACTION && b.kind == Kind::Station) { b.owner = lair; continue; }
        double  roll = Hash01(b.id ^ 0x0A11E5ull);
        uint64_t pick = b.id ^ 0xC0A9ull;
        auto company = [&](Slant line) { return PickCompany(fs, ly, line, pick); };
        // 'state': the ruler if there is one. Out past the borders, the colonial league looks after settlers.
        const int state  = ruler != NO_FACTION ? ruler : GuildOf(fs, GuildKind::Colonial);
        if (ruler == 0) { b.owner = 0; continue; }                 // the free port runs everything at home
        switch (b.settlement) {
        case Settlement::CoreWorld: case Settlement::SettledWorld: case Settlement::Starbase:
            b.owner = ruler; break;                                // (unclaimed: they govern themselves)
        case Settlement::DomedColonies: case Settlement::UndergroundColony:
            b.owner = roll < 0.75 ? state : company(Slant::Industry); break;
        case Settlement::OrbitalHabitats: case Settlement::SkimmerStation: case Settlement::SolarCollector:
            b.owner = roll < 0.7 ? company(Slant::Energy) : roll < 0.85 ? ruler : NO_FACTION; break;
        case Settlement::MiningOutpost: case Settlement::OreTerminal: case Settlement::OreRefinery:
            b.owner = roll < 0.65 ? company(Slant::Mining) : roll < 0.8 ? ruler : NO_FACTION; break;
        case Settlement::Shipyard:
            b.owner = roll < 0.6 ? company(Slant::Industry) : roll < 0.9 ? ruler : NO_FACTION; break;
        case Settlement::Greenhouse:
            b.owner = roll < 0.6 ? company(Slant::Agriculture) : roll < 0.8 ? state : NO_FACTION; break;
        case Settlement::OrbitalPort:
            b.owner = roll < 0.55 ? ruler : roll < 0.85 ? company(Slant::Shipping) : NO_FACTION; break;
        case Settlement::ResearchStation: case Settlement::ResearchOutpost:
            b.owner = roll < 0.4 ? GuildOf(fs, GuildKind::Explorers) : roll < 0.7 ? ruler : company(Slant::Finance); break;
        case Settlement::Waystation:
            b.owner = roll < 0.3 ? GuildOf(fs, GuildKind::Traders) : roll < 0.55 ? company(Slant::Shipping) : NO_FACTION; break;
        case Settlement::Freeport:
            b.owner = clan != NO_FACTION && roll < 0.7 ? clan : roll < 0.85 ? NO_FACTION : GuildOf(fs, GuildKind::Traders); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Agendas
// ---------------------------------------------------------------------------
// Every candidate has a base worth from the situation, a temperament that favours or
// disfavours it (weights on the six traits), and the goals it serves. Score:
//   base * (1 + traits . weights) * (1 + the weights of the goals it serves)
std::vector<AgendaItem> FactionAgenda(const FactionSet &fs, int id, int max)
{
    std::vector<AgendaItem> out;
    const Faction *self = fs.Get(id);
    if (!self) return out;
    const Faction &f = *self;

    auto serves = [&](GoalType type, int target = NO_FACTION) {
        float w = 0.0f;
        for (const Goal &g : f.goals) if (g.type == type && (g.target == NO_FACTION || target == NO_FACTION || g.target == target)) w += g.weight;
        return w;
    };
    //                                  aggr  ambit greed caut  honor zeal
    auto add = [&](const std::string &text, int target, double base, const float w[TR_COUNT], float goals) {
        double temper = 0.0;
        for (int t = 0; t < TR_COUNT; t++) temper += f.trait[t] * w[t];
        double score = base * fmax(0.1, 1.0 + temper) * (1.0 + goals);
        if (score > 0.05) out.push_back({ text, (float)score, target });
    };
    static const float W_PRESS[]   = {  0.9f,  0.4f,  0.0f, -0.8f, -0.1f,  0.3f };   // lean on a rival
    static const float W_PACT[]    = { -0.5f,  0.0f,  0.2f,  0.6f,  0.4f, -0.3f };   // make a friend
    static const float W_CLAIM[]   = {  0.2f,  1.0f,  0.2f, -0.4f,  0.0f,  0.2f };   // push the border out
    static const float W_PATROL[]  = {  0.2f,  0.0f,  0.2f,  0.7f,  0.3f,  0.0f };   // guard what it has
    static const float W_TRADE[]   = { -0.3f,  0.2f,  0.9f,  0.1f,  0.0f, -0.2f };   // do business
    static const float W_RAID[]    = {  0.9f,  0.3f,  0.6f, -0.7f, -0.5f,  0.0f };   // take it instead
    static const float W_PROXY[]   = {  0.5f,  0.3f,  0.2f,  0.2f, -0.9f,  0.0f };   // pay someone else to
    static const float W_PREACH[]  = {  0.0f,  0.3f, -0.2f,  0.0f,  0.2f,  1.0f };   // win minds

    for (const Faction &o : fs.list) {
        if (o.id == f.id) continue;
        float  rel   = fs.Relation(f.id, o.id);
        double near  = fmax(0.0, 1.0 - Len(f.seatLy - o.seatLy) / 60.0);              // 1 next door .. 0 across human space
        double enmity = fmax(0.0, -rel / 100.0), amity = fmax(0.0, rel / 100.0);
        switch (f.kind) {
        case FactionKind::Government:
            if (o.kind == FactionKind::Government) {
                add("Press its border with " + o.name, o.id, enmity * (0.4 + near), W_PRESS, serves(GoalType::Humble, o.id) + 0.5f * serves(GoalType::Expand));
                add("Seek a pact with " + o.name, o.id, amity * (0.5 + 0.5 * near) + 0.15 * near, W_PACT, serves(GoalType::Secure));
                if (f.axis[AX_FORCE] > -0.2f && rel < -20.0f)
                    add("Issue letters of marque against " + o.name + " shipping", o.id, enmity * 0.8, W_PROXY, serves(GoalType::Humble, o.id));
                add("Win over the people of " + o.name, o.id, 0.25 * near * AxisDistance(f, o) / 2.0, W_PREACH, serves(GoalType::Convert));
            } else if (o.kind == FactionKind::Clan) {
                if (o.patron == f.id) add("Fund " + o.name + "'s raids", o.id, 0.5, W_PROXY, serves(GoalType::Humble));
                else                  add("Hunt " + o.name + " out of its lanes", o.id, enmity * near * 0.9 * f.policing, W_PATROL, serves(GoalType::Secure));
            } else if (o.kind == FactionKind::Conglomerate) {
                if (rel >= 0.0f) add("Court investment from " + o.name, o.id, amity * 0.6 + 0.1, W_TRADE, serves(GoalType::Wealth));
                else             add("Seize " + o.name + "'s stations in its space", o.id, enmity * 0.7, W_PRESS, serves(GoalType::Wealth) * 0.5f);
            }
            break;
        case FactionKind::Conglomerate:
            if (o.kind == FactionKind::Conglomerate) {
                add("Undercut " + o.name, o.id, enmity * (0.5 + near) + (o.slant == f.slant ? 0.3 : 0.0), W_TRADE, serves(GoalType::Humble, o.id) + serves(GoalType::Monopoly));
                add("Hire raiders against " + o.name + "'s freight", o.id, enmity * 0.6, W_PROXY, serves(GoalType::Humble, o.id));
            } else if (o.kind == FactionKind::Government) {
                if (rel < 10.0f) add("Lobby " + o.name + " for access", o.id, 0.45 * o.strength, W_TRADE, serves(GoalType::Expand));
                else             add("Build more in " + o.name + " space", o.id, amity * 0.7 * o.strength, W_CLAIM, serves(GoalType::Expand) + serves(GoalType::Wealth) * 0.5f);
            } else if (o.kind == FactionKind::Clan) {
                add("Escort its convoys past " + o.name, o.id, near * 0.5, W_PATROL, serves(GoalType::Secure));
            }
            break;
        case FactionKind::Clan:
            if (o.kind == FactionKind::Government || o.kind == FactionKind::Conglomerate)
                add("Raid " + o.name + " shipping", o.id, (0.3 + near) * (o.patron == f.id || f.patron == o.id ? 0.0 : 1.0) * (1.2 - (o.kind == FactionKind::Government ? o.policing : 0.4)),
                    W_RAID, serves(GoalType::Plunder) + serves(GoalType::Wealth) * 0.5f);
            else if (o.kind == FactionKind::Clan)
                add("Drive " + o.name + " off its patch", o.id, enmity * near * 1.2, W_PRESS, serves(GoalType::Humble, o.id) + serves(GoalType::Expand));
            break;
        case FactionKind::Guild:
            if (f.guild == GuildKind::BountyHunters && o.kind == FactionKind::Clan)
                add("Post bounties on " + o.name, o.id, 0.4 + 0.5 * o.strength, W_PATROL, serves(GoalType::Serve));
            else if (o.kind == FactionKind::Government && f.guild != GuildKind::BountyHunters)
                add(std::string(f.guild == GuildKind::Colonial ? "Petition " : "Open a chapter with ") + o.name, o.id, 0.2 + amity * 0.6, W_TRADE, serves(GoalType::Expand));
            break;
        default:
            if (o.kind == FactionKind::Government && rel < 20.0f)
                add("Mend relations with " + o.name, o.id, 0.4 - rel / 200.0, W_PACT, serves(GoalType::Secure) + serves(GoalType::Serve));
            break;
        }
    }
    // The ones that aren't about anybody in particular
    if (f.kind == FactionKind::Government) {
        add("Claim unclaimed systems on its frontier", NO_FACTION, 0.45, W_CLAIM, serves(GoalType::Expand));
        add("Patrol its trade lanes", NO_FACTION, 0.35 + 0.3 * f.policing, W_PATROL, serves(GoalType::Secure));
        add(f.axis[AX_MARKETS] > 0.0f ? "Raise tariffs" : "Cut tariffs to draw trade", NO_FACTION, 0.3, W_TRADE, serves(GoalType::Wealth));
    } else if (f.kind == FactionKind::Conglomerate) {
        add(std::string("Buy out independent ") + (f.slant == Slant::Mining ? "refineries" : f.slant == Slant::Energy ? "skimmers" : "stations"),
            NO_FACTION, 0.45, W_CLAIM, serves(GoalType::Monopoly) + serves(GoalType::Expand));
    } else if (f.kind == FactionKind::Clan) {
        add("Lie low and refit", NO_FACTION, 0.3, W_PACT, 0.0f);
    } else if (f.kind == FactionKind::Guild && f.guild == GuildKind::Explorers) {
        add("Sponsor surveys beyond the frontier", NO_FACTION, 0.6, W_CLAIM, serves(GoalType::Serve) + serves(GoalType::Expand));
    } else if (f.kind == FactionKind::Guild && f.guild == GuildKind::Colonial) {
        add("Found colonies in unclaimed systems", NO_FACTION, 0.6, W_CLAIM, serves(GoalType::Serve) + serves(GoalType::Expand));
    } else if (f.kind == FactionKind::Guild && f.guild == GuildKind::Traders) {
        add("Keep the lanes to the free port open", NO_FACTION, 0.6, W_PATROL, serves(GoalType::Serve) + serves(GoalType::Secure));
    } else if (f.kind == FactionKind::FreePort) {
        add("Stay everybody's neutral ground", NO_FACTION, 0.7, W_PACT, serves(GoalType::Serve));
    }
    std::sort(out.begin(), out.end(), [](const AgendaItem &a, const AgendaItem &b) { return a.score > b.score; });
    if ((int)out.size() > max) out.resize(max);
    return out;
}
