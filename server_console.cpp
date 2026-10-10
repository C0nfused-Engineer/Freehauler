// server_console.cpp - options, the run loop and shutdown for the dedicated server
//
//   --port 23500  --password secret  --max-players 16  --data server_data  --seed 1977  --quiet
//
// Stop it with Ctrl+C (or by closing its window on Windows): it saves first.
// Players connect from the main menu (Multiplayer > Join) or with
//   freehauler --connect <address>[:port] --name <name>
//
// Hosting from home: allow the server through Windows Firewall (UDP, the port
// above) and forward that UDP port on your router to this PC. Testers then
// use your public IP. Or skip the router: a free VPN like Tailscale or ZeroTier
// puts everyone on one virtual network; connect to the server's VPN address.
//
// Started by the game ("Host a server") it also gets:
//   --admin-key K       the game may ask it to stop (when the host leaves)
//   --pause-on-error    if it can't start, wait for Enter so the window stays readable
#include "server_console.h"
#include "process.h"
#include "server.h"
#include "raylib.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int RunServerConsole(int argc, char **argv, int first)
{
    ServerConfig cfg;
    bool pauseOnError = false;
    for (int i = first; i < argc; i++) {
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if      (!strcmp(argv[i], "--port"))           cfg.port = (uint16_t)atoi(next());
        else if (!strcmp(argv[i], "--password"))       cfg.password = next();
        else if (!strcmp(argv[i], "--max-players"))    cfg.maxPlayers = atoi(next());
        else if (!strcmp(argv[i], "--data"))           cfg.dataDir = next();
        else if (!strcmp(argv[i], "--seed"))           cfg.seed = (unsigned)strtoul(next(), nullptr, 10);
        else if (!strcmp(argv[i], "--quiet"))          cfg.log = false;
        else if (!strcmp(argv[i], "--admin-key"))      cfg.adminKey = next();
        else if (!strcmp(argv[i], "--pause-on-error")) pauseOnError = true;
        else if (!strcmp(argv[i], "--server"))         continue;
        else {
            printf("Freehauler dedicated server\n\n"
                   "  --port N          UDP port to listen on (default %u)\n"
                   "  --password TEXT   players must give this to join (default: none)\n"
                   "  --max-players N   (default 16)\n"
                   "  --data FOLDER     where the galaxy and players are saved (default server_data)\n"
                   "  --seed N          galaxy seed for a NEW world (default %u)\n"
                   "  --quiet           don't log joins and saves\n", DEFAULT_PORT, DEFAULT_GALAXY_SEED);
            return strcmp(argv[i], "--help") && strcmp(argv[i], "-h") ? 1 : 0;
        }
    }
    auto fail = [&](const std::string &msg) {
        fprintf(stderr, "%s\n", msg.c_str());
        if (pauseOnError) { printf("\nPress Enter to close this window\n"); fflush(stdout); getchar(); }
        return 1;
    };
    if (cfg.port == 0 || cfg.maxPlayers < 1) return fail("Bad --port or --max-players");

    SetTraceLogLevel(LOG_WARNING);                   // (the game's own "loaded system" notes, once per captain)
    SetConsoleTitleText("Freehauler server - UDP " + std::to_string(cfg.port));
    if (!NetInit()) return fail("Couldn't start networking (WSAStartup failed)");
    static volatile bool quit = false;
    NetCatchQuit(&quit);

    Server server;
    std::string error;
    if (!server.Start(cfg, error)) {
        NetShutdown();
        return fail(error);
    }
    printf("Players on this network join with %s:%u\n", NetLocalAddress().c_str(), cfg.port);
    printf("Ctrl+C to stop (progress is saved on the way out)\n");
    fflush(stdout);
    server.Run(quit);
    server.Stop();
    NetShutdown();
    return 0;
}
