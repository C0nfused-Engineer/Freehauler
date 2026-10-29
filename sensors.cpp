// sensors.cpp - tachyon detection, radar, infrared and telescopes; a captain's scope
#include "sensors.h"
#include "npcs.h"
#include "shipparts.h"
#include <algorithm>

static const double AU_M = 1.495978707e11;       // metres

static double Roll(uint64_t &dice) { dice = Mix64(dice + 0x9E3779B97F4A7C15ull); return (double)(dice >> 11) * (1.0 / 9007199254740992.0); }
static double Gauss(uint64_t &dice) { double u = fmax(Roll(dice), 1e-300), v = Roll(dice); return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v); }

// ---------------------------------------------------------------------------
// Tachyons
// ---------------------------------------------------------------------------
TachyonSensor ShipDetector(const ShipSpec &spec)
{
    TachyonSensor s;
    s.area = spec.tachArea; s.depth = spec.tachDepth; s.eff = spec.tachEff; s.res = spec.tachRes;
    return s;
}

TachyonSensor OutpostDetector()
{
    TachyonSensor s;
    s.area = OUTPOST_AREA; s.depth = OUTPOST_DEPTH; s.eff = OUTPOST_EFF; s.res = OUTPOST_RES; s.minEvents = OUTPOST_MIN; s.noise = OUTPOST_NOISE;
    return s;
}

TachyonSensor PortDetector()
{
    TachyonSensor s;
    s.area = PORT_AREA; s.depth = PORT_DEPTH; s.eff = PORT_EFF; s.res = 0.0; s.minEvents = PORT_MIN; s.noise = PORT_NOISE;
    return s;
}

double TachyonDecayAU(double mj) { return TACH_L0_AU * pow(fmax(mj, 1.0) / TACH_E0_TJ, TACH_L_EXP); }
double TachyonCount(double tj)   { return TACH_ETA * fmax(tj, 0.0) * 1.0e12 / TACH_EPS_J; }

double TachyonExpected(const TachyonSensor &s, double mj, double au)
{
    double L = TachyonDecayAU(mj) * AU_M, r = fmax(au, 1e-4) * AU_M;
    // (what's left after the trip, spread over the sphere it has reached; and of what crosses the detector, the few that decay inside)
    return TachyonCount(mj) * s.area / (4.0 * PI * r * r) * exp(-r / L) * -expm1(-s.depth / L) * s.eff;
}

double TachyonChance(const TachyonSensor &s, double mj, double au)
{
    double lambda = TachyonExpected(s, mj, au), term = exp(-lambda), below = 0.0;
    if (lambda > 200.0) return 1.0;
    for (int k = 0; k < s.minEvents; k++) { below += term; term *= lambda / (k + 1); }
    return fmax(0.0, 1.0 - below);
}

double TachyonRange(const TachyonSensor &s, double mj, double counts)
{
    double lo = 1e-4, hi = 1e5;                                    // (the count only ever falls with distance)
    for (int i = 0; i < 60; i++) {
        double mid = sqrt(lo * hi);
        if (TachyonExpected(s, mj, mid) > counts) lo = mid; else hi = mid;
    }
    return sqrt(lo * hi);
}

int Poisson(double lambda, uint64_t &dice)
{
    if (lambda <= 0.0) return 0;
    if (lambda > 40.0) return (int)fmax(0.0, floor(lambda + sqrt(lambda) * Gauss(dice) + 0.5));      // (near enough to normal)
    double limit = exp(-lambda), p = 1.0;
    int k = 0;
    do { k++; p *= Roll(dice); } while (p > limit);
    return k - 1;
}

bool DetectBurst(const TachyonSensor &s, DVec3 at, DVec3 burst, double mj, uint64_t &dice, TachyonDetection &out)
{
    DVec3  to = burst - at;
    double dist = fmax(Len(to), 1.0);
    int    real = Poisson(TachyonExpected(s, mj, dist / AU_U), dice);
    int    stray = Poisson(s.noise, dice);
    out = TachyonDetection();
    out.counts = real + stray;
    out.from = at;
    // (it takes a burst to call a burst: stray counts alone never do. An honest detector would raise the odd
    // false alarm; these don't.)
    if (out.counts < s.minEvents || real < 1) return false;

    // Direction: each count places it a little, so many place it well. The stray ones don't help.
    out.directional = s.res > 0.0;
    DVec3 dir = to * (1.0 / dist);
    if (out.directional) {
        out.sigma = fmax(0.05, s.res / sqrt((double)real));
        DVec3 side = Cross(dir, fabs(dir.y) < 0.9 ? DVec3{ 0, 1, 0 } : DVec3{ 1, 0, 0 });
        side = Norm(side);
        DVec3  other = Cross(dir, side);
        double a = out.sigma * DEG2RAD * Gauss(dice), b = out.sigma * DEG2RAD * Gauss(dice);
        out.bearing = Norm(dir + side * tan(a) + other * tan(b));
    }
    // Range: nobody knows how big the burst was, so the count only brackets it: a small ship close by and a big one
    // far off look alike. Allow for both, and for the count's own scatter.
    double k = out.counts, spread = 2.0 * sqrt(k);
    out.rLo = TachyonRange(s, TACH_GUESS_LO, k + spread) * AU_U;
    out.rHi = TachyonRange(s, TACH_GUESS_HI, fmax(1.0, k - spread)) * AU_U;
    return true;
}

bool Triangulate(const std::vector<TachyonDetection> &all, DVec3 &fix, double &err)
{
    std::vector<const TachyonDetection *> d;
    for (const TachyonDetection &t : all) if (t.directional) d.push_back(&t);
    const int n = (int)d.size();
    if (n < 2) return false;
    double spreadMost = 0.0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) spreadMost = fmax(spreadMost, Len(Cross(d[i]->bearing, d[j]->bearing)));
    if (spreadMost < 0.03) return false;                           // (all but parallel: they cross nowhere in particular)

    // The point p nearest every line (o + t d): sum w (I - d d^T) p = sum w (I - d d^T) o
    double spreadOut = 0.0;                                        // (the trace of the inverse of the last system solved: see the end)
    auto solve = [&](const std::vector<double> &weight, DVec3 &p) {
        double A[3][3] = {}, b[3] = {};
        for (int k = 0; k < n; k++) {
            const double v[3] = { d[k]->bearing.x, d[k]->bearing.y, d[k]->bearing.z }, o[3] = { d[k]->from.x, d[k]->from.y, d[k]->from.z };
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) {
                    double m = ((i == j) ? 1.0 : 0.0) - v[i] * v[j];
                    A[i][j] += weight[k] * m; b[i] += weight[k] * m * o[j];
                }
        }
        double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                     A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
        if (fabs(det) < 1e-300) return false;
        spreadOut = ((A[1][1] * A[2][2] - A[1][2] * A[2][1]) + (A[0][0] * A[2][2] - A[0][2] * A[2][0]) + (A[0][0] * A[1][1] - A[0][1] * A[1][0])) / det;
        auto col = [&](int c) {
            double M[3][3];
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = j == c ? b[i] : A[i][j];
            return (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                    M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
        };
        p = { col(0), col(1), col(2) };
        return true;
    };
    // A bearing is an angle: how far its line may miss by is that angle times the distance, so a detector close to
    // the burst says far more about where it was than one a long way off. The distances aren't known until there's
    // a fix, so: a first fix by the angles alone, then again with each line weighted by what it's really worth there.
    std::vector<double> weight(n), miss(n);
    double most = 1.0;
    for (int k = 0; k < n; k++) weight[k] = 1.0 / (d[k]->sigma * d[k]->sigma);
    if (!solve(weight, fix)) return false;
    for (int pass = 0; pass < 3; pass++) {
        for (int k = 0; k < n; k++) {
            miss[k] = fmax(1.0, Len(fix - d[k]->from) * d[k]->sigma * DEG2RAD);
            weight[k] = 1.0 / (miss[k] * miss[k]);
        }
        // (scaled so the sums stay well away from the limits of a double)
        most = 0.0;
        for (double w : weight) most = fmax(most, w);
        for (double &w : weight) w /= most;
        if (!solve(weight, fix)) return false;
    }
    // How far off it may be. With each line weighted by one over its miss squared, the inverse of that system is how
    // the fix scatters: tightly across a good line, loosely along it unless other lines cross it squarely. Its trace
    // is the whole of that scatter.
    err = sqrt(fmax(0.0, spreadOut) / most);
    return true;
}

// ---------------------------------------------------------------------------
// Radar, infrared, telescope
// ---------------------------------------------------------------------------
Signature SignatureOf(const ShipSpec &spec, const PowerState &power)
{
    Signature s;
    s.size = fmax(0.05, spec.hullSize);
    s.heat = fmax(6.4, (double)power.heat);
    s.radar = RadarOn(spec, power);
    s.radarPower = spec.radar;
    return s;
}

SensorFit FitOf(const ShipSpec &spec, const PowerState &power)
{
    return { spec.radar, spec.ir, spec.scan, RadarOn(spec, power) };
}

SensorFit PortSensors(const Body &b)
{
    // (a great port sees four or five times as far as a ship's basic set; an outpost is all antenna)
    double k = b.settlement == Settlement::SensorOutpost ? 4.0 : fmax(1.5, fmin(4.5, 1.0 + 0.5 * log10(fmax(b.population, 1.0))));
    return { k, k, k, true };
}

double RadarRange(double radar, double size) { return RADAR_AU * AU_U * radar * pow(fmax(size, 1e-6), 0.25); }
double IrRange(double ir, double heat)       { return IR_AU * AU_U * ir * sqrt(fmax(heat, 0.0) / HEAT_REF); }
double ScopeRange(double scope, double size) { return SCOPE_AU * AU_U * scope * cbrt(fmax(size, 1e-6)); }

int Sense(const SensorFit &s, const Signature &t, double dist, double gain, uint8_t *by)
{
    dist = fmax(dist, 1.0);
    // Signal over what it takes to find the target: an echo falls with the fourth power of distance, heat and
    // light with the square
    double radar = s.radarOn ? pow(RadarRange(s.radar, t.size) / dist, 4.0) * gain : 0.0;
    double ir    = pow(IrRange(s.ir, t.heat) / dist, 2.0) * gain;
    double scope = pow(ScopeRange(s.scope, t.size) / dist, 2.0) * gain;
    bool   near  = dist < EYEBALL;
    uint8_t held = (radar >= 1.0 ? BY_RADAR : 0) | (ir >= 1.0 ? BY_IR : 0) | (scope >= 1.0 || near ? BY_SCOPE : 0);
    if (by) *by = held;
    if (!held) return CONTACT_NONE;
    if (scope >= 1.0 || near) return CONTACT_IDENTIFIED;
    if (radar >= CLASSIFY || ir >= CLASSIFY) return CONTACT_CLASSIFIED;
    return CONTACT_DETECTED;
}

bool HearsRadar(const Signature &emitter, double dist)
{
    return emitter.radar && dist < RWR_REACH * RadarRange(emitter.radarPower, 1.0);
}

const char *ContactLevelName(int level)
{
    static const char *names[] = { "Lost", "Detected", "Classified", "Identified" };
    return names[level >= 0 && level <= CONTACT_IDENTIFIED ? level : 0];
}

std::string SensedBy(uint8_t by)
{
    std::string s;
    auto add = [&](const char *what) { s += (s.empty() ? "" : ", "); s += what; };
    if (by & BY_RADAR)   add("radar");
    if (by & BY_IR)      add("infrared");
    if (by & BY_SCOPE)   add("telescope");
    if (by & BY_CONTROL) add("traffic control");
    return s.empty() ? "nothing" : s;
}

// ---------------------------------------------------------------------------
// A captain's scope
// ---------------------------------------------------------------------------
const Contact *FindContact(const Captain &c, int id)
{
    for (const Contact &k : c.contacts) if (k.id == id) return &k;
    return nullptr;
}

void AddMark(Captain &c, const SensorMark &m)
{
    c.marksChanged = true;
    if (m.source >= 0)
        for (SensorMark &o : c.marks) if (o.kind == m.kind && o.source == m.source) { o = m; return; }
    if (c.marks.size() >= 24) c.marks.erase(c.marks.begin());
    c.marks.push_back(m);
}

void UpdateContacts(World &w, Captain &c, const std::vector<SensorTarget> &targets, const std::unordered_map<int, uint8_t> *picture)
{
    c.sensing = true;
    const SensorFit mine = FitOf(c.player.spec, c.player.power);
    for (Contact &k : c.contacts) k.by = 0;
    for (const SensorTarget &t : targets) {
        Contact *k = nullptr;
        for (Contact &o : c.contacts) if (o.id == t.id) k = &o;
        uint8_t by = 0;
        int level = Sense(mine, t.sig, Len(t.rel), k ? HOLD_GAIN : 1.0, &by);
        if (picture) {
            auto shared = picture->find(t.id);
            if (shared != picture->end() && shared->second > CONTACT_NONE) { level = std::max(level, (int)shared->second); by |= BY_CONTROL; }
        }
        // A radar sweeping the ship is heard far beyond where it can be seen: a bearing, if nothing better
        if (!by && HearsRadar(t.sig, Len(t.rel))) {
            SensorMark m;
            m.kind = MARK_RADAR; m.source = t.id; m.anchor = c.player.ship.anchor; m.from = c.player.ship.pos;
            m.dir = Norm(t.rel); m.sigma = 2.0f; m.at = w.clock;
            bool had = false;
            for (const SensorMark &o : c.marks) if (o.kind == MARK_RADAR && o.source == t.id) had = true;
            AddMark(c, m);
            if (!had) w.Emit(EventType::Notice, "Radar warning: a radar is sweeping you", {}, -1, 0.0, 0.0, c.id);
        }
        if (!by) continue;
        if (!k) { c.contacts.push_back(Contact()); k = &c.contacts.back(); k->id = t.id; }
        k->by = by;
        k->level = (uint8_t)std::max((int)k->level, level);
        k->seen = w.clock;
    }
    // Ghosts fade; so do bearings (a radar's as soon as it stops sweeping, or the ship itself comes into view)
    c.contacts.erase(std::remove_if(c.contacts.begin(), c.contacts.end(), [&](const Contact &k) {
        // (a ghost lasts a while; not once its system is left behind)
        return !k.Held() && (w.clock - k.seen > GHOST_TIME || w.clock < k.seen || (k.id >= NPC_ID_BASE && k.snap.ship.anchor != c.loaded.index)); }), c.contacts.end());
    size_t before = c.marks.size();
    c.marks.erase(std::remove_if(c.marks.begin(), c.marks.end(), [&](const SensorMark &m) {
        if (m.kind == MARK_RADAR) { const Contact *k = FindContact(c, m.source); return w.clock - m.at > 1.0 || w.clock < m.at || (k && k->Held()); }
        return w.clock - m.at > MARK_TIME || w.clock < m.at; }), c.marks.end());
    if (c.marks.size() != before) c.marksChanged = true;
}

void MaskTraffic(Traffic &t)
{
    if (t.level >= CONTACT_IDENTIFIED) return;
    t.shipName.clear(); t.from.clear(); t.dest.clear();
    t.good = -1; t.tonnes = 0.0f; t.faction = NO_FACTION; t.stance = STANCE_NEUTRAL;
    t.mining = false; t.miningRock = 0;
    if (t.level >= CONTACT_CLASSIFIED) {
        // What kind of ship, and how big: not which, or whose
        t.name = std::string("Unidentified ") + (t.npc ? NpcKindName(t.kind) : "ship");
        for (size_t i = 13; i < t.name.size(); i++) if (t.name[i] >= 'A' && t.name[i] <= 'Z') t.name[i] += 'a' - 'A';
    } else {
        t.name = "Unknown contact";
        t.kind = NpcKind::Freighter; t.hull = 255;
    }
}

void ScopeTraffic(const World &w, Captain &c, std::vector<Traffic> &list)
{
    std::vector<Traffic> shown;
    for (Contact &k : c.contacts) {
        if (k.Held()) {
            const Traffic *truth = nullptr;
            for (const Traffic &t : list) if (t.id == k.id) truth = &t;
            if (!truth) continue;
            k.snap = *truth;
            k.snap.level = k.level; k.snap.by = k.by; k.snap.ghost = false; k.snap.age = 0.0f;
            MaskTraffic(k.snap);
        } else {
            if (k.snap.id != k.id) continue;                       // (never shown: nothing to remember)
            k.snap.ghost = true; k.snap.by = 0;
            k.snap.age = (float)(w.clock - k.seen);
            k.snap.thrusting = k.snap.hyper = k.snap.mining = false;
        }
        shown.push_back(k.snap);
    }
    list = std::move(shown);
}
