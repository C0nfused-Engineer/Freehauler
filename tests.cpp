// tests.cpp - headless tests of the game rules, and scripted demo runs
#include "tests.h"
#include "client.h"
#include "comms.h"
#include "contracts.h"
#include "economy.h"
#include "npcs.h"
#include "process.h"
#include "save.h"
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
        if (i > 0) s.allStop = s.toggleDampeners = s.cyclePower = false;      // one-shot presses happen once
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
    const double perMJ = 1.0 / (spec.reactorEff * spec.fuelMJ);           // tonnes of fuel a megajoule

    // Sublight: thrust burns fuel; coasting costs only what the reactor idles at
    g.player.ship.anchor = H; g.player.ship.pos = home.n * (0.7 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    Face(g, home.u);
    double f0 = g.player.fuel;
    g.player.ship.throttle = 1.0f;
    Run(g, 5.0);
    double v = Vector3Length(g.player.ship.vel), burned = f0 - g.player.fuel;
    CHECK(v > 0.5 * LIGHT_SEC_U && fabs(burned - 5.0 * (spec.idleMW + spec.engineMW) * perMJ) < 0.002,
          "5 s at full throttle: %.2f c on %.4f t of fuel", v / LIGHT_SEC_U, burned);
    Controls damp; damp.toggleDampeners = true;
    Run(g, 1.0 / SIM_HZ, damp);
    g.player.ship.throttle = 0.0f;
    f0 = g.player.fuel;
    Run(g, 5.0);
    CHECK(fabs(Vector3Length(g.player.ship.vel) - v) < 1e-3 * v && fabs(f0 - g.player.fuel - 5.0 * spec.idleMW * perMJ) < 1e-6,
          "dampeners off, throttle shut: it coasts at %.2f c, and that costs only what the reactor idles at (%.4f t in 5 s)",
          Vector3Length(g.player.ship.vel) / LIGHT_SEC_U, f0 - g.player.fuel);
    Run(g, 1.0 / SIM_HZ, damp);
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
    CHECK(!g.flight.drive.inField && f0 - g.player.fuel > 1.0 && f0 - g.player.fuel < 2.2,
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
    Run(g, 0.2, up);
    double gentle = speed();
    Run(g, 0.8, up);
    CHECK(fabsf(ship.throttle - THROTTLE_RATE) < 0.01f && gentle > 0.0 && gentle < 5.0 && speed() > 20.0 && speed() < 5000.0,
          "a touch of W is a touch of thrust (%.1f m/s after 0.2 s); a second of it, throttle %.0f%%, %.0f m/s", gentle * METERS_PER_UNIT,
          ship.throttle * 100.0f, speed() * METERS_PER_UNIT);
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
    CHECK(ship.throttle == -THROTTLE_REVERSE && Vector3DotProduct(ship.vel, ShipForward(ship)) < -300.0f && speed() < 50000.0,
          "S from a standstill: going astern, gently (%.1f km/s after 5 s)", speed() / KM_U);
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

    Controls hyper; hyper.hyper = true;
    Run(g, 0.5, hyper, aim);
    CHECK(g.flight.lock == aim.hovered, "Shift locks onto the selection");
    Run(g, 0.5, hyper, Aim{});
    CHECK(g.flight.lock == aim.hovered && g.flight.selected == aim.hovered, "lock (and selection) hold when the reticle drifts");
    Controls jerk = hyper; jerk.yaw = 10.0f;
    Run(g, 1.0 / SIM_HZ, jerk, aim);
    CHECK(!g.flight.lock.valid() && g.flight.lockSuppressed, "a deliberate mouse move breaks the lock");
    Run(g, 0.2, hyper, aim);
    CHECK(!g.flight.lock.valid(), "...and it stays broken while Shift is held");
    Run(g, 0.1, {}, Aim{});
    CHECK(!g.flight.lock.valid() && !g.flight.lockSuppressed, "releasing Shift resets");

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
        if (s->develop >= 0.3f && s->ruler == NO_FACTION) unclaimedSettled++;
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
          "%d systems are somebody's; the free port holds only home; the fringe (%d systems) and beyond are nobody's", claimed, fringe);
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
    for (int i = 0; i < N; i++) if (g.econ.state[i].stock[REF_FOOD] < 0.25f * base.defs[i].target[REF_FOOD]) hungry++;
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        double out = 0, use = 0;
        for (int i = 0; i < N; i++) {
            out += base.defs[i].out[gd]; use += base.defs[i].use[gd];
            double p = MarketPrice(g, i, gd);
            if (p > 0.0) { priced++; if (p > 5.9 * GoodData(gd).basePrice || p < 0.31 * GoodData(gd).basePrice) capped++; }
        }
        for (int other = 0; other < GOOD_COUNT; other++)
            for (const Ingredient &in : GoodData(other).recipe) if (in.tonnes > 0.0f && in.good == gd) use += 1.0;
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
        float target = base.defs[home].target[gd];
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
        for (int k = 0; k < GOOD_COUNT; k++) if (base.defs[small].target[k] > 0.0f && base.defs[big].target[k] > 0.0f) { both = k; break; }
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
                if (base.defs[i].out[k] > 0.2f * base.defs[i].scale && GoodData(k).recipe[0].tonnes > 0.0f && base.defs[i].sys != g.systems.home) { m = i; gd = k; break; }
        CHECK(m >= 0, "found a plant: %s makes %s", m >= 0 ? base.defs[m].name.c_str() : "?", m >= 0 ? GoodData(gd).name : "?");
        if (m >= 0) {
            Economy was = g.econ;
            auto made = [&](float inputs) {
                g.econ = was;
                g.econ.state[m].stock[gd] = 0.5f * base.defs[m].target[gd];
                for (const Ingredient &in : GoodData(gd).recipe) if (in.tonnes > 0.0f) g.econ.state[m].stock[in.good] = inputs;
                float before = g.econ.state[m].stock[gd];
                StepEconomy(g, 0.01);
                return g.econ.state[m].stock[gd] - before;
            };
            float starved = made(0.0f), fed = made(1e6f);
            CHECK(fed - starved > 0.5f * base.defs[m].out[gd] * 0.01f, "in 1%% of a day its stock grows %.2f t with its inputs on hand and %.2f t without (what trade brought in)", fed, starved);
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
                if (d.target[k] > 0.0f && d.out[k] == 0.0f && d.use[k] == 0.0f && base.defs[d.hub].target[k] > 0.0f) { m = i; gd = k; break; }
        }
        CHECK(m >= 0, "found a market that neither makes nor uses something it deals in (%s, %s)", m >= 0 ? base.defs[m].name.c_str() : "?", m >= 0 ? GoodData(gd).name : "?");
        if (m >= 0) {
            Economy was = g.econ;
            int hub = base.defs[m].hub;
            g.econ.state[m].stock[gd] = 3.0f * base.defs[m].target[gd];
            g.econ.state[hub].stock[gd] = base.defs[hub].target[gd];
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
        for (int k = 0; k < GOOD_COUNT; k++) if (mk.Buys(k) && d.target[k] > 2000.0f) { gd = k; break; }
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
                if (base.defs[i].ruler == base.defs[banned].ruler && !base.defs[i].banned[gd] && base.defs[i].target[gd] > 0.0f) lawless = i;
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
              h.know.prices[base.defs[home].bodyId].target[REF_FOOD] == base.defs[home].target[REF_FOOD],
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
        int kinds[(int)ContractKind::Count] = {}, bad = 0, guild = 0, gated = 0, relief = 0, boards = 0;
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
        }
        int n = (int)all.offers.size();
        CHECK(bad == 0 && n >= 3 * boards && kinds[0] > 0 && kinds[1] > 0 && kinds[2] > 0 && kinds[3] > 0 && kinds[4] > 0 && guild > 0 && gated > 0,
              "%d boards: %d contracts, all sound - %d haulage, %d supply, %d smuggling, %d survey, %d courier; %d for guild members, %d wanting standing",
              boards, n, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], guild, gated);
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
        for (int k = FIRST_FINISHED; k < GOOD_COUNT; k++) if (base.defs[i].target[k] <= 0.0f) { dest = i; gd = k; break; }
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
            for (int tries = 0; tries < 60 && g.news.size() == news; tries++) Run(g, 0.5);
            const Npc *after = FindNpc(g, fid), *raider = FindNpc(g, pid);
            (void)was; (void)to;
            CHECK(after && fabs(after->total - 0.5f * had) < 0.01f * had && fabs(after->cargo[bulk] - 0.5f * carried) < 0.01f * carried + 0.01f &&
                  g.news.size() == news + 1 && (!raider || raider->doing == NpcDo::Leave),
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
        Run(g, 0.05);
    };
    auto load = [&](double tonnes) { for (int k = 0; k < GOOD_COUNT; k++) p.cargo[k] = 0.0; p.cargo[ORE_IRON] = tonnes; };
    auto timeTo = [&](double v) {
        ship.throttle = 1.0f;
        double t = 0.0;
        while (speed() < v && t < 120.0) { Run(g, 0.05); t += 0.05; }
        return t;
    };
    const double perMJ = 1.0 / (p.spec.reactorEff * p.spec.fuelMJ);

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
    CHECK(fabs(idle - 10.0 * p.spec.idleMW * perMJ) < 1e-6 && fabs(g.flight.load - p.spec.idleMW) < 1e-9,
          "sitting still: the reactor idles at %.0f MW, %.3f t of fuel in 10 s (%.0f%% of %.0f MJ/t)", g.flight.load, idle, p.spec.reactorEff * 100.0, p.spec.fuelMJ);
    ship.throttle = 1.0f;
    Run(g, 2.0);
    CHECK(fabs(g.flight.load - (p.spec.idleMW + p.spec.engineMW)) < 0.01, "at full thrust it draws %.0f MW", g.flight.load);
    ship.throttle = 0.5f;
    Run(g, 0.5);
    CHECK(g.flight.load < p.spec.idleMW + 0.01 * p.spec.engineMW && fabs(ThrottleThrust(0.5f) - 1e-3) < 1e-5,
          "half throttle is a thousandth of the thrust, and of the engines' power (%.3f MW)", g.flight.load);

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
          p.power.driveCap > 0.99 * p.spec.driveCapMJ && g.flight.load < 0.5 * p.spec.reactorMW,
          "hyper in a system: %.0f c takes %.0f MW to hold (the reactor has %.0f); the drive capacitor isn't touched", fullSpeed / LIGHT_SEC_U, fullDraw, p.spec.reactorMW);
    Controls slower = hyper; slower.throttle = -1;
    Run(g, 1.5, slower);
    Run(g, 2.0, hyper);
    double half = ship.ftl;
    CHECK(half < 0.6f && half > 0.4f && fabs(speed() - half * p.spec.hyperSpeed) < 0.01 * speed() &&
          fabs((g.flight.drivePower - p.spec.hyperP0) / (fullDraw - p.spec.hyperP0) - pow(half, 3.0)) < 0.01,
          "S with the drive engaged sets it slower: at %.0f%% speed the field takes %.1f MW", half * 100.0f, g.flight.drivePower);
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
    CHECK(fabs(spent - need) < 0.02 * need && fabs(need - p.spec.jumpMJ * massLight / p.spec.capOut) < 0.01 * need && p.power.warping &&
          top > 0.99 * p.spec.warpSpeed && speed() > 0.99 * p.spec.warpSpeed && !g.flight.driveStarved && p.power.driveCap >= capMid && cap0 / need > 1.5 && cap0 / need < 3.0,
          "empty hold: the jump to warp takes %.0f MJ from the capacitor (%.0f for the bubble, the rest lost getting it out): %.1f jumps to a charge",
          spent, p.spec.jumpMJ * massLight, cap0 / need);
    CHECK(fabs(held - wlaw) < 0.01 * wlaw && held > 0.45 * p.spec.reactorMW, "holding the field at %.1f million c takes %.0f MW of the reactor's %.0f", top / LIGHT_SEC_U / 1e6, held, p.spec.reactorMW);
    load(100.0); open();
    need = JumpCharge(p.spec, p.Mass());
    Run(g, 12.0, hyper);
    CHECK(p.power.warping && speed() > 0.9 * p.spec.warpSpeed && g.flight.drivePower > 0.9 * (p.spec.reactorMW - p.spec.idleMW) && p.spec.driveCapMJ / need > 1.0 && p.spec.driveCapMJ / need < 2.0,
          "full hold: %.0f MJ a jump (%.1f to a charge), and the field takes all but all the reactor has (%.0f MW, for %.0f%% of the drive's speed)",
          need, p.spec.driveCapMJ / need, g.flight.drivePower, 100.0 * speed() / p.spec.warpSpeed);
    // Charging: what goes in is less than what the reactor sends
    open();
    p.power.driveCap = 0.0; p.power.bank = p.spec.bankMJ;
    Run(g, 2.0);
    CHECK(fabs(p.power.driveCap - 2.0 * (p.spec.reactorMW - p.spec.idleMW) * p.spec.capIn) < 2.0,
          "an empty capacitor takes all the reactor can spare, and keeps %.0f%% of it (%.0f MJ in 2 s)", p.spec.capIn * 100.0, p.power.driveCap);
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
    p.spec.reactorMW = 60.0;
    Run(g, 1.0, hyper);
    double gaining = speed();
    Run(g, 11.0, hyper);
    double limit = LIGHT_SEC_U * pow((p.spec.reactorMW - p.spec.idleMW - p.spec.warpP0) / (p.spec.warpK * p.Mass()), 1.0 / p.spec.warpN);
    CHECK(g.flight.driveStarved && p.power.warping && fabs(speed() - limit) < 0.01 * limit && speed() < 0.7 * p.spec.warpSpeed &&
          fabs(gaining - p.spec.warpAccel * LIGHT_SEC_U) < 0.05 * gaining,
          "the same load on a 60 MW reactor: at warp, but held to %.0f%% of the drive's speed", 100.0 * speed() / p.spec.warpSpeed);
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
          fabs(travelDrive / p.spec.capIn + travelBank - 2.0 * (p.spec.reactorMW - p.spec.idleMW)) < 5.0,
          "X cycles the power mode: TRAVEL sends three quarters of the spare output to the drive capacitor, COMBAT to the bank");
    Run(g, 1.0 / SIM_HZ, cycle); Run(g, 1.0 / SIM_HZ, cycle);
    CHECK(p.power.mode == POWER_TRAVEL, "...and round again to TRAVEL");

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
    return market >= 0 && g.econ.base->defs[market].target[good] > 0.0f;
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
    CHECK(fabs(s.dryMass - d.dryMass) < 0.01 && s.thrust == d.thrust && s.turn == d.turn && s.reactorMW == d.reactorMW && s.reactorEff == d.reactorEff &&
          s.fuelGood == REF_REACTOR_FUEL && d.fuelGood == REF_REACTOR_FUEL && fabs(s.idleMW - d.idleMW) < 1e-9 && s.engineMW == d.engineMW && s.bankMJ == d.bankMJ && s.driveCapMJ == d.driveCapMJ &&
          s.hyperSpeed == d.hyperSpeed && s.hyperP0 == d.hyperP0 && s.hyperMaxMass == d.hyperMaxMass && s.warpSpeed == d.warpSpeed && s.warpP0 == d.warpP0 &&
          s.jumpMJ == d.jumpMJ && s.tank == FUEL_CAPACITY && s.hold == CARGO_CAPACITY && s.laserRate == d.laserRate && s.scoopRate == d.scoopRate &&
          s.skimRate == d.skimRate && s.scan == d.scan,
          "the starter ship: a %s with %d parts, %.1f t dry, %.0f MW, %.0f t of fuel, %.0f t of cargo", Hull(p.build.hull).name, (int)p.build.parts.size(),
          s.dryMass, s.reactorMW, s.tank, s.hold);
    CHECK(fabs(VolumeUsed(p.build) - 330.0) < 0.01 && BuildVolume(p.build) == 400.0 && HardpointsUsed(p.build) == 4 && BuildHardpoints(p.build) == 6,
          "it uses %.0f of its %.0f m3 and %d of its %d hardpoints", VolumeUsed(p.build), BuildVolume(p.build), HardpointsUsed(p.build), BuildHardpoints(p.build));
    // Size doubles a part; grade makes it lighter, better and much dearer
    {
        PartInfo a = Describe(MakePart(PartType::Reactor, 2)), b2 = Describe(MakePart(PartType::Reactor, 3)), c3 = Describe(MakePart(PartType::Reactor, 2, 3));
        ShipBuild one; one.parts = { MakePart(PartType::Reactor, 2, 3) };
        ShipSpec elite = SpecOf(one);
        CHECK(b2.mass == 2.0 * a.mass && b2.volume == 2.0 * a.volume && b2.cost == 2.0 * a.cost && c3.mass < 0.75 * a.mass && c3.volume == a.volume &&
              c3.cost == 15.0 * a.cost && elite.reactorMW > 1.4 * s.reactorMW && elite.reactorEff > s.reactorEff + 0.1,
              "a size 3 reactor is twice a size 2; an elite one is %.0f%% lighter, gives %.0f MW at %.0f%%, and costs 15 times as much",
              100.0 * (1.0 - c3.mass / a.mass), elite.reactorMW, elite.reactorEff * 100.0);
    }

    // Away from a yard nothing can be done
    CHECK(!Shipyard(g) && !FitPart(g, g, MakePart(PartType::Hold, 1)) && p.build.parts.size() == 13, "fitting out needs a shipyard or a starbase");
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
              fabs(VolumeUsed(p.build) - 360.0) < 0.01 && (!base_target(g, market, GOOD_HULL_PLATING) || fabs(plating - g.econ.state[market].stock[GOOD_HULL_PLATING] - info.tonnes) < 1e-3),
              "fitted a %s for %.0f cr: 125 t of hold now, %.1f t heavier, %.0f m3 used", info.name.c_str(), price, info.mass, VolumeUsed(p.build));
        // No room for a big one; no hardpoint for a third thing outside
        bool big = FitPart(g, g, MakePart(PartType::Hold, 4));
        bool laser = FitPart(g, g, MakePart(PartType::MiningLaser, 1)), scoop = FitPart(g, g, MakePart(PartType::Scoop, 1));
        bool another = FitPart(g, g, MakePart(PartType::MiningLaser, 1));
        CHECK(!big && laser && scoop && !another && p.spec.laserRate == 4.0 && p.spec.scoopRate == 1.0 && HardpointsUsed(p.build) == 6,
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
              BuildHardpoints(p.build) == 7 && p.spec.dryMass > dry && first > 0.2 * Hull(p.build.hull).cost,
              "stretched %d times for %.0f m3 (the first cost %.0f cr, the second %.0f), and a seventh hardpoint welded on; then no more",
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
        auto perMJ = [](const ShipSpec &r) { return GoodData(r.fuelGood).basePrice / r.fuelMJ; };
        CHECK(fis.fuelGood == REF_REACTOR_FUEL && deu.fuelGood == REF_FUEL && he3.fuelGood == RAW_HELIUM3 && std::string(GoodData(REF_FUEL).name) == "Deuterium" &&
              fis.reactorMW < deu.reactorMW && deu.reactorMW < he3.reactorMW && fis.fuelMJ < deu.fuelMJ && deu.fuelMJ < he3.fuelMJ && costF < costD && costD < costH &&
              perMJ(he3) < perMJ(deu) && perMJ(deu) < perMJ(fis),
              "size 2 reactors: fission %.0f MW on %.0f MJ/t (%.0f cr), deuterium fusion %.0f MW on %.0f MJ/t (%.0f cr), helium-3 fusor %.0f MW on %.0f MJ/t (%.0f cr)",
              fis.reactorMW, fis.fuelMJ, costF, deu.reactorMW, deu.fuelMJ, costD, he3.reactorMW, he3.fuelMJ, costH);
        const GoodInfo &d2 = GoodData(REF_FUEL);
        CHECK(d2.recipe[0].good == REF_WATER && d2.recipe[0].tonnes >= 3.0f && d2.recipe[1].good == REF_CHEMICALS && GoodData(REF_REACTOR_FUEL).recipe[0].good == ORE_URANIUM,
              "deuterium is made from water, %.0f t of it and a little of the exchange towers' chemicals to the tonne; reactor fuel from uranium", d2.recipe[0].tonnes);

        int reactor = -1;
        for (int i = 0; i < (int)p.build.parts.size(); i++) if (p.build.parts[i].type == PartType::Reactor) reactor = i;
        bool mixed = FitPart(g, g, MakePart(PartType::Reactor, 1, 0, BURNS_DEUTERIUM));
        double perTonne = p.spec.reactorEff * p.spec.fuelMJ;
        p.fuel = 0.0;
        RemovePart(g, g, reactor);
        bool dead = p.spec.reactorMW == 0.0;
        bool fusion = FitPart(g, g, MakePart(PartType::Reactor, 2, 0, BURNS_DEUTERIUM));
        CHECK(!mixed && dead && fusion && p.spec.fuelGood == REF_FUEL && fabs(p.spec.reactorEff * p.spec.fuelMJ / perTonne - 3.2) < 1e-9 && p.spec.reactorMW == 260.0,
              "reactors aboard must share a fuel; swapped the fission reactor for a deuterium one: %.0f MW, and a tonne of %s goes 3.2 times as far",
              p.spec.reactorMW, GoodData(p.spec.fuelGood).name);
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
        bool tooGood = best < PART_GRADES - 1 && FitPart(g, g, MakePart(PartType::Scanner, 1, PART_GRADES - 1));
        int company = NO_FACTION;
        for (const Faction &f : fs.list) if (f.kind == FactionKind::Conglomerate && Specialises(f.slant, PartType::Reactor) && company == NO_FACTION) company = f.id;
        Body theirs = *yard;
        theirs.owner = company;
        int stranger = YardGrade(g, g, theirs, PartType::Reactor), other = YardGrade(g, g, theirs, PartType::Scanner);
        g.standing[company] = 30.0f;
        int known = YardGrade(g, g, theirs, PartType::Reactor);
        CHECK(best >= 1 && !tooGood && company != NO_FACTION && stranger == 2 && known == 3 && other == 2 && YardMaker(g, theirs, PartType::Reactor) == company &&
              YardMaker(g, theirs, PartType::Scanner) == NO_FACTION,
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
              DrivePower(bulk, crew, full, false) < bulk.reactorMW - bulk.idleMW && raider.hyperSpeed > 0.0 && raider.dryMass < 0.5 * small.dryMass + 30.0,
              "a 3,000 t freighter: %.0f t dry, a %.0f MW reactor, a drive good for %.0f t; a raider: %.0f t dry", bulk.dryMass, bulk.reactorMW, bulk.hyperMaxMass, raider.dryMass);
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

    // Tachyons: a warp bubble's collapse can be picked up, the nearer and the better the scanner the likelier
    {
        GameState h; NewGame(h);
        Run(h, 0.5);
        const StarSystem &home = h.systems[h.systems.home];
        h.player.ship.pos = home.u * (20.0 * AU_U) + home.n * (0.5 * AU_U); h.player.ship.vel = { 0, 0, 0 };
        Run(h, 0.5);
        auto heard = [&](double au, int tries) {
            int n = 0;
            for (int i = 0; i < tries; i++) {
                h.events.clear();
                h.bursts.push_back({ h.systems.home, ShipLocal(h) + home.u * (au * AU_U), 3000.0, 424242 });
                Run(h, 2.0 / SIM_HZ);
                for (const GameEvent &e : h.events) if (e.text.find("Tachyon burst") != std::string::npos) n++;
            }
            return n;
        };
        int near = heard(1.0, 40), mid = heard(6.0, 200), far = heard(120.0, 40);
        h.player.spec.scan = 4.0;
        int better = heard(6.0, 200);
        CHECK(near == 40 && far == 0 && mid > 40 && mid < 160 && better > mid + 20,
              "a ship dropping out of warp 1 AU off is always noticed, 120 AU off never; at 6 AU, %d times in 200 with the basic scanner and %d with a far better one",
              mid, better);
        // Your own arrival is noticed too: a raider that picks it up comes to look
        h.player.spec = ShipSpec();
        h.npcs.clear();
        h.trafficZoned = h.clock;
        int clan = NO_FACTION;
        for (const Faction &f : h.systems.factions.list) if (f.kind == FactionKind::Clan) { clan = f.id; break; }
        DVec3 here = ShipLocal(h);
        Npc &raider = PutShip(h, NpcKind::Pirate, h.systems.home, here + home.v * (2.0 * AU_U), clan);
        int rid = raider.id;
        h.bursts.push_back({ h.systems.home, here, 3000.0, h.id });
        for (int k = 0; k < GOOD_COUNT; k++) h.player.cargo[k] = 0.0;
        Run(h, 0.1);
        const Npc *r = FindNpc(h, rid);
        CHECK(r && Len(r->goal - here) < 0.02 * AU_U, "a raider 2 AU away picks up the captain's own arrival and makes for the spot");
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
    CHECK(carol.net.state == NetClient::State::Joined && carol.g.deltas.mined.count(rockId) == 1 && Sees(carol, "Bob") && Sees(carol, "Alice"),
          "Carol joined through 25%% packet loss and has the whole picture");
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
    struct T { const char *name; void (*fn)(); };
    const T tests[] = {
        { "fuel", TestFuel }, { "flight", TestFlight }, { "trade", TestTrade }, { "stations", TestStations }, { "hz", TestHabitableZone },
        { "skimming", TestSkimming }, { "comms", TestComms }, { "galaxy", TestGalaxy }, { "systems", TestSystems }, { "maps", TestMaps }, { "factions", TestFactions }, { "economy", TestEconomy }, { "contracts", TestContracts }, { "traffic", TestTraffic }, { "power", TestPower }, { "ships", TestShips },
        { "targeting", TestTargeting }, { "mining", TestMining }, { "save", TestSaveLoad },
        { "protocol", TestProtocol }, { "net", TestNetwork }, { "host", TestHosting },
    };
    SetTraceLogLevel(LOG_WARNING);
    for (const T &t : tests) {
        if (only && *only && strcmp(only, "all") && strcmp(only, t.name)) continue;
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
    host.net.Connect("127.0.0.1", PORT, "Holden", password, err);
    CHECK(WaitFor(host, NetClient::State::Joined, 8.0), "the host joined it, with a password full of quotes and spaces");
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
            if (f == 40)  in.mousePressed[MOUSE_BUTTON_MIDDLE] = true;        // info on what's ahead
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
