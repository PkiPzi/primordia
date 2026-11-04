#include "sim/World.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <numeric>

#include "core/Noise.hpp"
#include "core/ThreadPool.hpp"

namespace pm {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

constexpr float SAMPLE_D[4] = {0.15f, 0.4f, 0.65f, 0.9f};
constexpr float SAMPLE_W[4] = {0.4f, 0.3f, 0.2f, 0.1f};

// Recurrent brain step. Input-major weights: each active input adds a contiguous
// column into the hidden/output accumulators; silent inputs (most eye channels) are skipped.
inline void think(const float* __restrict w, const float* __restrict in, const float* __restrict learn,
                  float* __restrict hid, float* __restrict out) {
    alignas(32) float h[N_HIDDEN];
    alignas(32) float o[N_OUTPUTS];
    for (int k = 0; k < N_HIDDEN; ++k) h[k] = w[OFF_BH + k];
    for (int q = 0; q < N_OUTPUTS; ++q) o[q] = w[OFF_BO + q];
    for (int i = 0; i < N_INPUTS; ++i) {
        const float x = in[i];
        if (x == 0.f) continue;
        const float* ch = w + OFF_IH + i * N_HIDDEN;
#pragma omp simd
        for (int k = 0; k < N_HIDDEN; ++k) h[k] += ch[k] * x;
        const float* co = w + OFF_IO + i * N_OUTPUTS;
#pragma omp simd
        for (int q = 0; q < N_OUTPUTS; ++q) o[q] += co[q] * x;
    }
    for (int j = 0; j < N_HIDDEN; ++j) {
        const float x = hid[j];
        const float* ch = w + OFF_HH + j * N_HIDDEN;
#pragma omp simd
        for (int k = 0; k < N_HIDDEN; ++k) h[k] += ch[k] * x;
    }
    for (int k = 0; k < N_HIDDEN; ++k) h[k] = fastTanh(h[k]);
    for (int j = 0; j < N_HIDDEN; ++j) {
        const float x = h[j];
        const float* co = w + OFF_HO + j * N_OUTPUTS;
        const float* cl = learn + j * N_OUTPUTS;
#pragma omp simd
        for (int q = 0; q < N_OUTPUTS; ++q) o[q] += (co[q] + cl[q]) * x;
    }
    for (int q = 0; q < N_OUTPUTS; ++q) out[q] = fastTanh(o[q]);
    for (int k = 0; k < N_HIDDEN; ++k) hid[k] = h[k];
}

float sq(float v) { return v * v; }

} // namespace

namespace {
const AchievementInfo ACHIEVEMENTS[] = {
    {"First blood", "A creature is killed by a predator"},
    {"Origin of species", "A lineage diverges into a new species"},
    {"Diversity", "10 species alive at the same time"},
    {"Adaptive radiation", "25 species alive at the same time"},
    {"Deep time", "Reach generation 100"},
    {"Pack hunters", "A fifth of all bites come from group attacks"},
    {"Warning colours", "A quarter of all creatures carry potent toxins"},
    {"Night owls", "Average night vision passes 0.5"},
    {"Altruists", "3% of creatures are feeding neighbours at once"},
    {"Herd immunity", "Over 40% of creatures have recovered from disease"},
    {"Bounce back", "Population recovers above 1000 after falling below 200"},
    {"Giants", "A creature reaches radius 18"},
    {"Trail blazers", "15% of creatures are laying pheromone trails"},
    {"Glow in the dark", "After year 2, 60% of creatures glow with signals"},
    {"Picky lovers", "Average choosiness about mates passes 0.5"},
    {"Mass extinction", "5 established species go extinct within one year"},
    {"Big brains", "Average brain passes 200 synapses"},
    {"Old world", "The world reaches year 25"},
    {"Masters of disguise", "Over a third of creatures blend into their background"},
};
} // namespace

int achievementCount() { return int(std::size(ACHIEVEMENTS)); }
const AchievementInfo& achievementInfo(int i) { return ACHIEVEMENTS[i]; }

void World::unlock(int id) {
    if (achieved & (1ull << id)) return;
    achieved |= 1ull << id;
    achievedTick[id] = tickCount;
    log(EventKind::Milestone, 0, {}, std::string("Achievement: ") + ACHIEVEMENTS[id].name + " (" + ACHIEVEMENTS[id].description + ")");
}

void World::checkAchievements(const StatSample& s) {
    if (s.population > 0) {
        if (s.species >= 10) unlock(2);
        if (s.species >= 25) unlock(3);
        if (s.maxGeneration >= 100) unlock(4);
        if (s.packHunting > 0.2f && s.attackRate > 0.05f) unlock(5);
        if (s.toxic > 0.25f) unlock(6);
        if (s.geneMean[G_NIGHT_VISION] > 0.5f) unlock(7);
        if (s.sharing > 0.03f) unlock(8);
        if (s.immune > 0.4f * s.population) unlock(9);
        if (s.marking > 0.15f) unlock(12);
        if (s.signalling > 0.6f && tickCount > 2 * int64_t(params.yearLength)) unlock(13);
        if (s.geneMean[G_CHOOSY] > 0.5f) unlock(14);
        if (s.connections > 200.f) unlock(16);
        if (s.camouflaged > 0.34f && tickCount > int64_t(params.yearLength)) unlock(18);
    }
    lowestPopulation_ = std::min(lowestPopulation_, int(s.population));
    if (lowestPopulation_ < 200 && s.population > 1000) unlock(10);
    for (const Creature& c : creatures)
        if (c.radius >= 18.f) { unlock(11); break; }
    if (tickCount >= int64_t(params.yearLength) * 25) unlock(17);
    if (stats.causeTotals[size_t(DeathCause::Predation)] > 0) unlock(0);
    if (species.all().size() > size_t(settings.foundingLineages + settings.predatorLineages) + 2) unlock(1);
}

const char* deathCauseName(DeathCause c) {
    switch (c) {
        case DeathCause::Starvation: return "Starvation";
        case DeathCause::OldAge: return "Old age";
        case DeathCause::Predation: return "Predation";
        case DeathCause::Poison: return "Poisoned";
        case DeathCause::Meteor: return "Meteor";
        case DeathCause::Smite: return "Smitten";
        case DeathCause::Plague: return "Disease";
        case DeathCause::Thirst: return "Thirst";
        default: return "Alive";
    }
}

// ---------------------------------------------------------------------------
// Terrain
// ---------------------------------------------------------------------------
void Terrain::generate(const WorldSettings& ws) {
    cellSize = ws.cellSize;
    width = ws.width;
    height = ws.height;
    cols = int(std::ceil(width / cellSize));
    rows = int(std::ceil(height / cellSize));
    const size_t n = size_t(cols) * rows;
    elevation.assign(n, 0.f);
    moisture.assign(n, 0.f);
    fertility.assign(n, 0.f);
    climateBase.assign(n, 0.f);
    baseTemp.assign(n, 0.f);
    plant.assign(n, 0.f);
    pheromone.assign(n, 0.f);
    tilled.assign(n, 0.f);
    tiller.assign(n, 0u);
    water.assign(n, 0);

    Noise2D ne(ws.seed * 31 + 1), nm(ws.seed * 31 + 2), nt(ws.seed * 31 + 3), nw(ws.seed * 31 + 4);
    float freq = 1.f / (1400.f * std::max(0.2f, ws.featureScale));
    float edgeFalloff = 0.35f, waterFraction = ws.waterFraction;
    bool radial = false;
    switch (ws.preset) {
        case TerrainPreset::Continents: break;
        case TerrainPreset::Archipelago: freq *= 2.3f; edgeFalloff = 0.3f; waterFraction = std::max(waterFraction, 0.55f); break;
        case TerrainPreset::Pangaea: freq *= 0.75f; edgeFalloff = 0.9f; radial = true; break;
        case TerrainPreset::Lakes: freq *= 1.7f; edgeFalloff = 0.f; waterFraction = std::min(waterFraction, 0.2f); break;
        case TerrainPreset::Flatland: edgeFalloff = 0.f; waterFraction = 0.f; break;
    }

    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            const int i = cy * cols + cx;
            float x = (cx + 0.5f) * cellSize, y = (cy + 0.5f) * cellSize;
            float wx = nw.fbm(x * freq * 0.5f, y * freq * 0.5f, 3);
            float wy = nw.fbm(x * freq * 0.5f + 31.7f, y * freq * 0.5f + 11.3f, 3);
            float qx = x * freq + 0.55f * wx, qy = y * freq + 0.55f * wy;
            float e = ne.fbm(qx, qy, 6) * 0.5f + 0.5f;
            e += 0.08f * (ne.ridged(qx * 1.3f + 5.f, qy * 1.3f + 9.f, 4) - 0.5f);
            float u = x / width * 2.f - 1.f, v = y / height * 2.f - 1.f;
            float edge = radial ? std::sqrt(u * u + v * v) * 0.85f : std::max(std::fabs(u), std::fabs(v));
            e -= edgeFalloff * smoothstep(0.55f, 1.0f, edge);
            elevation[i] = e;
            moisture[i] = clamp01(nm.fbm(x * freq * 1.4f, y * freq * 1.4f, 5) * 0.6f + 0.5f + ws.moisture);
            float lat = latitude(cy);
            climateBase[i] = 0.85f - 1.55f * std::pow(std::fabs(lat), 1.25f) +
                             0.12f * nt.fbm(x * freq * 2.f, y * freq * 2.f, 3) + ws.temperature;
        }
    }
    // Normalise elevation to [0,1].
    auto [mnIt, mxIt] = std::minmax_element(elevation.begin(), elevation.end());
    float mn = *mnIt, range = std::max(1e-5f, *mxIt - mn);
    for (float& e : elevation) e = (e - mn) / range;
    // Sea level from the requested water fraction.
    if (waterFraction <= 0.001f) {
        seaLevel = -1.f;
    } else {
        std::vector<float> tmp = elevation;
        size_t k = std::min(n - 1, size_t(double(n) * waterFraction));
        std::nth_element(tmp.begin(), tmp.begin() + k, tmp.end());
        seaLevel = tmp[k];
    }
    baseSeaLevel = seaLevel;
    for (size_t i = 0; i < n; ++i) refreshCell(int(i));
    // Tileable rain pattern: periodic value noise, two octaves, thresholded into cloud banks.
    {
        Rng rr(ws.seed * 7 + 11);
        const int T = RAIN_TILE;
        auto lattice = [&](int L) {
            std::vector<float> v(size_t(L) * L);
            for (float& x : v) x = rr.uniform();
            return v;
        };
        auto sample = [](const std::vector<float>& v, int L, float x, float y) {
            int x0 = int(std::floor(x)), y0 = int(std::floor(y));
            float fx = x - float(x0), fy = y - float(y0);
            fx = fx * fx * (3 - 2 * fx);
            fy = fy * fy * (3 - 2 * fy);
            auto at = [&](int i, int j) { return v[size_t(((j % L) + L) % L) * L + size_t(((i % L) + L) % L)]; };
            return lerp(lerp(at(x0, y0), at(x0 + 1, y0), fx), lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
        };
        auto a = lattice(6), b = lattice(12);
        rainTile.assign(size_t(T) * T, 0.f);
        for (int y = 0; y < T; ++y)
            for (int x = 0; x < T; ++x) {
                float u = float(x) / T, v = float(y) / T;
                float val = 0.65f * sample(a, 6, u * 6, v * 6) + 0.35f * sample(b, 12, u * 12, v * 12);
                rainTile[size_t(y) * T + x] = smoothstep(0.48f, 0.72f, val);
            }
    }
    ++version;
}

void Terrain::refreshCell(int i) {
    float e = elevation[i];
    water[i] = e < seaLevel ? 1 : 0;
    float sl = std::max(seaLevel, 0.f);
    float h = std::max(0.f, (e - sl) / std::max(1e-3f, 1.f - sl));
    baseTemp[i] = climateBase[i] - 0.65f * std::pow(h, 1.4f);
    if (water[i]) {
        fertility[i] = 0.f;
        return;
    }
    float f = clamp01(moisture[i] * 1.25f - 0.12f);
    f *= 1.f - smoothstep(0.45f, 0.92f, h);
    fertility[i] = clamp01(f);
}

void SpatialGrid::resize(float w, float h, float cellSize) {
    cell = cellSize;
    cols = std::max(1, int(std::ceil(w / cell)));
    rows = std::max(1, int(std::ceil(h / cell)));
    start.assign(size_t(cols) * rows + 1, 0);
}

// ---------------------------------------------------------------------------
// World setup
// ---------------------------------------------------------------------------
World::World(ThreadPool* pool) : pool_(pool) {}

void World::log(EventKind kind, uint32_t speciesId, Vec2 pos, std::string text) {
    events.push({tickCount, kind, speciesId, pos, std::move(text), 0});
}

void World::pushFx(FxEvent::Kind k, Vec2 p, Color c, float size) {
    if (!fxEnabled || fx.size() > 4096) return;
    fx.push_back({k, p, c, size});
}

Vec2 World::randomLand(Rng& r, float minFertility) const {
    for (int attempt = 0; attempt < 400; ++attempt) {
        Vec2 p{r.range(20.f, terrain.width - 20.f), r.range(20.f, terrain.height - 20.f)};
        int i = terrain.index(p.x, p.y);
        if (!terrain.water[i] && terrain.fertility[i] >= minFertility) return p;
    }
    for (int attempt = 0; attempt < 400; ++attempt) {
        Vec2 p{r.range(20.f, terrain.width - 20.f), r.range(20.f, terrain.height - 20.f)};
        if (!terrain.water[terrain.index(p.x, p.y)]) return p;
    }
    return {terrain.width * 0.5f, terrain.height * 0.5f};
}

void World::updateBody(Creature& c) const {
    const Phenotype& p = c.ph;
    c.radius = p.adultRadius * (0.45f + 0.55f * c.growth);
    float rs = c.radius / 10.f;
    c.mass = rs * rs * (1.f + 0.6f * p.armor + 0.3f * p.weapon);
    c.mass075 = std::pow(c.mass, 0.75f);
    c.maxEnergy = 115.f * c.mass + 3.f;
    c.maxHealth = 50.f * c.mass * (1.f + 1.5f * p.armor) + 5.f;
}

Creature World::makeCreature(const Genome& g, Vec2 pos, uint32_t speciesId, uint32_t parentId, uint32_t generation,
                             float energy, float growth) {
    Creature c;
    c.id = nextId_++;
    c.parentId = parentId;
    c.speciesId = speciesId;
    c.generation = generation;
    c.genome = g;
    c.ph = Phenotype::from(g, params.specialization);
    c.pos = pos;
    c.angle = rng_.range(-PI, PI);
    c.growth = growth;
    c.rng = Rng(rng_.next64(), c.id);
    updateBody(c);
    c.energy = std::min(energy, c.maxEnergy);
    c.lastEnergy = c.energy;
    c.health = c.maxHealth;
    c.birthTick = tickCount;
    c.clockPhase = rng_.range(0.f, TAU);
    c.animPhase = rng_.range(0.f, TAU);
    return c;
}

Creature World::preview(const Genome& g) const {
    Creature c;
    c.genome = g;
    c.ph = Phenotype::from(g, params.specialization);
    c.growth = 1.f;
    updateBody(c);
    c.energy = c.maxEnergy;
    c.health = c.maxHealth;
    return c;
}

void World::refreshPhenotypes() {
    for (Creature& c : creatures) {
        c.ph = Phenotype::from(c.genome, params.specialization);
        updateBody(c);
    }
    lastSpecialization_ = params.specialization;
}

void World::create(const WorldSettings& ws) {
    settings = ws;
    rng_.reseed(ws.seed, 0x1234);
    terrain.generate(ws);
    for (size_t i = 0; i < terrain.plant.size(); ++i)
        terrain.plant[i] = terrain.fertility[i] * params.plantCapacity * rng_.range(0.5f, 1.f);

    creatures.clear();
    creatures.reserve(size_t(params.maxPopulation) + 4096);
    corpses.clear();
    newborns_.clear();
    fx.clear();
    species.clear(ws.seed);
    species.threshold = params.speciesThreshold;
    stats.clear();
    events.clear();
    records = Records{};
    achieved = 0;
    std::fill(std::begin(achievedTick), std::end(achievedTick), int64_t(0));
    lowestPopulation_ = 1 << 30;
    recentExtinctions_.clear();
    behav_.clear();
    announced_.clear();
    prevGenes_.clear();
    lastGeneYear_ = -1;
    recentPeak_ = 0.f;
    tickCount = 0;
    nextId_ = 1;
    maxGeneration = 0;
    climateEvent_ = 0.f;
    nextGenMilestone_ = 10;
    lastSpecialization_ = params.specialization;
    cgrid_.resize(terrain.width, terrain.height, 64.f);
    mgrid_.resize(terrain.width, terrain.height, 64.f);

    const int lineages = std::max(1, ws.foundingLineages);
    const int hunters = std::max(0, ws.predatorLineages);
    const int hunterPop = hunters ? int(float(ws.initialPopulation) * ws.predatorShare) : 0;
    const int grazerPop = ws.initialPopulation - hunterPop;
    for (int l = 0; l < lineages + hunters; ++l) {
        const bool isHunter = l >= lineages;
        Genome base = isHunter ? Genome::predator(rng_) : Genome::random(rng_, ws.initialDietMax);
        uint32_t sid = species.createRoot(base, 0, nextId_);
        int count = isHunter ? hunterPop / hunters : grazerPop / lineages + (l == 0 ? grazerPop % lineages : 0);
        Vec2 home = randomLand(rng_, 0.35f);
        for (int k = 0; k < count; ++k) {
            Genome g = base;
            mutate(g, rng_, 1.f);
            Vec2 p = home + Vec2(rng_.normal() * 450.f, rng_.normal() * 450.f);
            p.x = clampf(p.x, 20.f, terrain.width - 20.f);
            p.y = clampf(p.y, 20.f, terrain.height - 20.f);
            if (terrain.water[terrain.index(p.x, p.y)]) p = randomLand(rng_, 0.2f);
            Creature c = makeCreature(g, p, sid, 0, 0, 0.f, rng_.range(0.6f, 1.f));
            c.energy = c.maxEnergy * rng_.range(0.5f, 0.85f);
            c.age = rng_.range(0.f, c.ph.maturity);
            species.onBirth(sid);
            creatures.push_back(c);
        }
        const Species& s = species.get(sid);
        log(EventKind::Speciation, sid, home,
            std::string(isHunter ? "Predator lineage " : "Founding lineage ") + s.name() + " appears");
    }
    sampleStats();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
void World::tick() {
    auto t0 = Clock::now();
    species.threshold = params.speciesThreshold;
    if (params.specialization != lastSpecialization_) refreshPhenotypes();

    buildGrids();
    auto t1 = Clock::now();
    const int n = int(creatures.size());
    pool_->parallelFor(0, n, 32, [this](int b, int e, int) {
        for (int i = b; i < e; ++i) senseAndThink(i);
    });
    auto t2 = Clock::now();
    interact();
    auto t3 = Clock::now();
    pool_->parallelFor(0, n, 64, [this](int b, int e, int) {
        for (int i = b; i < e; ++i) moveAndMetabolize(i);
    });
    auto t4 = Clock::now();
    resolveDeathsAndBirths();
    updateEnvironment();
    auto t5 = Clock::now();

    ++tickCount;
    if (tickCount % 50 == 0) census();
    if (tickCount % 200 == 0) updateSeaLevel();
    if (tickCount % params.yearLength == 0) {
        // Chronicle: a one-line summary of the year that just ended.
        const Species* top = nullptr;
        int predatorSpecies = 0;
        for (const Species& s : species.all())
            if (s.alive()) {
                if (!top || s.population > top->population) top = &s;
                if (s.meanGenes[G_DIET] > 0.6f) ++predatorSpecies;
            }
        char buf[256];
        std::snprintf(buf, sizeof buf, "End of year %lld: %zu creatures in %d species (%d hunting). Dominant: %s (%d%%)",
                      (long long)(tickCount / params.yearLength), creatures.size(), species.aliveCount(), predatorSpecies,
                      top ? top->name().c_str() : "none",
                      top && !creatures.empty() ? int(100 * top->population / int(creatures.size())) : 0);
        log(EventKind::Info, top ? top->id : 0, top ? top->centroid : Vec2{}, buf);
    }
    if (tickCount % SpeciesTracker::HISTORY_INTERVAL == 0) species.recordHistory(tickCount);
    if (tickCount % stats.interval == 0) sampleStats();

    if (maxGeneration >= nextGenMilestone_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Generation %u reached", nextGenMilestone_);
        log(EventKind::Milestone, 0, {}, buf);
        nextGenMilestone_ = nextGenMilestone_ < 50 ? nextGenMilestone_ * 5 / 2 : nextGenMilestone_ * 2;
    }
    if (params.epidemics && rng_.chance(params.outbreaksPerYear / float(std::max(1, params.yearLength)))) {
        // Dense, populous species are the likeliest hosts for a new pathogen.
        uint32_t host = 0;
        int best = 40;
        for (const Species& s : species.all())
            if (s.alive() && s.population > best) { best = s.population; host = s.id; }
        if (host) {
            int seeded = 0;
            for (Creature& c : creatures)
                if (c.speciesId == host && c.infection <= 0.f && c.immuneTicks <= 0.f && rng_.chance(8.f / float(best))) {
                    c.infection = float(params.diseaseDuration);
                    c.strain = 0.5f;
                    ++seeded;
                }
            if (seeded)
                log(EventKind::Catastrophe, host, species.get(host).centroid,
                    "A new disease emerges among " + species.get(host).name());
        }
    }
    if (params.predatorImmigration && tickCount % 500 == 0) {
        int hunters = 0;
        for (const Creature& c : creatures) hunters += c.ph.diet > 0.6f;
        if (hunters >= 3) noPredatorsSince_ = -1;
        else if (noPredatorsSince_ < 0) noPredatorsSince_ = tickCount;
        else if (tickCount - noPredatorsSince_ > params.yearLength / 2 && creatures.size() > 200) {
            Vec2 at = randomLand(rng_, 0.4f);
            int k = spawnPredators(at, 30, 350.f);
            log(EventKind::Catastrophe, 0, at, std::to_string(k) + " hunters migrate in from beyond the map");
            noPredatorsSince_ = -1;
        }
    }
    if (params.autoReseed && int(creatures.size()) < params.reseedBelow) {
        Vec2 at = randomLand(rng_, 0.4f);
        int k = spawnRandom(at, params.reseedCount, 500.f, 0.3f);
        log(EventKind::Catastrophe, 0, at, "Life nearly vanished. " + std::to_string(k) + " new colonists arrive");
    }

    auto ema = [](double& v, double x) { v = v * 0.9 + x * 0.1; };
    ema(timings.grid, std::chrono::duration<double, std::milli>(t1 - t0).count());
    ema(timings.sense, std::chrono::duration<double, std::milli>(t2 - t1).count());
    ema(timings.interact, std::chrono::duration<double, std::milli>(t3 - t2).count());
    ema(timings.move, std::chrono::duration<double, std::milli>(t4 - t3).count());
    ema(timings.env, std::chrono::duration<double, std::milli>(t5 - t4).count());
    ema(timings.book, msSince(t5));
    ema(timings.total, msSince(t0));
}

void World::buildGrids() {
    const int n = int(creatures.size());
    cgrid_.build(n, [this](int i) { return creatures[i].pos; });
    cgrid_.r.resize(n);
    cgrid_.hue.resize(n);
    cgrid_.signal.resize(n);
    cgrid_.mass.resize(n);
    cgrid_.angle.resize(n);
    cgrid_.camo.resize(n);
    cgrid_.callA.resize(n);
    cgrid_.callB.resize(n);
    float maxR = 4.f;
    for (int k = 0; k < n; ++k) {
        const Creature& c = creatures[cgrid_.idx[k]];
        cgrid_.r[k] = c.radius;
        cgrid_.hue[k] = c.genome.g[G_HUE];
        cgrid_.signal[k] = c.signal;
        cgrid_.mass[k] = c.mass;
        cgrid_.angle[k] = c.angle;
        cgrid_.camo[k] = c.camo;
        cgrid_.callA[k] = c.call[0];
        cgrid_.callB[k] = c.call[1];
        maxR = std::max(maxR, c.radius);
    }
    maxRadius_ = maxR;
    const int m = int(corpses.size());
    mgrid_.build(m, [this](int i) { return corpses[i].pos; });
    mgrid_.r.resize(m);
    for (int k = 0; k < m; ++k) mgrid_.r[k] = corpses[mgrid_.idx[k]].radius;
}

float World::seasonPhase() const {
    return float(tickCount % std::max(1, params.yearLength)) / float(std::max(1, params.yearLength));
}
float World::seasonSin() const { return std::sin(TAU * seasonPhase()); }

float World::camouflageOf(const Creature& c) const {
    if (!params.camouflage) return 0.f;
    // Background colour: green over grass, tan over bare ground, pale over snow/sand. Matching it hides you,
    // more so when standing still.
    const int idx = terrain.index(c.pos.x, c.pos.y);
    if (terrain.water[idx]) return 0.f;
    const float grass = clamp01(terrain.plant[idx] / std::max(0.01f, params.plantCapacity) * 1.4f);
    const float bgHue = lerp(0.09f, 0.30f, grass);
    float match = clamp01(1.f - hueDistance(c.genome.g[G_HUE], bgHue) * 7.f);
    float stillness = 1.f - clamp01(c.vel.length() / 2.5f);
    return 0.5f * match * (0.45f + 0.55f * stillness);
}

Vec2 World::rainOffset() const {
    const float t = float(tickCount % 2000000) * 0.012f * params.windSpeed;
    return {t, t * 0.37f};
}

float World::rainAt(float x, float y) const {
    if (!params.weather || terrain.rainTile.empty()) return 0.5f;
    const int T = Terrain::RAIN_TILE;
    Vec2 off = rainOffset();
    float u = x / rainScale() * T + off.x, v = y / rainScale() * T + off.y;
    int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    float fx = u - float(x0), fy = v - float(y0);
    auto at = [&](int i, int j) { return terrain.rainTile[size_t(((j % T) + T) % T) * T + size_t(((i % T) + T) % T)]; };
    return lerp(lerp(at(x0, y0), at(x0 + 1, y0), fx), lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

float World::climate() const {
    float c = params.climateOffset + climateEvent_;
    if (params.glacialCycleYears > 0.f)
        c += params.glacialAmplitude *
             std::sin(TAU * float(double(tickCount) / (double(params.yearLength) * params.glacialCycleYears)));
    return c;
}

void World::updateSeaLevel() {
    if (terrain.baseSeaLevel <= 0.f) return; // no oceans (flatland)
    float target = terrain.baseSeaLevel;
    if (params.dynamicSeaLevel) target += 0.07f * clampf(climate(), -1.f, 1.f);
    target = clampf(target, 0.02f, 0.95f);
    if (std::fabs(target - terrain.seaLevel) < 0.003f) return;
    terrain.seaLevel = target;
    for (size_t i = 0; i < terrain.elevation.size(); ++i) {
        uint8_t was = terrain.water[i];
        terrain.refreshCell(int(i));
        if (terrain.water[i] && !was) terrain.plant[i] = 0.f;
    }
    ++terrain.version;
}

void World::volcano(Vec2 at, float radius) {
    // Lava builds land (a new island if at sea), scorches everything nearby and buries the plants.
    for (int k = 0; k < 4; ++k) terraform(at, radius * (1.f - 0.2f * float(k)), 0.12f);
    int killed = killRadius(at, radius * 0.6f, DeathCause::Meteor);
    feed(at, radius * 1.3f, -1.f);
    for (size_t i = 0; i < terrain.plant.size(); ++i) {
        int cx = int(i) % terrain.cols, cy = int(i) / terrain.cols;
        Vec2 cc{(cx + 0.5f) * terrain.cellSize, (cy + 0.5f) * terrain.cellSize};
        if ((cc - at).lengthSq() < radius * radius * 1.7f) terrain.plant[i] = 0.f;
    }
    pushFx(FxEvent::Meteor, at, {1.f, 0.35f, 0.1f, 1.f}, radius * 0.8f);
    log(EventKind::Catastrophe, 0, at, "A volcano erupts, killing " + std::to_string(killed) + " and raising new land");
}

float World::daylight() const {
    if (!params.dayNight) return 1.f;
    float p = float(tickCount % std::max(1, params.dayLength)) / float(std::max(1, params.dayLength));
    return smoothstep(-0.3f, 0.3f, std::cos(TAU * p));
}

const char* World::seasonName(bool north) const {
    float p = seasonPhase();
    if (!north) p = std::fmod(p + 0.5f, 1.f);
    if (p >= 0.125f && p < 0.375f) return "Summer";
    if (p >= 0.375f && p < 0.625f) return "Autumn";
    if (p >= 0.625f && p < 0.875f) return "Winter";
    return "Spring";
}

float World::temperatureAtCell(int idx) const {
    int row = idx / terrain.cols;
    float lat = terrain.latitude(row);
    return terrain.baseTemp[idx] - 0.45f * params.seasonStrength * lat * seasonSin() + climate();
}

void World::senseAndThink(int i) {
    Creature& c = creatures[i];
    float* in = c.in;
    std::fill(in, in + N_INPUTS, 0.f);
    const Phenotype& p = c.ph;
    // Lifetime learning: reward-modulated Hebbian update of the hidden->output offsets. Reward is the energy just
    // gained (eating) minus pain; big lump drops (giving birth, sharing) are not punished. Offsets slowly fade.
    if (p.learnRate > 1e-4f) {
        const float dE = (c.energy - c.lastEnergy) / c.maxEnergy;
        const float r = (dE < -0.05f ? 0.f : clampf(dE * 400.f, -1.f, 1.f)) - 0.3f * c.pain;
        for (int j = 0; j < N_HIDDEN; ++j) {
            const float hj = c.hid[j] * p.learnRate * r;
            float* L = c.learn + j * N_OUTPUTS;
            for (int q = 0; q < N_OUTPUTS; ++q) L[q] = clampf(L[q] * 0.9997f + hj * c.out[q], -1.5f, 1.5f);
        }
    }
    c.lastEnergy = c.energy;
    const float light = daylight();
    const float fullR = p.visionRange;
    const float R = fullR * (light + (1.f - light) * (0.35f + 0.65f * p.nightVision));
    const float halfFov = p.fov * 0.5f;
    const float invEyeW = float(NUM_EYES) / p.fov;
    const float myHue = c.genome.g[G_HUE];
    const float px = c.pos.x, py = c.pos.y;
    const float hearR = std::min(R, 160.f) + 40.f;
    float hearSum = 0.f, hearW = 0.f;
    float hearA = 0.f, hearB = 0.f, dirA = 0.f, dirB = 0.f;
    float flockX = 0.f, flockY = 0.f, crowd = 0.f;
    bool touch = false;

    const SpatialGrid& g = cgrid_;
    g.query(px, py, std::max(fullR, hearR) + maxRadius_, [&](int k) {
        if (g.idx[k] == i) return;
        const float dx = g.x[k] - px, dy = g.y[k] - py;
        const float d2 = dx * dx + dy * dy;
        const float rk = g.r[k];
        // Bioluminescent (signalling) creatures stay visible in the dark.
        const float glow = clamp01((std::fabs(g.signal[k]) - 0.2f) * 1.25f);
        const float effR = (R + (fullR - R) * glow) * (1.f - g.camo[k] * (1.f - glow));
        const float lim = effR + rk;
        if (d2 > lim * lim && d2 > hearR * hearR) return;
        const float d = std::sqrt(d2);
        if (d < c.radius + rk + 1.5f) touch = true;
        if (d < hearR) {
            float w = 1.f - d / hearR;
            hearSum += g.signal[k] * w;
            hearW += w;
            if (g.callA[k] > 0.f || g.callB[k] > 0.f) {
                const float rel = std::sin(fastAtan2(dy, dx) - c.angle); // +right, -left
                hearA += w * g.callA[k];
                hearB += w * g.callB[k];
                dirA += w * g.callA[k] * rel;
                dirB += w * g.callB[k] * rel;
            }
            // Flocking cues: neighbours' headings (kin weighted more) and crowding.
            float kin = 1.f - 2.f * hueDistance(myHue, g.hue[k]);
            float fw = w * (0.3f + 0.7f * std::max(0.f, kin));
            flockX += std::cos(g.angle[k]) * fw;
            flockY += std::sin(g.angle[k]) * fw;
            crowd += w;
        }
        if (d > lim) return;
        float a = wrapAngle(fastAtan2(dy, dx) - c.angle);
        if (a < -halfFov || a > halfFov) return;
        int e = std::clamp(int((a + halfFov) * invEyeW), 0, NUM_EYES - 1);
        float cl = clamp01(1.f - std::max(0.f, d - rk - c.radius) / effR);
        float* eye = in + e * EC_COUNT;
        if (cl > eye[EC_CREATURE]) {
            eye[EC_CREATURE] = cl;
            float ratio = rk / c.radius;
            eye[EC_SIZE] = (ratio - 1.f) / (ratio + 1.f);
            eye[EC_KIN] = 1.f - 4.f * hueDistance(myHue, g.hue[k]);
        }
    });

    const SpatialGrid& mg = mgrid_;
    mg.query(px, py, R + 16.f, [&](int k) {
        const float dx = mg.x[k] - px, dy = mg.y[k] - py;
        const float d2 = dx * dx + dy * dy;
        const float lim = R + mg.r[k];
        if (d2 > lim * lim) return;
        float a = wrapAngle(fastAtan2(dy, dx) - c.angle);
        if (a < -halfFov || a > halfFov) return;
        int e = std::clamp(int((a + halfFov) * invEyeW), 0, NUM_EYES - 1);
        float cl = 1.f - std::max(0.f, std::sqrt(d2) - mg.r[k] - c.radius) / R;
        float& slot = in[e * EC_COUNT + EC_MEAT];
        if (cl > slot) slot = cl;
    });

    const float invCap = 1.f / std::max(0.01f, params.plantCapacity);
    const float eyeW = p.fov / float(NUM_EYES);
    float waterDir = 0.f, waterBest = 0.f;
    for (int e = 0; e < NUM_EYES; ++e) {
        float a = c.angle - halfFov + (float(e) + 0.5f) * eyeW;
        float ca = std::cos(a), sa = std::sin(a);
        float plant = 0.f, obstacle = 0.f;
        for (int s = 0; s < 4; ++s) {
            float d = R * SAMPLE_D[s];
            float sx = px + ca * d, sy = py + sa * d;
            if (!terrain.inside(sx, sy)) {
                if (obstacle == 0.f) obstacle = 1.f - SAMPLE_D[s];
                continue;
            }
            int idx = terrain.index(sx, sy);
            if (terrain.water[idx] && obstacle == 0.f) {
                obstacle = 1.f - SAMPLE_D[s];
                if (obstacle > waterBest) {
                    waterBest = obstacle;
                    waterDir = (float(e) - float(NUM_EYES - 1) * 0.5f) / (float(NUM_EYES - 1) * 0.5f);
                }
            }
            plant += SAMPLE_W[s] * terrain.plant[idx] * invCap;
        }
        in[e * EC_COUNT + EC_PLANT] = std::min(plant, 1.f);
        in[e * EC_COUNT + EC_OBSTACLE] = obstacle;
    }

    const int cell = terrain.index(px, py);
    const Vec2 dir = fromAngle(c.angle);
    const int mouthCell = terrain.index(px + dir.x * c.radius, py + dir.y * c.radius);
    in[IN_ENERGY] = c.energy / c.maxEnergy;
    in[IN_HEALTH] = c.health / c.maxHealth;
    in[IN_AGE] = std::min(1.5f, c.age / p.lifespan);
    in[IN_SPEED] = clampf(c.vel.dot(dir) / (p.accel * 8.f + 1e-3f), -1.f, 1.f);
    in[IN_WATER] = terrain.water[cell] ? 1.f : 0.f;
    c.temperature = temperatureAtCell(cell);
    in[IN_TEMP] = clampf(c.temperature - p.thermalOpt, -1.5f, 1.5f);
    in[IN_GRASS] = terrain.plant[mouthCell] * invCap;
    in[IN_PAIN] = c.pain;
    in[IN_CLOCK] = std::sin(c.clockPhase);
    in[IN_SIGNAL] = hearW > 0.f ? hearSum / hearW : 0.f;
    in[IN_TOUCH] = touch ? 1.f : 0.f;
    in[IN_SICK] = c.infection > 0.f ? 1.f : 0.f;
    in[IN_LIGHT] = light;
    if (flockX != 0.f || flockY != 0.f)
        in[IN_FLOCK] = std::sin(wrapAngle(fastAtan2(flockY, flockX) - c.angle));
    in[IN_CROWD] = std::min(1.f, crowd * 0.2f);
    in[IN_THIRST] = 1.f - c.hydration;
    in[IN_WATER_DIR] = waterBest > 0.f ? (waterDir == 0.f ? 0.01f : waterDir) : 0.f;
    // Smell: pheromone here and the gradient across the head (right minus left).
    const Vec2 right{-dir.y, dir.x};
    const float side = c.radius * 2.f + 10.f;
    const Vec2 front = c.pos + dir * side;
    const Vec2 pr = front + right * side, pl = front - right * side;
    float smellR = terrain.inside(pr.x, pr.y) ? terrain.pheromone[terrain.index(pr.x, pr.y)] : 0.f;
    float smellL = terrain.inside(pl.x, pl.y) ? terrain.pheromone[terrain.index(pl.x, pl.y)] : 0.f;
    in[IN_SMELL] = std::min(1.f, terrain.pheromone[cell]);
    in[IN_SMELL_DIR] = clampf(smellR - smellL, -1.f, 1.f);
    in[IN_HEAR_A] = std::min(1.f, hearA);
    in[IN_HEAR_A_DIR] = hearA > 1e-3f ? clampf(dirA / hearA, -1.f, 1.f) : 0.f;
    in[IN_HEAR_B] = std::min(1.f, hearB);
    in[IN_HEAR_B_DIR] = hearB > 1e-3f ? clampf(dirB / hearB, -1.f, 1.f) : 0.f;
    in[IN_TENDED] = terrain.tilled[cell];

    think(c.genome.w.data(), in, c.learn, c.hid, c.out);
}

void World::kill(Creature& c, DeathCause cause) {
    if (!c.alive) return;
    c.alive = false;
    c.cause = cause;
}

void World::interact() {
    const int n = int(creatures.size());
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0);
    for (int i = n - 1; i > 0; --i) std::swap(order_[i], order_[rng_.below(i + 1)]);
    for (Creature& c : creatures) c.hitsThisTick = 0;

    for (int oi = 0; oi < n; ++oi) {
        const int i = order_[oi];
        Creature& c = creatures[i];
        if (!c.alive) continue;
        c.flags &= ~(CF_EATING | CF_ATTACKING | CF_SHARING | CF_MATED | CF_HIT | CF_POISONED);
        const Phenotype& p = c.ph;
        const Vec2 dir = fromAngle(c.angle);

        // --- Eating ---------------------------------------------------------
        if (c.out[OUT_EAT] > 0.f) {
            const float eager = 0.5f + 0.5f * c.out[OUT_EAT];
            const Vec2 mouth = c.pos + dir * (c.radius * 0.8f);
            if (p.plantEff > 0.01f) {
                // Graze every cell under the mouth: big bodies sweep a wider swath of sparse grass.
                const float reach = std::max(4.f, c.radius * 0.9f);
                const float cs = terrain.cellSize;
                int x0 = std::max(0, int((mouth.x - reach) / cs)), x1 = std::min(terrain.cols - 1, int((mouth.x + reach) / cs));
                int y0 = std::max(0, int((mouth.y - reach) / cs)), y1 = std::min(terrain.rows - 1, int((mouth.y + reach) / cs));
                float avail = 0.f;
                for (int yy = y0; yy <= y1; ++yy)
                    for (int xx = x0; xx <= x1; ++xx) avail += terrain.plant[yy * terrain.cols + xx];
                float bite = std::min(avail, 0.5f * std::pow(c.mass, 0.9f) * eager);
                // Hawk-dove food contest with whoever already grazed this patch this tick.
                if (params.foodContests && bite > 0.f) {
                    const size_t cell = size_t(terrain.index(mouth.x, mouth.y));
                    if (cellEater_.size() != terrain.plant.size()) {
                        cellEater_.assign(terrain.plant.size(), -1);
                        cellEaterTick_.assign(terrain.plant.size(), -1);
                    }
                    if (cellEaterTick_[cell] == tickCount && cellEater_[cell] != i && cellEater_[cell] >= 0 &&
                        cellEater_[cell] < int(creatures.size())) {
                        Creature& rival = creatures[size_t(cellEater_[cell])];
                        const bool meHawk = c.out[OUT_ATTACK] > 0.f, rivalHawk = rival.out[OUT_ATTACK] > 0.f;
                        if (!meHawk && !rivalHawk) {          // doves share
                            bite *= 0.5f;
                            ++stats.contestDD;
                        } else if (meHawk && !rivalHawk) {    // hawk intimidates dove and eats more
                            bite = std::min(avail, bite * 1.5f);
                            rival.energy = std::max(0.f, rival.energy - bite * 0.3f);
                            ++stats.contestHD;
                        } else if (!meHawk && rivalHawk) {    // dove backs off
                            bite = 0.f;
                            ++stats.contestHD;
                        } else {                              // hawks fight: both pay
                            const float cost = 0.06f * std::sqrt(c.mass * rival.mass);
                            c.health -= c.maxHealth * cost;
                            rival.health -= rival.maxHealth * cost;
                            c.flags |= CF_HURT;
                            rival.flags |= CF_HURT;
                            bite *= 0.5f;
                            ++stats.contestHH;
                            if (c.health <= 0.f) kill(c, DeathCause::Predation);
                            if (rival.health <= 0.f) kill(rival, DeathCause::Predation);
                        }
                    }
                    cellEater_[cell] = i;
                    cellEaterTick_[cell] = tickCount;
                }
                if (bite > 0.f) {
                    const size_t gcell = size_t(terrain.index(mouth.x, mouth.y));
                    BehaviorAcc& ba = behaviorOf(c.speciesId);
                    ba.bites += 1.f;
                    if (terrain.tilled[gcell] > 0.2f) {
                        if (terrain.tiller[gcell] == c.speciesId) ba.bitesOwn += 1.f;
                        else { ba.bitesOther += 1.f; ba.otherTiller = terrain.tiller[gcell]; }
                    }
                    const float take = bite / avail;
                    for (int yy = y0; yy <= y1; ++yy)
                        for (int xx = x0; xx <= x1; ++xx) terrain.plant[yy * terrain.cols + xx] *= 1.f - take;
                    float gain = bite * params.plantEnergy * p.plantEff;
                    c.energy += gain;
                    c.hydration = std::min(1.f, c.hydration + bite * 0.025f); // juicy grass carries some water
                    c.eatenPlant += gain;
                    c.flags |= CF_EATING;
                }
            }
            if (p.meatEff > 0.01f && !corpses.empty()) {
                int best = -1;
                float bestD = 0.f;
                mgrid_.query(mouth.x, mouth.y, c.radius + 16.f, [&](int k) {
                    int ci = mgrid_.idx[k];
                    const Corpse& m = corpses[ci];
                    if (m.energy <= 0.f) return;
                    float d = (m.pos - mouth).length() - m.radius - c.radius * 0.5f;
                    if (d < bestD) { bestD = d; best = ci; }
                });
                if (best >= 0) {
                    Corpse& m = corpses[best];
                    float bite = std::min(m.energy, (0.6f + 1.4f * p.weapon) * c.mass075 * eager);
                    m.energy -= bite;
                    float gain = bite * p.meatEff;
                    c.energy += gain;
                    c.eatenMeat += gain;
                    c.flags |= CF_EATING;
                    if (m.toxin > 0.05f) {
                        c.health -= bite * m.toxin * 1.2f;
                        c.pain = std::min(1.f, c.pain + 0.3f);
                        c.flags |= CF_POISONED;
                        if (c.health <= 0.f) kill(c, DeathCause::Poison);
                    }
                }
            }
            if (c.energy > c.maxEnergy) c.energy = c.maxEnergy;
        }
        if (!c.alive) continue;

        // --- Attacking ------------------------------------------------------
        if (c.out[OUT_ATTACK] > 0.f) {
            c.flags |= CF_ATTACKING;
            c.energy -= 0.012f * c.mass * c.out[OUT_ATTACK] * params.metabolism;
            int target = -1;
            float bestGap = 1e9f;
            cgrid_.query(c.pos.x, c.pos.y, c.radius + maxRadius_ + 4.f, [&](int k) {
                int j = cgrid_.idx[k];
                if (j == i) return;
                const Creature& t = creatures[j];
                if (!t.alive) return;
                Vec2 d = t.pos - c.pos;
                float gap = d.length() - c.radius - t.radius;
                if (gap > 4.f) return;
                float a = wrapAngle(fastAtan2(d.y, d.x) - c.angle);
                if (std::fabs(a) > 0.8f) return;
                if (gap < bestGap) { bestGap = gap; target = j; }
            });
            if (target >= 0) {
                Creature& t = creatures[target];
                float strength = 0.5f + 0.5f * c.out[OUT_ATTACK];
                // Primer-style size rule: big attackers shred small targets, small ones barely scratch giants.
                const float sizeEdge = clampf(std::sqrt(c.mass / t.mass), 0.25f, 3.f);
                // Pack hunting: every extra attacker this tick adds 25%. Safety in numbers: crowded prey are
                // harder to single out.
                const float pack = 1.f + 0.25f * float(std::min<uint8_t>(t.hitsThisTick, 3));
                const float herd = 1.f - 0.35f * t.in[IN_CROWD];
                float dmg = p.damage * strength * sizeEdge * pack * herd * (1.f - 0.75f * t.ph.armor);
                if (t.hitsThisTick > 0) ++stats.attacksPack;
                if (t.hitsThisTick < 255) ++t.hitsThisTick;
                t.health -= dmg;
                t.pain = std::min(1.f, t.pain + dmg / t.maxHealth * 6.f + 0.1f);
                t.flags |= CF_HURT;
                t.lastAttacker = c.id;
                float drain = std::min(t.energy, dmg * 1.0f);
                t.energy -= drain;
                c.energy = std::min(c.maxEnergy, c.energy + drain * p.meatEff);
                c.flags |= CF_HIT;
                if (t.speciesId == c.speciesId) ++stats.attacksIntra;
                if (t.ph.toxin > 0.3f) ++stats.attacksToxic;
                else ++stats.attacksInter;
                if (t.ph.toxin > 0.05f) {
                    c.health -= dmg * t.ph.toxin * 1.5f;
                    c.pain = std::min(1.f, c.pain + 0.4f);
                    c.flags |= CF_POISONED;
                    if (rng_.chance(0.3f)) pushFx(FxEvent::Poison, c.pos, {0.7f, 1.f, 0.2f, 1.f}, c.radius);
                }
                if (rng_.chance(0.25f)) pushFx(FxEvent::Hit, t.pos, {1.f, 0.25f, 0.2f, 1.f}, t.radius);
                if (t.health <= 0.f && t.alive) {
                    kill(t, DeathCause::Predation);
                    ++c.kills;
                }
                if (c.health <= 0.f) kill(c, DeathCause::Poison);
            }
        }
        if (!c.alive) continue;

        // --- Sharing (altruism) ----------------------------------------------
        if (c.out[OUT_SHARE] > 0.3f && c.energy > 0.3f * c.maxEnergy) {
            int target = -1;
            float lowest = c.energy / c.maxEnergy;
            cgrid_.query(c.pos.x, c.pos.y, c.radius + maxRadius_ + 3.f, [&](int k) {
                int j = cgrid_.idx[k];
                if (j == i) return;
                const Creature& t = creatures[j];
                if (!t.alive) return;
                if ((t.pos - c.pos).length() - c.radius - t.radius > 3.f) return;
                float f = t.energy / t.maxEnergy;
                if (f < lowest) { lowest = f; target = j; }
            });
            if (target >= 0) {
                Creature& t = creatures[target];
                float amount = std::min(0.8f, c.energy * 0.02f);
                c.energy -= amount;
                t.energy = std::min(t.maxEnergy, t.energy + amount * params.shareEfficiency);
                c.flags |= CF_SHARING;
                c.shared += amount;
                if (rng_.chance(0.05f)) pushFx(FxEvent::Gift, t.pos, {1.f, 0.8f, 0.35f, 1.f}, t.radius);
            }
        }

        // --- Contagion --------------------------------------------------------
        if (c.infection > 0.f) {
            cgrid_.query(c.pos.x, c.pos.y, c.radius + maxRadius_ + 8.f, [&](int k) {
                int j = cgrid_.idx[k];
                if (j == i) return;
                Creature& t = creatures[j];
                if (!t.alive || t.infection > 0.f || t.immuneTicks > 0.f) return;
                if ((t.pos - c.pos).length() - c.radius - t.radius > 8.f) return;
                // Aggressive strains shed more (spread faster) but burn out sooner and hurt more.
                float p = params.transmission * (0.4f + 1.2f * c.strain) * (1.f - 0.9f * t.ph.immunity) *
                          (t.speciesId == c.speciesId ? 1.f : params.crossSpecies);
                if (rng_.chance(p)) {
                    t.strain = clamp01(c.strain + rng_.normal() * 0.04f);
                    t.infection = float(params.diseaseDuration) * (1.3f - 0.6f * t.strain) * rng_.range(0.7f, 1.3f);
                    t.flags |= CF_SICK;
                }
            });
            if (rng_.chance(0.01f)) pushFx(FxEvent::Sick, c.pos, {0.65f, 0.95f, 0.35f, 0.8f}, c.radius);
        }

        // --- Reproduction ---------------------------------------------------
        if (c.out[OUT_REPRODUCE] > 0.f && c.reproCooldown <= 0 && c.growth >= 1.f &&
            c.energy > 0.55f * c.maxEnergy && int(creatures.size() + newborns_.size()) < params.maxPopulation)
            reproduce(c);
    }
}

void World::reproduce(Creature& c) {
    const Phenotype& p = c.ph;
    Creature* mate = nullptr;
    if (params.sexualReproduction && c.rng.chance(p.sexuality)) {
        // Mate choice: nearby adults of the same species, scored by how well their colour matches
        // this creature's preference. Choosy creatures turn down poor matches (sexual selection).
        const float pref = c.genome.g[G_MATE_PREF], choosy = c.genome.g[G_CHOOSY];
        const float tolerance = 0.5f - 0.45f * choosy;
        float best = 1e18f;
        cgrid_.query(c.pos.x, c.pos.y, c.radius + maxRadius_ + 40.f, [&](int k) {
            Creature& o = creatures[cgrid_.idx[k]];
            if (&o == &c || !o.alive || o.growth < 1.f) return;
            if (o.speciesId != c.speciesId) {
                // Hybridization: only sister species, only rarely, only if the genomes are still compatible.
                if (!params.hybridization || !c.rng.chance(0.15f)) return;
                const Species& a = species.get(c.speciesId);
                const Species& b = species.get(o.speciesId);
                const bool sisters = a.parentId == o.speciesId || b.parentId == c.speciesId ||
                                     (a.parentId && a.parentId == b.parentId);
                if (!sisters || geneticDistance(c.genome, o.genome) > species.threshold * 1.3f) return;
            }
            float d2 = (o.pos - c.pos).lengthSq();
            if (d2 > sq(c.radius + o.radius + 40.f)) return;
            float mismatch = hueDistance(pref, o.genome.g[G_HUE]);
            if (mismatch > tolerance) return;
            float score = std::sqrt(d2) * 0.02f + mismatch * 8.f * choosy;
            if (score < best) { best = score; mate = &o; }
        });
    }
    const int litter = p.litter;
    const float total = c.energy * p.investment;
    const float each = total / float(litter);
    c.energy -= total * 1.08f;
    c.reproCooldown = int(150.f + p.maturity * 0.25f);
    const uint32_t gen = std::max(c.generation, mate ? mate->generation : 0u) + 1;
    const Vec2 back = fromAngle(c.angle) * -(c.radius + 2.f);
    for (int k = 0; k < litter; ++k) {
        Genome g = mate ? crossover(c.genome, mate->genome, c.rng) : c.genome;
        mutate(g, c.rng, params.mutationRate);
        Vec2 pos = c.pos + back + Vec2(c.rng.range(-5.f, 5.f), c.rng.range(-5.f, 5.f));
        pos.x = clampf(pos.x, 5.f, terrain.width - 5.f);
        pos.y = clampf(pos.y, 5.f, terrain.height - 5.f);
        bool created = false;
        uint32_t sid = species.assign(g, c.speciesId, tickCount, nextId_, created);
        Creature child = makeCreature(g, pos, sid, c.id, gen, each, 0.f);
        // Record what changed relative to the parent (mutation, plus the mate's genes if sexual).
        int changed = 0;
        for (int k = 0; k < BRAIN_SIZE; ++k) changed += g.w[k] != c.genome.w[k];
        for (int gi = 0; gi < G_COUNT; ++gi) child.geneDelta[gi] = g.g[gi] - c.genome.g[gi];
        child.synapsesChanged = uint16_t(std::min(changed, 65535));
        child.sexual = mate != nullptr;
        // Parental teaching: a plastic child starts with a fraction of what its mother has learned.
        if (child.ph.learnRate > 1e-4f)
            for (int li = 0; li < N_LEARN; ++li) child.learn[li] = c.learn[li] * 0.4f;
        species.onBirth(sid);
        if (created) {
            const Species& s = species.get(sid);
            std::string from = species.valid(s.parentId) ? species.get(s.parentId).name() : "unknown";
            log(EventKind::Speciation, sid, pos, "New species " + s.name() + " branches from " + from);
            pushFx(FxEvent::Speciation, pos, s.color, 40.f);
        }
        newborns_.push_back(child);
        maxGeneration = std::max(maxGeneration, gen);
    }
    c.children += uint32_t(litter);
    if (mate && mate->speciesId != c.speciesId) {
        ++stats.totalHybrids;
        const uint64_t pair = (uint64_t(std::min(c.speciesId, mate->speciesId)) << 32) | std::max(c.speciesId, mate->speciesId);
        if (pair != lastHybridPair_) {
            lastHybridPair_ = pair;
            log(EventKind::Info, c.speciesId, c.pos,
                "Hybrid born: " + species.get(c.speciesId).name() + " x " + species.get(mate->speciesId).name());
        }
    }
    if (mate) {
        c.flags |= CF_MATED;
        mate->flags |= CF_MATED;
        pushFx(FxEvent::Love, (c.pos + mate->pos) * 0.5f, {1.f, 0.45f, 0.65f, 1.f}, c.radius);
    }
    stats.birthsAcc += litter;
    stats.totalBirths += uint64_t(litter);
    pushFx(FxEvent::Birth, c.pos, hsv(c.genome.g[G_HUE], 0.5f, 1.f), c.radius);
}

void World::moveAndMetabolize(int i) {
    Creature& c = creatures[i];
    if (!c.alive) return;
    const Phenotype& p = c.ph;
    float thrust = c.out[OUT_THRUST];
    if (thrust < 0.f) thrust *= 0.4f;
    const float turn = c.out[OUT_TURN];
    c.angle = wrapAngle(c.angle + turn * p.turnRate);
    const int cell = terrain.index(c.pos.x, c.pos.y);
    const bool inWater = terrain.water[cell] != 0;
    if (inWater) c.flags |= CF_IN_WATER;
    else c.flags &= ~CF_IN_WATER;
    const Vec2 dir = fromAngle(c.angle);
    c.vel += dir * (p.accel * thrust * (inWater ? 0.45f : 1.f));
    c.vel *= inWater ? 0.80f : 0.88f;

    Vec2 push;
    const SpatialGrid& g = cgrid_;
    g.query(c.pos.x, c.pos.y, c.radius + maxRadius_, [&](int k) {
        if (g.idx[k] == i) return;
        float dx = c.pos.x - g.x[k], dy = c.pos.y - g.y[k];
        float minD = c.radius + g.r[k];
        float d2 = dx * dx + dy * dy;
        if (d2 >= minD * minD || d2 < 1e-8f) return;
        float d = std::sqrt(d2);
        float overlap = minD - d;
        float w = g.mass[k] / (g.mass[k] + c.mass);
        push.x += dx / d * overlap * w * 0.5f;
        push.y += dy / d * overlap * w * 0.5f;
    });
    c.pos += c.vel + push;
    const float W = terrain.width, H = terrain.height;
    if (c.pos.x < c.radius) { c.pos.x = c.radius; if (c.vel.x < 0) c.vel.x *= -0.3f; }
    if (c.pos.y < c.radius) { c.pos.y = c.radius; if (c.vel.y < 0) c.vel.y *= -0.3f; }
    if (c.pos.x > W - c.radius) { c.pos.x = W - c.radius; if (c.vel.x > 0) c.vel.x *= -0.3f; }
    if (c.pos.y > H - c.radius) { c.pos.y = H - c.radius; if (c.vel.y > 0) c.vel.y *= -0.3f; }

    // Metabolism (Kleiber scaling for basal cost).
    const float m = c.mass;
    const float basal = 0.010f * c.mass075 * (1.f + 0.5f * p.longevity);
    const float toxin = 0.008f * c.mass075 * p.toxin * params.toxinCost;
    const float move = 0.030f * c.mass075 * thrust * thrust * (0.6f + 0.8f * c.genome.g[G_SPEED]) * (inWater ? 2.f : 1.f) *
                       params.moveCost;
    const float turnCost = 0.004f * m * turn * turn * params.moveCost;
    const float organScale = 0.4f + 0.6f * c.mass075; // eyes and brains scale sub-linearly with body
    const float vision = 0.00005f * p.visionRange * (0.3f + 0.7f * p.fov / TAU) * organScale * params.visionCost;
    const float brain = 0.000012f * float(p.connections) * organScale * params.brainCost;
    const float dT = c.temperature - p.thermalOpt;
    const float thermal = 0.03f * std::sqrt(m) * dT * dT * params.thermalCost; // surface-area loss (Bergmann)
    const float signal = 0.002f * std::fabs(c.out[OUT_SIGNAL]);
    c.call[0] = std::max(0.f, c.out[OUT_CALL_A]);
    c.call[1] = std::max(0.f, c.out[OUT_CALL_B]);
    c.tend = std::max(0.f, c.out[OUT_TEND]);
    const float voice = 0.0025f * (c.call[0] + c.call[1]);
    const float tendCost = 0.008f * c.mass075 * c.tend;
    const float plasticity = 0.0015f * c.mass075 * sq(c.genome.g[G_PLASTICITY]);
    const float immuneCost = 0.004f * c.mass075 * p.immunity + 0.003f * c.mass075 * p.nightVision;
    float disease = 0.f;
    if (c.infection > 0.f) {
        const float harm = params.virulence * (0.3f + 2.f * c.strain * c.strain);
        disease = 0.010f * c.mass075 * harm;
        c.health -= c.maxHealth * 0.00022f * harm;
        c.infection -= 1.f;
        c.flags |= CF_SICK;
        if (c.infection <= 0.f) {
            c.infection = 0.f;
            c.immuneTicks = float(params.immunityDuration);
            c.flags &= ~CF_SICK;
        }
        if (c.health <= 0.f) kill(c, DeathCause::Plague);
    } else if (c.immuneTicks > 0.f) {
        c.immuneTicks -= 1.f;
        c.flags |= CF_IMMUNE;
    } else {
        c.flags &= ~(CF_SICK | CF_IMMUNE);
    }
    const float pher = c.out[OUT_PHEROMONE] > 0.f ? 0.003f * c.out[OUT_PHEROMONE] : 0.f;
    c.energy -= (basal + toxin + move + turnCost + vision + brain + thermal + signal + pher + immuneCost + disease + voice +
                 tendCost + plasticity) *
                params.metabolism;

    if (c.energy <= 0.f) {
        c.health += c.energy * 3.f;
        c.energy = 0.f;
        if (c.health <= 0.f) kill(c, DeathCause::Starvation);
    } else if (c.health < c.maxHealth && c.energy > 0.15f * c.maxEnergy && c.infection <= 0.f) {
        float heal = std::min(c.maxHealth * 0.0015f, c.maxHealth - c.health);
        c.health += heal;
        c.energy -= heal * 0.4f;
    }
    if (params.thirst) {
        // Drinking: standing in or right beside water refills quickly. Heat makes creatures thirstier.
        bool nearWater = inWater;
        if (!nearWater) {
            Vec2 snout = c.pos + dir * (c.radius + 6.f);
            nearWater = terrain.inside(snout.x, snout.y) && terrain.water[terrain.index(snout.x, snout.y)];
        }
        if (params.weather) c.hydration = std::min(1.f, c.hydration + 0.0015f * rainAt(c.pos.x, c.pos.y));
        if (nearWater) c.hydration = std::min(1.f, c.hydration + 0.02f);
        else c.hydration -= 0.00035f * (1.f + std::max(0.f, c.temperature) * 1.5f) * (0.6f + 0.4f * std::fabs(thrust));
        if (c.hydration <= 0.f) {
            c.hydration = 0.f;
            c.health -= c.maxHealth * 0.002f;
            if (c.health <= 0.f) kill(c, DeathCause::Thirst);
        }
    } else {
        c.hydration = 1.f;
    }
    c.age += 1.f;
    if (c.age > p.lifespan) {
        c.health -= c.maxHealth * 0.002f * (1.f + (c.age - p.lifespan) / p.lifespan * 4.f);
        if (c.health <= 0.f) kill(c, DeathCause::OldAge);
    }
    if (c.health <= 0.f) kill(c, c.lastAttacker ? DeathCause::Predation : DeathCause::Starvation);

    if (c.growth < 1.f && c.energy > 0.2f * c.maxEnergy) {
        c.growth = std::min(1.f, c.growth + 1.f / p.maturity);
        updateBody(c);
    }
    c.clockPhase += p.clockFreq;
    if (c.clockPhase > TAU) c.clockPhase -= TAU;
    c.animPhase += 0.12f + std::fabs(thrust) * 0.45f;
    if (c.animPhase > 1000.f) c.animPhase -= 1000.f;
    c.pain *= 0.92f;
    if (c.reproCooldown > 0) --c.reproCooldown;
    c.signal = c.out[OUT_SIGNAL];
    c.pheromone = std::max(0.f, c.out[OUT_PHEROMONE]);
    c.camo = camouflageOf(c);
}

void World::resolveDeathsAndBirths() {
    for (size_t i = 0; i < creatures.size();) {
        Creature& c = creatures[i];
        if (c.alive) {
            ++i;
            continue;
        }
        if (c.cause != DeathCause::Smite) {
            Corpse m;
            m.pos = c.pos;
            m.energy = m.initial = 70.f * c.mass + 0.6f * std::max(0.f, c.energy);
            m.radius = 3.f + std::sqrt(m.energy) * 0.7f;
            m.hue = c.genome.g[G_HUE];
            m.toxin = c.ph.toxin;
            corpses.push_back(m);
        }
        considerRecords(c);
        if (species.onDeath(c.speciesId, tickCount)) {
            const Species& s = species.get(c.speciesId);
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s went extinct after %.1f years (peak %d)", s.name().c_str(),
                          double(tickCount - s.bornTick) / params.yearLength, s.peakPopulation);
            if (s.peakPopulation >= 5) log(EventKind::Extinction, s.id, c.pos, buf);
            if (s.peakPopulation >= 20) {
                recentExtinctions_.push_back(tickCount);
                while (!recentExtinctions_.empty() && recentExtinctions_.front() < tickCount - params.yearLength)
                    recentExtinctions_.erase(recentExtinctions_.begin());
                if (recentExtinctions_.size() >= 5) unlock(15);
            }
        }
        ++stats.deathsAcc;
        ++stats.totalDeaths;
        ++stats.causeTotals[std::min<size_t>(size_t(c.cause), stats.causeTotals.size() - 1)];
        if (c.id == watchId) {
            watchCause = c.cause;
            watchDeathTick = tickCount;
        }
        pushFx(FxEvent::Death, c.pos, hsv(c.genome.g[G_HUE], 0.3f, 0.6f), c.radius);
        if (i != creatures.size() - 1) creatures[i] = creatures.back();
        creatures.pop_back();
    }
    for (const Creature& nb : newborns_) creatures.push_back(nb);
    newborns_.clear();
}

void World::updateEnvironment() {
    Terrain& t = terrain;
    for (const Creature& c : creatures)
        if (c.pheromone > 0.01f) {
            float& ph = t.pheromone[t.index(c.pos.x, c.pos.y)];
            ph = std::min(4.f, ph + 0.2f * c.pheromone);
        }
    // Niche construction: tending raises a cell and its neighbours (the creature's own cell the most).
    for (const Creature& c : creatures)
        if (c.tend > 0.05f) {
            const int cx = int(c.pos.x / t.cellSize), cy = int(c.pos.y / t.cellSize);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= t.cols || y >= t.rows) continue;
                    const size_t k = size_t(y) * t.cols + x;
                    if (t.water[k]) continue;
                    t.tilled[k] = std::min(1.f, t.tilled[k] + 0.03f * c.tend * ((dx || dy) ? 0.4f : 1.f));
                    t.tiller[k] = c.speciesId;
                }
        }

    const float s = seasonSin();
    const float amp = 0.45f * params.seasonStrength;
    const float clim = climate();
    const float rate = 0.005f * params.plantGrowth;
    const float cap = params.plantCapacity;
    const float decay = params.pheromoneDecay;
    pool_->parallelFor(0, t.rows, 4, [&](int r0, int r1, int) {
        for (int row = r0; row < r1; ++row) {
            const float seasonal = -amp * t.latitude(row) * s + clim;
            float* plant = t.plant.data() + size_t(row) * t.cols;
            float* pher = t.pheromone.data() + size_t(row) * t.cols;
            const float* fert = t.fertility.data() + size_t(row) * t.cols;
            const float* temp = t.baseTemp.data() + size_t(row) * t.cols;
            float* tilled = t.tilled.data() + size_t(row) * t.cols;
            for (int col = 0; col < t.cols; ++col) {
                pher[col] = pher[col] > 1e-3f ? pher[col] * decay : 0.f;
                const float till = tilled[col];
                tilled[col] = till > 1e-3f ? till * 0.9998f : 0.f;
                const float K = fert[col] * cap * (1.f + 2.f * till);
                float P = plant[col];
                if (K <= 0.f) {
                    plant[col] = P * 0.995f;
                    continue;
                }
                const float T = temp[col] + seasonal;
                if (T < -0.45f) P *= 0.9996f; // frost die-back
                float tf = 1.f - sq((T - 0.25f) / 0.85f);
                if (params.weather) tf *= 0.45f + 1.25f * rainAt((float(col) + 0.5f) * t.cellSize, (float(row) + 0.5f) * t.cellSize);
                if (P > K) P -= (P - K) * 0.002f;
                else if (tf > 0.f) P += rate * (1.f + till) * tf * (P + 0.03f * K) * (1.f - P / K);
                plant[col] = std::max(0.f, P);
            }
        }
    });

    for (size_t i = 0; i < corpses.size();) {
        Corpse& m = corpses[i];
        m.energy -= m.initial * params.meatDecay * 0.5f + m.energy * params.meatDecay;
        ++m.age;
        if (m.energy < 0.5f) {
            if (i != corpses.size() - 1) corpses[i] = corpses.back();
            corpses.pop_back();
            continue;
        }
        m.radius = 3.f + std::sqrt(m.energy) * 0.7f;
        ++i;
    }
    climateEvent_ *= 0.99996f;
}

void World::considerRecords(const Creature& c) {
    auto holder = [&]() {
        std::string n = species.valid(c.speciesId) ? species.get(c.speciesId).name() : "?";
        return n + " #" + std::to_string(c.id);
    };
    auto check = [&](Record& r, float v) {
        if (v > r.value) {
            r.value = v;
            r.holder = holder();
            r.speciesId = c.speciesId;
            r.tick = tickCount;
        }
    };
    check(records.oldest, c.age / float(params.yearLength));
    check(records.mostChildren, float(c.children));
    check(records.mostKills, float(c.kills));
    check(records.mostGenerous, c.shared);
    if (c.growth >= 1.f) check(records.largest, c.radius);
    check(records.mostSynapses, float(c.ph.connections));
}

void World::census() {
    auto& all = species.all();
    struct Acc {
        double genes[G_COUNT];
        double x, y, gen;
        int n;
    };
    std::vector<Acc> acc(all.size(), Acc{});
    for (const Creature& c : creatures) {
        if (!species.valid(c.speciesId)) continue;
        Acc& a = acc[c.speciesId - 1];
        for (int g = 0; g < G_COUNT; ++g) a.genes[g] += c.genome.g[g];
        a.x += c.pos.x;
        a.y += c.pos.y;
        a.gen += c.generation;
        ++a.n;
    }
    const bool recenter = tickCount % 2000 == 0;
    std::vector<int> bestIdx;
    if (recenter) bestIdx.assign(all.size(), -1);
    for (size_t s = 0; s < all.size(); ++s) {
        Acc& a = acc[s];
        if (a.n == 0) continue;
        Species& sp = all[s];
        for (int g = 0; g < G_COUNT; ++g) sp.meanGenes[g] = float(a.genes[g] / a.n);
        sp.centroid = {float(a.x / a.n), float(a.y / a.n)};
        sp.meanGeneration = float(a.gen / a.n);
        auto speciesRecord = [&](Record& r, float v) {
            if (v > r.value) {
                r.value = v;
                r.holder = sp.name();
                r.speciesId = sp.id;
                r.tick = tickCount;
            }
        };
        speciesRecord(records.biggestSpecies, float(sp.peakPopulation));
        speciesRecord(records.longestSpecies, float(tickCount - sp.bornTick) / float(params.yearLength));
        if (!sp.announcedPredator && sp.meanGenes[G_DIET] > 0.6f && sp.population >= 8) {
            sp.announcedPredator = true;
            log(EventKind::Milestone, sp.id, sp.centroid, sp.name() + " has become a predator");
        }
    }
    if (recenter) {
        // Re-centre each species' representative on its most typical living member.
        std::vector<float> bestD(all.size(), 1e9f);
        for (int i = 0; i < int(creatures.size()); ++i) {
            const Creature& c = creatures[i];
            if (!species.valid(c.speciesId)) continue;
            Genome meanG;
            const Species& sp = all[c.speciesId - 1];
            meanG.g = sp.meanGenes;
            float d = bodyDistance(c.genome, meanG);
            if (d < bestD[c.speciesId - 1]) {
                bestD[c.speciesId - 1] = d;
                bestIdx[c.speciesId - 1] = i;
            }
        }
        for (size_t s = 0; s < all.size(); ++s)
            if (bestIdx[s] >= 0) all[s].rep = creatures[bestIdx[s]].genome;
    }
    scanBehaviours();
}

void World::sampleStats() {
    StatSample s{};
    TraitHistogram h{};
    for (auto& row : h) row.fill(0);
    s.tick = float(tickCount);
    const int n = int(creatures.size());
    double sum[G_COUNT] = {}, sum2[G_COUNT] = {};
    double gen = 0, conn = 0, energy = 0;
    int toxic = 0;
    for (const Creature& c : creatures) {
        float d = c.genome.g[G_DIET];
        if (d < 0.33f) s.herbivores += 1;
        else if (d < 0.66f) s.omnivores += 1;
        else s.carnivores += 1;
        gen += c.generation;
        conn += c.ph.connections;
        energy += c.energy / c.maxEnergy;
        if (c.ph.toxin > 0.3f) ++toxic;
        if (c.infection > 0.f) {
            s.infected += 1;
            s.strainAggression += c.strain;
        }
        else if (c.immuneTicks > 0.f) s.immune += 1;
        if (c.flags & CF_SHARING) s.sharing += 1;
        if (std::fabs(c.signal) > 0.3f) s.signalling += 1;
        if (c.camo > 0.3f) s.camouflaged += 1;
        if (c.pheromone > 0.1f) s.marking += 1;
        s.speed += c.vel.length();
        for (int g = 0; g < G_COUNT; ++g) {
            float v = c.genome.g[g];
            sum[g] += v;
            sum2[g] += double(v) * v;
            int bin = std::min(TRAIT_BINS - 1, int(v * TRAIT_BINS));
            if (h[g][bin] < 65535) ++h[g][bin];
        }
    }
    s.population = float(n);
    s.species = float(species.aliveCount());
    double plants = 0;
    for (float p : terrain.plant) plants += p;
    s.plants = float(plants);
    double meat = 0;
    for (const Corpse& m : corpses) meat += m.energy;
    s.meat = float(meat);
    s.births = float(stats.birthsAcc);
    s.deaths = float(stats.deathsAcc);
    stats.birthsAcc = stats.deathsAcc = 0;
    s.maxGeneration = float(maxGeneration);
    s.climate = climate();
    if (n > 0) {
        s.meanGeneration = float(gen / n);
        s.connections = float(conn / n);
        s.meanEnergy = float(energy / n);
        s.toxic = float(toxic) / float(n);
        s.sharing /= float(n);
        s.signalling /= float(n);
        s.marking /= float(n);
        s.camouflaged /= float(n);
        if (s.infected > 0) s.strainAggression /= s.infected;
        s.speed /= float(n);
        int attacks = stats.attacksIntra + stats.attacksInter;
        s.attackRate = float(attacks) / float(n) * 100.f / float(std::max(1, stats.interval));
        s.cannibalism = attacks ? float(stats.attacksIntra) / float(attacks) : 0.f;
        s.toxicTargeted = attacks ? float(stats.attacksToxic) / float(attacks) : 0.f;
        s.packHunting = attacks ? float(stats.attacksPack) / float(attacks) : 0.f;
        const int contests = stats.contestDD + stats.contestHD + stats.contestHH;
        s.contests = float(contests) * 100.f / float(std::max(1, stats.interval));
        if (contests > 0) {
            s.doveDove = float(stats.contestDD) / float(contests);
            s.hawkDove = float(stats.contestHD) / float(contests);
            s.hawkHawk = float(stats.contestHH) / float(contests);
            // Each contest has two contestants: DD has 0 hawks, HD one, HH two.
            s.hawkShare = (float(stats.contestHD) + 2.f * float(stats.contestHH)) / (2.f * float(contests));
        }
        for (int g = 0; g < G_COUNT; ++g) {
            double mean = sum[g] / n;
            s.geneMean[g] = float(mean);
            s.geneStd[g] = float(std::sqrt(std::max(0.0, sum2[g] / n - mean * mean)));
        }
    }
    // Reset the per-interval behaviour counters only after they have been read above.
    stats.attacksIntra = stats.attacksInter = stats.attacksToxic = stats.attacksPack = 0;
    stats.contestDD = stats.contestHD = stats.contestHH = 0;
    stats.push(s, h);
    checkAchievements(s);
}

// ---------------------------------------------------------------------------
// God tools
// ---------------------------------------------------------------------------
int World::spawnRandom(Vec2 at, int count, float radius, float dietMax) {
    Genome base = Genome::random(rng_, dietMax);
    uint32_t sid = species.createRoot(base, tickCount, nextId_);
    int spawned = 0;
    for (int k = 0; k < count; ++k) {
        Genome g = base;
        mutate(g, rng_, 1.f);
        Vec2 p = at;
        for (int tries = 0; tries < 12; ++tries) {
            float a = rng_.range(0.f, TAU), r = radius * std::sqrt(rng_.uniform());
            p = at + Vec2(std::cos(a), std::sin(a)) * r;
            p.x = clampf(p.x, 10.f, terrain.width - 10.f);
            p.y = clampf(p.y, 10.f, terrain.height - 10.f);
            if (!terrain.water[terrain.index(p.x, p.y)]) break;
        }
        Creature c = makeCreature(g, p, sid, 0, 0, 0.f, 1.f);
        c.energy = c.maxEnergy * 0.7f;
        species.onBirth(sid);
        creatures.push_back(c);
        ++spawned;
    }
    log(EventKind::Speciation, sid, at, "New lineage " + species.get(sid).name() + " introduced");
    return spawned;
}

int World::spawnPredators(Vec2 at, int count, float radius) {
    Genome base = Genome::predator(rng_);
    uint32_t sid = species.createRoot(base, tickCount, nextId_);
    for (int k = 0; k < count; ++k) {
        Genome g = base;
        mutate(g, rng_, 1.f);
        float a = rng_.range(0.f, TAU), r = radius * std::sqrt(rng_.uniform());
        Vec2 p = at + Vec2(std::cos(a), std::sin(a)) * r;
        p.x = clampf(p.x, 10.f, terrain.width - 10.f);
        p.y = clampf(p.y, 10.f, terrain.height - 10.f);
        Creature c = makeCreature(g, p, sid, 0, 0, 0.f, 1.f);
        c.energy = c.maxEnergy * 0.8f;
        species.onBirth(sid);
        creatures.push_back(c);
    }
    return count;
}

int World::spawnGenome(const Genome& g, uint32_t speciesHint, Vec2 at, int count, float radius) {
    int spawned = 0;
    uint32_t root = 0;
    if (!species.valid(speciesHint)) {
        root = species.createRoot(g, tickCount, nextId_);
        log(EventKind::Speciation, root, at, "Designed lineage " + species.get(root).name() + " released");
    }
    for (int k = 0; k < count; ++k) {
        float a = rng_.range(0.f, TAU), r = radius * std::sqrt(rng_.uniform());
        Vec2 p = at + Vec2(std::cos(a), std::sin(a)) * r;
        p.x = clampf(p.x, 10.f, terrain.width - 10.f);
        p.y = clampf(p.y, 10.f, terrain.height - 10.f);
        bool created = false;
        uint32_t sid = root ? root : species.assign(g, speciesHint, tickCount, nextId_, created);
        Creature c = makeCreature(g, p, sid, 0, 0, 0.f, 1.f);
        c.energy = c.maxEnergy * 0.7f;
        species.onBirth(sid);
        creatures.push_back(c);
        ++spawned;
    }
    return spawned;
}

int World::killRadius(Vec2 at, float radius, DeathCause cause) {
    int killed = 0;
    for (Creature& c : creatures)
        if (c.alive && (c.pos - at).lengthSq() < sq(radius + c.radius)) {
            kill(c, cause);
            ++killed;
        }
    resolveDeathsAndBirths();
    return killed;
}

void World::meteor(Vec2 at, float radius) {
    int killed = killRadius(at, radius, DeathCause::Meteor);
    const float r2 = sq(radius * 1.4f);
    int c0 = std::max(0, int((at.x - radius * 1.4f) / terrain.cellSize));
    int c1 = std::min(terrain.cols - 1, int((at.x + radius * 1.4f) / terrain.cellSize));
    int r0 = std::max(0, int((at.y - radius * 1.4f) / terrain.cellSize));
    int r1 = std::min(terrain.rows - 1, int((at.y + radius * 1.4f) / terrain.cellSize));
    for (int y = r0; y <= r1; ++y)
        for (int x = c0; x <= c1; ++x) {
            Vec2 cc{(x + 0.5f) * terrain.cellSize, (y + 0.5f) * terrain.cellSize};
            if ((cc - at).lengthSq() < r2) {
                int i = y * terrain.cols + x;
                terrain.plant[i] = 0.f;
                terrain.pheromone[i] = 0.f;
            }
        }
    pushFx(FxEvent::Meteor, at, {1.f, 0.6f, 0.2f, 1.f}, radius);
    log(EventKind::Catastrophe, 0, at, "Meteor impact killed " + std::to_string(killed) + " creatures");
}

void World::feed(Vec2 at, float radius, float amount) {
    const float cap = params.plantCapacity;
    int c0 = std::max(0, int((at.x - radius) / terrain.cellSize));
    int c1 = std::min(terrain.cols - 1, int((at.x + radius) / terrain.cellSize));
    int r0 = std::max(0, int((at.y - radius) / terrain.cellSize));
    int r1 = std::min(terrain.rows - 1, int((at.y + radius) / terrain.cellSize));
    for (int y = r0; y <= r1; ++y)
        for (int x = c0; x <= c1; ++x) {
            Vec2 cc{(x + 0.5f) * terrain.cellSize, (y + 0.5f) * terrain.cellSize};
            float d = (cc - at).length();
            if (d > radius) continue;
            int i = y * terrain.cols + x;
            if (terrain.water[i]) continue;
            float f = 1.f - d / radius;
            terrain.plant[i] = clampf(terrain.plant[i] + amount * cap * f, 0.f, cap);
        }
}

void World::terraform(Vec2 at, float radius, float delta) {
    int c0 = std::max(0, int((at.x - radius) / terrain.cellSize));
    int c1 = std::min(terrain.cols - 1, int((at.x + radius) / terrain.cellSize));
    int r0 = std::max(0, int((at.y - radius) / terrain.cellSize));
    int r1 = std::min(terrain.rows - 1, int((at.y + radius) / terrain.cellSize));
    for (int y = r0; y <= r1; ++y)
        for (int x = c0; x <= c1; ++x) {
            Vec2 cc{(x + 0.5f) * terrain.cellSize, (y + 0.5f) * terrain.cellSize};
            float d = (cc - at).length();
            if (d > radius) continue;
            int i = y * terrain.cols + x;
            float f = 1.f - sq(d / radius);
            terrain.elevation[i] = clamp01(terrain.elevation[i] + delta * f);
            terrain.refreshCell(i);
            if (terrain.water[i]) terrain.plant[i] = 0.f;
        }
    ++terrain.version;
}

void World::plague(uint32_t speciesId, float fraction) {
    if (!species.valid(speciesId) || !species.get(speciesId).alive()) {
        int best = 0;
        for (const Species& s : species.all())
            if (s.alive() && s.population > best) { best = s.population; speciesId = s.id; }
    }
    if (!species.valid(speciesId)) return;
    int infected = 0;
    for (Creature& c : creatures)
        if (c.alive && c.speciesId == speciesId && c.infection <= 0.f && rng_.chance(fraction)) {
            c.infection = float(params.diseaseDuration) * rng_.range(0.7f, 1.3f);
            c.strain = rng_.range(0.3f, 0.7f);
            c.immuneTicks = 0.f;
            c.flags |= CF_SICK;
            ++infected;
        }
    log(EventKind::Catastrophe, speciesId, species.get(speciesId).centroid,
        "Epidemic breaks out among " + species.get(speciesId).name() + " (" + std::to_string(infected) + " infected)");
}

int World::infect(Vec2 at, float radius) {
    int n = 0;
    for (Creature& c : creatures)
        if (c.alive && c.infection <= 0.f && (c.pos - at).lengthSq() < sq(radius + c.radius)) {
            c.infection = float(params.diseaseDuration) * rng_.range(0.7f, 1.3f);
            c.strain = rng_.range(0.3f, 0.7f);
            c.immuneTicks = 0.f;
            c.flags |= CF_SICK;
            ++n;
        }
    if (n) log(EventKind::Catastrophe, 0, at, "A pathogen is released: " + std::to_string(n) + " infected");
    return n;
}

void World::climateShock(float delta, const char* label) {
    climateEvent_ += delta;
    log(EventKind::Catastrophe, 0, {}, label);
}

void World::bloom() {
    for (size_t i = 0; i < terrain.plant.size(); ++i)
        terrain.plant[i] = std::max(terrain.plant[i], terrain.fertility[i] * params.plantCapacity);
    log(EventKind::Catastrophe, 0, {}, "A great bloom covers the land");
}

void World::drought() {
    for (float& p : terrain.plant) p *= 0.15f;
    log(EventKind::Catastrophe, 0, {}, "Drought withers 85% of all plants");
}

std::string World::newick() const {
    const auto& all = species.all();
    std::vector<std::vector<uint32_t>> kids(all.size() + 1);
    for (const Species& s : all) kids[s.parentId].push_back(s.id);
    auto clean = [](std::string n) {
        for (char& ch : n)
            if (ch == ' ' || ch == '(' || ch == ')' || ch == ',' || ch == ':' || ch == ';') ch = '_';
        return n;
    };
    const double yl = params.yearLength;
    std::function<std::string(uint32_t)> emit = [&](uint32_t id) -> std::string {
        const Species& s = all[id - 1];
        std::string out;
        if (!kids[id].empty()) {
            out += "(";
            for (size_t k = 0; k < kids[id].size(); ++k) {
                if (k) out += ",";
                out += emit(kids[id][k]);
            }
            out += ")";
        }
        const int64_t end = s.alive() ? tickCount : s.extinctTick;
        const int64_t start = s.parentId ? s.bornTick : 0;
        char len[32];
        std::snprintf(len, sizeof len, ":%.3f", double(std::max<int64_t>(0, end - start)) / yl);
        return out + clean(s.name()) + (s.alive() ? "" : "_extinct") + len;
    };
    std::string tree = "(";
    for (size_t k = 0; k < kids[0].size(); ++k) {
        if (k) tree += ",";
        tree += emit(kids[0][k]);
    }
    tree += ")root;\n";
    return tree;
}

int World::indexOfId(uint32_t id) const {
    if (!id) return -1;
    for (int i = 0; i < int(creatures.size()); ++i)
        if (creatures[i].id == id) return i;
    return -1;
}

int World::pick(Vec2 p, float slack) const {
    int best = -1;
    float bestD = 1e18f;
    for (int i = 0; i < int(creatures.size()); ++i) {
        const Creature& c = creatures[i];
        float d = (c.pos - p).lengthSq();
        if (d < sq(c.radius + slack) && d < bestD) { bestD = d; best = i; }
    }
    return best;
}

} // namespace pm
