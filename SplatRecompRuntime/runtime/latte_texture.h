// Reading GPU surfaces (textures) out of the game's memory for upload to the
// PC's graphics card: undoing the Wii U GPU's tiled layouts and converting
// the few pixel formats PC cards don't have.
#ifndef LATTE_TEXTURE_H
#define LATTE_TEXTURE_H

#include "cpu/cpu.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u32 addr;          // image address (swizzle bits removed)
    u32 width, height; // pixels
    u32 pitch;         // pixels per row as the GPU sees it
    u32 hwfmt;         // hardware format (low 6 bits of the GX2 format)
    u32 tile_mode;
    u32 swizzle;       // pipe/bank swizzle in bits 8-10
    u32 slices;        // array layers / cube faces / 3D depth (0 = 1)
    u32 rows;          // rows of elements per slice in memory (0: from the height)
} LatteTexDesc;

typedef struct {
    int vk_format;     // VkFormat
    u32 elem_bytes;    // bytes per element in the uploaded data
    u32 src_bits;      // bits per element in the game's data
    bool block;        // 4x4 compressed blocks
    u8 convert;        // 0 copy, else a CONV_* conversion to RGBA8
} LatteTexFormat;

// Which Vulkan format to use for a GX2 surface format (with its flag bits).
bool latte_tex_format(u32 gx2_format, LatteTexFormat* out);
bool latte_depth_format(u32 gx2_format, int* vk_format, bool* has_stencil);

// Decode level 0, every slice, into `dst` (tightly packed rows of elements
// in the upload format, one slice after another). Returns the bytes
// written, 0 on failure.
size_t latte_tex_decode(const LatteTexDesc* d, const LatteTexFormat* f, u8* dst, size_t cap);

// Fills the decoder's shared lookup tables (made on first use otherwise):
// called once before decodes run on several threads at a time.
void latte_tex_prepare(void);

// Only slices first..first+count-1 (packed from the start of `dst`).
size_t latte_tex_decode_slices(const LatteTexDesc* d, const LatteTexFormat* f, u32 first, u32 count, u8* dst, size_t cap);

// Packed elements -> the game's layout in memory (formats that need no
// conversion only). Returns false if it couldn't.
bool latte_tex_encode_slices(const LatteTexDesc* d, const LatteTexFormat* f, u32 first, u32 count, const u8* src, size_t size);

// Size of the data a texture covers in game memory (level 0, all slices).
u32 latte_tex_span(const LatteTexDesc* d, const LatteTexFormat* f);

#endif
