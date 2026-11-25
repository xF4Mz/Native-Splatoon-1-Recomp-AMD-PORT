// Emotes (F5): a dance played on the player's own Inkling -- only on this PC, the game itself knows
// nothing about it.
//
// emotes\<name>.emote (made by tools/emote_convert.py from a Unity humanoid clip): per frame, the
// local rotations of the Inkling bones it drives and joint_root's position. Its sound, if any:
// emotes\<name>.ogg / .wav / .mp3 (decoded by Windows' Media Foundation; settings row "Emote sound").
// The first .emote in the folder (alphabetically) is the one F5 plays.
//
// How it reaches the Inkling: the game's skeleton step that turns each bone's local pose into world
// matrices (nw::g3d SkeletonObj::CalcWorldMtx, game address 02D26D80; r3 = the skeleton: +0 its
// FSKL, +8 bones, +12 local poses (64 bytes each: flags, scale, 3x4 matrix), +16 world matrices,
// +44 bone count; r4 = the model's matrix) is hooked: for the player's skeleton, the emote's pose is
// written over the animation's just before the game uses it, blended in and out over 0.3 s. Which
// skeleton is the player's: of the Inkling skeletons in a frame, the one nearest the gameplay
// camera's target (the camera hook, native_funcs.c). Moving, jumping or shooting stops the dance.
#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fast_cpu.h"
#include "wiiu_mem.h"
#include "vk_ctx.h"

u32 gx2_frame_count(void);

#define EM_MAX_BONES 64
static struct {
    volatile int state;          // 0 not tried, 1 loading, 2 ready, -1 none / broken
    char name[64];
    float fps;
    u32 frames, nbones;
    char bone[EM_MAX_BONES][32];
    float* data;                 // frames x (3 + 4 x bones)
    float* pcm;                  // the sound: stereo float
    u32 pcm_frames, pcm_rate;
} g_em;
static volatile int g_em_on;     // the dance is playing (or blending out)
static volatile double g_em_t0, g_em_stop;   // its start, and when it was told to stop (0: not)
static volatile double g_em_snd_pos;         // (the sound's place, in its own frames)

static double em_now(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

// --- loading ---
#ifdef _WIN32
static bool em_decode_audio(const char* path) {
    wchar_t wp[600];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 600);
    bool ok = false;
    IMFSourceReader* rd = NULL;
    IMFMediaType *mt = NULL, *cur = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    HRESULT hr = MFCreateSourceReaderFromURL(wp, NULL, &rd);
    if (FAILED(hr)) { printf("[emote] Media Foundation can't open the sound (%08lX)\n", (unsigned long)hr); goto done; }
    if (FAILED(MFCreateMediaType(&mt))) goto done;
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, mt);
    if (FAILED(hr)) { printf("[emote] the sound won't decode to float samples (%08lX)\n", (unsigned long)hr); goto done; }
    if (FAILED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) goto done;
    UINT32 rate = 0, ch = 0;
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
    if (!rate || !ch) goto done;
    size_t cap = (size_t)rate * 2 * 30, n = 0;   // (stereo frames; grows)
    float* out = (float*)malloc(cap * sizeof(float));
    for (;;) {
        DWORD flags = 0;
        IMFSample* smp = NULL;
        if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &smp))) break;
        if (smp) {
            IMFMediaBuffer* buf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf))) {
                BYTE* p = NULL;
                DWORD len = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
                    const float* fs = (const float*)p;
                    size_t frames = len / (sizeof(float) * ch);
                    if (n + frames * 2 > cap) { cap = (n + frames * 2) * 2; out = (float*)realloc(out, cap * sizeof(float)); }
                    for (size_t i = 0; i < frames; i++) {   // (to stereo)
                        out[n++] = fs[i * ch];
                        out[n++] = fs[i * ch + (ch > 1 ? 1 : 0)];
                    }
                    IMFMediaBuffer_Unlock(buf);
                }
                IMFMediaBuffer_Release(buf);
            }
            IMFSample_Release(smp);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (n) { g_em.pcm = out; g_em.pcm_frames = (u32)(n / 2); g_em.pcm_rate = rate; ok = true; }
    else free(out);
done:
    if (cur) IMFMediaType_Release(cur);
    if (mt) IMFMediaType_Release(mt);
    if (rd) IMFSourceReader_Release(rd);
    MFShutdown();
    return ok;
}
#endif

static void em_load(void) {
    g_em.state = 1;
    char path[600] = "";
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("emotes\\*.emote", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        char best[260] = "";
        do { if (!best[0] || _stricmp(fd.cFileName, best) < 0) snprintf(best, sizeof(best), "%s", fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
        snprintf(path, sizeof(path), "emotes\\%s", best);
    }
#endif
    FILE* f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) { g_em.state = -1; printf("[emote] no emote: put a .emote file (tools/emote_convert.py) in emotes\\\n"); return; }
    char magic[4];
    u32 ver = 0, frames = 0, nb = 0;
    float fps = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "SEMO", 4) && fread(&ver, 4, 1, f) == 1 && fread(&fps, 4, 1, f) == 1 &&
              fread(&frames, 4, 1, f) == 1 && fread(&nb, 4, 1, f) == 1 && ver == 1 && nb && nb <= EM_MAX_BONES && frames && frames < 100000 && fps > 0;
    if (ok) for (u32 i = 0; i < nb && ok; i++) { ok = fread(g_em.bone[i], 1, 32, f) == 32; g_em.bone[i][31] = 0; }
    size_t per = 3 + 4 * (size_t)nb;
    float* data = ok ? (float*)malloc(frames * per * sizeof(float)) : NULL;
    if (data && fread(data, sizeof(float), frames * per, f) != frames * per) ok = false;
    fclose(f);
    if (!ok) { free(data); g_em.state = -1; printf("[emote] %s isn't an emote file this game knows\n", path); return; }
    g_em.data = data; g_em.frames = frames; g_em.nbones = nb; g_em.fps = fps;
    const char* base = strrchr(path, '\\');
    snprintf(g_em.name, sizeof(g_em.name), "%s", base ? base + 1 : path);
    char* dot = strrchr(g_em.name, '.');
    if (dot) *dot = 0;
    // its sound
#ifdef _WIN32
    static const char* const ext[3] = { ".wav", ".mp3", ".ogg" };   // (Ogg needs codecs Windows often lacks: ffmpeg -i x.ogg x.wav)
    for (int e = 0; e < 3 && !g_em.pcm; e++) {
        char ap[600];
        snprintf(ap, sizeof(ap), "%.*s%s", (int)(strlen(path) - 6), path, ext[e]);
        FILE* t = fopen(ap, "rb");
        if (!t) continue;
        fclose(t);
        char full[600];
        if (!GetFullPathNameA(ap, sizeof(full), full, NULL)) snprintf(full, sizeof(full), "%s", ap);
        if (em_decode_audio(full)) printf("[emote] sound %s: %.1f s at %u Hz\n", ap, g_em.pcm_frames / (double)g_em.pcm_rate, g_em.pcm_rate);
        else printf("[emote] couldn't decode %s (Windows needs its media codecs for that format)\n", ap);
    }
#endif
    printf("[emote] \"%s\": %u frames at %g fps (%.1f s), %u bones -- F5 dances\n", g_em.name, frames, fps, frames / fps, nb);
    g_em.state = 2;
}
#ifdef _WIN32
static DWORD WINAPI em_load_thread(void* a) { (void)a; em_load(); return 0; }
#endif
void splat_emote_init(void) {
    if (g_em.state) return;
    g_em.state = 1;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, em_load_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else em_load();
#else
    em_load();
#endif
}

// --- starting / stopping ---
void splat_emote_toggle(void) {
    if (g_em.state != 2) { printf("[emote] F5: %s\n", g_em.state == 1 ? "still loading" : "no emote in emotes\\"); return; }
    double now = em_now();
    if (g_em_on && !g_em_stop) { g_em_stop = now; printf("[emote] stop\n"); return; }
    g_em_t0 = now;
    g_em_stop = 0;
    g_em_snd_pos = 0;
    g_em_on = 1;
    printf("[emote] \"%s\" (F5 again, moving, jumping or shooting stops it)\n", g_em.name);
}
// (input.c, every poll: moving or pressing jump / shoot / squid stops it)
void splat_emote_input(float lx, float ly, unsigned hold) {
    if (!g_em_on || g_em_stop) return;
    double now = em_now();
    if (now - g_em_t0 < 0.4) return;   // (the press that started it)
    if (lx * lx + ly * ly > 0.25f || (hold & (0x8000 | 0x4000 | 0x0080 | 0x0040))) { g_em_stop = now; printf("[emote] stopped by the controls\n"); }
}
// how much of the dance shows: 0..1 (0.3 s in, 0.3 s out)
static float em_weight(double now) {
    float w = (float)((now - g_em_t0) / 0.3);
    if (w > 1.0f) w = 1.0f;
    if (g_em_stop) {
        float o = 1.0f - (float)((now - g_em_stop) / 0.3);
        if (o < w) w = o;
    }
    return w < 0.0f ? 0.0f : w;
}

// --- the sound (audio.c's speaker thread adds it to what it hands the speakers) ---
void splat_emote_mix(float* out, u32 n, u32 channels, u32 rate, float vol) {
    if (!g_em_on || !g_em.pcm || !g_vk.emote_sound || !rate) return;
    double now = em_now(), step = (double)g_em.pcm_rate / (double)rate, pos = g_em_snd_pos;
    float w = em_weight(now);
    for (u32 i = 0; i < n; i++) {
        u32 a = (u32)pos;
        float t = (float)(pos - a);
        u32 b = a + 1 < g_em.pcm_frames ? a + 1 : 0;
        float l = g_em.pcm[a * 2] * (1 - t) + g_em.pcm[b * 2] * t, r = g_em.pcm[a * 2 + 1] * (1 - t) + g_em.pcm[b * 2 + 1] * t;
        out[i * channels] += l * vol * w * 0.35f;   // (0.35: full-scale music drowned the game, ~20x its level in the plaza)
        out[i * channels + 1] += r * vol * w * 0.35f;
        pos += step;
        if (pos >= g_em.pcm_frames) pos -= g_em.pcm_frames;   // (loops with the dance)
    }
    g_em_snd_pos = pos;
}

// --- which skeleton is the player's ---
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
static inline u32 em_ld(u32 a) { u32 v; memcpy(&v, wiiu_fastmem + a, 4); return FAST_BSWAP32(v); }
static inline float em_ldf(u32 a) { u32 v = em_ld(a); float f; memcpy(&f, &v, 4); return f; }
static inline void em_stf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); v = FAST_BSWAP32(v); memcpy(wiiu_fastmem + a, &v, 4); }
static inline void em_st(u32 a, u32 v) { v = FAST_BSWAP32(v); memcpy(wiiu_fastmem + a, &v, 4); }

// --- Inklings and follow cameras, for the Dojo bot (dojo_bot.c) ---
// While the bot runs (splat_track_on), the skeleton hook below lists every Inkling's position each frame and the
// camera hook every camera that follows a player (target 1 unit ahead). Written on the game's thread, read by the
// window's thread: each list is published whole at the next frame, behind a sequence number.
volatile int splat_track_on;
#define TR_MAX 8
typedef struct { u32 frame; int n; u32 id[TR_MAX]; float pos[TR_MAX][3]; float dir[TR_MAX][3]; } TrList;
static TrList g_tr_ink, g_tr_ink_pub, g_tr_cam, g_tr_cam_pub;
static volatile u32 g_tr_ink_seq, g_tr_cam_seq;
static struct { u32 fskl; int ink; } g_ik[64];
static int g_nik;
// (an Inkling: 40-80 bones, with hip sockets, shoulders and toes by those names -- without an emote loaded too)
static int ink_like(u32 skel) {
    u32 fskl = em_ld(skel);
    for (int i = 0; i < g_nik; i++) if (g_ik[i].fskl == fskl) return g_ik[i].ink;
    if (g_nik >= 64 || fskl < 0x10000000u || fskl >= 0x50000000u) return 0;
    int k = g_nik++;
    g_ik[k].fskl = fskl;
    g_ik[k].ink = 0;
    u32 nb = em_ld(skel + 44) >> 16, barr = em_ld(skel + 8);
    if (nb < 40 || nb > 80 || barr < 0x10000000u || barr >= 0x50000000u) return 0;
    static const char* const need[] = { "crotch_L", "shoulder_L", "toe_L" };
    int found = 0;
    for (int e = 0; e < 3; e++)
        for (u32 b = 0; b < nb; b++) {
            u32 ba = barr + b * 0x40, na = ba + em_ld(ba);
            if (na >= 0x10000000u && na < 0x50000000u && !strncmp((const char*)(wiiu_fastmem + na), need[e], 32)) { found++; break; }
        }
    g_ik[k].ink = found == 3;
    return g_ik[k].ink;
}
static void tr_add(TrList* cur, TrList* pub, volatile u32* seq, u32 id, const float pos[3], const float dir[3]) {
    u32 f = gx2_frame_count();
    if (cur->frame != f) {
        if (cur->n) { (*seq)++; *pub = *cur; (*seq)++; }
        cur->frame = f;
        cur->n = 0;
    }
    int i = 0;
    while (i < cur->n && cur->id[i] != id) i++;
    if (i == cur->n) { if (i == TR_MAX) return; cur->n++; }
    cur->id[i] = id;
    memcpy(cur->pos[i], pos, 12);
    if (dir) memcpy(cur->dir[i], dir, 12);
}
static int tr_read(const TrList* pub, volatile u32* seq, u32 id[], float pos[][3], float dir[][3], int max, u32* frame) {
    TrList c;
    for (int tries = 0; tries < 100; tries++) {
        u32 s = *seq;
        if (s & 1) continue;
        c = *pub;
        if (*seq == s) {
            int n = c.n < max ? c.n : max;
            for (int i = 0; i < n; i++) { id[i] = c.id[i]; memcpy(pos[i], c.pos[i], 12); if (dir) memcpy(dir[i], c.dir[i], 12); }
            if (frame) *frame = c.frame;
            return n;
        }
    }
    return 0;
}
int splat_track_inklings(u32 id[], float pos[][3], int max, u32* frame) { return tr_read(&g_tr_ink_pub, &g_tr_ink_seq, id, pos, NULL, max, frame); }
int splat_track_cams(u32 id[], float pos[][3], float dir[][3], int max, u32* frame) { return tr_read(&g_tr_cam_pub, &g_tr_cam_seq, id, pos, dir, max, frame); }

// The sun: the game's light-direction cameras sit at the origin aiming along the light (the plaza: -0.4, -0.8,
// -0.5, several of them -- the shadow cascades); a downward one, not along an axis (the GamePad map's and the
// ink maps' top-down cameras are), is the sun's light (latte_render.c's sky puts its sun there).
static float g_sun_dir[3];
static u32 g_sun_frame;
int splat_sun_now(float d[3]) {
    memcpy(d, g_sun_dir, 12);
    return g_sun_frame && g_sun_frame + 120 >= gx2_frame_count();
}
// (native_funcs.c, sead::LookAtCamera updates: r3 = the camera, position +52, target +64)
void splat_emote_camera(u32 cam) {
    if (!wiiu_fastmem || cam < 0x10000000u || cam >= 0x50000000u) return;
    float p[3], t[3];
    for (int i = 0; i < 3; i++) { p[i] = em_ldf(cam + 52 + 4 * i); t[i] = em_ldf(cam + 64 + 4 * i); }
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
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("SPLAT_EMOTE_DEBUG") != NULL;
    if (dbg) {   // (SPLAT_EMOTE_DEBUG=1: the cameras, every 300 frames)
        static struct { u32 cam, n; float p[3], t[3]; } cams[32];
        static int nc;
        static u32 last;
        int k = 0;
        while (k < nc && cams[k].cam != cam) k++;
        if (k == nc && nc < 32) { cams[nc].cam = cam; cams[nc].n = 0; nc++; }
        if (k < nc) { cams[k].n++; memcpy(cams[k].p, p, 12); memcpy(cams[k].t, t, 12); }
        u32 f = gx2_frame_count();
        if (f >= last + 300) {
            last = f;
            for (int i = 0; i < nc; i++)
                printf("[emote] camera %08X x%u: pos %.1f %.1f %.1f target %.1f %.1f %.1f (%.1f away)\n", cams[i].cam, cams[i].n, cams[i].p[0], cams[i].p[1], cams[i].p[2],
                       cams[i].t[0], cams[i].t[1], cams[i].t[2],
                       sqrtf((cams[i].p[0] - cams[i].t[0]) * (cams[i].p[0] - cams[i].t[0]) + (cams[i].p[1] - cams[i].t[1]) * (cams[i].p[1] - cams[i].t[1]) + (cams[i].p[2] - cams[i].t[2]) * (cams[i].p[2] - cams[i].t[2])));
            nc = 0;
        }
    }
    // (the camera that follows the player keeps its target 1 unit ahead: a direction; the HUD's are 989 away,
    // fixed views of the plaza 20-300, the light directions sit at the origin)
    if (p[0] == 0.0f && p[1] == 0.0f && t[0] == 0.0f && t[1] == 0.0f && t[2] == 0.0f && p[2] >= 100.0f) {   // (a layout camera: its distance marks the HUD's draws)
        extern float splat_hud_dist[8];
        extern volatile int splat_hud_ndist;
        int k = 0;
        while (k < splat_hud_ndist && fabsf(splat_hud_dist[k] - p[2]) > 0.05f) k++;
        if (k == splat_hud_ndist && k < 8) { splat_hud_dist[k] = p[2]; splat_hud_ndist = k + 1; }
        return;
    }
    if (!(d2 > 0.25f && d2 < 4.0f) || p[0] * p[0] + p[1] * p[1] + p[2] * p[2] < 1.0f) return;
    float l = sqrtf(d2);
    if (splat_track_on) { float dd[3] = { (t[0] - p[0]) / l, (t[1] - p[1]) / l, (t[2] - p[2]) / l }; tr_add(&g_tr_cam, &g_tr_cam_pub, &g_tr_cam_seq, cam, p, dd); }
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

float splat_hud_dist[8];
volatile int splat_hud_ndist;
void splat_emote_camera_now(float pos[3], float dir[3], int* fresh) {
    memcpy(pos, g_cam_pos, 12);
    memcpy(dir, g_cam_dir, 12);
    *fresh = g_cam_frame && g_cam_frame + 60 >= gx2_frame_count();
}

#define EM_SKEL_TYPES 256
// The bones between the driven ones that the emote file doesn't carry: the hip sockets the legs hang from, the
// shoulders the arms hang from, the toes. The converter worked the limbs out with these at rest; left to the
// game's own animation (a level's weapon stance bends them a lot) the dancing legs and arms were turned with
// them -- legs up at the arms, folded joints. During a dance they're held at rest.
static const char* const k_em_pin[] = { "crotch_L", "crotch_R", "shoulder_L", "shoulder_R", "toe_L", "toe_R" };
#define EM_NPIN 6
static struct { u32 fskl; int ink; s16 map[EM_MAX_BONES]; float rest_root[3]; s16 pin[EM_NPIN]; float pin_q[EM_NPIN][4]; } g_types[EM_SKEL_TYPES];
static int g_ntypes;
// (an Inkling skeleton: has every bone the emote drives)
static int em_type(u32 skel) {
    u32 fskl = em_ld(skel);
    for (int i = 0; i < g_ntypes; i++) if (g_types[i].fskl == fskl) return i;
    if (g_ntypes >= EM_SKEL_TYPES || fskl < 0x10000000u || fskl >= 0x50000000u) return -1;
    int k = g_ntypes++;
    g_types[k].fskl = fskl;
    g_types[k].ink = 0;
    u32 nb = em_ld(skel + 44) >> 16, barr = em_ld(skel + 8);
    if (nb < 40 || nb > 80 || barr < 0x10000000u || barr >= 0x50000000u) return k;
    int found = 0;
    for (u32 e = 0; e < g_em.nbones; e++) {
        g_types[k].map[e] = -1;
        for (u32 b = 0; b < nb; b++) {
            u32 ba = barr + b * 0x40;
            u32 na = ba + em_ld(ba);
            if (na < 0x10000000u || na >= 0x50000000u) continue;
            if (!strncmp((const char*)(wiiu_fastmem + na), g_em.bone[e], 32)) {
                g_types[k].map[e] = (s16)b;
                if (!strcmp(g_em.bone[e], "joint_root")) for (int c = 0; c < 3; c++) g_types[k].rest_root[c] = em_ldf(ba + 0x30 + 4 * c);
                found++;
                break;
            }
        }
    }
    for (int e = 0; e < EM_NPIN; e++) {   // (the bones held at rest: their rest rotation from the skeleton's own data)
        g_types[k].pin[e] = -1;
        for (u32 b = 0; b < nb; b++) {
            u32 ba = barr + b * 0x40;
            u32 na = ba + em_ld(ba);
            if (na < 0x10000000u || na >= 0x50000000u || strncmp((const char*)(wiiu_fastmem + na), k_em_pin[e], 32)) continue;
            float r[4] = { em_ldf(ba + 0x20), em_ldf(ba + 0x24), em_ldf(ba + 0x28), em_ldf(ba + 0x2C) };
            float* q = g_types[k].pin_q[e];
            if ((em_ld(ba + 0x10) >> 12) & 1) {   // (Euler X, Y, Z: q = z * y * x)
                float cx = cosf(r[0] * 0.5f), sx = sinf(r[0] * 0.5f), cy = cosf(r[1] * 0.5f), sy = sinf(r[1] * 0.5f), cz = cosf(r[2] * 0.5f), sz = sinf(r[2] * 0.5f);
                q[0] = sx * cy * cz - cx * sy * sz;
                q[1] = cx * sy * cz + sx * cy * sz;
                q[2] = cx * cy * sz - sx * sy * cz;
                q[3] = cx * cy * cz + sx * sy * sz;
            } else memcpy(q, r, 16);
            g_types[k].pin[e] = (s16)b;
            break;
        }
    }
    g_types[k].ink = found == (int)g_em.nbones;
    if (!g_types[k].ink && nb >= 60) printf("[emote] skeleton %08X (%u bones): %d of the emote's %u bones found\n", fskl, nb, found, g_em.nbones);
    if (g_types[k].ink) printf("[emote] Inkling skeleton %08X (%u bones)\n", fskl, nb);
    return k;
}

static u32 g_player, g_best, g_frame;
static float g_player_pos[3];
static u32 g_player_pos_frame;
// (freecam.c keeps the free camera near the player)
void splat_player_pos(float pos[3], int* fresh) {
    memcpy(pos, g_player_pos, 12);
    *fresh = g_player_pos_frame && g_player_pos_frame + 120 >= gx2_frame_count();
}
static float g_best_d, g_player_d;
static bool g_player_seen;

static void q_from_rows(const float r[9], float q[4]) {
    float tr = r[0] + r[4] + r[8];
    if (tr > 0) { float s = sqrtf(tr + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (r[7] - r[5]) / s; q[1] = (r[2] - r[6]) / s; q[2] = (r[3] - r[1]) / s; }
    else if (r[0] > r[4] && r[0] > r[8]) { float s = sqrtf(1.0f + r[0] - r[4] - r[8]) * 2; q[3] = (r[7] - r[5]) / s; q[0] = 0.25f * s; q[1] = (r[1] + r[3]) / s; q[2] = (r[2] + r[6]) / s; }
    else if (r[4] > r[8]) { float s = sqrtf(1.0f + r[4] - r[0] - r[8]) * 2; q[3] = (r[2] - r[6]) / s; q[0] = (r[1] + r[3]) / s; q[1] = 0.25f * s; q[2] = (r[5] + r[7]) / s; }
    else { float s = sqrtf(1.0f + r[8] - r[0] - r[4]) * 2; q[3] = (r[3] - r[1]) / s; q[0] = (r[2] + r[6]) / s; q[1] = (r[5] + r[7]) / s; q[2] = 0.25f * s; }
}

// A bone's local rotation set to q (x, y, z, w), blended with the game's own by w.
static bool em_set_rot(u32 L, const float q_in[4], float w) {
    float q[4] = { q_in[0], q_in[1], q_in[2], q_in[3] };
    if (w < 1.0f) {
        float r[9], qg[4];
        for (int row = 0; row < 3; row++) for (int c = 0; c < 3; c++) r[row * 3 + c] = em_ldf(L + 16 + row * 16 + c * 4);
        q_from_rows(r, qg);
        if (qg[0] * q[0] + qg[1] * q[1] + qg[2] * q[2] + qg[3] * q[3] < 0) for (int c = 0; c < 4; c++) qg[c] = -qg[c];
        for (int c = 0; c < 4; c++) q[c] = qg[c] + (q[c] - qg[c]) * w;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-6f) return false;
    float x = q[0] / l, y = q[1] / l, z = q[2] / l, qw = q[3] / l;
    float m[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * qw), 2 * (x * z + y * qw),
                   2 * (x * y + z * qw), 1 - 2 * (x * x + z * z), 2 * (y * z - x * qw),
                   2 * (x * z - y * qw), 2 * (y * z + x * qw), 1 - 2 * (x * x + y * y) };
    for (int row = 0; row < 3; row++) for (int c = 0; c < 3; c++) em_stf(L + 16 + row * 16 + c * 4, m[row * 3 + c]);
    return true;
}

// The hook (game address 02D26D80): 0 = the game's own code runs on.
int splat_native_skel_world(CPUState* ctx) {
    if (!wiiu_fastmem) return 0;
    if (splat_track_on) {   // (the Dojo bot: where every Inkling is)
        u32 sk = ctx->gpr[3], bs = ctx->gpr[4];
        if (sk >= 0x10000000u && sk < 0x50000000u && bs >= 0x10000000u && bs < 0x50000000u && ink_like(sk)) {
            float ps[3] = { em_ldf(bs + 12), em_ldf(bs + 28), em_ldf(bs + 44) };
            tr_add(&g_tr_ink, &g_tr_ink_pub, &g_tr_ink_seq, sk, ps, NULL);
        }
    }
    if (g_em.state != 2) return 0;
    {   // (a word in the log the first times: is the hook reached, which skeletons)
        static u32 calls;
        if (++calls == 2000) printf("[emote] skeleton hook: %u calls, %d skeleton kinds seen\n", calls, g_ntypes);
    }
    u32 skel = ctx->gpr[3], base = ctx->gpr[4];
    if (skel < 0x10000000u || skel >= 0x50000000u || base < 0x10000000u || base >= 0x50000000u) return 0;
    int ty = em_type(skel);
    if (ty < 0 || !g_types[ty].ink) return 0;
    u32 f = gx2_frame_count();
    if (f != g_frame) {   // (a new frame: last frame's nearest skeleton becomes the player's)
        extern volatile int splat_freecam_on;
        bool locked = (g_em_on || splat_freecam_on) && g_player && g_player_seen;   // (mid-dance or free camera: stay on the same Inkling)
        if (g_best && !locked && (!g_player || !g_player_seen || g_best == g_player || g_best_d + 4.0f < g_player_d)) {
            if (g_best != g_player) printf("[emote] the player's skeleton: %08X\n", g_best);
            g_player = g_best;
        }
        g_frame = f;
        g_best = 0;
        g_best_d = 1e30f;
        g_player_seen = false;
    }
    float pos[3] = { em_ldf(base + 12), em_ldf(base + 28), em_ldf(base + 44) };
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
    {
        static int dbg = -1;
        static u32 last;
        if (dbg < 0) dbg = getenv("SPLAT_EMOTE_DEBUG") != NULL;
        if (dbg && f >= last + 300 && f < last + 302) printf("[emote] Inkling %08X at %.1f %.1f %.1f: %.1f from the camera (in view)%s\n", skel, pos[0], pos[1], pos[2], d, skel == g_player ? " (picked)" : "");
        if (dbg && f >= last + 302) last = f;
    }
    if (skel == g_player) { g_player_d = d; g_player_seen = true; memcpy(g_player_pos, pos, sizeof(pos)); g_player_pos_frame = f; }
    if (!g_em_on || skel != g_player) return 0;
    double now = em_now();
    float w = em_weight(now);
    if (g_em_stop && w <= 0.0f) { g_em_on = 0; return 0; }
    // the frame: looping, between two of the clip's
    double ft = (now - g_em_t0) * g_em.fps;
    u32 fa = (u32)fmod(ft, (double)g_em.frames), fb = fa + 1 < g_em.frames ? fa + 1 : 0;
    float t = (float)(ft - floor(ft));
    size_t per = 3 + 4 * (size_t)g_em.nbones;
    const float *A = g_em.data + fa * per, *B = g_em.data + fb * per, *F0 = g_em.data;
    u32 local = em_ld(skel + 12);
    for (u32 e = 0; e < g_em.nbones; e++) {
        int bi = g_types[ty].map[e];
        if (bi < 0) continue;
        u32 L = local + (u32)bi * 64;
        // the emote's rotation (nlerp between the two frames), blended with the game's own
        float qa[4], qb[4], q[4];
        memcpy(qa, A + 3 + 4 * e, 16);
        memcpy(qb, B + 3 + 4 * e, 16);
        if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0) for (int c = 0; c < 4; c++) qb[c] = -qb[c];
        for (int c = 0; c < 4; c++) q[c] = qa[c] + (qb[c] - qa[c]) * t;
        if (!em_set_rot(L, q, w)) continue;
        if (!strcmp(g_em.bone[e], "joint_root")) {   // (its height / sway: the clip's change since its first frame, on this skeleton's own rest)
            for (int c = 0; c < 3; c++) {
                float em = g_types[ty].rest_root[c] + (A[c] + (B[c] - A[c]) * t) - F0[c];
                float cur = em_ldf(L + 16 + c * 16 + 12);
                em_stf(L + 16 + c * 16 + 12, cur + (em - cur) * w);
            }
        }
        em_st(L, em_ld(L) & ~0x3F000000u);   // (not "rotation / translation zero" any more: the game's quick paths skip it)
    }
    static int pin_off = -1;   // (SPLAT_EMOTE_NO_PIN=1: the sockets / shoulders / toes left to the game, as before)
    if (pin_off < 0) pin_off = getenv("SPLAT_EMOTE_NO_PIN") != NULL;
    for (int e = 0; e < EM_NPIN && !pin_off; e++) {
        int bi = g_types[ty].pin[e];
        if (bi < 0) continue;
        u32 L = local + (u32)bi * 64;
        if (em_set_rot(L, g_types[ty].pin_q[e], w)) em_st(L, em_ld(L) & ~0x3F000000u);
    }
    return 0;
}
