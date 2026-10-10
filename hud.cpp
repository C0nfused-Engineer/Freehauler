// hud.cpp - the 2D overlay
#include "hud.h"
#include "economy.h"
#include "format.h"
#include "theme.h"
#include <algorithm>

// The reticle marks where the nose points (the chase camera looks a few
// degrees below that), so it sits on whatever you'd fly into.
static Vector2 AimPoint(const Ship &eye, const ViewState &v)
{
    return GetWorldToScreen(Vector3Scale(ShipForward(eye), 100000.0f), v.camera);
}

// ---------------------------------------------------------------------------
// Nav overlay
// ---------------------------------------------------------------------------
Aim DrawNavOverlay(const GameState &g, const Ship &eye, const ViewState &v, const SceneInfo &scene,
                   HudState &hud, bool infoOpen, float frameDt)
{
    const std::vector<NavBody> &nav = scene.nav;
    const FlightState &f = g.flight;
    const Camera3D &camera = v.camera;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    Aim aim;

    const float HOVER_RADIUS  = 24.0f;                     // px from the reticle
    const float UNRESOLVED_PX = 12.0f;                     // planets closer than this to their star fold into it
    Vector2 reticle  = AimPoint(eye, v);
    float   pxPerRad = (sh * 0.5f) / tanf(camera.fovy * 0.5f * DEG2RAD);

    // Screen position and size of every body in front of us
    struct Screen { bool visible = false; Vector2 pos = {}; float radiusPx = 0.0f; };
    std::vector<Screen> scr(nav.size());
    for (int i = 0; i < (int)nav.size(); i++) {
        const NavBody &b = nav[i];
        Vector3 rp = MapPoint(b.rel);
        if (Vector3DotProduct(Vector3Subtract(rp, camera.position), v.camFwd) <= 0.0f) continue;   // behind us
        if (LineOfSightBlocked(b.rel, b.radius, scene.occluders, b.occ)) continue;               // hidden by a body
        scr[i] = { true, GetWorldToScreen(rp, camera), (float)(asin(fmin(1.0, b.radius / Len(b.rel))) * pxPerRad) };
    }
    auto navIndex = [&](TargetId id) {
        for (int i = 0; i < (int)nav.size(); i++) if (nav[i].id == id) return i;
        return -1;
    };

    // Dots: planets first, stars on top, so an unresolved system reads as one white dot
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < (int)nav.size(); i++)
            if (scr[i].visible && nav[i].dot && nav[i].star == (pass == 1))
                DrawCircleV(scr[i].pos, nav[i].star ? 1.1f + 1.5f * nav[i].bright : nav[i].radius == 0.0 ? 1.5f : 2.0f,
                            nav[i].star ? Dim(nav[i].color, 0.45f + 0.55f * nav[i].bright) : nav[i].color);

    // Other captains: a small name tag beside their dot, so you can find each other
    for (int i = 0; i < (int)nav.size(); i++)
        if (scr[i].visible && nav[i].id.type == TargetId::Ship && nav[i].id != f.selected)
            DrawText(nav[i].name.c_str(), (int)scr[i].pos.x + 7, (int)scr[i].pos.y - 6, 14, Fade(nav[i].color, 0.8f));

    // Closest body to a screen point, within HOVER_RADIUS of its disk. Ties go to the nearer body.
    // Planets that haven't separated from their star on screen aren't selectable yet.
    auto pick = [&](Vector2 at, int exclude) {
        int best = -1; float bestScore = 0.0f;
        for (int i = 0; i < (int)nav.size(); i++) {
            if (!scr[i].visible || i == exclude) continue;
            int p = nav[i].parent;            // e.g. a planet still overlapping its star, a ring its planet
            if (p >= 0 && scr[p].visible && nav[i].dot && Vector2Distance(scr[i].pos, scr[p].pos) < UNRESOLVED_PX) continue;
            float score = fmaxf(0.0f, Vector2Distance(scr[i].pos, at) - scr[i].radiusPx);
            if (score > HOVER_RADIUS) continue;
            if (best < 0 || score < bestScore ||
                (score == bestScore && Len(nav[i].rel) < Len(nav[best].rel))) { best = i; bestScore = score; }
        }
        return best;
    };

    int hover = -1;
    if (f.lock.valid()) {
        int li = navIndex(f.lock);
        if (li >= 0 && scr[li].visible) hover = li;

        // The nudge cursor: where the mouse would be steering if the lock weren't holding it.
        // Touching another body with it switches the lock.
        Vector2 nudge = { reticle.x - f.lockBreak.x * DEG2RAD * pxPerRad, reticle.y - f.lockBreak.y * DEG2RAD * pxPerRad };
        TargetId candidate;
        int ci = -1;
        if (Vector2Length(f.lockBreak) > 0.4f) DrawCircleLinesV(nudge, 5.0f, Fade(WHITE, 0.6f));
        if (Vector2Length(f.lockBreak) > 1.0f) {
            // A candidate must be right under the cursor AND closer to it than the current target is
            int other = pick(nudge, hover);
            if (other >= 0) {
                float toOther  = Vector2Distance(scr[other].pos, nudge) - scr[other].radiusPx;
                float toLocked = hover >= 0 ? Vector2Distance(scr[hover].pos, nudge) - scr[hover].radiusPx : 1e9f;
                if (toOther < 10.0f && toOther < toLocked) { candidate = nav[other].id; ci = other; }
            }
        }
        // ...and the cursor has to rest on it briefly, so jitter can't switch targets
        if (candidate.valid() && candidate == hud.retargetCandidate) hud.retargetDwell += frameDt;
        else hud.retargetDwell = 0.0f;
        hud.retargetCandidate = candidate;
        if (ci >= 0)
            DrawCircleLinesV(scr[ci].pos, fmaxf(scr[ci].radiusPx, 4.0f) + 6.0f * (1.0f - fminf(hud.retargetDwell / 0.3f, 1.0f)) + 3.0f,
                             Fade(WHITE, 0.8f));               // shrinking ring = switching
        if (hud.retargetDwell >= 0.3f) aim.retarget = candidate;
    } else {
        hover = pick(reticle, -1);
        hud.retargetCandidate = {};
        hud.retargetDwell = 0.0f;
    }
    if (hover >= 0) aim.hovered = nav[hover].id;

    // Brackets + text panel for a target. 'full' adds ETA, info and a tag line.
    auto drawTarget = [&](const std::string &name, Vector2 pos, float radiusPx, DVec3 rel, double radius,
                          bool star, const std::string &info, Color infoColor, const std::string &tag,
                          Color tagColor, Color bracket, bool full) {
        float h = fmaxf(radiusPx + 6.0f, 10.0f);
        float c = Clamp(h * 0.4f, 5.0f, 16.0f);
        float x0 = pos.x - h, x1 = pos.x + h, y0 = pos.y - h, y1 = pos.y + h;
        DrawLineV({ x0, y0 }, { x0 + c, y0 }, bracket); DrawLineV({ x0, y0 }, { x0, y0 + c }, bracket);
        DrawLineV({ x1, y0 }, { x1 - c, y0 }, bracket); DrawLineV({ x1, y0 }, { x1, y0 + c }, bracket);
        DrawLineV({ x0, y1 }, { x0 + c, y1 }, bracket); DrawLineV({ x0, y1 }, { x0, y1 - c }, bracket);
        DrawLineV({ x1, y1 }, { x1 - c, y1 }, bracket); DrawLineV({ x1, y1 }, { x1, y1 - c }, bracket);

        // Distance (stars in ly, planets in AU) and ETA at the current closing speed
        double centerDist = Len(rel);
        double surfDist   = fmax(0.0, centerDist - radius);
        double closing    = Dot(ToD(eye.vel), rel * (1.0 / centerDist));
        std::vector<std::pair<std::string, Color>> lines = {
            { star || surfDist > 0.01 * LY_U ? FormatStarDist(surfDist) : FormatPlanetDist(surfDist), LIGHTGRAY } };
        if (full) {
            lines.push_back({ closing > 1e-3 ? "ETA " + FormatETA(surfDist / closing) : std::string("ETA --  not closing"), LIGHTGRAY });
            if (!info.empty()) lines.push_back({ info, infoColor });
            if (!tag.empty())  lines.push_back({ tag, tagColor });
        }
        // Beside the top-right bracket, clear of the HUD, on a dark panel (readable over a white star)
        int w = MeasureText(name.c_str(), 18);
        for (auto &l : lines) w = std::max(w, MeasureText(l.first.c_str(), 16));
        int hgt = 22 + 18 * (int)lines.size();
        int tx = (int)(x1 + 10), ty = (int)y0;
        if (tx + w > sw - 20) tx = (int)(x0 - 10 - w);      // flip to the left side...
        if (infoOpen && tx + w > sw - 430 && ty < 560) tx = (int)(x0 - 10 - w);   // ...or out from under the info panel
        tx = (int)Clamp((float)tx, 20.0f, (float)(sw - 20 - w));
        ty = (int)Clamp((float)ty, 140.0f, (float)(sh - 130 - hgt));
        DrawRectangle(tx - 5, ty - 3, w + 10, hgt, Fade(BLACK, 0.7f));
        DrawText(name.c_str(), tx, ty, 18, full ? WHITE : LIGHTGRAY);
        for (size_t li = 0; li < lines.size(); li++)
            DrawText(lines[li].first.c_str(), tx, ty + 20 + 18 * (int)li, 16, lines[li].second);
    };

    // --- The selection: bracketed wherever it is, or an edge arrow if it's off screen ---
    DVec3  selRel;
    double selRadius = 0.0;
    TargetId sel = f.selected;
    if (sel.valid() && FindTarget(g, sel, selRel, &selRadius)) {
        selRel = selRel + (g.player.ship.pos - eye.pos);    // seen from the (interpolated) eye
        int ni = navIndex(sel);
        const NavBody *nb = ni >= 0 ? &nav[ni] : nullptr;
        std::string name = TargetName(g, sel);
        bool star   = CanonicalTarget(g, sel).type == TargetId::Star;
        bool locked = sel == f.lock;
        std::string tag = locked ? "LOCKED" : (infoOpen ? "SELECTED" : "SELECTED  [MMB] info");

        Vector3 rp = MapPoint(selRel);
        bool inFront = Vector3DotProduct(Vector3Subtract(rp, camera.position), v.camFwd) > 0.0f;
        Vector2 sp = inFront ? GetWorldToScreen(rp, camera) : Vector2{ -1, -1 };
        bool onScreen = inFront && sp.x >= 0 && sp.y >= 0 && sp.x <= sw && sp.y <= sh;
        if (onScreen) {
            float rpx = (float)(asin(fmin(1.0, selRadius / Len(selRel))) * pxPerRad);
            drawTarget(name, sp, rpx, selRel, selRadius, star, nb ? nb->info : std::string(),
                       nb ? nb->color : LIGHTGRAY, tag, locked ? HUD : HUD_LABEL, WHITE, true);
        } else {
            // Arrow on the screen edge, pointing the way to turn
            Vector2 center = { sw * 0.5f, sh * 0.5f };
            Vector2 d = { (float)Dot(selRel, ToD(v.camRight)), -(float)Dot(selRel, ToD(camera.up)) };
            if (Vector2Length(d) < 1e-6f) d = { 0.0f, 1.0f };
            d = Vector2Normalize(d);
            float t = fminf((sw * 0.5f - 60.0f) / fmaxf(fabsf(d.x), 1e-4f), (sh * 0.5f - 60.0f) / fmaxf(fabsf(d.y), 1e-4f));
            Vector2 at  = Vector2Add(center, Vector2Scale(d, t));
            Vector2 tip = Vector2Add(at, Vector2Scale(d, 14.0f));
            Vector2 n   = { -d.y, d.x };
            Vector2 b1  = Vector2Add(at, Vector2Scale(n, 8.0f)), b2 = Vector2Subtract(at, Vector2Scale(n, 8.0f));
            DrawLineV(tip, b1, WHITE); DrawLineV(b1, b2, WHITE); DrawLineV(b2, tip, WHITE);
            std::string lbl = name + "  " + (star ? FormatStarDist(Len(selRel)) : FormatPlanetDist(fmax(0.0, Len(selRel) - selRadius)));
            int lw = MeasureText(lbl.c_str(), 16);
            Vector2 lp = Vector2Subtract(at, Vector2Scale(d, 24.0f));
            DrawText(lbl.c_str(), (int)Clamp(lp.x - lw * 0.5f, 10.0f, (float)(sw - lw - 10)), (int)Clamp(lp.y - 8.0f, 10.0f, (float)(sh - 26)), 16, WHITE);
        }
    } else {
        sel = {};
    }

    // --- Whatever's under the reticle (if it isn't the selection) ---
    if (hover >= 0 && nav[hover].id != sel) {
        const NavBody &b = nav[hover];
        bool full = !sel.valid();                          // compact when something else is selected
        drawTarget(b.name, scr[hover].pos, scr[hover].radiusPx, b.rel, b.radius, b.star, b.info, b.color,
                   "[MMB] info", HUD_LABEL, full ? WHITE : GRAY, full);
    }
    return aim;
}

// ---------------------------------------------------------------------------
// HUD readouts
// ---------------------------------------------------------------------------
void DrawHud(const GameState &g, const Ship &eye, const ViewState &v, bool tradeOpen, bool showHelp, bool online)
{
    const FlightState &f = g.flight;
    const Player &p = g.player;
    const DriveInfo &drive = f.drive;
    const Galaxy &systems = g.systems;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float speed = Vector3Length(eye.vel);
    bool hyper = f.last.hyper;

    // --- Left column: ship ---
    const char *mode = hyper ? (drive.noFuel ? "LIMPING - NO FUEL" : drive.inField ? "HYPER" : "WARP")
                             : (speed > MAX_SPEED * 1.01f ? "SPOOLING DOWN" : "CRUISE");
    DrawText(("SPEED " + FormatSpeed(speed)).c_str(), 20, 20, 24, HUD);

    // Drive gauge, log scale from cruise to warp, tick at hyper
    float gauge = (float)Clamp((float)(log(fmax(speed, MAX_SPEED) / MAX_SPEED) / log(WARP_SPEED / MAX_SPEED)), 0.0f, 1.0f);
    int   tick  = 22 + (int)(196 * log(HYPER_SPEED / MAX_SPEED) / log(WARP_SPEED / MAX_SPEED));
    DrawRectangleLines(20, 52, 200, 12, HUD_DIM);
    DrawRectangle(22, 54, (int)(196 * gauge), 8, hyper ? HUD : HUD_DIM);
    DrawLine(tick, 50, tick, 66, HUD);
    DrawText(mode, 228, 50, 16, hyper ? HUD : HUD_DIM);

    DrawText(p.ship.dampeners ? "DAMPENERS ON" : "DAMPENERS OFF  - COASTING", 20, 74, 18,
             p.ship.dampeners ? HUD : ORANGE);
    int y = 98;
    if (p.ship.allStop) { DrawText("ALL STOP", 20, y, 18, YELLOW); y += 24; }
    if (hyper && !drive.limiter.empty() && drive.limit < (drive.inField ? HYPER_SPEED : WARP_SPEED))
        { DrawText(TextFormat("GRAVITY WELL: slowing for %s", drive.limiter.c_str()), 20, y, 18, YELLOW); y += 24; }
    if (f.lock.valid())          { DrawText(TextFormat("TARGET LOCK: %s", TargetName(g, f.lock).c_str()), 20, y, 18, HUD); y += 24; }
    else if (f.selected.valid()) { DrawText(TextFormat("TARGET: %s   [MMB] info", TargetName(g, f.selected).c_str()), 20, y, 18, HUD_LABEL); y += 24; }
    if (!f.miningStatus.empty()) DrawText(f.miningStatus.c_str(), 20, y, 18, f.miningActive ? ORANGE : YELLOW);

    // Cargo hold, bottom left
    {
        const int MAX_LINES = 10;                       // the biggest loads; the rest summed up
        std::vector<int> held;
        for (int o = 0; o < GOOD_COUNT; o++) if (p.cargo[o] > 0.05) held.push_back(o);
        std::stable_sort(held.begin(), held.end(), [&](int a, int b) { return p.cargo[a] > p.cargo[b]; });
        int lines = std::min((int)held.size(), MAX_LINES) + ((int)held.size() > MAX_LINES ? 1 : 0);
        double inHold = p.CargoTotal();
        int cy = sh - 56 - 18 * lines;
        DrawText(TextFormat("CARGO  %.1f / %.0f t", inHold, CARGO_CAPACITY), 20, cy, 18,
                 inHold >= CARGO_CAPACITY - 1e-6 ? ORANGE : HUD);
        int top = cy;
        for (int i = 0; i < (int)held.size() && i < MAX_LINES; i++) {
            int o = held[i];
            cy += 18;
            DrawText(TextFormat("  %-16s %6.1f t", GoodData(o).name, p.cargo[o]), 20, cy + 4, 16, GoodData(o).color);
        }
        if ((int)held.size() > MAX_LINES) {
            double rest = 0;
            for (int i = MAX_LINES; i < (int)held.size(); i++) rest += p.cargo[held[i]];
            cy += 18;
            DrawText(TextFormat("  %d more %11.1f t", (int)held.size() - MAX_LINES, rest), 20, cy + 4, 16, HUD_DIM);
        }

        // Fuel gauge and credits above the cargo
        float frac = (float)(p.fuel / FUEL_CAPACITY);
        Color fc = p.fuel <= 0.0 ? RED : frac < 0.2f ? ORANGE : HUD;
        DrawText(TextFormat("FUEL   %.1f / %.0f t", p.fuel, FUEL_CAPACITY), 20, top - 26, 18, fc);
        DrawRectangleLines(230, top - 24, 120, 12, HUD_DIM);
        DrawRectangle(232, top - 22, (int)(116 * frac), 8, fc);
        if (f.burn > 0.0) DrawText(TextFormat("-%.3f t/s", f.burn), 360, top - 26, 18, f.burn > HYPER_BURN * 1.01 ? ORANGE : HUD_DIM);
        DrawText(TextFormat("CREDITS  %s cr", Thousands(p.credits).c_str()), 20, top - 50, 18, HUD);
        int sy = top - 74;
        if (f.scooping) { DrawText(TextFormat("FUEL SCOOPING  +%.1f t/s", SCOOP_RATE), 20, sy, 18, ORANGE); sy -= 24; }
        if (f.skimming) DrawText(TextFormat("SKIMMING HELIUM-3  +%.2f t/s", SKIM_RATE), 20, sy, 18, GoodData(ORE_HELIUM3).color);
        else if (f.skimFull) DrawText("SKIMMING HELIUM-3  -  HOLD FULL", 20, sy, 18, ORANGE);
    }

    // --- Right column: navigation ---
    int x = sw - 400; y = 20;
    DVec3 galPos = systems.PositionLy(eye.anchor, eye.pos);
    double anchorRatio = FieldRatio(systems[eye.anchor], eye.pos);
    if (g.loaded.index >= 0 && anchorRatio < 1.5) {
        const StarSystem &s = systems[g.loaded.index];
        DVec3  sp = FromShip(systems, eye, g.loaded.index) * -1.0;
        double h = Dot(sp, s.n), r = Len(sp - s.n * h);
        DrawText(TextFormat("SYSTEM  %s", s.name.c_str()), x, y, 20, HUD);                              y += 26;
        DrawText(TextFormat("GAL  %.1f  %.1f  %.1f ly", galPos.x, galPos.y, galPos.z), x, y, 16, HUD_DIM); y += 20;
        DrawText(TextFormat("SYS  r %.2f AU   h %+.2f AU", r / AU_U, h / AU_U), x, y, 16, HUD_DIM);        y += 24;
    } else {
        const StarSystem &s = systems[eye.anchor];
        DrawText("INTERSTELLAR SPACE", x, y, 20, HUD);                                                      y += 26;
        DrawText(TextFormat("GAL  %.1f  %.1f  %.1f ly", galPos.x, galPos.y, galPos.z), x, y, 16, HUD_DIM); y += 20;
        DrawText(TextFormat("NEAREST STAR  %s  %s", s.name.c_str(), FormatStarDist(Len(eye.pos)).c_str()), x, y, 16, HUD_DIM); y += 24;
    }
    if (drive.inField) {
        DrawText("IN GRAVITY FIELD - HYPER ONLY", x, y, 18, ORANGE);                                     y += 22;
        DrawText(TextFormat("EXIT: climb %.2f AU  or  edge %.1f AU",
                            drive.exitClimb / AU_U, drive.exitEdge / AU_U), x, y, 16, ORANGE);
    } else {
        DrawText(p.fuel > 0.0 ? "CLEAR OF GRAVITY FIELDS - WARP READY" : "CLEAR OF GRAVITY FIELDS", x, y, 18, HUD);
    }
    if (p.fuel <= 0.0) {
        y += 22; DrawText("NO FUEL - WARP OFFLINE, HYPER LIMPING", x, y, 18, RED);
        y += 22; DrawText("Scoop near a star (within 3 radii) or buy fuel", x, y, 16, ORANGE);
    }
    y += 22;

    // Inside a belt or ring?
    if (g.loaded.index >= 0) {
        DVec3 here = FromShip(systems, eye, g.loaded.index) * -1.0;
        for (const Zone &z : g.loaded.zones)
            if (ZoneDensity(z, here) > 0.0) {
                DrawText(TextFormat("%s  %s", z.ring ? "IN RING SYSTEM" : "IN ASTEROID BELT", z.name.c_str()),
                         x, y, 18, z.color.r > 150 ? z.color : Color{ 200, 185, 160, 255 });
                y += 22;
            }
    }

    // Reticle
    Vector2 aim = AimPoint(eye, v);
    DrawLineV({ aim.x - 8, aim.y }, { aim.x - 3, aim.y }, HUD_DIM);
    DrawLineV({ aim.x + 3, aim.y }, { aim.x + 8, aim.y }, HUD_DIM);
    DrawLineV({ aim.x, aim.y - 8 }, { aim.x, aim.y - 3 }, HUD_DIM);
    DrawLineV({ aim.x, aim.y + 3 }, { aim.x, aim.y + 8 }, HUD_DIM);

    // Interaction prompt
    if (!tradeOpen && f.nearBody >= 0 && f.nearBody < (int)g.loaded.bodies.size()) {
        const Body &b = g.loaded.bodies[f.nearBody];
        std::string pr = b.population > 0 ? "[F] Trade at " + b.name + "  -  " + SettlementName(b.settlement)
                                          : b.name + "  -  uninhabited";
        int w = MeasureText(pr.c_str(), 20);
        DrawRectangle(sw / 2 - w / 2 - 10, sh - 104, w + 20, 30, Fade(BLACK, 0.7f));
        DrawText(pr.c_str(), sw / 2 - w / 2, sh - 99, 20, b.population > 0 ? HUD : GRAY);
    }

    if (showHelp) {
        const char *help = online ? "W/S/A/D thrust  Q/E roll  SHIFT hyper/warp  X stop  CAPS damp  C comms  O orbits  "
                                    "MMB info  F trade  LMB mine  ESC menu"
                                  : "W/S/A/D thrust  Q/E roll  SHIFT hyper/warp  X stop  CAPS damp  C comms  O orbits  "
                                    "MMB info  F trade  LMB mine  F5/F9 quicksave/load  ESC menu";
        int size = 16;
        while (size > 10 && MeasureText(help, size) > sw - 40) size--;          // shrink to fit narrow windows
        DrawText(help, 20, sh - 14 - size, size, HUD_DIM);
    }
}
