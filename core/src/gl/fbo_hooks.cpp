#include "zb/fbo_scaler.h"

#include <algorithm>
#include <atomic>

#include "zb/options.h"

namespace zb {

namespace {
std::atomic<const void*> g_window_surface{nullptr};

int clamp_size(int value, int fallback) {
    if (value <= 0) return fallback;
    return std::min(std::max(value, 160), 4096);
}
}  // namespace

const FboConfig& fbo_config() {
    static const FboConfig config = [] {
        FboConfig c{};
        c.on = option_enabled("fbo_scale", false) || option_enabled("spoof_gameloft_res", false);
        c.width = clamp_size(option_number("fbo_width", 1280), 1280);
        c.height = clamp_size(option_number("fbo_height", 720), 720);
        c.stretch = option_enabled("fbo_stretch", false);
        c.linear = option_enabled("fbo_linear", true);
        return c;
    }();
    return config;
}

bool fbo_display_spoof(int* width, int* height) {
    struct Spoof {
        bool on;
        int width;
        int height;
    };
    static const Spoof spoof = [] {
        Spoof result{};
        const FboConfig& fbo = fbo_config();
        if (fbo.on) {
            result = {true, fbo.width, fbo.height};
        } else {
            const int window_width = option_number("window_width", 0);
            const int window_height = option_number("window_height", 0);
            if (window_width >= 160 && window_height >= 160 && option_enabled("window_spoof_display", true)) {
                result = {true, clamp_size(window_width, 1280), clamp_size(window_height, 720)};
            }
        }
        return result;
    }();
    if (!spoof.on) return false;
    *width = spoof.width;
    *height = spoof.height;
    return true;
}

FboScalerHooks& fbo_scaler_hooks() {
    static FboScalerHooks hooks;
    return hooks;
}

void fbo_note_window_surface(const void* surface) {
    if (surface != nullptr) g_window_surface.store(surface, std::memory_order_relaxed);
}

bool fbo_is_window_surface(const void* surface) {
    return surface != nullptr && surface == g_window_surface.load(std::memory_order_relaxed);
}

bool fbo_spoof_surface_size(const void* surface, std::int32_t attribute, std::int32_t* value) {
    if (!fbo_scaler_enabled() || value == nullptr || !fbo_is_window_surface(surface)) return false;
    if (attribute == 0x3057) {  // EGL_WIDTH
        *value = fbo_config().width;
        return true;
    }
    if (attribute == 0x3056) {  // EGL_HEIGHT
        *value = fbo_config().height;
        return true;
    }
    return false;
}

}  // namespace zb
