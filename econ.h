// econ.h - the state of the economy (the rules are in economy.h).
//
// Every inhabited world, with the stations around it, is one economy and one
// market; so is a station on its own. Where it is and what could be done there
// never changes and comes from the seed (EconBase). What it has does change: its
// industries (industry.h) grow, its stock goes up with what they make and down
// with what its people use, and flows along trade lanes from where a good is
// cheap to where it's dear. Price follows stock.
//
// On top of that sit the things the factions do to it (tariffs, embargoes,
// subsidies, price wars, raids, buyouts) and local shocks (strikes, failed
// harvests, booms). That part is world state: it's saved, and in multiplayer
// only the server has it. A captain knows a market's prices only as of the
// last time they docked there, hailed it or bought data about it (PriceRecord).
#pragma once
#include "galaxy.h"
#include "fleet.h"
#include "goods.h"
#include "industry.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

const double MARKET_DAY      = 600.0;   // seconds of game clock: ten minutes
const double ECON_TICK       = 5.0;     // the economy advances in steps of this many seconds
const float  STOCK_DAYS      = 10.0f;   // a market aims to hold this many days of what it makes or needs
const double TRADE_DATA_COST = 150.0;   // credits for a station's snapshot of the markets it trades with
const double LANE_RANGE_LY   = 12.0;    // systems this close trade directly (each with its nearest few)

// People. A world's living standard (0 .. 1) is these shares of how well it's fed, what food it has in store, how
// many of its people have a roof, how many have work, its water, its medicines, its comforts, and how safe it is.
const double LIVING_FOOD = 0.30, LIVING_RESERVE = 0.08, LIVING_HOUSING = 0.24, LIVING_WORK = 0.12, LIVING_WATER = 0.07,
             LIVING_MEDICINE = 0.05, LIVING_COMFORT = 0.06, LIVING_SAFETY = 0.08;
// What people buy beyond the necessities follows how well they live: this much more (or less) of it for each point
// their living standard is above (or below) WEALTH_AT, between the limits
const double WEALTH_AT = 0.90, WEALTH_SLOPE = 2.0, WEALTH_MIN = 0.4, WEALTH_MAX = 1.3;
const double FED_AT        = 0.25;      // a world counts as fed (watered, doctored) with this share of its usual stock in hand: what trade leaves it
const double GROW_ABOVE    = 0.80;      // above this a world's people increase (if there's a roof for more)...
const double GROWTH_MAX    = 0.03;      // ...by up to this share a market day
const double LEAVE_BELOW   = 0.75;      // below this people start to leave for somewhere better...
const double LEAVE_MAX     = 0.05;      // ...up to this share of them a day, at the worst
const double CROWD_LEAVE   = 0.20;      // and this share a day of those with no roof go, whatever else
const double STARVE_BELOW  = 0.5, STARVE_MAX = 0.03;      // food under this share of what counts as fed: up to this share a day die
// How far a place can grow from the size it starts at, and the least and most that can come to: a station; a world
// without a sky to live under; and one with
const double ROOM_STATION = 25.0,  ROOM_STATION_MIN = 3000.0, ROOM_STATION_MAX = 2.0e6;
const double ROOM_GROUND  = 200.0, ROOM_GROUND_MIN  = 5.0e4,  ROOM_GROUND_MAX  = 3.0e8;
const double ROOM_SKY     = 50.0,  ROOM_SKY_MIN     = 1.0e7,  ROOM_SKY_MAX     = 4.0e10;
const double SKELETON_CREW = 30.0;      // a settlement never quite empties: this many caretakers stay
const double PEOPLE_PER_TONNE = 5.0;    // how many migrants a day a lane carries, for each tonne of trade it could
enum BorderPolicy : uint8_t { BORDER_OPEN, BORDER_SELECTIVE, BORDER_CLOSED };     // a government's, towards migrants from outside its space
const float  SELECTIVE_WORK  = 0.60f;   // a selective border lets people in only where at least this share of labour is in work (and there are roofs)...
const double SELECTIVE_SHARE = 0.3;     // ...and then this share of those who'd come

enum class Role : int8_t { None, Export, Import, Trade };   // what a good is to a market: made, needed, or passed through

// The ships of one owner based at a hub, by role (fleet.h)
struct FleetAt { int16_t owner = NO_FACTION; float ships[ROLE_COUNT] = {}; };

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
    double      pop = 0.0;                 // its people, stations and all, to begin with
    double      room = 0.0;                // the most people the place can ever hold: a station fills up, and a world under domes long before one under a sky
    SiteInfo    site;                      // the place, as industry sees it
    float       use0[GOOD_COUNT] = {};     // what its people, ports and shipyards consume, tonnes a day
    std::vector<IndustryDef> ind;          // its industries to begin with, and the ones that could be started here (cap 0)
    bool        banned[GOOD_COUNT] = {};   // contraband under its ruler's law, and it keeps the law
    int         hub = -1;                  // the market of its system that trades with other systems (may be itself)
    float       traffic = 0;               // hubs: the scale of its whole system (what moves through it)
    float       haulShare = 0;             // the share of its hub's local haulers that work its run
    float       fleetFuel[3] = {};         // the fuel ships can be expected to draw here a day: reactor fuel, deuterium, helium-3
};

// Two hubs that trade: how much dealing there is between them (tonnes a day of a good, for a price gap of two thirds),
// and the share of each one's ships that work the lane
struct Lane { int a, b; float ly; float vol = 0; float shareA = 0, shareB = 0; };

struct EconBase {
    std::vector<MarketDef> defs;
    std::vector<Lane>      lanes;
    std::vector<float>     stock0;         // settled starting stock: defs.size() * GOOD_COUNT
    std::vector<int>       capital;        // by faction: the hub market at its seat (-1: it has none, and its money needs no carrying)
    std::vector<int>       toCapital;      // by faction and market: the next hub on the way there (-1: none, or there already)
    std::vector<ShipDesign> designs;       // every faction's ship for every role, and independent owners' first (FleetDesign)
    std::vector<std::vector<FleetAt>> fleets0;   // the fleets to begin with, by hub
    float                  yardYield = 0;  // tonnes of ship a unit of shipyard turns out a day
    float                  perCap[GOOD_COUNT] = {};  // tonnes of each good a unit of capacity given over to it turns out a day
    std::unordered_map<uint64_t, int> byBody;
};

// One industry of one economy, as it stands
enum { SHORT_LABOUR = 1, SHORT_CELLS = 2, SHORT_MATERIALS = 4, SHORT_DEMAND = 8 };
struct IndustryState {
    float   cap = 0.0f;                    // capacity
    int16_t owner = NO_FACTION;
    float   run = 0.0f;                    // the share of it that ran, last time round
    uint8_t shortOf = 0;                   // ...and what held it back (SHORT_ bits)
    float   output = 0.0f, profit = 0.0f;  // tonnes and credits a day, at that rate
    float   building = 0.0f;               // capacity being added...
    float   buildLeft = 0.0f;              // ...ready in this many market days
    double  earned = 0.0;                  // credits of profit each unit of its capacity has made, in all (stakes are paid from this)
};

struct MarketState {
    float stock[GOOD_COUNT] = {};
    int   owner = NO_FACTION;
    std::vector<IndustryState> ind;        // (in step with MarketDef::ind)
    double pop = 0.0;
    float labour = 0.0f, employed = 0.0f;  // labour a day, and how much of it found work
    float housing = 0.0f;                  // people its cities can house, as they're running
    float services = 1.0f;                 // how fully its services are running, 0 .. 1
    float servicesLabour = 0.0f;           // ...and the labour they have (hands an industry worth building could take)
    float cellsMade = 0.0f, cellsUsed = 0.0f;   // plasma cells a day
    float living = 1.0f;                   // its living standard, 0 .. 1 (LivingStandard)
    float trend = 0.0f;                    // how its people are changing: the share a market day
    float refugees = 0.0f;                 // people lately arrived from somewhere worse, not yet settled in
    float leaving = 0.0f, stuck = 0.0f;    // people a day who are going; and who would, with nowhere that will have them
    int   bestTo = -1, barredTo = -1;      // where most of them go; and the best place that's turning them away at its border
    double popDerived = 0.0;               // (the population 'use' and the targets were last worked out for)
    // What all that adds up to (Derive keeps these so)
    float out[GOOD_COUNT] = {};            // production capacity, tonnes a day
    float use[GOOD_COUNT] = {};            // what its people, ports and shipyards consume, tonnes a day
    float target[GOOD_COUNT] = {};         // the stock it wants; 0 = doesn't deal in it
    Role  role[GOOD_COUNT] = {};           // what each good is to it
};

// Goods on the dock at 'from', waiting for a ship to 'to' (where ships carry the trade: npc.h)
// ('since': when the last ship left for there, or when these goods started waiting)
struct Shipment { int from = -1, to = -1; float cargo[GOOD_COUNT] = {}; float total = 0.0f; double since = 0.0; };

// Something local that has knocked a market off its stride for a while
struct Shock { int market; float supply, demand; double until; };

struct EconNews {
    double      at = 0.0;                  // game clock
    int         faction = NO_FACTION;      // whose doing it is (NO_FACTION: local news)
    SysId       where = NO_SYSTEM;
    std::string text;
    uint32_t    seq = 0;                   // (counts up: the server sends what a client hasn't had)
};

// ---------------------------------------------------------------------------
// Money. A faction earns where its business is: a government its tariffs, a tax on the profit of works in its space
// that aren't its own, and a tax on its people; a company the profit of its works, its ships' freight, and what its
// yards are paid for new ships; a clan what it takes. What's earned waits in a COFFER at the market where it was
// earned until the treasury ship sails, every few days; then it is carried a hop at a time along the lanes to the
// faction's capital, joined on the way by whatever else of its owner's is going the same way. Only what has got
// there is in its TREASURY, and only that can be spent. Most of it goes on keeping the faction going (its overhead:
// what it's used to earning sets what that costs); the rest on additions to its industries and on new ships. With
// nothing in the treasury it builds nothing, and a government that can't meet its bills taxes harder.
// ---------------------------------------------------------------------------
enum Income : uint8_t { INC_TARIFF, INC_TAX, INC_HEADS, INC_PROFIT, INC_FREIGHT, INC_YARDS, INC_LOOT, INC_COUNT };
const char *IncomeName(int kind);

// A coffer on its way where ships are flown (npcs.h puts it aboard one): from a market toward its owner's capital
struct Specie { int from = -1, to = -1; int faction = NO_FACTION; double credits = 0.0; };

struct FactionMoney {
    double treasury = 0.0;                 // credits at its capital: what it can spend
    float  squeeze = 1.0f;                 // governments: how hard it's taxing, against what its politics would have it do (1)
    float  profitTax = 0.0f, headTax = 0.0f;   // ...and what its politics would have it do: the share of others' profits, and credits a head a day
    float  income[INC_COUNT] = {};         // credits a day, lately, by where it comes from (as earned: not yet home)
    float  overhead = 0.0f;                // the share of its usual income that goes on keeping itself going: officials and patrols, wages, dividends
    float  usual = 0.0f;                   // ...that usual income, credits a day (a long memory of it: commitments are slow to change)
    float  upkeep = 0.0f;                  // ...and what it has been paying toward that, credits a day, lately
    float  spent = 0.0f;                   // credits a day, lately, on new works and ships
    float  denied = 0.0f;                  // ...and what it would have spent and hadn't got
    double incomeNow[INC_COUNT] = {}, spentNow = 0.0, deniedNow = 0.0;     // (since the books were last made up)
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
    double              developed = 0.0;   // the clock when owners last looked at what to build
    std::vector<uint8_t> border;           // governments: a BorderPolicy
    // Shipping (fleet.h)
    std::vector<std::vector<FleetAt>> fleets;    // by market: the ships based there (hubs only)
    std::vector<float>  busy;              // by hub and role: how much there has been to carry lately, against what its ships could
    std::vector<float>  slips;             // by market: tonnes of ship its yards have finished, not yet handed over
    std::vector<float>  clanShips;         // by faction: ships the clans have taken
    // Money
    std::vector<FactionMoney> money;       // by faction
    std::vector<double> coffer;            // by market and faction: credits earned there, waiting to be carried home
    std::vector<double> moving;            // by market and faction: money on its way home that has got this far (it goes on at the next hop)
    int                 sailings = 0;      // (how many times the money on the move has been sent on a hop: a market's coffer sails every few of these)
    std::vector<Specie> specie;            // money waiting for a ship, where ships are flown
    double              collected = 0.0;   // the clock when the coffers were last sent on
    double              ratesLooked = 0.0; // ...and when governments last looked at their taxes
    bool                paying = false;    // (the economy is running for real: not being settled while a galaxy is built)
    double Moving(int market, int faction) const { size_t F = money.size(); return F && market >= 0 && faction >= 0 && (size_t)faction < F && ((size_t)market + 1) * F <= moving.size() ? moving[(size_t)market * F + faction] : 0.0; }
    double Coffer(int market, int faction) const { size_t F = money.size(); return F && market >= 0 && faction >= 0 && (size_t)faction < F && ((size_t)market + 1) * F <= coffer.size() ? coffer[(size_t)market * F + faction] : 0.0; }
    bool                fleetsRead = false;// (loading: the first fleet line clears what a new galaxy starts with)
    // (building a galaxy: with these set, trade is run with fuel for the asking and what moves is added up, to size the fleets by)
    std::vector<double> measured;          // tonnes moved: for each lane dry then tanked, then for each market its local run
    double              measuredDays = 0.0;
    double              bordersLooked = 0.0;

    // Where captains are, goods go by ship (npcs.h sets 'zone' and moves what's 'waiting')
    std::vector<uint8_t>  zone;            // by market: 0 trade is numbers; 1 its lane trade is tracked ships; 2 everything is flown
    std::vector<Shipment> waiting;         // on the docks
    std::vector<float>    yield;           // by market and good: dug or skimmed, not yet brought in by the miners
    std::vector<float>    afloat;          // by market and good: on its way there (on a dock, in a hold, in 'yield')
    void Ship(int from, int to, int good, float tonnes, double clock)
    {
        afloat[(size_t)to * GOOD_COUNT + good] += tonnes;
        for (Shipment &s : waiting) if (s.from == from && s.to == to) { s.cargo[good] += tonnes; s.total += tonnes; if (clock < s.since) s.since = clock; return; }
        Shipment s; s.from = from; s.to = to; s.cargo[good] = tonnes; s.total = tonnes; s.since = clock;
        waiting.push_back(s);
    }
    // Cargo reaches its market (or, 'lost', never will)
    void Land(int to, const float *cargo, bool lost = false)
    {
        for (int g = 0; g < GOOD_COUNT; g++) {
            if (cargo[g] <= 0.0f) continue;
            float &on = afloat[(size_t)to * GOOD_COUNT + g];
            on = on > cargo[g] ? on - cargo[g] : 0.0f;
            if (!lost) state[to].stock[g] += cargo[g];
        }
    }

    bool Built() const { return base != nullptr; }
    int  Count() const { return base ? (int)base->defs.size() : 0; }
    int  MarketOf(uint64_t bodyId) const
    {
        if (!base) return -1;
        auto it = base->byBody.find(bodyId);
        return it == base->byBody.end() ? -1 : it->second;
    }
};

// A world's economy as one captain last saw it: its people, and each industry (the economy page shows this)
struct IndustryRow {
    uint8_t type = 0;
    int16_t owner = NO_FACTION;
    float   site = 0, cap = 0, run = 0, output = 0, profit = 0, building = 0, buildLeft = 0;
    uint8_t shortOf = 0;
    float   stake = 0;                     // the capacity this captain has paid for...
    float   pending = 0;                   // ...and what it has earned them since they last collected
    float   cost = 0;                      // credits to fund the next addition to it (0: it can't be funded just now)
    float   step = 0;                      // ...which would add this much
};
struct WorldRecord {
    double at = -1.0;
    std::string name;
    double pop = 0.0;
    float  labour = 0, employed = 0, housing = 0, cellsMade = 0, cellsUsed = 0;
    float  living = 0, trend = 0, refugees = 0, leaving = 0, stuck = 0;
    uint8_t border = BORDER_OPEN;         // its government's policy
    std::string goingTo, barredAt;         // where its emigrants mostly go; and who is turning them away
    float  fleet[ROLE_COUNT] = {}, busy[ROLE_COUNT] = {};    // a hub's shipping: ships based there by role, and how busy they are
    std::vector<IndustryRow> rows;
};
// A share a captain has bought in an industry
struct Stake { uint64_t body = 0; uint8_t type = 0; float cap = 0.0f; double mark = 0.0; };

// A market as one captain last saw it
struct PriceRecord {
    double      at = -1.0;                 // game clock of the snapshot
    SysId       sys = NO_SYSTEM;
    std::string name;
    int         owner = NO_FACTION, ruler = NO_FACTION;
    float       tariff = 0.0f;
    float       price[GOOD_COUNT] = {};    // mid price; 0 = not traded there
    float       stock[GOOD_COUNT] = {};
    float       target[GOOD_COUNT] = {};   // the stock it likes to hold: below that it pays over the going rate, above it under
    uint8_t     role[GOOD_COUNT] = {};     // a Role; +128 = contraband there
};
