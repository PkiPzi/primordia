#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/Math.hpp"
#include "sim/Genome.hpp"

namespace pm {

// One row of the world's history. All floats so ImPlot can stride over it.
struct StatSample {
    float tick;
    float population, herbivores, omnivores, carnivores;
    float species;
    float plants, meat;
    float births, deaths;
    float meanGeneration, maxGeneration;
    float connections;
    float meanEnergy;
    float climate;           // global temperature offset (incl. events)
    float toxic;             // share of creatures carrying toxins
    float infected, immune;  // epidemic state (SIR)
    float strainAggression;  // mean aggressiveness of circulating pathogen strains
    float sharing;           // share of creatures giving food away this tick
    float attackRate;        // attacks landed per creature per 100 ticks
    float cannibalism;       // fraction of attacks aimed at the attacker's own species
    float signalling;        // share of creatures glowing a signal
    float marking;           // share of creatures laying pheromone
    float speed;             // mean speed
    float toxicTargeted;     // share of attacks that hit toxic prey (compare with `toxic`)
    float packHunting;       // share of bites that were part of a group attack
    float camouflaged;       // share of creatures blending into their background
    float contests;          // food contests per 100 ticks
    float hawkShare;         // share of contestants that acted aggressively (hawks)
    float doveDove, hawkDove, hawkHawk; // shares of each contest type
    float geneMean[G_COUNT];
    float geneStd[G_COUNT];
};

constexpr int TRAIT_BINS = 32;
using TraitHistogram = std::array<std::array<uint16_t, TRAIT_BINS>, G_COUNT>;

struct Stats {
    int interval = 100;               // ticks between samples (doubles when history is compacted)
    static constexpr size_t MAX_SAMPLES = 6000;
    std::vector<StatSample> samples;
    std::vector<TraitHistogram> traitHist; // parallel to samples
    int birthsAcc = 0, deathsAcc = 0;
    int attacksIntra = 0, attacksInter = 0, attacksToxic = 0, attacksPack = 0;
    int contestDD = 0, contestHD = 0, contestHH = 0;
    uint64_t totalBirths = 0, totalDeaths = 0, totalHybrids = 0;
    std::array<uint64_t, 9> causeTotals{}; // indexed by DeathCause

    void clear() {
        interval = 100;
        samples.clear();
        traitHist.clear();
        birthsAcc = deathsAcc = 0;
        totalBirths = totalDeaths = 0;
        causeTotals.fill(0);
    }
    void push(const StatSample& s, const TraitHistogram& h) {
        samples.push_back(s);
        traitHist.push_back(h);
        if (samples.size() > MAX_SAMPLES) {
            size_t w = 0;
            for (size_t r = 0; r < samples.size(); r += 2, ++w) {
                samples[w] = samples[r];
                traitHist[w] = traitHist[r];
            }
            samples.resize(w);
            traitHist.resize(w);
            interval *= 2;
        }
    }
};

struct Record {
    float value = 0.f;
    std::string holder; // "Genus species #id"
    uint32_t speciesId = 0;
    int64_t tick = 0;
};

// Hall of fame. Values are compared with >, so "smallest" style records store negatives.
struct Records {
    Record oldest, mostChildren, mostKills, mostGenerous, largest, longestSpecies, biggestSpecies, mostSynapses;
    template <class F>
    void forEach(F&& f) {
        f("Oldest creature", "years", oldest);
        f("Most offspring", "children", mostChildren);
        f("Deadliest hunter", "kills", mostKills);
        f("Most generous", "energy shared", mostGenerous);
        f("Largest body", "radius", largest);
        f("Most complex brain", "synapses", mostSynapses);
        f("Longest-lived species", "years", longestSpecies);
        f("Most populous species", "individuals", biggestSpecies);
    }
};

enum class EventKind : uint8_t { Info, Speciation, Extinction, Milestone, Catastrophe, Discovery };

struct WorldEvent {
    int64_t tick = 0;
    EventKind kind = EventKind::Info;
    uint32_t speciesId = 0;
    Vec2 pos;
    std::string text;
    uint8_t tag = 0; // for Discovery events: which behaviour (so it is announced once per species)
};

struct EventLog {
    std::deque<WorldEvent> items;
    uint64_t counter = 0; // total ever pushed; UI uses it to detect new events
    void clear() { items.clear(); counter = 0; }
    void push(WorldEvent e) {
        items.push_back(std::move(e));
        ++counter;
        if (items.size() > 4000) items.pop_front();
    }
};

} // namespace pm
