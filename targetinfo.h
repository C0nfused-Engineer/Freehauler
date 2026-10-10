// targetinfo.h - the facts shown about a target (info panel, hover summary)
#pragma once
#include "game.h"
#include <string>
#include <vector>

struct InfoRow { std::string label, value; };

std::vector<InfoRow> TargetInfo(const World &w, const Captain &c, TargetId t);   // empty if it's gone
inline std::vector<InfoRow> TargetInfo(const GameState &g, TargetId t) { return TargetInfo(g, g, t); }
std::string          WorldSummary(const Body &b);                  // one line: type and population
std::string          StarSummary(const StarSystem &s);             // one line: class and multiplicity
