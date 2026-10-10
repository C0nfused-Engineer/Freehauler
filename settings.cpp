// settings.cpp - reading and writing settings.txt ("key value" lines; values may contain spaces)
#include "settings.h"
#include "raylib.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::string SettingsPath() { return std::string(GetApplicationDirectory()) + "settings.txt"; }

bool LoadSettings(Settings &s)
{
    FILE *f = fopen(SettingsPath().c_str(), "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64] = "";
        int  used = 0;
        if (sscanf(line, "%63s %n", key, &used) < 1) continue;
        std::string value = line + used;                       // the rest of the line, without the line ending
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
        float v = (float)atof(value.c_str());
        if (!strcmp(key, "mouse_sensitivity")) s.mouseSens = v < MOUSE_SENS_MIN ? MOUSE_SENS_MIN : v > MOUSE_SENS_MAX ? MOUSE_SENS_MAX : v;
        else if (!strcmp(key, "invert_mouse_y"))     s.invertY    = v != 0;
        else if (!strcmp(key, "show_orbits"))        s.showOrbits = v != 0;
        else if (!strcmp(key, "show_key_help"))      s.showHelp   = v != 0;
        else if (!strcmp(key, "fullscreen"))         s.fullscreen = v != 0;
        else if (!strcmp(key, "player_name"))        s.playerName = value;
        else if (!strcmp(key, "last_server"))        s.lastServer = value;
        else if (!strcmp(key, "ship_name"))          s.shipName = value;
        else if (!strcmp(key, "host_port"))          s.hostPort = (int)v;
        else if (!strcmp(key, "host_max_players"))   s.hostMaxPlayers = (int)v;
        else if (!strcmp(key, "host_stop_on_leave")) s.hostStopOnLeave = v != 0;
    }
    fclose(f);
    return true;
}

bool SaveSettings(const Settings &s)
{
    FILE *f = fopen(SettingsPath().c_str(), "w");
    if (!f) return false;
    fprintf(f, "mouse_sensitivity %.3f\n", s.mouseSens);
    fprintf(f, "invert_mouse_y %d\n", s.invertY ? 1 : 0);
    fprintf(f, "show_orbits %d\n", s.showOrbits ? 1 : 0);
    fprintf(f, "show_key_help %d\n", s.showHelp ? 1 : 0);
    fprintf(f, "fullscreen %d\n", s.fullscreen ? 1 : 0);
    if (!s.playerName.empty()) fprintf(f, "player_name %s\n", s.playerName.c_str());
    if (!s.lastServer.empty()) fprintf(f, "last_server %s\n", s.lastServer.c_str());
    if (!s.shipName.empty())   fprintf(f, "ship_name %s\n", s.shipName.c_str());
    fprintf(f, "host_port %d\n", s.hostPort);
    fprintf(f, "host_max_players %d\n", s.hostMaxPlayers);
    fprintf(f, "host_stop_on_leave %d\n", s.hostStopOnLeave ? 1 : 0);
    fclose(f);
    return true;
}
