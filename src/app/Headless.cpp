#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "app/Options.hpp"
#include "core/ThreadPool.hpp"
#include "sim/Scenarios.hpp"
#include "sim/World.hpp"

namespace pm {

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* s) { return std::strcmp(argv[i], s) == 0; };
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (is("--headless")) o.headless = true;
        else if (is("--bench")) o.bench = o.headless = true;
        else if (is("--ticks")) o.ticks = std::atoll(next());
        else if (is("--seed")) o.seed = std::strtoull(next(), nullptr, 10);
        else if (is("--threads")) o.threads = std::atoi(next());
        else if (is("--population")) o.population = std::atoi(next());
        else if (is("--think-every")) o.thinkEvery = std::atoi(next());
        else if (is("--shocks")) o.shocks = float(std::atof(next()));
        else if (is("--pace")) o.pace = float(std::atof(next()));
        else if (is("--size")) o.worldSize = float(std::atof(next()));
        else if (is("--load")) o.load = next();
        else if (is("--save")) o.save = next();
        else if (is("--screenshot")) o.screenshot = next();
        else if (is("--frames")) o.frames = std::atoi(next());
        else if (is("--zoom")) o.zoom = float(std::atof(next()));
        else if (is("--select")) o.select = true;
        else if (is("--welcome")) o.welcome = true;
        else if (is("--icon")) o.icon = next();
        else if (is("--overlay")) o.overlay = std::atoi(next());
        else if (is("--scenario")) o.scenario = std::atoi(next());
        else if (is("--newick")) o.newick = next();
        else if (is("--release")) o.release.push_back(next());
        else if (is("--focus")) o.focus.push_back(next());
        else if (is("--warmup")) o.warmup = std::atoi(next());
        else if (is("--window")) {
            o.width = std::atoi(next());
            o.height = std::atoi(next());
        } else if (is("-h") || is("--help")) o.help = true;
        else std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
    }
    return o;
}

void printUsage() {
    std::puts(
        "Primordia - evolution simulator\n\n"
        "Usage: primordia [options]\n"
        "  --seed N            World seed (default: random)\n"
        "  --size N            World width/height in units (default 4096)\n"
        "  --population N      Initial population\n"
        "  --shocks X          Random shocks (meteor, drought...) per year (default 0 = none)\n"
        "  --think-every N     Each brain runs every Nth tick (1 = every tick, default 2; faster, slightly coarser)\n"
        "  --threads N         Simulation threads (default: all cores)\n"
        "  --load FILE         Load a saved world\n"
        "  --scenario N        Start from a built-in scenario (0-12, see the welcome screen)\n"
        "  --headless          Run without a window and print statistics\n"
        "  --ticks N           Ticks to run in headless mode (default 20000)\n"
        "  --save FILE         Save the world when the headless run ends\n"
        "  --newick FILE       Write the tree of life (Newick) when the headless run ends\n"
        "  --release FILE      Headless: release the genome in FILE (Genome Lab > Copy as text) as a designed lineage;\n"
        "                      repeat to pit several designs against each other and the wild\n"
        "  --bench             Headless benchmark (ticks per second)\n"
        "  --screenshot FILE   Render --frames frames, save a PNG, quit\n"
        "  --window W H        Window size\n");
}

int runHeadless(const Options& opt) {
    ThreadPool pool(opt.threads > 0 ? unsigned(opt.threads) : 0u);
    World world(&pool);
    world.fxEnabled = false;
    if (!opt.load.empty()) {
        std::string err;
        if (!world.load(opt.load, err)) {
            std::fprintf(stderr, "Load failed: %s\n", err.c_str());
            return 1;
        }
    } else {
        WorldSettings ws;
        if (opt.scenario >= 0 && opt.scenario < scenarioCount()) scenario(opt.scenario).setup(ws, world.params);
        ws.seed = opt.seed ? opt.seed : std::random_device{}();
        if (opt.worldSize > 0) ws.width = ws.height = opt.worldSize;
        if (opt.population > 0) ws.initialPopulation = opt.population;
        if (opt.shocks >= 0.f) world.params.shocksPerYear = opt.shocks;
        if (opt.thinkEvery > 0) world.params.thinkEvery = std::clamp(opt.thinkEvery, 1, 8);
        if (opt.pace > 0.f) world.params.lifePace = world.params.mutationRate = opt.pace;
        world.create(ws);
        if (opt.scenario >= 0 && opt.scenario < scenarioCount()) {
            if (scenario(opt.scenario).post) scenario(opt.scenario).post(world);
            std::printf("Scenario: %s\n", scenario(opt.scenario).name);
        }
    }
    // Tournament: each pasted genome starts as 40 creatures on its own patch of land.
    {
        std::mt19937_64 placer(world.settings.seed ^ 0x7075726eull);
        for (const std::string& path : opt.release) {
            std::string text;
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                char buf[4096];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
                std::fclose(f);
            }
            Genome g;
            if (text.empty() || !genomeFromText(text, g)) {
                std::fprintf(stderr, "Could not read a genome from %s\n", path.c_str());
                return 1;
            }
            Vec2 at{world.terrain.width * 0.5f, world.terrain.height * 0.5f};
            for (int tries = 0; tries < 2000; ++tries) {
                Vec2 p{float(placer() % uint64_t(world.terrain.width)), float(placer() % uint64_t(world.terrain.height))};
                const int cell = world.terrain.index(p.x, p.y);
                if (!world.terrain.water[size_t(cell)] && world.terrain.fertility[size_t(cell)] > 0.3f) { at = p; break; }
            }
            world.spawnGenome(g, 0, at, 40, 120.f);
            std::printf("Released %s as %s\n", path.c_str(), world.species.all().back().name().c_str());
        }
    }
    std::printf("Primordia headless | seed %llu | %u threads | %dx%d world | %zu creatures\n",
                (unsigned long long)world.settings.seed, pool.size(), int(world.terrain.width),
                int(world.terrain.height), world.creatures.size());
    std::printf("%9s %6s %5s %5s %5s %4s %5s %6s %6s %6s %6s %6s %5s %7s\n", "tick", "pop", "herb", "omni", "carn",
                "spp", "gen", "size", "speed", "diet", "toxin", "vision", "conn", "tps");

    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    int64_t lastTick = world.tickCount;
    const int64_t end = world.tickCount + opt.ticks;
    const int64_t reportEvery = std::max<int64_t>(1000, opt.ticks / 20);
    size_t eventCursor = world.events.items.size();
    double creatureTicks = 0; // work done: sum of population over all ticks, so speed is comparable between runs
    // Are disease and thirst doing anything? Sampled every 100 ticks.
    double hydSum = 0, parchedSum = 0, infectedPeak = 0, infectedSum = 0;
    int vitalSamples = 0;
    while (world.tickCount < end) {
        creatureTicks += double(world.creatures.size());
        world.tick();
        if (world.tickCount % 100 == 0 && !world.creatures.empty()) {
            double hyd = 0, parched = 0, infected = 0;
            for (const Creature& c : world.creatures) {
                hyd += c.hydration;
                parched += c.hydration < 0.15f;
                infected += c.infection > 0.f;
            }
            const double n = double(world.creatures.size());
            hydSum += hyd / n;
            parchedSum += parched / n;
            infectedSum += infected / n;
            infectedPeak = std::max(infectedPeak, infected / n);
            ++vitalSamples;
        }
        if (world.tickCount % reportEvery == 0 || world.tickCount == end) {
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - last).count();
            double tps = double(world.tickCount - lastTick) / std::max(1e-9, secs);
            last = now;
            lastTick = world.tickCount;
            int herb = 0, omni = 0, carn = 0;
            double size = 0, speed = 0, diet = 0, toxin = 0, vision = 0, conn = 0, gen = 0;
            for (const Creature& c : world.creatures) {
                float d = c.genome.g[G_DIET];
                (d < 0.33f ? herb : d < 0.66f ? omni : carn)++;
                size += c.ph.adultRadius;
                speed += c.genome.g[G_SPEED];
                diet += d;
                toxin += c.genome.g[G_TOXIN];
                vision += c.ph.visionRange;
                conn += c.ph.connections;
                gen += c.generation;
            }
            double n = std::max<size_t>(1, world.creatures.size());
            std::printf("%9lld %6zu %5d %5d %5d %4d %5.0f %6.2f %6.2f %6.2f %6.2f %6.0f %5.0f %7.0f\n",
                        (long long)world.tickCount, world.creatures.size(), herb, omni, carn,
                        world.species.aliveCount(), gen / n, size / n, speed / n, diet / n, toxin / n, vision / n,
                        conn / n, tps);
            if (!opt.bench) {
                for (; eventCursor < world.events.items.size(); ++eventCursor) {
                    const WorldEvent& e = world.events.items[eventCursor];
                    if (e.kind != EventKind::Speciation || eventCursor % 4 == 0)
                        std::printf("          * %s\n", e.text.c_str());
                }
            }
            eventCursor = world.events.items.size();
        }
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Ran %lld ticks in %.2fs (%.0f ticks/s). Species ever: %zu, alive: %d. Births %llu, deaths %llu\n",
                (long long)opt.ticks, total, double(opt.ticks) / total, world.species.all().size(),
                world.species.aliveCount(), (unsigned long long)world.stats.totalBirths,
                (unsigned long long)world.stats.totalDeaths);
    int discoveries = 0, flash = 0;
    for (const Species& sp : world.species.all()) flash += sp.peakPopulation < 5;
    for (const WorldEvent& e : world.events.items) discoveries += e.kind == EventKind::Discovery;
    std::printf("RESULT seed=%llu ticks=%lld secs=%.2f ticks_per_s=%.0f mcreature_ticks_per_s=%.3f pop=%zu species_alive=%d species_ever=%zu discoveries=%d flash_species=%d\n",
                (unsigned long long)world.settings.seed, (long long)opt.ticks, total, double(opt.ticks) / total,
                creatureTicks / total / 1e6, world.creatures.size(), world.species.aliveCount(), world.species.all().size(),
                discoveries, flash);
    std::printf("Deaths by cause:");
    for (int k = 1; k < int(world.stats.causeTotals.size()); ++k)
        std::printf(" %s=%llu", deathCauseName(DeathCause(k)), (unsigned long long)world.stats.causeTotals[k]);
    std::printf("\n");
    {
        int outbreaks = 0;
        for (const WorldEvent& e : world.events.items) outbreaks += e.text.find("disease emerges") != std::string::npos;
        const double v = std::max(1, vitalSamples);
        double wet = 0, aq = 0, marine = 0, body[3][2] = {};
        for (const Creature& c : world.creatures) {
            wet += (c.flags & CF_IN_WATER) != 0;
            aq += c.genome.g[G_AQUATIC];
            marine += c.genome.g[G_AQUATIC] > 0.7f;
            for (int k = 0; k < 3; ++k) {
                body[k][0] += c.genome.g[G_WINGS + k];
                body[k][1] += c.genome.g[G_WINGS + k] > 0.5f;
            }
        }
        const double pop = std::max<size_t>(1, world.creatures.size());
        std::printf("SEA in_water=%.3f aquatic_gene_mean=%.3f sea_animals=%.3f\n", wet / pop, aq / pop, marine / pop);
        std::printf("BODY wings_mean=%.3f fliers=%.3f quills_mean=%.3f quilled=%.3f photo_mean=%.3f sunbathers=%.3f\n", body[0][0] / pop,
                    body[0][1] / pop, body[1][0] / pop, body[1][1] / pop, body[2][0] / pop, body[2][1] / pop);
        std::printf("VITALS outbreaks=%d infected_mean=%.4f infected_peak=%.3f hydration_mean=%.3f thirsty_share=%.4f\n", outbreaks,
                    infectedSum / v, infectedPeak, hydSum / v, parchedSum / v);
    }
    std::printf("Timings per tick (ms): grid %.3f sense %.3f interact %.3f move %.3f env %.3f book %.3f total %.3f\n",
                world.timings.grid, world.timings.sense, world.timings.interact, world.timings.move, world.timings.env,
                world.timings.book, world.timings.total);
    {
        double plast = 0, callA = 0, callB = 0, tend = 0, learned = 0;
        for (const Creature& c : world.creatures) {
            plast += c.genome.g[G_PLASTICITY];
            callA += c.call[0] > 0.3f;
            callB += c.call[1] > 0.3f;
            tend += c.tend > 0.3f;
            for (float l : c.learn) learned += std::fabs(l);
        }
        const double n = std::max<size_t>(1, world.creatures.size());
        std::printf("Behaviour now: plasticity gene %.2f, mean learned offset %.3f, calling A %.1f%% B %.1f%%, tending %.1f%%\n",
                    plast / n, learned / n / N_LEARN, 100 * callA / n, 100 * callB / n, 100 * tend / n);
        std::printf("Species profiles (15+ alive):\n");
        for (const Species& sp : world.species.all())
            if (sp.alive() && sp.population >= 15)
                std::printf("  %-28s %4d alive  best at: %s\n", sp.name().c_str(), sp.population, world.bestAtText(sp.id).c_str());
        std::printf("Discoveries:\n");
        for (const WorldEvent& e : world.events.items)
            if (e.kind == EventKind::Discovery)
                std::printf("  year %5.2f  %s\n", double(e.tick) / world.params.yearLength, e.text.c_str());
    }
    {
        std::printf("Lineages (founding lineage and all its descendants):\n");
        const double total = std::max<size_t>(1, world.creatures.size());
        int rank = 0, counted = 0;
        for (const LineageScore& ls : world.lineageScores()) counted += ls.population;
        // Self-check: every living creature belongs to exactly one founding lineage.
        if (counted != int(world.creatures.size()))
            std::fprintf(stderr, "BUG: lineages account for %d creatures, world has %zu\n", counted, world.creatures.size());
        for (const LineageScore& ls : world.lineageScores()) {
            if (!ls.population && !ls.designed) continue;
            std::printf("  %2d. %-28s %5d alive (%4.1f%%)  %2d living species, %3d ever%s\n", ++rank,
                        world.species.get(ls.root).name().c_str(), ls.population, 100.0 * ls.population / total,
                        ls.livingSpecies, ls.speciesEver, ls.designed ? "  [designed]" : "");
        }
    }
    if (opt.scenario >= 0 && opt.scenario < scenarioCount() && opt.load.empty()) {
        const float p = objective(opt.scenario).progress(world);
        std::printf("OBJECTIVE %s: %s (%.0f%%)\n", objective(opt.scenario).goal,
                    p < 0.f ? "failed" : p >= 1.f ? "achieved" : "in progress", 100.f * std::max(0.f, std::min(1.f, p)));
    }
    if (!opt.newick.empty()) {
        if (std::FILE* f = std::fopen(opt.newick.c_str(), "w")) {
            std::fputs(world.newick().c_str(), f);
            std::fclose(f);
            std::printf("Tree of life written to %s\n", opt.newick.c_str());
        }
    }
    if (!opt.save.empty()) {
        std::string err;
        if (!world.save(opt.save, err)) std::fprintf(stderr, "Save failed: %s\n", err.c_str());
        else std::printf("Saved to %s\n", opt.save.c_str());
    }
    return 0;
}

} // namespace pm
