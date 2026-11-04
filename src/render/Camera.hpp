#pragma once
#include <cmath>

#include "core/Math.hpp"

namespace pm {

// 2D camera. Screen space is framebuffer pixels, y down. The camera centre is
// drawn at `viewCenter` (the middle of the free area between docked panels).
struct Camera {
    Vec2 center{2048.f, 2048.f};
    float zoom = 0.25f; // pixels per world unit
    Vec2 targetCenter{2048.f, 2048.f};
    float targetZoom = 0.25f;
    Vec2 viewCenter{800.f, 480.f};
    Vec2 viewSize{1600.f, 960.f};
    Vec2 fbSize{1600.f, 960.f};

    Vec2 worldToScreen(Vec2 w) const { return (w - center) * zoom + viewCenter; }
    Vec2 screenToWorld(Vec2 s) const { return (s - viewCenter) / zoom + center; }

    void zoomAt(Vec2 screen, float factor) {
        float nz = clampf(targetZoom * factor, 0.02f, 12.f);
        // Keep the world point under the cursor fixed (relative to the target state).
        Vec2 w = (screen - viewCenter) / targetZoom + targetCenter;
        targetCenter = w - (screen - viewCenter) / nz;
        targetZoom = nz;
    }
    void panPixels(Vec2 delta) {
        targetCenter -= delta / targetZoom;
        center -= delta / zoom;
    }
    void fit(float worldW, float worldH) {
        targetCenter = {worldW * 0.5f, worldH * 0.5f};
        targetZoom = std::min(viewSize.x / worldW, viewSize.y / worldH) * 0.95f;
    }
    void update(float dt) {
        float k = 1.f - std::exp(-dt * 14.f);
        center = center + (targetCenter - center) * k;
        zoom = std::exp(std::log(zoom) + (std::log(targetZoom) - std::log(zoom)) * k);
    }
};

} // namespace pm
