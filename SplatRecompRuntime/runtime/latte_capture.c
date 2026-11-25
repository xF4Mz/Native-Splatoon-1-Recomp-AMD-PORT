// Frame capture (see latte_capture.h). Runs on the GPU thread only.
//
// The frame's records are gathered in memory and compressed + written to
// disk on a helper thread afterwards: doing that during the frame stopped
// the game for several seconds at every F12.
#include "latte_capture.h"
#include "latte_cp.h"
#include "wiiu_mem.h"
#include "host_thread.h"

#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile bool g_cap_active = false;
typedef struct { u8* data; size_t len, cap; char path[260]; } CapBuf;
static CapBuf* g_buf;
static u32* g_prev;   // registers as of the last record

// memory already saved: (addr, size) -> hash of the bytes
#define MEMTAB 65536
static struct { u32 addr, size; u64 hash; } *g_memtab;

static u64 hash64(const u8* p, u32 n) {
    u64 h = 0xCBF29CE484222325ull ^ n;
    while (n >= 8) {
        u64 v;
        memcpy(&v, p, 8);
        h ^= v;
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 31;
        p += 8;
        n -= 8;
    }
    while (n--) { h ^= *p++; h *= 0x100000001B3ull; }
    return h;
}

static void put(const void* p, u32 n) {
    if (!n || !g_buf) return;
    if (g_buf->len + n > g_buf->cap) {
        size_t cap = g_buf->cap ? g_buf->cap * 2 : (64u << 20);
        while (cap < g_buf->len + n) cap *= 2;
        u8* d = (u8*)realloc(g_buf->data, cap);
        if (!d) return;   // (out of memory: the capture comes out cut short)
        g_buf->data = d;
        g_buf->cap = cap;
    }
    memcpy(g_buf->data + g_buf->len, p, n);
    g_buf->len += n;
}
static void put32(u32 v) { put(&v, 4); }

static int cap_write_thread(void* arg) {
    CapBuf* b = (CapBuf*)arg;
    gzFile f = gzopen(b->path, "wb1");
    if (!f) printf("[capture] couldn't create %s\n", b->path);
    else {
        size_t done = 0;
        while (done < b->len) {
            unsigned n = (unsigned)(b->len - done > (64u << 20) ? (64u << 20) : b->len - done);
            if (gzwrite(f, b->data + done, n) != (int)n) break;
            done += n;
        }
        gzclose(f);
        printf("[capture] written: %s (%.0f MB)\n", b->path, b->len / 1048576.0);
    }
    free(b->data);
    free(b);
    return 0;
}

bool cap_open(const char* path) {
    if (g_cap_active) return true;
    g_buf = (CapBuf*)calloc(1, sizeof(CapBuf));
    if (!g_buf) return false;
    snprintf(g_buf->path, sizeof(g_buf->path), "%s", path);
    if (!g_prev) g_prev = (u32*)malloc(REG_COUNT * 4);
    if (!g_memtab) g_memtab = calloc(MEMTAB, sizeof(*g_memtab));
    memset(g_memtab, 0, MEMTAB * sizeof(*g_memtab));
    put32(CAP_MAGIC);
    put32(1);   // version
    put32(CAP_REGS);
    put32(REG_COUNT * 4);
    put(g_latte_regs, REG_COUNT * 4);
    memcpy(g_prev, g_latte_regs, REG_COUNT * 4);
    g_cap_active = true;
    printf("[capture] saving this frame to %s\n", path);
    return true;
}

void cap_close(void) {
    if (!g_cap_active) return;
    put32(CAP_END);
    put32(0);
    g_cap_active = false;
    CapBuf* b = g_buf;
    g_buf = NULL;
    // (compressed and saved in the background; the game goes on)
    hthrd_t t;
    if (!hthrd_create(&t, cap_write_thread, b, 1u << 20)) cap_write_thread(b);
#ifdef _WIN32
    else CloseHandle(t);
#endif
    printf("[capture] frame recorded, saving in the background\n");
}

void cap_mem(u32 addr, u32 size) {
    if (!g_cap_active || !size) return;
    const u8* p = wiiu_mem_ptr(addr, size);
    if (!p) return;
    u64 h = hash64(p, size);
    u32 slot = ((addr >> 4) * 2654435761u ^ size) & (MEMTAB - 1);
    for (u32 i = 0; i < 64; i++) {
        u32 k = (slot + i) & (MEMTAB - 1);
        if (g_memtab[k].size == 0) {
            g_memtab[k].addr = addr; g_memtab[k].size = size; g_memtab[k].hash = h;
            break;
        }
        if (g_memtab[k].addr == addr && g_memtab[k].size == size) {
            if (g_memtab[k].hash == h) return;   // already saved, unchanged
            g_memtab[k].hash = h;
            break;
        }
    }
    put32(CAP_MEM);
    put32(8 + size);
    put32(addr);
    put32(size);
    put(p, size);
}

void cap_regs_diff(void) {
    if (!g_cap_active) return;
    u32 n = 0;
    for (u32 i = 0; i < REG_COUNT; i++) if (g_latte_regs[i] != g_prev[i]) n++;
    if (!n) return;
    put32(CAP_RDIF);
    put32(n * 8);
    for (u32 i = 0; i < REG_COUNT; i++)
        if (g_latte_regs[i] != g_prev[i]) {
            put32(i);
            put32(g_latte_regs[i]);
            g_prev[i] = g_latte_regs[i];
        }
}

void cap_event(u32 type, const u32* words, u32 n) {
    if (!g_cap_active) return;
    put32(CAP_EVENT);
    put32(4 + n * 4);
    put32(type);
    put(words, n * 4);
}

void cap_surface(u32 addr, u32 gx2fmt, u32 w, u32 h, u32 flags, u32 vkformat, const void* bytes, u32 size) {
    if (!g_cap_active) return;
    put32(CAP_SURF);
    put32(24 + size);
    put32(addr);
    put32(gx2fmt);
    put32(w);
    put32(h);
    put32(flags);
    put32(vkformat);
    put(bytes, size);
}
