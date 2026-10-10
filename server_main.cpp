// server_main.cpp - the standalone dedicated server program (no window, no graphics)
//
//   freehauler_server [--port 23500] [--password secret] [--max-players 16]
//                      [--data server_data] [--seed 1977] [--quiet]
//
// The game can also be its own server: "Host a server" in the main menu runs
// "freehauler --server ..." in a console window. This program is the same
// server without raylib, for running on a machine with no graphics (a VPS).
// Options and hosting notes: server_console.cpp.
#include "server_console.h"

int main(int argc, char **argv)
{
    return RunServerConsole(argc, argv, 1);
}
