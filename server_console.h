// server_console.h - the dedicated server as a console program.
// Used two ways, with the same options:
//   freehauler_server [options]          the standalone server (server_main.cpp; no raylib, runs on a VPS)
//   freehauler --server [options]        the game's own executable, which is what "Host a server"
//                                         in the main menu starts in a console window of its own
#pragma once

// argv[first..argc) are the server options; returns the process exit code
int RunServerConsole(int argc, char **argv, int first);
