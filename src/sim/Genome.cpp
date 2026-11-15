#include "sim/Genome.hpp"

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace pm {

const GeneInfo GENE_INFO_TABLE[] = {
    {"Size", "Adult body radius. Bigger bodies store more energy and hit harder, but cost more to run.", 1.2f},
    {"Speed", "Muscle power. Faster creatures pay more energy per unit of thrust.", 1.0f},
    {"Diet", "0 = herbivore, 1 = carnivore. Generalists digest both foods poorly.", 2.0f},
    {"Armor", "Reduces damage taken. Adds mass and slows the creature down.", 1.0f},
    {"Weapon", "Jaws and claws. Raises attack damage and meat bite size.", 1.0f},
    {"Toxin", "Poison glands. Biting or eating a toxic creature hurts the attacker. Costs upkeep.", 1.0f},
    {"Immunity", "Resistance to infectious disease. Costs upkeep; pays off during epidemics.", 0.5f},
    {"Night vision", "How much sight survives in the dark. Large reflective eyes cost energy.", 0.6f},
    {"Vision range", "How far the eyes see. Long sight costs energy every tick.", 0.8f},
    {"Field of view", "Angle covered by the five eye sectors.", 0.8f},
    {"Hue", "Body colour. Visible to other creatures, so it can evolve into a kin signal.", 1.6f},
    {"Pattern", "Body markings: plain, stripes or spots.", 0.8f},
    {"Thermal optimum", "Preferred temperature. Distance from it costs energy.", 1.2f},
    {"Mutation rate", "Chance per locus to mutate in offspring (evolvable evolvability).", 0.3f},
    {"Mutation size", "Step size of mutations.", 0.3f},
    {"Investment", "Share of the parent's energy handed to its offspring.", 0.6f},
    {"Maturity", "Time needed to reach adult size and breed.", 0.6f},
    {"Longevity", "Longer life at a higher upkeep cost.", 0.6f},
    {"Litter", "Offspring per birth (1-4). Energy is split between them.", 0.6f},
    {"Sexuality", "Chance to recombine genes with a nearby mate of the same species.", 0.5f},
    {"Mate preference", "The partner colour this creature finds attractive. Drives sexual selection.", 0.9f},
    {"Choosiness", "How strictly mates must match the preferred colour. Choosy creatures may skip mating.", 0.5f},
    {"Clock", "Frequency of the internal oscillator neuron.", 0.3f},
    {"Plasticity", "Lifetime learning rate: brains rewire themselves toward whatever just paid off. Costs upkeep.", 0.4f},
    {"Neural link", "Couples this brain's hidden state to nearby kin, so neighbours partly think together. Costs upkeep.", 0.5f},
    {"Detox", "Tolerance of plant chemical defences. Costs upkeep; pays off where plants have evolved poisons.", 0.5f},
    {"Aquatic", "Built for water: swims fast and cheaply, feeds on algae and never thirsts at sea, but is slow, hungry and quick to dry out on land.", 1.2f},
    {"Wings", "Flight: faster, crosses water as if it were land and dodges bites from wingless attackers. Costly to carry and to flap; heavy bodies get little lift.", 1.2f},
    {"Quills", "Spines that hurt whoever bites this creature. They cost upkeep and slow the body down.", 0.8f},
    {"Photosynthesis", "Green skin that makes energy from daylight while the creature keeps still and uncrowded. Leafy bodies are slow and digest food badly.", 1.2f},
};

static_assert(std::size(GENE_INFO_TABLE) == G_COUNT, "one GeneInfo per gene");
const GeneInfo* const GENE_INFO = GENE_INFO_TABLE;

namespace {

std::string g_inputNames[N_INPUTS];
const char* g_outputNames[N_OUTPUTS] = {"Thrust", "Turn", "Eat", "Attack", "Reproduce", "Signal", "Share", "Pheromone", "Call A", "Call B", "Tend soil", "Blink", "Overclock", "Phase"};
const char* g_channelNames[EC_COUNT] = {"Plant", "Meat", "Creature", "Size", "Kin", "Obstacle"};

struct NameInit {
    NameInit() {
        for (int e = 0; e < NUM_EYES; ++e)
            for (int c = 0; c < EC_COUNT; ++c) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "Eye%d %s", e + 1, g_channelNames[c]);
                g_inputNames[e * EC_COUNT + c] = buf;
            }
        const char* rest[] = {"Energy", "Health", "Age", "Speed", "In water", "Temperature",
                              "Grass here", "Pain", "Clock", "Heard signal", "Touch", "Sick", "Daylight",
                              "Flock heading", "Crowding", "Thirst", "Water dir", "Smell", "Smell dir",
                              "Hear A", "Hear A dir", "Hear B", "Hear B dir", "Tended soil", "Linked kin"};
        static_assert(std::size(rest) == N_INPUTS - IN_ENERGY, "one name per scalar input");
        for (int i = IN_ENERGY; i < N_INPUTS; ++i) g_inputNames[i] = rest[i - IN_ENERGY];
    }
} g_nameInit;

float mutationRateOf(const Genome& g) { return 0.002f * std::pow(100.f, g.g[G_MUT_RATE]); }
float mutationScaleOf(const Genome& g) { return lerp(0.05f, 0.6f, g.g[G_MUT_SCALE]); }

} // namespace

const char* inputName(int i) { return g_inputNames[i].c_str(); }
const char* outputName(int o) { return g_outputNames[o]; }
const char* eyeChannelName(int c) { return g_channelNames[c]; }

Genome Genome::random(Rng& rng, float dietMax) {
    Genome gn;
    auto& g = gn.g;
    g[G_SIZE] = rng.range(0.2f, 0.55f);
    g[G_SPEED] = rng.range(0.3f, 0.7f);
    g[G_DIET] = rng.range(0.f, dietMax);
    g[G_ARMOR] = rng.range(0.f, 0.3f);
    g[G_WEAPON] = rng.range(0.f, 0.3f);
    g[G_TOXIN] = rng.range(0.f, 0.15f);
    g[G_IMMUNITY] = rng.range(0.f, 0.12f);
    g[G_NIGHT_VISION] = rng.range(0.f, 0.25f);
    g[G_VISION_RANGE] = rng.range(0.25f, 0.55f);
    g[G_VISION_FOV] = rng.range(0.35f, 0.7f);
    g[G_HUE] = rng.uniform();
    g[G_PATTERN] = rng.uniform();
    g[G_THERMAL] = rng.range(0.4f, 0.75f);
    g[G_MUT_RATE] = rng.range(0.45f, 0.55f);
    g[G_MUT_SCALE] = rng.range(0.4f, 0.6f);
    g[G_INVEST] = rng.range(0.3f, 0.6f);
    g[G_MATURITY] = rng.range(0.15f, 0.45f);
    g[G_LONGEVITY] = rng.range(0.3f, 0.6f);
    g[G_LITTER] = rng.range(0.f, 0.5f);
    g[G_SEXUALITY] = rng.range(0.2f, 0.6f);
    g[G_MATE_PREF] = g[G_HUE];
    g[G_CHOOSY] = rng.range(0.f, 0.2f);
    g[G_CLOCK] = rng.uniform();
    g[G_PLASTICITY] = rng.range(0.f, 0.4f);
    g[G_LINK] = rng.range(0.f, 0.2f);
    g[G_DETOX] = rng.range(0.f, 0.12f);
    g[G_AQUATIC] = rng.range(0.f, 0.1f); // founders are land animals; the sea has to be conquered
    g[G_WINGS] = rng.range(0.f, 0.08f);  // ...and so do the air, quills and green skin
    g[G_SPIKES] = rng.range(0.f, 0.08f);
    g[G_PHOTO] = rng.range(0.f, 0.08f);

    // Sparse random wiring; behaviours with real costs (attack, share, pheromone)
    // start disconnected and must be discovered by structural mutation.
    for (int k = 0; k < BRAIN_SIZE; ++k) gn.w[k] = rng.chance(0.08f) ? rng.normal() * 0.6f : 0.f;
    for (int o : {int(OUT_ATTACK), int(OUT_SHARE), int(OUT_PHEROMONE), int(OUT_CALL_A), int(OUT_CALL_B), int(OUT_TEND), int(OUT_BLINK), int(OUT_OVERCLOCK), int(OUT_PHASE)}) {
        for (int h = 0; h < N_HIDDEN; ++h) gn.w[wHO(o, h)] = 0.f;
        for (int i = 0; i < N_INPUTS; ++i) gn.w[wIO(o, i)] = 0.f;
    }
    auto io = [&](int o, int i) -> float& { return gn.w[wIO(o, i)]; };
    auto eye = [](int e, int ch) { return e * int(EC_COUNT) + ch; };
    gn.w[OFF_BO + OUT_EAT] = 1.2f;
    gn.w[OFF_BO + OUT_THRUST] = 0.4f;
    gn.w[OFF_BO + OUT_REPRODUCE] = 0.8f;
    gn.w[OFF_BO + OUT_ATTACK] = rng.range(-1.6f, 0.35f); // some founders are innately hawkish
    gn.w[OFF_BO + OUT_SHARE] = -1.5f;
    gn.w[OFF_BO + OUT_PHEROMONE] = -1.0f;
    gn.w[OFF_BO + OUT_CALL_A] = gn.w[OFF_BO + OUT_CALL_B] = -1.2f;
    gn.w[OFF_BO + OUT_TEND] = -1.4f;
    gn.w[OFF_BO + OUT_BLINK] = gn.w[OFF_BO + OUT_OVERCLOCK] = gn.w[OFF_BO + OUT_PHASE] = -1.6f;
    // Innate grazing: steer toward grass seen by side eyes, slow down on lush grass, avoid water.
    io(OUT_TURN, eye(0, EC_PLANT)) = -1.0f;
    io(OUT_TURN, eye(1, EC_PLANT)) = -0.5f;
    io(OUT_TURN, eye(3, EC_PLANT)) = 0.5f;
    io(OUT_TURN, eye(4, EC_PLANT)) = 1.0f;
    io(OUT_TURN, eye(2, EC_OBSTACLE)) = 1.5f;
    io(OUT_THRUST, eye(2, EC_PLANT)) = 0.6f;
    io(OUT_THRUST, IN_GRASS) = -0.9f;
    return gn;
}

Genome Genome::predator(Rng& rng) {
    Genome gn = random(rng, 0.f);
    auto& g = gn.g;
    g[G_DIET] = rng.range(0.82f, 0.92f);
    g[G_SIZE] = rng.range(0.45f, 0.6f);
    g[G_SPEED] = rng.range(0.55f, 0.7f);
    g[G_WEAPON] = rng.range(0.5f, 0.65f);
    g[G_VISION_RANGE] = rng.range(0.55f, 0.7f);
    g[G_VISION_FOV] = rng.range(0.2f, 0.35f);
    g[G_TOXIN] = 0.f;
    g[G_NIGHT_VISION] = rng.range(0.5f, 0.7f);
    g[G_LITTER] = rng.range(0.f, 0.25f);
    g[G_INVEST] = rng.range(0.5f, 0.65f);
    auto io = [&](int o, int i) -> float& { return gn.w[wIO(o, i)]; };
    auto eye = [](int e, int ch) { return e * int(EC_COUNT) + ch; };
    for (int i = 0; i < N_INPUTS; ++i) io(OUT_TURN, i) = io(OUT_THRUST, i) = 0.f;
    // Hunting instincts: turn toward prey and carrion, charge and bite what is dead ahead,
    // but leave kin (similar colour) alone.
    const float side[NUM_EYES] = {-1.4f, -0.7f, 0.f, 0.7f, 1.4f};
    for (int e = 0; e < NUM_EYES; ++e) {
        io(OUT_TURN, eye(e, EC_CREATURE)) = side[e];
        io(OUT_TURN, eye(e, EC_MEAT)) = side[e] * 0.8f;
    }
    io(OUT_TURN, eye(2, EC_OBSTACLE)) = 1.5f;
    io(OUT_THRUST, eye(2, EC_CREATURE)) = 1.2f;
    io(OUT_THRUST, eye(2, EC_MEAT)) = 0.8f;
    io(OUT_ATTACK, eye(2, EC_CREATURE)) = 3.0f;
    io(OUT_ATTACK, eye(1, EC_CREATURE)) = 1.0f;
    io(OUT_ATTACK, eye(3, EC_CREATURE)) = 1.0f;
    io(OUT_ATTACK, eye(2, EC_KIN)) = -1.2f;
    gn.w[OFF_BO + OUT_ATTACK] = -1.6f;
    gn.w[OFF_BO + OUT_THRUST] = 0.3f;
    gn.w[OFF_BO + OUT_EAT] = 1.5f;
    return gn;
}

int Genome::connectionCount() const {
    int n = 0;
    for (float v : w) n += (v != 0.f);
    return n;
}

const char* instinctName(Instinct i) {
    static const char* n[] = {"Graze", "Hunt", "Flee bigger creatures", "Herd with kin", "Drink when thirsty",
                              "Follow scent trails", "Cry out when hurt", "Rest at night", "Share with kin"};
    return n[int(i)];
}

const char* instinctDescription(Instinct i) {
    static const char* d[] = {
        "Steer toward grass seen by the side eyes, slow down on lush ground, avoid water.",
        "Steer toward creatures and carcasses, charge and bite whatever is dead ahead, spare similar colours.",
        "Turn away from anything bigger and sprint when in pain.",
        "Match the heading of nearby kin and drift toward similar colours.",
        "Turn toward water seen by the eyes, more urgently when thirsty.",
        "Follow pheromone gradients and mark trails where grass is good.",
        "Glow a bright signal when in pain, a warning to relatives.",
        "Slow down when it is dark.",
        "Feed touching neighbours with a similar colour when well fed.",
    };
    return d[int(i)];
}

void addInstinct(Genome& gn, Instinct which) {
    auto io = [&](int o, int i) -> float& { return gn.w[wIO(o, i)]; };
    auto eye = [](int e, int ch) { return e * int(EC_COUNT) + ch; };
    const float side[NUM_EYES] = {-1.2f, -0.6f, 0.f, 0.6f, 1.2f};
    switch (which) {
        case Instinct::Graze:
            for (int e = 0; e < NUM_EYES; ++e) io(OUT_TURN, eye(e, EC_PLANT)) = side[e] * 0.85f;
            io(OUT_THRUST, eye(2, EC_PLANT)) = 0.6f;
            io(OUT_THRUST, IN_GRASS) = -0.9f;
            io(OUT_TURN, eye(2, EC_OBSTACLE)) = 1.5f;
            gn.w[OFF_BO + OUT_EAT] = std::max(gn.w[OFF_BO + OUT_EAT], 1.2f);
            break;
        case Instinct::Hunt:
            for (int e = 0; e < NUM_EYES; ++e) {
                io(OUT_TURN, eye(e, EC_CREATURE)) = side[e] * 1.15f;
                io(OUT_TURN, eye(e, EC_MEAT)) = side[e] * 0.9f;
            }
            io(OUT_THRUST, eye(2, EC_CREATURE)) = 1.2f;
            io(OUT_ATTACK, eye(2, EC_CREATURE)) = 3.f;
            io(OUT_ATTACK, eye(2, EC_KIN)) = -1.2f;
            gn.w[OFF_BO + OUT_ATTACK] = -1.6f;
            gn.g[G_DIET] = std::max(gn.g[G_DIET], 0.8f);
            gn.g[G_WEAPON] = std::max(gn.g[G_WEAPON], 0.5f);
            break;
        case Instinct::Flee:
            for (int e = 0; e < NUM_EYES; ++e) io(OUT_TURN, eye(e, EC_SIZE)) = -side[e] * 1.4f;
            io(OUT_THRUST, IN_PAIN) = 2.f;
            io(OUT_THRUST, eye(2, EC_SIZE)) = -0.6f;
            break;
        case Instinct::Herd:
            io(OUT_TURN, IN_FLOCK) = 1.2f;
            for (int e = 0; e < NUM_EYES; ++e) io(OUT_TURN, eye(e, EC_KIN)) = side[e] * 0.5f;
            break;
        case Instinct::Drink:
            io(OUT_TURN, IN_WATER_DIR) = 1.6f;
            io(OUT_THRUST, IN_THIRST) = 0.8f;
            io(OUT_TURN, eye(2, EC_OBSTACLE)) = 0.f; // stop shying away from shores
            break;
        case Instinct::FollowScent:
            io(OUT_TURN, IN_SMELL_DIR) = 1.8f;
            io(OUT_PHEROMONE, IN_GRASS) = 2.f;
            gn.w[OFF_BO + OUT_PHEROMONE] = -1.f;
            break;
        case Instinct::CryOut:
            io(OUT_SIGNAL, IN_PAIN) = 3.f;
            io(OUT_CALL_A, IN_PAIN) = 3.f; // also a sound: kin that evolve to heed call A can use it as an alarm
            gn.w[OFF_BO + OUT_CALL_A] = -0.8f;
            break;
        case Instinct::RestAtNight:
            io(OUT_THRUST, IN_LIGHT) = 1.2f;
            gn.w[OFF_BO + OUT_THRUST] -= 0.6f;
            break;
        case Instinct::ShareWithKin:
            io(OUT_SHARE, IN_TOUCH) = 1.6f;
            io(OUT_SHARE, IN_ENERGY) = 1.8f;
            io(OUT_SHARE, eye(2, EC_KIN)) = 1.f;
            gn.w[OFF_BO + OUT_SHARE] = -2.2f;
            break;
        default: break;
    }
}

namespace {
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr const char* GENOME_TAG = "PRIMORDIA-GENOME-1:";
} // namespace

std::string genomeToText(const Genome& g) {
    static_assert(sizeof(Genome) % 4 == 0);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&g);
    const size_t n = sizeof(Genome);
    std::string out = GENOME_TAG;
    out += std::to_string(int(G_COUNT)) + ":" + std::to_string(BRAIN_SIZE) + ":";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
        if (i + 2 < n) v |= p[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? B64[v & 63] : '=';
    }
    return out;
}

bool genomeFromText(const std::string& textIn, Genome& out) {
    std::string text;
    for (char ch : textIn)
        if (!std::isspace(static_cast<unsigned char>(ch))) text += ch;
    const std::string tag = GENOME_TAG;
    if (text.compare(0, tag.size(), tag) != 0) return false;
    size_t pos = tag.size();
    size_t c1 = text.find(':', pos), c2 = c1 == std::string::npos ? c1 : text.find(':', c1 + 1);
    if (c2 == std::string::npos) return false;
    if (std::atoi(text.substr(pos, c1 - pos).c_str()) != int(G_COUNT) ||
        std::atoi(text.substr(c1 + 1, c2 - c1 - 1).c_str()) != BRAIN_SIZE)
        return false; // made by a different version
    std::string bytes;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = c2 + 1; i < text.size(); ++i) {
        char ch = text[i];
        if (ch == '=') break;
        const char* f = std::strchr(B64, ch);
        if (!f || !*f) return false;
        v = (v << 6) | uint32_t(f - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += char((v >> bits) & 0xFF);
        }
    }
    if (bytes.size() != sizeof(Genome)) return false;
    Genome g;
    std::memcpy(&g, bytes.data(), sizeof(Genome));
    for (float& x : g.g)
        if (!std::isfinite(x)) return false;
    for (float& x : g.w)
        if (!std::isfinite(x)) return false;
    for (float& x : g.g) x = clamp01(x);
    out = g;
    return true;
}

void mutate(Genome& gn, Rng& rng, float rateMultiplier) {
    const float rate = mutationRateOf(gn) * rateMultiplier;
    const float scale = mutationScaleOf(gn);
    if (rate <= 0.f) return;

    const float bodyP = std::min(1.f, rate * 4.f);
    for (int i = 0; i < G_COUNT; ++i) {
        if (!rng.chance(bodyP)) continue;
        float v = gn.g[i] + rng.normal() * scale * 0.3f;
        if (i == G_HUE || i == G_MATE_PREF) v -= std::floor(v);
        else v = clamp01(v);
        gn.g[i] = v;
    }

    for (int k = 0; k < BRAIN_SIZE; ++k) {
        float& w = gn.w[k];
        if (w == 0.f || !rng.chance(rate)) continue;
        if (rng.chance(0.04f)) w = rng.normal();
        else w += rng.normal() * scale;
        w = clampf(w, -4.f, 4.f);
        if (w == 0.f) w = 1e-4f;
    }

    // Structural mutation: grow and prune connections.
    if (rng.chance(std::min(1.f, rate * 14.f))) {
        for (int tries = 0; tries < 16; ++tries) {
            int k = rng.below(BRAIN_SIZE);
            if (gn.w[k] == 0.f) { gn.w[k] = rng.normal() * 0.6f + (rng.chance(0.5f) ? 1e-3f : -1e-3f); break; }
        }
    }
    if (rng.chance(std::min(1.f, rate * 12.f))) {
        for (int tries = 0; tries < 16; ++tries) {
            int k = rng.below(BRAIN_SIZE);
            if (gn.w[k] != 0.f) { gn.w[k] = 0.f; break; }
        }
    }
}

Genome crossover(const Genome& a, const Genome& b, Rng& rng) {
    Genome c;
    for (int i = 0; i < G_COUNT; ++i) c.g[i] = rng.chance(0.5f) ? a.g[i] : b.g[i];
    // Inherit whole neurons (incoming weights + bias) so functional units stay intact.
    for (int h = 0; h < N_HIDDEN; ++h) {
        const Genome& src = rng.chance(0.5f) ? a : b;
        for (int i = 0; i < N_INPUTS; ++i) c.w[wIH(h, i)] = src.w[wIH(h, i)];
        for (int k = 0; k < N_HIDDEN; ++k) c.w[wHH(h, k)] = src.w[wHH(h, k)];
        c.w[OFF_BH + h] = src.w[OFF_BH + h];
    }
    for (int o = 0; o < N_OUTPUTS; ++o) {
        const Genome& src = rng.chance(0.5f) ? a : b;
        for (int h = 0; h < N_HIDDEN; ++h) c.w[wHO(o, h)] = src.w[wHO(o, h)];
        for (int i = 0; i < N_INPUTS; ++i) c.w[wIO(o, i)] = src.w[wIO(o, i)];
        c.w[OFF_BO + o] = src.w[OFF_BO + o];
    }
    return c;
}

float bodyDistance(const Genome& a, const Genome& b) {
    float s = 0.f;
    for (int i = 0; i < G_COUNT; ++i) {
        float d = (i == G_HUE || i == G_MATE_PREF) ? hueDistance(a.g[i], b.g[i]) * 2.f : (a.g[i] - b.g[i]);
        s += GENE_INFO[i].distanceWeight * d * d;
    }
    return std::sqrt(s);
}

float geneticDistance(const Genome& a, const Genome& b) {
    float brain = 0.f;
#pragma omp simd reduction(+ : brain)
    for (int k = 0; k < BRAIN_SIZE; ++k) brain += std::fabs(a.w[k] - b.w[k]);
    brain /= float(BRAIN_SIZE);
    return bodyDistance(a, b) + brain * 1.5f;
}

Phenotype Phenotype::from(const Genome& gn, float specialization) {
    const auto& g = gn.g;
    Phenotype p{};
    p.adultRadius = lerp(5.f, 20.f, g[G_SIZE]);
    const float rs = p.adultRadius / 10.f;
    p.armor = g[G_ARMOR];
    p.weapon = g[G_WEAPON];
    p.toxin = g[G_TOXIN];
    p.immunity = g[G_IMMUNITY];
    p.nightVision = g[G_NIGHT_VISION];
    // Big bodies are faster in a straight line; small ones turn tighter.
    p.accel = (0.06f + 0.30f * g[G_SPEED]) * std::pow(rs, 0.25f) * (1.f - 0.35f * p.armor);
    p.turnRate = (0.07f + 0.10f * g[G_SPEED]) * std::pow(rs, -0.5f);
    p.visionRange = lerp(40.f, 320.f, g[G_VISION_RANGE]);
    p.fov = lerp(0.35f * PI, 1.9f * PI, g[G_VISION_FOV]);
    p.diet = g[G_DIET];
    // Jarman-Bell principle: bigger guts extract more from fibrous plants.
    const float gut = 0.62f + 0.38f * smoothstep(0.45f, 1.5f, rs);
    p.plantEff = std::pow(1.f - p.diet, specialization) * gut;
    p.meatEff = std::pow(p.diet, specialization);
    p.damage = (0.3f + 3.2f * p.weapon) * rs;
    p.longevity = g[G_LONGEVITY];
    p.lifespan = (9000.f + 18000.f * p.longevity) * std::pow(rs, 0.35f);
    p.maturity = lerp(400.f, 3500.f, g[G_MATURITY]);
    p.litter = 1 + int(g[G_LITTER] * 3.999f);
    p.investment = lerp(0.15f, 0.85f, g[G_INVEST]);
    p.sexuality = g[G_SEXUALITY];
    p.clockFreq = lerp(0.004f, 0.2f, g[G_CLOCK]);
    p.thermalOpt = lerp(-1.f, 1.f, g[G_THERMAL]);
    p.mutRate = mutationRateOf(gn);
    p.mutScale = mutationScaleOf(gn);
    p.connections = gn.connectionCount();
    p.upkeep = 0.f;
    p.linkStrength = 1.5f * g[G_LINK];
    {
        const float step = p.fov / float(NUM_EYES);
        const float off = -0.5f * p.fov + 0.5f * step;
        p.cOff = std::cos(off); p.sOff = std::sin(off);
        p.cStep = std::cos(step); p.sStep = std::sin(step);
    }
    p.detox = g[G_DETOX];
    p.aquatic = g[G_AQUATIC];
    p.wings = g[G_WINGS];
    p.lift = p.wings * clampf(1.7f - rs, 0.15f, 1.f); // rs is 0.5 (tiny) .. 2 (huge)
    p.spikes = g[G_SPIKES];
    p.photo = g[G_PHOTO];
    // Costs grow with the square of the gene, so a first step toward a new body is cheap and the last is dear.
    p.carn = smoothstep(0.7f, 0.9f, p.diet); // only committed meat eaters get the hunter's body, not grazers that dabble
    p.accel *= (1.f + 0.25f * p.carn) * (1.f + 0.25f * p.lift) * (1.f - 0.1f * p.spikes) * (1.f - 0.4f * p.photo * p.photo);
    p.plantEff *= 1.f - 0.3f * p.photo * p.photo;
    p.meatEff *= 1.f - p.photo;
    p.learnRate = 0.05f * g[G_PLASTICITY] * g[G_PLASTICITY] * 4.f; // quadratic: low plasticity is nearly free
    return p;
}

} // namespace pm
