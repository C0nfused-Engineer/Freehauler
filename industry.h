// industry.h - what a world's people do for a living.
//
// A world (with the stations around it) is one economy: one population, one
// stock of goods, one market. What it makes comes from its INDUSTRIES. Which
// industries it can have depends on the place: open farming needs a sky you can
// breathe under and a climate to match; agridomes go anywhere there's ground,
// at a price; mines are as good as the rock; a skimmer needs a giant to skim;
// research labs do best where things are strangest. A site quality of zero
// means the industry can't be built there at all.
//
// An industry has a capacity. To run it needs
//   labour        a world turns out 0.9 units a day for each of its people.
//                 Small works are efficient and big ones less so: the labour an
//                 industry takes grows faster than what it makes (as its
//                 capacity to the power 2.2), and a poor site takes more.
//   plasma cells  power, made on the spot by whatever the world has to make it
//                 with, or shipped in
//   materials     what its products are made of (goods.h), and for some
//                 industries scientific data or fuel besides
// Short of any of them it runs below capacity. Who goes short first is the
// owner's choice: power, housing and food always come first, then whatever the
// owner cares most about (a warlike state its arms plants, a company its line
// of business), then whatever pays best.
//
// Cities are an industry too: they take labour and plasma cells and make
// housing.
//
// Capacity can be added, for materials and time, by the owner when the product
// is selling well above its usual price; and by a captain who puts up the
// money, for a share of what that industry earns from then on.
#pragma once
#include "galaxy.h"
#include "goods.h"
#include <vector>

enum IndustryType : uint8_t {
    // The land and what's on it
    IND_FARM,           // open farming: grain, livestock, a little flora
    IND_AGRIDOME,       // farming under domes: grain, anywhere there's ground
    IND_HYDROPONICS,    // ...and in tanks, on stations
    IND_MINE,           // automated subsurface mines: ores, by the geology
    IND_ICEWORKS,       // ice and volatiles, from cold worlds
    IND_SKIMMER,        // helium-3 from a gas giant
    IND_XENO,           // xenobiology outposts: exotic fauna and flora, from worlds with life of their own
    // Making things
    IND_SMELTER,        // metals, silicon, ceramics
    IND_CHEMICAL,       // polymers, chemicals, fertiliser, water, deuterium, reactor fuel
    IND_FOOD,           // foodstuffs and liquor
    IND_LIGHT,          // consumer and luxury goods
    IND_PHARMA,         // medicines (uses scientific data)
    IND_HEAVY,          // machinery, hull plating, habitat modules
    IND_MUNITIONS,      // munitions
    IND_ARMS,           // weapons (uses scientific data)
    IND_FOUNDRY,        // semiconductor foundry: electronics (uses scientific data)
    IND_ROBOTICS,       // robotics (uses scientific data)
    IND_REACTOR,        // reactor works: reactor cores (uses scientific data)
    IND_PRECISION,      // instruments and ship components (uses scientific data)
    IND_LAB,            // research labs: scientific data, from labour and power alone
    IND_SHIPYARD,       // an orbital yard: plating, components, reactor cores and machinery go in, ships come out (fleet.h)
    // Power: plasma cells
    IND_SOLAR, IND_GEOTHERMAL, IND_FISSION, IND_FUSION,
    // Housing
    IND_CITY,
    // Everything people do for each other and not for sale: shops, schools, clinics, offices. It makes nothing to
    // ship, takes labour and power in proportion to the people there are, and grows and shrinks with them.
    IND_SERVICES,
    IND_COUNT
};

const int    MAX_PRODUCTS  = 8;
const double LABOUR_SHARE  = 0.9;       // labour units a day for each of a world's people
const double LABOUR_POWER  = 1.0 / 0.45;// labour grows as capacity to this power (so output grows as labour to the 0.45)
const double CITY_LABOUR   = 0.08;      // the share of a world's people it takes to run cities that house them all
const double CITY_CELLS    = 0.25;      // plasma cells a day for each unit of city, under an open sky (more elsewhere)
const double SERVICES_LABOUR = 0.28;    // the share of a world's people its services employ, fully staffed...
const double SERVICES_CELLS  = 0.15;    // ...and the plasma cells a day for each unit of them
const double HOUSING_SPARE = 1.05;      // a new world's cities house this many times its people
const double EMPLOYED_AT_START = 0.90;  // the share of a world's labour its industries are sized to use to begin with
const double UPGRADE_STEP  = 0.10;      // an upgrade adds this share of an industry's capacity (at least UPGRADE_MIN)...
const double UPGRADE_MIN   = 1.0;
const double UPGRADE_DAYS  = 2.0;       // ...and takes this many market days to build
const double UPGRADE_PRICE = 1.25;      // an owner builds when the product fetches this many times its usual price...
const double FOUND_PRICE   = 1.6;       // ...and starts a new industry at this
const double UPGRADE_STEEL = 0.4, UPGRADE_MACHINERY = 0.15, UPGRADE_MODULES = 0.25;   // tonnes for each unit added (modules: domes and tanks)
const double CITY_STEP     = 0.02;      // a city grows by this share of its capacity at a time (about 4.5% more people housed)...
const double CITY_STEEL = 0.001, CITY_MACHINERY = 0.0003, CITY_MODULES = 0.002;      // ...for this many tonnes a person housed (more off an open world)
const double INVEST_DAYS   = 25.0;      // a captain funding an addition pays this many days of what it should earn at usual prices...
const double INVEST_FEE    = 150.0;     // ...(at least this much a unit), on top of the materials
// What a tonne of ship takes: hull plating, ship components, reactor cores, machinery
const double YARD_PLATING = 0.55, YARD_COMPONENTS = 0.25, YARD_CORES = 0.05, YARD_MACHINERY = 0.15;
const double INDUSTRY_COST = 0.85;      // what an industry's output costs it to make, as a share of its usual price: the rest is profit

struct IndustryInfo {
    const char *name;
    float       labour;                 // how much labour it takes, against the others
    float       cells;                  // plasma cells for each unit of capacity running, a day
    Ingredient  extra[2];               // what it uses besides its products' own recipes, per tonne made
    const char *site;                   // what makes a good place for it, in a few words
};
const IndustryInfo &IndustryData(int type);
inline bool IsPower(int type) { return type >= IND_SOLAR && type <= IND_FUSION; }
inline bool IsCivic(int type) { return type == IND_CITY || type == IND_SERVICES; }     // sized by the people, not by what sells
double CivicCells(int type, double cap, double site);         // the plasma cells a city or its services take a day, flat out

// What an industry turns out: each product's share of its output
struct Product { uint8_t good = 0; float share = 0.0f; };

// The place, as far as industry cares
struct SiteInfo {
    float  res[RES_COUNT] = {};
    float  flux = 0.0f;                 // starlight (1 = Earth's)
    float  tempK = 0.0f, gravity = 0.0f;
    bool   openAir = false;             // people live under the sky here
    bool   surface = false;             // there's ground to build on (not a station)
    bool   giant = false;               // it orbits a gas or ice giant (or is a station that does)
    bool   belt = false;                // a station in an asteroid belt
    bool   yard = false;                // an orbital shipyard, or a starbase with slips
    bool   strangeStar = false;         // its star is no ordinary one: a giant, a white dwarf, a neutron star, a brown dwarf
    int8_t life = 0;                    // 0 none .. 3 complex
};
// How good a place it is for an industry: 0 it can't be done here, 1 ordinary, more for the best sites
double SiteQuality(int type, const SiteInfo &s);
// What that industry makes here (mines: by the geology). Returns how many products.
int    IndustryProducts(int type, const SiteInfo &s, Product out[MAX_PRODUCTS]);

// One industry of one economy, as the seed made it ('cap': 0 if it could be built here but hasn't been)
struct IndustryDef {
    uint8_t type = IND_FARM;
    int16_t owner = NO_FACTION;         // whose it is (a station's works belong to whoever runs the station)
    float   site = 1.0f;
    float   cap = 0.0f;
    Product out[MAX_PRODUCTS];
    uint8_t outs = 0;
};

// The labour an industry of this capacity takes a day, flat out
double IndustryLabour(int type, double cap, double site);
// How many people a city of this capacity houses
double CityHousing(double cap);
double CityFor(double people);          // ...and the capacity that houses this many

// The industries a settlement of this kind starts with, as shares of its scale (0-terminated by type IND_COUNT)
struct IndustrySeed { uint8_t type; float share; };
const IndustrySeed *SettlementIndustries(Settlement s);
