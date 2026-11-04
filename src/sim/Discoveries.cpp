// The behaviour detector. Nothing here scripts behaviour: it watches what evolution produced and writes the
// interesting parts into the event log as EventKind::Discovery entries (the "Discoveries" tab).
#include <algorithm>
#include <cmath>
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
};

const char* const CTX_MEANING[BehaviorAcc::CTX] = {
    "when hurt (a distress cry)",
    "when a bigger creature is near (a warning call)",
    "while eating (a food call)",
    "when ready to breed (a mating call)",
    "when starving (a begging call)",
};

std::string pct(float v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", 100.f * v);
    return b;
}

} // namespace

BehaviorAcc& World::behaviorOf(uint32_t speciesId) {
    if (behav_.size() < species.all().size()) behav_.resize(species.all().size());
    return behav_[(speciesId ? speciesId : 1) - 1];
}

void World::discover(uint8_t tag, uint32_t speciesId, Vec2 pos, std::string text, uint32_t bucket) {
    const uint64_t key = (uint64_t(speciesId) << 32) | (uint64_t(bucket) << 8) | tag;
    if (!announced_.insert(key).second) return;
    // Bucketed (repeatable) discoveries are keyed by tag alone after a reload; good enough for a journal.
    announced_.insert((uint64_t(speciesId) << 32) | tag);
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
                                      c.reproCooldown <= 0 && c.energy > 0.55f * c.maxEnergy, c.energy < 0.2f * c.maxEnergy};
        for (int e = 0; e < NUM_EYES; ++e)
            if (c.in[e * EC_COUNT + EC_CREATURE] > 0.3f && c.in[e * EC_COUNT + EC_SIZE] > 0.25f) ctx[1] = true;
        for (int k = 0; k < BehaviorAcc::CTX; ++k) a.nCtx[k] += ctx[k];
        const float speed = c.vel.length();
        for (int ch = 0; ch < 2; ++ch) {
            if (c.call[ch] > 0.3f) {
                a.calling[ch] += 1.f;
                for (int k = 0; k < BehaviorAcc::CTX; ++k) a.callingIn[ch][k] += ctx[k];
            }
            const float heard = c.in[ch ? IN_HEAR_B : IN_HEAR_A];
            if (heard > 0.2f) { a.nHear[ch] += 1.f; a.spdHear[ch] += speed; }
            else if (heard < 0.02f) { a.nQuiet[ch] += 1.f; a.spdQuiet[ch] += speed; }
        }
        if (c.tend > 0.3f) a.tenders += 1.f;
        plastic[c.speciesId - 1] += c.ph.learnRate;
        float l1 = 0.f;
        for (int j = 0; j < N_LEARN; ++j) l1 += std::fabs(c.learn[j]);
        learned[c.speciesId - 1] += l1 / float(N_LEARN);
    }

    // 2. Look for behaviours that clearly stand out.
    for (size_t s = 0; s < behav_.size(); ++s) {
        BehaviorAcc& a = behav_[s];
        const Species& sp = species.all()[s];
        const uint32_t sid = uint32_t(s + 1);
        if (!sp.alive() || a.n < 40.f) continue;

        for (int ch = 0; ch < 2; ++ch) {
            const char* chName = ch ? "Call B" : "Call A";
            for (int k = 0; k < BehaviorAcc::CTX; ++k) {
                const float inCtx = a.nCtx[k], outCtx = a.n - a.nCtx[k];
                if (inCtx < 30.f || outCtx < 30.f || a.callingIn[ch][k] < 10.f) continue;
                const float pIn = a.callingIn[ch][k] / inCtx;
                const float pOut = (a.calling[ch] - a.callingIn[ch][k]) / outCtx;
                if (pIn >= 0.25f && pIn >= 3.f * std::max(pOut, 0.02f))
                    discover(uint8_t(T_CALL_CTX + ch * 8 + k), sid, sp.centroid,
                             name(sid) + " use " + chName + " " + CTX_MEANING[k] + ": " + pct(pIn) +
                                 " of such moments vs " + pct(pOut) + " otherwise");
            }
            if (a.nHear[ch] >= 30.f && a.nQuiet[ch] >= 30.f && a.calling[ch] >= 10.f && a.calling[ch] >= 0.03f * a.n) {
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
            if (a.bitesOwn >= 0.3f * a.bites)
                discover(T_FARM, sid, sp.centroid,
                         name(sid) + " are farmers: " + pct(a.bitesOwn / a.bites) + " of their grazing is on soil their own species tended");
            if (a.bitesOther >= 0.25f * a.bites && species.valid(a.otherTiller) && a.otherTiller != sid)
                discover(T_POACH, sid, sp.centroid,
                         name(sid) + " poach the fields of " + name(a.otherTiller) + " (" + pct(a.bitesOther / a.bites) +
                             " of their bites)");
        }
        const float meanLearned = learned[s] / std::max(1.f, a.n);
        if (plastic[s] / std::max(1.f, a.n) > 0.003f && meanLearned > 0.04f)
            discover(T_LEARN, sid, sp.centroid,
                     name(sid) + " learn during their lifetimes: brains rewire themselves toward what paid off");

        // Forget slowly so the tallies describe the last few thousand ticks.
        const float keep = 0.97f;
        a.n *= keep; a.tenders *= keep; a.bites *= keep; a.bitesOwn *= keep; a.bitesOther *= keep;
        for (int k = 0; k < BehaviorAcc::CTX; ++k) a.nCtx[k] *= keep;
        for (int ch = 0; ch < 2; ++ch) {
            a.calling[ch] *= keep; a.nHear[ch] *= keep; a.spdHear[ch] *= keep; a.nQuiet[ch] *= keep; a.spdQuiet[ch] *= keep;
            for (int k = 0; k < BehaviorAcc::CTX; ++k) a.callingIn[ch][k] *= keep;
        }
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
                    discover(T_FAST_EVO, sp.id, sp.centroid, b, uint32_t(year));
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
        discover(T_CRASH, 0, {}, b, uint32_t(year));
        recentPeak_ = pop;
    }
    if (!stats.samples.empty()) {
        const StatSample& s = stats.samples.back();
        if (s.cannibalism > 0.3f && s.attackRate > 0.05f && tickCount > params.yearLength / 2)
            discover(T_CANNIBAL, 0, {}, "Cannibalism is common: " + pct(s.cannibalism) + " of bites are creatures attacking their own species");
    }
}

} // namespace pm
