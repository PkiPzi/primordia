#include "sim/Scenarios.hpp"

#include <algorithm>
#include <cmath>

#include "ui/Icons.hpp"

namespace pm {

namespace {

float& io(Genome& g, int o, int i) { return g.w[wIO(o, i)]; }

// Give the first founding lineage an instinct to feed hungry relatives it touches.
void makeAltruists(World& w) {
    if (w.species.all().empty()) return;
    auto edit = [](Genome& g) {
        g.g[G_HUE] = 0.33f; // a shared "green beard" colour
        io(g, OUT_SHARE, IN_TOUCH) = 1.6f;
        io(g, OUT_SHARE, IN_ENERGY) = 1.8f;
        io(g, OUT_SHARE, 2 * EC_COUNT + EC_KIN) = 1.0f;
        g.w[OFF_BO + OUT_SHARE] = -2.2f;
    };
    Species& s = w.species.get(1);
    edit(s.rep);
    s.color = hsv(0.33f, 0.75f, 0.95f);
    for (Creature& c : w.creatures)
        if (c.speciesId == 1) {
            edit(c.genome);
            c.ph = Phenotype::from(c.genome, w.params.specialization);
        }
}

// Founders start with bright warning colours and potent toxins on one lineage only.
void makeToxicLineage(World& w) {
    if (w.species.all().size() < 2) return;
    auto edit = [](Genome& g) {
        g.g[G_TOXIN] = 0.75f;
        g.g[G_HUE] = 0.13f; // wasp yellow
        g.g[G_PATTERN] = 0.5f;
    };
    Species& s = w.species.get(1);
    edit(s.rep);
    s.color = hsv(0.13f, 0.85f, 1.f);
    for (Creature& c : w.creatures)
        if (c.speciesId == 1) {
            edit(c.genome);
            c.ph = Phenotype::from(c.genome, w.params.specialization);
        }
}

// The first four lineages each start with one finished body plan: flier, porcupine, sunbather, swimmer.
void makeMenagerie(World& w) {
    static const int plan[4] = {G_WINGS, G_SPIKES, G_PHOTO, G_AQUATIC};
    auto edit = [](Genome& g, int k) {
        g.g[size_t(plan[k])] = 0.8f;
        if (plan[k] != G_SPIKES) g.g[G_SIZE] = std::min(g.g[G_SIZE], 0.3f); // small bodies fly and sunbathe best
    };
    for (Species& s : w.species.all())
        if (s.id >= 1 && s.id <= 4) edit(s.rep, int(s.id) - 1);
    for (Creature& c : w.creatures)
        if (c.speciesId >= 1 && c.speciesId <= 4) {
            edit(c.genome, int(c.speciesId) - 1);
            c.ph = Phenotype::from(c.genome, w.params.specialization);
        }
}

// Every founder starts half-way to the water: clumsy waders that can already nibble algae.
void makeWaders(World& w) {
    for (Species& s : w.species.all()) s.rep.g[G_AQUATIC] = 0.35f;
    for (Creature& c : w.creatures) {
        c.genome.g[G_AQUATIC] = 0.35f;
        c.ph = Phenotype::from(c.genome, w.params.specialization);
    }
}

const Scenario SCENARIOS[] = {
    {"Primordial Soup", ICON_GLOBE, "The full sandbox",
     "Four grazing lineages and one hunting lineage on a continent with seasons, disease and climate. "
     "Everything is switched on; watch what emerges.",
     [](WorldSettings& ws, SimParams&) { ws = WorldSettings{}; },
     nullptr,
     "Open Population > Overview, then follow a hunter with F. Watch the Tree of Life fill in, and try an ice age or a meteor from Tools."},
    {"Natural Selection 101", ICON_FLASK, "Speed, size and senses",
     "A flat, mild world with no predators and no seasons. The only pressure is finding food, so watch how "
     "speed, size and vision trade off against their energy costs.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Flatland;
         ws.width = ws.height = 2048.f;
         ws.initialPopulation = 250;
         ws.foundingLineages = 1;
         ws.predatorLineages = 0;
         p.predatorImmigration = false;
         ws.initialDietMax = 0.f;
         p.seasonStrength = 0.f;
         p.epidemics = false;
         p.mutationRate = 2.f;
     },
     nullptr,
     "Traits > Evolution shows Size, Speed and Vision range shifting as the population finds the cheapest way to make a living. Morphospace shows the trade-offs."},
    {"Predator & Prey", ICON_PAW, "Arms races and boom-bust cycles",
     "Two hunting lineages share a continent with the grazers. Expect Lotka-Volterra oscillations, faster prey, "
     "armour, toxins and ever sharper predators.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.predatorLineages = 2;
         ws.predatorShare = 0.15f;
         p.epidemics = false;
     },
     nullptr,
     "Population > Predator-prey traces Lotka-Volterra loops. Traits > Evolution with Speed or Armor shows the prey's arms race."},
    {"Warning Colours", ICON_WARNING, "Toxins, aposematism and mimicry",
     "One grazer lineage starts out toxic and wasp-yellow. Predators that bite it get poisoned. Will predators "
     "learn to avoid yellow, and will harmless species evolve to look yellow too?",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.predatorLineages = 2;
         ws.predatorShare = 0.15f;
         ws.foundingLineages = 3;
         p.epidemics = false;
         p.toxinCost = 0.5f;
     },
     makeToxicLineage,
     "Population > Behaviour: does 'Attacks hitting toxic prey' fall below the toxic share? Colour creatures by Toxin, then by Genes, to spot yellow mimics."},
    {"Epidemic", ICON_BUG, "Contagion in a crowded world",
     "One dense lineage, frequent outbreaks and a contagious pathogen with short-lived immunity. Watch the "
     "SIR waves in Population > Epidemic and see whether immunity evolves.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Lakes;
         ws.width = ws.height = 3072.f;
         ws.foundingLineages = 1;
         ws.predatorLineages = 0;
         p.predatorImmigration = false;
         ws.initialPopulation = 1200;
         p.outbreaksPerYear = 1.5f;
         p.transmission = 0.09f;
         p.immunityDuration = 5000;
         p.virulence = 1.4f;
     },
     nullptr,
     "Population > Epidemic shows susceptible, infected and immune waves. Traits > Evolution with Immunity shows whether resistance evolves."},
    {"Island Hopping", ICON_MAP, "Speciation in isolation",
     "A single lineage on an archipelago. Islands are separate gene pools, but slow glacial cycles lower the sea "
     "and open land bridges, then flood them again. A textbook speciation pump.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Archipelago;
         ws.waterFraction = 0.6f;
         p.glacialCycleYears = 12.f;
         p.glacialAmplitude = 0.5f;
         ws.foundingLineages = 1;
         ws.predatorLineages = 0;
         p.predatorImmigration = false;
         p.epidemics = false;
     },
     nullptr,
     "Watch the Tree of Life branch as islands drift apart, and the Species territories overlay. Glacial cycles lower the sea every few years."},
    {"Ice Age", ICON_SNOW, "Fur, fat and migration",
     "A cold planet with brutal winters. Plants freeze at the poles and creatures far from their thermal "
     "optimum burn energy. Bigger bodies hold heat better.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.temperature = -0.35f;
         p.seasonStrength = 1.7f;
         p.glacialCycleYears = 10.f;
         p.glacialAmplitude = 0.35f;
         p.epidemics = false;
     },
     nullptr,
     "Traits > Evolution with Thermal optimum and Size. Overlay > Temperature shows winter sweeping the poles. Migrations follow the seasons."},
    {"Kin & Altruism", ICON_GIFT, "The green-beard experiment",
     "Food is scarce. One green lineage instinctively feeds hungry relatives it touches. Can generosity "
     "survive against selfish neighbours, and does it spread or fade?",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.foundingLineages = 3;
         ws.predatorLineages = 0;
         p.predatorImmigration = false;
         p.plantGrowth = 0.7f;
         p.shareEfficiency = 0.95f;
         p.epidemics = false;
     },
     makeAltruists,
     "Population > Behaviour: 'Sharing food'. Colour by Species: does the green lineage hold its ground? Altruists feed relatives they touch."},
    {"Cambrian Explosion", ICON_STAR, "A burst of new body plans",
     "Twelve small founding lineages, doubled mutation rates and a lenient species definition on a lush world. "
     "Expect a rapid radiation of forms, followed by a culling as competition sorts the winners.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.foundingLineages = 12;
         ws.predatorLineages = 2;
         ws.initialPopulation = 1200;
         ws.initialDietMax = 0.6f;
         ws.moisture = 0.15f;
         p.mutationRate = 2.2f;
         p.speciesThreshold = 0.65f;
     },
     nullptr,
     "Tree of Life and Population > Species show the radiation and the shake-out. Traits > Morphospace (Size vs Diet) "
     "shows the body plans spreading apart."},
    {"Single Origin", ICON_BULLSEYE, "All life from one ancestor",
     "Twenty identical grazers on an empty continent, no predators. Every species that ever appears will descend "
     "from them. Watch hunters, armour and toxins arise from scratch, if they ever do.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.foundingLineages = 1;
         ws.predatorLineages = 0;
         ws.initialPopulation = 20;
         p.predatorImmigration = false;
         p.mutationRate = 1.5f;
     },
     nullptr,
     "Population > Overview counts species as they appear. Colour by Diet to spot the first meat-eaters, and keep "
     "an eye on Events for 'has become a predator'."},
    {"Desert Pangaea", ICON_SUN, "One hot, dry supercontinent",
     "A single landmass ringed by ocean, hot and dry. Fertile oases are rare, so lineages crowd around them "
     "and compete hard.",
     [](WorldSettings& ws, SimParams&) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Pangaea;
         ws.temperature = 0.3f;
         ws.moisture = -0.2f;
     },
     nullptr,
     "Overlay > Fertility shows the rare oases. Follow a creature and watch the Water bar in the Inspector."},
    {"Water World", ICON_TINT, "Back to the sea",
     "Small islands in a wide, algae-rich ocean. The founders are clumsy waders. The land is crowded and the sea is "
     "empty: watch for lineages that grow fins, stop coming ashore and become true sea animals.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Archipelago;
         ws.waterFraction = 0.7f;
         ws.foundingLineages = 3;
         ws.predatorLineages = 0;
         p.predatorImmigration = false;
         p.epidemics = false;
     },
     makeWaders,
     "Traits > Evolution with the Aquatic gene shows bodies shifting toward water. Events > Discoveries reports the first "
     "amphibians and the first true sea animals."},
    {"Menagerie", ICON_PAW, "Four body plans, one world",
     "The founders are already strange: one lineage flies, one bristles with quills, one lives on sunlight and one swims. "
     "Hunters are loose among them. See which bodies hold their ground, which merge and which vanish.",
     [](WorldSettings& ws, SimParams& p) {
         ws = WorldSettings{};
         ws.preset = TerrainPreset::Archipelago;
         ws.foundingLineages = 4;
         ws.predatorLineages = 2;
         p.epidemics = false;
     },
     makeMenagerie,
     "Zoom in: wings, quills, leaves and fins are drawn on the body. Traits > Evolution shows each gene rising or fading."},
};

// --- Objectives -------------------------------------------------------------
float years(const World& w) { return float(w.year()); }

// Mean of a statistic over the most recent samples (single samples are noisy).
template <class F>
float recent(const World& w, F f, size_t n = 20) {
    const auto& v = w.stats.samples;
    if (v.empty()) return 0.f;
    n = std::min(n, v.size());
    float sum = 0.f;
    for (size_t i = v.size() - n; i < v.size(); ++i) sum += f(v[i]);
    return sum / float(n);
}

// Lowest value a statistic ever took (skipping the first sample, taken before the world settles).
template <class F>
float lowest(const World& w, F f) {
    float m = 1e30f;
    for (size_t i = 1; i < w.stats.samples.size(); ++i) m = std::min(m, f(w.stats.samples[i]));
    return m;
}

float geneShift(const World& w, int g) {
    const auto& v = w.stats.samples;
    return v.size() < 2 ? 0.f : recent(w, [g](const StatSample& s) { return s.geneMean[g]; }) - v.front().geneMean[g];
}

bool hasHunters(const World& w) {
    for (const Species& s : w.species.all())
        if (s.alive() && s.population >= 8 && s.meanGenes[G_DIET] > 0.6f) return true;
    return false;
}

// "Still going at year N, provided `ok`": creeps to 99% and waits there until the condition holds.
float lastUntil(const World& w, float targetYears, bool ok) {
    const float p = years(w) / targetYears;
    return p >= 1.f ? (ok ? 1.f : 0.99f) : p;
}

const Objective OBJECTIVES[] = {
    {"Reach year 10 with at least 6 species alive, one of them a hunter",
     [](const World& w) { return lastUntil(w, 10.f, w.species.aliveCount() >= 6 && hasHunters(w)); }},
    {"Let selection move the average Size, Speed or Vision range gene by 0.15 from where the founders started",
     [](const World& w) {
         return std::max({std::fabs(geneShift(w, G_SIZE)), std::fabs(geneShift(w, G_SPEED)),
                          std::fabs(geneShift(w, G_VISION_RANGE))}) / 0.15f;
     }},
    {"Keep hunters and grazers together until year 8 without the hunters ever dying out",
     [](const World& w) {
         if (lowest(w, [](const StatSample& s) { return s.carnivores; }) <= 0.f) return -1.f;
         return lastUntil(w, 8.f, true);
     }},
    {"Teach the predators: attacks on toxic prey fall below half of the toxic share of the population",
     [](const World& w) {
         const float toxic = recent(w, [](const StatSample& s) { return s.toxic; });
         const float hit = recent(w, [](const StatSample& s) { return s.toxicTargeted; });
         const float attacks = recent(w, [](const StatSample& s) { return s.attackRate; });
         if (years(w) < 1.f || toxic < 0.05f || attacks <= 0.f) return std::min(0.2f, years(w) * 0.2f);
         return clamp01(2.f * (1.f - hit / toxic)) * 0.99f + (hit < 0.5f * toxic ? 0.01f : 0.f);
     }},
    {"Evolve resistance: raise the average Immunity gene by 0.06",
     [](const World& w) { return std::max(0.f, geneShift(w, G_IMMUNITY)) / 0.06f; }},
    {"Have 6 species alive at once",
     [](const World& w) { return float(w.species.aliveCount()) / 6.f; }},
    {"Survive to year 8 without the population ever falling below 300",
     [](const World& w) {
         if (lowest(w, [](const StatSample& s) { return s.population; }) < 300.f) return -1.f;
         return lastUntil(w, 8.f, true);
     }},
    {"Keep generosity alive: at year 6, at least 2% of creatures are still sharing food",
     [](const World& w) { return lastUntil(w, 6.f, recent(w, [](const StatSample& s) { return s.sharing; }, 60) >= 0.02f); }},
    {"Have 60 species alive at once",
     [](const World& w) { return float(w.species.aliveCount()) / 60.f; }},
    {"See a predator species (average Diet gene above 0.6) arise from the grazers",
     [](const World& w) {
         float best = 0.f;
         for (const Species& s : w.species.all())
             if (s.alive() && s.population >= 8) best = std::max(best, s.meanGenes[G_DIET]);
         return best / 0.6f;
     }},
    {"Outlast the drought: reach year 8 without the population ever falling below 250",
     [](const World& w) {
         if (lowest(w, [](const StatSample& s) { return s.population; }) < 250.f) return -1.f;
         return lastUntil(w, 8.f, true);
     }},
    {"Evolve a true sea animal: a species of 20 or more with an average Aquatic gene above 0.7",
     [](const World& w) {
         float best = 0.f;
         for (const Species& s : w.species.all())
             if (s.alive() && s.population >= 20) best = std::max(best, s.meanGenes[G_AQUATIC]);
         return best / 0.7f;
     }},
    {"Keep the menagerie varied: at year 5, at least two of the four body plans (wings, quills, sun-fed skin, fins: gene above 0.5) still number 20 or more",
     [](const World& w) {
         static const int plan[4] = {G_WINGS, G_SPIKES, G_PHOTO, G_AQUATIC};
         int n[4] = {}, kept = 0;
         for (const Creature& c : w.creatures)
             for (int k = 0; k < 4; ++k) n[k] += c.genome.g[size_t(plan[k])] > 0.5f;
         for (int k = 0; k < 4; ++k) kept += n[k] >= 20;
         return lastUntil(w, 5.f, kept >= 2);
     }},
};
static_assert(sizeof(OBJECTIVES) / sizeof(OBJECTIVES[0]) == sizeof(SCENARIOS) / sizeof(SCENARIOS[0]),
              "every scenario needs an objective");

} // namespace

const Objective& objective(int i) { return OBJECTIVES[i]; }

int scenarioCount() { return int(sizeof(SCENARIOS) / sizeof(SCENARIOS[0])); }
const Scenario& scenario(int i) { return SCENARIOS[i]; }

} // namespace pm
