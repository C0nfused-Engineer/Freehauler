// npcs.h - space traffic: freighters, miners, patrols and pirates.
//
// Freighters   carry what the economy trades (see npc.h): market to hub within
//              the system, and hub to hub along the lanes, jumping at the edge.
//              What they carry lands in the destination's stock when they dock,
//              and not before.
// Miners       work the belt (or skim the giant) out of the places that produce
//              raw materials, and bring the day's output home in their holds.
// Patrols      a government's ships, more of them where policing is heavy. They
//              run pirates off, and now and then close on a captain to scan the
//              hold: contraband is seized as at customs. Some are pickets: warships
//              stationed at a sensor outpost, which keep close to it.
// Pirates      a clan's raiders, more of them when the clan is raiding hard.
//              They rob freighters and miners (the cargo is gone: the shortage
//              is real), and captains: they close in, name their price, and
//              take half the hold if they catch you. Pay up, or run.
//
// All of them fly by the same rules as a captain's ship (ship.h), with an autopilot at the helm,
// and know only what their sensors tell them (sensors.h): patrols run their radar and have the
// system's traffic picture besides; raiders keep theirs off and hunt by heat and by eye. Either
// can lose what it's chasing, and then it searches where it last saw it, for a while.
// No shooting yet: that's the combat step.
//
// Ships that leave a captain's sight are settled at once (their cargo lands),
// so nothing is lost by looking away, and nothing here needs saving.
#pragma once
#include "game.h"

const double PIRATE_SHARE   = 0.5;      // of the hold, if they catch you
const double PIRATE_DEMAND  = 0.25;     // ...or this much handed over, and they go
const double PIRATE_PEACE   = 300.0;    // seconds they leave you alone afterwards
const double SCAN_PEACE     = 600.0;    // seconds between patrol scans of the same captain
const double CARGO_SCAN_RANGE = 1000.0 * KM_U;   // close enough to read another ship's manifest
const double TANKER_CALLOUT = 400.0, TANKER_LOAD = 20.0;   // credits to come out; the most it brings (at three times the going rate)
const int    OUTPOST_PICKETS = 2;       // warships stationed at each sensor outpost
const double LOSE_AFTER     = 60.0;     // seconds a ship searches for something it has lost before giving it up

// One step for all traffic, around these captains (whoever owns the world calls it)
void StepTraffic(World &w, Captain *const *captains, int count, float dt);

// How an NPC ship looks at this captain; and for any ship on the scope
Stance ShipStance(const World &w, const Captain &c, const Npc &n);
Stance TrafficStance(const World &w, const Captain &c, const Traffic &t);
const Npc *FindNpc(const World &w, int id);
// Does that ship's crew know where this captain is, right now? (Their own sensors; for a patrol, its system's traffic picture too.)
bool NpcHolds(const World &w, const Npc &n, const Captain &c);
const char *NpcKindName(NpcKind k);

// Actions (sent as Commands)
bool HailShip(World &w, Captain &c, int id);        // a word with another ship: the reply comes at light speed
bool PayPirates(World &w, Captain &c);              // hand over what the pirate chasing you has demanded
bool CallTanker(World &w, Captain &c);              // nearly out of fuel: a tanker comes out to you, at a price
