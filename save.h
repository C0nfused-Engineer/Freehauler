// save.h - save games.
// A save is just the galaxy seed + the deltas + the player: everything else
// is regenerated from the seed. Plain text with a version number, so it's
// easy to inspect and old saves can be upgraded when the format grows.
#pragma once
#include "game.h"

#include <string>

const int SAVE_VERSION = 4;           // 2: the galaxy at full size. 3: the economy, and the goods redrawn. 4: sensors
const int SAVE_OLDEST  = 2;           // saves from the old twelve-star galaxy can't be placed in this one
const int SAVE_SENSORS = 4;           // ships carry radar, an infrared array and a tachyon detector: older saves' ships are given the basic ones
const int SAVE_GOODS   = 3;           // cargo is stored by good number: older saves' holds can't be read, and load empty

// Saves live in a "saves" folder beside the executable
std::string SaveDirectory();                       // ends with a path separator; created if missing
std::string SavePathFor(const std::string &name);  // "<save dir>/<name>.txt"
std::string QuicksavePath();                       // F5 / F9
bool        ValidSaveName(const std::string &name);

bool SaveGame(const GameState &g, const char *path, const std::string &name = "");
bool LoadGame(GameState &g, const char *path);     // on failure 'g' is left unchanged

// The headline facts of a save file, for the load browser (without loading it)
struct SaveSummary {
    bool        valid = false;
    int         version = 0;
    std::string name, location;
    unsigned    seed = 0;
    double      clock = 0, credits = 0, fuel = 0, cargo = 0;
};
SaveSummary ReadSaveSummary(const char *path);
