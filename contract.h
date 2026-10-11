// contract.h - a contract, as data (the rules are in contracts.h).
//
// A contract is posted at a market (a world or station), by whoever runs the
// place, a faction with an agenda, or a guild for its members. While it's on
// offer it belongs to the world; the captain who accepts it takes it off the
// board and carries it (Captain::jobs) until it's delivered, failed or handed
// back. What a captain knows of the boards is a copy, as of when they asked
// (Captain::offers): like prices, it goes stale.
#pragma once
#include "galaxy.h"
#include <string>

enum class ContractKind : uint8_t {
    Haulage,        // the issuer's cargo, from one market to another, against a deposit
    Supply,         // bring so many tonnes of a good, found however you like
    Smuggling,      // haulage of something the law where it's going forbids
    Survey,         // scan the worlds of an empty system and come back
    Courier,        // messages and passengers: no hold space, but no dawdling
    Count
};

// What finishing it does to the world, beyond the goods
enum class ContractEffect : uint8_t {
    None,
    Relief,         // whatever has gone wrong at the destination is over sooner
    ArmPatrols,     // the clan 'effectOn' gets chased off the lanes
    ArmClan,        // the clan 'effectOn' raids harder
    Charts,         // the surveyed system goes on the guild's charts
    Count
};

struct Contract {
    uint32_t       id = 0;
    ContractKind   kind = ContractKind::Haulage;
    ContractEffect effect = ContractEffect::None;
    int      issuer = NO_FACTION;       // who pays, and whose regard it earns (NO_FACTION: the station itself)
    int      guild = NO_FACTION;        // members of this guild only
    int      against = NO_FACTION;      // aimed at this faction: it will think less of you
    int      effectOn = NO_FACTION;
    int      licence = NO_FACTION;      // needs a bonded carrier's licence from this government
    int      law = NO_FACTION;          // the cargo is contraband under this government, whose space it starts or ends in: smuggling
    float    minStanding = 0.0f;        // the least standing with the issuer (or the guild) to be given it
    bool     wide = false;              // on its faction's list as well as the station's board

    uint64_t fromBody = 0, toBody = 0;  // where it's posted (and any cargo collected); where it's delivered
    SysId    fromSys = NO_SYSTEM, toSys = NO_SYSTEM;
    std::string fromName, toName;
    SysId    surveySys = NO_SYSTEM;     // surveys: the system to scan...
    std::string surveyName;
    int      need = 0;                  // ...and how many of its worlds
    uint64_t scanMask = 0;              // ...and which have been (by body index)

    int      good = -1;
    float    tonnes = 0.0f;
    float    pay = 0.0f;
    float    collateral = 0.0f;         // put down on accepting, returned on delivery
    double   expires = 0.0;             // on offer until then
    double   allowed = 0.0;             // seconds from accepting to delivery

    // Once taken
    double   deadline = 0.0;
    bool     loaded = false;            // the cargo is aboard (or there's none to load)

    // In a captain's record of the boards
    uint64_t listKey = 0;               // which board it was seen on (BoardKey)

    int Scanned() const { int n = 0; for (uint64_t m = scanMask; m; m &= m - 1) n++; return n; }
};

// Papers a captain holds from a faction (Captain::papers, by faction id)
enum Papers : uint8_t {
    PAPER_MEMBER  = 1,                  // guilds: a member
    PAPER_CARRIER = 2,                  // governments: bonded carrier (may take high-value haulage from its space)
    PAPER_PERMIT  = 4,                  // governments: restricted goods permit (its contraband is lawful for you)
};

// A board is a market's (its body id) or a faction's
inline uint64_t BoardKey(int faction) { return 0xF000000000000000ull | (uint32_t)faction; }
