#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "render/Camera.hpp"
#include "render/Renderer.hpp"
#include "sim/World.hpp"
#include "ui/PrimerLab.hpp"

struct ImFont;

namespace pm {

class ThreadPool;

enum class Tool : int { Select, Feed, Spawn, Clone, Smite, Meteor, Raise, Lower, Infect, Count };
const char* toolName(Tool t);

constexpr int SPEED_COUNT = 11;
extern const float SPEED_TICKS[SPEED_COUNT]; // ticks per frame; last entry = unlimited

// State shared between the app shell, the UI and the renderer.
struct AppState {
    World* world = nullptr;
    ThreadPool* pool = nullptr;
    Camera* cam = nullptr;
    RenderSettings* rs = nullptr;
    Renderer* renderer = nullptr;

    bool paused = false;
    int speed = 3;           // index into SPEED_TICKS
    bool stepOnce = false;
    double tps = 0, fps = 0, frameMs = 0, simMs = 0;

    Tool tool = Tool::Select;
    float brushRadius = 140.f;
    int spawnCount = 20;
    bool spawnPredators = false;

    uint32_t selectedId = 0;
    bool follow = false;
    bool dynasty = true; // when the followed creature dies, follow its offspring
    bool pauseOnSpeciation = false, pauseOnExtinction = false, pauseOnAchievement = false;
    uint32_t lastSelectedSpecies = 0;

    Genome clipboard;
    bool hasClipboard = false;
    uint32_t clipboardSpecies = 0;

    bool showUi = true;
    bool cinematic = false;
    float cineTimer = 0.f;
    bool requestNewWorld = false;
    bool requestQuit = false;
    bool requestScreenshot = false;
    bool requestResetLayout = false;
    bool requestCsv = false;
    bool requestNewick = false, requestSpeciesCsv = false;
    float uiScale = 1.f;
    bool sound = false;
    float soundVolume = 0.6f;
    int requestThreads = 0; // > 0: rebuild the thread pool with this many threads
    int threadsSetting = 0; // persisted choice (0 = all cores)
    bool fullscreen = false, vsync = true;
    bool applyDisplay = false; // fullscreen/vsync changed
    bool recording = false, toggleRecording = false;
    int recordEvery = 4; // frames between time-lapse captures
    int recordFrame = 0;
    std::string recordDir;
    std::string requestSave, requestLoad;
    WorldSettings newWorld;
    SimParams newParams;
    int newScenario = -1; // >= 0: also apply the scenario's parameters and founder tweaks
    int currentScenario = -1;
    uint32_t contextId = 0;      // creature whose right-click menu should open
    bool openContext = false;
    bool showNotes = true;
    std::string saveDir;
    std::string autosavePath;
    struct Checkpoint {
        int64_t tick;
        double wallTime;
        size_t creatures;
        int species;
        std::string data;
    };
    std::vector<Checkpoint> checkpoints; // newest last
    int requestRewind = -1;
    bool autosaveExists = false;
    float autosaveMinutes = 10.f;
    std::string lastSavePath;

    // Toast notifications
    std::string toast;
    float toastTime = 0.f;
    void notify(std::string msg) {
        toast = std::move(msg);
        toastTime = 3.5f;
    }
};

class Ui {
public:
    void init(float scale, const std::string& assetDir);
    void draw(AppState& app, float dt);
    void drawCinematic(AppState& app, float dt); // caption shown while touring
    // Central free area between docked panels, in ImGui coordinates (points).
    float viewX = 0, viewY = 0, viewW = 0, viewH = 0;
    bool openNewWorld = false;
    bool showWelcome = true;
    std::vector<std::string> focusRequests;

private:
    void applyTheme(float scale);
    void buildDefaultLayout(unsigned dockId);
    void drawMenuBar(AppState& app);
    void drawToolbar(AppState& app);
    void drawStatusBar(AppState& app);
    void drawInspector(AppState& app);
    void drawBrain(AppState& app);
    void drawPopulation(AppState& app);
    void drawTraits(AppState& app);
    void drawSpecies(AppState& app);
    void drawPhylogeny(AppState& app);
    void drawEvents(AppState& app);
    void drawEnvironment(AppState& app);
    void drawTools(AppState& app);
    void drawMinimap(AppState& app);
    void drawPerformance(AppState& app);
    void drawNewWorldModal(AppState& app);
    void drawHelp(AppState& app);
    void drawToasts(AppState& app, float dt);
    void drawHoverTooltip(AppState& app);
    void drawEncyclopedia(AppState& app);
    void drawWelcome(AppState& app);
    void drawGenomeLab(AppState& app);
    void drawPreferences(AppState& app);
    void drawTour(AppState& app);
    void drawLegend(AppState& app);
    void drawNameTag(AppState& app);
    void speciesBadge(AppState& app, uint32_t speciesId); // icon if cached, else colour swatch

    ImFont* fontBody_ = nullptr;
    ImFont* fontMono_ = nullptr;
    ImFont* fontTitle_ = nullptr;
    float scale_ = 1.f;
    bool layoutBuilt_ = false;
    // window visibility
    bool showInspector_ = true, showBrain_ = true, showPopulation_ = true, showTraits_ = true, showSpecies_ = true,
         showPhylogeny_ = true, showEvents_ = true, showEnvironment_ = true, showTools_ = true, showMinimap_ = true,
         showPerformance_ = false, showHelp_ = false, showDemo_ = false, showEncyclopedia_ = false,
         showLab_ = false, showPrefs_ = false, showPrimer_ = false;
    PrimerLab primerLab_;
    int tourStep_ = -1; // -1 = not touring
    int tourFocused_ = -1;
    Genome lab_;
    bool labReady_ = false;
    float time_ = 0.f;
    // widget state
    int traitGene_ = G_DIET;
    int scatterX_ = G_SIZE, scatterY_ = G_DIET;
    int historyWindow_ = 0; // 0 = all
    bool showExtinct_ = false;
    int minPhyloPeak_ = 8;
    float phyloZoom_ = 1.f, phyloScroll_ = 0.f;
    bool brainShowAll_ = false;
    int hoveredNeuron_ = -1;
    uint64_t lastEventCount_ = 0;
    int eventFilter_ = 0;
    std::vector<float> scratchA_, scratchB_, scratchC_;
    std::vector<std::pair<float, uint32_t>> sortScratch_;
    std::vector<float> senseImportance_;
    uint32_t historyId_ = 0;
    std::vector<float> energyHist_, healthHist_;
    float hintTime_ = 0.f;
    char renameBuf_[96] = {};
    uint32_t renameId_ = 0;
    int senseFrame_ = 0;
};

} // namespace pm
