// ui.h - windows (info, market, comms, maps), hotkeys, messages.
//
// Windows live on a stack. Each frame the top window sees input first and
// "consumes" the keys it uses; a modal window (the market) eats everything
// and pauses flight; menus also stop the simulation and show the mouse
// pointer. Whatever is left goes to the global hotkeys. Esc closes the top
// window, or opens the pause menu when none are open.
//
// Windows never change GameState directly: they make Commands (SellGoods, Hail,
// ...; see commands.h) and hand them to Do(). Single player applies them on
// the spot; online they go to the server. Either way the results come back as
// events, which the Ui turns into messages.
#pragma once
#include "commands.h"
#include "game.h"
#include "input.h"
#include "settings.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class WindowKind { Info, Market, Comms, SystemMap, SectorMap, Factions, TradeLog, News, Pause, SaveMenu, LoadMenu, SettingsMenu, ConfirmExit,
                        MainMenu, SinglePlayerMenu, MultiplayerMenu, JoinMenu, HostMenu, NewGameMenu };

// What the main menu decided
struct MenuChoice {
    enum Kind { None, Play, Join, Host } kind = None;   // Play: the GameState is ready (a new game, or a loaded save)
    std::string address;                 // Join: host name or IP
    uint16_t    port = 23500;            // Join / Host
    std::string name, password;          // your captain's name; the server password (may be empty)
    std::string ship;                    // your ship's name (may be empty)
    std::string seed;                    // Host: the galaxy seed, as typed (empty: a random one)
    int         maxPlayers = 16;         // Host
    bool        stopOnLeave = true;      // Host: stop the server when you leave it
};

struct Ui;

class Window {
public:
    virtual ~Window() = default;
    virtual WindowKind Kind() const = 0;
    virtual bool Modal() const { return false; }                       // pauses flight, eats all input
    virtual bool PausesGame() const { return false; }                  // stops the simulation (menus)
    virtual bool WantsCursor() const { return false; }                 // shows the mouse pointer
    virtual void HandleInput(InputFrame &in, GameState &g, Ui &ui) { (void)in; (void)g; (void)ui; }
    virtual bool StillValid(const GameState &g) const { (void)g; return true; }
    virtual void Draw(const GameState &g) const = 0;
    bool closed = false;
};

struct Ui {
    std::vector<std::unique_ptr<Window>> stack;   // back = top
    std::string toast;                            // short message at the bottom of the screen...
    float       toastTime = 0.0f;                 // ...and how long it stays
    bool        quit = false;
    bool        gameReloaded = false;             // a save was loaded this frame (the view should snap)
    Settings    settings;

    // The course set on a map: it becomes the selection (hold Shift to lock on and go), and
    // the nose swings onto it until it's there or the pilot takes the stick
    TargetId    course;
    bool        steer = false;
    void        SetCourse(TargetId t) { course = t; steer = t.valid(); }

    // Multiplayer: the server runs the world, so menus don't pause it and
    // saving is the server's job. Commands go to sendCommand.
    bool        online = false;
    bool        hosting = false;                  // ...on the server this game started
    bool        stopServerOnLeave = false;        // ...which stops when we leave
    std::function<void(const Command &)> sendCommand;

    // The main menu (no game running behind it): no hotkeys, and 'choice' says what to do next
    bool        mainMenu = false;
    MenuChoice  choice;
    bool        toMainMenu = false;               // in a game: the player asked to go back to the main menu

    bool    Has(WindowKind k) const;
    Window *Find(WindowKind k);
    void    Open(std::unique_ptr<Window> w);
    void    Close(WindowKind k);
    bool    FlightPaused() const;                 // a modal window is open
    bool    GamePaused() const;                   // a menu is open: the simulation stops
    bool    WantsCursor() const;                  // a menu is open: show the mouse pointer
    void    CloseAll();
    void    Load(GameState &g, const std::string &path);   // load a save; reports the result
    void    Say(const std::string &msg) { toast = msg; toastTime = 3.0f; }
    void    Do(GameState &g, const Command &cmd);  // a player action: applied here, or sent to the server

    // Windows first, then global hotkeys
    void HandleInput(InputFrame &in, GameState &g);
    // Close windows that no longer apply; turn this frame's events into messages
    void Update(GameState &g, float frameDt);
    void Draw(const GameState &g) const;
};

// Mouse and flight keys -> Controls for this frame (rotation in degrees for the whole frame)
Controls ReadFlightControls(const InputFrame &in, float frameDt, const Settings &settings, bool &throttleCut);

// The maps (map.cpp): M opens the system map inside a system's gravity field, the sector map
// outside one; M again goes from the system map to the sector map
std::unique_ptr<Window> MakeSystemMap();
std::unique_ptr<Window> MakeSectorMap();

// The factions window (factionui.cpp): R
std::unique_ptr<Window> MakeFactionsWindow();

// The trade log (T): every market you have prices for, good by good. The news (N). (tradeui.cpp)
std::unique_ptr<Window> MakeTradeLog();
std::unique_ptr<Window> MakeNewsWindow();

// One line of the system map's list: a star, world, belt, moon or station (indented under its world)
struct MapRow {
    TargetId    target;
    int         indent = 0;
    bool        known = false;       // on the captain's charts in full
    bool        inhabited = false;
    std::string name, what;
};
std::vector<MapRow> SystemMapRows(const GameState &g);

// The pause menu and its sub-menus (menus.cpp). Online it has no save/load
// and doesn't stop the simulation (the server keeps running).
std::unique_ptr<Window> MakePauseMenu(bool online = false);

// The main menu: Single Player (new game, load), Multiplayer (host, join), Settings, Exit
std::unique_ptr<Window> MakeMainMenu();
