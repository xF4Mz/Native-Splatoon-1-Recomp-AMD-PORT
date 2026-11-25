// Reading the game's GPU textures out of guest memory: undoing the Wii U
// GPU's tiled memory layout and decoding the pixel formats to plain RGBA.
#ifndef GX2_TEXTURE_H
#define GX2_TEXTURE_H

#include "common/types.h"
#include <stdbool.h>

// The parts of a GX2Surface we need (it is the first 0x74 bytes of a
// GX2Texture, GX2ColorBuffer and GX2DepthBuffer).
typedef struct {
    u32 dim, width, height, depth, mip_levels, format, aa, use;
    u32 image_size, image, tile_mode, swizzle, pitch;
} Gx2Surface;

bool gx2_surface_read(u32 addr, Gx2Surface* s);

// Can we decode this surface's format and tiling?
bool gx2_texture_supported(const Gx2Surface* s);

// Mip level 0, slice 0, as width*height RGBA8 pixels (malloc'd; free it).
// NULL if unsupported.
u8* gx2_texture_to_rgba(const Gx2Surface* s);

#endif
