// Binary save format for a whole world. Bump FORMAT_VERSION whenever any
// trivially-copied struct (Creature, Genome, StatSample...) changes layout.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <type_traits>

#include "sim/World.hpp"

namespace pm {

namespace {

constexpr char MAGIC[8] = {'P', 'R', 'I', 'M', 'O', 'R', 'D', 'I'};
constexpr uint32_t FORMAT_VERSION = 23;

static_assert(std::is_trivially_copyable_v<Creature>);
static_assert(std::is_trivially_copyable_v<Corpse>);
static_assert(std::is_trivially_copyable_v<StatSample>);
static_assert(std::is_trivially_copyable_v<Genome>);

struct Writer {
    std::ostream& f;
    explicit Writer(std::ostream& o) : f(o) {}
    template <class T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        f.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    template <class T>
    void vec(const std::vector<T>& v) {
        uint64_t n = v.size();
        pod(n);
        if (n) f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(n * sizeof(T)));
    }
    void str(const std::string& s) {
        uint32_t n = uint32_t(s.size());
        pod(n);
        f.write(s.data(), n);
    }
};

struct Reader {
    std::istream& f;
    explicit Reader(std::istream& i) : f(i) {}
    bool ok = true;
    template <class T>
    void pod(T& v) {
        if (!f.read(reinterpret_cast<char*>(&v), sizeof(T))) ok = false;
    }
    template <class T>
    void vec(std::vector<T>& v, uint64_t limit = 1ull << 32) {
        uint64_t n = 0;
        pod(n);
        if (!ok || n > limit) { ok = false; return; }
        v.resize(n);
        if (n && !f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n * sizeof(T)))) ok = false;
    }
    void str(std::string& s) {
        uint32_t n = 0;
        pod(n);
        if (!ok || n > (1u << 20)) { ok = false; return; }
        s.resize(n);
        if (n && !f.read(s.data(), n)) ok = false;
    }
};

} // namespace

bool World::save(const std::string& path, std::string& err) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "Could not open " + path + " for writing";
        return false;
    }
    return saveStream(out, err);
}

std::string World::snapshot() const {
    std::ostringstream out(std::ios::binary);
    std::string err;
    saveStream(out, err);
    return std::move(out).str();
}

bool World::restore(const std::string& data, std::string& err) {
    std::istringstream in(data, std::ios::binary);
    return loadStream(in, err);
}

bool World::saveStream(std::ostream& out, std::string& err) const {
    Writer w(out);
    w.f.write(MAGIC, 8);
    w.pod(FORMAT_VERSION);
    uint32_t sizes[4] = {uint32_t(sizeof(Creature)), uint32_t(sizeof(Genome)), uint32_t(sizeof(StatSample)),
                         uint32_t(G_COUNT)};
    w.pod(sizes);
    w.pod(settings);
    w.pod(params);
    w.pod(tickCount);
    w.pod(maxGeneration);
    w.pod(rng_);
    w.pod(nextId_);
    w.pod(climateEvent_);
    w.pod(nextGenMilestone_);

    const Terrain& t = terrain;
    w.pod(t.cols);
    w.pod(t.rows);
    w.pod(t.cellSize);
    w.pod(t.width);
    w.pod(t.height);
    w.pod(t.seaLevel);
    w.pod(t.baseSeaLevel);
    w.vec(t.elevation);
    w.vec(t.moisture);
    w.vec(t.fertility);
    w.vec(t.climateBase);
    w.vec(t.baseTemp);
    w.vec(t.plant);
    w.vec(t.pheromone);
    w.vec(t.tilled);
    w.vec(t.tiller);
    w.vec(t.plantDef);
    w.vec(t.plantSpr);
    w.vec(t.water);
    w.vec(t.rainTile);

    w.vec(creatures);
    w.vec(corpses);

    const auto& all = species.all();
    uint64_t ns = all.size();
    w.pod(ns);
    for (const Species& s : all) {
        w.pod(s.id);
        w.pod(s.parentId);
        w.str(s.genus);
        w.str(s.epithet);
        w.pod(s.genusDepth);
        w.pod(s.depth);
        w.pod(s.bornTick);
        w.pod(s.extinctTick);
        w.pod(s.founderId);
        w.pod(s.rep);
        w.pod(s.population);
        w.pod(s.peakPopulation);
        w.pod(s.totalBorn);
        w.pod(s.meanGenes);
        w.pod(s.centroid);
        w.pod(s.meanGeneration);
        w.pod(s.color);
        w.vec(s.children);
        w.vec(s.history);
        w.vec(s.track);
        w.pod(s.historyStart);
        w.pod(s.announcedPredator);
    }
    w.pod(species.threshold);

    w.pod(stats.interval);
    w.vec(stats.samples);
    w.vec(stats.traitHist);
    w.pod(stats.birthsAcc);
    w.pod(stats.deathsAcc);
    w.pod(stats.totalBirths);
    w.pod(stats.totalDeaths);
    w.pod(stats.totalHybrids);
    w.pod(stats.causeTotals);

    const_cast<Records&>(records).forEach([&](const char*, const char*, Record& r) {
        w.pod(r.value);
        w.str(r.holder);
        w.pod(r.speciesId);
        w.pod(r.tick);
    });
    w.pod(achieved);
    w.pod(achievedTick);
    uint64_t ne = events.items.size();
    w.pod(ne);
    for (const WorldEvent& e : events.items) {
        w.pod(e.tick);
        w.pod(e.kind);
        w.pod(e.speciesId);
        w.pod(e.pos);
        w.str(e.text);
        w.pod(e.tag);
    }
    w.f.flush();
    if (!w.f) {
        err = "Write failed (disk full?)";
        return false;
    }
    return true;
}

bool World::load(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Could not open " + path;
        return false;
    }
    return loadStream(in, err);
}

bool World::loadStream(std::istream& in, std::string& err) {
    Reader r(in);
    char magic[8];
    r.f.read(magic, 8);
    if (!r.f || std::memcmp(magic, MAGIC, 8) != 0) {
        err = "Not a Primordia save file";
        return false;
    }
    uint32_t version = 0;
    r.pod(version);
    uint32_t sizes[4] = {};
    r.pod(sizes);
    if (version != FORMAT_VERSION || sizes[0] != sizeof(Creature) || sizes[1] != sizeof(Genome) ||
        sizes[2] != sizeof(StatSample) || sizes[3] != uint32_t(G_COUNT)) {
        err = "This save was made by a different version of Primordia";
        return false;
    }
    World tmp(pool_);
    r.pod(tmp.settings);
    r.pod(tmp.params);
    r.pod(tmp.tickCount);
    r.pod(tmp.maxGeneration);
    r.pod(tmp.rng_);
    r.pod(tmp.nextId_);
    r.pod(tmp.climateEvent_);
    r.pod(tmp.nextGenMilestone_);

    Terrain& t = tmp.terrain;
    r.pod(t.cols);
    r.pod(t.rows);
    r.pod(t.cellSize);
    r.pod(t.width);
    r.pod(t.height);
    r.pod(t.seaLevel);
    r.pod(t.baseSeaLevel);
    r.vec(t.elevation);
    r.vec(t.moisture);
    r.vec(t.fertility);
    r.vec(t.climateBase);
    r.vec(t.baseTemp);
    r.vec(t.plant);
    r.vec(t.pheromone);
    r.vec(t.tilled);
    r.vec(t.tiller);
    r.vec(t.plantDef);
    r.vec(t.plantSpr);
    r.vec(t.water);
    r.vec(t.rainTile);
    if (r.ok && (t.tilled.size() != t.plant.size() || t.tiller.size() != t.plant.size() ||
                      t.plantDef.size() != t.plant.size() || t.plantSpr.size() != t.plant.size())) {
        err = "Save file is corrupt";
        return false;
    }
    t.version = terrain.version + 1;

    r.vec(tmp.creatures, 1u << 22);
    r.vec(tmp.corpses, 1u << 22);

    uint64_t ns = 0;
    r.pod(ns);
    if (!r.ok || ns > (1u << 24)) {
        err = "Save file is corrupt";
        return false;
    }
    tmp.species.clear(tmp.settings.seed ^ uint64_t(tmp.tickCount));
    auto& all = tmp.species.all();
    all.resize(ns);
    int alive = 0;
    for (Species& s : all) {
        r.pod(s.id);
        r.pod(s.parentId);
        r.str(s.genus);
        r.str(s.epithet);
        r.pod(s.genusDepth);
        r.pod(s.depth);
        r.pod(s.bornTick);
        r.pod(s.extinctTick);
        r.pod(s.founderId);
        r.pod(s.rep);
        r.pod(s.population);
        r.pod(s.peakPopulation);
        r.pod(s.totalBorn);
        r.pod(s.meanGenes);
        r.pod(s.centroid);
        r.pod(s.meanGeneration);
        r.pod(s.color);
        r.vec(s.children);
        r.vec(s.history);
        r.vec(s.track);
        r.pod(s.historyStart);
        r.pod(s.announcedPredator);
        if (!r.ok) break;
        tmp.species.setNameUsed(s.name());
        if (s.alive()) ++alive;
    }
    tmp.species.setAliveCount(alive);
    r.pod(tmp.species.threshold);

    r.pod(tmp.stats.interval);
    r.vec(tmp.stats.samples);
    r.vec(tmp.stats.traitHist);
    r.pod(tmp.stats.birthsAcc);
    r.pod(tmp.stats.deathsAcc);
    r.pod(tmp.stats.totalBirths);
    r.pod(tmp.stats.totalDeaths);
    r.pod(tmp.stats.totalHybrids);
    r.pod(tmp.stats.causeTotals);

    tmp.records.forEach([&](const char*, const char*, Record& rec) {
        r.pod(rec.value);
        r.str(rec.holder);
        r.pod(rec.speciesId);
        r.pod(rec.tick);
    });
    r.pod(tmp.achieved);
    r.pod(tmp.achievedTick);
    uint64_t ne = 0;
    r.pod(ne);
    for (uint64_t k = 0; k < ne && r.ok && k < 100000; ++k) {
        WorldEvent e;
        r.pod(e.tick);
        r.pod(e.kind);
        r.pod(e.speciesId);
        r.pod(e.pos);
        r.str(e.text);
        r.pod(e.tag);
        tmp.events.items.push_back(std::move(e));
    }
    tmp.events.counter = tmp.events.items.size();
    if (!r.ok) {
        err = "Save file is truncated or corrupt";
        return false;
    }

    // Commit.
    settings = tmp.settings;
    params = tmp.params;
    tickCount = tmp.tickCount;
    maxGeneration = tmp.maxGeneration;
    rng_ = tmp.rng_;
    nextId_ = tmp.nextId_;
    climateEvent_ = tmp.climateEvent_;
    nextGenMilestone_ = tmp.nextGenMilestone_;
    terrain = std::move(tmp.terrain);
    creatures = std::move(tmp.creatures);
    creatures.reserve(size_t(params.maxPopulation) + 4096);
    corpses = std::move(tmp.corpses);
    species = std::move(tmp.species);
    stats = std::move(tmp.stats);
    events = std::move(tmp.events);
    records = std::move(tmp.records);
    achieved = tmp.achieved;
    std::copy(std::begin(tmp.achievedTick), std::end(tmp.achievedTick), std::begin(achievedTick));
    newborns_.clear();
    behav_.clear();
    prevGenes_.clear();
    lastGeneYear_ = -1;
    recentPeak_ = 0.f;
    announced_.clear();
    streaks_.clear();
    // Same key discover() uses, so a reload or rewind does not announce the same behaviour twice. The bucket of
    // yearly entries is the year they were logged in.
    for (const WorldEvent& e : events.items)
        if (e.kind == EventKind::Discovery) {
            announced_.insert(discoveryKey(e.tag, e.speciesId, 0));
            announced_.insert(discoveryKey(e.tag, e.speciesId, uint32_t(e.tick / std::max(1, params.yearLength)) + 1));
        }
    fx.clear();
    lastSpecialization_ = params.specialization;
    cgrid_.resize(terrain.width, terrain.height, 64.f);
    mgrid_.resize(terrain.width, terrain.height, 64.f);
    return true;
}

} // namespace pm
