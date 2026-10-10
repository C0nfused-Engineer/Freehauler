// game.h - the game state, and the simulation step.
//
// The state splits along two lines.
//
// Who it belongs to:
//   World      shared by everyone: the galaxy, what's been mined, the clock,
//              the other ships in sight (traffic)
//   Captain    one player: their ship, cargo and credits, flight and comms
//              state, and the system and rocks generated around their ship
//   GameState  a World plus the Captain this machine flies. Single player and
//              the multiplayer client use this; the dedicated server keeps one
//              World and a Captain per connected player.
//
// How it's kept:
//   generated  systems, loaded, field          rebuilt from the seed, never saved
//   deltas     deltas                          what players changed in the world (saved)
//   player     player, clock                   a captain's own state (saved)
// plus transient flight/comms state that's fine to lose on load.
//
// The simulation only changes state through StepCaptain (fixed time step) and
// the action functions (SellGoods, Hail, ...; see commands.h). It never reads the
// keyboard or draws anything, so it runs headless for tests and on the server.
#pragma once
#include "asteroids.h"
#include "goods.h"
#include "ship.h"
#include "targets.h"
#include <string>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
const double SIM_HZ         = 120.0;        // fixed simulation rate
const double MINING_RANGE   = 300.0;        // 1.5 km from the surface
const double MINING_RATE    = 2.0;          // tonnes per second
const double SCOOP_RATE     = 1.0;          // tonnes per second while skimming a star...
const double SCOOP_RANGE    = 3.0;          // ...within this many star radii of its center
const double COMMS_RANGE    = 3.0 * LIGHT_SEC_U;   // hail inhabited worlds within this of their surface
const double INTERACT_RADII = 2.0;          // F works within this many radii of a body's center...
const double INTERACT_MIN   = 500.0 * KM_U; // ...or this far above its surface, whichever is larger
const double STATION_RANGE  = 20.0 * KM_U;  // F works within this of a station
const double SKIM_RATE      = 0.25;         // tonnes of helium-3 per second while skimming a gas giant...
const double SKIM_DEPTH     = 1.25;         // ...within this many of its radii of its center (gas and ice giants only)
const double START_CREDITS  = 200.0;
const double SCAN_RADII     = 20.0;         // passing within this many radii of a world's surface scans it...
const double SCAN_MIN       = 1.0 * LIGHT_SEC_U;   // ...or within this, whichever is farther
const double SHIP_RADIUS    = 2.5;          // units; for targeting other ships

// ---------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------
struct Player {
    Ship   ship;
    double fuel    = FUEL_CAPACITY;
    double credits = START_CREDITS;
    double cargo[GOOD_COUNT] = {};           // tonnes of each ore and good in the hold

    double CargoTotal() const { double t = 0; for (double c : cargo) t += c; return t; }
};

// Changes players made to the generated world
struct WorldDeltas {
    MinedMap mined;                          // ore left in rocks that have been mined
};

// ---------------------------------------------------------------------------
// Events: systems announce what happened instead of calling each other.
// The UI turns them into messages; later, missions and reputation listen too.
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
    Notice,             // plain message for the player
    SoldGoods, BoughtFuel, OreMined, AsteroidDepleted, BoughtGoods,
    SystemLoaded, SystemUnloaded,
    CommsOpened, CommsClosed, CommsLost,
    GameSaved, GameLoaded,
    PlayerJoined, PlayerLeft,
};

struct GameEvent {
    EventType   type;
    std::string text;           // player-facing message ("" = none)
    TargetId    where;          // world / rock / system involved
    int         item   = -1;    // ore, later commodity
    double      amount = 0.0;   // tonnes
    double      value  = 0.0;   // credits
    int         player = -1;    // captain it happened to (-1: everyone)
};

// ---------------------------------------------------------------------------
// Transient state
// ---------------------------------------------------------------------------
// What the view reports back each frame: the thing under the reticle, and a
// body the nudge cursor rested on while locked (switch the lock to it).
// It also carries the course the pilot set on a map: the selection whenever the
// reticle isn't on something else, and while 'steer' is on the nose swings onto it.
struct Aim {
    TargetId hovered;
    TargetId retarget;
    TargetId course;
    bool     steer = false;
};

// What this captain's charts hold. A system's map shows only what can be seen
// from afar (its stars, planets and belts) until someone living there has sent
// their charts, or the captain has flown close enough to a world to scan it.
struct Knowledge {
    std::unordered_set<int64_t>  charted;    // whole systems (SysId): everything in them is known
    std::unordered_set<uint64_t> scanned;    // single bodies (Body::id), scanned in passing
};

struct FlightState {
    TargetId  selected;                  // what's under the reticle, or the lock target
    TargetId  lock;                      // body the nose is locked onto in hyper
    bool      lockSuppressed = false;    // lock was broken; don't re-lock until the reticle leaves it
    Vector2   lockBreak = { 0, 0 };      // accumulated mouse steering while locked (degrees)
    float     retargetCooldown = 0.0f;   // seconds before the lock may switch again

    DriveInfo drive;
    double    burn = 0.0;                // fuel burn this step (t/s)
    bool      scooping = false;
    bool      skimming = false;          // in a gas giant's upper atmosphere, collecting helium-3
    bool      skimFull = false;          // ...but the hold is full
    int       nearBody = -1;             // planet/moon/station within interaction range (index in loaded.bodies)
    bool      miningActive = false;
    uint64_t  miningRock = 0;
    std::string miningStatus;
    Controls  last;                      // controls of the latest step (for the engine flame etc.)
};

struct CommsMsg { double at; bool ours; std::string text; };   // 'at' = when it arrives (game clock)
struct CommsState {
    TargetId with;                       // open channel (a Body), or none
    std::string withName;                // its name (still known after its system unloads)
    std::vector<CommsMsg> log;
};

// Another captain's ship, as this machine sees it. On the server these are
// copied from the other Captains every step; on a client they come from the
// server's snapshots (smoothed between them).
struct Traffic {
    int         id = -1;
    std::string name;
    Ship        ship;
    bool        thrusting = false;       // engine flame
    bool        hyper = false;
    bool        mining = false;          // mining beam...
    uint64_t    miningRock = 0;          // ...on this rock
};

// ---------------------------------------------------------------------------
// The shared world, one captain, and the two together
// ---------------------------------------------------------------------------
struct World {
    unsigned                seed = DEFAULT_GALAXY_SEED;
    Galaxy                  systems;     // generated (on demand, around whoever is looking)
    WorldDeltas             deltas;      // saved
    double                  clock = 0.0; // saved: game seconds since the start
    std::vector<Traffic>    traffic;     // other captains' ships (never includes your own)
    MinedMap                minedChanged;// rocks mined since the last time someone looked (the server syncs these)
    std::vector<GameEvent>  events;      // this frame's events (the main loop clears them)

    // An event for everyone, or for one captain
    void Emit(EventType type, const std::string &text, TargetId where = {}, int item = -1,
              double amount = 0.0, double value = 0.0, int player = -1)
    {
        events.push_back({ type, text, where, item, amount, value, player });
    }
    const Traffic *FindTraffic(int id) const
    {
        for (const Traffic &t : traffic) if (t.id == id) return &t;
        return nullptr;
    }
};

struct Captain {
    int           id = 0;                // 0 in single player; the server hands out 1, 2, ...
    std::string   name;                  // multiplayer: the name you joined with
    Player        player;                // saved
    FlightState   flight;
    CommsState    comms;
    Knowledge     know;                  // saved
    LoadedSystem  loaded;                // generated around this ship
    AsteroidField field;                 // generated around this ship
};

// The world as one machine plays it: everything shared, plus the captain you fly
struct GameState : World, Captain {
    void Emit(EventType type, const std::string &text, TargetId where = {}, int item = -1,
              double amount = 0.0, double value = 0.0)
    {
        World::Emit(type, text, where, item, amount, value, id);
    }
};

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
void NewWorld(World &w, unsigned seed = DEFAULT_GALAXY_SEED);
void NewCaptain(World &w, Captain &c, int id = 0, const std::string &name = "");   // at the start position
void NewGame(GameState &g, unsigned seed = DEFAULT_GALAXY_SEED);                   // NewWorld + NewCaptain

// One fixed step for one captain. Doesn't advance the world clock (the caller
// does that once per step, however many captains it steps).
//   authoritative = true    the whole step: flight, fuel, mining, comms
//   authoritative = false   a client predicting its own ship: flight and fuel
//                           only; mining and comms results come from the server
void StepCaptain(World &w, Captain &c, const Controls &in, const Aim &aim, float dt, bool authoritative = true);

// Single player: advance the clock, step your captain
inline void StepGame(GameState &g, const Controls &in, const Aim &aim, float dt)
{
    g.clock += dt;
    StepCaptain(g, g, in, aim, dt);
}

void UpdateLoadedSystem(World &w, Captain &c);   // re-anchor; load / unload systems by distance
inline void UpdateLoadedSystem(GameState &g) { UpdateLoadedSystem(g, g); }

// Scans the worlds the ship is passing, and takes the charts of a system whose people have
// answered a hail. Run by whoever keeps this captain's charts (the game, not the server).
void UpdateKnowledge(World &w, Captain &c);
// Is this body of the loaded system on the captain's charts in full? (Moons and stations come with their world.)
bool Known(const Captain &c, int body);

// A rock's ore changed (someone mined it): update this captain's copy, if it has one
void ApplyRockChange(Captain &c, uint64_t rock, float oreLeft);

// ---------------------------------------------------------------------------
// Queries (a captain's view of the world)
// ---------------------------------------------------------------------------
// Ship position relative to the loaded system's primary star
DVec3       ShipLocal(const World &w, const Captain &c);
// Star targets of the loaded system become its first body and back again
TargetId    CanonicalTarget(const Captain &c, TargetId t);
// Ship-to-target vector and radius; false if the target no longer exists
bool        FindTarget(const World &w, const Captain &c, TargetId t, DVec3 &rel, double *radius = nullptr);
std::string TargetName(const World &w, const Captain &c, TargetId t);
const Body *TargetBody(const Captain &c, TargetId t);        // a loaded planet/moon/star, or null

inline DVec3       ShipLocal(const GameState &g) { return ShipLocal(g, g); }
inline bool        FindTarget(const GameState &g, TargetId t, DVec3 &rel, double *radius = nullptr) { return FindTarget(g, g, t, rel, radius); }
inline std::string TargetName(const GameState &g, TargetId t) { return TargetName(g, g, t); }
