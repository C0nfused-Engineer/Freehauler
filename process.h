// process.h - starting the dedicated server as its own program.
//
// "Host a server" in the game runs this same executable again with --server,
// in a console window of its own (on Linux: in the background, logging to a
// file). It's a real dedicated server: it keeps running if the game closes
// (unless you asked it to stop when you leave), and anyone can join it.
//
// Like net.cpp, process.cpp is the only place that includes <windows.h>.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ProcessHandle {
    intptr_t handle = 0;      // Windows: process HANDLE
    int      pid = 0;
    bool Running() const;     // false once it has exited (e.g. its port was taken)
};

// Starts 'exe' with 'args' (no shell; arguments are passed as they are).
// Windows: in a new console window. Elsewhere: detached, output to 'logFile'.
bool LaunchProcess(const std::string &exe, const std::vector<std::string> &args, ProcessHandle &out,
                   std::string &error, const std::string &logFile = "");

std::string SelfPath();                          // full path of the running executable
void        SetConsoleTitleText(const std::string &title);   // Windows console title (no-op elsewhere)
std::string RandomKey();                         // 16 hex digits, unpredictable enough for an admin key
