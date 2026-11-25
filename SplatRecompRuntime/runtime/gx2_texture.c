// Reading the game's textures out of guest memory (see gx2_texture.h).
//
// The Wii U GPU (an AMD R600/R700-family "Latte") stores most textures
// "tiled": pixels are grouped into 8x8 micro tiles, and those into macro
// tiles spread across memory banks and pipes, so the GPU can fetch nearby
// pixels in parallel. To look at a texture on the PC we undo that
// arrangement (detile) and then decode its pixel format (often a block
// compressed BC1-BC5 format) into plain RGBA.
//
// The address math follows the public description of AMD's R600 address
// library: 2 pipes, 4 banks, 256-byte pipe interleave, 2 KB split size.
#include "gx2_texture.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PIPES = 2, BANKS = 4, GROUP_BITS = 8, PIPE_BITS = 1, BANK_BITS = 2 };

bool gx2_surface_read(u32 addr, Gx2Surface* s) {
    if (!wiiu_mem_ptr(addr, 0x74)) return false;
    s->dim = wiiu_read32(addr + 0x00);
    s->width = wiiu_read32(addr + 0x04);
    s->height = wiiu_read32(addr + 0x08);
    s->depth = wiiu_read32(addr + 0x0C);
    s->mip_levels = wiiu_read32(addr + 0x10);
    s->format = wiiu_read32(addr + 0x14);
    s->aa = wiiu_read32(addr + 0x18);
    s->use = wiiu_read32(addr + 0x1C);
    s->image_size = wiiu_read32(addr + 0x20);
    s->image = wiiu_read32(addr + 0x24);
    s->tile_mode = wiiu_read32(addr + 0x30);
    s->swizzle = wiiu_read32(addr + 0x34);
    s->pitch = wiiu_read32(addr + 0x3C);
    return true;
}

// Bits per element (a pixel, or a 4x4 block for BC formats), 0 = unknown.
static u32 element_bits(u32 hw, bool* block) {
    *block = false;
    switch (hw) {
    case 0x01: case 0x02: return 8;                          // R8, R4G4
    case 0x05: case 0x06: case 0x07: case 0x08:              // R16, R16F, RG8, R5G6B5
    case 0x0A: case 0x0B: case 0x0C: return 16;              // RGB5A1, RGBA4, A1BGR5
    case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x16:   // R32, R32F, RG16, RG16F, R11G11B10F
    case 0x19: case 0x1A: case 0x1B: return 32;              // RGB10A2, RGBA8, A2BGR10
    case 0x1D: case 0x1E: case 0x1F: case 0x20: return 64;   // RG32, RGBA16, RGBA16F
    case 0x22: case 0x23: return 128;                        // RGBA32
    case 0x31: case 0x34: *block = true; return 64;          // BC1, BC4
    case 0x32: case 0x33: case 0x35: *block = true; return 128;   // BC2, BC3, BC5
    default: return 0;
    }
}

static u32 thickness(u32 mode) {
    switch (mode) {
    case 3: case 7: case 11: case 13: case 15: return 4;
    case 16: case 17: return 8;
    default: return 1;
    }
}

static u32 pixel_index(u32 x, u32 y, u32 bpp) {
    u32 b0, b1, b2, b3, b4, b5;
    switch (bpp) {
    case 8:
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = (x >> 2) & 1;
        b3 = (y >> 1) & 1; b4 = y & 1; b5 = (y >> 2) & 1;
        break;
    case 16:
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = (x >> 2) & 1;
        b3 = y & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
        break;
    case 64:
        b0 = x & 1; b1 = y & 1; b2 = (x >> 1) & 1;
        b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
        break;
    default:   // 32, 96 and anything else
        b0 = x & 1; b1 = (x >> 1) & 1; b2 = y & 1;
        b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
        break;
    case 128:
        b0 = y & 1; b1 = x & 1; b2 = (x >> 1) & 1;
        b3 = (x >> 2) & 1; b4 = (y >> 1) & 1; b5 = (y >> 2) & 1;
        break;
    }
    return (b5 << 5) | (b4 << 4) | (b3 << 3) | (b2 << 2) | (b1 << 1) | b0;
}

static u64 addr_micro_tiled(u32 x, u32 y, u32 bpp, u32 pitch) {
    u64 micro_tile_bytes = (64ull * bpp + 7) / 8;
    u64 tile = (x >> 3) + (u64)(y >> 3) * (pitch >> 3);
    return micro_tile_bytes * tile + ((u64)bpp * pixel_index(x, y, bpp) >> 3);
}

static u64 addr_macro_tiled(u32 x, u32 y, u32 bpp, u32 pitch, u32 mode, u32 pipe_swizzle, u32 bank_swizzle) {
    u64 micro_tile_bits = (u64)bpp * 64;
    u64 elem_offset = ((u64)bpp * pixel_index(x, y, bpp) + 7) / 8;

    u32 pipe = ((y >> 3) ^ (x >> 3)) & 1;
    u32 bank = (((y / (16 * PIPES)) ^ (x >> 3)) & 1) | ((((y / (8 * PIPES)) ^ (x >> 4)) & 1) << 1);
    u32 bank_pipe = pipe + PIPES * bank;
    u32 rotation = (mode >= 4 && mode <= 11) ? PIPES * ((BANKS >> 1) - 1) : 0;
    u32 swizzle = pipe_swizzle + PIPES * bank_swizzle;
    (void)rotation;   // only matters for slices > 0
    bank_pipe ^= swizzle;
    bank_pipe %= PIPES * BANKS;
    pipe = bank_pipe % PIPES;
    bank = bank_pipe / PIPES;

    u32 macro_pitch = 8 * BANKS, macro_height = 8 * PIPES;
    if (mode == 5 || mode == 9) { macro_pitch >>= 1; macro_height *= 2; }
    else if (mode == 6 || mode == 10) { macro_pitch >>= 2; macro_height *= 4; }
    u64 macro_tiles_per_row = pitch / macro_pitch;
    u64 macro_tile_bytes = (micro_tile_bits / 64 * macro_height * macro_pitch + 7) / 8;
    u64 macro_tile_offset = ((x / macro_pitch) + macro_tiles_per_row * (y / macro_height)) * macro_tile_bytes;

    u64 group_mask = (1u << GROUP_BITS) - 1;
    u64 total = elem_offset + (macro_tile_offset >> (BANK_BITS + PIPE_BITS));
    u64 high = (total & ~group_mask) << (BANK_BITS + PIPE_BITS);
    u64 low = total & group_mask;
    return ((u64)bank << (PIPE_BITS + GROUP_BITS)) | ((u64)pipe << GROUP_BITS) | low | high;
}

// --- Pixel format decoding -------------------------------------------------

static void rgb565(u16 c, u8* out) {
    out[0] = (u8)(((c >> 11) & 31) * 255 / 31);
    out[1] = (u8)(((c >> 5) & 63) * 255 / 63);
    out[2] = (u8)((c & 31) * 255 / 31);
}

static void bc1_colors(const u8* b, u8 pal[4][4], bool allow_alpha) {
    u16 c0 = (u16)(b[0] | (b[1] << 8)), c1 = (u16)(b[2] | (b[3] << 8));
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !allow_alpha) {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (u8)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (u8)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) { pal[2][i] = (u8)((pal[0][i] + pal[1][i]) / 2); pal[3][i] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
}

static void bc4_values(const u8* b, u8 v[16]) {
    u8 pal[8];
    pal[0] = b[0]; pal[1] = b[1];
    if (pal[0] > pal[1]) {
        for (int i = 1; i < 7; i++) pal[i + 1] = (u8)(((7 - i) * pal[0] + i * pal[1]) / 7);
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = (u8)(((5 - i) * pal[0] + i * pal[1]) / 5);
        pal[6] = 0; pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) v[i] = pal[(bits >> (3 * i)) & 7];
}

static void decode_block(u32 hw, const u8* b, u8 px[16][4]) {
    u8 pal[4][4], a[16], g[16];
    switch (hw) {
    case 0x31: {   // BC1
        bc1_colors(b, pal, true);
        u32 idx = (u32)(b[4] | (b[5] << 8) | (b[6] << 16) | ((u32)b[7] << 24));
        for (int i = 0; i < 16; i++) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
        break;
    }
    case 0x32: case 0x33: {   // BC2 / BC3: alpha block, then a BC1 color block
        bc1_colors(b + 8, pal, false);
        u32 idx = (u32)(b[12] | (b[13] << 8) | (b[14] << 16) | ((u32)b[15] << 24));
        if (hw == 0x32) for (int i = 0; i < 16; i++) a[i] = (u8)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
        else bc4_values(b, a);
        for (int i = 0; i < 16; i++) { memcpy(px[i], pal[(idx >> (2 * i)) & 3], 3); px[i][3] = a[i]; }
        break;
    }
    case 0x34:   // BC4: one channel
        bc4_values(b, a);
        for (int i = 0; i < 16; i++) { px[i][0] = px[i][1] = px[i][2] = a[i]; px[i][3] = 255; }
        break;
    default:     // 0x35 BC5: two channels (usually a normal map)
        bc4_values(b, a);
        bc4_values(b + 8, g);
        for (int i = 0; i < 16; i++) { px[i][0] = a[i]; px[i][1] = g[i]; px[i][2] = 255; px[i][3] = 255; }
        break;
    }
}

static bool decode_pixel(u32 hw, const u8* p, u8* out) {
    switch (hw) {
    case 0x01: out[0] = out[1] = out[2] = p[0]; out[3] = 255; return true;
    case 0x02: out[0] = out[1] = out[2] = (u8)((p[0] & 15) * 17); out[3] = 255; return true;
    case 0x07: out[0] = p[0]; out[1] = p[1]; out[2] = 255; out[3] = 255; return true;
    case 0x08: rgb565((u16)(p[0] | (p[1] << 8)), out); out[3] = 255; return true;
    case 0x0A: {
        u16 c = (u16)(p[0] | (p[1] << 8));
        out[0] = (u8)((c & 31) * 255 / 31); out[1] = (u8)(((c >> 5) & 31) * 255 / 31);
        out[2] = (u8)(((c >> 10) & 31) * 255 / 31); out[3] = (c >> 15) ? 255 : 0;
        return true;
    }
    case 0x0B: {
        u16 c = (u16)(p[0] | (p[1] << 8));
        out[0] = (u8)((c & 15) * 17); out[1] = (u8)(((c >> 4) & 15) * 17);
        out[2] = (u8)(((c >> 8) & 15) * 17); out[3] = (u8)(((c >> 12) & 15) * 17);
        return true;
    }
    case 0x19: {
        u32 c = (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24));
        out[0] = (u8)((c & 1023) >> 2); out[1] = (u8)(((c >> 10) & 1023) >> 2);
        out[2] = (u8)(((c >> 20) & 1023) >> 2); out[3] = (u8)((c >> 30) * 85);
        return true;
    }
    case 0x1A: memcpy(out, p, 4); return true;
    default: return false;
    }
}

static u32 align_up_u32(u32 v, u32 a) { return (v + a - 1) / a * a; }

// Row pitch in elements. The pitch stored in the surface isn't always in
// elements (Splatoon's compressed textures store it in pixels), so like
// Cemu we work out what the address library would choose, and use whichever
// candidate matches the size of the image data.
static u32 pick_pitch(const Gx2Surface* s, u32 ew, u32 eh, u32 bpp, bool block) {
    u32 align, height_align;
    switch (s->tile_mode) {
    case 4: {   // 2D tiled: whole macro tiles (32 wide, more for small elements)
        u32 factor = 256 / bpp / 8;
        align = factor > 1 ? 32 * factor : 32;
        height_align = 16;
        break;
    }
    case 2: case 3:   // 1D tiled: whole 8x8 micro tiles
        align = 256 / bpp > 8 ? 256 / bpp : 8;
        height_align = 8;
        break;
    case 1:     // linear aligned
        align = 2048 / bpp > 64 ? 2048 / bpp : 64;
        height_align = 1;
        break;
    default:
        align = 1;
        height_align = 1;
        break;
    }
    u32 candidates[3] = { align_up_u32(ew, align), s->pitch, block ? s->pitch / 4 : 0 };
    u32 best = 0;
    u64 best_need = 0;
    // The address library's own answer wins whenever the data is big enough
    // for it (the image size can include extra slices or padding).
    bool computed_fits = (u64)candidates[0] * align_up_u32(eh, height_align) * (bpp / 8) <= s->image_size;
    if (computed_fits) best = candidates[0];
    for (int i = 1; i < 3 && !computed_fits; i++) {
        u32 c = candidates[i];
        if (c < ew) continue;
        u64 need = (u64)c * align_up_u32(eh, height_align) * (bpp / 8);
        if (need <= s->image_size && need > best_need) { best = c; best_need = need; }
    }
    if (!best) best = candidates[0];
    static int logged = 0;
    if (best != s->pitch && logged < 4) {
        logged++;
        printf("[gx2] %ux%u texture (format %03X, tile mode %u, dim %u, depth %u, %u bytes, swizzle %X): "
               "stored pitch %u, using %u\n", s->width, s->height, s->format, s->tile_mode, s->dim, s->depth,
               s->image_size, s->swizzle, s->pitch, best);
    }
    return best;
}

bool gx2_texture_supported(const Gx2Surface* s) {
    bool block;
    u32 hw = s->format & 0x3F;
    u32 bits = element_bits(hw, &block);
    if (!bits || s->width == 0 || s->height == 0 || s->width > 8192 || s->height > 8192) return false;
    if (!block && !(hw == 0x01 || hw == 0x02 || hw == 0x07 || hw == 0x08 || hw == 0x0A ||
                    hw == 0x0B || hw == 0x19 || hw == 0x1A)) return false;
    if (s->aa) return false;
    u32 m = s->tile_mode;
    return m <= 2 || m == 4;
}

u8* gx2_texture_to_rgba(const Gx2Surface* s) {
    if (!gx2_texture_supported(s)) return NULL;
    bool block;
    u32 hw = s->format & 0x3F;
    u32 bpp = element_bits(hw, &block);
    u32 bytes = bpp / 8;
    u32 ew = block ? (s->width + 3) / 4 : s->width;    // size in elements
    u32 eh = block ? (s->height + 3) / 4 : s->height;
    u32 pitch = pick_pitch(s, ew, eh, bpp, block);
    const u8* src = wiiu_mem_ptr(s->image, s->image_size ? s->image_size : 1);
    if (!src || !s->image_size) return NULL;

    // (Smaller mip levels can fall back to simpler tiling; level 0, the
    // only one we read, always uses the surface's own tile mode.)
    u32 mode = s->tile_mode;
    u32 pipe_swizzle = (s->swizzle >> 8) & 1, bank_swizzle = (s->swizzle >> 9) & 3;

    u8* rgba = (u8*)malloc((size_t)s->width * s->height * 4);
    if (!rgba) return NULL;
    memset(rgba, 0, (size_t)s->width * s->height * 4);
    for (u32 y = 0; y < eh; y++) {
        for (u32 x = 0; x < ew; x++) {
            u64 off;
            if (mode == 4) off = addr_macro_tiled(x, y, bpp, pitch, mode, pipe_swizzle, bank_swizzle);
            else if (mode == 2) off = addr_micro_tiled(x, y, bpp, pitch);
            else off = ((u64)y * pitch + x) * bytes;
            if (off + bytes > s->image_size) continue;
            const u8* e = src + off;
            if (block) {
                u8 px[16][4];
                decode_block(hw, e, px);
                for (u32 i = 0; i < 16; i++) {
                    u32 px_x = x * 4 + (i & 3), px_y = y * 4 + (i >> 2);
                    if (px_x < s->width && px_y < s->height)
                        memcpy(rgba + ((size_t)px_y * s->width + px_x) * 4, px[i], 4);
                }
            } else {
                decode_pixel(hw, e, rgba + ((size_t)y * s->width + x) * 4);
            }
        }
    }
    return rgba;
}
