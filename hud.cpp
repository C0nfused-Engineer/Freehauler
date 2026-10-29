// hud.cpp - the 2D overlay
#include "hud.h"
#include "comms.h"
#include "economy.h"
#include "format.h"
#include "sensors.h"
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

    // Bearings (sensors.h): the direction of something that can't be placed. A tachyon burst is marked where the
    // middle of its range would put it, inside a ring as wide as the bearing may be off; as the ship moves, the mark
    // shifts against the stars, the less the further off it is. A radar sweeping the ship is marked the same way.
    // A network's fix is a place, with a ring as wide as it may be out.
    for (const SensorMark &m : g.marks) {
        bool   fix = m.kind == MARK_FIX, radar = m.kind == MARK_RADAR;
        double range = fix ? 0.0 : radar ? 1.0 * AU_U : 0.5 * (m.rLo + m.rHi);
        DVec3  rel = FromShip(g.systems, eye, m.anchor, m.from + m.dir * range);
        double dist = Len(rel);
        Vector3 rp = MapPoint(rel);
        if (dist < 1.0 || Vector3DotProduct(Vector3Subtract(rp, camera.position), v.camFwd) <= 0.0f) continue;
        Vector2 p = GetWorldToScreen(rp, camera);
        if (p.x < -50 || p.y < -50 || p.x > sw + 50 || p.y > sh + 50) continue;
        float fade = radar ? 1.0f : 0.3f + 0.7f * Clamp(1.0f - (float)((g.clock - m.at) / MARK_TIME), 0.0f, 1.0f);
        Color col = Fade(fix ? MARK_FIX_COLOR : radar ? MARK_RADAR_COLOR : MARK_TACHYON_COLOR, fade);
        float ring = fix ? (float)(atan(m.rLo / dist) * pxPerRad) : m.sigma * DEG2RAD * pxPerRad;
        ring = Clamp(ring, 9.0f, sh * 0.45f);
        DrawCircleLinesV(p, ring, col);
        DrawLineV({ p.x, p.y - 6 }, { p.x + 6, p.y }, col); DrawLineV({ p.x + 6, p.y }, { p.x, p.y + 6 }, col);
        DrawLineV({ p.x, p.y + 6 }, { p.x - 6, p.y }, col); DrawLineV({ p.x - 6, p.y }, { p.x, p.y - 6 }, col);
        std::string label = radar ? std::string("RADAR")
                          : fix   ? "FIX  " + FormatPlanetDist(dist) + "  +/- " + FormatPlanetDist(m.rLo)
                                  : std::string(TextFormat("TACHYON  %d counts  %.1f-%.1f AU", m.counts, m.rLo / AU_U, m.rHi / AU_U));
        if (!radar) label += "  " + FormatETA(fmax(0.0, g.clock - m.at)) + " ago";
        DrawText(label.c_str(), (int)(p.x + ring * 0.75f + 6), (int)(p.y - ring * 0.75f - 14), 14, col);
        if (!m.note.empty()) DrawText(m.note.c_str(), (int)(p.x + ring * 0.75f + 6), (int)(p.y - ring * 0.75f + 2), 12, Fade(col, 0.8f));
    }

    // Other captains: a small name tag beside their dot, so you can find each other
    for (int i = 0; i < (int)nav.size(); i++)
        if (scr[i].visible && nav[i].id.type == TargetId::Ship && nav[i].id.index < NPC_ID_BASE && nav[i].id != f.selected)
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

    int hover = pick(reticle, -1);
    (void)hud; (void)frameDt;
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

    // --- The destination locked (or, with none, the selection): bracketed wherever it is, or an edge arrow if it's off screen ---
    DVec3  selRel;
    double selRadius = 0.0;
    TargetId sel = f.lock.valid() ? f.lock : f.selected;
    if (sel.valid() && FindTarget(g, sel, selRel, &selRadius)) {
        selRel = selRel + (g.player.ship.pos - eye.pos);    // seen from the (interpolated) eye
        int ni = navIndex(sel);
        const NavBody *nb = ni >= 0 ? &nav[ni] : nullptr;
        std::string name = TargetName(g, sel);
        bool star   = CanonicalTarget(g, sel).type == TargetId::Star;
        bool locked = sel == f.lock;
        std::string tag = locked ? "LOCKED  -  SHIFT to go  [SPACE] release" : (infoOpen ? "[SPACE] lock" : "[SPACE] lock  [I] info");

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
                   "[SPACE] lock  [I] info", HUD_LABEL, full ? WHITE : GRAY, full);
    }
    return aim;
}

// ---------------------------------------------------------------------------
// HUD readouts
// ---------------------------------------------------------------------------
void DrawHud(const GameState &g, const Ship &eye, const ViewState &v, bool tradeOpen, bool commsOpen, bool showHelp, bool online)
{
    const FlightState &f = g.flight;
    const Player &p = g.player;
    const DriveInfo &drive = f.drive;
    const Galaxy &systems = g.systems;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float speed = Vector3Length(eye.vel);
    bool hyper = f.last.hyper;

    // --- Left column: ship ---
    const char *mode = f.driveNone ? "NO HYPER DRIVE FITTED" : f.driveCharging ? "CHARGING FOR WARP" : f.driveTooHeavy ? "TOO HEAVY FOR THE DRIVE"
                     : hyper ? (drive.noFuel ? "LIMPING - NO FUEL" : f.driveStarved ? "WARP - REACTOR AT ITS LIMIT" : drive.inField ? "HYPER" : "WARP")
                             : (speed > MAX_SPEED * 1.01f ? "SPOOLING DOWN" : "CRUISE");
    DrawText(("SPEED " + FormatSpeed(speed)).c_str(), 20, 20, 24, HUD);

    // Drive gauge, log scale from a crawl to warp: ticks where cruise tops out (0.99 c) and at
    // full hyper, and a marker under it for the speed the throttle is asking for
    const double GAUGE_LOW = 2.0;
    auto place = [&](double v) { return (float)Clamp((float)(log(fmax(v, GAUGE_LOW) / GAUGE_LOW) / log(WARP_SPEED / GAUGE_LOW)), 0.0f, 1.0f); };
    double asked = ThrottleSpeed(p.ship.throttle);
    DrawRectangleLines(20, 52, 200, 12, HUD_DIM);
    DrawRectangle(22, 54, (int)(196 * place(speed)), 8, hyper ? HUD : HUD_DIM);
    for (double v : { (double)MAX_SPEED, p.spec.hyperSpeed }) { int t = 22 + (int)(196 * place(v)); DrawLine(t, 50, t, 66, HUD); }
    {   // (under it: the speed asked for: the throttle's, or what the drive is set to run at)
        double set = hyper ? p.ship.ftl * (p.power.warping ? p.spec.warpSpeed : p.spec.hyperSpeed) : fabs(asked);
        int mark = 22 + (int)(196 * place(set));
        DrawTriangle({ (float)mark, 65.0f }, { mark - 5.0f, 73.0f }, { mark + 5.0f, 73.0f }, !hyper && asked < 0.0 ? ORANGE : HUD);
    }
    DrawText(mode, 228, 50, 16, f.driveCharging || f.driveTooHeavy || f.driveStarved || f.driveNone ? YELLOW : hyper ? HUD : HUD_DIM);

    if (hyper) DrawText(TextFormat("DRIVE SET %d%%   field %.0f TW   [W/S]", (int)(p.ship.ftl * 100.0f + 0.5f), f.drivePower), 20, 78, 18, HUD);
    else DrawText(TextFormat("THROTTLE %s%d%%   %s", asked < 0.0 ? "REV " : "", (int)(fabsf(p.ship.throttle) * 100.0f + 0.5f),
                             FormatSpeed((float)fabs(asked)).c_str()), 20, 78, 18, asked < 0.0 ? ORANGE : HUD);
    DrawText(p.ship.dampeners ? "DAMPENERS ON" : "DAMPENERS OFF  - DRIFTING", 20, 102, 18,
             p.ship.dampeners ? HUD : ORANGE);
    int y = 126;
    if (p.ship.allStop) { DrawText("ALL STOP", 20, y, 18, YELLOW); y += 24; }
    if (hyper && drive.holding)
        { DrawText(TextFormat("ARRIVED: holding off %s", drive.limiter.c_str()), 20, y, 18, YELLOW); y += 24; }
    else if (hyper && !drive.limiter.empty() && drive.limit < (drive.inField ? p.spec.hyperSpeed : p.spec.warpSpeed) && !f.driveStarved)
        { DrawText(TextFormat("GRAVITY WELL: slowing for %s", drive.limiter.c_str()), 20, y, 18, YELLOW); y += 24; }
    else if (!hyper && !drive.cruiseLimiter.empty() && fabs(asked) > drive.cruiseLimit)
        { DrawText(TextFormat("GRAVITY WELL: slowing for %s", drive.cruiseLimiter.c_str()), 20, y, 18, YELLOW); y += 24; }
    if (f.lock.valid())          { DrawText(TextFormat("LOCKED: %s   [SPACE] release", TargetName(g, f.lock).c_str()), 20, y, 18, HUD); y += 24; }
    else if (f.selected.valid()) { DrawText(TextFormat("TARGET: %s   [SPACE] lock", TargetName(g, f.selected).c_str()), 20, y, 18, HUD_LABEL); y += 24; }
    else                         { DrawText("NO LOCK  -  flying free", 20, y, 18, HUD_DIM); y += 24; }
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
        if (p.passengers + p.steerage > 0.0 || p.spec.berths > 0.0) {      // who's aboard: in the cabins, and in the hold
            std::string who = TextFormat("PASSENGERS  %.0f / %.0f berths", p.passengers, p.spec.berths);
            if (p.steerage > 0.0) who += TextFormat("   +%.0f in the hold (%.0f t)", p.steerage, p.steerage * PERSON_HOLD);
            DrawText(who.c_str(), 20, cy - 22, 16, p.steerage > 0.0 ? YELLOW : HUD_LABEL);
        }
        DrawText(TextFormat("CARGO  %.1f / %.0f t", inHold, p.spec.hold), 20, cy, 18,
                 inHold >= p.spec.hold - 1e-6 ? ORANGE : HUD);
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
        float frac = (float)(p.fuel / p.spec.tank);
        Color fc = p.fuel <= 0.0 ? RED : frac < 0.2f ? ORANGE : HUD;
        DrawText(TextFormat("FUEL   %.1f / %.0f t", p.fuel, p.spec.tank), 20, top - 26, 18, fc);
        DrawRectangleLines(230, top - 24, 120, 12, HUD_DIM);
        DrawRectangle(232, top - 22, (int)(116 * frac), 8, fc);
        // (what the reactor is burning, and how much of the heat that makes is coming out as power in use)
        if (p.power.cold) DrawText(TextFormat("REACTOR DOWN: on the bank  [Z]"), 360, top - 26, 18, YELLOW);
        else if (p.power.restart > 0.0f) DrawText(TextFormat("LIGHTING THE REACTOR  %.0f s", p.power.restart), 360, top - 26, 18, YELLOW);
        else if (f.burn > 0.0) DrawText(TextFormat("-%.3f t/s  at %.0f%%", f.burn, f.efficiency * 100.0), 360, top - 26, 18, f.load > 0.9 * p.spec.reactorTW ? ORANGE : HUD_DIM);
        DrawText(TextFormat("CREDITS  %s cr", Thousands(p.credits).c_str()), 20, top - 50, 18, HUD);
        if (p.marked > 0.0) DrawText(TextFormat("MARKED  %s cr  (stolen: a fence at a clan haven or a free port takes them)", Thousands(p.marked).c_str()), 300, top - 50, 16, YELLOW);
        // Power: what the reactor is putting out, the two capacitors, and where spare output is going
        {
            int py = top - 76;
            auto bar = [&](int bx, const char *label, double frac, Color c) {
                DrawText(label, bx, py, 16, HUD_LABEL);
                int lw = MeasureText(label, 16) + 6;
                DrawRectangleLines(bx + lw, py + 2, 54, 11, HUD_DIM);
                DrawRectangle(bx + lw + 2, py + 4, (int)(50 * Clamp((float)frac, 0.0f, 1.0f)), 7, c);
                return bx + lw + 64;
            };
            double dc = p.power.driveCap / p.spec.driveCapTJ, bk = p.power.bank / p.spec.bankTJ, ld = f.load / p.spec.reactorTW;
            int bx = bar(20, "REACTOR", p.fuel > 0.0 ? ld : 0.0, ld > 0.9 ? ORANGE : HUD);
            bx = bar(bx, "BANK", bk, bk < 0.25 ? ORANGE : HUD);
            bool jump = p.power.driveCap >= JumpCharge(p.spec, p.Mass());          // (charge for a jump to warp?)
            bx = bar(bx, "DRIVE", dc, jump ? HUD : ORANGE);
            DrawText(TextFormat("%.1f jumps", p.power.driveCap / JumpCharge(p.spec, p.Mass())), bx - 4, py, 16, jump ? HUD_LABEL : ORANGE);
            bx += MeasureText("0.0 jumps", 16) + 8;
            DrawText(TextFormat("%s [X]", PowerModeName(p.power.mode)), bx, py, 16, HUD_LABEL);
            DrawText(TextFormat("MASS  %.0f t   0.99c in %.0f s   turn %.0f deg/s", p.Mass(), 7.02 * LIGHT_SEC_U / (THRUST_K * fmax(p.spec.thrust, 1e-9) / p.Mass()), p.spec.turn / p.Mass()),
                     20, py - 22, 16, p.Mass() > p.spec.hyperMaxMass ? ORANGE : HUD_LABEL);
            if (p.fuel <= 0.0) DrawText("OUT OF FUEL: emergency power  -  K: call a tanker", 20, py - 44, 16, RED);
        }
        int sy = top - (p.fuel <= 0.0 ? 146 : 124);
        if (f.scooping) { DrawText(TextFormat("FUEL SCOOPING  +%.1f t/s", p.spec.scoopRate), 20, sy, 18, ORANGE); sy -= 24; }
        if (f.skimming) DrawText(TextFormat("SKIMMING HELIUM-3  +%.2f t/s", p.spec.skimRate), 20, sy, 18, GoodData(RAW_HELIUM3).color);
        else if (f.skimFull) DrawText("SKIMMING HELIUM-3  -  HOLD FULL", 20, sy, 18, ORANGE);
    }

    // Contraband aboard, under the law of the nearest system: a warning up top for as long as it's in the hold
    if (const Faction *law = systems.factions.Get(systems[eye.anchor].ruler)) {
        std::string what;
        if (!(g.Papers(law->id) & PAPER_PERMIT))
            for (int good : law->contraband) if (p.cargo[good] > 0.05) what += (what.empty() ? "" : ", ") + std::string(GoodData(good).name);
        if (!what.empty()) {
            // (between the two columns of the HUD: two lines, the second cut short if it has to be)
            const char *head = "CONTRABAND ABOARD";
            std::string text = law->name + " bans " + what;
            int room = std::max(260, sw - 2 * 420);
            while (text.size() > 4 && MeasureText(text.c_str(), 16) > room - 24) text = text.substr(0, text.size() - 4) + "...";
            int tw = std::max(MeasureText(head, 18), MeasureText(text.c_str(), 16)), bx = sw / 2 - tw / 2 - 12;
            DrawRectangle(bx, 48, tw + 24, 50, Fade(BLACK, 0.8f));
            DrawRectangleLines(bx, 48, tw + 24, 50, RED);
            DrawText(head, sw / 2 - MeasureText(head, 18) / 2, 54, 18, RED);
            DrawText(text.c_str(), sw / 2 - MeasureText(text.c_str(), 16) / 2, 76, 16, RED);
        }
    }

    // --- Right column: navigation ---
    int x = sw - 400; y = 20;
    DVec3 galPos = systems.PositionLy(eye.anchor, eye.pos);
    double anchorRatio = FieldRatio(systems[eye.anchor], eye.pos);
    if (g.loaded.index >= 0 && anchorRatio < 1.5) {
        const StarSystem &s = systems[g.loaded.index];
        DVec3  sp = FromShip(systems, eye, g.loaded.index) * -1.0;
        double h = Dot(sp, s.n), r = Len(sp - s.n * h);
        DrawText(TextFormat("SYSTEM  %s", s.name.c_str()), x, y, 20, HUD);                              y += 24;
        if (const Faction *ruler = systems.factions.Get(s.ruler)) DrawText(ruler->name.c_str(), x, y, 16, ruler->color);
        else if (s.develop > 0.0f)                                DrawText("Unclaimed space", x, y, 16, HUD_DIM);
        else                                                      DrawText("Uncharted", x, y, 16, HUD_DIM);
        y += 20;
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
        y += 22; DrawText(p.spec.scoopRate > 0.0 && p.spec.fuelGood == REF_FUEL ? "Scoop near a star (within 3 radii) or buy fuel" : "Buy fuel, or call a tanker (K)", x, y, 16, ORANGE);
    }
    y += 22;

    // Sensors: is the radar on (it gives the ship away), how hot the ship is running, what's on the scope
    {
        bool silent = p.power.mode == POWER_SILENT, on = RadarOn(p.spec, p.power);
        DrawText(silent ? "RUNNING SILENT  -  radar off, reactor throttled" : on ? "RADAR ON  [G]" : "RADAR OFF  [G]  -  passive only",
                 x, y, 16, silent ? MARK_FIX_COLOR : on ? HUD_LABEL : YELLOW);
        y += 20;
        int held = 0, lost = 0, radars = 0, bursts = 0;
        for (const Traffic &t : g.traffic) (t.ghost ? lost : held)++;
        for (const SensorMark &m : g.marks) (m.kind == MARK_RADAR ? radars : bursts)++;
        // (the heat an infrared array sees, and how far off a basic one sees it)
        DrawText(TextFormat("HEAT %.1f TW  -  seen at %.2f AU", p.power.heat, IrRange(1.0, p.power.heat) / AU_U), x, y, 16, HUD_DIM);
        y += 20;
        std::string scope = TextFormat("CONTACTS %d", held);
        if (lost) scope += TextFormat("  (%d lost)", lost);
        DrawText(scope.c_str(), x, y, 16, HUD_DIM);
        y += 20;
        if (radars) { DrawText(TextFormat("RADAR WARNING  -  %d sweeping you", radars), x, y, 16, MARK_RADAR_COLOR); y += 20; }
        if (bursts) { DrawText(TextFormat("TACHYON BEARINGS  %d", bursts), x, y, 16, MARK_TACHYON_COLOR); y += 20; }
        if (!g.trafficControl.empty()) { DrawText(("TRAFFIC CONTROL  " + g.trafficControl).c_str(), x, y, 16, HUD_LABEL); y += 20; }
        y += 2;
    }

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

    // Comms in flight: one line per reply on its way, with a blip that goes out to them and comes back
    // (on the left, under the ship's readouts, where the comms window goes when it's open: it shows the same itself)
    if (!commsOpen) {
        std::vector<CommsFlight> flights = CommsInFlight(g, g);
        if (!flights.empty()) {
            const int SHOWN = 4, BAR = 110;
            int x = 20, y = 250;
            DrawRectangle(x - 6, y - 6, 400, 30 + 20 * std::min((int)flights.size(), SHOWN) + ((int)flights.size() > SHOWN ? 18 : 0), Fade(BLACK, 0.6f));
            DrawText(TextFormat("COMMS  -  %d in flight", (int)flights.size()), x, y, 16, HUD_LABEL);
            y += 22;
            for (int i = 0; i < (int)flights.size() && i < SHOWN; i++, y += 20) {
                const CommsFlight &fl = flights[i];
                float p = Clamp((float)fl.Progress(g.clock), 0.0f, 1.0f);
                bool  out = p < 0.5f;                                 // our request is still on its way there
                std::string name = fl.name;
                while (name.size() > 4 && MeasureText(name.c_str(), 16) > 170) name = name.substr(0, name.size() - 4) + "...";
                DrawText(name.c_str(), x, y, 16, out ? HUD_LABEL : HUD);
                int bx = x + 180, by = y + 8;
                DrawLine(bx, by, bx + BAR, by, HUD_DIM);
                DrawRectangle(bx - 2, by - 4, 3, 9, HUD);                                  // us
                DrawRectangle(bx + BAR, by - 4, 3, 9, HUD_DIM);                            // them
                float at = out ? p * 2.0f : 2.0f - p * 2.0f;                               // 0 here .. 1 there
                int   mx = bx + (int)(at * BAR);
                if (out) DrawTriangle({ mx + 5.0f, (float)by }, { mx - 3.0f, by - 4.0f }, { mx - 3.0f, by + 4.0f }, HUD_LABEL);
                else     DrawTriangle({ mx - 5.0f, (float)by }, { mx + 3.0f, by + 4.0f }, { mx + 3.0f, by - 4.0f }, HUD);
                DrawText(FormatETA(fl.at - g.clock).c_str(), bx + BAR + 12, y, 16, out ? LIGHTGRAY : HUD);
            }
            if ((int)flights.size() > SHOWN) { DrawText(TextFormat("+ %d more", (int)flights.size() - SHOWN), x, y, 14, HUD_DIM); y += 18; }
        }
    }

    // Attitude: an artificial horizon against the system's ecliptic, or the galactic plane between the stars
    {
        bool inSystem = g.loaded.index >= 0 && anchorRatio < 1.5;
        float pitch, roll;
        PlaneAttitude(eye, inSystem ? systems[g.loaded.index].n : DVec3{ 0, 1, 0 }, pitch, roll);
        const float R = 72.0f;
        Vector2 c = { sw - 104.0f, sh - 196.0f };
        // The horizon, as it would look out of the windscreen: roll right and it tips up on the right; nose up and it drops
        float   a = -roll * DEG2RAD, off = Clamp(pitch / 90.0f, -1.0f, 1.0f) * R;
        Vector2 along = { cosf(a), sinf(a) }, down = { -sinf(a), cosf(a) };
        Vector2 mid = { c.x + down.x * off, c.y + down.y * off };
        float   half = sqrtf(fmaxf(0.0f, R * R - off * off));
        DrawCircleLinesV(c, R, HUD_DIM);
        DrawLineEx({ mid.x - along.x * half, mid.y - along.y * half }, { mid.x + along.x * half, mid.y + along.y * half }, 2.0f, HUD);
        DrawLineV(mid, { mid.x - down.x * 10.0f, mid.y - down.y * 10.0f }, HUD);            // a tick on the 'up' side
        DrawLineEx({ c.x - 26, c.y }, { c.x - 9, c.y }, 2.0f, WHITE); DrawLineEx({ c.x + 9, c.y }, { c.x + 26, c.y }, 2.0f, WHITE);   // the ship
        DrawCircleV(c, 2.5f, WHITE);
        const char *plane = inSystem ? "ECLIPTIC" : "GALACTIC PLANE";
        DrawText(plane, (int)(c.x - MeasureText(plane, 16) / 2), (int)(c.y - R - 24), 16, HUD_LABEL);
        const char *p = TextFormat("PITCH %+.0f", pitch);
        DrawText(p, (int)(c.x - MeasureText(p, 16) / 2), (int)(c.y + R + 8), 16, HUD);
        const char *r = TextFormat("ROLL %+.0f", roll);                  // (TextFormat reuses its buffers: one at a time)
        DrawText(r, (int)(c.x - MeasureText(r, 16) / 2), (int)(c.y + R + 28), 16, HUD);
    }

    // The ship's name, and which galaxy this is (the seed, to pass on to a friend)
    {
        std::string id = (g.shipName.empty() ? std::string() : g.shipName + "   -   ") + TextFormat("galaxy %u", g.seed);
        DrawText(id.c_str(), sw / 2 - MeasureText(id.c_str(), 14) / 2, 30, 14, HUD_DIM);
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
        std::string pr = b.settlement == Settlement::SensorOutpost ? b.name + "  -  military installation: no docking"
                       : b.population <= 0 ? b.name + "  -  uninhabited"
                       : CanTradeOverComms(g)  ? "[F] Comms with " + b.name + "  -  3 to trade"
                                               : "[F] Hail " + b.name + " to trade  -  " + SettlementName(b.settlement);
        int w = MeasureText(pr.c_str(), 20);
        DrawRectangle(sw / 2 - w / 2 - 10, sh - 104, w + 20, 30, Fade(BLACK, 0.7f));
        DrawText(pr.c_str(), sw / 2 - w / 2, sh - 99, 20, b.population > 0 ? HUD : GRAY);
    }

    if (showHelp) {
        const char *help = online ? "MOUSE aim  W/S throttle (both: stop)  A/D yaw  Q/E roll  SHIFT hyper/warp  CAPS damp  ALT look  TAB mouse  X power  Z reactor  G radar  Y board  SPACE/MMB lock  M map  R factions  T trade log  N news  J contracts  "
                                    "I info  F hail / comms  LMB mine  ESC menu"
                                  : "MOUSE aim  W/S throttle (both: stop)  A/D yaw  Q/E roll  SHIFT hyper/warp  CAPS damp  ALT look  TAB mouse  X power  Z reactor  G radar  Y board  SPACE/MMB lock  M map  R factions  T trade log  N news  J contracts  "
                                    "I info  F hail / comms  LMB mine  F5/F9 quicksave/load  ESC menu";
        int size = 16;
        while (size > 10 && MeasureText(help, size) > sw - 40) size--;          // shrink to fit narrow windows
        DrawText(help, 20, sh - 14 - size, size, HUD_DIM);
    }
}
