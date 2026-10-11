// commands.cpp - applying player actions
#include "commands.h"
#include "comms.h"
#include "contracts.h"
#include "economy.h"
#include "npcs.h"
#include "shipyard.h"

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
    case Command::BuyData:      return BuyTradeData(w, c);
    case Command::Contracts:    RequestContracts(w, c, cmd.arg); return true;
    case Command::FactionBoard: return RequestFactionBoard(w, c, cmd.arg);
    case Command::Accept:       return AcceptContract(w, c, (uint32_t)cmd.arg);
    case Command::Advance:      return AdvanceContract(w, c, (uint32_t)cmd.arg);
    case Command::Abandon:      return AbandonContract(w, c, (uint32_t)cmd.arg);
    case Command::JoinGuild:    return JoinGuild(w, c, cmd.arg);
    case Command::BuyLicence:   return BuyLicence(w, c, cmd.arg, (int)cmd.qty);
    case Command::HailShip:     return HailShip(w, c, cmd.arg);
    case Command::PayPirates:   return PayPirates(w, c);
    case Command::CallTanker:   return CallTanker(w, c);
    case Command::FitPart:      return FitPart(w, c, UnpackPart(cmd.arg));
    case Command::RemovePart:   return RemovePart(w, c, cmd.arg);
    case Command::BuyHull:      return BuyHull(w, c, cmd.arg);
    case Command::StretchHull:  return StretchHull(w, c, cmd.arg != 0);
    default:                    return false;
    }
}
