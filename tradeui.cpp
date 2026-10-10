// tradeui.cpp - the trade log (T) and the news (N)
//
// The trade log is everything the captain knows about prices: one good at a
// time, every market they have a record of that deals in it, with how old the
// record is. Records come from docking, market reports over the comms, and
// trade data bought at stations (economy.h).
#include "ui.h"
#include "economy.h"
#include "format.h"
#include "theme.h"
#include <algorithm>

static std::string Age(double seconds) { return seconds < 5.0 ? std::string("now") : FormatETA(seconds); }

// ===========================================================================
// Trade log
// ===========================================================================
class TradeLogWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::TradeLog; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }

    struct Row { uint64_t body; const PriceRecord *rec; double buy, sell, ly; };

    // The markets on record that deal in the good, best first
    std::vector<Row> Rows(const GameState &g) const
    {
        std::vector<Row> rows;
        for (const auto &kv : g.know.prices) {
            const PriceRecord &r = kv.second;
            Role role = (Role)(r.role[good] & 127);
            if (role == Role::None || (r.role[good] & 128)) continue;
            double edge = StandingEdge(g, r.owner);
            Row row = { kv.first, &r, r.stock[good] >= 0.5 ? r.price[good] * 1.04 * (1.0 - edge) : 0.0,
                        r.price[good] * 0.96 * (1.0 + edge) * (1.0 - r.tariff),
                        g.systems.Exists(r.sys) ? Len(g.systems.Between(g.player.ship.anchor, r.sys) - g.player.ship.pos) / LY_U : 0.0 };
            rows.push_back(row);
        }
        if (bySell) std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.sell > b.sell; });
        else        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return (a.buy > 0 ? a.buy : 1e30) < (b.buy > 0 ? b.buy : 1e30); });
        return rows;
    }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_T)) { closed = true; return; }
        if (in.Pressed(KEY_RIGHT) || in.Pressed(KEY_D)) { good = (good + 1) % GOOD_COUNT; row = top = 0; }
        if (in.Pressed(KEY_LEFT)  || in.Pressed(KEY_A)) { good = (good + GOOD_COUNT - 1) % GOOD_COUNT; row = top = 0; }
        if (in.Pressed(KEY_TAB)) { bySell = !bySell; row = top = 0; }
        std::vector<Row> rows = Rows(g);
        int n = (int)rows.size();
        if (n == 0) return;
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = (row + n - 1) % n;
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = (row + 1) % n;
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        top = std::max(std::min(top, row), row - VISIBLE + 1);
        top = std::max(0, std::min(top, n - VISIBLE));
        if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) {
            const PriceRecord &r = *rows[row].rec;
            if (g.systems.Exists(r.sys)) {
                ui.SetCourse(TargetId::MakeStar(r.sys));
                ui.Say("Course set for " + g.systems[r.sys].name + "  -  hold SHIFT to go");
                closed = true;
            }
        }
    }

    void Draw(const GameState &g) const override
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { sw / 2 - 480, sh / 2 - 300, 960, 600 };
        int px = (int)panel.x, py = (int)panel.y;
        DrawRectangleRec(panel, Fade(BLACK, 0.93f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        DrawText("TRADE LOG", px + 20, py + 14, 24, WHITE);
        const GoodInfo &info = GoodData(good);
        std::string head = std::string("<  ") + info.name + "  >";
        DrawText(head.c_str(), px + 200, py + 14, 24, info.color);
        std::string what = std::string(TierName(GoodTier(good))) + TextFormat("  -  going rate %.0f cr/t", info.basePrice);
        for (const Ingredient &in : info.recipe)
            if (in.tonnes > 0.0f) what += std::string(in.good == info.recipe[0].good ? "  -  made from " : ", ") + GoodData(in.good).name;
        DrawText(what.c_str(), px + 20, py + 46, 16, HUD_LABEL);
        DrawText(bySell ? "best place to SELL first" : "cheapest place to BUY first", px + 700, py + 20, 16, HUD);
        DrawLine(px + 18, py + 72, px + (int)panel.width - 18, py + 72, HUD_DIM);

        std::vector<Row> rows = Rows(g);
        int n = (int)rows.size(), sel = std::min(row, n - 1);
        int cx[] = { px + 30, px + 360, px + 450, px + 540, px + 640, px + 750, px + 850 };
        const char *cols[] = { "MARKET", "BUY AT", "SELL AT", "IN STOCK", "DISTANCE", "AS OF", "" };
        for (int i = 0; i < 6; i++) DrawText(cols[i], cx[i], py + 80, 16, HUD_LABEL);
        if (n == 0) {
            DrawText("No market on record deals in this.", px + 30, py + 116, 18, LIGHTGRAY);
            DrawText("Dock, ask for a market report over the comms, or buy trade data at a station.", px + 30, py + 142, 16, HUD_DIM);
        }
        const Color CHEAP = { 140, 220, 140, 255 }, DEAR = { 240, 170, 110, 255 };
        for (int i = top; i < n && i < top + VISIBLE; i++) {
            const Row &r = rows[i];
            int y = py + 106 + (i - top) * 24;
            if (i == sel) DrawRectangle(px + 18, y - 3, (int)panel.width - 36, 24, Fade(HUD, 0.18f));
            const Faction *owner = g.systems.factions.Get(r.rec->owner);
            DrawRectangle(cx[0] - 12, y + 5, 8, 8, owner ? owner->color : GRAY);
            DrawText(r.rec->name.c_str(), cx[0], y, 18, WHITE);
            double mid = r.rec->price[good];
            Color pc = mid < 0.8 * info.basePrice ? CHEAP : mid > 1.3 * info.basePrice ? DEAR : LIGHTGRAY;
            DrawText(r.buy > 0.0 ? TextFormat("%.0f", r.buy) : "-", cx[1], y, 18, pc);
            DrawText(TextFormat("%.0f", r.sell), cx[2], y, 18, pc);
            DrawText(TextFormat("%s t", Thousands(r.rec->stock[good]).c_str()), cx[3], y, 18, LIGHTGRAY);
            DrawText(r.ly < 0.05 ? "here" : TextFormat("%.1f ly", r.ly), cx[4], y, 18, LIGHTGRAY);
            double age = g.clock - r.rec->at;
            DrawText(Age(age).c_str(), cx[5], y, 18, age > 2.0 * MARKET_DAY ? ORANGE : LIGHTGRAY);     // (stale: a couple of market days)
            const char *role = (Role)(r.rec->role[good] & 127) == Role::Export ? "makes it" : (Role)(r.rec->role[good] & 127) == Role::Import ? "needs it" : "";
            DrawText(role, cx[6], y + 2, 14, HUD_DIM);
        }
        if (top > 0)           DrawText("^ more", px + (int)panel.width - 90, py + 80, 14, HUD_DIM);
        if (top + VISIBLE < n) DrawText("v more", px + (int)panel.width - 90, py + 106 + VISIBLE * 24 - 2, 14, HUD_DIM);
        DrawText(TextFormat("%d markets on record   -   prices include your standing and their tariffs", (int)g.know.prices.size()),
                 px + 20, py + (int)panel.height - 50, 16, HUD_LABEL);
        DrawText("A/D: another good   TAB: buy / sell order   W/S or wheel: pick   ENTER: set course for its system   T / ESC: close",
                 px + 20, py + (int)panel.height - 26, 16, HUD_DIM);
    }

private:
    static const int VISIBLE = 18;
    int  good = REF_FUEL;
    bool bySell = false;
    int  row = 0, top = 0;
};

// ===========================================================================
// News: what the factions have been doing, and what's gone wrong or right locally
// ===========================================================================
class NewsWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::News; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_N)) { closed = true; return; }
        int n = (int)g.news.size();
        if (n == 0) return;
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = std::max(0, row - 1);
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = std::min(n - 1, row + 1);
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        top = std::max(std::min(top, row), row - VISIBLE + 1);
        top = std::max(0, std::min(top, n - VISIBLE));
        if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) {
            const EconNews &item = g.news[n - 1 - row];                // (newest first)
            if (g.systems.Exists(item.where)) {
                ui.SetCourse(TargetId::MakeStar(item.where));
                ui.Say("Course set for " + g.systems[item.where].name + "  -  hold SHIFT to go");
                closed = true;
            }
        }
    }

    void Draw(const GameState &g) const override
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { sw / 2 - 480, sh / 2 - 280, 960, 560 };
        int px = (int)panel.x, py = (int)panel.y;
        DrawRectangleRec(panel, Fade(BLACK, 0.93f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        DrawText("NEWS", px + 20, py + 14, 24, WHITE);
        DrawText("broadcast across human space", px + 110, py + 22, 16, HUD_LABEL);
        DrawLine(px + 18, py + 50, px + (int)panel.width - 18, py + 50, HUD_DIM);
        int n = (int)g.news.size(), sel = std::min(row, n - 1);
        if (n == 0) DrawText("Nothing to report yet.", px + 30, py + 70, 18, LIGHTGRAY);
        for (int i = top; i < n && i < top + VISIBLE; i++) {
            const EconNews &item = g.news[n - 1 - i];
            int y = py + 62 + (i - top) * 24;
            if (i == sel) DrawRectangle(px + 18, y - 3, (int)panel.width - 36, 24, Fade(HUD, 0.18f));
            const Faction *f = g.systems.factions.Get(item.faction);
            DrawRectangle(px + 30, y + 5, 8, 8, f ? f->color : GRAY);
            std::string when = Age(g.clock - item.at);
            DrawText(when.c_str(), px + 46, y + 1, 16, HUD_DIM);
            std::string text = item.text;
            double ly = g.systems.Exists(item.where) ? Len(g.systems.Between(g.player.ship.anchor, item.where) - g.player.ship.pos) / LY_U : -1.0;
            std::string far = ly < 0.0 ? "" : ly < 0.05 ? "here" : TextFormat("%.0f ly", ly);
            while (text.size() > 4 && MeasureText(text.c_str(), 18) > (int)panel.width - 260) text = text.substr(0, text.size() - 4) + "...";
            DrawText(text.c_str(), px + 130, y, 18, f ? WHITE : LIGHTGRAY);
            DrawText(far.c_str(), px + (int)panel.width - 30 - MeasureText(far.c_str(), 16), y + 1, 16, ly >= 0.0 && ly < 12.0 ? HUD : HUD_DIM);
        }
        if (top > 0)           DrawText("^ newer", px + (int)panel.width - 100, py + 26, 14, HUD_DIM);
        if (top + VISIBLE < n) DrawText("v older", px + (int)panel.width - 100, py + 62 + VISIBLE * 24 - 2, 14, HUD_DIM);
        DrawText("W/S or wheel: pick   ENTER: set course for where it happened   N / ESC: close", px + 20, py + (int)panel.height - 26, 16, HUD_DIM);
    }

private:
    static const int VISIBLE = 19;
    int row = 0, top = 0;
};

std::unique_ptr<Window> MakeTradeLog()   { return std::make_unique<TradeLogWindow>(); }
std::unique_ptr<Window> MakeNewsWindow() { return std::make_unique<NewsWindow>(); }
