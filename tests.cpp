// tests.cpp - headless tests of the game rules, and scripted demo runs
#include "tests.h"
#include "client.h"
#include "comms.h"
#include "economy.h"
#include "process.h"
#include "save.h"
#include "server.h"
#include "settings.h"
#include "targetinfo.h"
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

// The first inhabited planet in the loaded system (a world you can trade with)
static int TradeWorld(const Captain &c)
{
    for (int i = 0; i < (int)c.loaded.bodies.size(); i++)
        if (c.loaded.bodies[i].kind == Kind::Planet && c.loaded.bodies[i].population > 0) return i;
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
    Face(w, c, b.local - c.player.ship.pos);
}
static void Park(GameState &g, int body, double altRadii) { Park(g, g, body, altRadii); }

// Runs the simulation for a while with fixed controls
static void Run(GameState &g, double seconds, Controls c = {}, Aim aim = {})
{
    int steps = (int)(seconds * SIM_HZ + 0.5);
    for (int i = 0; i < steps; i++) {
        Controls s = c;
        if (i > 0) s.allStop = s.toggleDampeners = false;      // one-shot presses happen once
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
    const StarSystem &home = g.systems[0];
    int c = BodyIndex(g, home.name + " c"), d = BodyIndex(g, home.name + " d");

    Park(g, c, 0.5); Face(g, home.u);
    double f0 = g.player.fuel;
    Controls thrust; thrust.forward = 1;
    Run(g, 10.0, thrust);
    CHECK(fabs((f0 - g.player.fuel) - 0.10) < 0.002, "cruise thrust 10 s burns 0.10 t (got %.4f)", f0 - g.player.fuel);

    Controls damp; damp.toggleDampeners = true;
    Run(g, 1.0 / SIM_HZ, damp);
    f0 = g.player.fuel;
    Run(g, 5.0);
    CHECK(f0 - g.player.fuel == 0.0, "coasting with dampeners off is free (got %.4f)", f0 - g.player.fuel);

    // Hyper from c toward d
    const Body &bc = g.loaded.bodies[c], &bd = g.loaded.bodies[d];
    g.player.ship.pos = bc.local + Norm(bd.local - bc.local) * (bc.radius + 2000 * KM_U);
    g.player.ship.vel = { 0, 0, 0 };
    Face(g, bd.local - bc.local);
    f0 = g.player.fuel;
    Controls hyper; hyper.hyper = true;
    Aim toward; toward.hovered = TargetId::MakeBody(0, d);     // locked on, so we stop there
    Run(g, 25.0, hyper, toward);
    double toD = Len(FromShip(g.systems, g.player.ship, 0, bd.local)) - bd.radius;
    CHECK(f0 - g.player.fuel > 0.2 && f0 - g.player.fuel < 1.5, "hyper c -> d (%.2f AU, locked on) burns %.3f t", Len(bd.local - bc.local) / AU_U, f0 - g.player.fuel);
    CHECK(toD < 0.001 * AU_U, "hyper arrives at d (%.0f km from its surface)", toD / KM_U);

    // Warp, out of the field
    g.player.ship.anchor = 0; g.player.ship.pos = home.n * (1.0 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    Face(g, home.u);
    g.player.fuel = 50; f0 = 50;
    Run(g, 10.0, hyper);
    CHECK(!g.flight.drive.inField && f0 - g.player.fuel > 1.9 && f0 - g.player.fuel < 2.5,
          "10 s of warp burns %.3f t (speed %.3f ly/s)", f0 - g.player.fuel, Vector3Length(g.player.ship.vel) / LY_U);

    // Empty tank: limp
    g.player.ship.pos = home.n * (1.0 * LY_U); g.player.ship.vel = { 0, 0, 0 };
    g.player.fuel = 0;
    Run(g, 5.0, hyper);
    CHECK(g.flight.drive.noFuel && fabs(Vector3Length(g.player.ship.vel) - LIMP_SPEED) < LIMP_SPEED * 0.01,
          "empty tank limps at %.4f AU/s", Vector3Length(g.player.ship.vel) / AU_U);
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

    Market mk = MakeMarket(g.loaded.bodies[c]);
    g.player.cargo[ORE_IRON] = 30; g.player.cargo[ORE_PLATINUM] = 2;
    double cr = g.player.credits;
    SellGoods(g, ORE_IRON, 1.0);
    CHECK(fabs(g.player.credits - cr - mk.SellPrice(ORE_IRON)) < 1e-6, "sold 1 t iron for %.0f cr", g.player.credits - cr);
    SellGoods(g, ORE_IRON, 1e30);
    CHECK(g.player.cargo[ORE_IRON] == 0.0, "sell all empties the iron");
    g.player.fuel = 12; g.player.credits = 10000; cr = g.player.credits;
    BuyFuel(g, 1e30);
    CHECK(fabs(g.player.fuel - 50) < 1e-9 && fabs(cr - g.player.credits - 38 * mk.fuelPrice) < 1e-6,
          "filled 38 t of fuel for %.0f cr", cr - g.player.credits);
    bool sold = SellGoods(g, ORE_NICKEL, 1.0);
    CHECK(!sold && !g.events.empty() && g.events.back().type == EventType::Notice, "selling ore you don't have fails with a message");

    // Buying: an export, cheap; then something it doesn't sell
    int ex = -1, notSold = -1;
    for (int gd = 0; gd < GOOD_COUNT; gd++) {
        if (ex < 0 && mk.role[gd] == Role::Export) ex = gd;
        if (notSold < 0 && !mk.Sells(gd)) notSold = gd;
    }
    CHECK(ex >= 0, "%s exports %s", g.loaded.bodies[c].name.c_str(), ExportsLine(mk).c_str());
    if (ex >= 0) {
        g.player.credits = 100000; cr = g.player.credits;
        bool bought = BuyGoods(g, ex, 2.0);
        CHECK(bought && fabs(g.player.cargo[ex] - 2.0) < 1e-9 && fabs(cr - g.player.credits - 2.0 * mk.BuyPrice(ex)) < 1e-6 &&
              mk.BuyPrice(ex) < GoodData(ex).basePrice,
              "bought 2 t %s at %.0f cr/t (base %.0f)", GoodData(ex).name, mk.BuyPrice(ex), GoodData(ex).basePrice);
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
    for (int si = 0; si < (int)g.systems.size(); si++) {
        LoadedSystem L;
        GenerateSystemContents(g.systems[si], L);
        for (const Body &o : L.bodies)
            if (o.kind == Kind::Station) {
                stations++;
                (L.bodies[o.parent].kind == Kind::Star ? free : inOrbit)++;
            }
    }
    CHECK(stations >= (int)g.systems.size() && inOrbit > 0 && free > 0,
          "%d stations across %d systems (%d orbiting worlds, %d on their own)", stations, (int)g.systems.size(), inOrbit, free);
    if (!b) return;
    Market mk = MakeMarket(*b);
    int ex = -1;
    for (int gd = 0; gd < GOOD_COUNT; gd++) if (ex < 0 && mk.role[gd] == Role::Export) ex = gd;
    g.player.cargo[ORE_IRON] = 5;
    double cr = g.player.credits;
    CHECK(SellGoods(g, ORE_IRON, 1e30) && g.player.credits > cr, "%s buys iron (+%.0f cr)", b->name.c_str(), g.player.credits - cr);
    CHECK(ex >= 0 && BuyGoods(g, ex, 1.0), "...and sells %s", ex >= 0 ? GoodData(ex).name : "nothing");
}

// The habitable zone: oceans and complex life only inside it
static void TestHabitableZone()
{
    GameState g; NewGame(g);
    int liquidIn = 0, liquidOut = 0, lifeOut = 0;
    for (int si = 0; si < (int)g.systems.size(); si++) {
        LoadedSystem L;
        GenerateSystemContents(g.systems[si], L);
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
    for (int si = 0; si < (int)g.systems.size() && giant < 0; si++) {
        g.player.ship.anchor = si; g.player.ship.pos = {};          // fly to its star: that system loads
        UpdateLoadedSystem(g);
        for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
            if (g.loaded.bodies[i].kind == Kind::Planet && g.loaded.bodies[i].radius >= SKIM_MIN_RADIUS) { giant = i; break; }
    }
    CHECK(giant >= 0, "found a gas giant to skim (%s)", giant >= 0 ? g.loaded.bodies[giant].name.c_str() : "none");
    if (giant < 0) return;
    Park(g, giant, 0.15); Run(g, 2.0);
    CHECK(g.flight.skimming && fabs(g.player.cargo[ORE_HELIUM3] - 2.0 * SKIM_RATE) < 0.01,
          "2 s in the upper atmosphere: %.2f t helium-3", g.player.cargo[ORE_HELIUM3]);
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
    const StarSystem &home = g.systems[0];
    int c = TradeWorld(g);
    if (c < 0) { CHECK(false, "no inhabited planet to hail"); return; }
    const Body &b = g.loaded.bodies[c];
    g.player.ship.pos = b.local + Norm(home.u) * (b.radius + 200000 * KM_U);
    g.player.ship.vel = { 0, 0, 0 };
    Run(g, 0.05);
    auto list = CommsInRange(g);
    const CommsContact *cc = FindContact(list, TargetId::MakeBody(0, c));
    CHECK(cc, "%s is in comms range (%.2f ls)", b.name.c_str(), cc ? cc->dist / LIGHT_SEC_U : 0.0);
    if (!cc) return;
    double rt = 2.0 * cc->dist / LIGHT_SEC_U;
    Hail(g, c);
    CHECK(g.comms.with == TargetId::MakeBody(0, c) && CommsWaiting(g), "hailing: reply on its way");
    Run(g, rt + 0.5);
    CHECK(CommsWaiting(g), "no reply before the round trip (%.2f s) + 0.6 s", rt);
    Run(g, 0.2);
    CHECK(!CommsWaiting(g), "reply arrived: \"%s\"", g.comms.log.back().text.c_str());
    RequestMarketReport(g);
    size_t n = g.comms.log.size();
    RequestLocalInfo(g);
    CHECK(g.comms.log.size() == n, "only one request at a time");
    g.events.clear();
    g.player.ship.pos = b.local + Norm(home.u) * (b.radius + 1000000 * KM_U);
    Run(g, 1.0 / SIM_HZ);
    bool lost = false;
    for (auto &e : g.events) if (e.type == EventType::CommsLost) lost = true;
    CHECK(lost && !g.comms.with.valid(), "flying out of range drops the channel");
}

static void TestTargeting()
{
    GameState g; NewGame(g);
    const StarSystem &home = g.systems[0];
    int c = BodyIndex(g, home.name + " c"), d = BodyIndex(g, home.name + " d");
    Park(g, c, 2.0);
    Aim aim; aim.hovered = TargetId::MakeBody(0, d);
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
    TargetId star = CanonicalTarget(g, TargetId::MakeStar(0));
    CHECK(star == TargetId::MakeBody(0, 0), "a loaded system's star is its first body");
    DVec3 rel; double r;
    CHECK(FindTarget(g, TargetId::MakeStar(3), rel, &r) && TargetName(g, TargetId::MakeStar(3)) == g.systems[3].name,
          "distant star %s found at %.2f ly", g.systems[3].name.c_str(), Len(rel) / LY_U);
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
    Aim aim; aim.hovered = TargetId::MakeRock(0, id);
    Controls fire; fire.fire = true;
    Run(g, 2.0, fire, aim);
    double got = g.player.CargoTotal();
    CHECK(got > 3.9 && got < 4.1, "2 s of mining yields %.2f t of %s", got, GoodData(FindAsteroid(g.field, id) ? FindAsteroid(g.field, id)->ore : 0).name);
    CHECK(g.deltas.mined.count(id) == 1, "the rock remembers it was mined");
}

static void TestSaveLoad()
{
    GameState g; NewGame(g);
    int c = BodyIndex(g, g.systems[0].name + " c");
    Park(g, c, 0.9);
    Run(g, 1.0);
    g.player.credits = 1234.5; g.player.fuel = 17.25; g.player.cargo[ORE_PLATINUM] = 3.5;
    g.player.ship.dampeners = false;
    g.deltas.mined[0xDEADBEEFull] = 12.5f;
    CHECK(SaveGame(g, "test_save.txt"), "saved");
    GameState h; NewGame(h);
    CHECK(LoadGame(h, "test_save.txt"), "loaded");
    CHECK(h.player.credits == g.player.credits && h.player.fuel == g.player.fuel &&
          h.player.cargo[ORE_PLATINUM] == 3.5 && !h.player.ship.dampeners, "player state round-trips");
    CHECK(Len(h.player.ship.pos - g.player.ship.pos) < 1e-6 && h.player.ship.anchor == g.player.ship.anchor,
          "ship position round-trips exactly");
    CHECK(h.deltas.mined.count(0xDEADBEEFull) && h.deltas.mined[0xDEADBEEFull] == 12.5f, "mined rocks round-trip");
    CHECK(h.loaded.index == 0 && h.clock == g.clock, "system reloaded around the ship, clock restored");
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
    CHECK(sum.valid && sum.name == "unit test save" && sum.location == g.systems[0].name && sum.credits == 1234.5 &&
          fabs(sum.cargo - 3.5) < 1e-9 && sum.seed == g.seed, "summary: \"%s\" at %s, %.0f cr", sum.name.c_str(), sum.location.c_str(), sum.credits);
    CHECK(!ReadSaveSummary(SettingsPathForTests().c_str()).valid, "a file that isn't a save is recognized");
    remove(path.c_str());
    remove("not_a_save.txt");
}

// ---------------------------------------------------------------------------
// Multiplayer: the wire format and the reliable channel, without sockets
// ---------------------------------------------------------------------------
static void TestProtocol()
{
    // Game data round-trips exactly
    GameState g; NewGame(g);
    g.player.cargo[ORE_PLATINUM] = 7.25; g.player.credits = 987.5; g.player.ship.vel = { 1.5f, -2.0f, 3.25f };
    InputStep in; in.seq = 77;
    in.controls.forward = 1; in.controls.yaw = -0.37f; in.controls.hyper = true; in.controls.toggleDampeners = true;
    in.aim.hovered = TargetId::MakeRock(0, 0xFEEDFACECAFEull); in.aim.retarget = TargetId::MakeShip(5);
    Writer w;
    WritePlayer(w, g.player);
    WriteInput(w, in);
    Reader r(w.buf);
    Player p = ReadPlayer(r);
    InputStep back = ReadInput(r, 77);
    CHECK(r.ok && r.Left() == 0 && Len(p.ship.pos - g.player.ship.pos) == 0.0 && p.cargo[ORE_PLATINUM] == 7.25 &&
          p.credits == 987.5 && p.ship.vel.z == 3.25f && p.ship.rot.w == g.player.ship.rot.w, "player state round-trips exactly");
    CHECK(back.controls.yaw == -0.37f && back.controls.hyper && back.controls.toggleDampeners && !back.controls.fire &&
          back.aim.hovered == in.aim.hovered && back.aim.retarget == in.aim.retarget, "an input step round-trips exactly");
    Reader shortR(w.buf.data(), 10);
    ReadPlayer(shortR);
    CHECK(!shortR.ok, "a truncated packet is caught, not read past its end");
    Writer bad; bad.F32(NAN); bad.F32(50.0f); bad.F32(1e9f); bad.F32(0); bad.F32(0); bad.U8(0); bad.U8(0); bad.U8(0);
    Reader badR(bad.buf);
    InputStep sane = ReadInput(badR, 1);
    CHECK(sane.controls.forward == 0.0f && sane.controls.strafe == 1.0f && sane.controls.yaw == 90.0f, "garbage inputs are clamped");

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
    CHECK(alice.net.Connect("127.0.0.1", PORT, "Alice", "", err) && bob.net.Connect("localhost", PORT, "Bob", "", err),
          "two clients connect (%s)", err.c_str());
    Pump(srv, { &alice, &bob }, 0.5);
    CHECK(alice.net.state == NetClient::State::Joined && bob.net.state == NetClient::State::Joined && srv.PlayerCount() == 2 &&
          alice.g.id != bob.g.id && alice.g.seed == srv.world.seed, "both joined: ids %d and %d, same galaxy", alice.g.id, bob.g.id);
    CHECK(alice.Heard("Welcome aboard") && bob.Heard("1 other captain"), "welcome messages: \"%s\"", bob.said.empty() ? "" : bob.said[0].c_str());
    const Traffic *seen = Sees(alice, "Bob");
    double gap = seen ? Len(FromShip(alice.g.systems, alice.g.player.ship, seen->ship.anchor, seen->ship.pos)) : -1.0;
    CHECK(seen && Sees(bob, "Alice") && alice.g.traffic.size() == 1, "each sees the other (spawned %.0f m apart)", gap * METERS_PER_UNIT);

    // --- Prediction: the client flies ahead; the server, given the same inputs, lands in the same place ---
    alice.c.forward = 1; alice.c.yaw = 0.05f; alice.c.roll = 0.2f;
    Pump(srv, { &alice, &bob }, 2.0);
    alice.c = {};
    Pump(srv, { &alice, &bob }, 1.0);
    Pump(srv, { &alice, &bob }, 0.3, false);
    Captain *sa = OnServer(srv, "Alice");
    double err1 = sa ? Len(sa->player.ship.pos - alice.g.player.ship.pos) : 1e9;
    CHECK(sa && err1 < 1e-6 && alice.net.corrections == 0,
          "3 s of thrusting and turning: server and client agree to %.1e units, %d corrections", err1, alice.net.corrections);
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
    sa->player.ship.pos = sa->player.ship.pos + srv.world.systems[0].n * (0.4 * AU_U);
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

    // --- Trading: a command, applied on the server, results back by snapshot and message ---
    sa = OnServer(srv, "Alice");
    int c = TradeWorld(*sa);
    Park(srv.world, *sa, c, 0.9);
    sa->player.cargo[ORE_IRON] = 10;
    Pump(srv, { &alice, &bob }, 0.5);
    CHECK(alice.g.flight.nearBody == c && alice.g.player.cargo[ORE_IRON] == 10, "server parked Alice at %s with 10 t of iron",
          alice.g.loaded.bodies[c].name.c_str());
    double cr = alice.g.player.credits;
    alice.net.SendCommand({ Command::SellGoods, ORE_IRON, 1e30 });
    Pump(srv, { &alice, &bob }, 0.5);
    CHECK(alice.g.player.cargo[ORE_IRON] == 0 && alice.g.player.credits > cr + 250 && alice.Heard("Sold 10.0 t Iron"),
          "sold the iron through the server: +%.0f cr", alice.g.player.credits - cr);
    CHECK(!bob.Heard("Sold"), "...and Bob didn't get Alice's message");

    // --- Comms: the log comes back from the server, replies after the light-speed delay ---
    alice.net.SendCommand({ Command::Hail, c });
    Pump(srv, { &alice, &bob }, 3.0);
    CHECK(alice.g.comms.with == TargetId::MakeBody(0, c) && alice.g.comms.log.size() >= 2 && !CommsWaiting(alice.g),
          "hailed %s: \"%s\"", alice.g.comms.withName.c_str(), alice.g.comms.log.empty() ? "" : alice.g.comms.log.back().text.c_str());

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
    alice.aim.hovered = TargetId::MakeRock(0, rockId);
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
    carol.c.forward = 1; carol.c.yaw = -0.1f;
    Pump(srv, { &alice2, &bob, &carol }, 1.5, true, 0.004);
    carol.c = {};
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
        { "fuel", TestFuel }, { "trade", TestTrade }, { "stations", TestStations }, { "hz", TestHabitableZone },
        { "skimming", TestSkimming }, { "comms", TestComms },
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
                c.forward = fmod(t, 12.0) < 8.0 ? 1.0f : 0.0f;
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
            const StarSystem &home = g.systems[0];
            int f = s.frame;
            int c = TradeWorld(g);
            (void)home;
            if (f == 30)  s.screenshot = "tour_start.png";
            if (f == 40)  in.mousePressed[MOUSE_BUTTON_MIDDLE] = true;        // info on what's ahead
            if (f == 50)  s.screenshot = "tour_info.png";
            if (f == 60 && c >= 0) { Park(g, c, 0.9); s.snap = true; }
            if (f == 70)  in.Press(KEY_C);                                    // comms
            if (f == 80)  in.Press(KEY_ENTER);                                // hail
            if (f == 260) s.screenshot = "tour_comms.png";
            if (f == 270) in.Press(KEY_ONE);
            if (f == 420) s.screenshot = "tour_comms_market.png";
            if (f == 430) in.Press(KEY_F);                                    // market (closes comms)
            if (f == 440) s.screenshot = "tour_market.png";
            if (f == 450) in.Press(KEY_ESCAPE);
            if (f == 460) in.Press(KEY_F5);
            if (f == 470) {                                                   // into the asteroid belt
                for (const Zone &z : g.loaded.zones)
                    if (!z.ring) { g.player.ship.pos = ZoneTargetPoint(z, ShipLocal(g)); g.player.ship.vel = { 0, 0, 0 }; break; }
                s.snap = true;
            }
            if (f == 520) s.screenshot = "tour_belt.png";
            if (f == 530) { g.player.ship.pos = home.n * (1.0 * LY_U); g.player.ship.vel = { 0, 0, 0 }; s.snap = true; }
            if (f > 535 && f < 700) in.Hold(KEY_LEFT_SHIFT);                 // warp
            if (f == 690) s.screenshot = "tour_warp.png";
            if (f == 720) in.Press(KEY_F9);                                   // back to the save
            if (f == 760) s.screenshot = "tour_loaded.png";
            if (f == 770) s.done = true;
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
            if (f == 85)  in.Press(KEY_ENTER);                                // New Game
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
