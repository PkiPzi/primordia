#include "sim/Scenarios.hpp"

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
};

} // namespace

int scenarioCount() { return int(sizeof(SCENARIOS) / sizeof(SCENARIOS[0])); }
const Scenario& scenario(int i) { return SCENARIOS[i]; }

} // namespace pm
