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

// Re-anchor the ship on whichever star is nearest
void Rebase(Ship &s, const std::vector<StarSystem> &sys)
{
    int best = s.anchor;
    double bestDist = Len(s.pos);
    for (int j = 0; j < (int)sys.size(); j++) {
        double d = Len(FromShip(sys, s, j));
        if (d < bestDist) { bestDist = d; best = j; }
    }
    if (best != s.anchor) {
        s.pos = s.pos + (sys[s.anchor].gal - sys[best].gal);
        s.anchor = best;
    }
}

DriveInfo ComputeDrive(const Ship &ship, const std::vector<StarSystem> &sys, const LoadedSystem &loaded,
                       const DVec3 *lockRel, double lockRadius, const std::string &lockName, bool haveFuel)
{
    DriveInfo d;
    float speed = Vector3Length(ship.vel);
    DVec3 dir = ToD(speed > MAX_SPEED * 2.0f ? Vector3Scale(ship.vel, 1.0f / speed) : ShipForward(ship));

    if (loaded.index >= 0) {
        const StarSystem &s = sys[loaded.index];
        DVec3  p = FromShip(sys, ship, loaded.index) * -1.0;   // ship relative to the star
        double h = Dot(p, s.n), r = Len(p - s.n * h);
        d.fieldRatio = FieldRatio(s, p);
        d.inField    = d.fieldRatio < 1.0;
        d.exitClimb  = s.fieldH * sqrt(fmax(0.0, 1.0 - (r / s.fieldR) * (r / s.fieldR))) - fabs(h);
        d.exitEdge   = s.fieldR * sqrt(fmax(0.0, 1.0 - (h / s.fieldH) * (h / s.fieldH))) - r;
    }

    if (d.inField) {
        // Hyper: slow down for any body ahead
        d.limit = HYPER_SPEED;
        for (const Body &b : loaded.bodies) {
            DVec3 to = FromShip(sys, ship, loaded.index, b.local);
            if (Dot(to, dir) <= 0.0) continue;                 // behind us
            double surf = fmax(0.0, Len(to) - b.radius);
            double lim  = fmax((double)MAX_SPEED, WELL_K * surf);
            if (lim < d.limit) { d.limit = lim; d.limiter = b.name; }
        }
        // Whatever you're locked onto (a belt, a ring, a rock) brakes you on arrival too
        if (lockRel) {
            double lim = fmax((double)MAX_SPEED, WELL_K * fmax(0.0, Len(*lockRel) - lockRadius));
            if (lim < d.limit) { d.limit = lim; d.limiter = lockName; }
        }
        if (!haveFuel) { d.limit = fmin(d.limit, LIMP_SPEED); d.noFuel = true; }   // emergency power only
    } else if (!haveFuel) {
        // Warp needs fuel; without it the drive limps along on emergency power
        d.limit  = LIMP_SPEED;
        d.noFuel = true;
    } else {
        // Warp: slow down for any system's gravity field ahead
        d.limit = WARP_SPEED;
        for (int j = 0; j < (int)sys.size(); j++) {
            DVec3 to = FromShip(sys, ship, j);
            if (Dot(to, dir) <= 0.0) continue;
            double e = FieldRatio(sys[j], to * -1.0);
            if (e <= 1.0) continue;
            double distToField = Len(to) * (1.0 - 1.0 / e);   // along the line to the star
            double lim = fmax(HYPER_SPEED, WARP_WELL_K * distToField);
            if (lim < d.limit) { d.limit = lim; d.limiter = sys[j].name; }
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

    // --- Modes ---
    if (in.toggleDampeners) s.dampeners = !s.dampeners;
    if (in.allStop)         s.allStop = true;
    if (in.forward != 0.0f || in.strafe != 0.0f || in.hyper) s.allStop = false;  // pilot override

    Vector3 fwd = ShipForward(s), right = ShipRight(s), up = ShipUp(s);
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
        if (speed < drive.limit) speed = fmin(drive.limit, fmax(speed, MAX_SPEED * 0.25) * exp(SPOOL_UP * dt));
        else                     speed = fmax(drive.limit, speed * exp(-SPOOL_DOWN * dt));
        s.vel = Vector3Scale(Vector3Normalize(dir), (float)speed);
        s.thrustUse = 0.0f;                                   // the drive's burn is counted separately
    } else {
        // --- Cruise: thrust is a force, so it changes velocity, not position ---
        Vector3 thrust = Vector3Add(Vector3Scale(fwd, in.forward), Vector3Scale(right, in.strafe));
        if (Vector3LengthSqr(thrust) > 0.0f) thrust = Vector3Normalize(thrust);
        Vector3 accel = Vector3Scale(thrust, THRUST_ACCEL);
        s.vel = Vector3Add(s.vel, Vector3Scale(accel, dt));
        float used = Vector3LengthSqr(accel) > 0.0f ? 1.0f : 0.0f;   // thruster work, for fuel burn

        // Dampeners: brake all motion except along the thrust direction
        if (s.dampeners) {
            Vector3 kept = Vector3Zero();
            if (Vector3LengthSqr(accel) > 0.0f) {
                Vector3 t = Vector3Normalize(accel);
                float along = Vector3DotProduct(s.vel, t);
                if (along > 0.0f) kept = Vector3Scale(t, along);
            }
            Vector3 drift = Vector3Subtract(s.vel, kept);
            float driftSpeed = Vector3Length(drift);
            if (driftSpeed > 0.0f) {
                float after = fmaxf(0.0f, driftSpeed - DAMPER_ACCEL * dt);
                drift = Vector3Scale(drift, after / driftSpeed);
                used += (driftSpeed - after) / (THRUST_ACCEL * dt);     // braking uses the thrusters too
            }
            s.vel = Vector3Add(kept, drift);
        }

        // All stop: autopilot brakes straight against the velocity
        if (s.allStop) {
            float sp = Vector3Length(s.vel);
            float ns = sp - BRAKE_ACCEL * dt;
            used += fminf(sp, BRAKE_ACCEL * dt) / (THRUST_ACCEL * dt);
            if (ns <= 0.0f) { s.vel = Vector3Zero(); s.allStop = false; }
            else            { s.vel = Vector3Scale(s.vel, ns / sp); }
        }
        s.thrustUse = used;

        // Cruise limit; coming out of hyper/warp, speed drops fast toward it
        double limit = fmax((double)MAX_SPEED, prevSpeed * exp(-SPOOL_DOWN * dt));
        float sp = Vector3Length(s.vel);
        if (sp > limit) s.vel = Vector3Scale(s.vel, (float)(limit / sp));
    }

    s.pos = s.pos + ToD(s.vel) * (double)dt;
}
