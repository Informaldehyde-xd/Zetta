#pragma once

#include <cstdint>

#include "zb/guest_thread.h"

namespace zb {

class HostGl;

// OpenGL ES 1.1 fixed-function calls (libGLESv1_CM.so, host call indices 492-578) that old
// Unity 3.x games use when the engine creates an "OpenGL ES 1.x context". The calls that
// GLES 2.0 shares by name (glEnable, glBindTexture, glDrawElements, ...) are served by HostGl as
// before; this handles only the names that exist in ES 1.x alone. Returns false for an index
// it does not handle.
bool gles1_handle_host_call(HostGl& host, std::uint32_t index, GuestThread& thread);

// Resolves a GL entry point of the current driver by name (eglGetProcAddress on Android). The
// default definition returns nullptr; core/android/gl_driver_backend.cpp overrides it.
void* gl_resolve_proc(const char* name);

}  // namespace zb
