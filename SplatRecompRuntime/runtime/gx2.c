// GX2, the Wii U's graphics library (see gx2.h).
//
// Like the console's GX2, every call here turns into GPU command packets
// (PM4): register writes, draws, and a few of our own packets for clears,
// copies and presenting. The packets go to the GPU thread (latte_cp.c),
// which turns them into Vulkan work, or into a display list the game is
// recording. Packet layouts match the console's GX2 where games depend on
// them (display lists can be patched by offset); register values follow
// AMD's R600/R700 documentation, cross-checked against Cemu's GX2.
//
// Struct layouts used below (offsets in bytes):
//   GX2Surface (0x74): dim 0, width 4, height 8, depth C, numLevels 10,
//     format 14, aa 18, resFlag 1C, imageSize 20, imagePtr 24, mipSize 28,
//     mipPtr 2C, tileMode 30, swizzle 34, alignment 38, pitch 3C,
//     mipOffset[13] 40.
//   GX2ColorBuffer: surface, viewMip 74, viewFirstSlice 78, viewNumSlices
//     7C, aux 80/84, regs: size 88, info 8C, view 90, mask 94.
//   GX2DepthBuffer: surface, viewMip 74, firstSlice 78, numSlices 7C,
//     hiZ 80/84, clearDepth 88, clearStencil 8C, regs: size 90, view 94,
//     base 98, htile 9C, prefetch A0, preload A4, polyoffset fmt A8.
//   GX2Texture: surface, viewFirstMip 74, viewNumMips 78, viewFirstSlice 7C,
//     viewNumSlices 80, compSel 84, regs word0 88, word1 8C, word4 90,
//     word5 94, word6 98.
#include "gx2.h"
#include "gx2_cmd.h"
#include "latte_cp.h"
#include "latte_render.h"
#include "display.h"
#include "texture_gallery.h"
#include "wiiu_mem.h"
#include "guest_threads.h"
#include "hle.h"
#include "gx2_layout.h"
#include "host_thread.h"
#include "vk_ctx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
static float farg(CPUState* c, int i) { return (float)c->fpr[1 + i]; }
static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static void nothing(CPUState* c) { (void)c; }
static void gpu_version(CPUState* c) { ret(c, 2); }
static void zero(CPUState* c) { ret(c, 0); }
static void one(CPUState* c) { ret(c, 1); }

// ==== Surfaces =====================================================================

static void calc_surface(u32 s) {
    GX2Layout L;
    gx2_surface_layout(wiiu_read32(s + 0x00), wiiu_read32(s + 0x04), wiiu_read32(s + 0x08), wiiu_read32(s + 0x0C),
                       wiiu_read32(s + 0x10), wiiu_read32(s + 0x14), wiiu_read32(s + 0x18), wiiu_read32(s + 0x1C),
                       wiiu_read32(s + 0x30), wiiu_read32(s + 0x34), &L);
    for (u32 i = 0; i < 13; i++) wiiu_write32(s + 0x40 + i * 4, L.mip_offset[i]);
    wiiu_write32(s + 0x20, L.image_size);
    wiiu_write32(s + 0x28, L.mip_size);
    wiiu_write32(s + 0x30, L.mode);
    wiiu_write32(s + 0x34, L.swizzle);
    wiiu_write32(s + 0x38, L.align);
    wiiu_write32(s + 0x3C, L.level[0].pitch);
}
static void GX2CalcSurfaceSizeAndAlignment(CPUState* c) { calc_surface(arg(c, 0)); }

static void calc_scan(CPUState* c, u32 w, u32 h) {   // (mode, format, buffering, *size, *scaleNeeded)
    u32 buffers = arg(c, 2) ? arg(c, 2) : 2;
    if (arg(c, 3)) wiiu_write32(arg(c, 3), align_up(w * h * 4 * buffers, 0x1000));
    if (arg(c, 4)) wiiu_write32(arg(c, 4), 0);
}
static void GX2CalcTVSize(CPUState* c) {
    u32 mode = arg(c, 0), w = 1280, h = 720;
    if (mode == 1 || mode == 2) { w = 854; h = 480; }
    else if (mode >= 5) { w = 1920; h = 1080; }
    calc_scan(c, w, h);
}
static void GX2CalcDRCSize(CPUState* c) { calc_scan(c, 854, 480); }

static void aux_info(CPUState* c) {   // (buffer, *size, *align)
    u32 surf = arg(c, 0);
    u32 w = wiiu_read32(surf + 0x04), h = wiiu_read32(surf + 0x08);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), align_up((w / 8 + 1) * (h / 8 + 1) * 4 + 0x1000, 0x800));
    if (arg(c, 2)) wiiu_write32(arg(c, 2), 0x800);
}

static void GX2SetSurfaceSwizzle(CPUState* c) {   // (surface, swizzle)
    u32 s = arg(c, 0);
    u32 v = wiiu_read32(s + 0x34) & ~0xFF00u;
    wiiu_write32(s + 0x34, v | (arg(c, 1) << 8));
}
static void GX2GetSurfaceSwizzle(CPUState* c) { ret(c, (wiiu_read32(arg(c, 0) + 0x34) >> 8) & 0xFF); }
static void GX2GetSurfaceFormatBits(CPUState* c) { bool b; ret(c, gx2_format_bits(arg(c, 0), &b)); }
static void GX2SurfaceIsCompressed(CPUState* c) { u32 f = arg(c, 0) & 0x3F; ret(c, f >= 0x31 && f <= 0x35); }

// ==== Textures and samplers =======================================================

static void GX2InitTextureRegs(CPUState* c) {
    u32 t = arg(c, 0);
    if (!wiiu_read32(t + 0x78)) wiiu_write32(t + 0x78, 1);   // viewNumMips
    if (!wiiu_read32(t + 0x80)) wiiu_write32(t + 0x80, 1);   // viewNumSlices
    if (!wiiu_read32(t + 0x08)) wiiu_write32(t + 0x08, 1);
    if (!wiiu_read32(t + 0x0C)) wiiu_write32(t + 0x0C, 1);
    if (!wiiu_read32(t + 0x10)) wiiu_write32(t + 0x10, 1);
    u32 dim = wiiu_read32(t + 0x00), width = wiiu_read32(t + 0x04), height = wiiu_read32(t + 0x08);
    u32 depth = wiiu_read32(t + 0x0C), format = wiiu_read32(t + 0x14), aa = wiiu_read32(t + 0x18);
    u32 resflag = wiiu_read32(t + 0x1C), tile = wiiu_read32(t + 0x30), pitch = wiiu_read32(t + 0x3C);
    u32 first_mip = wiiu_read32(t + 0x74), num_mips = wiiu_read32(t + 0x78);
    u32 first_slice = wiiu_read32(t + 0x7C), num_slices = wiiu_read32(t + 0x80), comp_sel = wiiu_read32(t + 0x84);
    u32 hw = format & 0x3F;
    bool compressed = hw >= 0x31 && hw <= 0x35;

    u32 pixel_pitch = compressed ? pitch * 4 : pitch;
    u32 w0 = (dim & 7) | ((tile & 0xF) << 3) | (((resflag & 4) ? 1u : 0u) << 7);
    w0 |= (pixel_pitch ? ((pixel_pitch >> 3) - 1) & 0x7FF : 0x7FF) << 8;
    w0 |= (width ? (width - 1) & 0x1FFF : 0x1FFF) << 19;

    u32 d = 0;
    if (dim == 3) d = depth / 6 - 1;                                        // cube map
    else if (dim == 2 || dim == 4 || dim == 5 || dim == 7) d = depth - 1;   // 3D / arrays
    u32 w1 = ((height - 1) & 0x1FFF) | ((d & 0x1FFF) << 13) | (hw << 26);

    u32 comp = (format & FMT_BIT_SIGNED) ? 1 : 0;
    u32 w4 = comp | (comp << 2) | (comp << 4) | (comp << 6);
    w4 |= ((format & FMT_BIT_FLOAT) ? 2u : (format & FMT_BIT_INT) ? 1u : 0u) << 8;   // NUM_FORMAT_ALL
    if (format & FMT_BIT_SRGB) w4 |= 1u << 11;                                       // FORCE_DEGAMMA
    w4 |= 2u << 14;                                                                  // REQUEST_SIZE
    w4 |= ((comp_sel >> 24) & 7) << 16 | ((comp_sel >> 16) & 7) << 19 |
          ((comp_sel >> 8) & 7) << 22 | ((comp_sel >> 0) & 7) << 25;
    w4 |= (first_mip & 0xF) << 28;

    u32 last_level = first_mip + num_mips - 1;
    if (aa >= 1 && aa <= 3) last_level = aa;
    u32 w5 = (last_level & 0xF) | ((first_slice & 0x1FFF) << 4) | (((first_slice + num_slices - 1) & 0x1FFF) << 17);
    if (dim == 3 && depth / 6 - 1 != 0) w5 |= 1u << 30;

    u32 w6 = (4u << 2) | (7u << 5) | (2u << 30);   // max aniso, perf modulation, valid texture

    wiiu_write32(t + 0x88, w0);
    wiiu_write32(t + 0x8C, w1);
    wiiu_write32(t + 0x90, w4);
    wiiu_write32(t + 0x94, w5);
    wiiu_write32(t + 0x98, w6);
}

// (a guest word straight from memory: the whole 4 GB space is mapped, as the game's own code reads it --
// wiiu_read32's region search for each of the ~30 words a texture or uniform call reads was ~1% of the
// main thread)
extern u8* wiiu_fastmem;
static inline u32 grd(u32 a) {
    if (wiiu_fastmem) { u32 v; memcpy(&v, wiiu_fastmem + a, 4); return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
    return wiiu_read32(a);
}
static void set_texture(u32 t, u32 base_reg, u32 unit) {
    if (unit >= 18) return;   // (a "not found" unit from the game: nothing to set)
    gallery_note_texture(t);
    u32 image = grd(t + 0x24), mip = grd(t + 0x2C);
    if (!mip) mip = image;
    u32 tile = grd(t + 0x30), swizzle = grd(t + 0x34);
    if ((tile & 0xF) >= 4 && tile != 16) {   // macro tiled: swizzle combines with the addresses
        u32 stop = (swizzle >> 16) & 0xFF;
        if (stop > 0) image ^= swizzle & 0xFFFF;
        if (stop > 1) mip ^= swizzle & 0xFFFF;
    }
    {   // the texture's real mip offsets, for the renderer
        GX2MipInfo mi;
        mi.levels = grd(t + 0x10);
        mi.swizzle = swizzle;
        if (mi.levels > 1) {
            for (u32 i = 0; i < 13; i++) mi.offs[i] = grd(t + 0x40 + i * 4);
            gx2_note_mips(mip & ~0xFFu, &mi);
        }
    }
    u32 words[9] = { PM4_TYPE3(IT_SET_RESOURCE, 8), base_reg + unit * 7 - REG_BASE_RESOURCE,
                     grd(t + 0x88), grd(t + 0x8C), image >> 8, mip >> 8, grd(t + 0x90), grd(t + 0x94), grd(t + 0x98) };
    gx2_emit_n(words, 9);
}
static void GX2SetPixelTexture(CPUState* c) { set_texture(arg(c, 0), SQ_TEX_RESOURCE_PS, arg(c, 1)); }
static void GX2SetVertexTexture(CPUState* c) { set_texture(arg(c, 0), SQ_TEX_RESOURCE_VS, arg(c, 1)); }
static void GX2SetGeometryTexture(CPUState* c) { set_texture(arg(c, 0), SQ_TEX_RESOURCE_GS, arg(c, 1)); }

// Sampler: three words. word0: clamp x/y/z (3 bits each), mag filter 9,
// min filter 12, z filter 15, mip filter 17, max aniso 19, border type 22,
// array override 25, depth compare 26. word1: min lod 0, max lod 10,
// lod bias 20 (signed, 1/64 steps). word2: type bit 31.
static void GX2InitSampler(CPUState* c) {   // (sampler, clamp, filter)
    u32 s = arg(c, 0), clamp = arg(c, 1) & 7, filter = arg(c, 2) & 7;
    u32 w0 = clamp | (clamp << 3) | (clamp << 6) | (filter << 9) | (filter << 12) | (1u << 15) | (1u << 17) | (1u << 25);
    wiiu_write32(s + 0, w0);
    wiiu_write32(s + 4, 0x3FFu << 10);
    wiiu_write32(s + 8, 1u << 31);
}
static void GX2InitSamplerXYFilter(CPUState* c) {   // (sampler, mag, min, maxAniso)
    u32 s = arg(c, 0), mag = arg(c, 1) & 7, min = arg(c, 2) & 7, aniso = arg(c, 3) & 7;
    u32 w0 = wiiu_read32(s) & ~((7u << 9) | (7u << 12) | (7u << 19));
    if (aniso) { mag |= 4; min |= 4; }   // point/bilinear -> aniso point/bilinear
    wiiu_write32(s, w0 | (mag << 9) | (min << 12) | (aniso << 19));
}
static void GX2InitSamplerZMFilter(CPUState* c) {   // (sampler, z, mip)
    u32 s = arg(c, 0);
    u32 w0 = wiiu_read32(s) & ~((3u << 15) | (3u << 17));
    wiiu_write32(s, w0 | ((arg(c, 1) & 3) << 15) | ((arg(c, 2) & 3) << 17));
}
static void GX2InitSamplerLOD(CPUState* c) {   // (sampler, minLod, maxLod, bias)
    u32 s = arg(c, 0);
    float mn = farg(c, 0), mx = farg(c, 1), bias = farg(c, 2);
    if (mn < 0) mn = 0;
    if (mx > 16) mx = 16;
    int imin = (int)floorf(mn * 64.0f), imax = (int)floorf(mx * 64.0f), ib = (int)floorf(bias * 64.0f);
    imin = imin < 0 ? 0 : imin > 1023 ? 1023 : imin;
    imax = imax < 0 ? 0 : imax > 1023 ? 1023 : imax;
    ib = ib < -2048 ? -2048 : ib > 2047 ? 2047 : ib;
    wiiu_write32(s + 4, (u32)imin | ((u32)imax << 10) | (((u32)ib & 0xFFF) << 20));
}
static void GX2InitSamplerClamping(CPUState* c) {   // (sampler, x, y, z)
    u32 s = arg(c, 0);
    u32 w0 = wiiu_read32(s) & ~0x1FFu;
    wiiu_write32(s, w0 | (arg(c, 1) & 7) | ((arg(c, 2) & 7) << 3) | ((arg(c, 3) & 7) << 6));
}
static void GX2InitSamplerBorderType(CPUState* c) {
    u32 s = arg(c, 0);
    wiiu_write32(s, (wiiu_read32(s) & ~(3u << 22)) | ((arg(c, 1) & 3) << 22));
}
static void GX2InitSamplerDepthCompare(CPUState* c) {
    u32 s = arg(c, 0);
    wiiu_write32(s, (wiiu_read32(s) & ~(7u << 26)) | ((arg(c, 1) & 7) << 26));
}
static void set_sampler(u32 s, u32 index) {
    u32 w[5] = { PM4_TYPE3(IT_SET_SAMPLER, 4), index * 3, grd(s), grd(s + 4), grd(s + 8) };
    gx2_emit_n(w, 5);
}
static void GX2SetPixelSampler(CPUState* c) { if (arg(c, 1) < 18) set_sampler(arg(c, 0), arg(c, 1)); }
static void GX2SetVertexSampler(CPUState* c) { if (arg(c, 1) < 18) set_sampler(arg(c, 0), arg(c, 1) + LATTE_SAMPLER_BASE_VS); }
static void GX2SetGeometrySampler(CPUState* c) { if (arg(c, 1) < 18) set_sampler(arg(c, 0), arg(c, 1) + LATTE_SAMPLER_BASE_GS); }
static void border_color(CPUState* c, u32 base) {   // (index, r, g, b, a)
    u32 v[4] = { fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)) };
    gx2_set_regs(IT_SET_CONFIG_REG, REG_BASE_CONFIG, base + arg(c, 0) * 4, v, 4);
}
static void GX2SetPixelSamplerBorderColor(CPUState* c) { border_color(c, TD_PS_SAMPLER0_BORDER); }
static void GX2SetVertexSamplerBorderColor(CPUState* c) { border_color(c, TD_VS_SAMPLER0_BORDER); }
static void GX2SetGeometrySamplerBorderColor(CPUState* c) { border_color(c, TD_GS_SAMPLER0_BORDER); }

// ==== Color and depth buffers =====================================================

static u32 export_format(u32 format) {   // 0 = full precision export, 1 = normalized
    static const u8 table[0x40] = {
        1,1,1,1,1,0,1,1, 1,1,1,1,1,0,0,0, 1,0,0,0,0,1,1,1, 1,1,1,1,0,0,0,0,
        1,0,0,0,1,1,1,0, 0,0,0,0,1,0,0,0, 0,1,1,1,1,1,0,0, 0,0,0,0,0,0,0,0 };
    return table[format & 0x3F];
}

static void GX2InitColorBufferRegs(CPUState* c) {
    u32 b = arg(c, 0);
    u32 pitch = wiiu_read32(b + 0x3C), height = wiiu_read32(b + 0x08), format = wiiu_read32(b + 0x14);
    u32 tile = wiiu_read32(b + 0x30), aa = wiiu_read32(b + 0x18);
    bool block;
    gx2_format_bits(format, &block);
    if (block) pitch *= 4;
    if (pitch < 8) pitch = 8;
    u32 hgt = align_up(height ? height : 1, 8);
    u32 pitch_height = (hgt * pitch) >> 6;
    u32 size = (((pitch >> 3) - 1) & 0x3FF) | (((pitch_height - 1) & 0xFFFFF) << 10);
    u32 hw = format & 0x3F, high = format & 0xF00;
    u32 info = (hw << 2) | ((tile & 0xF) << 8);
    bool clamp_blend = false;
    switch (high) {
    case 0x000: clamp_blend = true; break;
    case 0x100: info |= 4u << 12; break;
    case 0x200: info |= 1u << 12; clamp_blend = true; break;
    case 0x300: info |= 5u << 12; break;
    case 0x400: info |= 6u << 12; clamp_blend = true; break;
    case 0x800: info |= 7u << 12; break;
    }
    if (hw == 0x0C || hw == 0x1B) info |= 2u << 16;   // component swap for 5_5_5_1 / 10_10_10_2
    if (aa) info |= 2u << 18;
    bool is_int = (format & FMT_BIT_INT) != 0;
    if (!is_int) info |= export_format(format) << 27;
    if (is_int || format == 0x11 || format == 0x811 || format == 0x81C) { info |= 1u << 22; clamp_blend = false; }
    if (clamp_blend) info |= 1u << 20;
    if (format & FMT_BIT_FLOAT) info |= 1u << 25;
    u32 first = wiiu_read32(b + 0x78), num = wiiu_read32(b + 0x7C);
    u32 view = (first & 0x7FF) | (((num + first - 1) & 0x7FF) << 13);
    if (tile == 16) view = 0;
    wiiu_write32(b + 0x88, size);
    wiiu_write32(b + 0x8C, info);
    wiiu_write32(b + 0x90, view);
    wiiu_write32(b + 0x94, 0);
}

static void GX2InitDepthBufferRegs(CPUState* c) {
    u32 b = arg(c, 0);
    u32 pitch = wiiu_read32(b + 0x3C), height = wiiu_read32(b + 0x08);
    if (pitch < 8) pitch = 8;
    u32 hgt = align_up(height ? height : 1, 8);
    u32 size = (((pitch >> 3) - 1) & 0x3FF) | (((((hgt * pitch) >> 6) - 1) & 0xFFFFF) << 10);
    wiiu_write32(b + 0x90, size);
}

static u32 surface_level_address(u32 s, u32 level) {
    if (level == 0) return wiiu_read32(s + 0x24);
    u32 mip = wiiu_read32(s + 0x2C);
    return level == 1 ? mip : mip + wiiu_read32(s + 0x40 + (level - 1) * 4);
}

// Our own packet: the logical size and layout of a color (target 0-7) or
// depth (target 8) buffer, which the registers alone don't carry.
static void emit_rt_info(u32 target, u32 s, u32 level) {
    u32 w = wiiu_read32(s + 0x04) >> level, h = wiiu_read32(s + 0x08) >> level;
    gx2_emit(PM4_TYPE3(IT_HLE_RT_INFO, 8));
    gx2_emit(target);
    gx2_emit(surface_level_address(s, level));
    gx2_emit(w ? w : 1);
    gx2_emit(h ? h : 1);
    gx2_emit(wiiu_read32(s + 0x14));   // format
    gx2_emit(wiiu_read32(s + 0x30));   // tile mode
    gx2_emit(wiiu_read32(s + 0x34));   // swizzle
    u32 depth = wiiu_read32(s + 0x0C);
    gx2_emit((wiiu_read32(s + 0x00) & 0xFF) | ((depth ? depth : 1) & 0xFFF) << 8);   // dim, slices
}

static void GX2SetColorBuffer(CPUState* c) {   // (colorBuffer, target)
    u32 b = arg(c, 0), target = arg(c, 1) & 7;
    u32 level = wiiu_read32(b + 0x74);
    u32 base = surface_level_address(b, level);
    u32 tile = wiiu_read32(b + 0x30), swizzle = wiiu_read32(b + 0x34);
    if ((tile & 0xF) >= 4 && tile != 16 && level < ((swizzle >> 16) & 0xFF)) base ^= swizzle & 0xFFFF;
    gx2_set_context_reg(CB_COLOR0_BASE + target, base >> 8);
    gx2_set_context_reg(CB_COLOR0_SIZE + target, wiiu_read32(b + 0x88));
    gx2_set_context_reg(CB_COLOR0_VIEW + target, wiiu_read32(b + 0x90));
    gx2_set_context_reg(CB_COLOR0_INFO + target, wiiu_read32(b + 0x8C));
    emit_rt_info(target, b, level);
}

static void GX2SetDepthBuffer(CPUState* c) {
    u32 b = arg(c, 0);
    u32 format = wiiu_read32(b + 0x14), tile = wiiu_read32(b + 0x30);
    u32 info = (tile & 0xF) << 15;
    switch (format) {
    case 0x005: info |= 1; break;   // D16
    case 0x011: info |= 3; break;   // D24S8
    case 0x80E: info |= 6; break;   // D32F
    case 0x81C: info |= 7; break;   // D32F S8
    case 0x811: info |= 5; break;   // D24S8 float
    default: break;
    }
    u32 first = wiiu_read32(b + 0x78), num = wiiu_read32(b + 0x7C);
    gx2_set_context_reg(DB_DEPTH_SIZE, wiiu_read32(b + 0x90));
    u32 regs[3] = { wiiu_read32(b + 0x24) >> 8, info, wiiu_read32(b + 0x24) >> 8 };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, DB_DEPTH_BASE, regs, 3);
    gx2_set_context_reg(DB_DEPTH_VIEW, (first & 0x7FF) | (((num + first - 1) & 0x7FF) << 13));
    emit_rt_info(8, b, wiiu_read32(b + 0x74));
}

// ==== Clears ==========================================================================

static void clear_regs(float depth, u32 stencil, u32 flags) {   // flags: 1 set depth reg, 2 set stencil reg
    if ((flags & 3) == 3) {
        u32 v[2] = { stencil & 0xFF, fbits(depth) };
        gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, DB_STENCIL_CLEAR, v, 2);
    } else if (flags & 1) {
        gx2_set_context_reg(DB_DEPTH_CLEAR, fbits(depth));
    } else if (flags & 2) {
        gx2_set_context_reg(DB_STENCIL_CLEAR, stencil & 0xFF);
    }
}

// Our clear packet: what (1 color, 2 depth, 4 stencil), then the color
// buffer's surface address/format/size/slice, the depth buffer's, the color
// as float bits, depth, stencil.
static void emit_clear(u32 what, u32 cb, const float rgba[4], u32 db, float depth, u32 stencil) {
    gx2_emit(PM4_TYPE3(IT_HLE_CLEAR, 17));
    gx2_emit(what);
    for (int k = 0; k < 2; k++) {
        u32 b = k == 0 ? cb : db;
        if (b) {
            u32 level = wiiu_read32(b + 0x74);
            gx2_emit(surface_level_address(b, level));
            gx2_emit(wiiu_read32(b + 0x14));
            gx2_emit(wiiu_read32(b + 0x04) >> level);
            gx2_emit(wiiu_read32(b + 0x08) >> level);
            gx2_emit(wiiu_read32(b + 0x78));
        } else {
            for (int i = 0; i < 5; i++) gx2_emit(0);
        }
    }
    for (int i = 0; i < 4; i++) gx2_emit(fbits(rgba[i]));
    gx2_emit(fbits(depth));
    gx2_emit(stencil & 0xFF);
}

static void GX2ClearColor(CPUState* c) {   // (colorBuffer, r, g, b, a)
    float rgba[4] = { farg(c, 0), farg(c, 1), farg(c, 2), farg(c, 3) };
    emit_clear(1, arg(c, 0), rgba, 0, 0, 0);
}
// (color, depth, r, g, b, a, depthValue, stencil, flags): flags 1 clear depth,
// 2 clear stencil, 4 set depth reg, 8 set stencil reg.
static void GX2ClearBuffersEx(CPUState* c) {
    float rgba[4] = { farg(c, 0), farg(c, 1), farg(c, 2), farg(c, 3) };
    float depth = farg(c, 4);
    u32 stencil = arg(c, 2), flags = arg(c, 3);
    clear_regs(depth, stencil, ((flags >> 2) & 3));
    emit_clear(1 | ((flags & 1) ? 2 : 0) | ((flags & 2) ? 4 : 0), arg(c, 0), rgba, arg(c, 1), depth, stencil);
}
static void GX2ClearDepthStencilEx(CPUState* c) {   // (depthBuffer, depth, stencil, flags)
    float rgba[4] = { 0, 0, 0, 0 };
    float depth = farg(c, 0);
    u32 stencil = arg(c, 1), flags = arg(c, 2);
    if (!arg(c, 0)) return;
    clear_regs(depth, stencil, ((flags >> 2) & 3));
    emit_clear(((flags & 1) ? 2 : 0) | ((flags & 2) ? 4 : 0), 0, rgba, arg(c, 0), depth, stencil);
}
static void GX2SetClearDepthStencil(CPUState* c) {   // (depthBuffer, depth, stencil)
    u32 b = arg(c, 0);
    float depth = farg(c, 0);
    if (b) { wiiu_write32(b + 0x88, fbits(depth)); wiiu_write32(b + 0x8C, arg(c, 1) & 0xFF); }
    clear_regs(depth, arg(c, 1), 3);
}
static void GX2SetClearDepth(CPUState* c) {
    if (arg(c, 0)) wiiu_write32(arg(c, 0) + 0x88, fbits(farg(c, 0)));
    clear_regs(farg(c, 0), 0, 1);
}
static void GX2SetClearStencil(CPUState* c) {
    if (arg(c, 0)) wiiu_write32(arg(c, 0) + 0x8C, arg(c, 1) & 0xFF);
    clear_regs(0, arg(c, 1), 2);
}

// ==== Fixed-function state ============================================================
// GX2 "Reg" structs hold ready register values; the plain Set calls build
// them first.

static u32 alpha_test_ctl(u32 enable, u32 func) { return (func & 7) | ((enable ? 1u : 0u) << 3); }
static void GX2InitAlphaTestReg(CPUState* c) {   // (reg, enable, func, ref)
    wiiu_write32(arg(c, 0), alpha_test_ctl(arg(c, 1), arg(c, 2)));
    wiiu_write32(arg(c, 0) + 4, fbits(farg(c, 0)));
}
static void emit_alpha_test(u32 ctl, u32 ref) {
    gx2_set_context_reg(SX_ALPHA_TEST_CONTROL, ctl);
    gx2_set_context_reg(SX_ALPHA_REF, ref);
}
static void GX2SetAlphaTestReg(CPUState* c) { emit_alpha_test(wiiu_read32(arg(c, 0)), wiiu_read32(arg(c, 0) + 4)); }
static void GX2SetAlphaTest(CPUState* c) { emit_alpha_test(alpha_test_ctl(arg(c, 0), arg(c, 1)), fbits(farg(c, 0))); }

static u32 color_ctl(u32 logic_op, u32 blend_mask, u32 multiwrite, u32 enable) {
    return ((multiwrite ? 1u : 0u) << 1) | ((enable ? 0u : 1u) << 4) | ((blend_mask & 0xFF) << 8) | ((logic_op & 0xFF) << 16);
}
static void GX2InitColorControlReg(CPUState* c) { wiiu_write32(arg(c, 0), color_ctl(arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4))); }
static void GX2SetColorControlReg(CPUState* c) { gx2_set_context_reg(CB_COLOR_CONTROL, wiiu_read32(arg(c, 0))); }
static void GX2SetColorControl(CPUState* c) { gx2_set_context_reg(CB_COLOR_CONTROL, color_ctl(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3))); }

// (frontFace, cullFront, cullBack, polyMode, frontMode, backMode, offFront, offBack, offPara)
static u32 polygon_ctl(CPUState* c, int a) {
    return ((arg(c, a + 1) & 1) << 0) | ((arg(c, a + 2) & 1) << 1) | ((arg(c, a + 0) & 1) << 2) |
           ((arg(c, a + 3) & 3) << 3) | ((arg(c, a + 4) & 7) << 5) | ((arg(c, a + 5) & 7) << 8) |
           ((arg(c, a + 6) & 1) << 11) | ((arg(c, a + 7) & 1) << 12);
}
static u32 polygon_ctl_para(CPUState* c, int a) {
    // the 9th argument (para offset) is on the stack when a == 1
    u32 para = a == 0 ? arg(c, 8) : wiiu_read32(c->gpr[1] + 8);
    return polygon_ctl(c, a) | ((para & 1) << 13);
}
static void GX2InitPolygonControlReg(CPUState* c) { wiiu_write32(arg(c, 0), polygon_ctl_para(c, 1)); }
static void GX2SetPolygonControlReg(CPUState* c) { gx2_set_context_reg(PA_SU_SC_MODE_CNTL, wiiu_read32(arg(c, 0))); }
static void GX2SetPolygonControl(CPUState* c) { gx2_set_context_reg(PA_SU_SC_MODE_CNTL, polygon_ctl_para(c, 0)); }
static void GX2SetCullOnlyControl(CPUState* c) {   // (frontFace, cullFront, cullBack)
    gx2_set_context_reg(PA_SU_SC_MODE_CNTL, ((arg(c, 1) & 1) << 0) | ((arg(c, 2) & 1) << 1) | ((arg(c, 0) & 1) << 2));
}

static void GX2InitPolygonOffsetReg(CPUState* c) {   // (reg, frontOffset, frontScale, backOffset, backScale, clamp)
    u32 r = arg(c, 0);
    wiiu_write32(r + 0, fbits(farg(c, 1) * 16.0f));
    wiiu_write32(r + 4, fbits(farg(c, 0)));
    wiiu_write32(r + 8, fbits(farg(c, 3) * 16.0f));
    wiiu_write32(r + 12, fbits(farg(c, 2)));
    wiiu_write32(r + 16, fbits(farg(c, 4)));
}
static void emit_poly_offset(const u32 v[5]) {
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, PA_SU_POLY_OFFSET_FRONT_SCALE, v, 4);
    gx2_set_context_reg(PA_SU_POLY_OFFSET_CLAMP, v[4]);
}
static void GX2SetPolygonOffsetReg(CPUState* c) {
    u32 r = arg(c, 0), v[5];
    for (int i = 0; i < 5; i++) v[i] = wiiu_read32(r + i * 4);
    emit_poly_offset(v);
}
static void GX2SetPolygonOffset(CPUState* c) {
    u32 v[5] = { fbits(farg(c, 1) * 16.0f), fbits(farg(c, 0)), fbits(farg(c, 3) * 16.0f), fbits(farg(c, 2)), fbits(farg(c, 4)) };
    emit_poly_offset(v);
}

static void clip_control(bool raster, bool zclip, bool halfz) {
    u32 v = (zclip ? 0u : (3u << 26)) | (raster ? 0u : (1u << 22)) | (halfz ? (1u << 19) : 0u) | (1u << 24);
    gx2_set_context_reg(PA_CL_CLIP_CNTL, v);
}
static void GX2SetRasterizerClipControl(CPUState* c) { clip_control(arg(c, 0) & 1, arg(c, 1) & 1, false); }
static void GX2SetRasterizerClipControlEx(CPUState* c) { clip_control(arg(c, 0) & 1, arg(c, 1) & 1, arg(c, 2) & 1); }

static void viewport_values(float x, float y, float w, float h, float n, float f, u32 out[6]) {
    out[0] = fbits(w * 0.5f);
    out[1] = fbits(x + w * 0.5f);
    out[2] = fbits(h * -0.5f);
    out[3] = fbits(y + h * 0.5f);
    out[4] = fbits((f - n) * 0.5f);
    out[5] = fbits((n + f) * 0.5f);
}
static void GX2InitViewportReg(CPUState* c) {   // (reg, x, y, w, h, near, far)
    u32 v[6];
    viewport_values(farg(c, 0), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), v);
    for (int i = 0; i < 6; i++) wiiu_write32(arg(c, 0) + i * 4, v[i]);
    // (the console's struct also holds clip/guard band registers after these)
}
static void GX2SetViewportReg(CPUState* c) {
    u32 v[6];
    for (int i = 0; i < 6; i++) v[i] = wiiu_read32(arg(c, 0) + i * 4);
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, PA_CL_VPORT_XSCALE, v, 6);
}
static void GX2SetViewport(CPUState* c) {
    u32 v[6];
    viewport_values(farg(c, 0), farg(c, 1), farg(c, 2), farg(c, 3), farg(c, 4), farg(c, 5), v);
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, PA_CL_VPORT_XSCALE, v, 6);
}

static void scissor_values(u32 x, u32 y, u32 w, u32 h, u32 out[2]) {
    u32 tlx = x < 8192 ? x : 8192, tly = y < 8192 ? y : 8192;
    u32 brx = x + w < 8192 ? x + w : 8192, bry = y + h < 8192 ? y + h : 8192;
    out[0] = tlx | (tly << 16) | (1u << 31);
    out[1] = brx | (bry << 16);
}
static void GX2InitScissorReg(CPUState* c) {   // (reg, x, y, w, h)
    u32 v[2];
    scissor_values(arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), v);
    wiiu_write32(arg(c, 0), v[0]);
    wiiu_write32(arg(c, 0) + 4, v[1]);
}
static void GX2SetScissorReg(CPUState* c) {
    u32 v[2] = { wiiu_read32(arg(c, 0)), wiiu_read32(arg(c, 0) + 4) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, PA_SC_GENERIC_SCISSOR_TL, v, 2);
}
static void GX2SetScissor(CPUState* c) {
    u32 v[2];
    scissor_values(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), v);
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, PA_SC_GENERIC_SCISSOR_TL, v, 2);
}
static void GX2GetScissorReg(CPUState* c) {   // (reg, *x, *y, *w, *h)
    u32 tl = wiiu_read32(arg(c, 0)), br = wiiu_read32(arg(c, 0) + 4);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), tl & 0x7FFF);
    if (arg(c, 2)) wiiu_write32(arg(c, 2), (tl >> 16) & 0x7FFF);
    if (arg(c, 3)) wiiu_write32(arg(c, 3), (br & 0x7FFF) - (tl & 0x7FFF));
    if (arg(c, 4)) wiiu_write32(arg(c, 4), ((br >> 16) & 0x7FFF) - ((tl >> 16) & 0x7FFF));
}

// Depth/stencil control. Argument order: depthTest, depthWrite, depthFunc,
// stencilTest, backStencilTest, frontFunc, frontZPass, frontZFail,
// frontFail, backFunc, backZPass, backZFail, backFail (the last few on the
// stack, which starts at r1+8).
static u32 stack_arg(CPUState* c, int i) { return i < 8 ? arg(c, i) : wiiu_read32(c->gpr[1] + 8 + (u32)(i - 8) * 4); }
static u32 depth_ctl(CPUState* c, int a) {
    u32 v = 0;
    v |= (stack_arg(c, a + 3) & 1) << 0;
    v |= (stack_arg(c, a + 0) & 1) << 1;
    v |= (stack_arg(c, a + 1) & 1) << 2;
    v |= (stack_arg(c, a + 2) & 7) << 4;
    v |= (stack_arg(c, a + 4) & 1) << 7;
    v |= (stack_arg(c, a + 5) & 7) << 8;
    v |= (stack_arg(c, a + 8) & 7) << 11;
    v |= (stack_arg(c, a + 6) & 7) << 14;
    v |= (stack_arg(c, a + 7) & 7) << 17;
    v |= (stack_arg(c, a + 9) & 7) << 20;
    v |= (stack_arg(c, a + 12) & 7) << 23;
    v |= (stack_arg(c, a + 10) & 7) << 26;
    v |= (stack_arg(c, a + 11) & 7) << 29;
    return v;
}
static void GX2SetDepthStencilControl(CPUState* c) { gx2_set_context_reg(DB_DEPTH_CONTROL, depth_ctl(c, 0)); }
static void GX2InitDepthStencilControlReg(CPUState* c) { wiiu_write32(arg(c, 0), depth_ctl(c, 1)); }
static void GX2SetDepthStencilControlReg(CPUState* c) { gx2_set_context_reg(DB_DEPTH_CONTROL, wiiu_read32(arg(c, 0))); }
static void GX2SetDepthOnlyControl(CPUState* c) {   // (test, write, func)
    gx2_set_context_reg(DB_DEPTH_CONTROL, ((arg(c, 0) & 1) << 1) | ((arg(c, 1) & 1) << 2) | ((arg(c, 2) & 7) << 4));
}

// (compareFront, writeFront, refFront, compareBack, writeBack, refBack)
static void stencil_values(CPUState* c, int a, u32 out[2]) {
    out[0] = (arg(c, a + 2) & 0xFF) | ((arg(c, a + 0) & 0xFF) << 8) | ((arg(c, a + 1) & 0xFF) << 16);
    out[1] = (arg(c, a + 5) & 0xFF) | ((arg(c, a + 3) & 0xFF) << 8) | ((arg(c, a + 4) & 0xFF) << 16);
}
static void emit_stencil(const u32 v[2]) {
    gx2_set_context_reg(DB_STENCILREFMASK, v[0]);
    gx2_set_context_reg(DB_STENCILREFMASK_BF, v[1]);
}
static void GX2SetStencilMask(CPUState* c) { u32 v[2]; stencil_values(c, 0, v); emit_stencil(v); }
static void GX2InitStencilMaskReg(CPUState* c) {
    u32 v[2];
    stencil_values(c, 1, v);
    wiiu_write32(arg(c, 0), v[0]);
    wiiu_write32(arg(c, 0) + 4, v[1]);
}
static void GX2SetStencilMaskReg(CPUState* c) {
    u32 v[2] = { wiiu_read32(arg(c, 0)), wiiu_read32(arg(c, 0) + 4) };
    emit_stencil(v);
}
static void GX2SetPrimitiveRestartIndex(CPUState* c) { gx2_set_context_reg(VGT_MULTI_PRIM_IB_RESET_INDX, arg(c, 0)); }

static u32 channel_masks(CPUState* c, int a) {
    u32 m = 0;
    for (int i = 0; i < 8; i++) m |= (stack_arg(c, a + i) & 0xF) << (i * 4);
    return m;
}
static void GX2SetTargetChannelMasks(CPUState* c) { gx2_set_context_reg(CB_TARGET_MASK, channel_masks(c, 0)); }
static void GX2InitTargetChannelMasksReg(CPUState* c) { wiiu_write32(arg(c, 0), channel_masks(c, 1)); }
static void GX2SetTargetChannelMasksReg(CPUState* c) { gx2_set_context_reg(CB_TARGET_MASK, wiiu_read32(arg(c, 0))); }

// (target, colorSrc, colorDst, colorFunc, separateAlpha, alphaSrc, alphaDst, alphaFunc)
static u32 blend_ctl(CPUState* c, int a) {
    return (stack_arg(c, a + 1) & 0x1F) | ((stack_arg(c, a + 3) & 7) << 5) | ((stack_arg(c, a + 2) & 0x1F) << 8) |
           ((stack_arg(c, a + 5) & 0x1F) << 16) | ((stack_arg(c, a + 7) & 7) << 21) | ((stack_arg(c, a + 6) & 0x1F) << 24) |
           ((stack_arg(c, a + 4) & 1) << 29);
}
static void GX2SetBlendControl(CPUState* c) { gx2_set_context_reg(CB_BLEND0_CONTROL + (arg(c, 0) & 7), blend_ctl(c, 0)); }
static void GX2InitBlendControlReg(CPUState* c) {
    wiiu_write32(arg(c, 0), arg(c, 1));
    wiiu_write32(arg(c, 0) + 4, blend_ctl(c, 1));
}
static void GX2SetBlendControlReg(CPUState* c) {
    gx2_set_context_reg(CB_BLEND0_CONTROL + (wiiu_read32(arg(c, 0)) & 7), wiiu_read32(arg(c, 0) + 4));
}
static void GX2SetBlendConstantColor(CPUState* c) {
    u32 v[4] = { fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, CB_BLEND_RED, v, 4);
}
static void GX2InitBlendConstantColorReg(CPUState* c) {
    for (int i = 0; i < 4; i++) wiiu_write32(arg(c, 0) + i * 4, fbits(farg(c, i)));
}
static void GX2SetBlendConstantColorReg(CPUState* c) {
    u32 v[4];
    for (int i = 0; i < 4; i++) v[i] = wiiu_read32(arg(c, 0) + i * 4);
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, CB_BLEND_RED, v, 4);
}
static u32 point_pair(float a, float b) {
    u32 ia = (u32)(a * 8.0f), ib = (u32)(b * 8.0f);
    if (ia > 0xFFFF) ia = 0xFFFF;
    if (ib > 0xFFFF) ib = 0xFFFF;
    return ia | (ib << 16);
}
static void GX2SetPointSize(CPUState* c) { gx2_set_context_reg(PA_SU_POINT_SIZE, point_pair(farg(c, 1), farg(c, 0))); }
static void GX2SetPointLimits(CPUState* c) { gx2_set_context_reg(PA_SU_POINT_MINMAX, point_pair(farg(c, 0), farg(c, 1))); }
static void GX2InitPointSizeReg(CPUState* c) { wiiu_write32(arg(c, 0), point_pair(farg(c, 1), farg(c, 0))); }
static void GX2SetPointSizeReg(CPUState* c) { gx2_set_context_reg(PA_SU_POINT_SIZE, wiiu_read32(arg(c, 0))); }
static void GX2InitPointLimitsReg(CPUState* c) { wiiu_write32(arg(c, 0), point_pair(farg(c, 0), farg(c, 1))); }
static void GX2SetPointLimitsReg(CPUState* c) { gx2_set_context_reg(PA_SU_POINT_MINMAX, wiiu_read32(arg(c, 0))); }

static void GX2SetSpecialState(CPUState* c) {   // (id, enable)
    if (arg(c, 0) != 0) return;
    if (arg(c, 1)) {
        gx2_set_context_reg(PA_CL_VTE_CNTL, 0x300);
        gx2_set_context_reg(PA_CL_CLIP_CNTL, 0x490000);
    } else {
        gx2_set_context_reg(PA_CL_VTE_CNTL, 0x43F);
        clip_control(true, true, false);
    }
}

// ==== Shaders ==========================================================================

// Fetch shaders: a small GPU program GX2 builds from the attribute list,
// which the vertex shader calls to load its inputs. We build the same
// program the console does; latte_shader.c reads it back.
static u32 fetch_cf_size(u32 attribs) { return align_up(((attribs + 15) / 16 + 1) * 8, 16); }
static void GX2CalcFetchShaderSizeEx(CPUState* c) { ret(c, fetch_cf_size(arg(c, 0)) + arg(c, 0) * 16); }

static u32 attrib_default_swap(u32 fmt) {
    switch (fmt & 0x3F) {
    case 0: case 1: case 4: case 10: return 0;
    case 2: case 3: case 7: case 8: case 14: case 15: return 1;
    default: return 2;
    }
}

// (fetchShader, program, attribCount, attribs, type, tessMode)
// GX2AttribStream (0x20): location 0, buffer 4, offset 8, format C,
// indexType 10, aluDivisor 14, destSel 18, endianSwap 1C.
static void GX2InitFetchShaderEx(CPUState* c) {
    static const u8 raw_to_fetch[20] = { 1, 2, 5, 6, 7, 0xD, 0xE, 0xF, 0x10, 0x16, 0x1A, 0x19,
                                         0x1D, 0x1E, 0x1F, 0x20, 0x2F, 0x30, 0x22, 0x23 };
    u32 fs = arg(c, 0), prog = arg(c, 1), count = arg(c, 2), attribs = arg(c, 3);
    u8* zero_fs = wiiu_mem_ptr(fs, 0x20);
    if (zero_fs) memset(zero_fs, 0, 0x20);
    wiiu_write32(fs + 0x10, count);
    wiiu_write32(fs + 0x0C, prog);
    u32 cf = fetch_cf_size(count);
    // Control flow: VTX_TC clauses of up to 16 fetches, then RETURN.
    u32 p = prog, a = 0;
    for (; a < count; a += 16) {
        u32 n = count - a < 16 ? count - a : 16;
        u32 word0 = (cf + a * 16) >> 3;
        u32 word1 = (0x03u << 23) | (((n - 1) & 7) << 10) | ((((n - 1) >> 3) & 1) << 19);
        wiiu_write32_le(p, word0);
        wiiu_write32_le(p + 4, word1);
        p += 8;
    }
    wiiu_write32_le(p, 0);
    wiiu_write32_le(p + 4, (0x14u << 23) | (1u << 31));   // RETURN, barrier
    p += 8;
    while (p < prog + cf) { wiiu_write32_le(p, 0); p += 4; }
    // One semantic fetch per attribute.
    u32 divisors[2] = { 0, 0 }, ndiv = 0;
    for (u32 i = 0; i < count; i++) {
        u32 at = attribs + i * 0x20;
        u32 loc = wiiu_read32(at), buf = wiiu_read32(at + 4), off = wiiu_read32(at + 8), fmt = wiiu_read32(at + 0xC);
        u32 index_type = wiiu_read32(at + 0x10), divisor = wiiu_read32(at + 0x14);
        u32 dest_sel = wiiu_read32(at + 0x18), swap = wiiu_read32(at + 0x1C);
        if (swap == 3) swap = attrib_default_swap(fmt);
        u32 src_sel_x = 0;
        if (index_type == 1) {
            if (divisor == 1) src_sel_x = 3;
            else {
                u32 k = 0;
                while (k < ndiv && divisors[k] != divisor) k++;
                if (k == ndiv && ndiv < 2) divisors[ndiv++] = divisor;
                src_sel_x = k == 0 ? 1 : 2;
            }
        }
        u32 fetch_fmt = raw_to_fetch[(fmt & 0x3F) < 20 ? (fmt & 0x3F) : 0] & 0x3F;
        u32 nfa = (fmt & 0x800) ? 2 : (fmt & 0x100) ? 1 : 0;
        u32 w0 = 1 | ((index_type & 3) << 5) | (((buf + 0xA0) & 0xFF) << 8) | (src_sel_x << 24);
        u32 w1 = (loc & 0xFF) | (((dest_sel >> 24) & 7) << 9) | (((dest_sel >> 16) & 7) << 12) |
                 (((dest_sel >> 8) & 7) << 15) | ((dest_sel & 7) << 18) | (fetch_fmt << 22) | (nfa << 28) |
                 ((fmt & 0x200) ? (1u << 30) : 0);
        u32 w2 = (off & 0xFFFF) | ((swap & 3) << 16);
        u32 dst = prog + cf + i * 16;
        wiiu_write32_le(dst, w0);
        wiiu_write32_le(dst + 4, w1);
        wiiu_write32_le(dst + 8, w2);
        wiiu_write32_le(dst + 12, 0);
    }
    wiiu_write32(fs + 0x14, ndiv);
    wiiu_write32(fs + 0x18, divisors[0]);
    wiiu_write32(fs + 0x1C, divisors[1]);
    wiiu_write32(fs + 0x08, cf + count * 16);
    wiiu_write32(fs + 0x04, 2);   // SQ_PGM_RESOURCES_FS: 2 GPRs
}

static void GX2SetFetchShader(CPUState* c) {
    u32 fs = arg(c, 0);
    u32 v[5] = { wiiu_read32(fs + 0x0C) >> 8, wiiu_read32(fs + 0x08) >> 3, 0x10000, 0x10000, wiiu_read32(fs + 0x04) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_FS, v, 5);
    u32 steps[2] = { wiiu_read32(fs + 0x18), wiiu_read32(fs + 0x1C) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, VGT_INSTANCE_STEP_RATE_0, steps, 2);
}

// GX2VertexShader: regs 0..CC (SQ_PGM_RESOURCES_VS 0, VGT_PRIMITIVEID_EN 4,
// SPI_VS_OUT_CONFIG 8, outIdCount C, SPI_VS_OUT_ID[10] 10, PA_CL_VS_OUT_CNTL
// 38, semanticCount 40, SQ_VTX_SEMANTIC[32] 44), size D0, program D4, mode
// D8, ..., streamOut 110, strides 114, GX2RBuffer 124 (its address at +0x0C).
static u32 program_addr(u32 ptr_field, u32 rbuffer) { return ptr_field ? ptr_field : wiiu_read32(rbuffer + 0x0C); }
// GX2RBuffer: flags 0, element size 4, element count 8, address C.
static u32 rbuffer_size(u32 rbuffer) { return wiiu_read32(rbuffer + 0x4) * wiiu_read32(rbuffer + 0x8); }

// A shader with no program: report who set it (the menus/UI are drawn with
// such shaders right now, so find out where they come from).
static void report_empty_shader(CPUState* c, const char* kind, u32 sh) {
    static int reported = 0;
    if (reported >= 6) return;
    reported++;
    printf("[gx2] %s shader at %08X has no program. First words:", kind, sh);
    for (u32 i = 0; i < 16; i++) printf(" %08X", wiiu_read32(sh + i * 4));
    printf("\n  called from:\n");
    runtime_print_chain(c, stdout, 12, false);
}

static void GX2SetVertexShader(CPUState* c) {
    u32 vs = arg(c, 0);
    u32 prog = program_addr(wiiu_read32(vs + 0xD4), vs + 0x124);
    u32 size = wiiu_read32(vs + 0xD4) ? wiiu_read32(vs + 0xD0) : rbuffer_size(vs + 0x124);
    u32 mode = wiiu_read32(vs + 0xD8);
    if (!prog) report_empty_shader(c, "vertex", vs);
    u32 v[5] = { prog >> 8, size >> 3, 0x100000, 0x100000, wiiu_read32(vs + 0x00) };
    if (mode == 2) {   // geometry shader mode: the vertex shader runs as the "export shader"
        gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_ES, v, 5);
    } else {
        gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_VS, v, 5);
        gx2_set_context_reg(VGT_PRIMITIVEID_EN, wiiu_read32(vs + 0x04));
        gx2_set_context_reg(SPI_VS_OUT_CONFIG, wiiu_read32(vs + 0x08));
        gx2_set_context_reg(PA_CL_VS_OUT_CNTL, wiiu_read32(vs + 0x38));
        u32 n = wiiu_read32(vs + 0x0C);
        if (n > 10) n = 10;
        // all 10 (unused ones cleared: left-over values from the previous
        // shader would make needless extra shader translations)
        gx2_emit(PM4_TYPE3(IT_SET_CONTEXT_REG, 1 + 10));
        gx2_emit(SPI_VS_OUT_ID_0 - REG_BASE_CONTEXT);
        for (u32 i = 0; i < 10; i++) gx2_emit(i < n ? wiiu_read32(vs + 0x10 + i * 4) : 0xFFFFFFFFu);
        if (wiiu_read32(vs + 0x110)) {
            for (u32 i = 0; i < 4; i++)
                gx2_set_context_reg(VGT_STRMOUT_BUFFER_SIZE_0 + 1 + i * 4, wiiu_read32(vs + 0x114 + i * 4) >> 2);
        }
    }
    u32 sem = wiiu_read32(vs + 0x40);
    {   // (SQ_VTX_SEMANTIC_CLEAR: the ones this shader doesn't set are unused)
        gx2_set_context_reg(SQ_VTX_SEMANTIC_CLEAR, 0xFFFFFFFF);
        if (sem > 32) sem = 32;
        gx2_emit(PM4_TYPE3(IT_SET_CONTEXT_REG, 1 + 32));
        gx2_emit(SQ_VTX_SEMANTIC_0 - REG_BASE_CONTEXT);
        for (u32 i = 0; i < 32; i++) gx2_emit(i < sem ? wiiu_read32(vs + 0x44 + i * 4) : 0xFFu);
    }
}

// GX2PixelShader: regs[41] (resources 0, ?, SPI_PS_IN_CONTROL_0 8,
// SPI_PS_IN_CONTROL_1 C, numInputs 10, SPI_PS_INPUT_CNTL[32] 14,
// CB_SHADER_MASK 94, CB_SHADER_CONTROL 98, DB_SHADER_CONTROL 9C,
// SPI_INPUT_Z A0), size A4, program A8, mode AC, ..., GX2RBuffer D8.
static void GX2SetPixelShader(CPUState* c) {
    u32 ps = arg(c, 0);
    u32 prog = program_addr(wiiu_read32(ps + 0xA8), ps + 0xD8);
    u32 size = wiiu_read32(ps + 0xA8) ? wiiu_read32(ps + 0xA4) : rbuffer_size(ps + 0xD8);
    if (!prog) report_empty_shader(c, "pixel", ps);
    u32 v[5] = { prog >> 8, size >> 3, 0x100000, 0x100000, wiiu_read32(ps + 0x00) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_PS, v, 5);
    u32 ctl[2] = { wiiu_read32(ps + 0x08), wiiu_read32(ps + 0x0C) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SPI_PS_IN_CONTROL_0, ctl, 2);
    u32 n = wiiu_read32(ps + 0x10);
    if (n > 32) n = 32;
    gx2_emit(PM4_TYPE3(IT_SET_CONTEXT_REG, 1 + n));
    gx2_emit(SPI_PS_INPUT_CNTL_0 - REG_BASE_CONTEXT);
    for (u32 i = 0; i < n; i++) gx2_emit(wiiu_read32(ps + 0x14 + i * 4));
    gx2_set_context_reg(CB_SHADER_MASK, wiiu_read32(ps + 0x94));
    gx2_set_context_reg(CB_SHADER_CONTROL, wiiu_read32(ps + 0x98));
    gx2_set_context_reg(DB_SHADER_CONTROL, wiiu_read32(ps + 0x9C));
    gx2_set_context_reg(SPI_INPUT_Z, wiiu_read32(ps + 0xA0));
}

// GX2GeometryShader: regs[19] (resources GS 0, out prim type 4, GS mode 8,
// PA_CL_VS_OUT_CNTL C, copy shader resources 10, GS vert itemsize 14,
// SPI_VS_OUT_CONFIG 18, outIdCount 1C, SPI_VS_OUT_ID[10] 20,
// VGT_STRMOUT_BUFFER_EN 48), size 4C, program 50, copySize 54, copyProgram
// 58, mode 5C, ..., ringItemsize 88, streamout 8C, strides 90, GX2RBuffers
// A0 / B0.
static void GX2SetGeometryShader(CPUState* c) {
    u32 gs = arg(c, 0);
    u32 prog = program_addr(wiiu_read32(gs + 0x50), gs + 0xA0);
    u32 size = wiiu_read32(gs + 0x50) ? wiiu_read32(gs + 0x4C) : rbuffer_size(gs + 0xA0);
    u32 copy = program_addr(wiiu_read32(gs + 0x58), gs + 0xB0);
    u32 copy_size = wiiu_read32(gs + 0x58) ? wiiu_read32(gs + 0x54) : rbuffer_size(gs + 0xB0);
    u32 v[5] = { prog >> 8, size >> 3, 0x100000, 0x100000, wiiu_read32(gs + 0x00) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_GS, v, 5);
    gx2_set_context_reg(VGT_GS_OUT_PRIM_TYPE, wiiu_read32(gs + 0x04));
    gx2_set_context_reg(VGT_GS_MODE, wiiu_read32(gs + 0x08));
    gx2_set_context_reg(SQ_PGM_RESOURCES_GS, wiiu_read32(gs + 0x00));
    gx2_set_context_reg(SQ_GS_VERT_ITEMSIZE, wiiu_read32(gs + 0x14));
    gx2_set_context_reg(VGT_STRMOUT_BUFFER_EN, wiiu_read32(gs + 0x48));
    u32 cv[5] = { copy >> 8, copy_size >> 3, 0x100000, 0x100000, wiiu_read32(gs + 0x10) };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, SQ_PGM_START_VS, cv, 5);
    gx2_set_context_reg(PA_CL_VS_OUT_CNTL, wiiu_read32(gs + 0x0C));
    u32 n = wiiu_read32(gs + 0x1C);
    if (n > 10) n = 10;
    if (n) {
        gx2_emit(PM4_TYPE3(IT_SET_CONTEXT_REG, 1 + n));
        gx2_emit(SPI_VS_OUT_ID_0 - REG_BASE_CONTEXT);
        for (u32 i = 0; i < n; i++) gx2_emit(wiiu_read32(gs + 0x20 + i * 4));
    }
    gx2_set_context_reg(SPI_VS_OUT_CONFIG, wiiu_read32(gs + 0x18));
    gx2_set_context_reg(SQ_GSVS_RING_ITEMSIZE, wiiu_read32(gs + 0x88) & 0x7FFF);
}

static void GX2GetVertexShaderGPRs(CPUState* c) { ret(c, wiiu_read32(arg(c, 0)) & 0xFF); }
static void GX2GetVertexShaderStackEntries(CPUState* c) { ret(c, (wiiu_read32(arg(c, 0)) >> 8) & 0xFF); }
static void GX2GetPixelShaderGPRs(CPUState* c) { ret(c, wiiu_read32(arg(c, 0)) & 0xFF); }
static void GX2GetPixelShaderStackEntries(CPUState* c) { ret(c, (wiiu_read32(arg(c, 0)) >> 8) & 0xFF); }
static void GX2GetGeometryShaderGPRs(CPUState* c) { ret(c, wiiu_read32(arg(c, 0)) & 0xFF); }
static void GX2GetGeometryShaderStackEntries(CPUState* c) { ret(c, (wiiu_read32(arg(c, 0)) >> 8) & 0xFF); }

// (mode, vsGPRs, vsStack, gsGPRs, gsStack, psGPRs, psStack)
static void GX2SetShaderModeEx(CPUState* c) {
    u32 mode = arg(c, 0);
    if (mode > 3) return;
    bool geometry = mode == 2, compute = mode == 3;
    if (!geometry) gx2_set_context_reg(VGT_GS_MODE, compute ? (3u | (3u << 14) | (1u << 17)) : 0);
    u32 sq_config = ((mode == 0) ? 1u << 2 : 0) | (1u << 3) |
                    ((compute ? 0u : 3u) << 24) | ((compute ? 1u : 2u) << 26) | ((compute ? 2u : 1u) << 28) | ((compute ? 3u : 0u) << 30);
    u32 gpr1 = 0, gpr2 = 0, stack1 = 0, stack2 = 0, thread = 0x04043088;
    if (compute) {
        thread = 0xBD010101;
        gpr2 = 0xF8u << 8;
        stack2 = 0x100u << 16;
    } else {
        gpr1 = arg(c, 5) & 0xFF;
        stack1 = arg(c, 6) & 0xFFF;
        if (geometry) {
            gpr1 |= 0x40u << 8;
            gpr2 = (arg(c, 3) & 0xFF) | ((arg(c, 1) & 0xFF) << 8);
            stack2 = (arg(c, 4) & 0xFFF) | ((arg(c, 2) & 0xFFF) << 16);
            thread = 0x1C08207C;
        } else {
            gpr1 |= (arg(c, 1) & 0xFF) << 8;
            stack1 |= (arg(c, 2) & 0xFFF) << 16;
        }
    }
    u32 v[6] = { sq_config, gpr1 | 0x40000000, gpr2, thread, stack1, stack2 };
    gx2_set_regs(IT_SET_CONFIG_REG, REG_BASE_CONFIG, SQ_CONFIG, v, 6);
}

// Uniform registers: (offset in words, count, values). Vertex constants
// start at ALU constant 0x400, pixel ones at 0.
static void uniform_regs(CPUState* c, u32 base) {
    u32 offset = arg(c, 0), count = arg(c, 1) & ~3u, values = arg(c, 2);
    if (offset & 0x8000) return;
    if (!count) return;
    u32 w[2 + 64];
    if (count <= 64) {   // (the usual few registers: one packet, written at once)
        w[0] = PM4_TYPE3(IT_SET_ALU_CONST, 1 + count);
        w[1] = base + offset;
        for (u32 i = 0; i < count; i++) w[2 + i] = grd(values + i * 4);
        gx2_emit_n(w, 2 + count);
        return;
    }
    gx2_emit(PM4_TYPE3(IT_SET_ALU_CONST, 1 + count));
    gx2_emit(base + offset);
    for (u32 i = 0; i < count; i++) gx2_emit(grd(values + i * 4));
}
static void GX2SetVertexUniformReg(CPUState* c) { uniform_regs(c, 0x400); }
static void GX2SetPixelUniformReg(CPUState* c) { uniform_regs(c, 0); }

// Uniform blocks: (index, size, data) -> a buffer resource.
static void uniform_block(u32 base, u32 index, u32 addr, u32 size) {
    u32 v[9] = { PM4_TYPE3(IT_SET_RESOURCE, 8), base + index * 7 - REG_BASE_RESOURCE, addr, size - 1, 0, 1, 0, 0, 0xC0000000 };
    gx2_emit_n(v, 9);
}
static void GX2SetVertexUniformBlock(CPUState* c) { uniform_block(SQ_VS_UNIFORM_BLOCK, arg(c, 0), arg(c, 2), arg(c, 1)); }
static void GX2SetPixelUniformBlock(CPUState* c) { uniform_block(SQ_PS_UNIFORM_BLOCK, arg(c, 0), arg(c, 2), arg(c, 1)); }
static void GX2SetGeometryUniformBlock(CPUState* c) { uniform_block(SQ_GS_UNIFORM_BLOCK, arg(c, 0), arg(c, 2), arg(c, 1)); }

static void GX2SetAttribBuffer(CPUState* c) {   // (index, size, stride, data)
    u32 v[9] = { PM4_TYPE3(IT_SET_RESOURCE, 8), SQ_VTX_ATTRIB_BUFFER + arg(c, 0) * 7 - REG_BASE_RESOURCE,
                 arg(c, 3), arg(c, 1) - 1, (arg(c, 2) & 0xFFFF) << 11, 0, 0, 0, 0xC0000000 };
    gx2_emit_n(v, 9);
}

// ==== Stream out (vertex shaders writing results to memory) ========================
// GX2StreamOutBuffer: size 0, dataPtr 4, vertexStride 8, GX2RBuffer C, context pointer 1C.
static void GX2SetStreamOutBuffer(CPUState* c) {   // (index, buffer)
    u32 i = arg(c, 0), b = arg(c, 1);
    if (i >= 4) return;
    u32 addr = wiiu_read32(b + 4), size = wiiu_read32(b);
    if (!addr) { addr = wiiu_read32(b + 0x0C + 0x0C); size = rbuffer_size(b + 0x0C); }
    gx2_set_context_reg(VGT_STRMOUT_BUFFER_SIZE_0 + i * 4, size >> 2);
    gx2_set_context_reg(VGT_STRMOUT_BUFFER_SIZE_0 + 2 + i * 4, addr >> 8);
}
static void GX2SetStreamOutEnable(CPUState* c) { gx2_set_context_reg(VGT_STRMOUT_EN, arg(c, 0) & 1); }
static void GX2SetStreamOutContext(CPUState* c) {   // (index, buffer, reset)
    u32 i = arg(c, 0), b = arg(c, 1), reset = arg(c, 2);
    if (i >= 4) return;
    gx2_emit(PM4_TYPE3(IT_STRMOUT_BUFFER_UPDATE, 5));
    gx2_emit(((reset ? 0u : 2u) << 1) | (i << 8));
    gx2_emit(0);
    gx2_emit(0);
    gx2_emit(reset ? 0 : wiiu_read32(b + 0x1C));
    gx2_emit(0);
}
static void GX2SaveStreamOutContext(CPUState* c) {   // (index, buffer)
    u32 i = arg(c, 0), b = arg(c, 1);
    if (i >= 4) return;
    gx2_emit(PM4_TYPE3(IT_STRMOUT_BUFFER_UPDATE, 5));
    gx2_emit(1 | (3u << 1) | (i << 8));
    gx2_emit(wiiu_read32(b + 0x1C));
    gx2_emit(0);
    gx2_emit(0);
    gx2_emit(0);
}

static void GX2CalcGeometryShaderRingBufferSize(CPUState* c) { ret(c, arg(c, 0) * 4 * 0x1000); }
static void GX2SetGeometryShaderInputRingBuffer(CPUState* c) {   // (buffer, size)
    u32 v[2] = { arg(c, 0) >> 8, arg(c, 1) >> 8 };
    gx2_set_regs(IT_SET_CONFIG_REG, REG_BASE_CONFIG, 0x2310, v, 2);
}
static void GX2SetGeometryShaderOutputRingBuffer(CPUState* c) {
    u32 v[2] = { arg(c, 0) >> 8, arg(c, 1) >> 8 };
    gx2_set_regs(IT_SET_CONFIG_REG, REG_BASE_CONFIG, 0x2312, v, 2);
}

// ==== Draws ================================================================================

static void draw_setup(u32 prim, u32 base_vertex, u32 index_type, u32 instances) {
    u32 w[3] = { PM4_TYPE3(IT_SET_CTL_CONST, 2), 0, base_vertex };
    gx2_emit_n(w, 3);
    gx2_set_config_reg(VGT_PRIMITIVE_TYPE, prim);
    u32 w2[4] = { PM4_TYPE3(IT_INDEX_TYPE, 1), index_type, PM4_TYPE3(IT_NUM_INSTANCES, 1), instances };
    gx2_emit_n(w2, 4);
}
// Hand the commands so far to the GPU thread every few draws. Only flushes
// did that before -- mostly once a frame -- so the GPU thread couldn't start
// a frame until the game had nearly finished it, and then the game waited
// for the GPU thread: the two took turns (each idle ~17-21 ms per frame in
// story mode) instead of working side by side. SPLAT_COMMIT_DRAWS=N sets
// how often (0: only at flushes, as before).
static void draw_commit(void) {
    static int every = -1;
    static u32 n;
    if (every < 0) { const char* e = getenv("SPLAT_COMMIT_DRAWS"); every = e ? atoi(e) : 8; if (every < 0) every = 0; }
    if (gx2_dl_active()) return;   // (a draw into a display list: nothing new in the main commands to hand over)
    if (every && ++n >= (u32)every) { n = 0; gx2_commit(); }
}
// (mode, count, indexType, indices, baseVertex, instances)
static void GX2DrawIndexedEx(CPUState* c) {
    draw_setup(arg(c, 0), arg(c, 4), arg(c, 2), arg(c, 5));
    u32 w[6] = { PM4_TYPE3(IT_DRAW_INDEX_2, 5) | 1, 0xFFFFFFFF, arg(c, 3), 0, arg(c, 1), 0 };
    gx2_emit_n(w, 6);
    draw_commit();
}
static void GX2DrawIndexedEx2(CPUState* c) {   // (... , baseInstance)
    gx2_emit(PM4_TYPE3(IT_SET_CTL_CONST, 2));
    gx2_emit(1);
    gx2_emit(arg(c, 6));
    GX2DrawIndexedEx(c);
    gx2_emit(PM4_TYPE3(IT_SET_CTL_CONST, 2));
    gx2_emit(1);
    gx2_emit(0);
}
static void GX2DrawEx(CPUState* c) {   // (mode, count, baseVertex, instances)
    draw_setup(arg(c, 0), arg(c, 2), LATTE_INDEX_U32_BE, arg(c, 3));
    gx2_emit(PM4_TYPE3(IT_DRAW_INDEX_AUTO, 2) | 1);
    gx2_emit(arg(c, 1));
    gx2_emit(0);
    draw_commit();
}
static void GX2DrawEx2(CPUState* c) {   // (mode, count, baseVertex, instances, baseInstance)
    gx2_emit(PM4_TYPE3(IT_SET_CTL_CONST, 2));
    gx2_emit(1);
    gx2_emit(arg(c, 4));
    GX2DrawEx(c);
    gx2_emit(PM4_TYPE3(IT_SET_CTL_CONST, 2));
    gx2_emit(1);
    gx2_emit(0);
}
static void GX2DrawIndexedImmediateEx(CPUState* c) {   // (mode, count, indexType, indices, baseVertex, instances)
    u32 type = arg(c, 2), count = arg(c, 1), data = arg(c, 3);
    bool wide = type == LATTE_INDEX_U32_BE || type == LATTE_INDEX_U32_LE;
    u32 words = wide ? count : (count + 1) / 2;
    if (words > 0x3FF0) return;
    draw_setup(arg(c, 0), arg(c, 4), type, arg(c, 5));
    gx2_emit(PM4_TYPE3(IT_DRAW_INDEX_IMMD, 2 + words) | 1);
    gx2_emit(count);
    gx2_emit(0);
    for (u32 i = 0; i < words; i++) gx2_emit(wiiu_read32(data + i * 4));
    draw_commit();
}

// ==== Display lists ===================================================================

static void GX2BeginDisplayList(CPUState* c) { gx2_dl_begin(arg(c, 0), arg(c, 1)); }
static void GX2EndDisplayList(CPUState* c) { ret(c, gx2_dl_end()); }
static void GX2GetCurrentDisplayList(CPUState* c) {   // (*list, *size) -> bool
    u32 addr = 0, size = 0;
    bool active = gx2_dl_current(&addr, &size);
    if (arg(c, 0)) wiiu_write32(arg(c, 0), addr);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), size);
    ret(c, active);
}
static void GX2GetDisplayListWriteStatus(CPUState* c) { ret(c, gx2_dl_active()); }
static void GX2CallDisplayList(CPUState* c) {   // (list, bytes)
    u32 w[4] = { PM4_TYPE3(IT_INDIRECT_BUFFER_PRIV, 3), arg(c, 0), 0, arg(c, 1) / 4 };
    gx2_emit_n(w, 4);
    // Handed to the GPU thread every few calls, so it works on the frame
    // while the game is still building it (story levels: the GPU thread sat
    // idle most of the frame, then the game waited ~8 ms in GX2DrawDone while
    // it did everything at the flush). The data those draws read that the
    // game may still be filling in (uniform blocks, vertex data) is copied
    // only at the game's flush (latte_render.c, upload_guest).
    // SPLAT_DL_COMMIT=0: only at flushes, as before.
    static int every = -1;
    static u32 n;
    if (every < 0) {
        const char* e = getenv("SPLAT_DL_COMMIT");
        every = e ? atoi(e) : 4;
        if (every < 0 || getenv("SPLAT_NO_DEFER")) every = 0;
    }
    if (every && !gx2_dl_active() && ++n >= (u32)every) { n = 0; gx2_commit(); }   // (only for calls in the main commands)
}
// (Like on the console, the graphics card only gets the commands at the next
// flush: the game may still fill in the uniforms those draws read until
// then -- handing them over right away made draws read half-written
// values: wrong colors, flicker.)
static void GX2DirectCallDisplayList(CPUState* c) { GX2CallDisplayList(c); }
static void GX2CopyDisplayList(CPUState* c) {   // (list, bytes): inline copy
    u32 a = arg(c, 0), n = arg(c, 1) / 4;
    for (u32 i = 0; i < n; i++) gx2_emit(wiiu_read32(a + i * 4));
}
// (list, type, offset, shader): replace the program address in a recorded
// "set shader program" packet (word 2 of the packet at `offset`).
static void GX2PatchDisplayList(CPUState* c) {
    u32 list = arg(c, 0), type = arg(c, 1), off = arg(c, 2), obj = arg(c, 3);
    u32 addr;
    switch (type) {
    case 1: addr = wiiu_read32(obj + 0x0C); break;                                 // fetch shader
    case 2: addr = program_addr(wiiu_read32(obj + 0xD4), obj + 0x124); break;      // vertex
    case 3: addr = program_addr(wiiu_read32(obj + 0x58), obj + 0xB0); break;       // GS copy
    case 4: addr = program_addr(wiiu_read32(obj + 0x50), obj + 0xA0); break;       // geometry
    case 5: addr = program_addr(wiiu_read32(obj + 0xA8), obj + 0xD8); break;       // pixel
    default: return;
    }
    wiiu_write32(list + off + 8, addr >> 8);
}

// ==== Context states ==================================================================
// A GX2ContextState is a block of guest memory the GPU mirrors registers
// into ("shadowing"); switching state loads them back. Area sizes and the
// register ranges each LOAD packet covers match the console.

typedef struct { u32 offset, count; } RegRange;
static const RegRange k_load_alu[] = { { 0, 0x800 } };
static const RegRange k_load_loop[] = { { 0, 0x60 } };
static const RegRange k_load_sampler[] = { { 0, 0x36 }, { 0x36, 0x36 }, { 0x6C, 0x36 } };
static const RegRange k_load_config[] = {
    { 0x300, 0x6 }, { 0x900, 0x48 }, { 0x980, 0x48 }, { 0xA00, 0x48 }, { 0x310, 0xC }, { 0x542, 0x1 },
    { 0x235, 0x1 }, { 0x232, 0x2 }, { 0x23A, 0x1 }, { 0x256, 0x1 }, { 0x60C, 0x1 }, { 0x5C5, 0x1 },
    { 0x2C8, 0x1 }, { 0x363, 0x1 }, { 0x404, 0x2 } };
static const RegRange k_load_context[] = {
    { 0x0, 0x2 }, { 0x3, 0x3 }, { 0xA, 0x4 }, { 0x10, 0x38 }, { 0x50, 0x34 }, { 0x8E, 0x4 }, { 0x94, 0x40 },
    { 0x100, 0x9 }, { 0x10C, 0x3 }, { 0x10F, 0x60 }, { 0x185, 0xA }, { 0x191, 0x27 }, { 0x1E0, 0x9 },
    { 0x200, 0x1 }, { 0x202, 0x7 }, { 0xE0, 0x20 }, { 0x210, 0x29 }, { 0x250, 0x34 }, { 0x290, 0x1 },
    { 0x292, 0x2 }, { 0x2A1, 0x1 }, { 0x2A5, 0x1 }, { 0x2A8, 0x2 }, { 0x2AC, 0x3 }, { 0x2CA, 0x1 },
    { 0x2CC, 0x1 }, { 0x2CE, 0x1 }, { 0x300, 0x9 }, { 0x30C, 0x1 }, { 0x312, 0x1 }, { 0x316, 0x2 },
    { 0x343, 0x2 }, { 0x349, 0x3 }, { 0x34C, 0x2 }, { 0x351, 0x1 }, { 0x37E, 0x6 }, { 0x2B4, 0x3 },
    { 0x2B8, 0x3 }, { 0x2BC, 0x3 }, { 0x2C0, 0x3 }, { 0x2C8, 0x1 }, { 0x29B, 0x1 }, { 0x8C, 0x1 },
    { 0xD5, 0x1 }, { 0x284, 0xC } };
static const RegRange k_load_resource[] = {
    { 0, 0x70 }, { 0x380, 0x70 }, { 0x460, 0x70 }, { 0x7E0, 0x70 }, { 0x8B9, 0x7 }, { 0x8C0, 0x70 },
    { 0x930, 0x70 }, { 0xCB0, 0x70 }, { 0xD89, 0x7 } };

#define AREA_BYTES(regs) (((regs) * 4 + 0xFF) & ~0xFFu)
enum {
    CTX_CONFIG = 0,
    CTX_CONTEXT = CTX_CONFIG + AREA_BYTES(0xB00),
    CTX_ALU = CTX_CONTEXT + AREA_BYTES(0x400),
    CTX_LOOP = CTX_ALU + AREA_BYTES(0x800),
    CTX_RESOURCE = CTX_LOOP + AREA_BYTES(0x60),
    CTX_SAMPLER = CTX_RESOURCE + AREA_BYTES(0xD9E),
    CTX_DL_SIZE = 0x9804,
    CTX_DL = 0x9E00,
    CTX_TOTAL = 0xA100,
};

static void emit_load(u32 op, u32 area, const RegRange* r, u32 n, u32 wait) {
    gx2_emit(PM4_TYPE3(op, 2 + n * 2));
    gx2_emit(area);
    gx2_emit(wait);
    for (u32 i = 0; i < n; i++) { gx2_emit(r[i].offset); gx2_emit(r[i].count); }
}
#define COUNT(a) (u32)(sizeof(a) / sizeof(a[0]))
static void emit_restore_state(u32 ctx) {
    gx2_emit(PM4_TYPE3(IT_CONTEXT_CONTROL, 2));
    gx2_emit(0x80000077);
    gx2_emit(0x80000077);
    emit_load(IT_LOAD_CONFIG_REG, ctx + CTX_CONFIG, k_load_config, COUNT(k_load_config), 0x80000000);
    emit_load(IT_LOAD_CONTEXT_REG, ctx + CTX_CONTEXT, k_load_context, COUNT(k_load_context), 0);
    emit_load(IT_LOAD_ALU_CONST, ctx + CTX_ALU, k_load_alu, COUNT(k_load_alu), 0);
    emit_load(IT_LOAD_LOOP_CONST, ctx + CTX_LOOP, k_load_loop, COUNT(k_load_loop), 0);
    emit_load(IT_LOAD_RESOURCE, ctx + CTX_RESOURCE, k_load_resource, COUNT(k_load_resource), 0);
    emit_load(IT_LOAD_SAMPLER, ctx + CTX_SAMPLER, k_load_sampler, COUNT(k_load_sampler), 0);
}
static void shadowing_on(void) {
    gx2_emit(PM4_TYPE3(IT_CONTEXT_CONTROL, 2));
    gx2_emit(0x80000077);
    gx2_emit(0x80000077);
}

static void set_default_state(void);

static void GX2SetupContextStateEx(CPUState* c) {   // (state, profiling)
    u32 ctx = arg(c, 0);
    u8* p = wiiu_mem_ptr(ctx, CTX_TOTAL);
    if (p) memset(p, 0, CTX_TOTAL);
    emit_restore_state(ctx);
    shadowing_on();
    set_default_state();
    // Record the "load this state" commands as a display list inside the state.
    gx2_dl_begin(ctx + CTX_DL, 0x300);
    emit_restore_state(ctx);
    wiiu_write32(ctx + CTX_DL_SIZE, gx2_dl_end());
}
static void GX2SetContextState(CPUState* c) {
    u32 ctx = arg(c, 0);
    if (!ctx) {
        gx2_emit(PM4_TYPE3(IT_CONTEXT_CONTROL, 2));
        gx2_emit(0x80000000);
        gx2_emit(0x80000000);
        return;
    }
    u32 size = wiiu_read32(ctx + CTX_DL_SIZE);
    shadowing_on();
    if (!size) {
        emit_restore_state(ctx);
    } else {
        gx2_emit(PM4_TYPE3(IT_INDIRECT_BUFFER_PRIV, 3));
        gx2_emit(ctx + CTX_DL);
        gx2_emit(0);
        gx2_emit(size / 4);
    }
}
static void GX2GetContextStateDisplayList(CPUState* c) {   // (state, *list, *size)
    if (arg(c, 1)) wiiu_write32(arg(c, 1), arg(c, 0) + CTX_DL);
    if (arg(c, 2)) wiiu_write32(arg(c, 2), wiiu_read32(arg(c, 0) + CTX_DL_SIZE));
}

static void set_default_state(void) {
    // shader mode: uniform registers, default GPR split
    u32 v[6] = { (1u << 2) | (1u << 3) | (3u << 24) | (2u << 26) | (1u << 28), 200 | (48u << 8) | 0x40000000, 0,
                 0x04043088, 192 | (64u << 16), 0 };
    gx2_set_context_reg(VGT_GS_MODE, 0);
    gx2_set_regs(IT_SET_CONFIG_REG, REG_BASE_CONFIG, SQ_CONFIG, v, 6);
    gx2_set_context_reg(PA_CL_VTE_CNTL, 0x43F);
    gx2_set_context_reg(DB_DEPTH_CONTROL, (1u << 1) | (1u << 2) | (1u << 4));
    emit_alpha_test(alpha_test_ctl(0, 1), 0);
    gx2_set_context_reg(PA_SU_SC_MODE_CNTL, (2u << 5) | (2u << 8));
    u32 po[5] = { 0, 0, 0, 0, 0 };
    emit_poly_offset(po);
    gx2_set_context_reg(VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    gx2_set_context_reg(CB_TARGET_MASK, 0xFFFFFFFF);
    u32 bc[4] = { 0, 0, 0, 0 };
    gx2_set_regs(IT_SET_CONTEXT_REG, REG_BASE_CONTEXT, CB_BLEND_RED, bc, 4);
    gx2_set_context_reg(PA_SU_POINT_SIZE, point_pair(1, 1));
    gx2_set_context_reg(PA_SU_POINT_MINMAX, point_pair(1, 1));
    gx2_set_context_reg(CB_COLOR_CONTROL, color_ctl(0xCC, 0, 0, 1));
    clip_control(true, true, false);
    gx2_set_context_reg(DB_DEPTH_CLEAR, fbits(1.0f));
}
static void GX2SetDefaultState(CPUState* c) { (void)c; set_default_state(); }

// ==== Frames, sync, misc =========================================================================

static u32 g_swaps = 0, g_flips = 0, g_swap_interval = 1;
u32 gx2_frame_count(void) { return g_swaps; }
static u64 g_last_flip = 0, g_last_vsync = 0;
static int g_clock_vsync = -1;   // vsync at the clock's 1/60 s marks (default) or a refresh after the last frame (SPLAT_FRAME_VSYNC=1)
static u32 g_vsync_calls, g_flip_calls;   // (how the game paces itself: for the log)
static u64 g_vs_late_sum, g_vs_late_max, g_vs_waits;   // (how late the vsync waits woke up: for the log)
static u64 g_vb_hits, g_vb_misses;   // (low input lag: vsync waits ended by the screen / by the clock)
static u64 g_wake_qpc = 0, g_swap_qpc = 0;   // when the game last woke from its vsync wait / last swapped (QPC)
// (low input lag: the screen refresh each frame was paced for, by swap number -- the GPU thread
// compares its present with it, the [pace] line: frames handed over in time, or a refresh late)
static u64 g_ll_next_qpc;
static volatile u64 g_swap_target[16];
u64 gx2_swap_target_qpc(u32 swap) { return g_swap_target[swap & 15]; }
static s64 g_work_avg = 0;                   // the game's work per frame (wake to swap), smoothed, QPC ticks
static u64 g_ll_waits = 0, g_ll_lead_sum = 0, g_ll_work_sum = 0, g_ll_per = 0, g_ll_freq = 1;
u32 gx2_frames_presented(void) { return g_swaps; }
// SPLAT_VSYNC_TRACE=<frame>: every vsync wait and frame hand-over for 20
// frames from that frame on (who waits, how long after the frame was handed over)
static u32 g_vsync_trace = 0;
static void vsync_trace(const char* what, u32 lr, u64 since_flip, u64 slept) {
    if (!g_vsync_trace || g_swaps < g_vsync_trace || g_swaps >= g_vsync_trace + 20) return;
    printf("[vsync] frame %u %s (from %08X) %.2f ms after the frame was handed over, slept %.2f ms\n", g_swaps, what, lr,
           since_flip * 1000.0 / WIIU_TIMER_HZ, slept * 1000.0 / WIIU_TIMER_HZ);
}

static void GX2Init(CPUState* c) {
    (void)c;
    gx2_cmd_init();
    latte_cp_start();
}
static void GX2Flush(CPUState* c) { (void)c; gx2_flush(); }
static u64 g_drawdone_ns;   // time the game spent waiting in GX2DrawDone (for the log)
// (for the [sync] line: the game's waits for the GPU, per call site)
#define GW_SITES 6
static struct { u32 lr, n; u64 ns, max; } g_gw[GW_SITES];
static void gpu_wait_note(u32 lr, u64 ns) {
    int i;
    for (i = 0; i < GW_SITES; i++) {
        if (g_gw[i].lr == lr || !g_gw[i].lr) break;
    }
    if (i == GW_SITES) i = GW_SITES - 1;
    g_gw[i].lr = lr;
    g_gw[i].n++;
    g_gw[i].ns += ns;
    if (ns > g_gw[i].max) g_gw[i].max = ns;
}
static void GX2DrawDone(CPUState* c) {
    u64 t0 = hnow_ns();
    extern volatile u64 latte_cp_polls, latte_cp_packets, latte_cp_cur_header, latte_cp_idle_waits;
    u64 cp0 = latte_cp_packets, ci0 = latte_cp_idle_waits, cpl0 = latte_cp_polls;
    u64 ts = gx2_flush();
    gx2_cmd_note_wait(ts);
    bool ok = gx2_wait_retired(ts, 5000);
    if (hnow_ns() - t0 > 8000000ull) {   // (the Oct 31 bench's "pacing / system" late frames: the game waited ~20 ms here while the drawing thread reported 2-3 ms of work -- where did it go?)
        static int n = 0;
        static u64 last;
        u64 now = hnow_ns();
        if (n < 400 && now - last > 300000000ull) {
            n++;
            last = now;
            u64 emit, reach, retire, ahead;
            gx2_cmd_fence_times(ts, &emit, &reach, &retire, &ahead);
            extern u32 latte_render_deferred_count(void);
            printf("[drawdone] slow wait %.1f ms for fence %llu (frame %u): %llu ring words were queued ahead of it; the GPU thread reached it %.1f ms after the flush and it retired %.1f ms after; the GPU thread meanwhile: %llu packets, %llu idle waits, %llu polls; %s, %u copies/fences deferred, retired %llu, submitted %llu\n",
                   (now - t0) / 1e6, (unsigned long long)ts, g_swaps, (unsigned long long)ahead, reach && emit ? (reach - emit) / 1e6 : -1.0, retire && emit ? (retire - emit) / 1e6 : -1.0,
                   (unsigned long long)(latte_cp_packets - cp0), (unsigned long long)(latte_cp_idle_waits - ci0), (unsigned long long)(latte_cp_polls - cpl0),
                   gx2_cmd_gpu_sleeping() ? "GPU thread asleep now" : latte_cp_cur_header ? "GPU thread in a packet" : "GPU thread between packets",
                   latte_render_deferred_count(), (unsigned long long)gx2_retired(), (unsigned long long)gx2_last_submitted());
            if (n <= 4) { extern void latte_render_report_waits(FILE* out); latte_render_report_waits(stdout); }
        }
    }
    if (!ok) {   // (the boot stall, Oct 25: the drawing thread never finishes -- what does it wait for?)
        static int n = 0;
        if (n++ < 3) {
            extern void latte_render_report_waits(FILE* out);
            printf("[gx2] GX2DrawDone waited 5 s in vain: timestamp %llu asked for, %llu retired, %llu submitted\n", (unsigned long long)ts,
                   (unsigned long long)gx2_retired(), (unsigned long long)gx2_last_submitted());
            latte_render_report_waits(stdout);
            fflush(stdout);
        }
    }
    ret(c, ok ? 1 : 0);
    u64 dt = hnow_ns() - t0;
    g_drawdone_ns += dt;
    gpu_wait_note(c->lr, dt);
}
static void ret64(CPUState* c, u64 v) { c->gpr[3] = (u32)(v >> 32); c->gpr[4] = (u32)v; }
static void GX2GetLastSubmittedTimeStamp(CPUState* c) { ret64(c, gx2_last_submitted()); }
static void GX2GetRetiredTimeStamp(CPUState* c) { ret64(c, gx2_retired()); }
static void GX2WaitTimeStamp(CPUState* c) {   // (u64 ts in r3:r4)
    u64 ts = ((u64)arg(c, 0) << 32) | arg(c, 1);
    u64 t0 = hnow_ns();
    gx2_commit();
    ret(c, gx2_wait_retired(ts, 5000) ? 1 : 0);
    gpu_wait_note(c->lr | 1, hnow_ns() - t0);   // (|1: told apart from GX2DrawDone sites)
}

static void GX2Invalidate(CPUState* c) {   // (flags, addr, size)
    gx2_emit(PM4_TYPE3(IT_SURFACE_SYNC, 4));
    gx2_emit(arg(c, 0));
    gx2_emit(arg(c, 2) >> 8);
    gx2_emit(arg(c, 1) >> 8);
    gx2_emit(4);
}

// (colorBuffer, target): 1 TV, 4 GamePad
// The TV scan buffer's format. When it's an sRGB format (as Splatoon's is),
// copying the finished picture into it turns the linear colors into sRGB --
// that's the game's final brightness curve; without it everything looks
// far too dark.
static u32 g_tv_format = 0, g_drc_format = 0;
static void GX2SetTVBuffer(CPUState* c) {   // (buffer, size, render mode, surface format, buffering)
    g_tv_format = arg(c, 3);
    static int logged = 0;
    if (logged++ < 2) printf("[gx2] TV scan buffer: format %03X%s\n", g_tv_format, (g_tv_format & 0x400) ? " (sRGB)" : "");
}
// The GamePad screen's scan buffer (shown in the window with the View button).
static void GX2SetDRCBuffer(CPUState* c) {   // (buffer, size, render mode, surface format, buffering)
    g_drc_format = arg(c, 3);
    static int logged = 0;
    if (logged++ < 2) printf("[gx2] GamePad scan buffer: format %03X%s\n", g_drc_format, (g_drc_format & 0x400) ? " (sRGB)" : "");
}

static void GX2CopyColorBufferToScanBuffer(CPUState* c) {
    u32 b = arg(c, 0);
    u32 level = wiiu_read32(b + 0x74);
    gx2_emit(PM4_TYPE3(IT_HLE_COPY_TO_SCAN, 6));
    gx2_emit(surface_level_address(b, level));
    gx2_emit(wiiu_read32(b + 0x04) >> level);
    gx2_emit(wiiu_read32(b + 0x08) >> level);
    gx2_emit(wiiu_read32(b + 0x14));
    gx2_emit(wiiu_read32(b + 0x78));
    // (0x100: the TV buffer is sRGB; 0x200: it's known not to be)
    u32 scanfmt = (arg(c, 1) & 1) ? g_tv_format : g_drc_format;
    u32 tvfmt = !scanfmt ? 0u : (scanfmt & 0x400) ? 0x100u : 0x200u;
    gx2_emit(arg(c, 1) | tvfmt);
}

static void full_fps_on_swap(void);
// SPLAT_SPAWN_DETECT="A:B" (test runs only): which of the plaza's four start
// points the player got (the plaza picks one at random, so one fixed stick
// route can't reach a given place). At frame A, float triples (x, y, z) in
// game memory standing on a start point are noted; the run walks the player
// for a moment; at frame B the one that moved like a walking player (5-300
// units, same height) tells the start point: splat_test_spawn (0-3), and the
// autoplay then adds the keys of SPLAT_AUTOPLAY_S<n> (input.c).
int splat_test_spawn = -1;
// SPLAT_WALK_TO="x,z" (test runs, with SPLAT_SPAWN_DETECT): once the player's
// position is known, steer it there with the left stick. The stick is
// camera-relative and the camera follows behind the player, so the heading
// the player actually moves in (from its last positions) stands in for the
// camera's, and the stick angle is the bearing to the target minus that.
// On arrival A is pressed a few times (enters the Octo Valley manhole).
static struct { bool on, arrived; float tx, tz, px, pz, heading, lx, ly; u32 steps, arrived_at; bool have_heading; } g_walk;
void splat_walk_stick(float* lx, float* ly, bool* a) {
    if (!g_walk.on || !g_walk.steps) return;
    if (g_walk.arrived) {
        u32 since = g_swaps - g_walk.arrived_at;
        *a = since < 600 && (since % 90) < 12;   // (A for 0.2 s every 1.5 s, for 10 s)
        return;
    }
    *lx = g_walk.lx;
    *ly = g_walk.ly;
}
static void walk_to_step(float x, float z) {
    static int checked = 0;
    if (!checked) {
        checked = 1;
        const char* e = getenv("SPLAT_WALK_TO");
        g_walk.on = e && sscanf(e, "%f,%f", &g_walk.tx, &g_walk.tz) == 2;
    }
    if (!g_walk.on || g_walk.arrived) return;
    float dx = g_walk.tx - x, dz = g_walk.tz - z, dist = sqrtf(dx * dx + dz * dz);
    if (g_walk.steps && dist < 6.0f) {
        g_walk.arrived = true;
        g_walk.arrived_at = g_swaps;
        g_walk.lx = g_walk.ly = 0;
        printf("[walk] frame %u: arrived at %.1f, %.1f (%.1f away): pressing A\n", g_swaps, x, z, dist);
        return;
    }
    // bearing: 0 = towards -Z, 90 = towards +X (clockwise seen from above)
    float bearing = atan2f(dx, -dz) * 180.0f / 3.14159265f;
    if (g_walk.steps) {
        float mx = x - g_walk.px, mz = z - g_walk.pz, moved = sqrtf(mx * mx + mz * mz);
        if (moved > 1.0f) { g_walk.heading = atan2f(mx, -mz) * 180.0f / 3.14159265f; g_walk.have_heading = true; }
    }
    g_walk.px = x; g_walk.pz = z;
    g_walk.steps++;
    float a = g_walk.have_heading ? bearing - g_walk.heading : 0.0f;   // (unknown heading: walk straight first)
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    if (a > 60.0f) a = 60.0f;       // (gentle turns: the camera must keep up)
    if (a < -60.0f) a = -60.0f;
    float mag = dist < 20.0f ? 0.5f : 1.0f;
    g_walk.lx = mag * sinf(a * 3.14159265f / 180.0f);
    g_walk.ly = mag * cosf(a * 3.14159265f / 180.0f);
    if (g_walk.steps % 3 == 1)
        printf("[walk] frame %u: at %.1f, %.1f, %.0f away, bearing %.0f, heading %s%.0f, stick %.0f\n", g_swaps, x, z, dist, bearing,
               g_walk.have_heading ? "" : "?", g_walk.heading, a);
}
static void spawn_detect(void) {
    static int on = -1;
    static u32 at_a, at_b;
    static const float sp[4][2] = { { 0.0f, 179.7f }, { 0.0f, 30.0f }, { 225.1f, 228.7f }, { -199.1f, 145.2f } };
    if (on < 0) {
        const char* e = getenv("SPLAT_SPAWN_DETECT");
        on = e && sscanf(e, "%u:%u", &at_a, &at_b) == 2;
    }
    static u32 track;   // (after that: the player's position every 30 frames, for steering test routes)
    if (on && track && g_swaps > at_b && g_swaps % 10 == 0) {
        float x, y, z;
        u32 w = wiiu_read32(track); memcpy(&x, &w, 4);
        w = wiiu_read32(track + 4); memcpy(&y, &w, 4);
        w = wiiu_read32(track + 8); memcpy(&z, &w, 4);
        if (g_swaps % 30 == 0) printf("[spawn] frame %u: player at %.1f %.1f %.1f\n", g_swaps, x, y, z);
        walk_to_step(x, z);
    }
    // At frame A (the player has landed and stands still) every (x, y, z)
    // float triple at floor height inside the plaza is noted; the run then
    // walks the player for a moment, and at frame B the biggest group of
    // triples that all moved to the same spot like a walking player (20-80
    // units, same height) is the player's position (it's kept in several
    // copies; stray matches each go somewhere else).
    typedef struct { u32 addr; float x, y, z; } Cand;
    enum { CAND_MAX = 6 << 20 };
    static Cand* cand;
    static u32 n;
    if (!on || (g_swaps != at_a && g_swaps != at_b)) return;
    if (g_swaps == at_a) {
        if (!cand) cand = (Cand*)malloc((size_t)CAND_MAX * sizeof(Cand));
        for (u32 base = 0x10000000u; base < 0x50000000u && cand; base += 0x100000u) {
            const u8* m = wiiu_mem_ptr(base, 0x100000u);
            if (!m) continue;
            for (u32 i = 0; i + 12 <= 0x100000u && n < CAND_MAX; i += 4) {
                float x, y, z;
                u32 w = ((u32)m[i + 4] << 24) | ((u32)m[i + 5] << 16) | ((u32)m[i + 6] << 8) | m[i + 7];
                memcpy(&y, &w, 4);
                if (!(y > -2.0f && y < 6.0f)) continue;
                w = ((u32)m[i] << 24) | ((u32)m[i + 1] << 16) | ((u32)m[i + 2] << 8) | m[i + 3];
                memcpy(&x, &w, 4);
                if (!(x > -300.0f && x < 300.0f)) continue;
                w = ((u32)m[i + 8] << 24) | ((u32)m[i + 9] << 16) | ((u32)m[i + 10] << 8) | m[i + 11];
                memcpy(&z, &w, 4);
                if (!(z > -300.0f && z < 300.0f)) continue;
                if (fabsf(x) < 0.01f && fabsf(z) < 0.01f) continue;
                cand[n].addr = base + i; cand[n].x = x; cand[n].y = y; cand[n].z = z; n++;
            }
        }
        printf("[spawn] frame %u: %u places at floor height in the plaza\n", g_swaps, n);
        return;
    }
    enum { GROUPS = 8192 };
    static struct { float x, z, ox, oz; u32 n, addr; } grp[GROUPS];
    int ng = 0;
    u32 moved = 0;
    for (u32 c = 0; cand && c < n; c++) {
        float x, y, z;
        u32 w = wiiu_read32(cand[c].addr); memcpy(&x, &w, 4);
        w = wiiu_read32(cand[c].addr + 4); memcpy(&y, &w, 4);
        w = wiiu_read32(cand[c].addr + 8); memcpy(&z, &w, 4);
        float dx = x - cand[c].x, dz = z - cand[c].z, d = sqrtf(dx * dx + dz * dz);
        if (fabsf(x) < 3.0f && fabsf(z) < 3.0f) continue;
        if (fabsf(cand[c].x) < 3.0f && fabsf(cand[c].z) < 3.0f) continue;
        if (!(d > 20.0f && d < 80.0f && fabsf(y - cand[c].y) < 1.0f && x > -300.0f && x < 300.0f && z > -300.0f && z < 300.0f)) continue;
        moved++;
        int gi = 0;
        while (gi < ng && !(fabsf(grp[gi].x - x) < 0.5f && fabsf(grp[gi].z - z) < 0.5f && fabsf(grp[gi].ox - cand[c].x) < 0.5f && fabsf(grp[gi].oz - cand[c].z) < 0.5f)) gi++;
        if (gi == ng && ng < GROUPS) { grp[ng].x = x; grp[ng].z = z; grp[ng].ox = cand[c].x; grp[ng].oz = cand[c].z; grp[ng].n = 0; grp[ng].addr = cand[c].addr; ng++; }
        if (gi < ng) grp[gi].n++;
    }
    int bi = -1;
    for (int gi = 0; gi < ng; gi++) if (bi < 0 || grp[gi].n > grp[bi].n) bi = gi;
    for (int gi = 0; gi < ng; gi++) if (grp[gi].n >= 3)
        printf("[spawn] %u copies moved %.1f, %.1f -> %.1f, %.1f (first at %08X)\n", grp[gi].n, grp[gi].ox, grp[gi].oz, grp[gi].x, grp[gi].z, grp[gi].addr);
    printf("[spawn] frame %u: %u of %u moved like a walk, in %d groups\n", g_swaps, moved, n, ng);
    if (bi >= 0 && grp[bi].n >= 2) {
        track = grp[bi].addr;
        for (int s = 0; s < 4; s++) if (fabsf(grp[bi].ox - sp[s][0]) < 8.0f && fabsf(grp[bi].oz - sp[s][1]) < 8.0f) splat_test_spawn = s;
        printf("[spawn] the player: %u copies, started at %.1f, %.1f (start point %d), tracked at %08X\n", grp[bi].n, grp[bi].ox, grp[bi].oz, splat_test_spawn, track);
    } else printf("[spawn] the player wasn't found\n");
    free(cand);
    cand = NULL;
    n = 0;
}
// How many vsync waits the game makes per frame (1 at 60 fps, 2 when a scene
// runs at 30 -- the plaza in Performance mode): the stutter analyzer's idea of
// "on time" (frame_tracker.c). The count of the last 4 frames when they agree.
volatile int splat_vsyncs_per_swap = 1;
static void note_vsyncs_per_swap(void) {
    static u32 last_calls, hist[4], n;
    u32 v = g_vsync_calls - last_calls;
    last_calls = g_vsync_calls;
    hist[n++ & 3] = v;
    if (n >= 4 && hist[0] == hist[1] && hist[1] == hist[2] && hist[2] == hist[3] && v >= 1 && v <= 4) splat_vsyncs_per_swap = (int)v;
}
static void GX2SwapScanBuffers(CPUState* c) {
    spawn_detect();
    (void)c;
    g_swaps++;
    { extern void watch_poll(u32 frame); watch_poll(g_swaps); }   // (SPLAT_WATCH_HEX tests: nan_trap.c)
    note_vsyncs_per_swap();
    g_swap_target[g_swaps & 15] = g_vk.low_latency ? g_ll_next_qpc : 0;
    gx2_emit(PM4_TYPE3(IT_HLE_SWAP, 1));
    gx2_emit(g_swaps);
    gx2_flush();
    g_flips++;
    full_fps_on_swap();
    if (g_vsync_trace) vsync_trace("hand-over", c->lr - 4, wiiu_time_ticks() - g_last_flip, 0);
    g_last_flip = wiiu_time_ticks();
#ifdef _WIN32
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_swap_qpc = (u64)q.QuadPart; }
#endif
    display_frame_done();
    {   // how even the frames are (choppiness): the gaps between frames
        static u64 last, n, sum, worst, over40, over60;
        static u32 hist[101];   // (1 ms steps; the last one is 100 ms and up)
        u64 now = wiiu_time_ticks();
        static u64 waited_before;
        u64 waited = threads_main_waited_ns();
        if (last) {
            u64 us = (now - last) * 1000000ull / WIIU_TIMER_HZ;
            {   // each second: frames, the slowest one, and how much of it ZR (shoot) was held
                extern bool input_zr_held(void);
                extern bool input_zl_held(void);
                static u64 sec_start, sec_worst;
                static u32 sec_frames, sec_zr, sec_zl, sec_n;
                if (!sec_start) sec_start = now;
                sec_frames++;
                if (input_zr_held()) sec_zr++;
                if (input_zl_held()) sec_zl++;
                if (us > sec_worst) sec_worst = us;
                if (now - sec_start >= WIIU_TIMER_HZ && sec_n < 3600) {
                    sec_n++;
                    printf("[fps] %u frames this second, slowest %.0f ms, shooting %u%% of frames, squid %u%%\n", sec_frames, sec_worst / 1000.0,
                           sec_frames ? sec_zr * 100 / sec_frames : 0, sec_frames ? sec_zl * 100 / sec_frames : 0);
                    {   // the game's waits for the GPU this second, and what the GPU thread read back
                        static int on = -1;   // (SPLAT_SYNC_LOG=0 turns it off)
                        if (on < 0) { const char* e = getenv("SPLAT_SYNC_LOG"); on = !e || atoi(e) != 0; }
                        if (on) {
                            GX2FenceStats fs;
                            LatteSyncStats ss;
                            gx2_cmd_fence_stats(&fs);
                            latte_render_sync_stats(&ss);
                            double f = sec_frames ? sec_frames : 1;
                            printf("[sync] waits per frame:");
                            for (int i = 0; i < GW_SITES; i++) {
                                if (!g_gw[i].lr) continue;
                                printf(" %s %08X %.2f ms (%.1fx, max %.1f)", (g_gw[i].lr & 1) ? "WaitTS" : "DrawDone", g_gw[i].lr & ~1u, g_gw[i].ns / 1e6 / f,
                                       g_gw[i].n / f, g_gw[i].max / 1e6);
                            }
                            memset(g_gw, 0, sizeof(g_gw));
                            printf(" | waited-for fences %u: flush->GPU thread %.2f ms, ->retired %.2f ms", fs.wn, fs.wn ? fs.wreach_sum / 1e6 / fs.wn : 0,
                                   fs.wn ? fs.wretire_sum / 1e6 / fs.wn : 0);
                            printf(" | fences %u: flush->GPU thread %.2f ms (max %.1f), ->retired %.2f ms (max %.1f), %u held for copies"
                                   " | per frame: stream-out draws %.1f (%.0f B), card waits %.1f (%.2f ms), draws reading SO %.1f, "
                                   "readback batches %.1f (%.1f buffers, %.0f KB, encode %.2f ms), submits %.1f\n",
                                   fs.n, fs.n ? fs.reach_sum / 1e6 / fs.n : 0, fs.reach_max / 1e6, fs.n ? fs.retire_sum / 1e6 / fs.n : 0, fs.retire_max / 1e6, ss.fences_deferred,
                                   ss.so_draws / f, ss.so_bytes / f, ss.so_waits / f, ss.so_wait_ns / 1e6 / f, ss.so_draw_reads / f,
                                   ss.rb_batches / f, ss.rb_surfs / f, ss.rb_bytes / 1024.0 / f, ss.rb_encode_ns / 1e6 / f, ss.submits / f);
                            printf("[tv] per frame: scene draws %.1f, other draws %.1f, skipped (shader %u, pipeline %u), copies to the TV %.2f from %08X (drawn in frame %llu), "
                                   "texture memo hits %.0f, depth-copy reuses %.1f\n",
                                   ss.scene_draws / f, ss.other_draws / f, ss.skip_shader, ss.skip_pipe, ss.scan_copies / f, ss.scan_src,
                                   (unsigned long long)ss.scan_src_seen, ss.memo_hits / f, ss.fb_reuse / f);
                        }
                    }
                    sec_start = now; sec_frames = sec_zr = sec_zl = 0; sec_worst = 0;
                }
            }
            {   // (a slow frame on the game's side: was the game busy, or waiting -- and on what?)
                static u32 logged;
                extern void threads_main_longest_wait(const char** what, u32* lr, u64* ns);
                const char* lw = NULL;
                u32 llr = 0;
                u64 lns = 0;
                threads_main_longest_wait(&lw, &llr, &lns);
                {
                    extern void tracker_game_frame(u32 swap, u64 us, u64 waited_ns, const char* wait_what, u32 wait_lr, u64 wait_ns);
                    tracker_game_frame(g_swaps, us, waited - waited_before, lw, llr, lns);   // (frame_tracker.c: the stutter analyzer)
                }
                if (us > 50000 && logged < 400) {
                    logged++;
                    printf("[spike] game frame %u: %.0f ms, the main thread waited %.0f ms of it (longest: %s from %08X, %.1f ms)\n", g_swaps, us / 1000.0,
                           (waited - waited_before) / 1e6, lw ? lw : "-", llr, lns / 1e6);
                }
            }
            n++; sum += us;
            if (us > worst) worst = us;
            if (us > 40000) over40++;
            if (us > 60000) over60++;
            hist[us / 1000 < 100 ? us / 1000 : 100]++;
        }
        last = now;
        waited_before = waited;
        if (g_swaps % 600 == 0 && n) {
            // (the frame time that 50% / 95% / 99% of frames were faster than)
            u32 p[3] = { 0, 0, 0 }, acc = 0, k = 0;
            const double q[3] = { 0.50, 0.95, 0.99 };
            for (u32 i = 0; i <= 100 && k < 3; i++) { acc += hist[i]; while (k < 3 && acc >= q[k] * n) p[k++] = i + 1; }
            printf("[gx2] frame times: average %.1f ms, slowest %.1f ms, %llu over 40 ms, %llu over 60 ms (of %llu); 50%%/95%%/99%% under %u/%u/%u ms; swap interval %u, %u vsync waits, %u flip waits\n", sum / 1000.0 / n, worst / 1000.0,
                   (unsigned long long)over40, (unsigned long long)over60, (unsigned long long)n, p[0], p[1], p[2], g_swap_interval, g_vsync_calls, g_flip_calls);
            g_vsync_calls = g_flip_calls = 0;
            if (g_ll_waits) {
                printf("[gx2] low input lag: %llu vsync waits paced to the screen's refresh (every %.2f ms): the game's work %.1f ms per frame, woken %.1f ms before a refresh\n",
                       (unsigned long long)g_ll_waits, g_ll_per * 1000.0 / (double)g_ll_freq, g_ll_work_sum * 1000.0 / (double)g_ll_freq / g_ll_waits,
                       g_ll_lead_sum * 1000.0 / (double)g_ll_freq / g_ll_waits);
                g_ll_waits = g_ll_lead_sum = g_ll_work_sum = 0;
            }
            if (g_vb_hits || g_vb_misses) {
                printf("[gx2] low input lag: %llu vsync waits ended by the screen's refresh, %llu by the clock (no word within 25 ms)\n",
                       (unsigned long long)g_vb_hits, (unsigned long long)g_vb_misses);
                g_vb_hits = g_vb_misses = 0;
            }
            if (g_vs_waits)
                printf("[gx2] vsync waits woke up late by %.2f ms on average (worst %.2f ms, %llu waits)\n", g_vs_late_sum * 1000.0 / WIIU_TIMER_HZ / g_vs_waits,
                       g_vs_late_max * 1000.0 / WIIU_TIMER_HZ, (unsigned long long)g_vs_waits);
            g_vs_late_sum = g_vs_late_max = g_vs_waits = 0;
            threads_report_main_waits((u32)n);
            hle_report_main_calls((u32)n);
            { extern void splat_report_call_hist(void); splat_report_call_hist(); }
            {   // SPLAT_MEMSCAN=XLNK,BARS,...: once (at frame 1800), where these 4-byte
                // tags are in game memory, with the next 12 bytes (finding loaded resources)
                static int done = 0;
                const char* e = getenv("SPLAT_MEMSCAN");
                if (e && !done && g_swaps >= 1800) {
                    done = 1;
                    char list[256];
                    snprintf(list, sizeof(list), "%s", e);
                    for (char* tag = strtok(list, ","); tag; tag = strtok(NULL, ",")) {
                        if (strlen(tag) != 4) continue;
                        int hits = 0;
                        for (u32 base = 0x10000000u; base < 0x50000000u && hits < 40; base += 0x100000u) {
                            const u8* m = wiiu_mem_ptr(base, 0x100000u + 16);
                            if (!m) continue;
                            for (u32 i = 0; i < 0x100000u && hits < 40; i += 4)
                                if (m[i] == (u8)tag[0] && !memcmp(m + i, tag, 4)) {
                                    printf("[memscan] %s at %08X:", tag, base + i);
                                    for (int k = 4; k < 16; k++) printf(" %02X", m[i + k]);
                                    printf("\n");
                                    hits++;
                                }
                        }
                        printf("[memscan] %s: %d found\n", tag, hits);
                    }
                }
            }
            {   // SPLAT_AB_FAST_CALLS=1 / SPLAT_AB_WAITS=1 / SPLAT_AB_VSYNC=1: fast calls /
                // quick waits / frame-based vsync on and off by turns, 600 frames
                // each (same scene, both ways)
                extern void splat_set_fast_calls(int on);
                static int ab = -1, on = 1, waits = 0, vs = 0;
                if (ab < 0) {
                    waits = getenv("SPLAT_AB_WAITS") != NULL;
                    vs = getenv("SPLAT_AB_VSYNC") != NULL;
                    ab = waits || vs || getenv("SPLAT_AB_FAST_CALLS") != NULL;
                }
                if (ab) {
                    on = !on;
                    if (vs) g_clock_vsync = !on;
                    else if (waits) threads_set_quick_waits(on);
                    else splat_set_fast_calls(on);
                    printf("[ab] %s %s for the next 600 frames\n", vs ? "frame-based vsync" : waits ? "quick waits" : "fast calls", on ? "ON" : "OFF");
                }
            }
            n = sum = worst = over40 = over60 = 0;
            memset(hist, 0, sizeof(hist));
        }
    }
    if (g_swaps == 1 || g_swaps % 600 == 0) {
        printf("[gx2] frame %u (the game waited %.1f ms per frame for the GPU thread to catch up)\n", g_swaps, g_drawdone_ns / 1e6 / 600.0);
        g_drawdone_ns = 0;
    }
    // Don't let the game get more than 1 frame ahead of the GPU thread: each
    // frame waiting in line is one more frame between pressing a button and
    // seeing it happen. (SPLAT_FRAMES_AHEAD=2 allows 2, as before.)
    static u32 ahead = 0;
    if (!ahead) { const char* e = getenv("SPLAT_FRAMES_AHEAD"); ahead = e && atoi(e) >= 1 && atoi(e) <= 4 ? (u32)atoi(e) : 1; }
    u32 done = latte_cp_frames_done();
    if (g_swaps > done + ahead) {
        for (int i = 0; i < 200 && g_swaps > latte_cp_frames_done() + ahead; i++) threads_sleep_ticks(WIIU_TIMER_HZ / 1000);
    }
}
// 60 fps where the game runs at 30 (the plaza). The game's frame loop
// (Lp/sead framework, 028E4DC4) runs every 1/60 s everywhere; with bit 0 of
// the framework's word at +116 set it draws only every other time round,
// bit 1 being the phase -- and the game's own code splits work between the
// two phases (02894C4C, 026DD96C, 02703EB0, 0262501C read them: the plaza's
// characters are updated on the frames that aren't drawn). The scene manager
// (the object at 0x101E7BDC) sets that mode every frame from its scene's
// settings: bit 0 of its word at +772 = "this scene runs at 30" (02AD5DD8 /
// 02AD5E44; nothing else reads that bit), switching only between phases.
// Clearing that request makes the game run the scene the way it runs
// battles: everything every frame, every frame drawn, same game speed.
// (Clearing the framework's bits directly drew every frame but stopped the
// other phase's work: the plaza's people froze in their bind pose.)
// SPLAT_GAME_30FPS=1: leave the game's setting alone.
#define LP_SCENE_MGR_PTR 0x101E7BDCu
static int g_full_fps = -1;
// The request is made once, at the scene's start: clearing it there made the "Plaza frame rate" / Performance
// mode settings take effect only at the next visit. The maps that asked are remembered (the place from fs.c),
// so the bit goes back on / off as the setting changes.
static void full_fps_on_swap(void) {
    if (g_full_fps < 0) g_full_fps = getenv("SPLAT_GAME_30FPS") == NULL;
    extern volatile int splat_game_own_fps;   // (Performance mode, "Plaza frame rate")
    extern char splat_scene_map[64];
    u32 sm = wiiu_read32(LP_SCENE_MGR_PTR);
    if (!sm || (sm & 3) || !wiiu_mem_ptr(sm, 776)) return;
    u32 v = wiiu_read32(sm + 772);
    static char asked[8][64];
    static int nasked;
    int k = 0;
    while (k < nasked && strcmp(asked[k], splat_scene_map)) k++;
    if (v & 1) {
        if (k == nasked && nasked < 8 && splat_scene_map[0]) { snprintf(asked[nasked], 64, "%s", splat_scene_map); nasked++; }
    } else if (k == nasked) return;   // (a scene that never asked)
    bool want30 = !g_full_fps || splat_game_own_fps;
    static int last = -1;
    if (want30 && !(v & 1)) wiiu_write32(sm + 772, v | 1u);
    if (!want30 && (v & 1)) wiiu_write32(sm + 772, v & ~1u);
    if (last != (int)want30) {
        last = want30;
        printf("[gx2] the scene asks for 30 fps (%s, frame %u): %s\n", splat_scene_map[0] ? splat_scene_map : "?", g_swaps,
               want30 ? "kept (Performance mode / Plaza frame rate 30)" : "running it at 60 instead (SPLAT_GAME_30FPS=1 keeps 30)");
    }
}
// The next "vsync" comes one refresh (1/60 s) after the last frame was handed
// over -- not at the clock's next 1/60 s mark: a frame that already took
// longer than a refresh doesn't wait at all (it used to wait up to another
// 16 ms for the mark, ~2 ms a frame on average in the plaza), and a fast one
// still can't go faster than 60. Only with SPLAT_FRAME_VSYNC=1: measured 0.5 ms a frame slower than the clock marks.
// --- Low input lag: the game's vsync follows the screen -----------------------
// With the setting on (and VK_KHR_present_wait in the driver), the GPU thread
// waits after each present until that frame is actually on the screen and
// says so here; GX2WaitForVsync then ends at that moment instead of at the
// next mark of the free-running 60 Hz clock. The game's next frame starts
// right as the previous one is shown: one frame of input lag. A frame that
// misses its refresh costs a whole refresh (no slack), so this is a setting.
// Without a word from the screen within 25 ms the clock takes over.
static hmtx_t g_vb_m;
static hcnd_t g_vb_c;
static volatile bool g_vb_init = false;
static u64 g_vb_seq = 0, g_vb_seen = 0;
void gx2_note_vblank(void) {
    if (!g_vb_init) return;
    hmtx_lock(&g_vb_m);
    g_vb_seq++;
    WakeAllConditionVariable(&g_vb_c);
    hmtx_unlock(&g_vb_m);
}
static bool vblank_wait(void) {
    if (!g_vb_init) { hmtx_init(&g_vb_m, false); hcnd_init(&g_vb_c); g_vb_init = true; }
    if (!threads_quick_waits()) return false;
    u64 t0 = hnow_ns();
    int big = hle_big_release_all();
    hmtx_lock(&g_vb_m);
    while (g_vb_seq <= g_vb_seen && hnow_ns() - t0 < 25000000ull) SleepConditionVariableCS(&g_vb_c, &g_vb_m, 1);
    bool ok = g_vb_seq > g_vb_seen;
    g_vb_seen = g_vb_seq;
    hmtx_unlock(&g_vb_m);
    hle_big_reacquire(big);
    threads_note_host_wait("screen refresh", hnow_ns() - t0);
    if (ok) g_vb_hits++; else g_vb_misses++;
    return ok;
}
// Low input lag, the way that works in a window on Windows: the desktop
// compositor (DWM) tells when the screen's last refresh was and how long a
// refresh is (DwmGetCompositionTimingInfo). The game is woken so that its
// frame -- its own work, measured, plus ~3 ms for the GPU thread -- ends
// just before the next refresh: the picture is then shown one refresh after
// the game read the controller instead of one to two. Never two frames in
// one refresh. (The image-acquire approach above is kept for a driver whose
// acquires block; this one's don't in a window.)
#ifdef _WIN32
#include <dwmapi.h>
static bool dwm_vblank(u64* vb, u64* per) {
    static HMODULE m = NULL;
    static HRESULT(WINAPI * fn)(HWND, DWM_TIMING_INFO*) = NULL;
    static int tried = 0;
    if (!tried) { tried = 1; m = LoadLibraryA("dwmapi.dll"); if (m) fn = (HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*))GetProcAddress(m, "DwmGetCompositionTimingInfo"); }
    if (!fn) return false;
    DWM_TIMING_INFO ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    if (fn(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod) return false;
    *vb = ti.qpcVBlank;
    *per = ti.qpcRefreshPeriod;
    return true;
}
#endif
static void GX2WaitForVsync(CPUState* c) {
    g_vsync_calls++;
#ifdef _WIN32
    u64 vb, per;
    if (g_vk.low_latency && dwm_vblank(&vb, &per)) {
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        u64 now_q = (u64)q.QuadPart;
        if (g_wake_qpc && g_swap_qpc > g_wake_qpc) {
            s64 work = (s64)(g_swap_qpc - g_wake_qpc);
            g_work_avg = g_work_avg ? (g_work_avg * 7 + work) / 8 : work;
        }
        // (a 120 / 144 / 180 Hz screen: the game's frame spans several refreshes; the compositor's "last refresh" can lie a little ahead of now)
        u64 game_per = per * (u64)((16666.7 * (double)f.QuadPart / 1e6) / (double)per + 0.5);
        if (game_per < per) game_per = per;
        while (vb > now_q) vb -= per;
        // (the frame's work plus 4.5 ms for the GPU thread and jitter: with 3 ms, ~4% of presents went out
        // 0-1 ms after the refresh they were meant for -- shown a refresh late)
        extern volatile s64 g_pace_extra_ns;   // (latte_render.c pace_feedback: more lead while presents come out close to their refresh)
        s64 lead = g_work_avg + (s64)(f.QuadPart * 9 / 2000) + (s64)(g_pace_extra_ns * f.QuadPart / 1000000000);
        // (up to 1.5 frames: in the busy plaza the game's ~14 ms of work plus the drawing thread's tail outgrew one
        // frame -- starting a frame before the last one is shown keeps it in time; extra input lag only then)
        if (lead > (s64)(game_per + game_per / 2)) lead = (s64)(game_per + game_per / 2);
        // A locked 60: each frame aims at the refresh one 60 Hz frame (3 refreshes at 180 Hz) after the
        // one the last frame aimed at, so frames reach the screen evenly. (Before: the next refresh we
        // could make, never faster than 60 "give or take 2%" -- the game ran at ~61.5 and slipped against
        // the screen: ~10% of frames shown 2 refreshes after the last, ~5% 4.) A frame too late for its
        // refresh takes the next one it can make, and the rhythm goes on from there.
        // SPLAT_FREE_PACING=1: the old way.
        static int free_pacing = -1;
        if (free_pacing < 0) free_pacing = getenv("SPLAT_FREE_PACING") != NULL;
        u64 next = vb + ((now_q - vb) / per + 1) * per;   // the next refresh
        s64 wake = (s64)next - lead;
        while (wake < (s64)now_q && (s64)(next + per) - lead > (s64)now_q && next + per - now_q < game_per) { next += per; wake = (s64)next - lead; }   // (a later refresh we can still make)
        if (!free_pacing && g_ll_next_qpc) {
            u64 want = g_ll_next_qpc + game_per;   // (the rhythm's next refresh)
            s64 reach = (s64)now_q + lead - (s64)(f.QuadPart / 1000);   // (the earliest refresh this frame can still make, within 1 ms)
            if ((s64)want < reach) want += ((u64)(reach - (s64)want) + per - 1) / per * per;   // (late: the first one after it we can make -- never earlier)
            next = want;
            wake = (s64)next - lead;
        }
        if (free_pacing && g_wake_qpc && wake < (s64)(g_wake_qpc + game_per - game_per / 50)) wake = (s64)(g_wake_qpc + game_per - game_per / 50);   // (never faster than 60, give or take 2%)
        if (g_ll_next_qpc) {   // (the refreshes between this frame's target and the last one's: the frame tracker's "paced")
            extern void tracker_paced(int refreshes);
            tracker_paced((int)(((s64)(next - g_ll_next_qpc) + (s64)per / 2) / (s64)per));
        }
        g_ll_next_qpc = next;
        if (wake > (s64)now_q) threads_sleep_ticks_precise((u64)(wake - (s64)now_q) * WIIU_TIMER_HZ / (u64)f.QuadPart);
        QueryPerformanceCounter(&q);
        g_wake_qpc = (u64)q.QuadPart;
        g_last_vsync = wiiu_time_ticks();
        g_vs_waits++;
        g_ll_waits++;
        g_ll_lead_sum += (u64)lead;
        g_ll_work_sum += (u64)g_work_avg;
        g_ll_per = game_per;
        g_ll_freq = (u64)f.QuadPart;
        return;
    }
#endif
    if (g_vk.low_latency && g_vk.present_wait && vblank_wait()) {
        g_last_vsync = wiiu_time_ticks();
        g_vs_waits++;
        return;
    }
    u64 period = WIIU_TIMER_HZ / 60, now = wiiu_time_ticks();
    if (g_clock_vsync < 0) {
        g_clock_vsync = getenv("SPLAT_FRAME_VSYNC") == NULL;   // (A/B: frame-based was 0.5 ms a frame slower, so off)
        const char* e = getenv("SPLAT_VSYNC_TRACE");
        g_vsync_trace = e ? (u32)atoi(e) : 0;
    }
    u64 next = (now / period + 1) * period;
    // (one refresh after the later of the last frame and the last vsync: waiting
    // for several vsyncs in a row still takes a refresh each)
    if (!g_clock_vsync && g_last_flip) next = (g_last_flip > g_last_vsync ? g_last_flip : g_last_vsync) + period;
    if (next > now) threads_sleep_ticks_precise(next - now);
    g_last_vsync = wiiu_time_ticks();
    if (next > now) {
        u64 late = g_last_vsync > next ? g_last_vsync - next : 0;
        g_vs_late_sum += late;
        if (late > g_vs_late_max) g_vs_late_max = late;
        g_vs_waits++;
    }
    if (g_vsync_trace) vsync_trace("vsync wait", c->lr - 4, now - g_last_flip, g_last_vsync - now);
}
static void GX2WaitForFlip(CPUState* c) { (void)c; g_flip_calls++; }
static void GX2GetSwapStatus(CPUState* c) {   // (*swapCount, *flipCount, *lastFlip, *lastVsync)
    if (arg(c, 0)) wiiu_write32(arg(c, 0), g_swaps);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), g_flips);
    if (arg(c, 2)) { wiiu_write32(arg(c, 2), (u32)(g_last_flip >> 32)); wiiu_write32(arg(c, 2) + 4, (u32)g_last_flip); }
    if (arg(c, 3)) { wiiu_write32(arg(c, 3), (u32)(g_last_vsync >> 32)); wiiu_write32(arg(c, 3) + 4, (u32)g_last_vsync); }
}
static void GX2SetSwapInterval(CPUState* c) { if (arg(c, 0) < 20) g_swap_interval = arg(c, 0); }
static void GX2GetSwapInterval(CPUState* c) { ret(c, g_swap_interval); }
static void GX2GPUTimeToCPUTime(CPUState* c) { (void)c; }   // u64 in r3:r4 stays the same
static void GX2SampleTopGPUCycle(CPUState* c) {
    u64 t = wiiu_time_ticks();
    if (arg(c, 0)) { wiiu_write32(arg(c, 0), (u32)(t >> 32)); wiiu_write32(arg(c, 0) + 4, (u32)t); }
}
static void GX2SampleBottomGPUCycle(CPUState* c) {
    // "no value": games that see GPU timings sometimes start scaling their
    // resolution down, which we don't want.
    if (arg(c, 0)) { wiiu_write32(arg(c, 0), 0xFFFFFFFF); wiiu_write32(arg(c, 0) + 4, 0xFFFFFFFF); }
}
// (timestamp*, u64 value r5:r6, type, interrupt): write when the GPU gets here.
static void GX2SubmitUserTimeStamp(CPUState* c) {
    gx2_emit(PM4_TYPE3(IT_MEM_WRITE, 4));
    gx2_emit(arg(c, 0) | 2);
    gx2_emit(0);
    gx2_emit(arg(c, 3));
    gx2_emit(arg(c, 2));
}
// Occlusion queries: how many pixels of the draws in between passed the
// depth/stencil tests. Splatoon uses them to find out what ink is under the
// players (and more), so the counts must be real. The renderer writes the
// result into the query's memory: word 0 = 'QRDY' when done, words 2-3 = count.
#define QUERY_READY 0x51524459u
static u32 g_qstat_begin[4], g_qstat_get, g_qstat_ready, g_qstat_cached, g_qstat_wait, g_qstat_none, g_qstat_lost;
static void qstat_report(void) {
    static u32 next = 2000;
    if (g_qstat_get < next) return;
    next *= 4;
    printf("[gx2] occlusion queries: begun %u/%u/%u/%u (by type); results asked %u: ready %u, newest finished %u, after a short wait %u, not ready %u, lost %u\n",
           g_qstat_begin[0], g_qstat_begin[1], g_qstat_begin[2], g_qstat_begin[3], g_qstat_get, g_qstat_ready, g_qstat_cached, g_qstat_wait, g_qstat_none, g_qstat_lost);
}
// When each query was last started (frame number), to spot lost ones.
#define QBEGIN_N 4096
static struct { u32 addr, frame; } g_qbegin[QBEGIN_N];
static u32* qbegin_slot(u32 q) {
    u32 i = ((q >> 4) * 2654435761u) >> 20;
    for (u32 n = 0; n < 16; n++, i = (i + 1) & (QBEGIN_N - 1))
        if (g_qbegin[i].addr == q || !g_qbegin[i].addr) { g_qbegin[i].addr = q; return &g_qbegin[i].frame; }
    i = ((q >> 4) * 2654435761u) >> 20;
    g_qbegin[i].addr = q;
    return &g_qbegin[i].frame;
}
static void GX2QueryBegin(CPUState* c) {   // (type, query)
    u32 type = arg(c, 0), q = arg(c, 1);
    g_qstat_begin[type & 3]++;
    if (!q || (type != 0 && type != 2)) return;   // occlusion (CPU / GPU) only
    *qbegin_slot(q) = g_swaps;
    wiiu_write32(q, 0);
    wiiu_write32(q + 8, 0);
    wiiu_write32(q + 12, 0);
    gx2_emit(PM4_TYPE3(IT_HLE_QUERY, 2));
    gx2_emit(1);
    gx2_emit(q);
}
static void GX2QueryEnd(CPUState* c) {   // (type, query)
    u32 type = arg(c, 0), q = arg(c, 1);
    if (!q || (type != 0 && type != 2)) return;
    gx2_emit(PM4_TYPE3(IT_HLE_QUERY, 2));
    gx2_emit(2);
    gx2_emit(q);
}
// Conditional rendering: the draws until the End are only made if the query
// saw something (predicate set) / saw nothing (predicate clear) -- light
// flares behind walls, things hidden behind the player, ...
static void GX2QueryBeginConditionalRender(CPUState* c) {   // (type, query, hint, predicate)
    u32 q = arg(c, 1);
    if (!q) return;
    gx2_emit(PM4_TYPE3(IT_HLE_QUERY, 2));
    gx2_emit(3 | (arg(c, 3) ? 0x100u : 0u));
    gx2_emit(q);
}
static void GX2QueryEndConditionalRender(CPUState* c) {
    (void)c;
    gx2_emit(PM4_TYPE3(IT_HLE_QUERY, 2));
    gx2_emit(4);
    gx2_emit(0);
}
// Results: the count of this very query once the graphics card has done it.
// The GPU thread gets a short moment to finish it (the console's GPU is
// done with last frame's queries long before the game asks). If it still
// isn't done, the game gets the newest finished count of this query --
// never a value frozen from long ago (that made every ink check answer "no
// ink" forever: no squid swim), and never "not ready" (some parts of the
// game stop drawing the world when that happens -- the frozen picture with
// the glowing circle).
static void GX2QueryGetOcclusionResult(CPUState* c) {   // (query, *u64) -> ready
    u32 q = arg(c, 0), out = arg(c, 1);
    u32 hi = 0, lo = 0;
    g_qstat_get++;
    qstat_report();
    if (!q) { hi = 0; lo = 1000; }
    else if (wiiu_read32(q) == QUERY_READY) {
        g_qstat_ready++;
        hi = wiiu_read32(q + 8);
        lo = wiiu_read32(q + 12);
    } else {
        if (wiiu_read32(q) == QUERY_READY) {
            g_qstat_wait++;
            hi = wiiu_read32(q + 8);
            lo = wiiu_read32(q + 12);
        } else if (latte_render_query_last(q, &lo)) {
            g_qstat_cached++;
        } else if (g_swaps - *qbegin_slot(q) > 8) {
            // started long ago and never counted: lost on the way. Answer 0
            // so the game's checks don't wait for it forever.
            static int shown = 0;
            if (shown++ < 12) printf("[gx2] occlusion query %08X never finished (started frame %u, now %u; asked from %08X)\n", q, *qbegin_slot(q), g_swaps, c->lr);
            g_qstat_lost++;
            lo = 0;
        } else {
            g_qstat_none++;
            ret(c, 0);
            return;
        }
    }
    if (out) { wiiu_write32(out, hi); wiiu_write32(out + 4, lo); }
    {   // a sample of the answers in the log (for finding ink-check problems)
        static u32 n = 0, shown = 0;
        if ((++n % 97) == 0 && shown < 60) { shown++; printf("[gx2] query %08X -> %u samples (called from %08X)\n", q, lo, c->lr); }
    }
    ret(c, 1);
}
static void GX2AllocateTilingApertureEx(CPUState* c) {   // (surface, level, slice, endian, *handle, *address)
    u32 surf = arg(c, 0);
    if (arg(c, 4)) wiiu_write32(arg(c, 4), 1);
    if (arg(c, 5)) wiiu_write32(arg(c, 5), wiiu_read32(surf + 0x24));
}
static void GX2GetSystemTVScanMode(CPUState* c) { ret(c, 3); }   // 720p
static void GX2GetSystemTVAspectRatio(CPUState* c) { ret(c, 1); }   // 16:9
static void GX2GetMainCoreId(CPUState* c) { ret(c, 1); }
static void GX2GetGPUTimeout(CPUState* c) { ret(c, 1000); }
static void GX2GetLastFrame(CPUState* c) { ret(c, 0); }
static void GX2GetLastFrameGamma(CPUState* c) { if (arg(c, 1)) wiiu_write32(arg(c, 1), fbits(1.0f)); ret(c, 1); }

// One side of a surface copy (9 words): address, format, width, height,
// slice, tile mode, pitch in pixels, swizzle, dim | slices << 8 -- all for
// that mip level.
static void emit_copy_side(u32 s, u32 addr, u32 level, u32 slice) {
    u32 levels = wiiu_read32(s + 0x10);
    if (levels < level + 1) levels = level + 1;
    if (levels > 14) levels = 14;
    GX2Layout L;
    gx2_surface_layout(wiiu_read32(s + 0x00), wiiu_read32(s + 0x04), wiiu_read32(s + 0x08), wiiu_read32(s + 0x0C), levels,
                       wiiu_read32(s + 0x14), wiiu_read32(s + 0x18), wiiu_read32(s + 0x1C), wiiu_read32(s + 0x30),
                       wiiu_read32(s + 0x34), &L);
    if (level > 13) level = 13;
    const GX2LevelLayout* lv = &L.level[level];
    bool block;
    gx2_format_bits(wiiu_read32(s + 0x14), &block);
    gx2_emit(addr);
    gx2_emit(wiiu_read32(s + 0x14));
    gx2_emit(lv->width);
    gx2_emit(lv->height);
    gx2_emit(slice);
    gx2_emit(level ? lv->mode : wiiu_read32(s + 0x30));
    gx2_emit(block ? (level ? lv->pitch : wiiu_read32(s + 0x3C)) * 4 : (level ? lv->pitch : wiiu_read32(s + 0x3C)));
    gx2_emit(lv->swizzled || (level == 0 && wiiu_read32(s + 0x30) >= 4) ? wiiu_read32(s + 0x34) & 0x700 : 0);
    gx2_emit((wiiu_read32(s + 0x00) & 7) | (lv->depth & 0xFFFF) << 8);
}

// GX2CopySurface(src, srcLevel, srcSlice, dst, dstLevel, dstSlice)
static void GX2CopySurface(CPUState* c) {
    u32 src = arg(c, 0), dst = arg(c, 3);
    u32 sl = arg(c, 1), dl = arg(c, 4);
    gx2_emit(PM4_TYPE3(IT_HLE_COPY_SURFACE, 18));
    emit_copy_side(src, surface_level_address(src, sl), sl, arg(c, 2));
    emit_copy_side(dst, surface_level_address(dst, dl), dl, arg(c, 5));
    // Done before returning: games copy a texture into a temporary buffer
    // and straight away copy the bytes back with the CPU (the lighting data
    // is set up that way). On the console the GPU has finished by then; here
    // the GPU thread could still be behind, and the CPU copied zeros.
    u64 ts = gx2_flush();
    gx2_wait_retired(ts, 2000);
}
static void GX2ResolveAAColorBuffer(CPUState* c) {   // (src colorBuffer, dst surface, level, slice)
    u32 src = arg(c, 0), dst = arg(c, 1), dl = arg(c, 2);
    gx2_emit(PM4_TYPE3(IT_HLE_COPY_SURFACE, 18));
    emit_copy_side(src, wiiu_read32(src + 0x24), 0, wiiu_read32(src + 0x78));
    emit_copy_side(dst, surface_level_address(dst, dl), dl, arg(c, 3));
}

// (depthBuffer, dst surface, level, slice): the depth buffer as a texture
// the shaders can read -- shadows, ambient occlusion, soft particles, fog.
// (It wasn't done at all: those effects read whatever was in memory.)
static void GX2ConvertDepthBufferToTextureSurface(CPUState* c) {
    u32 src = arg(c, 0), dst = arg(c, 1), level = arg(c, 2), slice = arg(c, 3);
    u32 sl = wiiu_read32(src + 0x74);
    // every slice of the depth buffer's view (Splatoon's shadow map has two:
    // the near and the far shadows -- copying only the first left the
    // shadows close to the camera, under the player, missing)
    u32 first = wiiu_read32(src + 0x78), n = wiiu_read32(src + 0x7C);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    for (u32 i = 0; i < n; i++) {
        gx2_emit(PM4_TYPE3(IT_HLE_COPY_SURFACE, 18));
        emit_copy_side(src, surface_level_address(src, sl), sl, first + i);
        emit_copy_side(dst, surface_level_address(dst, level), level, slice + i);
    }
}

static const HleEntry g_gx2_table[] = {
    { "GX2ConvertDepthBufferToTextureSurface", GX2ConvertDepthBufferToTextureSurface },
    { "GX2CalcSurfaceSizeAndAlignment", GX2CalcSurfaceSizeAndAlignment },
    { "GX2CalcTVSize", GX2CalcTVSize },
    { "GX2CalcDRCSize", GX2CalcDRCSize },
    { "GX2CalcDepthBufferHiZInfo", aux_info },
    { "GX2CalcColorBufferAuxInfo", aux_info },
    { "GX2SetSurfaceSwizzle", GX2SetSurfaceSwizzle },
    { "GX2GetSurfaceSwizzle", GX2GetSurfaceSwizzle },
    { "GX2GetSurfaceFormatBits", GX2GetSurfaceFormatBits },
    { "GX2SurfaceIsCompressed", GX2SurfaceIsCompressed },
    { "GX2CheckSurfaceUseVsFormat", one },
    { "GX2TempGetGPUVersion", gpu_version },   // 2 = the Wii U's GPU (shader files are checked against it)

    { "GX2InitTextureRegs", GX2InitTextureRegs },
    { "GX2SetPixelTexture", GX2SetPixelTexture },
    { "GX2SetVertexTexture", GX2SetVertexTexture },
    { "GX2SetGeometryTexture", GX2SetGeometryTexture },
    { "GX2SetComputeTexture", GX2SetVertexTexture },
    { "GX2InitSampler", GX2InitSampler },
    { "GX2InitSamplerXYFilter", GX2InitSamplerXYFilter },
    { "GX2InitSamplerZMFilter", GX2InitSamplerZMFilter },
    { "GX2InitSamplerLOD", GX2InitSamplerLOD },
    { "GX2InitSamplerClamping", GX2InitSamplerClamping },
    { "GX2InitSamplerBorderType", GX2InitSamplerBorderType },
    { "GX2InitSamplerDepthCompare", GX2InitSamplerDepthCompare },
    { "GX2SetPixelSampler", GX2SetPixelSampler },
    { "GX2SetVertexSampler", GX2SetVertexSampler },
    { "GX2SetGeometrySampler", GX2SetGeometrySampler },
    { "GX2SetComputeSampler", GX2SetVertexSampler },
    { "GX2SetPixelSamplerBorderColor", GX2SetPixelSamplerBorderColor },
    { "GX2SetVertexSamplerBorderColor", GX2SetVertexSamplerBorderColor },
    { "GX2SetGeometrySamplerBorderColor", GX2SetGeometrySamplerBorderColor },

    { "GX2InitColorBufferRegs", GX2InitColorBufferRegs },
    { "GX2InitDepthBufferRegs", GX2InitDepthBufferRegs },
    { "GX2InitDepthBufferHiZEnable", nothing },
    { "GX2SetColorBuffer", GX2SetColorBuffer },
    { "GX2SetDepthBuffer", GX2SetDepthBuffer },
    { "GX2ClearColor", GX2ClearColor },
    { "GX2ClearBuffersEx", GX2ClearBuffersEx },
    { "GX2ClearDepthStencilEx", GX2ClearDepthStencilEx },
    { "GX2SetClearDepthStencil", GX2SetClearDepthStencil },
    { "GX2SetClearDepth", GX2SetClearDepth },
    { "GX2SetClearStencil", GX2SetClearStencil },

    { "GX2InitAlphaTestReg", GX2InitAlphaTestReg },
    { "GX2SetAlphaTestReg", GX2SetAlphaTestReg },
    { "GX2SetAlphaTest", GX2SetAlphaTest },
    { "GX2SetAlphaToMask", nothing },
    { "GX2SetStreamOutBuffer", GX2SetStreamOutBuffer },
    { "GX2SetStreamOutEnable", GX2SetStreamOutEnable },
    { "GX2SetStreamOutContext", GX2SetStreamOutContext },
    { "GX2SaveStreamOutContext", GX2SaveStreamOutContext },       // (alpha to coverage: no multisampling here, nothing to do)
    { "GX2ExpandDepthBuffer", nothing },    // (no compressed depth here)
    { "GX2InitColorControlReg", GX2InitColorControlReg },
    { "GX2SetColorControlReg", GX2SetColorControlReg },
    { "GX2SetColorControl", GX2SetColorControl },
    { "GX2InitPolygonControlReg", GX2InitPolygonControlReg },
    { "GX2SetPolygonControlReg", GX2SetPolygonControlReg },
    { "GX2SetPolygonControl", GX2SetPolygonControl },
    { "GX2SetCullOnlyControl", GX2SetCullOnlyControl },
    { "GX2InitPolygonOffsetReg", GX2InitPolygonOffsetReg },
    { "GX2SetPolygonOffsetReg", GX2SetPolygonOffsetReg },
    { "GX2SetPolygonOffset", GX2SetPolygonOffset },
    { "GX2SetRasterizerClipControl", GX2SetRasterizerClipControl },
    { "GX2SetRasterizerClipControlEx", GX2SetRasterizerClipControlEx },
    { "GX2SetRasterizerClipControlHalfZ", GX2SetRasterizerClipControlEx },
    { "GX2InitViewportReg", GX2InitViewportReg },
    { "GX2SetViewportReg", GX2SetViewportReg },
    { "GX2SetViewport", GX2SetViewport },
    { "GX2InitScissorReg", GX2InitScissorReg },
    { "GX2SetScissorReg", GX2SetScissorReg },
    { "GX2SetScissor", GX2SetScissor },
    { "GX2GetScissorReg", GX2GetScissorReg },
    { "GX2SetDepthStencilControl", GX2SetDepthStencilControl },
    { "GX2InitDepthStencilControlReg", GX2InitDepthStencilControlReg },
    { "GX2SetDepthStencilControlReg", GX2SetDepthStencilControlReg },
    { "GX2SetDepthOnlyControl", GX2SetDepthOnlyControl },
    { "GX2SetStencilMask", GX2SetStencilMask },
    { "GX2InitStencilMaskReg", GX2InitStencilMaskReg },
    { "GX2SetStencilMaskReg", GX2SetStencilMaskReg },
    { "GX2SetPrimitiveRestartIndex", GX2SetPrimitiveRestartIndex },
    { "GX2SetTargetChannelMasks", GX2SetTargetChannelMasks },
    { "GX2InitTargetChannelMasksReg", GX2InitTargetChannelMasksReg },
    { "GX2SetTargetChannelMasksReg", GX2SetTargetChannelMasksReg },
    { "GX2SetBlendControl", GX2SetBlendControl },
    { "GX2InitBlendControlReg", GX2InitBlendControlReg },
    { "GX2SetBlendControlReg", GX2SetBlendControlReg },
    { "GX2SetBlendConstantColor", GX2SetBlendConstantColor },
    { "GX2InitBlendConstantColorReg", GX2InitBlendConstantColorReg },
    { "GX2SetBlendConstantColorReg", GX2SetBlendConstantColorReg },
    { "GX2SetPointSize", GX2SetPointSize },
    { "GX2SetPointLimits", GX2SetPointLimits },
    { "GX2InitPointSizeReg", GX2InitPointSizeReg },
    { "GX2SetPointSizeReg", GX2SetPointSizeReg },
    { "GX2InitPointLimitsReg", GX2InitPointLimitsReg },
    { "GX2SetPointLimitsReg", GX2SetPointLimitsReg },
    { "GX2SetSpecialState", GX2SetSpecialState },
    { "GX2InitHiStencilInfoRegs", nothing },

    { "GX2CalcFetchShaderSizeEx", GX2CalcFetchShaderSizeEx },
    { "GX2InitFetchShaderEx", GX2InitFetchShaderEx },
    { "GX2SetFetchShader", GX2SetFetchShader },
    { "GX2SetVertexShader", GX2SetVertexShader },
    { "GX2SetPixelShader", GX2SetPixelShader },
    { "GX2SetGeometryShader", GX2SetGeometryShader },
    { "GX2GetVertexShaderGPRs", GX2GetVertexShaderGPRs },
    { "GX2GetVertexShaderStackEntries", GX2GetVertexShaderStackEntries },
    { "GX2GetPixelShaderGPRs", GX2GetPixelShaderGPRs },
    { "GX2GetPixelShaderStackEntries", GX2GetPixelShaderStackEntries },
    { "GX2GetGeometryShaderGPRs", GX2GetGeometryShaderGPRs },
    { "GX2GetGeometryShaderStackEntries", GX2GetGeometryShaderStackEntries },
    { "GX2SetShaderModeEx", GX2SetShaderModeEx },
    { "GX2SetVertexUniformReg", GX2SetVertexUniformReg },
    { "GX2SetPixelUniformReg", GX2SetPixelUniformReg },
    { "GX2SetVertexUniformBlock", GX2SetVertexUniformBlock },
    { "GX2SetPixelUniformBlock", GX2SetPixelUniformBlock },
    { "GX2SetGeometryUniformBlock", GX2SetGeometryUniformBlock },
    { "GX2SetAttribBuffer", GX2SetAttribBuffer },
    { "GX2CalcGeometryShaderInputRingBufferSize", GX2CalcGeometryShaderRingBufferSize },
    { "GX2CalcGeometryShaderOutputRingBufferSize", GX2CalcGeometryShaderRingBufferSize },
    { "GX2SetGeometryShaderInputRingBuffer", GX2SetGeometryShaderInputRingBuffer },
    { "GX2SetGeometryShaderOutputRingBuffer", GX2SetGeometryShaderOutputRingBuffer },

    { "GX2DrawEx", GX2DrawEx },
    { "GX2DrawEx2", GX2DrawEx2 },
    { "GX2DrawIndexedEx", GX2DrawIndexedEx },
    { "GX2DrawIndexedEx2", GX2DrawIndexedEx2 },
    { "GX2DrawIndexedImmediateEx", GX2DrawIndexedImmediateEx },

    { "GX2BeginDisplayList", GX2BeginDisplayList },
    { "GX2BeginDisplayListEx", GX2BeginDisplayList },
    { "GX2EndDisplayList", GX2EndDisplayList },
    { "GX2GetCurrentDisplayList", GX2GetCurrentDisplayList },
    { "GX2GetDisplayListWriteStatus", GX2GetDisplayListWriteStatus },
    { "GX2CallDisplayList", GX2CallDisplayList },
    { "GX2DirectCallDisplayList", GX2DirectCallDisplayList },
    { "GX2CopyDisplayList", GX2CopyDisplayList },
    { "GX2PatchDisplayList", GX2PatchDisplayList },

    { "GX2SetupContextStateEx", GX2SetupContextStateEx },
    { "GX2SetContextState", GX2SetContextState },
    { "GX2GetContextStateDisplayList", GX2GetContextStateDisplayList },
    { "GX2SetDefaultState", GX2SetDefaultState },

    { "GX2Init", GX2Init },
    { "GX2Shutdown", nothing },
    { "GX2Flush", GX2Flush },
    { "GX2DrawDone", GX2DrawDone },
    { "GX2GetLastSubmittedTimeStamp", GX2GetLastSubmittedTimeStamp },
    { "GX2GetRetiredTimeStamp", GX2GetRetiredTimeStamp },
    { "GX2WaitTimeStamp", GX2WaitTimeStamp },
    { "GX2Invalidate", GX2Invalidate },
    { "GX2SetTVBuffer", GX2SetTVBuffer },
    { "GX2SetDRCBuffer", GX2SetDRCBuffer },
    { "GX2SetTVScale", nothing },
    { "GX2SetDRCScale", nothing },
    { "GX2SetTVEnable", nothing },
    { "GX2SetDRCEnable", nothing },
    { "GX2SetTVGamma", nothing },
    { "GX2SetDRCGamma", nothing },
    { "GX2CopyColorBufferToScanBuffer", GX2CopyColorBufferToScanBuffer },
    { "GX2SwapScanBuffers", GX2SwapScanBuffers },
    { "GX2WaitForVsync", GX2WaitForVsync },
    { "GX2WaitForFlip", GX2WaitForFlip },
    { "GX2WaitForFreeScanBuffer", nothing },
    { "GX2GetSwapStatus", GX2GetSwapStatus },
    { "GX2SetSwapInterval", GX2SetSwapInterval },
    { "GX2GetSwapInterval", GX2GetSwapInterval },
    { "GX2GPUTimeToCPUTime", GX2GPUTimeToCPUTime },
    { "GX2SampleTopGPUCycle", GX2SampleTopGPUCycle },
    { "GX2SampleBottomGPUCycle", GX2SampleBottomGPUCycle },
    { "GX2SubmitUserTimeStamp", GX2SubmitUserTimeStamp },
    { "GX2QueryGetOcclusionResult", GX2QueryGetOcclusionResult },
    { "GX2QueryBegin", GX2QueryBegin },
    { "GX2QueryEnd", GX2QueryEnd },
    { "GX2QueryBeginConditionalRender", GX2QueryBeginConditionalRender },
    { "GX2QueryEndConditionalRender", GX2QueryEndConditionalRender },
    { "GX2AllocateTilingApertureEx", GX2AllocateTilingApertureEx },
    { "GX2FreeTilingAperture", nothing },
    { "GX2GetSystemTVScanMode", GX2GetSystemTVScanMode },
    { "GX2GetSystemTVAspectRatio", GX2GetSystemTVAspectRatio },
    { "GX2GetSystemDRCMode", one },
    { "GX2IsVideoOutReady", one },
    { "GX2GetMainCoreId", GX2GetMainCoreId },
    { "GX2GetGPUTimeout", GX2GetGPUTimeout },
    { "GX2SetGPUTimeout", nothing },
    { "GX2GetLastFrame", GX2GetLastFrame },
    { "GX2GetLastFrameGammaA", GX2GetLastFrameGamma },
    { "GX2GetLastFrameGammaB", GX2GetLastFrameGamma },
    { "GX2SetEventCallback", nothing },
    { "GX2GetEventCallback", zero },
    { "GX2SetDRCConnectCallback", zero },
    { "GX2CopySurface", GX2CopySurface },
    { "GX2ResolveAAColorBuffer", GX2ResolveAAColorBuffer },
};

const HleEntry* gx2_hle_table(size_t* count) {
    *count = sizeof(g_gx2_table) / sizeof(g_gx2_table[0]);
    return g_gx2_table;
}

// The GX2 side for the freeze watchdog: frames swapped, timestamps, queries.
void gx2_report_state(FILE* out) {
    fprintf(out, "  gx2: swaps %u, flips %u, swap interval %u, vsync waits %u, flip waits %u (this second); timestamps: submitted %llu, retired %llu\n",
            g_swaps, g_flips, g_swap_interval, g_vsync_calls, g_flip_calls, (unsigned long long)gx2_last_submitted(), (unsigned long long)gx2_retired());
    fprintf(out, "  gx2: occlusion queries begun %u/%u/%u/%u (by type); results asked %u: ready %u, newest finished %u, after a short wait %u, not ready %u, lost %u\n",
            g_qstat_begin[0], g_qstat_begin[1], g_qstat_begin[2], g_qstat_begin[3], g_qstat_get, g_qstat_ready, g_qstat_cached, g_qstat_wait, g_qstat_none, g_qstat_lost);
}
