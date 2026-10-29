// partsdata.h - the numbers ships are built from, read from a file.
//
// Every hull and every part a yard sells is described in parts.txt, beside the
// executable: what it weighs, the room it takes, what it costs, and what it
// does. Change a number there and the game flies by it; add a section and
// there is a new part on the yards' shelves. With no file, the game uses the
// set it was built with (partsdefault.h, the same text: --write-parts writes
// it out to start from).
//
// The file is sections of "key = value" lines; '#' starts a comment.
//
//   [rules]              what a grade is worth, and what stretching a hull costs
//   [fuel <good>]        the energy in a tonne of a reactor fuel
//   [hull <id>]          one of the six hulls (courier, hauler, prospector,
//                        cutter, freighter, bulk): their shapes are drawn, so
//                        there are just these; what they hold is up to the file
//   [<part type> <id>]   a MODEL of a part: one of the kinds of reactor, of
//                        warp drive, and so on. A part aboard a ship is a model
//                        in a size (each twice the last) and a grade.
//
// The numbers in a model are for size 1, standard grade. Which ones a part uses
// goes by its type (shipparts.cpp turns them into what a ship can do).
//
// A ship's parts are saved by the ORDER their models come in the file, so new
// models go after the ones already there.
//
// Online, everyone must fly by the server's numbers: a captain whose parts
// differ from the server's is turned away (PartsHash).
#pragma once
#include "shipparts.h"
#include <string>
#include <vector>

struct PartModel {
    std::string id, name;               // "fission", "Fission reactor"
    double mass = 0.0, volume = 0.0, cost = 0.0;
    int    material = -1;               // the good a yard builds it from (half its mass of it)
    // Reactors. Heat is made at burn rate x the fuel's energy; power is that x the efficiency AT that burn rate.
    double burn = 0.0;                  // tonnes of fuel a second, flat out
    double floor = 0.0;                 // the share of that it can't be throttled below while it's lit
    double restart = 0.0;               // seconds to light it from cold
    std::vector<std::pair<double, double>> curve;       // efficiency against load (burn rate as a share of flat out): points, joined by straight lines
    int    fuel = -1;                   // the good it burns
    // Reactors (what each grade adds to the whole curve) and drive capacitors (what survives going in, and coming out)
    double efficiency = 0.0, efficiencyGrade = 0.0;
    // Engines and thrusters
    double thrust = 0.0, turn = 0.0;
    double draw = 0.0;                  // terawatts it takes (engines flat out; lasers cutting; sensors and cabins always)
    // Hyper and warp drives
    double speed = 0.0, speedGrade = 0.0;               // top speed, as a multiple of the standard drive's; and what each grade adds
    double fieldBase = 0.0;             // terawatts to hold the field at all (grows with the drive's size)
    double fieldMass = 0.0;             // ...and for each tonne at the speed of light, to the power below (the same at any size)
    double fieldPower = 0.0;            // the exponent: how steeply holding the field costs more with speed
    double accel = 0.0;                 // how fast it speeds up, c per second
    double maxMass = 0.0;               // the heaviest ship it will move, tonnes
    double jump = 0.0, jumpGrade = 0.0; // warp: terajoules a tonne to form the bubble; and the share each grade saves
    // Capacitors, tanks, bays and cabins
    double capacity = 0.0;              // terajoules; tonnes; berths
    // Mining lasers, scoops and skimmers
    double rate = 0.0;                  // tonnes a second
    double range = 0.0, rangeGrade = 0.0;
    // Sensors
    double reachSize = 0.0, reachGrade = 0.0;           // how much further each size and each grade reaches
    double area = 0.0, depth = 0.0;                     // tachyon detectors: the face (m^2) and depth (m)...
    double catches = 0.0, catchesGrade = 0.0;           // ...the share of decays caught...
    double resolution = 0.0, resolutionGrade = 0.0;     // ...and how well one count gives a direction (degrees)
};

struct PartRules {
    std::string gradeName[PART_GRADES];
    double better = 0.15;               // each grade does this much more...
    double leaner = 0.10;               // ...and weighs (or wastes) this much less
    double gradeCost[PART_GRADES] = { 1.0, 2.5, 6.0, 15.0 };
    double stretchRoom = 0.12, stretchMass = 0.06, mountMass = 0.03;        // what a stretch adds to a hull's room and mass; and a hardpoint to its mass
    double stretchCost = 0.25, stretchCostGrowth = 1.7;                     // the first stretch, as a share of the hull's price; and each one after against the last
    double mountCost = 0.20, mountCostGrowth = 1.8;
};

struct PartsData {
    PartRules rules;
    std::string hullName[HULL_COUNT], hullBlurb[HULL_COUNT];
    HullDef     hulls[HULL_COUNT];
    std::vector<PartModel> models[(int)PartType::Count];
    double   fuelTJ[GOOD_COUNT] = {};   // terajoules in a tonne of each fuel (0: not one)
    uint64_t hash = 0;
};

const PartsData &Parts();                                   // what the game is flying by
const PartModel &PartModelOf(PartType type, int variant);         // (a variant there isn't: the type's first)
inline const PartModel &PartModelOf(const Part &p) { return PartModelOf(p.type, p.variant); }
int      ModelCount(PartType type);
double   ReactorCurveAt(const PartModel &reactor, double load);     // its efficiency at a load, 0 .. 1 (standard grade)
uint64_t PartsHash();                                       // the same for two games only if every number is

const char *DefaultPartsText();
// Reads a parts file's text. False, with what's wrong and where, if it won't do: 'out' is then not to be used.
bool ParseParts(const std::string &text, PartsData &out, std::string &problem);
// Flies by the file at 'path' from here on. False if it isn't there ('problem' empty) or won't do (what's wrong): nothing changes then.
bool LoadPartsFile(const std::string &path, std::string &problem);
// ...the one beside the executable. Returns what was wrong with it, if it was there and wasn't used.
std::string LoadPartsBesideExe();
void UseDefaultParts();
