// hud.h - the 2D overlay: nav dots, target brackets, ship and navigation readouts.
#pragma once
#include "render.h"

// View-side state that persists between frames
struct HudState {
    TargetId retargetCandidate;      // body the nudge cursor is resting on (while locked)...
    float    retargetDwell = 0.0f;   // ...and for how long (must be deliberate, not jitter)
};

// Nav dots and target brackets. Picks what's under the reticle and returns it
// for the next simulation step. 'infoOpen' moves labels out from under the info panel.
Aim  DrawNavOverlay(const GameState &g, const Ship &eye, const ViewState &v, const SceneInfo &scene,
                    HudState &hud, bool infoOpen, float frameDt);

// Ship status (left), navigation (right), reticle, interaction prompt, key help
void DrawHud(const GameState &g, const Ship &eye, const ViewState &v, bool tradeOpen, bool showHelp, bool online = false);
