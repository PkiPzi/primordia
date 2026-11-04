#pragma once
#include <array>
#include <cstdint>
#include <string>

#include "core/Random.hpp"

namespace pm {

// ---------------------------------------------------------------------------
// Body genes. Every gene is stored normalised to [0,1] and mapped to a
// phenotype range in Phenotype::from().
// ---------------------------------------------------------------------------
enum Gene : int {
    G_SIZE,
    G_SPEED,
    G_DIET,        // 0 = strict herbivore, 1 = strict carnivore
    G_ARMOR,
    G_WEAPON,
    G_TOXIN,       // poison: hurts whoever bites this creature or its corpse
    G_IMMUNITY,    // resistance to infectious disease
    G_NIGHT_VISION,// sight in the dark (and eye-shine)
    G_VISION_RANGE,
    G_VISION_FOV,
    G_HUE,         // visible colour, circular
    G_PATTERN,     // visible markings
    G_THERMAL,     // preferred temperature
    G_MUT_RATE,    // meta: per-locus mutation probability
    G_MUT_SCALE,   // meta: mutation step size
    G_INVEST,      // fraction of energy handed to offspring
    G_MATURITY,    // time to adulthood
    G_LONGEVITY,   // lifespan vs. upkeep
    G_LITTER,      // offspring per birth
    G_SEXUALITY,   // chance to recombine with a mate
    G_MATE_PREF,   // preferred partner colour (hue, circular)
    G_CHOOSY,      // how strictly that preference is enforced
    G_CLOCK,       // internal oscillator frequency
    G_PLASTICITY,  // lifetime learning rate (costs upkeep)
    G_LINK,        // strength of the neural link to nearby kin (alien: brains share thoughts)
    G_DETOX,       // tolerance of plant chemical defences (costs upkeep)
    G_COUNT
};

struct GeneInfo {
    const char* name;
    const char* description;
    float distanceWeight; // contribution to genetic distance between genomes
};

extern const GeneInfo* const GENE_INFO; // G_COUNT entries

// ---------------------------------------------------------------------------
// Brain layout: recurrent net, inputs -> hidden(recurrent) -> outputs, plus
// direct input -> output links. Connections with weight exactly 0 are absent;
// structural mutation adds and prunes them (NEAT-like complexification while
// keeping dense, SIMD-friendly evaluation).
// ---------------------------------------------------------------------------
constexpr int NUM_EYES = 5;
enum EyeChannel : int { EC_PLANT, EC_MEAT, EC_CREATURE, EC_SIZE, EC_KIN, EC_OBSTACLE, EC_COUNT };

enum Input : int {
    IN_EYES = 0,
    IN_ENERGY = NUM_EYES * EC_COUNT,
    IN_HEALTH,
    IN_AGE,
    IN_SPEED,
    IN_WATER,
    IN_TEMP,
    IN_GRASS,
    IN_PAIN,
    IN_CLOCK,
    IN_SIGNAL,
    IN_TOUCH,
    IN_SICK,       // currently infected
    IN_LIGHT,      // daylight level
    IN_FLOCK,      // turn needed to match neighbours' heading (-1 left .. 1 right)
    IN_CROWD,      // how crowded the surroundings are
    IN_THIRST,     // 0 = fully hydrated, 1 = parched
    IN_WATER_DIR,  // direction to the nearest water: -1 left .. 1 right (0 = none in sight)
    IN_SMELL,      // pheromone concentration here
    IN_SMELL_DIR,  // pheromone gradient: right minus left
    IN_HEAR_A,     // loudness of nearby call A (meaning is not assigned: it evolves)
    IN_HEAR_A_DIR, // where call A comes from: -1 left .. 1 right
    IN_HEAR_B,
    IN_HEAR_B_DIR,
    IN_TENDED,     // how well-tended the soil underfoot is
    IN_LINKED,     // how many kin are neurally linked to this creature (0..1)
    N_INPUTS
};

enum Output : int { OUT_THRUST, OUT_TURN, OUT_EAT, OUT_ATTACK, OUT_REPRODUCE, OUT_SIGNAL, OUT_SHARE, OUT_PHEROMONE, OUT_CALL_A, OUT_CALL_B, OUT_TEND, OUT_BLINK, OUT_OVERCLOCK, OUT_PHASE, N_OUTPUTS };

constexpr int N_HIDDEN = 16;
constexpr int N_LEARN = N_HIDDEN * int(N_OUTPUTS); // lifetime-learned hidden->output offsets (not inherited as genes)

// Weights are stored input-major ([source][target]) so evaluation is a series of
// contiguous 16-wide (hidden) or 8-wide (output) multiply-adds, skipping silent inputs.
constexpr int OFF_IH = 0;                                  // [i][h]
constexpr int OFF_HH = OFF_IH + N_HIDDEN * int(N_INPUTS);       // [from][to]
constexpr int OFF_BH = OFF_HH + N_HIDDEN * N_HIDDEN;       // [h]
constexpr int OFF_HO = OFF_BH + N_HIDDEN;                  // [h][o]
constexpr int OFF_IO = OFF_HO + int(N_OUTPUTS) * N_HIDDEN;      // [i][o]
constexpr int OFF_BO = OFF_IO + int(N_OUTPUTS) * int(N_INPUTS);      // [o]
constexpr int BRAIN_SIZE = OFF_BO + N_OUTPUTS;

// Index of the weight for a connection (target first, source second).
constexpr int wIH(int h, int i) { return OFF_IH + i * N_HIDDEN + h; }
constexpr int wHH(int to, int from) { return OFF_HH + from * N_HIDDEN + to; }
constexpr int wHO(int o, int h) { return OFF_HO + h * int(N_OUTPUTS) + o; }
constexpr int wIO(int o, int i) { return OFF_IO + i * int(N_OUTPUTS) + o; }

const char* inputName(int i);   // e.g. "Eye3 Meat"
const char* outputName(int o);
const char* eyeChannelName(int c);

struct Genome {
    std::array<float, G_COUNT> g{};
    std::array<float, BRAIN_SIZE> w{};

    static Genome random(Rng& rng, float dietMax = 1.f);
    static Genome predator(Rng& rng); // carnivore founder with hunting instincts
    int connectionCount() const;
};

enum class Instinct : int { Graze, Hunt, Flee, Herd, Drink, FollowScent, CryOut, RestAtNight, ShareWithKin, Count };
const char* instinctName(Instinct i);
const char* instinctDescription(Instinct i);
void addInstinct(Genome& g, Instinct i); // wires a hand-made behaviour into the brain

std::string genomeToText(const Genome& g);                 // portable text (for the clipboard)
bool genomeFromText(const std::string& text, Genome& out);

void mutate(Genome& g, Rng& rng, float rateMultiplier);
Genome crossover(const Genome& a, const Genome& b, Rng& rng);
float bodyDistance(const Genome& a, const Genome& b);
float geneticDistance(const Genome& a, const Genome& b);

// ---------------------------------------------------------------------------
// Phenotype: genes mapped to physical quantities, cached per creature.
// ---------------------------------------------------------------------------
struct Phenotype {
    float adultRadius;
    float accel;        // max acceleration per tick
    float turnRate;     // max radians per tick
    float visionRange;  // world units
    float fov;          // total field of view, radians
    float diet, plantEff, meatEff;
    float armor, weapon, toxin, immunity, nightVision;
    float damage;       // health removed per tick of attack (before armour)
    float lifespan;     // ticks
    float maturity;     // ticks to adulthood
    int litter;
    float investment;
    float sexuality;
    float clockFreq;
    float thermalOpt;   // in normalised temperature units [-1, 1]
    float mutRate, mutScale;
    float longevity;
    float upkeep;       // fixed per-tick cost (vision + brain), before global multipliers
    float learnRate;    // lifetime learning rate
    float linkStrength; // coupling to linked kin's hidden state
    float cOff, sOff, cStep, sStep; // eye-ray rotations: first ray vs heading, and between neighbouring rays
    float detox;        // resistance to plant defences
    int connections;

    static Phenotype from(const Genome& g, float specialization);
};

} // namespace pm
