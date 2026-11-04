#pragma once
#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/Math.hpp"
#include "core/Random.hpp"
#include "sim/Genome.hpp"
#include "sim/Species.hpp"
#include "sim/Stats.hpp"

namespace pm {

class ThreadPool;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
enum class TerrainPreset : int { Continents, Archipelago, Pangaea, Lakes, Flatland };

struct WorldSettings {
    uint64_t seed = 1;
    float width = 4096.f, height = 4096.f;
    float cellSize = 16.f;
    TerrainPreset preset = TerrainPreset::Continents;
    float waterFraction = 0.32f;   // share of the map under water
    float featureScale = 1.0f;     // bigger = larger landmasses
    float moisture = 0.0f;         // -0.5 .. 0.5 global moisture bias
    float regionContrast = 0.6f;   // 0 = uniform land; 1 = strong oases and badlands (distinct regional niches)
    float temperature = 0.0f;      // -0.5 .. 0.5 global temperature bias
    int initialPopulation = 900;
    int foundingLineages = 4;
    int predatorLineages = 1;      // carnivore founders with hunting instincts
    float predatorShare = 0.08f;   // fraction of the initial population that hunts
    float initialDietMax = 0.25f;  // 0.25 = herbivores, 1 = anything
};

struct SimParams {
    // Environment
    float plantGrowth = 1.0f;
    float plantCapacity = 13.0f;
    float plantEnergy = 1.0f;
    int yearLength = 12000;
    float seasonStrength = 1.0f;
    float climateOffset = 0.0f;
    float glacialCycleYears = 0.f;  // 0 = off; period of slow climate oscillation
    float glacialAmplitude = 0.4f;
    bool dynamicSeaLevel = true;    // cold climates lock water in ice and expose land bridges
    bool weather = true;            // drifting rain fronts boost plant growth where they pass
    bool camouflage = true;         // colours matching the ground are harder to spot
    bool foodContests = true;       // hawk-dove payoffs when two creatures graze the same patch
    bool hybridization = true;      // close sister species can occasionally interbreed
    float windSpeed = 1.f;
    float meatDecay = 0.0012f;
    float pheromoneDecay = 0.985f;
    bool dayNight = true;
    int dayLength = 3000;           // ticks per day/night cycle
    // Metabolism
    float metabolism = 1.0f;
    float moveCost = 1.0f;
    float brainCost = 1.0f;
    float visionCost = 1.0f;
    float thermalCost = 1.0f;
    // Evolution
    float mutationRate = 1.0f;
    float speciesThreshold = 0.8f;
    float specialization = 1.4f;
    bool sexualReproduction = true;
    // Population
    int maxPopulation = 12000;
    float shocksPerYear = 1.0f;     // random catastrophes and windfalls (meteor, drought, volcano, bloom, climate, plague)
    int thinkEvery = 2;             // each brain runs every Nth tick (outputs persist in between): the big speed lever
    bool autoReseed = true;
    int reseedBelow = 30;
    int reseedCount = 200;
    bool predatorImmigration = true; // new hunters arrive if predators die out
    bool thirst = true;             // creatures must drink (or eat lush grass) to stay hydrated
    // Disease
    bool epidemics = true;          // spontaneous outbreaks
    float outbreaksPerYear = 0.2f;
    float transmission = 0.05f;     // infection chance per tick of contact
    float virulence = 1.0f;
    int diseaseDuration = 1800;     // ticks until recovery
    int immunityDuration = 9000;    // ticks of immunity after recovery
    float crossSpecies = 0.2f;      // transmission multiplier between species
    float toxinCost = 1.0f;         // upkeep multiplier for poison glands
    float shareEfficiency = 0.85f;  // fraction of shared energy that arrives
};

// ---------------------------------------------------------------------------
// Terrain: elevation, moisture, temperature, plant biomass and pheromone on a grid.
// ---------------------------------------------------------------------------
struct Terrain {
    int cols = 0, rows = 0;
    float cellSize = 16.f, width = 0.f, height = 0.f;
    float seaLevel = 0.4f; // current elevation threshold
    float baseSeaLevel = 0.4f;
    std::vector<float> elevation, moisture, fertility, climateBase, baseTemp, plant, pheromone;
    std::vector<float> tilled;        // 0..1: how well-tended each cell is (niche construction), boosts plant growth
    std::vector<uint32_t> tiller;     // species that last tended the cell
    std::vector<float> plantDef, plantSpr; // evolving flora: chemical defence and seed dispersal of each cell's plants
    std::vector<uint8_t> water;
    static constexpr int RAIN_TILE = 64;
    std::vector<float> rainTile; // tileable rain-cloud pattern, RAIN_TILE x RAIN_TILE
    uint32_t version = 0; // bumped when the static layers change (terraforming)

    void generate(const WorldSettings& ws);
    void refreshCell(int idx); // recompute water/fertility after an elevation edit
    int index(float x, float y) const {
        int cx = int(x / cellSize), cy = int(y / cellSize);
        cx = cx < 0 ? 0 : (cx >= cols ? cols - 1 : cx);
        cy = cy < 0 ? 0 : (cy >= rows ? rows - 1 : cy);
        return cy * cols + cx;
    }
    bool inside(float x, float y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    float latitude(int row) const { return (float(row) + 0.5f) / float(rows) * 2.f - 1.f; } // -1 north .. 1 south
};

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------
enum CreatureFlags : uint32_t {
    CF_EATING = 1u << 0,
    CF_ATTACKING = 1u << 1,
    CF_SHARING = 1u << 2,
    CF_IN_WATER = 1u << 3,
    CF_HURT = 1u << 4,
    CF_MATED = 1u << 5,
    CF_POISONED = 1u << 6,
    CF_HIT = 1u << 7, // landed an attack this tick
    CF_BLINK = 1u << 12, // teleported this tick
    CF_SICK = 1u << 10,
    CF_IMMUNE = 1u << 11,
};

enum class DeathCause : uint8_t { None, Starvation, OldAge, Predation, Poison, Meteor, Smite, Plague, Thirst };
const char* deathCauseName(DeathCause c);

struct Creature {
    uint32_t id = 0, parentId = 0, speciesId = 0, generation = 0;
    Vec2 pos, vel;
    float angle = 0.f;
    float ca = 1.f, sa = 0.f; // cos/sin of angle, refreshed whenever the angle changes (saves libm calls)
    float energy = 0.f, health = 0.f;
    float age = 0.f, growth = 1.f;
    float radius = 10.f, mass = 1.f, mass075 = 1.f, maxEnergy = 100.f, maxHealth = 50.f;
    float clockPhase = 0.f, animPhase = 0.f;
    float pain = 0.f, signal = 0.f, pheromone = 0.f;
    float call[2]{};         // voice channels A and B this tick (0..1); their meaning is not assigned, it evolves
    float tend = 0.f;        // effort spent cultivating the ground this tick
    float lastEnergy = 0.f;  // for the learning reward signal
    int blinkCd = 0;         // ticks until the next blink is possible
    float linkN = 0.f, linkSync = 0.f; // linked kin this tick, and how in step their thoughts are
    float temperature = 0.f;
    float hydration = 1.f;   // 0 = parched, 1 = fully watered
    float infection = 0.f;   // ticks of illness remaining
    float strain = 0.5f;     // aggressiveness of the pathogen strain it carries (evolves on transmission)
    float immuneTicks = 0.f; // ticks of acquired immunity remaining
    float shared = 0.f;      // energy given away over its life
    float camo = 0.f;        // how well it blends into the ground right now (0..0.6)
    float geneDelta[G_COUNT]{}; // genome difference from its (first) parent at birth
    uint16_t synapsesChanged = 0;
    bool sexual = false;     // conceived with a mate (vs. cloned)
    int reproCooldown = 0;
    uint32_t flags = 0;
    uint32_t children = 0, kills = 0;
    float eatenPlant = 0.f, eatenMeat = 0.f;
    int64_t birthTick = 0;
    uint32_t lastAttacker = 0;
    uint8_t hitsThisTick = 0; // attackers that bit this creature this tick (pack hunting)
    DeathCause cause = DeathCause::None;
    bool alive = true;
    Phenotype ph{};
    float in[N_INPUTS]{};
    float hid[N_HIDDEN]{};
    float learn[N_LEARN]{};  // lifetime-learned offsets to the hidden->output weights
    float out[N_OUTPUTS]{};
    Rng rng;
    Genome genome;
};

struct Corpse {
    Vec2 pos;
    float energy = 0.f, initial = 0.f;
    float radius = 4.f;
    float hue = 0.f;
    float toxin = 0.f;
    uint32_t age = 0;
};

// Uniform grid with cell-sorted copies of hot fields (cache-friendly neighbour scans).
struct SpatialGrid {
    float cell = 64.f;
    int cols = 1, rows = 1;
    std::vector<int> start, idx, cellOf, fill;
    std::vector<float> x, y, r, hue, signal, mass, angle, cosA, sinA, camo, callA, callB;
    std::vector<float> hid; // N_HIDDEN floats per point, for the neural link
    void resize(float w, float h, float cellSize);
    // Counting sort of n points into cells; fills idx/x/y. Extra fields are filled by the caller.
    template <class GetPos>
    void build(int n, GetPos&& getPos) {
        const int ncell = cols * rows;
        std::fill(start.begin(), start.end(), 0);
        cellOf.resize(n);
        idx.resize(n);
        x.resize(n);
        y.resize(n);
        for (int i = 0; i < n; ++i) {
            Vec2 p = getPos(i);
            int cx = int(p.x / cell), cy = int(p.y / cell);
            cx = cx < 0 ? 0 : (cx >= cols ? cols - 1 : cx);
            cy = cy < 0 ? 0 : (cy >= rows ? rows - 1 : cy);
            cellOf[i] = cy * cols + cx;
            ++start[cellOf[i] + 1];
        }
        for (int c = 0; c < ncell; ++c) start[c + 1] += start[c];
        fill.assign(start.begin(), start.end() - 1);
        for (int i = 0; i < n; ++i) {
            int k = fill[cellOf[i]]++;
            idx[k] = i;
            Vec2 p = getPos(i);
            x[k] = p.x;
            y[k] = p.y;
        }
    }
    template <class F>
    void query(float px, float py, float radius, F&& f) const {
        int x0 = int((px - radius) / cell), x1 = int((px + radius) / cell);
        int y0 = int((py - radius) / cell), y1 = int((py + radius) / cell);
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
        x1 = x1 >= cols ? cols - 1 : x1; y1 = y1 >= rows ? rows - 1 : y1;
        const float r2 = radius * radius;
        for (int cy = y0; cy <= y1; ++cy) {
            const float ny = std::max(0.f, std::max(float(cy) * cell - py, py - float(cy + 1) * cell));
            for (int cx = x0; cx <= x1; ++cx) {
                // Skip cells whose nearest point is outside the search circle (square -> circle: ~22% fewer cells).
                const float nx = std::max(0.f, std::max(float(cx) * cell - px, px - float(cx + 1) * cell));
                if (nx * nx + ny * ny > r2) continue;
                int c = cy * cols + cx;
                for (int k = start[c], e = start[c + 1]; k < e; ++k) f(k);
            }
        }
    }
};

struct FxEvent {
    enum Kind : uint8_t { Birth, Death, Hit, Speciation, Meteor, Poison, Sick, Gift, Love } kind;
    Vec2 pos;
    Color color;
    float size;
};

struct AchievementInfo {
    const char* name;
    const char* description;
};
int achievementCount();
const AchievementInfo& achievementInfo(int i);

// Rolling per-species tallies the behaviour detector (Discoveries.cpp) turns into journal entries.
struct BehaviorAcc {
    static constexpr int CTX = 6; // hurt, sees a bigger creature, eating, ready to breed, starving, attacking
    static constexpr int ACT = 4; // call A, call B, overclock, phase shift
    float n = 0;                   // creature-samples
    float nCtx[CTX]{};
    float acting[ACT]{};           // samples performing each action
    float actingIn[ACT][CTX]{};
    float nHear[2]{}, spdHear[2]{}, nQuiet[2]{}, spdQuiet[2]{};
    float bites = 0, bitesOwn = 0, bitesOther = 0; // grazing bites by whose soil they came from
    float tenders = 0;
    float blinks = 0, blinkHurt = 0, blinkAttack = 0; // teleports and what the blinker was doing
    float linkN = 0, linkSync = 0, linkers = 0;       // neural-link partners and thought synchrony
    uint32_t otherTiller = 0;      // species whose fields were poached most recently
};

// What each species is good at, refreshed at every census (see Discoveries.cpp).
constexpr int N_SKILLS = 12;
const char* skillName(int i);
struct SpeciesSkills {
    bool valid = false;            // enough living adults to rank
    float raw[N_SKILLS]{};
    float z[N_SKILLS]{};           // standard score against the other living species
    int best[3] = {-1, -1, -1};    // standout skills, strongest first
};

struct TickTimings {
    double grid = 0, sense = 0, interact = 0, move = 0, env = 0, book = 0, total = 0;
};

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
class World {
public:
    explicit World(ThreadPool* pool);
    void setPool(ThreadPool* pool) { pool_ = pool; }

    void create(const WorldSettings& ws);
    void tick();

    // God tools ------------------------------------------------------------
    int spawnRandom(Vec2 at, int count, float radius, float dietMax = 0.3f);
    int spawnPredators(Vec2 at, int count, float radius);
    int spawnGenome(const Genome& g, uint32_t speciesHint, Vec2 at, int count, float radius);
    int killRadius(Vec2 at, float radius, DeathCause cause);
    void meteor(Vec2 at, float radius);
    void feed(Vec2 at, float radius, float amount);
    void terraform(Vec2 at, float radius, float delta);
    void plague(uint32_t speciesId, float fraction); // seed an epidemic
    int infect(Vec2 at, float radius);
    void climateShock(float delta, const char* label);
    void bloom();
    void drought();

    // Queries ---------------------------------------------------------------
    int indexOfId(uint32_t id) const;
    int pick(Vec2 p, float slack) const;
    float temperatureAtCell(int idx) const;
    float temperatureAt(Vec2 p) const { return temperatureAtCell(terrain.index(p.x, p.y)); }
    Creature preview(const Genome& g) const; // an adult built from g, not added to the world
    float seasonPhase() const; // 0..1 through the year
    float climate() const;
    void volcano(Vec2 at, float radius);
    float seasonSin() const;
    float daylight() const; // 1 = noon, 0 = midnight
    float rainAt(float x, float y) const; // 0..1 rain intensity right now
    Vec2 rainOffset() const;               // cloud drift, in tile units
    float rainScale() const { return terrain.width / 1.6f; } // world units per tile repeat
    float camouflageOf(const Creature& c) const;
    const char* seasonName(bool north) const;
    double year() const { return double(tickCount) / double(params.yearLength); }

    // Persistence -----------------------------------------------------------
    bool save(const std::string& path, std::string& err) const;
    bool load(const std::string& path, std::string& err);
    bool saveStream(std::ostream& out, std::string& err) const;
    bool loadStream(std::istream& in, std::string& err);
    std::string snapshot() const;                         // whole world as bytes (for rewind)
    std::string newick() const;                           // tree of life in Newick format (years)
    bool restore(const std::string& data, std::string& err);

    // Public state (read by renderer/UI) ------------------------------------
    WorldSettings settings;
    SimParams params;
    Terrain terrain;
    std::vector<Creature> creatures;
    std::vector<Corpse> corpses;
    SpeciesTracker species;
    Stats stats;
    EventLog events;
    Records records;
    uint64_t achieved = 0;              // bit per achievement
    int64_t achievedTick[64] = {};
    std::vector<FxEvent> fx;
    bool fxEnabled = true;
    int64_t tickCount = 0;
    uint32_t maxGeneration = 0;
    TickTimings timings;
    // The UI watches one creature so it can report how it died.
    uint32_t watchId = 0;
    DeathCause watchCause = DeathCause::None;
    int64_t watchDeathTick = -1;
    float maxPlantCap() const { return params.plantCapacity; }
    std::vector<SpeciesSkills> skills;  // indexed by species id - 1
    const SpeciesSkills* skillsOf(uint32_t speciesId) const {
        return speciesId >= 1 && speciesId <= skills.size() && skills[speciesId - 1].valid ? &skills[speciesId - 1] : nullptr;
    }
    std::string bestAtText(uint32_t speciesId) const; // e.g. "Hunting, Speed, Brainpower" or "no standout skill"

private:
    void buildGrids();
    void senseAndThink(int i);
    void interact();
    void moveAndMetabolize(int i);
    void resolveDeathsAndBirths();
    void updateEnvironment();
    void census();
    void sampleStats();
    void updateBody(Creature& c) const;
    Creature makeCreature(const Genome& g, Vec2 pos, uint32_t speciesId, uint32_t parentId, uint32_t generation,
                          float energy, float growth);
    void reproduce(Creature& c);
    void kill(Creature& c, DeathCause cause);
    Vec2 randomLand(Rng& r, float minFertility) const;
    void log(EventKind kind, uint32_t speciesId, Vec2 pos, std::string text);
    void pushFx(FxEvent::Kind k, Vec2 p, Color c, float size);
    void refreshPhenotypes();
    void updateSeaLevel();
    void unlock(int id);
    void checkAchievements(const StatSample& s);
    void considerRecords(const Creature& c);
    // Discoveries.cpp: notice behaviours nobody scripted and write them into the event log.
    void scanBehaviours();
    void randomShock();
    void discover(uint8_t tag, uint32_t speciesId, Vec2 pos, std::string text, uint32_t bucket = 0);
    BehaviorAcc& behaviorOf(uint32_t speciesId);

    ThreadPool* pool_;
    Rng rng_;
    uint32_t nextId_ = 1;
    SpatialGrid cgrid_, mgrid_; // creatures, meat
    std::vector<Creature> newborns_;
    std::vector<int> order_;
    std::vector<int> cellEater_;     // last creature index to graze each cell this tick
    std::vector<int64_t> cellEaterTick_;
    float maxRadius_ = 18.f;
    float climateEvent_ = 0.f;
    float lastSpecialization_ = -1.f;
    uint32_t nextGenMilestone_ = 10;
    int lastPopRecord_ = 0;
    int64_t noPredatorsSince_ = -1;
    int lowestPopulation_ = 1 << 30;
    uint64_t lastHybridPair_ = 0;
    std::vector<int64_t> recentExtinctions_;
    std::vector<BehaviorAcc> behav_;                 // indexed by species id - 1 (transient)
    std::unordered_set<uint64_t> announced_;          // (species, bucket, tag) already in the journal
    std::vector<std::array<float, G_COUNT>> prevGenes_; // species mean genes one year ago
    int64_t lastGeneYear_ = -1;
    std::vector<float> plantDef2_, plantSpr2_; // scratch buffers for the flora takeover pass
    float recentPeak_ = 0.f;
};

} // namespace pm
