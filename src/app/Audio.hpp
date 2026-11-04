#pragma once
#include <atomic>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace pm {

// Small procedural sound engine: ambient wind and night crickets plus synthesized
// cues for world events. Everything is generated in the audio callback; no files.
class Audio {
public:
    enum class Cue { Speciation, Extinction, Catastrophe, Achievement };

    bool init();
    void shutdown();
    void setEnabled(bool on) { enabled_.store(on); }
    void setVolume(float v) { volume_.store(v); }
    void setAmbience(float rain, float night) {
        rain_.store(rain);
        night_.store(night);
    }
    void play(Cue cue);

private:
    struct Voice {
        float freq, phase, amp, decay, noise;
        int kind; // 0 = bell, 1 = thump
        int delay; // samples before it starts
    };
    static void callback(void* user, SDL_AudioStream* stream, int additional, int total);
    void render(float* out, int frames);

    SDL_AudioStream* stream_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<float> volume_{0.6f}, rain_{0.f}, night_{0.f};
    std::mutex mutex_;
    std::vector<Voice> pending_, voices_;
    std::vector<float> buffer_;
    // ambience state (audio thread only)
    float lp1_ = 0.f, lp2_ = 0.f, windLfo_ = 0.f, cricketPhase_ = 0.f, cricketGate_ = 0.f;
    unsigned rng_ = 12345u;
    int note_ = 0;
};

} // namespace pm
