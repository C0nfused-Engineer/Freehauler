// ui.cpp - windows, hotkeys and messages
#include "ui.h"
#include "comms.h"
#include "economy.h"
#include "format.h"
#include "save.h"
#include "targetinfo.h"
#include "theme.h"
#include <algorithm>

const float ROLL_RATE = 90.0f;              // degrees per second
const float YAW_RATE  = 45.0f;              // (rudder; the mouse does the real aiming)

Controls ReadFlightControls(const InputFrame &in, float frameDt, const Settings &settings, bool &throttleCut)
{
    Controls c;
    // W and S together cut the throttle (all stop). Until both are let go they do nothing
    // more, so the one released last doesn't nudge the throttle off zero again.
    bool w = in.Down(KEY_W), s = in.Down(KEY_S);
    c.allStop = w && s && !throttleCut;
    if (w && s) throttleCut = true;
    else if (!w && !s) throttleCut = false;
    c.throttle = throttleCut ? 0.0f : (float)w - (float)s;
    c.roll    = ((float)in.Down(KEY_Q) - (float)in.Down(KEY_E)) * ROLL_RATE * frameDt;
    c.yaw     = -in.mouseDelta.x * settings.mouseSens + ((float)in.Down(KEY_A) - (float)in.Down(KEY_D)) * YAW_RATE * frameDt;
    c.pitch   = -in.mouseDelta.y * settings.mouseSens * (settings.invertY ? -1.0f : 1.0f);
    c.hyper   = in.Down(KEY_LEFT_SHIFT) || in.Down(KEY_RIGHT_SHIFT);
    c.fire    = in.MouseDown(MOUSE_BUTTON_LEFT);
    c.toggleDampeners = in.Pressed(KEY_CAPS_LOCK);
    return c;
}

// ===========================================================================
// Info panel: facts about the selection (closes when the selection goes)
// ===========================================================================
class InfoWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::Info; }
    bool StillValid(const GameState &g) const override { return g.flight.selected.valid(); }
    void Draw(const GameState &g) const override
    {
        int sw = GetScreenWidth();
        int px = sw - 460, py = 200, pw = 440;
        TargetId sel = g.flight.selected;
        std::vector<InfoRow> rows = TargetInfo(g, sel);
        int ph = 52 + 20 * std::max(1, (int)rows.size()) + 24;
        DrawRectangle(px, py, pw, ph, Fade(BLACK, 0.8f));
        DrawRectangleLines(px, py, pw, ph, HUD_DIM);
        DrawText(TargetName(g, sel).c_str(), px + 14, py + 12, 20, WHITE);
        DrawLine(px + 14, py + 38, px + pw - 14, py + 38, HUD_DIM);
        for (size_t r = 0; r < rows.size(); r++) {
            int ry = py + 48 + 20 * (int)r;
            DrawText(rows[r].label.c_str(), px + 14, ry, 16, HUD_LABEL);
            DrawText(rows[r].value.c_str(), px + 140, ry, 16, WHITE);
        }
        DrawText("[MMB] close", px + pw - 100, py + ph - 22, 14, HUD_DIM);
    }
};

// ===========================================================================
// Market: buy and sell goods, buy fuel. Modal: flight pauses while it's open.
// It shows the captain's record of the market, which is kept up to the second
// while docked (online, the server sends it).
// ===========================================================================
// The rows: what this market trades (what it makes, what it needs, the rest), then
// anything in the hold it doesn't, then the fuel tank (-1)
static std::vector<int> MarketRows(const Market &mk, const Player &p)
{
    std::vector<int> rows = mk.Listed();
    for (int gd = 0; gd < GOOD_COUNT; gd++)
        if (p.cargo[gd] > 0.05 && mk.role[gd] == Role::None) rows.push_back(gd);
    rows.push_back(-1);
    return rows;
}

class MarketWindow : public Window {
public:
    explicit MarketWindow(TargetId world) : world(world) {}
    WindowKind Kind() const override { return WindowKind::Market; }
    bool Modal() const override { return true; }
    bool StillValid(const GameState &g) const override
    {
        return TradePartner(g) && TargetId::MakeBody(g.loaded.index, g.flight.nearBody) == world;
    }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        const Body *b = TradePartner(g);
        Market mk;
        if (!b) { in.ConsumeAll(); return; }
        if (in.Pressed(KEY_F) || in.Pressed(KEY_ESCAPE)) closed = true;
        if (!KnownMarket(g, *b, mk)) { in.ConsumeAll(); return; }          // (its prices are on their way)
        std::vector<int> rows = MarketRows(mk, g.player);
        int n = (int)rows.size();
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = (row + n - 1) % n;
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = (row + 1) % n;
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));      // (the wheel stops at the ends)
        if (in.Pressed(KEY_PAGE_UP))   row = std::max(0, row - VISIBLE);
        if (in.Pressed(KEY_PAGE_DOWN)) row = std::min(n - 1, row + VISIBLE);
        row = std::min(row, n - 1);
        bool shift = in.Down(KEY_LEFT_SHIFT) || in.Down(KEY_RIGHT_SHIFT);
        bool buy  = in.Pressed(KEY_RIGHT) || in.Pressed(KEY_D);
        bool sell = in.Pressed(KEY_LEFT)  || in.Pressed(KEY_A);
        // ENTER / SPACE: sell 1 t / all of a good, or buy 1 t / fill up on fuel. SHIFT: ten at a time... or the lot with SPACE
        bool one = in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER), all = in.Pressed(KEY_SPACE);
        int gd = rows[row];
        if (gd < 0) {
            if (buy || one || all) ui.Do(g, { Command::BuyFuel, 0, (shift || all) ? 1e30 : 1.0 });
        } else {
            if (buy)         ui.Do(g, { Command::BuyGoods,  gd, shift ? 1e30 : 1.0 });
            if (sell || one) ui.Do(g, { Command::SellGoods, gd, shift ? 1e30 : 1.0 });
            if (all)         ui.Do(g, { Command::SellGoods, gd, 1e30 });
        }
        if (in.Pressed(KEY_B)) ui.Do(g, { Command::BuyData });
        // keep the selection in view
        if (row < top) top = row;
        if (row >= top + VISIBLE) top = row - VISIBLE + 1;
        in.ConsumeAll();
    }

    void Draw(const GameState &g) const override
    {
        const Body *b = TradePartner(g);
        if (!b) return;
        const Player &p = g.player;
        const FactionSet &fs = g.systems.factions;
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int pw = 800, ph = 168 + 24 * VISIBLE + 100;
        int px = sw / 2 - pw / 2, py = sh / 2 - ph / 2;
        DrawRectangle(px, py, pw, ph, Fade(BLACK, 0.9f));
        DrawRectangleLines(px, py, pw, ph, HUD);
        DrawText(TextFormat("MARKET  -  %s", b->name.c_str()), px + 18, py + 14, 24, WHITE);
        Market mk;
        if (!KnownMarket(g, *b, mk)) { DrawText("Opening the market...", px + 18, py + 60, 18, HUD_DIM); return; }
        std::vector<int> rows = MarketRows(mk, p);
        int n = (int)rows.size(), first = std::max(0, std::min(top, n - VISIBLE));

        // Who you're dealing with, and what that costs you
        const Faction *owner = fs.Get(mk.owner);
        std::string who = std::string(SettlementName(b->settlement)) + "  -  run by " + fs.Name(mk.owner, "itself") + "  -  population " + FormatPopulation(b->population);
        DrawText(who.c_str(), px + 18, py + 44, 16, owner ? owner->color : LIGHTGRAY);
        std::string terms = mk.tariff > 0.0 ? TextFormat("%s takes %.0f%% of what you're paid", fs.Name(mk.ruler).c_str(), mk.tariff * 100.0)
                                            : std::string("No tariff here");
        if (owner && fabs(mk.edge) >= 0.0005) terms += TextFormat("   -   %s: prices %.1f%% %s", RankName(*owner, g.Standing(mk.owner)), fabs(mk.edge) * 100.0, mk.edge > 0.0 ? "in your favour" : "against you");
        else if (owner) terms += TextFormat("   -   %s: the going rate", RankName(*owner, g.Standing(mk.owner)));
        DrawText(terms.c_str(), px + 18, py + 64, 14, HUD_LABEL);
        std::string ex = ExportsLine(mk, 5), im = ImportsLine(mk, 5);
        DrawText(TextFormat("Makes: %s", ex.empty() ? "-" : ex.c_str()), px + 18, py + 82, 14, Color{ 140, 220, 140, 255 });
        DrawText(TextFormat("Needs: %s", im.empty() ? "-" : im.c_str()), px + 18, py + 100, 14, Color{ 240, 170, 110, 255 });
        DrawLine(px + 18, py + 120, px + pw - 18, py + 120, HUD_DIM);
        int cx[] = { px + 30, px + 240, px + 340, px + 430, px + 515, px + 595, px + 675 };
        int hy = py + 128;
        DrawText("ITEM", cx[0], hy, 16, HUD_LABEL);  DrawText("IN STOCK", cx[1], hy, 16, HUD_LABEL);
        DrawText("WANTS", cx[2], hy, 16, HUD_LABEL); DrawText("HOLD", cx[3], hy, 16, HUD_LABEL);
        DrawText("BUY", cx[4], hy, 16, HUD_LABEL);   DrawText("SELL", cx[5], hy, 16, HUD_LABEL);
        const Color CHEAP = { 140, 220, 140, 255 }, DEAR = { 240, 170, 110, 255 }, OFF = { 120, 120, 120, 255 };
        for (int i = 0; i < VISIBLE && first + i < n; i++) {
            int r = first + i, gd = rows[r];
            int ry = py + 154 + 24 * i;
            if (r == row) DrawRectangle(px + 18, ry - 4, pw - 36, 24, Fade(HUD, 0.18f));
            if (gd >= 0) {
                const GoodInfo &info = GoodData(gd);
                bool have = p.cargo[gd] > 0.05, traded = mk.role[gd] != Role::None && !mk.banned[gd];
                Color c = have || mk.Sells(gd) ? info.color : OFF;
                // Against the going rate: green is cheap here (buy), orange is dear (sell)
                Color pc = !traded ? OFF : mk.price[gd] < 0.8 * info.basePrice ? CHEAP : mk.price[gd] > 1.3 * info.basePrice ? DEAR : LIGHTGRAY;
                DrawText(info.name, cx[0], ry, 18, c);
                DrawText(traded ? TextFormat("%s t", Thousands(mk.stock[gd]).c_str()) : "-", cx[1], ry, 18, traded ? LIGHTGRAY : OFF);
                // How much more it would take before it stops paying over the going rate
                DrawText(mk.Wants(gd) >= 0.5 ? TextFormat("%s t", Thousands(mk.Wants(gd)).c_str()) : "-", cx[2], ry, 18, mk.Wants(gd) >= 0.5 ? DEAR : OFF);
                DrawText(have ? TextFormat("%.1f t", p.cargo[gd]) : "-", cx[3], ry, 18, c);
                DrawText(mk.Sells(gd) ? TextFormat("%.0f", mk.BuyPrice(gd)) : "-", cx[4], ry, 18, pc);
                DrawText(mk.Buys(gd) ? TextFormat("%.0f", mk.SellPrice(gd)) : "-", cx[5], ry, 18, pc);
                const char *tag = "not traded"; Color tc = { 200, 90, 90, 255 };
                if (mk.banned[gd])                      { tag = "CONTRABAND"; tc = RED; }
                else if (mk.role[gd] == Role::Export)   { tag = "made here"; tc = CHEAP; }
                else if (mk.role[gd] == Role::Import)   { tag = "needed"; tc = DEAR; }
                else if (mk.role[gd] == Role::Trade)    { tag = "traded"; tc = HUD_DIM; }
                DrawText(tag, cx[6], ry, 16, tc);
            } else {
                DrawText("Fuel, into the tank", cx[0], ry, 18, WHITE);
                DrawText(TextFormat("%.1f/%.0f", p.fuel, FUEL_CAPACITY), cx[3], ry, 18, WHITE);
                DrawText(mk.fuelPrice > 0.0 ? TextFormat("%.0f", mk.fuelPrice) : "none", cx[4], ry, 18, mk.fuelPrice > 0.0 ? WHITE : OFF);
                if (mk.fuelPrice > 0.0) DrawText(TextFormat("fill: about %s", Thousands((FUEL_CAPACITY - p.fuel) * mk.fuelPrice).c_str()), cx[6] - 30, ry, 16, HUD_DIM);
            }
        }
        if (first > 0)             DrawText("^ more", px + pw - 90, py + 128, 14, HUD_DIM);
        if (first + VISIBLE < n)   DrawText("v more", px + pw - 90, py + 154 + 24 * VISIBLE - 2, 14, HUD_DIM);
        int fy = py + ph - 62;
        DrawLine(px + 18, fy - 38, px + pw - 18, fy - 38, HUD_DIM);
        DrawText(TextFormat("CREDITS  %s cr", Thousands(p.credits).c_str()), px + 30, fy, 20, HUD);
        DrawText(TextFormat("HOLD  %.1f / %.0f t", p.CargoTotal(), CARGO_CAPACITY), px + 350, fy, 20, HUD);
        DrawText(TextFormat("B: trade data, %.0f cr", TRADE_DATA_COST), px + 590, fy + 2, 16, HUD_LABEL);
        {   // What the hold of the picked good is really worth here: the price falls as you sell
            int pick = rows[std::min(row, n - 1)];
            if (pick >= 0 && p.cargo[pick] > 0.05 && mk.Buys(pick)) {
                double all = mk.SellValue(pick, p.cargo[pick]), flat = p.cargo[pick] * mk.SellPrice(pick);
                DrawText(TextFormat("Your %.1f t of %s would fetch about %s cr (%.0f cr/t)%s", p.cargo[pick], GoodData(pick).name, Thousands(all).c_str(),
                                    all / p.cargo[pick], all < 0.9 * flat ? "  -  more than it wants" : ""),
                         px + 30, fy - 32, 16, all < 0.9 * flat ? ORANGE : HUD_LABEL);
            }
        }
        DrawText(rows[std::min(row, n - 1)] >= 0
                     ? "W/S select   D buy 1 t   A sell 1 t   SHIFT: max   SPACE sell all   F / ESC close"
                     : "W/S select   D buy 1 t   SHIFT+D / SPACE fill tank   F / ESC close",
                 px + 30, fy + 30, 16, HUD_DIM);
    }

private:
    static const int VISIBLE = 14;
    TargetId world;
    int      row = 0;
    int      top = 0;
};

// ===========================================================================
// Comms: hail anyone in the system; ask for prices and news; open trade when
// you're in docking range. Any number of channels can be open at once: the
// list shows who's on a channel and whose reply is on its way, and a channel
// carries on while you look at another (or shut the window: the HUD keeps
// track of what's in flight). You keep flying.
// ===========================================================================
class CommsWindow : public Window {
public:
    explicit CommsWindow(TargetId focus = {}) : focus(focus) {}
    WindowKind Kind() const override { return WindowKind::Comms; }
    TargetId Focus() const { return focus; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        auto list = CommsInRange(g);
        if (focus.valid() && (focus.system != g.loaded.index || !FindContact(list, focus))) focus = {};    // they're gone
        if (!focus.valid()) {
            int n = (int)list.size();
            if (n > 0) {
                if (in.Pressed(KEY_UP))   row = (row + n - 1) % n;
                if (in.Pressed(KEY_DOWN)) row = (row + 1) % n;
                row = std::max(0, std::min(n - 1, row + in.WheelStep()));
                top = std::max(std::min(top, row), row - LIST_ROWS + 1);
                top = std::max(0, std::min(top, n - LIST_ROWS));
                if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER) || in.Pressed(KEY_RIGHT)) {
                    focus = TargetId::MakeBody(g.loaded.index, list[row].body);
                    if (!g.comms.Find(focus)) ui.Do(g, { Command::Hail, list[row].body });
                }
            }
        } else {
            bool open = g.comms.Find(focus) != nullptr;
            if (in.Pressed(KEY_ONE) || in.Pressed(KEY_KP_1)) ui.Do(g, { Command::MarketReport, focus.index });
            if (in.Pressed(KEY_TWO) || in.Pressed(KEY_KP_2)) ui.Do(g, { Command::LocalInfo, focus.index });
            if (in.Pressed(KEY_THREE) || in.Pressed(KEY_KP_3)) {
                const std::string &who = g.loaded.bodies[focus.index].name;
                if (CanTradeOverComms(g) && focus.index == g.flight.nearBody) { closed = true; ui.Open(std::make_unique<MarketWindow>(focus)); }
                else if (!CommsAnswered(g, g, focus))                         ui.Say("Wait for " + who + " to answer");
                else                                                          ui.Say("Get into docking range of " + who + " to trade");
            }
            if (in.Pressed(KEY_BACKSPACE)) { if (open) ui.Do(g, { Command::HangUp, focus.index }); focus = {}; }
            if (in.Pressed(KEY_LEFT) || in.Pressed(KEY_TAB)) focus = {};       // back to the list; the channel stays open
        }
        for (int k : { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_TAB, KEY_ENTER, KEY_KP_ENTER, KEY_ONE, KEY_KP_1, KEY_TWO, KEY_KP_2,
                       KEY_THREE, KEY_KP_3, KEY_BACKSPACE })
            in.Consume(k);
    }

    void Draw(const GameState &g) const override
    {
        auto list = CommsInRange(g);
        int px = 20, py = 230, pw = 560, ph = 330;
        DrawRectangle(px, py, pw, ph, Fade(BLACK, 0.85f));
        DrawRectangleLines(px, py, pw, ph, HUD);
        int x = px + 16, y = py + 12, right = px + pw - 16;
        const CommsContact *contact = focus.valid() && focus.system == g.loaded.index ? FindContact(list, focus) : nullptr;
        if (!contact) {
            DrawText("COMMUNICATIONS", x, y, 22, WHITE);
            const char *note = "the whole system  -  replies take light-time";
            DrawText(note, right - MeasureText(note, 14), y + 6, 14, HUD_DIM);
            y += 32; DrawLine(x, y, right, y, HUD_DIM); y += 10;
            if (list.empty()) {
                DrawText(g.loaded.index >= 0 ? "Nobody lives in this system." : "No one in range.", x, y, 18, LIGHTGRAY); y += 24;
                DrawText(g.loaded.index >= 0 ? "No one to hail here." : "Comms reach the system you're in; this is open space.", x, y, 16, HUD_DIM);
            } else {
                int shown = std::min(row, (int)list.size() - 1);
                if (top > 0) DrawText("...", right - 20, y - 14, 16, HUD_DIM);
                if (top + LIST_ROWS < (int)list.size()) DrawText("...", right - 20, py + ph - 44, 16, HUD_DIM);
                for (int r = top; r < (int)list.size() && r < top + LIST_ROWS; r++) {
                    const Body &b = g.loaded.bodies[list[r].body];
                    TargetId who = TargetId::MakeBody(g.loaded.index, list[r].body);
                    const CommsChannel *ch = g.comms.Find(who);
                    if (r == shown) DrawRectangle(x - 6, y - 3, pw - 20, 42, Fade(HUD, 0.18f));
                    DrawText(b.name.c_str(), x, y, 18, ch ? HUD : WHITE);
                    std::string away = FormatPlanetDist(list[r].dist) + "   reply in " + FormatETA(2.0 * list[r].dist / LIGHT_SEC_U + 0.6);
                    DrawText(away.c_str(), right - MeasureText(away.c_str(), 16), y + 2, 16, LIGHTGRAY);
                    // Who they are is on the charts, or it's just a signal until they answer
                    std::string what = Known(g, list[r].body)
                                     ? std::string(SettlementName(b.settlement)) + "  -  pop " + FormatPopulation(b.population)
                                     : std::string("Transponder signal  -  unscanned");
                    DrawText(what.c_str(), x, y + 20, 14, Known(g, list[r].body) ? HUD_LABEL : HUD_DIM);
                    if (ch) {
                        double soonest = 0.0;
                        for (const CommsMsg &m : ch->log) if (!m.ours && m.at > g.clock && (soonest == 0.0 || m.at < soonest)) soonest = m.at;
                        std::string st = soonest > 0.0 ? "CHANNEL OPEN  -  reply in " + FormatETA(soonest - g.clock) : std::string("CHANNEL OPEN");
                        DrawText(st.c_str(), right - MeasureText(st.c_str(), 14), y + 21, 14, soonest > 0.0 ? YELLOW : HUD);
                    }
                    y += 46;
                }
            }
            DrawText("UP/DOWN or wheel: select   ENTER: hail / open channel   F / ESC close", x, py + ph - 24, 16, HUD_DIM);
            return;
        }

        const CommsChannel *ch = g.comms.Find(focus);
        const Body &who = g.loaded.bodies[focus.index];
        DrawText(TextFormat("CHANNEL  -  %s", who.name.c_str()), x, y, 22, WHITE);
        std::string d = FormatPlanetDist(contact->dist) + " away  -  a reply takes " + FormatETA(2.0 * contact->dist / LIGHT_SEC_U + 0.6);
        DrawText(d.c_str(), x, y + 26, 14, HUD_DIM);
        y += 46; DrawLine(x, y, right, y, HUD_DIM); y += 10;

        // Word-wrap what's been said, then show the last lines that fit. Our own messages show
        // at once; theirs when they arrive.
        std::vector<std::pair<std::string, Color>> lines;
        int wrapW = pw - 32 - 20, inFlight = 0;
        double soonest = 0.0, counted = 0.0;
        if (ch)
            for (const CommsMsg &m : ch->log) {
                if (!m.ours && m.at > g.clock) {
                    if (soonest == 0.0 || m.at < soonest) soonest = m.at;
                    if (m.at != counted) { inFlight++; counted = m.at; }       // (count replies, not their lines)
                    continue;
                }
                Color c = m.ours ? HUD_LABEL : WHITE;
                std::string line = m.ours ? "> " : "", word;
                bool empty = true;
                for (size_t i = 0; i <= m.text.size(); i++) {
                    if (i < m.text.size() && m.text[i] != ' ') { word += m.text[i]; continue; }
                    if (word.empty()) { if (i < m.text.size() && !empty) line += ' '; continue; }   // keep runs of spaces (price columns)
                    std::string cand = line + word;
                    if (!empty && MeasureText(cand.c_str(), 16) > wrapW) { lines.push_back({ line, c }); cand = (m.ours ? "  " : "") + word; }
                    line = cand; empty = false; word.clear();
                    if (i < m.text.size()) line += ' ';
                }
                lines.push_back({ line, c });
            }
        if (!ch) lines.push_back({ "  opening channel...", HUD_DIM });
        if (inFlight > 0) {
            int dots = 1 + (int)(g.clock * 3.0) % 3;
            std::string wait = inFlight == 1 ? "  awaiting reply" : TextFormat("  awaiting %d replies", inFlight);
            lines.push_back({ wait + "  (" + FormatETA(soonest - g.clock) + ")" + std::string(dots, '.'), HUD_DIM });
        }
        int maxLines = (ph - 54 - 34) / 20;
        int first = std::max(0, (int)lines.size() - maxLines);
        for (int i = first; i < (int)lines.size(); i++, y += 20) DrawText(lines[i].first.c_str(), x, y, 16, lines[i].second);

        const char *asks = "1 prices   2 local info   ";
        bool trade = CanTradeOverComms(g) && focus.index == g.flight.nearBody;
        DrawText(asks, x, py + ph - 24, 16, HUD_DIM);
        int tx = x + MeasureText(asks, 16);
        const char *t = trade ? "3 TRADE   " : "3 trade (dock first)   ";
        DrawText(t, tx, py + ph - 24, 16, trade ? HUD : Fade(HUD_DIM, 0.6f));
        DrawText("LEFT: others   BKSP hang up", tx + MeasureText(t, 16), py + ph - 24, 16, HUD_DIM);
    }

private:
    static const int LIST_ROWS = 5;
    TargetId focus;              // the channel on show, or none: the list of everyone
    int row = 0, top = 0;
};

// ===========================================================================
// Ui
// ===========================================================================
bool Ui::Has(WindowKind k) const
{
    for (const auto &w : stack) if (w->Kind() == k && !w->closed) return true;
    return false;
}

Window *Ui::Find(WindowKind k)
{
    for (auto &w : stack) if (w->Kind() == k && !w->closed) return w.get();
    return nullptr;
}

void Ui::Open(std::unique_ptr<Window> w)
{
    Close(w->Kind());
    stack.push_back(std::move(w));
}

void Ui::Close(WindowKind k)
{
    for (auto &w : stack) if (w->Kind() == k) w->closed = true;
}

bool Ui::FlightPaused() const
{
    for (const auto &w : stack) if (w->Modal() && !w->closed) return true;
    return false;
}

bool Ui::GamePaused() const
{
    for (const auto &w : stack) if (w->PausesGame() && !w->closed) return true;
    return false;
}

bool Ui::WantsCursor() const
{
    for (const auto &w : stack) if (w->WantsCursor() && !w->closed) return true;
    return false;
}

void Ui::CloseAll()
{
    for (auto &w : stack) w->closed = true;
}

void Ui::Load(GameState &g, const std::string &path)
{
    std::string file = GetFileNameWithoutExt(path.c_str());
    if (LoadGame(g, path.c_str())) {
        CloseAll();
        gameReloaded = true;
        SetCourse({});
        g.Emit(EventType::GameLoaded, "Loaded \"" + file + "\"");
    } else {
        Say("Could not load " + file);
    }
}

void Ui::Do(GameState &g, const Command &cmd)
{
    if (sendCommand) sendCommand(cmd);
    else             ApplyCommand(g, g, cmd);
}

void Ui::HandleInput(InputFrame &in, GameState &g)
{
    gameReloaded = false;

    // Windows, top first
    for (int i = (int)stack.size() - 1; i >= 0; i--) {
        Window &w = *stack[i];
        if (w.closed) continue;
        w.HandleInput(in, g, *this);
        if (w.Modal()) in.ConsumeAll();
    }

    if (mainMenu) return;                                  // no game behind the menu: no hotkeys

    // Global hotkeys. Esc closes the top window; with none open it pauses.
    if (in.Pressed(KEY_ESCAPE)) {
        Window *top = nullptr;
        for (auto &w : stack) if (!w->closed) top = w.get();
        if (top) top->closed = true;
        else     Open(MakePauseMenu(online));
    }
    if (in.Pressed(KEY_O)) { settings.showOrbits = !settings.showOrbits; SaveSettings(settings); }
    if (in.Pressed(KEY_R)) Open(MakeFactionsWindow());     // (and takes R itself to close)
    if (in.Pressed(KEY_T)) Open(MakeTradeLog());
    if (in.Pressed(KEY_N)) Open(MakeNewsWindow());
    if (in.Pressed(KEY_M)) {                               // (an open map takes M itself)
        if (g.loaded.index >= 0 && g.flight.drive.inField) Open(MakeSystemMap());
        else                                               Open(MakeSectorMap());
    }
    // F: comms. It hails whoever the nose is on (or whoever you're docked beside); with
    // nobody to hail it opens the list of everyone in the system. C is the list alone.
    if (in.Pressed(KEY_F)) {
        int who = HailTarget(g);
        const TargetId &sel = g.flight.selected;
        TargetId with = who >= 0 ? TargetId::MakeBody(g.loaded.index, who) : TargetId{};
        CommsWindow *shown = static_cast<CommsWindow *>(Find(WindowKind::Comms));
        if (who >= 0 && !(shown && shown->Focus() == with)) {
            if (!g.comms.Find(with)) Do(g, { Command::Hail, who });      // (already talking: just bring the channel up)
            Open(std::make_unique<CommsWindow>(with));
        } else if (shown) {
            Close(WindowKind::Comms);
        } else {
            if (who < 0 && sel.type == TargetId::Body && sel.system == g.loaded.index && sel.index >= 0 &&
                sel.index < (int)g.loaded.bodies.size() && g.loaded.bodies[sel.index].kind != Kind::Star)
                Say("Nobody at " + g.loaded.bodies[sel.index].name + " to hail");
            Open(std::make_unique<CommsWindow>());
        }
    }
    if (in.Pressed(KEY_C)) {
        if (Has(WindowKind::Comms)) Close(WindowKind::Comms);
        else                        Open(std::make_unique<CommsWindow>());
    }
    if (in.Pressed(KEY_I) || in.MousePressed(MOUSE_BUTTON_MIDDLE)) {
        if (Has(WindowKind::Info))           Close(WindowKind::Info);
        else if (g.flight.selected.valid())  Open(std::make_unique<InfoWindow>());
    }
    if (online && (in.Pressed(KEY_F5) || in.Pressed(KEY_F9))) {
        Say("Online, the server saves your progress for you");
        in.Consume(KEY_F5); in.Consume(KEY_F9);
    }
    if (in.Pressed(KEY_F5)) {
        if (SaveGame(g, QuicksavePath().c_str(), "quicksave")) g.Emit(EventType::GameSaved, "Quicksaved");
        else                                                   Say("Could not save the game");
    }
    if (in.Pressed(KEY_F9)) {
        if (FileExists(QuicksavePath().c_str())) Load(g, QuicksavePath());
        else                                     Say("No quicksave yet - press F5 to make one");
    }
}

void Ui::Update(GameState &g, float frameDt)
{
    for (auto &w : stack) if (!w->closed && !w->StillValid(g)) w->closed = true;
    stack.erase(std::remove_if(stack.begin(), stack.end(), [](const std::unique_ptr<Window> &w) { return w->closed; }),
                stack.end());
    for (const GameEvent &e : g.events)
        if (!e.text.empty() && (e.player < 0 || e.player == g.id)) Say(e.text);   // yours, or for everyone
    toastTime = fmaxf(0.0f, toastTime - frameDt);
}

void Ui::Draw(const GameState &g) const
{
    for (const auto &w : stack) if (!w->closed) w->Draw(g);

    if (toastTime > 0.0f) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int w = MeasureText(toast.c_str(), 20);
        Color c = Fade(YELLOW, fminf(1.0f, toastTime));
        int y = WantsCursor() ? 24 : sh - 144;           // above the menus while one is open
        DrawRectangle(sw / 2 - w / 2 - 10, y, w + 20, 30, Fade(BLACK, 0.7f * fminf(1.0f, toastTime)));
        DrawText(toast.c_str(), sw / 2 - w / 2, y + 5, 20, c);
    }
}
