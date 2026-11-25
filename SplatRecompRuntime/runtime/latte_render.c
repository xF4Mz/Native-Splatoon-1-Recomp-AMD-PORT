// The renderer (see latte_render.h): GPU work -> Vulkan, on the GPU thread.
//
// - Shaders: the game's GPU programs are translated to GLSL (latte_shader.c),
//   compiled to SPIR-V once, and cached by a hash of the program and the
//   state the translation depends on.
// - Vertex data, uniform blocks and uniform registers are copied per draw
//   into one storage buffer ("arena") that the shaders read directly.
// - Color/depth buffers live only on the PC's graphics card ("surfaces",
//   found by their address in game memory); textures are read from game
//   memory, untiled and uploaded, unless the game rendered them itself.
// - All images stay in the GENERAL layout, with a full barrier between
//   passes. Simple and correct; fast enough for now.
// - GX2CopyColorBufferToScanBuffer copies into a "TV image" that the swap
//   presents to the window, scaled to fit.
#include "latte_render.h"
#include "latte_cp.h"
#include "latte_shader.h"
#include "latte_texture.h"
#include "glsl_compile.h"
#include "texture_gallery.h"
#include "vk_ctx.h"
#include "wiiu_mem.h"
#include "host_thread.h"
static bool debug_auto(void);
#include "latte_capture.h"
#include "gx2_layout.h"
#include "gx2_cmd.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#include <zlib.h>
#include <math.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <dwmapi.h>
#else
#include <sys/stat.h>
#define _fseeki64 fseeko
#define _ftelli64 ftello
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSUB 8   // (recordings in flight: work goes to the card at every game flush now, ~5 a frame)
#define CHUNK_SIZE (32u << 20)
#define MAX_TEX_DATA (64u << 20)
#define SO_RING_SIZE (8u << 20)

static inline u32 R(u32 r) { return g_latte_regs[r]; }
static float RF(u32 r) { float f; memcpy(&f, &g_latte_regs[r], 4); return f; }

// Stream-out results not yet copied into game memory (see the end of
// latte_render_draw): copied at the next point the game can look at them.
typedef struct { u32 addr, ring, size; } SoPend;
// SPLAT_SO_HASH=1 (tests): the stream-out results handed back to the game, summed up -- the first 40
// one by one, then every 600th copy: comparing two builds' ink checks
static void so_hash_note(const u8* p, u32 n) {
    static int on = -1;
    static u32 count, nonzero_copies;
    static u64 h_all = 1469598103934665603ull;
    if (on < 0) on = getenv("SPLAT_SO_HASH") != NULL;
    if (!on) return;
    u64 h = 1469598103934665603ull;
    u32 nz = 0;
    for (u32 i = 0; i < n; i++) { h = (h ^ p[i]) * 1099511628211ull; nz += p[i] != 0; }
    h_all = (h_all ^ h) * 1099511628211ull;
    count++;
    if (nz) nonzero_copies++;
    float f[4] = { 0, 0, 0, 0 };
    for (u32 k = 0; k < 4 && k * 4 + 4 <= n; k++) { u32 w = ((u32)p[k * 4] << 24) | ((u32)p[k * 4 + 1] << 16) | ((u32)p[k * 4 + 2] << 8) | p[k * 4 + 3]; memcpy(&f[k], &w, 4); }
    if (count <= 40) printf("[render] stream-out result %u: %u bytes, %u non-zero, hash %016llx, first floats %g %g %g %g\n", count, n, nz, (unsigned long long)h, f[0], f[1], f[2], f[3]);
    if (count % 600 == 0) printf("[render] stream-out results: %u copies (%u with data), all so far hash %016llx\n", count, nonzero_copies, (unsigned long long)h_all);
}
#define SO_PEND_MAX 256
static SoPend g_so_pend[SO_PEND_MAX];
static u32 g_so_npend;
static u32 g_so_last_draw;     // the frame's draw number of the last stream-out draw
static u32 g_cb_first_draw;    // draws counted when the current recording began
static void so_sync(void);
static bool so_pending_in(u32 addr, u32 size) {
    for (u32 i = 0; i < g_so_npend; i++)
        if (addr < g_so_pend[i].addr + g_so_pend[i].size && g_so_pend[i].addr < addr + size) return true;
    return false;
}

// Work done once the graphics card has finished a submission, in order:
// copies of stream-out results into game memory, then the fences and memory
// writes the game waits on that came after them. (At a fence the drawing
// thread used to submit everything and wait for the graphics card -- about
// 4 ms every frame -- although the game rarely waits on those fences.)
// SPLAT_FENCE_SYNC=1: wait at each fence, as before.
typedef struct { u64 serial; u32 kind, a, b, c, d; } Deferred;   // kind 0: copy (addr, ring offset, size); 1: fence (ts high, low); 2: write (addr, low, high, 64-bit)
#define DEFER_MAX 1024
static Deferred g_def[DEFER_MAX];
static u32 g_def_head, g_def_n;
static void (*g_retire_fn)(u64 ts);
static void defer_poll(bool wait);
static bool defer_pending_in(u32 addr, u32 size) {
    for (u32 i = 0; i < g_def_n; i++) {
        const Deferred* e = &g_def[(g_def_head + i) % DEFER_MAX];
        if (e->kind == 0 && addr < e->a + e->c && e->a < addr + size) return true;
    }
    return false;
}

// Readback counters for the [sync] log line (gx2.c prints it every second).
static LatteSyncStats g_ss;
static LatteSyncStats g_ss_last;   // (the last second's, for the freeze dump)
static struct Surf* g_fz_src;      // the buffer last copied into the TV picture
void latte_render_sync_stats(LatteSyncStats* out) {
    *out = g_ss;
    g_ss_last = g_ss;
    memset(&g_ss, 0, sizeof(g_ss));
}

// Game memory the renderer reads (also saved when a frame is being captured).
static inline const u8* gmem(u32 addr, u32 size) {
    if (g_so_npend && so_pending_in(addr, size)) { g_ss.so_draw_reads++; so_sync(); }   // (a draw reading what an earlier one streamed out)
    if (g_def_n && defer_pending_in(addr, size)) defer_poll(true);
    const u8* p = wiiu_mem_ptr(addr, size);
    if (p && g_cap_active) cap_mem(addr, size);
    return p;
}

// Debug pictures, screenshots and frame captures go in their own folder so
// the main folder stays tidy.
#define DEBUG_DIR "debug_output"
static const char* debug_dir(void) {
    static bool made = false;
    if (!made) {
#ifdef _WIN32
        _mkdir(DEBUG_DIR);
#else
        mkdir(DEBUG_DIR, 0755);
#endif
        made = true;
    }
    return DEBUG_DIR;
}
// Screenshots (F12, SPLAT_SHOT_EVERY) and buffer pictures go to images\.
#define IMAGES_DIR "images"
static const char* images_dir(void) {
    static bool made = false;
    if (!made) {
#ifdef _WIN32
        _mkdir(IMAGES_DIR);
#else
        mkdir(IMAGES_DIR, 0755);
#endif
        made = true;
    }
    return IMAGES_DIR;
}

bool g_render_headless = false;   // set by main: no window, but render (for tests / frame dumps)
u32 g_render_dump_frames = 0;     // save the first N frames as PNG files

// --- State --------------------------------------------------------------------------------

typedef struct Chunk {
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* ptr;
    u32 size, used;
    u32 uid;   // (new for every stretch of use: see upload_guest)
    struct Chunk* next;
} Chunk;

typedef struct DPool { VkDescriptorPool pool; struct DPool* next; } DPool;

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    bool busy;
    Chunk* chunks;
    DPool* pools;           // descriptor pools (when the driver lacks push descriptors)
    DPool* pool_cur;
    VkSemaphore acquire;
    VkBuffer rb;            // frame read-back (screenshots / dumps)
    VkDeviceMemory rb_mem;
    void* rb_ptr;
    u32 rb_size, rb_w, rb_h;
    bool rb_pending;
    u64 serial;             // which recording this is (occlusion query results)
    VkBuffer th_buf;        // the picture's thumbnail (the flicker catcher)
    VkBuffer pw_buf;        // a tiny thumbnail for the pause watcher
    VkDeviceMemory pw_mem;
    void* pw_ptr;
    bool pw_pending;
    VkBuffer pw2_buf;       // the same for the GamePad's picture (only while the TV holds still)
    VkDeviceMemory pw2_mem;
    void* pw2_ptr;
    bool pw2_pending;
    bool ts_on;             // GPU timestamps written at its start and end (see g_ts_pool)
    u8 sec_on;              // card-time sections recorded in it (bit per section, see g_sec_pool)
    VkDeviceMemory th_mem;
    void* th_ptr;
    bool th_pending;
    u64 th_seq;
} Sub;
static u64 g_sub_serial;

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkFormat format;
    VkImageAspectFlags aspect;
    u32 w, h, layers;
    u32 depth3d;               // 3D images: the depth (layers is then 1)
    u32 levels;                // mip levels (textures from game memory)
    bool cube;
    VkImageView att_view;
    VkImageView* att_layers;   // one attachment view per slice (layered buffers)
    struct { VkImageView view; u64 key; } views[24];
    int nviews;
} Img;

typedef struct Surf {
    u32 addr, gx2fmt, w, h, layers;
    bool vol;               // a 3D (volume) buffer, drawn slice by slice
    bool depth, stencil;
    bool rendered;          // the GPU wrote it (render target / copy target)
    u64 seq;                // when it was last used as a target (newest wins)
    Img img;
    // texture cache
    u64 key, hash;
    struct Surf* anext;           // next buffer whose address lands in the same g_rth bucket
    u64 block_rt_seq;       // compressed texture the GPU wrote as a buffer of blocks: copy done for this draw seq
    u64 checked_frame, gen_frame;
    u32 gen;                // game-memory change counter when last uploaded
    bool dirty;
    u32 span;
    u32 mip_addr, mip_span; // mip levels 1.. in game memory
    Img fb_copy;            // copy for draws that read the depth buffer they test against
    bool fb_ok;
    u64 wseq, fb_seq, fb_frame;   // depth writes so far / when the copy was taken (reused while unchanged)
    // written back to game memory after drawing (small buffers the CPU reads)
    u32 tile, swizzle, dim;
    bool rb_candidate, rb_dirty;
    u64 seen;               // frame it was last drawn to, + 1
    bool recurring;         // drawn every frame (so skipping a draw for a frame is harmless)
    bool guessed;           // (never_stop: a screen buffer taken as drawn every frame from its very first frame -- checked a frame later)
    u32 guess_skips;        // (draws left out of it in that first frame)
    u64 dv_clear;           // (debug view 1: the frame it was cleared for, + 1)
    bool scan_src;          // shown on the TV (copied to the scan buffer)
    u64 band_frame;         // (wide screen: the frame its sides outside 16:9 were seen to, + 1 -- side_bands_clear)
    // read as a texture with mip levels the game drew into separate buffers
    // (the light probes: level 0 = sunlit, level 1 = in shadow)
    Img mipc;
    bool mipc_ok;
    u64 mipc_seq;
    Img gsc;                // (render scale: a copy at the game's size, for the bloom chain's first pass -- rt_game_size)
    bool gsc_ok;
    u64 gsc_seq;
    u64 wdraw;              // the draw (g_draw_id) that last set it up as a buffer to draw into
    u8 mip_found;           // (levels found last time: logged when it changes)
    float scale;            // render scale: the image is w*scale x h*scale (1 = the game's size)
    float scalex;           // (wide screen: the image is w*scalex wide; = scale otherwise)
    u8 dirty_streak;        // how many times in a row a re-hash of this texture found new data (a streaming texture: skip the hash)
    struct Surf* next;
} Surf;

typedef struct ShaderEnt {
    u64 key;
    VkShaderModule mod;
    LatteShaderOut info;
    bool failed;
    volatile uint64_t pending;   // being compiled on a helper thread
    bool fragment;
    bool unchecked;              // from the disk cache of an older translator, not yet compared (preloaded)
    u8 disk_rev;                 // (then: the translator revisions its record matches, DISK_REV_*)
    u8 skip_frames;              // frames its draws were skipped in while it was being built
    u64 skip_frame;              // (the last of those, + 1)
    u8 ub_fixed[16];             // per uniform block: 1 + the highest index its SPIR-V reads without a clamp (0 none: shader_note_ub)
    char* glsl;                  // (for the helper thread)
    u32* disk_in;                // (or: SPIR-V from the disk cache, for the helper thread to make the module of)
    size_t disk_words;
    u32* spv;                    // compiled, not yet saved to the disk cache
    size_t words;
    struct ShaderEnt* next;
} ShaderEnt;
static void shader_note_ub(ShaderEnt* e, const u32* spv, size_t words);

typedef struct {
    VkShaderModule vs, ps;
    u32 color_fmt[8], color_mask[8], blend[8], blend_on;
    u32 depth_fmt, stencil_fmt;
    u32 topo_class, depth_clamp, logic_op, poly_mode, dual_src, raster_off;
} PipeKey;

typedef struct PipeEnt {
    PipeKey key;
    u64 hash;
    VkPipeline pipe;
    VkPipeline interim;          // (a fast-linked stand-in while pipe is being built: gl_fast)
    volatile uint64_t pending;   // being built on a helper thread
    u8 skip_frames;              // frames its draws were skipped in while it was being built
    u64 skip_frame;              // (the last of those, + 1)
    struct PipeEnt* next;
} PipeEnt;

// A pipeline the look-ahead (latte_cp_lookahead) expects a coming draw to
// need: a helper thread compiles its two shaders if nobody has yet, then
// builds it. The draw takes it in get_pipeline; one no draw took joins the
// others at the end of the frame (pf_sweep).
typedef struct PrefEnt {
    ShaderEnt *vs, *ps;
    PipeKey k;                   // (vs / ps empty: the shaders' modules may not exist yet)
    u64 hash;                    // of k
    VkPipeline pipe;
    VkPipeline interim;          // (a fast-linked stand-in meanwhile: gl_fast)
    volatile uint64_t pending;   // as a PipeEnt's: 1 waiting, 2 a helper builds it, 3 the drawing thread does, 0 done
    bool taken;                  // a draw took its pipeline
    bool warm;                   // made by the shader warm-up (counted for its progress), not by the look-ahead
    u8 skip_frames;
    u64 skip_frame;
    struct PrefEnt *next, *all;
} PrefEnt;
#define PF_BUCKETS 1024
static PrefEnt* g_pf_tab[PF_BUCKETS];
static PrefEnt* g_pf_all;
static u32 g_pf_n;                                 // entries now
static u64 g_pf_made, g_pf_used, g_pf_kept;        // (for the log)
static bool g_la_want;                             // a build on the drawing thread took long: look ahead at the next draw
static u32 g_once_relaxed;                         // draws into screen-sized "made once" buffers that may skip for a build (see must_have)
#define LA_SLOW_NS 20000000ull                     // (what "long" is)
static bool async_on(void);
static void async_queue(ShaderEnt* se, PipeEnt* pe);
static void pipe_time_note(const PipeKey* k, u64 ns);
static void modreg_put(VkShaderModule m, u64 key, size_t words);
static void build_now(ShaderEnt* se, PipeEnt* pe);

#define LR_BUCKETS 65536   // (shader and pipeline tables)
static struct {
    bool tried, ok;
    VkCommandPool pool;
    Sub sub[NSUB];
    int cur;              // current submission, -1 = none open
    Chunk* free_chunks;
    Chunk* chunk;         // chunk being filled
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkSampler default_sampler;
    Img dummy[4];         // 2D, 2D array, 3D, cube
    Img dummy_1d;
    // render pass
    bool in_pass;
    VkImageView pass_color[8];
    VkFormat pass_cfmt[8];
    u32 pass_ncolor;
    VkImageView pass_depth;
    VkFormat pass_dfmt;
    bool pass_stencil;
    u32 pass_w, pass_h;
    bool need_barrier;
    // surfaces
    Surf* rts;
    Surf* tex[4096];
    Surf** tex_list;      // all textures, for invalidation
    u32 tex_count, tex_cap;
    struct { u32 addr, w, h, fmt, layers, dim, tile, swizzle; bool depth, used; } rti[128];   // buffer sizes/formats GX2 told us about
    u32 rti_next;
    // shaders / pipelines
    ShaderEnt* shaders[LR_BUCKETS];   // (26000+ shaders, and as many pipelines: 1024 buckets made chains of 26)
    PipeEnt* pipes[LR_BUCKETS];
    // frame
    u64 frame;
    u32 draws;
    // TV image
    Img tv;
    bool tv_valid;
    // GamePad screen (shown over the TV picture with the View button)
    Img drc;
    bool drc_valid;
    // swapchain
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    u32 sc_count;
    VkImage sc_images[8];
    VkSemaphore sc_done[8];
    bool sc_rebuild;
    // the graphics driver's compiled pipelines, kept on disk (pipeline_cache.bin)
    VkPipelineCache pcache;
    u32 new_pipes;
    // stream out
    VkBuffer so_buf;
    VkDeviceMemory so_mem;
    void* so_ptr;
    u32 so_off;
    // texture decode buffer
    u8* tex_buf;
    // gallery
    u8* gallery_px;
} g;

static void barrier_all(VkCommandBuffer cb);
static void wait_mark(const char* what, volatile uint64_t* p);   // (the wait-state markers: what a thread waits for, see latte_render_report_waits)
static void wait_unmark(void);
static Img* present_source(void);   // (the anti-aliased picture when it was made this frame, else the TV picture)
static bool upload_level(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 level);
static bool upload_texture(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 dim);
static void flush_wait(void);
static void gpu_lost_check(VkResult r);
static u32 texel_bytes(VkFormat f);
static void query_suspend(VkCommandBuffer cb);
static void query_resume(VkCommandBuffer cb);

// --- Memory -------------------------------------------------------------------------------------

typedef struct Block { VkDeviceMemory mem; VkDeviceSize size, used; u32 type; struct Block* next; } Block;
static Block* g_blocks;

static bool alloc_image_mem(VkMemoryRequirements req, VkDeviceMemory* mem, VkDeviceSize* off) {
    u32 type = vkctx_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = vkctx_memory_type(req.memoryTypeBits, 0);
    if (type == UINT32_MAX) return false;
    if (req.size > (64ull << 20)) {   // big: its own allocation
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(g_vk.device, &ai, NULL, mem) != VK_SUCCESS) return false;
        *off = 0;
        return true;
    }
    for (Block* b = g_blocks; b; b = b->next) {
        if (b->type != type) continue;
        VkDeviceSize o = (b->used + req.alignment - 1) / req.alignment * req.alignment;
        if (o + req.size <= b->size) {
            b->used = o + req.size;
            *mem = b->mem;
            *off = o;
            return true;
        }
    }
    Block* b = (Block*)calloc(1, sizeof(Block));
    b->size = 128ull << 20;
    if (req.size > b->size) b->size = req.size;
    b->type = type;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = b->size;
    ai.memoryTypeIndex = type;
    VkResult ar = vkAllocateMemory(g_vk.device, &ai, NULL, &b->mem);
    if (ar != VK_SUCCESS) {
        free(b);
        // fall back to a dedicated allocation of just this size
        VkMemoryAllocateInfo di = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        di.allocationSize = req.size;
        di.memoryTypeIndex = type;
        ar = vkAllocateMemory(g_vk.device, &di, NULL, mem);
        if (ar != VK_SUCCESS) { fprintf(stderr, "[render] vkAllocateMemory failed (%d)\n", (int)ar); return false; }
        *off = 0;
        return true;
    }
    b->next = g_blocks;
    g_blocks = b;
    b->used = req.size;
    *mem = b->mem;
    *off = 0;
    return true;
}

static bool host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** ptr) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(g_vk.device, &bi, NULL, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, *buf, &req);
    u32 type = vkctx_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g_vk.device, &ai, NULL, mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_vk.device, *buf, *mem, 0);
    return vkMapMemory(g_vk.device, *mem, 0, size, 0, ptr) == VK_SUCCESS;
}
// The same for buffers the CPU *reads* (results copied back from the card:
// ink maps, stream-out, screenshots): memory the CPU caches. The first
// host-visible type is write-combined on this driver -- fine to fill, but
// reading it a texel at a time (the ink map's re-tiling) ran at ~75 ns per
// read: 15 ms a frame while shooting. SPLAT_NO_CACHED_READBACK=1: as before.
static bool host_buffer_cached(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** ptr) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_NO_CACHED_READBACK") == NULL;
    if (!on) return host_buffer(size, usage, buf, mem, ptr);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(g_vk.device, &bi, NULL, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_vk.device, *buf, &req);
    u32 type = vkctx_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    static int logged = 0;
    if (type == UINT32_MAX) {
        if (logged++ < 1) printf("[render] no cached host memory type: readbacks stay in write-combined memory\n");
        vkDestroyBuffer(g_vk.device, *buf, NULL);
        return host_buffer(size, usage, buf, mem, ptr);
    }
    if (logged++ < 1) printf("[render] readback buffers in cached host memory (type %u)\n", type);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(g_vk.device, &ai, NULL, mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_vk.device, *buf, *mem, 0);
    return vkMapMemory(g_vk.device, *mem, 0, size, 0, ptr) == VK_SUCCESS;
}

// --- Submissions and the upload arena -----------------------------------------------------------

static Chunk* new_chunk(void) {
    Chunk* c = g.free_chunks;
    if (c) { g.free_chunks = c->next; c->used = 0; c->next = NULL; return c; }
    c = (Chunk*)calloc(1, sizeof(Chunk));
    c->size = CHUNK_SIZE;
    if (!host_buffer(CHUNK_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &c->buf, &c->mem, (void**)&c->ptr)) {
        fprintf(stderr, "[render] out of upload memory\n");
        free(c);
        return NULL;
    }
    return c;
}

// time spent per stage, printed every 300 frames as [perf]
enum { PF_SHADERS, PF_TEXTURES, PF_UPLOAD, PF_RECORD, PF_WAIT, PF_SUBMIT, PF_SO, PF_SO_COUNT, PF_IDLE, PF_COUNT };
static u64 g_pf[PF_COUNT];
// The command processor's non-draw packets, in CPU ticks (latte_cp.c): converted with a rate measured between reports.
extern u64 g_cp_tsc[2];
extern u64 g_cp_count[3];
static double g_tsc_per_ns = 0;
static void tsc_rate_update(void) {
    static u64 t0_tsc, t0_ns;
    u64 t = __rdtsc(), n = hnow_ns();
    if (t0_ns && n > t0_ns + 100000000ull) g_tsc_per_ns = (double)(t - t0_tsc) / (double)(n - t0_ns);
    if (!t0_ns || n > t0_ns + 100000000ull) { t0_tsc = t; t0_ns = n; }
}
static double cp_ms(u64 tsc) { return g_tsc_per_ns > 0 ? (double)tsc / g_tsc_per_ns / 1e6 : 0.0; }
static u64 g_pf_draw_start, g_pf_frames;
static u32 g_skipped_draws;   // waiting for a shader/pipeline being built
static u32 g_drc_draws_skipped;   // (Performance mode: GamePad-picture draws left out while it isn't shown)
static u32 g_center_still;        // TV: samples (8 frames apart) the middle of the picture held still
// This frame left a draw out (its shader/pipeline is still being built on a
// helper thread): it isn't shown, so nothing blinks out -- the previous
// complete picture stays up instead. At most HOLD_MAX frames in a row.
static bool g_frame_skipped;
static u32 g_held_in_row, g_held_frames, g_held_few;   // (g_held_few: of them, frames missing only a few draws -- New shaders: wait)
static u32 g_frame_skip_n;      // (draws left out this frame)
static bool g_frame_must_hold;  // (one of them was a full-screen pass that may only wait while the frame isn't shown)
static bool g_hold_spent;   // (held HOLD_MAX frames in a row: show the next ones as they are until a frame is complete again --
                            // holding again at once showed 1 frame in 9, a slide show while a new area's graphics were built)
static u32 g_tex_uploads;          // texture levels uploaded (for the [spike] lines)
static u64 g_tex_ns[5];            // (of the texture time: hashing, image creation, decoding, all of upload_level, all of upload_texture)
static u64 g_tex_ns2[3];           // (and: image views made, samplers made, feedback copies + mip sets of drawn buffers)
static u64 g_lap_split[2];
static u64 g_att_split[3];          // (of "buffers to draw into": colour buffers, depth buffer, the rest)         // (the "textures" stretch of a draw also holds: setting up its buffers to draw into, its shaders' readiness)
static u32 g_tex_views_made;
static u64 g_tex_upload_bytes;
#define HOLD_MAX 8
// (the per-draw timing reads the processor's cycle counter, scaled to ns once
// measured against the system clock: the system clock call, ~6 times a draw,
// was 2% of the drawing thread)
#ifdef _WIN32
#include <intrin.h>
static double g_ns_per_tick;
static u64 g_cal_tsc, g_cal_ns;
static inline u64 pf_now(void) {
    u64 t = __rdtsc();
    if (g_ns_per_tick > 0) return g_cal_ns + (u64)((double)(t - g_cal_tsc) * g_ns_per_tick);
    u64 ns = hnow_ns();
    if (!g_cal_ns) { g_cal_ns = ns; g_cal_tsc = t; }
    else if (ns - g_cal_ns > 500000000ull && t > g_cal_tsc) g_ns_per_tick = (double)(ns - g_cal_ns) / (double)(t - g_cal_tsc);
    return ns;
}
#else
#define pf_now hnow_ns
#endif
#define PF_BEGIN() u64 pf_t_ = pf_now()
#define PF_LAP(slot) do { u64 n_ = pf_now(); g_pf[slot] += n_ > pf_t_ ? n_ - pf_t_ : 0; pf_t_ = n_; } while (0)
// (the [spike] line's threshold: a drawing-thread frame over this many ms gets its breakdown logged; SPLAT_SPIKE_MS, default 50)
static u64 spike_ns(void) {
    static u64 v;
    if (!v) { const char* e = getenv("SPLAT_SPIKE_MS"); v = (e && atoi(e) > 0 ? (u64)atoi(e) : 50) * 1000000ull; }
    return v;
}
static u32 spike_cap(void) { return spike_ns() < 50000000ull ? 4000 : 400; }   // (a lowered threshold logs more: boot alone has hundreds)
// How long a swap waits for an acquired swapchain image's release fence before leaving the frame unshown
// (acq_ready). 2 ms since the Oct 25 boot-stall fix; the Oct 31 bench dropped 16-37 presents per 10 s on
// Port Mackerel / Kelp Dome at 2 ms -- the compositor hands images back a few ms after their replacement
// is shown (6 ms: 0 dropped in the same sweeps). SPLAT_ACQ_FENCE_MS overrides (2 = as before).
static u64 acq_fence_ns(void) {
    static u64 v;
    if (!v) { const char* e = getenv("SPLAT_ACQ_FENCE_MS"); v = (e && atoi(e) > 0 ? (u64)atoi(e) : 6) * 1000000ull; }
    return v;
}

static void thumb_process(Sub* s);
static void pause_watch_process(Sub* s);
static void drc_watch_process(Sub* s);
static u32 g_drc_scan_addr;   // (the GamePad pop-up: the GamePad picture's scan-buffer copy source)
// What the command buffer's dynamic state and bindings were last set to: a
// draw only sets what differs from the draw before (about 30 driver calls per
// draw otherwise, ~half the drawing thread's time in a busy view). Reset when
// a command buffer starts; anything else that sets state in the buffer
// (the FXAA pass) invalidates it.
typedef struct {
    bool valid;
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkBool32 restart, bias_en, ztest, zwrite, stencil;
    float bias[3];
    VkCompareOp zop;
    u32 sop_f, sop_b, sref_f, sref_b, scmp_f, scmp_b, swm_f, swm_b;
    float bc[4];
    VkBuffer ibuf;
    VkDeviceSize ioff;
} DynState;
static DynState g_dyn;
// (a stream-out-only pipeline -- rasterizer off, no colour, no depth -- ignores every viewport / depth / stencil /
// blend state, and binding it leaves the card's copies of them undefined: nothing is set while it is bound (the
// layer's VUID 08608) and the draw after it sets everything again (VUIDs 07844-07847))
static bool g_dyn_skip;
static void buffer_destroy_later(VkBuffer buf, VkDeviceMemory mem);   // (see dead_collect)
#define DYN_SET(field, value, call) do { if (!g_dyn_skip && (!g_dyn.valid || memcmp(&g_dyn.field, &(value), sizeof(g_dyn.field)) != 0)) { call; memcpy(&g_dyn.field, &(value), sizeof(g_dyn.field)); } } while (0)
// How long the graphics card itself works per frame: a timestamp at the
// start and end of every recording, read back when its slot is reused.
// (The game waits for the card when it reads results back -- ink under the
// players -- so a slow card shows up as game-thread waits.)
static VkQueryPool g_ts_pool;
static int g_ts_ok = -1;
static u64 g_gpu_ns;           // card time since the last [perf] line
static u64 g_gpu_ns_all;       // (all of it: the frame tracker takes differences)
// Card time of a few of our own passes (the SHADERS pass: its march and its composite), per recording:
// two timestamps per section, read back with the frame's own pair. [perf] "card time in our passes".
enum { SEC_VOL_MARCH, SEC_VOL_COMP, SEC_N };
static VkQueryPool g_sec_pool;
static u64 g_sec_ns[SEC_N], g_sec_runs[SEC_N];
static void sec_mark(VkCommandBuffer cb, int sec, int end) {
    if (g_ts_ok <= 0 || g.cur < 0 || !g.sub[g.cur].ts_on || !g_sec_pool) return;
    Sub* s = &g.sub[g.cur];
    u32 q = ((u32)g.cur * SEC_N + (u32)sec) * 2;
    if (!end) {
        if (s->sec_on & (1u << sec)) return;   // (once per recording: the first run counts)
        vkCmdResetQueryPool(cb, g_sec_pool, q, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_sec_pool, q);
        s->sec_on |= (u8)(1u << sec);
        s->sec_on |= (u8)(0x10u << sec);   // (an end is due)
    } else if (s->sec_on & (0x10u << sec)) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_sec_pool, q + 1);
        s->sec_on &= (u8)~(0x10u << sec);
    }
}
static void ts_end(VkCommandBuffer cb) {
    if (g_ts_ok > 0 && g.cur >= 0 && g.sub[g.cur].ts_on)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, (u32)g.cur * 2 + 1);
}
static VkCommandBuffer cmd(void) {
    if (g.cur >= 0) return g.sub[g.cur].cb;
    static int next = 0;
    Sub* s = &g.sub[next];
    bool was_busy = s->busy;
    if (s->busy) {
        u64 t0 = hnow_ns();
        wait_mark("the card's fence of an older command buffer (cmd)", NULL);
        vkWaitForFences(g_vk.device, 1, &s->fence, VK_TRUE, UINT64_MAX);
        wait_unmark();
        g_pf[PF_WAIT] += hnow_ns() - t0;
        s->busy = false;
    }
    if (s->th_pending) {   // (its picture's thumbnail is ready now)
        if (was_busy) thumb_process(s);
        s->th_pending = false;
    }
    if (s->pw_pending) {
        if (was_busy) pause_watch_process(s);
        s->pw_pending = false;
    }
    if (s->pw2_pending) {
        if (was_busy) drc_watch_process(s);
        s->pw2_pending = false;
    }
    if (s->ts_on && was_busy) {
        u64 t[2];
        if (vkGetQueryPoolResults(g_vk.device, g_ts_pool, (u32)next * 2, 2, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
        {
            u64 d = (u64)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            g_gpu_ns += d;
            g_gpu_ns_all += d;
        }
    }
    if (s->sec_on && was_busy) {
        for (int k = 0; k < SEC_N; k++) {
            if (!(s->sec_on & (1u << k)) || (s->sec_on & (0x10u << k))) continue;   // (not recorded, or no end written)
            u64 t[2];
            if (vkGetQueryPoolResults(g_vk.device, g_sec_pool, ((u32)next * SEC_N + (u32)k) * 2, 2, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                g_sec_ns[k] += (u64)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
                g_sec_runs[k]++;
            }
        }
    }
    s->sec_on = 0;
    s->ts_on = false;
    // recycle its upload chunks
    while (s->chunks) {
        Chunk* c = s->chunks;
        s->chunks = c->next;
        c->next = g.free_chunks;
        g.free_chunks = c;
    }
    for (DPool* p = s->pools; p; p = p->next) vkResetDescriptorPool(g_vk.device, p->pool, 0);
    s->pool_cur = s->pools;
    vkResetFences(g_vk.device, 1, &s->fence);
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->cb, &bi);
    g_dyn.valid = false;
    if (g_ts_ok < 0) {
        g_ts_ok = 0;
        if (g_vk.props.limits.timestampComputeAndGraphics && !getenv("SPLAT_NO_GPU_TIME")) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = NSUB * 2;
            g_ts_ok = vkCreateQueryPool(g_vk.device, &qi, NULL, &g_ts_pool) == VK_SUCCESS;
            qi.queryCount = NSUB * SEC_N * 2;
            if (g_ts_ok && vkCreateQueryPool(g_vk.device, &qi, NULL, &g_sec_pool) != VK_SUCCESS) g_sec_pool = VK_NULL_HANDLE;
        }
    }
    if (g_ts_ok > 0) {
        vkCmdResetQueryPool(s->cb, g_ts_pool, (u32)next * 2, 2);
        vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, (u32)next * 2);
        s->ts_on = true;
    }
    s->serial = ++g_sub_serial;
    g.cur = next;
    static int nsub = -1;   // SPLAT_NSUB=N: recordings in flight (tests)
    if (nsub < 0) { const char* e = getenv("SPLAT_NSUB"); nsub = e ? atoi(e) : 4; if (nsub < 2 || nsub > NSUB) nsub = 4; }   // (4: each slot holds a 32 MB upload chunk; 8 cost ~160 MB more)
    next = (next + 1) % nsub;
    g.chunk = NULL;
    g_cb_first_draw = g.draws;
    query_resume(s->cb);
    return s->cb;
}

// Uniform blocks and vertex data already copied into this upload chunk
// earlier on: the same guest memory (address and size), not marked as
// changed since (wiiu_mem_mark: the game's cache flushes, file loads, stream
// out), is used from there instead of being copied again. Many draws read
// the same blocks (camera, lights, the same model drawn again); copying
// every one for every draw was ~6 ms a frame with ~1,800 draws in the
// plaza. Only within one chunk: each draw's data is addressed from its
// chunk's start. SPLAT_NO_UPLOAD_REUSE=1: copy everything, as before.
typedef struct { u32 addr, size, uid, gen, off, batch; u32 st0, st1; u64 head; bool pending, ring; } UpEnt;
#define UP_ENTS 32768u
static u64 up_head_hash(const u8* p, u32 size);
static UpEnt g_up[UP_ENTS];
static u32 g_chunk_uid;
static u64 g_up_copied, g_up_reused, g_up_deferred;   // (bytes: for the log)
static bool g_up_noreuse;      // (this copy must land where asked: a padded uniform block)

// --- The draw-data ring ----------------------------------------------------------------------
// Every draw reads its data (table, uniform blocks, vertex data) through ONE storage buffer
// binding. With an upload chunk per recording, nothing uploaded in an earlier frame was
// reachable, so the same models' vertex data was copied again every frame: in the busy plaza
// 22 of the 24 MB copied per frame, ~3 ms of the drawing thread. Now draw data and indices go
// into one big buffer used as a ring, and every draw binds all of it: data uploaded in earlier
// frames is reused as long as it is still there (its ring pages not written over since: the
// page stamps), the game hasn't written that memory since (DCFlushRange marks pages) and its
// first bytes are the same. A page is written over only after the card is done with every
// recording that read it (page_use). When the ring can't give space (more than it holds in one
// recording, or a buffer that isn't allowed that big), draws use upload chunks as before.
// Texture data (copied, not read by draws) stays in the chunks. SPLAT_NO_RING=1: off.
// SPLAT_RING_MB=N: its size (192).
#define RING_PAGE_SHIFT 20u
static struct {
    int state;           // 0 not made yet, 1 in use, -1 off
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* ptr;
    u32 size, pages, head, lap_end;
    u32 guard;           // data less than this far ahead of the head isn't reused (copied again instead): its
                         // pages are written over soon, and must not be read by the recording being made then
    u32* stamp;          // per 1 MB page: bumped when the page is written over (a new lap)
    u64* use;            // per page: the newest recording that reads data in it
    u64 laps, waits, fallbacks;
} g_ring;
static bool g_up_ring;   // (the draw being set up has its data in the ring: see upload_guest)
static bool serial_done(u64 serial);
static void serial_wait(u64 serial, u64 timeout_ns);
static u64 cur_serial(void) { return g.cur >= 0 ? g.sub[g.cur].serial : 0; }
static bool ring_ready(void) {
    if (g_ring.state) return g_ring.state > 0;
    g_ring.state = -1;
    if (getenv("SPLAT_NO_RING")) { printf("[render] draw-data ring off (SPLAT_NO_RING)\n"); return false; }
    u32 mb = 192;
    { const char* e = getenv("SPLAT_RING_MB"); if (e && atoi(e) >= 64 && atoi(e) <= 1024) mb = (u32)atoi(e); }
    u64 size = (u64)mb << 20;
    if (size > g_vk.props.limits.maxStorageBufferRange) size = (u64)g_vk.props.limits.maxStorageBufferRange & ~((1ull << RING_PAGE_SHIFT) - 1);
    if (size < (64ull << 20)) { printf("[render] draw-data ring off: storage buffers limited to %u bytes\n", (unsigned)g_vk.props.limits.maxStorageBufferRange); return false; }
    if (!host_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &g_ring.buf, &g_ring.mem, (void**)&g_ring.ptr)) { printf("[render] draw-data ring off: no memory for it\n"); return false; }
    g_ring.size = (u32)size;
    g_ring.pages = (u32)(size >> RING_PAGE_SHIFT);
    g_ring.guard = g_ring.size / 4;
    g_ring.stamp = (u32*)calloc(g_ring.pages, sizeof(u32));
    g_ring.use = (u64*)calloc(g_ring.pages, sizeof(u64));
    if (!g_ring.stamp || !g_ring.use) return false;
    g_ring.state = 1;
    printf("[render] draw-data ring: %u MB (draw data from earlier frames is reused while unchanged)\n", (unsigned)(size >> 20));
    return true;
}
static void ring_use(u32 off, u32 size) {   // (this recording reads [off, off + size))
    u64 sn = cur_serial();
    for (u32 pg = off >> RING_PAGE_SHIFT; pg <= (off + (size ? size - 1 : 0)) >> RING_PAGE_SHIFT && pg < g_ring.pages; pg++) g_ring.use[pg] = sn;
}
static bool ring_near_head(u32 off) {   // (in the stretch the head writes over next)
    u32 d = off >= g_ring.head ? off - g_ring.head : off + g_ring.size - g_ring.head;
    return d < g_ring.guard;
}
static u8* ring_alloc(u32 size, u32 align, u32* off) {
    if (size > g_ring.size / 8) return NULL;
    u32 o = (g_ring.head + align - 1) / align * align, lap_end = g_ring.lap_end;
    bool wrap = false;
    if ((u64)o + size > g_ring.size) { o = 0; lap_end = 0; wrap = true; }   // (a new lap from the start)
    u32 le = lap_end;
    while (le < o + size) le += 1u << RING_PAGE_SHIFT;
    // the pages written over now: none may be read by the recording being made (then: an upload chunk)
    u64 cs = cur_serial();
    for (u32 e = lap_end; e < le; e += 1u << RING_PAGE_SHIFT) {
        u64 u = g_ring.use[e >> RING_PAGE_SHIFT];
        if (u && u >= cs) return NULL;
    }
    for (u32 e = lap_end; e < le; e += 1u << RING_PAGE_SHIFT) {   // (and the card done with them)
        u32 pg = e >> RING_PAGE_SHIFT;
        u64 u = g_ring.use[pg];
        if (u && !serial_done(u)) { serial_wait(u, UINT64_MAX); g_ring.waits++; }
        g_ring.stamp[pg]++;
        g_ring.use[pg] = 0;
    }
    if (wrap) g_ring.laps++;
    g_ring.lap_end = le;
    g_ring.head = o + size;
    ring_use(o, size);
    *off = o;
    return g_ring.ptr + o;
}
// (per kind of draw data, for the log: 0 uniform blocks, 1 vertex data, 2 uniform registers, 3 draw tables)
static int g_up_cat;
static u64 g_up_cat_copied[4], g_up_cat_reused[4];
static u64 g_ub_padded, g_ub_pad_bytes;   // (uniform blocks padded for fixed-index reads: for the log)
static int g_up_on = -1;
// Draws the GPU thread works on before the game's flush (display lists are
// handed over early, see GX2CallDisplayList): the game may still be filling
// in the uniform blocks / vertex data they read, so those are copied at the
// flush (the fence packet), or before anything is submitted -- not when the
// draw is recorded. SPLAT_NO_DEFER=1: copy right away, as before.
typedef struct { u8* dst; u32 addr, size, up; } DefCopy;
static DefCopy* g_dc;
static u32 g_dc_n, g_dc_cap, g_dc_batch = 1;
static int g_dc_on = -1;
static bool g_dbg;   // (declared here for upload_guest; see texture_view)
static bool g_dbg_nodump;   // (SPLAT_DEBUG_SCENE: the draw log without the picture dumps)
static void defer_copies_run(void) {
    if (!g_dc_n) return;
    u32 gen = wiiu_mem_generation();
    for (u32 i = 0; i < g_dc_n; i++) {
        DefCopy* c = &g_dc[i];
        const u8* src = wiiu_mem_ptr(c->addr, c->size);
        if (src) memcpy(c->dst, src, c->size);
        else memset(c->dst, 0, c->size);
        UpEnt* e = &g_up[c->up];
        if (e->pending && e->addr == c->addr && e->size == c->size && e->batch == g_dc_batch) {
            e->pending = false;
            e->gen = gen > 16 ? gen - 16 : 0;
            if (src) e->head = up_head_hash(src, c->size);
        }
        g_up_copied += c->size;
    }
    g_dc_n = 0;
    g_dc_batch++;
}
static bool draw_data_final(void) {
    static int always = -1;   // SPLAT_DEFER_ALWAYS=1 (tests): every copy waits for the next flush
    if (always < 0) always = getenv("SPLAT_DEFER_ALWAYS") != NULL;
    return !always && gx2_read_pos() <= gx2_flush_pos();
}
// Puts guest memory [addr, addr + size) at dst (chunk offset dst_off), or
// finds it already in the chunk; returns the chunk offset to read it from.
static bool up_valid(const UpEnt* e) {   // (is the copy this entry remembers still there?)
    if (!e->ring) return !g_up_ring && g.chunk && e->uid == g.chunk->uid;
    if (!g_up_ring) return false;
    u32 p0 = e->off >> RING_PAGE_SHIFT, p1 = (e->off + e->size - 1) >> RING_PAGE_SHIFT;
    return p1 < g_ring.pages && g_ring.stamp[p0] == e->st0 && g_ring.stamp[p1] == e->st1 &&
           !ring_near_head(e->off) && !ring_near_head(e->off + e->size - 1);
}
static void up_note(UpEnt* e, u32 off, u32 size) {   // (where the copy is: ring pages or chunk)
    e->ring = g_up_ring;
    if (g_up_ring) {
        e->st0 = g_ring.stamp[off >> RING_PAGE_SHIFT];
        e->st1 = g_ring.stamp[(off + size - 1) >> RING_PAGE_SHIFT];
    } else e->uid = g.chunk ? g.chunk->uid : 0;
}
static u64 up_head_hash(const u8* p, u32 size) {   // (the first 32 bytes: unchanged?)
    u64 w[4] = { 0, 0, 0, 0 };
    memcpy(w, p, size < 32 ? size : 32);
    return (w[0] * 0x9E3779B97F4A7C15ull) ^ (w[1] * 0xC2B2AE3D27D4EB4Full) ^ (w[2] * 0x165667B19E3779F9ull) ^ (w[3] * 0x27D4EB2F165667C5ull) ^ size;
}
static u32 up_slot(u32 addr, u32 size) {
    u32 h = (addr * 0x9E3779B1u) ^ (size * 0x85EBCA77u);
    return (h ^ (h >> 15)) & (UP_ENTS - 1);
}
static u32 upload_guest(u32 addr, u32 size, u8* dst, u32 dst_off) {
    if (g_up_on < 0) g_up_on = getenv("SPLAT_NO_UPLOAD_REUSE") == NULL;
    if (g_dc_on < 0) g_dc_on = getenv("SPLAT_NO_DEFER") == NULL;
    if (addr && g_dc_on && !g_cap_active && !g_dbg && !draw_data_final() &&
        !(g_so_npend && so_pending_in(addr, size)) && !(g_def_n && defer_pending_in(addr, size)) && wiiu_mem_ptr(addr, size)) {
        // (not flushed yet: copied later; the same block again in this batch is shared)
        u32 ei = up_slot(addr, size);
        UpEnt* e = &g_up[ei];
        if (g_up_on && !g_up_noreuse && size >= 64 && e->pending && e->addr == addr && e->size == size && up_valid(e) && e->batch == g_dc_batch) {
            if (e->ring) ring_use(e->off, size);
            g_up_reused += size;
            g_up_cat_reused[g_up_cat] += size;
            return e->off;
        }
        if (g_dc_n == g_dc_cap) {
            u32 nc = g_dc_cap ? g_dc_cap * 2 : 4096;
            DefCopy* nd = (DefCopy*)realloc(g_dc, nc * sizeof(DefCopy));
            if (!nd) goto now;
            g_dc = nd;
            g_dc_cap = nc;
        }
        g_dc[g_dc_n].dst = dst; g_dc[g_dc_n].addr = addr; g_dc[g_dc_n].size = size; g_dc[g_dc_n].up = ei;
        g_dc_n++;
        g_up_deferred += size;
        g_up_cat_copied[g_up_cat] += size;
        if (g_up_on && size >= 64) {
            e->addr = addr; e->size = size; e->off = dst_off; e->batch = g_dc_batch; e->pending = true; e->gen = 0;
            up_note(e, dst_off, size);
            e->head = 0;   // (filled in when the copy is made)
        }
        return dst_off;
    }
now:;
    const u8* src = addr ? gmem(addr, size) : NULL;
    if (!src) { memset(dst, 0, size); return dst_off; }
    if (g_up_on && size >= 64 && !g_cap_active) {
        UpEnt* e = &g_up[up_slot(addr, size)];
        if (!g_up_noreuse && !e->pending && e->addr == addr && e->size == size && up_valid(e) && !wiiu_mem_changed_since(addr, size, e->gen) &&
            e->head == up_head_hash(src, size)) {
            if (e->ring) ring_use(e->off, size);
            g_up_reused += size;
            g_up_cat_reused[g_up_cat] += size;
            return e->off;
        }
        u32 gen = wiiu_mem_generation();
        memcpy(dst, src, size);
        // (16 below: marks other threads were making at that moment count as later)
        e->addr = addr; e->size = size; e->gen = gen > 16 ? gen - 16 : 0; e->off = dst_off; e->pending = false; e->batch = 0;
        up_note(e, dst_off, size);
        e->head = up_head_hash(src, size);
    } else memcpy(dst, src, size);
    g_up_copied += size;
    g_up_cat_copied[g_up_cat] += size;
    return dst_off;
}

// Space in the current submission's upload memory. All of one request is in
// one chunk (so one storage buffer binding covers a whole draw).
static u8* arena(u32 size, u32 align, VkBuffer* buf, u32* off) {
    cmd();
    if (size > CHUNK_SIZE) return NULL;
    Chunk* c = g.chunk;
    u32 o = c ? (c->used + align - 1) / align * align : 0;
    if (!c || o + size > c->size) {
        c = new_chunk();
        if (!c) return NULL;
        c->next = g.sub[g.cur].chunks;
        g.sub[g.cur].chunks = c;
        g.chunk = c;
        c->uid = ++g_chunk_uid;
        o = 0;
    }
    c->used = o + size;
    *buf = c->buf;
    *off = o;
    return c->ptr + o;
}

// (the end of the newest allocation back: [off + used, off + reserved) wasn't needed)
static void arena_trim(VkBuffer buf, u32 off, u32 reserved, u32 used) {
    if (used >= reserved) return;
    if (g_ring.state > 0 && buf == g_ring.buf) { if (g_ring.head == off + reserved) g_ring.head = off + used; }
    else if (g.chunk && buf == g.chunk->buf && g.chunk->used == off + reserved) g.chunk->used = off + used;
}
// Draw data and indices: the ring when it can (see g_ring), an upload chunk otherwise.
static u8* arena_draw(u32 size, u32 align, VkBuffer* buf, u32* off) {
    cmd();
    if (ring_ready()) {
        u8* p = ring_alloc(size, align, off);
        if (p) { *buf = g_ring.buf; return p; }
        g_ring.fallbacks++;
    }
    return arena(size, align, buf, off);
}

// Without push descriptors: a descriptor set from the submission's pools.
static VkDescriptorSet alloc_set(void) {
    Sub* s = &g.sub[g.cur];
    for (;;) {
        if (!s->pool_cur) {
            DPool* p = (DPool*)calloc(1, sizeof(DPool));
            VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 * 2 },
                                              { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * 8 } };
            VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            ci.maxSets = 4096;
            ci.poolSizeCount = 2;
            ci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(g_vk.device, &ci, NULL, &p->pool) != VK_SUCCESS) { free(p); return VK_NULL_HANDLE; }
            // append to the list so later resets reuse it
            DPool** pp = &s->pools;
            while (*pp) pp = &(*pp)->next;
            *pp = p;
            s->pool_cur = p;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = s->pool_cur->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g.dsl;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &set) == VK_SUCCESS) return set;
        s->pool_cur = s->pool_cur->next;   // full: next pool (or a new one)
    }
}

static void end_pass(void) {
    if (!g.in_pass) return;
    vkCmdEndRendering(cmd());
    g.in_pass = false;
    g.need_barrier = true;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

// Before transfer work (uploads, clears, copies): leave the pass, sync.
static VkCommandBuffer transfer_cmd(void) {
    end_pass();
    VkCommandBuffer cb = cmd();
    if (g.need_barrier) { barrier_all(cb); g.need_barrier = false; }
    g.need_barrier = true;   // whatever follows must see this transfer
    return cb;
}

// --- Images ---------------------------------------------------------------------------------------

static void image_layout_init(VkCommandBuffer cb, Img* im) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = im->aspect;
    b.subresourceRange.levelCount = im->levels ? im->levels : 1;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;   // (3D images made 2D-array-compatible: all slices)
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static u32 g_next_levels = 1;   // mip levels for the next make_image (textures)
static bool make_image(Img* im, VkFormat fmt, u32 w, u32 h, u32 layers, VkImageType type, bool cube, bool attachment, bool depth) {
    memset(im, 0, sizeof(*im));
    im->levels = g_next_levels ? g_next_levels : 1;
    g_next_levels = 1;
    im->format = fmt;
    im->w = w ? w : 1;
    im->h = h ? h : 1;
    im->layers = layers ? layers : 1;
    im->cube = cube;
    im->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth && (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)) im->aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = type;
    ci.format = fmt;
    ci.extent.width = im->w;
    ci.extent.height = type == VK_IMAGE_TYPE_1D ? 1 : im->h;
    ci.extent.depth = type == VK_IMAGE_TYPE_3D ? im->layers : 1;
    ci.mipLevels = im->levels;
    ci.arrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : im->layers;
    if (type == VK_IMAGE_TYPE_3D) { im->depth3d = im->layers; im->layers = 1; }
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (attachment) ci.usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D && attachment) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // drawn one slice at a time
    if (!depth) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult cr = vkCreateImage(g_vk.device, &ci, NULL, &im->image);
    if (cr != VK_SUCCESS && (ci.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
        ci.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // some drivers refuse format-changeable images
        cr = vkCreateImage(g_vk.device, &ci, NULL, &im->image);
    }
    if (cr != VK_SUCCESS) {
        static int warned = 0;
        if (warned++ < 8) fprintf(stderr, "[render] vkCreateImage %ux%u format %d failed (%d)\n", w, h, (int)fmt, (int)cr);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.device, im->image, &req);
    VkDeviceSize off;
    if (!alloc_image_mem(req, &im->mem, &off)) {
        static int warned = 0;
        if (warned++ < 8) fprintf(stderr, "[render] no graphics memory for a %ux%u image (%llu bytes)\n", w, h, (unsigned long long)req.size);
        vkDestroyImage(g_vk.device, im->image, NULL);
        im->image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(g_vk.device, im->image, im->mem, off);
    if (attachment) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = im->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange.aspectMask = im->aspect;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(g_vk.device, &vi, NULL, &im->att_view);
    }
    VkCommandBuffer cb = transfer_cmd();
    image_layout_init(cb, im);
    VkImageSubresourceRange range = { im->aspect, 0, 1, 0, im->layers };
    if (depth) {
        VkClearDepthStencilValue v = { 1.0f, 0 };
        vkCmdClearDepthStencilImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    } else if (attachment) {
        VkClearColorValue v;
        memset(&v, 0, sizeof(v));
        vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    return true;
}

// Attachment view of one slice of a layered buffer.
static VkImageView att_view(Img* im, u32 slice) {
    u32 n = im->depth3d ? im->depth3d : im->layers;
    if (slice == 0 || slice >= n) return im->att_view;
    if (!im->att_layers) im->att_layers = (VkImageView*)calloc(n, sizeof(VkImageView));
    if (!im->att_layers[slice]) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = im->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = im->format;
        vi.subresourceRange.aspectMask = im->aspect;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.baseArrayLayer = slice;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_vk.device, &vi, NULL, &im->att_layers[slice]) != VK_SUCCESS) return im->att_view;
    }
    return im->att_layers[slice];
}

static VkComponentSwizzle swz(u32 s) {
    switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

static bool g_no_mips = false;   // SPLAT_NO_MIPS=1: no mip levels at all
// A view for sampling: view type from the shader's sampler, format (same
// size class as the image), and the texture's channel selection.
static VkImageView sample_view(Img* im, VkImageViewType type, VkFormat fmt, u32 sel) {
    bool base_only = im->levels > 1 && (g_vk.no_mips || g_no_mips);
    u64 key = ((u64)type << 48) | ((u64)(u32)fmt << 16) | sel | ((u64)base_only << 63);
    for (int i = 0; i < im->nviews; i++)
        if (im->views[i].key == key) return im->views[i].view;
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->image;
    vi.viewType = type;
    vi.format = fmt;
    vi.components.r = swz(sel & 7);
    vi.components.g = swz((sel >> 3) & 7);
    vi.components.b = swz((sel >> 6) & 7);
    vi.components.a = swz((sel >> 9) & 7);
    vi.subresourceRange.aspectMask = im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT && im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                         ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect;
    vi.subresourceRange.levelCount = 1;
    u32 layers = im->layers;
    if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_3D) layers = 1;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
    vi.subresourceRange.layerCount = layers;
    vi.subresourceRange.levelCount = base_only || !im->levels ? 1 : im->levels;
    VkImageView v = VK_NULL_HANDLE;
    u64 vt0 = pf_now();
    VkResult vr = vkCreateImageView(g_vk.device, &vi, NULL, &v);
    g_tex_ns2[0] += pf_now() - vt0;
    g_tex_views_made++;
    if (vr != VK_SUCCESS) return VK_NULL_HANDLE;
    if (im->nviews >= 24) {   // full: reuse one rather than leak (rare)
        vkDestroyImageView(g_vk.device, v, NULL);
        return im->views[0].view;
    }
    {
        im->views[im->nviews].key = key;
        im->views[im->nviews].view = v;
        im->nviews++;
    }
    return v;
}

// --- Formats ------------------------------------------------------------------------------------

static VkFormat color_format(u32 fmt) {
    LatteTexFormat tf;
    if (!latte_tex_format(fmt, &tf)) return VK_FORMAT_R8G8B8A8_UNORM;
    if (tf.block || tf.convert) return VK_FORMAT_R8G8B8A8_UNORM;   // not renderable: use RGBA8
    if (fmt == 0x11 || fmt == 0x811 || fmt == 0x81C) return VK_FORMAT_R32_UINT;
    return (VkFormat)tf.vk_format;
}

static VkFormat depth_format(u32 fmt, bool* stencil) {
    int f;
    if (!latte_depth_format(fmt, &f, stencil)) { *stencil = false; return VK_FORMAT_D32_SFLOAT; }
    if (f == VK_FORMAT_D24_UNORM_S8_UINT && !g_vk.d24s8) f = VK_FORMAT_D32_SFLOAT_S8_UINT;
    return (VkFormat)f;
}

// --- Surfaces (color/depth buffers) ---------------------------------------------------------------


// --- Smooth motion: generated pictures between the game's 60 frames (settings "Smooth motion", SPLAT_FRAMEGEN=2|3|4) -----
// Between two of the game's frames, 1-3 extra pictures are shown: the last real picture re-projected through the camera
// as it is expected to have moved by then (the camera's motion over the last two frames, carried on). The depth buffer
// gives every pixel its distance, so near and far things shift by the right amounts; the HUD -- the part of the
// picture that differs from the scene copied just before the HUD was drawn -- stays where it is. Players and ink move
// at 60 (they aren't re-animated); the camera moves at 120 / 180 / 240. Nothing is made up by guessing motion from the
// pictures (no smearing) and no input lag is added: the generated pictures carry the camera on, they don't wait for
// the next frame. Shown from the drawing thread between the game's frames (fg_tick: latte_render_poll / idle).
static struct {
    bool ok, tried, have;
    Img pic, depth, scene, out;   // the last real picture, its depth, the scene before its HUD, a generated picture
    bool scene_copied;            // (this frame's scene copy taken: the HUD is drawn after it)
    bool depth_ok;                // (the depth copy matches the picture)
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkFormat pipe_fmt;
    VkDescriptorPool pool;
    VkDescriptorSet set[2];
    VkSampler nearest;
    VkCommandBuffer cb[2];
    VkFence fence[2];
    bool busy[2];
    int which;
    struct { bool ok; float pos[3], dir[3], fovy, nr, fr; } cam[2];   // (the last two real frames' cameras)
    u64 real_ns;                  // when the last real picture was presented
    int made;                     // generated pictures shown since then
    u64 shown, skipped;           // (per [perf] line)
    Surf* dsurf;                  // the TV-size depth buffer drawn this frame
    u64 dsurf_frame;
} g_fg;
static int fg_n(void) {
    static int env = -1;
    if (env < 0) { const char* e = getenv("SPLAT_FRAMEGEN"); env = 0; if (e) { g_vk.frame_gen = atoi(e); env = 1; } }
    int n = g_vk.frame_gen;
    return n >= 2 && n <= 4 ? n : 0;
}
static void fg_scene_copy(Surf* s);
static u64 g_rt_seq = 0;
static u32 g_present_skips;   // frames not shown: the screen had no free image (see the swapchain acquire)
static u32 g_present_skips_fence;   // (of them: an image was acquired but not released within acq_fence_ns)
static Surf* g_rt_last_drawn[2];   // (colour, depth: see get_rt)
static u32 g_tv_epoch = 1;   // (texture_view's per-frame memo is valid while this stays the same)
static u64 g_tvc[7];   // (per [perf] line: slow lookups -- new/other entry, same entry a frame or epoch old (drawn buffer), (plain texture) --, epoch changes, plain re-checks passed)
static u64 g_draw_id = 0;   // counts every draw, never reset
// buffers by address (the full list is long; textures look up by address every draw)
static Surf* g_rth[4096];
#define RTH(a) ((((a) >> 11) * 2654435761u) >> 20)
#define FOR_RT_AT(t, a) for (Surf* t = g_rth[RTH((a) & ~0x7FFu)]; t; t = t->anext)

// The buffer at this address the GPU wrote most recently (games reuse the
// same memory for buffers of different sizes within one frame).
static Surf* find_rt(u32 addr, bool depth) {
    addr &= ~0x7FFu;
    Surf* best = NULL;
    FOR_RT_AT(s, addr)
        if (s->addr == addr && s->depth == depth && (!best || s->seq > best->seq)) best = s;
    return best;
}

// The drawn buffer at addr with exactly that format and size (depth or
// colour), newest first. (Several buffers share addresses: the shadow map's
// memory is also a colour buffer of another size.)
static Surf* find_rt_exact(u32 addr, u32 fmt, u32 w, u32 h) {
    addr &= ~0x7FFu;
    Surf* best = NULL;
    FOR_RT_AT(s, addr)
        if (s->addr == addr && s->gx2fmt == fmt && s->w == w && s->h == h && (!best || s->seq > best->seq)) best = s;
    return best;
}

#define RT_3D 0x10000u   // flag in get_rt's `layers`: a 3D buffer
// --- Render scale ---------------------------------------------------------------------------------
// Settings panel "Render scale" (SPLAT_RENDER_SCALE=0.5|0.75|1|1.5|2 at
// start): the buffers that make up the TV picture (1280x720) are drawn
// 1.5x or 2x bigger -- or 0.75x / 0.5x smaller, for small PCs (a quarter of
// the pixels at 0.5x); everything else -- the GamePad's 854x480, the ink maps
// the game reads back, shadow maps, lookup tables -- keeps the game's size.
// The scale actually drawn at: never more than twice the window's own pixels on a side (more is shrunk away unseen).
// A phone-shaped window in wide mode made the picture far taller than 720: at 2x a 538x964 window got a 2560x4584
// picture, the card busy 14 ms a frame (the user's Oct 26 log, 50-58 fps); capped it is 960x1720 (0.75x). A 1280x720
// window at 2x and full screen at 2x are unchanged. SPLAT_RS_NO_CAP=1: the old way.
static float rs_cap(float f) {
    static int off = -1;
    if (off < 0) off = getenv("SPLAT_RS_NO_CAP") != NULL;
    u32 W = g.sc_extent.width, H = g.sc_extent.height;
    if (off || W < 64 || H < 64) return f;   // (no window size yet, or minimized: leave it)
    float kx = 1.0f, ky = 1.0f;   // (the picture's shape, as rs_wide works it out)
    if (g_vk.wide_mode) {
        float a = (float)W / (float)H;
        if (a > 16.0f / 9.0f + 0.02f) kx = a / (16.0f / 9.0f);
        else if (a < 16.0f / 9.0f - 0.02f) ky = (16.0f / 9.0f) / a;
        if (kx > 3.6f) kx = 3.6f;
        if (ky > 3.2f) ky = 3.2f;
    }
    static const float steps[5] = { 2.0f, 1.5f, 1.0f, 0.75f, 0.5f };
    float s = 0.5f;
    for (int k = 0; k < 5; k++) {
        if (steps[k] > f) continue;
        float over = fmaxf(1280.0f * steps[k] * kx / (float)W, 720.0f * steps[k] * ky / (float)H);   // (the picture is fitted into the window)
        if (over <= 2.02f) { s = steps[k]; break; }
    }
    if (s > f) s = f;
    static float ls = -1.0f;
    static u32 lw, lh;
    if (s != ls || W != lw || H != lh) {
        if (s != f && (s != ls || ls < 0.0f)) printf("[render] render scale %g drawn at %g in this %ux%u window (more than twice the window's pixels on a side is never seen)\n", f, s, W, H);
        else if (s == f && ls >= 0.0f && ls != f) printf("[render] render scale %g drawn in full again (%ux%u window)\n", f, W, H);
        ls = s; lw = W; lh = H;
    }
    return s;
}
static float rs_factor(void) {
    static int env = -1;
    if (env < 0) {
        env = 0;
        const char* e = getenv("SPLAT_RENDER_SCALE");
        if (e) { double v = atof(e); g_vk.render_scale = v >= 2.0 ? 2 : v >= 1.5 ? 1 : v >= 1.0 ? 0 : v >= 0.75 ? 4 : 3; env = 1; }
    }
    static const float f[5] = { 1.0f, 1.5f, 2.0f, 0.5f, 0.75f };
    int i = g_vk.render_scale;
    return rs_cap(i >= 0 && i < 5 ? f[i] : 1.0f);
}
// Settings panel "Wide screen" (SPLAT_WIDE=1 at start; SPLAT_FOV=N adds N degrees to the field of view): with a
// window wider than 16:9 the 1280x720 buffers get the window's shape (a 21:9 window: 1680 wide at 1x), the camera's
// projection follows (native_funcs.c, splat_wide_aspect_x1000 / splat_fov_add_deg are what the game's projection
// functions read) and the screen-shaped orthos (HUD, menus) are widened so the HUD keeps its size in the middle.
extern volatile int splat_wide_kx_x1000, splat_wide_ky_x1000, splat_fov_add_deg, splat_hud_stretch;
static float g_kx = 1.0f, g_ky = 1.0f;   // the picture's shape against 16:9: wider by kx (a 21:9 window), or taller by ky (a phone-shaped one)
static float rs_wide(void) {
    static int env = -1;
    if (env < 0) {
        env = 0;
        const char* e = getenv("SPLAT_WIDE");
        if (e && atoi(e)) g_vk.wide_mode = 1;
        const char* f = getenv("SPLAT_FOV");
        if (f) g_vk.fov_add = atoi(f);
        if (getenv("SPLAT_DRC")) g_vk.drc_on = 1;   // (test runs: the GamePad screen shown from the start)
        const char* hs = getenv("SPLAT_HUD");
        if (hs) g_vk.hud_mode = atoi(hs);
        const char* ll = getenv("SPLAT_LOWLAT");
        if (ll) g_vk.low_latency = atoi(ll);   // (on by default: vk_ctx.c)
        const char* pm = getenv("SPLAT_PERF_MODE");   // (Performance mode from the start: the launcher's switch and tests)
        (void)pm;   // (Performance mode from the start: read with the saved settings, vk_ctx.c settings_load)
    }
    float kx = 1.0f, ky = 1.0f;
    if (g_vk.wide_mode && g.sc_extent.width && g.sc_extent.height) {
        float a = (float)g.sc_extent.width / (float)g.sc_extent.height;
        if (a > 16.0f / 9.0f + 0.02f) kx = a / (16.0f / 9.0f);
        else if (a < 16.0f / 9.0f - 0.02f) ky = (16.0f / 9.0f) / a;
        if (kx > 3.6f) kx = 3.6f;   // (32:9 at most)
        if (ky > 3.2f) ky = 3.2f;   // (9:16 at most)
    }
    // (whole pixels: a side the window's shape stretches is rounded to a multiple of 8; an unstretched
    // one is the game's size times the render scale exactly -- 0.75x: 540 high, not 544, which made
    // the camera's picture 0.7% taller)
    float f = rs_factor();
    float wpx = kx != 1.0f ? (float)((u32)(1280.0f * f * kx / 8.0f + 0.5f) * 8) : 1280.0f * f;
    float hpx = ky != 1.0f ? (float)((u32)(720.0f * f * ky / 8.0f + 0.5f) * 8) : 720.0f * f;
    kx = wpx / (1280.0f * f);
    ky = hpx / (720.0f * f);
    {
        static float lkx, lky;
        static u32 lw, lh;
        if (kx != lkx || ky != lky || g.sc_extent.width != lw || g.sc_extent.height != lh) {
            lkx = kx; lky = ky; lw = g.sc_extent.width; lh = g.sc_extent.height;
            printf("[render] picture shape: window %ux%u, render scale %g -> wider x%.3f, taller x%.3f (%gx%g)\n", lw, lh, f, kx, ky, wpx, hpx);
        }
    }
    g_kx = kx > 1.001f ? kx : 1.0f;
    g_ky = ky > 1.001f ? ky : 1.0f;
    splat_wide_kx_x1000 = g_kx > 1.0f ? (int)(g_kx * 1000.0f + 0.5f) : 0;
    splat_wide_ky_x1000 = g_ky > 1.0f ? (int)(g_ky * 1000.0f + 0.5f) : 0;
    splat_fov_add_deg = g_vk.fov_add;
    { extern volatile int splat_dojo_both; splat_dojo_both = g_vk.dojo_both && !g_vk.perf_mode; }   // (native_funcs.c: the Dojo's two views every frame)
    splat_hud_stretch = g_vk.hud_mode;
    { extern volatile int splat_volume_pct; splat_volume_pct = g_vk.volume; }
    { extern volatile int splat_game_own_fps; splat_game_own_fps = g_vk.perf_mode || g_vk.plaza_30; }   // (gx2.c: leave the scene's 30 fps request alone)
    return g_kx;
}
static float rt_scale_for(u32 w, u32 h, u32 layers, bool vol, float* sx) {
    float f = rs_factor();
    (void)rs_wide();
    *sx = 1.0f;
    if (vol) return 1.0f;   // (layered TV-sized buffers scale too: a 2-layer 1280x720 drawn with the scaled depth buffer left the picture's right third black)
    // (only the full-size buffers: the smaller ones are the bloom / blur chain 640-320-160-80-40-20, whose
    // downsample passes expect the game's sizes -- scaling part of the chain blew the lens flare up)
    bool tv_size = (w == 1280 && h == 720);
    // (the GamePad's buffers take the picture's shape too -- its cameras are built the same way -- and the
    // overlay shows their middle 854x480, so the GamePad screen looks as always; with the HUD stretched its
    // cameras are left alone, so its buffers stay 854x480)
    if (w == 854 && h == 480) { if (splat_hud_stretch) return 1.0f; *sx = g_kx; return g_ky; }
    if (!tv_size) return 1.0f;
    if ((float)(u32)(w * f) != w * f || (float)(u32)(h * f) != h * f) return 1.0f;   // (whole pixels only)
    *sx = f * g_kx;
    return f * g_ky;
}

// Settings "Shadows": 0 off (latte_render_draw leaves the map's draws out), 1 normal, 2 sharp -- the sun's
// shadow map (a square depth buffer, 2048x2048 x2 cascades in the plaza) drawn at twice the size: the game
// reads it with 0..1 coordinates, so its edges just get finer. Performance mode: normal at most.
static int shadow_mode(void) {
    static int env = -1;
    if (env == -1) { const char* e = getenv("SPLAT_SHADOWS"); env = e ? atoi(e) : -2; if (env >= 0) g_vk.shadow_quality = env; }   // (once: -2 = not set -- "< 0" here read the whole environment on every call, hundreds of times a frame)
    int m = g_vk.shadow_quality;
    if (g_vk.perf_mode && m > 1) m = 1;
    return m;
}
// The sun's shadow map: a square depth buffer of 1024 or 2048, 16-bit (005) or its 32-bit copy (80E) -- the
// plaza's 2048x2048 x2 cascades, Octo Valley's 1024. (Not just "square and big": Octo Valley's ink uses a
// 1600x1600 depth buffer (format 011), and drawing that one at 2x -- or leaving its draws out -- broke the ink
// check: the squid couldn't swim.)
static bool sun_shadow_map(u32 fmt, u32 w, u32 h) {
    return w == h && (w == 1024 || w == 2048) && ((fmt & 0x3F) == 0x05 || fmt == 0x80E);
}
static Surf* get_rt(u32 addr, u32 fmt, u32 w, u32 h, bool depth, u32 layers) {
    addr &= ~0x7FFu;
    bool vol = (layers & RT_3D) != 0 && !depth;
    layers &= 0xFFFF;
    if (layers == 0) layers = 1;
    if (layers > 64) layers = 64;
    float wantx;
    float want = rt_scale_for(w ? w : 1, h ? h : 1, layers, vol, &wantx);
    if (depth && !vol && sun_shadow_map(fmt, w, h) && shadow_mode() == 2) want = wantx = 2.0f;   // (sharp shadows)
    Surf* s;
    for (s = g_rth[RTH(addr)]; s; s = s->anext)
        if (s->addr == addr && s->depth == depth && s->gx2fmt == fmt && s->w == (w ? w : 1) && s->h == (h ? h : 1) && s->layers == layers && s->vol == vol && s->scale == want && s->scalex == wantx) {
            s->rendered = true;
            if (g_rt_last_drawn[depth] != s) {   // (another buffer at this address? which one is newest changes)
                g_rt_last_drawn[depth] = s;
                FOR_RT_AT(o, addr) if (o != s && o->addr == addr) { g_tv_epoch++; g_tvc[3]++; break; }
            }
            s->seq = ++g_rt_seq;
            s->wdraw = g_draw_id;
            s->gen = wiiu_mem_generation();
            return s;
        }
    static u32 failed[32][4];
    static u32 nfailed = 0;
    for (u32 i = 0; i < nfailed; i++)
        if (failed[i][0] == addr && failed[i][1] == fmt && failed[i][2] == w && failed[i][3] == h) return NULL;
    s = (Surf*)calloc(1, sizeof(Surf));
    s->addr = addr;
    s->gx2fmt = fmt;
    s->w = w ? w : 1;
    s->h = h ? h : 1;
    s->depth = depth;
    s->layers = layers;
    s->vol = vol;
    s->rendered = true;
    s->scale = want;
    s->scalex = wantx;
    VkFormat vf = depth ? depth_format(fmt, &s->stencil) : color_format(fmt);
    if (depth) {   // (experiments for the Hammerhead Bridge device lost: a 1600x1600 x5 depth+stencil array)
        static int nls = -1, d24 = -1;
        if (nls < 0) { nls = getenv("SPLAT_NO_LAYERED_STENCIL") != NULL; d24 = getenv("SPLAT_FORCE_D24S8") != NULL; printf("[render] depth experiments: no layered stencil %d, force D24S8 %d (D24S8 supported %d)\n", nls, d24, g_vk.d24s8); }
        if (nls && layers > 1 && s->stencil) { vf = VK_FORMAT_D32_SFLOAT; s->stencil = false; printf("[render] layered depth buffer %08X %ux%u x%u: made without stencil (SPLAT_NO_LAYERED_STENCIL)\n", addr, s->w, s->h, layers); }
        if (d24 && s->stencil && g_vk.d24s8) vf = VK_FORMAT_D24_UNORM_S8_UINT;
    }
    bool cube = !vol && layers >= 6 && layers % 6 == 0 && s->w == s->h;
    if (!make_image(&s->img, vf, (u32)(s->w * wantx + 0.5f), (u32)(s->h * want + 0.5f), layers, vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, cube, true, depth)) {
        fprintf(stderr, "[render] couldn't create a %ux%u %s buffer (format %03X)\n", w, h, depth ? "depth" : "color", fmt);
        if (nfailed < 32) { failed[nfailed][0] = addr; failed[nfailed][1] = fmt; failed[nfailed][2] = w; failed[nfailed][3] = h; nfailed++; }
        free(s);
        return NULL;
    }
    s->seq = ++g_rt_seq;
    g_tv_epoch++; g_tvc[3]++;
    s->wdraw = g_draw_id;
    s->gen = wiiu_mem_generation();
    s->next = g.rts;
    g.rts = s;
    s->anext = g_rth[RTH(addr)];
    g_rth[RTH(addr)] = s;
    static int logged = 0;
    if (logged++ < 24 || want != 1.0f)
        printf("[render] new %s buffer %ux%u%s format %03X at %08X%s\n", depth ? "depth" : "color", s->w, s->h, layers > 1 ? " (layered)" : "", fmt, addr,
               want != 1.0f ? " (drawn at render scale)" : "");
    return s;
}

// --- Textures ---------------------------------------------------------------------------------------

static u64 hash_bytes(const void* p, size_t n, u64 h) {
    const u8* b = (const u8*)p;
    while (n >= 8) {
        u64 v;
        memcpy(&v, b, 8);
        h ^= v;
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
        b += 8;
        n -= 8;
    }
    while (n--) { h ^= *b++; h *= 0x100000001B3ull; }
    return h;
}

// Textures by the 1 MB regions of game memory they cover (level 0 and the
// mip levels), so GX2Invalidate only looks at the few textures in its range
// instead of every texture the game ever used (thousands, many times a frame).
typedef struct { Surf** v; u32 n, cap; } TexBucket;
static TexBucket g_tex_idx[4096];
static void tex_index_add(Surf* s, u32 addr, u32 size) {
    if (!size) return;
    u64 end = (u64)addr + size;
    for (u64 r = addr >> 20; r <= ((end - 1) >> 20) && r < 4096; r++) {
        TexBucket* b = &g_tex_idx[r];
        if (b->n == b->cap) {
            b->cap = b->cap ? b->cap * 2 : 8;
            b->v = (Surf**)realloc(b->v, b->cap * sizeof(Surf*));
        }
        b->v[b->n++] = s;
    }
}

static u64 mem_hash(u32 addr, u32 size) {
    const u8* p = wiiu_mem_ptr(addr, size);
    if (!p) return 0;
    if (size <= (256u << 10)) return hash_bytes(p, size, 0xCBF29CE484222325ull);
    // big: sample 512 spread-out pieces
    u64 h = 0xCBF29CE484222325ull ^ size;
    u32 step = size / 512;
    for (u32 i = 0; i < 512; i++) h = hash_bytes(p + (size_t)i * step, 32, h);
    return h;
}

// (not a Latte DIM: a cube map read as a cube-map array, see LS_PS_CUBE_ARRAYS)
#define DIM_CUBE_ARRAY 8u
static VkImageViewType view_type(u32 dim) {
    switch (dim) {
    case 0: return VK_IMAGE_VIEW_TYPE_1D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return VK_IMAGE_VIEW_TYPE_CUBE;
    case 4: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case 5: case 7: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case DIM_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

// --- Texture decodes on helper threads ----------------------------------------------------------
// A scene load brings thousands of texture levels in one frame (the plaza:
// 4,774 levels, 59 MB), each un-tiled out of game memory one after another on
// the drawing thread: 130-630 ms frames. Now the drawing thread records each
// level's copy at once and hands the un-tiling to decode workers (they sleep
// when there's nothing to do); before anything is submitted (tex_jobs_finish)
// it helps with what's left and waits for the rest. The data goes into the
// submission's own upload memory, so nothing else reads it before then.
// SPLAT_SYNC_TEX_DECODE=1: on the drawing thread as before.
typedef struct { LatteTexDesc d; LatteTexFormat tf; u8* dst; size_t bytes; } TexJob;
#define TEXJ_MAX 8192
#define TEXJ_MIN_BYTES 16384u   // (smaller levels: decoded on the spot)
static TexJob g_tj[TEXJ_MAX];
static u32 g_tj_n, g_tj_next, g_tj_busy;   // (queued, taken, being decoded: under g_tj_lock)
static hmtx_t g_tj_lock;
static hcnd_t g_tj_cv, g_tj_done_cv;
static int g_tj_on = -1;
static u64 g_tj_count, g_tj_bytes, g_tj_wait_ns;   // (for the log)
static void tex_job_run(TexJob* j) {
    if (!latte_tex_decode(&j->d, &j->tf, j->dst, j->bytes)) memset(j->dst, 0, j->bytes);
}
static int tex_worker(void* arg) {
    (void)arg;
    for (;;) {
        hmtx_lock(&g_tj_lock);
        while (g_tj_next >= g_tj_n) hcnd_wait(&g_tj_cv, &g_tj_lock);
        TexJob* j = &g_tj[g_tj_next++];
        g_tj_busy++;
        hmtx_unlock(&g_tj_lock);
        tex_job_run(j);
        hmtx_lock(&g_tj_lock);
        g_tj_busy--;
        if (g_tj_next >= g_tj_n && !g_tj_busy) hcnd_broadcast(&g_tj_done_cv);
        hmtx_unlock(&g_tj_lock);
    }
    return 0;
}
static bool tex_jobs_on(void) {
    if (g_tj_on < 0) {
        g_tj_on = 0;
        if (getenv("SPLAT_SYNC_TEX_DECODE")) { printf("[render] texture decodes on the drawing thread (SPLAT_SYNC_TEX_DECODE)\n"); return false; }
        int procs = 8;
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        procs = (int)si.dwNumberOfProcessors;
#endif
        int n = procs / 2 - 1;   // (+ the drawing thread itself, while it waits)
        if (n < 1) n = 1;
        if (n > 6) n = 6;
        { const char* e = getenv("SPLAT_TEX_DECODERS"); if (e) n = atoi(e); }
        if (n <= 0) return false;
        latte_tex_prepare();
        hmtx_init(&g_tj_lock, false);
        hcnd_init(&g_tj_cv);
        hcnd_init(&g_tj_done_cv);
        for (int i = 0; i < n; i++) { hthrd_t t; hthrd_create(&t, tex_worker, NULL, 1u << 20); }
        g_tj_on = 1;
        printf("[render] texture decodes on %d helper threads\n", n);
    }
    return g_tj_on == 1;
}
// Everything queued decoded (the drawing thread helps): before a submission.
static void tex_jobs_finish(void) {
    if (g_tj_on != 1) return;
    hmtx_lock(&g_tj_lock);
    if (!g_tj_n) { hmtx_unlock(&g_tj_lock); return; }
    u64 t0 = pf_now();
    while (g_tj_next < g_tj_n) {
        TexJob* j = &g_tj[g_tj_next++];
        g_tj_busy++;
        hmtx_unlock(&g_tj_lock);
        tex_job_run(j);
        hmtx_lock(&g_tj_lock);
        g_tj_busy--;
    }
    if (g_tj_busy) { wait_mark("the texture decode helpers to finish (tex_jobs_finish)", NULL); while (g_tj_busy) hcnd_wait(&g_tj_done_cv, &g_tj_lock); wait_unmark(); }
    g_tj_n = g_tj_next = 0;
    hmtx_unlock(&g_tj_lock);
    u64 dt = pf_now() - t0;
    g_tj_wait_ns += dt;
    g_tex_ns[2] += dt;
    g_pf[PF_TEXTURES] += dt;
}
static bool tex_job_queue(const LatteTexDesc* d, const LatteTexFormat* tf, u8* dst, size_t bytes) {
    if (bytes < TEXJ_MIN_BYTES || g_cap_active || !tex_jobs_on()) return false;
    hmtx_lock(&g_tj_lock);
    bool full = g_tj_n >= TEXJ_MAX;
    hmtx_unlock(&g_tj_lock);
    if (full) tex_jobs_finish();
    hmtx_lock(&g_tj_lock);
    TexJob* j = &g_tj[g_tj_n++];
    j->d = *d; j->tf = *tf; j->dst = dst; j->bytes = bytes;
    hcnd_signal(&g_tj_cv);
    hmtx_unlock(&g_tj_lock);
    g_tj_count++;
    g_tj_bytes += bytes;
    return true;
}

static bool upload_level_(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 level) {
    u32 ew = tf->block ? (d->width + 3) / 4 : d->width, eh = tf->block ? (d->height + 3) / 4 : d->height;
    u32 slices = d->slices ? d->slices : 1;
    size_t bytes = (size_t)ew * eh * tf->elem_bytes * slices;
    if (bytes > MAX_TEX_DATA || bytes > CHUNK_SIZE) return false;
    VkBuffer buf;
    u32 off;
    u8* dst = arena((u32)bytes, 16, &buf, &off);
    if (!dst) return false;
    static int tex_save = -1;
    static u32 want;
    if (tex_save < 0) { const char* e = getenv("TEX_SAVE"); tex_save = e != NULL; if (e) want = (u32)strtoul(e, NULL, 16); }
    u64 td0 = pf_now();
    if (tex_save || !tex_job_queue(d, tf, dst, bytes))
        if (!latte_tex_decode(d, tf, dst, bytes)) memset(dst, 0, bytes);
    g_tex_ns[2] += pf_now() - td0;
    g_tex_uploads++;
    g_tex_upload_bytes += bytes;
    {   // TEX_SAVE=addr (debugging): write the texture's data as uploaded
        bool on = tex_save == 1;
        if (on && ((d->addr & ~0x7FFu) == (want & ~0x7FFu) || (s->addr & ~0x7FFu) == (want & ~0x7FFu))) {
            char name[96];
            snprintf(name, sizeof(name), "tex_%08X_L%u_%ux%u_%02X_m%u.bin", d->addr, level, d->width, d->height, d->hwfmt, d->tile_mode);
            FILE* f = fopen(name, "wb");
            if (f) { fwrite(dst, 1, bytes, f); fclose(f); printf("[render] saved %s (pitch %u swizzle %X)\n", name, d->pitch, d->swizzle); }
        }
    }
    VkCommandBuffer cb = transfer_cmd();
    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = off;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = level;
    copy.imageSubresource.layerCount = is3d ? 1 : slices;
    copy.imageExtent.width = d->width;
    copy.imageExtent.height = d->height;
    copy.imageExtent.depth = is3d ? slices : 1;
    vkCmdCopyBufferToImage(cb, buf, s->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    return true;
}

// Level 0 and, when the image has them, the mip levels (laid out in game
// memory the way GX2 computes them; see gx2_layout.c). Without mip levels
// distant and small surfaces shimmer and look noisy.
static bool upload_texture_(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 dim) {
    u32 levels = s->img.levels ? s->img.levels : 1;
    if (levels == 1) return upload_level(s, d, tf, is3d, 0);
    if (getenv("SPLAT_MIP_LOG")) printf("[mips] %08X %ux%u fmt %02X mode %u levels %u mip %08X span %X\n", d->addr, d->width, d->height, d->hwfmt, d->tile_mode, levels, s->mip_addr, s->mip_span);
    GX2Layout L;
    u32 fmt = d->hwfmt;
    gx2_surface_layout(dim, d->width, d->height, d->slices ? d->slices : 1, levels, fmt, 0, 1, d->tile_mode, d->swizzle, &L);
    // the game's own offsets when we have them (and a note when ours differ)
    GX2MipInfo mi;
    u32 stop = 0xFF;
    if (gx2_lookup_mips(s->mip_addr & ~0xFFu, &mi)) {
        static int warned = 0;
        for (u32 i = 1; i + 1 < levels && i < 13; i++)
            if (mi.offs[i] != L.mip_offset[i]) {
                if (warned++ < 12)
                    printf("[render] texture %08X %ux%u fmt %02X mode %u: mip %u at +%X, we computed +%X\n", d->addr, d->width, d->height,
                           d->hwfmt, d->tile_mode, i + 1, mi.offs[i], L.mip_offset[i]);
                break;
            }
        for (u32 i = 1; i < 13; i++) L.mip_offset[i] = mi.offs[i];
        stop = (mi.swizzle >> 16) & 0xFF;
    }
    if (dim == 3 && d->slices >= 6) {   // (cube maps: where their mip levels really are -- the purple sheen)
        static int logged = 0;
        if (logged++ < 16) {
            GX2Layout L2;
            gx2_surface_layout(dim, d->width, d->height, d->slices, levels, fmt, 0, 1, d->tile_mode, d->swizzle, &L2);
            printf("[mips] cube array %08X %ux%u x%u fmt %02X levels %u mip %08X: game offsets %s", d->addr, d->width, d->height,
                   d->slices, fmt, levels, s->mip_addr, gx2_lookup_mips(s->mip_addr & ~0xFFu, &mi) ? "" : "(none)");
            for (u32 i = 1; i + 1 < levels && i < 13; i++) printf(" %X", L.mip_offset[i]);
            printf(" | ours");
            for (u32 i = 1; i + 1 < levels && i < 13; i++) printf(" %X", L2.mip_offset[i]);
            printf(" | 4 KB pages with data:");
            u32 m = s->mip_addr & ~0xFFFu;
            for (u32 p = 0; p < 112; p++) {
                const u8* q = wiiu_mem_ptr(m + p * 0x1000, 0x1000);
                bool any = false;
                for (u32 k = 0; q && k < 0x1000 && !any; k += 4) any = *(const u32*)(q + k) != 0;
                if ((p & 15) == 0) printf(" ");
                printf("%c", any ? '#' : '.');
            }
            printf("\n");
        }
    }
    bool ok = true;
    for (u32 lv = 0; lv < levels && ok; lv++) {
        const GX2LevelLayout* ll = &L.level[lv];
        if (lv == 0) { ok = upload_level(s, d, tf, is3d, 0); continue; }   // (as the registers say)
        LatteTexDesc ld = *d;
        ld.width = ll->width;
        ld.height = ll->height;
        ld.slices = ll->depth;
        ld.pitch = tf->block ? ll->pitch * 4 : ll->pitch;
        ld.tile_mode = ll->mode;
        ld.rows = ll->height_pad;
        {
            // GX2 XORs the swizzle into the mip pointer when level 1 is swizzled
            bool l1_swz = stop != 0xFF ? stop > 1 : L.level[1].swizzled;
            u32 a = l1_swz ? s->mip_addr ^ (d->swizzle & 0x700) : s->mip_addr;
            bool swz = (ll->mode >= 4 && ll->mode <= 15) && (stop != 0xFF ? lv < stop : ll->swizzled);
            ld.addr = lv == 1 ? a : a + L.mip_offset[lv - 1];
            ld.swizzle = swz ? (d->swizzle & 0x700) : 0;
        }
        if (g_cap_active && lv > 0) cap_mem(ld.addr, latte_tex_span(&ld, tf));
        ok = upload_level(s, &ld, tf, is3d, lv);
    }
    return ok;
}

// Can an image made for resource dimension `rdim` be viewed as `vt`?
static bool view_fits(u32 rdim, u32 layers, bool cube_ok, VkImageViewType vt) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_3D: return rdim == 2;
    case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_ok && layers >= 6;
    case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return rdim != 2 && rdim != 0 && rdim != 4;
    case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return rdim == 0 || rdim == 4;
    default: return false;
    }
}

// Texture unit -> image view. `dim_shader` is what the shader was built for.
// A copy of a depth buffer, for a draw that samples the depth buffer it is
// testing against (screen-space shadows: read depth, test depth, no writes).
static VkImageView feedback_copy(Surf* s, VkImageViewType vt) {
    if (!s->depth || s->layers != 1 || s->vol) return VK_NULL_HANDLE;
    if (!s->fb_ok) {
        if (!make_image(&s->fb_copy, s->img.format, s->img.w, s->img.h, 1, VK_IMAGE_TYPE_2D, false, false, true)) return VK_NULL_HANDLE;
        s->fb_ok = true;
    }
    // (the copy is still good if nothing wrote the depth buffer since: soft
    // particles read it one emitter after another without writing it -- a
    // full copy for each of them broke up the frame and cost a lot)
    if (s->fb_seq == s->wseq && s->fb_frame == g.frame + 1) {
        g_ss.fb_reuse++;
        return sample_view(&s->fb_copy, vt, s->fb_copy.format, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    }
    s->fb_seq = s->wseq;
    s->fb_frame = g.frame + 1;
    VkCommandBuffer cb = transfer_cmd();
    VkImageCopy rg;
    memset(&rg, 0, sizeof(rg));
    rg.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    rg.srcSubresource.layerCount = 1;
    rg.dstSubresource = rg.srcSubresource;
    rg.extent.width = s->img.w;
    rg.extent.height = s->img.h;
    rg.extent.depth = 1;
    vkCmdCopyImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, s->fb_copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    return sample_view(&s->fb_copy, vt, s->fb_copy.format, 0 | (1 << 3) | (2 << 6) | (3 << 9));
}

// Where mip levels 1.. of a texture are, as the game laid them out.
static void mip_level_addrs(u32 dim, u32 width, u32 height, u32 slices, u32 nlev, u32 hw, u32 tile, u32 swizzle, u32 mip_addr, u32* laddr) {
    GX2Layout L;
    gx2_surface_layout(dim, width, height, slices, nlev, hw, 0, 1, tile, swizzle, &L);
    GX2MipInfo mi;
    u32 stop = 0xFF;
    if (gx2_lookup_mips(mip_addr & ~0xFFu, &mi)) {
        for (u32 i = 1; i < 13; i++) L.mip_offset[i] = mi.offs[i];
        stop = (mi.swizzle >> 16) & 0xFF;
    }
    bool l1_swz = stop != 0xFF ? stop > 1 : L.level[1].swizzled;
    u32 m = l1_swz ? mip_addr ^ (swizzle & 0x700) : mip_addr;
    for (u32 lv = 1; lv < nlev; lv++) laddr[lv] = (lv == 1 ? m : m + L.mip_offset[lv - 1]) & ~0x7FFu;
}

// A buffer the game drew, read as a texture with mip levels: the game draws
// each level into its own buffer (the light probes Splatoon's lighting reads
// with "level = how much in shadow": level 0 is the sunlit light, level 1 the
// light in shadow). Put the levels together in one image so the shader's
// level choice works -- without it, everything got the sunlit light and the
// shadows nearly vanished. Returns NULL when there are no level buffers.
static Img* rt_with_mips(Surf* s, u32 dim, u32 width, u32 height, u32 slices, u32 nlev, u32 hw, u32 tile, u32 swizzle, u32 mip_addr) {
    static int off = -1;
    if (off < 0) off = getenv("SPLAT_NO_RTMIPS") != NULL;
    if (off || s->vol || s->depth || nlev < 2 || s->w != width || s->h != height) return NULL;
    if (s->mipc_ok && s->mipc.levels < nlev) nlev = s->mipc.levels;
    u32 laddr[14];
    mip_level_addrs(dim, width, height, slices, nlev, hw, tile, swizzle, mip_addr, laddr);
    Surf* lr[14] = { s };
    u32 found = 1;
    bool stale = false;
    u64 seqsum = s->seq;
    for (u32 lv = 1; lv < nlev; lv++) {
        u32 lw = width >> lv, lh = height >> lv;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        // The level is what the game last drew at that address -- like on
        // the console, where the texture just reads that memory. So: the
        // newest buffer of the level's size and format drawn there, unless
        // something else was drawn over that memory later (another buffer
        // that isn't a level of this texture: the small levels share
        // addresses). (It used to also have
        // to be drawn within 2 frames of level 0; the light probes are
        // redrawn a level at a time now and then, so the levels came and
        // went: lighting and shadows flickered.)
        Surf* best = NULL;
        FOR_RT_AT(t, laddr[lv])
            if (t->addr == laddr[lv] && !t->depth && !t->vol && t->w == lw && t->h == lh && t->img.format == s->img.format &&
                (!best || t->seq > best->seq)) best = t;
        if (best)
            FOR_RT_AT(t, laddr[lv]) {
                if (t->addr != laddr[lv] || t->seq <= best->seq) continue;
                bool chain = !t->depth && !t->vol && t->img.format == s->img.format;
                if (chain) {
                    chain = false;
                    for (u32 k = 1; k < 14 && !chain; k++) {
                        u32 kw = width >> k, kh = height >> k;
                        chain = t->w == (kw ? kw : 1) && t->h == (kh ? kh : 1);
                    }
                }
                if (!chain) { best = NULL; break; }
            }
        if (!best) break;
        // Level 0 drawn this frame, this level last frame, and it's one the
        // game draws every frame: it still holds last frame's picture (the
        // game makes it again only later in the frame). Sampling it showed
        // last frame's picture over this one -- the ghost copies of the
        // plaza tower in the sky when the camera turns. Make the level from
        // level 0 instead (below).
        // (Only then: the light probes' levels are drawn once in a while,
        // a level at a time, so level 1 -- the light in shadow -- is often
        // older than level 0, and still the right one. Taking it from level 0
        // gave shadowed skin the sunlit sky light: the purple glow under the
        // chin and on the hair's underside.)
        if (best->seq < s->seq && s->seen == g.frame + 1 && best->seen != s->seen && best->recurring) { stale = true; break; }
        {   // FORCE_STALE_LEVELS=1 (debugging): every level made from level 0 (how v0.18 lit the light probes)
            static int force = -1;
            if (force < 0) force = getenv("FORCE_STALE_LEVELS") != NULL;
            if (force) { stale = true; break; }
        }
        lr[lv] = best;
        {   // MIPDBG=1 (debugging): which buffers became the levels
            static int on = -1;
            if (on < 0) on = getenv("MIPDBG") != NULL;
            if (on) printf("[mipdbg] %08X %ux%u level %u at %08X <- buffer %ux%u seq %llu seen %llu (level 0 seq %llu seen %llu, frame %llu)\n", s->addr, width, height, lv,
                           laddr[lv], best->w, best->h, (unsigned long long)best->seq, (unsigned long long)best->seen, (unsigned long long)s->seq,
                           (unsigned long long)s->seen, (unsigned long long)g.frame);
        }
        found++;
        seqsum = seqsum * 31 + best->seq;
    }
    if (s->mip_found && s->mip_found != found) {   // (a sign of flickering lighting)
        static int logged = 0;
        if (logged++ < 20) printf("[render] buffer %08X %ux%u: mip levels found went from %u to %u (frame %u)\n", s->addr, s->w, s->h, s->mip_found, found, (unsigned)g.frame);
    }
    s->mip_found = (u8)found;
    if (found < 2 && !stale) return NULL;
    if (!s->mipc_ok) {
        g_next_levels = nlev;
        if (!make_image(&s->mipc, s->img.format, s->img.w, s->img.h, s->layers, VK_IMAGE_TYPE_2D, s->img.cube, false, false)) return NULL;
        s->mipc_ok = true;
        s->mipc_seq = 0;
        static int logged = 0;
        if (logged++ < 8) printf("[render] buffer %08X %ux%u read with %u of %u mip levels the game drew\n", s->addr, s->w, s->h, found, nlev);
    }
    if (s->mipc_seq != seqsum + found) {
        // (a level this very draw draws into still holds its old picture:
        // copy it again next time, after the draw -- otherwise the copy of
        // last frame's level stayed, and the plaza's depth-of-field showed
        // last frame's blurred picture over this one: ghost images)
        bool live = false;
        for (u32 lv = 0; lv < found; lv++) if (lr[lv]->wdraw == g_draw_id) live = true;
        s->mipc_seq = live ? 0 : seqsum + found;
        VkCommandBuffer cb = transfer_cmd();
        for (u32 lv = 0; lv < s->mipc.levels; lv++) {
            u32 lw = s->img.w >> lv, lh = s->img.h >> lv;   // (render scale: the image's sizes, not the game's)
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (lv < found && (lr[lv]->img.w != lw || lr[lv]->img.h != lh)) {   // (render scale: the level's buffer has the game's size; stretch it)
                barrier_all(cb);
                VkImageBlit bl;
                memset(&bl, 0, sizeof(bl));
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = lr[lv]->img.layers < s->mipc.layers ? lr[lv]->img.layers : s->mipc.layers;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstSubresource.mipLevel = lv;
                bl.srcOffsets[1].x = (int)lr[lv]->img.w; bl.srcOffsets[1].y = (int)lr[lv]->img.h; bl.srcOffsets[1].z = 1;
                bl.dstOffsets[1].x = (int)lw; bl.dstOffsets[1].y = (int)lh; bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(cb, lr[lv]->img.image, VK_IMAGE_LAYOUT_GENERAL, s->mipc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            } else if (lv < found) {
                u32 layers = lr[lv]->img.layers < s->mipc.layers ? lr[lv]->img.layers : s->mipc.layers;
                VkImageCopy ic;
                memset(&ic, 0, sizeof(ic));
                ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ic.srcSubresource.layerCount = layers;
                ic.dstSubresource = ic.srcSubresource;
                ic.dstSubresource.mipLevel = lv;
                ic.extent.width = lw;
                ic.extent.height = lh;
                ic.extent.depth = 1;
                vkCmdCopyImage(cb, lr[lv]->img.image, VK_IMAGE_LAYOUT_GENERAL, s->mipc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            } else {   // (a level the game didn't draw: a smaller copy of the one above)
                barrier_all(cb);
                VkImageBlit bl;
                memset(&bl, 0, sizeof(bl));
                bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                bl.srcSubresource.layerCount = s->mipc.layers;
                bl.srcSubresource.mipLevel = lv - 1;
                bl.dstSubresource = bl.srcSubresource;
                bl.dstSubresource.mipLevel = lv;
                u32 pw = s->img.w >> (lv - 1), ph = s->img.h >> (lv - 1);
                bl.srcOffsets[1].x = (int)(pw ? pw : 1); bl.srcOffsets[1].y = (int)(ph ? ph : 1); bl.srcOffsets[1].z = 1;
                bl.dstOffsets[1].x = (int)lw; bl.dstOffsets[1].y = (int)lh; bl.dstOffsets[1].z = 1;
                vkCmdBlitImage(cb, s->mipc.image, VK_IMAGE_LAYOUT_GENERAL, s->mipc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            }
        }
    }
    return &s->mipc;
}

// Render scale and the bloom / blur chains: their first pass draws a buffer of the game's size or smaller
// (1280x720 -> 640x360) with taps placed for a 1280-wide picture. From the scaled picture (2560 wide at 2x)
// one bilinear tap then covers 2x2 of the 4x4 pixels each output pixel stands for: thin bright things (the
// lamp cages, edges) fall in and out of the taps, and the glow came out blocky and crawling. Such passes read
// a copy of the picture at the game's size instead (a filtered blit: the 2x2 average at 2x), as on the
// console. SPLAT_BLOOM_OLD=1: the scaled picture as before.
static bool g_small_target;   // (this draw: only unscaled colour buffers of at most g_small_w wide)
static u32 g_small_w;
static bool g_tv_gsc;         // (texture_view_slow handed out such a copy: not for the memo)
static Img* rt_game_size(Surf* s) {
    static int old = -1;
    if (old < 0) old = getenv("SPLAT_BLOOM_OLD") != NULL;
    if (old || (g_vk.wide_test & 4) || s->depth || s->vol || s->img.levels != 1) return NULL;   // (live test "WT:4": off)
    static VkFormat fmts[16];
    static bool fmt_ok[16];
    static int nf;
    int fi = 0;
    while (fi < nf && fmts[fi] != s->img.format) fi++;
    if (fi == nf) {   // (a filtered blit of this format: the card must allow it)
        if (nf == 16) return NULL;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_vk.gpu, s->img.format, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        fmts[nf] = s->img.format;
        fmt_ok[nf++] = (fp.optimalTilingFeatures & need) == need;
    }
    if (!fmt_ok[fi]) return NULL;
    if (!s->gsc_ok) {
        if (!make_image(&s->gsc, s->img.format, s->w, s->h, s->layers, VK_IMAGE_TYPE_2D, false, false, false)) return NULL;
        s->gsc_ok = true;
        s->gsc_seq = 0;
        static int logged = 0;
        if (logged++ < 4) printf("[render] buffer %08X %ux%u (drawn at %ux%u) read at the game's size by a smaller pass (glow / blur)\n", s->addr, s->w, s->h, s->img.w, s->img.h);
    }
    if (s->gsc_seq != s->seq || s->wdraw == g_draw_id) {   // (drawn since the last copy)
        s->gsc_seq = s->seq;
        VkCommandBuffer cb = transfer_cmd();
        VkImageBlit bl;
        memset(&bl, 0, sizeof(bl));
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = s->layers;
        bl.dstSubresource = bl.srcSubresource;
        bl.srcOffsets[1].x = (int)s->img.w; bl.srcOffsets[1].y = (int)s->img.h; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int)s->w; bl.dstOffsets[1].y = (int)s->h; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, s->gsc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
    return &s->gsc;
}

static Surf* g_tv_surf;   // (the game-drawn buffer the last texture_view used)
static Surf* g_tvs_tex;   // (the plain texture the slow path settled on; see the memo in texture_view)
static VkImageView texture_view_slow(u32 res_reg, u32 dim_shader) {
    u32 w0 = R(res_reg), w1 = R(res_reg + 1), w2 = R(res_reg + 2), w3 = R(res_reg + 3), w4 = R(res_reg + 4), w5 = R(res_reg + 5);
    u32 dim = w0 & 7, tile = (w0 >> 3) & 0xF;
    u32 pitch = (((w0 >> 8) & 0x7FF) + 1) * 8;
    u32 width = ((w0 >> 19) & 0x1FFF) + 1, height = (w1 & 0x1FFF) + 1, depth = ((w1 >> 13) & 0x1FFF) + 1;
    u32 hw = (w1 >> 26) & 0x3F;
    u32 base = w2 << 8;
    u32 numfmt = (w4 >> 8) & 3;
    u32 fmt = hw | (numfmt == 1 ? 0x100 : 0) | ((w4 & 1) ? 0x200 : 0) | ((w4 >> 11) & 1 ? 0x400 : 0);
    u32 sel = ((w4 >> 16) & 7) | (((w4 >> 19) & 7) << 3) | (((w4 >> 22) & 7) << 6) | (((w4 >> 25) & 7) << 9);
    if (!base) return VK_NULL_HANDLE;
    {   // TEX_INFO=addr (debugging): print how the game set that texture up
        static int on = -1;
        static u32 want, shown;
        if (on < 0) { const char* e = getenv("TEX_INFO"); on = e != NULL; if (e) want = (u32)strtoul(e, NULL, 16); }
        if (on && (base & ~0x7FFu) == (want & ~0x7FFu) && shown++ < 4)
            printf("[texinfo] %08X fmt %03X words %08X %08X %08X %08X %08X %08X\n", base, fmt, w0, w1, w2, w3, w4, w5);
    }
    {   // SPLAT_DUMMY_TEX=a,b,...: these textures read as the stand-in (for finding what darkens a scene)
        static u32 dummies[16];
        static int nd = -1;
        if (nd < 0) {
            nd = 0;
            const char* e = getenv("SPLAT_DUMMY_TEX");
            while (e && *e && nd < 16) { dummies[nd++] = (u32)strtoul(e, (char**)&e, 16); if (*e == ',') e++; else break; }
        }
        for (int i = 0; i < nd; i++) if ((base & ~0x7FFu) == (dummies[i] & ~0x7FFu)) return VK_NULL_HANDLE;
        // SPLAT_WHITE_TEX=a,b,...: these read as plain white instead
        static u32 whites[16];
        static int nw = -1;
        static Img white2d;
        if (nw < 0) {
            nw = 0;
            const char* e = getenv("SPLAT_WHITE_TEX");
            while (e && *e && nw < 16) { whites[nw++] = (u32)strtoul(e, (char**)&e, 16); if (*e == ',') e++; else break; }
            if (nw && make_image(&white2d, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_IMAGE_TYPE_2D, false, false, false)) {
                VkCommandBuffer cb = transfer_cmd();
                VkClearColorValue v;
                v.float32[0] = v.float32[1] = v.float32[2] = v.float32[3] = 1.0f;
                VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(cb, white2d.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
        }
        for (int i = 0; i < nw; i++)
            if ((base & ~0x7FFu) == (whites[i] & ~0x7FFu)) return sample_view(&white2d, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    }
    static int no_mips = -1;
    if (no_mips < 0) { no_mips = getenv("SPLAT_NO_MIPS") != NULL; g_no_mips = no_mips != 0; }
    u32 swizzle = 0;
    if (tile >= 4 && tile != 16) { swizzle = base & 0x700; base &= ~0x700u; }
    VkImageViewType vt = view_type(dim_shader);
    // slices: array layers, cube faces, or 3D depth
    u32 slices = 1;
    // (a cube map's depth field counts cubes: 1 = one cube, 2 = two cubes,
    // 12 faces -- GX2 writes depth / 6 - 1. Splatoon's environment maps are
    // two cubes, and the character shaders pick one of them per pixel
    // (SET_CUBEMAP_INDEX, latte_shader.c); taken as one cube, the second was
    // missing -- the purple sheen under the chin -- and mip offsets worked
    // out here were wrong.)
    if (dim == 3) slices = depth * 6;
    else if (dim == 2 || dim == 4 || dim == 5 || dim == 7) slices = depth;
    if (slices > 64) slices = 64;

    // Rendered by the game? Use the GPU copy -- unless that memory has been
    // reused since: a texture of another size or a compressed format there,
    // or the game loaded new data over it (games free buffers and load
    // textures into the same memory; the old buffer used to hide the
    // texture, e.g. the black reflection maps).
    // (several buffers can share an address: prefer the newest of the same size)
    Surf* newest = NULL;
    Surf* same = NULL;
    FOR_RT_AT(t, base)
        if (t->addr == (base & ~0x7FFu)) {
            if (!newest || t->seq > newest->seq) newest = t;
            if (t->w == width && t->h == height && (!same || t->seq > same->seq)) same = t;
        }
    static int rt_old = -1;
    if (rt_old < 0) rt_old = getenv("SPLAT_RT_OLD") != NULL;
    if (newest && !rt_old) {
        LatteTexFormat ntf;
        bool block = latte_tex_format(fmt, &ntf) && ntf.block;
        if (block) newest = NULL;
        else if (same) newest = same;
        else if (wiiu_mem_changed_since(base & ~0x7FFu, 0x1000, newest->gen)) newest = NULL;
    }
    for (Surf* s = newest; s; s = NULL) {
        g_tv_surf = s;
        bool cube_ok = s->layers >= 6 && s->layers % 6 == 0 && s->w == s->h;
        bool fits = s->vol ? vt == VK_IMAGE_VIEW_TYPE_3D
                           : vt == VK_IMAGE_VIEW_TYPE_2D || vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY ||
                             ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && cube_ok);
        if (!fits) {
            static int warned = 0;
            if (warned++ < 8) printf("[render] buffer %08X (%u slices) can't be read as texture kind %d\n", s->addr, s->layers, (int)vt);
            return VK_NULL_HANDLE;
        }
        if (s->depth) return sample_view(&s->img, vt, s->img.format, 0 | (1 << 3) | (2 << 6) | (3 << 9));
        VkFormat vf = s->img.format;
        LatteTexFormat tfg;
        if (g_small_target && g_small_w * 2 <= s->w && s->img.w > s->w && s->w == width && s->h == height && (w5 & 0xF) == 0) {   // (a glow / blur pass reading the scaled picture)
            Img* gc = rt_game_size(s);
            if (gc) {
                if (latte_tex_format(fmt, &tfg) && !tfg.block && !tfg.convert && tfg.elem_bytes * 8 == tfg.src_bits) {
                    LatteTexFormat rf;
                    if (latte_tex_format(s->gx2fmt, &rf) && rf.src_bits == tfg.src_bits && !rf.convert && !rf.block) vf = (VkFormat)tfg.vk_format;
                }
                g_tv_gsc = true;
                g_tvc[6]++;
                return sample_view(gc, vt, vf, sel);
            }
        }
        LatteTexFormat tf;
        if (latte_tex_format(fmt, &tf) && !tf.block && !tf.convert && tf.elem_bytes * 8 == tf.src_bits) {
            // same size class -> view in the texture's format (e.g. sRGB)
            LatteTexFormat rf;
            if (latte_tex_format(s->gx2fmt, &rf) && rf.src_bits == tf.src_bits && !rf.convert && !rf.block) vf = (VkFormat)tf.vk_format;
        }
        {   // mip levels drawn into their own buffers?
            u32 nlev = (w5 & 0xF) + 1, maxl = 1;
            for (u32 m = width > height ? width : height; m > 1; m >>= 1) maxl++;
            if (nlev > maxl) nlev = maxl;
            if (nlev > 14) nlev = 14;
            u32 maddr = w3 << 8;
            if (maddr && maddr != (w2 << 8) && !g_no_mips && nlev > 1) {
                Img* mc = rt_with_mips(s, dim, width, height, slices, nlev, hw, tile, swizzle, maddr);
                if (mc) return sample_view(mc, vt, vf, sel);
            }
        }
        return sample_view(&s->img, vt, vf, sel);
    }

    LatteTexFormat tf;
    if (!latte_tex_format(fmt, &tf)) {
        static int warned = 0;
        if (warned++ < 8) printf("[render] texture format %03X not supported yet\n", fmt);
        return VK_NULL_HANDLE;
    }
    if (tf.block && !g_vk.bc_textures) return VK_NULL_HANDLE;
    bool is3d = dim == 2, cube = dim == 3;
    LatteTexDesc d = { base, width, height, pitch, hw, tile, swizzle, slices, 0 };
    // mip levels: up to LAST_LEVEL, as many as the size allows
    u32 levels = (w5 & 0xF) + 1, maxl = 1;
    for (u32 m = width > height ? width : height; m > 1; m >>= 1) maxl++;
    if (levels > maxl) levels = maxl;
    if (levels > 14) levels = 14;
    u32 mip_addr = w3 << 8;
    if (!mip_addr || mip_addr == (w2 << 8) || g_no_mips) levels = 1;
    u64 key = hash_bytes(&d, sizeof(d), fmt ^ ((u64)dim << 40));
    if (levels > 1) key = hash_bytes(&mip_addr, 4, key ^ levels);
    Surf** bucket = &g.tex[key & 4095];
    Surf* s = *bucket;
    while (s && s->key != key) s = s->next;
    if (!s) {
        s = (Surf*)calloc(1, sizeof(Surf));
        s->key = key;
        s->addr = base;
        s->gx2fmt = fmt;
        s->w = width;
        s->h = height;
        s->layers = slices;
        s->span = latte_tex_span(&d, &tf);
        if (levels > 1) {
            GX2Layout L;
            gx2_surface_layout(dim, width, height, slices, levels, hw, 0, 1, tile, swizzle, &L);
            s->mip_addr = mip_addr;
            s->mip_span = L.mip_size;
        }
        g_next_levels = levels;
        u64 tc0 = pf_now();
        bool ok = is3d ? make_image(&s->img, (VkFormat)tf.vk_format, width, height, slices, VK_IMAGE_TYPE_3D, false, false, false)
                       : make_image(&s->img, (VkFormat)tf.vk_format, width, height, slices, VK_IMAGE_TYPE_2D, cube && width == height, false, false);
        g_tex_ns[1] += pf_now() - tc0;
        if (!ok) {
            free(s);
            return VK_NULL_HANDLE;
        }
        s->next = *bucket;
        *bucket = s;
        s->dirty = true;
        if (g.tex_count == g.tex_cap) {
            g.tex_cap = g.tex_cap ? g.tex_cap * 2 : 1024;
            g.tex_list = (Surf**)realloc(g.tex_list, g.tex_cap * sizeof(Surf*));
        }
        g.tex_list[g.tex_count++] = s;
        tex_index_add(s, s->addr, s->span);
        if (s->mip_span) tex_index_add(s, s->mip_addr & ~0x700u, s->mip_span);
    }
    if (g_cap_active) {
        cap_mem(base, s->span);
        if (s->mip_span) cap_mem(s->mip_addr & ~0x700u, s->mip_span);
    }
    // Games tell the GPU when they change a texture (GX2Invalidate); as a
    // safety net the data is also re-checked every 10 seconds.
    if (!s->dirty && s->gen_frame != g.frame) {   // the game flushed/loaded new data there?
        s->gen_frame = g.frame;
        if (wiiu_mem_changed_since(base, s->span, s->gen) ||
            (s->mip_span && wiiu_mem_changed_since(s->mip_addr & ~0x700u, s->mip_span, s->gen))) s->dirty = true;
    }
    if (!tf.block) g_tvs_tex = s;   // (the memo in texture_view may keep this across frames)
    // A compressed texture the game drew itself: it renders one 64/128-bit
    // "pixel" per 4x4 block into a buffer at the same address (the
    // "Girl"/"Boy" labels are made that way), then samples it as BC. Copy
    // those blocks into the texture on the graphics card.
    if (tf.block) {
        // every mip level can be drawn that way too (the light probes' cube maps:
        // one buffer per level); find the buffer for each level
        u32 nlev = s->img.levels ? s->img.levels : 1;
        if (nlev > 14) nlev = 14;
        u32 laddr[14];
        laddr[0] = base & ~0x7FFu;
        if (nlev > 1) {
            GX2Layout L;
            gx2_surface_layout(dim, width, height, slices, nlev, hw, 0, 1, tile, swizzle, &L);
            GX2MipInfo mi;
            u32 stop = 0xFF;
            if (gx2_lookup_mips(s->mip_addr & ~0xFFu, &mi)) {
                for (u32 i = 1; i < 13; i++) L.mip_offset[i] = mi.offs[i];
                stop = (mi.swizzle >> 16) & 0xFF;
            }
            bool l1_swz = stop != 0xFF ? stop > 1 : L.level[1].swizzled;
            u32 m = l1_swz ? s->mip_addr ^ (swizzle & 0x700) : s->mip_addr;
            for (u32 lv = 1; lv < nlev; lv++) laddr[lv] = (lv == 1 ? m : m + L.mip_offset[lv - 1]) & ~0x7FFu;
        }
        Surf* brts[14] = { 0 };
        u64 seqsum = 0;
        bool any = false;
        for (u32 lv = 0; lv < nlev; lv++) {
            u32 lw = width >> lv, lh = height >> lv;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            u32 bw = (lw + 3) / 4, bh = (lh + 3) / 4;
            if (lv > 0 && !brts[0]) break;   // (level 0 isn't drawn: a normal texture)
            FOR_RT_AT(t, laddr[lv])
                if (t->addr == laddr[lv] && !t->depth && t->w == bw && t->h == bh && texel_bytes(t->img.format) * 8 == tf.src_bits &&
                    (!brts[lv] || t->seq > brts[lv]->seq)) brts[lv] = t;
            if (brts[lv]) { seqsum += brts[lv]->seq * (lv + 1); any = true; }
        }
        // (level 0 not drawn by the game: an ordinary compressed texture -- most of the game's. The memo
        // in texture_view may keep it across frames like other plain textures: it looks again whether
        // anything is drawn at its address. Re-done every frame, ~770 lookups a frame in the busy plaza.)
        if (!brts[0]) g_tvs_tex = s;
        if (any && brts[0]) {
            if (s->block_rt_seq != seqsum) {
                s->block_rt_seq = seqsum;
                u32 offs[14], bytes = 0;
                for (u32 lv = 0; lv < nlev; lv++) {
                    offs[lv] = bytes;
                    if (!brts[lv]) continue;
                    u32 layers = brts[lv]->layers < slices ? brts[lv]->layers : slices;
                    if (!layers) layers = 1;
                    bytes += (brts[lv]->w * brts[lv]->h * texel_bytes(brts[lv]->img.format) * layers + 255) & ~255u;
                }
                static VkBuffer bbuf;
                static VkDeviceMemory bmem;
                static void* bptr;
                static u32 bsize;
                if (bytes > bsize) {
                    if (bbuf) { buffer_destroy_later(bbuf, bmem); bbuf = VK_NULL_HANDLE; }   // (this frame's command buffer may hold copies into it)
                    bsize = bytes > (1u << 20) ? bytes : (1u << 20);
                    if (!host_buffer(bsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &bbuf, &bmem, &bptr)) { bsize = 0; bbuf = VK_NULL_HANDLE; }
                }
                if (bbuf) {
                    VkCommandBuffer cb = transfer_cmd();
                    barrier_all(cb);
                    u32 nl = 0;
                    for (u32 lv = 0; lv < nlev; lv++) {
                        if (!brts[lv]) continue;
                        u32 layers = brts[lv]->layers < slices ? brts[lv]->layers : slices;
                        if (!layers) layers = 1;
                        VkBufferImageCopy c;
                        memset(&c, 0, sizeof(c));
                        c.bufferOffset = offs[lv];
                        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        c.imageSubresource.layerCount = layers;
                        c.imageExtent.width = brts[lv]->w;
                        c.imageExtent.height = brts[lv]->h;
                        c.imageExtent.depth = 1;
                        vkCmdCopyImageToBuffer(cb, brts[lv]->img.image, VK_IMAGE_LAYOUT_GENERAL, bbuf, 1, &c);
                        nl++;
                    }
                    VkBufferMemoryBarrier mb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    mb.buffer = bbuf;
                    mb.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &mb, 0, NULL);
                    for (u32 lv = 0; lv < nlev; lv++) {
                        if (!brts[lv]) continue;
                        u32 layers = brts[lv]->layers < slices ? brts[lv]->layers : slices;
                        if (!layers) layers = 1;
                        u32 lw = s->img.w >> lv, lh = s->img.h >> lv;   // (render scale: the image's sizes, not the game's)
                        VkBufferImageCopy c;
                        memset(&c, 0, sizeof(c));
                        c.bufferOffset = offs[lv];
                        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        c.imageSubresource.mipLevel = lv;
                        c.imageSubresource.layerCount = layers;
                        c.imageExtent.width = lw ? lw : 1;
                        c.imageExtent.height = lh ? lh : 1;
                        c.imageExtent.depth = 1;
                        vkCmdCopyBufferToImage(cb, bbuf, s->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                    }
                    static int logged = 0;
                    if (logged++ < 8) printf("[render] texture %08X %ux%u: made by the game on the GPU (compressed blocks, %u of %u levels), copied over\n", base, width, height, nl, nlev);
                }
            }
            s->dirty = false;
            s->checked_frame = g.frame;
            s->gen = wiiu_mem_generation();
            s->gen_frame = g.frame;
            if (!view_fits(dim, slices, cube && width == height, vt)) return VK_NULL_HANDLE;
            return sample_view(&s->img, vt, (VkFormat)tf.vk_format, sel);
        }
    }
    // (every 10 s: hashing every texture each 1/2 s was ~1 ms a frame in busy
    // scenes. At most a few per frame: a scene's textures all came due in the
    // same frame, 15-130 ms of hashing = a hiccup every 10 s in levels.)
    static u64 recheck_frame;
    static u32 rechecks;
    if (recheck_frame != g.frame) { recheck_frame = g.frame; rechecks = 0; }
    if (s->dirty || (g.frame - s->checked_frame >= 600 && rechecks < 4)) {
        if (!s->dirty) rechecks++;
        s->checked_frame = g.frame;
        u32 gen_now = wiiu_mem_generation();
        bool streaming = s->dirty && s->dirty_streak >= 2 && (g.frame - s->checked_frame) < 600;   // (rewritten every time it was looked at: don't hash, just upload)
        u64 h = 0;
        if (!streaming) {
            u64 th0 = pf_now();
            h = mem_hash(base, s->span);
            g_tex_ns[0] += pf_now() - th0;
        }
        if (streaming || h != s->hash || s->dirty) {
            if (streaming || h != s->hash) { if (s->dirty_streak < 255) s->dirty_streak++; } else s->dirty_streak = 0;
            s->hash = streaming ? 0 : h;
            upload_texture(s, &d, &tf, is3d, dim);
        } else s->dirty_streak = 0;
        s->dirty = false;
        s->gen = gen_now;
    }
    if (!view_fits(dim, slices, cube && width == height, vt)) {
        static int warned = 0;
        if (warned++ < 8) printf("[render] texture %08X (kind %u) can't be read as texture kind %d\n", base, dim, (int)vt);
        return VK_NULL_HANDLE;
    }
    return sample_view(&s->img, vt, (VkFormat)tf.vk_format, sel);
}

// Within a frame the game binds the same few hundred textures thousands of
// times (~1,000 draws, several textures each); what texture_view_slow works
// out for one -- which drawn buffer or loaded texture, its view -- holds
// until the frame ends or something that decides it changes: a texture
// marked as changed (GX2Invalidate) or a buffer drawn into at an address
// another buffer shares (g_tv_epoch). (Not while capturing or debugging.)
typedef struct { u32 w[6]; u32 dim; u32 epoch, vepoch; u64 frame, vframe; VkImageView v; Surf* tv; Surf* tex; } TvMemo;
static TvMemo g_tv_memo[8192];
// (the plain texture the slow path settled on -- not a drawn buffer, not a block-compressed one the game
// draws itself: a memo entry for it stays good across frames while its memory is unchanged and it isn't
// marked dirty, for up to 64 frames; then the slow path runs again, with its periodic re-check)


static bool g_dbg;
static VkImageView texture_view(u32 res_reg, u32 dim_shader) {
    if (g_cap_active || g_dbg) return texture_view_slow(res_reg, dim_shader);
    u32 w[6];
    for (u32 i = 0; i < 6; i++) w[i] = R(res_reg + i);
    u32 h = dim_shader * 0x27D4EB2Fu;
    for (u32 i = 0; i < 6; i++) h = (h ^ w[i]) * 0x9E3779B1u;
    TvMemo* m = &g_tv_memo[(h ^ (h >> 16)) & 8191];
    bool same_entry = m->dim == dim_shader && !memcmp(m->w, w, sizeof(w));
    if (same_entry) {
        bool good = m->frame == g.frame && m->epoch == g_tv_epoch;
        if (!good && m->tex && !m->tv && m->vframe == g.frame && m->vepoch == g_tv_epoch) good = true;   // (checked below already this frame)
        else if (!good && m->tex && !m->tv && g.frame < m->frame + 64) {   // (a plain texture: still the same data, and nothing drawn at its address?)
            Surf* t = m->tex;
            bool drawn = false;
            FOR_RT_AT(r, t->addr) if (r->addr == (t->addr & ~0x7FFu)) { drawn = true; break; }
            good = !drawn && !t->dirty && !wiiu_mem_changed_since(t->addr, t->span, t->gen) &&
                   (!t->mip_span || !wiiu_mem_changed_since(t->mip_addr & ~0x700u, t->mip_span, t->gen));
            if (good) { m->vframe = g.frame; m->vepoch = g_tv_epoch; }
        }
        if (good && g_small_target && m->tv && !m->tv->depth && m->tv->img.w > m->tv->w && g_small_w * 2 <= m->tv->w) good = false;   // (a glow pass may read a game-size copy: rt_game_size)
        if (good) {
            if (m->frame != g.frame || m->epoch != g_tv_epoch) g_tvc[4]++;
            g_tv_surf = m->tv;
            g_ss.memo_hits++;
            return m->v;
        }
    }
    g_tvc[!same_entry ? 0 : m->tv ? 1 : 2]++;
    g_tvs_tex = NULL;
    g_tv_gsc = false;
    VkImageView v = texture_view_slow(res_reg, dim_shader);
    if (g_tv_gsc) return v;   // (the game-size copy: only for this kind of pass, not remembered)
    memcpy(m->w, w, sizeof(w));
    m->dim = dim_shader;
    m->epoch = g_tv_epoch;
    m->frame = g.frame;
    m->vframe = 0;
    m->v = v;
    m->tv = g_tv_surf;
    m->tex = g_tv_surf ? NULL : g_tvs_tex;
    return v;
}

// --- Samplers -------------------------------------------------------------------------------------

// (g_dbg: one frame where every draw is logged and all buffers are saved -- declared above texture_view)
#define DBG(...) do { if (g_dbg) printf("[dbg] " __VA_ARGS__); } while (0)

typedef struct SampEnt { u32 w0, w1, w2; int cmp; VkSampler s; struct SampEnt* next; } SampEnt;
// (by a hash of the three words: one list of every sampler made, searched
// on each texture of each draw, was ~9% of the drawing thread in the plaza)
#define SAMP_BUCKETS 1024
static SampEnt* g_samplers[SAMP_BUCKETS];
static u32 samp_bucket(u32 w0, u32 w1, u32 w2, int cmp) {
    u32 h = (w0 * 0x9E3779B1u) ^ (w1 * 0x85EBCA77u) ^ (w2 * 0xC2B2AE3Du) ^ ((u32)cmp * 0x27D4EB2Fu);
    return (h ^ (h >> 15)) & (SAMP_BUCKETS - 1);
}

static VkSamplerAddressMode clamp_mode(u32 c) {
    switch (c) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: case 5: case 7: return g_vk.mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

static VkSampler sampler_for(u32 index, bool is_cmp) {
    u32 w0 = R(REG_BASE_SAMPLER + index * 3), w1 = R(REG_BASE_SAMPLER + index * 3 + 1), w2 = R(REG_BASE_SAMPLER + index * 3 + 2);
    int cmp = is_cmp ? (shadow_mode() == 2 ? 2 : 1) : 0;   // (2: "Shadows: sharp" also filters the comparisons)
    u32 bk = samp_bucket(w0, w1, w2, cmp);
    for (SampEnt* e = g_samplers[bk]; e; e = e->next)
        if (e->w0 == w0 && e->w1 == w1 && e->w2 == w2 && e->cmp == cmp) return e->s;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.addressModeU = clamp_mode(w0 & 7);
    ci.addressModeV = clamp_mode((w0 >> 3) & 7);
    ci.addressModeW = clamp_mode((w0 >> 6) & 7);
    u32 mag = (w0 >> 9) & 7, min = (w0 >> 12) & 7, mip = (w0 >> 17) & 3, aniso = (w0 >> 19) & 7;
    ci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = (float)(w1 & 0x3FF) / 64.0f;
    ci.maxLod = mip == 0 ? 0.25f : (float)((w1 >> 10) & 0x3FF) / 64.0f;
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    int bias = (int)((w1 >> 20) & 0xFFF);
    if (bias & 0x800) bias -= 0x1000;
    ci.mipLodBias = (float)bias / 64.0f;
    if (aniso && g_vk.aniso && ((mag & 4) || (min & 4))) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)(1u << aniso);
        if (ci.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy) ci.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    }
    u32 border = (w0 >> 22) & 3;
    ci.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (cmp) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = (VkCompareOp)((w0 >> 26) & 7);
        // "Shadows: sharp": the comparison filtered over the 2x2 nearest map texels (the card's own soft
        // edge, free) where the game asked for the nearest one only -- no stair steps along shadow edges
        static int pcf = -1;
        if (pcf < 0) { const char* e = getenv("SPLAT_SHADOW_PCF"); pcf = e ? atoi(e) : 1; }
        if (cmp == 2 && pcf) ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        static int logged;
        if (logged < 8) { logged++; printf("[shadow] compare sampler %08X %08X %08X: game filter mag %u min %u%s\n", w0, w1, w2, mag, min, cmp == 2 && pcf ? " -> filtered (sharp shadows)" : ""); }
    }
    VkSampler s = VK_NULL_HANDLE;
    u64 st0 = pf_now();
    VkResult sr = vkCreateSampler(g_vk.device, &ci, NULL, &s);
    g_tex_ns2[1] += pf_now() - st0;
    if (sr != VK_SUCCESS) return g.default_sampler;
    DBG("  new sampler %08X %08X %08X: wrap %u/%u filter %u/%u mip %u lod %.2f..%.2f bias %.2f aniso %u\n", w0, w1, w2, w0 & 7, (w0 >> 3) & 7,
        mag, min, mip, ci.minLod, ci.maxLod, ci.mipLodBias, aniso);
    SampEnt* e = (SampEnt*)calloc(1, sizeof(SampEnt));
    e->w0 = w0; e->w1 = w1; e->w2 = w2; e->cmp = cmp; e->s = s;
    e->next = g_samplers[bk];
    g_samplers[bk] = e;
    return s;
}

// --- Shaders ----------------------------------------------------------------------------------------

typedef struct { u32 addr, size, gen; u64 quick, full; } ProgHash;
static ProgHash g_prog_hash[4096];

// (asked 2-3 times per draw: the full hash only when the program's memory
// pages were written since, or its first/last 64 bytes differ -- hashing
// 1 KB per program per draw was a good part of the drawing thread's time)
static u64 program_hash(u32 addr, u32 size) {
    const u8* p = wiiu_mem_ptr(addr, size);
    if (!p || !size) return 0;
    u32 n = size < 64 ? size : 64;
    u64 quick = hash_bytes(p, n, size);
    quick = hash_bytes(p + size - n, n, quick);
    ProgHash* e = &g_prog_hash[(addr >> 8) & 4095];
    if (e->addr == addr && e->size == size && e->quick == quick && !wiiu_mem_changed_since(addr, size, e->gen)) return e->full;
    e->addr = addr;
    e->size = size;
    e->quick = quick;
    e->gen = wiiu_mem_generation();
    e->full = hash_bytes(p, size, 0x84222325CBF29CE4ull ^ size);
    return e->full;
}

// Translated shaders are kept on disk (shader_cache.bin), so the next run
// doesn't translate and compile them again: one record per shader (key,
// what the shader reads, SPIR-V). Bump SHADER_CACHE_VERSION whenever the
// record layout changes. When only some shaders translate differently, the
// records instead carry the translator revision they match (DISK_REV_*, in
// their `fragment` byte): older ones are compared with today's translation
// when first used, and only the shaders that changed are built again (see
// latte_shader_changed_since_cache) -- not the whole cache.
#define SHADER_CACHE_VERSION 0x53430007u
#define DISK_REV1 2u   // abs/neg on integer operands
#define DISK_REV2 4u   // cube-map array index (v0.19)
#define DISK_REV_ALL (DISK_REV1 | DISK_REV2)
// Does a record of this revision need building again, given what today's
// translation of it uses?
static bool disk_stale(u8 rev, u32 changed) {
    return ((changed & 1) && !(rev & DISK_REV1)) || ((changed & 2) && !(rev & DISK_REV2));
}
// (the SPIR-V stays in the file -- off: where it starts -- and is read when the
// shader is first used: the whole cache, 2+ GB, used to be read into memory)
typedef struct DiskShader { u64 key; u8 fragment; LatteShaderOut info; u64 off; u32 words; struct DiskShader* next; } DiskShader;
static DiskShader* g_disk_shaders[4096];
static FILE* g_shader_cache_out;

// The caches live in SplatRecompRuntime\cache\ (they used to sit in
// SplatRecompRuntime itself: moved over the first time, same drive, so
// it's instant). CACHE("x.bin") = "cache/x.bin".
#define CACHE(name) "cache/" name
static void cache_dir_setup(void) {
    static bool done = false;
    if (done) return;
    done = true;
#ifdef _WIN32
    CreateDirectoryA("cache", NULL);
    static const char* const names[] = { "shader_cache.bin", "shader_cache_ok.bin", "shader_cache_ok2.bin", "pipeline_cache.bin", "pipeline_keys.bin" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char to[64];
        snprintf(to, sizeof(to), "cache/%s", names[i]);
        if (GetFileAttributesA(names[i]) != INVALID_FILE_ATTRIBUTES && GetFileAttributesA(to) == INVALID_FILE_ATTRIBUTES &&
            MoveFileA(names[i], to))
            printf("[render] moved %s into the cache folder\n", names[i]);
    }
#else
    mkdir("cache", 0755);
#endif
}

static void shader_cache_load(void) {
    cache_dir_setup();
    static bool done = false;
    if (done) return;
    done = true;
    FILE* f = fopen(CACHE("shader_cache.bin"), "rb");
    u32 ver = 0, count = 0;
    if (f && fread(&ver, 4, 1, f) == 1 && ver == SHADER_CACHE_VERSION) {
        _fseeki64(f, 0, SEEK_END);
        u64 fsize = (u64)_ftelli64(f);
        _fseeki64(f, 4, SEEK_SET);
        for (;;) {
            DiskShader e;
            memset(&e, 0, sizeof(e));
            if (fread(&e.key, 8, 1, f) != 1 || fread(&e.fragment, 1, 1, f) != 1 || fread(&e.info, sizeof(e.info), 1, f) != 1 ||
                fread(&e.words, 4, 1, f) != 1 || e.words == 0 || e.words > (16u << 20))
                break;
            e.off = (u64)_ftelli64(f);
            if (e.off + (u64)e.words * 4 > fsize || _fseeki64(f, (s64)e.words * 4, SEEK_CUR) != 0) break;   // (a cut-off last record)
            e.info.glsl = NULL;
            DiskShader* n = (DiskShader*)malloc(sizeof(DiskShader));
            *n = e;
            u32 b = (u32)((e.key ^ (e.fragment & 1)) & 4095);
            n->next = g_disk_shaders[b];
            g_disk_shaders[b] = n;
            count++;
        }
        fclose(f);
        f = NULL;
        g_shader_cache_out = fopen(CACHE("shader_cache.bin"), "ab");
    } else {
        if (f) fclose(f);
        remove(CACHE("shader_cache_ok.bin"));
        remove(CACHE("shader_cache_ok2.bin"));
        g_shader_cache_out = fopen(CACHE("shader_cache.bin"), "wb");
        if (g_shader_cache_out) { u32 v = SHADER_CACHE_VERSION; fwrite(&v, 4, 1, g_shader_cache_out); }
    }
    if (count) {   // the older ones already checked (see shader_cache_note_ok)
        // (shader_cache_ok.bin: checked against revision 1; _ok2: against revision 2)
        u32 marked = 0;
        for (int file = 0; file < 2; file++) {
            FILE* okf = fopen(file ? CACHE("shader_cache_ok2.bin") : CACHE("shader_cache_ok.bin"), "rb");
            if (!okf) continue;
            u8 rev = file ? DISK_REV_ALL : DISK_REV1;
            u64 k;
            u8 fr;
            while (fread(&k, 8, 1, okf) == 1 && fread(&fr, 1, 1, okf) == 1)
                for (DiskShader* d = g_disk_shaders[(k ^ (fr & 1)) & 4095]; d; d = d->next)
                    if (d->key == k && (d->fragment & 1) == (fr & 1) && (d->fragment & rev) != rev) {
                        d->fragment |= rev;
                        if (file) marked++;
                    }
            fclose(okf);
        }
        printf("[render] %u shaders loaded from shader_cache.bin (%u checked earlier)\n", count, marked);
    }
}

// A cached shader's SPIR-V, read from the file now (malloc'd; NULL if it can't be).
// (Only the drawing thread asks, so one read handle does.)
static u32* disk_spv(const DiskShader* d) {
    static FILE* in;
    if (!in) in = fopen(CACHE("shader_cache.bin"), "rb");
    if (!in) return NULL;
    u32* spv = (u32*)malloc((size_t)d->words * 4);
    if (!spv) return NULL;
    if (_fseeki64(in, (s64)d->off, SEEK_SET) != 0 || fread(spv, 4, d->words, in) != d->words) { free(spv); return NULL; }
    return spv;
}

// Shaders from an older translator that were checked and found unchanged
// (shader_cache_ok2.bin: key + stage, 9 bytes each).
static FILE* g_shader_ok_out;
static u64 g_recheck_ns, g_recheck_count;   // (time spent re-checking shaders from an older translator)
static u64 g_sd_t[3], g_sd_start, g_sd_rc, g_sd_tr, g_sd_translated;   // (the draw being set up: see "slow draw setup")
static u64 g_sd_sum[4];   // (per-frame sums of the above + the pipeline lookup: the periodic [perf] line)
static u32 g_sd_logged;
static void shader_cache_note_ok(u64 key, bool fragment) {
    if (!g_shader_ok_out) g_shader_ok_out = fopen(CACHE("shader_cache_ok2.bin"), "ab");
    if (!g_shader_ok_out) return;
    u8 fr = fragment ? 1 : 0;
    fwrite(&key, 8, 1, g_shader_ok_out);
    fwrite(&fr, 1, 1, g_shader_ok_out);
    fflush(g_shader_ok_out);
}

static void shader_cache_save(u64 key, bool fragment, const LatteShaderOut* info, const u32* spv, size_t words) {
    if (!g_shader_cache_out) return;
    LatteShaderOut o = *info;
    o.glsl = NULL;
    u8 fr = (fragment ? 1 : 0) | DISK_REV_ALL;   // (written by this translator revision)
    u32 w = (u32)words;
    fwrite(&key, 8, 1, g_shader_cache_out);
    fwrite(&fr, 1, 1, g_shader_cache_out);
    fwrite(&o, sizeof(o), 1, g_shader_cache_out);
    fwrite(&w, 4, 1, g_shader_cache_out);
    fwrite(spv, 4, words, g_shader_cache_out);
    fflush(g_shader_cache_out);
}

static void shader_translate(ShaderEnt* e, bool fragment, u64 key, const LatteShaderKey* k, bool sub_this, const char* sub_spec);

// A shader the disk cache had from an older translator, preloaded before the
// game showed it: now that the game uses it, compare it with what the
// translator makes today, and build it again if that differs.
static void shader_recheck(ShaderEnt* e, bool fragment, u64 key, const LatteShaderKey* k) {
    e->unchecked = false;
    u64 t0 = hnow_ns();
    LatteShaderOut t;
    memset(&t, 0, sizeof(t));
    bool ok = fragment ? latte_decompile_ps(k, &t) : latte_decompile_vs(k, &t);
    free(t.glsl);
    g_recheck_ns += hnow_ns() - t0;
    g_recheck_count++;
    if (!ok || !disk_stale(e->disk_rev, latte_shader_changed_since_cache())) { shader_cache_note_ok(key, fragment); return; }
    static int logged = 0;
    if (logged++ < 8) printf("[render] shader %016llx: newer translation than the disk cache, rebuilding it\n", (unsigned long long)key);
    // (the old module stays alive: pipelines made with it may still be in use)
    e->mod = VK_NULL_HANDLE;
    e->failed = false;
    memset(&e->info, 0, sizeof(e->info));
    shader_translate(e, fragment, key, k, false, NULL);
}

static ShaderEnt* get_shader(bool fragment, u64 key, const LatteShaderKey* k) {
    ShaderEnt** bucket = &g.shaders[key & (LR_BUCKETS - 1)];
    for (ShaderEnt* e = *bucket; e; e = e->next)
        if (e->key == key) {
            if (e->unchecked && k && !hatomic_load64(&e->pending)) shader_recheck(e, fragment, key, k);
            return e;
        }
    ShaderEnt* e = (ShaderEnt*)calloc(1, sizeof(ShaderEnt));
    e->key = key;
    e->next = *bucket;
    *bucket = e;
    shader_cache_load();
    // SPLAT_GLSL_SUB=key|from|to[|from|to...] (debugging): edit that shader's
    // GLSL text before it is compiled (the disk cache is skipped for it)
    // (several shaders: specs separated by "@@")
    const char* sub = getenv("SPLAT_GLSL_SUB");
    bool sub_this = false;
    char sub_spec[4096];
    for (const char* p = sub; p && *p && !sub_this;) {
        const char* end = strstr(p, "@@");
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n < sizeof(sub_spec) && strtoull(p, NULL, 16) == key) {
            memcpy(sub_spec, p, n);
            sub_spec[n] = 0;
            sub_this = true;
        }
        p = end ? end + 2 : NULL;
    }
    for (DiskShader* d = sub_this ? NULL : g_disk_shaders[(key ^ (fragment ? 1 : 0)) & 4095]; d; d = d->next) {
        if (d->key != key || (d->fragment & 1) != (fragment ? 1 : 0)) continue;
        bool checked = (d->fragment & DISK_REV_ALL) == DISK_REV_ALL;
        if (!checked && !k) { e->unchecked = true; e->disk_rev = d->fragment & DISK_REV_ALL; }   // (preloading: compared once the game uses it)
        else if (!checked) {   // (from an older translator) translated differently now? then again
            u64 t0 = hnow_ns();
            LatteShaderOut t;
            bool ok = fragment ? latte_decompile_ps(k, &t) : latte_decompile_vs(k, &t);
            free(t.glsl);
            g_recheck_ns += hnow_ns() - t0;
            g_recheck_count++;
            if (ok && disk_stale(d->fragment, latte_shader_changed_since_cache())) {
                static int logged = 0;
                if (logged++ < 4) printf("[render] shader %016llx: newer translation than the disk cache, rebuilding it\n", (unsigned long long)key);
                break;
            }
            // the same as before: keep it, noted as checked (so it can be preloaded again)
            shader_cache_note_ok(key, fragment);
            d->fragment |= DISK_REV_ALL;
        }
        e->info = d->info;
        u64 dt0 = hnow_ns();
        u32* spv = disk_spv(d);
        u64 dt1 = hnow_ns();
        shader_note_ub(e, spv, d->words);
        u64 dt2 = hnow_ns();
        if (spv && async_on() && !getenv("SPLAT_SYNC_MODULES")) {
            // The graphics driver takes its time over a module too (AMD: 10-14 ms
            // for a 70 KB shader -- each new weapon in the gear menu brought two,
            // on the drawing thread): made on a helper thread, like new shaders.
            e->disk_in = spv;
            e->disk_words = d->words;
            e->pending = 1;
            async_queue(e, NULL);
            return e;
        }
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = (size_t)d->words * 4;
        ci.pCode = spv;
        if (!spv || vkCreateShaderModule(g_vk.device, &ci, NULL, &e->mod) != VK_SUCCESS) e->failed = true;
        else modreg_put(e->mod, key, d->words);
        free(spv);
        {
            u64 dt3 = hnow_ns();
            static int shown = 0;
            if (dt3 - dt0 > 3000000ull && shown++ < 30)
                printf("[render] cached shader %016llx (%u words) slow to load: read %.1f ms, uniform notes %.1f ms, module %.1f ms\n",
                       (unsigned long long)key, d->words, (dt1 - dt0) / 1e6, (dt2 - dt1) / 1e6, (dt3 - dt2) / 1e6);
        }
        return e;
    }
    if (!k) { e->failed = true; return e; }
    {   // SPLAT_SHADER_LOG=1: what makes this shader new (the Oct 31 bench translated 14-17 "new" vertex shaders per run in the same sweep)
        static int slog = -1;
        if (slog < 0) slog = getenv("SPLAT_SHADER_LOG") != NULL;
        if (slog) {
            char sem[65], out[81];
            for (int i = 0; i < 32; i++) snprintf(sem + 2 * i, 3, "%02X", k->vtx_semantic[i]);
            for (int i = 0; i < 40; i++) snprintf(out + 2 * i, 3, "%02X", k->vs_out_semantic[i]);
            u32 nin = k->ps_in_control0 & 0x3F;
            printf("[shader] new %s shader %016llx: program %016llx (%u bytes), fetch %016llx (%u bytes), vtx_semantic %s, vs_out %s, vte %08X clip %08X points %d, ps_in_control0 %08X (%u inputs, hash %016llx) control1 %08X, cb_mask %08X alpha %08X\n",
                   fragment ? "pixel" : "vertex", (unsigned long long)key,
                   (unsigned long long)(fragment ? (k->ps ? hash_bytes(k->ps, k->ps_size, 0) : 0) : (k->vs ? hash_bytes(k->vs, k->vs_size, 0) : 0)), fragment ? k->ps_size : k->vs_size,
                   (unsigned long long)(k->fs ? hash_bytes(k->fs, k->fs_size, 0) : 0), k->fs_size, sem, out, k->vte_cntl, k->clip_cntl, k->points,
                   k->ps_in_control0, nin, (unsigned long long)hash_bytes(k->ps_input_cntl, 4 * nin, 0), k->ps_in_control1, k->cb_shader_mask, k->alpha_test);
            if (!fragment && k->fs && getenv("SPLAT_SHADER_LOG_FS")) {   // (the fetch shader's bytes: what differs between variants)
                printf("[shader]   fetch bytes:");
                for (u32 i = 0; i < k->fs_size && i < 256; i++) printf("%s%02X", i % 16 ? "" : " ", k->fs[i]);
                printf("\n");
            }
        }
    }
    shader_translate(e, fragment, key, k, sub_this, sub_spec);
    return e;
}

static void shader_translate(ShaderEnt* e, bool fragment, u64 key, const LatteShaderKey* k, bool sub_this, const char* sub_spec) {
    g_sd_translated++;
    bool ok = fragment ? latte_decompile_ps(k, &e->info) : latte_decompile_vs(k, &e->info);
    if (!ok) { e->failed = true; return; }
    if (sub_this && e->info.glsl) {
        char* spec = strdup(sub_spec);
        char* parts[17];
        int np = 0;
        for (char* t = strtok(spec, "|"); t && np < 17; t = strtok(NULL, "|")) parts[np++] = t;
        for (int i = 1; i + 1 < np; i += 2) {
            char* at = strstr(e->info.glsl, parts[i]);
            if (!at) { printf("[render] SPLAT_GLSL_SUB: '%s' not found\n", parts[i]); continue; }
            size_t lf = strlen(parts[i]), lt = strlen(parts[i + 1]), ln = strlen(e->info.glsl);
            char* g2 = (char*)malloc(ln - lf + lt + 1);
            size_t pre = (size_t)(at - e->info.glsl);
            memcpy(g2, e->info.glsl, pre);
            memcpy(g2 + pre, parts[i + 1], lt);
            strcpy(g2 + pre + lt, at + lf);
            free(e->info.glsl);
            e->info.glsl = g2;
            printf("[render] SPLAT_GLSL_SUB: edited shader %016llx\n", (unsigned long long)key);
        }
        free(spec);
        if (getenv("SPLAT_GLSL_SUB_PRINT")) printf("%s\n", e->info.glsl);
    }
    static u32 count = 0;
    count++;
    if (count <= 4 || count % 100 == 0) printf("[render] %u shaders translated so far\n", count);
    if (async_on()) {   // compiled on a helper thread; saved to the disk cache when done
        {
            static int saved = 0;
            if ((debug_auto() || getenv("SPLAT_GLSL_DUMP")) && saved++ < 20000) {
                char sp[64];
                snprintf(sp, sizeof(sp), "%s/debug_shaders.txt", debug_dir());
                FILE* f = fopen(sp, saved == 1 ? "w" : "a");
                if (f) { fprintf(f, "\n==== %s %016llx ====\n%s", fragment ? "PS" : "VS", (unsigned long long)key, e->info.glsl); fclose(f); }
            }
        }
        e->fragment = fragment;
        e->glsl = e->info.glsl;
        e->info.glsl = NULL;
        e->pending = 1;
        async_queue(e, NULL);
        return;
    }
    size_t words = 0;
    u32* spv = glsl_to_spirv(fragment, e->info.glsl, &words);
    shader_note_ub(e, spv, words);
    {   // keep the first shaders for debugging (debug_output/debug_shaders.txt)
        static int saved = 0;
        if ((debug_auto() || getenv("SPLAT_GLSL_DUMP")) && saved++ < 20000) {
            char sp[64];
            snprintf(sp, sizeof(sp), "%s/debug_shaders.txt", debug_dir());
            FILE* f = fopen(sp, saved == 1 ? "w" : "a");
            if (f) { fprintf(f, "\n==== %s %016llx%s ====\n%s", fragment ? "PS" : "VS", (unsigned long long)key, spv ? "" : " (FAILED)", e->info.glsl); fclose(f); }
        }
    }
    free(e->info.glsl);
    e->info.glsl = NULL;
    if (!spv) { e->failed = true; return; }
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = words * 4;
    ci.pCode = spv;
    if (vkCreateShaderModule(g_vk.device, &ci, NULL, &e->mod) != VK_SUCCESS) e->failed = true;
    else shader_cache_save(key, fragment, &e->info, spv, words);
    free(spv);
}

// Ready to draw with? (Saves shaders the helper threads finished.)
// Each frame may wait this long in total for shaders still being built
// (the drawing thread mostly waits for the game anyway): fewer draws are
// skipped, so less pops in and flickers while shaders are built.
// Never stop the game for a build (the default since Oct 20): a draw whose
// shader or pipeline a helper thread is still building is left out for as
// long as that takes (the frame is held, see g_frame_skipped), instead of
// being built on the drawing thread after 2 frames. Pipelines a cold driver
// compiles took 100-650 ms each there: the user's 15-minute session froze 63
// times for 18 s, and the game's music ran dry each time. Only draws whose
// results the game reads back, draws into buffers made once and small
// full-screen passes still wait. SPLAT_OLD_BUILDS=1: the old way.
static bool never_stop(void) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_OLD_BUILDS") == NULL;
    return on == 1;
}
#define BUILD_WAIT_NS 16000000ull
#define BUILD_WAIT_NS_NEW 1000000ull   // (a frame may still wait a moment for a build that is nearly done)
static u64 g_build_wait_used;
static u64 g_frame_start_ns;   // (the drawing thread's frame: the moment the previous swap was done)
// (settings "New shaders": wait = a frame may spend time on builds before it leaves a draw out -- a short hitch
// instead of things blinking in and out; skip = the old 1 ms. Oct 31: the waiting ends at a point in the frame --
// SPLAT_BUILD_WAIT_MS, 11 ms after the previous swap -- instead of after 30 ms of waiting: 14-30 ms waits for
// helpers' builds were the all-maps bench's last hitches. What isn't ready by then appears a frame late.)
static u64 build_wait_limit_ns(void) {
    static u64 v;
    if (!v) { const char* e = getenv("SPLAT_BUILD_WAIT_MS"); v = (e && atoi(e) > 0 ? (u64)atoi(e) : 11) * 1000000ull; }
    return v;
}
static bool wait_built(volatile u64* pending) {
    u64 budget = never_stop() ? (g_vk.build_wait ? 30000000ull : BUILD_WAIT_NS_NEW) : BUILD_WAIT_NS;
    if (never_stop() && g_vk.build_wait && g_frame_start_ns) {
        u64 el = hnow_ns() - g_frame_start_ns, lim = build_wait_limit_ns();
        if (el >= lim) return false;
        budget = lim - el;   // (what is left of this frame's time; the waits already spent are inside el)
        g_build_wait_used = 0;
    }
    if (g_build_wait_used >= budget) return false;
    u64 t0 = hnow_ns();
    wait_mark("a build, briefly (wait_built)", pending);
    while (hatomic_load64(pending)) {
        u64 dt = hnow_ns() - t0;
        if (g_build_wait_used + dt >= budget) { g_build_wait_used += dt; wait_unmark(); return false; }
        hsleep_ns(50000);
    }
    wait_unmark();
    g_build_wait_used += hnow_ns() - t0;
    return true;
}
// A shader or pipeline still being built may make its draws go missing for
// at most this many frames; after that the frame waits for it. (Things
// blinking in and out while new graphics are built was worse than a short
// wait.) SPLAT_MAX_SKIP_FRAMES changes it (0: always wait -- then every new
// shader and pipeline is built one after another on the drawing thread:
// a new level froze for 10 s).
// A frame that had to leave a draw out isn't shown (see g_frame_skipped):
// the last complete picture stays up a moment instead of things blinking.
static int max_skip(void) {
    static int max = -1;
    if (max < 0) { const char* e = getenv("SPLAT_MAX_SKIP_FRAMES"); max = e ? atoi(e) : 2; if (max < 0) max = 0; if (max > 250) max = 250; }
    return max;
}
static bool may_skip(u64* last, u8* n) {
    if (*last == g.frame + 1) return true;   // (already skipped in this frame)
    if (never_stop()) { if (*n < 255) (*n)++; *last = g.frame + 1; return true; }
    if (*n >= max_skip()) return false;
    (*n)++;
    *last = g.frame + 1;
    return true;
}
// (what the drawing thread waited for in shader_ready -- the [spike] "shaders ready" line:
// [0] a cached shader's module, made here, [1] the same, waited for a helper, [2] a new shader
// compiled here, [3] waited for a helper's compile; g_sw_save: writing new ones to the disk cache)
static u32 g_sw_n[4];
static u64 g_sw_ns[4], g_sw_save_ns, g_sw_wait_ns;
static bool shader_ready(ShaderEnt* e, bool must_have) {
    if (hatomic_load64(&e->pending)) {
        u64 t0 = pf_now();
        if (!must_have && max_skip() > 0 && !wait_built(&e->pending) && may_skip(&e->skip_frame, &e->skip_frames)) { g_sw_wait_ns += pf_now() - t0; return false; }
        int kind = (e->disk_in ? 0 : 2) + (hatomic_load64(&e->pending) == 1 ? 0 : 1);
        u64 t1 = pf_now();
        g_sw_wait_ns += t1 - t0;
        build_now(e, NULL);
        g_sw_n[kind]++;
        g_sw_ns[kind] += pf_now() - t1;
    }
    if (e->spv) {
        u64 t0 = pf_now();
        shader_cache_save(e->key, e->fragment, &e->info, e->spv, e->words);
        free(e->spv);
        e->spv = NULL;
        g_sw_save_ns += pf_now() - t0;
    }
    return true;
}

// --- Pipelines ---------------------------------------------------------------------------------------

static VkBlendFactor blend_factor(u32 f) {
    static const VkBlendFactor t[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_SRC1_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA };
    if (f >= sizeof(t) / sizeof(t[0])) return VK_BLEND_FACTOR_ONE;
    VkBlendFactor r = t[f];
    if (!g_vk.dual_src && (r >= VK_BLEND_FACTOR_SRC1_COLOR && r <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA)) r = VK_BLEND_FACTOR_ONE;
    return r;
}
static VkBlendOp blend_op(u32 o) {
    switch (o) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkBlendFactor alpha_factor(VkBlendFactor f) {   // color factors used for alpha
    switch (f) {
    case VK_BLEND_FACTOR_SRC_COLOR: return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR: return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_ONE;
    default: return f;
    }
}

volatile uint64_t g_pipe_build_sum_ns, g_pipe_build_max_par;   // (all threads: summed vkCreateGraphicsPipelines time, most at once -- the spike line)
static VkPipeline build_pipeline(const PipeKey* k) {

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof(st));
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = k->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = k->ps;
    st[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = k->poly_mode ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.rasterizerDiscardEnable = k->raster_off ? VK_TRUE : VK_FALSE;   // (stream-out only draws)
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = k->depth_clamp ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof(cba));
    u32 ncolor = 0;
    for (u32 i = 0; i < 8; i++) if (k->color_fmt[i]) ncolor = i + 1;
    for (u32 i = 0; i < ncolor; i++) {
        cba[i].colorWriteMask = k->color_mask[i];
        if ((k->blend_on >> i) & 1) {
            u32 b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b & 0x1F);
            cba[i].colorBlendOp = blend_op((b >> 5) & 7);
            cba[i].dstColorBlendFactor = blend_factor((b >> 8) & 0x1F);
            if ((b >> 29) & 1) {
                cba[i].srcAlphaBlendFactor = blend_factor((b >> 16) & 0x1F);
                cba[i].alphaBlendOp = blend_op((b >> 21) & 7);
                cba[i].dstAlphaBlendFactor = blend_factor((b >> 24) & 0x1F);
            } else {
                cba[i].srcAlphaBlendFactor = alpha_factor(cba[i].srcColorBlendFactor);
                cba[i].alphaBlendOp = cba[i].colorBlendOp;
                cba[i].dstAlphaBlendFactor = alpha_factor(cba[i].dstColorBlendFactor);
            }
        }
    }
    if (!g_vk.independent_blend)
        for (u32 i = 1; i < ncolor; i++) { VkColorComponentFlags m = cba[i].colorWriteMask; cba[i] = cba[0]; cba[i].colorWriteMask = m; }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = ncolor;
    cb.pAttachments = cba;
    if (k->logic_op != 0xCC && g_vk.logic_op) {
        cb.logicOpEnable = VK_TRUE;
        switch (k->logic_op) {
        case 0x00: cb.logicOp = VK_LOGIC_OP_CLEAR; break;
        case 0xFF: cb.logicOp = VK_LOGIC_OP_SET; break;
        case 0x33: cb.logicOp = VK_LOGIC_OP_COPY_INVERTED; break;
        case 0xAA: cb.logicOp = VK_LOGIC_OP_NO_OP; break;
        case 0x55: cb.logicOp = VK_LOGIC_OP_INVERT; break;
        case 0x88: cb.logicOp = VK_LOGIC_OP_AND; break;
        case 0xEE: cb.logicOp = VK_LOGIC_OP_OR; break;
        case 0x66: cb.logicOp = VK_LOGIC_OP_XOR; break;
        default: cb.logicOpEnable = VK_FALSE; break;
        }
    }
    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
    };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;
    VkFormat cf[8];
    for (u32 i = 0; i < 8; i++) cf[i] = (VkFormat)k->color_fmt[i];
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = (VkFormat)k->depth_fmt;
    ri.stencilAttachmentFormat = (VkFormat)k->stencil_fmt;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &ri;
    pi.stageCount = 2;
    {   // (stream-out only -- nothing rasterized: no pixel shader to compile. These are the ink checks the
        // game waits for, so they're built on the spot; at a cold first plaza visit 7 of them took 1.7 s.
        // SPLAT_SO_WITH_PS=1: with it, as before)
        static int with_ps = -1;
        if (with_ps < 0) with_ps = getenv("SPLAT_SO_WITH_PS") != NULL;
        if (k->raster_off && !with_ps) pi.stageCount = 1;
    }
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    // (for the spike line: how many pipelines the graphics driver builds at once, and the building time summed)
    static volatile uint64_t in_flight, max_flight;
    extern volatile uint64_t g_pipe_build_sum_ns, g_pipe_build_max_par;
    u64 nf = hatomic_add64(&in_flight, 1) + 1;
    for (u64 m = hatomic_load64(&g_pipe_build_max_par); nf > m && !hatomic_cas64(&g_pipe_build_max_par, m, nf); m = hatomic_load64(&g_pipe_build_max_par)) {}
    (void)max_flight;
    {   // (SPLAT_PIPE_NOOPT=1, tests: without the driver's optimizations)
        static int noopt = -1;
        if (noopt < 0) noopt = getenv("SPLAT_PIPE_NOOPT") != NULL;
        if (noopt) pi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    }
    u64 pt0 = hnow_ns();
    VkResult r = vkCreateGraphicsPipelines(g_vk.device, g.pcache, 1, &pi, NULL, &pipe);
    hatomic_add64(&g_pipe_build_sum_ns, hnow_ns() - pt0);
    {   // SPLAT_PIPE_LOG=1: every game pipeline's handle (to read the validation layer's messages)
        static int plog = -1;
        if (plog < 0) plog = getenv("SPLAT_PIPE_LOG") != NULL;
        if (plog && pipe) printf("[pipe] %llx: game draw pipeline, %u colours, depth fmt %d stencil fmt %d, raster %s\n", (unsigned long long)(uintptr_t)pipe, ncolor, (int)k->depth_fmt, (int)k->stencil_fmt, k->raster_off ? "off" : "on");
    }
    pipe_time_note(k, hnow_ns() - pt0);
    hatomic_add64(&in_flight, (u64)-1);
    if (r != VK_SUCCESS) {
        static int warned = 0;
        if (warned++ < 4) fprintf(stderr, "[render] pipeline creation failed (%d)\n", (int)r);
        pipe = VK_NULL_HANDLE;
    }
    return pipe;
}

// --- Fast-linked pipelines (VK_EXT_graphics_pipeline_library) ----------------------------------------------------
// A match's held frames (Nov 10) were draws whose pipeline was new because its two shaders had never been PAIRED
// before -- both shaders known, compiled, but the driver builds a pipeline per pair (tens of ms cold), so the draw was
// left out and the frame held. With pipeline libraries each part is built once: the vertex input (topology), the
// vertex shader with its rasterization settings, the pixel shader with the render-target formats, and the output
// (formats, blending) -- and any pairing of ready parts is LINKED in well under a millisecond (fast linking, no
// whole-pipeline optimization). The draw uses that linked pipeline at once; the helper threads still build the
// optimized pipeline as before, which takes over when ready. Parts for every shader a map's list names are built in
// the background at the map's load (gpl_scene_parts), and for any shader the moment a draw needs a part it lacks.
// Linked pipelines are kept to the end (a draw recorded with one may still be on the card). Stream-out-only draws
// (no rasterization) keep the old path. SPLAT_GPL=0: off (vk_ctx.c).
enum { GL_VI = 1, GL_PRE = 2, GL_FS = 3, GL_FO = 4 };
typedef struct GLib {
    u64 h;
    u32 kind;
    PipeKey k;                   // (the parts of the key this kind depends on; the rest zeroed)
    ShaderEnt* se;               // (PRE / FS: the shader -- its module may still be in the making)
    VkPipeline lib;
    volatile uint64_t state;     // 0 not started, 1 queued, 2 being built, 3 ready, 4 failed
    struct GLib* next;           // (hash chain)
    struct GLib* qnext;          // (build queue)
} GLib;
#define GL_BUCKETS 16384
static GLib* g_gl[GL_BUCKETS];   // (looked up and added to on the drawing thread only)
static GLib* g_glq_head, *g_glq_tail;
static hmtx_t g_glq_mx;
static hcnd_t g_glq_cv;
static bool g_gl_started;
static volatile uint64_t g_gl_linked, g_gl_built, g_gl_used_draws, g_gl_link_ns, g_gl_missing;

static void gl_part_key(u32 kind, const PipeKey* k, PipeKey* o) {
    memset(o, 0, sizeof(*o));
    switch (kind) {
    case GL_VI: o->topo_class = k->topo_class; break;
    case GL_PRE: o->vs = k->vs; o->poly_mode = k->poly_mode; o->depth_clamp = k->depth_clamp; break;
    case GL_FS: o->ps = k->ps; memcpy(o->color_fmt, k->color_fmt, sizeof(o->color_fmt)); o->depth_fmt = k->depth_fmt; o->stencil_fmt = k->stencil_fmt; break;
    case GL_FO:
        memcpy(o->color_fmt, k->color_fmt, sizeof(o->color_fmt)); memcpy(o->color_mask, k->color_mask, sizeof(o->color_mask));
        memcpy(o->blend, k->blend, sizeof(o->blend)); o->blend_on = k->blend_on; o->logic_op = k->logic_op; o->dual_src = k->dual_src;
        o->depth_fmt = k->depth_fmt; o->stencil_fmt = k->stencil_fmt;
        break;
    }
}
static VkPipeline gl_build(u32 kind, const PipeKey* k) {
    static const VkDynamicState d_vi[] = { VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE };
    static const VkDynamicState d_pre[] = { VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS };
    static const VkDynamicState d_fs[] = { VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE };
    static const VkDynamicState d_fo[] = { VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkGraphicsPipelineLibraryCreateInfoEXT li = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st;
    memset(&st, 0, sizeof(st));
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st.pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkFormat cf[8];
    u32 ncolor = 0;
    for (u32 i = 0; i < 8; i++) { cf[i] = (VkFormat)k->color_fmt[i]; if (k->color_fmt[i]) ncolor = i + 1; }
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    li.pNext = NULL;
    pi.pNext = &li;
    pi.pDynamicState = &dy;
    switch (kind) {
    case GL_VI:
        li.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pi.pVertexInputState = &vin;
        pi.pInputAssemblyState = &ia;
        dy.dynamicStateCount = sizeof(d_vi) / sizeof(d_vi[0]); dy.pDynamicStates = d_vi;
        break;
    case GL_PRE:
        li.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
        li.pNext = &ri;   // (view mask only)
        st.stage = VK_SHADER_STAGE_VERTEX_BIT; st.module = k->vs;
        pi.stageCount = 1; pi.pStages = &st;
        rs.polygonMode = k->poly_mode ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.depthClampEnable = k->depth_clamp ? VK_TRUE : VK_FALSE;
        pi.pViewportState = &vp;
        pi.pRasterizationState = &rs;
        pi.layout = g.layout;
        dy.dynamicStateCount = sizeof(d_pre) / sizeof(d_pre[0]); dy.pDynamicStates = d_pre;
        break;
    case GL_FS:
        li.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
        ri.colorAttachmentCount = ncolor; ri.pColorAttachmentFormats = cf;
        ri.depthAttachmentFormat = (VkFormat)k->depth_fmt; ri.stencilAttachmentFormat = (VkFormat)k->stencil_fmt;
        li.pNext = &ri;
        st.stage = VK_SHADER_STAGE_FRAGMENT_BIT; st.module = k->ps;
        pi.stageCount = 1; pi.pStages = &st;
        pi.pMultisampleState = &ms;
        pi.pDepthStencilState = &ds;
        pi.layout = g.layout;
        dy.dynamicStateCount = sizeof(d_fs) / sizeof(d_fs[0]); dy.pDynamicStates = d_fs;
        break;
    case GL_FO: {
        li.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        ri.colorAttachmentCount = ncolor; ri.pColorAttachmentFormats = cf;
        ri.depthAttachmentFormat = (VkFormat)k->depth_fmt; ri.stencilAttachmentFormat = (VkFormat)k->stencil_fmt;
        li.pNext = &ri;
        memset(cba, 0, sizeof(cba));
        for (u32 i = 0; i < ncolor; i++) {   // (as build_pipeline)
            cba[i].colorWriteMask = k->color_mask[i];
            if ((k->blend_on >> i) & 1) {
                u32 b = k->blend[i];
                cba[i].blendEnable = VK_TRUE;
                cba[i].srcColorBlendFactor = blend_factor(b & 0x1F);
                cba[i].colorBlendOp = blend_op((b >> 5) & 7);
                cba[i].dstColorBlendFactor = blend_factor((b >> 8) & 0x1F);
                if ((b >> 29) & 1) {
                    cba[i].srcAlphaBlendFactor = blend_factor((b >> 16) & 0x1F);
                    cba[i].alphaBlendOp = blend_op((b >> 21) & 7);
                    cba[i].dstAlphaBlendFactor = blend_factor((b >> 24) & 0x1F);
                } else {
                    cba[i].srcAlphaBlendFactor = alpha_factor(cba[i].srcColorBlendFactor);
                    cba[i].alphaBlendOp = cba[i].colorBlendOp;
                    cba[i].dstAlphaBlendFactor = alpha_factor(cba[i].dstColorBlendFactor);
                }
            }
        }
        if (!g_vk.independent_blend)
            for (u32 i = 1; i < ncolor; i++) { VkColorComponentFlags m = cba[i].colorWriteMask; cba[i] = cba[0]; cba[i].colorWriteMask = m; }
        cb.attachmentCount = ncolor;
        cb.pAttachments = cba;
        if (k->logic_op != 0xCC && g_vk.logic_op) {
            cb.logicOpEnable = VK_TRUE;
            switch (k->logic_op) {
            case 0x00: cb.logicOp = VK_LOGIC_OP_CLEAR; break;
            case 0xFF: cb.logicOp = VK_LOGIC_OP_SET; break;
            case 0x33: cb.logicOp = VK_LOGIC_OP_COPY_INVERTED; break;
            case 0xAA: cb.logicOp = VK_LOGIC_OP_NO_OP; break;
            case 0x55: cb.logicOp = VK_LOGIC_OP_INVERT; break;
            case 0x88: cb.logicOp = VK_LOGIC_OP_AND; break;
            case 0xEE: cb.logicOp = VK_LOGIC_OP_OR; break;
            case 0x66: cb.logicOp = VK_LOGIC_OP_XOR; break;
            default: cb.logicOpEnable = VK_FALSE; break;
            }
        }
        pi.pMultisampleState = &ms;
        pi.pColorBlendState = &cb;
        dy.dynamicStateCount = sizeof(d_fo) / sizeof(d_fo[0]); dy.pDynamicStates = d_fo;
        break;
    }
    }
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.device, g.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        static int warned;
        if (warned++ < 4) fprintf(stderr, "[render] pipeline part (kind %u) creation failed\n", kind);
        p = VK_NULL_HANDLE;
    }
    return p;
}
static int gl_worker(void* arg) {   // (builds queued parts; a shader still in the making goes to the back of the queue)
    (void)arg;
    for (;;) {
        hmtx_lock(&g_glq_mx);
        while (!g_glq_head) hcnd_wait(&g_glq_cv, &g_glq_mx);
        GLib* l = g_glq_head;
        g_glq_head = l->qnext;
        if (!g_glq_head) g_glq_tail = NULL;
        l->qnext = NULL;
        hmtx_unlock(&g_glq_mx);
        if (l->se && (hatomic_load64(&l->se->pending) || !l->se->mod)) {   // (not compiled yet: later)
            if (l->se->failed) { hatomic_store64(&l->state, 4); continue; }
            hsleep_ns(2000000);
            hmtx_lock(&g_glq_mx);
            if (g_glq_tail) g_glq_tail->qnext = l; else g_glq_head = l;
            g_glq_tail = l;
            hmtx_unlock(&g_glq_mx);
            continue;
        }
        hatomic_store64(&l->state, 2);
        PipeKey k = l->k;
        if (l->kind == GL_PRE) k.vs = l->se->mod;
        if (l->kind == GL_FS) k.ps = l->se->mod;
        l->lib = gl_build(l->kind, &k);
        hatomic_add64(&g_gl_built, 1);
        hatomic_store64(&l->state, l->lib ? 3 : 4);
    }
    return 0;
}
static void gl_start(void) {
    if (g_gl_started) return;
    g_gl_started = true;
    hmtx_init(&g_glq_mx, false);
    hcnd_init(&g_glq_cv);
    for (int i = 0; i < 2; i++) { hthrd_t t; hthrd_create(&t, gl_worker, NULL, 1u << 20); }
}
static void gl_enqueue(GLib* l) {
    gl_start();
    hatomic_store64(&l->state, 1);
    hmtx_lock(&g_glq_mx);
    if (g_glq_tail) g_glq_tail->qnext = l; else g_glq_head = l;
    g_glq_tail = l;
    hcnd_signal(&g_glq_cv);
    hmtx_unlock(&g_glq_mx);
}
// The part of this kind for key k (made and queued if new). PRE / FS are keyed by the shader entry (se), whose module
// may still be in the making (the worker waits for it); VI / FO have no shader and are built right here when build_here
// (a fraction of a millisecond), else queued too.
static GLib* gl_part(u32 kind, const PipeKey* k, ShaderEnt* se, bool build_here) {
    PipeKey pk;
    gl_part_key(kind, k, &pk);
    if (kind == GL_PRE) pk.vs = (VkShaderModule)(uintptr_t)se;
    if (kind == GL_FS) pk.ps = (VkShaderModule)(uintptr_t)se;
    u64 h = hash_bytes(&pk, sizeof(pk), 0x474C0000ull + kind);
    GLib** b = &g_gl[h & (GL_BUCKETS - 1)];
    for (GLib* l = *b; l; l = l->next)
        if (l->h == h && l->kind == kind && !memcmp(&l->k, &pk, sizeof(pk))) return l;
    GLib* l = (GLib*)calloc(1, sizeof(GLib));
    l->h = h; l->kind = kind; l->k = pk; l->se = (kind == GL_PRE || kind == GL_FS) ? se : NULL;
    l->next = *b;
    *b = l;
    if (!l->se && build_here) { l->lib = gl_build(kind, &pk); hatomic_add64(&g_gl_built, 1); hatomic_store64(&l->state, l->lib ? 3 : 4); }
    else gl_enqueue(l);
    return l;
}
static ShaderEnt* g_gl_cur_vs, *g_gl_cur_ps;   // (the draw's two shaders, set just before its get_pipeline)
// A fast-linked pipeline for key k if its four parts are ready (missing parts are queued for next time); NULL if not.
static VkPipeline gl_fast(const PipeKey* k) {
    if (!g_vk.gpl || k->raster_off || !k->vs || !k->ps) return VK_NULL_HANDLE;
    ShaderEnt* vs = g_gl_cur_vs, *ps = g_gl_cur_ps;
    if (!vs || !ps || vs->mod != k->vs || ps->mod != k->ps) return VK_NULL_HANDLE;
    GLib* p[4];
    p[0] = gl_part(GL_VI, k, NULL, true);
    p[1] = gl_part(GL_PRE, k, vs, false);
    p[2] = gl_part(GL_FS, k, ps, false);
    p[3] = gl_part(GL_FO, k, NULL, true);
    VkPipeline libs[4];
    for (int i = 0; i < 4; i++) {
        if (hatomic_load64(&p[i]->state) != 3) { hatomic_add64(&g_gl_missing, 1); return VK_NULL_HANDLE; }
        libs[i] = p[i]->lib;
    }
    u64 t0 = hnow_ns();
    VkPipelineLibraryCreateInfoKHR lci = { VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
    lci.libraryCount = 4;
    lci.pLibraries = libs;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &lci;
    pi.layout = g.layout;
    VkPipeline out = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &pi, NULL, &out) != VK_SUCCESS) out = VK_NULL_HANDLE;
    hatomic_add64(&g_gl_link_ns, hnow_ns() - t0);
    if (out) hatomic_add64(&g_gl_linked, 1);
    return out;
}
// A map's load: parts for every shader its list names (and the battle maps' shared list), built in the background.
static void gl_scene_parts(const PipeKey* rk, ShaderEnt* v, ShaderEnt* ps) {
    if (!g_vk.gpl || rk->raster_off) return;
    PipeKey k = *rk;
    gl_part(GL_VI, &k, NULL, false);
    gl_part(GL_PRE, &k, v, false);
    gl_part(GL_FS, &k, ps, false);
    gl_part(GL_FO, &k, NULL, false);
}

// --- Helper threads for new shaders and pipelines -------------------------------------------------
// Compiling a new shader or pipeline can take tens of milliseconds, and a
// new area brings hundreds: done on the GPU thread they froze the game
// (1-2 fps while entering the plaza). They're built on helper threads
// instead; draws that need one that isn't ready yet are skipped for those
// few frames, like Cemu's asynchronous shader compiling.
// SPLAT_SYNC_SHADERS=1 turns this off.
typedef struct AJob { ShaderEnt* se; PipeEnt* pe; PrefEnt* pf; struct AJob* next; } AJob;
static hmtx_t g_aq_lock;
static hcnd_t g_aq_cv;
static AJob* g_aq_head;   // the start-up preload's jobs
static AJob* g_aq_tail;
static AJob* g_dq_head;   // jobs a draw is waiting for: always taken first
static AJob* g_dq_tail;
static bool g_preloading;   // (async_queue: the preload is queueing)
static int g_async = -1;
static volatile uint64_t g_async_built;
static u64 g_async_t0;

// A queued shader: GLSL -> SPIR-V -> module.
// SPLAT_PIPE_TIMES=1 (measurement): every pipeline built, how long the graphics driver took and how big its
// two shaders' SPIR-V was -> debug_output/pipe_times.txt ("ms vs_key vs_words ps_key ps_words")
static int pipe_times_on(void) { static int on = -1; if (on < 0) on = getenv("SPLAT_PIPE_TIMES") != NULL; return on; }
static struct { VkShaderModule m; u64 key; u32 words; } g_modreg[65536];
static volatile long g_modreg_lock;
static void modreg_put(VkShaderModule m, u64 key, size_t words) {
    if (!pipe_times_on() || !m) return;
    while (_InterlockedCompareExchange(&g_modreg_lock, 1, 0)) {}
    u32 i = (u32)(((uintptr_t)m >> 3) * 2654435761u) & 65535u;
    for (u32 n = 0; n < 65536 && g_modreg[i].m && g_modreg[i].m != m; n++) i = (i + 1) & 65535u;
    g_modreg[i].m = m; g_modreg[i].key = key; g_modreg[i].words = (u32)words;
    _InterlockedExchange(&g_modreg_lock, 0);
}
static void modreg_get(VkShaderModule m, u64* key, u32* words) {
    *key = 0; *words = 0;
    while (_InterlockedCompareExchange(&g_modreg_lock, 1, 0)) {}
    u32 i = (u32)(((uintptr_t)m >> 3) * 2654435761u) & 65535u;
    for (u32 n = 0; n < 65536 && g_modreg[i].m; n++, i = (i + 1) & 65535u)
        if (g_modreg[i].m == m) { *key = g_modreg[i].key; *words = g_modreg[i].words; break; }
    _InterlockedExchange(&g_modreg_lock, 0);
}
static void pipe_time_note(const PipeKey* k, u64 ns) {
    if (!pipe_times_on()) return;
    u64 vk, pk; u32 vw, pw;
    modreg_get(k->vs, &vk, &vw);
    modreg_get(k->ps, &pk, &pw);
    static volatile long lock;
    while (_InterlockedCompareExchange(&lock, 1, 0)) {}
    static FILE* f;
    if (!f) { char path[96]; snprintf(path, sizeof(path), "%s/pipe_times.txt", debug_dir()); f = fopen(path, "w"); }
    if (f) { fprintf(f, "%.2f %016llx %u %016llx %u\n", ns / 1e6, (unsigned long long)vk, vw, (unsigned long long)pk, pw); fflush(f); }
    _InterlockedExchange(&lock, 0);
}

// The highest fixed index each uniform block is read at without a clamp, from the translator's
// "splat.ub=B:N,..." note in the SPIR-V (latte_shader.c assemble; none in shaders cached by older
// translators, which clamp every read): the upload pads a smaller block up to there.
static void shader_note_ub(ShaderEnt* e, const u32* spv, size_t words) {
    memset(e->ub_fixed, 0, sizeof(e->ub_fixed));
    if (!spv) return;
    for (size_t i = 5; i < words && i < 4096; ) {
        u32 wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > words) return;
        if (op == 5 || op == 6 || op == 71) return;   // (OpName, OpMemberName, OpDecorate: past the notes)
        if (op == 4 && wc > 3) {   // OpSourceExtension
            const char* t = (const char*)(spv + i + 1);
            size_t max = (size_t)(wc - 1) * 4;
            if (memchr(t, 0, max) && !strncmp(t, "splat.ub=", 9)) {
                const char* p = t + 9;
                for (;;) {
                    char* q;
                    unsigned long b = strtoul(p, &q, 10);
                    if (*q != ':') return;
                    unsigned long n = strtoul(q + 1, &q, 10);
                    if (b < 16 && n <= 255) e->ub_fixed[b] = (u8)n;
                    if (*q != ',') {
                        static volatile long shown;
                        if (!_InterlockedExchange(&shown, 1))
                            printf("[render] fixed-index uniform reads: shader %016llx reads up to %s (vec4s per block; smaller blocks get padded)\n", (unsigned long long)e->key, t + 9);
                        return;
                    }
                    p = q + 1;
                }
            }
        }
        i += wc;
    }
}

static void shader_build_job(ShaderEnt* e) {
    if (e->disk_in) {   // (from the disk cache: only the module to make)
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = e->disk_words * 4;
        ci.pCode = e->disk_in;
        if (vkCreateShaderModule(g_vk.device, &ci, NULL, &e->mod) != VK_SUCCESS) e->failed = true;
        else modreg_put(e->mod, e->key, e->disk_words);
        free(e->disk_in);
        e->disk_in = NULL;
        return;
    }
    size_t words = 0;
    u32* spv = glsl_to_spirv(e->fragment, e->glsl, &words);
    shader_note_ub(e, spv, words);
    free(e->glsl);
    e->glsl = NULL;
    if (!spv) e->failed = true;
    else {
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = words * 4;
        ci.pCode = spv;
        if (vkCreateShaderModule(g_vk.device, &ci, NULL, &e->mod) != VK_SUCCESS) { e->failed = true; free(spv); spv = NULL; }
        else modreg_put(e->mod, e->key, words);
    }
    e->words = words;
    e->spv = spv;
}

// What each thread of ours waits for right now. A boot that stalled (Oct 25: the game's GX2DrawDone timing out
// every 5 s from frame ~8 on, the drawing thread never drawing again) is a wait here that never ends:
// latte_render_report_waits prints them when GX2DrawDone times out (gx2.c).
#ifdef _MSC_VER
#define SPLAT_TLS __declspec(thread)
#else
#define SPLAT_TLS _Thread_local
#endif
typedef struct { const char* volatile what[2]; volatile uint64_t* volatile p[2]; volatile u64 since[2]; volatile int depth; unsigned long tid; } WaitState;
static WaitState g_ws[64];
static volatile uint64_t g_ws_n;
static SPLAT_TLS WaitState* t_ws;
static WaitState* ws_me(void) {
    if (!t_ws) {
        u64 i = hatomic_add64(&g_ws_n, 1);
        if (i >= 64) i = 63;
#ifdef _WIN32
        g_ws[i].tid = GetCurrentThreadId();
#endif
        t_ws = &g_ws[i];
    }
    return t_ws;
}
static void wait_mark(const char* what, volatile uint64_t* p) {
    WaitState* w = ws_me();
    int d = w->depth;
    if (d < 2) { w->p[d] = p; w->since[d] = hnow_ns(); w->what[d] = what; }
    w->depth = d + 1;
}
static void wait_unmark(void) {
    WaitState* w = ws_me();
    int d = w->depth - 1;
    if (d < 0) d = 0;
    if (d < 2) { w->what[d] = NULL; w->p[d] = NULL; }
    w->depth = d;
}

// A look-ahead pipeline: its shaders first (compiled here if no one has
// started them; waited for if someone has), then the pipeline itself.
static void ensure_shader(ShaderEnt* e) {
    if (hatomic_cas64(&e->pending, 1, 2)) { shader_build_job(e); hatomic_store64(&e->pending, 0); }
    else { wait_mark("a shader another thread is building (ensure_shader)", &e->pending); while (hatomic_load64(&e->pending)) hsleep_ns(200000); wait_unmark(); }
}
static void prefetch_job(PrefEnt* f) {
    if (!hatomic_cas64(&f->pending, 1, 2)) return;
    ensure_shader(f->vs);
    ensure_shader(f->ps);
    if (!f->vs->failed && !f->ps->failed && f->vs->mod && f->ps->mod) {
        PipeKey k = f->k;
        k.vs = f->vs->mod;
        k.ps = f->ps->mod;
        f->pipe = build_pipeline(&k);
    }
    hatomic_store64(&f->pending, 0);
}

// pending: 1 = waiting in line, 2 = a helper thread is building it,
// 3 = the drawing thread took it out of line to build it itself (a frame
// that can't do without it shouldn't wait for everything queued before it).
static void async_run(AJob* j);
static int async_worker(void* arg) {
    (void)arg;
#ifdef _WIN32
    // Below the game's own threads: at start-up these build the ~20,000
    // pipelines the game used before, and at normal priority that took
    // processor time from the game while it loaded the plaza / story levels.
    // (A pipeline the game is waiting for is built by the drawing thread
    // itself meanwhile: build_now.)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        hmtx_lock(&g_aq_lock);
        while (!g_dq_head && !g_aq_head) hcnd_wait(&g_aq_cv, &g_aq_lock);
        AJob* j;
        if (g_dq_head) {
            j = g_dq_head;
            g_dq_head = j->next;
            if (!g_dq_head) g_dq_tail = NULL;
        } else {
            j = g_aq_head;
            g_aq_head = j->next;
            if (!g_aq_head) g_aq_tail = NULL;
        }
        hmtx_unlock(&g_aq_lock);
        async_run(j);
    }
    return 0;
}

// Extra helpers for the moments the drawing thread itself waits for a build
// (a new scene's first frame: hundreds of pipelines the game can't do
// without, and the game waits for the drawing thread meanwhile -- its
// processors are free). They sleep otherwise, so play keeps the processors
// the helpers above leave it. SPLAT_BURST_HELPERS=n: how many (0: none).
static hcnd_t g_burst_cv;
static u32 g_draw_blocked;   // (under g_aq_lock)
static bool g_burst_on;
static void burst_begin(void) {
    if (!g_burst_on) return;
    hmtx_lock(&g_aq_lock);
    g_draw_blocked++;
    if (g_dq_head) hcnd_broadcast(&g_burst_cv);
    hmtx_unlock(&g_aq_lock);
}
static void burst_end(void) {
    if (!g_burst_on) return;
    hmtx_lock(&g_aq_lock);
    g_draw_blocked--;
    hmtx_unlock(&g_aq_lock);
}
static int burst_worker(void* arg) {
    (void)arg;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        hmtx_lock(&g_aq_lock);
        while (!g_draw_blocked || !g_dq_head) hcnd_wait(&g_burst_cv, &g_aq_lock);
        AJob* j = g_dq_head;
        g_dq_head = j->next;
        if (!g_dq_head) g_dq_tail = NULL;
        hmtx_unlock(&g_aq_lock);
        async_run(j);
    }
    return 0;
}

static void async_run(AJob* j) {
        wait_mark(j->pf ? "building a look-ahead pipeline (helper)" : j->se ? "building a shader (helper)" : "building a pipeline (helper)",
                 j->pf ? &j->pf->pending : j->se ? &j->se->pending : &j->pe->pending);
        if (j->pf) {
            prefetch_job(j->pf);
        } else if (j->se) {
            ShaderEnt* e = j->se;
            if (hatomic_cas64(&e->pending, 1, 2)) {
                shader_build_job(e);
                hatomic_store64(&e->pending, 0);
            }
        } else {
            PipeEnt* e = j->pe;
            if (hatomic_cas64(&e->pending, 1, 2)) {
                e->pipe = build_pipeline(&e->key);
                hatomic_store64(&e->pending, 0);
            }
        }
        wait_unmark();
        u64 built = hatomic_add64(&g_async_built, 1) + 1;
        free(j);
        {   // (for the log: when a big batch -- the start-up preload -- is done)
            static volatile uint64_t reported;
            hmtx_lock(&g_aq_lock);
            bool empty = !g_aq_head && !g_dq_head;
            hmtx_unlock(&g_aq_lock);
            u64 r = hatomic_load64(&reported);
            if (empty && built >= r + 200 && hatomic_cas64(&reported, r, built))
                printf("[render] helper threads done for now: %llu shaders/pipelines built in the background so far (%.1f s after start)\n",
                       (unsigned long long)built, (hnow_ns() - g_async_t0) / 1e9);
        }
}

// The drawing thread needs this one now: build it here if no helper thread
// has started on it yet, else wait for that helper.
static u32 g_now_builds;   // (for the log: built or waited for by the drawing thread, and how long)
volatile unsigned splat_sync_builds;   // (the same count for the window title: "preparing graphics" while a frame waits on builds)
static u64 g_now_ns;
static u32 g_fr_build_n[2]; static u64 g_fr_build_ns[2];   // (this frame's drawing-thread builds: [0] shaders, [1] pipelines -- the spike line)
// Why the drawing thread had to build (or wait for) one itself: the [perf] "built while drawing" reasons.
enum { BW_RESULTS, BW_ONCE, BW_SMALL, BW_SKIPS, BW_SHADER_SKIPS, BW_N };
static const char* const k_bw_name[BW_N] = { "results read back", "buffers made once", "small full-screen draws", "skipped long enough", "shaders skipped long enough" };
static int g_bw_why = BW_SKIPS;   // (set by the draw asking)
static u32 g_bw_n[BW_N];
static u64 g_bw_ns[BW_N];
typedef struct Surf Surf;
static Surf* g_od_cur[9]; static u32 g_od_ncur;   // (SPLAT_ONCE_DEBUG: the draw's buffers)
typedef struct { Surf* s; u64 frame, ns; u32 n; } OnceDbg;
static OnceDbg g_od[512]; static u32 g_od_n;
static int once_dbg(void) { static int on = -1; if (on < 0) on = getenv("SPLAT_ONCE_DEBUG") != NULL; return on; }
static void bw_note(u64 ns) {
    g_bw_n[g_bw_why]++; g_bw_ns[g_bw_why] += ns;
    if (g_bw_why == BW_ONCE && once_dbg())
        for (u32 i = 0; i < g_od_ncur; i++) {
            u32 j = 0;
            while (j < g_od_n && !(g_od[j].s == g_od_cur[i] && g_od[j].frame == g.frame)) j++;
            if (j == g_od_n) { if (g_od_n >= 512) continue; g_od_n++; g_od[j].s = g_od_cur[i]; g_od[j].frame = g.frame; g_od[j].ns = 0; g_od[j].n = 0; }
            g_od[j].ns += ns / g_od_ncur; g_od[j].n++;
        }
}
static void burst_begin(void);
static void burst_end(void);
static void build_now(ShaderEnt* se, PipeEnt* pe) {
    volatile uint64_t* p = se ? &se->pending : &pe->pending;
    u64 t0 = hnow_ns();
    g_fr_build_n[se ? 0 : 1]++;
    g_now_builds++; splat_sync_builds++;
    bool blocked = hatomic_load64(p) != 0;
    if (blocked) burst_begin();
    if (hatomic_cas64(p, 1, 3)) {
        if (se) shader_build_job(se);
        else pe->pipe = build_pipeline(&pe->key);
        hatomic_store64(p, 0);
    } else {
        wait_mark(se ? "a shader a helper is building (build_now)" : "a pipeline a helper is building (build_now)", p);
        while (hatomic_load64(p)) hsleep_ns(100000);
        wait_unmark();
    }
    if (blocked) burst_end();
    g_now_ns += hnow_ns() - t0;
    g_fr_build_ns[se ? 0 : 1] += hnow_ns() - t0;
    bw_note(hnow_ns() - t0);
    // (any wait here starts the look-ahead, not only a slow one: at a warm scene
    // load each of ~250 cached shader modules took 0.1-0.5 ms -- never slow
    // enough -- and the drawing thread met them one draw at a time, 40-65 ms
    // a load; read ahead, the helpers make them side by side. SPLAT_LA_SLOW_ONLY=1:
    // only after a slow one, as before)
    static int slow_only = -1;
    if (slow_only < 0) slow_only = getenv("SPLAT_LA_SLOW_ONLY") != NULL;
    if (hnow_ns() - t0 > LA_SLOW_NS || (blocked && !slow_only)) g_la_want = true;
}

static bool async_on(void) {
    if (g_async < 0) {
        g_async = getenv("SPLAT_SYNC_SHADERS") ? 0 : 1;
        g_async_t0 = hnow_ns();
        if (g_async) {
            hmtx_init(&g_aq_lock, false);
            hcnd_init(&g_aq_cv);
            glsl_init();
            int procs = 8;
#ifdef _WIN32
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            procs = (int)si.dwNumberOfProcessors;
#else
            procs = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
            int n = procs - 4;
            // (a laptop with 4 logical processors: one helper, so the game's
            // main thread and the drawing thread keep their processors)
            if (n < (procs <= 4 ? 1 : 2)) n = procs <= 4 ? 1 : 2;
            if (n > 6) n = 6;
            { const char* e = getenv("SPLAT_BUILD_THREADS"); if (e && atoi(e) > 0) n = atoi(e) > 16 ? 16 : atoi(e); }   // (tests)
            for (int i = 0; i < n; i++) {
                hthrd_t t;
                hthrd_create(&t, async_worker, NULL, 8u << 20);
            }
            // (+ the burst helpers: up to all processors but one, together with the drawing thread)
            int extra = procs - 2 - n;
            if (extra > 8) extra = 8;
            { const char* e = getenv("SPLAT_BURST_HELPERS"); if (e) extra = atoi(e); }
            if (extra < 0) extra = 0;
            hcnd_init(&g_burst_cv);
            for (int i = 0; i < extra; i++) {
                hthrd_t t;
                hthrd_create(&t, burst_worker, NULL, 8u << 20);
            }
            g_burst_on = extra > 0;
            printf("[render] building new shaders on %d helper threads (+%d while the drawing thread waits for one)\n", n, extra);
        }
    }
    return g_async == 1;
}

static void async_queue(ShaderEnt* se, PipeEnt* pe) {
    AJob* j = (AJob*)calloc(1, sizeof(AJob));
    j->se = se;
    j->pe = pe;
    hmtx_lock(&g_aq_lock);
    if (g_preloading) {   // (the start-up preload: at the back of the line)
        if (g_aq_tail) g_aq_tail->next = j; else g_aq_head = j;
        g_aq_tail = j;
    } else {
        // Something a draw needs now: ahead of the preload. At start-up the
        // helpers rebuild every pipeline of earlier sessions (tens of
        // thousands, minutes of work); a new effect's shaders waited behind
        // all of them, so its draws ran out of skips and the drawing thread
        // built them one by one itself (the ink rail: 0.4-1.4 s frames).
        if (g_dq_tail) g_dq_tail->next = j; else g_dq_head = j;
        g_dq_tail = j;
        if (g_draw_blocked) hcnd_signal(&g_burst_cv);
    }
    hcnd_signal(&g_aq_cv);
    hmtx_unlock(&g_aq_lock);
}

static void async_queue_pf(PrefEnt* f) {   // (look-ahead pipelines: in the line of things draws need, at its end)
    AJob* j = (AJob*)calloc(1, sizeof(AJob));
    j->pf = f;
    hmtx_lock(&g_aq_lock);
    if (g_dq_tail) g_dq_tail->next = j; else g_dq_head = j;
    g_dq_tail = j;
    if (g_draw_blocked) hcnd_signal(&g_burst_cv);
    hcnd_signal(&g_aq_cv);
    hmtx_unlock(&g_aq_lock);
}

// Every pipeline the game has used is listed on disk (pipeline_keys.bin: the
// two shaders' keys + the rest of the pipeline's settings). Next time they're
// all built before the game's first picture, so nothing pops in later.
#define PIPE_KEYS_VERSION 0x504B0003u      // (v3: + the scene the pipeline was used in, for the per-scene warm-up)
#define PIPE_KEYS_VERSION_V2 0x504B0002u
typedef struct { u64 vs, ps; PipeKey k; } PipeRecV2;
typedef struct { u64 vs, ps; PipeKey k; char scene[24]; } PipeRec;
static PipeRec* g_pk_recs;   // every listed pipeline (the file, de-duplicated), kept for the per-scene warm-ups
static u32 g_pk_nrec, g_pk_cap_recs;
static FILE* g_pipe_keys_out;
static u64 g_cur_vs_key, g_cur_ps_key;   // shaders of the draw asking for a pipeline
static bool g_pipe_keys_skip;    // (a debug view's pipeline: not listed for the next start, and never built on
                                 // the drawing thread -- its draws are left out until a helper built it: switching
                                 // views built dozens on the spot, 3-second stops that cut the sound)
// Each pipeline is listed once: every session used to append every pipeline
// it built again (the user's file: 763,000 records of 50,000 different
// pipelines, 128 MB read at each start). The set of listed ones (hashes of
// the records) keeps them from being listed again; a file mostly made of
// repeats is rewritten once at start-up (preload_pipelines).
static u64* g_pk_set;
static u32 g_pk_cap, g_pk_n;
static bool pk_add(u64 h) {   // (false: listed already)
    if (!h) h = 1;
    if ((g_pk_n + 1) * 2 > g_pk_cap) {
        u32 cap = g_pk_cap ? g_pk_cap * 2 : 65536;
        u64* t = (u64*)calloc(cap, sizeof(u64));
        if (!t) return true;
        for (u32 i = 0; i < g_pk_cap; i++) {
            u64 v = g_pk_set[i];
            if (!v) continue;
            u32 j = (u32)(v ^ (v >> 32)) & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = v;
        }
        free(g_pk_set);
        g_pk_set = t;
        g_pk_cap = cap;
    }
    u32 m = g_pk_cap - 1, i = (u32)(h ^ (h >> 32)) & m;
    while (g_pk_set[i]) { if (g_pk_set[i] == h) return false; i = (i + 1) & m; }
    g_pk_set[i] = h;
    g_pk_n++;
    return true;
}
static void pipe_keys_save(const PipeKey* k) {
    if (!g_pipe_keys_out || g_pipe_keys_skip) return;
    PipeRec r;
    memset(&r, 0, sizeof(r));
    r.vs = g_cur_vs_key;
    r.ps = g_cur_ps_key;
    r.k = *k;
    r.k.vs = VK_NULL_HANDLE;
    r.k.ps = VK_NULL_HANDLE;
    {   // (the scene it was used in: a pipeline used in two scenes is listed under both)
        extern char splat_scene_map[64];
        snprintf(r.scene, sizeof(r.scene), "%s", splat_scene_map[0] ? splat_scene_map : "Boot");   // (before any map: the title screens)
    }
    if (!pk_add(hash_bytes(&r, sizeof(r), 13))) return;
    fwrite(&r, sizeof(r), 1, g_pipe_keys_out);
    fflush(g_pipe_keys_out);
    if (g_pk_nrec < 400000) {   // (kept in memory too: this session's warm-up of a scene seen again)
        if (g_pk_nrec >= g_pk_cap_recs) {
            u32 cap = g_pk_cap_recs ? g_pk_cap_recs * 2 : 4096;
            while (cap <= g_pk_nrec) cap *= 2;
            PipeRec* t2 = (PipeRec*)realloc(g_pk_recs, cap * sizeof(PipeRec));
            if (!t2) return;
            g_pk_recs = t2;
            g_pk_cap_recs = cap;
        }
        g_pk_recs[g_pk_nrec++] = r;
    }
}

// --- the look-ahead's pipelines (PrefEnt) ---
static u64 pf_hash(const PipeKey* k) {   // (k's vs / ps are left out)
    PipeKey z = *k;
    z.vs = VK_NULL_HANDLE;
    z.ps = VK_NULL_HANDLE;
    return hash_bytes(&z, sizeof(z), 91);
}
static PrefEnt* pf_find(ShaderEnt* vs, ShaderEnt* ps, const PipeKey* partial, u64 h) {
    for (PrefEnt* f = g_pf_tab[h & (PF_BUCKETS - 1)]; f; f = f->next)
        if (f->hash == h && f->vs == vs && f->ps == ps && memcmp(&f->k, partial, sizeof(*partial)) == 0) return f;
    return NULL;
}
// (a draw's finished key: the same shaders' modules + the rest)
static PrefEnt* pf_find_full(const PipeKey* k) {
    PipeKey z = *k;
    z.vs = VK_NULL_HANDLE;
    z.ps = VK_NULL_HANDLE;
    u64 h = hash_bytes(&z, sizeof(z), 91);
    for (PrefEnt* f = g_pf_tab[h & (PF_BUCKETS - 1)]; f; f = f->next)
        if (f->hash == h && !f->taken && f->vs->mod == k->vs && f->ps->mod == k->ps && memcmp(&f->k, &z, sizeof(z)) == 0) return f;
    return NULL;
}
static PipeEnt* pipe_lookup(const PipeKey* k) {
    u64 h = hash_bytes(k, sizeof(*k), 77);
    for (PipeEnt* e = g.pipes[h & (LR_BUCKETS - 1)]; e; e = e->next)
        if (e->hash == h && memcmp(&e->key, k, sizeof(*k)) == 0) return e;
    return NULL;
}
// End of a frame: finished look-ahead pipelines no draw took join the others
// (a later draw may well need them); taken ones are freed.
static void pf_sweep(void) {
    PrefEnt** pp = &g_pf_all;
    while (*pp) {
        PrefEnt* f = *pp;
        if (hatomic_load64(&f->pending)) { pp = &f->all; continue; }
        if (f->pipe) {
            PipeKey k = f->k;
            k.vs = f->vs->mod;
            k.ps = f->ps->mod;
            if (k.vs && k.ps && !pipe_lookup(&k)) {
                PipeEnt* e = (PipeEnt*)calloc(1, sizeof(PipeEnt));
                e->key = k;
                e->hash = hash_bytes(&k, sizeof(k), 77);
                e->pipe = f->pipe;
                e->next = g.pipes[e->hash & (LR_BUCKETS - 1)];
                g.pipes[e->hash & (LR_BUCKETS - 1)] = e;
                g_pf_kept++;
            } else vkDestroyPipeline(g_vk.device, f->pipe, NULL);   // (never bound: safe to drop)
        }
        PrefEnt** b = &g_pf_tab[f->hash & (PF_BUCKETS - 1)];
        while (*b && *b != f) b = &(*b)->next;
        if (*b) *b = f->next;
        *pp = f->all;
        free(f);
        g_pf_n--;
    }
}

// NULL with *wait set: still being built.
static VkPipeline get_pipeline(const PipeKey* k, bool must_have, bool* wait) {
    *wait = false;
    {   // (most draws use the same pipeline as the draw before: no hashing then)
        static PipeEnt* last;
        if (last && !hatomic_load64(&last->pending) && last->pipe && memcmp(&last->key, k, sizeof(*k)) == 0) return last->pipe;
        last = NULL;
        VkPipeline p = VK_NULL_HANDLE;
        u64 h = hash_bytes(k, sizeof(*k), 77);
        for (PipeEnt* e = g.pipes[h & (LR_BUCKETS - 1)]; e; e = e->next)
            if (e->hash == h && memcmp(&e->key, k, sizeof(*k)) == 0) {
                if (!hatomic_load64(&e->pending) && e->pipe) { last = e; p = e->pipe; }
                break;
            }
        if (p) return p;
    }
    u64 h = hash_bytes(k, sizeof(*k), 77);
    PipeEnt** bucket = &g.pipes[h & (LR_BUCKETS - 1)];
    for (PipeEnt* e = *bucket; e; e = e->next)
        if (e->hash == h && memcmp(&e->key, k, sizeof(*k)) == 0) {
            if (hatomic_load64(&e->pending)) {
                if (g_pipe_keys_skip && async_on()) { *wait = true; return VK_NULL_HANDLE; }
                if (!e->interim && async_on()) e->interim = gl_fast(k);   // (a fast-linked stand-in: drawn now instead of left out / waited for)
                if (e->interim && hatomic_load64(&e->pending)) { hatomic_add64(&g_gl_used_draws, 1); return e->interim; }
                if (!must_have && max_skip() > 0 && !wait_built(&e->pending) && may_skip(&e->skip_frame, &e->skip_frames)) { *wait = true; return VK_NULL_HANDLE; }
                build_now(NULL, e);
            }
            return e->pipe;
        }
    PrefEnt* f = g_pf_n ? pf_find_full(k) : NULL;
    if (f) {   // the look-ahead saw this draw coming: built (or being built) already
        if (hatomic_load64(&f->pending) && !g_pipe_keys_skip && async_on()) {   // (still building: a fast-linked stand-in meanwhile)
            if (!f->interim) f->interim = gl_fast(k);
            if (f->interim && hatomic_load64(&f->pending)) { hatomic_add64(&g_gl_used_draws, 1); return f->interim; }
        }
        if (hatomic_load64(&f->pending) && !must_have && max_skip() > 0 && !wait_built(&f->pending) && may_skip(&f->skip_frame, &f->skip_frames)) { *wait = true; return VK_NULL_HANDLE; }
        if (hatomic_load64(&f->pending)) {
            u64 t0 = hnow_ns();
            burst_begin();
            if (hatomic_cas64(&f->pending, 1, 3)) { f->pipe = build_pipeline(k); hatomic_store64(&f->pending, 0); }   // (no helper got to it yet)
            else { wait_mark("a look-ahead pipeline a helper is building (get_pipeline)", &f->pending); while (hatomic_load64(&f->pending)) hsleep_ns(100000); wait_unmark(); }
            burst_end();
            u64 dt = hnow_ns() - t0;
            g_now_builds++; splat_sync_builds++; g_now_ns += dt; g_fr_build_n[1]++; g_fr_build_ns[1] += dt;
            bw_note(dt);
            if (dt > LA_SLOW_NS) g_la_want = true;
        }
        PipeEnt* e = (PipeEnt*)calloc(1, sizeof(PipeEnt));
        e->key = *k;
        e->hash = h;
        e->next = *bucket;
        *bucket = e;
        g.new_pipes++;
        pipe_keys_save(k);
        e->pipe = f->pipe ? f->pipe : build_pipeline(k);
        f->pipe = VK_NULL_HANDLE;
        f->taken = true;   // (freed at the end of the frame)
        g_pf_used++;
        return e->pipe;
    }
    if (g_pf_n) {   // SPLAT_LA_DEBUG=1: a draw the look-ahead got wrong -- which part of the key differs?
        static int dbg = -1, shown = 0;
        if (dbg < 0) dbg = getenv("SPLAT_LA_DEBUG") != NULL;
        if (dbg && shown < 30) {
            for (PrefEnt* q = g_pf_all; q; q = q->all) {
                if (q->taken || q->vs->mod != k->vs || q->ps->mod != k->ps) continue;
                const PipeKey* a = &q->k;
                char why[300] = "";
                #define KD(f) if (memcmp(&a->f, &k->f, sizeof(a->f))) strcat(why, " " #f);
                KD(color_fmt) KD(color_mask) KD(blend) KD(blend_on) KD(depth_fmt) KD(stencil_fmt) KD(topo_class) KD(depth_clamp) KD(logic_op) KD(poly_mode) KD(dual_src) KD(raster_off)
                #undef KD
                printf("[render] look-ahead missed a draw's pipeline (same shaders) by:%s | fmt0 %u/%u mask0 %X/%X depth %u/%u\n", why,
                       a->color_fmt[0], k->color_fmt[0], a->color_mask[0], k->color_mask[0], a->depth_fmt, k->depth_fmt);
                shown++;
                break;
            }
        }
    }
    PipeEnt* e = (PipeEnt*)calloc(1, sizeof(PipeEnt));
    e->key = *k;
    e->hash = h;
    e->next = *bucket;
    *bucket = e;
    g.new_pipes++;
    pipe_keys_save(k);
    if (g_pipe_keys_skip && async_on()) {   // (a debug view's: built by a helper, the draw left out meanwhile)
        e->pending = 1;
        async_queue(NULL, e);
        *wait = true;
        return VK_NULL_HANDLE;
    }
    if (async_on() && (e->interim = gl_fast(k)) != VK_NULL_HANDLE) {   // (both shaders' parts ready: linked now, the real one built meanwhile)
        e->pending = 1;
        async_queue(NULL, e);
        hatomic_add64(&g_gl_used_draws, 1);
        return e->interim;
    }
    if (!must_have && max_skip() > 0 && async_on()) {
        e->pending = 1;
        async_queue(NULL, e);
        if (wait_built(&e->pending)) return e->pipe;
        may_skip(&e->skip_frame, &e->skip_frames);
        *wait = true;
        return VK_NULL_HANDLE;
    }
    u64 t0 = hnow_ns();   // (a draw that can't wait: built right here -- counted with build_now's)
    burst_begin();
    e->pipe = build_pipeline(k);
    burst_end();
    g_now_builds++; splat_sync_builds++;
    g_now_ns += hnow_ns() - t0;
    g_fr_build_n[1]++; g_fr_build_ns[1] += hnow_ns() - t0;
    bw_note(hnow_ns() - t0);
    if (hnow_ns() - t0 > LA_SLOW_NS) g_la_want = true;
    return e->pipe;
}

// --- Setup -------------------------------------------------------------------------------------------

static bool make_dummy(Img* im, VkImageType type, u32 layers, bool cube) {
    if (!make_image(im, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, layers, type, cube, false, false)) return false;
    VkCommandBuffer cb = transfer_cmd();
    VkClearColorValue v;
    memset(&v, 0, sizeof(v));
    if (cube) { v.float32[0] = v.float32[1] = v.float32[2] = 0.5f; v.float32[3] = 1.0f; }   // missing light probes: flat gray light
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, im->layers };
    vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    return true;
}

// Missing 3D textures are almost always color-grading tables; an identity
// table leaves colors as they are (instead of turning everything black).
static bool make_identity_lut(Img* im) {
    const u32 n = 16;
    if (!make_image(im, VK_FORMAT_R8G8B8A8_UNORM, n, n, n, VK_IMAGE_TYPE_3D, false, false, false)) return false;
    VkBuffer buf;
    u32 off;
    u8* p = arena(n * n * n * 4, 16, &buf, &off);
    if (!p) return false;
    for (u32 z = 0; z < n; z++)
        for (u32 y = 0; y < n; y++)
            for (u32 x = 0; x < n; x++) {
                u8* t = p + ((z * n + y) * n + x) * 4;
                t[0] = (u8)(x * 255 / (n - 1)); t[1] = (u8)(y * 255 / (n - 1)); t[2] = (u8)(z * 255 / (n - 1)); t[3] = 255;
            }
    VkCommandBuffer cb = transfer_cmd();
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = n; c.imageExtent.height = n; c.imageExtent.depth = n;
    vkCmdCopyBufferToImage(cb, buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    return true;
}

static bool init(void) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_vk.queue_family;
    if (vkCreateCommandPool(g_vk.device, &pci, NULL, &g.pool) != VK_SUCCESS) return false;
    for (int i = 0; i < NSUB; i++) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = g.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g_vk.device, &ai, &g.sub[i].cb) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(g_vk.device, &fi, NULL, &g.sub[i].fence);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_vk.device, &si, NULL, &g.sub[i].acquire);
    }
    g.cur = -1;
    // descriptor layout: 0 = draw data, 1.. vertex textures, 8.. pixel textures
    VkDescriptorSetLayoutBinding b[2 + LS_VS_TEX_UNITS + LS_TEX_UNITS];
    u32 nb = 0;
    memset(b, 0, sizeof(b));
    b[nb].binding = 0;
    b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[nb].descriptorCount = 1;
    b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    nb++;
    b[nb].binding = LS_BINDING_STREAMOUT;
    b[nb].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[nb].descriptorCount = 1;
    b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    nb++;
    for (u32 i = 0; i < LS_VS_TEX_UNITS; i++, nb++) {
        b[nb].binding = LS_BINDING_VS_TEX + i;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].descriptorCount = 1;
        b[nb].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    for (u32 i = 0; i < LS_TEX_UNITS; i++, nb++) {
        b[nb].binding = LS_BINDING_PS_TEX + i;
        b[nb].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[nb].descriptorCount = 1;
        b[nb].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    if (g_vk.push_descriptors && nb > g_vk.max_push_desc) {
        // (38 descriptors a draw, the driver's limit is 32 (AMD): over it, pushing is undefined -- the sets path
        // instead. SPLAT_PUSH_OVER_LIMIT=1: push anyway, as before Oct 31)
        static int keep = -1;
        if (keep < 0) keep = getenv("SPLAT_PUSH_OVER_LIMIT") != NULL;
        printf("[render] %u descriptors a draw, the driver pushes at most %u: %s\n", nb, g_vk.max_push_desc, keep ? "pushed anyway (SPLAT_PUSH_OVER_LIMIT)" : "descriptor sets instead");
        if (!keep) g_vk.push_descriptors = false;
    }
    if (g_vk.push_descriptors) dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = nb;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.device, &dl, NULL, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g.layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.maxLod = 0.25f;
    vkCreateSampler(g_vk.device, &sci, NULL, &g.default_sampler);
    if (!make_dummy(&g.dummy[0], VK_IMAGE_TYPE_2D, 1, false) || !make_dummy(&g.dummy[1], VK_IMAGE_TYPE_2D, 1, false) ||
        !make_identity_lut(&g.dummy[2]) || !make_dummy(&g.dummy[3], VK_IMAGE_TYPE_2D, 6, true) ||
        !make_dummy(&g.dummy_1d, VK_IMAGE_TYPE_1D, 1, false))
        return false;
    g.tex_buf = NULL;
    {
        VkPipelineCacheCreateInfo pc = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        void* data = NULL;
        cache_dir_setup();
        FILE* f = fopen(CACHE("pipeline_cache.bin"), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (256 << 20) && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) == (size_t)n) {
                pc.initialDataSize = (size_t)n;
                pc.pInitialData = data;
            }
            fclose(f);
        }
        if (vkCreatePipelineCache(g_vk.device, &pc, NULL, &g.pcache) != VK_SUCCESS) {
            pc.initialDataSize = 0;   // (stale data from another driver version)
            pc.pInitialData = NULL;
            vkCreatePipelineCache(g_vk.device, &pc, NULL, &g.pcache);
        }
        free(data);
    }
    printf("[render] renderer ready\n");
    return true;
}

static bool ready(void) {
    if (g.ok) return true;
    if (g.tried) return false;
    int st = g_vk.state;
    if (st == 0) {
        if (!g_render_headless) return false;
        vkctx_create(NULL);   // no window: our own device
        st = g_vk.state;
    }
    g.tried = true;
    if (st != 1) {
        printf("[render] no graphics card connection, so nothing is drawn (the game keeps running)\n");
        return false;
    }
    g.ok = init();
    if (!g.ok) fprintf(stderr, "[render] renderer setup failed, nothing will be drawn\n");
    return g.ok;
}

static VkImageView dummy_view(u32 dim) {
    switch (dim) {
    case 0: return sample_view(&g.dummy_1d, VK_IMAGE_VIEW_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    case 2: return sample_view(&g.dummy[2], VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    case 3: return sample_view(&g.dummy[3], VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    case DIM_CUBE_ARRAY: return sample_view(&g.dummy[3], VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    case 4: return sample_view(&g.dummy_1d, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    case 5: case 7: return sample_view(&g.dummy[1], VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    default: return sample_view(&g.dummy[0], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 0x688);
    }
}

// --- Draws ---------------------------------------------------------------------------------------

// GX2 sets a color (target 0-7) or depth (8) buffer: remember its size and
// format by address, since the registers alone don't carry them.
static u32 g_rti_gen = 1;   // (changes whenever the table does: find_rti's cache)
void latte_render_rt_info(const u32* d) {
    bool depth = d[0] == 8;
    g_rti_gen++;
    u32 addr = d[1] & ~0x7FFu;
    u32 slot = 128;
    for (u32 i = 0; i < 128; i++)
        if (g.rti[i].used && g.rti[i].addr == addr && g.rti[i].depth == depth) { slot = i; break; }
    if (slot == 128) { slot = g.rti_next; g.rti_next = (g.rti_next + 1) & 127; }
    g.rti[slot].addr = addr;
    g.rti[slot].w = d[2];
    g.rti[slot].h = d[3];
    g.rti[slot].fmt = d[4];
    g.rti[slot].layers = (d[7] >> 8) & 0xFFF ? (d[7] >> 8) & 0xFFF : 1;
    g.rti[slot].dim = d[7] & 0xFF;
    g.rti[slot].tile = d[5];
    g.rti[slot].swizzle = d[6];
    g.rti[slot].depth = depth;
    g.rti[slot].used = true;
    {   // (SPLAT_RT_LOG=1: every registration, for a buffer that misbehaves)
        static int on = -1;
        if (on < 0) on = getenv("SPLAT_RT_LOG") != NULL;
        if (on) printf("[rti] %s buffer at %08X: %ux%u fmt %03X layers %u dim %u tile %u swizzle %08X\n", depth ? "depth" : "color", addr, d[2], d[3], d[4], g.rti[slot].layers, g.rti[slot].dim, d[5], d[6]);
    }
}

// (the look-ahead pass reads buffer-table packets too: the table is put back after it)
static u8 g_rti_save[sizeof(g.rti)];
static u32 g_rti_next_save, g_rti_gen_save;
void latte_render_rti_save(void) { memcpy(g_rti_save, g.rti, sizeof(g.rti)); g_rti_next_save = g.rti_next; g_rti_gen_save = g_rti_gen; }
void latte_render_rti_restore(void) {
    memcpy(g.rti, g_rti_save, sizeof(g.rti));
    g.rti_next = g_rti_next_save;
    g_rti_gen = (g_rti_gen > g_rti_gen_save ? g_rti_gen : g_rti_gen_save) + 1;   // (find_rti's remembered answers: all stale)
}

// (asked ~17 times a draw: the answers are remembered until the table
// changes -- going through all 128 entries each time was the drawing
// thread's busiest loop)
static int find_rti(u32 base, bool depth) {
    base &= ~0x7FFu;
    static struct { u32 base, gen; int idx; bool depth; } cache[64];
    u32 h = ((base >> 11) ^ (depth ? 0x25u : 0u)) & 63;
    if (cache[h].gen == g_rti_gen && cache[h].base == base && cache[h].depth == depth) {
        int i = cache[h].idx;
        if (i < 0 || (g.rti[i].used && g.rti[i].addr == base && g.rti[i].depth == depth)) return i;
    }
    int r = -1;
    for (u32 i = 0; i < 128; i++)
        if (g.rti[i].used && g.rti[i].addr == base && g.rti[i].depth == depth) { r = (int)i; break; }
    cache[h].base = base; cache[h].gen = g_rti_gen; cache[h].idx = r; cache[h].depth = depth;
    return r;
}

// --- Write-back of small buffers to game memory -------------------------------------------------
// Splatoon checks the ink under the players (and more) by drawing into small
// buffers in main memory and reading them with the CPU. On the console the
// GPU writes game memory directly; here the picture lives on the graphics
// card, so buffers that look like that (not in MEM1, up to 1 MB) are copied
// back -- tiled the way the game expects -- before the game is told the GPU
// is done (timestamps / fences).
#define RB_MAX 64
static Surf* g_rb[RB_MAX];
static u32 g_rb_count;
static bool g_rb_any;
static u32 texel_bytes(VkFormat f);

static void readback_note(Surf* s, int ri) {
    if (s->depth || s->vol || s->scale != 1.0f || s->scalex != 1.0f) return;   // (buffers drawn at render scale aren't the ones the game reads back)
    if (!s->rb_candidate) {
        if (s->tile == 0 && ri >= 0) { s->tile = g.rti[ri].tile; s->swizzle = g.rti[ri].swizzle; s->dim = g.rti[ri].dim; }
        if (s->addr >= 0xF4000000u || s->addr < 0x10000000u) return;   // MEM1 / not game data
        static int on = -1;   // (SPLAT_NO_READBACK=1 turns it off)
        if (on < 0) on = getenv("SPLAT_NO_READBACK") == NULL;
        if (!on) return;
        LatteTexFormat tf;
        if (!latte_tex_format(s->gx2fmt, &tf) || tf.block || tf.convert) return;
        u32 tb = texel_bytes(s->img.format);
        if (!tb || tb * 8 != tf.src_bits || tb > 2) return;   // (ink buffers are 8 / 8_8; not the lighting buffers)
        if ((u64)s->w * s->h * tb * s->layers > (1u << 20)) return;
        s->rb_candidate = true;
        static int logged = 0;
        if (logged++ < 16) printf("[render] buffer %08X %ux%u (format %03X) is copied back to game memory after drawing\n", s->addr, s->w, s->h, s->gx2fmt);
    }
    if (!s->rb_dirty) {
        if (g_rb_count < RB_MAX) { g_rb[g_rb_count++] = s; s->rb_dirty = true; g_rb_any = true; }
    }
}


// --- Occlusion queries -----------------------------------------------------------------------------
// A game query can span command buffers (we submit in between for stream out
// and readbacks); Vulkan queries can't, so each one is a chain of slots whose
// counts are added up.
#define QSLOTS 2048
#define MAXQ 256
typedef struct { u32 addr; u32 slot[16]; u32 nslot; bool active, ended; u64 serial; float scale, scalex; } GQuery;   // (scale: render scale of the buffer its draws went into)
static VkQueryPool g_qpool;
static u32 g_qnext;
static GQuery g_queries[MAXQ];
static u32 g_nq;
static int g_qcur = -1;           // the query running now
static bool g_qresult_pending;

static u32 query_slot(VkCommandBuffer cb) {
    u32 sl = g_qnext;
    g_qnext = (g_qnext + 1) % QSLOTS;
    vkCmdResetQueryPool(cb, g_qpool, sl, 1);
    vkCmdBeginQuery(cb, g_qpool, sl, g_vk.precise_queries ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    return sl;
}
static void query_suspend(VkCommandBuffer cb) {
    if (g_qcur < 0) return;
    GQuery* q = &g_queries[g_qcur];
    if (q->active) { vkCmdEndQuery(cb, g_qpool, q->slot[q->nslot - 1]); q->active = false; }
}
static void query_resume(VkCommandBuffer cb) {
    if (g_qcur < 0) return;
    GQuery* q = &g_queries[g_qcur];
    if (q->active || q->nslot >= 16) return;
    q->slot[q->nslot++] = query_slot(cb);
    q->active = true;
    q->serial = g.cur >= 0 ? g.sub[g.cur].serial : g_sub_serial;
}

// Results by query address (for conditional rendering: the newest known).
#define PRED_CACHE 1024
static struct { u32 addr; u64 total; } g_qlast[PRED_CACHE];
static bool g_pred_skip;   // conditional rendering says: skip the draws
static void qlast_set(u32 addr, u64 total) {
    u32 i = (addr >> 4) * 2654435761u >> 22;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & (PRED_CACHE - 1))
        if (!g_qlast[i].addr || g_qlast[i].addr == addr) { g_qlast[i].addr = addr; g_qlast[i].total = total; return; }
    g_qlast[(addr >> 4) * 2654435761u >> 22].addr = addr;
    g_qlast[(addr >> 4) * 2654435761u >> 22].total = total;
}
// The same, for the game threads (GX2QueryGetOcclusionResult): address and
// count packed into one 64-bit word, so a reader never sees half an update.
#define QDONE_N 2048
static volatile u64 g_qdone[QDONE_N];
static void qdone_set(u32 addr, u64 total) {
    u32 lo = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)total;
    u32 i = ((addr >> 4) * 2654435761u) >> 21;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & (QDONE_N - 1)) {
        u64 v = hatomic_load64(&g_qdone[i]);
        if (!v || (u32)(v >> 32) == addr) { hatomic_store64(&g_qdone[i], ((u64)addr << 32) | lo); return; }
    }
    hatomic_store64(&g_qdone[((addr >> 4) * 2654435761u) >> 21], ((u64)addr << 32) | lo);
}
bool latte_render_query_last(u32 addr, u32* count) {
    u32 i = ((addr >> 4) * 2654435761u) >> 21;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & (QDONE_N - 1)) {
        u64 v = hatomic_load64(&g_qdone[i]);
        if (!v) return false;
        if ((u32)(v >> 32) == addr) { *count = (u32)v; return true; }
    }
    return false;
}
static bool qlast_get(u32 addr, u64* total) {
    u32 i = (addr >> 4) * 2654435761u >> 22;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & (PRED_CACHE - 1))
        if (g_qlast[i].addr == addr) { *total = g_qlast[i].total; return true; }
    return false;
}
static void query_results(void);

// A query that can't be counted still gets an answer (0): the game waits
// for every query it starts before it starts that check again.
static void query_give_up(u32 addr) {
    if (!addr) return;
    wiiu_write32(addr + 8, 0);
    wiiu_write32(addr + 12, 0);
    wiiu_write32(addr, 0x51524459u);
}
void latte_render_query(u32 op, u32 addr) {
    if (!ready()) { if (op == 1) query_give_up(addr); return; }
    if ((op & 0xFF) == 3) {   // conditional rendering on
        query_results();
        u64 total = 0;
        bool known = qlast_get(addr, &total);
        bool draw_if_visible = (op & 0x100) != 0;
        // The console decides on the GPU with this very frame's count; here
        // only an older count is known, and things the camera has just
        // turned to (characters, enemies, scenery) vanished for a frame or
        // more. Draw them all (SPLAT_PREDICATION=1: skip like before).
        static int on = -1;
        if (on < 0) on = getenv("SPLAT_PREDICATION") != NULL;
        g_pred_skip = on && known && (draw_if_visible ? total == 0 : total != 0);
        return;
    }
    if ((op & 0xFF) == 4) { g_pred_skip = false; return; }
    if (!g_qpool) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = QSLOTS;
        if (vkCreateQueryPool(g_vk.device, &qi, NULL, &g_qpool) != VK_SUCCESS) { g_qpool = VK_NULL_HANDLE; if (op == 1) query_give_up(addr); return; }
    }
    {
        static int qlog = -1;
        if (qlog < 0) qlog = getenv("QLOG") != NULL;
        if (qlog) printf("[dbg] query %s %08X\n", op == 1 ? "begin" : op == 2 ? "end" : "?", addr);
    }
    if (op == 1) {
        if (g_qcur >= 0) latte_render_query(2, g_queries[g_qcur].addr);   // (one at a time, like GX2)
        if (g_nq >= MAXQ) { flush_wait(); latte_render_sync_readbacks(); }
        if (g_nq >= MAXQ) { query_give_up(addr); return; }
        GQuery* q = &g_queries[g_nq];
        memset(q, 0, sizeof(*q));
        q->addr = addr;
        g_qcur = (int)g_nq++;
        end_pass();
        VkCommandBuffer cb = cmd();   // (resumes the query when it starts a new command buffer)
        if (!q->active) query_resume(cb);
    } else if (op == 2) {
        if (g_qcur < 0 || g_queries[g_qcur].addr != addr) return;
        end_pass();
        GQuery* q = &g_queries[g_qcur];
        if (g.cur >= 0) query_suspend(g.sub[g.cur].cb);
        q->ended = true;
        g_qcur = -1;
        g_qresult_pending = true;
        g_rb_any = true;   // results are written once the graphics card has done them (no waiting)
    }
}

// Has the recording with this serial finished on the graphics card?
static bool serial_done(u64 serial) {
    for (u32 i = 0; i < NSUB; i++) {
        Sub* sb = &g.sub[i];
        if (sb->serial != serial) continue;
        if ((int)i == g.cur) return false;   // still being recorded
        if (!sb->busy) return true;
        return vkGetFenceStatus(g_vk.device, sb->fence) == VK_SUCCESS;
    }
    return true;   // (that recording's slot was reused long ago)
}

// Write the results of finished queries into game memory. Never waits.
static void query_results(void) {
    if (!g_qresult_pending) return;
    bool left = false;
    u32 keep = 0;
    for (u32 i = 0; i < g_nq; i++) {
        GQuery* q = &g_queries[i];
        if (!q->ended || !serial_done(q->serial)) { if (q->ended) left = true; g_queries[keep++] = *q; continue; }
        u64 total = 0;
        for (u32 k = 0; k < q->nslot; k++) {
            u64 v = 0;
            if (vkGetQueryPoolResults(g_vk.device, g_qpool, q->slot[k], 1, sizeof(v), &v, sizeof(v), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) total += v;
        }
        if ((q->scale > 1.0f || q->scalex > 1.0f) && total) {   // (render scale: the game expects its own pixel counts; never 0 for a non-zero count --
            double s2 = (double)q->scale * (q->scalex > 0.0f ? q->scalex : q->scale);   // the game skips the draws a query predicates when it saw nothing: a frozen world)
            u64 t = (u64)((double)total / s2);
            total = t ? t : 1;
        }
        qlast_set(q->addr, total);
        qdone_set(q->addr, total);
        wiiu_write32(q->addr + 8, (u32)(total >> 32));
        wiiu_write32(q->addr + 12, (u32)total);
        wiiu_write32(q->addr, 0x51524459u);   // ready
        static int logged = 0, qlog = -1;
        if (qlog < 0) qlog = getenv("QLOG") != NULL;
        if (logged++ < 8 || qlog) printf("[render] occlusion query %08X: %llu samples\n", q->addr, (unsigned long long)total);
    }
    g_qcur = -1;
    for (u32 i = 0; i < keep; i++) if (!g_queries[i].ended) g_qcur = (int)i;
    g_nq = keep;
    g_qresult_pending = left;
}

// Hand recorded work with finished queries to the graphics card without waiting.
static void flush_nowait(void);

// Copies on their way back from the graphics card (written into game memory
// once that recording has finished -- the game reads a frame-old picture
// of the ink, like it would on the console, and nobody waits for it).
#define RB_BATCHES 4
typedef struct {
    bool used;
    u64 serial;
    VkBuffer buf;
    VkDeviceMemory mem;
    u8* ptr;
    u32 size, n;
    Surf* s[RB_MAX];
    u32 off[RB_MAX];
} RbBatch;
static RbBatch g_rbb[RB_BATCHES];

static void rb_write_back(RbBatch* b) {
    u64 t0 = hnow_ns();
    g_ss.rb_batches++;
    static u32 nb = 0;   // (every 600th batch: what's in it and how long each buffer takes -- the ink maps the game reads back)
    bool show = (nb++ % 600) == 0;
    for (u32 i = 0; i < b->n; i++) {
        Surf* s = b->s[i];
        u32 tb = texel_bytes(s->img.format), bytes = s->w * s->h * tb * s->layers;
        u64 ts = show ? hnow_ns() : 0;
        g_ss.rb_surfs++;
        g_ss.rb_bytes += bytes;
        LatteTexFormat tf;
        latte_tex_format(s->gx2fmt, &tf);
        GX2Layout L;
        gx2_surface_layout(s->dim ? s->dim : 1, s->w, s->h, s->layers, 1, s->gx2fmt, 0, 2, s->tile, s->swizzle, &L);
        LatteTexDesc d = { s->addr, s->w, s->h, L.level[0].pitch, s->gx2fmt & 0x3F, s->tile, s->swizzle & 0x700, s->layers, L.level[0].height_pad };
        tf.elem_bytes = tb;
        // (not marked as changed: the GPU copy of this buffer is the newer one already)
        latte_tex_encode_slices(&d, &tf, 0, s->layers, b->ptr + b->off[i], bytes);
        if (show) printf("[render] readback %u: %08X %ux%u x%u layers, format %03X, tile mode %u, %u KB: %.2f ms\n", nb - 1, s->addr, s->w, s->h, s->layers,
                         s->gx2fmt, s->tile, bytes / 1024, (hnow_ns() - ts) / 1e6);
    }
    b->used = false;
    g_ss.rb_encode_ns += hnow_ns() - t0;
}

// Finished copies -> game memory. Never waits.
// A host buffer that the command buffer being recorded (or one still on the card) may name must not be
// destroyed yet: vkDeviceWaitIdle covers submitted work only. Hammerhead Bridge (Oct 31, the validation layer):
// a readback staging buffer grown in mid-frame was destroyed while the frame's command buffer already held
// copies into it -- the card hung on the freed memory. Such buffers wait here until the recording they were
// last named in has retired.
typedef struct { VkBuffer buf; VkDeviceMemory mem; u64 serial; } DeadBuf;
#define DEAD_MAX 64
static DeadBuf g_dead[DEAD_MAX];
static u32 g_ndead;
static void dead_collect(bool all) {
    for (u32 i = 0; i < g_ndead;) {
        if (all || serial_done(g_dead[i].serial)) {
            vkDestroyBuffer(g_vk.device, g_dead[i].buf, NULL);
            if (g_dead[i].mem) vkFreeMemory(g_vk.device, g_dead[i].mem, NULL);
            g_dead[i] = g_dead[--g_ndead];
        } else i++;
    }
}
static void buffer_destroy_later(VkBuffer buf, VkDeviceMemory mem) {
    if (!buf) { if (mem) vkFreeMemory(g_vk.device, mem, NULL); return; }
    if (g_ndead == DEAD_MAX) {
        vkDeviceWaitIdle(g_vk.device);
        dead_collect(false);   // (everything submitted is done now; what the current recording names stays)
        if (g_ndead == DEAD_MAX) {   // (not expected: 64 buffers replaced within one recording)
            static int warned = 0;
            if (warned++ < 2) printf("[render] %u buffers replaced within one recording: the oldest is freed early\n", DEAD_MAX);
            vkDestroyBuffer(g_vk.device, g_dead[0].buf, NULL);
            if (g_dead[0].mem) vkFreeMemory(g_vk.device, g_dead[0].mem, NULL);
            g_dead[0] = g_dead[--g_ndead];
        }
    }
    g_dead[g_ndead].buf = buf;
    g_dead[g_ndead].mem = mem;
    g_dead[g_ndead].serial = g.cur >= 0 ? g.sub[g.cur].serial : g_sub_serial;
    g_ndead++;
}

static void rb_collect(void) {
    dead_collect(false);
    for (u32 i = 0; i < RB_BATCHES; i++)
        if (g_rbb[i].used && serial_done(g_rbb[i].serial)) rb_write_back(&g_rbb[i]);
}

static void defer_run(const Deferred* e) {
    switch (e->kind) {
    case 0: {
        u8* dst = wiiu_mem_ptr(e->a, e->c);
        if (dst && g.so_ptr) { memcpy(dst, (u8*)g.so_ptr + e->b, e->c); wiiu_mem_mark(e->a, e->c); so_hash_note(dst, e->c); }
        break;
    }
    case 1: if (g_retire_fn) g_retire_fn(((u64)e->a << 32) | e->b); break;
    default:
        if (e->d) { wiiu_write32(e->a, e->c); wiiu_write32(e->a + 4, e->b); }
        else wiiu_write32(e->a, e->b);
        break;
    }
}
static void serial_wait(u64 serial, u64 timeout_ns) {
    for (u32 i = 0; i < NSUB; i++) {
        Sub* sb = &g.sub[i];
        if (sb->serial != serial) continue;
        if ((int)i == g.cur) flush_nowait();   // (never the case: entries are queued after a submit)
        if (sb->busy) gpu_lost_check(vkWaitForFences(g_vk.device, 1, &sb->fence, VK_TRUE, timeout_ns));
        return;
    }
}
// Run the queued work whose submissions are done (wait: all of it, waiting
// for the graphics card as needed).
static void defer_poll(bool wait) {
    while (g_def_n) {
        Deferred* e = &g_def[g_def_head];
        if (!serial_done(e->serial)) {
            if (!wait) return;
            u64 t0 = hnow_ns();
            serial_wait(e->serial, UINT64_MAX);
            g_pf[PF_SO] += hnow_ns() - t0;
            g_pf[PF_SO_COUNT]++;
            g_ss.so_waits++;
            g_ss.so_wait_ns += hnow_ns() - t0;
        }
        defer_run(e);
        g_def_head = (g_def_head + 1) % DEFER_MAX;
        g_def_n--;
    }
}
static void defer_push(u64 serial, u32 kind, u32 a, u32 b, u32 c, u32 d) {
    if (g_def_n == DEFER_MAX) defer_poll(true);
    Deferred* e = &g_def[(g_def_head + g_def_n) % DEFER_MAX];
    e->serial = serial; e->kind = kind; e->a = a; e->b = b; e->c = c; e->d = d;
    g_def_n++;
}
// Stream-out results so far: submitted, copied back once done.
static void so_defer(void) {
    if (!g_so_npend) return;
    u64 serial = g.cur >= 0 ? g.sub[g.cur].serial : g_sub_serial;
    flush_nowait();
    for (u32 i = 0; i < g_so_npend; i++) defer_push(serial, 0, g_so_pend[i].addr, g_so_pend[i].ring, g_so_pend[i].size, 0);
    g_so_npend = 0;
}
static bool fence_sync(void) {
    static int sync = -1;
    if (sync < 0) sync = getenv("SPLAT_FENCE_SYNC") != NULL || getenv("SPLAT_SO_SYNC") != NULL;
    return sync != 0;
}

void latte_render_fence(u64 ts, void (*retire)(u64 ts)) {
    gx2_cmd_note_fence_reached(ts);
    defer_copies_run();   // (the game flushed: the data of the draws before this is final)
    g_retire_fn = retire;
    if (!g.ok || fence_sync()) {
        latte_render_sync_readbacks();
        if (g.ok) defer_poll(true);
        retire(ts);
        return;
    }
    so_defer();
    latte_render_sync_readbacks();   // (the rest: query results, drawn buffers -- these never wait)
    if (g_def_n) { g_ss.fences_deferred++; defer_push(g_def[(g_def_head + g_def_n - 1) % DEFER_MAX].serial, 1, (u32)(ts >> 32), (u32)ts, 0, 0); }
    else retire(ts);
    // The game flushed, so what's recorded is final: hand it to the graphics
    // card now instead of with the whole frame at the swap. The card then
    // works while the rest of the frame is recorded, and by the time the game
    // waits for it (GX2DrawDone: the ink checks' results) little is left.
    // SPLAT_FLUSH_SUBMIT=N: only with at least N draws recorded (0 = never).
    {
        static int min_draws = -1, always = -1;
        if (min_draws < 0) { const char* e = getenv("SPLAT_FLUSH_SUBMIT"); min_draws = e ? atoi(e) : 64; always = getenv("SPLAT_SUBMIT_ALWAYS") != NULL; }
        if (min_draws > 0 && g.cur >= 0 && g.draws - g_cb_first_draw >= (u32)min_draws && !g_dc_n && (always || gx2_cmd_wait_pressure())) flush_nowait();
    }
    defer_poll(false);
}

void latte_render_mem_write(const u32* d, u32 n) {
    if (n < 4) { latte_render_sync_readbacks(); return; }
    u32 addr = d[0] & ~3u, wide = (d[0] & 2) ? 1 : 0;
    if (!g.ok || fence_sync()) {
        latte_render_sync_readbacks();
        if (g.ok) defer_poll(true);
        if (wide) { wiiu_write32(addr, d[3]); wiiu_write32(addr + 4, d[2]); } else wiiu_write32(addr, d[2]);
        return;
    }
    so_defer();
    latte_render_sync_readbacks();
    if (g_def_n) defer_push(g_def[(g_def_head + g_def_n - 1) % DEFER_MAX].serial, 2, addr, d[2], d[3], wide);
    else if (wide) { wiiu_write32(addr, d[3]); wiiu_write32(addr + 4, d[2]); }
    else wiiu_write32(addr, d[2]);
    defer_poll(false);
}

void latte_render_sync_readbacks(void) {
    if (!g.ok) return;
    so_sync();
    rb_collect();
    if (!g_rb_any) return;
    if (g_rb_count == 0) {   // queries only: submit, collect what's done, don't wait
        g_rb_any = false;
        if (g_qresult_pending) {
            bool in_cur = false;
            for (u32 i = 0; i < g_nq; i++)
                if (g_queries[i].ended && g.cur >= 0 && g_queries[i].serial == g.sub[g.cur].serial) in_cur = true;
            if (in_cur) flush_nowait();
            query_results();
            if (g_qresult_pending) g_rb_any = true;   // (check again later)
        }
        return;
    }
    g_rb_any = false;
    RbBatch* b = NULL;
    for (u32 i = 0; i < RB_BATCHES && !b; i++) if (!g_rbb[i].used) b = &g_rbb[i];
    if (!b) {   // all still on their way: skip this one (the next is a frame away)
        for (u32 i = 0; i < g_rb_count; i++) g_rb[i]->rb_dirty = false;
        g_rb_count = 0;
        query_results();
        return;
    }
    u32 total = 0;
    for (u32 i = 0; i < g_rb_count; i++) {
        Surf* s = g_rb[i];
        total += (s->w * s->h * texel_bytes(s->img.format) * s->layers + 255) & ~255u;
    }
    if (total > b->size) {
        if (b->buf) { buffer_destroy_later(b->buf, b->mem); b->buf = VK_NULL_HANDLE; }
        b->size = total + (1u << 20);
        if (!host_buffer_cached(b->size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b->buf, &b->mem, (void**)&b->ptr)) {
            b->size = 0;
            for (u32 i = 0; i < g_rb_count; i++) g_rb[i]->rb_dirty = false;
            g_rb_count = 0;
            return;
        }
    }
    VkCommandBuffer cb = transfer_cmd();
    barrier_all(cb);
    u32 off = 0;
    b->n = 0;
    for (u32 i = 0; i < g_rb_count; i++) {
        Surf* s = g_rb[i];
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = s->layers;
        c.imageExtent.width = s->w;
        c.imageExtent.height = s->h;
        c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, b->buf, 1, &c);
        b->s[b->n] = s;
        b->off[b->n] = off;
        b->n++;
        off += (s->w * s->h * texel_bytes(s->img.format) * s->layers + 255) & ~255u;
        s->rb_dirty = false;
    }
    b->serial = g.cur >= 0 ? g.sub[g.cur].serial : g_sub_serial;
    b->used = true;
    g_rb_count = 0;
    static int sync = -1;   // SPLAT_READBACK_SYNC=1: wait for it (the old way)
    if (sync < 0) sync = getenv("SPLAT_READBACK_SYNC") != NULL;
    if (sync) { flush_wait(); rb_collect(); }
    else flush_nowait();
    query_results();
}

static u32 tex_kind(u32 res_reg) {
    u32 w1 = R(res_reg + 1), w4 = R(res_reg + 4);
    u32 hw = (w1 >> 26) & 0x3F, numfmt = (w4 >> 8) & 3;
    bool float_fmt = hw == 0x06 || hw == 0x0E || hw == 0x10 || hw == 0x16 || hw == 0x1E || hw == 0x20 || hw == 0x23 || hw >= 0x31;
    if (numfmt == 1 && !float_fmt) return (w4 & 1) ? 1 : 2;
    return 0;
}

static void fill_tex_keys(LatteTexKey* out, u32 base_reg, u32 units) {
    for (u32 u = 0; u < units; u++) {
        u32 r = base_reg + u * 7;
        u32 w0 = R(r), w2 = R(r + 2), w6 = R(r + 6);
        if (!w2 && !w0) { out[u].dim = 0; out[u].kind = 0; continue; }
        (void)w6;
        out[u].dim = (u8)((w0 & 7) + 1);
        out[u].kind = (u8)tex_kind(r);
    }
}

// Why does a program get yet another translation? (logged for the first
// variants: which parts of the shader key differ from its first one)
static void variant_note(u64 vsh, u64 psh, const LatteShaderKey* k) {
    static struct { u64 vsh, psh; LatteShaderKey k; u32 n; } * tab;
    static u32 logged = 0;
    if (logged >= 60) return;
    if (!tab) tab = calloc(4096, sizeof(*tab));
    if (!tab) return;
    u32 i = (u32)((vsh * 31 + psh) >> 7) & 4095;
    if (tab[i].vsh != vsh || tab[i].psh != psh) { tab[i].vsh = vsh; tab[i].psh = psh; tab[i].k = *k; tab[i].n = 0; return; }
    const LatteShaderKey* a = &tab[i].k;
    char why[256] = "";
    #define DIFF(f) if (memcmp(&a->f, &k->f, sizeof(a->f))) { strcat(why, " " #f); }
    DIFF(vtx_semantic) DIFF(vs_out_semantic) DIFF(vte_cntl) DIFF(clip_cntl) DIFF(points) DIFF(vs_tex)
    DIFF(ps_input_cntl) DIFF(ps_in_control0) DIFF(ps_in_control1) DIFF(cb_shader_mask) DIFF(alpha_test) DIFF(cb_kind) DIFF(ps_tex)
    #undef DIFF
    if (!why[0]) return;
    tab[i].k = *k;
    if (++tab[i].n > 3) return;
    logged++;
    printf("[render] shader variant (vs %016llx ps %016llx) differs in:%s\n", (unsigned long long)vsh, (unsigned long long)psh, why);
}

static u32 prog_tex_mask(u64 h, const u8* prog, u32 size) {
    static struct { u64 h; u32 mask; } tab[8192];
    u32 i = (u32)(h ^ (h >> 29)) & 8191;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & 8191) {
        if (tab[i].h == h) return tab[i].mask;
        if (!tab[i].h) break;
    }
    u32 m = latte_shader_tex_mask(prog, size);
    tab[i].h = h;
    tab[i].mask = m;
    return m;
}

static u64 hash_key_vs(const LatteShaderKey* k, u64 vs_hash, u64 fs_hash) {
    u64 h = hash_bytes(&vs_hash, 8, fs_hash);
    h = hash_bytes(k->vtx_semantic, sizeof(k->vtx_semantic), h);
    h = hash_bytes(k->vs_out_semantic, sizeof(k->vs_out_semantic), h);
    u32 x[4] = { k->vte_cntl & 0x3F, (k->clip_cntl >> 19) & 1, k->points, k->ps_in_control0 & 0x7F3F };
    h = hash_bytes(x, sizeof(x), h);
    h = hash_bytes(k->ps_input_cntl, 4 * (k->ps_in_control0 & 0x3F), h);
    h = hash_bytes(k->vs_tex, sizeof(k->vs_tex), h);
    return h ^ 0x5653;
}

static u64 hash_key_ps(const LatteShaderKey* k, u64 ps_hash) {
    u64 h = hash_bytes(&ps_hash, 8, 0x5053);
    u32 x[5] = { k->ps_in_control0, k->ps_in_control1, k->cb_shader_mask, k->alpha_test & 0xF, 0 };
    h = hash_bytes(x, sizeof(x), h);
    h = hash_bytes(k->ps_input_cntl, 4 * (k->ps_in_control0 & 0x3F), h);
    h = hash_bytes(k->cb_kind, sizeof(k->cb_kind), h);
    h = hash_bytes(k->ps_tex, sizeof(k->ps_tex), h);
    return h;
}

static u32 bswap(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

// Indices: read (in whatever byte order), convert primitive types Vulkan
// lacks, write 32-bit indices. Returns count written.
// Converted index data kept on the graphics card between frames: the same
// models are drawn every frame from index data that doesn't change, and
// copying their converted indices into the upload memory for every draw was
// ~5 ms a frame with ~1,800 draws in the plaza. Blocks of 32 MB, filled in
// turn; when all 4 are used the oldest is emptied (its entries forgotten:
// they are converted again), after the graphics card is done with it.
#define ISTORE_BLOCK (32u << 20)
#define ISTORE_BLOCKS 8
typedef struct { VkBuffer buf; VkDeviceMemory mem; u8* ptr; u32 used, epoch; u64 last_serial; } IBlock;
static IBlock g_ib[ISTORE_BLOCKS];
static u32 g_ib_cur;
static int g_ib_on = -1;   // SPLAT_NO_INDEX_STORE=1: copy the indices for every draw, as before
static bool serial_done(u64 serial);
static void serial_wait(u64 serial, u64 timeout_ns);
static bool istore_alloc(u32 bytes, u32* blk, u32* off) {
    if (g_ib_on < 0) g_ib_on = getenv("SPLAT_NO_INDEX_STORE") == NULL;
    if (!g_ib_on || !bytes || bytes > ISTORE_BLOCK / 8) return false;
    IBlock* b = &g_ib[g_ib_cur];
    if (!b->buf || b->used + bytes > ISTORE_BLOCK) {
        u32 nb = b->buf ? (g_ib_cur + 1) % ISTORE_BLOCKS : g_ib_cur;
        IBlock* n = &g_ib[nb];
        if (!n->buf) {
            if (!host_buffer(ISTORE_BLOCK, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &n->buf, &n->mem, (void**)&n->ptr)) { n->buf = VK_NULL_HANDLE; return false; }
        } else {
            if (!serial_done(n->last_serial)) serial_wait(n->last_serial, UINT64_MAX);
            n->epoch++;   // (everything in it is forgotten)
        }
        n->used = 0;
        g_ib_cur = nb;
        b = n;
    }
    *blk = g_ib_cur;
    *off = b->used;
    b->used += (bytes + 255) & ~255u;
    b->last_serial = g.sub[g.cur].serial;
    return true;
}

// (converted index data remembered by address, count and kind: see build_indices)
typedef struct { u32 addr, count, key, m, lo, hi, gen, cap; bool restart; u8 head[32]; u32* data; u32 blk, boff, bep; u32 hits; } IdxEnt;
static u32 IDX_CACHE = 0;   // (entries: 32768; SPLAT_IDX_CACHE=N for tests -- was 8192, too few for the busy plaza)
static IdxEnt* icache;
static u64 g_ix[5];   // (per [perf] line: draws found in the index store, found converted in memory, converted, words converted, moved into the store)
static void icache_init(void) {
    if (icache) return;
    if (!IDX_CACHE) { const char* e = getenv("SPLAT_IDX_CACHE"); IDX_CACHE = e && atoi(e) >= 1024 ? (u32)atoi(e) : 32768; while (IDX_CACHE & (IDX_CACHE - 1)) IDX_CACHE &= IDX_CACHE - 1; }
    icache = (IdxEnt*)calloc(IDX_CACHE, sizeof(IdxEnt));
}
static u32 idx_key(const LatteDraw* dr, u32 prim, bool wide) {
    u32 reset_en = R(VGT_MULTI_PRIM_IB_RESET_EN) & 1, reset_idx = R(VGT_MULTI_PRIM_IB_RESET_INDX);
    return (wide ? 1u : 0u) | (prim << 1) | (reset_en << 8) | (reset_idx & 0xFFFF0000u ? 0x200u : 0u) | ((reset_idx & 0xFFFF) << 16);
}
// A draw whose converted indices are on the graphics card already (and the
// game's index data unchanged): where they are, how many, the vertex range.
static bool index_store_hit(const LatteDraw* dr, VkBuffer* buf, u32* off, u32* nidx, u32* vmin, u32* vmax, bool* restart) {
    if (!icache || dr->immediate || !dr->index_addr || dr->prim == PRIM_QUADS || dr->prim == PRIM_LINE_LOOP || dr->count < 16) return false;
    bool wide = dr->index_type == LATTE_INDEX_U32_BE || dr->index_type == LATTE_INDEX_U32_LE;
    bool be = dr->index_type == LATTE_INDEX_U16_BE || dr->index_type == LATTE_INDEX_U32_BE;
    if (!be) return false;
    u32 n = dr->count, bytes = n * (wide ? 4 : 2), ikey = idx_key(dr, dr->prim, wide);
    IdxEnt* ie = &icache[((dr->index_addr >> 2) ^ (n * 2654435761u) ^ ikey) & (IDX_CACHE - 1)];
    if (!ie->bep || ie->addr != dr->index_addr || ie->count != n || ie->key != ikey || ie->bep != g_ib[ie->blk].epoch + 1) return false;
    cmd();
    const u8* src = gmem(dr->index_addr, bytes);
    u32 hb = bytes < 32 ? bytes : 32;
    if (!src || memcmp(ie->head, src, hb) || wiiu_mem_changed_since(dr->index_addr, bytes, ie->gen)) return false;
    IBlock* b = &g_ib[ie->blk];
    b->last_serial = g.sub[g.cur].serial;
    *buf = b->buf;
    *off = ie->boff;
    *nidx = ie->m;
    *vmin = ie->lo;
    *vmax = ie->hi;
    *restart = ie->restart;
    if (ie->m) g_ix[0]++;
    return ie->m != 0;
}

static u32 build_indices(const LatteDraw* dr, u32 prim, u32* out, u32 cap, u32* vmin, u32* vmax, bool* restart) {
    u32 n = dr->count;
    bool wide = dr->index_type == LATTE_INDEX_U32_BE || dr->index_type == LATTE_INDEX_U32_LE;
    bool be = dr->index_type == LATTE_INDEX_U16_BE || dr->index_type == LATTE_INDEX_U32_BE;
    const u8* src = NULL;
    u8 tmp_words[8];
    (void)tmp_words;
    if (dr->index_addr) src = gmem(dr->index_addr, n * (wide ? 4 : 2));
    u32 reset_en = R(VGT_MULTI_PRIM_IB_RESET_EN) & 1, reset_idx = R(VGT_MULTI_PRIM_IB_RESET_INDX);
    *restart = false;
    u32 lo = 0xFFFFFFFF, hi = 0;
    // fetch index i
    #define IDX(i) get_index(dr, src, wide, be, (i))
    u32 m = 0;
    u32 quads = prim == PRIM_QUADS;
    u32 loop = prim == PRIM_LINE_LOOP;
    // The usual case -- big-endian indices in memory -- in a tight loop
    // (the general loop below was 17% of the drawing thread's time).
    // And remembered: the same models are drawn every frame from index data
    // that doesn't change, and converting it again each time was still 16%
    // of the drawing thread. Kept until the game writes that memory again
    // (DCFlushRange marks it) or its first bytes differ.
    IdxEnt* ie = NULL;
    u32 ikey = idx_key(dr, prim, wide);
    u32 hb = n * (wide ? 4 : 2) < 32 ? n * (wide ? 4 : 2) : 32;
    if (!dr->immediate && src && be && n <= cap && n >= 16) {
        if (!icache) icache_init();
        if (icache) {
            ie = &icache[((dr->index_addr >> 2) ^ (n * 2654435761u) ^ ikey) & (IDX_CACHE - 1)];
            if (ie->data && ie->addr == dr->index_addr && ie->count == n && ie->key == ikey && !memcmp(ie->head, src, hb) &&
                !wiiu_mem_changed_since(dr->index_addr, n * (wide ? 4 : 2), ie->gen)) {
                memcpy(out, ie->data, (size_t)ie->m * 4);
                g_ix[1]++;
                m = ie->m;
                *restart = ie->restart;
                lo = ie->lo;
                hi = ie->hi;
                n = 0;   // (nothing left to convert)
                // (kept here because its memory had been written when it was converted -- a scene loading its
                // models over the last one's: back in the plaza, ~650 draws a frame then copied their indices
                // every frame for the rest of the session. Unchanged for a few draws: into the store after all.)
                if (++ie->hits >= 4 && prim != PRIM_QUADS && prim != PRIM_LINE_LOOP && ie->m) {
                    u32 blk = 0, boff = 0;
                    if (istore_alloc(ie->m * 4, &blk, &boff)) {
                        memcpy(g_ib[blk].ptr + boff, ie->data, (size_t)ie->m * 4);
                        free(ie->data);
                        ie->data = NULL;
                        ie->cap = 0;
                        ie->blk = blk; ie->boff = boff; ie->bep = g_ib[blk].epoch + 1;
                        g_ix[4]++;
                    }
                }
                ie = NULL;
            }
        }
    }
    u32 igen = wiiu_mem_generation();
    if (n) { g_ix[2]++; g_ix[3] += n; }
    if (n && !dr->immediate && src && be) {
        bool list = prim == PRIM_POINTS || prim == PRIM_LINES || prim == PRIM_TRIANGLES || prim == PRIM_QUADS;
        u32 lim = n < cap ? n : cap;
        u32 lo_ = 0xFFFFFFFF, hi_ = 0;
        if (wide) {
            for (u32 i = 0; i < lim; i++) {
                u32 w;
                memcpy(&w, src + (size_t)i * 4, 4);
                u32 v = bswap(w);
                if (reset_en && v == reset_idx) { if (!list) { out[m++] = 0xFFFFFFFF; *restart = true; } continue; }
                lo_ = v < lo_ ? v : lo_;
                hi_ = v > hi_ ? v : hi_;
                out[m++] = v;
            }
        } else {
            for (u32 i = 0; i < lim; i++) {
                u32 v = ((u32)src[i * 2] << 8) | src[i * 2 + 1];
                if (reset_en && v == reset_idx) { if (!list) { out[m++] = 0xFFFFFFFF; *restart = true; } continue; }
                lo_ = v < lo_ ? v : lo_;
                hi_ = v > hi_ ? v : hi_;
                out[m++] = v;
            }
        }
        lo = lo_;
        hi = hi_;
        n = 0;   // (skip the general loop)
        if (ie) {   // (remember it: on the graphics card when draws can use it from there, else here)
            u32 blk = 0, boff = 0;
            // (index data the game rewrites -- the same entry, changed since --
            // stays out of the store: it would fill it with copies every frame)
            bool rewritten = ie->addr == dr->index_addr && ie->count == dr->count && ie->key == ikey &&
                             (memcmp(ie->head, src, hb) || wiiu_mem_changed_since(dr->index_addr, dr->count * (wide ? 4 : 2), ie->gen));
            bool gpu = !rewritten && prim != PRIM_QUADS && prim != PRIM_LINE_LOOP && m && istore_alloc(m * 4, &blk, &boff);
            if (gpu) {
                memcpy(g_ib[blk].ptr + boff, out, (size_t)m * 4);
                free(ie->data);
                ie->data = NULL;
                ie->cap = 0;
                ie->blk = blk; ie->boff = boff; ie->bep = g_ib[blk].epoch + 1;
            } else {
                ie->bep = 0;
                if (ie->cap < m) { free(ie->data); ie->data = (u32*)malloc((size_t)m * 4); ie->cap = ie->data ? m : 0; }
                if (ie->data) memcpy(ie->data, out, (size_t)m * 4);
            }
            if (gpu || ie->data) {
                memcpy(ie->head, src, hb);
                ie->addr = dr->index_addr; ie->count = dr->count; ie->key = ikey;
                ie->m = m; ie->lo = lo; ie->hi = hi; ie->restart = *restart; ie->gen = igen; ie->hits = 0;
            } else ie->addr = 0;
        }
    }
    for (u32 i = 0; i < n && m < cap; i++) {
        u32 v;
        if (dr->immediate) {
            u32 wi = wide ? i : i / 2;
            if (wi >= dr->immediate_words) break;
            u32 word = dr->immediate[wi];   // as read big-endian from memory
            u8 b[4] = { (u8)(word >> 24), (u8)(word >> 16), (u8)(word >> 8), (u8)word };
            if (wide) v = be ? ((u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]) : ((u32)b[3] << 24 | (u32)b[2] << 16 | (u32)b[1] << 8 | b[0]);
            else {
                const u8* p = b + (i & 1) * 2;
                v = be ? ((u32)p[0] << 8 | p[1]) : ((u32)p[1] << 8 | p[0]);
            }
        } else if (src) {
            if (wide) { u32 w; memcpy(&w, src + i * 4, 4); v = be ? bswap(w) : w; }
            else { const u8* p = src + i * 2; v = be ? ((u32)p[0] << 8 | p[1]) : ((u32)p[1] << 8 | p[0]); }
        } else {
            v = i;
        }
        if (reset_en && v == reset_idx) {
            bool list = prim == PRIM_POINTS || prim == PRIM_LINES || prim == PRIM_TRIANGLES || prim == PRIM_QUADS;
            if (!list) { out[m++] = 0xFFFFFFFF; *restart = true; }
            continue;
        }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        out[m++] = v;
    }
    #undef IDX
    if (quads) {   // 0 1 2 3 -> 0 1 2, 0 2 3
        u32 nq = m / 4;
        u32* tmp = (u32*)malloc(nq * 6 * 4 + 4);
        for (u32 q = 0; q < nq; q++) {
            u32* s = out + q * 4;
            tmp[q * 6 + 0] = s[0]; tmp[q * 6 + 1] = s[1]; tmp[q * 6 + 2] = s[2];
            tmp[q * 6 + 3] = s[0]; tmp[q * 6 + 4] = s[2]; tmp[q * 6 + 5] = s[3];
        }
        m = nq * 6 <= cap ? nq * 6 : 0;
        memcpy(out, tmp, m * 4);
        free(tmp);
    } else if (loop && m > 1 && m < cap) {
        out[m] = out[0];
        m++;
    }
    if (lo == 0xFFFFFFFF) lo = hi = 0;
    *vmin = lo;
    *vmax = hi;
    return m;
}

static VkPrimitiveTopology topology(u32 prim, u32* cls) {
    switch (prim) {
    case PRIM_POINTS: *cls = 0; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_LINES: *cls = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_LINE_STRIP: case PRIM_LINE_LOOP: *cls = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TRIANGLE_FAN: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TRIANGLE_STRIP: case PRIM_QUAD_STRIP: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // triangles, quads (converted), rects
    }
}

static void begin_pass(VkImageView* colors, VkFormat* cfmt, u32 ncolor, VkImageView depth, VkFormat dfmt, bool stencil, u32 w, u32 h) {
    bool same = g.in_pass && g.pass_ncolor == ncolor && g.pass_depth == depth && g.pass_w == w && g.pass_h == h;
    for (u32 i = 0; same && i < ncolor; i++) same = g.pass_color[i] == colors[i];
    if (same) return;
    end_pass();
    VkCommandBuffer cb = cmd();
    if (g.need_barrier) { barrier_all(cb); g.need_barrier = false; }
    VkRenderingAttachmentInfo ca[8];
    memset(ca, 0, sizeof(ca));
    for (u32 i = 0; i < ncolor; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = colors[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        g.pass_color[i] = colors[i];
        g.pass_cfmt[i] = cfmt[i];
    }
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = depth;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachments = ca;
    if (depth) {
        ri.pDepthAttachment = &da;
        if (stencil) ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    g.in_pass = true;
    g.pass_ncolor = ncolor;
    g.pass_depth = depth;
    g.pass_dfmt = dfmt;
    g.pass_stencil = stencil;
    g.pass_w = w;
    g.pass_h = h;
}

static u32 g_unsupported_gs = 0;
// Where the GPU thread's time goes (printed every 300 frames).

// 2 = unsigned integer, 1 = signed integer, 0 = float/normalized format.
static int format_int_kind(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return 2;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT:
        return 1;
    case VK_FORMAT_UNDEFINED:
        return -1;
    default:
        return 0;
    }
}

// Was this buffer also drawn in the previous frame?
// The screen's own buffers: the TV's 1280x720, the GamePad's 854x480 and the
// halves down the bloom chain -- drawn anew every frame.
static bool screen_sized(const Surf* s) {
    if (s->vol || s->layers > 1) return false;
    u32 w = s->w, h = s->h;
    return (w == 1280 && h == 720) || (w == 854 && h == 480) || (w == 640 && h == 360) || (w == 320 && h == 180) ||
           (w == 160 && h == 90) || (w == 80 && h == 45) || (w == 427 && h == 240);
}
#define GUESS_MAX 64
static Surf* g_guess[GUESS_MAX]; static u64 g_guess_frame[GUESS_MAX]; static u32 g_guess_n;
static u32 g_guess_wrong, g_guess_right;
static bool note_recurring(Surf* s) {
    if (!s->depth) { if (s->w >= 960) g_ss.scene_draws++; else g_ss.other_draws++; }   // (the game's sizes: 1280-wide buffers are the scene)
    if (s->seen != g.frame + 1) {
        s->recurring = s->seen == g.frame;   // (last drawn in frame g.frame - 1)
        // (never_stop: a screen buffer seen for the first time will be drawn
        // every frame from now on -- its draws may wait for their builds like
        // the rest. At a new scene's first frame every buffer is new, and all
        // its draws were built one by one on the drawing thread: 40% of the
        // cold plaza's 15 s first frame went into buffers drawn again the
        // next frame. Checked a frame later: see guess_check.)
        if (!s->recurring && !s->seen && never_stop() && screen_sized(s)) {
            s->recurring = s->guessed = true;
            s->guess_skips = 0;
            if (g_guess_n < GUESS_MAX) { g_guess[g_guess_n] = s; g_guess_frame[g_guess_n++] = g.frame; }
        }
        s->seen = g.frame + 1;
    }
    return s->recurring;
}
// (at the end of a frame: buffers guessed two frames back -- drawn again in the frame just done?)
static void guess_check(void) {
    u32 k = 0;
    for (u32 i = 0; i < g_guess_n; i++) {
        Surf* s = g_guess[i];
        if (g_guess_frame[i] + 2 > g.frame) { g_guess[k] = s; g_guess_frame[k++] = g_guess_frame[i]; continue; }
        if (s->seen == g.frame) g_guess_right++;
        else {
            g_guess_wrong++;
            static int shown = 0;
            if (shown++ < 20) printf("[render] screen-sized buffer %ux%u fmt %03X at %08X was drawn only once (%u of its draws had been left out while their pipelines were built)\n",
                                     s->w, s->h, s->gx2fmt, s->addr, s->guess_skips);
        }
        s->guessed = false;
    }
    g_guess_n = k;
}

static bool g_cap_request = false;   // capture the next frame
static int g_cap_more = 0;           // then this many more frames right after


// A shader from the disk cache (NULL if it isn't there).
static ShaderEnt* cached_shader(bool fragment, u64 key) {
    for (ShaderEnt* e = g.shaders[key & (LR_BUCKETS - 1)]; e; e = e->next)
        if (e->key == key) return e;
    shader_cache_load();
    for (DiskShader* d = g_disk_shaders[(key ^ (fragment ? 1 : 0)) & 4095]; d; d = d->next)
        if (d->key == key && (d->fragment & 1) == (fragment ? 1 : 0)) return get_shader(fragment, key, NULL);
    return NULL;
}

// The warm-up's progress picture: a dark window with two lines of text and a bar, shown while the pipelines build
// (the game hasn't drawn its first picture yet, so this is the only thing on the screen). Its own small submit:
// the current recording goes out with it (it holds nothing of the game's yet).
static bool g_warm_done;   // (every known pipeline was built before the game's first picture)
static bool g_warm_all_done;   // (the start-up warm-up covered every scene: the per-scene queue at a load has nothing new then)
static void warm_present(const char* l1, const char* l2, float frac);   // (below: it needs the text-bar helpers)
static volatile const char* g_wp_step;   // (the warm-up picture: the step the drawing thread is in -- the [stall] report shows it)
static volatile u64 g_wp_since;
static void wp_step(const char* s) { u64 now = hnow_ns(); static u64 last; static const char* prev; static int said;
    if (prev && now - last > 200000000ull && said < 40) { said++; printf("[render] start-up picture: %s took %.0f ms\n", prev, (now - last) / 1e6); }
    prev = s; last = now; g_wp_step = s; g_wp_since = now; }

// Before the first picture: build every pipeline listed in pipeline_keys.bin (on the helper threads). With a warm
// driver cache that takes under a second and nothing shows; with a cold one (a fresh install, the driver's cache
// cleared) a progress picture stays up until they're all built -- a minute or two once, instead of draws blinking in
// and out for the first visit to every place. Settings "Shader warm-up" (warm_shaders; SPLAT_WARM=0/1, the old
// SPLAT_PRELOAD=1 too).
// A scene's pipelines (the records tagged with its map) queued on the helper threads as look-ahead entries (a helper
// compiles the two shaders' modules if nobody has yet, then builds the pipeline; pf_sweep keeps the ones no draw
// took). warm_left() = how many are still building. Returns how many were queued.
static u32 g_warm_n;
static char g_warm_scene[64];
static u64 g_warm_t0;
static PrefEnt* pf_find(ShaderEnt* vs, ShaderEnt* ps, const PipeKey* partial, u64 h);
static u64 pf_hash(const PipeKey* k);
static void async_queue_pf(PrefEnt* f);
// Battle maps share players, gear, weapons, ink and screen effects, but each map's list only holds what was drawn
// there: a third to a half of the pipelines a bot match built new (Nov 10, 13 maps) were already listed under another
// battle map -- and with them the full-screen effect passes that can't be left out (built on the drawing thread:
// 100-330 ms hitches). So a battle map's load also queues every pipeline listed under two or more battle maps
// (~1,700 of 19,000; the ones that turn up again). SPLAT_BATTLE_SHARED=0: off.
static const char* const k_battle_scenes[] = { "Crank00", "Crank01", "Warehouse00", "SeaPlant00", "UpDown00", "SkatePark00", "Athletic00",
    "Amida00", "Tuzura00", "Ruins00", "Kaisou00", "Jyoheki00", "Pivot00", "Office00", "Maze00" };
static int battle_scene_index(const char* s) {
    for (u32 i = 0; i < sizeof(k_battle_scenes) / sizeof(k_battle_scenes[0]); i++) if (!strcmp(s, k_battle_scenes[i])) return (int)i;
    return -1;
}
static u8* g_pk_shared;   // per record: listed under 2+ battle maps
static u32 g_pk_shared_n, g_pk_shared_for;
static void battle_shared_mark(void) {
    if (g_pk_shared && g_pk_shared_for == g_pk_nrec) return;
    free(g_pk_shared);
    g_pk_shared = (u8*)calloc(g_pk_nrec ? g_pk_nrec : 1, 1);
    g_pk_shared_for = g_pk_nrec;
    g_pk_shared_n = 0;
    u32 cap = 1; while (cap < g_pk_nrec * 2 + 16) cap <<= 1;
    struct { u64 h; u32 maps; }* tab = calloc(cap, sizeof(*tab));
    u64* hs = (u64*)calloc(g_pk_nrec ? g_pk_nrec : 1, 8);
    if (!tab || !hs || !g_pk_shared) { free(tab); free(hs); return; }
    for (int pass = 0; pass < 2; pass++)
        for (u32 i = 0; i < g_pk_nrec; i++) {
            const PipeRec* r = &g_pk_recs[i];
            int bi = battle_scene_index(r->scene);
            if (bi < 0) continue;
            if (pass == 0) {
                PipeKey pk = r->k;
                pk.vs = VK_NULL_HANDLE;
                pk.ps = VK_NULL_HANDLE;
                u64 h = hash_bytes(&pk, sizeof(pk), hash_bytes(&r->vs, 16, 0x4253)) | 1;
                hs[i] = h;
            }
            u32 j = (u32)(hs[i] * 0x9E3779B97F4A7C15ull >> 40) & (cap - 1);
            while (tab[j].h && tab[j].h != hs[i]) j = (j + 1) & (cap - 1);
            tab[j].h = hs[i];
            if (pass == 0) tab[j].maps |= 1u << bi;
            else if (tab[j].maps & (tab[j].maps - 1)) { g_pk_shared[i] = 1; g_pk_shared_n++; }   // (2+ bits)
        }
    free(tab);
    free(hs);
}
static u32 warm_scene_queue(const char* scene) {
    g_warm_n = 0;
    if (!g_pk_nrec || !scene || !scene[0]) return 0;
    snprintf(g_warm_scene, sizeof(g_warm_scene), "%s", scene);
    g_warm_t0 = hnow_ns();
    u32 queued = 0;
    bool all = !strcmp(scene, "*");
    static int shared_on = -1;
    if (shared_on < 0) { const char* e = getenv("SPLAT_BATTLE_SHARED"); shared_on = !e || atoi(e) != 0; }
    bool battle = !all && shared_on && battle_scene_index(scene) >= 0;
    if (battle) battle_shared_mark();
    bool gl_scene = !all && battle_scene_index(scene) >= 0;
    for (u32 pass = 0; pass < (all || battle ? 2u : 1u); pass++)
    for (u32 i = 0; i < g_pk_nrec && queued < 25000; i++) {   // (memory: a pipeline holds ~45 KB in the driver)
        const PipeRec* r = &g_pk_recs[i];
        if (all) { bool plaza = !strcmp(r->scene, "Plaza00"); if (plaza != (pass == 0)) continue; }   // (the plaza's first: the game starts there)
        else if (pass == 1) { if (!g_pk_shared || i >= g_pk_shared_for || !g_pk_shared[i] || !strcmp(r->scene, scene)) continue; }   // (battle maps: then the shared ones)
        else if (strcmp(r->scene, scene)) continue;
        ShaderEnt* v = cached_shader(false, r->vs);
        ShaderEnt* ps = cached_shader(true, r->ps);
        if (!v || !ps || v->failed || ps->failed) continue;
        if (gl_scene) gl_scene_parts(&r->k, v, ps);   // (battle maps: pipeline parts for its shaders -- new pairings during a match link at once)
        PipeKey pk = r->k;
        pk.vs = VK_NULL_HANDLE;
        pk.ps = VK_NULL_HANDLE;
        if (!hatomic_load64(&v->pending) && !hatomic_load64(&ps->pending) && v->mod && ps->mod) {
            PipeKey fk = pk;
            fk.vs = v->mod;
            fk.ps = ps->mod;
            if (pipe_lookup(&fk)) continue;   // (built already)
        }
        u64 h = pf_hash(&pk);
        if (pf_find(v, ps, &pk, h)) continue;   // (the look-ahead has it)
        PrefEnt* f = (PrefEnt*)calloc(1, sizeof(PrefEnt));
        f->vs = v;
        f->ps = ps;
        f->k = pk;
        f->hash = h;
        f->pending = 1;
        f->warm = true;
        f->next = g_pf_tab[h & (PF_BUCKETS - 1)];
        g_pf_tab[h & (PF_BUCKETS - 1)] = f;
        f->all = g_pf_all;
        g_pf_all = f;
        g_pf_n++;
        async_queue_pf(f);
        queued++;
    }
    g_warm_n = queued;
    return queued;
}
static u32 warm_left(void) {
    u32 left = 0;
    for (PrefEnt* f = g_pf_all; f; f = f->all) left += f->warm && hatomic_load64(&f->pending) ? 1 : 0;
    return left;
}
// (benchmark.c: shaders + pipelines built so far on any thread, and the warm-up entries still pending -- a map is
// settled when neither moves for a while)
u64 latte_builds_so_far(u32* warm_pending) { if (warm_pending) *warm_pending = warm_left(); return hatomic_load64(&g_async_built) + g_now_builds; }
static void preload_pipelines(void) {
    static bool done = false;
    if (done) return;
    done = true;
    shader_cache_load();
    PipeRec* recs = NULL;
    u32 n = 0, cap = 0;
    bool ok = false, rewrite = false;
    cache_dir_setup();
    FILE* f = fopen(CACHE("pipeline_keys.bin"), "rb");
    if (f) {
        u32 ver = 0;
        setvbuf(f, NULL, _IOFBF, 1u << 20);   // (before the first read: set after it, the C library dropped what it had read ahead and every record came out shifted -- garbage, which is why "107,000 different pipelines" were listed)
        if (fread(&ver, 4, 1, f) == 1 && (ver == PIPE_KEYS_VERSION || ver == PIPE_KEYS_VERSION_V2)) {
            ok = true;
            PipeRec r;
            u32 total = 0, dropped = 0;
            for (;;) {
                memset(&r, 0, sizeof(r));
                if (ver == PIPE_KEYS_VERSION_V2) { PipeRecV2 r2; if (fread(&r2, sizeof(r2), 1, f) != 1) break; r.vs = r2.vs; r.ps = r2.ps; r.k = r2.k; }
                else if (fread(&r, sizeof(r), 1, f) != 1) break;
                total++;
                if (!r.scene[0]) { dropped++; continue; }   // (listed before the scenes were: unusable -- and the v2 file was read shifted, so those are garbage too)
                if (!pk_add(hash_bytes(&r, sizeof(r), 13))) continue;   // (a repeat)
                if (n == cap) { cap = cap ? cap * 2 : 4096; recs = (PipeRec*)realloc(recs, cap * sizeof(PipeRec)); }
                recs[n++] = r;
            }
            fclose(f);
            f = NULL;
            rewrite = ver == PIPE_KEYS_VERSION_V2 || dropped > 0 || (total > 1000 && total - n > n / 4);   // (an old file, old records, or mostly repeats)
            if (rewrite) {
                FILE* o = fopen(CACHE("pipeline_keys.tmp"), "wb");
                u32 v = PIPE_KEYS_VERSION;
                bool w = o && fwrite(&v, 4, 1, o) == 1 && fwrite(recs, sizeof(PipeRec), n, o) == n;
                if (o) w = fclose(o) == 0 && w;
                if (w) { remove(CACHE("pipeline_keys.bin")); w = rename(CACHE("pipeline_keys.tmp"), CACHE("pipeline_keys.bin")) == 0; }
                printf("[render] pipeline_keys.bin: %u records, %u kept (%u without a scene dropped) -- %s\n", total, n, dropped, w ? "written again" : "couldn't rewrite it");
            }
        }
        if (f) fclose(f);
    }
    g_pipe_keys_out = fopen(CACHE("pipeline_keys.bin"), ok ? "ab" : "wb");
    if (g_pipe_keys_out && !ok) { u32 v = PIPE_KEYS_VERSION; fwrite(&v, 4, 1, g_pipe_keys_out); fflush(g_pipe_keys_out); }
    g_pk_recs = recs;
    g_pk_nrec = n;
    g_pk_cap_recs = cap;
    {
        u32 tagged = 0, plaza = 0;
        for (u32 i = 0; i < n; i++) {
            tagged += recs[i].scene[0] != 0;
            plaza += strcmp(recs[i].scene, "Plaza00") == 0;
        }
        printf("[render] %u pipelines known by scene (%u the plaza's)\n", tagged, plaza);
    }
    int warm = g_vk.warm_shaders;
    {
        const char* e = getenv("SPLAT_WARM");
        if (e) warm = atoi(e) != 0;
        else if (getenv("SPLAT_PRELOAD")) warm = 1;
    }
    if (!warm) { printf("[render] the shader warm-up is off: pipelines are built when first needed\n"); return; }
    // every known pipeline (the plaza's first, the game starts there): built before the first picture -- quietly for a
    // second (a warm driver cache is done by then), then with the progress picture up to fifteen minutes (a cold one:
    // ~13,000 pipelines at ~100 ms each over the helper threads). The user's story level dropped to 26-35 fps for ten
    // seconds and froze 4 s on its first visit after a driver-cache wipe: that building now happens here.
    u64 t0 = hnow_ns();
    u32 queued = warm_scene_queue("*");
    if (!queued) { printf("[render] shader warm-up: nothing listed yet (it fills in as you play)\n"); return; }
    printf("[render] shader warm-up: %u known pipelines (every place seen so far)...\n", queued);
    u64 last = t0, last_pic = 0;
    u32 left = 0;
    bool shown = false;
    for (;;) {
        left = warm_left();
        u64 now = hnow_ns();
        if (!left) break;
        if (now - t0 > 900000000000ull) { printf("[render]   (%u still building in the background after 15 minutes)\n", left); break; }
        if (now - t0 > 1000000000ull && now - last_pic > 100000000ull) {
            last_pic = now;
            shown = true;
            char l1[96], l2[96];
            snprintf(l1, sizeof(l1), "Preparing graphics  %u / %u", queued - left, queued);
            snprintf(l2, sizeof(l2), "(every place's shaders, once after an install or a driver change: %.0f s)", (now - t0) / 1e9);
            warm_present(l1, l2, queued ? (float)(queued - left) / (float)queued : 1.0f);
        }
        if (now - last > 2000000000ull) { last = now; printf("[render]   %u of %u built\n", queued - left, queued); }
        hsleep_ns(shown ? 50000000 : 10000000);
    }
    g_warm_done = left == 0;
    printf("[render] shader warm-up: %u pipelines%s (%.1f s%s)\n", queued, left ? " queued" : " built", (hnow_ns() - t0) / 1e9, shown ? ", with the progress picture" : "");
    g_warm_all_done = left == 0;
}
// A new scene (scene_place.c saw its map open): its pipelines queued now, while the game's loading screen is up; the
// overlay (draw_warm) shows the count until they're done.
static void warm_scene_check(void) {
    extern volatile unsigned splat_scene_seq;
    extern char splat_scene_map[64];
    static unsigned seen;
    if (splat_scene_seq == seen) return;
    seen = splat_scene_seq;
    if (!g_vk.warm_shaders || !g_pk_nrec) return;
    u32 q = warm_scene_queue(splat_scene_map);
    if (q) printf("[render] shader warm-up: %u pipelines of %s queued at its load%s\n", q, splat_scene_map,
                  battle_scene_index(splat_scene_map) >= 0 && g_pk_shared ? " (with the ones listed under 2+ battle maps)" : "");
}

// A draw's shader key (its programs and the registers their translation
// depends on) -- one function for the draw itself and for the look-ahead, so
// the look-ahead asks for exactly what the draw will.
static bool shader_key_for(const LatteDraw* dr, LatteShaderKey* k, u64* vsh_out, u64* psh_out, u64* fsh_out) {
    memset(k, 0, sizeof(*k));
    u32 vs_addr = R(SQ_PGM_START_VS) << 8, vs_size = R(SQ_PGM_START_VS + 1) << 3;
    u32 ps_addr = R(SQ_PGM_START_PS) << 8, ps_size = R(SQ_PGM_START_PS + 1) << 3;
    u32 fs_addr = R(SQ_PGM_START_FS) << 8, fs_size = R(SQ_PGM_START_FS + 1) << 3;
    if (!vs_addr || !ps_addr || !vs_size || !ps_size) { DBG("draw skipped: no program (vs %08X+%X ps %08X+%X)\n", vs_addr, vs_size, ps_addr, ps_size); return false; }
    k->vs = gmem(vs_addr, vs_size);
    k->vs_size = vs_size;
    k->ps = gmem(ps_addr, ps_size);
    k->ps_size = ps_size;
    if (fs_addr && fs_size) { k->fs = gmem(fs_addr, fs_size); k->fs_size = fs_size; }
    if (!k->vs || !k->ps) { DBG("draw skipped: program outside memory (vs %08X+%X ps %08X+%X)\n", vs_addr, vs_size, ps_addr, ps_size); return false; }
    for (u32 i = 0; i < 32; i++) k->vtx_semantic[i] = (u8)(R(SQ_VTX_SEMANTIC_0 + i) & 0xFF);
    for (u32 i = 0; i < 40; i++) k->vs_out_semantic[i] = (u8)((R(SPI_VS_OUT_ID_0 + i / 4) >> (8 * (i % 4))) & 0xFF);
    k->vte_cntl = R(PA_CL_VTE_CNTL);
    k->clip_cntl = R(PA_CL_CLIP_CNTL);
    k->points = dr->prim == PRIM_POINTS;
    for (u32 i = 0; i < 32; i++) k->ps_input_cntl[i] = R(SPI_PS_INPUT_CNTL_0 + i);
    k->ps_in_control0 = R(SPI_PS_IN_CONTROL_0);
    k->ps_in_control1 = R(SPI_PS_IN_CONTROL_1);
    if ((k->ps_in_control1 >> 8) & 1) DBG("  pixel shader reads the front-facing bit (in control 1 %08X)\n", k->ps_in_control1);
    k->cb_shader_mask = R(CB_SHADER_MASK);
    k->alpha_test = R(SX_ALPHA_TEST_CONTROL);
    for (u32 i = 0; i < 8; i++) {
        u32 nt = (R(CB_COLOR0_INFO + i) >> 12) & 7;
        k->cb_kind[i] = nt == 4 ? 2 : nt == 5 ? 1 : 0;
        // The buffer's own format decides (the game can draw float-typed
        // output into a 32-bit integer buffer: the bits go in unchanged --
        // that's how it builds its compressed lighting cube maps).
        int ri = R(CB_COLOR0_BASE + i) ? find_rti(R(CB_COLOR0_BASE + i) << 8, false) : -1;
        if (ri >= 0) {
            VkFormat vf = color_format(g.rti[ri].fmt);
            int fk = format_int_kind(vf);
            if (fk >= 0) k->cb_kind[i] = (u8)fk;
        }
    }
    fill_tex_keys(k->vs_tex, SQ_TEX_RESOURCE_VS, LS_VS_TEX_UNITS);
    fill_tex_keys(k->ps_tex, SQ_TEX_RESOURCE_PS, LS_TEX_UNITS);
    u64 vsh = program_hash(vs_addr, vs_size), fsh = k->fs ? program_hash(fs_addr, fs_size) : 0, psh = program_hash(ps_addr, ps_size);
    *vsh_out = vsh; *psh_out = psh; *fsh_out = fsh;
    {   // only the texture units the programs read are part of the key
        static int nomask = -1;
        if (nomask < 0) nomask = getenv("SPLAT_NO_TEXMASK") != NULL;
        u32 vm = nomask ? ~0u : prog_tex_mask(vsh, k->vs, vs_size), pm = nomask ? ~0u : prog_tex_mask(psh, k->ps, ps_size);
        for (u32 u = 0; u < LS_TEX_UNITS; u++) {
            if (!(vm & (1u << u))) { k->vs_tex[u].dim = 0; k->vs_tex[u].kind = 0; }
            if (!(pm & (1u << u))) { k->ps_tex[u].dim = 0; k->ps_tex[u].kind = 0; }
        }
    }
    return true;
}

// --- Vertex shader variants that translate the same ---------------------------------------------
// A vertex shader's key holds the whole 32-entry semantic table, but the
// translation only reads it to find, for each semantic the fetch shader
// fetches, the first table entry with that semantic (emit_fetch). Tables that
// differ elsewhere give the same shader: the plaza had 659 vertex keys from
// 260 programs, 388 once only that part counts. A variant that isn't loaded
// yet takes the already loaded shader of its class (same module, so the
// same pipelines too) instead of a translation, a compile and new pipelines
// of its own -- on a first run that work was most of the multi-second
// stalls. Keys themselves are unchanged: the disk cache, the start-up
// preload and pipeline_keys.bin keep working as before. SPLAT_NO_VS_ALIAS=1: off.
static u32 fetched_sem_mask(u64 fsh, const LatteShaderKey* k, u32 out[8]) {   // (which semantics the fetch shader fetches; remembered per program)
    static struct { u64 h; u32 m[8]; } tab[4096];
    u32 i = (u32)(fsh ^ (fsh >> 31)) & 4095;
    if (fsh && tab[i].h == fsh) { memcpy(out, tab[i].m, 32); return 1; }
    memset(out, 0, 32);
    if (k->fs && k->fs_size >= 8) {
        for (u32 c = 0; c * 8 + 8 <= k->fs_size; c++) {   // (the same walk as emit_fetch)
            const u8* q = k->fs + c * 8;
            u32 w0 = (u32)q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24);
            u32 w1 = (u32)q[4] | ((u32)q[5] << 8) | ((u32)q[6] << 16) | ((u32)q[7] << 24);
            u32 inst = (w1 >> 23) & 0x7F;
            if (inst == 0x02 || inst == 0x03) {   // CF_VTX, CF_VTX_TC
                u32 count = (((w1 >> 10) & 7) | (((w1 >> 19) & 1) << 3)) + 1;
                for (u32 n = 0; n < count; n++) {
                    u32 off = w0 * 8 + n * 16;
                    if (off + 16 > k->fs_size) break;
                    const u8* v = k->fs + off;
                    u32 v0 = (u32)v[0] | ((u32)v[1] << 8) | ((u32)v[2] << 16) | ((u32)v[3] << 24);
                    u32 v1 = (u32)v[4] | ((u32)v[5] << 8) | ((u32)v[6] << 16) | ((u32)v[7] << 24);
                    if ((v0 & 0x1F) == 1) { u32 sem = v1 & 0xFF; out[sem >> 5] |= 1u << (sem & 31); }
                }
            }
            if (inst == 0x14 || ((w1 >> 21) & 1)) break;   // CF_RETURN / end of program
        }
    }
    if (fsh) { tab[i].h = fsh; memcpy(tab[i].m, out, 32); }
    return 1;
}
// The export side the same way (Nov 10): the 40-entry export semantic table (SPI_VS_OUT_ID) is only read for the
// parameter exports the program writes; the rest is left over from whatever program came before. Bot matches made
// 40-45% of their "new" vertex shaders from that alone (same program, same fetch, only stale entries differing),
// each one a translation, a compile and new pipelines in the middle of a match. SPLAT_VS_CLASS_OLD=1: the table
// counts whole again (and shaders from the disk cache don't stand for their class).
static int vs_class_old(void) { static int o = -1; if (o < 0) { o = getenv("SPLAT_VS_CLASS_OLD") != NULL; if (o) printf("[render] vertex shader classes as before Nov 10 (SPLAT_VS_CLASS_OLD)\n"); } return o; }
static u64 prog_exp_mask(u64 h, const u8* prog, u32 size) {
    static struct { u64 h; u64 mask; } tab[8192];
    u32 i = (u32)(h ^ (h >> 29)) & 8191;
    for (u32 n = 0; n < 8; n++, i = (i + 1) & 8191) {
        if (tab[i].h == h) return tab[i].mask;
        if (!tab[i].h) break;
    }
    u64 m = latte_shader_param_exports(prog, size);
    if (h) { tab[i].h = h; tab[i].mask = m; }
    return m;
}
static u64 vs_class_key(const LatteShaderKey* k, u64 vsh, u64 fsh) {   // (the key with only what the translation reads of the table)
    LatteShaderKey n = *k;
    u32 fetched[8], seen[8] = { 0 };
    fetched_sem_mask(fsh, k, fetched);
    for (u32 si = 0; si < 32; si++) {
        u32 sem = k->vtx_semantic[si];
        bool keep = (fetched[sem >> 5] >> (sem & 31) & 1) && !(seen[sem >> 5] >> (sem & 31) & 1);
        if (keep) seen[sem >> 5] |= 1u << (sem & 31);
        else n.vtx_semantic[si] = 0xFF;
    }
    if (!vs_class_old()) {
        u64 em = prog_exp_mask(vsh, k->vs, k->vs_size);
        for (u32 i = 0; i < 40; i++)
            if (!((em >> i) & 1)) n.vs_out_semantic[i] = 0xFF;
    }
    return hash_key_vs(&n, vsh, fsh) ^ 0x434C;
}
#define VSA_SIZE 65536u
static struct { u64 key; ShaderEnt* e; } g_vs_class[VSA_SIZE], g_vs_alias[VSA_SIZE];   // class -> its shader; variant key -> the class's shader
static u64 g_vs_aliased;
static ShaderEnt* vsa_get(void* tabp, u64 key) {
    struct { u64 key; ShaderEnt* e; }* tab = tabp;
    u32 i = (u32)(key * 0x9E3779B97F4A7C15ull >> 50) & (VSA_SIZE - 1);
    for (u32 n = 0; n < 32; n++, i = (i + 1) & (VSA_SIZE - 1)) {
        if (!tab[i].e) return NULL;
        if (tab[i].key == key) return tab[i].e;
    }
    return NULL;
}
static void vsa_put(void* tabp, u64 key, ShaderEnt* e) {
    struct { u64 key; ShaderEnt* e; }* tab = tabp;
    u32 i = (u32)(key * 0x9E3779B97F4A7C15ull >> 50) & (VSA_SIZE - 1);
    for (u32 n = 0; n < 32; n++, i = (i + 1) & (VSA_SIZE - 1))
        if (!tab[i].e || tab[i].key == key) { tab[i].key = key; tab[i].e = e; return; }
}
// --- A memo of the draw's shader setup -------------------------------------------------------------
// shader_key_for + get_vs + get_shader took ~1.7 ms of the drawing thread's frame in the busy plaza (1,400 draws, about
// a microsecond each: 350 register reads, three program hashes, the texture units decoded, two hash-table probes).
// Most draws repeat the same programs with the same registers frame after frame, so the result is remembered per
// program triple together with a snapshot of every register the key reads; a draw whose snapshot matches -- and
// whose programs' memory and the render-target table haven't changed -- takes the remembered key and shader
// entries. SPLAT_NO_KEY_MEMO=1: off.
#define KM_PLAIN (32 + 10 + 32 + 2 + 2 + 2 + 8 + 8 + 6)
#define KM_TEXU (LS_VS_TEX_UNITS + LS_TEX_UNITS)
#define KM_REGS (KM_PLAIN + KM_TEXU * 4)   // (per texture unit: words 0, 1, 4 and whether word 2 -- the address -- is set)
#define KM_SETS 1024
#define KM_WAYS 4
// (the colour targets' entries in the render-target table at the time, checked on a hit -- not the table's generation,
// which changes hundreds of times a frame)
typedef struct { u32 vs_addr, ps_addr, fs_addr, vs_size, ps_size, fs_size; u32 regs[KM_REGS]; u32 ntex; u16 tex_reg[KM_TEXU]; int cb_ri[8]; u32 cb_fmt[8]; u32 mem_gen; bool points, used; LatteShaderKey k; u64 vsh, psh, fsh; ShaderEnt* vs; ShaderEnt* ps; } KeyMemo;
static KeyMemo* g_km;
static u8 g_km_next[KM_SETS];   // (the way the next store in a set takes: round robin)
static u32 g_km_idx[KM_PLAIN];
static int g_km_on = -1;
static u64 g_km_hits, g_km_misses, g_km_bad;
static int g_km_check = -1;
static KeyMemo* km_slot(void) {
    if (g_km_on < 0) {
        g_km_on = getenv("SPLAT_NO_KEY_MEMO") ? 0 : 1;
        if (g_km_on) {
            g_km = (KeyMemo*)calloc(KM_SETS * KM_WAYS, sizeof(KeyMemo));
            if (!g_km) g_km_on = 0;
            u32 n = 0;
            for (u32 i = 0; i < 32; i++) g_km_idx[n++] = SQ_VTX_SEMANTIC_0 + i;
            for (u32 i = 0; i < 10; i++) g_km_idx[n++] = SPI_VS_OUT_ID_0 + i;
            for (u32 i = 0; i < 32; i++) g_km_idx[n++] = SPI_PS_INPUT_CNTL_0 + i;
            g_km_idx[n++] = SPI_PS_IN_CONTROL_0; g_km_idx[n++] = SPI_PS_IN_CONTROL_1;
            g_km_idx[n++] = CB_SHADER_MASK; g_km_idx[n++] = SX_ALPHA_TEST_CONTROL;
            g_km_idx[n++] = PA_CL_VTE_CNTL; g_km_idx[n++] = PA_CL_CLIP_CNTL;
            for (u32 i = 0; i < 8; i++) g_km_idx[n++] = CB_COLOR0_INFO + i;
            for (u32 i = 0; i < 8; i++) g_km_idx[n++] = CB_COLOR0_BASE + i;
            g_km_idx[n++] = SQ_PGM_START_VS; g_km_idx[n++] = SQ_PGM_START_VS + 1;
            g_km_idx[n++] = SQ_PGM_START_PS; g_km_idx[n++] = SQ_PGM_START_PS + 1;
            g_km_idx[n++] = SQ_PGM_START_FS; g_km_idx[n++] = SQ_PGM_START_FS + 1;
            if (n != KM_PLAIN) { printf("[render] shader-setup memo off: %u registers listed, %u expected\n", n, (unsigned)KM_PLAIN); g_km_on = 0; }
        }
        if (!g_km_on) printf("[render] shader-setup memo off\n");
    }
    if (!g_km_on) return NULL;
    u32 va = R(SQ_PGM_START_VS), pa = R(SQ_PGM_START_PS), fa = R(SQ_PGM_START_FS);
    u32 h = va * 2654435761u ^ (pa * 2246822519u) ^ (fa * 3266489917u);
    h ^= h >> 15;
    return &g_km[(h & (KM_SETS - 1)) * KM_WAYS];   // (the set's first way; km_find picks the way)
}
// the way holding these programs, or NULL; `store` gives the way a store should take
static KeyMemo* km_find(KeyMemo* set, KeyMemo** store) {
    u32 va = R(SQ_PGM_START_VS) << 8, pa = R(SQ_PGM_START_PS) << 8, fa = R(SQ_PGM_START_FS) << 8;
    KeyMemo* free_way = NULL;
    for (int w = 0; w < KM_WAYS; w++) {
        KeyMemo* e = &set[w];
        if (!e->used) { if (!free_way) free_way = e; continue; }
        if (e->vs_addr == va && e->ps_addr == pa && e->fs_addr == fa) { *store = e; return e; }
    }
    if (free_way) *store = free_way;
    else { u32 s = (u32)(set - g_km) / KM_WAYS; *store = &set[g_km_next[s]]; g_km_next[s] = (u8)((g_km_next[s] + 1) % KM_WAYS); }
    return NULL;
}
static void km_snapshot(KeyMemo* e) {   // (the plain registers in g_km_idx order, then 4 words per texture unit the programs read)
    u32 n = 0;
    for (u32 i = 0; i < KM_PLAIN; i++) e->regs[n++] = R(g_km_idx[i]);
    for (u32 i = 0; i < e->ntex; i++) {
        u32 r = e->tex_reg[i];
        e->regs[n++] = R(r) & 7; e->regs[n++] = R(r + 1) >> 26; e->regs[n++] = R(r + 4) & 0x301; e->regs[n++] = R(r + 2) != 0;
    }
}
static bool km_regs_same(const KeyMemo* e) {
    const u32* r = g_latte_regs;
    const u32* s = e->regs;
    if (memcmp(r + SQ_VTX_SEMANTIC_0, s, 32 * 4)) return false; s += 32;
    if (memcmp(r + SPI_VS_OUT_ID_0, s, 10 * 4)) return false; s += 10;
    if (memcmp(r + SPI_PS_INPUT_CNTL_0, s, 32 * 4)) return false; s += 32;
    if (r[SPI_PS_IN_CONTROL_0] != s[0] || r[SPI_PS_IN_CONTROL_1] != s[1] || r[CB_SHADER_MASK] != s[2] || r[SX_ALPHA_TEST_CONTROL] != s[3] || r[PA_CL_VTE_CNTL] != s[4] || r[PA_CL_CLIP_CNTL] != s[5]) return false;
    s += 6;
    if (memcmp(r + CB_COLOR0_INFO, s, 8 * 4)) return false; s += 8;
    if (memcmp(r + CB_COLOR0_BASE, s, 8 * 4)) return false; s += 8;
    if (r[SQ_PGM_START_VS] != s[0] || r[SQ_PGM_START_VS + 1] != s[1] || r[SQ_PGM_START_PS] != s[2] || r[SQ_PGM_START_PS + 1] != s[3] || r[SQ_PGM_START_FS] != s[4] || r[SQ_PGM_START_FS + 1] != s[5]) return false;
    s += 6;
    for (u32 i = 0; i < e->ntex; i++, s += 4) {
        u32 g = e->tex_reg[i];
        if ((r[g] & 7) != s[0] || (r[g + 1] >> 26) != s[1] || (r[g + 4] & 0x301) != s[2] || (u32)(r[g + 2] != 0) != s[3]) return false;
    }
    return true;
}
static u64 g_km_why[6], g_km_reg[KM_REGS];   // (misses by cause: unused slot, other programs, points, rti table, registers, memory / unchecked)
static bool km_hit(const KeyMemo* e, const LatteDraw* dr) {
    if (e->points != (dr->prim == PRIM_POINTS)) { g_km_why[2]++; return false; }
    for (u32 i = 0; i < 8; i++) {
        u32 base = R(CB_COLOR0_BASE + i) << 8;
        int ri = e->cb_ri[i];
        if (ri >= 0) { if (!g.rti[ri].used || g.rti[ri].addr != (base & ~0x7FFu) || g.rti[ri].fmt != e->cb_fmt[i]) { g_km_why[3]++; return false; } }
        else if (base && find_rti(base, false) >= 0) { g_km_why[3]++; return false; }
    }
    if (!km_regs_same(e)) { g_km_why[4]++; return false; }
    if (e->vs->unchecked || e->ps->unchecked) { g_km_why[5]++; return false; }   // (a cached shader not compared yet: the full path compares it once)
    if (wiiu_mem_changed_since(e->vs_addr, e->vs_size, e->mem_gen) || wiiu_mem_changed_since(e->ps_addr, e->ps_size, e->mem_gen) ||
        (e->fs_size && wiiu_mem_changed_since(e->fs_addr, e->fs_size, e->mem_gen))) { g_km_why[5]++; return false; }
    return true;
}
static void km_store(KeyMemo* e, const LatteDraw* dr, const LatteShaderKey* k, u64 vsh, u64 psh, u64 fsh, ShaderEnt* vs, ShaderEnt* ps) {
    e->mem_gen = wiiu_mem_generation();   // (before the snapshot: a change racing the store is seen as a change)
    e->vs_addr = R(SQ_PGM_START_VS) << 8; e->vs_size = R(SQ_PGM_START_VS + 1) << 3;
    e->ps_addr = R(SQ_PGM_START_PS) << 8; e->ps_size = R(SQ_PGM_START_PS + 1) << 3;
    e->fs_addr = R(SQ_PGM_START_FS) << 8; e->fs_size = R(SQ_PGM_START_FS + 1) << 3;
    {   // (the texture units the programs read: the key's units outside their masks are zeroed, so only these can matter)
        u32 vm = prog_tex_mask(vsh, k->vs, k->vs_size), pm = prog_tex_mask(psh, k->ps, k->ps_size);
        e->ntex = 0;
        for (u32 u = 0; u < LS_VS_TEX_UNITS; u++) if (vm & (1u << u)) e->tex_reg[e->ntex++] = (u16)(SQ_TEX_RESOURCE_VS + u * 7);
        for (u32 u = 0; u < LS_TEX_UNITS; u++) if (pm & (1u << u)) e->tex_reg[e->ntex++] = (u16)(SQ_TEX_RESOURCE_PS + u * 7);
    }
    km_snapshot(e);
    for (u32 i = 0; i < 8; i++) {
        u32 base = R(CB_COLOR0_BASE + i) << 8;
        e->cb_ri[i] = base ? find_rti(base, false) : -1;
        e->cb_fmt[i] = e->cb_ri[i] >= 0 ? g.rti[e->cb_ri[i]].fmt : 0;
    }
    e->points = dr->prim == PRIM_POINTS;
    e->k = *k; e->vsh = vsh; e->psh = psh; e->fsh = fsh; e->vs = vs; e->ps = ps;
    e->used = true;
}

static ShaderEnt* get_vs(const LatteShaderKey* k, u64 vsh, u64 fsh) {
    u64 key = hash_key_vs(k, vsh, fsh);
    static int off = -1;
    if (off < 0) { off = getenv("SPLAT_NO_VS_ALIAS") != NULL; if (off) printf("[render] vertex shader variant reuse off (SPLAT_NO_VS_ALIAS)\n"); }
    if (off) return get_shader(false, key, k);
    for (ShaderEnt* e = g.shaders[key & (LR_BUCKETS - 1)]; e; e = e->next)   // (loaded already -- preloaded, or used before: as before)
        if (e->key == key) {
            ShaderEnt* s = get_shader(false, key, k);
            // (a shader from the disk cache stands for its class too -- before, only ones translated this session did,
            // so variants of the warmed-up shaders were all translated and compiled again; once per key, the alias
            // table remembering it)
            if (!vs_class_old() && s && !s->failed && !vsa_get(g_vs_alias, key)) {
                u64 cls = vs_class_key(k, vsh, fsh);
                if (!vsa_get(g_vs_class, cls)) vsa_put(g_vs_class, cls, s);
                vsa_put(g_vs_alias, key, s);
            }
            return s;
        }
    ShaderEnt* a = vsa_get(g_vs_alias, key);
    if (a) return a;
    u64 cls = vs_class_key(k, vsh, fsh);
    ShaderEnt* c = vsa_get(g_vs_class, cls);
    if (c && !c->failed) {
        vsa_put(g_vs_alias, key, c);
        if (g_vs_aliased++ < 3 || g_vs_aliased % 100 == 0)
            printf("[render] %llu vertex shader variants reused an equivalent shader (no translation, compile or pipelines of their own)\n", (unsigned long long)g_vs_aliased);
        return c;
    }
    ShaderEnt* e = get_shader(false, key, k);
    vsa_put(g_vs_class, cls, e);
    return e;
}

static void lookahead_now(void) {
    static int off = -1;
    if (off < 0) { off = getenv("SPLAT_NO_LOOKAHEAD") != NULL; if (off) printf("[render] look-ahead off (SPLAT_NO_LOOKAHEAD)\n"); }
    if (off || !async_on() || g_pf_n >= 3000) return;
    static u64 last_frame = ~0ull, last_draw;
    if (last_frame == g.frame && g.draws - last_draw < 200) return;   // (once a frame, again after 200 more draws: more of it may have come in)
    last_frame = g.frame;
    last_draw = g.draws;
    u64 made = g_pf_made, t0 = hnow_ns();
    u32 n = latte_cp_lookahead(4000);
    static int logged = 0;
    if (logged++ < 40) printf("[render] look-ahead (frame %llu): %u coming draws read, %llu pipelines handed to the helper threads (%.1f ms)\n",
                              (unsigned long long)g.frame, n, (unsigned long long)(g_pf_made - made), (hnow_ns() - t0) / 1e6);
}

// --- Debug views (F7 / F8) -----------------------------------------------------------------------
// Off unless the settings row "Debug views" is on (or SPLAT_DEBUG_VIEWS=1); then F7 steps through:
//  1 wireframe            the 3D scene's draws as lines, with their own shaders
//  2 wireframe + colours  lines in one colour per shader pair (hashed from the two shaders' keys:
//                         the same pair always gets the same colour)
//  3 shader colours       solid, one colour per shader pair (which objects share shaders)
//  4 overdraw heatmap     how many times each pixel of the 3D scene was drawn: blue once, cyan 2,
//                         green 3, yellow 4-5, red 8+, white 16+ (shown instead of the picture)
//  5 skipped draws / NaN  the normal picture; draws whose uniform blocks hold NaN or infinity are
//                         bright red; a magenta frame around the picture in frames where draws were
//                         skipped (their shaders / pipelines still building)
// and back to normal. F8 = pick: a click prints the 3D draws that touched that pixel (the last is
// the one on top): shaders, buffers, textures, uniform blocks. A line under the counter shows the
// view, draws, distinct shader pairs, skipped draws and pipelines built per frame.
// Only the TV's 3D scene is touched -- draws with the depth test on into a depth buffer of the TV
// picture's size, not stream-out, not counted by an occlusion query of the game's, not one-time
// buffers -- so everything the game reads back from the GPU (ink maps, ink checks, query counts)
// and the GamePad picture stay as they are. While the view is normal nothing of this runs.
#define DV_HDR_COLOR 120   // (the draw header's word with the flat colour: unused by the shaders, see latte_shader.h)
static VkShaderModule g_dv_fs;
static int g_dv_fs_state;   // 0 not tried, 1 ready, -1 failed
static VkShaderModule fx_module(bool fragment, const char* src);
static const char* k_dv_fs =
    "#version 450\n"
    "layout(std430, set = 0, binding = 0) readonly buffer DrawData { uint d[]; };\n"
    "layout(push_constant) uniform PC { uint hdr; } pc;\n"
    "layout(location = 0) out vec4 o0; layout(location = 1) out vec4 o1; layout(location = 2) out vec4 o2; layout(location = 3) out vec4 o3;\n"
    "void main() { vec4 c = unpackUnorm4x8(d[pc.hdr + 120u]); o0 = c; o1 = c; o2 = c; o3 = c; }\n";
static u32 g_dv_skips_at, g_dv_nan, g_dv_scene;
static u64 g_dv_built_at;
static u64 g_dv_pair_key[1024];
static u64 g_dv_pair_frame[1024];
static u32 g_dv_pairs;
static struct { u32 shaders, skipped, built, nan, scene; } g_dv_last;   // last frame's numbers (the debug line)
static Img g_dv_heat;            // overdraw counts (R16F), the TV scene's size
static u64 g_dv_heat_tag;        // g.frame + 1 of the frame it holds
static Img g_dv_img;             // views 2 and 3 (flat colours) and 5 (the NaN draws in red): RGBA8, the TV scene's size
static u64 g_dv_img_tag;
static void dv_note_pair(u64 vk, u64 pk) {
    u64 key = vk * 0x9E3779B97F4A7C15ull ^ pk;
    u32 i = (u32)(key >> 54);
    for (u32 n = 0; n < 64; n++, i = (i + 1) & 1023) {
        if (g_dv_pair_frame[i] != g.frame + 1) { g_dv_pair_frame[i] = g.frame + 1; g_dv_pair_key[i] = key; g_dv_pairs++; return; }
        if (g_dv_pair_key[i] == key) return;
    }
}
static u32 dv_pair_color(u64 vk, u64 pk) {   // a bright colour from the pair's keys (hue from the hash)
    u64 h = (vk * 0x9E3779B97F4A7C15ull) ^ (pk * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 31;
    float hue = (float)(h & 0xFFFF) / 65536.0f * 6.0f, sat = 0.55f + (float)((h >> 16) & 0xFF) / 255.0f * 0.45f, val = 0.7f + (float)((h >> 24) & 0xFF) / 255.0f * 0.3f;
    int sector = (int)hue;
    float f = hue - (float)sector, p = val * (1 - sat), q = val * (1 - sat * f), t = val * (1 - sat * (1 - f));
    float r, gg, b;
    switch (sector) { case 0: r = val; gg = t; b = p; break; case 1: r = q; gg = val; b = p; break; case 2: r = p; gg = val; b = t; break;
                      case 3: r = p; gg = q; b = val; break; case 4: r = t; gg = p; b = val; break; default: r = val; gg = p; b = q; break; }
    return (u32)(r * 255.0f) | ((u32)(gg * 255.0f) << 8) | ((u32)(b * 255.0f) << 16) | 0xFF000000u;
}
static bool dv_int_format(VkFormat f) {   // (integer colour buffers can't take the flat colour shader's float outputs)
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return true;
    default: return false;
    }
}
// NaN / infinity in this draw's uniform blocks (as the game wrote them: little-endian floats)?
static bool dv_uniforms_bad(ShaderEnt* vs, ShaderEnt* ps) {
    static int logged;   // (the first ones in the log: which slots, what bits -- telling real NaNs from padding)
    bool bad = false;
    for (int stage = 0; stage < 2; stage++) {
        ShaderEnt* sh = stage == 0 ? vs : ps;
        u32 base = stage == 0 ? SQ_VS_UNIFORM_BLOCK : SQ_PS_UNIFORM_BLOCK;
        for (u32 b = 0; b < 16; b++) {
            if (!(sh->info.ub_used & (1u << b))) continue;
            u32 addr = R(base + b * 7), sz = R(base + b * 7 + 1) + 1;
            if (!addr || sz > (64u << 10)) continue;
            const u8* p = gmem(addr, sz);
            if (!p) continue;
            char where[160];
            int wn = 0, hits = 0;
            for (u32 o = 0; o + 4 <= sz; o += 4) {
                u32 w;
                memcpy(&w, p + o, 4);
                // (only the NaN that arithmetic makes -- 7FC00000 on the console, FFC00000 on a PC: blocks carry
                // leftover junk past what their shaders read, FFFFFFFE / FFBFAFAB ..., that is no NaN of the game's)
                if (w != 0x7FC00000u && w != 0xFFC00000u) continue;
                bad = true;
                if (logged < 40 && hits < 4 && wn < 120) wn += snprintf(where + wn, sizeof(where) - wn, " %u.%c=%08X", o / 16, "xyzw"[(o / 4) & 3], w);
                hits++;
            }
            if (hits && logged < 40) {
                logged++;
                printf("[render] debug view 5: %s %016llx block %u at %08X (%u B, read at fixed places up to vec4 %u): %d NaN words:%s\n", stage ? "ps" : "vs",
                       (unsigned long long)sh->key, b, addr, sz, sh->ub_fixed[b] ? sh->ub_fixed[b] - 1 : 0, hits, wn ? where : "");
            }
        }
    }
    return bad;
}
// A debug view's own image for this frame, the TV scene's size (cleared at the first use in a frame).
static bool dv_target_ready(Img* im, u64* tag, VkFormat fmt, u32 w, u32 h) {
    if (im->image && (im->w != w || im->h != h)) {
        if (*tag == g.frame + 1) return false;   // (a second size in one frame: only the first counts)
        memset(im, 0, sizeof(*im));              // (a new render scale: the old image is left behind, rare)
    }
    if (!im->image && !make_image(im, fmt, w, h, 1, VK_IMAGE_TYPE_2D, false, true, false)) return false;
    if (*tag != g.frame + 1) {
        end_pass();
        VkCommandBuffer cb = cmd();
        barrier_all(cb);
        VkClearColorValue z;
        memset(&z, 0, sizeof(z));
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
        barrier_all(cb);
        *tag = g.frame + 1;
    }
    return true;
}
// View 1: a buffer is cleared (black) before the 3D scene's first draw into it each frame: otherwise
// what the lines don't cover keeps old frames (the game never clears what it always covers).
// Wide screen: the game's full-screen 2D draws went through the widened HUD cameras and covered only
// the middle 16:9 -- a layout pane of exactly 1280x720 round the middle (menu backgrounds, the
// blurred picture behind the hero-upgrade menu, fades) or a unit quad a 16:9 ortho camera scales to the
// whole screen (the captured picture under that blur). The game's blur pass then read the whole wide
// buffer back into that middle, frame after frame: the background shrank to a strip. Such a draw gets
// the camera's 16:9 shape back in its copy of the constants, so it covers the whole picture, its
// texture stretched with it (the pane's texture coordinates follow its 1280x720 rect: a widened rect
// read past the texture's edges). (SPLAT_WIDE_FULL_OLD=1: as before)
static u32 g_wide_full_n[2];   // (panes, quads stretched: logged the first time)
static void wide_full_screen(float* c, u32 n) {
    static int off = -1;
    if (off < 0) off = getenv("SPLAT_WIDE_FULL_OLD") != NULL;
    if (off || splat_hud_stretch || (g_vk.wide_test & 6) == 6) return;
    // (the cameras use the shape rounded to 1/1000: native_funcs.c)
    float kx = splat_wide_kx_x1000 > 0 ? (float)splat_wide_kx_x1000 / 1000.0f : 1.0f;
    float ky = splat_wide_ky_x1000 > 0 ? (float)splat_wide_ky_x1000 / 1000.0f : 1.0f;
    if (n > 32) n = 32;
    bool full = false;   // (a pane of the whole 16:9 screen: size 1280x720, top-left -640,360)
    for (u32 i = 0; i < n && !full; i++) {
        const float* v = c + i * 4;
        full = v[0] == 1280.0f && v[1] == 720.0f && v[2] == -640.0f && v[3] == 360.0f;
    }
    // (then its projection: a row (sx 0 0 0) followed by (0 sy 0 0), sy / sx = 16:9 widened)
    for (u32 i = 0; full && !(g_vk.wide_test & 2) && i + 1 < n; i++) {
        float* a = c + i * 4;
        float* b = a + 4;
        if (a[1] != 0.0f || a[2] != 0.0f || a[3] != 0.0f || b[0] != 0.0f || b[2] != 0.0f || b[3] != 0.0f || a[0] <= 0.0f || b[1] == 0.0f) continue;
        float r = fabsf(b[1]) / a[0], want = (16.0f / 9.0f) * kx / ky;
        if (fabsf(r - want) > 0.002f * want) continue;
        if (kx > 1.0f) a[0] = fabsf(b[1]) * (9.0f / 16.0f);
        else b[1] = (b[1] < 0.0f ? -1.0f : 1.0f) * a[0] * (16.0f / 9.0f);
        if (!g_wide_full_n[0]++) printf("[render] wide screen: a full-screen 2D pane stretched to the whole picture (frame %llu)\n", (unsigned long long)g.frame);
        break;
    }
    if (n >= 2 && !(g_vk.wide_test & 4)) {   // (rows 0 and 1: x scaled by 2/kx, y by 2/ky -- a quad of -0.5..0.5 over the whole 16:9 screen)
        float* a = c;
        float* b = c + 4;
        if (a[1] == 0.0f && a[2] == 0.0f && b[0] == 0.0f && b[2] == 0.0f && a[3] == 0.0f && b[3] == 0.0f &&
            fabsf(fabsf(a[0]) * kx - 2.0f) < 2e-3f && fabsf(fabsf(b[1]) * ky - 2.0f) < 2e-3f) {
            a[0] = a[0] < 0.0f ? -2.0f : 2.0f;
            b[1] = b[1] < 0.0f ? -2.0f : 2.0f;
            if (!g_wide_full_n[1]++) printf("[render] wide screen: a full-screen 2D quad stretched to the whole picture (frame %llu)\n", (unsigned long long)g.frame);
        }
    }
}// Wide screen: a shown buffer's sides outside the game's 16:9 (or above / below it, taller) are cleared
// to black at its first draw each frame. Menus drawn only in the middle -- the gear menu, the
// hero-upgrade menu's blur -- left whatever an earlier frame had there (stale world; junk at 0.5x).
// The 3D world draws over them as before. (SPLAT_WIDE_BANDS_OLD=1: not cleared)
static void side_bands_clear(Surf* s) {
    s->band_frame = g.frame + 1;
    static int off = -1;
    if (off < 0) off = getenv("SPLAT_WIDE_BANDS_OLD") != NULL;
    if (off || (g_vk.wide_test & 1) || s->depth || s->vol || s->layers != 1 || !s->img.att_view) return;
    float f = s->scale < s->scalex ? s->scale : s->scalex;
    u32 W = s->img.w, H = s->img.h, w16 = (u32)(s->w * f + 0.5f), h16 = (u32)(s->h * f + 0.5f);
    VkClearRect r[2];
    memset(r, 0, sizeof(r));
    u32 n = 0;
    if (W > w16 + 1) {
        u32 x0 = (W - w16) / 2;
        r[0].rect.offset.x = 0; r[0].rect.extent.width = x0; r[0].rect.extent.height = H;
        r[1].rect.offset.x = (int)(x0 + w16); r[1].rect.extent.width = W - x0 - w16; r[1].rect.extent.height = H;
        n = 2;
    } else if (H > h16 + 1) {
        u32 y0 = (H - h16) / 2;
        r[0].rect.offset.y = 0; r[0].rect.extent.height = y0; r[0].rect.extent.width = W;
        r[1].rect.offset.y = (int)(y0 + h16); r[1].rect.extent.height = H - y0 - h16; r[1].rect.extent.width = W;
        n = 2;
    }
    if (!n) return;
    r[0].layerCount = r[1].layerCount = 1;
    end_pass();
    VkCommandBuffer cb = cmd();
    barrier_all(cb);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = s->img.att_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = W;
    ri.renderArea.extent.height = H;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkClearAttachment at;
    memset(&at, 0, sizeof(at));
    at.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    at.clearValue.color.float32[3] = 1.0f;
    vkCmdClearAttachments(cb, 1, &at, n, r);
    vkCmdEndRendering(cb);
    barrier_all(cb);
}
static void dv_clear_fresh(Surf* s) {
    if (s->dv_clear == g.frame + 1) return;
    s->dv_clear = g.frame + 1;
    end_pass();
    VkCommandBuffer cb = cmd();
    barrier_all(cb);
    VkClearColorValue z;
    memset(&z, 0, sizeof(z));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->img.layers };
    vkCmdClearColorImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
    barrier_all(cb);
}
// F8 pick: the next frame's 3D draws are each counted by an occlusion query of their own, cut to the
// clicked pixel; the frame's end prints the ones that drew there.
#define PICK_MAX 4096
static VkQueryPool g_pick_pool;
static bool g_pick_frame;        // this frame's 3D draws are being counted
static u32 g_pick_n;
static int g_pick_px, g_pick_py; // the pixel (the game's 1280x720)
static char (*g_pick_info)[1024];
static void pick_note(u32 i, const LatteDraw* dr, ShaderEnt* vs, ShaderEnt* ps, Surf** att, u32 n_att) {
    char* t = g_pick_info[i];
    int n = snprintf(t, 1024, "draw %u (%s, %u vertices x%u): vs %016llx ps %016llx |", g.draws, dr->index_addr ? "indexed" : "direct", dr->count,
                     dr->instances ? dr->instances : 1, (unsigned long long)vs->key, (unsigned long long)ps->key);
    for (u32 a = 0; a < n_att && n < 960; a++)
        n += snprintf(t + n, 1024 - n, " %s %08X %ux%u fmt %03X", att[a]->depth ? "depth" : "colour", att[a]->addr, att[a]->w, att[a]->h, att[a]->gx2fmt);
    n += snprintf(t + n, 1024 - n, " | textures");
    for (u32 u = 0; u < LS_TEX_UNITS && n < 960; u++)
        if (ps->info.tex_used & (1u << u)) {
            u32 r = SQ_TEX_RESOURCE_PS + u * 7;
            n += snprintf(t + n, 1024 - n, " t%u=%08X/%ux%u/%02X", u, R(r + 2) << 8, ((R(r) >> 19) & 0x1FFF) + 1, (R(r + 1) & 0x1FFF) + 1, (R(r + 1) >> 26) & 0x3F);
        }
    n += snprintf(t + n, 1024 - n, " | uniforms");
    for (int stage = 0; stage < 2 && n < 960; stage++) {
        ShaderEnt* sh = stage == 0 ? vs : ps;
        u32 base = stage == 0 ? SQ_VS_UNIFORM_BLOCK : SQ_PS_UNIFORM_BLOCK;
        for (u32 b = 0; b < 16 && n < 960; b++) {
            if (!(sh->info.ub_used & (1u << b))) continue;
            u32 addr = R(base + b * 7), sz = R(base + b * 7 + 1) + 1, bad = 0;
            const u8* p = addr && sz <= (64u << 10) ? gmem(addr, sz) : NULL;
            float v0[4] = { 0, 0, 0, 0 };
            if (p) {
                for (u32 o = 0; o + 4 <= sz; o += 4) { u32 w; memcpy(&w, p + o, 4); if ((w & 0x7F800000u) == 0x7F800000u) bad++; }
                memcpy(v0, p, sz >= 16 ? 16 : sz & ~3u);
            }
            n += snprintf(t + n, 1024 - n, " %s%u %08X %uB%s (%g %g %g %g)", stage ? "ps" : "vs", b, addr, sz, bad ? " NaN/inf!" : "", v0[0], v0[1], v0[2], v0[3]);
        }
    }
}

// Settings "Sky & light" (SPLAT_SKY; live SKY:n): 1 = dramatic -- the sky dome's draw gets drawn by a shader of
// ours (moving storm clouds, the sun breaking through), and the finished 3D picture gets a gritty grade right
// after the game's tone-map, before the HUD (sky_grade_pass). The game's own lighting stays as it is.
static int sky_mode(void) {
    static int env = -1;
    if (env == -1) { const char* e = getenv("SPLAT_SKY"); env = e ? atoi(e) : -2; if (env >= 0) g_vk.sky_mode = env; }   // (once: -2 = not set)
    return g_vk.sky_mode;
}
static VkImageView sky_texture_view(void);
static void sky_grade_pass(Surf* s);
static u64 g_grade_frame, g_sky_frame, g_bg_frame, g_vol_frame;
static void vol_pass(Surf* scene, Surf* dsurf);
static VkPipelineLayout g_blend_tm_layout;   // (fx_pipeline_fmt: blend as picture * transmittance + light)
static u32 g_present_swap;   // (defined with the presenter, further down)
static void motion_blur_pass(struct Surf* s);   // (below, with the grade)
static u64 g_mb_frame;
static u32 g_scene_addr;   // (the TV's scene buffer: the one the sky dome is drawn into)
// (the game's sky picture -- found at its sky-dome draw: a 2:1 texture of 1024+ -- whose draws read ours instead)
static struct { u32 addr, w, h, fmt; } g_sky_tex;
static VkImageView g_sky_orig_view;   // (the game's own sky picture as a texture view, from its last use)
static u64 g_sky_orig_frame;          // (the frame it was seen in: only trusted this frame or the one before -- after a
                                      // scene change the old picture may be gone, and a stale view must never be read)
static bool sky_orig_fresh(void) { return g_sky_orig_view && g.frame <= g_sky_orig_frame + 240; }   // (4 s: the dome isn't drawn every frame when looking straight up)
// The volume pass runs right after the sky dome's draw. Looking straight up (and in some views) the game skips that
// draw now and then -- every such frame had no clouds, haze or rain: the flicker. So the scene and depth buffers of the
// last dome frame are remembered, and a frame that reaches its tone-map draw without the pass gets it right there,
// before the tone-map reads the scene.
static Surf* g_vol_last_scene; static Surf* g_vol_last_depth;
static u32 g_vol_last_scene_addr, g_vol_last_depth_addr, g_vol_calls, g_vol_fallbacks;
static u32 g_vol_after_n, g_vol_after_min = ~0u, g_vol_after_max, g_vol_after_frame;
void latte_render_draw(const LatteDraw* dr) {
    if (!ready()) return;
    if (g_la_want) { g_la_want = false; lookahead_now(); }
    if (g_pred_skip) { DBG("draw skipped: conditional rendering (the query saw %s)\n", "the wrong thing"); return; }
    if (!g_render_headless) { preload_pipelines(); warm_scene_check(); }
    PF_BEGIN();
    g.draws++;
    g_draw_id++;
    if (R(VGT_GS_MODE) & 3) {
        if (g_unsupported_gs++ < 2) printf("[render] geometry shaders aren't supported yet (draw skipped)\n");
        DBG("draw skipped: geometry shader\n");
        return;
    }
    bool so_on = (R(VGT_STRMOUT_EN) & 1) != 0;
    bool raster_off = ((R(PA_CL_CLIP_CNTL) >> 22) & 1) != 0;
    if (raster_off && !so_on) { DBG("draw skipped: rasterization off\n"); return; }
    {   // SPLAT_SKIP_RT=addr,addr,... (hex; a bisection tool): draws whose colour target 0 or depth buffer sits at one of these
        // addresses are left out -- which draw family kills the graphics card on Hammerhead Bridge?
        static u32 skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            const char* e = getenv("SPLAT_SKIP_RT");
            while (e && *e && nskip < 16) { skip[nskip++] = (u32)strtoul(e, (char**)&e, 16); while (*e == ',' || *e == ' ') e++; }
            if (nskip) printf("[render] SPLAT_SKIP_RT: draws into %d listed buffers are skipped\n", nskip);
        }
        if (nskip) {
            u32 cb0 = R(CB_COLOR0_BASE) << 8, db = R(DB_DEPTH_BASE) << 8;
            for (int i = 0; i < nskip; i++) if ((cb0 && skip[i] == cb0) || (db && skip[i] == db)) return;
        }
    }
    // Performance mode: while the GamePad's screen isn't shown on the PC, its
    // picture (854x480 buffers) is drawn only every 8th frame -- the frames
    // the pop-up's watcher looks at it -- and every frame once it is shown or
    // the TV picture stands still (a menu only on the GamePad). The plaza's
    // GamePad map, the battle map: draws and graphics-card time saved.
    if (g_vk.perf_mode && !so_on && g_qcur < 0 && !g_vk.drc_on && !g_vk.drc_auto && !g_center_still && (g.frame & 7) != 0 && (g.frame & 7) != 7) {
        u32 cb0 = R(CB_COLOR0_BASE) << 8;
        int ri = cb0 ? find_rti(cb0, false) : -1;
        if (ri < 0 && R(DB_DEPTH_BASE)) ri = find_rti(R(DB_DEPTH_BASE) << 8, true);
        if (ri >= 0 && g.rti[ri].w == 854 && g.rti[ri].h == 480) { g_drc_draws_skipped++; DBG("draw skipped: GamePad picture (Performance mode)\n"); return; }
    }

    // --- shaders
    LatteShaderKey k;
    u64 vsh, psh, fsh;
    u64 sd_t0 = pf_now();   // (a draw whose shaders + pipeline took long to set up: logged, see g_sd_*)
    u64 sd_rc0 = g_recheck_count, sd_tr0 = g_sd_translated;
    ShaderEnt* vs;
    ShaderEnt* ps;
    KeyMemo* km_set = km_slot();
    KeyMemo* km = NULL;
    KeyMemo* km_way = km_set ? km_find(km_set, &km) : NULL;
    if (km_way && km_hit(km_way, dr)) {
        km = km_way;   // (the same programs with the same registers as a draw before: see KeyMemo)
        k = km->k; vsh = km->vsh; psh = km->psh; fsh = km->fsh; vs = km->vs; ps = km->ps;
        g_km_hits++;
        if (g_km_check < 0) g_km_check = getenv("SPLAT_KEY_MEMO_CHECK") != NULL;
        if (g_km_check) {   // (the full path too, compared: a memo that keyed a wrong shader shows here, not as a flicker)
            LatteShaderKey k2;
            u64 v2 = 0, p2 = 0, f2 = 0;
            if (shader_key_for(dr, &k2, &v2, &p2, &f2)) {
                ShaderEnt* vs2 = get_vs(&k2, v2, f2);
                ShaderEnt* ps2 = get_shader(true, hash_key_ps(&k2, p2), &k2);
                bool kd = memcmp(&k2, &k, sizeof(k)) != 0;
                if (kd || v2 != vsh || p2 != psh || f2 != fsh || ps2 != ps || (vs2 != vs && vs2->mod != vs->mod)) {
                    g_km_bad++;
                    if (g_km_bad <= 20) {
                        char why[200] = "";
                        #define KD(f) if (memcmp(&k2.f, &k.f, sizeof(k2.f))) strcat(why, " " #f);
                        KD(vtx_semantic) KD(vs_out_semantic) KD(vte_cntl) KD(clip_cntl) KD(points) KD(vs_tex) KD(ps_input_cntl) KD(ps_in_control0) KD(ps_in_control1) KD(cb_shader_mask) KD(alpha_test) KD(cb_kind) KD(ps_tex)
                        #undef KD
                        printf("[render] MEMO MISMATCH %llu: key fields%s%s | hashes %s | vs %s ps %s\n", (unsigned long long)g_km_bad, why, why[0] ? "" : " same",
                               v2 != vsh || p2 != psh || f2 != fsh ? "differ" : "same", vs2 == vs ? "same" : vs2->mod == vs->mod ? "alias" : "DIFFERENT", ps2 == ps ? "same" : "DIFFERENT");
                    }
                }
            }
        }
        g_sd_t[0] = pf_now() - sd_t0; g_sd_t[1] = 0; g_sd_t[2] = 0;
        g_sd_sum[0] += g_sd_t[0];
    } else {
        if (!shader_key_for(dr, &k, &vsh, &psh, &fsh)) return;
        u64 sd_t1 = pf_now();
        variant_note(vsh, psh, &k);
        vs = get_vs(&k, vsh, fsh);
        u64 sd_t2 = pf_now();
        ps = get_shader(true, hash_key_ps(&k, psh), &k);
        g_sd_t[0] = sd_t1 - sd_t0; g_sd_t[1] = sd_t2 - sd_t1; g_sd_t[2] = pf_now() - sd_t2;
        g_sd_sum[0] += g_sd_t[0]; g_sd_sum[1] += g_sd_t[1]; g_sd_sum[2] += g_sd_t[2];
        g_km_misses++;
        if (km) km_store(km, dr, &k, vsh, psh, fsh, vs, ps);
    }
    g_sd_start = sd_t0; g_sd_rc = g_recheck_count - sd_rc0; g_sd_tr = g_sd_translated - sd_tr0;
    bool so_draw = so_on && vs->info.so_mask;
    // Stream-out results the game reads (ink under the players and shots):
    // handed to the graphics card as soon as the group of such draws is over,
    // not at the game's next flush, so they're long done when the game waits
    // for them at GX2DrawDone. (Only when nothing recorded is waiting for the
    // game's flush: that data isn't final yet.) SPLAT_NO_EARLY_SO=1: off.
    // (Each submission costs the drawing thread ~0.2 ms, so only while the
    // game has recently waited for such results: SPLAT_SUBMIT_ALWAYS=1 always.)
    if (so_draw) g_so_last_draw = g.draws;
    else if (g_so_npend && g.draws - g_so_last_draw > 16 && !g_dc_n) {
        static int early = -1;
        if (early < 0) early = getenv("SPLAT_NO_EARLY_SO") ? 0 : getenv("SPLAT_SUBMIT_ALWAYS") ? 2 : 1;
        if (early == 2 || (early == 1 && gx2_cmd_wait_pressure())) so_defer();
    }
    if (raster_off && !so_draw) { DBG("draw skipped: rasterization off\n"); return; }
    // F1: no HUD / menus on the TV picture. Their draws are the layouts': a vertex-shader constant row
    // (0, 0, +-1, -+D) with D the distance of a layout camera (emote.c notes them: 989.1 for the 720-high
    // HUD, 494.5, 791.3...) -- the view's z, alone or folded into the whole matrix. The GamePad's HUD
    // (its cameras are nearer) stays.
    if ((g_vk.hide_hud || (fg_n() && !g_fg.scene_copied)) && vs->info.cfile_count && !so_draw) {
        extern float splat_hud_dist[8];
        extern volatile int splat_hud_ndist;
        int nd = splat_hud_ndist;
        u32 nc = vs->info.cfile_count < 32 ? vs->info.cfile_count : 32;
        bool hud = false;
        for (u32 i = 0; i < nc && !hud; i++) {
            const float* c = (const float*)&g_latte_regs[REG_BASE_ALU_CONST + 0x400 + i * 4];
            if (c[0] != 0.0f || c[1] != 0.0f || fabsf(fabsf(c[2]) - 1.0f) > 0.01f) continue;
            for (int k = 0; k < nd && !hud; k++) hud = fabsf(fabsf(c[3]) - splat_hud_dist[k]) < 0.6f && (c[2] > 0) != (c[3] > 0);
        }
        if (hud) {
            int ri = find_rti(R(CB_COLOR0_BASE) << 8, false);
            if (ri >= 0 && g.rti[ri].w == 1280 && g.rti[ri].h == 720) {
                if (g_vk.hide_hud) { DBG("draw skipped: HUD hidden (F1)\n"); return; }
                fg_scene_copy(get_rt(g.rti[ri].addr, g.rti[ri].fmt, g.rti[ri].w, g.rti[ri].h, false, g.rti[ri].layers));   // (smooth motion: the scene under the HUD)
            }
        }
    }

    PF_LAP(PF_SHADERS);
    u64 lap_att0 = pf_now();
    // --- attachments
    bool one_time = false;   // draws into a buffer that isn't redrawn every frame
    Surf* att_surf[9];
    u32 n_att = 0;
    VkImageView colors[8] = { 0 };
    VkFormat cfmt[8] = { 0 };
    Surf* dv_csurf[8] = { 0 };   // (debug views: the colour buffers)
    u32 ncolor = 0, rw = 0xFFFF, rh = 0xFFFF;
    PipeKey pk;
    memset(&pk, 0, sizeof(pk));
    u32 target_mask = R(CB_TARGET_MASK), shader_mask = R(CB_SHADER_MASK);
    u32 color_ctl = R(CB_COLOR_CONTROL);
    bool colors_off = ((color_ctl >> 4) & 7) == 1;
    for (u32 i = 0; i < 8; i++) {
        if (!(ps->info.color_outputs & (1u << i))) continue;
        u32 mask = (target_mask >> (4 * i)) & (shader_mask >> (4 * i)) & 0xF;
        if (colors_off) mask = 0;
        u32 base = R(CB_COLOR0_BASE + i) << 8, info = R(CB_COLOR0_INFO + i);
        if (!base || !((info >> 2) & 0x3F) || !mask) continue;
        int ri = find_rti(base, false);
        if (ri < 0) {
            static int warned = 0;
            if (warned++ < 4) printf("[render] color buffer at %08X has unknown size\n", base);
            continue;
        }
        Surf* s = get_rt(g.rti[ri].addr, g.rti[ri].fmt, g.rti[ri].w, g.rti[ri].h, false, g.rti[ri].layers | (g.rti[ri].dim == 2 ? RT_3D : 0));
        if (!s) continue;
        if (s->addr == g_scene_addr && g_bg_frame != g.frame + 1 && !s->depth && s->layers == 1) {
            extern volatile int splat_freecam_on;
            g_bg_frame = g.frame + 1;
            if (splat_freecam_on) {   // (outside the sky dome the game draws no background: a haze colour instead of old frames)
                VkCommandBuffer cb = transfer_cmd();
                VkClearColorValue cv;
                cv.float32[0] = 0.55f; cv.float32[1] = 0.60f; cv.float32[2] = 0.68f; cv.float32[3] = 1.0f;
                VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &range);
            }
        }
        att_surf[n_att++] = s;
        dv_csurf[i] = s;
        if (s->scan_src && s->band_frame != g.frame + 1 && s->scalex != s->scale) side_bands_clear(s);
        if (!note_recurring(s)) one_time = true;
        readback_note(s, ri);
        colors[i] = att_view(&s->img, R(CB_COLOR0_VIEW + i) & 0x7FF);
        DBG("  cb%u -> buffer %p %ux%u x%u fmt %03X vk %d slice %u mask %X info %08X (number type %u)\n", i, (void*)s, s->w, s->h, s->layers, s->gx2fmt, (int)s->img.format, R(CB_COLOR0_VIEW + i) & 0x7FF, mask, info, (info >> 12) & 7);
        cfmt[i] = s->img.format;
        ncolor = i + 1;
        if (s->w < rw) rw = s->w;
        if (s->h < rh) rh = s->h;
        pk.color_fmt[i] = (u32)s->img.format;
        pk.color_mask[i] = mask;
        if ((color_ctl >> (8 + i)) & 1) {
            pk.blend_on |= 1u << i;
            pk.blend[i] = R(CB_BLEND0_CONTROL + i);
        }
    }
    u64 lap_att1 = pf_now();
    u32 dctl = R(DB_DEPTH_CONTROL);
    bool z_on = (dctl >> 1) & 1, zw_on = (dctl >> 2) & 1, st_on = dctl & 1;
    VkImageView depth = VK_NULL_HANDLE;
    VkFormat dfmt = VK_FORMAT_UNDEFINED;
    bool has_stencil = false;
    Surf* dv_dsurf = NULL;   // (the depth buffer: the debug views' test for "the TV's 3D scene")
    int dri = R(DB_DEPTH_BASE) ? find_rti(R(DB_DEPTH_BASE) << 8, true) : -1;
    // (a draw with no colour output still needs its depth buffer bound: it
    // may be counted by an occlusion query)
    if ((z_on || st_on || ncolor == 0) && dri >= 0 && (R(DB_DEPTH_INFO) & 7)) {
        Surf* s = get_rt(g.rti[dri].addr, g.rti[dri].fmt, g.rti[dri].w, g.rti[dri].h, true, g.rti[dri].layers);
        if (s) {
            if (zw_on || st_on) s->wseq++;   // (may change it: an older copy of it is out of date)
            dv_dsurf = s;
            if (ncolor && s->w == 1280 && s->h == 720 && !s->vol) { g_fg.dsurf = s; g_fg.dsurf_frame = g.frame; }   // (smooth motion: its depth)
            att_surf[n_att++] = s;
            if (!note_recurring(s)) one_time = true;
            depth = att_view(&s->img, R(DB_DEPTH_VIEW) & 0x7FF);
            DBG("  zbuf -> buffer %p %ux%u x%u fmt %03X vk %d stencil %d scale %.1f image %ux%u layers %u\n", (void*)s, s->w, s->h, s->layers, s->gx2fmt, (int)s->img.format, s->stencil, s->scale, s->img.w, s->img.h, s->img.layers);
            dfmt = s->img.format;
            has_stencil = s->stencil;
            if (s->w < rw) rw = s->w;
            if (s->h < rh) rh = s->h;
            pk.depth_fmt = (u32)dfmt;
            pk.stencil_fmt = has_stencil ? (u32)dfmt : 0;
        }
    }
    u64 lap_att2 = pf_now();
    if (raster_off) {   // stream-out only: nothing is drawn
        memset(colors, 0, sizeof(colors));
        memset(pk.color_fmt, 0, sizeof(pk.color_fmt));
        memset(pk.color_mask, 0, sizeof(pk.color_mask));
        pk.blend_on = 0;
        ncolor = 0;
        depth = VK_NULL_HANDLE;
        pk.depth_fmt = pk.stencil_fmt = 0;
        has_stencil = false;
        rw = rh = 1;
    }
    if (!ncolor && !depth && so_draw) rw = rh = 1;
    if (!ncolor && !depth && !so_draw) {
        DBG("draw skipped: no buffer (cb0 base %08X info %08X tmask %08X smask %08X outputs %X colorctl %08X)\n", R(CB_COLOR0_BASE) << 8,
            R(CB_COLOR0_INFO), R(CB_TARGET_MASK), R(CB_SHADER_MASK), ps->info.color_outputs, R(CB_COLOR_CONTROL));
        return;
    }
    // Settings "Shadows: off": the shadow map's draws (depth only, into a square depth buffer of 1024+ -- the
    // sun's map, 2048x2048 x2 cascades) are left out; the game clears that map every frame, so everything
    // reads as lit. (Not inside an occlusion query: the game skips what a query saw nothing of.)
    if (!ncolor && depth && !so_draw && g_qcur < 0 && shadow_mode() == 0 && dv_dsurf && sun_shadow_map(dv_dsurf->gx2fmt, dv_dsurf->w, dv_dsurf->h)) {
        DBG("draw skipped: shadows off\n");
        return;
    }

    // --- shaders built on the helper threads ready? Skipping a draw for a
    // frame is only invisible in buffers drawn again every frame. Draws into
    // anything else -- the colour-grading table, the lighting maps made once
    // per scene, the ink maps -- and draws the game reads results of (stream
    // out, occlusion queries) wait for their shaders instead.
    // (small draws -- full-screen passes: lighting, ambient occlusion,
    // post effects -- wait too: skipping one leaves last frame's picture in
    // that buffer, the "bleeding" ghost images)
    bool small_draw = dr->count <= 6 && dr->instances <= 1;
    bool held_pass = false;   // (left out only if this frame isn't shown)
    if (small_draw && !one_time && never_stop() && n_att && !g_vk.gallery) {
        // (never_stop: a full-screen pass into the screen's own buffers may
        // wait for its build too while this frame isn't going to be shown
        // anyway -- what the skip leaves stale is redrawn next frame. Once
        // the held frames run out -- a cold scene's build storm -- it may
        // still wait if its buffers hold an earlier frame's picture (that
        // frame then shows last frame's pass, not a 100-300 ms stop); a
        // buffer in its first frames has nothing in it yet: then it's built.
        // SPLAT_SMALL_WAIT_HELD=1: only while held, as before)
        static int held_only = -1;
        if (held_only < 0) held_only = getenv("SPLAT_SMALL_WAIT_HELD") != NULL;
        bool can_hold = g_held_in_row < HOLD_MAX && !g_hold_spent;
        bool screen = true, history = true;
        for (u32 i = 0; i < n_att && screen; i++) { screen = screen_sized(att_surf[i]); history = history && !att_surf[i]->guessed; }
        if (screen && (can_hold || (history && !held_only))) { small_draw = false; held_pass = can_hold; }
    }
    // (a buffer drawn into for the first time in a while whose size is the screen's -- a scene / GamePad buffer the
    // game double-buffers or allocates anew -- is drawn again within a frame or two: a draw into it may skip while
    // its new shader is built on a helper (the object appears a frame late) instead of compiling the shader here,
    // 10-20 ms each: the Oct 31 bench's remaining hitches, all in the free camera's sweeps over new objects. Buffers
    // truly made once still wait. SPLAT_ONCE_WAIT_ALL=1: all of them wait, as before)
    if (one_time && never_stop()) {
        static int all = -1;
        if (all < 0) all = getenv("SPLAT_ONCE_WAIT_ALL") != NULL;
        bool screen = n_att > 0;
        for (u32 i = 0; i < n_att && screen; i++)
            screen = screen_sized(att_surf[i]) || (att_surf[i]->w == 848 && att_surf[i]->h == 480 && !att_surf[i]->vol && att_surf[i]->layers <= 1);
        if (screen && !all) { one_time = false; g_once_relaxed++; }
    }
    bool must_have = so_on || g_qcur >= 0 || one_time || small_draw;
    g_bw_why = so_on || g_qcur >= 0 ? BW_RESULTS : one_time ? BW_ONCE : must_have ? BW_SMALL : BW_SKIPS;
    if (one_time && once_dbg()) { g_od_ncur = 0; for (u32 i = 0; i < n_att; i++) if (!att_surf[i]->recurring) g_od_cur[g_od_ncur++] = att_surf[i]; }
    u64 lap_rdy0 = pf_now();
    g_lap_split[0] += lap_rdy0 - lap_att0;
    g_att_split[0] += lap_att1 - lap_att0; g_att_split[1] += lap_att2 - lap_att1; g_att_split[2] += lap_rdy0 - lap_att2;
    bool shaders_ok = shader_ready(vs, must_have) && shader_ready(ps, must_have);
    g_lap_split[1] += pf_now() - lap_rdy0;
    if (!shaders_ok) {
        g_skipped_draws++; g_ss.skip_shader++; g_frame_skipped = true; g_frame_skip_n++;
        if (held_pass) g_frame_must_hold = true;
        for (u32 i = 0; i < n_att; i++) if (att_surf[i]->guessed) att_surf[i]->guess_skips++;
        DBG("draw skipped: shader still compiling\n");
        return;
    }
    if (vs->failed || ps->failed) { DBG("draw skipped: shader failed (vs %d ps %d)\n", vs->failed, ps->failed); return; }

    // --- textures (uploads happen before the pass starts)
    VkDescriptorImageInfo img_info[LS_VS_TEX_UNITS + LS_TEX_UNITS];
    VkWriteDescriptorSet writes[2 + LS_VS_TEX_UNITS + LS_TEX_UNITS];
    u32 nw = 0;
    memset(writes, 0, sizeof(writes));
    static int bloom_old = -1;
    if (bloom_old < 0) bloom_old = getenv("SPLAT_BLOOM_OLD") != NULL;
    g_small_target = !bloom_old && ncolor > 0 && rw <= 640;   // (rt_game_size)
    g_small_w = rw;
    for (u32 i = 0; i < ncolor && g_small_target; i++) if (dv_csurf[i] && (dv_csurf[i]->scale != 1.0f || dv_csurf[i]->scalex != 1.0f)) g_small_target = false;
    for (int stage = 0; stage < 2; stage++) {
        ShaderEnt* sh = stage == 0 ? vs : ps;
        u32 units = stage == 0 ? LS_VS_TEX_UNITS : LS_TEX_UNITS;
        for (u32 u = 0; u < units; u++) {
            if (!(sh->info.tex_used & (1u << u))) continue;
            const LatteTexKey* tk = stage == 0 ? &k.vs_tex[u] : &k.ps_tex[u];
            u32 dim = tk->dim ? tk->dim - 1u : 1u;
            if (stage == 1 && dim == 3 && (LS_PS_CUBE_ARRAYS(&sh->info) >> u & 1)) dim = DIM_CUBE_ARRAY;
            u32 reg = (stage == 0 ? SQ_TEX_RESOURCE_VS : SQ_TEX_RESOURCE_PS) + u * 7;
            g_tv_surf = NULL;
            u64 tv_t0 = pf_now();
            VkImageView v = tk->dim ? texture_view(reg, dim) : VK_NULL_HANDLE;
            if (pf_now() - tv_t0 > 3000000ull) {   // (the Oct 31 bench: 10-21 ms of "texture work" in single frames of Kelp Dome / Port Mackerel -- which texture?)
                static int n = 0;
                if (n++ < 24) printf("[render] slow texture (frame %llu): %.1f ms for %s texture %u at %08X, %ux%u, words %08X %08X %08X%s\n", (unsigned long long)g.frame, (pf_now() - tv_t0) / 1e6,
                                     stage ? "ps" : "vs", u, (R(reg + 2) << 8) & ~0xFFu, ((R(reg) >> 19) & 0x1FFF) + 1, (R(reg + 1) & 0x1FFF) + 1, R(reg), R(reg + 1), R(reg + 4), g_tv_surf ? " (a drawn buffer)" : "");
            }
            for (u32 a = 0; v && g_tv_surf && a < n_att; a++)
                if (att_surf[a] == g_tv_surf) {
                    // Reading the depth buffer this draw also tests against: the
                    // graphics card may be reading/compressing it at the same
                    // time (undefined in Vulkan -- glowing edge lines on AMD).
                    // Read a copy taken just before instead.
                    u64 ft0 = pf_now();
                    VkImageView cv = feedback_copy(g_tv_surf, view_type(dim));
                    g_tex_ns2[2] += pf_now() - ft0;
                    DBG("  feedback: %s texture %u reads buffer %08X that this draw also uses%s\n", stage ? "ps" : "vs", u, g_tv_surf->addr, cv ? ": reading a copy" : "");
                    if (cv) v = cv;
                    break;
                }
            if (!v) v = dummy_view(dim);
            if (g_sky_tex.addr && dim == 1 && sky_mode() && ((R(reg + 2) << 8) & ~0x7FFu) == g_sky_tex.addr &&
                ((R(reg) >> 19) & 0x1FFF) + 1 == g_sky_tex.w && (R(reg + 1) & 0x1FFF) + 1 == g_sky_tex.h) {   // (the game's sky picture: ours)
                g_sky_orig_view = v;   // (the game's own picture: our sky, haze and clouds take the level's colours from it)
                g_sky_orig_frame = g.frame;
                VkImageView sv = sky_texture_view();
                if (sv) v = sv;
            }
            bool cmp = (sh->info.tex_shadow >> u) & 1;   // shadow maps need compare samplers
            u32 slot = stage == 0 ? u : LS_VS_TEX_UNITS + u;
            img_info[slot].imageView = v;
            img_info[slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            img_info[slot].sampler = sampler_for(stage == 0 ? u + LATTE_SAMPLER_BASE_VS : u, cmp);
            writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[nw].dstBinding = stage == 0 ? LS_BINDING_VS_TEX + u : LS_BINDING_PS_TEX + u;
            writes[nw].descriptorCount = 1;
            writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[nw].pImageInfo = &img_info[slot];
            nw++;
        }
    }

    PF_LAP(PF_TEXTURES);
    // --- indices
    u32 base_vertex = R(SQ_VTX_BASE_VTX_LOC), base_instance = R(SQ_VTX_START_INST_LOC);
    u32 cls;
    VkPrimitiveTopology topo = topology(dr->prim, &cls);
    bool indexed = dr->index_addr || dr->immediate || dr->prim == PRIM_QUADS || dr->prim == PRIM_LINE_LOOP;
    u32 vmin = 0, vmax = dr->count ? dr->count - 1 : 0, nidx = dr->count;
    VkBuffer ibuf = VK_NULL_HANDLE;
    u32 ioff = 0;
    bool restart = false;
    // Quads without an index buffer (particles, 2D): always the same pattern
    // (0 1 2, 0 2 3, 4 5 6, ...), kept on the graphics card once -- building
    // and converting it for every such draw was 3% of the drawing thread.
    static VkBuffer quad_buf;
    static VkDeviceMemory quad_mem;
    static void* quad_ptr;
    enum { QUADS_MAX = 65536 };
    bool quad_pattern = false;
    if (dr->prim == PRIM_QUADS && !dr->index_addr && !dr->immediate && dr->count / 4 <= QUADS_MAX && dr->count >= 4) {
        if (!quad_buf) {
            if (host_buffer((VkDeviceSize)QUADS_MAX * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &quad_buf, &quad_mem, &quad_ptr)) {
                u32* q = (u32*)quad_ptr;
                for (u32 i = 0; i < QUADS_MAX; i++) {
                    q[i * 6 + 0] = i * 4; q[i * 6 + 1] = i * 4 + 1; q[i * 6 + 2] = i * 4 + 2;
                    q[i * 6 + 3] = i * 4; q[i * 6 + 4] = i * 4 + 2; q[i * 6 + 5] = i * 4 + 3;
                }
            } else quad_buf = VK_NULL_HANDLE;
        }
        if (quad_buf) {
            quad_pattern = true;
            ibuf = quad_buf;
            ioff = 0;
            nidx = dr->count / 4 * 6;
            vmin = 0;
            vmax = dr->count / 4 * 4 - 1;
        }
    }
    if (indexed && !quad_pattern && !index_store_hit(dr, &ibuf, &ioff, &nidx, &vmin, &vmax, &restart)) {
        u32 cap = dr->count * 2 + 8;
        u32* idx = (u32*)arena_draw(cap * 4, 4, &ibuf, &ioff);
        if (!idx) { DBG("draw skipped: no room for %u indices\n", dr->count); return; }
        nidx = build_indices(dr, dr->prim, idx, cap, &vmin, &vmax, &restart);
        if (!nidx) { DBG("draw skipped: no indices (prim %u count %u)\n", dr->prim, dr->count); return; }
    }

    // --- draw data: table, uniform blocks, uniform registers, vertex data
    u32 sizes = HDR_WORDS * 4;
    u32 ub_size[2][16], ub_pad[2][16], attr_first[16], attr_bytes[16];
    memset(ub_size, 0, sizeof(ub_size));
    memset(ub_pad, 0, sizeof(ub_pad));
    memset(attr_bytes, 0, sizeof(attr_bytes));
    for (int stage = 0; stage < 2; stage++) {
        ShaderEnt* sh = stage == 0 ? vs : ps;
        u32 base = stage == 0 ? SQ_VS_UNIFORM_BLOCK : SQ_PS_UNIFORM_BLOCK;
        for (u32 b = 0; b < 16; b++) {
            if (!(sh->info.ub_used & (1u << b))) continue;
            u32 sz = R(base + b * 7 + 1) + 1;
            if (!R(base + b * 7) || sz > (64u << 10)) sz = 16;
            ub_size[stage][b] = (sz + 15) & ~15u;
            sizes += ub_size[stage][b];
            // (the shader reads past the block's end at a fixed index: padded, see below)
            u32 need = sh->ub_fixed[b] * 16u;
            if (need > ub_size[stage][b]) { ub_pad[stage][b] = need - ub_size[stage][b]; sizes += ub_pad[stage][b]; }
        }
        if (sh->info.cfile_count) sizes += 256 * 16;
    }
    u32 attribs = vs->info.attrib_used | vs->info.attrib_instanced;
    u32 instances = dr->instances ? dr->instances : 1;
    for (u32 b = 0; b < 16; b++) {
        if (!(attribs & (1u << b))) continue;
        u32 r = SQ_VTX_ATTRIB_BUFFER + b * 7;
        u32 size = R(r + 1) + 1, stride = (R(r + 2) >> 11) & 0xFFFF;
        u32 first = 0, last = size;
        if (!(vs->info.attrib_instanced & (1u << b))) {
            u64 f = (u64)(vmin + base_vertex) * stride, l = (u64)(vmax + base_vertex) * stride + vs->info.attrib_extent[b];
            first = f < size ? (u32)f : size;
            last = l < size ? (u32)l : size;
        } else {
            u64 l = (u64)(instances + base_instance) * stride + vs->info.attrib_extent[b];
            last = l < size ? (u32)l : size;
        }
        if (last < first) last = first;
        first &= ~3u;
        attr_first[b] = first;
        attr_bytes[b] = ((last - first) + 7) & ~3u;
        if (attr_bytes[b] > (4u << 20)) attr_bytes[b] = 4u << 20;   // (keeps a draw within one upload chunk)
        sizes += attr_bytes[b] + 16;
    }
    sizes += 64;   // slack for shader reads just past the end
    VkBuffer dbuf;
    u32 doff;
    u8* data = arena_draw(sizes, (u32)g_vk.props.limits.minStorageBufferOffsetAlignment > 256 ? (u32)g_vk.props.limits.minStorageBufferOffsetAlignment : 256, &dbuf, &doff);
    if (!data) { DBG("draw skipped: draw data too big (%u bytes, vertices %u-%u)\n", sizes, vmin, vmax); return; }
    g_up_ring = g_ring.state > 0 && dbuf == g_ring.buf;
    u32* hdr = (u32*)data;
    memset(hdr, 0, HDR_WORDS * 4);
    g_up_cat_copied[3] += HDR_WORDS * 4;
    u32 pos = HDR_WORDS * 4;   // bytes from data start
    for (int stage = 0; stage < 2; stage++) {
        ShaderEnt* sh = stage == 0 ? vs : ps;
        u32 base = stage == 0 ? SQ_VS_UNIFORM_BLOCK : SQ_PS_UNIFORM_BLOCK;
        for (u32 b = 0; b < 16; b++) {
            if (!ub_size[stage][b]) continue;
            u32 addr = R(base + b * 7), sz = ub_size[stage][b];
            // (uniform blocks are stored little-endian, the GPU's own byte
            // order: games swap the bytes themselves when filling them)
            u32 pad = ub_pad[stage][b];
            g_up_noreuse = pad != 0;
            g_up_cat = 0;
            u32 at = upload_guest(addr, sz, data + pos, doff + pos);
            g_up_noreuse = false;
            if (pad) {
                // Fixed-index reads skip the clamp (latte_shader.c k_helpers): past the end they read
                // copies of the block's last vec4, the value a clamped read returns
                for (u32 o = 0; o < pad; o += 16)
                    upload_guest(addr ? addr + sz - 16 : 0, 16, data + pos + sz + o, doff + pos + sz + o);
                g_ub_padded++;
                g_ub_pad_bytes += pad;
            }
            const u8* src = g_dbg && addr ? gmem(addr, sz) : NULL;
            if (g_dbg && src) {   // DBG_UB=v7 / p2 ...: print that uniform block
                const char* e = getenv("DBG_UB");
                if (e && e[0] == (stage == 0 ? 'v' : 'p') && (u32)atoi(e + 1) == b) {
                    printf("[dbg]   %s uniform block %u (%u vec4):\n", stage == 0 ? "vs" : "ps", b, sz / 16);
                    for (u32 i = 0; i < sz / 16; i++) {
                        float v[4];
                        memcpy(v, src + i * 16, 16);
                        printf("[dbg]     %3u: %g %g %g %g\n", i, v[0], v[1], v[2], v[3]);
                    }
                }
            }
            hdr[(stage == 0 ? HDR_VS_UB : HDR_PS_UB) + b] = at / 4;
            hdr[(stage == 0 ? HDR_VS_UB_SIZE : HDR_PS_UB_SIZE) + b] = sz / 16;
            if (at == doff + pos) pos += sz + pad;   // (found already uploaded: its space here isn't needed)
        }
        if (sh->info.cfile_count) {
            g_up_cat_copied[2] += 256 * 16;
            memcpy(data + pos, &g_latte_regs[REG_BASE_ALU_CONST + (stage == 0 ? 0x400 : 0)], 256 * 16);
            if (stage == 0 && (g_kx > 1.0f || g_ky > 1.0f) && n_att && !att_surf[0]->depth && att_surf[0]->w == 1280 && att_surf[0]->h == 720 &&
                att_surf[0]->scalex != att_surf[0]->scale)
                wide_full_screen((float*)(data + pos), sh->info.cfile_count);
            if (g_dbg && stage == 0)   // (the vertex shader's constants: where a layout pane goes)
                for (u32 i = 0; i < sh->info.cfile_count && i < 32; i++) {
                    float v[4];
                    memcpy(v, data + pos + i * 16, 16);
                    printf("[dbg]     vs c%u: %g %g %g %g\n", i, v[0], v[1], v[2], v[3]);
                }
            hdr[stage == 0 ? HDR_VS_CFILE : HDR_PS_CFILE] = (doff + pos) / 4;
            pos += 256 * 16;
        }
    }
    for (u32 b = 0; b < 16; b++) {
        if (!(attribs & (1u << b))) continue;
        u32 r = SQ_VTX_ATTRIB_BUFFER + b * 7;
        u32 addr = R(r), stride = (R(r + 2) >> 11) & 0xFFFF;
        g_up_cat = 1;
        u32 at = attr_bytes[b] ? upload_guest(addr + attr_first[b], attr_bytes[b], data + pos, doff + pos) : doff + pos;
        g_up_cat = 0;
        const u8* src = g_dbg && attr_bytes[b] ? gmem(addr + attr_first[b], attr_bytes[b]) : NULL;
        hdr[HDR_ATTR_BASE + b] = (u32)((int)at - (int)attr_first[b]);
        hdr[HDR_ATTR_STRIDE + b] = stride;
        if (g_dbg) {
            printf("[dbg]   attrib buffer %u: %08X size %u stride %u%s:", b, addr, R(r + 1) + 1, stride, (vs->info.attrib_instanced >> b) & 1 ? " (per instance)" : "");
            for (u32 i = 0; i < 192 && src && i * 4 + 4 <= attr_bytes[b]; i++) {
                u32 w;
                memcpy(&w, src + i * 4, 4);
                w = (w >> 24) | ((w >> 8) & 0xFF00) | ((w << 8) & 0xFF0000) | (w << 24);
                float f;
                memcpy(&f, &w, 4);
                printf(" %g", f);
            }
            printf("\n");
        }
        if (at == doff + pos) pos += (attr_bytes[b] + 15) & ~15u;
    }
    arena_trim(dbuf, doff, sizes, pos + 64);   // (+ the slack for reads just past the end)
    hdr[HDR_STEP_RATE] = R(VGT_INSTANCE_STEP_RATE_0);
    hdr[HDR_STEP_RATE + 1] = R(VGT_INSTANCE_STEP_RATE_0 + 1);
    hdr[HDR_ALPHA_REF] = R(SX_ALPHA_REF);
    float ws[2] = { 2.0f / (float)rw, 2.0f / (float)rh };   // (the game's pixel sizes: window-space vertices are in those)
    memcpy(&hdr[HDR_WIN_SCALE], ws, 8);
    // render scale: the buffers are bigger than the game thinks; the viewport,
    // scissor and render area follow (the shader's window scale above doesn't)
    float fsc = n_att ? att_surf[0]->scale : 1.0f, fscx = n_att ? att_surf[0]->scalex : 1.0f;
    if (fsc == 0) fsc = 1.0f;
    if (fscx == 0) fscx = fsc;
    u32 rwp = (u32)(rw * fscx + 0.5f), rhp = (u32)(rh * fsc + 0.5f);
    if (g_qcur >= 0) { g_queries[g_qcur].scale = fsc; g_queries[g_qcur].scalex = fscx; }   // (the running occlusion query counts pixels of this buffer)
    float point_size = (float)(R(PA_SU_POINT_SIZE) & 0xFFFF) / 8.0f;
    if (point_size < 1.0f) point_size = 1.0f;
    memcpy(&hdr[HDR_POINT_SIZE], &point_size, 4);
    // stream out: where this draw's vertices go (read back into game memory after the draw)
    u32 so_ring_off[4] = { 0 }, so_range[4] = { 0 }, so_addr[4] = { 0 };
    if (vs->info.so_mask && !g.so_buf) {
        if (!host_buffer_cached(SO_RING_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &g.so_buf, &g.so_mem, &g.so_ptr)) g.so_buf = VK_NULL_HANDLE;
    }
    if (so_draw && g.so_buf) {
        u32 verts = dr->count * instances;
        for (u32 i = 0; i < 4; i++) {
            if (!(vs->info.so_mask & (1u << i))) continue;
            u32 stride = R(VGT_STRMOUT_BUFFER_SIZE_0 + 1 + i * 4) * 4;
            u32 size = R(VGT_STRMOUT_BUFFER_SIZE_0 + i * 4) * 4;
            u32 base = R(VGT_STRMOUT_BUFFER_SIZE_0 + 2 + i * 4) << 8;
            u32 off = R(VGT_STRMOUT_BUFFER_SIZE_0 + 3 + i * 4);
            u64 want = (u64)verts * stride + 16;
            if (want > SO_RING_SIZE / 4) want = SO_RING_SIZE / 4;
            if (g.so_off + want > SO_RING_SIZE) { if (g_so_npend) so_sync(); defer_poll(true); g.so_off = 0; }   // (queued copies still read the ring)
            so_ring_off[i] = g.so_off;
            g.so_off = (u32)((g.so_off + want + 255) & ~255ull);
            u32 avail = size > off ? size - off : 0;
            u64 wr = (u64)verts * stride;
            so_range[i] = (u32)(wr < avail ? wr : avail);
            if (so_range[i] > want) so_range[i] = (u32)want;
            so_addr[i] = base + off;
            DBG("  stream out %u -> %08X size %u stride %u (%u vertices)\n", i, so_addr[i], so_range[i], stride, verts);
            hdr[HDR_SO_BASE + i] = so_ring_off[i] / 4;
            hdr[HDR_SO_STRIDE + i] = stride / 4;
        }
        hdr[HDR_SO_ON] = 1;
        hdr[HDR_SO_FIRST] = base_vertex;
        hdr[HDR_SO_VPI] = dr->count;
    }

    PF_LAP(PF_UPLOAD);
    // --- pipeline
    pk.vs = vs->mod;
    pk.ps = ps->mod;
    g_cur_vs_key = vs->key;
    g_cur_ps_key = ps->key;
    pk.topo_class = cls;
    pk.raster_off = raster_off ? 1 : 0;
    u32 clip = R(PA_CL_CLIP_CNTL);
    pk.depth_clamp = g_vk.depth_clamp && ((clip >> 26) & 3) == 3;
    pk.logic_op = (color_ctl >> 16) & 0xFF;
    u32 su = R(PA_SU_SC_MODE_CNTL);
    pk.poly_mode = (((su >> 3) & 3) && ((su >> 5) & 7) == 1) ? 1 : 0;
    // --- debug views (F7 / F8, see "Debug views" above): nothing while the view is normal
    bool dv_scene = false, dv_flat = false, dv_heat = false, dv_pick = false, dv_lines = false, dv_red = false;
    if (g_vk.debug_view || g_vk.debug_pick || g_pick_frame) {
        dv_note_pair(vs->key, ps->key);
        dv_scene = dv_dsurf && z_on && dv_dsurf->w == 1280 && dv_dsurf->h == 720 && !so_on && g_qcur < 0 && !one_time && !raster_off &&
                   (dr->count > 6 || instances > 1);
        if (dv_scene) {
            g_dv_scene++;
            int dv = g_vk.debug_view;
            bool ints = false;
            for (u32 i = 0; i < ncolor; i++) if (colors[i] && dv_int_format(cfmt[i])) ints = true;
            if (g_dv_fs_state == 0) {
                g_dv_fs = fx_module(true, k_dv_fs);
                g_dv_fs_state = g_dv_fs ? 1 : -1;
                if (!g_dv_fs) printf("[render] debug views: the flat-colour shader didn't compile\n");
            }
            bool fs_ok = g_dv_fs_state > 0 && !ints;
            u32 color = 0;
            Img* to = NULL;   // (the debug view's own image the draw goes to instead of the game's buffers)
            VkFormat to_fmt = VK_FORMAT_UNDEFINED;
            if ((dv == 1 || dv == 2) && g_vk.wireframe && !pk.poly_mode) { pk.poly_mode = 1; dv_lines = true; }
            if (dv == 1)
                for (u32 i = 0; i < ncolor; i++) if (dv_csurf[i] && colors[i]) dv_clear_fresh(dv_csurf[i]);
            if ((dv == 2 || dv == 3) && fs_ok && dv_target_ready(&g_dv_img, &g_dv_img_tag, VK_FORMAT_R8G8B8A8_UNORM, rwp, rhp)) {
                dv_flat = true; color = dv_pair_color(vs->key, ps->key); to = &g_dv_img; to_fmt = VK_FORMAT_R8G8B8A8_UNORM;
            }
            if (dv == 4 && fs_ok && dv_target_ready(&g_dv_heat, &g_dv_heat_tag, VK_FORMAT_R16_SFLOAT, rwp, rhp)) {
                dv_heat = dv_flat = true; color = 0x000000FFu; to = &g_dv_heat; to_fmt = VK_FORMAT_R16_SFLOAT;
            }
            if (dv == 5 && fs_ok && dv_uniforms_bad(vs, ps)) {
                // (drawn as the game draws it, then again in red over itself in the same pass -- taking it out of
                // the game's picture let the bright sky behind show through: white bloom halos round the red; an
                // overlay image of its own meant a change of drawing targets per red draw: a few fps)
                dv_red = true; g_dv_nan++; hdr[DV_HDR_COLOR] = 0xFF0000FFu;
            }
            dv_pick = g_pick_frame && g_pick_n < PICK_MAX;
            if (dv_flat) {
                pk.ps = g_dv_fs;
                pk.blend_on = 0;
                memset(pk.blend, 0, sizeof(pk.blend));
                pk.dual_src = 0;
                pk.logic_op = 0xCC;
                hdr[DV_HDR_COLOR] = color;
                if (to) {   // drawn into the debug view's image instead of the game's colour buffers (its depth buffer stays)
                    memset(colors, 0, sizeof(colors));
                    memset(cfmt, 0, sizeof(cfmt));
                    memset(pk.color_fmt, 0, sizeof(pk.color_fmt));
                    memset(pk.color_mask, 0, sizeof(pk.color_mask));
                    colors[0] = att_view(to, 0);
                    cfmt[0] = to_fmt;
                    ncolor = 1;
                    pk.color_fmt[0] = (u32)to_fmt;
                    pk.color_mask[0] = dv_heat ? 0x1 : 0xF;
                    if (dv_heat) { pk.blend_on = 1; pk.blend[0] = 0x101; }   // (one + one: a count)
                }
            }
        }
    }
    bool grade_after = false, vol_after = false;
    // The sky dome: drawn into the TV's scene buffer (1280x720, not the shown one) with the depth test on, not
    // blended, many vertices, its pixel shader reading a texture shaped 2:1 (a 360-degree panorama: top edge
    // straight up, horizon across the middle; 2048x1024 in the plaza, Octo Valley and Urchin Underpass) -- alone,
    // or (battle maps) with one small texture next to it (Urchin Underpass: a 512x256 one-channel picture).
    // SPLAT_SKY_ONE_TEX=1: the panorama alone only (as before Oct 24).
    u32 dome_reg = 0;
    {
        Surf* s0 = n_att >= 1 && ncolor == 1 ? att_surf[0] : NULL;
        u32 tu = ps->info.tex_used;
        static int one_tex = -1;
        if (one_tex == -1) one_tex = getenv("SPLAT_SKY_ONE_TEX") != NULL;
        int ntex = 0;
        for (u32 t = tu; t; t &= t - 1) ntex++;
        if (s0 && !s0->depth && !s0->scan_src && s0->w == 1280 && s0->h == 720 && depth && !pk.blend_on && dr->count >= 200 && ntex >= 1 && ntex <= (one_tex ? 1 : 2)) {
            u32 pano = 0, n_small = 0;   // (not "small": a Windows header macro)
            for (u32 u = 0; u < 32; u++) {
                if (!((tu >> u) & 1)) continue;
                u32 reg = SQ_TEX_RESOURCE_PS + u * 7;
                u32 tw = ((R(reg) >> 19) & 0x1FFF) + 1, th = (R(reg + 1) & 0x1FFF) + 1;
                if (tw >= 1024 && tw == th * 2) { if (!pano) pano = reg; }
                else if (tw <= 512 && th <= 512) n_small++;
            }
            if (pano && (ntex == 1 || n_small == 1)) dome_reg = pano;
        }
        if (dome_reg) g_scene_addr = s0->addr;   // (the scene buffer: the sky dome's -- learnt whatever the settings, for the free camera's background)
    }
    if (sky_mode() && !dv_flat && !dv_red && ncolor == 1 && n_att >= 1) {
        Surf* s0 = att_surf[0];
        // The dome's picture is then read as ours wherever the game uses it (sky_texture_view): the dome, the fog
        // layers drawn over it with the same picture, reflections -- all of it moves with the world exactly as the
        // game's own sky did.
        if (dome_reg) {
            u32 reg = dome_reg;
            u32 tw = ((R(reg) >> 19) & 0x1FFF) + 1, th = (R(reg + 1) & 0x1FFF) + 1, tf = (R(reg + 1) >> 26) & 0x3F;
            {
                u32 ta = (R(reg + 2) << 8) & ~0x7FFu;
                if (g_sky_tex.addr != ta) printf("[sky] sky picture at %08X (%ux%u format %02X): drawn by us from now on\n", ta, tw, th, tf);
                g_sky_tex.addr = ta; g_sky_tex.w = tw; g_sky_tex.h = th; g_sky_tex.fmt = tf;
                g_sky_frame = g.frame + 1;
                if ((g_vk.sh_clouds || g_vk.sh_rays || g_vk.sh_ao || (g_vk.sh_rain && g_vk.sky_mode >= 2)) && dv_dsurf && g_vol_frame != g.frame + 1) {
                    vol_after = true; g_vol_frame = g.frame + 1;
                    g_vol_last_scene = s0; g_vol_last_depth = dv_dsurf; g_vol_last_scene_addr = s0->addr; g_vol_last_depth_addr = dv_dsurf->addr;
                }
            }
        }
        // the grade: after the first full-screen draw into the shown 1280x720 buffer each frame (the game's tone-map)
        if (g_vk.sh_grade && s0->scan_src && s0->w == 1280 && s0->h == 720 && dr->count == 3 && g_grade_frame != g.frame + 1) { grade_after = true; g_grade_frame = g.frame + 1; }
        // (no dome draw this frame: the pass now, on the last dome frame's buffers, before the tone-map reads the scene)
        if (s0->scan_src && s0->w == 1280 && s0->h == 720 && dr->count == 3 && g_vol_frame != g.frame + 1 && g_vol_last_scene && g_vol_last_depth &&
            (g_vk.sh_clouds || g_vk.sh_rays || g_vk.sh_ao || (g_vk.sh_rain && g_vk.sky_mode >= 2)) &&
            g_vol_last_scene->addr == g_vol_last_scene_addr && g_vol_last_depth->addr == g_vol_last_depth_addr && g_vol_last_scene->w == 1280 && g_vol_last_scene->h == 720) {
            g_vol_frame = g.frame + 1;
            g_vol_fallbacks++;
            vol_pass(g_vol_last_scene, g_vol_last_depth);
        }
    }
    g_pipe_keys_skip = dv_flat || dv_lines;   // (debug pipelines aren't listed for the next start)
    bool pipe_wait = false;
    // (shaders whose draws already went missing for long enough: their new
    // pipelines are waited for too, so the draws don't vanish again)
    bool pipe_must = !g_pipe_keys_skip && (must_have || (!never_stop() && (vs->skip_frames >= max_skip() || ps->skip_frames >= max_skip())));
    if (pipe_must && !must_have) g_bw_why = BW_SHADER_SKIPS;
    u64 sd_p0 = pf_now();
    g_gl_cur_vs = vs; g_gl_cur_ps = ps;
    VkPipeline pipe = get_pipeline(&pk, pipe_must, &pipe_wait);
    g_gl_cur_vs = g_gl_cur_ps = NULL;
    g_pipe_keys_skip = false;
    {
        u64 now = pf_now(), all = now - g_sd_start;
        g_sd_sum[3] += now - sd_p0;
        if (all > 8000000ull && g_sd_logged < 40) {
            g_sd_logged++;
            const Surf* t0 = n_att ? att_surf[0] : NULL;
            printf("[render] slow draw setup (frame %llu): %.1f ms -- key %.1f, vertex shader %.1f, pixel shader %.1f (%llu translated, %llu re-checked), "
                   "attachments + textures %.1f, pipeline %.1f%s | into %ux%u fmt %03X%s, %u vertices, waits because: %s\n", (unsigned long long)g.frame, all / 1e6, g_sd_t[0] / 1e6, g_sd_t[1] / 1e6, g_sd_t[2] / 1e6,
                   (unsigned long long)g_sd_tr, (unsigned long long)g_sd_rc, (sd_p0 - g_sd_start - g_sd_t[0] - g_sd_t[1] - g_sd_t[2]) / 1e6, (now - sd_p0) / 1e6,
                   pipe_wait ? " (left out: still building)" : "", t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? t0->gx2fmt : 0, t0 && t0->depth ? " depth" : "",
                   dr->count, pipe_must ? k_bw_name[g_bw_why] : "-");
        }
    }
    PF_LAP(PF_SHADERS);
    if (pipe_wait) {
        g_skipped_draws++; g_ss.skip_pipe++; g_frame_skipped = true; g_frame_skip_n++;
        if (held_pass) g_frame_must_hold = true;
        for (u32 i = 0; i < n_att; i++) if (att_surf[i]->guessed) att_surf[i]->guess_skips++;
        DBG("draw skipped: pipeline still building\n");
        return;
    }
    if (!pipe) { DBG("draw skipped: no pipeline\n"); return; }

    // --- record
    begin_pass(colors, cfmt, ncolor, depth, dfmt, has_stencil, rwp, rhp);
    VkCommandBuffer cb = cmd();
    DYN_SET(pipe, pipe, vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe));
    g_dyn_skip = raster_off;   // (stream-out only: no viewport / depth / stencil / blend state for it, see g_dyn_skip)
    VkDescriptorBufferInfo bi = { dbuf, 0, dbuf == g_ring.buf ? g_ring.size : CHUNK_SIZE };
    writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[nw].dstBinding = 0;
    writes[nw].descriptorCount = 1;
    writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[nw].pBufferInfo = &bi;
    nw++;
    VkDescriptorBufferInfo sbi = { g.so_buf, 0, SO_RING_SIZE };
    if (vs->info.so_mask && g.so_buf) {
        writes[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[nw].dstBinding = LS_BINDING_STREAMOUT;
        writes[nw].descriptorCount = 1;
        writes[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[nw].pBufferInfo = &sbi;
        nw++;
    }
    if (g_vk.push_descriptors) {
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, nw, writes);
    } else {
        VkDescriptorSet set = alloc_set();
        if (!set) { DBG("draw skipped: no descriptor set\n"); g_dyn_skip = false; g_dyn.valid = false; return; }
        for (u32 i = 0; i < nw; i++) writes[i].dstSet = set;
        vkUpdateDescriptorSets(g_vk.device, nw, writes, 0, NULL);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, NULL);
    }
    u32 hdr_words = doff / 4;
    vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &hdr_words);

    // viewport
    VkViewport vp;
    bool window_space = (k.vte_cntl & 1) == 0;
    if (window_space) {
        vp.x = 0; vp.y = 0; vp.width = (float)rwp; vp.height = (float)rhp;
        vp.minDepth = 0; vp.maxDepth = 1;
    } else {
        float xs = RF(PA_CL_VPORT_XSCALE) * fscx, xo = RF(PA_CL_VPORT_XSCALE + 1) * fscx, ys = RF(PA_CL_VPORT_XSCALE + 2) * fsc;
        float yo = RF(PA_CL_VPORT_XSCALE + 3) * fsc, zs = RF(PA_CL_VPORT_XSCALE + 4), zo = RF(PA_CL_VPORT_XSCALE + 5);
        vp.x = xo - xs;
        vp.width = 2.0f * xs;
        vp.y = yo - ys;
        vp.height = 2.0f * ys;
        if ((clip >> 19) & 1) { vp.minDepth = zo; vp.maxDepth = zo + zs; }
        else { vp.minDepth = zo - zs; vp.maxDepth = zo + zs; }
        if (vp.width < 0) { vp.x += vp.width; vp.width = -vp.width; }   // (x mirroring isn't possible in Vulkan)
        if (vp.width == 0) vp.width = 1;
        if (vp.height == 0) vp.height = 1;
        if (vp.minDepth < 0) vp.minDepth = 0;
        if (vp.minDepth > 1) vp.minDepth = 1;
        if (vp.maxDepth < 0) vp.maxDepth = 0;
        if (vp.maxDepth > 1) vp.maxDepth = 1;
    }
    DYN_SET(vp, vp, vkCmdSetViewportWithCount(cb, 1, &vp));
    u32 tl = R(PA_SC_GENERIC_SCISSOR_TL), br = R(PA_SC_GENERIC_SCISSOR_BR);
    int sx0 = (int)((tl & 0x7FFF) * fscx), sy0 = (int)(((tl >> 16) & 0x7FFF) * fsc), sx1 = (int)((br & 0x7FFF) * fscx + 0.5f), sy1 = (int)(((br >> 16) & 0x7FFF) * fsc + 0.5f);
    if (sx1 == 0 && sy1 == 0) { sx1 = (int)rwp; sy1 = (int)rhp; }
    if (sx1 > (int)rwp) sx1 = (int)rwp;
    if (sy1 > (int)rhp) sy1 = (int)rhp;
    if (sx0 > sx1) sx0 = sx1;
    if (sy0 > sy1) sy0 = sy1;
    VkRect2D sc = { { sx0, sy0 }, { (u32)(sx1 - sx0), (u32)(sy1 - sy0) } };
    if (dv_pick) {   // (F8: this draw only at the clicked pixel, counted by a query of its own)
        int px = (int)(g_pick_px * fscx), py = (int)(g_pick_py * fsc);
        if (px >= sx0 && px < sx1 && py >= sy0 && py < sy1) { sc.offset.x = px; sc.offset.y = py; sc.extent.width = sc.extent.height = 1; }
        else dv_pick = false;
    }
    DYN_SET(sc, sc, vkCmdSetScissorWithCount(cb, 1, &sc));
    // rasterizer
    VkCullModeFlags cull = 0;
    if (su & 1) cull |= VK_CULL_MODE_FRONT_BIT;
    if (su & 2) cull |= VK_CULL_MODE_BACK_BIT;
    DYN_SET(cull, cull, vkCmdSetCullMode(cb, cull));
    // GX2: 0 = counter-clockwise front. With the usual upside-down viewport
    // (negative height) Vulkan sees the same winding as the console; a game
    // viewport that is already flipped turns it around.
    bool ccw = ((su >> 2) & 1) == 0;
    if (!window_space && vp.height > 0) ccw = !ccw;
    VkFrontFace ff = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    DYN_SET(ff, ff, vkCmdSetFrontFace(cb, ff));
    {   // (input assembly is used with the rasterizer off too: a stream-out-only draw sets these, the layer's VUID 07500)
        bool sk = g_dyn_skip;
        g_dyn_skip = false;
        DYN_SET(topo, topo, vkCmdSetPrimitiveTopology(cb, topo));
        VkBool32 restart_en = restart && cls != 0 && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        DYN_SET(restart, restart_en, vkCmdSetPrimitiveRestartEnable(cb, restart_en));
        g_dyn_skip = sk;
    }
    bool poly_offset = (su >> 11) & 3;
    VkBool32 bias_en = poly_offset && depth;
    if (!g_dyn.valid) memset(g_dyn.bias, 0xFF, sizeof(g_dyn.bias));   // (a new command buffer sets its depth bias once even if unchanged: the layer's VUID 07834)
    DYN_SET(bias_en, bias_en, vkCmdSetDepthBiasEnable(cb, bias_en));
    if (poly_offset && depth) {
        float bias[3] = { RF(PA_SU_POLY_OFFSET_FRONT_SCALE + 1), RF(PA_SU_POLY_OFFSET_CLAMP), RF(PA_SU_POLY_OFFSET_FRONT_SCALE) / 16.0f };
        DYN_SET(bias, bias, vkCmdSetDepthBias(cb, bias[0], bias[1], bias[2]));
    }
    VkBool32 ztest = depth && z_on, zwrite = depth && zw_on;
    DYN_SET(ztest, ztest, vkCmdSetDepthTestEnable(cb, ztest));
    DYN_SET(zwrite, zwrite, vkCmdSetDepthWriteEnable(cb, zwrite));
    VkCompareOp zop = (VkCompareOp)((dctl >> 4) & 7);
    // EQUAL stays EQUAL: the colour pass after a depth pre-pass must not draw
    // over holes the pre-pass left (eyes showing through the face mask).
    static int lequal = -1;
    if (lequal < 0) lequal = getenv("SPLAT_LEQUAL") != NULL;
    if (zop == VK_COMPARE_OP_EQUAL && lequal) zop = VK_COMPARE_OP_LESS_OR_EQUAL;
    DYN_SET(zop, zop, vkCmdSetDepthCompareOp(cb, zop));
    if (!g_dyn.valid && !g_dyn_skip) vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    VkBool32 stencil = depth && has_stencil && st_on;
    DYN_SET(stencil, stencil, vkCmdSetStencilTestEnable(cb, stencil));
    u32 sf = R(DB_STENCILREFMASK), sb = R(DB_STENCILREFMASK_BF);
    bool back_sep = (dctl >> 7) & 1;
    if (!back_sep) sb = sf;
    u32 sop_f = (dctl >> 8) & 0xFFF, sop_b = back_sep ? (dctl >> 20) & 0xFFF : sop_f;   // (compare op + 3 stencil ops, packed as in the register)
    DYN_SET(sop_f, sop_f, vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, (VkStencilOp)((sop_f >> 3) & 7), (VkStencilOp)((sop_f >> 6) & 7),
                                           (VkStencilOp)((sop_f >> 9) & 7), (VkCompareOp)(sop_f & 7)));
    DYN_SET(sop_b, sop_b, vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, (VkStencilOp)((sop_b >> 3) & 7), (VkStencilOp)((sop_b >> 6) & 7),
                                           (VkStencilOp)((sop_b >> 9) & 7), (VkCompareOp)(sop_b & 7)));
    u32 sref_f = sf & 0xFF, sref_b = sb & 0xFF, scmp_f = (sf >> 8) & 0xFF, scmp_b = (sb >> 8) & 0xFF, swm_f = (sf >> 16) & 0xFF, swm_b = (sb >> 16) & 0xFF;
    DYN_SET(sref_f, sref_f, vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, sref_f));
    DYN_SET(sref_b, sref_b, vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, sref_b));
    DYN_SET(scmp_f, scmp_f, vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, scmp_f));
    DYN_SET(scmp_b, scmp_b, vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, scmp_b));
    DYN_SET(swm_f, swm_f, vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, swm_f));
    DYN_SET(swm_b, swm_b, vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, swm_b));
    float bc[4] = { RF(CB_BLEND_RED), RF(CB_BLEND_RED + 1), RF(CB_BLEND_RED + 2), RF(CB_BLEND_RED + 3) };
    DYN_SET(bc, bc, vkCmdSetBlendConstants(cb, bc));
    DBG("  depth-stencil: control %08X refmask %08X/%08X stencil %s (has %d)\n", dctl, sf, sb, stencil ? "on" : "off", has_stencil);
    DBG("  blend: color_control %08X blend0 %08X alpha-test %08X ref %.3f\n", R(CB_COLOR_CONTROL), R(CB_BLEND0_CONTROL), R(SX_ALPHA_TEST_CONTROL), RF(SX_ALPHA_REF));

    DBG("draw prim %u count %u inst %u idx %u | rt %08X %ux%u fmt %03X mask %X blend %X | depth %d z%d w%d func %u | vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f sc %d,%d-%d,%d cull %u | vs %016llx ps %016llx tex %X ub %X/%X cf %u/%u vte %X\n",
        dr->prim, dr->count, instances, indexed ? nidx : 0, R(CB_COLOR0_BASE) << 8, rw, rh, (R(CB_COLOR0_INFO) >> 2) & 0x3F, pk.color_mask[0], pk.blend_on,
        depth != VK_NULL_HANDLE, z_on, zw_on, (dctl >> 4) & 7, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, sx0, sy0, sx1, sy1, su & 3,
        (unsigned long long)vs->key, (unsigned long long)ps->key, ps->info.tex_used, vs->info.ub_used, ps->info.ub_used, vs->info.cfile_count, ps->info.cfile_count, k.vte_cntl);
    if (g_dbg) {   // which buffers / textures this draw uses
        char tl[400];
        int tn = 0;
        tl[0] = 0;
        for (u32 u = 0; u < LS_TEX_UNITS && tn < 360; u++)
            if (ps->info.tex_used & (1u << u)) {
                u32 r = SQ_TEX_RESOURCE_PS + u * 7;
                tn += snprintf(tl + tn, sizeof(tl) - tn, " t%u=%08X/%ux%u/%02X/d%u", u, R(r + 2) << 8, ((R(r) >> 19) & 0x1FFF) + 1,
                               (R(r + 1) & 0x1FFF) + 1, (R(r + 1) >> 26) & 0x3F, R(r) & 7);
            }
        DBG("  zbuf %08X view %08X cb1 %08X cb2 %08X%s\n", R(DB_DEPTH_BASE) << 8, R(DB_DEPTH_VIEW), R(CB_COLOR0_BASE + 1) << 8, R(CB_COLOR0_BASE + 2) << 8, tl);
        tn = 0;
        tl[0] = 0;
        for (u32 u = 0; u < LS_TEX_UNITS && tn < 360; u++)   // (the samplers: LOD clamps and bias)
            if (ps->info.tex_used & (1u << u)) {
                u32 sw = REG_BASE_SAMPLER + u * 3;
                tn += snprintf(tl + tn, sizeof(tl) - tn, " s%u=%08X/%08X/%08X", u, R(sw), R(sw + 1), R(sw + 2));
            }
        DBG("  samplers%s\n", tl);
    }
    if (dv_pick) vkCmdBeginQuery(cb, g_pick_pool, g_pick_n, 0);
    if (indexed) {
        if (!g_dyn.valid || g_dyn.ibuf != ibuf || g_dyn.ioff != ioff) { vkCmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT32); g_dyn.ibuf = ibuf; g_dyn.ioff = ioff; }
        vkCmdDrawIndexed(cb, nidx, instances, 0, (int32_t)base_vertex, base_instance);
    } else {
        vkCmdDraw(cb, dr->count, instances, base_vertex, base_instance);
    }
    if (dv_pick) { vkCmdEndQuery(cb, g_pick_pool, g_pick_n); pick_note(g_pick_n, dr, vs, ps, att_surf, n_att); g_pick_n++; }
    g_dyn.valid = !g_dyn_skip;   // (after a stream-out-only draw the next draw sets all its state again)
    g_dyn_skip = false;
    if (dv_red) {   // (debug view 5: the same draw again, flat red, on top of itself: same targets, same pass)
        PipeKey rk = pk;
        rk.ps = g_dv_fs;
        memset(rk.blend, 0, sizeof(rk.blend));
        rk.blend_on = 0;
        rk.dual_src = 0;
        rk.logic_op = 0xCC;
        bool rw_wait = false;
        g_pipe_keys_skip = true;
        VkPipeline rp = get_pipeline(&rk, false, &rw_wait);
        g_pipe_keys_skip = false;
        if (rp) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rp);
            vkCmdSetDepthWriteEnable(cb, VK_FALSE);
            vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
            vkCmdSetStencilTestEnable(cb, VK_FALSE);
            if (indexed) vkCmdDrawIndexed(cb, nidx, instances, 0, (int32_t)base_vertex, base_instance);
            else vkCmdDraw(cb, dr->count, instances, base_vertex, base_instance);
            g_dyn.valid = false;   // (the next draw sets all its state again)
        }
    }
    PF_LAP(PF_RECORD);
    if (g_vol_frame == g.frame + 1 && !vol_after && n_att >= 1 && att_surf[0] == g_vol_last_scene) {   // (a scene-buffer draw after this frame's pass)
        if (g_vol_after_frame != (u32)g.frame) {
            if (g_vol_after_frame) { if (g_vol_after_n < g_vol_after_min) g_vol_after_min = g_vol_after_n; if (g_vol_after_n > g_vol_after_max) g_vol_after_max = g_vol_after_n; }
            g_vol_after_frame = (u32)g.frame; g_vol_after_n = 0;
        }
        g_vol_after_n++;
    }
    if (grade_after) sky_grade_pass(att_surf[0]);
    if (g_vk.motion_blur && !dv_flat && !dv_red && ncolor == 1 && n_att >= 1) {   // (motion blur: after the tone-map, the first full-screen draw into the shown buffer)
        Surf* s0 = att_surf[0];
        if (s0 && s0->scan_src && s0->w == 1280 && s0->h == 720 && dr->count == 3 && g_mb_frame != g.frame + 1) { g_mb_frame = g.frame + 1; motion_blur_pass(s0); }
    }
    if (vol_after) {
        vol_pass(att_surf[0], dv_dsurf);   // (the 3D clouds, right after the last opaque draw: the sky dome)
        // (and the frame so far goes to the card now: the scene + the pass (~1-2 ms of the card's time) run while the
        // rest of the frame is recorded, instead of after it -- in the busy plaza that was the difference between
        // making the 16.7 ms rhythm or not. SPLAT_VOL_SUBMIT=0: with the rest of the frame, as before)
        static int vol_submit = -1;
        if (vol_submit < 0) { const char* e = getenv("SPLAT_VOL_SUBMIT"); vol_submit = e ? atoi(e) != 0 : 1; }
        if (vol_submit && g.cur >= 0 && !g_dc_n && !g_so_npend) flush_nowait();
    }
    if (so_draw && g.so_buf) {
        // The game reads what was written (Splatoon: what ink is under each
        // player and shot, a few vertices at a time, several times a frame).
        // It can only look after a fence / timestamp or a memory write from the
        // GPU, so the copy back waits until then (latte_render_sync_readbacks)
        // or until a draw reads that memory: one wait for the graphics card for
        // all of them, instead of one per stream-out draw (each emptied the
        // graphics card's queue: the frame rate fell while shooting ink).
        // SPLAT_SO_SYNC=1: wait at each one, like before.
        static int sync = -1;
        if (sync < 0) sync = getenv("SPLAT_SO_SYNC") != NULL;
        g_ss.so_draws++;
        for (u32 i = 0; i < 4; i++) {
            if (!(vs->info.so_mask & (1u << i))) continue;
            if (so_range[i] && wiiu_mem_ptr(so_addr[i], so_range[i])) {
                if (g_so_npend == SO_PEND_MAX) so_sync();
                g_so_pend[g_so_npend].addr = so_addr[i];
                g_so_pend[g_so_npend].ring = so_ring_off[i];
                g_so_pend[g_so_npend].size = so_range[i];
                g_so_npend++;
                g_ss.so_bytes += so_range[i];
            }
            g_latte_regs[VGT_STRMOUT_BUFFER_SIZE_0 + 3 + i * 4] += so_range[i];
        }
        if (sync) so_sync();
        // (SPLAT_SO_LOG=N: the first N stream-out draws in detail -- where in
        // the frame they are, what they write and what the shader reads)
        static int logged = 0, so_log = -1;
        if (so_log < 0) { const char* e = getenv("SPLAT_SO_LOG"); so_log = e ? atoi(e) : 4; }
        if (logged++ < so_log)
            printf("[render] stream out: frame %llu draw %u: %u vertices -> %08X (%u bytes), shader reads textures %X, uniform blocks %X%s\n",
                   (unsigned long long)g.frame, g.draws, dr->count * instances, so_addr[0], so_range[0], vs->info.tex_used, vs->info.ub_used,
                   sync ? " written back" : "");
    }
}

// --- Clears and copies ------------------------------------------------------------------------------

void latte_render_clear(const u32* d) {
    if (!ready()) return;
    {
        float c4[4];
        memcpy(c4, d + 11, 16);
        DBG("clear %u color %08X fmt %03X %ux%u (%.2f %.2f %.2f %.2f) depth %08X\n", d[0], d[1], d[2], d[3], d[4], c4[0], c4[1], c4[2], c4[3], d[6]);
    }
    u32 what = d[0];
    const u32* c = d + 1;    // color: addr, fmt, w, h, slice
    const u32* z = d + 6;    // depth: same
    float rgba[4];
    memcpy(rgba, d + 11, 16);
    float depth;
    memcpy(&depth, d + 15, 4);
    u32 stencil = d[16];
    if ((what & 1) && c[0]) {
        int ri = find_rti(c[0], false);
        Surf* s = get_rt(c[0], c[1], c[2], c[3], false, ri >= 0 && g.rti[ri].w == c[2] && g.rti[ri].h == c[3] ? g.rti[ri].layers | (g.rti[ri].dim == 2 ? RT_3D : 0) : 1);
        if (s) {
            VkCommandBuffer cb = transfer_cmd();
            VkClearColorValue v;
            u32 nt = 0;
            LatteTexFormat tf;
            if (latte_tex_format(c[1], &tf)) nt = (c[1] & 0x300) == 0x100 ? 2 : (c[1] & 0x300) == 0x300 ? 1 : 0;
            if (nt == 2) for (int i = 0; i < 4; i++) v.uint32[i] = (u32)rgba[i];
            else if (nt == 1) for (int i = 0; i < 4; i++) v.int32[i] = (int32_t)rgba[i];
            else memcpy(v.float32, rgba, 16);
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (!s->vol && c[4] < s->layers) ? c[4] : 0, 1 };   // (3D: one array layer holds all slices -- a slice number as the layer was out of range)
            vkCmdClearColorImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            s->band_frame = g.frame + 1;   // (cleared whole: sides too)
            readback_note(s, ri);
        }
    }
    if ((what & 6) && z[0]) {
        int ri = find_rti(z[0], true);
        Surf* s = get_rt(z[0], z[1], z[2], z[3], true, ri >= 0 && g.rti[ri].w == z[2] && g.rti[ri].h == z[3] ? g.rti[ri].layers : 1);
        if (s) {
            s->wseq++;
            VkCommandBuffer cb = transfer_cmd();
            VkClearDepthStencilValue v = { depth < 0 ? 0 : depth > 1 ? 1 : depth, stencil };
            VkImageSubresourceRange range = { 0, 0, 1, z[4] < s->layers ? z[4] : 0, 1 };
            if (what & 2) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if ((what & 4) && s->stencil) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (range.aspectMask) vkCmdClearDepthStencilImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        }
    }
}

static void copy_image_layer(VkCommandBuffer cb, Img* src, u32 sl, Img* dst, u32 dl, u32 w, u32 h);
static void copy_image(VkCommandBuffer cb, Img* src, Img* dst, u32 w, u32 h) { copy_image_layer(cb, src, 0, dst, 0, w, h); }
static void copy_image_layer(VkCommandBuffer cb, Img* src, u32 sl, Img* dst, u32 dl, u32 w, u32 h) {
    if (sl >= src->layers) sl = 0;
    if (dl >= dst->layers) dl = 0;
    bool same_size = false;
    u32 sb = 0, db = 0;
    LatteTexFormat a, b;
    (void)a; (void)b;
    switch (src->format) { case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: sb = 4; break; default: break; }
    switch (dst->format) { case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: db = 4; break; default: break; }
    same_size = sb && sb == db && src->w == dst->w && src->h == dst->h;
    if (same_size || src->format == dst->format) {
        VkImageCopy ic;
        memset(&ic, 0, sizeof(ic));
        ic.srcSubresource.aspectMask = src->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : src->aspect;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.srcSubresource.baseArrayLayer = sl;
        ic.dstSubresource.baseArrayLayer = dl;
        ic.extent.width = w < src->w ? (w < dst->w ? w : dst->w) : (src->w < dst->w ? src->w : dst->w);
        ic.extent.height = h < src->h ? (h < dst->h ? h : dst->h) : (src->h < dst->h ? src->h : dst->h);
        ic.extent.depth = 1;
        vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        VkImageBlit bl;
        memset(&bl, 0, sizeof(bl));
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.srcSubresource.baseArrayLayer = sl;
        bl.dstSubresource.baseArrayLayer = dl;
        bl.srcOffsets[1].x = (int)src->w; bl.srcOffsets[1].y = (int)src->h; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int)dst->w; bl.dstOffsets[1].y = (int)dst->h; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
}

// A scaling/converting copy (the GPU converts formats, e.g. encodes sRGB).
static void blit_image(VkCommandBuffer cb, Img* src, Img* dst, u32 w, u32 h) {
    VkImageBlit bl;
    memset(&bl, 0, sizeof(bl));
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int)(w < src->w ? w : src->w);
    bl.srcOffsets[1].y = (int)(h < src->h ? h : src->h);
    bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = (int)dst->w;
    bl.dstOffsets[1].y = (int)dst->h;
    bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
}

// 1x1 single-colour images, stretched over rectangles: the GamePad
// screen's frame and its cursor.
enum { SOLID_DARK, SOLID_WHITE, SOLID_YELLOW, SOLID_MAGENTA, SOLID_N };
static Img g_solid[SOLID_N];
static bool g_solid_ok = false;
static void make_solids(void) {
    if (g_solid_ok) return;
    static const float col[SOLID_N][4] = { { 0.05f, 0.05f, 0.07f, 1 }, { 1, 1, 1, 1 }, { 1.0f, 0.82f, 0.1f, 1 }, { 1.0f, 0.0f, 1.0f, 1 } };
    for (int i = 0; i < SOLID_N; i++) {
        if (!make_image(&g_solid[i], VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
        VkCommandBuffer cb = transfer_cmd();
        VkClearColorValue v;
        memcpy(v.float32, col[i], sizeof(v.float32));
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, g_solid[i].image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    g_solid_ok = true;
}

void latte_render_copy_to_scan(const u32* d) {
    if (!ready()) return;
    u32 addr = d[0], w = d[1], h = d[2], target = d[5];
    bool tv = (target & 1) != 0;
    if (!tv && (target & 0xC)) g_drc_scan_addr = addr;   // (the GamePad pop-up watches it even while hidden)
    // GamePad screen: only kept while it's shown in the window (View button)
    if (!tv && (!(target & 0xC) || !g_vk.drc_on)) return;
    Surf* s = find_rt(addr, false);
    DBG("copy to %s from %08X %ux%u format %03X (%s)\n", tv ? "TV" : "GamePad", addr, w, h, d[3], s ? "known" : "unknown");
    if (tv) { g_ss.scan_copies++; g_ss.scan_src = addr; g_ss.scan_src_seen = s ? s->seen : 0; g_fz_src = s; if (s) s->scan_src = true; }
    if (!s) {
        static int warned = 0;
        if (tv && warned++ < 4) printf("[render] the game showed a buffer at %08X that was never drawn to\n", addr);
        return;
    }
    // sRGB TV buffer (0x100): the copy encodes linear -> sRGB on the way.
    // (Copied into an sRGB-format image -- the GPU does the encoding -- and
    // from there byte for byte into the picture we show.)
    static int srgb_mode = -1;   // SPLAT_GAMMA=0 / 1 forces it off / on
    if (srgb_mode < 0) { const char* e = getenv("SPLAT_GAMMA"); srgb_mode = e ? (atoi(e) ? 1 : 0) : 2; }
    bool srgb = srgb_mode == 2 ? !(target & 0x200) : srgb_mode == 1;   // (Splatoon's is sRGB)
    Img* dst = tv ? &g.tv : &g.drc;
    // (render scale: the picture is as big as the buffer was drawn -- smaller ones too: 0.5x wide was
    // squeezed into a 1280x720 picture and shown 16:9, with bars, instead of the window's shape)
    static int old_size = -1;
    if (old_size < 0) old_size = getenv("SPLAT_SCAN_OLD_SIZE") != NULL;
    if ((s->img.w >= w && s->img.h >= h) || ((s->scale != 1.0f || s->scalex != 1.0f) && !s->vol && !old_size)) { w = s->img.w; h = s->img.h; }
    if (!dst->image || dst->w != w || dst->h != h) {
        // (the old image isn't destroyed -- make_image starts a new one -- so nothing in flight needs waiting for;
        // the device-wide wait here cost ~100 ms per window resize)
        if (!make_image(dst, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
        printf("[render] %s picture is %ux%u\n", tv ? "TV" : "GamePad", w, h);
        if (!tv) make_solids();
    }
    static Img enc_tv, enc_drc;
    Img* enc = tv ? &enc_tv : &enc_drc;
    if (srgb && (!enc->image || enc->w != w || enc->h != h)) {
        if (!make_image(enc, VK_FORMAT_R8G8B8A8_SRGB, w, h, 1, VK_IMAGE_TYPE_2D, false, false, false)) srgb = false;
        else if (tv) printf("[render] TV buffer is sRGB: colors get the game's gamma curve\n");
    }
    VkCommandBuffer cb = transfer_cmd();
    if (srgb && s->img.format != VK_FORMAT_R8G8B8A8_SRGB) {
        blit_image(cb, &s->img, enc, w, h);
        barrier_all(cb);
        copy_image(cb, enc, dst, w, h);   // (same size class: raw bytes)
    } else {
        copy_image(cb, &s->img, dst, w, h);
    }
    if (tv) g.tv_valid = true;
    else g.drc_valid = true;
}

// GX2CopySurface / GX2ResolveAAColorBuffer. Sources and targets can each be
// a buffer the GPU drew (kept on the graphics card) or a texture in game
// memory: drawn -> drawn is a GPU copy; memory -> drawn uploads the texture
// into that slice; memory -> memory is done on the CPU (that's how games
// put loaded cube maps / texture arrays together).
typedef struct { u32 addr, fmt, w, h, slice, mode, pitch, swizzle, dim, slices; } CopySide;

static void copy_side(const u32* d, bool full, CopySide* o) {
    o->addr = d[0]; o->fmt = d[1]; o->w = d[2] ? d[2] : 1; o->h = d[3] ? d[3] : 1; o->slice = d[4]; o->mode = d[5];
    if (full) { o->pitch = d[6]; o->swizzle = d[7] & 0x700; o->dim = d[8] & 0xFF; o->slices = (d[8] >> 8) & 0xFFFF; }
    else { o->pitch = 0; o->swizzle = 0; o->dim = 1; o->slices = 1; }
    if (!o->slices) o->slices = 1;
}

static u32 side_layers(const CopySide* c) {
    if (c->dim == 3 || c->dim == 4 || c->dim == 5 || c->dim == 7) return c->slices > 64 ? 64 : c->slices;   // cube / arrays
    return 1;
}

void latte_render_copy_surface(const u32* d, u32 n) {
    if (!ready()) return;
    bool full = n >= 18;
    CopySide a, b;
    copy_side(d, full, &a);
    copy_side(d + (full ? 9 : 6), full, &b);
    Surf* s = find_rt_exact(a.addr, a.fmt, a.w, a.h);
    if (!s) s = find_rt(a.addr, false);
    if (!s) s = find_rt(a.addr, true);
    Surf* t = find_rt(b.addr, false);
    static int logged = 0;
    if (logged++ < 40)
        printf("[render] copy surface %08X %ux%u fmt %03X slice %u mode %u (%s) -> %08X %ux%u fmt %03X slice %u mode %u kind %u x%u (%s)\n",
               a.addr, a.w, a.h, a.fmt, a.slice, a.mode, s ? "drawn" : "memory", b.addr, b.w, b.h, b.fmt, b.slice, b.mode, b.dim, b.slices,
               t ? "drawn" : "memory");
    if (s) {   // from a drawn buffer
        if (s->depth) {   // depth buffer -> texture (GX2ConvertDepthBufferToTextureSurface):
            // a depth copy at the texture's address; shaders then read it like the
            // depth buffer itself (shadows, ambient occlusion, soft edges)
            Surf* d = get_rt(b.addr, s->gx2fmt, b.w, b.h, true, side_layers(&b));
            if (!d || d == s || getenv("SKIP_DEPTH_COPY")) return;
            d->wseq++;
            VkCommandBuffer cb = transfer_cmd();
            copy_image_layer(cb, &s->img, a.slice, &d->img, b.slice, (u32)(b.w * d->scalex + 0.5f), (u32)(b.h * d->scale + 0.5f));
            return;
        }
        if (!t || t == s) {
            if (t == s) return;
            t = get_rt(b.addr, b.fmt, b.w, b.h, false, side_layers(&b) | (b.dim == 2 ? RT_3D : 0));
            if (!t) return;
        }
        t->wseq++;
        t->band_frame = g.frame + 1;   // (written by a copy: left as the game made it)
        VkCommandBuffer cb = transfer_cmd();
        if ((s->scale != t->scale || s->scalex != t->scalex) && !s->vol && !t->vol) {   // (one drawn at render scale, the other not: the whole picture, scaled)
            VkImageBlit bl;
            memset(&bl, 0, sizeof(bl));
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.srcSubresource.layerCount = 1;
            bl.srcSubresource.baseArrayLayer = a.slice < s->layers ? a.slice : 0;
            bl.dstSubresource = bl.srcSubresource;
            bl.dstSubresource.baseArrayLayer = b.slice < t->layers ? b.slice : 0;
            bl.srcOffsets[1].x = (int)s->img.w; bl.srcOffsets[1].y = (int)s->img.h; bl.srcOffsets[1].z = 1;
            bl.dstOffsets[1].x = (int)t->img.w; bl.dstOffsets[1].y = (int)t->img.h; bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, t->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            return;
        }
        copy_image_layer(cb, &s->img, s->vol ? 0 : a.slice, &t->img, t->vol ? 0 : b.slice, (u32)(b.w * t->scalex + 0.5f), (u32)(b.h * t->scale + 0.5f));
        return;
    }
    // from a texture in memory
    if (!full || !a.addr || !b.addr) return;
    LatteTexFormat tf;
    if (!latte_tex_format(a.fmt, &tf)) return;
    // (only 2D-tiled surfaces carry swizzle bits in their address; others
    // can start anywhere -- masking those wrote over the game's data)
    bool a_macro = a.mode >= 4 && a.mode != 16, b_macro = b.mode >= 4 && b.mode != 16;
    LatteTexDesc sd = { a_macro ? a.addr & ~0x700u : a.addr, a.w, a.h, a.pitch, a.fmt & 0x3F, a.mode, a_macro ? a.swizzle | (a.addr & 0x700) : 0, a.slice + 1, 0 };
    u32 ew = tf.block ? (a.w + 3) / 4 : a.w, eh = tf.block ? (a.h + 3) / 4 : a.h;
    size_t bytes = (size_t)ew * eh * tf.elem_bytes;
    if (!bytes || bytes > (64u << 20)) return;
    if (g_cap_active) cap_mem(sd.addr, latte_tex_span(&sd, &tf));
    u8* px = (u8*)malloc(bytes);
    if (!px) return;
    if (!latte_tex_decode_slices(&sd, &tf, a.slice, 1, px, bytes)) { free(px); return; }
    if (t && !t->depth && !tf.block && t->img.format == (VkFormat)tf.vk_format && b.w <= t->w && b.h <= t->h && (t->scale != 1.0f || t->scalex != 1.0f) && !t->vol) {
        // into a buffer drawn at render scale: through an image of the game's
        // size, stretched over the whole target (the sky backdrop is copied in
        // this way; written straight in, it covered only the top-left part)
        static Img tmp;
        static bool tmp_ready;
        if (tmp.image && (tmp.w != a.w || tmp.h != a.h || tmp.format != (VkFormat)tf.vk_format)) { memset(&tmp, 0, sizeof(tmp)); tmp_ready = false; }   // (the old one is left behind)
        VkBuffer buf;
        u32 off;
        u8* up = arena((u32)bytes, 16, &buf, &off);
        if (up && (tmp.image || make_image(&tmp, (VkFormat)tf.vk_format, a.w, a.h, 1, VK_IMAGE_TYPE_2D, false, false, false))) {
            memcpy(up, px, bytes);
            VkCommandBuffer cb = transfer_cmd();
            if (!tmp.layers) tmp.layers = 1;
            if (!tmp_ready) { image_layout_init(cb, &tmp); tmp_ready = true; }
            VkBufferImageCopy copy;
            memset(&copy, 0, sizeof(copy));
            copy.bufferOffset = off;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = a.w;
            copy.imageExtent.height = a.h;
            copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cb, buf, tmp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            barrier_all(cb);
            VkImageBlit bl;
            memset(&bl, 0, sizeof(bl));
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.srcSubresource.layerCount = 1;
            bl.dstSubresource = bl.srcSubresource;
            bl.dstSubresource.baseArrayLayer = b.slice < t->layers ? b.slice : 0;
            bl.srcOffsets[1].x = (int)(a.w < b.w ? a.w : b.w); bl.srcOffsets[1].y = (int)(a.h < b.h ? a.h : b.h); bl.srcOffsets[1].z = 1;
            bl.dstOffsets[1].x = (int)(bl.srcOffsets[1].x * t->scalex + 0.5f); bl.dstOffsets[1].y = (int)(bl.srcOffsets[1].y * t->scale + 0.5f); bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, tmp.image, VK_IMAGE_LAYOUT_GENERAL, t->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
            g.need_barrier = true;
        }
    } else if (t && !t->depth && !tf.block && t->img.format == (VkFormat)tf.vk_format && b.w <= t->w && b.h <= t->h) {
        // into the drawn buffer's slice
        VkBuffer buf;
        u32 off;
        u8* up = arena((u32)bytes, 16, &buf, &off);
        if (up) {
            memcpy(up, px, bytes);
            VkCommandBuffer cb = transfer_cmd();
            VkBufferImageCopy copy;
            memset(&copy, 0, sizeof(copy));
            copy.bufferOffset = off;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.baseArrayLayer = t->vol ? 0 : (b.slice < t->layers ? b.slice : 0);
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset.z = t->vol ? (int)b.slice : 0;
            copy.imageExtent.width = a.w < b.w ? a.w : b.w;
            copy.imageExtent.height = a.h < b.h ? a.h : b.h;
            copy.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cb, buf, t->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        }
    }
    // and into game memory, where textures are read from
    LatteTexFormat df;
    if (latte_tex_format(b.fmt, &df) && df.src_bits == tf.src_bits && df.block == tf.block && a.w == b.w && a.h == b.h) {
        LatteTexDesc dd = { b_macro ? b.addr & ~0x700u : b.addr, b.w, b.h, b.pitch, b.fmt & 0x3F, b.mode, b_macro ? b.swizzle | (b.addr & 0x700) : 0, b.slice + 1, 0 };
        LatteTexFormat raw = tf;   // copy the game's bytes as they are
        raw.convert = 0;
        raw.elem_bytes = tf.src_bits / 8;
        if (tf.convert) {   // decoded data was converted: decode again raw
            LatteTexDesc rd = sd;
            u8* r2 = (u8*)malloc((size_t)ew * eh * raw.elem_bytes);
            if (r2 && latte_tex_decode_slices(&rd, &raw, a.slice, 1, r2, (size_t)ew * eh * raw.elem_bytes) &&
                latte_tex_encode_slices(&dd, &raw, b.slice, 1, r2, (size_t)ew * eh * raw.elem_bytes))
                wiiu_mem_mark(dd.addr, latte_tex_span(&dd, &raw));
            free(r2);
        } else if (latte_tex_encode_slices(&dd, &raw, b.slice, 1, px, bytes)) {
            wiiu_mem_mark(dd.addr, latte_tex_span(&dd, &raw));
        }
    }
    free(px);
}

// GX2Invalidate: the game says textures in this range may have changed.
// Splatoon invalidates all of memory twice a frame, so only textures whose
// memory was really written since they were last loaded are loaded again
// (re-hashing every texture every frame cost a lot of frame time).
// (per 1 MB region: the change counter when every texture in it was last
// checked -- a region nothing was written to since is skipped; 16 below the
// counter, for marks other threads were making at that moment)
static u32 g_inv_checked[4096];
void latte_render_invalidate(u32 addr, u32 size) {
    if (!g.ok || !size) return;
    u64 end = (u64)addr + size;
    for (u64 r = addr >> 20; r <= ((end - 1) >> 20) && r < 4096; r++) {
        bool whole = (u64)addr <= (r << 20) && end >= ((r + 1) << 20);
        if (whole && wiiu_mem_region_generation((u32)r) <= g_inv_checked[r]) continue;
        u32 gen_now = wiiu_mem_generation();
        if (whole) g_inv_checked[r] = gen_now > 16 ? gen_now - 16 : 0;
        TexBucket* b = &g_tex_idx[r];
        for (u32 i = 0; i < b->n; i++) {
            Surf* s = b->v[i];
            if (s->dirty) continue;
            // (level 0, or the mip levels: either may have been loaded anew)
            if (s->addr < end && addr < (u64)s->addr + s->span) {
                if (wiiu_mem_changed_since(s->addr, s->span, s->gen)) { s->dirty = true; g_tv_epoch++; g_tvc[3]++; continue; }
            }
            u32 ma = s->mip_addr & ~0x700u;
            if (s->mip_span && ma < end && addr < (u64)ma + s->mip_span && wiiu_mem_changed_since(ma, s->mip_span, s->gen)) { s->dirty = true; g_tv_epoch++; g_tvc[3]++; }
        }
    }
}

// --- Presenting --------------------------------------------------------------------------------------

static void destroy_swapchain(void) {
    if (!g.swapchain) return;
    vkDeviceWaitIdle(g_vk.device);
    vkDestroySwapchainKHR(g_vk.device, g.swapchain, NULL);
    g.swapchain = VK_NULL_HANDLE;
}

// Low input lag without present wait: right after presenting a frame the next image is acquired at once
// (blocking); with a 2-image swapchain that returns when the presented frame reaches the screen, which is
// when the game's next frame should start. The acquired image and its semaphore are kept for the next swap.
static struct { bool have; u32 index; VkSemaphore sem[4]; int which; } g_pre;
// An acquired swapchain image is drawn into only once the presentation engine has really let go of it (the
// acquire's fence signalled). vkAcquireNextImageKHR may return SUCCESS for an image the engine still holds: the
// acquire semaphore then signals only when the engine releases it -- never, while Windows isn't composing the
// window (a display turned off / reconfigured: Oct 25, every boot stalled at frame 8). The frame's command buffer
// used to wait on that semaphore, so the game's own fences never retired: a stall for good. Now that frame just
// isn't shown (the acquired image is kept for the next one). SPLAT_OLD_ACQUIRE=1: the old way.
// (sem: a ring -- a binary semaphore must not go into a new acquire while a submitted wait on it is still pending
// (VUID 01779); `last` is the one the latest successful acq_ready acquired with: the present's submit waits on it)
#define ACQ_SEMS 8
static struct { bool have, polled; u32 index, not_yet; VkSemaphore sem[ACQ_SEMS], last; int which; VkFence fence; } g_acq;
static void acq_reset(void) {   // (a new swapchain; the device is idle here. An acquire still pending: its objects are left alone)
    if (!g_acq.have) {
        if (g_acq.fence) vkDestroyFence(g_vk.device, g_acq.fence, NULL);
        for (int i = 0; i < ACQ_SEMS; i++) if (g_acq.sem[i]) vkDestroySemaphore(g_vk.device, g_acq.sem[i], NULL);
    }
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < ACQ_SEMS; i++) vkCreateSemaphore(g_vk.device, &si, NULL, &g_acq.sem[i]);
    g_acq.last = g_acq.sem[0];
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(g_vk.device, &fi, NULL, &g_acq.fence);
    g_acq.have = false;
    g_acq.not_yet = 0;
}
// Acquire (once) and ask whether the image is really free: true = present into *index, waiting on g_acq.sem.
static bool acq_ready(u64 acquire_timeout_ns, u64 fence_wait_ns, u32* index) {
    if (!g_acq.have) {
        u32 idx = 0;
        g_acq.which = (g_acq.which + 1) % ACQ_SEMS;
        VkResult r = vkAcquireNextImageKHR(g_vk.device, g.swapchain, acquire_timeout_ns, g_acq.sem[g_acq.which], g_acq.fence, &idx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { g_acq.have = true; g_acq.polled = false; g_acq.index = idx; }
        else if (r == VK_TIMEOUT || r == VK_NOT_READY) g_present_skips++;
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
        if (!g_acq.have) return false;
    }
    if (g.sc_rebuild) return false;
    // (the first look waits a moment: a free image's fence signals right away; later looks just peek)
    VkResult fr = vkWaitForFences(g_vk.device, 1, &g_acq.fence, VK_TRUE, g_acq.polled ? 0 : fence_wait_ns);
    g_acq.polled = true;
    if (fr != VK_SUCCESS) {
        g_present_skips++;
        g_present_skips_fence++;
        if (++g_acq.not_yet == 300) printf("[render] Windows isn't showing the window (its last picture hasn't been released for 300 frames): the game runs on, nothing shown meanwhile\n");
        return false;
    }
    vkResetFences(g_vk.device, 1, &g_acq.fence);
    if (g_acq.not_yet >= 300) printf("[render] the window is shown again (after %u frames)\n", g_acq.not_yet);
    g_acq.not_yet = 0;
    g_acq.have = false;
    *index = g_acq.index;
    g_acq.last = g_acq.sem[g_acq.which];
    return true;
}
static void pre_acquire_reset(void) {
    g_pre.have = false;
    for (int i = 0; i < 4; i++) {
        if (g_pre.sem[i]) vkDestroySemaphore(g_vk.device, g_pre.sem[i], NULL);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_vk.device, &si, NULL, &g_pre.sem[i]);
    }
}
static bool build_swapchain(void) {
    if (!g_vk.surface) return false;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.gpu, g_vk.surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 1280; ext.height = 720; }
    if (ext.width == 0 || ext.height == 0) return false;   // minimized
    vkDeviceWaitIdle(g_vk.device);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.gpu, g_vk.surface, &fn, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (fn > 64) fn = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.gpu, g_vk.surface, &fn, fmts);
    if (!fn) return false;
    VkSurfaceFormatKHR chosen = fmts[0];
    for (u32 i = 0; i < fn; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = fmts[i]; break; }
    uint32_t mn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.gpu, g_vk.surface, &mn, NULL);
    VkPresentModeKHR modes[16];
    if (mn > 16) mn = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.gpu, g_vk.surface, &mn, modes);
    // Vsync. SPLAT_UNCAPPED=1: mailbox when the driver has it (never waits, no
    // tearing), else "immediate" (never waits for the screen's refresh, may
    // tear); the game still paces itself at 60 (GX2WaitForVsync). Measured: no
    // faster in the plaza (the game never waited on the monitor), so off.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (g_vk.vsync_mode == 2 || (g_vk.vsync_mode == 0 && getenv("SPLAT_UNCAPPED"))) {
        for (u32 i = 0; i < mn; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            for (u32 i = 0; i < mn; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    }
    // Low input lag (settings): the fewest images the driver allows, so that acquiring the next image blocks
    // until the screen's refresh -- that moment ends the game's vsync wait (gx2_note_vblank). Only worth it
    // with 2 images (one shown, one being drawn); with more the queue just gets deeper.
    g_vk.present_wait = true;   // (the setting paces by the desktop compositor's refresh timing, gx2.c; always there on Windows)
    bool low_lat = false;   // (a 2-image swapchain with blocking acquires didn't pace anything: in a window this driver's acquires never block -- 180 fps)
    u32 want = low_lat ? caps.minImageCount : caps.minImageCount + 2;   // (+2: room for the game's clock drifting against the screen's, see the image acquire)
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > 8) want = 8;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = g_vk.surface;
    ci.minImageCount = want;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // (screen capture -- OBS -- copies from these)
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                       : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -(int)caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(g_vk.device, &ci, NULL, &sc) != VK_SUCCESS) return false;
    if (g.swapchain) vkDestroySwapchainKHR(g_vk.device, g.swapchain, NULL);
    g.swapchain = sc;
    g.sc_format = chosen.format;
    g.sc_extent = ext;
    g.sc_count = 0;
    vkGetSwapchainImagesKHR(g_vk.device, sc, &g.sc_count, NULL);
    if (g.sc_count > 8) g.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.device, sc, &g.sc_count, g.sc_images);
    for (u32 i = 0; i < 8; i++) {
        if (g.sc_done[i]) continue;
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_vk.device, &si, NULL, &g.sc_done[i]);
    }
    g.sc_rebuild = false;
    pre_acquire_reset();
    acq_reset();
    printf("[render] swapchain: %u images (driver minimum %u)%s\n", g.sc_count, caps.minImageCount, low_lat ? ", low input lag" : "");
    static bool said = false;
    if (!said) {
        said = true;
        printf("[render] window %ux%u, %s\n", ext.width, ext.height, mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox (no tearing)"
               : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "uncapped (no vsync)" : "vsync");
    }
    return true;
}

static void sc_barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// --- PNG files (frame dumps / screenshots) ----------------------------------------------------------

static void put32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static void write_chunk(FILE* f, const char* type, const u8* data, u32 len) {
    u8 hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef*)type, 4);
    if (len) crc = crc32(crc, data, len);
    u8 c[4];
    put32(c, (u32)crc);
    fwrite(c, 1, 4, f);
}

static void write_png(const char* path, const u8* rgba, u32 w, u32 h) {
    size_t raw_size = (size_t)(w * 3 + 1) * h;
    u8* raw = (u8*)malloc(raw_size);
    if (!raw) return;
    for (u32 y = 0; y < h; y++) {
        u8* row = raw + (size_t)y * (w * 3 + 1);
        row[0] = 0;
        for (u32 x = 0; x < w; x++) memcpy(row + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    uLongf zsize = compressBound((uLong)raw_size);
    u8* z = (u8*)malloc(zsize);
    if (!z || compress2(z, &zsize, raw, (uLong)raw_size, 6) != Z_OK) { free(raw); free(z); return; }
    FILE* f = fopen(path, "wb");
    if (f) {
        static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        fwrite(sig, 1, 8, f);
        u8 ihdr[13];
        put32(ihdr, w);
        put32(ihdr + 4, h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        write_chunk(f, "IHDR", ihdr, 13);
        write_chunk(f, "IDAT", z, (u32)zsize);
        write_chunk(f, "IEND", NULL, 0);
        fclose(f);
        printf("[render] saved %s (%ux%u)\n", path, w, h);
    }
    free(raw);
    free(z);
}

// (the PNG is compressed and written on its own thread: doing it here held
// the drawing thread -- and so the game -- for ~300 ms per F12)
typedef struct { char path[64]; u8* rgba; u32 w, h; } PngJob;
static int png_job_run(void* arg) {
    PngJob* j = (PngJob*)arg;
    write_png(j->path, j->rgba, j->w, j->h);
    free(j->rgba);
    free(j);
    return 0;
}
static void finish_readback(Sub* s) {
    if (!s->rb_pending) return;
    s->rb_pending = false;
    static u32 n = 0;
    char path[64];
    static u32 run_id;   // (per program run: test runs don't overwrite the player's F12 pictures)
    if (!run_id) run_id = (u32)(hnow_ns() / 1000000000ull) % 100000u;
    snprintf(path, sizeof(path), "%s/frame_%05u_%04u.png", images_dir(), run_id, n++);
    printf("[render] screenshot %u at frame %llu (clock %.1f)\n", n - 1, (unsigned long long)g.frame, hnow_ns() / 1e9);
    PngJob* j = (PngJob*)malloc(sizeof(PngJob));
    size_t bytes = (size_t)s->rb_w * s->rb_h * 4;
    if (j && (j->rgba = (u8*)malloc(bytes)) != NULL) {
        memcpy(j->path, path, sizeof(j->path));
        memcpy(j->rgba, s->rb_ptr, bytes);
        j->w = s->rb_w; j->h = s->rb_h;
        hthrd_t t;
        if (hthrd_create(&t, png_job_run, j, 1u << 20)) {
#ifdef _WIN32
            CloseHandle(t);
#endif
            return;
        }
        free(j->rgba);
    }
    free(j);
    write_png(path, (const u8*)s->rb_ptr, s->rb_w, s->rb_h);   // (no thread: the old way)
}

static void record_readback(VkCommandBuffer cb, Sub* s) {
    Img* src = (g_vk.drc_on && g.drc.image) ? &g.drc : present_source();   // (the GamePad screen when it is shown)
    u32 size = src->w * src->h * 4;
    if (s->rb_size < size) {
        if (s->rb) buffer_destroy_later(s->rb, s->rb_mem);
        if (!host_buffer_cached(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->rb, &s->rb_mem, &s->rb_ptr)) { s->rb = VK_NULL_HANDLE; s->rb_size = 0; return; }
        s->rb_size = size;
    }
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = src->w;
    c.imageExtent.height = src->h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, s->rb, 1, &c);
    s->rb_w = src->w;
    s->rb_h = src->h;
    s->rb_pending = true;
}

// --- Flicker catcher -------------------------------------------------------------------------------
// Things that blink for a frame (shadows in one part of a level, say) are
// hard to catch with F12 by hand. Every shown frame a 64x36 thumbnail of the
// picture is copied back (a few KB, read once its submission is done anyway).
// When the picture changes and then changes back (A B A) while the view
// holds still otherwise, that's logged, and the first two times in a session
// an F12-style capture is taken by itself (debug_output/capture_f*.splatcap).
// Off unless SPLAT_FLICKER_CATCH=1 (it showed up as ~4% of the drawing thread).
#define TH_W 64
#define TH_H 36
static Img g_thumb;
static u8 g_th_prev[2][TH_W * TH_H];   // [0] the frame before, [1] the one before that
static u64 g_th_prev_seq[2] = { ~0ull, ~0ull };
static u64 g_th_seq;                   // shown frames
static u32 g_flicker_seen, g_flicker_caps;
static u64 g_flicker_last_cap;

static bool flicker_catch_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SPLAT_FLICKER_CATCH") != NULL;
    return on != 0;
}

static void thumb_record(VkCommandBuffer cb, Sub* s) {
    if (!g_thumb.image && !make_image(&g_thumb, VK_FORMAT_R8G8B8A8_UNORM, TH_W, TH_H, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
    if (!s->th_buf && !host_buffer_cached(TH_W * TH_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->th_buf, &s->th_mem, &s->th_ptr)) { s->th_buf = VK_NULL_HANDLE; return; }
    barrier_all(cb);
    VkImageBlit bl;
    memset(&bl, 0, sizeof(bl));
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int)g.tv.w; bl.srcOffsets[1].y = (int)g.tv.h; bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = TH_W; bl.dstOffsets[1].y = TH_H; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, g.tv.image, VK_IMAGE_LAYOUT_GENERAL, g_thumb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_all(cb);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = TH_W;
    c.imageExtent.height = TH_H;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g_thumb.image, VK_IMAGE_LAYOUT_GENERAL, s->th_buf, 1, &c);
    s->th_pending = true;
    s->th_seq = g_th_seq;
}

static double thumb_diff(const u8* a, const u8* b, double* changed) {
    u32 sum = 0, big = 0;
    for (u32 i = 0; i < TH_W * TH_H; i++) {
        int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        sum += (u32)d;
        if (d > 24) big++;
    }
    if (changed) *changed = 100.0 * big / (TH_W * TH_H);
    return (double)sum / (TH_W * TH_H);
}

static void thumb_process(Sub* s) {
    const u8* p = (const u8*)s->th_ptr;
    u8 cur[TH_W * TH_H];
    for (u32 i = 0; i < TH_W * TH_H; i++) cur[i] = (u8)((p[i * 4] * 54 + p[i * 4 + 1] * 183 + p[i * 4 + 2] * 19) >> 8);
    {   // SPLAT_FLICKER_TEST=1: pretend every 500th shown frame came out darker (checks the catcher itself)
        static int test = -1;
        if (test < 0) test = getenv("SPLAT_FLICKER_TEST") != NULL;
        if (test && s->th_seq % 500 == 250) for (u32 i = 0; i < TH_W * TH_H; i++) cur[i] = (u8)(cur[i] / 2);
    }
    if (g_th_prev_seq[0] + 1 == s->th_seq && g_th_prev_seq[1] + 2 == s->th_seq) {
        double changed;
        double d_ab = thumb_diff(g_th_prev[1], g_th_prev[0], &changed);   // A -> B
        double d_ba = thumb_diff(g_th_prev[0], cur, NULL);                // B -> A'
        double d_aa = thumb_diff(g_th_prev[1], cur, NULL);                // A -> A'
        double lo = d_ab < d_ba ? d_ab : d_ba;
        if (d_ab > 2.0 && d_ba > 2.0 && d_aa < 0.35 * lo) {
            g_flicker_seen++;
            if (g_flicker_seen <= 30)
                printf("[flicker] frame %llu: the picture changed (%.1f, %.1f%% of it) and changed back (%.1f) the next frame\n",
                       (unsigned long long)g.frame, d_ab, changed, d_ba);
            static int cap_on = -1;   // (SPLAT_FLICKER_CAPTURE=1: also take captures -- 100-600 MB each)
            if (cap_on < 0) cap_on = getenv("SPLAT_FLICKER_CAPTURE") != NULL;
            if (cap_on && g_flicker_caps < 2 && (!g_flicker_last_cap || g.frame - g_flicker_last_cap > 600)) {
                g_flicker_caps++;
                g_flicker_last_cap = g.frame;
                g_vk.screenshot = 1;   // (as if F12 was pressed: the next two frames are captured)
                printf("[flicker] taking a capture (like F12) so it can be looked at\n");
            }
        }
    }
    memcpy(g_th_prev[1], g_th_prev[0], sizeof(cur));
    g_th_prev_seq[1] = g_th_prev_seq[0];
    memcpy(g_th_prev[0], cur, sizeof(cur));
    g_th_prev_seq[0] = s->th_seq;
}

// --- Pause watcher -----------------------------------------------------------------------------------
// In story levels the game's pause menu (+) is only on the GamePad, and the
// TV picture stops: with the GamePad screen hidden that looked like a freeze
// (music on, everything else still). After + is pressed, a 32x18 copy of the
// TV picture is looked at every 8 frames; if it holds still the GamePad
// screen is shown by itself, and hidden again once the TV picture moves (or
// the player toggles it with View / M).
#define PW_W 32
#define PW_H 18
static Img g_pw_img;
// --- GamePad pop-up -----------------------------------------------------------------------------
// The game sometimes puts a screen on the GamePad alone and stops the TV's world
// (the + pause / upgrade menu, the Sunken Scrolls, prompts at a kettle): with the
// GamePad screen hidden that looked exactly like a freeze -- 60 fps, HUD moving,
// the world still -- for days. When the TV picture holds still for ~2 s while the
// GamePad's picture keeps changing, the GamePad screen is shown by itself; it goes
// away once the TV moves again. View / M still toggle it (hiding it during such a
// stop keeps it hidden until the TV moves); settings row "GamePad pop-up" or
// SPLAT_DRC_POPUP=0 turns it off.
static int popcount8(u32 v) { int n = 0; for (v &= 0xFF; v; v &= v - 1) n++; return n; }
static Img g_pw2_img;
static u8 g_pw2_prev[32 * 18];
static bool g_pw2_have;
static u32 g_pw2_hist;            // last samples' "GamePad picture changed" bits
static int g_popup_declined;      // the player hid the popped-up screen: not again until the TV moves
static u32 g_tv_moving;
static u8 g_pw_prev[PW_W * PW_H];
static bool g_pw_have;
static int g_pw_still;
static int g_pw_presses_seen;
static u64 g_pw_armed_until;   // (frame: watching after a + press)

static u32 g_pw_draws_ref;    // the draws per frame once the pause menu settled (the world resuming changes it)
static int g_pw_moving;
static void pause_watch_record(VkCommandBuffer cb, Sub* s) {
    if (g_vk.plus_presses != g_pw_presses_seen) {
        g_pw_presses_seen = g_vk.plus_presses;
        // (+ is the game's pause; the GamePad overlay is only ever opened by View / M -- the user's wish --
        // even though a story scene's pause menu is drawn on the GamePad alone)
        g_pw_armed_until = g.frame + 300;   // (5 s)
        g_pw_have = false;
        g_pw_still = 0;
    }
    if (g_vk.drc_auto == 1) {
        if (g.frame > g_pw_armed_until - 240 && !g_pw_draws_ref) g_pw_draws_ref = g_vk.draws_last_frame ? g_vk.draws_last_frame : 1;   // (1 s after +: the menu is up)
        if (g_pw_draws_ref && g.frame % 8 == 0) {
            u32 dn = g_vk.draws_last_frame;
            if (dn > g_pw_draws_ref + g_pw_draws_ref / 3 || dn + g_pw_draws_ref / 3 < g_pw_draws_ref) g_pw_moving++; else g_pw_moving = 0;
            if (g_pw_moving >= 12) {   // (~1.5 s of a different draw count: the world is back)
                g_vk.drc_on = 0;
                g_vk.drc_auto = 0;
                printf("[display] the world draws differently again (%u draws, was %u): GamePad screen hidden\n", dn, g_pw_draws_ref);
            }
        }
    }
    // (sampled every 8th frame all the time now: the frozen-scene watchdog below uses it too)
    if (g.frame % 8 || !g.tv_valid) return;
    if (!g_pw_img.image && !make_image(&g_pw_img, VK_FORMAT_R8G8B8A8_UNORM, PW_W, PW_H, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
    if (!s->pw_buf && !host_buffer_cached(PW_W * PW_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->pw_buf, &s->pw_mem, &s->pw_ptr)) { s->pw_buf = VK_NULL_HANDLE; return; }
    barrier_all(cb);
    VkImageBlit bl;
    memset(&bl, 0, sizeof(bl));
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int)g.tv.w; bl.srcOffsets[1].y = (int)g.tv.h; bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = PW_W; bl.dstOffsets[1].y = PW_H; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, g.tv.image, VK_IMAGE_LAYOUT_GENERAL, g_pw_img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_all(cb);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = PW_W;
    c.imageExtent.height = PW_H;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g_pw_img.image, VK_IMAGE_LAYOUT_GENERAL, s->pw_buf, 1, &c);
    s->pw_pending = true;
    if ((g_center_still >= 4 || g_vk.drc_auto == 2) && g_drc_scan_addr && !g_vk.drc_no_popup) {   // (GamePad pop-up: its picture moving?)
        Surf* d = find_rt(g_drc_scan_addr, false);
        if (d && (g_pw2_img.image || make_image(&g_pw2_img, VK_FORMAT_R8G8B8A8_UNORM, PW_W, PW_H, 1, VK_IMAGE_TYPE_2D, false, false, false)) &&
            (s->pw2_buf || host_buffer_cached(PW_W * PW_H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s->pw2_buf, &s->pw2_mem, &s->pw2_ptr))) {
            VkImageBlit b2;
            memset(&b2, 0, sizeof(b2));
            b2.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b2.srcSubresource.layerCount = 1;
            b2.dstSubresource = b2.srcSubresource;
            b2.srcOffsets[1].x = (int)d->img.w; b2.srcOffsets[1].y = (int)d->img.h; b2.srcOffsets[1].z = 1;
            b2.dstOffsets[1].x = PW_W; b2.dstOffsets[1].y = PW_H; b2.dstOffsets[1].z = 1;
            barrier_all(cb);
            vkCmdBlitImage(cb, d->img.image, VK_IMAGE_LAYOUT_GENERAL, g_pw2_img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b2, VK_FILTER_LINEAR);
            barrier_all(cb);
            vkCmdCopyImageToBuffer(cb, g_pw2_img.image, VK_IMAGE_LAYOUT_GENERAL, s->pw2_buf, 1, &c);
            s->pw2_pending = true;
        } else if (!s->pw2_buf) s->pw2_buf = VK_NULL_HANDLE;
    } else if (g_center_still == 0) { g_pw2_have = false; g_pw2_hist = 0; }
}
static void drc_watch_process(Sub* s) {
    const u8* p = (const u8*)s->pw2_ptr;
    u8 cur[PW_W * PW_H];
    for (u32 i = 0; i < PW_W * PW_H; i++) cur[i] = (u8)((p[i * 4] * 54 + p[i * 4 + 1] * 183 + p[i * 4 + 2] * 19) >> 8);
    if (g_pw2_have) {
        u32 sum = 0;
        for (u32 i = 0; i < PW_W * PW_H; i++) sum += (u32)(cur[i] > g_pw2_prev[i] ? cur[i] - g_pw2_prev[i] : g_pw2_prev[i] - cur[i]);
        g_pw2_hist = (g_pw2_hist << 1) | ((double)sum / (PW_W * PW_H) >= 0.6 ? 1u : 0u);
    }
    memcpy(g_pw2_prev, cur, sizeof(cur));
    g_pw2_have = true;
}

// The frozen-scene watchdog: the middle of the TV picture (the 3D scene;
// the HUD sits at the edges) unchanged for 2 s while the game runs -> a
// diagnostic dump in the log and a frame capture (as Ctrl+F12), once per
// freeze and again every 10 s while it lasts. SPLAT_FREEZE_TEST=1: one dump
// at frame 1200 (checks the dump itself).
void hle_calls_snapshot(void);
void hle_report_calls_since_snapshot(FILE* out, const char* title);
void profiler_report_now(FILE* out);
void audio_report_voices(FILE* out);
void gx2_report_state(FILE* out);
static void freeze_dump(double secs, double center_diff, double whole_diff) {
    printf("[freeze] the TV's 3D scene hasn't changed for %.0f s (middle of the picture differs %.2f per pixel, whole picture %.2f) at frame %llu\n",
           secs, center_diff, whole_diff, (unsigned long long)g.frame);
    const LatteSyncStats* t = &g_ss_last;
    printf("[freeze] last second: scene draws %u, other draws %u, skipped (shader %u, pipeline %u), copies to the TV %u from %08X (that buffer last drawn in frame %llu), "
           "texture memo hits %u, depth-copy reuses %u, frames held %u, skipped draws (total) %u\n",
           t->scene_draws, t->other_draws, t->skip_shader, t->skip_pipe, t->scan_copies, t->scan_src, (unsigned long long)t->scan_src_seen, t->memo_hits, t->fb_reuse,
           g_held_frames, g_skipped_draws);
    if (g_fz_src)
        printf("[freeze] TV source buffer %08X: %ux%u format %03X, seq %llu, last drawn frame %llu, set up by draw %llu, rendered %d, recurring %d, epoch %u\n",
               g_fz_src->addr, g_fz_src->w, g_fz_src->h, g_fz_src->gx2fmt, (unsigned long long)g_fz_src->seq, (unsigned long long)g_fz_src->seen,
               (unsigned long long)g_fz_src->wdraw, g_fz_src->rendered, g_fz_src->recurring, g_tv_epoch);
    int n = 0;
    for (u32 b = 0; b < 4096; b++)
        for (Surf* s = g_rth[b]; s; s = s->anext)
            if (s->seen + 2 > g.frame && n++ < 16)
                printf("[freeze]   drawn lately: %08X %ux%u x%u format %03X %s seq %llu last drawn frame %llu%s\n", s->addr, s->w, s->h, s->layers, s->gx2fmt,
                       s->depth ? "depth" : "color", (unsigned long long)s->seq, (unsigned long long)s->seen, s->recurring ? " (every frame)" : "");
    printf("[freeze] shaders/pipelines built on the helper threads so far %llu, by the drawing thread %u; screenshot taken (press Ctrl+F12 for a capture)\n",
           (unsigned long long)hatomic_load64(&g_async_built), g_now_builds);
    printf("[freeze] GamePad screen shown %d (auto %d), + presses %d, render scale %d, AA %d, settings open %d, wide %d, FOV %+d, performance mode %d; GamePad picture changed in %d of the last 8 samples\n",
           g_vk.drc_on, g_vk.drc_auto, g_vk.plus_presses, g_vk.render_scale, g_vk.aa_mode, g_vk.settings_open, g_vk.wide_mode, g_vk.fov_add, g_vk.perf_mode,
           popcount8(g_pw2_hist));
    {   // (the frozen TV world was a NaN camera: the safety net's count, native_funcs.c cam_guard)
        extern volatile int splat_cam_repairs, splat_cam_last_frame;
        extern volatile u32 splat_cam_last_self;
        printf("[freeze] camera guard: %d NaN/infinity repairs so far%s", splat_cam_repairs, splat_cam_repairs ? "" : " (the cameras held good numbers)\n");
        if (splat_cam_repairs) printf(" (last: camera %08X at frame %d)\n", splat_cam_last_self, splat_cam_last_frame);
        extern void nan_trap_report(FILE* out);
        nan_trap_report(stdout);   // (SPLAT_NAN_TRAP=1 only: where NaNs were made)
    }
    gx2_report_state(stdout);
    audio_report_voices(stdout);
    hle_report_calls_since_snapshot(stdout, "  system functions called since the picture stopped (about the last second)");
    {   // (who drew in that stretch -- compare with the last "[hle] draw / display-list calls by caller" before the freeze)
        extern void hle_report_callers(FILE* out, const char* title, u32 frames);
        hle_report_callers(stdout, "  draw / display-list calls by caller since the last report", 0);
    }
    profiler_report_now(stdout);
    fflush(stdout);
    g_vk.screenshot = 1;
}
static void pause_watch_process(Sub* s) {
    const u8* p = (const u8*)s->pw_ptr;
    u8 cur[PW_W * PW_H];
    for (u32 i = 0; i < PW_W * PW_H; i++) cur[i] = (u8)((p[i * 4] * 54 + p[i * 4 + 1] * 183 + p[i * 4 + 2] * 19) >> 8);
    if (g_pw_have) {
        u32 sum = 0, csum = 0, cn = 0;
        for (u32 i = 0; i < PW_W * PW_H; i++) sum += (u32)(cur[i] > g_pw_prev[i] ? cur[i] - g_pw_prev[i] : g_pw_prev[i] - cur[i]);
        for (u32 y = PW_H / 4; y < PW_H - PW_H / 4; y++)
            for (u32 x = PW_W / 4; x < PW_W - PW_W / 4; x++) {
                u32 i = y * PW_W + x;
                csum += (u32)(cur[i] > g_pw_prev[i] ? cur[i] - g_pw_prev[i] : g_pw_prev[i] - cur[i]);
                cn++;
            }
        double d = (double)sum / (PW_W * PW_H), cd = cn ? (double)csum / cn : 0;
        {   // the frozen-scene watchdog (samples are 8 frames apart)
            static u32 center_still, reported_at;
            static int test = -1;
            if (test < 0) test = getenv("SPLAT_FREEZE_TEST") != NULL;
            if (cd < 0.25) center_still++; else { center_still = 0; reported_at = 0; }
            g_center_still = center_still;
            {   // GamePad pop-up (see g_drc_scan_addr)
                static int env = -1;
                if (env < 0) { const char* e = getenv("SPLAT_DRC_POPUP"); env = e ? atoi(e) : 1; if (!env) g_vk.drc_no_popup = 1; }
                static int was_auto;
                if (was_auto == 2 && g_vk.drc_auto == 0 && !g_vk.drc_on && center_still) g_popup_declined = 1;   // (the player hid it)
                if (!center_still) g_popup_declined = 0;
                g_tv_moving = center_still ? 0 : g_tv_moving + 1;
                int moves = popcount8(g_pw2_hist);
                bool drawing = (g_ss_last.scene_draws + g_ss_last.other_draws) >= 20 * 50;
                if (!g_vk.drc_on && !g_vk.drc_no_popup && !g_popup_declined && drawing && center_still >= 15 && moves >= 4) {
                    g_vk.drc_on = 1;
                    g_vk.drc_auto = 2;
                    printf("[display] the TV picture stopped while the GamePad's keeps changing (the game is showing something on the GamePad): GamePad screen shown -- View / M hides it\n");
                }
                if (g_vk.drc_auto == 2 && (g_tv_moving >= 3 || g_vk.drc_no_popup)) {
                    g_vk.drc_on = 0;
                    g_vk.drc_auto = 0;
                    g_pw2_hist = 0;
                    printf("[display] the TV picture moves again: GamePad screen hidden\n");
                }
                was_auto = g_vk.drc_auto;
            }
            if (center_still == 8 || (center_still < 8 && g.frame % 64 == 0)) hle_calls_snapshot();   // (the dump lists what the game called in about the last second)
            // (only while a real scene is being drawn: loading screens and menus are still too)
            bool due = center_still >= 15 && (g_ss_last.scene_draws + g_ss_last.other_draws) >= 20 * 50 && (!reported_at || center_still >= reported_at + 75);   // (while the game draws at all)
            if (test == 1 && g.frame >= 1200) { test = 2; due = true; }
            if (due && !(g.frame < g_pw_armed_until && g_vk.drc_auto)) { reported_at = center_still; freeze_dump(center_still * 8 / 60.0, cd, d); }
        }
        if (d < 0.6) g_pw_still++;
        else g_pw_still = 0;
        if (g_vk.want_dump) { g_vk.want_dump = 0; freeze_dump(0.0, cd, d); }   // (F9: a dump on request)
    }
    memcpy(g_pw_prev, cur, sizeof(cur));
    g_pw_have = true;
}

// Gallery view (F1): the CPU-drawn picture of the game's textures.
static VkBuffer g_gal_buf;
static VkDeviceMemory g_gal_mem;
static void* g_gal_ptr;
static u32 g_gal_size;

static bool draw_gallery(VkCommandBuffer cb, VkImage target) {
    u32 w = g.sc_extent.width, h = g.sc_extent.height, size = w * h * 4;
    if (g_gal_size < size) {
        if (g_gal_buf) buffer_destroy_later(g_gal_buf, g_gal_mem);
        if (!host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_gal_buf, &g_gal_mem, &g_gal_ptr)) { g_gal_buf = VK_NULL_HANDLE; g_gal_size = 0; return false; }
        g_gal_size = size;
    }
    gallery_update();
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    static const u8 bg[3] = { 24, 20, 32 };
    gallery_compose((u8*)g_gal_ptr, w, h, w * 4, bgra, bg);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g_gal_buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return true;
}

// On-screen stats bar (F3 hides it): text drawn on the CPU with a small
// anti-aliased font and copied into the top-left corner of the window.
#include "overlay_font.h"
#include "frame_tracker.h"
#define BAR_MAXW 1024
#define BAR_H (OVF_H + 8)
static VkBuffer g_bar_buf[NSUB];
static VkDeviceMemory g_bar_mem[NSUB];
static void* g_bar_ptr[NSUB];

typedef struct { u8* px; u32 w; bool bgra; int x; } Bar;

static void bar_text(Bar* b, const char* t, u8 r, u8 gg, u8 bl) {
    for (; *t; t++) {
        int ci = (u8)*t - OVF_FIRST;
        if (ci < 0 || ci >= OVF_COUNT) ci = 0;
        int w = ovf_width[ci];
        const u8* a = ovf_alpha + ovf_offset[ci];
        for (int y = 0; y < OVF_H; y++)
            for (int x = 0; x < w; x++) {
                int px = b->x + x, py = 4 + y;
                if (px < 0 || px >= (int)b->w) continue;
                u32 al = a[y * w + x];
                if (!al) continue;
                u8* p = b->px + (py * BAR_MAXW + px) * 4;
                u8 c0 = b->bgra ? bl : r, c2 = b->bgra ? r : bl;
                p[0] = (u8)((p[0] * (255 - al) + c0 * al) / 255);
                p[1] = (u8)((p[1] * (255 - al) + gg * al) / 255);
                p[2] = (u8)((p[2] * (255 - al) + c2 * al) / 255);
            }
        b->x += w;
    }
}

static int text_width(const char* t) {
    int w = 0;
    for (; *t; t++) { int ci = (u8)*t - OVF_FIRST; w += ovf_width[ci >= 0 && ci < OVF_COUNT ? ci : 0]; }
    return w;
}

// Later transfer writes to the window's picture land on top of earlier ones.
static void transfer_order(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

// Stretch src (all of it) over a rectangle of the window's picture.
static void blit_rect_src(VkCommandBuffer cb, Img* src, float sx0, float sy0, float sx1, float sy1, VkImage target, int x, int y, int w, int h, VkFilter filter);
static void blit_rect(VkCommandBuffer cb, Img* src, VkImage target, int x, int y, int w, int h, VkFilter filter) {
    blit_rect_src(cb, src, 0, 0, (float)src->w, (float)src->h, target, x, y, w, h, filter);
}
// (a part of the source: sx0,sy0 - sx1,sy1 in source pixels)
static void blit_rect_src(VkCommandBuffer cb, Img* src, float sx0, float sy0, float sx1, float sy1, VkImage target, int x, int y, int w, int h, VkFilter filter) {
    int W = (int)g.sc_extent.width, H = (int)g.sc_extent.height;
    int x1 = x + w, y1 = y + h;
    if (w <= 0 || h <= 0) return;
    // (clipped to the window; the source shrinks to match)
    if (x < 0) { sx0 += (sx1 - sx0) * (float)(-x) / (float)(x1 - x); x = 0; }
    if (y < 0) { sy0 += (sy1 - sy0) * (float)(-y) / (float)(y1 - y); y = 0; }
    if (x1 > W) { sx1 -= (sx1 - sx0) * (float)(x1 - W) / (float)(x1 - x); x1 = W; }
    if (y1 > H) { sy1 -= (sy1 - sy0) * (float)(y1 - H) / (float)(y1 - y); y1 = H; }
    if (x1 <= x || y1 <= y) return;
    VkImageBlit bl;
    memset(&bl, 0, sizeof(bl));
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[0].x = (int)sx0; bl.srcOffsets[0].y = (int)sy0;
    bl.srcOffsets[1].x = (int)(sx1 + 0.5f); bl.srcOffsets[1].y = (int)(sy1 + 0.5f); bl.srcOffsets[1].z = 1;
    if (bl.srcOffsets[1].x <= bl.srcOffsets[0].x) bl.srcOffsets[1].x = bl.srcOffsets[0].x + 1;
    if (bl.srcOffsets[1].y <= bl.srcOffsets[0].y) bl.srcOffsets[1].y = bl.srcOffsets[0].y + 1;
    bl.dstOffsets[0].x = x; bl.dstOffsets[0].y = y;
    bl.dstOffsets[1].x = x1; bl.dstOffsets[1].y = y1; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, filter);
}

// The GamePad screen over the game (View button), with the cursor that
// touches it.
// One coming draw seen by the look-ahead (latte_cp_lookahead; the registers
// are the ones it will have): its shaders are translated now and compiled on
// the helper threads (get_shader), and its pipeline -- the same key the draw
// will make, from the buffer table's formats instead of the buffers
// themselves, which aren't created here -- is built there too.
void latte_render_prefetch_draw(const LatteDraw* dr) {
    if (!ready() || g_pf_n >= 3000) return;
    if (R(VGT_GS_MODE) & 3) return;
    bool so_on = (R(VGT_STRMOUT_EN) & 1) != 0;
    bool raster_off = ((R(PA_CL_CLIP_CNTL) >> 22) & 1) != 0;
    if (raster_off && !so_on) return;
    LatteShaderKey k;
    u64 vsh, psh, fsh;
    if (!shader_key_for(dr, &k, &vsh, &psh, &fsh)) return;
    ShaderEnt* vs = get_vs(&k, vsh, fsh);
    ShaderEnt* ps = get_shader(true, hash_key_ps(&k, psh), &k);
    if (vs->failed || ps->failed) return;
    bool so_draw = so_on && vs->info.so_mask;
    if (raster_off && !so_draw) return;
    PipeKey pk;
    memset(&pk, 0, sizeof(pk));
    u32 target_mask = R(CB_TARGET_MASK), shader_mask = R(CB_SHADER_MASK);
    u32 color_ctl = R(CB_COLOR_CONTROL);
    bool colors_off = ((color_ctl >> 4) & 7) == 1;
    u32 ncolor = 0;
    for (u32 i = 0; i < 8; i++) {   // (as the draw's attachment loop)
        if (!(ps->info.color_outputs & (1u << i))) continue;
        u32 mask = (target_mask >> (4 * i)) & (shader_mask >> (4 * i)) & 0xF;
        if (colors_off) mask = 0;
        u32 base = R(CB_COLOR0_BASE + i) << 8, info = R(CB_COLOR0_INFO + i);
        if (!base || !((info >> 2) & 0x3F) || !mask) continue;
        int ri = find_rti(base, false);
        if (ri < 0) continue;
        ncolor = i + 1;
        pk.color_fmt[i] = (u32)color_format(g.rti[ri].fmt);
        pk.color_mask[i] = mask;
        if ((color_ctl >> (8 + i)) & 1) {
            pk.blend_on |= 1u << i;
            pk.blend[i] = R(CB_BLEND0_CONTROL + i);
        }
    }
    u32 dctl = R(DB_DEPTH_CONTROL);
    bool z_on = (dctl >> 1) & 1, st_on = dctl & 1, has_depth = false;
    int dri = R(DB_DEPTH_BASE) ? find_rti(R(DB_DEPTH_BASE) << 8, true) : -1;
    if ((z_on || st_on || ncolor == 0) && dri >= 0 && (R(DB_DEPTH_INFO) & 7)) {
        bool stencil = false;
        VkFormat dfmt = depth_format(g.rti[dri].fmt, &stencil);
        pk.depth_fmt = (u32)dfmt;
        pk.stencil_fmt = stencil ? (u32)dfmt : 0;
        has_depth = true;
    }
    if (raster_off) {
        memset(pk.color_fmt, 0, sizeof(pk.color_fmt));
        memset(pk.color_mask, 0, sizeof(pk.color_mask));
        pk.blend_on = 0;
        ncolor = 0;
        pk.depth_fmt = pk.stencil_fmt = 0;
        has_depth = false;
    }
    if (!ncolor && !has_depth && !so_draw) return;
    u32 cls;
    (void)topology(dr->prim, &cls);
    pk.topo_class = cls;
    pk.raster_off = raster_off ? 1 : 0;
    u32 clip = R(PA_CL_CLIP_CNTL);
    pk.depth_clamp = g_vk.depth_clamp && ((clip >> 26) & 3) == 3;
    pk.logic_op = (color_ctl >> 16) & 0xFF;
    u32 su = R(PA_SU_SC_MODE_CNTL);
    pk.poly_mode = (((su >> 3) & 3) && ((su >> 5) & 7) == 1) ? 1 : 0;
    // there already? (both shaders done: the finished key can be looked up)
    if (!hatomic_load64(&vs->pending) && !hatomic_load64(&ps->pending) && vs->mod && ps->mod) {
        PipeKey fk = pk;
        fk.vs = vs->mod;
        fk.ps = ps->mod;
        if (pipe_lookup(&fk)) return;
    }
    u64 h = pf_hash(&pk);
    if (pf_find(vs, ps, &pk, h)) return;
    PrefEnt* f = (PrefEnt*)calloc(1, sizeof(PrefEnt));
    f->vs = vs;
    f->ps = ps;
    f->k = pk;
    f->hash = h;
    f->pending = 1;
    f->next = g_pf_tab[h & (PF_BUCKETS - 1)];
    g_pf_tab[h & (PF_BUCKETS - 1)] = f;
    f->all = g_pf_all;
    g_pf_all = f;
    g_pf_n++;
    g_pf_made++;
    async_queue_pf(f);
}

static void draw_drc(VkCommandBuffer cb, VkImage target) {
    if (!g_vk.drc_on || !g.drc_valid || !g_solid_ok) return;
    int x, y, w, h;
    drc_overlay_rect((int)g.sc_extent.width, (int)g.sc_extent.height, &x, &y, &w, &h);
    int b = h / 90 + 2;   // frame
    blit_rect(cb, &g_solid[SOLID_DARK], target, x - b, y - b, w + 2 * b, h + 2 * b, VK_FILTER_NEAREST);
    transfer_order(cb);
    {   // (wide screen draws the GamePad's buffers in the picture's shape: show their middle 854x480)
        float cw = g.drc.w > 854 ? 854.0f : (float)g.drc.w, ch = g.drc.h > 480 ? 480.0f : (float)g.drc.h;
        float ox = ((float)g.drc.w - cw) * 0.5f, oy = ((float)g.drc.h - ch) * 0.5f;
        blit_rect_src(cb, &g.drc, ox, oy, ox + cw, oy + ch, target, x, y, w, h, VK_FILTER_LINEAR);
    }
    transfer_order(cb);
    if (g_vk.drc_play) return;   // (playing on it: no touch cursor)
    int cx = x + (int)((long long)g_vk.drc_cursor_x * w / 10000), cy = y + (int)((long long)g_vk.drc_cursor_y * h / 10000);
    int l = h / 28 + 4, t = h / 300 + 1;   // arm length, half thickness
    blit_rect(cb, &g_solid[SOLID_DARK], target, cx - l - 2, cy - t - 2, 2 * l + 5, 2 * t + 5, VK_FILTER_NEAREST);
    blit_rect(cb, &g_solid[SOLID_DARK], target, cx - t - 2, cy - l - 2, 2 * t + 5, 2 * l + 5, VK_FILTER_NEAREST);
    transfer_order(cb);
    Img* c = &g_solid[g_vk.drc_touching ? SOLID_YELLOW : SOLID_WHITE];
    blit_rect(cb, c, target, cx - l, cy - t, 2 * l + 1, 2 * t + 1, VK_FILTER_NEAREST);
    blit_rect(cb, c, target, cx - t, cy - l, 2 * t + 1, 2 * l + 1, VK_FILTER_NEAREST);
    transfer_order(cb);
}

// --- Anti-aliasing (FXAA on the finished picture) --------------------------------------------------
// A full-screen pass over the TV picture into an image of the same size,
// which is then shown (and screenshotted) instead of the TV picture.
// Settings panel "Anti-aliasing", SPLAT_AA=1 at start.
static struct {
    bool tried, ok;
    Img out;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkDescriptorSet sets[NSUB];
    VkSampler sampler;
    u64 ready_frame;   // (the frame `out` holds)
} g_fx;
static const char* k_fx_vs =
    "#version 450\n"
    "layout(location=0) out vec2 uv;\n"
    "void main(){ vec2 p = vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2); uv = p; gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char* k_fx_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec2 inv; } pc;\n"
    "float luma(vec3 c){ return dot(c, vec3(0.299,0.587,0.114)); }\n"
    "void main(){\n"
    "  vec3 rgbM = texture(tex, uv).rgb;\n"
    "  vec3 rgbNW = texture(tex, uv + vec2(-1,-1)*pc.inv).rgb;\n"
    "  vec3 rgbNE = texture(tex, uv + vec2(1,-1)*pc.inv).rgb;\n"
    "  vec3 rgbSW = texture(tex, uv + vec2(-1,1)*pc.inv).rgb;\n"
    "  vec3 rgbSE = texture(tex, uv + vec2(1,1)*pc.inv).rgb;\n"
    "  float lM=luma(rgbM), lNW=luma(rgbNW), lNE=luma(rgbNE), lSW=luma(rgbSW), lSE=luma(rgbSE);\n"
    "  float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))), lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));\n"
    "  vec2 dir = vec2(-((lNW+lNE)-(lSW+lSE)), ((lNW+lSW)-(lNE+lSE)));\n"
    "  float dirReduce = max((lNW+lNE+lSW+lSE)*(0.25*(1.0/8.0)), 1.0/128.0);\n"
    "  float rcpDirMin = 1.0/(min(abs(dir.x),abs(dir.y))+dirReduce);\n"
    "  dir = clamp(dir*rcpDirMin, vec2(-8.0), vec2(8.0)) * pc.inv;\n"
    "  vec3 rgbA = 0.5*(texture(tex, uv+dir*(1.0/3.0-0.5)).rgb + texture(tex, uv+dir*(2.0/3.0-0.5)).rgb);\n"
    "  vec3 rgbB = rgbA*0.5 + 0.25*(texture(tex, uv+dir*-0.5).rgb + texture(tex, uv+dir*0.5).rgb);\n"
    "  float lB = luma(rgbB);\n"
    "  o = vec4((lB<lMin || lB>lMax) ? rgbA : rgbB, 1.0);\n"
    "}\n";
static VkShaderModule fx_module(bool fragment, const char* src) {
    size_t words = 0;
    u32* spv = glsl_to_spirv(fragment, src, &words);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = words * 4;
    ci.pCode = spv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.device, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    free(spv);
    return m;
}
static VkPipeline fx_pipeline(const char* fs_src, bool blend);
static bool fx_init(void) {
    if (g_fx.tried) return g_fx.ok;
    g_fx.tried = true;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 1;
    dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(g_vk.device, &dl, NULL, &g_fx.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_fx.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_fx.layout) != VK_SUCCESS) return false;
    if (!(g_fx.pipe = fx_pipeline(k_fx_fs, false))) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSUB };
    VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pc.maxSets = NSUB;
    pc.poolSizeCount = 1;
    pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(g_vk.device, &pc, NULL, &g_fx.pool) != VK_SUCCESS) return false;
    for (int i = 0; i < NSUB; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_fx.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_fx.dsl;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &g_fx.sets[i]) != VK_SUCCESS) return false;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.25f;
    if (vkCreateSampler(g_vk.device, &sci, NULL, &g_fx.sampler) != VK_SUCCESS) return false;
    g_fx.ok = true;
    printf("[render] anti-aliasing (FXAA) ready\n");
    return true;
}
// A full-screen pipeline for g_fx.layout (one texture, 8 bytes of push constants) drawing into RGBA8
// (blend: over what is there, by the shader's alpha).
static VkPipeline fx_pipeline_fmt(const char* fs_src, bool blend, VkFormat cf, VkPipelineLayout lay);
static VkPipeline fx_pipeline(const char* fs_src, bool blend) { return fx_pipeline_fmt(fs_src, blend, VK_FORMAT_R8G8B8A8_UNORM, g_fx.layout); }
static VkPipeline fx_pipeline_fmt(const char* fs_src, bool blend, VkFormat cf, VkPipelineLayout lay) {
    VkShaderModule vs = fx_module(false, k_fx_vs), fs = fx_module(true, fs_src);
    if (!vs || !fs) { printf("[render] a full-screen pass's shaders didn't compile\n"); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof(ba));
    ba.colorWriteMask = 0xF;
    if (blend && lay && lay == g_blend_tm_layout) {   // (the 3D clouds onto the scene: picture * transmittance + light; alpha kept)
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = lay;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.device, g.pcache, 1, &pi, NULL, &pipe);
    vkDestroyShaderModule(g_vk.device, vs, NULL);
    vkDestroyShaderModule(g_vk.device, fs, NULL);
    if (r != VK_SUCCESS) { printf("[render] a full-screen pass's pipeline didn't build (%d)\n", (int)r); return VK_NULL_HANDLE; }
    return pipe;
}
// The sky picture: a 360-degree panorama (u = around, v = from straight up to straight down), drawn every frame
// at 1024x512. Clouds are a layer above, seen along each direction; the noise is a 256x256 tiling texture made at
// start (texture reads instead of maths per pixel); lit from the game's own sun.
static const char* k_skytex_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D orig;\n"   // (the game's own sky picture for this level)
    "layout(push_constant) uniform P { vec4 sun; vec4 prm; } pc;\n"   // sun.xyz = towards the sun; prm = time, azimuth offset, azimuth sign, preset (1 Vibrant, 2 Storm)
    "void main() {\n"
    "  float az = uv.x * 6.2831853 * pc.prm.z + pc.prm.y, el = (0.5 - uv.y) * 3.14159265;\n"
    "  vec3 dir = vec3(cos(el) * sin(az), sin(el), cos(el) * cos(az));\n"
    "  vec3 sun = normalize(pc.sun.xyz);\n"
    "  float mu = max(dot(dir, sun), 0.0);\n"
    "  bool storm = pc.prm.w > 1.5;\n"
    // the level's own sky, smoothed (its clouds go: ours are drawn in 3D), kept as the colour base so each level keeps
    // its own light -- Kelp Dome's evening, the plaza's noon
    "  vec3 base = vec3(0.0);\n"
    "  for (int i = 0; i < 8; i++) base += textureLod(orig, vec2(uv.x + float(i) * 0.125, uv.y), 0.0).rgb;\n"
    "  base = clamp(base * 0.125, 0.0, 1.0);\n"
    "  float l = dot(base, vec3(0.299, 0.587, 0.114));\n"
    "  vec3 col = storm ? mix(vec3(l), base, 0.40) * vec3(0.44, 0.46, 0.50) + vec3(0.07, 0.08, 0.10)\n"   // (storm: the same light, grey and dim)
    "                   : mix(vec3(l), base, 1.35) * 1.06;\n"   // (vibrant: the same light, more of it)
    "  col += (storm ? vec3(1.0, 0.86, 0.66) : vec3(1.0, 0.95, 0.85)) * (pow(mu, 6.0) * (storm ? 0.22 : 0.30) + pow(mu, 60.0) * 0.5);\n"
    "  col = mix(col, vec3(1.0, 0.97, 0.90), smoothstep(0.9992, 0.9996, mu));\n"
    "  o = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
    "}\n";
// The grade (after the game's tone-map, before the HUD): contrast, colour, vignette; light shafts from the sun (the
// bright picture smeared towards the sun's place on the screen); rain streaks with Sky & light 2.
static const char* k_grade_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"   // a = time, rain, shafts strength, aspect; b = sun on screen (uv), -, -
    "float h1(float x) { return fract(sin(x * 12.9898) * 43758.5453); }\n"
    "float rain(vec2 p, float t, float sc, float sp, float dens, float asp) {\n"
    "  vec2 q = vec2((p.x * asp + p.y * 0.12) * sc, p.y * sc * 0.06 - t * sp);\n"
    "  float col = floor(q.x), hh = h1(col + 7.0 * floor(sc));\n"
    "  float y = fract(q.y + hh * 13.0), x = fract(q.x) - 0.5;\n"
    "  return smoothstep(0.06, 0.0, abs(x)) * smoothstep(0.0, 0.03, y) * smoothstep(0.30, 0.04, y) * step(1.0 - dens, hh);\n"
    "}\n"
    "void main() {\n"
    "  vec4 s = texture(tex, uv); vec3 c = s.rgb;\n"
    "  vec3 add = vec3(0.0);\n"
    "  if (pc.a.z > 0.0) {\n"
    "    vec2 dl = (pc.b.xy - uv) * (1.0 / 20.0), q = uv; float dec = 1.0, acc = 0.0;\n"
    "    for (int i = 0; i < 20; i++) { q += dl; vec3 v = texture(tex, q).rgb; acc += max(dot(v, vec3(0.333)) - 0.60, 0.0) * dec; dec *= 0.93; }\n"
    "    add = vec3(1.0, 0.88, 0.68) * acc * 0.16 * pc.a.z;\n"
    "  }\n"
    "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  vec2 d = uv - 0.5;\n"
    "  vec3 g;\n"
    "  if (pc.b.z > 1.5) {\n"   // (Storm: a little grittier -- some colour out, contrast, cool shadows, light vignette; the level's own light stays)
    "    g = mix(vec3(l), c, 0.88);\n"
    "    g = mix(g, g * g * (3.0 - 2.0 * g), 0.45);\n"
    "    g *= mix(vec3(0.94, 0.98, 1.03), vec3(1.03, 1.0, 0.95), smoothstep(0.12, 0.75, l));\n"
    "    g *= 1.0 - 0.22 * pow(dot(d, d) * 2.0, 1.2);\n"
    "  } else {\n"   // (Vibrant: punchy and clean -- more colour, a little contrast, light vignette)
    "    g = mix(vec3(l), c, 1.18);\n"
    "    g = mix(g, g * g * (3.0 - 2.0 * g), 0.25);\n"
    "    g *= 1.0 - 0.18 * pow(dot(d, d) * 2.0, 1.4);\n"
    "  }\n"
    "  g += add;\n"
    "  if (pc.a.y > 0.0) {\n"
    "    float t = pc.a.x, r = rain(uv, t, 55.0, 1.9, 0.45, pc.a.w) + 0.7 * rain(uv, t, 95.0, 2.6, 0.40, pc.a.w) + 0.45 * rain(uv, t, 160.0, 3.3, 0.35, pc.a.w);\n"
    "    g = mix(g, vec3(0.78, 0.81, 0.86), clamp(r * 0.30 * pc.a.y, 0.0, 1.0));\n"
    "  }\n"
    "  o = vec4(clamp(g, 0.0, 1.0), s.a);\n"
    "}\n";
// (both passes: one texture, 32 bytes of push constants)
static struct { bool tried, ok; VkPipelineLayout layout; VkSampler repeat; VkDescriptorPool pool; VkDescriptorSet sets[NSUB * 2];
                VkPipeline tex_pipe, grade_pipe; VkFormat grade_fmt; Img noise, sky, tmp; bool tmp_ready; u64 sky_frame; } g_sx;
static bool sx_init(void) {
    if (g_sx.tried) return g_sx.ok;
    g_sx.tried = true;
    if (!fx_init()) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_fx.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_sx.layout) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    if (vkCreateSampler(g_vk.device, &sci, NULL, &g_sx.repeat) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSUB * 2 };
    VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pc.maxSets = NSUB * 2;
    pc.poolSizeCount = 1;
    pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(g_vk.device, &pc, NULL, &g_sx.pool) != VK_SUCCESS) return false;
    for (int i = 0; i < NSUB * 2; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_sx.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_fx.dsl;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &g_sx.sets[i]) != VK_SUCCESS) return false;
    }
    if (!(g_sx.tex_pipe = fx_pipeline_fmt(k_skytex_fs, false, VK_FORMAT_R8G8B8A8_UNORM, g_sx.layout))) return false;
    if (!make_image(&g_sx.noise, VK_FORMAT_R8_UNORM, 256, 256, 1, VK_IMAGE_TYPE_2D, false, false, false)) return false;
    if (!make_image(&g_sx.sky, VK_FORMAT_R8G8B8A8_UNORM, 1024, 512, 1, VK_IMAGE_TYPE_2D, false, true, false)) return false;
    // the noise: smooth value noise on a 64x64 lattice, tiling (cubic between the lattice points)
    VkBuffer buf;
    u32 off;
    u8* up = arena(256 * 256, 16, &buf, &off);
    if (!up) return false;
    static u8 lat[64][64];
    u32 seed = 0x2545F491u;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { seed = seed * 1664525u + 1013904223u; lat[y][x] = (u8)(seed >> 24); }
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int x0 = x >> 2, y0 = y >> 2;
            float fx = (x & 3) / 4.0f, fy = (y & 3) / 4.0f;
            fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
            float a = lat[y0][x0], b = lat[y0][(x0 + 1) & 63], c = lat[(y0 + 1) & 63][x0], d = lat[(y0 + 1) & 63][(x0 + 1) & 63];
            up[y * 256 + x] = (u8)(a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy + 0.5f);
        }
    VkCommandBuffer cb = transfer_cmd();
    image_layout_init(cb, &g_sx.noise);
    image_layout_init(cb, &g_sx.sky);
    barrier_all(cb);
    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = off;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = 256;
    copy.imageExtent.height = 256;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, buf, g_sx.noise.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    barrier_all(cb);
    g.need_barrier = true;
    g_sx.ok = true;
    printf("[sky] sky and grade passes ready\n");
    return true;
}
// A full-screen pass of ours: `pipe` into `out` (its whole image), reading `in` through `smp`, with 32 bytes of numbers.
static void sx_draw_view(VkPipeline pipe, Img* out, VkImageView in_view, VkSampler smp, const float pcv[8], int set);
static void sx_draw(VkPipeline pipe, Img* out, Img* in, VkFormat in_fmt, VkSampler smp, const float pcv[8], int set) {
    sx_draw_view(pipe, out, sample_view(in, VK_IMAGE_VIEW_TYPE_2D, in_fmt, 0 | (1 << 3) | (2 << 6) | (3 << 9)), smp, pcv, set);
}
static void sx_draw_view(VkPipeline pipe, Img* out, VkImageView in_view, VkSampler smp, const float pcv[8], int set) {
    VkCommandBuffer cb = transfer_cmd();
    VkDescriptorImageInfo di = { smp, in_view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorSet ds = g_sx.sets[(g.cur < 0 ? 0 : g.cur) * 2 + set];
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.device, 1, &wr, 0, NULL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = att_view(out, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = out->w;
    ri.renderArea.extent.height = out->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)out->w, (float)out->h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { out->w, out->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_sx.layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, g_sx.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pcv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_dyn.valid = false;   // (the game's next draw sets its pipeline, state and bindings again)
    g.need_barrier = true;
}
static float sky_time(void) { return (float)fmod((double)pf_now() / 1e9, 20000.0); }
// The game's sun (its light cameras, emote.c), or SPLAT_SKY_SUN="azimuth,elevation" in degrees.
static void sky_sun(float s[3]) {
    static int env = -1;
    static float ea, ee;
    if (env < 0) { const char* e = getenv("SPLAT_SKY_SUN"); env = e && sscanf(e, "%f,%f", &ea, &ee) == 2; ea *= 3.14159265f / 180.0f; ee *= 3.14159265f / 180.0f; }
    int splat_sun_now(float d[3]);
    if (env || !splat_sun_now(s)) {
        float a = env ? ea : 0.6f, e = env ? ee : 0.5f;
        s[0] = cosf(e) * sinf(a); s[1] = sinf(e); s[2] = cosf(e) * cosf(a);
    }
}
// Our sky picture, drawn once per frame on first use.
static VkImageView sky_texture_view(void) {
    if (!sx_init()) return VK_NULL_HANDLE;
    if (g_sx.sky_frame != g.frame + 1) {
        g_sx.sky_frame = g.frame + 1;
        // (how the dome's panorama is turned against the world: SPLAT_SKY_MAP="offset degrees,sign")
        static float moff = -1000.0f, msign = 1.0f;
        if (moff < -999.0f) { moff = 0.0f; const char* e = getenv("SPLAT_SKY_MAP"); if (e) sscanf(e, "%f,%f", &moff, &msign); moff *= 3.14159265f / 180.0f; }
        float pcv[8];
        sky_sun(pcv);
        pcv[3] = 0.0f;
        pcv[4] = sky_time(); pcv[5] = moff; pcv[6] = msign; pcv[7] = (float)g_vk.sky_mode;
        if (!sky_orig_fresh()) { g_sx.sky_frame = 0; return VK_NULL_HANDLE; }   // (the game's picture not seen lately: the game's own sky stays)
        sx_draw_view(g_sx.tex_pipe, &g_sx.sky, g_sky_orig_view, g_sx.repeat, pcv, 0);
        static int logged;
        if (logged++ < 2) printf("[sky] sky picture drawn (1024x512)\n");
    }
    return sample_view(&g_sx.sky, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 0 | (1 << 3) | (2 << 6) | (3 << 9));
}
// The grade: the shown buffer copied, then drawn back over itself through k_grade_fs.
static void sky_grade_pass(Surf* s) {
    if (!sx_init() || g.cur < 0) return;
    if (!g_sx.grade_pipe || g_sx.grade_fmt != s->img.format) {
        g_sx.grade_pipe = fx_pipeline_fmt(k_grade_fs, false, s->img.format, g_sx.layout);   // (an old one for another format is left behind: once, if ever)
        g_sx.grade_fmt = s->img.format;
        if (!g_sx.grade_pipe) return;
    }
    if (g_sx.tmp.image && (g_sx.tmp.w != s->img.w || g_sx.tmp.h != s->img.h || g_sx.tmp.format != s->img.format)) { memset(&g_sx.tmp, 0, sizeof(g_sx.tmp)); g_sx.tmp_ready = false; }
    if (!g_sx.tmp.image && !make_image(&g_sx.tmp, s->img.format, s->img.w, s->img.h, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
    VkCommandBuffer cb = transfer_cmd();
    if (!g_sx.tmp_ready) { image_layout_init(cb, &g_sx.tmp); g_sx.tmp_ready = true; barrier_all(cb); }
    VkImageCopy ic;
    memset(&ic, 0, sizeof(ic));
    ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
    ic.extent.width = s->img.w;
    ic.extent.height = s->img.h;
    ic.extent.depth = 1;
    vkCmdCopyImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, g_sx.tmp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    barrier_all(cb);
    // the sun's place on the screen (from the gameplay camera): light shafts only while it's in front
    float pcv[8] = { sky_time(), 0.0f, 0.0f, (float)s->img.w / (float)(s->img.h ? s->img.h : 1), 0.5f, 0.5f, (float)g_vk.sky_mode, 0.0f };
    {
        float cp[3], fw[3], sn[3];
        int fresh = 0;
        void splat_emote_camera_now(float pos[3], float dir[3], int* fresh);
        splat_emote_camera_now(cp, fw, &fresh);
        extern volatile int splat_cam_fovy_now_x10;
        sky_sun(sn);
        float rx = -fw[2], rz = fw[0], rl = sqrtf(rx * rx + rz * rz);   // (right = forward x up)
        if (0 && fresh && rl > 1e-4f && splat_cam_fovy_now_x10 > 0 && g_sky_frame == g.frame + 1) {   // (screen-space shafts retired: the volume pass's)
            rx /= rl; rz /= rl;
            float up[3] = { -rz * fw[1], rz * fw[0] - rx * fw[2], rx * fw[1] };   // (right x forward)
            float fz = sn[0] * fw[0] + sn[1] * fw[1] + sn[2] * fw[2];
            float ty = tanf(splat_cam_fovy_now_x10 * 0.05f * 3.14159265f / 180.0f), tx = ty * pcv[3];
            if (fz > 0.05f) {
                float sx = (sn[0] * rx + sn[2] * rz) / (fz * tx), sy = (sn[0] * up[0] + sn[1] * up[1] + sn[2] * up[2]) / (fz * ty);
                pcv[4] = 0.5f + 0.5f * sx; pcv[5] = 0.5f - 0.5f * sy;
                float off = fmaxf(fabsf(sx), fabsf(sy));
                pcv[2] = fminf(1.0f, (fz - 0.05f) * 4.0f) * (off < 1.0f ? 1.0f : fmaxf(0.0f, 1.0f - (off - 1.0f) * 1.5f));
            }
        }
    }
    sx_draw(g_sx.grade_pipe, &s->img, &g_sx.tmp, s->img.format, g_fx.sampler, pcv, 1);
    static int logged;
    if (logged++ < 2) printf("[sky] grade pass over %08X (%ux%u)\n", s->addr, s->img.w, s->img.h);
}

// --- Motion blur (settings SHADERS "Motion blur": off / light / strong; SPLAT_MOTION_BLUR=0|1|2) ----------------------
// Camera motion blur from the camera's TURN between the shown frame and the one before (emote.c's per-frame camera):
// each pixel's view ray is put into the previous frame's camera, which gives where that point was on the screen; the
// picture is averaged along that path (12 taps, centred). Rotation only -- turning is where blur shows; moving straight
// changes little on screen. After the game's tone-map, before the HUD (the HUD stays sharp). Kept gentle (a fraction of
// the frame's motion, capped length), nothing flashes; skipped on camera cuts (respawn, super jump: a turn of more than
// 25 degrees in one frame) and in the two-player Battle Dojo (the TV is the other player's camera).
static const char* k_mblur_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"   // a.xyz previous forward (current camera's x right, y up, z forward), a.w world-up y | b.x world-up z, tanX, tanY, strength
    "void main() {\n"
    "  vec2 n = uv * 2.0 - 1.0;\n"
    "  vec3 r = vec3(n.x * pc.b.y, -n.y * pc.b.z, 1.0);\n"
    "  vec3 F = normalize(pc.a.xyz), W = vec3(0.0, pc.a.w, pc.b.x);\n"
    "  vec3 R = cross(W, F); float rl = length(R);\n"
    "  vec4 c = texture(tex, uv);\n"
    "  if (rl < 1e-4) { o = c; return; }\n"
    "  R /= rl; vec3 U = cross(F, R);\n"
    "  vec3 q = vec3(dot(r, R), dot(r, U), dot(r, F));\n"
    "  if (q.z <= 0.05) { o = c; return; }\n"
    "  vec2 pn = vec2(q.x / (q.z * pc.b.y), -q.y / (q.z * pc.b.z));\n"   // (where this point was on the screen, -1..1)
    "  vec2 v = (pn - n) * 0.5 * pc.b.w;\n"
    "  vec2 dp = (uv - vec2(0.5, 0.60)) * vec2(1.6, 1.0);\n"   // (the player stays in the middle, a little low: turning the camera
    "  v *= smoothstep(0.07, 0.32, length(dp));\n"              // orbits it, so it barely moves on the screen -- kept sharp)
    "  float L = length(v), mx = 0.06 * pc.b.w;\n"
    "  if (L > mx) v *= mx / L;\n"
    "  if (L < 0.0008) { o = c; return; }\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int i = 0; i < 12; i++) acc += texture(tex, uv + v * (float(i) / 11.0 - 0.5)).rgb;\n"
    "  o = vec4(acc / 12.0, c.a);\n"
    "}\n";
static VkPipeline g_mb_pipe;
static VkFormat g_mb_fmt;
static void motion_blur_pass(Surf* s) {
    if (!g_vk.motion_blur || !sx_init() || g.cur < 0) return;
    extern int splat_bot_in_battle(void);
    extern char splat_scene_map[64];
    if (splat_bot_in_battle() || (g_vk.pro_pad && battle_scene_index(splat_scene_map) >= 0)) return;   // (two-player Dojo)
    int splat_camera_for_frame(u32 frame, float pos[3], float dir[3], float* fovy_deg, float* nr, float* fr);
    float cp[3], fw[3], pp[3], pf[3], fovy = 0, pfovy = 0, nr, fr;
    if (!splat_camera_for_frame(g_present_swap, cp, fw, &fovy, &nr, &fr) || fovy <= 1.0f) return;
    if (!splat_camera_for_frame(g_present_swap - 1, pp, pf, &pfovy, &nr, &fr) || pfovy <= 1.0f) return;
    float fl = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]), pl = sqrtf(pf[0] * pf[0] + pf[1] * pf[1] + pf[2] * pf[2]);
    if (fl < 1e-4f || pl < 1e-4f) return;
    for (int i = 0; i < 3; i++) { fw[i] /= fl; pf[i] /= pl; }
    float cosang = fw[0] * pf[0] + fw[1] * pf[1] + fw[2] * pf[2];
    if (cosang < 0.906f) return;   // (> 25 degrees in a frame: a cut, not a turn)
    if (cosang > 0.9999995f) return;   // (still: nothing to blur)
    float rx = -fw[2], rz = fw[0], rl = sqrtf(rx * rx + rz * rz);   // (right = forward x up, as the volume pass)
    if (rl < 1e-4f) return;
    rx /= rl; rz /= rl;
    float up[3] = { -rz * fw[1], rz * fw[0] - rx * fw[2], rx * fw[1] };   // (right x forward)
    if (!g_mb_pipe || g_mb_fmt != s->img.format) {
        g_mb_pipe = fx_pipeline_fmt(k_mblur_fs, false, s->img.format, g_sx.layout);
        g_mb_fmt = s->img.format;
        if (!g_mb_pipe) return;
    }
    if (g_sx.tmp.image && (g_sx.tmp.w != s->img.w || g_sx.tmp.h != s->img.h || g_sx.tmp.format != s->img.format)) { memset(&g_sx.tmp, 0, sizeof(g_sx.tmp)); g_sx.tmp_ready = false; }
    if (!g_sx.tmp.image && !make_image(&g_sx.tmp, s->img.format, s->img.w, s->img.h, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
    VkCommandBuffer cb = transfer_cmd();
    if (!g_sx.tmp_ready) { image_layout_init(cb, &g_sx.tmp); g_sx.tmp_ready = true; barrier_all(cb); }
    VkImageCopy ic;
    memset(&ic, 0, sizeof(ic));
    ic.srcSubresource.aspectMask = ic.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = ic.dstSubresource.layerCount = 1;
    ic.extent.width = s->img.w;
    ic.extent.height = s->img.h;
    ic.extent.depth = 1;
    vkCmdCopyImage(cb, s->img.image, VK_IMAGE_LAYOUT_GENERAL, g_sx.tmp.image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    barrier_all(cb);
    float ty = tanf(fovy * 0.5f * 3.14159265f / 180.0f), tx = ty * (float)s->img.w / (float)(s->img.h ? s->img.h : 1);
    float pcv[8] = {
        pf[0] * rx + pf[2] * rz, pf[0] * up[0] + pf[1] * up[1] + pf[2] * up[2], pf[0] * fw[0] + pf[1] * fw[1] + pf[2] * fw[2],   // previous forward, in the current camera
        up[1], fw[1],                                                                                                       // world up, in the current camera (y, z)
        tx, ty, g_vk.motion_blur >= 2 ? 0.5f : 0.25f };
    sx_draw(g_mb_pipe, &s->img, &g_sx.tmp, s->img.format, g_fx.sampler, pcv, 1);
    static int logged;
    if (logged++ < 2) printf("[render] motion blur over %08X (%ux%u), %s\n", s->addr, s->img.w, s->img.h, g_vk.motion_blur >= 2 ? "strong" : "light");
}

// --- The 3D clouds (SHADERS "3D clouds"): a cloud layer with thickness at real heights in the world, marched per
// pixel from the camera (the same game frame's camera, emote.c) to the scene's depth -- in front of buildings when the
// camera is up among them (free camera), behind them otherwise. Half resolution, then blended into the scene buffer
// (picture * transmittance + light) right after the sky dome, before the game's own depth of field / glow / tone-map.
// Noise: two tiling 3D textures made at start on a thread of their own (a 128^3 Perlin-Worley shape, a 32^3 Worley
// detail); lit by the game's sun (its light cameras) with self-shadowing (a few steps towards the sun).
// The same pass draws the haze with light shafts and the clouds' shadows on the ground, from a cloud shadow map made
// first each frame: sunlight through the whole layer, for a 24000-unit square of the cloud base around the camera
// (its texels fixed in the world: no crawling edges when the camera moves).
// push constants: c0 cam pos, time | c1 forward, tanX | c2 right, tanY | c3 sun, near | c4 far, base, top, coverage |
// c5 preset, steps, haze density, haze ground height | c6 wind x, wind z, density, sigma |
// c7 edge width (small = crisp cumulus edges), top taper start (low = rounded towers), effects, many-bounce light
#define VOL_FUNCS \
    "layout(push_constant) uniform P { vec4 c[8]; } pc;\n" \
    "float hg(float c, float g) { float g2 = g * g; return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * c, 1.5); }\n" \
    "float phs(float c, float k) { return mix(hg(c, 0.70 * k), hg(c, -0.20 * k), 0.30) * 0.30 + 0.12; }\n" \
    /* the cloud field: the 5200-unit tiling noise is bent by a bigger noise (no repeating pattern from above) and */ \
    /* weighed by a very large weather map -- clear patches and, for Vibrant, towering cumulonimbus where it is dense */ \
    "float dens(vec3 p, float lod) {\n" \
    "  float H0 = pc.c[4].y, H1 = pc.c[4].z;\n" \
    "  vec3 w = vec3(pc.c[6].x, 0.0, pc.c[6].y) * pc.c[0].w;\n" \
    "  vec3 q = p + w;\n" \
    "  float wx = textureLod(shp, vec3(q.x, 777.0, q.z) * (1.0 / 61000.0), 0.0).r;\n"   /* (the weather map) */ \
    "  bool storm = pc.c[5].x > 1.5;\n" \
    "  float top = storm ? H1 : H0 + (H1 - H0) * (0.45 + 0.55 * smoothstep(0.35, 0.75, wx));\n"   /* (towers reach the slab's top) */ \
    "  float h = (p.y - H0) / (top - H0);\n" \
    "  if (h < 0.0 || h > 1.0) return 0.0;\n" \
    "  vec3 bend = (textureLod(shp, q * (1.0 / 17000.0) + 0.37, 0.0).rrr - 0.5) * 2600.0;\n" \
    "  float base = textureLod(shp, (q + bend) * (1.0 / 5200.0), lod).r;\n" \
    /* (Vibrant: clear patches clear wide -- the weather map pulls harder below its middle than above it) */ \
    "  float cov = pc.c[4].w + (wx - 0.5) * (storm ? 0.35 : (wx < 0.5 ? 0.9 : 0.5));\n" \
    "  float prof = smoothstep(0.0, 0.07, h) * smoothstep(1.0, pc.c[7].y, h);\n"   /* (flat bases; tops taper -> domes) */ \
    "  float s = clamp((base * prof - (1.0 - cov)) / pc.c[7].x, 0.0, 1.0);\n" \
    "  if (s <= 0.0) return 0.0;\n" \
    "  float er = 0.55 * (1.0 - 0.35 * h);\n" \
    "  float dd = textureLod(det, (q + w * 0.6) * (1.0 / 760.0), 0.0).r;\n" \
    "  s = clamp((s - dd * er) / (1.0 - dd * er), 0.0, 1.0);\n" \
    "  return s * pc.c[6].z;\n" \
    "}\n" \
    /* the level's own light: its sky picture near the horizon and overhead (smoothed) */ \
    "void skyLight(out vec3 hor, out vec3 zen) {\n" \
    "  hor = vec3(0.0); zen = vec3(0.0);\n" \
    "  for (int i = 0; i < 8; i++) { vec2 u = vec2(float(i) * 0.125 + 0.0625, 0.0); hor += textureLod(orig, u + vec2(0.0, 0.46), 0.0).rgb; zen += textureLod(orig, u + vec2(0.0, 0.12), 0.0).rgb; }\n" \
    "  hor = clamp(hor * 0.125, 0.0, 1.0); zen = clamp(zen * 0.125, 0.0, 1.0);\n" \
    "  float lh = dot(hor, vec3(0.299, 0.587, 0.114)), lz = dot(zen, vec3(0.299, 0.587, 0.114));\n" \
    "  hor *= min(lh, 0.72) / max(lh, 1e-3); zen *= min(lz, 0.60) / max(lz, 1e-3);\n"   /* (its colours, but never brighter than this: some skies are near white) */ \
    "}\n" \
    "const float SMAP = 24000.0, SMAP_TX = 24000.0 / 512.0;\n" \
    "vec2 smapCentre() { return floor(pc.c[0].xz / SMAP_TX) * SMAP_TX; }\n"
static const char* k_volm_fs =   // (the cloud shadow map: sunlight through the layer, up from the cloud base)
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=1) uniform sampler3D shp;\n"
    "layout(binding=2) uniform sampler3D det;\n"
    "layout(binding=4) uniform sampler2D orig;\n"
    VOL_FUNCS
    "void main() {\n"
    "  vec2 xz = smapCentre() + (uv - 0.5) * SMAP;\n"
    "  vec3 sun = normalize(pc.c[3].xyz);\n"
    "  float H0 = pc.c[4].y, H1 = pc.c[4].z;\n"
    "  if (sun.y < 0.03) { o = vec4(1.0); return; }\n"
    "  float st = (H1 - H0) / sun.y / 12.0, od = 0.0;\n"
    "  for (int k = 0; k < 12; k++) od += dens(vec3(xz.x, H0, xz.y) + sun * st * (float(k) + 0.5), 0.0);\n"
    "  o = vec4(exp(-od * st * pc.c[6].w));\n"
    "}\n";
static const char* k_vol_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D dep;\n"
    "layout(binding=1) uniform sampler3D shp;\n"
    "layout(binding=2) uniform sampler3D det;\n"
    "layout(binding=3) uniform sampler2D smap;\n"
    "layout(binding=4) uniform sampler2D orig;\n"
    VOL_FUNCS
    "float linz(float d) { float n = pc.c[3].w, f = pc.c[4].x; float z = d * 2.0 - 1.0; return 2.0 * n * f / (f + n - z * (f - n)); }\n"
    "float cloudSun(vec3 p, vec3 sun) {\n"   // (how much sunlight gets through the clouds to p: shafts, shadows on the ground)
    "  float H0 = pc.c[4].y, H1 = pc.c[4].z;\n"
    "  if (sun.y < 0.03 || p.y >= H1) return 1.0;\n"
    "  vec3 q = p + sun * ((H0 - p.y) / sun.y);\n"   // (the cloud-base point whose ray to the sun passes p)
    "  vec2 m = (q.xz - smapCentre()) / SMAP + 0.5;\n"
    "  if (m.x < 0.0 || m.y < 0.0 || m.x > 1.0 || m.y > 1.0) return 0.8;\n"
    "  float T = textureLod(smap, m, 0.0).r;\n"
    "  return p.y > H0 ? pow(T, (H1 - p.y) / (H1 - H0)) : T;\n"   // (inside the layer: only the part above)
    "}\n"
    "void main() {\n"
    "  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "  vec3 P = pc.c[0].xyz, fw = pc.c[1].xyz, rt = vec3(pc.c[2].x, 0.0, pc.c[2].z), up = cross(rt, fw);\n"   // (c2.y: the AO radius)
    "  vec3 D = normalize(fw + ndc.x * pc.c[1].w * rt + ndc.y * pc.c[2].w * up);\n"
    "  float d = textureLod(dep, uv, 0.0).r, far = pc.c[4].x;\n"
    "  float tmax = linz(d) / max(dot(D, fw), 1e-3);\n"
    "  if (d >= 0.99999 || tmax > far * 0.5) tmax = 1e9;\n"   // (the sky dome and anything as far: open sky)
    "  int fl = int(pc.c[7].z + 0.5);\n"   // (1 clouds, 2 haze + light shafts, 4 cloud shadows on the ground, 32 ambient occlusion)
    // (this pixel's place in camera space and its surface's facing, for the ambient occlusion -- worked out here,
    // before any branch: the screen-space slopes need every pixel)
    "  float zc = linz(d);\n"
    "  vec3 Pc = vec3(ndc.x * pc.c[1].w * zc, ndc.y * pc.c[2].w * zc, zc);\n"
    "  vec3 Nc = cross(dFdx(Pc), dFdy(Pc));\n"
    "  float ze = max(abs(dFdx(zc)), abs(dFdy(zc)));\n"   // (how much the depth jumps to the next pixel: a silhouette)
    "  bool storm = pc.c[5].x > 1.5;\n"
    "  vec3 sun = normalize(pc.c[3].xyz);\n"
    "  float mu = dot(D, sun);\n"
    "  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"   // (fixed per pixel: no crawling grain)
    // (the lights come from the level's own sky: the clouds' sunlit sides from its horizon tint, their shade from its
    // overhead colour -- so they sit in Kelp Dome's evening as well as in the plaza's noon)
    "  vec3 hor, zen; skyLight(hor, zen);\n"
    "  float lh = dot(hor, vec3(0.299, 0.587, 0.114)), lz = dot(zen, vec3(0.299, 0.587, 0.114));\n"
    "  vec3 horT = hor / max(lh, 0.05), zenT = zen / max(lz, 0.05);\n"   /* (tints: the colours at unit brightness) */
    "  vec3 sunc = storm ? mix(vec3(1.0), horT, 0.5) * vec3(1.0, 0.9, 0.78) * 2.4 : mix(vec3(1.0), horT, 0.35) * 4.2;\n"
    "  vec3 ambT = storm ? mix(vec3(1.0), zenT, 0.6) * 0.33 : zen * 1.15 + 0.08;\n"
    "  vec3 ambB = storm ? mix(vec3(1.0), zenT, 0.6) * 0.07 : zen * 0.9 + 0.05;\n"   /* (Vibrant: bottoms lit by the blue overhead, not grey) */
    "  float H0 = pc.c[4].y, H1 = pc.c[4].z, ta = 0.0, tb = 0.0;\n"
    "  float Tc = 1.0, tcl = 1e9; vec3 Lc = vec3(0.0);\n"
    "  if ((fl & 1) != 0) {\n"
    "  if (abs(D.y) < 1e-4) { if (P.y > H0 && P.y < H1) tb = 60000.0; }\n"
    "  else { float t0 = (H0 - P.y) / D.y, t1 = (H1 - P.y) / D.y; ta = max(min(t0, t1), 0.0); tb = max(t0, t1); }\n"
    "  bool inside = P.y > H0 && P.y < H1;\n"
    "  tb = min(tb, min(tmax, ta + (inside ? 24000.0 : 60000.0)));\n"
    "  if (tb > ta) {\n"
    "  int N = int(pc.c[5].y);\n"
    "  float span = tb - ta;\n"
    "  float ph0 = phs(mu, 1.0), ph1 = phs(mu, 0.5), ph2 = phs(mu, 0.25);\n"
    "  float sig = pc.c[6].w, ls = (H1 - H0) * 0.10, msk = pc.c[7].w;\n"
    "  float T = 1.0, tw = 0.0, tsum = 0.0; vec3 L = vec3(0.0);\n"
    "  for (int i = 0; i < N; i++) {\n"
    "    float x = (float(i) + jit) / float(N);\n"   // (inside the layer: steps packed near the camera)
    "    float t = ta + span * (inside ? x * x : x), dt = span * (inside ? 2.0 * x + 1.0 / float(N) : 1.0) / float(N);\n"
    "    vec3 p = P + D * t;\n"
    "    float s = dens(p, 0.0);\n"
    "    if (s > 0.002) {\n"
    "      float od = 0.0;\n"
    "      for (int j = 1; j <= 5; j++) od += dens(p + sun * ls * float(j * j) * 0.4, 1.5) * ls * float(2 * j - 1) * 0.4;\n"
    "      float e = od * sig;\n"   // (light that bounced inside the cloud: softer, reaches deeper -- white bright insides)
    "      float sl = exp(-e) * ph0 + msk * (0.5 * exp(-e * 0.5) * ph1 + 0.25 * exp(-e * 0.25) * ph2);\n"
    "      float pw = 1.0 - exp(-s * sig * 600.0);\n"   // (dark edges facing away: the 'powder' look of real cumulus)
    "      float h = clamp((p.y - H0) / (H1 - H0), 0.0, 1.0);\n"
    "      vec3 S = sunc * sl * mix(0.6, 1.0, pw) + mix(ambB, ambT, h);\n"
    "      float Ts = exp(-s * sig * dt);\n"
    "      float wgt = T * (1.0 - Ts);\n"
    "      L += wgt * S;\n"
    "      tw += wgt; tsum += wgt * t;\n"
    "      T *= Ts;\n"
    "      if (T < 0.01) break;\n"
    "    }\n"
    "  }\n"
    "  float fade = exp(-(tw > 0.0 ? tsum / tw : ta) / 45000.0);\n"   // (far clouds melt into the horizon haze: by where the cloud is)
    "  Lc = L * fade; Tc = mix(1.0, T, fade);\n"
    "  if (tw > 0.0) tcl = tsum / tw;\n"
    "  }\n"
    "  }\n"
    // haze with light shafts: from the camera to the scene, the clouds or 40000 units -- denser near the ground, lit
    // by the sun where the clouds above let it through (world-space: the shafts stay put when the camera moves)
    "  float Tf = 1.0; vec3 Lf = vec3(0.0);\n"
    "  if ((fl & 2) != 0) {\n"
    "    float te = min(min(tmax, tcl), 24000.0), s0 = pc.c[5].z, gy = pc.c[5].w;\n"
    "    float phF = 0.06 + 0.30 * hg(mu, 0.65);\n"
    "    vec3 amb = storm ? mix(vec3(1.0), horT, 0.6) * 0.17 : hor * 0.62;\n"   /* (the haze is lit like the level's horizon) */
    // (near the ground: distance haze; a moist layer up through the clouds -- the light shafts live there, between the
    // cloud towers and under the gaps, and stay put in the world when the camera flies through)
    "    float low = storm ? 0.7 : 1.0, mist = storm ? 0.55 : 0.06;\n"
    "    int NF = max(int(pc.c[5].y * 0.55), 12);\n"   // (15 / 24 / 40 by quality)
    "    for (int i = 0; i < NF; i++) {\n"
    "      float x = (float(i) + jit) / float(NF);\n"
    "      float t = te * x * x, dt = te * (2.0 * x + 1.0 / float(NF)) / float(NF);\n"
    "      vec3 p = P + D * t;\n"
    "      float sg = s0 * (low * exp(-max(p.y - gy, 0.0) / 1600.0) + mist * (1.0 - smoothstep(mix(H0, H1, 0.55), H1, p.y)));\n"
    "      float vis = (fl & 1) != 0 ? cloudSun(p, sun) : 1.0;\n"
    "      float Ts = exp(-sg * dt);\n"
    "      Lf += Tf * (sunc * vis * phF * (storm ? 2.0 : 1.2) + amb) * (1.0 - Ts);\n"   // (the shafts a little stronger than plain light)
    "      Tf *= Ts;\n"
    "    }\n"
    "  }\n"
    // cloud shadows on the ground: the scene's light dimmed where the clouds cover the sun (they drift with the wind)
    "  float gsh = 1.0;\n"
    "  if ((fl & 5) == 5 && tmax < 1e8) gsh = mix(storm ? 0.55 : 0.70, 1.0, cloudSun(P + D * tmax, sun));\n"
    // ambient occlusion: nearby surfaces in front of this one's facing (8 depth samples in a disc of 14 world units)
    // darken corners, the ground under benches and around feet; fades out with distance
    "  float ao = 1.0;\n"
    "  if ((fl & 32) != 0 && tmax < 1e8 && zc < 4000.0 && dot(Nc, Nc) > 0.0 && ze < 0.03 * zc) {\n"   // (not across a depth edge: the facing is wrong there)
    "    vec3 N = normalize(Nc);\n"
    "    if (dot(N, Pc) > 0.0) N = -N;\n"
    "    float R = pc.c[2].y, rs = R / (zc * pc.c[2].w), sum = 0.0;\n"
    "    for (int k = 0; k < 8; k++) {\n"
    "      float a = (float(k) + jit) * 2.39996, r = rs * sqrt((float(k) + 0.5) / 8.0);\n"
    "      vec2 n2 = ndc + vec2(cos(a) * r * pc.c[2].w / pc.c[1].w, sin(a) * r);\n"
    "      float z2 = linz(textureLod(dep, vec2(n2.x * 0.5 + 0.5, 0.5 - n2.y * 0.5), 0.0).r);\n"
    "      vec3 v = vec3(n2.x * pc.c[1].w * z2, n2.y * pc.c[2].w * z2, z2) - Pc;\n"
    "      float vv = dot(v, v);\n"
    "      sum += max(dot(v, N) - 0.002 * zc, 0.0) / (vv + 0.02 * R * R) * (1.0 - smoothstep(R * R, 4.0 * R * R, vv));\n"
    "    }\n"
    "    ao = mix(1.0, clamp(1.0 - 1.5 * R * sum / 8.0, 0.0, 1.0), 1.0 - smoothstep(2000.0, 4000.0, zc));\n"
    "  }\n"
    "  o = vec4(Lf + Tf * Lc, Tf * Tc * gsh * ao);\n"
    "  if ((fl & 64) != 0) o = vec4(vec3(ao), 0.0);\n"   // (live VOLDBG:3 -- the ambient occlusion alone)
    "  if ((fl & 128) != 0) o = vec4(dot(Nc, Nc) > 0.0 ? normalize(Nc) * (dot(Nc, Pc) > 0.0 ? -0.5 : 0.5) + 0.5 : vec3(0.0), 0.0);\n"   // (VOLDBG:4 -- surface facing)
    "  if ((fl & 8) != 0) o = vec4(Lf, 0.0);\n"   // (live VOLDBG:1 -- the haze's light alone)
    "  if ((fl & 16) != 0) o = vec4(vec3(tmax < 1e8 ? cloudSun(P + D * tmax, sun) : textureLod(smap, uv, 0.0).r), 0.0);\n"   // (VOLDBG:2 -- sunlight on the ground; the map itself over the sky)
    "}\n";
static const char* k_volc_fs =   // (half resolution -> the scene buffer: dst * a + rgb; the half-size texels at a depth
                                 // like this pixel's count most -- no cloud fringe over building edges. Then, at full
                                 // resolution, the rain: streaks, wet ground, puddles with ripples)
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D dep;\n"
    "layout(binding=1) uniform sampler2D vol;\n"
    "layout(binding=2) uniform sampler3D det;\n"
    "layout(binding=3) uniform sampler2D scn;\n"   // (with rain: a copy of the scene, for the puddles' reflections)
    "layout(binding=4) uniform sampler2D skyp;\n"   // (our sky picture for this level: the rain's colour; the background above the dome)
    "layout(push_constant) uniform P { vec4 c[8]; } pc;\n"
    "float linz(float d) { float n = pc.c[3].w, f = pc.c[4].x; float z = d * 2.0 - 1.0; return 2.0 * n * f / (f + n - z * (f - n)); }\n"
    "float h11(float x) { return fract(sin(x * 127.1 + 311.7) * 43758.5453); }\n"
    "vec3 skyAt(vec3 D) { return textureLod(skyp, vec2(atan(D.x, D.z) / 6.2831853, 0.5 - asin(clamp(D.y, -1.0, 1.0)) / 3.14159265), 0.0).rgb; }\n"
    // one sheet of rain: thin vertical streaks on a cylinder of radius r around the camera, q = (distance along the
    // cylinder = azimuth * r, height) in world units -- fixed to the world's directions, so turning the camera doesn't
    // drag the rain; sparse (two columns in five carry a drop), slow, long and thin; ps = a pixel's size there
    "float rainSheet(vec2 q, float k, float ps, float gain, float speed) {\n"
    "  float cw = 2.2 + 0.9 * k, col = floor(q.x / cw);\n"
    "  float h1 = h11(col * 1.37 + k * 17.1), h2 = h11(col * 2.91 + k * 5.3), h3 = h11(col * 0.73 + k * 9.9), h4 = h11(col * 5.11 + k * 3.7);\n"
    "  if (h4 > 0.25) return 0.0;\n"
    "  float wdt = max(0.05, ps * 1.1);\n"
    "  float line = 1.0 - smoothstep(wdt * 0.4, wdt, abs(q.x - (col + 0.2 + 0.6 * h1) * cw));\n"
    "  if (line <= 0.0) return 0.0;\n"
    "  float per = 44.0 + 30.0 * h2, len = 9.0 + 6.0 * h3;\n"
    "  float m = mod(q.y + pc.c[0].w * speed * (105.0 + 30.0 * h3) + h2 * per * 7.0, per);\n"   // (falls ~120 units/s)
    "  return line * smoothstep(0.0, 0.5, m) * (1.0 - smoothstep(len * 0.5, len, m)) * min(1.0, gain / wdt);\n"
    "}\n"
    // is there sky straight above the world point Q? Checked against what is on the screen: points up to 500 units
    // above Q that land on a nearer surface mean a roof / ceiling / bridge (off the screen = taken as open)
    "float openAbove(vec3 Q, vec3 P, vec3 fw, vec3 rt, vec3 up) {\n"
    "  for (int i = 1; i <= 4; i++) {\n"
    "    vec3 v = Q + vec3(0.0, 15.0 * pow(2.6, float(i)), 0.0) - P;\n"
    "    float z = dot(v, fw);\n"
    "    if (z < 1.0) continue;\n"
    "    vec2 sp = vec2(dot(v, rt) / (z * pc.c[1].w), dot(v, up) / (z * pc.c[2].w));\n"
    "    if (abs(sp.x) > 1.0 || abs(sp.y) > 1.0) return 1.0;\n"
    "    float dz = linz(textureLod(dep, vec2(sp.x * 0.5 + 0.5, 0.5 - sp.y * 0.5), 0.0).r);\n"
    "    if (dz < z - 2.0) return 0.0;\n"
    "  }\n"
    "  return 1.0;\n"
    "}\n"
    "void main() {\n"
    "  vec2 hs = vec2(textureSize(vol, 0));\n"
    "  vec2 fp = uv * hs - 0.5, b = floor(fp), f = fp - b;\n"
    "  float zc = linz(textureLod(dep, uv, 0.0).r);\n"
    "  vec4 acc = vec4(0.0); float ws = 0.0;\n"
    "  for (int k = 0; k < 4; k++) {\n"
    "    vec2 q = vec2(float(k & 1), float(k >> 1));\n"
    "    vec2 tuv = (b + q + 0.5) / hs;\n"
    "    float zk = linz(textureLod(dep, tuv, 0.0).r);\n"
    "    float wb = mix(1.0 - f.x, f.x, q.x) * mix(1.0 - f.y, f.y, q.y);\n"
    "    float w = wb / (1e-3 + abs(zk - zc) / max(zc, 1.0));\n"
    "    acc += textureLod(vol, tuv, 0.0) * w; ws += w;\n"
    "  }\n"
    "  o = ws > 0.0 ? acc / ws : texture(vol, uv);\n"
    "  if (ws < 0.02) {\n"   // (an edge: no half-size texel matches this depth -- take the closest one, not a blend of the wrong ones)
    "    float bd = 1e30;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "      vec2 tuv = (b + vec2(float(k & 1), float(k >> 1)) + 0.5) / hs;\n"
    "      float dz = abs(linz(textureLod(dep, tuv, 0.0).r) - zc);\n"
    "      if (dz < bd) { bd = dz; o = textureLod(vol, tuv, 0.0); }\n"
    "    }\n"
    "  }\n"
    "  int fl = int(pc.c[7].z + 0.5);\n"
    "  if ((fl & 512) != 0 && textureLod(dep, uv, 0.0).r >= 0.99999) {\n"   // (free camera outside the sky dome: our sky where the game drew nothing)
    "    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "    vec3 fw = pc.c[1].xyz, rt = vec3(pc.c[2].x, 0.0, pc.c[2].z), up = cross(rt, fw);\n"
    "    vec3 D = normalize(fw + ndc.x * pc.c[1].w * rt + ndc.y * pc.c[2].w * up);\n"
    "    o = vec4(skyAt(D) * o.a + o.rgb, 0.0);\n"
    "  }\n"
    "  if ((fl & 256) != 0) {\n"   // (rain: Storm with SHADERS "Rain" on)
    "    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "    vec3 P = pc.c[0].xyz, fw = pc.c[1].xyz, rt = vec3(pc.c[2].x, 0.0, pc.c[2].z), up = cross(rt, fw);\n"
    "    vec3 D = normalize(fw + ndc.x * pc.c[1].w * rt + ndc.y * pc.c[2].w * up);\n"
    "    float d = textureLod(dep, uv, 0.0).r, H0 = pc.c[4].y, tm = linz(d) / max(dot(D, fw), 1e-3), t = pc.c[0].w;\n"
    "    bool sky = d >= 0.99999 || tm > pc.c[4].x * 0.5;\n"
    "    vec3 X = P + D * min(tm, 1e5);\n"
    "    vec3 N = cross(dFdx(X), dFdy(X));\n"
    "    vec3 skyc = vec3(0.0);\n"   // (the rain's grey: this level's sky near the horizon, a little dimmer)
    "    for (int i = 0; i < 4; i++) skyc += textureLod(skyp, vec2(float(i) * 0.25 + 0.125, 0.42), 0.0).rgb;\n"
    "    skyc *= 0.25 * 0.85;\n"
    // (a ceiling close above, looked at from below -- the points above it are off the screen, so this is asked
    // outright: the view hits something within 700 units that lies higher than the camera while looking up)
    "    bool ceil = !sky && tm < 700.0 && D.y > 0.2 && X.y > P.y + 15.0;\n"
    "    float roof = ceil ? 0.0 : openAbove(X, P, fw, rt, up);\n"   // (1 = sky above this spot, 0 = something over it: no rain, no wet)
    "    if (!sky && X.y < H0 && dot(N, N) > 0.0 && roof > 0.0) {\n"   // (wet ground: darker, a sheen of the sky, puddles with ripples)
    "      N = normalize(N); if (dot(N, D) > 0.0) N = -N;\n"
    "      float upf = smoothstep(0.6, 0.9, N.y);\n"
    "      float plod = clamp(log2(max(tm, 1.0) / 350.0), 0.0, 3.0);\n"   // (farther: a blurrier noise, no shimmer)
    "      float n1 = textureLod(det, vec3(X.xz / 170.0, 0.3), plod * 0.5).r, n2 = textureLod(det, vec3(X.xz / 46.0, 0.7), plod).r;\n"
    "      float pud = smoothstep(0.52, 0.68, n1 * 0.7 + n2 * 0.3) * upf;\n"
    // (the wet look only on surfaces that face up: on walls, people and shoes the sky's sheen at grazing angles was
    // a cyan rim along every silhouette)
    "      float near = (1.0 - smoothstep(1500.0, 3000.0, tm)) * roof, wet = mix(0.5, 1.0, pud) * upf * near;\n"
    "      o.a *= 1.0 - 0.22 * wet - 0.26 * pud * near;\n"
    "      float fr = min(0.04 + 0.96 * pow(1.0 - clamp(dot(-D, N), 0.0, 1.0), 5.0), 0.5);\n"
    "      vec3 refl = skyc * 0.35;\n"   // (no hit below: a dim sky, never a white patch)
    "      if (pud * near > 0.08) {\n"   // (puddles mirror the scene: the reflected ray followed across the screen -- 12 steps)
    "        vec3 Rd = reflect(D, N), Q = X + N * 0.5;\n"
    "        float sl = 7.0;\n"
    "        for (int i = 0; i < 12; i++) {\n"
    "          Q += Rd * sl; sl *= 1.42;\n"
    "          vec3 v = Q - P;\n"
    "          float z = dot(v, fw);\n"
    "          if (z < 1.0) break;\n"
    "          vec2 sp = vec2(dot(v, rt) / (z * pc.c[1].w), dot(v, up) / (z * pc.c[2].w));\n"
    "          if (abs(sp.x) > 1.0 || abs(sp.y) > 1.0) break;\n"
    "          vec2 su = vec2(sp.x * 0.5 + 0.5, 0.5 - sp.y * 0.5);\n"
    "          float dz = linz(textureLod(dep, su, 0.0).r);\n"
    "          if (z > dz + 0.5 && z < dz + sl * 2.0) {\n"   // (fading out towards the screen's edges, where the picture ends)
    "            vec2 e = min(su, 1.0 - su);\n"
    "            refl = mix(skyc * 0.35, textureLod(scn, su, 0.0).rgb * 0.7, smoothstep(0.0, 0.08, e.x) * smoothstep(0.0, 0.08, e.y));\n"
    "            break;\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "      o.rgb += refl * (fr * (0.25 + 0.75 * pud) * wet + 0.12 * pud * near);\n"
    "      vec2 g = X.xz / 5.0, cell = floor(g);\n"   // (ripples: small rings at random spots, only inside the puddles, few at a time)
    "      float hc = h11(cell.x * 3.1 + cell.y * 17.7), cyc = t * 1.1 + hc * 7.0, ph = fract(cyc), k = floor(cyc);\n"
    "      vec2 cc = cell + 0.2 + 0.6 * vec2(h11(hc * 13.0 + k), h11(hc * 91.3 + k));\n"
    "      float rr = length(g - cc) * 5.0;\n"
    "      float ring = (1.0 - smoothstep(0.0, 0.18, abs(rr - ph * 1.7))) * (1.0 - ph) * smoothstep(0.0, 0.15, ph) * step(rr, 2.0) * step(h11(hc * 53.0 + k), 0.10);\n"
    "      o.rgb += skyc * 0.2 * ring * smoothstep(0.6, 0.85, pud) * (1.0 - smoothstep(250.0, 700.0, tm));\n"
    "    }\n"
    // the streaks: 4 cylinders around the camera, 60..295 units out, each a sparse sheet of slow thin streaks fixed
    // to the world's directions; hidden behind nearer things and under roofs; none when looking down (seen end-on
    // they were only dots and rings); a translucent grey of the sky's colour, never white, never more than a third
    "    float rain = 0.0, ph = 2.0 * pc.c[2].w / float(textureSize(dep, 0).y), hl = length(D.xz);\n"
    "    if (ceil) tm = 0.0;\n"   // (under a ceiling: no streaks in front of it)
    "    float down = mix(0.25, 1.0, smoothstep(-0.7, -0.25, D.y)) * smoothstep(0.1, 0.25, hl) * (1.0 - smoothstep(0.6, 0.85, D.y));\n"
    "    if (down > 0.0) {\n"
    "      float az = atan(D.z, D.x);\n"
    "      for (int k = 0; k < 4; k++) {\n"
    "        float r = 60.0 * pow(1.7, float(k)), th = r / hl;\n"
    "        if (th >= tm) break;\n"
    "        vec3 S = P + D * th;\n"
    "        if (S.y >= H0) continue;\n"
    "        float rs = rainSheet(vec2(az * r, S.y + az * r * 0.08), float(k), th * ph, 0.05, 1.0) * (1.0 - 0.12 * float(k));\n"
    "        if (rs > 0.0) rain += rs * ((k & 1) == 0 ? openAbove(S, P, fw, rt, up) : openAbove(P + D * th * 0.6, P, fw, rt, up));\n"
    "      }\n"
    "    }\n"
    // looking up (above ~35 degrees): the cylinders are seen end-on, so two sheets in the sky's own coordinates take over --
    // the world azimuth of the view (streaks keep their place as the camera turns) and the zenith angle, in which a drop
    // falling towards the camera moves outward from the point straight above; roofs through the same screen test
    "    float rup = 0.0, wup = smoothstep(0.5, 0.85, D.y);\n"
    "    if (wup > 0.0 && !ceil) {\n"
    "      float az = atan(D.z, D.x), zr = log(hl / D.y + 0.03);\n"   // (zr: the zenith angle, stretched so a fall reads evenly)
    "      for (int k = 0; k < 2; k++) {\n"
    "        float h = k == 0 ? 45.0 : 120.0, th = h / D.y;\n"
    "        if (th >= tm) break;\n"   // (something nearer above: a roof)
    "        float r = k == 0 ? 70.0 : 120.0;\n"   // (the azimuth scale: how many streaks round the sky)
    "        float rs = rainSheet(vec2(az * r, -zr * 12.0 + float(k) * 11.0), float(k) + 7.0, ph / max(hl, 0.02) * r, 0.7, 0.2) * (1.0 - 0.15 * float(k));\n"
    "        if (rs > 0.0) rup += rs * openAbove(P + D * th, P, fw, rt, up);\n"
    "      }\n"
    "    }\n"
    "    rain = min(rain * 0.34 * down + rup * 0.45 * wup, 0.42);\n"
    "    o.a *= 1.0 - rain;\n"
    "    o.rgb = o.rgb * (1.0 - rain) + skyc * rain;\n"
    "  }\n"
    "}\n";
// How much rain the player is in, for the rain sound (rain_sound.c): a 16x16 picture of the screen -- 1 where a spot
// has sky above it (the roof test of the composite), 0 under roofs -- read back a few frames later, averaged.
static const char* k_rainp_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D dep;\n"
    "layout(push_constant) uniform P { vec4 c[8]; } pc;\n"
    "float linz(float d) { float n = pc.c[3].w, f = pc.c[4].x; float z = d * 2.0 - 1.0; return 2.0 * n * f / (f + n - z * (f - n)); }\n"
    "void main() {\n"
    "  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "  vec3 P = pc.c[0].xyz, fw = pc.c[1].xyz, rt = vec3(pc.c[2].x, 0.0, pc.c[2].z), up = cross(rt, fw);\n"
    "  vec3 D = normalize(fw + ndc.x * pc.c[1].w * rt + ndc.y * pc.c[2].w * up);\n"
    "  float d = textureLod(dep, uv, 0.0).r, tm = linz(d) / max(dot(D, fw), 1e-3);\n"
    "  if (d >= 0.99999 || tm > pc.c[4].x * 0.5) { o = vec4(1.0); return; }\n"   // (open sky in view: rain falls in front of it)
    "  vec3 X = P + D * tm;\n"
    "  if (tm < 700.0 && D.y > 0.2 && X.y > P.y + 15.0) { o = vec4(0.0); return; }\n"   // (a ceiling close above, seen from below)
    "  vec3 Q = P + D * min(tm, 300.0);\n"   // (a nearby spot along the view: is it under something?)
    "  float open = 1.0;\n"
    "  for (int i = 1; i <= 5; i++) {\n"
    "    vec3 v = Q + vec3(0.0, 20.0 * pow(2.2, float(i)), 0.0) - P;\n"
    "    float z = dot(v, fw);\n"
    "    if (z < 1.0) continue;\n"
    "    vec2 sp = vec2(dot(v, rt) / (z * pc.c[1].w), dot(v, up) / (z * pc.c[2].w));\n"
    "    if (abs(sp.x) > 1.0 || abs(sp.y) > 1.0) break;\n"
    "    if (linz(textureLod(dep, vec2(sp.x * 0.5 + 0.5, 0.5 - sp.y * 0.5), 0.0).r) < z - 2.0) { open = 0.0; break; }\n"
    "  }\n"
    "  o = vec4(open);\n"
    "}\n";
#define VOL_SETS 4   // (per frame: the march, the blend into the scene, the cloud shadow map, the rain probe)
volatile int splat_vol_dbg;   // (live VOLDBG:n, input.c: 1 the haze's light alone, 2 the cloud shadows)
static struct { bool tried, ok; VkDescriptorSetLayout dsl; VkPipelineLayout layout; VkPipeline march, comp, smap_pipe; VkFormat comp_fmt;
                VkDescriptorPool pool; VkDescriptorSet sets[NSUB * VOL_SETS]; VkSampler nearest; Img shape, detail, half, smap, scopy; bool half_ready, smap_ready, scopy_ready;
                volatile int noise_state; u8* shape_px; u8* detail_px; bool uploaded;
                VkPipeline probe_pipe; Img probe; bool probe_ready; VkBuffer probe_buf[NSUB]; VkDeviceMemory probe_mem[NSUB]; void* probe_ptr[NSUB]; } g_vol;
extern volatile float splat_rain_level;   // (rain_sound.c)
extern void splat_rain_level_set(float level);
static void vol_draw(VkPipeline pipe, Img* out, VkAttachmentLoadOp load, VkDescriptorSet ds, const float pcv[32]);
static void vol_set(VkDescriptorSet ds, int binding, VkSampler smp, VkImageView v);
static void rain_probe(VkImageView dv, const float pcv[32]) {
    if (!g_vol.probe_pipe && !(g_vol.probe_pipe = fx_pipeline_fmt(k_rainp_fs, false, VK_FORMAT_R8_UNORM, g_vol.layout))) return;
    if (!g_vol.probe.image && !make_image(&g_vol.probe, VK_FORMAT_R8_UNORM, 16, 16, 1, VK_IMAGE_TYPE_2D, false, true, false)) return;
    if (!g_vol.probe_buf[g.cur] && !host_buffer_cached(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_vol.probe_buf[g.cur], &g_vol.probe_mem[g.cur], &g_vol.probe_ptr[g.cur])) return;
    VkCommandBuffer cb = transfer_cmd();
    if (!g_vol.probe_ready) { image_layout_init(cb, &g_vol.probe); g_vol.probe_ready = true; }
    VkDescriptorSet ds = g_vol.sets[g.cur * VOL_SETS + 3];
    vol_set(ds, 0, g_vol.nearest, dv);
    vol_draw(g_vol.probe_pipe, &g_vol.probe, VK_ATTACHMENT_LOAD_OP_DONT_CARE, ds, pcv);
    cb = transfer_cmd();
    barrier_all(cb);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = c.imageExtent.height = 16;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, g_vol.probe.image, VK_IMAGE_LAYOUT_GENERAL, g_vol.probe_buf[g.cur], 1, &c);
    // (the oldest recording's result: that submission is long done -- NSUB frames back)
    int old = (g.cur + 1) % NSUB;
    if (g_vol.probe_ptr[old]) {
        const u8* p = (const u8*)g_vol.probe_ptr[old];
        u32 sum = 0;
        for (int i = 0; i < 256; i++) sum += p[i];
        splat_rain_level_set((float)sum / (255.0f * 256.0f));
        static u64 logf_;
        if (g.frame >= logf_) { logf_ = g.frame + 600; printf("[rain] the player is in the rain %.0f%% (the sound follows)\n", splat_rain_level * 100.0f); }
    }
}
// tiling 3D noise (period = the cell counts): Worley (distance to the nearest of one random point per cell), value noise
static u32 vh3(int x, int y, int z, u32 s) { u32 h = (u32)x * 73856093u ^ (u32)y * 19349663u ^ (u32)z * 83492791u ^ s * 2654435761u; h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15; return h; }
static float worley3(float x, float y, float z, int cells, u32 seed) {
    float fx = x * cells, fy = y * cells, fz = z * cells;
    int cx = (int)floorf(fx), cy = (int)floorf(fy), cz = (int)floorf(fz);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int X = cx + dx, Y = cy + dy, Z = cz + dz;
        int wx = ((X % cells) + cells) % cells, wy = ((Y % cells) + cells) % cells, wz = ((Z % cells) + cells) % cells;
        u32 h = vh3(wx, wy, wz, seed);
        float px = X + (h & 1023) / 1023.0f, py = Y + ((h >> 10) & 1023) / 1023.0f, pz = Z + ((h >> 20) & 1023) / 1023.0f;
        float d = (px - fx) * (px - fx) + (py - fy) * (py - fy) + (pz - fz) * (pz - fz);
        if (d < best) best = d;
    }
    return 1.0f - fminf(sqrtf(best), 1.0f);
}
static float vnoise3(float x, float y, float z, int per, u32 seed) {
    float fx = x * per, fy = y * per, fz = z * per;
    int ix = (int)floorf(fx), iy = (int)floorf(fy), iz = (int)floorf(fz);
    float ux = fx - ix, uy = fy - iy, uz = fz - iz;
    ux = ux * ux * (3 - 2 * ux); uy = uy * uy * (3 - 2 * uy); uz = uz * uz * (3 - 2 * uz);
    float v[8];
    for (int k = 0; k < 8; k++) {
        int X = ((ix + (k & 1)) % per + per) % per, Y = ((iy + ((k >> 1) & 1)) % per + per) % per, Z = ((iz + ((k >> 2) & 1)) % per + per) % per;
        v[k] = (vh3(X, Y, Z, seed) & 0xFFFF) / 65535.0f;
    }
    float a = v[0] + (v[1] - v[0]) * ux, b = v[2] + (v[3] - v[2]) * ux, c = v[4] + (v[5] - v[4]) * ux, d = v[6] + (v[7] - v[6]) * ux;
    float e = a + (b - a) * uy, f = c + (d - c) * uy;
    return e + (f - e) * uz;
}
static DWORD WINAPI vol_noise_thread(LPVOID arg) {
    (void)arg;
    const int S = 128, Dn = 32;
    u8* sp = (u8*)malloc((size_t)S * S * S);
    u8* dp = (u8*)malloc((size_t)Dn * Dn * Dn);
    if (!sp || !dp) { free(sp); free(dp); g_vol.noise_state = -1; return 0; }
    for (int z = 0; z < S; z++) for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        float fx = (x + 0.5f) / S, fy = (y + 0.5f) / S, fz = (z + 0.5f) / S;
        float pn = vnoise3(fx, fy, fz, 4, 1) * 0.5f + vnoise3(fx, fy, fz, 8, 2) * 0.3f + vnoise3(fx, fy, fz, 16, 3) * 0.2f;
        float wf = worley3(fx, fy, fz, 4, 11) * 0.625f + worley3(fx, fy, fz, 8, 12) * 0.25f + worley3(fx, fy, fz, 16, 13) * 0.125f;
        float v = pn - (1.0f - wf) * 0.75f;   // (Perlin-Worley: billows from the Worley cells inside the smooth noise)
        v = (v + 0.45f) / 1.1f;
        sp[((size_t)z * S + y) * S + x] = (u8)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
    }
    for (int z = 0; z < Dn; z++) for (int y = 0; y < Dn; y++) for (int x = 0; x < Dn; x++) {
        float fx = (x + 0.5f) / Dn, fy = (y + 0.5f) / Dn, fz = (z + 0.5f) / Dn;
        float w = worley3(fx, fy, fz, 4, 21) * 0.625f + worley3(fx, fy, fz, 8, 22) * 0.25f + worley3(fx, fy, fz, 16, 23) * 0.125f;
        dp[((size_t)z * Dn + y) * Dn + x] = (u8)(fminf(fmaxf(w, 0.0f), 1.0f) * 255.0f + 0.5f);
    }
    g_vol.shape_px = sp;
    g_vol.detail_px = dp;
    g_vol.noise_state = 2;
    return 0;
}
static bool vol_init(void) {
    if (g_vol.tried) return g_vol.ok;
    g_vol.tried = true;
    if (!sx_init()) return false;
    VkDescriptorSetLayoutBinding b[5];
    memset(b, 0, sizeof(b));
    for (int i = 0; i < 5; i++) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 5;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.device, &dl, NULL, &g_vol.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_vol.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_vol.layout) != VK_SUCCESS) return false;
    g_blend_tm_layout = g_vol.layout;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSUB * VOL_SETS * 5 };
    VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pc.maxSets = NSUB * VOL_SETS;
    pc.poolSizeCount = 1;
    pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(g_vk.device, &pc, NULL, &g_vol.pool) != VK_SUCCESS) return false;
    for (int i = 0; i < NSUB * VOL_SETS; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_vol.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_vol.dsl;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &g_vol.sets[i]) != VK_SUCCESS) return false;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(g_vk.device, &sci, NULL, &g_vol.nearest) != VK_SUCCESS) return false;
    if (!(g_vol.march = fx_pipeline_fmt(k_vol_fs, false, VK_FORMAT_R16G16B16A16_SFLOAT, g_vol.layout))) return false;
    if (!(g_vol.smap_pipe = fx_pipeline_fmt(k_volm_fs, false, VK_FORMAT_R16G16B16A16_SFLOAT, g_vol.layout))) return false;
    if (!make_image(&g_vol.shape, VK_FORMAT_R8_UNORM, 128, 128, 128, VK_IMAGE_TYPE_3D, false, false, false)) return false;
    if (!make_image(&g_vol.detail, VK_FORMAT_R8_UNORM, 32, 32, 32, VK_IMAGE_TYPE_3D, false, false, false)) return false;
    g_vol.noise_state = 1;
    HANDLE th = CreateThread(NULL, 0, vol_noise_thread, NULL, 0, NULL);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); } else return false;
    g_vol.ok = true;
    printf("[shaders] 3D clouds: making the cloud noise on a thread of its own\n");
    return true;
}
static bool vol_upload(void) {   // (once the noise thread is done: into the 3D images)
    if (g_vol.uploaded) return true;
    if (g_vol.noise_state != 2) return false;
    VkCommandBuffer cb = transfer_cmd();
    image_layout_init(cb, &g_vol.shape);
    image_layout_init(cb, &g_vol.detail);
    barrier_all(cb);
    Img* im[2] = { &g_vol.shape, &g_vol.detail };
    u8* px[2] = { g_vol.shape_px, g_vol.detail_px };
    u32 n[2] = { 128, 32 };
    for (int k = 0; k < 2; k++) {
        for (u32 z0 = 0; z0 < n[k]; z0 += 32) {   // (in slabs: the upload chunks are 32 MB at most)
            u32 bytes = n[k] * n[k] * 32;
            VkBuffer buf;
            u32 off;
            u8* up = arena(bytes, 16, &buf, &off);
            if (!up) return false;
            memcpy(up, px[k] + (size_t)z0 * n[k] * n[k], bytes);
            VkBufferImageCopy copy;
            memset(&copy, 0, sizeof(copy));
            copy.bufferOffset = off;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset.z = (int)z0;
            copy.imageExtent.width = n[k];
            copy.imageExtent.height = n[k];
            copy.imageExtent.depth = 32;
            vkCmdCopyBufferToImage(cb, buf, im[k]->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        }
    }
    barrier_all(cb);
    g.need_barrier = true;
    free(g_vol.shape_px); free(g_vol.detail_px);
    g_vol.shape_px = g_vol.detail_px = NULL;
    g_vol.uploaded = true;
    printf("[shaders] 3D clouds ready\n");
    return true;
}
static void vol_set(VkDescriptorSet ds, int binding, VkSampler smp, VkImageView v) {
    VkDescriptorImageInfo di = { smp, v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = ds;
    wr.dstBinding = binding;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.device, 1, &wr, 0, NULL);
}
static void vol_draw(VkPipeline pipe, Img* out, VkAttachmentLoadOp load, VkDescriptorSet ds, const float pcv[32]) {
    VkCommandBuffer cb = transfer_cmd();
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = att_view(out, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = load;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = out->w;
    ri.renderArea.extent.height = out->h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)out->w, (float)out->h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { out->w, out->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vol.layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, g_vol.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, pcv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_dyn.valid = false;
    g.need_barrier = true;
}
static void vol_pass(Surf* scene, Surf* dsurf) {
    if (!dsurf || !vol_init() || !vol_upload() || g.cur < 0) return;
    float cp[3], fw[3], fovy = 0, nr = 1, fr = 10000;
    int splat_camera_for_frame(u32 frame, float pos[3], float dir[3], float* fovy_deg, float* nr, float* fr);
    // (a frame whose camera wasn't recorded -- a menu frame, a scene load -- keeps the last one: skipping the pass
    // for one frame showed as a flicker of the clouds, the haze and the puddles)
    static float lcp[3], lfw[3], lfovy, lnr = 1.0f, lfr = 10000.0f;
    if (splat_camera_for_frame(g_present_swap, cp, fw, &fovy, &nr, &fr) && fovy > 0.0f) { memcpy(lcp, cp, 12); memcpy(lfw, fw, 12); lfovy = fovy; lnr = nr; lfr = fr; }
    else if (lfovy > 0.0f) { memcpy(cp, lcp, 12); memcpy(fw, lfw, 12); fovy = lfovy; nr = lnr; fr = lfr; }
    else return;
    if (!sky_orig_fresh()) return;
    if (!g_vol.comp || g_vol.comp_fmt != scene->img.format) {
        g_vol.comp = fx_pipeline_fmt(k_volc_fs, true, scene->img.format, g_vol.layout);   // (blend: see fx_pipeline_fmt)
        g_vol.comp_fmt = scene->img.format;
        if (!g_vol.comp) return;
    }
    // half resolution (quality low: a quarter)
    u32 div = g_vk.sh_quality == 0 ? 4 : 2, hw = (scene->img.w + div - 1) / div, hh = (scene->img.h + div - 1) / div;
    if (g_vol.half.image && (g_vol.half.w != hw || g_vol.half.h != hh)) { memset(&g_vol.half, 0, sizeof(g_vol.half)); g_vol.half_ready = false; }
    if (!g_vol.half.image && !make_image(&g_vol.half, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1, VK_IMAGE_TYPE_2D, false, true, false)) return;
    if (!g_vol.half_ready) { VkCommandBuffer cb = transfer_cmd(); image_layout_init(cb, &g_vol.half); g_vol.half_ready = true; }
    float rx = -fw[2], rz = fw[0], rl = sqrtf(rx * rx + rz * rz);
    if (rl < 1e-4f) return;
    rx /= rl; rz /= rl;
    float ty = tanf(fovy * 0.5f * 3.14159265f / 180.0f), tx = ty * (float)scene->img.w / (float)scene->img.h;
    float sn[3];
    sky_sun(sn);
    bool storm = g_vk.sky_mode >= 2;
    static float hb = -1.0f, ht, hz_y, hz_d;
    if (hb < 0.0f) {
        hb = 2600.0f; ht = 7000.0f; hz_y = 0.0f; hz_d = 1.0f;
        const char* e = getenv("SPLAT_CLOUD_H"); if (e) sscanf(e, "%f,%f", &hb, &ht);
        e = getenv("SPLAT_HAZE"); if (e) sscanf(e, "%f,%f", &hz_d, &hz_y);   // (density scale, ground height)
    }
    // (1 clouds, 2 haze + light shafts, 4 cloud shadows on the ground: the SHADERS rows)
    // (maps under a roof -- glass or not -- get no rain: the screen test can't see a roof the depth buffer doesn't hold)
    extern char splat_scene_map[64];
    static const char* const k_indoor[] = { "Maze00", "Warehouse00", "Office00", "Pivot00", "PlazaLobby", "Room_" };   // (Kelp Dome, Walleye Warehouse, Ancho-V Games, Museum d'Alfonsino, the lobby, the shops)
    bool indoor = false;
    for (size_t i = 0; i < sizeof(k_indoor) / sizeof(k_indoor[0]); i++) if (!strncmp(splat_scene_map, k_indoor[i], strlen(k_indoor[i]))) indoor = true;
    extern volatile int splat_freecam_on;
    int fl = (g_vk.sh_clouds ? 1 : 0) | (g_vk.sh_rays ? 2 : 0) | (g_vk.sh_cshadow ? 4 : 0) | (g_vk.sh_ao ? 32 : 0) | (g_vk.sh_rain && storm && !indoor ? 256 : 0) | (splat_freecam_on ? 512 : 0) |
             (splat_vol_dbg == 1 ? 8 : splat_vol_dbg == 2 ? 16 : splat_vol_dbg == 3 ? 64 : splat_vol_dbg == 4 ? 128 : 0);
    static float ao_r = -1.0f;
    if (ao_r < 0.0f) { const char* e = getenv("SPLAT_AO_R"); ao_r = e ? (float)atof(e) : 24.0f; }   // (world units)
    float pcv[32] = {
        cp[0], cp[1], cp[2], sky_time(),
        fw[0], fw[1], fw[2], tx,
        rx, ao_r, rz, ty,
        sn[0], sn[1], sn[2], nr,
        fr, hb, storm ? ht : ht * 1.35f, storm ? 0.66f : 0.42f,   // (Vibrant: room above the layer for the cumulonimbus towers; half the sky open)
        (float)g_vk.sky_mode, (float)(g_vk.sh_quality == 0 ? 28 : g_vk.sh_quality == 1 ? 44 : 72), (storm ? 0.00016f : 0.000022f) * hz_d, hz_y,
        storm ? 70.0f : 30.0f, storm ? 30.0f : 10.0f, storm ? 1.4f : 1.0f, storm ? 0.0042f : 0.0030f,
        storm ? 0.35f : 0.14f, storm ? 0.45f : 0.30f, (float)fl, storm ? 0.6f : 1.0f };
    static u64 cam_log, cam_frame0;
    static u32 cam_calls0, cam_fb0;
    g_vol_calls++;
    if (g.frame >= cam_log) {
        printf("[shaders] camera at %.0f %.0f %.0f, fov %.1f deg, near %.1f far %.0f, sun %.2f %.2f %.2f, effects %d | pass ran in %u of the last %llu frames (%u by the tone-map fallback); scene draws after the pass %u..%u a frame\n",
               cp[0], cp[1], cp[2], fovy, nr, fr, sn[0], sn[1], sn[2], fl, g_vol_calls - cam_calls0, (unsigned long long)(g.frame - cam_frame0), g_vol_fallbacks - cam_fb0,
               g_vol_after_min == ~0u ? 0 : g_vol_after_min, g_vol_after_max);
        g_vol_after_min = ~0u; g_vol_after_max = 0;
        cam_log = g.frame + 600; cam_frame0 = g.frame; cam_calls0 = g_vol_calls; cam_fb0 = g_vol_fallbacks;
    }
    VkDescriptorSet ds0 = g_vol.sets[g.cur * VOL_SETS], ds1 = g_vol.sets[g.cur * VOL_SETS + 1], dsm = g_vol.sets[g.cur * VOL_SETS + 2];
    VkImageView dv = sample_view(&dsurf->img, VK_IMAGE_VIEW_TYPE_2D, dsurf->img.format, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    VkImageView shv = sample_view(&g_vol.shape, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8_UNORM, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    VkImageView dtv = sample_view(&g_vol.detail, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8_UNORM, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    // the cloud shadow map (light shafts, shadows on the ground): sunlight through the layer above each spot of the cloud base
    if (!g_vol.smap.image && !make_image(&g_vol.smap, VK_FORMAT_R16G16B16A16_SFLOAT, 512, 512, 1, VK_IMAGE_TYPE_2D, false, true, false)) return;
    if (!g_vol.smap_ready) { VkCommandBuffer cb = transfer_cmd(); image_layout_init(cb, &g_vol.smap); g_vol.smap_ready = true; }
    sec_mark(transfer_cmd(), SEC_VOL_MARCH, 0);
    if ((fl & 1) && (fl & 6)) {
        vol_set(dsm, 1, g_sx.repeat, shv);
        vol_set(dsm, 2, g_sx.repeat, dtv);
        vol_set(dsm, 4, g_fx.sampler, g_sky_orig_view);
        vol_draw(g_vol.smap_pipe, &g_vol.smap, VK_ATTACHMENT_LOAD_OP_DONT_CARE, dsm, pcv);
        barrier_all(transfer_cmd());
    }
    vol_set(ds0, 0, g_vol.nearest, dv);
    vol_set(ds0, 1, g_sx.repeat, shv);
    vol_set(ds0, 2, g_sx.repeat, dtv);
    vol_set(ds0, 3, g_fx.sampler, sample_view(&g_vol.smap, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 0 | (1 << 3) | (2 << 6) | (3 << 9)));
    vol_set(ds0, 4, g_fx.sampler, g_sky_orig_view);
    vol_draw(g_vol.march, &g_vol.half, VK_ATTACHMENT_LOAD_OP_DONT_CARE, ds0, pcv);
    VkCommandBuffer cb = transfer_cmd();
    sec_mark(cb, SEC_VOL_MARCH, 1);
    sec_mark(cb, SEC_VOL_COMP, 0);
    barrier_all(cb);
    vol_set(ds1, 0, g_vol.nearest, dv);
    vol_set(ds1, 1, g_fx.sampler, sample_view(&g_vol.half, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 0 | (1 << 3) | (2 << 6) | (3 << 9)));
    vol_set(ds1, 2, g_sx.repeat, sample_view(&g_vol.detail, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8_UNORM, 0 | (1 << 3) | (2 << 6) | (3 << 9)));
    // (rain: the puddles reflect a copy of the scene as it is now -- the blend below draws into the scene itself)
    VkImageView scv = sample_view(&g_vol.smap, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 0 | (1 << 3) | (2 << 6) | (3 << 9));
    if (fl & 256) {
        if (g_vol.scopy.image && (g_vol.scopy.w != scene->img.w || g_vol.scopy.h != scene->img.h || g_vol.scopy.format != scene->img.format)) {
            memset(&g_vol.scopy, 0, sizeof(g_vol.scopy));   // (a new scene size or format: the old copy is left behind -- rare)
            g_vol.scopy_ready = false;
        }
        if (g_vol.scopy.image || make_image(&g_vol.scopy, scene->img.format, scene->img.w, scene->img.h, 1, VK_IMAGE_TYPE_2D, false, false, false)) {
            VkCommandBuffer ccb = transfer_cmd();
            if (!g_vol.scopy_ready) { image_layout_init(ccb, &g_vol.scopy); g_vol.scopy_ready = true; }
            barrier_all(ccb);
            VkImageCopy rg;
            memset(&rg, 0, sizeof(rg));
            rg.srcSubresource.aspectMask = rg.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg.srcSubresource.layerCount = rg.dstSubresource.layerCount = 1;
            rg.extent.width = scene->img.w;
            rg.extent.height = scene->img.h;
            rg.extent.depth = 1;
            vkCmdCopyImage(ccb, scene->img.image, VK_IMAGE_LAYOUT_GENERAL, g_vol.scopy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            barrier_all(ccb);
            scv = sample_view(&g_vol.scopy, VK_IMAGE_VIEW_TYPE_2D, scene->img.format, 0 | (1 << 3) | (2 << 6) | (3 << 9));
        }
    }
    vol_set(ds1, 3, g_fx.sampler, scv);
    { VkImageView sv = sky_texture_view(); vol_set(ds1, 4, g_sx.repeat, sv ? sv : g_sky_orig_view); }
    vol_draw(g_vol.comp, &scene->img, VK_ATTACHMENT_LOAD_OP_LOAD, ds1, pcv);
    sec_mark(transfer_cmd(), SEC_VOL_COMP, 1);
    if (fl & 256) rain_probe(dv, pcv);
    else splat_rain_level_set(0.0f);
    static int logged;
    if (logged++ < 2) printf("[shaders] 3D clouds / haze drawn (%ux%u, %s)\n", hw, hh, storm ? "storm" : "Vibrant");
}
// The anti-aliased picture of this frame, made now if the setting is on.
static void fx_pass(VkCommandBuffer cb, int sub) {
    static int env = -1;
    if (env < 0) { env = getenv("SPLAT_AA") != NULL; if (env) g_vk.aa_mode = 1; }
    if (!g_vk.aa_mode || !g.tv_valid || !fx_init()) return;
    if (g_fx.out.image && (g_fx.out.w != g.tv.w || g_fx.out.h != g.tv.h)) {
        // (a new TV size: the old image is left behind -- it happens once, if ever)
        memset(&g_fx.out, 0, sizeof(g_fx.out));
    }
    if (!g_fx.out.image) {
        if (!make_image(&g_fx.out, VK_FORMAT_R8G8B8A8_UNORM, g.tv.w, g.tv.h, 1, VK_IMAGE_TYPE_2D, false, true, false)) { g_fx.ok = false; return; }
        image_layout_init(cb, &g_fx.out);
    }
    end_pass();
    barrier_all(cb);
    // (a plain view of the TV picture: the texture views carry the game's channel selection)
    static VkImageView tv_view;
    static VkImage tv_view_of;
    if (!tv_view || tv_view_of != g.tv.image) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = g.tv.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.tv.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g_vk.device, &vi, NULL, &v) != VK_SUCCESS) { g_fx.ok = false; return; }
        tv_view = v;   // (the old one, if any, is left behind: the TV image changes once, if ever)
        tv_view_of = g.tv.image;
    }
    VkDescriptorImageInfo di = { g_fx.sampler, tv_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = g_fx.sets[sub];
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.device, 1, &wr, 0, NULL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = att_view(&g_fx.out, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = g.tv.w;
    ri.renderArea.extent.height = g.tv.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)g.tv.w, (float)g.tv.h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { g.tv.w, g.tv.h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx.layout, 0, 1, &g_fx.sets[sub], 0, NULL);
    float inv[2] = { 1.0f / g.tv.w, 1.0f / g.tv.h };
    vkCmdPushConstants(cb, g_fx.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, inv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_dyn.valid = false;   // (this pass set its own viewport, scissor and pipeline)
    barrier_all(cb);
    g.need_barrier = true;
    g_fx.ready_frame = g.frame;
}
// What the debug views show instead of the picture (or on top of it): views 2 and 3 their own image
// of flat colours, view 4 the overdraw counts coloured (blue 1, cyan 2, green 3, yellow 4-5, orange
// 6-7, red 8-15, white 16+), view 5 the picture with the NaN draws in red over it.
static struct { bool tried, ok; Img out; VkPipeline copy, ramp, over; VkDescriptorPool pool; VkDescriptorSet sets[NSUB]; VkSampler sampler; u64 ready_frame; } g_dvh;
static const char* k_dvc_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec2 inv; } pc;\n"
    "void main(){ o = vec4(texture(tex, uv).rgb, 1.0); }\n";
static const char* k_dvo_fs =   // (the NaN draws' red, where they drew)
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec2 inv; } pc;\n"
    "void main(){ o = texture(tex, uv); }\n";
static const char* k_dvh_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D tex;\n"
    "layout(push_constant) uniform P { vec2 inv; } pc;\n"
    "void main(){ float n = texture(tex, uv).r; vec3 c;\n"
    "  if (n < 0.5) c = vec3(0.0); else if (n < 1.5) c = vec3(0.0, 0.2, 1.0); else if (n < 2.5) c = vec3(0.0, 0.85, 1.0);\n"
    "  else if (n < 3.5) c = vec3(0.1, 0.9, 0.2); else if (n < 5.5) c = vec3(1.0, 0.9, 0.1); else if (n < 7.5) c = vec3(1.0, 0.5, 0.0);\n"
    "  else if (n < 15.5) c = vec3(1.0, 0.05, 0.05); else c = vec3(1.0);\n"
    "  o = vec4(c, 1.0); }\n";
static void dv_present_pass(VkCommandBuffer cb, int sub) {
    int dv = g_vk.debug_view;
    Img* src = dv == 4 ? &g_dv_heat : &g_dv_img;
    u64 tag = dv == 4 ? g_dv_heat_tag : g_dv_img_tag;
    if (dv < 2 || tag != g.frame || !src->image || !g.tv_valid || !fx_init()) return;
    if (!g_dvh.tried) {
        g_dvh.tried = true;
        g_dvh.copy = fx_pipeline(k_dvc_fs, false);
        g_dvh.ramp = fx_pipeline(k_dvh_fs, false);
        g_dvh.over = fx_pipeline(k_dvo_fs, true);
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSUB };
        VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pc.maxSets = NSUB;
        pc.poolSizeCount = 1;
        pc.pPoolSizes = &ps;
        bool ok = g_dvh.copy && g_dvh.ramp && g_dvh.over && vkCreateDescriptorPool(g_vk.device, &pc, NULL, &g_dvh.pool) == VK_SUCCESS;
        for (int i = 0; ok && i < NSUB; i++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            ai.descriptorPool = g_dvh.pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &g_fx.dsl;
            ok = vkAllocateDescriptorSets(g_vk.device, &ai, &g_dvh.sets[i]) == VK_SUCCESS;
        }
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };   // (nearest: counts and edges aren't blended)
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ok = ok && vkCreateSampler(g_vk.device, &sci, NULL, &g_dvh.sampler) == VK_SUCCESS;
        g_dvh.ok = ok;
        if (!ok) printf("[render] debug views: their picture passes couldn't be set up\n");
    }
    if (!g_dvh.ok) return;
    if (g_dvh.out.image && (g_dvh.out.w != g.tv.w || g_dvh.out.h != g.tv.h)) memset(&g_dvh.out, 0, sizeof(g_dvh.out));   // (a new TV size: rare)
    if (!g_dvh.out.image && !make_image(&g_dvh.out, VK_FORMAT_R8G8B8A8_UNORM, g.tv.w, g.tv.h, 1, VK_IMAGE_TYPE_2D, false, true, false)) { g_dvh.ok = false; return; }
    end_pass();
    barrier_all(cb);
    if (dv == 5) {   // (the picture first; the red goes over it)
        VkImageCopy c;
        memset(&c, 0, sizeof(c));
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = g.tv.w;
        c.extent.height = g.tv.h;
        c.extent.depth = 1;
        Img* pic = g_fx.ready_frame == g.frame && g_fx.out.image ? &g_fx.out : &g.tv;
        vkCmdCopyImage(cb, pic->image, VK_IMAGE_LAYOUT_GENERAL, g_dvh.out.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        barrier_all(cb);
    }
    VkDescriptorImageInfo di = { g_dvh.sampler, src->att_view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = g_dvh.sets[sub];
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(g_vk.device, 1, &wr, 0, NULL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = att_view(&g_dvh.out, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = dv == 5 ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = g.tv.w;
    ri.renderArea.extent.height = g.tv.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)g.tv.w, (float)g.tv.h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { g.tv.w, g.tv.h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dv == 4 ? g_dvh.ramp : dv == 5 ? g_dvh.over : g_dvh.copy);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fx.layout, 0, 1, &g_dvh.sets[sub], 0, NULL);
    float inv[2] = { 1.0f / g.tv.w, 1.0f / g.tv.h };
    vkCmdPushConstants(cb, g_fx.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, inv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    g_dyn.valid = false;
    barrier_all(cb);
    g.need_barrier = true;
    g_dvh.ready_frame = g.frame;
}
// What to show (and screenshot): a debug view's picture, else the anti-aliased picture when it was
// made this frame.
static Img* present_source(void) {
    if (g_dvh.ready_frame == g.frame && g_dvh.out.image) return &g_dvh.out;
    return g_fx.ready_frame == g.frame && g_fx.out.image ? &g_fx.out : &g.tv;
}

// F8 pick: set up the queries for the next frame's 3D draws (cb: this frame's, outside a pass).
static bool pick_start(VkCommandBuffer cb) {
    if (!g_pick_pool) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = PICK_MAX;
        if (vkCreateQueryPool(g_vk.device, &qi, NULL, &g_pick_pool) != VK_SUCCESS) { g_pick_pool = VK_NULL_HANDLE; return false; }
    }
    if (!g_pick_info && !(g_pick_info = (char (*)[1024])calloc(PICK_MAX, 1024))) return false;
    vkCmdResetQueryPool(cb, g_pick_pool, 0, PICK_MAX);
    g_pick_px = g_vk.pick_x < 0 ? 0 : g_vk.pick_x > 1279 ? 1279 : g_vk.pick_x;
    g_pick_py = g_vk.pick_y < 0 ? 0 : g_vk.pick_y > 719 ? 719 : g_vk.pick_y;
    g_pick_n = 0;
    return true;
}
// After the picked frame was handed to the graphics card: wait for it, print the draws that drew there.
static void pick_report(void) {
    vkQueueWaitIdle(g_vk.queue);   // (once per click)
    static u64 res[PICK_MAX];
    u32 n = g_pick_n, hit = 0, last = 0;
    if (n && vkGetQueryPoolResults(g_vk.device, g_pick_pool, 0, n, sizeof(res), res, sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) n = 0;
    for (u32 i = 0; i < n; i++) if (res[i]) { hit++; last = i; }
    printf("[pick] pixel %d,%d of the TV picture: %u of the frame's %u 3D draws drew there%s\n", g_pick_px, g_pick_py, hit, n, hit ? " (the last one is on top):" : "");
    u32 shown = 0;
    for (int i = (int)n - 1; i >= 0 && shown < 8; i--) {
        if (!res[i]) continue;
        shown++;
        printf("[pick] %s %s\n", (u32)i == last ? "TOP " : "    ", g_pick_info[i]);
    }
}

// The frame tracker's graph (F6; frame_tracker.c has the numbers): the last 300 game frames as bars
// (height = frame time, 0-50 ms; a line at 16.7 and 33.3), each in its cause's colour -- green on
// time, orange shader/pipeline builds, purple texture work, cyan graphics card, red game logic,
// yellow drawing thread, grey waiting, magenta pacing/system -- with a white dot at the main thread's
// work and a light-blue one at the card's; above: the last second's averages and late frames by cause.
#define TRK_W 600
#define TRK_GH 100
#define TRK_H (2 * (OVF_H + 2) + 6 + TRK_GH)
static VkBuffer g_trk_buf[NSUB];
static VkDeviceMemory g_trk_mem[NSUB];
static void* g_trk_ptr[NSUB];
static const u8 k_trk_col[TR_CAUSES][3] = { { 60, 200, 90 }, { 255, 140, 0 }, { 190, 90, 255 }, { 0, 200, 255 }, { 255, 60, 60 }, { 255, 220, 40 }, { 170, 170, 170 }, { 255, 0, 200 } };
static void trk_text(u8* px, bool bgra, int* x, int y, const char* t, u8 r, u8 gg, u8 bl) {
    for (; *t; t++) {
        int ci = (u8)*t - OVF_FIRST;
        if (ci < 0 || ci >= OVF_COUNT) ci = 0;
        int w = ovf_width[ci];
        const u8* a = ovf_alpha + ovf_offset[ci];
        for (int yy = 0; yy < OVF_H; yy++)
            for (int xx = 0; xx < w; xx++) {
                int ax = *x + xx, ay = y + yy;
                u32 al = a[yy * w + xx];
                if (!al || ax < 0 || ax >= TRK_W || ay < 0 || ay >= TRK_H) continue;
                u8* p = px + (ay * TRK_W + ax) * 4;
                u8 c0 = bgra ? bl : r, c2 = bgra ? r : bl;
                p[0] = (u8)((p[0] * (255 - al) + c0 * al) / 255);
                p[1] = (u8)((p[1] * (255 - al) + gg * al) / 255);
                p[2] = (u8)((p[2] * (255 - al) + c2 * al) / 255);
            }
        *x += w;
    }
}
static void draw_tracker(VkCommandBuffer cb, VkImage target, int sub) {
    static int env = -1;
    if (env < 0) { env = getenv("SPLAT_TRACKER") != NULL; if (env) g_vk.tracker_on = 1; }
    if (!g_vk.tracker_on) return;
    u32 W = g.sc_extent.width, H = g.sc_extent.height;
    if (W < TRK_W + 16 || H < TRK_H + 80) return;
    if (!g_trk_buf[sub] && !host_buffer(TRK_W * TRK_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_trk_buf[sub], &g_trk_mem[sub], &g_trk_ptr[sub])) { g_trk_buf[sub] = VK_NULL_HANDLE; return; }
    static u8 px[TRK_W * TRK_H * 4];
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    for (u32 y = 0; y < TRK_H; y++)
        for (u32 x = 0; x < TRK_W; x++) {
            u8* p = px + (y * TRK_W + x) * 4;
            bool edge = y == 0 || y == TRK_H - 1 || x == 0 || x == TRK_W - 1;
            p[0] = p[1] = p[2] = edge ? 60 : 14;
            p[3] = 255;
        }
    u32 head = g_tr_head, n = head < 300 ? head : 300;
    double cpu = 0, drw = 0, gpu = 0;
    u32 m = 0, late = 0, by[TR_CAUSES] = { 0 };
    for (u32 i = 0; i < n; i++) {
        const TrackFrame* f = &g_tr[(head - n + i) % TR_N];
        if (i + 60 >= n) { cpu += f->main_busy_ms; drw += f->draw_ms; gpu += f->gpu_ms; m++; }
        if (f->cause != TR_OK) { late++; by[f->cause < TR_CAUSES ? f->cause : TR_PACE]++; }
    }
    char t[160];
    int x = 8;
    snprintf(t, sizeof(t), "CPU (game) %.1f ms   DRAWING %.1f ms   GPU %.1f ms   (last second)", m ? cpu / m : 0, m ? drw / m : 0, m ? gpu / m : 0);
    trk_text(px, bgra, &x, 3, t, 235, 235, 235);
    x = 8;
    snprintf(t, sizeof(t), "STUTTERS (5 s): %u", late);
    trk_text(px, bgra, &x, OVF_H + 4, t, late ? 255 : 120, late ? 120 : 220, late ? 120 : 120);
    for (int c = 1; c < TR_CAUSES; c++) {
        if (!by[c]) continue;
        static const char* const sh[TR_CAUSES] = { "", "builds", "textures", "GPU", "game", "drawing", "waiting", "pacing" };
        snprintf(t, sizeof(t), "   %s %u", sh[c], by[c]);
        trk_text(px, bgra, &x, OVF_H + 4, t, k_trk_col[c][0], k_trk_col[c][1], k_trk_col[c][2]);
    }
    int gy0 = 2 * (OVF_H + 2) + 4, gy1 = gy0 + TRK_GH;   // graph rows; 0 ms at gy1
    float scale = (float)TRK_GH / 50.0f;
    for (int x2 = 1; x2 < TRK_W - 1; x2++) {   // 16.7 and 33.3 ms lines
        u8* p = px + ((gy1 - (int)(16.7f * scale)) * TRK_W + x2) * 4;
        p[0] = p[1] = p[2] = 70;
        p = px + ((gy1 - (int)(33.3f * scale)) * TRK_W + x2) * 4;
        p[bgra ? 2 : 0] = 110; p[1] = 40; p[bgra ? 0 : 2] = 40;
    }
    for (u32 i = 0; i < n; i++) {
        const TrackFrame* f = &g_tr[(head - n + i) % TR_N];
        int bx = (int)(TRK_W - 2 * n + 2 * i), bh = (int)(f->ms * scale);
        if (bh > TRK_GH) bh = TRK_GH;
        const u8* c = k_trk_col[f->cause < TR_CAUSES ? f->cause : TR_PACE];
        for (int k = 0; k < 2; k++) {
            int xx = bx + k;
            if (xx <= 0 || xx >= TRK_W - 1) continue;
            for (int y = gy1 - bh; y < gy1; y++) {
                u8* p = px + (y * TRK_W + xx) * 4;
                p[0] = bgra ? c[2] : c[0]; p[1] = c[1]; p[2] = bgra ? c[0] : c[2];
            }
            int yb = gy1 - (int)(f->main_busy_ms * scale), yg = gy1 - (int)(f->gpu_ms * scale);
            if (yb > gy0 && yb < gy1) { u8* p = px + (yb * TRK_W + xx) * 4; p[0] = p[1] = p[2] = 255; }
            if (yg > gy0 && yg < gy1) { u8* p = px + (yg * TRK_W + xx) * 4; p[bgra ? 2 : 0] = 120; p[1] = 200; p[bgra ? 0 : 2] = 255; }
        }
    }
    memcpy(g_trk_ptr[sub], px, sizeof(px));
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferRowLength = TRK_W;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = 8;
    c.imageOffset.y = (int)(H - TRK_H - 8);
    c.imageExtent.width = TRK_W;
    c.imageExtent.height = TRK_H;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g_trk_buf[sub], target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// The debug views' line under the counter (which view, the frame's numbers) and view 5's magenta frame.
static VkBuffer g_dbar_buf[NSUB];
static VkDeviceMemory g_dbar_mem[NSUB];
static void* g_dbar_ptr[NSUB];
static void draw_debug_line(VkCommandBuffer cb, VkImage target, int sub, int ox, int oy, int dw, int dh) {
    if (!g_vk.debug_view && !g_vk.debug_pick) return;
    if (g_vk.debug_view == 5 && g_dv_last.skipped) {   // (draws were left out this frame: a magenta frame around the picture)
        make_solids();
        if (g_solid_ok) {
            int t = dh / 120 + 2;
            blit_rect(cb, &g_solid[SOLID_MAGENTA], target, ox, oy, dw, t, VK_FILTER_NEAREST);
            blit_rect(cb, &g_solid[SOLID_MAGENTA], target, ox, oy + dh - t, dw, t, VK_FILTER_NEAREST);
            blit_rect(cb, &g_solid[SOLID_MAGENTA], target, ox, oy, t, dh, VK_FILTER_NEAREST);
            blit_rect(cb, &g_solid[SOLID_MAGENTA], target, ox + dw - t, oy, t, dh, VK_FILTER_NEAREST);
            transfer_order(cb);
        }
    }
    if (!g_dbar_buf[sub] && !host_buffer(BAR_MAXW * BAR_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_dbar_buf[sub], &g_dbar_mem[sub], &g_dbar_ptr[sub])) { g_dbar_buf[sub] = VK_NULL_HANDLE; return; }
    static const char* const names[6] = { "NORMAL", "WIREFRAME", "WIREFRAME + SHADER COLORS", "SOLID SHADER COLORS", "OVERDRAW HEATMAP", "SKIPPED/PENDING DRAWS + NaN" };
    char view[64], nums[160], nan[32] = "";
    snprintf(view, sizeof(view), "%s%s", names[g_vk.debug_view % 6], (g_vk.debug_view == 1 || g_vk.debug_view == 2) && !g_vk.wireframe ? " (no line drawing here)" : "");
    snprintf(nums, sizeof(nums), "3D DRAWS %u   SHADER PAIRS %u   SKIPPED %u   PIPELINES BUILT %u", g_dv_last.scene, g_dv_last.shaders, g_dv_last.skipped, g_dv_last.built);
    if (g_vk.debug_view == 5) snprintf(nan, sizeof(nan), "   NaN DRAWS %u", g_dv_last.nan);
    const char* sep = "   |   ";
    const char* pick = g_vk.debug_pick ? "   |   PICK: CLICK THE PICTURE" : "";
    int tw = text_width("VIEW ") + text_width(view) + text_width(sep) + text_width(nums) + text_width(nan) + text_width(pick);
    u32 w = (u32)tw + 20;
    if (w > BAR_MAXW) w = BAR_MAXW;
    int y = 8 + BAR_H + 4;
    if (g.sc_extent.width < w + 16 || g.sc_extent.height < (u32)y + BAR_H + 16) return;
    static u8 px[BAR_MAXW * BAR_H * 4];
    Bar b = { px, w, g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB, 10 };
    for (u32 yy = 0; yy < BAR_H; yy++)
        for (u32 x = 0; x < w; x++) {
            u8* p = b.px + (yy * BAR_MAXW + x) * 4;
            bool edge = yy == 0 || yy == BAR_H - 1 || x == 0 || x == w - 1;
            p[0] = p[1] = p[2] = edge ? 60 : 18;
            p[3] = 255;
        }
    bar_text(&b, "VIEW ", 170, 170, 170);
    bar_text(&b, view, 255, 120, 255);
    bar_text(&b, sep, 90, 90, 90);
    bar_text(&b, nums, 255, 255, 255);
    if (nan[0]) bar_text(&b, nan, 255, g_dv_last.nan ? 60 : 255, g_dv_last.nan ? 60 : 255);
    if (pick[0]) bar_text(&b, pick, 255, 210, 80);
    for (u32 yy = 0; yy < BAR_H; yy++) memcpy((u8*)g_dbar_ptr[sub] + yy * BAR_MAXW * 4, px + yy * BAR_MAXW * 4, w * 4);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferRowLength = BAR_MAXW;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = 8;
    c.imageOffset.y = y;
    c.imageExtent.width = w;
    c.imageExtent.height = BAR_H;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g_dbar_buf[sub], target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// --- The settings panel (windowed mode) ------------------------------------------------------------
// A small SETTINGS box top right; clicking it (or F2) opens rows the player
// clicks to change. Drawn like the counter: text into a host buffer, copied
// onto the picture. input.c turns clicks into changes (it reads the places
// recorded in g_vk); display.c does the window changes.
#define SET_ROWS 40      // (settings by number: input.c's cases)
#define SET_W 420        // (one column of the panel)
#define SET_GAP 12
#define SET_MAXROWS 48   // (both columns' rows in one: a narrow window shows them all in one column)
static VkBuffer g_set_buf[NSUB];
static VkDeviceMemory g_set_mem[NSUB];
static void* g_set_ptr[NSUB];
static void bar_fill(u8* px, u32 w, u32 h, u8 v, bool edge_box) {
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u8* p = px + (y * BAR_MAXW + x) * 4;
            bool edge = edge_box && (y == 0 || y == h - 1 || x == 0 || x == w - 1);
            p[0] = p[1] = p[2] = edge ? 60 : v;
            p[3] = 255;
        }
}
static void bar_copy(VkCommandBuffer cb, VkBuffer buf, size_t offset, VkImage target, int x, int y, u32 w, u32 h) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferOffset = offset;
    c.bufferRowLength = BAR_MAXW;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = x;
    c.imageOffset.y = y;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, buf, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}
// The benchmark (benchmark.c): its status line while it runs (top middle), its results panel when it's done
static void draw_bench(VkCommandBuffer cb, VkImage target, int sub) {
    const char* splat_bench_status(void);
    int splat_bench_lines(const char*** out);
    int splat_bench_hud(const char*** out, float* progress);
    const char* status = splat_bench_status();
    const char** lines = NULL;
    float progress = 0.0f;
    int hud = splat_bench_hud(&lines, &progress);   // (the run's readout: a few lines and a progress bar, top left)
    int n = hud ? hud : status ? 0 : splat_bench_lines(&lines);
    if (!hud && !status && !n) return;
    if (g_vk.hide_settings && !status && !hud) return;   // (F10 hides the results)
    static VkBuffer buf[NSUB];
    static VkDeviceMemory mem[NSUB];
    static void* ptr[NSUB];
    const bool one = !hud && status;
    const bool bar = hud && progress >= 0.0f;   // (the Bot Lobby's readout has no bar)
    const u32 rows = one ? 1 : (u32)n + (bar ? 1 : 0), H = rows * BAR_H;
    if (!buf[sub] && !host_buffer((VkDeviceSize)BAR_MAXW * 40 * BAR_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf[sub], &mem[sub], &ptr[sub])) { buf[sub] = VK_NULL_HANDLE; return; }
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    u8* px = (u8*)ptr[sub];
    u32 w = 0;
    if (one) w = (u32)text_width(status) + 24;
    else for (int i = 0; i < n; i++) { u32 tw = (u32)text_width(lines[i]) + 24; if (tw > w) w = tw; }
    if (hud && w < 520) w = 520;
    if (w > BAR_MAXW) w = BAR_MAXW;
    if (g.sc_extent.width < w + 16 || g.sc_extent.height < H + 16) return;
    // (redrawn only when the text or the bar changes, in ordinary memory, then copied: drawing it every frame
    // straight into the upload buffer -- write-combined memory the text's blending reads back byte by byte --
    // was 37% of the drawing thread's own work during a benchmark, which then measured itself.
    // SPLAT_BENCH_HUD_OLD=1: the old way)
    static int old_hud = -1;
    if (old_hud < 0) old_hud = getenv("SPLAT_BENCH_HUD_OLD") != NULL;
    u32 fill_px = hud ? (u32)((float)(w - 24) * (progress < 0 ? 0 : progress > 1 ? 1 : progress)) : 0;
    u64 sig = 0xCBF29CE484222325ull ^ ((u64)w << 1) ^ ((u64)rows << 20) ^ ((u64)fill_px << 32) ^ (bgra ? 7 : 3) ^ (one ? 0x100 : 0);
    if (one) { for (const char* c = status; *c; c++) sig = (sig ^ (u8)*c) * 0x100000001B3ull; }
    else for (int i = 0; i < n; i++) { for (const char* c = lines[i]; *c; c++) sig = (sig ^ (u8)*c) * 0x100000001B3ull; sig = (sig ^ 0xFF) * 0x100000001B3ull; }
    static u64 drawn_sig[NSUB];
    static u8* cpu_px;
    if (!old_hud && !cpu_px) cpu_px = (u8*)malloc((size_t)BAR_MAXW * 40 * BAR_H * 4);
    if (!old_hud && cpu_px && drawn_sig[sub] == sig) {
        transfer_order(cb);
        bar_copy(cb, buf[sub], 0, target, one ? ((int)g.sc_extent.width - (int)w) / 2 : 8, one ? 8 + BAR_H + 4 : 8 + BAR_H + 8, w, H);
        return;
    }
    u8* up = px;   // (the upload buffer)
    if (!old_hud && cpu_px) { px = cpu_px; drawn_sig[sub] = sig; }
    bar_fill(px, w, H, 24, true);
    if (one) { Bar b = { px, w, bgra, 12 }; bar_text(&b, status, 230, 230, 230); }
    else for (int i = 0; i < n; i++) { Bar b = { px + (size_t)i * BAR_H * BAR_MAXW * 4, w, bgra, 12 }; bar_text(&b, lines[i], i == 0 ? 255 : 220, i == 0 ? 220 : 220, i == 0 ? 120 : 220); }
    if (bar) {   // (the bar: the whole run's progress)
        u32 y0 = (u32)n * BAR_H + 6, y1 = H - 6, x0 = 12, x1 = w - 12, fill = x0 + (u32)((float)(x1 - x0) * (progress < 0 ? 0 : progress > 1 ? 1 : progress));
        for (u32 y = y0; y < y1; y++)
            for (u32 x = x0; x < x1; x++) {
                u8* p = px + (y * BAR_MAXW + x) * 4;
                bool in = x < fill;
                u8 r = in ? 255 : 70, gg = in ? 220 : 70, bl = in ? 120 : 70;
                p[0] = bgra ? bl : r; p[1] = gg; p[2] = bgra ? r : bl; p[3] = 255;
            }
    }
    if (px != up) for (u32 y = 0; y < H; y++) memcpy(up + (size_t)y * BAR_MAXW * 4, px + (size_t)y * BAR_MAXW * 4, (size_t)w * 4);
    transfer_order(cb);
    bar_copy(cb, buf[sub], 0, target, one ? ((int)g.sc_extent.width - (int)w) / 2 : 8, one ? 8 + BAR_H + 4 : 8 + BAR_H + 8, w, H);
}
// The warm-up overlay in play: "Preparing graphics: <place>  120 / 811" while a scene's pipelines build (top middle)
static void draw_warm(VkCommandBuffer cb, VkImage target, int sub) {
    if (!g_warm_n) return;
    u32 left = warm_left();
    if (!left) { printf("[render] shader warm-up: %s's %u pipelines built (%.1f s)\n", g_warm_scene, g_warm_n, (hnow_ns() - g_warm_t0) / 1e9); g_warm_n = 0; return; }
    static VkBuffer buf[NSUB];
    static VkDeviceMemory mem[NSUB];
    static void* ptr[NSUB];
    if (!buf[sub] && !host_buffer((VkDeviceSize)BAR_MAXW * BAR_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf[sub], &mem[sub], &ptr[sub])) { buf[sub] = VK_NULL_HANDLE; return; }
    const char* splat_place_name(void);
    char line[160];
    snprintf(line, sizeof(line), "Preparing graphics: %s  %u / %u", splat_place_name(), g_warm_n - left, g_warm_n);
    u32 w = (u32)text_width(line) + 24;
    if (w > BAR_MAXW) w = BAR_MAXW;
    if (g.sc_extent.width < w + 16 || g.sc_extent.height < BAR_H + 16) return;
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    u8* px = (u8*)ptr[sub];
    bar_fill(px, w, BAR_H, 24, true);
    u32 fill = (u32)((w - 2) * (float)(g_warm_n - left) / (float)g_warm_n);
    for (u32 y = BAR_H - 4; y < BAR_H - 1; y++) for (u32 x = 1; x < 1 + fill; x++) { u8* q = px + ((size_t)y * BAR_MAXW + x) * 4; q[0] = 120; q[1] = 200; q[2] = 120; }
    Bar b = { px, w, bgra, 12 };
    bar_text(&b, line, 230, 230, 230);
    transfer_order(cb);
    bar_copy(cb, buf[sub], 0, target, ((int)g.sc_extent.width - (int)w) / 2, 8, w, BAR_H);
}
// (the shader warm-up's progress picture: see preload_pipelines)
static void warm_present(const char* l1, const char* l2, float frac) {
    if (!g_vk.surface) return;
    wp_step("making the swapchain");
    if (!g.swapchain && !build_swapchain()) { wp_step(NULL); return; }
    wp_step("drawing the panel");
    static VkBuffer buf;
    static VkDeviceMemory mem;
    static void* ptr;
    const u32 W = 560, H = BAR_H * 2 + 28;
    if (!buf && !host_buffer((VkDeviceSize)BAR_MAXW * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf, &mem, &ptr)) return;
    if (W + 16 > g.sc_extent.width || H + 16 > g.sc_extent.height) return;
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    u8* px = (u8*)ptr;
    bar_fill(px, W, H, 22, true);
    Bar b1 = { px, W, bgra, 18 };
    bar_text(&b1, l1, 235, 235, 235);
    Bar b2 = { px + (size_t)BAR_H * BAR_MAXW * 4, W, bgra, 18 };
    bar_text(&b2, l2, 170, 170, 170);
    u32 fill = (u32)((W - 36) * (frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac));
    for (u32 y = BAR_H * 2 + 6; y < BAR_H * 2 + 20; y++)
        for (u32 x = 18; x < W - 18; x++) { u8* p = px + ((size_t)y * BAR_MAXW + x) * 4; u8 v = x - 18 < fill ? 190 : 48; p[0] = v; p[1] = x - 18 < fill ? 210 : 48; p[2] = v; p[3] = 255; }
    {   // SPLAT_WARM_TEST=1: the picture's pixels once, as images\warm_panel.png (the TV screenshot can't show it)
        static int test = -1;
        if (test < 0) test = getenv("SPLAT_WARM_TEST") != NULL;
        if (test == 1 && frac > 0.3f) {
            test = 2;
            u8* packed = (u8*)malloc((size_t)W * H * 4);
            if (packed) {
                for (u32 y = 0; y < H; y++) memcpy(packed + (size_t)y * W * 4, px + (size_t)y * BAR_MAXW * 4, (size_t)W * 4);
                char path[80];
                snprintf(path, sizeof(path), "%s/warm_panel.png", images_dir());
                write_png(path, packed, W, H);
                free(packed);
            }
        }
    }
    wp_step("a command buffer");
    VkCommandBuffer cb = transfer_cmd();
    Sub* s = &g.sub[g.cur];
    u32 index = 0;
    wp_step("acquiring a swapchain image");
    if (!acq_ready(100000000ull, 100000000ull, &index)) { wp_step(NULL); return; }   // (no free image, or one Windows hasn't released: no progress picture this time)
    wp_step("recording");
    VkImage im = g.sc_images[index];
    sc_barrier(cb, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue dark;
    memset(&dark, 0, sizeof(dark));
    dark.float32[0] = 0.04f; dark.float32[1] = 0.04f; dark.float32[2] = 0.05f; dark.float32[3] = 1.0f;
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dark, 1, &range);
    transfer_order(cb);
    bar_copy(cb, buf, 0, im, ((int)g.sc_extent.width - (int)W) / 2, ((int)g.sc_extent.height - (int)H) / 2, W, H);
    sc_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    end_pass();
    query_suspend(s->cb);
    ts_end(s->cb);
    vkEndCommandBuffer(s->cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cb;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_acq.last;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.sc_done[index];
    wp_step("submitting");
    VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, s->fence);
    gpu_lost_check(sr);
    s->busy = sr == VK_SUCCESS;
    g.cur = -1;
    g.need_barrier = false;
    if (sr != VK_SUCCESS) { wp_step(NULL); return; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sc_done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    wp_step("presenting");
    VkResult pr = vkQueuePresentKHR(g_vk.queue, &pi);
    gpu_lost_check(pr);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
    wp_step(NULL);
}
static void draw_settings(VkCommandBuffer cb, VkImage target, int sub) {
    (void)rs_wide();   // (every shown frame: hands the FOV / wide-screen settings to the game's projection functions)
    if (g_vk.gallery) { g_vk.gear_w = 0; g_vk.panel_rows = 0; return; }
    if (g_vk.hide_settings) { g_vk.gear_w = 0; g_vk.panel_rows = 0; return; }   // (F10: hidden, in a window and in fullscreen)
    u32 H = (u32)SET_MAXROWS * BAR_H + BAR_H + 8;   // (the box row, then the panel)
    if (!g_set_buf[sub] && !host_buffer((VkDeviceSize)BAR_MAXW * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_set_buf[sub], &g_set_mem[sub], &g_set_ptr[sub])) {
        g_set_buf[sub] = VK_NULL_HANDLE;
        return;
    }
    bool bgra = g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    u8* px = (u8*)g_set_ptr[sub];
    // the box
    u32 bw = (u32)text_width("SETTINGS - GRAPHICS ENGINE") + 20;
    if (g.sc_extent.width < bw + 24 || g.sc_extent.height < BAR_H + 24) { g_vk.gear_w = 0; g_vk.panel_rows = 0; return; }
    static int box_state[NSUB];
    if (box_state[sub] != (g_vk.settings_open ? 2 : 1) + (bgra ? 4 : 0)) {
        box_state[sub] = (g_vk.settings_open ? 2 : 1) + (bgra ? 4 : 0);
        bar_fill(px, bw, BAR_H, g_vk.settings_open ? 40 : 18, true);
        Bar b = { px, bw, bgra, 10 };
        bar_text(&b, "SETTINGS - GRAPHICS ENGINE", 220, 220, 220);
    }
    int gx = (int)(g_vk.fps_bar_w ? g_vk.fps_bar_w + 24 : 8), gy = 8;   // (next to the counter, away from the window's close button: clicks aimed at the box hit the X)
    bar_copy(cb, g_set_buf[sub], 0, target, gx, gy, bw, BAR_H);
    g_vk.gear_x = gx; g_vk.gear_y = gy; g_vk.gear_w = (int)bw; g_vk.gear_h = BAR_H;
    if (!g_vk.settings_open) { g_vk.panel_rows = 0; return; }
    // the panel
    u8* pp = px + (size_t)(BAR_H + 8) * BAR_MAXW * 4;
    char v[SET_ROWS][48];
    snprintf(v[0], 48, "%s   (F11)", g_vk.fullscreen ? "on" : "off");
    snprintf(v[1], 48, "%ux%u   (click)", g.sc_extent.width, g.sc_extent.height);
    snprintf(v[2], 48, "%s", g_vk.vsync_mode == 1 ? "on" : g_vk.vsync_mode == 2 ? "off (uncapped)" : "auto");
    snprintf(v[3], 48, "%s   (F3)", g_vk.hide_fps ? "off" : "on");
    snprintf(v[4], 48, "%s   (Ctrl+F4)", g_vk.no_mips ? "off" : "on");
    snprintf(v[5], 48, "%s   (M / hold View)", g_vk.drc_on ? "shown" : "hidden");
    snprintf(v[6], 48, "%s   (FXAA)", g_vk.aa_mode ? "on" : "off");
    {
        static const char* const rs[5] = { "1x     (720p)", "1.5x   (1080p)", "2x     (1440p)", "0.5x   (360p)", "0.75x  (540p)" };
        snprintf(v[7], 48, "%s", rs[g_vk.render_scale >= 0 && g_vk.render_scale < 5 ? g_vk.render_scale : 0]);
    }
    {
        extern volatile int splat_cam_fovy_x10, splat_cam_fovy_now_x10;
        int a = splat_cam_fovy_x10, b = splat_cam_fovy_now_x10;
        if (a) snprintf(v[8], 48, "%+d deg   (camera %d -> %d)", g_vk.fov_add, (a + 5) / 10, (b + 5) / 10);
        else snprintf(v[8], 48, "%+d deg   (click: +5)", g_vk.fov_add);
    }
    snprintf(v[9], 48, "%s", g_vk.wide_mode ? "fit the window" : "off (16:9)");
    snprintf(v[10], 48, "%s", g_vk.hud_mode ? "stretched" : "16:9, in the middle");
    snprintf(v[11], 48, "%s", g_vk.drc_size == 1 ? "small" : g_vk.drc_size == 2 ? "large" : g_vk.drc_size == 3 ? "full height" : "normal");
    snprintf(v[12], 48, "%s", g_vk.drc_cursor_speed == 1 ? "1.5x" : g_vk.drc_cursor_speed == 2 ? "2x" : g_vk.drc_cursor_speed == 3 ? "3x" : "1x");
    snprintf(v[13], 48, "%s", !g_vk.present_wait ? "not available" : g_vk.low_latency ? "on" : "off");
    snprintf(v[14], 48, "%d%%   (click: -20)", g_vk.volume);
    snprintf(v[15], 48, "%s", g_vk.perf_mode ? "on   (0.5x, no AA, plaza 30)" : "off");
    snprintf(v[16], 48, "%s", g_vk.drc_no_popup ? "off  (only M / hold View)" : "on");
    snprintf(v[17], 48, "%s", g_vk.debug_views_on ? "on   (F7 / F8)" : "off");
    snprintf(v[18], 48, "%s", g_vk.tracker_on ? "on   (Ctrl+F6)" : "off  (Ctrl+F6)");
    snprintf(v[19], 48, "%s", g_vk.pad_layout ? "Nintendo (by position)" : "Xbox (A = A, B = B)");
    snprintf(v[20], 48, "%s", g_vk.perf_mode ? "30   (Performance mode)" : g_vk.plaza_30 ? "30   (the game's own)" : "60");
    snprintf(v[22], 48, "%s   (F5 dances)", g_vk.emote_sound ? "on" : "off");
    {
        extern bool input_second_pad(void);
        snprintf(v[32], 48, "%s", !g_vk.pro_pad ? "off" : g_vk.pro_pad == 3 ? "Bot (Battle Dojo)" : g_vk.pro_pad == 2 ? "Pro Controller (always)" : input_second_pad() ? "2nd controller = Pro Ctrl" : "on   (plug in a 2nd pad)");
    snprintf(v[33], 48, "%s", g_vk.warm_shaders ? "on   (at start)" : "off");
    {
        static const char* const ms[5] = { "off", "1x", "1.5x", "2x", "0.5x" };
        snprintf(v[36], 48, "%s%s", ms[g_vk.mouse_aim >= 0 && g_vk.mouse_aim <= 4 ? g_vk.mouse_aim : 0], !g_vk.mouse_aim ? "" : g_vk.mouse_captured ? "   (aiming; Esc frees)" : "   (click the picture)");
        extern const char* gyro_name(void);
        const char* gn = gyro_name();
        snprintf(v[37], 48, "%s%s", g_vk.gyro_on ? "on   " : "off", g_vk.gyro_on ? (gn ? gn : "(no PlayStation pad)") : "");
    }
    snprintf(v[34], 48, "%s", g_vk.build_wait ? "wait  (no flicker)" : "skip  (no hitch)");
    snprintf(v[39], 48, "%s", !g_vk.dojo_both ? "alternate (30 each, as the console)" : g_vk.perf_mode ? "alternate (Performance mode)" : "both every frame (60)");
    snprintf(v[35], 48, "%s", fg_n() == 2 ? "2x  (120 fps)" : fg_n() == 3 ? "3x  (180 fps)" : fg_n() == 4 ? "4x  (240, VSync off)" : "off");
    }
    snprintf(v[24], 48, "%s", g_vk.sky_mode >= 2 ? "Storm" : g_vk.sky_mode ? "Vibrant" : "the game's own");
    snprintf(v[25], 48, "%s", g_vk.sh_clouds ? "on" : "off");
    snprintf(v[26], 48, "%s", g_vk.sh_grade ? "on" : "off");
    snprintf(v[38], 48, "%s", g_vk.motion_blur >= 2 ? "strong" : g_vk.motion_blur ? "light" : "off");
    snprintf(v[27], 48, "%s", g_vk.sh_quality == 0 ? "low" : g_vk.sh_quality == 1 ? "medium" : "high");
    snprintf(v[28], 48, "%s", g_vk.sh_rays ? "on" : "off");
    snprintf(v[29], 48, "%s", g_vk.sh_cshadow ? "on" : "off");
    snprintf(v[30], 48, "%s", g_vk.sh_ao ? "on" : "off");
    snprintf(v[31], 48, "%s", !g_vk.sh_rain ? "off" : g_vk.sky_mode >= 2 ? "on" : "on   (with Storm)");
    snprintf(v[23], 48, "%s", g_vk.shadow_quality == 0 ? "off  (faster)" : g_vk.shadow_quality == 1 ? "normal" : g_vk.perf_mode ? "sharp (normal in Performance mode)" : "sharp  (2x map)");
    // Grouped, in two columns when the window is wide enough (else one); the
    // group titles can't be clicked. input.c maps a click through panel_ids.
    typedef struct { int id; const char* name; } SetItem;
    static const SetItem col0[] = {
        { -1, "DISPLAY" }, { 0, "Fullscreen" }, { 1, "Window size" }, { 2, "VSync" }, { 13, "Low input lag" }, { 3, "FPS counter" }, { 18, "Frame tracker" },
        { -1, "GRAPHICS" }, { 7, "Render scale" }, { 6, "Anti-aliasing" }, { 23, "Shadows" }, { 4, "Texture mip levels" }, { 8, "Field of view" }, { 9, "Wide screen" }, { 10, "Wide-screen HUD" },
        { -1, "PERFORMANCE" }, { 15, "Performance mode" }, { 20, "Plaza frame rate" }, { 35, "Smooth motion" }, { 39, "Dojo views" }, { 33, "Shader warm-up" }, { 34, "New shaders" },
        { -1, "DEBUG" }, { 17, "Debug views" } };
    static const SetItem col1[] = {
        { -1, "GAMEPAD" }, { 5, "GamePad screen" }, { 11, "GamePad size" }, { 12, "GamePad cursor" }, { 16, "GamePad pop-up" },
        { -1, "CONTROLS & SOUND" }, { 19, "Button layout" }, { 32, "Player 2" }, { 36, "Mouse aim" }, { 37, "Motion controller" }, { 14, "Volume" }, { 22, "Emote sound" },
        { -1, "SHADERS" }, { 24, "Sky" }, { 25, "3D clouds" }, { 28, "God rays & haze" }, { 29, "Cloud shadows" }, { 30, "Ambient occlusion" }, { 31, "Rain" }, { 26, "Colour grade" }, { 38, "Motion blur" }, { 27, "Shader quality" } };
    const int n0 = (int)(sizeof(col0) / sizeof(col0[0])), n1 = (int)(sizeof(col1) / sizeof(col1[0]));
    int py = gy + BAR_H + 4, pxl = 8;   // (under the counter, from the left: two columns fit a 1280-wide window)
    bool two = g.sc_extent.width >= (u32)(pxl + 2 * SET_W + SET_GAP + 8) && g.sc_extent.height >= (u32)(py + (n0 > n1 ? n0 : n1) * BAR_H + 8);
    int rows = two ? (n0 > n1 ? n0 : n1) : n0 + n1;
    if (!two && (g.sc_extent.width < (u32)(pxl + SET_W + 8) || g.sc_extent.height < (u32)(py + rows * BAR_H + 8))) { g_vk.panel_rows = 0; return; }
    const SetItem* cols[2][SET_MAXROWS];
    int ncol[2] = { 0, 0 };
    for (int i = 0; i < n0; i++) cols[0][ncol[0]++] = &col0[i];
    for (int i = 0; i < n1; i++) { int c = two ? 1 : 0; cols[c][ncol[c]++] = &col1[i]; }
    u64 sig = (bgra ? 0x9E3779B97F4A7C15ull : 1) ^ (two ? 0x51ull : 0x17ull);
    for (int r = 0; r < SET_ROWS; r++) for (const char* c = v[r]; *c; c++) sig = (sig ^ (u8)*c) * 0x100000001B3ull;
    static u64 pan_sig[NSUB];
    if (pan_sig[sub] != sig) {
        pan_sig[sub] = sig;
        for (int c = 0; c < (two ? 2 : 1); c++) {
            u8* cp = pp + (size_t)c * (SET_W + SET_GAP) * 4;
            bar_fill(cp, SET_W, (u32)rows * BAR_H, 18, true);
            for (int r = 0; r < ncol[c]; r++) {
                const SetItem* it = cols[c][r];
                u8* rp = cp + (size_t)r * BAR_H * BAR_MAXW * 4;
                Bar b = { rp, SET_W, bgra, 10 };
                if (it->id < 0) {   // (a group's title)
                    for (u32 y = 1; y + 1 < BAR_H; y++) for (u32 x = 1; x + 1 < SET_W; x++) { u8* p = rp + ((size_t)y * BAR_MAXW + x) * 4; p[0] = p[1] = p[2] = 34; }
                    bar_text(&b, it->name, 255, 200, 90);
                    continue;
                }
                bar_text(&b, it->name, 170, 170, 170);
                b.x = 180;
                bar_text(&b, v[it->id], 255, 255, 255);
            }
        }
    }
    for (int c = 0; c < (two ? 2 : 1); c++)
        bar_copy(cb, g_set_buf[sub], (size_t)(BAR_H + 8) * BAR_MAXW * 4 + (size_t)c * (SET_W + SET_GAP) * 4, target, pxl + c * (SET_W + SET_GAP), py, SET_W, (u32)rows * BAR_H);
    // (one row's pixels start BAR_H*BAR_MAXW*4 bytes after the previous: the copies above read rows in order)
    for (int c = 0; c < 2; c++) for (int r = 0; r < SET_MAXROWS; r++) g_vk.panel_ids[c][r] = r < ncol[c] && c < (two ? 2 : 1) ? cols[c][r]->id : -1;
    g_vk.panel_x = pxl; g_vk.panel_y = py; g_vk.panel_w = SET_W; g_vk.panel_row_h = BAR_H;
    g_vk.panel_cols = two ? 2 : 1; g_vk.panel_col_w = SET_W + SET_GAP; g_vk.panel_rows = rows;
    {   // SPLAT_SETTINGS_TEST=1: the panel's pixels once, as images\settings_panel.png, and where it is
        static int test = -1;
        if (test < 0) test = getenv("SPLAT_SETTINGS_TEST") != NULL;
        if (test == 1) {
            test = 2;
            u32 ph = (u32)rows * BAR_H, pw = two ? 2 * SET_W + SET_GAP : SET_W;
            u8* packed = (u8*)malloc((size_t)pw * ph * 4);
            if (packed) {
                for (u32 y = 0; y < ph; y++) memcpy(packed + (size_t)y * pw * 4, pp + (size_t)y * BAR_MAXW * 4, (size_t)pw * 4);
                char path[80];
                snprintf(path, sizeof(path), "%s/settings_panel.png", images_dir());
                write_png(path, packed, pw, ph);
                free(packed);
            }
            printf("[display] settings box at %d,%d %ux%u; panel at %d,%d %ux%u (%d rows of %d px, %d columns) in a %ux%u window\n", gx, gy, bw, BAR_H, pxl, py, pw, ph,
                   rows, BAR_H, two ? 2 : 1, g.sc_extent.width, g.sc_extent.height);
        }
    }
}

static void draw_fps(VkCommandBuffer cb, VkImage target, int sub) {
    if (g_vk.hide_fps) return;
    if (!g_bar_buf[sub] && !host_buffer(BAR_MAXW * BAR_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_bar_buf[sub], &g_bar_mem[sub], &g_bar_ptr[sub])) {
        g_bar_buf[sub] = VK_NULL_HANDLE;
        return;
    }
    int f = g_vk.fps_x10, ms = f > 0 ? (int)(100000.0 / f + 0.5) : 0;   // (tenths of a millisecond)
    {   // GPU: the share of the time the graphics card spent on the game's frames (its timestamps), every half second
        static u64 t0, g0;
        u64 now = hnow_ns();
        if (!t0) { t0 = now; g0 = g_gpu_ns_all; }
        else if (now - t0 > 500000000ull) {
            int p = g_ts_ok > 0 ? (int)((g_gpu_ns_all - g0) * 100 / (now - t0)) : -1;
            g_vk.gpu_pct = p > 100 ? 100 : p;
            t0 = now; g0 = g_gpu_ns_all;
        }
    }
    // (the numbers change twice a second: the bar is only redrawn then)
    static int last_f[NSUB], last_cpu[NSUB], last_w[NSUB], last_gpu[NSUB];
    static u64 last_frame[NSUB];
    if (last_w[sub] && (g.frame < last_frame[sub] + 15 || (last_f[sub] == f && last_cpu[sub] == g_vk.cpu_pct && last_gpu[sub] == g_vk.gpu_pct))) {
        if (g.sc_extent.width < (u32)last_w[sub] + 16 || g.sc_extent.height < BAR_H + 16) return;
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.bufferRowLength = BAR_MAXW;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageOffset.x = 8;
        c.imageOffset.y = 8;
        c.imageExtent.width = (u32)last_w[sub];
        c.imageExtent.height = BAR_H;
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, g_bar_buf[sub], target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        return;
    }
    last_frame[sub] = g.frame;
    char fps[16], cpu[16], gpu[16], frame[24], draws[16];
    snprintf(fps, sizeof(fps), "%d.%d", f / 10, f % 10);
    snprintf(cpu, sizeof(cpu), "%d%%", g_vk.cpu_pct);
    if (g_vk.gpu_pct >= 0) snprintf(gpu, sizeof(gpu), "%d%%", g_vk.gpu_pct); else snprintf(gpu, sizeof(gpu), "-");
    snprintf(frame, sizeof(frame), "%d.%d ms", ms / 10, ms % 10);
    snprintf(draws, sizeof(draws), "%u", g_vk.draws_last_frame);
    const char* sep = "   |   ";
    int tw = text_width("FPS ") + text_width(fps) + text_width(sep) + text_width("CPU ") + text_width(cpu) + text_width(sep) + text_width("GPU ") + text_width(gpu) + text_width(sep) +
             text_width("FRAME ") + text_width(frame) + text_width(sep) + text_width("DRAWS ") + text_width(draws);
    u32 w = (u32)tw + 20;
    if (w > BAR_MAXW) w = BAR_MAXW;
    if (g.sc_extent.width < w + 16 || g.sc_extent.height < BAR_H + 16) return;
    // (drawn in ordinary memory, then copied: the text's blending reads the pixels back, and reading
    // the upload buffer -- write-combined memory -- one byte at a time was ~2% of the drawing thread)
    static u8 bar_px[BAR_MAXW * BAR_H * 4];
    Bar b = { bar_px, w, g.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g.sc_format == VK_FORMAT_B8G8R8A8_SRGB, 10 };
    for (u32 y = 0; y < BAR_H; y++)
        for (u32 x = 0; x < w; x++) {
            u8* p = b.px + (y * BAR_MAXW + x) * 4;
            bool edge = y == 0 || y == BAR_H - 1 || x == 0 || x == w - 1;
            p[0] = p[1] = p[2] = edge ? 60 : 18;
            p[3] = 255;
        }
    // FPS value: green when smooth, yellow when so-so, red when slow
    u8 fr = f >= 550 ? 90 : f >= 300 ? 255 : 255, fg = f >= 550 ? 230 : f >= 300 ? 210 : 80, fb = f >= 550 ? 120 : 60;
    bar_text(&b, "FPS ", 170, 170, 170);
    bar_text(&b, fps, fr, fg, fb);
    bar_text(&b, sep, 90, 90, 90);
    bar_text(&b, "CPU ", 170, 170, 170);
    bar_text(&b, cpu, 255, 255, 255);
    bar_text(&b, sep, 90, 90, 90);
    bar_text(&b, "GPU ", 170, 170, 170);
    bar_text(&b, gpu, 255, 255, 255);
    bar_text(&b, sep, 90, 90, 90);
    bar_text(&b, "FRAME ", 170, 170, 170);
    bar_text(&b, frame, 255, 255, 255);
    bar_text(&b, sep, 90, 90, 90);
    bar_text(&b, "DRAWS ", 170, 170, 170);
    bar_text(&b, draws, 255, 255, 255);
    for (u32 y = 0; y < BAR_H; y++) memcpy((u8*)g_bar_ptr[sub] + y * BAR_MAXW * 4, bar_px + y * BAR_MAXW * 4, w * 4);
    last_f[sub] = f;
    last_cpu[sub] = g_vk.cpu_pct;
    last_gpu[sub] = g_vk.gpu_pct;
    last_w[sub] = (int)w;
    g_vk.fps_bar_w = (int)w + 8;
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferRowLength = BAR_MAXW;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = 8;
    c.imageOffset.y = 8;
    c.imageExtent.width = w;
    c.imageExtent.height = BAR_H;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g_bar_buf[sub], target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// Debug frame: copy every color buffer into a readable picture.
typedef struct { Img img; VkBuffer buf; VkDeviceMemory mem; void* ptr; u32 addr, fmt, layers; } RtDump;
static RtDump g_rtd[48];
static u32 g_nrtd = 0;

static void record_rt_dumps(VkCommandBuffer cb) {
    g_nrtd = 0;
    for (Surf* s = g.rts; s && g_nrtd < 48; s = s->next) {
        if (s->depth || s->w * s->h > 1920 * 1080) continue;
        RtDump* r = &g_rtd[g_nrtd];
        memset(r, 0, sizeof(*r));
        // (small layered buffers -- the light probes' cube faces -- all layers, one under the other)
        u32 nl = s->layers > 1 && s->w * s->h <= 64 * 64 ? (s->layers < 12 ? s->layers : 12) : 1;
        if (!make_image(&r->img, VK_FORMAT_R8G8B8A8_UNORM, s->w, s->h, nl, VK_IMAGE_TYPE_2D, false, false, false)) continue;
        if (!host_buffer((VkDeviceSize)s->w * s->h * 4 * nl, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &r->buf, &r->mem, &r->ptr)) continue;
        cb = transfer_cmd();
        for (u32 l = 0; l < nl; l++) copy_image_layer(cb, &s->img, l, &r->img, l, s->w, s->h);
        r->layers = nl;
        barrier_all(cb);
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = nl;
        c.imageExtent.width = s->w;
        c.imageExtent.height = s->h;
        c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, r->img.image, VK_IMAGE_LAYOUT_GENERAL, r->buf, 1, &c);
        r->addr = s->addr;
        r->fmt = s->gx2fmt;
        g_nrtd++;
    }
}

static void write_rt_dumps(void) {
    for (u32 i = 0; i < g_nrtd; i++) {
        RtDump* r = &g_rtd[i];
        // force alpha opaque so the picture is visible
        u8* px = (u8*)r->ptr;
        u32 nl = r->layers ? r->layers : 1;
        for (u32 k = 0; k < r->img.w * r->img.h * nl; k++) px[k * 4 + 3] = 255;
        char path[96];
        snprintf(path, sizeof(path), "%s/debug_f%llu_%08X_%ux%u_%03X%s.png", images_dir(), (unsigned long long)g.frame, r->addr, r->img.w, r->img.h, r->fmt, nl > 1 ? "_layers" : "");
        write_png(path, px, r->img.w, r->img.h * nl);
    }
}

// Automatic debug pictures/captures (a few fixed frames): off unless asked.
static bool debug_auto(void) {
    static int on = -1;
    if (on < 0) {
        FILE* f = fopen("debug_on.txt", "rb");
        on = getenv("SPLAT_DEBUG") != NULL || f != NULL;
        if (f) fclose(f);
    }
    return on != 0;
}
static volatile u64 g_pcache_saving;
static int pcache_save_thread(void* arg) {
    (void)arg;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_vk.device, g.pcache, &n, NULL) == VK_SUCCESS && n) {
        void* data = malloc(n);
        if (data && vkGetPipelineCacheData(g_vk.device, g.pcache, &n, data) == VK_SUCCESS) {
            FILE* f = fopen(CACHE("pipeline_cache.tmp"), "wb");
            if (f) {
                bool ok = fwrite(data, 1, n, f) == n;
                fclose(f);
                if (ok) { remove(CACHE("pipeline_cache.bin")); rename(CACHE("pipeline_cache.tmp"), CACHE("pipeline_cache.bin")); }
            }
        }
        free(data);
    }
    hatomic_store64(&g_pcache_saving, 0);
    return 0;
}

// The graphics card gave up on our work (a GPU crash, "device lost"): close
// the game at once. Carrying on keeps the card (and the whole screen) stuck
// while Windows resets the driver.
static void gpu_lost_check(VkResult r) {
    if (r != VK_ERROR_DEVICE_LOST) return;
    printf("[render] the graphics card stopped responding (device lost): closing the game\n");
    if (g_vk.device_fault && vkGetDeviceFaultInfoEXT) {   // (what the card tripped on: VK_EXT_device_fault)
        VkDeviceFaultCountsEXT cnt = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        if (vkGetDeviceFaultInfoEXT(g_vk.device, &cnt, NULL) == VK_SUCCESS || cnt.addressInfoCount || cnt.vendorInfoCount) {
            VkDeviceFaultAddressInfoEXT* ai = (VkDeviceFaultAddressInfoEXT*)calloc(cnt.addressInfoCount ? cnt.addressInfoCount : 1, sizeof(*ai));
            VkDeviceFaultVendorInfoEXT* vi = (VkDeviceFaultVendorInfoEXT*)calloc(cnt.vendorInfoCount ? cnt.vendorInfoCount : 1, sizeof(*vi));
            VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
            info.pAddressInfos = ai;
            info.pVendorInfos = vi;
            cnt.vendorBinarySize = 0;
            VkResult fr = vkGetDeviceFaultInfoEXT(g_vk.device, &cnt, &info);
            printf("[render] device fault: %s (%d) -- %u addresses, %u vendor notes\n", info.description, (int)fr, cnt.addressInfoCount, cnt.vendorInfoCount);
            static const char* kinds[] = { "none", "invalid read", "invalid write", "invalid execute", "unknown instruction pointer", "invalid instruction pointer", "instruction pointer fault" };
            for (u32 i = 0; i < cnt.addressInfoCount; i++)
                printf("[render]   %s at %016llX (precision %llu bytes)\n", ai[i].addressType < 7 ? kinds[ai[i].addressType] : "?", (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
            for (u32 i = 0; i < cnt.vendorInfoCount; i++)
                printf("[render]   vendor: %s (code %llu, data %llu)\n", vi[i].description, (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
            free(ai); free(vi);
        }
    }
    fprintf(stderr, "[render] the graphics card stopped responding (device lost): closing the game\n");
    fflush(stdout);
    fflush(stderr);
    _Exit(3);
}

static u32 g_present_swap;   // (the swap being presented: see the [pace] line)
#ifdef _WIN32
// --- Frame spacing (the [pace] line) ------------------------------------------------------------
// How evenly frames reach the screen: the screen refreshes between one present and the next (the
// same number every time = even; at 60 fps that's 2 on a 120 Hz screen, 3 at 180 Hz) and how many
// refreshes after the one the game was paced for (gx2.c GX2WaitForVsync) each present went out.
// Busy plaza at 180 Hz: ~85-88% of frames 3 refreshes apart, the rest 2 or 4. Tried and not kept
// (no change in these numbers): holding early frames back to a common offset -- on this thread
// (delayed the next frame, worse, -1 fps) and on a presenter thread of its own -- and pacing the
// game's targets on a strict refresh grid (gx2.c). These are present-call times, not what the
// screen shows (no present feedback in this driver): judge pacing changes by eye too.
// The adaptive lead: gx2.c's GX2WaitForVsync wakes the game `work + 4.5 ms + g_pace_extra_ns` before the
// refresh a frame is paced for. The 4.5 ms covered the drawing thread's tail after the game's flush; while a
// level streams in (new objects: uploads, texture work, pipelines) that tail grows to 7-9 ms and frames came out
// after their refresh (the Oct 31 all-maps bench: 1-8 late frames a map, "waiting"). Now a present that goes out
// less than 2.5 ms before its refresh adds the shortfall to the lead at once; the extra decays slowly (1/128 a
// frame) while presents are comfortably early. Costs input lag only in such stretches. SPLAT_FIXED_LEAD=1: off.
volatile s64 g_pace_extra_ns;
static void pace_feedback(double margin_ms) {   // (margin: ms the present went out before the paced-for refresh)
    static int fixed = -1;
    if (fixed < 0) fixed = getenv("SPLAT_FIXED_LEAD") != NULL;
    if (fixed) return;
    s64 extra = g_pace_extra_ns;
    if (margin_ms < 2.5) extra += (s64)((2.5 - margin_ms) * 1e6);
    else if (margin_ms > 5.0) extra -= extra / 128 + 5000;
    if (extra < 0) extra = 0;
    if (extra > 8000000) extra = 8000000;
    g_pace_extra_ns = extra;
}
static void present_pace(void) {
    static HRESULT(WINAPI * dwm_timing)(HWND, DWM_TIMING_INFO*) = NULL;
    static int tried = 0;
    static u64 cad[8], late[8], prev_idx, freq;
    static s64 ref_vb, ref_per;   // (a refresh time and the period, taken every 600 frames: the compositor's
                                  // refresh counter doesn't count every refresh of a 180 Hz screen here)
    static u8 hist[120];
    static u32 hist_n, hist_pos, K;
    static double per_ms;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("dwmapi.dll");
        if (m) dwm_timing = (HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*))GetProcAddress(m, "DwmGetCompositionTimingInfo");
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (u64)f.QuadPart;
    }
    if (!dwm_timing) return;
    extern u64 gx2_swap_target_qpc(u32 swap);
    DWM_TIMING_INFO ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = sizeof(ti);
    if (dwm_timing(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod) return;
    s64 per = (s64)ti.qpcRefreshPeriod;
    per_ms = per * 1000.0 / (double)freq;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    // (refresh numbers: the compositor's count, + the refreshes since that one)
    if (!ref_per) { ref_vb = (s64)ti.qpcVBlank - per * 1000000; ref_per = per; }   // (far enough back: indexes stay positive)
    // (a present: the refresh it goes out at -- the next one; a target, which lies exactly on a refresh: the
    // nearest one. Counting both the same way put the targets on a boundary: as the period estimate drifted
    // they flipped between two numbers -- "2" and "4" in pairs that the screen never showed)
    #define REFRESH_OF(t) (((s64)(t) - ref_vb) / ref_per + 1)
    #define TARGET_REFRESH(t) (((s64)(t) + ref_per / 2 - ref_vb) / ref_per)
    s64 idx = REFRESH_OF(q.QuadPart);
    u64 target = gx2_swap_target_qpc(g_present_swap);
    static u32 offs[24];   // (where presents go out against the refresh the frame was paced for: 1 ms bins from -12 ms)
    if (target) {
        s64 o = ((s64)q.QuadPart - (s64)target) * 1000 / (s64)freq + 12;
        offs[o < 0 ? 0 : o > 23 ? 23 : o]++;
        pace_feedback(((double)(s64)target - (double)q.QuadPart) * 1000.0 / (double)freq);
    }
    bool cad_done = false;
    if (target && g_vk.low_latency) {
        // (how the screen shows it, from differences only: the refreshes between this frame's target and the
        // last one's, plus how many refreshes each present came after its own target -- a present before its
        // target goes out at it. Placing presents on an absolute refresh grid drifted with the period estimate
        // and counted "2"/"4" pairs; this is still a model of the screen -- the driver reports no flips)
        static u64 prev_target;
        static s64 prev_late;
        s64 late_r = (s64)q.QuadPart >= (s64)target ? ((s64)q.QuadPart - (s64)target) / ref_per + 1 : 0;
        if (prev_target && target > prev_target) {
            s64 d = ((s64)(target - prev_target) + ref_per / 2) / ref_per + late_r - prev_late;
            cad[d < 1 ? 0 : d < 7 ? d : 7]++;
            tracker_present((int)d, (int)((16.6667 + per_ms / 2) / per_ms));
            cad_done = true;
        }
        prev_target = target;
        prev_late = late_r;
        s64 tidx = TARGET_REFRESH(target);
        s64 L = idx - tidx;
        hist[hist_pos] = (u8)(L < 0 ? 0 : L > 15 ? 15 : L);
        hist_pos = (hist_pos + 1) % 120;
        if (hist_n < 120) hist_n++;
        if (hist_n >= 60 && hist_pos % 30 == 0) {   // (the offset ~95% of the last frames made)
            u32 c[16] = { 0 };
            for (u32 i = 0; i < hist_n; i++) c[hist[i]]++;
            u32 acc = 0, k = 0;
            for (k = 0; k < 16; k++) { acc += c[k]; if (acc * 100 >= hist_n * 95) break; }
            K = k;
        }
        L = idx - tidx;
        late[L < 0 ? 0 : L < 7 ? L : 7]++;
    }
    #undef REFRESH_OF
    #undef TARGET_REFRESH
    if (prev_idx && !cad_done) { s64 d = idx - (s64)prev_idx; cad[d < 1 ? 0 : d < 7 ? d : 7]++; tracker_present((int)d, (int)((16.6667 + per_ms / 2) / per_ms)); }
    prev_idx = (u64)idx;
    u64 tot = 0;
    for (int i = 0; i < 8; i++) tot += cad[i];
    if (tot >= 600) {
        printf("[pace] presents against the paced-for refresh (ms, from -12):");
        for (int i = 0; i < 24; i++) printf(" %d:%u", i - 12, offs[i]);
        printf(" | lead extra now %.1f ms\n", g_pace_extra_ns / 1e6);
        memset(offs, 0, sizeof(offs));
        printf("[pace] screen refreshes (%.2f ms) between frames: 1:%llu 2:%llu 3:%llu 4:%llu 5:%llu 6+:%llu | after the paced-for one: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu | 95%% within %u\n", per_ms,
               (unsigned long long)(cad[0] + cad[1]), (unsigned long long)cad[2], (unsigned long long)cad[3], (unsigned long long)cad[4], (unsigned long long)cad[5], (unsigned long long)(cad[6] + cad[7]),
               (unsigned long long)late[0], (unsigned long long)late[1], (unsigned long long)late[2], (unsigned long long)late[3], (unsigned long long)late[4], (unsigned long long)(late[5] + late[6] + late[7]),
               K);
        memset(cad, 0, sizeof(cad));
        memset(late, 0, sizeof(late));
        ref_per = 0;   // (taken again: no drift between the clock and the screen)
        prev_idx = 0;
    }
}
#endif


// --- Smooth motion (see g_fg) --------------------------------------------------------------------
// The warp: a grid over the last picture; each vertex takes its depth, goes out into the world through the old camera
// and back through the new one (the same picture <-> world mapping the SHADERS pass uses: k_vol_fs). Cells across a
// depth edge stretch over what the move uncovers -- a few pixels at 120+, gone at the next real frame.
static const char* k_fg_vs =
    "#version 450\n"
    "layout(location=0) out vec2 uv;\n"
    "layout(binding=1) uniform sampler2D dep;\n"
    "layout(push_constant) uniform P { vec4 c[8]; } pc;\n"   // c0 P1 tx | c1 fw1 ty | c2 rt1 near | c3 up1 far | c4 P2 gridw | c5 fw2 gridh | c6 rt2 mask | c7 up2 -
    "float linz(float d) { float n = pc.c[2].w, f = pc.c[3].w; float z = d * 2.0 - 1.0; return 2.0 * n * f / (f + n - z * (f - n)); }\n"
    "void main() {\n"
    "  int gw = int(pc.c[4].w), gh = int(pc.c[5].w);\n"
    "  int cell = gl_VertexIndex / 6, k = gl_VertexIndex - cell * 6;\n"
    "  int cx = cell % gw, cy = cell / gw;\n"
    "  int ox = (k == 1 || k == 3 || k == 4) ? 1 : 0, oy = (k == 2 || k == 4 || k == 5) ? 1 : 0;\n"
    "  uv = vec2(float(cx + ox) / float(gw), float(cy + oy) / float(gh));\n"
    "  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "  float d = textureLod(dep, uv, 0.0).r;\n"
    "  float z = (d >= 0.99999) ? pc.c[3].w * 0.5 : linz(d);\n"   // (the sky: far away, turns with the camera, never slides)
    "  float tx = pc.c[0].w, ty = pc.c[1].w;\n"
    "  vec3 w = pc.c[0].xyz + pc.c[1].xyz * z + pc.c[2].xyz * (ndc.x * tx * z) + pc.c[3].xyz * (ndc.y * ty * z);\n"
    "  vec3 v = w - pc.c[4].xyz;\n"
    "  float zc = max(dot(v, pc.c[5].xyz), 0.05);\n"
    "  vec2 n2 = vec2(dot(v, pc.c[6].xyz) / (zc * tx), dot(v, pc.c[7].xyz) / (zc * ty));\n"
    "  gl_Position = vec4(n2.x, -n2.y, 0.5, 1.0);\n"
    "}\n";
static const char* k_fg_fs =
    "#version 450\n"
    "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
    "layout(binding=0) uniform sampler2D pic;\n"
    "layout(binding=2) uniform sampler2D scene;\n"
    "layout(push_constant) uniform P { vec4 c[8]; } pc;\n"
    "void main() {\n"
    "  vec2 here = gl_FragCoord.xy / vec2(textureSize(pic, 0));\n"
    "  vec3 a = textureLod(pic, here, 0.0).rgb;\n"
    "  if (pc.c[6].w > 0.5) {\n"   // (the HUD: where the shown picture differs from the scene under it, it stays put)
    "    vec3 b = textureLod(scene, here, 0.0).rgb;\n"
    "    if (dot(abs(a - b), vec3(1.0)) > 0.02) { o = vec4(a, 1.0); return; }\n"
    "  }\n"
    "  o = vec4(textureLod(pic, uv, 0.0).rgb, 1.0);\n"
    "}\n";
#define FG_GW 160
#define FG_GH 90
static bool fg_init(void) {
    if (g_fg.tried) return g_fg.ok;
    g_fg.tried = true;
    if (!fx_init()) return false;
    VkDescriptorSetLayoutBinding b[3];
    memset(b, 0, sizeof(b));
    for (int i = 0; i < 3; i++) { b[i].binding = (u32)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; }
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 3;
    dl.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.device, &dl, NULL, &g_fg.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
    VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_fg.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_vk.device, &pl, NULL, &g_fg.layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 };
    VkDescriptorPoolCreateInfo pc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pc.maxSets = 2;
    pc.poolSizeCount = 1;
    pc.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(g_vk.device, &pc, NULL, &g_fg.pool) != VK_SUCCESS) return false;
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_fg.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_fg.dsl;
        if (vkAllocateDescriptorSets(g_vk.device, &ai, &g_fg.set[i]) != VK_SUCCESS) return false;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.25f;
    if (vkCreateSampler(g_vk.device, &sci, NULL, &g_fg.nearest) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    if (vkAllocateCommandBuffers(g_vk.device, &cai, g_fg.cb) != VK_SUCCESS) return false;
    for (int i = 0; i < 2; i++) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(g_vk.device, &fi, NULL, &g_fg.fence[i]) != VK_SUCCESS) return false;
    }
    g_fg.ok = true;
    printf("[render] smooth motion ready (%dx: generated pictures between the game's frames)\n", fg_n());
    return true;
}
// (the warp pipeline: the grid's vertex shader, my layout, the picture's format)
static VkPipeline fg_pipeline(VkFormat cf) {
    VkShaderModule vs = fx_module(false, k_fg_vs), fs = fx_module(true, k_fg_fs);
    if (!vs || !fs) { printf("[render] smooth motion: its shaders didn't compile\n"); return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof(ba));
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &cf;
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = g_fg.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.device, g.pcache, 1, &pi, NULL, &pipe);
    vkDestroyShaderModule(g_vk.device, vs, NULL);
    vkDestroyShaderModule(g_vk.device, fs, NULL);
    if (r != VK_SUCCESS) { printf("[render] smooth motion: its pipeline didn't build (%d)\n", (int)r); return VK_NULL_HANDLE; }
    return pipe;
}
static void fg_blit_same(VkCommandBuffer cb, Img* from, Img* to) {   // (same size, colour)
    VkImageBlit bl;
    memset(&bl, 0, sizeof(bl));
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.srcOffsets[1].x = (int)from->w; bl.srcOffsets[1].y = (int)from->h; bl.srcOffsets[1].z = 1;
    bl.dstOffsets[1].x = (int)to->w; bl.dstOffsets[1].y = (int)to->h; bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb, from->image, VK_IMAGE_LAYOUT_GENERAL, to->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
}
// The scene just before its HUD is drawn (from latte_render_draw, the first HUD draw of the frame).
static void fg_scene_copy(Surf* s) {
    if (!fg_n() || g_fg.scene_copied || !s || s->depth) return;
    g_fg.scene_copied = true;
    if (g_fg.scene.image && (g_fg.scene.w != s->img.w || g_fg.scene.h != s->img.h || g_fg.scene.format != s->img.format)) memset(&g_fg.scene, 0, sizeof(g_fg.scene));   // (a new size: the old one is left behind -- once, if ever)
    VkCommandBuffer cb = transfer_cmd();
    if (!g_fg.scene.image) {
        if (!make_image(&g_fg.scene, s->img.format, s->img.w, s->img.h, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
        image_layout_init(cb, &g_fg.scene);
    }
    barrier_all(cb);
    fg_blit_same(cb, &s->img, &g_fg.scene);
    barrier_all(cb);
}
// At a real present: keep the picture, its depth and the camera (recorded into the frame's own command buffer).
static void fg_capture(VkCommandBuffer cb, Img* src) {
    g_fg.scene_copied = false;
    if (!fg_n() || !fg_init() || !src) { g_fg.have = false; return; }
    if (g_fg.pic.image && (g_fg.pic.w != src->w || g_fg.pic.h != src->h || g_fg.pic.format != src->format)) { memset(&g_fg.pic, 0, sizeof(g_fg.pic)); memset(&g_fg.out, 0, sizeof(g_fg.out)); }
    if (!g_fg.pic.image) {
        if (!make_image(&g_fg.pic, src->format, src->w, src->h, 1, VK_IMAGE_TYPE_2D, false, false, false)) return;
        image_layout_init(cb, &g_fg.pic);
    }
    if (!g_fg.out.image) {
        if (!make_image(&g_fg.out, src->format, src->w, src->h, 1, VK_IMAGE_TYPE_2D, false, true, false)) return;
        image_layout_init(cb, &g_fg.out);
    }
    barrier_all(cb);
    fg_blit_same(cb, src, &g_fg.pic);
    // the depth: the TV-size depth buffer drawn this frame, same size as the picture
    g_fg.depth_ok = false;
    Surf* d = g_fg.dsurf;
    if (d && g_fg.dsurf_frame + 1 == g.frame && d->img.w == src->w && d->img.h == src->h) {
        if (g_fg.depth.image && (g_fg.depth.w != d->img.w || g_fg.depth.h != d->img.h || g_fg.depth.format != d->img.format)) memset(&g_fg.depth, 0, sizeof(g_fg.depth));
        if (!g_fg.depth.image) {
            if (make_image(&g_fg.depth, d->img.format, d->img.w, d->img.h, 1, VK_IMAGE_TYPE_2D, false, false, true)) image_layout_init(cb, &g_fg.depth);
        }
        if (g_fg.depth.image) {
            VkImageCopy c;
            memset(&c, 0, sizeof(c));
            c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
            c.extent.width = d->img.w; c.extent.height = d->img.h; c.extent.depth = 1;
            vkCmdCopyImage(cb, d->img.image, VK_IMAGE_LAYOUT_GENERAL, g_fg.depth.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            g_fg.depth_ok = true;
        }
    }
    barrier_all(cb);
    // the camera of the frame just drawn
    int splat_camera_for_frame(u32 frame, float pos[3], float dir[3], float* fovy_deg, float* nr, float* fr);
    g_fg.cam[0] = g_fg.cam[1];
    float cp[3], fw[3], fovy = 0, nr = 1, fr = 10000;
    g_fg.cam[1].ok = splat_camera_for_frame(g_present_swap, cp, fw, &fovy, &nr, &fr) && fovy > 0.0f;
    if (g_fg.cam[1].ok) { memcpy(g_fg.cam[1].pos, cp, 12); memcpy(g_fg.cam[1].dir, fw, 12); g_fg.cam[1].fovy = fovy; g_fg.cam[1].nr = nr; g_fg.cam[1].fr = fr; }
    g_fg.made = 0;
    g_fg.have = true;
}
static void fg_cam_frame(const float fw[3], float rt[3], float up[3]) {   // (as the SHADERS pass: right is level, up follows)
    float rx = -fw[2], rz = fw[0], rl = sqrtf(rx * rx + rz * rz);
    if (rl < 1e-4f) { rx = 1.0f; rz = 0.0f; rl = 1.0f; }
    rt[0] = rx / rl; rt[1] = 0.0f; rt[2] = rz / rl;
    up[0] = rt[1] * fw[2] - rt[2] * fw[1]; up[1] = rt[2] * fw[0] - rt[0] * fw[2]; up[2] = rt[0] * fw[1] - rt[1] * fw[0];
}
// One generated picture: the last real one, re-projected to where the camera should be now.
static void fg_present(void) {
    int n = fg_n();
    if (!g_fg.ok || !g_fg.have || !g_fg.pic.image || !g_fg.out.image) return;
    int w = g_fg.which;
    if (g_fg.busy[w]) {
        if (vkWaitForFences(g_vk.device, 1, &g_fg.fence[w], VK_TRUE, 0) != VK_SUCCESS) { g_fg.skipped++; return; }   // (the one before is still on the card)
        g_fg.busy[w] = false;
    }
    u32 index = 0;
    if (!acq_ready(0, 2000000ull, &index)) { g_fg.skipped++; return; }
    if (!g_fg.pipe || g_fg.pipe_fmt != g_fg.out.format) { g_fg.pipe = fg_pipeline(g_fg.out.format); g_fg.pipe_fmt = g_fg.out.format; if (!g_fg.pipe) return; }
    vkResetFences(g_vk.device, 1, &g_fg.fence[w]);
    VkCommandBuffer cb = g_fg.cb[w];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    barrier_all(cb);
    // the camera now: the last two frames' motion carried on by made/n of a frame (a cut, a scene change, a menu: no motion)
    float t = (float)g_fg.made / (float)n;
    float p1[3] = { 0, 0, 0 }, d1[3] = { 0, 0, 1 };
    if (g_fg.cam[1].ok) { memcpy(p1, g_fg.cam[1].pos, 12); memcpy(d1, g_fg.cam[1].dir, 12); }
    float p2[3], d2[3];
    bool move = g_fg.cam[0].ok && g_fg.cam[1].ok && g_fg.depth_ok;
    if (move) {
        float dp[3] = { p1[0] - g_fg.cam[0].pos[0], p1[1] - g_fg.cam[0].pos[1], p1[2] - g_fg.cam[0].pos[2] };
        float dd = dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2];
        float cosang = d1[0] * g_fg.cam[0].dir[0] + d1[1] * g_fg.cam[0].dir[1] + d1[2] * g_fg.cam[0].dir[2];
        if (dd > 150.0f * 150.0f || cosang < 0.966f || fabsf(g_fg.cam[1].fovy - g_fg.cam[0].fovy) > 0.5f) move = false;   // (a jump: 15 degrees or 150 units in a frame)
        else {
            for (int i = 0; i < 3; i++) { p2[i] = p1[i] + dp[i] * t; d2[i] = d1[i] + (d1[i] - g_fg.cam[0].dir[i]) * t; }
            float l = sqrtf(d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2]);
            if (l < 1e-4f) move = false; else for (int i = 0; i < 3; i++) d2[i] /= l;
        }
    }
    if (!move) { memcpy(p2, p1, 12); memcpy(d2, d1, 12); }
    float rt1[3], up1[3], rt2[3], up2[3];
    fg_cam_frame(d1, rt1, up1);
    fg_cam_frame(d2, rt2, up2);
    float ty = tanf((g_fg.cam[1].ok ? g_fg.cam[1].fovy : 60.0f) * 0.5f * 3.14159265f / 180.0f), tx = ty * (float)g_fg.pic.w / (float)g_fg.pic.h;
    float nr = g_fg.cam[1].ok ? g_fg.cam[1].nr : 1.0f, fr = g_fg.cam[1].ok ? g_fg.cam[1].fr : 10000.0f;
    float pcv[32] = {
        p1[0], p1[1], p1[2], tx,
        d1[0], d1[1], d1[2], ty,
        rt1[0], rt1[1], rt1[2], nr,
        up1[0], up1[1], up1[2], fr,
        p2[0], p2[1], p2[2], (float)FG_GW,
        d2[0], d2[1], d2[2], (float)FG_GH,
        rt2[0], rt2[1], rt2[2], g_fg.scene_copied || g_fg.scene.image ? 1.0f : 0.0f,
        up2[0], up2[1], up2[2], 0.0f };
    if (!g_fg.scene.image) pcv[27] = 0.0f;
    // the textures
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet wr[3];
    memset(ii, 0, sizeof(ii)); memset(wr, 0, sizeof(wr));
    u32 sel = 0 | (1 << 3) | (2 << 6) | (3 << 9);
    ii[0].sampler = g_fx.sampler; ii[0].imageView = sample_view(&g_fg.pic, VK_IMAGE_VIEW_TYPE_2D, g_fg.pic.format, sel); ii[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[1].sampler = g_fg.nearest; ii[1].imageView = g_fg.depth.image ? sample_view(&g_fg.depth, VK_IMAGE_VIEW_TYPE_2D, g_fg.depth.format, sel) : ii[0].imageView; ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ii[2].sampler = g_fg.nearest; ii[2].imageView = g_fg.scene.image ? sample_view(&g_fg.scene, VK_IMAGE_VIEW_TYPE_2D, g_fg.scene.format, sel) : ii[0].imageView; ii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (int i = 0; i < 3; i++) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = g_fg.set[w]; wr[i].dstBinding = (u32)i; wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &ii[i]; }
    vkUpdateDescriptorSets(g_vk.device, 3, wr, 0, NULL);
    // the warp into g_fg.out
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = att_view(&g_fg.out, 0);
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea.extent.width = g_fg.out.w;
    ri.renderArea.extent.height = g_fg.out.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)g_fg.out.w, (float)g_fg.out.h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { g_fg.out.w, g_fg.out.h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fg.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_fg.layout, 0, 1, &g_fg.set[w], 0, NULL);
    vkCmdPushConstants(cb, g_fg.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, pcv);
    vkCmdDraw(cb, FG_GW * FG_GH * 6, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier_all(cb);
    // into the window, as a real frame goes (the same fit, the same overlays)
    VkImage im = g.sc_images[index];
    sc_barrier(cb, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black;
    memset(&black, 0, sizeof(black));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    {
        float sx = (float)g.sc_extent.width / g_fg.out.w, sy = (float)g.sc_extent.height / g_fg.out.h;
        float s = sx < sy ? sx : sy;
        int dw = (int)(g_fg.out.w * s), dh = (int)(g_fg.out.h * s);
        int ox = ((int)g.sc_extent.width - dw) / 2, oy = ((int)g.sc_extent.height - dh) / 2;
        VkImageBlit bl;
        memset(&bl, 0, sizeof(bl));
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.srcOffsets[1].x = (int)g_fg.out.w; bl.srcOffsets[1].y = (int)g_fg.out.h; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[0].x = ox; bl.dstOffsets[0].y = oy;
        bl.dstOffsets[1].x = ox + dw; bl.dstOffsets[1].y = oy + dh; bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, g_fg.out.image, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        draw_drc(cb, im);
        draw_fps(cb, im, w);
        draw_settings(cb, im, w);
    }
    sc_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_acq.last;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.sc_done[index];
    VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, g_fg.fence[w]);
    gpu_lost_check(sr);
    g_fg.busy[w] = sr == VK_SUCCESS;
    if (sr != VK_SUCCESS) return;
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.sc_done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    VkResult pr = vkQueuePresentKHR(g_vk.queue, &pi);
    gpu_lost_check(pr);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
    g_fg.shown++;
    g_fg.which ^= 1;
}
// Between the game's frames (latte_render_poll / latte_render_idle, the drawing thread): a generated picture when its time
// has come -- 1/n of a frame after the real one, then 2/n...; none once the game is late with its next frame (40 ms).
static void fg_tick(void) {
    int n = fg_n();
    if (!n || !g_fg.have || !g.swapchain || g.sc_rebuild || !g_vk.surface) return;
    if (g_fg.made >= n - 1) return;
    u64 now = hnow_ns();
    if (now > g_fg.real_ns + 40000000ull) { g_fg.made = n - 1; return; }
    u64 due = g_fg.real_ns + (u64)(g_fg.made + 1) * (16666667ull / (u64)n);
    if (now < due) return;
    g_fg.made++;
    fg_present();
}
static u64 g_sw_t0, g_sw_submit, g_sw_pace, g_sw_present, g_sw_report;   // (a slow swap packet: where its time went -- the Oct 31 "pacing / system" late frames)
void latte_render_swap(u32 swap_count) {
    g_build_wait_used = 0;
    g_present_swap = swap_count;
    g_sw_t0 = hnow_ns();
    g_sw_submit = g_sw_pace = g_sw_present = g_sw_report = 0;
    if (!ready()) return;
    if (g_pf_all) {
        u64 used = g_pf_used, kept = g_pf_kept;
        pf_sweep();
        static int logged = 0;
        if ((g_pf_used != used || g_pf_kept != kept) && logged++ < 40)
            printf("[render] look-ahead pipelines: %llu taken by draws, %llu kept for later, %u still building (all so far: %llu made)\n",
                   (unsigned long long)g_pf_used, (unsigned long long)g_pf_kept, g_pf_n, (unsigned long long)g_pf_made);
    }
    end_pass();
    VkCommandBuffer cb = cmd();
    if (g.need_barrier) { barrier_all(cb); g.need_barrier = false; }
    Sub* s = &g.sub[g.cur];
    g.frame++;
    g_vk.draws_last_frame = g.draws;
    g.draws = 0;

    {   // SPLAT_SHOT_EVERY=N: a screenshot (as F12) every N frames, for unattended test runs
        static int every = -1;
        if (every < 0) { const char* e = getenv("SPLAT_SHOT_EVERY"); every = e ? atoi(e) : 0; }
        if (every > 0 && g.frame % (u64)every == 0 && !g_vk.screenshot) g_vk.screenshot = 1;
    }
    bool f12 = g_vk.screenshot == 2;   // (Ctrl+F12: a capture too; F12 alone is just the screenshot)
    fx_pass(cb, (int)(s - g.sub));
    {   // debug views (F7 / F8): the frame's numbers for the line, the overdraw picture
        static int env = -1;
        if (env < 0) {
            env = getenv("SPLAT_DEBUG_VIEWS") != NULL;
            const char* v = getenv("SPLAT_DEBUG_VIEW");   // (start in view n: test runs, frame_replay)
            if (env || v) g_vk.debug_views_on = 1;
            if (v) g_vk.debug_view = atoi(v) % 6;
        }
        if (!g_vk.debug_views_on) { g_vk.debug_view = 0; g_vk.debug_pick = 0; g_vk.pick_req = 0; }
        u64 built = hatomic_load64(&g_async_built) + g_now_builds;
        g_dv_last.shaders = g_dv_pairs;
        g_dv_last.scene = g_dv_scene;
        g_dv_last.nan = g_dv_nan;
        g_dv_last.skipped = g_skipped_draws >= g_dv_skips_at ? g_skipped_draws - g_dv_skips_at : g_skipped_draws;   // (the counters
        g_dv_last.built = built >= g_dv_built_at ? (u32)(built - g_dv_built_at) : 0;                                 // start over every 300 frames)
        g_dv_pairs = g_dv_scene = g_dv_nan = 0;
        g_dv_skips_at = g_skipped_draws;
        g_dv_built_at = built;
        dv_present_pass(cb, (int)(s - g.sub));
    }
    bool pick_read = g_pick_frame, pick_next = false;   // (this frame's 3D draws were counted for a click / start that now)
    g_pick_frame = false;
    if (g_vk.pick_req && g_vk.debug_views_on) { g_vk.pick_req = 0; end_pass(); pick_next = pick_start(cb); }
    bool dump = g.tv_valid && (g.frame <= g_render_dump_frames || g_vk.screenshot || (g_dbg && !g_dbg_nodump));
    if (dump) { g_vk.screenshot = 0; record_readback(cb, s); }
    bool dbg_dump = g_dbg && !g_dbg_nodump;
    if (dbg_dump) { record_rt_dumps(cb); cb = cmd(); }

    // present (unless this frame left draws out: see g_frame_skipped)
    bool presenting = false;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    bool resizing = false;
    u32 index = 0;
    if (g_guess_n) guess_check();
    if (g_od_n && once_dbg()) {   // (buffers noted two frames back: drawn again in the frame just done?)
        u32 k = 0;
        double again = 0, once = 0;
        for (u32 j = 0; j < g_od_n; j++) {
            OnceDbg* o = &g_od[j];
            if (o->frame + 2 > g.frame) { g_od[k++] = *o; continue; }
            bool drawn = o->s->seen == g.frame;
            if (drawn) again += o->ns / 1e6; else once += o->ns / 1e6;
            printf("[render] made-once builds (frame %llu): %ux%u x%u fmt %03X%s, %u builds %.0f ms -- drawn again next frame: %s\n", (unsigned long long)o->frame,
                   o->s->w, o->s->h, o->s->layers, o->s->gx2fmt, o->s->depth ? " depth" : "", o->n, o->ns / 1e6, drawn ? "yes" : "no");
        }
        if (k != g_od_n) printf("[render] made-once builds: %.0f ms for buffers drawn again next frame, %.0f ms for ones that weren't\n", again, once);
        g_od_n = k;
    }
    // (never_stop: a frame missing only a few draws -- a new weapon in the gear
    // menu, a new effect -- is shown as it is: holding the picture 8 frames
    // for 2 draws out of 500 was a 130 ms stop at every weapon switch; the
    // draws just turn up once built. A frame missing much -- a new scene's
    // first frames -- or a full-screen pass is held, as before.)
    u64 frame_draws = g_vk.draws_last_frame;   // (g.draws was set back to 0 for the next frame already)
    // (settings "New shaders: wait (no flicker)": a frame missing even a few draws -- a new ink effect's first
    // splat in a match -- isn't shown either, for at most 3 frames in a row (50 ms): the object no longer
    // blinks in and out while its shader is built. "skip (no hitch)" shows those frames, as before.
    // SPLAT_FLICKER_OLD=1: the old rule in both.)
    static int flicker_old = -1;
    if (flicker_old < 0) flicker_old = getenv("SPLAT_FLICKER_OLD") != NULL;
    bool few = !g_frame_must_hold && g_frame_skip_n <= 16 && (u64)g_frame_skip_n * 50 <= frame_draws;
    bool hold_few = few && g_vk.build_wait && !flicker_old;
    bool want_hold = g_frame_skipped && !g_vk.gallery && (!never_stop() || !few || hold_few);
    bool hold = want_hold && g_held_in_row < (hold_few ? 3u : HOLD_MAX) && !g_hold_spent;
    if (hold && hold_few) g_held_few++;
    if (hold && never_stop() && g_held_in_row == 0) {
        static int shown = 0;
        if (shown++ < 40) printf("[render] picture held (frame %llu): %u of %llu draws left out while their pipelines are built%s\n", (unsigned long long)g.frame,
                                 g_frame_skip_n, (unsigned long long)frame_draws, g_frame_must_hold ? ", a full-screen pass among them" : "");
    }
    if (never_stop()) { if (want_hold && !hold) g_hold_spent = true; if (!g_frame_skipped) g_hold_spent = false; }
    g_frame_skipped = false;
    g_frame_skip_n = 0;
    g_frame_must_hold = false;
    if (hold) { g_held_in_row++; g_held_frames++; }
    else g_held_in_row = 0;
    if (g_vk.surface && !hold) {
        {   // A window being dragged to a new size sends a size change every frame; each one meant a new
            // swapchain (a device-wide wait, ~100 ms). Now the picture is rebuilt once the size has held for 0.2 s;
            // meanwhile nothing is presented (the window keeps its last picture).
            static u64 resize_at = 0;
            static bool resize_pending = false;
            if (g_vk.resized) { g_vk.resized = 0; resize_at = hnow_ns(); resize_pending = true; }
            if (resize_pending) {
                if (hnow_ns() - resize_at < 200000000ull) { resizing = true; }
                else { resize_pending = false; g.sc_rebuild = true; }
            }
        }
        acquire_sem = s->acquire;
        {   // (the settings panel changed vsync: a new swapchain)
            static int built_with = -1, built_ll = -1;
            if (built_with < 0) { built_with = g_vk.vsync_mode; built_ll = g_vk.low_latency; }
            if (built_with != g_vk.vsync_mode || built_ll != g_vk.low_latency) { built_with = g_vk.vsync_mode; built_ll = g_vk.low_latency; g.sc_rebuild = true; }
        }
        if (!resizing && (!g.swapchain || g.sc_rebuild) && !build_swapchain()) g.sc_rebuild = true;
        if (!resizing && g.swapchain && !g.sc_rebuild) {
            // No waiting for the monitor: the game paces itself at 60 by its own
            // clock, which drifts against the screen's refresh; when the screen
            // still had frames queued, waiting for a free image here stalled
            // this thread up to a refresh (the game then waited ~20 ms in
            // GX2DrawDone: 50 ms frames) -- or 100 ms at a time while the window
            // was covered. Now that frame just isn't shown (the next one is).
            // SPLAT_PRESENT_WAIT=1: wait, as before.
            static int wait_present = -1;
            if (wait_present < 0) wait_present = getenv("SPLAT_PRESENT_WAIT") != NULL;
            bool low_lat = false;   // (see build_swapchain)
            if (low_lat && g_pre.have) {   // (acquired right after the last present: see below)
                index = g_pre.index;
                acquire_sem = g_pre.sem[g_pre.which];
                g_pre.have = false;
                presenting = true;
            } else {
                static int old_acq = -1;
                if (old_acq < 0) old_acq = getenv("SPLAT_OLD_ACQUIRE") != NULL;
                if (old_acq) {
                    VkResult r = vkAcquireNextImageKHR(g_vk.device, g.swapchain, (wait_present || low_lat) ? 100000000ull : 0, s->acquire, VK_NULL_HANDLE, &index);
                    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) presenting = true;
                    else if (r == VK_TIMEOUT || r == VK_NOT_READY) g_present_skips++;
                    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
                } else if (acq_ready((wait_present || low_lat) ? 100000000ull : 0, acq_fence_ns(), &index)) {
                    acquire_sem = g_acq.last;
                    presenting = true;
                }
            }
        }
    }
    if (presenting) {
        VkImage im = g.sc_images[index];
        sc_barrier(cb, im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black;
        memset(&black, 0, sizeof(black));
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        if (g_vk.gallery) {
            draw_gallery(cb, im);
        } else if (g.tv_valid) {
            // fit the TV picture into the window, keeping its shape
            float sx = (float)g.sc_extent.width / g.tv.w, sy = (float)g.sc_extent.height / g.tv.h;
            float sc = sx < sy ? sx : sy;
            int dw = (int)(g.tv.w * sc), dh = (int)(g.tv.h * sc);
            int ox = ((int)g.sc_extent.width - dw) / 2, oy = ((int)g.sc_extent.height - dh) / 2;
            VkImageBlit bl;
            memset(&bl, 0, sizeof(bl));
            bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bl.srcSubresource.layerCount = 1;
            bl.dstSubresource = bl.srcSubresource;
            bl.srcOffsets[1].x = (int)g.tv.w; bl.srcOffsets[1].y = (int)g.tv.h; bl.srcOffsets[1].z = 1;
            bl.dstOffsets[0].x = ox; bl.dstOffsets[0].y = oy;
            bl.dstOffsets[1].x = ox + dw; bl.dstOffsets[1].y = oy + dh; bl.dstOffsets[1].z = 1;
            vkCmdBlitImage(cb, present_source()->image, VK_IMAGE_LAYOUT_GENERAL, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            g_vk.tv_x = ox; g_vk.tv_y = oy; g_vk.tv_w = dw; g_vk.tv_h = dh;   // (F8 pick maps clicks through it)
            if (fg_n()) fg_capture(cb, present_source()); else g_fg.have = false;
            // (the blit and the counter both write the image: keep them in order)
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            draw_drc(cb, im);
            draw_fps(cb, im, (int)(s - g.sub));
            draw_warm(cb, im, (int)(s - g.sub));
            draw_bench(cb, im, (int)(s - g.sub));
            draw_debug_line(cb, im, (int)(s - g.sub), ox, oy, dw, dh);
            draw_tracker(cb, im, (int)(s - g.sub));
            draw_settings(cb, im, (int)(s - g.sub));
        }
        sc_barrier(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    if (!hold && g.tv_valid && !g_vk.gallery && flicker_catch_on()) { g_th_seq++; thumb_record(cb, s); }
    if (!hold && !g_vk.gallery) pause_watch_record(cb, s);
    u64 pf_sub = hnow_ns();
    query_suspend(cb);
    ts_end(cb);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (presenting) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquire_sem;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g.sc_done[index];
    }
    u64 sw_s0 = hnow_ns();
    tex_jobs_finish();
    defer_copies_run();
    VkResult r = vkQueueSubmit(g_vk.queue, 1, &si, s->fence);
    g_sw_submit = hnow_ns() - sw_s0;   // (texture jobs finishing + deferred copies + the submit)
    g_ss.submits++;
    gpu_lost_check(r);
    if (pick_read) pick_report();
    if (pick_next) g_pick_frame = true;
    s->busy = r == VK_SUCCESS;
    if (r != VK_SUCCESS) {
        static int warned = 0;
        if (warned++ < 4) fprintf(stderr, "[render] submitting GPU work failed (%d)\n", (int)r);
    }
    g.cur = -1;
    if (presenting) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g.sc_done[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g.swapchain;
        pi.pImageIndices = &index;
        VkPresentIdKHR pid = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
        static u64 present_id = 0;
        u64 my_id = 0;
        if (g_vk.present_wait_ext) { my_id = ++present_id; pid.swapchainCount = 1; pid.pPresentIds = &my_id; pi.pNext = &pid; }
        u64 sw_p0 = hnow_ns();
#ifdef _WIN32
        present_pace();
#endif
        u64 sw_p1 = hnow_ns();
        VkResult pr = vkQueuePresentKHR(g_vk.queue, &pi);
        g_sw_pace = sw_p1 - sw_p0;
        g_sw_present = hnow_ns() - sw_p1;
        gpu_lost_check(pr);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
        g_vk.presented++;
        g_fg.real_ns = hnow_ns();
        g_pf[PF_SUBMIT] += hnow_ns() - pf_sub;
        pf_sub = hnow_ns();
        // Low input lag: wait here until this frame is on the screen, then tell the game (its vsync wait ends).
        // The game's next frame can't have started yet, so nothing is waiting for this thread meanwhile.
        if (my_id && g_vk.low_latency && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) && !g.sc_rebuild) {
            VkResult wr = vkWaitForPresentKHR(g_vk.device, g.swapchain, my_id, 30000000ull);
            if (wr == VK_SUCCESS) gx2_note_vblank();
            else if (wr == VK_ERROR_OUT_OF_DATE_KHR) g.sc_rebuild = true;
        } else if (false && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) && !g.sc_rebuild) {   // (the image-acquire pacing: off, see build_swapchain)
            // (no present wait in the driver: acquire the next image now; with 2 images this blocks until
            // the frame just presented is on the screen -- the game's next frame starts then)
            g_pre.which = (g_pre.which + 1) & 3;
            VkResult ar = vkAcquireNextImageKHR(g_vk.device, g.swapchain, 100000000ull, g_pre.sem[g_pre.which], VK_NULL_HANDLE, &g_pre.index);
            if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) { g_pre.have = true; gx2_note_vblank(); }
            if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) g.sc_rebuild = true;
        }
    }
    g_pf[PF_SUBMIT] += hnow_ns() - pf_sub;
    if (g_qresult_pending) query_results();
    {   // GPU-thread timing report
        u64 now = hnow_ns();
        if (!g_pf_draw_start) g_pf_draw_start = now;
        g_pf_frames++;
        // One slow frame (over 50 ms between swaps here): what the drawing
        // thread did in it. Big drops -- shooting things, effects, wide views
        // -- vanish in the 300-frame averages.
        static u64 sp_last, sp_prev[PF_COUNT], sp_prev_now_ns, sp_prev_up_bytes;
        static u32 sp_prev_skipped, sp_prev_now, sp_prev_ups, sp_logged;
        if (sp_last && now - sp_last > spike_ns() && sp_logged < spike_cap()) {
            sp_logged++;
            #define SPD(i) ((g_pf[i] >= sp_prev[i] ? g_pf[i] - sp_prev[i] : g_pf[i]) / 1e6)
            static u64 sp_prev_tex[5], sp_prev_tex2[3], sp_prev_lap[2];
            static u32 sp_prev_views;
            static u64 sp_prev_cp[2];
            double sp_regs = cp_ms(g_cp_tsc[0] - sp_prev_cp[0]), sp_pk = cp_ms(g_cp_tsc[1] - sp_prev_cp[1]);
            double sp_known = SPD(PF_SHADERS) + SPD(PF_TEXTURES) + SPD(PF_UPLOAD) + SPD(PF_RECORD) + SPD(PF_WAIT) + SPD(PF_SUBMIT) + SPD(PF_SO) + SPD(PF_IDLE) + sp_regs + sp_pk;
            printf("[spike] frame %llu: %.0f ms on the drawing thread, %u draws | shaders+pipelines %.1f | textures %.1f (%u levels uploaded, %.1f MB; hash %.0f, create %.0f, decode %.0f, level uploads %.0f, texture uploads %.0f; %u views made %.0f, samplers %.0f, feedback copies %.0f; buffers to draw into %.0f, shaders ready %.0f) | "
                   "uploads %.1f | recording %.1f | ink waits %.1f (%.0f) | skipped %u | built while drawing %u (%.0f ms) | idle (waiting for the game) %.1f | register packets %.1f | other packets %.1f | unaccounted %.1f\n",
                   (unsigned long long)g.frame, (now - sp_last) / 1e6, g_vk.draws_last_frame, SPD(PF_SHADERS), SPD(PF_TEXTURES),
                   g_tex_uploads - sp_prev_ups, (g_tex_upload_bytes - sp_prev_up_bytes) / 1048576.0,
                   (g_tex_ns[0] - sp_prev_tex[0]) / 1e6, (g_tex_ns[1] - sp_prev_tex[1]) / 1e6, (g_tex_ns[2] - sp_prev_tex[2]) / 1e6, (g_tex_ns[3] - sp_prev_tex[3]) / 1e6, (g_tex_ns[4] - sp_prev_tex[4]) / 1e6,
                   g_tex_views_made - sp_prev_views, (g_tex_ns2[0] - sp_prev_tex2[0]) / 1e6, (g_tex_ns2[1] - sp_prev_tex2[1]) / 1e6, (g_tex_ns2[2] - sp_prev_tex2[2]) / 1e6,
                   (g_lap_split[0] - sp_prev_lap[0]) / 1e6, (g_lap_split[1] - sp_prev_lap[1]) / 1e6, SPD(PF_UPLOAD), SPD(PF_RECORD),
                   SPD(PF_SO), (double)(g_pf[PF_SO_COUNT] >= sp_prev[PF_SO_COUNT] ? g_pf[PF_SO_COUNT] - sp_prev[PF_SO_COUNT] : g_pf[PF_SO_COUNT]),
                   g_skipped_draws >= sp_prev_skipped ? g_skipped_draws - sp_prev_skipped : g_skipped_draws,
                   g_now_builds >= sp_prev_now ? g_now_builds - sp_prev_now : g_now_builds,
                   (g_now_ns >= sp_prev_now_ns ? g_now_ns - sp_prev_now_ns : g_now_ns) / 1e6, SPD(PF_IDLE),
                   sp_regs, sp_pk, (now - sp_last) / 1e6 - sp_known);
            #undef SPD
            memcpy(sp_prev_tex, g_tex_ns, sizeof(sp_prev_tex)); memcpy(sp_prev_tex2, g_tex_ns2, sizeof(sp_prev_tex2)); sp_prev_views = g_tex_views_made; memcpy(sp_prev_lap, g_lap_split, sizeof(sp_prev_lap));
            sp_prev_cp[0] = g_cp_tsc[0]; sp_prev_cp[1] = g_cp_tsc[1];
            {
                static u64 prev_sum;
                u64 sum = hatomic_load64(&g_pipe_build_sum_ns);
                if (sum - prev_sum > 50000000ull)
                    printf("[spike]   pipelines built by all threads since the last spike: %.0f ms of building, at most %llu at once\n", (sum - prev_sum) / 1e6, (unsigned long long)hatomic_load64(&g_pipe_build_max_par));
                prev_sum = sum;
                hatomic_store64(&g_pipe_build_max_par, 0);
            }
            if (g_fr_build_n[0] + g_fr_build_n[1])
                printf("[spike]   built while drawing: %u shaders (%.0f ms), %u pipelines (%.0f ms)\n", g_fr_build_n[0], g_fr_build_ns[0] / 1e6, g_fr_build_n[1], g_fr_build_ns[1] / 1e6);
            {   // (shader_ready since the last spike)
                static u32 pn[4];
                static u64 pns[4], psave, pwait;
                u32 n = 0;
                for (int i = 0; i < 4; i++) n += g_sw_n[i] - pn[i];
                if (n || g_sw_save_ns - psave > 2000000ull)
                    printf("[spike]   shaders ready: cached modules made here %u (%.0f ms), waited for helpers %u (%.0f ms); new shaders compiled here %u (%.0f ms), waited for helpers %u (%.0f ms); short waits %.0f ms; disk cache writes %.0f ms\n",
                           g_sw_n[0] - pn[0], (g_sw_ns[0] - pns[0]) / 1e6, g_sw_n[1] - pn[1], (g_sw_ns[1] - pns[1]) / 1e6,
                           g_sw_n[2] - pn[2], (g_sw_ns[2] - pns[2]) / 1e6, g_sw_n[3] - pn[3], (g_sw_ns[3] - pns[3]) / 1e6,
                           (g_sw_wait_ns - pwait) / 1e6, (g_sw_save_ns - psave) / 1e6);
                memcpy(pn, g_sw_n, sizeof(pn)); memcpy(pns, g_sw_ns, sizeof(pns)); psave = g_sw_save_ns; pwait = g_sw_wait_ns;
            }
        }
        {   // the frame tracker (frame_tracker.c): this frame's drawing-thread times
            #define TRD(i) ((float)((g_pf[i] >= sp_prev[i] ? g_pf[i] - sp_prev[i] : g_pf[i]) / 1e6))
            static u64 gpu_prev;
            TrackDraw td;
            memset(&td, 0, sizeof(td));
            td.total_ms = sp_last ? (float)((now - sp_last) / 1e6) : 0;
            td.busy_ms = td.total_ms - TRD(PF_IDLE);
            if (td.busy_ms < 0) td.busy_ms = 0;
            td.build_ms = TRD(PF_SHADERS);
            td.builds = g_fr_build_n[0] + g_fr_build_n[1];
            td.tex_ms = TRD(PF_TEXTURES);
            td.upload_ms = TRD(PF_UPLOAD);
            td.record_ms = TRD(PF_RECORD);
            td.card_wait_ms = TRD(PF_WAIT);
            td.gpu_ms = (float)((g_gpu_ns_all - gpu_prev) / 1e6);
            gpu_prev = g_gpu_ns_all;
            {
                static u64 ptex[5], patt;
                #define DT(a, p) ((float)((a) >= (p) ? ((a) - (p)) / 1e6 : 0))
                td.tex_hash_ms = DT(g_tex_ns[0], ptex[0]);
                td.tex_create_ms = DT(g_tex_ns[1], ptex[1]);
                td.tex_decode_ms = DT(g_tex_ns[2], ptex[2]);
                td.tex_upload_ms = DT(g_tex_ns[4], ptex[4]);
                td.att_ms = DT(g_lap_split[0], patt);
                static u64 prdy, psw[4], pswait;
                td.rdy_ms = DT(g_lap_split[1], prdy);
                for (int i = 0; i < 4; i++) { td.sw_ms[i] = DT(g_sw_ns[i], psw[i]); psw[i] = g_sw_ns[i]; }
                td.sw_ms[4] = DT(g_sw_wait_ns, pswait);
                pswait = g_sw_wait_ns;
                prdy = g_lap_split[1];
                #undef DT
                memcpy(ptex, g_tex_ns, sizeof(ptex));
                patt = g_lap_split[0];
            }
            tracker_draw_frame(&td);
            #undef TRD
        }
        g_fr_build_n[0] = g_fr_build_n[1] = 0; g_fr_build_ns[0] = g_fr_build_ns[1] = 0;
        {   // (the spike line's packet counters start over each frame; keep them current when no spike was printed)
            static u64 cp_seen[2];
            if (!(sp_last && now - sp_last > spike_ns() && sp_logged <= spike_cap())) { cp_seen[0] = g_cp_tsc[0]; cp_seen[1] = g_cp_tsc[1]; }
            (void)cp_seen;
        }
        sp_last = now;
        sp_prev_ups = g_tex_uploads;
        sp_prev_up_bytes = g_tex_upload_bytes;
        static u64 every = 0;
        if (!every) { const char* e = getenv("SPLAT_PERF_FRAMES"); every = e && atoi(e) > 0 ? (u64)atoi(e) : 300; }
        if (g_pf_frames >= every) {
            double f = (double)g_pf_frames, wall = (double)(now - g_pf_draw_start) / 1e6 / f;
            tsc_rate_update();
            static u64 pf_prev_cp[2];
            double pf_regs = cp_ms(g_cp_tsc[0] - pf_prev_cp[0]) / f, pf_pk = cp_ms(g_cp_tsc[1] - pf_prev_cp[1]) / f;
            pf_prev_cp[0] = g_cp_tsc[0]; pf_prev_cp[1] = g_cp_tsc[1];
            double pf_known = (g_pf[PF_SHADERS] + g_pf[PF_TEXTURES] + g_pf[PF_UPLOAD] + g_pf[PF_RECORD] + g_pf[PF_WAIT] + g_pf[PF_SUBMIT] + g_pf[PF_SO] + g_pf[PF_IDLE]) / 1e6 / f + pf_regs + pf_pk;
            printf("[perf] drawing thread besides draws: register packets %.1f ms, other packets %.1f ms, unaccounted %.1f ms per frame\n", pf_regs, pf_pk, wall - pf_known);
            printf("[perf] texture lookups the slow way per frame: %.0f new, %.0f drawn buffers again, %.0f textures again; %.0f memo resets, %.0f textures re-checked; %.1f glow-pass reads of a game-size copy\n",
                   g_tvc[0] / f, g_tvc[1] / f, g_tvc[2] / f, g_tvc[3] / f, g_tvc[4] / f, g_tvc[6] / f);
            memset(g_tvc, 0, sizeof(g_tvc));
            printf("[perf] indices: draws with them on the card already %.0f, copied from memory %.0f, converted %.0f (%.0f indices) per frame; %.0f moved to the card\n",
                   g_ix[0] / f, g_ix[1] / f, g_ix[2] / f, g_ix[3] / f, (double)g_ix[4]);
            memset(g_ix, 0, sizeof(g_ix));
            {
                static u64 prev[3];
                printf("[perf] command packets per frame: %.0f register packets, %.0f others, %.0f display-list words\n",
                       (g_cp_count[0] - prev[0]) / f, (g_cp_count[1] - prev[1]) / f, (g_cp_count[2] - prev[2]) / f);
                memcpy(prev, g_cp_count, sizeof(prev));
            }
            printf("[perf] per frame (avg of %u): %.1f ms total | shaders+pipelines %.1f | textures %.1f | uploads %.1f | "
                   "recording %.1f | waiting for graphics card %.1f | submit+present %.1f | ink waits %.1f (%.1f per frame) | draws skipped (shaders building) %u, frames not shown for it %u | built while drawing %u (%.0f ms) | idle (waiting for the game) %.1f\n", (unsigned)g_pf_frames, wall,
                   g_pf[PF_SHADERS] / 1e6 / f, g_pf[PF_TEXTURES] / 1e6 / f, g_pf[PF_UPLOAD] / 1e6 / f, g_pf[PF_RECORD] / 1e6 / f,
                   g_pf[PF_WAIT] / 1e6 / f, g_pf[PF_SUBMIT] / 1e6 / f, g_pf[PF_SO] / 1e6 / f, g_pf[PF_SO_COUNT] / f, g_skipped_draws, g_held_frames,
                   g_now_builds, g_now_ns / 1e6, g_pf[PF_IDLE] / 1e6 / f);
            if (g_held_few) { printf("[perf] frames held back for a few missing draws (no flicker): %u\n", g_held_few); g_held_few = 0; }
            {   // (fast-linked stand-ins: how many, how fast, and the parts behind them)
                static u64 l0, d0, ns0, b0, m0;
                u64 l = hatomic_load64(&g_gl_linked), d = hatomic_load64(&g_gl_used_draws), ns = hatomic_load64(&g_gl_link_ns), b = hatomic_load64(&g_gl_built), m = hatomic_load64(&g_gl_missing);
                if (l != l0 || b != b0 || m != m0)
                    printf("[perf] fast-linked pipelines: %llu linked (%.2f ms each), %llu draws drawn with one, %llu pipeline parts built, %llu times a part wasn't ready\n",
                           (unsigned long long)(l - l0), l > l0 ? (ns - ns0) / 1e6 / (double)(l - l0) : 0.0, (unsigned long long)(d - d0), (unsigned long long)(b - b0), (unsigned long long)(m - m0));
                l0 = l; d0 = d; ns0 = ns; b0 = b; m0 = m;
            }
            if (g_ts_ok > 0) printf("[perf] graphics card busy %.1f ms per frame\n", g_gpu_ns / 1e6 / f);
            if (g_fg.shown | g_fg.skipped) { printf("[perf] smooth motion: %llu generated pictures shown (%.2f per frame), %llu skipped (no free image / the last one still on the card)\n", (unsigned long long)g_fg.shown, (double)g_fg.shown / f, (unsigned long long)g_fg.skipped); g_fg.shown = g_fg.skipped = 0; }
            if (g_sec_runs[SEC_VOL_MARCH] | g_sec_runs[SEC_VOL_COMP]) {
                printf("[perf] card time in our passes: SHADERS march %.2f ms, composite %.2f ms (per run; %llu / %llu runs measured)\n",
                       g_sec_runs[SEC_VOL_MARCH] ? g_sec_ns[SEC_VOL_MARCH] / 1e6 / g_sec_runs[SEC_VOL_MARCH] : 0.0,
                       g_sec_runs[SEC_VOL_COMP] ? g_sec_ns[SEC_VOL_COMP] / 1e6 / g_sec_runs[SEC_VOL_COMP] : 0.0,
                       (unsigned long long)g_sec_runs[SEC_VOL_MARCH], (unsigned long long)g_sec_runs[SEC_VOL_COMP]);
                memset(g_sec_ns, 0, sizeof(g_sec_ns)); memset(g_sec_runs, 0, sizeof(g_sec_runs));
            }
            {   // (the "textures" part, broken down -- as on the [spike] lines, per frame)
                static u64 pt[5], pt2[3], pl[2], pa[3];
                printf("[perf] texture work per frame: hash %.2f, create %.2f, decode %.2f, level uploads %.2f, texture uploads %.2f; views made %.2f, samplers %.2f, "
                       "feedback copies + mip sets %.2f; buffers to draw into %.2f (colour %.2f, depth %.2f, rest %.2f), shaders ready %.2f ms\n",
                       (g_tex_ns[0] - pt[0]) / 1e6 / f, (g_tex_ns[1] - pt[1]) / 1e6 / f, (g_tex_ns[2] - pt[2]) / 1e6 / f, (g_tex_ns[3] - pt[3]) / 1e6 / f,
                       (g_tex_ns[4] - pt[4]) / 1e6 / f, (g_tex_ns2[0] - pt2[0]) / 1e6 / f, (g_tex_ns2[1] - pt2[1]) / 1e6 / f, (g_tex_ns2[2] - pt2[2]) / 1e6 / f,
                       (g_lap_split[0] - pl[0]) / 1e6 / f, (g_att_split[0] - pa[0]) / 1e6 / f, (g_att_split[1] - pa[1]) / 1e6 / f, (g_att_split[2] - pa[2]) / 1e6 / f,
                       (g_lap_split[1] - pl[1]) / 1e6 / f);
                {   // (the draw setup's parts per frame: where the "shaders" bucket goes)
                    static u64 ps_[4];
                    static u64 pk_[2];
                    printf("[perf] draw setup per frame: shader key %.2f, vertex shader lookup %.2f, pixel shader lookup %.2f, pipeline lookup %.2f ms; memo hits %.0f, misses %.0f%s\n",
                           (g_sd_sum[0] - ps_[0]) / 1e6 / f, (g_sd_sum[1] - ps_[1]) / 1e6 / f, (g_sd_sum[2] - ps_[2]) / 1e6 / f, (g_sd_sum[3] - ps_[3]) / 1e6 / f,
                           (double)(g_km_hits - pk_[0]) / f, (double)(g_km_misses - pk_[1]) / f, g_km_check > 0 ? (g_km_bad ? " (CHECK: MISMATCHES, see above)" : " (check: all hits right)") : "");
                    memcpy(ps_, g_sd_sum, sizeof(ps_));
                    pk_[0] = g_km_hits; pk_[1] = g_km_misses;
                    {
                        static u64 pw_[6], pr_[KM_REGS];
                        (void)pr_;
                        printf("[perf] memo misses per frame: empty slot %.0f, other programs in the slot %.0f, points %.0f, target table %.0f, registers %.0f, memory/unchecked %.0f\n",
                               (double)(g_km_why[0] - pw_[0]) / f, (double)(g_km_why[1] - pw_[1]) / f, (double)(g_km_why[2] - pw_[2]) / f, (double)(g_km_why[3] - pw_[3]) / f, (double)(g_km_why[4] - pw_[4]) / f, (double)(g_km_why[5] - pw_[5]) / f);
                        memcpy(pw_, g_km_why, sizeof(pw_));
                    }
                }
                memcpy(pt, g_tex_ns, sizeof(pt)); memcpy(pt2, g_tex_ns2, sizeof(pt2)); memcpy(pl, g_lap_split, sizeof(pl)); memcpy(pa, g_att_split, sizeof(pa));
            }
            {   // (drawn buffers kept: how many, and the longest list behind one lookup slot / at one address)
                u32 n_rt = 0, worst = 0, worst_b = 0, same = 0, same_addr = 0;
                for (Surf* s = g.rts; s; s = s->next) n_rt++;
                for (u32 b = 0; b < 4096; b++) {
                    u32 k = 0;
                    for (Surf* s = g_rth[b]; s; s = s->anext) k++;
                    if (k > worst) { worst = k; worst_b = b; }
                }
                for (Surf* s = g_rth[worst_b]; s; s = s->anext) {
                    u32 k = 0;
                    for (Surf* o = g_rth[worst_b]; o; o = o->anext) k += o->addr == s->addr;
                    if (k > same) { same = k; same_addr = s->addr; }
                }
                printf("[perf] drawn buffers kept: %u, longest lookup list %u, most at one address %u (%08X)\n", n_rt, worst, same, same_addr);
            }
            if (g_drc_draws_skipped) printf("[perf] Performance mode: %.0f GamePad-picture draws per frame left out (its screen isn't shown)\n", g_drc_draws_skipped / f);
            g_drc_draws_skipped = 0;
            if (g_now_builds) {
                char why[400]; int o = 0;
                for (int i = 0; i < BW_N; i++)
                    if (g_bw_n[i]) o += snprintf(why + o, sizeof(why) - o, "%s%s %u (%.0f ms)", o ? ", " : "", k_bw_name[i], g_bw_n[i], g_bw_ns[i] / 1e6);
                printf("[perf] built while drawing, why: %s\n", o ? why : "-");
            }
            memset(g_bw_n, 0, sizeof(g_bw_n)); memset(g_bw_ns, 0, sizeof(g_bw_ns));
            g_gpu_ns = 0;
            g_skipped_draws = 0;
            g_held_frames = 0;
            g_now_builds = 0;
            g_now_ns = 0;
            memset(g_pf, 0, sizeof(g_pf));
            g_pf_frames = 0;
            g_pf_draw_start = now;
        }
        memcpy(sp_prev, g_pf, sizeof(sp_prev));
        sp_prev_skipped = g_skipped_draws;
        sp_prev_now = g_now_builds;
        sp_prev_now_ns = g_now_ns;
    }
    if (dump && s->busy) {
        wait_mark("the card's fence for a debug dump", NULL);
        vkWaitForFences(g_vk.device, 1, &s->fence, VK_TRUE, UINT64_MAX);
        wait_unmark();
        finish_readback(s);
    }
    if (dbg_dump) {
        vkDeviceWaitIdle(g_vk.device);
        write_rt_dumps();
        printf("[dbg] end of debug frame\n");
    }
    // log every draw of a few frames (the loading screen starts after the first few hundred)
    // (only when asked for: SPLAT_DEBUG=1 or a debug_on.txt next to the
    // game -- writing all the pictures and captures made big hitches)
    bool auto_dbg = debug_auto();
    // (F12 too, but only in debug mode: logging every draw and saving every
    // buffer as a picture froze the game ~15 s, and the sound ran dry)
    static long long dbg_frame = -2;   // (SPLAT_DEBUG_FRAME=N: also that frame, e.g. a menu reached by an autoplay route)
    if (dbg_frame == -2) { const char* e = getenv("SPLAT_DEBUG_FRAME"); dbg_frame = e ? atoll(e) : -1; }
    g_dbg = auto_dbg && (g.frame == 299 || g.frame == 899 || g.frame == 1799 || g_vk.screenshot || (long long)g.frame == dbg_frame);
    if (g_vk.dbg_req) { g_vk.dbg_req = 0; g_dbg = true; }
    {   // SPLAT_DEBUG_SCENE=1: every draw logged (no picture dumps) for 20 s after each scene change -- a map whose first
        // frame kills the graphics driver (Hammerhead Bridge) leaves its draws in the log
        static const char* on = NULL;
        static int asked;
        static unsigned seen;
        static u64 until;
        if (!asked) { asked = 1; on = getenv("SPLAT_DEBUG_SCENE"); }   // (=1: every scene; =Kaisou: scenes whose name starts so)
        if (on) {
            extern volatile unsigned splat_scene_seq;
            extern char splat_scene_map[64];
            bool want = !strcmp(on, "1") || !strncmp(splat_scene_map, on, strlen(on));
            if (splat_scene_seq != seen) { seen = splat_scene_seq; until = want ? g.frame + 1200 : 0; }
            g_dbg_nodump = g.frame < until;
            if (g_dbg_nodump) g_dbg = true;
        }
    }
    // save one whole frame for replaying (tools/frame_replay.c), and again on F12
    static long long cap_frame = -1;
    if (cap_frame < 0) { const char* e = getenv("SPLAT_CAPTURE_FRAME"); cap_frame = e ? atoll(e) : auto_dbg ? 1200 : 0x7FFFFFFFFFFFll; }
    if ((long long)g.frame == cap_frame || f12) g_cap_request = true;
    if (f12) g_cap_more = 1;   // (Ctrl+F12: two frames in a row, to compare for flicker)
    if (auto_dbg) {   // also save one busy frame (a big scene, after it has settled)
        static u32 busy_frames = 0;
        if (g_vk.draws_last_frame > 600 && ++busy_frames == 300) g_cap_request = true;
    }
    if (g_dbg) printf("[dbg] ---- debug frame %llu ----\n", (unsigned long long)g.frame + 1);
    // save newly built pipelines now and then -- on a helper thread (the
    // driver's cache can be tens of MB: writing it here stalled the frame)
    if (g.new_pipes && g.frame % 1200 == 0 && g.pcache && !hatomic_load64(&g_pcache_saving)) {
        hatomic_store64(&g_pcache_saving, 1);
        hthrd_t t;
        if (!hthrd_create(&t, pcache_save_thread, NULL, 1u << 20)) hatomic_store64(&g_pcache_saving, 0);
#ifdef _WIN32
        else CloseHandle(t);
#endif
        g.new_pipes = 0;
    }
    u64 sw_r0 = hnow_ns();
    if (g.frame == 1 || g.frame % 600 == 0) {
        printf("[render] frame %llu: %u draws\n", (unsigned long long)g.frame, g_vk.draws_last_frame);
        if (g_present_skips)
            printf("[render] %u frames not shown since the last report (no free swapchain image: %u acquires timed out, %u images not released within %.0f ms of their acquire)\n",
                   g_present_skips, g_present_skips - g_present_skips_fence, g_present_skips_fence, acq_fence_ns() / 1e6);
        g_present_skips = g_present_skips_fence = 0;
        {
            u32 nb = 0;
            for (u32 i = 0; i < ISTORE_BLOCKS; i++) nb += g_ib[i].buf != VK_NULL_HANDLE;
            if (nb) printf("[render] index store: %u of %u blocks of %u MB in use\n", nb, ISTORE_BLOCKS, ISTORE_BLOCK >> 20);
        }
        if (g_up_copied + g_up_reused)
            printf("[render] draw data per frame: %.1f MB copied (%.1f MB of it at the game's flush), %.1f MB found already uploaded (%.0f%%)\n",
                   g_up_copied / 1048576.0 / 600, g_up_deferred / 1048576.0 / 600, g_up_reused / 1048576.0 / 600, 100.0 * g_up_reused / (double)(g_up_copied + g_up_reused));
        g_up_copied = g_up_reused = g_up_deferred = 0;
        printf("[render] draw data per frame by kind (copied / found already uploaded, MB): uniform blocks %.2f / %.2f, vertex data %.2f / %.2f, uniform registers %.2f, draw tables %.2f\n",
               g_up_cat_copied[0] / 1048576.0 / 600, g_up_cat_reused[0] / 1048576.0 / 600, g_up_cat_copied[1] / 1048576.0 / 600, g_up_cat_reused[1] / 1048576.0 / 600,
               g_up_cat_copied[2] / 1048576.0 / 600, g_up_cat_copied[3] / 1048576.0 / 600);
        memset(g_up_cat_copied, 0, sizeof(g_up_cat_copied));
        if (g_ring.state > 0) {
            static u64 pl, pw, pf;
            printf("[render] draw-data ring: %llu laps, %llu waits for the card, %llu draws in upload chunks instead (since the last line)\n",
                   (unsigned long long)(g_ring.laps - pl), (unsigned long long)(g_ring.waits - pw), (unsigned long long)(g_ring.fallbacks - pf));
            pl = g_ring.laps; pw = g_ring.waits; pf = g_ring.fallbacks;
        }
        memset(g_up_cat_reused, 0, sizeof(g_up_cat_reused));
        if (g_ub_padded)
            printf("[render] uniform blocks padded for fixed-index reads past their end: %.1f per frame (%.1f KB)\n", g_ub_padded / 600.0, g_ub_pad_bytes / 1024.0 / 600);
        g_ub_padded = g_ub_pad_bytes = 0;
    }
    g_sw_report = hnow_ns() - sw_r0;
    g_frame_start_ns = hnow_ns();   // (wait_built: the next frame's build waits end 11 ms from here)
    if (hnow_ns() - g_sw_t0 > 5000000ull) {   // (the swap packet took over 5 ms: the game waits for it in GX2DrawDone)
        static int n = 0;
        if (n++ < 60) printf("[render] slow swap (frame %llu): %.1f ms -- submit %.1f, pacing query %.1f, present %.1f, the 600-frame report %.1f, the rest %.1f\n",
                             (unsigned long long)g.frame, (hnow_ns() - g_sw_t0) / 1e6, g_sw_submit / 1e6, g_sw_pace / 1e6, g_sw_present / 1e6, g_sw_report / 1e6,
                             (hnow_ns() - g_sw_t0 - g_sw_submit - g_sw_pace - g_sw_present - g_sw_report) / 1e6);
    }
}

void latte_render_thread_start(void) {}

void latte_render_note_idle(u64 ns) { g_pf[PF_IDLE] += ns; }
u32 latte_render_deferred_count(void) { return g_def_n; }   // (gx2.c's slow-DrawDone line)
// (frame_tracker.c: the drawing thread's work since the last game frame, read live by the game thread as it classifies
// its frame. Its own per-swap report comes a frame later, which blamed "pacing / system" for texture work the game had
// been waiting on -- the Oct 31 bench. Torn reads only blur a number.)
void latte_render_tracker_live(TrackDraw* d) {
    static u64 prev[PF_COUNT];
    memset(d, 0, sizeof(*d));
    #define LIVE(i) ((float)((g_pf[i] >= prev[i] ? g_pf[i] - prev[i] : 0) / 1e6))
    d->build_ms = LIVE(PF_SHADERS);
    d->tex_ms = LIVE(PF_TEXTURES);
    d->upload_ms = LIVE(PF_UPLOAD);
    d->record_ms = LIVE(PF_RECORD);
    d->card_wait_ms = LIVE(PF_WAIT);
    d->busy_ms = d->build_ms + d->tex_ms + d->upload_ms + d->record_ms + d->card_wait_ms + LIVE(PF_SUBMIT) + LIVE(PF_SO);
    #undef LIVE
    for (int i = 0; i < PF_COUNT; i++) prev[i] = g_pf[i];
    static u64 ptex[5], patt;
    #define DT(a, p) ((float)((a) >= (p) ? ((a) - (p)) / 1e6 : 0))
    d->tex_hash_ms = DT(g_tex_ns[0], ptex[0]);
    d->tex_create_ms = DT(g_tex_ns[1], ptex[1]);
    d->tex_decode_ms = DT(g_tex_ns[2], ptex[2]);
    d->tex_upload_ms = DT(g_tex_ns[4], ptex[4]);
    d->att_ms = DT(g_lap_split[0], patt);
    static u64 prdy, psw[4], pswait;
    d->rdy_ms = DT(g_lap_split[1], prdy);
    for (int i = 0; i < 4; i++) { d->sw_ms[i] = DT(g_sw_ns[i], psw[i]); psw[i] = g_sw_ns[i]; }
    d->sw_ms[4] = DT(g_sw_wait_ns, pswait);
    pswait = g_sw_wait_ns;
    prdy = g_lap_split[1];
    #undef DT
    memcpy(ptex, g_tex_ns, sizeof(ptex));
    patt = g_lap_split[0];
}

bool latte_render_idle(void) {
    // the GPU thread has nothing to do: hand over finished query results
    if (!g.ok) return false;
    static int old = -1;   // SPLAT_IDLE_OLD=1: nap between polls as before (tests)
    if (old < 0) old = getenv("SPLAT_IDLE_OLD") != NULL;
    if (g_def_n) {   // (queued copies / fences the game may be waiting for: wait on the card's fence, then hand them over)
        serial_wait(g_def[g_def_head].serial, old ? 500000 : 1000000);
        defer_poll(false);
    }
    bool rb = false;
    for (u32 i = 0; i < RB_BATCHES; i++) rb |= g_rbb[i].used;
    if (g_qresult_pending || rb) latte_render_sync_readbacks();
    fg_tick();
    return !old && g_def_n > 0;
}

// (between packets, now and then) queued copies / fences whose work the
// graphics card has finished -> game memory, without waiting.
void latte_render_poll(void) {
    static int old = -1;
    if (old < 0) old = getenv("SPLAT_IDLE_OLD") != NULL;
    if (!old && g_def_n && serial_done(g_def[g_def_head].serial)) defer_poll(false);
    fg_tick();
}


// --- Frame capture and replay support --------------------------------------------------------------

static float texel_value(VkFormat f, const u8* px, u32 i, u32 c);

// Bytes per texel when copying an image to/from a buffer (0 = unknown).
static u32 texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
        return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_D16_UNORM:
        return 2;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32:
        return 4;   // (depth+stencil formats: the depth part only)
    default:
        return 0;
    }
}

static void flush_nowait(void) {
    end_pass();
    if (g.cur < 0) return;
    Sub* s = &g.sub[g.cur];
    query_suspend(s->cb);
    ts_end(s->cb);
    vkEndCommandBuffer(s->cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cb;
    tex_jobs_finish();
    defer_copies_run();
    VkResult sr = vkQueueSubmit(g_vk.queue, 1, &si, s->fence);
    g_ss.submits++;
    gpu_lost_check(sr);
    s->busy = sr == VK_SUCCESS;
    g.cur = -1;
    g.need_barrier = false;
}

// Stream-out results -> game memory (waits for the graphics card).
static void so_sync(void) {
    if (!g_so_npend) return;
    u64 t0 = hnow_ns();
    flush_wait();
    g_pf[PF_SO] += hnow_ns() - t0;
    g_pf[PF_SO_COUNT]++;
    g_ss.so_waits++;
    g_ss.so_wait_ns += hnow_ns() - t0;
    defer_poll(true);   // (older queued copies first: the graphics card is done with them now)
    for (u32 i = 0; i < g_so_npend; i++) {
        const SoPend* p = &g_so_pend[i];
        u8* dst = wiiu_mem_ptr(p->addr, p->size);
        if (dst) { memcpy(dst, (u8*)g.so_ptr + p->ring, p->size); wiiu_mem_mark(p->addr, p->size); so_hash_note(dst, p->size); }
    }
    g_so_npend = 0;
}

// Submit what's recorded so far and wait for the graphics card to finish.
static void flush_wait(void) {
    end_pass();
    if (g.cur < 0) return;
    Sub* s = &g.sub[g.cur];
    query_suspend(s->cb);
    ts_end(s->cb);
    vkEndCommandBuffer(s->cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cb;
    tex_jobs_finish();
    defer_copies_run();
    VkResult r = vkQueueSubmit(g_vk.queue, 1, &si, s->fence);
    s->busy = r == VK_SUCCESS;
    g.cur = -1;
    if (s->busy) { wait_mark("the card's fence after a flush (flush_wait)", NULL); r = vkWaitForFences(g_vk.device, 1, &s->fence, VK_TRUE, UINT64_MAX); wait_unmark(); }
    gpu_lost_check(r);
    if (r != VK_SUCCESS) printf("[render] flush failed (%d)\n", (int)r);
    g.need_barrier = false;
}

// Raw texels of an image (depth part for depth buffers). malloc'd.
static u8* read_image(Img* im, u32* out_size) {
    u32 tb = texel_bytes(im->format);
    if (!tb || !im->image) return NULL;
    u32 size = im->w * im->h * tb * im->layers * (im->depth3d ? im->depth3d : 1);
    VkBuffer buf;
    VkDeviceMemory mem;
    void* ptr;
    if (!host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &ptr)) return NULL;
    VkCommandBuffer cb = transfer_cmd();
    barrier_all(cb);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = im->layers;
    c.imageExtent.width = im->w;
    c.imageExtent.height = im->h;
    c.imageExtent.depth = im->depth3d ? im->depth3d : 1;
    vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    flush_wait();
    u8* out = (u8*)malloc(size);
    if (out) memcpy(out, ptr, size);
    vkDestroyBuffer(g_vk.device, buf, NULL);
    vkFreeMemory(g_vk.device, mem, NULL);
    *out_size = size;
    return out;
}

// The stencil half of a depth/stencil buffer (one byte per texel), or NULL.
static u8* read_stencil(Img* im, u32* out_size) {
    if (!(im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) || !im->image) return NULL;
    u32 size = im->w * im->h * im->layers;
    VkBuffer buf;
    VkDeviceMemory mem;
    void* ptr;
    if (!host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buf, &mem, &ptr)) return NULL;
    VkCommandBuffer cb = transfer_cmd();
    barrier_all(cb);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    c.imageSubresource.layerCount = im->layers;
    c.imageExtent.width = im->w;
    c.imageExtent.height = im->h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    flush_wait();
    u8* out = (u8*)malloc(size);
    if (out) memcpy(out, ptr, size);
    vkDestroyBuffer(g_vk.device, buf, NULL);
    vkFreeMemory(g_vk.device, mem, NULL);
    *out_size = size;
    return out;
}

bool latte_render_early_capture(void) {
    static int done = 0;
    if (done >= 1 || !ready() || !debug_auto()) return false;
    u32 info = R(CB_COLOR0_INFO), base = R(CB_COLOR0_BASE) << 8;
    if (((info >> 2) & 0x3F) != 0x22 || !base) return false;
    int ri = find_rti(base, false);
    if (ri < 0 || g.rti[ri].layers < 6) return false;
    done++;
    printf("[capture] the game is building a compressed cube map at %08X: saving the rest of this frame\n", base);
    return true;
}

bool latte_render_capture_wanted(void) { bool r = g_cap_request; g_cap_request = false; return r; }
void latte_render_perf_print(void) {
    printf("[perf] shader re-checks: %llu, %.2f ms\n", (unsigned long long)g_recheck_count, g_recheck_ns / 1e6);
    printf("[perf] totals: shaders+pipelines %.2f ms, textures %.2f, uploads %.2f, recording %.2f, waits %.2f, submit %.2f\n",
           g_pf[PF_SHADERS] / 1e6, g_pf[PF_TEXTURES] / 1e6, g_pf[PF_UPLOAD] / 1e6, g_pf[PF_RECORD] / 1e6, g_pf[PF_WAIT] / 1e6, g_pf[PF_SUBMIT] / 1e6);
}
bool latte_render_capture_more(void) { if (g_cap_more <= 0) return false; g_cap_more--; return true; }

// Start a capture: the file, then everything already on the graphics card
// that the frame may use (buffer sizes, buffer contents, oldest first).
void latte_render_begin_capture(void) {
    if (!ready()) return;
    char path[64];
    snprintf(path, sizeof(path), "%s/capture_f%llu.splatcap", debug_dir(), (unsigned long long)g.frame);
    if (!cap_open(path)) return;
    for (u32 i = 0; i < 128; i++) {
        if (!g.rti[i].used) continue;
        u32 d[8] = { g.rti[i].depth ? 8u : 0u, g.rti[i].addr, g.rti[i].w, g.rti[i].h, g.rti[i].fmt, 0, 0, (g.rti[i].layers << 8) | g.rti[i].dim };
        cap_event(CAPE_RT_INFO, d, 8);
    }
    u64 last = 0;
    for (;;) {   // surfaces in the order they were last drawn to
        Surf* next = NULL;
        for (Surf* s = g.rts; s; s = s->next)
            if (s->seq > last && (!next || s->seq < next->seq)) next = s;
        if (!next) break;
        last = next->seq;
        u32 size = 0;
        u8* px = read_image(&next->img, &size);
        if (px) {
            cap_surface(next->addr, next->gx2fmt, next->w, next->h, (next->depth ? 1u : 0u) | (next->vol ? 2u : 0u), (u32)next->img.format, px, size);
            free(px);
        }
        if (next->depth && next->stencil) {   // (flag 4: the stencil half)
            u8* st = read_stencil(&next->img, &size);
            if (st) { cap_surface(next->addr, next->gx2fmt, next->w, next->h, 1u | 4u, (u32)next->img.format, st, size); free(st); }
        }
    }
}

// Replay: put a saved buffer back.
void latte_render_restore_surface(u32 addr, u32 gx2fmt, u32 w, u32 h, u32 flags, u32 vkformat, const void* bytes, u32 size) {
    bool depth = flags & 1, vol = (flags & 2) != 0;
    if (!ready()) return;
    if (flags & 4) {   // the stencil half of a depth buffer restored just before
        Surf* s = NULL;
        for (Surf* t = g.rts; t; t = t->next)
            if (t->addr == (addr & ~0x7FFu) && t->depth && t->w == w && t->h == h && t->w * t->h * t->layers == size) { s = t; break; }
        if (!s || !s->stencil) return;
        VkBuffer buf;
        VkDeviceMemory mem;
        void* ptr;
        if (!host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf, &mem, &ptr)) return;
        memcpy(ptr, bytes, size);
        VkCommandBuffer cb = transfer_cmd();
        VkBufferImageCopy c;
        memset(&c, 0, sizeof(c));
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        c.imageSubresource.layerCount = s->layers;
        c.imageExtent.width = w;
        c.imageExtent.height = h;
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, buf, s->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        flush_wait();
        vkDestroyBuffer(g_vk.device, buf, NULL);
        vkFreeMemory(g_vk.device, mem, NULL);
        return;
    }
    u32 vtb = texel_bytes((VkFormat)vkformat);
    u32 layers = vtb && w * h ? size / (w * h * vtb) : 1;
    Surf* s = get_rt(addr, gx2fmt, w, h, depth, layers | (vol ? RT_3D : 0));
    u8* conv = NULL;
    if (s && depth && (u32)s->img.format != vkformat && texel_bytes(s->img.format) == 4 && vtb == 4) {
        // this graphics card stores depth differently: convert
        conv = (u8*)malloc(size);
        for (u32 i = 0; i < size / 4; i++) {
            float v = texel_value((VkFormat)vkformat, (const u8*)bytes, i, 0);
            u32 out;
            if (s->img.format == VK_FORMAT_D24_UNORM_S8_UINT || s->img.format == VK_FORMAT_X8_D24_UNORM_PACK32) out = (u32)(v * 16777215.0f + 0.5f);
            else memcpy(&out, &v, 4);
            memcpy(conv + i * 4, &out, 4);
        }
        bytes = conv;
        vkformat = (u32)s->img.format;
    }
    if (!s || (u32)s->img.format != vkformat || size != w * h * texel_bytes(s->img.format) * (vol ? s->img.depth3d : s->layers)) {
        free(conv);
        printf("[replay] couldn't restore buffer %08X %ux%u\n", addr, w, h);
        return;
    }
    VkBuffer buf;
    VkDeviceMemory mem;
    void* ptr;
    if (!host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &buf, &mem, &ptr)) { free(conv); return; }
    memcpy(ptr, bytes, size);
    free(conv);
    VkCommandBuffer cb = transfer_cmd();
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = vol ? 1 : s->layers;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = vol ? s->img.depth3d : 1;
    vkCmdCopyBufferToImage(cb, buf, s->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    flush_wait();
    vkDestroyBuffer(g_vk.device, buf, NULL);
    vkFreeMemory(g_vk.device, mem, NULL);
}

void latte_render_set_debug(bool on) { g_dbg = on; }

static float half_to_float(u16 h) {
    u32 e = (h >> 10) & 0x1F, m = h & 0x3FF, sgn = (u32)(h >> 15) << 31, bits;
    if (e == 0) { float f = (float)m / 16777216.0f; return sgn ? -f : f; }
    if (e == 31) bits = sgn | 0x7F800000u | (m << 13);
    else bits = sgn | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Value of channel c of texel i as a float (for pictures / statistics).
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static float texel_value(VkFormat f, const u8* px, u32 i, u32 c) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return px[i * 4 + c] / 255.0f;
    case VK_FORMAT_R8_UNORM: return c == 0 ? px[i] / 255.0f : 0;
    case VK_FORMAT_R8G8_UNORM: return c < 2 ? px[i * 2 + c] / 255.0f : 0;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { u32 v; memcpy(&v, px + i * 4, 4); return c < 3 ? ((v >> (10 * c)) & 0x3FF) / 1023.0f : (v >> 30) / 3.0f; }
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
        u32 v; memcpy(&v, px + i * 4, 4);
        if (c == 3) return 1;
        u32 bits = c == 0 ? v & 0x7FF : c == 1 ? (v >> 11) & 0x7FF : (v >> 22) & 0x3FF;
        u32 mb = c == 2 ? 5 : 6;
        u32 e = bits >> mb, m = bits & ((1u << mb) - 1);
        if (e == 0) return (float)m / (float)(1u << mb) / 16384.0f;
        if (e == 31) return 65504.0f;
        return ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
    }
    case VK_FORMAT_R16_SFLOAT: { u16 h; memcpy(&h, px + i * 2, 2); return c == 0 ? half_to_float(h) : 0; }
    case VK_FORMAT_R16G16_SFLOAT: { u16 h; if (c > 1) return 0; memcpy(&h, px + i * 4 + c * 2, 2); return half_to_float(h); }
    case VK_FORMAT_R16G16B16A16_SFLOAT: { u16 h; memcpy(&h, px + i * 8 + c * 2, 2); return half_to_float(h); }
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: { float v; if (c) return 0; memcpy(&v, px + i * 4, 4); return v; }
    case VK_FORMAT_R32G32_SFLOAT: { float v; if (c > 1) return 0; memcpy(&v, px + i * 8 + c * 4, 4); return v; }
    case VK_FORMAT_R32G32B32A32_SFLOAT: { float v; memcpy(&v, px + i * 16 + c * 4, 4); return v; }
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: { u32 v; if (c) return 0; memcpy(&v, px + i * 4, 4); return (v & 0xFFFFFF) / 16777215.0f; }
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_R16_UNORM: { u16 v; if (c) return 0; memcpy(&v, px + i * 2, 2); return v / 65535.0f; }
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: { u32 v; memcpy(&v, px + i * 16 + c * 4, 4); return (float)v; }   // (raw numbers)
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: { u32 v; if (c > 1) return 0; memcpy(&v, px + i * 8 + c * 4, 4); return (float)v; }
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: { u32 v; if (c) return 0; memcpy(&v, px + i * 4, 4); return (float)v; }
    default: return 0;
    }
}

// Debugging (replay): one pixel of the newest buffer at addr, as numbers.
bool latte_render_probe(u32 addr, u32 x, u32 y, float out[4]) {
    if (!ready()) return false;
    Surf* s = find_rt(addr, false);
    if (!s || x >= s->w || y >= s->h) return false;
    u32 size = 0;
    u8* px = read_image(&s->img, &size);
    if (!px) return false;
    for (u32 c = 0; c < 4; c++) out[c] = texel_value(s->img.format, px, y * s->w + x, c);
    free(px);
    return true;
}

// Save every buffer as a picture named <prefix>_<addr>_<w>x<h>_<fmt>.png,
// and print what's in it (lowest / highest / average value per channel).
// Depth buffers are stretched so near and far differ visibly.
// frame_replay: the picture as it would be shown (a debug view's too) -> PNG.
void latte_render_save_shown(const char* path) {
    if (!ready() || !g.tv_valid) return;
    Img* src = present_source();
    u32 size = 0;
    u8* px = read_image(src, &size);
    if (!px) return;
    write_png(path, px, src->w, src->h);
    free(px);
    printf("[render] saved %s (%ux%u, debug view %d)\n", path, src->w, src->h, g_vk.debug_view);
}
// frame_replay: the coming draws are counted for a pick at that TV pixel (printed at the frame's end).
void latte_render_pick_now(int x, int y) {
    if (!ready()) return;
    g_vk.debug_views_on = 1;
    g_vk.pick_x = x;
    g_vk.pick_y = y;
    end_pass();
    if (pick_start(cmd())) g_pick_frame = true;
}

void latte_render_dump_all(const char* prefix) {
    if (!ready()) return;
    const char* only = getenv("DUMP_ONLY");   // (replay: just this buffer address, hex)
    u32 only_addr = only ? (u32)strtoul(only, NULL, 16) : 0;
    for (Surf* s = g.rts; s; s = s->next) {
        if (only_addr && s->addr != only_addr) continue;
        u32 size = 0;
        u8* px = read_image(&s->img, &size);
        if (!px) { printf("[dump] %08X %ux%u: format %d not readable\n", s->addr, s->w, s->h, (int)s->img.format); continue; }
        if (getenv("RAW_DUMP")) {   // (debugging: the bytes as they are, all slices)
            char name[96];
            snprintf(name, sizeof(name), "%s_%08X_%ux%ux%u_%03X.raw", prefix, s->addr, s->w, s->h, s->layers, s->gx2fmt);
            FILE* rf = fopen(name, "wb");
            if (rf) { fwrite(px, 1, size, rf); fclose(rf); }
        }
        u32 n = s->w * s->h;
        float* fv = (float*)malloc((size_t)n * 16);
        u8* rgba = (u8*)malloc((size_t)n * 4);
        if (!fv || !rgba) { free(fv); free(rgba); free(px); continue; }
        for (u32 i = 0; i < n; i++)
            for (u32 c = 0; c < 4; c++) fv[i * 4 + c] = texel_value(s->img.format, px, i, c);
        float lo[4], hi[4];
        double sum[4];
        for (u32 c = 0; c < 4; c++) {
            lo[c] = hi[c] = fv[c];
            sum[c] = 0;
        }
        for (u32 i = 0; i < n; i++)
            for (u32 c = 0; c < 4; c++) {
                float v = fv[i * 4 + c];
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
                sum[c] += v;
            }
        printf("[dump] %s %08X %ux%u %s fmt %03X vk %d: R %.4g..%.4g (avg %.4g) G %.4g..%.4g (%.4g) B %.4g..%.4g (%.4g) A %.4g..%.4g (%.4g)\n",
               prefix, s->addr, s->w, s->h, s->depth ? "depth" : "color", s->gx2fmt, (int)s->img.format,
               lo[0], hi[0], sum[0] / n, lo[1], hi[1], sum[1] / n, lo[2], hi[2], sum[2] / n, lo[3], hi[3], sum[3] / n);
        for (u32 L = 1; L < s->layers && !s->depth; L++) {   // the other slices: just their averages
            double ls[4] = { 0, 0, 0, 0 };
            u32 nz = 0;
            const u8* lp = px + (size_t)L * n * texel_bytes(s->img.format);
            for (u32 i = 0; i < n; i++) {
                bool any = false;
                for (u32 c = 0; c < 4; c++) { float v = texel_value(s->img.format, lp, i, c); ls[c] += v; if (c < 3 && v != 0) any = true; }
                nz += any;
            }
            printf("[dump]    slice %u: avg %.4g %.4g %.4g %.4g, %u texels not zero\n", L, ls[0] / n, ls[1] / n, ls[2] / n, ls[3] / n, nz);
        }
        {
            float dlo = lo[0], dhi = hi[0];
            if (s->depth) {   // stretch, ignoring the cleared "far" value
                dhi = -1e30f;
                for (u32 i = 0; i < n; i++) { float v = fv[i * 4]; if (v < 0.999999f && v > dhi) dhi = v; }
                if (dhi <= dlo) dhi = dlo + 1e-6f;
            }
            for (u32 i = 0; i < n; i++) {
                for (u32 c = 0; c < 3; c++) {
                    float v = fv[i * 4 + (s->depth ? 0 : c)];
                    if (s->depth) v = v >= 0.999999f ? 1.0f : (v - dlo) / (dhi - dlo) * 0.9f;
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    rgba[i * 4 + c] = (u8)(v * 255.0f + 0.5f);
                }
                rgba[i * 4 + 3] = 255;
            }
            char path[160];
            snprintf(path, sizeof(path), "%s_%08X_%ux%u_%03X%s.png", prefix, s->addr, s->w, s->h, s->gx2fmt, s->depth ? "_depth" : "");
            write_png(path, rgba, s->w, s->h);
        }
        free(rgba);
        free(fv);
        free(px);
    }
}

// (timed, for the [spike] line's texture breakdown)
static bool upload_level(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 level) {
    u64 t0 = pf_now();
    bool ok = upload_level_(s, d, tf, is3d, level);
    g_tex_ns[3] += pf_now() - t0;
    return ok;
}
static bool upload_texture(Surf* s, const LatteTexDesc* d, const LatteTexFormat* tf, bool is3d, u32 dim) {
    u64 t0 = pf_now();
    bool ok = upload_texture_(s, d, tf, is3d, dim);
    g_tex_ns[4] += pf_now() - t0;
    return ok;
}

// The boot stall (Oct 25): the game's GX2DrawDone timed out -- what is every thread of ours waiting for?
// (called from the game's thread, gx2.c; touches nothing that could block it except a try-lock)
void latte_render_report_waits(FILE* out) {
    extern volatile u64 latte_cp_polls, latte_cp_packets, latte_cp_cur_header, latte_cp_idle_waits, latte_cp_cur_inner, latte_cp_inner_packets;
    u64 now = hnow_ns();
    fprintf(out, "[stall] drawing thread: packets %llu, idle waits %llu, partial-packet polls %llu, packet being processed %08X (inside a display list: %08X, %llu list packets so far); frame %llu, draws %u; builds on helpers %llu, by the drawing thread %u\n",
            (unsigned long long)latte_cp_packets, (unsigned long long)latte_cp_idle_waits, (unsigned long long)latte_cp_polls, (unsigned)latte_cp_cur_header,
            (unsigned)latte_cp_cur_inner, (unsigned long long)latte_cp_inner_packets,
            (unsigned long long)g.frame, g.draws, (unsigned long long)hatomic_load64(&g_async_built), g_now_builds);
    u64 n = hatomic_load64(&g_ws_n);
    if (n > 64) n = 64;
    int shown = 0;
    for (u64 i = 0; i < n; i++) {
        WaitState* w = &g_ws[i];
        for (int k = 0; k < 2; k++) {
            const char* what = w->what[k];
            volatile uint64_t* p = w->p[k];
            if (!what) continue;
            fprintf(out, "[stall]   thread %lu waits for %s: pending %llu, for %.1f s\n", w->tid, what, p ? (unsigned long long)hatomic_load64(p) : 0ull, (now - w->since[k]) / 1e9);
            shown++;
        }
    }
    if (!shown) fprintf(out, "[stall]   no thread of ours is in a build wait\n");
    { const char* st = (const char*)g_wp_step; if (st) fprintf(out, "[stall]   drawing thread in the start-up picture: %s, for %.1f s\n", st, (now - g_wp_since) / 1e9); }
    fprintf(out, "[stall]   presents: %u shown, %u frames not shown (no free image, or one Windows hasn't released); an acquired image waiting %d (unreleased for %u frames)\n",
            g_vk.presented, g_present_skips, g_acq.have, g_acq.not_yet);
#ifdef _WIN32
    if (TryEnterCriticalSection(&g_aq_lock)) {
        u32 qa = 0, qd = 0;
        for (AJob* j = g_aq_head; j; j = j->next) qa++;
        for (AJob* j = g_dq_head; j; j = j->next) qd++;
        u32 blocked = g_draw_blocked;
        LeaveCriticalSection(&g_aq_lock);
        fprintf(out, "[stall]   helper queues: %u background jobs, %u urgent jobs waiting; drawing-thread-blocked count %u\n", qa, qd, blocked);
    } else fprintf(out, "[stall]   helper queue lock is held by someone\n");
#endif
}
