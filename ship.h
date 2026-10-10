// ship.h - flight model and drives.
// The same Ship + Controls + UpdateShip will drive NPC ships later: the
// player's keyboard fills in Controls, an AI pilot will fill in the same struct.
#pragma once
#include "galaxy.h"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Flight tuning
// ---------------------------------------------------------------------------
// Cruise is flown like an aircraft: the throttle sets a speed, the nose sets the direction.
const float  MAX_SPEED      = (float)(0.99 * LIGHT_SEC_U);   // cruise tops out at 0.99 c
const float  APPROACH_SPEED = 240.0f;       // u/s (1.2 km/s): gravity wells never slow you below this
const float  THRUST_ACCEL   = 25.0f;        // u/s^2 (125 m/s^2): the least the engines push...
const double CRUISE_SPOOL   = 2.0;          // ...and above ~60 m/s, speed grows by e^2 (~7x) per second
const float  BRAKE_ACCEL    = 35.0f;        // slowing down: at least this...
const double CRUISE_BRAKE   = 6.0;          // ...and speed drops by e^6 per second
const float  DAMPER_RATE    = 4.0f;         // dampeners: sideways drift dies off by e^4 per second...
const float  DAMPER_ACCEL   = 20.0f;        // ...plus this, so it ends at zero
const float  THROTTLE_RATE  = 0.35f;        // holding W or S moves the throttle this much per second (1 = full)
const float  THROTTLE_REVERSE = 0.3f;       // how far the throttle goes into reverse (about 1.7 km/s backward)
const double THROTTLE_MIN_SPEED = 2.0;      // u/s: sets how fine the bottom of the throttle is
const double HYPER_SPEED  = 0.7 * AU_U;     // per second
const double WARP_SPEED   = 0.35 * LY_U;    // per second
const double SPOOL_UP     = 4.0;            // while spooling up, speed grows by e^4 (~55x) per second
const double SPOOL_DOWN   = 40.0;           // coming down is 10x faster
const double WELL_K       = 1.5;            // gravity-well limit: max speed = WELL_K * distance, per second
const double WARP_WELL_K  = 1.0;            // same for warp approaching a system
const float  HYPER_ALIGN  = 4.0f;           // how fast hyper/warp velocity swings to follow the nose

// Hyper stops by itself short of a body you fly straight at
const double STOP_WORLD_RADII = 0.5;        // planets and moons: this many radii above the surface (in trading range)
const double STOP_STAR_RADII  = 1.5;        // stars: this many radii above the surface (in scooping range)
const double STOP_STATION     = 600.0;      // stations: 3 km off
const double STOP_LOCK        = 100.0;      // a locked rock, ship or belt: 500 m off
const double STOP_SPEED       = 10.0;       // closing slower than this (50 m/s) counts as arrived

// Target lock: holding Shift while a body is targeted locks the nose onto it
const float  LOCK_TURN_RATE   = 6.0f;       // how fast the nose swings onto the target (per second)
const float  LOCK_BREAK_DEG   = 6.0f;       // mouse movement (degrees of steering) that breaks the lock...
const float  LOCK_BREAK_DECAY = 3.0f;       // ...accumulated with this decay, so small jitter never adds up

// Ship stats (one ship type for now; later these come from a ship-type table)
const double FUEL_CAPACITY  = 50.0;         // tonnes
const double CARGO_CAPACITY = 100.0;        // tonnes

// Fuel burn climbs exponentially with speed between these anchors (log-log interpolation)
const double CRUISE_BURN = 0.01;            // t/s at full cruise thrust (coasting is free)
const double HYPER_BURN  = 0.05;            // t/s at full hyper speed  (~2 t to cross a system)
const double WARP_BURN   = 0.6;             // t/s at full warp speed   (~10 t for a 5 ly jump)
const double LIMP_SPEED  = 0.02 * AU_U;     // hyper on emergency power, with an empty tank

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
    float      throttle  = 0.0f;                     // cruise speed setting: 0 stopped, 1 = 0.99 c, below 0 reverse (hyper zeroes it)
    float      thrustUse = 0.0f;                     // how hard the cruise engines worked this step (1 = full)
};

// What the pilot (player or AI) wants this step
struct Controls {
    float throttle = 0.0f;  // +1 throttle up, -1 throttle down
    float yaw     = 0.0f;   // degrees this step
    float pitch   = 0.0f;   // degrees this step
    float roll    = 0.0f;   // degrees this step
    bool  hyper   = false;
    bool  fire    = false;  // mining laser (later: weapons)
    bool  allStop = false;          // pressed (one step only)
    bool  toggleDampeners = false;  // pressed (one step only)
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

inline Vector3 ShipForward(const Ship &s) { return Vector3RotateByQuaternion({ 0, 0, -1 }, s.rot); }
inline Vector3 ShipRight  (const Ship &s) { return Vector3RotateByQuaternion({ 1, 0,  0 }, s.rot); }
inline Vector3 ShipUp     (const Ship &s) { return Vector3RotateByQuaternion({ 0, 1,  0 }, s.rot); }

// Vector from the ship to a point given relative to system j's star
inline DVec3 FromShip(const Galaxy &sys, const Ship &s, SysId j, DVec3 local = {})
{
    return sys.Between(s.anchor, j) + local - s.pos;
}

void      Rebase(Ship &s, const Galaxy &sys);                        // re-anchor on the nearest star
DriveInfo ComputeDrive(const Ship &ship, const Galaxy &sys, const LoadedSystem &loaded,
                       const DVec3 *lockRel, double lockRadius, const std::string &lockName, bool haveFuel);
void      UpdateShip(Ship &s, const Controls &in, float dt, const DriveInfo &drive);
double    DriveBurnRate(double speed);                                // hyper/warp fuel burn at a speed (t/s)
double    ThrottleSpeed(float throttle);                              // the speed a throttle setting asks for (u/s)
double    StopDistance(const Body &b);                                // from its center: where hyper stops short of it
// The ship's attitude against a plane with normal 'up' (a system's ecliptic, the galactic disk): pitch is the
// nose above (+) or below the plane, roll is the right wing down (+) or up, both in degrees
void      PlaneAttitude(const Ship &s, DVec3 up, float &pitch, float &roll);
