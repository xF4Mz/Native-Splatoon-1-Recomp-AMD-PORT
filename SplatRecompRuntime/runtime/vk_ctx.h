// The Vulkan connection shared by the window thread (which creates it and
// handles the window) and the GPU thread (which draws and presents).
#ifndef VK_CTX_H
#define VK_CTX_H

#include "volk.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkSurfaceKHR surface;            // VK_NULL_HANDLE without a window
    void* window;                    // GLFWwindow*
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    bool push_descriptors, bc_textures, d24s8, independent_blend, depth_clamp, aniso, dual_src, logic_op, mirror_clamp, wide_lines, precise_queries;
    uint32_t max_push_desc;          // maxPushDescriptors (0 without the extension)
    bool present_wait_ext;           // VK_KHR_present_wait in the driver (then a frame's arrival on the screen is waited for exactly)
    bool present_wait;               // the low-input-lag setting can work (present wait, or a 2-image swapchain whose image acquire marks the refresh)
    volatile int state;              // 0 starting, 1 ready, -1 not available
    volatile int resized;            // window size changed
    volatile int gallery;            // F1: show the texture gallery instead of the game
    volatile int screenshot;         // F12: save the next frame
    volatile uint32_t presented;     // frames shown in the window
    volatile uint32_t draws_last_frame;
    volatile int fps_x10;            // game frames per second x10, for the on-screen counter
    volatile int hide_fps;           // F3 hides the on-screen counter
    volatile int no_mips;            // F4: textures without their mip levels (for comparing)
    volatile int cpu_pct;            // how busy the PC's processor is with the game (all cores = 100)
    bool device_fault, robust2;
    bool gpl;                        // (VK_EXT_graphics_pipeline_library with fast linking: draws whose pipeline is still building use a fast-linked one -- latte_render.c gpl_*)      // (VK_EXT_device_fault on; VK_EXT_robustness2's robust image access on)
    // The GamePad screen shown over the game (View button / M key), with a
    // cursor for touching it (input.c moves it, the GPU thread draws it).
    volatile int drc_on;
    volatile int drc_cursor_x, drc_cursor_y;   // on the GamePad screen, 0..10000
    volatile int drc_touching;
    // + pressed (counts up): in story levels the game's pause menu is only on
    // the GamePad and the TV picture stops -- the GPU thread then shows the
    // GamePad screen by itself (drc_auto) and hides it once the TV moves again
    volatile int plus_presses;
    volatile int drc_auto;                 // (2: shown because the TV stopped while the GamePad picture kept changing)
    volatile int drc_no_popup;             // 1 = never show the GamePad screen by itself (settings row "GamePad pop-up")
    // The settings panel (windowed mode: the SETTINGS box top right, or F2).
    volatile int settings_open;
    volatile int fullscreen;               // (display.c keeps it current)
    volatile int want_fullscreen_toggle;   // the panel asked for it (display.c does it)
    volatile int want_window_height;       // the panel asked for a window of this height (0 = nothing asked)
    volatile int vsync_mode;               // 0 = as configured, 1 = vsync (FIFO), 2 = uncapped (mailbox / immediate)
    volatile int aa_mode;                  // 0 = off, 1 = FXAA on the finished picture
    volatile int render_scale;             // 0 = 1x (720p), 1 = 1.5x (1080p), 2 = 2x (1440p), 3 = 0.5x (360p), 4 = 0.75x (540p): TV-sized buffers drawn bigger / smaller
    volatile int fov_add;                  // degrees added to the camera's vertical field of view (0 = the game's)
    volatile int wide_mode;                // 1 = the 3D picture takes the window's shape when it's wider than 16:9
    volatile int hud_mode;                 // wide screen: 0 = the HUD keeps its 16:9 size in the middle, 1 = stretched over the whole picture
    volatile int drc_size;                 // the GamePad screen's size in the window: 0 = 64% of the height, 1 = 48%, 2 = 80%, 3 = 96%
    volatile int drc_play;                 // the GamePad screen fills the window as the picture to play by (a Battle Dojo match against the bot: player 1's view); the controls play, no touch cursor (dojo_bot.c)
    volatile int drc_cursor_speed;         // 0 = 1x, 1 = 1.5x, 2 = 2x, 3 = 3x
    volatile int low_latency;              // 1 = the game's vsync wait ends when its frame reaches the screen (one frame of input lag; needs present_wait)
    volatile int volume;                   // the sound's volume in percent (100 = as the game makes it)
    volatile int perf_mode;                // 1 = Performance mode: render scale 1x, no anti-aliasing, scenes that ask for 30 fps (the plaza) keep it
    volatile int want_dump;                // F9: write the freeze watchdog's dump now (what the game waits for)
    // Debug views (latte_render.c "Debug views"): off unless the settings row "Debug views" or SPLAT_DEBUG_VIEWS=1
    volatile int debug_views_on;           // F7 / F8 work
    volatile int debug_view;               // F7: 0 normal, 1 wireframe, 2 wireframe + shader colours, 3 shader colours, 4 overdraw, 5 skipped draws / NaN uniforms
    volatile int debug_pick;               // F8: a click prints the draws under the cursor
    volatile int pick_req, pick_x, pick_y; // a click to look at: TV picture pixel (the game's 1280x720)
    volatile int dbg_req;                  // (test runs: live input "DBGFRAME" -- the next frame's draws logged, as SPLAT_DEBUG_FRAME)
    volatile int wide_test;                // (test runs: live input "WT:n" -- wide-screen fixes off: 1 side clears, 2 full-screen panes, 4 full-screen quads)
    volatile int tv_x, tv_y, tv_w, tv_h;   // where the TV picture is in the window (the renderer sets these)
    bool wireframe;                        // the graphics card can draw lines for triangles (fillModeNonSolid)
    volatile int pad_layout;               // controller face buttons: 0 = by label (Xbox A = A, B = B, X = X, Y = Y), 1 = by position (as on a Nintendo pad: Xbox A = B, Y = X)
    volatile int tracker_on;               // F6 / settings row "Frame tracker": the frame-time graph (frame_tracker.c)
    volatile int gear_x, gear_y, gear_w, gear_h;                   // where the SETTINGS box is (window pixels; the renderer sets these)
    volatile int fps_bar_w;                                        // width of the counter bar (the box sits to its right)
    volatile int panel_x, panel_y, panel_w, panel_row_h, panel_rows;   // where the panel's rows are (panel_w: one column's width)
    volatile int panel_cols, panel_col_w;                          // (the panel's columns: how many, and from one to the next)
    volatile int panel_ids[2][48];                                 // which setting each row of each column is (-1: a group's title)
    volatile int hide_settings;                                    // F10: the SETTINGS box hidden (like F3 for the counter)
    volatile int plaza_30;                                         // settings "Plaza frame rate": 1 = the game's own 30 there (for slower PCs)
    volatile int gpu_pct;                                          // how busy the graphics card is with the game's frames (-1: can't tell)
    volatile int emote_sound;                                      // settings "Emote sound": the F5 dance's music (emote.c)
    volatile int hide_hud;                                         // F1: the TV picture without its HUD / menus (latte_render.c)
    volatile int sky_mode;                                         // SHADERS "Sky": 0 the game's own, 1 Splatoon (sunny), 2 Storm (latte_render.c)
    volatile int dojo_both;                                       // "Dojo views": 1 = both players' 3D views every frame in the 2-player Dojo (native_funcs.c splat_native_duel_draw)
    volatile int motion_blur;                                     // SHADERS "Motion blur": 0 off, 1 light, 2 strong (latte_render.c motion_blur_pass)
    volatile int sh_clouds, sh_grade, sh_quality;                  // SHADERS rows: 3D clouds on/off, colour grade on/off, quality 0 low / 1 medium / 2 high
    volatile int sh_rays, sh_cshadow, sh_ao, sh_rain;              // SHADERS rows: god rays & haze, cloud shadows on the ground, ambient occlusion, rain with Storm (on/off)
    volatile int warm_shaders;                                     // settings "Shader warm-up": build every known pipeline before the game's first picture (latte_render.c)
    volatile int build_wait;
    volatile int frame_gen;                                        // settings "Smooth motion": 0 off, 2 / 3 / 4 = generated pictures between the game's frames (latte_render.c g_fg); SPLAT_FRAMEGEN                                       // settings "New shaders": 1 = a frame waits up to 30 ms for builds (no flicker), 0 = draws are left out (no hitch)
    volatile int online;                                           // online mode: not part of this release (net_offline.c answers offline)
    volatile int pro_pad;                                          // settings "Player 2": 0 off, 1 a second controller is a Wii U Pro Controller (Battle Dojo), 2 always one there (tests); input.c, SPLAT_PRO_PAD
    volatile int shadow_quality;                                   // settings "Shadows": 0 off, 1 normal (the game's 2048 map), 2 sharp (drawn at 4096)
    volatile int mouse_aim;                                        // settings "Mouse aim": 0 off, 1 / 2 / 3 / 4 = sensitivity 1x / 1.5x / 2x / 0.5x -- the mouse turns the GamePad's gyro (input.c); SPLAT_MOUSE_AIM
    volatile int mouse_captured;                                   // (the mouse is held by the game for aiming: a click on the picture takes it, Esc gives it back)
    volatile int gyro_on;                                          // settings "Motion controller": a PlayStation pad's real gyro aims (gyro_hid.c); SPLAT_GYRO
} VkCtx;

extern VkCtx g_vk;

// Where the GamePad screen goes in a W x H window: centred, about 2/3 of the
// height, the picture's shape (input.c maps the mouse with the same box).
static inline void drc_overlay_rect(int W, int H, int* x, int* y, int* w, int* h) {
    int pct = g_vk.drc_play ? 100 : g_vk.drc_size == 1 ? 48 : g_vk.drc_size == 2 ? 80 : g_vk.drc_size == 3 ? 96 : 64;
    int hh = H * pct / 100, ww = hh * 854 / 480;
    int maxw = g_vk.drc_play ? W : W * 94 / 100;
    if (ww > maxw) { ww = maxw; hh = ww * 480 / 854; }
    *x = (W - ww) / 2; *y = (H - hh) / 2; *w = ww; *h = hh;
}

// Create the instance, device and (with a window) surface. Sets g_vk.state.
bool vkctx_create(void* glfw_window);
void vkctx_settings_save(void);   // (the settings panel's choices -> splat_settings.ini; read back at the next start)
void vkctx_perf_set(int on);       // Performance mode on / off (each keeps its own render scale)

uint32_t vkctx_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want);

#endif
