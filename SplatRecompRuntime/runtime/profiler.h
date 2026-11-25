// Sampling profiler: finds out where the game spends its time (see profiler.c).
#ifndef PROFILER_H
#define PROFILER_H

#include "rpx_loader.h"
#include <stdio.h>

void profiler_start(const RpxImage* image);
void profiler_report(FILE* out);   // printed when the game exits
void profiler_report_now(FILE* out);   // the same report now (a freeze); the profiler keeps running
void profiler_add_jumps(u32 n);       // the run loop counts jumps between game functions
void profiler_note_spin_yield(void);  // a busy-waiting thread gave its turn away
void profiler_register_gpu_thread(void);   // (called by the GPU command thread: sampled too)

#endif
