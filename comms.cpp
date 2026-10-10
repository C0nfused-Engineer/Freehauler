// comms.cpp - hailing worlds, and what they say back
#include "comms.h"
#include "economy.h"
#include "format.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// What worlds and stations say
// ---------------------------------------------------------------------------
static std::string Greeting(const Body &b)
{
    struct Line { Settlement settlement; const char *text[2]; };
    static const Line lines[] = {
        { Settlement::CoreWorld,       { "%s Orbital Control. Welcome, captain - traffic is heavy today, keep to your assigned lane.",
                                "This is %s Approach. We have you on scope, captain. Go ahead." } },
        { Settlement::SettledWorld,    { "This is %s Traffic. Go ahead, captain.",
                                "%s Port Authority, receiving. What can we do for you?" } },
        { Settlement::DomedColonies,   { "%s Dome Authority here. We read you, captain.",
                                "%s Colonial Comms. Good to hear a new voice out here - go ahead." } },
        { Settlement::OrbitalHabitats, { "%s Habitat Ring, receiving. Mind the tankers on the way in.",
                                "This is %s Skimmer Control. Go ahead, but keep it short - storm season." } },
        { Settlement::ResearchStation, { "%s Research, duty officer speaking. We don't get many visitors.",
                                "%s Station here. Please don't fly through our sensor field, captain." } },
        { Settlement::ResearchOutpost, { "%s Outpost. Two of us out here and we're both listening. Go ahead.",
                                "This is %s Survey Camp. Surprised anyone's out this way." } },
        { Settlement::MiningOutpost,   { "%s Mining Co-op. Make it quick, captain, shift change in ten.",
                                "This is %s Pit Boss. If you're hauling ore, we're listening." } },
        { Settlement::OrbitalPort,     { "%s Control. Docking ring's clear, captain - match spin and come on in.",
                                "This is %s. Welcome aboard, captain; market deck is open." } },
        { Settlement::Shipyard,        { "%s Yard Control. Keep clear of the slipways, captain.",
                                "This is %s. Unless you're delivering steel, make it quick." } },
        { Settlement::SkimmerStation,  { "%s here. Tanks are full and fuel's cheap - go ahead.",
                                "This is %s. Watch the scoop booms on approach, captain." } },
        { Settlement::OreTerminal,     { "%s Dispatch. Got ore? We've got credits.",
                                "This is %s. Loading arms are busy - state your business." } },
        { Settlement::OreRefinery,     { "%s, smelter control. Bring us rock, captain, any rock.",
                                "This is %s. Mind the slag plume. Go ahead." } },
        { Settlement::Waystation,      { "%s, receiving. Fuel, food, and a little of everything.",
                                "This is %s. Long way between worlds, captain - rest up." } },
        { Settlement::Freeport,        { "%s. No tariffs, no questions. What are you buying?",
                                "This is %s. Everyone's welcome out here, captain." } },
        { Settlement::UndergroundColony, { "%s Deep Colony comms. Signal's weak down here - go ahead.",
                                "This is %s Undercity. We hear you, captain." } },
    };
    int pick = (int)(Mix64(b.id + 7) & 1);
    for (const Line &l : lines)
        if (b.settlement == l.settlement) return TextFormat(l.text[pick], b.name.c_str());
    return TextFormat("This is %s. Go ahead, captain.", b.name.c_str());
}

static std::vector<std::string> MarketReport(const Body &b)
{
    Market m = MakeMarket(b);
    std::vector<std::string> out;
    // Lists goods of one role with a price, wrapped a few to a line
    auto list = [&](const char *head, Role r, bool buying) {
        std::string line = head;
        int n = 0;
        for (int g = 0; g < GOOD_COUNT; g++) {
            if (m.role[g] != r) continue;
            if (n && n % 3 == 0) { out.push_back(line); line = "   "; }
            line += TextFormat("  %s %.0f", GoodData(g).name, buying ? m.BuyPrice(g) : m.SellPrice(g));
            n++;
        }
        if (n) out.push_back(line);
        return n;
    };
    int ex = list("Selling cheap:", Role::Export, true);
    int im = list("Paying well for:", Role::Import, false);
    if (!ex && !im) list("Trading:", Role::Trade, true);
    out.push_back(TextFormat("Prices in cr/t. Fuel is %.0f cr/t.", m.fuelPrice));
    return out;
}

static std::string LocalInfo(const Body &b)
{
    Market m = MakeMarket(b);
    std::string s = TextFormat("%s, population %s. ", SettlementName(b.settlement), FormatPopulation(b.population).c_str());
    if (b.kind == Kind::Station) s += std::string(EconName(b.econ)) + " economy. ";
    else {
        s += "World type: " + b.type + ". ";
        if (!b.atmosphere.empty()) s += "Atmosphere: " + b.atmosphere + ". ";
        if (b.lifeLevel > 0 && !b.life.empty()) s += "Native life: " + b.life + ". ";
        s += std::string(EconName(b.econ)) + " economy. ";
    }
    std::string ex = ExportsLine(m, 4), im = ImportsLine(m, 4);
    if (!ex.empty()) s += "We make " + ex + ". ";
    if (!im.empty()) s += "We need " + im + ".";
    return s;
}

// ---------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------
std::vector<CommsContact> CommsInRange(const World &w, const Captain &c)
{
    std::vector<CommsContact> out;
    if (c.loaded.index < 0) return out;
    for (int bi = 0; bi < (int)c.loaded.bodies.size(); bi++) {
        const Body &b = c.loaded.bodies[bi];
        if (b.population <= 0) continue;
        double d = fmax(0.0, Len(FromShip(w.systems, c.player.ship, c.loaded.index, b.local)) - b.radius);
        if (d <= COMMS_RANGE) out.push_back({ bi, d });
    }
    std::sort(out.begin(), out.end(), [](const CommsContact &a, const CommsContact &b) { return a.dist < b.dist; });
    return out;
}

const CommsContact *FindContact(const std::vector<CommsContact> &list, TargetId who)
{
    for (const CommsContact &c : list)
        if (who.type == TargetId::Body && c.body == who.index) return &c;
    return nullptr;
}

bool CommsWaiting(const World &w, const Captain &c)
{
    return !c.comms.log.empty() && c.comms.log.back().at > w.clock;
}

static double RoundTrip(double dist) { return 2.0 * dist / LIGHT_SEC_U; }

void Hail(World &w, Captain &c, int body)
{
    auto list = CommsInRange(w, c);
    const CommsContact *cc = FindContact(list, TargetId::MakeBody(c.loaded.index, body));
    if (!cc) return;
    const Body &b = c.loaded.bodies[body];
    c.comms.with = TargetId::MakeBody(c.loaded.index, body);
    c.comms.withName = b.name;
    c.comms.log.clear();
    c.comms.log.push_back({ w.clock, true, "Hailing " + b.name + "..." });
    c.comms.log.push_back({ w.clock + RoundTrip(cc->dist) + 0.6, false, Greeting(b) });
    w.Emit(EventType::CommsOpened, "", c.comms.with, -1, 0.0, 0.0, c.id);
}

void RequestMarketReport(World &w, Captain &c)
{
    auto list = CommsInRange(w, c);
    const CommsContact *cc = FindContact(list, c.comms.with);
    if (!cc || CommsWaiting(w, c)) return;
    c.comms.log.push_back({ w.clock, true, "Requesting a market report." });
    double at = w.clock + RoundTrip(cc->dist) + 0.8;
    for (const std::string &l : MarketReport(c.loaded.bodies[cc->body])) c.comms.log.push_back({ at, false, l });
}

void RequestLocalInfo(World &w, Captain &c)
{
    auto list = CommsInRange(w, c);
    const CommsContact *cc = FindContact(list, c.comms.with);
    if (!cc || CommsWaiting(w, c)) return;
    const Body &b = c.loaded.bodies[cc->body];
    c.comms.log.push_back({ w.clock, true, "What can you tell me about " + b.name + "?" });
    c.comms.log.push_back({ w.clock + RoundTrip(cc->dist) + 0.8, false, LocalInfo(b) });
}

void HangUp(World &w, Captain &c)
{
    if (!c.comms.with.valid()) return;
    w.Emit(EventType::CommsClosed, "Channel to " + c.comms.withName + " closed", c.comms.with, -1, 0.0, 0.0, c.id);
    c.comms.with = {};
}

void UpdateComms(World &w, Captain &c)
{
    CommsState &cs = c.comms;
    if (cs.log.size() > 40) cs.log.erase(cs.log.begin(), cs.log.end() - 40);
    if (!cs.with.valid()) return;
    auto list = CommsInRange(w, c);
    if (cs.with.system == c.loaded.index && FindContact(list, cs.with)) return;
    // Out of range, or the system unloaded under us
    w.Emit(EventType::CommsLost, "Lost signal with " + cs.withName, cs.with, -1, 0.0, 0.0, c.id);
    cs.with = {};
}
