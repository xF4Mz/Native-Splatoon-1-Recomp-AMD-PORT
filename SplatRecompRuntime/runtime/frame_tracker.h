// Frame tracker / stutter analyzer (frame_tracker.c): per game frame, where the time went and,
// for late frames, why. The drawing thread reports its frames, the game's main thread its own;
// latte_render.c draws the graph (F6).
#ifndef FRAME_TRACKER_H
#define FRAME_TRACKER_H
#include "common/types.h"

enum { TR_OK, TR_BUILD, TR_TEX, TR_GPU, TR_GAME, TR_DRAW, TR_WAIT, TR_PACE, TR_CAUSES };
extern const char* const k_tr_cause[TR_CAUSES];

typedef struct {          // one drawing-thread frame (ms)
    float total_ms, busy_ms, build_ms, tex_ms, upload_ms, record_ms, card_wait_ms, gpu_ms;
    float tex_hash_ms, tex_create_ms, tex_decode_ms, tex_upload_ms, att_ms;   // (of tex_ms: hashing changed memory, images made, decoding, uploads; buffers drawn into)
    float rdy_ms, sw_ms[5];   // (of tex_ms: shaders made ready -- cached modules made here, waited for a helper's, new shaders compiled here, waited for a helper's, brief waits for builds)
    u32 builds;
} TrackDraw;
typedef struct {          // one game frame
    u32 swap, wait_lr, builds;
    float ms, main_busy_ms, main_wait_ms, wait_ms, draw_ms, build_ms, tex_ms, upload_ms, record_ms, card_wait_ms, gpu_ms;
    float tex_hash_ms, tex_create_ms, tex_decode_ms, tex_upload_ms, att_ms, rdy_ms, sw_ms[5];
    const char* wait_what;
    u8 cause;             // TR_OK, or why it was a stutter (shown late / over 25 ms)
    u8 candidate;         // what would be blamed if it turns out late
    u8 refreshes;         // refreshes after the previous shown frame
} TrackFrame;
typedef struct {
    u32 frames, late, by[TR_CAUSES];
    float cpu_ms, draw_ms, gpu_ms, worst_ms;
    int cadence[8];
} TrackSummary;

#define TR_N 512
extern TrackFrame g_tr[TR_N];
extern volatile u32 g_tr_head;
extern TrackSummary g_tr_sum;

void tracker_draw_frame(const TrackDraw* d);            // drawing thread, each swap
void tracker_present(int refreshes, int per_frame);     // drawing thread, each present (refreshes since the last; a 60 Hz frame's)
void tracker_paced(int refreshes);                      // gx2.c: refreshes between the game's targets
void tracker_game_frame(u32 swap, u64 us, u64 waited_ns, const char* wait_what, u32 wait_lr, u64 wait_ns);   // game main thread
void latte_render_tracker_live(TrackDraw* d);           // latte_render.c: the drawing thread's work since the last call (live)
#endif
