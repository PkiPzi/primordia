#include "core/Noise.hpp"

#include <cmath>
#include <numeric>

#include "core/Random.hpp"

namespace pm {

namespace {
constexpr float GRAD[12][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0},
                               {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 1}, {0, -1}};
inline int fastFloor(float x) { int i = int(x); return x < float(i) ? i - 1 : i; }
} // namespace

Noise2D::Noise2D(uint64_t seed) {
    std::array<uint8_t, 256> p{};
    std::iota(p.begin(), p.end(), uint8_t(0));
    Rng rng(seed, 0xA5A5A5A5ULL);
    for (int i = 255; i > 0; --i) {
        int j = rng.below(i + 1);
        std::swap(p[i], p[j]);
    }
    for (int i = 0; i < 512; ++i) perm_[i] = p[i & 255];
}

float Noise2D::simplex(float xin, float yin) const {
    constexpr float F2 = 0.36602540378f; // 0.5*(sqrt(3)-1)
    constexpr float G2 = 0.21132486540f; // (3-sqrt(3))/6
    float s = (xin + yin) * F2;
    int i = fastFloor(xin + s);
    int j = fastFloor(yin + s);
    float t = float(i + j) * G2;
    float x0 = xin - (float(i) - t);
    float y0 = yin - (float(j) - t);
    int i1 = x0 > y0 ? 1 : 0;
    int j1 = 1 - i1;
    float x1 = x0 - float(i1) + G2, y1 = y0 - float(j1) + G2;
    float x2 = x0 - 1.f + 2.f * G2, y2 = y0 - 1.f + 2.f * G2;
    int ii = i & 255, jj = j & 255;
    int g0 = perm_[ii + perm_[jj]] % 12;
    int g1 = perm_[ii + i1 + perm_[jj + j1]] % 12;
    int g2 = perm_[ii + 1 + perm_[jj + 1]] % 12;
    float n = 0.f;
    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0) { t0 *= t0; n += t0 * t0 * (GRAD[g0][0] * x0 + GRAD[g0][1] * y0); }
    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0) { t1 *= t1; n += t1 * t1 * (GRAD[g1][0] * x1 + GRAD[g1][1] * y1); }
    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0) { t2 *= t2; n += t2 * t2 * (GRAD[g2][0] * x2 + GRAD[g2][1] * y2); }
    return 70.f * n;
}

float Noise2D::fbm(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.f, amp = 1.f, norm = 0.f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * simplex(x, y);
        norm += amp;
        x = x * lacunarity + 17.31f;
        y = y * lacunarity - 9.17f;
        amp *= gain;
    }
    return sum / norm;
}

float Noise2D::ridged(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.f, amp = 1.f, norm = 0.f;
    for (int o = 0; o < octaves; ++o) {
        float v = 1.f - std::fabs(simplex(x, y));
        sum += amp * v * v;
        norm += amp;
        x = x * lacunarity + 5.71f;
        y = y * lacunarity + 3.33f;
        amp *= gain;
    }
    return sum / norm;
}

} // namespace pm
