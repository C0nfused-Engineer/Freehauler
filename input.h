// input.h - one frame of player input, from the keyboard or from a script.
// Nothing outside input.cpp calls raylib's IsKeyPressed / GetMouseDelta: the
// game reads an InputFrame, so a test script can stand in for the player.
#pragma once
#include "raylib.h"
#include <bitset>
#include <string>

const int INPUT_KEYS = 400;     // raylib key codes go up to ~350

struct InputFrame {
    std::bitset<INPUT_KEYS> down, pressed;
    std::bitset<8>          mouseDown, mousePressed;
    Vector2                 mouseDelta = { 0, 0 };
    Vector2                 mousePos   = { 0, 0 };   // cursor position (menus)
    float                   wheel      = 0.0f;       // mouse wheel this frame
    std::string             text;                    // characters typed this frame (printable ASCII)
    float                   dt         = 0.0f;       // frame time (key repeat in text fields)

    bool Down(int key) const      { return key >= 0 && key < INPUT_KEYS && down[key]; }
    bool Pressed(int key) const   { return key >= 0 && key < INPUT_KEYS && pressed[key]; }
    bool MouseDown(int b) const   { return mouseDown[b]; }
    bool MousePressed(int b) const{ return mousePressed[b]; }
    // A window that handled a key "consumes" it so nothing below reacts to it too
    void Consume(int key)         { if (key >= 0 && key < INPUT_KEYS) pressed[key] = false; }
    void ConsumeAll()             { pressed.reset(); mousePressed.reset(); text.clear(); wheel = 0.0f; }

    // Test helpers
    void Press(int key)           { pressed[key] = true; down[key] = true; }
    void Hold(int key)            { down[key] = true; }
    void Type(const std::string &s) { text += s; }
};

struct InputSource {
    virtual ~InputSource() = default;
    virtual InputFrame Poll() = 0;
};

struct KeyboardInput : InputSource {
    InputFrame Poll() override;
};
