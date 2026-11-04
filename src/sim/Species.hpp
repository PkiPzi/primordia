#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/Math.hpp"
#include "sim/Genome.hpp"

namespace pm {

struct Species {
    uint32_t id = 0;
    uint32_t parentId = 0;       // 0 = founding lineage
    std::string genus, epithet;
    int genusDepth = 0;          // speciation events since this genus was named
    int depth = 0;               // depth in the tree of life
    int64_t bornTick = 0;
    int64_t extinctTick = -1;
    uint32_t founderId = 0;
    Genome rep;                  // representative genome (re-centred periodically)
    int population = 0;
    int peakPopulation = 0;
    uint64_t totalBorn = 0;
    std::array<float, G_COUNT> meanGenes{};
    Vec2 centroid;
    float meanGeneration = 0.f;
    Color color;
    std::vector<uint32_t> children;
    std::vector<uint16_t> history; // population every HISTORY_INTERVAL ticks
    std::vector<Vec2> track;       // range centre at the same samples (migration path)
    int64_t historyStart = -1;
    bool announcedPredator = false;

    bool alive() const { return extinctTick < 0; }
    std::string name() const { return genus + " " + epithet; }
};

class SpeciesTracker {
public:
    static constexpr int HISTORY_INTERVAL = 250;

    void clear(uint64_t seed);
    uint32_t createRoot(const Genome& g, int64_t tick, uint32_t founderId);
    // Returns the species for a newborn; creates a new one when it has diverged.
    // A new species needs MIN_FOUNDERS births clustered in the same new genetic region; lone outliers stay with their
    // parent species (stops a stream of short-lived 'flash' species from hypermutating lineages).
    uint32_t assign(const Genome& child, uint32_t parentSpecies, int64_t tick, uint32_t childId, bool& created);
    static constexpr int MIN_FOUNDERS = 6;
    void onBirth(uint32_t id);
    bool onDeath(uint32_t id, int64_t tick); // true when the species just went extinct
    void recordHistory(int64_t tick);

    Species& get(uint32_t id) { return all_[id - 1]; }
    const Species& get(uint32_t id) const { return all_[id - 1]; }
    bool valid(uint32_t id) const { return id >= 1 && id <= all_.size(); }
    const std::vector<Species>& all() const { return all_; }
    std::vector<Species>& all() { return all_; }
    int aliveCount() const { return aliveCount_; }

    float threshold = 0.8f;

    // Persistence helpers
    void setNameUsed(const std::string& n) { usedNames_.insert(n); }
    void setAliveCount(int n) { aliveCount_ = n; }

private:
    struct Candidate { Genome rep; int count; int64_t lastTick; };
    std::unordered_map<uint32_t, std::vector<Candidate>> candidates_; // per parent species (transient, not saved)
    uint32_t create(const Genome& g, uint32_t parent, int64_t tick, uint32_t founderId);
    std::string makeGenus();
    std::string makeEpithet(const Genome& g, const Genome* parent);

    std::vector<Species> all_;
    std::unordered_set<std::string> usedNames_;
    Rng nameRng_;
    int aliveCount_ = 0;
};

} // namespace pm
