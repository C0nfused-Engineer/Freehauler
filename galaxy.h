// galaxy.h - the galaxy, its stars, star systems, planets, moons, belts and rings.
// All of it is GENERATED from the galaxy seed and never saved: the same seed
// always rebuilds the same galaxy.
//
// The galaxy is the size of a real one (about 100,000 light years across, tens
// of billions of systems), so it's never built whole. Space is a grid of cells
// 2.5 light years on a side; each cell holds one system or none, decided by a
// hash of the seed and the cell against the density of stars there (a disk
// with spiral arms and a central bulge). Systems are made a sector (4x4x4
// cells) at a time as something looks at them, and forgotten again later. A
// system's planets are built whenever you get close to it.
//
// People (humans only; nobody else has been found) live in one bubble around
// the home system: crowded core worlds near home, thinning out to frontier
// outposts, and beyond that nobody at all.
#pragma once
#include "core.h"
#include "factions.h"
#include <string>
#include <unordered_map>
#include <vector>

// Which star system: its cell in the galaxy grid, packed into one number
typedef int64_t SysId;
const SysId NO_SYSTEM = -1;

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
// Star positions sit on a grid of POS_GRID units and a cell is a whole number of
// grid steps, so star-to-star differences are exact in double precision.
const double POS_GRID      = 1024.0;
const double CELL_U        = 4619497301065.0 * POS_GRID;   // 2.5 light years
const double CELL_LY       = CELL_U / LY_U;
const int    SECTOR_CELLS  = 4;             // cells per sector edge (generated and cached together)
const double CELL_MARGIN   = 0.2;           // stars keep this fraction of a cell from its walls: never under 1 ly apart

const double GALAXY_RADIUS_LY      = 55000.0;   // no stars beyond this...
const double GALAXY_HALF_HEIGHT_LY = 5000.0;    // ...or this far above or below the disk
const double DISK_SCALE_LY         = 8500.0;    // the disk thins out by e every this far from the center
const double THIN_DISK_LY          = 1000.0;    // scale heights
const double THICK_DISK_LY         = 3000.0;
const double LOCAL_DENSITY         = 0.003;     // systems per cubic light year around home (as near the Sun)
const double MAX_DENSITY           = 0.05;      // the core is capped at this
const double HOME_RADIUS_LY        = 26000.0;   // home is as far from the center as the Sun is

const double CORE_RADIUS_LY     = 20.0;     // human space: fully developed within this of home...
const double FRONTIER_RADIUS_LY = 65.0;     // ...thinning to nothing by here
const int    NAMED_CELLS        = 32;       // systems within this many cells of home can have proper names
const double NAV_RANGE_LY       = 30.0;     // stars within this of the ship are on the scope

const double FIELD_EDGE_MARGIN   = 1.35;    // field radius = outermost orbit * this
const double FIELD_HEIGHT_RATIO  = 0.10;    // field half-thickness = radius * this
const double LOAD_DISTANCE_LY    = 0.5;     // load a system's planets within this of its star...
const double UNLOAD_DISTANCE_LY  = 0.6;     // ...unload beyond this (hysteresis)
const double OUTPOST_ORBIT       = 0.22;    // sensor outposts sit well inside the system: this far out, as a share of the gravity field's radius
const double BELT_DENSITY        = 3.0e-10; // rocks per cubic unit at a belt's core (~250 within 30 km)
const double RING_DENSITY        = 6.0e-9;  // ring chunks are smaller and packed tighter
const unsigned DEFAULT_GALAXY_SEED = 1977;

// The habitable zone, as starlight received (1 = what Earth gets): liquid surface
// water is possible between these, and only between these. The HZ rings drawn
// around a star are the same limits as distances (0.95 and 1.67 AU for the Sun).
const double HZ_FLUX_INNER = 1.0 / (0.95 * 0.95);   // closer in: runaway greenhouse, oceans boil off
const double HZ_FLUX_OUTER = 1.0 / (1.67 * 1.67);   // farther out: oceans freeze

// Systems are kept flyable: hyper crosses about 0.7 AU a second
const double MAX_PLANET_ORBIT_AU = 80.0;     // no planet forms (or survives) farther out than this...
const double MAX_BELT_AU         = 150.0;    // ...no belt reaches past this...
const double MAX_COMPANION_AU    = 600.0;    // ...and no companion star orbits beyond this
const double CORE_WORLD_POPULATION = 3e8;    // a settled world with this many people is a core world

const double MEARTH_KG = 5.972e24, REARTH_KM = 6371.0;
const double GIANT_MASS = 14.0;             // Earths: from here up a planet is all deep atmosphere (Neptune is 17)
const double GAS_GIANT_MASS = 50.0;         // ...and from here it's a gas giant rather than an ice giant

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
    Power,          // solar collectors: fuel cells, and cheap fuel
};

// Who lives on a world, or what kind of station it is
enum class Settlement : int8_t {
    None,
    CoreWorld, SettledWorld, DomedColonies, OrbitalHabitats, ResearchStation, ResearchOutpost,
    MiningOutpost, UndergroundColony,
    // stations
    OrbitalPort, Shipyard, SkimmerStation, OreTerminal, OreRefinery, Waystation, Freeport,
    Starbase,           // the great port over a core world
    SolarCollector,     // close in to a star, beaming its power out
    Greenhouse,         // feeding a colony that can't feed itself
    SensorOutpost,      // a state's tachyon early-warning station, one of several around a system (sensors.h): a military
                        // installation, with pickets on guard; nobody trades there
};

// What a world has to offer, each 0 (none) to 1 (rich). Settlements go where
// these are; the economy decides what's made from them.
enum Resource { RES_METALS, RES_PRECIOUS, RES_MINERALS, RES_ICES, RES_GASES, RES_BIOMASS, RES_COUNT };

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
    uint64_t id   = 0;       // the same for this body in every session: markets and missions key on it
    int    owner  = NO_FACTION;   // the faction that runs it (inhabited places; set when the system loads). NO_FACTION: its own.

    // Description (filled in by the world / star generators)
    double massSolar  = 0.0; // stars: mass in Suns
    double massEarth  = 0.0; // planets and moons: mass in Earths
    bool   giant      = false;   // gas giant, ice giant or mini-Neptune: all atmosphere, nowhere to land
    bool   dwarf      = false;   // dwarf planet
    std::string type = "";   // e.g. "G2 V yellow dwarf", "Ocean world", "Icy moon"
    double tempK      = 0.0; // stars: photosphere; worlds: mean surface (cloud tops for giants)
    double gravity    = 0.0; // surface gravity in g
    std::string atmosphere = "", water = "", life = "";
    Settlement settlement = Settlement::None;
    int    lifeLevel  = 0;   // 0 none, 1 microbial, 2 simple multicellular, 3 complex
    double population = 0.0;
    bool   inHZ       = false;
    double flux       = 0.0; // starlight received (1 = Earth's)
    Econ   econ       = Econ::None;   // what its market makes and needs (inhabited only)
    float  resource[RES_COUNT] = {};
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
    uint64_t seed = 0;
    Composition composition = Composition::Mixed;   // which ores its rocks hold
    Color  color = GRAY;               // distant-view color
    std::vector<double> bandEdges;     // rings: band boundaries...
    std::vector<float>  bandDensity;   // ...and each band's relative density (0 = gap)
    std::vector<DVec3>  macro;         // belts: point cloud for the distant view (star-relative)
};

enum class StarKind : int8_t { MainSequence, Giant, WhiteDwarf, NeutronStar, BrownDwarf };

struct StarInfo {
    StarKind kind = StarKind::MainSequence;
    char   cls = 'G';        // spectral class: O B A F G K M; L T Y for brown dwarfs; D white dwarf; N neutron star
    int    sub = 0;          // subclass 0 (hottest) - 9
    double mass = 1, lum = 1;// Suns
    double radius = 0;       // units
    double temp = 0;         // K
    Color  color = WHITE;
    bool   close = false;    // companion that orbits close in (planets circle both stars)
    double sep = 0;          // close: separation in AU; wide: multiple of the system's outer edge
};

struct StarSystem {
    SysId    id = NO_SYSTEM;
    std::string name;
    int      cx = 0, cy = 0, cz = 0; // its cell in the galaxy grid (the galactic center is cell 0, 0, 0's corner)
    DVec3    off;            // the primary's position within the cell (units, on the position grid)
    double   starRadius = 0; // the primary's
    uint64_t seed = 0;       // contents are regenerated from this whenever the system loads
    DVec3    n, u, v;        // system plane: normal + in-plane basis
    double   fieldR = 1, fieldH = 1; // gravity field: ellipsoid radius and half-thickness
    std::vector<StarInfo> stars;     // [0] = primary, then companions
    double   lumInner = 1.0;         // luminosity of the primary + close companions (Suns)
    double   hzIn = 0.0, hzOut = 0.0;// habitable zone (units)
    double   age = 4.5;              // billions of years
    double   metal = 0.0;            // heavy elements, [Fe/H]: 0 = the Sun's, -1 = a tenth of it
    float    develop = 0.0f;         // human presence: 1 at home, 0 outside the bubble
    int      ruler = NO_FACTION;     // the government whose space this is (NO_FACTION: unclaimed)
    bool     named = false;          // has a proper name (otherwise a catalogue number)
};

// The contents of the one system you're in (or near). Stars come first in
// 'bodies', in the same order as StarSystem::stars.
struct LoadedSystem {
    SysId index = NO_SYSTEM; // NO_SYSTEM: nothing loaded (interstellar space)
    std::vector<Body>  bodies;
    std::vector<Orbit> orbits;
    std::vector<Zone>  zones;   // asteroid belts and planetary rings
};

// ---------------------------------------------------------------------------
// The galaxy
// ---------------------------------------------------------------------------
class Galaxy {
public:
    unsigned seed = DEFAULT_GALAXY_SEED;
    SysId    home = NO_SYSTEM;       // where new captains start, and the middle of human space
    FactionSet factions;             // the states, companies, guilds and clans of human space (factions.h)

    void Init(unsigned seed);        // a new galaxy: finds the home system, and who holds the space around it

    bool              Exists(SysId id) const { return Find(id) != nullptr; }
    const StarSystem *Find(SysId id) const;
    const StarSystem &operator[](SysId id) const;   // (an empty stand-in if there's no such system)

    // Vector from one system's primary to another's (exact between neighbours)
    DVec3 Between(SysId from, SysId to) const;
    // Galactic coordinates (light years from the center) of a point given relative to a system's star
    DVec3 PositionLy(SysId anchor, DVec3 pos = {}) const;
    // Every system within 'radius' of that point
    void  Nearby(SysId anchor, DVec3 pos, double radius, std::vector<const StarSystem *> &out) const;
    // The system nearest that point, looking 'cells' cells in every direction (null if none that close)
    const StarSystem *Nearest(SysId anchor, DVec3 pos, int cells = 3) const;

    void Trim();                     // forget sectors nobody has looked at for a while
    int  CachedSectors() const { return (int)sectors.size(); }

private:
    struct Sector { uint32_t used = 0; int8_t slot[SECTOR_CELLS * SECTOR_CELLS * SECTOR_CELLS]; std::vector<StarSystem> systems; };
    mutable std::unordered_map<uint64_t, Sector> sectors;
    mutable uint32_t tick = 0;
    int homeCell[3] = { 0, 0, 0 };

    const Sector     &SectorOf(int cx, int cy, int cz) const;
    const StarSystem *AtCell(int cx, int cy, int cz) const;
};

SysId CellId(int cx, int cy, int cz);                        // NO_SYSTEM if outside the grid
uint64_t CellHashOf(unsigned seed, const StarSystem &s);     // the hash its cell was made from (the one TerritoryRuler takes)
void  IdCell(SysId id, int &cx, int &cy, int &cz);

// Systems per cubic light year at a galactic position (light years from the center; y is up out of the disk)
double GalaxyDensity(DVec3 ly);

// ---------------------------------------------------------------------------
// Generation and queries
// ---------------------------------------------------------------------------
// 'lean': skip what only drawing needs (the belts' point clouds), for surveys of many systems
void        GenerateSystemContents(const StarSystem &sys, LoadedSystem &out, bool lean = false);
const char *StarDescription(const StarInfo &s);      // "G2 V yellow dwarf", "White dwarf", ...
const char *StarClassCode(const StarInfo &s);        // "G2 V", "K1 III", "DA", "NS", "T5"
const char *CompositionName(Composition c);          // "Metallic (M-type)", ...
const char *EconName(Econ e);                        // "High-tech", "Agricultural", ...
const char *SettlementName(Settlement s);            // "Core world", "Orbital port", ... ("Uninhabited")
const char *ResourceName(int r);                     // "Metals", "Ices", ...

// How far out of a system's gravity field a point is: < 1 inside, > 1 outside
double FieldRatio(const StarSystem &sys, DVec3 fromStar);
// Rock density at a star-relative point (0 outside the zone)
double ZoneDensity(const Zone &z, DVec3 p);
// Nearest point on a zone's middle circle: where you arrive when you target it
DVec3  ZoneTargetPoint(const Zone &z, DVec3 shipLocal);
