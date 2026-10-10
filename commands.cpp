// commands.cpp - applying player actions
#include "commands.h"
#include "comms.h"
#include "economy.h"

bool ApplyCommand(World &w, Captain &c, const Command &cmd)
{
    switch (cmd.type) {
    case Command::SellGoods:    return SellGoods(w, c, cmd.arg, cmd.qty);
    case Command::BuyGoods:     return BuyGoods(w, c, cmd.arg, cmd.qty);
    case Command::BuyFuel:      return BuyFuel(w, c, cmd.qty);
    case Command::Hail:         Hail(w, c, cmd.arg); return true;
    case Command::MarketReport: RequestMarketReport(w, c, cmd.arg); return true;
    case Command::LocalInfo:    RequestLocalInfo(w, c, cmd.arg); return true;
    case Command::HangUp:       HangUp(w, c, cmd.arg); return true;
    default:                    return false;
    }
}
