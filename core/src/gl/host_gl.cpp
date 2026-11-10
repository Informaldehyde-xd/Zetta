#include "zb/host_gl.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

bool HostGl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    // FBO scaler: notice a new current context before any ES1/ES2 split.
    if (fbo_scaler_enabled()) {
        const FboScalerHooks& scaler = fbo_scaler_hooks();
        if (scaler.gl_call != nullptr) scaler.gl_call();
    }

    // OpenGL ES 1.x fixed-function names (old Unity 3.x games) live outside the GLES 2.0/3.0 ranges.
    if (!is_gl_host_call(index)) {
        // FBO scaler: redirect glBindFramebufferOES(…, 0) to our off-screen framebuffer too.
        if (fbo_scaler_enabled() && index == ZB_GL_HC_glBindFramebufferOES) {
            const FboScalerHooks& hooks = fbo_scaler_hooks();
            std::uint32_t actual = 0;
            if (hooks.bind_redirect != nullptr && hooks.bind_redirect(thread.regs()[0], thread.regs()[1], &actual)) {
                // fall through to the ES1 dispatcher with the redirected framebuffer
                thread.regs()[1] = actual;
            }
        }
        return gles1_handle_host_call(*this, index, thread);
    }
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
    if (index == ZB_GL_HC_glBindFramebuffer && fbo_scaler_enabled()) {
        const FboScalerHooks& hooks = fbo_scaler_hooks();
        std::uint32_t actual = 0;
        if (hooks.bind_redirect != nullptr && hooks.bind_redirect(call.arg(0), call.arg(1), &actual)) {
            backend_.glBindFramebuffer(call.arg(0), actual);
            return true;
        }
    }
    // -----------------------------------------

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
