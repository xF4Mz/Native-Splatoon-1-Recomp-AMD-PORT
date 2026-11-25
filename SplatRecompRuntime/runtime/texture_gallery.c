// Texture gallery (see texture_gallery.h).
#include "texture_gallery.h"
#include "gx2_texture.h"
#include "host_thread.h"
#include "vk_ctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THUMB 128          // thumbnail size in pixels
#define GAP 8
#define MAX_THUMBS 512
#define QUEUE 1024
#define SEEN_SLOTS 16384   // power of two

typedef struct {
    u32 w, h;              // thumbnail size (fits in THUMB x THUMB)
    u8 px[THUMB * THUMB * 4];
} Thumb;

static hmtx_t g_lock;
static bool g_lock_ready = false;
static Gx2Surface g_queue[QUEUE];
static u32 g_queue_head = 0, g_queue_tail = 0;
static u64 g_seen[SEEN_SLOTS];
static Thumb* g_thumbs = NULL;   // ring of MAX_THUMBS, newest at g_thumb_count-1
static u32 g_thumb_count = 0;
static u32 g_skipped = 0;

static void init_once(void) {
    if (g_lock_ready) return;
    hmtx_init(&g_lock, false);
    g_lock_ready = true;
}

static bool mark_seen(u64 key) {
    u32 i = (u32)((key * 0x9E3779B97F4A7C15ull) >> 50) & (SEEN_SLOTS - 1);
    for (u32 n = 0; n < SEEN_SLOTS; n++, i = (i + 1) & (SEEN_SLOTS - 1)) {
        if (g_seen[i] == key) return false;
        if (g_seen[i] == 0) { g_seen[i] = key; return true; }
    }
    return false;   // table full: treat as seen
}

void gallery_note_texture(u32 texture_addr) {
    // (only while the gallery is on screen (F1): noting every texture the game
    // binds -- read its whole description, lock, look it up -- cost the main
    // thread a microsecond per bind, hundreds of times a frame)
    if (!g_vk.gallery) return;
    Gx2Surface s;
    if (!texture_addr || !gx2_surface_read(texture_addr, &s)) return;
    if (!s.image || (s.use & 0x6)) return;   // render targets hold nothing to see yet
    if (s.width < 16 && s.height < 16) return;
    if (!gx2_texture_supported(&s)) { g_skipped++; return; }
    init_once();
    u64 key = ((u64)s.image << 32) ^ ((u64)s.width << 16) ^ s.height ^ ((u64)s.format << 40);
    hmtx_lock(&g_lock);
    if (mark_seen(key ? key : 1) && g_queue_tail - g_queue_head < QUEUE)
        g_queue[g_queue_tail++ % QUEUE] = s;
    hmtx_unlock(&g_lock);
}

// Shrink a decoded texture into a thumbnail, showing transparency over a
// checkerboard. Returns false if the texture is one flat color (dummies).
static bool make_thumb(const Gx2Surface* s, const u8* rgba, Thumb* t) {
    u32 w = s->width, h = s->height;
    if (w >= h) { t->w = THUMB; t->h = (u32)((u64)THUMB * h / w); }
    else { t->h = THUMB; t->w = (u32)((u64)THUMB * w / h); }
    if (t->w == 0) t->w = 1;
    if (t->h == 0) t->h = 1;
    bool flat = true;
    for (u32 y = 0; y < t->h; y++) {
        for (u32 x = 0; x < t->w; x++) {
            const u8* p = rgba + ((size_t)(y * h / t->h) * w + (x * w / t->w)) * 4;
            if (memcmp(p, rgba, 4) != 0) flat = false;
            u8 check = (((x >> 3) ^ (y >> 3)) & 1) ? 70 : 40;
            u8* o = t->px + ((size_t)y * THUMB + x) * 4;
            for (int c = 0; c < 3; c++) o[c] = (u8)((p[c] * p[3] + check * (255 - p[3])) / 255);
            o[3] = 255;
        }
    }
    return !flat;
}

u32 gallery_update(void) {
    init_once();
    if (!g_thumbs) {
        g_thumbs = (Thumb*)calloc(MAX_THUMBS, sizeof(Thumb));
        if (!g_thumbs) return 0;
    }
    for (int budget = 0; budget < 6; budget++) {   // a few per frame keeps the window smooth
        Gx2Surface s;
        hmtx_lock(&g_lock);
        bool have = g_queue_head != g_queue_tail;
        if (have) s = g_queue[g_queue_head++ % QUEUE];
        hmtx_unlock(&g_lock);
        if (!have) break;
        u8* rgba = gx2_texture_to_rgba(&s);
        if (!rgba) continue;
        Thumb* t = &g_thumbs[g_thumb_count % MAX_THUMBS];
        if (make_thumb(&s, rgba, t)) {
            g_thumb_count++;
            if (g_thumb_count <= 5 || g_thumb_count % 50 == 0)
                printf("[gallery] texture #%u: %ux%u format %03X tile mode %u\n",
                       g_thumb_count, s.width, s.height, s.format, s.tile_mode);
        }
        free(rgba);
    }
    return g_thumb_count;
}

void gallery_compose(u8* out, u32 w, u32 h, u32 stride, bool bgra, const u8 bg[3]) {
    for (u32 y = 0; y < h; y++) {
        u8* row = out + (size_t)y * stride;
        for (u32 x = 0; x < w; x++) {
            row[x * 4 + 0] = bgra ? bg[2] : bg[0];
            row[x * 4 + 1] = bg[1];
            row[x * 4 + 2] = bgra ? bg[0] : bg[2];
            row[x * 4 + 3] = 255;
        }
    }
    if (!g_thumbs || w < THUMB + 2 * GAP || h < THUMB + 2 * GAP) return;
    u32 cols = (w - GAP) / (THUMB + GAP), rows = (h - GAP) / (THUMB + GAP);
    u32 shown = g_thumb_count < cols * rows ? g_thumb_count : cols * rows;
    if (shown > MAX_THUMBS) shown = MAX_THUMBS;
    u32 x0 = (w - cols * (THUMB + GAP) + GAP) / 2;
    for (u32 i = 0; i < shown; i++) {   // newest first
        const Thumb* t = &g_thumbs[(g_thumb_count - 1 - i) % MAX_THUMBS];
        u32 cx = x0 + (i % cols) * (THUMB + GAP) + (THUMB - t->w) / 2;
        u32 cy = GAP + (i / cols) * (THUMB + GAP) + (THUMB - t->h) / 2;
        for (u32 y = 0; y < t->h; y++) {
            u8* dst = out + (size_t)(cy + y) * stride + (size_t)cx * 4;
            const u8* src = t->px + (size_t)y * THUMB * 4;
            for (u32 x = 0; x < t->w; x++) {
                dst[x * 4 + 0] = bgra ? src[x * 4 + 2] : src[x * 4 + 0];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = bgra ? src[x * 4 + 0] : src[x * 4 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    }
}

u32 gallery_count(void) { return g_thumb_count; }
