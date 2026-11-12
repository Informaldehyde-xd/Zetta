#include "zb/host_gl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

#include "zb/options.h"
#include "zb/fbo_scaler.h"
#include "gl/gl_diagnostics.h"
#include "gl/host_gles1.h"
#include "zb/gl_hostcalls.h"
#include "zb/options.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

// Ensure GL constants are defined if not included
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

namespace zb {

HostGl::Call::Call(HostGl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]} {
    // Capture arguments before installing the default void/zero result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostGl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address = static_cast<std::uint64_t>(thread_.regs()[13]) + 4u * (position - 4u);
    if (address > UINT32_MAX) {
        fail(kGlInvalidValue, "argument address overflowed the guest stack");
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime().memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        fail(kGlInvalidValue, "argument is not on a readable guest stack");
        return 0;
    }
    std::uint32_t value;
    std::memcpy(&value, source, sizeof value);
    return value;
}

void HostGl::Call::fail(GLenum error, const char* reason) {
    if (!valid_) return;
    valid_ = false;
    host_.reject(*this, error, reason);
}

void HostGl::reject(Call& call, GLenum error, const char* reason) {
    const char* name = gl_host_call_name(call.index());
    log("GLES %s rejected: %s", name, reason);
    // Guests rarely call glGetError, so a rejection is otherwise silent (a texture that never
    // got its pixels renders black). Count them, and keep the first few with their arguments.
    static std::atomic<std::uint64_t> rejections{0};
    const std::uint64_t number = ++rejections;
    runtime_report().note_gl_detail("rejections", std::to_string(number), true);
    if (number <= 4) {
        char text[256];
        std::snprintf(text, sizeof text, "%s: %s args=0x%x,0x%x,0x%x,0x%x", name, reason, call.arg(0),
                      call.arg(1), call.arg(2), call.arg(3));
        runtime_report().note_gl_detail("rejection-" + std::to_string(number), text, false);
    }
    backend_.set_error(error);
}

#include "gen/gl_manual.inc"

#include "gen/gl_dispatch.inc"

namespace {

bool gl_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ZB_GL_TRACE");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

// --- DXT5 DECOMPRESSOR ---
// Decompresses a 0x83F3 (DXT5/S3TC) texture to raw RGBA8888.
void decompress_dxt5(const std::uint8_t* data, std::uint8_t* out, int width, int height) {
    int blocks_w = (width + 3) / 4;
    int blocks_h = (height + 3) / 4;
    
    for (int by = 0; by < blocks_h; ++by) {
        for (int bx = 0; bx < blocks_w; ++bx) {
            const std::uint8_t* block = data + (by * blocks_w + bx) * 16;
            
            // --- ALPHA BLOCK (8 bytes) ---
            std::uint8_t a0 = block[0];
            std::uint8_t a1 = block[1];
            std::uint8_t alpha[8];
            alpha[0] = a0;
            alpha[1] = a1;
            if (a0 > a1) {
                alpha[2] = (6 * a0 + 1 * a1) / 7;
                alpha[3] = (5 * a0 + 2 * a1) / 7;
                alpha[4] = (4 * a0 + 3 * a1) / 7;
                alpha[5] = (3 * a0 + 4 * a1) / 7;
                alpha[6] = (2 * a0 + 5 * a1) / 7;
                alpha[7] = (1 * a0 + 6 * a1) / 7;
            } else {
                alpha[2] = (4 * a0 + 1 * a1) / 5;
                alpha[3] = (3 * a0 + 2 * a1) / 5;
                alpha[4] = (2 * a0 + 3 * a1) / 5;
                alpha[5] = (1 * a0 + 4 * a1) / 5;
                alpha[6] = 0;
                alpha[7] = 255;
            }
            
            std::uint64_t alpha_bits = 0;
            for (int i = 0; i < 6; ++i) alpha_bits |= (std::uint64_t)block[2+i] << (i*8);
            
            // --- COLOR BLOCK (8 bytes) ---
            std::uint16_t c0 = block[8] | (block[9] << 8);
            std::uint16_t c1 = block[10] | (block[11] << 8);
            std::uint32_t color_bits = block[12] | (block[13] << 8) | (block[14] << 16) | (block[15] << 24);
            
            std::uint8_t r[4], g[4], b[4];
            auto unpack565 = [](std::uint16_t c, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
                r = ((c >> 11) & 0x1F) << 3; r |= r >> 5;
                g = ((c >> 5) & 0x3F) << 2; g |= g >> 6;
                b = (c & 0x1F) << 3; b |= b >> 5;
            };
            unpack565(c0, r[0], g[0], b[0]);
            unpack565(c1, r[1], g[1], b[1]);
            
            if (c0 > c1) {
                r[2] = (2*r[0] + r[1]) / 3; g[2] = (2*g[0] + g[1]) / 3; b[2] = (2*b[0] + b[1]) / 3;
                r[3] = (r[0] + 2*r[1]) / 3; g[3] = (g[0] + 2*g[1]) / 3; b[3] = (b[0] + 2*b[1]) / 3;
            } else {
                r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
                r[3] = 0; g[3] = 0; b[3] = 0;
            }
            
            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    int x = bx * 4 + px;
                    int y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    
                    int pixel_index = py * 4 + px;
                    int a_idx = (alpha_bits >> (pixel_index * 3)) & 0x7;
                    int c_idx = (color_bits >> (pixel_index * 2)) & 0x3;
                    
                    int out_idx = (y * width + x) * 4;
                    out[out_idx + 0] = r[c_idx];
                    out[out_idx + 1] = g[c_idx];
                    out[out_idx + 2] = b[c_idx];
                    out[out_idx + 3] = alpha[a_idx];
                }
            }
        }
    }
}
}  // namespace

std::atomic<std::uint32_t>& gl_state_generation() {
    static std::atomic<std::uint32_t> generation{0};
    return generation;
}

// ---------------------------------------------------------------------------------------------
// Duplicate GL state calls (gl_skip_dup=1, default on). Old engines set the same state again and
// again (bind the buffer that is already bound, enable what is already enabled...). Each such call
// still costs a guest -> host transition, so a repeat of the last value is answered here and never
// reaches the driver. The cache starts "unknown" for every context and is dropped whenever the
// current EGL context changes, when something that can change the state behind it is called
// (delete of a texture / buffer / program, link, vertex array objects), so it never skips a call
// that would change something.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kDupUnknown = 0xFFFFFFFFu;
enum DupKind { kDupProgram, kDupTexture, kDupBuffer, kDupCaps, kDupDepth, kDupViewport, kDupBlend, kDupKinds };
constexpr const char* kDupNames[kDupKinds] = {"use-program", "bind-texture", "bind-buffer", "enable",
                                              "depth", "viewport", "blend/cull"};

struct DupCache {
    std::uintptr_t context = 0;
    std::uint32_t program = kDupUnknown;
    std::uint32_t array_buffer = kDupUnknown;
    std::uint32_t element_buffer = kDupUnknown;
    std::uint32_t unit = kDupUnknown;  // GL_TEXTURE0 + n, unknown until glActiveTexture was seen
    std::uint32_t texture[32][2];      // [unit][2D, cube map]
    std::int8_t caps[9];               // 0 off, 1 on, 2 unknown
    std::uint32_t depth_mask = kDupUnknown, depth_func = kDupUnknown, cull = kDupUnknown;
    std::uint32_t blend_src = kDupUnknown, blend_dst = kDupUnknown, color_mask = kDupUnknown;
    std::uint32_t viewport[4] = {kDupUnknown, kDupUnknown, kDupUnknown, kDupUnknown};
    bool vao_used = false;
    std::uint64_t total[kDupKinds] = {};
    std::uint64_t skipped[kDupKinds] = {};
    std::uint64_t since_report = 0;

    void reset(std::uintptr_t new_context) {
        context = new_context;
        program = array_buffer = element_buffer = unit = kDupUnknown;
        forget_textures();
        std::memset(caps, 2, sizeof caps);
        depth_mask = depth_func = cull = blend_src = blend_dst = color_mask = kDupUnknown;
        for (std::uint32_t& value : viewport) value = kDupUnknown;
        vao_used = false;
    }
    void forget_textures() { std::memset(texture, 0xFF, sizeof texture); }
};

int dup_cap_index(std::uint32_t cap) {
    switch (cap) {
        case 0x0BE2: return 0;  // BLEND
        case 0x0B44: return 1;  // CULL_FACE
        case 0x0B71: return 2;  // DEPTH_TEST
        case 0x0C11: return 3;  // SCISSOR_TEST
        case 0x0B90: return 4;  // STENCIL_TEST
        case 0x0BD0: return 5;  // DITHER
        case 0x8037: return 6;  // POLYGON_OFFSET_FILL
        case 0x809E: return 7;  // SAMPLE_ALPHA_TO_COVERAGE
        case 0x80A0: return 8;  // SAMPLE_COVERAGE
        default: return -1;
    }
}

// True when the call repeats what the context already has and can be dropped.
bool dup_same(DupCache& cache, DupKind kind, std::uint32_t& slot, std::uint32_t value) {
    ++cache.total[kind];
    if (slot == value) {
        ++cache.skipped[kind];
        return true;
    }
    slot = value;
    return false;
}

bool dup_check(DupCache& cache, std::uint32_t index, const std::uint32_t a[4]) {
    switch (index) {
        case ZB_GL_HC_glUseProgram:
            return dup_same(cache, kDupProgram, cache.program, a[0]);
        case ZB_GL_HC_glActiveTexture:
            cache.unit = a[0];  // always passed on; only remembered
            return false;
        case ZB_GL_HC_glBindTexture: {
            const int slot = a[0] == 0x0DE1 ? 0 : (a[0] == 0x8513 ? 1 : -1);
            if (slot < 0 || cache.unit == kDupUnknown || cache.unit < 0x84C0u || cache.unit >= 0x84C0u + 32u) return false;
            return dup_same(cache, kDupTexture, cache.texture[cache.unit - 0x84C0u][slot], a[1]);
        }
        case ZB_GL_HC_glBindBuffer:
            if (a[0] == 0x8892) return dup_same(cache, kDupBuffer, cache.array_buffer, a[1]);  // ARRAY_BUFFER
            // The element buffer belongs to the vertex array object in GLES 3; only cache it while
            // the game has never used one.
            if (a[0] == 0x8893 && !cache.vao_used) return dup_same(cache, kDupBuffer, cache.element_buffer, a[1]);
            return false;
        case ZB_GL_HC_glEnable:
        case ZB_GL_HC_glDisable: {
            const int slot = dup_cap_index(a[0]);
            if (slot < 0) return false;
            const std::int8_t wanted = index == ZB_GL_HC_glEnable ? 1 : 0;
            ++cache.total[kDupCaps];
            if (cache.caps[slot] == wanted) {
                ++cache.skipped[kDupCaps];
                return true;
            }
            cache.caps[slot] = wanted;
            return false;
        }
        case ZB_GL_HC_glDepthMask:
            return dup_same(cache, kDupDepth, cache.depth_mask, a[0] & 0xFFu);
        case ZB_GL_HC_glDepthFunc:
            return dup_same(cache, kDupDepth, cache.depth_func, a[0]);
        case ZB_GL_HC_glCullFace:
            return dup_same(cache, kDupBlend, cache.cull, a[0]);
        case ZB_GL_HC_glBlendFunc: {
            ++cache.total[kDupBlend];
            if (cache.blend_src == a[0] && cache.blend_dst == a[1]) {
                ++cache.skipped[kDupBlend];
                return true;
            }
            cache.blend_src = a[0];
            cache.blend_dst = a[1];
            return false;
        }
        case ZB_GL_HC_glColorMask:
            return dup_same(cache, kDupDepth, cache.color_mask,
                            (a[0] & 1u) | ((a[1] & 1u) << 1) | ((a[2] & 1u) << 2) | ((a[3] & 1u) << 3));
        case ZB_GL_HC_glViewport: {
            ++cache.total[kDupViewport];
            if (cache.viewport[0] == a[0] && cache.viewport[1] == a[1] && cache.viewport[2] == a[2] &&
                cache.viewport[3] == a[3]) {
                ++cache.skipped[kDupViewport];
                return true;
            }
            for (int i = 0; i < 4; ++i) cache.viewport[i] = a[i];
            return false;
        }
        // Things that can change the cached state without a matching call:
        case ZB_GL_HC_glDeleteTextures:
            cache.forget_textures();
            return false;
        case ZB_GL_HC_glDeleteBuffers:
            cache.array_buffer = cache.element_buffer = kDupUnknown;
            return false;
        case ZB_GL_HC_glDeleteProgram:
        case ZB_GL_HC_glLinkProgram:
            cache.program = kDupUnknown;
            return false;
        case ZB_GL_HC_glBindVertexArray:
        case ZB_GL_HC_glBindVertexArrayOES:
            cache.vao_used = true;
            cache.element_buffer = kDupUnknown;
            return false;
        default:
            return false;
    }
}

void dup_report(DupCache& cache) {
    std::uint64_t total = 0, skipped = 0;
    std::string detail;
    for (int k = 0; k < kDupKinds; ++k) {
        total += cache.total[k];
        skipped += cache.skipped[k];
        detail += std::string(k ? ", " : "") + kDupNames[k] + " " + std::to_string(cache.skipped[k]) + "/" +
                  std::to_string(cache.total[k]);
    }
    runtime_report().note_gl_detail("gl-dedupe",
                                    "skipped " + std::to_string(skipped) + " of " + std::to_string(total) +
                                        " state calls (" + detail + ")",
                                    true);
}

}  // namespace

// Which viewport / scissor rectangles the game really sets (and how often): written to the runtime report as
// "gl-viewports". Cheap: a handful of integer compares per glViewport / glScissor.
namespace {
struct RectRecord {
    char kind;
    int x, y, w, h;
    std::uint64_t count;
};
RectRecord g_rects[24];
int g_rect_count = 0;
std::uint64_t g_rect_calls = 0;

void record_rect(char kind, int x, int y, int w, int h) {
    int slot = -1;
    for (int i = 0; i < g_rect_count; ++i) {
        const RectRecord& r = g_rects[i];
        if (r.kind == kind && r.x == x && r.y == y && r.w == w && r.h == h) {
            slot = i;
            break;
        }
    }
    bool is_new = false;
    if (slot < 0 && g_rect_count < 24) {
        slot = g_rect_count++;
        g_rects[slot] = {kind, x, y, w, h, 0};
        is_new = true;
    }
    if (slot >= 0) ++g_rects[slot].count;
    if (is_new || (++g_rect_calls & 0x3FFF) == 0) {
        std::string text;
        for (int i = 0; i < g_rect_count; ++i) {
            const RectRecord& r = g_rects[i];
            char part[96];
            std::snprintf(part, sizeof part, "%s%c %d,%d %dx%d x%llu", i ? "; " : "", r.kind, r.x, r.y, r.w, r.h,
                          static_cast<unsigned long long>(r.count));
            text += part;
        }
        runtime_report().note_gl_detail("gl-viewports", text, true);
    }
}
}  // namespace

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    // A game that calls GL from a thread of its own before any context is current there (Marmalade,
    // Peggle) would get NULL from the driver and crash. Until this thread has a context, make the
    // game's own one current here. Games that already have one never pay for more than this latch.
    if (ensure_context_) {
        static thread_local bool has_context = false;
        if (!has_context) {
            if (egl_context() != 0 || ensure_context_()) has_context = true;
        }
    }
    if (index == ZB_GL_HC_glViewport || index == ZB_GL_HC_glScissor) {
        record_rect(index == ZB_GL_HC_glViewport ? 'V' : 'S', static_cast<int>(call.arg(0)), static_cast<int>(call.arg(1)),
                    static_cast<int>(call.arg(2)), static_cast<int>(call.arg(3)));
    }
    // OpenGL ES 1.x fixed-function names (old Unity 3.x games) live outside the GLES 2.0/3.0 ranges.
    if (!is_gl_host_call(index)) return gles1_handle_host_call(*this, index, thread);
    const char* name = gl_host_call_name(index);
    if (gl_trace_enabled()) {
        log("GLES trace: %s(0x%x, 0x%x, 0x%x, 0x%x)", name, thread.regs()[0], thread.regs()[1],
            thread.regs()[2], thread.regs()[3]);
    }
    
    thread_local const std::uint64_t host_tid = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    if (!gl_first_call_noted_) {
        gl_first_call_noted_ = true;
        runtime_report().note_gl_calls(name, host_tid, 1);
    } else if (++gl_calls_pending_ >= 256) {
        runtime_report().note_gl_calls(name, host_tid, gl_calls_pending_);
        gl_calls_pending_ = 0;
    }
    const std::uint64_t egl_generation = runtime_report().egl_current_generation();
    if (egl_generation != gl_thread_sampled_generation_) {
        gl_thread_sampled_generation_ = egl_generation;
        runtime_report().note_gl_thread(host_tid);
    }
    if (!egl_context_checked_) {
        egl_context_checked_ = true;
        if (egl_context_probe_) runtime_report().note_gl_egl_context(egl_context() != 0);
    }
    
    Call call(*this, thread, index);

    // --- INTERCEPT DXT5 TEXTURES (0x83F3) ---
    if (std::strcmp(name, "glCompressedTexImage2D") == 0) {
        GLenum target = call.arg(0);
        GLint level = call.arg(1);
        GLenum internalformat = call.arg(2);
        GLsizei width = call.arg(3);
        GLsizei height = call.arg(4);
        GLint border = call.arg(5);
        GLsizei imageSize = call.arg(6);
        std::uint32_t data_addr = call.arg(7); // Guest pointer (32-bit)

        if (internalformat == 0x83F3 && data_addr != 0 && width > 0 && height > 0) {
            const std::uint8_t* data = runtime().memory().host_ptr(data_addr, imageSize, kPageRead);
            if (data) {
                // Decompress to raw RGBA
                std::vector<std::uint8_t> rgba(width * height * 4);
                decompress_dxt5(data, rgba.data(), width, height);

                // Send uncompressed RGBA to Adreno (bypasses the unsupported DXT5 format)
                backend_.glTexImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                return true;
            }
        }
    }
    // -----------------------------------------
    if (fbo_scaler_enabled()) {
        const FboScalerHooks& scaler = fbo_scaler_hooks();
        if (scaler.gl_call != nullptr) scaler.gl_call();
    }
    // --- FBO SCREEN SCALER (fbo_scale=1): the game's "screen" (framebuffer 0) is our off-screen one ---
    if (index == ZB_GL_HC_glBindFramebuffer && fbo_scaler_enabled()) {
        const FboScalerHooks& hooks = fbo_scaler_hooks();
        std::uint32_t actual = 0;
        if (hooks.bind_redirect != nullptr && hooks.bind_redirect(call.arg(0), call.arg(1), &actual)) {
            backend_.glBindFramebuffer(call.arg(0), actual);
            return true;
        }
    }
    // -----------------------------------------

    // --- duplicate state calls (gl_skip_dup=1) ---
    {
        static const bool skip_dup = option_enabled("gl_skip_dup", true);
        if (skip_dup) {
            static thread_local DupCache cache;
            const std::uintptr_t context = egl_context();
            // A game that re-creates its GL context in Java (GLSurfaceView) can get the same context
            // address back. The native entry points that mean "new surface / context" bump this number.
            static thread_local std::uint32_t seen_generation = 0;
            const std::uint32_t generation = gl_state_generation().load(std::memory_order_relaxed);
            if (generation != seen_generation) {
                seen_generation = generation;
                cache.reset(0);
            }
            if (context == 0) {
                cache.reset(0);  // cannot tell which context this is: do not cache
            } else {
                if (context != cache.context) cache.reset(context);
                const std::uint32_t args[4] = {call.arg(0), call.arg(1), call.arg(2), call.arg(3)};
                if (dup_check(cache, index, args)) {
                    if (++cache.since_report >= 20000) {
                        cache.since_report = 0;
                        dup_report(cache);
                    }
                    return true;
                }
                if (++cache.since_report >= 20000) {
                    cache.since_report = 0;
                    dup_report(cache);
                }
            }
        }
    }

    if (gl_diagnostics_enabled()) gl_diagnose_before(*this, call);
    if (dispatch(call)) {
        if (call.valid() && gl_diagnostics_enabled()) gl_diagnose(*this, call);
        if (call.valid() && std::strcmp(name, "glGetError") == 0) {
            const std::uint32_t error = thread.regs()[0];
            if (error != 0) runtime_report().note_gl_error(name, error);
        }
        return true;
    }
    log("GLES host call index %u has no generated handler", index);
    std::abort();
}

}  // namespace zb
