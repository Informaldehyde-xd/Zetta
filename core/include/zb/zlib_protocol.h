/* Native zlib: shared by the arm32 guest (guest/compat/zbzlib.c) and the arm64 host
 * (core/src/android/host_zlib.cpp). C-compatible. The guest keeps zlib's own z_stream, but the
 * real work (inflate, crc32, ...) runs as native arm64 code in the host instead of being
 * translated. Guest memory is mapped flat in the host, so no data is copied. */
#pragma once

#define ZB_ZLIB_HC_INFLATE_INIT 700  /* r0 = struct zb_zcall*, arg = windowBits */
#define ZB_ZLIB_HC_INFLATE 701       /* r0 = struct zb_zcall*, arg = flush */
#define ZB_ZLIB_HC_INFLATE_RESET 702
#define ZB_ZLIB_HC_INFLATE_END 703
#define ZB_ZLIB_HC_INFLATE_DICT 704  /* next_in/avail_in = dictionary */
#define ZB_ZLIB_HC_CRC32 705         /* r0 = crc, r1 = buf, r2 = len */
#define ZB_ZLIB_HC_ADLER32 706       /* r0 = adler, r1 = buf, r2 = len */
#define ZB_ZLIB_HC_UNCOMPRESS 707    /* r0 = dest, r1 = &destLen, r2 = src, r3 = srcLen */
#define ZB_ZLIB_HC_PROBE 708         /* r0 = 1 when native zlib is switched on, else 0 */
#define ZB_ZLIB_HC_MALLOC_PROBE 709  /* r0 = 1 when the allocator should keep freed memory, else 0 */
#define ZB_ZLIB_HC_LAST 709

#define ZB_ZLIB_MAGIC 0x425A5A4Eu

struct zb_zcall {
    unsigned int handle;
    unsigned int next_in;
    unsigned int avail_in;
    unsigned int next_out;
    unsigned int avail_out;
    unsigned int arg;
    int ret;
    unsigned int total_in;
    unsigned int total_out;
    unsigned int adler;
    unsigned int data_type;
    char msg[64];
};
