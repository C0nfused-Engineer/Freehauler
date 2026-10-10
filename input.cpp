#include "input.h"

InputFrame KeyboardInput::Poll()
{
    InputFrame f;
    for (int k = 32; k < INPUT_KEYS; k++) {       // 0-31 aren't keys in raylib
        if (IsKeyDown(k))    f.down[k] = true;
        if (IsKeyPressed(k)) f.pressed[k] = true;
    }
    for (int b = 0; b < 3; b++) {
        if (IsMouseButtonDown(b))    f.mouseDown[b] = true;
        if (IsMouseButtonPressed(b)) f.mousePressed[b] = true;
    }
    f.mouseDelta = GetMouseDelta();
    f.mousePos   = GetMousePosition();
    f.wheel      = GetMouseWheelMove();
    f.dt         = GetFrameTime();
    for (int ch = GetCharPressed(); ch != 0; ch = GetCharPressed())
        if (ch >= 32 && ch < 127) f.text += (char)ch;
    return f;
}
