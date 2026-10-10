// tests.h - automated checks and scripted demo runs.
//   freehauler --test            run every headless test (no window), print PASS/FAIL
//   freehauler --test fuel       run one
//   freehauler --script tour     play a scripted run in the window, saving screenshots
//   freehauler --bot HOST        a headless test pilot on a server
#pragma once
#include "game.h"
#include "input.h"
#include <functional>
#include <memory>
#include <string>

int RunHeadlessTests(const char *only);   // returns the number of failures

// A headless captain that joins a server and flies loops for a while, printing
// how often the server had to correct it. Handy for load-testing a server or
// giving yourself company:  freehauler --bot HOST[:PORT] [--name NAME] [--seconds N] [--idle]
// (--idle: it just sits at the start, a parked ship to practice flying up to)
int RunBot(const std::string &host, uint16_t port, const std::string &name, double seconds, bool idle = false);

// Input that comes from a script instead of the keyboard. The script can also
// move the ship around (teleport) between frames and ask for screenshots.
struct ScriptedInput : InputSource {
    GameState  *game = nullptr;
    int         frame = 0;
    std::string screenshot;       // set to a file name to save the next frame
    bool        done = false;     // ends the run
    bool        snap = false;     // the script moved the ship: don't interpolate this frame
    bool        startInMenu = false;   // begin at the main menu instead of straight in a game
    std::function<void(ScriptedInput &, InputFrame &)> body;

    InputFrame Poll() override
    {
        InputFrame f;
        if (body) body(*this, f);
        frame++;
        return f;
    }
};

std::unique_ptr<ScriptedInput> MakeScript(const std::string &name);   // null if unknown
