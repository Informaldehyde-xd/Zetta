// FBO screen scaler, Android part. See zb/fbo_scaler.h for what it does and the switches.
//
// Only this file talks to the real GL driver for the scaler. All of it runs on the game's GL thread,
// inside the game's own context, so it saves every piece of GL state it touches and puts it back.

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "zb/fbo_scaler.h"
#include "zb/log.h"

namespace zb {

namespace {

struct Ctx {
    bool ok = false;
    GLuint fbo = 0;
    GLuint tex = 0;
    GLuint depth = 0;
    GLuint program = 0;
    GLuint vbo = 0;
    GLint u_tex = -1;
};

std::mutex g_mutex;
std::map<EGLContext, Ctx> g_contexts;

constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kReadFramebuffer = 0x8CA8;

bool is_es1() {
    const GLubyte* version = glGetString(GL_VERSION);
    if (version == nullptr) return false;
    const char* text = reinterpret_cast<const char*>(version);
    return std::strstr(text, "ES-CM") != nullptr || std::strstr(text, "OpenGL ES 1.") != nullptr;
}

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool make_program(Ctx& c) {
    static const char* const vertex =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
    static const char* const fragment =
        "precision mediump float;\n"
        "uniform sampler2D u_tex;\n"
        "varying vec2 v_uv;\n"
        "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return false;
    }
    c.program = glCreateProgram();
    glAttachShader(c.program, vs);
    glAttachShader(c.program, fs);
    glBindAttribLocation(c.program, 0, "a_pos");
    glBindAttribLocation(c.program, 1, "a_uv");
    glLinkProgram(c.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(c.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(c.program);
        c.program = 0;
        return false;
    }
    c.u_tex = glGetUniformLocation(c.program, "u_tex");
    return true;
}

// Builds the off-screen framebuffer, the scaling program and the quad. The game's own bindings are
// put back before it returns.
bool create_resources(Ctx& c) {
    const FboConfig& cfg = fbo_config();
    GLint prev_fb = 0, prev_rb = 0, prev_tex = 0, prev_active = 0, prev_abuf = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abuf);

    // colour: a texture, so the scaling pass can read it (works in a GLES 2.0 context too)
    glGenTextures(1, &c.tex);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    const GLint filter = cfg.linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cfg.width, cfg.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &c.fbo);
    glBindFramebuffer(kFramebuffer, c.fbo);
    glFramebufferTexture2D(kFramebuffer, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.tex, 0);

    // depth + stencil together (the game may use either); fall back to depth only
    glGenRenderbuffers(1, &c.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, c.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, cfg.width, cfg.height);
    glFramebufferRenderbuffer(kFramebuffer, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c.depth);
    glFramebufferRenderbuffer(kFramebuffer, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c.depth);
    bool complete = glCheckFramebufferStatus(kFramebuffer) == GL_FRAMEBUFFER_COMPLETE;
    if (!complete) {
        glFramebufferRenderbuffer(kFramebuffer, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, cfg.width, cfg.height);
        glFramebufferRenderbuffer(kFramebuffer, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c.depth);
        complete = glCheckFramebufferStatus(kFramebuffer) == GL_FRAMEBUFFER_COMPLETE;
    }

    bool ok = complete && make_program(c);
    if (ok) {
        static const GLfloat quad[16] = {
            -1.f, -1.f, 0.f, 0.f,
             1.f, -1.f, 1.f, 0.f,
            -1.f,  1.f, 0.f, 1.f,
             1.f,  1.f, 1.f, 1.f,
        };
        glGenBuffers(1, &c.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    }

    glBindFramebuffer(kFramebuffer, static_cast<GLuint>(prev_fb));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prev_rb));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_abuf));
    return ok;
}

bool redirect(std::uint32_t target, std::uint32_t framebuffer, std::uint32_t* actual) {
    if (framebuffer != 0) return false;
    if (target != kFramebuffer && target != kDrawFramebuffer && target != kReadFramebuffer) return false;
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_contexts.find(context);
    if (found == g_contexts.end() || !found->second.ok) return false;
    *actual = found->second.fbo;
    return true;
}

void made_current() {
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;
    if (!fbo_is_window_surface(eglGetCurrentSurface(EGL_DRAW))) return;  // loader / pbuffer contexts: leave alone
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_contexts.find(context);
    if (found != g_contexts.end() && (!found->second.ok || glIsFramebuffer(found->second.fbo))) return;
    Ctx c;
    if (is_es1()) {
        g_contexts[context] = c;
        log("fbo scaler: GLES 1 context, scaler not used");
        return;
    }
    GLint prev_fb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    c.ok = create_resources(c);
    g_contexts[context] = c;
    if (!c.ok) {
        log("fbo scaler: could not create the off-screen framebuffer");
        return;
    }
    const FboConfig& cfg = fbo_config();
    // A new context draws to "the screen" until the game says otherwise: that is now our framebuffer.
    if (prev_fb == 0) {
        glBindFramebuffer(kFramebuffer, c.fbo);
        glViewport(0, 0, cfg.width, cfg.height);
        glScissor(0, 0, cfg.width, cfg.height);
    }
    log("fbo scaler: %dx%d off-screen framebuffer ready (fbo %u)", cfg.width, cfg.height, c.fbo);
}

struct Attrib {
    GLint enabled = 0, size = 4, type = GL_FLOAT, normalized = 0, stride = 0, buffer = 0;
    void* pointer = nullptr;
};

void save_attrib(GLuint index, Attrib& a) {
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
}

void restore_attrib(GLuint index, const Attrib& a) {
    // A disabled array with no buffer holds nothing the game needs: it sets the pointer before use.
    if (a.buffer != 0 || a.enabled) {
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
        glVertexAttribPointer(index, a.size, static_cast<GLenum>(a.type), static_cast<GLboolean>(a.normalized),
                              a.stride, a.pointer);
    }
    if (a.enabled) glEnableVertexAttribArray(index);
    else glDisableVertexAttribArray(index);
}

void present(const void* surface) {
    const EGLContext context = eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) return;
    const EGLDisplay display = eglGetCurrentDisplay();
    if (eglGetCurrentSurface(EGL_DRAW) != const_cast<EGLSurface>(surface)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto found = g_contexts.find(context);
    if (found == g_contexts.end() || !found->second.ok) return;
    const Ctx& c = found->second;

    EGLint real_w = 0, real_h = 0;
    eglQuerySurface(display, const_cast<EGLSurface>(surface), EGL_WIDTH, &real_w);
    eglQuerySurface(display, const_cast<EGLSurface>(surface), EGL_HEIGHT, &real_h);
    if (real_w <= 0 || real_h <= 0) return;

    const FboConfig& cfg = fbo_config();
    int dx = 0, dy = 0, dw = real_w, dh = real_h;
    if (!cfg.stretch) {
        const double scale = std::min(static_cast<double>(real_w) / cfg.width, static_cast<double>(real_h) / cfg.height);
        dw = static_cast<int>(cfg.width * scale + 0.5);
        dh = static_cast<int>(cfg.height * scale + 0.5);
        dx = (real_w - dw) / 2;
        dy = (real_h - dh) / 2;
    }
    const bool bars = dw != real_w || dh != real_h;

    // ---- save the game's state ----
    GLint prev_fb = 0, prev_program = 0, prev_abuf = 0, prev_active = 0, prev_tex = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLboolean mask[4] = {1, 1, 1, 1};
    GLfloat clear[4] = {0.f, 0.f, 0.f, 0.f};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fb);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    static const GLenum kCaps[] = {GL_SCISSOR_TEST, GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
                                   GL_CULL_FACE, GL_DITHER, GL_SAMPLE_ALPHA_TO_COVERAGE};
    constexpr int kCapCount = sizeof kCaps / sizeof kCaps[0];
    GLboolean was[kCapCount];
    for (int i = 0; i < kCapCount; ++i) was[i] = glIsEnabled(kCaps[i]);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    Attrib attribs[2];
    save_attrib(0, attribs[0]);
    save_attrib(1, attribs[1]);

    // ---- scale the picture onto the real surface ----
    glBindFramebuffer(kFramebuffer, 0);
    for (int i = 0; i < kCapCount; ++i) glDisable(kCaps[i]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (bars) {
        glViewport(0, 0, real_w, real_h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glViewport(dx, dy, dw, dh);
    glUseProgram(c.program);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    glUniform1i(c.u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<const void*>(8));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ---- put the game's state back ----
    restore_attrib(0, attribs[0]);
    restore_attrib(1, attribs[1]);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_abuf));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
    glActiveTexture(static_cast<GLenum>(prev_active));
    glUseProgram(static_cast<GLuint>(prev_program));
    for (int i = 0; i < kCapCount; ++i) {
        if (was[i]) glEnable(kCaps[i]);
    }
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glBindFramebuffer(kFramebuffer, static_cast<GLuint>(prev_fb));
    // Anything our own calls left in the error queue is not the game's.
    while (glGetError() != GL_NO_ERROR) {
    }
}

struct Installer {
    Installer() {
        FboScalerHooks& hooks = fbo_scaler_hooks();
        hooks.bind_redirect = redirect;
        hooks.made_current = made_current;
        hooks.present = present;
    }
} g_installer;

}  // namespace

}  // namespace zb
