// core.h - units, double-precision vectors, hashing and random numbers.
// Everything else builds on this; it has no game knowledge.
#pragma once

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Units: 1 world unit = 5 m
// ---------------------------------------------------------------------------
const double METERS_PER_UNIT = 5.0;
const double KM_U        = 1000.0 / METERS_PER_UNIT;            // units per km
const double AU_U        = 1.495978707e11 / METERS_PER_UNIT;    // units per AU
const double LY_U        = 9.4607304725808e15 / METERS_PER_UNIT;// units per light year
const double RSUN_U      = 6.957e8 / METERS_PER_UNIT;           // units per solar radius
const double LIGHT_SEC_U = 299792.458 * KM_U;                   // units per light second

// ---------------------------------------------------------------------------
// Double-precision vector (positions; raylib's Vector3 is float)
// ---------------------------------------------------------------------------
struct DVec3 { double x = 0.0, y = 0.0, z = 0.0; };
inline DVec3   operator+(DVec3 a, DVec3 b)   { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline DVec3   operator-(DVec3 a, DVec3 b)   { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline DVec3   operator*(DVec3 a, double s)  { return { a.x * s, a.y * s, a.z * s }; }
inline double  Dot(DVec3 a, DVec3 b)         { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double  Len(DVec3 a)                  { return sqrt(Dot(a, a)); }
inline DVec3   Norm(DVec3 a)                 { double l = Len(a); return l > 0.0 ? a * (1.0 / l) : a; }
inline DVec3   Cross(DVec3 a, DVec3 b)       { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline DVec3   ToD(Vector3 v)                { return { v.x, v.y, v.z }; }
inline Vector3 ToF(DVec3 v)                  { return { (float)v.x, (float)v.y, (float)v.z }; }

// ---------------------------------------------------------------------------
// Hashing and random numbers
// ---------------------------------------------------------------------------
// Rule for generation: every generator draws from its OWN stream, so adding a
// new feature never shifts the random numbers an existing one sees (and the
// galaxy you know stays the same). New streams get new constants.
inline uint64_t Mix64(uint64_t x)              // splitmix64 finalizer
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline uint64_t NameHash(const std::string &n) // FNV-1a
{
    uint64_t h = 1469598103934665603ull;
    for (char c : n) h = (h ^ (uint8_t)c) * 1099511628211ull;
    return h;
}

struct Rng64 {
    uint64_t s;
    double uni() { s = Mix64(s); return (s >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline Color Dim(Color c, float k)
{
    return { (unsigned char)(c.r * k), (unsigned char)(c.g * k), (unsigned char)(c.b * k), 255 };
}
inline Color Whiten(Color c)                   // halfway to white (star outlines, ring colors)
{
    return { (unsigned char)((c.r + 255) / 2), (unsigned char)((c.g + 255) / 2), (unsigned char)((c.b + 255) / 2), 255 };
}
