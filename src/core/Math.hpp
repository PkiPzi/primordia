#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pm {

constexpr float PI = 3.14159265358979f;
constexpr float TAU = 6.28318530717959f;

struct Vec2 {
    float x = 0.f, y = 0.f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    float lengthSq() const { return x * x + y * y; }
    float length() const { return std::sqrt(x * x + y * y); }
    float dot(Vec2 o) const { return x * o.x + y * o.y; }
};
inline constexpr Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }
inline Vec2 fromAngle(float a) { return {std::cos(a), std::sin(a)}; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp01(float v) { return clampf(v, 0.f, 1.f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

// Wrap an angle to [-PI, PI).
inline float wrapAngle(float a) {
    if (a >= -PI && a < PI) return a;
    a = std::fmod(a + PI, TAU);
    if (a < 0) a += TAU;
    return a - PI;
}

// Rational tanh approximation, max error ~2e-3, monotonic, exact saturation.
inline float fastTanh(float x) {
    if (x <= -3.f) return -1.f;
    if (x >= 3.f) return 1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// Polynomial atan2, max error ~1e-5 rad.
inline float fastAtan2(float y, float x) {
    float ax = std::fabs(x), ay = std::fabs(y);
    float mn = std::min(ax, ay), mx = std::max(ax, ay);
    float a = mn / (mx + 1e-20f);
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    if (y < 0) r = -r;
    return r;
}

// Circular distance between two hues in [0,1), result in [0, 0.5].
inline float hueDistance(float a, float b) {
    float d = std::fabs(a - b);
    return std::min(d, 1.f - d);
}

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};

inline Color hsv(float h, float s, float v, float a = 1.f) {
    h = h - std::floor(h);
    float i = std::floor(h * 6.f);
    float f = h * 6.f - i;
    float p = v * (1.f - s), q = v * (1.f - f * s), t = v * (1.f - (1.f - f) * s);
    switch (int(i) % 6) {
        case 0: return {v, t, p, a};
        case 1: return {q, v, p, a};
        case 2: return {p, v, t, a};
        case 3: return {p, q, v, a};
        case 4: return {t, p, v, a};
        default: return {v, p, q, a};
    }
}

inline Color mix(Color a, Color b, float t) {
    return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t)};
}

inline uint32_t packColor(Color c) {
    auto u = [](float v) { return uint32_t(clamp01(v) * 255.f + 0.5f); };
    return u(c.r) | (u(c.g) << 8) | (u(c.b) << 16) | (u(c.a) << 24);
}

} // namespace pm
