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
#include "contract.h"
#include "econ.h"
#include "goods.h"
#include "npc.h"
#include "ship.h"
#include "shipparts.h"
#include "targets.h"
#include <string>
#include <unordered_map>
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
    ShipBuild  build = StarterBuild();       // saved: the hull and what's fitted in it (shipparts.h)
    ShipSpec   spec = SpecOf(build);         // ...and what that adds up to (shipyard.h's Refit keeps it so)
    PowerState power;                        // its capacitors and power mode

    double CargoTotal() const { double t = 0; for (double c : cargo) t += c; return t; }
    double Mass() const { return spec.dryMass + fuel + CargoTotal(); }   // tonnes, all up
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
// What the view reports back each frame: the thing under the reticle. It also
// carries the destination the pilot has locked ('course': set by locking onto
// what's under the reticle, or on a map), and while 'steer' is on the nose swings
// onto it. ('retarget' is no longer used.)
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
    std::unordered_map<uint64_t, PriceRecord> prices;   // markets (by Body::id) as last seen: docked at, reported, or bought data on
};

struct FlightState {
    TargetId  selected;                  // what's under the reticle, or the lock target
    TargetId  lock;                      // the destination locked: under drive, the nose is held on it and the ship stops there
    bool      lockSuppressed = false;    // lock was broken; don't re-lock until the reticle leaves it
    Vector2   lockBreak = { 0, 0 };      // accumulated mouse steering while locked (degrees)
    float     retargetCooldown = 0.0f;   // seconds before the lock may switch again

    DriveInfo drive;
    double    burn = 0.0;                // fuel burn this step (t/s)
    double    load = 0.0;                // megawatts the reactor is putting out
    bool      driveCharging = false;     // the drive was asked for, but its capacitor hasn't the charge to engage
    bool      driveTooHeavy = false;     // ...or the ship is too heavy for it
    bool      driveStarved = false;      // the reactor can't hold the field at the speed set
    bool      driveNone = false;         // no hyper drive fitted
    double    drivePower = 0.0;          // megawatts the FTL field is taking
    bool      scooping = false;
    bool      skimming = false;          // in a gas giant's upper atmosphere, collecting helium-3
    bool      skimFull = false;          // ...but the hold is full
    int       nearBody = -1;             // planet/moon/station within interaction range (index in loaded.bodies)
    bool      miningActive = false;
    uint64_t  miningRock = 0;
    std::string miningStatus;
    Controls  last;                      // controls of the latest step (for the engine flame etc.)
};

// Comms: any number of channels open at once, each to a world or station of the
// loaded system, each with messages on their way out and replies on their way back.
struct CommsMsg {
    double at;                           // when it arrives (game clock): ours, there; theirs, here
    bool   ours;
    std::string text;
    double sent = 0.0;                   // replies: when the request they answer went out
    bool   told = false;                 // replies: its arrival has been announced
    bool   prices = false;               // replies: a market report (its arrival updates the captain's price records)
    bool   board = false;                // replies: a station's contracts (its arrival updates the captain's record of the board)
};
struct CommsChannel {
    TargetId with;                       // a Body
    std::string withName;                // its name (still known after its system unloads)
    std::vector<CommsMsg> log;
};
struct CommsState {
    std::vector<CommsChannel> channels;
    const CommsChannel *Find(TargetId with) const
    {
        for (const CommsChannel &ch : channels) if (ch.with == with) return &ch;
        return nullptr;
    }
    CommsChannel *Find(TargetId with) { return const_cast<CommsChannel *>(static_cast<const CommsState *>(this)->Find(with)); }
};

// Another captain's ship, as this machine sees it. On the server these are
// copied from the other Captains every step; on a client they come from the
// server's snapshots (smoothed between them).
struct Traffic {
    int         id = -1;
    std::string name;
    std::string shipName;                // (may be empty)
    Ship        ship;
    bool        thrusting = false;       // engine flame
    bool        hyper = false;
    bool        mining = false;          // mining beam...
    uint64_t    miningRock = 0;          // ...on this rock

    // NPC ships (id >= NPC_ID_BASE)
    bool        npc = false;
    NpcKind     kind = NpcKind::Freighter;
    int         faction = NO_FACTION;
    Stance      stance = STANCE_NEUTRAL; // towards whoever is looking
    std::string from, dest;              // where its cargo came from and is going (may be empty)
    int         good = -1;               // the bulk of its cargo...
    float       tonnes = 0.0f;           // ...and how much it carries in all
    uint8_t     hull = 1;                // which hull it is (shipparts.h)

    // How it stands on the scope of whoever is looking (sensors.h)
    uint8_t     level = 3;               // a ContactLevel: how much is known of it
    uint8_t     by = 0;                  // which sensors hold it (BY_ bits)
    bool        ghost = false;           // lost: this is where it was last seen...
    float       age = 0.0f;              // ...this many seconds ago
};

// ---------------------------------------------------------------------------
// Sensors (sensors.h): what a captain's scope holds
// ---------------------------------------------------------------------------
enum ContactLevel : uint8_t { CONTACT_NONE, CONTACT_DETECTED, CONTACT_CLASSIFIED, CONTACT_IDENTIFIED };

struct Contact {
    int      id = -1;
    uint8_t  level = 0;                  // how much is known of it; it never goes down while the contact lasts
    uint8_t  by = 0;                     // which sensors hold it now (BY_ bits); 0: it's lost
    double   seen = 0.0;                 // when it was last held (game clock)
    Traffic  snap;                       // as it was last shown
    bool     Held() const { return by != 0; }
};

// A direction to something that can't be placed: the tachyons of a warp exit, or a radar sweeping the ship.
// Or a place someone else has worked out and passed on (a network's fix).
enum MarkKind : uint8_t { MARK_TACHYON, MARK_RADAR, MARK_FIX };
struct SensorMark {
    uint8_t  kind = MARK_TACHYON;
    int      source = -1;                // radar: whose it is (so its bearing is kept up to date; it's never shown)
    SysId    anchor = NO_SYSTEM;         // 'from' is relative to this star
    DVec3    from;                       // where the bearing was taken (a fix: the place itself)
    DVec3    dir;                        // unit vector along it
    float    sigma = 0.0f;               // degrees it may be off by
    double   rLo = 0.0, rHi = 0.0;       // units along it the source could be (a fix: how far off it may be, in rLo)
    int      counts = 0;                 // tachyon decays counted
    double   at = 0.0;                   // when (game clock)
    std::string note;                    // who passed it on, if someone did
};

// ---------------------------------------------------------------------------
// The shared world, one captain, and the two together
// ---------------------------------------------------------------------------
struct World {
    unsigned                seed = DEFAULT_GALAXY_SEED;
    Galaxy                  systems;     // generated (on demand, around whoever is looking)
    WorldDeltas             deltas;      // saved
    Economy                 econ;        // saved: markets' stocks, and what the factions are doing to them (econ.h)
    std::vector<EconNews>   news;        // saved: the last few dozen headlines, oldest first
    uint32_t                newsSeq = 0;
    std::vector<Contract>   contracts;   // saved: the contracts on offer, on every board that's been drawn up (contracts.h)
    uint32_t                contractSeq = 0;
    std::unordered_map<uint64_t, double> boardMade;   // saved: when each board (a market's, a faction's) was last drawn up
    double                  contractsChecked = 0.0;
    double                  clock = 0.0; // saved: game seconds since the start
    std::vector<Traffic>    traffic;     // every other ship in sight: captains' and NPCs' (never your own)
    std::vector<Npc>        npcs;        // the ships flown around the captains (npcs.h); not saved
    std::vector<Transit>    transits;    // lane freighters under way between systems near them
    int                     npcSeq = 0;
    uint64_t                npcDice = 1;
    double                  trafficZoned = -1e9, trafficNews = -1e9;
    std::vector<SysId>      liveSystems; // where ships are flown just now
    // Warp bubbles that have just collapsed (a ship dropped out of warp): tachyons others may pick up (npcs.cpp)
    struct Burst { SysId sys; DVec3 pos; double mj; int source; };
    std::vector<Burst>      bursts;
    // What a system's sensor network made of a burst, on its way to the people who act on it
    struct Alert { SysId sys; double due; bool fixed; bool bearing; DVec3 pos; double err; DVec3 from, dir; float sigma; double rLo, rHi; int counts; int source; };
    std::vector<Alert>      alerts;
    // The traffic picture each flown system's ports and patrols hold between them: how well each ship is known (by id)
    std::unordered_map<SysId, std::unordered_map<int, uint8_t>> picture;
    double                  pictureAt = -1e9;
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
    std::string   shipName;              // saved: what the captain calls the ship
    Player        player;                // saved
    FlightState   flight;
    CommsState    comms;
    Knowledge     know;                  // saved
    std::vector<float> standing;         // saved: with each faction (by id), -100 .. +100; missing = 0
    float Standing(int faction) const { return faction >= 0 && faction < (int)standing.size() ? standing[faction] : 0.0f; }
    std::vector<Contract> jobs;          // saved: the contracts this captain has taken
    std::vector<Contract> offers;        // saved: contracts seen on boards, as of when each board was read...
    std::unordered_map<uint64_t, double> boards;   // ...which was then (by BoardKey)
    std::vector<uint8_t> papers;         // saved: guild memberships and licences, by faction (Papers bits)
    uint8_t Papers(int faction) const { return faction >= 0 && faction < (int)papers.size() ? papers[faction] : 0; }
    void    SetPaper(int faction, uint8_t bits) { if (faction < 0) return; if ((int)papers.size() <= faction) papers.resize(faction + 1, 0); papers[faction] |= bits; }
    std::vector<uint64_t> boardsToSend;  // (the server: boards read since it last told the client)
    uint64_t      dockedAt = 0;          // the body last seen in docking range (contracts.cpp: customs, reminders)
    double        pirateSafeUntil = 0.0; // pirates have had their share for now
    double        scanSafeUntil = 0.0;   // a patrol has just been through the hold
    bool          tankerCalled = false;  // (a tanker has been asked for: sent out on the next traffic step)
    int           demandFrom = -1;       // the pirate (an NPC id) that has named its price...
    double        demandTonnes = 0.0;    // ...this many tonnes
    std::vector<std::pair<double, std::string>> shipReplies;   // what other ships have said, by when it gets here
    std::vector<Contact>    contacts;    // the ships on this captain's scope (sensors.h)
    std::vector<SensorMark> marks;       // ...and the bearings: tachyon bursts, radars sweeping the ship
    bool          sensing = false;       // (the contacts are kept here: on a client they aren't, the server sends what they show)
    std::string   trafficControl;        // the port whose traffic control is sharing its picture, if one is in reach
    bool          marksChanged = false;  // (the server: tell the client)
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
// A galaxy seed from what the player typed: a number as it is, any other text by its hash, nothing at all: a random one
unsigned SeedFromText(const std::string &text, uint64_t entropy);

// One fixed step for one captain. Doesn't advance the world clock (the caller
// does that once per step, however many captains it steps).
//   authoritative = true    the whole step: flight, fuel, mining, comms
//   authoritative = false   a client predicting its own ship: flight and fuel
//                           only; mining and comms results come from the server
void StepCaptain(World &w, Captain &c, const Controls &in, const Aim &aim, float dt, bool authoritative = true);

void UpdateEconomy(World &w);                    // economy.cpp: runs the economy up to the clock (whoever owns the world calls it)
void UpdateContracts(World &w);                  // contracts.cpp: lapses old offers (likewise)
void StepTraffic(World &w, Captain *const *captains, int count, float dt);   // npcs.cpp: the other ships (likewise)
void ScopeTraffic(const World &w, Captain &c, std::vector<Traffic> &list);   // sensors.cpp: every ship there is -> what this captain's scope shows

// Single player: advance the clock and the economy, step your captain
inline void StepGame(GameState &g, const Controls &in, const Aim &aim, float dt)
{
    g.clock += dt;
    UpdateEconomy(g);
    UpdateContracts(g);
    Captain *me = &g;
    StepTraffic(g, &me, 1, dt);
    ScopeTraffic(g, g, g.traffic);                 // (from here on, the other ships are the ones this captain's sensors hold)
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
