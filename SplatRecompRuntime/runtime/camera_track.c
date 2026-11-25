// The game's own camera and the player's position, read at two game hooks (nothing is changed in the game):
//  - the gameplay camera per frame (sead::LookAtCamera updates, native_funcs.c): the sky / volume passes, motion blur
//    and smooth motion take the camera of the frame being drawn; the free camera (freecam.c) starts from it; the
//    sun's direction comes from the game's light cameras; layout cameras' distances mark the HUD's draws;
//  - the player's position (the skeleton step nw::g3d SkeletonObj::CalcWorldMtx, game address 02D26D80; r3 = the
//    skeleton: +0 its FSKL, +8 bones, +44 bone count; r4 = the model's matrix): of the Inkling skeletons in a frame,
//    the one nearest the gameplay camera, in front of it. The free camera stays near it, the benchmark checks it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fast_cpu.h"
#include "wiiu_mem.h"
#include "vk_ctx.h"

u32 gx2_frame_count(void);

static float g_cam_pos[3], g_cam_dir[3];
static u32 g_cam_frame;
// The gameplay camera per game frame: the drawing thread draws a frame a little after the game made it, so
// effects that need the camera (latte_render.c's sky and volume passes) take the one of the frame being drawn
// -- with the newest one the sky slid against the world when the camera turned (Octo Valley).
volatile float splat_cam_near = 1.0f, splat_cam_far = 10000.0f;
static struct { u32 frame; float pos[3], dir[3], fovy, nr, fr; } g_cam_ring[16];
int splat_camera_for_frame(u32 frame, float pos[3], float dir[3], float* fovy_deg, float* nr, float* fr) {
    for (int back = 0; back < 3; back++) {   // (that frame's, else the nearest before it)
        const u32 f = frame - (u32)back;
        if (g_cam_ring[f & 15].frame != f || !g_cam_ring[f & 15].fovy) continue;
        memcpy(pos, g_cam_ring[f & 15].pos, 12);
        memcpy(dir, g_cam_ring[f & 15].dir, 12);
        *fovy_deg = g_cam_ring[f & 15].fovy; *nr = g_cam_ring[f & 15].nr; *fr = g_cam_ring[f & 15].fr;
        return 1;
    }
    return 0;
}
static inline u32 ct_ld(u32 a) { u32 v; memcpy(&v, wiiu_fastmem + a, 4); return FAST_BSWAP32(v); }
static inline float ct_ldf(u32 a) { u32 v = ct_ld(a); float f; memcpy(&f, &v, 4); return f; }

// (an Inkling: 40-80 bones, with hip sockets, shoulders and toes by those names)
static struct { u32 fskl; int ink; } g_ik[64];
static int g_nik;
static int ink_like(u32 skel) {
    u32 fskl = ct_ld(skel);
    for (int i = 0; i < g_nik; i++) if (g_ik[i].fskl == fskl) return g_ik[i].ink;
    if (g_nik >= 64 || fskl < 0x10000000u || fskl >= 0x50000000u) return 0;
    int k = g_nik++;
    g_ik[k].fskl = fskl;
    g_ik[k].ink = 0;
    u32 nb = ct_ld(skel + 44) >> 16, barr = ct_ld(skel + 8);
    if (nb < 40 || nb > 80 || barr < 0x10000000u || barr >= 0x50000000u) return 0;
    static const char* const need[] = { "crotch_L", "shoulder_L", "toe_L" };
    int found = 0;
    for (int e = 0; e < 3; e++)
        for (u32 b = 0; b < nb; b++) {
            u32 ba = barr + b * 0x40, na = ba + ct_ld(ba);
            if (na >= 0x10000000u && na < 0x50000000u && !strncmp((const char*)(wiiu_fastmem + na), need[e], 32)) { found++; break; }
        }
    g_ik[k].ink = found == 3;
    return g_ik[k].ink;
}

// The sun: the game's light-direction cameras sit at the origin aiming along the light (the plaza: -0.4, -0.8,
// -0.5, several of them -- the shadow cascades); a downward one, not along an axis (the GamePad map's and the
// ink maps' top-down cameras are), is the sun's light (latte_render.c's sky puts its sun there).
static float g_sun_dir[3];
static u32 g_sun_frame;
int splat_sun_now(float d[3]) {
    memcpy(d, g_sun_dir, 12);
    return g_sun_frame && g_sun_frame + 120 >= gx2_frame_count();
}
float splat_hud_dist[8];
volatile int splat_hud_ndist;
// (native_funcs.c, sead::LookAtCamera updates: r3 = the camera, position +52, target +64)
void splat_camera_seen(u32 cam) {
    if (!wiiu_fastmem || cam < 0x10000000u || cam >= 0x50000000u) return;
    float p[3], t[3];
    for (int i = 0; i < 3; i++) { p[i] = ct_ldf(cam + 52 + 4 * i); t[i] = ct_ldf(cam + 64 + 4 * i); }
    float d2 = (p[0] - t[0]) * (p[0] - t[0]) + (p[1] - t[1]) * (p[1] - t[1]) + (p[2] - t[2]) * (p[2] - t[2]);
    if (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) < 0.01f && d2 > 0.8f && d2 < 1.2f && t[1] < -0.3f && fabsf(t[0]) < 0.999f && fabsf(t[1]) < 0.999f && fabsf(t[2]) < 0.999f) {
        // (other downward light cameras pass this test now and then -- high up with the free camera the sun's
        // direction jumped between frames, and the clouds' light and shadows with it -- so a new direction has to
        // hold for 90 frames before it counts: a scene change settles in 1.5 s, a stray camera never does)
        static float cand[3];
        static u32 cand_n;
        float l = sqrtf(d2), nd[3] = { -t[0] / l, -t[1] / l, -t[2] / l };
        float same = nd[0] * g_sun_dir[0] + nd[1] * g_sun_dir[1] + nd[2] * g_sun_dir[2];
        if (!g_sun_frame || same > 0.995f) { memcpy(g_sun_dir, nd, 12); g_sun_frame = gx2_frame_count(); }
        else {
            float sc = nd[0] * cand[0] + nd[1] * cand[1] + nd[2] * cand[2];
            if (sc > 0.995f) { if (++cand_n >= 90) { memcpy(g_sun_dir, nd, 12); g_sun_frame = gx2_frame_count(); cand_n = 0; } }
            else { memcpy(cand, nd, 12); cand_n = 1; }
        }
    }
    // (the camera that follows the player keeps its target 1 unit ahead: a direction; the HUD's are 989 away,
    // fixed views of the plaza 20-300, the light directions sit at the origin)
    if (p[0] == 0.0f && p[1] == 0.0f && t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && p[2] >= 100.0f) {   // (a layout camera: its distance marks the HUD's draws)
        int k = 0;
        while (k < splat_hud_ndist && fabsf(splat_hud_dist[k] - p[2]) > 0.05f) k++;
        if (k == splat_hud_ndist && k < 8) { splat_hud_dist[k] = p[2]; splat_hud_ndist = k + 1; }
        return;
    }
    if (!(d2 > 0.25f && d2 < 4.0f) || p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 1.0f) return;
    float l = sqrtf(d2);
    for (int i = 0; i < 3; i++) { g_cam_pos[i] = p[i]; g_cam_dir[i] = (t[i] - p[i]) / l; }
    g_cam_frame = gx2_frame_count();
    {
        extern volatile int splat_cam_fovy_now_x10;
        u32 f = g_cam_frame;
        g_cam_ring[f & 15].frame = f;
        memcpy(g_cam_ring[f & 15].pos, g_cam_pos, 12);
        memcpy(g_cam_ring[f & 15].dir, g_cam_dir, 12);
        g_cam_ring[f & 15].fovy = splat_cam_fovy_now_x10 * 0.1f;
        g_cam_ring[f & 15].nr = splat_cam_near; g_cam_ring[f & 15].fr = splat_cam_far;
    }
    { extern void splat_freecam_apply(u32 cam); splat_freecam_apply(cam); }   // (freecam.c: F4)
}
void splat_camera_now(float pos[3], float dir[3], int* fresh) {
    memcpy(pos, g_cam_pos, 12);
    memcpy(dir, g_cam_dir, 12);
    *fresh = g_cam_frame && g_cam_frame + 60 >= gx2_frame_count();
}

static u32 g_player, g_best, g_frame;
static float g_player_pos[3];
static u32 g_player_pos_frame;
// (freecam.c keeps the free camera near the player; the benchmark checks the player moved)
void splat_player_pos(float pos[3], int* fresh) {
    memcpy(pos, g_player_pos, 12);
    *fresh = g_player_pos_frame && g_player_pos_frame + 120 >= gx2_frame_count();
}
static float g_best_d, g_player_d;
static int g_player_seen;

// The hook (game address 02D26D80): always 0 = the game's own code runs on.
int splat_native_skel_world(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    u32 skel = ctx->gpr[3], base = ctx->gpr[4];
    if (skel < 0x10000000u || skel >= 0x50000000u || base < 0x10000000u || base >= 0x50000000u) return 0;
    if (!ink_like(skel)) return 0;
    u32 f = gx2_frame_count();
    if (f != g_frame) {   // (a new frame: last frame's nearest skeleton becomes the player's)
        extern volatile int splat_freecam_on;
        int locked = splat_freecam_on && g_player && g_player_seen;   // (free camera: stay on the same Inkling)
        if (g_best && !locked && (!g_player || !g_player_seen || g_best == g_player || g_best_d + 4.0f < g_player_d)) g_player = g_best;
        g_frame = f;
        g_best = 0;
        g_best_d = 1e30f;
        g_player_seen = 0;
    }
    float pos[3] = { ct_ldf(base + 12), ct_ldf(base + 28), ct_ldf(base + 44) };
    // (the player: the Inkling nearest that camera, in front of it, within its view)
    float d = 1e29f;
    if (g_cam_frame + 30 >= f) {
        float v[3] = { pos[0] - g_cam_pos[0], pos[1] - g_cam_pos[1], pos[2] - g_cam_pos[2] };
        float along = v[0] * g_cam_dir[0] + v[1] * g_cam_dir[1] + v[2] * g_cam_dir[2];
        float dist = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float side = sqrtf(fmaxf(dist * dist - along * along, 0.0f));
        if (along > 5.0f && side < 0.8f * along) d = dist;
    }
    if (d < g_best_d) { g_best_d = d; g_best = skel; }
    if (skel == g_player) { g_player_d = d; g_player_seen = 1; memcpy(g_player_pos, pos, sizeof(pos)); g_player_pos_frame = f; }
    return 0;
}
