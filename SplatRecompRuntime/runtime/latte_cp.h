// The GPU thread ("command processor"): reads the command packets GX2 writes
// (gx2_cmd.c) and the display lists they call, keeps the GPU's register
// state, and hands draws, clears and frame presents to the renderer
// (latte_render.c). Like the console, the game never waits on drawing
// itself, only on fences ("timestamps") it asks for.
#ifndef LATTE_CP_H
#define LATTE_CP_H

#include "cpu/cpu.h"
#include "latte.h"
#include <stdbool.h>

void latte_cp_start(void);
// Reads the rest of the frame's commands ahead (registers only) and hands every coming draw's shaders and
// pipeline to the helper threads; returns how many draws it read (see latte_cp.c).
u32 latte_cp_lookahead(u32 max_draws);

// Frames (GX2SwapScanBuffers) the GPU thread has finished with.
u32 latte_cp_frames_done(void);

// The GPU's registers (0x10000 words). Only the GPU thread writes them; the
// renderer reads them while handling a draw.
extern u32 g_latte_regs[REG_COUNT];

static inline u32 latte_reg(u32 r) { return g_latte_regs[r]; }

#endif
