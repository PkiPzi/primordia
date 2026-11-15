// The behaviour detector. Nothing here scripts behaviour: it watches what evolution produced and writes the
// interesting parts into the event log as EventKind::Discovery entries (the "Discoveries" tab).
#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>

#include "sim/World.hpp"

namespace pm {

namespace {

enum Tag : uint8_t {
    T_CALL_CTX = 1,    // + channel * 8 + context  (1..16)
    T_CALL_REPLY = 20, // + channel
    T_FARM = 30,
    T_POACH = 31,
    T_LEARN = 32,
    T_FAST_EVO = 33,
    T_CANNIBAL = 34,
    T_CRASH = 35,
    T_ACT = 40,        // + action * 8 + context (overclock = 2, phase = 3): 56..78
    T_BLINK_ESC = 90,
    T_BLINK_AMBUSH = 91,
    T_HIVE = 92,
    T_PLANT_DEF = 93,
    T_ARMS_RACE = 94,
    T_PLANT_SPREAD = 95,
    T_AMPHIBIAN = 96,
    T_MARINE = 97,
    T_FLIER = 98,
    T_QUILLS = 99,
    T_SUNBATHER = 100,
};

const char* const CTX_WHEN[BehaviorAcc::CTX] = {"when hurt", "when a bigger creature is near", "while eating",
                                                 "when ready to breed", "when starving", "while attacking"};
// What a call in that context might be, as a hint (calls only).
const char* const CALL_HINT[BehaviorAcc::CTX] = {" (a distress cry)", " (a warning call)", " (a food call)",
                                                 " (a mating call)", " (a begging call)", " (a battle cry)"};
const char* const ACT_NAME[BehaviorAcc::ACT] = {"Call A", "Call B", "overclock their brains", "phase out of reality"};

std::string pct(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", 100.f * v);
    return b;
}

} // namespace

const char* skillName(int i) {
    static const char* names[N_SKILLS] = {"Grazing",  "Hunting",  "Speed",         "Long life", "Breeding", "Sharing",
                                          "Calling",  "Farming",  "Lifetime learning", "Hive thinking", "Alien tricks",
                                          "Toughness"};
    return names[i];
}

std::string World::bestAtText(uint32_t speciesId) const {
    const SpeciesSkills* sk = skillsOf(speciesId);
    if (!sk) return "too few adults to rank";
    std::string t;
    for (int k = 0; k < 3 && sk->best[k] >= 0; ++k) t += (k ? ", " : "") + std::string(skillName(sk->best[k]));
    return t.empty() ? "no standout skill" : t;
}

uint32_t genusKey(const std::string& genus) {
    uint32_t h = 2166136261u;
    for (unsigned char ch : genus) h = (h ^ ch) * 16777619u;
    return h;
}

BehaviorAcc& World::behaviorOf(uint32_t speciesId) {
    if (behav_.size() < species.all().size()) behav_.resize(species.all().size());
    return behav_[(speciesId ? speciesId : 1) - 1];
}

uint64_t World::discoveryKey(uint8_t tag, uint32_t speciesId, uint32_t bucket) const {
    // One entry per behaviour per genus: close relatives doing the same thing are not news.
    const uint64_t who = speciesId && species.valid(speciesId) ? genusKey(species.get(speciesId).genus) : 0;
    return (who << 32) | (uint64_t(bucket & 0xffffffu) << 8) | tag;
}

void World::discover(uint8_t tag, uint32_t speciesId, Vec2 pos, std::string text, uint32_t bucket) {
    const uint64_t key = discoveryKey(tag, speciesId, bucket);
    if (announced_.count(key)) return;
    // Ongoing behaviours (no bucket) must hold for 2000 ticks without a break: one lucky scan is not a behaviour.
    // Bucketed entries are one-off events (a crash, a fast year) and are reported at once.
    if (!bucket) {
        constexpr int SCAN_EVERY = 50, NEEDED = 40; // census interval, scans in a row
        Streak& st = streaks_[key];
        st.scans = tickCount - st.lastTick <= SCAN_EVERY ? st.scans + 1 : 1;
        st.lastTick = tickCount;
        if (st.scans < NEEDED) return;
        streaks_.erase(key);
        // The first three lineages to do something are news; after that it is just how this world works.
        int same = 0;
        for (const WorldEvent& e : events.items) same += e.kind == EventKind::Discovery && e.tag == tag;
        if (same >= 3) {
            announced_.insert(key);
            return;
        }
    }
    announced_.insert(key);
    events.push({tickCount, EventKind::Discovery, speciesId, pos, std::move(text), tag});
}

void World::scanBehaviours() {
    if (!species.all().size()) return;
    behav_.resize(species.all().size());
    const auto name = [&](uint32_t sid) { return species.get(sid).name(); };

    // 1. Sample every creature into its species' rolling tallies.
    std::vector<float> learned(behav_.size(), 0.f), plastic(behav_.size(), 0.f);
    for (const Creature& c : creatures) {
        if (!species.valid(c.speciesId) || c.growth < 1.f) continue;
        BehaviorAcc& a = behav_[c.speciesId - 1];
        a.n += 1.f;
        bool ctx[BehaviorAcc::CTX] = {c.pain > 0.25f, false, (c.flags & CF_EATING) != 0,
                                      c.reproCooldown <= 0 && c.energy > 0.55f * c.maxEnergy, c.energy < 0.2f * c.maxEnergy,
                                      (c.flags & CF_ATTACKING) != 0};
        for (int e = 0; e < NUM_EYES; ++e)
            if (c.in[e * EC_COUNT + EC_CREATURE] > 0.3f && c.in[e * EC_COUNT + EC_SIZE] > 0.25f) ctx[1] = true;
        for (int k = 0; k < BehaviorAcc::CTX; ++k) a.nCtx[k] += ctx[k];
        const float speed = c.vel.length();
        const bool doing[BehaviorAcc::ACT] = {c.call[0] > 0.3f, c.call[1] > 0.3f, c.out[OUT_OVERCLOCK] > 0.5f,
                                              c.out[OUT_PHASE] > 0.3f};
        for (int ac = 0; ac < BehaviorAcc::ACT; ++ac)
            if (doing[ac]) {
                a.acting[ac] += 1.f;
                for (int k = 0; k < BehaviorAcc::CTX; ++k) a.actingIn[ac][k] += ctx[k];
            }
        if (c.linkN > 0.f) { a.linkers += 1.f; a.linkN += c.linkN; a.linkSync += c.linkSync; }
        for (int ch = 0; ch < 2; ++ch) {
            const float heard = c.in[ch ? IN_HEAR_B : IN_HEAR_A];
            if (heard > 0.2f) { a.nHear[ch] += 1.f; a.spdHear[ch] += speed; }
            else if (heard < 0.02f) { a.nQuiet[ch] += 1.f; a.spdQuiet[ch] += speed; }
        }
        if (c.tend > 0.3f) a.tenders += 1.f;
        if (c.flags & CF_IN_WATER) a.wet += 1.f;
        plastic[c.speciesId - 1] += c.ph.learnRate;
        float l1 = 0.f;
        for (int j = 0; j < N_LEARN; ++j) l1 += std::fabs(c.learn[j]);
        learned[c.speciesId - 1] += l1 / float(N_LEARN);
    }

    // 1b. Skill profile of each species: what it does best compared with the others alive.
    {
        const size_t ns = behav_.size();
        skills.assign(ns, SpeciesSkills{});
        std::vector<std::array<double, N_SKILLS>> sum(ns);
        std::vector<int> cnt(ns, 0);
        for (auto& row : sum) row.fill(0.0);
        for (const Creature& c : creatures) {
            if (!species.valid(c.speciesId) || c.growth < 1.f) continue;
            auto& v = sum[c.speciesId - 1];
            const double age = double(c.age) + 1.0;
            float l1 = 0.f;
            for (int j = 0; j < N_LEARN; ++j) l1 += std::fabs(c.learn[j]);
            v[0] += c.eatenPlant / age * 100.0;                            // grazing: energy per 100 ticks of life
            v[1] += c.eatenMeat / age * 100.0 + c.kills * 100.0 / age;     // hunting
            v[2] += c.vel.length();                                         // speed
            v[3] += c.ph.lifespan / 10000.0;                                // long life
            v[4] += c.children * 1000.0 / age;                              // breeding
            v[5] += c.shared / age * 100.0;                                 // sharing
            v[6] += (c.call[0] > 0.3f || c.call[1] > 0.3f) ? 1.0 : 0.0;     // calling
            v[8] += l1 / float(N_LEARN);                                    // lifetime learning
            v[9] += c.linkN > 0.f ? c.linkSync * std::min(c.linkN, 4.f) / 4.f : 0.f; // hive thinking
            v[11] += c.ph.armor + c.ph.toxin;                               // toughness
            ++cnt[c.speciesId - 1];
        }
        // Absolute floors: a skill nobody really has is never "best".
        static const double FLOOR[N_SKILLS] = {0, 0, 0, 0, 0, 0.002, 0.05, 0.05, 0.05, 0.1, 0.02, 0};
        std::vector<size_t> ok;
        for (size_t s = 0; s < ns; ++s) {
            if (cnt[s] < 15 || !species.all()[s].alive()) continue;
            const BehaviorAcc& a = behav_[s];
            for (int k = 0; k < N_SKILLS; ++k) sum[s][size_t(k)] /= cnt[s];
            sum[s][7] = a.n > 0 ? double(a.tenders) / a.n + (a.bites > 0 ? 0.5 * a.bitesOwn / a.bites : 0.0) : 0.0;
            sum[s][10] = a.n > 0 ? double(a.acting[2] + a.acting[3] + a.blinks) / a.n : 0.0;
            for (int k = 0; k < N_SKILLS; ++k) skills[s].raw[k] = float(sum[s][size_t(k)]);
            skills[s].valid = true;
            ok.push_back(s);
        }
        if (ok.size() >= 3)
            for (int k = 0; k < N_SKILLS; ++k) {
                double m = 0, v = 0;
                for (size_t s : ok) m += skills[s].raw[k];
                m /= double(ok.size());
                for (size_t s : ok) v += (skills[s].raw[k] - m) * (skills[s].raw[k] - m);
                const double sd = std::sqrt(v / double(ok.size()));
                if (sd < 1e-9) continue;
                for (size_t s : ok) skills[s].z[k] = float((skills[s].raw[k] - m) / sd);
            }
        for (size_t s : ok) {
            int order[N_SKILLS];
            for (int k = 0; k < N_SKILLS; ++k) order[k] = k;
            std::sort(order, order + N_SKILLS, [&](int a, int b) { return skills[s].z[a] > skills[s].z[b]; });
            int n = 0;
            for (int k = 0; k < N_SKILLS && n < 3; ++k)
                if (skills[s].z[order[k]] >= 1.0f && skills[s].raw[order[k]] > FLOOR[order[k]]) skills[s].best[n++] = order[k];
        }
    }

    // 2. Look for behaviours that clearly stand out. Not in the first half year: founders are random genomes.
    if (tickCount < params.yearLength / 2) return;
    for (size_t s = 0; s < behav_.size(); ++s) {
        BehaviorAcc& a = behav_[s];
        const Species& sp = species.all()[s];
        const uint32_t sid = uint32_t(s + 1);
        if (!sp.alive() || a.n < 40.f) continue;

        for (int ac = 0; ac < BehaviorAcc::ACT; ++ac)
            for (int k = 0; k < BehaviorAcc::CTX; ++k) {
                const float inCtx = a.nCtx[k], outCtx = a.n - a.nCtx[k];
                if (inCtx < 30.f || outCtx < 30.f || a.actingIn[ac][k] < 10.f) continue;
                const float pIn = a.actingIn[ac][k] / inCtx;
                const float pOut = (a.acting[ac] - a.actingIn[ac][k]) / outCtx;
                if (pIn >= 0.25f && pIn >= 3.f * std::max(pOut, 0.02f))
                    discover(uint8_t(ac < 2 ? T_CALL_CTX + ac * 8 + k : T_ACT + ac * 8 + k), sid, sp.centroid,
                             name(sid) + " use " + ACT_NAME[ac] + " " + CTX_WHEN[k] + (ac < 2 ? CALL_HINT[k] : "") + ": " +
                                 pct(pIn) + " of such moments vs " + pct(pOut) + " otherwise");
            }
        for (int ch = 0; ch < 2; ++ch) {
            const char* chName = ch ? "Call B" : "Call A";
            if (a.nHear[ch] >= 30.f && a.nQuiet[ch] >= 30.f && a.acting[ch] >= 10.f && a.acting[ch] >= 0.03f * a.n) {
                const float sh = a.spdHear[ch] / a.nHear[ch], sq = std::max(0.05f, a.spdQuiet[ch] / a.nQuiet[ch]);
                const float ratio = sh / sq;
                if (ratio >= 1.6f || ratio <= 0.55f) {
                    char b[160];
                    std::snprintf(b, sizeof b, "%s react to %s: they %s (%.1fx the usual speed)", name(sid).c_str(), chName,
                                  ratio > 1 ? "bolt" : "freeze", ratio);
                    discover(uint8_t(T_CALL_REPLY + ch), sid, sp.centroid, b);
                }
            }
        }
        if (a.bites >= 60.f) {
            // Farmers mostly eat what their own kind grew; poachers mostly live off another species' work. Two
            // species grazing each other's fields a little is just shared pasture, not news.
            if (a.bitesOwn >= 0.4f * a.bites && a.bitesOwn > 2.f * a.bitesOther)
                discover(T_FARM, sid, sp.centroid,
                         name(sid) + " are farmers: " + pct(a.bitesOwn / a.bites) + " of their grazing is on soil their own species tended");
            if (a.bitesOther >= 0.4f * a.bites && a.bitesOther > 2.f * a.bitesOwn && species.valid(a.otherTiller) &&
                a.otherTiller != sid &&
                species.get(a.otherTiller).genus != sp.genus) // grazing a sister species' fields is family business
                discover(T_POACH, sid, sp.centroid,
                         name(sid) + " poach the fields of " + name(a.otherTiller) + " (" + pct(a.bitesOther / a.bites) +
                             " of their bites)");
        }
        // Life in the sea: the body has to have changed (the Aquatic gene) and the animals have to actually live there.
        {
            const float aq = sp.meanGenes[G_AQUATIC], inSea = a.wet / a.n;
            if (aq > 0.7f && inSea > 0.65f)
                discover(T_MARINE, sid, sp.centroid,
                         name(sid) + " have left the land for good: sea animals that spend " + pct(inSea) + " of their lives in the water");
            else if (aq > 0.3f && inSea > 0.25f && inSea < 0.8f)
                discover(T_AMPHIBIAN, sid, sp.centroid,
                         name(sid) + " are amphibious: at home on the shore, " + pct(inSea) + " of the time in the water");
        }
        // New body plans: the gene has to have gone most of the way, species-wide.
        if (sp.meanGenes[G_WINGS] > 0.55f)
            discover(T_FLIER, sid, sp.centroid, name(sid) + " have taken to the air: winged bodies that cross water and dodge grounded hunters");
        if (sp.meanGenes[G_SPIKES] > 0.55f)
            discover(T_QUILLS, sid, sp.centroid, name(sid) + " grew quills: whoever bites them is hurt back");
        if (sp.meanGenes[G_PHOTO] > 0.55f)
            discover(T_SUNBATHER, sid, sp.centroid, name(sid) + " live on sunlight: green-skinned animals that feed like plants");
        const float meanLearned = learned[s] / std::max(1.f, a.n);
        if (plastic[s] / std::max(1.f, a.n) > 0.003f && meanLearned > 0.04f)
            discover(T_LEARN, sid, sp.centroid,
                     name(sid) + " learn during their lifetimes: brains rewire themselves toward what paid off");

        if (a.blinks >= 15.f) {
            if (a.blinkHurt >= 0.5f * a.blinks)
                discover(T_BLINK_ESC, sid, sp.centroid, name(sid) + " blink away when hurt: teleporting as an escape");
            else if (a.blinkAttack >= 0.5f * a.blinks)
                discover(T_BLINK_AMBUSH, sid, sp.centroid, name(sid) + " blink onto their prey: a teleport ambush");
        }
        if (a.linkers >= 20.f) {
            const float ln = a.linkN / a.linkers, sy = a.linkSync / a.linkers;
            if (sp.meanGenes[G_LINK] > 0.25f && ln >= 1.5f && sy >= 0.45f) {
                char b[200];
                std::snprintf(b, sizeof b, "%s think as a hive: brains linked to %.1f neighbours, %s in step", name(sid).c_str(),
                              ln, pct(sy).c_str());
                discover(T_HIVE, sid, sp.centroid, b);
            }
        }

        // Forget slowly so the tallies describe the last few thousand ticks.
        const float keep = 0.97f;
        a.n *= keep; a.tenders *= keep; a.bites *= keep; a.bitesOwn *= keep; a.bitesOther *= keep;
        a.blinks *= keep; a.blinkHurt *= keep; a.blinkAttack *= keep; a.wet *= keep;
        a.linkN *= keep; a.linkSync *= keep; a.linkers *= keep;
        for (int k = 0; k < BehaviorAcc::CTX; ++k) a.nCtx[k] *= keep;
        for (int ac = 0; ac < BehaviorAcc::ACT; ++ac) {
            a.acting[ac] *= keep;
            for (int k = 0; k < BehaviorAcc::CTX; ++k) a.actingIn[ac][k] *= keep;
        }
        for (int ch = 0; ch < 2; ++ch) { a.nHear[ch] *= keep; a.spdHear[ch] *= keep; a.nQuiet[ch] *= keep; a.spdQuiet[ch] *= keep; }
    }

    // 3. Once a year: which lineage changed the most?
    const int64_t year = tickCount / std::max(1, params.yearLength);
    if (year != lastGeneYear_) {
        lastGeneYear_ = year;
        prevGenes_.resize(species.all().size());
        for (size_t s = 0; s < species.all().size(); ++s) {
            const Species& sp = species.all()[s];
            if (!sp.alive()) continue;
            if (sp.population >= 25 && year > 1 && s < prevGenes_.size() && sp.bornTick < tickCount - params.yearLength) {
                int best = -1;
                float bestD = 0.f;
                for (int g = 0; g < G_COUNT; ++g) {
                    if (g == G_HUE || g == G_MATE_PREF || g == G_CLOCK) continue;
                    const float d = sp.meanGenes[g] - prevGenes_[s][size_t(g)];
                    if (std::fabs(d) > std::fabs(bestD)) { bestD = d; best = g; }
                }
                if (best >= 0 && std::fabs(bestD) >= 0.2f) {
                    char b[200];
                    std::snprintf(b, sizeof b, "%s evolved fast: %s %s from %.2f to %.2f in one year", sp.name().c_str(),
                                  GENE_INFO[best].name, bestD > 0 ? "rose" : "fell", prevGenes_[s][size_t(best)],
                                  sp.meanGenes[best]);
                    discover(T_FAST_EVO, sp.id, sp.centroid, b, uint32_t(year) + 1);
                }
            }
            prevGenes_[s] = sp.meanGenes;
        }
    }

    // 4. World-level moments.
    const float pop = float(creatures.size());
    recentPeak_ = std::max(recentPeak_ * 0.999f, pop);
    if (recentPeak_ > 400.f && pop < 0.45f * recentPeak_) {
        char b[160];
        std::snprintf(b, sizeof b, "Population crash: %d creatures left, down from a recent peak of %d", int(pop), int(recentPeak_));
        discover(T_CRASH, 0, {}, b, uint32_t(year) + 1);
        recentPeak_ = pop;
    }
    if (!stats.samples.empty()) {
        const StatSample& s = stats.samples.back();
        if (s.plantDefence > 0.3f)
            discover(T_PLANT_DEF, 0, {}, "The plants evolved chemical defences: biomass-weighted defence has reached " + pct(s.plantDefence));
        if (s.plantDefence > 0.25f && s.geneMean[G_DETOX] > 0.35f)
            discover(T_ARMS_RACE, 0, {}, "Plant-herbivore arms race: defended plants and detoxifying grazers now dominate");
        if (s.plantSpread > 0.7f)
            discover(T_PLANT_SPREAD, 0, {}, "The plants are colonising aggressively: seed dispersal has evolved to " + pct(s.plantSpread));
        if (s.cannibalism > 0.3f && s.attackRate > 0.05f && tickCount > params.yearLength / 2)
            discover(T_CANNIBAL, 0, {}, "Cannibalism is common: " + pct(s.cannibalism) + " of bites are creatures attacking their own species");
    }
}

} // namespace pm
