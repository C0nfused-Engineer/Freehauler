// worldui.cpp - a world's economy page: its people, its labour and power, and each of its industries.
//
// Opened over comms (6 on an open channel). It shows the world as the captain last saw it: brought up to date
// whenever they're docked there, or ask over an open channel. Docked, ENTER funds the next addition to the
// industry picked out, for a share of what it earns from then on.
#include "ui.h"
#include "comms.h"
#include "economy.h"
#include "format.h"
#include "theme.h"
#include <algorithm>

class WorldWindow : public Window {
public:
    explicit WorldWindow(TargetId body) : body(body) {}
    WindowKind Kind() const override { return WindowKind::World; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }
    bool StillValid(const GameState &g) const override { return body.system == g.loaded.index && body.index >= 0 && body.index < (int)g.loaded.bodies.size(); }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_SIX) || in.Pressed(KEY_KP_6)) { closed = true; return; }
        const Body &b = g.loaded.bodies[body.index];
        refresh -= in.dt;
        if (refresh <= 0.0f) { refresh = 1.0f; ui.Do(g, { Command::WorldReport, body.index }); }       // (kept current while it's open)
        auto it = g.know.worlds.find(EconomyBody(g, b));
        std::vector<int> shown = it == g.know.worlds.end() ? std::vector<int>() : Shown(it->second);
        int n = (int)shown.size();
        if (n == 0) return;
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = std::max(0, row - 1);
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = std::min(n - 1, row + 1);
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        top = std::max(std::min(top, row), row - VISIBLE + 1);
        top = std::max(0, std::min(top, n - VISIBLE));
        if (in.Pressed(KEY_TAB)) { unbuilt = !unbuilt; row = top = 0; }
        if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) {
            if (g.flight.nearBody < 0 || EconomyBody(g, g.loaded.bodies[g.flight.nearBody]) != EconomyBody(g, b)) ui.Say("Dock at " + b.name + " to put money into its industry");
            else { ui.Do(g, { Command::Invest, shown[row] }); refresh = 0.1f; }
        }
    }

    void Draw(const GameState &g) const override
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { sw / 2 - 600, sh / 2 - 320, 1200, 640 };
        int px = (int)panel.x, py = (int)panel.y, pw = (int)panel.width;
        DrawRectangleRec(panel, Fade(BLACK, 0.94f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        const Body &b = g.loaded.bodies[body.index];
        auto it = g.know.worlds.find(EconomyBody(g, b));
        if (it == g.know.worlds.end()) {
            DrawText(("ECONOMY  -  " + b.name).c_str(), px + 20, py + 14, 24, WHITE);
            DrawText("Nothing on record yet: wait for them to answer, or dock.", px + 30, py + 70, 18, LIGHTGRAY);
            return;
        }
        const WorldRecord &r = it->second;
        DrawText(("ECONOMY  -  " + r.name).c_str(), px + 20, py + 14, 24, WHITE);
        double age = g.clock - r.at;
        const char *asOf = age < 2.0 ? "as of now" : TextFormat("as of %s ago", FormatETA(age).c_str());
        DrawText(asOf, px + pw - 24 - MeasureText(asOf, 16), py + 20, 16, age < 2.0 ? HUD_LABEL : YELLOW);
        // Its people, and what they have to work with
        double idle = r.labour > 0.0f ? 1.0 - r.employed / r.labour : 0.0, housed = r.pop > 0.0 ? r.housing / r.pop : 0.0;
        // (their numbers, and which way they're going: a market day is ten minutes)
        std::string people = "PEOPLE  " + FormatPopulation(r.pop);
        if (fabsf(r.trend) >= 0.0005f) people += TextFormat("  %+.1f%% a day", r.trend * 100.0f);
        DrawText(people.c_str(), px + 24, py + 52, 18, r.trend < -0.0005f ? ORANGE : HUD);
        DrawText(TextFormat("LIVING STANDARD  %.0f%%", r.living * 100.0f), px + 330, py + 52, 18, r.living < LEAVE_BELOW ? ORANGE : r.living < GROW_ABOVE ? YELLOW : HUD);
        DrawText(TextFormat("LABOUR  %.0f%% in work", 100.0 * (1.0 - idle)), px + 600, py + 52, 18, idle > 0.35 ? YELLOW : HUD);
        DrawText(TextFormat("HOUSING  for %.0f%%", 100.0 * housed), px + 850, py + 52, 18, housed < 0.98 ? ORANGE : HUD);
        DrawText(TextFormat("POWER  %.0f made, %.0f used  (cells a day)", r.cellsMade, r.cellsUsed), px + 24, py + 76, 18,
                 r.cellsMade < 0.9f * r.cellsUsed ? YELLOW : HUD);
        // Who's coming and going, and how the border stands
        static const char *borders[] = { "open", "selective", "closed" };
        std::string moving = TextFormat("BORDER %s", borders[r.border <= BORDER_CLOSED ? r.border : 0]);
        if (r.leaving >= 1.0f) moving += TextFormat("   %s a day leaving", Thousands(r.leaving).c_str()) + (r.goingTo.empty() ? std::string() : ", mostly for " + r.goingTo);
        if (r.stuck >= 1.0f)   moving += TextFormat("   %s a day would go but can't", Thousands(r.stuck).c_str()) + (r.barredAt.empty() ? std::string() : ": " + r.barredAt + " turns them away");
        if (r.refugees >= 1.0f) moving += TextFormat("   %s newly arrived", Thousands(r.refugees).c_str());
        while (moving.size() > 4 && MeasureText(moving.c_str(), 16) > 720) moving = moving.substr(0, moving.size() - 4) + "...";
        DrawText(moving.c_str(), px + 440, py + 78, 16, r.stuck >= 1.0f ? ORANGE : HUD_LABEL);
        // The ships based in its system, and whether there are enough of them for what there is to carry
        {
            float booked = fmaxf(r.busy[ROLE_BULK], r.busy[ROLE_TANKER]);
            // (ships work more than one system: a small place has the use of part of one)
            auto ships = [](float n) { return n >= 10.0f ? TextFormat("%.0f", n) : TextFormat("%.1f", n); };
            std::string based = std::string("SHIPPING  ") + ships(r.fleet[ROLE_BULK]) + " freighters  ";
            based += std::string(ships(r.fleet[ROLE_TANKER])) + " tankers  ";
            based += std::string(ships(r.fleet[ROLE_HAULER])) + " local haulers  ";
            based += std::string(ships(r.fleet[ROLE_LINER])) + " liners";
            DrawText(based.c_str(),
                     px + 24, py + 100, 18, HUD);
            const char *say = booked > 1.05f ? TextFormat("more cargo than ships: %.0f%% of what they can carry", booked * 100.0f)
                            : TextFormat("holds %.0f%% full", booked * 100.0f);
            DrawText(say, px + 640, py + 102, 16, booked > 1.5f ? ORANGE : booked > 1.05f ? YELLOW : HUD_LABEL);
        }
        DrawLine(px + 18, py + 126, px + pw - 18, py + 126, HUD_DIM);
        const int cx[] = { px + 24, px + 250, px + 440, px + 510, px + 610, px + 700, px + 810, px + 930, px + 1050 };
        const char *heads[] = { unbuilt ? "COULD BE STARTED HERE" : "INDUSTRY", "RUN BY", "SITE", "CAPACITY", "RUNNING", "OUTPUT t/d", "PROFIT cr/d", "YOUR STAKE", "TO FUND" };
        for (int c = 0; c < 9; c++) DrawText(heads[c], cx[c], py + 134, 14, HUD_LABEL);
        std::vector<int> shown = Shown(r);
        int n = (int)shown.size(), sel = std::min(row, n - 1);
        for (int i = top; i < n && i < top + VISIBLE; i++) {
            const IndustryRow &in = r.rows[shown[i]];
            int y = py + 156 + (i - top) * 26;
            if (i == sel) DrawRectangle(px + 18, y - 3, pw - 36, 26, Fade(HUD, 0.18f));
            Color text = in.cap > 0.0f ? WHITE : LIGHTGRAY;
            DrawText(IndustryData(in.type).name, cx[0], y, 18, text);
            const Faction *owner = g.systems.factions.Get(in.owner);
            std::string who = owner ? owner->name : std::string("Its own people");
            while (who.size() > 4 && MeasureText(who.c_str(), 16) > 180) who = who.substr(0, who.size() - 4) + "...";
            DrawText(who.c_str(), cx[1], y + 1, 16, owner ? owner->color : LIGHTGRAY);
            DrawText(TextFormat("%.0f%%", in.site * 100.0f), cx[2], y + 1, 16, in.site >= 1.0f ? HUD : in.site < 0.6f ? ORANGE : LIGHTGRAY);
            if (in.cap > 0.0f) {
                DrawText(in.building > 0.0f ? TextFormat("%.0f +%.0f", in.cap, in.building) : TextFormat("%.0f", in.cap), cx[3], y + 1, 16, in.building > 0.0f ? HUD : text);
                // What's holding it back, if something is
                std::string why;
                if (in.shortOf & SHORT_LABOUR)    why += " labour";
                if (in.shortOf & SHORT_CELLS)     why += " power";
                if (in.shortOf & SHORT_MATERIALS) why += " materials";
                if ((in.shortOf & SHORT_DEMAND) && why.empty()) why = " no demand";
                DrawText(TextFormat("%.0f%%", in.run * 100.0f), cx[4], y + 1, 16, in.run > 0.9f ? HUD : in.run < 0.5f ? ORANGE : YELLOW);
                if (!IsCivic(in.type)) {
                    DrawText(TextFormat("%.1f", in.output), cx[5], y + 1, 16, text);
                    DrawText(Thousands(in.profit).c_str(), cx[6], y + 1, 16, in.profit < 0.0f ? ORANGE : text);
                }
                if (!why.empty()) DrawText(("short:" + why).c_str(), cx[8] + 70, y + 3, 12, ORANGE);
            } else if (in.building > 0.0f) {
                DrawText(TextFormat("+%.0f in %.1f d", in.building, in.buildLeft), cx[3], y + 1, 16, HUD);
            } else DrawText(IndustryData(in.type).site, cx[3], y + 2, 14, HUD_DIM);
            if (in.stake > 0.0f) DrawText(TextFormat("%.0f%%  +%s cr", 100.0f * in.stake / fmaxf(in.cap + in.building, in.stake), Thousands(in.pending).c_str()), cx[7], y + 1, 16, HUD);
            if (in.cost > 0.0f && (in.cap > 0.0f || unbuilt)) DrawText(TextFormat("%s", Thousands(in.cost).c_str()), cx[8], y + 1, 16, in.cost <= g.player.credits ? HUD : GRAY);
        }
        if (top > 0)           DrawText("^ more", px + pw - 90, py + 134, 14, HUD_DIM);
        if (top + VISIBLE < n) DrawText("v more", px + pw - 90, py + 156 + VISIBLE * 26, 14, HUD_DIM);
        // The one picked out, in words
        if (n > 0) {
            const IndustryRow &in = r.rows[shown[sel]];
            int y = py + (int)panel.height - 92;
            DrawLine(px + 18, y - 8, px + pw - 18, y - 8, HUD_DIM);
            const IndustryInfo &info = IndustryData(in.type);
            DrawText(TextFormat("%s  -  a good site: %s", info.name, info.site), px + 24, y, 16, LIGHTGRAY);
            std::string funding = in.type == IND_SERVICES ? std::string("Services grow and shrink with the people there are: nobody builds them.")
                                : in.type == IND_CITY ? std::string("Cities are their owner's to build.")
                                : in.cost > 0.0f ? TextFormat("Funding adds %.1f capacity in %.0f days for %s cr, and that much of it is yours: its share of the profit waits for you here.",
                                                              in.step, UPGRADE_DAYS, Thousands(in.cost).c_str())
                                : std::string("Nothing can be added now: it takes spare labour to run it, steel and machinery in the market to build it, and nothing else being built.");
            DrawText(funding.c_str(), px + 24, y + 22, 16, in.cost > 0.0f ? HUD_LABEL : GRAY);
        }
        DrawText(TextFormat("UP/DOWN pick   ENTER fund it (docked)   TAB %s   ESC close        CREDITS %s", unbuilt ? "what's here" : "what could be started",
                            Thousands(g.player.credits).c_str()), px + 24, py + (int)panel.height - 30, 16, HUD_DIM);
    }

private:
    static const int VISIBLE = 14;
    TargetId body;
    int   row = 0, top = 0;
    bool  unbuilt = false;                       // showing what could be started here, not what's running
    float refresh = 0.0f;

    // The rows of the record this page is showing: the industries it has (or is building), or the ones it could have
    std::vector<int> Shown(const WorldRecord &r) const
    {
        std::vector<int> out;
        for (int i = 0; i < (int)r.rows.size(); i++) if ((r.rows[i].cap > 0.0f || r.rows[i].building > 0.0f) != unbuilt) out.push_back(i);
        return out;
    }
};

std::unique_ptr<Window> MakeWorldWindow(TargetId body) { return std::make_unique<WorldWindow>(body); }
