#include "gl/host_gles1.h"

#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

#include "zb/guest_memory.h"
#include "zb/host_gl.h"
#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

__attribute__((weak)) void* gl_resolve_proc(const char*) { return nullptr; }

namespace {

struct HostCallName {
    std::uint32_t index;
    const char* library;
    const char* name;
};

constexpr HostCallName kNames[] = {
#include "../gen/hostcalls.inc"
};

enum class Op : std::uint8_t {
    None,
    AlphaFunc,
    ClientActiveTexture,
    Color4f,
    ColorPointer,
    DisableClientState,
    EnableClientState,
    Fogf,
    Fogfv,
    Fogx,
    LightModelf,
    LightModelfv,
    Lightf,
    Lightfv,
    LoadIdentity,
    LoadMatrixf,
    Materialf,
    Materialfv,
    MatrixMode,
    NormalPointer,
    ShadeModel,
    TexCoordPointer,
    TexEnvf,
    TexEnvfv,
    TexEnvi,
    VertexPointer,
    BindFramebufferOES,
    BindRenderbufferOES,
    CheckFramebufferStatusOES,
    DeleteFramebuffersOES,
    DeleteRenderbuffersOES,
    FramebufferRenderbufferOES,
    FramebufferTexture2DOES,
    GenFramebuffersOES,
    GenRenderbuffersOES,
    GenerateMipmapOES,
    GetFramebufferAttachmentParameterivOES,
    RenderbufferStorageOES,
};

struct OpName {
    Op op;
    const char* name;
};

constexpr OpName kOps[] = {
    {Op::AlphaFunc, "glAlphaFunc"},
    {Op::ClientActiveTexture, "glClientActiveTexture"},
    {Op::Color4f, "glColor4f"},
    {Op::ColorPointer, "glColorPointer"},
    {Op::DisableClientState, "glDisableClientState"},
    {Op::EnableClientState, "glEnableClientState"},
    {Op::Fogf, "glFogf"},
    {Op::Fogfv, "glFogfv"},
    {Op::Fogx, "glFogx"},
    {Op::LightModelf, "glLightModelf"},
    {Op::LightModelfv, "glLightModelfv"},
    {Op::Lightf, "glLightf"},
    {Op::Lightfv, "glLightfv"},
    {Op::LoadIdentity, "glLoadIdentity"},
    {Op::LoadMatrixf, "glLoadMatrixf"},
    {Op::Materialf, "glMaterialf"},
    {Op::Materialfv, "glMaterialfv"},
    {Op::MatrixMode, "glMatrixMode"},
    {Op::NormalPointer, "glNormalPointer"},
    {Op::ShadeModel, "glShadeModel"},
    {Op::TexCoordPointer, "glTexCoordPointer"},
    {Op::TexEnvf, "glTexEnvf"},
    {Op::TexEnvfv, "glTexEnvfv"},
    {Op::TexEnvi, "glTexEnvi"},
    {Op::VertexPointer, "glVertexPointer"},
    {Op::BindFramebufferOES, "glBindFramebufferOES"},
    {Op::BindRenderbufferOES, "glBindRenderbufferOES"},
    {Op::CheckFramebufferStatusOES, "glCheckFramebufferStatusOES"},
    {Op::DeleteFramebuffersOES, "glDeleteFramebuffersOES"},
    {Op::DeleteRenderbuffersOES, "glDeleteRenderbuffersOES"},
    {Op::FramebufferRenderbufferOES, "glFramebufferRenderbufferOES"},
    {Op::FramebufferTexture2DOES, "glFramebufferTexture2DOES"},
    {Op::GenFramebuffersOES, "glGenFramebuffersOES"},
    {Op::GenRenderbuffersOES, "glGenRenderbuffersOES"},
    {Op::GenerateMipmapOES, "glGenerateMipmapOES"},
    {Op::GetFramebufferAttachmentParameterivOES, "glGetFramebufferAttachmentParameterivOES"},
    {Op::RenderbufferStorageOES, "glRenderbufferStorageOES"},
};

constexpr std::uint32_t kFirstIndex = 492;
constexpr std::uint32_t kLastIndex = 590;
// GL_OES_framebuffer_object: guest stubs 579-590 (an ES 1.x context has no GLES 2 twin to borrow).
constexpr std::uint32_t kFirstOesIndex = 579;
constexpr Op kOesOps[] = {
    Op::BindFramebufferOES, Op::BindRenderbufferOES, Op::CheckFramebufferStatusOES,
    Op::DeleteFramebuffersOES, Op::DeleteRenderbuffersOES, Op::FramebufferRenderbufferOES,
    Op::FramebufferTexture2DOES, Op::GenFramebuffersOES, Op::GenRenderbuffersOES,
    Op::GenerateMipmapOES, Op::GetFramebufferAttachmentParameterivOES, Op::RenderbufferStorageOES,
};
constexpr std::uint32_t kArrayBufferBinding = 0x8894;

Op op_for_index(std::uint32_t index) {
    static const std::array<Op, kLastIndex - kFirstIndex + 1> table = [] {
        std::array<Op, kLastIndex - kFirstIndex + 1> result{};
        for (const HostCallName& entry : kNames) {
            if (entry.index < kFirstIndex || entry.index > kLastIndex) continue;
            if (std::strcmp(entry.library, "libGLESv1_CM.so") != 0) continue;
            for (const OpName& op : kOps) {
                if (std::strcmp(op.name, entry.name) == 0) result[entry.index - kFirstIndex] = op.op;
            }
        }
        for (std::uint32_t i = 0; i < sizeof kOesOps / sizeof kOesOps[0]; ++i) {
            result[kFirstOesIndex + i - kFirstIndex] = kOesOps[i];
        }
        return result;
    }();
    return table[index - kFirstIndex];
}

const char* name_for_op(Op op) {
    for (const OpName& entry : kOps) {
        if (entry.op == op) return entry.name;
    }
    return "?";
}

// The driver entry for each operation, resolved on first use. A missing one is noted once in the
// runtime report and the call does nothing, as on a driver without it.
void* driver_entry(Op op) {
    static void* entries[64] = {};
    static bool resolved[64] = {};
    const unsigned slot = static_cast<unsigned>(op);
    if (!resolved[slot]) {
        resolved[slot] = true;
        entries[slot] = gl_resolve_proc(name_for_op(op));
        if (entries[slot] == nullptr) {
            runtime_report().note_gl_detail(std::string("missing-") + name_for_op(op), "no driver entry", false);
        }
    }
    return entries[slot];
}

// Values a "fv" call reads for each parameter name.
unsigned fog_count(std::uint32_t pname) { return pname == 0x0B66 ? 4u : 1u; }  // GL_FOG_COLOR
unsigned light_model_count(std::uint32_t pname) { return pname == 0x0B53 ? 4u : 1u; }  // LIGHT_MODEL_AMBIENT
unsigned light_count(std::uint32_t pname) {
    switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1203:  // GL_POSITION
        return 4;
    case 0x1204:  // GL_SPOT_DIRECTION
        return 3;
    default:
        return 1;
    }
}
unsigned material_count(std::uint32_t pname) {
    switch (pname) {
    case 0x1200:  // GL_AMBIENT
    case 0x1201:  // GL_DIFFUSE
    case 0x1202:  // GL_SPECULAR
    case 0x1600:  // GL_EMISSION
    case 0x1602:  // GL_AMBIENT_AND_DIFFUSE
        return 4;
    default:
        return 1;
    }
}
unsigned texenv_count(std::uint32_t pname) { return pname == 0x2201 ? 4u : 1u; }  // GL_TEXTURE_ENV_COLOR

}  // namespace

bool gles1_handle_host_call(HostGl& host, std::uint32_t index, GuestThread& thread) {
    if (index < kFirstIndex || index > kLastIndex) return false;
    const Op op = op_for_index(index);
    if (op == Op::None) return false;  // not one of the calls implemented here

    auto& regs = thread.regs();
    const std::uint32_t a0 = regs[0], a1 = regs[1], a2 = regs[2], a3 = regs[3];
    regs[0] = 0;
    regs[1] = 0;

    void* entry = driver_entry(op);
    if (entry == nullptr) return true;

    auto f = [](std::uint32_t word) { return std::bit_cast<float>(word); };

    // Reads `count` floats from the guest address into out[]. False if the range is unreadable.
    auto read_floats = [&](std::uint32_t address, unsigned count, float* out) {
        const std::uint8_t* source =
            host.runtime().memory().host_ptr(address, 4ull * count, kPageRead);
        if (source == nullptr) {
            log("GLES1 %s: guest pointer 0x%x is not readable", name_for_op(op), address);
            return false;
        }
        std::memcpy(out, source, 4u * count);
        return true;
    };

    // A client-array "pointer" is a guest address, or an offset into the bound array buffer.
    auto array_pointer = [&](std::uint32_t guest_value) -> const void* {
        std::int32_t buffer = 0;
        host.backend().glGetIntegerv(kArrayBufferBinding, &buffer);
        if (buffer != 0) return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(guest_value));
        if (guest_value == 0) return nullptr;
        return host.runtime().memory().base() + guest_value;
    };

    float values[16];
    switch (op) {
    case Op::AlphaFunc:
        reinterpret_cast<void (*)(std::uint32_t, float)>(entry)(a0, f(a1));
        break;
    case Op::ClientActiveTexture:
    case Op::DisableClientState:
    case Op::EnableClientState:
    case Op::MatrixMode:
    case Op::ShadeModel:
        reinterpret_cast<void (*)(std::uint32_t)>(entry)(a0);
        break;
    case Op::Color4f:
        reinterpret_cast<void (*)(float, float, float, float)>(entry)(f(a0), f(a1), f(a2), f(a3));
        break;
    case Op::ColorPointer:
    case Op::TexCoordPointer:
    case Op::VertexPointer:
        reinterpret_cast<void (*)(std::int32_t, std::uint32_t, std::int32_t, const void*)>(entry)(
            static_cast<std::int32_t>(a0), a1, static_cast<std::int32_t>(a2), array_pointer(a3));
        break;
    case Op::NormalPointer:
        reinterpret_cast<void (*)(std::uint32_t, std::int32_t, const void*)>(entry)(
            a0, static_cast<std::int32_t>(a1), array_pointer(a2));
        break;
    case Op::Fogf:
    case Op::LightModelf:
        reinterpret_cast<void (*)(std::uint32_t, float)>(entry)(a0, f(a1));
        break;
    case Op::Fogx:
        reinterpret_cast<void (*)(std::uint32_t, std::int32_t)>(entry)(a0, static_cast<std::int32_t>(a1));
        break;
    case Op::Fogfv:
    case Op::LightModelfv: {
        const unsigned count = op == Op::Fogfv ? fog_count(a0) : light_model_count(a0);
        if (!read_floats(a1, count, values)) break;
        reinterpret_cast<void (*)(std::uint32_t, const float*)>(entry)(a0, values);
        break;
    }
    case Op::Lightf:
    case Op::Materialf:
    case Op::TexEnvf:
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, float)>(entry)(a0, a1, f(a2));
        break;
    case Op::TexEnvi:
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, std::int32_t)>(entry)(
            a0, a1, static_cast<std::int32_t>(a2));
        break;
    case Op::Lightfv:
    case Op::Materialfv:
    case Op::TexEnvfv: {
        unsigned count = 1;
        if (op == Op::Lightfv) count = light_count(a1);
        else if (op == Op::Materialfv) count = material_count(a1);
        else count = texenv_count(a1);
        if (!read_floats(a2, count, values)) break;
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, const float*)>(entry)(a0, a1, values);
        break;
    }
    case Op::LoadIdentity:
        reinterpret_cast<void (*)()>(entry)();
        break;
    case Op::LoadMatrixf:
        if (!read_floats(a0, 16, values)) break;
        reinterpret_cast<void (*)(const float*)>(entry)(values);
        break;
    case Op::BindFramebufferOES:
    case Op::BindRenderbufferOES:
    case Op::RenderbufferStorageOES:
        if (op == Op::RenderbufferStorageOES) {
            reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, std::int32_t, std::int32_t)>(entry)(
                a0, a1, static_cast<std::int32_t>(a2), static_cast<std::int32_t>(a3));
        } else {
            reinterpret_cast<void (*)(std::uint32_t, std::uint32_t)>(entry)(a0, a1);
        }
        break;
    case Op::CheckFramebufferStatusOES:
        regs[0] = reinterpret_cast<std::uint32_t (*)(std::uint32_t)>(entry)(a0);
        break;
    case Op::GenerateMipmapOES:
        reinterpret_cast<void (*)(std::uint32_t)>(entry)(a0);
        break;
    case Op::FramebufferRenderbufferOES:
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>(entry)(a0, a1, a2, a3);
        break;
    case Op::FramebufferTexture2DOES: {
        // The fifth argument (level) is on the guest stack.
        const std::uint8_t* level_word = host.runtime().memory().host_ptr(regs[13], 4, kPageRead);
        if (level_word == nullptr) break;
        std::int32_t level;
        std::memcpy(&level, level_word, sizeof level);
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t)>(entry)(
            a0, a1, a2, a3, level);
        break;
    }
    case Op::GenFramebuffersOES:
    case Op::GenRenderbuffersOES: {
        // The driver writes the new names straight into guest memory (it is mapped linearly).
        const std::int32_t count = static_cast<std::int32_t>(a0);
        if (count <= 0 || count > 4096) break;
        std::uint8_t* target = host.runtime().memory().host_ptr(a1, 4ull * count, kPageRead | kPageWrite);
        if (target == nullptr) {
            log("GLES1 %s: guest pointer 0x%x is not writable", name_for_op(op), a1);
            break;
        }
        reinterpret_cast<void (*)(std::int32_t, std::uint32_t*)>(entry)(
            count, reinterpret_cast<std::uint32_t*>(target));
        break;
    }
    case Op::DeleteFramebuffersOES:
    case Op::DeleteRenderbuffersOES: {
        const std::int32_t count = static_cast<std::int32_t>(a0);
        if (count <= 0 || count > 4096) break;
        const std::uint8_t* source = host.runtime().memory().host_ptr(a1, 4ull * count, kPageRead);
        if (source == nullptr) break;
        std::vector<std::uint32_t> names(static_cast<std::size_t>(count));
        std::memcpy(names.data(), source, 4u * static_cast<std::size_t>(count));
        reinterpret_cast<void (*)(std::int32_t, const std::uint32_t*)>(entry)(count, names.data());
        break;
    }
    case Op::GetFramebufferAttachmentParameterivOES: {
        std::uint8_t* target = host.runtime().memory().host_ptr(a3, 4, kPageRead | kPageWrite);
        if (target == nullptr) break;
        reinterpret_cast<void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t*)>(entry)(
            a0, a1, a2, reinterpret_cast<std::int32_t*>(target));
        break;
    }
    case Op::None:
        break;
    }
    return true;
}

}  // namespace zb
