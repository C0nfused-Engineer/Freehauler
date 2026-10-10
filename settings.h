// settings.h - player options, kept in settings.txt next to the game
#pragma once
#include <string>

struct Settings {
    float mouseSens  = 0.12f;     // degrees of turn per pixel of mouse movement
    bool  invertY    = false;
    bool  showOrbits = true;
    bool  showHelp   = true;      // key help line at the bottom of the screen
    bool  fullscreen = false;     // borderless fullscreen window

    // Multiplayer: remembered from the last time you joined or hosted
    std::string playerName;       // your captain's name on servers
    std::string lastServer;       // "host" or "host:port"
    int   hostPort        = 23500;
    int   hostMaxPlayers  = 16;
    bool  hostStopOnLeave = true; // stop your server when you leave it (otherwise it keeps running)
};

const float MOUSE_SENS_MIN = 0.02f, MOUSE_SENS_MAX = 0.40f;

std::string SettingsPath();       // settings.txt beside the executable
bool LoadSettings(Settings &s);   // keeps the defaults for anything missing
bool SaveSettings(const Settings &s);
