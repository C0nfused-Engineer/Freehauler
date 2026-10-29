// shipyardui.cpp - fitting out, in dock at a shipyard or starbase.
// On the left, the ship: its hull, what's fitted, and what that adds up to.
// On the right, what the yard sells: a page for each kind of part (A/D turns
// the page), and a last page for hulls and hull work.
#include "ui.h"
#include "economy.h"
#include "format.h"
#include "shipyard.h"
#include "partsdata.h"
#include "theme.h"
#include "sensors.h"
#include <algorithm>

class OutfitWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::Outfit; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }
    bool StillValid(const GameState &g) const override { return Shipyard(g) != nullptr; }

    static const int PAGES = (int)PartType::Count + 1;             // (the last one: hulls)

    // What the yard offers on the page: for parts, every size in every grade it sells
    struct Offer { Part part; int hull = -1; int work = -1; };     // a part; or a hull; or hull work (0 room, 1 a hardpoint)
    std::vector<Offer> Offers(const GameState &g, const Body &yard) const
    {
        std::vector<Offer> out;
        if (page == PAGES - 1) {
            for (int h = 0; h < HULL_COUNT; h++) { Offer o; o.hull = h; out.push_back(o); }
            for (int k = 0; k < 2; k++) { Offer o; o.work = k; out.push_back(o); }
            return out;
        }
        PartType type = (PartType)page;
        int best = YardGrade(g, g, yard, type);
        for (int size = 1; size <= PART_SIZES; size++)
            for (int grade = 0; grade <= best; grade++) {
                Offer o;
                o.part.type = type; o.part.size = (uint8_t)size; o.part.grade = (uint8_t)grade;
                o.part.variant = (uint8_t)(model[page] < ModelCount(type) ? model[page] : 0);
                o.part.maker = (int16_t)YardMaker(g, yard, type);
                out.push_back(o);
            }
        return out;
    }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_F)) { closed = true; return; }
        const Body *yard = Shipyard(g);
        if (!yard) return;
        if (in.Pressed(KEY_TAB)) shop = !shop;
        if (in.Pressed(KEY_D) || in.Pressed(KEY_RIGHT)) { page = (page + 1) % PAGES; row[1] = top[1] = 0; shop = true; }
        if (in.Pressed(KEY_A) || in.Pressed(KEY_LEFT))  { page = (page + PAGES - 1) % PAGES; row[1] = top[1] = 0; shop = true; }
        if ((in.Pressed(KEY_Q) || in.Pressed(KEY_E)) && page < PAGES - 1) {            // (the next model of this kind of part)
            int kinds = std::max(1, ModelCount((PartType)page));
            model[page] = (model[page] + (in.Pressed(KEY_E) ? 1 : kinds - 1)) % kinds;
        }
        std::vector<Offer> offers = Offers(g, *yard);
        int n = shop ? (int)offers.size() : (int)g.player.build.parts.size();
        int &r = row[shop ? 1 : 0], &t = top[shop ? 1 : 0];
        if (n > 0) {
            if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) r = (r + n - 1) % n;
            if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) r = (r + 1) % n;
            r = std::max(0, std::min(n - 1, r + in.WheelStep()));
            t = std::max(std::min(t, r), r - VISIBLE + 1);
            t = std::max(0, std::min(t, n - VISIBLE));
        }
        if (n > 0 && (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) && shop) {
            const Offer &o = offers[r];
            if (o.hull >= 0)      ui.Do(g, { Command::BuyHull, o.hull });
            else if (o.work >= 0) ui.Do(g, { Command::StretchHull, o.work });
            else                  ui.Do(g, { Command::FitPart, PackPart(o.part) });
        }
        if (n > 0 && in.Pressed(KEY_BACKSPACE) && !shop) {
            ui.Do(g, { Command::RemovePart, r });
            r = std::max(0, std::min(r, n - 2));
        }
    }

    void Draw(const GameState &g) const override
    {
        const Body *yard = Shipyard(g);
        if (!yard) return;
        const FactionSet &fs = g.systems.factions;
        const Player &p = g.player;
        const ShipBuild &b = p.build;
        const ShipSpec &s = p.spec;
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { 20, 20, sw - 40, sh - 40 };
        int px = (int)panel.x, py = (int)panel.y, pw = (int)panel.width, ph = (int)panel.height;
        DrawRectangleRec(panel, Fade(BLACK, 0.97f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        DrawText(TextFormat("SHIPYARD  -  %s", yard->name.c_str()), px + 20, py + 12, 24, WHITE);
        std::string purse = TextFormat("CREDITS  %s cr", Thousands(p.credits).c_str());
        DrawText(purse.c_str(), px + pw - 20 - MeasureText(purse.c_str(), 20), py + 14, 20, HUD);
        DrawLine(px + 18, py + 46, px + pw - 18, py + 46, HUD_DIM);
        int half = px + pw / 2;
        DrawLine(half - 8, py + 54, half - 8, py + ph - 150, HUD_DIM);

        // --- The ship ---
        int x = px + 24, y = py + 56;
        double used = VolumeUsed(b), room = BuildVolume(b);
        DrawText(TextFormat("YOUR SHIP  -  %s", Hull(b.hull).name), x, y, 18, shop ? HUD_LABEL : WHITE); y += 24;
        DrawText(TextFormat("room %.0f / %.0f m3     hardpoints %d / %d     %.0f t dry", used, room, HardpointsUsed(b), BuildHardpoints(b), s.dryMass),
                 x, y, 16, used > room ? ORANGE : LIGHTGRAY); y += 24;
        int n = (int)b.parts.size(), sel = std::min(row[0], n - 1), colW = half - x - 24;
        for (int i = top[0]; i < n && i < top[0] + VISIBLE; i++) {
            PartInfo info = Describe(b.parts[i], &fs);
            int ry = y + (i - top[0]) * ROW_H;
            if (i == sel && !shop) DrawRectangle(x - 6, ry - 2, colW + 12, ROW_H, Fade(HUD, 0.18f));
            DrawText(info.name.c_str(), x, ry, 16, WHITE);
            std::string what = info.does + TextFormat("   %.0f m3, %.1f t%s", info.volume, info.mass, info.external ? ", hardpoint" : "");
            while (what.size() > 4 && MeasureText(what.c_str(), 14) > colW) what = what.substr(0, what.size() - 4) + "...";
            DrawText(what.c_str(), x + 10, ry + 17, 14, HUD_LABEL);
        }
        if (top[0] > 0)           DrawText("^ more", half - 80, y - 20, 14, HUD_DIM);
        if (top[0] + VISIBLE < n) DrawText("v more", half - 80, y + VISIBLE * ROW_H - 6, 14, HUD_DIM);

        // --- The yard ---
        x = half + 8; y = py + 56;
        std::vector<Offer> offers = Offers(g, *yard);
        const char *title = page == PAGES - 1 ? "Hulls and hull work" : PartTypeName((PartType)page);
        std::string head = std::string("<  ") + title + "  >";
        DrawText(head.c_str(), x, y, 18, shop ? WHITE : HUD_LABEL);
        if (page < PAGES - 1 && ModelCount((PartType)page) > 1) {
            const PartModel &md = PartModelOf((PartType)page, model[page]);
            DrawText(TextFormat("%s   [Q/E]  %d of %d", md.name.c_str(), model[page] + 1, ModelCount((PartType)page)), x + 260, y + 2, 16,
                     page == (int)PartType::Reactor ? GoodData(md.fuel).color : HUD);
        }
        y += 24;
        if (page < PAGES - 1) {
            int maker = YardMaker(g, *yard, (PartType)page), best = YardGrade(g, g, *yard, (PartType)page);
            std::string note = maker != NO_FACTION ? fs.Name(maker) + "'s own line: up to " + GradeName(best) + " grade here" : std::string("up to ") + GradeName(best) + " grade here";
            DrawText(note.c_str(), x, y, 14, maker != NO_FACTION && fs.Get(maker) ? fs.Get(maker)->color : HUD_LABEL);
        } else {
            DrawText("What's fitted moves across to a new hull, and the old one is traded in.", x, y, 14, HUD_LABEL);
        }
        y += 24;
        n = (int)offers.size(); sel = std::min(row[1], n - 1); colW = px + pw - 24 - x;
        for (int i = top[1]; i < n && i < top[1] + VISIBLE; i++) {
            const Offer &o = offers[i];
            int ry = y + (i - top[1]) * ROW_H;
            if (i == sel && shop) DrawRectangle(x - 6, ry - 2, colW + 12, ROW_H, Fade(HUD, 0.18f));
            std::string name, what, why;
            double price = 0.0;
            if (o.hull >= 0) {
                const HullDef &h = Hull(o.hull);
                name = h.name; what = TextFormat("%.0f m3, %d hardpoints, %.0f t  -  %s", h.volume, h.hardpoints, h.mass, h.blurb);
                price = HullPrice(g, g, *yard, o.hull);
                if (o.hull == b.hull) why = "yours";
                else {
                    ShipBuild next = b; next.hull = o.hull; next.stretch = next.mounts = 0;
                    if (VolumeUsed(next) > BuildVolume(next) + 1e-6 || HardpointsUsed(next) > BuildHardpoints(next)) why = "too small for what's fitted";
                }
            } else if (o.work >= 0) {
                bool mount = o.work == 1;
                name = mount ? "Weld on another hardpoint" : "Stretch the hull: 12% more room";
                what = TextFormat("done %d of %d times; each costs more than the last", mount ? b.mounts : b.stretch, MAX_STRETCH);
                price = StretchCost(b, mount);
                if ((mount ? b.mounts : b.stretch) >= MAX_STRETCH) why = "no more: get a bigger hull";
            } else {
                PartInfo info = Describe(o.part, &fs);
                name = info.name;
                what = info.does + TextFormat("   %.0f m3, %.1f t%s", info.volume, info.mass, info.external ? ", hardpoint" : "");
                price = PartPrice(g, g, *yard, o.part);
                why = FitProblem(b, o.part);
                if (why.empty() && price < 0.0) why = std::string("yard is out of ") + GoodData(info.material).name;
                if (why.size() > 26) why = why.substr(0, why.find(':') != std::string::npos ? why.find(':') : 26);
            }
            bool can = why.empty() && price <= p.credits;
            std::string cost = why == "yours" ? std::string("yours") : !why.empty() ? why : (price < 0.0 ? "+" : "") + Thousands(fabs(price)) + " cr";
            DrawText(name.c_str(), x, ry, 16, can ? WHITE : GRAY);
            DrawText(cost.c_str(), x + colW - MeasureText(cost.c_str(), 16), ry, 16, !why.empty() ? (why == "yours" ? HUD : ORANGE) : can ? HUD : GRAY);
            while (what.size() > 4 && MeasureText(what.c_str(), 14) > colW) what = what.substr(0, what.size() - 4) + "...";
            DrawText(what.c_str(), x + 10, ry + 17, 14, HUD_LABEL);
        }
        if (top[1] > 0)           DrawText("^ more", px + pw - 90, y - 20, 14, HUD_DIM);
        if (top[1] + VISIBLE < n) DrawText("v more", px + pw - 90, y + VISIBLE * ROW_H - 6, 14, HUD_DIM);

        // --- What it all adds up to ---
        int by = py + ph - 164;
        DrawLine(px + 18, by - 6, px + pw - 18, by - 6, HUD_DIM);
        double mass = p.Mass(), full = s.dryMass + s.tank + s.hold;
        auto to99 = [&](double m) { return s.thrust > 0.0 ? 7.02 * LIGHT_SEC_U / (THRUST_K * s.thrust / m) : 0.0; };
        auto line = [&](int lx, int ly, const std::string &text, Color c = LIGHTGRAY) { DrawText(text.c_str(), lx, ly, 16, c); };
        int c1 = px + 24, c2 = px + pw / 3 + 10, c3 = px + 2 * pw / 3;
        line(c1, by, TextFormat("MASS  %.0f t now, %.0f t full", mass, full));
        line(c1, by + 22, s.thrust > 0.0 ? TextFormat("0.99 c in %.0f s  (%.0f s full)", to99(mass), to99(full)) : std::string("NO ENGINES"), s.thrust > 0.0 ? LIGHTGRAY : ORANGE);
        line(c1, by + 44, TextFormat("TURN  %.0f deg/s  (%.0f full)", s.turn / mass, s.turn / full), s.turn > 0.0 ? LIGHTGRAY : ORANGE);
        line(c1, by + 66, TextFormat("TANK %.0f t %s   HOLD %.0f t", s.tank, GoodData(s.fuelGood).name, s.hold));
        line(c2, by, s.reactorTW > 0.0 ? TextFormat("REACTOR  %.0f TW at %.0f%%  (idling: %.0f TW, %.3f t/s)", s.reactorTW, s.reactorEff * 100.0, s.idleTW, ReactorBurnFor(s, s.idleTW)) : std::string("NO REACTOR"),
             s.reactorTW > 0.0 ? LIGHTGRAY : ORANGE);
        double burn = s.reactorBurn;
        line(c2, by + 22, TextFormat("flat out it burns %.2f t/s: %.0f min on a tank", burn, burn > 0.0 ? s.tank / burn / 60.0 : 0.0));
        line(c2, by + 44, TextFormat("CAPACITORS  bank %.0f TJ, drive %.0f TJ", s.bankTJ, s.driveCapTJ));
        line(c2, by + 66, TextFormat("GEAR  laser %.1f t/s  scoop %.1f  skim %.2f  berths %.0f", s.laserRate, s.scoopRate, s.skimRate, s.berths));
        // (how far each sensor finds a hauler going about its business; and how far off this ship is found the same way)
        line(c1, by + 88, TextFormat("SENSORS  radar %.2f AU   infrared %.2f AU   telescope %.2f AU   tachyon detector %.0f m3 at %.0f%%      SEEN  by radar at %.2f AU, by eye at %.2f AU",
                                     RadarRange(s.radar, 1.0) / AU_U, IrRange(s.ir, HEAT_REF) / AU_U, ScopeRange(s.scan, 1.0) / AU_U, s.tachArea * s.tachDepth, s.tachEff * 100.0,
                                     RadarRange(1.0, s.hullSize) / AU_U, ScopeRange(1.0, s.hullSize) / AU_U));
        double spare = s.reactorTW - s.idleTW;
        if (s.hyperSpeed > 0.0) {
            double need = DrivePower(s, s.hyperSpeed, full, false);
            line(c3, by, TextFormat("HYPER  %.0f c, %.0f TW full%s", s.hyperSpeed / LIGHT_SEC_U, need, full > s.hyperMaxMass ? "  - TOO HEAVY" : need > spare ? "  - short of power" : ""),
                 full > s.hyperMaxMass || need > spare ? ORANGE : LIGHTGRAY);
        } else line(c3, by, "NO HYPER DRIVE", ORANGE);
        if (s.warpSpeed > 0.0) {
            double need = DrivePower(s, s.warpSpeed, full, true), jump = JumpCharge(s, full);
            line(c3, by + 22, TextFormat("WARP  %.1fM c, %.0f TW full%s", s.warpSpeed / LIGHT_SEC_U / 1e6, need, full > s.warpMaxMass ? "  - TOO HEAVY" : need > spare ? "  - short of power" : ""),
                 full > s.warpMaxMass || need > spare ? ORANGE : LIGHTGRAY);
            line(c3, by + 44, TextFormat("a jump takes %.0f TJ full: %.1f to a charge", jump, jump > 0.0 ? s.driveCapTJ / jump : 0.0), s.driveCapTJ >= jump ? LIGHTGRAY : ORANGE);
            double perLy = ReactorBurnFor(s, fmin(need, spare) + s.idleTW) * LY_U / fmin(s.warpSpeed, fmax(1.0, DriveSpeedFor(s, spare, full, true)));
            line(c3, by + 66, TextFormat("about %.2f t of fuel a light year, full", perLy));
        } else line(c3, by + 22, "NO WARP DRIVE: this ship stays in its system", ORANGE);
        DrawText("TAB: your ship / the yard   A/D: another kind of part   W/S or wheel: pick   ENTER: buy and fit   BACKSPACE: take out and sell   F / ESC: close",
                 px + 20, py + ph - 26, 16, HUD_DIM);
    }

private:
    static const int ROW_H = 38, VISIBLE = 12;
    bool shop = true;                // which list the keys work on: the yard's (true) or what's fitted
    int  page = 0;                   // a PartType, or the hulls page
    int  model[PAGES] = {};          // which model of each kind of part is on show (Q/E): the fuel a reactor burns, the kind of drive...
    int  row[2] = { 0, 0 }, top[2] = { 0, 0 };
};

std::unique_ptr<Window> MakeOutfitWindow() { return std::make_unique<OutfitWindow>(); }
