// The console's zlib (zlib125.rpl), done with the PC's zlib.
//
// The game's z_stream lives in guest memory (big-endian, 32-bit pointers):
//   0x00 next_in, 0x04 avail_in, 0x08 total_in, 0x0C next_out,
//   0x10 avail_out, 0x14 total_out, 0x18 msg, 0x1C state, 0x20 zalloc,
//   0x24 zfree, 0x28 opaque, 0x2C data_type, 0x30 adler, 0x34 reserved.
// Each guest stream gets a PC z_stream; before every call we copy the
// buffer positions in, afterwards we copy them back. zlib's own working
// memory stays on the PC side (Cemu notes Splatoon's zalloc hands out too
// little memory for a newer zlib, so we never use it).
#include "zlib125.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MAX_STREAMS 64
typedef struct { u32 guest; bool deflating; z_stream z; } Stream;
static Stream g_streams[MAX_STREAMS];

static Stream* stream_find(u32 guest) {
    for (int i = 0; i < MAX_STREAMS; i++) if (g_streams[i].guest == guest) return &g_streams[i];
    return NULL;
}
static Stream* stream_new(u32 guest, bool deflating) {
    Stream* s = stream_find(guest);
    if (s) {   // re-initialised without End: drop the old one
        if (s->deflating) deflateEnd(&s->z); else inflateEnd(&s->z);
    } else {
        for (int i = 0; i < MAX_STREAMS && !s; i++) if (!g_streams[i].guest) s = &g_streams[i];
    }
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->guest = guest;
    s->deflating = deflating;
    return s;
}
static void stream_free(Stream* s) { memset(s, 0, sizeof(*s)); }

// Guest buffer -> PC pointer (whole range must exist).
static Bytef* gptr(u32 addr, u32 len) { return addr ? (Bytef*)wiiu_mem_ptr(addr, len ? len : 1) : Z_NULL; }

static void copy_in(Stream* s) {
    u32 g = s->guest;
    u32 next_in = wiiu_read32(g + 0x00), avail_in = wiiu_read32(g + 0x04);
    u32 next_out = wiiu_read32(g + 0x0C), avail_out = wiiu_read32(g + 0x10);
    s->z.next_in = gptr(next_in, avail_in);
    s->z.avail_in = s->z.next_in ? avail_in : 0;
    s->z.next_out = gptr(next_out, avail_out);
    s->z.avail_out = s->z.next_out ? avail_out : 0;
}
static void copy_out(Stream* s, Bytef* in0, Bytef* out0) {
    u32 g = s->guest;
    u32 used_in = s->z.next_in && in0 ? (u32)(s->z.next_in - in0) : 0;
    u32 used_out = s->z.next_out && out0 ? (u32)(s->z.next_out - out0) : 0;
    wiiu_write32(g + 0x00, wiiu_read32(g + 0x00) + used_in);
    wiiu_write32(g + 0x04, s->z.avail_in);
    wiiu_write32(g + 0x08, (u32)s->z.total_in);
    wiiu_write32(g + 0x0C, wiiu_read32(g + 0x0C) + used_out);
    wiiu_write32(g + 0x10, s->z.avail_out);
    wiiu_write32(g + 0x14, (u32)s->z.total_out);
    wiiu_write32(g + 0x18, 0);
    wiiu_write32(g + 0x2C, (u32)s->z.data_type);
    wiiu_write32(g + 0x30, (u32)s->z.adler);
}
static void mark_state(u32 g, Stream* s) {
    wiiu_write32(g + 0x1C, s ? 0x5A100000u + (u32)(s - g_streams) : 0);   // non-null "state"
    wiiu_write32(g + 0x08, 0);
    wiiu_write32(g + 0x14, 0);
    wiiu_write32(g + 0x18, 0);
}

static int do_init(CPUState* c, bool deflating, int level, int method, int window, int mem, int strategy) {
    u32 g = arg(c, 0);
    Stream* s = g ? stream_new(g, deflating) : NULL;
    if (!s) return Z_MEM_ERROR;
    int r = deflating ? deflateInit2(&s->z, level, method, window, mem, strategy)
                      : inflateInit2(&s->z, window);
    if (r != Z_OK) { stream_free(s); mark_state(g, NULL); return r; }
    mark_state(g, s);
    wiiu_write32(g + 0x30, (u32)s->z.adler);
    return r;
}

static void hle_inflateInit_(CPUState* c) { ret(c, (u32)do_init(c, false, 0, 0, MAX_WBITS, 0, 0)); }
static void hle_inflateInit2_(CPUState* c) { ret(c, (u32)do_init(c, false, 0, 0, (int)arg(c, 1), 0, 0)); }
static void hle_deflateInit_(CPUState* c) {   // (strm, level, version, size)
    ret(c, (u32)do_init(c, true, (int)arg(c, 1), Z_DEFLATED, MAX_WBITS, 8, Z_DEFAULT_STRATEGY));
}
static void hle_deflateInit2_(CPUState* c) {   // (strm, level, method, windowBits, memLevel, strategy, version, size)
    ret(c, (u32)do_init(c, true, (int)arg(c, 1), (int)arg(c, 2), (int)arg(c, 3), (int)arg(c, 4), (int)arg(c, 5)));
}

static void run(CPUState* c, bool deflating) {
    Stream* s = stream_find(arg(c, 0));
    if (!s || s->deflating != deflating) { ret(c, (u32)Z_STREAM_ERROR); return; }
    copy_in(s);
    Bytef* in0 = s->z.next_in;
    Bytef* out0 = s->z.next_out;
    int r = deflating ? deflate(&s->z, (int)arg(c, 1)) : inflate(&s->z, (int)arg(c, 1));
    copy_out(s, in0, out0);
    ret(c, (u32)r);
}
static void inflate_(CPUState* c) { run(c, false); }
static void deflate_(CPUState* c) { run(c, true); }

static void end(CPUState* c, bool deflating) {
    Stream* s = stream_find(arg(c, 0));
    if (!s) { ret(c, (u32)Z_STREAM_ERROR); return; }
    int r = deflating ? deflateEnd(&s->z) : inflateEnd(&s->z);
    stream_free(s);
    wiiu_write32(arg(c, 0) + 0x1C, 0);   // state = NULL (totals stay, like zlib)
    ret(c, (u32)r);
}
static void inflateEnd_(CPUState* c) { end(c, false); }
static void deflateEnd_(CPUState* c) { end(c, true); }

static void inflateReset_(CPUState* c) {
    Stream* s = stream_find(arg(c, 0));
    if (!s || s->deflating) { ret(c, (u32)Z_STREAM_ERROR); return; }
    int r = inflateReset(&s->z);
    wiiu_write32(arg(c, 0) + 0x08, 0);
    wiiu_write32(arg(c, 0) + 0x14, 0);
    ret(c, (u32)r);
}
static void inflateReset2_(CPUState* c) {
    Stream* s = stream_find(arg(c, 0));
    if (!s || s->deflating) { ret(c, (u32)Z_STREAM_ERROR); return; }
    int r = inflateReset2(&s->z, (int)arg(c, 1));
    wiiu_write32(arg(c, 0) + 0x08, 0);
    wiiu_write32(arg(c, 0) + 0x14, 0);
    ret(c, (u32)r);
}
static void deflateReset_(CPUState* c) {
    Stream* s = stream_find(arg(c, 0));
    if (!s || !s->deflating) { ret(c, (u32)Z_STREAM_ERROR); return; }
    ret(c, (u32)deflateReset(&s->z));
}
static void deflateBound_(CPUState* c) {
    Stream* s = stream_find(arg(c, 0));
    ret(c, (u32)(s ? deflateBound(&s->z, arg(c, 1)) : compressBound(arg(c, 1)) + 64));
}

// One-shot helpers: (dest, u32* destLen, source, sourceLen[, level])
static void uncompress_(CPUState* c) {
    u32 len_ptr = arg(c, 1);
    uLongf dlen = wiiu_read32(len_ptr);
    Bytef* d = gptr(arg(c, 0), (u32)dlen);
    Bytef* src = gptr(arg(c, 2), arg(c, 3));
    if (!d || !src) { ret(c, (u32)Z_BUF_ERROR); return; }
    int r = uncompress(d, &dlen, src, arg(c, 3));
    wiiu_write32(len_ptr, (u32)dlen);
    ret(c, (u32)r);
}
static void compress_level(CPUState* c, int level) {
    u32 len_ptr = arg(c, 1);
    uLongf dlen = wiiu_read32(len_ptr);
    Bytef* d = gptr(arg(c, 0), (u32)dlen);
    Bytef* src = gptr(arg(c, 2), arg(c, 3));
    if (!d || !src) { ret(c, (u32)Z_BUF_ERROR); return; }
    int r = compress2(d, &dlen, src, arg(c, 3), level);
    wiiu_write32(len_ptr, (u32)dlen);
    ret(c, (u32)r);
}
static void compress_(CPUState* c) { compress_level(c, Z_DEFAULT_COMPRESSION); }
static void compress2_(CPUState* c) { compress_level(c, (int)arg(c, 4)); }
static void compressBound_(CPUState* c) { ret(c, (u32)compressBound(arg(c, 0))); }

static void crc32_(CPUState* c) {   // (crc, buf, len)
    Bytef* p = gptr(arg(c, 1), arg(c, 2));
    ret(c, (u32)crc32(arg(c, 0), p, p ? arg(c, 2) : 0));
}
static void adler32_(CPUState* c) {
    Bytef* p = gptr(arg(c, 1), arg(c, 2));
    ret(c, (u32)adler32(arg(c, 0), p, p ? arg(c, 2) : 0));
}
static void zlibVersion_(CPUState* c) {
    static u32 addr = 0;
    if (!addr) {
        addr = hle_heap_alloc(16, 4);
        const char* v = "1.2.5";
        for (int i = 0; i < 6; i++) wiiu_write8(addr + (u32)i, (u8)v[i]);
    }
    ret(c, addr);
}

static const HleEntry g_zlib_table[] = {
    { "inflateInit_", hle_inflateInit_ },
    { "inflateInit2_", hle_inflateInit2_ },
    { "inflate", inflate_ },
    { "inflateEnd", inflateEnd_ },
    { "inflateReset", inflateReset_ },
    { "inflateReset2", inflateReset2_ },
    { "deflateInit_", hle_deflateInit_ },
    { "deflateInit2_", hle_deflateInit2_ },
    { "deflate", deflate_ },
    { "deflateEnd", deflateEnd_ },
    { "deflateReset", deflateReset_ },
    { "deflateBound", deflateBound_ },
    { "uncompress", uncompress_ },
    { "compress", compress_ },
    { "compress2", compress2_ },
    { "compressBound", compressBound_ },
    { "crc32", crc32_ },
    { "adler32", adler32_ },
    { "zlibVersion", zlibVersion_ },
};

const HleEntry* zlib_hle_table(size_t* count) {
    *count = sizeof(g_zlib_table) / sizeof(g_zlib_table[0]);
    return g_zlib_table;
}
