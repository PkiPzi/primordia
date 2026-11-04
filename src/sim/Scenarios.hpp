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

int scenarioCount();
const Scenario& scenario(int i);

} // namespace pm
