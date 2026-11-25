// Controllers: the PC's game controller (Xbox, or anything Windows sees as
// a gamepad) and keyboard/mouse act as the Wii U GamePad.
//
// The window thread reads the controller every pass (input_poll); the game
// reads the latest state through VPADRead, like it reads a real GamePad.
//
// Buttons go by position, like on the GamePad: the bottom face button
// (Xbox A) is the GamePad's B, the right one (Xbox B) is A, left (X) is Y,
// top (Y) is X. LB/RB = L/R, LT/RT = ZL/ZR, Start = +, Guide = HOME, stick
// clicks = L3/R3. The mouse touches the GamePad screen only while it's
// shown (View / M); with it hidden, left clicks do nothing.
//
// The View button (two squares; M on the keyboard) shows the GamePad's
// screen over the game and hides it again -- the map in battles, the menu
// in the plaza. While it's up, the right stick moves a cursor on it and A
// touches it there (like the map in Splatoon 2); the mouse works too.
//
// Keyboard: WASD left stick, IJKL right stick, arrows = D-pad, Space = B,
// Enter = A (Alt+Enter: fullscreen, F11 too), X/Y = X/Y, Q/E = L/R, Shift = ZL, right mouse button = ZR,
// Tab = +, Backspace = -.
//
// Player 2 (Battle Dojo, settings "Player 2" / SPLAT_PRO_PAD=1): a second
// controller acts as a Wii U Pro Controller on Wii Remote channel 0 (WPADProbe /
// KPADReadEx below), same buttons as the first; the first controller and the
// keyboard stay the GamePad. SPLAT_PRO_PAD=2: the Pro Controller is there with
// or without a second controller (test runs press its buttons with "P2:A:300").
#include "input.h"
#include "host_thread.h"
#include "wiiu_mem.h"
#include "vk_ctx.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GamePad buttons (VPAD_BUTTON_*).
enum {
    PAD_A = 0x8000, PAD_B = 0x4000, PAD_X = 0x2000, PAD_Y = 0x1000,
    PAD_LEFT = 0x0800, PAD_RIGHT = 0x0400, PAD_UP = 0x0200, PAD_DOWN = 0x0100,
    PAD_ZL = 0x0080, PAD_ZR = 0x0040, PAD_L = 0x0020, PAD_R = 0x0010,
    PAD_PLUS = 0x0008, PAD_MINUS = 0x0004, PAD_HOME = 0x0002,
    PAD_STICK_R = 0x00020000, PAD_STICK_L = 0x00040000,
    // the sticks pushed past halfway also count as "direction buttons"
    PAD_L_LEFT = 0x40000000, PAD_L_RIGHT = 0x20000000, PAD_L_UP = 0x10000000, PAD_L_DOWN = 0x08000000,
    PAD_R_LEFT = 0x04000000, PAD_R_RIGHT = 0x02000000, PAD_R_UP = 0x01000000, PAD_R_DOWN = 0x00800000,
};

typedef struct {
    u32 hold;
    float lx, ly, rx, ry;      // -1..1, up is +
    bool touching;
    u16 tx, ty;                // GamePad touch panel units
} PadState;

static hmtx_t g_lock;
static bool g_lock_ready = false;
static PadState g_pad;
static bool g_announced = false;
static PadState g_pro;              // player 2's Pro Controller
static bool g_pro_connected;        // (WPADProbe: one on channel 0)
static volatile bool g_second_pad;  // a second controller is plugged in (and player 2 is on)
bool input_second_pad(void) { return g_second_pad; }

static void lock(void) {
    if (!g_lock_ready) { hmtx_init(&g_lock, false); g_lock_ready = true; }
    hmtx_lock(&g_lock);
}

static float deadzone(float v) {
    if (fabsf(v) < 0.15f) return 0.0f;
    return v > 0 ? (v - 0.15f) / 0.85f : (v + 0.15f) / 0.85f;
}
static float clamp1(float v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

// The GamePad screen over the game: cursor position (0..1) and button edges.
static float g_cur_x = 0.5f, g_cur_y = 0.5f;
static double g_live_touch_until;   // (SPLAT_LIVE_INPUT "TOUCH:x:y": touching until then)
static bool g_view_was = false, g_mkey_was = false;
static double g_mouse_x = -1, g_mouse_y = -1, g_last_poll = 0;
static bool g_mouse_touch_hidden = false;   // SPLAT_MOUSE_TOUCH=1: left clicks touch the GamePad screen while it's hidden too
// Motion aiming from the mouse (settings "Mouse aim") and a real gyro (gyro_hid.c): turns not yet given to the
// game, in turns of the virtual GamePad (1.0 = 360 degrees; pitch: + looks down, as the game takes it; yaw: left +). VPADRead takes them.
static float g_aim_pitch, g_aim_yaw;

// Scripted presses for unattended test runs (the same route into the game
// every time, so fps numbers can be compared between builds):
// SPLAT_AUTOPLAY="40:A,45:A,60:LU:3000,..." = at 40 s press A, at 60 s hold
// the left stick up for 3 s (default hold 150 ms). Buttons: A B X Y L R ZL ZR
// PLUS MINUS UP DOWN LEFT RIGHT; sticks LU LD LL LR / RU RD RL RR.
// SPLAT_EXIT_AFTER=seconds closes the window then.
// SPLAT_AUTOPLAY_S0..S3: more keys (times in seconds from that moment) once the test run knows which of the
// plaza's start points the player got (SPLAT_SPAWN_DETECT, gx2.c).
// "P2:" in front of a press is player 2's Pro Controller; there the time may be left out ("P2:A:300" = now).
typedef struct { double t, len; u32 pad; int stick; float ang; int player; } AutoKey;   // (stick 9 / 10: left / right stick at `ang` degrees, 0 = up, 90 = right: "t:LA:ms:deg")
static int parse_keys(const char* e, AutoKey* keys, int n, int max);
static void apply_key(const AutoKey* k, PadState* s) {
    s->hold |= k->pad;
    switch (k->stick) {
    case 1: s->ly = 1; break;  case 2: s->ly = -1; break;  case 3: s->lx = -1; break;  case 4: s->lx = 1; break;
    case 5: s->ry = 1; break;  case 6: s->ry = -1; break;  case 7: s->rx = -1; break;  case 8: s->rx = 1; break;
    case 9: s->lx = sinf(k->ang * 3.14159265f / 180.0f); s->ly = cosf(k->ang * 3.14159265f / 180.0f); break;
    case 10: s->rx = sinf(k->ang * 3.14159265f / 180.0f); s->ry = cosf(k->ang * 3.14159265f / 180.0f); break;
    }
}
static AutoKey lk[256];   // (the live presses now running)
static int ln = 0;
static int parse_keys(const char* e, AutoKey* keys, int n, int max);
// (SPLAT_LIVE_INPUT lines -- from the file, or pushed from inside: the benchmark, benchmark.c)
static void live_lines(char* buf, double now) {
    for (char* p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ',';
    if (strstr(buf, "SHOT")) g_vk.screenshot = 1;
    if (strstr(buf, "DBGFRAME")) g_vk.dbg_req = 1;   // (the next frame's draws logged)
    if (strstr(buf, "HIDEHUD")) { g_vk.hide_hud = !g_vk.hide_hud; printf("[input] live: HUD %s\n", g_vk.hide_hud ? "hidden" : "shown"); }   // (as F1)
    if (strstr(buf, "FREECAM")) { extern void splat_freecam_toggle(void); splat_freecam_toggle(); }   // (as F4)
    if (strstr(buf, "MEMDUMP")) { extern void splat_memdump(const char* path); splat_memdump("debug_output/memdump.bin"); }
    else if (strstr(buf, "DUMP")) { g_vk.want_dump = 1; printf("[input] live: writing the freeze watchdog's dump (as F9)\n"); }   // (a stalled test run: what waits for what)
    const char* wp = strstr(buf, "WATCH:");   // "WATCH:addr,addr": watch those game words on the main thread (nan_trap.c)
    if (wp) {
        extern void watch_request(const unsigned* addrs, int n);
        unsigned wa[4];
        int wn = 0;
        for (const char* q = wp + 6; *q && wn < 4; ) {
            char* end;
            unsigned long v = strtoul(q, &end, 16);
            if (end == q) break;
            wa[wn++] = (unsigned)v;
            q = end;
            if (*q != ':') break;   // (more addresses after ':')
            q++;
        }
        if (wn) watch_request(wa, wn);
    }
    if (strstr(buf, "WATCHREPORT")) { extern void watch_report(FILE* out); watch_report(stdout); }
    const char* pp = strstr(buf, "PEEK:");   // "PEEK:addr": 16 game words from there
    if (pp) {
        unsigned a = (unsigned)strtoul(pp + 5, NULL, 16) & ~3u;
        printf("[input] live: peek %08X:", a);
        for (int i = 0; i < 16; i++) printf(" %08X", wiiu_mem_ptr(a + 4 * i, 4) ? wiiu_read32(a + 4 * i) : 0);
        printf("\n");
    }
    const char* wt = strstr(buf, "WT:");   // "WT:n": wide-screen fixes off (latte_render.c wide_full_screen / side_bands_clear)
    if (wt) { g_vk.wide_test = atoi(wt + 3); printf("[input] live: wide-screen test flags %d\n", g_vk.wide_test); }
    const char* shp = strstr(buf, "SHADOWS:");   // "SHADOWS:n": settings "Shadows" (0 off, 1 normal, 2 sharp)
    if (shp) { g_vk.shadow_quality = atoi(shp + 8) % 3; printf("[input] live: shadows %d\n", g_vk.shadow_quality); }
    const char* skp = strstr(buf, "SKY:");   // "SKY:n": settings "Sky & light"
    if (skp) { g_vk.sky_mode = atoi(skp + 4) % 3; printf("[input] live: sky %d\n", g_vk.sky_mode); }
    const char* shc = strstr(buf, "SHC:");   // "SHC:n": SHADERS 3D clouds
    if (shc) { g_vk.sh_clouds = atoi(shc + 4) != 0; printf("[input] live: 3D clouds %d\n", g_vk.sh_clouds); }
    const char* shr = strstr(buf, "SHR:");   // "SHR:n": SHADERS god rays & haze
    if (shr) { g_vk.sh_rays = atoi(shr + 4) != 0; printf("[input] live: god rays & haze %d\n", g_vk.sh_rays); }
    const char* shs = strstr(buf, "SHS:");   // "SHS:n": SHADERS cloud shadows
    if (shs) { g_vk.sh_cshadow = atoi(shs + 4) != 0; printf("[input] live: cloud shadows %d\n", g_vk.sh_cshadow); }
    const char* shn = strstr(buf, "RAIN:");   // "RAIN:n": SHADERS rain (with Storm)
    if (shn) { g_vk.sh_rain = atoi(shn + 5) != 0; printf("[input] live: rain %d\n", g_vk.sh_rain); }
    const char* sha = strstr(buf, "SHA:");   // "SHA:n": SHADERS ambient occlusion
    if (sha) { g_vk.sh_ao = atoi(sha + 4) != 0; printf("[input] live: ambient occlusion %d\n", g_vk.sh_ao); }
    const char* vdb = strstr(buf, "VOLDBG:");   // "VOLDBG:n": the volume pass's debug pictures (latte_render.c)
    if (vdb) { extern volatile int splat_vol_dbg; splat_vol_dbg = atoi(vdb + 7); printf("[input] live: volume debug %d\n", splat_vol_dbg); }
    const char* shq = strstr(buf, "SHQ:");   // "SHQ:n": SHADERS quality
    if (shq) { g_vk.sh_quality = atoi(shq + 4) % 3; printf("[input] live: shader quality %d\n", g_vk.sh_quality); }
    const char* p30 = strstr(buf, "P30:");   // "P30:n": settings "Plaza frame rate" (1 = the game's own 30)
    if (p30) { g_vk.plaza_30 = atoi(p30 + 4) != 0; printf("[input] live: plaza 30 %d\n", g_vk.plaza_30); }
    const char* fvp = strstr(buf, "FOV:");   // "FOV:n": settings "Field of view" (+n degrees, 0..40)
    if (fvp) { int f = atoi(fvp + 4); g_vk.fov_add = f < 0 ? 0 : f > 40 ? 40 : f; printf("[input] live: field of view %+d\n", g_vk.fov_add); }
    const char* mbp = strstr(buf, "MBLUR:");   // "MBLUR:n": settings "Motion blur" (0 off, 1 light, 2 strong)
    if (mbp) { g_vk.motion_blur = atoi(mbp + 6) % 3; printf("[input] live: motion blur %d\n", g_vk.motion_blur); }
    if (strstr(buf, "VIEW")) { g_vk.drc_on = !g_vk.drc_on; g_vk.drc_auto = 0; printf("[input] live: GamePad screen %s\n", g_vk.drc_on ? "shown" : "hidden"); }
    const char* dvp = strstr(buf, "DV:");   // "DV:n": debug view n (turns the debug views on)
    if (dvp) { g_vk.debug_views_on = 1; g_vk.debug_view = atoi(dvp + 3) % 6; }
    const char* pk = strstr(buf, "PICK:");   // "PICK:x:y": pick at that TV pixel (the game's 1280x720)
    int pkx, pky;
    if (pk && sscanf(pk + 5, "%d:%d", &pkx, &pky) == 2) { g_vk.debug_views_on = 1; g_vk.pick_x = pkx; g_vk.pick_y = pky; g_vk.pick_req = 1; }
    const char* lbp = strstr(buf, "LOBBY:");   // "LOBBY:6" / "LOBBY:7" / "LOBBY:8": the Bot Lobby's F6 / F7 / F8 (bot_lobby.c)
    if (lbp) { extern void splat_lobby_key(int key, int shift); splat_lobby_key(atoi(lbp + 6), 0); }
    const char* amp = strstr(buf, "AIM:");   // "AIM:dx:dy": as if the captured mouse moved that many pixels (mouse-aim tests)
    float adx, ady;
    if (amp && sscanf(amp + 4, "%f:%f", &adx, &ady) == 2) {
        static const float sens[5] = { 0, 1.0f, 1.5f, 2.0f, 0.5f };
        float k = 0.04f / 360.0f * sens[g_vk.mouse_aim >= 1 && g_vk.mouse_aim <= 4 ? g_vk.mouse_aim : 1];
        lock(); g_aim_yaw -= adx * k; g_aim_pitch += ady * k; hmtx_unlock(&g_lock);
        printf("[input] live: aim moved %.0f, %.0f pixels\n", adx, ady);
    }
    const char* tp = strstr(buf, "TOUCH:");   // "TOUCH:x:y" (0..1 across / down the GamePad screen; it must be shown)
    if (tp && sscanf(tp + 6, "%f:%f", &g_cur_x, &g_cur_y) == 2) g_live_touch_until = now + 0.25;
    int added = 0, cnt;
    AutoKey tmp[64];
    cnt = parse_keys(buf, tmp, 0, 64);
    for (int i = 0; i < cnt; i++) {
        int slot = -1;
        for (int k = 0; k < ln; k++) if (now >= lk[k].t + lk[k].len) { slot = k; break; }
        if (slot < 0 && ln < 256) slot = ln++;
        if (slot < 0) break;
        lk[slot] = tmp[i];
        lk[slot].t += now;
        added++;
    }
    printf("[input] live: %d presses at %.1f s: %s\n", added, now, buf);
}
static char g_live_q[32][256];
static volatile int g_live_qh, g_live_qt;
void input_live_push(const char* line) {
    int t = g_live_qt;
    if ((t + 1) % 32 == g_live_qh) return;   // (full: dropped)
    snprintf(g_live_q[t], sizeof(g_live_q[t]), "%s", line);
    g_live_qt = (t + 1) % 32;
}
static void autoplay(GLFWwindow* window, double now, PadState* s, PadState* p2) {
    {   // (the benchmark drives itself through the same live lines)
        extern void splat_bench_tick(double now);
        extern void splat_lobby_tick(double now);
        splat_bench_tick(now);
        splat_lobby_tick(now);
        while (g_live_qh != g_live_qt) {
            char buf[4096];
            snprintf(buf, sizeof(buf), "%s", g_live_q[g_live_qh]);
            g_live_qh = (g_live_qh + 1) % 32;
            live_lines(buf, now);
        }
    }
    static int n = -1;
    static AutoKey keys[256];
    static double exit_at = 0;
    static bool spawn_keys = false;
    extern int splat_test_spawn;
    if (!spawn_keys && splat_test_spawn >= 0 && n >= 0) {
        spawn_keys = true;
        char var[24];
        snprintf(var, sizeof(var), "SPLAT_AUTOPLAY_S%d", splat_test_spawn);
        int before = n;
        n = parse_keys(getenv(var), keys, n, 256);
        for (int i = before; i < n; i++) keys[i].t += now;   // (these times count from now)
        printf("[input] autoplay: start point %d: %d more presses\n", splat_test_spawn, n - before);
    }
    if (n < 0) {
        const char* x = getenv("SPLAT_EXIT_AFTER");
        if (x) exit_at = atof(x);
        n = parse_keys(getenv("SPLAT_AUTOPLAY"), keys, 0, 256);
        if (n) printf("[input] autoplay: %d scripted presses\n", n);
    }
    static u8 started[256];
    for (int i = 0; i < n; i++) {
        if (now < keys[i].t || now >= keys[i].t + keys[i].len) continue;
        if (!started[i]) { started[i] = 1; printf("[input] autoplay: press %d (%s%08X stick %d) at %.1f s\n", i, keys[i].player == 2 ? "P2 " : "", keys[i].pad, keys[i].stick, now); }
        apply_key(&keys[i], keys[i].player == 2 ? p2 : s);
    }
    {   // SPLAT_LIVE_INPUT=<file>: presses written into that file while the game runs (one per line or
        // comma-separated, as SPLAT_AUTOPLAY but the time counts from when the line is read: "0:ZL:2000",
        // "0.5:LA:800:90"); "SHOT" takes a screenshot (as F12). For steering test runs step by step.
        static int live = -1;
        static char live_path[512];
        static long live_off;
        static double live_next;
        if (live < 0) {
            const char* e = getenv("SPLAT_LIVE_INPUT");
            live = e && strlen(e) < sizeof(live_path);
            if (live) { strcpy(live_path, e); FILE* f = fopen(live_path, "rb"); if (f) { fseek(f, 0, SEEK_END); live_off = ftell(f); fclose(f); } }
        }
        if (live && now >= live_next) {
            live_next = now + 0.1;
            FILE* f = fopen(live_path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long end = ftell(f);
                if (end < live_off) live_off = 0;   // (the file was started over)
                if (end > live_off && end - live_off < 4000) {
                    char buf[4096];
                    fseek(f, live_off, SEEK_SET);
                    size_t got = fread(buf, 1, (size_t)(end - live_off), f);
                    buf[got] = 0;
                    char* last_nl = strrchr(buf, '\n');
                    if (last_nl) {   // (whole lines only)
                        live_off += (long)(last_nl - buf) + 1;
                        last_nl[1] = 0;
                        live_lines(buf, now);
                    }
                } else if (end > live_off) live_off = end;
                fclose(f);
            }
        }
        for (int i = 0; i < ln; i++) {
            if (now < lk[i].t || now >= lk[i].t + lk[i].len) continue;
            apply_key(&lk[i], lk[i].player == 2 ? p2 : s);
        }
    }
    {   // (SPLAT_WALK_TO: the test route's autopilot, gx2.c)
        extern void splat_walk_stick(float* lx, float* ly, bool* a);
        float lx = 0, ly = 0;
        bool a = false;
        splat_walk_stick(&lx, &ly, &a);
        if (lx != 0 || ly != 0) { s->lx = lx; s->ly = ly; }
        if (a) s->hold |= PAD_A;
    }
    if (exit_at > 0 && now >= exit_at && window) glfwSetWindowShouldClose(window, 1);
}
static int parse_keys(const char* e, AutoKey* keys, int n, int max) {
    {
        static const struct { const char* name; u32 pad; int stick; } names[] = {
            { "A", PAD_A }, { "B", PAD_B }, { "X", PAD_X }, { "Y", PAD_Y }, { "L", PAD_L }, { "R", PAD_R },
            { "ZL", PAD_ZL }, { "ZR", PAD_ZR }, { "PLUS", PAD_PLUS }, { "MINUS", PAD_MINUS },
            { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
            { "LU", 0, 1 }, { "LD", 0, 2 }, { "LL", 0, 3 }, { "LR", 0, 4 },
            { "RU", 0, 5 }, { "RD", 0, 6 }, { "RL", 0, 7 }, { "RR", 0, 8 },
            { "LA", 0, 9 }, { "RA", 0, 10 },
        };
        char buf[4096];
        if (e && strlen(e) < sizeof(buf)) {
            strcpy(buf, e);
            for (char* tok = strtok(buf, ","); tok && n < max; tok = strtok(NULL, ",")) {
                char name[16] = { 0 };
                double t = 0, len = 150, ang = 0;
                int player = 1;
                if (!strncmp(tok, "P2:", 3)) { player = 2; tok += 3; }
                if (player == 2 && *tok >= 'A' && *tok <= 'Z') { if (sscanf(tok, "%15[A-Z]:%lf:%lf", name, &len, &ang) < 1) continue; }
                else if (sscanf(tok, "%lf:%15[A-Z]:%lf:%lf", &t, name, &len, &ang) < 2) continue;
                for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                    if (!strcmp(name, names[i].name)) { keys[n++] = (AutoKey){ t, len / 1000.0, names[i].pad, names[i].stick, (float)ang, player }; break; }
            }
        }
    }
    return n;
}

// A controller's buttons and sticks as GamePad buttons (for player 2 they become Pro Controller buttons in KPADReadEx).
static void read_gamepad(const GLFWgamepadstate* g, PadState* s) {
    // (face buttons by label by default -- the button the game's prompt names is the
    // one printed on an Xbox pad; settings row "Button layout" / SPLAT_PAD_LAYOUT=1:
    // by position, as on a Nintendo pad)
    { static int env = -1; if (env < 0) { const char* e = getenv("SPLAT_PAD_LAYOUT"); env = e != NULL; if (e) g_vk.pad_layout = atoi(e) ? 1 : 0; } }
    bool by_pos = g_vk.pad_layout == 1;
    if (g->buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS) s->hold |= by_pos ? PAD_B : PAD_A;
    if (g->buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS) s->hold |= by_pos ? PAD_A : PAD_B;
    if (g->buttons[GLFW_GAMEPAD_BUTTON_X] == GLFW_PRESS) s->hold |= by_pos ? PAD_Y : PAD_X;
    if (g->buttons[GLFW_GAMEPAD_BUTTON_Y] == GLFW_PRESS) s->hold |= by_pos ? PAD_X : PAD_Y;
    static const struct { int button; u32 pad; } map[] = {
        { GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, PAD_L }, { GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, PAD_R },
        { GLFW_GAMEPAD_BUTTON_START, PAD_PLUS },
        { GLFW_GAMEPAD_BUTTON_GUIDE, PAD_HOME },
        { GLFW_GAMEPAD_BUTTON_LEFT_THUMB, PAD_STICK_L }, { GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, PAD_STICK_R },
        { GLFW_GAMEPAD_BUTTON_DPAD_UP, PAD_UP }, { GLFW_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN },
        { GLFW_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT }, { GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (g->buttons[map[i].button] == GLFW_PRESS) s->hold |= map[i].pad;
    if (g->axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.0f) s->hold |= PAD_ZL;     // -1 released .. 1 pressed
    if (g->axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.0f) s->hold |= PAD_ZR;
    s->lx = deadzone(g->axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
    s->ly = -deadzone(g->axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
    s->rx = deadzone(g->axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
    s->ry = -deadzone(g->axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
}
// Sticks inside their circle (two keys at once would otherwise move 1.4x as fast diagonally),
// and pushed past halfway they also count as "direction buttons".
static void stick_bits(PadState* s) {
    s->lx = clamp1(s->lx); s->ly = clamp1(s->ly); s->rx = clamp1(s->rx); s->ry = clamp1(s->ry);
    float l = sqrtf(s->lx * s->lx + s->ly * s->ly), r = sqrtf(s->rx * s->rx + s->ry * s->ry);
    if (l > 1.0f) { s->lx /= l; s->ly /= l; }
    if (r > 1.0f) { s->rx /= r; s->ry /= r; }
    if (s->lx < -0.5f) s->hold |= PAD_L_LEFT;
    if (s->lx > 0.5f) s->hold |= PAD_L_RIGHT;
    if (s->ly > 0.5f) s->hold |= PAD_L_UP;
    if (s->ly < -0.5f) s->hold |= PAD_L_DOWN;
    if (s->rx < -0.5f) s->hold |= PAD_R_LEFT;
    if (s->rx > 0.5f) s->hold |= PAD_R_RIGHT;
    if (s->ry > 0.5f) s->hold |= PAD_R_UP;
    if (s->ry < -0.5f) s->hold |= PAD_R_DOWN;
}

void input_poll(void* window_ptr) {
    GLFWwindow* window = (GLFWwindow*)window_ptr;
    PadState s, p2;   // (the GamePad; player 2's Pro Controller)
    memset(&s, 0, sizeof(s));
    memset(&p2, 0, sizeof(p2));
    bool view = false, pad_a = false;   // (the controller's View and A buttons)
    double now = glfwGetTime();
    float dt = g_last_poll > 0 ? (float)(now - g_last_poll) : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_last_poll = now;
    {   // settings "Player 2" (SPLAT_PRO_PAD wins: 0 off, 1 with a second controller, 2 always)
        static int env = -1;
        if (env == -1) { const char* e = getenv("SPLAT_PRO_PAD"); env = e ? 1 : 0; if (e) g_vk.pro_pad = atoi(e) < 0 ? 0 : atoi(e) > 3 ? 3 : atoi(e); }
    }
    const int pro = g_vk.pro_pad;
    bool second = false;

    // A game controller, if one is plugged in (a second one: player 2's Pro Controller).
    for (int j = GLFW_JOYSTICK_1, found = 0; j <= GLFW_JOYSTICK_LAST; j++) {
        GLFWgamepadstate g;
        if (!glfwJoystickIsGamepad(j) || !glfwGetGamepadState(j, &g)) continue;
        if (found) {
            static bool told;
            if (!told) { told = true; printf("[input] second controller found: %s (acts as player 2's Pro Controller)\n", glfwGetGamepadName(j)); }
            read_gamepad(&g, &p2);
            if (g.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS) p2.hold |= PAD_MINUS;   // (View: the Pro Controller's -)
            second = true;
            break;
        }
        found = 1;
        if (!g_announced) {
            printf("[input] controller found: %s (acts as the GamePad)\n", glfwGetGamepadName(j));
            g_announced = true;
        }
        read_gamepad(&g, &s);
        view = g.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS;
        pad_a = g.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS;
        if (!pro) break;
    }
    g_second_pad = second;

    // View button: a tap is the GamePad's - button (an Xbox pad has no other
    // button left for it: Sunken Scrolls, the test range's weapons), holding
    // it ~0.4 s shows / hides the GamePad screen. M: the GamePad screen at
    // once, Backspace: -. SPLAT_VIEW_OLD=1: View = GamePad screen only.
    static int view_old = -1;
    if (view_old < 0) view_old = getenv("SPLAT_VIEW_OLD") != NULL;
    static double view_down_at = -1.0, minus_until = 0.0;
    static bool view_long_done;
    bool mkey = window && glfwGetWindowAttrib(window, GLFW_FOCUSED) && glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
    bool toggle_drc = mkey && !g_mkey_was;
    if (view_old) toggle_drc = toggle_drc || (view && !g_view_was);
    else {
        if (view && !g_view_was) { view_down_at = now; view_long_done = false; }
        if (view && view_down_at >= 0.0 && !view_long_done && now - view_down_at >= 0.4) { toggle_drc = true; view_long_done = true; }
        if (!view && g_view_was) { if (!view_long_done) minus_until = now + 0.12; view_down_at = -1.0; }
        if (now < minus_until) s.hold |= PAD_MINUS;
    }
    if (toggle_drc) {
        g_vk.drc_on = !g_vk.drc_on;
        g_vk.drc_auto = 0;   // (the player decides from now on)
        printf("[input] GamePad screen %s\n", g_vk.drc_on ? "shown (right stick + A, or the mouse, to touch it; hold View or M to hide)" : "hidden");
    }
    g_view_was = view;
    g_mkey_was = mkey;
    bool drc = g_vk.drc_on != 0 && !g_vk.drc_play;   // (drc_play: the GamePad screen is the picture to play by -- no cursor)
    { static int once = 0; if (!once) { once = 1; g_mouse_touch_hidden = getenv("SPLAT_MOUSE_TOUCH") != NULL; } }

    // Keyboard and mouse (only while the window has focus).
    if (window && glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
        #define KEY(k) (glfwGetKey(window, k) == GLFW_PRESS)
        if (KEY(GLFW_KEY_ENTER) && !KEY(GLFW_KEY_LEFT_ALT) && !KEY(GLFW_KEY_RIGHT_ALT)) s.hold |= PAD_A;   // (Alt+Enter: fullscreen)
        if (KEY(GLFW_KEY_SPACE)) s.hold |= PAD_B;
        if (KEY(GLFW_KEY_X)) s.hold |= PAD_X;
        if (KEY(GLFW_KEY_Y)) s.hold |= PAD_Y;
        if (KEY(GLFW_KEY_Q)) s.hold |= PAD_L;
        if (KEY(GLFW_KEY_E)) s.hold |= PAD_R;
        if (KEY(GLFW_KEY_LEFT_SHIFT)) s.hold |= PAD_ZL;
        if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) s.hold |= PAD_ZR;
        // (Tab = +, but not as part of Alt+Tab: that paused story levels -- the pause
        // menu is only on the GamePad, so the game looked frozen)
        if (KEY(GLFW_KEY_TAB) && !KEY(GLFW_KEY_LEFT_ALT) && !KEY(GLFW_KEY_RIGHT_ALT)) s.hold |= PAD_PLUS;
        if (KEY(GLFW_KEY_BACKSPACE)) s.hold |= PAD_MINUS;
        if (KEY(GLFW_KEY_UP)) s.hold |= PAD_UP;
        if (KEY(GLFW_KEY_DOWN)) s.hold |= PAD_DOWN;
        if (KEY(GLFW_KEY_LEFT)) s.hold |= PAD_LEFT;
        if (KEY(GLFW_KEY_RIGHT)) s.hold |= PAD_RIGHT;
        if (KEY(GLFW_KEY_A)) s.lx = -1;
        if (KEY(GLFW_KEY_D)) s.lx = 1;
        if (KEY(GLFW_KEY_W)) s.ly = 1;
        if (KEY(GLFW_KEY_S)) s.ly = -1;
        if (KEY(GLFW_KEY_J)) s.rx = -1;
        if (KEY(GLFW_KEY_L)) s.rx = 1;
        if (KEY(GLFW_KEY_I)) s.ry = 1;
        if (KEY(GLFW_KEY_K)) s.ry = -1;
        #undef KEY
        double mx, my;
        int w, h;
        glfwGetCursorPos(window, &mx, &my);
        glfwGetWindowSize(window, &w, &h);
        bool moved = mx != g_mouse_x || my != g_mouse_y;
        g_mouse_x = mx;
        g_mouse_y = my;
        {   // the settings panel (windowed mode): a click on the SETTINGS box opens it, a click on a row changes it
            static bool was_down = false;
            static int ignore_clicks = -1;   // (SPLAT_IGNORE_REAL_INPUT test runs: clicks on the panel too)
            if (ignore_clicks < 0) ignore_clicks = getenv("SPLAT_IGNORE_REAL_INPUT") != NULL;
            bool down = !ignore_clicks && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (down && !was_down) {
                int ix = (int)mx, iy = (int)my;
                if (g_vk.gear_w && ix >= g_vk.gear_x && ix < g_vk.gear_x + g_vk.gear_w && iy >= g_vk.gear_y && iy < g_vk.gear_y + g_vk.gear_h) {
                    g_vk.settings_open = !g_vk.settings_open;
                } else if (g_vk.settings_open && g_vk.panel_rows && g_vk.panel_col_w > 0 && ix >= g_vk.panel_x && iy >= g_vk.panel_y &&
                           iy < g_vk.panel_y + g_vk.panel_rows * g_vk.panel_row_h) {
                    // (the panel's columns and rows -> which setting: latte_render.c draw_settings fills panel_ids; group titles are -1)
                    int col = (ix - g_vk.panel_x) / g_vk.panel_col_w, cx = (ix - g_vk.panel_x) % g_vk.panel_col_w;
                    int row = (iy - g_vk.panel_y) / g_vk.panel_row_h;
                    int id = col < g_vk.panel_cols && col < 2 && cx < g_vk.panel_w && row < 48 ? g_vk.panel_ids[col][row] : -1;
                    switch (id) {
                    case 0: g_vk.want_fullscreen_toggle = 1; break;
                    case 1: { int cur = h; g_vk.want_window_height = cur < 1000 ? 1080 : cur < 1300 ? 1440 : 720; break; }
                    case 2: g_vk.vsync_mode = (g_vk.vsync_mode + 1) % 3; printf("[display] vsync %s (settings)\n", g_vk.vsync_mode == 1 ? "on" : g_vk.vsync_mode == 2 ? "off" : "auto"); break;
                    case 3: g_vk.hide_fps = !g_vk.hide_fps; break;
                    case 4: g_vk.no_mips = !g_vk.no_mips; break;
                    case 5: g_vk.drc_on = !g_vk.drc_on; g_vk.drc_auto = 0; break;
                    case 6: g_vk.aa_mode = !g_vk.aa_mode; printf("[display] anti-aliasing %s (settings)\n", g_vk.aa_mode ? "on" : "off"); break;
                    case 7: {   // (1x -> 1.5x -> 2x -> 0.5x -> 0.75x -> 1x)
                        static const char* const names[5] = { "1x", "1.5x", "2x", "0.5x", "0.75x" };
                        g_vk.render_scale = (g_vk.render_scale + 1) % 5;
                        printf("[display] render scale %s (settings)\n", names[g_vk.render_scale]);
                        break;
                    }
                    case 8: g_vk.fov_add = g_vk.fov_add >= 40 ? 0 : g_vk.fov_add + 5; printf("[display] field of view %+d degrees (settings)\n", g_vk.fov_add); break;
                    case 9: g_vk.wide_mode = !g_vk.wide_mode; printf("[display] wide screen %s (settings)\n", g_vk.wide_mode ? "on: the picture takes the window's shape" : "off: 16:9"); break;
                    case 10: g_vk.hud_mode = !g_vk.hud_mode; printf("[display] wide-screen HUD %s (settings)\n", g_vk.hud_mode ? "stretched over the picture" : "16:9 size in the middle"); break;
                    case 11: g_vk.drc_size = (g_vk.drc_size + 1) % 4; break;
                    case 12: g_vk.drc_cursor_speed = (g_vk.drc_cursor_speed + 1) % 4; break;
                    case 13: if (g_vk.present_wait) { g_vk.low_latency = !g_vk.low_latency; printf("[display] low input lag %s (settings)\n", g_vk.low_latency ? "on" : "off"); } break;
                    case 14: g_vk.volume = g_vk.volume <= 0 ? 100 : g_vk.volume - 20; printf("[display] volume %d%% (settings)\n", g_vk.volume); break;
                    case 24:
                        g_vk.sky_mode = (g_vk.sky_mode + 1) % 3;
                        printf("[display] shaders: sky %s (settings)\n", g_vk.sky_mode >= 2 ? "storm" : g_vk.sky_mode ? "Vibrant" : "the game's own");
                        break;
                    case 25:
                        g_vk.sh_clouds = !g_vk.sh_clouds;
                        printf("[display] shaders: 3D clouds %s (settings)\n", g_vk.sh_clouds ? "on" : "off");
                        break;
                    case 26:
                        g_vk.sh_grade = !g_vk.sh_grade;
                        printf("[display] shaders: colour grade %s (settings)\n", g_vk.sh_grade ? "on" : "off");
                        break;
                    case 27:
                        g_vk.sh_quality = (g_vk.sh_quality + 1) % 3;
                        printf("[display] shaders: quality %d (settings)\n", g_vk.sh_quality);
                        break;
                    case 28:
                        g_vk.sh_rays = !g_vk.sh_rays;
                        printf("[display] shaders: god rays & haze %s (settings)\n", g_vk.sh_rays ? "on" : "off");
                        break;
                    case 29:
                        g_vk.sh_cshadow = !g_vk.sh_cshadow;
                        printf("[display] shaders: cloud shadows %s (settings)\n", g_vk.sh_cshadow ? "on" : "off");
                        break;
                    case 30:
                        g_vk.sh_ao = !g_vk.sh_ao;
                        printf("[display] shaders: ambient occlusion %s (settings)\n", g_vk.sh_ao ? "on" : "off");
                        break;
                    case 31:
                        g_vk.sh_rain = !g_vk.sh_rain;
                        printf("[display] shaders: rain %s (settings)\n", g_vk.sh_rain ? "on (with Storm)" : "off");
                        break;
                    case 23:
                        g_vk.shadow_quality = (g_vk.shadow_quality + 2) % 3;   // (sharp -> normal -> off -> sharp)
                        printf("[display] shadows %s (settings)\n", g_vk.shadow_quality == 0 ? "off" : g_vk.shadow_quality == 1 ? "normal" : "sharp (2x map)");
                        break;
                    case 20:
                        g_vk.plaza_30 = !g_vk.plaza_30;
                        printf("[display] plaza frame rate %s (settings)\n", g_vk.plaza_30 ? "30: the game's own there (less work for the processor)" : "60");
                        break;
                    case 33:
                        g_vk.warm_shaders = !g_vk.warm_shaders;
                        printf("[display] shader warm-up at start %s (settings)\n", g_vk.warm_shaders ? "on" : "off");
                        break;
                    case 35:
                        g_vk.frame_gen = g_vk.frame_gen == 0 ? 2 : g_vk.frame_gen == 2 ? 3 : g_vk.frame_gen == 3 ? 4 : 0;
                        printf("[display] smooth motion %s (settings)\n", g_vk.frame_gen == 2 ? "2x (120 fps)" : g_vk.frame_gen == 3 ? "3x (180 fps)" : g_vk.frame_gen == 4 ? "4x (240, needs VSync off)" : "off");
                        break;
                    case 34:
                        g_vk.build_wait = !g_vk.build_wait;
                        printf("[display] new shaders: %s (settings)\n", g_vk.build_wait ? "wait (no flicker)" : "skip (no hitch)");
                        break;
                    case 32:
                        g_vk.pro_pad = g_vk.pro_pad == 0 ? 1 : g_vk.pro_pad == 1 ? 3 : 0;   // (off -> a second controller -> the bot -> off)
                        printf("[display] player 2 %s (settings)\n", g_vk.pro_pad == 3 ? "the bot: the PC plays player 2 in the Battle Dojo" : g_vk.pro_pad ? "on: a second controller is a Wii U Pro Controller (Battle Dojo)" : "off");
                        break;
                    case 36:
                        g_vk.mouse_aim = (g_vk.mouse_aim + 1) % 5;   // (off -> 1x -> 1.5x -> 2x -> 0.5x -> off)
                        printf("[display] mouse aim %s (settings)\n", g_vk.mouse_aim ? "on: click the picture to aim with the mouse, Esc frees it" : "off");
                        break;
                    case 39:
                        g_vk.dojo_both = !g_vk.dojo_both;
                        printf("[display] Dojo views: %s (settings; from the next Dojo match)\n", g_vk.dojo_both ? "both players' views every frame (60 each)" : "alternate frames (30 each, as the console)");
                        break;
                    case 38:
                        g_vk.motion_blur = (g_vk.motion_blur + 1) % 3;   // (off -> light -> strong -> off)
                        printf("[display] motion blur %s (settings)\n", g_vk.motion_blur >= 2 ? "strong" : g_vk.motion_blur ? "light" : "off");
                        break;
                    case 37:
                        g_vk.gyro_on = !g_vk.gyro_on;
                        printf("[display] motion controller %s (settings)\n", g_vk.gyro_on ? "on: a PlayStation pad's gyro aims" : "off");
                        break;
                    case 19:
                        g_vk.pad_layout = !g_vk.pad_layout;
                        printf("[display] button layout %s (settings)\n", g_vk.pad_layout ? "by position (Nintendo: bottom = B, right = A)" : "by label (Xbox: A = A, B = B)");
                        break;
                    case 18:
                        g_vk.tracker_on = !g_vk.tracker_on;
                        printf("[display] frame tracker %s (settings)\n", g_vk.tracker_on ? "shown" : "hidden");
                        break;
                    case 17:
                        g_vk.debug_views_on = !g_vk.debug_views_on;
                        if (!g_vk.debug_views_on) { g_vk.debug_view = 0; g_vk.debug_pick = 0; }
                        printf("[display] debug views %s (settings)\n", g_vk.debug_views_on ? "on: F7 steps through them, F8 = pick (click the picture)" : "off");
                        break;
                    case 16:
                        g_vk.drc_no_popup = !g_vk.drc_no_popup;
                        printf("[display] GamePad pop-up %s (settings)\n", g_vk.drc_no_popup ? "off: only View / M show the GamePad screen" : "on: shown by itself when the TV stops while the GamePad moves");
                        break;
                    case 15:
                        vkctx_perf_set(!g_vk.perf_mode);   // (each keeps its own render scale: Performance mode starts at 0.5x)
                        printf("[display] performance mode %s (settings)\n", g_vk.perf_mode ? "on: render scale 0.5x, anti-aliasing off, the GamePad picture only drawn when shown, scenes that ask for 30 fps keep it" : "off");
                        break;
                    default: break;
                    }
                    vkctx_settings_save();
                } else if (g_vk.debug_pick && g_vk.debug_views_on && !g_vk.drc_on && g_vk.tv_w > 0 && ix >= g_vk.tv_x && ix < g_vk.tv_x + g_vk.tv_w &&
                           iy >= g_vk.tv_y && iy < g_vk.tv_y + g_vk.tv_h) {
                    // F8 pick: the draws under this pixel go to the log (latte_render.c "Debug views")
                    g_vk.pick_x = (ix - g_vk.tv_x) * 1280 / g_vk.tv_w;
                    g_vk.pick_y = (iy - g_vk.tv_y) * 720 / g_vk.tv_h;
                    g_vk.pick_req = 1;
                } else if (g_vk.mouse_aim && !drc && !g_vk.settings_open && !g_vk.debug_pick && !g_vk.mouse_captured) {
                    g_vk.mouse_captured = 1;   // (a click on the picture: the mouse aims now; Esc gives it back)
                    printf("[input] mouse aim: the mouse turns the camera now (Esc frees the mouse)\n");
                }
            }
            was_down = down;
        }
        if (drc && w > 0 && h > 0) {
            // the mouse over the GamePad screen moves the cursor; a click touches
            int ox, oy, ow, oh;
            drc_overlay_rect(w, h, &ox, &oy, &ow, &oh);
            float fx = (float)((mx - ox) / ow), fy = (float)((my - oy) / oh);
            bool inside = fx >= 0 && fx <= 1 && fy >= 0 && fy <= 1;
            if (inside && moved) { g_cur_x = fx; g_cur_y = fy; }
            if (inside && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                g_cur_x = fx; g_cur_y = fy;
                s.touching = true;
            }
        } else if (g_mouse_touch_hidden && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            // (SPLAT_MOUSE_TOUCH=1 only: a click with the GamePad screen hidden
            // touches it blind -- off by default, clicks made menu sounds)
            if (w > 0 && h > 0) {
                float fx = (float)(mx / w), fy = (float)(my / h);
                fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
                fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
                s.touching = true;
                s.tx = (u16)(fx * 3883.0f + 92.0f);           // the panel's raw range
                s.ty = (u16)(4095.0f - fy * 3694.0f - 254.0f);
            }
        }
    }

    {   // Mouse aim: while the game holds the mouse (cursor hidden, movement unlimited) it turns the virtual
        // GamePad's gyro, and the left button shoots (ZR). Given back on Esc (display.c), when the window loses
        // focus, the GamePad screen or the settings panel opens, or the free camera takes the mouse.
        // A real gyro (gyro_hid.c) adds its turns the same way.
        extern volatile int splat_freecam_on;
        extern int gyro_take(float* dpitch, float* dyaw);
        static int held = 0;
        static double hx, hy;
        bool focused = window && glfwGetWindowAttrib(window, GLFW_FOCUSED);
        if (g_vk.mouse_captured && (!g_vk.mouse_aim || !focused || drc || g_vk.settings_open || splat_freecam_on)) g_vk.mouse_captured = 0;
        if (window && g_vk.mouse_captured && !held) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
            glfwGetCursorPos(window, &hx, &hy);
            held = 1;
        } else if (window && !g_vk.mouse_captured && held) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            if (!splat_freecam_on) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            held = 0;
            printf("[input] mouse aim: the mouse is free (click the picture to aim with it again)\n");
        }
        float gp = 0, gy = 0, mdx = 0, mdy = 0;
        if (held) {
            double x, y;
            glfwGetCursorPos(window, &x, &y);
            mdx = (float)(x - hx); mdy = (float)(y - hy);
            hx = x; hy = y;
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) s.hold |= PAD_ZR;
        }
        bool gyro = gyro_take(&gp, &gy) != 0;
        if (held || gyro) {
            static const float sens[5] = { 0, 1.0f, 1.5f, 2.0f, 0.5f };
            float k = 0.04f / 360.0f * sens[g_vk.mouse_aim >= 1 && g_vk.mouse_aim <= 4 ? g_vk.mouse_aim : 1];   // (0.04 degrees a pixel at 1x, times the game's own camera sensitivity)
            lock();
            g_aim_yaw += -mdx * k + gy / 360.0f;
            g_aim_pitch += mdy * k - gp / 360.0f;   // (the game's pitch: + looks down)
            hmtx_unlock(&g_lock);
        }
    }

    {   // SPLAT_IGNORE_REAL_INPUT=1 (unattended tests): only the scripted presses count, so a controller, mouse or
        // keyboard touched while a test window is open can't steer the run
        static int ignore = -1;
        if (ignore < 0) { ignore = getenv("SPLAT_IGNORE_REAL_INPUT") != NULL; if (ignore) printf("[input] real controller / keyboard / mouse ignored (SPLAT_IGNORE_REAL_INPUT)\n"); }
        extern bool splat_bench_active(void);
        if (ignore || splat_bench_active()) { memset(&s, 0, sizeof(s)); memset(&p2, 0, sizeof(p2)); view = false; pad_a = false; }   // (the benchmark too: its route must not be steered)
    }
    autoplay(window, now, &s, &p2);
    stick_bits(&p2);
    {   // (+ pressed: see VkCtx.plus_presses)
        static bool plus_was;
        bool plus = (s.hold & PAD_PLUS) != 0;
        if (plus && !plus_was) g_vk.plus_presses++;
        plus_was = plus;
    }

    if (drc) {
        // GamePad screen up: the right stick moves the cursor instead of the
        // camera, and A touches the screen instead of jumping
        static const float speeds[4] = { 1.1f, 1.65f, 2.2f, 3.3f };   // screen heights per second at full tilt (settings: 1x, 1.5x, 2x, 3x)
        const float speed = speeds[g_vk.drc_cursor_speed & 3];
        g_cur_x += clamp1(s.rx) * speed * dt * (480.0f / 854.0f);
        g_cur_y -= clamp1(s.ry) * speed * dt;
        g_cur_x = g_cur_x < 0 ? 0 : g_cur_x > 1 ? 1 : g_cur_x;
        g_cur_y = g_cur_y < 0 ? 0 : g_cur_y > 1 ? 1 : g_cur_y;
        s.rx = s.ry = 0;
        if (pad_a) { s.hold &= ~(u32)PAD_B; s.touching = true; }
        if (now < g_live_touch_until) s.touching = true;
        if (s.touching) {
            s.tx = (u16)(g_cur_x * 3883.0f + 92.0f);
            s.ty = (u16)(4095.0f - g_cur_y * 3694.0f - 254.0f);
        }
        g_vk.drc_cursor_x = (int)(g_cur_x * 10000.0f);
        g_vk.drc_cursor_y = (int)(g_cur_y * 10000.0f);
    }
    g_vk.drc_touching = s.touching;
    stick_bits(&s);

    {   // the free camera (F4, freecam.c): the controls and the mouse fly it, the Inkling stands still
        extern volatile int splat_freecam_on;
        extern void splat_freecam_input(float lx, float ly, float rx, float ry, unsigned hold, float mdx, float mdy, float dt);
        static int cursor_off = 0;
        static double fc_last = 0, fc_mx = 0, fc_my = 0;
        double now = glfwGetTime();
        float dt = fc_last > 0 ? (float)(now - fc_last) : 0.0f;
        fc_last = now;
        float mdx = 0, mdy = 0;
        if (window) {
            if (splat_freecam_on && !cursor_off) { glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED); glfwGetCursorPos(window, &fc_mx, &fc_my); cursor_off = 1; }
            else if (!splat_freecam_on && cursor_off) { glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); cursor_off = 0; }
            if (cursor_off) {
                double x, y;
                glfwGetCursorPos(window, &x, &y);
                mdx = (float)(x - fc_mx); mdy = (float)(y - fc_my);
                fc_mx = x; fc_my = y;
            }
        }
        if (splat_freecam_on) {
            splat_freecam_input(s.lx, s.ly, s.rx, s.ry, s.hold, mdx, mdy, dt);
            s.lx = s.ly = s.rx = s.ry = 0.0f;
            s.hold = 0;
        }
    }
    if (pro == 3) {   // settings "Player 2" = Bot: the PC plays player 2 (dojo_bot.c)
        extern int splat_bot_pad(double now, float sticks[4], unsigned* bits);
        float st[4] = { 0, 0, 0, 0 };
        unsigned b = 0;
        if (splat_bot_pad(now, st, &b)) {
            p2.lx = st[0]; p2.ly = st[1]; p2.rx = st[2]; p2.ry = st[3];
            p2.hold = (b & 1 ? PAD_ZR : 0) | (b & 2 ? PAD_ZL : 0) | (b & 4 ? PAD_B : 0) | (b & 8 ? PAD_A : 0) | (b & 16 ? PAD_R : 0);
            stick_bits(&p2);
        }
    }
    lock();
    if (!s.touching) { s.tx = g_pad.tx; s.ty = g_pad.ty; }   // games expect the last spot to stay
    g_pad = s;
    g_pro = p2;
    g_pro_connected = pro == 2 || pro == 3 || (pro == 1 && second);
    hmtx_unlock(&g_lock);
}

// Is ZR (shoot) held right now? (for the log: drops while shooting)
bool input_zr_held(void) { return (g_pad.hold & PAD_ZR) != 0; }
// Is ZL (squid form) held right now?
bool input_zl_held(void) { return (g_pad.hold & PAD_ZL) != 0; }

bool input_gamepad_a_pressed(void) {
    lock();
    bool a = (g_pad.hold & PAD_A) != 0;
    hmtx_unlock(&g_lock);
    return a;
}

static void write_float(u32 addr, float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    wiiu_write32(addr, bits);
}

// VPADRead(channel, VPADStatus* buffer, count, s32* error) -> samples.
// VPADStatus (0xAC bytes): hold 0x00, trigger 0x04, release 0x08, left
// stick 0x0C, right stick 0x14, accelerometer 0x1C.., accXY 0x30, gyro 0x38,
// angle 0x44, error 0x50, touch 0x52/0x5A/0x62 (x, y, touched, validity),
// direction matrix 0x6C, magnet 0x94, volume 0xA0, battery 0xA1.
static void VPADRead(CPUState* c) {
    u32 chan = arg(c, 0), buf = arg(c, 1), count = arg(c, 2), err = arg(c, 3);
    if (chan != 0 || !buf || !count) {
        if (err) wiiu_write32(err, (u32)-2);   // no controller on that channel
        ret(c, 0);
        return;
    }
    lock();
    PadState s = g_pad;
    hmtx_unlock(&g_lock);

    static u32 last_hold = 0;
    u8* p = wiiu_mem_ptr(buf, 0xAC);
    if (p) memset(p, 0, 0xAC);
    wiiu_write32(buf + 0x00, s.hold);
    wiiu_write32(buf + 0x04, s.hold & ~last_hold);
    wiiu_write32(buf + 0x08, last_hold & ~s.hold);
    last_hold = s.hold;
    write_float(buf + 0x0C, s.lx);
    write_float(buf + 0x10, s.ly);
    write_float(buf + 0x14, s.rx);
    write_float(buf + 0x18, s.ry);
    // Motion aiming: Splatoon aims by tilting and turning the GamePad (gyro).
    // The right stick moves a virtual GamePad instead: up/down tilts it
    // (pushing up looks down, like an inverted camera) and left/right turns it.
    {
        static float pitch = 0.0f, yaw = 0.0f;            // in turns (1.0 = 360 degrees)
        static double last = 0.0;
        double now = glfwGetTime();
        float dt = last > 0.0 ? (float)(now - last) : 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        const float pitch_speed = 0.3f, yaw_speed = 0.4f;  // turns per second at full tilt
        float prate = -s.ry * pitch_speed;
        float yrate = -s.rx * yaw_speed;
        pitch += prate * dt;
        yaw += yrate * dt;
        {   // (the mouse / a real gyro: turns since the last read, input_poll)
            lock();
            float ap = g_aim_pitch, ay = g_aim_yaw;
            g_aim_pitch = g_aim_yaw = 0.0f;
            hmtx_unlock(&g_lock);
            pitch += ap;
            yaw += ay;
            if (dt > 0.002f) { prate += ap / dt; yrate += ay / dt; }
        }
        if (yaw > 1.0f) yaw -= 1.0f;
        if (yaw < -1.0f) yaw += 1.0f;
        if (pitch > 0.22f) { pitch = 0.22f; prate = 0.0f; }
        if (pitch < -0.22f) { pitch = -0.22f; prate = 0.0f; }
        if (s.hold & PAD_Y) pitch = 0.0f;                  // Y re-centres the camera, like on the console
        static float base = -9, rscale = 1;               // (tests: SPLAT_GYRO_BASE = the held tilt in turns, SPLAT_GYRO_RATE = rate scale)
        if (base == -9) { const char* e = getenv("SPLAT_GYRO_BASE"); base = e ? (float)atof(e) : 0.0f; e = getenv("SPLAT_GYRO_RATE"); rscale = e ? (float)atof(e) : 1.0f; }
        prate *= rscale; yrate *= rscale;
        float a = (pitch + base) * 6.2831853f, cp = cosf(a), sp = sinf(a);
        float b = yaw * 6.2831853f, cy = cosf(b), sy = sinf(b);
        // accelerometer: just gravity, as felt by a GamePad held at that tilt
        // (all zeros made the game's motion correction pull the aim sideways)
        write_float(buf + 0x1C, 0.0f);
        write_float(buf + 0x20, -cp);
        write_float(buf + 0x24, sp);
        write_float(buf + 0x28, 1.0f);                    // magnitude
        write_float(buf + 0x2C, 0.0f);                    // variation
        write_float(buf + 0x30, 1.0f);                    // accXY: held level (as before)
        write_float(buf + 0x38, prate);                   // gyro x: tilt speed
        write_float(buf + 0x3C, yrate);                   // gyro y: turn speed
        write_float(buf + 0x44, pitch);                   // angle x
        write_float(buf + 0x48, yaw);                     // angle y
        // direction: the GamePad's x, y, z axes, tilted then turned
        float dir[9] = { cy, 0, -sy,  sp * sy, cp, sp * cy,  cp * sy, -sp, cp * cy };
        for (int i = 0; i < 9; i++) write_float(buf + 0x6C + i * 4, dir[i]);
    }
    for (int i = 0; i < 3; i++) {
        u32 t = buf + 0x52 + i * 8;
        wiiu_write16(t + 0, s.tx);
        wiiu_write16(t + 2, s.ty);
        wiiu_write16(t + 4, s.touching ? 1 : 0);
        wiiu_write16(t + 6, s.touching ? 0 : 3);          // 3 = no valid position
    }
    wiiu_write8(buf + 0xA0, 0xFF);                        // volume slider
    wiiu_write8(buf + 0xA1, 0xC0);                        // battery full
    wiiu_write8(buf + 0xA3, 0xFF);
    if (err) wiiu_write32(err, 0);
    ret(c, 1);
}

// (channel, TPData* out, const TPData* in): the panel is already calibrated.
static void VPADGetTPCalibratedPoint(CPUState* c) {
    u8* out = wiiu_mem_ptr(arg(c, 1), 8);
    u8* in = wiiu_mem_ptr(arg(c, 2), 8);
    if (out && in) memmove(out, in, 8);
    if (out) {   // map the raw range onto the 1280x720 screen like the console does
        u32 o = arg(c, 1);
        u16 x = wiiu_read16(o), y = wiiu_read16(o + 2);
        float fx = ((float)x - 92.0f) / 3883.0f, fy = (4095.0f - 254.0f - (float)y) / 3694.0f;
        wiiu_write16(o, (u16)(clamp1(fx) < 0 ? 0 : clamp1(fx) * 1279.0f));
        wiiu_write16(o + 2, (u16)(clamp1(fy) < 0 ? 0 : clamp1(fy) * 719.0f));
    }
    ret(c, 0);
}
static void VPADGetLcdMode(CPUState* c) {   // (channel, *mode): 1 = screen on
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 1);
    ret(c, 0);
}

// Wii Remotes / Pro Controllers: none, or player 2's Pro Controller on channel 0
// (settings "Player 2"). The game polls WPADProbe for what's on a channel and
// reads it with KPADReadEx.
enum { WPAD_DEV_URCC = 31, WPAD_DEV_NONE = 0xFD, WPAD_FMT_URCC = 22 };
// Pro Controller buttons (KPADStatus's Pro part)
enum {
    PRO_UP = 0x0001, PRO_LEFT = 0x0002, PRO_ZR = 0x0004, PRO_X = 0x0008, PRO_A = 0x0010, PRO_Y = 0x0020,
    PRO_B = 0x0040, PRO_ZL = 0x0080, PRO_R = 0x0200, PRO_PLUS = 0x0400, PRO_HOME = 0x0800, PRO_MINUS = 0x1000,
    PRO_L = 0x2000, PRO_DOWN = 0x4000, PRO_RIGHT = 0x8000, PRO_STICK_R = 0x10000, PRO_STICK_L = 0x20000,
    PRO_L_LEFT = 0x40000, PRO_L_RIGHT = 0x80000, PRO_L_DOWN = 0x100000, PRO_L_UP = 0x200000,
    PRO_R_LEFT = 0x400000, PRO_R_RIGHT = 0x800000, PRO_R_DOWN = 0x1000000, PRO_R_UP = 0x2000000,
};
static u32 pro_buttons(u32 hold) {
    static const struct { u32 pad, pro; } m[] = {
        { PAD_A, PRO_A }, { PAD_B, PRO_B }, { PAD_X, PRO_X }, { PAD_Y, PRO_Y },
        { PAD_UP, PRO_UP }, { PAD_DOWN, PRO_DOWN }, { PAD_LEFT, PRO_LEFT }, { PAD_RIGHT, PRO_RIGHT },
        { PAD_ZL, PRO_ZL }, { PAD_ZR, PRO_ZR }, { PAD_L, PRO_L }, { PAD_R, PRO_R },
        { PAD_PLUS, PRO_PLUS }, { PAD_MINUS, PRO_MINUS }, { PAD_HOME, PRO_HOME },
        { PAD_STICK_L, PRO_STICK_L }, { PAD_STICK_R, PRO_STICK_R },
        { PAD_L_LEFT, PRO_L_LEFT }, { PAD_L_RIGHT, PRO_L_RIGHT }, { PAD_L_UP, PRO_L_UP }, { PAD_L_DOWN, PRO_L_DOWN },
        { PAD_R_LEFT, PRO_R_LEFT }, { PAD_R_RIGHT, PRO_R_RIGHT }, { PAD_R_UP, PRO_R_UP }, { PAD_R_DOWN, PRO_R_DOWN },
    };
    u32 b = 0;
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) if (hold & m[i].pad) b |= m[i].pro;
    return b;
}
static bool pro_there(u32 chan) {
    lock();
    bool on = chan == 0 && g_pro_connected;
    hmtx_unlock(&g_lock);
    return on;
}

// Every Wii Remote / Pro Controller call the game makes goes to the log the first 3 times ("[pad]" lines).
static void pad_log(const char* name, CPUState* c, u32 result) {
    static struct { const char* name; u32 calls; } seen[64];
    int i = 0;
    while (i < 64 && seen[i].name && seen[i].name != name) i++;
    if (i == 64) return;
    seen[i].name = name;
    if (++seen[i].calls <= 3)
        printf("[pad] %s(%08X, %08X, %08X, %08X) -> %08X%s (from %08X)\n", name, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), result,
               seen[i].calls == 3 ? " (not logged again)" : "", c->lr - 4);
}
#define PAD_CALL(fn, value) static void fn(CPUState* c) { pad_log(#fn, c, (u32)(value)); ret(c, (u32)(value)); }

static void WPADProbe(CPUState* c) {        // (channel, *type) -> 0 and the type, or -1: nothing there
    u32 chan = arg(c, 0);
    bool on = pro_there(chan);
    static int was[8];   // (0 not asked yet, 1 nothing, 2 a Pro Controller)
    if (chan < 8 && was[chan] != (on ? 2 : 1)) {
        if (was[chan] || on) printf("[pad] channel %u: %s\n", chan, on ? "Pro Controller connected (player 2)" : "nothing connected");
        was[chan] = on ? 2 : 1;
    }
    pad_log("WPADProbe", c, on ? 0 : (u32)-1);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), on ? WPAD_DEV_URCC : WPAD_DEV_NONE);
    ret(c, on ? 0 : (u32)-1);
}
// KPADStatus (0xF0 bytes): Wii Remote buttons hold / trigger / release 0x00..0x08, motion and pointer
// 0x0C..0x5B (none on a Pro Controller), extension type 0x5C, error 0x5D, pointer valid 0x5E, data format
// 0x5F; then the Pro Controller part at 0x60: hold, trigger, release, left stick 0x6C, right stick 0x74
// (floats, -1..1, up is +), charging 0x7C, wired 0x80.
static void kpad_read(CPUState* c, const char* name, u32 err) {   // (channel, KPADStatus* buffer, count, ...) -> samples
    u32 chan = arg(c, 0), buf = arg(c, 1), count = arg(c, 2);
    if (!buf || !count || !pro_there(chan)) {
        pad_log(name, c, 0);
        if (err) wiiu_write32(err, (u32)-2);   // (no controller on that channel)
        ret(c, 0);
        return;
    }
    pad_log(name, c, 1);
    lock();
    PadState s = g_pro;
    hmtx_unlock(&g_lock);
    static u32 last_hold = 0;
    u32 hold = pro_buttons(s.hold);
    u8* p = wiiu_mem_ptr(buf, 0xF0);
    if (p) memset(p, 0, 0xF0);
    wiiu_write8(buf + 0x5C, WPAD_DEV_URCC);
    wiiu_write8(buf + 0x5F, WPAD_FMT_URCC);
    wiiu_write32(buf + 0x60, hold);
    wiiu_write32(buf + 0x64, hold & ~last_hold);
    wiiu_write32(buf + 0x68, last_hold & ~hold);
    last_hold = hold;
    write_float(buf + 0x6C, s.lx);
    write_float(buf + 0x70, s.ly);
    write_float(buf + 0x74, s.rx);
    write_float(buf + 0x78, s.ry);
    wiiu_write32(buf + 0x80, 1);                          // wired: no battery to run out
    if (err) wiiu_write32(err, 0);
    ret(c, 1);
}
static void KPADReadEx(CPUState* c) { kpad_read(c, "KPADReadEx", arg(c, 3)); }   // (channel, data, count, s32* error)
static void KPADRead(CPUState* c) { kpad_read(c, "KPADRead", 0); }              // (channel, data, count)
static void WPADIsMplsAttached(CPUState* c) {   // (channel, BOOL* attached, callback): no Motion Plus on a Pro Controller
    bool on = pro_there(arg(c, 0));
    pad_log("WPADIsMplsAttached", c, on ? 0 : (u32)-1);
    if (arg(c, 1)) wiiu_write32(arg(c, 1), 0);
    ret(c, on ? 0 : (u32)-1);
}
static void nothing(CPUState* c) { ret(c, 0); }
PAD_CALL(KPADGetMplsWorkSize, 0x5FE0)
PAD_CALL(WPADGetBatteryLevel, pro_there(arg(c, 0)) ? 4 : 0)   // (4: full)
PAD_CALL(WPADIsMotorEnabled, 1)
PAD_CALL(WPADInit, 0) PAD_CALL(WPADEnableURCC, 0) PAD_CALL(WPADEnableWiiRemote, 0) PAD_CALL(WPADControlMotor, 0)
PAD_CALL(WPADSetAutoSleepTime, 0) PAD_CALL(WPADSetConnectCallback, 0) PAD_CALL(WPADSetExtensionCallback, 0) PAD_CALL(WPADDisconnect, 0)
PAD_CALL(WPADControlSpeaker, -1) PAD_CALL(WPADCanSendStreamData, 0) PAD_CALL(WPADSendStreamData, -1) PAD_CALL(WENCGetEncodeData, 0)   // (no speaker)
PAD_CALL(KPADInit, 0) PAD_CALL(KPADInitEx, 0) PAD_CALL(KPADShutdown, 0) PAD_CALL(KPADSetMplsWorkarea, 0) PAD_CALL(KPADSetPosParam, 0)
PAD_CALL(KPADEnableStickCrossClamp, 0) PAD_CALL(KPADDisableStickCrossClamp, 0) PAD_CALL(KPADGetMplsZeroDriftMode, 0)
PAD_CALL(KPADSetMplsZeroDriftMode, 0) PAD_CALL(KPADSetMplsDirection, 0) PAD_CALL(KPADDisableMplsDirRevise, 0)
PAD_CALL(KPADEnableMplsDirRevise, 0) PAD_CALL(KPADSetMplsDirReviseBase, 0) PAD_CALL(KPADSetMplsDirReviseParam, 0)
PAD_CALL(KPADEnableMplsDpdRevise, 0) PAD_CALL(KPADDisableMplsDpdRevise, 0) PAD_CALL(KPADSetMplsDpdReviseParam, 0)
PAD_CALL(KPADEnableMpls, 0) PAD_CALL(KPADDisableMpls, 0) PAD_CALL(KPADSetConnectCallback, 0) PAD_CALL(KPADSetControlDpdCallback, 0)
PAD_CALL(KPADEnableDPD, 0) PAD_CALL(KPADDisableDPD, 0) PAD_CALL(KPADResetMpls, 0)

static const HleEntry g_input_table[] = {
    { "VPADRead", VPADRead },
    { "VPADGetTPCalibratedPoint", VPADGetTPCalibratedPoint },
    { "VPADGetTPCalibratedPointEx", VPADGetTPCalibratedPoint },
    { "VPADGetLcdMode", VPADGetLcdMode },
    { "VPADInit", nothing },
    { "VPADShutdown", nothing },
    { "VPADSetLcdMode", nothing },
    { "VPADControlMotor", nothing },
    { "VPADStopMotor", nothing },
    { "VPADSetTPCalibrationParam", nothing },
    { "VPADGetTPCalibrationParam", nothing },
    { "VPADSetAccParam", nothing },
    { "VPADSetBtnRepeat", nothing },
    { "VPADEnableStickCrossClamp", nothing },
    { "VPADDisableStickCrossClamp", nothing },
    { "VPADSetStickOrigin", nothing },
    { "VPADSetCrossStickEmulationParamsL", nothing },
    { "VPADSetCrossStickEmulationParamsR", nothing },
    { "VPADGetGyroZeroDriftMode", nothing },
    { "VPADSetGyroZeroDriftMode", nothing },
    { "VPADSetGyroDirection", nothing },
    { "VPADSetGyroDirectionMag", nothing },
    { "VPADDisableGyroDirRevise", nothing },
    { "VPADEnableGyroDirRevise", nothing },
    { "VPADSetGyroDirReviseBase", nothing },
    { "VPADSetGyroDirReviseParam", nothing },
    { "VPADDisableGyroAccRevise", nothing },
    { "VPADEnableGyroAccRevise", nothing },
    { "VPADDisableGyroZeroPlay", nothing },
    { "VPADEnableGyroZeroPlay", nothing },
    { "VPADDisableGyroZeroDrift", nothing },
    { "VPADEnableGyroZeroDrift", nothing },
    { "VPADInitGyroZeroDriftMode", nothing },
    { "VPADInitGyroAccReviseParam", nothing },
    { "VPADInitGyroDirReviseParam", nothing },
    { "VPADInitGyroZeroPlayParam", nothing },
    { "VPADSetGyroAngle", nothing },
    { "VPADBASESetMotorOnRemainingCount", nothing },
    { "VPADBASEGetMotorOnRemainingCount", nothing },

    { "WPADProbe", WPADProbe },
    { "WPADInit", WPADInit },
    { "WPADEnableURCC", WPADEnableURCC },
    { "WPADEnableWiiRemote", WPADEnableWiiRemote },
    { "WPADControlMotor", WPADControlMotor },
    { "WPADIsMotorEnabled", WPADIsMotorEnabled },
    { "WPADIsMplsAttached", WPADIsMplsAttached },
    { "WPADDisconnect", WPADDisconnect },
    { "WPADControlSpeaker", WPADControlSpeaker },
    { "WPADCanSendStreamData", WPADCanSendStreamData },
    { "WPADSendStreamData", WPADSendStreamData },
    { "WENCGetEncodeData", WENCGetEncodeData },
    { "WPADSetAutoSleepTime", WPADSetAutoSleepTime },
    { "WPADSetConnectCallback", WPADSetConnectCallback },
    { "WPADSetExtensionCallback", WPADSetExtensionCallback },
    { "WPADGetBatteryLevel", WPADGetBatteryLevel },
    { "KPADReadEx", KPADReadEx },
    { "KPADRead", KPADRead },
    { "KPADInit", KPADInit },
    { "KPADInitEx", KPADInitEx },
    { "KPADShutdown", KPADShutdown },
    { "KPADGetMplsWorkSize", KPADGetMplsWorkSize },
    { "KPADSetMplsWorkarea", KPADSetMplsWorkarea },
    { "KPADSetPosParam", KPADSetPosParam },
    { "KPADEnableStickCrossClamp", KPADEnableStickCrossClamp },
    { "KPADDisableStickCrossClamp", KPADDisableStickCrossClamp },
    { "KPADGetMplsZeroDriftMode", KPADGetMplsZeroDriftMode },
    { "KPADSetMplsZeroDriftMode", KPADSetMplsZeroDriftMode },
    { "KPADSetMplsDirection", KPADSetMplsDirection },
    { "KPADDisableMplsDirRevise", KPADDisableMplsDirRevise },
    { "KPADEnableMplsDirRevise", KPADEnableMplsDirRevise },
    { "KPADSetMplsDirReviseBase", KPADSetMplsDirReviseBase },
    { "KPADSetMplsDirReviseParam", KPADSetMplsDirReviseParam },
    { "KPADEnableMplsDpdRevise", KPADEnableMplsDpdRevise },
    { "KPADDisableMplsDpdRevise", KPADDisableMplsDpdRevise },
    { "KPADSetMplsDpdReviseParam", KPADSetMplsDpdReviseParam },
    { "KPADEnableMpls", KPADEnableMpls },
    { "KPADDisableMpls", KPADDisableMpls },
    { "KPADSetConnectCallback", KPADSetConnectCallback },
    { "KPADSetControlDpdCallback", KPADSetControlDpdCallback },
    { "KPADEnableDPD", KPADEnableDPD },
    { "KPADDisableDPD", KPADDisableDPD },
    { "KPADResetMpls", KPADResetMpls },
};

const HleEntry* input_hle_table(size_t* count) {
    *count = sizeof(g_input_table) / sizeof(g_input_table[0]);
    return g_input_table;
}
