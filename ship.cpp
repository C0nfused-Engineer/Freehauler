// ship.cpp - flight model and drives
#include "ship.h"

// Fuel burn of the hyper/warp drive at a given speed. Each step up in speed
// multiplies the burn: between anchors the rate is interpolated exponentially
// in log-speed, so cruise -> hyper is x5 and hyper -> warp another x12.
double DriveBurnRate(double speed)
{
    auto seg = [](double v, double v0, double v1, double r0, double r1) {
        double t = Clamp((float)(log(v / v0) / log(v1 / v0)), 0.0f, 1.0f);
        return r0 * pow(r1 / r0, t);
    };
    if (speed <= MAX_SPEED)   return CRUISE_BURN;
    if (speed <= HYPER_SPEED) return seg(speed, MAX_SPEED, HYPER_SPEED, CRUISE_BURN, HYPER_BURN);
    return seg(speed, HYPER_SPEED, WARP_SPEED, HYPER_BURN, WARP_BURN);
}

// The speed a throttle setting asks for. Each notch multiplies the speed, so the
// low end is fine enough for docking and the top is 0.99 c.
double ThrottleSpeed(float throttle)
{
    double t = fabs(throttle);
    double v = THROTTLE_MIN_SPEED * (pow((double)MAX_SPEED / THROTTLE_MIN_SPEED + 1.0, t) - 1.0);
    return throttle < 0.0f ? -v : v;
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

DriveInfo ComputeDrive(const Ship &ship, const Galaxy &sys, const LoadedSystem &loaded,
                       const DVec3 *lockRel, double lockRadius, const std::string &lockName, bool haveFuel)
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
    double      hyperLimit = HYPER_SPEED;
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
        if (lockRel) {
            double lim = WELL_K * fmax(0.0, Len(*lockRel) - lockRadius - STOP_LOCK);
            if (lim < STOP_SPEED) lim = 0.0;
            if (lim < d.limit) { d.limit = lim; d.limiter = lockName; stopping = true; }
        }
        d.holding = stopping && d.limit <= 0.0;
        if (!haveFuel) { d.limit = fmin(d.limit, LIMP_SPEED); d.noFuel = true; }   // emergency power only
    } else if (!haveFuel) {
        // Warp needs fuel; without it the drive limps along on emergency power
        d.limit  = LIMP_SPEED;
        d.noFuel = true;
    } else {
        // Warp: slow down for any system's gravity field ahead
        // (only fields within full warp speed's braking distance can matter)
        d.limit = WARP_SPEED;
        std::vector<const StarSystem *> ahead;
        sys.Nearby(ship.anchor, ship.pos, WARP_SPEED / WARP_WELL_K + 0.05 * LY_U, ahead);
        for (const StarSystem *s : ahead) {
            DVec3 to = FromShip(sys, ship, s->id);
            if (Dot(to, dir) <= 0.0) continue;
            double e = FieldRatio(*s, to * -1.0);
            if (e <= 1.0) continue;
            double distToField = Len(to) * (1.0 - 1.0 / e);   // along the line to the star
            double lim = fmax(HYPER_SPEED, WARP_WELL_K * distToField);
            if (lim < d.limit) { d.limit = lim; d.limiter = s->name; }
        }
    }
    return d;
}

void UpdateShip(Ship &s, const Controls &in, float dt, const DriveInfo &drive)
{
    // --- Rotation, about the ship's own axes ---
    Quaternion dq = QuaternionMultiply(
        QuaternionFromAxisAngle({ 0, 1, 0 }, in.yaw * DEG2RAD),
        QuaternionMultiply(QuaternionFromAxisAngle({ 1, 0, 0 }, in.pitch * DEG2RAD),
                           QuaternionFromAxisAngle({ 0, 0, 1 }, in.roll * DEG2RAD)));
    s.rot = QuaternionNormalize(QuaternionMultiply(s.rot, dq));

    // --- Modes and throttle ---
    if (in.toggleDampeners) s.dampeners = !s.dampeners;
    if (in.allStop)         { s.allStop = true; s.throttle = 0.0f; }
    if (in.throttle != 0.0f || in.hyper) s.allStop = false;   // pilot override
    s.throttle = Clamp(s.throttle + in.throttle * THROTTLE_RATE * dt, -THROTTLE_REVERSE, 1.0f);

    Vector3 fwd = ShipForward(s), up = ShipUp(s);
    float prevSpeed = Vector3Length(s.vel);

    if (in.hyper) {
        // --- Hyper / warp: steer velocity toward the nose, spool speed toward the limit ---
        Vector3 dir = prevSpeed > 0.001f ? Vector3Scale(s.vel, 1.0f / prevSpeed) : fwd;
        float angle = acosf(Clamp(Vector3DotProduct(dir, fwd), -1.0f, 1.0f));
        if (angle > 0.0001f) {
            Vector3 axis = Vector3CrossProduct(dir, fwd);
            if (Vector3LengthSqr(axis) < 1e-8f) axis = up;     // exactly backwards: turn over the top
            dir = Vector3RotateByAxisAngle(dir, Vector3Normalize(axis), angle * (1.0f - expf(-HYPER_ALIGN * dt)));
        }

        double speed = prevSpeed;
        if (speed < drive.limit) speed = fmin(drive.limit, fmax(speed, APPROACH_SPEED * 0.25) * exp(SPOOL_UP * dt));
        else                     speed = fmax(drive.limit, speed * exp(-SPOOL_DOWN * dt));
        s.vel = Vector3Scale(Vector3Normalize(dir), (float)speed);
        s.thrustUse = 0.0f;                                   // the drive's burn is counted separately
        s.throttle = 0.0f;                                    // let go of Shift and the ship comes to a stop
    } else {
        // --- Cruise: the engines hold the speed the throttle asks for, along the nose ---
        Vector3 before = s.vel;
        double  target = fmax(-drive.cruiseLimit, fmin(drive.cruiseLimit, ThrottleSpeed(s.throttle)));
        float   along  = Vector3DotProduct(s.vel, fwd);
        Vector3 drift  = Vector3Subtract(s.vel, Vector3Scale(fwd, along));

        // Dampeners kill sideways drift hard, so the ship goes where it points. Flying
        // forward, a turn keeps its speed: what comes off the drift goes into the new heading.
        if (s.dampeners || s.allStop) {
            float d = Vector3Length(drift);
            if (d > 0.0f) {
                float after = fmaxf(0.0f, d * expf(-DAMPER_RATE * dt) - DAMPER_ACCEL * dt);
                if (along > 0.0f && target > 0.0 && !s.allStop) along = sqrtf(along * along + d * d - after * after);
                drift = Vector3Scale(drift, after / d);
            }
        }

        // Along the nose: toward the throttle's speed. The engines push harder the faster
        // you're going (and brake harder still), so 0.99 c is seconds away, not weeks.
        bool   faster = target * along >= 0.0 && fabs(target) > fabsf(along);
        double rate   = faster ? CRUISE_SPOOL : fabsf(along) > MAX_SPEED ? SPOOL_DOWN : CRUISE_BRAKE;
        double step   = fmax(faster ? THRUST_ACCEL : BRAKE_ACCEL, fabsf(along) * rate) * dt;
        along += (float)fmax(-step, fmin(step, target - along));
        s.vel = Vector3Add(Vector3Scale(fwd, along), drift);

        // Coming out of hyper/warp, speed drops fast to what cruise allows
        double limit = fmax((double)MAX_SPEED, prevSpeed * exp(-SPOOL_DOWN * dt));
        float  sp = Vector3Length(s.vel);
        if (sp > limit) s.vel = Vector3Scale(s.vel, (float)(limit / sp));
        if (s.allStop && sp < 0.01f) { s.vel = Vector3Zero(); s.allStop = false; }

        // How hard the engines worked, for fuel: holding a steady speed is free
        float work = Vector3Length(Vector3Subtract(s.vel, before)) / (fmaxf(THRUST_ACCEL, prevSpeed * (float)CRUISE_SPOOL) * dt);
        s.thrustUse = work < 1e-3f ? 0.0f : fminf(work, 1.0f);
    }

    s.pos = s.pos + ToD(s.vel) * (double)dt;
}
