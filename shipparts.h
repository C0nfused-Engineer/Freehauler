// shipparts.h - what ships are built from.
//
// A ship is a hull with parts fitted in it. A hull is so many cubic metres of
// room inside and so many hardpoints outside; every part takes room and adds
// mass, and the ones that work on the outside (engines, thrusters, lasers,
// scoops, radar, infrared arrays, telescopes) take a hardpoint each. Anything that fits can go in: what the ship
// can then DO is whatever its parts add up to (SpecOf gives the ShipSpec the
// flight model flies by).
//
// Parts come in sizes (each twice the last) and grades (each lighter and
// better, and a good deal dearer). A hull can be stretched for more room, or
// given another hardpoint, a few times; each time costs more than the last,
// and before long the money is better spent on a bigger hull.
#pragma once
#include "factions.h"
#include "npc.h"
#include "ship.h"
#include <string>
#include <vector>

enum class PartType : uint8_t {
    Reactor, Engine, Thrusters, HyperDrive, WarpDrive, DriveCap, Bank,
    Tank, Hold, MiningLaser, Scoop, Skimmer,
    // Sensors (sensors.h): each sees its own way
    Telescope,          // visual: short-ranged, but it's what tells you who a ship is; it charts worlds too
    Radar,              // active: finds anything in reach, and anyone can hear it
    IrArray,            // passive: sees heat, so a ship working hard shows from far off and a cold one hardly at all
    TachyonDetector,    // catches the burst a warp bubble gives off when it collapses, and the direction it came from
    Cabins,             // berths for passengers (who pay) and for refugees (who'd otherwise ride in the hold)
    Count
};
const int PART_SIZES  = 6;          // 1 .. 6
const int PART_GRADES = 4;          // 0 standard .. 3 elite
const int MAX_STRETCH = 5;          // how many times a hull can be given more room, or another hardpoint

// Reactors are built for one fuel, and there are three kinds, each a step up: fission, on reactor fuel, is the
// everyday one; deuterium fusion gets several times as much from a tonne and more power from the same room; the
// helium-3 fusor is the best of both again, and light with it.
enum ReactorFuel : uint8_t { BURNS_REACTOR_FUEL, BURNS_DEUTERIUM, BURNS_HELIUM3, REACTOR_FUELS };

struct Part {
    PartType type = PartType::Hold;
    uint8_t  size = 1, grade = 0;
    uint8_t  variant = 0;           // which model of the part it is (partsdata.h): for reactors, the first three are a ReactorFuel
    int16_t  maker = NO_FACTION;    // the conglomerate whose line it is, if it's one of their specialities
    bool operator==(const Part &o) const { return type == o.type && size == o.size && grade == o.grade && variant == o.variant && maker == o.maker; }
};

enum { HULL_COURIER, HULL_HAULER, HULL_PROSPECTOR, HULL_CUTTER, HULL_FREIGHTER, HULL_BULK, HULL_COUNT };
struct HullDef {
    const char *name = "", *blurb = "";
    double volume;                  // cubic metres inside
    int    hardpoints;
    double mass;                    // tonnes, bare
    double cost;
};

struct ShipBuild {
    int     hull = HULL_HAULER;
    uint8_t stretch = 0;            // times it's been given more room...
    uint8_t mounts = 0;             // ...and extra hardpoints
    std::vector<Part> parts;
};

// What one part is, in numbers
struct PartInfo {
    std::string name;               // "Improved fusion reactor, size 2"
    std::string does;               // "230 TW at 44% from Fuel"
    double mass = 0, volume = 0, cost = 0;
    bool   external = false;        // takes a hardpoint
    int    material = -1;           // the good a yard builds it from...
    double tonnes = 0;              // ...and how much
};

const HullDef &Hull(int id);
const char    *PartTypeName(PartType t);
const char    *GradeName(int grade);
bool           IsExternal(PartType t);
PartInfo       Describe(const Part &p, const FactionSet *fs = nullptr);
int            FuelGoodOf(int reactorFuel);
// What a part of a type does at size 1: the thing it's sized by (a reactor's terawatts, a drive's heaviest ship, a bay's tonnes...)
double         PartRating(PartType type, int grade = 0, int variant = 0);

// The hull as built: its room and hardpoints with the stretches, and what's used of them
double BuildVolume(const ShipBuild &b);
double VolumeUsed(const ShipBuild &b);
int    BuildHardpoints(const ShipBuild &b);
int    HardpointsUsed(const ShipBuild &b);
// What the next stretch (or hardpoint) would cost; and what the ship as it stands is worth new
double StretchCost(const ShipBuild &b, bool hardpoint);
double BuildValue(const ShipBuild &b);

ShipBuild StarterBuild();
ShipSpec  SpecOf(const ShipBuild &b);
// A ship of each NPC kind, from the same hulls and parts ('load': the tonnes a freighter must carry)
ShipBuild NpcBuild(NpcKind kind, double load);

// Which parts a conglomerate's line of business makes it good at
bool Specialises(Slant s, PartType t);
