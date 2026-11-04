#pragma once
#include <vector>

#include "core/Math.hpp"
#include "core/Random.hpp"

struct ImFont;

namespace pm {

// A self-contained recreation of the classic "speed, size and sense" natural-selection experiment
// popularised by Primer: blobs leave the edge of an arena each day, eat, and must get home.
// One food = survive, two = reproduce. Every step costs size^3 * speed^2 + sense.
class PrimerLab {
public:
    void draw(bool* open, float dt, ImFont* titleFont);

private:
    struct Blob {
        Vec2 pos, home, dir;
        float speed = 1.f, size = 1.f, sense = 1.f;
        float energy = 0.f;
        int eaten = 0;
        bool homeSafe = false, alive = true, eatenByOther = false;
        float wobble = 0.f;
    };
    struct DayStats {
        int population;
        float speed, size, sense;
    };

    void reset();
    void startDay();
    void step();
    void endDay();
    Vec2 edgePoint();
    float cost(const Blob& b) const { return b.size * b.size * b.size * b.speed * b.speed + b.sense; }

    std::vector<Blob> blobs_;
    std::vector<Vec2> food_;
    std::vector<DayStats> history_;
    std::vector<float> snapSpeed_, snapSize_, snapSense_;
    Rng rng_{2024};
    bool initialised_ = false, running_ = true;
    int day_ = 0, stepInDay_ = 0;
    float accum_ = 0.f;
    // Settings
    int startBlobs_ = 20, foodPerDay_ = 60;
    float mutation_ = 0.15f, simSpeed_ = 4.f;
    bool evolveSpeed_ = true, evolveSize_ = true, evolveSense_ = true, predation_ = true;
    static constexpr float ARENA = 100.f;
    static constexpr float DAY_ENERGY = 1400.f;
    static constexpr int MAX_STEPS = 900;
};

} // namespace pm
