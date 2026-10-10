// factionui.cpp - the factions window (R): who's who in human space, what they
// want, how they get on with each other, and where you stand with them
#include "ui.h"
#include "contracts.h"
#include "format.h"
#include "theme.h"
#include <algorithm>

class FactionsWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::Factions; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_R)) { closed = true; return; }
        const FactionSet &fs = g.systems.factions;
        int n = fs.Count();
        if (n == 0) return;
        if (holdings.empty()) CountHoldings(g);
        Layout L = Lay();
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = (row + n - 1) % n;
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = (row + 1) % n;
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        if (CheckCollisionPointRec(in.mousePos, L.list)) {
            int over = top + (int)((in.mousePos.y - L.list.y) / ROW_H);
            bool moved = in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f;
            if (over >= 0 && over < n && (moved || in.MousePressed(MOUSE_BUTTON_LEFT))) row = over;
        }
        top = std::max(std::min(top, row), row - L.visible + 1);
        top = std::max(0, std::min(top, n - L.visible));
        {
            const Faction &f = fs.list[row];
            bool state = f.kind == FactionKind::Government || f.kind == FactionKind::FreePort;
            if (in.Pressed(KEY_C)) ui.Open(MakeContractsWindow({}, f.id));
            if (in.Pressed(KEY_G) && f.kind == FactionKind::Guild) ui.Do(g, { Command::JoinGuild, f.id });
            if (in.Pressed(KEY_L) && state) ui.Do(g, { Command::BuyLicence, f.id, (double)PAPER_CARRIER });
            if (in.Pressed(KEY_P) && state) ui.Do(g, { Command::BuyLicence, f.id, (double)PAPER_PERMIT });
        }
        if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) {
            const Faction &f = fs.list[row];
            if (g.systems.Exists(f.seat) && f.kind != FactionKind::Clan) {
                ui.SetCourse(TargetId::MakeStar(f.seat));
                ui.Say("Course set for " + g.systems[f.seat].name + "  -  hold SHIFT to go");
                closed = true;
            } else {
                ui.Say("Nobody knows where " + f.name + " lair");
            }
        }
    }

    void Draw(const GameState &g) const override
    {
        const FactionSet &fs = g.systems.factions;
        Layout L = Lay();
        DrawRectangleRec(L.panel, Fade(BLACK, 0.93f));
        DrawRectangleLinesEx(L.panel, 1.0f, HUD);
        DrawText("THE POWERS OF HUMAN SPACE", (int)L.panel.x + 20, (int)L.panel.y + 14, 24, WHITE);
        int n = fs.Count();
        if (n == 0) return;
        int sel = std::min(row, n - 1);

        // --- Everyone, by kind ---
        int x = (int)L.list.x;
        for (int i = top; i < n && i < top + L.visible; i++) {
            const Faction &f = fs.list[i];
            int y = (int)L.list.y + (i - top) * ROW_H;
            if (i == sel) DrawRectangle(x - 8, y - 2, (int)L.list.width + 8, ROW_H, Fade(HUD, 0.2f));
            DrawRectangle(x, y + 4, 10, 10, f.color);
            DrawText(f.name.c_str(), x + 18, y, 18, i == sel ? WHITE : LIGHTGRAY);
            const char *kind = KindName(f.kind);
            DrawText(kind, (int)(L.list.x + L.list.width) - MeasureText(kind, 14), y + 3, 14, HUD_DIM);
        }
        if (top > 0)             DrawText("...", (int)(L.list.x + L.list.width) - 20, (int)L.list.y - 18, 16, HUD_DIM);
        if (top + L.visible < n) DrawText("...", (int)(L.list.x + L.list.width) - 20, (int)(L.list.y + L.list.height) - 2, 16, HUD_DIM);
        int dx = (int)L.detail.x;
        DrawLine(dx - 16, (int)L.list.y - 6, dx - 16, (int)(L.list.y + L.list.height), HUD_DIM);

        // --- The one picked out ---
        const Faction &f = fs.list[sel];
        int y = (int)L.detail.y, right = (int)(L.detail.x + L.detail.width), half = dx + (int)L.detail.width / 2 + 10;
        DrawText(f.name.c_str(), dx, y, 24, f.color); y += 30;
        std::string what = KindName(f.kind);
        if (f.kind == FactionKind::Conglomerate) what += std::string("  -  ") + SlantName(f.slant);
        what += "  -  " + IdeologyBlurb(f);
        DrawText(what.c_str(), dx, y, 16, HUD_LABEL); y += 22;
        std::string where;
        if (g.systems.Exists(f.seat))
            where = std::string(f.kind == FactionKind::Government ? "Capital: " : f.kind == FactionKind::Clan ? "Haunts the space around " : "Seat: ") +
                    g.systems[f.seat].name + TextFormat("  (%.0f ly from the free port)", Len(f.seatLy));
        if (sel < (int)holdings.size() && holdings[sel] > 0) where += TextFormat("   -   holds %d systems", holdings[sel]);
        if (f.patron != NO_FACTION) where += "   -   said to be in the pay of " + fs.Name(f.patron);
        DrawText(where.c_str(), dx, y, 16, LIGHTGRAY); y += 28;

        // Where you stand
        float st = g.Standing(sel);
        DrawText(TextFormat("YOUR STANDING   %+.0f   %s", st, RankName(f, st)), dx, y, 18, st < -20.0f ? ORANGE : st >= 20.0f ? HUD : WHITE);
        y += 26;
        // Papers: what you hold from it, and what you could
        {
            uint8_t have = g.Papers(sel);
            std::string papers;
            Color pc = HUD_LABEL;
            if (f.kind == FactionKind::Guild) {
                if (have & PAPER_MEMBER)                          { papers = "You are a member: its contracts are yours wherever it has a chapter"; pc = HUD; }
                else if (f.guild == GuildKind::BountyHunters)     papers = "Not taking members yet";
                else papers = TextFormat("G: join for %s cr  -  members get its contracts at stations with a chapter", Thousands(GUILD_FEE).c_str());
            } else if (f.kind == FactionKind::Government || f.kind == FactionKind::FreePort) {
                papers = have & PAPER_CARRIER ? std::string("Bonded carrier: held")
                       : TextFormat("L: bonded carrier's licence, %s cr (%s)", Thousands(CARRIER_COST).c_str(), RankName(f, CARRIER_STANDING));
                if (!f.contraband.empty())
                    papers += have & PAPER_PERMIT ? std::string("    Restricted goods permit: held")
                            : TextFormat("    P: restricted goods permit, %s cr (%s)", Thousands(PERMIT_COST).c_str(), RankName(f, PERMIT_STANDING));
                if (have) pc = HUD;
            }
            if (!papers.empty()) { DrawText(papers.c_str(), dx, y, 14, pc); y += 20; }
        }
        y += 8;

        // Left column: character. Right column: what it wants.
        int top0 = y, cy = y;
        DrawText("IDEOLOGY", dx, cy, 16, HUD); cy += 22;
        for (int a = 0; a < AX_COUNT; a++, cy += 22) {
            const int W = 100, bx = dx + 100;
            DrawText(AxisName(a, false), dx, cy, 14, f.axis[a] < -0.3f ? WHITE : HUD_DIM);
            DrawLine(bx, cy + 7, bx + W, cy + 7, HUD_DIM);
            DrawLine(bx + W / 2, cy + 3, bx + W / 2, cy + 11, HUD_DIM);
            DrawRectangle(bx + (int)((f.axis[a] + 1.0f) * 0.5f * W) - 3, cy + 2, 6, 11, f.color);
            DrawText(AxisName(a, true), bx + W + 10, cy, 14, f.axis[a] > 0.3f ? WHITE : HUD_DIM);
        }
        cy += 10;
        DrawText("TEMPERAMENT", dx, cy, 16, HUD); cy += 22;
        for (int t = 0; t < TR_COUNT; t++, cy += 20) {
            const int W = 100, bx = dx + 100;
            DrawText(TraitName(t), dx, cy, 14, LIGHTGRAY);
            DrawRectangleLines(bx, cy + 2, W, 10, HUD_DIM);
            DrawRectangle(bx + 1, cy + 3, (int)((W - 2) * f.trait[t]), 8, f.trait[t] > 0.66f ? HUD : HUD_DIM);
        }
        if (f.kind == FactionKind::Government || f.kind == FactionKind::FreePort) {
            cy += 10;
            DrawText("LAW", dx, cy, 16, HUD); cy += 22;
            DrawText(TextFormat("Tariff %.0f%%    Patrols: %s", f.tariff * 100.0f, f.policing > 0.75f ? "heavy" : f.policing > 0.45f ? "regular" : "thin"),
                     dx, cy, 14, LIGHTGRAY); cy += 20;
            std::string banned;
            for (int good : f.contraband) banned += (banned.empty() ? "" : ", ") + std::string(GoodData(good).name);
            DrawText(("Contraband: " + (banned.empty() ? std::string("nothing") : banned)).c_str(), dx, cy, 14, banned.empty() ? LIGHTGRAY : ORANGE);
            cy += 20;
        }

        y = top0;
        DrawText("GOALS", half, y, 16, HUD); y += 22;
        for (const Goal &goal : f.goals) {
            DrawText(GoalText(fs, goal).c_str(), half, y, 14, LIGHTGRAY);
            DrawRectangle(right - 60, y + 4, (int)(60 * goal.weight), 6, HUD_DIM);
            y += 20;
        }
        y += 10;
        DrawText("AGENDA  -  what it would do next", half, y, 16, HUD); y += 22;
        for (const AgendaItem &item : FactionAgenda(fs, sel, 5)) {
            std::string text = item.text;
            while (text.size() > 4 && MeasureText(text.c_str(), 14) > right - half - 10) text = text.substr(0, text.size() - 4) + "...";
            DrawText(text.c_str(), half, y, 14, LIGHTGRAY); y += 20;
        }
        y += 10;
        DrawText("RELATIONS", half, y, 16, HUD); y += 22;
        // The ones that matter most to it: its best friends and worst enemies
        std::vector<int> others;
        for (int i = 0; i < n; i++) if (i != sel) others.push_back(i);
        std::sort(others.begin(), others.end(), [&](int a, int b) { return fabsf(fs.Relation(sel, a)) > fabsf(fs.Relation(sel, b)); });
        int room = std::max(0, ((int)(L.detail.y + L.detail.height) - y) / 18);
        if ((int)others.size() > room) others.resize(room);
        std::sort(others.begin(), others.end(), [&](int a, int b) { return fs.Relation(sel, a) > fs.Relation(sel, b); });
        for (int o : others) {
            float rel = fs.Relation(sel, o);
            const char *stance = StanceName(rel);
            DrawRectangle(half, y + 4, 8, 8, fs.list[o].color);
            DrawText(fs.list[o].name.c_str(), half + 14, y, 14, LIGHTGRAY);
            DrawText(stance, right - MeasureText(stance, 14), y, 14, rel >= 15.0f ? HUD : rel > -20.0f ? HUD_DIM : rel > -50.0f ? ORANGE : RED);
            y += 18;
        }

        DrawText("UP/DOWN, wheel or mouse: pick   ENTER: set course for its seat   C: its contracts   R / ESC: close",
                 (int)L.panel.x + 20, (int)(L.panel.y + L.panel.height) - 26, 16, HUD_DIM);
    }

private:
    static const int ROW_H = 24;
    struct Layout { Rectangle panel, list, detail; int visible; };
    static Layout Lay()
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Layout L;
        L.panel = { 30, 30, sw - 60, sh - 60 };
        float listW = fminf(430.0f, L.panel.width * 0.36f);
        L.list = { L.panel.x + 24, L.panel.y + 60, listW, L.panel.height - 100.0f };
        L.visible = std::max(1, (int)(L.list.height / ROW_H));
        L.list.height = (float)(L.visible * ROW_H);
        L.detail = { L.list.x + listW + 34, L.panel.y + 56, L.panel.width - listW - 80, L.panel.height - 96.0f };
        return L;
    }
    // How many systems each government holds (worked out once: it means looking at all of human space)
    void CountHoldings(const GameState &g)
    {
        holdings.assign(g.systems.factions.Count(), 0);
        std::vector<const StarSystem *> all;
        g.systems.Nearby(g.systems.home, {}, FRONTIER_RADIUS_LY * 1.25 * LY_U, all);
        for (const StarSystem *s : all) if (s->ruler >= 0 && s->ruler < (int)holdings.size()) holdings[s->ruler]++;
    }

    int row = 0, top = 0;
    std::vector<int> holdings;
};

std::unique_ptr<Window> MakeFactionsWindow() { return std::make_unique<FactionsWindow>(); }
