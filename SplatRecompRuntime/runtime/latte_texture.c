// Reading GPU surfaces out of game memory (see latte_texture.h).
//
// The Wii U GPU stores most surfaces "tiled": pixels are grouped into 8x8
// micro tiles, and those into macro tiles spread over memory banks and
// pipes. The address math follows the public description of AMD's R600
// address library: 2 pipes, 4 banks, 256-byte pipe interleave.
#include "latte_texture.h"
#include "wiiu_mem.h"
#include "volk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CONV_NONE = 0, CONV_565, CONV_5551, CONV_4444, CONV_1555, CONV_R4G4 };

static bool set(LatteTexFormat* o, VkFormat f, u32 bits, bool block, u8 conv) {
    o->vk_format = (int)f;
    o->src_bits = bits;
    o->block = block;
    o->convert = conv;
    o->elem_bytes = conv ? 4 : bits / 8;
    return true;
}

bool latte_tex_format(u32 fmt, LatteTexFormat* o) {
    u32 hw = fmt & 0x3F, fl = fmt & 0xF00;
    bool uint_ = (fl & 0x300) == 0x100, sint_ = (fl & 0x300) == 0x300, snorm = (fl & 0x300) == 0x200, srgb = (fl & 0x400) != 0;
    switch (hw) {
    case 0x01: return set(o, uint_ ? VK_FORMAT_R8_UINT : sint_ ? VK_FORMAT_R8_SINT : snorm ? VK_FORMAT_R8_SNORM : srgb ? VK_FORMAT_R8_SRGB : VK_FORMAT_R8_UNORM, 8, false, 0);
    case 0x02: return set(o, VK_FORMAT_R8G8B8A8_UNORM, 8, false, CONV_R4G4);
    case 0x05: return set(o, uint_ ? VK_FORMAT_R16_UINT : sint_ ? VK_FORMAT_R16_SINT : snorm ? VK_FORMAT_R16_SNORM : VK_FORMAT_R16_UNORM, 16, false, 0);
    case 0x06: return set(o, VK_FORMAT_R16_SFLOAT, 16, false, 0);
    case 0x07: return set(o, uint_ ? VK_FORMAT_R8G8_UINT : sint_ ? VK_FORMAT_R8G8_SINT : snorm ? VK_FORMAT_R8G8_SNORM : srgb ? VK_FORMAT_R8G8_SRGB : VK_FORMAT_R8G8_UNORM, 16, false, 0);
    case 0x08: return set(o, VK_FORMAT_R8G8B8A8_UNORM, 16, false, CONV_565);
    case 0x0A: return set(o, VK_FORMAT_R8G8B8A8_UNORM, 16, false, CONV_5551);
    case 0x0B: return set(o, VK_FORMAT_R8G8B8A8_UNORM, 16, false, CONV_4444);
    case 0x0C: return set(o, VK_FORMAT_R8G8B8A8_UNORM, 16, false, CONV_1555);
    case 0x0D: return set(o, sint_ ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT, 32, false, 0);
    case 0x0E: return set(o, VK_FORMAT_R32_SFLOAT, 32, false, 0);
    case 0x0F: return set(o, uint_ ? VK_FORMAT_R16G16_UINT : sint_ ? VK_FORMAT_R16G16_SINT : snorm ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM, 32, false, 0);
    case 0x10: return set(o, VK_FORMAT_R16G16_SFLOAT, 32, false, 0);
    case 0x11: case 0x12: case 0x13: case 0x14: return set(o, VK_FORMAT_R32_UINT, 32, false, 0);
    case 0x15: case 0x16: case 0x17: case 0x18: return set(o, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 32, false, 0);
    case 0x19: case 0x1B: return set(o, uint_ ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32, 32, false, 0);
    case 0x1A: return set(o, uint_ ? VK_FORMAT_R8G8B8A8_UINT : sint_ ? VK_FORMAT_R8G8B8A8_SINT : snorm ? VK_FORMAT_R8G8B8A8_SNORM : srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 32, false, 0);
    case 0x1C: return set(o, VK_FORMAT_R32G32_UINT, 64, false, 0);
    case 0x1D: return set(o, sint_ ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT, 64, false, 0);
    case 0x1E: return set(o, VK_FORMAT_R32G32_SFLOAT, 64, false, 0);
    case 0x1F: return set(o, uint_ ? VK_FORMAT_R16G16B16A16_UINT : sint_ ? VK_FORMAT_R16G16B16A16_SINT : snorm ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM, 64, false, 0);
    case 0x20: return set(o, VK_FORMAT_R16G16B16A16_SFLOAT, 64, false, 0);
    case 0x22: return set(o, sint_ ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT, 128, false, 0);
    case 0x23: return set(o, VK_FORMAT_R32G32B32A32_SFLOAT, 128, false, 0);
    case 0x31: return set(o, srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 64, true, 0);
    case 0x32: return set(o, srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK, 128, true, 0);
    case 0x33: return set(o, srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 128, true, 0);
    case 0x34: return set(o, snorm ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK, 64, true, 0);
    case 0x35: return set(o, snorm ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK, 128, true, 0);
    default: return false;
    }
}

bool latte_depth_format(u32 fmt, int* vk, bool* stencil) {
    switch (fmt) {
    case 0x005: *vk = VK_FORMAT_D16_UNORM; *stencil = false; return true;
    case 0x011: *vk = VK_FORMAT_D24_UNORM_S8_UINT; *stencil = true; return true;
    case 0x80E: *vk = VK_FORMAT_D32_SFLOAT; *stencil = false; return true;
    case 0x81C: case 0x811: *vk = VK_FORMAT_D32_SFLOAT_S8_UINT; *stencil = true; return true;
    default: return false;
    }
}

// --- Tiling ------------------------------------------------------------------------------

enum { PIPES = 2, BANKS = 4, GROUP_BITS = 8, PIPE_BITS = 1, BANK_BITS = 2 };

static u32 tile_thickness(u32 mode) {
    return (mode == 3 || mode == 7 || mode == 11 || mode == 13 || mode == 15) ? 4 : 1;
}

// Position of a pixel inside its 8x8 (x thickness) micro tile.
static u32 pixel_index(u32 x, u32 y, u32 z, u32 bpp, u32 thickness) {
    u32 x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    u32 y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    u32 z0 = z & 1, z1 = (z >> 1) & 1;
    u32 b[8] = { 0 };
    if (thickness == 1) {
        switch (bpp) {
        case 8: b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y1; b[4] = y0; b[5] = y2; break;
        case 16: b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y0; b[4] = y1; b[5] = y2; break;
        case 64: b[0] = x0; b[1] = y0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2; break;
        case 128: b[0] = y0; b[1] = x0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2; break;
        default: b[0] = x0; b[1] = x1; b[2] = y0; b[3] = x2; b[4] = y1; b[5] = y2; break;   // 32
        }
    } else {
        switch (bpp) {
        case 8: case 16: b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y0; b[4] = y1; b[5] = z0; b[6] = y2; b[7] = z1; break;
        case 64: case 128: b[0] = x0; b[1] = y0; b[2] = x1; b[3] = x2; b[4] = z0; b[5] = y1; b[6] = y2; b[7] = z1; break;
        default: b[0] = x0; b[1] = x1; b[2] = y0; b[3] = x2; b[4] = y1; b[5] = z0; b[6] = y2; b[7] = z1; break;   // 32
        }
    }
    return b[0] | (b[1] << 1) | (b[2] << 2) | (b[3] << 3) | (b[4] << 4) | (b[5] << 5) | (b[6] << 6) | (b[7] << 7);
}

static u64 addr_micro(u32 x, u32 y, u32 slice, u32 bpp, u32 pitch, u32 height, u32 thickness) {
    u64 micro_tile_bytes = (64ull * thickness * bpp + 7) / 8;
    u64 slice_bytes = ((u64)pitch * height * thickness * bpp + 7) / 8;
    u64 slice_offset = slice_bytes * (slice / thickness);
    u64 tile = (x >> 3) + (u64)(y >> 3) * (pitch >> 3);
    return slice_offset + micro_tile_bytes * tile + ((u64)bpp * pixel_index(x, y, slice, bpp, thickness) >> 3);
}

static u32 macro_aspect(u32 mode) {
    if (mode == 5 || mode == 9) return 2;
    if (mode == 6 || mode == 10) return 4;
    return 1;
}

// Bank-swapped tilings (2B/3B): how wide a run of macro tiles is before the
// banks swap (AMD AddrLib's ComputeSurfaceBankSwappedWidth, 1 sample).
static u32 bank_swap_width(u32 mode, u32 bpp, u32 pitch) {
    u32 bytes_per_sample = 8 * bpp, num_samples = 1, slices_per_tile = 1;
    if (mode == 11 || mode == 15) num_samples = 4;   // thick
    u32 bytes_per_tile_slice = num_samples * bytes_per_sample / slices_per_tile;
    u32 swap_tiles = (256 >> 1) / bpp;
    if (swap_tiles < 1) swap_tiles = 1;
    u32 swap_width = swap_tiles * 8 * BANKS;
    u32 height_bytes = num_samples * macro_aspect(mode) * PIPES * bpp / slices_per_tile;
    u32 swap_max = PIPES * BANKS * 2048 / height_bytes;
    u32 swap_min = 256 * 8 * BANKS / bytes_per_tile_slice;
    u32 v = swap_max >= swap_width ? (swap_width > swap_min ? swap_width : swap_min) : swap_max;
    while (v >= 2 * pitch && v > 1) v >>= 1;
    return v;
}

static u64 addr_macro(u32 x, u32 y, u32 slice, u32 bpp, u32 pitch, u32 height, u32 mode, u32 pipe_swizzle, u32 bank_swizzle) {
    u32 thickness = tile_thickness(mode);
    u64 micro_tile_bits = (u64)bpp * 64 * thickness;
    u64 elem_offset = ((u64)bpp * pixel_index(x, y, slice, bpp, thickness) + 7) / 8;
    u32 pipe = ((y >> 3) ^ (x >> 3)) & 1;
    u32 bank = (((y / (16 * PIPES)) ^ (x >> 3)) & 1) | ((((y / (8 * PIPES)) ^ (x >> 4)) & 1) << 1);
    u32 bank_pipe = pipe + PIPES * bank;
    // each slice is rotated through the banks (AddrLib: swizzle + slice * rotation)
    u32 rotation = mode >= 12 ? 1 : PIPES * ((BANKS >> 1) - 1);
    u32 slice_in = thickness > 1 ? slice >> 2 : slice;
    bank_pipe ^= (pipe_swizzle + PIPES * bank_swizzle) + slice_in * rotation;
    bank_pipe %= PIPES * BANKS;
    pipe = bank_pipe % PIPES;
    bank = bank_pipe / PIPES;
    u32 macro_pitch = 8 * BANKS, macro_height = 8 * PIPES;
    if (mode == 5 || mode == 9) { macro_pitch >>= 1; macro_height *= 2; }
    else if (mode == 6 || mode == 10) { macro_pitch >>= 2; macro_height *= 4; }
    u64 macro_tiles_per_row = pitch / macro_pitch;
    u64 macro_tile_bytes = (micro_tile_bits / 64 * macro_height * macro_pitch + 7) / 8;
    u64 macro_tile_offset = ((x / macro_pitch) + macro_tiles_per_row * (y / macro_height)) * macro_tile_bytes;
    if (mode >= 8 && mode <= 11 || mode == 14 || mode == 15) {   // bank swapped
        static const u32 order[4] = { 0, 1, 3, 2 };
        u32 bsw = bank_swap_width(mode, bpp, pitch);
        u32 swap_index = bsw ? macro_pitch * (x / macro_pitch) / bsw : 0;
        bank ^= order[swap_index & (BANKS - 1)];
    }
    u64 slice_bytes = ((u64)pitch * height * thickness * bpp + 7) / 8;
    u64 slice_offset = slice_bytes * (slice / thickness);
    u64 group_mask = (1u << GROUP_BITS) - 1;
    u64 total = elem_offset + ((macro_tile_offset + slice_offset) >> (BANK_BITS + PIPE_BITS));
    u64 high = (total & ~group_mask) << (BANK_BITS + PIPE_BITS);
    u64 low = total & group_mask;
    return ((u64)bank << (PIPE_BITS + GROUP_BITS)) | ((u64)pipe << GROUP_BITS) | low | high;
}

static u32 align_up(u32 v, u32 a) { return (v + a - 1) / a * a; }

// Pitch in elements, kept sane.
static u32 elem_pitch(const LatteTexDesc* d, const LatteTexFormat* f, u32 ew) {
    u32 p = d->pitch ? d->pitch : d->width;
    if (f->block) p = (p + 3) / 4;
    if (p < ew) p = ew;
    u32 mode = d->tile_mode;
    if (mode >= 4 && mode != 16) p = align_up(p, 32);
    else if (mode == 2 || mode == 3) p = align_up(p, 8);
    return p;
}

// Rows per slice as laid out in memory.
static u32 slice_rows(u32 mode, u32 eh) {
    if (mode <= 1 || mode == 16) return eh;
    if (mode == 2 || mode == 3) return align_up(eh, 8);
    u32 mh = 8 * PIPES;
    if (mode == 5 || mode == 9) mh *= 2;
    else if (mode == 6 || mode == 10) mh *= 4;
    return align_up(eh, mh);
}

u32 latte_tex_span(const LatteTexDesc* d, const LatteTexFormat* f) {
    u32 ew = f->block ? (d->width + 3) / 4 : d->width, eh = f->block ? (d->height + 3) / 4 : d->height;
    u32 p = elem_pitch(d, f, ew);
    u32 rows = d->rows ? d->rows : slice_rows(d->tile_mode, eh);
    u32 th = tile_thickness(d->tile_mode);
    u32 slices = d->slices ? d->slices : 1;
    u32 nslab = (slices + th - 1) / th;
    u64 bytes = (u64)p * rows * th * f->src_bits / 8 * nslab;
    return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)bytes;
}

// --- Pixel conversions ----------------------------------------------------------------------

static void convert(u8 conv, const u8* s, u8* o) {
    u32 v = (u32)s[0] | ((u32)s[1] << 8);
    switch (conv) {
    case CONV_565:
        o[0] = (u8)((v & 31) * 255 / 31); o[1] = (u8)(((v >> 5) & 63) * 255 / 63); o[2] = (u8)(((v >> 11) & 31) * 255 / 31); o[3] = 255;
        break;
    case CONV_5551:
        o[0] = (u8)((v & 31) * 255 / 31); o[1] = (u8)(((v >> 5) & 31) * 255 / 31); o[2] = (u8)(((v >> 10) & 31) * 255 / 31); o[3] = (v >> 15) ? 255 : 0;
        break;
    case CONV_4444:
        o[0] = (u8)((v & 15) * 17); o[1] = (u8)(((v >> 4) & 15) * 17); o[2] = (u8)(((v >> 8) & 15) * 17); o[3] = (u8)(((v >> 12) & 15) * 17);
        break;
    case CONV_1555:
        o[3] = (v & 1) ? 255 : 0; o[2] = (u8)(((v >> 1) & 31) * 255 / 31); o[1] = (u8)(((v >> 6) & 31) * 255 / 31); o[0] = (u8)(((v >> 11) & 31) * 255 / 31);
        break;
    case CONV_R4G4:
        o[0] = (u8)((s[0] & 15) * 17); o[1] = (u8)((s[0] >> 4) * 17); o[2] = 0; o[3] = 255;
        break;
    }
}


// --- Fast tiled addresses -------------------------------------------------------------------------
// addr_macro / addr_micro work a whole address out per element (~75 ns);
// decoding a scene's textures that way took 130-250 ms at every level load.
// Within a micro tile only the element's position inside the 8x8 tile
// changes (a 64-entry table per bits-per-pixel), everything else is the
// same for the 8 elements of a row span: spans are computed once.
static const u8* pixel_index_table(u32 bpp, u32 thickness, u32 z) {
    static u8 tabs[5][2][4][64];
    static bool made[5][2][4];
    u32 bi = bpp == 8 ? 0 : bpp == 16 ? 1 : bpp == 64 ? 3 : bpp == 128 ? 4 : 2;
    u32 ti = thickness > 1 ? 1 : 0, zi = thickness > 1 ? (z & 3) : 0;
    if (!made[bi][ti][zi]) {
        for (u32 y = 0; y < 8; y++)
            for (u32 x = 0; x < 8; x++) tabs[bi][ti][zi][y * 8 + x] = (u8)pixel_index(x, y, z, bpp, thickness);
        made[bi][ti][zi] = true;
    }
    return tabs[bi][ti][zi];
}
void latte_tex_prepare(void) {
    static const u32 bpps[5] = { 8, 16, 32, 64, 128 };
    for (u32 b = 0; b < 5; b++)
        for (u32 z = 0; z < 4; z++) { (void)pixel_index_table(bpps[b], 1, z); (void)pixel_index_table(bpps[b], 4, z); }
}
// The 8 addresses of elements x0..x0+7 (x0 a multiple of 8) in row y.
static void addr_micro_span(u32 x0, u32 y, u32 slice, u32 bpp, u32 pitch, u32 height, u32 thickness, u64 out[8]) {
    u64 micro_tile_bytes = (64ull * thickness * bpp + 7) / 8;
    u64 slice_bytes = ((u64)pitch * height * thickness * bpp + 7) / 8;
    u64 base = slice_bytes * (slice / thickness) + micro_tile_bytes * ((x0 >> 3) + (u64)(y >> 3) * (pitch >> 3));
    const u8* pt = pixel_index_table(bpp, thickness, slice) + (y & 7) * 8;
    for (u32 i = 0; i < 8; i++) out[i] = base + (((u64)bpp * pt[i]) >> 3);
}
static void addr_macro_span(u32 x0, u32 y, u32 slice, u32 bpp, u32 pitch, u32 height, u32 mode, u32 pipe_swizzle, u32 bank_swizzle, u64 out[8]) {
    u32 thickness = tile_thickness(mode);
    u64 micro_tile_bits = (u64)bpp * 64 * thickness;
    u32 x = x0;
    u32 pipe = ((y >> 3) ^ (x >> 3)) & 1;
    u32 bank = (((y / (16 * PIPES)) ^ (x >> 3)) & 1) | ((((y / (8 * PIPES)) ^ (x >> 4)) & 1) << 1);
    u32 bank_pipe = pipe + PIPES * bank;
    u32 rotation = mode >= 12 ? 1 : PIPES * ((BANKS >> 1) - 1);
    u32 slice_in = thickness > 1 ? slice >> 2 : slice;
    bank_pipe ^= (pipe_swizzle + PIPES * bank_swizzle) + slice_in * rotation;
    bank_pipe %= PIPES * BANKS;
    pipe = bank_pipe % PIPES;
    bank = bank_pipe / PIPES;
    u32 macro_pitch = 8 * BANKS, macro_height = 8 * PIPES;
    if (mode == 5 || mode == 9) { macro_pitch >>= 1; macro_height *= 2; }
    else if (mode == 6 || mode == 10) { macro_pitch >>= 2; macro_height *= 4; }
    u64 macro_tiles_per_row = pitch / macro_pitch;
    u64 macro_tile_bytes = (micro_tile_bits / 64 * macro_height * macro_pitch + 7) / 8;
    u64 macro_tile_offset = ((x / macro_pitch) + macro_tiles_per_row * (y / macro_height)) * macro_tile_bytes;
    if (mode >= 8 && mode <= 11 || mode == 14 || mode == 15) {
        static const u32 order[4] = { 0, 1, 3, 2 };
        u32 bsw = bank_swap_width(mode, bpp, pitch);
        u32 swap_index = bsw ? macro_pitch * (x / macro_pitch) / bsw : 0;
        bank ^= order[swap_index & (BANKS - 1)];
    }
    u64 slice_bytes = ((u64)pitch * height * thickness * bpp + 7) / 8;
    u64 slice_offset = slice_bytes * (slice / thickness);
    u64 group_mask = (1u << GROUP_BITS) - 1;
    u64 base = (macro_tile_offset + slice_offset) >> (BANK_BITS + PIPE_BITS);
    u64 bp = ((u64)bank << (PIPE_BITS + GROUP_BITS)) | ((u64)pipe << GROUP_BITS);
    const u8* pt = pixel_index_table(bpp, thickness, slice) + (y & 7) * 8;
    for (u32 i = 0; i < 8; i++) {
        u64 total = (((u64)bpp * pt[i] + 7) / 8) + base;
        out[i] = bp | (total & group_mask) | ((total & ~group_mask) << (BANK_BITS + PIPE_BITS));
    }
}
static inline void addr_span(u32 mode, u32 x0, u32 y, u32 z, u32 bpp, u32 pitch, u32 rows, u32 th, u32 pipe_sw, u32 bank_sw, u64 out[8]) {
    if (mode == 2 || mode == 3) addr_micro_span(x0, y, z, bpp, pitch, rows, th, out);
    else addr_macro_span(x0, y, z, bpp, pitch, rows, mode, pipe_sw, bank_sw, out);
}
// SPLAT_TILE_SELFTEST=1: the spans against the element-by-element functions, once.
static void tile_selftest(void) {
    static int done = 0;
    if (done || !getenv("SPLAT_TILE_SELFTEST")) return;
    done = 1;
    static const u32 modes[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, bpps[] = { 8, 16, 32, 64, 128 };
    u32 bad = 0, n = 0, seed = 12345;
    for (u32 mi = 0; mi < 14; mi++)
        for (u32 bi = 0; bi < 5; bi++)
            for (u32 t = 0; t < 400; t++) {
                seed = seed * 1103515245u + 12345u;
                u32 mode = modes[mi], bpp = bpps[bi], th = tile_thickness(mode);
                u32 pitch = 32u << ((seed >> 4) % 6), rows = 16u << ((seed >> 8) % 6);
                u32 x0 = ((seed >> 12) % (pitch / 8)) * 8, y = (seed >> 20) % rows, z = (seed >> 27) % 8;
                u32 psw = (seed >> 3) & 1, bsw = (seed >> 1) & 3;
                u64 o[8];
                addr_span(mode, x0, y, z, bpp, pitch, rows, th, psw, bsw, o);
                for (u32 i = 0; i < 8; i++) {
                    u64 ref = (mode == 2 || mode == 3) ? addr_micro(x0 + i, y, z, bpp, pitch, rows, th) : addr_macro(x0 + i, y, z, bpp, pitch, rows, mode, psw, bsw);
                    n++;
                    if (ref != o[i] && bad++ < 8)
                        printf("[tile] mismatch mode %u bpp %u pitch %u rows %u x %u y %u z %u: span %llu, one by one %llu\n", mode, bpp, pitch, rows, x0 + i, y, z,
                               (unsigned long long)o[i], (unsigned long long)ref);
                }
            }
    printf("[tile] self-test: %u addresses, %u mismatches\n", n, bad);
}

// Tiled element offsets of one slice, remembered per layout: the buffers the
// game reads back are re-tiled every frame. (Per PC thread; separate caches
// for uploads (0) and readbacks (1), so an upload's mip levels don't push
// the ink maps' tables out.)
#if defined(_MSC_VER)
#define TT_TLS __declspec(thread)
#else
#define TT_TLS __thread
#endif
typedef struct { u32 mode, bpp, pitch, rows, ew, eh, th, pipe_sw, bank_sw, z, age; u32* offs; } TileTab;
#define TILETAB_SLOTS 12
static const u32* tile_offsets(int which, u32 mode, u32 bpp, u32 pitch, u32 rows, u32 ew, u32 eh, u32 th, u32 pipe_sw, u32 bank_sw, u32 z) {
    if ((u64)ew * eh > (1u << 18)) return NULL;   // (bigger ones: spans straight from the address functions)
    static TT_TLS TileTab tabs_all[2][TILETAB_SLOTS];
    static TT_TLS u32 clocks[2];
    TileTab* tabs = tabs_all[which & 1];
    u32 clock = ++clocks[which & 1];
    int pick = 0;
    for (int i = 0; i < TILETAB_SLOTS; i++) {
        TileTab* t = &tabs[i];
        if (t->offs && t->mode == mode && t->bpp == bpp && t->pitch == pitch && t->rows == rows && t->ew == ew && t->eh == eh &&
            t->th == th && t->pipe_sw == pipe_sw && t->bank_sw == bank_sw && t->z == z) { t->age = clock; return t->offs; }
        if (!tabs[pick].offs) continue;
        if (!t->offs || t->age < tabs[pick].age) pick = i;
    }
    TileTab* t = &tabs[pick];
    free(t->offs);
    t->offs = (u32*)malloc((size_t)ew * eh * sizeof(u32));
    if (!t->offs) return NULL;
    for (u32 y = 0; y < eh; y++)
        for (u32 x0 = 0; x0 < ew; x0 += 8) {
            u64 o[8];
            addr_span(mode, x0, y, z, bpp, pitch, rows, th, pipe_sw, bank_sw, o);
            for (u32 i = 0; i < 8 && x0 + i < ew; i++) t->offs[(size_t)y * ew + x0 + i] = o[i] > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (u32)o[i];
        }
    t->mode = mode; t->bpp = bpp; t->pitch = pitch; t->rows = rows; t->ew = ew; t->eh = eh;
    t->th = th; t->pipe_sw = pipe_sw; t->bank_sw = bank_sw; t->z = z; t->age = clock;
    return t->offs;
}

size_t latte_tex_decode(const LatteTexDesc* d, const LatteTexFormat* f, u8* dst, size_t cap) {
    return latte_tex_decode_slices(d, f, 0, d->slices ? d->slices : 1, dst, cap);
}

// One tiled row of a slice: offsets from the table when there is one, else
// from 8-element spans; fixed-size copies for the usual element sizes.
#define ROW_COPY_IN(N) \
    for (u32 i = 0; i < n; i++) { u64 o = offs[i]; u8* out = row + (size_t)(x0 + i) * (N); if (o + (N) <= span) memcpy(out, src + o, (N)); else memset(out, 0, (N)); }
static void decode_row_tiled(const u8* src, u32 span, u8* row, u32 ew, u32 y, u32 z, u32 bytes, const LatteTexFormat* f, const u32* tab,
                             u32 mode, u32 bpp, u32 pitch, u32 rows, u32 th, u32 pipe_sw, u32 bank_sw) {
    u64 o8[8];
    for (u32 x0 = 0; x0 < ew; x0 += 8) {
        u32 n = ew - x0 < 8 ? ew - x0 : 8;
        u64 offs[8];
        if (tab) { const u32* t = tab + (size_t)y * ew + x0; for (u32 i = 0; i < n; i++) offs[i] = t[i]; }
        else { addr_span(mode, x0, y, z, bpp, pitch, rows, th, pipe_sw, bank_sw, o8); for (u32 i = 0; i < n; i++) offs[i] = o8[i]; }
        if (f->convert) {
            for (u32 i = 0; i < n; i++) {
                u64 o = offs[i];
                u8* out = row + (size_t)(x0 + i) * f->elem_bytes;
                if (o + bytes <= span) convert(f->convert, src + o, out); else memset(out, 0, f->elem_bytes);
            }
            continue;
        }
        switch (bytes) {
        case 1: for (u32 i = 0; i < n; i++) { u64 o = offs[i]; row[x0 + i] = o < span ? src[o] : 0; } break;
        case 2: ROW_COPY_IN(2) break;
        case 4: ROW_COPY_IN(4) break;
        case 8: ROW_COPY_IN(8) break;
        case 16: ROW_COPY_IN(16) break;
        default:
            for (u32 i = 0; i < n; i++) { u64 o = offs[i]; u8* out = row + (size_t)(x0 + i) * bytes; if (o + bytes <= span) memcpy(out, src + o, bytes); else memset(out, 0, bytes); }
            break;
        }
    }
}
#undef ROW_COPY_IN

size_t latte_tex_decode_slices(const LatteTexDesc* d, const LatteTexFormat* f, u32 first, u32 count, u8* dst, size_t cap) {
    tile_selftest();
    u32 ew = f->block ? (d->width + 3) / 4 : d->width, eh = f->block ? (d->height + 3) / 4 : d->height;
    u32 bpp = f->src_bits, bytes = bpp / 8;
    u32 slices = first + count;
    size_t slice_size = (size_t)ew * eh * f->elem_bytes;
    size_t need = slice_size * count;
    if (need > cap || ew == 0 || eh == 0) return 0;
    u32 pitch = elem_pitch(d, f, ew);
    u32 span = latte_tex_span(d, f);
    const u8* src = wiiu_mem_ptr(d->addr, span);
    if (!src) {
        // partly outside mapped memory: read what we can
        span = 0;
        for (u32 s = latte_tex_span(d, f); s >= 4096; s /= 2)
            if (wiiu_mem_ptr(d->addr, s)) { span = s; src = wiiu_mem_ptr(d->addr, s); break; }
        if (!src) return 0;
    }
    u32 mode = d->tile_mode;
    u32 rows = d->rows ? d->rows : slice_rows(mode, eh);
    u32 th = tile_thickness(mode);
    u32 pipe_sw = (d->swizzle >> 8) & 1, bank_sw = (d->swizzle >> 9) & 3;
    (void)slices;
    bool linear = mode <= 1 || mode == 16;
    for (u32 z = first; z < first + count; z++) {
        // (uploads: a table only for small, often-repeated layouts; spans are as fast for the rest)
        const u32* tab = (!linear && (u64)ew * eh <= (1u << 14)) ? tile_offsets(0, mode, bpp, pitch, rows, ew, eh, th, pipe_sw, bank_sw, z) : NULL;
        u8* out_slice = dst + slice_size * (z - first);
        for (u32 y = 0; y < eh; y++) {
            u8* row = out_slice + (size_t)y * ew * f->elem_bytes;
            if (linear) {
                u64 off = ((u64)z * rows + y) * pitch * bytes;
                if (!f->convert && off + (u64)ew * bytes <= span) {
                    memcpy(row, src + off, (size_t)ew * bytes);
                    continue;
                }
                for (u32 x = 0; x < ew; x++) {
                    u64 o = off + (u64)x * bytes;
                    u8* out = row + (size_t)x * f->elem_bytes;
                    if (o + bytes > span) { memset(out, 0, f->elem_bytes); continue; }
                    if (f->convert) convert(f->convert, src + o, out); else memcpy(out, src + o, bytes);
                }
                continue;
            }
            decode_row_tiled(src, span, row, ew, y, z, bytes, f, tab, mode, bpp, pitch, rows, th, pipe_sw, bank_sw);
        }
    }
    return need;
}

// The other way round: tightly packed elements -> the game's tiled layout
// in memory (raw copies only: formats without a conversion).
#define ROW_COPY_OUT(N) \
    for (u32 i = 0; i < n; i++) { u64 o = offs[i]; if (o + (N) <= span) memcpy(dst + o, row + (size_t)(x0 + i) * (N), (N)); }
bool latte_tex_encode_slices(const LatteTexDesc* d, const LatteTexFormat* f, u32 first, u32 count, const u8* src, size_t size) {
    if (f->convert || f->elem_bytes * 8 != f->src_bits) return false;
    u32 ew = f->block ? (d->width + 3) / 4 : d->width, eh = f->block ? (d->height + 3) / 4 : d->height;
    u32 bpp = f->src_bits, bytes = bpp / 8;
    size_t slice_size = (size_t)ew * eh * bytes;
    if (slice_size * count > size || ew == 0 || eh == 0) return false;
    LatteTexDesc whole = *d;
    if (whole.slices < first + count) whole.slices = first + count;
    u32 span = latte_tex_span(&whole, f);
    u8* dst = wiiu_mem_ptr(d->addr, span);
    if (!dst) return false;
    u32 pitch = elem_pitch(d, f, ew);
    u32 mode = d->tile_mode;
    u32 rows = d->rows ? d->rows : slice_rows(mode, eh);
    u32 th = tile_thickness(mode);
    u32 pipe_sw = (d->swizzle >> 8) & 1, bank_sw = (d->swizzle >> 9) & 3;
    bool linear = mode <= 1 || mode == 16;
    for (u32 z = first; z < first + count; z++) {
        const u32* tab = linear ? NULL : tile_offsets(1, mode, bpp, pitch, rows, ew, eh, th, pipe_sw, bank_sw, z);
        const u8* in_slice = src + slice_size * (z - first);
        for (u32 y = 0; y < eh; y++) {
            const u8* row = in_slice + (size_t)y * ew * bytes;
            if (linear) {
                u64 off = ((u64)z * rows + y) * pitch * bytes;
                if (off + (u64)ew * bytes <= span) memcpy(dst + off, row, (size_t)ew * bytes);
                continue;
            }
            u64 o8[8];
            for (u32 x0 = 0; x0 < ew; x0 += 8) {
                u32 n = ew - x0 < 8 ? ew - x0 : 8;
                u64 offs[8];
                if (tab) { const u32* t = tab + (size_t)y * ew + x0; for (u32 i = 0; i < n; i++) offs[i] = t[i]; }
                else { addr_span(mode, x0, y, z, bpp, pitch, rows, th, pipe_sw, bank_sw, o8); for (u32 i = 0; i < n; i++) offs[i] = o8[i]; }
                switch (bytes) {
                case 1: for (u32 i = 0; i < n; i++) { u64 o = offs[i]; if (o < span) dst[o] = row[x0 + i]; } break;
                case 2: ROW_COPY_OUT(2) break;
                case 4: ROW_COPY_OUT(4) break;
                case 8: ROW_COPY_OUT(8) break;
                case 16: ROW_COPY_OUT(16) break;
                default: for (u32 i = 0; i < n; i++) { u64 o = offs[i]; if (o + bytes <= span) memcpy(dst + o, row + (size_t)(x0 + i) * bytes, bytes); } break;
                }
            }
        }
    }
    return true;
}
#undef ROW_COPY_OUT
