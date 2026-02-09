#pragma once

// A small vec math library for vec3 only

#include <array>
#include <cmath>
#include <cstdint>

struct Vec3 {
    float x, y, z;
};
static_assert(sizeof(Vec3) == 3 * 4, "Vec3 is exactly 3 32-bit floats");

inline Vec3 operator+(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }