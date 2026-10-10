// econ.h - the state of the economy (the rules are in economy.h).
//
// Every inhabited world and station in human space is a market. What a market
// is never changes and comes from the seed (EconBase: what it makes and uses,
// how big it is, who it trades with). What it holds does change: stock goes up
// with production and down with consumption, and flows along trade lanes from
// where a good is cheap to where it's dear. Price follows stock.
//
// On top of that sit the things the factions do to it (tariffs, embargoes,
// subsidies, price wars, raids, buyouts) and local shocks (strikes, failed
// harvests, booms). That part is world state: it's saved, and in multiplayer
// only the server has it. A captain knows a market's prices only as of the
// last time they docked there, hailed it or bought data about it (PriceRecord).
#pragma once
#include "galaxy.h"
#include "goods.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

const double MARKET_DAY      = 600.0;   // seconds of game clock: ten minutes
const double ECON_TICK       = 5.0;     // the economy advances in steps of this many seconds
const float  STOCK_DAYS      = 10.0f;   // a market aims to hold this many days of what it makes or needs
const double TRADE_DATA_COST = 150.0;   // credits for a station's snapshot of the markets it trades with
const double LANE_RANGE_LY   = 12.0;    // systems this close trade directly (each with its nearest few)

enum class Role : int8_t { None, Export, Import, Trade };   // what a good is to a market: made, needed, or passed through

// One market, as the seed made it
struct MarketDef {
    SysId       sys = NO_SYSTEM;
    int         body = -1;                 // index in its loaded system
    uint64_t    bodyId = 0;
    std::string name;
    Settlement  settlement = Settlement::None;
    int         ruler = NO_FACTION;        // the government whose space it's in
    int         owner = NO_FACTION;        // who ran it to begin with (buyouts change MarketState::owner)
    bool        station = false;
    DVec3       ly;                        // its system, in light years from home
    float       scale = 0;                 // tonnes a day it handles: a few dozen for an outpost, thousands for a core world
    float       out[GOOD_COUNT] = {};      // production capacity, tonnes a day
    float       use[GOOD_COUNT] = {};      // what its people and shipyards consume, tonnes a day
    float       target[GOOD_COUNT] = {};   // the stock it wants; 0 = doesn't deal in it
    Role        role[GOOD_COUNT] = {};     // what each good is to it
    bool        banned[GOOD_COUNT] = {};   // contraband under its ruler's law, and it keeps the law
    int         hub = -1;                  // the market of its system that trades with other systems (may be itself)
    float       traffic = 0;               // hubs: the scale of its whole system (what moves through it)
};

struct Lane { int a, b; float ly; };       // two hubs that trade

struct EconBase {
    std::vector<MarketDef> defs;
    std::vector<Lane>      lanes;
    std::vector<float>     stock0;         // settled starting stock: defs.size() * GOOD_COUNT
    std::unordered_map<uint64_t, int> byBody;
};

struct MarketState {
    float stock[GOOD_COUNT] = {};
    int   owner = NO_FACTION;
};

// Something local that has knocked a market off its stride for a while
struct Shock { int market; float supply, demand; double until; };

struct EconNews {
    double      at = 0.0;                  // game clock
    int         faction = NO_FACTION;      // whose doing it is (NO_FACTION: local news)
    SysId       where = NO_SYSTEM;
    std::string text;
    uint32_t    seq = 0;                   // (counts up: the server sends what a client hasn't had)
};

struct Economy {
    std::shared_ptr<const EconBase> base;  // null until built (clients never build it: the server runs the economy)
    std::vector<MarketState> state;
    double simulated = 0.0;                // game clock it has been run up to

    // What the factions are doing to it, each by faction id
    std::vector<float>  tariff;            // governments: the current rate
    std::vector<double> embargoUntil;      // n * n: [a * n + b] > clock: a refuses b's trade
    std::vector<float>  raid;              // clans: how hard they're hitting the lanes in their patch, 0 .. 1
    std::vector<double> raidUntil;         // ...and until when (then it drifts back to their usual)
    std::vector<float>  output;            // production at this faction's stations, as a multiple (subsidies)
    std::vector<double> outputUntil;
    std::vector<float>  pricing;           // prices at this faction's stations, as a multiple (price wars)
    std::vector<double> pricingUntil;
    std::vector<double> nextAct;           // when each next does something
    std::vector<int>    lastAct;           // (so it doesn't do the same thing twice running)
    std::vector<Shock>  shocks;
    uint64_t            dice = 1;

    bool Built() const { return base != nullptr; }
    int  Count() const { return base ? (int)base->defs.size() : 0; }
    int  MarketOf(uint64_t bodyId) const
    {
        if (!base) return -1;
        auto it = base->byBody.find(bodyId);
        return it == base->byBody.end() ? -1 : it->second;
    }
};

// A market as one captain last saw it
struct PriceRecord {
    double      at = -1.0;                 // game clock of the snapshot
    SysId       sys = NO_SYSTEM;
    std::string name;
    int         owner = NO_FACTION, ruler = NO_FACTION;
    float       tariff = 0.0f;
    float       price[GOOD_COUNT] = {};    // mid price; 0 = not traded there
    float       stock[GOOD_COUNT] = {};
    uint8_t     role[GOOD_COUNT] = {};     // a Role; +128 = contraband there
};
