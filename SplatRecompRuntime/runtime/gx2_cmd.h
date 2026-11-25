// Where GX2's GPU commands go: the main command stream (read by the GPU
// thread, latte_cp.c) or a display list the game is recording.
#ifndef GX2_CMD_H
#define GX2_CMD_H

#include "cpu/cpu.h"
#include "latte.h"
#include <stdbool.h>

void gx2_cmd_init(void);

// Append command words to the current target (display list or stream).
void gx2_emit(u32 w);
void gx2_emit_n(const u32* w, u32 n);

// Make everything written so far visible to the GPU thread.
void gx2_commit(void);
// Commit and add a fence; returns its number ("timestamp").
u64 gx2_flush(void);
u64 gx2_last_submitted(void);
u64 gx2_flush_pos(void);   // ring position of the game's last flush (commands before it: their data is final)
u64 gx2_read_pos(void);
void gx2_ring_read(u32 offset, u32 n, u32* out);   // (the GPU thread: n words from offset, at once)    // the GPU thread's position (just past the packet it's working on)
u64 gx2_retired(void);
// Wait until the GPU thread has passed fence `ts` (0 timeout = forever).
bool gx2_wait_retired(u64 ts, u32 timeout_ms);
void gx2_cmd_fence_times(u64 ts, u64* emit, u64* reach, u64* retire, u64* ahead);   // (a slow DrawDone: when the GPU thread reached / retired the fence, ring words queued ahead of it)
bool gx2_cmd_gpu_sleeping(void);

// Display lists.
void gx2_dl_begin(u32 addr, u32 size_bytes);
u32 gx2_dl_end(void);
bool gx2_dl_current(u32* addr, u32* size_bytes);
bool gx2_dl_active(void);

// Packet helpers.
// (each packet written at once: word by word it was three calls and three display-list lookups)
static inline void gx2_set_regs(u32 op, u32 base, u32 reg, const u32* values, u32 n) {
    if (n <= 32) {
        u32 w[2 + 32];
        w[0] = PM4_TYPE3(op, 1 + n);
        w[1] = reg - base;
        for (u32 i = 0; i < n; i++) w[2 + i] = values[i];
        gx2_emit_n(w, 2 + n);
        return;
    }
    gx2_emit(PM4_TYPE3(op, 1 + n));
    gx2_emit(reg - base);
    gx2_emit_n(values, n);
}
static inline void gx2_set_context_reg(u32 reg, u32 v) {
    u32 w[3] = { PM4_TYPE3(IT_SET_CONTEXT_REG, 2), reg - REG_BASE_CONTEXT, v };
    gx2_emit_n(w, 3);
}
static inline void gx2_set_config_reg(u32 reg, u32 v) {
    u32 w[3] = { PM4_TYPE3(IT_SET_CONFIG_REG, 2), reg - REG_BASE_CONFIG, v };
    gx2_emit_n(w, 3);
}

// GPU thread side: read the committed stream.
u32 gx2_ring_available(void);
u32 gx2_ring_peek(u32 offset);
void gx2_ring_consume(u32 words);
// Nothing to read: sleep until the next flush (at most max_ms).
void gx2_ring_wait(u32 max_ms);
void gx2_cmd_set_retired(u64 ts);
// (for the [sync] log line) the GPU thread reached fence `ts` in the stream
void gx2_cmd_note_fence_reached(u64 ts);
// Fence timing since the last call (and reset): count, sum/max ns from the
// game's flush to the GPU thread reaching it, sum/max ns from there to the retire.
typedef struct {
    u32 n; u64 reach_sum, reach_max, retire_sum, retire_max; u32 deferred;
    u32 wn; u64 wreach_sum, wretire_sum;   // the fences the game waited for (GX2DrawDone) only
} GX2FenceStats;
void gx2_cmd_fence_stats(GX2FenceStats* out);
void gx2_cmd_note_wait(u64 ts);   // the game is about to wait for fence ts
// The game waited for the graphics card's results (a fence held for stream-out
// copies) in the last 3 s: worth handing work to the card early.
bool gx2_cmd_wait_pressure(void);

#endif
