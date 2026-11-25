// Free camera (F4): the game's own camera, moved by the player instead of following the Inkling --
// for screenshots and looking around. The Inkling stands still meanwhile (the controls go to the
// camera). WASD / left stick fly, the mouse / right stick (or I J K L) look, Space / jump up,
// Shift / ZL down, right mouse button / ZR faster.
//
// How: sead::LookAtCamera's update (game address 028E6CFC, native_funcs.c) gets, for the camera that
// follows the player (its target kept 1 unit ahead of it: camera_track.c finds it), this camera's position
// and target written in just before it builds its view matrix. The world is culled and drawn from
// there by the game itself.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fast_cpu.h"
#include "wiiu_mem.h"

volatile int splat_freecam_on;
static float g_fc_pos[3], g_fc_yaw, g_fc_pitch;
static volatile int g_fc_seeded;

void splat_camera_now(float pos[3], float dir[3], int* fresh);   // (camera_track.c: the game's own camera, last frame)

void splat_freecam_toggle(void) {
    if (!splat_freecam_on) {
        float p[3], d[3];
        int fresh = 0;
        splat_camera_now(p, d, &fresh);
        if (!fresh) { printf("[freecam] no game camera to start from yet\n"); return; }
        memcpy(g_fc_pos, p, sizeof(p));
        g_fc_yaw = atan2f(d[0], d[2]);
        g_fc_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, d[1])));
        g_fc_seeded = 1;
        splat_freecam_on = 1;
        printf("[freecam] on: WASD / left stick fly, mouse / right stick look, Space up, Shift down, right mouse / ZR faster (F4 again: back)\n");
    } else {
        splat_freecam_on = 0;
        printf("[freecam] off\n");
    }
}

// (benchmark.c: the camera put somewhere, looking somewhere -- the benchmark's sweep)
void splat_freecam_place(const float pos[3], float yaw, float pitch) {
    memcpy(g_fc_pos, pos, 12);
    g_fc_yaw = yaw;
    g_fc_pitch = pitch;
    g_fc_seeded = 1;
    splat_freecam_on = 1;
}
void splat_freecam_off(void) { splat_freecam_on = 0; }
void splat_freecam_where(float pos[3], float* yaw, float* pitch) { memcpy(pos, g_fc_pos, 12); *yaw = g_fc_yaw; *pitch = g_fc_pitch; }

// (input.c, each poll while it's on: the controls move the camera)
void splat_freecam_input(float lx, float ly, float rx, float ry, unsigned hold, float mouse_dx, float mouse_dy, float dt) {
    if (!splat_freecam_on || !g_fc_seeded) return;
    if (dt > 0.1f) dt = 0.1f;
    const float look = 2.2f;   // (radians per second at a full stick)
    g_fc_yaw -= rx * look * dt + mouse_dx * 0.0035f;
    g_fc_pitch += ry * look * dt - mouse_dy * 0.0035f;
    if (g_fc_pitch > 1.5f) g_fc_pitch = 1.5f;
    if (g_fc_pitch < -1.5f) g_fc_pitch = -1.5f;
    float speed = (hold & 0x0040) ? 260.0f : 70.0f;   // (ZR: faster)
    float fx = sinf(g_fc_yaw), fz = cosf(g_fc_yaw);   // (forward on the ground)
    float sxv = cosf(g_fc_yaw), szv = -sinf(g_fc_yaw);   // (its right: the model's +x is the Inkling's left)
    float up = ((hold & 0x4000) ? 1.0f : 0.0f) - ((hold & 0x0080) ? 1.0f : 0.0f);   // (B / Space up, ZL / Shift down)
    float fwd3[3] = { cosf(g_fc_pitch) * fx, sinf(g_fc_pitch), cosf(g_fc_pitch) * fz };
    g_fc_pos[0] += (fwd3[0] * ly - sxv * lx) * speed * dt;
    g_fc_pos[1] += (fwd3[1] * ly + up) * speed * dt;
    g_fc_pos[2] += (fwd3[2] * ly - szv * lx) * speed * dt;
    // (anywhere: outside a map's sky dome the scene picture gets a haze background each frame -- latte_render.c
    // fills it while the free camera is on -- so old frames no longer smear into rings. SPLAT_FREECAM_RANGE=n
    // keeps it within n units of the player)
    static float range = -1.0f;
    if (range < 0.0f) { const char* e = getenv("SPLAT_FREECAM_RANGE"); range = e ? (float)atof(e) : 0.0f; }
    float pp[3];
    int fresh = 0;
    void splat_player_pos(float pos[3], int* fresh);
    splat_player_pos(pp, &fresh);
    if (range > 0.0f && fresh) {
        float dx = g_fc_pos[0] - pp[0], dz = g_fc_pos[2] - pp[2], h = sqrtf(dx * dx + dz * dz);
        if (h > range) { g_fc_pos[0] = pp[0] + dx * range / h; g_fc_pos[2] = pp[2] + dz * range / h; }
        if (g_fc_pos[1] > pp[1] + range * 0.66f) g_fc_pos[1] = pp[1] + range * 0.66f;
        if (g_fc_pos[1] < pp[1] - range * 0.2f) g_fc_pos[1] = pp[1] - range * 0.2f;
    }
}

// (camera_track.c, for the camera that follows the player, before the game builds its view matrix)
void splat_freecam_apply(u32 cam) {
    if (!splat_freecam_on || !g_fc_seeded || !wiiu_fastmem) return;
    float d[3] = { cosf(g_fc_pitch) * sinf(g_fc_yaw), sinf(g_fc_pitch), cosf(g_fc_pitch) * cosf(g_fc_yaw) };
    float v[9] = { g_fc_pos[0], g_fc_pos[1], g_fc_pos[2], g_fc_pos[0] + d[0], g_fc_pos[1] + d[1], g_fc_pos[2] + d[2], 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 9; i++) {   // (position +52, target +64, up +76)
        u32 b;
        memcpy(&b, &v[i], 4);
        b = FAST_BSWAP32(b);
        memcpy(wiiu_fastmem + cam + 52 + 4 * i, &b, 4);
    }
}
