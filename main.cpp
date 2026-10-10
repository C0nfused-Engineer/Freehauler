// Freehauler - a retro vector-graphics space captain game (raylib)
//
// Build: see the Makefile (or build_windows.bat). Two programs:
//   freehauler          the game: main menu, single player, multiplayer (host or join)
//   freehauler_server   the dedicated server on its own (no window, no raylib; for a VPS)
//
// Run:
//   freehauler                  the main menu
//   freehauler --connect HOST[:PORT] --name NAME [--password PW]
//                                skip the menu and join a server (default port 23500)
//   freehauler --server [options]
//                                be a dedicated server in this console (what Multiplayer >
//                                Host starts in a window of its own; options: server_console.cpp)
//   freehauler --test [name]    run the headless tests (fuel, flight, trade, stations, hz, skimming, comms,
//                                galaxy, systems, maps, factions, targeting, mining, save, protocol, net, host)
//   freehauler --script tour    scripted run in the window that saves screenshots
//                                (also: stations - a look at each kind of station; menus; mainmenu)
//   freehauler --bot HOST[:PORT] [--name N] [--seconds S] [--idle]
//                                a headless test pilot that flies loops on a server
//
// Code map (each layer only uses the ones above it):
//   core.h          units, DVec3, hashing / random numbers
//   format          numbers -> HUD text
//   goods.h         ores (later: the commodity table)
//   factions        the states, conglomerates, guilds and pirate clans of human space, their
//                   characters and agendas - generated from the seed
//   galaxy          the galaxy (full size, made a sector at a time), its stars, systems, worlds,
//                   belts and rings - all generated from the seed
//   asteroids       rocks streamed in around the ship
//   ship            flight model and drives (Ship + Controls: shared with future NPCs)
//   targets.h       TargetId: stable references to things
//   game            World (shared), Captain (one player), GameState (World + your Captain),
//                   StepCaptain (fixed time step), events, target queries
//   econ.h, economy the markets of human space: stock, production, trade, prices; what the
//                   factions do to them; trading (actions on a Captain)
//   comms           hailing worlds and stations (actions on a Captain)
//   commands        player actions as data: applied here, or sent to the server
//   targetinfo      info-panel facts
//   save            save games (seed + deltas + player)
//   input           InputFrame from the keyboard or a script
//   render, hud     3D scene; 2D overlay and readouts (read GameState, never change it)
//   settings        player options (settings.txt)
//   map             the system map and the sector map (M)
//   ui              window stack (info, market, comms), hotkeys, messages
//   menus           main menu (single player, multiplayer host / join, settings) and
//                   pause menu: save, load (file browser), settings, main menu, exit
//   net             UDP sockets (Winsock on Windows) - no raylib, so no windows.h clashes
//   protocol        packets: reliable messages, input steps, snapshots
//   server          the dedicated server; server_console runs it as a console program
//                   (freehauler --server, or freehauler_server via server_main.cpp)
//   process         starting the server from the game, in its own window
//   client          joining a server: prediction, reconciliation, smoothing other ships
//   tests           headless tests and scripted runs
//   main            the loop
//
// Controls
//   Mouse        aim (pitch / yaw)    Q / E      roll left / right
//   W / S        throttle up / down   A / D      yaw left / right
//   Shift (hold) hyper (in a system) / warp (between systems)
//   W + S        all stop (throttle to zero)     Caps Lock  toggle inertial dampeners
//   F            comms: hail whoever the nose is on (or you're docked beside), else list
//                everyone in the system (C: the list)      O   toggle orbit rings
//   R            the factions: who they are, what they want, where you stand
//   T            the trade log: prices at every market you know of      N   the news
//   M            map: the system you're in (list its worlds, pick one to set course); M again,
//                or M between the stars, for the sector map of nearby systems
//   MMB or I     info on the target   3 (on a channel, in docking range)   trade
//   LMB (hold)   mining laser         F5 / F9    quick save / quick load (single player)
//   Esc          close the top window, or pause (save, load, settings, main menu, exit)
//
// Multiplayer (main menu > Multiplayer)
//   Host a Server starts a dedicated server in its own console window and joins
//   it; others join with your address. Join a Server connects to one.
//   Other captains show as colored ships with name tags at any distance; put
//   the reticle on one and hold Shift to lock on and fly to them. The server
//   runs the world (Esc doesn't pause it) and keeps your progress under your
//   name; join again with the same name to carry on.
//
// Navigation
//   Planets and stars show as small colored dots. Put the reticle near one to
//   see its name, distance, and arrival time at your current velocity.
//   Whatever is under the reticle is the selection; steer away and it's
//   deselected. Hold Shift with a selection and the ship locks onto it: the
//   nose tracks it and small mouse movements are ignored. A deliberate move
//   breaks the lock; while locked, a small nudge moves a faint cursor - rest
//   it on another body to switch the lock. Systems load within 0.5 ly.
//
// Drives
//   Cruise  (no Shift)  flown like an aircraft: the throttle sets a speed, from a crawl
//           for docking and mining up to 0.99 c, and the ship goes where its nose points
//           (dampeners kill sideways drift; with them off it keeps drifting).
//   Hyper   (Shift, inside a system's gravity field)  up to 0.7 AU/s. Holding Shift zeroes
//           the throttle, so the ship comes to a stop when you let go.
//   Warp    (Shift, outside every gravity field)      up to 0.35 ly/s.
//   Leave a system's flattened gravity field by climbing out of its plane
//   (short) or flying past the outermost planets (long). Gravity wells slow
//   you near bodies ahead, and warp near systems ahead. Hyper straight at a
//   body and the drive stops by itself short of it: half a radius above a
//   world (in trading range), 1.5 radii above a star (in scooping range),
//   3 km off a station.
//   Fuel burn climbs exponentially from cruise to hyper to warp. Out of fuel,
//   warp is offline and hyper limps at 0.02 AU/s; scoop fuel within 3 radii
//   of a star or buy it.
//
// Mining, trading, comms
//   Belts and rings fill with rocks up close; target one within 1.5 km and
//   hold LMB. What a rock holds depends on its belt: metallic, stony,
//   carbonaceous, mixed, or (rings) ices. Dip below a quarter radius above a
//   gas giant's cloud tops (not in hyper) to skim helium-3.
//   Everything with people starts on the comms. F hails the world or station
//   the nose is on (or the one you're docked beside); anyone in the system can
//   be raised, but a reply takes the light-speed round trip - seconds from
//   close by, hours from the far side of a big system. On a channel: 1 asks
//   for prices, 2 for local information, and within 2 radii of a world or
//   20 km of a station, 3 opens trade. Every economy exports some goods cheap
//   and imports others dear; haul from one to the other. In the market, W/S
//   select, D buys, A sells (Shift: as much as you can).
//
// Scale: 1 world unit = 5 m. Planets, stars and distances are realistic.

#include "client.h"
#include "game.h"
#include "hud.h"
#include "input.h"
#include "process.h"
#include "render.h"
#include "server_console.h"
#include "tests.h"
#include "ui.h"
#include "rlgl.h"
#include "theme.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// Rendering happens between simulation steps: blend the last two ship states
static Ship Interpolate(const Ship &prev, const Ship &cur, float alpha)
{
    if (prev.anchor != cur.anchor) return cur;
    Ship s = cur;
    s.pos = prev.pos + (cur.pos - prev.pos) * alpha;
    s.vel = Vector3Lerp(prev.vel, cur.vel, alpha);
    s.rot = QuaternionSlerp(prev.rot, cur.rot, alpha);
    return s;
}

static void SetCursorShown(bool shown, ScriptedInput *script)
{
    if (script) return;
    if (shown) EnableCursor(); else DisableCursor();
}

// A centered message screen: "CONNECTING...", or why something failed
static void DrawMessageScreen(const char *title, const std::string &sub, const char *hint, Color titleColor)
{
    BeginDrawing();
    ClearBackground(BLACK);
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawText(title, sw / 2 - MeasureText(title, 30) / 2, sh / 2 - 40, 30, titleColor);
    DrawText(sub.c_str(), sw / 2 - MeasureText(sub.c_str(), 20) / 2, sh / 2 + 10, 20, WHITE);
    if (hint) DrawText(hint, sw / 2 - MeasureText(hint, 16) / 2, sh / 2 + 44, 16, HUD_DIM);
    EndDrawing();
}

static void ShowError(const char *title, const std::string &why)
{
    while (!WindowShouldClose()) {
        if (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
        DrawMessageScreen(title, why, "Press any key", ORANGE);
    }
}

// Multiplayer: "Connecting..." until the server welcomes us (true), or turns us
// away / doesn't answer (false, after showing why). 'server': the server we just
// started, if we're hosting (if it quits, we needn't wait for the timeout).
static bool WaitForWelcome(NetClient &net, GameState &g, const ProcessHandle *server)
{
    double started = NetTime();
    while (!WindowShouldClose()) {
        double now = NetTime();
        net.Poll(g, now);
        net.Flush(now);
        if (net.state == NetClient::State::Joined) return true;
        if (server && net.state == NetClient::State::Connecting && now - started > 0.5 && !server->Running()) {
            net.state = NetClient::State::Lost;
            net.message = "Your server stopped right away - is another one already using that port?";
        }
        if (net.state != NetClient::State::Connecting) { ShowError("COULDN'T JOIN", net.message); return false; }
        if (IsKeyPressed(KEY_ESCAPE)) return false;
        std::string dots((size_t)((int)(now * 2.0) % 4), '.');
        DrawMessageScreen(server ? TextFormat("STARTING YOUR SERVER%s", dots.c_str())
                                 : TextFormat("CONNECTING TO %s%s", net.host.c_str(), dots.c_str()),
                          "", "ESC to cancel", HUD);
    }
    return false;
}

// ---------------------------------------------------------------------------
// The main menu, over a slowly turning view of the home system
// ---------------------------------------------------------------------------
static MenuChoice RunMainMenu(InputSource &input, ScriptedInput *script, const RenderAssets &assets, GameState &g)
{
    Ui ui;
    LoadSettings(ui.settings);
    ui.mainMenu = true;
    ui.Open(MakeMainMenu());
    SetCursorShown(true, script);

    GameState backdrop;
    NewGame(backdrop);
    if (script) script->game = &g;
    ViewState view;
    view.showOrbits = true;
    ResetView(view, backdrop.player.ship);
    Quaternion start = backdrop.player.ship.rot;
    double t = 0.0;

    while (!WindowShouldClose() && !(script && script->done)) {
        float frameDt = script ? 1.0f / 60.0f : fminf(GetFrameTime(), 0.25f);
        InputFrame in = input.Poll();
        ui.HandleInput(in, g);
        if (ui.gameReloaded) ui.choice.kind = MenuChoice::Play;      // a save was loaded into g
        if (ui.quit) return MenuChoice{};
        if (ui.choice.kind != MenuChoice::None) return ui.choice;
        ui.Update(g, frameDt);
        g.events.clear();

        t += frameDt;
        Ship eye = backdrop.player.ship;
        eye.rot = QuaternionMultiply(start, QuaternionFromAxisAngle({ 0, 1, 0 }, (float)(t * 0.04)));
        UpdateView(view, eye, frameDt);
        BeginDrawing();
        ClearBackground(BLACK);
        SceneInfo scene;
        DrawScene(backdrop, eye, view, assets, frameDt, scene);
        ui.Draw(g);
        EndDrawing();
        if (script && !script->screenshot.empty()) { TakeScreenshot(script->screenshot.c_str()); script->screenshot.clear(); }
    }
    return MenuChoice{};
}

// ---------------------------------------------------------------------------
// Playing: single player, or online through 'net'. Returns true to go back to
// the main menu, false to quit the game.
// ---------------------------------------------------------------------------
static bool PlaySession(InputSource &input, ScriptedInput *script, const RenderAssets &assets, GameState &g,
                        NetClient *net = nullptr, const std::string &statusLine = "", bool hosting = false)
{
    Ui ui;
    LoadSettings(ui.settings);
    if (script) script->game = &g;
    if (net) {
        ui.online = true;
        ui.hosting = hosting;
        ui.stopServerOnLeave = net->stopServerOnLeave;
        ui.sendCommand = [net](const Command &c) { net->SendCommand(c); };
    }
    SetCursorShown(false, script);

    ViewState view;
    ResetView(view, g.player.ship);
    HudState hud;
    Aim      aim;

    const float STEP = (float)(1.0 / SIM_HZ);
    double   acc = 0.0;                       // real time not yet simulated
    Ship     prev = g.player.ship;            // ship before the latest step (for interpolation)
    Controls pending;                         // mouse rotation and key presses not yet given to a step
    bool throttleCut = false;                 // W and S were pressed together and aren't both up yet
    bool     cursorShown = false;             // menus show the mouse pointer; flight hides and captures it
    int      ignoreMouse = 2;                 // frames to ignore mouse motion after the pointer mode changes

    while (!WindowShouldClose() && !ui.quit && !ui.toMainMenu && !(script && script->done)) {
        float frameDt = script ? 1.0f / 60.0f : fminf(GetFrameTime(), 0.25f);

        // --- Network: what the server said (may correct our ship, or end the session) ---
        if (net) {
            net->Poll(g, NetTime());
            if (net->joinedThisFrame) { prev = g.player.ship; acc = 0.0; pending = {}; aim = {}; ResetView(view, prev); }
        }
        bool offline = net && net->state != NetClient::State::Joined;   // turned away or lost: nothing more to simulate

        // --- Input: windows and hotkeys first, then flight controls ---
        InputFrame in = input.Poll();
        if (ignoreMouse > 0) { in.mouseDelta = { 0, 0 }; ignoreMouse--; }
        if (offline) {
            ui.CloseAll();                    // nothing left to do in the menus or the market
            if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_ENTER)) ui.toMainMenu = true;
            in.ConsumeAll();
            in.down.reset();
            in.mouseDown.reset();
        } else {
            ui.HandleInput(in, g);
        }
        if (ui.gameReloaded || (script && script->snap)) {
            prev = g.player.ship; acc = 0.0; pending = {}; aim = {};
            if (ui.gameReloaded) ResetView(view, prev);
            if (script) script->snap = false;
        }
        if (!script && ui.WantsCursor() != cursorShown) {
            cursorShown = ui.WantsCursor();
            SetCursorShown(cursorShown, script);
            ignoreMouse = 2;                  // re-capturing the mouse reports a big jump
        }
        bool paused = ui.GamePaused() || offline;
        Controls c = ui.FlightPaused() ? Controls{} : ReadFlightControls(in, frameDt, ui.settings, throttleCut);
        // A course from the map: the nose swings onto it until it's there or the pilot steers
        DVec3 courseRel;
        if (ui.course.valid() && !FindTarget(g, ui.course, courseRel)) ui.SetCourse({});
        if (ui.steer && (c.yaw != 0.0f || c.pitch != 0.0f ||
                         Vector3DotProduct(ShipForward(g.player.ship), ToF(Norm(courseRel))) > 0.99999f)) ui.steer = false;
        aim.course = ui.course;
        aim.steer  = ui.steer;
        pending.yaw += c.yaw; pending.pitch += c.pitch; pending.roll += c.roll;
        pending.allStop |= c.allStop; pending.toggleDampeners |= c.toggleDampeners;

        // --- Simulation: fixed steps; this frame's rotation is spread across them ---
        if (paused) { acc = 0.0; pending = {}; prev = g.player.ship; }
        acc += paused ? 0.0f : frameDt;
        int steps = (int)(acc * SIM_HZ + 1e-6);
        acc -= steps * (double)STEP;
        for (int k = 0; k < steps; k++) {
            Controls s = c;
            s.yaw   = pending.yaw / steps;
            s.pitch = pending.pitch / steps;
            s.roll  = pending.roll / steps;
            s.allStop         = k == 0 && pending.allStop;
            s.toggleDampeners = k == 0 && pending.toggleDampeners;
            prev = g.player.ship;
            if (net) net->Step(g, s, aim, STEP);       // predicted here, confirmed by the server
            else     StepGame(g, s, aim, STEP);
        }
        if (steps > 0) pending = {};
        if (net && !offline) net->Flush(NetTime());
        if (net) net->UpdateTraffic(g, offline ? 0.0f : frameDt);
        UpdateKnowledge(g, g);
        ui.Update(g, frameDt);
        g.events.clear();
        g.minedChanged.clear();

        // --- Draw ---
        Ship eye = Interpolate(prev, g.player.ship, (float)(acc * SIM_HZ));
        view.showOrbits = ui.settings.showOrbits;
        UpdateView(view, eye, paused ? 0.0f : frameDt);
        BeginDrawing();
        ClearBackground(BLACK);
        SceneInfo scene;
        DrawScene(g, eye, view, assets, paused ? 0.0f : frameDt, scene);
        aim = DrawNavOverlay(g, eye, view, scene, hud, ui.Has(WindowKind::Info), frameDt);
        DrawHud(g, eye, view, ui.Has(WindowKind::Market), ui.Has(WindowKind::Comms), ui.settings.showHelp, ui.online);
        ui.Draw(g);
        if (net) {
            int sw = GetScreenWidth(), sh = GetScreenHeight();
            if (!offline) {
                int n = net->Captains();
                const char *status = TextFormat("%s  -  %d captain%s  -  %d ms", statusLine.c_str(), n, n == 1 ? "" : "s", net->pingMs);
                DrawText(status, sw / 2 - MeasureText(status, 16) / 2, 8, 16, HUD_DIM);
            } else {
                DrawRectangle(0, 0, sw, sh, Fade(BLACK, 0.6f));
                const char *title = "DISCONNECTED";
                DrawText(title, sw / 2 - MeasureText(title, 30) / 2, sh / 2 - 40, 30, ORANGE);
                DrawText(net->message.c_str(), sw / 2 - MeasureText(net->message.c_str(), 20) / 2, sh / 2 + 10, 20, WHITE);
                DrawText("ESC for the main menu", sw / 2 - MeasureText("ESC for the main menu", 16) / 2, sh / 2 + 44, 16, HUD_DIM);
            }
        }
        EndDrawing();
        if (script && !script->screenshot.empty()) { TakeScreenshot(script->screenshot.c_str()); script->screenshot.clear(); }
    }

    if (net) net->Disconnect();               // (hosting with "stop when I leave": stops the server too)
    return ui.toMainMenu && !WindowShouldClose();
}

// ---------------------------------------------------------------------------
// Multiplayer from the menu (or the command line)
// ---------------------------------------------------------------------------
static bool JoinAndPlay(InputSource &input, const RenderAssets &assets, const MenuChoice &c,
                        const ProcessHandle *server = nullptr, const std::string &adminKey = "")
{
    NetClient net;
    std::string error;
    if (!net.Connect(c.address, c.port, c.name, c.password, error, c.ship)) { ShowError("COULDN'T JOIN", error); return true; }
    net.adminKey = adminKey;
    net.stopServerOnLeave = server && c.stopOnLeave;
    GameState g;
    if (!WaitForWelcome(net, g, server)) {
        net.Disconnect();
        return !WindowShouldClose();
    }
    std::string status = server ? TextFormat("HOSTING  %s:%u", NetLocalAddress().c_str(), c.port)
                                : TextFormat("ONLINE  %s", c.address.c_str());
    return PlaySession(input, nullptr, assets, g, &net, status, server != nullptr);
}

// "Host a Server": run this executable again as a dedicated server, in its own console window
static bool HostAndPlay(InputSource &input, const RenderAssets &assets, const MenuChoice &c)
{
    std::string dataDir = std::string(GetApplicationDirectory()) + "server_data";
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    std::string key = RandomKey();
    std::vector<std::string> args = { "--server", "--port", std::to_string(c.port), "--max-players", std::to_string(c.maxPlayers),
                                      "--data", dataDir, "--admin-key", key, "--pause-on-error" };
    if (!c.password.empty()) { args.push_back("--password"); args.push_back(c.password); }
    args.push_back("--seed");                 // (only used if this data folder has no galaxy yet)
    args.push_back(std::to_string(SeedFromText(c.seed, (uint64_t)(NetTime() * 1e6))));
    ProcessHandle server;
    std::string error;
    if (!LaunchProcess(SelfPath(), args, server, error, dataDir + "/server.log")) { ShowError("COULDN'T START THE SERVER", error); return true; }
    return JoinAndPlay(input, assets, c, &server, key);
}

int main(int argc, char **argv)
{
    std::string connect, name, password, bot, scriptName;
    double botSeconds = 60.0;
    bool   botIdle = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--test"))
            return RunHeadlessTests(i + 1 < argc ? argv[i + 1] : "all") == 0 ? 0 : 1;
        if (!strcmp(argv[i], "--server"))
            return RunServerConsole(argc, argv, i + 1);
        if (!strcmp(argv[i], "--script") && i + 1 < argc)   scriptName = argv[++i];
        if (!strcmp(argv[i], "--connect") && i + 1 < argc)  connect  = argv[++i];
        if (!strcmp(argv[i], "--name") && i + 1 < argc)     name     = argv[++i];
        if (!strcmp(argv[i], "--password") && i + 1 < argc) password = argv[++i];
        if (!strcmp(argv[i], "--bot") && i + 1 < argc)      bot      = argv[++i];
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)  botSeconds = atof(argv[++i]);
        if (!strcmp(argv[i], "--idle"))                     botIdle = true;
    }
    if (!bot.empty()) connect = bot;

    MenuChoice direct;                        // --connect: straight into a server
    if (!connect.empty()) {
        direct.kind = MenuChoice::Join;
        direct.address = connect;
        size_t colon = connect.rfind(':');
        if (colon != std::string::npos) { direct.port = (uint16_t)atoi(connect.c_str() + colon + 1); direct.address.resize(colon); }
        direct.name = !name.empty() ? name : TextFormat("%s %04d", bot.empty() ? "Captain" : "Bot", (int)(NetTime() * 1e6) % 10000);
        direct.password = password;
        if (!bot.empty()) return RunBot(direct.address, direct.port, direct.name, botSeconds, botIdle);
    }

    std::unique_ptr<ScriptedInput> script;
    if (!scriptName.empty()) {
        script = MakeScript(scriptName);
        if (!script) { fprintf(stderr, "unknown script '%s'\n", scriptName.c_str()); return 1; }
    }
    KeyboardInput keyboard;
    InputSource &input = script ? (InputSource &)*script : (InputSource &)keyboard;

    // One window for the whole run
    if (!NetInit()) fprintf(stderr, "Couldn't start networking - multiplayer won't work\n");
    Settings settings;
    LoadSettings(settings);
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 800, "Freehauler");
    SetTargetFPS(60);
    if (settings.fullscreen && !script) ToggleBorderlessWindowed();
    rlSetClipPlanes(1.0, 400000.0);           // compressed space tops out around 190,000
    SetExitKey(KEY_NULL);                     // Esc opens menus instead of quitting
    RenderAssets assets;
    assets.Load();

    bool again = true;
    if (script && !script->startInMenu) {     // scripted runs start straight in a game
        GameState g;
        NewGame(g);
        PlaySession(input, script.get(), assets, g);
        again = false;
    } else if (direct.kind == MenuChoice::Join) {
        again = JoinAndPlay(input, assets, direct);
    }
    while (again && !WindowShouldClose() && !(script && script->done)) {
        GameState g;
        NewGame(g);
        MenuChoice c = RunMainMenu(input, script.get(), assets, g);
        switch (c.kind) {
        case MenuChoice::Play: again = PlaySession(input, script.get(), assets, g); break;
        case MenuChoice::Join: again = JoinAndPlay(input, assets, c); break;
        case MenuChoice::Host: again = HostAndPlay(input, assets, c); break;
        default:               again = false; break;
        }
    }

    assets.Unload();
    CloseWindow();
    NetShutdown();
    return 0;
}
