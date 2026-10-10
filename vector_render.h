// vector_render.h — retro "vector graphics" rendering for raylib (C++)
//
// Solid-filled faces (black by default) with colored edges on top, so
// the fill hides back edges the way Battlezone / Elite looked.
//
// Usage:
//   VectorModel ship = LoadVectorModel(LoadModel("ship.obj"));
//   ...
//   BeginMode3D(camera);
//       DrawVectorModel(ship, {0, 0, 0}, 1.0f, GREEN);             // black fill
//       DrawVectorModel(ship, {3, 0, 0}, 1.0f, GREEN, DARKBLUE);   // custom fill
//       DrawVectorPlane({0, -1, 0}, {20, 20}, 10, GREEN);          // grid floor
//       DrawVectorModel(ship, {6, 0, 0}, 1.0f, GREEN, BLANK);      // no fill: see-through
//   EndMode3D();
//
// A fill color with alpha 0 (e.g. BLANK) skips the fill entirely.
//   ...
//   UnloadVectorModel(ship);

#pragma once

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <cmath>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

// Keeping edges visible on top of their own fill takes two mechanisms:
//  1. Polygon offset pushes the fill slightly away from the camera, by more
//     where a face is seen at a grazing angle. This is what keeps edges
//     solid on faces seen almost edge-on.
//  2. Lines are also pulled toward the camera by this fraction of their
//     distance, which covers coarse depth precision on very distant objects.
//     Raise it if far edges flicker, lower it if edges show through faces.
#ifndef VECTOR_LINE_DEPTH_BIAS
#define VECTOR_LINE_DEPTH_BIAS 0.0015f
#endif
#ifndef VECTOR_FILL_OFFSET_FACTOR
#define VECTOR_FILL_OFFSET_FACTOR 1.5f     // slope-scaled part of the polygon offset
#endif
#ifndef VECTOR_FILL_OFFSET_UNITS
#define VECTOR_FILL_OFFSET_UNITS  2.0f     // constant part, in depth-buffer steps
#endif

// raylib doesn't expose glPolygonOffset, so declare the three OpenGL 1.1
// functions we need by hand. Every desktop GL library exports them
// (opengl32.dll, libGL.so, the macOS framework). Including <GL/gl.h> instead
// would pull in <windows.h> on Windows, which clashes with raylib's names.
// Desktop OpenGL only; not for OpenGL ES / web builds.
#if defined(_WIN32)
    #define VECTOR_GLAPI  __declspec(dllimport)
    #define VECTOR_GLCALL __stdcall
#else
    #define VECTOR_GLAPI
    #define VECTOR_GLCALL
#endif
extern "C" {
    VECTOR_GLAPI void VECTOR_GLCALL glEnable(unsigned int cap);
    VECTOR_GLAPI void VECTOR_GLCALL glDisable(unsigned int cap);
    VECTOR_GLAPI void VECTOR_GLCALL glPolygonOffset(float factor, float units);
}

struct VectorModel {
    Model model;                 // the mesh used for the fill
    std::vector<Vector3> edges;  // line segments in model space, stored as pairs (a, b)
};

// ---------------------------------------------------------------------------
// Edge extraction
//
// Raylib meshes are triangles, so drawing every triangle edge puts a
// diagonal across every flat face. Instead we keep only the edges that
// matter visually:
//   - boundary edges (used by one triangle), and
//   - crease edges (where the two triangles meet at more than creaseAngleDeg).
// Edges between coplanar triangles (the diagonals) are dropped.
// ---------------------------------------------------------------------------
inline std::vector<Vector3> ExtractVectorEdges(const Mesh *meshes, int meshCount,
                                               float creaseAngleDeg = 1.0f)
{
    using Key = std::tuple<long long, long long, long long>;
    const float weldEps = 1e-4f;
    const float creaseCos = cosf(creaseAngleDeg * DEG2RAD);

    std::vector<Vector3> edges;

    for (int m = 0; m < meshCount; m++) {
        const Mesh &mesh = meshes[m];
        if (mesh.vertices == nullptr) continue;   // no CPU-side data

        // Weld vertices by position. Raylib duplicates vertices per face
        // (for normals/UVs), which would otherwise hide shared edges.
        std::map<Key, int> weldMap;
        std::vector<Vector3> positions;
        auto weld = [&](int v) {
            Vector3 p = { mesh.vertices[3*v], mesh.vertices[3*v + 1], mesh.vertices[3*v + 2] };
            Key k = { llroundf(p.x / weldEps), llroundf(p.y / weldEps), llroundf(p.z / weldEps) };
            auto it = weldMap.find(k);
            if (it != weldMap.end()) return it->second;
            int id = (int)positions.size();
            positions.push_back(p);
            weldMap.emplace(k, id);
            return id;
        };

        // Edge (lowId, highId) -> normals of the triangles that use it
        std::map<std::pair<int, int>, std::vector<Vector3>> edgeFaces;

        for (int t = 0; t < mesh.triangleCount; t++) {
            int v[3];
            for (int i = 0; i < 3; i++) {
                int raw = mesh.indices ? mesh.indices[3*t + i] : 3*t + i;
                v[i] = weld(raw);
            }
            if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;  // degenerate

            Vector3 n = Vector3CrossProduct(Vector3Subtract(positions[v[1]], positions[v[0]]),
                                            Vector3Subtract(positions[v[2]], positions[v[0]]));
            if (Vector3Length(n) < 1e-12f) continue;  // zero-area triangle
            n = Vector3Normalize(n);

            for (int i = 0; i < 3; i++) {
                int a = v[i], b = v[(i + 1) % 3];
                if (a > b) std::swap(a, b);
                edgeFaces[{a, b}].push_back(n);
            }
        }

        for (const auto &[edge, normals] : edgeFaces) {
            bool keep = (normals.size() == 1);  // boundary edge
            for (size_t i = 0; !keep && i < normals.size(); i++)
                for (size_t j = i + 1; !keep && j < normals.size(); j++)
                    if (Vector3DotProduct(normals[i], normals[j]) < creaseCos) keep = true;

            if (keep) {
                edges.push_back(positions[edge.first]);
                edges.push_back(positions[edge.second]);
            }
        }
    }
    return edges;
}

// Wraps a model and precomputes its edge list (do this once, not per frame).
inline VectorModel LoadVectorModel(Model model, float creaseAngleDeg = 1.0f)
{
    VectorModel vm;
    vm.model = model;
    vm.edges = ExtractVectorEdges(model.meshes, model.meshCount, creaseAngleDeg);
    return vm;
}

inline void UnloadVectorModel(VectorModel &vm)
{
    UnloadModel(vm.model);
    vm.edges.clear();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace vector_render_detail {

// Camera position recovered from the current view matrix (valid inside BeginMode3D).
inline Vector3 CameraPosition()
{
    Matrix inv = MatrixInvert(rlGetMatrixModelview());
    return { inv.m12, inv.m13, inv.m14 };
}

// Wrap fill drawing in these. The batch flushes make sure the offset applies
// to the fill only (raylib queues some geometry and draws it later).
inline void BeginFillOffset()
{
    rlDrawRenderBatchActive();
    glEnable(0x8037);   // GL_POLYGON_OFFSET_FILL
    glPolygonOffset(VECTOR_FILL_OFFSET_FACTOR, VECTOR_FILL_OFFSET_UNITS);
}

inline void EndFillOffset()
{
    rlDrawRenderBatchActive();
    glDisable(0x8037);
}

// Draw a world-space line nudged slightly toward the camera.
inline void BiasedLine(Vector3 a, Vector3 b, Vector3 cam, Color color)
{
    a = Vector3Lerp(a, cam, VECTOR_LINE_DEPTH_BIAS);
    b = Vector3Lerp(b, cam, VECTOR_LINE_DEPTH_BIAS);
    DrawLine3D(a, b, color);
}

} // namespace vector_render_detail

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Full version: position, rotation (axis + degrees) and per-axis scale.
// model.transform is respected too, same as raylib's DrawModelEx.
inline void DrawVectorModelEx(const VectorModel &vm, Vector3 position,
                              Vector3 rotationAxis, float rotationAngle, Vector3 scale,
                              Color lineColor, Color fillColor = BLACK)
{
    // 1. Fill. Writes the depth buffer so hidden edges get occluded.
    if (fillColor.a > 0) {
        vector_render_detail::BeginFillOffset();
        DrawModelEx(vm.model, position, rotationAxis, rotationAngle, scale, fillColor);
        vector_render_detail::EndFillOffset();
    }

    // 2. Edges. Same transform DrawModelEx builds: transform * S * R * T
    Matrix world = MatrixMultiply(
        vm.model.transform,
        MatrixMultiply(MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z),
                                      MatrixRotate(rotationAxis, rotationAngle * DEG2RAD)),
                       MatrixTranslate(position.x, position.y, position.z)));

    Vector3 cam = vector_render_detail::CameraPosition();
    for (size_t i = 0; i + 1 < vm.edges.size(); i += 2) {
        Vector3 a = Vector3Transform(vm.edges[i], world);
        Vector3 b = Vector3Transform(vm.edges[i + 1], world);
        vector_render_detail::BiasedLine(a, b, cam, lineColor);
    }
}

// Simple version: position + uniform scale. Rotate via vm.model.transform.
inline void DrawVectorModel(const VectorModel &vm, Vector3 position, float scale,
                            Color lineColor, Color fillColor = BLACK)
{
    DrawVectorModelEx(vm, position, { 0.0f, 1.0f, 0.0f }, 0.0f,
                      { scale, scale, scale }, lineColor, fillColor);
}

// Plane with a line grid, rotated about its center (rotationAngle in degrees).
// With no rotation it lies flat in XZ. divisions = grid cells per side
// (1 = just the outline). A filled plane is solid from both sides.
inline void DrawVectorPlaneEx(Vector3 center, Vector2 size, int divisions,
                              Vector3 rotationAxis, float rotationAngle,
                              Color lineColor, Color fillColor = BLACK)
{
    if (divisions < 1) divisions = 1;

    // 1. Fill, with backface culling off so it's solid from either side.
    //    Flush the batch around the state change so it applies to the plane only.
    if (fillColor.a > 0) {
        vector_render_detail::BeginFillOffset();
        rlDisableBackfaceCulling();
        rlPushMatrix();
            rlTranslatef(center.x, center.y, center.z);
            rlRotatef(rotationAngle, rotationAxis.x, rotationAxis.y, rotationAxis.z);
            DrawPlane({ 0.0f, 0.0f, 0.0f }, size, fillColor);
        rlPopMatrix();
        vector_render_detail::EndFillOffset();
        rlEnableBackfaceCulling();
    }

    // 2. Grid lines, built flat around the origin then rotated and moved into place.
    Matrix world = MatrixMultiply(MatrixRotate(rotationAxis, rotationAngle * DEG2RAD),
                                  MatrixTranslate(center.x, center.y, center.z));
    Vector3 cam = vector_render_detail::CameraPosition();
    float hx = size.x * 0.5f, hz = size.y * 0.5f;

    for (int i = 0; i <= divisions; i++) {
        float t = (float)i / (float)divisions;
        float x = -hx + t * size.x;
        float z = -hz + t * size.y;
        vector_render_detail::BiasedLine(Vector3Transform({ x, 0.0f, -hz }, world),
                                         Vector3Transform({ x, 0.0f,  hz }, world), cam, lineColor);
        vector_render_detail::BiasedLine(Vector3Transform({ -hx, 0.0f, z }, world),
                                         Vector3Transform({  hx, 0.0f, z }, world), cam, lineColor);
    }
}

// Flat XZ plane, no rotation.
inline void DrawVectorPlane(Vector3 center, Vector2 size, int divisions,
                            Color lineColor, Color fillColor = BLACK)
{
    DrawVectorPlaneEx(center, size, divisions, { 0.0f, 1.0f, 0.0f }, 0.0f, lineColor, fillColor);
}
