// theme.h - HUD and ship colors
#pragma once
#include "raylib.h"

const Color SHIP_LINES  = { 0, 255, 120, 255 };
const Color FLAME_COLOR = { 255, 170, 40, 255 };
const Color HUD         = SHIP_LINES;
const Color HUD_DIM     = { 0, 120, 60, 255 };
const Color HUD_LABEL   = { 120, 200, 150, 255 };

// Other captains' ships, by id (yours is always SHIP_LINES green)
inline Color CaptainColor(int id)
{
    static const Color palette[] = { { 0, 220, 255, 255 }, { 255, 90, 220, 255 }, { 255, 230, 60, 255 },
                                     { 255, 150, 40, 255 }, { 170, 130, 255, 255 }, { 230, 230, 230, 255 },
                                     { 255, 110, 110, 255 }, { 140, 255, 200, 255 } };
    return palette[(id > 0 ? id - 1 : 0) % 8];
}
