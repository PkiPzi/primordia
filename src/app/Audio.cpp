#include "app/Audio.hpp"

#include <SDL3/SDL.h>

#include <cmath>

namespace pm {

namespace {
constexpr int RATE = 48000;
constexpr float TWO_PI = 6.28318530718f;
// A minor pentatonic across two octaves: chimes never clash with each other.
constexpr float PENTA[] = {220.0f, 261.6f, 293.7f, 329.6f, 392.0f, 440.0f, 523.3f, 587.3f, 659.3f, 784.0f};
} // namespace

bool Audio::init() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, RATE};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, callback, this);
    if (!stream_) return false;
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void Audio::shutdown() {
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

void Audio::play(Cue cue) {
    if (!enabled_.load() || !stream_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (pending_.size() > 16) return;
    switch (cue) {
        case Cue::Speciation:
            note_ = (note_ + 3 + int(rng_ % 4)) % 10;
            rng_ = rng_ * 1664525u + 1013904223u;
            pending_.push_back({PENTA[note_], 0.f, 0.16f, 0.99985f, 0.f, 0, 0});
            break;
        case Cue::Extinction:
            pending_.push_back({PENTA[0] * 0.5f, 0.f, 0.12f, 0.99992f, 0.f, 0, 0});
            break;
        case Cue::Catastrophe:
            pending_.push_back({48.f, 0.f, 0.5f, 0.99975f, 0.35f, 1, 0});
            break;
        case Cue::Achievement:
            for (int k = 0; k < 3; ++k) pending_.push_back({PENTA[4 + k * 2], 0.f, 0.12f, 0.99988f, 0.f, 0, k * 4800});
            break;
    }
}

void Audio::callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* self = static_cast<Audio*>(user);
    const int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0) return;
    self->buffer_.resize(size_t(frames) * 2);
    self->render(self->buffer_.data(), frames);
    SDL_PutAudioStreamData(stream, self->buffer_.data(), frames * int(sizeof(float) * 2));
}

void Audio::render(float* out, int frames) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (Voice& v : pending_) voices_.push_back(v);
        pending_.clear();
    }
    const bool on = enabled_.load();
    const float vol = volume_.load();
    const float rain = rain_.load(), night = night_.load();
    auto noise = [&]() {
        rng_ = rng_ * 1664525u + 1013904223u;
        return float(rng_ >> 9) * (1.f / 4194304.f) - 1.f;
    };
    for (int i = 0; i < frames; ++i) {
        float s = 0.f;
        if (on) {
            // Wind: doubly low-passed noise with a slow gusting LFO, stronger under rain.
            windLfo_ += 0.25f / RATE;
            if (windLfo_ > 1.f) windLfo_ -= 1.f;
            float gust = 0.6f + 0.4f * std::sin(TWO_PI * windLfo_) * std::sin(TWO_PI * windLfo_ * 2.7f);
            lp1_ += 0.02f * (noise() - lp1_);
            lp2_ += 0.02f * (lp1_ - lp2_);
            s += lp2_ * (0.35f + 0.9f * rain) * gust;
            // Crickets at night: gated high chirps.
            if (night > 0.05f) {
                cricketGate_ += 1.f / RATE * 14.f;
                if (cricketGate_ > 1.f) cricketGate_ -= 1.f;
                float gate = cricketGate_ < 0.35f ? std::sin(cricketGate_ / 0.35f * 3.14159f) : 0.f;
                cricketPhase_ += 4200.f / RATE;
                if (cricketPhase_ > 1.f) cricketPhase_ -= 1.f;
                s += std::sin(TWO_PI * cricketPhase_) * gate * gate * 0.012f * night;
            }
            for (Voice& v : voices_) {
                if (v.delay > 0) { --v.delay; continue; }
                v.phase += v.freq / RATE;
                if (v.phase > 1.f) v.phase -= 1.f;
                float tone = v.kind == 0
                                 ? std::sin(TWO_PI * v.phase) + 0.35f * std::sin(TWO_PI * v.phase * 2.f) +
                                       0.12f * std::sin(TWO_PI * v.phase * 3.01f)
                                 : std::sin(TWO_PI * v.phase) + v.noise * noise();
                s += tone * v.amp;
                v.amp *= v.decay;
                v.noise *= 0.9995f;
            }
        }
        s *= vol;
        s = s / (1.f + std::fabs(s)); // soft clip
        out[i * 2] = s;
        out[i * 2 + 1] = s;
    }
    for (size_t k = 0; k < voices_.size();)
        if (voices_[k].amp < 0.0005f && voices_[k].delay <= 0) {
            voices_[k] = voices_.back();
            voices_.pop_back();
        } else {
            ++k;
        }
}

} // namespace pm
