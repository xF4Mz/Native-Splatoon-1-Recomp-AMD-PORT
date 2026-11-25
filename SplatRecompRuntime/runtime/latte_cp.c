// The GPU thread (see latte_cp.h).
//
// Packets come from two places: the main stream (a ring of words in PC
// memory, filled by gx2_cmd.c) and display lists in the game's memory
// (stored big-endian like on the console), which the stream "calls" with
// INDIRECT_BUFFER packets. Each packet is copied into a small local buffer in
// PC byte order and then handled the same way whichever place it came from.
//
// Register state "shadowing": GX2 context states are blocks of game memory
// that mirror the registers. LOAD_* packets load registers from such a block
// and remember where each register lives in it; while shadowing is on,
// every register write is copied into the block too.
#include "latte_cp.h"
#include "profiler.h"
#include "latte_render.h"
#include "latte_capture.h"
#include "gx2_cmd.h"
#include "host_thread.h"
#include "wiiu_mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 g_latte_regs[REG_COUNT];
static u32* g_shadow_addr;          // per register: where it's mirrored (0 = nowhere)
static u32 g_context_control = 0;   // CONTEXT_CONTROL word 0 (0x80000077 = shadowing on)
static volatile u64 g_frames_done = 0;
static bool g_started = false;

static u32 g_index_type = LATTE_INDEX_U32_BE;
static u32 g_instances = 1;
static bool g_dry;   // the look-ahead pass is reading (latte_cp_lookahead): registers only, nothing written to game memory
// (in that pass the writes into the game's context-state copies go here instead, and loads read here first:
// a state the frame saves and loads again later must come back as the frame left it)
#define DRY_OV_SIZE 65536u
static struct { u32 addr, val, gen; } g_dry_ov[DRY_OV_SIZE];
static u32 g_dry_gen = 1;
static void dry_ov_put(u32 addr, u32 v) {
    u32 i = (addr >> 2) * 2654435761u & (DRY_OV_SIZE - 1);
    for (u32 n = 0; n < 64; n++, i = (i + 1) & (DRY_OV_SIZE - 1))
        if (g_dry_ov[i].gen != g_dry_gen || g_dry_ov[i].addr == addr) { g_dry_ov[i].addr = addr; g_dry_ov[i].val = v; g_dry_ov[i].gen = g_dry_gen; return; }
}
static u32 dry_read32(u32 addr) {
    u32 i = (addr >> 2) * 2654435761u & (DRY_OV_SIZE - 1);
    for (u32 n = 0; n < 64; n++, i = (i + 1) & (DRY_OV_SIZE - 1)) {
        if (g_dry_ov[i].gen != g_dry_gen) break;
        if (g_dry_ov[i].addr == addr) return g_dry_ov[i].val;
    }
    return wiiu_read32(addr);
}

u32 latte_cp_frames_done(void) { return (u32)hatomic_load64(&g_frames_done); }

static inline u32 be32(u32 v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
// n big-endian words from game memory into host order: four at a time (the
// display lists' ~330,000 words a frame in the busy plaza, one by one before)
#include <tmmintrin.h>
static inline void be32_copy(u32* dst, const u8* src, u32 n) {
    const __m128i sh = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    u32 k = 0;
    for (; k + 4 <= n; k += 4)
        _mm_storeu_si128((__m128i*)(dst + k), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(src + k * 4)), sh));
    for (; k < n; k++) { u32 w; memcpy(&w, src + k * 4, 4); dst[k] = be32(w); }
}

// --- Registers -------------------------------------------------------------------------

static void set_regs(u32 base, const u32* d, u32 n) {
    if (n < 1) return;
    u32 reg = base + d[0];
    u32 count = n - 1;
    if (reg >= REG_COUNT) return;
    if (reg + count > REG_COUNT) count = REG_COUNT - reg;
    const u32* v = d + 1;
    if (g_context_control == 0x80000077 && g_dry) {
        for (u32 i = 0; i < count; i++) {
            u32 sa = g_shadow_addr[reg + i];
            if (sa) dry_ov_put(sa, v[i]);
            g_latte_regs[reg + i] = v[i];
        }
    } else if (g_context_control == 0x80000077) {
        // (the game's saved copy of the registers, its context state: written
        // straight into game memory -- a call per word was ~5% of this thread)
        extern u8* wiiu_fastmem;
        u8* fm = wiiu_fastmem;
        for (u32 i = 0; i < count; i++) {
            u32 sa = g_shadow_addr[reg + i];
            if (sa) {
                if (fm) { u32 b = be32(v[i]); memcpy(fm + sa, &b, 4); }
                else wiiu_write32(sa, v[i]);
            }
            g_latte_regs[reg + i] = v[i];
        }
    } else {
        memcpy(&g_latte_regs[reg], v, count * 4);
    }
}

// (shadow block address, wait, then pairs of register offset / count)
static void load_regs(u32 base, const u32* d, u32 n) {
    if (n < 2) return;
    u32 addr = d[0];
    extern u8* wiiu_fastmem;
    for (u32 i = 2; i + 1 < n; i += 2) {
        u32 reg = base + d[i], count = d[i + 1];
        if (reg >= REG_COUNT) { addr += count * 4; continue; }
        u32 c = count < REG_COUNT - reg ? count : REG_COUNT - reg;
        if (!g_dry && wiiu_fastmem && (u64)addr + c * 4 <= 0x100000000ull) {   // (a block at once: the usual case)
            be32_copy(&g_latte_regs[reg], wiiu_fastmem + addr, c);
            for (u32 k = 0; k < c; k++) g_shadow_addr[reg + k] = addr + k * 4;
            addr += count * 4;
            continue;
        }
        for (u32 k = 0; k < count && reg < REG_COUNT; k++, reg++, addr += 4) {
            g_shadow_addr[reg] = addr;
            g_latte_regs[reg] = g_dry ? dry_read32(addr) : wiiu_read32(addr);
        }
    }
}

// --- Packets -------------------------------------------------------------------------------

static void run_display_list(u32 addr, u32 words, int depth);

static bool make_draw(LatteDraw* dr, const u32* d, u32 n, u32 op) {
    memset(dr, 0, sizeof(*dr));
    dr->prim = g_latte_regs[VGT_PRIMITIVE_TYPE];
    dr->instances = g_instances ? g_instances : 1;
    dr->index_type = g_index_type;
    if (op == IT_DRAW_INDEX_2) {
        if (n < 4) return false;
        dr->index_addr = d[1];
        dr->count = d[3];
    } else if (op == IT_DRAW_INDEX_AUTO) {
        if (n < 1) return false;
        dr->count = d[0];
    } else {   // IT_DRAW_INDEX_IMMD
        if (n < 2) return false;
        dr->count = d[0];
        dr->immediate = d + 2;
        dr->immediate_words = n - 2;
    }
    return dr->count != 0;
}
static void draw(u32 prim_unused, const u32* d, u32 n, u32 op) {
    (void)prim_unused;
    LatteDraw dr;
    if (!make_draw(&dr, d, n, op)) return;
    if (!g_cap_active && latte_render_early_capture()) latte_render_begin_capture();
    if (g_cap_active) cap_regs_diff();
    latte_render_draw(&dr);
    if (g_cap_active) {
        u32 w[6 + 0x4000];
        u32 imm = dr.immediate ? (dr.immediate_words < 0x4000 ? dr.immediate_words : 0x4000) : 0;
        w[0] = dr.prim; w[1] = dr.count; w[2] = dr.instances; w[3] = dr.index_type; w[4] = dr.index_addr; w[5] = imm;
        if (imm) memcpy(w + 6, dr.immediate, imm * 4);
        cap_event(CAPE_DRAW, w, 6 + imm);
    }
}

// Time spent in packets that aren't draws (register writes, and everything
// else: syncs, copies, fences, queries), in CPU ticks; the [perf] line
// converts them (what the drawing thread does besides drawing).
#include <intrin.h>
u64 g_cp_tsc[2];
u64 g_cp_count[3];   // (register packets, other packets, display-list words: for the [perf] line)
static bool g_cp_timed = true;   // (packets are timed in every 16th frame, counted x16: see IT_HLE_SWAP)
static inline bool op_is_regs(u32 op) { return op >= IT_SET_CONFIG_REG && op <= IT_SET_CTL_CONST; }
// Handle one packet. `d` points at its data words (host order), n of them.
volatile u64 latte_cp_polls, latte_cp_packets, latte_cp_cur_header, latte_cp_idle_waits, latte_cp_cur_inner, latte_cp_inner_packets;   // (latte_render_report_waits: the boot stall)
static void packet_(u32 header, const u32* d, u32 n, int depth);
static void packet(u32 header, const u32* d, u32 n, int depth) {
    u32 op = (header >> 8) & 0xFF;
    if (op == IT_DRAW_INDEX_2 || op == IT_DRAW_INDEX_AUTO || op == IT_DRAW_INDEX_IMMD || op == IT_HLE_SWAP || op == IT_HLE_FENCE || op == IT_INDIRECT_BUFFER_PRIV) {
        packet_(header, d, n, depth);   // (timed by their own buckets, or nested lists)
        return;
    }
    g_cp_count[op_is_regs(op) ? 0 : 1]++;
    if (!g_cp_timed) { packet_(header, d, n, depth); return; }
    u64 t0 = __rdtsc();
    packet_(header, d, n, depth);
    g_cp_tsc[op_is_regs(op) ? 0 : 1] += (__rdtsc() - t0) * 16;
}
static void packet_(u32 header, const u32* d, u32 n, int depth) {
    u32 op = (header >> 8) & 0xFF;
    switch (op) {
    case IT_NOP: break;
    case IT_SET_CONFIG_REG: set_regs(REG_BASE_CONFIG, d, n); break;
    case IT_SET_CONTEXT_REG: set_regs(REG_BASE_CONTEXT, d, n); break;
    case IT_SET_ALU_CONST: set_regs(REG_BASE_ALU_CONST, d, n); break;
    case IT_SET_LOOP_CONST: set_regs(REG_BASE_LOOP_CONST, d, n); break;
    case IT_SET_RESOURCE: set_regs(REG_BASE_RESOURCE, d, n); break;
    case IT_SET_SAMPLER: set_regs(REG_BASE_SAMPLER, d, n); break;
    case IT_SET_CTL_CONST: set_regs(REG_BASE_CTL_CONST, d, n); break;
    case IT_LOAD_CONFIG_REG: load_regs(REG_BASE_CONFIG, d, n); break;
    case IT_LOAD_CONTEXT_REG: load_regs(REG_BASE_CONTEXT, d, n); break;
    case IT_LOAD_ALU_CONST: load_regs(REG_BASE_ALU_CONST, d, n); break;
    case IT_LOAD_LOOP_CONST: load_regs(REG_BASE_LOOP_CONST, d, n); break;
    case IT_LOAD_RESOURCE: load_regs(REG_BASE_RESOURCE, d, n); break;
    case IT_LOAD_SAMPLER: load_regs(REG_BASE_SAMPLER, d, n); break;
    case IT_CONTEXT_CONTROL: if (n >= 1) g_context_control = d[0]; break;
    case IT_INDIRECT_BUFFER_PRIV:
        if (n >= 3) run_display_list(d[0], d[2], depth + 1);
        break;
    case IT_INDEX_TYPE: if (n >= 1) g_index_type = d[0]; break;
    case IT_NUM_INSTANCES: if (n >= 1) g_instances = d[0]; break;
    case IT_DRAW_INDEX_2:
    case IT_DRAW_INDEX_AUTO:
    case IT_DRAW_INDEX_IMMD:
        draw(0, d, n, op);
        break;
    case IT_MEM_WRITE:   // (address | flags, high address, low word, high word)
        latte_render_mem_write(d, n);
        break;
    case IT_SURFACE_SYNC:   // (GX2 invalidate flags, size >> 8, address >> 8, poll interval)
        if (n >= 3 && (d[0] & 2)) {   // textures changed
            latte_render_invalidate(d[2] << 8, d[1] << 8);
            if (g_cap_active) { u32 w[2] = { d[2] << 8, d[1] << 8 }; cap_event(CAPE_INVALIDATE, w, 2); }
        }
        break;
    case IT_STRMOUT_BUFFER_UPDATE:   // (control, write address, -, read address, -)
        if (n >= 5) {
            u32 mode = (d[0] >> 1) & 3, i = (d[0] >> 8) & 3;
            u32 reg = VGT_STRMOUT_BUFFER_SIZE_0 + 3 + i * 4;   // buffer offset
            if (mode == 0) g_latte_regs[reg] = 0;                        // start over
            else if (mode == 2) g_latte_regs[reg] = wiiu_read32(d[3]);  // continue from a saved context
            if (d[0] & 1) wiiu_write32(d[1], g_latte_regs[reg]);        // save the offset for the game
        }
        break;
    case IT_HLE_QUERY: if (n >= 2) { latte_render_query(d[0], d[1]); cap_event(CAPE_QUERY, d, 2); } break;
    case IT_HLE_RT_INFO: if (n >= 8) { latte_render_rt_info(d); cap_event(CAPE_RT_INFO, d, 8); } break;
    case IT_HLE_CLEAR: if (n >= 17) { cap_regs_diff(); latte_render_clear(d); cap_event(CAPE_CLEAR, d, 17); } break;
    case IT_HLE_COPY_TO_SCAN: if (n >= 6) { latte_render_copy_to_scan(d); cap_event(CAPE_COPY_TO_SCAN, d, 6); } break;
    case IT_HLE_COPY_SURFACE: if (n >= 12) { latte_render_copy_surface(d, n); cap_event(CAPE_COPY_SURFACE, d, n < 18 ? n : 18); } break;
    case IT_HLE_SWAP:
        latte_render_swap(n >= 1 ? d[0] : 0);
        if (g_cap_active) {   // one frame saved
            u32 w = n >= 1 ? d[0] : 0;
            cap_event(CAPE_SWAP, &w, 1);
            cap_close();
            if (latte_render_capture_more()) latte_render_begin_capture();
        } else if (latte_render_capture_wanted()) {
            latte_render_begin_capture();
        }
        hatomic_add64(&g_frames_done, 1);
        g_cp_timed = (hatomic_load64(&g_frames_done) & 15) == 0;
        break;
    case IT_HLE_FENCE:
        if (n >= 2) latte_render_fence(((u64)d[0] << 32) | d[1], gx2_cmd_set_retired);
        else latte_render_sync_readbacks();
        break;
    default:   // events, waits, predication, stream out: nothing to do here
        break;
    }
}

// Display lists: big-endian words in game memory.
#define MAX_DEPTH 8
static u32* g_dl_buf[MAX_DEPTH];
// (the look-ahead goes on from here: each list being run and the word after its current packet)
typedef struct { const u8* p; u32 words, next; } DlPos;
static DlPos g_dl_pos[MAX_DEPTH];
static int g_dl_cur = 0;   // depth of the innermost list being run (0: the main stream)

static void run_display_list(u32 addr, u32 words, int depth) {
    if (depth >= MAX_DEPTH || words == 0) return;
    const u8* p = wiiu_mem_ptr(addr, words * 4);
    if (!p) {
        static int warned = 0;
        if (warned++ < 4) printf("[gpu] display list at %08X (%u words) isn't in game memory\n", addr, words);
        return;
    }
    if (!g_dl_buf[depth]) g_dl_buf[depth] = (u32*)malloc(0x4002 * 4);
    u32* buf = g_dl_buf[depth];
    int saved_cur = g_dl_cur;
    g_dl_cur = depth;
    g_dl_pos[depth].p = p;
    g_dl_pos[depth].words = words;
    u32 i = 0;
    while (i < words) {
        u32 header;
        memcpy(&header, p + i * 4, 4);
        header = be32(header);
        u32 type = header >> 30;
        if (type == 2) { i++; continue; }   // filler
        u32 n = ((header >> 16) & 0x3FFF) + 1;
        if (i + 1 + n > words) break;
        be32_copy(buf, p + (i + 1) * 4, n);
        g_dl_pos[depth].next = i + 1 + n;
        g_cp_count[2] += 1 + n;
        latte_cp_cur_inner = header;
        latte_cp_inner_packets++;
        if (type == 3) packet(header, buf, n, depth);
        else if (type == 0) {   // type 0: write n registers starting at the header's register
            u32 reg = header & 0xFFFF;
            for (u32 k = 0; k < n && reg + k < REG_COUNT; k++) g_latte_regs[reg + k] = buf[k];
        }
        i += 1 + n;
    }
    g_dl_cur = saved_cur;
}

// --- Look-ahead --------------------------------------------------------------------------
// With nothing cached (a new player's first run: an empty shader cache and a
// graphics driver that never saw this program), a new scene's draws that
// can't be skipped had their pipelines built by the drawing thread one after
// another, 90-400 ms each: the picture stood still for 4-12 s. When that
// starts, the rest of the frame's commands -- what's left of the display
// lists being run, then the main stream as far as the game has handed it
// over -- is read ahead with a copy of the registers, and every draw's
// shaders and pipeline go to the helper threads at once
// (latte_render_prefetch_draw). Nothing is drawn or written in this pass;
// the registers, the index/instance state and the buffer table are put back.
static u32* g_dry_buf[MAX_DEPTH];
static u32 g_regs_save[REG_COUNT], g_shadow_save[REG_COUNT];
static u32 g_dry_draws, g_dry_max;
static bool g_dry_stop;
static void dry_list(const u8* p, u32 words, u32 start, int depth);
static void dry_regs0(u32 header, const u32* d, u32 n) {
    u32 reg = header & 0xFFFF;
    for (u32 k = 0; k < n && reg + k < REG_COUNT; k++) g_latte_regs[reg + k] = d[k];
}
static void dry_packet(u32 header, const u32* d, u32 n, int depth) {
    u32 op = (header >> 8) & 0xFF;
    switch (op) {
    case IT_SET_CONFIG_REG: set_regs(REG_BASE_CONFIG, d, n); break;
    case IT_SET_CONTEXT_REG: set_regs(REG_BASE_CONTEXT, d, n); break;
    case IT_SET_ALU_CONST: set_regs(REG_BASE_ALU_CONST, d, n); break;
    case IT_SET_LOOP_CONST: set_regs(REG_BASE_LOOP_CONST, d, n); break;
    case IT_SET_RESOURCE: set_regs(REG_BASE_RESOURCE, d, n); break;
    case IT_SET_SAMPLER: set_regs(REG_BASE_SAMPLER, d, n); break;
    case IT_SET_CTL_CONST: set_regs(REG_BASE_CTL_CONST, d, n); break;
    case IT_LOAD_CONFIG_REG: load_regs(REG_BASE_CONFIG, d, n); break;
    case IT_LOAD_CONTEXT_REG: load_regs(REG_BASE_CONTEXT, d, n); break;
    case IT_LOAD_ALU_CONST: load_regs(REG_BASE_ALU_CONST, d, n); break;
    case IT_LOAD_LOOP_CONST: load_regs(REG_BASE_LOOP_CONST, d, n); break;
    case IT_LOAD_RESOURCE: load_regs(REG_BASE_RESOURCE, d, n); break;
    case IT_LOAD_SAMPLER: load_regs(REG_BASE_SAMPLER, d, n); break;
    case IT_CONTEXT_CONTROL: if (n >= 1) g_context_control = d[0]; break;
    case IT_INDIRECT_BUFFER_PRIV:
        if (n >= 3 && depth + 1 < MAX_DEPTH && d[2]) {
            const u8* q = wiiu_mem_ptr(d[0], d[2] * 4);
            if (q) dry_list(q, d[2], 0, depth + 1);
        }
        break;
    case IT_INDEX_TYPE: if (n >= 1) g_index_type = d[0]; break;
    case IT_NUM_INSTANCES: if (n >= 1) g_instances = d[0]; break;
    case IT_DRAW_INDEX_2:
    case IT_DRAW_INDEX_AUTO:
    case IT_DRAW_INDEX_IMMD: {
        LatteDraw dr;
        if (make_draw(&dr, d, n, op)) latte_render_prefetch_draw(&dr);
        if (++g_dry_draws >= g_dry_max) g_dry_stop = true;
        break;
    }
    case IT_HLE_RT_INFO: if (n >= 8) latte_render_rt_info(d); break;   // (the buffer table is put back afterwards)
    case IT_HLE_SWAP: g_dry_stop = true; break;                       // (the frame ends here)
    default: break;   // copies, clears, fences, queries, memory writes, stream-out offsets: not in this pass
    }
}
static void dry_list(const u8* p, u32 words, u32 start, int depth) {
    if (depth >= MAX_DEPTH) return;
    if (!g_dry_buf[depth]) g_dry_buf[depth] = (u32*)malloc(0x4002 * 4);
    u32* buf = g_dry_buf[depth];
    u32 i = start;
    while (i < words && !g_dry_stop) {
        u32 header;
        memcpy(&header, p + i * 4, 4);
        header = be32(header);
        u32 type = header >> 30;
        if (type == 2) { i++; continue; }
        u32 n = ((header >> 16) & 0x3FFF) + 1;
        if (i + 1 + n > words) break;
        be32_copy(buf, p + (i + 1) * 4, n);
        if (type == 3) dry_packet(header, buf, n, depth);
        else if (type == 0) dry_regs0(header, buf, n);
        i += 1 + n;
    }
}
u32 latte_cp_lookahead(u32 max_draws) {
    memcpy(g_regs_save, g_latte_regs, sizeof(g_regs_save));
    memcpy(g_shadow_save, g_shadow_addr, sizeof(g_shadow_save));
    u32 index_type = g_index_type, instances = g_instances, context_control = g_context_control;
    g_dry_gen++;
    latte_render_rti_save();
    g_dry = true;
    g_dry_draws = 0;
    g_dry_max = max_draws;
    g_dry_stop = false;
    // the rest of each display list being run, innermost first...
    for (int dpt = g_dl_cur; dpt >= 1 && !g_dry_stop; dpt--) dry_list(g_dl_pos[dpt].p, g_dl_pos[dpt].words, g_dl_pos[dpt].next, dpt);
    // ...then the main stream, as far as the game has handed it over
    if (!g_dry_buf[0]) g_dry_buf[0] = (u32*)malloc(0x4002 * 4);
    u32 avail = gx2_ring_available(), off = 0;
    while (off < avail && !g_dry_stop) {
        u32 header = gx2_ring_peek(off);
        u32 type = header >> 30;
        if (type == 2) { off++; continue; }
        u32 n = ((header >> 16) & 0x3FFF) + 1;
        if (off + 1 + n > avail) break;
        for (u32 k = 0; k < n; k++) g_dry_buf[0][k] = gx2_ring_peek(off + 1 + k);
        if (type == 3) dry_packet(header, g_dry_buf[0], n, 0);
        else if (type == 0) dry_regs0(header, g_dry_buf[0], n);
        off += 1 + n;
    }
    g_dry = false;
    memcpy(g_latte_regs, g_regs_save, sizeof(g_regs_save));
    memcpy(g_shadow_addr, g_shadow_save, sizeof(g_shadow_save));
    g_index_type = index_type;
    g_instances = instances;
    g_context_control = context_control;
    latte_render_rti_restore();
    return g_dry_draws;
}

// --- The main stream ---------------------------------------------------------------------

static u32 g_ring_buf[0x4002];

static int gpu_thread(void* arg) {
    (void)arg;
    profiler_register_gpu_thread();
    latte_render_thread_start();
    u32 idle_spins = 0, packets = 0;
    for (;;) {
        u32 avail = gx2_ring_available();
        if (avail == 0) {
            latte_cp_idle_waits++;
            if (++idle_spins > 64) {
                u64 t0 = hnow_ns();
                // (with a copy or fence queued it waited on the card's fence
                // already; the game may be waiting for that, so no nap then)
                if (!latte_render_idle()) gx2_ring_wait(1);   // (woken by the next flush; 1 ms at most)
                latte_render_note_idle(hnow_ns() - t0);
            }
            continue;
        }
        idle_spins = 0;
        if ((++packets & 127) == 0) latte_render_poll();
        u32 header = gx2_ring_peek(0);
        u32 type = header >> 30;
        if (type == 2) { gx2_ring_consume(1); continue; }
        u32 n = ((header >> 16) & 0x3FFF) + 1;
        if (avail < 1 + n) {   // packet not complete yet
            latte_cp_polls++;
            hsleep_ns(20000);
            continue;
        }
        gx2_ring_read(1, n, g_ring_buf);
        gx2_ring_consume(1 + n);
        // The swap is always followed by the game's fence (GX2SwapScanBuffers
        // flushes right after it), and the game waits for that fence at once
        // (GX2DrawDone) while this thread presents the picture: ~0.5 ms of
        // every frame. Everything the game can touch was handled before the
        // swap packet, and the present only reads the card's own images, so
        // the fence is retired first. SPLAT_SWAP_FENCE_LATE=1: as before.
        // (GX2DrawDone adds a fence of its own right after the swap's: both
        // are taken, waiting up to 0.2 ms for them to be written.)
        if (type == 3 && ((header >> 8) & 0xFF) == IT_HLE_SWAP) {
            static int late = -1;
            if (late < 0) late = getenv("SPLAT_SWAP_FENCE_LATE") != NULL;
            u64 t0 = late ? 0 : hnow_ns();
            int taken = 0;
            while (!late) {
                if (gx2_ring_available() >= 3) {
                    u32 h2 = gx2_ring_peek(0);
                    if ((h2 >> 30) == 3 && ((h2 >> 8) & 0xFF) == IT_HLE_FENCE && (((h2 >> 16) & 0x3FFF) + 1) == 2) {
                        u32 f[2] = { gx2_ring_peek(1), gx2_ring_peek(2) };
                        gx2_ring_consume(3);
                        packet(h2, f, 2, 0);
                        taken++;
                        if (taken >= 2) break;
                        continue;
                    }
                    if (gx2_ring_available() >= 1 + (((h2 >> 16) & 0x3FFF) + 1)) break;   // (a whole other packet is there: the fences are done)
                }
                if (hnow_ns() - t0 > 200000) break;
                for (int i = 0; i < 32; i++) YieldProcessor();
            }
        }
        latte_cp_cur_header = header;
        latte_cp_packets++;
        if (type == 3) packet(header, g_ring_buf, n, 0);
        else if (type == 0) {
            u32 reg = header & 0xFFFF;
            for (u32 k = 0; k < n && reg + k < REG_COUNT; k++) g_latte_regs[reg + k] = g_ring_buf[k];
        } else {
            static int warned = 0;
            if (warned++ < 4) printf("[gpu] unknown packet %08X\n", header);
        }
        latte_cp_cur_header = 0;
    }
    return 0;
}

void latte_cp_start(void) {
    if (g_started) return;
    g_started = true;
    g_shadow_addr = (u32*)calloc(REG_COUNT, sizeof(u32));
    g_latte_regs[VGT_PRIMITIVE_TYPE] = PRIM_TRIANGLES;
    static hthrd_t thread;
    if (!hthrd_create(&thread, gpu_thread, NULL, 16u << 20))
        fprintf(stderr, "[gpu] couldn't start the GPU thread\n");
    else
        printf("[gpu] GPU thread started\n");
}
