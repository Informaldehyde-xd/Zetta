#pragma once

#include <cstdint>

namespace zb {

// FBO screen scaler: for old games that draw for a fixed screen size (N.O.V.A. 1, Zenonia, ...).
//
// The game is told the screen is width x height (eglQuerySurface, ANativeWindow_getWidth/Height and
// Android's Display.getWidth/getHeight all answer with that size). Everything the game draws to
// "the screen" (framebuffer 0) really goes into an off-screen framebuffer of exactly that size.
// Just before eglSwapBuffers the picture is scaled to the real surface, with black bars unless
// fbo_stretch=1. The Java side (FixedSurface) scales the touches the same way.
//
// Switches in zb-options.txt:
//   fbo_scale=1        turn it on (default 0). spoof_gameloft_res=1 does the same (older name).
//   fbo_width=1280     the size the game believes (default 1280)
//   fbo_height=720     (default 720)
//   fbo_stretch=0      1 = fill the whole screen without black bars (default 0)
//   fbo_linear=1       1 = smooth scaling, 0 = sharp pixels (default 1)
// Works for games that use OpenGL ES 2.0 or 3.0. A GLES 1 context is left alone.
struct FboConfig {
    bool on;
    int width;
    int height;
    bool stretch;
    bool linear;
};
const FboConfig& fbo_config();
inline bool fbo_scaler_enabled() { return fbo_config().on; }

// Filled in by the Android part (core/android/fbo_scaler.cpp), which is the only code that calls the
// real GL driver. Host tests leave them empty.
struct FboScalerHooks {
    // glBindFramebuffer(target, 0) -> returns true and the framebuffer to really bind.
    bool (*bind_redirect)(std::uint32_t target, std::uint32_t framebuffer, std::uint32_t* actual) = nullptr;
    // The game's context was made current: create its off-screen framebuffer the first time.
    void (*made_current)() = nullptr;
    // Right before eglSwapBuffers(surface): scale the off-screen picture onto the real surface.
    void (*present)(const void* surface) = nullptr;
};
FboScalerHooks& fbo_scaler_hooks();

// The window surface the game draws to (the newest one created).
void fbo_note_window_surface(const void* surface);
bool fbo_is_window_surface(const void* surface);
// eglQuerySurface answer for the game's window surface: replaces EGL_WIDTH / EGL_HEIGHT.
bool fbo_spoof_surface_size(const void* surface, std::int32_t attribute, std::int32_t* value);

}  // namespace zb
