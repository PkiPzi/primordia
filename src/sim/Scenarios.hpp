#pragma once
#include "sim/World.hpp"

namespace pm {

// Ready-made experiments, in the spirit of Primer's evolution videos.
struct Scenario {
    const char* name;
    const char* icon;
    const char* tagline;
    const char* description;
    void (*setup)(WorldSettings&, SimParams&);
    void (*post)(World&); // optional: adjust founders after the world is created
    const char* watch;    // what to look for, and where
};

// What to aim for in a scenario. progress() reads only the world's own history, so it survives save, load and rewind:
// below 0 = failed (rewind and try again), 0..1 = how far along, 1 or more = achieved.
struct Objective {
    const char* goal;
    float (*progress)(const World&);
};

int scenarioCount();
const Objective& objective(int i);
const Scenario& scenario(int i);

} // namespace pm
