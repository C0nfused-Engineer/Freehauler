// render.cpp - drawing the 3D scene
#include "render.h"
#include "targetinfo.h"
#include "theme.h"
#include "rlgl.h"
#include <algorithm>
#include <map>
#include <random>

// A small arrowhead: nose forward, flat diamond-shaped engine face at the back.
static Mesh GenShipMesh()
{
    const Vector3 N = {  0.0f,   0.0f, -2.0f };   // nose
    const Vector3 L = { -1.3f,   0.0f,  1.0f };   // left wingtip
    const Vector3 R = {  1.3f,   0.0f,  1.0f };   // right wingtip
    const Vector3 T = {  0.0f,  0.45f,  1.0f };   // dorsal ridge
    const Vector3 B = {  0.0f, -0.25f,  1.0f };   // keel
    const Vector3 tris[][3] = {
        { N, L, T }, { N, T, R }, { N, R, B }, { N, B, L },   // hull
        { L, B, R }, { L, R, T },                             // engine face (coplanar)
    };
    const int triCount = sizeof(tris) / sizeof(tris[0]);

    Vector3 centroid = Vector3Scale(Vector3Add(Vector3Add(N, L), Vector3Add(Vector3Add(R, T), B)), 0.2f);

    Mesh mesh = {};
    mesh.triangleCount = triCount;
    mesh.vertexCount   = triCount * 3;
    mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
    mesh.normals  = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));

    for (int t = 0; t < triCount; t++) {
        Vector3 a = tris[t][0], b = tris[t][1], c = tris[t][2];
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
        // Make sure every triangle faces outward, otherwise backface culling would hide it
        Vector3 faceCenter = Vector3Scale(Vector3Add(Vector3Add(a, b), c), 1.0f / 3.0f);
        if (Vector3DotProduct(n, Vector3Subtract(faceCenter, centroid)) < 0.0f) {
            std::swap(b, c);
            n = Vector3Negate(n);
        }
        const Vector3 v[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            int k = (t * 3 + i) * 3;
            mesh.vertices[k] = v[i].x; mesh.vertices[k + 1] = v[i].y; mesh.vertices[k + 2] = v[i].z;
            mesh.normals[k]  = n.x;    mesh.normals[k + 1]  = n.y;    mesh.normals[k + 2]  = n.z;
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

// A station: a spinning wheel on spokes around a hub, with a docking spindle.
// Unit radius, spin axis along +Y.
static Mesh GenStationMesh()
{
    std::vector<Vector3> tri;                                 // 3 vertices per triangle
    std::vector<Vector3> nrm;
    auto quad = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 out) {
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
        if (Vector3DotProduct(n, out) < 0.0f) { std::swap(b, d); n = Vector3Negate(n); }
        for (Vector3 v : { a, b, c, a, c, d }) { tri.push_back(v); nrm.push_back(n); }
    };
    // Box from its center and three half-axes
    auto box = [&](Vector3 c, Vector3 e1, Vector3 e2, Vector3 e3) {
        Vector3 axes[3] = { e1, e2, e3 };
        for (int k = 0; k < 3; k++)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                Vector3 n = Vector3Scale(axes[k], (float)sgn), u = axes[(k + 1) % 3], v = axes[(k + 2) % 3];
                Vector3 f = Vector3Add(c, n);
                quad(Vector3Subtract(Vector3Subtract(f, u), v), Vector3Subtract(Vector3Add(f, u), v),
                     Vector3Add(Vector3Add(f, u), v), Vector3Add(Vector3Subtract(f, u), v), n);
            }
    };
    // The wheel: a box-section ring
    const int SEG = 12;
    const float RO = 1.0f, RI = 0.8f, H = 0.12f;
    for (int i = 0; i < SEG; i++) {
        float a0 = 2.0f * PI * i / SEG, a1 = 2.0f * PI * (i + 1) / SEG, am = 0.5f * (a0 + a1);
        Vector3 d0 = { cosf(a0), 0, sinf(a0) }, d1 = { cosf(a1), 0, sinf(a1) }, dm = { cosf(am), 0, sinf(am) };
        auto at = [](Vector3 d, float r, float y) { return Vector3{ d.x * r, y, d.z * r }; };
        quad(at(d0, RO, -H), at(d1, RO, -H), at(d1, RO, H), at(d0, RO, H), dm);                  // outer
        quad(at(d0, RI, -H), at(d1, RI, -H), at(d1, RI, H), at(d0, RI, H), Vector3Negate(dm));   // inner
        quad(at(d0, RI, H), at(d1, RI, H), at(d1, RO, H), at(d0, RO, H), { 0, 1, 0 });           // top
        quad(at(d0, RI, -H), at(d1, RI, -H), at(d1, RO, -H), at(d0, RO, -H), { 0, -1, 0 });      // bottom
    }
    // Hub, spokes, spindle
    box({ 0, 0, 0 }, { 0.22f, 0, 0 }, { 0, 0.28f, 0 }, { 0, 0, 0.22f });
    for (int k = 0; k < 3; k++) {
        float a = 2.0f * PI * k / 3.0f + PI / SEG;            // between the wheel's corners
        Vector3 d = { cosf(a), 0, sinf(a) }, side = { -sinf(a), 0, cosf(a) };
        box(Vector3Scale(d, 0.51f), Vector3Scale(d, 0.29f), { 0, 0.04f, 0 }, Vector3Scale(side, 0.04f));
    }
    box({ 0, 0.45f, 0 }, { 0.07f, 0, 0 }, { 0, 0.17f, 0 }, { 0, 0, 0.07f });
    box({ 0, 0.66f, 0 }, { 0.14f, 0, 0 }, { 0, 0.04f, 0 }, { 0, 0, 0.14f });   // docking ring
    box({ 0, -0.36f, 0 }, { 0.12f, 0, 0 }, { 0, 0.08f, 0 }, { 0, 0, 0.12f });  // reactor

    Mesh mesh = {};
    mesh.vertexCount   = (int)tri.size();
    mesh.triangleCount = mesh.vertexCount / 3;
    mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
    mesh.normals  = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
    for (int i = 0; i < mesh.vertexCount; i++) {
        mesh.vertices[3 * i] = tri[i].x; mesh.vertices[3 * i + 1] = tri[i].y; mesh.vertices[3 * i + 2] = tri[i].z;
        mesh.normals[3 * i]  = nrm[i].x; mesh.normals[3 * i + 1]  = nrm[i].y; mesh.normals[3 * i + 2]  = nrm[i].z;
    }
    UploadMesh(&mesh, false);
    return mesh;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
// Bodies smaller than this (radius / distance) are drawn as a nav dot instead
// of a wireframe sphere; at that size the sphere would be a couple of pixels.
const double SPHERE_MIN_ANGLE = 0.003;

// Big enough on screen to draw as a sphere? (Otherwise it gets a nav dot.)
static bool SphereVisible(DVec3 rel, double radius) { return radius / Len(rel) > SPHERE_MIN_ANGLE; }

// Draws a body seen from the ship. 'rel' is the ship-to-center vector. The
// compression factor comes from the distance to the SURFACE, so a planet
// you're skimming is drawn at true scale where it's close to you.
static void DrawBodyRel(DVec3 rel, double radius, Color lines, Color fill, const VectorModel &sphere,
                        Vector3 axis = { 0, 1, 0 }, float angle = 0.0f)
{
    double a = fmax(Len(rel) - radius, 1.0);
    double s = MapDist(a) / a;
    float  r = (float)(radius * s);
    DrawVectorModelEx(sphere, ToF(rel * s), axis, angle, { r, r, r }, lines, fill);
}

// Each body is distance-compressed on its own, so the depth buffer can't be
// trusted to sort one body against another (very close to a big planet, a far
// body's compressed depth can land in front of the planet's edge). Bodies are
// therefore painted back to front with the depth buffer cleared in between:
// depth only resolves a sphere's own lines against its own fill.
extern "C" VECTOR_GLAPI void VECTOR_GLCALL glClear(unsigned int mask);
static void ClearDepth()
{
    rlDrawRenderBatchActive();       // draw everything queued so far first
    glClear(0x00000100);             // GL_DEPTH_BUFFER_BIT
}


// True if any other body sits between the ship and the target (exact, in double
// precision). Used for nav dots and targeting, which are drawn in 2D on top.
bool LineOfSightBlocked(DVec3 target, double targetRadius, const std::vector<Occluder> &occ, int self)
{
    double D = Len(target);
    DVec3  dir = target * (1.0 / D);
    for (int k = 0; k < (int)occ.size(); k++) {
        if (k == self) continue;
        double t = Dot(occ[k].rel, dir);                     // along the line of sight
        if (t <= 0.0) continue;                              // behind us
        double perp2 = Dot(occ[k].rel, occ[k].rel) - t * t;  // squared miss distance
        double R2 = occ[k].radius * occ[k].radius;
        if (perp2 >= R2) continue;
        if (t - sqrt(R2 - perp2) < D - targetRadius) return true;   // its surface is in front of the target
    }
    return false;
}

// An orbit ring around the star at 'starRel' (ship-to-star). Each vertex is
// mapped through the distance compression, and segments near the ship are
// subdivided so the ring still passes exactly through its planet up close.
static void DrawOrbitRel(DVec3 starRel, const Orbit &o)
{
    const int N = 360;
    const double chordErr = o.radius * (1.0 - cos(PI / N));     // max gap between chord and arc
    auto at = [&](double ang) { return starRel + o.u * (cos(ang) * o.radius) + o.v * (sin(ang) * o.radius); };

    for (int i = 0; i < N; i++) {
        double a0 = 2.0 * PI * i / N, a1 = 2.0 * PI * (i + 1) / N;
        double dm = Len(at(0.5 * (a0 + a1)));
        int k = (int)Clamp((float)ceil(sqrt(chordErr / (0.0005 * fmax(dm, 1.0)))), 1.0f, 2000.0f);
        Vector3 prev = MapPoint(at(a0));
        for (int j = 1; j <= k; j++) {
            Vector3 p = MapPoint(at(a0 + (a1 - a0) * j / k));
            DrawLine3D(prev, p, o.color);
            prev = p;
        }
    }
}

// A circle drawn with one uniform compression factor 's' (so it stays rigid
// with the planet it belongs to). Segments near the ship are subdivided so
// the circle stays accurate when you're flying inside it.
static void DrawCircleScaled(DVec3 centerRel, DVec3 u, DVec3 v, double radius, Color col, double s)
{
    const int N = 256;
    const double chordErr = radius * (1.0 - cos(PI / N));
    auto at = [&](double ang) { return centerRel + u * (cos(ang) * radius) + v * (sin(ang) * radius); };
    for (int i = 0; i < N; i++) {
        double a0 = 2.0 * PI * i / N, a1 = 2.0 * PI * (i + 1) / N;
        double dm = Len(at(0.5 * (a0 + a1)));
        int k = (int)Clamp((float)ceil(sqrt(chordErr / (0.0005 * fmax(dm, 1.0)))), 1.0f, 2000.0f);
        Vector3 prev = ToF(at(a0) * s);
        for (int j = 1; j <= k; j++) {
            Vector3 p = ToF(at(a0 + (a1 - a0) * j / k) * s);
            DrawLine3D(prev, p, col);
            prev = p;
        }
    }
}

// A planet's rings as concentric lines at the band edges, brighter where
// the bands are denser; empty gaps get no lines. Drawn with the planet's own
// compression factor so the rings and planet occlude each other correctly.
static void DrawRingRel(DVec3 planetRel, const Zone &z, double s)
{
    for (size_t e = 0; e < z.bandEdges.size(); e++) {
        float dens = 0.0f;
        if (e > 0)                       dens = fmaxf(dens, z.bandDensity[e - 1]);
        if (e < z.bandDensity.size())    dens = fmaxf(dens, z.bandDensity[e]);
        if (dens <= 0.0f) continue;
        DrawCircleScaled(planetRel, z.u, z.v, z.bandEdges[e], Dim(z.color, 0.3f + 0.5f * dens), s);
    }
}

// A belt seen from afar: its point cloud, each point a speck about a pixel wide
static void DrawBeltMacro(DVec3 starRel, const Zone &z, Vector3 camRight)
{
    for (const DVec3 &p : z.macro) {
        Vector3 rp = MapPoint(starRel + p);
        float   m  = Vector3Length(rp) * 0.0012f;
        DrawLine3D(Vector3Subtract(rp, Vector3Scale(camRight, m)), Vector3Add(rp, Vector3Scale(camRight, m)), z.color);
    }
}

// A lumpy rock: a once-subdivided icosahedron (80 faces) with each vertex
// pushed in or out, a few broad bulges and dents, and a squash. The faces
// end up non-flat, so every edge shows: the classic faceted vector rock.
static Mesh GenAsteroidMesh(unsigned seed)
{
    std::mt19937 rng(seed);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

    const float t = (1.0f + sqrtf(5.0f)) * 0.5f;
    std::vector<Vector3> v = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
                               { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    for (Vector3 &p : v) p = Vector3Normalize(p);
    std::vector<std::array<int, 3>> f = {
        { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
        { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
        { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };

    // Subdivide each triangle into four (shared midpoints, so the surface stays closed)
    std::map<std::pair<int, int>, int> mid;
    auto midpoint = [&](int a, int b) {
        auto key = std::minmax(a, b);
        auto it = mid.find(key);
        if (it != mid.end()) return it->second;
        v.push_back(Vector3Normalize(Vector3Scale(Vector3Add(v[a], v[b]), 0.5f)));
        return mid[key] = (int)v.size() - 1;
    };
    std::vector<std::array<int, 3>> f2;
    for (auto &tri : f) {
        int ab = midpoint(tri[0], tri[1]), bc = midpoint(tri[1], tri[2]), ca = midpoint(tri[2], tri[0]);
        f2.push_back({ tri[0], ab, ca }); f2.push_back({ tri[1], bc, ab });
        f2.push_back({ tri[2], ca, bc }); f2.push_back({ ab, bc, ca });
    }

    // Lumps
    Vector3 lobe[3]; float lobeAmp[3];
    for (int k = 0; k < 3; k++) {
        lobe[k] = Vector3Normalize({ uni(-1, 1), uni(-1, 1), uni(-1, 1) });
        lobeAmp[k] = uni(-0.3f, 0.3f);
    }
    Vector3 squash = { 1.0f, uni(0.6f, 0.95f), uni(0.65f, 1.0f) };
    float maxR = 0.0f;
    for (Vector3 &p : v) {
        float r = uni(0.85f, 1.1f);
        for (int k = 0; k < 3; k++) r += lobeAmp[k] * powf(fmaxf(0.0f, Vector3DotProduct(p, lobe[k])), 3.0f);
        p = Vector3Multiply(Vector3Scale(p, r), squash);
        maxR = fmaxf(maxR, Vector3Length(p));
    }
    for (Vector3 &p : v) p = Vector3Scale(p, 1.0f / maxR);       // unit radius

    Mesh mesh = {};
    mesh.triangleCount = (int)f2.size();
    mesh.vertexCount   = mesh.triangleCount * 3;
    mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
    mesh.normals  = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
    for (int i = 0; i < mesh.triangleCount; i++) {
        Vector3 a = v[f2[i][0]], b = v[f2[i][1]], c = v[f2[i][2]];
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a)));
        Vector3 center = Vector3Scale(Vector3Add(Vector3Add(a, b), c), 1.0f / 3.0f);
        if (Vector3DotProduct(n, center) < 0.0f) { std::swap(b, c); n = Vector3Negate(n); }   // face outward
        const Vector3 tri[3] = { a, b, c };
        for (int j = 0; j < 3; j++) {
            int k = (i * 3 + j) * 3;
            mesh.vertices[k] = tri[j].x; mesh.vertices[k + 1] = tri[j].y; mesh.vertices[k + 2] = tri[j].z;
            mesh.normals[k]  = n.x;      mesh.normals[k + 1]  = n.y;      mesh.normals[k + 2]  = n.z;
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

// ---------------------------------------------------------------------------
// Space dust: faint and sparse at cruise; at hyper/warp the specks all come in
// and stretch into long bright streaks.
// ---------------------------------------------------------------------------
void Dust::Init(unsigned seed, int count)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-box * 0.5f, box * 0.5f);
    specks.resize(count);
    for (Vector3 &p : specks) p = { u(rng), u(rng), u(rng) };
}

// moved: how far the ship moved this frame. hyper: 0 = cruise, 1 = full hyper.
void Dust::UpdateAndDraw(Vector3 moved, Vector3 vel, Vector3 camRight, float hyper)
{
    int   visible    = (int)(specks.size() * Lerp(0.25f, 1.0f, hyper));
    float brightness = Lerp(55.0f, 170.0f, hyper);
    Vector3 streak   = Vector3Scale(vel, Lerp(0.008f, 0.035f, hyper));
    float len = Vector3Length(streak);
    if (len > 45.0f) streak = Vector3Scale(streak, 45.0f / len);
    if (len < 0.2f)  streak = Vector3Scale(camRight, 0.2f);    // tiny dash when slow

    // At hyper speeds the ship moves far more than one box per frame;
    // only the fractional part matters for wrapping.
    Vector3 shift = { fmodf(moved.x, box), fmodf(moved.y, box), fmodf(moved.z, box) };

    for (int i = 0; i < (int)specks.size(); i++) {
        Vector3 &p = specks[i];
        p = Vector3Subtract(p, shift);
        p.x -= box * floorf(p.x / box + 0.5f);                  // wrap into the box
        p.y -= box * floorf(p.y / box + 0.5f);
        p.z -= box * floorf(p.z / box + 0.5f);
        if (i >= visible) continue;

        float fade = 1.0f - Vector3Length(p) / (box * 0.5f);    // fade toward box edge
        if (fade <= 0.0f) continue;
        unsigned char g = (unsigned char)(brightness * fade);
        DrawLine3D(p, Vector3Subtract(p, streak), { g, g, g, 255 });
    }
}

// ---------------------------------------------------------------------------
// Assets, camera
// ---------------------------------------------------------------------------
void RenderAssets::Load()
{
    const Matrix POLE_TO_Y = MatrixRotateX(-PI / 2.0f);     // raylib's sphere has its poles on Z
    ship   = LoadVectorModel(LoadModelFromMesh(GenShipMesh()));
    planet = LoadVectorModel(LoadModelFromMesh(GenMeshSphere(1.0f, 12, 24)));
    star   = LoadVectorModel(LoadModelFromMesh(GenMeshSphere(1.0f, 8, 16)));
    station = LoadVectorModel(LoadModelFromMesh(GenStationMesh()));
    planet.model.transform = POLE_TO_Y;
    star.model.transform   = POLE_TO_Y;
    for (int i = 0; i < AST_TEMPLATES; i++) rocks.push_back(LoadVectorModel(LoadModelFromMesh(GenAsteroidMesh(1000 + i))));
}

void RenderAssets::Unload()
{
    UnloadVectorModel(ship);
    UnloadVectorModel(planet);
    UnloadVectorModel(star);
    UnloadVectorModel(station);
    for (VectorModel &m : rocks) UnloadVectorModel(m);
    rocks.clear();
}

float HyperFraction(float speed)
{
    return (float)Clamp((float)(log(fmax(speed, 1e-3) / MAX_SPEED) / log(HYPER_SPEED / MAX_SPEED)), 0.0f, 1.0f);
}

static void PlaceCamera(ViewState &v)
{
    float pull = 1.0f + 0.4f * v.hyperVis;
    v.camera.position   = Vector3RotateByQuaternion({ 0.0f, 1.8f * pull, 7.0f * pull }, v.camRot);
    v.camera.target     = Vector3RotateByQuaternion({ 0.0f, 0.9f, -6.0f }, v.camRot);
    v.camera.up         = Vector3RotateByQuaternion({ 0.0f, 1.0f, 0.0f }, v.camRot);
    v.camera.fovy       = 60.0f + 25.0f * v.hyperVis;
    v.camera.projection = CAMERA_PERSPECTIVE;
    v.camRight = Vector3RotateByQuaternion({ 1, 0, 0 }, v.camRot);
    v.camFwd   = Vector3Normalize(Vector3Subtract(v.camera.target, v.camera.position));
}

void ResetView(ViewState &v, const Ship &eye)
{
    v.camRot   = eye.rot;
    v.hyperVis = HyperFraction(Vector3Length(eye.vel));
    if (v.dust.specks.empty()) v.dust.Init(42, 600);
    PlaceCamera(v);
}

// Chase camera: everything is drawn relative to the ship, which sits at the origin
void UpdateView(ViewState &v, const Ship &eye, float frameDt)
{
    v.hyperVis = Lerp(v.hyperVis, HyperFraction(Vector3Length(eye.vel)), 1.0f - expf(-4.0f * frameDt));
    v.camRot   = QuaternionSlerp(v.camRot, eye.rot, 1.0f - expf(-7.0f * frameDt));
    PlaceCamera(v);
}

// ---------------------------------------------------------------------------
// The scene
// ---------------------------------------------------------------------------
void DrawScene(const GameState &g, const Ship &eye, ViewState &v, const RenderAssets &assets,
               float frameDt, SceneInfo &out)
{
    const std::vector<StarSystem> &systems = g.systems;
    const LoadedSystem &loaded = g.loaded;
    std::vector<NavBody>  &nav = out.nav;
    std::vector<Occluder> &occluders = out.occluders;
    nav.clear();
    occluders.clear();
    float hyperFrac = HyperFraction(Vector3Length(eye.vel));

    BeginMode3D(v.camera);

    struct SphereDraw { DVec3 rel; double radius; Color lines, fill; const VectorModel *model;
                        const Zone *ring = nullptr; bool body = true;
                        Vector3 axis = { 0, 1, 0 }; float angle = 0.0f; };
    std::vector<SphereDraw> spheres;

    // Stars of every system (the loaded one is handled with its planets)
    for (int j = 0; j < (int)systems.size(); j++) {
        if (j == loaded.index) continue;
        DVec3 rel = FromShip(systems, eye, j);
        bool  big = SphereVisible(rel, systems[j].starRadius);
        const StarInfo &st = systems[j].stars[0];
        if (big) spheres.push_back({ rel, systems[j].starRadius, Whiten(st.color), st.color, &assets.star, nullptr, true });
        occluders.push_back({ rel, systems[j].starRadius });
        NavBody nb;
        nb.id = TargetId::MakeStar(j); nb.name = systems[j].name; nb.rel = rel; nb.radius = systems[j].starRadius;
        nb.color = st.color; nb.star = true; nb.dot = !big; nb.occ = (int)occluders.size() - 1;
        nb.info = StarSummary(systems[j]);
        nav.push_back(nb);
    }

    // The loaded system: star, planets, moons, orbits, belts, rings
    if (loaded.index >= 0) {
        DVec3 starRel = FromShip(systems, eye, loaded.index);
        if (v.showOrbits)
            for (const Orbit &o : loaded.orbits) DrawOrbitRel(starRel, o);
        for (const Zone &z : loaded.zones)
            if (!z.ring) DrawBeltMacro(starRel, z, v.camRight);

        std::vector<const Zone *> ringOf(loaded.bodies.size(), nullptr);
        for (const Zone &z : loaded.zones) if (z.ring) ringOf[z.parent] = &z;
        std::vector<int> navOf(loaded.bodies.size(), -1);

        // Stations turn about the system's axis: tip the model's +Y onto it, then spin
        Quaternion tip = QuaternionFromVector3ToVector3({ 0, 1, 0 }, ToF(systems[loaded.index].n));
        for (int bi = 0; bi < (int)loaded.bodies.size(); bi++) {
            const Body &b = loaded.bodies[bi];
            DVec3 rel = starRel + b.local;
            bool  big = SphereVisible(rel, b.radius);
            if (b.kind == Kind::Station) {
                if (b.parent >= 0 && loaded.bodies[b.parent].kind == Kind::Moon && navOf[b.parent] < 0) continue;  // with its moon
                if (big) {
                    float spinDeg = fmodf((float)fmod(g.clock * b.spin, 360.0), 360.0f);
                    Quaternion q = QuaternionMultiply(QuaternionFromAxisAngle(ToF(systems[loaded.index].n), spinDeg * DEG2RAD), tip);
                    SphereDraw sd = { rel, b.radius, b.lines, b.fill, &assets.station, nullptr, true };
                    QuaternionToAxisAngle(q, &sd.axis, &sd.angle);
                    sd.angle *= RAD2DEG;
                    spheres.push_back(sd);
                }
                occluders.push_back({ rel, b.radius * 0.5 });
                NavBody nb;
                nb.id = TargetId::MakeBody(loaded.index, bi); nb.name = b.name; nb.rel = rel; nb.radius = b.radius;
                nb.color = b.lines; nb.dot = !big; nb.occ = (int)occluders.size() - 1; nb.info = WorldSummary(b);
                nb.parent = navOf[b.parent];
                navOf[bi] = (int)nav.size();
                nav.push_back(nb);
                continue;
            }
            bool  ringBig = ringOf[bi] && SphereVisible(rel, ringOf[bi]->rOut);
            if (big || ringBig)
                spheres.push_back({ rel, b.radius, b.lines, b.fill,
                                    b.kind == Kind::Star ? &assets.star : &assets.planet, ringBig ? ringOf[bi] : nullptr, big });
            occluders.push_back({ rel, b.radius });
            // Moons only get a dot (and can only be targeted) when you're near them
            if (b.kind == Kind::Moon && Len(rel) > 0.02 * AU_U) continue;
            NavBody nb;
            nb.id = TargetId::MakeBody(loaded.index, bi); nb.name = b.name; nb.rel = rel; nb.radius = b.radius;
            nb.color = b.kind == Kind::Star ? b.fill : b.lines; nb.star = b.kind == Kind::Star; nb.dot = !big;
            nb.occ = (int)occluders.size() - 1; nb.info = WorldSummary(b);
            nb.parent = b.parent >= 0 ? navOf[b.parent] : -1;
            navOf[bi] = (int)nav.size();
            nav.push_back(nb);
        }

        // Belts and rings as targets: their nearest arrival point
        DVec3 shipLocal = starRel * -1.0;
        for (int zi = 0; zi < (int)loaded.zones.size(); zi++) {
            const Zone &z = loaded.zones[zi];
            NavBody nb;
            nb.id = TargetId::MakeZone(loaded.index, zi); nb.name = z.name;
            nb.rel = starRel + ZoneTargetPoint(z, shipLocal); nb.radius = 0.0; nb.color = z.color; nb.dot = true;
            nb.parent = navOf[z.parent];
            nb.info   = std::string(z.ring ? "Ring system - " : "Asteroid belt - ") + CompositionName(z.composition);
            nav.push_back(nb);
        }
    }

    // Spheres back to front, each on a fresh depth buffer, so nearer bodies always cover farther ones
    std::sort(spheres.begin(), spheres.end(), [](const SphereDraw &a, const SphereDraw &b) {
        return Len(a.rel) - a.radius > Len(b.rel) - b.radius;
    });
    for (const SphereDraw &s : spheres) {
        ClearDepth();
        if (s.body) DrawBodyRel(s.rel, s.radius, s.lines, s.fill, *s.model, s.axis, s.angle);
        if (s.ring) {
            double a = fmax(Len(s.rel) - s.radius, 1.0);
            DrawRingRel(s.rel, *s.ring, MapDist(a) / a);   // same compression as its planet
        }
    }
    ClearDepth();                                         // rocks, the ship and dust go on top

    // Nearby rocks, sharing one depth layer with the ship
    if (loaded.index >= 0 && g.field.system == loaded.index) {
        DVec3 starRel = FromShip(systems, eye, loaded.index);
        float now     = (float)g.clock;
        for (const auto &cell : g.field.cells)
            for (const Asteroid &a : cell.second) {
                DVec3  rel = starRel + a.local;
                double d   = Len(rel);
                float  r   = AsteroidRadius(a);
                if (d > AST_DRAW_RANGE || r / d < 0.0015) continue;              // too far / too small
                if (Vector3DotProduct(ToF(rel), v.camFwd) < -r - 10.0f) continue; // behind the camera
                double s = MapDist(d) / d;
                DrawVectorModelEx(assets.rocks[a.tmpl], ToF(rel * s), a.axis, a.phase + a.spin * now,
                                  { (float)(r * s), (float)(r * s), (float)(r * s) }, GoodData(a.ore).color, BLACK);
                // Anything big enough to draw as a wireframe is targetable, unless you're
                // going so fast the rocks are just streaming past (a lock would chase them)
                if (Vector3Length(eye.vel) < AST_TARGET_MAX_SPEED) {
                    NavBody nb;
                    nb.id = TargetId::MakeRock(loaded.index, a.id); nb.name = a.name; nb.rel = rel;
                    nb.radius = r; nb.color = GoodData(a.ore).color;
                    nb.info = TextFormat("%s  %.0f t", GoodData(a.ore).name, a.oreLeft);
                    nav.push_back(nb);
                }
            }
    }

    // Other captains' ships: drawn when they're close enough to see, a nav dot at any range
    for (const Traffic &t : g.traffic) {
        DVec3  rel = FromShip(systems, eye, t.ship.anchor, t.ship.pos);
        double d   = Len(rel);
        Color  col = CaptainColor(t.id);
        bool   big = d > 1e-3 && SHIP_RADIUS / d > 0.0006;         // within ~20 km
        if (big) {
            double  s  = MapDist(d) / d;
            float   sc = (float)s;
            Vector3 at = ToF(rel * s);
            Vector3 axis; float angle;
            QuaternionToAxisAngle(t.ship.rot, &axis, &angle);
            DrawVectorModelEx(assets.ship, at, axis, angle * RAD2DEG, { sc, sc, sc }, col);
            if (t.thrusting || t.hyper) {
                float hf  = HyperFraction(Vector3Length(t.ship.vel));
                float len = (t.hyper ? 2.5f + 3.0f * hf : 1.2f) * (0.85f + 0.15f * sinf((float)GetTime() * 60.0f + t.id));
                Vector3 tip = Vector3Add(at, Vector3Scale(Vector3RotateByQuaternion({ 0.0f, 0.05f, 1.0f + len }, t.ship.rot), sc));
                const Vector3 base[] = { { -0.4f, 0.0f, 1.0f }, { 0.4f, 0.0f, 1.0f }, { 0.0f, 0.25f, 1.0f }, { 0.0f, -0.12f, 1.0f } };
                for (const Vector3 &p : base)
                    DrawLine3D(Vector3Add(at, Vector3Scale(Vector3RotateByQuaternion(p, t.ship.rot), sc)), tip, FLAME_COLOR);
            }
            if (t.mining && loaded.index >= 0)
                if (const Asteroid *a = FindAsteroid(g.field, t.miningRock)) {
                    DVec3  rr = FromShip(systems, eye, loaded.index, a->local);
                    double rd = Len(rr), rs = MapDist(rd) / rd;
                    Vector3 end   = ToF((rr - rr * (AsteroidRadius(*a) * 0.6 / rd)) * rs);
                    Vector3 start = Vector3Add(at, Vector3Scale(Vector3RotateByQuaternion({ 0.0f, 0.0f, -2.0f }, t.ship.rot), sc));
                    float   fl    = 0.5f + 0.5f * sinf((float)GetTime() * 47.0f + t.id);
                    DrawLine3D(start, end, ColorLerp({ 255, 120, 30, 255 }, { 255, 230, 120, 255 }, fl));
                }
        }
        NavBody nb;
        nb.id = TargetId::MakeShip(t.id); nb.name = t.name; nb.rel = rel; nb.radius = SHIP_RADIUS;
        nb.color = col; nb.dot = !big;
        nb.info = TextFormat("Captain  -  %s", t.mining ? "mining" : t.hyper ? "under drive" : "cruising");
        nav.push_back(nb);
    }

    v.dust.UpdateAndDraw(Vector3Scale(eye.vel, frameDt), eye.vel, v.camRight, hyperFrac);

    Vector3 shipAxis; float shipAngle;
    QuaternionToAxisAngle(eye.rot, &shipAxis, &shipAngle);
    DrawVectorModelEx(assets.ship, Vector3Zero(), shipAxis, shipAngle * RAD2DEG, { 1, 1, 1 }, SHIP_LINES);

    // Engine flame when thrusting forward or in hyper/warp
    const Controls &in = g.flight.last;
    if (in.forward > 0.0f || in.hyper) {
        float len = (in.hyper ? 2.5f + 3.0f * hyperFrac : 1.2f)
                  * (0.85f + 0.15f * sinf((float)GetTime() * 60.0f));
        Vector3 tip = Vector3RotateByQuaternion({ 0.0f, 0.05f, 1.0f + len }, eye.rot);
        const Vector3 base[] = { { -0.4f, 0.0f, 1.0f }, { 0.4f, 0.0f, 1.0f },
                                 { 0.0f, 0.25f, 1.0f }, { 0.0f, -0.12f, 1.0f } };
        for (const Vector3 &p : base)
            DrawLine3D(Vector3RotateByQuaternion(p, eye.rot), tip, FLAME_COLOR);
    }

    // Mining beam: nose to the rock's surface, flickering
    if (g.flight.miningActive && loaded.index >= 0)
        if (const Asteroid *a = FindAsteroid(g.field, g.flight.miningRock)) {
            DVec3  rel = FromShip(systems, eye, loaded.index, a->local);
            double d = Len(rel), s = MapDist(d) / d;
            Vector3 end   = ToF((rel - rel * (AsteroidRadius(*a) * 0.6 / d)) * s);
            Vector3 start = Vector3RotateByQuaternion({ 0.0f, 0.0f, -2.0f }, eye.rot);
            float   fl    = 0.5f + 0.5f * sinf((float)GetTime() * 47.0f);
            Color   core  = ColorLerp({ 255, 120, 30, 255 }, { 255, 230, 120, 255 }, fl);
            DrawLine3D(start, end, core);
            for (int k = 0; k < 2; k++) {               // a couple of jittering side strands
                Vector3 j = { GetRandomValue(-10, 10) * 0.03f, GetRandomValue(-10, 10) * 0.03f, GetRandomValue(-10, 10) * 0.03f };
                DrawLine3D(Vector3Add(start, Vector3Scale(j, 0.3f)), Vector3Add(end, j), { 255, 170, 60, 255 });
            }
            // Sparks where it hits
            float sp = Vector3Distance(start, end) * 0.04f + 0.5f;
            for (int k = 0; k < 5; k++) {
                Vector3 dir = Vector3Normalize({ (float)GetRandomValue(-100, 100), (float)GetRandomValue(-100, 100), (float)GetRandomValue(-100, 100) });
                DrawLine3D(end, Vector3Add(end, Vector3Scale(dir, sp * GetRandomValue(30, 100) / 100.0f)), { 255, 220, 140, 255 });
            }
        }

    EndMode3D();
}
