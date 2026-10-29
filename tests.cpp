// tests.cpp - headless tests of the game rules, and scripted demo runs
#include "tests.h"
#include "client.h"
#include "comms.h"
#include "contracts.h"
#include "economy.h"
#include "fleet.h"
#include "npcs.h"
#include "partsdata.h"
#include "process.h"
#include "save.h"
#include "sensors.h"
#include "server.h"
#include "settings.h"
#include "shipyard.h"
#include "format.h"
#include "targetinfo.h"
#include "ui.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int BodyIndex(const Captain &c, const std::string &name)
{
    for (int i = 0; i < (int)c.loaded.bodies.size(); i++) if (c.loaded.bodies[i].name == name) return i;
    return -1;
}

// Every system within 'ly' of home (home included)
static std::vector<const StarSystem *> Neighbours(const World &w, double ly)
{
    std::vector<const StarSystem *> out;
    w.systems.Nearby(w.systems.home, {}, ly * LY_U, out);
    return out;
}

// The first inhabited planet in the loaded system (a world you can trade with)
static int TradeWorld(const Captain &c)
{
    for (int i = 0; i < (int)c.loaded.bodies.size(); i++)
        if (c.loaded.bodies[i].kind == Kind::Planet && c.loaded.bodies[i].population > 0) return i;
    return -1;
}

// Somewhere else in the loaded system with people to talk to
static int TradeWorldOther(const Captain &c, int notThis)
{
    for (int i = 0; i < (int)c.loaded.bodies.size(); i++)
        if (i != notThis && c.loaded.bodies[i].population > 0) return i;
    return -1;
}

static void Face(const World &w, Captain &c, DVec3 dir)
{
    const StarSystem &home = w.systems[c.player.ship.anchor];
    c.player.ship.rot = QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, ToF(Norm(dir)), ToF(Norm(home.n + home.u * 0.3)))));
}
static void Face(GameState &g, DVec3 dir) { Face(g, g, dir); }

// Beside a body at an altitude (in its radii), stopped, facing it
static void Park(const World &w, Captain &c, int body, double altRadii)
{
    const StarSystem &home = w.systems[c.loaded.index];
    const Body &b = c.loaded.bodies[body];
    DVec3 dir = Norm(home.u * 0.6 + home.v * 0.7 + home.n * 0.4);
    c.player.ship.anchor = c.loaded.index;
    c.player.ship.pos = b.local + dir * (b.radius * (1.0 + altRadii));
    c.player.ship.vel = { 0, 0, 0 };
    c.player.ship.throttle = 0.0f;
    Face(w, c, b.local - c.player.ship.pos);
}
static void Park(GameState &g, int body, double altRadii) { Park(g, g, body, altRadii); }

// Runs the simulation for a while with fixed controls
static void Run(GameState &g, double seconds, Controls c = {}, Aim aim = {})
{
    int steps = (int)(seconds * SIM_HZ + 0.5);
    for (int i = 0; i < steps; i++) {
        Controls s = c;
        if (i > 0) s.allStop = s.toggleDampeners = s.cyclePower = s.toggleRadar = s.toggleReactor = false;      // one-shot presses happen once
        StepGame(g, s, aim, (float)(1.0 / SIM_HZ));
    }
}

static std::string SettingsPathForTests()                 // any non-save text file
{
    FILE *f = fopen("not_a_save.txt", "w");
    if (f) { fputs("hello\n", f); fclose(f); }
    return "not_a_save.txt";
}

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { g_checks++; bool ok_ = (cond); printf("  %s  ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) g_fail++; } while (0)

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void TestFuel()
{
    GameState g; NewGame(g);
    const SysId H = g.systems.home;
    const StarSystem &home = g.systems[H];
    int c = BodyIndex(g, home.name + " c"), d = BodyIndex(g, home.name + " d");
    const ShipSpec &spec = g.player.spec;
    g.player.power.radar = false;                                 // (the radar's draw is its own test: sensors)

    // Sublight: thrust burns fuel; coasting costs only what the reactor idles at
    g.player.ship.anchor = H; g.player.ship.pos = home.n * (0.7 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    Face(g, home.u);
    double f0 = g.player.fuel;
    g.player.ship.throttle = 1.0f;
    Run(g, 5.0);
    double v = Vector3Length(g.player.ship.vel), burned = f0 - g.player.fuel;
    CHECK(v > 0.5 * LIGHT_SEC_U && fabs(burned - 5.0 * ReactorBurnFor(spec, spec.idleTW + spec.engineTW)) < 0.002,
          "5 s at full throttle: %.2f c on %.4f t of fuel", v / LIGHT_SEC_U, burned);
    g.player.ship.vel = { 0, 0, 0 }; g.player.ship.throttle = 0.6f;
    Run(g, 10.0);
    v = Vector3Length(g.player.ship.vel);
    f0 = g.player.fuel;
    Run(g, 5.0);
    CHECK(fabs(v - ThrottleSpeed(0.6f)) < 1e-3 * v && fabs(Vector3Length(g.player.ship.vel) - v) < 1e-3 * v && fabs(f0 - g.player.fuel - 5.0 * ReactorBurnFor(spec, spec.idleTW)) < 1e-6,
          "throttle at 60%%: up to %.0f km/s, and holding it there costs only what the reactor idles at (%.4f t in 5 s)", v / KM_U, f0 - g.player.fuel);
    g.player.ship.throttle = 0.0f;
    g.player.ship.vel = { 0, 0, 0 };

    // Hyper from c toward d
    g.player.ship.anchor = H; g.player.ship.pos = {};
    UpdateLoadedSystem(g);
    const Body &bc = g.loaded.bodies[c], &bd = g.loaded.bodies[d];
    g.player.ship.pos = bc.local + Norm(bd.local - bc.local) * (bc.radius + 2000 * KM_U);
    g.player.ship.vel = { 0, 0, 0 };
    Face(g, bd.local - bc.local);
    f0 = g.player.fuel;
    Controls hyper; hyper.hyper = true;
    Aim toward; toward.hovered = TargetId::MakeBody(H, d);     // locked on, so we stop there
    Run(g, 25.0, hyper, toward);
    double toD = Len(FromShip(g.systems, g.player.ship, H, bd.local)) - bd.radius;
    CHECK(f0 - g.player.fuel > 0.1 && f0 - g.player.fuel < 1.5, "hyper c -> d (%.2f AU, locked on) burns %.3f t", Len(bd.local - bc.local) / AU_U, f0 - g.player.fuel);
    CHECK(toD < 0.001 * AU_U, "hyper arrives at d (%.0f km from its surface)", toD / KM_U);

    // Warp, out of the field
    g.player.ship.anchor = H; g.player.ship.pos = home.n * (0.7 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    Face(g, home.n);
    g.player.fuel = 50; f0 = 50;
    g.player.power = PowerState();
    Run(g, 10.0, hyper);
    CHECK(!g.flight.drive.inField && f0 - g.player.fuel > 1.6 && f0 - g.player.fuel < 3.5,
          "10 s of warp burns %.3f t (speed %.3f ly/s)", f0 - g.player.fuel, Vector3Length(g.player.ship.vel) / LY_U);

    // Empty tank: limp
    g.player.ship.anchor = H; g.player.ship.pos = home.n * (0.7 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    g.player.fuel = 0;
    Run(g, 5.0, hyper);
    CHECK(g.flight.drive.noFuel && fabs(Vector3Length(g.player.ship.vel) - LIMP_SPEED) < LIMP_SPEED * 0.01,
          "empty tank limps at %.4f AU/s", Vector3Length(g.player.ship.vel) / AU_U);
}

// Flying: the throttle is thrust, rudder, dampeners, and the hyper drive stopping short of what it's aimed at
static void TestFlight()
{
    GameState g; NewGame(g);
    const SysId H = g.systems.home;
    const StarSystem &home = g.systems[H];
    Ship &ship = g.player.ship;
    auto speed = [&]() { return (double)Vector3Length(ship.vel); };
    auto nose  = [&]() { return speed() > 0.0 ? Vector3DotProduct(Vector3Scale(ship.vel, 1.0f / (float)speed()), ShipForward(ship)) : 0.0f; };

    // Open space, nothing to slow for
    ship.anchor = H; ship.pos = home.n * (0.7 * LY_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.0f;
    Face(g, home.n);
    Controls up; up.throttle = 1;
    Run(g, 1.0, up);
    CHECK(fabsf(ship.throttle - THROTTLE_RATE) < 0.01f && speed() > 1.0 && speed() < 2000.0,
          "1 s of W: throttle %.0f%%, %.0f m/s", ship.throttle * 100.0f, speed() * METERS_PER_UNIT);
    {
        // The throttle sets a speed: the ship goes to it and holds. Most of its travel is speeds for manoeuvring.
        auto settle = [&](float throttle) { ship.throttle = throttle; Run(g, 6.0); return speed(); };
        double v20 = settle(0.2f), v50 = settle(0.5f), v80 = settle(0.8f), back = settle(0.2f);
        CHECK(fabs(v20 - ThrottleSpeed(0.2f)) < 0.01 * v20 && fabs(v50 - ThrottleSpeed(0.5f)) < 0.01 * v50 && fabs(v80 - ThrottleSpeed(0.8f)) < 0.01 * v80 &&
              fabs(back - v20) < 0.01 * v20 && v20 < 100.0 && v50 < 20000.0 && v80 < 0.05 * LIGHT_SEC_U && g.player.ship.thrustUse == 0.0f,
              "throttle to speed: 20%% holds %.0f m/s, 50%% holds %.0f km/s, 80%% holds %.0f km/s; back to 20%% and it slows to that again",
              v20 * METERS_PER_UNIT, v50 / KM_U, v80 / KM_U);
    }
    ship.vel = { 0, 0, 0 }; ship.throttle = 1.0f;
    double t99 = 0.0;
    while (speed() < 0.99 * LIGHT_SEC_U * 0.9999 && t99 < 60.0) { Run(g, 0.25); t99 += 0.25; }
    CHECK(ship.throttle == 1.0f && t99 > 17.0 && t99 < 23.0 && speed() <= 0.9901 * LIGHT_SEC_U,
          "full throttle from rest: 0.99 c in %.1f s, and no further", t99);
    Run(g, 3.0);
    CHECK(ship.throttle == 1.0f && speed() > 0.989 * LIGHT_SEC_U, "hands off: the throttle stays where it was set");

    // Turning: the ship goes where it points, and keeps its speed
    Controls rudder; rudder.yaw = 90.0f / (float)SIM_HZ;                  // A held for a second: 90 degrees
    Run(g, 1.0, rudder);
    CHECK(speed() > 0.9 * LIGHT_SEC_U && nose() > 0.9f, "a 90 degree turn at 0.99 c: still %.2f c, %.0f degrees of slip",
          speed() / LIGHT_SEC_U, acosf(fminf(nose(), 1.0f)) * RAD2DEG);
    Run(g, 1.5);
    CHECK(nose() > 0.9999f, "...and a moment later the slip is gone");
    Controls damp; damp.toggleDampeners = true;
    Run(g, 1.0 / SIM_HZ, damp);
    Run(g, 1.0, rudder);
    CHECK(!ship.dampeners && nose() < 0.8f, "dampeners off: the same turn leaves the ship sliding (%.0f degrees of slip)", acosf(nose()) * RAD2DEG);
    Run(g, 1.0 / SIM_HZ, damp);

    // Throttle shut: the dampeners bring the ship to rest with the engines. All stop does the same from any setting.
    Run(g, 3.0);
    ship.throttle = 0.0f;
    double before = speed();
    Run(g, 5.0);
    double slowing = speed();
    Run(g, 20.0);
    CHECK(before > 0.9 * LIGHT_SEC_U && slowing < before && slowing > 0.1 * LIGHT_SEC_U && speed() == 0.0,
          "throttle shut at 0.99 c: the dampeners brake with the engines (%.2f c after 5 s), stopped inside 25 s", slowing / LIGHT_SEC_U);
    ship.throttle = 1.0f;
    Run(g, 4.0);
    Controls stop; stop.allStop = true;
    Run(g, 25.0, stop);
    CHECK(speed() == 0.0 && ship.throttle == 0.0f && !ship.allStop, "all stop (W+S): throttle to zero, and stopped");
    {
        // The keys: W and S together are all stop, and neither does anything more until both are up
        Settings keys;
        bool cut = false;
        InputFrame in;
        in.Hold(KEY_W);
        Controls c1 = ReadFlightControls(in, 1.0f / 60.0f, keys, cut);
        in.Hold(KEY_S);
        Controls c2 = ReadFlightControls(in, 1.0f / 60.0f, keys, cut), c3 = ReadFlightControls(in, 1.0f / 60.0f, keys, cut);
        InputFrame onlyS; onlyS.Hold(KEY_S);
        Controls c4 = ReadFlightControls(onlyS, 1.0f / 60.0f, keys, cut);
        Controls c5 = ReadFlightControls(InputFrame(), 1.0f / 60.0f, keys, cut);
        Controls c6 = ReadFlightControls(onlyS, 1.0f / 60.0f, keys, cut);
        CHECK(c1.throttle == 1.0f && !c1.allStop && c2.allStop && c2.throttle == 0.0f && !c3.allStop && c4.throttle == 0.0f &&
              !c4.allStop && c5.throttle == 0.0f && c6.throttle == -1.0f,
              "W+S is all stop; letting go of one of them doesn't move the throttle; S works again once both are up");
    }
    Controls down; down.throttle = -1;
    Run(g, 5.0, down);
    CHECK(ship.throttle == -THROTTLE_REVERSE && Vector3DotProduct(ship.vel, ShipForward(ship)) < -300.0f && speed() < 600.0,
          "S from a standstill: reverse, no faster than %.1f km/s", speed() / KM_U);
    Run(g, 10.0, stop);

    // Hyper straight at a planet: the drive stops by itself, in trading range
    UpdateLoadedSystem(g);
    ship.anchor = H; ship.pos = {};
    UpdateLoadedSystem(g);
    int d = BodyIndex(g, home.name + " d");
    const Body bd = g.loaded.bodies[d];
    DVec3 out = Norm(bd.local * -1.0 + home.n * (0.3 * Len(bd.local)));   // approach from sunward and above the plane
    DVec3 side = Norm(Cross(out, home.n));
    ship.pos = bd.local + out * (0.05 * AU_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.0f;
    Face(g, bd.local - ship.pos);
    Controls hyper; hyper.hyper = true;
    Run(g, 20.0, hyper);
    double from = Len(FromShip(g.systems, ship, H, bd.local));
    CHECK(speed() == 0.0 && g.flight.drive.holding && fabs(from - StopDistance(bd)) < 0.01 * bd.radius && g.flight.nearBody == d,
          "hyper at %s from 0.05 AU: stopped %.0f km above it (%.2f radii), in trading range", bd.name.c_str(),
          (from - bd.radius) / KM_U, (from - bd.radius) / bd.radius);
    Run(g, 2.0);
    CHECK(speed() == 0.0, "let go of Shift with the throttle at zero: stays put");

    // Hyper zeroes the throttle, so letting go of Shift doesn't send you into the thing under thrust
    ship.pos = bd.local + out * (0.05 * AU_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.8f;
    Face(g, bd.local - ship.pos);
    Run(g, 20.0, hyper);
    CHECK(g.flight.drive.holding && ship.throttle == 0.0f, "the same run with the throttle at 80%%: it's at zero on arrival");
    Run(g, 2.0);
    CHECK(speed() == 0.0 && fabs(Len(FromShip(g.systems, ship, H, bd.local)) - StopDistance(bd)) < 0.01 * bd.radius,
          "...so the ship stays where the drive stopped it");
    ship.pos = bd.local + out * (0.05 * AU_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.8f;
    Face(g, bd.local + side * (3.0 * StopDistance(bd)) - ship.pos);
    Run(g, 3.0, hyper);
    double inHyper = speed();
    Run(g, 2.0);
    CHECK(ship.throttle == 0.0f && inHyper > 1000.0 * APPROACH_SPEED && speed() == 0.0,
          "any hyper run does: 3 s of Shift in the open, let go, and the collapsing field has the ship stopped 2 s later");

    // ...but not if you're only passing by
    ship.pos = bd.local + out * (0.05 * AU_U); ship.vel = { 0, 0, 0 };
    Face(g, bd.local + side * (3.0 * StopDistance(bd)) - ship.pos);
    Run(g, 20.0, hyper);
    CHECK(!g.flight.drive.holding && speed() > 1000.0 * APPROACH_SPEED && Len(FromShip(g.systems, ship, H, bd.local)) > 0.05 * AU_U,
          "aimed 3 stop-distances wide of it: flies on past (%.3f AU beyond)", Len(FromShip(g.systems, ship, H, bd.local)) / AU_U);

    // A station: 3 km off, close enough to trade
    int st = -1;
    for (int i = 0; i < (int)g.loaded.bodies.size(); i++) if (g.loaded.bodies[i].kind == Kind::Station) { st = i; break; }
    const Body bs = g.loaded.bodies[st];
    ship.pos = bs.local + home.n * (20000.0 * KM_U); ship.vel = { 0, 0, 0 };
    Face(g, bs.local - ship.pos);
    Run(g, 20.0, hyper);
    Run(g, 0.1);
    from = Len(FromShip(g.systems, ship, H, bs.local));
    CHECK(speed() == 0.0 && fabs(from - StopDistance(bs)) < 20.0 && g.flight.nearBody == st && TradePartner(g),
          "hyper at %s from 20,000 km: stopped %.1f km off, in trading range", bs.name.c_str(), (from - bs.radius) / KM_U);

    // Sublight flat out at a planet: the gravity well slows you; you arrive at the surface gently
    ship.pos = bd.local + out * (0.1 * AU_U); ship.vel = { 0, 0, 0 }; ship.throttle = 1.0f;
    Face(g, bd.local - ship.pos);
    double fastest = 0.0, over = 0.0;
    for (int i = 0; i < 120 * (int)SIM_HZ; i++) {
        StepGame(g, {}, {}, (float)(1.0 / SIM_HZ));
        double up = Len(FromShip(g.systems, ship, H, bd.local)) - bd.radius;
        fastest = fmax(fastest, speed());
        over = fmax(over, speed() / fmax((double)APPROACH_SPEED, WELL_K * up));
    }
    double alt = Len(FromShip(g.systems, ship, H, bd.local)) - bd.radius;
    CHECK(fastest > 0.9 * LIGHT_SEC_U && over < 1.1 && speed() <= APPROACH_SPEED * 1.01 && alt < 50.0 * KM_U && alt > 0.0,
          "full throttle at %s from 0.1 AU: %.2f c on the way, slowed by its gravity well, at the surface doing %.1f km/s",
          bd.name.c_str(), fastest / LIGHT_SEC_U, speed() / KM_U);
}

static void TestTrade()
{
    GameState g; NewGame(g);
    int c = TradeWorld(g);
    CHECK(c >= 0, "the home system has an inhabited planet (%s)", c >= 0 ? g.loaded.bodies[c].name.c_str() : "none");
    if (c < 0) return;
    Park(g, c, 1.1); Run(g, 0.05);
    CHECK(g.flight.nearBody != c, "1.1 radii up: out of trading range");
    Park(g, c, 0.9); Run(g, 0.05);
    CHECK(g.flight.nearBody == c && TradePartner(g), "0.9 radii up: can trade with %s", g.loaded.bodies[c].name.c_str());

    Market mk = LiveMarket(g, g, g.loaded.bodies[c]);
    // Something it wants (an import if it has one), something it makes, something it doesn't deal in
    int want = -1, ex = -1, notSold = -1, none = -1;
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        if (mk.role[gd] == Role::Import && (want < 0 || mk.role[want] != Role::Import)) want = gd;
        if (want < 0 && mk.Buys(gd)) want = gd;
        if (mk.role[gd] == Role::Export && mk.Sells(gd) && (ex < 0 || mk.stock[gd] > mk.stock[ex])) ex = gd;
        if (notSold < 0 && !mk.Sells(gd)) notSold = gd;
        if (none < 0 && mk.role[gd] == Role::None) none = gd;
    }
    CHECK(want >= 0 && ex >= 0 && mk.price[want] > 0.0, "%s needs %s and makes %s", g.loaded.bodies[c].name.c_str(),
          ImportsLine(mk).c_str(), ExportsLine(mk).c_str());
    if (want < 0 || ex < 0) return;
    g.player.cargo[want] = 30;
    double cr = g.player.credits;
    double quote1 = mk.SellValue(want, 1.0);
    SellGoods(g, want, 1.0);
    double got = g.player.credits - cr;
    CHECK(got <= mk.SellPrice(want) + 1e-6 && got > 0.97 * mk.SellPrice(want), "sold 1 t %s for %.1f cr (quoted %.1f)", GoodData(want).name, got, mk.SellPrice(want));
    {
        Market now = LiveMarket(g, g, g.loaded.bodies[c]);
        double quote = now.SellValue(want, 29.0), before = g.player.credits;
        SellGoods(g, want, 1e30);
        CHECK(fabs(quote1 - got) < 0.01 * got && fabs(g.player.credits - before - quote) < 0.005 * quote && mk.Wants(want) == fmax(0.0, mk.target[want] - mk.stock[want]) && mk.target[want] > 0.0,
              "the market's estimate for the other 29 t (%.0f cr) is what they fetch (%.0f cr); it likes to hold %.0f t", quote, g.player.credits - before, mk.target[want]);
    }
    CHECK(g.player.cargo[want] == 0.0 && LiveMarket(g, g, g.loaded.bodies[c]).price[want] < mk.price[want] &&
          fabs(LiveMarket(g, g, g.loaded.bodies[c]).stock[want] - mk.stock[want] - 30.0) < 0.01,
          "sell all empties the hold, into its stock, and the price drops: %.1f to %.1f", mk.price[want], LiveMarket(g, g, g.loaded.bodies[c]).price[want]);
    mk = LiveMarket(g, g, g.loaded.bodies[c]);
    g.player.fuel = 12; g.player.credits = 100000; cr = g.player.credits;
    BuyFuel(g, 1e30);
    double paid = cr - g.player.credits;
    CHECK(mk.fuelPrice > 0.0 && fabs(g.player.fuel - 50) < 1e-9 && paid >= 38 * mk.fuelPrice - 1e-6 && paid < 38 * mk.fuelPrice * 1.5,
          "filled 38 t of fuel for %.0f cr (%.1f cr/t when asked)", paid, mk.fuelPrice);
    bool sold = SellGoods(g, want, 1.0);
    CHECK(!sold && !g.events.empty() && g.events.back().type == EventType::Notice, "selling what you don't have fails with a message");
    if (none >= 0) {
        g.player.cargo[none] = 1;
        CHECK(!SellGoods(g, none, 1.0) && g.player.cargo[none] == 1, "%s isn't traded here: no sale", GoodData(none).name);
        g.player.cargo[none] = 0;
    }

    // Buying: the price climbs as the deal goes through
    {
        mk = LiveMarket(g, g, g.loaded.bodies[c]);
        g.player.credits = 100000; cr = g.player.credits;
        bool bought = BuyGoods(g, ex, 2.0);
        paid = cr - g.player.credits;
        CHECK(bought && fabs(g.player.cargo[ex] - 2.0) < 1e-9 && paid >= 2.0 * mk.BuyPrice(ex) - 1e-6 && paid < 2.2 * mk.BuyPrice(ex),
              "bought 2 t %s for %.1f cr (quoted %.1f cr/t)", GoodData(ex).name, paid, mk.BuyPrice(ex));
        g.player.credits = 1e9;
        BuyGoods(g, ex, 1e30);
        CHECK(fabs(g.player.CargoTotal() - CARGO_CAPACITY) < 1e-6, "buy max fills the hold (%.1f t)", g.player.CargoTotal());
        CHECK(!BuyGoods(g, ex, 1.0), "a full hold can't take more");
        SellGoods(g, ex, 1e30);
    }
    if (notSold >= 0) CHECK(!BuyGoods(g, notSold, 1.0), "can't buy %s here", GoodData(notSold).name);
}

// Stations: the game starts beside one, and they trade
static void TestStations()
{
    GameState g; NewGame(g);
    Run(g, 1.0 / SIM_HZ);
    const Body *b = TradePartner(g);
    CHECK(b && b->kind == Kind::Station, "a new game starts in trading range of %s", b ? b->name.c_str() : "(nothing)");
    int stations = 0, inOrbit = 0, free = 0;
    std::vector<const StarSystem *> near = Neighbours(g, 20.0);
    for (const StarSystem *s : near) {
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        for (const Body &o : L.bodies)
            if (o.kind == Kind::Station) {
                stations++;
                (L.bodies[o.parent].kind == Kind::Star ? free : inOrbit)++;
            }
    }
    CHECK(stations >= (int)near.size() / 4 && inOrbit > 0 && free > 0,
          "%d stations across the %d systems within 20 ly (%d orbiting worlds, %d on their own)", stations, (int)near.size(), inOrbit, free);
    if (!b) return;
    Market mk = LiveMarket(g, g, *b), seen;
    CHECK(KnownMarket(g, *b, seen) && seen.price[REF_FUEL] == (float)mk.price[REF_FUEL], "docked: its prices are on record");
    int ex = -1, want = -1;
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        if (ex < 0 && mk.Sells(gd)) ex = gd;
        if (mk.Buys(gd) && gd != ex) want = gd;
    }
    if (want >= 0) g.player.cargo[want] = 5;
    double cr = g.player.credits;
    CHECK(want >= 0 && SellGoods(g, want, 1e30) && g.player.credits > cr, "%s buys %s (+%.0f cr)", b->name.c_str(),
          want >= 0 ? GoodData(want).name : "nothing", g.player.credits - cr);
    CHECK(ex >= 0 && BuyGoods(g, ex, 1.0), "...and sells %s", ex >= 0 ? GoodData(ex).name : "nothing");
}

// The habitable zone: oceans and complex life only inside it
static void TestHabitableZone()
{
    GameState g; NewGame(g);
    int liquidIn = 0, liquidOut = 0, lifeOut = 0;
    for (const StarSystem *s : Neighbours(g, 40.0)) {
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        for (const Body &o : L.bodies) {
            if (o.kind == Kind::Star || o.kind == Kind::Station) continue;
            bool liquid = o.water.rfind("Oceans", 0) == 0 || o.water == "Global ocean";
            if (liquid) (o.inHZ ? liquidIn : liquidOut)++;
            if (o.lifeLevel >= 2 && !o.inHZ) lifeOut++;
        }
    }
    CHECK(liquidIn > 0 && liquidOut == 0 && lifeOut == 0,
          "surface oceans: %d in the habitable zone, %d outside; complex life outside: %d", liquidIn, liquidOut, lifeOut);
}

// Helium-3: skim a gas giant's upper atmosphere
static void TestSkimming()
{
    GameState g; NewGame(g);
    int giant = -1;
    for (const StarSystem *s : Neighbours(g, 15.0)) {
        if (giant >= 0) break;
        if (s->id == g.systems.home) continue;                      // (somewhere else: flying there loads it)
        g.player.ship.anchor = s->id; g.player.ship.pos = {};       // at its star: that system loads
        UpdateLoadedSystem(g);
        for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
            if (g.loaded.bodies[i].kind == Kind::Planet && g.loaded.bodies[i].massEarth >= GAS_GIANT_MASS) { giant = i; break; }
    }
    CHECK(giant >= 0, "found a gas giant to skim (%s)", giant >= 0 ? g.loaded.bodies[giant].name.c_str() : "none");
    if (giant < 0) return;
    Park(g, giant, 0.15); Run(g, 2.0);
    CHECK(g.flight.skimming && fabs(g.player.cargo[RAW_HELIUM3] - 2.0 * SKIM_RATE) < 0.01,
          "2 s in the upper atmosphere: %.2f t helium-3", g.player.cargo[RAW_HELIUM3]);
    Park(g, giant, 0.5); Run(g, 0.5);
    CHECK(!g.flight.skimming, "higher up: no skimming");
    Park(g, giant, 0.15);
    g.player.cargo[ORE_IRON] = CARGO_CAPACITY - g.player.CargoTotal();
    Run(g, 0.5);
    CHECK(!g.flight.skimming && g.flight.skimFull && g.player.CargoTotal() <= CARGO_CAPACITY + 1e-9, "a full hold stops skimming");
}

static void TestComms()
{
    GameState g; NewGame(g);
    const SysId H = g.systems.home;
    const StarSystem &home = g.systems[H];
    int c = TradeWorld(g);
    if (c < 0) { CHECK(false, "no inhabited planet to hail"); return; }
    const Body &b = g.loaded.bodies[c];
    g.player.ship.pos = b.local + Norm(home.u) * (b.radius + 200000 * KM_U);
    g.player.ship.vel = { 0, 0, 0 };
    Run(g, 0.05);
    auto list = CommsInRange(g);
    const CommsContact *cc = FindContact(list, TargetId::MakeBody(H, c));
    int everyone = 0;
    for (const Body &o : g.loaded.bodies) if (o.population > 0) everyone++;
    CHECK(cc && (int)list.size() == everyone && everyone > 3 && list.back().dist > 10.0 * LIGHT_SEC_U,
          "comms reach all %d inhabited places in the system (%s: %.2f ls; the farthest: %.0f ls)", everyone, b.name.c_str(),
          cc ? cc->dist / LIGHT_SEC_U : 0.0, list.back().dist / LIGHT_SEC_U);
    if (!cc) return;
    double rt = 2.0 * cc->dist / LIGHT_SEC_U;
    const TargetId WITH = TargetId::MakeBody(H, c);
    Hail(g, c);
    CHECK(g.comms.Find(WITH) && CommsWaiting(g, g, WITH) && CommsInFlight(g, g).size() == 1, "hailing: reply on its way");
    Run(g, rt + 0.5);
    CHECK(CommsWaiting(g, g, WITH), "no reply before the round trip (%.2f s) + 0.6 s", rt);
    Run(g, 0.2);
    CHECK(!CommsWaiting(g, g, WITH) && CommsInFlight(g, g).empty(), "reply arrived: \"%s\"", g.comms.Find(WITH)->log.back().text.c_str());
    Hail(g, c);
    CHECK(g.comms.channels.size() == 1 && g.comms.channels[0].log.size() == 2, "hailing someone you're already talking to does nothing");

    // Requests don't wait for each other, on one channel or across several
    RequestMarketReport(g, c);
    RequestLocalInfo(g, c);
    const CommsContact far0 = CommsInRange(g).back();
    Hail(g, far0.body);
    std::vector<CommsFlight> flights = CommsInFlight(g, g);
    CHECK(flights.size() == 3 && g.comms.channels.size() == 2 && flights[0].with == WITH && flights[2].with == TargetId::MakeBody(H, far0.body) &&
          flights[2].at - g.clock > 100.0 * (flights[0].at - g.clock),
          "three requests in flight at once on two channels: replies due in %.1f s, %.1f s and %s",
          flights[0].at - g.clock, flights[1].at - g.clock, FormatETA(flights[2].at - g.clock).c_str());
    double half = 0.5 * (flights[2].at - flights[2].sent);
    CHECK(flights[2].Progress(g.clock) < 0.01 && flights[2].Progress(g.clock + half * 0.9) < 0.5 && flights[2].Progress(g.clock + half * 1.1) > 0.5,
          "a flight's progress: outbound for the first half of the wait, inbound for the second");
    Run(g, rt + 1.5);
    CHECK(CommsInFlight(g, g).size() == 1 && g.comms.Find(WITH)->log.size() > 6, "the near replies are in; the far one is still out");
    HangUp(g, far0.body);
    CHECK(g.comms.channels.size() == 1 && CommsInFlight(g, g).empty(), "hanging up on the far one leaves the other channel alone");

    // Trade is opened over the channel, once you're in docking range
    CHECK(CommsAnswered(g, g, WITH) && !CanTradeOverComms(g), "200,000 km out: on the channel, but too far to trade");
    Park(g, c, 0.9); Run(g, 0.05);
    CHECK(CanTradeOverComms(g), "0.9 radii up, same channel: trade can be opened");
    HangUp(g, c);
    CHECK(!CanTradeOverComms(g), "no channel, no trade");

    // F hails whoever the nose is on, or whoever you're docked beside
    CHECK(HailTarget(g) == c, "docked and pointing at nothing: F hails %s", b.name.c_str());
    int station = -1, empty = -1;
    for (int i = 0; i < (int)g.loaded.bodies.size(); i++) {
        const Body &o = g.loaded.bodies[i];
        if (o.kind == Kind::Station && o.parent != c && station < 0) station = i;
        if (o.kind == Kind::Planet && o.population <= 0 && empty < 0) empty = i;
    }
    Aim at; at.hovered = TargetId::MakeBody(H, station);
    Run(g, 0.05, {}, at);
    CHECK(HailTarget(g) == station, "pointing at %s: F hails that instead", g.loaded.bodies[station].name.c_str());
    at.hovered = TargetId::MakeBody(H, empty);
    Run(g, 0.05, {}, at);
    CHECK(HailTarget(g) == c, "pointing at somewhere uninhabited: back to who you're docked beside");
    g.player.ship.pos = b.local + Norm(home.u) * (b.radius + 200000 * KM_U);
    Run(g, 0.05, {}, at);
    CHECK(HailTarget(g) == -1, "...and away from everyone, F has nobody to hail (it opens the list)");

    // A hail across the system takes its time
    const CommsContact far = CommsInRange(g).back();
    const TargetId FAR = TargetId::MakeBody(H, far.body);
    Hail(g, far.body);
    Run(g, 5.0);
    CHECK(CommsWaiting(g, g, FAR) && !CommsAnswered(g, g, FAR), "hailing %s, %.0f light seconds away: still waiting after 5 s",
          g.loaded.bodies[far.body].name.c_str(), far.dist / LIGHT_SEC_U);
    g.clock += 2.0 * far.dist / LIGHT_SEC_U;
    g.events.clear();
    Run(g, 0.05);
    bool said = false;
    for (auto &e : g.events) if (e.text.find(g.loaded.bodies[far.body].name + " has replied") == 0) said = true;
    CHECK(CommsAnswered(g, g, FAR) && said, "...and answered once light has made the round trip (%s), with a word to say so",
          FormatETA(2.0 * far.dist / LIGHT_SEC_U).c_str());
    Hail(g, c);

    // Leaving the system drops the channel
    g.events.clear();
    g.player.ship.pos = home.n * (0.7 * LY_U);
    Run(g, 1.0 / SIM_HZ);
    bool lost = false;
    for (auto &e : g.events) if (e.type == EventType::CommsLost) lost = true;
    CHECK(lost && g.comms.channels.empty(), "leaving the system drops every channel");
}

static double AngleTo(const GameState &g, DVec3 rel)
{
    DVec3 fwd = ToD(ShipForward(g.player.ship)), want = Norm(rel);
    return atan2(Len(Cross(fwd, want)), Dot(fwd, want)) * RAD2DEG;
}

static void TestTargeting()
{
    GameState g; NewGame(g);
    const SysId H = g.systems.home;
    const StarSystem &home = g.systems[H];
    int c = BodyIndex(g, home.name + " c"), d = BodyIndex(g, home.name + " d");
    Park(g, c, 2.0);
    Aim aim; aim.hovered = TargetId::MakeBody(H, d);
    Run(g, 0.05, {}, aim);
    CHECK(g.flight.selected == aim.hovered, "selection follows the reticle");
    Run(g, 0.05, {}, Aim{});
    CHECK(!g.flight.selected.valid(), "reticle off the body: deselected");

    // Looking at something doesn't lock it: Shift alone flies wherever the nose points
    Controls hyper; hyper.hyper = true;
    Run(g, 0.2, hyper, aim);
    CHECK(!g.flight.lock.valid() && g.flight.selected == aim.hovered, "Shift with nothing locked: no lock, whatever the reticle is on");
    g.player.ship.vel = { 0, 0, 0 };
    Park(g, c, 2.0);
    // Locked (SPACE or the middle button on what's under the reticle): it stays locked wherever the pilot looks
    Aim locked; locked.course = TargetId::MakeBody(H, d);
    Run(g, 0.05, {}, locked);
    CHECK(g.flight.lock == locked.course && g.flight.selected == locked.course, "a destination locked stays locked with the reticle off it, Shift or no Shift");
    Aim elsewhere = locked; elsewhere.hovered = TargetId::MakeBody(H, c);
    Run(g, 0.05, {}, elsewhere);
    CHECK(g.flight.lock == locked.course && g.flight.selected == elsewhere.hovered, "looking at something else selects it; the lock is still the destination");
    // Under drive the nose is held on it: the mouse does nothing until the lock is released
    Controls jerk = hyper; jerk.yaw = 10.0f;
    Run(g, 1.0, hyper, locked);
    DVec3 to;
    FindTarget(g, locked.course, to);
    double before = AngleTo(g, to);
    Run(g, 0.2, jerk, locked);
    FindTarget(g, locked.course, to);
    CHECK(g.flight.lock == locked.course && before < 3.0 && AngleTo(g, to) < 3.0, "under drive the nose is held on the lock (%.2f degrees off it), mouse or no mouse", AngleTo(g, to));
    // Released: free flight, and the mouse steers again
    Run(g, 0.2, jerk, Aim{});
    FindTarget(g, locked.course, to);
    CHECK(!g.flight.lock.valid() && AngleTo(g, to) > 20.0, "released, there's no lock and the ship goes where it's steered (%.0f degrees off the old destination)", AngleTo(g, to));
    g.player.ship.vel = { 0, 0, 0 };
    Park(g, c, 2.0);
    Run(g, 0.1, {}, Aim{});

    // Stars: the loaded system's star is a Body; far away it's a Star again
    TargetId star = CanonicalTarget(g, TargetId::MakeStar(H));
    CHECK(star == TargetId::MakeBody(H, 0), "a loaded system's star is its first body");
    DVec3 rel; double r;
    const StarSystem *far = nullptr;
    for (const StarSystem *s : Neighbours(g, 12.0)) if (s->id != H) far = s;
    CHECK(far && FindTarget(g, TargetId::MakeStar(far->id), rel, &r) && TargetName(g, TargetId::MakeStar(far->id)) == far->name,
          "distant star %s found at %.2f ly", far ? far->name.c_str() : "?", Len(rel) / LY_U);
    CHECK(!FindTarget(g, TargetId::MakeStar(CellId(0, 2000, 0)), rel) && !FindTarget(g, TargetId::MakeStar(-5), rel),
          "a system that doesn't exist can't be targeted");
}

// The galaxy: the size of a real one, the same every time, and exact where it matters
static void TestGalaxy()
{
    GameState g; NewGame(g);
    const Galaxy &gal = g.systems;
    const SysId H = gal.home;
    DVec3 hp = gal.PositionLy(H);
    CHECK(gal.Exists(H) && fabs(sqrt(hp.x * hp.x + hp.z * hp.z) - HOME_RADIUS_LY) < 1000.0 && fabs(hp.y) < 300.0,
          "home (%s) is %.0f ly from the galactic center, %.0f ly above the disk", gal[H].name.c_str(), sqrt(hp.x * hp.x + hp.z * hp.z), hp.y);

    // The same seed is the same galaxy; another seed is another
    Galaxy again, other;
    again.Init(g.seed);
    other.Init(g.seed + 1);
    std::vector<const StarSystem *> a = Neighbours(g, 30.0), b;
    again.Nearby(again.home, {}, 30.0 * LY_U, b);
    bool same = again.home == H && a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); i++)
        same = a[i]->id == b[i]->id && a[i]->name == b[i]->name && a[i]->seed == b[i]->seed && a[i]->stars.size() == b[i]->stars.size() &&
               a[i]->stars[0].mass == b[i]->stars[0].mass && Len(a[i]->off - b[i]->off) == 0.0 && a[i]->fieldR == b[i]->fieldR;
    CHECK(same && other.home != H, "the same seed rebuilds the same %d systems within 30 ly; another seed doesn't", (int)a.size());

    // As crowded as the Sun's neighbourhood, and in the real proportions
    std::vector<const StarSystem *> near = Neighbours(g, 60.0);
    double density = near.size() / (4.0 / 3.0 * PI * 60.0 * 60.0 * 60.0);
    int red = 0, sunlike = 0, dead = 0, brown = 0, multi = 0;
    double closest = 1e30;
    std::vector<std::string> names;
    for (const StarSystem *s : near) {
        const StarInfo &st = s->stars[0];
        if (st.kind == StarKind::MainSequence && st.cls == 'M') red++;
        if (st.kind == StarKind::MainSequence && st.cls == 'G') sunlike++;
        if (st.kind == StarKind::WhiteDwarf || st.kind == StarKind::NeutronStar) dead++;
        if (st.kind == StarKind::BrownDwarf) brown++;
        if (s->stars.size() > 1) multi++;
        names.push_back(s->name);
    }
    for (size_t i = 0; i < near.size() && i < 600; i++)
        for (size_t j = i + 1; j < near.size(); j++)
            closest = fmin(closest, Len(gal.Between(near[i]->id, near[j]->id)));
    std::sort(names.begin(), names.end());
    bool unique = std::adjacent_find(names.begin(), names.end()) == names.end();
    double n = (double)near.size();
    CHECK(density > 0.0015 && density < 0.006, "%d systems within 60 ly: %.4f per cubic light year (the Sun's neighbourhood: ~0.003)", (int)near.size(), density);
    CHECK(red / n > 0.55 && red / n < 0.75 && sunlike / n > 0.03 && sunlike / n < 0.12 && dead > 0 && brown > 0,
          "the stars: %.0f%% red dwarfs, %.0f%% Sun-like, %.0f%% white dwarfs and neutron stars, %.0f%% brown dwarfs; %.0f%% multiple",
          100 * red / n, 100 * sunlike / n, 100 * dead / n, 100 * brown / n, 100 * multi / n);
    CHECK(unique && closest >= 0.99 * LY_U, "no two share a name; the closest pair is %.2f ly apart", closest / LY_U);

    // Exact arithmetic between neighbours: going round a triangle of stars gets you back exactly
    const StarSystem *p = near[near.size() / 3], *q = near[2 * near.size() / 3];
    DVec3 round = gal.Between(H, p->id) + gal.Between(p->id, q->id) + gal.Between(q->id, H);
    CHECK(Len(round) == 0.0 && Len(gal.Between(H, p->id) + gal.Between(p->id, H)) == 0.0,
          "star-to-star vectors are exact (%s -> %s -> %s -> home: off by %.1e units)", gal[H].name.c_str(), p->name.c_str(), q->name.c_str(), Len(round));

    // Human space: one bubble around home, and nobody beyond it
    double inside = 0, outside = 0, farthest = 0;
    int inhabited = 0, core = 0, named = 0;
    for (const StarSystem *s : Neighbours(g, 100.0)) {
        double d = Len(gal.Between(H, s->id)) / LY_U;
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        double pop = 0;
        for (const Body &o : L.bodies) { pop += o.population; if (o.settlement == Settlement::CoreWorld) core++; }
        if (pop > 0) { inhabited++; farthest = fmax(farthest, d); }
        (s->develop > 0.0f ? inside : outside) += pop;
        if (s->named) named++;
    }
    CHECK(inside > 1e9 && outside == 0.0 && farthest < FRONTIER_RADIUS_LY * 1.2 && farthest > CORE_RADIUS_LY && core >= 1 && gal[H].develop == 1.0f,
          "human space: %s people in %d systems (%d core worlds), the farthest %.0f ly from home; none beyond",
          FormatPopulation(inside).c_str(), inhabited, core, farthest);

    // The far side of the galaxy is there too: find a system 50,000 ly away and build it
    const StarSystem *farSys = nullptr;
    int fx = (int)(-HOME_RADIUS_LY * 0.92 / CELL_LY), fz = (int)(0.3 * HOME_RADIUS_LY / CELL_LY);
    for (int i = 0; i < 4000 && !farSys; i++) farSys = gal.Find(CellId(fx + i % 20, i / 400, fz + (i / 20) % 20));
    LoadedSystem L;
    if (farSys) GenerateSystemContents(*farSys, L);
    double across = farSys ? Len(gal.Between(H, farSys->id)) / LY_U : 0.0;
    CHECK(farSys && across > 45000.0 && farSys->develop == 0.0f && !farSys->named && !L.bodies.empty() &&
          GalaxyDensity({ 0, 0, 0 }) > 10.0 * GalaxyDensity(hp) && GalaxyDensity({ 70000, 0, 0 }) == 0.0,
          "across the galaxy: %s, %.0f ly from home, %d bodies, uncharted", farSys ? farSys->name.c_str() : "nothing", across, (int)L.bodies.size());

    // Flying to the next star: the ship re-anchors on it without moving, and its system loads
    const StarSystem *next = nullptr;
    for (const StarSystem *s : Neighbours(g, 12.0))
        if (s->id != H && (!next || Len(gal.Between(H, s->id)) < Len(gal.Between(H, next->id)))) next = s;
    CHECK(next != nullptr, "home has a neighbour within 12 ly");
    if (!next) return;
    DVec3 way = gal.Between(H, next->id);
    g.player.ship.anchor = H; g.player.ship.pos = way * 0.7; g.player.ship.vel = { 0, 0, 0 };
    UpdateLoadedSystem(g);
    double moved = Len(FromShip(gal, g.player.ship, H) + way * 0.7);
    CHECK(g.player.ship.anchor == next->id && moved < 1e-3 && g.loaded.index < 0,
          "70%% of the way to %s (%.1f ly): anchored on it, moved %.1e units, nothing loaded", next->name.c_str(), Len(way) / LY_U, moved);
    g.player.ship.pos = next->n * (0.2 * LY_U);
    UpdateLoadedSystem(g);
    CHECK(g.loaded.index == next->id && !g.loaded.bodies.empty() && g.loaded.bodies[0].name.rfind(next->name, 0) == 0,
          "0.2 ly from %s: its system loads (%d bodies)", next->name.c_str(), (int)g.loaded.bodies.size());

    // Sectors are forgotten once nobody is near them
    Galaxy tour;
    tour.Init(g.seed);
    for (int i = 0; i < 400; i++) {
        std::vector<const StarSystem *> seen;
        tour.Nearby(tour.home, { i * 60.0 * LY_U, 0, 0 }, 30.0 * LY_U, seen);
        for (int k = 0; k < 300; k++) tour.Trim();
    }
    CHECK(tour.CachedSectors() < 12000 && tour.Exists(tour.home), "a 24,000 ly flight leaves %d sectors in memory", tour.CachedSectors());
}

// Planetary systems: built the way real ones are
static void TestSystems()
{
    GameState g; NewGame(g);
    int systems = 0, planets = 0, giants = 0, giantsInside = 0, moons = 0, moonsOutside = 0, crowded = 0, dwarfs = 0, belts = 0;
    int youngLife = 0, redGiants = 0, planetsAtHot = 0, hotStars = 0, planetsAtCool = 0, coolStars = 0, badBodies = 0;
    std::vector<uint64_t> ids;
    for (const StarSystem *s : Neighbours(g, 60.0)) {
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        systems++;
        const StarInfo &st = s->stars[0];
        double frost = 2.7 * sqrt(s->lumInner) * AU_U, innerMass = 0.0;
        for (size_t i = 0; i < s->stars.size(); i++) if (i == 0 || s->stars[i].close) innerMass += s->stars[i].mass;
        bool plain = st.kind == StarKind::MainSequence;            // (dead stars' planets have moved)
        double last = 0.0;
        int count = 0;
        for (const Body &b : L.bodies) {
            ids.push_back(b.id);
            if (b.kind == Kind::Star || b.kind == Kind::Station) continue;
            if (!(b.massEarth > 0.0) || !(b.radius > 0.0) || !(b.gravity > 0.0) || b.type.empty() || !(b.tempK > 0.0) ||
                Len(b.local) > s->fieldR) badBodies++;
            if (s->age < 0.6 && b.lifeLevel > 0) youngLife++;
            if (b.kind == Kind::Moon) {
                const Body &host = L.bodies[b.parent];
                moons++;
                if (b.orbit > 0.5 * host.orbit * cbrt(host.massEarth * 3.003e-6 / (3.0 * innerMass)) || b.orbit < host.radius * 2.0) moonsOutside++;
                continue;
            }
            if (b.dwarf) { dwarfs++; continue; }
            planets++; count++;
            if (last > 0.0 && b.orbit < last * 1.3) crowded++;
            last = b.orbit;
            if (b.giant && b.massEarth >= GIANT_MASS) { giants++; if (plain && b.orbit < frost && count > 1) giantsInside++; }   // (a hot Jupiter is always innermost)
        }
        for (const Zone &z : L.zones) if (!z.ring) belts++;
        if (st.kind == StarKind::Giant) redGiants++;
        if (plain && st.mass > 1.4) { hotStars++; planetsAtHot += count; }
        if (plain && st.mass < 0.45) { coolStars++; planetsAtCool += count; }
    }
    std::sort(ids.begin(), ids.end());
    bool uniqueIds = std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    CHECK(badBodies == 0 && crowded == 0 && uniqueIds, "%d planets, %d dwarf planets, %d moons and %d belts around %d stars: all sound, "
          "none crowding the next, every body id unique", planets, dwarfs, moons, belts, systems);
    CHECK(giants > systems / 20 && giantsInside == 0, "%d giants, all beyond their star's frost line (bar the few that migrated right in)", giants);
    CHECK(moons > 0 && moonsOutside == 0, "every moon orbits inside its planet's Hill sphere");
    CHECK(youngLife == 0, "nothing lives in systems under 600 million years old");
    CHECK(coolStars > 0 && planets > 2 * systems, "%.1f planets per system (red dwarfs %.1f, stars over 1.4 Suns %.1f)", (double)planets / systems,
          (double)planetsAtCool / coolStars, hotStars ? (double)planetsAtHot / hotStars : 0.0);

    // Home is somewhere worth starting
    const StarSystem &home = g.systems[g.systems.home];
    bool core = false, refinery = false, giant = false;
    for (const Body &b : g.loaded.bodies) {
        if (b.settlement == Settlement::CoreWorld) core = true;
        if (b.settlement == Settlement::OreRefinery) refinery = true;
        if (b.giant && b.massEarth >= GIANT_MASS) giant = true;
    }
    CHECK(core && refinery && giant && home.stars[0].kind == StarKind::MainSequence && home.age > 3.0,
          "home: a %s, %.1f billion years old, with a core world, a refinery and a giant to skim", StarDescription(home.stars[0]), home.age);

    // Markets belong to bodies, not names: the same body always has the same market
    int tw = TradeWorld(g);
    if (tw >= 0) {
        EnsureEconomy(g);
        Body copy = g.loaded.bodies[tw];
        copy.name = "Renamed";
        Market m1 = LiveMarket(g, g, g.loaded.bodies[tw]), m2 = LiveMarket(g, g, copy);
        copy.id ^= 1;
        Market m3 = LiveMarket(g, g, copy);
        CHECK(m1.price[REF_FOOD] > 0.0 && m1.price[REF_FOOD] == m2.price[REF_FOOD] && m3.price[REF_FOOD] == 0.0,
              "a market belongs to the body's id, not its name");
    }
}

// Charts, courses and the maps
static void TestMaps()
{
    GameState g; NewGame(g);
    const Galaxy &gal = g.systems;
    const SysId H = gal.home;
    const StarSystem &home = gal[H];

    // Home is charted from the start: the map lists everything, moons and stations under their worlds
    std::vector<MapRow> rows = SystemMapRows(g);
    int unknown = 0, nested = 0, stations = 0, misplaced = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const MapRow &r = rows[i];
        if (!r.known) unknown++;
        if (r.indent > 0) nested++;
        if (r.target.type != TargetId::Body) continue;
        const Body &b = g.loaded.bodies[r.target.index];
        if (b.kind == Kind::Station) stations++;
        if (b.kind == Kind::Moon || (b.kind == Kind::Station && b.parent != 0)) {      // must sit below its parent, indented
            bool under = false;
            for (size_t k = i; k-- > 0;)
                if (rows[k].indent < r.indent) { under = rows[k].target == TargetId::MakeBody(H, b.parent); break; }
            if (!under) misplaced++;
        }
    }
    int allStations = 0;
    for (const Body &b : g.loaded.bodies) if (b.kind == Kind::Station) allStations++;
    CHECK(unknown == 0 && nested > 0 && stations == allStations && misplaced == 0 && rows[0].name == g.loaded.bodies[0].name,
          "home's map: %d rows, all charted, %d moons and stations nested under their worlds", (int)rows.size(), nested);

    // A course from the map: it's the selection, the nose swings onto it, and Shift locks on
    int d = BodyIndex(g, home.name + " d");
    const Body bd = g.loaded.bodies[d];
    Park(g, BodyIndex(g, home.name + " c"), 2.0);
    Face(g, home.n);
    Aim aim; aim.course = TargetId::MakeBody(H, d); aim.steer = true;
    Run(g, 2.0, {}, aim);
    Vector3 to = ToF(Norm(bd.local - ShipLocal(g)));
    CHECK(g.flight.selected == aim.course && Vector3DotProduct(ShipForward(g.player.ship), to) > 0.99999f,
          "course set for %s: selected, and 2 s later the nose is on it", bd.name.c_str());
    aim.steer = false;
    Controls hyper; hyper.hyper = true;
    Run(g, 0.2, hyper, aim);
    CHECK(g.flight.lock == aim.course, "Shift locks onto the course");
    Run(g, 25.0, hyper, aim);
    CHECK(g.flight.drive.holding && g.flight.nearBody == d, "...and takes the ship there");
    Aim other = aim; other.hovered = TargetId::MakeBody(H, 0);
    Run(g, 0.1, {}, other);
    CHECK(g.flight.selected == other.hovered, "whatever's under the reticle still comes first");

    // Another system: no charts, so only what can be seen from afar
    const StarSystem *next = nullptr;
    for (const StarSystem *s : Neighbours(g, 20.0)) {
        if (s->id == H) continue;
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        int planets = 0; bool people = false, moons = false;
        for (const Body &b : L.bodies) { if (b.kind == Kind::Planet && b.population > 0) people = true; if (b.kind == Kind::Planet) planets++;
                                         if (b.kind == Kind::Moon) moons = true; }
        if (people && moons && planets >= 3) { next = s; break; }
    }
    CHECK(next != nullptr, "an inhabited neighbour to visit (%s)", next ? next->name.c_str() : "none");
    if (!next) return;
    g.player.ship.anchor = next->id; g.player.ship.pos = next->n * (0.5 * next->fieldH); g.player.ship.vel = { 0, 0, 0 };
    g.player.ship.throttle = 0.0f;
    Run(g, 0.05);
    UpdateKnowledge(g, g);
    rows = SystemMapRows(g);
    int planets = 0, known = 0, deep = 0;
    for (const MapRow &r : rows) {
        if (r.target.type != TargetId::Body) continue;
        const Body &b = g.loaded.bodies[r.target.index];
        if (b.kind == Kind::Planet) { planets++; if (r.known) known++; }
        if (b.kind == Kind::Moon || b.kind == Kind::Station) deep++;
    }
    CHECK(g.flight.drive.inField && planets >= 3 && known == 0 && deep == 0 && !g.know.charted.count(next->id),
          "arriving at %s: %d planets on the map, none scanned, no moons or stations shown", next->name.c_str(), planets);

    // The info panel and the hover line keep an unscanned world's secrets too
    auto has = [](const std::vector<InfoRow> &info, const char *label) {
        for (const InfoRow &r : info) if (r.label == label) return true;
        return false;
    };
    int secret = -1, base = -1;
    for (int i = 0; i < (int)g.loaded.bodies.size(); i++) {
        const Body &o = g.loaded.bodies[i];
        if (o.kind == Kind::Planet && o.population > 0 && secret < 0) secret = i;
        if (o.kind == Kind::Station && base < 0) base = i;
    }
    std::vector<InfoRow> info = TargetInfo(g, TargetId::MakeBody(next->id, secret));
    std::vector<InfoRow> star = TargetInfo(g, TargetId::MakeBody(next->id, 0));
    std::string hoverLine = WorldSummary(g.loaded.bodies[secret], Known(g, secret));
    bool stationHidden = base < 0;
    if (base >= 0) {
        std::vector<InfoRow> st = TargetInfo(g, TargetId::MakeBody(next->id, base));
        stationHidden = has(st, "Survey") && !has(st, "Population") && !has(st, "Economy") && st[0].value == "Station";
    }
    CHECK(has(info, "Survey") && has(info, "Orbit") && has(info, "Radius") && !has(info, "Settlement") && !has(info, "Atmosphere") &&
          !has(info, "Temperature") && !has(info, "Life") && !has(info, "Resources") && !has(info, "Economy") && !has(info, "Moons") &&
          hoverLine.find("unscanned") != std::string::npos && hoverLine.find("pop") == std::string::npos && stationHidden &&
          has(star, "Planets") && !has(star, "Population") && !has(star, "Stations"),
          "unscanned %s: the info panel gives its size and orbit and nothing else (\"%s\")", g.loaded.bodies[secret].name.c_str(), hoverLine.c_str());

    // Fly close to a world: it's scanned, and its moons and stations appear
    int world = -1;
    for (int i = 0; i < (int)g.loaded.bodies.size() && world < 0; i++) {
        if (g.loaded.bodies[i].kind != Kind::Planet) continue;
        for (const Body &m : g.loaded.bodies) if (m.kind == Kind::Moon && m.parent == i) world = i;
    }
    Park(g, world, 1.0);
    Run(g, 0.05);
    g.events.clear();
    UpdateKnowledge(g, g);
    bool told = false;
    for (const GameEvent &e : g.events) if (e.text.find("Scanned " + g.loaded.bodies[world].name) == 0) told = true;
    rows = SystemMapRows(g);
    int shown = 0; bool worldKnown = false;
    for (const MapRow &r : rows)
        if (r.target.type == TargetId::Body) {
            if (r.target.index == world) worldKnown = r.known;
            if (g.loaded.bodies[r.target.index].parent == world) shown++;
        }
    CHECK(told && worldKnown && shown > 0 && Known(g, world) && !g.know.charted.count(next->id),
          "one radius above %s: scanned; its %d moons and stations are on the map now", g.loaded.bodies[world].name.c_str(), shown);

    // Hail someone who lives here: they send the lot
    int tw = TradeWorld(g);
    const Body &b = g.loaded.bodies[tw];
    Park(g, tw, 0.9);
    Run(g, 0.05);
    Hail(g, tw);
    UpdateKnowledge(g, g);
    bool early = g.know.charted.count(next->id) > 0;
    Run(g, 3.0);
    UpdateKnowledge(g, g);
    rows = SystemMapRows(g);
    unknown = 0;
    for (const MapRow &r : rows) if (!r.known) unknown++;
    CHECK(!early && g.know.charted.count(next->id) && unknown == 0, "%s answered the hail: the whole system is charted (%d rows)",
          b.name.c_str(), (int)rows.size());
    info = TargetInfo(g, TargetId::MakeBody(next->id, secret));
    star = TargetInfo(g, TargetId::MakeBody(next->id, 0));
    CHECK(!has(info, "Survey") && has(info, "Settlement") && has(info, "Atmosphere") && has(info, "Resources") && has(star, "Population"),
          "...and the info panel has everything on %s now", g.loaded.bodies[secret].name.c_str());

    // Charts are kept with the save
    CHECK(SaveGame(g, "test_charts.txt"), "saved");
    GameState h; NewGame(h);
    CHECK(LoadGame(h, "test_charts.txt") && h.know.charted == g.know.charted && h.know.scanned == g.know.scanned &&
          h.know.charted.count(H) && h.know.charted.size() == 2, "charts and scans round-trip through a save");
    remove("test_charts.txt");

    // Attitude against the ecliptic
    float pitch, roll;
    g.player.ship.rot = QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, ToF(next->u), ToF(next->n))));
    PlaneAttitude(g.player.ship, next->n, pitch, roll);
    bool level = fabsf(pitch) < 0.1f && fabsf(roll) < 0.1f;
    Controls nose; nose.pitch = 30.0f / (float)SIM_HZ;
    Run(g, 1.0, nose);
    PlaneAttitude(g.player.ship, next->n, pitch, roll);
    bool climbed = fabsf(pitch - 30.0f) < 0.5f && fabsf(roll) < 0.5f;
    g.player.ship.rot = QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, ToF(next->u), ToF(next->n))));
    Controls bank; bank.roll = -40.0f / (float)SIM_HZ;                       // E: roll right
    Run(g, 1.0, bank);
    PlaneAttitude(g.player.ship, next->n, pitch, roll);
    CHECK(level && climbed && fabsf(roll - 40.0f) < 0.5f && fabsf(pitch) < 0.5f,
          "attitude: level on the ecliptic reads 0 / 0; nose up 30 reads pitch +30; rolled right 40 reads roll %+.0f", roll);
}

// Factions: who holds human space, generated from the seed
static void TestFactions()
{
    GameState g; NewGame(g);
    const Galaxy &gal = g.systems;
    const FactionSet &fs = gal.factions;
    int kinds[5] = {};
    for (const Faction &f : fs.list) kinds[(int)f.kind]++;
    CHECK(fs.Count() > 20 && fs.list[0].kind == FactionKind::FreePort && kinds[(int)FactionKind::Government] >= 5 &&
          kinds[(int)FactionKind::Government] <= 7 && kinds[(int)FactionKind::Conglomerate] >= 5 && kinds[(int)FactionKind::Conglomerate] <= 6 &&
          kinds[(int)FactionKind::Guild] == 4 && kinds[(int)FactionKind::Clan] >= 8,
          "%d factions: the free port, %d governments, %d conglomerates, %d guilds, %d pirate clans", fs.Count(),
          kinds[(int)FactionKind::Government], kinds[(int)FactionKind::Conglomerate], kinds[(int)FactionKind::Guild], kinds[(int)FactionKind::Clan]);

    // The same seed, the same powers; another seed, others
    Galaxy again, other;
    again.Init(g.seed);
    other.Init(g.seed + 1);
    bool same = again.factions.Count() == fs.Count();
    for (int i = 0; same && i < fs.Count(); i++)
        same = again.factions.list[i].name == fs.list[i].name && again.factions.list[i].seat == fs.list[i].seat &&
               again.factions.list[i].axis[0] == fs.list[i].axis[0] && again.factions.Relation(i, 1) == fs.Relation(i, 1);
    std::string a = fs.list[1].name, b = other.factions.Count() > 1 ? other.factions.list[1].name : "";
    CHECK(same && a != b, "the same seed rebuilds them; another seed has others (%s / %s)", a.c_str(), b.c_str());

    // Governments: well mixed, with distinct names
    double closest = 1e30;
    std::vector<std::string> names;
    std::string roll;
    for (const Faction &f : fs.list) {
        names.push_back(f.name);
        if (f.kind != FactionKind::Government) continue;
        roll += (roll.empty() ? "" : "; ") + f.name + " (" + IdeologyBlurb(f) + ")";
        for (const Faction &o : fs.list) {
            if (o.kind != FactionKind::Government || o.id <= f.id) continue;
            double dist = 0.0;
            for (int ax = 0; ax < AX_COUNT; ax++) dist += (f.axis[ax] - o.axis[ax]) * (f.axis[ax] - o.axis[ax]);
            closest = fmin(closest, sqrt(dist));
        }
    }
    std::sort(names.begin(), names.end());
    CHECK(closest > 0.7 && std::adjacent_find(names.begin(), names.end()) == names.end(),
          "no two governments think alike (closest pair %.2f apart of a possible 4), no two factions share a name", closest);
    printf("        %s\n", roll.c_str());

    // Territory: home is the free port; settled space is all somebody's; the fringe is nobody's; shares are comparable
    std::vector<int> held(fs.Count(), 0);
    int claimed = 0, unclaimedSettled = 0, fringe = 0, fringeClaimed = 0, outside = 0;
    for (const StarSystem *s : Neighbours(g, 90.0)) {
        if (s->ruler != NO_FACTION) { held[s->ruler]++; claimed++; }
        if (s->develop >= 0.3f && s->claim == NO_FACTION) unclaimedSettled++;       // (well inside human space, and in nobody's territory)
        if (s->develop > 0.0f && s->develop < 0.1f) { fringe++; if (s->ruler != NO_FACTION) fringeClaimed++; }
        if (s->develop <= 0.0f && s->ruler != NO_FACTION) outside++;
    }
    int least = 1 << 30, most = 0;
    bool seated = true;
    for (const Faction &f : fs.list) {
        if (f.kind != FactionKind::Government) continue;
        least = std::min(least, held[f.id]); most = std::max(most, held[f.id]);
        if (gal[f.seat].ruler != f.id) seated = false;
    }
    CHECK(gal[gal.home].ruler == 0 && held[0] == 1 && unclaimedSettled == 0 && fringe > 50 && fringeClaimed == 0 && outside == 0 && seated,
          "%d systems are somebody's: a government has people there (those inside its borders that nobody lives in are unclaimed); the free port holds only home; the fringe (%d systems) and beyond are nobody's", claimed, fringe);
    CHECK(least > 100 && most < 4 * least, "the governments hold between %d and %d systems each", least, most);

    // Ownership: everything inhabited at home is the free port's; elsewhere states, companies, guilds and nobody all own things,
    // and a conglomerate's stations sit in more than one state's space
    bool homeAll = true;
    for (const Body &o : g.loaded.bodies) if (o.population > 0 && o.owner != 0) homeAll = false;
    int byKind[5] = {}, self = 0, bad = 0, clanPorts = 0;
    std::vector<std::vector<int>> hostsOf(fs.Count());
    for (const StarSystem *s : Neighbours(g, 70.0)) {
        if (s->develop <= 0.0f || s->id == gal.home) continue;
        LoadedSystem L;
        GenerateSystemContents(*s, L);
        AssignOwners(fs, *s, L);
        for (const Body &o : L.bodies) {
            if (o.settlement == Settlement::SensorOutpost) { if (o.owner != s->ruler || s->ruler == NO_FACTION) bad++; continue; }
            if (o.population <= 0) { if (o.owner != NO_FACTION) bad++; continue; }
            const Faction *f = fs.Get(o.owner);
            if (!f) { self++; continue; }
            byKind[(int)f->kind]++;
            if (f->kind == FactionKind::Government && f->id != s->ruler) bad++;          // a state owns nothing outside its borders
            if (f->kind == FactionKind::Clan) { clanPorts++; if (s->ruler != NO_FACTION) bad++; }
            if (f->kind == FactionKind::Conglomerate && s->ruler > 0 &&
                std::find(hostsOf[f->id].begin(), hostsOf[f->id].end(), s->ruler) == hostsOf[f->id].end()) hostsOf[f->id].push_back(s->ruler);
        }
    }
    int spread = 0;
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Conglomerate && hostsOf[f.id].size() >= 3) spread++;
    CHECK(homeAll && bad == 0 && byKind[(int)FactionKind::Government] > 100 && byKind[(int)FactionKind::Conglomerate] > 100 &&
          byKind[(int)FactionKind::Guild] > 10 && self > 50 && clanPorts > 0 && spread == kinds[(int)FactionKind::Conglomerate],
          "who runs what: governments %d, conglomerates %d (every one in 3+ states), guilds %d, clans %d, independents %d",
          byKind[(int)FactionKind::Government], byKind[(int)FactionKind::Conglomerate], byKind[(int)FactionKind::Guild], clanPorts, self);

    // Relations: symmetric; states have rivals as well as friends; the bounty hunters and the clans are at war
    bool symmetric = true;
    int rivals = 0, friends = 0, hunters = NO_FACTION, patrons = 0;
    for (const Faction &f : fs.list) {
        if (f.kind == FactionKind::Guild && f.guild == GuildKind::BountyHunters) hunters = f.id;
        if (f.kind == FactionKind::Clan && f.patron != NO_FACTION) patrons++;
        for (const Faction &o : fs.list) {
            if (fs.Relation(f.id, o.id) != fs.Relation(o.id, f.id)) symmetric = false;
            if (f.kind == FactionKind::Government && o.kind == FactionKind::Government && o.id > f.id) {
                if (fs.Relation(f.id, o.id) <= -20.0f) rivals++;
                if (fs.Relation(f.id, o.id) >= 15.0f) friends++;
            }
        }
    }
    bool atWar = true;
    for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan && fs.Relation(hunters, f.id) > -50.0f) atWar = false;
    CHECK(symmetric && rivals > 0 && friends > 0 && atWar, "between the governments: %d rivalries, %d friendships; %d clans have a paymaster; the lodge hunts them all",
          rivals, friends, patrons);

    // Character: everyone has goals, and an agenda that follows from them and their temperament
    int aimless = 0;
    for (const Faction &f : fs.list) if (f.goals.empty() || FactionAgenda(fs, f.id).empty()) aimless++;
    Faction hawk = fs.list[1], dove = fs.list[1];
    FactionSet tweak = fs;
    int foe = NO_FACTION;
    for (const Faction &o : fs.list) if (o.kind == FactionKind::Government && o.id != 1 && (foe == NO_FACTION || fs.Relation(1, o.id) < fs.Relation(1, foe))) foe = o.id;
    tweak.relation[1 * fs.Count() + foe] = tweak.relation[foe * fs.Count() + 1] = -70.0f;
    auto wants = [&](float aggression, float caution, const char *what) {
        FactionSet t = tweak;
        t.list[1].trait[TR_AGGRESSION] = aggression; t.list[1].trait[TR_CAUTION] = caution;
        t.list[1].goals = { { GoalType::Humble, foe, aggression }, { GoalType::Secure, NO_FACTION, caution } };
        std::vector<AgendaItem> agenda = FactionAgenda(t, 1, 3);
        for (const AgendaItem &item : agenda) if (item.text.find(what) == 0) return true;
        return false;
    };
    std::vector<AgendaItem> top = FactionAgenda(fs, 1, 1);
    CHECK(aimless == 0 && wants(0.95f, 0.05f, "Press its border") && !wants(0.05f, 0.95f, "Press its border") && wants(0.05f, 0.95f, "Patrol"),
          "every faction has goals and an agenda (%s: \"%s\"); a hawk presses its rival where a dove patrols", fs.list[1].name.c_str(),
          top.empty() ? "" : top[0].text.c_str());

    // Standing: nobody's friend to start with; kept in the save
    bool blank = (int)g.standing.size() == fs.Count();
    for (float s : g.standing) if (s != 0.0f) blank = false;
    g.standing[2] = 37.5f; g.standing[fs.Count() - 1] = -80.0f;
    SaveGame(g, "test_standing.txt");
    GameState h; NewGame(h);
    bool loaded = LoadGame(h, "test_standing.txt");
    remove("test_standing.txt");
    CHECK(blank && loaded && h.standing == g.standing && std::string(RankName(fs.list[2], 37.5f)) != RankName(fs.list[2], 0.0f) &&
          h.Standing(fs.Count() - 1) == -80.0f && h.Standing(999) == 0.0f,
          "standing starts at zero everywhere and round-trips through a save (%+.1f: \"%s\"; %+.0f: \"%s\")", g.standing[2],
          RankName(fs.list[2], g.standing[2]), g.standing[fs.Count() - 1], RankName(fs.list[fs.Count() - 1], g.standing[fs.Count() - 1]));
}

static void TestMining()
{
    {
        // A station in a belt keeps its berth clear of rocks; the field is brought in from 70 km out
        GameState h; NewGame(h);
        int st = -1;
        for (int i = 0; i < (int)h.loaded.bodies.size(); i++) if (h.loaded.bodies[i].settlement == Settlement::OreRefinery) { st = i; break; }
        if (st >= 0) {
            const Body &b = h.loaded.bodies[st];
            h.player.ship.pos = b.local + h.systems[h.systems.home].n * (b.radius + 100.0); h.player.ship.vel = { 0, 0, 0 };
            Run(h, 0.05);
            int rocks = 0, inside = 0;
            double nearest = 1e30, farthest = 0.0;
            for (const auto &cell : h.field.cells)
                for (const Asteroid &r : cell.second) {
                    double d = Len(r.local - b.local);
                    rocks++;
                    nearest = fmin(nearest, d); farthest = fmax(farthest, d);
                    if (d < 2.0 * b.radius + AST_KEEP_CLEAR) inside++;
                }
            CHECK(rocks > 50 && inside == 0 && nearest < 6.0 * (2.0 * b.radius + AST_KEEP_CLEAR) && farthest > 60.0 * KM_U,
                  "around %s: %d rocks out to %.0f km, the nearest %.1f km off; none in its berth", b.name.c_str(), rocks, farthest / KM_U, nearest / KM_U);
        }
    }
    GameState g; NewGame(g);
    int z = -1;
    for (int i = 0; i < (int)g.loaded.zones.size(); i++) if (!g.loaded.zones[i].ring) { z = i; break; }
    CHECK(z >= 0, "home system has an asteroid belt");
    if (z < 0) return;
    g.player.ship.pos = ZoneTargetPoint(g.loaded.zones[z], ShipLocal(g));
    g.player.ship.vel = { 0, 0, 0 };
    Run(g, 0.05);
    const Asteroid *best = nullptr;
    DVec3 here = ShipLocal(g);
    for (auto &cell : g.field.cells)
        for (const Asteroid &a : cell.second)
            if (!best || Len(a.local - here) < Len(best->local - here)) best = &a;
    CHECK(best != nullptr, "rocks generated around the ship (%d cells)", (int)g.field.cells.size());
    if (!best) return;
    uint64_t id = best->id;
    DVec3 rockPos = best->local;
    float r = AsteroidRadius(*best);
    g.player.ship.pos = rockPos + Norm(here - rockPos) * (r + 100.0);
    Face(g, rockPos - g.player.ship.pos);
    Aim aim; aim.hovered = TargetId::MakeRock(g.systems.home, id);
    Controls fire; fire.fire = true;
    Run(g, 2.0, fire, aim);
    double got = g.player.CargoTotal();
    CHECK(got > 3.9 && got < 4.1, "2 s of mining yields %.2f t of %s", got, GoodData(FindAsteroid(g.field, id) ? FindAsteroid(g.field, id)->ore : 0).name);
    CHECK(g.deltas.mined.count(id) == 1, "the rock remembers it was mined");
}

static void TestSaveLoad()
{
    GameState g; NewGame(g);
    int c = TradeWorld(g);
    Park(g, c, 0.9);
    Run(g, 1.0);
    g.player.credits = 1234.5; g.player.fuel = 17.25; g.player.cargo[ORE_PRECIOUS] = 3.5;
    g.player.ship.dampeners = false;
    g.shipName = "Canterbury";
    g.deltas.mined[0xDEADBEEFull] = 12.5f;
    CHECK(SaveGame(g, "test_save.txt"), "saved");
    GameState h; NewGame(h);
    CHECK(LoadGame(h, "test_save.txt"), "loaded");
    CHECK(h.player.credits == g.player.credits && h.player.fuel == g.player.fuel &&
          h.player.cargo[ORE_PRECIOUS] == 3.5 && !h.player.ship.dampeners, "player state round-trips");
    CHECK(Len(h.player.ship.pos - g.player.ship.pos) < 1e-6 && h.player.ship.anchor == g.player.ship.anchor,
          "ship position round-trips exactly");
    CHECK(h.deltas.mined.count(0xDEADBEEFull) && h.deltas.mined[0xDEADBEEFull] == 12.5f, "mined rocks round-trip");
    CHECK(h.loaded.index == g.systems.home && h.clock == g.clock, "system reloaded around the ship, clock restored");
    CHECK(h.shipName == "Canterbury", "the ship keeps her name");
    CHECK(SeedFromText("1977", 5) == 1977u && SeedFromText(" 42 ", 5) == 42u && SeedFromText("tycho", 1) == SeedFromText("tycho", 2) &&
          SeedFromText("tycho", 1) != SeedFromText("ceres", 1) && SeedFromText("", 1) != SeedFromText("", 2) && SeedFromText("", 7) != 0,
          "a seed can be typed as a number or as words; left blank it's random");
    Run(h, 0.05);
    CHECK(h.flight.nearBody == c, "still in trading range after loading");
    CHECK(!LoadGame(h, "no_such_file.txt") && h.player.credits == 1234.5, "a missing save leaves the game alone");
    remove("test_save.txt");

    // Named saves in the saves folder, and the load browser's summary
    CHECK(ValidSaveName("Luaros 3") && ValidSaveName("run_2.b-final") && !ValidSaveName("") && !ValidSaveName("../x") &&
          !ValidSaveName("a/b") && !ValidSaveName(" lead") && !ValidSaveName(std::string(41, 'x')), "save names are checked");
    std::string path = SavePathFor("unit test save");
    CHECK(SaveGame(g, path.c_str(), "unit test save"), "named save written to %s", path.c_str());
    SaveSummary sum = ReadSaveSummary(path.c_str());
    CHECK(sum.valid && sum.name == "unit test save" && sum.location == g.systems[g.systems.home].name && sum.credits == 1234.5 &&
          fabs(sum.cargo - 3.5) < 1e-9 && sum.seed == g.seed, "summary: \"%s\" at %s, %.0f cr", sum.name.c_str(), sum.location.c_str(), sum.credits);
    CHECK(!ReadSaveSummary(SettingsPathForTests().c_str()).valid, "a file that isn't a save is recognized");
    {
        FILE *old = fopen("old_save.txt", "w");                 // from the twelve-star galaxy: system 3 no longer means anything
        if (old) { fputs("SPACEFLIGHT_SAVE 1\nseed 1977\nclock 5\nship 3 0 0 0 0 0 0 0 0 0 1 1\nfuel 10\ncredits 99\nend\n", old); fclose(old); }
        CHECK(!LoadGame(h, "old_save.txt") && !ReadSaveSummary("old_save.txt").valid && h.player.credits == 1234.5,
              "a save from the old small galaxy is turned away, not misplaced");
        remove("old_save.txt");
    }
    remove(path.c_str());
    remove("not_a_save.txt");
}

// How the economy stands, good by good (a tuning aid: --test econprobe)
static void ProbeEconomy(GameState &g, const char *when)
{
    const int N = g.econ.Count();
    printf("  -- %s --\n", when);
    double lab = 0, emp = 0, cellsMade = 0, cellsUsed = 0, housed = 0, pop = 0;
    int building = 0, shortL = 0, shortC = 0, shortM = 0, inds = 0;
    for (int i = 0; i < N; i++) {
        const MarketState &s = g.econ.state[i];
        lab += s.labour; emp += s.employed; cellsMade += s.cellsMade; cellsUsed += s.cellsUsed; housed += fmin((double)s.housing, s.pop); pop += s.pop;
        for (const IndustryState &st : s.ind) {
            if (st.cap <= 0.0f) continue;
            inds++; if (st.building > 0.0f) building++;
            if (st.shortOf & SHORT_LABOUR) shortL++; if (st.shortOf & SHORT_CELLS) shortC++; if (st.shortOf & SHORT_MATERIALS) shortM++;
        }
    }
    printf("  employed %.0f%%  housed %.0f%%  cells made/used %.0f / %.0f  industries %d: building %d, short of labour %d, cells %d, materials %d\n",
           100.0 * emp / lab, 100.0 * housed / pop, cellsMade, cellsUsed, inds, building, shortL, shortC, shortM);
    {
        double was = 0, living = 0, worst = 1, refugees = 0, leaving = 0, stuck = 0;
        int growing = 0, shrinking = 0, skeleton = 0, poor = 0, borders[3] = {};
        for (int i = 0; i < N; i++) {
            const MarketState &s = g.econ.state[i];
            was += g.econ.base->defs[i].pop; living += s.living; worst = fmin(worst, (double)s.living); refugees += s.refugees; leaving += s.leaving; stuck += s.stuck;
            if (s.pop > 1.02 * g.econ.base->defs[i].pop) growing++; if (s.pop < 0.98 * g.econ.base->defs[i].pop) shrinking++;
            if (s.pop < 2.0 * SKELETON_CREW) skeleton++; if (s.living < LEAVE_BELOW) poor++;
        }
        for (const Faction &f : g.systems.factions.list) if (f.kind == FactionKind::Government) borders[g.econ.border[f.id]]++;
        printf("  people %.4g (%.1f%% of the start)  living %.2f on average, %.2f at worst, %d below %.2f  bigger %d, smaller %d, emptied %d  on the move %.0f a day, stuck %.0f, unsettled %.0f  borders open %d, selective %d, closed %d\n",
               pop, 100.0 * pop / was, living / N, worst, poor, LEAVE_BELOW, growing, shrinking, skeleton, leaving, stuck, refugees, borders[0], borders[1], borders[2]);
    }
    {
        // The fleets: ships of each kind, how busy they are (what there was to carry over what they could), and what the yards turn out against what wears out
        const EconBase &base = *g.econ.base;
        double ships[ROLE_COUNT] = {}, busy[ROLE_COUNT] = {}, tonnage = 0, built = 0, clans = 0;
        for (int i = 0; i < N && i < (int)g.econ.fleets.size(); i++) {
            for (const FleetAt &f : g.econ.fleets[i])
                for (int r = 0; r < ROLE_COUNT; r++) {
                    ships[r] += f.ships[r]; tonnage += f.ships[r] * FleetDesign(base, f.owner, r).tonnage;
                    busy[r] += f.ships[r] * g.econ.busy[(size_t)i * ROLE_COUNT + r];
                }
            for (size_t k = 0; k < base.defs[i].ind.size() && k < g.econ.state[i].ind.size(); k++)
                if (base.defs[i].ind[k].type == IND_SHIPYARD) built += g.econ.state[i].ind[k].output;
        }
        for (float c : g.econ.clanShips) clans += c;
        printf("  ships: %.0f bulk (busy %.2f), %.0f haulers (%.2f), %.0f tankers (%.2f), %.0f liners (%.2f); %.0f with the clans; yards build %.0f t/d, %.0f t/d wears out\n",
               ships[0], busy[0] / fmax(ships[0], 1e-9), ships[1], busy[1] / fmax(ships[1], 1e-9), ships[2], busy[2] / fmax(ships[2], 1e-9), ships[3], busy[3] / fmax(ships[3], 1e-9),
               clans, built, tonnage / SHIP_LIFE_DAYS);
    }
    const double fd = fmax(g_flowDays, 1e-9);
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        double out = 0, stock = 0, target = 0, made = 0; int low = 0, high = 0, deal = 0, makers = 0;
        for (int i = 0; i < N; i++) {
            const MarketState &s = g.econ.state[i];
            if (s.target[gd] <= 0.0f) continue;
            deal++; out += s.out[gd]; stock += s.stock[gd]; target += s.target[gd];
            if (s.out[gd] > 0.0f) makers++;
            const MarketDef &d = g.econ.base->defs[i];
            for (size_t k = 0; k < d.ind.size() && k < s.ind.size(); k++)
                for (int p = 0; p < d.ind[k].outs; p++)
                    if (d.ind[k].out[p].good == gd) made += s.ind[k].cap * s.ind[k].run * d.ind[k].out[p].share * g.econ.base->perCap[gd];
            if (s.stock[gd] < 0.25f * s.target[gd]) low++; if (s.stock[gd] > 1.8f * s.target[gd]) high++;
        }
        (void)made;
        printf("  %-18s cap %9.0f t/d at %4d, made %7.0f, to industry %7.0f, people %7.0f, ships %7.0f (+%.0f not there)  stock %3.0f%% of target  short at %4d, glutted at %4d of %d\n", GoodData(gd).name, out, makers, g_flow[0][gd] / fd, g_flow[1][gd] / fd, g_flow[2][gd] / fd, g_flow[3][gd] / fd, g_flow[4][gd] / fd, 100.0 * stock / fmax(target, 1.0), low, high, deal);
    }
    memset(g_flow, 0, sizeof g_flow); g_flowDays = 0.0;
}

static void RunEconomy(World &w, double days);
static void TestEconProbe()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    ProbeEconomy(g, "at the start");
    RunEconomy(g, 14.0);
    ProbeEconomy(g, "after two weeks");
    RunEconomy(g, 30.0);
    ProbeEconomy(g, "after six weeks");
}

// Who has grown most, and who has emptied (--test econwho)
static void TestEconWho()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const int N = g.econ.Count();
    RunEconomy(g, getenv("FH_DAYS") ? atof(getenv("FH_DAYS")) : 365.0);
    std::vector<int> order(N);
    for (int i = 0; i < N; i++) order[i] = i;
    auto ratio = [&](int i) { return g.econ.state[i].pop / fmax(base.defs[i].pop, 1.0); };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ratio(a) > ratio(b); });
    auto show = [&](int i) {
        const MarketDef &d = base.defs[i];
        const MarketState &s = g.econ.state[i];
        double food = s.target[REF_FOOD] > 0.0f ? s.stock[REF_FOOD] / s.target[REF_FOOD] : -1.0;
        printf("  %-30s %-18s %s pop %11.0f -> %13.0f  housing %13.0f  living %.2f  food %4.0f%%  work %3.0f%%  services %3.0f%%  trend %+.3f  arrived %.0f  leaving %.0f stuck %.0f  scale %.0f\n",
               d.name.c_str(), SettlementName(d.settlement), d.hub == i ? "HUB" : "   ", d.pop, s.pop, s.housing, s.living, food * 100.0,
               s.labour > 0 ? 100.0 * s.employed / s.labour : 0.0, s.services * 100.0, s.trend, s.refugees, s.leaving, s.stuck, d.scale);
    };
    printf("  -- grown most --\n");
    for (int k = 0; k < 10; k++) show(order[k]);
    printf("  -- emptied (a sample) --\n");
    int shown = 0, bySettlement[32] = {}, hubs = 0, total = 0;
    for (int i = 0; i < N; i++) {
        if (g.econ.state[i].pop >= 2.0 * SKELETON_CREW || base.defs[i].pop < 4.0 * SKELETON_CREW) continue;
        total++; bySettlement[(int)base.defs[i].settlement & 31]++; if (base.defs[i].hub == i) hubs++;
        if (shown++ % 12 == 0) show(i);
    }
    printf("  -- %d emptied (%d of them hubs), by kind:", total, hubs);
    for (int k = 0; k < 32; k++) if (bySettlement[k]) printf("  %s %d", SettlementName((Settlement)k), bySettlement[k]);
    printf("\n");
}

// The ships each kind of faction designs (a look at the designer: --test fleetprobe)
static void TestFleetProbe()
{
    GameState g; NewGame(g);
    const FactionSet &fs = g.systems.factions;
    std::vector<int> who = { NO_FACTION };
    bool seen[8][8] = {};
    for (const Faction &f : fs.list) {
        int kind = (int)f.kind, sub = f.kind == FactionKind::Conglomerate ? (int)f.slant : f.kind == FactionKind::Government ? (f.axis[AX_FORCE] > 0.3f) + 2 * (f.axis[AX_OUTLOOK] > 0.3f) : 0;
        if (seen[kind][sub & 7]) continue;
        seen[kind][sub & 7] = true;
        who.push_back(f.id);
    }
    for (int id : who)
        for (int role = 0; role < ROLE_COUNT; role++) {
            ShipDesign d = DesignShip(fs, id, role);
            printf("  %-44s %-12s x%d  %5.0f t cargo %4.0f berths  %6.0f t loaded  %5.0f TW %-12s cruise %.2f  run: %4.0f s + %4.1f s/ly, %6.1f t fuel + %5.2f t/ly  local %3.0f s %5.1f t  "
                   "%5.0f m3 of %5.0f  built from %5.0f t, %s cr\n", d.name.c_str(), Hull(d.build.hull).name, (int)d.build.stretch, d.cargo, d.berths, d.mass, d.spec.reactorTW,
                   GoodData(d.fuelGood).name, d.cruise, d.fixedSec, d.secPerLy, d.fixedFuel, d.fuelPerLy, d.localSec, d.localFuel, VolumeUsed(d.build), BuildVolume(d.build), d.tonnage,
                   Thousands(d.value).c_str());
            // (the parts it was built around)
            std::string parts;
            for (const Part &p : d.build.parts)
                if (p.type == PartType::Reactor || p.type == PartType::WarpDrive || p.type == PartType::HyperDrive || p.type == PartType::DriveCap)
                    parts += std::string(parts.empty() ? "" : ", ") + PartModelOf(p).id + TextFormat(" %s x%d", PartTypeName(p.type), (int)p.size);
            printf("      %s;  %.1f%% of its load in fuel on a %.0f ly run\n", parts.c_str(), 100.0 * (role == ROLE_HAULER ? d.localFuel : d.Fuel(USUAL_RUN_LY)) / fmax(role == ROLE_LINER ? d.berths * PERSON_CABIN : d.cargo, 1.0), USUAL_RUN_LY);
        }
}

// How settled human space is, by distance from home (a tuning aid: --test spaceprobe)
static void TestSpaceProbe()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    std::vector<const StarSystem *> all;
    g.systems.Nearby(g.systems.home, {}, FRONTIER_RADIUS_LY * 1.25 * LY_U, all);
    std::unordered_set<int64_t> settled;
    for (const MarketDef &d : base.defs) settled.insert((int64_t)d.sys);
    const double edges[] = { 10, 20, 30, 40, 50, 65, 82 };
    int n[7] = {}, dev[7] = {}, ruled[7] = {}, lived[7] = {};
    const SysId home = g.systems.home;
    for (const StarSystem *sys : all) {
        double d = Len(g.systems.Between(home, sys->id)) / LY_U;
        int band = 0;
        while (band < 6 && d > edges[band]) band++;
        n[band]++;
        if (sys->develop > 0.0f) dev[band]++;
        if (sys->ruler != NO_FACTION) ruled[band]++;
        if (settled.count((int64_t)sys->id)) lived[band]++;
    }
    int tn = 0, td = 0, tr = 0, tl = 0;
    for (int b = 0; b < 7; b++) {
        printf("  out to %2.0f ly: %5d systems, %5d with any development, %5d inside a government's borders, %5d with people living in them (%.0f%%)\n", edges[b], n[b], dev[b], ruled[b], lived[b],
               100.0 * lived[b] / std::max(n[b], 1));
        tn += n[b]; td += dev[b]; tr += ruled[b]; tl += lived[b];
    }
    printf("  in all:        %5d systems, %5d with any development, %5d inside a government's borders, %5d with people living in them (%.0f%%); %d markets\n", tn, td, tr, tl, 100.0 * tl / std::max(tn, 1),
           (int)base.defs.size());
}

// The economy over years (a stability check: --test econyears [not part of the ordinary run])
static void YearsLine(GameState &g, double day)
{
    const int N = g.econ.Count();
    double lab = 0, emp = 0, pop = 0, pop0 = 0, living = 0, cap = 0, made = 0, used = 0, stuck = 0;
    int poor = 0, empty = 0, hungry = 0, dark = 0, wild = 0, priced = 0, bad = 0, unhoused = 0, building = 0, borders[3] = {};
    double key[5] = {}, keyT[5] = {};
    const int goods[5] = { REF_FOOD, REF_CELLS, REF_STEEL, GOOD_HABITAT_MODULES, GOOD_MEDICINES };
    double biggest = 0.0;
    for (int i = 0; i < N; i++) {
        const MarketState &s = g.econ.state[i];
        lab += s.labour; emp += s.employed; pop += s.pop; pop0 += g.econ.base->defs[i].pop; living += s.living; made += s.cellsMade; used += s.cellsUsed; stuck += s.stuck;
        if (s.living < LEAVE_BELOW) poor++;
        if (s.pop < 2.0 * SKELETON_CREW && g.econ.base->defs[i].pop >= 4.0 * SKELETON_CREW) empty++;
        if (s.target[REF_FOOD] > 0.0f && s.stock[REF_FOOD] < 0.25f * s.target[REF_FOOD]) hungry++;
        if (s.cellsMade + 1.0f < 0.5f * s.cellsUsed && s.stock[REF_CELLS] < 0.1f * s.target[REF_CELLS]) dark++;
        if (s.housing < 0.95 * s.pop) unhoused++;
        biggest = fmax(biggest, s.pop / fmax(g.econ.base->defs[i].pop, 1.0));
        for (const IndustryState &st : s.ind) { if (st.cap > 0.0f) cap += st.cap; if (st.building > 0.0f) building++; if (!(st.cap >= 0.0f) || !std::isfinite(st.earned)) bad++; }
        if (!(s.pop > 0.0) || !std::isfinite(s.pop) || !std::isfinite(s.living)) bad++;
        for (int k = 0; k < GOOD_COUNT; k++) {
            if (!(s.stock[k] >= 0.0f) || !std::isfinite(s.stock[k])) bad++;
            double p = MarketPrice(g, i, k);
            if (p > 0.0) { priced++; if (p > 5.9 * GoodData(k).basePrice || p < 0.31 * GoodData(k).basePrice) wild++; }
        }
        for (int k = 0; k < 5; k++) { key[k] += s.stock[goods[k]]; keyT[k] += s.target[goods[k]]; }
    }
    for (const Faction &f : g.systems.factions.list) if (f.kind == FactionKind::Government) borders[g.econ.border[f.id]]++;
    printf("  day %5.0f  people %5.1f%% (most grown x%.1f)  work %2.0f%%  living %.2f  poor %4d  emptied %3d  hungry %4d  dark %3d  unhoused %4d  prices at a limit %4.1f%%  "
           "capacity %7.0fk  building %4d  cells %3.0f%%  food %3.0f%% steel %3.0f%% modules %3.0f%% medicine %3.0f%%  stuck %.1fM  borders %d/%d/%d  news %u%s\n",
           day, 100.0 * pop / pop0, biggest, 100.0 * emp / lab, living / N, poor, empty, hungry, dark, unhoused, 100.0 * wild / fmax(priced, 1), cap / 1000.0, building,
           100.0 * made / fmax(used, 1.0), 100.0 * key[0] / keyT[0], 100.0 * key[2] / keyT[2], 100.0 * key[3] / keyT[3], 100.0 * key[4] / keyT[4], stuck / 1e6,
           borders[0], borders[1], borders[2], g.newsSeq, bad ? "   *** BAD NUMBERS ***" : "");
    {
        // (shipping: the fleets, how stretched they are, and the fuel they got against what they wanted)
        double ships[ROLE_COUNT] = {}, busy[ROLE_COUNT] = {}, burned = 0.0, missing = 0.0;
        for (int i = 0; i < N && i < (int)g.econ.fleets.size(); i++)
            for (const FleetAt &f : g.econ.fleets[i])
                for (int r = 0; r < ROLE_COUNT; r++) { ships[r] += f.ships[r]; busy[r] += f.ships[r] * g.econ.busy[(size_t)i * ROLE_COUNT + r]; }
        for (int k : { REF_REACTOR_FUEL, REF_FUEL, RAW_HELIUM3 }) { burned += g_flow[3][k]; missing += g_flow[4][k]; }
        printf("             ships %.0f bulk (busy %.2f) %.0f haulers (%.2f) %.0f tankers (%.2f) %.0f liners (%.2f)  fuel burned %.0f t/d, %.0f%% more wanted and not there\n",
               ships[0], busy[0] / fmax(ships[0], 1e-9), ships[1], busy[1] / fmax(ships[1], 1e-9), ships[2], busy[2] / fmax(ships[2], 1e-9), ships[3], busy[3] / fmax(ships[3], 1e-9),
               burned / fmax(g_flowDays, 1e-9), 100.0 * missing / fmax(burned, 1.0));
        memset(g_flow, 0, sizeof g_flow); g_flowDays = 0.0;
    }
    {
        // (money: by kind of faction, what's in the treasuries and on its way, what they earn a day and from what, what they spend and can't)
        static const char *kinds[] = { "governments", "companies", "clans", "others" };
        double waiting[4] = {};
        const int F = g.systems.factions.Count();
        for (int i = 0; i < N; i++) for (int f = 0; f < F; f++) {
            FactionKind k = g.systems.factions.list[f].kind;
            waiting[k == FactionKind::Government ? 0 : k == FactionKind::Conglomerate ? 1 : k == FactionKind::Clan ? 2 : 3] += g.econ.Coffer(i, f) + g.econ.Moving(i, f);
        }
        {
            // (the treasury ships under way just now: how many, and what they carry)
            std::vector<double> runs;
            for (int i = 0; i < N; i++) for (int f = 0; f < F; f++) if (g.econ.Moving(i, f) > 0.0) runs.push_back(g.econ.Moving(i, f));
            std::sort(runs.begin(), runs.end());
            double all = 0.0;
            for (double r : runs) all += r;
            if (!runs.empty())
                printf("             treasury ships under way: %d, carrying %.1fM in all; the middling one %.0f cr, one in ten more than %.0f, the richest %.1fM\n", (int)runs.size(), all / 1e6,
                       runs[runs.size() / 2], runs[runs.size() * 9 / 10], runs.back() / 1e6);
        }
        for (int kind = 0; kind < 4; kind++) {
            double treasury = 0, income[INC_COUNT] = {}, spent = 0, denied = 0, squeeze = 0, upkeep = 0; int n = 0, broke = 0;
            for (const Faction &f : g.systems.factions.list) {
                int k = f.kind == FactionKind::Government ? 0 : f.kind == FactionKind::Conglomerate ? 1 : f.kind == FactionKind::Clan ? 2 : 3;
                if (k != kind) continue;
                const FactionMoney &m = g.econ.money[f.id];
                n++; treasury += m.treasury; spent += m.spent; denied += m.denied; squeeze += m.squeeze; upkeep += m.upkeep;
                if (m.denied > m.spent) broke++;
                for (int s = 0; s < INC_COUNT; s++) income[s] += m.income[s];
            }
            if (!n) continue;
            printf("             %-11s x%-2d treasury %7.1fM  on its way %6.1fM  a day: tariffs %5.2fM tax %5.2fM heads %5.2fM profit %6.2fM freight %5.2fM yards %5.2fM loot %5.2fM  upkeep %6.2fM  built %6.2fM  couldn't %6.2fM  (%d going without)  squeeze %.2f\n",
                   kinds[kind], n, treasury / 1e6, waiting[kind] / 1e6, income[0] / 1e6, income[1] / 1e6, income[2] / 1e6, income[3] / 1e6, income[4] / 1e6, income[5] / 1e6, income[6] / 1e6,
                   upkeep / 1e6, spent / 1e6, denied / 1e6, broke, squeeze / n);
        }
    }
    fflush(stdout);
}

static void TestEconYears()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const double years = getenv("FH_YEARS") ? atof(getenv("FH_YEARS")) : 3.0;
    // (FH_FINE: at the step the game really takes, a hundred and twenty to the day: slow, and the one that counts)
    const bool fine = getenv("FH_FINE") != nullptr;
    printf("  %d economies, %.1f years of 365 market days, in %s steps\n", g.econ.Count(), years, fine ? "the game's own five-second" : "quarter-day");
    YearsLine(g, 0.0);
    for (int q = 1; q <= (int)(years * 8.0 + 0.5); q++) {
        if (!fine) RunEconomy(g, 365.0 / 8.0);
        else for (int i = 0, n = (int)(365.0 / 8.0 * MARKET_DAY / ECON_TICK + 0.5); i < n; i++) { g.clock += ECON_TICK; StepEconomy(g, ECON_TICK / MARKET_DAY); }
        YearsLine(g, q * 365.0 / 8.0);
    }
    if (fine) return;
    // ...and the same economy at the pace the game really runs it (a step every five seconds), from a fresh start, for comparison
    GameState h; NewGame(h);
    EnsureEconomy(h);
    printf("  at the game's own step (%.4f of a day):\n", ECON_TICK / MARKET_DAY);
    for (int d = 0; d <= 90; d += 30) {
        YearsLine(h, d);
        if (d < 90) for (int i = 0; i < 30 * (int)(MARKET_DAY / ECON_TICK); i++) { h.clock += ECON_TICK; StepEconomy(h, ECON_TICK / MARKET_DAY); }
    }
}

// The economy: stock, production, trade and prices; what the factions do to it
static void RunEconomy(World &w, double days)
{
    for (int i = 0, n = (int)(days * 4 + 0.5); i < n; i++) { w.clock += MARKET_DAY / 4; StepEconomy(w, 0.25); }
    w.econ.simulated = w.clock;
}

static void TestEconomy()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const FactionSet &fs = g.systems.factions;
    const int N = g.econ.Count();
    int hubs = 0, stations = 0, small = -1, big = -1, lawful = 0;
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        if (d.hub == i) hubs++;
        if (d.station) stations++;
        if (small < 0 || d.scale < base.defs[small].scale) small = i;
        if (big < 0 || d.scale > base.defs[big].scale) big = i;
        for (int gd = 0; gd < GOOD_COUNT; gd++) if (d.banned[gd]) { lawful++; break; }
    }
    CHECK(N > 500 && hubs > 100 && stations > 0 && (int)base.lanes.size() > hubs && base.defs[big].scale > 50.0f * base.defs[small].scale,
          "%d markets in %d systems, %d trade lanes; from %s (%.0f t a day) to %s (%.0f t a day)", N, hubs, (int)base.lanes.size(),
          base.defs[small].name.c_str(), base.defs[small].scale, base.defs[big].name.c_str(), base.defs[big].scale);

    // Every good is made somewhere and wanted somewhere, and prices start out spread but sane
    int unmade = 0, unwanted = 0, capped = 0, priced = 0, hungry = 0;
    for (int i = 0; i < N; i++) if (g.econ.state[i].stock[REF_FOOD] < 0.25f * g.econ.state[i].target[REF_FOOD]) hungry++;
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        double out = 0, use = 0;
        for (int i = 0; i < N; i++) {
            out += g.econ.state[i].out[gd]; use += g.econ.state[i].use[gd];
            double p = MarketPrice(g, i, gd);
            if (p > 0.0) { priced++; if (p > 5.9 * GoodData(gd).basePrice || p < 0.31 * GoodData(gd).basePrice) capped++; }
        }
        for (int other = 0; other < GOOD_COUNT; other++)
            for (const Ingredient &in : GoodData(other).recipe) if (in.tonnes > 0.0f && in.good == gd) use += 1.0;
        if (gd == REF_CELLS || gd == GOOD_DATA) use += 1.0;      // (what industries themselves run on: industry.h)
        if (gd == GOOD_HULL_PLATING || gd == GOOD_SHIP_COMPONENTS || gd == GOOD_REACTOR_CORES) use += 1.0;      // (what shipyards build from)
        if (out <= 0.0) unmade++;
        if (use <= 0.0) unwanted++;
    }
    CHECK(unmade == 0 && unwanted == 0 && capped < priced / 30 && hungry < N / 50, "all %d goods are made and wanted somewhere; %.1f%% of %d prices are at a limit; %d markets are short of food",
          GOOD_COUNT, 100.0 * capped / priced, priced, hungry);

    // Price follows stock
    int home = g.econ.MarketOf(g.loaded.bodies[TradeWorld(g)].id);
    CHECK(home >= 0, "the home world is a market");
    if (home < 0) return;
    {
        Economy was = g.econ;
        int gd = REF_FOOD;
        float target = g.econ.state[home].target[gd];
        g.econ.state[home].stock[gd] = target;
        double at = MarketPrice(g, home, gd);
        g.econ.state[home].stock[gd] = target * 0.5f;
        double scarce = MarketPrice(g, home, gd);
        g.econ.state[home].stock[gd] = target * 2.0f;
        double glut = MarketPrice(g, home, gd);
        g.econ.state[home].stock[gd] = 0.0f;
        double none = MarketPrice(g, home, gd);
        CHECK(fabs(at - GoodData(gd).basePrice) < 0.01 && fabs(scarce / at - pow(2.0, 0.6)) < 0.01 && fabs(glut / at - pow(0.5, 0.6)) < 0.01 &&
              fabs(none / at - 6.0) < 0.01,
              "food at its target stock costs %.0f; at half that %.0f, at double %.0f, with none left %.0f", at, scarce, glut, none);

        // A hold of cargo matters at a small place and not at a big one
        int both = -1;
        for (int k = 0; k < GOOD_COUNT; k++) if (g.econ.state[small].target[k] > 0.0f && g.econ.state[big].target[k] > 0.0f) { both = k; break; }
        if (both >= 0) {
            double s0 = MarketPrice(g, small, both), b0 = MarketPrice(g, big, both);
            g.econ.state[small].stock[both] += 50.0f; g.econ.state[big].stock[both] += 50.0f;
            double sd = 1.0 - MarketPrice(g, small, both) / s0, bd = 1.0 - MarketPrice(g, big, both) / b0;
            CHECK(sd > 0.05 && bd < 0.02 && sd > 5.0 * bd, "50 t of %s knocks %.0f%% off the price at %s and %.2f%% at %s", GoodData(both).name,
                  sd * 100.0, base.defs[small].name.c_str(), bd * 100.0, base.defs[big].name.c_str());
        }
        g.econ = was;
    }

    // Production needs its inputs
    {
        int m = -1, gd = -1;
        for (int i = 0; i < N && m < 0; i++)
            for (int k = FIRST_REFINED; k < GOOD_COUNT; k++)
                if (g.econ.state[i].out[k] > 0.2f * base.defs[i].scale && GoodData(k).recipe[0].tonnes > 0.0f && base.defs[i].sys != g.systems.home) { m = i; gd = k; break; }
        CHECK(m >= 0, "found a plant: %s makes %s", m >= 0 ? base.defs[m].name.c_str() : "?", m >= 0 ? GoodData(gd).name : "?");
        if (m >= 0) {
            Economy was = g.econ;
            auto made = [&](float inputs) {
                g.econ = was;
                g.econ.state[m].stock[gd] = 0.5f * g.econ.state[m].target[gd];
                for (const Ingredient &in : GoodData(gd).recipe) if (in.tonnes > 0.0f) g.econ.state[m].stock[in.good] = inputs;
                float before = g.econ.state[m].stock[gd];
                StepEconomy(g, 0.01);
                return g.econ.state[m].stock[gd] - before;
            };
            float starved = made(0.0f), fed = made(1e6f);
            CHECK(fed - starved > 0.5f * g.econ.state[m].out[gd] * 0.01f, "in 1%% of a day its stock grows %.2f t with its inputs on hand and %.2f t without (what trade brought in)", fed, starved);
            g.econ = was;
        }
    }

    // Trade: a glut at one market flows to its hub
    {
        int m = -1, gd = -1;
        for (int i = 0; i < N && m < 0; i++) {
            const MarketDef &d = base.defs[i];
            if (d.hub == i || d.hub < 0 || d.sys == g.systems.home) continue;
            for (int k = 0; k < GOOD_COUNT; k++)
                if (g.econ.state[i].target[k] > 20.0f && g.econ.state[i].out[k] == 0.0f && g.econ.state[i].use[k] == 0.0f && g.econ.state[d.hub].target[k] > 0.0f && d.hub != i) { m = i; gd = k; break; }
        }
        CHECK(m >= 0, "found a market that neither makes nor uses something it deals in (%s, %s)", m >= 0 ? base.defs[m].name.c_str() : "?", m >= 0 ? GoodData(gd).name : "?");
        if (m >= 0) {
            Economy was = g.econ;
            int hub = base.defs[m].hub;
            g.econ.state[m].stock[gd] = 3.0f * g.econ.state[m].target[gd];
            g.econ.state[hub].stock[gd] = g.econ.state[hub].target[gd];
            double gap0 = MarketPrice(g, hub, gd) / MarketPrice(g, m, gd);
            float s0 = g.econ.state[m].stock[gd];
            for (int i = 0; i < 8; i++) StepEconomy(g, 0.25);
            double gap1 = MarketPrice(g, hub, gd) / MarketPrice(g, m, gd);
            CHECK(g.econ.state[m].stock[gd] < s0 && gap1 < gap0 && gap0 > 1.2, "a glut of %s at %s: two days of trade take %.0f t of it away, and the gap to %s closes from %.0f%% to %.0f%%",
                  GoodData(gd).name, base.defs[m].name.c_str(), s0 - g.econ.state[m].stock[gd], base.defs[hub].name.c_str(), (gap0 - 1) * 100, (gap1 - 1) * 100);
            g.econ = was;
        }
    }

    // The law, and who you know
    {
        int c = TradeWorld(g);
        Park(g, c, 0.9); Run(g, 1.1);
        const Body &b = g.loaded.bodies[c];
        const MarketDef &d = base.defs[home];
        Market mk = LiveMarket(g, g, b), seen;
        double at = -1.0;
        CHECK(KnownMarket(g, b, seen, &at) && g.clock - at < 1.01 && seen.price[REF_FUEL] == (float)mk.price[REF_FUEL], "docked: the market's prices are on record, as of now");
        // ...and the record goes stale when you leave
        Park(g, c, 3.0); Run(g, 0.1);
        g.econ.state[home].stock[REF_FUEL] *= 0.5f;
        Run(g, 1.5);
        KnownMarket(g, b, seen, &at);
        CHECK(!TradePartner(g) && g.clock - at > 1.5 && seen.price[REF_FUEL] == (float)mk.price[REF_FUEL] && LiveMarket(g, g, b).price[REF_FUEL] > 1.3 * mk.price[REF_FUEL],
              "away from it the record goes stale: fuel on record at %.0f, really %.0f", seen.price[REF_FUEL], LiveMarket(g, g, b).price[REF_FUEL]);
        Park(g, c, 0.9); Run(g, 1.1);
        KnownMarket(g, b, seen);
        CHECK(seen.price[REF_FUEL] > 1.3 * mk.price[REF_FUEL], "back in dock it's brought up to date");

        // Tariff: off what you're paid
        int gd = -1;
        for (int k = 0; k < GOOD_COUNT; k++) if (mk.Buys(k) && g.econ.state[home].target[k] > 2000.0f) { gd = k; break; }
        (void)d;
        if (gd < 0) for (int k = 0; k < GOOD_COUNT; k++) if (mk.Buys(k)) { gd = k; break; }
        float tariffWas = g.econ.tariff[d.ruler];
        g.econ.tariff[d.ruler] = 0.0f;
        double free = LiveMarket(g, g, b).SellPrice(gd);
        g.econ.tariff[d.ruler] = 0.2f;
        mk = LiveMarket(g, g, b);
        g.player.cargo[gd] = 1.0;
        double cr = g.player.credits;
        SellGoods(g, gd, 1.0);
        CHECK(fabs(mk.SellPrice(gd) / free - 0.8) < 1e-6 && g.player.credits - cr < 0.81 * free && g.player.credits - cr > 0.75 * free,
              "a 20%% tariff: %s pays %.1f cr/t instead of %.1f", GoodData(gd).name, g.player.credits - cr, free);
        g.econ.tariff[d.ruler] = tariffWas;

        // Standing with the owner: up to 5% either way; trade builds it
        int owner = g.econ.state[home].owner;
        if (owner < 0) { owner = d.ruler; g.econ.state[home].owner = owner; }
        g.standing.assign(fs.Count(), 0.0f);
        double fair = LiveMarket(g, g, b).BuyPrice(gd);
        g.standing[owner] = 100.0f;
        double friendly = LiveMarket(g, g, b).BuyPrice(gd), friendSells = LiveMarket(g, g, b).SellPrice(gd);
        g.standing[owner] = -100.0f;
        double hostile = LiveMarket(g, g, b).BuyPrice(gd);
        CHECK(fabs(friendly / fair - 0.97) < 1e-6 && fabs(hostile / fair - 1.03) < 1e-6 && friendSells < friendly,
              "%s charges a friend %.1f, a stranger %.1f, an enemy %.1f - and never pays more than it charges", fs.Name(owner).c_str(), friendly, fair, hostile);
        g.standing[owner] = 0.0f;
        g.player.credits = 1e7;
        for (int k = 0; k < GOOD_COUNT; k++) g.player.cargo[k] = 0.0;
        int dear = -1;
        mk = LiveMarket(g, g, b);
        for (int k = 0; k < GOOD_COUNT; k++) if (mk.Sells(k) && mk.stock[k] > 2.0 * CARGO_CAPACITY && (dear < 0 || mk.price[k] > mk.price[dear])) dear = k;
        cr = g.player.credits;
        if (dear >= 0) BuyGoods(g, dear, 1e30);
        CHECK(dear >= 0 && g.standing[owner] > 0.0f && fabs(g.standing[owner] - (cr - g.player.credits) / 40000.0) < 0.01,
              "a hold of %s for %.0f cr: standing with %s up %.2f", dear >= 0 ? GoodData(dear).name : "?", cr - g.player.credits, fs.Name(owner).c_str(), g.standing[owner]);

        // Trade data: the markets this one trades with
        size_t knew = g.know.prices.size();
        cr = g.player.credits;
        bool boughtData = BuyTradeData(g, g);
        int elsewhere = 0;
        for (const auto &kv : g.know.prices) if (kv.second.sys != g.systems.home) elsewhere++;
        CHECK(boughtData && cr - g.player.credits == TRADE_DATA_COST && g.know.prices.size() > knew + 3 && elsewhere > 3,
              "trade data for %.0f cr: prices at %d markets, %d of them in other systems", TRADE_DATA_COST, (int)g.know.prices.size(), elsewhere);
    }

    // Contraband: lawful markets in a government's space won't touch what it bans; the rest don't ask
    {
        int banned = -1, gd = -1, lawless = -1;
        for (int i = 0; i < N && banned < 0; i++)
            for (int k = 0; k < GOOD_COUNT; k++) if (base.defs[i].banned[k] && !base.defs[i].station) { banned = i; gd = k; break; }
        if (banned >= 0)
            for (int i = 0; i < N && lawless < 0; i++)
                if (base.defs[i].ruler == base.defs[banned].ruler && !base.defs[i].banned[gd] && g.econ.state[i].target[gd] > 0.0f) lawless = i;
        CHECK(lawful > N / 10 && banned >= 0, "%d markets keep their government's bans (%s won't take %s)", lawful,
              banned >= 0 ? base.defs[banned].name.c_str() : "?", banned >= 0 ? GoodData(gd).name : "?");
        if (banned >= 0) {
            const MarketDef &d = base.defs[banned];
            g.player.ship.anchor = d.sys; g.player.ship.pos = {};
            UpdateLoadedSystem(g);
            Park(g, d.body, 0.9); Run(g, 0.1);
            g.player.cargo[gd] = 2.0;
            for (int k = 0; k < GOOD_COUNT; k++) if (k != gd) g.player.cargo[k] = 0.0;
            bool sold = SellGoods(g, gd, 1.0);
            std::string said = g.events.empty() ? "" : g.events.back().text;
            CHECK(TradePartner(g) && TradePartner(g)->id == d.bodyId && !sold && g.player.cargo[gd] == 2.0 &&
                  said.find("contraband") != std::string::npos && !BuyGoods(g, gd, 1.0), "at %s: \"%s\"", d.name.c_str(), said.c_str());
            CHECK(lawless >= 0, "...but %s, in the same space, deals in it", lawless >= 0 ? base.defs[lawless].name.c_str() : "nowhere");
        }
    }

    // The factions act on it, things happen locally, and it all makes the news
    {
        std::vector<float> tariff0 = g.econ.tariff;
        size_t news0 = g.news.size();
        RunEconomy(g, 14.0);
        int byFaction = 0, local = 0, tariffs = 0, embargoes = 0, raids = 0, buyouts = 0, boosts = 0, wars = 0;
        for (size_t i = news0; i < g.news.size(); i++) (g.news[i].faction == NO_FACTION ? local : byFaction)++;
        const int F = fs.Count();
        for (int f = 0; f < F; f++) {
            if (g.econ.tariff[f] != tariff0[f]) tariffs++;
            if (fs.list[f].kind == FactionKind::Clan && fabs(g.econ.raid[f] - 0.3f) > 0.01f) raids++;
            if (g.econ.outputUntil[f] > 0.0) boosts++;
            if (g.econ.pricingUntil[f] > 0.0) wars++;
            for (int o = 0; o < F; o++) if (g.econ.embargoUntil[f * F + o] > 0.0) embargoes++;
        }
        for (int i = 0; i < N; i++) if (g.econ.state[i].owner != base.defs[i].owner) buyouts++;
        CHECK(byFaction > 5 && local > 0 && tariffs + embargoes > 0 && raids > 0 && buyouts + boosts + wars > 0,
              "two weeks: %d faction moves and %d local events in the news; %d tariffs changed, %d embargoes, %d clans stirred up or put down, "
              "%d stations changed hands, %d subsidies, %d price wars", byFaction, local, tariffs, embargoes, raids, buyouts, boosts, wars);
        printf("        latest: \"%s\"\n", g.news.empty() ? "" : g.news.back().text.c_str());
        int wild = 0, counted = 0;
        for (int i = 0; i < N; i++)
            for (int k = 0; k < GOOD_COUNT; k++) {
                double p = MarketPrice(g, i, k);
                if (p <= 0.0) continue;
                counted++;
                if (p > 5.9 * GoodData(k).basePrice || p < 0.31 * GoodData(k).basePrice) wild++;
                if (!(g.econ.state[i].stock[k] >= 0.0f)) wild += 1000000;                // (never negative, never NaN)
            }
        CHECK(wild < counted / 4, "...and the markets hold together: %.1f%% of prices at a limit", 100.0 * wild / counted);

        // The same seed plays out the same way
        GameState h; NewGame(h);
        EnsureEconomy(h);
        RunEconomy(h, 14.0);
        double sumG = 0, sumH = 0;
        GameState fresh; NewGame(fresh); EnsureEconomy(fresh);
        for (int i = 0; i < N; i++) for (int k = 0; k < GOOD_COUNT; k++) { sumG += fresh.econ.state[i].stock[k]; sumH += h.econ.state[i].stock[k]; }
        GameState h2; NewGame(h2);
        EnsureEconomy(h2);
        RunEconomy(h2, 14.0);
        double sumH2 = 0;
        for (int i = 0; i < N; i++) for (int k = 0; k < GOOD_COUNT; k++) sumH2 += h2.econ.state[i].stock[k];
        CHECK(sumH == sumH2 && h.news.size() == h2.news.size() && (h.news.empty() || h.news.back().text == h2.news.back().text) && sumH != sumG,
              "the same seed plays out the same way (%d news items both times)", (int)h.news.size());
    }

    // Saved and loaded
    {
        g.player.ship.anchor = g.systems.home; g.player.ship.pos = {};
        UpdateLoadedSystem(g);
        Park(g, TradeWorld(g), 0.9); Run(g, 1.1);
        g.econ.state[home].stock[REF_FOOD] = 1234.5f;
        int f = base.defs[home].ruler;
        g.econ.tariff[f] = 0.17f;
        int taken = -1;
        for (int i = 0; i < N; i++) if (base.defs[i].station && base.defs[i].owner != 1) { taken = i; break; }
        if (taken >= 0) g.econ.state[taken].owner = 1;
        CHECK(SaveGame(g, "test_econ.txt"), "saved with the economy");
        GameState h; NewGame(h);
        bool loaded = LoadGame(h, "test_econ.txt");
        double diff = 0;
        if (loaded && h.econ.Built()) for (int i = 0; i < N; i++) for (int k = 0; k < GOOD_COUNT; k++) diff = fmax(diff, fabs(h.econ.state[i].stock[k] - g.econ.state[i].stock[k]) / fmax(1.0f, g.econ.state[i].stock[k]));
        bool embargoesMatch = loaded && h.econ.Built();
        for (size_t i = 0; embargoesMatch && i < g.econ.embargoUntil.size(); i++)                  // (the ones still in force)
            if (g.econ.embargoUntil[i] > g.clock && h.econ.embargoUntil[i] != g.econ.embargoUntil[i]) embargoesMatch = false;
        CHECK(loaded && h.econ.Built() && h.econ.state[home].stock[REF_FOOD] == 1234.5f && diff < 1e-4 && h.econ.tariff[f] == 0.17f &&
              (taken < 0 || h.econ.state[taken].owner == 1) && h.econ.shocks.size() == g.econ.shocks.size() && h.econ.raid == g.econ.raid && embargoesMatch,
              "stocks, tariffs, owners, raids, embargoes and shocks round-trip (worst stock off by %.4f%%)", diff * 100.0);
        CHECK(h.news.size() == g.news.size() && (g.news.empty() || h.news.back().text == g.news.back().text) && h.know.prices.size() == g.know.prices.size() &&
              h.know.prices.count(base.defs[home].bodyId) && h.know.prices[base.defs[home].bodyId].name == base.defs[home].name &&
              h.know.prices[base.defs[home].bodyId].price[REF_FOOD] == g.know.prices[base.defs[home].bodyId].price[REF_FOOD] &&
              h.know.prices[base.defs[home].bodyId].target[REF_FOOD] == g.econ.state[home].target[REF_FOOD],
              "...and so do the news (%d items) and the captain's price records (%d markets)", (int)h.news.size(), (int)h.know.prices.size());
        remove("test_econ.txt");
    }
}

// Contracts: boards drawn up from the economy, taking and doing them, what's at stake
static Contract &Inject(World &w, ContractKind kind, SysId sys, const Body &from, const Body &to, int good, float tonnes, float pay, float deposit)
{
    Contract k;
    k.id = ++w.contractSeq; k.kind = kind;
    k.fromBody = from.id; k.fromSys = sys; k.fromName = from.name;
    k.toBody = to.id; k.toSys = sys; k.toName = to.name;
    k.good = good; k.tonnes = tonnes; k.pay = pay; k.collateral = deposit;
    k.allowed = 600.0; k.expires = w.clock + 5000.0;
    w.contracts.push_back(k);
    return w.contracts.back();
}

static void TestContracts()
{
    GameState g; NewGame(g);
    Run(g, 0.1);
    EnsureEconomy(g);
    const FactionSet &fs = g.systems.factions;
    const EconBase &base = *g.econ.base;
    const SysId H = g.systems.home;
    int station = g.flight.nearBody, world = TradeWorld(g);
    CHECK(station >= 0 && world >= 0 && station != world, "docked at %s, with %s to run to", g.loaded.bodies[station].name.c_str(), g.loaded.bodies[world].name.c_str());
    if (station < 0 || world < 0) return;
    const Body A = g.loaded.bodies[station], B = g.loaded.bodies[world];
    int mA = g.econ.MarketOf(A.id), mB = g.econ.MarketOf(B.id);

    // The board comes over the comms, after the light delay, and is a copy as of then
    Hail(g, station); Run(g, 2.0);
    RequestContracts(g, g, station);
    CHECK(g.offers.empty() && !g.boards.count(A.id), "asked %s for work: nothing until the reply arrives", A.name.c_str());
    Run(g, 3.0);
    int here = 0;
    for (const Contract &k : g.offers) if (k.listKey == A.id && k.fromBody == A.id) here++;
    CHECK(g.boards.count(A.id) && here >= 3 && here == (int)g.offers.size(), "...then its board: %d contracts (\"%s\")", here,
          g.offers.empty() ? "" : ContractTitle(g.offers[0]).c_str());

    // Boards across human space: every kind of work turns up, and it all adds up
    {
        Captain all;                                               // (a member of every guild, to see everything)
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Guild) all.SetPaper(f.id, PAPER_MEMBER);
        int kinds[(int)ContractKind::Count] = {}, bad = 0, guild = 0, gated = 0, relief = 0, boards = 0, illicit = 0, unlabelled = 0;
        for (int i = 0; i < g.econ.Count(); i += 7) {
            Body fake; fake.id = base.defs[i].bodyId;
            RecordBoard(g, all, fake);
            boards++;
        }
        for (const Contract &k : all.offers) {
            kinds[(int)k.kind]++;
            if (k.guild != NO_FACTION) guild++;
            if (k.minStanding > 0.0f) gated++;
            if (k.effect == ContractEffect::Relief) relief++;
            bool cargo = k.kind == ContractKind::Haulage || k.kind == ContractKind::Supply || k.kind == ContractKind::Smuggling;
            if (k.pay <= 0.0f || k.allowed < 300.0 || k.expires <= g.clock || g.econ.MarketOf(k.fromBody) < 0 || g.econ.MarketOf(k.toBody) < 0 ||
                (cargo && (k.good < 0 || k.tonnes < 3.0f || k.tonnes > CARGO_CAPACITY)) ||
                ((k.kind == ContractKind::Haulage || k.kind == ContractKind::Smuggling) && k.collateral <= 0.0f) ||
                (k.kind == ContractKind::Survey && (!g.systems.Exists(k.surveySys) || g.systems[k.surveySys].develop > 0.0f || k.need < 2)) ||
                (k.kind == ContractKind::Smuggling && (fs.Get(k.against) == nullptr || k.fromBody == k.toBody))) bad++;
            // Whatever a government at either end bans is smuggling, and says so
            if (cargo) {
                int law = NO_FACTION;
                for (uint64_t body : { k.toBody, k.fromBody }) {
                    const Faction *ruler = fs.Get(base.defs[g.econ.MarketOf(body)].ruler);
                    if (ruler && std::find(ruler->contraband.begin(), ruler->contraband.end(), k.good) != ruler->contraband.end()) law = ruler->id;
                }
                if (law != NO_FACTION) illicit++;
                if ((law != NO_FACTION) != (k.law != NO_FACTION) || (law != NO_FACTION && (ContractLabel(k).find("Smuggling") != 0 || k.kind == ContractKind::Haulage || k.issuer == law)) ||
                    (law == NO_FACTION && ContractLabel(k).find("Smuggling") == 0)) unlabelled++;
            }
        }
        int n = (int)all.offers.size();
        CHECK(bad == 0 && n >= 3 * boards && kinds[0] > 0 && kinds[1] > 0 && kinds[2] > 0 && kinds[3] > 0 && kinds[4] > 0 && guild > 0 && gated > 0,
              "%d boards: %d contracts, all sound - %d haulage, %d supply, %d smuggling, %d survey, %d courier; %d for guild members, %d wanting standing",
              boards, n, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], guild, gated);
        CHECK(illicit > 0 && unlabelled == 0, "%d of them carry what a government at one end or the other bans: every one is labelled smuggling, and no lawful haulage or supply order does", illicit);
        Captain nobody;
        Body fake; fake.id = A.id;
        RecordBoard(g, nobody, fake);
        int open = 0, shut = 0;
        for (const Contract &k : nobody.offers) (k.guild == NO_FACTION ? open : shut)++;
        size_t before = g.contracts.size();
        g.clock += BOARD_REFRESH * 0.5; RecordBoard(g, nobody, fake);
        bool kept = g.contracts.size() == before;
        g.clock += BOARD_REFRESH; RecordBoard(g, nobody, fake);
        CHECK(open > 0 && shut == 0 && kept, "a captain in no guild sees only the open work (%d here); a board stands for a market day, then is drawn up afresh", open);
        g.clock += 3.0 * MARKET_DAY; Run(g, 6.0);
        CHECK(g.contracts.size() < before, "offers lapse (%d left of %d)", (int)g.contracts.size(), (int)before);
    }

    // Haulage: a deposit, their cargo, real stock moved, pay and standing
    g.player.credits = 5000.0;
    g.standing.assign(fs.Count(), 0.0f);
    for (int k = 0; k < GOOD_COUNT; k++) g.player.cargo[k] = 0.0;
    Park(g, station, 0.9); Run(g, 0.1);
    {
        Contract &k = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 20.0f, 1000.0f, 2000.0f);
        k.issuer = 0;
        uint32_t id = k.id;
        g.econ.state[mA].stock[REF_FOOD] = 200.0f;
        float stockA = g.econ.state[mA].stock[REF_FOOD], stockB = g.econ.state[mB].stock[REF_FOOD];
        bool took = AcceptContract(g, g, id);
        CHECK(took && g.jobs.size() == 1 && g.player.credits == 3000.0 && g.contracts.end() == std::find_if(g.contracts.begin(), g.contracts.end(),
              [&](const Contract &c) { return c.id == id; }) && !g.jobs[0].loaded, "accepted: off the board, 2,000 cr deposit down");
        CHECK(!AcceptContract(g, g, id), "...and nobody can take it twice");
        bool loadedOk = AdvanceContract(g, g, id);
        CHECK(loadedOk && g.player.cargo[REF_FOOD] == 20.0 && fabs(stockA - g.econ.state[mA].stock[REF_FOOD] - 20.0f) < 0.01f && g.jobs[0].loaded,
              "collected at the dock: 20 t of their food aboard, out of %s's stock", A.name.c_str());
        CHECK(!AdvanceContract(g, g, id) && g.jobs.size() == 1, "can't deliver from here");
        Park(g, world, 0.9); Run(g, 0.1);
        stockB = g.econ.state[mB].stock[REF_FOOD];
        bool here2 = false;
        JobStatus(g, g, g.jobs[0], &here2);
        bool delivered = AdvanceContract(g, g, id);
        CHECK(here2 && delivered && g.jobs.empty() && g.player.credits == 6000.0 && g.player.cargo[REF_FOOD] == 0.0 &&
              fabs(g.econ.state[mB].stock[REF_FOOD] - stockB - 20.0f) < 0.01f && g.standing[0] > 1.0f,
              "delivered: paid 1,000 cr, deposit back, the food is in %s's stock, standing with %s %+.1f", B.name.c_str(), fs.Name(0).c_str(), g.standing[0]);

        // Late: half pay. Very late: failed, and the deposit is gone (the cargo is yours).
        Park(g, station, 0.9); Run(g, 0.1);
        Contract &late = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 10.0f, 1000.0f, 1500.0f);
        late.issuer = 0; id = late.id;
        AcceptContract(g, g, id); AdvanceContract(g, g, id);
        Park(g, world, 0.9); Run(g, 0.1);
        g.clock += 700.0;
        float was = g.standing[0];
        double cr = g.player.credits;
        AdvanceContract(g, g, id);
        CHECK(g.player.credits - cr == 500.0 + 1500.0 && g.standing[0] == was, "ten minutes allowed, delivered after twelve: half pay and no thanks");
        Park(g, station, 0.9); Run(g, 0.1);
        Contract &lost = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 10.0f, 1000.0f, 1500.0f);
        lost.issuer = 0; id = lost.id;
        AcceptContract(g, g, id); AdvanceContract(g, g, id);
        cr = g.player.credits; was = g.standing[0];
        g.clock += 901.0; Run(g, 0.05);
        CHECK(g.jobs.empty() && g.player.credits == cr && g.player.cargo[REF_FOOD] == 10.0 && g.standing[0] < was - 2.0f,
              "never delivered: failed at 15 minutes, deposit forfeit, their 10 t is yours, standing %+.1f", g.standing[0] - was);
        g.player.cargo[REF_FOOD] = 0.0;
        // Handed back before collecting: deposit returned, back on the board
        Contract &back = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 10.0f, 1000.0f, 1500.0f);
        id = back.id;
        cr = g.player.credits;
        AcceptContract(g, g, id);
        bool gave = AbandonContract(g, g, id);
        CHECK(gave && g.jobs.empty() && g.player.credits == cr && std::any_of(g.contracts.begin(), g.contracts.end(), [&](const Contract &c) { return c.id == id; }),
              "handed back before collecting: deposit returned, and it's on the board again");
    }

    // Supply from your own hold; five at once and no more; standing, guilds and licences open the better work
    {
        Park(g, station, 0.9); Run(g, 0.1);
        std::vector<uint32_t> ids;
        for (int i = 0; i < MAX_JOBS + 1; i++) ids.push_back(Inject(g, ContractKind::Supply, H, A, A, ORE_IRON, 5.0f, 300.0f, 0.0f).id);
        int taken = 0;
        for (uint32_t id : ids) if (AcceptContract(g, g, id)) taken++;
        CHECK(taken == MAX_JOBS && (int)g.jobs.size() == MAX_JOBS, "%d contracts at once, and no more", MAX_JOBS);
        double cr = g.player.credits;
        bool short_ = AdvanceContract(g, g, ids[0]);
        g.player.cargo[ORE_IRON] = 7.0;
        bool full = AdvanceContract(g, g, ids[0]);
        CHECK(!short_ && full && g.player.cargo[ORE_IRON] == 2.0 && g.player.credits == cr + 300.0, "a supply order takes the goods from your own hold");
        for (uint32_t id : ids) AbandonContract(g, g, id);
        g.contracts.clear();
        g.standing.assign(fs.Count(), 0.0f);

        int guild = NO_FACTION, state = NO_FACTION;
        for (const Faction &f : fs.list) {
            if (guild == NO_FACTION && f.kind == FactionKind::Guild && f.guild == GuildKind::Traders) guild = f.id;
            if (state == NO_FACTION && f.kind == FactionKind::Government && !f.contraband.empty()) state = f.id;
        }
        Contract &ranked = Inject(g, ContractKind::Supply, H, A, A, ORE_IRON, 5.0f, 300.0f, 0.0f);
        ranked.issuer = 0; ranked.minStanding = 20.0f;
        uint32_t rid = ranked.id;
        bool barred = !AcceptContract(g, g, rid);
        g.standing[0] = 25.0f;
        CHECK(barred && AcceptContract(g, g, rid), "work that wants standing: \"%s\" - refused a stranger, given to a %s", ContractTerms(fs, g.jobs.back()).c_str(), RankName(fs.list[0], 25.0f));
        Contract &members = Inject(g, ContractKind::Supply, H, A, A, ORE_IRON, 5.0f, 300.0f, 0.0f);
        members.guild = members.issuer = guild;
        uint32_t mid = members.id;
        bool shut = !AcceptContract(g, g, mid);
        g.player.credits = 100.0;
        bool poor = !JoinGuild(g, g, guild);
        g.player.credits = 30000.0;
        bool joined = JoinGuild(g, g, guild);
        CHECK(shut && poor && joined && g.player.credits == 30000.0 - GUILD_FEE && (g.Papers(guild) & PAPER_MEMBER) && AcceptContract(g, g, mid) && !JoinGuild(g, g, guild),
              "%s work is for members: joined for %.0f cr, then took it", fs.Name(guild).c_str(), GUILD_FEE);
        Contract &bonded = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 10.0f, 900.0f, 1000.0f);
        bonded.licence = 0;
        uint32_t bid = bonded.id;
        bool noBond = !AcceptContract(g, g, bid);
        g.standing[0] = 0.0f;
        bool noRank = !BuyLicence(g, g, 0, PAPER_CARRIER);
        g.standing[0] = 25.0f;
        double cr2 = g.player.credits;
        bool bought = BuyLicence(g, g, 0, PAPER_CARRIER);
        CHECK(noBond && noRank && bought && cr2 - g.player.credits == CARRIER_COST && AcceptContract(g, g, bid),
              "high-value haulage wants a bonded carrier: the licence takes standing and %.0f cr", CARRIER_COST);
        CHECK(!JoinGuild(g, g, 0) && !BuyLicence(g, g, guild, PAPER_CARRIER) && !BuyLicence(g, g, 0, PAPER_PERMIT),
              "guilds don't sell licences, states don't take members, and the free port bans nothing to permit");
        while (!g.jobs.empty()) AbandonContract(g, g, g.jobs[0].id);
        g.player.cargo[REF_FOOD] = 0.0;

        // Customs, and the permit that makes it lawful
        int banned = -1, gd = -1;
        for (int i = 0; i < g.econ.Count() && banned < 0; i++)
            for (int k = 0; k < GOOD_COUNT; k++)
                if (base.defs[i].banned[k] && !base.defs[i].station && base.defs[i].ruler == state) { banned = i; gd = k; break; }
        CHECK(banned >= 0, "found a lawful port of %s (%s bans %s)", fs.Name(state).c_str(), banned >= 0 ? base.defs[banned].name.c_str() : "?", banned >= 0 ? GoodData(gd).name : "?");
        if (banned >= 0) {
            const MarketDef &d = base.defs[banned];
            g.player.ship.anchor = d.sys; g.player.ship.pos = {};
            UpdateLoadedSystem(g);
            g.standing[state] = 0.0f;
            int searched = 0, waved = 0;
            double fines = 0.0;
            for (int tries = 0; tries < 24; tries++) {
                Park(g, d.body, 5.0); Run(g, 0.05);
                g.player.cargo[gd] = 4.0; g.player.credits = 10000.0;
                g.clock += 7.0;
                Park(g, d.body, 0.9); Run(g, 0.05);
                if (g.player.cargo[gd] == 0.0) { searched++; fines += 10000.0 - g.player.credits; } else waved++;
            }
            CHECK(searched > 0 && fines > 0.0 && g.standing[state] < -3.0f, "docking at %s with %s aboard, 24 times: searched %d, waved through %d; fined %.0f cr in all, standing %+.0f",
                  d.name.c_str(), GoodData(gd).name, searched, waved, fines, g.standing[state]);
            g.standing[state] = 60.0f; g.player.credits = 50000.0;
            bool permit = BuyLicence(g, g, state, PAPER_PERMIT);
            int seized = 0;
            for (int tries = 0; tries < 12; tries++) {
                Park(g, d.body, 5.0); Run(g, 0.05);
                g.player.cargo[gd] = 4.0;
                g.clock += 7.0;
                Park(g, d.body, 0.9); Run(g, 0.05);
                if (g.player.cargo[gd] == 0.0) seized++;
            }
            double cr3 = g.player.credits;
            bool sold = SellGoods(g, gd, 4.0);
            CHECK(permit && seized == 0 && sold && g.player.credits > cr3 + 4.0 * GoodData(gd).basePrice,
                  "with %s's restricted goods permit: no searches, and the port buys it under licence (+%.0f cr for 4 t)", fs.Name(state).c_str(), g.player.credits - cr3);
        }
    }

    // Surveys: scan the worlds of an empty system from close by, and bring it back
    g.player.ship.anchor = H; g.player.ship.pos = {};
    UpdateLoadedSystem(g);
    Park(g, station, 0.9); Run(g, 0.1);
    {
        Captain all;
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Guild) all.SetPaper(f.id, PAPER_MEMBER);
        Contract survey;
        bool found = false;
        for (int i = 0; i < g.econ.Count() && !found; i++) {
            Body fake; fake.id = base.defs[i].bodyId;
            RecordBoard(g, all, fake);
            for (const Contract &k : all.offers) if (k.kind == ContractKind::Survey) { survey = k; found = true; break; }
        }
        CHECK(found, "found a survey on offer: %s", found ? ContractTitle(survey).c_str() : "none");
        if (found) {
            Contract &k = Inject(g, ContractKind::Survey, H, A, A, -1, 0.0f, 900.0f, 0.0f);
            k.surveySys = survey.surveySys; k.surveyName = survey.surveyName; k.need = survey.need; k.effect = ContractEffect::Charts;
            uint32_t id = k.id;
            AcceptContract(g, g, id);
            bool early = AdvanceContract(g, g, id);
            g.player.ship.anchor = survey.surveySys; g.player.ship.pos = {};
            UpdateLoadedSystem(g);
            Run(g, 0.05);
            int atStar = g.jobs[0].Scanned();
            for (int i = 0; i < (int)g.loaded.bodies.size() && g.jobs[0].Scanned() < survey.need; i++)
                if (g.loaded.bodies[i].kind == Kind::Planet) { Park(g, i, 0.5); Run(g, 0.05); }
            int scanned = g.jobs[0].Scanned();
            bool far = AdvanceContract(g, g, id);
            g.player.ship.anchor = H; g.player.ship.pos = {};
            UpdateLoadedSystem(g);
            Park(g, station, 0.9); Run(g, 0.1);
            double cr = g.player.credits;
            size_t news = g.news.size();
            bool done = AdvanceContract(g, g, id);
            CHECK(!early && atStar < survey.need && scanned == survey.need && !far && done && g.player.credits == cr + 900.0 && g.news.size() == news + 1,
                  "surveyed %d worlds of %s and brought it home: \"%s\"", scanned, survey.surveyName.c_str(), g.news.empty() ? "" : g.news.back().text.c_str());
        }
    }

    // Contracts that push the world: relief, and arms for a faction's fight
    {
        int clan = NO_FACTION;
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) { clan = f.id; break; }
        g.econ.shocks.push_back({ mA, 0.2f, 1.0f, g.clock + 4.0 * MARKET_DAY });
        double until = g.econ.shocks.back().until;
        Contract &relief = Inject(g, ContractKind::Supply, H, A, A, REF_FOOD, 5.0f, 500.0f, 0.0f);
        relief.effect = ContractEffect::Relief;
        uint32_t id = relief.id;
        g.player.cargo[REF_FOOD] = 10.0;
        AcceptContract(g, g, id); AdvanceContract(g, g, id);
        Contract &arms = Inject(g, ContractKind::Supply, H, A, A, REF_FOOD, 5.0f, 500.0f, 0.0f);
        arms.effect = ContractEffect::ArmPatrols; arms.effectOn = clan; arms.against = clan; arms.issuer = 0;
        id = arms.id;
        float raid = g.econ.raid[clan], regard = g.Standing(clan);
        AcceptContract(g, g, id); AdvanceContract(g, g, id);
        CHECK(g.econ.shocks.back().until == until - MARKET_DAY && g.econ.raid[clan] < raid && g.Standing(clan) < regard && g.jobs.empty(),
              "a relief run ends a strike a day sooner; resupplying patrols drives %s back (raiding %.2f to %.2f) and they don't forget it (%+.1f)",
              fs.Name(clan).c_str(), raid, g.econ.raid[clan], g.Standing(clan) - regard);
        g.econ.shocks.pop_back();
    }

    // A faction's list: its agenda's work and its worst shortages, for anyone to read
    {
        int lists = 0, total = 0, agenda = 0;
        for (const Faction &f : fs.list) {
            RequestFactionBoard(g, g, f.id);
            int n = 0;
            for (const Contract &k : g.offers) if (k.listKey == BoardKey(f.id)) { n++; if (k.effect == ContractEffect::ArmPatrols || k.effect == ContractEffect::ArmClan || k.against != NO_FACTION) agenda++; }
            if (n) lists++;
            total += n;
        }
        CHECK(lists > fs.Count() / 3 && agenda > 0, "%d of %d factions have work on their lists: %d contracts, %d of them from their agendas", lists, fs.Count(), total, agenda);
    }

    // Saved and loaded
    {
        Contract &k = Inject(g, ContractKind::Haulage, H, A, B, REF_FOOD, 10.0f, 1000.0f, 1500.0f);
        g.player.credits = 9000.0; g.player.cargo[REF_FOOD] = 0.0;
        uint32_t id = k.id;
        AcceptContract(g, g, id); AdvanceContract(g, g, id);
        CHECK(SaveGame(g, "test_contracts.txt"), "saved with contracts");
        GameState h; NewGame(h);
        bool loaded = LoadGame(h, "test_contracts.txt");
        CHECK(loaded && h.jobs.size() == 1 && h.jobs[0].id == id && h.jobs[0].loaded && h.jobs[0].toName == B.name && h.jobs[0].deadline == g.jobs[0].deadline &&
              h.jobs[0].pay == 1000.0f && h.papers == g.papers && h.offers.size() == g.offers.size() && h.boards.size() == g.boards.size() &&
              h.contracts.size() == g.contracts.size() && h.contractSeq == g.contractSeq && h.boardMade.size() == g.boardMade.size(),
              "your contracts, papers and what you'd read of the boards round-trip, and so do the boards themselves (%d on offer)", (int)h.contracts.size());
        remove("test_contracts.txt");
    }
}

// Somewhere in the loaded system a long way from any inhabited place
static DVec3 RandomPointFarFromPorts(const GameState &g)
{
    const StarSystem &s = g.systems[g.loaded.index];
    return s.u * (0.35 * s.fieldR) + s.n * (0.5 * s.fieldH);
}

// Space traffic: the ships that carry the economy where a captain is, and the ones that prey on and police them
static Npc &PutShip(World &w, NpcKind kind, SysId sys, DVec3 pos, int faction)
{
    Npc n;
    n.id = NPC_ID_BASE + (++w.npcSeq);
    n.kind = kind; n.sys = sys; n.pos = pos; n.goal = pos; n.faction = faction;
    n.name = kind == NpcKind::Pirate ? "Test raider" : kind == NpcKind::Patrol ? "Test patrol" : "Test freighter";
    n.doing = kind == NpcKind::Pirate ? NpcDo::Lurk : NpcDo::Dock;
    w.npcs.push_back(n);
    return w.npcs.back();
}

static void TestTraffic()
{
    GameState g; NewGame(g);
    Run(g, 6.0);
    const EconBase &base = *g.econ.base;
    const FactionSet &fs = g.systems.factions;
    const SysId H = g.systems.home;
    int hub = -1, tracked = 0, flown = 0;
    for (int i = 0; i < g.econ.Count(); i++) {
        if (base.defs[i].sys == H) hub = base.defs[i].hub;
        if (g.econ.zone[i] == 2) flown++;
        if (g.econ.zone[i] == 1) tracked++;
    }
    int kinds[4] = {}, elsewhere = 0;
    for (const Npc &n : g.npcs) { kinds[(int)n.kind]++; if (n.sys != H) elsewhere++; }
    CHECK(hub >= 0 && flown >= 5 && tracked >= 2 && kinds[(int)NpcKind::Miner] > 0 && kinds[(int)NpcKind::Patrol] > 0 && elsewhere == 0 &&
          g.traffic.size() == g.npcs.size(),
          "in the home system: %d markets flown, %d neighbouring hubs tracked; %d miners and %d patrols out already", flown, tracked,
          kinds[(int)NpcKind::Miner], kinds[(int)NpcKind::Patrol]);
    Run(g, 200.0);
    int most = 0, loaded = 0;
    for (int k = 0; k < 4; k++) kinds[k] = 0;
    for (const Npc &n : g.npcs) { kinds[(int)n.kind]++; if (n.total > 0.0f) loaded++; }
    most = (int)g.npcs.size();
    CHECK(kinds[0] >= 3 && most >= 8 && most <= 90 && loaded >= 3, "200 s on: %d ships in flight (%d freighters, %d miners, %d patrols, %d pirates), %d of them loaded; %d more under way between systems",
          most, kinds[0], kinds[3], kinds[1], kinds[2], loaded, (int)g.transits.size());

    // Goods in a ship are in nobody's stock until it docks
    int dest = -1, gd = -1;
    for (int i = 0; i < g.econ.Count() && dest < 0; i++) {
        if (base.defs[i].hub != hub || i == hub) continue;
        for (int k = FIRST_FINISHED; k < GOOD_COUNT; k++) if (g.econ.state[i].target[k] <= 0.0f) { dest = i; gd = k; break; }
    }
    CHECK(dest >= 0, "found a market that doesn't deal in something (%s, %s)", dest >= 0 ? base.defs[dest].name.c_str() : "?", dest >= 0 ? GoodData(gd).name : "?");
    if (dest < 0) return;
    float s0 = g.econ.state[dest].stock[gd];
    g.econ.Ship(hub, dest, gd, 60.0f, g.clock - 1000.0);           // (it's been on the dock a while: the next ship takes it)
    float afloat = g.econ.afloat[(size_t)dest * GOOD_COUNT + gd];
    const Npc *carrier = nullptr;
    for (int tries = 0; tries < 130 && !carrier; tries++) {         // (the next ship for there takes it)
        Run(g, 1.0);
        for (const Npc &n : g.npcs) if (n.kind == NpcKind::Freighter && n.from == hub && n.to == dest && n.cargo[gd] >= 60.0f) carrier = &n;
    }
    int id = carrier ? carrier->id : 0;
    const Traffic *seen = g.FindTraffic(id);
    CHECK(carrier && afloat == 60.0f && g.econ.state[dest].stock[gd] == s0 && seen && seen->npc && seen->dest == base.defs[dest].name && seen->tonnes >= 60.0f,
          "60 t on the dock at %s: the %s takes it aboard, bound for %s - on nobody's shelves yet", base.defs[hub].name.c_str(),
          carrier ? carrier->name.c_str() : "?", base.defs[dest].name.c_str());
    double waited = 0.0;
    while (FindNpc(g, id) && waited < 400.0) { Run(g, 5.0); waited += 5.0; }
    CHECK(!FindNpc(g, id) && g.econ.state[dest].stock[gd] == s0 + 60.0f && g.econ.afloat[(size_t)dest * GOOD_COUNT + gd] == 0.0f,
          "...it docked %.0f s later and the goods are in stock", waited);

    // Hailing a ship; reading a manifest from close by
    {
        const Npc *f = nullptr;
        for (int tries = 0; tries < 200 && !f; tries++) {
            Run(g, 1.0);
            for (const Npc &n : g.npcs) if (n.kind == NpcKind::Freighter && n.total > 5.0f && n.doing == NpcDo::Dock && n.close > 0.0 && n.until > g.clock + 6.0) f = &n;
        }
        CHECK(f != nullptr, "found a loaded freighter tied up and unloading (%s)", f ? f->name.c_str() : "none");
        if (f) {
            int fid = f->id;
            g.player.ship.anchor = H; g.player.ship.pos = f->pos + DVec3{ 300.0 * KM_U, 0, 0 }; g.player.ship.vel = {};
            Run(g, 0.05);
            std::vector<InfoRow> rows = TargetInfo(g, g, TargetId::MakeShip(fid));
            bool cargo = false;
            for (const InfoRow &r : rows) if (r.label == "Cargo" && r.value.find("t,") != std::string::npos) cargo = true;
            g.events.clear();
            bool hailed = HailShip(g, g, fid);
            Run(g, 3.0);
            bool reply = false;
            for (const GameEvent &e : g.events) if (e.text.find("Bound for") != std::string::npos) reply = true;
            CHECK(rows.size() >= 6 && cargo && hailed && reply && TargetName(g, TargetId::MakeShip(fid)) == rows[0].value,
                  "alongside the %s: its manifest can be read, and it answers a hail", rows.empty() ? "?" : rows[0].value.c_str());
        }
    }

    // A pirate robs a freighter: the cargo never arrives
    {
        Npc *f = nullptr;
        for (Npc &n : g.npcs) if (n.kind == NpcKind::Freighter && n.total > 20.0f && n.doing == NpcDo::Travel && !n.jumping) f = &n;
        int clan = NO_FACTION;
        for (const Faction &fa : fs.list) if (fa.kind == FactionKind::Clan) { clan = fa.id; break; }
        if (f) {
            int fid = f->id, to = f->to;
            int bulk = 0;
            for (int k = 0; k < GOOD_COUNT; k++) if (f->cargo[k] > f->cargo[bulk]) bulk = k;
            float had = f->total, was = g.econ.afloat[(size_t)to * GOOD_COUNT + bulk], carried = f->cargo[bulk];
            f->pos = RandomPointFarFromPorts(g);                   // (out in the open, a long way from help)
            f->goal = f->pos; f->vel = {};
            DVec3 at = f->pos;
            g.npcs.erase(std::remove_if(g.npcs.begin(), g.npcs.end(), [](const Npc &n) { return n.kind == NpcKind::Patrol; }), g.npcs.end());
            g.trafficZoned = g.clock;
            Npc &p = PutShip(g, NpcKind::Pirate, H, at + DVec3{ 100.0 * KM_U, 0, 0 }, clan);
            int pid = p.id;
            size_t news = g.news.size();
            g.trafficNews = -1e9;
            for (int tries = 0; tries < 80; tries++) {              // (until it's been robbed: other news may come in meanwhile)
                const Npc *victim = FindNpc(g, fid);
                if (!victim || victim->total < 0.9f * had) break;
                Run(g, 0.5);
            }
            const Npc *after = FindNpc(g, fid), *raider = FindNpc(g, pid);
            (void)was; (void)to;
            CHECK(after && fabs(after->total - 0.5f * had) < 0.01f * had && fabs(after->cargo[bulk] - 0.5f * carried) < 0.01f * carried + 0.01f &&
                  g.news.size() > news && (!raider || raider->doing == NpcDo::Leave),
                  "a raider caught the %s in the open: half its %.0f t gone for good  (\"%s\")", after ? after->name.c_str() : "?", had,
                  g.news.empty() ? "" : g.news.back().text.c_str());
        } else CHECK(false, "found a freighter for the raider to rob");
    }

    // Looked away from: everything in flight is settled at once
    {
        g.econ.Ship(hub, dest, gd, 40.0f, g.clock - 1000.0);
        Run(g, 20.0);
        size_t ships = g.npcs.size();
        double afloatAll = 0.0;
        for (float a : g.econ.afloat) afloatAll += a;
        g.player.ship.anchor = H; g.player.ship.pos = g.systems[H].n * (0.9 * LY_U); g.player.ship.vel = {};
        Run(g, 0.2);
        double left = 0.0;
        for (float a : g.econ.afloat) left += a;
        CHECK(ships > 0 && afloatAll > 40.0 && g.loaded.index == NO_SYSTEM && g.npcs.empty() && g.traffic.empty() && left < 1e-3 * afloatAll + 0.5 &&
              g.econ.state[dest].stock[gd] == s0 + 100.0f && g.econ.waiting.empty() && g.transits.empty(),
              "out in interstellar space: the %d ships and %.0f t that were in flight have landed; nothing is flown, tracked or lost", (int)ships, afloatAll);
    }

    // Pirates and a captain: out in an empty system, with a hold worth taking
    {
        int clan = NO_FACTION;
        for (const Faction &fa : fs.list) if (fa.kind == FactionKind::Clan) { clan = fa.id; break; }
        const StarSystem *empty = nullptr;                          // (off the edge of the bubble)
        for (int i = g.econ.Count() - 1; i >= 0 && !empty; i -= 5) {
            std::vector<const StarSystem *> near;
            g.systems.Nearby(base.defs[i].sys, {}, 9.0 * LY_U, near);
            for (const StarSystem *sys : near) if (sys->develop <= 0.0f && !empty) empty = sys;
        }
        CHECK(empty != nullptr, "found an empty system (%s)", empty ? empty->name.c_str() : "none");
        if (empty) {
            auto setUp = [&]() {
                g.npcs.clear();
                g.player.ship.anchor = empty->id; g.player.ship.pos = empty->u * (3.0 * AU_U); g.player.ship.vel = {}; g.player.ship.throttle = 0.0f;
                for (int k = 0; k < GOOD_COUNT; k++) g.player.cargo[k] = 0.0;
                g.player.cargo[ORE_IRON] = 30.0; g.player.cargo[ORE_PRECIOUS] = 10.0;
                g.pirateSafeUntil = 0.0; g.demandFrom = -1;
                g.standing.assign(fs.Count(), 0.0f);
                Run(g, 0.05);
                g.npcs.clear();
                g.trafficZoned = g.clock;
                g.events.clear();
                return PutShip(g, NpcKind::Pirate, empty->id, ShipLocal(g) + DVec3{ 3000.0 * KM_U, 0, 0 }, clan).id;
            };
            int pid = setUp();
            Run(g, 1.5);
            bool demanded = false;
            for (const GameEvent &e : g.events) if (e.text.find("heave to") != std::string::npos) demanded = true;
            const Npc *p = FindNpc(g, pid);
            const Traffic *t = g.FindTraffic(pid);
            CHECK(p && p->doing == NpcDo::Hunt && demanded && g.demandFrom == pid && g.demandTonnes == 10.0 && t && t->stance == STANCE_HOSTILE,
                  "a raider 3,000 km off sees 40 t in the hold: it closes, demands 10 t, and shows hostile");
            bool paid = PayPirates(g, g);
            p = FindNpc(g, pid);
            CHECK(paid && g.player.cargo[ORE_PRECIOUS] == 0.0 && g.player.cargo[ORE_IRON] == 30.0 && p && p->doing == NpcDo::Lurk && g.pirateSafeUntil > g.clock + 200.0,
                  "handed over (the dearest 10 t): it turns away and leaves you be for a while");
            pid = setUp();
            double total = g.player.CargoTotal();
            Run(g, 30.0);
            bool boarded = false;
            for (const GameEvent &e : g.events) if (e.text.find("boarded you") != std::string::npos) boarded = true;
            CHECK(boarded && fabs(g.player.CargoTotal() - total * (1.0 - PIRATE_SHARE)) < 1e-6 && g.player.cargo[ORE_PRECIOUS] == 0.0,
                  "sat still instead: boarded, and half the hold gone (%.0f t left of %.0f)", g.player.CargoTotal(), total);
            pid = setUp();
            g.standing[clan] = 30.0f;
            Run(g, 5.0);
            p = FindNpc(g, pid); t = g.FindTraffic(pid);
            bool tolerated = p && p->doing == NpcDo::Lurk && t && t->stance == STANCE_NEUTRAL;
            g.standing[clan] = 60.0f; Run(g, 0.05);
            t = g.FindTraffic(pid);
            CHECK(tolerated && t && t->stance == STANCE_ALLIED, "a captain the clan tolerates is left alone (neutral); a friend of the clan sees its ships as allies");
            // Running: a captain's drive outpaces anything
            pid = setUp();
            Run(g, 1.5);
            Controls run; run.hyper = true; run.throttle = 1.0f;
            Face(g, empty->u);
            Run(g, 25.0, run);
            p = FindNpc(g, pid);
            CHECK(g.player.CargoTotal() == total && (!p || p->doing != NpcDo::Hunt || Len(ShipLocal(g) - p->pos) > 0.5 * AU_U),
                  "or run for it: 25 s under hyper drive and the hold is untouched");
        }
    }

    // A patrol scans the hold: contraband is seized as at customs
    {
        g.player.ship.anchor = H; g.player.ship.pos = {};
        UpdateLoadedSystem(g);
        int state = NO_FACTION;
        for (const Faction &f : fs.list) if (state == NO_FACTION && f.kind == FactionKind::Government && !f.contraband.empty()) state = f.id;
        int gd2 = fs.list[state].contraband[0];
        g.player.ship.pos = g.systems[H].u * (20.0 * AU_U) + g.systems[H].n * (1.0 * AU_U); g.player.ship.vel = {}; g.player.ship.throttle = 0.0f;
        for (int k = 0; k < GOOD_COUNT; k++) g.player.cargo[k] = 0.0;
        g.player.cargo[gd2] = 6.0; g.player.credits = 20000.0; g.scanSafeUntil = 0.0;
        g.standing.assign(fs.Count(), 0.0f);
        Run(g, 0.05);
        Npc &p = PutShip(g, NpcKind::Patrol, H, ShipLocal(g) + DVec3{ 500.0 * KM_U, 0, 0 }, state);
        p.doing = NpcDo::Inspect; p.prey = g.id; p.since = g.clock;
        int pid = p.id;
        Run(g, 0.05);
        const Traffic *t = g.FindTraffic(pid);
        bool alerted = t && t->stance == STANCE_ALERTED;
        Run(g, 12.0);
        CHECK(alerted && g.player.cargo[gd2] == 0.0 && g.player.credits < 20000.0 && g.standing[state] < 0.0f && g.scanSafeUntil > g.clock,
              "a %s patrol closes (alerted), scans the hold and seizes 6 t of %s: fined %.0f cr", fs.Name(state).c_str(), GoodData(gd2).name, 20000.0 - g.player.credits);
        g.standing[state] = -70.0f; Run(g, 0.05);
        t = g.FindTraffic(pid);
        CHECK(t && t->stance == STANCE_HOSTILE, "...and to an enemy of the state its ships show hostile");
    }

    // A save has no ships in flight: what was afloat is saved as landed
    {
        g.player.ship.anchor = H; g.player.ship.pos = {};
        UpdateLoadedSystem(g);
        Park(g, TradeWorld(g), 0.9); Run(g, 0.5);
        g.econ.Ship(hub, dest, gd, 25.0f, g.clock);
        float before = g.econ.state[dest].stock[gd];
        SaveGame(g, "test_traffic.txt");
        GameState h; NewGame(h);
        bool ok = LoadGame(h, "test_traffic.txt");
        CHECK(ok && h.econ.state[dest].stock[gd] == before + 25.0f && h.npcs.empty() && h.econ.waiting.empty(),
              "saved with 25 t on a dock: loaded with it in stock at %s, and the lanes start afresh", base.defs[dest].name.c_str());
        remove("test_traffic.txt");
    }
}

// Thrust and mass, the reactor and its capacitors, the drives' power laws
static void TestPower()
{
    GameState g; NewGame(g);
    const SysId H = g.systems.home;
    const StarSystem &home = g.systems[H];
    Ship &ship = g.player.ship;
    Player &p = g.player;
    auto speed = [&]() { return (double)Vector3Length(ship.vel); };
    auto open = [&]() {                                           // out in the open, at rest, everything charged
        ship.anchor = H; ship.pos = home.n * (0.7 * LY_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.0f; ship.ftl = 1.0f;
        Face(g, home.n);
        p.fuel = p.spec.tank; p.power = PowerState();
        p.power.radar = false;                                    // (the radar's draw is its own test: sensors)
        Run(g, 0.05);
    };
    auto load = [&](double tonnes) { for (int k = 0; k < GOOD_COUNT; k++) p.cargo[k] = 0.0; p.cargo[ORE_IRON] = tonnes; };
    auto timeTo = [&](double v) {
        ship.throttle = 1.0f;
        double t = 0.0;
        while (speed() < v && t < 120.0) { Run(g, 0.05); t += 0.05; }
        return t;
    };

    // Mass: the same engines push a full hold about half as hard, and the thrusters swing it half as fast
    load(0.0); open();
    double massLight = p.Mass(), light = timeTo(0.5 * LIGHT_SEC_U);
    load(100.0); open();
    double massHeavy = p.Mass(), heavy = timeTo(0.5 * LIGHT_SEC_U);
    CHECK(fabs(massLight - (p.spec.dryMass + p.spec.tank)) < 0.5 && fabs(massHeavy - massLight - 100.0) < 0.5 && fabs(heavy / light - massHeavy / massLight) < 0.15,
          "to half light speed at full throttle: %.2f s at %.0f t, %.2f s at %.0f t (thrust over mass)", light, massLight, heavy, massHeavy);
    auto swing = [&]() {
        open();
        Vector3 before = ShipForward(ship);
        Controls hard; hard.yaw = 600.0f / (float)SIM_HZ;         // (far more than any thrusters can give)
        Run(g, 0.5, hard);
        return acosf(Clamp(Vector3DotProduct(before, ShipForward(ship)), -1.0f, 1.0f)) * RAD2DEG;
    };
    load(0.0);   float swingLight = swing();
    load(100.0); float swingHeavy = swing();
    CHECK(fabsf(swingLight - 0.5f * (float)(p.spec.turn / massLight)) < 2.0f && fabsf(swingHeavy - 0.5f * (float)(p.spec.turn / massHeavy)) < 2.0f,
          "hard over for half a second: %.0f degrees empty, %.0f with a full hold", swingLight, swingHeavy);
    // The last of it is the slow part (a = F / gamma^3 m)
    load(0.0); open();
    double to50 = timeTo(0.5 * LIGHT_SEC_U), to90 = to50 + timeTo(0.9 * LIGHT_SEC_U), to99 = to90 + timeTo(0.989 * LIGHT_SEC_U);
    CHECK(to90 > 2.5 * to50 && to99 > 2.5 * to90 && to99 > 17.0 && to99 < 23.0, "relativity: 0.5 c in %.1f s, 0.9 c in %.1f s, 0.99 c in %.1f s", to50, to90, to99);

    // The reactor burns fuel for what's drawn: rate = P / (efficiency x specific energy)
    open();
    double f0 = p.fuel;
    Run(g, 10.0);
    double idle = f0 - p.fuel;
    CHECK(fabs(idle - 10.0 * ReactorBurnFor(p.spec, p.spec.idleTW)) < 1e-6 && fabs(g.flight.load - p.spec.idleTW) < 1e-9 && g.flight.efficiency < 0.6 * p.spec.reactorEff,
          "sitting still: the reactor idles at %.0f TW, %.3f t of fuel in 10 s (turned right down it gets %.0f%% of the fuel's %.0f TJ/t, where flat out it gets %.0f%%)",
          g.flight.load, idle, g.flight.efficiency * 100.0, p.spec.fuelTJ, p.spec.reactorEff * 100.0);
    ship.throttle = 1.0f;
    Run(g, 2.0);
    CHECK(fabs(g.flight.load - (p.spec.idleTW + p.spec.engineTW)) < 0.01, "at full thrust it draws %.0f TW", g.flight.load);
    Run(g, 30.0);
    CHECK(fabs(g.flight.load - p.spec.idleTW) < 1e-9 && speed() > 0.989 * LIGHT_SEC_U, "once it's at the speed set, the engines shut down: back to %.0f TW at %.2f c", g.flight.load, speed() / LIGHT_SEC_U);
    ship.throttle = 0.0f;
    Run(g, 30.0);

    // Hyper: P = P0 + K m (v/c)^N. Run it slower and it draws far less.
    load(0.0);
    ship.anchor = H; ship.pos = home.u * (30.0 * AU_U) + home.n * (0.5 * AU_U); ship.vel = { 0, 0, 0 }; ship.throttle = 0.0f; ship.ftl = 1.0f;
    UpdateLoadedSystem(g);
    Face(g, home.v);
    p.power = PowerState();
    Controls hyper; hyper.hyper = true;
    Run(g, 6.0, hyper);
    double fullSpeed = speed(), fullDraw = g.flight.drivePower, law = p.spec.hyperP0 + p.spec.hyperK * p.Mass() * pow(fullSpeed / LIGHT_SEC_U, p.spec.hyperN);
    CHECK(g.flight.drive.inField && fabs(fullSpeed - p.spec.hyperSpeed) < 1e-3 * fullSpeed && fabs(fullDraw - law) < 0.01 * law &&
          p.power.driveCap > 0.99 * p.spec.driveCapTJ && g.flight.load < 0.5 * p.spec.reactorTW,
          "hyper in a system: %.0f c takes %.0f TW to hold (the reactor has %.0f); the drive capacitor isn't touched", fullSpeed / LIGHT_SEC_U, fullDraw, p.spec.reactorTW);
    Controls slower = hyper; slower.throttle = -1;
    Run(g, 1.5, slower);
    Run(g, 2.0, hyper);
    double half = ship.ftl;
    CHECK(half < 0.6f && half > 0.4f && fabs(speed() - half * p.spec.hyperSpeed) < 0.01 * speed() &&
          fabs((g.flight.drivePower - p.spec.hyperP0) / (fullDraw - p.spec.hyperP0) - pow(half, 3.0)) < 0.01,
          "S with the drive engaged sets it slower: at %.0f%% speed the field takes %.1f TW", half * 100.0f, g.flight.drivePower);
    Controls faster = hyper; faster.throttle = 1;
    Run(g, 3.0, faster);
    // It gets there at a steady rate, not all at once
    ship.vel = { 0, 0, 0 };
    Run(g, 0.5);
    Run(g, 1.0, hyper);
    CHECK(fabs(speed() - p.spec.hyperAccel * LIGHT_SEC_U) < 0.03 * speed(), "from rest the drive gains %.0f c every second", speed() / LIGHT_SEC_U);
    Run(g, 1.0);

    // Warp: the capacitor's charge forms the bubble; the reactor holds the field from there
    load(0.0); open();
    double cap0 = p.power.driveCap, need = JumpCharge(p.spec, p.Mass());
    Run(g, 0.1, hyper);
    double spent = cap0 - p.power.driveCap;
    Run(g, 8.0, hyper);
    double top = speed(), capMid = p.power.driveCap, wlaw = p.spec.warpP0 + p.spec.warpK * p.Mass() * pow(top / LIGHT_SEC_U, p.spec.warpN);
    double held = g.flight.drivePower;
    Run(g, 30.0, hyper);
    CHECK(fabs(spent - need) < 0.02 * need && fabs(need - p.spec.jumpTJ * massLight / p.spec.capOut) < 0.01 * need && p.power.warping &&
          top > 0.99 * p.spec.warpSpeed && speed() > 0.99 * p.spec.warpSpeed && !g.flight.driveStarved && p.power.driveCap >= capMid && cap0 / need > 1.5 && cap0 / need < 3.0,
          "empty hold: the jump to warp takes %.0f TJ from the capacitor (%.0f for the bubble, the rest lost getting it out): %.1f jumps to a charge",
          spent, p.spec.jumpTJ * massLight, cap0 / need);
    CHECK(fabs(held - wlaw) < 0.01 * wlaw && held > 0.45 * p.spec.reactorTW, "holding the field at %.1f million c takes %.0f TW of the reactor's %.0f", top / LIGHT_SEC_U / 1e6, held, p.spec.reactorTW);
    load(100.0); open();
    need = JumpCharge(p.spec, p.Mass());
    Run(g, 12.0, hyper);
    CHECK(p.power.warping && speed() > 0.9 * p.spec.warpSpeed && g.flight.drivePower > 0.9 * (p.spec.reactorTW - p.spec.idleTW) && p.spec.driveCapTJ / need > 1.0 && p.spec.driveCapTJ / need < 2.0,
          "full hold: %.0f TJ a jump (%.1f to a charge), and the field takes all but all the reactor has (%.0f TW, for %.0f%% of the drive's speed)",
          need, p.spec.driveCapTJ / need, g.flight.drivePower, 100.0 * speed() / p.spec.warpSpeed);
    // Charging: what goes in is less than what the reactor sends
    open();
    p.power.driveCap = 0.0; p.power.bank = p.spec.bankTJ;
    Run(g, 2.0);
    CHECK(fabs(p.power.driveCap - 2.0 * (p.spec.reactorTW - p.spec.idleTW) * p.spec.capIn) < 2.0,
          "an empty capacitor takes all the reactor can spare, and keeps %.0f%% of it (%.0f TJ in 2 s)", p.spec.capIn * 100.0, p.power.driveCap);
    // Without the charge for a jump the drive holds at hyper speed until it has it
    open();
    p.power.driveCap = 0.0;
    Run(g, 8.0, hyper);
    bool waiting = g.flight.driveCharging && !p.power.warping && speed() <= 1.001 * p.spec.hyperSpeed && speed() > 0.5 * p.spec.hyperSpeed;
    double t = 8.0;
    while (g.flight.driveCharging && t < 180.0) { Run(g, 0.5, hyper); t += 0.5; }
    Run(g, 8.0, hyper);
    CHECK(waiting && t > 10.0 && t < 120.0 && p.power.warping && speed() > 0.9 * p.spec.warpSpeed,
          "flat capacitor: the drive holds at hyper speed for %.0f s while it charges, then jumps", t);
    // Short of power: the drive speeds up as fast as ever, but can't go as fast
    load(100.0); open();
    p.spec.reactorTW = 1920.0;
    Run(g, 1.0, hyper);
    double gaining = speed();
    Run(g, 11.0, hyper);
    double limit = LIGHT_SEC_U * pow((p.spec.reactorTW - p.spec.idleTW - p.spec.warpP0) / (p.spec.warpK * p.Mass()), 1.0 / p.spec.warpN);
    CHECK(g.flight.driveStarved && p.power.warping && fabs(speed() - limit) < 0.01 * limit && speed() < 0.7 * p.spec.warpSpeed &&
          fabs(gaining - p.spec.warpAccel * LIGHT_SEC_U) < 0.05 * gaining,
          "the same load on a 1,920 TW reactor: at warp, but held to %.0f%% of the drive's speed", 100.0 * speed() / p.spec.warpSpeed);
    Refit(g);
    ship.vel = { 0, 0, 0 };
    Run(g, 1.0);
    // Too much ship for the drive
    ship.anchor = H; ship.pos = home.u * (30.0 * AU_U) + home.n * (0.5 * AU_U); ship.vel = { 0, 0, 0 };
    UpdateLoadedSystem(g);
    p.spec.hyperMaxMass = 100.0;
    Run(g, 0.5);
    Run(g, 2.0, hyper);
    CHECK(g.flight.driveTooHeavy && speed() < 1.0, "a ship heavier than its drive can move doesn't go to hyper");
    Refit(g);

    // Power modes: where the spare output goes
    load(0.0); open();
    Controls cycle; cycle.cyclePower = true;
    p.power.bank = p.power.driveCap = 0.0;
    Run(g, 2.0);
    double travelDrive = p.power.driveCap, travelBank = p.power.bank;
    Run(g, 1.0 / SIM_HZ, cycle);
    uint8_t second = p.power.mode;
    p.power.bank = p.power.driveCap = 0.0;
    Run(g, 2.0);
    CHECK(second == POWER_COMBAT && travelDrive > 2.5 * travelBank && p.power.bank > 2.5 * p.power.driveCap &&
          fabs(travelDrive / p.spec.capIn + travelBank - 2.0 * (p.spec.reactorTW - p.spec.idleTW)) < 5.0,
          "X cycles the power mode: TRAVEL sends three quarters of the spare output to the drive capacitor, COMBAT to the bank");
    Run(g, 1.0 / SIM_HZ, cycle);
    uint8_t third = p.power.mode;
    Run(g, 1.0 / SIM_HZ, cycle);
    uint8_t fourth = p.power.mode;
    Run(g, 1.0 / SIM_HZ, cycle);
    CHECK(third == POWER_DEFENCE && fourth == POWER_SILENT && p.power.mode == POWER_TRAVEL, "...then DEFENCE, SILENT, and round again to TRAVEL");

    // Out of fuel: emergency power, and a tanker if you want one
    ship.anchor = H; ship.pos = home.u * (6.0 * AU_U) + home.n * (0.3 * AU_U); ship.vel = { 0, 0, 0 };
    UpdateLoadedSystem(g);
    Face(g, home.v);
    p.fuel = 0.0; p.credits = 10000.0;
    Run(g, 4.0, hyper);
    CHECK(g.flight.drive.noFuel && g.flight.load == 0.0 && fabs(speed() - LIMP_SPEED) < 0.01 * LIMP_SPEED, "tank dry: the reactor is out, the drive limps on the emergency cell");
    Run(g, 2.0);
    g.events.clear();
    bool called = CallTanker(g, g);
    Run(g, 1.0);
    const Npc *tanker = nullptr;
    for (const Npc &n : g.npcs) if (n.kind == NpcKind::Tanker) tanker = &n;
    bool onWay = tanker != nullptr;
    t = 0.0;
    while (p.fuel <= 0.0 && t < 600.0) { Run(g, 2.0); t += 2.0; }
    double bill = TANKER_CALLOUT + TANKER_LOAD * 3.0 * GoodData(p.spec.fuelGood).basePrice;
    CHECK(called && onWay && fabs(p.fuel - TANKER_LOAD) < 0.5 && fabs(10000.0 - p.credits - bill) < 1.0,
          "called a tanker: alongside %.0f s later with %.0f t, for %.0f cr", t, p.fuel, 10000.0 - p.credits);
    CHECK(!CallTanker(g, g), "...and it won't come out for a ship with fuel in the tank");

    // NPC ships fly by the same rules: no faster than their drives, around what's in the way, burning fuel
    {
        GameState h; NewGame(h);
        Run(h, 120.0);
        int over = 0, inside = 0, burned = 0, flying = 0;
        for (int i = 0; i < 240; i++) {
            Run(h, 0.25);
            for (const Npc &n : h.npcs) {
                if (Len(n.vel) > 1.02 * n.spec.hyperSpeed * n.ship.ftl) over++;
                if (n.fuel < n.tank - 1e-6) burned++;
                if (Len(n.vel) > MAX_SPEED) flying++;
                for (const Body &b : h.loaded.bodies) if (Len(n.pos - b.local) < 0.98 * b.radius) inside++;
            }
        }
        CHECK(flying > 100 && over == 0 && inside == 0 && burned > 100,
              "a minute of home traffic, sampled 4 times a second: nobody over the speed their drive is set to (%d), nobody inside a planet or the star (%d); %d ship-samples in hyper, %d with fuel burned",
              over, inside, flying, burned);
    }
}

static bool base_target(const GameState &g, int market, int good)
{
    return market >= 0 && g.econ.state[market].target[good] > 0.0f;
}

// Ships built from parts: what they add up to, the yard, and the tachyons a warp exit gives off
static Part MakePart(PartType type, int size, int grade = 0, int variant = 0)
{
    Part p;
    p.type = type; p.size = (uint8_t)size; p.grade = (uint8_t)grade; p.variant = (uint8_t)variant;
    return p;
}

static void TestShips()
{
    GameState g; NewGame(g);
    Run(g, 0.5);
    EnsureEconomy(g);
    const FactionSet &fs = g.systems.factions;
    Player &p = g.player;

    // The starter ship is the sum of its parts
    ShipSpec d, s = SpecOf(StarterBuild());
    CHECK(fabs(s.dryMass - d.dryMass) < 0.01 && s.thrust == d.thrust && s.turn == d.turn && fabs(s.reactorTW - d.reactorTW) < 1e-6 && fabs(s.reactorEff - d.reactorEff) < 1e-9 && fabs(s.reactorBurn - d.reactorBurn) < 1e-12 &&
          s.fuelGood == REF_REACTOR_FUEL && d.fuelGood == REF_REACTOR_FUEL && fabs(s.idleTW - d.idleTW) < 1e-9 && s.engineTW == d.engineTW && s.bankTJ == d.bankTJ && s.driveCapTJ == d.driveCapTJ &&
          s.hyperSpeed == d.hyperSpeed && s.hyperP0 == d.hyperP0 && s.hyperMaxMass == d.hyperMaxMass && s.warpSpeed == d.warpSpeed && s.warpP0 == d.warpP0 &&
          s.jumpTJ == d.jumpTJ && s.tank == FUEL_CAPACITY && s.hold == CARGO_CAPACITY && s.laserRate == d.laserRate && s.scoopRate == d.scoopRate &&
          s.skimRate == d.skimRate && s.scan == d.scan && s.radar == d.radar && s.radarTW == d.radarTW && s.ir == d.ir && s.tachArea == d.tachArea &&
          s.tachDepth == d.tachDepth && s.tachEff == d.tachEff && s.tachRes == d.tachRes && s.hullSize == d.hullSize,
          "the starter ship: a %s with %d parts, %.1f t dry, %.0f TW, %.0f t of fuel, %.0f t of cargo", Hull(p.build.hull).name, (int)p.build.parts.size(),
          s.dryMass, s.reactorTW, s.tank, s.hold);
    CHECK(fabs(VolumeUsed(p.build) - 343.0) < 0.01 && BuildVolume(p.build) == 400.0 && HardpointsUsed(p.build) == 7 && BuildHardpoints(p.build) == 9,
          "it uses %.0f of its %.0f m3 and %d of its %d hardpoints", VolumeUsed(p.build), BuildVolume(p.build), HardpointsUsed(p.build), BuildHardpoints(p.build));
    // Size doubles a part; grade makes it lighter, better and much dearer
    {
        PartInfo a = Describe(MakePart(PartType::Reactor, 2)), b2 = Describe(MakePart(PartType::Reactor, 3)), c3 = Describe(MakePart(PartType::Reactor, 2, 3));
        ShipBuild one; one.parts = { MakePart(PartType::Reactor, 2, 3) };
        ShipSpec elite = SpecOf(one);
        CHECK(b2.mass == 2.0 * a.mass && b2.volume == 2.0 * a.volume && b2.cost == 2.0 * a.cost && c3.mass < 0.75 * a.mass && c3.volume == a.volume &&
              c3.cost == 15.0 * a.cost && elite.reactorTW > 1.4 * s.reactorTW && elite.reactorEff > s.reactorEff + 0.05,
              "a size 3 reactor is twice a size 2; an elite one is %.0f%% lighter, gives %.0f TW at %.0f%%, and costs 15 times as much",
              100.0 * (1.0 - c3.mass / a.mass), elite.reactorTW, elite.reactorEff * 100.0);
    }

    // Away from a yard nothing can be done
    CHECK(!Shipyard(g) && !FitPart(g, g, MakePart(PartType::Hold, 1)) && p.build.parts.size() == 16, "fitting out needs a shipyard or a starbase");
    int yardIndex = -1;
    for (int i = 0; i < (int)g.loaded.bodies.size(); i++) if (g.loaded.bodies[i].settlement == Settlement::Shipyard) { yardIndex = i; break; }
    CHECK(yardIndex >= 0, "the home system has a shipyard (%s)", yardIndex >= 0 ? g.loaded.bodies[yardIndex].name.c_str() : "none");
    if (yardIndex < 0) return;
    Park(g, yardIndex, 0.9); Run(g, 0.5);
    const Body *yard = Shipyard(g);
    CHECK(yard != nullptr, "docked at the yard");
    if (!yard) return;
    int market = g.econ.MarketOf(yard->id);
    for (int k = 0; k < GOOD_COUNT; k++) if (base_target(g, market, k)) g.econ.state[market].stock[k] = 500.0f;      // (well stocked)
    p.credits = 1e6;
    g.standing.assign(fs.Count(), 0.0f);

    // A cargo bay: paid for, built out of the yard's stock, and the ship is the heavier and roomier for it
    {
        Part bay = MakePart(PartType::Hold, 1);
        PartInfo info = Describe(bay);
        double price = PartPrice(g, g, *yard, bay), cr = p.credits, mass = p.spec.dryMass;
        float plating = g.econ.state[market].stock[GOOD_HULL_PLATING];
        bool fitted = FitPart(g, g, bay);
        CHECK(fitted && price > 0.0 && fabs(cr - p.credits - price) < 1e-6 && p.spec.hold == 125.0 && fabs(p.spec.dryMass - mass - info.mass) < 1e-9 &&
              fabs(VolumeUsed(p.build) - 373.0) < 0.01 && (!base_target(g, market, GOOD_HULL_PLATING) || fabs(plating - g.econ.state[market].stock[GOOD_HULL_PLATING] - info.tonnes) < 1e-3),
              "fitted a %s for %.0f cr: 125 t of hold now, %.1f t heavier, %.0f m3 used", info.name.c_str(), price, info.mass, VolumeUsed(p.build));
        // No room for a big one; no hardpoint for a third thing outside
        bool big = FitPart(g, g, MakePart(PartType::Hold, 4));
        bool laser = FitPart(g, g, MakePart(PartType::MiningLaser, 1)), scoop = FitPart(g, g, MakePart(PartType::Scoop, 1));
        bool another = FitPart(g, g, MakePart(PartType::MiningLaser, 1));
        CHECK(!big && laser && scoop && !another && p.spec.laserRate == 4.0 && p.spec.scoopRate == 1.0 && HardpointsUsed(p.build) == 9,
              "no room for a size 4 bay; a second mining laser and a scoop take the last two hardpoints, and a third thing has nowhere to go");
        // Taken out again: sold at a loss
        cr = p.credits;
        int last = (int)p.build.parts.size() - 1;
        bool removed = RemovePart(g, g, last);
        CHECK(removed && fabs(p.credits - cr - RESALE * Describe(MakePart(PartType::Scoop, 1)).cost) < 1e-6 && p.spec.scoopRate == 0.0,
              "taken out again: it fetches %.0f%% of its price", RESALE * 100.0);
        p.fuel = p.spec.tank;
        int tank = -1;
        for (int i = 0; i < (int)p.build.parts.size(); i++) if (p.build.parts[i].type == PartType::Tank && p.build.parts[i].size == 3) tank = i;
        CHECK(tank >= 0 && !RemovePart(g, g, tank) && p.spec.tank == 50.0, "a tank with fuel in it stays where it is");
    }

    // Stretching the hull: dearer every time, and only so far
    {
        double first = StretchCost(p.build, false), room = BuildVolume(p.build), dry = p.spec.dryMass;
        bool ok = StretchHull(g, g, false);
        double second = StretchCost(p.build, false);
        bool mount = StretchHull(g, g, true);
        int done = 1;
        while (StretchHull(g, g, false)) done++;
        CHECK(ok && mount && fabs(BuildVolume(p.build) / room - (1.0 + 0.12 * MAX_STRETCH)) < 1e-9 && second > 1.6 * first && done == MAX_STRETCH &&
              BuildHardpoints(p.build) == 10 && p.spec.dryMass > dry && first > 0.2 * Hull(p.build.hull).cost,
              "stretched %d times for %.0f m3 (the first cost %.0f cr, the second %.0f), and a tenth hardpoint welded on; then no more",
              done, BuildVolume(p.build), first, second);
    }

    // The three reactors: fission on reactor fuel (the starter's), deuterium fusion, the helium-3 fusor. Each gets more
    // from a tonne of its fuel and more power from the same room.
    {
        ShipBuild one;
        one.parts = { MakePart(PartType::Reactor, 2, 0, BURNS_REACTOR_FUEL) }; ShipSpec fis = SpecOf(one);
        one.parts = { MakePart(PartType::Reactor, 2, 0, BURNS_DEUTERIUM) };    ShipSpec deu = SpecOf(one);
        one.parts = { MakePart(PartType::Reactor, 2, 0, BURNS_HELIUM3) };      ShipSpec he3 = SpecOf(one);
        double costF = Describe(MakePart(PartType::Reactor, 2, 0, BURNS_REACTOR_FUEL)).cost, costD = Describe(MakePart(PartType::Reactor, 2, 0, BURNS_DEUTERIUM)).cost,
               costH = Describe(MakePart(PartType::Reactor, 2, 0, BURNS_HELIUM3)).cost;
        auto perTJ = [](const ShipSpec &r) { return GoodData(r.fuelGood).basePrice / (r.fuelTJ * r.reactorEff); };      // (credits a terajoule of power, flat out)
        CHECK(fis.fuelGood == REF_REACTOR_FUEL && deu.fuelGood == REF_FUEL && he3.fuelGood == RAW_HELIUM3 && std::string(GoodData(REF_FUEL).name) == "Deuterium" &&
              fis.reactorTW < deu.reactorTW && deu.reactorTW < he3.reactorTW && 4.0 * fis.fuelTJ < deu.fuelTJ && deu.fuelTJ < he3.fuelTJ && he3.fuelTJ < 1.1 * deu.fuelTJ &&
              costF < costD && costD < costH && perTJ(deu) < perTJ(fis) && perTJ(deu) < perTJ(he3) && he3.reactorEff > deu.reactorEff + 0.15 && deu.reactorEff > fis.reactorEff,
              "size 2 reactors: fission %.0f TW at %.0f%% of %.0f TJ/t (%.0f cr), deuterium fusion %.0f TW at %.0f%% of %.0f TJ/t (%.0f cr), helium-3 fusor %.0f TW at %.0f%% of %.0f TJ/t (%.0f cr): "
              "fusion fuel holds four times what fission fuel does, deuterium is the cheapest power, and helium-3 wastes least as heat",
              fis.reactorTW, fis.reactorEff * 100.0, fis.fuelTJ, costF, deu.reactorTW, deu.reactorEff * 100.0, deu.fuelTJ, costD, he3.reactorTW, he3.reactorEff * 100.0, he3.fuelTJ, costH);
        const GoodInfo &d2 = GoodData(REF_FUEL);
        CHECK(d2.recipe[0].good == REF_WATER && d2.recipe[0].tonnes >= 3.0f && d2.recipe[1].good == REF_CHEMICALS && GoodData(REF_REACTOR_FUEL).recipe[0].good == ORE_URANIUM,
              "deuterium is made from water, %.0f t of it and a little of the exchange towers' chemicals to the tonne; reactor fuel from uranium", d2.recipe[0].tonnes);

        int reactor = -1;
        for (int i = 0; i < (int)p.build.parts.size(); i++) if (p.build.parts[i].type == PartType::Reactor) reactor = i;
        bool mixed = FitPart(g, g, MakePart(PartType::Reactor, 1, 0, BURNS_DEUTERIUM));
        double perTonne = p.spec.reactorEff * p.spec.fuelTJ;
        p.fuel = 0.0;
        RemovePart(g, g, reactor);
        bool dead = p.spec.reactorTW == 0.0;
        bool fusion = FitPart(g, g, MakePart(PartType::Reactor, 2, 0, BURNS_DEUTERIUM));
        CHECK(!mixed && dead && fusion && p.spec.fuelGood == REF_FUEL && p.spec.reactorEff * p.spec.fuelTJ / perTonne > 4.0 && p.spec.reactorTW > 8000.0 && p.spec.reactorTW < 8600.0,
              "reactors aboard must share a fuel; swapped the fission reactor for a deuterium one: %.0f TW, and a tonne of %s goes %.1f times as far",
              p.spec.reactorTW, GoodData(p.spec.fuelGood).name, p.spec.reactorEff * p.spec.fuelTJ / perTonne);
        g.econ.state[market].stock[REF_FUEL] = 100.0f;
        p.fuel = 0.0;
        bool fuelled = base_target(g, market, REF_FUEL) ? BuyFuel(g, 5.0) : (p.fuel = 5.0, true);
        CHECK(fuelled && p.fuel == 5.0, "...and it's %s the fuel pump sells this ship now", GoodData(p.spec.fuelGood).name);
        // With a scoop, that's what a star gives
        GameState h; NewGame(h);
        Run(h, 0.2);
        h.player.build = p.build;
        h.player.build.parts.push_back(MakePart(PartType::Scoop, 1));
        Refit(h);
        const Body &star = h.loaded.bodies[0];
        h.player.ship.pos = star.local + h.systems[h.systems.home].u * (star.radius * 2.0); h.player.ship.vel = { 0, 0, 0 };
        h.player.fuel = 1.0;
        Run(h, 2.0);
        bool scooped = h.flight.scooping && h.player.fuel > 2.5;
        GameState f; NewGame(f);
        Run(f, 0.2);
        f.player.ship.pos = f.loaded.bodies[0].local + f.systems[f.systems.home].u * (f.loaded.bodies[0].radius * 2.0); f.player.ship.vel = { 0, 0, 0 };
        f.player.fuel = 1.0;
        Run(f, 2.0);
        CHECK(scooped && !f.flight.scooping && f.player.fuel < 1.0, "a scoop takes deuterium off a star for a ship that burns it; the starter's fission reactor has no use for one");
    }

    // Grades: what a yard sells depends on whose it is, and how well it knows you
    {
        int best = YardGrade(g, g, *yard, PartType::Engine);
        bool tooGood = best < PART_GRADES - 1 && FitPart(g, g, MakePart(PartType::Telescope, 1, PART_GRADES - 1));
        int company = NO_FACTION;
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Conglomerate && Specialises(f.slant, PartType::Reactor) && company == NO_FACTION) company = f.id;
        Body theirs = *yard;
        theirs.owner = company;
        int stranger = YardGrade(g, g, theirs, PartType::Reactor), other = YardGrade(g, g, theirs, PartType::Telescope);
        g.standing[company] = 30.0f;
        int known = YardGrade(g, g, theirs, PartType::Reactor);
        CHECK(best >= 1 && !tooGood && company != NO_FACTION && stranger == 2 && known == 3 && other == 2 && YardMaker(g, theirs, PartType::Reactor) == company &&
              YardMaker(g, theirs, PartType::Telescope) == NO_FACTION,
              "%s sells up to %s grade; a yard of %s sells its own reactors up to %s, and %s to a customer it knows", yard->name.c_str(), GradeName(best),
              fs.Name(company).c_str(), GradeName(stranger), GradeName(known));
        // Out of what it's made of, a yard can't build it
        if (base_target(g, market, GOOD_SHIP_COMPONENTS)) {
            g.econ.state[market].stock[GOOD_SHIP_COMPONENTS] = 0.0f;
            CHECK(PartPrice(g, g, *yard, MakePart(PartType::Thrusters, 1)) < 0.0 && !FitPart(g, g, MakePart(PartType::Thrusters, 1)),
                  "with no ship components in stock the yard can't build thrusters");
            g.econ.state[market].stock[GOOD_SHIP_COMPONENTS] = 500.0f;
        }
    }

    // A bigger hull: what's fitted moves across, the old one is traded in; a smaller one won't take it all
    {
        size_t parts = p.build.parts.size();
        double cr = p.credits, quoted = HullPrice(g, g, *yard, HULL_FREIGHTER);
        bool small = BuyHull(g, g, HULL_COURIER);
        bool bought = BuyHull(g, g, HULL_FREIGHTER);
        CHECK(!small && bought && p.build.hull == HULL_FREIGHTER && p.build.parts.size() == parts && p.build.stretch == 0 && BuildVolume(p.build) == 1800.0 &&
              fabs(cr - p.credits - quoted) < 1e-6 && quoted < Hull(HULL_FREIGHTER).cost * 2.0 && p.spec.dryMass > 100.0,
              "a courier is too small for all this; traded up to a freighter for %.0f cr with the old hull's value off: %.0f t dry", quoted, p.spec.dryMass);
        while (FitPart(g, g, MakePart(PartType::Hold, 4))) {}
        double full = p.spec.dryMass + p.spec.tank + p.spec.hold;
        CHECK(p.spec.hold >= 1000.0 && full > p.spec.hyperMaxMass, "filled with cargo bays: %.0f t of hold, and at %.0f t full she's too much for the old drive (good to %.0f t)",
              p.spec.hold, full, p.spec.hyperMaxMass);
    }

    // The mining laser is a part like any other
    {
        GameState h; NewGame(h);
        Run(h, 0.2);
        for (size_t i = 0; i < h.player.build.parts.size();) if (h.player.build.parts[i].type == PartType::MiningLaser) h.player.build.parts.erase(h.player.build.parts.begin() + i); else i++;
        Refit(h);
        Controls fire; fire.fire = true;
        Run(h, 0.1, fire);
        CHECK(h.player.spec.laserRate == 0.0 && h.flight.miningStatus == "NO MINING LASER FITTED", "without a mining laser fitted: \"%s\"", h.flight.miningStatus.c_str());
    }

    // NPC ships are built from the same parts
    {
        ShipSpec bulk = SpecOf(NpcBuild(NpcKind::Freighter, 3000.0)), small = SpecOf(NpcBuild(NpcKind::Freighter, 60.0)), raider = SpecOf(NpcBuild(NpcKind::Pirate, 0.0));
        double full = bulk.dryMass + bulk.tank + 3000.0, crew = 0.45 * bulk.hyperSpeed;
        CHECK(bulk.hold >= 3000.0 && small.hold >= 60.0 && small.hold <= 100.0 && bulk.hyperMaxMass >= full && bulk.thrust > 0.0 &&
              DrivePower(bulk, crew, full, false) < bulk.reactorTW - bulk.idleTW && raider.hyperSpeed > 0.0 && raider.dryMass < 0.5 * small.dryMass + 30.0,
              "a 3,000 t freighter: %.0f t dry, a %.0f TW reactor, a drive good for %.0f t; a raider: %.0f t dry", bulk.dryMass, bulk.reactorTW, bulk.hyperMaxMass, raider.dryMass);
    }

    // Saved and loaded
    {
        CHECK(SaveGame(g, "test_ship.txt"), "saved with the refitted ship");
        GameState h; NewGame(h);
        bool loaded = LoadGame(h, "test_ship.txt");
        CHECK(loaded && h.player.build.hull == p.build.hull && h.player.build.parts == p.build.parts && h.player.build.mounts == p.build.mounts &&
              h.player.spec.hold == p.spec.hold && h.player.spec.fuelGood == p.spec.fuelGood && fabs(h.player.spec.dryMass - p.spec.dryMass) < 1e-9,
              "the ship round-trips: a %s with %d parts", Hull(h.player.build.hull).name, (int)h.player.build.parts.size());
        remove("test_ship.txt");
    }

}

// ---------------------------------------------------------------------------
// Sensors: tachyon bursts and who catches them, radar, infrared and telescopes,
// contacts and ghosts, running silent, sensor networks and traffic control
// ---------------------------------------------------------------------------
static double AngleBetween(DVec3 a, DVec3 b) { return atan2(Len(Cross(a, b)), Dot(a, b)) * RAD2DEG; }

static void TestSensors()
{
    const TachyonSensor ship = ShipDetector(ShipSpec()), outpost = OutpostDetector(), port = PortDetector();
    const double small = TACH_E0_TJ, big = 50.0 * TACH_E0_TJ;       // a 100 t ship, and a 5,000 t one

    // --- The physics ---
    {
        double near = TachyonExpected(ship, small, 0.5), twice = TachyonExpected(ship, small, 1.0);
        double far = TachyonExpected(ship, small, 80.0), further = TachyonExpected(ship, small, 160.0);
        CHECK(fabs(TachyonExpected(ship, small, 5.0) - 3.0) < 0.15 && fabs(near / twice - 4.0 * exp(0.5 / TACH_L0_AU)) < 0.01 &&
              fabs(far / further - 4.0 * exp(80.0 / TACH_L0_AU)) < 0.01 * far / further,
              "a small ship's burst: %.1f counts expected in a basic detector 5 AU off; inverse square close in (x%.2f for half the distance), and decay on top of it far out (x%.0f from 160 AU to 80)",
              TachyonExpected(ship, small, 5.0), near / twice, far / further);
        CHECK(fabs(TachyonDecayAU(small) - TACH_L0_AU) < 1e-9 && fabs(TachyonDecayAU(big) / TachyonDecayAU(small) - sqrt(50.0)) < 1e-9 &&
              fabs(TachyonCount(big) / TachyonCount(small) - 50.0) < 1e-9 && TachyonExpected(ship, big, 5.0) > 5.0 * TachyonExpected(ship, small, 5.0) &&
              TachyonExpected(ship, big, 60.0) > 20.0 * TachyonExpected(ship, small, 60.0),
              "a ship fifty times the mass: fifty times the tachyons, and they go %.0f AU, not %.0f; %.0f times the count at 5 AU, %.0f times at 60",
              TachyonDecayAU(big), TachyonDecayAU(small), TachyonExpected(ship, big, 5.0) / TachyonExpected(ship, small, 5.0),
              TachyonExpected(ship, big, 60.0) / TachyonExpected(ship, small, 60.0));
        // Counting is Poisson, and repeatable from the same seed
        uint64_t dice = 7, again = 7;
        double sum = 0.0, sq = 0.0;
        bool same = true;
        for (int i = 0; i < 20000; i++) { int k = Poisson(2.5, dice); sum += k; sq += (double)k * k; if (i < 100 && Poisson(2.5, again) != k) same = false; }
        double mean = sum / 20000.0, var = sq / 20000.0 - mean * mean;
        CHECK(fabs(mean - 2.5) < 0.05 && fabs(var - 2.5) < 0.12 && same, "counts are Poisson: 20,000 draws at 2.5 average %.3f, variance %.3f; the same seed gives the same draws", mean, var);
    }

    // --- Detection is a matter of odds that fall away with distance, never a hard range ---
    {
        uint64_t dice = 11;
        auto caught = [&](const TachyonSensor &s, double mj, double au, int tries) {
            int n = 0;
            TachyonDetection d;
            for (int i = 0; i < tries; i++) if (DetectBurst(s, {}, DVec3{ au * AU_U, 0, 0 }, mj, dice, d)) n++;
            return n;
        };
        int at2 = caught(ship, small, 2.0, 300), at6 = caught(ship, small, 6.0, 300), at10 = caught(ship, small, 10.0, 300), at40 = caught(ship, small, 40.0, 300);
        CHECK(at2 == 300 && at6 > 60 && at6 < 220 && at10 < at6 && at10 > 0 && at40 <= 3,
              "a basic detector and a small ship's arrival: caught %d times in 300 at 2 AU, %d at 6, %d at 10, %d at 40 (the odds by sum: %.0f%%, %.0f%%, %.0f%%)",
              at2, at6, at10, at40, 100.0 * TachyonChance(ship, small, 2.0), 100.0 * TachyonChance(ship, small, 6.0), 100.0 * TachyonChance(ship, small, 10.0));
        int smallAt15 = caught(ship, small, 15.0, 300), bigAt15 = caught(ship, big, 15.0, 300);
        int postAt40 = caught(outpost, small, 40.0, 300), postAt80 = caught(outpost, small, 80.0, 300), portAt20 = caught(port, small, 20.0, 300);
        CHECK(smallAt15 < 20 && bigAt15 > 90 && postAt40 == 300 && postAt80 > 150 && portAt20 > 150,
              "a big ship is caught from much further off (%d times in 300 at 15 AU, where the small one is caught %d); a state's outpost catches the small one every time at 40 AU and %d times at 80; "
              "a port's own detector, %d times at 20", bigAt15, smallAt15, postAt80, portAt20);
        int stealth = caught(outpost, small, 400.0, 300) + caught(outpost, big, 1500.0, 300);
        CHECK(stealth == 0 && TachyonExpected(outpost, small, 400.0) < 1e-3, "dropping out hundreds of AU beyond the edge, nothing catches it (%.5f counts expected at an outpost 400 AU away)",
              TachyonExpected(outpost, small, 400.0));

        // A bearing, better with more counts; and a range the count can only bracket
        auto spread = [&](const TachyonSensor &s, double au, double &inBand) {
            double sum = 0.0; int n = 0, in = 0;
            TachyonDetection d;
            DVec3 at = { au * AU_U, 0, 0 };
            for (int i = 0; i < 400; i++) {
                if (!DetectBurst(s, {}, at, small, dice, d)) continue;
                sum += AngleBetween(d.bearing, at); n++;
                if (d.rLo <= au * AU_U && au * AU_U <= d.rHi) in++;
            }
            inBand = n ? (double)in / n : 0.0;
            return n ? sum / n : 0.0;
        };
        double band1, band4, bandPost;
        double err1 = spread(ship, 1.0, band1), err4 = spread(ship, 4.0, band4), errPost = spread(outpost, 30.0, bandPost);
        TachyonDetection omni;
        bool heard = DetectBurst(port, {}, DVec3{ 3.0 * AU_U, 0, 0 }, small, dice, omni);
        CHECK(err1 < 2.5 && err4 > 2.0 * err1 && err4 < 12.0 && errPost < 0.5 && band1 > 0.95 && band4 > 0.95 && heard && !omni.directional,
              "the bearing is good to %.1f degrees at 1 AU and %.1f at 4 (fewer counts); an outpost's to %.2f at 30 AU; the true range is inside the bracket %.0f%% of the time; a port's detector gives no direction",
              err1, err4, errPost, 50.0 * (band1 + band4));

        // Two or more bearings from well apart cross at a place
        std::vector<DVec3> posts = { { 40.0 * AU_U, 0, 0 }, { -20.0 * AU_U, 0, 35.0 * AU_U }, { -20.0 * AU_U, 0, -35.0 * AU_U }, { 0, 6.0 * AU_U, 0 } };
        DVec3 burst = { 10.0 * AU_U, 1.0 * AU_U, 25.0 * AU_U };
        double worst = 0.0, claimed = 0.0;
        int fixes = 0;
        for (int i = 0; i < 100; i++) {
            std::vector<TachyonDetection> got;
            for (DVec3 at : posts) { TachyonDetection d; if (DetectBurst(outpost, at, burst, small, dice, d)) got.push_back(d); }
            DVec3 fix; double err;
            if (!Triangulate(got, fix, err)) continue;
            fixes++; worst = fmax(worst, Len(fix - burst)); claimed += err;
        }
        std::vector<TachyonDetection> one(1), parallel(2);
        one[0].directional = true; one[0].bearing = { 1, 0, 0 }; one[0].sigma = 1.0;
        parallel[0] = one[0]; parallel[1] = one[0]; parallel[1].from = { 0, 1000.0, 0 };
        DVec3 fix; double err;
        CHECK(fixes == 100 && worst < 1.0 * AU_U && worst < 4.0 * claimed / 100.0 && claimed / 100.0 < 0.5 * AU_U && claimed / 100.0 > 0.01 * AU_U && !Triangulate(one, fix, err) && !Triangulate(parallel, fix, err),
              "four outposts fix a small ship's arrival every time, never more than %.2f AU out (they claim +/- %.2f); one bearing alone, or two side by side, fix nothing",
              worst / AU_U, claimed / 100.0 / AU_U);
    }

    // --- Radar, infrared, telescope ---
    {
        SensorFit basic = { 1.0, 1.0, 1.0, true }, passive = { 1.0, 1.0, 1.0, false };
        Signature hauler, bulk, hot, cold;
        hauler.heat = 1275.0; bulk = hauler; bulk.size = Hull(HULL_BULK).volume / Hull(HULL_HAULER).volume;
        hot = hauler; hot.heat = 20400.0; cold = hauler; cold.heat = 85.0;
        uint8_t by = 0;
        int far = Sense(basic, hauler, 1.6 * AU_U), found = Sense(basic, hauler, 1.4 * AU_U, 1.0, &by), known = Sense(basic, hauler, 1.0 * AU_U), named = Sense(basic, hauler, 0.2 * AU_U);
        CHECK(far == CONTACT_NONE && found == CONTACT_DETECTED && by == BY_RADAR && known == CONTACT_CLASSIFIED && named == CONTACT_IDENTIFIED &&
              Sense(basic, hauler, 500.0 * KM_U) == CONTACT_IDENTIFIED,
              "a hauler: nothing at 1.6 AU, a radar contact at 1.4, classified at 1.0, identified by telescope at 0.2");
        CHECK(Sense(basic, bulk, 2.8 * AU_U) == CONTACT_DETECTED && Sense(basic, bulk, 0.5 * AU_U) == CONTACT_IDENTIFIED &&
              fabs(RadarRange(1.0, bulk.size) / RadarRange(1.0, 1.0) - pow(bulk.size, 0.25)) < 1e-9 && RadarRange(1.0, 150.0 / 400.0) < 0.8 * RadarRange(1.0, 1.0),
              "size matters: a bulk carrier shows on radar at %.2f AU and to the eye at %.2f; a courier at %.2f and %.2f", RadarRange(1.0, bulk.size) / AU_U,
              ScopeRange(1.0, bulk.size) / AU_U, RadarRange(1.0, 0.375) / AU_U, ScopeRange(1.0, 0.375) / AU_U);
        CHECK(Sense(passive, hauler, 1.4 * AU_U) == CONTACT_NONE && Sense(passive, hot, 3.0 * AU_U, 1.0, &by) == CONTACT_DETECTED && by == BY_IR &&
              Sense(passive, cold, 0.3 * AU_U) == CONTACT_NONE && Sense(passive, cold, 0.2 * AU_U) > CONTACT_NONE,
              "with the radar off there's only heat to go by: a ship working hard shows at %.1f AU, one running cold at %.2f", IrRange(1.0, hot.heat) / AU_U, IrRange(1.0, cold.heat) / AU_U);
        Signature emitter = hauler; emitter.radar = true;
        CHECK(HearsRadar(emitter, 4.0 * AU_U) && !HearsRadar(emitter, 5.0 * AU_U) && !HearsRadar(hauler, 0.5 * AU_U) && Sense(basic, hauler, 4.0 * AU_U) == CONTACT_NONE,
              "a radar is heard at 4 AU, far beyond where it can see; a ship that isn't transmitting isn't heard at all");
        CHECK(Sense(basic, hauler, 1.55 * AU_U) == CONTACT_NONE && Sense(basic, hauler, 1.55 * AU_U, HOLD_GAIN) == CONTACT_DETECTED,
              "a contact already held is kept a little past where it would be found");
    }

    // --- In the game: only what the sensors hold is on the scope ---
    GameState h; NewGame(h);
    Run(h, 0.5);
    const SysId H = h.systems.home;
    const StarSystem &home = h.systems[H];
    Player &p = h.player;
    auto shown = [&](int id) -> const Traffic * { for (const Traffic &t : h.traffic) if (t.id == id) return &t; return nullptr; };
    auto quiet = [&]() { h.npcs.clear(); h.transits.clear(); h.econ.waiting.clear(); h.trafficZoned = h.clock; h.contacts.clear(); h.marks.clear(); };
    auto away = [&]() {                                              // out in the open, far from every port, at rest
        p.ship.anchor = H; p.ship.pos = RandomPointFarFromPorts(h); p.ship.vel = { 0, 0, 0 }; p.ship.throttle = 0.0f;
        p.power = PowerState(); p.fuel = p.spec.tank;
        for (int k = 0; k < GOOD_COUNT; k++) p.cargo[k] = 0.0;
        Run(h, 0.05);
    };
    int clan = NO_FACTION;
    for (const Faction &f : h.systems.factions.list) if (f.kind == FactionKind::Clan) { clan = f.id; break; }
    away(); quiet();
    DVec3 here = ShipLocal(h);
    int rid = PutShip(h, NpcKind::Pirate, H, here + home.v * (3.0 * AU_U), clan).id;
    auto put = [&](int id, double au, double seconds = 0.1) {
        Npc *n = const_cast<Npc *>(FindNpc(h, id));
        if (n) { n->pos = here + home.v * (au * AU_U); n->goal = n->pos; n->vel = {}; n->until = h.clock + 1e6; }
        h.trafficZoned = h.clock;
        Run(h, seconds);
        return shown(id);
    };
    {
        bool none = put(rid, 3.0) == nullptr && h.trafficControl.empty();
        const Traffic *t = put(rid, 1.0);
        bool blip = t && t->level == CONTACT_DETECTED && t->by == BY_RADAR && t->name == "Unknown contact" && t->faction == NO_FACTION && !t->ghost;
        t = put(rid, 0.7);
        bool kind = t && t->level == CONTACT_CLASSIFIED && t->name == "Unidentified raider" && t->faction == NO_FACTION && t->hull == HULL_COURIER;
        t = put(rid, 0.1);
        bool named = t && t->level == CONTACT_IDENTIFIED && t->name == "Test raider" && t->faction == clan && (t->by & BY_SCOPE);
        t = put(rid, 0.7);
        bool kept = t && t->level == CONTACT_IDENTIFIED && t->name == "Test raider";     // (still tracked: still known)
        CHECK(none && blip && kind && named && kept,
              "a raider 3 AU off isn't on the scope; at 1 AU it's an unknown radar contact; at 0.7, an unidentified raider; at 0.1 the telescope names it, and it stays named while it's tracked");
        DVec3 rel;
        bool target = FindTarget(h, TargetId::MakeShip(rid), rel);
        t = put(rid, 3.0);
        bool ghost = t && t->ghost && t->name == "Test raider" && Len(t->ship.pos - (here + home.v * (0.7 * AU_U))) < 1000.0 * KM_U && t->age < 1.0f;
        bool dropped = !FindTarget(h, TargetId::MakeShip(rid), rel);
        t = put(rid, 3.0, 30.0);
        bool fading = t && t->ghost && t->age > 29.0f;
        t = put(rid, 3.0, GHOST_TIME - 29.0);
        CHECK(target && ghost && dropped && fading && !t && h.contacts.empty(),
              "out of reach again it's a ghost where it was last seen, and can't be targeted; after %.0f s the ghost is gone too", GHOST_TIME);
        // Passive only: the radar off
        Controls radar; radar.toggleRadar = true;
        put(rid, 1.0);
        Run(h, 1.0 / SIM_HZ, radar);
        t = put(rid, 1.0);
        bool lost = !p.power.radar && t && t->ghost;
        t = put(rid, 0.5);
        CHECK(lost && t && !t->ghost && t->by == BY_IR, "radar off: the contact at 1 AU is lost; at 0.5 AU the infrared array has it");
        Run(h, 1.0 / SIM_HZ, radar);
        CHECK(p.power.radar, "...and on again");
    }

    // --- What others see of you: heat and the radar ---
    {
        quiet(); away();
        here = ShipLocal(h);
        rid = PutShip(h, NpcKind::Pirate, H, here + home.v * (1.2 * AU_U), clan).id;
        put(rid, 1.2, 0.5);
        const Npc *r = FindNpc(h, rid);
        double idle = h.flight.load, heatOn = p.power.heat;
        bool seenOn = r && NpcHolds(h, *r, h);
        p.power.radar = false;
        put(rid, 1.2, 0.5); r = FindNpc(h, rid);
        double heatOff = p.power.heat;
        bool seenOff = r && NpcHolds(h, *r, h);
        CHECK(fabs(idle - (p.spec.idleTW + p.spec.radarTW)) < 1e-6 && fabs(h.flight.load - p.spec.idleTW) < 1e-6 && heatOn > heatOff && seenOn && !seenOff,
              "the radar draws %.0f TW, and it all ends up as heat: %.1f TW with it on, %.1f off. A raider's infrared 1.2 AU away holds the first and not the second", p.spec.radarTW, heatOn, heatOff);
        put(rid, 0.8, 0.5); r = FindNpc(h, rid);
        bool near = r && NpcHolds(h, *r, h);
        p.power.mode = POWER_SILENT;
        p.power.driveCap = 0.0;
        put(rid, 0.8, 2.0); r = FindNpc(h, rid);
        bool silent = r && !NpcHolds(h, *r, h);
        CHECK(near && silent && p.power.heat < 0.3 * heatOff && h.flight.load <= SILENT_REACTOR * p.spec.reactorTW && p.power.driveCap == 0.0 && !RadarOn(p.spec, p.power),
              "at 0.8 AU it holds a ship idling with its radar off; running silent (%.2f TW of heat, nothing charging) it doesn't", p.power.heat);
        p.ship.throttle = 1.0f;
        put(rid, 0.8, 0.5); r = FindNpc(h, rid);
        double silentBurn = p.power.heat;
        p.power.mode = POWER_TRAVEL;
        p.ship.vel = { 0, 0, 0 };
        put(rid, 3.0, 0.5); r = FindNpc(h, rid);
        CHECK(r && NpcHolds(h, *r, h) && p.power.heat > 5000.0 && silentBurn > 1.5 * heatOff,
              "burning the engines flat out gives off %.0f TW: it's held from 3 AU (and running silent doesn't hide an exhaust: %.0f TW)", p.power.heat, silentBurn);
        p.ship.throttle = 0.0f; p.ship.vel = { 0, 0, 0 };
    }

    // --- The radar warning: a bearing on whoever is sweeping you ---
    {
        quiet(); away();
        here = ShipLocal(h);
        int pid = PutShip(h, NpcKind::Patrol, H, here + home.v * (4.0 * AU_U), home.ruler).id;
        const Traffic *t = put(pid, 4.0, 0.5);
        const SensorMark *m = nullptr;
        for (const SensorMark &o : h.marks) if (o.kind == MARK_RADAR && o.source == pid) m = &o;
        bool warned = !t && m && AngleBetween(m->dir, home.v) < 0.5;
        put(pid, 8.0, 1.5);
        bool quietAgain = true;
        for (const SensorMark &o : h.marks) if (o.kind == MARK_RADAR) quietAgain = false;
        t = put(pid, 1.2, 0.5);
        bool held = t && !t->ghost;
        for (const SensorMark &o : h.marks) if (o.kind == MARK_RADAR) held = false;
        CHECK(warned && quietAgain && held, "a patrol's radar 4 AU off: no contact, but a bearing on it; nothing at 8 AU; at 1.2 AU it's a contact, and the bearing isn't needed");
    }

    // --- Tachyon bursts on the scope: a bearing and a rough range, never a place ---
    {
        quiet(); away();
        here = ShipLocal(h);
        DVec3 where = here + Norm(home.v + home.u * 0.4) * (3.0 * AU_U);
        const SensorMark *m = nullptr;
        for (int tries = 0; tries < 4 && !m; tries++) {
            h.bursts.push_back({ H, where, small, 424242 });
            Run(h, 2.0 / SIM_HZ);
            for (const SensorMark &o : h.marks) if (o.kind == MARK_TACHYON) m = &o;
        }
        bool told = false;
        for (const GameEvent &e : h.events) if (e.text.find("Tachyon burst") != std::string::npos) told = true;
        CHECK(m && told && m->counts >= TACH_MIN_SHIP && AngleBetween(m->dir, where - here) < 5.0 * m->sigma && m->rLo < 3.0 * AU_U && m->rHi > 3.0 * AU_U &&
              m->rHi > 2.0 * m->rLo && Len(m->from - here) < 1.0,
              "a ship drops out of warp 3 AU away: %d counts, a bearing good to %.1f degrees, and a range of %.1f to %.1f AU", m ? m->counts : 0, m ? m->sigma : 0.0,
              m ? m->rLo / AU_U : 0.0, m ? m->rHi / AU_U : 0.0);
        size_t marks = h.marks.size();
        h.bursts.push_back({ H, here, small, h.id });
        Run(h, 2.0 / SIM_HZ);
        CHECK(h.marks.size() == marks, "your own arrival isn't marked on your own scope");

        // A raider lying in wait hears your arrival, and comes to where it reckons you are: near, not exact
        quiet();
        rid = PutShip(h, NpcKind::Pirate, H, here + home.v * (2.0 * AU_U), clan).id;
        Run(h, 0.05);
        h.bursts.push_back({ H, here, 3000.0, h.id });
        Run(h, 2.0 / SIM_HZ);
        const Npc *r = FindNpc(h, rid);
        double off = r ? Len(r->goal - here) : 0.0;
        CHECK(r && r->doing == NpcDo::Lurk && off > 0.02 * AU_U && off < 2.0 * AU_U && AngleBetween(r->goal - r->pos, here - r->pos) < 8.0,
              "a raider 2 AU off picks up the captain's own arrival and makes for where it reckons that was: %.2f AU out", off / AU_U);
    }

    // --- With no lock the drive goes where it's pointed, so space can be searched ---
    {
        quiet(); away();
        int star = 0;
        Aim aim; aim.course = TargetId::MakeBody(H, star);
        Controls go; go.hyper = true;
        Face(h, home.v);
        Run(h, 0.5, go, aim);
        bool locked = h.flight.lock.valid();
        p.ship.vel = { 0, 0, 0 };
        Face(h, home.v);
        DVec3 before = ShipLocal(h);
        Run(h, 2.0, go, Aim());                                  // (the lock released)
        DVec3 moved = ShipLocal(h) - before;
        CHECK(locked && !h.flight.lock.valid() && Len(moved) > 0.3 * AU_U && AngleBetween(moved, home.v) < 1.0,
              "with a destination locked, SHIFT flies to it; with the lock released, the drive goes where the nose points (%.1f AU along it in 2 s)", Len(moved) / AU_U);
        p.ship.vel = { 0, 0, 0 };
    }

    // --- A warp bubble run slower carries less, but sheds it slowly ---
    {
        quiet();
        p.ship.anchor = H; p.ship.pos = home.n * (0.7 * LY_U); p.ship.vel = { 0, 0, 0 }; p.ship.ftl = 1.0f;
        Face(h, home.n);
        p.power = PowerState(); p.fuel = p.spec.tank;
        Controls hyper; hyper.hyper = true;
        Run(h, 3.0, hyper);
        float full = p.power.field;
        bool warping = p.power.warping;
        p.ship.ftl = 0.3f;
        Run(h, 1.0, hyper);
        float soon = p.power.field;
        Run(h, 60.0, hyper);
        float slow = p.power.field;
        double mass = p.Mass();
        CHECK(warping && fabs(full - 1.0f) < 0.01f && soon > 0.85f && fabs(slow - 0.09f) < 0.03f && fabs(BubbleEnergy(p.spec, mass, 1.0) - p.spec.jumpTJ * mass) < 1e-6 &&
              BubbleEnergy(p.spec, mass, slow) < 0.3 * BubbleEnergy(p.spec, mass, 1.0) && TachyonDecayAU(BubbleEnergy(p.spec, mass, slow)) < 0.55 * TachyonDecayAU(BubbleEnergy(p.spec, mass, 1.0)),
              "at full speed the bubble holds %.0f TJ; set to 30%% it still holds %.0f%% of that a second later, and %.0f%% after a minute: a burst that carries %.0f AU, not %.0f",
              BubbleEnergy(p.spec, mass, 1.0), soon * 100.0, 100.0 * BubbleEnergy(p.spec, mass, slow) / BubbleEnergy(p.spec, mass, 1.0),
              TachyonDecayAU(BubbleEnergy(p.spec, mass, slow)), TachyonDecayAU(BubbleEnergy(p.spec, mass, 1.0)));
        p.ship.anchor = H; p.ship.pos = RandomPointFarFromPorts(h); p.ship.vel = { 0, 0, 0 }; p.ship.ftl = 1.0f;
        p.power = PowerState();
        UpdateLoadedSystem(h);
        Run(h, 0.5);
    }

    // --- A state's sensor network: outposts, a fix, a patrol sent to look; and traffic control's picture ---
    {
        quiet();
        std::vector<int> posts;
        int best = -1;
        for (int i = 0; i < (int)h.loaded.bodies.size(); i++) {
            const Body &b = h.loaded.bodies[i];
            if (b.settlement == Settlement::SensorOutpost) posts.push_back(i);
            else if (b.population > 0 && (best < 0 || b.population > h.loaded.bodies[best].population)) best = i;
        }
        bool spreadOut = posts.size() >= 3, theirs = true;
        for (int a : posts) {
            const Body &b = h.loaded.bodies[a];
            if (b.owner != home.ruler || b.population > 0 || h.econ.MarketOf(b.id) >= 0 || b.kind != Kind::Station || FieldRatio(home, b.local) >= 1.0) theirs = false;
            for (int o : posts) if (o != a && Len(b.local - h.loaded.bodies[o].local) < 0.15 * home.fieldR) spreadOut = false;
            if (Len(b.local) > 0.3 * home.fieldR || b.radius < 300.0) theirs = false;                 // (big, and well inside the system)
        }
        CHECK(spreadOut && theirs && best >= 0, "the home system has %d sensor outposts, the state's own, well inside it (%.0f AU out in a field of %.0f); nobody trades there",
              (int)posts.size(), posts.empty() ? 0.0 : Len(h.loaded.bodies[posts[0]].local) / AU_U, home.fieldR / AU_U);
        if (posts.size() < 3 || best < 0) return;
        {
            // Each has its pickets, and they keep to it
            h.trafficZoned = -1e9;
            Run(h, 0.1);
            int pickets = 0, strays = 0;
            auto count = [&]() {
                pickets = strays = 0;
                for (const Npc &n : h.npcs) {
                    if (n.kind != NpcKind::Patrol || n.post < 0) continue;
                    pickets++;
                    const Body &b = h.loaded.bodies[n.post];
                    if (b.settlement != Settlement::SensorOutpost || n.faction != home.ruler || Len(n.pos - b.local) > b.radius + 2000.0 * KM_U) strays++;
                }
            };
            count();
            int first = pickets;
            Run(h, 90.0);
            count();
            CHECK(first == OUTPOST_PICKETS * (int)posts.size() && pickets == first && strays == 0,
                  "%d warships are stationed at each outpost (%d in all), and a minute and a half later every one is still at its post", OUTPOST_PICKETS, pickets);
            quiet();
        }

        // Under traffic control at the biggest port, with a patrol standing by
        Park(h, best, 2.0);
        p.power = PowerState();
        h.scanSafeUntil = h.clock + 1e6;                          // (no patrol takes an interest in the captain meanwhile)
        Run(h, 0.5);
        here = ShipLocal(h);
        std::string control = h.trafficControl;
        int pid = PutShip(h, NpcKind::Patrol, H, here + home.v * (0.05 * AU_U), home.ruler).id;
        const_cast<Npc *>(FindNpc(h, pid))->until = h.clock + 1e6;
        Run(h, 0.1);
        // An arrival nobody was expecting, between two of the outposts
        const Body &pa = h.loaded.bodies[posts[0]], &pb = h.loaded.bodies[posts[1]];
        DVec3 where = (pa.local + pb.local) * 0.5 + home.n * (2.0 * AU_U);
        h.bursts.push_back({ H, where, 20.0 * small, 424242 });
        Run(h, 2.0 / SIM_HZ);
        bool pending = h.alerts.size() == 1 && h.alerts[0].fixed;
        bool early = true;
        for (const SensorMark &o : h.marks) if (o.kind == MARK_FIX) early = false;
        Run(h, NET_RELAY * posts.size() + 1.0);
        const SensorMark *m = nullptr;
        for (const SensorMark &o : h.marks) if (o.kind == MARK_FIX) m = &o;
        const Npc *patrol = nullptr;                               // (whichever patrol was nearest)
        for (const Npc &n : h.npcs) if (n.kind == NpcKind::Patrol && Len(n.goal - where) < 1.0 * AU_U) patrol = &n;
        (void)pid;
        CHECK(!control.empty() && pending && early && m && Len(m->from - where) < fmax(3.0 * m->rLo, 0.05 * AU_U) && m->rLo < 1.0 * AU_U && m->note == control &&
              h.alerts.empty() && patrol,
              "an unscheduled arrival %.0f AU out: the outposts' fix reaches %s %.0f s later, %.3f AU off (+/- %.3f claimed), and a patrol is sent to look",
              Len(where) / AU_U, control.c_str(), NET_RELAY * posts.size(), m ? Len(m->from - where) / AU_U : 0.0, m ? m->rLo / AU_U : 0.0);

        // Far beyond the edge: nothing
        quiet();
        h.alerts.clear();
        h.bursts.push_back({ H, Norm(pa.local) * (home.fieldR + 400.0 * AU_U), small, 424242 });
        Run(h, 2.0 / SIM_HZ);
        bool unmarked = true;
        for (const SensorMark &o : h.marks) if (o.kind != MARK_RADAR) unmarked = false;
        CHECK(h.alerts.empty() && unmarked, "a small ship dropping out 400 AU beyond the edge of the field is caught by nobody");

        // Scheduled traffic arrives at the jump points: caught just the same, and nobody turns out
        quiet();
        int hub = -1, other = -1;
        for (int i = 0; i < h.econ.Count(); i++) if (h.econ.base->defs[i].sys == H) { hub = h.econ.base->defs[i].hub; break; }
        for (const Lane &l : h.econ.base->lanes) if (other < 0 && (l.a == hub || l.b == hub)) other = l.a == hub ? l.b : l.a;
        pid = PutShip(h, NpcKind::Patrol, H, here + home.v * (0.05 * AU_U), home.ruler).id;
        const_cast<Npc *>(FindNpc(h, pid))->until = h.clock + 1e6;
        if (hub >= 0 && other >= 0) {
            Transit t;
            t.from = other; t.to = hub; t.total = 400.0f; t.cargo[REF_FOOD] = 400.0f; t.arrive = h.clock; t.name = "Test liner";
            h.transits.push_back(t);
            Run(h, 2.0 / SIM_HZ);
            bool caught = h.alerts.size() == 1 && h.alerts[0].fixed;
            int arrived = 0;
            DVec3 came;
            for (const Npc &n : h.npcs) if (n.name == "Test liner") { arrived++; came = n.pos; }
            Run(h, NET_RELAY * posts.size() + 1.0);
            bool fix = false;
            for (const SensorMark &o : h.marks) if (o.kind == MARK_FIX) fix = true;
            bool sent = false;                                     // (has any patrol been sent there?)
            for (const Npc &n : h.npcs) if (n.kind == NpcKind::Patrol && n.post < 0 && n.doing == NpcDo::Travel && Len(n.goal - came) < 0.35 * AU_U) sent = true;
            (void)pid;
            CHECK(arrived == 1 && caught && !fix && !sent,
                  "a freighter jumping in on a trade lane is fixed too, where scheduled traffic is expected: no alarm, and no patrol is sent");
        } else CHECK(false, "the home system has a trade lane");

        // Traffic control's picture: a ship the port holds is on your scope while you're in its reach
        quiet();
        rid = PutShip(h, NpcKind::Pirate, H, here + home.v * (2.0 * AU_U), clan).id;
        Run(h, 1.2);
        const Traffic *t = shown(rid);
        bool shared = t && !t->ghost && t->by == BY_CONTROL && t->level >= CONTACT_DETECTED && h.trafficControl == control;
        p.ship.pos = p.ship.pos - home.v * (1.0 * AU_U);
        Run(h, 1.2);
        t = shown(rid);
        CHECK(shared && h.trafficControl.empty() && t && t->ghost,
              "docked at %s, a raider 2 AU out is on the scope by traffic control alone; an AU away from the port, out of its reach, the contact is lost", control.c_str());
    }

    // --- Over the wire ---
    {
        Snapshot s, back;
        Traffic a, b;
        a.id = NPC_ID_BASE + 7; a.npc = true; a.ship.anchor = H; a.ship.pos = { 1.0, 2.0, 3.0 }; a.level = CONTACT_CLASSIFIED; a.by = BY_RADAR | BY_IR; a.stance = STANCE_ALERTED;
        b.id = 3; b.ship.anchor = H; b.level = CONTACT_IDENTIFIED; b.ghost = true; b.age = 41.0f; b.by = 0;
        s.others = { a, b };
        Writer w;
        WriteSnapshot(w, s);
        Reader r(w.buf);
        r.U8();
        bool ok = ReadSnapshot(r, back) && back.others.size() == 2;
        CHECK(ok && back.others[0].level == CONTACT_CLASSIFIED && back.others[0].by == (BY_RADAR | BY_IR) && !back.others[0].ghost && back.others[0].stance == STANCE_ALERTED &&
              back.others[1].level == CONTACT_IDENTIFIED && back.others[1].ghost && back.others[1].age == 41.0f,
              "a snapshot carries how each ship stands on the scope: what's known of it, by which sensors, and how long a ghost has been one");
        Captain from, to;
        SensorMark m;
        m.kind = MARK_TACHYON; m.anchor = H; m.from = { 5.0, 6.0, 7.0 }; m.dir = Norm(DVec3{ 1, 2, 3 }); m.sigma = 2.5f; m.rLo = 1.0 * AU_U; m.rHi = 4.0 * AU_U;
        m.counts = 9; m.at = 990.0; m.note = "Somewhere Control";
        from.marks.push_back(m);
        m.kind = MARK_RADAR; m.source = NPC_ID_BASE + 3; from.marks.push_back(m);
        from.trafficControl = "Somewhere";
        std::vector<uint8_t> msg = MsgMarks(1000.0, from);
        Reader mr(msg);
        bool type = mr.U8() == MSG_MARKS;
        bool read = ReadMarks(mr, 2000.0, to);
        CHECK(type && read && to.trafficControl == "Somewhere" && to.marks.size() == 2 && to.marks[0].counts == 9 && Len(to.marks[0].dir - from.marks[0].dir) < 1e-6 &&
              to.marks[0].rHi == 4.0 * AU_U && fabs(to.marks[0].at - 1990.0) < 1e-3 && to.marks[0].note == "Somewhere Control" && to.marks[1].kind == MARK_RADAR &&
              to.marks[1].source == NPC_ID_BASE + 3,
              "the bearings on a scope reach the client, with their ages");
    }
}

// ---------------------------------------------------------------------------
// Industry: what a world can do, the labour and power it takes, and building more of it
// ---------------------------------------------------------------------------
static void TestIndustry()
{
    // --- The place decides what can be done there ---
    {
        SiteInfo garden, moon, station, hot;
        garden.openAir = garden.surface = true; garden.flux = 1.0f; garden.tempK = 288.0f; garden.gravity = 1.0f; garden.life = 3; garden.res[RES_BIOMASS] = 0.8f;
        moon.surface = true; moon.giant = true; moon.flux = 0.03f; moon.tempK = 60.0f; moon.gravity = 0.15f; moon.res[RES_ICES] = 0.9f; moon.res[RES_METALS] = 0.6f;
        station.giant = true; station.flux = 0.03f;
        hot.surface = true; hot.flux = 6.0f; hot.tempK = 700.0f; hot.gravity = 0.9f; hot.res[RES_METALS] = 0.8f; hot.strangeStar = true;
        CHECK(SiteQuality(IND_FARM, garden) > 1.0 && SiteQuality(IND_FARM, moon) == 0.0 && SiteQuality(IND_AGRIDOME, moon) > 0.9 && SiteQuality(IND_AGRIDOME, garden) == 0.0 &&
              SiteQuality(IND_HYDROPONICS, station) == 1.0 && SiteQuality(IND_HYDROPONICS, moon) == 0.0,
              "open farming wants a sky to breathe under; a far moon gets agridomes; a station, hydroponics");
        CHECK(SiteQuality(IND_MINE, garden) == 0.0 && SiteQuality(IND_MINE, moon) > 0.8 && SiteQuality(IND_MINE, hot) > SiteQuality(IND_MINE, moon) &&
              SiteQuality(IND_ICEWORKS, moon) > 1.0 && SiteQuality(IND_ICEWORKS, hot) == 0.0 && SiteQuality(IND_SKIMMER, station) == 1.0 && SiteQuality(IND_SKIMMER, garden) == 0.0,
              "mines are as good as the rock, ice works need ice, a skimmer needs a giant below");
        CHECK(SiteQuality(IND_LAB, hot) > 2.0 * SiteQuality(IND_LAB, garden) && SiteQuality(IND_LAB, moon) > SiteQuality(IND_LAB, garden) &&
              SiteQuality(IND_XENO, garden) > 1.0 && SiteQuality(IND_XENO, moon) == 0.0 && SiteQuality(IND_SOLAR, hot) > 2.0 && SiteQuality(IND_SOLAR, moon) == 0.0 &&
              SiteQuality(IND_GEOTHERMAL, hot) > 1.0 && SiteQuality(IND_GEOTHERMAL, garden) == 0.0 && SiteQuality(IND_CITY, garden) > SiteQuality(IND_CITY, moon),
              "research labs do best where it's strangest (%.1f on a scorched world of a dying star, %.1f on a garden world); xenobiology needs life; power is by the place; cities are cheapest in the open",
              SiteQuality(IND_LAB, hot), SiteQuality(IND_LAB, garden));
        Product mix[MAX_PRODUCTS];
        int n = IndustryProducts(IND_MINE, hot, mix);
        double sum = 0.0, iron = 0.0;
        for (int i = 0; i < n; i++) { sum += mix[i].share; if (mix[i].good == ORE_IRON) iron = mix[i].share; }
        CHECK(n >= 4 && fabs(sum - 1.0) < 1e-5 && iron > 0.4 && fabs(IndustryLabour(IND_MINE, 20.0, 1.0) / IndustryLabour(IND_MINE, 10.0, 1.0) - pow(2.0, LABOUR_POWER)) < 1e-9 &&
              IndustryLabour(IND_MINE, 10.0, 0.5) > 4.0 * IndustryLabour(IND_MINE, 10.0, 1.0) && fabs(CityHousing(CityFor(1e6)) - 1e6) < 1.0,
              "a metal-rich world's mines are %.0f%% iron; twice the capacity takes %.1f times the labour, and a site half as good more than four times",
              100.0 * iron, pow(2.0, LABOUR_POWER));
    }

    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const int N = g.econ.Count();
    auto find = [&](int m, int type) { for (int k = 0; k < (int)base.defs[m].ind.size(); k++) if (base.defs[m].ind[k].type == type && g.econ.state[m].ind[k].cap > 0.0f) return k; return -1; };
    int home = -1, domed = -1, mining = -1;
    for (int i = 0; i < N; i++) {
        const MarketDef &d = base.defs[i];
        if (d.sys == g.systems.home && d.settlement == Settlement::CoreWorld) home = i;
        if (domed < 0 && d.settlement == Settlement::DomedColonies && d.pop > 5000.0) domed = i;
        if (mining < 0 && d.settlement == Settlement::MiningOutpost) mining = i;
    }
    CHECK(home >= 0 && domed >= 0 && mining >= 0, "found a core world, a domed colony and a mining outpost");
    if (home < 0 || domed < 0 || mining < 0) return;

    // --- One economy to a world, stations and all; industries by what the place is ---
    {
        const MarketDef &d = base.defs[home];
        int stations = 0, merged = 0;
        for (const Body &b : g.loaded.bodies)
            if (b.kind == Kind::Station && b.population > 0 && b.parent > 0 && g.loaded.bodies[b.parent].id == d.bodyId) { stations++; if (g.econ.MarketOf(b.id) == home) merged++; }
        double bodies = 0.0;
        for (const Body &b : g.loaded.bodies) if (g.econ.MarketOf(b.id) == home) bodies += b.population;
        CHECK(stations > 0 && merged == stations && fabs(bodies - d.pop) < 1.0 && fabs(g.econ.state[home].pop - d.pop) < 1.0,
              "%s and the %d stations over it are one economy of %s people", d.name.c_str(), stations, FormatPopulation(d.pop).c_str());
        CHECK(find(home, IND_FARM) >= 0 && find(home, IND_AGRIDOME) < 0 && find(home, IND_CITY) >= 0 && find(home, IND_FOUNDRY) >= 0 && find(home, IND_PHARMA) >= 0 &&
              find(home, IND_ARMS) >= 0 && find(home, IND_LAB) >= 0 && find(domed, IND_AGRIDOME) >= 0 && find(domed, IND_FARM) < 0 && find(mining, IND_MINE) >= 0,
              "the core world farms in the open and has its foundries, labs and arms plants; the domed colony farms under domes; the outpost mines");
        int powered = 0, housed = 0, sized = 0, employed = 0;
        for (int i = 0; i < N; i++) {
            const MarketState &s = g.econ.state[i];
            bool power = false;
            double need = 0.0;
            for (int k = 0; k < (int)s.ind.size(); k++) {
                if (s.ind[k].cap <= 0.0f) continue;
                if (IsPower(base.defs[i].ind[k].type)) power = true;
                need += IndustryLabour(base.defs[i].ind[k].type, s.ind[k].cap, base.defs[i].ind[k].site);
                if (base.defs[i].ind[k].type == IND_CITY && fabs(CityHousing(s.ind[k].cap) / s.pop - HOUSING_SPARE) < 0.01) housed++;
            }
            if (power) powered++;
            // (stations, whose power takes more hands, come out a little over)
            if (need / (LABOUR_SHARE * s.pop) > EMPLOYED_AT_START - 0.06 && need / (LABOUR_SHARE * s.pop) < 1.0) sized++;
            if (s.employed <= s.labour * 1.0001f) employed++;
        }
        CHECK(powered == N && housed == N && sized > 0.97 * N && employed == N,
              "all %d economies make power and house their people (%.0f%% of them over); %d are sized to use about %.0f%% of their labour, services and all", N, (HOUSING_SPARE - 1.0) * 100.0, sized,
              EMPLOYED_AT_START * 100.0);
    }

    // --- Labour: 0.9 a head; short of it, the least important goes short first ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[home];
        StepEconomy(g, 0.02);
        double labour = s.labour, pop = s.pop;
        int farm = find(home, IND_FARM), light = find(home, IND_LIGHT), city = find(home, IND_CITY);
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];     // (everything wanted)
        StepEconomy(g, 0.02);
        float farmFull = s.ind[farm].run, lightFull = s.ind[light].run;
        s.pop *= 0.5;
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];
        StepEconomy(g, 0.02);
        int shortHanded = 0;
        for (const IndustryState &st : s.ind) if (st.shortOf & SHORT_LABOUR) shortHanded++;
        int services = find(home, IND_SERVICES);
        CHECK(fabs(labour - LABOUR_SHARE * pop) < 1e-3 * labour && fabs(s.labour - 0.5 * labour) < 1e-3 * labour && s.employed > 0.99f * s.labour && shortHanded > 2 &&
              s.housing > 0.99 * s.pop && farmFull > 0.8f && s.ind[farm].run > 0.9f * farmFull && lightFull > 0.0f && services >= 0 && s.ind[services].run < 0.05f,
              "labour is %.1f a head. With half its people, %s still houses them all (its cities at %.0f%% of what they could), and its farms run (%.0f%%); %d industries are short-handed (light industry at %.0f%%), and there's nobody left for services",
              LABOUR_SHARE, base.defs[home].name.c_str(), s.ind[city].run * 100.0f, s.ind[farm].run * 100.0f, shortHanded, s.ind[light].run * 100.0f);
        g.econ = was;
    }

    // --- Power: plasma cells, made here or brought in; without them the works stop and the cities go dark ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[home];
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];
        StepEconomy(g, 0.02);
        float made = s.cellsMade, used = s.cellsUsed, housing = s.housing;
        int foundry = find(home, IND_FOUNDRY);
        float before = s.ind[foundry].output;
        for (int k = 0; k < (int)s.ind.size(); k++) if (IsPower(base.defs[home].ind[k].type)) s.ind[k].cap = 0.0f;       // the plants shut
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];
        s.stock[REF_CELLS] = 0.0f;
        StepEconomy(g, 0.02);
        bool dark = s.cellsMade == 0.0f && s.housing < 0.05f * housing && s.ind[foundry].output < 0.05f * before && (s.ind[foundry].shortOf & SHORT_CELLS);
        s.stock[REF_CELLS] = (float)(used * 0.02 * 3.0);                        // a shipload comes in
        for (int k = 0; k < GOOD_COUNT; k++) if (k != REF_CELLS) s.stock[k] = 0.9f * s.target[k];
        StepEconomy(g, 0.02);
        CHECK(made > 0.9f * used && used > 0.0f && dark && s.housing > 0.95f * housing && s.ind[foundry].output > 0.9f * before,
              "%s makes %.0f plasma cells a day and uses %.0f. With its plants shut and none in store the foundry stops and the cities go dark; a shipload of cells brings both back",
              base.defs[home].name.c_str(), made, used);
        g.econ = was;
    }

    // --- Materials: an industry short of what it's made from (or of scientific data) makes less ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[home];
        int pharma = find(home, IND_PHARMA);
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];
        StepEconomy(g, 0.02);
        float before = s.ind[pharma].output;
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 0.9f * s.target[k];
        float data = s.stock[GOOD_DATA];
        s.stock[GOOD_DATA] = 0.0f;
        for (int k = 0; k < (int)s.ind.size(); k++) if (base.defs[home].ind[k].type == IND_LAB) s.ind[k].cap = 0.0f;
        StepEconomy(g, 0.02);
        CHECK(before > 0.0f && data > 0.0f && s.ind[pharma].output < 0.05f * before && (s.ind[pharma].shortOf & SHORT_MATERIALS),
              "pharmaceuticals run on scientific data: with the labs shut and none in store, medicine output falls from %.0f t a day to %.1f", before, s.ind[pharma].output);
        g.econ = was;
    }

    // --- Building more: the owner, when it pays; and a captain, for a share ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[home];
        int heavy = find(home, IND_LIGHT);
        for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = 1.1f * s.target[k];     // (nothing else worth building)
        for (int p = 0; p < base.defs[home].ind[heavy].outs; p++) s.stock[base.defs[home].ind[heavy].out[p].good] = 0.2f * s.target[base.defs[home].ind[heavy].out[p].good];
        s.stock[REF_STEEL] = 2.0f * s.target[REF_STEEL]; s.stock[GOOD_MACHINERY] = 2.0f * s.target[GOOD_MACHINERY];
        float cap = s.ind[heavy].cap, steel = s.stock[REF_STEEL];
        g.clock += MARKET_DAY;                                    // (time for the owners to look again)
        StepEconomy(g, 0.01);
        bool started = s.ind[heavy].building > 0.0f && fabs(s.ind[heavy].building - UPGRADE_STEP * cap) < 1e-3 * cap && s.stock[REF_STEEL] < steel - 0.3f * s.ind[heavy].building;
        float adding = s.ind[heavy].building;
        for (int i = 0; i < 12; i++) StepEconomy(g, 0.2);
        CHECK(started && s.ind[heavy].building == 0.0f && fabs(s.ind[heavy].cap - cap - adding) < 1e-3 * cap,
              "consumer goods are short at %s: its owner adds %.0f%% to its light industry (%.0f capacity, for steel and machinery), and %.0f days later it's running", base.defs[home].name.c_str(),
              UPGRADE_STEP * 100.0, adding, UPGRADE_DAYS);
        g.econ = was;

        // The captain's stake
        int world = -1;
        for (int i = 0; i < (int)g.loaded.bodies.size(); i++) if (g.loaded.bodies[i].id == base.defs[home].bodyId) world = i;
        Park(g, world, 0.9);
        g.player.credits = 5e6;
        Run(g, 1.2);
        MarketState &h = g.econ.state[home];
        int foundry = find(home, IND_FOUNDRY);
        h.stock[REF_STEEL] = 2.0f * h.target[REF_STEEL]; h.stock[GOOD_MACHINERY] = 2.0f * h.target[GOOD_MACHINERY];
        for (IndustryState &st : h.ind) { st.building = 0.0f; }
        h.employed = 0.5f * h.labour;
        float had = h.ind[foundry].cap;
        double credits = g.player.credits;
        bool funded = ApplyCommand(g, g, { Command::Invest, foundry });
        double paid = credits - g.player.credits;
        const WorldRecord &rec = g.know.worlds[base.defs[home].bodyId];
        bool onRecord = (int)rec.rows.size() > foundry && rec.rows[foundry].stake > 0.0f && rec.rows[foundry].building > 0.0f;
        bool again = ApplyCommand(g, g, { Command::Invest, foundry });        // (one thing at a time)
        CHECK(funded && paid > INVEST_FEE * UPGRADE_STEP * had && g.stakes.size() == 1 && fabs(g.stakes[0].cap - UPGRADE_STEP * had) < 1e-3 * had && onRecord && !again,
              "docked at %s, the captain funds an addition to its semiconductor foundry for %s cr: %.0f capacity in the building, and theirs", base.defs[home].name.c_str(),
              Thousands(paid).c_str(), g.stakes.empty() ? 0.0 : g.stakes[0].cap);
        // It earns while they're away, and pays when they're back
        Park(g, world, 40.0);
        g.player.ship.pos = g.player.ship.pos + g.systems[g.systems.home].n * (2.0 * AU_U);
        credits = g.player.credits;
        for (int i = 0; i < 30; i++) { h.stock[GOOD_ELECTRONICS] = 0.5f * h.target[GOOD_ELECTRONICS]; StepEconomy(g, 0.2); }    // (six days of a good price for electronics)
        Run(g, 0.5);
        bool waiting = g.player.credits == credits && h.ind[foundry].cap > had * 1.09f;
        Park(g, world, 0.9);
        Run(g, 1.5);
        double earned = g.player.credits - credits;
        CHECK(waiting && earned > 100.0 && earned < paid, "six days on, the foundry is %.0f%% bigger; back in dock, the stake pays out %s cr (on %s put in)",
              100.0 * (h.ind[foundry].cap / had - 1.0), Thousands(earned).c_str(), Thousands(paid).c_str());

        // Saved, and over the wire
        std::string path = "test_industry.txt";
        float capNow = h.ind[foundry].cap;
        double earnedMark = h.ind[foundry].earned;
        bool saved = SaveGame(g, path.c_str());
        GameState l; NewGame(l);
        bool loaded = LoadGame(l, path.c_str());
        remove(path.c_str());
        CHECK(saved && loaded && l.econ.state[home].ind[foundry].cap == capNow && fabs(l.econ.state[home].ind[foundry].earned - earnedMark) < 1e-6 * fabs(earnedMark) + 1e-9 &&
              l.stakes.size() == 1 && l.stakes[0].cap == g.stakes[0].cap && l.stakes[0].body == g.stakes[0].body && l.econ.state[home].pop == h.pop &&
              fabs(l.econ.state[home].target[GOOD_ELECTRONICS] - h.target[GOOD_ELECTRONICS]) < 1e-3f * h.target[GOOD_ELECTRONICS],
              "industries, what they've earned and the captain's stake are saved and come back");
        std::vector<uint8_t> msg = MsgWorld(base.defs[home].bodyId, g.know.worlds[base.defs[home].bodyId]);
        Reader r(msg);
        uint64_t body = 0; WorldRecord back;
        bool type = r.U8() == MSG_WORLD, read = ReadWorld(r, body, back);
        const WorldRecord &sent = g.know.worlds[base.defs[home].bodyId];
        CHECK(type && read && body == base.defs[home].bodyId && back.rows.size() == sent.rows.size() && back.pop == sent.pop && back.rows[foundry].cap == sent.rows[foundry].cap &&
              back.rows[foundry].stake == sent.rows[foundry].stake && back.rows[foundry].type == IND_FOUNDRY && back.name == sent.name,
              "a world's economy page reaches a client (%d industries, %d bytes)", (int)back.rows.size(), (int)msg.size());
    }
}

// ---------------------------------------------------------------------------
// Parts from a file: the built-in set, a changed one, a broken one
// ---------------------------------------------------------------------------
static void TestParts()
{
    const std::string builtIn = DefaultPartsText();
    auto save = [](const std::string &text) { FILE *f = fopen("test_parts.txt", "wb"); if (f) { fputs(text.c_str(), f); fclose(f); } };
    auto swap = [](std::string text, const std::string &from, const std::string &to) { size_t at = text.find(from); if (at != std::string::npos) text.replace(at, from.size(), to); return text; };

    PartsData d;
    std::string problem;
    bool ok = ParseParts(builtIn, d, problem);
    int models = 0;
    for (int t = 0; t < (int)PartType::Count; t++) models += (int)d.models[t].size();
    const uint64_t usual = PartsHash();
    ShipSpec before = SpecOf(StarterBuild());
    CHECK(ok && d.hash == usual && models == (int)PartType::Count + 24 && ModelCount(PartType::Reactor) == 15 && ModelCount(PartType::WarpDrive) == 6 && FuelGoodOf(BURNS_HELIUM3) == RAW_HELIUM3 &&
          std::string(Hull(HULL_BULK).name) == "Bulk carrier" && before.hold == 100.0 && before.jumpTJ == 800.0,
          "the parts the game is built with read as a parts file: %d models of %d kinds of part, six hulls %s", models, (int)PartType::Count, problem.c_str());

    // Comments and spacing aren't the numbers
    PartsData spaced;
    bool same = ParseParts("# a note at the top\r\n\r\n" + swap(builtIn, "capacity = 25\n", "capacity   =   25     # tonnes\n"), spaced, problem);
    CHECK(same && spaced.hash == usual, "a file that differs only in comments and spacing is the same parts (%s)", problem.c_str());

    // A changed number and a new model
    std::string changed = swap(builtIn, "capacity = 25\n", "capacity = 40\n") +
        "\n[warpdrive easy]\nname = Easy-jump warp drive\nmass = 8\nvolume = 20\ncost = 9000\nmaterial = Ship Components\n"
        "speed = 0.6\nspeed_grade = 0.1\nfield_base = 9\nfield_mass = 9e-15\nfield_power = 2\naccel = 2e6\nmax_mass = 225\njump = 10\njump_grade = 0.05\n";
    save(changed);
    bool loaded = LoadPartsFile("test_parts.txt", problem);
    ShipBuild b = StarterBuild();
    ShipSpec bigger = SpecOf(b);
    Part easy;
    easy.type = PartType::WarpDrive; easy.size = 3; easy.variant = 6;
    std::string fits = FitProblem(b, easy), name = Describe(easy).name;
    for (Part &p : b.parts) if (p.type == PartType::WarpDrive) p.variant = 6;
    ShipSpec jumps = SpecOf(b);
    CHECK(loaded && PartsHash() != usual && bigger.hold == 160.0 && ModelCount(PartType::WarpDrive) == 7 && jumps.jumpTJ == 10.0 && jumps.warpSpeed < 0.7 * before.warpSpeed &&
          jumps.warpAccel == 2e6 && name.find("easy-jump warp drive") != std::string::npos && (fits.empty() || fits.find("No room") == 0),
          "a file with bigger cargo bays and a second warp drive: the starter ship holds %.0f t, and with the \"%s\" jumps for %.0f TJ/t at %.0f%% of the speed %s",
          bigger.hold, name.c_str(), jumps.jumpTJ, 100.0 * jumps.warpSpeed / before.warpSpeed, problem.c_str());

    // A file that won't do changes nothing, and says where it's wrong
    const uint64_t held = PartsHash();
    save(swap(changed, "thrust = 1312.5", "thrust = lots"));
    bool bad1 = LoadPartsFile("test_parts.txt", problem);
    std::string said1 = problem;
    save(swap(builtIn, "[hull bulk]", "[hull barge]"));
    bool bad2 = LoadPartsFile("test_parts.txt", problem);
    std::string said2 = problem;
    save(swap(builtIn, "fuel = Deuterium", "fuel = Steel"));
    bool bad3 = LoadPartsFile("test_parts.txt", problem);
    std::string said3 = problem;
    bool missing = LoadPartsFile("no_such_parts.txt", problem);
    CHECK(!bad1 && said1.find("line ") != std::string::npos && said1.find("lots") != std::string::npos && !bad2 && said2.find("barge") != std::string::npos &&
          !bad3 && said3.find("fuel") != std::string::npos && !missing && problem.empty() && PartsHash() == held && SpecOf(StarterBuild()).hold == 160.0,
          "broken files are refused and nothing changes: \"%s\"; \"%s\"; \"%s\"; and no file at all is no complaint", said1.c_str(), said2.c_str(), said3.c_str());

    UseDefaultParts();
    remove("test_parts.txt");
    CHECK(PartsHash() == usual && SpecOf(StarterBuild()).hold == 100.0, "back to the built-in parts");
}

// ---------------------------------------------------------------------------
// Reactors: heat from the burn rate, power from the efficiency there; the floor; shutting down
// ---------------------------------------------------------------------------
static void TestReactors()
{
    auto model = [](const char *id) { const auto &all = Parts().models[(int)PartType::Reactor]; for (int k = 0; k < (int)all.size(); k++) if (all[k].id == id) return k; return -1; };
    auto ship = [](int variant, int size) { ShipBuild b; b.parts = { MakePart(PartType::Reactor, size, 0, variant) }; return SpecOf(b); };
    const ShipSpec s = SpecOf(StarterBuild());

    // --- Power is burn rate x the fuel's energy x the efficiency at that burn rate ---
    double half = ReactorOutput(s, 0.5), loadFor = ReactorLoadFor(s, half);
    int best = 0;
    for (int k = 1; k <= 100; k++) if (ReactorEfficiency(s, k / 100.0) > ReactorEfficiency(s, best / 100.0)) best = k;
    bool rising = true;
    for (int k = 1; k <= 100; k++) if (ReactorOutput(s, k / 100.0) <= ReactorOutput(s, (k - 1) / 100.0)) rising = false;
    CHECK(fabs(ReactorOutput(s, 1.0) - s.reactorTW) < 1e-6 && fabs(half - 0.5 * s.reactorBurn * s.fuelTJ * ReactorEfficiency(s, 0.5)) < 1e-6 && fabs(loadFor - 0.5) < 1e-6 && rising &&
          ReactorEfficiency(s, 0.05) < 0.5 * ReactorEfficiency(s, 0.8) && best > 60 && best < 100 && ReactorEfficiency(s, best / 100.0) > s.reactorEff &&
          ReactorLoadFor(s, 2.0 * s.reactorTW) == 1.0 && ReactorLoadFor(s, 0.0) == 0.0,
          "the starter's fission reactor: %.0f TW flat out at %.0f%%; it does best at %d%% of that burn (%.0f%%), and at a twentieth gets %.0f%%; more fuel is always more power",
          s.reactorTW, s.reactorEff * 100.0, best, ReactorEfficiency(s, best / 100.0) * 100.0, ReactorEfficiency(s, 0.05) * 100.0);

    // --- It can't be turned down below its floor: a big fusion reactor is a wasteful thing to idle ---
    {
        ShipSpec fis = ship(BURNS_REACTOR_FUEL, 4), deu = ship(BURNS_DEUTERIUM, 4);
        double idle = 100.0;                                        // terawatts of housekeeping
        double burnF = ReactorBurnFor(fis, idle), burnD = ReactorBurnFor(deu, idle), heatF = burnF * fis.fuelTJ - idle, heatD = burnD * deu.fuelTJ - idle;
        CHECK(fabs(ReactorBurnFor(deu, 0.0) - deu.reactorFloor * deu.reactorBurn) < 1e-12 && fabs(burnD - deu.reactorFloor * deu.reactorBurn) < 1e-12 && deu.reactorFloor > 3.0 * fis.reactorFloor &&
              ReactorOutput(deu, deu.reactorFloor) > 2.0 * idle && heatD > 2.0 * heatF && ReactorBurnFor(fis, 0.5 * fis.reactorTW) > 5.0 * burnF,
              "feeding %.0f TW of housekeeping, a size 4 fission reactor turns down to %.4f t/s and gives off %.0f TW of heat; a deuterium one can't go below %.0f%% of its burn: "
              "%.4f t/s, %.0f TW made that nothing wants, %.0f TW of heat", idle, burnF, heatF, deu.reactorFloor * 100.0, burnD, ReactorOutput(deu, deu.reactorFloor) - idle, heatD);
    }

    // --- The models: what each is for ---
    {
        int baseload = model("fission_baseload"), high = model("fission_high"), flat = model("fission_flat"), compact = model("fission_compact");
        CHECK(baseload == 3 && high == 4 && flat == 5 && compact == 6 && model("helium3_compact") == 14, "five fission reactors, five deuterium, five helium-3: the everyday three first");
        if (baseload < 0 || high < 0 || flat < 0 || compact < 0) return;
        ShipSpec usual = ship(BURNS_REACTOR_FUEL, 3), base = ship(baseload, 3), hot = ship(high, 3), even = ship(flat, 3), small = ship(compact, 3);
        auto perTJ = [](const ShipSpec &r, double load) { return 1e6 / (r.fuelTJ * ReactorEfficiency(r, load)); };        // grams of fuel a terajoule
        PartInfo pu = Describe(MakePart(PartType::Reactor, 3)), pc = Describe(MakePart(PartType::Reactor, 3, 0, compact)), pb = Describe(MakePart(PartType::Reactor, 3, 0, baseload));
        CHECK(base.reactorTW < 0.7 * usual.reactorTW && hot.reactorTW > 1.5 * usual.reactorTW && perTJ(base, 1.0) < 0.9 * perTJ(usual, 1.0) && perTJ(hot, 1.0) > perTJ(usual, 1.0) &&
              perTJ(hot, 0.3) > 1.5 * perTJ(usual, 0.3) && perTJ(even, 0.1) < 0.8 * perTJ(usual, 0.1) && fabs(perTJ(even, 0.2) - perTJ(even, 1.0)) < 1e-9 &&
              even.reactorFloor < usual.reactorFloor && base.reactorFloor > 2.0 * usual.reactorFloor && base.reactorRestart > usual.reactorRestart &&
              pc.mass < 0.6 * pu.mass && pc.volume < 0.6 * pu.volume && perTJ(small, 1.0) > 1.2 * perTJ(usual, 1.0) && pb.mass > pu.mass && pb.cost > pu.cost,
              "size 3 fission reactors, flat out: everyday %.0f TW at %.1f g of fuel a terajoule; baseload %.0f TW at %.1f; high-output %.0f TW at %.1f (and %.1f at a third of its burn); "
              "load-following %.1f at any load; compact %.1f, in %.0f m3 against %.0f",
              usual.reactorTW, perTJ(usual, 1.0), base.reactorTW, perTJ(base, 1.0), hot.reactorTW, perTJ(hot, 1.0), perTJ(hot, 0.3), perTJ(even, 1.0), perTJ(small, 1.0), pc.volume, pu.volume);
        // Two aboard run together: their burn adds, and the curve is theirs by how much each burns
        ShipBuild two; two.parts = { MakePart(PartType::Reactor, 3), MakePart(PartType::Reactor, 3, 0, baseload) };
        ShipSpec both = SpecOf(two);
        CHECK(fabs(both.reactorBurn - usual.reactorBurn - base.reactorBurn) < 1e-12 && fabs(both.reactorTW - usual.reactorTW - base.reactorTW) < 1e-6 &&
              both.reactorEff > usual.reactorEff && both.reactorEff < base.reactorEff && both.reactorFloor > usual.reactorFloor && both.reactorFloor < base.reactorFloor,
              "an everyday reactor and a baseload one together: %.0f TW at %.0f%%", both.reactorTW, both.reactorEff * 100.0);
    }

    // --- Shut down, the ship runs on its bank: nothing burned, next to nothing to see, and no drive ---
    {
        GameState g; NewGame(g);
        Player &p = g.player;
        p.ship.anchor = g.systems.home; p.ship.pos = g.systems[g.systems.home].n * (0.7 * LY_U); p.ship.vel = { 0, 0, 0 };
        UpdateLoadedSystem(g);
        p.power.radar = false;
        Run(g, 2.0);
        double lit = p.power.heat, burning = g.flight.burn;
        Controls press; press.toggleReactor = true;
        double f0 = p.fuel, b0 = p.power.bank;
        Run(g, 5.0, press);
        bool down = p.power.cold;
        double spent = b0 - p.power.bank, cold = p.power.heat;
        Controls hyper; hyper.hyper = true;
        Run(g, 1.0, hyper);
        bool refused = g.flight.driveCold && !p.power.engaged;
        CHECK(burning > 0.0 && down && p.fuel == f0 && fabs(spent - 5.0 * p.spec.idleTW) < 0.02 * spent && cold < 0.1 * lit && refused,
              "reactor down for 5 s: no fuel burned, %.0f TJ out of the bank for the housekeeping, %.0f TW of heat where it gave off %.0f lit; the drive won't engage", spent, cold, lit);
        // Lighting it again takes a while, and burns fuel before it gives anything
        f0 = p.fuel; b0 = p.power.bank;
        Run(g, 5.0, press);
        bool lighting = !p.power.cold && p.power.restart > 0.0f && fabs(p.power.restart - (p.spec.reactorRestart - 5.0)) < 0.1;
        double used = f0 - p.fuel;
        bool still = p.power.bank < b0;
        Run(g, p.spec.reactorRestart);
        CHECK(lighting && fabs(used - 5.0 * p.spec.reactorFloor * p.spec.reactorBurn) < 1e-6 && still && p.power.restart == 0.0f && g.flight.load > 0.0 && p.power.bank > b0 - 15.0 * p.spec.idleTW,
              "lit again: %.0f s before it gives power, burning at its floor meanwhile (%.3f t in the first 5 s) with the bank still carrying the ship; then the bank fills", p.spec.reactorRestart, used);
        // ...and nobody freezes in the dark: with the bank flat, it's lit
        Run(g, 0.1, press);
        p.power.bank = 0.5 * p.spec.idleTW;
        Run(g, 2.0);
        CHECK(!p.power.cold && p.power.restart > 0.0f, "shut down with a nearly flat bank: when that's gone the reactor is lit again (%.0f s to go)", p.power.restart);
    }
}

// ---------------------------------------------------------------------------
// Drives and drive capacitors: the kinds there are, and what each is for
// ---------------------------------------------------------------------------
static void TestDrives()
{
    auto model = [](PartType type, const char *id) { const auto &all = Parts().models[(int)type]; for (int k = 0; k < (int)all.size(); k++) if (all[k].id == id) return k; return -1; };
    auto ship = [](int warp, int hyper, int cap) {
        ShipBuild b;
        b.hull = HULL_BULK;
        b.parts = { MakePart(PartType::Reactor, 6), MakePart(PartType::WarpDrive, 6, 0, warp), MakePart(PartType::HyperDrive, 6, 0, hyper),
                    MakePart(PartType::DriveCap, 6, 0, cap), MakePart(PartType::DriveCap, 6, 0, cap) };
        return SpecOf(b);
    };
    const double mass = 4000.0;
    // (the fuel for a run: the jump, and the passage at half the drive's speed or what the reactor will hold)
    auto run = [&](const ShipSpec &s, double ly) {
        double v = fmin(0.5 * s.warpSpeed, DriveSpeedFor(s, s.reactorTW - s.idleTW, mass, true));
        return s.jumpTJ * mass / (s.capIn * s.capOut) / (s.reactorEff * s.fuelTJ) + ReactorBurnFor(s, DrivePower(s, v, mass, true) + s.idleTW) * ly * LY_U / v;
    };
    int wCruise = model(PartType::WarpDrive, "cruise"), wShort = model(PartType::WarpDrive, "shorthaul"), wDeep = model(PartType::WarpDrive, "deeprange"),
        wSprint = model(PartType::WarpDrive, "sprint"), wHeavy = model(PartType::WarpDrive, "heavy");
    int hCruise = model(PartType::HyperDrive, "cruise"), hFast = model(PartType::HyperDrive, "fast"), hHeavy = model(PartType::HyperDrive, "heavy");
    int cSuper = model(PartType::DriveCap, "superconducting"), cBulk = model(PartType::DriveCap, "bulk");
    CHECK(wCruise == 1 && wShort == 2 && wDeep == 3 && wSprint == 4 && wHeavy == 5 && hCruise == 1 && hFast == 2 && hHeavy == 3 && cSuper == 1 && cBulk == 2,
          "six warp drives, four hyper drives and three drive capacitors, the everyday one of each first");
    if (wCruise < 0 || wShort < 0 || wDeep < 0 || wSprint < 0 || wHeavy < 0 || hCruise < 0 || hFast < 0 || hHeavy < 0 || cSuper < 0 || cBulk < 0) return;

    // --- Warp: the jump, the upkeep of the bubble, and speed, traded against each other ---
    ShipSpec usual = ship(0, 0, 0), cruise = ship(wCruise, 0, 0), shorthaul = ship(wShort, 0, 0), deep = ship(wDeep, 0, 0), sprint = ship(wSprint, 0, 0), heavy = ship(wHeavy, 0, 0);
    const double v = 0.5 * usual.warpSpeed;
    auto hold = [&](const ShipSpec &s) { return DrivePower(s, v, mass, true); };
    CHECK(fabs(cruise.warpSpeed / usual.warpSpeed - 0.7) < 1e-9 && hold(cruise) < 0.55 * hold(usual) && cruise.jumpTJ == usual.jumpTJ &&
          shorthaul.jumpTJ < 0.5 * usual.jumpTJ && hold(shorthaul) > 2.0 * hold(usual) &&
          deep.jumpTJ > 1.2 * usual.jumpTJ && hold(deep) < 0.4 * hold(usual) &&
          sprint.warpSpeed > 1.5 * usual.warpSpeed && sprint.jumpTJ > usual.jumpTJ && hold(sprint) > hold(usual) && sprint.warpAccel > usual.warpAccel &&
          fabs(heavy.warpMaxMass / usual.warpMaxMass - 2.0) < 1e-9 && heavy.warpSpeed < usual.warpSpeed,
          "holding a %.0f t ship's bubble at the same speed: everyday %.0f TW; cruising %.0f (and 70%% of the top speed); short-haul %.0f, for a jump of %.0f TJ/t against %.0f; "
          "deep-range %.0f, for %.0f TJ/t; sprint %.0f, and 60%% faster; heavy-lift moves %.0f t where the others move %.0f",
          mass, hold(usual), hold(cruise), hold(shorthaul), shorthaul.jumpTJ, usual.jumpTJ, hold(deep), deep.jumpTJ, hold(sprint), heavy.warpMaxMass, usual.warpMaxMass);
    CHECK(run(shorthaul, 2.0) < 0.6 * run(usual, 2.0) && run(shorthaul, 25.0) > run(usual, 25.0) && run(deep, 2.0) > run(usual, 2.0) && run(deep, 25.0) < 0.8 * run(usual, 25.0) &&
          run(cruise, 12.0) < run(usual, 12.0) && run(sprint, 5.0) > run(usual, 5.0),
          "fuel for a 2 ly run and a 25 ly one: everyday %.0f t and %.0f; short-haul %.0f and %.0f; deep-range %.0f and %.0f; cruising %.0f and %.0f; sprint %.0f and %.0f",
          run(usual, 2.0), run(usual, 25.0), run(shorthaul, 2.0), run(shorthaul, 25.0), run(deep, 2.0), run(deep, 25.0), run(cruise, 2.0), run(cruise, 25.0), run(sprint, 2.0), run(sprint, 25.0));

    // --- Hyper drives ---
    {
        ShipSpec c = ship(0, hCruise, 0), f = ship(0, hFast, 0), h = ship(0, hHeavy, 0);
        const double hv = 0.5 * usual.hyperSpeed;
        CHECK(DrivePower(c, hv, mass, false) < 0.6 * DrivePower(usual, hv, mass, false) && fabs(c.hyperSpeed / usual.hyperSpeed - 0.75) < 1e-9 &&
              fabs(f.hyperSpeed / usual.hyperSpeed - 1.5) < 1e-9 && DrivePower(f, hv, mass, false) > 1.3 * DrivePower(usual, hv, mass, false) && f.hyperAccel > usual.hyperAccel &&
              fabs(h.hyperMaxMass / usual.hyperMaxMass - 2.0) < 1e-9 && h.hyperSpeed < usual.hyperSpeed,
              "hyper drives: the cruising one holds its field for %.0f TW where the everyday one takes %.0f, at three quarters of the speed; the fast one is half as fast again for %.0f; "
              "heavy-lift moves %.0f t", DrivePower(c, hv, mass, false), DrivePower(usual, hv, mass, false), DrivePower(f, hv, mass, false), h.hyperMaxMass);
    }

    // --- Capacitors: what a jump draws from the reactor is what the bubble takes over what survives going in and coming out ---
    {
        ShipSpec sc = ship(0, 0, cSuper), bk = ship(0, 0, cBulk);
        auto drawn = [&](const ShipSpec &s) { return s.jumpTJ * mass / (s.capIn * s.capOut); };
        CHECK(drawn(sc) < 0.9 * drawn(usual) && drawn(bk) > 1.1 * drawn(usual) && sc.driveCapTJ < 0.7 * usual.driveCapTJ && fabs(bk.driveCapTJ / usual.driveCapTJ - 2.0) < 1e-9 &&
              JumpCharge(sc, mass) < JumpCharge(usual, mass) && JumpCharge(usual, mass) < JumpCharge(bk, mass),
              "a jump for %.0f t draws %.0f TJ from the reactor through everyday capacitors, %.0f through superconducting ones (which hold %.0f%% as much), %.0f through bulk ones (twice as much)",
              mass, drawn(usual), drawn(sc), 100.0 * sc.driveCapTJ / usual.driveCapTJ, drawn(bk));
    }

    // --- The fleets' designers choose among them ---
    {
        GameState g; NewGame(g);
        EnsureEconomy(g);
        const EconBase &base = *g.econ.base;
        int odd = 0, baseload = 0, designs = 0;
        double worstFission = 0.0, worstFusion = 0.0;
        for (int id = NO_FACTION; id < g.systems.factions.Count(); id++)
            for (int role : { ROLE_BULK, ROLE_TANKER }) {
                const ShipDesign &d = FleetDesign(base, id, role);
                designs++;
                for (const Part &p : d.build.parts) {
                    if ((p.type == PartType::WarpDrive || p.type == PartType::DriveCap) && p.variant != 0) { odd++; break; }
                }
                for (const Part &p : d.build.parts) if (p.type == PartType::Reactor && PartModelOf(p).id.find("baseload") != std::string::npos) { baseload++; break; }
                double share = d.Fuel(USUAL_RUN_LY) / d.cargo;
                if (d.fuelGood == REF_REACTOR_FUEL) worstFission = fmax(worstFission, share); else worstFusion = fmax(worstFusion, share);
            }
        CHECK(odd > designs / 2 && baseload > 0 && worstFission < 0.10 && worstFusion < 0.02,
              "of %d freighter and tanker designs, %d are built around a drive or capacitor that isn't the everyday one and %d around a baseload reactor; "
              "the thirstiest fission one burns %.1f%% of its load on a %.0f ly run, the thirstiest fusion one %.1f%%", designs, odd, baseload, 100.0 * worstFission, USUAL_RUN_LY, 100.0 * worstFusion);
    }
}

// ---------------------------------------------------------------------------
// Money: what factions earn, how it gets home, and what happens without it
// ---------------------------------------------------------------------------
static void TestMoney()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const FactionSet &fs = g.systems.factions;
    const int N = g.econ.Count(), F = fs.Count();
    Economy &e = g.econ;
    auto run = [&](GameState &h, double days) { for (int step = 0; step < (int)(days * 8.0 + 0.5); step++) { h.clock += 0.125 * MARKET_DAY; StepEconomy(h, 0.125); } };

    // --- Every faction with a seat has a way home from wherever it does business ---
    int seated = 0, lost = 0, furthest = 0;
    const Faction *state = nullptr, *company = nullptr;
    for (const Faction &f : fs.list) {
        if (f.kind == FactionKind::Government && !state) state = &f;
        if (f.kind == FactionKind::Conglomerate && !company) company = &f;
        int cap = base.capital[f.id];
        if (cap < 0) continue;
        seated++;
        for (int i = 0; i < N; i++) {
            if (base.defs[i].hub != i || i == cap) continue;
            int at = i, hops = 0;
            while (at >= 0 && at != cap && hops < N) { at = base.toCapital[(size_t)f.id * N + at]; hops++; }
            if (at != cap) { if (base.toCapital[(size_t)f.id * N + i] >= 0) lost++; } else furthest = std::max(furthest, hops);
        }
    }
    CHECK(seated >= F / 2 && lost == 0 && furthest > 5 && state && company && base.capital[state->id] >= 0 && e.money.size() == (size_t)F && e.money[state->id].treasury > 0.0,
          "%d of %d factions have a capital on the lanes; from the furthest hub it is %d hops home; every route that starts, arrives", seated, F, furthest);
    if (!state || !company) return;

    // --- A day's business: who is paid, and for what ---
    {
        double before = 0.0;
        for (const FactionMoney &m : e.money) before += m.treasury;
        run(g, 2.0);
        double gov[INC_COUNT] = {}, co[INC_COUNT] = {}, waiting = 0.0;
        for (const Faction &f : fs.list)
            for (int k = 0; k < INC_COUNT; k++) (f.kind == FactionKind::Government ? gov : co)[k] += e.money[f.id].income[k];
        for (int i = 0; i < N; i++) for (int f = 0; f < F; f++) waiting += e.Coffer(i, f);
        CHECK(gov[INC_TARIFF] > 0.0 && gov[INC_TAX] > 0.0 && gov[INC_HEADS] > 0.0 && co[INC_TARIFF] == 0.0 && co[INC_TAX] == 0.0 && co[INC_HEADS] == 0.0 &&
              co[INC_PROFIT] > 0.0 && co[INC_FREIGHT] > 0.0 && co[INC_YARDS] > 0.0 && waiting > 0.0,
              "two days in, governments are taking %s cr a day in tariffs, %s in tax on others' profits and %s from their people; the rest earn %s from their works, %s in freight and %s for new ships; "
              "%s cr is in coffers waiting to be carried home", Thousands(gov[INC_TARIFF]).c_str(), Thousands(gov[INC_TAX]).c_str(), Thousands(gov[INC_HEADS]).c_str(),
              Thousands(co[INC_PROFIT]).c_str(), Thousands(co[INC_FREIGHT]).c_str(), Thousands(co[INC_YARDS]).c_str(), Thousands(waiting).c_str());
    }

    // --- Money earned far away comes home a hop at a time, and raiders take a share on the way ---
    {
        GameState h; NewGame(h); EnsureEconomy(h);
        Economy &m = h.econ;
        const int f = state->id, cap = base.capital[f];
        int far = -1, hops = 0;
        for (int i = 0; i < N && far < 0; i++) {
            if (base.defs[i].hub != i) continue;
            int at = i, n = 0;
            while (at >= 0 && at != cap && n < 50) { at = base.toCapital[(size_t)f * N + at]; n++; }
            if (at == cap && n == 4) { far = i; hops = n; }
        }
        CHECK(far >= 0, "found a hub four hops from %s's capital (%s)", state->name.c_str(), far >= 0 ? base.defs[far].name.c_str() : "?");
        if (far < 0) return;
        for (float &r : m.raid) r = 0.0f;
        for (double &until : m.raidUntil) until = 1e12;
        for (double &c : m.coffer) c = 0.0;
        for (double &c : m.moving) c = 0.0;
        m.paying = true;
        const double sum = 1.0e12;             // (far more than anything else that is moving: it can be followed)
        // A coffer waits for its market's sailing: every three days
        m.coffer[(size_t)far * F + f] = sum;
        int waited = 0;
        while (m.Coffer(far, f) >= 0.99 * sum && waited < 20) { h.clock += 0.51 * MARKET_DAY; StepEconomy(h, 1e-6); waited++; }
        CHECK(waited >= 1 && waited <= 6, "a coffer at %s waits for the treasury ship: it sails within three days (here, %.1f)", base.defs[far].name.c_str(), 0.5 * waited);
        for (double &c : m.coffer) c = 0.0;
        for (double &c : m.moving) c = 0.0;
        m.moving[(size_t)far * F + f] = sum;
        auto where = [&]() { for (int i = 0; i < N; i++) if (m.Moving(i, f) >= 0.99 * sum) return i; return -1; };
        double home0 = m.money[f].treasury;
        std::vector<int> path;
        // (a collection every half day: step just over that, and look where the money is)
        for (int k = 0; k < hops + 2; k++) { h.clock += 0.51 * MARKET_DAY; StepEconomy(h, 1e-6); path.push_back(where()); }
        int next = base.toCapital[(size_t)f * N + far], after = base.toCapital[(size_t)f * N + next];
        CHECK(path[0] == next && path[1] == after && path[hops - 1] == -1 && m.money[f].treasury > home0 + 0.99 * sum,
              "once it has sailed from %s it doesn't stop: half a day later it is at %s, then %s; after %d hops it is in the treasury", base.defs[far].name.c_str(), base.defs[next].name.c_str(),
              base.defs[after].name.c_str(), hops);
        // ...with raiders about
        int clan = -1;
        for (const Faction &c : fs.list) if (c.kind == FactionKind::Clan && clan < 0) clan = c.id;
        int raided = -1;
        for (int i = 0; i < N && raided < 0; i++)
            if (base.defs[i].hub == i && i != cap && base.toCapital[(size_t)f * N + i] >= 0 && Len(base.defs[i].ly - fs.list[clan].seatLy) < 0.4 * fs.list[clan].reach) raided = i;
        CHECK(raided >= 0, "found a hub in a clan's patch (%s)", raided >= 0 ? base.defs[raided].name.c_str() : "?");
        if (raided < 0) return;
        for (float &r : m.raid) r = 1.0f;
        for (double &c : m.coffer) c = 0.0;
        for (double &c : m.moving) c = 0.0;
        m.moving[(size_t)raided * F + f] = sum;
        double loot0 = 0.0, loot1 = 0.0;
        for (const Faction &c : fs.list) if (c.kind == FactionKind::Clan) loot0 += m.money[c.id].treasury;
        int to = base.toCapital[(size_t)f * N + raided];
        double there0 = to == cap ? m.money[f].treasury : m.Moving(to, f);
        h.clock += 0.51 * MARKET_DAY; StepEconomy(h, 1e-6);
        for (const Faction &c : fs.list) if (c.kind == FactionKind::Clan) loot1 += m.money[c.id].treasury;
        double got = (to == cap ? m.money[f].treasury : m.Moving(to, f)) - there0;
        CHECK(got < 0.97 * sum && got > 0.8 * sum && loot1 - loot0 > 0.3 * (sum - got) && loot1 - loot0 < 0.5 * (sum - got),
              "sent on through raided space, %s of the trillion arrives; the clans are %s the richer for it (what the rest fetched them)", Thousands(got).c_str(), Thousands(loot1 - loot0).c_str());
    }

    // --- A new ship goes to whoever can pay for it ---
    {
        GameState h; NewGame(h); EnsureEconomy(h);
        Economy &m = h.econ;
        int hub = -1; const FleetAt *top = nullptr;
        for (int i = 0; i < N && hub < 0; i++) {
            if (base.defs[i].hub != i || m.fleets[i].size() < 2) continue;
            bool yard = false;
            for (int j = 0; j < N; j++) if (base.defs[j].hub == i) for (const IndustryDef &in : base.defs[j].ind) if (in.type == IND_SHIPYARD && in.cap > 0.0f) yard = true;
            if (yard) continue;
            const FleetAt *best = &m.fleets[i][0];
            for (const FleetAt &f : m.fleets[i]) if (f.ships[ROLE_BULK] > best->ships[ROLE_BULK]) best = &f;
            if (best->owner >= 0 && best->ships[ROLE_BULK] > 0.0f) { hub = i; top = best; }
        }
        CHECK(hub >= 0, "found a hub with no yard of its own whose biggest fleet is a faction's (%s)", hub >= 0 ? base.defs[hub].name.c_str() : "?");
        if (hub < 0) return;
        const int who = top->owner;
        auto ships = [&](int owner) { for (const FleetAt &f : m.fleets[hub]) if (f.owner == owner) return (double)f.ships[ROLE_BULK]; return 0.0; };
        for (int r = 0; r < ROLE_COUNT; r++) m.busy[(size_t)hub * ROLE_COUNT + r] = r == ROLE_BULK ? 2.0f : 0.0f;
        const ShipDesign &d = FleetDesign(base, who, ROLE_BULK);
        const double tonnes = 2.0 * d.tonnage, price = tonnes * d.value / d.tonnage;
        double had = ships(who), others = ships(NO_FACTION);
        m.money[who].treasury = 10.0 * price; m.money[who].overhead = 0.0f;
        m.slips[hub] = (float)tonnes;
        h.clock += 5.0; StepEconomy(h, 1e-6);
        double paid = 10.0 * price - m.money[who].treasury, gained = ships(who) - had;
        m.money[who].treasury = 0.5 * price;
        for (int r = 0; r < ROLE_COUNT; r++) m.busy[(size_t)hub * ROLE_COUNT + r] = r == ROLE_BULK ? 2.0f : 0.0f;
        m.slips[hub] = (float)tonnes;
        had = ships(who);
        h.clock += 5.0; StepEconomy(h, 1e-6);
        CHECK(fabs(gained - 2.0) < 0.01 && fabs(paid - price) < 1e-3 * price && fabs(ships(who) - had) < 0.01 && fabs(m.money[who].treasury - 0.5 * price) < 1e-3 * price && m.money[who].deniedNow + m.money[who].denied > 0.0f &&
              ships(NO_FACTION) > others + 1.0,
              "two bulk freighters off the slips at %s: with money at its capital %s takes them for %s cr; without, it goes without and independent owners have them",
              base.defs[hub].name.c_str(), fs.list[who].name.c_str(), Thousands(price).c_str());
    }

    // --- A government that can't pay its way taxes harder, and its people feel it ---
    {
        GameState h; NewGame(h); EnsureEconomy(h);
        Economy &m = h.econ;
        const int f = state->id;
        int world = -1;
        for (int i = 0; i < N && world < 0; i++) if (base.defs[i].ruler == f && base.defs[i].pop > 1e6) world = i;
        run(h, 3.0);
        double living = LivingStandard(m, fs, world), tariff = TariffNow(m, f);
        float eased = m.money[f].squeeze;
        // (robbed blind: nothing gets home for a fortnight)
        for (int day = 0; day < 14; day++) { m.money[f].treasury = 0.0; for (int i = 0; i < N; i++) m.coffer[(size_t)i * F + f] = m.moving[(size_t)i * F + f] = 0.0; run(h, 1.0); m.money[f].treasury = 0.0; }
        float squeezed = m.money[f].squeeze;
        double taxed = LivingStandard(m, fs, world);
        CHECK(squeezed > eased + 0.2f && squeezed <= 2.0f && TariffNow(m, f) > 1.15 * tariff && m.money[f].denied > 0.0f,
              "%s, with nothing reaching its treasury for a fortnight, can't meet its bills: its taxes go from %.2f of the usual to %.2f, its tariff from %.1f%% to %.1f%% "
              "(living standard at %s %.2f, where it was %.2f)", state->name.c_str(), eased, squeezed, 100.0 * tariff, 100.0 * TariffNow(m, f), base.defs[world].name.c_str(), taxed, living);
        // (the tax itself, all else equal)
        float keep = m.money[f].squeeze;
        m.money[f].squeeze = 1.0f; double usual = LivingStandard(m, fs, world);
        m.money[f].squeeze = 2.0f; double hard = LivingStandard(m, fs, world);
        m.money[f].squeeze = keep;
        CHECK(fabs(usual - hard - 0.06) < 1e-6, "taxed twice as hard, a world's living standard is %.2f lower", usual - hard);
        // ...and all of it is saved
        m.coffer[(size_t)world * F + f] = 12345.0;
        CHECK(SaveGame(h, "test_money.txt"), "saved");
        GameState back;
        bool loaded = LoadGame(back, "test_money.txt");
        CHECK(loaded && back.econ.money.size() == m.money.size() && back.econ.money[f].squeeze == m.money[f].squeeze && fabs(back.econ.money[company->id].treasury - m.money[company->id].treasury) < 1.0 &&
              back.econ.Coffer(world, f) >= 12345.0 && back.econ.Coffer(world, f) < 12345.0 + 1e9 && fabs(back.econ.money[f].usual - m.money[f].usual) < 1e-3 * m.money[f].usual,
              "loaded again: %s has %s cr, and %s's taxes are where they were", company->name.c_str(), Thousands(back.econ.money[company->id].treasury).c_str(), state->name.c_str());
        remove("test_money.txt");
    }
    // --- Where ships are flown, a paymaster's ship carries the coffer: and can be robbed ---
    {
        GameState h; NewGame(h);
        Run(h, 2.0);
        Economy &m = h.econ;
        const SysId H = h.systems.home;
        int from = -1, hub = -1;
        for (int i = 0; i < N; i++) if (base.defs[i].sys == H) { hub = base.defs[i].hub; if (base.defs[i].hub != i && from < 0 && BodyIndex(h, base.defs[i].name) >= 0) from = i; }
        int who = -1;
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Government && base.capital[f.id] >= 0 && base.capital[f.id] != hub && who < 0) who = f.id;
        CHECK(from >= 0 && hub >= 0 && who >= 0 && m.zone[from] == 2, "a world in the home system (%s) with a coffer of %s's", from >= 0 ? base.defs[from].name.c_str() : "?", who >= 0 ? fs.list[who].name.c_str() : "?");
        if (from < 0 || who < 0) return;
        const double sum = 5.0e6;
        m.moving[(size_t)from * F + who] = sum;                     // (as on its sailing day)
        double atHub = m.Moving(hub, who);
        int id = -1;
        for (int t = 0; t < 400 && id < 0; t++) { Run(h, 1.0); for (const Npc &n : h.npcs) if (n.credits > 0.0 && n.payee == who && n.from == from) id = n.id; }
        const Npc *ship = nullptr;
        for (const Npc &n : h.npcs) if (n.id == id) ship = &n;
        CHECK(ship && ship->name.find("paymaster") != std::string::npos && ship->to == hub && fabs(ship->credits - sum) < 1.0 + 0.05 * sum && m.Moving(from, who) < 0.05 * sum && ship->faction == who,
              "on its sailing day the %s casts off from %s for %s with %s cr aboard", ship ? ship->name.c_str() : "?", base.defs[from].name.c_str(), base.defs[hub].name.c_str(),
              ship ? Thousands(ship->credits).c_str() : "?");
        if (!ship) return;
        const double aboard = ship->credits;
        // Left alone, it gets there
        {
            GameState other = h;
            bool gone = false;
            for (int t = 0; t < 900 && !gone; t++) { Run(other, 1.0); gone = true; for (const Npc &n : other.npcs) if (n.id == id) gone = false; }
            double onward = other.econ.Moving(hub, who) + other.econ.money[who].treasury;
            for (const Specie &s : other.econ.specie) if (s.faction == who) onward += s.credits;
            for (const Npc &n : other.npcs) if (n.payee == who) onward += n.credits;
            for (const Transit &t : other.transits) if (t.payee == who) onward += t.credits;
            CHECK(gone && onward >= atHub + h.econ.money[who].treasury + 0.9 * aboard,
                  "left alone it docks at %s, and the money goes on from there toward the capital", base.defs[hub].name.c_str());
        }
        // A captain who holds alongside has it instead
        bool hailed = BoardShip(h, h, id);
        float regard = h.Standing(who);
        bool under = false;
        for (int t = 0; t < 120 && !under; t++) { Run(h, 0.5); for (const Npc &n : h.npcs) if (n.id == id && n.doing != NpcDo::Dock) under = true; }
        for (int t = 0; t < 30 && h.player.marked <= 0.0; t++) {
            for (const Npc &n : h.npcs) if (n.id == id) { h.player.ship.anchor = H; h.player.ship.pos = n.pos + DVec3{ 50.0 * KM_U, 0, 0 }; h.player.ship.vel = ToF(n.vel); }
            Run(h, 0.25);
        }
        double left = -1.0;
        for (const Npc &n : h.npcs) if (n.id == id) left = n.credits;
        bool told = false;
        for (const EconNews &n : h.news) if (n.text.find("boarded") != std::string::npos && n.faction == who) told = true;
        CHECK(hailed && under && fabs(h.player.marked - aboard) < 1.0 && left == 0.0 && h.Standing(who) < regard - 30.0f && told && h.boardTarget == -1,
              "held 50 km off her for four seconds under way, the captain has the %s cr aboard as marked credits; the paymaster's ship flies on empty, it's in the news, and %s thinks %.0f less of them",
              Thousands(aboard).c_str(), fs.list[who].name.c_str(), regard - h.Standing(who));
        bool not1 = BoardShip(h, h, id);
        CHECK(!not1, "there is nothing left aboard her to go back for");
        // Marked credits: a free port's fence gives half
        int port = BodyIndex(h, base.defs[hub].name);
        const Faction *owner = fs.Get(m.state[hub].owner);
        double credits = h.player.credits;
        Park(h, port, 0.3);
        Run(h, 1.0);
        CHECK(owner && owner->kind == FactionKind::FreePort && h.player.marked == 0.0 && fabs(h.player.credits - credits - MARKED_FENCE * aboard) < 1.0,
              "docked at %s, a fence takes them for %s cr", base.defs[hub].name.c_str(), Thousands(h.player.credits - credits).c_str());
    }
}

// ---------------------------------------------------------------------------
// Fleets: the ships that carry the trade, what they burn, and what happens without them
// ---------------------------------------------------------------------------
static void TestFleets()
{
    static const int FUELS[3] = { REF_REACTOR_FUEL, REF_FUEL, RAW_HELIUM3 };
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const FactionSet &fs = g.systems.factions;
    const int N = g.econ.Count();

    // --- Every faction has a ship for every job, out of parts a captain could buy ---
    int bad = 0, fission = 0, designs = 0;
    const Faction *line = nullptr, *clan = nullptr;
    const ShipDesign *deut = nullptr;
    for (const Faction &f : fs.list) {
        if (f.kind == FactionKind::Conglomerate && f.slant == Slant::Shipping && !line) line = &f;
        if (f.kind == FactionKind::Clan && !clan) clan = &f;
    }
    for (int id = NO_FACTION; id < fs.Count(); id++)
        for (int role = 0; role < ROLE_COUNT; role++) {
            const ShipDesign &d = FleetDesign(base, id, role);
            const Faction *f = fs.Get(id);
            designs++;
            bool carries = role == ROLE_LINER ? d.berths > 0.0 : d.cargo > 0.0;
            bool flies = d.localSec > 0.0 && d.localFuel > 0.0 &&
                         (role == ROLE_HAULER || (d.spec.warpSpeed > 0.0 && d.fixedFuel > 0.0 && d.Fuel(10.0) > d.Fuel(1.0) && d.Seconds(10.0) > d.Seconds(1.0)));
            bool fits = VolumeUsed(d.build) <= BuildVolume(d.build) && d.tonnage > 0.0 && d.value > 0.0 && d.spec.tank >= (role == ROLE_HAULER ? d.localFuel : d.Fuel(FLEET_RANGE_LY));
            if (!carries || !flies || !fits) bad++;
            if (role != ROLE_HAULER && d.fuelGood == REF_REACTOR_FUEL && !(f && f->kind == FactionKind::Clan)) fission++;
            if (role == ROLE_BULK && d.build.hull == HULL_BULK && d.fuelGood == REF_FUEL && !deut) deut = &d;
        }
    CHECK(bad == 0 && fission == 0, "%d designs (every faction's freighter, hauler, tanker and liner): all carry, fly, fit their hulls and hold the fuel for their longest run; none that jumps for a living burns fission fuel but the clans' (%d do)",
          designs, fission);
    const ShipDesign &plain = FleetDesign(base, NO_FACTION, ROLE_BULK);
    CHECK(line && clan && deut, "found a shipping line, a clan, and a bulk carrier burning deuterium");
    if (!line || !clan || !deut) return;
    const ShipDesign &liner = FleetDesign(base, line->id, ROLE_BULK), &raider = FleetDesign(base, clan->id, ROLE_BULK);
    auto perTonne = [](const ShipDesign &d) { return d.Fuel(5.0) / d.cargo; };
    CHECK(liner.cargo > 3.0 * plain.cargo && liner.cruise < plain.cruise && raider.cruise > plain.cruise && raider.fuelGood == REF_REACTOR_FUEL &&
          perTonne(raider) > 3.0 * perTonne(*deut),
          "%s carries %.0f t (an independent's, %.0f t) and runs its drives easier; a clan's is quick and burns %.1f%% of its load in fission fuel a run, where a deuterium carrier's takes %.1f%%",
          liner.name.c_str(), liner.cargo, plain.cargo, 100.0 * perTonne(raider), 100.0 * perTonne(*deut));

    // --- Fleets: ships at every hub that trades ---
    std::vector<char> laned(N, 0);
    for (const Lane &l : base.lanes) { laned[l.a] = laned[l.b] = 1; }
    auto count = [&](const Economy &e, int role) { double n = 0.0; for (const auto &at : e.fleets) for (const FleetAt &f : at) n += f.ships[role]; return n; };
    int hubs = 0, shipless = 0, owners = 0;
    for (int i = 0; i < N; i++) {
        if (base.defs[i].hub != i) continue;
        hubs++;
        double here = 0.0;
        for (const FleetAt &f : g.econ.fleets[i]) { here += f.ships[ROLE_BULK]; owners++; }
        if (laned[i] && here <= 0.0) shipless++;
    }
    CHECK(shipless == 0 && count(g.econ, ROLE_BULK) > 100.0 && count(g.econ, ROLE_HAULER) > 50.0 && count(g.econ, ROLE_TANKER) > 10.0 && count(g.econ, ROLE_LINER) > 100.0 && owners > hubs,
          "%d hubs: %.0f bulk freighters, %.0f local haulers, %.0f tankers and %.0f liners based at them, in %d owners' fleets; no hub on a lane without freighters",
          hubs, count(g.econ, ROLE_BULK), count(g.econ, ROLE_HAULER), count(g.econ, ROLE_TANKER), count(g.econ, ROLE_LINER), owners);

    // --- Trade takes ships, and ships take fuel ---
    auto day = [&](GameState &h, double &burned, double &missing, float &busy) {
        memset(g_flow, 0, sizeof g_flow); g_flowDays = 0.0;
        for (int step = 0; step < 8; step++) StepEconomy(h, 0.125);
        burned = missing = 0.0;
        for (int k = 0; k < 3; k++) { burned += g_flow[3][FUELS[k]]; missing += g_flow[4][FUELS[k]]; }
        double all = 0.0; int n = 0;
        for (int i = 0; i < N; i++) if (base.defs[i].hub == i && laned[i]) { all += h.econ.busy[(size_t)i * ROLE_COUNT + ROLE_BULK]; n++; }
        busy = (float)(all / std::max(n, 1));
    };
    double burnA, missA, burnB, missB, burnC, missC; float busyA, busyB, busyC;
    day(g, burnA, missA, busyA);
    GameState b; NewGame(b); EnsureEconomy(b);
    for (auto &at : b.econ.fleets) for (FleetAt &f : at) for (float &n : f.ships) n = 0.0f;
    int food = 0; double before = 0.0, after = 0.0;
    for (int i = 0; i < N; i++) if (b.econ.state[i].out[REF_FOOD] <= 0.0f && b.econ.state[i].use[REF_FOOD] > 0.0f) { food++; before += b.econ.state[i].stock[REF_FOOD]; }
    day(b, burnB, missB, busyB);
    for (int i = 0; i < N; i++) if (b.econ.state[i].out[REF_FOOD] <= 0.0f && b.econ.state[i].use[REF_FOOD] > 0.0f) after += b.econ.state[i].stock[REF_FOOD];
    CHECK(burnA > 1000.0 && burnB < 0.01 * burnA && busyB > 1.5f && busyB > busyA && food > 100 && after < before,
          "a day's trade burns %.0f t of the fleets' fuel; with no ships next to nothing moves (%.0f t burned, by the few the yards launched that day), cargo piles up "
          "(%.1f times what there'd be to carry it, against %.1f), and the %d places that grow no food eat into what they have", burnA, burnB, busyB, busyA, food);
    GameState c; NewGame(c); EnsureEconomy(c);
    for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) c.econ.state[i].stock[FUELS[k]] = 0.0f;
    day(c, burnC, missC, busyC);
    CHECK(burnC < 0.05 * burnA && missC > 0.2 * burnA, "with every port's tanks dry the ships are stranded: %.0f t burned in the day (what little was made), %.0f t wanted and not there", burnC, missC);
    // A port keeps back what its own power plants run on
    {
        int m = -1, fuel = -1;
        for (int i = 0; i < N && m < 0; i++) for (int k = 0; k < 3; k++) if (c.econ.state[i].target[FUELS[k]] > 100.0f) { m = i; fuel = FUELS[k]; break; }
        MarketState &ms = c.econ.state[m];
        ms.stock[fuel] = 0.2f * ms.target[fuel];
        bool low = Bunker(c.econ, m, fuel, 5.0);
        ms.stock[fuel] = ms.target[fuel];
        float had = ms.stock[fuel];
        bool full = Bunker(c.econ, m, fuel, 5.0);
        CHECK(!low && full && fabsf(had - ms.stock[fuel] - 5.0f) < 0.01f, "%s won't sell ships %s out of its last fifth; with its tanks full a ship takes its 5 t", base.defs[m].name.c_str(), GoodData(fuel).name);
    }

    // --- Ships wear out, raiders take some, the yards build more ---
    {
        GameState h; NewGame(h); EnsureEconomy(h);
        double start = 0.0, built = 0.0;
        for (int r = 0; r < ROLE_COUNT; r++) start += count(h.econ, r);
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Clan) { h.econ.raid[f.id] = 1.0f; h.econ.raidUntil[f.id] = 1e12; }
        for (int step = 0; step < 80; step++) StepEconomy(h, 0.25);
        double now = 0.0, taken = 0.0;
        for (int r = 0; r < ROLE_COUNT; r++) now += count(h.econ, r);
        for (float n : h.econ.clanShips) taken += n;
        for (int i = 0; i < N; i++)
            for (size_t k = 0; k < base.defs[i].ind.size() && k < h.econ.state[i].ind.size(); k++) if (base.defs[i].ind[k].type == IND_SHIPYARD) built += h.econ.state[i].ind[k].output;
        CHECK(now > 0.9 * start && now < 1.25 * start && taken > 0.3 && built > 100.0,
              "20 days on, with every clan raiding flat out: %.0f ships where there were %.0f (a fiftieth wore out, the yards built more), %.1f taken by the clans and kept, and the yards turning out %.0f t of ship a day", now, start, taken, built);
        // ...and all of it is saved
        CHECK(SaveGame(h, "test_fleets.txt"), "saved");
        GameState back;
        bool loaded = LoadGame(back, "test_fleets.txt");
        double again = 0.0, theirs = 0.0;
        if (loaded) { for (int r = 0; r < ROLE_COUNT; r++) again += count(back.econ, r); for (float n : back.econ.clanShips) theirs += n; }
        CHECK(loaded && fabs(again - now) < 0.01 * now && fabs(theirs - taken) < 0.01 * taken + 0.1, "loaded again: %.0f ships in the fleets, %.0f with the clans", again, theirs);
        remove("test_fleets.txt");
    }

    // --- The freighters that are flown are those ships: their owners' designs, fuelled from the port ---
    {
        GameState t; NewGame(t);
        g_bunkered = 0.0;
        Run(t, 240.0);
        int freighters = 0, over = 0, theirs = 0;
        for (const Npc &n : t.npcs) {
            if (n.kind != NpcKind::Freighter) continue;
            freighters++;
            if (n.total > n.spec.hold * 1.01 + 1.0) over++;
            for (int role = 0; role < ROLE_COUNT; role++) if (fabs(n.spec.dryMass - FleetDesign(base, n.faction, role).spec.dryMass) < 0.5) { theirs++; break; }
        }
        double bought = g_bunkered;
        CHECK(freighters >= 3 && over == 0 && theirs == freighters && bought > 0.0,
              "four minutes in the home system: %d freighters in flight, every one its owner's design for the job with its load inside what it holds; %.1f t of fuel has left the ports' stock for them",
              freighters, bought);
    }

    // --- A world's page shows its system's shipping, and it reaches a client ---
    {
        int world = -1; const Body *body = nullptr;
        for (int i = 0; i < N && !body; i++) {
            if (base.defs[i].sys != g.systems.home || base.defs[i].hub == i) continue;
            for (const Body &bd : g.loaded.bodies) if (bd.id == base.defs[i].bodyId) { world = i; body = &bd; break; }
        }
        CHECK(body != nullptr, "found a world in the home system that isn't its hub (%s)", body ? body->name.c_str() : "?");
        if (!body) return;
        RecordWorld(g, g, *body);
        const WorldRecord &rec = g.know.worlds[base.defs[world].bodyId];
        const int hub = base.defs[world].hub;
        double based = 0.0;
        for (const FleetAt &f : g.econ.fleets[hub]) based += f.ships[ROLE_BULK];
        std::vector<uint8_t> msg = MsgWorld(base.defs[world].bodyId, rec);
        Reader r(msg);
        uint64_t id = 0; WorldRecord back;
        bool type = r.U8() == MSG_WORLD, read = ReadWorld(r, id, back);
        CHECK(based > 0.0 && fabs(rec.fleet[ROLE_BULK] - based) < 1e-3 * based && rec.busy[ROLE_BULK] > 0.0f && type && read && back.fleet[ROLE_BULK] == rec.fleet[ROLE_BULK] &&
              back.fleet[ROLE_LINER] == rec.fleet[ROLE_LINER] && back.busy[ROLE_TANKER] == rec.busy[ROLE_TANKER],
              "%s's page shows the %.1f freighters based at %s and how full their holds are (%.0f%%), and a client is sent the same", base.defs[world].name.c_str(), based,
              base.defs[hub].name.c_str(), 100.0f * rec.busy[ROLE_BULK]);
    }
}

// ---------------------------------------------------------------------------
// People: living standards, growth and famine, migration and borders, and carrying them
// ---------------------------------------------------------------------------
static void TestPeople()
{
    GameState g; NewGame(g);
    EnsureEconomy(g);
    const EconBase &base = *g.econ.base;
    const FactionSet &fs = g.systems.factions;
    const int N = g.econ.Count();
    auto stocked = [&](int m, float share) { MarketState &s = g.econ.state[m]; for (int k = 0; k < GOOD_COUNT; k++) s.stock[k] = share * s.target[k]; };
    // A colony that isn't its system's hub, and that hub: both under one government
    int colony = -1;
    for (int i = 0; i < N && colony < 0; i++) {
        const MarketDef &d = base.defs[i];
        if (d.hub != i && d.hub >= 0 && d.pop > 5000.0 && d.pop < 5e6 && base.defs[d.hub].pop > 20.0 * d.pop && d.sys != g.systems.home && !d.station) colony = i;
    }
    CHECK(colony >= 0, "found a colony with a bigger neighbour (%s, by %s)", colony >= 0 ? base.defs[colony].name.c_str() : "?", colony >= 0 ? base.defs[base.defs[colony].hub].name.c_str() : "?");
    if (colony < 0) return;
    const int hub = base.defs[colony].hub;

    // --- The living standard ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[colony];
        stocked(colony, 1.2f);
        s.employed = s.labour; s.housing = (float)(1.05 * s.pop);
        for (float &r : g.econ.raid) r = 0.0f;
        double best = LivingStandard(g.econ, fs, colony);
        s.stock[REF_FOOD] = 0.0f;
        double hungry = LivingStandard(g.econ, fs, colony);
        stocked(colony, 1.2f);
        s.housing = (float)(0.5 * s.pop);
        double crowded = LivingStandard(g.econ, fs, colony);
        s.housing = (float)(1.05 * s.pop); s.employed = 0.0f;
        double idle = LivingStandard(g.econ, fs, colony);
        s.employed = s.labour;
        s.stock[REF_WATER] = s.stock[GOOD_MEDICINES] = s.stock[GOOD_CONSUMER] = s.stock[GOOD_LIQUOR] = s.stock[GOOD_LUXURY] = 0.0f;
        double bare = LivingStandard(g.econ, fs, colony);
        CHECK(fabs(best - 1.0) < 1e-6 && fabs(best - hungry - (LIVING_FOOD + LIVING_RESERVE)) < 1e-6 && fabs(best - crowded - 0.5 * LIVING_HOUSING) < 0.01 &&
              fabs(best - idle - LIVING_WORK) < 1e-6 && best - bare > 0.1 && hungry < LEAVE_BELOW && best > GROW_ABOVE,
              "fed, housed, in work and supplied, a world's living standard is %.0f%%; with no food %.0f%%, with roofs for half %.0f%%, with no work %.0f%%, with no water, medicine or comforts %.0f%%",
              best * 100.0, hungry * 100.0, crowded * 100.0, idle * 100.0, bare * 100.0);
        g.econ = was;
    }

    // --- Growth, up to the roofs there are; famine; and never quite empty ---
    {
        Economy was = g.econ;
        MarketState &s = g.econ.state[colony];
        int city = -1;
        for (int k = 0; k < (int)s.ind.size(); k++) if (base.defs[colony].ind[k].type == IND_CITY) city = k;
        double pop = s.pop, housing = CityHousing(s.ind[city].cap);
        for (int k = 0; k < GOOD_COUNT; k++) if (k != REF_STEEL && k != GOOD_MACHINERY && k != GOOD_HABITAT_MODULES) s.stock[k] = 1.2f * s.target[k];
        s.stock[REF_STEEL] = s.stock[GOOD_MACHINERY] = s.stock[GOOD_HABITAT_MODULES] = 0.0f;      // (nothing to build with)
        for (int i = 0; i < 40; i++) { s.stock[REF_FOOD] = 1.2f * s.target[REF_FOOD]; s.stock[REF_STEEL] = s.stock[GOOD_HABITAT_MODULES] = 0.0f; StepEconomy(g, 0.1); }
        double full = s.pop;
        bool capped = full > 1.03 * pop && full <= housing * 1.0001 && s.trend < 0.001f;
        // ...and with the materials for more city, more people
        g.clock += MARKET_DAY;
        for (int i = 0; i < 60; i++) {
            s.stock[REF_FOOD] = 1.2f * s.target[REF_FOOD];
            s.stock[REF_STEEL] = s.stock[GOOD_MACHINERY] = s.stock[GOOD_HABITAT_MODULES] = 5000.0f;
            g.clock += 0.1 * MARKET_DAY;
            StepEconomy(g, 0.1);
        }
        CHECK(capped && s.pop > 1.03 * full && CityHousing(s.ind[city].cap) > 1.03 * housing,
              "well fed, %s grows from %s to the %s its cities can house, and stops; given steel and habitat modules its cities grow, and in six days it has %s",
              base.defs[colony].name.c_str(), FormatPopulation(pop).c_str(), FormatPopulation(full).c_str(), FormatPopulation(s.pop).c_str());
        g.econ = was;

        MarketState &t = g.econ.state[colony];
        for (uint8_t &b : g.econ.border) b = BORDER_CLOSED;                 // (nowhere to go)
        double before = t.pop;
        for (int i = 0; i < 50; i++) { t.stock[REF_FOOD] = 0.0f; t.stock[RAW_GRAIN] = t.stock[RAW_LIVESTOCK] = 0.0f; g.econ.state[hub].stock[REF_FOOD] = 0.0f; StepEconomy(g, 0.1); }
        double starved = t.pop;
        t.pop = 100.0;
        // (in small steps, so that what trade brings in is gone again before anyone eats it)
        for (int i = 0; i < 600; i++) { t.stock[REF_FOOD] = 0.0f; t.stock[RAW_GRAIN] = t.stock[RAW_LIVESTOCK] = 0.0f; g.econ.state[hub].stock[REF_FOOD] = 0.0f; StepEconomy(g, 0.1); }
        CHECK(starved < 0.93 * before && starved > 0.5 * before && t.trend <= 0.0f && fabs(t.pop - SKELETON_CREW) < 1.0 && t.living < GROW_ABOVE,
              "with nothing to eat it loses %.0f%% of its people in five days; left long enough, %.0f caretakers remain and no fewer", 100.0 * (1.0 - starved / before), t.pop);
        g.econ = was;
    }

    // --- Migration: a hop at a time to somewhere better; unless the border's shut ---
    {
        Economy was = g.econ;
        for (uint8_t &b : g.econ.border) b = BORDER_OPEN;
        MarketState &s = g.econ.state[colony], &h = g.econ.state[hub];
        stocked(hub, 1.2f);
        int city = -1;
        for (int k = 0; k < (int)h.ind.size(); k++) if (base.defs[hub].ind[k].type == IND_CITY) city = k;
        h.ind[city].cap *= 1.05f;                                  // (room at the hub)
        s.stock[REF_FOOD] = 0.0f; s.stock[RAW_GRAIN] = s.stock[RAW_LIVESTOCK] = 0.0f;
        double a0 = s.pop, b0 = h.pop;
        StepEconomy(g, 0.02);
        s.stock[REF_FOOD] = 0.0f;
        StepEconomy(g, 0.02);
        bool going = s.leaving > 1.0f && s.bestTo == hub && s.pop < a0 && h.refugees > 0.0f && s.living < LEAVE_BELOW && h.living > s.living + 0.05f;
        CHECK(going && h.pop > b0, "%s runs out of food: %.0f people a day leave for %s, where life is better (%.0f%% against %.0f%%)", base.defs[colony].name.c_str(),
              s.leaving, base.defs[hub].name.c_str(), h.living * 100.0f, s.living * 100.0f);
        g.econ = was;

        // Two hubs with a lane between them and different governments: the border between them
        int from = -1, to = -1;
        for (const Lane &l : base.lanes) {
            const MarketDef &a = base.defs[l.a], &b = base.defs[l.b];
            if (a.ruler == b.ruler || a.ruler < 0 || b.ruler <= 0 || a.pop < 1e5 || b.pop < 1e5) continue;
            from = l.a; to = l.b;
            break;
        }
        CHECK(from >= 0, "found a lane across a border (%s to %s)", from >= 0 ? base.defs[from].name.c_str() : "?", to >= 0 ? base.defs[to].name.c_str() : "?");
        if (from >= 0) {
            auto run = [&](uint8_t policy, bool work) {
                g.econ = was;
                for (uint8_t &b : g.econ.border) b = BORDER_OPEN;
                g.econ.border[base.defs[to].ruler] = policy;
                g.econ.bordersLooked = g.clock;                    // (the policy as set: no second thoughts meanwhile)
                MarketState &a = g.econ.state[from], &b = g.econ.state[to];
                for (int i = 0; i < N; i++) if (base.defs[i].hub == from || base.defs[i].hub == to) stocked(i, 1.2f);
                for (int k = 0; k < (int)b.ind.size(); k++) if (base.defs[to].ind[k].type == IND_CITY) b.ind[k].cap *= 1.05f;
                // (everywhere else the lanes from there go is no better, so it's this way or none)
                for (const Lane &l : base.lanes) { int other = l.a == from ? l.b : l.b == from ? l.a : -1; if (other >= 0 && other != to) g.econ.state[other].stock[REF_FOOD] = 0.0f; }
                for (int i = 0; i < N; i++) if (base.defs[i].hub == from && i != from) g.econ.state[i].stock[REF_FOOD] = 0.0f;
                a.stock[REF_FOOD] = 0.0f; a.stock[RAW_GRAIN] = a.stock[RAW_LIVESTOCK] = 0.0f;
                if (!work) for (int k = 0; k < (int)b.ind.size(); k++) if (!IsCivic(base.defs[to].ind[k].type)) b.ind[k].cap *= 0.05f;
                StepEconomy(g, 0.01);
                a.stock[REF_FOOD] = 0.0f;
                StepEconomy(g, 0.01);
                a.stock[REF_FOOD] = 0.0f;
                StepEconomy(g, 0.01);
            };
            // (counted where they arrive: some will always find another way out)
            run(BORDER_OPEN, true);
            float open = g.econ.state[to].refugees;
            run(BORDER_CLOSED, true);
            float shut = g.econ.state[to].refugees;
            int barred = g.econ.state[from].barredTo;
            run(BORDER_SELECTIVE, true);
            float picky = g.econ.state[to].refugees;
            run(BORDER_SELECTIVE, false);
            float noWork = g.econ.state[to].refugees;
            // (a few find another way out, whatever the border does)
            CHECK(open > 1.0f && shut < 0.05f * open && barred == to && picky > 0.2f * open && picky < 0.4f * open && noWork < 0.05f * open,
                  "famine at %s: with %s's border open %.0f people cross in a few minutes; shut, %.0f; selective, %.0f while it has work for them, and %.0f when it hasn't",
                  base.defs[from].name.c_str(), fs.Name(base.defs[to].ruler).c_str(), open, shut, picky, noWork);
            // A government swamped with arrivals tightens its border, and says so
            g.econ = was;
            int ruler = base.defs[to].ruler;
            g.econ.border[ruler] = BORDER_OPEN;
            for (int i = 0; i < N; i++) if (base.defs[i].ruler == ruler) g.econ.state[i].refugees = (float)(0.2 * g.econ.state[i].pop);
            uint32_t news = g.newsSeq;
            g.clock += 3.0 * MARKET_DAY;
            StepEconomy(g, 0.01);
            bool said = false;
            for (const EconNews &item : g.news) if (item.seq > news && item.faction == ruler && item.text.find("migrants") != std::string::npos) said = true;
            CHECK(g.econ.border[ruler] == BORDER_SELECTIVE && said, "swamped with new arrivals, %s stops taking all comers  (\"%s\")", fs.Name(ruler).c_str(), g.news.empty() ? "" : g.news.back().text.c_str());
            g.econ = was;
        }
    }

    // --- Carrying them: cabins for those who pay, the hold for those who can't ---
    {
        ShipBuild one; one.parts = { MakePart(PartType::Cabins, 1), MakePart(PartType::Cabins, 2) };
        CHECK(SpecOf(one).berths == 12.0 && SpecOf(StarterBuild()).berths == 0.0 && !IsExternal(PartType::Cabins), "passenger cabins: 4 berths at size 1, 8 at size 2; the starter ship has none");
        const SysId H = g.systems.home;
        int a = -1, b = -1;
        for (int i = 0; i < (int)g.loaded.bodies.size(); i++) {
            const Body &body = g.loaded.bodies[i];
            if (body.kind == Kind::Station || body.population <= 0) continue;
            if (a < 0) a = i; else if (b < 0) b = i;
        }
        CHECK(a >= 0 && b >= 0, "two inhabited worlds at home");
        if (a < 0 || b < 0) return;
        const Body &A = g.loaded.bodies[a], &B = g.loaded.bodies[b];
        int ma = g.econ.MarketOf(A.id), mb = g.econ.MarketOf(B.id);
        g.player.credits = 1000.0;
        g.scanSafeUntil = 1e18;
        Contract &fare = Inject(g, ContractKind::Passage, H, A, B, -1, 6.0f, 900.0f, 0.0f);
        uint32_t fareId = fare.id;
        Contract &flight = Inject(g, ContractKind::Refugees, H, A, B, -1, 40.0f, 600.0f, 0.0f);
        uint32_t flightId = flight.id;
        Park(g, a, 0.9); Run(g, 1.2);
        bool barred = !AcceptContract(g, g, fareId) && !ContractBar(g, g, *std::find_if(g.contracts.begin(), g.contracts.end(), [&](const Contract &k) { return k.id == fareId; })).empty();
        g.player.build.parts.push_back(MakePart(PartType::Cabins, 2));
        Refit(g);
        bool taken = AcceptContract(g, g, fareId) && AcceptContract(g, g, flightId);
        double popA = g.econ.state[ma].pop, popB = g.econ.state[mb].pop, hold = g.player.CargoTotal(), mass = g.player.Mass();
        bool aboard = AdvanceContract(g, g, fareId) && AdvanceContract(g, g, flightId);
        // (six fares in the cabins; of forty refugees two get the berths left and thirty-eight ride in the hold)
        CHECK(barred && taken && aboard && g.player.passengers == 8.0 && g.player.steerage == 38.0 && fabs(g.player.CargoTotal() - hold - 38.0 * PERSON_HOLD) < 1e-9 &&
              g.player.Mass() > mass + 19.0 && fabs(popA - g.econ.state[ma].pop - 46.0) < 1e-6,
              "six passengers need berths (none: refused; with cabins for 8, taken); forty refugees fill the two berths left and %.0f t of the hold; %s has 46 fewer people",
              38.0 * PERSON_HOLD, A.name.c_str());
        Park(g, b, 0.9); Run(g, 1.2);
        double credits = g.player.credits;
        bool delivered = AdvanceContract(g, g, fareId) && AdvanceContract(g, g, flightId);
        CHECK(delivered && g.player.passengers == 0.0 && g.player.steerage == 0.0 && fabs(g.player.credits - credits - 1500.0) < 1.0 &&
              g.econ.state[mb].pop > popB + 45.0 && g.econ.state[mb].refugees >= 40.0f && fabs(g.player.CargoTotal() - hold) < 1e-9,
              "landed at %s: 1,500 cr, the hold clear, and 46 more people there", B.name.c_str());

        // The boards carry them: where people are leaving, passage for refugees; and for a price, past a border that's shut
        {
            Economy was = g.econ;
            g.econ.state[ma].leaving = 400.0f; g.econ.state[ma].stuck = 900.0f; g.econ.state[ma].bestTo = mb; g.econ.state[ma].barredTo = mb;
            g.clock += 5.0 * MARKET_DAY;
            g.contracts.clear(); g.boardMade.clear();
            RecordBoard(g, g, A);
            int legal = 0, quiet = 0; float fare = 0.0f, bribe = 0.0f;
            for (const Contract &k : g.contracts) {
                if (k.kind != ContractKind::Refugees || k.fromBody != A.id || k.toBody != B.id) continue;
                if (k.law == NO_FACTION) { legal++; fare = k.pay / k.tonnes; } else { quiet++; bribe = k.pay / k.tonnes; }
            }
            int passages = 0;
            for (const Contract &k : g.contracts) if (k.kind == ContractKind::Passage) passages++;
            CHECK(legal == 1 && quiet == 1 && bribe > 1.5f * fare && fare > 0.0f, "people are leaving %s: its board offers refugees' passage to %s at %.0f cr a head, and a quiet run past the border at %.0f (%d ordinary passages on offer besides)",
                  A.name.c_str(), B.name.c_str(), fare, bribe, passages);
            g.econ = was;
        }

        // Past a shut border: customs may find them
        int law = base.defs[mb].ruler;
        int caught = 0, through = 0;
        for (int tries = 0; tries < 30 && law >= 0; tries++) {
            Contract &run = Inject(g, ContractKind::Refugees, H, A, B, -1, 20.0f, 3000.0f, 0.0f);
            run.law = law; run.against = law;
            uint32_t id = run.id;
            Park(g, a, 0.9); Run(g, 1.2);
            if (!AcceptContract(g, g, id) || !AdvanceContract(g, g, id)) break;
            g.standing.assign(fs.Count(), 0.0f);
            g.player.credits = 5000.0;
            Park(g, b, 0.9); Run(g, 1.2 + 0.1 * tries);
            if (g.jobs.empty()) { caught++; if (g.player.credits >= 5000.0 || g.Standing(law) >= 0.0f || g.player.steerage + g.player.passengers > 0.0) caught = -100; }
            else { through++; AdvanceContract(g, g, id); }
        }
        if (law >= 0) CHECK(caught > 0 && through > 0, "running refugees past a closed border: customs found them %d times in %d (held, a fine, and the government's regard lost); %d runs got through",
                           caught, caught + through, through);
        else CHECK(false, "the home system has a government to run a border past");
    }

    // --- Saved; and over the wire ---
    {
        g.econ.border[1 % (int)g.econ.border.size()] = BORDER_CLOSED;
        g.econ.state[colony].pop *= 1.5; g.econ.state[colony].refugees = 123.0f;
        double pop = g.econ.state[colony].pop;
        bool saved = SaveGame(g, "test_people.txt");
        GameState l; NewGame(l);
        bool loaded = LoadGame(l, "test_people.txt");
        remove("test_people.txt");
        Writer w;
        Player p = g.player; p.passengers = 5.0; p.steerage = 17.0;
        WritePlayer(w, p);
        Reader r(w.buf);
        Player back = ReadPlayer(r);
        CHECK(saved && loaded && l.econ.state[colony].pop == pop && l.econ.state[colony].refugees == 123.0f && l.econ.border[1 % (int)g.econ.border.size()] == BORDER_CLOSED &&
              back.passengers == 5.0 && back.steerage == 17.0, "populations, new arrivals and border policy are saved; who's aboard reaches the client");
    }
}

// ---------------------------------------------------------------------------
// Multiplayer: the wire format and the reliable channel, without sockets
// ---------------------------------------------------------------------------
static void TestProtocol()
{
    // Game data round-trips exactly
    GameState g; NewGame(g);
    g.player.cargo[ORE_PRECIOUS] = 7.25; g.player.credits = 987.5; g.player.ship.vel = { 1.5f, -2.0f, 3.25f };
    InputStep in; in.seq = 77;
    in.controls.throttle = 1; in.controls.yaw = -0.37f; in.controls.hyper = true; in.controls.toggleDampeners = true;
    in.aim.hovered = TargetId::MakeRock(g.systems.home, 0xFEEDFACECAFEull); in.aim.retarget = TargetId::MakeShip(5);
    Writer w;
    WritePlayer(w, g.player);
    WriteInput(w, in);
    Reader r(w.buf);
    Player p = ReadPlayer(r);
    InputStep back = ReadInput(r, 77);
    CHECK(r.ok && r.Left() == 0 && Len(p.ship.pos - g.player.ship.pos) == 0.0 && p.cargo[ORE_PRECIOUS] == 7.25 &&
          p.credits == 987.5 && p.ship.vel.z == 3.25f && p.ship.rot.w == g.player.ship.rot.w && p.ship.anchor == g.player.ship.anchor,
          "player state round-trips exactly");
    CHECK(back.controls.yaw == -0.37f && back.controls.hyper && back.controls.toggleDampeners && !back.controls.fire &&
          back.aim.hovered == in.aim.hovered && back.aim.retarget == in.aim.retarget, "an input step round-trips exactly");
    Reader shortR(w.buf.data(), 10);
    ReadPlayer(shortR);
    CHECK(!shortR.ok, "a truncated packet is caught, not read past its end");
    Writer bad; bad.F32(NAN); bad.F32(1e9f); bad.F32(-50.0f); bad.F32(0); bad.U8(0); bad.U8(0); bad.U8(0);
    Reader badR(bad.buf);
    InputStep sane = ReadInput(badR, 1);
    CHECK(sane.controls.throttle == 0.0f && sane.controls.yaw == 90.0f && sane.controls.pitch == -50.0f, "garbage inputs are clamped");

    // Reliable messages arrive once each, in order, through 30% loss and reordering
    Connection a, b;
    a.token = b.token = 1234;
    const int N = 300;
    for (int i = 0; i < N; i++) { Writer m; m.U32((uint32_t)i); m.Bytes(std::string(i % 50, 'x').data(), i % 50); a.Send(m.buf); }
    std::vector<std::vector<uint8_t>> inFlightAB, inFlightBA;
    std::vector<uint32_t> got;
    uint64_t rng = 42;
    auto chance = [&](double p) { rng = Mix64(rng); return (rng >> 11) * (1.0 / 9007199254740992.0) < p; };
    double now = 0.0;
    for (int frame = 0; frame < 2000 && (int)got.size() < N; frame++, now += 1.0 / 60.0) {
        std::vector<uint8_t> pa = a.BuildPacket({}, now, 0, true), pb = b.BuildPacket({}, now, 0, true);
        if (!chance(0.3)) inFlightAB.push_back(pa);
        if (!chance(0.3)) inFlightBA.push_back(pb);
        if (inFlightAB.size() > 1 && chance(0.5)) std::swap(inFlightAB[0], inFlightAB[1]);   // reorder
        for (auto &pkt : inFlightAB) {
            Reader pr(pkt); PacketHeader h; std::vector<std::vector<uint8_t>> d; std::vector<uint8_t> u;
            if (!ReadHeader(pr, h)) continue;
            b.Receive(h, pr, d, u, now);
            for (auto &m : d) { Reader mr(m); got.push_back(mr.U32()); }
        }
        for (auto &pkt : inFlightBA) {
            Reader pr(pkt); PacketHeader h; std::vector<std::vector<uint8_t>> d; std::vector<uint8_t> u;
            if (ReadHeader(pr, h)) a.Receive(h, pr, d, u, now);
        }
        inFlightAB.clear(); inFlightBA.clear();
    }
    bool inOrder = (int)got.size() == N;
    for (int i = 0; inOrder && i < N; i++) inOrder = got[i] == (uint32_t)i;
    CHECK(inOrder, "%d reliable messages through 30%% loss: %d delivered, in order, no duplicates", N, (int)got.size());
    for (int frame = 0; frame < 120 && a.Unacked() > 0; frame++, now += 1.0 / 60.0) {   // a few clean packets to settle the acks
        std::vector<uint8_t> pb = b.BuildPacket({}, now, 0, true);
        Reader pr(pb); PacketHeader h; std::vector<std::vector<uint8_t>> d; std::vector<uint8_t> u;
        if (ReadHeader(pr, h)) a.Receive(h, pr, d, u, now);
    }
    CHECK(a.Unacked() == 0, "...and the sender stops resending once they're acked");

    // A packet never goes over the size limit, even with a backlog
    Connection big; big.token = 1;
    for (const auto &m : MsgMined([] { MinedMap mm; for (uint64_t i = 1; i <= 2000; i++) mm[i * 7919] = 1.5f; return mm; }())) big.Send(m);
    size_t largest = big.BuildPacket(std::vector<uint8_t>(300, 0), 0.0).size();
    CHECK(largest <= (size_t)MAX_PACKET, "2000 mined rocks queued: first packet is %d bytes (limit %d)", (int)largest, MAX_PACKET);
}

// ---------------------------------------------------------------------------
// Multiplayer: a real server and clients, talking over UDP on this machine
// ---------------------------------------------------------------------------
struct NetTester {
    NetClient                net;
    GameState                g;
    Controls                 c;
    Aim                      aim;
    std::vector<std::string> said;          // every message the UI would have shown
    bool Heard(const std::string &part) const
    {
        for (const std::string &s : said) if (s.find(part) != std::string::npos) return true;
        return false;
    }
};

// Runs everyone for a while at 60 frames / 120 steps a second. 'fly' false: the
// clients stop simulating (so the server can catch up with what they've sent).
// 'pace': real seconds to wait each frame (lets resend timers run).
static void Pump(Server &srv, std::vector<NetTester *> who, double seconds, bool fly = true, double pace = 0.0)
{
    int frames = (int)(seconds * 60.0 + 0.5);
    for (int f = 0; f < frames; f++) {
        for (NetTester *t : who) {
            if (t->net.state == NetClient::State::Idle) continue;
            double now = NetTime();
            t->net.Poll(t->g, now);
            if (t->net.state == NetClient::State::Joined && fly)
                for (int k = 0; k < 2; k++) t->net.Step(t->g, t->c, t->aim, (float)(1.0 / SIM_HZ));
            t->net.Flush(now);
            t->net.UpdateTraffic(t->g, 1.0f / 60.0f);
            for (const GameEvent &e : t->g.events) if (!e.text.empty()) t->said.push_back(e.text);
            t->g.events.clear();
            t->g.minedChanged.clear();
        }
        srv.Tick();
        srv.Tick();
        if (pace > 0.0) NetSleep(pace);
    }
}

static Captain *OnServer(Server &srv, const std::string &name)
{
    for (auto &p : srv.peers) if (p->joined && !p->remove && p->cap.name == name) return &p->cap;
    return nullptr;
}

static const Traffic *Sees(const NetTester &t, const std::string &name)
{
    for (const Traffic &tr : t.g.traffic) if (tr.name == name) return &tr;
    return nullptr;
}

static void TestNetwork()
{
    const uint16_t PORT = 23571;
    const std::string DIR = "test_server_data";
    std::error_code ec;
    std::filesystem::remove_all(DIR, ec);
    CHECK(NetInit(), "networking starts");

    ServerConfig cfg;
    cfg.port = PORT; cfg.dataDir = DIR; cfg.log = false;
    Server srv;
    std::string err;
    CHECK(srv.Start(cfg, err), "server listening on UDP %u %s", PORT, err.c_str());

    // --- Joining ---
    NetTester alice, bob;
    CHECK(alice.net.Connect("127.0.0.1", PORT, "Alice", "", err, "Tachi") && bob.net.Connect("localhost", PORT, "Bob", "", err),
          "two clients connect (%s)", err.c_str());
    Pump(srv, { &alice, &bob }, 0.5);
    CHECK(alice.net.state == NetClient::State::Joined && bob.net.state == NetClient::State::Joined && srv.PlayerCount() == 2 &&
          alice.g.id != bob.g.id && alice.g.seed == srv.world.seed, "both joined: ids %d and %d, same galaxy", alice.g.id, bob.g.id);
    CHECK(alice.Heard("Welcome aboard") && bob.Heard("1 other captain"), "welcome messages: \"%s\"", bob.said.empty() ? "" : bob.said[0].c_str());
    const Traffic *seen = Sees(alice, "Bob");
    double gap = seen ? Len(FromShip(alice.g.systems, alice.g.player.ship, seen->ship.anchor, seen->ship.pos)) : -1.0;
    CHECK(seen && Sees(bob, "Alice") && std::count_if(alice.g.traffic.begin(), alice.g.traffic.end(), [](const Traffic &t) { return !t.npc; }) == 1, "each sees the other (spawned %.0f m apart)", gap * METERS_PER_UNIT);
    Pump(srv, { &alice, &bob }, 0.1);
    CHECK(Sees(bob, "Alice") && Sees(bob, "Alice")->shipName == "Tachi" && Sees(alice, "Bob")->shipName.empty(),
          "Bob sees that Alice's ship is the Tachi; Bob never named his");

    // --- Prediction: the client flies ahead; the server, given the same inputs, lands in the same place ---
    alice.c.throttle = 1; alice.c.yaw = 0.05f; alice.c.roll = 0.2f;
    Pump(srv, { &alice, &bob }, 2.0);
    alice.c = {}; alice.c.allStop = true;
    Pump(srv, { &alice, &bob }, 1.0);
    alice.c = {};
    Pump(srv, { &alice, &bob }, 0.3, false);
    Captain *sa = OnServer(srv, "Alice");
    double err1 = sa ? Len(sa->player.ship.pos - alice.g.player.ship.pos) : 1e9;
    CHECK(sa && err1 < 1e-6 && alice.net.corrections == 0,
          "3 s of throttling up, turning and stopping: server and client agree to %.1e units, %d corrections", err1, alice.net.corrections);
    seen = Sees(bob, "Alice");
    double err2 = seen && sa ? Len(FromShip(srv.world.systems, sa->player.ship, seen->ship.anchor, seen->ship.pos)) : 1e9;
    CHECK(err2 < 1.0, "Bob sees Alice where the server has her (%.2f m off)", err2 * METERS_PER_UNIT);

    // --- Targeting another captain, and a hyper lock onto them ---
    bob.aim.hovered = TargetId::MakeShip(alice.g.id);
    bob.aim.course = bob.aim.hovered;                          // (locked: SPACE on her)
    Pump(srv, { &alice, &bob }, 0.2);
    CHECK(bob.g.flight.selected == bob.aim.hovered && TargetName(bob.g, bob.aim.hovered) == "Alice" &&
          !TargetInfo(bob.g, bob.aim.hovered).empty(), "Bob can target Alice's ship (info panel: %d rows)", (int)TargetInfo(bob.g, bob.aim.hovered).size());
    // The server moves Alice 0.4 AU away: her client gets corrected to match
    sa = OnServer(srv, "Alice");
    sa->player.ship.pos = sa->player.ship.pos + srv.world.systems[srv.world.systems.home].n * (0.4 * AU_U);
    sa->player.ship.vel = { 0, 0, 0 };
    int before = alice.net.corrections;
    Pump(srv, { &alice, &bob }, 0.3);
    double err3 = Len(OnServer(srv, "Alice")->player.ship.pos - alice.g.player.ship.pos);
    CHECK(alice.net.corrections > before && err3 < 1.0, "server moved Alice: her client corrected itself (%d correction(s), %.2f m off)",
          alice.net.corrections - before, err3 * METERS_PER_UNIT);
    bob.c.hyper = true;
    Pump(srv, { &alice, &bob }, 12.0);
    Captain *sb = OnServer(srv, "Bob");
    sa = OnServer(srv, "Alice");
    double apart = Len(FromShip(srv.world.systems, sb->player.ship, sa->player.ship.anchor, sa->player.ship.pos));
    CHECK(apart < 5000 * KM_U, "Bob locked on in hyper and flew to her: %.0f km apart (from 0.4 AU)", apart / KM_U);
    bob.c = {}; bob.aim = {};
    Pump(srv, { &alice, &bob }, 0.5);
    Pump(srv, { &alice, &bob }, 0.3, false);
    CHECK(Len(OnServer(srv, "Bob")->player.ship.pos - bob.g.player.ship.pos) < 1.0,
          "after 12 s of hyper Bob's client and the server agree (%d corrections)", bob.net.corrections);

    // --- Space traffic: flown on the server, seen (and named) on the clients ---
    {
        Pump(srv, { &alice, &bob }, 3.0);
        int ships = 0, named = 0;
        for (const Traffic &t : alice.g.traffic) if (t.npc) { ships++; if (t.name != "Unknown ship" && !t.name.empty()) named++; }
        CHECK(!srv.world.npcs.empty() && ships > 0 && named == ships && alice.g.npcs.empty() && ships <= SNAPSHOT_MAX_SHIPS,
              "the server flies %d NPC ships; Alice's client sees the nearest %d, each by name", (int)srv.world.npcs.size(), ships);
        const Traffic *first = nullptr;
        for (const Traffic &t : alice.g.traffic) if (t.npc) { first = &t; break; }
        if (first) {
            int id = first->id;
            alice.said.clear();
            alice.net.SendCommand({ Command::HailShip, id });
            Pump(srv, { &alice, &bob }, 0.3);
            CHECK(alice.Heard("Hailing ") && TargetName(alice.g, TargetId::MakeShip(id)) == first->name && !TargetInfo(alice.g, TargetId::MakeShip(id)).empty(),
                  "she can pick one out (%s) and hail it", first->name.c_str());
        }
    }

    // --- A captain's ship is the server's to change; the client is told what it is now ---
    {
        Captain *sv = OnServer(srv, "Alice");
        sv->player.build.parts.push_back(MakePart(PartType::Hold, 1));
        Refit(*sv);
        Pump(srv, { &alice, &bob }, 0.5);
        CHECK(alice.g.player.build.parts == sv->player.build.parts && alice.g.player.spec.hold == sv->player.spec.hold && alice.g.player.spec.hold == CARGO_CAPACITY + 25.0 &&
              bob.g.player.spec.hold == CARGO_CAPACITY, "Alice's ship gained a cargo bay on the server: her client knows (%.0f t of hold), and Bob's ship is as it was",
              alice.g.player.spec.hold);
    }

    // --- Trading: a command, applied on the server, results back by snapshot and message ---
    sa = OnServer(srv, "Alice");
    int c = TradeWorld(*sa);
    Park(srv.world, *sa, c, 0.9);
    Market smk = LiveMarket(srv.world, *sa, sa->loaded.bodies[c]);
    int sg = 0;
    for (int gd = 0; gd < GOOD_COUNT; gd++) if (smk.role[gd] == Role::Import) { sg = gd; break; }
    sa->player.cargo[sg] = 10;
    Pump(srv, { &alice, &bob }, 1.5);
    CHECK(alice.g.flight.nearBody == c && alice.g.player.cargo[sg] == 10, "server parked Alice at %s with 10 t of %s",
          alice.g.loaded.bodies[c].name.c_str(), GoodData(sg).name);
    Market onRecord;
    bool known = KnownMarket(alice.g, alice.g.loaded.bodies[c], onRecord);
    CHECK(!alice.g.econ.Built() && known && fabs(onRecord.price[sg] - smk.price[sg]) < 0.02 * smk.price[sg] && onRecord.tariff == smk.tariff &&
          onRecord.target[sg] == (float)smk.target[sg] && smk.target[sg] > 0.0,
          "her client runs no economy: the server sent the market's prices (%s at %.0f cr/t)", GoodData(sg).name, onRecord.price[sg]);
    double cr = alice.g.player.credits;
    alice.net.SendCommand({ Command::SellGoods, sg, 1e30 });
    Pump(srv, { &alice, &bob }, 1.5);
    CHECK(alice.g.player.cargo[sg] == 0 && alice.g.player.credits > cr + 9.0 * smk.SellPrice(sg) && alice.Heard("Sold 10.0 t "),
          "sold it through the server: +%.0f cr", alice.g.player.credits - cr);
    CHECK(!bob.Heard("Sold"), "...and Bob didn't get Alice's message");
    KnownMarket(alice.g, alice.g.loaded.bodies[c], onRecord);
    CHECK(fabs(onRecord.stock[sg] - smk.stock[sg] - 10.0) < 0.5 && bob.g.know.prices.count(alice.g.loaded.bodies[c].id) == 0,
          "her record of the market follows the sale; Bob, who isn't there, has none");
    {
        EconNews item;
        item.at = srv.world.clock; item.where = srv.world.systems.home; item.text = "Test: the harvest is in"; item.seq = ++srv.world.newsSeq;
        srv.world.news.push_back(item);
        Pump(srv, { &alice, &bob }, 0.5);
        CHECK(!alice.g.news.empty() && alice.g.news.back().text == item.text && !bob.g.news.empty() && bob.g.news.back().where == item.where,
              "news reaches everyone");
    }

    // --- Contracts: the server keeps the boards; a contract goes to whoever accepts it first ---
    {
        sa = OnServer(srv, "Alice");
        Captain *sbob = OnServer(srv, "Bob");
        Park(srv.world, *sbob, c, 0.9);
        const Body &port = sa->loaded.bodies[c];
        Pump(srv, { &alice, &bob }, 0.5);
        alice.net.SendCommand({ Command::Hail, c });
        bob.net.SendCommand({ Command::Hail, c });
        Pump(srv, { &alice, &bob }, 2.5);
        alice.net.SendCommand({ Command::Contracts, c });
        Pump(srv, { &alice, &bob }, 3.0);                             // (the board is drawn up the first time someone asks)
        uint32_t id = Inject(srv.world, ContractKind::Supply, srv.world.systems.home, port, port, sg, 5.0f, 400.0f, 0.0f).id;
        alice.net.SendCommand({ Command::Contracts, c });
        bob.net.SendCommand({ Command::Contracts, c });
        Pump(srv, { &alice, &bob }, 3.0);
        auto has = [&](const std::vector<Contract> &list) { return std::any_of(list.begin(), list.end(), [&](const Contract &o) { return o.id == id; }); };
        CHECK(alice.g.boards.count(port.id) && has(alice.g.offers) && has(bob.g.offers) && alice.g.offers.size() >= 3 && alice.g.contracts.empty(),
              "both asked %s for work: the board (%d contracts) reached both clients", port.name.c_str(), (int)alice.g.offers.size());
        alice.net.SendCommand({ Command::Accept, (int32_t)id });
        Pump(srv, { &alice, &bob }, 0.5);
        bob.net.SendCommand({ Command::Accept, (int32_t)id });
        Pump(srv, { &alice, &bob }, 0.5);
        CHECK(has(alice.g.jobs) && !has(alice.g.offers) && bob.g.jobs.empty() && !has(bob.g.offers) && bob.Heard("someone else took it"),
              "Alice accepted first and has it; Bob was told it had gone, and it's off his list");
        OnServer(srv, "Alice")->player.cargo[sg] = 5.0;
        double before = alice.g.player.credits;
        alice.net.SendCommand({ Command::Advance, (int32_t)id });
        Pump(srv, { &alice, &bob }, 0.5);
        CHECK(alice.g.jobs.empty() && alice.g.player.credits == before + 400.0 && alice.Heard("Contract complete"), "...and delivered it through the server: +400 cr");
        alice.net.SendCommand({ Command::HangUp, c });
        bob.net.SendCommand({ Command::HangUp, c });
        Pump(srv, { &alice, &bob }, 0.3);
        alice.g.comms.channels.clear(); bob.g.comms.channels.clear();
    }

    // --- Comms: the log comes back from the server, replies after the light-speed delay ---
    alice.net.SendCommand({ Command::Hail, c });
    Pump(srv, { &alice, &bob }, 3.0);
    const TargetId hailed = TargetId::MakeBody(srv.world.systems.home, c);
    const CommsChannel *chan = alice.g.comms.Find(hailed);
    CHECK(chan && chan->log.size() >= 2 && !CommsWaiting(alice.g, alice.g, hailed),
          "hailed %s: \"%s\"", chan ? chan->withName.c_str() : "?", chan && !chan->log.empty() ? chan->log.back().text.c_str() : "");
    alice.net.SendCommand({ Command::MarketReport, c });
    alice.net.SendCommand({ Command::Hail, TradeWorldOther(*OnServer(srv, "Alice"), c) });
    Pump(srv, { &alice, &bob }, 0.3);
    CHECK(alice.g.comms.channels.size() == 2 && !CommsInFlight(alice.g, alice.g).empty(), "a second channel, and requests in flight, reach her client too");
    alice.net.SendCommand({ Command::HangUp, TradeWorldOther(*OnServer(srv, "Alice"), c) });
    Pump(srv, { &alice, &bob }, 0.5);
    CHECK(alice.g.comms.channels.size() == 1 && alice.g.comms.Find(hailed), "...and so does hanging up on one of them");

    // --- Standing with the factions is the server's to keep ---
    OnServer(srv, "Alice")->standing[3] = 42.0f;
    Pump(srv, { &alice, &bob }, 0.3);
    CHECK((int)alice.g.standing.size() == srv.world.systems.factions.Count() && alice.g.Standing(3) == 42.0f && bob.g.Standing(3) == 0.0f &&
          alice.g.systems.factions.list[3].name == srv.world.systems.factions.list[3].name,
          "the server raised Alice's standing with %s: her client knows (%+.0f), Bob's is untouched", srv.world.systems.factions.list[3].name.c_str(),
          alice.g.Standing(3));

    // --- Mining: everyone hears which rocks are gone ---
    sa = OnServer(srv, "Alice");
    int z = -1;
    for (int i = 0; i < (int)sa->loaded.zones.size(); i++) if (!sa->loaded.zones[i].ring) { z = i; break; }
    sa->player.ship.pos = ZoneTargetPoint(sa->loaded.zones[z], ShipLocal(srv.world, *sa));
    sa->player.ship.vel = { 0, 0, 0 };
    Pump(srv, { &alice, &bob }, 0.3);
    sa = OnServer(srv, "Alice");
    const Asteroid *rock = nullptr;
    DVec3 here = ShipLocal(srv.world, *sa);
    for (auto &cell : sa->field.cells)
        for (const Asteroid &a : cell.second)
            if (!rock || Len(a.local - here) < Len(rock->local - here)) rock = &a;
    uint64_t rockId = rock->id;
    sa->player.ship.pos = rock->local + Norm(here - rock->local) * (AsteroidRadius(*rock) + 100.0);
    Face(srv.world, *sa, rock->local - sa->player.ship.pos);
    Pump(srv, { &alice, &bob }, 0.3);
    alice.aim.hovered = TargetId::MakeRock(srv.world.systems.home, rockId);
    alice.c.fire = true;
    Pump(srv, { &alice, &bob }, 0.5);
    seen = Sees(bob, "Alice");
    CHECK(seen && seen->mining && seen->miningRock == rockId, "Bob sees Alice's mining laser on the rock");
    Pump(srv, { &alice, &bob }, 1.5);
    alice.c = {}; alice.aim = {};
    Pump(srv, { &alice, &bob }, 0.5);
    double mined = alice.g.player.CargoTotal();
    CHECK(mined > 3.5 && srv.world.deltas.mined.count(rockId), "Alice mined %.2f t (the server's count)", mined);
    CHECK(bob.g.deltas.mined.count(rockId) && fabsf(bob.g.deltas.mined[rockId] - srv.world.deltas.mined[rockId]) < 0.01f,
          "Bob's copy of the galaxy knows that rock was mined (%.1f t left)", bob.g.deltas.mined.count(rockId) ? bob.g.deltas.mined[rockId] : -1.0f);

    // --- Leaving and coming back: the server kept Alice's progress ---
    double credits = alice.g.player.credits;
    DVec3  where = alice.g.player.ship.pos;
    alice.net.Disconnect();
    Pump(srv, { &alice, &bob }, 0.3);
    CHECK(srv.PlayerCount() == 1 && bob.Heard("Captain Alice left") && !Sees(bob, "Alice"), "Alice left; Bob was told");
    CHECK(std::filesystem::exists(DIR + "/players/alice.txt"), "her captain was saved to players/alice.txt");
    NetTester alice2;
    alice2.net.Connect("127.0.0.1", PORT, "Alice", "", err);
    Pump(srv, { &alice2, &bob }, 0.5);
    CHECK(alice2.net.state == NetClient::State::Joined && alice2.g.player.credits == credits &&
          fabs(alice2.g.player.CargoTotal() - mined) < 1e-9 && Len(alice2.g.player.ship.pos - where) < 1.0,
          "back with the same name: same credits (%.0f), cargo (%.2f t) and position", alice2.g.player.credits, alice2.g.player.CargoTotal());
    CHECK(alice2.g.deltas.mined.count(rockId) == 1, "a newcomer's galaxy includes rocks mined before they joined");
    CHECK(alice2.g.Standing(3) == 42.0f, "...and her standing came back with her");
    CHECK(bob.Heard("Captain Alice joined"), "Bob was told she's back");

    // --- Turned away ---
    NetTester badName, dupName;
    badName.net.Connect("127.0.0.1", PORT, "Bad/Name!", "", err);
    Pump(srv, { &badName }, 0.3);
    CHECK(badName.net.state == NetClient::State::Rejected, "an invalid name is turned away: \"%s\"", badName.net.message.c_str());

    // --- A lossy connection (25% of packets lost each way) still works ---
    NetTester carol;
    carol.net.simulatedLoss = 0.25f;
    carol.net.Connect("127.0.0.1", PORT, "Carol", "", err);
    Pump(srv, { &alice2, &bob, &carol }, 2.0, true, 0.004);
    bool joined = carol.net.state == NetClient::State::Joined && carol.g.deltas.mined.count(rockId) == 1;
    // (other ships are on her scope only while her sensors hold them: the others are far off by now. Put her beside Bob.)
    bool farOff = !Sees(carol, "Bob") && !Sees(carol, "Alice");
    if (Captain *sc = OnServer(srv, "Carol")) {
        const Captain *sb2 = OnServer(srv, "Bob");
        sc->player.ship.anchor = sb2->player.ship.anchor; sc->player.ship.pos = sb2->player.ship.pos + DVec3{ 300.0, 0.0, 0.0 }; sc->player.ship.vel = { 0, 0, 0 };
    }
    Pump(srv, { &alice2, &bob, &carol }, 1.0, true, 0.004);
    const Traffic *beside = Sees(carol, "Bob");
    (void)farOff;
    CHECK(joined && beside && beside->level == CONTACT_IDENTIFIED && !beside->ghost,
          "Carol joined through 25%% packet loss and has the whole picture: the rocks mined before she came, and (once she's beside him) Bob on her scope");
    carol.c.throttle = 1; carol.c.yaw = -0.1f;
    Pump(srv, { &alice2, &bob, &carol }, 1.5, true, 0.004);
    carol.c = {}; carol.c.allStop = true;
    Pump(srv, { &alice2, &bob, &carol }, 0.5, true, 0.004);
    Pump(srv, { &alice2, &bob, &carol }, 0.5, false, 0.004);
    double err4 = Len(OnServer(srv, "Carol")->player.ship.pos - carol.g.player.ship.pos);
    CHECK(err4 < 1e-6, "...and her flying matches the server exactly (%.1e units, %d corrections)", err4, carol.net.corrections);

    // --- A password-protected server ---
    ServerConfig locked = cfg;
    locked.port = PORT + 1; locked.password = "hunter2"; locked.dataDir = DIR + "_locked";
    Server srv2;
    srv2.Start(locked, err);
    NetTester wrong, right;
    wrong.net.Connect("127.0.0.1", PORT + 1, "Mallory", "guess", err);
    right.net.Connect("127.0.0.1", PORT + 1, "Dave", "hunter2", err);
    Pump(srv2, { &wrong, &right }, 0.3);
    CHECK(wrong.net.state == NetClient::State::Rejected && right.net.state == NetClient::State::Joined,
          "password: wrong one turned away (\"%s\"), right one let in", wrong.net.message.c_str());
    srv2.Stop();

    // --- Shutting down saves the galaxy and tells everyone ---
    srv.Stop();
    Pump(srv, { &bob }, 0.2);
    Server again;
    again.Start(cfg, err);
    CHECK(bob.net.state == NetClient::State::Lost && again.world.deltas.mined.count(rockId) == 1,
          "shutdown: clients told (\"%s\"), mined rocks still there after a restart", bob.net.message.c_str());
    again.Stop();

    alice2.net.Disconnect(); bob.net.Disconnect(); carol.net.Disconnect(); right.net.Disconnect();
    std::filesystem::remove_all(DIR, ec);
    std::filesystem::remove_all(DIR + "_locked", ec);
    NetShutdown();
}

static void TestHosting();                    // (below, with the bot)

int RunHeadlessTests(const char *only)
{
    setvbuf(stdout, nullptr, _IONBF, 0);            // (so that what led up to a crash is there to read)
    struct T { const char *name; void (*fn)(); };
    const T tests[] = {
        { "fuel", TestFuel }, { "flight", TestFlight }, { "trade", TestTrade }, { "stations", TestStations }, { "hz", TestHabitableZone },
        { "skimming", TestSkimming }, { "comms", TestComms }, { "galaxy", TestGalaxy }, { "systems", TestSystems }, { "maps", TestMaps }, { "factions", TestFactions }, { "economy", TestEconomy }, { "contracts", TestContracts }, { "traffic", TestTraffic }, { "power", TestPower }, { "ships", TestShips }, { "sensors", TestSensors }, { "industry", TestIndustry }, { "people", TestPeople }, { "fleets", TestFleets }, { "parts", TestParts }, { "reactors", TestReactors }, { "drives", TestDrives }, { "money", TestMoney }, { "econprobe", TestEconProbe }, { "econyears", TestEconYears }, { "econwho", TestEconWho }, { "fleetprobe", TestFleetProbe }, { "spaceprobe", TestSpaceProbe },
        { "targeting", TestTargeting }, { "mining", TestMining }, { "save", TestSaveLoad },
        { "protocol", TestProtocol }, { "net", TestNetwork }, { "host", TestHosting },
    };
    SetTraceLogLevel(LOG_WARNING);
    for (const T &t : tests) {
        if (only && *only && strcmp(only, "all") && strcmp(only, t.name)) continue;
        if ((!strcmp(t.name, "econprobe") || !strcmp(t.name, "econyears") || !strcmp(t.name, "econwho") || !strcmp(t.name, "fleetprobe") || !strcmp(t.name, "spaceprobe")) && (!only || strcmp(only, t.name))) continue;      // (tuning aids: only when asked for by name)
        printf("[%s]\n", t.name);
        t.fn();
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail;
}

// ---------------------------------------------------------------------------
// Hosting from the game: this executable started again as a server, joined,
// and stopped by its host on the way out
// ---------------------------------------------------------------------------
static bool WaitFor(NetTester &t, NetClient::State want, double seconds)
{
    double end = NetTime() + seconds;
    while (NetTime() < end && t.net.state != want) {
        double now = NetTime();
        t.net.Poll(t.g, now);
        if (t.net.state == NetClient::State::Joined) t.net.Step(t.g, {}, {}, (float)(1.0 / SIM_HZ));
        t.net.Flush(now);
        NetSleep(0.01);
    }
    return t.net.state == want;
}

static void TestHosting()
{
    const std::string DIR = "test_host_data";
    std::error_code ec;
    std::filesystem::remove_all(DIR, ec);
    std::filesystem::create_directories(DIR, ec);
    CHECK(NetInit(), "networking starts");
    const uint16_t PORT = 23581;
    std::string key = RandomKey(), err;
    const std::string password = "a \"tricky\" pass\\";     // spaces, quotes, a trailing backslash: command-line quoting
    ProcessHandle server;
    bool launched = LaunchProcess(SelfPath(), { "--server", "--port", std::to_string(PORT), "--data", DIR, "--admin-key", key,
                                                "--password", password, "--quiet" }, server, err, DIR + "/server.log");
    CHECK(launched && server.Running(), "started this program as a server (%s) %s", SelfPath().c_str(), err.c_str());
    if (!launched) return;

    NetTester host, other;
    host.net.patience = 90.0;                                   // (it has a galaxy to build before it answers)
    host.net.Connect("127.0.0.1", PORT, "Holden", password, err);
    CHECK(WaitFor(host, NetClient::State::Joined, 60.0), "the host joined it, with a password full of quotes and spaces");
    other.net.Connect("127.0.0.1", PORT, "Amos", password, err);
    CHECK(WaitFor(other, NetClient::State::Joined, 5.0), "a second captain joined");
    other.net.adminKey = "not-the-key";
    other.net.stopServerOnLeave = true;
    other.net.Disconnect();
    NetSleep(0.5);
    CHECK(server.Running(), "someone without the admin key can't stop it");
    host.net.adminKey = key;
    host.net.stopServerOnLeave = true;
    host.net.Disconnect();
    double end = NetTime() + 5.0;
    while (server.Running() && NetTime() < end) NetSleep(0.05);
    CHECK(!server.Running() && std::filesystem::exists(DIR + "/players/holden.txt") && std::filesystem::exists(DIR + "/world.txt"),
          "the host left and the server stopped, saving the galaxy and both captains");
    std::filesystem::remove_all(DIR, ec);
    NetShutdown();
}

// ---------------------------------------------------------------------------
// A headless test pilot
// ---------------------------------------------------------------------------
int RunBot(const std::string &host, uint16_t port, const std::string &name, double seconds, bool idle)
{
    SetTraceLogLevel(LOG_WARNING);
    if (!NetInit()) { printf("Couldn't start networking\n"); return 1; }
    NetTester bot;
    std::string err;
    if (!bot.net.Connect(host, port, name, "", err)) { printf("%s\n", err.c_str()); return 1; }
    printf("%s: connecting to %s:%u...\n", name.c_str(), host.c_str(), port);
    double start = NetTime(), next = start, lastReport = start;
    uint32_t steps = 0;
    while (NetTime() - start < seconds) {
        double now = NetTime();
        if (now < next) { NetSleep(fmin(next - now, 0.002)); continue; }
        next += 1.0 / 60.0;
        bot.net.Poll(bot.g, now);
        if (bot.net.state == NetClient::State::Rejected || bot.net.state == NetClient::State::Lost) {
            printf("%s: %s\n", name.c_str(), bot.net.message.c_str());
            return 1;
        }
        if (bot.net.state == NetClient::State::Joined)
            for (int k = 0; k < 2; k++, steps++) {
                // Lazy loops: thrust, turn, roll, now and then stop
                double t = steps / SIM_HZ;
                Controls c;
                c.throttle = fmod(t, 12.0) < 1.5 ? 1.0f : 0.0f;       // up to about half throttle
                c.yaw     = (float)(0.6 * sin(t * 0.7));
                c.pitch   = (float)(0.3 * sin(t * 0.31));
                c.roll    = (float)(0.4 * cos(t * 0.5));
                c.allStop = fmod(t, 12.0) >= 8.0 && fmod(t, 12.0) < 8.0 + 1.0 / SIM_HZ;
                if (idle) c = {};
                bot.net.Step(bot.g, c, {}, (float)(1.0 / SIM_HZ));
            }
        bot.net.Flush(now);
        bot.net.UpdateTraffic(bot.g, 1.0f / 60.0f);
        for (const GameEvent &e : bot.g.events) if (!e.text.empty()) printf("%s: \"%s\"\n", name.c_str(), e.text.c_str());
        bot.g.events.clear();
        bot.g.minedChanged.clear();
        if (now - lastReport > 5.0 && bot.net.state == NetClient::State::Joined) {
            lastReport = now;
            printf("%s: %.0f s, %d captains, ping %d ms, %d corrections, speed %.0f m/s\n", name.c_str(), now - start,
                   bot.net.Captains(), bot.net.pingMs, bot.net.corrections, Vector3Length(bot.g.player.ship.vel) * METERS_PER_UNIT);
            fflush(stdout);
        }
    }
    printf("%s: done - %u steps flown, %d corrections\n", name.c_str(), steps, bot.net.corrections);
    bot.net.Disconnect();
    NetShutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Scripted runs (in the window). Each is a function of the frame number.
// ---------------------------------------------------------------------------
std::unique_ptr<ScriptedInput> MakeScript(const std::string &name)
{
    auto s = std::make_unique<ScriptedInput>();
    if (name == "tour") {
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            const StarSystem &home = g.systems[g.systems.home];
            int f = s.frame;
            int c = TradeWorld(g);
            (void)home;
            if (f == 30)  s.screenshot = "tour_start.png";
            if (f == 40)  in.Press(KEY_I);                                    // info on what's ahead
            if (f == 50)  s.screenshot = "tour_info.png";
            if (f == 60 && c >= 0) { Park(g, c, 0.9); s.snap = true; }
            if (f == 70)  in.Press(KEY_F);                                    // comms: hails the world we're parked over
            if (f == 260) s.screenshot = "tour_comms.png";
            if (f == 270) in.Press(KEY_ONE);
            if (f == 420) s.screenshot = "tour_comms_market.png";
            if (f == 430) in.Press(KEY_THREE);                                // trade, over the channel
            if (f == 440) s.screenshot = "tour_market.png";
            if (f == 450) in.Press(KEY_ESCAPE);
            if (f == 460) in.Press(KEY_F5);
            if (f == 470) {                                                   // into the asteroid belt
                for (const Zone &z : g.loaded.zones)
                    if (!z.ring) { g.player.ship.pos = ZoneTargetPoint(z, ShipLocal(g)); g.player.ship.vel = { 0, 0, 0 }; break; }
                s.snap = true;
            }
            if (f == 520) s.screenshot = "tour_belt.png";
            if (f == 530) { g.player.ship.pos = home.n * (0.7 * LY_U); g.player.ship.vel = { 0, 0, 0 }; s.snap = true; }
            if (f > 535 && f < 700) in.Hold(KEY_LEFT_SHIFT);                 // warp
            if (f == 690) s.screenshot = "tour_warp.png";
            if (f == 720) in.Press(KEY_F9);                                   // back to the save
            if (f == 760) s.screenshot = "tour_loaded.png";
            if (f == 770) s.done = true;
        };
        return s;
    }
    if (name == "stations") {                                                // a look at each kind of station
        s->body = [](ScriptedInput &s, InputFrame &in) {
            (void)in;
            GameState &g = *s.game;
            static const struct { Settlement kind; const char *shot; } kinds[] = {
                { Settlement::OrbitalPort, "station_port.png" }, { Settlement::Starbase, "station_starbase.png" },
                { Settlement::Shipyard, "station_shipyard.png" }, { Settlement::SolarCollector, "station_collector.png" },
                { Settlement::Greenhouse, "station_greenhouse.png" },
                { Settlement::SkimmerStation, "station_skimmer.png" }, { Settlement::OreRefinery, "station_refinery.png" } };
            const int COUNT = (int)(sizeof kinds / sizeof kinds[0]), EACH = 40;
            int k = (s.frame - 20) / EACH, at = (s.frame - 20) % EACH;
            if (s.frame < 20) return;
            if (k >= COUNT) { s.done = true; return; }
            if (at == 0)                                                     // the nearest one of this kind
                for (const StarSystem *sys : Neighbours(g, 25.0)) {
                    LoadedSystem L;
                    GenerateSystemContents(*sys, L);
                    int found = -1;
                    for (int i = 0; i < (int)L.bodies.size() && found < 0; i++)
                        if (L.bodies[i].kind == Kind::Station && L.bodies[i].settlement == kinds[k].kind) found = i;
                    if (found < 0) continue;
                    g.player.ship.anchor = sys->id; g.player.ship.pos = {};
                    UpdateLoadedSystem(g);
                    const Body &b = g.loaded.bodies[found];
                    DVec3 from = Norm(sys->u * 0.7 + sys->v * 0.5 + sys->n * 0.45);
                    g.player.ship.pos = b.local + from * (b.radius * 3.4);
                    g.player.ship.vel = { 0, 0, 0 }; g.player.ship.throttle = 0.0f;
                    Face(g, b.local - g.player.ship.pos);
                    s.snap = true;
                    break;
                }
            if (at == 30) s.screenshot = kinds[k].shot;
        };
        return s;
    }
    if (name == "maps") {                                                    // the system map (charted and not) and the sector map
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 20) in.Press(KEY_M);
            if (f == 24) in.Press(KEY_DOWN), in.Press(KEY_DOWN);
            if (f == 26) in.Press(KEY_DOWN), in.Press(KEY_DOWN);
            if (f == 30) s.screenshot = "map_system.png";
            if (f == 40) in.Press(KEY_M);
            if (f == 50) s.screenshot = "map_sector.png";
            if (f == 60) in.Press(KEY_ESCAPE);
            if (f == 70)                                                     // somewhere without charts
                for (const StarSystem *sys : Neighbours(g, 20.0))
                    if (sys->id != g.systems.home && sys->develop > 0.5f && sys->stars[0].mass > 0.6) {
                        g.player.ship.anchor = sys->id; g.player.ship.pos = sys->n * (0.5 * sys->fieldH) + sys->u * (0.2 * sys->fieldR);
                        g.player.ship.vel = { 0, 0, 0 };
                        s.snap = true;
                        break;
                    }
            if (f == 90)  in.Press(KEY_M);
            if (f == 100) s.screenshot = "map_uncharted.png";
            if (f == 110) in.Press(KEY_ESCAPE);
            if (f == 120) s.screenshot = "map_hud.png";
            if (f == 125) s.done = true;
        };
        return s;
    }
    if (name == "comms") {                                                   // several channels at once, and the HUD's markers
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 20) {
                auto list = CommsInRange(g);
                for (size_t i = 0; i < list.size(); i += std::max<size_t>(1, list.size() / 4)) Hail(g, list[i].body);
                RequestMarketReport(g, list[0].body);
            }
            if (f == 30) in.Press(KEY_C);
            if (f == 60) s.screenshot = "comms_list.png";
            if (f == 70) in.Press(KEY_ESCAPE);
            if (f == 80) { g.clock += 600.0; }
            if (f == 90) s.screenshot = "comms_hud.png";
            if (f == 95) s.done = true;
        };
        return s;
    }
    if (name == "economy") {                                                 // the market, the trade log and the news
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame, c = TradeWorld(g);
            if (f == 10) {                                                    // (a small place that needs robotics: a hold swamps it)
                EnsureEconomy(g);
                double least = 1e30;
                for (int i = 0; i < (int)g.loaded.bodies.size(); i++) {
                    Market mk = LiveMarket(g, g, g.loaded.bodies[i]);
                    if (mk.role[GOOD_ROBOTICS] == Role::Import && mk.target[GOOD_ROBOTICS] < least) { least = mk.target[GOOD_ROBOTICS]; c = i; }
                }
            }
            if (f == 10 && c >= 0) { Park(g, c, 0.9); s.snap = true; g.player.credits = 25000; g.player.cargo[GOOD_ROBOTICS] = 40; g.player.cargo[GOOD_WEAPONS] = 3; }
            if (f == 20)  in.Press(KEY_F);
            if (f == 210) in.Press(KEY_THREE);
            static int down = 0;                                              // down the list to the robotics
            if (f == 212) { Market mk; down = 0; if (g.flight.nearBody >= 0 && KnownMarket(g, g.loaded.bodies[g.flight.nearBody], mk)) { std::vector<int> rows = mk.Listed();
                            for (int i = 0; i < (int)rows.size(); i++) if (rows[i] == GOOD_ROBOTICS) down = i; } }
            if (f >= 216 && f < 216 + down) in.Press(KEY_DOWN);
            if (f == 240) s.screenshot = "econ_market.png";
            if (f == 242) in.Press(KEY_B);                                    // trade data
            if (f == 244) in.Press(KEY_ESCAPE);
            if (f == 246) RunEconomy(g, 9.0);                                 // some days go by
            if (f == 250) in.Press(KEY_T);
            if (f == 260) s.screenshot = "econ_tradelog.png";
            if (f >= 262 && f < 270 && f % 2 == 0) in.Press(KEY_D);
            if (f == 270) in.Press(KEY_TAB);
            if (f == 280) s.screenshot = "econ_tradelog_sell.png";
            if (f == 284) in.Press(KEY_ESCAPE);
            if (f == 290) in.Press(KEY_N);
            if (f == 300) s.screenshot = "econ_news.png";
            if (f == 304) in.Press(KEY_ESCAPE);
            if (f == 310) s.done = true;
        };
        return s;
    }
    if (name == "contracts") {                                               // a station's board, your contracts, a faction's list
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 10)  g.player.credits = 6000;
            if (f == 20)  in.Press(KEY_F);
            if (f == 200) in.Press(KEY_FOUR);
            if (f == 420) s.screenshot = "contracts_board.png";
            if (f == 424 || f == 430 || f == 436) in.Press(KEY_ENTER), in.Press(KEY_DOWN);     // take a few
            if (f == 444) in.Press(KEY_TAB);
            if (f == 452) s.screenshot = "contracts_mine.png";
            if (f == 456) in.Press(KEY_ESCAPE);
            if (f == 460) in.Press(KEY_R);
            if (f >= 464 && f < 470) in.Press(KEY_DOWN);
            if (f == 474) s.screenshot = "contracts_faction.png";
            if (f == 478) in.Press(KEY_C);
            if (f == 490) s.screenshot = "contracts_list.png";
            if (f == 495) s.done = true;
        };
        return s;
    }
    if (name == "traffic") {                                                 // other ships, in the colours of how they look at you
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 60) {                                                    // one of each, in a row: allied, neutral, alerted, hostile
                const StarSystem &sys = g.systems[g.loaded.index];
                const FactionSet &fs = g.systems.factions;
                int clan = NO_FACTION, state = NO_FACTION;
                for (const Faction &fa : fs.list) {
                    if (clan == NO_FACTION && fa.kind == FactionKind::Clan) clan = fa.id;
                    if (state == NO_FACTION && fa.kind == FactionKind::Government) state = fa.id;
                }
                g.player.ship.pos = sys.u * (12.0 * AU_U) + sys.n * (1.0 * AU_U); g.player.ship.vel = {}; g.player.ship.throttle = 0.0f;
                Face(g, sys.v);
                g.standing.assign(fs.Count(), 0.0f);
                g.standing[0] = 60.0f;
                g.npcs.clear();
                DVec3 mid = ShipLocal(g) + sys.v * 26.0 - sys.n * 4.0;
                struct { NpcKind kind; int faction; double x; } row[] = { { NpcKind::Freighter, 0, -15.0 }, { NpcKind::Freighter, NO_FACTION, -5.0 },
                                                                          { NpcKind::Patrol, state, 5.0 }, { NpcKind::Pirate, clan, 15.0 } };
                for (auto &r : row) {
                    Npc &n = PutShip(g, r.kind, g.loaded.index, mid + sys.u * r.x, r.faction);
                    n.rot = QuaternionFromMatrix(MatrixInvert(MatrixLookAt({ 0, 0, 0 }, ToF(Norm(sys.v * -1.0 + sys.n * 0.9)), ToF(sys.u))));
                    n.until = 1e18; n.look = 1e18;
                    if (r.kind == NpcKind::Patrol) { n.doing = NpcDo::Inspect; n.prey = g.id; n.since = g.clock; n.name = fs.Name(state) + " patrol 12"; }
                    if (r.kind == NpcKind::Pirate) { n.doing = NpcDo::Flee; n.name = fs.Name(clan) + " raider 7"; }
                }
                g.trafficZoned = g.clock;
                s.snap = true;
            }
            if (f == 80) s.screenshot = "traffic_colours.png";
            if (f == 90) s.done = true;
            (void)in;
        };
        return s;
    }
    if (name == "sensors") {                                                 // contacts at each level, a ghost, a radar warning, a tachyon bearing
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            static int ghost = -1;
            if (f == 60) {
                const StarSystem &sys = g.systems[g.loaded.index];
                const FactionSet &fs = g.systems.factions;
                int clan = NO_FACTION, state = sys.ruler;
                for (const Faction &fa : fs.list) if (clan == NO_FACTION && fa.kind == FactionKind::Clan) clan = fa.id;
                g.player.ship.pos = RandomPointFarFromPorts(g); g.player.ship.vel = {}; g.player.ship.throttle = 0.0f;
                Face(g, sys.v);
                for (int k = 0; k < GOOD_COUNT; k++) g.player.cargo[k] = 0.0;
                g.scanSafeUntil = 1e18;
                g.npcs.clear(); g.contacts.clear(); g.marks.clear();
                DVec3 here = ShipLocal(g);
                struct { NpcKind kind; int faction; double au, side, up; } row[] = {
                    { NpcKind::Pirate, clan, 1.0, -0.30, 0.10 },                  // an unknown radar contact
                    { NpcKind::Patrol, state, 1.1, -0.05, 0.16 },                 // classified
                    { NpcKind::Patrol, state, 0.15, 0.03, -0.01 },                // identified
                    { NpcKind::Patrol, state, 4.0, 1.3, 0.5 },                    // too far to see, but its radar is heard
                    { NpcKind::Pirate, clan, 0.5, 0.16, 0.08 } };                 // (about to be lost)
                for (auto &r : row) {
                    Npc &n = PutShip(g, r.kind, g.loaded.index, here + sys.v * (r.au * AU_U) + sys.u * (r.side * AU_U) + sys.n * (r.up * AU_U), r.faction);
                    n.until = 1e18; n.look = 1e18; n.doing = r.kind == NpcKind::Pirate ? NpcDo::Lurk : NpcDo::Dock;
                    ghost = n.id;
                }
                g.trafficZoned = g.clock;
                s.snap = true;
            }
            if (f == 90) {                                                    // one slips away; and a ship drops out of warp off to the left
                const StarSystem &sys = g.systems[g.loaded.index];
                for (Npc &n : g.npcs) if (n.id == ghost) { n.pos = n.pos + sys.v * (6.0 * AU_U); n.goal = n.pos; }
                g.bursts.push_back({ g.loaded.index, ShipLocal(g) + sys.v * (2.5 * AU_U) - sys.u * (1.2 * AU_U) - sys.n * (0.5 * AU_U), 4000.0, 424242 });
                g.trafficZoned = g.clock;
            }
            if (f == 200) s.screenshot = "sensors_scope.png";
            if (f == 205) in.Press(KEY_M);
            if (f == 225) s.screenshot = "sensors_map.png";
            if (f == 230) in.Press(KEY_ESCAPE);
            if (f == 235 || f == 240 || f == 245) in.Press(KEY_X);            // round to running silent
            if (f == 300) s.screenshot = "sensors_silent.png";
            if (f == 305) s.done = true;
        };
        return s;
    }
    if (name == "outpost") {                                                 // a sensor outpost and its pickets
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 10) {
                g.trafficZoned = -1e9;
                for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
                    if (g.loaded.bodies[i].settlement == Settlement::SensorOutpost) { Park(g, i, 2.2); s.snap = true; break; }
            }
            if (f == 80) s.screenshot = "outpost.png";
            if (f == 85) s.done = true;
            (void)in;
        };
        return s;
    }
    if (name == "industry") {                                                // a world's economy page
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 10) {
                for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
                    if (g.loaded.bodies[i].settlement == Settlement::CoreWorld) { Park(g, i, 0.9); s.snap = true; break; }
                g.player.credits = 2500000;
            }
            if (f == 20)  in.Press(KEY_F);
            if (f == 200) in.Press(KEY_SIX);
            if (f == 215) s.screenshot = "industry_page.png";
            if (f == 220) in.Press(KEY_TAB);
            if (f == 235) s.screenshot = "industry_unbuilt.png";
            if (f == 240) in.Press(KEY_ESCAPE);
            if (f == 250) in.Press(KEY_ESCAPE);
            if (f == 255) in.Press(KEY_I);
            if (f == 270) s.screenshot = "industry_info.png";
            if (f == 275) s.done = true;
        };
        return s;
    }
    if (name == "ships") {                                                   // fitting out at the yard
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 10) {
                for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
                    if (g.loaded.bodies[i].settlement == Settlement::Shipyard) { Park(g, i, 0.9); s.snap = true; break; }
                g.player.credits = 60000;
            }
            if (f == 20)  in.Press(KEY_F);
            if (f == 200) in.Press(KEY_FIVE);
            if (f == 215) s.screenshot = "ships_reactors.png";
            if (f == 220 || f == 224 || f == 228 || f == 232 || f == 236 || f == 240 || f == 244 || f == 248) in.Press(KEY_D);     // to the cargo bays
            if (f == 252) in.Press(KEY_ENTER);
            if (f == 260) s.screenshot = "ships_bays.png";
            if (f == 264) in.Press(KEY_A);                                    // round to the hulls page
            if (f >= 266 && f < 284 && f % 2 == 0) in.Press(KEY_A);
            if (f == 290) s.screenshot = "ships_hulls.png";
            if (f == 294) in.Press(KEY_ESCAPE);
            if (f == 300) s.screenshot = "ships_hud.png";
            if (f == 305) s.done = true;
        };
        return s;
    }
    if (name == "contraband") {                                              // the warning for carrying what the nearest system's law bans
        s->body = [](ScriptedInput &s, InputFrame &in) {
            GameState &g = *s.game;
            int f = s.frame;
            if (f == 20)
                for (const StarSystem *sys : Neighbours(g, 40.0)) {
                    const Faction *law = g.systems.factions.Get(sys->ruler);
                    if (!law || law->contraband.empty()) continue;
                    g.player.ship.anchor = sys->id; g.player.ship.pos = sys->n * (0.5 * sys->fieldH) + sys->u * (0.3 * sys->fieldR); g.player.ship.vel = { 0, 0, 0 };
                    g.player.cargo[law->contraband[0]] = 12.0;
                    if (law->contraband.size() > 1) g.player.cargo[law->contraband[1]] = 5.0;
                    s.snap = true;
                    break;
                }
            if (f == 40) s.screenshot = "contraband_box.png";
            if (f == 44) in.Press(KEY_TAB);
            if (f == 50) s.screenshot = "contraband_tab.png";
            if (f == 55) s.done = true;
        };
        return s;
    }
    if (name == "factions") {                                                // the factions window, and whose space is whose
        s->body = [](ScriptedInput &s, InputFrame &in) {
            int f = s.frame;
            if (f == 20) in.Press(KEY_R);
            if (f == 24) in.Press(KEY_DOWN);
            if (f == 40) s.screenshot = "factions_government.png";
            if (f >= 44 && f < 52 && f % 2 == 0) in.Press(KEY_DOWN);
            if (f == 54) in.Press(KEY_DOWN), in.Press(KEY_DOWN);
            if (f == 60) s.screenshot = "factions_company.png";
            if (f == 64) in.Press(KEY_UP);
            if (f == 66) in.Press(KEY_ESCAPE);
            if (f == 70) in.Press(KEY_M);
            if (f == 74) in.Press(KEY_M);
            if (f == 76) in.wheel = -1.0f;
            if (f == 78) in.wheel = -1.0f;
            if (f == 90) s.screenshot = "factions_sector.png";
            if (f == 95) s.done = true;
        };
        return s;
    }
    if (name == "menus") {
        s->body = [](ScriptedInput &s, InputFrame &in) {
            int f = s.frame;
            if (f == 20)  in.Press(KEY_ESCAPE);                               // pause
            if (f == 30)  s.screenshot = "menu_pause.png";
            if (f == 35)  in.Press(KEY_DOWN);                                 // Save Game
            if (f == 40)  in.Press(KEY_ENTER);
            if (f == 45)  s.screenshot = "menu_save_default.png";
            if (f >= 50 && f < 62) in.Hold(KEY_BACKSPACE), in.Press(KEY_BACKSPACE);   // clear the suggestion
            if (f == 65)  in.Type("Before the belt");
            if (f == 70)  s.screenshot = "menu_save_typed.png";
            if (f == 75)  in.Press(KEY_ENTER);                                // save -> back to the pause menu
            if (f == 80)  s.screenshot = "menu_saved.png";
            if (f == 85)  in.Press(KEY_DOWN);                                 // Load Game
            if (f == 90)  in.Press(KEY_ENTER);
            if (f == 100) s.screenshot = "menu_load.png";
            if (f == 105) in.Press(KEY_ESCAPE);
            if (f == 110) in.Press(KEY_DOWN);                                 // Settings
            if (f == 115) in.Press(KEY_ENTER);
            if (f == 120) in.Press(KEY_RIGHT);                                // sensitivity +0.01
            if (f == 125) in.Press(KEY_DOWN);
            if (f == 130) in.Press(KEY_ENTER);                                // invert Y on
            if (f == 135) s.screenshot = "menu_settings.png";
            if (f == 140) in.Press(KEY_ENTER);                                // invert Y back off
            if (f == 142) in.Press(KEY_UP);
            if (f == 143) in.Press(KEY_LEFT);                                 // sensitivity back
            if (f == 145) in.Press(KEY_ESCAPE);
            if (f == 150) in.Press(KEY_DOWN);                                 // Main Menu
            if (f == 155) in.Press(KEY_ENTER);
            if (f == 160) s.screenshot = "menu_exit.png";
            if (f == 165) in.Press(KEY_ENTER);                                // Cancel (the default)
            if (f == 170) in.Press(KEY_UP), in.Press(KEY_UP);
            if (f == 172) in.Press(KEY_UP);                                   // Load Game
            if (f == 175) in.Press(KEY_ENTER);
            if (f == 185) in.Press(KEY_ENTER);                                // load the newest save
            if (f == 200) s.screenshot = "menu_loaded.png";
            // The same with the mouse: Esc, click Settings, click Back, click Resume
            auto click = [&](float x, float y) { in.mousePos = { x, y }; in.mouseDelta = { 1, 0 };
                                                 in.mousePressed[MOUSE_BUTTON_LEFT] = in.mouseDown[MOUSE_BUTTON_LEFT] = true; };
            if (f == 210) in.Press(KEY_ESCAPE);
            if (f == 215) click(640, 436);                                    // Settings
            if (f == 220) s.screenshot = "menu_mouse_settings.png";
            if (f == 225) click(640, 570);                                    // Back
            if (f == 230) click(640, 262);                                    // Resume
            if (f == 240) s.screenshot = "menu_resumed.png";
            // Overwrite: save again under the same name, then browse up a folder
            if (f == 250) in.Press(KEY_ESCAPE);
            if (f == 252) in.Press(KEY_DOWN);
            if (f == 254) in.Press(KEY_ENTER);                                // Save Game
            if (f >= 256 && f < 268) in.Press(KEY_BACKSPACE);
            if (f == 270) in.Type("Before the belt");
            if (f == 272) in.Press(KEY_ENTER);                                // -> "exists, Save again"
            if (f == 276) s.screenshot = "menu_overwrite.png";
            if (f == 278) in.Press(KEY_ENTER);                                // overwrite
            if (f == 282) in.Press(KEY_DOWN);
            if (f == 284) in.Press(KEY_ENTER);                                // Load Game
            if (f == 288) in.Press(KEY_BACKSPACE);                            // up a folder
            if (f == 292) s.screenshot = "menu_browse_up.png";
            if (f == 300) s.done = true;
        };
        return s;
    }
    if (name == "mainmenu") {                                                // the main menu, its pages, and back again
        s->startInMenu = true;
        s->body = [](ScriptedInput &s, InputFrame &in) {
            int f = s.frame;
            if (f == 10)  s.screenshot = "mm_main.png";
            if (f == 15)  in.Press(KEY_ENTER);                                // Single Player
            if (f == 20)  s.screenshot = "mm_single.png";
            if (f == 25)  in.Press(KEY_ESCAPE);
            if (f == 30)  in.Press(KEY_DOWN);                                 // Multiplayer
            if (f == 32)  in.Press(KEY_ENTER);
            if (f == 36)  s.screenshot = "mm_multi.png";
            if (f == 40)  in.Press(KEY_ENTER);                                // Host a Server
            if (f == 45)  in.Type("Holden");
            if (f == 50)  s.screenshot = "mm_host.png";
            if (f == 55)  in.Press(KEY_ESCAPE);
            if (f == 58)  in.Press(KEY_DOWN);                                 // Join a Server
            if (f == 60)  in.Press(KEY_ENTER);
            if (f == 64)  in.Type("192.168.1.20");
            if (f == 66)  in.Press(KEY_TAB);
            if (f == 68)  in.Type("Naomi");
            if (f == 72)  s.screenshot = "mm_join.png";
            if (f == 75)  in.Press(KEY_ESCAPE);
            if (f == 78)  in.Press(KEY_ESCAPE);                               // back to the main menu
            if (f == 80)  in.Press(KEY_UP);                                   // Single Player
            if (f == 82)  in.Press(KEY_ENTER);
            if (f == 85)  in.Press(KEY_ENTER);                                // New Game: the form
            if (f == 87)  { for (int k = 0; k < 24; k++) in.Press(KEY_BACKSPACE); }
            if (f == 88)  in.Type("Rocinante");
            if (f == 89)  in.Press(KEY_TAB);
            if (f == 90)  in.Type("tycho station");
            if (f == 92)  s.screenshot = "mm_newgame.png";
            if (f == 94)  in.Press(KEY_ENTER);                                // Launch
            if (f == 100) s.screenshot = "mm_ingame.png";
            if (f == 105) in.Press(KEY_ESCAPE);                               // pause
            if (f == 108) s.screenshot = "mm_pause.png";
            if (f == 110 || f == 112 || f == 114 || f == 116) in.Press(KEY_DOWN);   // Main Menu
            if (f == 118) in.Press(KEY_ENTER);
            if (f == 122) s.screenshot = "mm_confirm.png";
            if (f == 125) in.Press(KEY_LEFT);                                 // the "Main Menu" button
            if (f == 127) in.Press(KEY_ENTER);
            if (f == 140) s.screenshot = "mm_back.png";
            if (f == 145) s.done = true;
        };
        return s;
    }
    return nullptr;
}
