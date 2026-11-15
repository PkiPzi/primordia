#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#include "app/Audio.hpp"
#include "app/Options.hpp"
#include "core/ThreadPool.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "implot.h"
#include "render/Renderer.hpp"
#include "sim/Scenarios.hpp"
#include "sim/World.hpp"
#include "ui/Ui.hpp"

namespace pm {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_dialogMutex;
std::string g_dialogSave, g_dialogLoad;

void SDLCALL onSaveDialog(void*, const char* const* files, int) {
    if (files && files[0]) {
        std::lock_guard<std::mutex> lk(g_dialogMutex);
        g_dialogSave = files[0];
    }
}
void SDLCALL onLoadDialog(void*, const char* const* files, int) {
    if (files && files[0]) {
        std::lock_guard<std::mutex> lk(g_dialogMutex);
        g_dialogLoad = files[0];
    }
}

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

} // namespace

class App {
public:
    int run(const Options& opt);

private:
    bool init(const Options& opt);
    void shutdown();
    void frame(float dt);
    void handleEvent(const SDL_Event& e);
    void handleWorldMouse(float dt);
    void simulate();
    void applyRequests();
    void loadSettings();
    void saveSettings();
    void autosave(const char* reason);
    bool saveScreenshot(const std::string& path, GLuint fbo, int w, int h, bool keepAlpha = false);

    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<World> world_;
    Renderer renderer_;
    Camera cam_;
    RenderSettings rs_;
    Ui ui_;
    AppState app_;
    Options opt_;
    bool running_ = true;
    float density_ = 1.f;

    // Mouse interaction
    bool dragging_ = false, painting_ = false;
    Vec2 lastMouse_;
    Vec2 downMouse_;
    double tpsWindowStart_ = 0;
    int64_t tpsTicks_ = 0;
    double tickAccum_ = 0;
    gl::Target shotTarget_;
    Audio audio_;
    bool audioReady_ = false, audioFailed_ = false;
    uint64_t audioEventCount_ = 0;
    double lastChime_ = 0;
    uint32_t dynastyParent_ = 0, dynastySpecies_ = 0;
    Vec2 dynastyPos_;
    Vec2 rightDown_;
    double lastClickTime_ = 0;
    std::string settingsPath_;
    double lastAutosave_ = 0;
};

int runApp(const Options& opt) {
    App app;
    return app.run(opt);
}

bool App::init(const Options& opt) {
    opt_ = opt;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (!opt.screenshot.empty() || !opt.icon.empty()) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow("Primordia", opt.width, opt.height, flags);
    if (!window_) {
        std::fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        return false;
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        std::fprintf(stderr, "OpenGL 3.3 context failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath;
    if (char* pref = SDL_GetPrefPath("Primordia", "primordia")) {
        iniPath = std::string(pref) + "layout.ini";
        SDL_free(pref);
        io.IniFilename = iniPath.c_str();
    }
    if (!opt.screenshot.empty()) io.IniFilename = nullptr;
    ImGui_ImplSDL3_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 330");

    float contentScale = SDL_GetWindowDisplayScale(window_) / std::max(0.5f, SDL_GetWindowPixelDensity(window_));
    std::string base = SDL_GetBasePath() ? SDL_GetBasePath() : "./";
    ui_.init(std::max(1.f, contentScale), base + "assets/");
    ui_.showWelcome = (opt.screenshot.empty() && opt.load.empty()) || opt.welcome;
    ui_.focusRequests = opt.focus;
    if (opt.overlay >= 0) rs_.overlay = Overlay(std::min(opt.overlay, int(Overlay::Count) - 1));
    renderer_.init();
    std::printf("Primordia: OpenGL renderer %s\n", renderer_.glRenderer());

    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    app_.saveDir = std::string(docs ? docs : (SDL_GetBasePath() ? SDL_GetBasePath() : "./")) + "Primordia/";
    SDL_CreateDirectory(app_.saveDir.c_str());
    app_.autosavePath = app_.saveDir + "autosave.prim";
    app_.autosaveExists = SDL_GetPathInfo(app_.autosavePath.c_str(), nullptr);
    if (!iniPath.empty()) settingsPath_ = iniPath.substr(0, iniPath.size() - 10) + "settings.ini";
    if (opt.screenshot.empty() && opt.icon.empty()) loadSettings();
    SDL_GL_SetSwapInterval(app_.vsync ? 1 : 0);

    const int threads = opt.threads > 0 ? opt.threads : app_.threadsSetting;
    pool_ = std::make_unique<ThreadPool>(threads > 0 ? unsigned(threads) : 0u);
    world_ = std::make_unique<World>(pool_.get());
    bool loaded = false;
    if (!opt.load.empty()) {
        std::string err;
        loaded = world_->load(opt.load, err);
        if (!loaded) std::fprintf(stderr, "Load failed: %s\n", err.c_str());
        else app_.lastSavePath = opt.load;
    }
    if (!loaded) {
        WorldSettings ws;
        if (opt.scenario >= 0 && opt.scenario < scenarioCount()) scenario(opt.scenario).setup(ws, world_->params);
        ws.seed = opt.seed ? opt.seed : std::random_device{}() % 1000000;
        if (opt.worldSize > 0) ws.width = ws.height = opt.worldSize;
        if (opt.population > 0) ws.initialPopulation = opt.population;
        world_->create(ws);
        if (opt.scenario >= 0 && opt.scenario < scenarioCount() && scenario(opt.scenario).post)
            scenario(opt.scenario).post(*world_);
        if (opt.scenario >= 0) ui_.showWelcome = false;
        app_.currentScenario = opt.scenario;
    }
    app_.newWorld = world_->settings;
    app_.world = world_.get();
    app_.pool = pool_.get();
    app_.cam = &cam_;
    app_.rs = &rs_;
    app_.renderer = &renderer_;

    int fw, fh;
    SDL_GetWindowSizeInPixels(window_, &fw, &fh);
    cam_.fbSize = {float(fw), float(fh)};
    cam_.viewSize = cam_.fbSize * 0.6f;
    cam_.viewCenter = cam_.fbSize * 0.5f;
    cam_.fit(world_->terrain.width, world_->terrain.height);
    cam_.center = cam_.targetCenter;
    cam_.zoom = cam_.targetZoom;
    for (int i = 0; i < opt.warmup; ++i) world_->tick();
    if (opt.zoom > 0.f) cam_.targetZoom = cam_.zoom = opt.zoom;
    if (opt.select && !world_->creatures.empty()) {
        // Pick the creature with the most neighbours on screen: makes for a good close-up.
        const Creature& c = world_->creatures[world_->creatures.size() / 3];
        app_.selectedId = c.id;
        app_.follow = true;
        rs_.highlightSpecies = c.speciesId;
        cam_.center = cam_.targetCenter = c.pos;
    }
    tpsWindowStart_ = SDL_GetTicksNS() * 1e-9;
    return true;
}

void App::loadSettings() {
    if (settingsPath_.empty()) return;
    std::FILE* f = std::fopen(settingsPath_.c_str(), "r");
    if (!f) return;
    char key[64];
    float v;
    while (std::fscanf(f, "%63[^=]=%f\n", key, &v) == 2) {
        std::string k = key;
        if (k == "colorMode") rs_.colorMode = ColorMode(std::clamp(int(v), 0, int(ColorMode::Count) - 1));
        else if (k == "overlay") rs_.overlay = Overlay(std::clamp(int(v), 0, int(Overlay::Count) - 1));
        else if (k == "bloom") rs_.bloom = v != 0;
        else if (k == "bloomStrength") rs_.bloomStrength = v;
        else if (k == "vision") rs_.showVision = v != 0;
        else if (k == "trails") rs_.showTrails = v != 0;
        else if (k == "particles") rs_.particles = v != 0;
        else if (k == "corpses") rs_.showCorpses = v != 0;
        else if (k == "signals") rs_.showSignals = v != 0;
        else if (k == "pheromones") rs_.showPheromones = v != 0;
        else if (k == "vignette") rs_.vignette = v != 0;
        else if (k == "creatureScale") rs_.creatureScale = std::clamp(v, 0.5f, 3.f);
        else if (k == "nightDarkness") rs_.nightDarkness = std::clamp(v, 0.f, 1.f);
        else if (k == "blob") rs_.blobStyle = v != 0;
        else if (k == "speed") app_.speed = std::clamp(int(v), 0, SPEED_COUNT - 1);
        else if (k == "dynasty") app_.dynasty = v != 0;
        else if (k == "autosaveMinutes") app_.autosaveMinutes = std::clamp(v, 0.f, 120.f);
        else if (k == "uiScale") app_.uiScale = std::clamp(v, 0.75f, 2.f);
        else if (k == "vsync") app_.vsync = v != 0;
        else if (k == "minCreaturePx") rs_.minCreaturePx = std::clamp(v, 0.f, 6.f);
        else if (k == "recordEvery") app_.recordEvery = std::clamp(int(v), 1, 60);
        else if (k == "pauseSpeciation") app_.pauseOnSpeciation = v != 0;
        else if (k == "threads") app_.threadsSetting = std::clamp(int(v), 0, 256);
        else if (k == "sound") app_.sound = v != 0;
        else if (k == "soundVolume") app_.soundVolume = std::clamp(v, 0.f, 1.f);
        else if (k == "pauseExtinction") app_.pauseOnExtinction = v != 0;
        else if (k == "pauseAchievement") app_.pauseOnAchievement = v != 0;
    }
    std::fclose(f);
}

void App::saveSettings() {
    if (settingsPath_.empty() || !opt_.screenshot.empty()) return;
    std::FILE* f = std::fopen(settingsPath_.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "colorMode=%d\noverlay=%d\nbloom=%d\nbloomStrength=%g\nvision=%d\ntrails=%d\nparticles=%d\n",
                 int(rs_.colorMode), int(rs_.overlay), rs_.bloom, rs_.bloomStrength, rs_.showVision, rs_.showTrails,
                 rs_.particles);
    std::fprintf(f, "corpses=%d\nsignals=%d\npheromones=%d\nvignette=%d\ncreatureScale=%g\nnightDarkness=%g\nblob=%d\n",
                 rs_.showCorpses, rs_.showSignals, rs_.showPheromones, rs_.vignette, rs_.creatureScale,
                 rs_.nightDarkness, rs_.blobStyle);
    std::fprintf(f, "speed=%d\ndynasty=%d\nautosaveMinutes=%g\n", app_.speed, app_.dynasty, app_.autosaveMinutes);
    std::fprintf(f, "uiScale=%g\nvsync=%d\nminCreaturePx=%g\nrecordEvery=%d\n", app_.uiScale, app_.vsync, rs_.minCreaturePx,
                 app_.recordEvery);
    std::fprintf(f, "pauseSpeciation=%d\npauseExtinction=%d\npauseAchievement=%d\nthreads=%d\n", app_.pauseOnSpeciation,
                 app_.pauseOnExtinction, app_.pauseOnAchievement, app_.threadsSetting);
    std::fprintf(f, "sound=%d\nsoundVolume=%g\n", app_.sound, app_.soundVolume);
    std::fclose(f);
}

void App::autosave(const char* reason) {
    if (!opt_.screenshot.empty() || app_.autosavePath.empty() || world_->tickCount < 600) return;
    std::string err;
    if (world_->save(app_.autosavePath, err)) {
        app_.autosaveExists = true;
        if (reason) app_.notify(reason);
    }
    lastAutosave_ = SDL_GetTicksNS() * 1e-9;
}

void App::shutdown() {
    if (world_ && opt_.icon.empty()) {
        saveSettings();
        autosave(nullptr);
    }
    audio_.shutdown();
    shotTarget_.destroy();
    renderer_.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (gl_) SDL_GL_DestroyContext(gl_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

int App::run(const Options& opt) {
    if (!init(opt)) {
        shutdown();
        return 1;
    }
    if (!opt.icon.empty()) {
        // A friendly round grazer with big eyes, rendered with the in-game shader.
        Rng r(4);
        Genome g = Genome::random(r, 0.f);
        g.g[G_HUE] = 0.46f;
        g.g[G_PATTERN] = 0.85f;
        g.g[G_SIZE] = 0.5f;
        g.g[G_VISION_RANGE] = 0.9f;
        g.g[G_VISION_FOV] = 0.55f;
        g.g[G_WEAPON] = 0.f;
        g.g[G_ARMOR] = 0.1f;
        Creature c = world_->preview(g);
        RenderSettings irs;
        GLuint tex = renderer_.portrait(0, *world_, c, irs, 0.6f);
        (void)tex;
        bool ok = saveScreenshot(opt.icon, renderer_.portraitFbo(0), 192, 192, true);
        std::printf(ok ? "Saved %s\n" : "Could not save %s\n", opt.icon.c_str());
        shutdown();
        return ok ? 0 : 1;
    }
    auto last = Clock::now();
    int frames = 0;
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        auto now = Clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        last = now;
        frame(dt);
        ++frames;
        if (!opt.screenshot.empty() && frames >= opt.frames) running_ = false;
    }
    shutdown();
    return 0;
}

void App::handleEvent(const SDL_Event& e) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case SDL_EVENT_QUIT: running_ = false; break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e.window.windowID == SDL_GetWindowID(window_)) running_ = false;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (!io.WantCaptureMouse) {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                cam_.zoomAt({mx * density_, my * density_}, std::pow(1.18f, e.wheel.y));
                app_.follow = app_.follow && true;
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            if (io.WantTextInput) break;
            const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
            switch (e.key.key) {
                case SDLK_SPACE: app_.paused = !app_.paused; break;
                case SDLK_PERIOD: app_.paused = true; app_.stepOnce = true; break;
                case SDLK_ESCAPE:
                    if (app_.cinematic) { app_.cinematic = false; app_.showUi = true; app_.follow = false; break; }
                    if (app_.tool != Tool::Select) app_.tool = Tool::Select;
                    else { app_.selectedId = 0; app_.follow = false; rs_.highlightSpecies = 0; }
                    break;
                case SDLK_F: if (app_.selectedId) app_.follow = !app_.follow; break;
                case SDLK_H: case SDLK_HOME:
                    app_.follow = false;
                    cam_.fit(world_->terrain.width, world_->terrain.height);
                    break;
                case SDLK_TAB: app_.showUi = !app_.showUi; app_.cinematic = false; break;
                case SDLK_K:
                    app_.cinematic = !app_.cinematic;
                    app_.showUi = !app_.cinematic;
                    app_.cineTimer = 0.f;
                    if (!app_.cinematic) app_.follow = false;
                    break;
                case SDLK_C:
                    if (!ctrl) rs_.colorMode = ColorMode((int(rs_.colorMode) + 1) % int(ColorMode::Count));
                    else if (app_.selectedId) {
                        int i = world_->indexOfId(app_.selectedId);
                        if (i >= 0) {
                            app_.clipboard = world_->creatures[i].genome;
                            app_.clipboardSpecies = world_->creatures[i].speciesId;
                            app_.hasClipboard = true;
                            app_.notify("Genome copied. Use the Clone tool to plant it.");
                        }
                    }
                    break;
                case SDLK_V: rs_.showVision = !rs_.showVision; break;
                case SDLK_G: rs_.bloom = !rs_.bloom; break;
                case SDLK_B: rs_.blobStyle = !rs_.blobStyle; break;
                case SDLK_M: app_.sound = !app_.sound; app_.notify(app_.sound ? "Sound on" : "Sound off"); break;
                case SDLK_LEFTBRACKET:
                case SDLK_RIGHTBRACKET: {
                    // Cycle the highlight through living species, most populous first.
                    std::vector<std::pair<int, uint32_t>> living;
                    for (const Species& s : world_->species.all())
                        if (s.alive()) living.push_back({-s.population, s.id});
                    std::sort(living.begin(), living.end());
                    if (living.empty()) break;
                    int cur = -1;
                    for (int k = 0; k < int(living.size()); ++k)
                        if (living[size_t(k)].second == rs_.highlightSpecies) cur = k;
                    int n = int(living.size());
                    int next = e.key.key == SDLK_RIGHTBRACKET ? (cur + 1) % n : (cur <= 0 ? n - 1 : cur - 1);
                    rs_.highlightSpecies = living[size_t(next)].second;
                    app_.notify(world_->species.get(rs_.highlightSpecies).name() + "  (" +
                                std::to_string(world_->species.get(rs_.highlightSpecies).population) + " alive)");
                    break;
                }
                case SDLK_O:
                    if (!ctrl) rs_.overlay = Overlay((int(rs_.overlay) + 1) % int(Overlay::Count));
                    break;
                case SDLK_DELETE:
                    if (int i = world_->indexOfId(app_.selectedId); i >= 0) {
                        world_->creatures[i].alive = false;
                        world_->creatures[i].cause = DeathCause::Smite;
                        world_->killRadius(world_->creatures[i].pos, 0.f, DeathCause::Smite);
                    }
                    break;
                case SDLK_S: if (ctrl) app_.requestSave = app_.lastSavePath.empty() ? "?" : app_.lastSavePath; break;
                case SDLK_L: if (ctrl) app_.requestLoad = "?"; break;
                case SDLK_N: if (ctrl) ui_.openNewWorld = true; break;
                case SDLK_F12: app_.requestScreenshot = true; break;
                case SDLK_F10:
                    app_.fullscreen = !app_.fullscreen;
                    app_.applyDisplay = true;
                    break;
                case SDLK_RETURN:
                    if (e.key.mod & SDL_KMOD_ALT) {
                        app_.fullscreen = !app_.fullscreen;
                        app_.applyDisplay = true;
                    }
                    break;
                case SDLK_F11: app_.toggleRecording = true; break;
                case SDLK_F1: break;
                default:
                    if (e.key.key >= SDLK_1 && e.key.key <= SDLK_9) {
                        app_.speed = std::min(SPEED_COUNT - 1, int(e.key.key - SDLK_1));
                        app_.paused = false;
                    } else if (e.key.key == SDLK_0) {
                        app_.speed = SPEED_COUNT - 2;
                        app_.paused = false;
                    }
                    break;
            }
            if (ctrl && e.key.key == SDLK_O) app_.requestLoad = "?";
            break;
        }
        default: break;
    }
}

void App::handleWorldMouse(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    float mx, my;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    Vec2 mouse{mx * density_, my * density_};
    Vec2 world = cam_.screenToWorld(mouse);
    const bool overUi = io.WantCaptureMouse;
    const bool left = buttons & SDL_BUTTON_LMASK, right = buttons & SDL_BUTTON_RMASK,
               middle = buttons & SDL_BUTTON_MMASK;
    World& w = *world_;

    // Hover highlight
    rs_.hoverId = 0;
    if (!overUi) {
        int h = w.pick(world, 6.f / cam_.zoom);
        if (h >= 0) rs_.hoverId = w.creatures[h].id;
    }

    // Brush cursor
    const Tool tool = app_.tool;
    const bool brushTool = tool == Tool::Feed || tool == Tool::Smite || tool == Tool::Raise || tool == Tool::Lower ||
                           tool == Tool::Meteor || tool == Tool::Spawn || tool == Tool::Clone || tool == Tool::Infect;
    rs_.brushVisible = brushTool && !overUi;
    rs_.brushPos = world;
    rs_.brushRadius = tool == Tool::Meteor ? app_.brushRadius * 1.6f : app_.brushRadius;
    switch (tool) {
        case Tool::Feed: rs_.brushColor = {0.45f, 1.f, 0.45f, 0.8f}; break;
        case Tool::Smite: rs_.brushColor = {1.f, 0.3f, 0.3f, 0.8f}; break;
        case Tool::Meteor: rs_.brushColor = {1.f, 0.6f, 0.2f, 0.9f}; break;
        case Tool::Raise: rs_.brushColor = {0.9f, 0.8f, 0.6f, 0.8f}; break;
        case Tool::Lower: rs_.brushColor = {0.35f, 0.65f, 1.f, 0.8f}; break;
        case Tool::Infect: rs_.brushColor = {0.7f, 1.f, 0.35f, 0.85f}; break;
        default: rs_.brushColor = {1.f, 1.f, 1.f, 0.7f}; break;
    }

    static bool prevLeft = false, prevRight = false, prevMiddle = false;
    const bool leftPressed = left && !prevLeft, leftReleased = !left && prevLeft;

    if (!overUi && ((right && !prevRight) || (middle && !prevMiddle))) {
        dragging_ = true;
        rightDown_ = mouse;
    }
    // A right click without dragging opens the creature context menu.
    if (!right && prevRight && !overUi && (mouse - rightDown_).lengthSq() < 16.f) {
        int h = w.pick(world, 6.f / cam_.zoom);
        if (h >= 0) {
            app_.contextId = w.creatures[h].id;
            app_.openContext = true;
        }
    }
    if (leftPressed && !overUi) {
        downMouse_ = mouse;
        painting_ = false;
        if (tool == Tool::Select) {
            int h = w.pick(world, 6.f / cam_.zoom);
            if (h >= 0) {
                const bool again = app_.selectedId == w.creatures[h].id;
                app_.selectedId = w.creatures[h].id;
                app_.lastSelectedSpecies = w.creatures[h].speciesId;
                // Double-click (second click on the same creature) follows it.
                const double now = SDL_GetTicksNS() * 1e-9;
                if (again && now - lastClickTime_ < 0.4) app_.follow = true;
                lastClickTime_ = now;
            } else {
                dragging_ = true; // drag empty space to pan
            }
        } else if (tool == Tool::Spawn) {
            int n = app_.spawnPredators ? w.spawnPredators(world, app_.spawnCount, app_.brushRadius)
                                        : w.spawnRandom(world, app_.spawnCount, app_.brushRadius, 0.3f);
            app_.notify("Spawned " + std::to_string(n) + (app_.spawnPredators ? " hunters" : " grazers") + " of a new lineage");
        } else if (tool == Tool::Clone) {
            if (app_.hasClipboard) {
                int n = w.spawnGenome(app_.clipboard, app_.clipboardSpecies, world, app_.spawnCount, app_.brushRadius);
                app_.notify("Planted " + std::to_string(n) + " clones");
            } else {
                app_.notify("Copy a genome first: select a creature and press Ctrl+C");
            }
        } else if (tool == Tool::Meteor) {
            w.meteor(world, app_.brushRadius * 1.6f);
        } else if (tool == Tool::Infect) {
            int n = w.infect(world, app_.brushRadius);
            app_.notify(n ? std::to_string(n) + " creatures infected" : "No creatures under the brush");
        } else {
            painting_ = true;
        }
    }
    if (!left && !right && !middle) dragging_ = false;
    if (leftReleased) painting_ = false;

    if (dragging_ && (left || right || middle)) {
        Vec2 d = mouse - lastMouse_;
        if (d.lengthSq() > 0.f) {
            cam_.panPixels(d);
            app_.follow = false;
        }
    }
    if (painting_ && left && !overUi) {
        const float r = app_.brushRadius;
        switch (tool) {
            case Tool::Feed: w.feed(world, r, 3.f * dt); break;
            case Tool::Smite: w.killRadius(world, r, DeathCause::Smite); break;
            case Tool::Raise: w.terraform(world, r, 0.25f * dt); break;
            case Tool::Lower: w.terraform(world, r, -0.25f * dt); break;
            default: break;
        }
    }
    prevLeft = left;
    prevRight = right;
    prevMiddle = middle;
    lastMouse_ = mouse;
}

void App::simulate() {
    World& w = *world_;
    auto start = Clock::now();
    int64_t before = w.tickCount;
    if (app_.stepOnce) {
        w.tick();
        app_.stepOnce = false;
    } else if (!app_.paused) {
        const float perFrame = SPEED_TICKS[app_.speed];
        const bool unlimited = perFrame <= 0.f;
        const bool turbo = perFrame < 0.f;
        const double budgetMs = turbo ? 250.0 : (unlimited ? 28.0 : 22.0);
        tickAccum_ += unlimited ? 1e9 : perFrame;
        while (tickAccum_ >= 1.0) {
            w.tick();
            tickAccum_ -= 1.0;
            if (std::chrono::duration<double, std::milli>(Clock::now() - start).count() > budgetMs) {
                tickAccum_ = std::min(tickAccum_, 1.0);
                break;
            }
        }
        if (unlimited) tickAccum_ = 0;
    }
    app_.simMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    tpsTicks_ += w.tickCount - before;
    double now = SDL_GetTicksNS() * 1e-9;
    if (now - tpsWindowStart_ >= 0.5) {
        app_.tps = double(tpsTicks_) / (now - tpsWindowStart_);
        tpsTicks_ = 0;
        tpsWindowStart_ = now;
    }
    // Fade the hurt flag so attacks flash briefly.
    renderer_.consumeFx(w, rs_);
}

void App::applyRequests() {
    World& w = *world_;
    std::string dialogSave, dialogLoad;
    {
        std::lock_guard<std::mutex> lk(g_dialogMutex);
        dialogSave.swap(g_dialogSave);
        dialogLoad.swap(g_dialogLoad);
    }
    if (!dialogSave.empty()) app_.requestSave = dialogSave;
    if (!dialogLoad.empty()) app_.requestLoad = dialogLoad;

    static const SDL_DialogFileFilter filters[] = {{"Primordia world", "prim"}};
    if (app_.requestSave == "?") {
        std::string def = app_.saveDir + "world-" + timestamp() + ".prim";
        SDL_ShowSaveFileDialog(onSaveDialog, nullptr, window_, filters, 1, def.c_str());
        app_.requestSave.clear();
    } else if (!app_.requestSave.empty()) {
        std::string path = app_.requestSave;
        if (path.size() < 5 || path.substr(path.size() - 5) != ".prim") path += ".prim";
        std::string err;
        if (w.save(path, err)) {
            app_.lastSavePath = path;
            app_.notify("Saved " + path);
        } else {
            app_.notify("Save failed: " + err);
        }
        app_.requestSave.clear();
    }
    if (app_.requestLoad == "?") {
        SDL_ShowOpenFileDialog(onLoadDialog, nullptr, window_, filters, 1, app_.saveDir.c_str(), false);
        app_.requestLoad.clear();
    } else if (!app_.requestLoad.empty()) {
        std::string err;
        if (w.load(app_.requestLoad, err)) {
            app_.lastSavePath = app_.requestLoad;
            app_.selectedId = 0;
            app_.follow = false;
            rs_.highlightSpecies = 0;
            cam_.fit(w.terrain.width, w.terrain.height);
            app_.checkpoints.clear();
            app_.ghost.clear();
            app_.currentScenario = -1; // the save does not record which scenario it came from
            app_.notify("Loaded " + app_.requestLoad);
        } else {
            app_.notify("Load failed: " + err);
        }
        app_.requestLoad.clear();
    }
    // Sound: open the device lazily the first time it is switched on.
    if (app_.sound && !audioReady_ && !audioFailed_ && opt_.screenshot.empty()) {
        audioReady_ = audio_.init();
        audioFailed_ = !audioReady_;
        if (audioFailed_) app_.notify("No audio device available");
    }
    if (audioReady_) {
        audio_.setEnabled(app_.sound);
        audio_.setVolume(app_.soundVolume);
        audio_.setAmbience(w.rainAt(cam_.center.x, cam_.center.y), 1.f - w.daylight());
        if (w.events.counter != audioEventCount_) {
            size_t fresh = std::min<size_t>(w.events.items.size(), size_t(w.events.counter - audioEventCount_));
            const double now = SDL_GetTicksNS() * 1e-9;
            for (size_t k = w.events.items.size() - fresh; k < w.events.items.size() && audioEventCount_; ++k) {
                const WorldEvent& e = w.events.items[k];
                if (e.kind == EventKind::Catastrophe) audio_.play(Audio::Cue::Catastrophe);
                else if (e.kind == EventKind::Milestone && e.text.rfind("Achievement", 0) == 0) audio_.play(Audio::Cue::Achievement);
                else if (now - lastChime_ > 0.25) {
                    if (e.kind == EventKind::Speciation && e.tick > 0) audio_.play(Audio::Cue::Speciation), lastChime_ = now;
                    else if (e.kind == EventKind::Extinction) audio_.play(Audio::Cue::Extinction), lastChime_ = now;
                }
            }
            audioEventCount_ = w.events.counter;
        }
    }
    if (app_.requestThreads > 0) {
        // Swap in a new pool between ticks; the world only uses it inside tick().
        pool_ = std::make_unique<ThreadPool>(unsigned(app_.requestThreads));
        world_->setPool(pool_.get());
        app_.pool = pool_.get();
        app_.threadsSetting = app_.requestThreads;
        app_.notify("Simulation now uses " + std::to_string(pool_->size()) + " threads");
        app_.requestThreads = 0;
    }
    if (app_.applyDisplay) {
        app_.applyDisplay = false;
        SDL_SetWindowFullscreen(window_, app_.fullscreen);
        SDL_GL_SetSwapInterval(app_.vsync ? 1 : 0);
    }
    if (app_.requestNewick) {
        // Newick: (child1:len,child2:len)Name:len; branch lengths in years. Founders hang off a common root.
        app_.requestNewick = false;
        const std::string tree = w.newick();
        std::string path = app_.saveDir + "tree-of-life-" + timestamp() + ".nwk";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fputs(tree.c_str(), f);
            std::fclose(f);
            app_.notify("Tree of life exported to " + path);
        }
    }
    if (app_.requestSpeciesCsv) {
        app_.requestSpeciesCsv = false;
        std::string path = app_.saveDir + "species-" + timestamp() + ".csv";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "id,name,parent_id,born_year,extinct_year,population,peak,total_born,depth");
            for (int g = 0; g < G_COUNT; ++g) std::fprintf(f, ",mean_%s", GENE_INFO[g].name);
            std::fprintf(f, "\n");
            for (const Species& s : w.species.all()) {
                std::fprintf(f, "%u,\"%s\",%u,%.3f,%s,%d,%d,%llu,%d", s.id, s.name().c_str(), s.parentId,
                             double(s.bornTick) / w.params.yearLength,
                             s.alive() ? "" : std::to_string(double(s.extinctTick) / w.params.yearLength).c_str(),
                             s.population, s.peakPopulation, (unsigned long long)s.totalBorn, s.depth);
                for (int g = 0; g < G_COUNT; ++g) std::fprintf(f, ",%.4f", s.meanGenes[g]);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
            app_.notify("Species exported to " + path);
        }
    }
    if (app_.requestCsv) {
        app_.requestCsv = false;
        std::string path = app_.saveDir + "statistics-" + timestamp() + ".csv";
        if (std::FILE* f = std::fopen(path.c_str(), "w")) {
            std::fprintf(f, "tick,year,population,herbivores,omnivores,carnivores,species,plants,meat,births,deaths,"
                            "mean_generation,max_generation,synapses,mean_energy,climate,toxic_share,infected,immune,"
                            "sharing,attack_rate,cannibalism,signalling,marking,speed,toxic_targeted");
            for (int g = 0; g < G_COUNT; ++g) std::fprintf(f, ",mean_%s,std_%s", GENE_INFO[g].name, GENE_INFO[g].name);
            std::fprintf(f, "\n");
            for (const StatSample& s : w.stats.samples) {
                std::fprintf(f, "%.0f,%.4f,%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.1f,%.0f,%.0f,%.3f,%.0f,%.2f,%.4f,%.4f,%.4f,%.0f,%.0f,"
                                "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f",
                             s.tick, s.tick / w.params.yearLength, s.population, s.herbivores, s.omnivores, s.carnivores,
                             s.species, s.plants, s.meat, s.births, s.deaths, s.meanGeneration, s.maxGeneration,
                             s.connections, s.meanEnergy, s.climate, s.toxic, s.infected, s.immune, s.sharing,
                             s.attackRate, s.cannibalism, s.signalling, s.marking, s.speed, s.toxicTargeted);
                for (int g = 0; g < G_COUNT; ++g) std::fprintf(f, ",%.4f,%.4f", s.geneMean[g], s.geneStd[g]);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
            app_.notify("Statistics exported to " + path);
        } else {
            app_.notify("Could not write " + path);
        }
    }
    if (app_.requestNewWorld) {
        app_.requestNewWorld = false;
        if (app_.newScenario >= 0) w.params = app_.newParams;
        w.create(app_.newWorld);
        if (app_.newScenario >= 0 && scenario(app_.newScenario).post) scenario(app_.newScenario).post(w);
        if (app_.newScenario >= 0) app_.notify(std::string("Scenario: ") + scenario(app_.newScenario).name);
        app_.currentScenario = app_.newScenario;
        app_.showNotes = true;
        app_.newScenario = -1;
        app_.selectedId = 0;
        app_.follow = false;
        rs_.highlightSpecies = 0;
        cam_.fit(w.terrain.width, w.terrain.height);
        app_.checkpoints.clear();
        app_.ghost.clear();
        app_.notify("New world created (seed " + std::to_string(app_.newWorld.seed) + ")");
    }
}

bool App::saveScreenshot(const std::string& path, GLuint fbo, int w, int h, bool keepAlpha) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    if (fbo == 0) glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!s) return false;
    for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(s->pixels) + size_t(y) * s->pitch, px.data() + size_t(h - 1 - y) * w * 4,
                    size_t(w) * 4);
    if (!keepAlpha)
        for (int i = 0; i < w * h; ++i) static_cast<uint8_t*>(s->pixels)[(i / w) * s->pitch + (i % w) * 4 + 3] = 255;
    bool ok = SDL_SavePNG(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

void App::frame(float dt) {
    int ww, wh, fw, fh;
    SDL_GetWindowSize(window_, &ww, &wh);
    SDL_GetWindowSizeInPixels(window_, &fw, &fh);
    if (!opt_.screenshot.empty()) {
        fw = opt_.width;
        fh = opt_.height;
        ww = fw;
        wh = fh;
    }
    density_ = ww > 0 ? float(fw) / float(ww) : 1.f;
    cam_.fbSize = {float(fw), float(fh)};

    applyRequests();
    simulate();
    // Rewind checkpoints: one per simulated year, newest 8 kept in memory.
    {
        const int64_t every = std::max(1, world_->params.yearLength);
        const int64_t last = app_.checkpoints.empty() ? -every : app_.checkpoints.back().tick;
        if (world_->tickCount >= last + every && opt_.screenshot.empty()) {
            app_.checkpoints.push_back({world_->tickCount, SDL_GetTicksNS() * 1e-9, world_->creatures.size(),
                                        world_->species.aliveCount(), world_->snapshot()});
            if (app_.checkpoints.size() > 8) app_.checkpoints.erase(app_.checkpoints.begin());
        }
    }
    if (app_.requestRewind >= 0 && app_.requestRewind < int(app_.checkpoints.size())) {
        std::string err;
        const auto& cp = app_.checkpoints[size_t(app_.requestRewind)];
        // Keep what happened after the checkpoint so the new branch can be compared with it.
        std::vector<StatSample> abandoned;
        for (const StatSample& s : world_->stats.samples)
            if (s.tick >= float(cp.tick)) abandoned.push_back(s);
        if (world_->restore(cp.data, err)) {
            if (abandoned.size() >= 2) app_.ghost = std::move(abandoned);
            char buf[96];
            std::snprintf(buf, sizeof buf, "Rewound to year %.2f. Population shows the old branch for comparison.",
                          double(cp.tick) / world_->params.yearLength);
            app_.notify(buf);
            app_.checkpoints.resize(size_t(app_.requestRewind) + 1);
            app_.selectedId = 0;
            app_.follow = false;
        } else {
            app_.notify("Rewind failed: " + err);
        }
    }
    app_.requestRewind = -1;
    if (app_.autosaveMinutes > 0.f && !app_.paused) {
        double now = SDL_GetTicksNS() * 1e-9;
        if (lastAutosave_ == 0) lastAutosave_ = now;
        if (now - lastAutosave_ > app_.autosaveMinutes * 60.0) autosave("Autosaved");
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (!opt_.screenshot.empty()) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(float(fw), float(fh));
        io.DisplayFramebufferScale = ImVec2(1, 1);
    }
    ImGui::NewFrame();
    if (app_.cinematic) {
        // Tour: every few seconds cut to whichever creature is doing something interesting.
        app_.cineTimer -= dt;
        bool lost = world_->indexOfId(app_.selectedId) < 0;
        if ((app_.cineTimer <= 0.f || lost) && !world_->creatures.empty()) {
            static Rng cineRng(99);
            int best = -1;
            float bestScore = -1.f;
            for (int k = 0; k < 400; ++k) {
                const Creature& c = world_->creatures[size_t(cineRng.below(int(world_->creatures.size())))];
                float s = cineRng.uniform();
                if (c.flags & CF_HIT) s += 4.f;
                if (c.out[OUT_ATTACK] > 0.f && c.ph.meatEff > c.ph.plantEff) s += 2.f;
                if (c.flags & CF_SHARING) s += 2.5f;
                if (c.infection > 0.f) s += 1.f;
                if (std::fabs(c.signal) > 0.5f) s += 1.f;
                if (c.growth < 0.4f) s += 0.8f;
                if (world_->species.valid(c.speciesId) && world_->tickCount - world_->species.get(c.speciesId).bornTick < 20000)
                    s += 1.5f;
                if (s > bestScore) { bestScore = s; best = int(&c - world_->creatures.data()); }
            }
            if (best >= 0) {
                app_.selectedId = world_->creatures[best].id;
                app_.follow = true;
                cam_.targetZoom = 1.4f + float(cineRng.uniform()) * 1.4f;
            }
            app_.cineTimer = 11.f;
        }
        ui_.drawCinematic(app_, dt);
        ui_.viewX = 0;
        ui_.viewY = 0;
        ui_.viewW = float(ww);
        ui_.viewH = float(wh);
    } else if (app_.showUi) ui_.draw(app_, dt);
    else {
        ui_.viewX = 0;
        ui_.viewY = 0;
        ui_.viewW = float(ww);
        ui_.viewH = float(wh);
    }
    if (app_.requestQuit) running_ = false;
    if (app_.requestResetLayout) app_.requestResetLayout = false;

    // Camera: centre on the free area between panels.
    cam_.viewCenter = {(ui_.viewX + ui_.viewW * 0.5f) * density_, (ui_.viewY + ui_.viewH * 0.5f) * density_};
    cam_.viewSize = {std::max(100.f, ui_.viewW * density_), std::max(100.f, ui_.viewH * density_)};
    handleWorldMouse(dt);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        Vec2 pan;
        float sp = 900.f * dt;
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) pan.y += sp;
        if (keys[SDL_SCANCODE_S] && !(SDL_GetModState() & SDL_KMOD_CTRL)) pan.y -= sp;
        if (keys[SDL_SCANCODE_DOWN]) pan.y -= sp;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) pan.x += sp;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) pan.x -= sp;
        if (pan.lengthSq() > 0) {
            cam_.targetCenter -= pan / cam_.targetZoom;
            app_.follow = false;
        }
    }
    if (app_.selectedId) {
        int i = world_->indexOfId(app_.selectedId);
        if (i >= 0) {
            dynastyParent_ = app_.selectedId;
            dynastySpecies_ = world_->creatures[i].speciesId;
            dynastyPos_ = world_->creatures[i].pos;
            if (app_.follow) cam_.targetCenter = world_->creatures[i].pos;
        } else if (app_.dynasty && app_.follow && dynastyParent_ == app_.selectedId) {
            // The followed creature died: pass the camera to its youngest child, else the nearest relative.
            int best = -1;
            int64_t youngest = -1;
            float nearest = 1e18f;
            for (int k = 0; k < int(world_->creatures.size()); ++k) {
                const Creature& c = world_->creatures[k];
                if (c.parentId == app_.selectedId && c.birthTick > youngest) { youngest = c.birthTick; best = k; }
            }
            if (best < 0)
                for (int k = 0; k < int(world_->creatures.size()); ++k) {
                    const Creature& c = world_->creatures[k];
                    float d = (c.pos - dynastyPos_).lengthSq();
                    if (c.speciesId == dynastySpecies_ && d < nearest) { nearest = d; best = k; }
                }
            if (best >= 0) {
                const Creature& heir = world_->creatures[best];
                app_.notify(heir.parentId == app_.selectedId ? "Following the next generation: child #" + std::to_string(heir.id)
                                                             : "Line ended. Following relative #" + std::to_string(heir.id));
                app_.selectedId = heir.id;
                // Track the heir immediately so the chain survives even if it dies before the next frame.
                dynastyParent_ = heir.id;
                dynastySpecies_ = heir.speciesId;
                dynastyPos_ = heir.pos;
            } else {
                app_.follow = false;
            }
        } else if (app_.follow) {
            app_.follow = false;
        }
    }
    cam_.update(dt);
    rs_.selectedId = app_.selectedId;

    ImGui::Render();
    GLuint target = 0;
    if (!opt_.screenshot.empty()) {
        if (!shotTarget_.fbo) shotTarget_.create(fw, fh, 1, GL_RGBA8);
        target = shotTarget_.fbo;
    }
    renderer_.render(*world_, cam_, rs_, dt, target);
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, fw, fh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (app_.toggleRecording) {
        app_.toggleRecording = false;
        app_.recording = !app_.recording;
        if (app_.recording) {
            app_.recordDir = app_.saveDir + "timelapse-" + timestamp() + "/";
            SDL_CreateDirectory(app_.recordDir.c_str());
            app_.recordFrame = 0;
            app_.notify("Recording time-lapse frames to " + app_.recordDir);
        } else {
            app_.notify("Saved " + std::to_string(app_.recordFrame) + " frames. Make a video with: ffmpeg -framerate 30 -i " +
                        app_.recordDir + "frame_%05d.png timelapse.mp4");
        }
    }
    if (app_.recording && ImGui::GetFrameCount() % std::max(1, app_.recordEvery) == 0) {
        char name[32];
        std::snprintf(name, sizeof name, "frame_%05d.png", app_.recordFrame++);
        saveScreenshot(app_.recordDir + name, target, fw, fh);
    }
    if (app_.requestScreenshot) {
        app_.requestScreenshot = false;
        std::string path = app_.saveDir + "screenshot-" + timestamp() + ".png";
        if (saveScreenshot(path, target, fw, fh)) app_.notify("Screenshot saved to " + path);
        else app_.notify("Screenshot failed");
    }
    if (!opt_.screenshot.empty() && ImGui::GetFrameCount() >= opt_.frames - 1) {
        if (saveScreenshot(opt_.screenshot, target, fw, fh)) std::printf("Saved %s\n", opt_.screenshot.c_str());
        else std::fprintf(stderr, "Could not save %s\n", opt_.screenshot.c_str());
        running_ = false;
    }
    if (ImGui::GetFrameCount() % 60 == 0) {
        char title[160];
        std::snprintf(title, sizeof title, "Primordia  ·  Year %.1f  ·  %zu creatures  ·  %d species%s", world_->year(),
                      world_->creatures.size(), world_->species.aliveCount(), app_.paused ? "  ·  paused" : "");
        SDL_SetWindowTitle(window_, title);
    }
    SDL_GL_SwapWindow(window_);
    app_.fps = app_.fps * 0.95 + (dt > 0 ? 1.0 / dt : 0) * 0.05;
    app_.frameMs = app_.frameMs * 0.95 + dt * 1000.0 * 0.05;
}

} // namespace pm
