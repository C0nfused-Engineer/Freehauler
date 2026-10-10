// commands.h - player actions as data.
// The market and comms windows don't call SellGoods / Hail directly: they make
// a Command. In single player it's applied on the spot; in multiplayer it goes
// to the server, which applies it to your captain there (the result comes
// back as events and in the next snapshot).
#pragma once
#include "game.h"

struct Command {
    enum Type : uint8_t { SellGoods, BuyFuel, Hail, MarketReport, LocalInfo, HangUp, BuyGoods, BuyData,
                          Contracts, FactionBoard, Accept, Advance, Abandon, JoinGuild, BuyLicence, Count };
    Type     type = SellGoods;
    int32_t  arg  = 0;        // SellGoods / BuyGoods: which good (goods.h); the comms ones (and Contracts): body index in the loaded
                              // system; Accept / Advance / Abandon: the contract's id; FactionBoard / JoinGuild / BuyLicence: the faction
    double   qty  = 0.0;      // tonnes (1e30 = all you can); BuyLicence: which paper (contract.h)
    uint32_t step = 0;        // multiplayer: the input step it was issued after (the server applies it then)
};

// Applies a command to a captain; false if it was invalid or did nothing
bool ApplyCommand(World &w, Captain &c, const Command &cmd);
