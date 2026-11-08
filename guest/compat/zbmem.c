/* Native memcpy / memmove / memset for big blocks (part of libzbcompat.so, loaded RTLD_GLOBAL before
 * the game, so the game's imports of these names resolve here first).
 *
 * Why: the translator runs the game's arm32 libc copy loops as translated code, and a game copies
 * and clears a lot of memory (vertex and texture data, array growth, loading). For a block of at
 * least native_mem_min bytes (default 16384) the work is handed to the host, which runs the real
 * arm64 memmove / memset on the guest's memory directly. Smaller blocks, overlapping memcpy, bad
 * pointers and everything else go to the original libc functions, so behaviour does not change; a
 * bad pointer faults inside the original libc, where a real device would fault.
 * Switch: native_mem_min=0 in zb-options.txt. Compile with -marm -fno-builtin (svc needs ARM
 * state, and the compiler must not turn code in here back into a call to memcpy). */
#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>

#include "zb/zlib_protocol.h"

#ifndef ZB_TEST
#define ZB_STR2(x) #x
#define ZB_STR(x) ZB_STR2(x)
#define ZB_MEM_HC(idx, a0, a1, a2)                                                               \
    ({                                                                                           \
        register unsigned _r0 __asm__("r0") = (unsigned)(a0);                                    \
        register unsigned _r1 __asm__("r1") = (unsigned)(a1);                                    \
        register unsigned _r2 __asm__("r2") = (unsigned)(a2);                                    \
        __asm__ volatile("svc #(0x5A0000|" ZB_STR(idx) ")" : "+r"(_r0), "+r"(_r1), "+r"(_r2) : \
                         : "memory", "cc");                                                      \
        _r0;                                                                                     \
    })
#endif

typedef void* (*copy_fn)(void*, const void*, size_t);
typedef void* (*set_fn)(void*, int, size_t);

static copy_fn real_memcpy;
static copy_fn real_memmove;
static set_fn real_memset;
static size_t native_min;  /* 0 = off */

/* last resort if libc cannot be looked up: plain byte loops (never reached in practice) */
static void* fallback_copy(void* d, const void* s, size_t n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    if (dp < sp || dp >= sp + n) {
        while (n--) *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}
static void* fallback_set(void* d, int c, size_t n) {
    unsigned char* dp = (unsigned char*)d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}

__attribute__((constructor(101))) static void zb_mem_init(void) {
#ifdef ZB_TEST
    void* handle = RTLD_NEXT;
#else
    void* handle = dlopen("libc.so", RTLD_NOW); /* already loaded: this only returns its handle */
#endif
    if (handle != NULL) {
        real_memcpy = (copy_fn)dlsym(handle, "memcpy");
        real_memmove = (copy_fn)dlsym(handle, "memmove");
        real_memset = (set_fn)dlsym(handle, "memset");
    }
#ifdef ZB_TEST
    native_min = 16384;
#else
    native_min = (size_t)ZB_MEM_HC(ZB_MEM_HC_PROBE, 0, 0, 0);
    if (real_memcpy == NULL || real_memmove == NULL || real_memset == NULL) native_min = 0;
#endif
}

static inline int far_apart(const void* d, const void* s, size_t n) {
    uintptr_t a = (uintptr_t)d, b = (uintptr_t)s;
    return a >= b + n || b >= a + n;
}

static void* zb_memcpy_impl(void* d, const void* s, size_t n) {
    if (native_min != 0 && n >= native_min && far_apart(d, s, n) && ZB_MEM_HC(ZB_MEM_HC_COPY, d, s, n) != 0) return d;
    return real_memcpy != NULL ? real_memcpy(d, s, n) : fallback_copy(d, s, n);
}

static void* zb_memmove_impl(void* d, const void* s, size_t n) {
    if (native_min != 0 && n >= native_min && ZB_MEM_HC(ZB_MEM_HC_COPY, d, s, n) != 0) return d;
    return real_memmove != NULL ? real_memmove(d, s, n) : fallback_copy(d, s, n);
}

static void* zb_memset_impl(void* d, int c, size_t n) {
    if (native_min != 0 && n >= native_min && ZB_MEM_HC(ZB_MEM_HC_SET, d, c, n) != 0) return d;
    return real_memset != NULL ? real_memset(d, c, n) : fallback_set(d, c, n);
}

/* The ARM EABI helpers the compiler calls for struct copies and clears. Note: __aeabi_memset takes
 * (dest, n, byte), unlike memset. */
static void zb_aeabi_memcpy(void* d, const void* s, size_t n) { zb_memcpy_impl(d, s, n); }
static void zb_aeabi_memmove(void* d, const void* s, size_t n) { zb_memmove_impl(d, s, n); }
static void zb_aeabi_memset(void* d, size_t n, int c) { zb_memset_impl(d, c, n); }
static void zb_aeabi_memclr(void* d, size_t n) { zb_memset_impl(d, 0, n); }

/* The public names, for the case where the linker does prefer this library's symbols ... */
void* memcpy(void* d, const void* s, size_t n) { return zb_memcpy_impl(d, s, n); }
void* memmove(void* d, const void* s, size_t n) { return zb_memmove_impl(d, s, n); }
void* memset(void* d, int c, size_t n) { return zb_memset_impl(d, c, n); }
void __aeabi_memcpy(void* d, const void* s, size_t n) { zb_aeabi_memcpy(d, s, n); }
void __aeabi_memcpy4(void* d, const void* s, size_t n) { zb_aeabi_memcpy(d, s, n); }
void __aeabi_memcpy8(void* d, const void* s, size_t n) { zb_aeabi_memcpy(d, s, n); }
void __aeabi_memmove(void* d, const void* s, size_t n) { zb_aeabi_memmove(d, s, n); }
void __aeabi_memmove4(void* d, const void* s, size_t n) { zb_aeabi_memmove(d, s, n); }
void __aeabi_memmove8(void* d, const void* s, size_t n) { zb_aeabi_memmove(d, s, n); }
void __aeabi_memset(void* d, size_t n, int c) { zb_aeabi_memset(d, n, c); }
void __aeabi_memset4(void* d, size_t n, int c) { zb_aeabi_memset(d, n, c); }
void __aeabi_memset8(void* d, size_t n, int c) { zb_aeabi_memset(d, n, c); }
void __aeabi_memclr(void* d, size_t n) { zb_aeabi_memclr(d, n); }
void __aeabi_memclr4(void* d, size_t n) { zb_aeabi_memclr(d, n); }
void __aeabi_memclr8(void* d, size_t n) { zb_aeabi_memclr(d, n); }

/* ... and, because that preference is not reliable, the host patches these functions into the import
 * tables of the game's libraries itself (see Process::patch_imports). */
struct zb_override {
    const char* name;
    void* function;
};
static const struct zb_override zb_mem_overrides[] = {
    {"memcpy", (void*)zb_memcpy_impl},          {"memmove", (void*)zb_memmove_impl},          {"memset", (void*)zb_memset_impl},
    {"__aeabi_memcpy", (void*)zb_aeabi_memcpy}, {"__aeabi_memcpy4", (void*)zb_aeabi_memcpy}, {"__aeabi_memcpy8", (void*)zb_aeabi_memcpy},
    {"__aeabi_memmove", (void*)zb_aeabi_memmove}, {"__aeabi_memmove4", (void*)zb_aeabi_memmove}, {"__aeabi_memmove8", (void*)zb_aeabi_memmove},
    {"__aeabi_memset", (void*)zb_aeabi_memset}, {"__aeabi_memset4", (void*)zb_aeabi_memset}, {"__aeabi_memset8", (void*)zb_aeabi_memset},
    {"__aeabi_memclr", (void*)zb_aeabi_memclr}, {"__aeabi_memclr4", (void*)zb_aeabi_memclr}, {"__aeabi_memclr8", (void*)zb_aeabi_memclr},
};

__attribute__((constructor(102))) static void zb_mem_register(void) {
#ifndef ZB_TEST
    if (native_min != 0) ZB_MEM_HC(ZB_PATCH_REGISTER, zb_mem_overrides, sizeof zb_mem_overrides / sizeof zb_mem_overrides[0], 0);
#endif
}
