// Surface layouts (see gx2_layout.h). Moved here from gx2.c so the
// renderer computes mip levels exactly like GX2 does.
#include "gx2_layout.h"

#include "host_thread.h"

#include <string.h>

// Bits per element for a surface format (low 6 bits), and whether it's a
// 4x4 block-compressed format.
u32 gx2_format_bits(u32 format, bool* block) {
    *block = false;
    switch (format & 0x3F) {
    case 0x01: case 0x02: case 0x03: return 8;
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
    case 0x0B: case 0x0C: return 16;
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: return 32;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: return 64;
    case 0x22: case 0x23: return 128;
    case 0x31: case 0x34: *block = true; return 64;
    case 0x32: case 0x33: case 0x35: *block = true; return 128;
    default: return 32;
    }
}

// Surface sizes and layouts, following the rules of the GPU's address
// library (AMD AddrLib, R600 family: 2 pipes, 4 banks, 256-byte pipe
// interleave). These must match the console exactly: games place mip
// levels by these offsets, and the renderer reads textures with this pitch.
static bool tm_macro(u32 m) { return m >= 4 && m <= 15; }
static u32 tm_thickness(u32 m) { return (m == 3 || m == 7 || m == 11 || m == 13 || m == 15) ? 4 : 1; }
static u32 next_pow2(u32 v) { u32 p = 1; while (p < v) p <<= 1; return p; }
static u32 align_any(u32 v, u32 a) { return a ? (v + a - 1) / a * a : v; }

typedef struct { u32 pitch, height, slices, mode, base_align, pitch_align, height_align; u64 size; } SurfInfo;

static u32 tm_aspect(u32 m) { return (m == 5 || m == 9) ? 2 : (m == 6 || m == 10) ? 4 : 1; }

static u32 bank_swapped_width(u32 mode, u32 bpp, u32 pitch) {
    if (!((mode >= 8 && mode <= 11) || mode == 14 || mode == 15)) return 0;
    u32 bytes_per_sample = 8 * bpp, samples = tm_thickness(mode) > 1 ? 4 : 1;
    u32 bytes_per_tile_slice = samples * bytes_per_sample;
    u32 swap_tiles = 128 / bpp ? 128 / bpp : 1;
    u32 swap_width = swap_tiles * 8 * 4;
    u32 height_bytes = samples * tm_aspect(mode) * 2 * bpp;
    u32 swap_max = 2 * 4 * 2048 / height_bytes, swap_min = 256 * 8 * 4 / bytes_per_tile_slice;
    u32 v = swap_max >= swap_width ? (swap_width > swap_min ? swap_width : swap_min) : swap_max;
    while (v >= 2 * pitch && v > 1) v >>= 1;
    return v;
}

// One level. w/h in elements (4x4 blocks for compressed formats), bpp in
// bits per element. `pad_slices` false for cube maps' first level.
static void surf_info(u32 mode, u32 bpp, u32 w, u32 h, u32 slices, bool pad_slices, SurfInfo* o) {
    u32 thick = tm_thickness(mode);
    u32 pa, ha, ba;
    if (mode <= 1 || mode == 16) {   // linear
        if (mode == 1) { pa = 2048 / bpp > 64 ? 2048 / bpp : 64; ha = 1; ba = 256; }
        else { pa = 1; ha = 1; ba = 1; }
        thick = 1;
    } else if (mode == 2 || mode == 3) {   // 1D (micro) tiled
        ba = 256;
        pa = 256 / bpp / thick;
        if (pa < 8) pa = 8;
        ha = 8;
    } else {   // 2D (macro) tiled
        u32 aspect = tm_aspect(mode);
        u32 mtw = 32 / aspect, mth = 16 * aspect;
        u32 f = 256 / bpp / (8 * thick);
        pa = mtw * f > mtw ? mtw * f : mtw;
        ha = mth;
        u32 mtb = (bpp * mth * mtw + 7) >> 3;
        if (thick == 1) { u32 b2 = (ha * bpp * pa + 7) >> 3; ba = mtb > b2 ? mtb : b2; }
        else { u32 b2 = (4 * ha * bpp * pa + 7) >> 3; ba = 256 > b2 ? 256 : b2; }
        u32 micro_bytes = (thick * (bpp << 6) + 7) >> 3;
        if (micro_bytes >= 2048) ba /= micro_bytes / 2048;
        u32 bsw = bank_swapped_width(mode, bpp, align_any(w, pa));
        if (bsw > pa) pa = bsw;
    }
    o->mode = mode;
    o->pitch_align = pa;
    o->height_align = ha;
    o->base_align = ba;
    o->pitch = align_any(w, pa);
    o->height = align_any(h, ha);
    o->slices = pad_slices ? align_any(slices, thick) : slices;
    o->size = (u64)o->pitch * o->height * o->slices * bpp / 8;
}

// The tiling a mip level actually uses: small levels drop to simpler ones.
static u32 level_mode(u32 mode, u32 bpp, u32 w, u32 h, u32 slices) {
    if (mode >= 8 && mode <= 11) mode -= 4;          // no bank swapping below the first level
    else if (mode == 14) mode = 12;
    else if (mode == 15) mode = 13;
    u32 thick = tm_thickness(mode);
    if (thick > 1 && slices < 4) mode = mode == 3 ? 2 : mode == 13 ? 12 : 4;
    if (!tm_macro(mode)) return mode;
    thick = tm_thickness(mode);
    u32 micro_bytes = (bpp * (thick << 6) + 7) >> 3;
    u32 waf = micro_bytes >= 256 ? 1 : 256 / micro_bytes;
    u32 mtw = 32 / tm_aspect(mode), mth = 16 * tm_aspect(mode);
    if (next_pow2(w) < waf * mtw || next_pow2(h) < mth) return thick > 1 ? 3 : 2;
    return mode;
}

void gx2_surface_layout(u32 dim, u32 width, u32 height, u32 depth, u32 levels, u32 format, u32 aa, u32 resflag,
                        u32 mode, u32 swizzle, GX2Layout* out) {
    memset(out, 0, sizeof(*out));
    dim &= 7;

    if (!width) width = 1;
    if (!height) height = 1;
    if (!depth) depth = 1;
    if (!levels) levels = 1;
    if (levels > 14) levels = 14;
    bool block;
    u32 bpp = gx2_format_bits(format, &block);
    u32 samples = 1u << (aa & 3);
    bool changed_mode = false;
    if (mode == 0) {   // "default": the library picks (resFlag: 1 texture, 2 color buffer, 4 depth buffer)
        bool depth_buf = (resflag & 4) != 0;
        if (dim != 0 || depth_buf || aa) {
            mode = (dim == 2 && !(resflag & 2)) ? 7 : 4;
            changed_mode = true;
        } else {
            mode = 1;
        }
    }
    swizzle = tm_macro(mode) ? (swizzle & 0xFF00FFFF) | 0xD0000 : swizzle & 0xFF00FFFF;
    u32 last_mode = mode;
    u64 prev_size = 0;
    u32 first_mip_offset = 0, mip_offset[14] = { 0 };
    u32 image_size = 0, align = 0x800, pitch0 = 0;
    for (u32 level = 0; level < levels; level++) {
        u32 w = width >> level, h = height >> level, d = dim == 2 ? depth >> level : depth;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!d) d = 1;
        u32 ew = block ? (w + 3) / 4 : w, eh = block ? (h + 3) / 4 : h;
        if (level) {
            ew = next_pow2(ew);
            eh = next_pow2(eh);
            // (cube maps too: below the first level their faces are padded to
            // a power of two -- 12 faces take 16 faces' room, 6 take 8. Seen in
            // Splatoon's environment cubes: the data of each level sits after
            // that padding. Without it levels 2+ were read from empty memory,
            // the purple sheen on the characters' faces.)
            if (dim == 2 || dim == 3) d = next_pow2(d);
        }
        SurfInfo si;
        u32 lm = level ? level_mode(mode, bpp, ew, eh, d) : mode;
        surf_info(lm, bpp * samples, ew, eh, d, !(dim == 3 && level == 0), &si);
        if (level == 0) {
            if (changed_mode && width < si.pitch_align && height < si.height_align) {
                mode = mode == 7 ? 3 : 2;   // too small for 2D tiling
                surf_info(mode, bpp * samples, ew, eh, d, dim != 3, &si);
                swizzle &= 0xFF00FFFF;
                last_mode = mode;
            }
            image_size = (u32)si.size;
            align = si.base_align;
            pitch0 = si.pitch;
        } else {
            u32 pad = 0;
            if (tm_macro(last_mode) && !tm_macro(si.mode)) {
                swizzle = (swizzle & 0xFF00FFFF) | (level << 16);
                last_mode = si.mode;
                if (level > 1) pad = swizzle & 0xFFFF;
            }
            u32 ba = si.base_align ? si.base_align : 1;
            pad += (u32)((ba - prev_size % ba) % ba);
            if (level == 1) first_mip_offset = pad + (u32)prev_size;
            else mip_offset[level - 1] = pad + (u32)prev_size + mip_offset[level - 2];
        }
        prev_size = si.size;
        GX2LevelLayout* lv = &out->level[level];
        u32 lw = width >> level, lh = height >> level;
        lv->width = lw ? lw : 1;
        lv->height = lh ? lh : 1;
        u32 ld = dim == 2 ? depth >> level : depth;
        lv->depth = ld ? ld : 1;
        lv->pitch = si.pitch;
        lv->height_pad = si.height;
        lv->mode = si.mode;
        lv->swizzled = tm_macro(si.mode) && level < ((swizzle >> 16) & 0xFF);
    }
    u32 mip_size = levels > 1 ? (u32)prev_size + mip_offset[levels - 2] : 0;
    mip_offset[0] = first_mip_offset;
    for (u32 i = 0; i < 13; i++) out->mip_offset[i] = i < levels ? mip_offset[i] : 0;
    out->image_size = image_size;
    out->mip_size = mip_size;
    out->mode = mode;
    out->swizzle = swizzle;
    out->align = align;
    out->levels = levels;
    (void)pitch0;
}

// --- mip info from the game's textures --------------------------------------------------
#define MIPTAB 16384
static struct { volatile uint64_t key; GX2MipInfo m; } g_miptab[MIPTAB];

void gx2_note_mips(u32 key, const GX2MipInfo* m) {
    if (!key) return;
    u32 h = (key >> 8) * 2654435761u;
    for (u32 i = 0; i < 32; i++) {   // (already there and the same: the usual case, nothing written)
        u32 k = (h + i) & (MIPTAB - 1);
        u64 cur = hatomic_load64(&g_miptab[k].key);
        if (cur == key) { if (!memcmp(&g_miptab[k].m, m, sizeof(*m))) return; break; }
        if (cur == 0) break;
    }
    // (texture calls on several game threads at once -- display lists recorded side by side -- may get
    // here together: a write under a small lock)
    static volatile long lk;
    while (_InterlockedCompareExchange(&lk, 1, 0)) YieldProcessor();
    for (u32 i = 0; i < 32; i++) {
        u32 k = (h + i) & (MIPTAB - 1);
        u64 cur = hatomic_load64(&g_miptab[k].key);
        if (cur == key) {
            if (memcmp(&g_miptab[k].m, m, sizeof(*m))) g_miptab[k].m = *m;
            break;
        }
        if (cur == 0) {
            g_miptab[k].m = *m;
            hatomic_store64(&g_miptab[k].key, key);
            break;
        }
    }
    _InterlockedExchange(&lk, 0);
}

bool gx2_lookup_mips(u32 key, GX2MipInfo* out) {
    if (!key) return false;
    u32 h = (key >> 8) * 2654435761u;
    for (u32 i = 0; i < 32; i++) {
        u32 k = (h + i) & (MIPTAB - 1);
        u64 cur = hatomic_load64(&g_miptab[k].key);
        if (cur == key) { *out = g_miptab[k].m; return true; }
        if (cur == 0) return false;
    }
    return false;
}
