// ship.cpp - flight model and drives
#include "ship.h"

const char *PowerModeName(uint8_t mode)
{
    static const char *names[] = { "TRAVEL", "COMBAT", "DEFENCE" };
    return names[mode < POWER_MODES ? mode : 0];
}

double DrivePower(const ShipSpec &spec, double speed, double mass, bool warp)
{
    double beta = speed / LIGHT_SEC_U;
    return warp ? spec.warpP0 + spec.warpK * mass * pow(beta, spec.warpN)
                : spec.hyperP0 + spec.hyperK * mass * pow(beta, spec.hyperN);
}

double DriveSpeedFor(const ShipSpec &spec, double megawatts, double mass, bool warp)
{
    double p0 = warp ? spec.warpP0 : spec.hyperP0, k = warp ? spec.warpK : spec.hyperK, n = warp ? spec.warpN : spec.hyperN;
    if (megawatts <= p0 || k <= 0.0 || mass <= 0.0) return 0.0;
    return LIGHT_SEC_U * pow((megawatts - p0) / (k * mass), 1.0 / n);
}

double ThrottleThrust(float throttle)
{
    double t = fabs(throttle);
    double f = (pow(THROTTLE_RANGE, t) - 1.0) / (THROTTLE_RANGE - 1.0);
    return throttle < 0.0f ? -f : f;
}

FlightStats ShipStats(const ShipSpec &spec, const PowerState &power, double mass, bool haveFuel)
{
    FlightStats st;
    mass = fmax(mass, 1.0);
    st.accel = THRUST_K * spec.thrust / mass * (haveFuel ? fmax(0.1, (double)power.engines) : LIMP_THRUST);
    st.turn  = spec.turn / mass;
    st.ftlAccel = (power.warping ? spec.warpAccel : spec.hyperAccel) * LIGHT_SEC_U;
    return st;
}

FlightReport FlyShip(Ship &ship, const ShipSpec &spec, PowerState &power, double &fuel, double mass, Controls in, float dt,
                     const Galaxy &sys, const LoadedSystem &loaded, const LockInfo *lock, bool laser)
{
    FlightReport r;
    const bool reactor = fuel > 0.0, wasWarping = power.warping;
    if (in.cyclePower) power.mode = (uint8_t)((power.mode + 1) % POWER_MODES);

    r.drive = ComputeDrive(ship, sys, loaded, lock, reactor, spec.hyperSpeed, spec.warpSpeed);

    // Will the drive run? It needs to be there, and able to move this much ship. (On an empty tank it
    // limps on the emergency cell.) In a system it's the hyper field, on reactor power. Clear of the
    // system's field, forming a warp bubble takes the capacitor's charge, all at once; without it (or
    // without a warp drive) the drive holds at hyper speed. Then: no faster than the pilot has set it,
    // and no faster than the reactor can hold the field.
    const double spare = spec.reactorMW - spec.idleMW;
    if (in.hyper && spec.hyperSpeed <= 0.0) { in.hyper = false; r.noDrive = true; }
    if (in.hyper && reactor) {
        if (mass > spec.hyperMaxMass) { in.hyper = false; r.tooHeavy = true; }
        else {
            if (r.drive.inField) power.warping = false;
            else if (!power.warping) {
                bool   can = spec.warpSpeed > 0.0 && mass <= spec.warpMaxMass;
                double need = JumpCharge(spec, mass);
                if (can && power.driveCap >= need) { power.driveCap -= need; power.warping = true; }
                else { r.charging = can; r.drive.limit = fmin(r.drive.limit, spec.hyperSpeed); }
            }
            double top = power.warping ? spec.warpSpeed : spec.hyperSpeed;
            double set = fmax(FTL_MIN, fmin(1.0f, ship.ftl)) * top;
            r.drive.limit = fmin(r.drive.limit, set);
            double v = DriveSpeedFor(spec, spare, mass, power.warping);
            if (v < r.drive.limit) { r.starved = !r.charging && v < 0.999 * set; r.drive.limit = v; }
        }
    }
    if (!in.hyper) power.warping = false;
    if (wasWarping && !power.warping) r.collapse = spec.jumpMJ * mass;        // the bubble goes, and what was in it with it
    power.engaged = in.hyper;

    UpdateShip(ship, in, dt, r.drive, ShipStats(spec, power, mass, reactor));
    if (!reactor) { power.engines = 1.0f; return r; }              // (the emergency cell isn't the reactor's business)

    // What everything drew this step, and where it came from
    double speed  = Vector3Length(ship.vel);
    double drive  = in.hyper ? DrivePower(spec, speed, mass, power.warping) : 0.0;
    double engine = in.hyper ? 0.0 : spec.engineMW * ship.thrustUse;
    double bankLoads = engine + (laser ? spec.laserMW : 0.0);
    double out = spec.reactorMW, used = fmin(out, spec.idleMW);
    out -= used;
    double toDrive = fmin(drive, out);                             // the drive's field is held on reactor power
    out -= toDrive; used += toDrive;
    r.drivePower = toDrive;
    double toLoads = fmin(bankLoads, out);                         // engines and the rest: reactor first, then the bank
    out -= toLoads; used += toLoads;
    double shortBy = (bankLoads - toLoads) * dt, fromBank = fmin(power.bank, shortBy);
    power.bank -= fromBank;
    power.engines = bankLoads > 0.0 ? (float)(1.0 - (shortBy - fromBank) / (bankLoads * dt)) : 1.0f;

    // Anything left over recharges the capacitors, split by the power mode (a full one's share goes to the
    // other). Some of what goes into the drive capacitor is lost on the way in.
    double share = power.mode == POWER_TRAVEL ? 0.75 : 0.25;
    double roomD = fmax(0.0, spec.driveCapMJ - power.driveCap) / spec.capIn, roomB = fmax(0.0, spec.bankMJ - power.bank), e = out * dt;
    double d = fmin(roomD, e * share), b = fmin(roomB, e * (1.0 - share));
    double left = e - d - b;
    double d2 = fmin(roomD - d, left); d += d2; left -= d2;
    b += fmin(roomB - b, left);
    power.driveCap += d * spec.capIn; power.bank += b;
    used += (d + b) / dt;

    r.load = used;
    r.burn = used / (spec.reactorEff * spec.fuelMJ);
    fuel = fmax(0.0, fuel - r.burn * dt);
    return r;
}

// How far from a body's center the hyper drive brings you to a stop when you fly
// at it: close enough to trade, scoop or carry on at cruise, clear of the surface.
double StopDistance(const Body &b)
{
    switch (b.kind) {
    case Kind::Star:    return b.radius * (1.0 + STOP_STAR_RADII);
    case Kind::Station: return b.radius + STOP_STATION;
    default:            return b.radius * (1.0 + STOP_WORLD_RADII);
    }
}

void PlaneAttitude(const Ship &s, DVec3 up, float &pitch, float &roll)
{
    Vector3 n = ToF(Norm(up)), fwd = ShipForward(s);
    float   climb = Clamp(Vector3DotProduct(fwd, n), -1.0f, 1.0f);
    pitch = asinf(climb) * RAD2DEG;
    Vector3 zenith = Vector3Subtract(n, Vector3Scale(fwd, climb));     // the plane's 'up' as seen looking along the nose
    roll = Vector3LengthSqr(zenith) < 1e-8f ? 0.0f
         : atan2f(-Vector3DotProduct(zenith, ShipRight(s)), Vector3DotProduct(zenith, ShipUp(s))) * RAD2DEG;
}

// Re-anchor the ship on whichever star is nearest. In the empty stretches (no
// star within a few cells) it keeps the one it had.
void Rebase(Ship &s, const Galaxy &sys)
{
    const StarSystem *near = sys.Nearest(s.anchor, s.pos);
    if (!near || near->id == s.anchor) return;
    DVec3 fromNew = s.pos - sys.Between(s.anchor, near->id);
    if (Len(fromNew) < Len(s.pos)) {
        s.pos = fromNew;
        s.anchor = near->id;
    }
}

DriveInfo ComputeDrive(const Ship &ship, const Galaxy &sys, const LoadedSystem &loaded, const LockInfo *lock, bool haveFuel,
                       double hyperTop, double warpTop)
{
    DriveInfo d;
    float speed = Vector3Length(ship.vel);
    DVec3 dir = ToD(speed > APPROACH_SPEED * 2.0f ? Vector3Scale(ship.vel, 1.0f / speed) : ShipForward(ship));

    if (loaded.index >= 0) {
        const StarSystem &s = sys[loaded.index];
        DVec3  p = FromShip(sys, ship, loaded.index) * -1.0;   // ship relative to the star
        double h = Dot(p, s.n), r = Len(p - s.n * h);
        d.fieldRatio = FieldRatio(s, p);
        d.inField    = d.fieldRatio < 1.0;
        d.exitClimb  = s.fieldH * sqrt(fmax(0.0, 1.0 - (r / s.fieldR) * (r / s.fieldR))) - fabs(h);
        d.exitEdge   = s.fieldR * sqrt(fmax(0.0, 1.0 - (h / s.fieldH) * (h / s.fieldH))) - r;
    }

    // Bodies ahead. Any of them slows you as you close on it, at cruise or in hyper;
    // one you're flying straight at brings the hyper drive to a stop short of it.
    double      hyperLimit = hyperTop;
    std::string hyperLimiter;
    bool        stopping = false;
    for (const Body &b : loaded.bodies) {
        DVec3  to    = FromShip(sys, ship, loaded.index, b.local);
        double along = Dot(to, dir);
        if (along <= 0.0) continue;                            // behind us
        double dist = Len(to), stop = StopDistance(b);
        double lim  = fmax((double)APPROACH_SPEED, WELL_K * fmax(0.0, dist - b.radius));
        if (lim < d.cruiseLimit) { d.cruiseLimit = lim; d.cruiseLimiter = b.name; }
        bool aimedAt = dist > stop && dist * dist - along * along < stop * stop;
        if (aimedAt) {
            lim = WELL_K * (dist - stop);
            if (lim < STOP_SPEED) lim = 0.0;
        }
        if (lim < hyperLimit) { hyperLimit = lim; hyperLimiter = b.name; stopping = aimedAt; }
    }

    if (d.inField) {
        d.limit = hyperLimit;
        d.limiter = hyperLimiter;
        // Whatever you're locked onto (a belt, a ring, a rock, a ship) stops you on arrival too
        // (if it's moving, that means matching its speed: closing on it is what tapers off)
        double keepUp = 0.0;
        if (lock) {
            double lim = WELL_K * fmax(0.0, Len(lock->rel) - lock->radius - STOP_LOCK);
            if (lim < STOP_SPEED) lim = 0.0;
            if (lim + lock->speed < d.limit) { d.limit = lim + lock->speed; d.limiter = lock->name; stopping = true; keepUp = lock->speed; }
        }
        d.holding = stopping && d.limit <= keepUp;
        if (!haveFuel) { d.limit = fmin(d.limit, LIMP_SPEED); d.noFuel = true; }   // emergency power only
    } else if (!haveFuel) {
        // Warp needs fuel; without it the drive limps along on emergency power
        d.limit  = LIMP_SPEED;
        d.noFuel = true;
    } else {
        // Warp: slow down for any system's gravity field ahead
        // (only fields within full warp speed's braking distance can matter)
        d.limit = warpTop;
        std::vector<const StarSystem *> ahead;
        sys.Nearby(ship.anchor, ship.pos, warpTop / WARP_WELL_K + 0.05 * LY_U, ahead);
        for (const StarSystem *s : ahead) {
            DVec3 to = FromShip(sys, ship, s->id);
            if (Dot(to, dir) <= 0.0) continue;
            double e = FieldRatio(*s, to * -1.0);
            if (e <= 1.0) continue;
            double distToField = Len(to) * (1.0 - 1.0 / e);   // along the line to the star
            double lim = fmax(hyperTop, WARP_WELL_K * distToField);
            if (lim < d.limit) { d.limit = lim; d.limiter = s->name; }
        }
    }
    return d;
}

void UpdateShip(Ship &s, const Controls &in, float dt, const DriveInfo &drive, const FlightStats &stats)
{
    // --- Rotation, about the ship's own axes: as far as asked, or as far as the thrusters can swing it ---
    float yaw = in.yaw, pitch = in.pitch, roll = in.roll;
    float most = (float)(stats.turn * dt), asked = sqrtf(yaw * yaw + pitch * pitch);
    if (asked > most) { yaw *= most / asked; pitch *= most / asked; }
    roll = Clamp(roll, -most, most);
    Quaternion dq = QuaternionMultiply(
        QuaternionFromAxisAngle({ 0, 1, 0 }, yaw * DEG2RAD),
        QuaternionMultiply(QuaternionFromAxisAngle({ 1, 0, 0 }, pitch * DEG2RAD),
                           QuaternionFromAxisAngle({ 0, 0, 1 }, roll * DEG2RAD)));
    s.rot = QuaternionNormalize(QuaternionMultiply(s.rot, dq));

    // --- Modes and throttle (while the FTL drive is engaged, the throttle keys set its speed instead) ---
    if (in.toggleDampeners) s.dampeners = !s.dampeners;
    if (in.allStop)         { s.allStop = true; s.throttle = 0.0f; }
    if (in.throttle != 0.0f || in.hyper) s.allStop = false;   // pilot override
    if (in.hyper) s.ftl = Clamp(s.ftl + in.throttle * THROTTLE_RATE * dt, FTL_MIN, 1.0f);
    else          s.throttle = Clamp(s.throttle + in.throttle * THROTTLE_RATE * dt, -THROTTLE_REVERSE, 1.0f);

    Vector3 fwd = ShipForward(s), up = ShipUp(s);
    float prevSpeed = Vector3Length(s.vel);

    if (in.hyper) {
        // --- Hyper / warp: steer velocity toward the nose; speed climbs steadily toward the limit ---
        Vector3 dir = prevSpeed > 0.001f ? Vector3Scale(s.vel, 1.0f / prevSpeed) : fwd;
        float angle = acosf(Clamp(Vector3DotProduct(dir, fwd), -1.0f, 1.0f));
        if (angle > 0.0001f) {
            Vector3 axis = Vector3CrossProduct(dir, fwd);
            if (Vector3LengthSqr(axis) < 1e-8f) axis = up;     // exactly backwards: turn over the top
            dir = Vector3RotateByAxisAngle(dir, Vector3Normalize(axis), angle * (1.0f - expf(-HYPER_ALIGN * dt)));
        } else {
            dir = fwd;                                         // (as near as makes no odds: exactly along the nose)
        }

        double speed = prevSpeed;
        if (speed < drive.limit) speed = fmin(drive.limit, speed + stats.ftlAccel * dt);
        else                     speed = fmax(drive.limit, speed * exp(-SPOOL_DOWN * dt));
        s.vel = Vector3Scale(Vector3Normalize(dir), (float)speed);
        s.thrustUse = 0.0f;                                   // the drive's power is counted separately
        s.dropout = true;
        s.throttle = 0.0f;                                    // let go of Shift and the ship comes to a stop
    } else {
        // --- Sublight: the throttle is thrust, along the nose ---
        float   along  = Vector3DotProduct(s.vel, fwd);
        Vector3 drift  = Vector3Subtract(s.vel, Vector3Scale(fwd, along));
        double  thrust = s.allStop ? 0.0 : ThrottleThrust(s.throttle);

        // Dampeners kill sideways drift hard, so the ship goes where it points. Under power,
        // a turn keeps its speed: what comes off the drift goes into the new heading.
        if (s.dampeners || s.allStop) {
            float d = Vector3Length(drift);
            if (d > 0.0f) {
                float after = fmaxf(0.0f, d * expf(-DAMPER_RATE * dt) - DAMPER_ACCEL * dt);
                if (along > 0.0f && thrust > 0.0) along = sqrtf(along * along + d * d - after * after);
                drift = Vector3Scale(drift, after / d);
            }
        }

        // What the engines can do to the ship's speed this step: thrust over mass, falling away as
        // light speed nears (a = F / gamma^3 m). Coming down from FTL speeds is the drive's doing.
        double beta  = fmin(0.999999, fabs(along) / LIGHT_SEC_U);
        double most  = stats.accel * pow(1.0 - beta * beta, 1.5) * dt;
        double limit = drive.cruiseLimit;
        float  was   = along;
        // (and with the throttle shut, it goes on shedding it: let go of Shift and the ship stops)
        if (s.dropout && (thrust != 0.0 || fabsf(along) < 1.0f)) s.dropout = false;
        if (fabsf(along) > MAX_SPEED || s.dropout) {
            along *= (float)exp(-SPOOL_DOWN * dt);
            if (fabsf(along) < 1.0f) along = 0.0f;
        } else if (thrust != 0.0 && !(thrust < 0.0 && along > 0.0)) {
            along += (float)(thrust * most);                   // pushing (forward, or astern from rest)
        } else if (thrust < 0.0 || s.dampeners || s.allStop) {
            // Braking: reverse thrust against forward motion, or the dampeners bringing the ship to rest
            // with the throttle at zero. Either way with everything the engines have.
            along = along > 0.0f ? fmaxf(0.0f, along - (float)most) : fminf(0.0f, along + (float)most);
        }
        if (fabsf(was) <= MAX_SPEED) along = (float)fmax(-limit, fmin(limit, (double)along));   // (wells ahead, and 0.99 c)
        s.vel = Vector3Add(Vector3Scale(fwd, along), drift);

        float sp = Vector3Length(s.vel);
        if (s.allStop && sp < 0.01f) { s.vel = Vector3Zero(); s.allStop = false; }

        // How hard the engines worked, for power: coasting is free
        double work = fabs((double)along - was) / fmax(most, 1e-9);
        s.thrustUse = fabsf(was) > MAX_SPEED || s.dropout || work < 1e-3 ? 0.0f : (float)fmin(work, 1.0);
    }

    s.pos = s.pos + ToD(s.vel) * (double)dt;
}
