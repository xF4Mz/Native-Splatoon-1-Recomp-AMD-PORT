// The Wii U's sound library, AX (snd_core): voices, the mixer, and the PC's
// speakers.
//
// On the console AX mixes audio in 3 ms frames and, after each one, calls
// the "frame callbacks" the game registered. Splatoon's sound engine
// (nw::snd) does all its regular work from that callback: starting and
// stopping voices, updating volumes and pitch, handing data-loading jobs to
// its task thread. So every 3 ms we call that callback, then mix every
// playing voice ourselves (what the console's DSP does):
//   - read its samples from game memory (DSP-ADPCM, 16-bit or 8-bit PCM),
//   - resample it to the mixer's rate (32 kHz, what AXInit sets up),
//   - apply its volume and its TV / GamePad channel volumes,
// then hand the mix to the game's final-mix callbacks, mix it down to
// stereo, convert it to the speakers' rate and queue it for the sound card
// (WASAPI, on its own thread). The queue sits ~60 ms ahead of the speakers,
// so a late frame now and then doesn't make the sound stutter.
#include "audio.h"
#include "guest_threads.h"
#include "host_thread.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#pragma comment(lib, "ole32.lib")

#define AX_FRAME_TICKS (WIIU_TIMER_HZ * 3 / 1000)   // 3 ms
#define AX_RATE 32000                               // the mixer's rate
#define AX_SAMPLES 96                               // per 3 ms frame at 32 kHz
#define AX_MAX_VOICES 96
#define AX_VOICE_BYTES 0x58                         // sizeof(AXVoice)

static u32 g_app_callback = 0;       // AXRegisterAppFrameCallback
static u32 g_frame_callback = 0;     // AXRegisterFrameCallback (older API)
static u32 g_final_mix[4] = { 0 };   // per output device (TV, GamePad, ...)
static bool g_init = false;

// ---------------------------------------------------------------------------
// Voices. The game gets an AXVoice in its own memory (it reads the index and
// the state from it); everything the mixer needs lives here.

enum { FMT_ADPCM = 0, FMT_PCM16 = 10, FMT_PCM8 = 25 };

typedef struct {
    u32 obj;                      // the game's AXVoice (0 = free)
    u32 priority;
    bool playing;
    bool stream;                  // voice type 1: the loop doesn't reload the decoder state
    bool loop;
    bool primed;                  // first sample fetched since it (re)started
    u16 format;
    u32 base;                     // sample data the offsets count from
    u32 loop_pos, end_pos, cur_pos;   // in samples (in 4-bit nibbles for ADPCM)
    u32 loops;
    s16 coef[16];                 // ADPCM decoder
    u16 ps;
    s16 yn1, yn2;
    s16 loop_yn1, loop_yn2;
    u32 frac, ratio;              // resampling: position and step, 16.16
    bool src_none;
    s32 prev, cur;                // the last two source samples (interpolated between)
    bool lpf_on;                  // one-pole low-pass filter
    s32 lpf_a0, lpf_b0, lpf_y;
    s32 ve, ve_delta;             // voice volume (0x8000 = 1.0), change per sample
    s32 tv[4][6], tv_d[4][6];     // TV volumes per bus (0 = main, 1-3 = effect buses A-C) and channel
    s32 drc[4][4], drc_d[4][4];   // GamePad volumes per bus and channel
} Voice;

static Voice g_voices[AX_MAX_VOICES];
static u32 g_route_tv, g_route_drc, g_route_drc_only;   // (voice-frames sent to each, for the log)
static u32 g_voice_mem = 0;       // the AXVoice array in game memory
static hmtx_t g_vlock;
static bool g_vlock_ready = false;
static void vlock(void) {
    if (!g_vlock_ready) { hmtx_init(&g_vlock, true); g_vlock_ready = true; }
    hmtx_lock(&g_vlock);
}
static void vunlock(void) { hmtx_unlock(&g_vlock); }

static Voice* voice_of(u32 obj) {
    if (!g_voice_mem || obj < g_voice_mem) return NULL;
    u32 i = (obj - g_voice_mem) / AX_VOICE_BYTES;
    if (i >= AX_MAX_VOICES || g_voices[i].obj != obj) return NULL;
    return &g_voices[i];
}

// (to - from) in the format's units: nibbles for ADPCM, samples for PCM
static s64 units_between(u16 fmt, u32 from, u32 to) {
    s64 d = (s64)to - (s64)from;
    return fmt == FMT_ADPCM ? d * 2 : fmt == FMT_PCM16 ? d / 2 : d;
}

static void set_state(Voice* v, bool playing) {
    if (playing && !v->playing) v->primed = false;
    v->playing = playing;
    wiiu_write32(v->obj + 4, playing ? 1 : 0);
}

// The sample at the voice's position, then step on (looping or stopping at
// the end, as the DSP does).
static s32 fetch(Voice* v) {
    u32 p = v->cur_pos;
    s32 s;
    if (v->format == FMT_PCM16) {
        s = (s16)wiiu_read16(v->base + p * 2);
    } else if (v->format == FMT_PCM8) {
        s = (s32)(s8)wiiu_read8(v->base + p) << 8;
    } else {
        // DSP-ADPCM: 8-byte frames, a header byte (coefficient pair, scale)
        // and 14 4-bit samples; offsets count nibbles, the header included.
        if ((p & 15) < 2) {
            v->ps = wiiu_read8(v->base + ((p & ~15u) >> 1));
            p = (p & ~15u) + 2;
        }
        u8 b = wiiu_read8(v->base + (p >> 1));
        s32 n = (p & 1) ? (b & 15) : (b >> 4);
        if (n >= 8) n -= 16;
        u32 ci = (v->ps >> 4) & 7;
        s64 x = ((s64)(n << (v->ps & 15)) << 11) + (s64)v->coef[ci * 2] * v->yn1 + (s64)v->coef[ci * 2 + 1] * v->yn2;
        x = (x + 1024) >> 11;
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        v->yn2 = v->yn1;
        v->yn1 = (s16)x;
        s = (s32)x;
    }
    // Exactly at the end, as the DSP checks: streams move the end to the
    // block the voice has just entered a frame after it got there, so a
    // position past an old end is normal (">=" stuck the music for 3 ms at
    // every block -- it cut in and out).
    if (p == v->end_pos) {
        if (v->loop) {
            v->cur_pos = v->loop_pos;
            v->loops++;
            if (v->format == FMT_ADPCM) {
                if (!v->stream) { v->yn1 = v->loop_yn1; v->yn2 = v->loop_yn2; }
                if ((v->cur_pos & 15) >= 2) v->ps = wiiu_read8(v->base + ((v->cur_pos & ~15u) >> 1));
            }
        } else {
            v->cur_pos = p;
            set_state(v, false);
            static int dbg = -1;   // (SPLAT_AX_STOPS=n: log the first n voices that ran off their end)
            if (dbg < 0) { const char* e = getenv("SPLAT_AX_STOPS"); dbg = e ? atoi(e) : 0; }
            if (dbg > 0) {
                dbg--;
                printf("[ax] voice %08X ended: %s fmt %u base %08X loop %u end %u pos %u, loops %u\n", v->obj, v->stream ? "stream" : "normal",
                       v->format, v->base, v->loop_pos, v->end_pos, p, v->loops);
            }
        }
    } else {
        v->cur_pos = p + 1;
    }
    return s;
}

static s32 clamp_vol(s32 v) { return v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v; }

static void add_scaled(s32* out, const s32* in, s32* vol_io, s32 d) {
    s32 vol = *vol_io;
    for (int i = 0; i < AX_SAMPLES; i++) { out[i] += (s32)(((s64)in[i] * vol) >> 15); vol = clamp_vol(vol + d); }
    *vol_io = vol;
}

// Mix one voice's next 3 ms into the TV and GamePad buses. A sound that
// only goes to the GamePad's speaker (map taps, super jump picks, lobby
// minigames) is also added to `pc` (the PC hears the TV mix plus these).
static void mix_voice(Voice* v, s32 tv[4][6][AX_SAMPLES], s32 drc[4][4][AX_SAMPLES], s32 pc[4][AX_SAMPLES]) {
    if (!v->primed) {
        v->cur = fetch(v);
        v->prev = v->cur;
        v->frac = 0;
        v->lpf_y = 0;
        v->primed = true;
    }
    s32 buf[AX_SAMPLES];
    s32 ve = v->ve;
    for (int i = 0; i < AX_SAMPLES; i++) {
        s32 s = v->src_none ? v->cur : v->prev + (s32)(((s64)(v->cur - v->prev) * (s64)v->frac) >> 16);
        if (v->lpf_on) { v->lpf_y = (v->lpf_a0 * s + v->lpf_b0 * v->lpf_y) >> 15; s = v->lpf_y; }
        buf[i] = (s32)(((s64)s * ve) >> 15);
        ve = clamp_vol(ve + v->ve_delta);
        v->frac += v->ratio;
        while (v->frac >= 0x10000u) {
            v->frac -= 0x10000u;
            v->prev = v->cur;
            v->cur = v->playing ? fetch(v) : 0;
        }
    }
    v->ve = ve;
    bool on_tv = false, on_drc = false;
    for (int b = 0; b < 4; b++) {
        for (int c = 0; c < 6; c++) if (v->tv[b][c] || v->tv_d[b][c]) on_tv = true;
        for (int c = 0; c < 4; c++) if (v->drc[b][c] || v->drc_d[b][c]) on_drc = true;
    }
    if (on_tv) g_route_tv++;
    if (on_drc) g_route_drc++;
    if (on_drc && !on_tv) g_route_drc_only++;
    for (int b = 0; b < 4; b++)
        for (int c = 0; c < 6; c++)
            if (v->tv[b][c] || v->tv_d[b][c]) add_scaled(tv[b][c], buf, &v->tv[b][c], v->tv_d[b][c]);
    for (int c = 0; c < 4; c++) {
        if (!on_tv && (v->drc[0][c] || v->drc_d[0][c])) { s32 vol = v->drc[0][c]; add_scaled(pc[c], buf, &vol, v->drc_d[0][c]); }
        for (int b = 0; b < 4; b++)
            if (v->drc[b][c] || v->drc_d[b][c]) add_scaled(drc[b][c], buf, &v->drc[b][c], v->drc_d[b][c]);
    }
}

// ---------------------------------------------------------------------------
// Speaker output: a queue of stereo float frames at the sound card's rate,
// filled every 3 ms by the mixer and drained by the WASAPI thread.

#define OUT_QUEUE 32768                 // frames (~0.7 s at 48 kHz)
static float g_q[OUT_QUEUE][2];
static volatile LONG g_q_read = 0, g_q_write = 0;   // (one writer, one reader)
static u32 g_out_rate = 48000;
static volatile LONG g_out_ready = 0;
static u32 g_target = 2880;             // frames the queue aims to hold (60 ms at 48 kHz)
static volatile LONG g_underruns = 0;

static u32 q_fill(void) { return (u32)(g_q_write - g_q_read) & (OUT_QUEUE - 1); }

static void q_push(float l, float r) {
    LONG w = g_q_write;
    if ((((u32)(w + 1)) & (OUT_QUEUE - 1)) == ((u32)g_q_read & (OUT_QUEUE - 1))) return;   // full: drop
    g_q[w & (OUT_QUEUE - 1)][0] = l;
    g_q[w & (OUT_QUEUE - 1)][1] = r;
    MemoryBarrier();
    g_q_write = (w + 1) & (OUT_QUEUE - 1);
}

static const GUID kCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

// The Volume setting (percent) as a factor; SPLAT_VOLUME=N at start.
extern volatile int splat_volume_pct;
static float audio_volume(void) {
    static int env = -1;
    if (env < 0) { env = 0; const char* e = getenv("SPLAT_VOLUME"); if (e) splat_volume_pct = atoi(e); }
    int v = splat_volume_pct;
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return (float)v / 100.0f;
}
// One session with the default speakers; returns when the device goes away
// (unplugged, switched) so the caller can open the new default.
static void out_session(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioClient* ac = NULL;
    IAudioRenderClient* rc = NULL;
    WAVEFORMATEX* mix = NULL;
    HANDLE ev = NULL;
    if (FAILED(CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &kIID_IMMDeviceEnumerator, (void**)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) goto done;
    if (FAILED(IMMDevice_Activate(dev, &kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&ac))) goto done;
    if (FAILED(IAudioClient_GetMixFormat(ac, &mix))) goto done;
    // Float samples at the device's own rate and channel count (what the
    // shared mixer takes without converting); we fill the first two channels.
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32 &&
                     ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
    if (!is_float || mix->nChannels < 2) {
        printf("[audio] speakers want a format we don't write (tag %u, %u channels, %u bits)\n", mix->wFormatTag, mix->nChannels, mix->wBitsPerSample);
        goto done;
    }
    u32 channels = mix->nChannels;
    ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 300000 /* 30 ms */, 0, mix, NULL))) goto done;
    UINT32 frames = 0;
    if (FAILED(IAudioClient_GetBufferSize(ac, &frames))) goto done;
    if (FAILED(IAudioClient_GetService(ac, &kIID_IAudioRenderClient, (void**)&rc))) goto done;
    IAudioClient_SetEventHandle(ac, ev);
    g_out_rate = mix->nSamplesPerSec;
    g_target = g_out_rate * 60 / 1000;
    InterlockedExchange(&g_out_ready, 1);
    printf("[audio] speakers: %u Hz, %u channels, %u-frame buffer\n", (unsigned)mix->nSamplesPerSec, channels, (unsigned)frames);
    IAudioClient_Start(ac);
    bool buffering = true;   // (wait until the queue holds its target before playing)
    for (;;) {
        WaitForSingleObject(ev, 200);
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 n = frames - pad;
        if (!n) continue;
        BYTE* p = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, n, &p))) break;
        float* f = (float*)p;
        memset(f, 0, (size_t)n * channels * sizeof(float));
        u32 have = q_fill();
        if (buffering && have >= g_target) buffering = false;
        if (!buffering) {
            u32 take = have < n ? have : n;
            LONG r = g_q_read;
            float vol = audio_volume();   // (the settings panel's Volume row)
            for (u32 i = 0; i < take; i++) {
                f[i * channels + 0] = g_q[(r + i) & (OUT_QUEUE - 1)][0] * vol;
                f[i * channels + 1] = g_q[(r + i) & (OUT_QUEUE - 1)][1] * vol;
            }
            MemoryBarrier();
            g_q_read = (r + take) & (OUT_QUEUE - 1);
            if (take < n) { InterlockedIncrement(&g_underruns); buffering = true; }
        }
        { extern void splat_emote_mix(float* out, u32 n, u32 channels, u32 rate, float vol); splat_emote_mix(f, n, channels, g_out_rate, audio_volume()); }
        { extern void splat_rain_mix(float* out, u32 n, u32 channels, u32 rate, float vol); splat_rain_mix(f, n, channels, g_out_rate, audio_volume()); }   // (the Storm sky's rain, rain_sound.c)
        {   // (SPLAT_AUDIO_OUT_WAV=file: what the speakers were given, 16-bit stereo at their rate)
            static FILE* wf = NULL;
            static int wstate = -1;
            static u32 wframes = 0;
            if (wstate < 0) {
                const char* e = getenv("SPLAT_AUDIO_OUT_WAV");
                wstate = e && (wf = fopen(e, "wb")) != NULL;
                if (wstate) {
                    u32 rate = g_out_rate, bps = rate * 4;
                    u8 h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                                 (u8)rate, (u8)(rate >> 8), (u8)(rate >> 16), 0, (u8)bps, (u8)(bps >> 8), (u8)(bps >> 16), 0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
                    fwrite(h, 1, 44, wf);
                }
            }
            if (wstate) {
                for (u32 i = 0; i < n; i++) {
                    s16 o[2] = { (s16)(f[i * channels] * 32767.0f), (s16)(f[i * channels + 1] * 32767.0f) };
                    fwrite(o, 2, 2, wf);
                }
                wframes += n;
                long pos = ftell(wf);
                u32 data = wframes * 4, riff = data + 36;
                fseek(wf, 4, SEEK_SET); fwrite(&riff, 4, 1, wf);
                fseek(wf, 40, SEEK_SET); fwrite(&data, 4, 1, wf);
                fseek(wf, pos, SEEK_SET);
            }
        }
        IAudioRenderClient_ReleaseBuffer(rc, n, 0);
    }
    IAudioClient_Stop(ac);
done:
    InterlockedExchange(&g_out_ready, 0);
    if (rc) IAudioRenderClient_Release(rc);
    if (ac) IAudioClient_Release(ac);
    if (mix) CoTaskMemFree(mix);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (ev) CloseHandle(ev);
}

static int out_thread(void* arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (int tries = 0;; tries++) {
        out_session();
        if (tries == 0) printf("[audio] speaker output stopped; reopening\n");
        Sleep(1000);
    }
    return 0;
}

static void out_start(void) {
    static bool started = false;
    if (started) return;
    started = true;
    if (getenv("SPLAT_NO_SOUND")) { printf("[audio] SPLAT_NO_SOUND: no speaker output\n"); return; }
    hthrd_t t;
    hthrd_create(&t, out_thread, NULL, 256 * 1024);
}

// ---------------------------------------------------------------------------
// The 3 ms frame.

// The final mix handed to the game's final-mix callbacks after each frame
// (as AX does): per device a small header -- the channel buffer pointers,
// channel count, samples per channel, device count -- and the buffers
// (32-bit samples).
static const u32 g_dev_channels[2] = { 6, 4 };   // TV (5.1 layout), GamePad
static u32 g_fm_header[2], g_fm_ptrs[2], g_fm_buf[2];

static void final_mix_setup(void) {
    for (int d = 0; d < 2; d++) {
        u32 ch = g_dev_channels[d];
        g_fm_buf[d] = hle_heap_alloc(ch * 144 * 4, 64);
        g_fm_ptrs[d] = hle_heap_alloc(ch * 4, 16);
        g_fm_header[d] = hle_heap_alloc(16, 16);
        for (u32 c = 0; c < ch; c++) wiiu_write32(g_fm_ptrs[d] + c * 4, g_fm_buf[d] + c * 144 * 4);
        wiiu_write32(g_fm_header[d] + 0, g_fm_ptrs[d]);
        wiiu_write16(g_fm_header[d] + 4, (u16)ch);
        wiiu_write16(g_fm_header[d] + 6, AX_SAMPLES);
        wiiu_write16(g_fm_header[d] + 8, 1);
        wiiu_write16(g_fm_header[d] + 10, (u16)ch);
    }
}

// Hand a device's mix to the game's final-mix callback (it may change it).
static void final_mix_callback(int d, s32* mix /* [ch][AX_SAMPLES] */) {
    if (!g_final_mix[d]) return;
    u32 n = g_dev_channels[d];
    for (u32 c = 0; c < n; c++) {
        u8* p = wiiu_mem_ptr(g_fm_buf[d] + c * 144 * 4, AX_SAMPLES * 4);
        if (!p) return;
        for (int i = 0; i < AX_SAMPLES; i++) { u32 w = _byteswap_ulong((u32)mix[c * AX_SAMPLES + i]); memcpy(p + i * 4, &w, 4); }
    }
    threads_call_guest_from_timer(g_final_mix[d], g_fm_header[d], 0);
    for (u32 c = 0; c < n; c++) {
        u8* p = wiiu_mem_ptr(g_fm_buf[d] + c * 144 * 4, AX_SAMPLES * 4);
        for (int i = 0; i < AX_SAMPLES; i++) { u32 w; memcpy(&w, p + i * 4, 4); mix[c * AX_SAMPLES + i] = (s32)_byteswap_ulong(w); }
    }
}

// SPLAT_AUDIO_WAV=file: also write the mix (32 kHz stereo, before the
// speakers' rate) to a .wav file, for checking the sound without listening.
static void wav_dump(const s32* tv, const s32* drc) {
    static FILE* f = NULL;
    static int state = -1;
    static u32 frames = 0;
    if (state < 0) {
        const char* e = getenv("SPLAT_AUDIO_WAV");
        state = 0;
        if (e && (f = fopen(e, "wb")) != NULL) {
            state = 1;
            u8 h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                         0x00,0x7D,0,0, 0x00,0xF4,0x01,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };   // 32000 Hz, 16-bit stereo
            fwrite(h, 1, 44, f);
        }
    }
    if (state != 1) return;
    s16 out[AX_SAMPLES * 2];
    for (int i = 0; i < AX_SAMPLES; i++) {
        float L = tv[0 * AX_SAMPLES + i] + 0.7071f * (tv[4 * AX_SAMPLES + i] + tv[2 * AX_SAMPLES + i]) + drc[0 * AX_SAMPLES + i];
        float R = tv[1 * AX_SAMPLES + i] + 0.7071f * (tv[4 * AX_SAMPLES + i] + tv[3 * AX_SAMPLES + i]) + drc[1 * AX_SAMPLES + i];
        out[i * 2] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        out[i * 2 + 1] = (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    fwrite(out, 4, AX_SAMPLES, f);
    frames += AX_SAMPLES;
    if ((frames % (AX_SAMPLES * 333)) == 0) {   // (keep the header's sizes current, about once a second)
        long pos = ftell(f);
        u32 data = frames * 4, riff = data + 36;
        fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&data, 4, 1, f);
        fseek(f, pos, SEEK_SET);
        fflush(f);
    }
}

static u64 g_frames, g_t0;
static double g_phase = 0.0, g_fill_avg = 0.0;
static float g_last_l = 0.0f, g_last_r = 0.0f;
static u32 g_playing_peak = 0;

// Effect buses (reverb etc.): each voice also sends to buses A-C; after the
// voices are mixed, AX hands each bus to the callback the game registered
// for it (nw::snd's effects, 02CF2548: (channel buffers, context,
// {channels, samples})), which processes it in place, and adds the result
// to the main mix at the bus's return volume.
static u32 g_aux_cb[3][3], g_aux_ctx[3][3];      // [device][bus]
static s32 g_aux_ret[3][3] = { { 0x8000, 0x8000, 0x8000 }, { 0x8000, 0x8000, 0x8000 }, { 0x8000, 0x8000, 0x8000 } };
static u32 g_aux_ptrs[2][3], g_aux_buf[2][3], g_aux_info[2][3];   // (in game memory)
static u32 g_aux_peak_in, g_aux_peak_out;   // (for the log)

static void aux_process(int d, int bus, s32* busbuf /* [ch][AX_SAMPLES] */, s32* main /* [ch][AX_SAMPLES] */) {
    u32 cb = g_aux_cb[d][bus];
    if (!cb) return;
    u32 n = g_dev_channels[d];
    if (!g_aux_buf[d][bus]) {
        g_aux_buf[d][bus] = hle_heap_alloc(n * AX_SAMPLES * 4, 64);
        g_aux_ptrs[d][bus] = hle_heap_alloc(n * 4, 16);
        g_aux_info[d][bus] = hle_heap_alloc(16, 16);
        for (u32 c = 0; c < n; c++) wiiu_write32(g_aux_ptrs[d][bus] + c * 4, g_aux_buf[d][bus] + c * AX_SAMPLES * 4);
        wiiu_write32(g_aux_info[d][bus] + 0, n);
        wiiu_write32(g_aux_info[d][bus] + 4, AX_SAMPLES);
    }
    u8* p = wiiu_mem_ptr(g_aux_buf[d][bus], n * AX_SAMPLES * 4);
    if (!p) return;
    for (u32 i = 0; i < n * AX_SAMPLES; i++) { u32 a = busbuf[i] < 0 ? (u32)-busbuf[i] : (u32)busbuf[i]; if (a > g_aux_peak_in) g_aux_peak_in = a; }
    for (u32 i = 0; i < n * AX_SAMPLES; i++) { u32 w = _byteswap_ulong((u32)busbuf[i]); memcpy(p + i * 4, &w, 4); }
    threads_call_guest_from_timer3(cb, g_aux_ptrs[d][bus], g_aux_ctx[d][bus], g_aux_info[d][bus]);
    s32 ret = g_aux_ret[d][bus];
    for (u32 i = 0; i < n * AX_SAMPLES; i++) {
        u32 w; memcpy(&w, p + i * 4, 4);
        { s32 o = (s32)_byteswap_ulong(w); u32 a = o < 0 ? (u32)-o : (u32)o; if (a > g_aux_peak_out) g_aux_peak_out = a; }
        main[i] += (s32)(((s64)(s32)_byteswap_ulong(w) * ret) >> 15);
    }
}

static u64 g_cost[4], g_cost_max;   // (ns spent: game callback, mixing, effects, final mix; slowest frame)

static void audio_frame(void) {
    g_frames++;
    u64 c0 = hnow_ns();
    if (g_app_callback) threads_call_guest_from_timer(g_app_callback, 0, 0);
    if (g_frame_callback && g_frame_callback != g_app_callback) threads_call_guest_from_timer(g_frame_callback, 0, 0);
    u64 c1 = hnow_ns();
    if (!g_fm_header[0]) final_mix_setup();

    static s32 tv[4][6][AX_SAMPLES], drc[4][4][AX_SAMPLES], pc[4][AX_SAMPLES];
    memset(tv, 0, sizeof(tv));
    memset(drc, 0, sizeof(drc));
    memset(pc, 0, sizeof(pc));
    u32 playing = 0;
    vlock();
    for (int i = 0; i < AX_MAX_VOICES; i++) {
        Voice* v = &g_voices[i];
        if (!v->obj || !v->playing) continue;
        playing++;
        mix_voice(v, tv, drc, pc);
    }
    vunlock();
    u64 c2 = hnow_ns();
    if (playing > g_playing_peak) g_playing_peak = playing;
    for (int b = 0; b < 3; b++) {
        aux_process(0, b, &tv[b + 1][0][0], &tv[0][0][0]);
        aux_process(1, b, &drc[b + 1][0][0], &drc[0][0][0]);
    }
    u64 c3 = hnow_ns();
    final_mix_callback(0, &tv[0][0][0]);
    final_mix_callback(1, &drc[0][0][0]);
    u64 c4 = hnow_ns();
    g_cost[0] += c1 - c0; g_cost[1] += c2 - c1; g_cost[2] += c3 - c2; g_cost[3] += c4 - c3;
    if (c4 - c0 > g_cost_max) g_cost_max = c4 - c0;

    wav_dump(&tv[0][0][0], &pc[0][0]);
    if (!g_out_ready) return;
    // Down to stereo (TV channels: left, right, surround left, surround
    // right, centre, LFE), plus the GamePad-only sounds, then on to the
    // speakers' rate. The step is nudged by up to 0.5% to keep the queue
    // at its target (the game's clock and the sound card's drift apart).
    float l[AX_SAMPLES + 1], r[AX_SAMPLES + 1];
    l[0] = g_last_l; r[0] = g_last_r;
    const float k = 1.0f / 32768.0f, c7 = 0.7071f;
    for (int i = 0; i < AX_SAMPLES; i++) {
        float L = tv[0][0][i] + c7 * (tv[0][4][i] + tv[0][2][i]) + pc[0][i] + c7 * pc[2][i];
        float R = tv[0][1][i] + c7 * (tv[0][4][i] + tv[0][3][i]) + pc[1][i] + c7 * pc[3][i];
        L *= k; R *= k;
        l[i + 1] = L < -1.0f ? -1.0f : L > 1.0f ? 1.0f : L;
        r[i + 1] = R < -1.0f ? -1.0f : R > 1.0f ? 1.0f : R;
    }
    g_last_l = l[AX_SAMPLES]; g_last_r = r[AX_SAMPLES];
    u32 fill = q_fill();
    g_fill_avg += ((double)fill - g_fill_avg) * 0.02;
    double adj = (g_fill_avg - (double)g_target) / (double)g_target * 0.01;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    double step = (double)AX_RATE / (double)g_out_rate * (1.0 + adj);
    while (g_phase < (double)AX_SAMPLES) {
        int i = (int)g_phase;
        float f = (float)(g_phase - i);
        q_push(l[i] + (l[i + 1] - l[i]) * f, r[i] + (r[i + 1] - r[i]) * f);
        g_phase += step;
    }
    g_phase -= (double)AX_SAMPLES;

    static u64 last_report = 0;
    if (g_frames - last_report >= 3333) {   // (about every 10 s)
        last_report = g_frames;
        printf("[audio] voices playing (peak) %u, queue %u ms, speaker underruns %ld | voice-frames to TV %u, GamePad %u (GamePad only %u) | effects on buses %s%s%s (peak in %u, out %u)\n",
               g_playing_peak, (unsigned)((u64)fill * 1000 / g_out_rate), (long)g_underruns, g_route_tv, g_route_drc, g_route_drc_only,
               g_aux_cb[0][0] ? "A" : "", g_aux_cb[0][1] ? "B" : "", g_aux_cb[0][2] ? "C" : "", g_aux_peak_in, g_aux_peak_out);
        printf("[audio] work per second of sound: game's sound engine %.0f ms, mixing %.0f ms, effects %.0f ms, final mix %.0f ms; slowest 3 ms frame %.1f ms\n",
               g_cost[0] / 1e7, g_cost[1] / 1e7, g_cost[2] / 1e7, g_cost[3] / 1e7, g_cost_max / 1e6);
        memset(g_cost, 0, sizeof(g_cost));
        g_cost_max = 0;
        g_aux_peak_in = g_aux_peak_out = 0;
        g_playing_peak = 0;
        g_route_tv = g_route_drc = g_route_drc_only = 0;
    }
}

// The timer thread's 3 ms tick: run as many frames as real time says are
// due (a tick that comes late runs the missed frames too, so no sound is lost).
static void audio_tick(void) {
    u64 now = wiiu_time_ticks();
    if (!g_t0) g_t0 = now - AX_FRAME_TICKS;
    u64 due = (now - g_t0) / AX_FRAME_TICKS;
    if (due > g_frames + 16) g_frames = due - 16;   // (a long stall: skip ahead)
    while (g_frames < due) audio_frame();
}

static void start_timer(u32 callback) {
    static bool started = false;
    if (!callback || started) return;
    started = true;
    out_start();
    threads_add_host_timer(audio_tick, AX_FRAME_TICKS);
    printf("[audio] sound frame every 3 ms (callback %08X)\n", callback);
}

// ---------------------------------------------------------------------------
// The system calls.

static void AXInit(CPUState* c) { g_init = true; (void)c; }
static void AXInitWithParams(CPUState* c) { g_init = true; (void)c; }
static void AXIsInit(CPUState* c) { ret(c, g_init ? 1 : 0); }
static void AXQuit(CPUState* c) { g_init = false; (void)c; }

// (callback) -> result 0 = success
static void AXRegisterAppFrameCallback(CPUState* c) {
    u32 cb = arg(c, 0);
    if (cb && cb != g_app_callback && cb != g_frame_callback) start_timer(cb);
    g_app_callback = cb;
    ret(c, 0);
}
static void AXDeregisterAppFrameCallback(CPUState* c) { ret(c, 0); }

// (callback) -> the previous callback
static void AXRegisterFrameCallback(CPUState* c) {
    u32 cb = arg(c, 0), old = g_frame_callback;
    if (cb && cb != g_frame_callback && cb != g_app_callback) start_timer(cb);
    g_frame_callback = cb;
    ret(c, old);
}

// (device, callback) / (device, &callback)
static void AXRegisterDeviceFinalMixCallback(CPUState* c) {
    u32 dev = arg(c, 0);
    if (dev < 4) g_final_mix[dev] = arg(c, 1);
    ret(c, 0);
}
static void AXGetDeviceFinalMixCallback(CPUState* c) {
    u32 dev = arg(c, 0), out = arg(c, 1);
    if (out) wiiu_write32(out, dev < 4 ? g_final_mix[dev] : 0);
    ret(c, 0);
}

// (device, &mode)
static void AXGetDeviceMode(CPUState* c) {
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 0);   // stereo
    ret(c, 0);
}
static void AXGetInputSamplesPerFrame(CPUState* c) { ret(c, AX_SAMPLES); }
static void AXGetInputSamplesPerSec(CPUState* c) { ret(c, AX_RATE); }
static void zero(CPUState* c) { ret(c, 0); }
// (device, device id, bus, callback, context)
static void AXRegisterAuxCallback(CPUState* c) {
    u32 dev = arg(c, 0), bus = arg(c, 2);
    if (dev < 3 && bus < 3 && arg(c, 1) == 0) { g_aux_cb[dev][bus] = arg(c, 3); g_aux_ctx[dev][bus] = arg(c, 4); }
    ret(c, 0);
}
// (device, device id, bus, &callback, &context)
static void AXGetAuxCallback(CPUState* c) {
    u32 dev = arg(c, 0), bus = arg(c, 2);
    bool ok = dev < 3 && bus < 3;
    if (arg(c, 3)) wiiu_write32(arg(c, 3), ok ? g_aux_cb[dev][bus] : 0);
    if (arg(c, 4)) wiiu_write32(arg(c, 4), ok ? g_aux_ctx[dev][bus] : 0);
    ret(c, 0);
}
// (device, device id, bus, volume 0x8000 = 1.0)
static void AXSetAuxReturnVolume(CPUState* c) {
    u32 dev = arg(c, 0), bus = arg(c, 2);
    if (dev < 3 && bus < 3 && arg(c, 1) == 0) g_aux_ret[dev][bus] = arg(c, 3) & 0xFFFF;
    ret(c, 0);
}

// (priority, callback(voice, context, reason), context) -> AXVoice* or NULL
static void AXAcquireVoiceEx(CPUState* c) {
    vlock();
    if (!g_voice_mem) g_voice_mem = hle_heap_alloc(AX_MAX_VOICES * AX_VOICE_BYTES, 64);
    u32 obj = 0;
    for (u32 i = 0; i < AX_MAX_VOICES && g_voice_mem; i++) {
        Voice* v = &g_voices[i];
        if (v->obj) continue;
        memset(v, 0, sizeof(*v));
        obj = g_voice_mem + i * AX_VOICE_BYTES;
        v->obj = obj;
        v->priority = arg(c, 0);
        v->ratio = 0x10000;
        u8* p = wiiu_mem_ptr(obj, AX_VOICE_BYTES);
        if (p) memset(p, 0, AX_VOICE_BYTES);
        wiiu_write32(obj + 0x00, i);             // index
        wiiu_write32(obj + 0x1C, arg(c, 0));     // priority
        wiiu_write32(obj + 0x24, arg(c, 2));     // user context
        wiiu_write32(obj + 0x48, arg(c, 1));     // callback
        break;
    }
    vunlock();
    ret(c, obj);
}
static void AXFreeVoice(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    if (v) { wiiu_write32(v->obj + 4, 0); memset(v, 0, sizeof(*v)); }
    vunlock();
}
static void AXGetNumVoices(CPUState* c) {
    u32 n = 0;
    for (int i = 0; i < AX_MAX_VOICES; i++) if (g_voices[i].obj) n++;
    ret(c, n);
}

// (voice, AXVoiceOffsets*): format u16, loop u16, loop/end/current offsets
// (from data), data
static int stream_dbg(Voice* v) {   // (SPLAT_AX_STREAM=n: log the first n offset changes of stream voices)
    static int n = -1;
    if (n < 0) { const char* e = getenv("SPLAT_AX_STREAM"); n = e ? atoi(e) : 0; }
    if (!v || !v->stream || n <= 0) return 0;
    n--;
    return 1;
}
static void AXSetVoiceOffsets(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 o = arg(c, 1);
    if (v && o) {
        v->format = wiiu_read16(o + 0);
        v->loop = wiiu_read16(o + 2) != 0;
        v->loop_pos = wiiu_read32(o + 4);
        v->end_pos = wiiu_read32(o + 8);
        v->cur_pos = wiiu_read32(o + 12);
        v->base = wiiu_read32(o + 16);
        v->primed = false;
        if (stream_dbg(v)) printf("[ax] %08X offsets: fmt %u loop %d loop %u end %u cur %u data %08X\n", v->obj, v->format, v->loop, v->loop_pos, v->end_pos, v->cur_pos, v->base);
    }
    vunlock();
}
static void AXGetVoiceOffsets(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 o = arg(c, 1);
    if (v && o) {
        wiiu_write16(o + 0, v->format);
        wiiu_write16(o + 2, v->loop ? 1 : 0);
        wiiu_write32(o + 4, v->loop_pos);
        wiiu_write32(o + 8, v->end_pos);
        wiiu_write32(o + 12, v->cur_pos);
        wiiu_write32(o + 16, v->base);
    }
    vunlock();
}
// (voice, offset, samples): offsets counted from `samples` instead of the data
static void AXSetVoiceLoopOffsetEx(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    if (stream_dbg(v)) printf("[ax] %08X loop -> %u from %08X (playing at %lld from it, loops %u)\n", v->obj, arg(c, 1), arg(c, 2),
                              (long long)((s64)v->cur_pos - units_between(v->format, v->base, arg(c, 2))), v->loops);
    if (v) v->loop_pos = (u32)((s64)arg(c, 1) + units_between(v->format, v->base, arg(c, 2)));
    vunlock();
}
static void AXSetVoiceEndOffsetEx(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    if (stream_dbg(v)) printf("[ax] %08X end -> %u from %08X (playing at %lld from it, loops %u)\n", v->obj, arg(c, 1), arg(c, 2),
                              (long long)((s64)v->cur_pos - units_between(v->format, v->base, arg(c, 2))), v->loops);
    if (v) v->end_pos = (u32)((s64)arg(c, 1) + units_between(v->format, v->base, arg(c, 2)));
    vunlock();
}
static void AXGetVoiceCurrentOffsetEx(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 r = v ? (u32)((s64)v->cur_pos - units_between(v->format, v->base, arg(c, 1))) : 0;
    vunlock();
    ret(c, r);
}
static void AXGetVoiceLoopCount(CPUState* c) {
    Voice* v = voice_of(arg(c, 0));
    ret(c, v ? v->loops : 0);
}

// (voice, AXVoiceAdpcm*): 16 coefficients, gain, predictor/scale, history
static void AXSetVoiceAdpcm(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 a = arg(c, 1);
    if (v && a) {
        for (int i = 0; i < 16; i++) v->coef[i] = (s16)wiiu_read16(a + i * 2);
        v->ps = wiiu_read16(a + 0x22);
        v->yn1 = (s16)wiiu_read16(a + 0x24);
        v->yn2 = (s16)wiiu_read16(a + 0x26);
    }
    vunlock();
}
// (voice, {predictor/scale, history}) for the loop
static void AXSetVoiceAdpcmLoop(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 a = arg(c, 1);
    if (v && a) {
        v->loop_yn1 = (s16)wiiu_read16(a + 2);
        v->loop_yn2 = (s16)wiiu_read16(a + 4);
    }
    vunlock();
}
// (voice, {ratio integer u16, ratio fraction u16, position fraction u16, last samples})
static void AXSetVoiceSrc(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 a = arg(c, 1);
    if (v && a) {
        v->ratio = ((u32)wiiu_read16(a + 0) << 16) | wiiu_read16(a + 2);
        v->frac = wiiu_read16(a + 4);
    }
    vunlock();
}
// (voice, float ratio) -> 0 = ok
static void AXSetVoiceSrcRatio(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    double r = c->fpr[1];
    if (r < 0.0) r = 0.0;
    if (r > 8.0) r = 8.0;
    if (v) v->ratio = (u32)(r * 65536.0 + 0.5);
    vunlock();
    ret(c, 0);
}
static void AXSetVoiceSrcType(CPUState* c) {
    Voice* v = voice_of(arg(c, 0));
    if (v) v->src_none = arg(c, 1) == 0;
}
static void AXSetVoiceType(CPUState* c) {
    Voice* v = voice_of(arg(c, 0));
    if (v) v->stream = arg(c, 1) == 1;
}
static void AXSetVoiceLoop(CPUState* c) {
    Voice* v = voice_of(arg(c, 0));
    if (v) v->loop = arg(c, 1) != 0;
}
static void AXSetVoiceState(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    if (v) set_state(v, (arg(c, 1) & 0xFFFF) != 0);
    vunlock();
}
static void AXSetVoicePriority(CPUState* c) {
    Voice* v = voice_of(arg(c, 0));
    if (v) { v->priority = arg(c, 1); wiiu_write32(v->obj + 0x1C, arg(c, 1)); }
}
// (voice, {volume u16, delta s16})
static void AXSetVoiceVe(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 a = arg(c, 1);
    if (v && a) { v->ve = wiiu_read16(a); v->ve_delta = (s16)wiiu_read16(a + 2); }
    vunlock();
}
// (voice, device, id, mix): per channel 4 buses of {volume u16, delta s16};
// bus 0 is the main mix (1-3 feed the effects)
static void AXSetVoiceDeviceMix(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 dev = arg(c, 1), id = arg(c, 2), m = arg(c, 3);
    static int dbg = -1;   // (SPLAT_AX_DEBUG=n: print the first n mixes as given)
    if (dbg < 0) { const char* e = getenv("SPLAT_AX_DEBUG"); dbg = e ? atoi(e) : 0; }
    if (dbg > 0 && m) {
        dbg--;
        printf("[ax] voice %08X device %u id %u mix:", arg(c, 0), dev, id);
        for (int i = 0; i < (dev == 0 ? 24 : 16); i++) printf(" %04X/%d", wiiu_read16(m + i * 4), (s16)wiiu_read16(m + i * 4 + 2));
        printf("\n");
    }
    if (v && m && id == 0) {
        for (int b = 0; b < 4; b++) {
            if (dev == 0) for (int ch = 0; ch < 6; ch++) { v->tv[b][ch] = wiiu_read16(m + ch * 16 + b * 4); v->tv_d[b][ch] = (s16)wiiu_read16(m + ch * 16 + b * 4 + 2); }
            if (dev == 1) for (int ch = 0; ch < 4; ch++) { v->drc[b][ch] = wiiu_read16(m + ch * 16 + b * 4); v->drc_d[b][ch] = (s16)wiiu_read16(m + ch * 16 + b * 4 + 2); }
        }
    }
    vunlock();
    ret(c, 0);
}
// (voice, {on u16, history s16, a0 u16, b0 u16})
static void AXSetVoiceLpf(CPUState* c) {
    vlock();
    Voice* v = voice_of(arg(c, 0));
    u32 a = arg(c, 1);
    if (v && a) {
        v->lpf_on = wiiu_read16(a) != 0;
        v->lpf_y = (s16)wiiu_read16(a + 2);
        v->lpf_a0 = wiiu_read16(a + 4);
        v->lpf_b0 = wiiu_read16(a + 6);
    }
    vunlock();
}
static void AXSetVoiceLpfCoefs(CPUState* c) {   // (voice, a0, b0)
    Voice* v = voice_of(arg(c, 0));
    if (v) { v->lpf_a0 = arg(c, 1) & 0xFFFF; v->lpf_b0 = arg(c, 2) & 0xFFFF; }
}
// (cutoff Hz, &a0, &b0): a one-pole low-pass at the mixer's rate
static void AXComputeLpfCoefs(CPUState* c) {
    double f = (double)(arg(c, 0) & 0xFFFF);
    double b = exp(-2.0 * 3.14159265358979 * f / AX_RATE);
    if (arg(c, 1)) wiiu_write16(arg(c, 1), (u16)((1.0 - b) * 32767.0 + 0.5));
    if (arg(c, 2)) wiiu_write16(arg(c, 2), (u16)(b * 32767.0 + 0.5));
}
// (ADPCM data, AXVoiceAdpcm* decoder state, samples, s16* out): decode on the CPU
static void AXDecodeAdpcmData(CPUState* c) {
    u32 src = arg(c, 0), st = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (!src || !st || !out) return;
    Voice tmp;
    memset(&tmp, 0, sizeof(tmp));
    for (int i = 0; i < 16; i++) tmp.coef[i] = (s16)wiiu_read16(st + i * 2);
    tmp.ps = wiiu_read16(st + 0x22);
    tmp.yn1 = (s16)wiiu_read16(st + 0x24);
    tmp.yn2 = (s16)wiiu_read16(st + 0x26);
    tmp.base = src;
    tmp.cur_pos = 0;
    tmp.end_pos = 0xFFFFFFFFu;
    tmp.obj = 0;
    for (u32 i = 0; i < n; i++) {
        u32 p = tmp.cur_pos;
        if ((p & 15) < 2) { tmp.ps = wiiu_read8(src + ((p & ~15u) >> 1)); p = (p & ~15u) + 2; }
        u8 b = wiiu_read8(src + (p >> 1));
        s32 nib = (p & 1) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        u32 ci = (tmp.ps >> 4) & 7;
        s64 x = ((s64)(nib << (tmp.ps & 15)) << 11) + (s64)tmp.coef[ci * 2] * tmp.yn1 + (s64)tmp.coef[ci * 2 + 1] * tmp.yn2;
        x = (x + 1024) >> 11;
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        tmp.yn2 = tmp.yn1;
        tmp.yn1 = (s16)x;
        wiiu_write16(out + i * 2, (u16)(s16)x);
        tmp.cur_pos = p + 1;
    }
    wiiu_write16(st + 0x22, tmp.ps);
    wiiu_write16(st + 0x24, (u16)tmp.yn1);
    wiiu_write16(st + 0x26, (u16)tmp.yn2);
}
static void AXGetDspLoad(CPUState* c) { ret(c, 10); }

static const HleEntry g_audio_table[] = {
    { "AXInit", AXInit },
    { "AXInitWithParams", AXInitWithParams },
    { "AXIsInit", AXIsInit },
    { "AXQuit", AXQuit },
    { "AXRegisterAppFrameCallback", AXRegisterAppFrameCallback },
    { "AXDeregisterAppFrameCallback", AXDeregisterAppFrameCallback },
    { "AXRegisterFrameCallback", AXRegisterFrameCallback },
    { "AXRegisterDeviceFinalMixCallback", AXRegisterDeviceFinalMixCallback },
    { "AXGetDeviceFinalMixCallback", AXGetDeviceFinalMixCallback },
    { "AXGetDeviceMode", AXGetDeviceMode },
    { "AXGetInputSamplesPerFrame", AXGetInputSamplesPerFrame },
    { "AXGetInputSamplesPerSec", AXGetInputSamplesPerSec },
    // Voices
    { "AXAcquireVoiceEx", AXAcquireVoiceEx },
    { "AXFreeVoice", AXFreeVoice },
    { "AXGetNumVoices", AXGetNumVoices },
    { "AXSetVoiceOffsets", AXSetVoiceOffsets },
    { "AXGetVoiceOffsets", AXGetVoiceOffsets },
    { "AXSetVoiceLoopOffsetEx", AXSetVoiceLoopOffsetEx },
    { "AXSetVoiceEndOffsetEx", AXSetVoiceEndOffsetEx },
    { "AXGetVoiceCurrentOffsetEx", AXGetVoiceCurrentOffsetEx },
    { "AXGetVoiceLoopCount", AXGetVoiceLoopCount },
    { "AXSetVoiceAdpcm", AXSetVoiceAdpcm },
    { "AXSetVoiceAdpcmLoop", AXSetVoiceAdpcmLoop },
    { "AXSetVoiceSrc", AXSetVoiceSrc },
    { "AXSetVoiceSrcRatio", AXSetVoiceSrcRatio },
    { "AXSetVoiceSrcType", AXSetVoiceSrcType },
    { "AXSetVoiceType", AXSetVoiceType },
    { "AXSetVoiceLoop", AXSetVoiceLoop },
    { "AXSetVoiceState", AXSetVoiceState },
    { "AXSetVoicePriority", AXSetVoicePriority },
    { "AXSetVoiceVe", AXSetVoiceVe },
    { "AXSetVoiceDeviceMix", AXSetVoiceDeviceMix },
    { "AXSetVoiceLpf", AXSetVoiceLpf },
    { "AXSetVoiceLpfCoefs", AXSetVoiceLpfCoefs },
    { "AXComputeLpfCoefs", AXComputeLpfCoefs },
    { "AXDecodeAdpcmData", AXDecodeAdpcmData },
    { "AXGetDspLoad", AXGetDspLoad },
    { "AXGetDroppedVoiceCount", zero },
    // Filters, effects, the Wii Remote speaker, GamePad virtual surround:
    // accepted, not applied.
    { "AXSetVoiceBiquad", zero },
    { "AXSetVoiceBiquadCoefs", zero },
    { "AXSetVoiceMixerSelect", zero },
    { "AXSetVoiceRmtOn", zero },
    { "AXSetVoiceRmtIIR", zero },
    { "AXSetVoiceRmtIIRCoefs", zero },
    { "AXSetDRCVSLC", zero },
    { "AXSetDRCVSSpeakerPosition", zero },
    { "AXSetDRCVSSurroundDepth", zero },
    { "AXSetDRCVSDownmixBalance", zero },
    { "AXSetDRCVSOutputGain", zero },
    { "DRCVS_Process", zero },
    { "DRCVS_SetOutputMode", zero },
    // Mixing settings, effects and profiling
    { "AXGetAuxCallback", AXGetAuxCallback },
    { "AXRegisterAuxCallback", AXRegisterAuxCallback },
    { "AXSetAuxReturnVolume", AXSetAuxReturnVolume },
    { "AXSetDRCVSMode", zero },
    { "AXSetDRCVSSurroundLevelGain", zero },
    { "AXSetDefaultMixerSelect", zero },
    { "AXSetDeviceLinearUpsampler", zero },
    { "AXSetDeviceCompressor", zero },
    { "AXSetDeviceUpsampleStage", zero },
    { "AXSetDeviceMode", zero },
    { "AXSetDeviceVolume", zero },
    { "AXInitProfile", zero },
    { "AXGetSwapProfile", zero },
    { "AXRegisterExceedCallback", zero },
    { "AXRmtGetSamplesLeft", zero },
    { "AXRmtGetSamples", zero },
    { "AXRmtAdvancePtr", zero },
    { "AXUserIsProtected", zero },
    { "AXUserBegin", zero },
    { "AXUserEnd", zero },
};

const HleEntry* audio_hle_table(size_t* count) {
    *count = sizeof(g_audio_table) / sizeof(g_audio_table[0]);
    return g_audio_table;
}

// The voices in use right now (the freeze watchdog: a game waiting for a
// sound to finish shows up as a voice that never stops).
void audio_report_voices(FILE* out) {
    vlock();
    int n = 0;
    for (int i = 0; i < AX_MAX_VOICES; i++) {
        const Voice* v = &g_voices[i];
        if (!v->obj) continue;
        if (!n++) fprintf(out, "  sound voices in use:\n");
        fprintf(out, "    %08X %-8s %s%s prio %u at %u of %u (loop at %u, looped %u times) fmt %u ratio %.3f ve %d\n",
                v->obj, v->playing ? "playing" : "stopped", v->loop ? "loop" : "one-shot", v->stream ? " stream" : "",
                v->priority, v->cur_pos, v->end_pos, v->loop_pos, v->loops, v->format, v->ratio / 65536.0, v->ve);
    }
    if (!n) fprintf(out, "  sound voices in use: none\n");
    vunlock();
}
