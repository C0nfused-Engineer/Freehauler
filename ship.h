// ship.h - flight model, drives and power.
//
// Every ship, a captain's or an NPC's, flies by FlyShip: the pilot (keyboard or
// autopilot) fills in Controls, and what the ship then does depends on what
// it's made of (ShipSpec: what its parts add up to, see shipparts.h) and what
// it weighs.
//
//   Sublight   the throttle sets a speed. The engines take the ship to it and
//              then shut down: holding a speed costs nothing, and they only fire
//              again to change speed or heading, or to stop. How fast they get
//              there is thrust over mass, falling away toward light speed
//              (a = F / (gamma^3 m)): 0.99 c is the limit. Dampeners kill
//              sideways drift, so the ship goes where it points.
//   Turning    manoeuvring thrusters over mass: a loaded ship swings slowly.
//   Hyper      a continuously powered field, good inside a system. The power to
//              hold it is P0 + k m (v/c)^n: more for more mass, much more for
//              more speed. While it's engaged the throttle sets its speed.
//   Warp       between the stars. Going to warp takes a burst no reactor can
//              give: the drive capacitor is discharged to form the bubble (more
//              for more mass; it holds a jump or two). After that the reactor
//              holds the field, by the same kind of law with its own numbers.
//              Without the charge the drive stays at hyper speed while the
//              capacitor fills. When the bubble collapses it gives off a burst
//              of tachyons that others may detect.
//   Power      the reactor burns fuel for exactly what's drawn
//              (fuel rate = P / (efficiency x the fuel's specific energy)).
//              FTL drives speed up at a fixed rate; what a shortage of power
//              costs them is top speed. The capacitor bank covers engines and
//              the mining laser (weapons and shields to come) when the reactor
//              is short. The power mode says where spare output goes.
//   Emissions  everything the reactor makes that doesn't come out as power comes out
//              as heat, and so does the engines' exhaust: that is what an infrared
//              array sees (sensors.h). Running silent throttles the reactor right
//              down and shuts the radar off: cold and quiet, and slow.
#pragma once
#include "galaxy.h"
#include "goods.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Flight tuning
// ---------------------------------------------------------------------------
const float  MAX_SPEED      = (float)(0.99 * LIGHT_SEC_U);   // sublight tops out at 0.99 c
const float  APPROACH_SPEED = 240.0f;       // u/s (1.2 km/s): gravity wells never slow you below this
const double THRUST_K       = 1.0e6;        // u/s^2 per unit of thrust per tonne (engines are fierce: 0.99 c in seconds, not weeks)
const double THROTTLE_MIN_SPEED = 2.0;      // u/s: sets how fine the bottom of the throttle is
const double SETTLE_RATE    = 3.0;          // closing on the speed the throttle asks for, the engines ease off: the gap shrinks by e^3 a second...
const double SETTLE_ACCEL   = 25.0;         // ...but by no less than this (u/s^2), so it ends there
const double THROTTLE_RANGE = 1.0e6;        // full throttle is this many times the thrust of the first touch of it
const float  DAMPER_RATE    = 4.0f;         // dampeners: sideways drift dies off by e^4 per second...
const float  DAMPER_ACCEL   = 20.0f;        // ...plus this, so it ends at zero
const float  THROTTLE_RATE  = 0.35f;        // holding W or S moves the throttle this much per second (1 = full)
const float  THROTTLE_REVERSE = 0.3f;       // how far the throttle goes into reverse (about 1.7 km/s backward)
const float  FTL_MIN        = 0.05f;        // the slowest an FTL drive can be set to run, as a share of its top speed
const double HYPER_SPEED  = 0.7 * AU_U;     // per second
const double WARP_SPEED   = 0.35 * LY_U;    // per second
const double SPOOL_DOWN   = 40.0;           // coming down from FTL: speed drops by e^40 per second
const double WELL_K       = 1.5;            // gravity-well limit: max speed = WELL_K * distance, per second
const double WARP_WELL_K  = 1.0;            // same for warp approaching a system
const float  HYPER_ALIGN  = 4.0f;           // how fast hyper/warp velocity swings to follow the nose

// Hyper stops by itself short of a body you fly straight at
const double STOP_WORLD_RADII = 0.5;        // planets and moons: this many radii above the surface (in trading range)
const double STOP_STAR_RADII  = 1.5;        // stars: this many radii above the surface (in scooping range)
const double STOP_STATION     = 600.0;      // stations: 3 km off
const double STOP_LOCK        = 100.0;      // a locked rock, ship or belt: 500 m off
const double STOP_SPEED       = 10.0;       // closing slower than this (50 m/s) counts as arrived

// Target lock: while a destination is locked, holding Shift swings the nose onto it
const float  LOCK_TURN_RATE   = 6.0f;       // how fast the nose swings onto the target (per second)
const float  LOCK_BREAK_DEG   = 6.0f;       // mouse movement (degrees of steering) that breaks the lock...
const float  LOCK_BREAK_DECAY = 3.0f;       // ...accumulated with this decay, so small jitter never adds up

// The starter ship's tank and hold (every captain flies one for now)
const double FUEL_CAPACITY  = 50.0;         // tonnes
const double CARGO_CAPACITY = 100.0;        // tonnes

const double LIMP_SPEED  = 0.02 * AU_U;     // hyper on emergency power, with an empty tank
const double LIMP_THRUST = 0.15;            // ...and what's left of the main engines

// Running silent: the reactor is held to a fraction of its output, the housekeeping is cut back, nothing charges,
// and what heat is left is bled off slowly through shuttered radiators
const double SILENT_REACTOR = 0.20, SILENT_IDLE = 0.5, SILENT_HEAT = 0.25;
const double PLUME_HEAT   = 3.0;            // megawatts of heat seen in the engines' exhaust for each megawatt they draw
const double FIELD_RELAX  = 15.0;           // seconds: a warp field run slower sheds the energy it was carrying this slowly
const double FIELD_FLOOR  = 0.2;            // the share of a full bubble's energy that the slowest field still holds

// ---------------------------------------------------------------------------
// What a ship is made of, added up (SpecOf in shipparts.h does the adding).
// The defaults are the starter ship's. Mass in tonnes, power in megawatts,
// energy in megajoules, speeds in units a second.
// ---------------------------------------------------------------------------
struct ShipSpec {
    double dryMass    = 77.1;        // hull and everything fitted (fuel and cargo come on top)
    double thrust     = 2625.0;      // main engines: acceleration at full throttle is THRUST_K times this over the ship's mass
    double turn       = 27500.0;     // manoeuvring thrusters: degrees a second is this over the ship's mass

    double reactorMW  = 200.0;       // the most the reactor puts out
    double reactorEff = 0.40;        // how much of its fuel's energy comes out as power
    double fuelMJ     = 2500.0;      // the fuel's specific energy, megajoules a tonne...
    int    fuelGood   = REF_REACTOR_FUEL;   // ...and which good it is (the starter's reactor is a fission one)
    double idleMW     = 3.0;         // life support, computers, sensors: always on
    double engineMW   = 10.0;        // main engines at full thrust
    double bankMJ     = 600.0;       // capacitor bank

    double hyperSpeed = HYPER_SPEED; // the hyper drive's top speed (0: none fitted)
    double hyperP0    = 5.0, hyperK = 6.6e-9, hyperN = 3.0;      // its power: P0 + K m (v/c)^N
    double hyperAccel = 120.0;       // how fast it speeds up, c per second
    double hyperMaxMass = 450.0;     // heavier than this and it won't engage
    double warpSpeed  = WARP_SPEED;  // likewise for the warp drive
    double warpP0     = 15.0, warpK = 6.9e-15, warpN = 2.0;
    double warpAccel  = 3.0e6;
    double warpMaxMass = 450.0;
    double jumpMJ     = 25.0;        // energy to form the warp bubble, per tonne of ship
    double driveCapMJ = 8000.0;      // drive capacitor...
    double capIn      = 0.90, capOut = 0.90;   // ...and how much survives going in, and coming out

    double tank       = 50.0;        // tonnes of fuel
    double hold       = 100.0;       // tonnes of cargo
    double laserRate  = 2.0;         // mining laser: tonnes a second (0: none fitted)...
    double laserRange = 300.0;       // ...units from the rock's surface...
    double laserMW    = 8.0;         // ...and what it draws
    double scoopRate  = 0.0;         // fuel scoop: tonnes of deuterium a second off a star, for a ship that burns it (0: none)
    double skimRate   = 0.25;        // gas skimmer: tonnes of helium-3 a second (0: none)
    // Sensors (sensors.h). Each is how far it reaches as a multiple of the basic set's (0.3: what a bare hull has).
    double scan       = 1.0;         // telescope: identifies ships, and charts the worlds it passes
    double radar      = 1.0;         // radar: finds anything, and gives the ship away while it's on...
    double radarMW    = 2.0;         // ...and what it draws then
    double ir         = 1.0;         // infrared array: sees heat, and says nothing
    double tachArea   = 4.0, tachDepth = 2.0;   // tachyon detector: the face it shows a burst (m^2) and its depth (m)...
    double tachEff    = 0.5, tachRes = 12.0;    // ...the share of decays in it that it catches, and how well one count gives a direction (degrees)
    double hullSize   = 1.0;         // how big a target the hull is: its volume against a hauler's
};

// Where the reactor's spare output goes (and, when weapons and shields exist, who goes short first)
enum PowerMode : uint8_t { POWER_TRAVEL, POWER_COMBAT, POWER_DEFENCE, POWER_SILENT, POWER_MODES };
const char *PowerModeName(uint8_t mode);

struct PowerState {
    double  bank = 600.0, driveCap = 8000.0;   // megajoules in each capacitor
    uint8_t mode = POWER_TRAVEL;
    bool    engaged = false;         // the drive was running last step
    bool    warping = false;         // ...and is at warp (the capacitor's charge for the jump is spent)
    float   engines = 1.0f;          // the share of the power they asked for that the engines got last step
    bool    radar = true;            // the radar is switched on (running silent overrides it: see RadarOn)
    float   field = 0.0f;            // at warp: the energy in the bubble, as a share of a full-speed one's (it comes out as tachyons when it goes)
    float   heat = 4.5f;             // megawatts of heat the ship is giving off (what infrared sees)
};

// ---------------------------------------------------------------------------
// Ship
// ---------------------------------------------------------------------------
// Position is stored relative to an "anchor" star (the nearest one, wherever
// there's a star within a few light years), so it stays precise to well under
// a meter anywhere near a system.
// Local axes: forward = -Z, up = +Y, right = +X
struct Ship {
    SysId      anchor    = NO_SYSTEM;
    DVec3      pos;                                  // relative to the anchor star
    Vector3    vel       = { 0.0f, 0.0f, 0.0f };     // units per second
    Quaternion rot       = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool       dampeners = true;
    bool       allStop   = false;
    bool       dropout   = false;                    // just out of FTL: the collapsing field is shedding the ship's speed
    float      throttle  = 0.0f;                     // sublight speed setting: 0 stopped, 1 = 0.99 c, below 0 reverse (hyper zeroes it)
    float      ftl       = 1.0f;                     // how fast the FTL drive is set to run, as a share of its top speed
    float      thrustUse = 0.0f;                     // how hard the engines worked this step (1 = full)
};

// What the pilot (player or AI) wants this step
struct Controls {
    float throttle = 0.0f;  // +1 throttle up, -1 throttle down (the FTL drive's speed setting, while it's engaged)
    float yaw     = 0.0f;   // degrees this step
    float pitch   = 0.0f;   // degrees this step
    float roll    = 0.0f;   // degrees this step
    bool  hyper   = false;
    bool  fire    = false;  // mining laser (later: weapons)
    bool  allStop = false;          // pressed (one step only)
    bool  toggleDampeners = false;  // pressed (one step only)
    bool  cyclePower = false;       // pressed (one step only): the next power mode
    bool  toggleRadar = false;      // pressed (one step only): radar on / off
};

// What the drive is allowed to do this step
struct DriveInfo {
    bool        inField   = false;   // inside the loaded system's gravity field
    double      limit     = 0.0;     // top speed for hyper/warp right now
    std::string limiter;             // body/system that's slowing us, if any
    bool        holding   = false;   // hyper has stopped short of 'limiter'
    double      cruiseLimit = MAX_SPEED;   // top speed for cruise right now (bodies ahead slow it)
    std::string cruiseLimiter;
    double      fieldRatio = 0.0;
    double      exitClimb  = 0.0;    // distance to leave the field by climbing out of the plane
    double      exitEdge   = 0.0;    // distance to leave it by flying outward in the plane
    bool        noFuel     = false;  // empty tank: warp offline, hyper limping
};

// What the ship can do right now (from its spec, its mass and its power)
struct FlightStats {
    double accel = 2.1e7;            // sublight acceleration at full throttle and low speed, u/s^2
    double turn  = 1e9;              // degrees a second the nose can swing
    double ftlAccel = 120.0 * LIGHT_SEC_U;   // how fast the FTL drive speeds up, u/s^2
};

// Something the nose is locked onto: the drive stops short of it (matching its speed, if it's moving)
struct LockInfo { DVec3 rel; double radius = 0.0; double speed = 0.0; std::string name; };

// What a step of flight came to
struct FlightReport {
    DriveInfo drive;
    double burn = 0.0;               // fuel, t/s
    double load = 0.0;               // megawatts drawn from the reactor (out of spec.reactorMW)
    bool   charging = false;         // clear to warp, but the capacitor hasn't the charge for the jump yet: held at hyper speed
    bool   tooHeavy = false;         // ...or the ship is more than it can move
    bool   starved = false;          // the reactor can't hold the field at the speed asked for: slower than that
    bool   noDrive = false;          // no hyper drive fitted
    double drivePower = 0.0;         // megawatts the FTL field is taking
    double collapse = 0.0;           // a warp bubble collapsed this step: the energy that was in it (megajoules)
    double heat = 0.0;               // megawatts of heat given off
};

inline Vector3 ShipForward(const Ship &s) { return Vector3RotateByQuaternion({ 0, 0, -1 }, s.rot); }
inline Vector3 ShipRight  (const Ship &s) { return Vector3RotateByQuaternion({ 1, 0,  0 }, s.rot); }
inline Vector3 ShipUp     (const Ship &s) { return Vector3RotateByQuaternion({ 0, 1,  0 }, s.rot); }

// Vector from the ship to a point given relative to system j's star
inline DVec3 FromShip(const Galaxy &sys, const Ship &s, SysId j, DVec3 local = {})
{
    return sys.Between(s.anchor, j) + local - s.pos;
}

void      Rebase(Ship &s, const Galaxy &sys);                        // re-anchor on the nearest star
DriveInfo ComputeDrive(const Ship &ship, const Galaxy &sys, const LoadedSystem &loaded, const LockInfo *lock, bool haveFuel,
                       double hyperTop = HYPER_SPEED, double warpTop = WARP_SPEED);
void      UpdateShip(Ship &s, const Controls &in, float dt, const DriveInfo &drive, const FlightStats &stats = {});
// The power an FTL field takes at a speed: P0 + K m (v/c)^N, by the hyper drive's numbers or ('warp') the warp drive's
double    DrivePower(const ShipSpec &spec, double speed, double mass, bool warp);
double    DriveSpeedFor(const ShipSpec &spec, double megawatts, double mass, bool warp);   // ...and the speed that much power holds
// Is the radar transmitting? (Switched on, and not running silent.)
inline bool RadarOn(const ShipSpec &spec, const PowerState &power) { return power.radar && power.mode != POWER_SILENT && spec.radar > 0.0; }
// The energy in a warp bubble (megajoules): what its collapse lets go. 'field': how hard it was being run (PowerState::field).
inline double BubbleEnergy(const ShipSpec &spec, double mass, double field) { return spec.jumpMJ * mass * (FIELD_FLOOR + (1.0 - FIELD_FLOOR) * field); }
// What the drive capacitor must hold for a jump to warp (what the bubble takes, plus what's lost getting it out)
inline double JumpCharge(const ShipSpec &spec, double mass) { return spec.jumpMJ * mass / spec.capOut; }
// The speed a throttle setting asks for (u/s). Each notch multiplies it, so most of the throttle's travel is speeds
// for manoeuvring and the top of it is 0.99 c.
double    ThrottleSpeed(float throttle);
FlightStats ShipStats(const ShipSpec &spec, const PowerState &power, double mass, bool haveFuel);
// One step for any ship: what the pilot asks, within what the ship can do. Moves the ship, runs the
// reactor and the capacitors, burns the fuel. 'laser': the mining laser is cutting.
FlightReport FlyShip(Ship &ship, const ShipSpec &spec, PowerState &power, double &fuel, double mass, Controls in, float dt,
                     const Galaxy &sys, const LoadedSystem &loaded, const LockInfo *lock, bool laser = false);
double    StopDistance(const Body &b);                                // from its center: where hyper stops short of it
// The ship's attitude against a plane with normal 'up' (a system's ecliptic, the galactic disk): pitch is the
// nose above (+) or below the plane, roll is the right wing down (+) or up, both in degrees
void      PlaneAttitude(const Ship &s, DVec3 up, float &pitch, float &roll);
