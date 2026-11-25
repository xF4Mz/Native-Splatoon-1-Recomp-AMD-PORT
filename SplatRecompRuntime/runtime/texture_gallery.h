// Texture gallery: until the real renderer exists, the window shows the
// textures the game hands to the GPU, decoded live from its memory.
// Game threads report textures; the window thread decodes and draws them.
#ifndef TEXTURE_GALLERY_H
#define TEXTURE_GALLERY_H

#include "common/types.h"
#include <stdbool.h>

// Game side (GX2SetPixelTexture etc.): note a GX2Texture.
void gallery_note_texture(u32 texture_addr);

// Window side: decode a few newly seen textures; returns how many are shown.
u32 gallery_update(void);
u32 gallery_count(void);

// Window side: draw the gallery (newest first) into a w x h image of 4-byte
// pixels, BGRA or RGBA order, on a background color.
void gallery_compose(u8* out, u32 w, u32 h, u32 stride, bool bgra, const u8 bg[3]);

#endif
