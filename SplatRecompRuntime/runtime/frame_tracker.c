// Frame tracker / stutter analyzer: for every game frame, where the time went
// (the game's main thread working or waiting, the drawing thread and its
// shader/pipeline builds, texture work, uploads and recording, the graphics
// card itself) and, for every frame the screen showed late, why. A frame is a
// stutter when it reached the screen more refreshes after the last one than a
// 60 Hz frame spans (a missed refresh: latte_render.c present_pace tells), or
// took over 25 ms (that always misses). Its cause is the part over budget:
// shader/pipeline builds or texture work on the drawing thread, the graphics
// card, the game's own logic (main thread busy over 15 ms), the drawing
// thread, the main thread waiting on something, else pacing / system.
//   [stutter] frame N shown a refresh late: <cause> | main thread ... | drawing thread ... | card ...
//   [stutter] last 600 frames: 3 stutters (...) ... (every ~10 s)
// On screen (F6, settings row "Frame tracker", SPLAT_TRACKER=1): the last 300
// frames as bars coloured by cause (latte_render.c draw_tracker). Always
// counting (a few adds per frame); the graph only when shown.
#include "common/types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "frame_tracker.h"

TrackFrame g_tr[TR_N];
volatile u32 g_tr_head;            // frames written so far (the newest at (head - 1) % TR_N)
static volatile int g_tr_cadence[8];   // refreshes between shown frames, last 10 s (1..6, 7 = more)
static volatile int g_tr_paced[8];     // refreshes between the game's targets (gx2.c), last 10 s
static u32 g_late, g_by[TR_CAUSES];    // stutters this 10 s
TrackSummary g_tr_sum;

const char* const k_tr_cause[TR_CAUSES] = { "on time", "shader/pipeline builds", "texture work", "graphics card", "game logic (main thread)",
                                           "drawing thread", "waiting", "pacing / system" };

// The drawing thread's last frames (written by it at each swap; read by the
// game thread when it classifies a frame: torn reads only blur a number).
static TrackDraw g_td[4];
static volatile u32 g_td_n;
void tracker_draw_frame(const TrackDraw* d) {
    g_td[g_td_n & 3] = *d;
    g_td_n++;
}

// One stutter: counted, coloured in the graph, logged (at most 4 a second).
static void stutter(TrackFrame* f, u32 swap, const char* how) {
    if (f->cause != TR_OK) return;   // (already counted)
    f->cause = f->candidate;
    g_late++;
    g_by[f->cause]++;
    static int quiet = -1;   // (SPLAT_STUTTER_LOG=0: only the summaries)
    if (quiet < 0) { const char* e = getenv("SPLAT_STUTTER_LOG"); quiet = e && atoi(e) == 0; }
    static u32 sec, in_sec, logged;
    if (swap / 60 != sec) { sec = swap / 60; in_sec = 0; }
    if (quiet || in_sec >= 4 || logged >= 3000) return;
    in_sec++;
    logged++;
    char why[320] = "";
    switch (f->cause) {
    case TR_BUILD: snprintf(why, sizeof(why), " (%u built, %.1f ms on the drawing thread)", f->builds, f->build_ms); break;
    case TR_TEX: snprintf(why, sizeof(why), " (%.1f ms: hashing %.1f, images made %.1f, decoding %.1f, uploads %.1f; buffers to draw into %.1f; shaders made ready %.1f = cached modules made here %.1f, waited for a helper's %.1f, new shaders compiled here %.1f, waited for a helper's %.1f, brief build waits %.1f)",
                          f->tex_ms, f->tex_hash_ms, f->tex_create_ms, f->tex_decode_ms, f->tex_upload_ms, f->att_ms, f->rdy_ms, f->sw_ms[0], f->sw_ms[1], f->sw_ms[2], f->sw_ms[3], f->sw_ms[4]); break;
    case TR_GPU: snprintf(why, sizeof(why), " (card %.1f ms, drawing thread waited %.1f ms for it)", f->gpu_ms, f->card_wait_ms); break;
    case TR_GAME: snprintf(why, sizeof(why), " (%.1f ms of work)", f->main_busy_ms); break;
    case TR_WAIT: snprintf(why, sizeof(why), " (%s from %08X, %.1f ms)", f->wait_what ? f->wait_what : "-", f->wait_lr, f->wait_ms); break;
    default: break;
    }
    printf("[stutter] frame %u %s: %s%s | frame %.1f ms; main thread busy %.1f ms, waited %.1f ms (longest: %s from %08X, %.1f ms) | "
           "drawing thread %.1f ms busy (builds %.1f, textures %.1f, uploads %.1f, recording %.1f, waiting for the card %.1f) | card %.1f ms\n",
           swap, how, k_tr_cause[f->cause], why, f->ms, f->main_busy_ms, f->main_wait_ms, f->wait_what ? f->wait_what : "-", f->wait_lr, f->wait_ms,
           f->draw_ms, f->build_ms, f->tex_ms, f->upload_ms, f->record_ms, f->card_wait_ms, f->gpu_ms);
}

// A present: refreshes since the last shown frame, and how many a 60 Hz frame spans on this screen.
extern volatile int splat_vsyncs_per_swap;   // (gx2.c: 2 while the game runs a scene at 30)
void tracker_present(int refreshes, int per_frame) {
    per_frame *= splat_vsyncs_per_swap >= 1 && splat_vsyncs_per_swap <= 4 ? splat_vsyncs_per_swap : 1;
    int r = refreshes < 1 ? 1 : refreshes > 7 ? 7 : refreshes;
    g_tr_cadence[r]++;
    TrackFrame* f = &g_tr[(g_tr_head + TR_N - 1) % TR_N];   // (the newest game frame: the one being shown)
    f->refreshes = (u8)r;
    if (per_frame > 0 && refreshes > per_frame) {
        char how[48];
        snprintf(how, sizeof(how), "shown %d refresh%s late", refreshes - per_frame, refreshes - per_frame > 1 ? "es" : "");
        stutter(f, f->swap, how);
    }
}
void tracker_paced(int refreshes) {
    if (refreshes < 1) refreshes = 1;
    if (refreshes > 7) refreshes = 7;
    g_tr_paced[refreshes]++;
}

void tracker_game_frame(u32 swap, u64 us, u64 waited_ns, const char* wait_what, u32 wait_lr, u64 wait_ns) {
    // the drawing thread's recent frames: the worst of the last two (it runs a frame behind the game)
    TrackDraw d;
    memset(&d, 0, sizeof(d));
    u32 n = g_td_n;
    for (u32 k = 1; k <= 2 && k <= n; k++) {
        const TrackDraw* x = &g_td[(n - k) & 3];
        if (x->busy_ms > d.busy_ms) d.busy_ms = x->busy_ms;
        if (x->build_ms > d.build_ms) { d.build_ms = x->build_ms; d.builds = x->builds; }
        if (x->tex_ms > d.tex_ms) { d.tex_ms = x->tex_ms; d.tex_hash_ms = x->tex_hash_ms; d.tex_create_ms = x->tex_create_ms; d.tex_decode_ms = x->tex_decode_ms; d.tex_upload_ms = x->tex_upload_ms; d.att_ms = x->att_ms; d.rdy_ms = x->rdy_ms; memcpy(d.sw_ms, x->sw_ms, sizeof(d.sw_ms)); }
        if (x->upload_ms > d.upload_ms) d.upload_ms = x->upload_ms;
        if (x->record_ms > d.record_ms) d.record_ms = x->record_ms;
        if (x->card_wait_ms > d.card_wait_ms) d.card_wait_ms = x->card_wait_ms;
        if (x->gpu_ms > d.gpu_ms) d.gpu_ms = x->gpu_ms;
    }
    {   // (and what the drawing thread did during this very game frame: its per-swap report of that comes a frame later)
        TrackDraw l;
        latte_render_tracker_live(&l);
        if (l.busy_ms > d.busy_ms) d.busy_ms = l.busy_ms;
        if (l.build_ms > d.build_ms) d.build_ms = l.build_ms;
        if (l.tex_ms > d.tex_ms) { d.tex_ms = l.tex_ms; d.tex_hash_ms = l.tex_hash_ms; d.tex_create_ms = l.tex_create_ms; d.tex_decode_ms = l.tex_decode_ms; d.tex_upload_ms = l.tex_upload_ms; d.att_ms = l.att_ms; d.rdy_ms = l.rdy_ms; memcpy(d.sw_ms, l.sw_ms, sizeof(d.sw_ms)); }
        if (l.upload_ms > d.upload_ms) d.upload_ms = l.upload_ms;
        if (l.record_ms > d.record_ms) d.record_ms = l.record_ms;
        if (l.card_wait_ms > d.card_wait_ms) d.card_wait_ms = l.card_wait_ms;
    }
    float ms = (float)(us / 1000.0), waited = (float)(waited_ns / 1e6), busy = ms - waited;
    if (busy < 0) busy = 0;
    // (a scene at 30 fps: twice the time per frame is on time -- the limits below count in 60 Hz frames)
    float k = splat_vsyncs_per_swap >= 1 && splat_vsyncs_per_swap <= 4 ? (float)splat_vsyncs_per_swap : 1.0f;
    bool on_gpu_thread = wait_what && strstr(wait_what, "GPU thread");
    int cause = TR_PACE;   // (what would be blamed if this frame turns out late)
    if (d.build_ms >= 4.0f) cause = TR_BUILD;
    else if (d.tex_ms >= 4.0f) cause = TR_TEX;
    else if (d.gpu_ms >= 14.0f || d.card_wait_ms >= 4.0f) cause = TR_GPU;
    else if (busy >= 15.0f * k) cause = TR_GAME;
    else if (d.busy_ms >= 15.0f * k) cause = TR_DRAW;
    else if (wait_ns >= 5000000ull && !on_gpu_thread) cause = TR_WAIT;
    TrackFrame* f = &g_tr[g_tr_head % TR_N];
    memset(f, 0, sizeof(*f));
    f->swap = swap;
    f->ms = ms;
    f->main_busy_ms = busy;
    f->main_wait_ms = waited;
    f->wait_what = wait_what;
    f->wait_lr = wait_lr;
    f->wait_ms = (float)(wait_ns / 1e6);
    f->draw_ms = d.busy_ms;
    f->build_ms = d.build_ms;
    f->builds = d.builds;
    f->tex_ms = d.tex_ms;
    f->tex_hash_ms = d.tex_hash_ms; f->tex_create_ms = d.tex_create_ms; f->tex_decode_ms = d.tex_decode_ms; f->tex_upload_ms = d.tex_upload_ms; f->att_ms = d.att_ms;
    f->rdy_ms = d.rdy_ms; memcpy(f->sw_ms, d.sw_ms, sizeof(f->sw_ms));
    f->upload_ms = d.upload_ms;
    f->record_ms = d.record_ms;
    f->card_wait_ms = d.card_wait_ms;
    f->gpu_ms = d.gpu_ms;
    f->candidate = (u8)cause;
    f->cause = TR_OK;
    g_tr_head++;
    if (ms > 25.0f * k) stutter(f, swap, k > 1.0f ? "took over 1.5 frames" : "took over 25 ms");

    static u32 frames;
    static double sum_busy, sum_draw, sum_gpu, worst;
    frames++;
    sum_busy += busy; sum_draw += d.busy_ms; sum_gpu += d.gpu_ms;
    if (ms > worst) worst = ms;
    if (frames >= 600) {   // ~10 s
        TrackSummary s;
        memset(&s, 0, sizeof(s));
        s.frames = frames;
        s.late = g_late;
        memcpy(s.by, g_by, sizeof(s.by));
        s.cpu_ms = (float)(sum_busy / frames);
        s.draw_ms = (float)(sum_draw / frames);
        s.gpu_ms = (float)(sum_gpu / frames);
        s.worst_ms = (float)worst;
        for (int i = 0; i < 8; i++) s.cadence[i] = g_tr_cadence[i];
        g_tr_sum = s;
        char causes[256] = "";
        int cn = 0;
        for (int i = 1; i < TR_CAUSES; i++)
            if (g_by[i]) cn += snprintf(causes + cn, sizeof(causes) - cn, "%s%s %u", cn ? ", " : "", k_tr_cause[i], g_by[i]);
        printf("[stutter] last %u frames: %u stutter%s%s%s%s, slowest frame %.1f ms | per frame: main thread busy %.1f ms, drawing thread %.1f ms, card %.1f ms | "
               "shown 1/2/3/4/5+ refreshes after the previous: %d/%d/%d/%d/%d (paced for: %d/%d/%d/%d/%d)\n",
               frames, g_late, g_late == 1 ? "" : "s", g_late ? " (" : "", causes, g_late ? ")" : "", worst, s.cpu_ms, s.draw_ms, s.gpu_ms,
               s.cadence[1], s.cadence[2], s.cadence[3], s.cadence[4], s.cadence[5] + s.cadence[6] + s.cadence[7],
               g_tr_paced[1], g_tr_paced[2], g_tr_paced[3], g_tr_paced[4], g_tr_paced[5] + g_tr_paced[6] + g_tr_paced[7]);
        for (int i = 0; i < 8; i++) { g_tr_cadence[i] = 0; g_tr_paced[i] = 0; }
        frames = g_late = 0;
        memset(g_by, 0, sizeof(g_by));
        sum_busy = sum_draw = sum_gpu = worst = 0;
    }
}
