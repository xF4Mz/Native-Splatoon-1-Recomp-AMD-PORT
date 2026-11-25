// The built-in benchmark (SPLAT_BENCH=1, the launcher's Benchmark button): the game plays itself through every map --
// the plaza, Octo Valley, a story level and the battle maps (through the Battle Dojo, with player 2 scripted and the
// Dojo's first stage slot pointed at each map in turn in the game's own map list) -- and on each one flies the free
// camera on a slow sweep for a fixed time while every frame is counted: average fps, the slowest 1% and 0.1%, late
// frames by cause (the stutter tracker), the graphics card's time per frame, memory. At the end a results panel in
// the window and a text file in SplatRecompRuntime\benchmarks\. Nothing is touched on disk: the map list is patched in
// the game's memory after it read Static.pack (fs.c tells us where).
// SPLAT_BENCH_SECS=n: seconds per map (25). SPLAT_BENCH_ALL=1: the two maps that have crashed the graphics driver
// (Hammerhead Bridge, Ancho-V Games) too. SPLAT_BENCH_ONLY=<scene>: one map (e.g. Plaza00) for tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "hle.h"
#include "vk_ctx.h"
#include "wiiu_mem.h"
#include "frame_tracker.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
static u32 mem_in_use_mb(void) { PROCESS_MEMORY_COUNTERS pm; pm.cb = sizeof(pm); return GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm)) ? (u32)(pm.WorkingSetSize >> 20) : 0; }
#else
static u32 mem_in_use_mb(void) { return 0; }
#endif

extern char splat_scene_map[64];
extern volatile unsigned splat_scene_seq;
void input_live_push(const char* line);
void splat_freecam_place(const float pos[3], float yaw, float pitch);
void splat_freecam_off(void);
void splat_player_pos(float pos[3], int* fresh);
void splat_emote_camera_now(float pos[3], float dir[3], int* fresh);
u32 gx2_frame_count(void);

static int g_on = -1;
static bool bench_on(void) {
    if (g_on < 0) { const char* e = getenv("SPLAT_BENCH"); g_on = e && atoi(e) != 0; }
    return g_on == 1;
}
bool splat_bench_active(void) { return bench_on(); }   // (input.c: the real controller / keyboard don't steer the game while it runs)
static char g_msg[96];      // (a line for the screen for a few seconds: "go to the plaza first")
static double g_msg_until;
static double g_now_s;

// --- the map list in the game's memory -------------------------------------------------------------------------
static u32 g_pack_addr, g_pack_size;
static bool g_pack_parsed;
typedef struct { int id; u32 ent; char name[40]; } MapEnt;   // ent: the entry's hash node in game memory
static MapEnt g_maps[200];
static int g_nmaps;
static u32 g_byml, g_keys, g_strs;   // the BYML, its key and string tables (game addresses)

void splat_bench_pack_read(u32 addr, u32 off, u32 bytes) {   // (fs.c: a read of Static.pack into game memory)
    extern int splat_lobby_on(void);
    if ((!bench_on() && !splat_lobby_on()) || !bytes) return;   // (the Bot Lobby swaps the Dojo's map too)
    printf("[bench] Static.pack read: %u bytes at file offset %u into %08X\n", bytes, off, addr);
    if (off == 0) { g_pack_addr = addr; g_pack_size = bytes; g_pack_parsed = false; g_nmaps = 0; }
    else if (g_pack_addr && addr == g_pack_addr + off) g_pack_size = off + bytes;   // (read in pieces into one buffer)
}
static u32 rd32(u32 a) { const u8* p = (const u8*)wiiu_mem_ptr(a, 4); return p ? ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3] : 0; }
static u32 rd16(u32 a) { const u8* p = (const u8*)wiiu_mem_ptr(a, 2); return p ? ((u32)p[0] << 8) | p[1] : 0; }
static u32 rd8(u32 a) { const u8* p = (const u8*)wiiu_mem_ptr(a, 1); return p ? p[0] : 0; }
static void wr32(u32 a, u32 v) { u8* p = (u8*)wiiu_mem_ptr(a, 4); if (!p) return; p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; wiiu_mem_mark(a, 4); }
static void str_at(u32 a, char* out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { u32 c = rd8(a + (u32)i); if (!c) break; out[i] = (char)c; }
    out[i] = 0;
}
static void tab_str(u32 tab, u32 idx, char* out, int max) {   // (a BYML string table's idx-th string)
    u32 cnt = rd32(tab) & 0xFFFFFF;
    if (idx >= cnt) { out[0] = 0; return; }
    str_at(tab + rd32(tab + 4 + 4 * idx), out, max);
}
static u32 ent_value(u32 ent, const char* key, u32* type) {   // (the address of the value word of that key in a hash node, 0 if none)
    u32 m = rd32(ent) & 0xFFFFFF;
    for (u32 j = 0; j < m; j++) {
        u32 e = ent + 4 + 8 * j;
        char k[32];
        tab_str(g_keys, rd32(e) >> 8, k, sizeof(k));
        if (!strcmp(k, key)) { if (type) *type = rd8(e + 3); return e + 4; }
    }
    return 0;
}
// The read of Static.pack lands in a staging buffer the game frees again (the archive is taken apart: each file gets its
// own place), so the map list is found where it lives: a scan of the committed guest memory (VirtualQuery: untouched
// pages stay untouched) for a 4-byte pattern at 4-byte steps, with a check at each hit.
static u32 mem_scan_step(const char pat[4], bool (*check)(u32 a), u32 step) {
#ifdef _WIN32
    const u32 bases[2] = { WIIU_DATA_BASE, WIIU_MEM1_BASE }, sizes[2] = { WIIU_DATA_SIZE, WIIU_MEM1_SIZE };
    for (int r = 0; r < 2; r++) {
        u8* host = wiiu_mem_ptr(bases[r], 1);
        if (!host) continue;
        for (u64 off = 0; off < sizes[r];) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(host + off, &mi, sizeof(mi))) break;
            u64 len = (u64)mi.RegionSize - ((u64)(host + off) - (u64)mi.BaseAddress);
            if (off + len > sizes[r]) len = sizes[r] - off;
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const u8* q = host + off;
                for (u64 i = 0; i + 32 <= len; i += step) {
                    if (q[i] != (u8)pat[0] || q[i + 1] != (u8)pat[1] || q[i + 2] != (u8)pat[2] || q[i + 3] != (u8)pat[3]) continue;
                    u32 a = bases[r] + (u32)(off + i);
                    if (check(a)) return a;
                }
            }
            off += len;
        }
    }
#else
    (void)pat; (void)check; (void)step;
#endif
    return 0;
}
static u32 mem_scan(const char pat[4], bool (*check)(u32 a)) { return mem_scan_step(pat, check, 4); }
static bool sarc_check(u32 a) { return rd16(a + 4) == 0x14 && rd16(a + 6) == 0xFEFF && rd32(a + 8) == g_pack_size; }
static bool byml_check(u32 a) {   // (a BYML v1 whose key table holds "MapFileName": MapInfo.byaml)
    u32 keys = rd32(a + 4), strs = rd32(a + 8);
    if (keys < 16 || keys > 0x100000 || strs < keys || strs > 0x400000 || rd8(a + keys) != 0xC2) return false;
    u32 n = rd32(a + keys) & 0xFFFFFF;
    if (n < 2 || n > 400) return false;
    for (u32 i = 0; i < n; i++) { char k[32]; tab_str(a + keys, i, k, sizeof(k)); if (!strcmp(k, "MapFileName")) return true; }
    return false;
}
static bool pack_parse(void) {
    if (g_pack_parsed) return g_nmaps > 0;
    if (!g_pack_addr || g_pack_size < 0x100) return false;
    g_pack_parsed = true;
    u32 b = g_pack_addr, mapinfo = 0;
    if (rd32(b) != 0x53415243u) { b = mem_scan("SARC", sarc_check); if (b) printf("[bench] Static.pack lives at %08X (the read went to %08X)\n", b, g_pack_addr); }
    if (b) {   // (the whole archive: Mush/MapInfo.byaml through its file table)
        u32 hdr = rd16(b + 4), data_off = rd32(b + 0x0C);
        u32 sfat = b + hdr;
        if (rd32(sfat) == 0x53464154u) {
            u32 n = rd16(sfat + 6), names = sfat + 12 + 16 * n + 8;
            for (u32 i = 0; i < n; i++) {
                u32 e = sfat + 12 + 16 * i, attr = rd32(e + 4);
                if (!(attr & 0x01000000u)) continue;
                char nm[64];
                str_at(names + (attr & 0xFFFF) * 4, nm, sizeof(nm));
                if (!strcmp(nm, "Mush/MapInfo.byaml")) { mapinfo = b + data_off + rd32(e + 8); break; }
            }
        }
    }
    if (!mapinfo) {   // (the archive taken apart: the BYML on its own)
        mapinfo = mem_scan("BY\0\1", byml_check);
        if (mapinfo) printf("[bench] MapInfo.byaml lives at %08X on its own (Static.pack was read into %08X and taken apart)\n", mapinfo, g_pack_addr);
    }
    if (!mapinfo || rd16(mapinfo) != 0x4259u) { printf("[bench] Mush/MapInfo.byaml not found in the game's memory\n"); return false; }
    g_byml = mapinfo;
    g_keys = mapinfo + rd32(mapinfo + 4);
    g_strs = mapinfo + rd32(mapinfo + 8);
    u32 root = rd32(mapinfo + 12);
    if (rd8(mapinfo + root) != 0xC0) root = rd32(mapinfo + 16);
    if (rd8(mapinfo + root) != 0xC0) return false;
    u32 cnt = rd32(mapinfo + root) & 0xFFFFFF, vals = mapinfo + root + 4 + ((cnt + 3) & ~3u);
    for (u32 i = 0; i < cnt && g_nmaps < 200; i++) {
        u32 h = rd32(vals + 4 * i), ent = mapinfo + h;
        if (rd8(ent) != 0xC1) continue;
        u32 idv = ent_value(ent, "Id", NULL), nmv = ent_value(ent, "MapFileName", NULL);
        if (!idv || !nmv) continue;
        MapEnt* m = &g_maps[g_nmaps];
        m->id = (int)rd32(idv);
        m->ent = ent;
        tab_str(g_strs, rd32(nmv), m->name, sizeof(m->name));
        g_nmaps++;
    }
    printf("[bench] map list found in memory: %d entries\n", g_nmaps);
    return g_nmaps > 0;
}
// The Battle Dojo's first stage slot (Id 2000) gets every setting of the named map's entry (the map file, lighting,
// music, the GamePad map camera) except its Id -- so the Dojo loads that map.
static u32 str_addr(u32 tab, u32 idx) { return tab + rd32(tab + 4 + 4 * idx); }   // (the idx-th string's address in a BYML string table)
static u32 g_hits[16];
static int g_nhits;
static bool hit_collect(u32 a) { if (g_nhits < 16) g_hits[g_nhits++] = a; return false; }
static int ptr_hits(u32 v) {   // (every 4-aligned big-endian word equal to v in the committed guest memory)
    char pat[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v };
    g_nhits = 0;
    mem_scan(pat, hit_collect);
    return g_nhits;
}
static u32 str_find(u32 tab, const char* s) {   // (the address of that string in a BYML string table, 0 if none)
    u32 n = rd32(tab) & 0xFFFFFF;
    for (u32 i = 0; i < n && i < 100000; i++) { char k[64]; tab_str(tab, i, k, sizeof(k)); if (!strcmp(k, s)) return str_addr(tab, i); }
    return 0;
}
static const char* g_copy_of;
static u32 g_copy_skip;
static bool copy_check(u32 a) {
    if (a == g_copy_skip) return false;
    size_t n = strlen(g_copy_of);
    const u8* p = wiiu_mem_ptr(a, (u32)n + 1);
    if (p && !memcmp(p, g_copy_of, n + 1)) { if (g_nhits < 16) g_hits[g_nhits++] = a; }
    return false;
}
static void dump_around(u32 at) {
    u32 a = (at - 96) & ~3u;
    for (int row = 0; row < 8; row++) {
        printf("[bench] diag:   %08X:", a + row * 32);
        for (int w = 0; w < 8; w++) printf(" %08X", rd32(a + row * 32 + w * 4));
        printf("\n");
    }
}
static void table_diag(const char* slot_name, const char* next_name, const char* target) {
    const char* names[3] = { slot_name, next_name, target };
    for (int k = 0; k < 3; k++) {
        u32 sa = str_find(g_strs, names[k]);
        if (!sa) { printf("[bench] diag: no string %s\n", names[k]); continue; }
        int n = ptr_hits(sa);
        printf("[bench] diag: \"%s\" is at %08X; %d pointer(s) to it:", names[k], sa, n);
        for (int i = 0; i < n; i++) printf(" %08X", g_hits[i]);
        printf("\n");
        for (int i = 0; i < n && i < 2; i++) dump_around(g_hits[i]);
        g_copy_of = names[k]; g_copy_skip = sa; g_nhits = 0;
        mem_scan_step(names[k], copy_check, 1);
        printf("[bench] diag: %d other cop%s of \"%s\":", g_nhits, g_nhits == 1 ? "y" : "ies", names[k]);
        for (int i = 0; i < g_nhits; i++) printf(" %08X", g_hits[i]);
        printf("\n");
        for (int i = 0; i < g_nhits && i < 3; i++) dump_around(g_hits[i]);
    }
}
// The game parsed MapInfo at boot into its own tables (one entry of 1296 bytes per map with the Id, and the map file name
// in a sead FixedSafeString<64>: self-pointer, vtable, capacity 0x40, then the characters; a second table with the same
// strings). The BYML patch alone changes nothing (the Dojo loaded Urchin Underpass), so the slot's name is rewritten in
// every such string object too: MapFileName is the loading key (the other keys are the same for every battle map or only
// steer the GamePad's map picture).
static char g_slot_now[40];   // (what the Dojo slot's name reads in the game's tables now)
static u32 g_slot_objs[8];    // (the slot's own string objects, found once by the slot's original name -- unique; later
static int g_nslot_objs;      //  swaps rewrite just those, never the genuine entry of a map that was in the slot before)
static bool table_swap(const char* from, const char* to) {
    if (!*from || strlen(to) > 62) return false;
    if (!g_nslot_objs) {
        g_copy_of = from; g_copy_skip = 0; g_nhits = 0;
        mem_scan_step(from, copy_check, 1);
        for (int i = 0; i < g_nhits && g_nslot_objs < 8; i++) {
            u32 h = g_hits[i];
            if (rd32(h - 0xC) == h && rd32(h - 4) == 0x40) g_slot_objs[g_nslot_objs++] = h;   // (a FixedSafeString<64>)
        }
        if (!g_nslot_objs) { printf("[bench] map table: no string object holds \"%s\" (%d copies seen)\n", from, g_nhits); return false; }
    }
    int done = 0;
    for (int i = 0; i < g_nslot_objs; i++) {
        u32 h = g_slot_objs[i];
        u8* b = wiiu_mem_ptr(h, 64);
        if (!b || strncmp((const char*)b, from, 64) != 0) { printf("[bench] map table: %08X no longer reads \"%s\"\n", h, from); continue; }
        memset(b, 0, 64);
        memcpy(b, to, strlen(to));
        wiiu_mem_mark(h, 64);
        printf("[bench] map table: %08X \"%s\" -> \"%s\"\n", h, from, to);
        done++;
    }
    return done > 0;
}
static bool map_swap(int slot_id, const char* name) {
    if (!pack_parse()) return false;
    const MapEnt* slot = NULL;
    const MapEnt* tgt = NULL;
    for (int i = 0; i < g_nmaps; i++) { if (g_maps[i].id == slot_id) slot = &g_maps[i]; if (!strcmp(g_maps[i].name, name)) tgt = &g_maps[i]; }
    if (!slot || !tgt) { printf("[bench] map swap: %s not in the list\n", slot ? name : "the Dojo slot"); return false; }
    u32 m = rd32(slot->ent) & 0xFFFFFF, copied = 0;
    for (u32 j = 0; j < m; j++) {
        u32 e = slot->ent + 4 + 8 * j, type = rd8(e + 3);
        char k[32];
        tab_str(g_keys, rd32(e) >> 8, k, sizeof(k));
        if (!strcmp(k, "Id") || !strcmp(k, "MsnStageNo") || type == 0xC0 || type == 0xC1) continue;
        u32 ttype = 0, tv = ent_value(tgt->ent, k, &ttype);
        if (!tv || ttype != type) continue;
        wr32(e + 4, rd32(tv));
        copied++;
    }
    printf("[bench] Dojo stage slot %d -> %s (%u settings in the BYML)\n", slot_id, name, copied);
    if (!g_slot_now[0]) snprintf(g_slot_now, sizeof(g_slot_now), "%s", slot->name);
    if (table_swap(g_slot_now, name)) snprintf(g_slot_now, sizeof(g_slot_now), "%s", name);
    return copied > 0;
}

bool splat_dojo_map_swap(const char* vss) { return map_swap(2000, vss); }   // (the Bot Lobby, bot_lobby.c)

// --- the steps -----------------------------------------------------------------------------------------------------
typedef struct { const char* label; const char* scene; const char* vss; bool risky; } BStep;
static const BStep k_steps[] = {
    { "Inkopolis Plaza", "Plaza00", NULL, false },
    { "Urchin Underpass", "Crank01", "Fld_Crank00_Vss", false },   // (the versus map file loads the v2.0 layout, scene Crank01; the Dojo's own is Crank00)
    { "Walleye Warehouse", "Warehouse00", "Fld_Warehouse00_Vss", false },
    { "Saltspray Rig", "SeaPlant00", "Fld_SeaPlant00_Vss", false },
    { "Arowana Mall", "UpDown00", "Fld_UpDown00_Vss", false },
    { "Blackbelly Skatepark", "SkatePark00", "Fld_SkatePark00_Vss", false },
    { "Camp Triggerfish", "Athletic00", "Fld_Athletic00_Vss", false },
    { "Port Mackerel", "Amida00", "Fld_Amida00_Vss", false },
    { "Moray Towers", "Tuzura00", "Fld_Tuzura00_Vss", false },
    { "Bluefin Depot", "Ruins00", "Fld_Ruins00_Vss", false },
    { "Hammerhead Bridge", "Kaisou00", "Fld_Kaisou00_Vss", true },
    { "Flounder Heights", "Jyoheki00", "Fld_Jyoheki00_Vss", false },
    { "Museum d'Alfonsino", "Pivot00", "Fld_Pivot00_Vss", false },
    { "Ancho-V Games", "Office00", "Fld_Office00_Vss", true },
    { "Kelp Dome", "Maze00", "Fld_Maze00_Vss", false },
    { "Octo Valley", "World00", NULL, false },
    { "Octo Valley: Lair of the Octoballs", "EasyClimb00", NULL, false },
};
#define NSTEPS ((int)(sizeof(k_steps) / sizeof(k_steps[0])))
typedef struct { float at; const char* cmd; } Seq;   // (a live-input line at that many seconds into the phase)
// ("@drc1" / "@drc0": the GamePad screen shown / hidden -- its buttons are touched, the measuring is done without it)
// from the plaza into the Battle Dojo and its first stage (player 2 = the scripted Pro Controller): the Dojo button on the
// GamePad's map, both players join (ZL+ZR), ready (A), the stage (A)
static const Seq k_dojo_enter[] = { { 0, "@drc1" }, { 2, "TOUCH:0.618:0.6875" }, { 4, "@drc0" }, { 16, "0:ZL:600,0:ZR:600" }, { 30, "P2:ZL:600,P2:ZR:600" }, { 46, "P2:A:200" }, { 54, "0:A:200" }, { 66, "0:A:200" } };
// the next stage: pause (+), Quit (down, A) -> the Dojo's weapon select; both ready (A), the stage (A)
static const Seq k_dojo_again[] = { { 0, "0:PLUS:300" }, { 2, "0:DOWN:300" }, { 4, "0:A:300" }, { 12, "P2:A:200" }, { 14, "0:A:200" }, { 24, "0:A:200" } };
// out of the Dojo: pause, Quit, then the weapon select's "Plaza" (B)
static const Seq k_dojo_leave[] = { { 0, "0:PLUS:300" }, { 2, "0:DOWN:300" }, { 4, "0:A:300" }, { 12, "0:B:300" }, { 16, "0:B:300" } };
// from the plaza to Octo Valley (the GamePad's "Octo Valley" button), and from there into kettle 02 (a super jump, ZL)
static const Seq k_hub_enter[] = { { 0, "@drc1" }, { 1.5f, "TOUCH:0.62:0.115" }, { 3.5f, "@drc0" } };
static const Seq k_level_enter[] = { { 0, "@drc1" }, { 1.5f, "TOUCH:0.474:0.4375" }, { 3.5f, "@drc0" }, { 13, "0:ZL:1200" } };
#define SEQ(s) s, (int)(sizeof(s) / sizeof(s[0]))

// --- the numbers -----------------------------------------------------------------------------------------------
#define BF_MAX 6000
typedef struct {
    const BStep* step;
    bool done, skipped;
    u32 n, late, by[TR_CAUSES];
    float ms[BF_MAX];
    double sum_ms, sum_gpu, worst;
    u32 mem_mb;
} BResult;
static BResult g_res[NSTEPS];
static int g_nres;
static u32 g_tr_last;
static void frames_collect(BResult* r) {
    u32 head = g_tr_head;
    for (; g_tr_last != head; g_tr_last++) {
        const TrackFrame* f = &g_tr[g_tr_last % TR_N];
        if (f->ms <= 0.0f || f->ms > 2000.0f) continue;
        if (r->n < BF_MAX) r->ms[r->n++] = f->ms;
        r->sum_ms += f->ms;
        r->sum_gpu += f->gpu_ms;
        if (f->ms > r->worst) r->worst = f->ms;
        if (f->cause != TR_OK) { r->late++; r->by[f->cause < TR_CAUSES ? f->cause : 0]++; }
    }
}
static int cmp_desc(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? 1 : x > y ? -1 : 0; }
static float low_fps(const BResult* r, float frac) {   // (the average frame rate over the slowest frac of the frames)
    if (!r->n) return 0.0f;
    float* s = (float*)malloc(r->n * sizeof(float));
    if (!s) return 0.0f;
    memcpy(s, r->ms, r->n * sizeof(float));
    qsort(s, r->n, sizeof(float), cmp_desc);
    u32 k = (u32)(r->n * frac);
    if (k < 1) k = 1;
    double sum = 0;
    for (u32 i = 0; i < k; i++) sum += s[i];
    free(s);
    return sum > 0 ? (float)(1000.0 * k / sum) : 0.0f;
}
static float avg_fps(const BResult* r) { return r->sum_ms > 0 ? (float)(1000.0 * r->n / r->sum_ms) : 0.0f; }

// --- the camera sweep ----------------------------------------------------------------------------------------
static float g_p0[3], g_yaw0;
static float ease(float x) { return x < 0 ? 0 : x > 1 ? 1 : x * x * (3.0f - 2.0f * x); }
static void sweep_begin(void) {
    float p[3], d[3];
    int fresh = 0;
    splat_player_pos(p, &fresh);
    float cp[3];
    int cf = 0;
    splat_emote_camera_now(cp, d, &cf);
    if (!fresh && cf) memcpy(p, cp, 12);
    memcpy(g_p0, p, 12);
    g_yaw0 = cf ? atan2f(d[0], d[2]) : 0.0f;
}
static void sweep_at(float u) {   // u = 0..1 of the measured time
    float pos[3], yaw, pitch;
    float fx = sinf(g_yaw0), fz = cosf(g_yaw0);
    if (u < 0.32f) {              // low, gliding forward at a walking pace
        float s = u / 0.32f;
        pos[0] = g_p0[0] + fx * s * 150.0f; pos[2] = g_p0[2] + fz * s * 150.0f; pos[1] = g_p0[1] + 30.0f + 10.0f * ease(s);
        yaw = g_yaw0; pitch = -0.12f;
    } else if (u < 0.62f) {       // rising, turning half round, looking down over the place
        float s = ease((u - 0.32f) / 0.30f);
        pos[0] = g_p0[0] + fx * (150.0f - s * 60.0f); pos[2] = g_p0[2] + fz * (150.0f - s * 60.0f); pos[1] = g_p0[1] + 40.0f + s * 150.0f;
        yaw = g_yaw0 + s * 3.14159265f; pitch = -0.12f - s * 0.5f;
    } else if (u < 0.80f) {       // looking up at the sky
        float s = ease((u - 0.62f) / 0.18f);
        pos[0] = g_p0[0] + fx * 90.0f; pos[2] = g_p0[2] + fz * 90.0f; pos[1] = g_p0[1] + 190.0f;
        yaw = g_yaw0 + 3.14159265f + s * 1.2f; pitch = -0.62f + s * 1.5f;
    } else {                      // down again, turning back to the start
        float s = ease((u - 0.80f) / 0.20f);
        pos[0] = g_p0[0] + fx * (90.0f - s * 90.0f); pos[2] = g_p0[2] + fz * (90.0f - s * 90.0f); pos[1] = g_p0[1] + 190.0f - s * 155.0f;
        yaw = g_yaw0 + 3.14159265f + 1.2f + s * (2.0f * 3.14159265f - 1.2f - 3.14159265f); pitch = 0.88f - s * 1.0f;
    }
    splat_freecam_place(pos, yaw, pitch);
}

// --- the run ---------------------------------------------------------------------------------------------------
enum { PH_START, PH_ENTER, PH_WAIT, PH_SETTLE, PH_MEASURE, PH_LEAVE, PH_DONE };
static int g_phase = PH_START, g_step = -1, g_seq_i;
static double g_t0, g_secs = 40.0;
static unsigned g_seq_seen;
static const Seq* g_seq;
static int g_seq_n;
static char g_status[96] = "";
static bool g_wrote;
static bool g_in_dojo;

static void results_write(void);
static void results_clear(void);
static void seq_start(const Seq* s, int n) { g_seq = s; g_seq_n = n; g_seq_i = 0; }
static bool seq_run(double t) {   // (the lines due by t seconds into the sequence; true once they're all out)
    while (g_seq_i < g_seq_n && t >= g_seq[g_seq_i].at) {
        const char* c = g_seq[g_seq_i].cmd;
        if (!strcmp(c, "@drc1") || !strcmp(c, "@drc0")) { g_vk.drc_on = c[4] == '1'; g_vk.drc_auto = 0; }
        else input_live_push(c);
        g_seq_i++;
    }
    return g_seq_i >= g_seq_n;
}
static void step_begin(int i, double now) {
    g_step = i;
    g_res[i].step = &k_steps[i];
    g_nres = i + 1;
    g_t0 = now;
    g_seq_seen = splat_scene_seq;
    snprintf(g_status, sizeof(g_status), "BENCHMARK  %s  (loading)", k_steps[i].label);
    const BStep* st = &k_steps[i];
    if (st->vss) {
        map_swap(2000, st->vss);
        if (!g_in_dojo) { seq_start(SEQ(k_dojo_enter)); g_in_dojo = true; }
        else seq_start(SEQ(k_dojo_again));
    } else if (!strcmp(st->scene, "World00")) seq_start(SEQ(k_hub_enter));
    else if (!strcmp(st->scene, "EasyClimb00")) seq_start(SEQ(k_level_enter));
    else seq_start(NULL, 0);
    g_phase = PH_ENTER;
    printf("[bench] step %d/%d: %s\n", i + 1, NSTEPS, st->label);
}
// (the scene that arrived vs the one a step wants: the same name, or the same name before the digits -- Fld_Crank00_Vss
// loads as Crank01)
static bool scene_is(const char* now, const char* want) {
    if (!strcmp(now, want)) return true;
    size_t a = strcspn(now, "0123456789"), b = strcspn(want, "0123456789");
    return a > 0 && a == b && !strncmp(now, want, a);
}
static bool step_wanted(int i) {
    const char* only = getenv("SPLAT_BENCH_ONLY");   // (one scene, or several with commas)
    if (only && *only) {
        const char* s = k_steps[i].scene;
        size_t n = strlen(s);
        for (const char* p = only; (p = strstr(p, s)) != NULL; p += n) if ((p == only || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
        return false;
    }
    if (k_steps[i].risky) { const char* a = getenv("SPLAT_BENCH_ALL"); return a && atoi(a) != 0; }
    return true;
}
static int next_step(int from) {
    for (int i = from + 1; i < NSTEPS; i++) if (step_wanted(i)) return i;
    return -1;
}

// F6 in play (display.c): the benchmark starts from the plaza. Elsewhere it says so and does nothing.
void splat_bench_start(void) {
    if (bench_on() && g_phase != PH_DONE) return;   // (running)
    if (strcmp(splat_scene_map, "Plaza00")) { snprintf(g_msg, sizeof(g_msg), "BENCHMARK (F6): go to the plaza first"); g_msg_until = g_now_s + 4.0; return; }
    g_on = 1;
    g_phase = PH_START;
    g_step = -1; g_nres = 0; g_wrote = false; g_in_dojo = false; results_clear();
    memset(g_res, 0, sizeof(g_res));
    g_msg[0] = 0;
    printf("[bench] started with F6\n");
}
void splat_bench_tick(double now) {
    g_now_s = now;
    if (!bench_on() || g_phase == PH_DONE) return;
    static bool init;
    if (!init) {
        init = true;
        const char* e = getenv("SPLAT_BENCH_SECS");
        if (e && atof(e) > 3) g_secs = atof(e);
        g_vk.hide_hud = 1;
        g_vk.pro_pad = 2;   // (player 2 = the scripted Pro Controller: the Battle Dojo needs two players)
        printf("[bench] benchmark mode: %.0f s per map, %d maps\n", g_secs, NSTEPS);
    }
    switch (g_phase) {
    case PH_START: {   // (through the title screen -- ZL+ZR together, then A -- and the news, until the player can move)
        // The plaza's name arrives while the title screen is still up (the scene loads early), so the presses go on until
        // a nudge of the left stick moves the player: that's the plaza with the player in control.
        static double last = -1.0, ctrl_at, first;
        static int which;
        static bool ctrl;
        if (!first) first = now;
        if (!ctrl) {
            // every 6 s: ZL+ZR, A, a nudge of the stick (0.7 s), A. The nudge must move the player 5-120 units and the
            // player must stand still in the 2 s after it -- twice in a row (the title screen's flying camera picks
            // other Inklings as "the player"; the news keeps the player out of view).
            static int good;
            static float p0[3], p2[3];
            double t = now - last;
            if (last < 0 || t >= 6.0) { last = now; which = 0; input_live_push("0:ZL:400,0:ZR:400"); }
            else if (which == 0 && t >= 1.5) { which = 1; input_live_push("0:A:200"); }
            else if (which == 1 && t >= 3.0) { which = 2; int f = 0; splat_player_pos(p0, &f); if (!f) p0[0] = 1e9f; input_live_push("0:LU:700"); }
            else if (which == 2 && t >= 3.9) { which = 3; int f = 0; splat_player_pos(p2, &f); if (!f) p2[0] = 1e9f; }
            else if (which == 3 && t >= 5.9) {
                which = 4;
                float p3[3];
                int f = 0;
                splat_player_pos(p3, &f);
                float moved = p0[0] < 1e8f && p2[0] < 1e8f ? fabsf(p2[0] - p0[0]) + fabsf(p2[1] - p0[1]) + fabsf(p2[2] - p0[2]) : 0.0f;
                float still = f && p2[0] < 1e8f ? fabsf(p3[0] - p2[0]) + fabsf(p3[1] - p2[1]) + fabsf(p3[2] - p2[2]) : 1e9f;
                bool ok = !strcmp(splat_scene_map, "Plaza00") && moved > 5.0f && moved < 120.0f && still < 2.0f;
                good = ok ? good + 1 : 0;
                printf("[bench] nudge at %.0f s: moved %.1f, then %.1f%s\n", now - first, moved, still, ok ? " (ok)" : "");
                if (good >= 2) { ctrl = true; ctrl_at = now; printf("[bench] the player moves on the stick and stands still otherwise: in the plaza, in control (%.0f s after the start)\n", now - first); }
                else input_live_push("0:A:200");
            }
            if (!ctrl && now - first > 240.0) { ctrl = true; ctrl_at = now; printf("[bench] no sign of the player moving after 240 s: starting anyway\n"); }
        } else if (now - ctrl_at > 5.0) {
            int i = next_step(-1);
            if (i < 0) { g_phase = PH_DONE; break; }
            step_begin(i, now);
            if (!k_steps[i].vss && !strcmp(k_steps[i].scene, "Plaza00")) { g_phase = PH_SETTLE; g_t0 = now - 6.0; }
        }
        break;
    }
    case PH_ENTER:
        if (seq_run(now - g_t0)) g_phase = PH_WAIT;
        break;
    case PH_WAIT:   // (the scene has to arrive: its map name; 120 s and it's skipped)
        { static bool once; if (!once && getenv("SPLAT_BENCH_DIAG") && g_in_dojo && strstr(splat_scene_map, "Plaza") == NULL && *splat_scene_map) { once = true; table_diag("Fld_Crank00_Dul", "Fld_Warehouse00_Dul", k_steps[g_step].vss); } }
        if (scene_is(splat_scene_map, k_steps[g_step].scene) && splat_scene_seq != g_seq_seen) { g_phase = PH_SETTLE; g_t0 = now; }
        else if (now - g_t0 > 150.0) { printf("[bench] %s never loaded: skipped\n", k_steps[g_step].label); g_res[g_step].skipped = true; g_phase = PH_LEAVE; g_t0 = now; }
        break;
    case PH_SETTLE: {   // (loading screens; then the sweep once quickly, unmeasured, so every view's pipelines get built; then
                        // calm -- no builds for 3 s -- before counting. 10 s + up to 40 s.)
        extern u64 latte_builds_so_far(u32* warm_pending);
        static u64 built_last;
        static double built_t, pre_t0;
        static bool pre;
        double t = now - g_t0;
        snprintf(g_status, sizeof(g_status), "BENCHMARK  %s  (%s)", k_steps[g_step].label, t < 10.0 ? "loading" : "warming up");
        if (t < 10.0) { pre = false; break; }
        if (!pre) { pre = true; pre_t0 = now; sweep_begin(); built_t = now; built_last = ~0ull; }
        double u = (now - pre_t0) / 8.0;
        if (u <= 1.0) sweep_at((float)u);
        u32 wl = 0;
        u64 b = latte_builds_so_far(&wl);
        if (b != built_last || wl) { built_last = b; built_t = now; }
        if (u >= 1.0 && (now - built_t > 3.0 || now - pre_t0 > 40.0)) {
            g_phase = PH_MEASURE;
            g_t0 = now;
            g_tr_last = g_tr_head;
            g_vk.drc_on = 0; g_vk.drc_auto = 0;   // (the GamePad pop-up stays off the TV while counting)
            sweep_begin();
            printf("[bench] measuring %s for %.0f s (warm-up took %.0f s)\n", k_steps[g_step].label, g_secs, now - pre_t0);
        }
        break;
    }
    case PH_MEASURE: {
        double t = now - g_t0;
        BResult* r = &g_res[g_step];
        sweep_at((float)(t / g_secs));
        frames_collect(r);
        {   // (SPLAT_BENCH_SHOTS=1, test runs: a screenshot at 5%, 45% and 70% of each map's sweep -> images\frame_*.png)
            static int shot_step = -1, shots;
            static bool want;
            static int asked;
            if (!asked) { asked = 1; const char* e = getenv("SPLAT_BENCH_SHOTS"); want = e && atoi(e) != 0; }
            if (want) {
                if (shot_step != g_step) { shot_step = g_step; shots = 0; }
                const double at[3] = { 0.05, 0.45, 0.70 };
                if (shots < 3 && t >= at[shots] * g_secs) { shots++; input_live_push("SHOT"); }
            }
        }
        snprintf(g_status, sizeof(g_status), "BENCHMARK  %s  %.0f / %.0f s   %.1f fps", k_steps[g_step].label, t, g_secs, avg_fps(r));
        if (t >= g_secs) {
            frames_collect(r);
            r->done = true;
            r->mem_mb = mem_in_use_mb();
            splat_freecam_off();
            printf("[bench] %s: %.1f fps avg, 1%% low %.1f, 0.1%% low %.1f, %u late of %u, card %.1f ms, worst %.0f ms\n", k_steps[g_step].label, avg_fps(r), low_fps(r, 0.01f), low_fps(r, 0.001f), r->late, r->n, r->n ? r->sum_gpu / r->n : 0.0, r->worst);
            g_phase = PH_LEAVE;
            g_t0 = now;
        }
        break;
    }
    case PH_LEAVE: {
        int nx = next_step(g_step);
        const BStep* cur = &k_steps[g_step];
        if (g_in_dojo && (nx < 0 || !k_steps[nx].vss)) {   // (out of the Dojo first: the match quit, the weapon select's Plaza)
            static bool leaving;
            if (!leaving) { leaving = true; seq_start(SEQ(k_dojo_leave)); g_t0 = now; g_seq_seen = splat_scene_seq; snprintf(g_status, sizeof(g_status), "BENCHMARK  (back to the plaza)"); }
            bool out = seq_run(now - g_t0);
            bool there = !strcmp(splat_scene_map, "Plaza00") && splat_scene_seq != g_seq_seen;
            if (out && there && now - g_t0 > 30.0) { g_in_dojo = false; leaving = false; }
            else if (now - g_t0 > 150.0) {
                printf("[bench] the Dojo didn't let go: the maps after it are skipped\n");
                for (int i = nx; nx >= 0 && i < NSTEPS; i++) if (step_wanted(i)) { g_res[i].step = &k_steps[i]; g_res[i].skipped = true; g_nres = i + 1; }
                nx = -1; g_in_dojo = false; leaving = false;
            } else break;
        }
        if (nx < 0) { g_phase = PH_DONE; results_write(); break; }
        if (cur->vss && now - g_t0 < 2.0) break;   // (a breath after the measuring before the pause)
        step_begin(nx, now);
        break;
    }
    default: break;
    }
}

// --- the results: the log, a file, the panel -------------------------------------------------------------------
static char g_lines[40][96];
static int g_nlines;
static void results_clear(void) { g_nlines = 0; }
static void results_write(void) {
    if (g_wrote) return;
    g_wrote = true;
    splat_freecam_off();
    g_nlines = 0;
    snprintf(g_lines[g_nlines++], 96, "BENCHMARK  %.0f s per map   render %s   shaders %s", g_secs,
             g_vk.render_scale == 2 ? "2x" : g_vk.render_scale == 1 ? "1.5x" : g_vk.render_scale == 3 ? "0.5x" : g_vk.render_scale == 4 ? "0.75x" : "1x",
             g_vk.sky_mode >= 2 ? "Storm" : g_vk.sky_mode ? "Vibrant" : "off");
    snprintf(g_lines[g_nlines++], 96, "%-30s %7s %7s %8s %6s %6s", "map", "avg", "1% low", "0.1% low", "late", "card");
    double tot = 0; int nt = 0;
    for (int i = 0; i < g_nres && g_nlines < 38; i++) {
        BResult* r = &g_res[i];
        if (!r->step) continue;
        if (r->skipped || !r->done) { snprintf(g_lines[g_nlines++], 96, "%-30s %s", r->step->label, r->skipped ? "skipped" : "-"); continue; }
        snprintf(g_lines[g_nlines++], 96, "%-30s %7.1f %7.1f %8.1f %6u %5.1fms", r->step->label, avg_fps(r), low_fps(r, 0.01f), low_fps(r, 0.001f), r->late, r->n ? r->sum_gpu / r->n : 0.0);
        tot += avg_fps(r); nt++;
    }
    if (nt) snprintf(g_lines[g_nlines++], 96, "%-30s %7.1f   (%d maps)", "overall", tot / nt, nt);
    snprintf(g_lines[g_nlines++], 96, "(F10 hides this; the file is in the benchmarks folder next to the game)");
#ifdef _WIN32
    CreateDirectoryA("benchmarks", NULL);
#endif
    char path[128];
    time_t tt = time(NULL);
    struct tm* tm = localtime(&tt);
    snprintf(path, sizeof(path), "benchmarks/bench_%04d-%02d-%02d_%02d%02d.txt", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "Splatoon Recomp benchmark -- %04d-%02d-%02d %02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
        fprintf(f, "settings: render scale %d, anti-aliasing %d, shadows %d, sky %d (clouds %d rays %d cloud shadows %d AO %d rain %d), quality %d, %.0f s per map\n",
                g_vk.render_scale, g_vk.aa_mode, g_vk.shadow_quality, g_vk.sky_mode, g_vk.sh_clouds, g_vk.sh_rays, g_vk.sh_cshadow, g_vk.sh_ao, g_vk.sh_rain, g_vk.sh_quality, g_secs);
        for (int i = 0; i < g_nlines; i++) fprintf(f, "%s\n", g_lines[i]);
        fprintf(f, "\nlate frames by cause (per map):\n");
        for (int i = 0; i < g_nres; i++) {
            BResult* r = &g_res[i];
            if (!r->step || !r->done) continue;
            fprintf(f, "%-30s frames %u late %u worst %.0f ms memory %u MB:", r->step->label, r->n, r->late, r->worst, r->mem_mb);
            for (int c = 1; c < TR_CAUSES; c++) if (r->by[c]) fprintf(f, "  %s %u", k_tr_cause[c], r->by[c]);
            fprintf(f, "\n");
        }
        fclose(f);
        printf("[bench] results written to %s\n", path);
    }
    for (int i = 0; i < g_nlines; i++) printf("[bench] %s\n", g_lines[i]);
    snprintf(g_status, sizeof(g_status), "BENCHMARK done");
}
// (latte_render.c: the status line while it runs, the panel afterwards)
const char* splat_bench_status(void) {
    if (g_msg[0] && g_now_s < g_msg_until) return g_msg;
    return bench_on() && g_phase != PH_DONE ? g_status : NULL;
}
// The readout while it runs (latte_render.c draws it top left with a progress bar): what a benchmark shows --
// the frame rate now, the frame and card times, the map and its progress, the settings. progress = the whole run.
int splat_bench_hud(const char*** out, float* progress) {
    static char l[4][112];
    static const char* p[4];
    if (!bench_on() || g_phase == PH_DONE) {   // (no benchmark running: the Bot Lobby's readout, without a bar)
        extern int splat_lobby_hud(const char*** out);
        int n = splat_lobby_hud(out);
        if (n) { *progress = -1.0f; return n; }
    }
    if (!bench_on() || g_phase == PH_DONE || g_phase == PH_START || g_step < 0) return 0;
    const BResult* r = &g_res[g_step];
    float fps_now = 0, ms_now = 0, card_now = 0;
    if (g_phase == PH_MEASURE && r->n) {
        u32 k = r->n < 60 ? r->n : 60;
        double s = 0; for (u32 i = r->n - k; i < r->n; i++) s += r->ms[i];
        fps_now = s > 0 ? (float)(1000.0 * k / s) : 0; ms_now = (float)(s / k);
        card_now = r->n ? (float)(r->sum_gpu / r->n) : 0;
    }
    int total = 0, done = 0;
    for (int i = 0; i < NSTEPS; i++) if (step_wanted(i)) { total++; if (i < g_step) done++; }
    double t = g_now_s - g_t0;
    float frac = g_phase == PH_MEASURE ? (float)(t / g_secs) : g_phase == PH_SETTLE ? 0.0f : 0.0f;
    if (frac > 1) frac = 1;
    *progress = total ? ((float)done + (g_phase == PH_MEASURE ? 0.3f + 0.7f * frac : 0.15f)) / (float)total : 0;
    snprintf(l[0], sizeof(l[0]), "BENCHMARK   %d / %d   %s", done + 1, total, k_steps[g_step].label);
    if (g_phase == PH_MEASURE) snprintf(l[1], sizeof(l[1]), "FPS %5.1f    frame %5.1f ms    card %4.1f ms    %.0f / %.0f s", fps_now, ms_now, card_now, t, g_secs);
    else snprintf(l[1], sizeof(l[1]), "%s", g_phase == PH_SETTLE ? (t < 10.0 ? "loading..." : "warming up the views...") : "on the way...");
    snprintf(l[2], sizeof(l[2]), "render %s    sky %s    shadows %s    GPU %d%%   CPU %d%%",
             g_vk.render_scale == 2 ? "2x" : g_vk.render_scale == 1 ? "1.5x" : g_vk.render_scale == 3 ? "0.5x" : g_vk.render_scale == 4 ? "0.75x" : "1x",
             g_vk.sky_mode >= 2 ? "Storm" : g_vk.sky_mode ? "Vibrant" : "game's own", g_vk.shadow_quality == 0 ? "off" : g_vk.shadow_quality == 1 ? "normal" : "sharp",
             g_vk.gpu_pct < 0 ? 0 : g_vk.gpu_pct, g_vk.cpu_pct);
    p[0] = l[0]; p[1] = l[1]; p[2] = l[2];
    *out = p;
    return 3;
}
int splat_bench_lines(const char*** out) { if (!bench_on() || g_phase != PH_DONE || !g_nlines) return 0; static const char* p[40]; for (int i = 0; i < g_nlines; i++) p[i] = g_lines[i]; *out = p; return g_nlines; }
