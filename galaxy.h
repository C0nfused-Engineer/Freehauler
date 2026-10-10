// galaxy.h - stars, star systems, planets, moons, belts and rings.
// All of it is GENERATED from the galaxy seed and never saved: the same seed
// always rebuilds the same galaxy, and a system's contents are rebuilt
// whenever you get close to it.
#pragma once
#include "core.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
const int    SYSTEM_COUNT        = 12;
const double GALAXY_RADIUS_LY    = 22.0;
const double GALAXY_THICKNESS_LY = 3.0;
const double MIN_SEPARATION_LY   = 4.0;
const double FIELD_EDGE_MARGIN   = 1.35;    // field radius = outermost orbit * this
const double FIELD_HEIGHT_RATIO  = 0.10;    // field half-thickness = radius * this
const double LOAD_DISTANCE_LY    = 0.5;     // load a system's planets within this of its star...
const double UNLOAD_DISTANCE_LY  = 0.6;     // ...unload beyond this (hysteresis)
const double BELT_DENSITY        = 3.0e-10; // rocks per cubic unit at a belt's core (~250 within 30 km)
const double RING_DENSITY        = 6.0e-9;  // ring chunks are smaller and packed tighter
const unsigned DEFAULT_GALAXY_SEED = 1977;

// The habitable zone, as starlight received (1 = what Earth gets): liquid surface
// water is possible between these, and only between these. The HZ rings drawn
// around a star are the same limits as distances (0.95 and 1.67 AU for the Sun).
const double HZ_FLUX_INNER = 1.0 / (0.95 * 0.95);   // closer in: runaway greenhouse, oceans boil off
const double HZ_FLUX_OUTER = 1.0 / (1.67 * 1.67);   // farther out: oceans freeze

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
enum class Kind { Star, Planet, Moon, Station };

// What a settlement or station makes and needs (economy.cpp turns this into a market)
enum class Econ : int8_t {
    None,
    HighTech,       // core worlds: electronics, robotics, luxuries; hungry for food and metals
    Agricultural,   // garden worlds: food, spirits, textiles, biologicals
    Colony,         // domed colonies: need nearly everything
    GasMining,      // gas giant habitats: helium-3, fuel cells
    Research,       // research stations and outposts
    Mining,         // mining outposts: raw ores, cheap
    Industrial,     // underground colonies: alloys, machinery, reactor parts
    Refinery,       // belt refineries: pay well for ore, sell alloys
    Skimmer,        // gas giant skimmer stations: cheap fuel and helium-3
    Freeport,       // waystations: a bit of everything at fair prices
    Shipyard,       // orbital yards: buy alloys, electronics, reactor parts
};

// An asteroid belt's makeup (which ores its rocks hold); planetary rings are icy
enum class Composition : int8_t { Metallic, Stony, Carbonaceous, Mixed, Icy };

struct Body {
    std::string name;
    DVec3  local;            // position relative to the system's primary star
    double radius = 0.0;
    Color  lines = WHITE, fill = BLACK;
    Kind   kind = Kind::Planet;
    int    parent = -1;      // body it belongs to: planets -> star, moons -> planet
    double orbit  = 0.0;     // orbital radius around the parent

    // Description (filled in by the world / star generators)
    double massSolar  = 0.0; // stars: mass in Suns
    std::string type = "";   // e.g. "G2 V yellow dwarf", "Ocean world", "Icy moon"
    double tempK      = 0.0; // stars: photosphere; worlds: mean surface (cloud tops for giants)
    double gravity    = 0.0; // surface gravity in g
    std::string atmosphere = "", water = "", life = "", settlement = "";
    int    lifeLevel  = 0;   // 0 none, 1 microbial, 2 simple multicellular, 3 complex
    double population = 0.0;
    bool   inHZ       = false;
    double flux       = 0.0; // starlight received (1 = Earth's)
    Econ   econ       = Econ::None;   // what its market makes and needs (inhabited only)
    float  spin       = 0.0f;          // stations: rotation, degrees per second
};

struct Orbit { DVec3 u, v; double radius; Color color; };   // circle around the primary

// An asteroid belt around the star, or a ring system around a planet
struct Zone {
    std::string name;
    bool   ring = false;               // planetary ring (true) or asteroid belt
    int    parent = 0;                 // body index: the star for belts, the planet for rings
    DVec3  center, n, u, v;            // center (star-relative) and plane
    double rIn = 0, rOut = 0, halfH = 0;   // annulus radii and half-thickness
    double density = 0;                // peak rocks per cubic unit
    float  sizeMin = 1, sizeMax = 1;   // rock radius range (units)
    unsigned seed = 0;
    Composition composition = Composition::Mixed;   // which ores its rocks hold
    Color  color = GRAY;               // distant-view color
    std::vector<double> bandEdges;     // rings: band boundaries...
    std::vector<float>  bandDensity;   // ...and each band's relative density (0 = gap)
    std::vector<DVec3>  macro;         // belts: point cloud for the distant view (star-relative)
};

// A main-sequence star
struct StarInfo {
    char   cls = 'G';        // spectral class: O B A F G K M
    int    sub = 0;          // subclass 0 (hottest) - 9
    double mass = 1, lum = 1;// Suns
    double radius = 0;       // units
    double temp = 0;         // K
    Color  color = WHITE;
    bool   close = false;    // companion that orbits close in (planets circle both stars)
    double sep = 0;          // close: separation (AU before orbit scaling); wide: multiple of the outermost orbit
};

struct StarSystem {
    std::string name;
    DVec3    gal;            // galactic position (units), snapped so differences are exact
    double   starRadius = 0; // the primary's
    unsigned seed = 0;       // contents are regenerated from this whenever the system loads
    DVec3    n, u, v;        // system plane: normal + in-plane basis
    double   fieldR = 1, fieldH = 1; // gravity field: ellipsoid radius and half-thickness
    std::vector<StarInfo> stars;     // [0] = primary, then companions
    double   lumInner = 1.0;         // luminosity of the primary + close companions (Suns)
    double   hzIn = 0.0, hzOut = 0.0;// habitable zone (units)
};

// The contents of the one system you're in (or near). Stars come first in
// 'bodies', in the same order as StarSystem::stars.
struct LoadedSystem {
    int index = -1;          // -1: nothing loaded (interstellar space)
    std::vector<Body>  bodies;
    std::vector<Orbit> orbits;
    std::vector<Zone>  zones;   // asteroid belts and planetary rings
};

// ---------------------------------------------------------------------------
// Generation and queries
// ---------------------------------------------------------------------------
std::vector<StarSystem> GenerateGalaxy(unsigned seed);
void        GenerateSystemContents(const StarSystem &sys, LoadedSystem &out);
const char *StarDescription(const StarInfo &s);
const char *CompositionName(Composition c);          // "Metallic (M-type)", ...
const char *EconName(Econ e);                        // "High-tech", "Agricultural", ...

// How far out of a system's gravity field a point is: < 1 inside, > 1 outside
double FieldRatio(const StarSystem &sys, DVec3 fromStar);
// Rock density at a star-relative point (0 outside the zone)
double ZoneDensity(const Zone &z, DVec3 p);
// Nearest point on a zone's middle circle: where you arrive when you target it
DVec3  ZoneTargetPoint(const Zone &z, DVec3 shipLocal);
