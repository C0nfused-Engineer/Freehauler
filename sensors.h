// sensors.h - what ships and stations can know about each other.
//
// Nothing here is seen for free. A ship is on your scope because one of your
// sensors holds it, or because a port's traffic control is passing you its
// picture; otherwise it isn't there.
//
// Tachyons     When a warp bubble collapses it gives off a burst of short-wave
//              tachyons, in every direction, at once: they cross a system in no
//              time at all. They can't be seen, only caught: a detector is a
//              volume, and what it counts is the tachyons that happen to decay
//              inside it. How many do falls with the square of the distance,
//              and with their decay on the way:
//
//                  count = N A / (4 pi r^2)  x  e^(-r/L)  x  (1 - e^(-d/L))  x  efficiency
//
//              N is the burst (the bubble's energy), A and d the detector's face
//              and depth, L the decay length. L grows with the burst: a heavy
//              ship running a hard field makes tachyons that go further, as well
//              as more of them. The count is a matter of chance (Poisson), and
//              there is no range at which detection stops, only odds that fall
//              away. Since they all arrive together, timing says nothing about
//              where a burst was. A detector gives a direction (better with more
//              counts) and the count hints at the range; two or more detectors
//              well apart can cross their bearings for a place. That is what a
//              state's sensor outposts are for. A burst says a ship arrived. It
//              doesn't say whose, or where it went next.
//
// Radar        Active. Finds anything in reach (further for a bigger hull) and
//              follows it, but its pulses carry much further than its echoes:
//              anyone it sweeps knows it's there, and which way.
// Infrared     Passive. Sees heat: a ship holding a hyper field or burning its
//              engines shows from far off; one drifting with its reactor
//              throttled down hardly shows at all.
// Telescope    Passive and short-ranged, but it's the one that tells you who a
//              ship is.
//
// A contact is DETECTED (something's there), CLASSIFIED (what kind of ship) or
// IDENTIFIED (which ship, whose, and how it looks at you). Lose it and it's a
// ghost where it was last seen, for a minute or so.
//
// Long-wave tachyons (signalling between systems) are not here yet.
#pragma once
#include "game.h"

// ---------------------------------------------------------------------------
// Tuning: tachyons. These are set for the scale the game is played at: a basic
// ship's detector gets a usable count from a small ship's arrival a few AU off;
// a state's outposts cover most of a system; and nothing catches a ship that
// drops out hundreds of AU beyond the edge.
// ---------------------------------------------------------------------------
const double TACH_ETA      = 0.10;          // the share of a collapsing bubble's energy that comes out as tachyons
const double TACH_EPS_J    = 2.24e-22;       // joules to each short-wave tachyon (so a burst is its energy over this many)
const double TACH_E0_TJ    = 80000.0;        // a small ship's bubble (100 t at full speed)...
const double TACH_L0_AU    = 40.0;          // ...whose tachyons decay over this distance...
const double TACH_L_EXP    = 0.5;           // ...and a burst of other energy's over this power of the ratio
const int    TACH_MIN_SHIP = 3;             // counts a ship's detector wants before it will call it a burst
const double TACH_NOISE_SHIP = 0.3;         // stray counts in the same instant, on average (they blur the range)
const double TACH_GUESS_LO = 32000.0, TACH_GUESS_HI = 1600000.0;   // the bursts a range estimate allows for, terajoules (a courier .. a bulk carrier)
const double MARK_TIME     = 120.0;         // seconds a bearing stays on the scope
// What a state's outposts carry (and the single all-round detector a port has, which gives no direction)
const double OUTPOST_AREA = 1000.0, OUTPOST_DEPTH = 30.0, OUTPOST_EFF = 0.8, OUTPOST_RES = 3.0, OUTPOST_NOISE = 2.0;
const int    OUTPOST_MIN  = 6;
const double PORT_AREA = 100.0, PORT_DEPTH = 10.0, PORT_EFF = 0.6, PORT_NOISE = 1.0;
const int    PORT_MIN  = 4;
const double NET_RELAY     = 4.0;           // seconds for each outpost's report to reach the network (pulse-coded: a few bits a second)

// ---------------------------------------------------------------------------
// Tuning: radar, infrared, telescope. Ranges are for the basic set against a
// hauler going about its business.
// ---------------------------------------------------------------------------
const double RADAR_AU      = 1.5;           // radar: echoes fall with the fourth power of distance
const double IR_AU         = 1.0;           // infrared: against HEAT_REF terawatts of heat (it falls with the square)
const double HEAT_REF      = 1700.0;
const double SCOPE_AU      = 0.25;          // telescope: against a hauler-sized hull (further, in proportion, for a longer one)
const double RWR_REACH     = 3.0;           // a radar is heard this many times further off than it can see
const double CLASSIFY      = 3.0;           // radar or infrared makes out what kind of ship it is at this many times the signal that finds it
const double EYEBALL       = 1000.0 * KM_U; // this close, nothing is hidden from anyone
const double HOLD_GAIN     = 1.25;          // a contact already held is kept at this much less signal than it took to find
const double GHOST_TIME    = 75.0;          // seconds a lost contact's last known place stays on the scope
const double TRAFFIC_CONTROL = 0.5 * AU_U;  // within this of a lawful port, its traffic control shares the system's picture

// ---------------------------------------------------------------------------
// Tachyons
// ---------------------------------------------------------------------------
struct TachyonSensor {
    double area = 4.0, depth = 2.0;         // m^2 facing the burst, metres deep
    double eff = 0.5;                       // the share of decays inside it that it records
    int    minEvents = TACH_MIN_SHIP;       // fewer counts than this aren't called a detection
    double noise = TACH_NOISE_SHIP;         // background counts expected in the same gate
    double res = 12.0;                      // degrees: how well a single count places the direction (0: it gives none)
};
TachyonSensor ShipDetector(const ShipSpec &spec);
TachyonSensor OutpostDetector();
TachyonSensor PortDetector();

double TachyonDecayAU(double mj);                                  // L for a burst of this energy
double TachyonCount(double mj);                                    // N: how many tachyons it is
double TachyonExpected(const TachyonSensor &s, double mj, double au);          // lambda: counts expected this far off
double TachyonChance(const TachyonSensor &s, double mj, double au);            // the odds of at least minEvents (noise aside)
double TachyonRange(const TachyonSensor &s, double mj, double counts);         // the distance (AU) at which that many are expected
int    Poisson(double lambda, uint64_t &dice);

// What one detector made of one burst
struct TachyonDetection {
    int    counts = 0;
    bool   directional = false;
    DVec3  from;                            // where the detector was
    DVec3  bearing;                         // unit vector toward the burst, as measured (with its error)
    double sigma = 0.0;                     // degrees: how good that is
    double rLo = 0.0, rHi = 0.0;            // units: how far off the count says it could be
};
// Samples a detection (false: too few counts to call). 'dice' is the random stream: the same seed, the same result.
bool DetectBurst(const TachyonSensor &s, DVec3 at, DVec3 burst, double mj, uint64_t &dice, TachyonDetection &out);
// Where several bearings cross: the point nearest all the lines (least squares, weighted by how good each is).
// False if there are fewer than two, or they're too nearly parallel to cross. 'err': how far off it may be (units).
bool Triangulate(const std::vector<TachyonDetection> &d, DVec3 &fix, double &err);

// ---------------------------------------------------------------------------
// Radar, infrared, telescope
// ---------------------------------------------------------------------------
struct Signature {
    double size = 1.0;                      // hull volume against a hauler's
    double heat = 144.0;                      // terawatts
    bool   radar = false;                   // its own radar is transmitting...
    double radarPower = 1.0;                // ...this strongly (its radar's reach, as a multiple of the basic set's)
};
struct SensorFit { double radar = 0.0, ir = 0.0, scope = 0.0; bool radarOn = false; };
enum { BY_RADAR = 1, BY_IR = 2, BY_SCOPE = 4, BY_CONTROL = 8 };

Signature SignatureOf(const ShipSpec &spec, const PowerState &power);
SensorFit FitOf(const ShipSpec &spec, const PowerState &power);
SensorFit PortSensors(const Body &b);                              // a port's traffic control, by its size; an outpost's
// How well this fit holds that target at this distance: a ContactLevel, and (in 'by') which sensors it's by.
// 'gain': 1 to find it, HOLD_GAIN to keep it.
int    Sense(const SensorFit &s, const Signature &t, double dist, double gain = 1.0, uint8_t *by = nullptr);
double RadarRange(double radar, double size);                      // units: how far each finds such a target
double IrRange(double ir, double heat);
double ScopeRange(double scope, double size);
bool   HearsRadar(const Signature &emitter, double dist);          // the radar warning: is its radar sweeping us?

// ---------------------------------------------------------------------------
// A captain's scope
// ---------------------------------------------------------------------------
// Something a captain's sensors might hold: where it really is, and how it shows
struct SensorTarget { int id; DVec3 rel, vel; Signature sig; };    // 'rel': from the captain's ship
// Brings the captain's contacts up to date. 'picture': what traffic control is sharing (levels by ship id), or null.
void UpdateContacts(World &w, Captain &c, const std::vector<SensorTarget> &targets, const std::unordered_map<int, uint8_t> *picture);
// (ScopeTraffic, in game.h, then turns the list of every ship there is into what this captain's scope shows: only
// contacts, each with no more known of it than has been learned, and the ghosts of the ones just lost.)
// Hides what isn't known of a ship at this level (the client does the same to what it's sent)
void MaskTraffic(Traffic &t);
const Contact *FindContact(const Captain &c, int id);
const char *ContactLevelName(int level);
std::string SensedBy(uint8_t by);                                  // "radar, infrared"
// Records a bearing on the scope (replacing the one from the same source, if it has one)
void AddMark(Captain &c, const SensorMark &m);
