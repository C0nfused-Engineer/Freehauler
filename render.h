// render.h - drawing the 3D scene.
// Everything is drawn relative to the ship ("eye") with far distances
// compressed logarithmically, so everything from the hull to the stars on the
// scope fits in the depth buffer. The rest of the galaxy is a backdrop (Sky).
// Drawing never changes GameState; it hands back the nav list (every body on
// screen) for the overlay to pick targets from.
#pragma once
#include "game.h"

#define VECTOR_LINE_DEPTH_BIAS 0.003f
#include "vector_render.h"
#include <string>
#include <vector>

// Distance compression: within BUBBLE units of the camera everything is drawn at
// true scale. Beyond that, distance d is drawn at BUBBLE + LOG_K * ln(d / BUBBLE).
const double BUBBLE = 2000.0;               // 10 km of true-scale space around the ship
const double LOG_K  = 6000.0;

inline double MapDist(double d) { return d <= BUBBLE ? d : BUBBLE + LOG_K * log(d / BUBBLE); }
inline Vector3 MapPoint(DVec3 rel)
{
    double d = Len(rel);
    return d < 1e-9 ? Vector3Zero() : ToF(rel * (MapDist(d) / d));
}

struct RenderAssets {
    VectorModel ship, planet, star, station;
    std::vector<VectorModel> rocks;          // AST_TEMPLATES procedural shapes
    void Load();
    void Unload();
};

// Space dust: specks in a box that wraps around the ship, giving a sense of motion
struct Dust {
    std::vector<Vector3> specks;             // offsets from the ship
    float box = 160.0f;
    void Init(unsigned seed, int count);
    void UpdateAndDraw(Vector3 moved, Vector3 vel, Vector3 camRight, float hyper);
};

// The galaxy beyond the stars on the scope: tens of billions of them, far too many
// and too far to draw one by one, so they're a backdrop of specks laid out by how
// much starlight comes from each direction - thick along the disk, thin toward
// the poles, dimmed by dust. Rebuilt when you've flown far enough to change the view.
const int    SKY_SPECKS     = 6000;
const double SKY_REBUILD_LY = 150.0;
const float  SKY_DISTANCE   = 300000.0f;     // drawn behind everything (compressed space tops out around 190,000)

struct Sky {
    struct Speck { Vector3 dir; Color color; bool big; };
    std::vector<Speck> specks;
    DVec3 madeAt;                            // galactic position (ly) it was made for
    void  Update(DVec3 hereLy);
    void  Draw(Vector3 camRight) const;
};

struct ViewState {
    Camera3D   camera = {};
    Quaternion camRot = { 0, 0, 0, 1 };
    float      hyperVis = 0.0f;              // smoothed 0..1 hyper amount (camera pull, FOV)
    Vector3    camRight = { 1, 0, 0 }, camFwd = { 0, 0, -1 };
    bool       showOrbits = true;
    Dust       dust;
    Sky        sky;
};

// Everything the nav overlay needs to know about one body this frame
struct NavBody {
    TargetId    id;
    std::string name;
    DVec3       rel;          // ship -> body center
    double      radius = 0;
    Color       color = WHITE;
    bool        star = false;
    float       bright = 1.0f;// stars: how bright it looks from here, 0..1 (sizes and dims its dot)
    bool        dot = false;  // draw a nav dot (too small to draw as a sphere)
    int         occ = -1;     // index in the occluder list (so it doesn't block itself)
    int         parent = -1;  // nav index of what it belongs to; unselectable until it separates on screen
    std::string info;         // extra line for the target panel
};
struct Occluder { DVec3 rel; double radius; };
struct SceneInfo {
    std::vector<NavBody>  nav;
    std::vector<Occluder> occluders;
};

// 0 at cruise speed, 1 at full hyper (log scale)
float HyperFraction(float speed);
void  ResetView(ViewState &v, const Ship &eye);
void  UpdateView(ViewState &v, const Ship &eye, float frameDt);
void  DrawScene(const GameState &g, const Ship &eye, ViewState &v, const RenderAssets &assets,
                float frameDt, SceneInfo &out);
// True if any other body sits between the ship and the target
bool  LineOfSightBlocked(DVec3 target, double targetRadius, const std::vector<Occluder> &occ, int self);
