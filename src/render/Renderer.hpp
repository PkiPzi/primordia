#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/Math.hpp"
#include "core/Random.hpp"
#include "render/Camera.hpp"
#include "render/Gl.hpp"

namespace pm {

class World;
struct Creature;

enum class ColorMode : int { Genetic, Species, Diet, Energy, Generation, Age, Thermal, Toxin, Brain, Disease, Count };
enum class Overlay : int { None, Temperature, Fertility, Plants, Pheromone, Territories, Rainfall, Count };
const char* colorModeName(ColorMode m);
const char* overlayName(Overlay o);

struct RenderSettings {
    ColorMode colorMode = ColorMode::Genetic;
    Overlay overlay = Overlay::None;
    bool bloom = true;
    float bloomStrength = 0.9f;
    bool showVision = true;
    bool showTrails = true;
    bool particles = true;
    bool showCorpses = true;
    bool showSignals = true;
    bool showPheromones = true;
    bool vignette = true;
    float creatureScale = 1.f;
    float nightDarkness = 0.6f; // how dark nights look (visual only)
    float minCreaturePx = 2.6f; // creatures never shrink below this on screen
    bool blobStyle = false;     // round Primer-style blobs instead of tadpoles
    bool clouds = true;         // drifting cloud shadows
    uint32_t selectedId = 0;
    uint32_t highlightSpecies = 0;
    uint32_t hoverId = 0;
    bool brushVisible = false;
    Vec2 brushPos;
    float brushRadius = 100.f;
    Color brushColor{1, 1, 1, 0.6f};
};

class Renderer {
public:
    bool init();
    void shutdown();
    // Renders the world into the scene buffers and composites to `targetFbo` (0 = window).
    void render(const World& world, const Camera& cam, const RenderSettings& rs, float dt, GLuint targetFbo = 0);
    void consumeFx(World& world, const RenderSettings& rs);

    // Draws a single creature into an offscreen texture (for the inspector and genome lab).
    GLuint portrait(int slot, const World& world, const Creature& c, const RenderSettings& rs, float time);
    GLuint minimapTexture() const { return minimap_.tex[0]; }
    GLuint portraitFbo(int slot) const { return portrait_[slot & 1].fbo; }
    // Small cached portrait of a species' representative creature (atlas texture + uv rect).
    bool speciesIcon(const World& world, uint32_t speciesId, GLuint& tex, float uv[4]);
    void beginFrameIcons() { iconBudget_ = 6; }
    int minimapSize() const { return minimap_.w; }
    void requestMinimapRefresh() { minimapDirty_ = true; }
    const char* glRenderer() const { return glRenderer_.c_str(); }
    Color creatureColor(const World& w, const Creature& c, ColorMode mode) const;

private:
    struct CreatureInst {
        float x, y, angle, radius;
        float r, g, b, a;
        float pattern, diet, armor, weapon;
        float fov, phase, signal, flags;
        float health, eye, thrust, toxin;
        float nightVision, sick, pad0, pad1;
        float aquatic, wings, spikes, photo;
    };
    struct CorpseInst {
        float x, y, radius, fresh;
        float hue, toxin, seed, pad;
    };
    struct ParticleInst {
        float x, y, size, kind;
        float r, g, b, a;
    };
    struct Particle {
        Vec2 pos, vel;
        float life, maxLife, size, kind;
        Color color;
    };
    struct Vtx {
        float x, y;
        uint32_t color;
    };

    void ensureTargets(int w, int h);
    void uploadTerrain(const World& world);
    void drawTerrain(const World& world, const Camera& cam, const RenderSettings& rs, bool minimap);
    void drawCorpses(const World& world, const Camera& cam);
    void drawCreatures(const World& world, const Camera& cam, const RenderSettings& rs);
    void drawParticles(const Camera& cam, float dt);
    void drawOverlays(const World& world, const Camera& cam, const RenderSettings& rs);
    void bloomAndComposite(const RenderSettings& rs, GLuint targetFbo, int w, int h);
    void setCameraUniforms(const gl::Program& p, const Camera& cam) const;
    // Overlay geometry helpers (world space).
    void line(Vec2 a, Vec2 b, float width, Color c);
    void circle(Vec2 c, float r, float width, Color col, int segments = 48);
    void disc(Vec2 c, float r, Color col, int segments = 32);
    void sector(Vec2 c, float r, float a0, float a1, Color col);

    gl::Program terrainProg_, creatureProg_, corpseProg_, particleProg_, overlayProg_, blurProg_, compositeProg_;
    GLuint quadVao_ = 0, quadVbo_ = 0;
    GLuint creatureVao_ = 0, creatureVbo_ = 0;
    GLuint corpseVao_ = 0, corpseVbo_ = 0;
    GLuint particleVao_ = 0, particleVbo_ = 0;
    GLuint overlayVao_ = 0, overlayVbo_ = 0;
    GLuint staticTex_ = 0, dynTex_ = 0, territoryTex_ = 0, rainTex_ = 0;
    void updateTerritories(const World& world);
    std::vector<uint64_t> territoryKeys_;
    std::vector<uint8_t> territoryPixels_;
    uint32_t terrainVersion_ = 0xffffffffu;
    int terrainW_ = 0, terrainH_ = 0;
    gl::Target scene_, blurA_, blurB_, minimap_, portrait_[2], iconAtlas_;
    struct IconSlot { uint32_t species = 0; uint64_t lastUse = 0; };
    std::vector<IconSlot> iconSlots_;
    uint64_t iconClock_ = 0;
    int iconBudget_ = 6;
    void drawCreatureInto(const CreatureInst& inst, int x, int y, int size, float time);
    CreatureInst makeInstance(const World& world, const Creature& c, const RenderSettings& rs) const;
    bool minimapDirty_ = true;
    int frame_ = 0;
    float time_ = 0.f;
    std::vector<CreatureInst> creatureInst_;
    std::vector<CorpseInst> corpseInst_;
    std::vector<ParticleInst> particleInst_;
    std::vector<Particle> particles_;
    std::vector<Vtx> overlayVerts_;
    std::vector<uint8_t> dynPixels_;
    std::vector<uint8_t> staticPixels_;
    std::deque<Vec2> trail_;
    uint32_t trailId_ = 0;
    Rng rng_{12345};
    std::string glRenderer_;
    float zoomForOverlay_ = 1.f;
    Color ambient_{1, 1, 1, 1};
    float night_ = 0.f;
    bool blobStyleForIcons_ = false;
};

} // namespace pm
