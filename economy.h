// economy.h - markets, trading, and the economy's own life.
//
// Each market day (ten minutes), every market:
//   uses      what its people and yards consume, if it has it
//   makes     what it produces, if it has the inputs (goods.h recipes) and isn't overstocked
//   trades    with its system's hub, and hubs with the hubs of nearby systems: goods flow
//             from where they're cheap to where they're dear, in proportion to the gap
// Price follows stock: at its target stock a good costs its base price, half as much stock
// is about half again dearer, and so on, up to six times the base price where there's none left.
//
// A captain's hold is a small thing to a core world and a large one to an outpost:
// buying raises the price and selling lowers it as the deal goes through.
//
// Law: the ruling government's tariff comes off what you're paid; goods it bans
// can't be traded at lawful stations in its space (independents and clans don't ask).
// A station's owner gives friends a better price, and doing business with it makes
// you one, slowly.
//
// The factions act on all this from their agendas (factions.h), and now and then
// something local goes wrong or right. All of it makes the news.
#pragma once
#include "game.h"
#include <vector>

// A market as it's shown and dealt with: live, or as a captain last saw it
struct Market {
    Role   role[GOOD_COUNT] = {};
    double price[GOOD_COUNT] = {};       // mid price, credits per tonne
    double stock[GOOD_COUNT] = {};       // tonnes on hand
    double target[GOOD_COUNT] = {};      // tonnes it likes to have on hand (0: not known)
    bool   banned[GOOD_COUNT] = {};      // contraband here
    double fuelPrice = 0.0;              // what a tonne of fuel costs you here (0: none to be had)
    double tariff = 0.0;                 // share taken from what you're paid
    double edge = 0.0;                   // what your standing with the owner is worth: +0.03 = 3% in your favour
    int    owner = NO_FACTION, ruler = NO_FACTION;

    bool   Sells(int g) const { return role[g] != Role::None && !banned[g] && stock[g] >= 0.5; }   // you can buy it here
    bool   Buys(int g) const  { return role[g] != Role::None && !banned[g]; }                      // you can sell it here
    double BuyPrice(int g) const  { return price[g] * 1.04 * (1.0 - edge); }                       // what you pay
    double SellPrice(int g) const { return price[g] * 0.96 * (1.0 + edge) * (1.0 - tariff); }      // what you get
    double Wants(int g) const { return Buys(g) && target[g] > stock[g] ? target[g] - stock[g] : 0.0; }   // tonnes short of what it likes to hold
    double SellValue(int g, double qty) const;   // what selling 'qty' tonnes here would fetch, the price falling as it goes
    std::vector<int> Listed() const;     // the goods traded here: exports, imports, then the rest
};

const Body *TradePartner(const Captain &c);     // inhabited world or station in interaction range, or null

// The market at a body as things stand right now. Only where the economy runs (single
// player, the server); empty elsewhere.
Market LiveMarket(const World &w, const Captain &c, const Body &b);
// ...and as this captain last saw it. False if they never have. 'at': when that was.
bool   KnownMarket(const Captain &c, const Body &b, Market &out, double *at = nullptr);
// Notes a market's prices in the captain's records (docking and market reports do this)
void   RecordPrices(World &w, Captain &c, const Body &b);

double StandingEdge(const Captain &c, int owner);      // 'edge' for a captain at this owner's markets

// The market's character in a few words: "Electronics, Robotics, Medicines"
std::string ExportsLine(const Market &m, int max = 3);
std::string ImportsLine(const Market &m, int max = 3);

// Trading actions (the market window sends these as Commands; see commands.h).
// 'qty' is clamped to what's possible (1e30 = as much as you can); each returns
// false (with a message event) if nothing happened.
bool SellGoods(World &w, Captain &c, int good, double qty);
bool BuyGoods(World &w, Captain &c, int good, double qty);
bool BuyFuel(World &w, Captain &c, double qty);
bool BuyTradeData(World &w, Captain &c);        // the prices at every market this one's system trades with

inline bool SellGoods(GameState &g, int good, double qty) { return SellGoods(g, g, good, qty); }
inline bool BuyGoods(GameState &g, int good, double qty)  { return BuyGoods(g, g, good, qty); }
inline bool BuyFuel(GameState &g, double qty)             { return BuyFuel(g, g, qty); }

// The economy itself (UpdateEconomy is declared in game.h: the world's step calls it)
void   EnsureEconomy(World &w);                 // builds it on first use
double MarketPrice(const World &w, int market, int good);     // mid price right now; 0 if it isn't traded there
double RaidLevel(const World &w, DVec3 ly);     // how hard the clans are hitting shipping at a place (light years from home): 0 .. 1
void   StepEconomy(World &w, double days);      // (tests: run it forward without the clock)
void   PostNews(World &w, int faction, SysId where, const std::string &text);   // a headline (NO_FACTION: local news)

// Saving: the economy as lines of text, and reading them back (after EnsureEconomy)
void   WriteEconomy(const World &w, std::string &out);
bool   ReadEconomyLine(World &w, const char *key, const char *rest);
// ...and a captain's price records
void   WritePrices(const Captain &c, std::string &out);
bool   ReadPriceLine(Captain &c, const char *key, const char *rest);
