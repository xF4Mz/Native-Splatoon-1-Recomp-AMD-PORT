// The game window (see display.h). This thread makes the window, connects
// Vulkan to it (vk_ctx.c), and then just keeps the window responsive: it
// reads the keyboard/controller and updates the title. The GPU thread
// (latte_render.c) draws the game's frames and puts them in the window.
//
// Keys: F1 switches between the game and the texture gallery, F12 saves a
// screenshot (imagesrame_XXXX.png).
#include "display.h"
#include "host_thread.h"
#include "vk_ctx.h"
#include "texture_gallery.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "input.h"
#include "profiler.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <psapi.h>
#endif

void runtime_exit(int code);

static volatile uint32_t g_game_frames = 0;   // written by the game, read here

#ifdef _WIN32
// Until the game's first picture, Windows would show the empty window white:
// paint it black ourselves (GLFW leaves the background unpainted).
static WNDPROC g_glfw_proc;
static LRESULT CALLBACK black_until_first_picture(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_vk.presented == 0 && (m == WM_ERASEBKGND || m == WM_PAINT)) {
        PAINTSTRUCT ps;
        HDC dc = m == WM_PAINT ? BeginPaint(h, &ps) : (HDC)w;
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (m == WM_PAINT) { EndPaint(h, &ps); return 0; }
        return 1;
    }
    return CallWindowProcW(g_glfw_proc, h, m, w, l);
}
#endif

static void on_resize(GLFWwindow* w, int width, int height) {
    (void)w; (void)width; (void)height;
    g_vk.resized = 1;
}

// F11 / Alt+Enter: borderless fullscreen on the monitor the window is mostly
// on, and back to the window as it was. (Borderless, not exclusive: no
// display mode change, so switching is instant and Alt+Tab stays smooth.)
static int g_fs_on, g_fs_x, g_fs_y, g_fs_w, g_fs_h;
static void toggle_fullscreen(GLFWwindow* w) {
    if (!g_fs_on) {
        glfwGetWindowPos(w, &g_fs_x, &g_fs_y);
        glfwGetWindowSize(w, &g_fs_w, &g_fs_h);
        int n = 0;
        GLFWmonitor** mons = glfwGetMonitors(&n);
        GLFWmonitor* best = glfwGetPrimaryMonitor();
        int best_area = -1;
        for (int i = 0; i < n; i++) {   // (the monitor most of the window is on)
            int mx, my;
            glfwGetMonitorPos(mons[i], &mx, &my);
            const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
            if (!vm) continue;
            int ox = (g_fs_x + g_fs_w < mx + vm->width ? g_fs_x + g_fs_w : mx + vm->width) - (g_fs_x > mx ? g_fs_x : mx);
            int oy = (g_fs_y + g_fs_h < my + vm->height ? g_fs_y + g_fs_h : my + vm->height) - (g_fs_y > my ? g_fs_y : my);
            int area = ox > 0 && oy > 0 ? ox * oy : 0;
            if (area > best_area) { best_area = area; best = mons[i]; }
        }
        const GLFWvidmode* vm = best ? glfwGetVideoMode(best) : NULL;
        if (!vm) return;
        int mx, my;
        glfwGetMonitorPos(best, &mx, &my);
        glfwSetWindowAttrib(w, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowPos(w, mx, my);
        glfwSetWindowSize(w, vm->width, vm->height);
        g_fs_on = 1;
        printf("[display] fullscreen %dx%d (F11 or Alt+Enter for the window again)\n", vm->width, vm->height);
    } else {
        glfwSetWindowAttrib(w, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowSize(w, g_fs_w, g_fs_h);
        glfwSetWindowPos(w, g_fs_x, g_fs_y);
        g_fs_on = 0;
        printf("[display] windowed %dx%d\n", g_fs_w, g_fs_h);
    }
    g_vk.resized = 1;
}

#ifdef _WIN32
static WNDPROC g_prev_proc;
static LRESULT CALLBACK no_menu_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;   // (no keyboard menu mode: see where this is installed)
    if (m == WM_CLOSE) printf("[display] the system asked the window to close (close button, Alt+F4 or the window menu)\n");
    return CallWindowProcW(g_prev_proc, h, m, w, l);
}
#endif
int splat_lobby_on(void);
void splat_lobby_key(int key, int shift);
static void on_key(GLFWwindow* w, int key, int scancode, int action, int mods) {
    (void)w; (void)scancode; (void)mods;
    if (action != GLFW_PRESS) return;
    { static int ignore = -1; if (ignore < 0) ignore = getenv("SPLAT_IGNORE_REAL_INPUT") != NULL; if (ignore) return; }   // (unattended tests: see input.c)
    if (key == GLFW_KEY_F11 || (key == GLFW_KEY_ENTER && (mods & GLFW_MOD_ALT))) {
        toggle_fullscreen(w);
    } else if (key == GLFW_KEY_F1 && (mods & GLFW_MOD_CONTROL)) {
        g_vk.gallery = !g_vk.gallery;
        printf("[display] showing %s\n", g_vk.gallery ? "the texture gallery (Ctrl+F1 to go back)" : "the game");
    } else if (key == GLFW_KEY_F1) {   // (the HUD / menus off, like Minecraft's F1: clean shots)
        g_vk.hide_hud = !g_vk.hide_hud;
        printf("[display] HUD %s (F1)\n", g_vk.hide_hud ? "hidden" : "shown");
    } else if (key == GLFW_KEY_ESCAPE && g_vk.mouse_captured) {   // (mouse aim: the mouse back to the desktop, input.c)
        g_vk.mouse_captured = 0;
    } else if (key == GLFW_KEY_F10) {   // (the SETTINGS box on / off, in fullscreen too -- like F3 for the counter)
        g_vk.hide_settings = !g_vk.hide_settings;
        if (g_vk.hide_settings) g_vk.settings_open = 0;
        vkctx_settings_save();
    } else if (key == GLFW_KEY_F2) {    // (the panel open / closed)
        g_vk.settings_open = !g_vk.settings_open;
        if (g_vk.settings_open) g_vk.hide_settings = 0;
    } else if (key == GLFW_KEY_F9) {
        g_vk.want_dump = 1;
        printf("[display] F9: writing the freeze watchdog's dump (what the game is doing / waiting for)\n");
    } else if (key == GLFW_KEY_F3) {
        g_vk.hide_fps = !g_vk.hide_fps;
        vkctx_settings_save();
    } else if (key == GLFW_KEY_F4 && (mods & GLFW_MOD_CONTROL)) {
        g_vk.no_mips = !g_vk.no_mips;
        printf("[display] texture mip levels %s (Ctrl+F4)\n", g_vk.no_mips ? "off" : "on");
    } else if (key == GLFW_KEY_F4) {   // (the free camera: freecam.c)
        extern void splat_freecam_toggle(void);
        splat_freecam_toggle();
    } else if ((key == GLFW_KEY_F6 || key == GLFW_KEY_F7 || key == GLFW_KEY_F8) && !(mods & GLFW_MOD_CONTROL) && splat_lobby_on()) {
        // (the Bot Lobby, bot_lobby.c: F6 start, F7 / Shift+F7 map, F8 the bot's skill)
        splat_lobby_key(key == GLFW_KEY_F6 ? 6 : key == GLFW_KEY_F7 ? 7 : 8, (mods & GLFW_MOD_SHIFT) != 0);
    } else if (key == GLFW_KEY_F6 && (mods & GLFW_MOD_CONTROL)) {
        g_vk.tracker_on = !g_vk.tracker_on;
        printf("[display] frame tracker %s (Ctrl+F6)\n", g_vk.tracker_on ? "shown: the last 300 frames by cause, bottom left" : "hidden");
    } else if (key == GLFW_KEY_F6) {   // (the built-in benchmark, from the plaza: benchmark.c)
        extern void splat_bench_start(void);
        splat_bench_start();
    } else if (key == GLFW_KEY_F7 || key == GLFW_KEY_F8) {
        // Debug views (latte_render.c): F7 steps through them, F8 = pick (a click prints the draws there).
        // Off unless the settings row "Debug views" is on (or SPLAT_DEBUG_VIEWS=1).
        static const char* const names[6] = { "normal", "wireframe", "wireframe + shader colors", "solid shader colors", "overdraw heatmap", "skipped/pending draws + NaN uniforms" };
        if (!g_vk.debug_views_on) printf("[display] F7 / F8: the debug views are off (settings row \"Debug views\", or SPLAT_DEBUG_VIEWS=1)\n");
        else if (key == GLFW_KEY_F7) { g_vk.debug_view = (g_vk.debug_view + 1) % 6; printf("[display] debug view: %s (F7)\n", names[g_vk.debug_view]); }
        else { g_vk.debug_pick = !g_vk.debug_pick; printf("[display] pick %s (F8)\n", g_vk.debug_pick ? "on: click the picture -- the draws under the cursor go to the log" : "off"); }
    } else if (key == GLFW_KEY_F12) {
        // F12: a screenshot. Ctrl+F12: also a frame capture for replaying
        // (two frames of game memory, ~200 MB each: a 1-3 s hitch).
        g_vk.screenshot = (mods & GLFW_MOD_CONTROL) ? 2 : 1;
    }
}

// Share of all the PC's processor cores the game used since the last call.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int cpu_usage_percent(void) {
    static ULONGLONG last_cpu = 0, last_wall = 0;
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    ULONGLONG cpu = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    ULONGLONG wall = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int pct = 0;
    if (last_wall && wall > last_wall)
        pct = (int)((cpu - last_cpu) * 100 / ((wall - last_wall) * (si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1)));
    last_cpu = cpu;
    last_wall = wall;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}
#else
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static int cpu_usage_percent(void) {
    static double last_cpu = 0, last_wall = 0;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double wall = ts.tv_sec + ts.tv_nsec / 1e9;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    int pct = last_wall > 0 && wall > last_wall ? (int)((cpu - last_cpu) * 100 / ((wall - last_wall) * (n > 0 ? n : 1))) : 0;
    last_cpu = cpu;
    last_wall = wall;
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}
#endif

static int display_thread(void* arg) {
    (void)arg;
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "[display] couldn't load Vulkan (vulkan-1.dll); is the graphics driver installed?\n");
        g_vk.state = -1;
        return 0;
    }
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwInit()) {
        const char* why = NULL;
        glfwGetError(&why);
        fprintf(stderr, "[display] couldn't start the window system: %s\n", why ? why : "unknown reason");
        g_vk.state = -1;
        return 0;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    // (hidden until the first picture is ready: an empty window shows white)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    int ww = 1280, wh = 720;   // (SPLAT_WINDOW=WxH: another starting size, e.g. 2560x1080 for a wide-screen test)
    { const char* e = getenv("SPLAT_WINDOW"); if (e && (sscanf(e, "%dx%d", &ww, &wh) != 2 || ww < 320 || wh < 180)) { ww = 1280; wh = 720; } }
    GLFWwindow* window = glfwCreateWindow(ww, wh, "Splatoon Recomp", NULL, NULL);
    { const char* e = getenv("SPLAT_HIDE_FPS"); if (e && atoi(e)) g_vk.hide_fps = 1; }   // (the launcher's "FPS counter" toggle)
    if (!window) {
        const char* why = NULL;
        glfwGetError(&why);
        fprintf(stderr, "[display] couldn't open the window: %s\n", why ? why : "unknown reason");
        glfwTerminate();
        g_vk.state = -1;
        return 0;
    }
    glfwSetFramebufferSizeCallback(window, on_resize);
    glfwSetKeyCallback(window, on_key);
#ifdef _WIN32
    {
        HWND hw = glfwGetWin32Window(window);
        g_glfw_proc = (WNDPROC)SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)black_until_first_picture);
        SetClassLongPtrW(hw, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
    }
#endif
    if (!vkctx_create(window))
        fprintf(stderr, "[display] Vulkan isn't working, so the window stays empty; the game keeps running\n");
    // (shown right away, black, so starting the game shows something at once)
    glfwShowWindow(window);
#ifdef _WIN32
    // Windows enters the window's keyboard menu mode on Alt (Alt+Enter is
    // fullscreen here) and on F10 (the settings panel). The game's keys
    // pressed next -- Space, Enter, the arrows -- then walked the window's
    // own menu, whose last item is Close: the game "crashed" (the log said
    // only "window closed"). This window doesn't enter that mode.
    {
        HWND hwnd = glfwGetWin32Window(window);
        if (hwnd) g_prev_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)no_menu_proc);
    }
#endif
    if (getenv("SPLAT_SETTINGS_OPEN")) g_vk.settings_open = 1;   // (test runs: the panel open from the start)

    uint32_t last_shown = 0, last_frames = 0, last_frames_title = 0;
    double fps_start = glfwGetTime(), opened = fps_start;
    int pictured = 0;
    // Freeze report: when the game finishes no frame for 3 s (and again at
    // 15 s), write where every thread is to the log -- the next time a
    // freeze happens, the log says where the game got stuck.
    uint32_t stall_frame = 0;
    double stall_since = opened;
    int stall_reports = 0;
    for (;;) {
        glfwPollEvents();
        input_poll(window);
        // (the settings panel's window changes happen here, on the window's thread)
        if (g_vk.want_fullscreen_toggle) { g_vk.want_fullscreen_toggle = 0; toggle_fullscreen(window); }
        {   // SPLAT_FULLSCREEN=1 (the launcher's toggle): fullscreen once the window is up
            static int fs_env = -1;
            if (fs_env < 0) { const char* e = getenv("SPLAT_FULLSCREEN"); fs_env = e && atoi(e) ? 1 : 0; if (fs_env) g_vk.want_fullscreen_toggle = 1; }
        }
        if (g_vk.want_window_height && !g_fs_on) {
            int hgt = g_vk.want_window_height, wid = hgt * 16 / 9;
            g_vk.want_window_height = 0;
            glfwSetWindowSize(window, wid, hgt);
            printf("[display] window %dx%d (settings)\n", wid, hgt);
            g_vk.resized = 1;
        }
        g_vk.fullscreen = g_fs_on;
        if (glfwWindowShouldClose(window)) {
            printf("[display] window closed\n");
            runtime_exit(0);
        }
        hsleep_ns(2000000);   // 2 ms
        double now = glfwGetTime();
        {   // SPLAT_TEST_FULLSCREEN=a,b (test runs): fullscreen at a s, the window again at b s
            static double fa = -1, fb = -1;
            static int fs_step = 0;
            if (fa < 0) { const char* e = getenv("SPLAT_TEST_FULLSCREEN"); if (!e || sscanf(e, "%lf,%lf", &fa, &fb) != 2) fa = fb = 1e30; }
            if (fs_step == 0 && now >= fa) { fs_step = 1; toggle_fullscreen(window); }
            else if (fs_step == 1 && now >= fb) { fs_step = 2; toggle_fullscreen(window); }
        }
        if (!pictured && g_vk.presented > 0) {
            pictured = 1;
            printf("[display] first picture after %.1f s\n", now - opened);
        }
#ifdef _WIN32
        {   // memory use, every 10 s (for the log)
            static double mem_last = 0;
            if (now - mem_last >= 10.0) {
                mem_last = now;
                PROCESS_MEMORY_COUNTERS_EX pm;
                memset(&pm, 0, sizeof(pm));
                pm.cb = sizeof(pm);
                if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm)))
                    printf("[mem] in use %llu MB (private %llu MB, peak in use %llu MB)\n", (unsigned long long)(pm.WorkingSetSize >> 20),
                           (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20));
                static int detail = -1;   // SPLAT_MEM_DETAIL=1: how much of that is the game's own memory
                if (detail < 0) detail = getenv("SPLAT_MEM_DETAIL") != NULL;
                if (detail) {
                    extern unsigned long long wiiu_mem_resident_bytes(void);
                    unsigned long long game = wiiu_mem_resident_bytes() >> 20;
                    printf("[mem] of that: game memory (the Wii U's RAM) %llu MB, everything else (shaders, textures, driver, program) %llu MB\n",
                           game, (unsigned long long)(pm.WorkingSetSize >> 20) - game);
                }
            }
        }
#endif
        if (g_game_frames != stall_frame) {
            stall_frame = g_game_frames;
            stall_since = now;
            stall_reports = 0;
        } else if (stall_frame >= 10 && stall_reports < 2 && now - stall_since > (stall_reports ? 15.0 : (stall_frame > 300 ? 3.0 : 20.0))) {
            // (while loading -- the first 300 frames -- only after 20 s: loading
            // screens legitimately take a few seconds per frame)
            stall_reports++;
            printf("[stall] the game hasn't finished a frame for %.0f s (stuck after frame %u); where everything is:\n", now - stall_since, stall_frame);
            profiler_report_now(stdout);
            fflush(stdout);
        }
        if (now - fps_start >= 0.5) {
            uint32_t shown = g_vk.presented, frames = g_game_frames;
            double fps = (shown - last_shown) / (now - fps_start);
            double game_fps = (frames - last_frames) / (now - fps_start);
            last_shown = shown;
            last_frames = frames;
            g_vk.fps_x10 = (int)(game_fps * 10.0 + 0.5);
            g_vk.cpu_pct = cpu_usage_percent();
            fps_start = now;
            char title[256];
            // (no frame finished in this half second while the drawing thread builds new graphics: on a first
            // run each new scene's shaders are compiled by the graphics driver once -- seconds, not a crash)
            extern volatile unsigned splat_sync_builds;
            static unsigned builds_seen, builds_at_start;
            static double build_seen_at = -10;
            unsigned builds = splat_sync_builds;
            if (builds != builds_seen) build_seen_at = now;
            if (frames == last_frames_title && now - build_seen_at < 2.0) {   // (stays up for the whole stall)
                if (!builds_at_start) builds_at_start = builds_seen ? builds_seen : builds;
                snprintf(title, sizeof(title), "Splatoon Recomp | preparing graphics for this area: %u built (only the first time you visit it)", builds - builds_at_start);
            } else {
                builds_at_start = 0;
                snprintf(title, sizeof(title), "Splatoon Recomp | game frame %u | %.1f fps (game %.1f) | %u draws | hold View or M: GamePad screen, tap View: - | F1 HUD, F4 free camera, F6 benchmark, F11 fullscreen, F3 fps, F12 screenshot",
                         frames, fps, game_fps, g_vk.draws_last_frame);
            }
            builds_seen = builds;
            last_frames_title = frames;
            glfwSetWindowTitle(window, title);
        }
    }
    return 0;
}

void display_start(bool headless) {
    if (headless) {
        printf("[display] no window (--no-window)\n");
        return;
    }
    static hthrd_t thread;
    if (!hthrd_create(&thread, display_thread, NULL, 8u << 20)) {
        fprintf(stderr, "[display] couldn't start the window thread\n");
        g_vk.state = -1;
    }
}

void display_frame_done(void) {
    g_game_frames++;
}
