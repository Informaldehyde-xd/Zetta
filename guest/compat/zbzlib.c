/* Native zlib for old arm32 games (part of libzbcompat.so, loaded RTLD_GLOBAL before the game).
 * The game's imports of inflate, inflateInit_, inflateInit2_, inflateReset, inflateEnd,
 * inflateSetDictionary, uncompress, crc32 and adler32 resolve to the functions below first. They
 * hand the work to the translator's host, which runs real arm64 zlib on the guest's buffers.
 * With native_zlib=0 (zb-options.txt) every call goes to the system libz.so as before.
 * Not covered (the game keeps using libz for them): deflate*, gz*, inflateSync, inflateCopy,
 * inflatePrime, inflateGetHeader. Compile with -marm: svc needs ARM state. */
#include <dlfcn.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "zb/zlib_protocol.h"

#define ZB_STR2(x) #x
#define ZB_STR(x) ZB_STR2(x)
/* idx must be a plain number (the ZB_ZLIB_HC_* names are); it becomes svc #(0x5A0000 | idx). */
#define ZB_HC(idx, a0, a1, a2, a3)                                                                  \
    ({                                                                                              \
        register unsigned _r0 __asm__("r0") = (unsigned)(a0);                                       \
        register unsigned _r1 __asm__("r1") = (unsigned)(a1);                                       \
        register unsigned _r2 __asm__("r2") = (unsigned)(a2);                                       \
        register unsigned _r3 __asm__("r3") = (unsigned)(a3);                                       \
        __asm__ volatile("svc #(0x5A0000|" ZB_STR(idx) ")" : "+r"(_r0), "+r"(_r1), "+r"(_r2), "+r"(_r3) \
                         : : "memory", "cc");                                                       \
        _r0;                                                                                        \
    })

_Static_assert(sizeof(z_stream) == 56, "z_stream must be the 32-bit layout");
_Static_assert(sizeof(struct zb_zcall) == 108, "zb_zcall layout");

typedef struct {
    unsigned magic;
    unsigned handle;
    struct zb_zcall call;
} ZbCtx;

static int native(void) {
    static int state = -1;
    if (state < 0) state = (int)ZB_HC(ZB_ZLIB_HC_PROBE, 0, 0, 0, 0) != 0;
    return state;
}

static void* real(const char* name) {
    static void* lib;
    if (lib == NULL) lib = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
    return lib != NULL ? dlsym(lib, name) : NULL;
}

static ZbCtx* ours(z_streamp strm) {
    if (strm == Z_NULL || strm->state == Z_NULL) return NULL;
    ZbCtx* ctx = (ZbCtx*)strm->state;
    return ctx->magic == ZB_ZLIB_MAGIC ? ctx : NULL;
}

static void take_results(z_streamp strm, ZbCtx* ctx) {
    strm->total_in = ctx->call.total_in;
    strm->total_out = ctx->call.total_out;
    strm->adler = ctx->call.adler;
    strm->data_type = (int)ctx->call.data_type;
    strm->msg = ctx->call.msg[0] != 0 ? ctx->call.msg : Z_NULL;
}

int inflateInit2_(z_streamp strm, int windowBits, const char* version, int stream_size) {
    if (native()) {
        if (version == Z_NULL || version[0] != ZLIB_VERSION[0] || stream_size != (int)sizeof(z_stream))
            return Z_VERSION_ERROR;
        if (strm == Z_NULL) return Z_STREAM_ERROR;
        ZbCtx* ctx = (ZbCtx*)calloc(1, sizeof(ZbCtx));
        if (ctx == NULL) return Z_MEM_ERROR;
        ctx->call.arg = (unsigned)windowBits;
        ZB_HC(ZB_ZLIB_HC_INFLATE_INIT, &ctx->call, 0, 0, 0);
        if (ctx->call.ret != Z_OK) {
            int result = ctx->call.ret;
            free(ctx);
            return result;
        }
        ctx->magic = ZB_ZLIB_MAGIC;
        ctx->handle = ctx->call.handle;
        strm->state = (struct internal_state*)ctx;
        take_results(strm, ctx);
        strm->msg = Z_NULL;
        strm->total_in = 0;
        strm->total_out = 0;
        return Z_OK;
    }
    int (*fn)(z_streamp, int, const char*, int) = (int (*)(z_streamp, int, const char*, int))real("inflateInit2_");
    return fn != NULL ? fn(strm, windowBits, version, stream_size) : Z_STREAM_ERROR;
}

int inflateInit_(z_streamp strm, const char* version, int stream_size) {
    return inflateInit2_(strm, 15, version, stream_size);
}

int inflate(z_streamp strm, int flush) {
    ZbCtx* ctx = ours(strm);
    if (ctx == NULL) {
        int (*fn)(z_streamp, int) = (int (*)(z_streamp, int))real("inflate");
        return fn != NULL ? fn(strm, flush) : Z_STREAM_ERROR;
    }
    ctx->call.handle = ctx->handle;
    ctx->call.next_in = (unsigned)strm->next_in;
    ctx->call.avail_in = strm->avail_in;
    ctx->call.next_out = (unsigned)strm->next_out;
    ctx->call.avail_out = strm->avail_out;
    ctx->call.arg = (unsigned)flush;
    ZB_HC(ZB_ZLIB_HC_INFLATE, &ctx->call, 0, 0, 0);
    strm->next_in = (z_const Bytef*)ctx->call.next_in;
    strm->avail_in = ctx->call.avail_in;
    strm->next_out = (Bytef*)ctx->call.next_out;
    strm->avail_out = ctx->call.avail_out;
    take_results(strm, ctx);
    return ctx->call.ret;
}

int inflateReset(z_streamp strm) {
    ZbCtx* ctx = ours(strm);
    if (ctx == NULL) {
        int (*fn)(z_streamp) = (int (*)(z_streamp))real("inflateReset");
        return fn != NULL ? fn(strm) : Z_STREAM_ERROR;
    }
    ctx->call.handle = ctx->handle;
    ZB_HC(ZB_ZLIB_HC_INFLATE_RESET, &ctx->call, 0, 0, 0);
    take_results(strm, ctx);
    strm->msg = Z_NULL;
    return ctx->call.ret;
}

int inflateEnd(z_streamp strm) {
    ZbCtx* ctx = ours(strm);
    if (ctx == NULL) {
        int (*fn)(z_streamp) = (int (*)(z_streamp))real("inflateEnd");
        return fn != NULL ? fn(strm) : Z_STREAM_ERROR;
    }
    ctx->call.handle = ctx->handle;
    ZB_HC(ZB_ZLIB_HC_INFLATE_END, &ctx->call, 0, 0, 0);
    int result = ctx->call.ret;
    ctx->magic = 0;
    strm->state = Z_NULL;
    free(ctx);
    return result;
}

int inflateSetDictionary(z_streamp strm, const Bytef* dictionary, uInt dictLength) {
    ZbCtx* ctx = ours(strm);
    if (ctx == NULL) {
        int (*fn)(z_streamp, const Bytef*, uInt) = (int (*)(z_streamp, const Bytef*, uInt))real("inflateSetDictionary");
        return fn != NULL ? fn(strm, dictionary, dictLength) : Z_STREAM_ERROR;
    }
    ctx->call.handle = ctx->handle;
    ctx->call.next_in = (unsigned)dictionary;
    ctx->call.avail_in = dictLength;
    ZB_HC(ZB_ZLIB_HC_INFLATE_DICT, &ctx->call, 0, 0, 0);
    take_results(strm, ctx);
    return ctx->call.ret;
}

int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen) {
    if (native()) return (int)ZB_HC(ZB_ZLIB_HC_UNCOMPRESS, dest, destLen, source, sourceLen);
    int (*fn)(Bytef*, uLongf*, const Bytef*, uLong) = (int (*)(Bytef*, uLongf*, const Bytef*, uLong))real("uncompress");
    return fn != NULL ? fn(dest, destLen, source, sourceLen) : Z_STREAM_ERROR;
}

uLong crc32(uLong crc, const Bytef* buf, uInt len) {
    if (native()) return (uLong)ZB_HC(ZB_ZLIB_HC_CRC32, crc, buf, len, 0);
    uLong (*fn)(uLong, const Bytef*, uInt) = (uLong (*)(uLong, const Bytef*, uInt))real("crc32");
    return fn != NULL ? fn(crc, buf, len) : 0;
}

uLong adler32(uLong adler, const Bytef* buf, uInt len) {
    if (native()) return (uLong)ZB_HC(ZB_ZLIB_HC_ADLER32, adler, buf, len, 0);
    uLong (*fn)(uLong, const Bytef*, uInt) = (uLong (*)(uLong, const Bytef*, uInt))real("adler32");
    return fn != NULL ? fn(adler, buf, len) : 1;
}

/* The guest's allocator (scudo) hands freed memory back to the system on a short timer, and the
 * game then touches it again: a loading screen of one game showed 1.1 million page faults in 9
 * seconds. mallopt(M_DECAY_TIME, 1) switches the automatic release to its least eager setting.
 * Switch: malloc_keep_memory=0 in zb-options.txt. */
#ifndef M_DECAY_TIME
#define M_DECAY_TIME (-100)
#endif
__attribute__((constructor)) static void zb_malloc_tune(void) {
    if (ZB_HC(ZB_ZLIB_HC_MALLOC_PROBE, 0, 0, 0, 0) != 0) mallopt(M_DECAY_TIME, 1);
}
