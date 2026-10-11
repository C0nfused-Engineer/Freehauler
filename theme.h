// theme.h - HUD and ship colors
#pragma once
#include "raylib.h"

const Color SHIP_LINES  = { 0, 255, 120, 255 };
const Color FLAME_COLOR = { 255, 170, 40, 255 };
const Color HUD         = SHIP_LINES;
const Color HUD_DIM     = { 0, 120, 60, 255 };
const Color HUD_LABEL   = { 120, 200, 150, 255 };

// NPC ships: filled in the colour of how they look at you (npc.h: allied, neutral, alerted, hostile), edged in white
const Color STANCE_FILL[4] = { { 120, 190, 255, 255 }, { 170, 170, 170, 255 }, { 240, 210, 50, 255 }, { 225, 45, 45, 255 } };

// Other captains' ships, by id (yours is always SHIP_LINES green)
// Bearings on the scope: a tachyon burst, a radar sweeping the ship, a place a sensor network has fixed
const Color MARK_TACHYON_COLOR = { 200, 140, 255, 255 }, MARK_RADAR_COLOR = { 255, 160, 60, 255 }, MARK_FIX_COLOR = { 140, 255, 190, 255 };

inline Color CaptainColor(int id)
{
    static const Color palette[] = { { 0, 220, 255, 255 }, { 255, 90, 220, 255 }, { 255, 230, 60, 255 },
                                     { 255, 150, 40, 255 }, { 170, 130, 255, 255 }, { 230, 230, 230, 255 },
                                     { 255, 110, 110, 255 }, { 140, 255, 200, 255 } };
    return palette[(id > 0 ? id - 1 : 0) % 8];
}
