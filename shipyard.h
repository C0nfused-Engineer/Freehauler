// shipyard.h - changing your ship.
//
// Done in dock at a shipyard or a starbase. A yard builds parts out of what it
// has in stock (reactor cores, ship components, hull plating, machinery,
// electronics, instruments), so a part costs more where those are dear and
// can't be had at all where they've run out; what's taken out is bought back at
// a loss. Every yard offers the standard and improved grades; a company's yard,
// or a starbase, goes a grade better, a company's own speciality a grade better
// again, and the top grade is for customers it knows.
#pragma once
#include "game.h"

const double RESALE = 0.6;                  // what a part (or a hull) fetches, of what it costs new
const float  ELITE_STANDING = 20.0f;        // the standing with a yard's owner that the top grade wants

// The body whose yard the captain is docked at, or null
const Body *Shipyard(const Captain &c);
// The best grade of a part this yard will sell this captain; and whose line it is there (NO_FACTION: nobody's in particular)
int    YardGrade(const World &w, const Captain &c, const Body &yard, PartType type);
int    YardMaker(const World &w, const Body &yard, PartType type);
// What things cost here, to this captain. Negative: the yard hasn't the stock to build it.
double PartPrice(const World &w, const Captain &c, const Body &yard, const Part &p);
double HullPrice(const World &w, const Captain &c, const Body &yard, int hull);       // (after trading in the hull you have)
// Why a part can't go in ("" if it can): no room, no hardpoint, wrong fuel, ...
std::string FitProblem(const ShipBuild &b, const Part &p);

// Actions (sent as Commands). Each says why not, if not.
bool FitPart(World &w, Captain &c, Part p);
bool RemovePart(World &w, Captain &c, int index);
bool BuyHull(World &w, Captain &c, int hull);
bool StretchHull(World &w, Captain &c, bool hardpoint);

// A captain's ship has changed: work out afresh what it can do (and keep its tank, hold and capacitors within it)
void Refit(Captain &c);

// Parts as numbers, for Commands and the wire
int32_t PackPart(const Part &p);
Part    UnpackPart(int32_t v);

// Saving a captain's ship
void WriteBuild(const Captain &c, std::string &out);
bool ReadBuildLine(Captain &c, const char *key, const char *rest);
