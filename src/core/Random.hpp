#pragma once
#include <cmath>
#include <cstdint>

#include "core/Math.hpp"

namespace pm {

inline uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// PCG32: small state, fast, statistically solid. Every creature owns one so
// parallel phases stay deterministic regardless of thread count.
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc = 0xda3e39cb94b95bdbULL;

    Rng() = default;
    explicit Rng(uint64_t seed, uint64_t seq = 0x14057B7EF767814FULL) { reseed(seed, seq); }

    void reseed(uint64_t seed, uint64_t seq = 0x14057B7EF767814FULL) {
        state = 0;
        inc = (seq << 1u) | 1u;
        next();
        state += seed;
        next();
    }

    uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-rot) & 31u));
    }
    uint64_t next64() { return (uint64_t(next()) << 32) | next(); }

    float uniform() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
    int below(int n) { return int((uint64_t(next()) * uint64_t(n)) >> 32); }
    bool chance(float p) { return uniform() < p; }
    float normal() {
        float u1 = uniform();
        if (u1 < 1e-7f) u1 = 1e-7f;
        float u2 = uniform();
        return std::sqrt(-2.f * std::log(u1)) * std::cos(TAU * u2);
    }
    Rng fork() { return Rng(next64(), next64()); }
};

} // namespace pm
