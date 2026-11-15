#include "ui/Ui.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>

#include "core/ThreadPool.hpp"
#include "sim/Scenarios.hpp"
#include <SDL3/SDL_clipboard.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "ui/Icons.hpp"

namespace pm {

const float SPEED_TICKS[SPEED_COUNT] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 64.f, 256.f, 0.f, -1.f};
static const char* SPEED_LABELS[SPEED_COUNT] = {"1/4x", "1/2x", "1x", "2x", "4x", "8x", "16x", "64x", "256x", "MAX", "TURBO"};

const char* toolName(Tool t) {
    static const char* n[] = {"Select", "Feed", "Spawn", "Clone", "Smite", "Meteor", "Raise land", "Lower land", "Infect"};
    return n[int(t)];
}

namespace {

// Palette: deep-sea slate with a bioluminescent mint accent and amber for focus.
const ImVec4 C_BG{0.050f, 0.071f, 0.082f, 0.97f};
const ImVec4 C_PANEL{0.071f, 0.098f, 0.110f, 1.f};
const ImVec4 C_FRAME{0.098f, 0.137f, 0.157f, 1.f};
const ImVec4 C_FRAME_HI{0.133f, 0.188f, 0.212f, 1.f};
const ImVec4 C_ACCENT{0.247f, 0.816f, 0.722f, 1.f};
const ImVec4 C_ACCENT_DIM{0.157f, 0.471f, 0.427f, 1.f};
const ImVec4 C_AMBER{0.949f, 0.710f, 0.290f, 1.f};
const ImVec4 C_TEXT{0.863f, 0.902f, 0.910f, 1.f};
const ImVec4 C_MUTED{0.502f, 0.580f, 0.604f, 1.f};
const ImVec4 C_BORDER{0.133f, 0.196f, 0.227f, 1.f};
const ImVec4 C_RED{0.937f, 0.353f, 0.341f, 1.f};
const ImVec4 C_GREEN{0.416f, 0.851f, 0.459f, 1.f};

ImU32 u32(Color c, float a = -1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a < 0 ? c.a : a)); }
ImVec4 v4(Color c) { return ImVec4(c.r, c.g, c.b, c.a); }

void swatch(Color c, float size = 0.f) {
    if (size <= 0.f) size = ImGui::GetTextLineHeight() * 0.8f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float pad = (ImGui::GetTextLineHeight() - size) * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(p + ImVec2(0, pad), p + ImVec2(size, pad + size), u32(c), size * 0.3f);
    ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
}

void bar(float frac, ImU32 col, const char* overlay, float height = 0.f) {
    frac = clamp01(frac);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = height > 0 ? height : ImGui::GetTextLineHeight() + 4.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), ImGui::GetColorU32(C_FRAME), 4.f);
    dl->AddRectFilled(p, p + ImVec2(std::max(4.f, w * frac), h), col, 4.f);
    if (overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(p + ImVec2(8.f, (h - ts.y) * 0.5f), ImGui::GetColorU32(C_TEXT), overlay);
    }
    ImGui::Dummy(ImVec2(w, h));
}

std::string years(int64_t ticks, int yearLen) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f y", double(ticks) / double(std::max(1, yearLen)));
    return buf;
}

int formatYears(double value, char* buff, int size, void* data) {
    int yl = *static_cast<int*>(data);
    return std::snprintf(buff, size_t(size), "%.1fy", value / double(std::max(1, yl)));
}

// A friendly, deterministic personal name for each creature.
std::string personalName(uint32_t id) {
    static const char* a[] = {"Bo", "Zu", "Ki", "Mo", "Pi", "Lu", "Ta", "Ne", "Ru", "Fi", "Gu", "Wa", "Sa", "Do", "Yo", "Ba",
                              "Mi", "To", "Pe", "Ka", "Nu", "Ro", "Ve", "Ji"};
    static const char* b[] = {"mi", "lo", "ra", "bu", "ni", "ka", "po", "zi", "le", "du", "fa", "ri", "so", "mo", "tu", "ve"};
    static const char* c[] = {"", "", "n", "x", "s", "k", "", "l", "p", "", "m", "t"};
    uint32_t h = id * 2654435761u;
    std::string s = a[h % 24];
    s += b[(h >> 8) % 16];
    if ((h >> 16) % 3 == 0) s += b[(h >> 20) % 16];
    s += c[(h >> 24) % 12];
    return s;
}

Color creatureColorForUi(const World& w, const Creature& c) {
    return w.species.valid(c.speciesId) ? w.species.get(c.speciesId).color : hsv(c.genome.g[G_HUE], 0.6f, 0.95f);
}

const char* dietLabel(float d) { return d < 0.33f ? "Herbivore" : d < 0.66f ? "Omnivore" : "Carnivore"; }

float toCelsius(float t) { return 14.f + 26.f * t; }

// A plain-language reading of what a creature is doing right now, from its senses and actions.
const char* activityOf(const Creature& c) {
    const float* eye = c.in;
    float ahead = std::max({eye[1 * EC_COUNT + EC_CREATURE], eye[2 * EC_COUNT + EC_CREATURE], eye[3 * EC_COUNT + EC_CREATURE]});
    float bigAhead = 0.f;
    for (int e = 0; e < NUM_EYES; ++e)
        if (eye[e * EC_COUNT + EC_SIZE] > 0.25f) bigAhead = std::max(bigAhead, eye[e * EC_COUNT + EC_CREATURE]);
    const bool carnivore = c.ph.meatEff > c.ph.plantEff;
    if (c.infection > 0.f && c.health < 0.4f * c.maxHealth) return ICON_BUG " Sick and weak";
    if (c.flags & CF_POISONED) return ICON_WARNING " Poisoned!";
    if ((c.flags & CF_HIT) && carnivore) return ICON_BOLT " Hunting: biting prey";
    if (c.flags & CF_HIT) return ICON_BOLT " Fighting";
    if (c.out[OUT_ATTACK] > 0.f && ahead > 0.4f && carnivore) return ICON_PAW " Stalking prey";
    if (c.pain > 0.25f && std::fabs(c.out[OUT_THRUST]) > 0.4f) return ICON_FAST " Fleeing an attacker";
    if (bigAhead > 0.5f && c.out[OUT_THRUST] < -0.1f) return ICON_FAST " Backing away from a bigger creature";
    if (c.flags & CF_SHARING) return ICON_GIFT " Feeding a neighbour";
    if ((c.flags & CF_EATING) && c.eatenMeat > 0.f && carnivore) return ICON_CUBE " Eating a carcass";
    if (c.flags & CF_EATING) return ICON_LEAF " Grazing";
    if (c.flags & CF_IN_WATER) return ICON_TINT " Swimming";
    if (c.hydration < 0.25f) return ICON_TINT " Thirsty, looking for water";
    if (c.energy < 0.2f * c.maxEnergy) return ICON_SEARCH " Hungry, searching for food";
    if (c.out[OUT_REPRODUCE] > 0.f && c.growth >= 1.f && c.reproCooldown > 0) return ICON_HEART " Recovering after giving birth";
    if (std::fabs(c.out[OUT_THRUST]) < 0.12f) return ICON_CLOCK " Resting";
    if (c.pheromone > 0.2f) return ICON_SHARE " Marking a trail";
    if (std::fabs(c.signal) > 0.5f) return ICON_COMMENT " Signalling";
    return ICON_GLOBE " Wandering";
}

void sectionLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

bool iconButton(const char* icon, const char* tooltip, bool active = false, ImVec2 size = ImVec2(0, 0)) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_DIM);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C_ACCENT_DIM);
    }
    bool r = ImGui::Button(icon, size);
    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return r;
}

ImVec4 eventColor(EventKind k) {
    switch (k) {
        case EventKind::Speciation: return C_ACCENT;
        case EventKind::Extinction: return C_MUTED;
        case EventKind::Milestone: return C_AMBER;
        case EventKind::Catastrophe: return C_RED;
        case EventKind::Discovery: return C_GREEN;
        default: return C_TEXT;
    }
}
const char* eventIcon(EventKind k) {
    switch (k) {
        case EventKind::Speciation: return ICON_SITEMAP;
        case EventKind::Extinction: return ICON_TIMES;
        case EventKind::Milestone: return ICON_TROPHY;
        case EventKind::Catastrophe: return ICON_WARNING;
        case EventKind::Discovery: return ICON_EYE;
        default: return ICON_INFO;
    }
}

// Testing aid: a tab whose label contains g_forceTab gets selected (see --focus tab:NAME).
std::string g_forceTab;
bool tabItem(const char* label, bool* open = nullptr, ImGuiTabItemFlags flags = 0) {
    if (!g_forceTab.empty() && std::string(label).find(g_forceTab) != std::string::npos) flags |= ImGuiTabItemFlags_SetSelected;
    return ImGui::BeginTabItem(label, open, flags);
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void Ui::init(float scale, const std::string& assetDir) {
    scale_ = scale;
    ImGuiIO& io = ImGui::GetIO();
    const std::string body = assetDir + "fonts/NotoSans-Regular.ttf";
    const std::string semi = assetDir + "fonts/NotoSans-SemiBold.ttf";
    const std::string mono = assetDir + "fonts/JetBrainsMonoNerdFont-Regular.ttf";
    static const ImWchar iconRanges[] = {ICON_MIN, ICON_MAX, 0};
    auto addWithIcons = [&](const std::string& path, float size) -> ImFont* {
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size);
        if (!f) return nullptr;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphMinAdvanceX = size * 1.1f;
        cfg.GlyphOffset = ImVec2(0, 1.f);
        cfg.GlyphRanges = iconRanges;
        io.Fonts->AddFontFromFileTTF(mono.c_str(), size * 0.95f, &cfg);
        return f;
    };
    fontBody_ = addWithIcons(body, 16.f);
    if (!fontBody_) fontBody_ = io.Fonts->AddFontDefault();
    fontTitle_ = addWithIcons(semi, 19.f);
    if (!fontTitle_) fontTitle_ = fontBody_;
    fontMono_ = io.Fonts->AddFontFromFileTTF(mono.c_str(), 14.f);
    if (!fontMono_) fontMono_ = fontBody_;
    io.FontDefault = fontBody_;
    applyTheme(scale);
}

void Ui::applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.f;
    s.ChildRounding = 6.f;
    s.FrameRounding = 5.f;
    s.PopupRounding = 6.f;
    s.ScrollbarRounding = 6.f;
    s.GrabRounding = 4.f;
    s.TabRounding = 5.f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 3);
    s.ScrollbarSize = 12.f;
    s.WindowBorderSize = 1.f;
    s.FrameBorderSize = 0.f;
    s.TabBorderSize = 0.f;
    s.DockingSeparatorSize = 2.f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = C_MUTED;
    c[ImGuiCol_WindowBg] = C_BG;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.085f, 0.098f, 0.98f);
    c[ImGuiCol_Border] = C_BORDER;
    c[ImGuiCol_FrameBg] = C_FRAME;
    c[ImGuiCol_FrameBgHovered] = C_FRAME_HI;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.24f, 0.26f, 1.f);
    c[ImGuiCol_TitleBg] = C_PANEL;
    c[ImGuiCol_TitleBgActive] = C_PANEL;
    c[ImGuiCol_TitleBgCollapsed] = C_PANEL;
    c[ImGuiCol_MenuBarBg] = ImVec4(0.043f, 0.059f, 0.067f, 1.f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = C_FRAME_HI;
    c[ImGuiCol_ScrollbarGrabHovered] = C_ACCENT_DIM;
    c[ImGuiCol_ScrollbarGrabActive] = C_ACCENT;
    c[ImGuiCol_CheckMark] = C_ACCENT;
    c[ImGuiCol_SliderGrab] = C_ACCENT_DIM;
    c[ImGuiCol_SliderGrabActive] = C_ACCENT;
    c[ImGuiCol_Button] = C_FRAME;
    c[ImGuiCol_ButtonHovered] = C_FRAME_HI;
    c[ImGuiCol_ButtonActive] = C_ACCENT_DIM;
    c[ImGuiCol_Header] = ImVec4(0.157f, 0.471f, 0.427f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.157f, 0.471f, 0.427f, 0.65f);
    c[ImGuiCol_HeaderActive] = C_ACCENT_DIM;
    c[ImGuiCol_Separator] = C_BORDER;
    c[ImGuiCol_SeparatorHovered] = C_ACCENT_DIM;
    c[ImGuiCol_SeparatorActive] = C_ACCENT;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = C_ACCENT_DIM;
    c[ImGuiCol_ResizeGripActive] = C_ACCENT;
    c[ImGuiCol_Tab] = C_PANEL;
    c[ImGuiCol_TabHovered] = C_FRAME_HI;
    c[ImGuiCol_TabSelected] = ImVec4(0.098f, 0.161f, 0.169f, 1.f);
    c[ImGuiCol_TabSelectedOverline] = C_ACCENT;
    c[ImGuiCol_TabDimmed] = C_PANEL;
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.086f, 0.122f, 0.137f, 1.f);
    c[ImGuiCol_TabDimmedSelectedOverline] = C_ACCENT_DIM;
    c[ImGuiCol_DockingPreview] = ImVec4(0.247f, 0.816f, 0.722f, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotLines] = C_ACCENT;
    c[ImGuiCol_PlotHistogram] = C_ACCENT;
    c[ImGuiCol_TableHeaderBg] = C_PANEL;
    c[ImGuiCol_TableBorderStrong] = C_BORDER;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.11f, 0.16f, 0.18f, 1.f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.247f, 0.816f, 0.722f, 0.3f);
    c[ImGuiCol_NavCursor] = C_ACCENT;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;

    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding = ImVec2(6, 6);
    ps.LegendPadding = ImVec2(6, 6);
    ImVec4* pc = ps.Colors;
    pc[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    pc[ImPlotCol_PlotBg] = ImVec4(0.035f, 0.05f, 0.058f, 1.f);
    pc[ImPlotCol_PlotBorder] = C_BORDER;
    pc[ImPlotCol_LegendBg] = ImVec4(0.05f, 0.07f, 0.08f, 0.85f);
    pc[ImPlotCol_LegendBorder] = C_BORDER;
    pc[ImPlotCol_AxisGrid] = ImVec4(1, 1, 1, 0.06f);
    pc[ImPlotCol_AxisText] = C_MUTED;
}

void Ui::buildDefaultLayout(unsigned dockIdIn) {
    ImGuiID dock = dockIdIn;
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, int(ImGuiDockNodeFlags_DockSpace) | int(ImGuiDockNodeFlags_PassthruCentralNode));
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left, right, bottom, center = dock;
    // Keep usable minimum widths for the side panels on small screens.
    const float W = std::max(800.f, ImGui::GetMainViewport()->WorkSize.x);
    const float leftFrac = std::clamp(270.f / W, 0.17f, 0.3f);
    left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftFrac, nullptr, &center);
    const float rightFrac = std::clamp(400.f / (W * (1.f - leftFrac)), 0.3f, 0.45f);
    right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightFrac, nullptr, &center);
    bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.36f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);
    ImGui::DockBuilderDockWindow(ICON_MAGIC " Tools", left);
    ImGui::DockBuilderDockWindow(ICON_GLOBE " Environment", left);
    ImGui::DockBuilderDockWindow(ICON_MAP " Minimap", leftBottom);
    ImGui::DockBuilderDockWindow(ICON_SEARCH " Inspector", right);
    ImGui::DockBuilderDockWindow(ICON_BOLT " Brain", right);
    ImGui::DockBuilderDockWindow(ICON_USERS " Species", rightBottom);
    ImGui::DockBuilderDockWindow(ICON_SITEMAP " Tree of Life", rightBottom);
    ImGui::DockBuilderDockWindow(ICON_LINECHART " Population", bottom);
    ImGui::DockBuilderDockWindow(ICON_FLASK " Traits", bottom);
    ImGui::DockBuilderDockWindow(ICON_HISTORY " Events", bottom);
    ImGui::DockBuilderDockWindow(ICON_GAUGE " Performance", bottom);
    ImGui::DockBuilderFinish(dock);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void Ui::draw(AppState& app, float dt) {
    World& w = *app.world;
    w.watchId = app.selectedId;
    time_ += dt;
    app.renderer->beginFrameIcons();
    drawMenuBar(app);
    drawToolbar(app);
    drawStatusBar(app);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID dockId = ImGui::GetID("PrimordiaDock");
    if (!layoutBuilt_ || app.requestResetLayout) {
        if (app.requestResetLayout || !ImGui::DockBuilderGetNode(dockId)) buildDefaultLayout(dockId);
        layoutBuilt_ = true;
        app.requestResetLayout = false;
    }
    ImGui::DockSpaceOverViewport(dockId, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockId)) {
        viewX = central->Pos.x - vp->Pos.x;
        viewY = central->Pos.y - vp->Pos.y;
        viewW = central->Size.x;
        viewH = central->Size.y;
    } else {
        viewX = vp->WorkPos.x - vp->Pos.x;
        viewY = vp->WorkPos.y - vp->Pos.y;
        viewW = vp->WorkSize.x;
        viewH = vp->WorkSize.y;
    }

    for (const std::string& f : focusRequests) {
        if (f == "Genome Lab") showLab_ = true;
        if (f == "World report") showReport_ = true;
        if (f == "Preferences") showPrefs_ = true;
        if (f == "Performance") showPerformance_ = true;
        if (f == "Primer Lab") showPrimer_ = true;
        if (f.rfind("tab:", 0) == 0) g_forceTab = f.substr(4);
        if (f.rfind("Tour", 0) == 0 && tourStep_ < 0) tourStep_ = f.size() > 4 ? std::atoi(f.c_str() + 4) : 0;
    }
    if (showTools_) drawTools(app);
    if (!focusRequests.empty() && ImGui::GetFrameCount() > 3) {
        for (const std::string& f : focusRequests) {
            for (ImGuiWindow* win : ImGui::GetCurrentContext()->Windows)
                if (std::string(win->Name).find(f) != std::string::npos) ImGui::FocusWindow(win);
        }
        focusRequests.clear();
    }
    if (showEnvironment_) drawEnvironment(app);
    if (showMinimap_) drawMinimap(app);
    if (showInspector_) drawInspector(app);
    if (showBrain_) drawBrain(app);
    if (showSpecies_) drawSpecies(app);
    if (showPhylogeny_) drawPhylogeny(app);
    if (showPopulation_) drawPopulation(app);
    if (showTraits_) drawTraits(app);
    if (showEvents_) drawEvents(app);
    if (showPerformance_) drawPerformance(app);
    if (showHelp_) drawHelp(app);
    if (showEncyclopedia_) drawEncyclopedia(app);
    if (showLab_) drawGenomeLab(app);
    if (showReport_) drawReport(app);
    if (showPrefs_) drawPreferences(app);
    if (showPrimer_) primerLab_.draw(&showPrimer_, dt, fontTitle_);
    ImGui::GetStyle().FontScaleMain = app.uiScale;
    if (showDemo_) {
        ImGui::ShowDemoWindow(&showDemo_);
        ImPlot::ShowDemoWindow();
    }
    drawNewWorldModal(app);
    if (showWelcome) drawWelcome(app);
    if (app.openContext) {
        ImGui::OpenPopup("##creatureMenu");
        app.openContext = false;
    }
    if (ImGui::BeginPopup("##creatureMenu")) {
        int ci = w.indexOfId(app.contextId);
        if (ci < 0) {
            ImGui::TextDisabled("This creature is gone");
        } else {
            Creature& c = w.creatures[ci];
            if (w.species.valid(c.speciesId)) {
                swatch(w.species.get(c.speciesId).color);
                ImGui::SameLine();
                ImGui::TextUnformatted(w.species.get(c.speciesId).name().c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::Text("#%u · %s", c.id, activityOf(c));
            ImGui::PopStyleColor();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_CROSSHAIRS "  Select and follow")) {
                app.selectedId = c.id;
                app.follow = true;
            }
            if (ImGui::MenuItem(ICON_FILTER "  Highlight its species")) app.rs->highlightSpecies = c.speciesId;
            if (ImGui::MenuItem(ICON_COPY "  Copy genome")) {
                app.clipboard = c.genome;
                app.clipboardSpecies = c.speciesId;
                app.hasClipboard = true;
            }
            if (ImGui::MenuItem(ICON_FLASK "  Open in Genome Lab")) {
                lab_ = c.genome;
                labReady_ = true;
                showLab_ = true;
            }
            if (ImGui::MenuItem(ICON_PLUS_SQ "  Clone x10 here")) w.spawnGenome(c.genome, c.speciesId, c.pos, 10, 60.f);
            if (ImGui::MenuItem(ICON_BUG "  Infect")) w.infect(c.pos, 1.f);
            if (ImGui::MenuItem(ICON_TIMES "  Remove")) {
                c.alive = false;
                c.cause = DeathCause::Smite;
                w.killRadius(c.pos, 0.f, DeathCause::Smite);
            }
        }
        ImGui::EndPopup();
    }
    if (tourStep_ >= 0) drawTour(app);
    if (!showWelcome) {
        drawLegend(app);
        drawNameTag(app);
    }
    drawHoverTooltip(app);
    drawToasts(app, dt);
    if (app.currentScenario >= 0 && app.currentScenario < scenarioCount() && !showWelcome) {
        // Objective: re-evaluated a few times a second (it walks the statistics history).
        World& gw = *app.world;
        goalTimer_ -= dt;
        if (goalTimer_ <= 0.f || gw.tickCount < goalTick_) {
            goalTimer_ = 0.4f;
            goalTick_ = gw.tickCount;
            goalProgress_ = objective(app.currentScenario).progress(gw);
            goalDone_ = false;
            for (const WorldEvent& e : gw.events.items)
                if (e.kind == EventKind::Milestone && e.text.rfind("Objective complete", 0) == 0) goalDone_ = true;
            if (goalProgress_ >= 1.f && !goalDone_) {
                goalDone_ = true;
                gw.events.push({gw.tickCount, EventKind::Milestone, 0, {},
                                std::string("Objective complete: ") + objective(app.currentScenario).goal});
                app.notify(std::string("Objective complete: ") + scenario(app.currentScenario).name);
                showReport_ = true;
            }
            if (goalDone_) app.challengesDone |= 1 << app.currentScenario;
        }
    }
    if (app.currentScenario >= 0 && app.currentScenario < scenarioCount() && app.showNotes && !showWelcome) {
        const Scenario& sc = scenario(app.currentScenario);
        ImGuiViewport* vp3 = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp3->Pos + ImVec2(viewX + 12.f, viewY + 12.f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(380.f, viewW - 24.f), 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.82f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
        if (ImGui::Begin("##notes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
            ImGui::Text("%s  %s", sc.icon, sc.name);
            ImGui::PopStyleColor();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
            if (ImGui::SmallButton(ICON_TIMES "##closenotes")) app.showNotes = false;
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("%s", sc.watch);
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::TextWrapped(ICON_BULLSEYE "  Goal: %s", objective(app.currentScenario).goal);
            if (goalDone_) {
                ImGui::TextColored(C_GREEN, "Achieved. Keep going, or pick another scenario.");
            } else if (goalProgress_ < 0.f) {
                ImGui::TextColored(C_RED, "Failed. Rewind a year and try something different.");
            } else {
                char pct[16];
                std::snprintf(pct, sizeof pct, "%.0f%%", 100.f * std::min(goalProgress_, 0.99f));
                ImGui::ProgressBar(clamp01(goalProgress_), ImVec2(-1, 0), pct);
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    // First-minute hint in the free area of the world view.
    hintTime_ += dt;
    if (hintTime_ < 40.f && !showWelcome && !app.selectedId && !(app.currentScenario >= 0 && app.showNotes)) {
        float a = clamp01((40.f - hintTime_) / 4.f) * clamp01(hintTime_ / 1.5f);
        ImGuiViewport* vp2 = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp2->Pos + ImVec2(viewX + viewW * 0.5f, viewY + 18.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
        ImGui::SetNextWindowBgAlpha(0.7f * a);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f);
        ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking);
        ImGui::TextUnformatted(ICON_POINTER " Click a creature   " ICON_SEARCH " Scroll to zoom   " ICON_MOVE " Drag to pan   "
                               ICON_PAUSE " Space pauses   K: cinematic tour");
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void Ui::drawMenuBar(AppState& app) {
    World& w = *app.world;
    RenderSettings& rs = *app.rs;
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
    ImGui::TextUnformatted(ICON_LEAF " PRIMORDIA");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(6, 0));
    if (ImGui::BeginMenu("World")) {
        if (ImGui::MenuItem(ICON_NEW "  New world...", "Ctrl+N")) openNewWorld = true;
        if (ImGui::BeginMenu(ICON_FLASK "  Scenarios")) {
            int met = 0;
            for (int i = 0; i < scenarioCount(); ++i) met += (app.challengesDone >> i) & 1;
            ImGui::TextDisabled(ICON_TROPHY "  Challenges: %d of %d goals met", met, scenarioCount());
            ImGui::Separator();
            for (int i = 0; i < scenarioCount(); ++i) {
                const Scenario& sc = scenario(i);
                char label[128];
                std::snprintf(label, sizeof label, "%s  %s%s", sc.icon, sc.name, (app.challengesDone >> i) & 1 ? "  " ICON_CHECK : "");
                if (ImGui::MenuItem(label, sc.tagline)) {
                    app.newParams = SimParams{};
                    sc.setup(app.newWorld, app.newParams);
                    app.newWorld.seed = std::random_device{}() % 1000000;
                    app.newScenario = i;
                    app.requestNewWorld = true;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_STAR "  Welcome screen")) showWelcome = true;
        if (app.currentScenario >= 0 && ImGui::MenuItem(ICON_INFO "  Scenario notes", nullptr, app.showNotes))
            app.showNotes = !app.showNotes;
        if (ImGui::MenuItem(ICON_OPEN "  Open...", "Ctrl+O")) app.requestLoad = "?";
        if (ImGui::MenuItem(ICON_SAVE "  Save", "Ctrl+S"))
            app.requestSave = app.lastSavePath.empty() ? "?" : app.lastSavePath;
        if (ImGui::MenuItem(ICON_SAVE "  Save as...")) app.requestSave = "?";
        if (ImGui::MenuItem(ICON_SAVE "  Quick save to Documents/Primordia")) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "quicksave-%lld.prim", (long long)w.tickCount);
            app.requestSave = app.saveDir + buf;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_CAMERA "  Screenshot", "F12")) app.requestScreenshot = true;
        if (ImGui::MenuItem(ICON_BARCHART "  Export statistics (CSV)")) app.requestCsv = true;
        if (ImGui::MenuItem(ICON_LIST "  Export species (CSV)")) app.requestSpeciesCsv = true;
        if (ImGui::MenuItem(ICON_SITEMAP "  Export tree of life (Newick)")) app.requestNewick = true;
        if (ImGui::MenuItem(app.recording ? ICON_PAUSE "  Stop time-lapse" : ICON_CAMERA "  Record time-lapse", "F11"))
            app.toggleRecording = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) app.requestQuit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Simulation")) {
        if (ImGui::MenuItem(app.paused ? ICON_PLAY "  Resume" : ICON_PAUSE "  Pause", "Space")) app.paused = !app.paused;
        if (ImGui::MenuItem(ICON_STEP "  Step one tick", ".")) {
            app.paused = true;
            app.stepOnce = true;
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_HISTORY "  Rewind to", !app.checkpoints.empty())) {
            double now = ImGui::GetTime();
            (void)now;
            for (int i = int(app.checkpoints.size()) - 1; i >= 0; --i) {
                const auto& cp = app.checkpoints[size_t(i)];
                char label[128];
                std::snprintf(label, sizeof label, "Year %.1f  ·  %zu creatures, %d species", double(cp.tick) / w.params.yearLength,
                              cp.creatures, cp.species);
                if (ImGui::MenuItem(label)) app.requestRewind = i;
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (int i = 0; i < SPEED_COUNT; ++i) {
            char key[8];
            std::snprintf(key, sizeof key, "%d", (i + 1) % 10);
            if (ImGui::MenuItem(SPEED_LABELS[i], key, app.speed == i)) {
                app.speed = i;
                app.paused = false;
            }
        }
        if (ImGui::BeginMenu("Evolution pace")) {
            static const char* names[] = {"Normal", "Fast (2x)", "Faster (3x)", "Frantic (5x)"};
            static const float pace[] = {1.f, 2.f, 3.f, 5.f};
            SimParams& sp = app.world->params;
            for (int k = 0; k < 4; ++k)
                if (ImGui::MenuItem(names[k], nullptr, sp.lifePace == pace[k] && sp.mutationRate == pace[k])) sp.lifePace = sp.mutationRate = pace[k];
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Events")) {
        if (ImGui::MenuItem(ICON_FIRE "  Meteor strike")) {
            Rng r(uint64_t(w.tickCount) * 7919 + 1);
            Vec2 at{r.range(0.2f, 0.8f) * w.terrain.width, r.range(0.2f, 0.8f) * w.terrain.height};
            w.meteor(at, 320.f);
            app.cam->targetCenter = at;
        }
        if (ImGui::MenuItem(ICON_SNOW "  Ice age")) w.climateShock(-0.75f, "An ice age begins");
        if (ImGui::MenuItem(ICON_SUN "  Heat wave")) w.climateShock(0.6f, "A heat wave scorches the world");
        if (ImGui::MenuItem(ICON_BUG "  Epidemic (largest species)")) w.plague(0, 0.15f);
        if (ImGui::MenuItem(ICON_LEAF "  Great bloom")) w.bloom();
        if (ImGui::MenuItem(ICON_TINT "  Drought")) w.drought();
        if (ImGui::MenuItem(ICON_FIRE "  Volcanic eruption")) {
            Rng r(uint64_t(w.tickCount) * 131 + 3);
            Vec2 at{r.range(0.15f, 0.85f) * w.terrain.width, r.range(0.15f, 0.85f) * w.terrain.height};
            w.volcano(at, 260.f);
            app.cam->targetCenter = at;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Tools", nullptr, &showTools_);
        ImGui::MenuItem("Environment", nullptr, &showEnvironment_);
        ImGui::MenuItem("Minimap", nullptr, &showMinimap_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::MenuItem("Brain", nullptr, &showBrain_);
        ImGui::MenuItem("Species", nullptr, &showSpecies_);
        ImGui::MenuItem("Tree of Life", nullptr, &showPhylogeny_);
        ImGui::MenuItem("Population", nullptr, &showPopulation_);
        ImGui::MenuItem("Traits", nullptr, &showTraits_);
        ImGui::MenuItem("Events", nullptr, &showEvents_);
        ImGui::MenuItem("Performance", nullptr, &showPerformance_);
        ImGui::MenuItem("Genome Lab", nullptr, &showLab_);
        ImGui::MenuItem("World report", nullptr, &showReport_);
        ImGui::MenuItem("Primer Lab (speed, size, sense)", nullptr, &showPrimer_);
        ImGui::Separator();
        if (ImGui::BeginMenu("Colour creatures by")) {
            for (int i = 0; i < int(ColorMode::Count); ++i)
                if (ImGui::MenuItem(colorModeName(ColorMode(i)), nullptr, int(rs.colorMode) == i))
                    rs.colorMode = ColorMode(i);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Map overlay")) {
            for (int i = 0; i < int(Overlay::Count); ++i)
                if (ImGui::MenuItem(overlayName(Overlay(i)), nullptr, int(rs.overlay) == i)) rs.overlay = Overlay(i);
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Vision cone", "V", &rs.showVision);
        ImGui::MenuItem("Trail", nullptr, &rs.showTrails);
        ImGui::MenuItem("Bloom", "G", &rs.bloom);
        ImGui::MenuItem("Particles", nullptr, &rs.particles);
        ImGui::MenuItem("Signal glow", nullptr, &rs.showSignals);
        ImGui::MenuItem("Pheromone trails", nullptr, &rs.showPheromones);
        ImGui::MenuItem("Carcasses", nullptr, &rs.showCorpses);
        ImGui::MenuItem("Vignette", nullptr, &rs.vignette);
        ImGui::MenuItem("Cloud shadows", nullptr, &rs.clouds);
        ImGui::MenuItem("Sound", "M", &app.sound);
        ImGui::MenuItem("Blob style (Primer)", "B", &rs.blobStyle);
        ImGui::Separator();
        if (ImGui::MenuItem("Hide interface", "Tab")) app.showUi = false;
        if (ImGui::MenuItem("Cinematic tour", "K")) {
            app.cinematic = true;
            app.showUi = false;
            app.cineTimer = 0.f;
        }
        if (ImGui::MenuItem("Reset layout")) app.requestResetLayout = true;
        if (ImGui::MenuItem(ICON_COG "  Preferences...")) showPrefs_ = true;
        if (ImGui::MenuItem("Fullscreen", "F10", app.fullscreen)) {
            app.fullscreen = !app.fullscreen;
            app.applyDisplay = true;
        }
        if (ImGui::MenuItem("Fit world in view", "H")) app.cam->fit(w.terrain.width, w.terrain.height);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(ICON_STAR "  Guided tour")) tourStep_ = 0;
        ImGui::MenuItem(ICON_KEYBOARD "  Controls", nullptr, &showHelp_);
        ImGui::MenuItem(ICON_INFO "  How creatures work", nullptr, &showEncyclopedia_);
        ImGui::MenuItem("ImGui / ImPlot demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Ui::drawToolbar(AppState& app) {
    World& w = *app.world;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + 14.f;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_MenuBar * 0 | ImGuiWindowFlags_NoDecoration;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 7));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.09f, 1.f));
    if (ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, h, flags)) {
        if (iconButton(app.paused ? ICON_PLAY : ICON_PAUSE, app.paused ? "Resume (Space)" : "Pause (Space)",
                       !app.paused, ImVec2(ImGui::GetFrameHeight() * 1.3f, 0)))
            app.paused = !app.paused;
        ImGui::SameLine();
        if (iconButton(ICON_STEP, "Step one tick (.)")) {
            app.paused = true;
            app.stepOnce = true;
        }
        ImGui::SameLine();
        if (iconButton(ICON_HISTORY, "Rewind one year (keeps the last 8 yearly checkpoints)") && !app.checkpoints.empty()) {
            int target = int(app.checkpoints.size()) - 1;
            // Skip the checkpoint we are sitting on, if it was taken moments ago.
            if (target > 0 && w.tickCount - app.checkpoints[size_t(target)].tick < w.params.yearLength / 10) --target;
            app.requestRewind = target;
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(6, 0));
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
        for (int i = 0; i < SPEED_COUNT; ++i) {
            bool active = app.speed == i && !app.paused;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_DIM);
            if (ImGui::Button(SPEED_LABELS[i])) {
                app.speed = i;
                app.paused = false;
            }
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip(i == SPEED_COUNT - 1   ? "Turbo: nearly all CPU goes to the simulation; the screen refreshes a few times a second"
                                  : i == SPEED_COUNT - 2 ? "As fast as the CPU allows while staying smooth (key 0)"
                                                         : "Ticks per frame (key %d)",
                                  i + 1);
            ImGui::SameLine();
        }
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();

        auto chip = [&](const char* icon, const char* label, const char* value, ImVec4 col) {
            // Drop readouts that no longer fit rather than spilling off-screen on narrow windows.
            const float need = ImGui::CalcTextSize(icon).x + ImGui::CalcTextSize(label).x + ImGui::CalcTextSize(value).x + 40.f;
            if (ImGui::GetContentRegionAvail().x < need) return;
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::Text("%s %s", icon, label);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 4);
            ImGui::PushFont(fontMono_, 0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(value);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine(0, 16);
        };
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.2f", w.year());
        chip(ICON_CLOCK, "Year", buf, C_TEXT);
        std::snprintf(buf, sizeof buf, "%s / %s", w.seasonName(true), w.seasonName(false));
        float clim = w.climate();
        chip(clim < -0.2f ? ICON_SNOW : (clim > 0.2f ? ICON_SUN : ICON_TREE), "N/S", buf,
             clim < -0.2f ? ImVec4(0.6f, 0.8f, 1.f, 1) : C_TEXT);
        {
            float L = w.daylight();
            const char* tod = !w.params.dayNight ? "No night" : L > 0.85f ? "Day" : L < 0.15f ? "Night" : "Twilight";
            chip(L > 0.5f ? ICON_SUN : ICON_EYE, "", tod, L > 0.5f ? C_AMBER : ImVec4(0.6f, 0.7f, 1.f, 1));
        }
        std::snprintf(buf, sizeof buf, "%zu", w.creatures.size());
        chip(ICON_PAW, "Pop", buf, C_ACCENT);
        std::snprintf(buf, sizeof buf, "%d", w.species.aliveCount());
        chip(ICON_SITEMAP, "Species", buf, C_ACCENT);
        std::snprintf(buf, sizeof buf, "%u", w.maxGeneration);
        chip(ICON_HISTORY, "Gen", buf, C_TEXT);
        std::snprintf(buf, sizeof buf, "%.0f", app.tps);
        chip(ICON_GAUGE, "Ticks/s", buf, C_AMBER);
        std::snprintf(buf, sizeof buf, "%.0f", app.fps);
        chip("", "FPS", buf, C_MUTED);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void Ui::drawStatusBar(AppState& app) {
    World& w = *app.world;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.043f, 0.059f, 0.067f, 1.f));
    if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, ImGui::GetTextLineHeight() + 10.f,
                                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings)) {
        ImGuiIO& io = ImGui::GetIO();
        Vec2 mouse{io.MousePos.x * io.DisplayFramebufferScale.x, io.MousePos.y * io.DisplayFramebufferScale.y};
        Vec2 wp = app.cam->screenToWorld(mouse);
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        if (w.terrain.inside(wp.x, wp.y)) {
            int idx = w.terrain.index(wp.x, wp.y);
            const char* biome = w.terrain.water[idx] ? "Water"
                                : w.terrain.fertility[idx] > 0.6f ? "Lush land"
                                : w.terrain.fertility[idx] > 0.25f ? "Grassland"
                                                                   : "Barren land";
            ImGui::Text("%s  (%.0f, %.0f)   %s   %.1f°C   plants %.0f%%", ICON_CROSSHAIRS, wp.x, wp.y, biome,
                        toCelsius(w.temperatureAtCell(idx)),
                        100.f * w.terrain.plant[idx] / std::max(0.01f, w.params.plantCapacity));
        } else {
            ImGui::Text("%s  outside the world", ICON_CROSSHAIRS);
        }
        ImGui::SameLine(0, 30);
        ImGui::Text("Tool: %s", toolName(app.tool));
        ImGui::SameLine(0, 30);
        ImGui::Text("Colour: %s", colorModeName(app.rs->colorMode));
        ImGui::SameLine(0, 30);
        ImGui::Text("Seed %llu", (unsigned long long)w.settings.seed);
        ImGui::SameLine(0, 30);
        ImGui::Text("Births %llu  Deaths %llu", (unsigned long long)w.stats.totalBirths,
                    (unsigned long long)w.stats.totalDeaths);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------
void Ui::drawInspector(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_SEARCH " Inspector", &showInspector_)) {
        ImGui::End();
        return;
    }
    int idx = w.indexOfId(app.selectedId);
    if (idx < 0) {
        if (app.selectedId && w.watchDeathTick >= 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
            ImGui::TextWrapped("%s Creature #%u died (%s) at year %.2f.", ICON_TIMES, app.selectedId,
                               deathCauseName(w.watchCause), double(w.watchDeathTick) / w.params.yearLength);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::TextWrapped("Click a creature in the world to inspect its body, genome and brain.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        auto pickBy = [&](auto score) {
            int best = -1;
            float bestS = -1e30f;
            for (int i = 0; i < int(w.creatures.size()); ++i) {
                float s = score(w.creatures[i]);
                if (s > bestS) { bestS = s; best = i; }
            }
            if (best >= 0) {
                app.selectedId = w.creatures[best].id;
                app.follow = true;
            }
        };
        if (ImGui::Button(ICON_RANDOM " Random")) {
            static std::mt19937 gen{42};
            if (!w.creatures.empty()) {
                app.selectedId = w.creatures[gen() % w.creatures.size()].id;
                app.follow = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_CLOCK " Oldest")) pickBy([](const Creature& c) { return c.age; });
        ImGui::SameLine();
        if (ImGui::Button(ICON_HISTORY " Latest generation")) pickBy([](const Creature& c) { return float(c.generation); });
        if (ImGui::Button(ICON_PAW " Top predator")) pickBy([](const Creature& c) { return c.ph.diet * 10.f + float(c.kills); });
        ImGui::SameLine();
        if (ImGui::Button(ICON_USERS " Most children")) pickBy([](const Creature& c) { return float(c.children); });
        ImGui::SameLine();
        if (ImGui::Button(ICON_EXPAND " Biggest")) pickBy([](const Creature& c) { return c.radius; });

        // World at a glance: something to read (and click) before any creature is selected.
        ImGui::SeparatorText("World at a glance");
        const Species* top = nullptr;
        int hunters = 0;
        for (const Species& s : w.species.all())
            if (s.alive()) {
                if (!top || s.population > top->population) top = &s;
                hunters += s.meanGenes[G_DIET] > 0.6f;
            }
        ImGui::Text("Year %.1f  ·  %zu creatures  ·  %d species (%d hunting)", w.year(), w.creatures.size(),
                    w.species.aliveCount(), hunters);
        if (top) {
            ImGui::TextColored(C_MUTED, "Dominant");
            ImGui::SameLine();
            swatch(top->color);
            ImGui::SameLine();
            if (ImGui::SmallButton(top->name().c_str())) {
                app.rs->highlightSpecies = top->id;
                app.cam->targetCenter = top->centroid;
                app.follow = false;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("%d%% of all creatures. Best at: %s",
                               int(100 * top->population / int(std::max<size_t>(1, w.creatures.size()))),
                               w.bestAtText(top->id).c_str());
            ImGui::PopStyleColor();
        }
        if (app.currentScenario >= 0 && app.currentScenario < scenarioCount()) {
            ImGui::Spacing();
            ImGui::TextWrapped(ICON_BULLSEYE "  Goal: %s", objective(app.currentScenario).goal);
            if (goalDone_) ImGui::TextColored(C_GREEN, "Achieved");
            else if (goalProgress_ < 0.f) ImGui::TextColored(C_RED, "Failed. Rewind a year and try something different.");
            else ImGui::ProgressBar(clamp01(goalProgress_), ImVec2(-1, 0));
        }
        const WorldEvent* lastDiscovery = nullptr;
        const WorldEvent* lastNews = nullptr;
        for (auto it = w.events.items.rbegin(); it != w.events.items.rend() && !(lastDiscovery && lastNews); ++it) {
            if (!lastDiscovery && it->kind == EventKind::Discovery) lastDiscovery = &*it;
            if (!lastNews && (it->kind == EventKind::Extinction || it->kind == EventKind::Catastrophe ||
                              (it->kind == EventKind::Speciation && it->tick > 0)))
                lastNews = &*it;
        }
        auto eventLine = [&](const char* heading, const WorldEvent* e, const char* none) {
            ImGui::Spacing();
            ImGui::TextColored(C_MUTED, "%s", heading);
            if (!e) {
                ImGui::TextWrapped("%s", none);
                return;
            }
            ImGui::PushID(e);
            ImGui::PushStyleColor(ImGuiCol_Text, eventColor(e->kind));
            ImGui::TextWrapped("Year %.2f: %s", double(e->tick) / w.params.yearLength, e->text.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) {
                if (e->pos.x != 0.f || e->pos.y != 0.f) app.cam->targetCenter = e->pos;
                if (e->speciesId && w.species.valid(e->speciesId)) app.rs->highlightSpecies = e->speciesId;
                app.follow = false;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        };
        eventLine("Latest discovery", lastDiscovery, "Nothing yet. Behaviours appear here once they have held for a while.");
        eventLine("Latest news", lastNews, "No species has appeared or vanished yet.");
        ImGui::End();
        return;
    }
    Creature& c = w.creatures[idx];
    const Species* sp = w.species.valid(c.speciesId) ? &w.species.get(c.speciesId) : nullptr;
    const Phenotype& p = c.ph;

    {
        unsigned tex = app.renderer->portrait(0, w, c, *app.rs, time_);
        ImGui::Image(ImTextureRef(ImTextureID(intptr_t(tex))), ImVec2(84, 84), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine();
        ImGui::BeginGroup();
    }
    ImGui::PushFont(fontTitle_, fontTitle_->LegacySize);
    if (sp) {
        swatch(sp->color, 14.f);
        ImGui::SameLine();
        ImGui::TextUnformatted(sp->name().c_str());
    } else {
        ImGui::TextUnformatted("Unknown species");
    }
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%s  ·  #%u  ·  generation %u", personalName(c.id).c_str(), c.id, c.generation);
    ImGui::Text("%s  ·  %.1f°C preferred  ·  %d synapses", dietLabel(p.diet), toCelsius(p.thermalOpt), p.connections);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
    ImGui::TextUnformatted(activityOf(c));
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    if (sp && sp->parentId) {
        // Ancestry: the chain of species this creature's lineage passed through.
        std::vector<uint32_t> chain;
        for (uint32_t id = sp->id; id && chain.size() < 12; id = w.species.get(id).parentId) chain.push_back(id);
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::TextUnformatted(ICON_SITEMAP " Ancestry:");
        ImGui::PopStyleColor();
        for (int k = int(chain.size()) - 1; k >= 0; --k) {
            const Species& a = w.species.get(chain[size_t(k)]);
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Text, a.alive() ? v4(a.color) : C_MUTED);
            ImGui::PushID(int(a.id));
            if (ImGui::SmallButton(a.name().c_str())) app.rs->highlightSpecies = a.id;
            ImGui::PopID();
            ImGui::PopStyleColor();
            if (k > 0) {
                ImGui::SameLine(0, 4);
                ImGui::TextDisabled(">");
            }
            if (ImGui::GetContentRegionAvail().x < 120.f && k > 0) ImGui::NewLine();
        }
    }

    if (iconButton(ICON_CROSSHAIRS " Follow", "Keep the camera on this creature (F)", app.follow)) app.follow = !app.follow;
    ImGui::SameLine();
    if (iconButton(ICON_LINK " Dynasty", "When this creature dies, follow one of its children (or a relative)", app.dynasty))
        app.dynasty = !app.dynasty;
    ImGui::SameLine();
    if (iconButton(ICON_COPY " Copy genome", "Copy genome (Ctrl+C), then plant clones with the Clone tool")) {
        app.clipboard = c.genome;
        app.clipboardSpecies = c.speciesId;
        app.hasClipboard = true;
        app.tool = Tool::Clone;
        app.notify("Genome copied. Click the map to plant clones.");
    }
    ImGui::SameLine();
    if (iconButton(ICON_FILTER " Species", "Highlight this species in the world",
                   app.rs->highlightSpecies == c.speciesId))
        app.rs->highlightSpecies = app.rs->highlightSpecies == c.speciesId ? 0 : c.speciesId;
    ImGui::SameLine();
    if (iconButton(ICON_UP " Parent", "Select the parent if it is still alive")) {
        if (w.indexOfId(c.parentId) >= 0) app.selectedId = c.parentId;
        else app.notify("The parent is no longer alive");
    }
    ImGui::SameLine();
    if (iconButton(ICON_FLASK, "Open this genome in the Genome Lab")) {
        lab_ = c.genome;
        labReady_ = true;
        showLab_ = true;
    }
    ImGui::SameLine();
    if (iconButton(ICON_TIMES, "Remove this creature (Delete)")) {
        c.alive = false;
        c.cause = DeathCause::Smite;
        w.killRadius(c.pos, 0.f, DeathCause::Smite);
        ImGui::End();
        return;
    }

    ImGui::Spacing();
    char buf[96];
    std::snprintf(buf, sizeof buf, "Energy  %.0f / %.0f", c.energy, c.maxEnergy);
    bar(c.energy / c.maxEnergy, ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.25f, 0.85f)), buf);
    std::snprintf(buf, sizeof buf, "Health  %.0f / %.0f", c.health, c.maxHealth);
    bar(c.health / c.maxHealth, ImGui::GetColorU32(ImVec4(0.85f, 0.3f, 0.32f, 0.85f)), buf);
    if (w.params.thirst) {
        std::snprintf(buf, sizeof buf, "Water  %.0f%%", c.hydration * 100.f);
        bar(c.hydration, ImGui::GetColorU32(ImVec4(0.3f, 0.6f, 0.95f, 0.85f)), buf);
    }
    std::snprintf(buf, sizeof buf, "Age  %.2f of %.2f years", c.age / w.params.yearLength,
                  p.lifespan / w.params.yearLength);
    bar(c.age / p.lifespan, ImGui::GetColorU32(ImVec4(0.45f, 0.55f, 0.85f, 0.85f)), buf);
    if (c.growth < 1.f) {
        std::snprintf(buf, sizeof buf, "Growing  %.0f%%", c.growth * 100.f);
        bar(c.growth, ImGui::GetColorU32(ImVec4(0.4f, 0.8f, 0.5f, 0.85f)), buf);
    }
    // Rolling history of energy and health for the selected creature.
    if (historyId_ != c.id) {
        historyId_ = c.id;
        energyHist_.clear();
        healthHist_.clear();
    }
    if (ImGui::GetFrameCount() % 3 == 0) {
        energyHist_.push_back(c.energy / c.maxEnergy);
        healthHist_.push_back(c.health / c.maxHealth);
        if (energyHist_.size() > 240) {
            energyHist_.erase(energyHist_.begin());
            healthHist_.erase(healthHist_.begin());
        }
    }
    if (energyHist_.size() > 2 && ImPlot::BeginPlot("##vitals", ImVec2(-1, 70),
                                                    ImPlotFlags_CanvasOnly | ImPlotFlags_NoFrame)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_AutoFit,
                          ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1.05, ImPlotCond_Always);
        ImPlotSpec es;
        es.LineColor = ImVec4(0.95f, 0.75f, 0.25f, 1.f);
        es.FillColor = ImVec4(0.95f, 0.75f, 0.25f, 1.f);
        es.FillAlpha = 0.15f;
        es.Flags = ImPlotLineFlags_Shaded;
        ImPlot::PlotLine("energy", energyHist_.data(), int(energyHist_.size()), 1.0, 0.0, es);
        ImPlotSpec hs;
        hs.LineColor = ImVec4(0.9f, 0.35f, 0.35f, 1.f);
        ImPlot::PlotLine("health", healthHist_.data(), int(healthHist_.size()), 1.0, 0.0, hs);
        ImPlot::EndPlot();
    }

    // Current activity chips
    ImGui::Spacing();
    auto activity = [&](bool on, const char* label, ImVec4 col) {
        if (!on) return;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x, col.y, col.z, 0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::SmallButton(label);
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };
    activity(c.flags & CF_EATING, ICON_LEAF " eating", C_GREEN);
    activity(c.flags & CF_ATTACKING, ICON_BOLT " attacking", C_RED);
    activity(c.flags & CF_SHARING, ICON_GIFT " sharing", C_AMBER);
    activity(c.flags & CF_IN_WATER, ICON_TINT " swimming", ImVec4(0.4f, 0.7f, 1.f, 1));
    activity(c.flags & CF_POISONED, ICON_WARNING " poisoned", ImVec4(0.7f, 1.f, 0.3f, 1));
    activity(c.pain > 0.2f, ICON_HEART " in pain", C_RED);
    activity(std::fabs(c.signal) > 0.3f, ICON_COMMENT " signalling", ImVec4(0.5f, 0.9f, 1.f, 1));
    activity(c.pheromone > 0.1f, ICON_SHARE " marking trail", ImVec4(0.85f, 0.45f, 1.f, 1));
    activity(c.camo > 0.3f, ICON_EYE " camouflaged", ImVec4(0.45f, 0.75f, 0.35f, 1));
    activity(c.infection > 0.f, ICON_BUG " infected", ImVec4(0.65f, 0.95f, 0.35f, 1));
    activity(c.infection <= 0.f && c.immuneTicks > 0.f, ICON_CHECK " immune", C_AMBER);
    activity(c.out[OUT_REPRODUCE] > 0.f && c.growth >= 1.f, ICON_HEART " wants to breed", ImVec4(1.f, 0.6f, 0.75f, 1));
    ImGui::NewLine();

    if (ImGui::BeginTable("##stats", 4, ImGuiTableFlags_SizingStretchProp)) {
        auto row = [&](const char* k, const char* fmt, auto v) {
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextUnformatted(k);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::Text(fmt, v);
        };
        row("Children", "%u", c.children);
        row("Kills", "%u", c.kills);
        row("Radius", "%.1f", c.radius);
        row("Speed", "%.2f", c.vel.length());
        row("Vision", "%.0f", p.visionRange);
        row("Field of view", "%.0f°", p.fov * 180.f / PI);
        row("Plant digestion", "%.0f%%", p.plantEff * 100.f);
        row("Meat digestion", "%.0f%%", p.meatEff * 100.f);
        row("Armor", "%.2f", p.armor);
        row("Weapon", "%.2f", p.weapon);
        row("Toxin", "%.2f", p.toxin);
        row("Litter", "%d", p.litter);
        row("Prefers", "%.1f°C", toCelsius(p.thermalOpt));
        row("Feels", "%.1f°C", toCelsius(c.temperature));
        row("Mutation rate", "%.3f", p.mutRate);
        row("Synapses", "%d", p.connections);
        row("Ate plants", "%.0f", c.eatenPlant);
        row("Ate meat", "%.0f", c.eatenMeat);
        ImGui::EndTable();
    }

    {
        // Family: living children and siblings.
        std::vector<uint32_t> kids, sibs;
        for (const Creature& o : w.creatures) {
            if (o.parentId == c.id) kids.push_back(o.id);
            else if (c.parentId && o.parentId == c.parentId && o.id != c.id) sibs.push_back(o.id);
        }
        ImGui::Spacing();
        sectionLabel("FAMILY");
        ImGui::Text("%zu living children, %zu living siblings%s", kids.size(), sibs.size(),
                    w.indexOfId(c.parentId) >= 0 ? ", parent alive" : "");
        int shown = 0;
        for (uint32_t id : kids) {
            if (shown++ >= 8) break;
            if (shown > 1) ImGui::SameLine();
            ImGui::PushID(int(id));
            if (ImGui::SmallButton(personalName(id).c_str())) app.selectedId = id;
            ImGui::PopID();
        }
    }
    for (int k = 0, shown = 0; k < N_ORGANS; ++k) {
        const Organ& o = c.ph.organ[k];
        if (!o.on) continue;
        if (!shown++) {
            ImGui::Spacing();
            sectionLabel("ORGANS (not designed: evolved from parts)");
        }
        ImGui::TextWrapped("Power %.2f, in use %.0f%% of the time: %s", o.power, 100.f * std::min(1.f, c.organUse[k]), organText(o).c_str());
    }
    if (c.parentId) {
        // What changed at birth: the biggest differences from the parent's genome.
        ImGui::Spacing();
        sectionLabel(c.sexual ? "AT BIRTH (mutation + mate's genes)" : "AT BIRTH (mutations)");
        int order[G_COUNT];
        for (int k = 0; k < G_COUNT; ++k) order[k] = k;
        std::sort(order, order + G_COUNT, [&](int a, int b) { return std::fabs(c.geneDelta[a]) > std::fabs(c.geneDelta[b]); });
        int shown = 0;
        for (int k = 0; k < G_COUNT && shown < 4; ++k) {
            float d = c.geneDelta[order[k]];
            if (std::fabs(d) < 0.005f) break;
            if (shown++) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, d > 0 ? C_GREEN : C_RED);
            ImGui::Text("%s %+.2f", GENE_INFO[order[k]].name, d);
            ImGui::PopStyleColor();
        }
        if (!shown) ImGui::TextDisabled("no body genes changed");
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("%u of %d brain weights differ from the parent", unsigned(c.synapsesChanged), BRAIN_SIZE);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    sectionLabel("GENOME");
    for (int g = 0; g < G_COUNT; ++g) {
        float v = c.genome.g[g];
        float mean = w.stats.samples.empty() ? 0.5f : w.stats.samples.back().geneMean[g];
        ImGui::PushID(g);
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::TextUnformatted(GENE_INFO[g].name);
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetFontSize() * 7.5f);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float bw = ImGui::GetContentRegionAvail().x, bh = ImGui::GetTextLineHeight() * 0.7f;
        float y = pos.y + (ImGui::GetTextLineHeight() - bh) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + bw, y + bh), ImGui::GetColorU32(C_FRAME), 3.f);
        ImU32 col = g == G_HUE ? u32(hsv(v, 0.6f, 0.95f)) : ImGui::GetColorU32(C_ACCENT_DIM);
        dl->AddRectFilled(ImVec2(pos.x, y), ImVec2(pos.x + std::max(3.f, bw * v), y + bh), col, 3.f);
        dl->AddLine(ImVec2(pos.x + bw * mean, y - 2), ImVec2(pos.x + bw * mean, y + bh + 2),
                    ImGui::GetColorU32(C_AMBER), 2.f);
        ImGui::Dummy(ImVec2(bw, ImGui::GetTextLineHeight()));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %.3f (population mean %.3f)\n%s", GENE_INFO[g].name, v, mean,
                              GENE_INFO[g].description);
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("Amber ticks mark the population average.");
    ImGui::PopStyleColor();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Brain
// ---------------------------------------------------------------------------
void Ui::drawBrain(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_BOLT " Brain", &showBrain_)) {
        ImGui::End();
        return;
    }
    int idx = w.indexOfId(app.selectedId);
    if (idx < 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::TextWrapped("Select a creature to watch its neural network think in real time.");
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    const Creature& c = w.creatures[idx];
    const float* wt = c.genome.w.data();
    ImGui::Checkbox("Show weak synapses", &brainShowAll_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%d synapses · %d inputs · %d hidden (recurrent) · %d outputs", c.ph.connections, int(N_INPUTS),
                N_HIDDEN, int(N_OUTPUTS));
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.62f, 0.25f, 1.f));
    ImGui::TextUnformatted(ICON_CUBE " excitatory / positive");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.62f, 1.f, 1.f));
    ImGui::TextUnformatted(ICON_CUBE " inhibitory / negative");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("· line width = strength · hover a neuron to trace it");

    {
        // What the creature sees: its five eye sectors, each split into rings per channel.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float R = 74.f;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float width = ImGui::GetContentRegionAvail().x;
        ImVec2 ctr = p0 + ImVec2(R + 12.f, R + 8.f);
        const float fov = std::min(c.ph.fov, TAU * 0.999f), half = fov * 0.5f, ew = fov / NUM_EYES;
        struct Ch { int ch; ImVec4 col; const char* name; } chans[] = {
            {EC_PLANT, ImVec4(0.4f, 0.95f, 0.4f, 1), "plants"}, {EC_MEAT, ImVec4(1.f, 0.42f, 0.35f, 1), "carcass"},
            {EC_CREATURE, ImVec4(1.f, 0.85f, 0.3f, 1), "creature"}, {EC_OBSTACLE, ImVec4(0.4f, 0.65f, 1.f, 1), "water/wall"}};
        dl->AddCircleFilled(ctr, R, ImGui::GetColorU32(ImVec4(0.035f, 0.05f, 0.058f, 1)), 48);
        for (int e = 0; e < NUM_EYES; ++e) {
            // Facing up: heading = -90 degrees.
            float a0 = -PI * 0.5f - half + ew * float(e), a1 = a0 + ew;
            for (int k = 0; k < 4; ++k) {
                float v = clamp01(c.in[e * EC_COUNT + chans[k].ch]);
                float r0 = R * (0.25f + 0.1875f * float(k)), r1 = r0 + R * 0.1875f * 0.92f;
                ImVec4 col = chans[k].col;
                col.w = 0.08f + 0.85f * v;
                dl->PathArcTo(ctr, r1, a0 + 0.02f, a1 - 0.02f, 10);
                dl->PathArcTo(ctr, r0, a1 - 0.02f, a0 + 0.02f, 10);
                dl->PathFillConvex(ImGui::GetColorU32(col));
            }
        }
        dl->AddCircleFilled(ctr, R * 0.2f, u32(creatureColorForUi(w, c)), 24);
        dl->AddTriangleFilled(ctr + ImVec2(0, -R * 0.28f), ctr + ImVec2(-5, -R * 0.17f), ctr + ImVec2(5, -R * 0.17f),
                              IM_COL32(255, 255, 255, 200));
        ImGui::Dummy(ImVec2(2 * R + 24.f, 2 * R + 16.f));
        ImGui::SameLine();
        ImGui::BeginGroup();
        sectionLabel("WHAT IT SEES (inner to outer ring)");
        for (auto& ch : chans) {
            ImGui::PushStyleColor(ImGuiCol_Text, ch.col);
            ImGui::TextUnformatted(ICON_CUBE);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(ch.name);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("Hears %+.2f · smells %.2f · crowd %.2f", c.in[IN_SIGNAL], c.in[IN_SMELL], c.in[IN_CROWD]);
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        (void)width;
    }
    const float rowH = std::max(13.f, ImGui::GetTextLineHeight() * 0.86f);
    const float canvasH = std::max(ImGui::GetContentRegionAvail().y, rowH * (N_INPUTS + 2));
    ImGui::BeginChild("##brainCanvas", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = std::max(420.f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##brain", ImVec2(width, canvasH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fontMono_, fontMono_->LegacySize * 0.85f);
    const float labelW = ImGui::CalcTextSize("Eye5 Obstacle").x + 10.f;
    const float xIn = origin.x + labelW + 8.f;
    const float xOut = origin.x + width - ImGui::CalcTextSize("Reproduce +0.00").x - 18.f;
    const float xHid = (xIn + xOut) * 0.5f;
    auto yOf = [&](int i, int n) { return origin.y + (canvasH - 8.f) * (float(i) + 0.5f) / float(n) + 4.f; };
    auto actCol = [](float v, float alpha) {
        v = clampf(v, -1.f, 1.f);
        ImVec4 pos{1.f, 0.62f, 0.25f, alpha}, neg{0.35f, 0.62f, 1.f, alpha}, mid{0.25f, 0.3f, 0.33f, alpha};
        ImVec4 t = v >= 0 ? pos : neg;
        float a = std::fabs(v);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(mid.x + (t.x - mid.x) * a, mid.y + (t.y - mid.y) * a,
                                                     mid.z + (t.z - mid.z) * a, alpha));
    };
    const float thresh = brainShowAll_ ? 0.f : 0.35f;
    // Hover detection
    ImVec2 mp = ImGui::GetIO().MousePos;
    int hoverKind = -1, hoverIdx = -1;
    auto near = [&](float x, float y) { return std::fabs(mp.x - x) < 8.f && std::fabs(mp.y - y) < rowH * 0.5f; };
    for (int i = 0; i < N_INPUTS; ++i)
        if (near(xIn, yOf(i, N_INPUTS))) hoverKind = 0, hoverIdx = i;
    for (int h = 0; h < N_HIDDEN; ++h)
        if (near(xHid, yOf(h, N_HIDDEN))) hoverKind = 1, hoverIdx = h;
    for (int o = 0; o < N_OUTPUTS; ++o)
        if (near(xOut, yOf(o, N_OUTPUTS))) hoverKind = 2, hoverIdx = o;

    auto edge = [&](ImVec2 a, ImVec2 b, float weight, float signal, bool highlighted) {
        if (std::fabs(weight) < thresh && !highlighted) return;
        float mag = std::min(1.f, std::fabs(weight) / 2.f);
        float act = std::min(1.f, std::fabs(signal * weight));
        float alpha = (0.08f + 0.35f * mag + 0.45f * act) * (hoverKind >= 0 && !highlighted ? 0.25f : 1.f);
        ImU32 col = weight > 0 ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 0.6f, 0.25f, alpha))
                               : ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.6f, 1.f, alpha));
        float dx = (b.x - a.x) * 0.5f;
        dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, col, 0.6f + 2.f * mag, 12);
    };
    float hid[N_HIDDEN];
    std::copy(c.hid, c.hid + N_HIDDEN, hid);
    for (int h = 0; h < N_HIDDEN; ++h) {
        ImVec2 hb(xHid, yOf(h, N_HIDDEN));
        for (int i = 0; i < N_INPUTS; ++i) {
            float wv = wt[wIH(h, i)];
            if (wv == 0.f) continue;
            bool hl = (hoverKind == 0 && hoverIdx == i) || (hoverKind == 1 && hoverIdx == h);
            edge(ImVec2(xIn, yOf(i, N_INPUTS)), hb, wv, c.in[i], hl);
        }
    }
    for (int o = 0; o < N_OUTPUTS; ++o) {
        ImVec2 ob(xOut, yOf(o, N_OUTPUTS));
        for (int h = 0; h < N_HIDDEN; ++h) {
            float wv = wt[wHO(o, h)];
            if (wv == 0.f) continue;
            bool hl = (hoverKind == 1 && hoverIdx == h) || (hoverKind == 2 && hoverIdx == o);
            edge(ImVec2(xHid, yOf(h, N_HIDDEN)), ob, wv, hid[h], hl);
        }
        for (int i = 0; i < N_INPUTS; ++i) {
            float wv = wt[wIO(o, i)];
            if (wv == 0.f) continue;
            bool hl = (hoverKind == 0 && hoverIdx == i) || (hoverKind == 2 && hoverIdx == o);
            edge(ImVec2(xIn, yOf(i, N_INPUTS)), ob, wv * 0.8f, c.in[i], hl);
        }
    }
    // Recurrent self-loops as small arcs.
    for (int h = 0; h < N_HIDDEN; ++h) {
        float wv = wt[wHH(h, h)];
        if (std::fabs(wv) < 0.2f) continue;
        float y = yOf(h, N_HIDDEN);
        dl->AddCircle(ImVec2(xHid + 9.f, y - 6.f), 6.f, wv > 0 ? IM_COL32(255, 160, 70, 120) : IM_COL32(90, 160, 255, 120),
                      12, 1.2f);
    }
    // Nodes
    const float nr = std::min(6.f, rowH * 0.42f);
    for (int i = 0; i < N_INPUTS; ++i) {
        float y = yOf(i, N_INPUTS);
        dl->AddCircleFilled(ImVec2(xIn, y), nr, actCol(c.in[i], 1.f));
        dl->AddCircle(ImVec2(xIn, y), nr, IM_COL32(0, 0, 0, 160));
        const char* name = inputName(i);
        ImVec2 ts = ImGui::CalcTextSize(name);
        bool eyeGroup = i < IN_ENERGY && (i / EC_COUNT) % 2 == 0;
        dl->AddText(ImVec2(xIn - nr - 6.f - ts.x, y - ts.y * 0.5f),
                    ImGui::GetColorU32(eyeGroup ? ImVec4(0.75f, 0.82f, 0.84f, 1) : C_MUTED), name);
    }
    for (int h = 0; h < N_HIDDEN; ++h) {
        float y = yOf(h, N_HIDDEN);
        dl->AddCircleFilled(ImVec2(xHid, y), nr * 1.35f, actCol(hid[h], 1.f));
        dl->AddCircle(ImVec2(xHid, y), nr * 1.35f, IM_COL32(0, 0, 0, 180));
    }
    for (int o = 0; o < N_OUTPUTS; ++o) {
        float y = yOf(o, N_OUTPUTS);
        float v = c.out[o];
        dl->AddCircleFilled(ImVec2(xOut, y), nr * 1.6f, actCol(v, 1.f));
        dl->AddCircle(ImVec2(xOut, y), nr * 1.6f, IM_COL32(0, 0, 0, 180));
        char lbl[48];
        std::snprintf(lbl, sizeof lbl, "%s %+.2f", outputName(o), v);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(xOut + nr * 1.6f + 6.f, y - ts.y * 0.5f), ImGui::GetColorU32(C_TEXT), lbl);
        float bw = 46.f;
        ImVec2 b0(xOut + nr * 1.6f + 6.f, y + ts.y * 0.5f + 1.f);
        dl->AddRectFilled(b0, b0 + ImVec2(bw, 3.f), ImGui::GetColorU32(C_FRAME));
        float mid = b0.x + bw * 0.5f;
        dl->AddRectFilled(ImVec2(std::min(mid, mid + v * bw * 0.5f), b0.y), ImVec2(std::max(mid, mid + v * bw * 0.5f), b0.y + 3.f),
                          actCol(v, 1.f));
    }
    ImGui::PopFont();
    if (hoverKind >= 0) {
        if (hoverKind == 0) ImGui::SetTooltip("%s = %.3f", inputName(hoverIdx), c.in[hoverIdx]);
        else if (hoverKind == 1) ImGui::SetTooltip("Hidden neuron %d = %.3f\nself-weight %.2f", hoverIdx, hid[hoverIdx],
                                                   wt[wHH(hoverIdx, hoverIdx)]);
        else ImGui::SetTooltip("%s = %.3f (bias %.2f)", outputName(hoverIdx), c.out[hoverIdx], wt[OFF_BO + hoverIdx]);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Population charts
// ---------------------------------------------------------------------------
void Ui::drawPopulation(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_LINECHART " Population", &showPopulation_)) {
        ImGui::End();
        return;
    }
    const auto& S = w.stats.samples;
    const int n = int(S.size());
    static const char* windows[] = {"All time", "Last 10 years", "Last 3 years", "Last year"};
    ImGui::SetNextItemWidth(140.f);
    ImGui::Combo("##window", &historyWindow_, windows, 4);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%d samples, one every %d ticks", n, w.stats.interval);
    ImGui::PopStyleColor();
    if (n < 2) {
        ImGui::TextUnformatted("Collecting data...");
        ImGui::End();
        return;
    }
    const double tEnd = S.back().tick;
    double tStart = S.front().tick;
    const double yl = w.params.yearLength;
    if (historyWindow_ == 1) tStart = std::max(tStart, tEnd - 10 * yl);
    if (historyWindow_ == 2) tStart = std::max(tStart, tEnd - 3 * yl);
    if (historyWindow_ == 3) tStart = std::max(tStart, tEnd - yl);
    int yearLen = w.params.yearLength;
    ImPlotSpec spec;
    spec.Stride = sizeof(StatSample);
    auto setupX = [&]() {
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxisLimits(ImAxis_X1, tStart, tEnd, ImPlotCond_Always);
        ImPlot::SetupAxisFormat(ImAxis_X1, formatYears, &yearLen);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
    };
    auto line = [&](const char* label, const float* ys, ImVec4 col, float weight = 1.8f, bool shaded = false) {
        ImPlotSpec s = spec;
        s.LineColor = col;
        s.LineWeight = weight;
        if (shaded) {
            s.FillColor = col;
            s.FillAlpha = 0.15f;
            s.Flags = ImPlotLineFlags_Shaded;
        }
        ImPlot::PlotLine(label, &S[0].tick, ys, n, s);
    };
    // Branch comparison: after a rewind, the abandoned timeline stays on the charts as a fainter line.
    auto ghostAxis = [&]() { // show where the old branch went, even past the present
        if (!app.ghost.empty() && historyWindow_ == 0)
            ImPlot::SetupAxisLimits(ImAxis_X1, tStart, std::max(tEnd, double(app.ghost.back().tick)), ImPlotCond_Always);
    };
    auto ghostLine = [&](const char* label, float StatSample::*field, ImVec4 col) {
        if (app.ghost.empty()) return;
        ImPlotSpec gs = spec;
        gs.LineColor = col;
        gs.LineWeight = 1.6f;
        ImPlot::PlotLine(label, &app.ghost[0].tick, &(app.ghost[0].*field), int(app.ghost.size()), gs);
    };
    if (ImGui::BeginTabBar("##poptabs")) {
        if (tabItem(ICON_GAUGE " Overview")) {
            // At-a-glance tiles: value, trend sparkline over the selected window.
            int first = 0;
            while (first < n - 1 && S[first].tick < tStart) ++first;
            const int m = n - first;
            struct Tile {
                const char* label;
                const float* series;
                ImVec4 color;
                const char* fmt;
                float scale;
            };
            const Tile tiles[] = {
                {"Creatures", &S[first].population, C_TEXT, "%.0f", 1.f},
                {"Living species", &S[first].species, C_ACCENT, "%.0f", 1.f},
                {"Hunters", &S[first].carnivores, C_RED, "%.0f", 1.f},
                {"Plant eaters", &S[first].herbivores, C_GREEN, "%.0f", 1.f},
                {"Mean generation", &S[first].meanGeneration, C_AMBER, "%.1f", 1.f},
                {"Synapses per brain", &S[first].connections, ImVec4(0.75f, 0.55f, 1.f, 1.f), "%.0f", 1.f},
                {"Infected", &S[first].infected, ImVec4(0.65f, 0.95f, 0.35f, 1.f), "%.0f", 1.f},
                {"Mean energy", &S[first].meanEnergy, ImVec4(0.95f, 0.75f, 0.25f, 1.f), "%.0f%%", 100.f},
            };
            const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / 230.f));
            const float tileW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * float(cols - 1)) / float(cols);
            int ti = 0;
            for (const Tile& t : tiles) {
                if (ti % cols) ImGui::SameLine();
                ImGui::PushID(ti++);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
                ImGui::BeginChild("##tile", ImVec2(tileW, 92.f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
                ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
                ImGui::TextUnformatted(t.label);
                ImGui::PopStyleColor();
                const float now = t.series[(m - 1) * int(sizeof(StatSample) / sizeof(float))] * t.scale;
                const float then = t.series[0] * t.scale;
                ImGui::PushFont(fontTitle_, fontTitle_->LegacySize * 1.2f);
                ImGui::PushStyleColor(ImGuiCol_Text, t.color);
                ImGui::Text(t.fmt, now);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::SameLine();
                float delta = then != 0.f ? (now - then) / std::fabs(then) : 0.f;
                ImGui::PushStyleColor(ImGuiCol_Text, delta >= 0 ? C_GREEN : C_RED);
                ImGui::Text("%s%.0f%%", delta >= 0 ? "+" : "", delta * 100.f);
                ImGui::PopStyleColor();
                if (m > 1) {
                    scratchA_.resize(size_t(m));
                    for (int k = 0; k < m; ++k) scratchA_[k] = t.series[k * int(sizeof(StatSample) / sizeof(float))];
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, t.color);
                    ImGui::PlotLines("##spark", scratchA_.data(), m, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 26.f));
                    ImGui::PopStyleColor();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::Text("Sea level %.3f  ·  climate %+.2f  ·  %s  ·  %llu births, %llu deaths, %llu hybrids so far",
                        w.terrain.seaLevel, w.climate(), w.seasonName(true), (unsigned long long)w.stats.totalBirths,
                        (unsigned long long)w.stats.totalDeaths, (unsigned long long)w.stats.totalHybrids);
            ImGui::PopStyleColor();
            ImGui::EndTabItem();
        }
        if (tabItem("Population")) {
            // Branch comparison: after a rewind, the abandoned timeline stays on the chart as a grey line.
            const auto& G = app.ghost;
            if (!G.empty()) {
                size_t gi = 0;
                while (gi + 1 < G.size() && G[gi].tick < tEnd) ++gi;
                ImGui::TextColored(C_MUTED, "Previous branch at this moment: %.0f creatures, %.0f species, %.0f hunters.  Now: %.0f, %.0f, %.0f.",
                                   G[gi].population, G[gi].species, G[gi].carnivores, S.back().population, S.back().species,
                                   S.back().carnivores);
                ImGui::SameLine();
                if (ImGui::SmallButton("Forget previous branch")) app.ghost.clear();
            }
            if (ImPlot::BeginPlot("##pop", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ghostAxis();
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                ghostLine("Total (previous branch)", &StatSample::population, C_MUTED);
                ghostLine("Carnivores (previous branch)", &StatSample::carnivores, ImVec4(C_RED.x, C_RED.y, C_RED.z, 0.45f));
                line("Total", &S[0].population, C_TEXT, 2.2f, true);
                line("Herbivores", &S[0].herbivores, C_GREEN);
                line("Omnivores", &S[0].omnivores, C_AMBER);
                line("Carnivores", &S[0].carnivores, C_RED);
                if (uint32_t hs = app.rs->highlightSpecies; hs && w.species.valid(hs)) {
                    const Species& sp = w.species.get(hs);
                    if (sp.historyStart >= 0 && !sp.history.empty()) {
                        scratchA_.resize(sp.history.size());
                        scratchB_.resize(sp.history.size());
                        for (size_t k = 0; k < sp.history.size(); ++k) {
                            scratchA_[k] = float(sp.historyStart + int64_t(k) * SpeciesTracker::HISTORY_INTERVAL);
                            scratchB_[k] = float(sp.history[k]);
                        }
                        ImPlotSpec hsSpec;
                        hsSpec.LineColor = v4(sp.color);
                        hsSpec.LineWeight = 2.5f;
                        ImPlot::PlotLine(sp.name().c_str(), scratchA_.data(), scratchB_.data(), int(scratchA_.size()), hsSpec);
                    }
                }
                // Mark catastrophes so their effect on the curves is easy to spot.
                scratchC_.clear();
                for (const WorldEvent& e : w.events.items)
                    if (e.kind == EventKind::Catastrophe && double(e.tick) >= tStart) scratchC_.push_back(float(e.tick));
                if (!scratchC_.empty()) {
                    ImPlotSpec es;
                    es.LineColor = ImVec4(C_RED.x, C_RED.y, C_RED.z, 0.45f);
                    es.LineWeight = 1.f;
                    ImPlot::PlotInfLines("Catastrophes", scratchC_.data(), int(scratchC_.size()), es);
                }
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Predator-prey")) {
            // Lotka-Volterra phase portrait: each loop is one boom-and-bust cycle.
            int first = 0;
            while (first < n - 1 && S[first].tick < tStart) ++first;
            if (ImPlot::BeginPlot("##phase", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Plant eaters", "Hunters", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                const int m = n - first;
                if (m > 1) {
                    // Older parts of the trajectory fade out.
                    const int segs = 8;
                    for (int sgi = 0; sgi < segs; ++sgi) {
                        int a = first + m * sgi / segs, b = std::min(n, first + m * (sgi + 1) / segs + 1);
                        if (b - a < 2) continue;
                        ImPlotSpec ps;
                        ps.LineColor = ImVec4(C_ACCENT.x, C_ACCENT.y, C_ACCENT.z, 0.15f + 0.85f * float(sgi + 1) / segs);
                        ps.LineWeight = 1.f + 1.5f * float(sgi) / segs;
                        ps.Stride = sizeof(StatSample);
                        ImGui::PushID(sgi);
                        ImPlot::PlotLine("##traj", &S[a].herbivores, &S[a].carnivores, b - a, ps);
                        ImGui::PopID();
                    }
                    ImPlotSpec now;
                    now.Marker = ImPlotMarker_Circle;
                    now.MarkerSize = 5.f;
                    now.MarkerFillColor = C_AMBER;
                    now.MarkerLineColor = C_AMBER;
                    ImPlot::PlotScatter("now", &S[n - 1].herbivores, &S[n - 1].carnivores, 1, now);
                }
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Species")) {
            // Muller-style stacked populations of the most successful species.
            const auto& all = w.species.all();
            const int H = SpeciesTracker::HISTORY_INTERVAL;
            int64_t k0 = int64_t(tStart) / H, k1 = int64_t(tEnd) / H;
            if (k1 > k0) {
                sortScratch_.clear();
                for (const Species& s : all) {
                    if (s.historyStart < 0) continue;
                    int64_t sk0 = s.historyStart / H, sk1 = sk0 + int64_t(s.history.size());
                    if (sk1 < k0 || sk0 > k1) continue;
                    int peak = 0;
                    for (int64_t k = std::max(k0, sk0); k < std::min(k1 + 1, sk1); ++k) peak = std::max(peak, int(s.history[size_t(k - sk0)]));
                    if (peak > 0) sortScratch_.push_back({float(peak), s.id});
                }
                std::sort(sortScratch_.begin(), sortScratch_.end(), [](auto& a, auto& b) { return a.first > b.first; });
                if (sortScratch_.size() > 14) sortScratch_.resize(14);
                const int m = int(k1 - k0 + 1);
                scratchA_.assign(size_t(m), 0.f); // x
                scratchB_.assign(size_t(m), 0.f); // lower
                scratchC_.assign(size_t(m), 0.f); // upper
                for (int k = 0; k < m; ++k) scratchA_[k] = float((k0 + k) * H);
                if (ImPlot::BeginPlot("##muller", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                    setupX();
                    ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Outside);
                    for (auto& [peak, id] : sortScratch_) {
                        const Species& s = w.species.get(id);
                        int64_t sk0 = s.historyStart / H;
                        for (int k = 0; k < m; ++k) {
                            int64_t kk = k0 + k - sk0;
                            float v = (kk >= 0 && kk < int64_t(s.history.size())) ? float(s.history[size_t(kk)]) : 0.f;
                            scratchB_[k] = scratchC_[k];
                            scratchC_[k] += v;
                        }
                        ImPlotSpec ss;
                        ss.FillColor = v4(s.color);
                        ss.FillAlpha = 0.85f;
                        ImGui::PushID(int(id));
                        ImPlot::PlotShaded(s.name().c_str(), scratchA_.data(), scratchB_.data(), scratchC_.data(), m, ss);
                        ImGui::PopID();
                    }
                    ImPlotSpec ls;
                    ls.LineColor = C_TEXT;
                    ls.Stride = sizeof(StatSample);
                    ImPlot::PlotLine("All species", &S[0].tick, &S[0].population, n, ls);
                    ImPlot::EndPlot();
                }
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Diversity")) {
            if (ImPlot::BeginPlot("##div", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ghostAxis();
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                ghostLine("Living species (previous branch)", &StatSample::species, C_MUTED);
                line("Living species", &S[0].species, C_ACCENT, 2.f, true);
                line("Births / sample", &S[0].births, ImVec4(0.6f, 0.9f, 0.6f, 0.8f), 1.f);
                line("Deaths / sample", &S[0].deaths, ImVec4(0.9f, 0.5f, 0.5f, 0.8f), 1.f);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Environment")) {
            if (ImPlot::BeginPlot("##env", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ImPlot::SetupAxis(ImAxis_Y2, "climate", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                line("Plant biomass", &S[0].plants, C_GREEN, 2.f, true);
                line("Meat", &S[0].meat, C_RED);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("Climate offset", &S[0].climate, ImVec4(0.5f, 0.75f, 1.f, 1.f));
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Minds")) {
            if (ImPlot::BeginPlot("##minds", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ImPlot::SetupAxis(ImAxis_Y2, "generation", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                line("Synapses (mean)", &S[0].connections, ImVec4(0.75f, 0.55f, 1.f, 1.f), 2.f);
                line("Energy (mean, x100)", &S[0].meanEnergy, C_AMBER);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("Mean generation", &S[0].meanGeneration, C_ACCENT);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Behaviour")) {
            if (ImPlot::BeginPlot("##behav", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ImPlot::SetupAxis(ImAxis_Y2, "attacks", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                line("Signalling (share)", &S[0].signalling, ImVec4(0.45f, 0.9f, 1.f, 1.f));
                line("Laying scent (share)", &S[0].marking, ImVec4(0.85f, 0.45f, 1.f, 1.f));
                line("Sharing food (share)", &S[0].sharing, C_AMBER);
                line("Attacks on own species", &S[0].cannibalism, ImVec4(1.f, 0.55f, 0.5f, 1.f));
                line("Toxic creatures (share)", &S[0].toxic, ImVec4(0.75f, 1.f, 0.3f, 0.6f), 1.f);
                line("Attacks hitting toxic prey", &S[0].toxicTargeted, ImVec4(0.75f, 1.f, 0.3f, 1.f), 2.f);
                line("Bites in pack attacks", &S[0].packHunting, ImVec4(1.f, 0.7f, 0.3f, 1.f), 1.5f);
                line("Camouflaged (share)", &S[0].camouflaged, ImVec4(0.45f, 0.75f, 0.35f, 1.f), 1.5f);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("Attacks / creature / 100 ticks", &S[0].attackRate, C_RED, 2.f);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Game theory")) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("When two creatures graze the same patch at once, their aggression decides the payoff: doves split "
                               "the food, a hawk bullies a dove, and two hawks fight. Does a mix of hawks and doves settle "
                               "into a stable ratio?");
            ImGui::PopStyleColor();
            if (ImPlot::BeginPlot("##hawkdove", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ImPlot::SetupAxis(ImAxis_Y2, "contests", ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_AutoFit);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                line("Hawk share", &S[0].hawkShare, C_RED, 2.5f);
                line("Dove-dove", &S[0].doveDove, C_GREEN, 1.2f);
                line("Hawk-dove", &S[0].hawkDove, C_AMBER, 1.2f);
                line("Hawk-hawk", &S[0].hawkHawk, ImVec4(0.8f, 0.3f, 0.6f, 1.f), 1.2f);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("Contests / 100 ticks", &S[0].contests, ImVec4(0.6f, 0.6f, 0.65f, 0.7f), 1.f);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Epidemic")) {
            scratchA_.resize(size_t(n));
            for (int k = 0; k < n; ++k) scratchA_[k] = S[k].population - S[k].infected - S[k].immune;
            if (ImPlot::BeginPlot("##sir", ImVec2(-1, -1), ImPlotFlags_NoTitle)) {
                setupX();
                ImPlot::SetupAxis(ImAxis_Y2, "strain aggressiveness", ImPlotAxisFlags_AuxDefault);
                ImPlot::SetupAxisLimits(ImAxis_Y2, 0, 1, ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);
                // xs strided over samples, ys contiguous: copy ticks so both share a stride.
                scratchB_.resize(size_t(n));
                for (int k = 0; k < n; ++k) scratchB_[k] = S[k].tick;
                ImPlotSpec q;
                q.LineColor = ImVec4(0.55f, 0.7f, 0.95f, 1.f);
                q.LineWeight = 1.8f;
                ImPlot::PlotLine("Susceptible", scratchB_.data(), scratchA_.data(), n, q);
                line("Infected", &S[0].infected, ImVec4(0.65f, 0.95f, 0.35f, 1.f), 2.2f, true);
                line("Recovered (immune)", &S[0].immune, C_AMBER, 1.8f);
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                line("Pathogen aggressiveness", &S[0].strainAggression, ImVec4(0.85f, 0.3f, 0.75f, 1.f), 1.5f);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Causes of death")) {
            static const char* labels[] = {"Starvation", "Old age", "Predation", "Poison", "Meteor", "Smitten", "Disease", "Thirst"};
            double vals[8];
            for (int k = 0; k < 8; ++k) vals[k] = double(w.stats.causeTotals[k + 1]);
            if (ImPlot::BeginPlot("##causes", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupAxisTicks(ImAxis_Y1, 0, 7, 8, labels);
                ImPlotSpec bs;
                bs.FillColor = C_ACCENT_DIM;
                bs.Flags = ImPlotBarsFlags_Horizontal;
                ImPlot::PlotBars("Deaths", vals, 8, 0.6, 0, bs);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Traits
// ---------------------------------------------------------------------------
void Ui::drawTraits(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_FLASK " Traits", &showTraits_)) {
        ImGui::End();
        return;
    }
    const auto& S = w.stats.samples;
    const int n = int(S.size());
    auto geneCombo = [&](const char* id, int* g) {
        ImGui::SetNextItemWidth(150.f);
        if (ImGui::BeginCombo(id, GENE_INFO[*g].name)) {
            for (int i = 0; i < G_COUNT; ++i)
                if (ImGui::Selectable(GENE_INFO[i].name, *g == i)) *g = i;
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", GENE_INFO[*g].description);
    };
    int yearLen = w.params.yearLength;
    if (ImGui::BeginTabBar("##traittabs")) {
        if (tabItem("Evolution")) {
            geneCombo("##gene", &traitGene_);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("How the whole population's %s has shifted over time. Brighter = more creatures.",
                               GENE_INFO[traitGene_].name);
            ImGui::PopStyleColor();
            if (n >= 2) {
                const int cols = n, rows = TRAIT_BINS;
                scratchA_.assign(size_t(rows) * cols, 0.f);
                for (int c = 0; c < cols; ++c) {
                    const auto& h = w.stats.traitHist[c][traitGene_];
                    float mx = 0.f;
                    for (int b = 0; b < rows; ++b) mx = std::max(mx, float(h[b]));
                    if (mx <= 0.f) continue;
                    for (int b = 0; b < rows; ++b)
                        scratchA_[size_t(rows - 1 - b) * cols + c] = std::sqrt(float(h[b]) / mx);
                }
                ImPlot::PushColormap(ImPlotColormap_Viridis);
                if (ImPlot::BeginPlot("##traitheat", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
                    ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoLabel);
                    ImPlot::SetupAxisFormat(ImAxis_X1, formatYears, &yearLen);
                    ImPlot::SetupAxisLimits(ImAxis_X1, S.front().tick, S.back().tick, ImPlotCond_Always);
                    ImPlot::SetupAxis(ImAxis_Y1, GENE_INFO[traitGene_].name);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1, ImPlotCond_Always);
                    ImPlotSpec hs;
                    ImPlot::PlotHeatmap("##h", scratchA_.data(), rows, cols, 0.0, 1.0, nullptr,
                                        ImPlotPoint(S.front().tick, 0), ImPlotPoint(S.back().tick, 1), hs);
                    ImPlotSpec ls;
                    ls.LineColor = ImVec4(1, 1, 1, 0.85f);
                    ls.LineWeight = 1.5f;
                    ls.Stride = sizeof(StatSample);
                    ImPlot::PlotLine("Mean", &S[0].tick, &S[0].geneMean[traitGene_], n, ls);
                    ImPlot::EndPlot();
                }
                ImPlot::PopColormap();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Morphospace")) {
            geneCombo("X##sx", &scatterX_);
            ImGui::SameLine();
            geneCombo("Y##sy", &scatterY_);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextUnformatted("each dot is a living creature, coloured by species");
            ImGui::PopStyleColor();
            if (ImPlot::BeginPlot("##morpho", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes(GENE_INFO[scatterX_].name, GENE_INFO[scatterY_].name);
                ImPlot::SetupAxesLimits(0, 1, 0, 1, ImPlotCond_Once);
                // Group creatures by species (top 12 by population, rest grey).
                sortScratch_.clear();
                for (const Species& s : w.species.all())
                    if (s.alive() && s.population > 0) sortScratch_.push_back({float(s.population), s.id});
                std::sort(sortScratch_.begin(), sortScratch_.end(), [](auto& a, auto& b) { return a.first > b.first; });
                if (sortScratch_.size() > 12) sortScratch_.resize(12);
                for (int grp = -1; grp < int(sortScratch_.size()); ++grp) {
                    scratchA_.clear();
                    scratchB_.clear();
                    for (const Creature& c : w.creatures) {
                        bool inTop = false;
                        for (auto& e : sortScratch_) inTop |= e.second == c.speciesId;
                        bool mine = grp < 0 ? !inTop : c.speciesId == sortScratch_[size_t(grp)].second;
                        if (!mine) continue;
                        scratchA_.push_back(c.genome.g[scatterX_]);
                        scratchB_.push_back(c.genome.g[scatterY_]);
                    }
                    if (scratchA_.empty()) continue;
                    Color col = grp < 0 ? Color{0.5f, 0.55f, 0.58f, 0.6f} : w.species.get(sortScratch_[size_t(grp)].second).color;
                    ImPlotSpec ss;
                    ss.Marker = ImPlotMarker_Circle;
                    ss.MarkerSize = 2.5f;
                    ss.MarkerFillColor = v4(col);
                    ss.MarkerLineColor = ImVec4(0, 0, 0, 0);
                    ss.FillAlpha = 0.75f;
                    ImGui::PushID(grp);
                    ImPlot::PlotScatter(grp < 0 ? "other" : w.species.get(sortScratch_[size_t(grp)].second).name().c_str(),
                                        scratchA_.data(), scratchB_.data(), int(scratchA_.size()), ss);
                    ImGui::PopID();
                }
                if (int i = w.indexOfId(app.selectedId); i >= 0) {
                    float x = w.creatures[i].genome.g[scatterX_], y = w.creatures[i].genome.g[scatterY_];
                    ImPlotSpec ss;
                    ss.Marker = ImPlotMarker_Circle;
                    ss.MarkerSize = 7.f;
                    ss.MarkerFillColor = ImVec4(0, 0, 0, 0);
                    ss.MarkerLineColor = C_AMBER;
                    ss.LineWeight = 2.f;
                    ImPlot::PlotScatter("selected", &x, &y, 1, ss);
                }
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Senses")) {
            // Which inputs do evolved brains actually listen to? Mean |weight| leaving each input.
            if (senseImportance_.empty() || ++senseFrame_ % 30 == 0) {
                senseImportance_.assign(N_INPUTS, 0.f);
                for (const Creature& c : w.creatures) {
                    const float* wt = c.genome.w.data();
                    for (int i = 0; i < N_INPUTS; ++i) {
                        float s = 0.f;
                        for (int h = 0; h < N_HIDDEN; ++h) s += std::fabs(wt[wIH(h, i)]);
                        for (int o = 0; o < N_OUTPUTS; ++o) s += std::fabs(wt[wIO(o, i)]);
                        senseImportance_[i] += s;
                    }
                }
                float n = float(std::max<size_t>(1, w.creatures.size()));
                for (float& v : senseImportance_) v /= n;
            }
            // Collapse the 5 eyes into one bar per visual channel.
            const int nb = EC_COUNT + (N_INPUTS - IN_ENERGY);
            static const char* names[64];
            static double vals[64];
            for (int ch = 0; ch < EC_COUNT; ++ch) {
                double s = 0;
                for (int e = 0; e < NUM_EYES; ++e) s += senseImportance_[e * EC_COUNT + ch];
                vals[ch] = s / NUM_EYES;
                names[ch] = eyeChannelName(ch);
            }
            for (int i = IN_ENERGY; i < N_INPUTS; ++i) {
                vals[EC_COUNT + i - IN_ENERGY] = senseImportance_[i];
                names[EC_COUNT + i - IN_ENERGY] = inputName(i);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("Average synapse strength leaving each sense, across all living creatures. Eye channels are averaged over the five eyes. Bars that grow over time are senses natural selection finds useful.");
            ImGui::PopStyleColor();
            if (ImPlot::BeginPlot("##senses", ImVec2(-1, -1), ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_Invert);
                ImPlot::SetupAxisTicks(ImAxis_Y1, 0, nb - 1, nb, names);
                ImPlotSpec bs;
                bs.FillColor = C_ACCENT_DIM;
                bs.LineColor = C_ACCENT;
                bs.Flags = ImPlotBarsFlags_Horizontal;
                ImPlot::PlotBars("strength", vals, nb, 0.7, 0, bs);
                ImPlot::EndPlot();
            }
            ImGui::EndTabItem();
        }
        if (tabItem("Averages")) {
            if (n >= 1 && ImGui::BeginTable("##means", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                              ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Gene");
                ImGui::TableSetupColumn("Mean");
                ImGui::TableSetupColumn("Spread");
                ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 3.f);
                ImGui::TableHeadersRow();
                for (int g = 0; g < G_COUNT; ++g) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(GENE_INFO[g].name);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", GENE_INFO[g].description);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", S.back().geneMean[g]);
                    ImGui::TableNextColumn();
                    ImGui::Text("±%.3f", S.back().geneStd[g]);
                    ImGui::TableNextColumn();
                    ImGui::PushID(g);
                    ImGui::PlotLines("##spark", &S[0].geneMean[g], n, 0, nullptr, 0.f, 1.f,
                                     ImVec2(-1, ImGui::GetTextLineHeight()), int(sizeof(StatSample)));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Species list
// ---------------------------------------------------------------------------
void Ui::drawSpecies(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_USERS " Species", &showSpecies_)) {
        ImGui::End();
        return;
    }
    const auto& all = w.species.all();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%d living · %zu ever · %zu extinct", w.species.aliveCount(), all.size(),
                all.size() - size_t(w.species.aliveCount()));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Checkbox("Show extinct", &showExtinct_);
    // Leaderboard of founding lineages: how the creatures you designed are doing against the wild ones.
    // Opens by itself the first time one of your own designs is in the world.
    bool anyDesigned = false;
    for (const WorldEvent& e : w.events.items)
        if (e.kind == EventKind::Speciation && e.text.rfind("Designed lineage", 0) == 0) { anyDesigned = true; break; }
    if (anyDesigned) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::CollapsingHeader(ICON_STAR "  Lineage leaderboard")) {
        const float total = float(std::max<size_t>(1, w.creatures.size()));
        if (ImGui::BeginTable("##lineages", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.f);
            ImGui::TableSetupColumn("Founding lineage");
            ImGui::TableSetupColumn("Creatures");
            ImGui::TableSetupColumn("Share");
            ImGui::TableSetupColumn("Species (ever)");
            ImGui::TableHeadersRow();
            int rank = 0;
            for (const LineageScore& ls : w.lineageScores()) {
                if (!ls.population && !ls.designed) continue; // extinct wild lineages are in the table below
                const Species& root = w.species.get(ls.root);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%d", ++rank);
                ImGui::TableNextColumn();
                ImGui::PushID(int(ls.root));
                ImGui::PushStyleColor(ImGuiCol_Text, ls.population ? v4(root.color) : C_MUTED);
                const std::string label = (ls.designed ? std::string(ICON_FLASK " ") : std::string()) + root.name();
                if (ImGui::Selectable(label.c_str(), app.rs->highlightSpecies == ls.root, ImGuiSelectableFlags_SpanAllColumns))
                    app.rs->highlightSpecies = ls.root;
                ImGui::PopStyleColor();
                if (ls.designed && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("You designed this lineage in the Genome Lab");
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (ls.population) ImGui::Text("%d", ls.population);
                else ImGui::TextColored(C_MUTED, "extinct");
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", 100.f * float(ls.population) / total);
                ImGui::TableNextColumn();
                ImGui::Text("%d (%d)", ls.livingSpecies, ls.speciesEver);
            }
            ImGui::EndTable();
        }
    }
    if (app.rs->highlightSpecies && w.species.valid(app.rs->highlightSpecies)) {
        const Species& hs = w.species.get(app.rs->highlightSpecies);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
        ImGui::BeginChild("##card", ImVec2(0, 200), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        // Radar chart of the species' average body plan.
        static const int axes[] = {G_SIZE, G_SPEED, G_DIET, G_ARMOR, G_WEAPON, G_TOXIN, G_VISION_RANGE, G_IMMUNITY, G_LITTER};
        static const char* axisNames[] = {"Size", "Speed", "Meat", "Armor", "Weapon", "Toxin", "Sight", "Immune", "Litter"};
        const int na = 9;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float R = 62.f;
        ImVec2 ctr = p0 + ImVec2(R + 34.f, R + 14.f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int ring = 1; ring <= 4; ++ring) {
            float rr = R * float(ring) / 4.f;
            for (int a = 0; a < na; ++a) {
                float t0 = TAU * float(a) / na - PI * 0.5f, t1 = TAU * float(a + 1) / na - PI * 0.5f;
                dl->AddLine(ctr + ImVec2(std::cos(t0), std::sin(t0)) * rr, ctr + ImVec2(std::cos(t1), std::sin(t1)) * rr,
                            IM_COL32(255, 255, 255, 22));
            }
        }
        ImVec2 pts[na];
        for (int a = 0; a < na; ++a) {
            float t = TAU * float(a) / na - PI * 0.5f;
            ImVec2 dir(std::cos(t), std::sin(t));
            dl->AddLine(ctr, ctr + dir * R, IM_COL32(255, 255, 255, 25));
            float v = clamp01(hs.meanGenes[axes[a]]);
            pts[a] = ctr + dir * (R * (0.08f + 0.92f * v));
            ImVec2 ts = ImGui::CalcTextSize(axisNames[a]);
            dl->AddText(ctr + dir * (R + 12.f) - ts * 0.5f, ImGui::GetColorU32(C_MUTED), axisNames[a]);
        }
        dl->AddConvexPolyFilled(pts, na, u32(hs.color, 0.35f));
        dl->AddPolyline(pts, na, u32(hs.color), ImDrawFlags_Closed, 2.f);
        ImGui::Dummy(ImVec2(2 * R + 70.f, 2 * R + 28.f));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(fontTitle_, fontTitle_->LegacySize);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(hs.color));
        ImGui::TextUnformatted(hs.name().c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Double-click to rename");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            renameId_ = hs.id;
            std::snprintf(renameBuf_, sizeof renameBuf_, "%s", hs.name().c_str());
            ImGui::OpenPopup("##rename");
        }
        if (ImGui::BeginPopup("##rename")) {
            ImGui::TextUnformatted("Name this species");
            ImGui::SetNextItemWidth(240.f);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            bool done = ImGui::InputText("##name", renameBuf_, sizeof renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
            if ((done || ImGui::Button("Rename")) && w.species.valid(renameId_)) {
                Species& target = w.species.get(renameId_);
                std::string nm = renameBuf_;
                size_t sp = nm.find(' ');
                target.genus = sp == std::string::npos ? nm : nm.substr(0, sp);
                target.epithet = sp == std::string::npos ? "" : nm.substr(sp + 1);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        // A field-guide style description built from the average genome.
        const auto& g = hs.meanGenes;
        std::string d;
        float sz = g[G_SIZE];
        d += sz < 0.2f ? "Tiny" : sz < 0.4f ? "Small" : sz < 0.6f ? "Mid-sized" : sz < 0.8f ? "Large" : "Giant";
        if (g[G_SPEED] > 0.7f) d += ", fast";
        if (g[G_SPEED] < 0.3f) d += ", slow";
        if (g[G_ARMOR] > 0.5f) d += ", armoured";
        if (g[G_TOXIN] > 0.4f) d += ", toxic";
        if (g[G_VISION_RANGE] > 0.65f) d += ", keen-eyed";
        d += std::string(" ") + (g[G_DIET] < 0.33f ? "herbivore" : g[G_DIET] < 0.66f ? "omnivore" : "predator");
        if (g[G_THERMAL] < 0.35f) d += " of cold climates";
        else if (g[G_THERMAL] > 0.7f) d += " of hot climates";
        if (g[G_LITTER] > 0.6f) d += " that breeds in large litters";
        else if (g[G_INVEST] > 0.7f) d += " that invests heavily in few young";
        if (g[G_IMMUNITY] > 0.4f) d += ", resistant to disease";
        if (g[G_NIGHT_VISION] > 0.55f) d += ", active at night";
        if (g[G_CHOOSY] > 0.5f) d += ", choosy about mates";
        d += ".";
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
        ImGui::TextUnformatted(d.c_str());
        for (const OrganTally& t : w.organTallies())
            if (t.species == hs.id && t.count * 3 >= hs.population)
                ImGui::TextWrapped("Organ (%d of %d carry it): %s", t.count, hs.population, organText(t.organ).c_str());
        if (hs.alive()) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_GREEN);
            ImGui::TextWrapped(ICON_STAR " Best at: %s", w.bestAtText(hs.id).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                if (const SpeciesSkills* sk = w.skillsOf(hs.id)) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Skill vs the other living species (standard score)");
                    for (int k = 0; k < N_SKILLS; ++k) ImGui::Text("%-18s %+.1f", skillName(k), sk->z[k]);
                    ImGui::EndTooltip();
                }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("%s · %d alive · peak %d · %llu born", hs.alive() ? "Extant" : "Extinct", hs.population,
                    hs.peakPopulation, (unsigned long long)hs.totalBorn);
        ImGui::Text("Arose in year %.1f%s%s", double(hs.bornTick) / w.params.yearLength,
                    hs.parentId ? " from " : " as a founding lineage",
                    hs.parentId ? w.species.get(hs.parentId).name().c_str() : "");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (hs.history.size() > 1) {
            scratchA_.assign(hs.history.begin(), hs.history.end());
            ImGui::PushStyleColor(ImGuiCol_PlotLines, v4(hs.color));
            ImGui::PlotLines("##hist", scratchA_.data(), int(scratchA_.size()), 0, nullptr, 0.f, FLT_MAX,
                             ImVec2(ImGui::GetContentRegionAvail().x - 4.f, 36.f));
            ImGui::PopStyleColor();
        }
        if (ImGui::SmallButton(ICON_CROSSHAIRS " Go to")) app.cam->targetCenter = hs.centroid, app.follow = false;
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_TIMES " Clear")) app.rs->highlightSpecies = 0;
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                         ImGuiTableFlags_Hideable | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##species", 7, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const float numW = ImGui::CalcTextSize("00000").x;
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pop", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort |
                                           ImGuiTableColumnFlags_PreferSortDescending, numW);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, numW);
        ImGui::TableSetupColumn("Diet", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Plants").x);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, numW * 0.8f);
        ImGui::TableSetupColumn("Toxin", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, numW);
        ImGui::TableSetupColumn("Trend", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, numW * 1.2f);
        ImGui::TableHeadersRow();
        // Compact mode for narrow panels: keep name, population, diet and trend.
        const bool compact = ImGui::GetContentRegionAvail().x < 420.f;
        ImGui::TableSetColumnEnabled(2, !compact);
        ImGui::TableSetColumnEnabled(4, !compact);
        ImGui::TableSetColumnEnabled(5, !compact);

        std::vector<const Species*> rows;
        rows.reserve(all.size());
        for (const Species& s : all)
            if (s.alive() || (showExtinct_ && s.peakPopulation >= 3)) rows.push_back(&s);
        auto age = [&](const Species* s) { return (s->alive() ? w.tickCount : s->extinctTick) - s->bornTick; };
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& spec = ss->Specs[0];
            auto key = [&](const Species* s) -> double {
                switch (spec.ColumnIndex) {
                    case 0: return -double(s->id);
                    case 1: return s->population;
                    case 2: return double(age(s));
                    case 3: return s->meanGenes[G_DIET];
                    case 4: return s->meanGenes[G_SIZE];
                    case 5: return s->meanGenes[G_TOXIN];
                }
                return 0;
            };
            bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(rows.begin(), rows.end(), [&](const Species* a, const Species* b) {
                return asc ? key(a) < key(b) : key(a) > key(b);
            });
        }
        ImGuiListClipper clip;
        clip.Begin(int(rows.size()));
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const Species& s = *rows[size_t(r)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(int(s.id));
                {
                    unsigned tex = 0;
                    float uv[4];
                    const float isz = ImGui::GetTextLineHeight() * 1.25f;
                    if (app.renderer->speciesIcon(w, s.id, tex, uv)) {
                        ImVec2 cp = ImGui::GetCursorScreenPos();
                        ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ImTextureID(intptr_t(tex))),
                                                             cp + ImVec2(-2, -isz * 0.12f), cp + ImVec2(isz - 2, isz * 0.88f),
                                                             ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
                        ImGui::Dummy(ImVec2(isz - 4, ImGui::GetTextLineHeight()));
                    } else {
                        swatch(s.color);
                    }
                }
                ImGui::SameLine();
                if (!s.alive()) ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
                bool sel = app.rs->highlightSpecies == s.id;
                if (ImGui::Selectable(s.name().c_str(), sel,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    app.rs->highlightSpecies = sel ? 0 : s.id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && s.alive()) {
                        app.cam->targetCenter = s.centroid;
                        app.follow = false;
                    }
                }
                if (!s.alive()) ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(ICON_CROSSHAIRS " Go to")) app.cam->targetCenter = s.centroid, app.follow = false;
                    if (ImGui::MenuItem(ICON_SEARCH " Select a member")) {
                        for (const Creature& c : w.creatures)
                            if (c.speciesId == s.id) { app.selectedId = c.id; app.follow = true; break; }
                    }
                    if (ImGui::MenuItem(ICON_COPY " Copy representative genome")) {
                        app.clipboard = s.rep;
                        app.clipboardSpecies = s.id;
                        app.hasClipboard = true;
                        app.tool = Tool::Clone;
                    }
                    if (s.alive() && ImGui::MenuItem(ICON_BUG " Start an epidemic")) w.plague(s.id, 0.15f);
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s", s.name().c_str());
                    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
                    ImGui::Text("Branched from %s in year %.1f", s.parentId ? w.species.get(s.parentId).name().c_str() : "(founder)",
                                double(s.bornTick) / w.params.yearLength);
                    ImGui::Text("%llu born in total, %zu daughter species", (unsigned long long)s.totalBorn, s.children.size());
                    if (!s.alive()) ImGui::Text("Extinct in year %.1f", double(s.extinctTick) / w.params.yearLength);
                    ImGui::PopStyleColor();
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("%d", s.population);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(years(age(&s), w.params.yearLength).c_str());
                ImGui::TableNextColumn();
                {
                    float d = s.meanGenes[G_DIET];
                    ImVec4 col = d < 0.33f ? C_GREEN : d < 0.66f ? C_AMBER : C_RED;
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(d < 0.33f ? "Plants" : d < 0.66f ? "Mixed" : "Meat");
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", lerp(5.f, 20.f, s.meanGenes[G_SIZE]));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", s.meanGenes[G_TOXIN]);
                ImGui::TableNextColumn();
                if (s.history.size() > 1) {
                    const size_t keep = std::min<size_t>(s.history.size(), 60);
                    scratchA_.assign(s.history.end() - long(keep), s.history.end());
                    ImGui::PushID(int(s.id));
                    // Tint by recent trajectory: collapsing species read red, booming ones green.
                    const size_t win = std::min<size_t>(scratchA_.size(), 12);
                    const float recent = scratchA_.back(), before = scratchA_[scratchA_.size() - win];
                    ImVec4 tcol = v4(s.color);
                    if (before > 4.f && recent < before * 0.5f) tcol = C_RED;
                    else if (recent > 4.f && recent > before * 1.6f) tcol = C_GREEN;
                    ImGui::PushStyleColor(ImGuiCol_PlotLines, tcol);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                    ImGui::PlotLines("##trend", scratchA_.data(), int(scratchA_.size()), 0, nullptr, 0.f, FLT_MAX,
                                     ImVec2(-1, ImGui::GetTextLineHeight()));
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Tree of life
// ---------------------------------------------------------------------------
void Ui::drawPhylogeny(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_SITEMAP " Tree of Life", &showPhylogeny_)) {
        ImGui::End();
        return;
    }
    auto& all = w.species.all();
    ImGui::SetNextItemWidth(120.f);
    ImGui::SliderInt("min. peak", &minPhyloPeak_, 1, 100);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Hide short-lived species whose population never reached this size");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextUnformatted("scroll = zoom time · drag = pan · click = highlight");
    ImGui::PopStyleColor();

    // Visible set: notable species plus all of their ancestors.
    const size_t N = all.size();
    std::vector<uint8_t> vis(N, 0);
    for (const Species& s : all)
        if (s.peakPopulation >= minPhyloPeak_ || (s.alive() && s.population >= std::max(1, minPhyloPeak_ / 2))) {
            uint32_t id = s.id;
            while (id && !vis[id - 1]) {
                vis[id - 1] = 1;
                id = all[id - 1].parentId;
            }
        }
    // Depth-first ordering, children sorted by birth.
    std::vector<int> row(N, -1);
    std::vector<uint32_t> order;
    std::vector<uint32_t> stack;
    for (const Species& s : all)
        if (vis[s.id - 1] && (s.parentId == 0 || !vis[s.parentId - 1])) stack.push_back(s.id);
    std::reverse(stack.begin(), stack.end());
    while (!stack.empty()) {
        uint32_t id = stack.back();
        stack.pop_back();
        row[id - 1] = int(order.size());
        order.push_back(id);
        const auto& ch = all[id - 1].children;
        for (auto it = ch.rbegin(); it != ch.rend(); ++it)
            if (vis[*it - 1]) stack.push_back(*it);
    }
    const float rowH = 15.f;
    ImGui::BeginChild("##phylo", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse * 0);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float labelW = 170.f;
    float height = std::max(avail.y, rowH * float(order.size()) + 30.f);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##phylocanvas", ImVec2(avail.x, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double tMax = std::max<double>(1.0, double(w.tickCount));
    const float plotW = std::max(50.f, avail.x - labelW - 10.f);
    // Zoom/pan on the time axis.
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0.f && io.KeyCtrl == false) {
        float mxFrac = clamp01((io.MousePos.x - origin.x) / plotW);
        float oldZoom = phyloZoom_;
        phyloZoom_ = clampf(phyloZoom_ * std::pow(1.2f, io.MouseWheel), 1.f, 200.f);
        phyloScroll_ += mxFrac * (1.f / oldZoom - 1.f / phyloZoom_);
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) phyloScroll_ -= io.MouseDelta.x / plotW / phyloZoom_;
    phyloScroll_ = clampf(phyloScroll_, 0.f, 1.f - 1.f / phyloZoom_);
    auto xOf = [&](double t) { return origin.x + float((t / tMax - phyloScroll_) * phyloZoom_) * plotW; };
    const float clipL = ImGui::GetWindowPos().x, clipR = clipL + ImGui::GetWindowWidth();
    // Year grid
    const double yl = w.params.yearLength;
    double yearsTotal = tMax / yl;
    // Pick a "nice" tick step that leaves at least ~60px between labels.
    const double pxPerYear = double(plotW) * phyloZoom_ / std::max(1e-6, yearsTotal);
    double stepY = 0.1;
    for (double cand : {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0}) {
        stepY = cand;
        if (cand * pxPerYear >= 60.0) break;
    }
    float scrollY = ImGui::GetScrollY();
    float topY = origin.y + scrollY;
    for (double y = 0; y <= yearsTotal; y += stepY) {
        float x = xOf(y * yl);
        if (x < origin.x - 1 || x > origin.x + plotW + 1) continue;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(255, 255, 255, 12));
        char b[16];
        std::snprintf(b, sizeof b, "%gy", y);
        dl->AddText(ImVec2(x + 3, topY + 2), ImGui::GetColorU32(C_MUTED), b);
    }
    // Lineage of the selected creature, drawn in amber.
    std::vector<uint8_t> onPath(N, 0);
    if (int si = w.indexOfId(app.selectedId); si >= 0)
        for (uint32_t id = w.creatures[si].speciesId; id && w.species.valid(id); id = all[id - 1].parentId) onPath[id - 1] = 1;
    // Branches
    const float y0 = origin.y + 22.f;
    int hoverId = 0;
    float visTop = scrollY - rowH, visBot = scrollY + avail.y + rowH;
    for (uint32_t id : order) {
        const Species& s = all[id - 1];
        float y = y0 + rowH * float(row[id - 1]);
        float x0 = xOf(double(s.bornTick));
        float x1 = xOf(s.alive() ? double(w.tickCount) : double(s.extinctTick));
        bool sel = app.rs->highlightSpecies == id;
        ImU32 col = u32(s.color, s.alive() ? 1.f : 0.45f);
        if (s.parentId && vis[s.parentId - 1]) {
            float py = y0 + rowH * float(row[s.parentId - 1]);
            if (!(std::max(py, y) < origin.y + visTop || std::min(py, y) > origin.y + visBot))
                dl->AddLine(ImVec2(x0, py), ImVec2(x0, y),
                            onPath[id - 1] ? ImGui::GetColorU32(C_AMBER) : u32(s.color, 0.35f), onPath[id - 1] ? 2.5f : 1.f);
        }
        if (y < origin.y + visTop || y > origin.y + visBot) continue;
        if (x1 < clipL || x0 > clipR) continue;
        // Population-weighted thickness from history.
        const int H = SpeciesTracker::HISTORY_INTERVAL;
        float peak = float(std::max(1, s.peakPopulation));
        if (s.historyStart >= 0 && s.history.size() > 1) {
            for (size_t k = 0; k + 1 < s.history.size(); ++k) {
                float xa = xOf(double(s.historyStart + int64_t(k) * H)), xb = xOf(double(s.historyStart + int64_t(k + 1) * H));
                if (xb < clipL || xa > clipR) continue;
                float th = 1.f + 5.f * std::sqrt(float(s.history[k]) / std::max(peak, 20.f));
                dl->AddRectFilled(ImVec2(xa, y - th * 0.5f), ImVec2(xb + 0.5f, y + th * 0.5f), col);
            }
        } else {
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col, 1.5f);
        }
        if (onPath[id - 1]) dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImVec4(C_AMBER.x, C_AMBER.y, C_AMBER.z, 0.5f)), 6.f);
        dl->AddCircleFilled(ImVec2(x0, y), 2.5f, col);
        if (!s.alive()) dl->AddLine(ImVec2(x1, y - 4), ImVec2(x1, y + 4), u32(s.color, 0.6f), 1.5f);
        // Label to the right of living lineages.
        if (s.alive() || sel) {
            std::string label = s.name() + "  " + std::to_string(s.population);
            float lx = std::min(x1 + 6.f, origin.x + plotW + 6.f);
            unsigned tex = 0;
            float uv[4];
            if (s.alive() && app.renderer->speciesIcon(w, s.id, tex, uv)) {
                const float isz = rowH * 1.1f;
                dl->AddImage(ImTextureRef(ImTextureID(intptr_t(tex))), ImVec2(lx, y - isz * 0.5f), ImVec2(lx + isz, y + isz * 0.5f),
                             ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
                lx += isz + 2.f;
            }
            dl->AddText(ImVec2(lx, y - ImGui::GetTextLineHeight() * 0.5f),
                        sel ? ImGui::GetColorU32(C_AMBER) : ImGui::GetColorU32(C_TEXT), label.c_str());
        }
        if (sel) dl->AddRect(ImVec2(x0 - 3, y - 6), ImVec2(x1 + 3, y + 6), ImGui::GetColorU32(C_AMBER), 3.f);
        if (hovered && std::fabs(io.MousePos.y - y) < rowH * 0.5f && io.MousePos.x >= x0 - 4 && io.MousePos.x <= x1 + 200)
            hoverId = int(id);
    }
    if (hoverId) {
        const Species& s = all[size_t(hoverId) - 1];
        ImGui::BeginTooltip();
        ImGui::Text("%s", s.name().c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("Year %.1f -> %s", double(s.bornTick) / yl,
                    s.alive() ? "today" : (std::to_string(double(s.extinctTick) / yl).substr(0, 5)).c_str());
        ImGui::Text("Peak %d · %s · size %.1f", s.peakPopulation, dietLabel(s.meanGenes[G_DIET]),
                    lerp(5.f, 20.f, s.meanGenes[G_SIZE]));
        ImGui::PopStyleColor();
        ImGui::EndTooltip();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x == 0.f)
            app.rs->highlightSpecies = app.rs->highlightSpecies == uint32_t(hoverId) ? 0 : uint32_t(hoverId);
    }
    if (order.empty()) dl->AddText(origin + ImVec2(10, 30), ImGui::GetColorU32(C_MUTED), "No species reached the minimum peak yet.");
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void Ui::drawEvents(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_HISTORY " Events", &showEvents_)) {
        ImGui::End();
        return;
    }
    if (!ImGui::BeginTabBar("##evtabs")) {
        ImGui::End();
        return;
    }
    if (tabItem(ICON_EYE " Discoveries")) {
        // Behaviours nobody scripted, noticed by the detector in Discoveries.cpp. Newest first.
        int count = 0;
        for (const WorldEvent& e : w.events.items) count += e.kind == EventKind::Discovery;
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("%d found · click one to fly there and highlight the species", count);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_SAVE " Export journal")) {
            char name[64];
            std::snprintf(name, sizeof name, "discoveries-year%.1f.md", w.year());
            const std::string path = app.saveDir + name;
            std::ofstream f(path);
            f << "# Primordia discovery journal\n\nSeed " << w.settings.seed << ", year " << w.year() << ", "
              << w.creatures.size() << " creatures, " << w.species.aliveCount() << " species alive.\n\n";
            for (const WorldEvent& e : w.events.items)
                if (e.kind == EventKind::Discovery) {
                    char yr[32];
                    std::snprintf(yr, sizeof yr, "%.2f", double(e.tick) / w.params.yearLength);
                    f << "- **Year " << yr << "** " << e.text << "\n";
                }
            f << "\n## What each living species does best\n\n";
            for (const Species& sp : w.species.all())
                if (sp.alive() && sp.population >= 15)
                    f << "- **" << sp.name() << "** (" << sp.population << " alive): " << w.bestAtText(sp.id) << "\n";
            app.notify(f ? "Journal saved to " + path : "Could not write " + path);
        }
        ImGui::BeginChild("##disclist");
        if (count == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextWrapped("Nothing yet. Let it run: alarm calls, farming, learning and fast evolution show up here "
                               "once the detector has seen them hold for a while. Fast-forward helps.");
            ImGui::PopStyleColor();
        }
        const auto& items = w.events.items;
        for (int i = int(items.size()) - 1; i >= 0; --i) {
            const WorldEvent& e = items[size_t(i)];
            if (e.kind != EventKind::Discovery) continue;
            ImGui::PushID(i);
            ImGui::PushFont(fontMono_, 0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::Text("%7.2fy", double(e.tick) / w.params.yearLength);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, C_GREEN);
            ImGui::TextUnformatted(ICON_EYE);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (e.speciesId && w.species.valid(e.speciesId)) {
                speciesBadge(app, e.speciesId);
                ImGui::SameLine();
            }
            if (ImGui::Selectable(e.text.c_str(), false)) {
                if (e.pos.x != 0.f || e.pos.y != 0.f) {
                    app.cam->targetCenter = e.pos;
                    app.follow = false;
                }
                if (e.speciesId) app.rs->highlightSpecies = e.speciesId;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (tabItem(ICON_TROPHY " Hall of fame")) {
        if (ImGui::BeginTable("##records", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Record", 0, 1.2f);
            ImGui::TableSetupColumn("Holder", 0, 2.f);
            ImGui::TableSetupColumn("Value", 0, 1.f);
            ImGui::TableHeadersRow();
            w.records.forEach([&](const char* name, const char* unit, Record& r) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
                ImGui::Text(ICON_TROPHY " %s", name);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (r.holder.empty()) {
                    ImGui::TextDisabled("not yet set");
                } else {
                    if (w.species.valid(r.speciesId)) {
                        speciesBadge(app, r.speciesId);
                        ImGui::SameLine();
                    }
                    ImGui::PushID(name);
                    if (ImGui::Selectable(r.holder.c_str())) app.rs->highlightSpecies = r.speciesId;
                    ImGui::PopID();
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.1f %s", r.value, unit);
            });
            ImGui::EndTable();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::TextWrapped("Creature records are set when the holder dies, species records at each census.");
        ImGui::PopStyleColor();
        ImGui::EndTabItem();
    }
    if (tabItem(ICON_STAR " Achievements")) {
        int got = 0;
        for (int i = 0; i < achievementCount(); ++i) got += (w.achieved >> i) & 1;
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("%d of %d unlocked. They come from evolution itself; the tools can nudge the world toward them.", got,
                    achievementCount());
        ImGui::PopStyleColor();
        if (ImGui::BeginTable("##ach", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.f);
            ImGui::TableSetupColumn("Achievement", 0, 1.f);
            ImGui::TableSetupColumn("How", 0, 2.4f);
            for (int i = 0; i < achievementCount(); ++i) {
                bool on = (w.achieved >> i) & 1;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, on ? C_AMBER : C_MUTED);
                ImGui::TextUnformatted(on ? ICON_STAR : ICON_TIMES);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(achievementInfo(i).name);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
                if (on) ImGui::Text("%s  ·  year %.1f", achievementInfo(i).description, double(w.achievedTick[i]) / w.params.yearLength);
                else ImGui::TextUnformatted(achievementInfo(i).description);
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        ImGui::EndTabItem();
    }
    if (!tabItem(ICON_LIST " Timeline")) {
        ImGui::EndTabBar();
        ImGui::End();
        return;
    }
    static const char* filters[] = {"Everything", "New species", "Extinctions", "Milestones", "Catastrophes", "Discoveries"};
    ImGui::SetNextItemWidth(150.f);
    ImGui::Combo("##evfilter", &eventFilter_, filters, 6);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%zu events · click one to jump there", w.events.items.size());
    ImGui::PopStyleColor();
    ImGui::BeginChild("##evlist");
    const auto& items = w.events.items;
    for (int i = int(items.size()) - 1; i >= 0; --i) {
        const WorldEvent& e = items[size_t(i)];
        if (eventFilter_ == 1 && e.kind != EventKind::Speciation) continue;
        if (eventFilter_ == 2 && e.kind != EventKind::Extinction) continue;
        if (eventFilter_ == 3 && e.kind != EventKind::Milestone) continue;
        if (eventFilter_ == 4 && e.kind != EventKind::Catastrophe) continue;
        if (eventFilter_ == 5 && e.kind != EventKind::Discovery) continue;
        ImGui::PushID(i);
        ImGui::PushFont(fontMono_, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("%7.2fy", double(e.tick) / w.params.yearLength);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, eventColor(e.kind));
        ImGui::TextUnformatted(eventIcon(e.kind));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (e.speciesId && w.species.valid(e.speciesId)) {
            speciesBadge(app, e.speciesId);
            ImGui::SameLine();
        }
        if (ImGui::Selectable(e.text.c_str(), false)) {
            if (e.pos.x != 0.f || e.pos.y != 0.f) {
                app.cam->targetCenter = e.pos;
                app.follow = false;
            }
            if (e.speciesId) app.rs->highlightSpecies = e.speciesId;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
    ImGui::EndTabBar();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Environment controls
// ---------------------------------------------------------------------------
void Ui::drawEnvironment(AppState& app) {
    World& w = *app.world;
    SimParams& p = w.params;
    if (!ImGui::Begin(ICON_GLOBE " Environment", &showEnvironment_)) {
        ImGui::End();
        return;
    }
    auto help = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
    };
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.f);
    if (ImGui::CollapsingHeader(ICON_SUN "  Climate", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Climate shift", &p.climateOffset, -1.f, 1.f, "%+.2f");
        help("Shifts every temperature on the planet. Negative = ice age, positive = hothouse.");
        ImGui::SliderFloat("Seasons", &p.seasonStrength, 0.f, 2.f, "%.2f");
        help("Strength of summer/winter swings. Seasons are strongest near the poles and opposite in each hemisphere.");
        ImGui::SliderInt("Year length", &p.yearLength, 2000, 60000, "%d ticks");
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("Current offset %+.2f (%+.1f°C)", w.climate(), w.climate() * 26.f);
        ImGui::PopStyleColor();
        ImGui::SliderFloat("Glacial cycle", &p.glacialCycleYears, 0.f, 60.f, p.glacialCycleYears > 0 ? "%.0f years" : "off");
        help("Slow ice-age / warm-period oscillation of the whole planet's climate");
        if (p.glacialCycleYears > 0) ImGui::SliderFloat("Cycle strength", &p.glacialAmplitude, 0.f, 1.f, "%.2f");
        ImGui::Checkbox("Sea level follows climate", &p.dynamicSeaLevel);
        ImGui::Checkbox("Food contests (hawk-dove)", &p.foodContests);
        help("Two creatures grazing the same patch at once: doves share, hawks bully doves, two hawks fight and both get hurt.");
        ImGui::Checkbox("Camouflage", &p.camouflage);
        help("Creatures whose colour matches the ground beneath them (green grass, tan earth) are seen from much closer, especially when still.");
        ImGui::Checkbox("Weather", &p.weather);
        help("Rain fronts drift across the map on the wind. Plants grow faster under rain, and rain quenches thirst.");
        if (p.weather) ImGui::SliderFloat("Wind", &p.windSpeed, 0.f, 4.f, "%.2fx");
        help("Cold periods lock water in ice: seas retreat and land bridges appear between islands");
        ImGui::Checkbox("Day and night", &p.dayNight);
        help("At night sight shrinks unless a creature has night vision. Glowing (signalling) creatures stay visible.");
        if (p.dayNight) ImGui::SliderInt("Day length", &p.dayLength, 500, 20000, "%d ticks");
        ImGui::SliderFloat("Night darkness", &app.rs->nightDarkness, 0.f, 1.f, "%.2f");
        help("Visual only: how dark the screen gets at night");
    }
    if (ImGui::CollapsingHeader(ICON_LEAF "  Plants", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Growth rate", &p.plantGrowth, 0.f, 4.f, "%.2fx");
        ImGui::SliderFloat("Capacity", &p.plantCapacity, 1.f, 30.f, "%.1f");
        help("Maximum biomass per fertile map cell");
        ImGui::SliderFloat("Food value", &p.plantEnergy, 0.2f, 3.f, "%.2fx");
    }
    if (ImGui::CollapsingHeader(ICON_HEART "  Metabolism")) {
        ImGui::SliderFloat("Overall cost", &p.metabolism, 0.2f, 3.f, "%.2fx");
        ImGui::SliderFloat("Movement", &p.moveCost, 0.f, 3.f, "%.2fx");
        ImGui::SliderFloat("Brains", &p.brainCost, 0.f, 5.f, "%.2fx");
        help("Energy per synapse. Raise it to favour simple minds.");
        ImGui::SliderFloat("Eyes", &p.visionCost, 0.f, 5.f, "%.2fx");
        ImGui::SliderFloat("Cold & heat", &p.thermalCost, 0.f, 4.f, "%.2fx");
    }
    if (ImGui::CollapsingHeader(ICON_FLASK "  Evolution", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            // One knob for "show me evolution sooner": shorter lives (faster generations) and more mutations.
            static const char* names[] = {"Normal", "Fast", "Faster", "Frantic"};
            static const float pace[] = {1.f, 2.f, 3.f, 5.f};
            ImGui::TextUnformatted("Evolution pace");
            for (int k = 0; k < 4; ++k) {
                ImGui::SameLine();
                if (ImGui::RadioButton(names[k], p.lifePace == pace[k] && p.mutationRate == pace[k])) p.lifePace = p.mutationRate = pace[k];
            }
            help("Sets ageing and mutation together. Faster paces give shorter lives, quicker generations and more "
                 "mutations, so new traits show up sooner. Seasons and days keep their length.");
        }
        ImGui::SliderFloat("Ageing", &p.lifePace, 0.5f, 5.f, "%.2fx");
        help("How fast creatures grow up and grow old");
        ImGui::SliderFloat("Mutation", &p.mutationRate, 0.f, 5.f, "%.2fx");
        help("Multiplies every creature's own (evolvable) mutation rate");
        ImGui::SliderFloat("Species gap", &p.speciesThreshold, 0.2f, 1.5f, "%.2f");
        help("Genetic distance at which a lineage counts as a new species");
        ImGui::SliderFloat("Diet trade-off", &p.specialization, 0.5f, 3.f, "%.2f");
        help("Above 1, generalists digest both plants and meat badly. Higher values push lineages to specialise.");
        ImGui::SliderFloat("Generalist tax", &p.generalistTax, 0.f, 3.f, "%.2f");
        help("Upkeep for investing in several costly traits at once (wings, quills, armour, poison, organs...). Above 0, a body pays for everything except its strongest trait, so species are forced to pick one trade.");
        ImGui::Checkbox("Sexual reproduction", &p.sexualReproduction);
        ImGui::Checkbox("Hybridization", &p.hybridization);
        help("Close sister species occasionally interbreed when they meet, creating hybrid zones");
        help("Allow creatures to recombine genomes with nearby members of their own species");
    }
    if (ImGui::CollapsingHeader(ICON_USERS "  Population")) {
        ImGui::SliderInt("Max population", &p.maxPopulation, 500, 30000);
        ImGui::SliderFloat("Random shocks per year", &p.shocksPerYear, 0.f, 4.f, p.shocksPerYear > 0 ? "%.1f" : "off");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Meteors, droughts, volcanoes, blooms, cold snaps, heat waves and plagues strike at random. They reset the playing field, so new niches and radiations open up.");
        ImGui::SliderInt("Brain tick rate", &p.thinkEvery, 1, 4, p.thinkEvery == 1 ? "every tick" : "every %d ticks");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Each brain thinks every Nth tick (staggered; its outputs persist in between). Higher = much faster simulation, slightly coarser reactions.");
        ImGui::Checkbox("Reseed if life dies out", &p.autoReseed);
        ImGui::Checkbox("Predator immigration", &p.predatorImmigration);
        ImGui::Checkbox("Creatures need water", &p.thirst);
        help("Creatures dehydrate over time, faster in heat. Drinking at shores or eating lush grass refills them.");
        help("If hunters die out, a new hunting lineage wanders in half a year later");
        ImGui::SliderInt("Reseed below", &p.reseedBelow, 0, 500);
        ImGui::SliderFloat("Carcass decay", &p.meatDecay, 0.0002f, 0.01f, "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Scent persistence", &p.pheromoneDecay, 0.9f, 0.999f, "%.3f");
        ImGui::SliderFloat("Autosave every", &app.autosaveMinutes, 0.f, 60.f, app.autosaveMinutes > 0 ? "%.0f min" : "off");
    }
    if (ImGui::CollapsingHeader(ICON_BUG "  Disease")) {
        ImGui::Checkbox("Natural outbreaks", &p.epidemics);
        ImGui::SliderFloat("Outbreaks / year", &p.outbreaksPerYear, 0.f, 3.f, "%.2f");
        ImGui::SliderFloat("Contagiousness", &p.transmission, 0.f, 0.3f, "%.3f");
        help("Chance per tick that a touching neighbour catches the disease");
        ImGui::SliderFloat("Virulence", &p.virulence, 0.f, 5.f, "%.2fx");
        ImGui::SliderInt("Illness length", &p.diseaseDuration, 200, 8000, "%d ticks");
        ImGui::SliderInt("Immunity length", &p.immunityDuration, 0, 40000, "%d ticks");
        ImGui::SliderFloat("Cross-species", &p.crossSpecies, 0.f, 1.f, "%.2f");
        help("How easily the pathogen jumps between species");
        if (ImGui::Button(ICON_BUG " Start an epidemic now")) app.world->plague(app.rs->highlightSpecies, 0.15f);
    }
    ImGui::PopItemWidth();
    ImGui::Spacing();
    if (ImGui::Button(ICON_REFRESH " Reset to defaults")) p = SimParams{};
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------
void Ui::drawTools(AppState& app) {
    World& w = *app.world;
    RenderSettings& rs = *app.rs;
    if (!ImGui::Begin(ICON_MAGIC " Tools", &showTools_)) {
        ImGui::End();
        return;
    }
    static const char* icons[] = {ICON_POINTER, ICON_LEAF, ICON_PLUS_SQ, ICON_COPY, ICON_TIMES, ICON_FIRE, ICON_UP, ICON_DOWN, ICON_BUG};
    static const char* tips[] = {"Select creatures, drag to pan",
                                 "Paint fresh plants",
                                 "Introduce a brand-new lineage",
                                 "Plant clones of the copied genome",
                                 "Remove creatures under the brush",
                                 "Meteor impact: wipes out an area",
                                 "Raise land (create bridges and mountains)",
                                 "Lower land (carve seas and channels)",
                                 "Release a contagious pathogen"};
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.f;
    for (int t = 0; t < int(Tool::Count); ++t) {
        if (t % 3) ImGui::SameLine();
        static const char* shortNames[] = {"Select", "Feed", "Spawn", "Clone", "Smite", "Meteor", "Raise", "Lower", "Infect"};
        char label[64];
        std::snprintf(label, sizeof label, "%s %s", icons[t], shortNames[t]);
        // Icons only when the longest label ("Meteor") would not fit; measured, so it follows the interface scale.
        const bool fits = ImGui::CalcTextSize(ICON_FIRE " Meteor").x + ImGui::GetStyle().FramePadding.x * 2 <= bw;
        // Too narrow for one line: stack the icon over the name instead of dropping the name.
        if (fits) std::snprintf(label, sizeof label, "%s %s##tool%d", icons[t], shortNames[t], t);
        else std::snprintf(label, sizeof label, "  %s\n%s##tool%d", icons[t], shortNames[t], t);
        if (iconButton(label, tips[t], app.tool == Tool(t), ImVec2(bw, ImGui::GetFrameHeight() * (fits ? 1.4f : 2.1f))))
            app.tool = Tool(t);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
    ImGui::Text("%s", toolName(app.tool));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("%s", tips[int(app.tool)]);
    ImGui::PopStyleColor();
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 5.5f);
    if (app.tool != Tool::Select) ImGui::SliderFloat("Brush", &app.brushRadius, 20.f, 800.f, "%.0f");
    if (app.tool == Tool::Spawn || app.tool == Tool::Clone) ImGui::SliderInt("Count", &app.spawnCount, 1, 200);
    if (app.tool == Tool::Spawn) ImGui::Checkbox("Spawn hunters", &app.spawnPredators);
    if (app.tool == Tool::Clone && !app.hasClipboard) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_RED);
        ImGui::TextWrapped("No genome copied yet. Select a creature and press Copy genome.");
        ImGui::PopStyleColor();
    }
    ImGui::PopItemWidth();

    ImGui::SeparatorText("View");
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 5.5f);
    if (ImGui::BeginCombo("Colour", colorModeName(rs.colorMode))) {
        for (int i = 0; i < int(ColorMode::Count); ++i)
            if (ImGui::Selectable(colorModeName(ColorMode(i)), int(rs.colorMode) == i)) rs.colorMode = ColorMode(i);
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Overlay", overlayName(rs.overlay))) {
        for (int i = 0; i < int(Overlay::Count); ++i)
            if (ImGui::Selectable(overlayName(Overlay(i)), int(rs.overlay) == i)) rs.overlay = Overlay(i);
        ImGui::EndCombo();
    }
    ImGui::SliderFloat("Creature size", &rs.creatureScale, 0.5f, 3.f, "%.1fx");
    ImGui::PopItemWidth();
    ImGui::Checkbox("Vision", &rs.showVision);
    ImGui::SameLine();
    ImGui::Checkbox("Trail", &rs.showTrails);
    ImGui::SameLine();
    ImGui::Checkbox("Bloom", &rs.bloom);
    ImGui::Checkbox("Signals", &rs.showSignals);
    ImGui::SameLine();
    ImGui::Checkbox("Scent", &rs.showPheromones);
    ImGui::SameLine();
    ImGui::Checkbox("Particles", &rs.particles);
    ImGui::Checkbox("Blob style", &rs.blobStyle);
    ImGui::SameLine();
    ImGui::Checkbox("Day-night", &w.params.dayNight);

    ImGui::SeparatorText("Acts of god");
    float hw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(ICON_SNOW " Ice age", ImVec2(hw, 0))) w.climateShock(-0.75f, "An ice age begins");
    ImGui::SameLine();
    if (ImGui::Button(ICON_SUN " Heat wave", ImVec2(hw, 0))) w.climateShock(0.6f, "A heat wave scorches the world");
    if (ImGui::Button(ICON_BUG " Epidemic", ImVec2(hw, 0))) w.plague(rs.highlightSpecies, 0.15f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Infects the highlighted species, or the most populous one. Watch it spread in Population > Epidemic.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FIRE " Meteor", ImVec2(hw, 0))) {
        Rng r(uint64_t(w.tickCount) * 31 + 7);
        Vec2 at{r.range(0.15f, 0.85f) * w.terrain.width, r.range(0.15f, 0.85f) * w.terrain.height};
        w.meteor(at, 320.f);
        app.cam->targetCenter = at;
    }
    if (ImGui::Button(ICON_LEAF " Bloom", ImVec2(hw, 0))) w.bloom();
    ImGui::SameLine();
    if (ImGui::Button(ICON_TINT " Drought", ImVec2(hw, 0))) w.drought();
    if (ImGui::Button(ICON_FIRE " Volcano", ImVec2(hw, 0))) {
        Rng r(uint64_t(w.tickCount) * 977 + 5);
        Vec2 at{r.range(0.15f, 0.85f) * w.terrain.width, r.range(0.15f, 0.85f) * w.terrain.height};
        w.volcano(at, 260.f);
        app.cam->targetCenter = at;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_PAW " Hunters arrive", ImVec2(hw, 0))) {
        Rng r(uint64_t(w.tickCount) * 613 + 9);
        Vec2 at{r.range(0.2f, 0.8f) * w.terrain.width, r.range(0.2f, 0.8f) * w.terrain.height};
        w.spawnPredators(at, 30, 350.f);
        app.notify("30 hunters released");
    }
    ImGui::End();
}

void Ui::drawMinimap(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_MAP " Minimap", &showMinimap_, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float aspect = w.terrain.width / w.terrain.height;
    float size = std::min(avail.x, avail.y * aspect);
    ImVec2 sz(size, size / aspect);
    ImVec2 p0 = ImGui::GetCursorScreenPos() + ImVec2((avail.x - sz.x) * 0.5f, 0);
    ImGui::SetCursorScreenPos(p0);
    ImGui::Image(ImTextureRef(ImTextureID(intptr_t(app.renderer->minimapTexture()))), sz, ImVec2(0, 1), ImVec2(1, 0));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sx = sz.x / w.terrain.width, sy = sz.y / w.terrain.height;
    size_t step = std::max<size_t>(1, w.creatures.size() / 3000);
    for (size_t i = 0; i < w.creatures.size(); i += step) {
        const Creature& c = w.creatures[i];
        Color col = app.renderer->creatureColor(w, c, app.rs->colorMode);
        if (app.rs->highlightSpecies && c.speciesId != app.rs->highlightSpecies) col.a = 0.2f;
        dl->AddRectFilled(p0 + ImVec2(c.pos.x * sx - 1, c.pos.y * sy - 1), p0 + ImVec2(c.pos.x * sx + 1, c.pos.y * sy + 1),
                          u32(col));
    }
    const Camera& cam = *app.cam;
    Vec2 tl = cam.screenToWorld({0, 0}), br = cam.screenToWorld(cam.fbSize);
    dl->PushClipRect(p0, p0 + sz, true);
    dl->AddRect(p0 + ImVec2(tl.x * sx, tl.y * sy), p0 + ImVec2(br.x * sx, br.y * sy), IM_COL32(255, 255, 255, 200), 2.f, 0, 1.5f);
    dl->PopClipRect();
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.f) {
        app.cam->targetZoom = clampf(app.cam->targetZoom * std::pow(1.2f, ImGui::GetIO().MouseWheel), 0.02f, 12.f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 m = ImGui::GetIO().MousePos - p0;
        app.cam->targetCenter = {m.x / sx, m.y / sy};
        app.cam->center = app.cam->targetCenter;
        app.follow = false;
    }
    ImGui::End();
}

void Ui::drawPerformance(AppState& app) {
    World& w = *app.world;
    if (!ImGui::Begin(ICON_GAUGE " Performance", &showPerformance_)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%.0f FPS (%.2f ms)   ·   %.0f ticks/s   ·   simulation %.2f ms/frame", app.fps, app.frameMs, app.tps,
                app.simMs);
    ImGui::Text("%zu creatures · %zu carcasses · %d threads", w.creatures.size(), w.corpses.size(), int(app.pool->size()));
    static int wantThreads = 0;
    if (wantThreads == 0) wantThreads = int(app.pool->size());
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderInt("Simulation threads", &wantThreads, 1, int(std::max(1u, std::thread::hardware_concurrency())));
    ImGui::SameLine();
    if (ImGui::Button("Apply") && wantThreads != int(app.pool->size())) app.requestThreads = wantThreads;
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("GPU: %s", app.renderer->glRenderer());
    ImGui::PopStyleColor();
    const TickTimings& t = w.timings;
    const char* labels[] = {"Spatial grid", "Senses + brains", "Interactions", "Movement", "Environment", "Bookkeeping"};
    double vals[] = {t.grid, t.sense, t.interact, t.move, t.env, t.book};
    ImGui::SeparatorText("Per-tick cost");
    for (int i = 0; i < 6; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s  %.3f ms", labels[i], vals[i]);
        bar(float(vals[i] / std::max(1e-6, t.total)), ImGui::GetColorU32(C_ACCENT_DIM), buf);
    }
    ImGui::End();
}

void Ui::drawNewWorldModal(AppState& app) {
    if (openNewWorld) {
        ImGui::OpenPopup("New world");
        openNewWorld = false;
        app.newWorld = app.world->settings;
    }
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New world", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    WorldSettings& s = app.newWorld;
    ImGui::PushItemWidth(260.f);
    int seed = int(s.seed % 100000000);
    if (ImGui::InputInt("Seed", &seed)) s.seed = uint64_t(std::max(0, seed));
    ImGui::SameLine();
    if (ImGui::Button(ICON_RANDOM)) s.seed = std::random_device{}() % 100000000;
    static const char* presets[] = {"Continents", "Archipelago", "Pangaea", "Lakes", "Flatland"};
    int preset = int(s.preset);
    if (ImGui::Combo("Terrain", &preset, presets, 5)) s.preset = TerrainPreset(preset);
    static const float sizes[] = {2048, 3072, 4096, 6144, 8192};
    static const char* sizeNames[] = {"Small (2048)", "Medium (3072)", "Large (4096)", "Huge (6144)", "Vast (8192)"};
    int si = 2;
    for (int i = 0; i < 5; ++i)
        if (std::fabs(sizes[i] - s.width) < 1) si = i;
    if (ImGui::Combo("Size", &si, sizeNames, 5)) s.width = s.height = sizes[si];
    ImGui::SliderFloat("Water", &s.waterFraction, 0.f, 0.75f, "%.2f");
    ImGui::SliderFloat("Landmass scale", &s.featureScale, 0.4f, 2.5f, "%.2f");
    ImGui::SliderFloat("Moisture", &s.moisture, -0.4f, 0.4f, "%+.2f");
    ImGui::SliderFloat("Temperature", &s.temperature, -0.6f, 0.6f, "%+.2f");
    ImGui::SeparatorText("Life");
    ImGui::SliderInt("Creatures", &s.initialPopulation, 50, 5000);
    ImGui::SliderInt("Grazer lineages", &s.foundingLineages, 1, 12);
    ImGui::SliderInt("Predator lineages", &s.predatorLineages, 0, 4);
    if (s.predatorLineages > 0) ImGui::SliderFloat("Predator share", &s.predatorShare, 0.01f, 0.4f, "%.2f");
    ImGui::SliderFloat("Grazer diet spread", &s.initialDietMax, 0.f, 1.f, "%.2f");
    ImGui::PopItemWidth();
    ImGui::Spacing();
    if (ImGui::Button(ICON_GLOBE " Create world", ImVec2(160, 0))) {
        app.requestNewWorld = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Ui::drawHelp(AppState&) {
    ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_KEYBOARD " Controls", &showHelp_)) {
        ImGui::End();
        return;
    }
    const char* rows[][2] = {{"Space", "Pause / resume"},
                             {".", "Step one tick"},
                             {"1 - 9, 0", "Simulation speed (0 = maximum)"},
                             {"Mouse wheel", "Zoom at cursor"},
                             {"Right / middle drag", "Pan (left drag on empty ground too)"},
                             {"W A S D / arrows", "Pan"},
                             {"Left click", "Select a creature / use tool"},
                             {"Double-click", "Select and follow a creature"},
                             {"Right click", "Creature menu (follow, copy, clone, lab...)"},
                             {"F", "Follow the selected creature"},
                             {"Esc", "Back to select tool, then deselect"},
                             {"Delete", "Remove the selected creature"},
                             {"Ctrl+C", "Copy the selected genome"},
                             {"C", "Cycle colour mode"},
                             {"[ / ]", "Highlight previous / next living species"},
                             {"O", "Cycle map overlay"},
                             {"V", "Toggle vision cone"},
                             {"G", "Toggle bloom"},
                             {"H / Home", "Fit the world in view"},
                             {"Tab", "Hide / show the interface"},
                             {"K", "Cinematic tour of interesting creatures"},
                             {"B", "Primer-style blob art"},
                             {"F11", "Record a time-lapse"},
                             {"M", "Sound on / off"},
                             {"Ctrl+N", "New world"},
                             {"Ctrl+S / Ctrl+O", "Save / open"},
                             {"F12", "Screenshot to Documents/Primordia"}};
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg)) {
        for (auto& r : rows) {
            ImGui::TableNextColumn();
            ImGui::PushFont(fontMono_, 0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
            ImGui::TextUnformatted(r[0]);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Ui::drawEncyclopedia(AppState&) {
    ImGui::SetNextWindowSize(ImVec2(620, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_INFO " How creatures work", &showEncyclopedia_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped(
        "Every creature carries a genome of %d body genes plus the weights of a recurrent neural network. "
        "Nothing about behaviour is scripted: grazing, hunting, fleeing, herding, warning colours and trail-following "
        "all have to evolve through mutation and natural selection.",
        int(G_COUNT));
    ImGui::SeparatorText("Senses");
    ImGui::TextWrapped(
        "Five eye sectors spread over the field of view. Each eye reports plant density, the nearest carcass, the "
        "nearest creature, its relative size, how similar its colour is (a kin cue), and obstacles such as water. "
        "Creatures also feel energy, health, age, speed, temperature, pain and touch, hear the signals of neighbours, "
        "smell pheromone trails, and keep a clock neuron and recurrent memory.");
    ImGui::SeparatorText("Actions");
    ImGui::TextWrapped(
        "Thrust, turn, eat, attack, reproduce, emit a signal (visible as a glow), share energy with a touching "
        "neighbour, and lay pheromone.");
    ImGui::SeparatorText("Rules of life");
    ImGui::BulletText("Metabolism follows Kleiber's law: cost grows with mass^0.75.");
    ImGui::BulletText("Bigger attackers deal more damage to smaller targets (size ratio rule).");
    ImGui::BulletText("Toxic creatures poison whoever bites them or eats their carcass.");
    ImGui::BulletText("Pack hunting: each extra attacker on the same target adds damage.");
    ImGui::BulletText("Safety in numbers: prey in a crowd take less damage.");
    ImGui::BulletText("Hawk-dove food contests: when two graze the same patch, aggression decides who eats.");
    ImGui::BulletText("Camouflage: colours that match the ground are spotted from closer, especially when still.");
    ImGui::BulletText("Creatures choose mates by colour, and can evolve choosiness.");
    ImGui::BulletText("Creatures need water: shores, or the moisture in lush grass.");
    ImGui::BulletText("Rain fronts drift on the wind; plants grow faster under rain, and rain quenches thirst.");
    ImGui::BulletText("Glacial cycles (optional) swing the climate; cold periods lower the sea and expose land bridges.");
    ImGui::BulletText("Close sister species occasionally hybridize, so species boundaries can blur.");
    ImGui::BulletText("At night sight shrinks unless a creature has night vision; glowing creatures stay visible.");
    ImGui::BulletText("Diet is a trade-off: generalists digest both foods poorly.");
    ImGui::BulletText("Temperature away from a creature's thermal optimum costs energy.");
    ImGui::BulletText("Seasons are opposite in the two hemispheres; poles freeze in winter.");
    ImGui::BulletText("A lineage becomes a new species once its genome drifts far enough.");
    ImGui::SeparatorText("Genes");
    for (int g = 0; g < G_COUNT; ++g) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
        ImGui::TextUnformatted(GENE_INFO[g].name);
        ImGui::PopStyleColor();
        ImGui::SameLine(160.f);
        ImGui::TextWrapped("%s", GENE_INFO[g].description);
    }
    ImGui::End();
}

// A results card: the state of the world in a dozen lines, ready to copy and share.
static std::string worldReport(const World& w, int scenarioIdx, int challengesDone) {
    char b[320];
    std::string out;
    auto line = [&](const char* fmt, auto... a) { std::snprintf(b, sizeof b, fmt, a...); out += b; out += '\n'; };
    const double n = double(std::max<size_t>(1, w.creatures.size()));
    int hunters = 0, fliers = 0, quilled = 0, sunfed = 0, marine = 0;
    for (const Creature& c : w.creatures) {
        hunters += c.ph.diet > 0.66f;
        fliers += c.genome.g[G_WINGS] > 0.5f;
        quilled += c.genome.g[G_SPIKES] > 0.5f;
        sunfed += c.genome.g[G_PHOTO] > 0.5f;
        marine += c.genome.g[G_AQUATIC] > 0.7f;
    }
    int discoveries = 0;
    const WorldEvent* last = nullptr;
    for (const WorldEvent& e : w.events.items)
        if (e.kind == EventKind::Discovery) { ++discoveries; last = &e; }
    const bool sc = scenarioIdx >= 0 && scenarioIdx < scenarioCount();
    line("PRIMORDIA  ·  %s  ·  seed %llu", sc ? scenario(scenarioIdx).name : "Custom world", (unsigned long long)w.settings.seed);
    line("Year %.1f: %zu creatures in %d living species (%zu ever)", w.year(), w.creatures.size(), w.species.aliveCount(),
         w.species.all().size());
    line("Hunters %.0f%%  ·  fliers %.0f%%  ·  quilled %.0f%%  ·  sun-fed %.0f%%  ·  sea animals %.0f%%", 100 * hunters / n,
         100 * fliers / n, 100 * quilled / n, 100 * sunfed / n, 100 * marine / n);
    int rank = 0;
    for (const LineageScore& ls : w.lineageScores()) {
        if (!ls.population || rank >= 3) break;
        line("%d. %s%s: %d alive (%.0f%%), %d living species", ++rank, w.species.get(ls.root).name().c_str(),
             ls.designed ? " [designed]" : "", ls.population, 100 * ls.population / n, ls.livingSpecies);
    }
    line("Discoveries: %d%s%s", discoveries, last ? ". Latest: " : "", last ? last->text.c_str() : "");
    if (sc) {
        const float p = objective(scenarioIdx).progress(w);
        line("Goal: %s", objective(scenarioIdx).goal);
        line("Result: %s", p >= 1.f || ((challengesDone >> scenarioIdx) & 1) ? "achieved" : p < 0.f ? "failed" : "in progress");
    }
    return out;
}

void Ui::drawReport(AppState& app) {
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_TROPHY " World report", &showReport_)) {
        const std::string text = worldReport(*app.world, app.currentScenario, app.challengesDone);
        ImGui::TextWrapped("%s", text.c_str());
        if (ImGui::Button(ICON_COPY " Copy")) {
            SDL_SetClipboardText(text.c_str());
            app.notify("World report copied to the clipboard");
        }
    }
    ImGui::End();
}

void Ui::drawGenomeLab(AppState& app) {
    World& w = *app.world;
    ImGui::SetNextWindowSize(ImVec2(560, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FLASK " Genome Lab", &showLab_)) {
        ImGui::End();
        return;
    }
    Rng rng(uint64_t(time_ * 1000.f) + 17);
    if (!labReady_) {
        lab_ = Genome::random(rng, 0.2f);
        labReady_ = true;
    }
    Creature c = w.preview(lab_);
    c.animPhase = time_ * 4.f;
    unsigned tex = app.renderer->portrait(1, w, c, *app.rs, time_);
    ImGui::Image(ImTextureRef(ImTextureID(intptr_t(tex))), ImVec2(128, 128), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(fontTitle_, fontTitle_->LegacySize);
    ImGui::TextUnformatted("Design a creature");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    const Phenotype& p = c.ph;
    ImGui::Text("%s · radius %.1f · %d offspring per birth", dietLabel(p.diet), p.adultRadius, p.litter);
    ImGui::Text("Vision %.0f over %.0f° · lives %.1f years", p.visionRange, p.fov * 180.f / PI,
                p.lifespan / w.params.yearLength);
    ImGui::Text("Plants %.0f%% · meat %.0f%% · %d synapses", p.plantEff * 100.f, p.meatEff * 100.f, p.connections);
    ImGui::PopStyleColor();
    if (ImGui::Button(ICON_SEARCH " From selected")) {
        if (int i = w.indexOfId(app.selectedId); i >= 0) lab_ = w.creatures[i].genome;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_LEAF " Grazer")) lab_ = Genome::random(rng, 0.15f);
    ImGui::SameLine();
    if (ImGui::Button(ICON_PAW " Hunter")) lab_ = Genome::predator(rng);
    ImGui::SameLine();
    if (ImGui::Button(ICON_RANDOM " Mutate")) {
        for (int k = 0; k < 5; ++k) mutate(lab_, rng, 2.f);
    }
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderInt("##count", &app.spawnCount, 1, 200, "%d creatures");
    ImGui::SameLine();
    if (ImGui::Button(ICON_PLUS_SQ " Release into the world (click the map)")) {
        app.clipboard = lab_;
        app.clipboardSpecies = 0;
        app.hasClipboard = true;
        app.tool = Tool::Clone;
        app.notify("Click on the map to release your design");
    }
    if (ImGui::Button(ICON_TROPHY " Enter tournament")) {
        const Vec2 at = w.releaseDesign(lab_, 40);
        app.cam->targetCenter = at;
        app.follow = false;
        app.notify("40 of your design released as " + w.species.all().back().name() + ". Ranking: Species > Lineages");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Every entry starts equal: 40 creatures on a random fertile patch.\nEnter several designs (paste a friend's genome, enter it too) and see which lineage wins.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_COPY " Copy as text")) {
        std::string t = genomeToText(lab_);
        SDL_SetClipboardText(t.c_str());
        app.notify("Genome copied to the clipboard as text");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Share a creature: paste the text anywhere, or into another Primordia");
    ImGui::SameLine();
    if (ImGui::Button(ICON_PASTE " Paste genome")) {
        if (char* t = SDL_GetClipboardText()) {
            Genome g;
            if (genomeFromText(t, g)) {
                lab_ = g;
                app.notify("Genome pasted into the lab");
            } else {
                app.notify("The clipboard does not hold a Primordia genome from this version");
            }
            SDL_free(t);
        }
    }
    ImGui::SeparatorText("Body genes");
    ImGui::BeginChild("##genes");
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 8.f);
    for (int g = 0; g < G_COUNT; ++g) {
        ImGui::PushID(g);
        if (g == G_HUE) {
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, v4(hsv(lab_.g[g], 0.7f, 0.95f)));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, v4(hsv(lab_.g[g], 0.7f, 1.f)));
        }
        ImGui::SliderFloat(GENE_INFO[g].name, &lab_.g[g], 0.f, 1.f, "%.2f");
        if (g == G_HUE) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", GENE_INFO[g].description);
        ImGui::PopID();
    }
    ImGui::PopItemWidth();
    ImGui::SeparatorText("Brain");
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("The Grazer and Hunter buttons install innate wiring. Behaviour beyond that has to evolve.");
    ImGui::PopStyleColor();
    ImGui::TextUnformatted("Add instincts:");
    for (int k = 0; k < int(Instinct::Count); ++k) {
        if (k % 3) ImGui::SameLine();
        ImGui::PushID(k);
        if (ImGui::Button(instinctName(Instinct(k)), ImVec2((ImGui::GetContentRegionAvail().x - 16.f) / float(3 - k % 3), 0)))
            addInstinct(lab_, Instinct(k));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", instinctDescription(Instinct(k)));
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Clear all synapses")) {
        lab_.w.fill(0.f);
        lab_.w[OFF_BO + OUT_EAT] = 1.f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep body, new grazer brain")) {
        auto body = lab_.g;
        lab_ = Genome::random(rng, 0.15f);
        lab_.g = body;
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep body, hunter brain")) {
        auto body = lab_.g;
        lab_ = Genome::predator(rng);
        lab_.g = body;
    }
    ImGui::EndChild();
    ImGui::End();
}

void Ui::drawPreferences(AppState& app) {
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_COG " Preferences", &showPrefs_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    RenderSettings& rs = *app.rs;
    ImGui::SeparatorText("Display");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat("Interface scale", &app.uiScale, 0.75f, 2.f, "%.2fx");
    if (ImGui::Checkbox("Fullscreen (F10)", &app.fullscreen)) app.applyDisplay = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("V-sync", &app.vsync)) app.applyDisplay = true;
    ImGui::SeparatorText("Sound");
    ImGui::Checkbox("Sound (M)", &app.sound);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("Volume", &app.soundVolume, 0.f, 1.f, "%.2f");
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("Wind follows the rain, crickets sing at night, and chimes mark new species.");
    ImGui::PopStyleColor();
    ImGui::SeparatorText("World view");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat("Bloom strength", &rs.bloomStrength, 0.f, 2.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat("Night darkness", &rs.nightDarkness, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat("Min. creature size", &rs.minCreaturePx, 0.f, 6.f, "%.1f px");
    ImGui::Checkbox("Vignette", &rs.vignette);
    ImGui::SameLine();
    ImGui::Checkbox("Blob style", &rs.blobStyle);
    ImGui::SameLine();
    ImGui::Checkbox("Carcasses", &rs.showCorpses);
    ImGui::SeparatorText("Saving");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderFloat("Autosave every", &app.autosaveMinutes, 0.f, 60.f, app.autosaveMinutes > 0 ? "%.0f min" : "off");
    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderInt("Time-lapse frame every", &app.recordEvery, 1, 60, "%d frames");
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("Files go to %s", app.saveDir.c_str());
    ImGui::PopStyleColor();
    ImGui::Checkbox("Dynasty mode (follow offspring when the followed creature dies)", &app.dynasty);
    ImGui::SeparatorText("Pause automatically when");
    ImGui::Checkbox("a new species appears", &app.pauseOnSpeciation);
    ImGui::Checkbox("an established species goes extinct", &app.pauseOnExtinction);
    ImGui::Checkbox("an achievement is unlocked", &app.pauseOnAchievement);
    ImGui::End();
}

void Ui::speciesBadge(AppState& app, uint32_t speciesId) {
    World& w = *app.world;
    if (!w.species.valid(speciesId)) return;
    unsigned tex = 0;
    float uv[4];
    const float isz = ImGui::GetTextLineHeight() * 1.25f;
    if (app.renderer->speciesIcon(w, speciesId, tex, uv)) {
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ImTextureID(intptr_t(tex))), cp + ImVec2(-2, -isz * 0.12f),
                                             cp + ImVec2(isz - 2, isz * 0.88f), ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
        ImGui::Dummy(ImVec2(isz - 4, ImGui::GetTextLineHeight()));
    } else {
        swatch(w.species.get(speciesId).color);
    }
}

void Ui::drawNameTag(AppState& app) {
    World& w = *app.world;
    int i = w.indexOfId(app.selectedId);
    if (i < 0) return;
    const Creature& c = w.creatures[i];
    ImGuiIO& io = ImGui::GetIO();
    const float dens = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.f;
    Vec2 sp = app.cam->worldToScreen(c.pos - Vec2(0.f, c.radius * app.rs->creatureScale * 2.2f + 6.f / app.cam->zoom));
    ImVec2 anchor(sp.x / dens, sp.y / dens);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    anchor += vp->Pos;
    // Only inside the free world area.
    if (anchor.x < vp->Pos.x + viewX || anchor.x > vp->Pos.x + viewX + viewW || anchor.y < vp->Pos.y + viewY + 20.f ||
        anchor.y > vp->Pos.y + viewY + viewH)
        return;
    std::string line1 = personalName(c.id);
    if (w.species.valid(c.speciesId)) line1 += "  " + w.species.get(c.speciesId).name();
    const char* line2 = activityOf(c);
    ImDrawList* dl = ImGui::GetBackgroundDrawList(vp);
    ImVec2 s1 = ImGui::CalcTextSize(line1.c_str()), s2 = ImGui::CalcTextSize(line2);
    float wbox = std::max(s1.x, s2.x) + 16.f, hbox = s1.y + s2.y + 10.f;
    ImVec2 p0(anchor.x - wbox * 0.5f, anchor.y - hbox);
    dl->AddRectFilled(p0, p0 + ImVec2(wbox, hbox), IM_COL32(8, 14, 18, 190), 6.f);
    dl->AddTriangleFilled(ImVec2(anchor.x - 5, anchor.y), ImVec2(anchor.x + 5, anchor.y), ImVec2(anchor.x, anchor.y + 6),
                          IM_COL32(8, 14, 18, 190));
    dl->AddText(p0 + ImVec2((wbox - s1.x) * 0.5f, 4.f), ImGui::GetColorU32(C_TEXT), line1.c_str());
    dl->AddText(p0 + ImVec2((wbox - s2.x) * 0.5f, 5.f + s1.y), ImGui::GetColorU32(C_AMBER), line2);
}

void Ui::drawLegend(AppState& app) {
    World& w = *app.world;
    RenderSettings& rs = *app.rs;
    struct Grad {
        std::string title, lo, hi;
        std::vector<Color> stops;
    };
    std::vector<Grad> grads;
    switch (rs.colorMode) {
        case ColorMode::Diet: grads.push_back({"Diet", "plants", "meat", {{0.35f, 0.85f, 0.35f, 1}, {0.95f, 0.85f, 0.3f, 1}, {0.95f, 0.25f, 0.22f, 1}}}); break;
        case ColorMode::Energy: grads.push_back({"Energy", "starving", "full", {{0.9f, 0.2f, 0.2f, 1}, {0.95f, 0.85f, 0.3f, 1}, {0.3f, 0.9f, 0.45f, 1}}}); break;
        case ColorMode::Generation: grads.push_back({"Generation", "0", std::to_string(w.maxGeneration), {{0.27f, 0.f, 0.33f, 1}, {0.13f, 0.57f, 0.55f, 1}, {0.99f, 0.91f, 0.14f, 1}}}); break;
        case ColorMode::Age: grads.push_back({"Age", "newborn", "lifespan", {{0.75f, 1.f, 0.75f, 1}, {0.45f, 0.4f, 0.5f, 1}}}); break;
        case ColorMode::Thermal: grads.push_back({"Preferred temperature", "cold", "hot", {{0.3f, 0.5f, 1.f, 1}, {0.9f, 0.9f, 0.9f, 1}, {1.f, 0.35f, 0.2f, 1}}}); break;
        case ColorMode::Toxin: grads.push_back({"Toxin", "harmless", "deadly", {{0.55f, 0.55f, 0.6f, 1}, {0.7f, 1.f, 0.15f, 1}}}); break;
        case ColorMode::Brain: grads.push_back({"Synapses", "0", "300+", {{0.27f, 0.f, 0.33f, 1}, {0.13f, 0.57f, 0.55f, 1}, {0.99f, 0.91f, 0.14f, 1}}}); break;
        case ColorMode::Genetic: grads.push_back({"Body colour (hue gene)", "0", "1", {hsv(0.f, 0.6f, 0.95f), hsv(0.2f, 0.6f, 0.95f), hsv(0.4f, 0.6f, 0.95f), hsv(0.6f, 0.6f, 0.95f), hsv(0.8f, 0.6f, 0.95f), hsv(1.f, 0.6f, 0.95f)}}); break;
        default: break;
    }
    switch (rs.overlay) {
        case Overlay::Temperature: grads.push_back({"Temperature", "-12°C", "40°C", {{0.16f, 0.28f, 0.8f, 1}, {0.95f, 0.92f, 0.55f, 1}, {0.88f, 0.22f, 0.16f, 1}}}); break;
        case Overlay::Fertility: grads.push_back({"Fertility", "barren", "lush", {{0.28f, 0.2f, 0.12f, 1}, {0.35f, 0.92f, 0.32f, 1}}}); break;
        case Overlay::Plants: grads.push_back({"Plant biomass", "grazed", "full", {{0.08f, 0.08f, 0.06f, 1}, {0.45f, 1.f, 0.35f, 1}}}); break;
        case Overlay::Pheromone: grads.push_back({"Pheromone", "none", "strong", {{0.1f, 0.08f, 0.12f, 1}, {0.92f, 0.42f, 1.f, 1}}}); break;
        case Overlay::Rainfall: grads.push_back({"Rain", "dry", "storm", {{0.75f, 0.6f, 0.35f, 1}, {0.25f, 0.55f, 1.f, 1}}}); break;
        default: break;
    }
    const bool speciesList = rs.colorMode == ColorMode::Species || rs.overlay == Overlay::Territories;
    if (grads.empty() && !speciesList) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos + ImVec2(viewX + 12.f, viewY + viewH - 12.f), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    if (ImGui::Begin("##legend", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking)) {
        for (const Grad& g : grads) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextUnformatted(g.title.c_str());
            ImGui::PopStyleColor();
            const float bw = 180.f, bh = 9.f;
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const int segs = int(g.stops.size()) - 1;
            for (int k = 0; k < segs; ++k) {
                float x0 = p.x + bw * float(k) / float(segs), x1 = p.x + bw * float(k + 1) / float(segs);
                ImU32 a = u32(g.stops[size_t(k)]), b = u32(g.stops[size_t(k + 1)]);
                dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + bh), a, b, b, a);
            }
            ImGui::Dummy(ImVec2(bw, bh));
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextUnformatted(g.lo.c_str());
            ImGui::SameLine(bw - ImGui::CalcTextSize(g.hi.c_str()).x + ImGui::GetStyle().WindowPadding.x);
            ImGui::TextUnformatted(g.hi.c_str());
            ImGui::PopStyleColor();
        }
        if (speciesList) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
            ImGui::TextUnformatted("Largest species");
            ImGui::PopStyleColor();
            sortScratch_.clear();
            for (const Species& s : w.species.all())
                if (s.alive()) sortScratch_.push_back({float(s.population), s.id});
            std::sort(sortScratch_.begin(), sortScratch_.end(), [](auto& a, auto& b) { return a.first > b.first; });
            for (size_t k = 0; k < std::min<size_t>(6, sortScratch_.size()); ++k) {
                const Species& s = w.species.get(sortScratch_[k].second);
                swatch(s.color);
                ImGui::SameLine();
                ImGui::Text("%s  %d", s.name().c_str(), s.population);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Ui::drawTour(AppState& app) {
    struct Step {
        const char* window; // panel to highlight (nullptr = world view)
        const char* title;
        const char* text;
    };
    static const Step steps[] = {
        {nullptr, "Your world",
         "Thousands of creatures live here. Scroll to zoom, drag to pan, click a creature to inspect it, double-click to "
         "follow it and right-click for more. Nothing is scripted: every behaviour you see evolved."},
        {ICON_MAGIC " Tools", "Tools and acts of god",
         "Feed the land, spawn new lineages, plant clones, raise mountains, carve seas, release a disease or drop a "
         "meteor. The View section changes how creatures are coloured and which map overlay is shown."},
        {ICON_GLOBE " Environment", "The rules of the world",
         "Every parameter is live: climate, seasons, day and night, weather, plant growth, metabolism, mutation rate, "
         "disease, camouflage and food contests. Change them while the simulation runs."},
        {ICON_SEARCH " Inspector", "One creature, up close",
         "Body, genome, energy, water and what it is doing right now. Follow it, follow its children when it dies "
         "(Dynasty), copy its genome or open it in the Genome Lab."},
        {ICON_BOLT " Brain", "A mind that evolved",
         "The radar shows what its five eyes see. Below, every input, hidden neuron and output lights up live, with "
         "synapses coloured by sign. Mutation adds and removes connections over generations."},
        {ICON_USERS " Species", "Species",
         "Lineages that drift far enough apart become new species with generated Latin names. Click one to highlight "
         "it in the world and see its field-guide card and trait radar."},
        {ICON_SITEMAP " Tree of Life", "The tree of life",
         "Every species that ever lived, with branches as thick as their populations. Scroll to zoom through time. "
         "The selected creature's ancestry is drawn in amber."},
        {ICON_LINECHART " Population", "Charts",
         "Overview tiles, population, predator-prey loops, species stack plots, behaviour, hawk-dove game theory, "
         "epidemics and causes of death. Catastrophes are marked on the timeline."},
        {ICON_FLASK " Traits", "Evolution, visualised",
         "Pick any gene and watch the whole population's distribution shift over time, or explore the morphospace "
         "and which senses brains actually rely on."},
        {ICON_HISTORY " Events", "History",
         "A journal of behaviours the detector noticed (calls with meanings, farming, learning, fast evolution), a timeline "
         "of speciations, extinctions and catastrophes, a hall of fame, and achievements the ecosystem can unlock."},
        {"##toolbar", "Time",
         "Pause, step, or run up to 256x (or flat out with MAX). The rewind button jumps back a year; the world keeps "
         "the last 8 yearly checkpoints. Press K any time for a cinematic tour. Have fun!"},
    };
    const int n = int(std::size(steps));
    if (tourStep_ >= n) {
        tourStep_ = -1;
        return;
    }
    const Step& st = steps[tourStep_];
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImRect target(vp->Pos + ImVec2(viewX, viewY), vp->Pos + ImVec2(viewX + viewW, viewY + viewH));
    if (st.window) {
        if (ImGuiWindow* win = ImGui::FindWindowByName(st.window)) {
            target = ImRect(win->Pos, win->Pos + win->Size);
            if (tourFocused_ != tourStep_ && st.window[0] != '#') {
                ImGui::FocusWindow(win);
                tourFocused_ = tourStep_;
            }
        }
    }
    if (tourStep_ == 3 && !app.selectedId && !app.world->creatures.empty()) {
        app.selectedId = app.world->creatures[app.world->creatures.size() / 2].id;
        app.follow = true;
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float pulse = 0.6f + 0.4f * std::sin(time_ * 4.f);
    fg->AddRect(target.Min - ImVec2(3, 3), target.Max + ImVec2(3, 3),
                ImGui::GetColorU32(ImVec4(C_AMBER.x, C_AMBER.y, C_AMBER.z, pulse)), 8.f, 0, 3.f);
    // Card: place it inside the world view, away from the highlighted panel when possible.
    ImVec2 cardPos = vp->Pos + ImVec2(viewX + viewW * 0.5f, viewY + viewH * 0.62f);
    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(460.f, viewW - 30.f), 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    if (ImGui::Begin("##tour", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("Tour %d / %d", tourStep_ + 1, n);
        ImGui::PopStyleColor();
        ImGui::PushFont(fontTitle_, fontTitle_->LegacySize);
        ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
        ImGui::TextUnformatted(st.title);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::TextWrapped("%s", st.text);
        ImGui::Spacing();
        if (tourStep_ > 0 && ImGui::Button(ICON_UP " Back")) --tourStep_;
        if (tourStep_ > 0) ImGui::SameLine();
        if (ImGui::Button(tourStep_ == n - 1 ? ICON_CHECK " Finish" : ICON_PLAY " Next")) ++tourStep_;
        ImGui::SameLine();
        if (ImGui::Button("Skip")) tourStep_ = -1;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Ui::drawWelcome(AppState& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(std::min(vp->Size.x - 60.f, 1280.f), std::min(vp->Size.y - 60.f, 780.f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 22));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f);
    if (!ImGui::IsPopupOpen("Welcome")) ImGui::OpenPopup("Welcome");
    if (!ImGui::BeginPopupModal("Welcome", nullptr,
                                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::PushFont(fontTitle_, fontTitle_->LegacySize * 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
    ImGui::TextUnformatted(ICON_LEAF " Primordia");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::TextWrapped("Thousands of creatures with evolving bodies and neural-network brains. Nothing is scripted: "
                       "pick a starting world and watch natural selection do the rest.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Spacing();
    const int cols = size.x > 1150.f ? 4 : (size.x > 900.f ? 3 : 2);
    const float gap = 14.f;
    const float cardW = (ImGui::GetContentRegionAvail().x - gap * float(cols - 1)) / float(cols);
    const float cardH = cols >= 4 ? 158.f : 150.f;
    ImGui::BeginChild("##cards", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
    for (int i = 0; i < scenarioCount(); ++i) {
        const Scenario& sc = scenario(i);
        if (i % cols) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.f);
        ImGui::BeginChild("##card", ImVec2(cardW, cardH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushFont(fontTitle_, fontTitle_->LegacySize);
        ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
        ImGui::TextUnformatted(sc.icon);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(sc.name);
        ImGui::PopFont();
        if ((app.challengesDone >> i) & 1) {
            ImGui::SameLine();
            ImGui::TextColored(C_GREEN, ICON_CHECK);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("You have met this scenario's goal");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
        ImGui::TextUnformatted(sc.tagline);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - 26.f);
        ImGui::TextUnformatted(sc.description);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndChild();
        bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(a, b, ImGui::GetColorU32(C_ACCENT), 10.f, 0, 2.f);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            app.newParams = SimParams{};
            sc.setup(app.newWorld, app.newParams);
            app.newWorld.seed = std::random_device{}() % 1000000;
            app.newScenario = i;
            app.requestNewWorld = true;
            app.paused = false;
            showWelcome = false;
            hintTime_ = 100.f;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % cols == cols - 1) ImGui::Dummy(ImVec2(0, gap * 0.4f));
    }
    ImGui::EndChild();
    if (app.autosaveExists) {
        ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_DIM);
        if (ImGui::Button(ICON_HISTORY " Continue where you left off", ImVec2(260, 0))) {
            app.requestLoad = app.autosavePath;
            showWelcome = false;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (ImGui::Button(ICON_PLAY " Keep this world", ImVec2(170, 0))) showWelcome = false;
    ImGui::SameLine();
    if (ImGui::Button(ICON_STAR " Take the tour", ImVec2(150, 0))) {
        showWelcome = false;
        tourStep_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_OPEN " Open a saved world", ImVec2(200, 0))) {
        app.requestLoad = "?";
        showWelcome = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_COG " Custom world...", ImVec2(170, 0))) {
        openNewWorld = true;
        showWelcome = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FLASK " Primer Lab", ImVec2(150, 0))) {
        showPrimer_ = true;
        showWelcome = false;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The classic speed / size / sense natural-selection experiment, in a small arena");

    if (!showWelcome) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

void Ui::drawCinematic(AppState& app, float dt) {
    World& w = *app.world;
    time_ += dt;
    w.watchId = app.selectedId;
    int i = w.indexOfId(app.selectedId);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos + ImVec2(28, vp->Size.y - 28), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 12));
    ImGui::Begin("##cine", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    if (i >= 0) {
        const Creature& c = w.creatures[i];
        if (w.species.valid(c.speciesId)) {
            const Species& s = w.species.get(c.speciesId);
            ImGui::PushFont(fontTitle_, fontTitle_->LegacySize * 1.25f);
            ImGui::PushStyleColor(ImGuiCol_Text, v4(s.color));
            ImGui::TextUnformatted(s.name().c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
        ImGui::TextUnformatted(activityOf(c));
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
        ImGui::Text("Generation %u  ·  year %.1f  ·  %zu creatures  ·  %d species", c.generation, w.year(),
                    w.creatures.size(), w.species.aliveCount());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 0.35f));
    ImGui::TextUnformatted("K or Esc to leave cinematic mode");
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Ui::drawHoverTooltip(AppState& app) {
    if (!app.rs->hoverId || ImGui::GetIO().WantCaptureMouse) return;
    World& w = *app.world;
    int i = w.indexOfId(app.rs->hoverId);
    if (i < 0) return;
    const Creature& c = w.creatures[i];
    ImGui::BeginTooltip();
    if (w.species.valid(c.speciesId)) {
        swatch(w.species.get(c.speciesId).color);
        ImGui::SameLine();
        ImGui::TextUnformatted(w.species.get(c.speciesId).name().c_str());
    }
    ImGui::PushStyleColor(ImGuiCol_Text, C_MUTED);
    ImGui::Text("%s · %s · gen %u · energy %.0f%% · age %.1fy", personalName(c.id).c_str(), dietLabel(c.ph.diet), c.generation,
                100.f * c.energy / c.maxEnergy, c.age / w.params.yearLength);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, C_AMBER);
    ImGui::TextUnformatted(activityOf(c));
    ImGui::PopStyleColor();
    ImGui::EndTooltip();
}

void Ui::drawToasts(AppState& app, float dt) {
    World& w = *app.world;
    // Surface notable world events as toasts.
    if (w.events.counter != lastEventCount_) {
        if (lastEventCount_ != 0 && !w.events.items.empty()) {
            // Look at every event since the last frame (fast-forward can produce several).
            size_t fresh = std::min<size_t>(w.events.items.size(), size_t(w.events.counter - lastEventCount_));
            for (size_t k = w.events.items.size() - fresh; k < w.events.items.size(); ++k) {
                const WorldEvent& e = w.events.items[k];
                const bool founding = e.tick == 0;
                if ((app.pauseOnSpeciation && e.kind == EventKind::Speciation && !founding) ||
                    (app.pauseOnExtinction && e.kind == EventKind::Extinction) ||
                    (app.pauseOnAchievement && e.kind == EventKind::Milestone &&
                     e.text.rfind("Achievement", 0) == 0)) {
                    app.paused = true;
                    app.notify(ICON_PAUSE " Paused: " + e.text);
                    if (e.pos.x != 0.f || e.pos.y != 0.f) app.cam->targetCenter = e.pos;
                }
            }
            // A discovery anywhere in the batch wins over whatever happened to come last.
            const WorldEvent* e = &w.events.items.back();
            for (size_t k = w.events.items.size() - fresh; k < w.events.items.size(); ++k)
                if (w.events.items[k].kind == EventKind::Discovery) e = &w.events.items[k];
            if (!app.paused && e->kind == EventKind::Discovery) {
                app.notify(std::string(ICON_EYE " Discovery: ") + e->text);
                app.toastTime = 7.f; // long enough to read and click
                app.toastPos = e->pos;
                app.toastSpecies = e->speciesId;
                app.toastClickable = true;
            } else if (!app.paused && (e->kind == EventKind::Catastrophe || e->kind == EventKind::Milestone)) {
                app.notify(e->text);
            }
        }
        lastEventCount_ = w.events.counter;
    }
    if (app.toastTime <= 0.f || showWelcome) return;
    app.toastTime -= dt;
    float alpha = clamp01(app.toastTime / 0.5f);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pos(vp->Pos.x + viewX + viewW * 0.5f, vp->Pos.y + viewY + viewH - 30.f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.88f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 10));
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking |
                     (app.toastClickable ? 0 : ImGuiWindowFlags_NoInputs));
    ImGui::TextUnformatted(app.toast.c_str());
    if (app.toastClickable) {
        ImGui::SameLine();
        ImGui::TextColored(C_MUTED, "  click to go there");
        if (ImGui::IsWindowHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            app.toastTime = std::max(app.toastTime, 1.f); // do not fade out from under the pointer
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (app.toastPos.x != 0.f || app.toastPos.y != 0.f) app.cam->targetCenter = app.toastPos;
                if (app.toastSpecies && w.species.valid(app.toastSpecies)) app.rs->highlightSpecies = app.toastSpecies;
                app.follow = false;
                app.toastTime = 0.f;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

} // namespace pm
