// menus.cpp - the main menu (single player: new / load; multiplayer: host / join;
// settings; exit) and the pause menu (Esc) with its sub-menus: save, load,
// settings, main menu, exit. All of them show the mouse pointer and work with
// either the mouse or the keyboard (Up/Down, Enter, Esc). In single player
// they stop the game; online the world keeps going.
#include "ui.h"
#include "format.h"
#include "save.h"
#include "server.h"
#include "theme.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

void ShowBusy(const char *text)
{
    if (!IsWindowReady()) return;
    BeginDrawing();
    ClearBackground(BLACK);
    DrawText(text, (GetScreenWidth() - MeasureText(text, 24)) / 2, GetScreenHeight() / 2 - 12, 24, HUD);
    EndDrawing();
}

namespace {

// ---------------------------------------------------------------------------
// Drawing helpers: black panels with HUD-green vector outlines
// ---------------------------------------------------------------------------
int ScreenW() { return GetScreenWidth(); }
int ScreenH() { return GetScreenHeight(); }

Rectangle Centered(float w, float h) { return { (ScreenW() - w) * 0.5f, (ScreenH() - h) * 0.5f, w, h }; }

void DimScreen() { DrawRectangle(0, 0, ScreenW(), ScreenH(), Fade(BLACK, 0.55f)); }

void DrawPanel(Rectangle r, const char *title)
{
    DrawRectangleRec(r, BLACK);
    DrawRectangleLinesEx(r, 1.0f, HUD);
    // Corner ticks, like the target brackets
    float c = 12.0f, x0 = r.x - 4, y0 = r.y - 4, x1 = r.x + r.width + 4, y1 = r.y + r.height + 4;
    DrawLineV({ x0, y0 }, { x0 + c, y0 }, HUD); DrawLineV({ x0, y0 }, { x0, y0 + c }, HUD);
    DrawLineV({ x1, y0 }, { x1 - c, y0 }, HUD); DrawLineV({ x1, y0 }, { x1, y0 + c }, HUD);
    DrawLineV({ x0, y1 }, { x0 + c, y1 }, HUD); DrawLineV({ x0, y1 }, { x0, y1 - c }, HUD);
    DrawLineV({ x1, y1 }, { x1 - c, y1 }, HUD); DrawLineV({ x1, y1 }, { x1, y1 - c }, HUD);
    int tw = MeasureText(title, 26);
    DrawText(title, (int)(r.x + (r.width - tw) / 2), (int)r.y + 18, 26, WHITE);
    DrawLine((int)r.x + 20, (int)r.y + 56, (int)(r.x + r.width) - 20, (int)r.y + 56, HUD_DIM);
}

void DrawButton(Rectangle r, const char *label, bool focus, bool enabled = true)
{
    Color edge = !enabled ? Color{ 60, 60, 60, 255 } : focus ? HUD : HUD_DIM;
    if (focus && enabled) DrawRectangleRec(r, Fade(HUD, 0.18f));
    DrawRectangleLinesEx(r, 1.0f, edge);
    int tw = MeasureText(label, 20);
    DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - 20) / 2), 20,
             !enabled ? Color{ 90, 90, 90, 255 } : focus ? WHITE : LIGHTGRAY);
}

bool Over(const InputFrame &in, Rectangle r) { return CheckCollisionPointRec(in.mousePos, r); }
bool Clicked(const InputFrame &in, Rectangle r) { return in.MousePressed(MOUSE_BUTTON_LEFT) && Over(in, r); }
bool MouseMoved(const InputFrame &in) { return in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f; }
bool Activate(const InputFrame &in) { return in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER) || in.Pressed(KEY_SPACE); }
bool Up(const InputFrame &in)   { return in.Pressed(KEY_UP) || in.Pressed(KEY_W); }
bool Down(const InputFrame &in) { return in.Pressed(KEY_DOWN) || in.Pressed(KEY_S); }
// The mouse wheel steps the highlight too, stopping at the ends (the arrow keys wrap around)
void WheelFocus(const InputFrame &in, int &focus, int count) { focus = std::max(0, std::min(count - 1, focus + in.WheelStep())); }

std::string FormatDate(long t)
{
    if (t <= 0) return "";
    time_t tt = (time_t)t;
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", localtime(&tt));
    return buf;
}

std::string FormatPlayTime(double seconds)
{
    long s = (long)seconds;
    if (s < 3600) return TextFormat("%ldm %02lds", s / 60, s % 60);
    return TextFormat("%ldh %02ldm", s / 3600, (s / 60) % 60);
}

// Text cut from the left with "..." so it fits a width (long folder paths)
std::string FitLeft(const std::string &s, int width, int size)
{
    if (MeasureText(s.c_str(), size) <= width) return s;
    for (size_t i = 1; i < s.size(); i++) {
        std::string t = "..." + s.substr(i);
        if (MeasureText(t.c_str(), size) <= width) return t;
    }
    return "...";
}

// The .txt files in a folder, newest first (the save menu's list)
struct SaveFile { std::string name, path; long modified; };
std::vector<SaveFile> ListSaves(const std::string &dir)
{
    std::vector<SaveFile> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2) || e.path().extension() != ".txt") continue;
        std::string path = e.path().string();
        out.push_back({ e.path().stem().string(), path, GetFileModTime(path.c_str()) });
    }
    std::sort(out.begin(), out.end(), [](const SaveFile &a, const SaveFile &b) { return a.modified > b.modified; });
    return out;
}

// ===========================================================================
// Confirmation: leave the game for the main menu, or exit to the desktop
// ===========================================================================
class ConfirmExitMenu : public Window {
public:
    ConfirmExitMenu(const Ui &ui, bool toMenu) : online(ui.online), toMenu(toMenu)
    {
        if (!ui.online) {
            title = toMenu ? "RETURN TO MAIN MENU?" : "EXIT TO DESKTOP?";
            msg   = "Progress since your last save will be lost.";
            yes   = toMenu ? "Main Menu" : "Exit";
        } else {
            title = toMenu ? "LEAVE THE SERVER?" : "EXIT TO DESKTOP?";
            msg   = !ui.hosting ? "The server keeps your ship, cargo and credits."
                  : ui.stopServerOnLeave ? "This closes your server for everyone (progress is saved)."
                                         : "Your server keeps running in its own window.";
            yes   = toMenu ? "Leave" : "Exit";
        }
    }
    WindowKind Kind() const override { return WindowKind::ConfirmExit; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return !online; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        (void)g;
        Layout L = Lay();
        if (in.Pressed(KEY_ESCAPE)) { closed = true; return; }
        if (in.Pressed(KEY_LEFT) || in.Pressed(KEY_RIGHT) || in.Pressed(KEY_TAB) || Up(in) || Down(in)) focus = 1 - focus;
        if (MouseMoved(in)) { if (Over(in, L.exit)) focus = 0; if (Over(in, L.cancel)) focus = 1; }
        bool exitNow = Clicked(in, L.exit) || (Activate(in) && focus == 0);
        bool cancel  = Clicked(in, L.cancel) || (Activate(in) && focus == 1);
        if (exitNow) { if (toMenu) ui.toMainMenu = true; else ui.quit = true; }
        if (cancel)  closed = true;
    }

    void Draw(const GameState &g) const override
    {
        (void)g;
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, title);
        DrawText(msg, (int)(L.panel.x + (L.panel.width - MeasureText(msg, 18)) / 2), (int)L.panel.y + 76, 18, LIGHTGRAY);
        DrawButton(L.exit, yes, focus == 0);
        DrawButton(L.cancel, "Cancel", focus == 1);
    }

private:
    struct Layout { Rectangle panel, exit, cancel; };
    Layout Lay() const
    {
        Layout L;
        L.panel  = Centered(520, 190);
        L.exit   = { L.panel.x + 40, L.panel.y + 124, 190, 44 };
        L.cancel = { L.panel.x + L.panel.width - 230, L.panel.y + 124, 190, 44 };
        return L;
    }
    int  focus = 1;           // Cancel by default
    bool online, toMenu;
    const char *title, *msg, *yes;
};

// ===========================================================================
// Save: type a name (or pick an existing save to overwrite)
// ===========================================================================
class SaveMenu : public Window {
public:
    explicit SaveMenu(const GameState &g)
    {
        saves = ListSaves(SaveDirectory());
        // Suggest "<system> <n>" with the first free number
        std::string base = g.systems[g.player.ship.anchor].name;
        for (int n = 1; n < 1000; n++) {
            name = base + " " + std::to_string(n);
            if (!FileExists(SavePathFor(name).c_str())) break;
        }
    }
    WindowKind Kind() const override { return WindowKind::SaveMenu; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        Layout L = Lay();
        if (in.Pressed(KEY_ESCAPE) || Clicked(in, L.cancel)) { closed = true; return; }

        // Typing
        std::string before = name;
        for (char c : in.text)
            if (name.size() < 40 && (isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.')) name += c;
        if (in.Pressed(KEY_BACKSPACE)) { if (!name.empty()) name.pop_back(); backHeld = 0.0f; }
        else if (in.Down(KEY_BACKSPACE)) {                                // hold to keep deleting
            backHeld += in.dt;
            if (backHeld > 0.45f && !name.empty()) { name.pop_back(); backHeld = 0.41f; }
        }
        if (name != before) { confirmOverwrite = false; error.clear(); }

        // Pick an existing save from the list (to overwrite it)
        int rows = std::min((int)saves.size(), LIST_ROWS);
        if (in.wheel != 0.0f) scroll = std::max(0, std::min(scroll - (int)in.wheel, std::max(0, (int)saves.size() - LIST_ROWS)));
        for (int i = 0; i < rows; i++)
            if (Clicked(in, Row(L, i))) { name = saves[scroll + i].name; confirmOverwrite = false; error.clear(); }

        if (MouseMoved(in)) { if (Over(in, L.save)) focus = 0; if (Over(in, L.cancel)) focus = 1; }
        if (in.Pressed(KEY_TAB)) focus = 1 - focus;
        bool save = Clicked(in, L.save) || ((in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) && focus == 0);
        if ((in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER)) && focus == 1) { closed = true; return; }
        if (!save) return;

        std::string trimmed = name;
        while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
        while (!trimmed.empty() && trimmed[0] == ' ') trimmed.erase(0, 1);
        if (!ValidSaveName(trimmed)) { error = "Use letters, numbers, spaces, - _ . (up to 40)"; return; }
        std::string path = SavePathFor(trimmed);
        if (FileExists(path.c_str()) && !confirmOverwrite) {
            confirmOverwrite = true;
            error = "\"" + trimmed + "\" exists - Save again to overwrite it";
            return;
        }
        if (SaveGame(g, path.c_str(), trimmed)) {
            g.Emit(EventType::GameSaved, "Saved \"" + trimmed + "\"");
            closed = true;
        } else {
            error = "Could not write " + path;
        }
        (void)ui;
    }

    void Draw(const GameState &g) const override
    {
        (void)g;
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, "SAVE GAME");
        int x = (int)L.panel.x + 30;
        DrawText("Name", x, (int)L.field.y - 24, 18, HUD_LABEL);
        DrawRectangleRec(L.field, BLACK);
        DrawRectangleLinesEx(L.field, 1.0f, HUD);
        DrawText(name.c_str(), (int)L.field.x + 10, (int)L.field.y + 10, 20, WHITE);
        if (fmod(GetTime(), 1.0) < 0.6)                                    // blinking cursor
            DrawRectangle((int)L.field.x + 12 + MeasureText(name.c_str(), 20), (int)L.field.y + 9, 2, 22, HUD);
        if (!error.empty()) DrawText(error.c_str(), x, (int)L.field.y + 48, 16, confirmOverwrite ? YELLOW : ORANGE);

        DrawText("Existing saves  (click one to overwrite it)", x, (int)L.list.y - 24, 16, HUD_LABEL);
        DrawRectangleLinesEx(L.list, 1.0f, HUD_DIM);
        if (saves.empty()) DrawText("None yet", x + 10, (int)L.list.y + 10, 18, GRAY);
        for (int i = 0; i < std::min((int)saves.size(), LIST_ROWS); i++) {
            const SaveFile &s = saves[scroll + i];
            Rectangle r = Row(L, i);
            bool same = s.name == name;
            if (same) DrawRectangleRec(r, Fade(HUD, 0.18f));
            DrawText(s.name.c_str(), (int)r.x + 10, (int)r.y + 5, 18, same ? WHITE : LIGHTGRAY);
            std::string d = FormatDate(s.modified);
            DrawText(d.c_str(), (int)(r.x + r.width) - 10 - MeasureText(d.c_str(), 16), (int)r.y + 6, 16, HUD_DIM);
        }
        DrawButton(L.save, confirmOverwrite ? "Overwrite" : "Save", focus == 0);
        DrawButton(L.cancel, "Cancel", focus == 1);
        DrawText("Type a name   ENTER save   ESC back", x, (int)(L.panel.y + L.panel.height) - 28, 16, HUD_DIM);
    }

private:
    static const int LIST_ROWS = 7;
    struct Layout { Rectangle panel, field, list, save, cancel; };
    Layout Lay() const
    {
        Layout L;
        L.panel  = Centered(560, 520);
        L.field  = { L.panel.x + 30, L.panel.y + 100, L.panel.width - 60, 40 };
        L.list   = { L.panel.x + 30, L.panel.y + 200, L.panel.width - 60, 30.0f * LIST_ROWS + 4 };
        L.save   = { L.panel.x + 30, L.panel.y + L.panel.height - 92, 200, 44 };
        L.cancel = { L.panel.x + L.panel.width - 230, L.panel.y + L.panel.height - 92, 200, 44 };
        return L;
    }
    static Rectangle Row(const Layout &L, int i) { return { L.list.x + 2, L.list.y + 2 + 30.0f * i, L.list.width - 4, 30 }; }

    std::string name, error;
    std::vector<SaveFile> saves;
    int   scroll = 0;
    int   focus = 0;              // 0 Save, 1 Cancel
    bool  confirmOverwrite = false;
    float backHeld = 0.0f;
};

// ===========================================================================
// Load: browse folders for a save file
// ===========================================================================
class LoadMenu : public Window {
public:
    LoadMenu() { Open(SaveDirectory()); }
    WindowKind Kind() const override { return WindowKind::LoadMenu; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        Layout L = Lay();
        clickTimer += in.dt > 0 ? in.dt : 1.0f / 60.0f;
        if (in.Pressed(KEY_ESCAPE) || Clicked(in, L.cancel)) { closed = true; return; }
        if (Clicked(in, L.home)) { Open(SaveDirectory()); return; }
        if (in.Pressed(KEY_BACKSPACE) && hasParent) { Open(parent); return; }

        int n = (int)entries.size();
        if (n > 0) {
            if (Up(in))   Select(selected - 1);
            if (Down(in)) Select(selected + 1);
            if (in.Pressed(KEY_PAGE_UP))   Select(selected - LIST_ROWS);
            if (in.Pressed(KEY_PAGE_DOWN)) Select(selected + LIST_ROWS);
        }
        if (in.wheel != 0.0f) scroll = std::max(0, std::min(scroll - (int)in.wheel * 2, std::max(0, n - LIST_ROWS)));

        bool open = in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER) || Clicked(in, L.load);
        for (int i = 0; i < LIST_ROWS && scroll + i < n; i++)
            if (Clicked(in, Row(L, i))) {
                int idx = scroll + i;
                if (idx == selected && clickTimer < 0.4f) open = true;          // double click
                Select(idx);
                clickTimer = 0.0f;
            }
        if (!open || selected < 0 || selected >= n) return;

        Entry e = entries[selected];
        if (e.dir) { Open(e.path); return; }
        if (!e.summary.valid) { message = "That isn't a save file"; return; }
        ui.Load(g, e.path);                                   // closes every menu on success
    }

    void Draw(const GameState &g) const override
    {
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, "LOAD GAME");
        std::string where = FitLeft(dir, (int)L.list.width, 16);
        DrawText(where.c_str(), (int)L.list.x, (int)L.list.y - 24, 16, HUD_LABEL);

        // File list
        DrawRectangleLinesEx(L.list, 1.0f, HUD_DIM);
        if (entries.empty()) DrawText("No save files here", (int)L.list.x + 10, (int)L.list.y + 10, 18, GRAY);
        for (int i = 0; i < LIST_ROWS && scroll + i < (int)entries.size(); i++) {
            const Entry &e = entries[scroll + i];
            Rectangle r = Row(L, i);
            bool sel = scroll + i == selected;
            if (sel) DrawRectangleRec(r, Fade(HUD, 0.18f));
            Color c = e.dir ? HUD_LABEL : e.summary.valid ? (sel ? WHITE : LIGHTGRAY) : GRAY;
            std::string label = !e.dir ? e.label : e.label == ".." ? ".. (up a folder)" : e.label + "/";
            DrawText(label.c_str(), (int)r.x + 10, (int)r.y + 5, 18, c);
            if (!e.dir) {
                std::string d = FormatDate(e.modified);
                DrawText(d.c_str(), (int)(r.x + r.width) - 10 - MeasureText(d.c_str(), 14), (int)r.y + 7, 14, HUD_DIM);
            }
        }
        if ((int)entries.size() > LIST_ROWS) {                   // scroll bar
            float frac = (float)LIST_ROWS / entries.size();
            float pos  = (float)scroll / (entries.size() - LIST_ROWS);
            float h = L.list.height * frac;
            DrawRectangle((int)(L.list.x + L.list.width) - 5, (int)(L.list.y + (L.list.height - h) * pos), 3, (int)h, HUD_DIM);
        }

        // Details of the selected save
        int x = (int)L.details.x, y = (int)L.details.y;
        DrawRectangleLinesEx(L.details, 1.0f, HUD_DIM);
        x += 14; y += 12;
        if (selected >= 0 && selected < (int)entries.size() && !entries[selected].dir) {
            const Entry &e = entries[selected];
            const SaveSummary &s = e.summary;
            if (!s.valid) {
                DrawText("Not a save file", x, y, 18, ORANGE);
            } else {
                auto row = [&](const char *label, const std::string &value) {
                    DrawText(label, x, y, 16, HUD_LABEL);
                    DrawText(value.c_str(), x + 90, y, 16, WHITE);
                    y += 24;
                };
                DrawText((s.name.empty() ? e.label : s.name).c_str(), x, y, 20, WHITE); y += 34;
                row("Location", s.location.empty() ? "Unknown" : s.location);
                row("Played", FormatPlayTime(s.clock));
                row("Credits", Thousands(s.credits) + " cr");
                row("Fuel", TextFormat("%.1f t", s.fuel));
                row("Cargo", TextFormat("%.1f t", s.cargo));
                row("Saved", FormatDate(e.modified));
                if (s.seed != g.seed) { y += 6; DrawText(TextFormat("Different galaxy (seed %u)", s.seed), x, y, 16, YELLOW); }
            }
        } else {
            DrawText("Select a save", x, y, 18, GRAY);
        }
        if (!message.empty()) DrawText(message.c_str(), (int)L.list.x, (int)(L.list.y + L.list.height) + 10, 16, ORANGE);

        bool canLoad = selected >= 0 && selected < (int)entries.size() && (entries[selected].dir || entries[selected].summary.valid);
        bool isDir   = canLoad && entries[selected].dir;
        DrawButton(L.load, isDir ? "Open" : "Load", true, canLoad);
        DrawButton(L.home, "Saves folder", false);
        DrawButton(L.cancel, "Cancel", false);
        DrawText("UP/DOWN select   ENTER load / open   BACKSPACE up a folder   ESC back",
                 (int)L.panel.x + 30, (int)(L.panel.y + L.panel.height) - 28, 16, HUD_DIM);
    }

private:
    static const int LIST_ROWS = 11;
    struct Entry { std::string label, path; bool dir; long modified; SaveSummary summary; };
    struct Layout { Rectangle panel, list, details, load, home, cancel; };
    Layout Lay() const
    {
        Layout L;
        L.panel   = Centered(820, 560);
        L.list    = { L.panel.x + 30, L.panel.y + 100, 440, 30.0f * LIST_ROWS + 4 };
        L.details = { L.list.x + L.list.width + 20, L.list.y, L.panel.x + L.panel.width - 30 - (L.list.x + L.list.width + 20), L.list.height };
        float by = L.panel.y + L.panel.height - 92;
        L.load    = { L.panel.x + 30, by, 180, 44 };
        L.home    = { L.panel.x + 230, by, 180, 44 };
        L.cancel  = { L.panel.x + L.panel.width - 210, by, 180, 44 };
        return L;
    }
    static Rectangle Row(const Layout &L, int i) { return { L.list.x + 2, L.list.y + 2 + 30.0f * i, L.list.width - 10, 30 }; }

    void Select(int i)
    {
        if (entries.empty()) { selected = -1; return; }
        selected = std::max(0, std::min(i, (int)entries.size() - 1));
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + LIST_ROWS) scroll = selected - LIST_ROWS + 1;
        message.clear();
    }

    // List a folder: ".." first, then subfolders, then .txt files (newest first)
    void Open(const std::string &path)
    {
        std::error_code ec;
        fs::path p = fs::absolute(fs::path(path), ec);
        if (p.filename().empty() && p != p.root_path()) p = p.parent_path();    // drop a trailing separator
        dir = p.string();
        entries.clear();
        message.clear();
        hasParent = p != p.root_path() && !p.parent_path().empty() && p.parent_path() != p;
        parent = p.parent_path().string();
        if (hasParent) entries.push_back({ "..", parent, true, 0, {} });

        std::vector<Entry> dirs, files;
        for (const auto &e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code ec2;
            std::string name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;                    // hidden
            if (e.is_directory(ec2)) dirs.push_back({ name, e.path().string(), true, 0, {} });
            else if (e.is_regular_file(ec2) && e.path().extension() == ".txt") {
                std::string fp = e.path().string();
                files.push_back({ e.path().stem().string(), fp, false, GetFileModTime(fp.c_str()), ReadSaveSummary(fp.c_str()) });
            }
        }
        std::sort(dirs.begin(), dirs.end(), [](const Entry &a, const Entry &b) { return a.label < b.label; });
        std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) { return a.modified > b.modified; });
        entries.insert(entries.end(), dirs.begin(), dirs.end());
        int firstFile = (int)entries.size();
        entries.insert(entries.end(), files.begin(), files.end());
        scroll = 0;
        selected = -1;
        int pick = files.empty() ? 0 : firstFile;                         // the newest real save
        for (int i = firstFile; i < (int)entries.size(); i++) if (entries[i].summary.valid) { pick = i; break; }
        Select(pick);
        if (ec) message = "Can't read this folder";
    }

    std::string dir, parent, message;
    bool  hasParent = false;
    std::vector<Entry> entries;
    int   selected = -1, scroll = 0;
    float clickTimer = 1.0f;
};

// ===========================================================================
// Settings
// ===========================================================================
class SettingsMenu : public Window {
public:
    SettingsMenu(Settings &s, bool online) : settings(s), online(online) {}
    WindowKind Kind() const override { return WindowKind::SettingsMenu; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return !online; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        (void)g; (void)ui;
        Layout L = Lay();
        if (in.Pressed(KEY_ESCAPE) || Clicked(in, L.back)) { closed = true; return; }
        if (Up(in))   focus = (focus + ROWS) % (ROWS + 1);
        if (Down(in)) focus = (focus + 1) % (ROWS + 1);
        WheelFocus(in, focus, ROWS + 1);
        if (MouseMoved(in)) {
            for (int i = 0; i < ROWS; i++) if (Over(in, Row(L, i))) focus = i;
            if (Over(in, L.back)) focus = ROWS;
        }
        if (Activate(in) && focus == ROWS) { closed = true; return; }

        Settings before = settings;
        int step = in.Pressed(KEY_RIGHT) || in.Pressed(KEY_D) ? 1 : in.Pressed(KEY_LEFT) || in.Pressed(KEY_A) ? -1 : 0;
        if (focus == 0) {
            if (step) settings.mouseSens += 0.01f * step;
            Rectangle bar = Slider(L);
            Rectangle grab = { bar.x - 6, bar.y - 10, bar.width + 12, bar.height + 20 };
            if (in.MousePressed(MOUSE_BUTTON_LEFT) && Over(in, grab)) dragging = true;
            if (!in.MouseDown(MOUSE_BUTTON_LEFT)) dragging = false;
            if (dragging) {
                float t = Clamp((in.mousePos.x - bar.x) / bar.width, 0.0f, 1.0f);
                settings.mouseSens = MOUSE_SENS_MIN + t * (MOUSE_SENS_MAX - MOUSE_SENS_MIN);
            }
            settings.mouseSens = Clamp(settings.mouseSens, MOUSE_SENS_MIN, MOUSE_SENS_MAX);
        } else if (focus < ROWS && (step != 0 || Activate(in) || Clicked(in, Row(L, focus)))) {
            bool *flag = Flag(focus);
            *flag = !*flag;
        }
        if (settings.fullscreen != before.fullscreen) ToggleBorderlessWindowed();
        if (settings.mouseSens != before.mouseSens || settings.invertY != before.invertY || settings.showOrbits != before.showOrbits ||
            settings.showHelp != before.showHelp || settings.fullscreen != before.fullscreen)
            SaveSettings(settings);
    }

    void Draw(const GameState &g) const override
    {
        (void)g;
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, "SETTINGS");
        static const char *labels[ROWS] = { "Mouse sensitivity", "Invert mouse Y", "Orbit rings", "Key help line", "Fullscreen" };
        for (int i = 0; i < ROWS; i++) {
            Rectangle r = Row(L, i);
            bool f = focus == i;
            if (f) DrawRectangleRec(r, Fade(HUD, 0.18f));
            DrawText(labels[i], (int)r.x + 12, (int)r.y + 11, 20, f ? WHITE : LIGHTGRAY);
            if (i == 0) {
                Rectangle bar = Slider(L);
                float t = (settings.mouseSens - MOUSE_SENS_MIN) / (MOUSE_SENS_MAX - MOUSE_SENS_MIN);
                DrawLine((int)bar.x, (int)(bar.y + bar.height / 2), (int)(bar.x + bar.width), (int)(bar.y + bar.height / 2), HUD_DIM);
                DrawRectangle((int)bar.x, (int)(bar.y + bar.height / 2) - 1, (int)(bar.width * t), 3, HUD);
                float kx = bar.x + bar.width * t;
                DrawRectangleLines((int)kx - 5, (int)bar.y - 2, 10, (int)bar.height + 4, f ? WHITE : HUD);
                DrawText(TextFormat("%.2f", settings.mouseSens), (int)(r.x + r.width) - 52, (int)r.y + 13, 18, WHITE);
            } else {
                bool on = *const_cast<SettingsMenu *>(this)->Flag(i);
                const char *v = on ? "ON" : "OFF";
                Rectangle box = { r.x + r.width - 80, r.y + 8, 66, 28 };
                if (on) DrawRectangleRec(box, Fade(HUD, 0.25f));
                DrawRectangleLinesEx(box, 1.0f, on ? HUD : HUD_DIM);
                DrawText(v, (int)(box.x + (box.width - MeasureText(v, 18)) / 2), (int)box.y + 5, 18, on ? WHITE : GRAY);
            }
        }
        DrawButton(L.back, "Back", focus == ROWS);
        DrawText("UP/DOWN select   LEFT/RIGHT or ENTER change   ESC back",
                 (int)L.panel.x + 30, (int)(L.panel.y + L.panel.height) - 28, 16, HUD_DIM);
    }

private:
    static const int ROWS = 5;
    struct Layout { Rectangle panel, back; };
    Layout Lay() const
    {
        Layout L;
        L.panel = Centered(560, 140 + 50.0f * ROWS + 90);
        L.back  = { L.panel.x + (L.panel.width - 200) / 2, L.panel.y + L.panel.height - 92, 200, 44 };
        return L;
    }
    static Rectangle Row(const Layout &L, int i) { return { L.panel.x + 30, L.panel.y + 80 + 50.0f * i, L.panel.width - 60, 44 }; }
    static Rectangle Slider(const Layout &L) { Rectangle r = Row(L, 0); return { r.x + 230, r.y + 16, r.width - 300, 12 }; }
    bool *Flag(int i)
    {
        switch (i) {
        case 1:  return &settings.invertY;
        case 2:  return &settings.showOrbits;
        case 3:  return &settings.showHelp;
        default: return &settings.fullscreen;
        }
    }

    Settings &settings;
    bool online;
    int  focus = 0;
    bool dragging = false;
};

// ===========================================================================
// Pause menu. Online there's no save / load (the server keeps your progress)
// and the game doesn't stop: the other captains keep flying.
// ===========================================================================
class PauseMenu : public Window {
public:
    explicit PauseMenu(bool online) : online(online)
    {
        if (online) items = { Resume, Settings_, ToMenu, Exit };
        else        items = { Resume, Save, Load, Settings_, ToMenu, Exit };
    }
    WindowKind Kind() const override { return WindowKind::Pause; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return !online; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        Layout L = Lay();
        int count = (int)items.size();
        if (in.Pressed(KEY_ESCAPE)) { closed = true; return; }
        if (Up(in))   focus = (focus + count - 1) % count;
        if (Down(in)) focus = (focus + 1) % count;
        WheelFocus(in, focus, count);
        int chosen = Activate(in) ? focus : -1;
        for (int i = 0; i < count; i++) {
            if (MouseMoved(in) && Over(in, L.button[i])) focus = i;
            if (Clicked(in, L.button[i])) chosen = i;
        }
        if (chosen < 0) return;
        switch (items[chosen]) {
        case Resume:    closed = true; break;
        case Save:      ui.Open(std::make_unique<SaveMenu>(g)); break;
        case Load:      ui.Open(std::make_unique<LoadMenu>()); break;
        case Settings_: ui.Open(std::make_unique<SettingsMenu>(ui.settings, online)); break;
        case ToMenu:    ui.Open(std::make_unique<ConfirmExitMenu>(ui, true)); break;
        case Exit:      ui.Open(std::make_unique<ConfirmExitMenu>(ui, false)); break;
        }
    }

    void Draw(const GameState &g) const override
    {
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, online ? "MENU" : "PAUSED");
        for (int i = 0; i < (int)items.size(); i++) {
            static const char *labels[] = { "Resume", "Save Game", "Load Game", "Settings", "Main Menu", "Exit to Desktop" };
            DrawButton(L.button[i], items[i] == ToMenu && online ? "Leave Server" : labels[items[i]], focus == i);
        }
        std::string where = g.systems[g.player.ship.anchor].name + "  -  " + Thousands(g.player.credits) + " cr";
        DrawText(where.c_str(), (int)(L.panel.x + (L.panel.width - MeasureText(where.c_str(), 16)) / 2),
                 (int)(L.panel.y + L.panel.height) - 30, 16, HUD_DIM);
    }

private:
    enum Item { Resume, Save, Load, Settings_, ToMenu, Exit };
    static const int MAX_ITEMS = 6;
    struct Layout { Rectangle panel, button[MAX_ITEMS]; };
    Layout Lay() const
    {
        Layout L;
        int count = (int)items.size();
        L.panel = Centered(360, 80 + 58.0f * count + 50);
        for (int i = 0; i < count; i++) L.button[i] = { L.panel.x + 40, L.panel.y + 78 + 58.0f * i, L.panel.width - 80, 46 };
        return L;
    }
    bool online;
    std::vector<Item> items;
    int focus = 0;
};

// ===========================================================================
// A text field (the host / join forms)
// ===========================================================================
struct TextField {
    enum Allow { Any, Name, Digits, Address };
    std::string label, value, hint;
    Allow  allow  = Any;
    size_t maxLen = 40;
    bool   secret = false;                 // shown as stars (passwords)
    float  held   = 0.0f;                  // backspace held down (repeat)

    bool Ok(char c) const
    {
        switch (allow) {
        case Name:    return isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_';
        case Digits:  return isdigit((unsigned char)c) != 0;
        case Address: return isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '_';
        default:      return c >= 32 && c < 127;
        }
    }
    void Edit(const InputFrame &in)
    {
        for (char c : in.text) if (value.size() < maxLen && Ok(c)) value += c;
        if (in.Pressed(KEY_BACKSPACE)) { if (!value.empty()) value.pop_back(); held = 0.0f; }
        else if (in.Down(KEY_BACKSPACE)) {
            held += in.dt;
            if (held > 0.45f && !value.empty()) { value.pop_back(); held = 0.41f; }
        }
    }
    void Draw(Rectangle box, bool focus) const
    {
        DrawRectangleRec(box, BLACK);
        DrawRectangleLinesEx(box, 1.0f, focus ? HUD : HUD_DIM);
        std::string shown = secret ? std::string(value.size(), '*') : value;
        int tx = (int)box.x + 10, ty = (int)(box.y + (box.height - 20) / 2);
        if (shown.empty() && !focus) DrawText(hint.c_str(), tx, ty + 2, 16, GRAY);
        else                         DrawText(shown.c_str(), tx, ty, 20, WHITE);
        if (focus && fmod(GetTime(), 1.0) < 0.6)
            DrawRectangle(tx + 2 + MeasureText(shown.c_str(), 20), ty - 1, 2, 22, HUD);
    }
};

// ===========================================================================
// Host a server / join a server
// ===========================================================================
class ServerForm : public Window {
public:
    // host: start a server and join it. solo: a new single player game. Neither: join a server.
    ServerForm(bool host, const Settings &s, bool solo = false) : host(host), solo(solo)
    {
        const TextField ship = { "Ship name", s.shipName, "what she's called", TextField::Name, (size_t)MAX_PLAYER_NAME };
        const TextField seed = { "Galaxy seed", "", "blank = a random galaxy", TextField::Any, 24 };
        const TextField captain = { "Captain name", s.playerName, "what other captains see", TextField::Name, (size_t)MAX_PLAYER_NAME };
        if (solo) {
            fields = { ship, seed };
            iShip = 0; iSeed = 1;
        } else if (host) {
            fields = { captain, ship,
                       { "Port", std::to_string(s.hostPort), "23500", TextField::Digits, 5 },
                       { "Password", "", "optional", TextField::Any, 40, true },
                       { "Max players", std::to_string(s.hostMaxPlayers), "16", TextField::Digits, 3 },
                       seed };
            iName = 0; iShip = 1; iPort = 2; iPassword = 3; iMax = 4; iSeed = 5;
            stopOnLeave = s.hostStopOnLeave;
            lan = NetLocalAddress();
        } else {
            fields = { { "Server address", s.lastServer, "e.g. 192.168.1.20  or  myserver.net:23500", TextField::Address, 80 },
                       captain, ship,
                       { "Password", "", "if it has one", TextField::Any, 40, true } };
            iAddress = 0; iName = 1; iShip = 2; iPassword = 3;
        }
        for (int i = 0; i < (int)fields.size(); i++) if (fields[i].value.empty() && i != iSeed) { focus = i; break; }
    }
    WindowKind Kind() const override { return solo ? WindowKind::NewGameMenu : host ? WindowKind::HostMenu : WindowKind::JoinMenu; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        Layout L = Lay();
        int rows = Rows();
        if (in.Pressed(KEY_ESCAPE) || Clicked(in, L.back)) { closed = true; return; }
        if (iSeed >= 0 && Clicked(in, RollBox(L))) {                // a random seed, shown so it can be passed on
            fields[iSeed].value = std::to_string(SeedFromText("", (uint64_t)(GetTime() * 1e6) ^ ((uint64_t)in.mousePos.x << 32)));
            focus = iSeed;
            return;
        }
        // Arrows and Tab move between rows (W and S are letters here, not "up" and "down")
        bool shift = in.Down(KEY_LEFT_SHIFT) || in.Down(KEY_RIGHT_SHIFT);
        if (in.Pressed(KEY_UP) || (in.Pressed(KEY_TAB) && shift)) focus = (focus + rows - 1) % rows;
        else if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_TAB))     focus = (focus + 1) % rows;
        WheelFocus(in, focus, rows);
        for (int i = 0; i < (int)fields.size(); i++) if (Clicked(in, FieldBox(L, i))) focus = i;
        if (host && Clicked(in, L.toggle)) { stopOnLeave = !stopOnLeave; focus = ToggleRow(); }
        if (MouseMoved(in)) { if (Over(in, L.go)) focus = GoRow(); if (Over(in, L.back)) focus = GoRow() + 1; }

        if (focus < (int)fields.size()) {
            std::string before = fields[focus].value;
            fields[focus].Edit(in);
            if (fields[focus].value != before) error.clear();
        }
        bool enter = in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER);
        if (host && focus == ToggleRow() && (enter || in.Pressed(KEY_SPACE) || in.Pressed(KEY_LEFT) || in.Pressed(KEY_RIGHT))) {
            stopOnLeave = !stopOnLeave;
            return;
        }
        if (focus == GoRow() + 1 && enter) { closed = true; return; }
        if (enter || Clicked(in, L.go)) Submit(ui, g);
    }

    void Draw(const GameState &g) const override
    {
        (void)g;
        Layout L = Lay();
        DimScreen();
        DrawPanel(L.panel, solo ? "NEW GAME" : host ? "HOST A SERVER" : "JOIN A SERVER");
        for (int i = 0; i < (int)fields.size(); i++) {
            Rectangle b = FieldBox(L, i);
            DrawText(fields[i].label.c_str(), (int)L.panel.x + 30, (int)b.y + 10, 18, focus == i ? WHITE : HUD_LABEL);
            fields[i].Draw(b, focus == i);
        }
        if (iSeed >= 0) {
            Rectangle r = RollBox(L);
            DrawRectangleLinesEx(r, 1.0f, HUD_DIM);
            DrawText("RANDOM", (int)(r.x + (r.width - MeasureText("RANDOM", 16)) / 2), (int)r.y + 11, 16, HUD_LABEL);
        }
        int y = (int)(Box(L, (int)fields.size() - 1).y + 56);
        int x = (int)L.panel.x + 30;
        if (host) {
            bool f = focus == ToggleRow();
            if (f) DrawRectangleRec(L.toggle, Fade(HUD, 0.18f));
            DrawText("Stop the server when I leave", (int)L.toggle.x + 10, (int)L.toggle.y + 9, 18, f ? WHITE : LIGHTGRAY);
            Rectangle sw = { L.toggle.x + L.toggle.width - 76, L.toggle.y + 5, 66, 28 };
            if (stopOnLeave) DrawRectangleRec(sw, Fade(HUD, 0.25f));
            DrawRectangleLinesEx(sw, 1.0f, stopOnLeave ? HUD : HUD_DIM);
            const char *v = stopOnLeave ? "ON" : "OFF";
            DrawText(v, (int)(sw.x + (sw.width - MeasureText(v, 18)) / 2), (int)sw.y + 5, 18, stopOnLeave ? WHITE : GRAY);
            y = (int)(L.toggle.y + L.toggle.height + 14);
            const std::string &port = fields[iPort].value.empty() ? fields[iPort].hint : fields[iPort].value;
            DrawText("Starts a dedicated server in its own window; you join it like everyone else.", x, y, 16, HUD_DIM); y += 22;
            DrawText("The seed is for a new world: a server that already has a galaxy keeps it.", x, y, 16, HUD_DIM); y += 22;
            DrawText(TextFormat("Players on your network join:  %s:%s", lan.c_str(), port.c_str()), x, y, 16, HUD_LABEL); y += 22;
            DrawText(TextFormat("Over the internet: forward UDP %s on your router, or use Tailscale.", port.c_str()), x, y, 16, HUD_DIM);
        } else if (solo) {
            DrawText("The same seed always makes the same galaxy: its stars, its powers, its pirates.", x, y, 16, HUD_DIM); y += 22;
            DrawText("Type a number or any words, or leave it blank for one nobody has seen.", x, y, 16, HUD_DIM);
        } else {
            DrawText("Ask the host for the address. Port 23500 unless they say otherwise.", x, y, 16, HUD_DIM);
        }
        if (!error.empty()) DrawText(error.c_str(), x, (int)L.go.y - 30, 16, ORANGE);
        DrawButton(L.go, solo ? "Launch" : host ? "Start Server" : "Join", focus == GoRow());
        DrawButton(L.back, "Back", focus == GoRow() + 1);
        DrawText("TAB / arrows move   ENTER go   ESC back", x, (int)(L.panel.y + L.panel.height) - 28, 16, HUD_DIM);
    }

private:
    struct Layout { Rectangle panel, toggle, go, back; };
    int Rows() const      { return (int)fields.size() + (host ? 1 : 0) + 2; }
    int ToggleRow() const { return (int)fields.size(); }
    int GoRow() const     { return (int)fields.size() + (host ? 1 : 0); }
    Layout Lay() const
    {
        Layout L;
        float h = 90 + 50.0f * fields.size() + (host ? 50 + 102 : solo ? 62 : 40) + 40 + 100;
        L.panel  = Centered(680, h);
        float y  = L.panel.y + 80 + 50.0f * fields.size() + 6;
        L.toggle = { L.panel.x + 24, y, L.panel.width - 48, 38 };
        L.go     = { L.panel.x + 30, L.panel.y + L.panel.height - 92, 200, 44 };
        L.back   = { L.panel.x + L.panel.width - 230, L.panel.y + L.panel.height - 92, 200, 44 };
        return L;
    }
    static Rectangle Box(const Layout &L, int i) { return { L.panel.x + 210, L.panel.y + 80 + 50.0f * i, L.panel.width - 240, 38 }; }
    // (the seed's box leaves room for its RANDOM button)
    Rectangle FieldBox(const Layout &L, int i) const { Rectangle b = Box(L, i); if (i == iSeed) b.width -= 110; return b; }
    Rectangle RollBox(const Layout &L) const { Rectangle b = Box(L, iSeed); return { b.x + b.width - 100, b.y, 100, b.height }; }

    void Submit(Ui &ui, GameState &g)
    {
        MenuChoice c;
        c.ship = fields[iShip].value;
        while (!c.ship.empty() && c.ship.back() == ' ') c.ship.pop_back();
        if (iSeed >= 0) c.seed = fields[iSeed].value;
        ui.settings.shipName = c.ship;
        if (solo) {
            ShowBusy("CHARTING A NEW GALAXY...");
            NewGame(g, SeedFromText(c.seed, (uint64_t)(GetTime() * 1e6)));
            g.shipName = c.ship;
            SaveSettings(ui.settings);
            ui.choice.kind = MenuChoice::Play;
            return;
        }
        if (host) {
            c.name = fields[iName].value;
            int port = atoi((fields[iPort].value.empty() ? fields[iPort].hint : fields[iPort].value).c_str());
            c.password = fields[iPassword].value;
            c.maxPlayers = atoi((fields[iMax].value.empty() ? fields[iMax].hint : fields[iMax].value).c_str());
            if (!ValidPlayerName(c.name)) { error = "Captain name: 1-20 letters, digits, spaces, - or _"; focus = iName; return; }
            if (port < 1024 || port > 65535) { error = "Pick a port from 1024 to 65535"; focus = iPort; return; }
            if (c.maxPlayers < 1 || c.maxPlayers > 64) { error = "Max players: 1 to 64"; focus = iMax; return; }
            c.kind = MenuChoice::Host;
            c.address = "127.0.0.1";
            c.port = (uint16_t)port;
            c.stopOnLeave = stopOnLeave;
            ui.settings.hostPort = port;
            ui.settings.hostMaxPlayers = c.maxPlayers;
            ui.settings.hostStopOnLeave = stopOnLeave;
        } else {
            std::string addr = fields[iAddress].value;
            c.name = fields[iName].value;
            c.password = fields[iPassword].value;
            c.port = DEFAULT_PORT;
            size_t colon = addr.rfind(':');
            if (colon != std::string::npos) {
                int port = atoi(addr.c_str() + colon + 1);
                if (port < 1 || port > 65535) { error = "The port after ':' should be a number up to 65535"; focus = iAddress; return; }
                c.port = (uint16_t)port;
                addr.resize(colon);
            }
            if (addr.empty()) { error = "Type the server's address"; focus = iAddress; return; }
            if (!ValidPlayerName(c.name)) { error = "Captain name: 1-20 letters, digits, spaces, - or _"; focus = iName; return; }
            c.kind = MenuChoice::Join;
            c.address = addr;
            ui.settings.lastServer = fields[iAddress].value;
        }
        ui.settings.playerName = c.name;
        SaveSettings(ui.settings);
        ui.choice = c;
    }

    bool host, solo;
    int  iName = -1, iShip = -1, iSeed = -1, iPort = -1, iPassword = -1, iMax = -1, iAddress = -1;   // which field is which
    std::vector<TextField> fields;
    bool stopOnLeave = true;
    std::string lan, error;
    int focus = 0;
};

// ===========================================================================
// A list of buttons: the main menu and its Single Player / Multiplayer pages
// ===========================================================================
class ListMenu : public Window {
public:
    using Action = std::function<void(GameState &, Ui &)>;      // null: "Back"
    struct Item { std::string label; Action action; };

    // root: the main menu itself (title above it, Esc does nothing); page: one of its pages
    // (drawn in the same place and at least as big, so it covers the main menu)
    ListMenu(WindowKind kind, const char *title, std::vector<Item> items, bool root = false, bool page = false,
             std::string note = "")
        : kind(kind), title(title), items(std::move(items)), root(root), page(page), note(std::move(note)) {}
    WindowKind Kind() const override { return kind; }
    bool Modal() const override { return true; }
    bool PausesGame() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        Layout L = Lay();
        int count = (int)items.size();
        if (in.Pressed(KEY_ESCAPE) && !root) { closed = true; return; }
        if (Up(in))   focus = (focus + count - 1) % count;
        if (Down(in)) focus = (focus + 1) % count;
        WheelFocus(in, focus, count);
        int chosen = Activate(in) ? focus : -1;
        for (int i = 0; i < count; i++) {
            if (MouseMoved(in) && Over(in, L.button[i])) focus = i;
            if (Clicked(in, L.button[i])) chosen = i;
        }
        if (chosen < 0) return;
        if (items[chosen].action) items[chosen].action(g, ui);
        else                      closed = true;
    }

    void Draw(const GameState &g) const override
    {
        (void)g;
        Layout L = Lay();
        if (root || page) {
            // The title, over the drifting view of the home system
            const char *name = "FREEHAULER";
            int size = 64, w = MeasureText(name, size);
            int ty = (int)L.panel.y - 130;
            DrawText(name, (ScreenW() - w) / 2 + 3, ty + 3, size, HUD_DIM);
            DrawText(name, (ScreenW() - w) / 2, ty, size, HUD);
            DrawLine((ScreenW() - w) / 2, ty + 76, (ScreenW() + w) / 2, ty + 76, HUD_DIM);
        } else {
            DimScreen();
        }
        if (page) DrawRectangleRec({ L.panel.x - 6, L.panel.y - 6, L.panel.width + 12, L.panel.height + 12 }, BLACK);
        DrawPanel(L.panel, title);
        for (int i = 0; i < (int)items.size(); i++) DrawButton(L.button[i], items[i].label.c_str(), focus == i);
        if (!note.empty())
            DrawText(note.c_str(), (int)(L.panel.x + (L.panel.width - MeasureText(note.c_str(), 16)) / 2),
                     (int)(L.panel.y + L.panel.height) - 30, 16, HUD_DIM);
    }

private:
    static const int MAX_ITEMS = 6;
    struct Layout { Rectangle panel, button[MAX_ITEMS]; };
    Layout Lay() const
    {
        Layout L;
        int count = (int)items.size();
        float w = note.empty() ? 380.0f : std::max(380.0f, MeasureText(note.c_str(), 16) + 60.0f);
        int slots = root || page ? std::max(count, 4) : count;
        L.panel = Centered(w, 80 + 58.0f * slots + 50);
        if (root || page) L.panel.y += 70;                          // room for the title
        for (int i = 0; i < count && i < MAX_ITEMS; i++)
            L.button[i] = { L.panel.x + (w - 280) / 2, L.panel.y + 78 + 58.0f * i, 280, 46 };
        return L;
    }
    WindowKind kind;
    const char *title;
    std::vector<Item> items;
    bool root, page;
    std::string note;
    int focus = 0;
};

std::unique_ptr<Window> MakeSinglePlayerMenu()
{
    return std::make_unique<ListMenu>(WindowKind::SinglePlayerMenu, "SINGLE PLAYER", std::vector<ListMenu::Item>{
        { "New Game",  [](GameState &, Ui &ui) { ui.Open(std::make_unique<ServerForm>(false, ui.settings, true)); } },
        { "Load Game", [](GameState &, Ui &ui) { ui.Open(std::make_unique<LoadMenu>()); } },
        { "Back", nullptr } }, false, true);
}

std::unique_ptr<Window> MakeMultiplayerMenu()
{
    return std::make_unique<ListMenu>(WindowKind::MultiplayerMenu, "MULTIPLAYER", std::vector<ListMenu::Item>{
        { "Host a Server", [](GameState &, Ui &ui) { ui.Open(std::make_unique<ServerForm>(true, ui.settings)); } },
        { "Join a Server", [](GameState &, Ui &ui) { ui.Open(std::make_unique<ServerForm>(false, ui.settings)); } },
        { "Back", nullptr } }, false, true, "Your progress on a server is kept under your captain's name");
}

} // namespace

std::unique_ptr<Window> MakePauseMenu(bool online) { return std::make_unique<PauseMenu>(online); }

std::unique_ptr<Window> MakeMainMenu()
{
    return std::make_unique<ListMenu>(WindowKind::MainMenu, "MAIN MENU", std::vector<ListMenu::Item>{
        { "Single Player", [](GameState &, Ui &ui) { ui.Open(MakeSinglePlayerMenu()); } },
        { "Multiplayer",   [](GameState &, Ui &ui) { ui.Open(MakeMultiplayerMenu()); } },
        { "Settings",      [](GameState &, Ui &ui) { ui.Open(std::make_unique<SettingsMenu>(ui.settings, false)); } },
        { "Exit",          [](GameState &, Ui &ui) { ui.quit = true; } } }, true);
}
