// map.cpp - the system map and the sector map (M)
//
// Both free the mouse and take the flight keys while they're open; the ship
// flies on. Picking something sets a course to it (Ui::SetCourse): it becomes
// the selection, the nose swings onto it, and holding Shift locks on and goes.
#include "ui.h"
#include "format.h"
#include "targetinfo.h"
#include "theme.h"
#include <algorithm>

static const Color UNKNOWN = { 130, 130, 130, 255 };

static bool Inside(Vector2 p, Rectangle r) { return CheckCollisionPointRec(p, r); }

static void DrawDotted(Vector2 a, Vector2 b, Color c)
{
    float len = Vector2Distance(a, b);
    if (len < 1.0f) return;
    Vector2 d = Vector2Scale(Vector2Subtract(b, a), 1.0f / len);
    for (float t = 0.0f; t < len; t += 6.0f)
        DrawLineV(Vector2Add(a, Vector2Scale(d, t)), Vector2Add(a, Vector2Scale(d, fminf(t + 2.5f, len))), c);
}

// The body a row's target sits at on the map: a moon or station is drawn with its world
static int MapAnchor(const LoadedSystem &L, int body)
{
    while (body >= 0 && L.bodies[body].parent > 0 && L.bodies[body].kind != Kind::Planet) body = L.bodies[body].parent;
    return body;
}

// ===========================================================================
// What the system map lists
// ===========================================================================
// Stars, planets and belts can be seen from anywhere in the system, so they're
// always there. What a world is like, and its moons and stations, show only
// once it's on the charts (see Knowledge in game.h).
std::vector<MapRow> SystemMapRows(const GameState &g)
{
    std::vector<MapRow> rows;
    const LoadedSystem &L = g.loaded;
    if (L.index < 0) return rows;

    auto bodyRow = [&](int bi, int indent) {
        const Body &b = L.bodies[bi];
        MapRow r;
        r.target = TargetId::MakeBody(L.index, bi);
        r.indent = indent;
        r.known  = Known(g, bi);
        r.name   = b.name;
        r.inhabited = r.known && b.population > 0;
        if (b.kind == Kind::Star)          r.what = b.type;
        else if (!r.known)                 r.what = std::string(BasicKind(b)) + " - unscanned";
        else if (b.kind == Kind::Station)  r.what = std::string(SettlementName(b.settlement)) + "  -  " + g.systems.factions.Name(b.owner);
        else                               r.what = b.type + (b.population > 0 ? "  -  " + g.systems.factions.Name(b.owner) : "");
        rows.push_back(r);
    };
    auto stationsOf = [&](int bi, int indent) {
        for (int i = 0; i < (int)L.bodies.size(); i++)
            if (L.bodies[i].kind == Kind::Station && L.bodies[i].parent == bi && Known(g, i)) bodyRow(i, indent);
    };

    // Everything that circles the primary, from the inside out
    struct Top { double r; int body, zone; };
    std::vector<Top> tops;
    for (int i = 0; i < (int)L.bodies.size(); i++) {
        const Body &b = L.bodies[i];
        if (b.kind == Kind::Star || b.kind == Kind::Planet) tops.push_back({ b.kind == Kind::Star ? (i == 0 ? -1.0 : b.orbit) : b.orbit, i, -1 });
        else if (b.kind == Kind::Station && b.parent == 0 && Known(g, i)) tops.push_back({ b.orbit, i, -1 });
    }
    for (int z = 0; z < (int)L.zones.size(); z++)
        if (!L.zones[z].ring) tops.push_back({ 0.5 * (L.zones[z].rIn + L.zones[z].rOut), -1, z });
    std::stable_sort(tops.begin(), tops.end(), [](const Top &a, const Top &b) { return a.r < b.r; });

    for (const Top &t : tops) {
        if (t.zone >= 0) {
            const Zone &z = L.zones[t.zone];
            MapRow r;
            r.target = TargetId::MakeZone(L.index, t.zone);
            r.known = true; r.name = z.name;
            r.what = std::string("Asteroid belt - ") + CompositionName(z.composition);
            rows.push_back(r);
            continue;
        }
        bodyRow(t.body, 0);
        if (L.bodies[t.body].kind != Kind::Planet || !Known(g, t.body)) continue;
        for (int i = 0; i < (int)L.bodies.size(); i++)
            if (L.bodies[i].kind == Kind::Moon && L.bodies[i].parent == t.body) { bodyRow(i, 1); stationsOf(i, 2); }
        stationsOf(t.body, 1);
        for (int z = 0; z < (int)L.zones.size(); z++)
            if (L.zones[z].ring && L.zones[z].parent == t.body) {
                MapRow r;
                r.target = TargetId::MakeZone(L.index, z);
                r.indent = 1; r.known = true; r.name = L.zones[z].name; r.what = "Ring system";
                rows.push_back(r);
            }
    }
    return rows;
}

// ===========================================================================
// System map: the system from above, and a list of what's in it
// ===========================================================================
class SystemMapWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::SystemMap; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }
    bool StillValid(const GameState &g) const override { return g.loaded.index >= 0; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        courseShown = ui.course;
        if (in.Pressed(KEY_ESCAPE)) { closed = true; return; }
        if (in.Pressed(KEY_M)) { closed = true; ui.Open(MakeSectorMap()); return; }
        if (in.Pressed(KEY_BACKSPACE)) { ui.SetCourse({}); ui.Say("Course cleared"); }

        std::vector<MapRow> rows = SystemMapRows(g);
        int n = (int)rows.size();
        if (n == 0) return;
        Layout L = Lay();
        if (in.Pressed(KEY_UP)   || in.Pressed(KEY_W)) row = (row + n - 1) % n;
        if (in.Pressed(KEY_DOWN) || in.Pressed(KEY_S)) row = (row + 1) % n;
        row = std::max(0, std::min(n - 1, row + in.WheelStep()));
        bool moved = in.mouseDelta.x != 0.0f || in.mouseDelta.y != 0.0f;
        bool click = in.MousePressed(MOUSE_BUTTON_LEFT);
        int  chosen = in.Pressed(KEY_ENTER) || in.Pressed(KEY_KP_ENTER) ? row : -1;

        // The list
        if (Inside(in.mousePos, L.list)) {
            int over = top + (int)((in.mousePos.y - L.list.y) / ROW_H);
            if (over >= 0 && over < n) {
                if (moved) row = over;
                if (click) chosen = row = over;
            }
        }
        // The map: the nearest world or belt to the pointer
        if (Inside(in.mousePos, L.map) && (moved || click)) {
            Plot plot = MakePlot(g, L);
            int best = -1; float bestD = 16.0f;
            for (int i = 0; i < n; i++) {
                if (rows[i].indent > 0) continue;
                float d = Vector2Distance(in.mousePos, RowPoint(g, plot, rows[i], in.mousePos));
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0) { row = best; if (click) chosen = best; }
        }
        top = std::max(std::min(top, row), row - L.visible + 1);
        top = std::max(0, std::min(top, n - L.visible));

        if (chosen >= 0) {
            ui.SetCourse(rows[chosen].target);
            ui.Say("Course set for " + rows[chosen].name + "  -  hold SHIFT to go");
            closed = true;
        }
    }

    void Draw(const GameState &g) const override
    {
        Layout L = Lay();
        const LoadedSystem &S = g.loaded;
        if (S.index < 0) return;
        const StarSystem &sys = g.systems[S.index];
        std::vector<MapRow> rows = SystemMapRows(g);
        int n = (int)rows.size(), sel = std::min(row, n - 1);
        bool charted = g.know.charted.count(S.index) > 0;

        DrawRectangleRec(L.panel, Fade(BLACK, 0.92f));
        DrawRectangleLinesEx(L.panel, 1.0f, HUD);
        DrawText(TextFormat("SYSTEM MAP  -  %s", sys.name.c_str()), (int)L.panel.x + 20, (int)L.panel.y + 14, 24, WHITE);
        std::string sub = StarSummary(sys) + "  -  " + g.systems.factions.Name(sys.ruler, sys.develop > 0.0f ? "unclaimed" : "uncharted") +
                          (charted ? "  -  charts on file"
                                                      : "  -  no charts: hail someone who lives here, or fly close to a world to scan it");
        DrawText(sub.c_str(), (int)L.panel.x + 20, (int)L.panel.y + 44, 16, charted ? HUD_LABEL : YELLOW);

        // --- The system from above (distances from the star are on a log scale) ---
        Plot plot = MakePlot(g, L);
        Vector2 c = plot.center;
        DrawCircleLinesV(c, plot.radius, Fade(ORANGE, 0.5f));                       // the edge of the gravity field
        DrawText("gravity field", (int)(c.x - MeasureText("gravity field", 12) / 2), (int)(c.y - plot.radius - 14), 12, Fade(ORANGE, 0.7f));
        for (double hz : { sys.hzIn, sys.hzOut }) DrawCircleLinesV(c, plot.Scale(hz), { 30, 110, 55, 255 });
        for (const Zone &z : S.zones)
            if (!z.ring) DrawRing(c, plot.Scale(z.rIn), plot.Scale(z.rOut), 0.0f, 360.0f, 72, Fade(z.color, 0.35f));
        for (int i = 0; i < (int)S.bodies.size(); i++) {
            const Body &b = S.bodies[i];
            if (b.kind == Kind::Planet) DrawCircleLinesV(c, plot.Scale(b.orbit), Dim(Known(g, i) ? b.lines : UNKNOWN, b.dwarf ? 0.2f : 0.35f));
        }
        for (int i = 0; i < (int)S.bodies.size(); i++) {
            const Body &b = S.bodies[i];
            Vector2 p = plot.At(b.local);
            bool known = Known(g, i);
            if (b.kind == Kind::Star) {
                DrawCircleV(p, i == 0 ? 7.0f : 5.0f, b.fill);
            } else if (b.kind == Kind::Planet) {
                DrawCircleV(p, b.dwarf ? 2.5f : b.giant ? 5.5f : 4.0f, known ? b.lines : UNKNOWN);
                if (known && b.population > 0) DrawCircleLinesV(p, b.giant ? 9.0f : 7.5f, HUD);         // people live here
                std::string tag = b.name.size() > sys.name.size() + 1 ? b.name.substr(sys.name.size() + 1) : b.name;
                if (!b.dwarf) DrawText(tag.c_str(), (int)p.x + 8, (int)p.y - 14, 14, known ? LIGHTGRAY : UNKNOWN);
            } else if (b.kind == Kind::Station && b.parent == 0 && known) {
                DrawRectangleLines((int)p.x - 3, (int)p.y - 3, 7, 7, b.lines);
            }
        }

        // The ship, its heading, and the course
        DVec3   here = ShipLocal(g);
        Vector2 ship = plot.At(here);
        Vector2 nose = Vector2Subtract(plot.At(here + ToD(ShipForward(g.player.ship)) * (0.05 * fmax(Len(here), 0.01 * AU_U))), ship);
        nose = Vector2Length(nose) > 0.01f ? Vector2Normalize(nose) : Vector2{ 0, -1 };
        Vector2 wing = { -nose.y, nose.x };
        DVec3 rel;
        TargetId course = CanonicalTarget(g, courseShown);
        if (course.valid() && course.system == S.index && FindTarget(g, course, rel))
            DrawDotted(ship, plot.At(here + rel), Fade(WHITE, 0.7f));
        DrawTriangleLines(Vector2Add(ship, Vector2Scale(nose, 10.0f)),
                          Vector2Add(Vector2Subtract(ship, Vector2Scale(nose, 6.0f)), Vector2Scale(wing, 6.0f)),
                          Vector2Subtract(Vector2Subtract(ship, Vector2Scale(nose, 6.0f)), Vector2Scale(wing, 6.0f)), SHIP_LINES);
        if (sel >= 0) {
            Vector2 p = RowPoint(g, plot, rows[sel], ship);
            DrawCircleLinesV(p, 12.0f, WHITE);
        }
        DrawText("seen from above the ecliptic  -  distances on a log scale", (int)L.map.x, (int)(L.map.y + L.map.height + 6), 14, HUD_DIM);

        // --- The list ---
        int x = (int)L.list.x, right = (int)(L.list.x + L.list.width);
        DrawLine(x - 14, (int)L.list.y - 6, x - 14, (int)(L.list.y + L.list.height), HUD_DIM);
        for (int i = top; i < n && i < top + L.visible; i++) {
            const MapRow &r = rows[i];
            int y = (int)L.list.y + (i - top) * ROW_H;
            bool onCourse = course.valid() && CanonicalTarget(g, r.target) == course;
            if (i == sel) DrawRectangle(x - 8, y - 2, (int)L.list.width + 8, ROW_H, Fade(HUD, 0.2f));
            int ix = x + r.indent * 18;
            if (r.indent > 0) DrawLine(ix - 12, y + 9, ix - 4, y + 9, HUD_DIM);
            DrawText(r.name.c_str(), ix, y, 18, onCourse ? YELLOW : r.known ? WHITE : UNKNOWN);
            std::string dist = FindTarget(g, r.target, rel) ? FormatPlanetDist(Len(rel)) : "";
            int dw = MeasureText(dist.c_str(), 14);
            DrawText(dist.c_str(), right - dw, y + 3, 14, LIGHTGRAY);
            int wx = std::max(ix + MeasureText(r.name.c_str(), 18) + 14, x + 250);
            std::string what = r.what;
            while (what.size() > 4 && wx + MeasureText(what.c_str(), 14) > right - dw - 10) what = what.substr(0, what.size() - 4) + "...";
            DrawText(what.c_str(), wx, y + 3, 14, r.inhabited ? HUD : r.known ? HUD_LABEL : UNKNOWN);
        }
        if (top > 0)             DrawText("...", right - 20, (int)L.list.y - 18, 16, HUD_DIM);
        if (top + L.visible < n) DrawText("...", right - 20, (int)(L.list.y + L.list.height) - 2, 16, HUD_DIM);

        const char *help = "UP/DOWN, wheel or mouse: pick   ENTER / click: set course   BKSP: clear course   M: sector map   ESC: close";
        DrawText(help, (int)L.panel.x + 20, (int)(L.panel.y + L.panel.height) - 26, 16, HUD_DIM);
    }

    TargetId courseShown;        // the course laid in (the Ui's, as of the last input)

private:
    static const int ROW_H = 24;
    struct Layout { Rectangle panel, map, list; int visible; };
    static Layout Lay()
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Layout L;
        L.panel = { 30, 30, sw - 60, sh - 60 };
        float listW = fminf(520.0f, L.panel.width * 0.45f);
        float size  = fminf(L.panel.width - listW - 70.0f, L.panel.height - 130.0f);
        L.map  = { L.panel.x + 24, L.panel.y + 76, size, size };
        L.list = { L.panel.x + L.panel.width - listW - 16, L.panel.y + 80, listW, L.panel.height - 130.0f };
        L.visible = std::max(1, (int)(L.list.height / ROW_H));
        L.list.height = (float)(L.visible * ROW_H);
        return L;
    }

    // Star-relative positions onto the map: flattened onto the ecliptic, and squeezed
    // (log scale) so the inner worlds and the outer ones both fit
    struct Plot {
        Vector2 center; float radius; double rMin, rMax; DVec3 u, v;
        float Scale(double r) const { return (float)(radius * log(1.0 + fmax(r, 0.0) / rMin) / log(1.0 + rMax / rMin)); }
        Vector2 At(DVec3 local) const
        {
            double x = Dot(local, u), y = Dot(local, v), r = sqrt(x * x + y * y);
            if (r < 1e-9) return center;
            float k = fminf(Scale(r), radius * 1.04f) / (float)r;
            return { center.x + (float)x * k, center.y - (float)y * k };
        }
    };
    static Plot MakePlot(const GameState &g, const Layout &L)
    {
        const StarSystem &sys = g.systems[g.loaded.index];
        Plot p;
        p.center = { L.map.x + L.map.width * 0.5f, L.map.y + L.map.height * 0.5f };
        p.radius = L.map.width * 0.5f - 6.0f;
        p.rMax = sys.fieldR;
        p.rMin = sys.fieldR;
        for (const Body &b : g.loaded.bodies) if (b.kind == Kind::Planet) p.rMin = fmin(p.rMin, b.orbit);
        p.rMin = fmax(0.4 * fmin(p.rMin, 0.5 * sys.fieldR), 2.0 * sys.starRadius);
        p.u = sys.u; p.v = sys.v;
        return p;
    }
    // Where a row is on the map. A belt is a ring: its nearest point to 'toward' (a screen point).
    static Vector2 RowPoint(const GameState &g, const Plot &plot, const MapRow &r, Vector2 toward)
    {
        const LoadedSystem &L = g.loaded;
        if (r.target.type == TargetId::Body) return plot.At(L.bodies[MapAnchor(L, r.target.index)].local);
        const Zone &z = L.zones[r.target.index];
        if (z.ring) return plot.At(L.bodies[z.parent].local);
        Vector2 d = Vector2Subtract(toward, plot.center);
        d = Vector2Length(d) > 0.01f ? Vector2Normalize(d) : Vector2{ 1, 0 };
        return Vector2Add(plot.center, Vector2Scale(d, plot.Scale(0.5 * (z.rIn + z.rOut))));
    }

    int row = 0, top = 0;
};

// ===========================================================================
// Sector map: the stars around the nearest one, standing on the galactic plane
// ===========================================================================
class SectorMapWindow : public Window {
public:
    WindowKind Kind() const override { return WindowKind::SectorMap; }
    bool Modal() const override { return true; }
    bool WantsCursor() const override { return true; }

    void HandleInput(InputFrame &in, GameState &g, Ui &ui) override
    {
        courseShown = ui.course;
        if (in.Pressed(KEY_ESCAPE) || in.Pressed(KEY_M)) { closed = true; return; }
        if (in.Pressed(KEY_BACKSPACE)) { ui.SetCourse({}); ui.Say("Course cleared"); }
        if (in.wheel > 0.0f) range = std::max(0, range - 1);
        if (in.wheel < 0.0f) range = std::min(RANGES - 1, range + 1);
        float turn = (float)(in.Down(KEY_RIGHT) || in.Down(KEY_D)) - (float)(in.Down(KEY_LEFT) || in.Down(KEY_A));
        if (in.MouseDown(MOUSE_BUTTON_RIGHT)) azimuth += in.mouseDelta.x * 0.008f;
        azimuth += turn * 1.2f * in.dt;

        // The star under the pointer
        View v = MakeView(g);
        hover = NO_SYSTEM;
        float best = 14.0f;
        for (const StarSystem *s : Stars(g)) {
            float d = Vector2Distance(in.mousePos, v.Star(RelLy(g, *s)));
            if (d < best) { best = d; hover = s->id; }
        }
        if (hover != NO_SYSTEM && in.MousePressed(MOUSE_BUTTON_LEFT)) {
            ui.SetCourse(TargetId::MakeStar(hover));
            ui.Say("Course set for " + g.systems[hover].name + "  -  hold SHIFT to go");
            closed = true;
        }
    }

    void Draw(const GameState &g) const override
    {
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        Rectangle panel = { 30, 30, sw - 60, sh - 60 };
        const SysId center = g.player.ship.anchor;
        const float R = RANGE_LY[range];
        View v = MakeView(g);
        TargetId course = courseShown;
        if (course.type == TargetId::Body && course.index == 0) course = TargetId::MakeStar(course.system);   // (a loaded system's star)

        DrawRectangleRec(panel, Fade(BLACK, 0.92f));
        DrawRectangleLinesEx(panel, 1.0f, HUD);
        DrawText(TextFormat("SECTOR MAP  -  around %s", g.systems[center].name.c_str()), (int)panel.x + 20, (int)panel.y + 14, 24, WHITE);
        DrawText(TextFormat("stars within %.0f light years, standing on the galactic plane", R), (int)panel.x + 20, (int)panel.y + 44, 16, HUD_LABEL);

        // The plane: rings every third of the range, and spokes
        const Color GRID = { 0, 120, 160, 255 };
        for (int k = 1; k <= 3; k++) {
            float r = R * k / 3.0f;
            DrawEllipseLines((int)v.center.x, (int)v.center.y, r * v.scale, r * v.scale * v.squash, k == 3 ? GRID : Dim(GRID, 0.7f));
            const char *lbl = TextFormat("%.0f ly", r);
            DrawText(lbl, (int)(v.center.x + r * v.scale + 6), (int)v.center.y - 7, 14, Dim(GRID, 0.9f));
        }
        for (int k = 0; k < 6; k++) {
            float a = PI * k / 3.0f;
            DrawLineV(v.center, v.Floor({ R * cosf(a), 0, R * sinf(a) }), Dim(GRID, 0.6f));
        }

        // The stars, far side first, each on a stem to the plane (dotted below it)
        std::vector<const StarSystem *> stars = Stars(g);
        std::sort(stars.begin(), stars.end(), [&](const StarSystem *a, const StarSystem *b) { return v.Depth(RelLy(g, *a)) > v.Depth(RelLy(g, *b)); });
        bool crowded = stars.size() > 60;
        for (const StarSystem *s : stars) {
            Vector3 rel = RelLy(g, *s);
            Vector2 p = v.Star(rel), foot = v.Floor(rel);
            const StarInfo &st = s->stars[0];
            double lum = 0.0;
            for (const StarInfo &o : s->stars) lum += o.lum;
            float size = Clamp(3.2f + 0.9f * (float)log10(fmax(lum, 1e-4)), 2.0f, 7.0f);
            bool  hot = s->id == hover, onCourse = course.type == TargetId::Star && course.system == s->id;
            Color stem = { 255, 140, 40, 255 };
            if (rel.y >= 0.0f) DrawLineV(foot, p, Fade(stem, 0.8f));
            else               DrawDotted(foot, p, Fade(stem, 0.9f));
            if (const Faction *ruler = g.systems.factions.Get(s->ruler))                   // whose space it stands in
                DrawRectangle((int)foot.x - 3, (int)foot.y - 3, 7, 7, ruler->color);
            DrawCircleV(p, size, st.color);
            if (s->id == g.systems.home) DrawCircleLinesV(p, size + 4.0f, HUD);
            if (onCourse)                DrawCircleLinesV(p, size + 7.0f, YELLOW);
            if (hot)                     DrawCircleLinesV(p, size + 5.0f, WHITE);
            bool label = hot || onCourse || s->id == center || !crowded || lum > 0.4;
            if (label) DrawText(s->name.c_str(), (int)(p.x + size + 5), (int)p.y - 8, 16,
                                hot ? WHITE : onCourse ? YELLOW : s->develop > 0.0f ? LIGHTGRAY : UNKNOWN);
        }

        // The ship
        Vector3 shipLy = ToF(g.player.ship.pos * (1.0 / LY_U));
        Vector2 sp = v.Star(shipLy), sf = v.Floor(shipLy);
        if (Vector3Length(shipLy) > 0.02f * R) {
            if (shipLy.y >= 0.0f) DrawLineV(sf, sp, Fade(SHIP_LINES, 0.7f)); else DrawDotted(sf, sp, Fade(SHIP_LINES, 0.8f));
        }
        DrawTriangleLines({ sp.x, sp.y - 8 }, { sp.x - 6, sp.y + 5 }, { sp.x + 6, sp.y + 5 }, SHIP_LINES);
        DrawText("you", (int)sp.x - 34, (int)sp.y - 6, 14, SHIP_LINES);

        // What's under the pointer
        if (hover != NO_SYSTEM && g.systems.Exists(hover)) {
            const StarSystem &s = g.systems[hover];
            DVec3 rel;
            std::string lines[4] = {
                s.name, StarSummary(s),
                FindTarget(g, TargetId::MakeStar(hover), rel) ? FormatStarDist(Len(rel)) + " from the ship" : std::string(),
                s.develop > 0.0f ? g.systems.factions.Name(s.ruler, "Unclaimed space") : std::string("No human presence") };
            if (g.know.charted.count(hover)) lines[3] += "  -  charts on file";
            int w = 0;
            for (const std::string &l : lines) w = std::max(w, MeasureText(l.c_str(), 16));
            int bx = (int)(panel.x + panel.width) - w - 44, by = (int)panel.y + 16;
            DrawRectangle(bx - 10, by - 6, w + 20, 92, Fade(BLACK, 0.85f));
            DrawRectangleLines(bx - 10, by - 6, w + 20, 92, HUD_DIM);
            DrawText(lines[0].c_str(), bx, by, 20, WHITE);
            for (int i = 1; i < 4; i++) DrawText(lines[i].c_str(), bx, by + 6 + 20 * i, 16, i == 3 ? HUD_LABEL : LIGHTGRAY);
        }

        // Whose colours those are: the states with space on the map
        {
            std::vector<int> seen;
            for (const StarSystem *s : stars)
                if (s->ruler != NO_FACTION && std::find(seen.begin(), seen.end(), s->ruler) == seen.end()) seen.push_back(s->ruler);
            std::sort(seen.begin(), seen.end());
            int ly = (int)(panel.y + panel.height) - 40 - 20 * (int)seen.size();
            for (int id : seen) {
                const Faction *f = g.systems.factions.Get(id);
                if (!f) continue;
                DrawRectangle((int)panel.x + 20, ly + 4, 8, 8, f->color);
                DrawText(f->name.c_str(), (int)panel.x + 34, ly, 14, LIGHTGRAY);
                ly += 20;
            }
        }
        const char *help = "click a star: set course   wheel: range   A/D, arrows or RMB drag: turn   BKSP: clear course   M / ESC: close";
        DrawText(help, (int)panel.x + 20, (int)(panel.y + panel.height) - 26, 16, HUD_DIM);
    }

    TargetId courseShown;

private:
    static const int RANGES = 5;
    static constexpr float RANGE_LY[RANGES] = { 6.0f, 9.0f, 12.0f, 18.0f, 27.0f };

    // The view: looking down at the plane from a third of the way up, turned by 'azimuth'
    struct View {
        Vector2 center; float scale, squash, lift, ca, sa;
        Vector2 Floor(Vector3 ly) const
        {
            float x = ly.x * ca + ly.z * sa, z = -ly.x * sa + ly.z * ca;
            return { center.x + x * scale, center.y + z * scale * squash };
        }
        Vector2 Star(Vector3 ly) const { Vector2 f = Floor(ly); return { f.x, f.y - ly.y * scale * lift }; }
        float   Depth(Vector3 ly) const { return -(-ly.x * sa + ly.z * ca); }        // bigger = farther back
    };
    View MakeView(const GameState &g) const
    {
        (void)g;
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        const float tilt = 20.0f * DEG2RAD;                                  // how far above the plane we're looking from
        View v;
        v.center = { sw * 0.5f, sh * 0.5f + 10.0f };
        v.squash = sinf(tilt);
        v.lift   = cosf(tilt);
        // Fit the widest ring across, and the tallest stem up and down
        v.scale  = fminf((sw - 260.0f) * 0.5f, (sh - 200.0f) * 0.5f / (v.squash + v.lift * 0.75f)) / RANGE_LY[range];
        v.ca = cosf(azimuth); v.sa = sinf(azimuth);
        return v;
    }
    // A system's position from the map's center, in light years (galactic axes: y is up out of the disk)
    static Vector3 RelLy(const GameState &g, const StarSystem &s)
    {
        return ToF(g.systems.Between(g.player.ship.anchor, s.id) * (1.0 / LY_U));
    }
    std::vector<const StarSystem *> Stars(const GameState &g) const
    {
        std::vector<const StarSystem *> out;
        g.systems.Nearby(g.player.ship.anchor, {}, RANGE_LY[range] * LY_U, out);
        return out;
    }

    int   range = 2;
    float azimuth = 0.5f;
    SysId hover = NO_SYSTEM;
};

constexpr float SectorMapWindow::RANGE_LY[SectorMapWindow::RANGES];

std::unique_ptr<Window> MakeSystemMap() { return std::make_unique<SystemMapWindow>(); }
std::unique_ptr<Window> MakeSectorMap() { return std::make_unique<SectorMapWindow>(); }
