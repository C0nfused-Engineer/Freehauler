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

Controls ReadFlightControls(const InputFrame &in, float frameDt, const Settings &settings)
{
    Controls c;
    c.forward = (float)in.Down(KEY_W) - (float)in.Down(KEY_S);
    c.strafe  = (float)in.Down(KEY_D) - (float)in.Down(KEY_A);
    c.roll    = ((float)in.Down(KEY_Q) - (float)in.Down(KEY_E)) * ROLL_RATE * frameDt;
    c.yaw     = -in.mouseDelta.x * settings.mouseSens;
    c.pitch   = -in.mouseDelta.y * settings.mouseSens * (settings.invertY ? -1.0f : 1.0f);
    c.hyper   = in.Down(KEY_LEFT_SHIFT) || in.Down(KEY_RIGHT_SHIFT);
    c.fire    = in.MouseDown(MOUSE_BUTTON_LEFT);
    c.allStop         = in.Pressed(KEY_X);
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
// ===========================================================================
// The rows: what this market trades (exports, imports, trades, ores), then
// anything in the hold it doesn't, then fuel (-1)
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
        if (!b) { in.ConsumeAll(); return; }
        std::vector<int> rows = MarketRows(MakeMarket(*b), g.player);
        int n = (int)rows.size();
        if (in.Pressed(KEY_F) || in.Pressed(KEY_ESCAPE)) closed = true;
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = (row + n - 1) % n;
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = (row + 1) % n;
        if (in.Pressed(KEY_PAGE_UP))   row = std::max(0, row - VISIBLE);
        if (in.Pressed(KEY_PAGE_DOWN)) row = std::min(n - 1, row + VISIBLE);
        row = std::min(row, n - 1);
        bool shift = in.Down(KEY_LEFT_SHIFT) || in.Down(KEY_RIGHT_SHIFT);
        bool buy  = in.Pressed(KEY_RIGHT) || in.Pressed(KEY_D);
        bool sell = in.Pressed(KEY_LEFT)  || in.Pressed(KEY_A);
        // ENTER / SPACE: sell 1 t / all of a good, or buy 1 t / fill up on fuel
        bool one = in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER), all = in.Pressed(KEY_SPACE);
        int gd = rows[row];
        if (gd < 0) {
            if (buy || one || all) ui.Do(g, { Command::BuyFuel, 0, (shift || all) ? 1e30 : 1.0 });
        } else {
            if (buy)         ui.Do(g, { Command::BuyGoods,  gd, shift ? 1e30 : 1.0 });
            if (sell || one) ui.Do(g, { Command::SellGoods, gd, shift ? 1e30 : 1.0 });
            if (all)         ui.Do(g, { Command::SellGoods, gd, 1e30 });
        }
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
        Market mk = MakeMarket(*b);
        std::vector<int> rows = MarketRows(mk, p);
        int n = (int)rows.size(), first = std::max(0, std::min(top, n - VISIBLE));
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int pw = 760, ph = 150 + 24 * VISIBLE + 76;
        int px = sw / 2 - pw / 2, py = sh / 2 - ph / 2;
        DrawRectangle(px, py, pw, ph, Fade(BLACK, 0.9f));
        DrawRectangleLines(px, py, pw, ph, HUD);
        DrawText(TextFormat("MARKET  -  %s", b->name.c_str()), px + 18, py + 14, 24, WHITE);
        DrawText(TextFormat("%s  -  %s economy  -  population %s", b->settlement.c_str(), EconName(b->econ),
                            FormatPopulation(b->population).c_str()), px + 18, py + 44, 16, LIGHTGRAY);
        std::string ex = ExportsLine(mk, 4), im = ImportsLine(mk, 4);
        DrawText(TextFormat("Makes: %s", ex.empty() ? "-" : ex.c_str()), px + 18, py + 64, 14, Color{ 140, 220, 140, 255 });
        DrawText(TextFormat("Needs: %s", im.empty() ? "-" : im.c_str()), px + 18, py + 82, 14, Color{ 240, 170, 110, 255 });
        DrawLine(px + 18, py + 102, px + pw - 18, py + 102, HUD_DIM);
        int cx[] = { px + 30, px + 250, px + 350, px + 460, px + 570, px + 660 };
        int hy = py + 110;
        DrawText("ITEM", cx[0], hy, 16, HUD_LABEL);  DrawText("HOLD", cx[1], hy, 16, HUD_LABEL);
        DrawText("BUY", cx[2], hy, 16, HUD_LABEL);   DrawText("SELL", cx[3], hy, 16, HUD_LABEL);
        DrawText("VALUE", cx[4], hy, 16, HUD_LABEL); DrawText("", cx[5], hy, 16, HUD_LABEL);
        for (int i = 0; i < VISIBLE && first + i < n; i++) {
            int r = first + i, gd = rows[r];
            int ry = py + 136 + 24 * i;
            if (r == row) DrawRectangle(px + 18, ry - 4, pw - 36, 24, Fade(HUD, 0.18f));
            if (gd >= 0) {
                const GoodInfo &info = GoodData(gd);
                bool have = p.cargo[gd] > 0.05;
                Color c = have || mk.Sells(gd) ? info.color : Color{ 120, 120, 120, 255 };
                DrawText(info.name, cx[0], ry, 18, c);
                DrawText(have ? TextFormat("%.1f t", p.cargo[gd]) : "-", cx[1], ry, 18, c);
                DrawText(mk.Sells(gd) ? TextFormat("%.0f", mk.BuyPrice(gd)) : "-", cx[2], ry, 18, c);
                DrawText(mk.Buys(gd) ? TextFormat("%.0f", mk.SellPrice(gd)) : "-", cx[3], ry, 18, c);
                if (have && mk.Buys(gd)) DrawText(Thousands(p.cargo[gd] * mk.SellPrice(gd)).c_str(), cx[4], ry, 18, c);
                const char *tag = ""; Color tc = HUD_DIM;
                switch (mk.role[gd]) {
                case Role::Export: tag = "export"; tc = { 140, 220, 140, 255 }; break;
                case Role::Import: tag = "import"; tc = { 240, 170, 110, 255 }; break;
                case Role::Trade:  tag = "trade"; break;
                case Role::Ore:    tag = "ore";   break;
                default:           tag = "not traded"; tc = { 200, 90, 90, 255 }; break;
                }
                DrawText(tag, cx[5], ry, 16, tc);
            } else {
                DrawText("Fuel", cx[0], ry, 18, WHITE);
                DrawText(TextFormat("%.1f/%.0f", p.fuel, FUEL_CAPACITY), cx[1], ry, 18, WHITE);
                DrawText(TextFormat("%.0f", mk.fuelPrice), cx[2], ry, 18, WHITE);
                DrawText("-", cx[3], ry, 18, WHITE);
                DrawText(TextFormat("fill %s", Thousands((FUEL_CAPACITY - p.fuel) * mk.fuelPrice).c_str()), cx[4], ry, 18, WHITE);
            }
        }
        if (first > 0)             DrawText("^ more", px + pw - 90, py + 110, 14, HUD_DIM);
        if (first + VISIBLE < n)   DrawText("v more", px + pw - 90, py + 136 + 24 * VISIBLE - 2, 14, HUD_DIM);
        int fy = py + ph - 62;
        DrawLine(px + 18, fy - 8, px + pw - 18, fy - 8, HUD_DIM);
        DrawText(TextFormat("CREDITS  %s cr", Thousands(p.credits).c_str()), px + 30, fy, 20, HUD);
        DrawText(TextFormat("HOLD  %.1f / %.0f t", p.CargoTotal(), CARGO_CAPACITY), px + 350, fy, 20, HUD);
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
// Comms: hail inhabited worlds within 3 light seconds. You keep flying.
// ===========================================================================
class CommsWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::Comms; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        if (!g.comms.with.valid()) {
            auto list = CommsInRange(g);
            int n = (int)list.size();
            if (n > 0) {
                if (in.Pressed(KEY_UP))   row = (row + n - 1) % n;
                if (in.Pressed(KEY_DOWN)) row = (row + 1) % n;
                row = std::min(row, n - 1);
                if (in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) ui.Do(g, { Command::Hail, list[row].body });
            }
        } else {
            if (in.Pressed(KEY_ONE) || in.Pressed(KEY_KP_1)) ui.Do(g, { Command::MarketReport });
            if (in.Pressed(KEY_TWO) || in.Pressed(KEY_KP_2)) ui.Do(g, { Command::LocalInfo });
            if (in.Pressed(KEY_BACKSPACE)) ui.Do(g, { Command::HangUp });
        }
        for (int k : { KEY_UP, KEY_DOWN, KEY_ENTER, KEY_KP_ENTER, KEY_ONE, KEY_KP_1, KEY_TWO, KEY_KP_2, KEY_BACKSPACE })
            in.Consume(k);
    }

    void Draw(const GameState &g) const override
    {
        auto list = CommsInRange(g);
        int px = 20, py = 230, pw = 560, ph = 330;
        DrawRectangle(px, py, pw, ph, Fade(BLACK, 0.85f));
        DrawRectangleLines(px, py, pw, ph, HUD);
        int x = px + 16, y = py + 12, right = px + pw - 16;
        if (!g.comms.with.valid()) {
            DrawText("COMMUNICATIONS", x, y, 22, WHITE);
            DrawText("range 3 light seconds", right - MeasureText("range 3 light seconds", 14), y + 6, 14, HUD_DIM);
            y += 32; DrawLine(x, y, right, y, HUD_DIM); y += 10;
            if (list.empty()) {
                DrawText("No inhabited worlds in range.", x, y, 18, LIGHTGRAY); y += 24;
                DrawText("Get within 3 light seconds (~900,000 km) of one.", x, y, 16, HUD_DIM);
            } else {
                int shown = std::min(row, (int)list.size() - 1);
                for (int r = 0; r < (int)list.size() && r < 9; r++) {
                    const Body &b = g.loaded.bodies[list[r].body];
                    if (r == shown) DrawRectangle(x - 6, y - 3, pw - 20, 42, Fade(HUD, 0.18f));
                    DrawText(b.name.c_str(), x, y, 18, WHITE);
                    const char *d = TextFormat("%.2f ls   delay %.1f s", list[r].dist / LIGHT_SEC_U, 2.0 * list[r].dist / LIGHT_SEC_U);
                    DrawText(d, right - MeasureText(d, 16), y + 2, 16, LIGHTGRAY);
                    DrawText(TextFormat("%s  -  pop %s", b.settlement.c_str(), FormatPopulation(b.population).c_str()), x, y + 20, 14, HUD_LABEL);
                    y += 46;
                }
            }
            DrawText("UP/DOWN select   ENTER hail   C / ESC close", x, py + ph - 24, 16, HUD_DIM);
            return;
        }

        DrawText(TextFormat("CHANNEL  -  %s", g.comms.withName.c_str()), x, y, 22, WHITE);
        if (const CommsContact *c = FindContact(list, g.comms.with)) {
            const char *d = TextFormat("%.2f ls  -  round trip %.1f s", c->dist / LIGHT_SEC_U, 2.0 * c->dist / LIGHT_SEC_U);
            DrawText(d, x, y + 26, 14, HUD_DIM);
        }
        y += 46; DrawLine(x, y, right, y, HUD_DIM); y += 10;

        // Word-wrap the arrived messages, then show the last lines that fit
        std::vector<std::pair<std::string, Color>> lines;
        int wrapW = pw - 32 - 20;
        for (const CommsMsg &m : g.comms.log) {
            if (m.at > g.clock) continue;
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
        bool waiting = CommsWaiting(g);
        if (waiting) {
            int dots = 1 + (int)(g.clock * 3.0) % 3;
            lines.push_back({ std::string("  awaiting reply") + std::string(dots, '.'), HUD_DIM });
        }
        int maxLines = (ph - 54 - 34) / 20;
        int first = std::max(0, (int)lines.size() - maxLines);
        for (int i = first; i < (int)lines.size(); i++, y += 20) DrawText(lines[i].first.c_str(), x, y, 16, lines[i].second);
        DrawText(waiting ? "BACKSPACE hang up   C / ESC close"
                         : "1 market report   2 local info   BKSP hang up   C close",
                 x, py + ph - 24, 16, HUD_DIM);
    }

private:
    int row = 0;
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
    if (in.Pressed(KEY_C)) {
        if (Has(WindowKind::Comms)) Close(WindowKind::Comms);
        else                        Open(std::make_unique<CommsWindow>());
    }
    if (in.Pressed(KEY_I) || in.MousePressed(MOUSE_BUTTON_MIDDLE)) {
        if (Has(WindowKind::Info))           Close(WindowKind::Info);
        else if (g.flight.selected.valid())  Open(std::make_unique<InfoWindow>());
    }
    if (in.Pressed(KEY_F) && g.flight.nearBody >= 0 && g.flight.nearBody < (int)g.loaded.bodies.size()) {
        const Body &b = g.loaded.bodies[g.flight.nearBody];
        if (b.population > 0) {
            Close(WindowKind::Comms);
            Open(std::make_unique<MarketWindow>(TargetId::MakeBody(g.loaded.index, g.flight.nearBody)));
        } else {
            Say(b.name + " is uninhabited - nothing to trade here");
        }
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
