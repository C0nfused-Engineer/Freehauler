// asteroids.cpp - generating and streaming rocks
#include "asteroids.h"
#include "goods.h"
#include <cmath>

// What rocks of each composition hold: (ore, share) pairs, shares summing to 1
struct OreShare { int ore; double share; };
static const std::vector<OreShare> &OreTable(Composition c)
{
    static const std::vector<OreShare> metallic = {
        { ORE_IRON, .35 }, { ORE_NICKEL, .20 }, { ORE_COBALT, .12 }, { ORE_COPPER, .10 }, { ORE_PLATINUM, .08 },
        { ORE_GOLD, .06 }, { ORE_RARE_EARTHS, .05 }, { ORE_IRIDIUM, .04 } };
    static const std::vector<OreShare> stony = {
        { ORE_SILICATES, .35 }, { ORE_IRON, .18 }, { ORE_TITANIUM, .14 }, { ORE_COPPER, .10 }, { ORE_RARE_EARTHS, .08 },
        { ORE_NICKEL, .07 }, { ORE_URANIUM, .05 }, { ORE_PLATINUM, .03 } };
    static const std::vector<OreShare> carbonaceous = {
        { ORE_CARBONACEOUS, .40 }, { ORE_WATER_ICE, .20 }, { ORE_SILICATES, .15 }, { ORE_AMMONIA_ICE, .08 },
        { ORE_IRON, .07 }, { ORE_RARE_EARTHS, .05 }, { ORE_URANIUM, .03 }, { ORE_GOLD, .02 } };
    static const std::vector<OreShare> mixed = {
        { ORE_IRON, .25 }, { ORE_SILICATES, .20 }, { ORE_NICKEL, .12 }, { ORE_CARBONACEOUS, .10 }, { ORE_COPPER, .08 },
        { ORE_TITANIUM, .08 }, { ORE_PLATINUM, .07 }, { ORE_WATER_ICE, .05 }, { ORE_COBALT, .05 } };
    static const std::vector<OreShare> icy = {
        { ORE_WATER_ICE, .70 }, { ORE_AMMONIA_ICE, .14 }, { ORE_METHANE_ICE, .10 }, { ORE_CARBONACEOUS, .06 } };
    switch (c) {
    case Composition::Metallic:     return metallic;
    case Composition::Stony:        return stony;
    case Composition::Carbonaceous: return carbonaceous;
    case Composition::Icy:          return icy;
    default:                        return mixed;
    }
}

int PickOre(Composition c, double u)
{
    const std::vector<OreShare> &t = OreTable(c);
    for (const OreShare &o : t) { if (u < o.share) return o.ore; u -= o.share; }
    return t.back().ore;
}

std::string CompositionOres(Composition c, int max)
{
    std::string s;
    int n = 0;
    for (const OreShare &o : OreTable(c)) {
        if (n == max) { s += ", ..."; break; }
        s += (n++ ? ", " : "") + std::string(GoodData(o.ore).name);
    }
    return s;
}

float AsteroidRadius(const Asteroid &a)
{
    return a.baseRadius * (0.35f + 0.65f * cbrtf(fmaxf(a.oreLeft, 0.0f) / a.oreMax));
}

// The rocks in one 10 km cell. Same cell + same zone seed = same rocks, every time.
static void GenerateCell(const CellKey &key, const LoadedSystem &L, const MinedMap &mined, std::vector<Asteroid> &out)
{
    DVec3 origin = { key[0] * AST_CELL, key[1] * AST_CELL, key[2] * AST_CELL };
    DVec3 middle = origin + DVec3{ AST_CELL * 0.5, AST_CELL * 0.5, AST_CELL * 0.5 };

    for (const Zone &z : L.zones) {
        // Skip zones this cell can't touch
        DVec3  q = middle - z.center;
        double h = Dot(q, z.n);
        if (fabs(h) > z.halfH + AST_CELL) continue;
        double r = Len(q - z.n * h);
        if (r < z.rIn - AST_CELL || r > z.rOut + AST_CELL) continue;

        uint64_t cellHash = Mix64(Mix64(Mix64(Mix64(z.seed) ^ (uint64_t)key[0]) ^ (uint64_t)key[1]) ^ (uint64_t)key[2]);
        Rng64 rng{ cellHash };
        double lambda = z.density * AST_CELL * AST_CELL * AST_CELL;     // expected rocks at peak density
        int count = (int)lambda + (rng.uni() < lambda - floor(lambda) ? 1 : 0);

        for (int k = 0; k < count; k++) {
            // Draw every random number up front so each rock's values never depend on which others were kept
            DVec3  p = origin + DVec3{ rng.uni() * AST_CELL, rng.uni() * AST_CELL, rng.uni() * AST_CELL };
            double keep = rng.uni(), sizeU = rng.uni(), oreU = rng.uni(), shapeU = rng.uni();
            Vector3 axis = { (float)rng.uni() - 0.5f, (float)rng.uni() - 0.5f, (float)rng.uni() - 0.5f };
            float spin = (float)(rng.uni() * 20.0 - 10.0), phase = (float)(rng.uni() * 360.0);

            if (keep * z.density > ZoneDensity(z, p)) continue;            // thinner toward the edges

            Asteroid a;
            a.id    = Mix64(cellHash + (uint64_t)k);
            a.local = p;
            a.baseRadius = z.sizeMin * powf(z.sizeMax / z.sizeMin, (float)(sizeU * sizeU));   // mostly small
            a.tmpl  = (int)(shapeU * AST_TEMPLATES) % AST_TEMPLATES;
            a.axis  = Vector3LengthSqr(axis) > 1e-6f ? Vector3Normalize(axis) : Vector3{ 0, 1, 0 };
            a.spin  = spin; a.phase = phase;
            a.ore   = PickOre(z.composition, oreU);
            float sizeFrac = (a.baseRadius - z.sizeMin) / (z.sizeMax - z.sizeMin);
            a.oreMax  = z.ring ? 2.0f + 60.0f * sizeFrac : 10.0f + 290.0f * sizeFrac;
            auto it = mined.find(a.id);
            a.oreLeft = it != mined.end() ? it->second : a.oreMax;
            if (a.oreLeft <= 0.0f) continue;                                  // mined out earlier
            a.name = TextFormat("%s %06X", GoodData(a.ore).ice ? "Ice chunk" : "Asteroid", (unsigned)(a.id & 0xFFFFFF));
            out.push_back(a);
        }
    }
}

void UpdateField(AsteroidField &field, const LoadedSystem &L, const MinedMap &mined, DVec3 shipLocal)
{
    if (field.system != L.index) { field.cells.clear(); field.system = L.index; }
    if (L.index < 0 || L.zones.empty()) { field.cells.clear(); return; }

    CellKey c = { (long long)floor(shipLocal.x / AST_CELL), (long long)floor(shipLocal.y / AST_CELL),
                  (long long)floor(shipLocal.z / AST_CELL) };
    for (auto it = field.cells.begin(); it != field.cells.end();) {
        bool far = llabs(it->first[0] - c[0]) > AST_CELL_RANGE + 1 || llabs(it->first[1] - c[1]) > AST_CELL_RANGE + 1 ||
                   llabs(it->first[2] - c[2]) > AST_CELL_RANGE + 1;
        it = far ? field.cells.erase(it) : std::next(it);
    }
    for (int dx = -AST_CELL_RANGE; dx <= AST_CELL_RANGE; dx++)
        for (int dy = -AST_CELL_RANGE; dy <= AST_CELL_RANGE; dy++)
            for (int dz = -AST_CELL_RANGE; dz <= AST_CELL_RANGE; dz++) {
                CellKey k = { c[0] + dx, c[1] + dy, c[2] + dz };
                if (field.cells.count(k)) continue;
                std::vector<Asteroid> rocks;
                GenerateCell(k, L, mined, rocks);
                field.cells.emplace(k, std::move(rocks));
            }
}

Asteroid *FindAsteroid(AsteroidField &field, uint64_t id)
{
    if (id == 0) return nullptr;
    for (auto &cell : field.cells)
        for (Asteroid &a : cell.second)
            if (a.id == id) return &a;
    return nullptr;
}

const Asteroid *FindAsteroid(const AsteroidField &field, uint64_t id)
{
    return FindAsteroid(const_cast<AsteroidField &>(field), id);
}

void RemoveAsteroid(AsteroidField &field, uint64_t id)
{
    for (auto &cell : field.cells)
        for (size_t i = 0; i < cell.second.size(); i++)
            if (cell.second[i].id == id) { cell.second.erase(cell.second.begin() + i); return; }
}
