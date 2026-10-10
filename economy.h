// economy.h - markets and trading.
//
// Every inhabited world and station has an economy (Body::econ) that decides
// its market:
//   exports   what it makes: sold cheap (60-75% of the base price)
//   imports   what it needs: bought dear (130-160%)
//   trades    what passes through: around base price
//   ores      every market buys raw ore (refineries and industry pay best)
// Each market nudges its prices a little (a hash of its name, so they're
// stable), and you buy a little above and sell a little below its mid price.
// Profit is in carrying exports to where they're imports.
//
// Prices don't move with supply and demand yet; that's the next step.
#pragma once
#include "game.h"
#include <vector>

enum class Role : int8_t { None, Export, Import, Trade, Ore };

struct Market {
    Econ   econ = Econ::None;
    Role   role[GOOD_COUNT] = {};
    double price[GOOD_COUNT] = {};       // mid price, credits per tonne
    double fuelPrice = 50.0;

    bool   Sells(int g) const { return role[g] == Role::Export || role[g] == Role::Trade; }   // you can buy it here
    bool   Buys(int g) const  { return role[g] != Role::None; }                               // you can sell it here
    double BuyPrice(int g) const  { return price[g] * 1.04; }   // what you pay
    double SellPrice(int g) const { return price[g] * 0.96; }   // what you get
    std::vector<int> Listed() const;     // the goods traded here: exports, imports, trades, then ores
};

Market      MakeMarket(const Body &b);
const Body *TradePartner(const Captain &c);     // inhabited world or station in interaction range, or null

// The market's character in a few words: "Exports Food, Spirits, Textiles"
std::string ExportsLine(const Market &m, int max = 3);
std::string ImportsLine(const Market &m, int max = 3);

// Trading actions (the market window sends these as Commands; see commands.h).
// 'qty' is clamped to what's possible (1e30 = as much as you can); each returns
// false (with a message event) if nothing happened.
bool SellGoods(World &w, Captain &c, int good, double qty);
bool BuyGoods(World &w, Captain &c, int good, double qty);
bool BuyFuel(World &w, Captain &c, double qty);

inline bool SellGoods(GameState &g, int good, double qty) { return SellGoods(g, g, good, qty); }
inline bool BuyGoods(GameState &g, int good, double qty)  { return BuyGoods(g, g, good, qty); }
inline bool BuyFuel(GameState &g, double qty)             { return BuyFuel(g, g, qty); }
