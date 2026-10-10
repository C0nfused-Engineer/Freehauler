// asteroids.h - rocks streamed in around the ship.
// Belts and rings are far too big to hold every rock, so asteroids are made
// on demand in 10 km cells around the ship. Each cell's rocks come from a
// seed, so the same place always has the same rocks; how much ore you've
// taken from each rock is saved separately (WorldDeltas::mined).
#pragma once
#include "galaxy.h"
#include <array>
#include <map>
#include <unordered_map>
#include <vector>

const double AST_CELL        = 2000.0;      // 10 km cells
const int    AST_CELL_RANGE  = 3;           // cells generated in each direction around the ship
const double AST_DRAW_RANGE  = 6000.0;      // rocks drawn (and targetable) out to 30 km
const double AST_TARGET_MAX_SPEED = 2000.0; // rocks are visible at any speed, targetable below 10 km/s
const int    AST_TEMPLATES   = 16;          // distinct procedural rock shapes

struct Asteroid {
    std::string name;
    uint64_t id = 0;
    DVec3    local;                    // star-relative
    float    baseRadius = 1;
    int      tmpl = 0;                 // which procedural shape
    Vector3  axis = { 0, 1, 0 };       // tumble axis...
    float    spin = 0, phase = 0;      // ...degrees per second, starting angle
    int      ore = 0;
    float    oreMax = 1, oreLeft = 1;  // tonnes
};

using MinedMap = std::unordered_map<uint64_t, float>;   // rock id -> ore left

using CellKey = std::array<long long, 3>;
struct AsteroidField {
    SysId system = NO_SYSTEM;                          // loaded system the cells belong to
    std::map<CellKey, std::vector<Asteroid>> cells;    // generated cells around the ship
};

// Which ore a rock of this composition holds, for a uniform draw u in [0, 1)
int PickOre(Composition c, double u);
// The ores of a composition, most common first: "Iron, Nickel, Cobalt, ..."
std::string CompositionOres(Composition c, int max = 4);

// Rocks shrink as they're mined out
float AsteroidRadius(const Asteroid &a);

// Keep the cells around the ship generated; drop the ones left behind
void UpdateField(AsteroidField &field, const LoadedSystem &L, const MinedMap &mined, DVec3 shipLocal);
Asteroid       *FindAsteroid(AsteroidField &field, uint64_t id);
const Asteroid *FindAsteroid(const AsteroidField &field, uint64_t id);
void            RemoveAsteroid(AsteroidField &field, uint64_t id);
