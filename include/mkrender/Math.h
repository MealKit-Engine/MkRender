#pragma once

#include "mkrender/Config.h"

#include <cmath>

// Row vector convention (v * M), left-handed coordinates (x right, y up, z forward). View-side float math.
// A host can use its own types instead (mkrender/Config.h, MKRENDER_HOST_MATH).
#ifndef MKRENDER_HOST_MATH
namespace mkrender {

struct Vec3 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    constexpr Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3& operator+=(Vec3 b) {
        x += b.x;
        y += b.y;
        z += b.z;
        return *this;
    }
    constexpr Vec3& operator-=(Vec3 b) {
        x -= b.x;
        y -= b.y;
        z -= b.z;
        return *this;
    }
    constexpr Vec3& operator*=(f32 s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
};

constexpr Vec3 operator*(f32 s, Vec3 v) { return v * s; }
constexpr f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
constexpr f32 lengthSq(Vec3 v) { return dot(v, v); }
inline f32 length(Vec3 v) { return std::sqrt(lengthSq(v)); }
inline Vec3 normalize(Vec3 v) {
    const f32 len = length(v);
    return len > 1e-12f ? v / len : Vec3{};
}
constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
constexpr f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

struct Vec2 {
    f32 x = 0.0f, y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
};

struct Quat {
    f32 x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

constexpr Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}

struct Mat4 {
    f32 m[4][4] = {};

    static constexpr Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

// Row vector convention: v * (a * b) == (v * a) * b
constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}

inline Mat4 lookAtLH(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 zAxis = normalize(target - eye);
    const Vec3 xAxis = normalize(cross(up, zAxis));
    const Vec3 yAxis = cross(zAxis, xAxis);
    Mat4 r;
    r.m[0][0] = xAxis.x;
    r.m[0][1] = yAxis.x;
    r.m[0][2] = zAxis.x;
    r.m[1][0] = xAxis.y;
    r.m[1][1] = yAxis.y;
    r.m[1][2] = zAxis.y;
    r.m[2][0] = xAxis.z;
    r.m[2][1] = yAxis.z;
    r.m[2][2] = zAxis.z;
    r.m[3][0] = -dot(xAxis, eye);
    r.m[3][1] = -dot(yAxis, eye);
    r.m[3][2] = -dot(zAxis, eye);
    r.m[3][3] = 1.0f;
    return r;
}

// Reversed-Z perspective projection: near plane = 1, far plane = 0
inline Mat4 perspectiveReversedZ(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 yScale = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r;
    r.m[0][0] = yScale / aspect;
    r.m[1][1] = yScale;
    r.m[2][2] = zNear / (zNear - zFar);
    r.m[2][3] = 1.0f;
    r.m[3][2] = zFar * zNear / (zFar - zNear);
    return r;
}

} // namespace mkrender
#endif

namespace mkrender {
static_assert(sizeof(Vec3) == 12 && sizeof(Quat) == 16 && sizeof(Mat4) == 64, "mkrender math layouts (GPU structs hold them)");
} // namespace mkrender
