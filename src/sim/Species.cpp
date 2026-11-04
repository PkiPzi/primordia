#include "sim/Species.hpp"

#include <cmath>

namespace pm {

namespace {

struct EpithetSet {
    const char* low[3];
    const char* high[3];
};

// Latin-flavoured epithets keyed by the gene that most distinguishes a new species.
const EpithetSet EPITHETS[] = {
    {{"minimus", "parvus", "pusillus"}, {"magnus", "giganteus", "robustus"}},
    {{"lentus", "tardus", "placidus"}, {"velox", "celer", "rapidus"}},
    {{"herbivorus", "pascens", "phyllophagus"}, {"carnivorus", "rapax", "ferox"}},
    {{"nudus", "mollis", "tenuis"}, {"loricatus", "cataphractus", "scutatus"}},
    {{"inermis", "edentatus", "mitis"}, {"dentatus", "armatus", "mordax"}},
    {{"edulis", "innocuus", "sapidus"}, {"venenatus", "toxicus", "virulentus"}},
    {{"fragilis", "infirmus", "debilis"}, {"immunis", "resistens", "invictus"}},
    {{"diurnus", "heliophilus", "solaris"}, {"nocturnus", "noctivagus", "lucifugus"}},
    {{"caecus", "myops", "obscurus"}, {"oculatus", "vigilans", "speculator"}},
    {{"angustus", "directus", "focalis"}, {"circumspectus", "panopticus", "latus"}},
    {{"ruber", "flavus", "viridis"}, {"caeruleus", "violaceus", "roseus"}},
    {{"concolor", "simplex", "unicolor"}, {"maculatus", "striatus", "guttatus"}},
    {{"frigidus", "borealis", "glacialis"}, {"thermophilus", "aestivus", "tropicus"}},
    {{"constans", "stabilis", "fidelis"}, {"mutabilis", "varius", "proteus"}},
    {{"modestus", "subtilis", "lenis"}, {"audax", "saltator", "volubilis"}},
    {{"prolificus", "fecundus", "profusus"}, {"nutrix", "maternus", "diligens"}},
    {{"praecox", "festinans", "citatus"}, {"serotinus", "patiens", "tardigradus"}},
    {{"ephemerus", "brevis", "fugax"}, {"longaevus", "perennis", "senex"}},
    {{"solitarius", "singularis", "unicus"}, {"multiparus", "fertilis", "copiosus"}},
    {{"clonalis", "virgineus", "asexualis"}, {"amatorius", "socialis", "gemellus"}},
    {{"sanguineus", "aureus", "smaragdinus"}, {"cyaneus", "purpureus", "rosaceus"}},
    {{"promiscuus", "facilis", "vulgaris"}, {"fastidiosus", "elegans", "superbus"}},
    {{"quietus", "sopitus", "lentiger"}, {"pulsans", "vibrans", "rhythmicus"}},
    {{"instinctivus", "innatus", "rigidus"}, {"docilis", "sagax", "callidus"}},
    {{"solivagus", "eremita", "segregatus"}, {"collectivus", "hiveus", "consors"}},
    {{"vulnerabilis", "tener", "imprudens"}, {"purgatus", "resistens", "venenivorus"}},
};

static_assert(std::size(EPITHETS) == G_COUNT, "one epithet set per gene");

const char* GENUS_HEAD[] = {"Ar", "Bel", "Cor", "Dra", "El", "Fel", "Gor", "Hal", "Is", "Jun", "Ka", "Lum",
                            "Mor", "Nax", "Or", "Pel", "Quor", "Rha", "Syl", "Tor", "Ul", "Vel", "Xan", "Yr",
                            "Zel", "Cae", "Ther", "Phy", "Cten", "Lep", "Myx", "Pter", "Saur", "Chel", "Orn",
                            "Din", "Prot", "Ne", "Arch", "Eu", "Gly", "Spi", "Tri", "Amph", "Bra", "Cyn"};
const char* GENUS_MID[] = {"a", "o", "i", "e", "u", "an", "or", "ul", "ix", "yr", "am", "eth", "ono", "ali", "eri"};
const char* GENUS_TAIL[] = {"us", "ops", "odon", "ix", "oides", "aria", "ella", "yx", "on", "ura",
                            "ax", "ites", "onyx", "ellus", "orpha", "ia", "osaur", "athus", "ema", "is"};

std::string roman(int n) {
    const char* r[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    if (n <= 10) return r[n];
    return std::to_string(n);
}

} // namespace

void SpeciesTracker::clear(uint64_t seed) {
    all_.clear();
    candidates_.clear();
    usedNames_.clear();
    nameRng_.reseed(seed ^ 0x5bd1e995ULL, 77);
    aliveCount_ = 0;
}

std::string SpeciesTracker::makeGenus() {
    auto pick = [&](const auto& arr) { return std::string(arr[nameRng_.below(int(std::size(arr)))]); };
    std::string s = pick(GENUS_HEAD);
    if (nameRng_.chance(0.6f)) s += pick(GENUS_MID);
    s += pick(GENUS_TAIL);
    return s;
}

std::string SpeciesTracker::makeEpithet(const Genome& g, const Genome* parent) {
    int best = -1;
    float bestScore = 0.f;
    for (int i = 0; i < G_COUNT; ++i) {
        if (i == G_HUE || i == G_CLOCK) continue;
        float ref = parent ? parent->g[i] : 0.5f;
        float d = (g.g[i] - ref) * GENE_INFO[i].distanceWeight;
        if (std::fabs(d) > std::fabs(bestScore)) { bestScore = d; best = i; }
    }
    if (best < 0) best = G_HUE;
    const EpithetSet& set = EPITHETS[best];
    const char* const* words = bestScore >= 0 ? set.high : set.low;
    return words[nameRng_.below(3)];
}

uint32_t SpeciesTracker::create(const Genome& g, uint32_t parent, int64_t tick, uint32_t founderId) {
    Species s;
    s.id = uint32_t(all_.size() + 1);
    s.parentId = parent;
    s.bornTick = tick;
    s.founderId = founderId;
    s.rep = g;
    s.meanGenes = g.g;
    const Genome* parentRep = nullptr;
    if (parent) {
        const Species& p = get(parent);
        parentRep = &p.rep;
        s.depth = p.depth + 1;
        // Keep the genus for close relatives, coin a new one every few branchings.
        if (p.genusDepth < 3 && !nameRng_.chance(0.15f)) {
            s.genus = p.genus;
            s.genusDepth = p.genusDepth + 1;
        }
    }
    if (s.genus.empty()) {
        s.genus = makeGenus();
        s.genusDepth = 0;
    }
    std::string ep;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ep = makeEpithet(g, parentRep);
        if (!usedNames_.count(s.genus + " " + ep)) break;
        if (attempt >= 4) s.genus = makeGenus(), s.genusDepth = 0;
    }
    std::string full = s.genus + " " + ep;
    for (int n = 2; usedNames_.count(full); ++n) full = s.genus + " " + ep + " " + roman(n);
    s.epithet = full.substr(s.genus.size() + 1);
    usedNames_.insert(full);

    float h = g.g[G_HUE];
    uint32_t hash = s.id * 2654435761u;
    float sat = 0.55f + 0.3f * float((hash >> 8) & 255) / 255.f;
    float val = 0.78f + 0.2f * float((hash >> 16) & 255) / 255.f;
    s.color = hsv(h, sat, val);

    if (parent) get(parent).children.push_back(s.id);
    all_.push_back(std::move(s));
    ++aliveCount_;
    return uint32_t(all_.size());
}

uint32_t SpeciesTracker::createRoot(const Genome& g, int64_t tick, uint32_t founderId) {
    return create(g, 0, tick, founderId);
}

uint32_t SpeciesTracker::assign(const Genome& child, uint32_t parentSpecies, int64_t tick, uint32_t childId,
                                bool& created) {
    created = false;
    if (!valid(parentSpecies)) {
        created = true;
        return create(child, 0, tick, childId);
    }
    Species& ps = get(parentSpecies);
    if (geneticDistance(child, ps.rep) <= threshold) return parentSpecies;

    uint32_t best = 0;
    float bestD = threshold;
    for (uint32_t cid : ps.children) {
        const Species& cs = get(cid);
        if (!cs.alive()) continue;
        float d = geneticDistance(child, cs.rep);
        if (d < bestD) { bestD = d; best = cid; }
    }
    // Also allow re-joining the grandparent lineage (back-mutation / hybrid zones).
    if (!best && ps.parentId) {
        const Species& gp = get(ps.parentId);
        if (gp.alive() && geneticDistance(child, gp.rep) < threshold * 0.8f) best = ps.parentId;
    }
    if (best) return best;
    // Candidate founders: only a cluster of births in the same new region becomes a species.
    auto& cands = candidates_[parentSpecies];
    for (size_t i = 0; i < cands.size();) {
        if (tick - cands[i].lastTick > 4000) cands.erase(cands.begin() + long(i));
        else ++i;
    }
    for (Candidate& cd : cands)
        if (geneticDistance(child, cd.rep) < threshold) {
            cd.lastTick = tick;
            if (++cd.count >= MIN_FOUNDERS) {
                cands.erase(cands.begin() + (&cd - cands.data()));
                created = true;
                return create(child, parentSpecies, tick, childId);
            }
            return parentSpecies;
        }
    if (cands.size() >= 8) cands.erase(cands.begin());
    cands.push_back({child, 1, tick});
    return parentSpecies;
}

void SpeciesTracker::onBirth(uint32_t id) {
    if (!valid(id)) return;
    Species& s = get(id);
    if (!s.alive()) { // resurrected by a late birth: treat as alive again
        s.extinctTick = -1;
        ++aliveCount_;
    }
    ++s.population;
    ++s.totalBorn;
    s.peakPopulation = std::max(s.peakPopulation, s.population);
}

bool SpeciesTracker::onDeath(uint32_t id, int64_t tick) {
    if (!valid(id)) return false;
    Species& s = get(id);
    if (--s.population <= 0 && s.alive()) {
        s.population = 0;
        s.extinctTick = tick;
        --aliveCount_;
        return true;
    }
    return false;
}

void SpeciesTracker::recordHistory(int64_t tick) {
    for (Species& s : all_) {
        if (!s.alive()) {
            // Record a single trailing zero after extinction, then stop.
            if (s.historyStart < 0) continue;
            if (s.historyStart + int64_t(s.history.size() - 1) * HISTORY_INTERVAL >= s.extinctTick) continue;
        }
        if (s.historyStart < 0) s.historyStart = tick;
        s.history.push_back(uint16_t(std::min(s.population, 65535)));
        s.track.push_back(s.centroid);
    }
}

} // namespace pm
