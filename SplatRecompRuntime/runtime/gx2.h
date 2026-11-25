// GX2, the Wii U's graphics library. For now: the parts games use to
// measure and set up graphics memory, and frame timing. Nothing is drawn
// yet; drawing (Vulkan) comes later.
#ifndef GX2_H
#define GX2_H

#include "hle.h"

const HleEntry* gx2_hle_table(size_t* count);

u32 gx2_frames_presented(void);
#include <stdio.h>
void gx2_report_state(FILE* out);   // (the freeze watchdog)
void gx2_note_vblank(void);          // the GPU thread: the last presented frame is on the screen (low input lag)

#endif
