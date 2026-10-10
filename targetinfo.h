// targetinfo.h - the facts shown about a target (info panel, hover summary).
// A world that isn't on the captain's charts (see Knowledge in game.h) shows only what
// can be seen from afar: what sort of thing it is, how big, and its orbit.
#pragma once
#include "game.h"
#include <string>
#include <vector>

struct InfoRow { std::string label, value; };

std::vector<InfoRow> TargetInfo(const World &w, const Captain &c, TargetId t);   // empty if it's gone
inline std::vector<InfoRow> TargetInfo(const GameState &g, TargetId t) { return TargetInfo(g, g, t); }
std::string          WorldSummary(const Body &b, bool known = true);   // one line: type and population
const char          *BasicKind(const Body &b);                     // all that shows of an unscanned body: "Rocky planet", "Station", ...
std::string          StarSummary(const StarSystem &s);             // one line: class and multiplicity
