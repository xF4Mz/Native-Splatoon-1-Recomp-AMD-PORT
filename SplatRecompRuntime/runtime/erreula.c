// The system error viewer (erreula.rpl): the box that shows messages like
// "Could not connect to the Internet" over the game, with OK / Yes / No
// buttons. There's no box to draw yet, so a shown message is printed to the
// log instead and answered automatically: after a short moment it counts as
// "left button pressed" (OK / the first choice), or the GamePad's A button
// answers it sooner. That's enough for games that wait for the player to
// close a message before carrying on.
#include "erreula.h"
#include "input.h"
#include "guest_threads.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <string.h>

enum { HIDDEN = 0, APPEARING = 1, VISIBLE = 2, DISAPPEARING = 3 };
enum { NONE = -1, LEFT = 0, RIGHT = 1 };

#define FADE_TICKS (WIIU_TIMER_HZ * 80 / 1000)        // 80 ms fade in/out
#define AUTO_ANSWER_TICKS (WIIU_TIMER_HZ * 2)          // answer after 2 s

static bool g_created = false;
static int g_state = HIDDEN;
static u64 g_state_since = 0;
static int g_button = NONE;
static s32 g_result_code = -1;
static bool g_home_nix_sign = false;

static void set_state(int s) { g_state = s; g_state_since = wiiu_time_ticks(); }

// Print a UTF-16 message from guest memory (non-ASCII shown as '?').
static void print_text(const char* label, u32 addr) {
    if (!addr) return;
    char buf[512];
    u32 n = 0;
    for (u32 i = 0; i < 1000 && n + 1 < sizeof(buf); i++) {
        u16 ch = wiiu_read16(addr + i * 2);
        if (!ch) break;
        buf[n++] = ch == '\n' ? ' ' : (ch < 0x80 ? (char)ch : '?');
    }
    buf[n] = 0;
    if (n) printf("[erreula]   %s: %s\n", label, buf);
}

static void ErrEulaCreate(CPUState* c) {   // (work memory, region, language, FSClient*)
    g_created = true;
    g_button = NONE;
    g_result_code = -1;
    set_state(HIDDEN);
    (void)c;
}
static void ErrEulaDestroy(CPUState* c) { g_created = false; (void)c; }

// (const AppearArg*): type, screen, controller, hold type, error code,
// frame rate, text, button 1 text, button 2 text, title.
static void ErrEulaAppearError(CPUState* c) {
    u32 a = arg(c, 0);
    if (!g_created) return;
    u32 type = wiiu_read32(a + 0x00), code = wiiu_read32(a + 0x10);
    printf("[erreula] the game shows a system message (type %u, error code %u); answering it automatically\n",
           type, code);
    print_text("text", wiiu_read32(a + 0x18));
    print_text("button 1", wiiu_read32(a + 0x1C));
    print_text("button 2", wiiu_read32(a + 0x20));
    print_text("title", wiiu_read32(a + 0x24));
    fflush(stdout);
    g_button = NONE;
    g_result_code = -1;
    set_state(APPEARING);
}
static void ErrEulaDisappearError(CPUState* c) {
    if (g_state == VISIBLE) set_state(DISAPPEARING);
    (void)c;
}

static void ErrEulaCalc(CPUState* c) {   // (const ControllerInfo*), once per frame
    (void)c;
    if (!g_created) return;
    u64 now = wiiu_time_ticks();
    if ((g_state == APPEARING || g_state == DISAPPEARING) && now - g_state_since > FADE_TICKS) {
        g_state = g_state == APPEARING ? VISIBLE : HIDDEN;
        g_state_since = now;
    }
    if (g_state == VISIBLE && g_button == NONE &&
        (now - g_state_since > AUTO_ANSWER_TICKS || input_gamepad_a_pressed())) {
        g_button = LEFT;
        g_result_code = 0;
        printf("[erreula] message answered (first button)\n");
    }
}

static void ErrEulaGetStateErrorViewer(CPUState* c) { ret(c, g_created ? (u32)g_state : HIDDEN); }
static void ErrEulaIsDecideSelectButtonError(CPUState* c) { ret(c, g_created && g_button != NONE); }
static void ErrEulaIsDecideSelectLeftButtonError(CPUState* c) { ret(c, g_created && g_button == LEFT); }
static void ErrEulaIsDecideSelectRightButtonError(CPUState* c) { ret(c, g_created && g_button == RIGHT); }
static void ErrEulaGetResultCode(CPUState* c) { ret(c, g_created ? (u32)g_result_code : (u32)-1); }
// None 0, Finish 1 (a button, codes below 10), Next 2, Jump 3, Password 4.
static void ErrEulaGetResultType(CPUState* c) {
    s32 r = g_created ? g_result_code : -1;
    ret(c, r == -1 ? 0 : r < 10 ? 1 : r >= 9999 ? 2 : r == 40 ? 4 : 3);
}
static void ErrEulaAppearHomeNixSign(CPUState* c) { g_home_nix_sign = true; ret(c, 0); }
static void ErrEulaDisappearHomeNixSign(CPUState* c) { g_home_nix_sign = false; ret(c, 0); }
static void ErrEulaIsAppearHomeNixSign(CPUState* c) { ret(c, g_home_nix_sign); }
static void nothing(CPUState* c) { ret(c, 0); }

static const HleEntry g_erreula_table[] = {
    { "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", ErrEulaCreate },
    { "ErrEulaDestroy__3RplFv", ErrEulaDestroy },
    { "ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", ErrEulaAppearError },
    { "ErrEulaDisappearError__3RplFv", ErrEulaDisappearError },
    { "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo", ErrEulaCalc },
    { "ErrEulaDrawTV__3RplFv", nothing },
    { "ErrEulaDrawDRC__3RplFv", nothing },
    { "ErrEulaGetStateErrorViewer__3RplFv", ErrEulaGetStateErrorViewer },
    { "ErrEulaGetResultType__3RplFv", ErrEulaGetResultType },
    { "ErrEulaGetResultCode__3RplFv", ErrEulaGetResultCode },
    { "ErrEulaIsDecideSelectButtonError__3RplFv", ErrEulaIsDecideSelectButtonError },
    { "ErrEulaIsDecideSelectLeftButtonError__3RplFv", ErrEulaIsDecideSelectLeftButtonError },
    { "ErrEulaIsDecideSelectRightButtonError__3RplFv", ErrEulaIsDecideSelectRightButtonError },
    { "ErrEulaIsAppearHomeNixSign__3RplFv", ErrEulaIsAppearHomeNixSign },
    { "ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", ErrEulaAppearHomeNixSign },
    { "ErrEulaDisappearHomeNixSign__3RplFv", ErrEulaDisappearHomeNixSign },
    { "ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", nothing },
    { "ErrEulaSetVersion__3RplFi", nothing },
    { "ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType", nothing },
    { "ErrEulaPlayAppearSE__3RplFb", nothing },
    { "ErrEulaIsSelectCursorActive__3RplFv", nothing },
};

const HleEntry* erreula_hle_table(size_t* count) {
    *count = sizeof(g_erreula_table) / sizeof(g_erreula_table[0]);
    return g_erreula_table;
}
