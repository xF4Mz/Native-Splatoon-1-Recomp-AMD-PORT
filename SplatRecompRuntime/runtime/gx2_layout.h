// Surface sizes and layouts (pitch, tiling, mip level offsets) the way the
// console's GX2 / AMD AddrLib compute them. Shared by GX2 (which tells the
// game) and the renderer (which reads textures and their mip levels).
#ifndef GX2_LAYOUT_H
#define GX2_LAYOUT_H

#include "cpu/cpu.h"
#include <stdbool.h>

typedef struct {
    u32 width, height, depth;   // pixels / slices at this level
    u32 pitch;                  // elements (4x4 blocks for compressed formats)
    u32 height_pad;             // rows per slice in memory, in elements
    u32 mode;                   // tile mode this level really uses
    bool swizzled;              // uses the surface's pipe/bank swizzle
} GX2LevelLayout;

typedef struct {
    u32 mode, swizzle, align, image_size, mip_size;
    u32 mip_offset[13];          // [0]: level 1 from the image; [n>0]: level n+1 from level 1
    u32 levels;
    GX2LevelLayout level[14];
} GX2Layout;

// Bits per element for a surface format (low 6 bits), and whether it's a
// 4x4 block-compressed format.
u32 gx2_format_bits(u32 format, bool* block);

void gx2_surface_layout(u32 dim, u32 width, u32 height, u32 depth, u32 levels, u32 format, u32 aa, u32 resflag,
                        u32 mode, u32 swizzle, GX2Layout* out);

// What the game's own GX2Texture says about its mip levels (set when the
// game binds a texture; the renderer prefers these offsets to computed ones).
// Keyed by the mip pointer as it appears in the texture registers.
typedef struct { u32 levels, swizzle, offs[13]; } GX2MipInfo;
void gx2_note_mips(u32 mip_reg_addr, const GX2MipInfo* m);
bool gx2_lookup_mips(u32 mip_reg_addr, GX2MipInfo* out);

#endif
