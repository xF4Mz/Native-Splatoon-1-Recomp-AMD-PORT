// Game functions done natively instead of by the recompiled code (same
// results, much faster). CMakeLists.txt adds one line at the start of each
// of these functions in the game code: "if (splat_native_xxx(ctx)) return
// to the caller". Each returns 0 to let the game's own code run instead.
#include "fast_cpu.h"
#include "wiiu_mem.h"
#include <math.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

// Yaz0 decompression (game address 028FDCE0): r3 = destination, r4 = the
// compressed file ("Yaz0", size, 8 unused bytes, data). Returns the size.
// Everything the game loads is packed like this; decoding it one byte at a
// time in the recompiled code was a big part of the time spent loading.
int splat_native_yaz0(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    u32 dst = ctx->gpr[3], src = ctx->gpr[4];
    const u8* s = wiiu_fastmem + src;
    u32 size = ((u32)s[4] << 24) | ((u32)s[5] << 16) | ((u32)s[6] << 8) | s[7];
    if ((u64)dst + size > 0x100000000ull || size > (512u << 20)) return 0;
    u8* d = wiiu_fastmem + dst;
    const u8* p = s + 16;
    u32 out = 0;
    while (out < size) {
        u8 code = *p++;
        for (int bit = 0; bit < 8 && out < size; bit++, code <<= 1) {
            if (code & 0x80) {
                d[out++] = *p++;
            } else {
                u32 b1 = *p++, b2 = *p++;
                u32 dist = (((b1 & 0x0F) << 8) | b2) + 1;
                u32 n = b1 >> 4;
                n = n ? n + 2 : (u32)*p++ + 0x12;
                if (n > size - out) n = size - out;
                if (dist > out) {   // (bad data: stop like a wrong read would)
                    ctx->gpr[3] = size;
                    return 1;
                }
                const u8* from = d + out - dist;
                for (u32 i = 0; i < n; i++) d[out + i] = from[i];   // (overlapping copies repeat bytes)
                out += n;
            }
        }
    }
    ctx->gpr[3] = size;
    return 1;
}

// --- Math the game does thousands of times a frame (culling, matrices) ----
// Each one below does what the game's own function does, step by step in the
// same order and precision (single-precision results, the products formed
// exactly first), so the results match. They return 0 -- run the game's
// code instead -- if guest memory isn't directly reachable or the paired-
// single load/store setting (GQR0) isn't the plain-float default.

static inline float nf_ld(u32 a) {
    u32 v;
    memcpy(&v, wiiu_fastmem + a, 4);
    v = FAST_BSWAP32(v);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline void nf_st(u32 a, float f) {
    u32 v;
    memcpy(&v, &f, 4);
    if ((v & 0x7F800000u) == 0 && (v & 0x007FFFFFu)) v = 0;   // (tiny numbers are stored as 0, like psq_st)
    v = FAST_BSWAP32(v);
    memcpy(wiiu_fastmem + a, &v, 4);
}
static inline float nf_mul(float a, float b) { return (float)((double)a * (double)b); }
static inline float nf_add(float a, float b) { return (float)((double)a + (double)b); }
static inline float nf_sub(float a, float b) { return (float)((double)a - (double)b); }
static inline float nf_madd(float a, float c, float b) { return (float)((double)a * (double)c + (double)b); }   // a*c + b
static inline bool nf_ok(CPUState* ctx) { return wiiu_fastmem && ctx->gqr[0] == 0; }

// m * (x, y, z, 1) for one matrix row, in the order PSMTXMultVec does it.
static inline float nf_row(u32 m, float x, float y, float z) {
    float a = nf_madd(nf_ld(m + 8), z, nf_mul(nf_ld(m), x));
    float b = nf_madd(nf_ld(m + 12), 1.0f, nf_mul(nf_ld(m + 4), y));
    return nf_add(a, b);
}

// 02D1E130: is a box (min xyz, max xyz) at least partly inside the view
// (r3: planes and a bounding box; r4: the box). The game asks this for
// every object, every frame.
int splat_native_frustum_box(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 f = ctx->gpr[3], b = ctx->gpr[4];
    float bmin[3] = { nf_ld(b), nf_ld(b + 4), nf_ld(b + 8) };
    float bmax[3] = { nf_ld(b + 12), nf_ld(b + 16), nf_ld(b + 20) };
    ctx->gpr[3] = 0;
    if (wiiu_read32(f + 124) != 0) {
        for (int i = 0; i < 3; i++) {
            if (nf_ld(f + 4 * i) > bmax[i]) return 1;
            if (bmin[i] > nf_ld(f + 12 + 4 * i)) return 1;
        }
    }
    s32 n = (s32)wiiu_read32(f + 120);
    for (s32 i = 0; i < n; i++) {
        u32 p = f + 24 + 16 * (u32)i;
        float nx = nf_ld(p), ny = nf_ld(p + 4), nz = nf_ld(p + 8), d = nf_ld(p + 12);
        float vx = nx >= 0.0f ? bmin[0] : bmax[0];
        float vy = ny >= 0.0f ? bmin[1] : bmax[1];
        float vz = nz >= 0.0f ? bmin[2] : bmax[2];
        float s = nf_add(nf_mul(nx, vx), nf_mul(ny, vy));
        float dist = nf_add(nf_madd(nz, vz, s), d);
        if (dist > 0.0f) return 1;
    }
    ctx->gpr[3] = 1;
    return 1;
}

f64 ppc_approx_rsqrt(f64 value);

// 02D1D730: a bounding sphere (center, radius) moved by a 3x4 matrix
// (r3: out, r4: in, r5: matrix); the radius grows by the largest scale.
int splat_native_sphere_xform(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 o = ctx->gpr[3], s = ctx->gpr[4], m = ctx->gpr[5];
    float cx = nf_ld(s), cy = nf_ld(s + 4), cz = nf_ld(s + 8), r = nf_ld(s + 12);
    float x = nf_row(m, cx, cy, cz), y = nf_row(m + 16, cx, cy, cz), z = nf_row(m + 32, cx, cy, cz);
    float m00 = nf_ld(m), m01 = nf_ld(m + 4), m02 = nf_ld(m + 8);
    float m10 = nf_ld(m + 16), m11 = nf_ld(m + 20), m12 = nf_ld(m + 24);
    float m20 = nf_ld(m + 32), m21 = nf_ld(m + 36), m22 = nf_ld(m + 40);
    float c0 = nf_madd(m20, m20, nf_madd(m00, m00, nf_mul(m10, m10)));
    float c1 = nf_madd(m21, m21, nf_madd(m01, m01, nf_mul(m11, m11)));
    float c2 = nf_madd(m22, m22, nf_madd(m02, m02, nf_mul(m12, m12)));
    float big = nf_sub(c0, c1) >= 0.0f ? c0 : c1;
    big = nf_sub(big, c2) >= 0.0f ? big : c2;
    float scale = 0.0f;
    if (big != 0.0f) {
        float e = (float)ppc_approx_rsqrt((double)big);
        float h = nf_mul(0.5f, big);
        float t = (float)(1.5 - (double)nf_mul(h, e) * (double)e);   // (fnmsubs: one rounding)
        scale = nf_mul(big, nf_mul(e, t));
    }
    nf_st(o, x);
    nf_st(o + 4, y);
    nf_st(o + 8, z);
    nf_st(o + 12, nf_mul(r, scale));
    return 1;
}

// 02D1DA4C: a box (center, half-size) moved by a 3x4 matrix, written out
// as min xyz, max xyz (r3: out, r4: in, r5: matrix).
int splat_native_box_xform(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 o = ctx->gpr[3], s = ctx->gpr[4], m = ctx->gpr[5];
    float cx = nf_ld(s), cy = nf_ld(s + 4), cz = nf_ld(s + 8);
    float ex = nf_ld(s + 12), ey = nf_ld(s + 16), ez = nf_ld(s + 20);
    float c[3] = { nf_row(m, cx, cy, cz), nf_row(m + 16, cx, cy, cz), nf_row(m + 32, cx, cy, cz) };
    float e[3];
    for (int i = 0; i < 3; i++) {
        u32 row = m + 16 * (u32)i;
        float a0 = fabsf(nf_ld(row)), a1 = fabsf(nf_ld(row + 4)), a2 = fabsf(nf_ld(row + 8));
        e[i] = nf_madd(a2, ez, nf_madd(a1, ey, nf_mul(a0, ex)));
    }
    for (int i = 0; i < 3; i++) {
        nf_st(o + 4 * (u32)i, nf_sub(c[i], e[i]));
        nf_st(o + 12 + 4 * (u32)i, nf_add(c[i], e[i]));
    }
    return 1;
}

// 02D349B8: copy a 3x4 matrix (r3 -> r4).
int splat_native_mtx_copy(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 s = ctx->gpr[3], d = ctx->gpr[4];
    float v[12];
    for (int i = 0; i < 12; i++) v[i] = nf_ld(s + 4 * (u32)i);
    for (int i = 0; i < 12; i++) nf_st(d + 4 * (u32)i, v[i]);
    return 1;
}

// 02D34910: matrix * point (r3: matrix, r4: point, r5: out).
int splat_native_mtx_multvec(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 m = ctx->gpr[3], s = ctx->gpr[4], d = ctx->gpr[5];
    float x = nf_ld(s), y = nf_ld(s + 4), z = nf_ld(s + 8);
    float ox = nf_row(m, x, y, z), oy = nf_row(m + 16, x, y, z), oz = nf_row(m + 32, x, y, z);
    nf_st(d, ox);
    nf_st(d + 4, oy);
    nf_st(d + 8, oz);
    return 1;
}

// 02D34964: matrix * direction, no translation (r3: matrix, r4: in, r5: out).
int splat_native_mtx_multvecsr(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 m = ctx->gpr[3], s = ctx->gpr[4], d = ctx->gpr[5];
    float x = nf_ld(s), y = nf_ld(s + 4), z = nf_ld(s + 8);
    float o[3];
    for (int i = 0; i < 3; i++) {
        u32 row = m + 16 * (u32)i;
        float sum = nf_add(nf_mul(nf_ld(row), x), nf_mul(nf_ld(row + 4), y));
        o[i] = nf_madd(nf_ld(row + 8), z, sum);
    }
    nf_st(d, o[0]);
    nf_st(d + 4, o[1]);
    nf_st(d + 8, o[2]);
    return 1;
}

// 0251C518: is a point inside an object's box (r3: the object -- position
// at +120, its three axes at +84/+96/+108, the box size at +456..+476;
// r4: the point)? Asked for a great many objects every frame (it was the
// single busiest function on the main thread).
int splat_native_point_in_box(CPUState* ctx) {
    if (!nf_ok(ctx)) return 0;
    u32 o = ctx->gpr[3], p = ctx->gpr[4];
    if (wiiu_read32(o + 452) == 1) { ctx->gpr[3] = 0; return 1; }
    float dx = nf_sub(nf_ld(p), nf_ld(o + 120)), dy = nf_sub(nf_ld(p + 4), nf_ld(o + 124)), dz = nf_sub(nf_ld(p + 8), nf_ld(o + 128));
    float v0 = nf_madd(dz, nf_ld(o + 92), nf_add(nf_mul(dx, nf_ld(o + 84)), nf_mul(dy, nf_ld(o + 88))));
    float v1 = nf_madd(dz, nf_ld(o + 104), nf_add(nf_mul(dx, nf_ld(o + 96)), nf_mul(dy, nf_ld(o + 100))));
    float v2 = nf_madd(dz, nf_ld(o + 116), nf_add(nf_mul(dx, nf_ld(o + 108)), nf_mul(dy, nf_ld(o + 112))));
    float c = nf_ld(0x100BF038);   // (0.5)
    float h0 = nf_mul(nf_sub(nf_ld(o + 468), nf_ld(o + 456)), c);
    float h1 = nf_mul(nf_sub(nf_ld(o + 472), nf_ld(o + 460)), c);
    float h2 = nf_mul(nf_sub(nf_ld(o + 476), nf_ld(o + 464)), c);
    float k0 = nf_ld(0x10217338), k1 = nf_ld(0x1021733C), k2 = nf_ld(0x10217340);
    bool in = !(nf_sub(k0, h0) > v0) && !(v0 > nf_add(k0, h0)) &&
              !(nf_sub(k1, h1) > v1) && !(v1 > nf_add(k1, h1)) &&
              !(nf_sub(k2, h2) > v2) && !(v2 > nf_add(k2, h2));
    ctx->gpr[3] = in ? 1 : 0;
    return 1;
}

// --- Camera projection: the FOV, wide-screen and HUD settings --------------
// sead::PerspectiveProjection::doUpdateMatrix (game address 028E85C4):
// r3 = the projection (near +148, far +152, fovy +156, sin/cos/tan of fovy/2
// +160/+164/+168, aspect +172, offset x/y +176/+180), r4 = the 4x4
// (row-major) it writes. With a setting on, the matrix is built here from
// the changed values (the game's own arithmetic, in its order) AND the
// changed values are written into the projection, so the game's culling,
// aiming and screen projections use the same camera (without that, things
// at the edges of a wider view vanished). What we wrote is remembered per
// projection: the next call starts again from the game's own values.
#include <stdio.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
volatile int splat_fov_add_deg = 0;          // degrees added to the scene camera's vertical field of view (the panel sets it)
volatile int splat_wide_kx_x1000 = 0;        // picture wider than 16:9 by this factor (x1000; 0 = not wider)
volatile int splat_wide_ky_x1000 = 0;        // picture taller than 16:9 by this factor (x1000; 0 = not taller)
volatile int splat_hud_stretch = 0;          // 1 = the HUD cameras are left alone (the HUD stretches over the whole picture)
volatile int splat_cam_fovy_x10 = 0, splat_cam_fovy_now_x10 = 0;
volatile int splat_game_own_fps = 0;          // Performance mode: scenes that ask for 30 fps (the plaza) keep it (gx2.c reads it)
volatile int splat_volume_pct = 100;         // the sound's volume (audio.c reads it; the panel sets it)   // the scene camera's vertical FOV, the game's and the shown one (degrees x10, for the panel)
static int g_proj_log = -1;
// Each different projection is logged once (the first 96): which cameras a scene has.
static void proj_note(const char* kind, u32 self, float a, float b, float c, float d, const char* note) {
    static struct { u32 self; float a, b, c, d; } seen[96];
    static int n = 0;
    bool by_self = kind[0] == 'o';   // (one ortho object builds a whole chain of sizes: once per object)
    for (int i = 0; i < n; i++)
        if (seen[i].self == self && (by_self || (seen[i].a == a && seen[i].b == b && seen[i].c == c && seen[i].d == d))) return;
    if (n >= 96) return;
    seen[n].self = self; seen[n].a = a; seen[n].b = b; seen[n].c = c; seen[n].d = d; n++;
    if (kind[0] == 'p') printf("[proj] %s %08X: fovy %.1f deg, aspect %.4f, near %g, far %g%s (fields: fovy %g sin %g cos %g)\n", kind, self, a, b, c, d, note,
                               nf_ld(self + 156) * 180.0 / M_PI, nf_ld(self + 160), nf_ld(self + 164));
    else printf("[proj] %s %08X: %g x %g, near %g, far %g%s\n", kind, self, a, b, c, d, note);
}
typedef struct { u32 self; float t_in, asp_in, t_out, asp_out; } ProjMemo;
static ProjMemo g_pm[64];
static int g_pm_n = 0;
static ProjMemo* pm_find(u32 self) {
    for (int i = 0; i < g_pm_n; i++) if (g_pm[i].self == self) return &g_pm[i];
    return NULL;
}
static void write_perspective_fields(u32 self, float t, float asp) {
    float fovy = 2.0f * atanf(t);
    // (sin/cos of fovy/2 live at +160/+164 when they agree with the tan at +168; otherwise they are left alone)
    float s0 = nf_ld(self + 160), c0 = nf_ld(self + 164), t0 = nf_ld(self + 168);
    bool sc_ok = c0 != 0.0f && fabsf(s0 / c0 - t0) < 1e-3f * (1.0f + fabsf(t0)) && fabsf(s0 * s0 + c0 * c0 - 1.0f) < 1e-3f;
    nf_st(self + 156, fovy);
    if (sc_ok) { nf_st(self + 160, sinf(fovy * 0.5f)); nf_st(self + 164, cosf(fovy * 0.5f)); }
    nf_st(self + 168, t);
    nf_st(self + 172, asp);
}
static bool cam_guard(const char* what, u32 self, u32 a, u32 n);
int splat_native_perspective(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    u32 self = ctx->gpr[3], m = ctx->gpr[4];
    cam_guard("projection (near, far, fovy, sin, cos, tan, aspect, offset)", self, self + 148, 9);   // (safety net, see splat_native_lookat)
    float nr = nf_ld(self + 148), fr = nf_ld(self + 152), t = nf_ld(self + 168), asp = nf_ld(self + 172);
    float ox = nf_ld(self + 176), oy = nf_ld(self + 180);
    ProjMemo* pm = pm_find(self);
    if (pm && t == pm->t_out && asp == pm->asp_out) { t = pm->t_in; asp = pm->asp_in; }   // (ours from last time: start from the game's)
    else if (pm) { pm->t_in = t; pm->asp_in = asp; }
    float fovy_deg = (float)(2.0 * atan(t) * 180.0 / M_PI);
    if (g_proj_log < 0) { const char* e = getenv("SPLAT_PROJ_LOG"); g_proj_log = e ? atoi(e) : 0; }
    if (g_proj_log > 0) {
        g_proj_log--;
        printf("[proj] perspective %08X -> %08X: near %g far %g fovy %.1f deg aspect %.4f offset %g,%g (called from %08X)\n",
               self, m, nr, fr, fovy_deg, asp, ox, oy, ctx->lr);
    }
    int add = splat_fov_add_deg, kx = splat_wide_kx_x1000, ky = splat_wide_ky_x1000;
    bool is_tv = asp > 1.70f && asp < 1.85f;   // (the TV and GamePad cameras are 16:9; cube-map cameras are 1:1)
    // The HUD and menus are drawn by small 40-degree cameras at a fixed distance (one per layout): they keep
    // the game's field of view, or the HUD would shrink into the middle; they take the picture's shape unless
    // the HUD is set to stretch.
    bool is_hud = fovy_deg > 39.95f && fovy_deg < 40.05f && fr <= 10000.0f;
    // (a narrow camera -- the GamePad's map, a scope -- keeps its field of view too)
    bool is_map = is_tv && !is_hud && fovy_deg < 20.0f;
    // (menus' close-up cameras -- the gear menu's 45 degrees -- keep it as well: +30 made the Inkling there tiny;
    // the cameras that follow the player are 55-80 degrees. SPLAT_FOV_ALL_CAMS=1: every scene camera, as before)
    static int fov_all = -1;
    if (fov_all < 0) fov_all = getenv("SPLAT_FOV_ALL_CAMS") != NULL;
    // (the camera that follows the player zooms down to 26 degrees in cutscenes, super jumps and the plaza's intro -- the
    // same camera object; a camera once seen at 50 or more stays a scene camera below 50, or the field of view would jump
    // and the volume pass (clouds, rain, puddles: camera_track.c's camera ring takes splat_cam_fovy_now_x10) would keep a stale one)
    static u32 scene_cams[16];
    static int n_scene_cams;
    bool known_scene = false;
    for (int i = 0; i < n_scene_cams; i++) if (scene_cams[i] == self) known_scene = true;
    if (is_tv && !is_hud && fovy_deg >= 50.0f && fovy_deg < 20.0f + 180.0f && !known_scene && n_scene_cams < 16) { scene_cams[n_scene_cams++] = self; known_scene = true; }
    bool is_close = is_tv && !is_hud && !is_map && fovy_deg < 50.0f && !fov_all && !known_scene;
    proj_note("perspective", self, fovy_deg, asp, nr, fr, !is_tv ? " (not 16:9: left alone)" : is_hud ? " (a HUD camera)" : is_map ? " (a narrow camera: map / scope, FOV setting skipped)" :
              is_close ? " (a close-up camera: menus, FOV setting skipped)" : " (a scene camera)");
    if (is_tv && !is_hud && !is_map && !is_close) {
        splat_cam_fovy_x10 = (int)(fovy_deg * 10.0f + 0.5f);
        extern volatile float splat_cam_near, splat_cam_far;   // (camera_track.c: the effects' per-frame camera)
        splat_cam_near = nr; splat_cam_far = fr;
    }
    if (is_map || is_close) add = 0;
    bool change = is_tv && ((add && !is_hud) || ((kx || ky) && !(is_hud && splat_hud_stretch)));
    if (!change) {
        if (pm) { write_perspective_fields(self, t, asp); pm->t_out = t; pm->asp_out = asp; }   // (settings off again: the game's values back)
        // (only a scene camera sets the field of view the volume pass uses: the GamePad's narrow map camera, rebuilt every
        // frame, used to write the scene camera's value back here without the setting -- clouds, rain and puddles were
        // placed with the wrong field of view whenever the FOV setting was on)
        if (is_tv && !is_hud && !is_close && !is_map) splat_cam_fovy_now_x10 = splat_cam_fovy_x10;
        return 0;
    }
    float t2 = t, asp2 = asp;
    if (ky) t2 = t2 * ((float)ky / 1000.0f);   // (a taller picture: more above and below, the same width of view)
    if (add && !is_hud) {
        float fovy = 2.0f * atanf(t2) + (float)add * (float)M_PI / 180.0f;
        if (fovy > 110.0f * (float)M_PI / 180.0f) fovy = 110.0f * (float)M_PI / 180.0f;
        if (fovy < 5.0f * (float)M_PI / 180.0f) fovy = 5.0f * (float)M_PI / 180.0f;
        t2 = tanf(fovy * 0.5f);
    }
    if (kx || ky) asp2 = asp * (kx ? (float)kx / 1000.0f : 1.0f) / (ky ? (float)ky / 1000.0f : 1.0f);
    if (!is_hud && !is_map && !is_close) splat_cam_fovy_now_x10 = (int)(2.0 * atan(t2) * 180.0 / M_PI * 10.0 + 0.5);
    if (!pm && g_pm_n < 64) { pm = &g_pm[g_pm_n++]; pm->self = self; pm->t_in = t; pm->asp_in = asp; }
    if (pm) { pm->t_out = t2; pm->asp_out = asp2; }
    write_perspective_fields(self, t2, asp2);
    t = t2;
    asp = asp2;
    float n2 = nf_add(nr, nr);
    float h = nf_mul(n2, t), w = nf_mul(h, asp);
    float cx = nf_mul(w, ox), cy = nf_mul(h, oy);
    float hw = nf_mul(w, 0.5f), hh = nf_mul(h, 0.5f);
    float left = nf_sub(cx, hw), right = nf_add(hw, cx), bottom = nf_sub(cy, hh), top = nf_add(hh, cy);
    float inv = (float)(1.0 / (double)nf_sub(right, left));
    nf_st(m + 0, nf_mul(n2, inv));
    nf_st(m + 4, 0.0f);
    nf_st(m + 8, nf_mul(nf_add(right, left), inv));
    nf_st(m + 12, 0.0f);
    nf_st(m + 16, 0.0f);
    inv = (float)(1.0 / (double)nf_sub(top, bottom));
    nf_st(m + 20, nf_mul(n2, inv));
    nf_st(m + 24, nf_mul(nf_add(top, bottom), inv));
    nf_st(m + 28, 0.0f);
    nf_st(m + 32, 0.0f);
    nf_st(m + 36, 0.0f);
    inv = (float)(1.0 / (double)nf_sub(fr, nr));
    nf_st(m + 40, -nf_mul(nf_add(fr, nr), inv));
    nf_st(m + 44, -nf_mul(nf_mul(nf_add(fr, fr), nr), inv));
    nf_st(m + 48, 0.0f);
    nf_st(m + 52, 0.0f);
    nf_st(m + 56, -1.0f);
    nf_st(m + 60, 0.0f);
    return 1;
}
// sead::OrthoProjection::doUpdateMatrix (game address 028E8AFC): r3 = the
// projection (near +148, far +152, top +156, bottom +160, left +164, right
// +168), r4 = the 4x4. A screen-shaped (16:9) ortho -- HUD, menus -- takes
// the picture's shape (unless the HUD is set to stretch), so what it draws
// keeps its size in the middle; any other (shadow maps) runs as is.
int splat_native_ortho(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    u32 self = ctx->gpr[3], m = ctx->gpr[4];
    float nr = nf_ld(self + 148), fr = nf_ld(self + 152), top = nf_ld(self + 156), bottom = nf_ld(self + 160);
    float left = nf_ld(self + 164), right = nf_ld(self + 168);
    if (g_proj_log < 0) { const char* e = getenv("SPLAT_PROJ_LOG"); g_proj_log = e ? atoi(e) : 0; }
    if (g_proj_log > 0) {
        g_proj_log--;
        printf("[proj] ortho %08X -> %08X: near %g far %g top %g bottom %g left %g right %g (called from %08X)\n",
               self, m, nr, fr, top, bottom, left, right, ctx->lr);
    }
    int kx = splat_wide_kx_x1000, ky = splat_wide_ky_x1000;
    float w = right - left, h = top - bottom;
    proj_note("ortho", self, w, h, nr, fr, "");
    if ((!kx && !ky) || splat_hud_stretch || h == 0 || w / h < 1.70f || w / h > 1.85f) return 0;
    float fx = kx ? (float)kx / 1000.0f : 1.0f, fy = ky ? (float)ky / 1000.0f : 1.0f;
    float cx = (left + right) * 0.5f, hw = w * 0.5f * fx, cy = (top + bottom) * 0.5f, hh = h * 0.5f * fy;
    left = cx - hw;
    right = cx + hw;
    top = cy + hh;
    bottom = cy - hh;
    float hw2 = nf_mul(nf_sub(right, left), 0.5f), hh2 = nf_mul(nf_sub(top, bottom), 0.5f);
    float cz = nf_ld(0x10130000u + 476);   // (the game's depth constant for ortho: read where its code reads it)
    nf_st(m + 0, (float)(1.0 / (double)hw2));
    nf_st(m + 4, 0.0f);
    nf_st(m + 8, 0.0f);
    nf_st(m + 12, -(float)((double)nf_mul(nf_add(left, right), 0.5f) / (double)hw2));
    nf_st(m + 16, 0.0f);
    nf_st(m + 20, (float)(1.0 / (double)hh2));
    nf_st(m + 24, 0.0f);
    nf_st(m + 28, -(float)((double)nf_mul(nf_add(top, bottom), 0.5f) / (double)hh2));
    nf_st(m + 32, 0.0f);
    nf_st(m + 36, 0.0f);
    float inv = (float)(1.0 / (double)nf_sub(fr, nr));
    nf_st(m + 40, nf_mul(cz, inv));
    nf_st(m + 44, -nf_mul(nf_add(nr, fr), inv));
    nf_st(m + 48, 0.0f);
    nf_st(m + 52, 0.0f);
    nf_st(m + 56, 0.0f);
    nf_st(m + 60, 1.0f);
    return 1;
}
// MTXPerspective (game address 02DD03A0): r3 = the matrix, f1 = fovy in
// degrees, f2 = aspect, f3 = near, f4 = far. Logged only (SPLAT_PROJ_LOG=N)
// to see who uses it; the game's code runs.
int splat_native_mtx_perspective(CPUState* ctx) {
    if (g_proj_log < 0) { const char* e = getenv("SPLAT_PROJ_LOG"); g_proj_log = e ? atoi(e) : 0; }
    if (g_proj_log > 0) {
        g_proj_log--;
        printf("[proj] MTXPerspective -> %08X: fovy %g aspect %g near %g far %g (called from %08X)\n",
               ctx->gpr[3], ctx->fpr[1], ctx->fpr[2], ctx->fpr[3], ctx->fpr[4], ctx->lr);
    }
    return 0;
}

// --- Safety net: a camera that turned into NaN ------------------------------
// The frozen TV world: a NaN reached the TV camera and the game culled its
// whole world against a NaN view (only effects and the HUD were drawn) until
// the next scene load. The cause is fixed (flush-to-zero, runtime_run in
// main.c); should a NaN or infinity reach a camera anyway, the camera's last
// good values are put back before the game builds its matrices from them, so
// the world stays drawn (culling and the picture use the same repaired
// camera). Logged; counted for the freeze report. The game's code runs as
// always (these return 0).
#define CAM_MEMO 48
typedef struct { u32 self; u32 n; float v[9]; } CamMemo;
static CamMemo g_cam_good[CAM_MEMO];
static u32 g_cam_next;
static volatile long g_cam_lock;
volatile int splat_cam_repairs, splat_cam_last_frame;
volatile u32 splat_cam_last_self;
extern u32 gx2_frame_count(void);
static CamMemo* cam_memo(u32 self, u32 n, bool add) {
    for (int i = 0; i < CAM_MEMO; i++) if (g_cam_good[i].self == self && g_cam_good[i].n == n) return &g_cam_good[i];
    if (!add) return NULL;
    CamMemo* m = &g_cam_good[g_cam_next++ % CAM_MEMO];   // (cameras come and go with scenes: the oldest place is reused)
    m->self = self;
    m->n = n;
    return m;
}
// v = n floats at guest address a (part of the camera object self): all finite -> remembered; else
// the remembered ones are written back (returns true when it repaired).
static bool cam_guard(const char* what, u32 self, u32 a, u32 n) {
    static int on = -1;   // (SPLAT_CAMERA_GUARD=0: off -- to see a NaN camera's effect)
    if (on < 0) { const char* e = getenv("SPLAT_CAMERA_GUARD"); on = e ? atoi(e) : 1; }
    float v[9];
    bool ok = true;
    for (u32 i = 0; i < n; i++) { v[i] = nf_ld(a + 4 * i); if (!isfinite(v[i])) ok = false; }
    while (_InterlockedCompareExchange(&g_cam_lock, 1, 0)) {}
    CamMemo* m = cam_memo(self, n, ok);
    if (ok) memcpy(m->v, v, sizeof(float) * n);
    else if (m && on) for (u32 i = 0; i < n; i++) nf_st(a + 4 * i, m->v[i]);
    g_cam_lock = 0;
    if (ok) return false;
    splat_cam_repairs++;
    splat_cam_last_frame = (int)gx2_frame_count();
    splat_cam_last_self = self;
    static int shown = 0;
    if (shown < 20) {
        shown++;
        printf("[camera] %s %08X held NaN/infinity (%g %g %g ...) at frame %u: %s\n", what, self, v[0], v[1], v[2],
               gx2_frame_count(), !on ? "guard off (SPLAT_CAMERA_GUARD=0)" : m ? "its last good values put back" : "no good values known yet, left alone");
    }
    return m != NULL && on;
}
// sead::LookAtCamera::doUpdateMatrix (028E6CFC): r3 = the camera (position +52, target +64, up +76),
// r4 = the 3x4 view matrix it writes.
int splat_native_lookat(CPUState* ctx) {
    if (wiiu_fastmem) cam_guard("look-at camera", ctx->gpr[3], ctx->gpr[3] + 52, 9);
    { extern void splat_camera_seen(u32 cam); splat_camera_seen(ctx->gpr[3]); }   // (camera_track.c: the camera per frame, which Inkling is the player's)
    return 0;
}

// --- An animation frame wrap that can spin forever -------------------------
// Game address 0265C0FC, inside the animation player at 0265BE4C: the loop
// "while (frame >= length) frame -= length" (f12 = frame, f10 = length) after
// the frame was advanced. With a length of 0, or a frame that is NaN or
// infinite, it never ends -- the main thread spun there forever in the
// Octo Valley hub (the user's "crash while the sound keeps playing", Oct 18).
// A few steps run as the game's code; a hopeless case gets the frame wrapped
// here (or reset to 0) and the game continues at 0265C108.
int splat_native_anim_wrap(CPUState* ctx) {
    f64 fr = ctx->fpr[12], ln = ctx->fpr[10];
    if (ln > 0.0 && fr == fr && fr < ln * 64.0) return 0;
    float out = (ln > 0.0 && fr == fr && fr < 1e30) ? fmodf((float)fr, (float)ln) : 0.0f;
    static int shown = 0;
    if (shown < 20) {
        shown++;
        printf("[game] animation %08X would spin forever at its frame wrap: frame %g, length %g, step %g -> frame %g (game address 0265C0FC)\n",
               ctx->gpr[31], fr, ln, wiiu_fastmem ? nf_ld(ctx->gpr[31] + 16) : 0.0f, out);
    }
    ctx->fpr[12] = out;
    ctx->ps1[12] = out;
    return 1;
}

// --- The Battle Dojo's split drawing (game address 0242B094) ---------------------------------------------------------
// Nov 11: in the two-player Dojo the game draws the two players' 3D views on alternate frames (each view ~30 times a
// second while the game runs at 60 -- the user's "TV mode runs at about 40"). The scene setup that chooses it (0242A91C)
// reads a debug flag "EnableDuelDrawCalcCalc" (off in retail: then the draw mode byte at manager+14077 = 2 and a flag
// per view at +733; on: 30 fps with both views every frame). This hook runs right after that choice: it logs the
// manager and switches mode 2 to mode 1 (the plaza's: every view every frame) when splat_dojo_both is set -- the settings'
// "Dojo views" (latte_render.c sets it each frame; off in Performance mode). Tested Nov 11: the TV scene drawn in 600 of
// 600 frames instead of 300, the game still at 60.2 (card 2.5 -> 5 ms at 2x), the match clock unchanged.
// SPLAT_DUEL_DRAW=n: write mode n instead (experiments).
volatile int splat_dojo_both = 1;
int splat_native_duel_draw(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    u32 owner = ctx->gpr[29];
    if (!owner || !wiiu_mem_ptr(owner + 8, 4)) return 0;
    u32 mgr = wiiu_read32(owner + 8);
    if (!mgr || !wiiu_mem_ptr(mgr + 14077, 1)) return 0;
    u8 mode = wiiu_read8(mgr + 14077);
    u32 nviews = wiiu_read32(mgr + 14068), v0 = wiiu_read32(mgr + 14072);
    static int said;
    if (said++ < 8)
        printf("[duel] scene draw setup: manager %08X, draw mode %u, %u views at %08X (view flags %u / %u)\n", mgr, mode, nviews, v0,
               v0 && wiiu_mem_ptr(v0 + 733, 1) ? wiiu_read8(v0 + 733) : 0, v0 && nviews > 1 && wiiu_mem_ptr(v0 + 736 + 733, 1) ? wiiu_read8(v0 + 736 + 733) : 0);
    static int want = -2;
    if (want == -2) { const char* e = getenv("SPLAT_DUEL_DRAW"); want = e ? atoi(e) : -1; }
    if (want < 0 && splat_dojo_both && mode == 2) {
        wiiu_write8(mgr + 14077, 1);
        static int told;
        if (told++ < 4) printf("[duel] both players' views drawn every frame (settings \"Dojo views\"; the console alternates them)\n");
    }
    if (want >= 0 && mode == 2) {
        wiiu_write8(mgr + 14077, (u8)want);
        printf("[duel] draw mode 2 -> %d (SPLAT_DUEL_DRAW)\n", want);
    }
    return 0;
}
