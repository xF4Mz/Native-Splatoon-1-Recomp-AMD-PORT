// Frame capture: saves everything the renderer used for one frame (GPU
// registers, the game memory it read, and the contents of the buffers the
// GPU had already drawn) into one compressed file. tools/frame_replay.c
// draws that frame again without the game, so rendering problems can be
// studied and fixed one draw at a time.
//
// The file holds only data the game itself handed to the GPU on the
// player's PC; it isn't meant to be shared publicly.
#ifndef LATTE_CAPTURE_H
#define LATTE_CAPTURE_H

#include "cpu/cpu.h"
#include <stdbool.h>

// record tags
#define CAP_MAGIC 0x50414353u   // "SCAP"
#define CAP_REGS  1u   // all registers (0x10000 words)
#define CAP_RDIF  2u   // changed registers: (index, value) pairs
#define CAP_MEM   3u   // addr, size, bytes
#define CAP_SURF  4u   // addr, gx2fmt, w, h, depth, vkformat, bytes
#define CAP_EVENT 5u   // type, words...
#define CAP_END   6u

// event types
enum { CAPE_DRAW = 1, CAPE_CLEAR, CAPE_COPY_TO_SCAN, CAPE_COPY_SURFACE, CAPE_RT_INFO, CAPE_INVALIDATE, CAPE_SWAP, CAPE_QUERY };

extern volatile bool g_cap_active;

bool cap_open(const char* path);
void cap_close(void);
void cap_mem(u32 addr, u32 size);
void cap_regs_diff(void);          // writes registers that changed since the last call
void cap_event(u32 type, const u32* words, u32 n);
void cap_surface(u32 addr, u32 gx2fmt, u32 w, u32 h, u32 flags, u32 vkformat, const void* bytes, u32 size);   // flags: 1 depth, 2 3D

#endif
