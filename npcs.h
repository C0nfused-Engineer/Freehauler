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
//              hold: contraband is seized as at customs.
// Pirates      a clan's raiders, more of them when the clan is raiding hard.
//              They rob freighters and miners (the cargo is gone: the shortage
//              is real), and captains: they close in, name their price, and
//              take half the hold if they catch you. Pay up, or run.
//
// All of them fly by the same rules as a captain's ship (ship.h), with an autopilot at the helm.
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
const double TACHYON_RANGE_AU = 5.0;        // a basic scanner has an even chance or so of catching a starter ship's warp exit this far off...
const double TACHYON_DECAY_AU = 30.0;       // ...and the particles decay over about this distance

// One step for all traffic, around these captains (whoever owns the world calls it)
void StepTraffic(World &w, Captain *const *captains, int count, float dt);

// How an NPC ship looks at this captain; and for any ship on the scope
Stance ShipStance(const World &w, const Captain &c, const Npc &n);
Stance TrafficStance(const World &w, const Captain &c, const Traffic &t);
const Npc *FindNpc(const World &w, int id);
const char *NpcKindName(NpcKind k);

// Actions (sent as Commands)
bool HailShip(World &w, Captain &c, int id);        // a word with another ship: the reply comes at light speed
bool PayPirates(World &w, Captain &c);              // hand over what the pirate chasing you has demanded
bool CallTanker(World &w, Captain &c);              // nearly out of fuel: a tanker comes out to you, at a price
