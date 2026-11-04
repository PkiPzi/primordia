#pragma once
#include <array>
#include <cstdint>

namespace pm {

// Seeded 2D simplex noise with fractal helpers.
class Noise2D {
public:
    explicit Noise2D(uint64_t seed = 1);
    float simplex(float x, float y) const;                       // ~[-1, 1]
    float fbm(float x, float y, int octaves, float lacunarity = 2.f, float gain = 0.5f) const;
    float ridged(float x, float y, int octaves, float lacunarity = 2.f, float gain = 0.5f) const;

private:
    std::array<uint8_t, 512> perm_{};
};

} // namespace pm
