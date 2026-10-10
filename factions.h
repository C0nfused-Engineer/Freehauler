// factions.h - who holds human space.
//
// Like the galaxy, the factions are GENERATED from the seed: every game has
// different states, companies and outlaws, with different characters, and the
// same seed always rebuilds the same ones.
//
//   The free port      the home system: neutral ground that everyone trades at
//   Governments        five to seven states that divide the settled systems
//                      between them; each has its own laws, tariffs and patrols
//   Conglomerates      companies that own stations in every state's territory
//   Guilds             careers you can join: traders, explorers, the colonial
//                      league, bounty hunters
//   Clans              small pirate bands, each haunting a stretch of the frontier
//   (nobody)           unclaimed systems, and stations that own themselves
//
// Every faction has a character: where it stands on four ideological axes,
// six traits of temperament, and a few goals. FactionAgenda turns those, and
// the situation the faction is in, into what it most wants to do next. Nothing
// acts on an agenda yet; territory and ownership are fixed for now.
#pragma once
#include "core.h"
#include <string>
#include <vector>

struct StarSystem;
struct LoadedSystem;
class  Galaxy;

const int NO_FACTION = -1;

enum class FactionKind : int8_t { FreePort, Government, Conglomerate, Guild, Clan };

// Ideology: each axis runs -1 .. +1
enum Axis {
    AX_ORDER,       // personal liberty .. authority
    AX_MARKETS,     // free trade .. state control
    AX_FORCE,       // pacifist .. militarist
    AX_OUTLOOK,     // inward-looking, traditional .. expansionist
    AX_COUNT
};

// Temperament: each 0 .. 1. These are what an agenda is weighed by.
enum Trait {
    TR_AGGRESSION,  // reaches for force first
    TR_AMBITION,    // wants more than it has
    TR_GREED,       // values wealth over principle
    TR_CAUTION,     // avoids risk; prefers to defend
    TR_HONOR,       // keeps its word; dislikes underhand means
    TR_ZEAL,        // cares that others share its ideology
    TR_COUNT
};

enum class GoalType : int8_t {
    Expand,         // more systems (governments), more stations (conglomerates)
    Secure,         // safe borders and trade lanes
    Humble,         // bring a rival down (target: that faction)
    Wealth,         // get rich
    Convert,        // spread its ideology
    Monopoly,       // own its line of business outright (conglomerates)
    Plunder,        // take what others carry (clans)
    Serve,          // look after its members (guilds, the free port)
};
struct Goal { GoalType type; int target; float weight; };

enum class Slant : int8_t { Industry, Shipping, Agriculture, Energy, Finance, Mining, Count };
enum class GuildKind : int8_t { Traders, Explorers, Colonial, BountyHunters, Count };

struct Faction {
    int         id = NO_FACTION;
    FactionKind kind = FactionKind::Government;
    std::string name;
    Color       color = WHITE;
    float       axis[AX_COUNT] = {};
    float       trait[TR_COUNT] = {};
    std::vector<Goal> goals;
    float       strength = 1.0f;         // governments: about 1; how hard it pushes its borders out
    int64_t     seat = -1;               // its capital, headquarters or haunt (a SysId)
    DVec3       seatLy;                  // ...in light years from the home system
    float       reach = 0.0f;            // clans: how far from the haunt they raid (ly)

    // Governments (the free port has laws too: none)
    float       tariff = 0.0f;           // share of a cargo's value taken at its markets
    float       policing = 0.0f;         // 0 lawless .. 1 a patrol on every lane
    std::vector<int> contraband;         // goods (goods.h) that are illegal in its space
    // Conglomerates, guilds, clans
    Slant       slant = Slant::Industry;
    GuildKind   guild = GuildKind::Traders;
    int         patron = NO_FACTION;     // clans: the government that quietly pays for its raids, if any
};

struct FactionSet {
    std::vector<Faction> list;           // list[i].id == i. [0] is the free port.
    std::vector<float>   relation;       // how each pair gets on, -100 .. +100 (list.size() squared, symmetric)
    int homeCell[3] = { 0, 0, 0 };

    int   Count() const { return (int)list.size(); }
    const Faction *Get(int id) const { return id >= 0 && id < Count() ? &list[id] : nullptr; }
    float Relation(int a, int b) const { return a >= 0 && b >= 0 && a < Count() && b < Count() ? relation[a * Count() + b] : 0.0f; }
    std::string Name(int id, const char *nobody = "Independent") const { const Faction *f = Get(id); return f ? f->name : nobody; }
};

// The factions of a galaxy (which must already have its home system)
FactionSet GenerateFactions(const Galaxy &g);

// Which government rules a system: by where it is (light years from home) and how
// developed, plus the system's own hash for the ragged edge. NO_FACTION: unclaimed.
int  TerritoryRuler(const FactionSet &fs, DVec3 lyFromHome, float develop, uint64_t hash);
// Sets Body::owner for everything inhabited in a loaded system
void AssignOwners(const FactionSet &fs, const StarSystem &sys, LoadedSystem &loaded);

// How two factions get on, in a word: "Allied", "Friendly", "Neutral", "Rivals", "Hostile"
const char *StanceName(float relation);
const char *KindName(FactionKind k);
const char *SlantName(Slant s);
const char *TraitName(int t);
const char *AxisName(int a, bool positive);          // the name of one end of an axis
std::string IdeologyBlurb(const Faction &f);         // "Authoritarian, militarist, protectionist"
std::string GoalText(const FactionSet &fs, const Goal &g);

// Your standing with a faction (-100 .. +100) as a rank: "Enemy" ... "Honoured"
const char *RankName(const Faction &f, float standing);

// What a faction would most like to do next, best first. Each candidate action is
// scored from the situation (who its rivals and neighbours are, what's unclaimed
// nearby), weighed by its traits, and boosted where it serves one of its goals.
struct AgendaItem { std::string text; float score; int target; };
std::vector<AgendaItem> FactionAgenda(const FactionSet &fs, int id, int max = 4);
