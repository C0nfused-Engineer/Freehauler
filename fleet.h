// fleet.h - the ships that carry the trade, and what a run in one costs.
//
// Trade doesn't move by itself. Every faction that carries goods has its own
// DESIGN for each job, put together from the same hulls and parts a captain can
// buy (shipparts.h), by rule, to suit what the faction is: a shipping company
// stretches its hulls and runs them lean; a state that reaches for force runs
// faster, with engines to match; an energy company fits fusion; a clan flies
// whatever is small, cheap and quick. A conglomerate fits a better grade of the
// parts it is known for. When there are more parts to choose from, the same
// rules will make more of them.
//
//   Bulk freighter   between systems: the cheapest tonne carried
//   Local hauler     between a system's worlds and its hub
//   Tanker           fuels and plasma cells, between systems
//   Liner            people, between systems
//
// A design's trip figures are worked out once, in closed form, from the flight
// model's own laws (ship.h): the time and the fuel for a run of so many light
// years, loaded. Going to warp is what costs: the jump takes far more than the
// passage, so a short run is nearly as dear as a long one.
//
// FLEETS are counts of ships of each role, by owner, based at each system's
// hub (econ.h). What a lane can carry in a day is what the ships based at its
// two ends can. They burn real fuel, from the stock of the port they fill up
// at. They wear out, and raiders take some; shipyards build more.
#pragma once
#include "factions.h"
#include "shipparts.h"
#include <string>

enum ShipRole : uint8_t { ROLE_BULK, ROLE_HAULER, ROLE_TANKER, ROLE_LINER, ROLE_COUNT };
const char *ShipRoleName(int role);

const double FLEET_CRUISE     = 0.5;        // how hard crews run their drives, as a share of top speed (a design may say otherwise)
const double USUAL_RUN_LY     = 5.0;        // the run a design is costed on, when its parts are chosen
const double FLEET_RANGE_LY   = 12.0;       // the longest lane: tanks are sized to fly it and get home
const double SYSTEM_LEG_AU    = 25.0;       // from a port out to where a ship can go to warp (and in again at the other end)
const double LOCAL_LEG_AU     = 8.0;        // between a system's worlds
const double DOCK_SECONDS     = 120.0;      // alongside, loading or unloading
const double SHIP_LIFE_DAYS   = 1000.0;     // market days a ship lasts: a fleet loses this share of itself a day to age
const double RUNNING_COSTS    = 0.5;        // what a run costs besides its fuel (crew, upkeep, the owner's cut), as a share of the fuel bill
const double RAID_SHIP_LOSS   = 0.004;      // the share of the ships based in raided space that raiders take a day, at the worst
const double FLEET_SPARE      = 1.3;        // fleets begin with this many times the shipping their trade needs
const double FLEET_REBALANCE  = 0.02;       // the share of a difference in how busy two ends of a lane are that ships move to even out, a day

struct ShipDesign {
    std::string name;                   // "Xanabath Union bulk freighter"
    ShipBuild   build;
    ShipSpec    spec;
    double cargo = 0.0, berths = 0.0;   // what it carries: tonnes, or people
    double mass = 0.0;                  // loaded and fuelled, tonnes
    double cruise = FLEET_CRUISE;       // how hard it runs its drives
    int    fuelGood = REF_REACTOR_FUEL;
    // A run between systems, loaded: seconds and tonnes of fuel, as so much whatever the distance and so much a light year
    double fixedSec = 0.0, secPerLy = 0.0, fixedFuel = 0.0, fuelPerLy = 0.0;
    // A run within a system
    double localSec = 0.0, localFuel = 0.0;
    float  materials[GOOD_COUNT] = {};  // what a yard builds one from, tonnes
    double tonnage = 0.0;               // ...which comes to this much ship
    double value = 0.0;                 // credits, new

    double Seconds(double ly) const { return fixedSec + secPerLy * ly; }
    double Fuel(double ly) const    { return fixedFuel + fuelPerLy * ly; }
};

// The ship a faction builds for a job (NO_FACTION: what independent owners fly)
ShipDesign DesignShip(const FactionSet &fs, int faction, int role);
