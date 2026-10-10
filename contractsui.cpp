// contractsui.cpp - the contracts window (J): the contracts you hold, and the
// boards you've read (a station's, asked for over the comms; a faction's list,
// from the factions window). What's shown of a board is as of when it was read.
#include "ui.h"
#include "comms.h"
#include "contracts.h"
#include "economy.h"
#include "format.h"
#include "theme.h"
#include <algorithm>

class ContractsWindow : public Window {
public:
    ContractsWindow(TargetId station, int faction) : station(station), faction(faction), board(station.valid() || faction != NO_FACTION) {}
    WindowKind Kind() const override { return WindowKind::Contracts; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }

    // The board on show: a station's (by its body id) or a faction's
    uint64_t Key(const GameState &g) const
    {
        if (faction != NO_FACTION) return BoardKey(faction);
        if (station.valid() && station.system == g.loaded.index && station.index >= 0 && station.index < (int)g.loaded.bodies.size())
            return g.loaded.bodies[station.index].id;
        return stationId;
    }

    std::vector<const Contract *> Rows(const GameState &g) const
    {
        std::vector<const Contract *> rows;
        if (!board) { for (const Contract &k : g.jobs) rows.push_back(&k); return rows; }
        uint64_t key = Key(g);
        for (const Contract &k : g.offers) if (k.listKey == key && k.expires > g.clock) rows.push_back(&k);
        std::sort(rows.begin(), rows.end(), [](const Contract *a, const Contract *b) { return a->pay > b->pay; });
        return rows;
    }

    void Ask(GameState &g, Ui &ui)
    {
        if (faction != NO_FACTION) ui.Do(g, { Command::FactionBoard, faction });
        else if (station.valid() && station.system == g.loaded.index && g.comms.Find(station)) ui.Do(g, { Command::Contracts, station.index });
        asked = true;
    }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_J)) { closed = true; return; }
        if (station.valid() && station.system == g.loaded.index && station.index < (int)g.loaded.bodies.size()) {
            stationId = g.loaded.bodies[station.index].id;
            stationName = g.loaded.bodies[station.index].name;
        }
        // A board is asked for when it's opened (unless it was read moments ago), and again on R
        if (board && !asked) {
            auto seen = g.boards.find(Key(g));
            if (faction != NO_FACTION || seen == g.boards.end() || g.clock - seen->second > 30.0) Ask(g, ui); else asked = true;
        }
        if (board && in.Pressed(KEY_R)) Ask(g, ui);
        if (in.Pressed(KEY_TAB) && (station.valid() || faction != NO_FACTION || stationId)) { board = !board; row = top = 0; confirm = 0; }

        std::vector<const Contract *> rows = Rows(g);
        int n = (int)rows.size();
        if (n == 0) return;
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) { row = (row + n - 1) % n; confirm = 0; }
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) { row = (row + 1) % n; confirm = 0; }
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        top = std::max(std::min(top, row), row - VISIBLE + 1);
        top = std::max(0, std::min(top, n - VISIBLE));
        const Contract &k = *rows[row];
        if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) {
            if (board) {
                ui.Do(g, { Command::Accept, (int32_t)k.id });
            } else {
                bool here = false;
                JobStatus(g, g, k, &here);
                if (here) ui.Do(g, { Command::Advance, (int32_t)k.id });
                else      Course(g, ui, k);
            }
        }
        if (!board && in.Pressed(KEY_BACKSPACE)) {
            if (confirm == k.id) { ui.Do(g, { Command::Abandon, (int32_t)k.id }); confirm = 0; }
            else                 { confirm = k.id; ui.Say("BACKSPACE again to give up this contract"); }
        }
    }

    // Where the job wants the ship next: the body if it's in this system, or else its system
    void Course(GameState &g, Ui &ui, const Contract &k)
    {
        SysId sys = JobNextSystem(k);
        if (!g.systems.Exists(sys)) return;
        uint64_t body = !k.loaded || (k.kind == ContractKind::Survey && k.Scanned() >= k.need) ? k.fromBody
                      : k.kind == ContractKind::Survey ? 0 : k.toBody;
        if (sys == g.loaded.index && body)
            for (int i = 0; i < (int)g.loaded.bodies.size(); i++)
                if (g.loaded.bodies[i].id == body) {
                    ui.SetCourse(TargetId::MakeBody(sys, i));
                    ui.Say("Course set for " + g.loaded.bodies[i].name + "  -  hold SHIFT to go");
                    closed = true;
                    return;
                }
        ui.SetCourse(TargetId::MakeStar(sys));
        ui.Say("Course set for " + g.systems[sys].name + "  -  hold SHIFT to go");
        closed = true;
    }

    static double LyTo(const GameState &g, SysId sys)
    {
        return g.systems.Exists(sys) ? Len(g.systems.Between(g.player.ship.anchor, sys) - g.player.ship.pos) / LY_U : 0.0;
    }
    static std::string Place(const GameState &g, const std::string &name, SysId sys)
    {
        double ly = LyTo(g, sys);
        std::string where = g.systems.Exists(sys) ? g.systems[sys].name : std::string("?");
        return name + "   (" + (ly < 0.05 ? std::string("this system") : where + TextFormat(", %.1f ly", ly)) + ")";
    }

    void Draw(const GameState &g) const override
    {
        const FactionSet &fs = g.systems.factions;
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { sw / 2 - 560, sh / 2 - 300, 1120, 600 };
        if (panel.x < 10) { panel.x = 10; panel.width = sw - 20; }
        int px = (int)panel.x, py = (int)panel.y, pw = (int)panel.width, ph = (int)panel.height;
        DrawRectangleRec(panel, Fade(BLACK, 0.98f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        uint64_t key = Key(g);
        bool canTab = station.valid() || faction != NO_FACTION || stationId;

        std::string head = !board ? TextFormat("YOUR CONTRACTS   %d of %d", (int)g.jobs.size(), MAX_JOBS)
                         : faction != NO_FACTION ? "CONTRACTS  -  " + fs.Name(faction) : "CONTRACTS  -  " + stationName;
        DrawText(head.c_str(), px + 20, py + 14, 24, WHITE);
        if (board) {
            auto seen = g.boards.find(key);
            std::string as = seen == g.boards.end() ? std::string("asking for the board...  (the reply takes light-time)")
                           : g.clock - seen->second < 5.0 ? std::string("as of now") : "as of " + FormatETA(g.clock - seen->second) + " ago";
            DrawText(as.c_str(), px + pw - 20 - MeasureText(as.c_str(), 16), py + 20, 16, seen == g.boards.end() ? YELLOW : HUD_LABEL);
        }
        DrawLine(px + 18, py + 50, px + pw - 18, py + 50, HUD_DIM);

        std::vector<const Contract *> rows = Rows(g);
        int n = (int)rows.size(), sel = std::min(row, n - 1), listW = 540, lx = px + 24;
        if (n == 0) {
            const char *none = !board ? "You hold no contracts. Hail a station and ask for work (4), or see a faction's list (R, then C)."
                             : g.boards.count(key) ? "Nothing on the board just now." : "";
            DrawText(none, lx, py + 70, 18, LIGHTGRAY);
        }
        for (int i = top; i < n && i < top + VISIBLE; i++) {
            const Contract &k = *rows[i];
            int y = py + 62 + (i - top) * ROW_H;
            if (i == sel) DrawRectangle(lx - 6, y - 3, listW, ROW_H, Fade(HUD, 0.18f));
            const Faction *who = fs.Get(k.guild != NO_FACTION ? k.guild : k.issuer);
            DrawRectangle(lx, y + 6, 8, 8, who ? who->color : GRAY);
            std::string title = ContractTitle(k);
            while (title.size() > 4 && MeasureText(title.c_str(), 18) > listW - 130) title = title.substr(0, title.size() - 4) + "...";
            bool barred = board && !ContractBar(g, g, k).empty();
            DrawText(title.c_str(), lx + 16, y, 18, barred ? GRAY : WHITE);
            std::string pay = Thousands(k.pay) + " cr";
            DrawText(pay.c_str(), lx + listW - 16 - MeasureText(pay.c_str(), 18), y, 18, barred ? GRAY : HUD);
            std::string sub;
            Color sc = HUD_LABEL;
            if (board) {
                sub = std::string(ContractKindName(k.kind)) + "  -  " + (who ? who->name : k.fromName) + "  -  " + FormatETA(k.allowed) + " allowed";
            } else {
                double left = k.deadline - g.clock;
                sub = JobStatus(g, g, k) + "  -  " + (left >= 0.0 ? FormatETA(left) + " left" : "LATE: half pay, fails in " + FormatETA(left + 0.5 * k.allowed));
                if (left < 0.0) sc = ORANGE; else if (left < 0.25 * k.allowed) sc = YELLOW;
            }
            while (sub.size() > 4 && MeasureText(sub.c_str(), 14) > listW - 30) sub = sub.substr(0, sub.size() - 4) + "...";
            DrawText(sub.c_str(), lx + 16, y + 21, 14, sc);
        }
        if (top > 0)           DrawText("^ more", lx + listW - 70, py + 52, 14, HUD_DIM);
        if (top + VISIBLE < n) DrawText("v more", lx + listW - 70, py + 62 + VISIBLE * ROW_H - 4, 14, HUD_DIM);

        // --- The one picked out ---
        int dx = lx + listW + 24, y = py + 64, right = px + pw - 20;
        DrawLine(dx - 14, py + 58, dx - 14, py + ph - 40, HUD_DIM);
        if (n > 0) {
            const Contract &k = *rows[sel];
            auto line = [&](const char *label, const std::string &text, Color c = LIGHTGRAY) {
                DrawText(label, dx, y, 16, HUD_LABEL);
                std::string t = text;
                while (t.size() > 4 && MeasureText(t.c_str(), 16) > right - dx - 110) t = t.substr(0, t.size() - 4) + "...";
                DrawText(t.c_str(), dx + 110, y, 16, c);
                y += 24;
            };
            std::string title = ContractTitle(k);
            while (title.size() > 4 && MeasureText(title.c_str(), 20) > right - dx) title = title.substr(0, title.size() - 4) + "...";
            DrawText(title.c_str(), dx, y, 20, WHITE); y += 32;
            const Faction *issuer = fs.Get(k.issuer);
            line("Kind", ContractKindName(k.kind));
            line("Offered by", issuer ? issuer->name : k.fromName + " (independent)", issuer ? issuer->color : LIGHTGRAY);
            if (k.kind == ContractKind::Survey) {
                line("Survey", Place(g, k.surveyName, k.surveySys));
                line("Worlds", board ? TextFormat("%d, each scanned from close by", k.need) : TextFormat("%d of %d scanned", k.Scanned(), k.need));
                line("Report to", Place(g, k.fromName, k.fromSys));
            } else {
                if (k.fromBody != k.toBody || k.kind != ContractKind::Supply) line(k.kind == ContractKind::Supply ? "Posted at" : "Collect at", Place(g, k.fromName, k.fromSys));
                line("Deliver to", Place(g, k.toName, k.toSys));
                if (k.good >= 0) line("Cargo", TextFormat("%.0f t %s%s", k.tonnes, GoodData(k.good).name,
                                                          k.kind == ContractKind::Supply ? "  -  yours to find" : "  -  theirs: loaded at the dock"), GoodData(k.good).color);
            }
            line("Pays", Thousands(k.pay) + " cr on delivery", HUD);
            if (k.collateral > 0.0f) line("Deposit", Thousands(k.collateral) + " cr, returned on delivery");
            if (board) line("Time", FormatETA(k.allowed) + " from accepting  -  late pays half");
            else       line("Time", k.deadline >= g.clock ? FormatETA(k.deadline - g.clock) + " left" : std::string("LATE"), k.deadline >= g.clock ? LIGHTGRAY : ORANGE);
            const Faction *on = fs.Get(k.effectOn);
            switch (k.effect) {
            case ContractEffect::Relief:     line("Effect", "Relief: the trouble at " + k.toName + " ends sooner", HUD); break;
            case ContractEffect::ArmPatrols: line("Effect", "Supplies the patrols hunting " + (on ? on->name : std::string("the clans")), HUD); break;
            case ContractEffect::ArmClan:    line("Effect", "Arms " + (on ? on->name : std::string("a clan")) + ": more raiding", ORANGE); break;
            case ContractEffect::Charts:     line("Effect", k.surveyName + " goes on the charts", HUD); break;
            default: break;
            }
            if (const Faction *a = fs.Get(k.against)) line("Against", a->name + " will think less of you", ORANGE);
            if (k.kind == ContractKind::Smuggling) line("Risk", "Customs at lawful ports may search your hold", ORANGE);
            std::string terms = ContractTerms(fs, k);
            if (!terms.empty()) line("Open to", terms);
            y += 8;
            if (board) {
                std::string bar = ContractBar(g, g, k);
                if (bar.empty()) DrawText("ENTER: accept", dx, y, 18, HUD);
                else             DrawText(bar.c_str(), dx, y, 16, ORANGE);
                y += 24;
                DrawText(TextFormat("on offer for another %s", FormatETA(k.expires - g.clock).c_str()), dx, y, 14, HUD_DIM);
            } else {
                bool here = false;
                std::string next = JobStatus(g, g, k, &here);
                DrawText(("Next: " + next).c_str(), dx, y, 18, here ? HUD : WHITE); y += 26;
                DrawText(here ? "ENTER: do it here" : "ENTER: set course", dx, y, 16, here ? HUD : HUD_LABEL);
            }
        }
        std::string help = board ? "W/S or wheel: pick   ENTER: accept   R: ask again" : "W/S or wheel: pick   ENTER: next step / set course   BACKSPACE: give up";
        if (canTab) help += board ? "   TAB: your contracts" : "   TAB: the board";
        help += "   J / ESC: close";
        DrawText(help.c_str(), px + 20, py + ph - 26, 16, HUD_DIM);
    }

private:
    static const int ROW_H = 44, VISIBLE = 11;
    TargetId    station;             // the station whose board this is, if any
    uint64_t    stationId = 0;
    std::string stationName;
    int         faction;             // ...or the faction whose list it is
    bool        board;               // showing the board (false: the contracts you hold)
    bool        asked = false;
    int         row = 0, top = 0;
    uint32_t    confirm = 0;         // a contract BACKSPACE has been pressed on once
};

std::unique_ptr<Window> MakeContractsWindow(TargetId station, int faction) { return std::make_unique<ContractsWindow>(station, faction); }
