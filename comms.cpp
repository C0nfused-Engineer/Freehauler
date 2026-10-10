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
        { Settlement::Starbase,        { "%s Approach Control. You are cleared for the space doors, captain - thrusters only inside.",
                                "This is %s. Welcome, captain. All docks are busy; expect a short hold." } },
        { Settlement::SolarCollector,  { "%s, power control. Stay out of the beam path, captain, for your own sake.",
                                "This is %s. Mind the panels - they cost more than your ship." } },
        { Settlement::Greenhouse,      { "%s here. If you've brought fertilizer, you're our favourite captain today.",
                                "This is %s. Harvest's in - come and see what fresh food looks like." } },
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
        out.push_back({ bi, d });
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

int HailTarget(const Captain &c)
{
    auto lives = [&](int bi) { return bi >= 0 && bi < (int)c.loaded.bodies.size() && c.loaded.bodies[bi].population > 0; };
    const TargetId &sel = c.flight.selected;
    if (sel.type == TargetId::Body && sel.system == c.loaded.index && lives(sel.index)) return sel.index;
    return lives(c.flight.nearBody) ? c.flight.nearBody : -1;
}

bool CommsAnswered(const World &w, const Captain &c, TargetId with)
{
    const CommsChannel *ch = c.comms.Find(with);
    if (!ch) return false;
    for (const CommsMsg &m : ch->log) if (!m.ours && m.at <= w.clock) return true;
    return false;
}

bool CommsWaiting(const World &w, const Captain &c, TargetId with)
{
    const CommsChannel *ch = c.comms.Find(with);
    if (!ch) return false;
    for (const CommsMsg &m : ch->log) if (!m.ours && m.at > w.clock) return true;
    return false;
}

bool CanTradeOverComms(const World &w, const Captain &c)
{
    int near = c.flight.nearBody;
    return near >= 0 && near < (int)c.loaded.bodies.size() && c.loaded.bodies[near].population > 0 &&
           CommsAnswered(w, c, TargetId::MakeBody(c.loaded.index, near));
}

std::vector<CommsFlight> CommsInFlight(const World &w, const Captain &c)
{
    std::vector<CommsFlight> out;
    for (const CommsChannel &ch : c.comms.channels)
        for (const CommsMsg &m : ch.log) {
            if (m.ours || m.at <= w.clock) continue;
            bool listed = false;                                  // (a report's lines all travel together)
            for (const CommsFlight &f : out) if (f.with == ch.with && f.at == m.at) listed = true;
            if (!listed) out.push_back({ ch.with, ch.withName, m.sent, m.at });
        }
    std::sort(out.begin(), out.end(), [](const CommsFlight &a, const CommsFlight &b) { return a.at < b.at; });
    return out;
}

static double RoundTrip(double dist) { return 2.0 * dist / LIGHT_SEC_U; }

// Sends 'ask' to a body with an open channel, and queues the lines of its reply (which
// arrive after the round trip plus 'think' seconds)
static void Ask(World &w, Captain &c, int body, const std::string &ask, const std::vector<std::string> &reply, double think)
{
    auto list = CommsInRange(w, c);
    TargetId with = TargetId::MakeBody(c.loaded.index, body);
    const CommsContact *cc = FindContact(list, with);
    CommsChannel *ch = c.comms.Find(with);
    if (!cc || !ch) return;
    ch->log.push_back({ w.clock + cc->dist / LIGHT_SEC_U, true, ask });
    double at = w.clock + RoundTrip(cc->dist) + think;
    for (const std::string &l : reply) {
        CommsMsg m = { at, false, l };
        m.sent = w.clock;
        ch->log.push_back(m);
    }
    if (ch->log.size() > 40) ch->log.erase(ch->log.begin(), ch->log.end() - 40);
}

void Hail(World &w, Captain &c, int body)
{
    auto list = CommsInRange(w, c);
    TargetId with = TargetId::MakeBody(c.loaded.index, body);
    if (!FindContact(list, with) || c.comms.Find(with)) return;
    const Body &b = c.loaded.bodies[body];
    c.comms.channels.push_back({ with, b.name, {} });
    Ask(w, c, body, "Hailing " + b.name + "...", { Greeting(b) }, 0.6);
    w.Emit(EventType::CommsOpened, "", with, -1, 0.0, 0.0, c.id);
}

void RequestMarketReport(World &w, Captain &c, int body)
{
    if (body < 0 || body >= (int)c.loaded.bodies.size()) return;
    Ask(w, c, body, "Requesting a market report.", MarketReport(c.loaded.bodies[body]), 0.8);
}

void RequestLocalInfo(World &w, Captain &c, int body)
{
    if (body < 0 || body >= (int)c.loaded.bodies.size()) return;
    const Body &b = c.loaded.bodies[body];
    Ask(w, c, body, "What can you tell me about " + b.name + "?", { LocalInfo(b) }, 1.0);   // (a different pause from a market
                                                                                          // report's: replies are told apart by when they land)
}

void HangUp(World &w, Captain &c, int body)
{
    TargetId with = TargetId::MakeBody(c.loaded.index, body);
    for (size_t i = 0; i < c.comms.channels.size(); i++)
        if (c.comms.channels[i].with == with) {
            w.Emit(EventType::CommsClosed, "Channel to " + c.comms.channels[i].withName + " closed", with, -1, 0.0, 0.0, c.id);
            c.comms.channels.erase(c.comms.channels.begin() + i);
            return;
        }
}

void UpdateComms(World &w, Captain &c)
{
    CommsState &cs = c.comms;
    if (cs.channels.empty()) return;
    // The system unloaded under us: out of range of everyone in it
    if (cs.channels[0].with.system != c.loaded.index) {
        std::string who = cs.channels.size() == 1 ? cs.channels[0].withName : TextFormat("%d channels", (int)cs.channels.size());
        w.Emit(EventType::CommsLost, "Lost signal with " + who, cs.channels[0].with, -1, 0.0, 0.0, c.id);
        cs.channels.clear();
        return;
    }
    // Replies that have just come in. Ones that took a while are worth a word: the comms window may be shut.
    for (CommsChannel &ch : cs.channels) {
        bool tell = false;
        for (CommsMsg &m : ch.log) {
            if (m.ours || m.told || m.at > w.clock) continue;
            m.told = true;
            if (m.at - m.sent > 5.0) tell = true;
        }
        if (tell) w.Emit(EventType::Notice, ch.withName + " has replied  -  F for comms", ch.with, -1, 0.0, 0.0, c.id);
    }
}
