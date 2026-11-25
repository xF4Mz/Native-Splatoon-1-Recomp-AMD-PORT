// Turns the GPU's work into Vulkan work on the PC's graphics card. Called
// only from the GPU thread (latte_cp.c). Everything here is safe to call
// before the graphics card is ready (or with no window): the work is then
// skipped and the game keeps running.
#ifndef LATTE_RENDER_H
#define LATTE_RENDER_H

#include "cpu/cpu.h"
#include <stdbool.h>

typedef struct {
    u32 prim;          // VGT_PRIMITIVE_TYPE
    u32 count;         // vertices / indices
    u32 instances;
    u32 index_type;    // LATTE_INDEX_* (indexed draws)
    u32 index_addr;    // guest address of the indices, 0 if not indexed
    const u32* immediate;   // DRAW_INDEX_IMMD: index words as read big-endian
    u32 immediate_words;
} LatteDraw;

// GPU thread start / idle moments.
void latte_render_thread_start(void);
// Nothing to read: finish what the graphics card has done (waits up to 1 ms
// on its fence when a copy or a fence the game may be waiting for is queued).
// Returns true while such work is still queued (the caller shouldn't sleep then).
bool latte_render_idle(void);
void latte_render_poll(void);

// Packet handlers (word layouts: see gx2.c).
void latte_render_rt_info(const u32* d);
// The look-ahead (latte_cp_lookahead): one coming draw's shaders and pipeline to the helper threads; the
// buffer table saved before the pass and put back after it.
void latte_render_prefetch_draw(const LatteDraw* draw);
void latte_render_rti_save(void);
void latte_render_rti_restore(void);
void latte_render_clear(const u32* d);
void latte_render_draw(const LatteDraw* draw);
void latte_render_copy_to_scan(const u32* d);
// Before the game hears the GPU is done: copy small drawn buffers back to game memory.
void latte_render_sync_readbacks(void);
// Readback counters since the last call (and reset), for the [sync] log line.
typedef struct {
    u32 so_draws, so_bytes, so_waits; u64 so_wait_ns;       // stream-out draws, bytes copied back, waits for the card (so_sync / defer_poll)
    u32 so_draw_reads;                                       // draws that read memory a stream-out was still writing
    u32 rb_batches, rb_surfs, rb_bytes; u64 rb_encode_ns;    // buffers copied back to game memory
    u32 fences_deferred;                                     // fences retired only once stream-out copies were done
    u32 submits;                                             // vkQueueSubmit calls
    // the TV picture (the "frozen 3D scene" watch): draws into TV-sized buffers vs the rest, draws skipped and why,
    // copies into the TV scan buffer and where from, texture-view memo hits, depth-copy reuses
    u32 scene_draws, other_draws, skip_shader, skip_pipe, scan_copies, memo_hits, fb_reuse;
    u32 scan_src; u64 scan_src_seen;
} LatteSyncStats;
void latte_render_sync_stats(LatteSyncStats* out);
// A fence / memory write from the command stream. Stream-out results (ink)
// are copied back once the graphics card has finished them, without the
// drawing thread waiting; fence and write happen after that, in order
// (retire is called with the fence's timestamp then).
void latte_render_fence(u64 ts, void (*retire)(u64 ts));
void latte_render_mem_write(const u32* d, u32 n);
void latte_render_note_idle(u64 ns);   // GPU thread waited this long for the game
// Occlusion query: op 1 begin, 2 end; results go into the query memory at sync points.
void latte_render_query(u32 op, u32 addr);
// newest finished count of the query at addr (any thread); false if none yet
bool latte_render_query_last(u32 addr, u32* count);
void latte_render_copy_surface(const u32* d, u32 n);   // n: 12 (old captures) or 18 words
void latte_render_swap(u32 swap_count);
void latte_render_invalidate(u32 addr, u32 size);

// Frame capture / replay (latte_capture.h, tools/frame_replay.c).
bool latte_render_capture_wanted(void);
bool latte_render_capture_more(void);
void latte_render_perf_print(void);   // (frame_replay: where the time went)   // one more frame right after the last capture?
void latte_render_begin_capture(void);
// A draw worth saving from the middle of a frame (the first time the game
// builds a compressed cube map on the GPU): start a capture right now.
bool latte_render_early_capture(void);
bool latte_render_probe(u32 addr, u32 x, u32 y, float out[4]);   // (replay debugging)
void latte_render_restore_surface(u32 addr, u32 gx2fmt, u32 w, u32 h, u32 flags, u32 vkformat, const void* bytes, u32 size);
void latte_render_set_debug(bool on);
void latte_render_dump_all(const char* prefix);
// (frame_replay) the shown picture -> PNG; count the coming draws for a pick at a TV pixel
void latte_render_save_shown(const char* path);
void latte_render_pick_now(int x, int y);

#endif
