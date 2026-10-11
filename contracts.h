// contracts.h - paid work: who offers it, taking it, doing it.
//
// Boards are made from what's really going on. A market short of something
// posts a supply order; one with a glut wants it hauled to where it's dear; a
// strike or a failed harvest brings relief runs; a faction's agenda brings jobs
// that push it along (arming patrols against a clan, or the clan itself); the
// guilds keep better work for their members at stations where they have a
// chapter. Anything left over is filled in with ordinary jobs. Goods delivered
// really arrive: stocks, and so prices, move.
//
// You ask a station for its board over the comms (the reply takes light-time,
// and what you have is as of then), or look at a faction's list in the factions
// window. Accepting from a station's board needs you in its system (a faction's
// list can be taken up from anywhere); cargo is collected and delivered in
// docking range. Up to MAX_JOBS at once.
//
// Stakes: a deadline (late pays half, very late fails); a deposit for the
// issuer's cargo (fail and the cargo is yours, the deposit theirs); standing
// with the issuer, up for work done and down for work dropped; and the better
// work wants standing, guild membership or a licence first.
//
// Customs: dock at a lawful port carrying what its government bans and they may
// search the hold. A restricted goods permit from that government makes it legal.
#pragma once
#include "game.h"

const int    MAX_JOBS        = 5;
const double BOARD_REFRESH   = MARKET_DAY;        // a board is drawn up afresh this often
const double GUILD_FEE       = 2500.0;
const double CARRIER_COST    = 4000.0;            // bonded carrier's licence...
const float  CARRIER_STANDING = 20.0f;            // ...and the standing it takes
const double PERMIT_COST     = 12000.0;           // restricted goods permit
const float  PERMIT_STANDING = 50.0f;
const double BONDED_OVER     = 20000.0;           // haulage with a deposit above this needs a bonded carrier

// The world's side (whoever owns the world calls this as the clock advances)
void UpdateContracts(World &w);
// A captain's side, each authoritative step: surveys in progress, deadlines, customs
void StepContracts(World &w, Captain &c);

// Asking for boards. A station's comes back over the comms; a faction's list is public.
//   (RequestContracts is in comms.h)
void RecordBoard(World &w, Captain &c, const Body &b);         // (the reply has arrived)
bool RequestFactionBoard(World &w, Captain &c, int faction);

// Actions (sent as Commands). Each says why not, if not.
bool AcceptContract(World &w, Captain &c, uint32_t id);
bool AdvanceContract(World &w, Captain &c, uint32_t id);       // collect the cargo, or deliver, if this is the place
bool AbandonContract(World &w, Captain &c, uint32_t id);
bool JoinGuild(World &w, Captain &c, int faction);
bool BuyLicence(World &w, Captain &c, int faction, int paper); // PAPER_CARRIER or PAPER_PERMIT

// A search of the hold under a government's law, by 'who': what it bans is seized, with a fine and a black mark.
// False if there was nothing to find (or the captain holds its permit).
bool SearchHold(World &w, Captain &c, int law, const std::string &who);

// For the screens
const char *ContractKindName(ContractKind k);
std::string ContractTitle(const Contract &k);                              // "Haul 40 t Steel to Halagon b"
std::string ContractTerms(const FactionSet &fs, const Contract &k);        // what it wants of you first: "Traders' Guild members; Resident"
// Why this captain can't take it ("" if they can)
std::string ContractBar(const World &w, const Captain &c, const Contract &k);
// What a job needs next, and whether that can be done right here
std::string JobStatus(const World &w, const Captain &c, const Contract &k, bool *here = nullptr);
// Where a job wants the ship next (a system to set course for)
SysId       JobNextSystem(const Contract &k);
// Does a guild keep a chapter at this station? (Its members find its work there.)
bool        HasChapter(const World &w, SysId sys, const Body &b, int guildFaction);

// Saving: the world's boards, and a captain's jobs, papers and records of boards
void WriteContracts(const World &w, std::string &out);
bool ReadContractLine(World &w, const char *key, const char *rest);
void WriteCaptainContracts(const Captain &c, std::string &out);
bool ReadCaptainContractLine(Captain &c, const char *key, const char *rest);
