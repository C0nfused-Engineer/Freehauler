// commands.h - player actions as data.
// The market and comms windows don't call SellGoods / Hail directly: they make
// a Command. In single player it's applied on the spot; in multiplayer it goes
// to the server, which applies it to your captain there (the result comes
// back as events and in the next snapshot).
#pragma once
#include "game.h"

struct Command {
    enum Type : uint8_t { SellGoods, BuyFuel, Hail, MarketReport, LocalInfo, HangUp, BuyGoods, Count };
    Type     type = SellGoods;
    int32_t  arg  = 0;        // SellGoods / BuyGoods: which good (goods.h); the comms ones: body index in the loaded system
    double   qty  = 0.0;      // tonnes (1e30 = all you can)
    uint32_t step = 0;        // multiplayer: the input step it was issued after (the server applies it then)
};

// Applies a command to a captain; false if it was invalid or did nothing
bool ApplyCommand(World &w, Captain &c, const Command &cmd);
