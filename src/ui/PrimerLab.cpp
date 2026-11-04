#include "ui/PrimerLab.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "implot.h"
#include "ui/Icons.hpp"

namespace pm {

namespace {
const ImVec4 MUTED{0.502f, 0.580f, 0.604f, 1.f};
const ImVec4 ACCENT{0.247f, 0.816f, 0.722f, 1.f};
const ImVec4 AMBER{0.949f, 0.710f, 0.290f, 1.f};
} // namespace

Vec2 PrimerLab::edgePoint() {
    float t = rng_.range(0.f, 4.f);
    float u = (t - std::floor(t)) * ARENA;
    switch (int(t)) {
        case 0: return {u, 0.f};
        case 1: return {ARENA, u};
        case 2: return {u, ARENA};
        default: return {0.f, u};
    }
}

void PrimerLab::reset() {
    blobs_.clear();
    history_.clear();
    day_ = 0;
    for (int i = 0; i < startBlobs_; ++i) {
        Blob b;
        b.speed = b.size = b.sense = 1.f;
        blobs_.push_back(b);
    }
    initialised_ = true;
    startDay();
}

void PrimerLab::startDay() {
    food_.clear();
    for (int i = 0; i < foodPerDay_; ++i) food_.push_back({rng_.range(8.f, ARENA - 8.f), rng_.range(8.f, ARENA - 8.f)});
    for (Blob& b : blobs_) {
        b.home = edgePoint();
        b.pos = b.home;
        Vec2 toCenter = Vec2(ARENA * 0.5f, ARENA * 0.5f) - b.pos;
        b.dir = toCenter / std::max(1e-3f, toCenter.length());
        b.energy = DAY_ENERGY;
        b.eaten = 0;
        b.homeSafe = false;
        b.alive = true;
        b.eatenByOther = false;
    }
    stepInDay_ = 0;
    snapSpeed_.clear();
    snapSize_.clear();
    snapSense_.clear();
    for (const Blob& b : blobs_) {
        snapSpeed_.push_back(b.speed);
        snapSize_.push_back(b.size);
        snapSense_.push_back(b.sense);
    }
    DayStats d{int(blobs_.size()), 0, 0, 0};
    for (const Blob& b : blobs_) d.speed += b.speed, d.size += b.size, d.sense += b.sense;
    if (!blobs_.empty()) d.speed /= blobs_.size(), d.size /= blobs_.size(), d.sense /= blobs_.size();
    history_.push_back(d);
}

void PrimerLab::step() {
    ++stepInDay_;
    bool anyActive = false;
    for (size_t i = 0; i < blobs_.size(); ++i) {
        Blob& b = blobs_[i];
        if (!b.alive || b.homeSafe || b.energy <= 0.f) continue;
        anyActive = true;
        const float stride = 0.35f * b.speed;
        const float c = cost(b);
        const float distHome = std::min({b.pos.x, b.pos.y, ARENA - b.pos.x, ARENA - b.pos.y});
        const float stepsHome = distHome / stride;
        const bool goHome = b.eaten >= 2 || (b.eaten >= 1 && b.energy < c * stepsHome * 1.3f + c * 10.f);
        Vec2 want = b.dir;
        if (goHome) {
            // Head for the nearest edge.
            if (distHome == b.pos.x) want = {-1, 0};
            else if (distHome == b.pos.y) want = {0, -1};
            else if (distHome == ARENA - b.pos.x) want = {1, 0};
            else want = {0, 1};
        } else {
            const float senseR = b.sense * 9.f;
            float best = senseR * senseR;
            bool fleeing = false;
            // Flee anything 20% bigger that can eat us.
            if (predation_)
                for (const Blob& o : blobs_) {
                    if (&o == &b || !o.alive || o.homeSafe || o.size < b.size * 1.2f) continue;
                    Vec2 d = b.pos - o.pos;
                    float d2 = d.lengthSq();
                    if (d2 < best) {
                        best = d2;
                        want = d / std::max(1e-3f, std::sqrt(d2));
                        fleeing = true;
                    }
                }
            if (!fleeing) {
                best = senseR * senseR;
                bool found = false;
                for (const Vec2& f : food_) {
                    float d2 = (f - b.pos).lengthSq();
                    if (d2 < best) { best = d2; want = (f - b.pos) / std::max(1e-3f, std::sqrt(d2)); found = true; }
                }
                if (predation_)
                    for (const Blob& o : blobs_) {
                        if (&o == &b || !o.alive || o.homeSafe || b.size < o.size * 1.2f) continue;
                        float d2 = (o.pos - b.pos).lengthSq();
                        if (d2 < best) { best = d2; want = (o.pos - b.pos) / std::max(1e-3f, std::sqrt(d2)); found = true; }
                    }
                if (!found) {
                    // Wander: gently turning random walk.
                    float a = std::atan2(b.dir.y, b.dir.x) + rng_.normal() * 0.35f;
                    want = {std::cos(a), std::sin(a)};
                }
            }
        }
        b.dir = want;
        b.pos += b.dir * stride;
        if (!goHome) {
            if (b.pos.x < 1 || b.pos.x > ARENA - 1) b.dir.x = -b.dir.x;
            if (b.pos.y < 1 || b.pos.y > ARENA - 1) b.dir.y = -b.dir.y;
        }
        b.pos.x = clampf(b.pos.x, 0.f, ARENA);
        b.pos.y = clampf(b.pos.y, 0.f, ARENA);
        b.energy -= c;
        b.wobble += 0.3f + b.speed * 0.2f;
        // Eat food.
        const float reach = b.size * 1.2f + 0.8f;
        for (size_t k = 0; k < food_.size(); ++k)
            if ((food_[k] - b.pos).lengthSq() < reach * reach && b.eaten < 2) {
                food_[k] = food_.back();
                food_.pop_back();
                ++b.eaten;
                break;
            }
        // Eat a smaller blob.
        if (predation_ && b.eaten < 2)
            for (Blob& o : blobs_) {
                if (&o == &b || !o.alive || o.homeSafe || b.size < o.size * 1.2f) continue;
                if ((o.pos - b.pos).lengthSq() < reach * reach) {
                    o.alive = false;
                    o.eatenByOther = true;
                    ++b.eaten;
                    break;
                }
            }
        if (goHome && std::min({b.pos.x, b.pos.y, ARENA - b.pos.x, ARENA - b.pos.y}) < 0.5f) b.homeSafe = true;
    }
    if (!anyActive || stepInDay_ >= MAX_STEPS) endDay();
}

void PrimerLab::endDay() {
    std::vector<Blob> next;
    for (const Blob& b : blobs_) {
        if (!b.alive || !b.homeSafe || b.eaten < 1) continue;
        next.push_back(b);
        if (b.eaten >= 2) {
            Blob child = b;
            auto mut = [&](float v, bool on) { return on ? clampf(v + rng_.normal() * mutation_, 0.2f, 4.f) : v; };
            child.speed = mut(b.speed, evolveSpeed_);
            child.size = mut(b.size, evolveSize_);
            child.sense = mut(b.sense, evolveSense_);
            next.push_back(child);
        }
    }
    blobs_ = std::move(next);
    ++day_;
    if (blobs_.empty()) {
        running_ = false;
        history_.push_back({0, 0, 0, 0});
        return;
    }
    startDay();
}

void PrimerLab::draw(bool* open, float dt, ImFont* titleFont) {
    ImGui::SetNextWindowSize(ImVec2(1100, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FLASK " Primer Lab: speed, size and sense", open)) {
        ImGui::End();
        return;
    }
    if (!initialised_) reset();
    // Controls
    if (ImGui::Button(running_ ? ICON_PAUSE " Pause" : ICON_PLAY " Run")) running_ = !running_;
    ImGui::SameLine();
    if (ImGui::Button(ICON_REFRESH " Restart")) {
        reset();
        running_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("##speed", &simSpeed_, 0.5f, 60.f, "%.1f steps/frame", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    ImGui::SliderInt("Food/day", &foodPerDay_, 5, 200);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.f);
    ImGui::SliderFloat("Mutation", &mutation_, 0.f, 0.5f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    ImGui::SliderInt("Start", &startBlobs_, 1, 100);
    ImGui::Checkbox("Evolve speed", &evolveSpeed_);
    ImGui::SameLine();
    ImGui::Checkbox("Evolve size", &evolveSize_);
    ImGui::SameLine();
    ImGui::Checkbox("Evolve sense", &evolveSense_);
    ImGui::SameLine();
    ImGui::Checkbox("Big eat small (20% rule)", &predation_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, MUTED);
    ImGui::Text("   Day %d · %zu blobs · %zu food left", day_, blobs_.size(), food_.size());
    ImGui::PopStyleColor();

    if (running_) {
        accum_ += simSpeed_ * std::min(dt * 60.f, 3.f);
        int steps = 0;
        while (accum_ >= 1.f && steps < 400) {
            step();
            accum_ -= 1.f;
            ++steps;
            if (!running_) break;
        }
    }

    // Arena
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::min(avail.y, avail.x * 0.5f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##arena", ImVec2(side, side));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float s = (side - 24.f) / ARENA;
    ImVec2 o = p0 + ImVec2(12, 12);
    auto P = [&](Vec2 v) { return o + ImVec2(v.x * s, v.y * s); };
    dl->AddRectFilled(p0, p0 + ImVec2(side, side), IM_COL32(52, 74, 96, 255), 10.f);           // "home" border
    dl->AddRectFilled(o, o + ImVec2(ARENA * s, ARENA * s), IM_COL32(94, 168, 108, 255), 4.f); // field
    for (int g = 1; g < 10; ++g) {
        float t = ARENA * float(g) / 10.f;
        dl->AddLine(P({t, 0}), P({t, ARENA}), IM_COL32(255, 255, 255, 14));
        dl->AddLine(P({0, t}), P({ARENA, t}), IM_COL32(255, 255, 255, 14));
    }
    dl->PushClipRect(p0, p0 + ImVec2(side, side), true);
    for (const Vec2& f : food_) dl->AddCircleFilled(P(f), std::max(2.5f, 1.f * s), IM_COL32(60, 230, 90, 255), 10);
    for (const Blob& b : blobs_) {
        if (!b.alive) continue;
        const float r = std::max(3.5f, b.size * 1.9f * s);
        // Colour by speed (Primer-ish blue, warming for fast blobs).
        float t = clamp01((b.speed - 0.5f) / 2.f);
        Color col = mix({0.26f, 0.52f, 0.95f, 1}, {0.95f, 0.35f, 0.45f, 1}, t);
        if (b.energy <= 0.f && !b.homeSafe) col = mix(col, {0.4f, 0.4f, 0.45f, 1}, 0.7f);
        ImU32 c32 = ImGui::ColorConvertFloat4ToU32(ImVec4(col.r, col.g, col.b, b.homeSafe ? 0.55f : 1.f));
        ImVec2 c = P(b.pos);
        if (evolveSense_ && blobs_.size() < 120)
            dl->AddCircle(c, b.sense * 9.f * s, IM_COL32(255, 255, 255, 22), 32, 1.f);
        float squash = 1.f + 0.06f * std::sin(b.wobble);
        dl->AddEllipseFilled(c, ImVec2(r * squash, r / squash), c32);
        // Eyes looking where it is heading.
        ImVec2 d(b.dir.x, b.dir.y), n(-b.dir.y, b.dir.x);
        for (float sgn : {-1.f, 1.f}) {
            ImVec2 e = c + d * (r * 0.35f) + n * (sgn * r * 0.38f);
            dl->AddCircleFilled(e, r * 0.3f, IM_COL32(255, 255, 255, 255), 10);
            dl->AddCircleFilled(e + d * (r * 0.1f), r * 0.15f, IM_COL32(20, 20, 30, 255), 8);
        }
        if (b.eaten > 0)
            dl->AddText(c + ImVec2(r * 0.8f, -r * 1.6f), IM_COL32(255, 255, 255, 200), b.eaten >= 2 ? "++" : "+");
    }
    dl->PopClipRect();
    ImGui::SameLine();

    // Charts
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, MUTED);
    ImGui::TextWrapped("Each day, blobs leave the edge to find food. 1 food = survive, 2 = reproduce (with mutation). "
                       "Every step costs size^3 x speed^2 + sense energy. Blobs 20%% bigger can eat smaller ones.");
    ImGui::PopStyleColor();
    const float chartW = ImGui::GetContentRegionAvail().x;
    const float chartH = (avail.y - ImGui::GetTextLineHeightWithSpacing() * 3.f) / 2.f;
    if (history_.size() > 1 && ImPlot::BeginPlot("##primerpop", ImVec2(chartW, chartH), ImPlotFlags_NoTitle)) {
        ImPlot::SetupAxes("day", nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxis(ImAxis_Y2, "trait mean", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest);
        ImPlotSpec ps;
        ps.Stride = sizeof(DayStats);
        ps.LineColor = ImVec4(0.86f, 0.9f, 0.91f, 1.f);
        ps.LineWeight = 2.f;
        std::vector<float> days(history_.size());
        for (size_t k = 0; k < days.size(); ++k) days[k] = float(k);
        std::vector<float> pop(history_.size()), sp(history_.size()), sz(history_.size()), se(history_.size());
        for (size_t k = 0; k < history_.size(); ++k) {
            pop[k] = float(history_[k].population);
            sp[k] = history_[k].speed;
            sz[k] = history_[k].size;
            se[k] = history_[k].sense;
        }
        ImPlotSpec a;
        a.LineColor = ImVec4(0.86f, 0.9f, 0.91f, 1.f);
        a.LineWeight = 2.f;
        ImPlot::PlotLine("Population", days.data(), pop.data(), int(days.size()), a);
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        ImPlotSpec b1;
        b1.LineColor = ImVec4(0.3f, 0.55f, 0.95f, 1.f);
        ImPlot::PlotLine("Speed", days.data(), sp.data(), int(days.size()), b1);
        ImPlotSpec b2;
        b2.LineColor = AMBER;
        ImPlot::PlotLine("Size", days.data(), sz.data(), int(days.size()), b2);
        ImPlotSpec b3;
        b3.LineColor = ACCENT;
        ImPlot::PlotLine("Sense", days.data(), se.data(), int(days.size()), b3);
        ImPlot::EndPlot();
    }
    // Trait histograms for today's population (Primer's signature bar charts).
    const float histW = (chartW - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
    auto hist = [&](const char* id, const char* label, const std::vector<float>& v, ImVec4 col) {
        if (v.empty()) return;
        if (ImPlot::BeginPlot(id, ImVec2(histW, chartH), ImPlotFlags_NoLegend | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(label, nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, 3.0, ImPlotCond_Always);
            ImPlotSpec hs;
            hs.FillColor = col;
            hs.LineColor = col;
            ImPlot::PlotHistogram(label, v.data(), int(v.size()), 24, 1.0, ImPlotRange(0.2, 3.0), hs);
            ImPlot::EndPlot();
        }
    };
    hist("##hsp", "Speed", snapSpeed_, ImVec4(0.3f, 0.55f, 0.95f, 1.f));
    ImGui::SameLine();
    hist("##hsz", "Size", snapSize_, AMBER);
    ImGui::SameLine();
    hist("##hse", "Sense", snapSense_, ACCENT);
    ImGui::EndGroup();
    (void)titleFont;
    ImGui::End();
}

} // namespace pm
