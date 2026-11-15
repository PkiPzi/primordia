#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pm {

struct Options {
    bool headless = false;
    bool bench = false;
    int64_t ticks = 20000;
    uint64_t seed = 0;        // 0 = random
    int threads = 0;          // 0 = all hardware threads
    int population = -1;
    float shocks = -1.f;      // random shocks per year; <0 = default
    float pace = 0.f;         // evolution pace (ageing speed and mutation multiplier); 0 = default
    int thinkEvery = 0;       // 0 = default (see SimParams::thinkEvery)
    float worldSize = 0.f;    // 0 = default
    std::string load;
    std::string save;
    std::string screenshot;   // render N frames then save a PNG and quit
    int frames = 120;
    float zoom = 0.f;         // screenshot: camera zoom (pixels per unit)
    bool select = false;      // screenshot: select and follow a creature
    bool welcome = false;     // force the welcome screen
    std::string icon;         // render the app icon PNG and quit
    int overlay = -1;
    int scenario = -1;        // start from a built-in scenario (index)
    std::vector<std::string> release; // headless: genome text files to release as designed lineages (a tournament)
    std::string newick;       // headless: write the tree of life here at the end
    std::vector<std::string> focus; // panels to bring to front (screenshot testing)
    int warmup = 0;           // ticks to simulate before the first frame
    int width = 1600, height = 960;
    bool help = false;
};

Options parseOptions(int argc, char** argv);
void printUsage();

int runHeadless(const Options& opt);
int runApp(const Options& opt);

} // namespace pm
