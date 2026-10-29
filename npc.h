// npc.h - the other ships in space, as data (the rules are in npcs.h).
//
// Where a captain is, the economy's goods don't move as numbers: they move in
// ships. A market's trade with its system's hub goes by freighter, ore comes in
// from the belt in miners' holds, and trade along the lanes arrives and leaves
// in freighters that jump at the edge of the system. Those ships fly (Npc).
// One system further out nothing is flown, but lane cargoes are still tracked
// as ships under way with a time of arrival (Transit), so a freighter that
// drops out of warp beside you really did set out a while ago. Beyond that the
// economy is numbers, as before.
#pragma once
#include "galaxy.h"
#include "goods.h"
#include "ship.h"
#include <string>

const int NPC_ID_BASE = 1000000;           // ship ids from here up are NPCs (below: captains)

enum class NpcKind : uint8_t { Freighter, Patrol, Pirate, Miner, Tanker, Count };

enum class NpcDo : uint8_t {
    Travel,         // flying to 'goal'
    Dock,           // alongside, until 'until'
    Work,           // miners: at the rock face
    Lurk,           // pirates: waiting for something worth taking
    Hunt,           // closing on 'prey' (a ship to rob, a pirate to see off)
    Inspect,        // patrols: closing on a captain to scan the hold
    Flee,           // pirates: a patrol is too close
    Leave,          // heading out of the system (to jump, or to slink off)
    Deliver,        // tankers: closing on the captain who called
};

// How a ship looks at a captain (it picks the colour it's drawn in)
enum Stance : uint8_t { STANCE_ALLIED, STANCE_NEUTRAL, STANCE_ALERTED, STANCE_HOSTILE };

struct Npc {
    int         id = 0;
    NpcKind     kind = NpcKind::Freighter;
    NpcDo       doing = NpcDo::Travel;
    int         faction = NO_FACTION;
    std::string name;
    SysId       sys = NO_SYSTEM;           // the system it's flying in
    DVec3       pos, vel;                  // relative to the system's primary star
    DVec3       was;                       // where it was when this step began (chasers aim at that: who moves first mustn't matter)
    Quaternion  rot = { 0, 0, 0, 1 };
    // It flies by the same rules as a captain's ship (ship.h), with an autopilot at the helm. 'pos', 'vel' and 'rot'
    // above are where that has put it.
    Ship        ship;
    ShipSpec    spec;
    PowerState  power;
    double      fuel = 0.0, tank = 0.0;
    bool        arrived = false;           // the autopilot has brought it to where it was sent
    DVec3       left;                      // the port it has just cast off from...
    double      leftClear = 0.0;           // ...and that body's radius (0: it's well away, or never was in port)
    DVec3       goal;
    double      until = 0.0;               // when what it's doing ends (docking, working, fleeing)
    int         from = -1, to = -1;        // markets (economy indices): where its cargo came from and is going;
                                           // miners and patrols: 'from' is home
    bool        jumping = false;           // freighters: 'to' is in another system, and it's bound for the jump point
    float       cargo[GOOD_COUNT] = {};
    float       total = 0.0f;
    int         prey = -1;                 // Hunt / Inspect: a ship id (an NPC's, or a captain's)
    double      close = 0.0;               // seconds it has been within reach of its prey
    double      since = 0.0;               // when it took up the chase
    bool        demanded = false;          // pirates: it has told its prey what it wants
    double      credits = 0.0;             // a paymaster's ship: the money aboard, on its way to its owner's capital...
    int         payee = NO_FACTION;        // ...and whose it is
    double      look = 0.0;                // when it next looks around
    uint8_t     hull = 1;                  // which hull it's built on (shipparts.h)
    // What it knows of what it's after (sensors.h): it flies at the quarry while its sensors hold it, and at the
    // last place they did once they don't
    double      seenAt = 0.0;
    DVec3       lastPos;
    double      heard = 0.0;               // pirates: no chasing another tachyon burst until then
    int         post = -1;                 // patrols: the sensor outpost (a body of its system) it's stationed at as a picket, if it is one
};

// A lane freighter between two systems, not flown: just when it gets there
struct Transit {
    int         from = -1, to = -1;        // hub markets
    float       cargo[GOOD_COUNT] = {};
    float       total = 0.0f;
    double      credits = 0.0;             // a paymaster's run: money aboard...
    int         payee = NO_FACTION;        // ...and whose
    double      arrive = 0.0;
    int         faction = NO_FACTION;
    std::string name;
};
