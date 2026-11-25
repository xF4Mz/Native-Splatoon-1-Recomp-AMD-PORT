// The rain's sound (SHADERS "Rain" with the Storm sky): a soft, steady hiss made here -- filtered noise with a sprinkle
// of drops -- that audio.c's speaker thread adds to what it hands the speakers. Its loudness follows how much rain the
// player is in (latte_render.c's rain probe: the share of the screen with sky above it, so under a roof, in a dome or
// indoors it fades out), smoothed over about a second. SPLAT_RAIN_SOUND=0 turns it off; the volume setting scales it.
#include "hle.h"
#include "vk_ctx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double rs_now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
#else
#include <time.h>
static double rs_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
#endif

volatile float splat_rain_level;   // (0..1: the probe's latest answer)
static volatile double g_rs_when;  // (when it was set: an answer older than half a second means no rain is drawn)

void splat_rain_level_set(float level) {
    splat_rain_level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
    g_rs_when = rs_now();
}

void splat_rain_mix(float* out, u32 n, u32 channels, u32 rate, float vol) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPLAT_RAIN_SOUND"); on = !e || atoi(e) != 0; }
    static float lvl, lp1, lp2, lpr, drop, dropr;
    static u32 rng = 0x9E3779B9u;
    if (!on || !rate) return;
    float target = rs_now() - g_rs_when < 0.25 ? splat_rain_level : 0.0f;
    if (lvl < 0.002f && target <= 0.0f) return;
    float k = 1.0f - expf(-1.0f / ((target > 0.0f ? 0.8f : 0.15f) * (float)rate));   // (about a second to follow the probe; gone in a quarter when the rain stops)
    for (u32 i = 0; i < n; i++) {
        lvl += (target - lvl) * k;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        float w = (float)(int)(rng & 0xFFFF) / 32768.0f - 1.0f;
        lp1 += (w - lp1) * 0.12f;    // (the hiss: noise through two soft low-passes -- dull, not bright; a little stereo spread)
        lp2 += (lp1 - lp2) * 0.12f;
        lpr += (w - lpr) * 0.15f;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if ((rng & 0xFFFF) == 0) { drop = 0.4f; }   // (a drop now and then: a short soft click that dies away)
        if ((rng & 0x1FFFF) == 1) { dropr = 0.35f; }
        drop *= 0.9985f; dropr *= 0.9985f;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        float tick = ((float)(int)(rng & 0xFFFF) / 32768.0f - 1.0f);
        float l = (lp2 * 1.6f + lpr * 0.3f + tick * drop) * lvl * vol * 0.011f;   // (faint: a fifth of the first version)
        float r = (lp2 * 1.6f - lpr * 0.3f + tick * dropr) * lvl * vol * 0.011f;
        out[i * channels] += l;
        if (channels > 1) out[i * channels + 1] += r;
    }
}
